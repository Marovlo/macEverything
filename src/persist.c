#define _DARWIN_C_SOURCE 1
#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define ME_VERSION 2

typedef struct MEDiskHeader {
  char magic[8];
  uint32_t version;
  uint32_t header_bytes;
  uint32_t node_count;
  uint32_t root_count;
  uint32_t blob_size;
  uint32_t reserved;
  uint64_t keys_offset;
  uint64_t nodes_offset;
  uint64_t blob_offset;
  uint64_t file_size;
} MEDiskHeader;

typedef struct MEDiskRoot {
  uint64_t event_id;
  uint32_t node;
  uint32_t path_len;
  uint8_t uuid[16];
  char path[1024];
} MEDiskRoot;

_Static_assert(sizeof(MEKey) == 8, "MEKey");
_Static_assert(sizeof(MENode) == 48, "MENode");
_Static_assert(sizeof(MEDiskHeader) <= 256, "header");

static uint64_t align8(uint64_t v) { return (v + 7ull) & ~7ull; }

static int sidecar_path(const char *path, char *out, size_t cap) {
  int n = snprintf(out, cap, "%s.eid", path);
  return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

static int ensure_parent(const char *path) {
  char dir[1024];
  size_t plen = strlen(path);
  if (plen == 0 || plen >= sizeof dir) return -1;
  memcpy(dir, path, plen + 1);
  char *slash = strrchr(dir, '/');
  if (!slash || slash == dir) return 0;
  *slash = 0;
  if (me_mkdir_p(dir) != 0) {
    me_set_error("mkdir failed");
    return -1;
  }
  return 0;
}

static int write_sidecar(const char *path, const MEIndex *idx) {
  char sp[1200];
  if (sidecar_path(path, sp, sizeof sp) != 0) return -1;
  int fd = open(sp, O_CREAT | O_TRUNC | O_WRONLY, 0644);
  if (fd < 0) return -1;
  char magic[4] = {'E', 'I', 'D', '1'};
  uint32_t count = idx->root_count;
  if (me_write_full(fd, magic, 4) != 0 || me_write_full(fd, &count, 4) != 0) {
    close(fd);
    return -1;
  }
  for (uint32_t i = 0; i < count; i++) {
    uint64_t id = idx->roots[i].event_id;
    if (me_write_full(fd, &id, 8) != 0) {
      close(fd);
      return -1;
    }
  }
  if (fsync(fd) != 0) {
    close(fd);
    return -1;
  }
  close(fd);
  return 0;
}

static void read_sidecar(const char *path, MEIndex *idx) {
  char sp[1200];
  if (sidecar_path(path, sp, sizeof sp) != 0) return;
  int fd = open(sp, O_RDONLY);
  if (fd < 0) return;
  char magic[4];
  uint32_t count = 0;
  if (read(fd, magic, 4) != 4 || memcmp(magic, "EID1", 4) != 0 || read(fd, &count, 4) != 4) {
    close(fd);
    return;
  }
  if (count != idx->root_count || count > ME_MAX_ROOTS) {
    close(fd);
    return;
  }
  for (uint32_t i = 0; i < count; i++) {
    uint64_t id = 0;
    if (read(fd, &id, 8) != 8) {
      close(fd);
      return;
    }
    idx->roots[i].event_id = id;
  }
  close(fd);
}

int me_index_save(MEIndex *idx, const char *path) {
  if (!idx || !path) return -1;
  if (ensure_parent(path) != 0) return -1;
  if (idx->dirty && !idx->writable) {
    if (me_thaw(idx) != 0) return -1;
  }
  pthread_rwlock_wrlock(&idx->lock);
  if (idx->writable && idx->dirty && idx->live != idx->count) {
    if (me_compact_ul(idx) != 0) {
      pthread_rwlock_unlock(&idx->lock);
      me_set_error("compact failed");
      return -1;
    }
  }
  if (idx->eid_dirty) {
    if (write_sidecar(path, idx) != 0) {
      pthread_rwlock_unlock(&idx->lock);
      me_set_error("event id save failed");
      return -1;
    }
    idx->eid_dirty = 0;
  }
  int do_full = idx->dirty && idx->writable;
  pthread_rwlock_unlock(&idx->lock);
  if (!do_full) return 0;

  char tmp[1200];
  if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return -1;
  int fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY, 0644);
  if (fd < 0) {
    me_set_error("open %s: %s", tmp, strerror(errno));
    return -1;
  }

  pthread_rwlock_rdlock(&idx->lock);
  MEDiskHeader hdr;
  memset(&hdr, 0, sizeof hdr);
  memcpy(hdr.magic, "MEIDX002", 8);
  hdr.version = ME_VERSION;
  hdr.header_bytes = 256;
  hdr.node_count = idx->count;
  hdr.root_count = idx->root_count;
  hdr.blob_size = idx->blob_len;
  hdr.reserved = idx->complete ? 1u : 0u;
  uint64_t off = 256;
  MEDiskRoot roots[ME_MAX_ROOTS];
  memset(roots, 0, sizeof roots);
  for (uint32_t i = 0; i < idx->root_count; i++) {
    roots[i].event_id = idx->roots[i].event_id;
    roots[i].node = idx->roots[i].node;
    roots[i].path_len = idx->roots[i].path_len;
    memcpy(roots[i].uuid, idx->roots[i].uuid, 16);
    memcpy(roots[i].path, idx->roots[i].path, sizeof roots[i].path);
  }
  off += (uint64_t)idx->root_count * sizeof(MEDiskRoot);
  off = align8(off);
  hdr.keys_offset = off;
  off += (uint64_t)idx->count * sizeof(MEKey);
  off = align8(off);
  hdr.nodes_offset = off;
  off += (uint64_t)idx->count * sizeof(MENode);
  off = align8(off);
  hdr.blob_offset = off;
  off += idx->blob_len;
  hdr.file_size = off;

  char zeros[256];
  memset(zeros, 0, sizeof zeros);
  int rc = 0;
  if (me_write_full(fd, zeros, 256) != 0) rc = -1;
  if (rc == 0 && idx->root_count &&
      me_write_full(fd, roots, (size_t)idx->root_count * sizeof(MEDiskRoot)) != 0) {
    rc = -1;
  }
  if (rc == 0 && lseek(fd, hdr.keys_offset, SEEK_SET) < 0) rc = -1;
  if (rc == 0 && idx->count && me_write_full(fd, idx->keys, (size_t)idx->count * sizeof(MEKey)) != 0)
    rc = -1;
  if (rc == 0 && lseek(fd, hdr.nodes_offset, SEEK_SET) < 0) rc = -1;
  if (rc == 0 && idx->count && me_write_full(fd, idx->nodes, (size_t)idx->count * sizeof(MENode)) != 0)
    rc = -1;
  if (rc == 0 && lseek(fd, hdr.blob_offset, SEEK_SET) < 0) rc = -1;
  if (rc == 0 && idx->blob_len && me_write_full(fd, idx->blob, idx->blob_len) != 0) rc = -1;
  if (rc == 0) {
    if (lseek(fd, 0, SEEK_SET) < 0 || me_write_full(fd, &hdr, sizeof hdr) != 0) rc = -1;
  }
  if (rc == 0 && fsync(fd) != 0) rc = -1;
  pthread_rwlock_unlock(&idx->lock);
  close(fd);
  if (rc != 0) {
    unlink(tmp);
    me_set_error("write index failed");
    return -1;
  }
  if (rename(tmp, path) != 0) {
    me_set_error("rename index failed");
    return -1;
  }
  /* rename 成功后才清 dirty；用写锁避免与并发读者/写者竞争。 */
  pthread_rwlock_wrlock(&idx->lock);
  idx->dirty = 0;
  write_sidecar(path, idx);
  idx->eid_dirty = 0;
  pthread_rwlock_unlock(&idx->lock);
  return 0;
}

MEIndex *me_index_load(const char *path) {
  if (!path) return NULL;
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    me_set_error("open %s: %s", path, strerror(errno));
    return NULL;
  }
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size < 256) {
    close(fd);
    me_set_error("index file is too small");
    return NULL;
  }
  void *map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (map == MAP_FAILED) {
    close(fd);
    me_set_error("mmap failed");
    return NULL;
  }
  MEDiskHeader hdr;
  memcpy(&hdr, map, sizeof hdr);
  if (memcmp(hdr.magic, "MEIDX002", 8) != 0 || hdr.version != ME_VERSION || hdr.header_bytes < sizeof hdr ||
      hdr.root_count > ME_MAX_ROOTS || hdr.node_count > 50000000u) {
    munmap(map, (size_t)st.st_size);
    close(fd);
    me_set_error("bad index header");
    return NULL;
  }
  uint64_t size = (uint64_t)st.st_size;
  uint64_t keys_end = hdr.keys_offset + (uint64_t)hdr.node_count * sizeof(MEKey);
  uint64_t nodes_end = hdr.nodes_offset + (uint64_t)hdr.node_count * sizeof(MENode);
  uint64_t blob_end = hdr.blob_offset + hdr.blob_size;
  if (hdr.keys_offset > size || hdr.nodes_offset > size || hdr.blob_offset > size || keys_end > size ||
      nodes_end > size || blob_end > size || (hdr.keys_offset & 7) || (hdr.nodes_offset & 7)) {
    munmap(map, (size_t)st.st_size);
    close(fd);
    me_set_error("index offsets are corrupt");
    return NULL;
  }
  uint64_t roots_end = 256 + (uint64_t)hdr.root_count * sizeof(MEDiskRoot);
  if (roots_end > hdr.keys_offset || roots_end > size) {
    munmap(map, (size_t)st.st_size);
    close(fd);
    me_set_error("index roots are corrupt");
    return NULL;
  }

  MEIndex *idx = calloc(1, sizeof *idx);
  if (!idx) {
    munmap(map, (size_t)st.st_size);
    close(fd);
    return NULL;
  }
  idx->fd = fd;
  idx->map = map;
  idx->map_len = (size_t)st.st_size;
  idx->writable = 0;
  if (pthread_rwlock_init(&idx->lock, NULL) != 0) {
    munmap(map, (size_t)st.st_size);
    close(fd);
    free(idx);
    return NULL;
  }
  idx->count = hdr.node_count;
  idx->cap = hdr.node_count;
  idx->blob_len = hdr.blob_size;
  idx->blob_cap = 0;
  idx->keys = hdr.node_count ? (MEKey *)((char *)map + hdr.keys_offset) : NULL;
  idx->nodes = hdr.node_count ? (MENode *)((char *)map + hdr.nodes_offset) : NULL;
  idx->blob = hdr.blob_size ? (char *)map + hdr.blob_offset : NULL;
  idx->root_count = hdr.root_count;
  uint32_t live = 0;
  for (uint32_t i = 0; i < idx->count; i++) {
    if (!(idx->keys[i].flags & ME_F_DEAD)) live++;
  }
  idx->live = live;
  atomic_store_explicit(&idx->complete, hdr.reserved == 1 ? 1 : 0, memory_order_relaxed);
  for (uint32_t i = 0; i < hdr.root_count; i++) {
    MEDiskRoot disk;
    memcpy(&disk, (char *)map + 256 + (size_t)i * sizeof(MEDiskRoot), sizeof disk);
    if (disk.path_len >= 1024 || disk.node >= hdr.node_count) {
      me_index_free(idx);
      me_set_error("bad root record");
      return NULL;
    }
    MERoot *r = &idx->roots[i];
    memset(r, 0, sizeof *r);
    memcpy(r->path, disk.path, disk.path_len);
    r->path[disk.path_len] = 0;
    r->path_len = disk.path_len;
    r->node = disk.node;
    r->event_id = disk.event_id;
    memcpy(r->uuid, disk.uuid, 16);
    size_t fl = 0;
    if (me_fold_path(r->path, r->fold, sizeof r->fold, &fl) != 0) {
      me_index_free(idx);
      me_set_error("could not fold stored root");
      return NULL;
    }
    struct stat rst;
    if (stat(r->path, &rst) == 0) r->device = (uint64_t)rst.st_dev;
  }
  read_sidecar(path, idx);
  atomic_store(&idx->generation, 1);
  if (idx->keys && idx->count) {
    posix_madvise(idx->keys, (size_t)idx->count * sizeof(MEKey), POSIX_MADV_WILLNEED);
    posix_madvise(idx->keys, (size_t)idx->count * sizeof(MEKey), POSIX_MADV_SEQUENTIAL);
  }
  if (idx->blob && idx->blob_len) {
    posix_madvise(idx->blob, idx->blob_len, POSIX_MADV_WILLNEED);
    posix_madvise(idx->blob, idx->blob_len, POSIX_MADV_SEQUENTIAL);
  }
  return idx;
}
