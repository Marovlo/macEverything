#define _DARWIN_C_SOURCE 1
#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failed = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
      g_failed++;                                                                   \
    }                                                                               \
  } while (0)

static int naive_contains(const char *hay, size_t hlen, const char *needle, size_t nlen) {
  if (nlen == 0) return 1;
  if (nlen > hlen) return 0;
  for (size_t i = 0; i + nlen <= hlen; i++) {
    if (memcmp(hay + i, needle, nlen) == 0) return 1;
  }
  return 0;
}

static void test_contains(void) {
  CHECK(me_contains("abc", 3, "a", 1) == 1);
  CHECK(me_contains("abc", 3, "c", 1) == 1);
  CHECK(me_contains("abc", 3, "z", 1) == 0);
  CHECK(me_contains("abcdef", 6, "cde", 3) == 1);
  CHECK(me_contains("abcdef", 6, "xyz", 3) == 0);
  CHECK(me_contains("", 0, "a", 1) == 0);
  CHECK(me_contains("a", 1, "", 0) == 1);
  char hay[64];
  char needle[8];
  for (int t = 0; t < 3000; t++) {
    int n = rand() % 48;
    int m = 1 + rand() % 6;
    for (int i = 0; i < n; i++) hay[i] = (char)('a' + rand() % 26);
    for (int i = 0; i < m; i++) needle[i] = (char)('a' + rand() % 26);
    if (n >= m && (t % 3) == 0) memcpy(hay + (n - m), needle, (size_t)m);
    int got = me_contains(hay, (size_t)n, needle, (size_t)m);
    int exp = naive_contains(hay, (size_t)n, needle, (size_t)m);
    if (got != exp) {
      fprintf(stderr, "FAIL contains t=%d n=%d m=%d got=%d exp=%d\n", t, n, m, got, exp);
      g_failed++;
      break;
    }
  }
}

static uint32_t total_for(MEIndex *idx, const char *query) {
  uint32_t count = 0, total = 0;
  uint32_t id = 0;
  me_index_search(idx, query, 1, &id, &count, &total, NULL);
  return total;
}

static void test_query_semantics(void) {
  MEIndex *idx = me_index_create();
  uint32_t root = me_index_add_root(idx, "/tmp/bench");
  CHECK(root != ME_NONE);
  CHECK(me_index_add_child(idx, root, "README.md", 1, 10, 100, 0) != ME_NONE);
  CHECK(me_index_add_child(idx, root, "readme-notes.txt", 2, 3, 100, 0) != ME_NONE);
  CHECK(me_index_add_child(idx, root, "Café.txt", 3, 4, 100, 0) != ME_NONE);
  CHECK(me_index_add_child(idx, root, "文档.txt", 4, 5, 100, 0) != ME_NONE);
  CHECK(me_index_add_child(idx, root, "Old.log", 5, 6, 100, 0) != ME_NONE);
  CHECK(total_for(idx, "readme") == 2);
  CHECK(total_for(idx, "README") == 2);
  CHECK(total_for(idx, "readme md") == 1);
  CHECK(total_for(idx, "txt !notes") == 2);
  CHECK(total_for(idx, "*.log") == 1);
  CHECK(total_for(idx, "文档") == 1);
  CHECK(total_for(idx, "café") == 1);
  CHECK(total_for(idx, "cafe") == 0);
  CHECK(total_for(idx, "") == me_index_live(idx));

  MEInfo info;
  uint32_t ids[8];
  uint32_t count = 0, total = 0;
  me_index_search(idx, "README.md", 8, ids, &count, &total, NULL);
  CHECK(total == 1 && count == 1);
  me_index_info(idx, ids[0], &info);
  CHECK(strcmp(info.name, "README.md") == 0);
  CHECK(strcmp(info.path, "/tmp/bench/README.md") == 0);

  char path[] = "/tmp/me-sem-XXXXXX";
  CHECK(mkdtemp(path) != NULL);
  char file[1100];
  snprintf(file, sizeof file, "%s/gone.txt", path);
  FILE *f = fopen(file, "w");
  CHECK(f != NULL);
  fputs("z", f);
  fclose(f);
  MEIndex *real = me_index_create();
  CHECK(me_index_scan(real, path, NULL) == 0);
  CHECK(total_for(real, "gone.txt") == 1);
  unlink(file);
  CHECK(me_index_refresh_path(real, file) == 0);
  CHECK(total_for(real, "gone.txt") == 0);
  CHECK(!me_index_is_mapped(real));
  me_index_free(real);
  rmdir(path);
  me_index_free(idx);
}

static void write_file(const char *path, const char *text) {
  FILE *f = fopen(path, "w");
  if (!f) {
    fprintf(stderr, "fopen %s: %s\n", path, strerror(errno));
    g_failed++;
    return;
  }
  fputs(text, f);
  fclose(f);
}

static void test_scan_tree(void) {
  char root[] = "/tmp/me-scan-XXXXXX";
  CHECK(mkdtemp(root) != NULL);
  char buf[1200];
  snprintf(buf, sizeof buf, "%s/a.txt", root);
  write_file(buf, "a");
  snprintf(buf, sizeof buf, "%s/README.md", root);
  write_file(buf, "r");
  snprintf(buf, sizeof buf, "%s/Library", root);
  mkdir(buf, 0755);
  snprintf(buf, sizeof buf, "%s/Library/Caches", root);
  mkdir(buf, 0755);
  snprintf(buf, sizeof buf, "%s/Library/Caches/nope.txt", root);
  write_file(buf, "n");
  snprintf(buf, sizeof buf, "%s/Library/Docs", root);
  mkdir(buf, 0755);
  snprintf(buf, sizeof buf, "%s/Library/Docs/yes.txt", root);
  write_file(buf, "y");
  snprintf(buf, sizeof buf, "%s/Caches", root);
  mkdir(buf, 0755);
  snprintf(buf, sizeof buf, "%s/Caches/yes2.txt", root);
  write_file(buf, "y");
  snprintf(buf, sizeof buf, "%s/sub", root);
  mkdir(buf, 0755);
  snprintf(buf, sizeof buf, "%s/sub/inside.txt", root);
  write_file(buf, "i");
  snprintf(buf, sizeof buf, "%s/link", root);
  CHECK(symlink("sub", buf) == 0);
  snprintf(buf, sizeof buf, "%s/uni", root);
  mkdir(buf, 0755);
  snprintf(buf, sizeof buf, "%s/uni/文档.txt", root);
  write_file(buf, "u");
  snprintf(buf, sizeof buf, "%s/uni/Café.txt", root);
  write_file(buf, "c");
  snprintf(buf, sizeof buf, "%s/.fseventsd", root);
  mkdir(buf, 0755);
  snprintf(buf, sizeof buf, "%s/.fseventsd/x", root);
  write_file(buf, "x");
  for (int d = 0; d < 40; d++) {
    snprintf(buf, sizeof buf, "%s/d%02d", root, d);
    mkdir(buf, 0755);
    for (int f = 0; f < 20; f++) {
      char file[1200];
      snprintf(file, sizeof file, "%s/d%02d/f%02d.txt", root, d, f);
      write_file(file, "z");
    }
  }

  MEIndex *idx = me_index_create();
  MEScanStats stats;
  CHECK(me_index_scan(idx, root, &stats) == 0);
  fprintf(stderr, "scan nodes=%llu files=%llu dirs=%llu seconds=%.3f\n",
          (unsigned long long)stats.nodes, (unsigned long long)stats.files,
          (unsigned long long)stats.dirs, stats.seconds);
  CHECK(total_for(idx, "nope.txt") == 0);
  CHECK(total_for(idx, "yes.txt") == 1);
  CHECK(total_for(idx, "yes2.txt") == 1);
  CHECK(total_for(idx, "inside.txt") == 1);
  CHECK(total_for(idx, "readme") == 1);
  CHECK(total_for(idx, "文档") == 1);
  CHECK(total_for(idx, "café") == 1);
  CHECK(total_for(idx, "f07.txt") == 40);
  CHECK(total_for(idx, "fseventsd") == 0);

  uint32_t ids[4];
  uint32_t count = 0, total = 0;
  me_index_search(idx, "link", 4, ids, &count, &total, NULL);
  int saw_link = 0;
  MEInfo infos[4];
  me_index_infos(idx, ids, count, infos);
  for (uint32_t i = 0; i < count; i++) {
    if (strcmp(infos[i].name, "link") == 0) {
      saw_link = 1;
      CHECK(infos[i].is_link == 1);
      CHECK(infos[i].is_dir == 0);
    }
  }
  CHECK(saw_link == 1);

  char index_path[] = "/tmp/me-scan-index.bin";
  CHECK(me_index_save(idx, index_path) == 0);
  uint32_t live = me_index_live(idx);
  me_index_free(idx);
  MEIndex *loaded = me_index_load(index_path);
  CHECK(loaded != NULL);
  CHECK(me_index_is_mapped(loaded));
  CHECK(me_index_live(loaded) == live);
  CHECK(me_index_complete(loaded) == 0);
  me_index_set_complete(loaded, 1);
  CHECK(me_index_save(loaded, index_path) == 0);
  me_index_free(loaded);
  loaded = me_index_load(index_path);
  CHECK(loaded != NULL);
  CHECK(me_index_complete(loaded) == 1);
  CHECK(total_for(loaded, "f07.txt") == 40);
  char nested[] = "/tmp/me-nested-XXXXXX";
  CHECK(mkdtemp(nested) != NULL);
  char nested_index[1200];
  snprintf(nested_index, sizeof nested_index, "%s/MacEverything/index.bin", nested);
  me_index_set_complete(loaded, 0);
  me_index_set_complete(loaded, 1);
  CHECK(me_index_save(loaded, nested_index) == 0);
  MEIndex *nested_loaded = me_index_load(nested_index);
  CHECK(nested_loaded != NULL);
  CHECK(me_index_complete(nested_loaded) == 1);
  me_index_free(nested_loaded);
  char rm[1400];
  snprintf(rm, sizeof rm, "rm -rf '%s'", nested);
  CHECK(system(rm) == 0);
  CHECK(total_for(loaded, "nope.txt") == 0);
  me_index_free(loaded);

  char cmd[1400];
  snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
  CHECK(system(cmd) == 0);
  unlink(index_path);
  unlink("/tmp/me-scan-index.bin.eid");
}

static void test_perf(void) {
  const uint32_t N = 1000000;
  MEIndex *idx = me_index_create();
  uint32_t root = me_index_add_root(idx, "/tmp/perf");
  CHECK(root != ME_NONE);
  double t0 = me_now();
  for (uint32_t i = 0; i < N; i++) {
    char name[32];
    snprintf(name, sizeof name, "file-%07u.txt", i);
    if (me_index_add_child(idx, root, name, i, 16, 1700000000, 0) == ME_NONE) {
      CHECK(0);
      break;
    }
  }
  CHECK(me_index_add_child(idx, root, "README.md", N + 1, 8, 1700000000, 0) != ME_NONE);
  double build = me_now() - t0;
  uint64_t bytes = me_index_storage_bytes(idx);
  double per = (double)bytes / (double)me_index_live(idx);
  fprintf(stderr, "build %.3fs bytes/node %.1f live %u\n", build, per, me_index_live(idx));
  CHECK(per < 200.0);

  double sec = 0;
  uint32_t ids[32];
  uint32_t count = 0, total = 0;
  me_index_search(idx, "qqqq", 32, ids, &count, &total, &sec);
  fprintf(stderr, "hot miss %.3f ms total %u\n", sec * 1000.0, total);
  CHECK(total == 0);
  CHECK(sec < 0.05);
  me_index_search(idx, "e", 32, ids, &count, &total, &sec);
  fprintf(stderr, "hot 1-char %.3f ms total %u\n", sec * 1000.0, total);
  CHECK(total >= N);
  CHECK(sec < 0.05);
  me_index_search(idx, "readme", 32, ids, &count, &total, &sec);
  CHECK(total == 1);
  CHECK(sec < 0.05);

  char path[] = "/tmp/me-perf-index.bin";
  CHECK(me_index_save(idx, path) == 0);
  me_index_free(idx);

  double l0 = me_now();
  MEIndex *loaded = me_index_load(path);
  double load = me_now() - l0;
  CHECK(loaded != NULL);
  CHECK(me_index_is_mapped(loaded));
  fprintf(stderr, "cold load %.3f ms\n", load * 1000.0);
  CHECK(load < 0.05);
  me_index_search(loaded, "qqqq", 32, ids, &count, &total, &sec);
  fprintf(stderr, "cold query %.3f ms\n", sec * 1000.0);
  CHECK(sec < 0.08);
  double sec2 = 0;
  me_index_search(loaded, "qqqq", 32, ids, &count, &total, &sec2);
  fprintf(stderr, "warm query %.3f ms\n", sec2 * 1000.0);
  CHECK(sec2 < 0.03);
  CHECK(me_index_is_mapped(loaded));
  me_index_free(loaded);
  unlink(path);
  unlink("/tmp/me-perf-index.bin.eid");
}

static int wait_total(MEIndex *idx, const char *query, uint32_t expect, int ms) {
  for (int i = 0; i < ms / 50; i++) {
    if (total_for(idx, query) == expect) return 1;
    usleep(50 * 1000);
  }
  return 0;
}

static void test_watch(void) {
  char root[] = "/tmp/me-watch-XXXXXX";
  CHECK(mkdtemp(root) != NULL);
  MEIndex *idx = me_index_create();
  CHECK(me_index_scan(idx, root, NULL) == 0);
  CHECK(me_index_watch_start(idx, NULL, NULL) == 0);
  char file[1100];
  snprintf(file, sizeof file, "%s/me-watch-token.txt", root);
  write_file(file, "hello");
  CHECK(wait_total(idx, "me-watch-token", 1, 4000));
  MEInfo info;
  uint32_t id = 0, count = 0, total = 0;
  me_index_search(idx, "me-watch-token", 1, &id, &count, &total, NULL);
  me_index_info(idx, id, &info);
  CHECK(info.size == 5);
  unlink(file);
  CHECK(wait_total(idx, "me-watch-token", 0, 4000));
  if (g_failed) fprintf(stderr, "watch error: %s\n", me_last_error());
  me_index_free(idx);
  rmdir(root);
}

static void *search_loop(void *arg) {
  MEIndex *idx = arg;
  uint32_t ids[8];
  for (int i = 0; i < 200; i++) {
    uint32_t count = 0, total = 0;
    me_index_search(idx, "a", 8, ids, &count, &total, NULL);
  }
  return NULL;
}

static void test_concurrent_search(void) {
  char root[] = "/tmp/me-race-XXXXXX";
  CHECK(mkdtemp(root) != NULL);
  MEIndex *idx = me_index_create();
  CHECK(me_index_scan(idx, root, NULL) == 0);
  CHECK(me_index_watch_start(idx, NULL, NULL) == 0);
  pthread_t th[2];
  pthread_create(&th[0], NULL, search_loop, idx);
  pthread_create(&th[1], NULL, search_loop, idx);
  for (int i = 0; i < 30; i++) {
    char file[1100];
    snprintf(file, sizeof file, "%s/race-%d.txt", root, i);
    write_file(file, "a");
    usleep(20 * 1000);
  }
  pthread_join(th[0], NULL);
  pthread_join(th[1], NULL);
  me_index_free(idx);
  char cmd[1400];
  snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
  CHECK(system(cmd) == 0);
}

static void test_search_pagination(void) {
  MEIndex *idx = me_index_create();
  uint32_t root = me_index_add_root(idx, "/tmp/bench");
  CHECK(root != ME_NONE);
  const uint32_t N = 1000;
  for (uint32_t i = 0; i < N; i++) {
    char name[32];
    snprintf(name, sizeof name, "page-%05u.log", i);
    CHECK(me_index_add_child(idx, root, name, i, 1, 1, 0) != ME_NONE);
  }
  /* 分页拼起来必须等于全量顺序，total 各页一致。 */
  uint32_t seen = 0;
  uint32_t expect_total = 0;
  {
    uint32_t id = 0, count = 0, total = 0;
    me_index_search(idx, "page-", 1, &id, &count, &total, NULL);
    expect_total = total;
  }
  CHECK(expect_total == N);
  while (seen < N) {
    uint32_t ids[300];
    uint32_t count = 0, total = 0;
    CHECK(me_index_search_range(idx, "page-", seen, 300, ids, &count, &total, NULL) == 0);
    CHECK(total == expect_total);
    for (uint32_t i = 0; i < count; i++) {
      char name[32];
      snprintf(name, sizeof name, "page-%05u.log", seen + i);
      MEInfo info;
      CHECK(me_index_info(idx, ids[i], &info) == 0);
      CHECK(strcmp(info.name, name) == 0);
    }
    seen += count;
    if (count == 0) break;
  }
  CHECK(seen == N);
  me_index_free(idx);
}

static void test_search_sorted(void) {
  MEIndex *idx = me_index_create();
  uint32_t root = me_index_add_root(idx, "/tmp/bench");
  CHECK(root != ME_NONE);
  CHECK(me_index_add_child(idx, root, "b.txt", 1, 300, 500, 0) != ME_NONE);
  CHECK(me_index_add_child(idx, root, "a.txt", 2, 100, 900, 0) != ME_NONE);
  CHECK(me_index_add_child(idx, root, "sub", 3, 0, 100, ME_F_DIR) != ME_NONE);
  CHECK(me_index_add_child(idx, root, "c.txt", 4, 200, 700, 0) != ME_NONE);

  uint32_t *ids = NULL;
  uint32_t count = 0;
  CHECK(me_index_search_sorted(idx, ".txt", ME_SORT_NAME, 1, &ids, &count, NULL) == 0);
  CHECK(count == 3);
  MEInfo info;
  me_index_info(idx, ids[0], &info);
  CHECK(strcmp(info.name, "a.txt") == 0);
  me_index_info(idx, ids[1], &info);
  CHECK(strcmp(info.name, "b.txt") == 0);
  me_index_info(idx, ids[2], &info);
  CHECK(strcmp(info.name, "c.txt") == 0);
  free(ids);

  CHECK(me_index_search_sorted(idx, ".txt", ME_SORT_NAME, 0, &ids, &count, NULL) == 0);
  me_index_info(idx, ids[0], &info);
  CHECK(strcmp(info.name, "c.txt") == 0);
  free(ids);

  /* 大小排序：文件夹置底，文件按大小升序。空查询匹配 4 个子项 + 根节点（也是目录）。 */
  CHECK(me_index_search_sorted(idx, "", ME_SORT_SIZE, 1, &ids, &count, NULL) == 0);
  CHECK(count == 5);
  me_index_info(idx, ids[0], &info);
  CHECK(strcmp(info.name, "a.txt") == 0);
  me_index_info(idx, ids[1], &info);
  CHECK(strcmp(info.name, "c.txt") == 0);
  me_index_info(idx, ids[2], &info);
  CHECK(strcmp(info.name, "b.txt") == 0);
  me_index_info(idx, ids[3], &info);
  CHECK(info.is_dir == 1);
  me_index_info(idx, ids[4], &info);
  CHECK(info.is_dir == 1);
  free(ids);
  CHECK(me_index_search_sorted(idx, "", ME_SORT_SIZE, 0, &ids, &count, NULL) == 0);
  me_index_info(idx, ids[0], &info);
  CHECK(strcmp(info.name, "b.txt") == 0);
  me_index_info(idx, ids[4], &info);
  CHECK(info.is_dir == 1);
  free(ids);

  /* 修改时间降序：最新的在前 */
  CHECK(me_index_search_sorted(idx, "", ME_SORT_MTIME, 0, &ids, &count, NULL) == 0);
  me_index_info(idx, ids[0], &info);
  CHECK(strcmp(info.name, "a.txt") == 0);
  free(ids);

  /* 路径排序 */
  CHECK(me_index_search_sorted(idx, ".txt", ME_SORT_PATH, 1, &ids, &count, NULL) == 0);
  me_index_info(idx, ids[0], &info);
  CHECK(strcmp(info.path, "/tmp/bench/a.txt") == 0);
  free(ids);

  me_index_free(idx);
}

static void test_prefix_rank(void) {
  MEIndex *idx = me_index_create();
  uint32_t root = me_index_add_root(idx, "/tmp/bench");
  CHECK(root != ME_NONE);
  CHECK(me_index_add_child(idx, root, "zzzreadme.txt", 1, 1, 1, 0) != ME_NONE);
  CHECK(me_index_add_child(idx, root, "notes-readme.md", 2, 1, 1, 0) != ME_NONE);
  CHECK(me_index_add_child(idx, root, "readme.txt", 3, 1, 1, 0) != ME_NONE);
  CHECK(me_index_add_child(idx, root, "README-old.md", 4, 1, 1, 0) != ME_NONE);
  uint32_t ids[8];
  uint32_t count = 0, total = 0;
  CHECK(me_index_search(idx, "readme", 8, ids, &count, &total, NULL) == 0);
  CHECK(total == 4 && count == 4);
  MEInfo info;
  CHECK(me_index_info(idx, ids[0], &info) == 0);
  CHECK(strcmp(info.name, "readme.txt") == 0);
  CHECK(me_index_info(idx, ids[1], &info) == 0);
  CHECK(strcmp(info.name, "README-old.md") == 0);
  CHECK(me_index_info(idx, ids[2], &info) == 0);
  CHECK(strcmp(info.name, "zzzreadme.txt") == 0);
  me_index_free(idx);
}

static void test_list_sorted(void) {
  MEIndex *idx = me_index_create();
  uint32_t root = me_index_add_root(idx, "/tmp/bench");
  CHECK(me_index_add_child(idx, root, "Readme.md", 1, 1, 1, 0) != ME_NONE);
  CHECK(me_index_add_child(idx, root, "banana.txt", 2, 1, 1, 0) != ME_NONE);
  CHECK(me_index_add_child(idx, root, "Apple.txt", 3, 1, 1, 0) != ME_NONE);
  uint32_t ids[8];
  uint32_t n = 0;
  CHECK(me_index_list(idx, ids, 8, &n, 1) == 0);
  CHECK(n == 4);
  MEInfo info;
  const char *want[] = {"Apple.txt", "banana.txt", "bench", "Readme.md"};
  for (uint32_t i = 0; i < n; i++) {
    CHECK(me_index_info(idx, ids[i], &info) == 0);
    CHECK(strcmp(info.name, want[i]) == 0);
  }
  me_index_free(idx);
}

int main(void) {
  test_contains();
  test_query_semantics();
  test_prefix_rank();
  test_list_sorted();
  test_search_pagination();
  test_search_sorted();
  test_scan_tree();
  test_watch();
  test_concurrent_search();
  test_perf();
  if (g_failed) {
    fprintf(stderr, "%d check(s) failed\n", g_failed);
    return 1;
  }
  fprintf(stderr, "all tests passed\n");
  return 0;
}
