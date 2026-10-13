#define _DARWIN_C_SOURCE 1
#include "me.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(void) {
  fprintf(stderr,
          "用法:\n"
          "  maceverything scan <目录> [-o 索引文件]\n"
          "  maceverything search [-i 索引文件] [-n 条数] <文件名查询>\n"
          "  maceverything watch <目录>\n"
          "  maceverything gui\n"
          "\n"
          "查询是不区分大小写的文件名子串，空格表示同时包含。\n"
          "!词 排除，* 和 ? 是通配符。\n");
}

static int default_or(const char *given, char *buf, size_t cap) {
  if (given) {
    snprintf(buf, cap, "%s", given);
    return 0;
  }
  return me_default_index_path(buf, cap);
}

static int cmd_scan(int argc, char **argv) {
  const char *root = NULL;
  const char *out = NULL;
  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) out = argv[++i];
    else if (!root) root = argv[i];
    else {
      usage();
      return 2;
    }
  }
  if (!root) {
    usage();
    return 2;
  }
  char path[1024];
  if (default_or(out, path, sizeof path) != 0) return 1;
  MEIndex *idx = me_index_create();
  if (!idx) return 1;
  MEScanStats stats;
  if (me_index_scan(idx, root, &stats) != 0) {
    fprintf(stderr, "扫描失败: %s\n", me_last_error());
    me_index_free(idx);
    return 1;
  }
  me_index_set_complete(idx, 1);
  if (me_index_save(idx, path) != 0) {
    fprintf(stderr, "保存失败: %s\n", me_last_error());
    me_index_free(idx);
    return 1;
  }
  printf("nodes\t%llu\nfiles\t%llu\ndirs\t%llu\nseconds\t%.3f\nindex\t%s\n",
         (unsigned long long)stats.nodes, (unsigned long long)stats.files,
         (unsigned long long)stats.dirs, stats.seconds, path);
  me_index_free(idx);
  return 0;
}

static int cmd_search(int argc, char **argv) {
  const char *index = NULL;
  const char *query = NULL;
  uint32_t limit = 50;
  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) index = argv[++i];
    else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) limit = (uint32_t)atoi(argv[++i]);
    else if (!query) query = argv[i];
    else {
      usage();
      return 2;
    }
  }
  if (!query) query = "";
  char path[1024];
  if (default_or(index, path, sizeof path) != 0) return 1;
  MEIndex *idx = me_index_load(path);
  if (!idx) {
    fprintf(stderr, "无法读取索引: %s\n", me_last_error());
    return 1;
  }
  uint32_t *ids = limit ? calloc(limit, sizeof(uint32_t)) : NULL;
  uint32_t count = 0, total = 0;
  double seconds = 0;
  if (me_index_search(idx, query, limit, ids, &count, &total, &seconds) != 0) {
    me_index_free(idx);
    free(ids);
    return 1;
  }
  MEInfo *infos = count ? calloc(count, sizeof(MEInfo)) : NULL;
  if (count) me_index_infos(idx, ids, count, infos);
  for (uint32_t i = 0; i < count; i++) {
    printf("%s\t%s\t%llu\t%lld\n", infos[i].name, infos[i].path,
           (unsigned long long)infos[i].size, (long long)infos[i].mtime);
  }
  fprintf(stderr, "total\t%u\nms\t%.3f\nlive\t%u\nmapped\t%d\n", total, seconds * 1000.0,
          me_index_live(idx), me_index_is_mapped(idx));
  free(infos);
  free(ids);
  me_index_free(idx);
  return 0;
}

static void watch_changed(void *ctx) {
  uint64_t *gen = ctx;
  *gen = *gen + 1;
  fprintf(stderr, "change\t%llu\n", (unsigned long long)*gen);
}

static int cmd_watch(int argc, char **argv) {
  if (argc < 1) {
    usage();
    return 2;
  }
  MEIndex *idx = me_index_create();
  MEScanStats stats;
  if (me_index_scan(idx, argv[0], &stats) != 0) {
    fprintf(stderr, "扫描失败: %s\n", me_last_error());
    me_index_free(idx);
    return 1;
  }
  printf("watching\t%s\nnodes\t%llu\n", argv[0], (unsigned long long)stats.nodes);
  fflush(stdout);
  uint64_t gen = 0;
  if (me_index_watch_start(idx, watch_changed, &gen) != 0) {
    fprintf(stderr, "监视失败: %s\n", me_last_error());
    me_index_free(idx);
    return 1;
  }
  for (;;) pause();
}

int main(int argc, char **argv) {
  if (argc < 2) {
    if (strstr(argv[0], ".app/")) return me_app_main(argc, (const char **)argv);
    usage();
    return 2;
  }
  if (strcmp(argv[1], "gui") == 0 || strncmp(argv[1], "-psn_", 5) == 0) {
    return me_app_main(argc, (const char **)argv);
  }
  if (strcmp(argv[1], "scan") == 0) return cmd_scan(argc - 2, argv + 2);
  if (strcmp(argv[1], "search") == 0) return cmd_search(argc - 2, argv + 2);
  if (strcmp(argv[1], "watch") == 0) return cmd_watch(argc - 2, argv + 2);
  usage();
  return 2;
}
