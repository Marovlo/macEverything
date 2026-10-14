#ifndef ME_INTERNAL_H
#define ME_INTERNAL_H

#include "me.h"

#include <pthread.h>
#include <stdatomic.h>

#define ME_MAX_ROOTS 16

typedef struct MEKey {
  uint32_t off;
  uint16_t len;
  uint16_t flags;
} MEKey;

typedef struct MENode {
  uint64_t file_id;
  uint64_t size;
  int64_t mtime;
  uint32_t parent;
  uint32_t first_child;
  uint32_t next_sibling;
  uint32_t prev_sibling;
  uint32_t name_off;
  uint16_t name_len;
  uint16_t reserved;
} MENode;

typedef struct MERoot {
  char path[1024];
  char fold[1024];
  uint32_t node;
  uint32_t path_len;
  uint64_t event_id;
  uint8_t uuid[16];
  uint64_t device;
} MERoot;

typedef struct Slot {
  uint64_t hash;
  uint32_t node;
  uint32_t state;
} Slot;

struct MEIndex {
  MEKey *keys;
  MENode *nodes;
  uint32_t count;
  uint32_t cap;
  uint32_t live;
  char *blob;
  uint32_t blob_len;
  uint32_t blob_cap;
  MERoot roots[ME_MAX_ROOTS];
  uint32_t root_count;
  Slot *slots;
  uint32_t slot_cap;
  uint32_t slot_live;
  uint32_t slot_used;
  int writable;
  int dirty;
  int eid_dirty;
  atomic_int complete;
  int fd;
  void *map;
  size_t map_len;
  pthread_rwlock_t lock;
  atomic_uint_fast64_t generation;
  atomic_uint_fast64_t progress;
  atomic_int scanning;
  void *streams[ME_MAX_ROOTS];
  int stream_count;
  void *watch_q;
  void (*on_change)(void *);
  void *on_change_ctx;
  int watching;
};

int me_fold(const char *src, size_t src_len, char *dst, size_t dst_cap,
            const char **out, size_t *out_len);
int me_fold_path(const char *src, char *dst, size_t cap, size_t *out_len);
int me_contains(const char *hay, size_t hlen, const char *needle, size_t nlen);

void me_set_error(const char *fmt, ...);
double me_now(void);
int me_write_full(int fd, const void *buf, size_t n);
int me_mkdir_p(const char *path);

uint32_t me_add_root_ul(MEIndex *idx, const char *absolute_path, uint64_t event_id,
                        const uint8_t uuid[16], uint64_t device);
uint32_t me_add_child_ul(MEIndex *idx, uint32_t parent, const char *name,
                         size_t name_len, uint64_t file_id, uint64_t size,
                         int64_t mtime, uint32_t flags);
uint32_t me_lookup_ul(MEIndex *idx, const char *path);
int me_build_path_ul(const MEIndex *idx, uint32_t node, char *buf, size_t cap);
int me_thaw(MEIndex *idx);
void me_rebuild_hash_ul(MEIndex *idx);
int me_compact_ul(MEIndex *idx);
void me_bump(MEIndex *idx);
void me_note_event(MEIndex *idx, const char *path, uint64_t evid);
int me_should_skip(const char *parent_base, size_t parent_len, const char *name,
                   size_t name_len);
int me_path_ignored(const MEIndex *idx, const char *path);

typedef struct MEDirEnt {
  uint32_t name_off;
  uint32_t name_len;
  uint32_t flags;
  uint64_t file_id;
  uint64_t size;
  int64_t mtime;
} MEDirEnt;

typedef struct MEDirList {
  MEDirEnt *items;
  uint32_t count;
  uint32_t cap;
  char *blob;
  uint32_t blob_len;
  uint32_t blob_cap;
} MEDirList;

void me_dirlist_free(MEDirList *d);
int me_dirlist_read(int fd, MEDirList *d);

#endif
