#define _DARWIN_C_SOURCE 1
#include "internal.h"

#include <CoreServices/CoreServices.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdlib.h>
#include <string.h>
#include <sys/attr.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/vnode.h>
#include <unistd.h>

void me_dirlist_free(MEDirList *d) {
  if (!d) return;
  free(d->items);
  free(d->blob);
  memset(d, 0, sizeof *d);
}

static int dirlist_push(MEDirList *d, const char *name, uint32_t name_len, uint32_t flags,
                        uint64_t file_id, uint64_t size, int64_t mtime) {
  if (d->count == d->cap) {
    uint32_t cap = d->cap ? d->cap * 2 : 64;
    MEDirEnt *items = realloc(d->items, (size_t)cap * sizeof(MEDirEnt));
    if (!items) return -1;
    d->items = items;
    d->cap = cap;
  }
  uint32_t need = d->blob_len + name_len + 1;
  if (need > d->blob_cap) {
    uint32_t cap = d->blob_cap ? d->blob_cap : 4096;
    while (cap < need) cap *= 2;
    char *blob = realloc(d->blob, cap);
    if (!blob) return -1;
    d->blob = blob;
    d->blob_cap = cap;
  }
  MEDirEnt *e = &d->items[d->count++];
  e->name_off = d->blob_len;
  e->name_len = name_len;
  e->flags = flags;
  e->file_id = file_id;
  e->size = size;
  e->mtime = mtime;
  memcpy(d->blob + e->name_off, name, name_len);
  d->blob[e->name_off + name_len] = 0;
  d->blob_len = need;
  return 0;
}

int me_dirlist_read(int fd, MEDirList *d) {
  memset(d, 0, sizeof *d);
  struct attrlist al;
  memset(&al, 0, sizeof al);
  al.bitmapcount = ATTR_BIT_MAP_COUNT;
  al.commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME | ATTR_CMN_OBJTYPE | ATTR_CMN_MODTIME |
                  ATTR_CMN_FILEID;
  al.fileattr = ATTR_FILE_DATALENGTH;
  const size_t buf_size = 256 * 1024;
  char *buf = malloc(buf_size);
  if (!buf) return -1;
  for (;;) {
    int n = getattrlistbulk(fd, &al, buf, buf_size, 0);
    if (n == 0) break;
    if (n < 0) {
      if (errno == EINTR) continue;
      free(buf);
      me_dirlist_free(d);
      return -1;
    }
    char *p = buf;
    char *end = buf + buf_size;
    for (int i = 0; i < n; i++) {
      if (p + 4 > end) break;
      uint32_t len = 0;
      memcpy(&len, p, 4);
      if (len < 4 || p + len > end) break;
      uint32_t off = 4;
      attribute_set_t ret;
      memset(&ret, 0, sizeof ret);
      if (off + sizeof ret > len) {
        p += len;
        continue;
      }
      memcpy(&ret, p + off, sizeof ret);
      off += (uint32_t)sizeof ret;

      if (!(ret.commonattr & ATTR_CMN_NAME) || off + sizeof(attrreference_t) > len) {
        p += len;
        continue;
      }
      uint32_t name_at = off;
      attrreference_t name;
      memcpy(&name, p + off, sizeof name);
      off += (uint32_t)sizeof name;
      if (name.attr_dataoffset < 0) {
        p += len;
        continue;
      }
      const char *ns = p + name_at + name.attr_dataoffset;
      if (ns < p || ns >= p + len || name.attr_length == 0 || ns + name.attr_length > p + len) {
        p += len;
        continue;
      }
      uint32_t nlen = name.attr_length;
      if (nlen > 0 && ns[nlen - 1] == 0) nlen--;
      if (nlen == 0 || (nlen == 1 && ns[0] == '.') || (nlen == 2 && ns[0] == '.' && ns[1] == '.')) {
        p += len;
        continue;
      }

      uint32_t objtype = 0;
      if (ret.commonattr & ATTR_CMN_OBJTYPE) {
        if (off + 4 > len) {
          p += len;
          continue;
        }
        memcpy(&objtype, p + off, 4);
        off += 4;
      }
      struct timespec ts;
      memset(&ts, 0, sizeof ts);
      if (ret.commonattr & ATTR_CMN_MODTIME) {
        if (off + sizeof ts > len) {
          p += len;
          continue;
        }
        memcpy(&ts, p + off, sizeof ts);
        off += (uint32_t)sizeof ts;
      }
      uint64_t fileid = 0;
      if (ret.commonattr & ATTR_CMN_FILEID) {
        if (off + 8 > len) {
          p += len;
          continue;
        }
        memcpy(&fileid, p + off, 8);
        off += 8;
      }
      uint64_t datalen = 0;
      if (ret.fileattr & ATTR_FILE_DATALENGTH) {
        if (off + 8 <= len) memcpy(&datalen, p + off, 8);
      }
      uint32_t flags = 0;
      if (objtype == VLNK) flags |= ME_F_LINK;
      else if (objtype == VDIR) flags |= ME_F_DIR;
      uint64_t size = (flags & ME_F_DIR) ? 0 : datalen;
      if (dirlist_push(d, ns, nlen, flags, fileid, size, (int64_t)ts.tv_sec) != 0) {
        free(buf);
        me_dirlist_free(d);
        return -1;
      }
      p += len;
    }
  }
  free(buf);
  return 0;
}

typedef struct Job {
  int fd;
  uint32_t node;
  int counted;
} Job;

typedef struct Pool {
  MEIndex *idx;
  Job *jobs;
  uint32_t cap;
  uint32_t head;
  uint32_t tail;
  int waiting;
  int nthreads;
  int stop;
  int open_fds;
  int root_fd;
  int skip_tcc;
  uint64_t device;
  pthread_mutex_t mu;
  pthread_cond_t cv;
  atomic_uint_fast64_t files;
  atomic_uint_fast64_t dirs;
} Pool;

static int enqueue(Pool *pool, int fd, uint32_t node, int counted) {
  uint32_t next = (pool->tail + 1) % pool->cap;
  if (next == pool->head) {
    uint32_t ncap = pool->cap ? pool->cap * 2 : 1024;
    Job *jobs = calloc(ncap, sizeof(Job));
    if (!jobs) return -1;
    uint32_t n = 0;
    while (pool->head != pool->tail) {
      jobs[n++] = pool->jobs[pool->head];
      pool->head = (pool->head + 1) % pool->cap;
    }
    free(pool->jobs);
    pool->jobs = jobs;
    pool->cap = ncap;
    pool->head = 0;
    pool->tail = n;
  }
  pool->jobs[pool->tail].fd = fd;
  pool->jobs[pool->tail].node = node;
  pool->jobs[pool->tail].counted = counted;
  pool->tail = (pool->tail + 1) % pool->cap;
  if (counted && fd >= 0) pool->open_fds++;
  pthread_cond_signal(&pool->cv);
  return 0;
}

static int open_node(Pool *pool, uint32_t node) {
  uint32_t stack[512];
  uint16_t lens[512];
  char *names = NULL;
  int sp = 0;
  size_t bytes = 0;
  pthread_rwlock_rdlock(&pool->idx->lock);
  uint32_t cur = node;
  while (cur != ME_NONE && sp < 512) {
    int is_root = 0;
    for (uint32_t i = 0; i < pool->idx->root_count; i++) {
      if (pool->idx->roots[i].node == cur) {
        is_root = 1;
        break;
      }
    }
    if (is_root) break;
    stack[sp] = cur;
    lens[sp] = pool->idx->nodes[cur].name_len;
    bytes += lens[sp] + 1;
    sp++;
    cur = pool->idx->nodes[cur].parent;
  }
  names = malloc(bytes ? bytes : 1);
  if (!names) {
    pthread_rwlock_unlock(&pool->idx->lock);
    return -1;
  }
  size_t off = 0;
  for (int i = 0; i < sp; i++) {
    memcpy(names + off, pool->idx->blob + pool->idx->nodes[stack[i]].name_off, lens[i]);
    names[off + lens[i]] = 0;
    off += (size_t)lens[i] + 1;
  }
  pthread_rwlock_unlock(&pool->idx->lock);

  int fd = dup(pool->root_fd);
  if (fd < 0) {
    free(names);
    return -1;
  }
  /* stack[sp-1] is the child of root; stack[0] is the node itself. */
  off = 0;
  for (int i = 0; i < sp; i++) off += (size_t)lens[i] + 1;
  for (int i = sp - 1; i >= 0; i--) {
    off -= (size_t)lens[i] + 1;
    int next = openat(fd, names + off, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(fd);
    if (next < 0) {
      free(names);
      return -1;
    }
    fd = next;
  }
  free(names);
  return fd;
}

static int name_eq(const char *s, uint32_t n, const char *lit) {
  size_t m = strlen(lit);
  return n == m && memcmp(s, lit, m) == 0;
}

static int tcc_skip_dir(int parent_is_root, const char *parent_base, uint16_t parent_len, const char *nm,
                        uint32_t nl) {
  if (parent_is_root) {
    return name_eq(nm, nl, "Desktop") || name_eq(nm, nl, "Documents") || name_eq(nm, nl, "Downloads") ||
           name_eq(nm, nl, "Movies") || name_eq(nm, nl, "Music") || name_eq(nm, nl, "Pictures");
  }
  if (name_eq(parent_base, parent_len, "Library")) {
    return name_eq(nm, nl, "Mail") || name_eq(nm, nl, "Messages") || name_eq(nm, nl, "Safari") ||
           name_eq(nm, nl, "Accounts") || name_eq(nm, nl, "Cookies") || name_eq(nm, nl, "IdentityServices") ||
           name_eq(nm, nl, "HomeKit") || name_eq(nm, nl, "Suggestions");
  }
  return nl >= 14 && memcmp(nm + (nl - 14), ".photoslibrary", 14) == 0;
}

typedef struct TCCProbe {
  int dirfd;
  _Atomic int state;
  int fd;
  int err;
} TCCProbe;

static void *tcc_probe_main(void *arg) {
  TCCProbe *p = arg;
  int fd = openat(p->dirfd, "Desktop", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  p->err = errno;
  p->fd = fd;
  atomic_store_explicit(&p->state, 1, memory_order_release);
  return NULL;
}

/* Full Disk Access is not applied to an already-running process, and openat on
   Desktop/Documents then blocks instead of returning EPERM. */
static int probe_skips_tcc(int root_fd) {
  int dfd = dup(root_fd);
  if (dfd < 0) return 0;
  TCCProbe *p = calloc(1, sizeof *p);
  if (!p) {
    close(dfd);
    return 0;
  }
  p->dirfd = dfd;
  p->fd = -1;
  pthread_t th;
  if (pthread_create(&th, NULL, tcc_probe_main, p) != 0) {
    close(dfd);
    free(p);
    return 0;
  }
  for (int i = 0; i < 40 && !atomic_load_explicit(&p->state, memory_order_acquire); i++) usleep(10000);
  if (!atomic_load_explicit(&p->state, memory_order_acquire)) {
    pthread_detach(th);
    return 1;
  }
  pthread_join(th, NULL);
  int skip = p->fd < 0 && (p->err == EPERM || p->err == EACCES);
  if (p->fd >= 0) close(p->fd);
  close(dfd);
  free(p);
  return skip;
}

static void process_job(Pool *pool, Job job) {
  int private_fd = 0;
  if (job.fd < 0) {
    job.fd = open_node(pool, job.node);
    if (job.fd < 0) return;
    private_fd = 1;
  }
  struct stat st;
  if (fstat(job.fd, &st) != 0 || (pool->device && (uint64_t)st.st_dev != pool->device)) {
    close(job.fd);
    goto done_count;
  }
  MEDirList list;
  if (me_dirlist_read(job.fd, &list) != 0) {
    close(job.fd);
    goto done_count;
  }

  typedef struct Sub {
    uint32_t node;
    uint32_t name_off;
    uint32_t name_len;
  } Sub;
  Sub *subs = NULL;
  uint32_t nsub = 0, subcap = 0;
  char *namecopy = NULL;
  uint32_t name_len = 0, name_cap = 0;

  pthread_rwlock_wrlock(&pool->idx->lock);
  char parent_base[768];
  uint16_t parent_len = pool->idx->nodes[job.node].name_len;
  if (parent_len >= sizeof parent_base) parent_len = sizeof parent_base - 1;
  memcpy(parent_base, pool->idx->blob + pool->idx->nodes[job.node].name_off, parent_len);
  parent_base[parent_len] = 0;
  for (uint32_t i = 0; i < list.count; i++) {
    const char *nm = list.blob + list.items[i].name_off;
    uint32_t nl = list.items[i].name_len;
    if (me_should_skip(parent_base, parent_len, nm, nl)) continue;
    uint32_t flags = list.items[i].flags;
    uint32_t id = me_add_child_ul(pool->idx, job.node, nm, nl, list.items[i].file_id,
                                  list.items[i].size, list.items[i].mtime, flags);
    if (id == ME_NONE) continue;
    if (flags & ME_F_DIR) atomic_fetch_add(&pool->dirs, 1);
    else atomic_fetch_add(&pool->files, 1);
    int parent_is_root = pool->idx->nodes[job.node].parent == ME_NONE;
    int blocked = pool->skip_tcc && tcc_skip_dir(parent_is_root, parent_base, parent_len, nm, nl);
    if ((flags & ME_F_DIR) && !(flags & ME_F_LINK) && !blocked) {
      if (nsub == subcap) {
        uint32_t cap = subcap ? subcap * 2 : 32;
        Sub *ns = realloc(subs, (size_t)cap * sizeof(Sub));
        if (!ns) continue;
        subs = ns;
        subcap = cap;
      }
      if (name_len + nl + 1 > name_cap) {
        uint32_t cap = name_cap ? name_cap : 256;
        while (cap < name_len + nl + 1) cap *= 2;
        char *nb = realloc(namecopy, cap);
        if (!nb) continue;
        namecopy = nb;
        name_cap = cap;
      }
      memcpy(namecopy + name_len, nm, nl);
      namecopy[name_len + nl] = 0;
      subs[nsub].node = id;
      subs[nsub].name_off = name_len;
      subs[nsub].name_len = nl;
      name_len += nl + 1;
      nsub++;
    }
  }
  pthread_rwlock_unlock(&pool->idx->lock);

  for (uint32_t i = 0; i < nsub; i++) {
    const char *nm = namecopy + subs[i].name_off;
    int cfd = -1;
    int counted = 0;
    pthread_mutex_lock(&pool->mu);
    int room = pool->open_fds < 256;
    pthread_mutex_unlock(&pool->mu);
    if (room) {
      cfd = openat(job.fd, nm, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (cfd >= 0) {
        struct stat cst;
        if (fstat(cfd, &cst) != 0 || (pool->device && (uint64_t)cst.st_dev != pool->device)) {
          close(cfd);
          continue;
        }
        counted = 1;
      }
    }
    pthread_mutex_lock(&pool->mu);
    if (enqueue(pool, cfd, subs[i].node, counted) != 0 && cfd >= 0) close(cfd);
    pthread_mutex_unlock(&pool->mu);
  }

  close(job.fd);
  me_dirlist_free(&list);
  free(subs);
  free(namecopy);

done_count:
  if (job.counted && !private_fd) {
    pthread_mutex_lock(&pool->mu);
    if (pool->open_fds > 0) pool->open_fds--;
    pthread_mutex_unlock(&pool->mu);
  }
}

static void *worker_main(void *arg) {
  Pool *pool = arg;
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
  for (;;) {
    pthread_mutex_lock(&pool->mu);
    while (pool->head == pool->tail && !pool->stop) {
      pool->waiting++;
      if (pool->waiting == pool->nthreads) {
        pool->stop = 1;
        pthread_cond_broadcast(&pool->cv);
        pthread_mutex_unlock(&pool->mu);
        return NULL;
      }
      pthread_cond_wait(&pool->cv, &pool->mu);
      pool->waiting--;
      if (pool->stop && pool->head == pool->tail) {
        pthread_mutex_unlock(&pool->mu);
        return NULL;
      }
    }
    if (pool->head == pool->tail) {
      pthread_mutex_unlock(&pool->mu);
      return NULL;
    }
    Job job = pool->jobs[pool->head];
    pool->head = (pool->head + 1) % pool->cap;
    pthread_mutex_unlock(&pool->mu);
    process_job(pool, job);
  }
}

static int volume_uuid(const char *path, uint8_t out[16]) {
  struct attrlist al;
  memset(&al, 0, sizeof al);
  al.bitmapcount = ATTR_BIT_MAP_COUNT;
  al.volattr = ATTR_VOL_UUID;
  struct {
    uint32_t len;
    uint8_t uuid[16];
  } buf;
  memset(&buf, 0, sizeof buf);
  if (getattrlist(path, &al, &buf, sizeof buf, 0) != 0) return -1;
  memcpy(out, buf.uuid, 16);
  return 0;
}

int me_index_scan(MEIndex *idx, const char *root_path, MEScanStats *stats) {
  if (!idx || !root_path) return -1;
  double t0 = me_now();
  char resolved[PATH_MAX];
  if (!realpath(root_path, resolved)) {
    me_set_error("realpath %s: %s", root_path, strerror(errno));
    return -1;
  }
  int root_fd = open(resolved, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (root_fd < 0) {
    me_set_error("open %s: %s", resolved, strerror(errno));
    return -1;
  }
  struct stat st;
  if (fstat(root_fd, &st) != 0) {
    close(root_fd);
    me_set_error("fstat failed");
    return -1;
  }
  struct rlimit rl;
  if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < 4096) {
    rlim_t want = rl.rlim_max > 8192 ? 8192 : rl.rlim_max;
    if (want > rl.rlim_cur) {
      rl.rlim_cur = want;
      setrlimit(RLIMIT_NOFILE, &rl);
    }
  }
  uint8_t uuid[16];
  memset(uuid, 0, sizeof uuid);
  volume_uuid(resolved, uuid);
  uint64_t evid = (uint64_t)FSEventsGetCurrentEventId();

  if (me_thaw(idx) != 0) {
    close(root_fd);
    return -1;
  }
  pthread_rwlock_wrlock(&idx->lock);
  uint32_t root_node = me_add_root_ul(idx, resolved, evid, uuid, (uint64_t)st.st_dev);
  pthread_rwlock_unlock(&idx->lock);
  if (root_node == ME_NONE) {
    close(root_fd);
    return -1;
  }

  Pool pool;
  memset(&pool, 0, sizeof pool);
  pool.idx = idx;
  pool.root_fd = root_fd;
  pool.device = (uint64_t)st.st_dev;
  pool.skip_tcc = probe_skips_tcc(root_fd);
  pool.cap = 1024;
  pool.jobs = calloc(pool.cap, sizeof(Job));
  pthread_mutex_init(&pool.mu, NULL);
  pthread_cond_init(&pool.cv, NULL);
  atomic_store(&pool.dirs, 1);
  int nthreads = (int)sysconf(_SC_NPROCESSORS_ONLN);
  if (nthreads < 1) nthreads = 1;
  if (nthreads > 16) nthreads = 16;
  pool.nthreads = nthreads;

  int scan_fd = dup(root_fd);
  pthread_mutex_lock(&pool.mu);
  enqueue(&pool, scan_fd, root_node, scan_fd >= 0);
  atomic_store_explicit(&idx->scanning, 1, memory_order_release);
  pthread_t *threads = calloc((size_t)(nthreads > 1 ? nthreads - 1 : 0), sizeof(pthread_t));
  int started = 0;
  for (int i = 0; i < nthreads - 1; i++) {
    if (pthread_create(&threads[i], NULL, worker_main, &pool) == 0) started++;
  }
  pool.nthreads = started + 1;
  pthread_mutex_unlock(&pool.mu);
  worker_main(&pool);
  for (int i = 0; i < started; i++) pthread_join(threads[i], NULL);
  free(threads);

  pthread_rwlock_wrlock(&idx->lock);
  me_bump(idx);
  pthread_rwlock_unlock(&idx->lock);
  atomic_store_explicit(&idx->scanning, 0, memory_order_release);

  close(root_fd);
  free(pool.jobs);
  pthread_mutex_destroy(&pool.mu);
  pthread_cond_destroy(&pool.cv);

  if (stats) {
    stats->dirs = atomic_load(&pool.dirs);
    stats->files = atomic_load(&pool.files);
    stats->nodes = stats->dirs + stats->files;
    stats->seconds = me_now() - t0;
    stats->skipped_protected = pool.skip_tcc;
  }
  return 0;
}

static uint64_t name_hash(const char *s, size_t n) {
  uint64_t h = 14695981039346656037ull;
  for (size_t i = 0; i < n; i++) {
    h ^= (unsigned char)s[i];
    h *= 1099511628211ull;
  }
  return h ? h : 1;
}

static int name_in_set(uint64_t *slots, uint32_t cap, uint64_t h) {
  uint32_t mask = cap - 1;
  uint32_t i = (uint32_t)h & mask;
  for (uint32_t s = 0; s < cap; s++) {
    if (slots[i] == 0) return 0;
    if (slots[i] == h) return 1;
    i = (i + 1) & mask;
  }
  return 0;
}

static int rescan_dir(MEIndex *idx, const char *path);

static int reconcile_dir(MEIndex *idx, const char *path) {
  int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ENOENT) return me_index_forget_path(idx, path);
    return me_index_refresh_path(idx, path);
  }
  struct stat st;
  if (fstat(fd, &st) != 0) {
    close(fd);
    return -1;
  }
  MEDirList list;
  int rc = me_dirlist_read(fd, &list);
  close(fd);
  if (rc != 0) return -1;

  if (me_thaw(idx) != 0) {
    me_dirlist_free(&list);
    return -1;
  }
  pthread_rwlock_wrlock(&idx->lock);
  uint32_t node = me_lookup_ul(idx, path);
  pthread_rwlock_unlock(&idx->lock);
  if (node == ME_NONE) {
    if (me_index_refresh_path(idx, path) != 0) {
      me_dirlist_free(&list);
      return -1;
    }
    pthread_rwlock_wrlock(&idx->lock);
    node = me_lookup_ul(idx, path);
    pthread_rwlock_unlock(&idx->lock);
    if (node == ME_NONE) {
      me_dirlist_free(&list);
      return -1;
    }
  }

  uint32_t set_cap = 16;
  while (set_cap < list.count * 2 + 16) set_cap *= 2;
  uint64_t *set = calloc(set_cap, sizeof(uint64_t));
  char *folds = NULL;
  uint32_t folds_len = 0, folds_cap = 0;
  uint32_t *fold_off = calloc(list.count ? list.count : 1, sizeof(uint32_t));
  uint16_t *fold_len = calloc(list.count ? list.count : 1, sizeof(uint16_t));
  if (!set || !fold_off || !fold_len) {
    free(set);
    free(fold_off);
    free(fold_len);
    me_dirlist_free(&list);
    return -1;
  }
  for (uint32_t i = 0; i < list.count; i++) {
    char tmp[1024];
    const char *nm = list.blob + list.items[i].name_off;
    const char *folded = NULL;
    size_t fl = 0;
    if (list.items[i].name_len > sizeof tmp ||
        me_fold(nm, list.items[i].name_len, tmp, sizeof tmp, &folded, &fl) != 0) {
      continue;
    }
    if (folds_len + fl + 1 > folds_cap) {
      uint32_t cap = folds_cap ? folds_cap : 256;
      while (cap < folds_len + fl + 1) cap *= 2;
      char *nb = realloc(folds, cap);
      if (!nb) continue;
      folds = nb;
      folds_cap = cap;
    }
    memcpy(folds + folds_len, folded, fl);
    folds[folds_len + fl] = 0;
    fold_off[i] = folds_len;
    fold_len[i] = (uint16_t)fl;
    folds_len += (uint32_t)fl + 1;
    uint64_t h = name_hash(folds + fold_off[i], fl);
    uint32_t mask = set_cap - 1;
    uint32_t slot = (uint32_t)h & mask;
    while (set[slot] != 0 && set[slot] != h) slot = (slot + 1) & mask;
    set[slot] = h;
  }

  pthread_rwlock_wrlock(&idx->lock);
  char parent_base[768];
  uint16_t parent_len = idx->nodes[node].name_len;
  if (parent_len >= sizeof parent_base) parent_len = sizeof parent_base - 1;
  memcpy(parent_base, idx->blob + idx->nodes[node].name_off, parent_len);
  parent_base[parent_len] = 0;

  typedef struct Recurse {
    char *path;
  } Recurse;
  Recurse *recs = NULL;
  uint32_t nrec = 0, recap = 0;
  for (uint32_t i = 0; i < list.count; i++) {
    const char *nm = list.blob + list.items[i].name_off;
    uint32_t nl = list.items[i].name_len;
    if (me_should_skip(parent_base, parent_len, nm, nl)) continue;
    uint32_t flags = list.items[i].flags;
    uint32_t id = me_add_child_ul(idx, node, nm, nl, list.items[i].file_id, list.items[i].size,
                                  list.items[i].mtime, flags);
    if (id == ME_NONE) continue;
    if ((flags & ME_F_DIR) && !(flags & ME_F_LINK)) {
      size_t pl = strlen(path);
      char *cp = malloc(pl + 1 + nl + 1);
      if (!cp) continue;
      memcpy(cp, path, pl);
      cp[pl] = '/';
      memcpy(cp + pl + 1, nm, nl);
      cp[pl + 1 + nl] = 0;
      if (nrec == recap) {
        uint32_t cap = recap ? recap * 2 : 8;
        Recurse *nr = realloc(recs, cap * sizeof(Recurse));
        if (!nr) {
          free(cp);
          continue;
        }
        recs = nr;
        recap = cap;
      }
      recs[nrec++].path = cp;
    }
  }
  /* 磁盘上已消失的子项：在同一把写锁内构建路径（避免节点 id 被
     并发 compact 重映射后误删其他条目），解锁后按路径 forget。 */
  char **dead = NULL;
  uint32_t ndead = 0, deadcap = 0;
  for (uint32_t c = idx->nodes[node].first_child; c != ME_NONE; c = idx->nodes[c].next_sibling) {
    if (idx->keys[c].flags & ME_F_DEAD) continue;
    const MEKey *k = &idx->keys[c];
    uint64_t h = name_hash(idx->blob + k->off, k->len);
    if (name_in_set(set, set_cap, h)) continue;
    char child_path[4096];
    if (me_build_path_ul(idx, c, child_path, sizeof child_path) != 0) continue;
    if (ndead == deadcap) {
      uint32_t cap = deadcap ? deadcap * 2 : 8;
      char **nd = realloc(dead, cap * sizeof(char *));
      if (!nd) continue;
      dead = nd;
      deadcap = cap;
    }
    dead[ndead] = strdup(child_path);
    if (dead[ndead]) ndead++;
  }
  pthread_rwlock_unlock(&idx->lock);

  for (uint32_t i = 0; i < ndead; i++) {
    me_index_forget_path(idx, dead[i]);
    free(dead[i]);
  }
  free(dead);

  for (uint32_t i = 0; i < nrec; i++) {
    rescan_dir(idx, recs[i].path);
    free(recs[i].path);
  }
  free(recs);
  free(set);
  free(folds);
  free(fold_off);
  free(fold_len);
  me_dirlist_free(&list);
  me_bump(idx);
  return 0;
}

static int rescan_dir(MEIndex *idx, const char *path) { return reconcile_dir(idx, path); }

int me_index_rescan(MEIndex *idx, const char *path) {
  if (!idx || !path) return -1;
  pthread_rwlock_rdlock(&idx->lock);
  int ignored = me_path_ignored(idx, path);
  pthread_rwlock_unlock(&idx->lock);
  if (ignored) return 0;
  struct stat st;
  if (lstat(path, &st) != 0) return me_index_forget_path(idx, path);
  if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) return me_index_refresh_path(idx, path);
  return rescan_dir(idx, path);
}
