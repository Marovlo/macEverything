#ifndef ME_H
#define ME_H

#include <stddef.h>
#include <stdint.h>

/* Filename index. Search scans prefolded names; a saved index is mapped
 * and queried without copying. ME_NONE is an invalid node id. */

#define ME_NONE UINT32_MAX

#define ME_F_DIR 1u
#define ME_F_LINK 2u
#define ME_F_DEAD 4u

typedef struct MEIndex MEIndex;

typedef struct MEScanStats {
  uint64_t nodes;
  uint64_t dirs;
  uint64_t files;
  double seconds;
  int skipped_protected;
} MEScanStats;

typedef struct MEInfo {
  char name[768];
  char path[4096];
  uint64_t size;
  int64_t mtime;
  int is_dir;
  int is_link;
} MEInfo;

MEIndex *me_index_create(void);
void me_index_free(MEIndex *idx);

int me_index_scan(MEIndex *idx, const char *root_path, MEScanStats *stats);

int me_index_save(MEIndex *idx, const char *path);
MEIndex *me_index_load(const char *path);
void me_index_set_complete(MEIndex *idx, int complete);
int me_index_complete(const MEIndex *idx);

uint32_t me_index_count(const MEIndex *idx);
uint32_t me_index_live(const MEIndex *idx);
uint64_t me_index_generation(const MEIndex *idx);
int me_index_dirty(const MEIndex *idx);
int me_index_scanning(const MEIndex *idx);
int me_index_is_mapped(const MEIndex *idx);
uint64_t me_index_storage_bytes(const MEIndex *idx);
uint64_t me_index_progress(const MEIndex *idx);

uint32_t me_index_root_count(const MEIndex *idx);
int me_index_root(const MEIndex *idx, uint32_t i, char *buf, size_t cap);

uint32_t me_index_add_root(MEIndex *idx, const char *absolute_path);
uint32_t me_index_add_child(MEIndex *idx, uint32_t parent, const char *name,
                            uint64_t file_id, uint64_t size, int64_t mtime,
                            uint32_t flags);

int me_index_search(MEIndex *idx, const char *query, uint32_t limit,
                    uint32_t *out, uint32_t *out_count, uint32_t *out_total,
                    double *seconds);
/* Same order as me_index_search, starting at offset. out receives at most limit ids. */
int me_index_search_range(MEIndex *idx, const char *query, uint32_t offset, uint32_t limit,
                          uint32_t *out, uint32_t *out_count, uint32_t *out_total,
                          double *seconds);

/* Sort keys for me_index_search_sorted. */
enum {
  ME_SORT_NAME = 0,
  ME_SORT_PATH = 1,
  ME_SORT_SIZE = 2,
  ME_SORT_MTIME = 3,
};

/* All matches, sorted by key. Ascending unless ascending == 0.
 * Size sort keeps folders after files in both directions.
 * The caller frees *out_ids. */
int me_index_search_sorted(MEIndex *idx, const char *query, int key, int ascending,
                           uint32_t **out_ids, uint32_t *out_count, double *seconds);
/* Live node ids. sort_by_name orders by the folded filename. */
int me_index_list(MEIndex *idx, uint32_t *out, uint32_t cap, uint32_t *out_count,
                  int sort_by_name);

int me_index_info(MEIndex *idx, uint32_t node, MEInfo *out);
int me_index_infos(MEIndex *idx, const uint32_t *nodes, uint32_t n, MEInfo *out);

int me_index_refresh_path(MEIndex *idx, const char *path);
int me_index_forget_path(MEIndex *idx, const char *path);
int me_index_rescan(MEIndex *idx, const char *path);

int me_index_watch_start(MEIndex *idx, void (*on_change)(void *ctx), void *ctx);
void me_index_watch_stop(MEIndex *idx);

int me_default_index_path(char *buf, size_t cap);
const char *me_last_error(void);

int me_app_main(int argc, const char **argv);

#endif
