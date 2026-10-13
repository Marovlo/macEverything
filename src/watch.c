#define _DARWIN_C_SOURCE 1
#include "internal.h"

#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void fs_callback(ConstFSEventStreamRef stream, void *info, size_t num, void *event_paths,
                        const FSEventStreamEventFlags flags[], const FSEventStreamEventId ids[]) {
  (void)stream;
  MEIndex *idx = info;
  if (!idx || !idx->watching) return;
  CFArrayRef paths = (CFArrayRef)event_paths;
  int changed = 0;
  for (size_t i = 0; i < num; i++) {
    char buf[16 * 1024];
    CFStringRef s = CFArrayGetValueAtIndex(paths, (CFIndex)i);
    if (!s || !CFStringGetFileSystemRepresentation(s, buf, (CFIndex)sizeof buf)) continue;
    me_note_event(idx, buf, (uint64_t)ids[i]);
    FSEventStreamEventFlags f = flags[i];
    FSEventStreamEventFlags rest = f & ~(FSEventStreamEventFlags)kFSEventStreamEventFlagHistoryDone;
    if (rest == 0) continue;
    pthread_rwlock_rdlock(&idx->lock);
    int ignored = me_path_ignored(idx, buf);
    pthread_rwlock_unlock(&idx->lock);
    if (ignored) continue;
    changed = 1;
    if (f & (kFSEventStreamEventFlagUserDropped | kFSEventStreamEventFlagKernelDropped |
             kFSEventStreamEventFlagRootChanged | kFSEventStreamEventFlagMustScanSubDirs)) {
      me_index_rescan(idx, buf);
      continue;
    }
    int exists = access(buf, F_OK) == 0;
    if (((f & kFSEventStreamEventFlagItemRemoved) || (f & kFSEventStreamEventFlagItemRenamed)) && !exists) {
      me_index_forget_path(idx, buf);
      continue;
    }
    me_index_refresh_path(idx, buf);
  }
  if (changed && idx->on_change) idx->on_change(idx->on_change_ctx);
}

int me_index_watch_start(MEIndex *idx, void (*on_change)(void *ctx), void *ctx) {
  if (!idx) return -1;
  if (idx->watching) return 0;
  if (idx->root_count == 0) {
    me_set_error("nothing to watch");
    return -1;
  }
  dispatch_queue_t q = dispatch_queue_create("maceverything.fsevents", DISPATCH_QUEUE_SERIAL);
  if (!q) return -1;
  idx->watch_q = q;
  idx->on_change = on_change;
  idx->on_change_ctx = ctx;
  idx->watching = 1;
  idx->stream_count = 0;
  for (uint32_t i = 0; i < idx->root_count; i++) {
    CFStringRef p = CFStringCreateWithFileSystemRepresentation(NULL, idx->roots[i].path);
    if (!p) continue;
    CFArrayRef arr = CFArrayCreate(NULL, (const void **)&p, 1, &kCFTypeArrayCallBacks);
    CFRelease(p);
    if (!arr) continue;
    FSEventStreamContext context;
    memset(&context, 0, sizeof context);
    context.info = idx;
    FSEventStreamEventId since = idx->roots[i].event_id ? idx->roots[i].event_id
                                                        : kFSEventStreamEventIdSinceNow;
    FSEventStreamRef stream = FSEventStreamCreate(
        NULL, fs_callback, &context, arr, since, 0.15,
        kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer |
            kFSEventStreamCreateFlagWatchRoot | kFSEventStreamCreateFlagUseCFTypes);
    CFRelease(arr);
    if (!stream) continue;
    FSEventStreamSetDispatchQueue(stream, q);
    if (!FSEventStreamStart(stream)) {
      FSEventStreamInvalidate(stream);
      FSEventStreamRelease(stream);
      continue;
    }
    idx->streams[idx->stream_count++] = stream;
  }
  if (idx->stream_count == 0) {
    idx->watching = 0;
    dispatch_release(q);
    idx->watch_q = NULL;
    me_set_error("FSEvent stream failed");
    return -1;
  }
  return 0;
}

void me_index_watch_stop(MEIndex *idx) {
  if (!idx || !idx->watching) return;
  idx->watching = 0;
  for (int i = 0; i < idx->stream_count; i++) {
    FSEventStreamRef stream = idx->streams[i];
    if (!stream) continue;
    FSEventStreamStop(stream);
    FSEventStreamInvalidate(stream);
  }
  dispatch_queue_t q = idx->watch_q;
  if (q) {
    dispatch_sync(q, ^{
                  });
  }
  for (int i = 0; i < idx->stream_count; i++) {
    FSEventStreamRef stream = idx->streams[i];
    if (!stream) continue;
    FSEventStreamRelease(stream);
    idx->streams[i] = NULL;
  }
  idx->stream_count = 0;
  if (q) {
    dispatch_release(q);
    idx->watch_q = NULL;
  }
  idx->on_change = NULL;
  idx->on_change_ctx = NULL;
}
