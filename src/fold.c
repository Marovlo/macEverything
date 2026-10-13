#define _DARWIN_C_SOURCE 1
#include "internal.h"

#include <CoreFoundation/CoreFoundation.h>
#include <string.h>

int me_fold(const char *src, size_t src_len, char *dst, size_t dst_cap,
            const char **out, size_t *out_len) {
  int ascii = 1;
  int changed = 0;
  for (size_t i = 0; i < src_len; i++) {
    unsigned char c = (unsigned char)src[i];
    if (c >= 128) {
      ascii = 0;
      break;
    }
    if (c >= 'A' && c <= 'Z') changed = 1;
  }
  if (ascii && !changed) {
    *out = src;
    *out_len = src_len;
    return 0;
  }
  if (ascii) {
    if (src_len > dst_cap) return -1;
    for (size_t i = 0; i < src_len; i++) {
      unsigned char c = (unsigned char)src[i];
      if (c >= 'A' && c <= 'Z') c = (unsigned char)(c | 32);
      dst[i] = (char)c;
    }
    *out = dst;
    *out_len = src_len;
    return 0;
  }

  CFStringRef s = CFStringCreateWithBytes(kCFAllocatorDefault, (const UInt8 *)src,
                                           (CFIndex)src_len, kCFStringEncodingUTF8, false);
  if (!s) {
    if (src_len > dst_cap) return -1;
    memcpy(dst, src, src_len);
    *out = dst;
    *out_len = src_len;
    return 0;
  }
  CFMutableStringRef m = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, s);
  CFRelease(s);
  if (!m) return -1;
  CFStringNormalize(m, kCFStringNormalizationFormD);
  CFStringLowercase(m, NULL);
  CFIndex length = CFStringGetLength(m);
  CFIndex used = 0;
  CFIndex converted = CFStringGetBytes(m, CFRangeMake(0, length), kCFStringEncodingUTF8, 0,
                                        false, (UInt8 *)dst, (CFIndex)dst_cap, &used);
  CFRelease(m);
  if (converted < 0 || used < 0) return -1;
  if ((size_t)used == src_len && memcmp(dst, src, src_len) == 0) {
    *out = src;
    *out_len = src_len;
    return 0;
  }
  *out = dst;
  *out_len = (size_t)used;
  return 0;
}

int me_fold_path(const char *src, char *dst, size_t cap, size_t *out_len) {
  size_t n = strlen(src);
  size_t di = 0;
  size_t i = 0;
  while (i < n) {
    if (src[i] == '/') {
      if (di + 1 >= cap) return -1;
      dst[di++] = '/';
      i++;
      continue;
    }
    size_t j = i;
    while (j < n && src[j] != '/') j++;
    char tmp[1024];
    const char *folded = NULL;
    size_t fl = 0;
    if (j - i > sizeof tmp) return -1;
    if (me_fold(src + i, j - i, tmp, sizeof tmp, &folded, &fl) != 0) return -1;
    if (di + fl >= cap) return -1;
    memcpy(dst + di, folded, fl);
    di += fl;
    i = j;
  }
  if (di >= cap) return -1;
  dst[di] = 0;
  *out_len = di;
  return 0;
}
