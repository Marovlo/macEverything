#define _DARWIN_C_SOURCE 1
#include "internal.h"

#include <arm_neon.h>
#include <stdlib.h>
#include <string.h>

static int combining_lead(unsigned char c) { return c == 0xCC || c == 0xCD; }

/* A match that ends on a base character must not leave a combining mark
 * behind. Otherwise "cafe" matches the NFD bytes of "café". */
static int accept_hit(const unsigned char *h, size_t hlen, size_t pos, const unsigned char *nd,
                      size_t nlen) {
  if (pos + nlen > hlen || memcmp(h + pos, nd, nlen) != 0) return 0;
  size_t end = pos + nlen;
  int needle_combining = nlen >= 2 && combining_lead(nd[nlen - 2]);
  if (end < hlen && combining_lead(h[end]) && !needle_combining) return 0;
  return 1;
}

int me_contains(const char *hay, size_t hlen, const char *needle, size_t nlen) {
  if (nlen == 0) return 1;
  if (nlen > hlen) return 0;
  const unsigned char *h = (const unsigned char *)hay;
  const unsigned char *nd = (const unsigned char *)needle;
  if (nlen == 1) {
    const unsigned char *p = h;
    size_t left = hlen;
    while (left) {
      const unsigned char *hit = memchr(p, nd[0], left);
      if (!hit) return 0;
      size_t pos = (size_t)(hit - h);
      if (accept_hit(h, hlen, pos, nd, nlen)) return 1;
      size_t step = (size_t)(hit - p) + 1;
      p += step;
      left -= step;
    }
    return 0;
  }

  size_t last = hlen - nlen;
  size_t i = 0;
  uint8x16_t vfirst = vdupq_n_u8(nd[0]);
  const uint8_t bitv[16] = {1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};
  uint8x16_t bits = vld1q_u8(bitv);
  while (i + 16 <= hlen) {
    uint8x16_t v = vld1q_u8(h + i);
    uint8x16_t eq = vceqq_u8(v, vfirst);
    uint8x16_t masked = vandq_u8(eq, bits);
    uint32_t mask = (uint32_t)vaddv_u8(vget_low_u8(masked)) |
                    ((uint32_t)vaddv_u8(vget_high_u8(masked)) << 8);
    while (mask) {
      unsigned b = (unsigned)__builtin_ctz(mask);
      size_t pos = i + b;
      if (pos <= last && accept_hit(h, hlen, pos, nd, nlen)) return 1;
      mask &= mask - 1;
    }
    i += 16;
  }
  for (; i <= last; i++) {
    if (accept_hit(h, hlen, i, nd, nlen)) return 1;
  }
  return 0;
}

static int utf8_span(unsigned char c) {
  if (c < 0x80) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}

static int glob_match(const char *s, size_t n, const char *p, size_t m) {
  size_t si = 0, pi = 0;
  size_t star_p = (size_t)-1, star_s = 0;
  while (si < n) {
    if (pi < m && p[pi] == '?') {
      int k = utf8_span((unsigned char)s[si]);
      if (si + (size_t)k > n) return 0;
      si += (size_t)k;
      pi++;
      continue;
    }
    if (pi < m && p[pi] != '*' && p[pi] == s[si]) {
      si++;
      pi++;
      continue;
    }
    if (pi < m && p[pi] == '*') {
      star_p = pi++;
      star_s = si;
      continue;
    }
    if (star_p != (size_t)-1) {
      pi = star_p + 1;
      si = ++star_s;
      continue;
    }
    return 0;
  }
  while (pi < m && p[pi] == '*') pi++;
  return pi == m;
}

typedef struct Term {
  const char *s;
  uint16_t len;
  uint8_t exclude;
  uint8_t wild;
} Term;

static int term_worse(const Term *a, const Term *b) {
  /* Sort positives first, longest first, so mismatches fail early. */
  if (a->exclude != b->exclude) return a->exclude - b->exclude;
  return (int)b->len - (int)a->len;
}

static int starts_with_term(const char *s, size_t sl, const char *nd, size_t nl) {
  if (nl == 0 || nl > sl) return 0;
  return accept_hit((const unsigned char *)s, sl, 0, (const unsigned char *)nd, nl);
}

typedef struct NameRef {
  uint64_t p0;
  uint64_t p1;
  uint32_t id;
  uint32_t off;
  uint16_t len;
} NameRef;

typedef struct SortPack {
  const char *arena;
} SortPack;

static uint64_t prefix_at(const char *s, uint16_t len, uint16_t skip) {
  unsigned char b[8] = {0};
  if (len > skip) {
    uint16_t n = (uint16_t)(len - skip);
    if (n > 8) n = 8;
    memcpy(b, s + skip, n);
  }
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = (v << 8) | b[i];
  return v;
}

static void counting_pass(NameRef *src, NameRef *dst, uint32_t n, int which, int shift) {
  uint32_t count[256] = {0};
  for (uint32_t i = 0; i < n; i++) {
    uint64_t v = which ? src[i].p1 : src[i].p0;
    count[(v >> shift) & 255]++;
  }
  uint32_t sum = 0;
  for (int i = 0; i < 256; i++) {
    uint32_t c = count[i];
    count[i] = sum;
    sum += c;
  }
  for (uint32_t i = 0; i < n; i++) {
    uint64_t v = which ? src[i].p1 : src[i].p0;
    dst[count[(v >> shift) & 255]++] = src[i];
  }
}

static int cmp_name_ref(void *thunk, const void *a, const void *b);

static void sort_name_refs(NameRef *refs, uint32_t n, const char *arena) {
  NameRef *tmp = n ? malloc((size_t)n * sizeof(NameRef)) : NULL;
  if (!tmp) {
    SortPack pack = {arena};
    qsort_r(refs, n, sizeof(NameRef), &pack, cmp_name_ref);
    return;
  }
  NameRef *src = refs;
  NameRef *dst = tmp;
  for (int which = 1; which >= 0; which--) {
    for (int shift = 0; shift < 64; shift += 8) {
      counting_pass(src, dst, n, which, shift);
      NameRef *swap = src;
      src = dst;
      dst = swap;
    }
  }
  if (src != refs) memcpy(refs, src, (size_t)n * sizeof(NameRef));
  free(tmp);
  SortPack pack = {arena};
  uint32_t start = 0;
  while (start < n) {
    uint32_t end = start + 1;
    while (end < n && refs[end].p0 == refs[start].p0 && refs[end].p1 == refs[start].p1) end++;
    if (end - start > 1) {
      int tail = 0;
      for (uint32_t i = start; i < end; i++) {
        if (refs[i].len > 16) {
          tail = 1;
          break;
        }
      }
      if (tail) qsort_r(refs + start, end - start, sizeof(NameRef), &pack, cmp_name_ref);
    }
    start = end;
  }
}

static int cmp_name_ref(void *thunk, const void *a, const void *b) {
  const SortPack *pack = thunk;
  const NameRef *ra = a;
  const NameRef *rb = b;
  if (ra->p0 != rb->p0) return ra->p0 < rb->p0 ? -1 : 1;
  if (ra->p1 != rb->p1) return ra->p1 < rb->p1 ? -1 : 1;
  if (ra->len > 16 || rb->len > 16) {
    uint16_t la = ra->len > 16 ? (uint16_t)(ra->len - 16) : 0;
    uint16_t lb = rb->len > 16 ? (uint16_t)(rb->len - 16) : 0;
    size_t n = la < lb ? la : lb;
    int c = n ? memcmp(pack->arena + ra->off + 16, pack->arena + rb->off + 16, n) : 0;
    if (c != 0) return c;
    if (la != lb) return la < lb ? -1 : 1;
  }
  if (ra->len != rb->len) return ra->len < rb->len ? -1 : 1;
  return ra->id < rb->id ? -1 : ra->id > rb->id;
}

int me_index_list(MEIndex *idx, uint32_t *out, uint32_t cap, uint32_t *out_count, int sort_by_name) {
  if (!idx || !out_count) return -1;
  if (cap && !out) return -1;
  pthread_rwlock_rdlock(&idx->lock);
  uint32_t live = 0;
  size_t bytes = 0;
  for (uint32_t i = 0; i < idx->count; i++) {
    if (idx->keys[i].flags & ME_F_DEAD) continue;
    live++;
    bytes += idx->keys[i].len;
  }
  uint32_t filled = live < cap ? live : cap;
  if (!sort_by_name || filled < 2) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < idx->count && n < filled; i++) {
      if (idx->keys[i].flags & ME_F_DEAD) continue;
      out[n++] = i;
    }
    pthread_rwlock_unlock(&idx->lock);
    *out_count = filled;
    return 0;
  }
  NameRef *refs = calloc(live, sizeof(NameRef));
  char *arena = bytes ? malloc(bytes) : NULL;
  if (!refs || (bytes && !arena)) {
    free(refs);
    free(arena);
    pthread_rwlock_unlock(&idx->lock);
    return -1;
  }
  size_t cursor = 0;
  uint32_t n = 0;
  for (uint32_t i = 0; i < idx->count; i++) {
    if (idx->keys[i].flags & ME_F_DEAD) continue;
    uint16_t len = idx->keys[i].len;
    const char *name = idx->blob + idx->keys[i].off;
    refs[n].id = i;
    refs[n].off = (uint32_t)cursor;
    refs[n].len = len;
    refs[n].p0 = prefix_at(name, len, 0);
    refs[n].p1 = prefix_at(name, len, 8);
    if (len) memcpy(arena + cursor, idx->blob + idx->keys[i].off, len);
    cursor += len;
    n++;
  }
  pthread_rwlock_unlock(&idx->lock);
  sort_name_refs(refs, n, arena);
  uint32_t keep = n < cap ? n : cap;
  for (uint32_t i = 0; i < keep; i++) out[i] = refs[i].id;
  free(refs);
  free(arena);
  *out_count = keep;
  return 0;
}

/* 解析查询词，与搜索语义一致：空格分词，!前缀排除，* ? 通配。storage 由调用方提供。 */
static int parse_terms(const char *query, char storage[8][1024], Term terms[8]) {
  int nterms = 0;
  const char *q = query ? query : "";
  while (*q && nterms < 8) {
    while (*q == ' ' || *q == '\t') q++;
    if (!*q) break;
    const char *start = q;
    while (*q && *q != ' ' && *q != '\t') q++;
    size_t raw_len = (size_t)(q - start);
    int exclude = 0;
    if (raw_len > 1 && start[0] == '!') {
      exclude = 1;
      start++;
      raw_len--;
    }
    if (raw_len == 0 || raw_len > 1023) continue;
    const char *folded = NULL;
    size_t fl = 0;
    if (me_fold(start, raw_len, storage[nterms], sizeof storage[nterms], &folded, &fl) != 0)
      continue;
    if (folded != storage[nterms]) {
      if (fl > sizeof storage[nterms]) continue;
      memcpy(storage[nterms], folded, fl);
    }
    terms[nterms].s = storage[nterms];
    terms[nterms].len = (uint16_t)fl;
    terms[nterms].exclude = (uint8_t)exclude;
    terms[nterms].wild = (memchr(storage[nterms], '*', fl) || memchr(storage[nterms], '?', fl)) ? 1 : 0;
    nterms++;
  }
  for (int i = 1; i < nterms; i++) {
    Term key = terms[i];
    int j = i;
    while (j > 0 && term_worse(&terms[j - 1], &key) > 0) {
      terms[j] = terms[j - 1];
      j--;
    }
    terms[j] = key;
  }
  return nterms;
}

int me_index_search_range(MEIndex *idx, const char *query, uint32_t offset, uint32_t limit,
                          uint32_t *out, uint32_t *out_count, uint32_t *out_total, double *seconds) {
  if (!idx || !out_count || !out_total) return -1;
  double t0 = me_now();
  *out_count = 0;
  *out_total = 0;
  if (limit && !out) return -1;
  if (limit && offset > UINT32_MAX - limit) return -1;
  uint32_t need = offset + limit;

  char storage[8][1024];
  Term terms[8];
  int nterms = parse_terms(query, storage, terms);

  int prefix_i = -1;
  for (int t = 0; t < nterms; t++) {
    if (!terms[t].exclude && !terms[t].wild && terms[t].len > 0) {
      prefix_i = t;
      break;
    }
  }

  pthread_rwlock_rdlock(&idx->lock);
  const MEKey *keys = idx->keys;
  const char *blob = idx->blob;
  uint32_t count = idx->count;
  uint32_t filled = 0;
  uint32_t total = 0;
  uint32_t np = 0, nr = 0;
  uint32_t stack_pref[256], stack_rest[256];
  uint32_t *pref = stack_pref;
  uint32_t *rest = stack_rest;
  uint32_t *heap = NULL;
  int ranked = prefix_i >= 0 && need > 0;
  if (ranked && need > 256) {
    heap = malloc((size_t)need * 2 * sizeof(uint32_t));
    if (!heap) ranked = 0;
    else {
      pref = heap;
      rest = heap + need;
    }
  }
#define TAKE(id, is_prefix)                                                                          \
  do {                                                                                               \
    if (!ranked) {                                                                                   \
      if (filled < need) {                                                                           \
        if (filled >= offset) out[filled - offset] = (id);                                           \
        filled++;                                                                                    \
      }                                                                                              \
    } else if (is_prefix) {                                                                          \
      if (np < need) pref[np++] = (id);                                                              \
    } else if (nr < need) {                                                                          \
      rest[nr++] = (id);                                                                             \
    }                                                                                                \
    total++;                                                                                         \
  } while (0)

  if (nterms == 0) {
    uint32_t live = idx->live;
    for (uint32_t i = 0; i < count && filled < need; i++) {
      if (keys[i].flags & ME_F_DEAD) continue;
      if (filled >= offset) out[filled - offset] = i;
      filled++;
    }
    total = live;
  } else if (nterms == 1 && !terms[0].exclude && !terms[0].wild && terms[0].len == 1) {
    unsigned char c = (unsigned char)terms[0].s[0];
    for (uint32_t i = 0; i < count; i++) {
      if (keys[i].flags & ME_F_DEAD) continue;
      if (!memchr(blob + keys[i].off, c, keys[i].len)) continue;
      const char *s = blob + keys[i].off;
      size_t sl = keys[i].len;
      int prefix = ranked && starts_with_term(s, sl, terms[0].s, 1);
      TAKE(i, prefix);
    }
  } else {
    for (uint32_t i = 0; i < count; i++) {
      if (keys[i].flags & ME_F_DEAD) continue;
      const char *s = blob + keys[i].off;
      size_t sl = keys[i].len;
      int ok = 1;
      for (int t = 0; t < nterms; t++) {
        int hit = terms[t].wild ? glob_match(s, sl, terms[t].s, terms[t].len)
                                : me_contains(s, sl, terms[t].s, terms[t].len);
        if (terms[t].exclude) {
          if (hit) {
            ok = 0;
            break;
          }
        } else if (!hit) {
          ok = 0;
          break;
        }
      }
      if (!ok) continue;
      int prefix = ranked && starts_with_term(s, sl, terms[prefix_i].s, terms[prefix_i].len);
      TAKE(i, prefix);
    }
  }
#undef TAKE

  if (ranked) {
    uint32_t seen = 0;
    filled = 0;
    for (uint32_t i = 0; i < np; i++) {
      if (seen >= offset && filled < limit) out[filled++] = pref[i];
      seen++;
    }
    for (uint32_t i = 0; i < nr; i++) {
      if (seen >= offset && filled < limit) out[filled++] = rest[i];
      seen++;
    }
  } else if (filled > offset) {
    filled -= offset;
    if (filled > limit) filled = limit;
  } else {
    filled = 0;
  }
  free(heap);

  pthread_rwlock_unlock(&idx->lock);
  *out_count = filled;
  *out_total = total;
  if (seconds) *seconds = me_now() - t0;
  return 0;
}

int me_index_search(MEIndex *idx, const char *query, uint32_t limit, uint32_t *out,
                    uint32_t *out_count, uint32_t *out_total, double *seconds) {
  return me_index_search_range(idx, query, 0, limit, out, out_count, out_total, seconds);
}

/* 排序期间持有读锁，比较器可以直接读取 blob/keys。 */
static const MEIndex *g_sort_idx;

typedef struct MatchRef {
  uint32_t id;
  uint64_t size;
  int64_t mtime;
  uint32_t flags;
  char *path; /* 仅路径排序时使用 */
} MatchRef;

static int cmp_ref_name(void *thunk, const void *a, const void *b) {
  (void)thunk;
  const MatchRef *ra = a, *rb = b;
  const MEIndex *idx = g_sort_idx;
  const MEKey *ka = &idx->keys[ra->id], *kb = &idx->keys[rb->id];
  size_t n = ka->len < kb->len ? ka->len : kb->len;
  int c = n ? memcmp(idx->blob + ka->off, idx->blob + kb->off, n) : 0;
  if (c != 0) return c;
  if (ka->len != kb->len) return ka->len < kb->len ? -1 : 1;
  return ra->id < rb->id ? -1 : ra->id > rb->id;
}

static int cmp_ref_path(void *thunk, const void *a, const void *b) {
  (void)thunk;
  const MatchRef *ra = a, *rb = b;
  int c = strcmp(ra->path, rb->path);
  if (c != 0) return c;
  return ra->id < rb->id ? -1 : ra->id > rb->id;
}

static int cmp_ref_size(void *thunk, const void *a, const void *b) {
  const MatchRef *ra = a, *rb = b;
  /* 文件夹始终排在文件后面（升序、降序都一样）。 */
  int da = (ra->flags & ME_F_DIR) ? 1 : 0;
  int db = (rb->flags & ME_F_DIR) ? 1 : 0;
  if (da != db) return da - db;
  if (ra->size != rb->size) {
    int asc = *(const int *)thunk;
    return asc ? (ra->size < rb->size ? -1 : 1) : (ra->size > rb->size ? -1 : 1);
  }
  return ra->id < rb->id ? -1 : ra->id > rb->id;
}

static int cmp_ref_mtime(void *thunk, const void *a, const void *b) {
  const MatchRef *ra = a, *rb = b;
  if (ra->mtime != rb->mtime) {
    int asc = *(const int *)thunk;
    return asc ? (ra->mtime < rb->mtime ? -1 : 1) : (ra->mtime > rb->mtime ? -1 : 1);
  }
  return ra->id < rb->id ? -1 : ra->id > rb->id;
}

int me_index_search_sorted(MEIndex *idx, const char *query, int key, int ascending,
                           uint32_t **out_ids, uint32_t *out_count, double *seconds) {
  if (!idx || !out_ids || !out_count) return -1;
  double t0 = me_now();
  *out_ids = NULL;
  *out_count = 0;

  char storage[8][1024];
  Term terms[8];
  int nterms = parse_terms(query, storage, terms);

  pthread_rwlock_rdlock(&idx->lock);
  const MEKey *keys = idx->keys;
  const char *blob = idx->blob;
  uint32_t count = idx->count;

  uint32_t cap = 1024, n = 0;
  MatchRef *refs = malloc(cap * sizeof(MatchRef));
  if (!refs) {
    pthread_rwlock_unlock(&idx->lock);
    me_set_error("out of memory");
    return -1;
  }
  for (uint32_t i = 0; i < count; i++) {
    if (keys[i].flags & ME_F_DEAD) continue;
    const char *s = blob + keys[i].off;
    size_t sl = keys[i].len;
    int ok = 1;
    for (int t = 0; t < nterms; t++) {
      int hit = terms[t].wild ? glob_match(s, sl, terms[t].s, terms[t].len)
                              : me_contains(s, sl, terms[t].s, terms[t].len);
      if (terms[t].exclude ? hit : !hit) {
        ok = 0;
        break;
      }
    }
    if (!ok) continue;
    if (n == cap) {
      uint32_t ncap = cap * 2;
      MatchRef *nr = realloc(refs, (size_t)ncap * sizeof(MatchRef));
      if (!nr) break;
      refs = nr;
      cap = ncap;
    }
    refs[n].id = i;
    refs[n].size = idx->nodes[i].size;
    refs[n].mtime = idx->nodes[i].mtime;
    refs[n].flags = keys[i].flags;
    refs[n].path = NULL;
    n++;
  }

  int asc = ascending ? 1 : 0;
  if (key == ME_SORT_PATH) {
    for (uint32_t i = 0; i < n; i++) {
      char p[4096];
      refs[i].path = me_build_path_ul(idx, refs[i].id, p, sizeof p) == 0 ? strdup(p) : NULL;
    }
  }
  g_sort_idx = idx;
  if (key == ME_SORT_NAME) {
    qsort_r(refs, n, sizeof(MatchRef), &asc, cmp_ref_name);
  } else if (key == ME_SORT_PATH) {
    qsort_r(refs, n, sizeof(MatchRef), &asc, cmp_ref_path);
  } else if (key == ME_SORT_SIZE) {
    qsort_r(refs, n, sizeof(MatchRef), &asc, cmp_ref_size);
  } else if (key == ME_SORT_MTIME) {
    qsort_r(refs, n, sizeof(MatchRef), &asc, cmp_ref_mtime);
  }
  if (!asc && (key == ME_SORT_NAME || key == ME_SORT_PATH)) {
    for (uint32_t i = 0; i < n / 2; i++) {
      MatchRef tmp = refs[i];
      refs[i] = refs[n - 1 - i];
      refs[n - 1 - i] = tmp;
    }
  }

  uint32_t *ids = malloc((size_t)(n ? n : 1) * sizeof(uint32_t));
  if (!ids) {
    for (uint32_t i = 0; i < n; i++) free(refs[i].path);
    free(refs);
    pthread_rwlock_unlock(&idx->lock);
    me_set_error("out of memory");
    return -1;
  }
  for (uint32_t i = 0; i < n; i++) ids[i] = refs[i].id;
  for (uint32_t i = 0; i < n; i++) free(refs[i].path);
  free(refs);
  pthread_rwlock_unlock(&idx->lock);

  *out_ids = ids;
  *out_count = n;
  if (seconds) *seconds = me_now() - t0;
  return 0;
}
