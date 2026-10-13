#define _DARWIN_C_SOURCE 1
#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

enum { SLOT_EMPTY = 0, SLOT_FULL = 1, SLOT_TOMB = 2 };

static uint64_t mix_hash(uint32_t parent, const char *name, size_t len) {
  uint64_t h = 14695981039346656037ull;
  h ^= (uint64_t)parent + 1ull;
  h *= 1099511628211ull;
  for (size_t i = 0; i < len; i++) {
    h ^= (unsigned char)name[i];
    h *= 1099511628211ull;
  }
  if (h == 0) h = 1;
  return h;
}

void me_bump(MEIndex *idx) { atomic_fetch_add_explicit(&idx->generation, 1, memory_order_relaxed); }

static int ensure_nodes(MEIndex *idx, uint32_t need) {
  if (idx->cap >= need) return 0;
  uint32_t cap = idx->cap ? idx->cap : 1024;
  while (cap < need) {
    if (cap > UINT32_MAX / 2) return -1;
    cap *= 2;
  }
  MEKey *keys = realloc(idx->keys, (size_t)cap * sizeof(MEKey));
  if (!keys) return -1;
  idx->keys = keys;
  MENode *nodes = realloc(idx->nodes, (size_t)cap * sizeof(MENode));
  if (!nodes) return -1;
  idx->nodes = nodes;
  idx->cap = cap;
  return 0;
}

static int blob_add(MEIndex *idx, const char *bytes, size_t len, uint32_t *off) {
  if (idx->blob_len > UINT32_MAX - len - 1) return -1;
  uint32_t need = idx->blob_len + (uint32_t)len + 1;
  if (need > idx->blob_cap) {
    uint32_t cap = idx->blob_cap ? idx->blob_cap : 4096;
    while (cap < need) {
      if (cap > UINT32_MAX / 2) return -1;
      cap *= 2;
    }
    char *blob = realloc(idx->blob, cap);
    if (!blob) return -1;
    idx->blob = blob;
    idx->blob_cap = cap;
  }
  *off = idx->blob_len;
  memcpy(idx->blob + *off, bytes, len);
  idx->blob[*off + (uint32_t)len] = 0;
  idx->blob_len = need;
  return 0;
}

static void hash_place(MEIndex *idx, uint64_t h, uint32_t node) {
  uint32_t mask = idx->slot_cap - 1;
  uint32_t i = (uint32_t)h & mask;
  for (;;) {
    Slot *s = &idx->slots[i];
    if (s->state != SLOT_FULL) {
      if (s->state == SLOT_EMPTY) idx->slot_used++;
      s->state = SLOT_FULL;
      s->hash = h;
      s->node = node;
      idx->slot_live++;
      return;
    }
    i = (i + 1) & mask;
  }
}

static int hash_grow(MEIndex *idx, uint32_t min_cap) {
  uint32_t cap = idx->slot_cap ? idx->slot_cap : 1024;
  while (cap < min_cap) {
    if (cap > UINT32_MAX / 2) return -1;
    cap *= 2;
  }
  Slot *fresh = calloc(cap, sizeof(Slot));
  if (!fresh) return -1;
  Slot *old = idx->slots;
  uint32_t old_cap = idx->slot_cap;
  idx->slots = fresh;
  idx->slot_cap = cap;
  idx->slot_live = 0;
  idx->slot_used = 0;
  if (old) {
    for (uint32_t i = 0; i < old_cap; i++) {
      if (old[i].state == SLOT_FULL) hash_place(idx, old[i].hash, old[i].node);
    }
    free(old);
  }
  return 0;
}

static int hash_ensure(MEIndex *idx) {
  uint32_t need = idx->slot_live + 1;
  if (idx->slot_cap == 0 || need * 10 > idx->slot_cap * 7 || idx->slot_used * 10 > idx->slot_cap * 7) {
    uint32_t min = idx->slot_live * 2 + 16;
    if (min < 1024) min = 1024;
    return hash_grow(idx, min);
  }
  return 0;
}

static uint32_t hash_find(const MEIndex *idx, uint32_t parent, const char *name, size_t len, uint64_t h) {
  if (!idx->slots || idx->slot_cap == 0) return ME_NONE;
  uint32_t mask = idx->slot_cap - 1;
  uint32_t i = (uint32_t)h & mask;
  for (uint32_t steps = 0; steps < idx->slot_cap; steps++) {
    const Slot *s = &idx->slots[i];
    if (s->state == SLOT_EMPTY) return ME_NONE;
    if (s->state == SLOT_FULL && s->hash == h) {
      const MENode *n = &idx->nodes[s->node];
      const MEKey *k = &idx->keys[s->node];
      if (n->parent == parent && !(k->flags & ME_F_DEAD) && k->len == len &&
          memcmp(idx->blob + k->off, name, len) == 0) {
        return s->node;
      }
    }
    i = (i + 1) & mask;
  }
  return ME_NONE;
}

static void hash_remove(MEIndex *idx, uint32_t node) {
  if (!idx->slots || idx->slot_cap == 0) return;
  if (idx->nodes[node].parent == ME_NONE) return;
  const MEKey *k = &idx->keys[node];
  uint64_t h = mix_hash(idx->nodes[node].parent, idx->blob + k->off, k->len);
  uint32_t mask = idx->slot_cap - 1;
  uint32_t i = (uint32_t)h & mask;
  for (uint32_t steps = 0; steps < idx->slot_cap; steps++) {
    Slot *s = &idx->slots[i];
    if (s->state == SLOT_EMPTY) return;
    if (s->state == SLOT_FULL && s->node == node) {
      s->state = SLOT_TOMB;
      if (idx->slot_live) idx->slot_live--;
      return;
    }
    i = (i + 1) & mask;
  }
}

void me_rebuild_hash_ul(MEIndex *idx) {
  free(idx->slots);
  idx->slots = NULL;
  idx->slot_cap = 0;
  idx->slot_live = 0;
  idx->slot_used = 0;
  for (uint32_t i = 0; i < idx->count; i++) {
    if (idx->keys[i].flags & ME_F_DEAD) continue;
    if (idx->nodes[i].parent == ME_NONE) continue;
    if (hash_ensure(idx) != 0) return;
    const MEKey *k = &idx->keys[i];
    hash_place(idx, mix_hash(idx->nodes[i].parent, idx->blob + k->off, k->len), i);
  }
}

static void link_child(MEIndex *idx, uint32_t parent, uint32_t child) {
  MENode *c = &idx->nodes[child];
  MENode *p = &idx->nodes[parent];
  c->parent = parent;
  c->prev_sibling = ME_NONE;
  c->next_sibling = p->first_child;
  if (p->first_child != ME_NONE) idx->nodes[p->first_child].prev_sibling = child;
  p->first_child = child;
}

static void unlink_node(MEIndex *idx, uint32_t node) {
  MENode *n = &idx->nodes[node];
  uint32_t parent = n->parent;
  uint32_t prev = n->prev_sibling;
  uint32_t next = n->next_sibling;
  if (prev != ME_NONE) {
    idx->nodes[prev].next_sibling = next;
  } else if (parent != ME_NONE && idx->nodes[parent].first_child == node) {
    idx->nodes[parent].first_child = next;
  }
  if (next != ME_NONE) idx->nodes[next].prev_sibling = prev;
  n->next_sibling = ME_NONE;
  n->prev_sibling = ME_NONE;
}

static void mark_dead(MEIndex *idx, uint32_t node) {
  if (idx->keys[node].flags & ME_F_DEAD) return;
  hash_remove(idx, node);
  unlink_node(idx, node);
  idx->keys[node].flags |= ME_F_DEAD;
  if (idx->live) idx->live--;
}

static void kill_tree(MEIndex *idx, uint32_t node) {
  if (idx->nodes[node].first_child == ME_NONE) {
    mark_dead(idx, node);
    return;
  }
  uint32_t *stack = malloc((size_t)idx->count * sizeof(uint32_t));
  uint32_t *order = malloc((size_t)idx->count * sizeof(uint32_t));
  if (!stack || !order) {
    free(stack);
    free(order);
    mark_dead(idx, node);
    return;
  }
  uint32_t sp = 0, op = 0;
  stack[sp++] = node;
  while (sp) {
    uint32_t n = stack[--sp];
    order[op++] = n;
    for (uint32_t c = idx->nodes[n].first_child; c != ME_NONE; c = idx->nodes[c].next_sibling) {
      if (sp < idx->count) stack[sp++] = c;
    }
  }
  for (uint32_t i = op; i-- > 0;) mark_dead(idx, order[i]);
  free(stack);
  free(order);
}

static const char *basename_of(const char *path, size_t *len) {
  size_t n = strlen(path);
  while (n > 1 && path[n - 1] == '/') n--;
  if (n == 1 && path[0] == '/') {
    *len = 1;
    return path;
  }
  size_t i = n;
  while (i > 0 && path[i - 1] != '/') i--;
  *len = n - i;
  return path + i;
}

static int best_root(const MEIndex *idx, const char *folded, size_t flen) {
  int best = -1;
  size_t best_len = 0;
  for (uint32_t i = 0; i < idx->root_count; i++) {
    size_t rl = strlen(idx->roots[i].fold);
    if (rl == 1 && idx->roots[i].fold[0] == '/') {
      if (best < 0) {
        best = (int)i;
        best_len = 1;
      }
      continue;
    }
    if (flen >= rl && memcmp(folded, idx->roots[i].fold, rl) == 0 &&
        (flen == rl || folded[rl] == '/')) {
      if (rl >= best_len) {
        best = (int)i;
        best_len = rl;
      }
    }
  }
  return best;
}

static int canonicalize_path(const char *in, char *out, size_t cap) {
  if (!in || !in[0] || strlen(in) >= 8192) return -1;
  if (realpath(in, out)) return 0;
  char buf[8192];
  char suffix[8192];
  memcpy(buf, in, strlen(in) + 1);
  suffix[0] = 0;
  for (;;) {
    if (strcmp(buf, "/") == 0) return -1;
    char *slash = strrchr(buf, '/');
    if (!slash) return -1;
    size_t add = strlen(slash);
    size_t suflen = strlen(suffix);
    if (add + suflen >= sizeof suffix) return -1;
    memmove(suffix + add, suffix, suflen + 1);
    memcpy(suffix, slash, add);
    if (slash == buf) buf[1] = 0;
    else *slash = 0;
    if (!realpath(buf, out)) continue;
    if (out[0] == '/' && out[1] == 0) return -1;
    size_t ol = strlen(out);
    size_t sl = strlen(suffix);
    if (ol + sl + 1 > cap) return -1;
    memcpy(out + ol, suffix, sl + 1);
    return 0;
  }
}

static uint32_t lookup_folded(MEIndex *idx, const char *path) {
  char folded[8192];
  size_t fl = 0;
  if (!path || me_fold_path(path, folded, sizeof folded, &fl) != 0) return ME_NONE;
  int ri = best_root(idx, folded, fl);
  if (ri < 0) return ME_NONE;
  const MERoot *root = &idx->roots[ri];
  uint32_t node = root->node;
  size_t rl = strlen(root->fold);
  const char *p = folded + (rl == 1 && root->fold[0] == '/' ? 1 : rl);
  if (*p == '/') p++;
  while (*p) {
    const char *s = p;
    while (*p && *p != '/') p++;
    size_t n = (size_t)(p - s);
    if (n == 0) break;
    uint64_t h = mix_hash(node, s, n);
    uint32_t next = hash_find(idx, node, s, n, h);
    if (next == ME_NONE) return ME_NONE;
    node = next;
    if (*p == '/') p++;
  }
  return node;
}

uint32_t me_lookup_ul(MEIndex *idx, const char *path) {
  uint32_t node = lookup_folded(idx, path);
  if (node != ME_NONE) return node;
  char canon[8192];
  if (canonicalize_path(path, canon, sizeof canon) != 0 || strcmp(canon, path) == 0) return ME_NONE;
  return lookup_folded(idx, canon);
}

int me_should_skip(const char *parent_base, size_t parent_len, const char *name, size_t name_len) {
  if (name_len == 0) return 1;
  if (name_len == 1 && name[0] == '.') return 1;
  if (name_len == 2 && name[0] == '.' && name[1] == '.') return 1;
  static const char *skip[] = {".fseventsd", ".Spotlight-V100", ".DocumentRevisions-V100",
                               ".TemporaryItems", ".Trashes", ".Trash"};
  for (size_t i = 0; i < sizeof skip / sizeof skip[0]; i++) {
    size_t n = strlen(skip[i]);
    if (n == name_len && memcmp(name, skip[i], n) == 0) return 1;
  }
  if (parent_len == 7 && memcmp(parent_base, "Library", 7) == 0) {
    if (name_len == 6 && memcmp(name, "Caches", 6) == 0) return 1;
    if (name_len == 6 && memcmp(name, "Photos", 6) == 0) return 1;
    /* Container Data folders block in openat and are app-private, not user files. */
    if (name_len == 10 && memcmp(name, "Containers", 10) == 0) return 1;
    if (name_len == 16 && memcmp(name, "Group Containers", 16) == 0) return 1;
    if (name_len == 17 && memcmp(name, "Daemon Containers", 17) == 0) return 1;
  }
  return 0;
}

static int ignored_under_root(const MEIndex *idx, const char *path, int *found) {
  *found = 0;
  char folded[8192];
  size_t fl = 0;
  if (me_fold_path(path, folded, sizeof folded, &fl) != 0) return 1;
  int ri = best_root(idx, folded, fl);
  if (ri < 0) return 1;
  *found = 1;
  const MERoot *root = &idx->roots[ri];
  if (root->path_len > strlen(path)) return 1;
  const char *rel = path + root->path_len;
  if (*rel == '/') rel++;
  char prev[768];
  prev[0] = 0;
  while (*rel) {
    const char *s = rel;
    while (*rel && *rel != '/') rel++;
    size_t n = (size_t)(rel - s);
    if (me_should_skip(prev, strlen(prev), s, n)) return 1;
    size_t c = n < sizeof prev - 1 ? n : sizeof prev - 1;
    memcpy(prev, s, c);
    prev[c] = 0;
    if (*rel == '/') rel++;
  }
  return 0;
}

int me_path_ignored(const MEIndex *idx, const char *path) {
  if (!path) return 1;
  if (strstr(path, "/MacEverything/index.bin")) return 1;
  int found = 0;
  int ign = ignored_under_root(idx, path, &found);
  if (found) return ign;
  char canon[8192];
  if (canonicalize_path(path, canon, sizeof canon) != 0 || strcmp(canon, path) == 0) return 1;
  if (strstr(canon, "/MacEverything/index.bin")) return 1;
  return ignored_under_root(idx, canon, &found);
}

static int store_names(MEIndex *idx, const char *name, size_t name_len, uint32_t *name_off,
                       uint32_t *search_off, uint16_t *search_len) {
  if (name_len > 65535) return -1;
  char tmp[1024];
  const char *folded = NULL;
  size_t fl = 0;
  if (name_len > sizeof tmp) return -1;
  if (me_fold(name, name_len, tmp, sizeof tmp, &folded, &fl) != 0) return -1;
  if (fl > 65535) return -1;
  if (blob_add(idx, name, name_len, name_off) != 0) return -1;
  if (folded == name) {
    *search_off = *name_off;
    *search_len = (uint16_t)name_len;
    return 0;
  }
  if (blob_add(idx, folded, fl, search_off) != 0) return -1;
  *search_len = (uint16_t)fl;
  return 0;
}

uint32_t me_add_root_ul(MEIndex *idx, const char *absolute_path, uint64_t event_id,
                        const uint8_t uuid[16], uint64_t device) {
  if (!absolute_path || absolute_path[0] != '/') {
    me_set_error("root must be absolute");
    return ME_NONE;
  }
  if (idx->root_count >= ME_MAX_ROOTS) {
    me_set_error("too many roots");
    return ME_NONE;
  }
  size_t path_len = strlen(absolute_path);
  while (path_len > 1 && absolute_path[path_len - 1] == '/') path_len--;
  if (path_len >= 1024) {
    me_set_error("root path too long");
    return ME_NONE;
  }
  char fold[1024];
  size_t fl = 0;
  char trimmed[1024];
  memcpy(trimmed, absolute_path, path_len);
  trimmed[path_len] = 0;
  if (me_fold_path(trimmed, fold, sizeof fold, &fl) != 0 || fl >= 1024) {
    me_set_error("could not fold root path");
    return ME_NONE;
  }
  size_t base_len = 0;
  const char *base = basename_of(trimmed, &base_len);
  if (ensure_nodes(idx, idx->count + 1) != 0) return ME_NONE;
  uint32_t id = idx->count++;
  memset(&idx->nodes[id], 0, sizeof(MENode));
  idx->nodes[id].parent = ME_NONE;
  idx->nodes[id].first_child = ME_NONE;
  idx->nodes[id].next_sibling = ME_NONE;
  idx->nodes[id].prev_sibling = ME_NONE;
  uint32_t name_off = 0, search_off = 0;
  uint16_t search_len = 0;
  if (store_names(idx, base, base_len, &name_off, &search_off, &search_len) != 0) {
    idx->count--;
    return ME_NONE;
  }
  idx->nodes[id].name_off = name_off;
  idx->nodes[id].name_len = (uint16_t)base_len;
  idx->keys[id].off = search_off;
  idx->keys[id].len = search_len;
  idx->keys[id].flags = ME_F_DIR;
  MERoot *r = &idx->roots[idx->root_count++];
  memset(r, 0, sizeof *r);
  memcpy(r->path, trimmed, path_len + 1);
  memcpy(r->fold, fold, fl + 1);
  r->node = id;
  r->path_len = (uint32_t)path_len;
  r->event_id = event_id;
  r->device = device;
  if (uuid) memcpy(r->uuid, uuid, 16);
  idx->live++;
  idx->dirty = 1;
  atomic_fetch_add_explicit(&idx->progress, 1, memory_order_relaxed);
  return id;
}

uint32_t me_add_child_ul(MEIndex *idx, uint32_t parent, const char *name, size_t name_len,
                         uint64_t file_id, uint64_t size, int64_t mtime, uint32_t flags) {
  if (parent == ME_NONE || parent >= idx->count || !name || name_len == 0) return ME_NONE;
  flags &= (ME_F_DIR | ME_F_LINK);
  char tmp[1024];
  const char *folded = NULL;
  size_t fl = 0;
  if (name_len > sizeof tmp || me_fold(name, name_len, tmp, sizeof tmp, &folded, &fl) != 0) return ME_NONE;
  uint64_t h = mix_hash(parent, folded, fl);
  uint32_t existing = hash_find(idx, parent, folded, fl, h);
  if (existing != ME_NONE) {
    MENode *n = &idx->nodes[existing];
    n->file_id = file_id;
    n->size = size;
    n->mtime = mtime;
    idx->keys[existing].flags = (uint16_t)((idx->keys[existing].flags & ME_F_DEAD) | flags);
    idx->keys[existing].flags &= (uint16_t)~ME_F_DEAD;
    idx->dirty = 1;
    return existing;
  }
  if (ensure_nodes(idx, idx->count + 1) != 0) return ME_NONE;
  if (hash_ensure(idx) != 0) return ME_NONE;
  uint32_t id = idx->count++;
  memset(&idx->nodes[id], 0, sizeof(MENode));
  idx->nodes[id].file_id = file_id;
  idx->nodes[id].size = size;
  idx->nodes[id].mtime = mtime;
  idx->nodes[id].first_child = ME_NONE;
  idx->nodes[id].next_sibling = ME_NONE;
  idx->nodes[id].prev_sibling = ME_NONE;
  uint32_t name_off = 0, search_off = 0;
  uint16_t search_len = 0;
  if (store_names(idx, name, name_len, &name_off, &search_off, &search_len) != 0) {
    idx->count--;
    return ME_NONE;
  }
  idx->nodes[id].name_off = name_off;
  idx->nodes[id].name_len = (uint16_t)name_len;
  idx->keys[id].off = search_off;
  idx->keys[id].len = search_len;
  idx->keys[id].flags = (uint16_t)flags;
  link_child(idx, parent, id);
  hash_place(idx, mix_hash(parent, idx->blob + search_off, search_len), id);
  idx->live++;
  idx->dirty = 1;
  atomic_fetch_add_explicit(&idx->progress, 1, memory_order_relaxed);
  return id;
}

int me_build_path_ul(const MEIndex *idx, uint32_t node, char *buf, size_t cap) {
  if (!buf || cap == 0 || node >= idx->count) return -1;
  uint32_t stack[1024];
  int sp = 0;
  uint32_t cur = node;
  while (cur != ME_NONE && sp < 1024) {
    stack[sp++] = cur;
    if (idx->nodes[cur].parent == cur) return -1;
    cur = idx->nodes[cur].parent;
  }
  if (sp == 0) return -1;
  uint32_t top = stack[sp - 1];
  const MERoot *root = NULL;
  for (uint32_t i = 0; i < idx->root_count; i++) {
    if (idx->roots[i].node == top) {
      root = &idx->roots[i];
      break;
    }
  }
  if (!root) return -1;
  size_t len = 0;
  size_t rp = strlen(root->path);
  if (rp + 1 > cap) return -1;
  memcpy(buf, root->path, rp);
  len = rp;
  int slash = !(rp == 1 && buf[0] == '/');
  for (int i = sp - 2; i >= 0; i--) {
    const MENode *n = &idx->nodes[stack[i]];
    if (slash) {
      if (len + 1 >= cap) return -1;
      buf[len++] = '/';
    }
    slash = 1;
    if (len + n->name_len >= cap) return -1;
    memcpy(buf + len, idx->blob + n->name_off, n->name_len);
    len += n->name_len;
  }
  buf[len] = 0;
  return 0;
}

int me_compact_ul(MEIndex *idx) {
  if (idx->live == idx->count) return 0;
  uint32_t nlive = idx->live;
  uint32_t *remap = malloc((size_t)idx->count * sizeof(uint32_t));
  MEKey *keys = calloc(nlive ? nlive : 1, sizeof(MEKey));
  MENode *nodes = calloc(nlive ? nlive : 1, sizeof(MENode));
  if (!remap || !keys || !nodes) {
    free(remap);
    free(keys);
    free(nodes);
    return -1;
  }
  uint32_t w = 0;
  for (uint32_t i = 0; i < idx->count; i++) {
    if (idx->keys[i].flags & ME_F_DEAD) {
      remap[i] = ME_NONE;
      continue;
    }
    remap[i] = w++;
  }
  char *blob = NULL;
  uint32_t blob_len = 0, blob_cap = 0;
  for (uint32_t i = 0; i < idx->count; i++) {
    if (remap[i] == ME_NONE) continue;
    uint32_t d = remap[i];
    nodes[d] = idx->nodes[i];
    keys[d] = idx->keys[i];
    nodes[d].parent = nodes[d].parent == ME_NONE ? ME_NONE : remap[nodes[d].parent];
    nodes[d].first_child = ME_NONE;
    nodes[d].next_sibling = ME_NONE;
    nodes[d].prev_sibling = ME_NONE;
  }
  /* Rebuild names into a tight blob. Search keys that aliased the display name stay aliased. */
  for (uint32_t i = 0; i < idx->count; i++) {
    if (remap[i] == ME_NONE) continue;
    uint32_t d = remap[i];
    const char *name = idx->blob + idx->nodes[i].name_off;
    uint16_t name_len = idx->nodes[i].name_len;
    int alias = idx->keys[i].off == idx->nodes[i].name_off;
    uint32_t need = blob_len + name_len + 1;
    if (!alias) need += idx->keys[i].len + 1;
    if (need > blob_cap) {
      uint32_t cap = blob_cap ? blob_cap : 4096;
      while (cap < need) cap *= 2;
      char *nb = realloc(blob, cap);
      if (!nb) {
        free(remap);
        free(keys);
        free(nodes);
        free(blob);
        return -1;
      }
      blob = nb;
      blob_cap = cap;
    }
    uint32_t name_off = blob_len;
    memcpy(blob + name_off, name, name_len);
    blob[name_off + name_len] = 0;
    blob_len = name_off + name_len + 1;
    nodes[d].name_off = name_off;
    nodes[d].name_len = name_len;
    if (alias) {
      keys[d].off = name_off;
      keys[d].len = name_len;
    } else {
      uint32_t soff = blob_len;
      memcpy(blob + soff, idx->blob + idx->keys[i].off, idx->keys[i].len);
      blob[soff + idx->keys[i].len] = 0;
      blob_len = soff + idx->keys[i].len + 1;
      keys[d].off = soff;
      keys[d].len = idx->keys[i].len;
    }
  }
  if (!blob) {
    blob = calloc(1, 1);
    blob_cap = 1;
  }
  for (uint32_t r = 0; r < idx->root_count; r++) {
    uint32_t mapped = remap[idx->roots[r].node];
    idx->roots[r].node = mapped;
  }
  uint32_t rr = 0;
  for (uint32_t r = 0; r < idx->root_count; r++) {
    if (idx->roots[r].node == ME_NONE) continue;
    if (rr != r) idx->roots[rr] = idx->roots[r];
    rr++;
  }
  idx->root_count = rr;
  free(idx->keys);
  free(idx->nodes);
  free(idx->blob);
  idx->keys = keys;
  idx->nodes = nodes;
  idx->blob = blob;
  idx->blob_len = blob_len;
  idx->blob_cap = blob_cap;
  idx->count = w;
  idx->cap = nlive ? nlive : 1;
  idx->live = w;
  free(remap);
  for (uint32_t i = 0; i < idx->count; i++) {
    if (idx->nodes[i].parent != ME_NONE) link_child(idx, idx->nodes[i].parent, i);
  }
  me_rebuild_hash_ul(idx);
  idx->dirty = 1;
  return 0;
}

int me_thaw(MEIndex *idx) {
  pthread_rwlock_rdlock(&idx->lock);
  if (idx->writable) {
    pthread_rwlock_unlock(&idx->lock);
    return 0;
  }
  uint32_t n = idx->count;
  uint32_t blob_len = idx->blob_len;
  MEKey *keys = malloc((n ? n : 1) * sizeof(MEKey));
  MENode *nodes = malloc((n ? n : 1) * sizeof(MENode));
  char *blob = malloc(blob_len ? blob_len : 1);
  if (!keys || !nodes || !blob) {
    free(keys);
    free(nodes);
    free(blob);
    pthread_rwlock_unlock(&idx->lock);
    me_set_error("out of memory");
    return -1;
  }
  if (n) {
    memcpy(keys, idx->keys, (size_t)n * sizeof(MEKey));
    memcpy(nodes, idx->nodes, (size_t)n * sizeof(MENode));
  }
  if (blob_len) memcpy(blob, idx->blob, blob_len);
  pthread_rwlock_unlock(&idx->lock);

  pthread_rwlock_wrlock(&idx->lock);
  if (idx->writable) {
    free(keys);
    free(nodes);
    free(blob);
    pthread_rwlock_unlock(&idx->lock);
    return 0;
  }
  if (idx->map) {
    munmap(idx->map, idx->map_len);
    idx->map = NULL;
    idx->map_len = 0;
  }
  if (idx->fd >= 0) {
    close(idx->fd);
    idx->fd = -1;
  }
  idx->keys = keys;
  idx->nodes = nodes;
  idx->blob = blob;
  idx->cap = n ? n : 1;
  idx->blob_cap = blob_len ? blob_len : 1;
  idx->writable = 1;
  me_rebuild_hash_ul(idx);
  pthread_rwlock_unlock(&idx->lock);
  return 0;
}

static void fill_info(const MEIndex *idx, uint32_t node, MEInfo *out) {
  memset(out, 0, sizeof *out);
  if (node >= idx->count) return;
  const MENode *n = &idx->nodes[node];
  const MEKey *k = &idx->keys[node];
  size_t nl = n->name_len;
  if (nl >= sizeof out->name) nl = sizeof out->name - 1;
  memcpy(out->name, idx->blob + n->name_off, nl);
  out->name[nl] = 0;
  me_build_path_ul(idx, node, out->path, sizeof out->path);
  out->size = n->size;
  out->mtime = n->mtime;
  out->is_dir = (k->flags & ME_F_DIR) ? 1 : 0;
  out->is_link = (k->flags & ME_F_LINK) ? 1 : 0;
}

MEIndex *me_index_create(void) {
  MEIndex *idx = calloc(1, sizeof *idx);
  if (!idx) return NULL;
  idx->fd = -1;
  idx->writable = 1;
  if (pthread_rwlock_init(&idx->lock, NULL) != 0) {
    free(idx);
    return NULL;
  }
  atomic_store(&idx->generation, 1);
  return idx;
}

void me_index_free(MEIndex *idx) {
  if (!idx) return;
  me_index_watch_stop(idx);
  if (idx->map) munmap(idx->map, idx->map_len);
  if (idx->fd >= 0) close(idx->fd);
  if (idx->writable) {
    free(idx->keys);
    free(idx->nodes);
    free(idx->blob);
  }
  free(idx->slots);
  pthread_rwlock_destroy(&idx->lock);
  free(idx);
}

static void read_lock(const MEIndex *idx) { pthread_rwlock_rdlock((pthread_rwlock_t *)&idx->lock); }
static void read_unlock(const MEIndex *idx) { pthread_rwlock_unlock((pthread_rwlock_t *)&idx->lock); }

uint32_t me_index_count(const MEIndex *idx) {
  if (!idx) return 0;
  read_lock(idx);
  uint32_t n = idx->count;
  read_unlock(idx);
  return n;
}
uint32_t me_index_live(const MEIndex *idx) {
  if (!idx) return 0;
  read_lock(idx);
  uint32_t n = idx->live;
  read_unlock(idx);
  return n;
}
uint64_t me_index_generation(const MEIndex *idx) {
  return idx ? atomic_load_explicit(&idx->generation, memory_order_relaxed) : 0;
}
int me_index_dirty(const MEIndex *idx) {
  if (!idx) return 0;
  read_lock(idx);
  int dirty = idx->dirty || idx->eid_dirty;
  read_unlock(idx);
  return dirty;
}
int me_index_scanning(const MEIndex *idx) {
  return idx && atomic_load_explicit(&idx->scanning, memory_order_acquire);
}
void me_index_set_complete(MEIndex *idx, int complete) {
  if (!idx) return;
  int v = complete ? 1 : 0;
  if (idx->complete == v) return;
  idx->complete = v;
  idx->dirty = 1;
}
int me_index_complete(const MEIndex *idx) { return idx && idx->complete; }
int me_index_is_mapped(const MEIndex *idx) {
  if (!idx) return 0;
  read_lock(idx);
  int mapped = !idx->writable && idx->map != NULL;
  read_unlock(idx);
  return mapped;
}
uint64_t me_index_progress(const MEIndex *idx) {
  return idx ? atomic_load_explicit(&idx->progress, memory_order_relaxed) : 0;
}
uint64_t me_index_storage_bytes(const MEIndex *idx) {
  if (!idx) return 0;
  read_lock(idx);
  uint64_t n;
  if (!idx->writable && idx->map) n = idx->map_len;
  else {
    n = (uint64_t)idx->cap * sizeof(MEKey) + (uint64_t)idx->cap * sizeof(MENode) + idx->blob_cap +
        (uint64_t)idx->slot_cap * sizeof(Slot);
  }
  read_unlock(idx);
  return n;
}
uint32_t me_index_root_count(const MEIndex *idx) { return idx ? idx->root_count : 0; }

int me_index_root(const MEIndex *idx, uint32_t i, char *buf, size_t cap) {
  if (!idx || !buf || i >= idx->root_count || cap == 0) return -1;
  snprintf(buf, cap, "%s", idx->roots[i].path);
  return 0;
}

uint32_t me_index_add_root(MEIndex *idx, const char *absolute_path) {
  if (me_thaw(idx) != 0) return ME_NONE;
  pthread_rwlock_wrlock(&idx->lock);
  uint32_t id = me_add_root_ul(idx, absolute_path, 0, NULL, 0);
  if (id != ME_NONE) me_bump(idx);
  pthread_rwlock_unlock(&idx->lock);
  return id;
}

uint32_t me_index_add_child(MEIndex *idx, uint32_t parent, const char *name, uint64_t file_id,
                            uint64_t size, int64_t mtime, uint32_t flags) {
  if (me_thaw(idx) != 0) return ME_NONE;
  pthread_rwlock_wrlock(&idx->lock);
  uint32_t id = me_add_child_ul(idx, parent, name, name ? strlen(name) : 0, file_id, size, mtime, flags);
  if (id != ME_NONE) me_bump(idx);
  pthread_rwlock_unlock(&idx->lock);
  return id;
}

int me_index_info(MEIndex *idx, uint32_t node, MEInfo *out) {
  if (!idx || !out) return -1;
  pthread_rwlock_rdlock(&idx->lock);
  fill_info(idx, node, out);
  pthread_rwlock_unlock(&idx->lock);
  return 0;
}

int me_index_infos(MEIndex *idx, const uint32_t *nodes, uint32_t n, MEInfo *out) {
  if (!idx || (!nodes && n) || !out) return -1;
  pthread_rwlock_rdlock(&idx->lock);
  for (uint32_t i = 0; i < n; i++) fill_info(idx, nodes[i], &out[i]);
  pthread_rwlock_unlock(&idx->lock);
  return 0;
}

static int refresh_ul(MEIndex *idx, const char *path) {
  struct stat st;
  if (lstat(path, &st) != 0) {
    if (errno == ENOENT) {
      uint32_t node = me_lookup_ul(idx, path);
      if (node != ME_NONE) kill_tree(idx, node);
      return 0;
    }
    return -1;
  }
  uint32_t existing = me_lookup_ul(idx, path);
  uint32_t flags = 0;
  if (S_ISDIR(st.st_mode)) flags |= ME_F_DIR;
  if (S_ISLNK(st.st_mode)) flags |= ME_F_LINK;
  if (existing != ME_NONE) {
    idx->nodes[existing].file_id = (uint64_t)st.st_ino;
    idx->nodes[existing].size = S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size;
    idx->nodes[existing].mtime = (int64_t)st.st_mtimespec.tv_sec;
    uint16_t keep_dead = idx->keys[existing].flags & ME_F_DEAD;
    idx->keys[existing].flags = (uint16_t)(flags | keep_dead);
    idx->dirty = 1;
    return 0;
  }
  char tmp[8192];
  size_t plen = strlen(path);
  if (plen >= sizeof tmp) return -1;
  memcpy(tmp, path, plen + 1);
  while (plen > 1 && tmp[plen - 1] == '/') {
    tmp[--plen] = 0;
  }
  char *slash = strrchr(tmp, '/');
  if (!slash) return -1;
  char *base = slash + 1;
  if (slash == tmp) {
    /* parent is root "/" — only valid when a root is "/" */
  }
  char parent_path[8192];
  if (slash == tmp) {
    memcpy(parent_path, "/", 2);
  } else {
    size_t pl = (size_t)(slash - tmp);
    memcpy(parent_path, tmp, pl);
    parent_path[pl] = 0;
  }
  uint32_t parent = me_lookup_ul(idx, parent_path);
  if (parent == ME_NONE) {
    if (strcmp(parent_path, "/") == 0) return -1;
    if (refresh_ul(idx, parent_path) != 0) return -1;
    parent = me_lookup_ul(idx, parent_path);
    if (parent == ME_NONE) return -1;
  }
  size_t parent_base_len = 0;
  const char *parent_base = basename_of(parent_path, &parent_base_len);
  if (me_should_skip(parent_base, parent_base_len, base, strlen(base))) return 0;
  uint64_t sz = (flags & ME_F_DIR) ? 0 : (uint64_t)st.st_size;
  uint32_t id = me_add_child_ul(idx, parent, base, strlen(base), (uint64_t)st.st_ino, sz,
                                (int64_t)st.st_mtimespec.tv_sec, flags);
  return id == ME_NONE ? -1 : 0;
}

int me_index_refresh_path(MEIndex *idx, const char *path) {
  if (!idx || !path) return -1;
  if (me_thaw(idx) != 0) return -1;
  pthread_rwlock_wrlock(&idx->lock);
  int rc = 0;
  if (!me_path_ignored(idx, path)) rc = refresh_ul(idx, path);
  if (rc == 0) me_bump(idx);
  pthread_rwlock_unlock(&idx->lock);
  return rc;
}

int me_index_forget_path(MEIndex *idx, const char *path) {
  if (!idx || !path) return -1;
  if (me_thaw(idx) != 0) return -1;
  pthread_rwlock_wrlock(&idx->lock);
  if (!me_path_ignored(idx, path)) {
    uint32_t node = me_lookup_ul(idx, path);
    if (node != ME_NONE) kill_tree(idx, node);
  }
  me_bump(idx);
  pthread_rwlock_unlock(&idx->lock);
  return 0;
}

void me_note_event(MEIndex *idx, const char *path, uint64_t evid) {
  if (!idx || !path || evid == 0) return;
  char folded[8192];
  size_t fl = 0;
  const char *use = path;
  char canon[8192];
  if (me_fold_path(path, folded, sizeof folded, &fl) != 0) return;
  pthread_rwlock_rdlock(&idx->lock);
  int ri0 = best_root(idx, folded, fl);
  pthread_rwlock_unlock(&idx->lock);
  if (ri0 < 0 && canonicalize_path(path, canon, sizeof canon) == 0) {
    use = canon;
    if (me_fold_path(use, folded, sizeof folded, &fl) != 0) return;
  }
  pthread_rwlock_wrlock(&idx->lock);
  int ri = best_root(idx, folded, fl);
  if (ri >= 0 && evid > idx->roots[ri].event_id) {
    idx->roots[ri].event_id = evid;
    idx->eid_dirty = 1;
  }
  pthread_rwlock_unlock(&idx->lock);
}

int me_default_index_path(char *buf, size_t cap) {
  const char *home = getenv("HOME");
  if (!home) home = "/tmp";
  int n = snprintf(buf, cap, "%s/Library/Application Support/MacEverything/index.bin", home);
  if (n < 0 || (size_t)n >= cap) return -1;
  return 0;
}
