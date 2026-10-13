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

int me_index_search(MEIndex *idx, const char *query, uint32_t limit, uint32_t *out,
                    uint32_t *out_count, uint32_t *out_total, double *seconds) {
  if (!idx || !out_count || !out_total) return -1;
  double t0 = me_now();
  *out_count = 0;
  *out_total = 0;
  if (limit && !out) return -1;

  char storage[8][1024];
  Term terms[8];
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
  int ranked = prefix_i >= 0 && limit > 0;
  if (ranked && limit > 256) {
    heap = malloc((size_t)limit * 2 * sizeof(uint32_t));
    if (!heap) ranked = 0;
    else {
      pref = heap;
      rest = heap + limit;
    }
  }
#define TAKE(id, is_prefix)                                                                          \
  do {                                                                                               \
    if (!ranked) {                                                                                   \
      if (filled < limit) out[filled++] = (id);                                                      \
    } else if (is_prefix) {                                                                          \
      if (np < limit) pref[np++] = (id);                                                             \
    } else if (nr < limit) {                                                                         \
      rest[nr++] = (id);                                                                             \
    }                                                                                                \
    total++;                                                                                         \
  } while (0)

  if (nterms == 0) {
    uint32_t live = idx->live;
    for (uint32_t i = 0; i < count && filled < limit; i++) {
      if (keys[i].flags & ME_F_DEAD) continue;
      out[filled++] = i;
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
    filled = 0;
    for (uint32_t i = 0; i < np && filled < limit; i++) out[filled++] = pref[i];
    for (uint32_t i = 0; i < nr && filled < limit; i++) out[filled++] = rest[i];
  }
  free(heap);

  pthread_rwlock_unlock(&idx->lock);
  *out_count = filled;
  *out_total = total;
  if (seconds) *seconds = me_now() - t0;
  return 0;
}
