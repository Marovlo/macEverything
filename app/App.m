#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <fcntl.h>

#include "me.h"

#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

static _Atomic uint64_t g_epoch = 0;

@interface MERow : NSObject
@property(copy) NSString *name;
@property(copy) NSString *path;
@property(copy) NSString *sizeText;
@property(copy) NSString *timeText;
@property BOOL isDir;
@property BOOL isTerm; /* 历史页的词条行 */
@property(copy) NSString *term;
@property NSUInteger termCount;
@property NSInteger indent;
@property BOOL pinned; /* 已固定到首页 */
@end

@implementation MERow
@end

@interface AppDelegate : NSObject <NSApplicationDelegate, NSTableViewDataSource, NSTableViewDelegate, NSWindowDelegate, NSTextFieldDelegate, NSSearchFieldDelegate, NSMenuItemValidation, NSMenuDelegate>
@property(strong) NSStatusItem *status;
@property(strong) NSWindow *window;
@property(strong) NSSearchField *field;
@property(strong) NSTableView *table;
@property(strong) NSTextField *footer;
@property(strong) NSMutableArray<MERow *> *rows;
@property(strong) NSString *indexPath;
@property(strong) NSString *rootPath;
@property(nonatomic, assign) MEIndex *idx;
@property(nonatomic, assign) MEIndex *building;
@property dispatch_queue_t searchQ;
@property dispatch_queue_t saveQ;
@property(strong) NSMenu *menu;
@property BOOL ready;
@property BOOL skipSave;
@property BOOL updateCheckInFlight;
@property BOOL searchLoading;
@property BOOL quitting;
@property uint32_t searchTotal;
@property(nonatomic, assign) NSInteger meSortKey;
@property BOOL meSortAsc;
@property(nonatomic, assign) uint32_t *resultIds;  /* 搜索结果缓存，按当前排序，主线程持有 */
@property NSUInteger resultCount;
@property BOOL skippedProtected;
@property(strong) NSScrollView *listScroll;
@property(strong) NSMenuItem *countItem;
@property(strong) NSMutableDictionary<NSString *, NSImage *> *iconCache;
@property(strong) NSMutableArray<NSDictionary *> *history; /* [{q, files:[{p,n,d}]}]，最新在前 */
@property BOOL fdaGrantedCached; /* 完全磁盘访问已授权（缓存） */
@property BOOL fdaStateKnown;
@property BOOL fdaCheckInFlight;
@end

static AppDelegate *g_app = nil;

@class AppDelegate;
@interface MEScrollView : NSScrollView
@property(nonatomic, weak) AppDelegate *owner;
@end

@class AppDelegate;
@interface METableView : NSTableView
@property(nonatomic, weak) AppDelegate *owner;
@end

@implementation AppDelegate

static NSString * const MEActivateNotification = @"com.marovlo.macEverything.activate";

static NSString *FormatSize(uint64_t n) {
  if (n < 1024) return [NSString stringWithFormat:@"%llu B", (unsigned long long)n];
  double v = (double)n;
  const char *units[] = {"KB", "MB", "GB", "TB"};
  int u = -1;
  while (v >= 1024.0 && u < 3) {
    v /= 1024.0;
    u++;
  }
  return [NSString stringWithFormat:@"%.1f %s", v, units[u]];
}

static NSString *FormatTime(int64_t sec) {
  if (sec <= 0) return @"";
  static NSDateFormatter *fmt;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    fmt = [NSDateFormatter new];
    fmt.dateStyle = NSDateFormatterShortStyle;
    fmt.timeStyle = NSDateFormatterShortStyle;
  });
  return [fmt stringFromDate:[NSDate dateWithTimeIntervalSince1970:sec]];
}

static void OnIndexChange(void *ctx) {
  (void)ctx;
  dispatch_async(dispatch_get_main_queue(), ^{
    [g_app tick];
  });
}

- (void)applicationDidFinishLaunching:(NSNotification *)note {
  (void)note;
  g_app = self;
  self.meSortAsc = YES; /* 默认按名称升序 */
  [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
  self.rows = [NSMutableArray array];
  self.searchQ = dispatch_queue_create("maceverything.search", DISPATCH_QUEUE_SERIAL);
  self.saveQ = dispatch_queue_create("maceverything.save", DISPATCH_QUEUE_SERIAL);
  [[NSDistributedNotificationCenter defaultCenter] addObserver:self
                                                      selector:@selector(peerActivate:)
                                                           name:MEActivateNotification
                                                         object:nil];
  if ([NSBundle.mainBundle.bundlePath hasPrefix:@"/Volumes/"] && ![self hasLaunchFlag:@"--from-install"]) {
    [self installToApplicationsAndRelaunch];
    return;
  }
  if (![self wantsRescan] && ![self hasLaunchFlag:@"--from-install"]) {
    [[NSDistributedNotificationCenter defaultCenter] postNotificationName:MEActivateNotification
                                                                   object:nil
                                                                 userInfo:nil
                                                      deliverImmediately:YES];
    if ([self runningPeerCount] > 0) {
      [NSApp terminate:nil];
      return;
    }
  }
  [self finishLaunch];
}

/* 阻塞式探测（须在后台线程调用）。open 桌面目录：TCC 拒绝时会一直阻塞，
   与扫描器 scan.c 的探测方式一致；API 层的目录列举会静默返回空结果，不可靠。 */
- (BOOL)fdaProbeGrantedBlocking {
  NSString *desktop = [NSHomeDirectory() stringByAppendingPathComponent:@"Desktop"];
  int fd = open(desktop.fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd >= 0) {
    close(fd);
    return YES;
  }
  return NO;
}

/* 带超时的探测：0.4s 内打不开视为被 TCC 拦截。须在后台线程调用。 */
- (BOOL)fdaLikelyDenied {
  dispatch_semaphore_t sem = dispatch_semaphore_create(0);
  __block BOOL granted = NO;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    granted = [self fdaProbeGrantedBlocking];
    dispatch_semaphore_signal(sem);
  });
  BOOL got = dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW,
                                                        (int64_t)(0.4 * NSEC_PER_SEC))) == 0;
  return got ? !granted : YES; /* 超时未返回 = 被 TCC 挡住 */
}

/* 异步刷新授权状态缓存（被拒时探测会一直阻塞，in-flight 防止线程堆积）。 */
- (void)refreshFDAStateAsync {
  if (self.fdaCheckInFlight) return;
  self.fdaCheckInFlight = YES;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
    BOOL granted = [self fdaProbeGrantedBlocking]; /* 被拒时会阻塞挂起，属预期 */
    dispatch_async(dispatch_get_main_queue(), ^{
      self.fdaCheckInFlight = NO;
      if (granted != self.fdaGrantedCached || !self.fdaStateKnown) {
        self.fdaGrantedCached = granted;
        self.fdaStateKnown = YES;
      }
    });
  });
}

/* 首次启动且检测到受保护目录不可读时，引导用户授予完全磁盘访问（只提示一次）。 */
- (void)maybePromptForFDA {
  NSString *askedKey = @"MEAskedFDA1";
  if ([NSUserDefaults.standardUserDefaults boolForKey:askedKey]) return;
  [NSUserDefaults.standardUserDefaults setBool:YES forKey:askedKey];
  BOOL denied = [self fdaLikelyDenied];
  self.fdaGrantedCached = !denied;
  self.fdaStateKnown = YES;
  if (!denied) return;
  NSAlert *alert = [NSAlert new];
  alert.messageText = @"授予完全磁盘访问权限";
  alert.informativeText =
      @"MacEverything 需要完全磁盘访问权限，才能搜索桌面、文稿、下载等受保护目录。\n\n"
       "现在去系统设置授权吗？在开关处授权后，系统会提示重新打开应用。";
  [alert addButtonWithTitle:@"打开系统设置"];
  [alert addButtonWithTitle:@"以后再说"];
  alert.alertStyle = NSAlertStyleInformational;
  if ([alert runModal] == NSAlertFirstButtonReturn) [self openPrivacy];
}

- (void)finishLaunch {
  [self buildStatus];
  [self loadHistory];
  [self buildMainMenu];
  [self buildWindow];
  [self registerHotkey];
  [self showWindow];
  NSString *envIndex = NSProcessInfo.processInfo.environment[@"ME_INDEX"];
  NSString *envRoot = NSProcessInfo.processInfo.environment[@"ME_ROOT"];
  if (envIndex.length) self.indexPath = envIndex;
  else {
    char buf[1024];
    if (me_default_index_path(buf, sizeof buf) == 0) self.indexPath = @(buf);
  }
  self.rootPath = envRoot.length ? envRoot : NSHomeDirectory();
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    [self loadOrScan];
  });
  [NSTimer scheduledTimerWithTimeInterval:0.3 target:self selector:@selector(tick) userInfo:nil repeats:YES];
  [NSTimer scheduledTimerWithTimeInterval:10 target:self selector:@selector(checkpoint) userInfo:nil repeats:YES];
  [NSTimer scheduledTimerWithTimeInterval:3600 target:self selector:@selector(checkForUpdates) userInfo:nil repeats:YES];
  [self checkForUpdates];
  /* 窗口就绪后延迟 1 秒：刷新授权状态缓存 + 首次启动引导完全磁盘访问 */
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.0 * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
                   [self refreshFDAStateAsync];
                   [self maybePromptForFDA];
                 });
}

- (BOOL)hasLaunchFlag:(NSString *)flag {
  for (NSString *arg in NSProcessInfo.processInfo.arguments) {
    if ([arg isEqualToString:flag]) return YES;
  }
  return NO;
}

- (NSUInteger)runningPeerCount {
  NSString *bid = NSBundle.mainBundle.bundleIdentifier;
  NSUInteger n = 0;
  for (NSRunningApplication *app in NSWorkspace.sharedWorkspace.runningApplications) {
    if (app.processIdentifier == getpid()) continue;
    if ([app.bundleIdentifier isEqualToString:bid]) n++;
  }
  return n;
}

- (void)peerActivate:(NSNotification *)note {
  (void)note;
  dispatch_async(dispatch_get_main_queue(), ^{
    [self showWindow];
  });
}

// 从 DMG（/Volumes）启动时，把最新副本替换安装到 /Applications 再重启，避免旧副本一直留着。
- (void)installToApplicationsAndRelaunch {
  NSURL *src = NSBundle.mainBundle.bundleURL;
  NSURL *dst = [[NSURL fileURLWithPath:@"/Applications"] URLByAppendingPathComponent:src.lastPathComponent];
  NSFileManager *fm = NSFileManager.defaultManager;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:6];
    for (;;) {
      BOOL any = NO;
      for (NSRunningApplication *app in NSWorkspace.sharedWorkspace.runningApplications) {
        if (app.processIdentifier == getpid()) continue;
        if ([app.bundleIdentifier isEqualToString:NSBundle.mainBundle.bundleIdentifier]) {
          any = YES;
          [app terminate];
        }
      }
      if (!any || [NSDate.date timeIntervalSinceDate:deadline] >= 0) break;
      [NSThread sleepForTimeInterval:0.2];
    }
    [fm removeItemAtURL:dst error:NULL];
    BOOL ok = [fm copyItemAtURL:src toURL:dst error:NULL];
    dispatch_async(dispatch_get_main_queue(), ^{
      if (!ok) {
        [self finishLaunch];
        return;
      }
      // 不能直接 openApplicationAtURL：自己还活着时 LS 会把请求路由给运行中的实例。
      // 派生一个独立小进程，等自己完全退出后再打开安装好的副本（普通启动路径）。
      pid_t child = -1;
      char *argv[] = {"/bin/sh", "-c", (char *)[[NSString stringWithFormat:
          @"while kill -0 %d 2>/dev/null; do sleep 0.2; done; open %@", getpid(), dst.path] UTF8String], NULL};
      int rc = posix_spawn(&child, "/bin/sh", NULL, NULL, argv, environ);
      if (rc != 0) {
        [self finishLaunch];
        return;
      }
      self.skipSave = YES;
      [NSApp terminate:nil];
    });
  });
}

- (BOOL)wantsRescan {
  for (NSString *arg in NSProcessInfo.processInfo.arguments) {
    if ([arg isEqualToString:@"--rescan"]) return YES;
  }
  return NO;
}

- (void)showFooter:(NSString *)text {
  dispatch_async(dispatch_get_main_queue(), ^{
    self.footer.stringValue = text;
  });
}

#pragma mark - 搜索历史

- (NSString *)historyFilePath {
  return [NSHomeDirectory() stringByAppendingPathComponent:
                        @"Library/Application Support/MacEverything/history.json"];
}

- (void)loadHistory {
  NSData *d = [NSData dataWithContentsOfFile:[self historyFilePath]];
  NSArray *arr = d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:NULL] : nil;
  self.history = [arr isKindOfClass:[NSArray class]] ? [arr mutableCopy] : [NSMutableArray array];
}

- (void)saveHistory {
  NSData *d = [NSJSONSerialization dataWithJSONObject:self.history
                                              options:NSJSONWritingPrettyPrinted
                                                error:NULL];
  if (d) [d writeToFile:[self historyFilePath] options:NSDataWritingAtomic error:NULL];
}

/* 用户在某词条搜索结果里对文件执行了打开/显示/固定动作：记录 词条 → 文件。 */
- (void)recordHistory:(NSString *)query row:(MERow *)item pinned:(BOOL)pinned {
  if (query.length == 0 || item.isTerm || !self.history || item.path.length == 0) return;
  NSMutableDictionary *fileRec = [NSMutableDictionary dictionaryWithDictionary:@{
    @"p" : item.path,
    @"n" : item.name ?: @"",
    @"d" : @(item.isDir),
  }];
  if (pinned) fileRec[@"pin"] = @YES;
  NSUInteger ti = 0;
  for (NSDictionary *t in self.history) {
    if ([t[@"q"] isEqual:query]) break;
    ti++;
  }
  NSMutableDictionary *termDict;
  if (ti < self.history.count) {
    termDict = [self.history[ti] mutableCopy];
    [self.history removeObjectAtIndex:ti];
  } else {
    termDict = [NSMutableDictionary dictionaryWithObject:query forKey:@"q"];
  }
  NSMutableArray *files = [termDict[@"files"] isKindOfClass:[NSArray class]]
                              ? [termDict[@"files"] mutableCopy]
                              : [NSMutableArray array];
  NSUInteger fi = 0;
  for (NSDictionary *f in files) {
    if ([f[@"p"] isEqual:fileRec[@"p"]]) break;
    fi++;
  }
  if (fi < files.count) [files removeObjectAtIndex:fi];
  [files insertObject:fileRec atIndex:0];
  /* 超限时从尾部裁掉未固定的记录，已固定的保留 */
  if (files.count > 20) {
    NSUInteger i = files.count;
    while (files.count > 20 && i > 0) {
      i--;
      if (![files[i][@"pin"] boolValue]) [files removeObjectAtIndex:i];
    }
  }
  termDict[@"files"] = files;
  [self.history insertObject:termDict atIndex:0];
  if (self.history.count > 50) [self.history removeObjectsInRange:NSMakeRange(50, self.history.count - 50)];
  [self saveHistory];
}

/* 历史页点击词条：填入搜索框并重新搜索。 */
- (void)searchForTerm:(NSString *)term {
  if (term.length == 0) return;
  if (![self.field.stringValue isEqualToString:term]) self.field.stringValue = term;
  [self scheduleSearch];
}

/* 点击词条行的 ✕：删除该词条及其文件记录。 */
/* 右键"固定到首页"：把当前词条 + 文件立即记入首页历史（带固定标记）。 */
- (void)pinToHome:(id)sender {
  (void)sender;
  NSInteger row = self.table.selectedRow;
  if (row < 0 || (NSUInteger)row >= self.rows.count) return;
  MERow *item = self.rows[(NSUInteger)row];
  if (item.isTerm || item.path.length == 0 || self.field.stringValue.length == 0) return;
  [self recordHistory:self.field.stringValue row:item pinned:YES];
  self.footer.stringValue = [NSString stringWithFormat:@"📌 已固定到首页：%@", item.name];
}

- (void)deleteHistoryTerm:(NSButton *)btn {
  NSInteger row = [self.table rowForView:btn];
  if (row < 0 || (NSUInteger)row >= self.rows.count || !self.history) return;
  MERow *item = self.rows[(NSUInteger)row];
  if (!item.isTerm) return;
  for (NSUInteger ti = 0; ti < self.history.count; ti++) {
    if ([self.history[ti][@"q"] isEqual:item.term]) {
      [self.history removeObjectAtIndex:ti];
      break;
    }
  }
  [self saveHistory];
  [self scheduleSearch];
}

- (void)useIndex:(MEIndex *)idx {
  dispatch_async(dispatch_get_main_queue(), ^{
    self.idx = idx;
    self.ready = YES;
    [self tick];
    if (self.window.isVisible) [self scheduleSearch];
  });
}

- (void)finishScan:(MEIndex *)fresh stats:(MEScanStats)stats replacing:(MEIndex *)old {
  NSString *path = self.indexPath;
  dispatch_sync(self.saveQ, ^{
    me_index_set_complete(fresh, 1);
    if (path.length && me_index_save(fresh, path.UTF8String) != 0) {
      [self showFooter:[NSString stringWithFormat:@"索引在内存里，但没能保存：%s", me_last_error()]];
    }
  });
  me_index_watch_start(fresh, OnIndexChange, NULL);
  BOOL skipped = stats.skipped_protected != 0;
  dispatch_async(dispatch_get_main_queue(), ^{
    self.building = NULL;
    self.skippedProtected = skipped;
    self.idx = fresh;
    self.ready = YES;
    self.fdaGrantedCached = !skipped;
    self.fdaStateKnown = YES;
    [self tick];
    if (self.window.isVisible) [self scheduleSearch];
    if (old && old != fresh) {
      dispatch_async(self.searchQ, ^{
        me_index_free(old);
      });
    }
  });
}

- (void)loadOrScan {
  NSString *path = self.indexPath;
  NSString *root = self.rootPath;
  MEIndex *loaded = NULL;
  if (![self wantsRescan] && path.length && access(path.UTF8String, R_OK) == 0) {
    loaded = me_index_load(path.UTF8String);
  }
  if (loaded && me_index_complete(loaded)) {
    char stored[1024];
    if (me_index_root(loaded, 0, stored, sizeof stored) == 0) {
      NSString *r = @(stored);
      dispatch_async(dispatch_get_main_queue(), ^{
        self.rootPath = r;
      });
    }
    me_index_watch_start(loaded, OnIndexChange, NULL);
    [self useIndex:loaded];
    [self showFooter:[NSString stringWithFormat:@"已加载索引，共 %u 个名称", me_index_live(loaded)]];
    return;
  }
  if (loaded) {
    me_index_watch_start(loaded, OnIndexChange, NULL);
    [self useIndex:loaded];
    [self showFooter:@"已加载上次未完成的索引，正在补全…"];
  }
  MEIndex *fresh = me_index_create();
  if (!fresh) return;
  dispatch_async(dispatch_get_main_queue(), ^{
    self.building = fresh;
    if (!loaded) self.idx = fresh;
  });
  MEScanStats stats;
  memset(&stats, 0, sizeof stats);
  if (me_index_scan(fresh, root.UTF8String, &stats) != 0) {
    [self showFooter:[NSString stringWithFormat:@"索引失败：%s", me_last_error()]];
    if (!loaded) [self useIndex:fresh];
    dispatch_async(dispatch_get_main_queue(), ^{
      self.building = NULL;
    });
    return;
  }
  [self finishScan:fresh stats:stats replacing:loaded];
}

- (void)checkpoint {
  MEIndex *idx = self.building;
  if (!idx || !self.indexPath.length || !me_index_scanning(idx)) return;
  if (self.idx && self.idx != idx && me_index_complete(self.idx)) return;
  NSString *path = self.indexPath;
  dispatch_async(self.saveQ, ^{
    if (!me_index_scanning(idx)) return;
    me_index_save(idx, path.UTF8String);
  });
}

- (void)buildStatus {
  self.status = [NSStatusBar.systemStatusBar statusItemWithLength:NSVariableStatusItemLength];
  NSImage *image = [NSImage imageWithSystemSymbolName:@"magnifyingglass" accessibilityDescription:@"MacEverything"];
  image.template = YES;
  self.status.button.image = image;
  self.status.button.imagePosition = NSImageLeft;
  self.status.button.title = @"ME";
  self.status.visible = YES;
  NSMenu *menu = [NSMenu new];
  NSMenuItem *show = [[NSMenuItem alloc] initWithTitle:@"搜索" action:@selector(showWindow) keyEquivalent:@""];
  show.target = self;
  [menu addItem:show];
  NSMenuItem *re = [[NSMenuItem alloc] initWithTitle:@"重新索引" action:@selector(reindex) keyEquivalent:@""];
  re.target = self;
  [menu addItem:re];
  self.countItem = [[NSMenuItem alloc] initWithTitle:@"正在准备索引" action:NULL keyEquivalent:@""];
  self.countItem.enabled = NO;
  [menu addItem:self.countItem];
  [menu addItem:[NSMenuItem separatorItem]];
  NSMenuItem *fda = [[NSMenuItem alloc] initWithTitle:@"打开完全磁盘访问权限" action:@selector(openPrivacy) keyEquivalent:@""];
  fda.target = self;
  [menu addItem:fda];
  NSMenuItem *updates = [[NSMenuItem alloc] initWithTitle:@"检查更新" action:@selector(checkForUpdatesNow:) keyEquivalent:@""];
  updates.target = self;
  [menu addItem:updates];
  NSMenuItem *quit = [[NSMenuItem alloc] initWithTitle:@"退出" action:@selector(quit) keyEquivalent:@"q"];
  quit.target = self;
  [menu addItem:quit];
  self.menu = menu;
  self.status.button.target = self;
  self.status.button.action = @selector(statusClick:);
  [self.status.button sendActionOn:NSEventMaskLeftMouseUp | NSEventMaskRightMouseUp];
  self.status.button.toolTip = @"MacEverything  左键搜索，右键菜单，⌃⌥Space";
}

- (void)statusClick:(id)sender {
  (void)sender;
  NSEvent *event = NSApp.currentEvent;
  BOOL menuClick = event.type == NSEventTypeRightMouseUp || event.type == NSEventTypeRightMouseDown ||
                   (event.modifierFlags & NSEventModifierFlagControl) != 0;
  if (menuClick) {
    NSPoint origin = NSMakePoint(0, NSMinY(self.status.button.bounds) - 4);
    [self.menu popUpMenuPositioningItem:nil atLocation:origin inView:self.status.button];
    return;
  }
  BOOL shown = self.window.isVisible && !self.window.miniaturized && self.window.isKeyWindow;
  if (shown) [self.window orderOut:nil];
  else [self showWindow];
}

- (void)buildWindow {
  NSRect frame = NSMakeRect(0, 0, 1080, 600);
  self.window = [[NSWindow alloc] initWithContentRect:frame
                                            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                      NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                              backing:NSBackingStoreBuffered
                                                defer:NO];
  self.window.title = @"MacEverything";
  self.window.delegate = self;
  self.window.collectionBehavior = NSWindowCollectionBehaviorMoveToActiveSpace | NSWindowCollectionBehaviorFullScreenAuxiliary;
  self.window.contentMinSize = NSMakeSize(520, 260);
  self.window.frameAutosaveName = @"MESearchWindow";
  [self.window center];
  NSView *content = self.window.contentView;
  self.field = [NSSearchField new];
  self.field.placeholderString = @"输入文件名搜索";
  self.field.delegate = self;
  self.field.font = [NSFont systemFontOfSize:16];
  self.field.sendsSearchStringImmediately = NO;
  [content addSubview:self.field];

  MEScrollView *scroll = [MEScrollView new];
  scroll.owner = self;
  scroll.hasVerticalScroller = YES;
  scroll.autohidesScrollers = NO;
  self.listScroll = scroll;
  METableView *table = [[METableView alloc] initWithFrame:NSZeroRect];
  table.owner = self;
  self.table = table;
  self.table.rowHeight = 22;
  self.table.usesAlternatingRowBackgroundColors = YES;
  self.table.doubleAction = @selector(open:);
  self.table.target = self;
  self.table.dataSource = self;
  self.table.delegate = self;
  NSArray *titles = @[@"名称", @"路径", @"大小", @"修改时间"];
  NSArray *ids = @[@"name", @"path", @"size", @"time"];
  NSArray *sortKeys = @[@"name", @"path", @"size", @"mtime"];
  CGFloat widths[] = {220, 500, 90, 160};
  for (NSUInteger i = 0; i < titles.count; i++) {
    NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:ids[i]];
    col.title = titles[i];
    col.width = widths[i];
    col.resizingMask = [ids[i] isEqualToString:@"path"] ? NSTableColumnAutoresizingMask
                                                        : NSTableColumnUserResizingMask;
    col.sortDescriptorPrototype = [[NSSortDescriptor alloc] initWithKey:sortKeys[i] ascending:YES];
    [self.table addTableColumn:col];
  }
  NSMenu *ctx = [NSMenu new];
  NSMenuItem *ctxOpen = [[NSMenuItem alloc] initWithTitle:@"打开" action:@selector(open:) keyEquivalent:@""];
  ctxOpen.target = self;
  [ctx addItem:ctxOpen];
  NSMenuItem *ctxReveal = [[NSMenuItem alloc] initWithTitle:@"在 Finder 中显示" action:@selector(reveal:) keyEquivalent:@""];
  ctxReveal.target = self;
  [ctx addItem:ctxReveal];
  NSMenuItem *ctxPin = [[NSMenuItem alloc] initWithTitle:@"固定到首页 📌" action:@selector(pinToHome:) keyEquivalent:@""];
  ctxPin.target = self;
  [ctx addItem:ctxPin];
  NSMenuItem *ctxCopy = [[NSMenuItem alloc] initWithTitle:@"拷贝路径" action:@selector(copyPath:) keyEquivalent:@""];
  ctxCopy.target = self;
  [ctx addItem:ctxCopy];
  ctx.autoenablesItems = NO; /* 可用状态由 prepareContextMenuForEvent 显式控制 */
  ctx.delegate = self;
  self.table.menu = ctx;
  scroll.documentView = self.table;
  [content addSubview:scroll];
  self.footer = [NSTextField labelWithString:@"输入文件名"];
  self.footer.font = [NSFont systemFontOfSize:12];
  self.footer.textColor = NSColor.secondaryLabelColor;
  [content addSubview:self.footer];
  self.field.translatesAutoresizingMaskIntoConstraints = NO;
  scroll.translatesAutoresizingMaskIntoConstraints = NO;
  self.footer.translatesAutoresizingMaskIntoConstraints = NO;
  [NSLayoutConstraint activateConstraints:@[
    [self.field.topAnchor constraintEqualToAnchor:content.topAnchor constant:12],
    [self.field.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:12],
    [self.field.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-12],
    [scroll.topAnchor constraintEqualToAnchor:self.field.bottomAnchor constant:8],
    [scroll.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:12],
    [scroll.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-12],
    [scroll.bottomAnchor constraintEqualToAnchor:self.footer.topAnchor constant:-8],
    [self.footer.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:14],
    [self.footer.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-14],
    [self.footer.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-10],
  ]];
}

static OSStatus HotkeyHandler(EventHandlerCallRef next, EventRef event, void *user) {
  (void)next;
  (void)user;
  if (GetEventKind(event) == kEventHotKeyPressed) {
    dispatch_async(dispatch_get_main_queue(), ^{
      [g_app showWindow];
    });
  }
  return noErr;
}

- (void)registerHotkey {
  EventHotKeyID hid = {'MEVY', 1};
  EventHotKeyRef ref = NULL;
  OSStatus err = RegisterEventHotKey(kVK_Space, controlKey | optionKey, hid, GetApplicationEventTarget(), 0, &ref);
  EventTypeSpec spec = {kEventClassKeyboard, kEventHotKeyPressed};
  InstallApplicationEventHandler(HotkeyHandler, 1, &spec, NULL, NULL);
  if (err != noErr) {
    self.footer.stringValue = @"全局快捷键 ⌃⌥Space 注册失败，可能被其他应用占用";
  }
}

// LSUIElement 应用也需要主菜单，⌘Q/⌘W/⌘C 等键位等效只有通过主菜单才全局生效。
- (void)buildMainMenu {
  NSMenu *main = [NSMenu new];

  NSMenuItem *appItem = [[NSMenuItem alloc] initWithTitle:@"" action:NULL keyEquivalent:@""];
  NSMenu *appMenu = [[NSMenu alloc] initWithTitle:@"MacEverything"];
  [appMenu addItemWithTitle:@"关于 MacEverything" action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
  [appMenu addItem:[NSMenuItem separatorItem]];
  [appMenu addItemWithTitle:@"隐藏 MacEverything" action:@selector(hide:) keyEquivalent:@"h"];
  [appMenu addItemWithTitle:@"退出 MacEverything" action:@selector(terminate:) keyEquivalent:@"q"];
  appItem.submenu = appMenu;
  [main addItem:appItem];

  NSMenuItem *fileItem = [[NSMenuItem alloc] initWithTitle:@"文件" action:NULL keyEquivalent:@""];
  NSMenu *fileMenu = [[NSMenu alloc] initWithTitle:@"文件"];
  [fileMenu addItemWithTitle:@"搜索" action:@selector(focusSearch) keyEquivalent:@"f"];
  [fileMenu addItem:[NSMenuItem separatorItem]];
  [fileMenu addItemWithTitle:@"在 Finder 中显示" action:@selector(reveal:) keyEquivalent:@"r"];
  [fileMenu addItem:[NSMenuItem separatorItem]];
  [fileMenu addItemWithTitle:@"关闭窗口" action:@selector(performClose:) keyEquivalent:@"w"];
  fileItem.submenu = fileMenu;
  [main addItem:fileItem];

  NSMenuItem *editItem = [[NSMenuItem alloc] initWithTitle:@"编辑" action:NULL keyEquivalent:@""];
  NSMenu *editMenu = [[NSMenu alloc] initWithTitle:@"编辑"];
  [editMenu addItemWithTitle:@"拷贝路径" action:@selector(copy:) keyEquivalent:@"c"];
  editItem.submenu = editMenu;
  [main addItem:editItem];

  NSApp.mainMenu = main;
}

- (void)focusSearch {
  if (!self.window.isVisible) [self showWindow];
  [self.window makeFirstResponder:self.field];
}

- (void)copyPath:(id)sender {
  (void)sender;
  NSInteger row = self.table.selectedRow;
  if (row < 0 || (NSUInteger)row >= self.rows.count) return;
  MERow *item = self.rows[(NSUInteger)row];
  if (item.isTerm || item.path.length == 0) return;
  [NSPasteboard.generalPasteboard clearContents];
  [NSPasteboard.generalPasteboard setString:item.path forType:NSPasteboardTypeString];
}

- (BOOL)validateMenuItem:(NSMenuItem *)item {
  if (item.action == @selector(open:) || item.action == @selector(reveal:) ||
      item.action == @selector(copyPath:)) {
    return self.table.selectedRow >= 0;
  }
  if (item.action == @selector(reindex)) return !self.quitting && !me_index_scanning(self.idx);
  if (item.action == @selector(openPrivacy)) {
    [self refreshFDAStateAsync]; /* 打开菜单时惰性刷新，不阻塞菜单弹出 */
    return !self.fdaStateKnown ? YES : !self.fdaGrantedCached; /* 已授权则置灰 */
  }
  return YES;
}

// 右键弹出前调用：选中鼠标所在行，并直接设置菜单项可用状态
// （新系统菜单自动校验时机不可靠，autoenablesItems=NO 后由这里全权负责）。
- (void)prepareContextMenuForEvent:(NSEvent *)event inTable:(METableView *)table {
  NSPoint p = [table convertPoint:event.locationInWindow fromView:nil];
  NSInteger row = [table rowAtPoint:p];
  if (row >= 0 && ![table isRowSelected:row]) {
    [table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row] byExtendingSelection:NO];
  }
  NSInteger active = table.selectedRow >= 0 ? table.selectedRow : table.clickedRow;
  BOOL hasRow = NO;
  if (active >= 0 && active < (NSInteger)self.rows.count) {
    hasRow = !self.rows[(NSUInteger)active].isTerm;
  }
  for (NSMenuItem *it in table.menu.itemArray) {
    if (it.action == @selector(open:) || it.action == @selector(reveal:) ||
        it.action == @selector(copyPath:)) {
      it.enabled = hasRow;
    }
    if (it.action == @selector(pinToHome:)) {
      it.enabled = hasRow && self.field.stringValue.length > 0;
    }
  }
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
  (void)sender;
  return NO;
}

- (BOOL)windowShouldClose:(NSWindow *)sender {
  dispatch_async(dispatch_get_main_queue(), ^{
    [sender orderOut:nil];
  });
  return NO;
}

- (BOOL)applicationShouldHandleReopen:(NSApplication *)sender hasVisibleWindows:(BOOL)flag {
  (void)sender;
  (void)flag;
  [self showWindow];
  return NO;
}

- (void)showWindow {
  if (NSApp.hidden) [NSApp unhide:nil];
  if (self.window.miniaturized) [self.window deminiaturize:nil];
  NSWindow *statusWin = self.status.button.window;
  if (statusWin) {
    NSRect sf = statusWin.frame;
    NSRect wf = self.window.frame;
    NSScreen *screen = statusWin.screen ?: NSScreen.mainScreen;
    NSRect vis = screen.visibleFrame;
    CGFloat x = NSMidX(sf) - wf.size.width / 2.0;
    CGFloat y = NSMinY(sf) - wf.size.height - 6.0;
    if (x < NSMinX(vis)) x = NSMinX(vis);
    if (x + wf.size.width > NSMaxX(vis)) x = NSMaxX(vis) - wf.size.width;
    if (y < NSMinY(vis)) y = NSMinY(vis);
    [self.window setFrameOrigin:NSMakePoint(x, y)];
  }
  self.window.hidesOnDeactivate = NO;
  [NSApp activateIgnoringOtherApps:YES];
  [self.window orderFrontRegardless];
  [self.window makeKeyAndOrderFront:nil];
  [self ensureWindowOnScreen];
  /* frameAutosaveName 的恢复发生在窗口真正显示时甚至稍晚，可能把窗口放回已断开的
     显示器；延迟再校一次。 */
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.4 * NSEC_PER_SEC)),
                 dispatch_get_main_queue(), ^{
                   [self ensureWindowOnScreen];
                 });
  [self.window makeFirstResponder:self.field];
  [self scheduleSearch];
}

/* 窗口落在所有屏幕之外（如外接显示器被拔掉）时，拉回主屏居中。 */
- (void)ensureWindowOnScreen {
  NSRect wf = self.window.frame;
  BOOL onScreen = NO;
  for (NSScreen *s in NSScreen.screens) {
    if (NSIntersectsRect(wf, s.frame)) {
      onScreen = YES;
      break;
    }
  }
  if (onScreen) return;
  NSScreen *screen = NSScreen.mainScreen ?: NSScreen.screens.firstObject;
  if (!screen) return;
  NSRect vis = screen.visibleFrame;
  CGFloat w = MIN(NSWidth(wf), NSWidth(vis));
  CGFloat h = MIN(NSHeight(wf), NSHeight(vis));
  NSRect r = NSMakeRect(NSMidX(vis) - w / 2.0, NSMinY(vis) + (NSHeight(vis) - h) / 2.0, w, h);
  [self.window setFrame:r display:YES];
}

- (void)tick {
  MEIndex *idx = self.idx;
  if (!idx) return;
  MEIndex *building = self.building;
  if (building && me_index_scanning(building)) {
    unsigned long long n = (unsigned long long)me_index_progress(building);
    self.countItem.title = [NSString stringWithFormat:@"正在索引 %llu 个名称", n];
    if (self.window.isVisible && (!self.ready || self.idx != building)) {
      self.footer.stringValue = [NSString stringWithFormat:@"正在索引 %llu 个名称…", n];
    }
  } else if (me_index_scanning(idx)) {
    unsigned long long n = (unsigned long long)me_index_progress(idx);
    self.countItem.title = [NSString stringWithFormat:@"正在索引 %llu 个名称", n];
    if (self.window.isVisible) self.footer.stringValue = [NSString stringWithFormat:@"正在索引 %llu 个名称…", n];
  } else if (self.ready) {
    self.countItem.title = [NSString stringWithFormat:@"已索引 %u 个名称", me_index_live(idx)];
  }
}

- (void)scheduleSearch {
  if (!self.ready || !self.idx) {
    self.footer.stringValue = me_index_scanning(self.idx) ? @"正在建立索引…" : @"正在准备索引";
    return;
  }
  self.listScroll.verticalScroller.target = nil;
  self.listScroll.verticalScroller.action = NULL;
  self.searchLoading = NO;
  self.searchTotal = 0;
  if (self.field.stringValue.length == 0) {
    atomic_fetch_add(&g_epoch, 1);
    free(self.resultIds);
    self.resultIds = NULL;
    self.resultCount = 0;
    /* 首页展示搜索历史：词条行 + 缩进的文件行。
       含固定文件的词条始终排在最前，其余按最近使用排序。 */
    self.rows = [NSMutableArray array];
    NSMutableArray *pinnedTerms = [NSMutableArray array];
    NSMutableArray *plainTerms = [NSMutableArray array];
    for (NSDictionary *t in self.history) {
      BOOL hasPin = NO;
      for (NSDictionary *f in t[@"files"]) {
        if ([f[@"pin"] boolValue]) {
          hasPin = YES;
          break;
        }
      }
      [hasPin ? pinnedTerms : plainTerms addObject:t];
    }
    for (NSDictionary *t in [pinnedTerms arrayByAddingObjectsFromArray:plainTerms]) {
      MERow *tr = [MERow new];
      tr.isTerm = YES;
      tr.term = t[@"q"];
      tr.termCount = [t[@"files"] count];
      tr.name = tr.term;
      tr.indent = 0;
      [self.rows addObject:tr];
      for (NSDictionary *f in t[@"files"]) {
        MERow *fr = [MERow new];
        fr.name = f[@"n"] ?: @"";
        fr.path = f[@"p"];
        fr.isDir = [f[@"d"] boolValue];
        fr.indent = 1;
        fr.pinned = [f[@"pin"] boolValue];
        /* 实时 stat：文件被删后大小/时间留空 */
        NSDictionary *st = [NSFileManager.defaultManager attributesOfItemAtPath:fr.path error:NULL];
        if (st) {
          if (!fr.isDir) fr.sizeText = FormatSize([st[NSFileSize] unsignedLongLongValue]);
          fr.timeText = FormatTime([st[NSFileModificationDate] timeIntervalSince1970]);
        }
        [self.rows addObject:fr];
      }
    }
    [self.table reloadData];
    self.footer.stringValue =
        self.history.count
            ? [NSString stringWithFormat:@"搜索历史 %lu 条", (unsigned long)self.history.count]
            : @"输入文件名开始搜索";
    return;
  }
  uint64_t epoch = atomic_fetch_add(&g_epoch, 1) + 1;
  [self fetchSearch:self.field.stringValue epoch:epoch];
}

static const uint32_t kSearchPage = 300;

/* 一次性取回按当前排序排列的全部匹配，之后从缓存分页展示。 */
- (void)fetchSearch:(NSString *)query epoch:(uint64_t)epoch {
  MEIndex *idx = self.idx;
  if (!idx || !query) return;
  int key = (int)self.meSortKey;
  int asc = self.meSortAsc ? 1 : 0;
  dispatch_async(self.searchQ, ^{
    if (atomic_load(&g_epoch) != epoch) return;
    uint32_t *ids = NULL;
    uint32_t count = 0;
    double seconds = 0;
    int rc = me_index_search_sorted(idx, query.UTF8String, key, asc, &ids, &count, &seconds);
    dispatch_async(dispatch_get_main_queue(), ^{
      if (atomic_load(&g_epoch) != epoch) {
        free(ids);
        return;
      }
      if (rc != 0) {
        self.searchLoading = NO;
        self.footer.stringValue = [NSString stringWithFormat:@"搜索失败：%s", me_last_error()];
        return;
      }
      free(self.resultIds);
      self.resultIds = ids;
      self.resultCount = count;
      self.searchTotal = count;
      self.rows = [NSMutableArray array];
      [self loadMoreRows];
      [self.table reloadData];
      if (count == 0) {
        self.footer.stringValue = @"匹配 0 个文件";
      } else if ((uint32_t)self.rows.count < count) {
        self.footer.stringValue = [NSString stringWithFormat:@"匹配 %u 个，已显示 %lu 个", count,
                                                             (unsigned long)self.rows.count];
      } else {
        self.footer.stringValue = [NSString stringWithFormat:@"匹配 %u 个 · %.1f ms", count, seconds * 1000.0];
      }
    });
  });
}

/* 主线程调用：从结果缓存构建下一页行。 */
- (void)loadMoreRows {
  if (!self.resultIds || self.rows.count >= self.resultCount) return;
  uint32_t n = (uint32_t)MIN((NSUInteger)kSearchPage, self.resultCount - self.rows.count);
  MEInfo *infos = calloc(n, sizeof(MEInfo));
  if (!infos) return;
  me_index_infos(self.idx, self.resultIds + self.rows.count, n, infos);
  for (uint32_t i = 0; i < n; i++) {
    MERow *row = [MERow new];
    row.name = @(infos[i].name);
    row.path = @(infos[i].path);
    row.sizeText = infos[i].is_dir ? @"" : FormatSize(infos[i].size);
    row.timeText = FormatTime(infos[i].mtime);
    row.isDir = infos[i].is_dir != 0;
    [self.rows addObject:row];
  }
  free(infos);
}

- (void)updateResultFooter {
  if (self.rows.count < self.searchTotal) {
    self.footer.stringValue = [NSString stringWithFormat:@"匹配 %u 个，已显示 %lu 个", self.searchTotal,
                                                         (unsigned long)self.rows.count];
  } else {
    self.footer.stringValue = [NSString stringWithFormat:@"匹配 %u 个", self.searchTotal];
  }
}

- (void)loadMoreSearchIfNeeded {
  if (self.searchLoading || !self.ready) return;
  if (self.field.stringValue.length == 0) return; /* 历史页不需要分页 */
  if (self.searchTotal == 0 || self.rows.count >= self.searchTotal) return;
  if (!self.resultIds) return;
  NSClipView *clip = self.listScroll.contentView;
  CGFloat bottom = NSMaxY(clip.bounds);
  CGFloat height = NSHeight(self.table.bounds);
  CGFloat row = self.table.rowHeight > 1 ? self.table.rowHeight : 22;
  if (height > row && bottom + row * 8 < height) return;
  NSUInteger before = self.rows.count;
  [self loadMoreRows];
  if (self.rows.count == before) return;
  [self.table reloadData];
  [self updateResultFooter];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tableView {
  (void)tableView;
  return (NSInteger)self.rows.count;
}

/* viewForTableColumn 对越界行返回 nil 时，表格退回单元格路径渲染；
   必须提供空值兜底，否则 nil 值进 NSCell 格式化会抛异常。 */
- (id)tableView:(NSTableView *)tableView objectValueForTableColumn:(NSTableColumn *)column row:(NSInteger)row {
  (void)tableView;
  (void)column;
  (void)row;
  return @"";
}

/* 计算文件名中需要高亮的区间，语义与搜索一致：多词 AND、!排除不参与、
 * 大小写/音调/全半角不敏感、* ? 通配。 */
- (NSMutableIndexSet *)highlightRangesForName:(NSString *)name {
  NSMutableIndexSet *hits = [NSMutableIndexSet indexSet];
  NSString *q = self.field.stringValue;
  if (q.length == 0 || name.length == 0) return hits;
  NSArray<NSString *> *parts =
      [q componentsSeparatedByCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@" \t"]];
  for (NSString *raw in parts) {
    if (raw.length == 0) continue;
    NSString *term = raw;
    if ([term hasPrefix:@"!"] && term.length > 1) continue; /* 排除词不高亮 */
    BOOL wild = [term rangeOfCharacterFromSet:[NSCharacterSet characterSetWithCharactersInString:@"*?"]]
                    .location != NSNotFound;
    if (wild) {
      NSMutableString *pattern = [NSMutableString string];
      NSMutableString *literal = [NSMutableString string];
      for (NSUInteger i = 0; i < term.length; i++) {
        unichar c = [term characterAtIndex:i];
        if (c == '*' || c == '?') {
          [pattern appendString:[NSRegularExpression escapedPatternForString:literal]];
          [literal setString:@""];
          [pattern appendString:(c == '*' ? @".*" : @".")];
        } else {
          [literal appendFormat:@"%C", c];
        }
      }
      [pattern appendString:[NSRegularExpression escapedPatternForString:literal]];
      NSError *err = nil;
      NSRegularExpression *re = [NSRegularExpression regularExpressionWithPattern:pattern
                                                                          options:NSRegularExpressionCaseInsensitive
                                                                            error:&err];
      if (err) continue;
      [re enumerateMatchesInString:name
                           options:0
                             range:NSMakeRange(0, name.length)
                        usingBlock:^(NSTextCheckingResult *r, NSMatchingFlags flags, BOOL *stop) {
                          (void)flags;
                          (void)stop;
                          if (r.range.location != NSNotFound) [hits addIndexesInRange:r.range];
                        }];
      continue;
    }
    NSRange search = NSMakeRange(0, name.length);
    for (;;) {
      NSRange hit = [name rangeOfString:term
                                options:(NSCaseInsensitiveSearch | NSDiacriticInsensitiveSearch |
                                         NSWidthInsensitiveSearch)
                                  range:search];
      if (hit.location == NSNotFound) break;
      [hits addIndexesInRange:hit];
      if (NSMaxRange(hit) >= name.length) break;
      search = NSMakeRange(NSMaxRange(hit), name.length - NSMaxRange(hit));
    }
  }
  return hits;
}

/* 命中部分：加粗 + 主文字色（高对比）；其余：次级文字色。深浅色模式都清晰。
   富文本必须自带"单行尾部截断"段落样式，否则长文件名会换行。 */
- (NSAttributedString *)highlightedName:(NSString *)name {
  NSMutableParagraphStyle *para = [NSMutableParagraphStyle new];
  para.lineBreakMode = NSLineBreakByTruncatingTail;
  if (name.length == 0) {
    return [[NSAttributedString alloc] initWithString:@"" attributes:@{
      NSFontAttributeName : [NSFont systemFontOfSize:13],
      NSForegroundColorAttributeName : NSColor.secondaryLabelColor,
      NSParagraphStyleAttributeName : para,
    }];
  }
  NSMutableAttributedString *as =
      [[NSMutableAttributedString alloc] initWithString:name attributes:@{
        NSFontAttributeName : [NSFont systemFontOfSize:13],
        NSForegroundColorAttributeName : NSColor.secondaryLabelColor,
        NSParagraphStyleAttributeName : para,
      }];
  NSMutableIndexSet *hits = [self highlightRangesForName:name];
  [hits enumerateRangesInRange:NSMakeRange(0, name.length)
                       options:0
                    usingBlock:^(NSRange r, BOOL *stop) {
                      (void)stop;
                      [as addAttribute:NSFontAttributeName
                                 value:[NSFont systemFontOfSize:13 weight:NSFontWeightSemibold]
                                 range:r];
                      [as addAttribute:NSForegroundColorAttributeName
                                 value:NSColor.labelColor
                                 range:r];
                    }];
  return as;
}

- (NSImage *)iconFor:(MERow *)item {
  if (!self.iconCache) self.iconCache = [NSMutableDictionary dictionary];
  NSString *ext = item.path.pathExtension.lowercaseString;
  NSString *key = item.isDir ? @"__dir" : (ext.length ? ext : @"__file");
  NSImage *img = self.iconCache[key];
  if (img) return img;
  UTType *type = nil;
  if (!item.isDir && ext.length) type = [UTType typeWithFilenameExtension:ext];
  if (!type) type = [UTType typeWithIdentifier:item.isDir ? @"public.folder" : @"public.data"];
  if (!type) type = [UTType typeWithIdentifier:@"public.item"];
  img = [NSWorkspace.sharedWorkspace iconForContentType:type];
  img.size = NSMakeSize(16, 16);
  self.iconCache[key] = img;
  return img;
}

- (NSView *)tableView:(NSTableView *)tableView viewForTableColumn:(NSTableColumn *)column row:(NSInteger)row {
  /* 越界行渲染空内容，不返回 nil（nil 会触发单元格兜底路径） */
  BOOL rowValid = row >= 0 && (NSUInteger)row < self.rows.count;
  MERow *item = rowValid ? self.rows[(NSUInteger)row] : [MERow new];
  NSString *ident = column.identifier;
  NSTableCellView *cell = [tableView makeViewWithIdentifier:ident owner:self];
  if (!cell) {
    cell = [[NSTableCellView alloc] initWithFrame:NSZeroRect];
    cell.identifier = ident;
    BOOL isName = [ident isEqualToString:@"name"];
    NSTextField *tf = [NSTextField labelWithString:@""];
    tf.tag = 1;
    tf.translatesAutoresizingMaskIntoConstraints = NO;
    tf.lineBreakMode = [ident isEqualToString:@"path"] ? NSLineBreakByTruncatingMiddle
                                                       : NSLineBreakByTruncatingTail;
    [cell addSubview:tf];
    if (isName) {
      tf.maximumNumberOfLines = 1;
      tf.cell.wraps = NO;
      NSImageView *iv = [NSImageView new];
      iv.tag = 2;
      iv.translatesAutoresizingMaskIntoConstraints = NO;
      [cell addSubview:iv];
      NSLayoutConstraint *lead =
          [tf.leadingAnchor constraintEqualToAnchor:iv.trailingAnchor constant:6];
      NSImage *ximg = [NSImage imageWithSystemSymbolName:@"xmark.circle"
                                  accessibilityDescription:@"删除该搜索历史"];
      NSButton *del = [NSButton buttonWithImage:ximg target:self action:@selector(deleteHistoryTerm:)];
      del.tag = 3;
      del.bordered = NO;
      del.hidden = YES;
      del.translatesAutoresizingMaskIntoConstraints = NO;
      [cell addSubview:del];
      NSLayoutConstraint *delTrail =
          [del.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor constant:-4];
      NSImageView *pinIv = [NSImageView new];
      pinIv.tag = 4;
      pinIv.translatesAutoresizingMaskIntoConstraints = NO;
      pinIv.hidden = YES;
      NSImage *pinImg = [NSImage imageWithSystemSymbolName:@"pin.fill" accessibilityDescription:nil];
      pinImg.size = NSMakeSize(14, 14);
      pinIv.image = pinImg;
      [cell addSubview:pinIv];
      [NSLayoutConstraint activateConstraints:@[
        [iv.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor constant:2],
        [iv.widthAnchor constraintEqualToAnchor:cell.heightAnchor],
        [iv.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
        lead,
        [del.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
        [del.widthAnchor constraintEqualToConstant:16],
        [del.heightAnchor constraintEqualToConstant:16],
        delTrail,
        [pinIv.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
        [pinIv.widthAnchor constraintEqualToConstant:14],
        [pinIv.heightAnchor constraintEqualToConstant:14],
        [pinIv.trailingAnchor constraintEqualToAnchor:iv.leadingAnchor constant:-5],
      ]];
    } else {
      [NSLayoutConstraint activateConstraints:@[
        [tf.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor constant:4],
      ]];
    }
    [NSLayoutConstraint activateConstraints:@[
      [tf.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor constant:-4],
      [tf.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
    ]];
  }
  NSTextField *tf = [cell viewWithTag:1];
  NSImageView *iv = [cell viewWithTag:2];
  NSButton *del = [cell viewWithTag:3];
  NSImageView *pinIv = [cell viewWithTag:4];
  del.hidden = !item.isTerm;
  pinIv.hidden = !(rowValid && item.pinned);
  /* 兜底防御：复用/外来单元格若带格式器，setStringValue 会触发格式化异常 */
  if (tf.formatter) tf.formatter = nil;
  BOOL isNameCol = [ident isEqualToString:@"name"];
  if (isNameCol) {
    /* 历史页文件行按层级缩进名称列（图标与文字一起偏移）。
       viewForTableColumn 在布局事务内被调用，约束不能就地改，延迟到下一轮 runloop。 */
    CGFloat leadWanted = 6 + 18.0 * item.indent;
    CGFloat trailWanted = -4 - (item.isTerm ? 22 : 0);
    CGFloat ivWanted = 2 + 18.0 * item.indent;
    NSLayoutConstraint *leadC = nil, *trailC = nil, *ivLeadC = nil;
    for (NSLayoutConstraint *c in cell.constraints) {
      if (c.firstItem == tf && c.firstAttribute == NSLayoutAttributeLeading) leadC = c;
      if (c.firstItem == tf && c.firstAttribute == NSLayoutAttributeTrailing) trailC = c;
      if (c.firstItem == iv && c.firstAttribute == NSLayoutAttributeLeading) ivLeadC = c;
    }
    if (leadC.constant != leadWanted || trailC.constant != trailWanted ||
        ivLeadC.constant != ivWanted) {
      dispatch_async(dispatch_get_main_queue(), ^{
        leadC.constant = leadWanted;
        trailC.constant = trailWanted;
        ivLeadC.constant = ivWanted;
      });
    }
  }
  if (item.isTerm) {
    if (isNameCol) {
      NSMutableAttributedString *as = [[NSMutableAttributedString alloc]
          initWithString:[NSString stringWithFormat:@"🔍 %@　", item.term]
              attributes:@{
                NSFontAttributeName : [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold],
                NSForegroundColorAttributeName : NSColor.labelColor,
              }];
      [as appendAttributedString:[[NSAttributedString alloc]
                                      initWithString:[NSString stringWithFormat:@"%lu 个文件",
                                                                                (unsigned long)item.termCount]
                                          attributes:@{
                                            NSFontAttributeName : [NSFont systemFontOfSize:12],
                                            NSForegroundColorAttributeName : NSColor.secondaryLabelColor,
                                          }]];
      tf.attributedStringValue = as;
      NSImage *icon = [NSImage imageWithSystemSymbolName:@"clock.arrow.circlepath"
                                  accessibilityDescription:@"搜索历史"];
      icon.size = NSMakeSize(15, 15);
      iv.image = icon;
    } else {
      tf.stringValue = @"";
      iv.image = nil;
    }
  } else if (isNameCol) {
    tf.attributedStringValue = [self highlightedName:item.name];
    iv.image = [self iconFor:item];
  } else if ([ident isEqualToString:@"path"]) {
    tf.stringValue = item.isTerm ? @"" : (item.path ?: @"");
    tf.textColor = NSColor.secondaryLabelColor;
  } else if ([ident isEqualToString:@"size"]) {
    tf.stringValue = item.isTerm ? @"" : (item.sizeText ?: @"");
    tf.textColor = NSColor.secondaryLabelColor;
  } else {
    tf.stringValue = item.isTerm ? @"" : (item.timeText ?: @"");
    tf.textColor = NSColor.secondaryLabelColor;
  }
  cell.toolTip = (rowValid && !item.isTerm) ? item.path : nil;
  return cell;
}

- (void)tableView:(NSTableView *)tableView sortDescriptorsDidChange:(NSArray<NSSortDescriptor *> *)oldDescriptors {
  (void)oldDescriptors;
  NSSortDescriptor *d = tableView.sortDescriptors.firstObject;
  if (!d) return;
  NSInteger key = ME_SORT_NAME;
  if ([d.key isEqualToString:@"path"]) key = ME_SORT_PATH;
  else if ([d.key isEqualToString:@"size"]) key = ME_SORT_SIZE;
  else if ([d.key isEqualToString:@"mtime"]) key = ME_SORT_MTIME;
  self.meSortKey = key;
  self.meSortAsc = d.ascending;
  self.searchLoading = NO;
  [self scheduleSearch];
}

- (void)open:(id)sender {
  (void)sender;
  [self revealOrOpen:NO];
}

- (void)reveal:(id)sender {
  (void)sender;
  [self revealOrOpen:YES];
}

- (void)revealOrOpen:(BOOL)reveal {
  NSInteger row = self.table.selectedRow;
  if (row < 0 && self.rows.count > 0) row = 0;
  if (row < 0 || (NSUInteger)row >= self.rows.count) return;
  MERow *item = self.rows[(NSUInteger)row];
  if (item.isTerm) {
    [self searchForTerm:item.term];
    return;
  }
  [self recordHistory:self.field.stringValue row:item pinned:NO];
  NSString *path = item.path;
  NSURL *url = [NSURL fileURLWithPath:path];
  if (reveal) [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[url]];
  else [NSWorkspace.sharedWorkspace openURL:url];
  /* 不主动压窗：目标应用激活后自然盖在本窗口上面，本窗口保持可见 */
}

- (BOOL)control:(NSControl *)control textView:(NSTextView *)textView doCommandBySelector:(SEL)command {
  (void)control;
  (void)textView;
  if (command == @selector(insertNewline:)) {
    // 回车不再打开文件；历史页词条行回车=重新搜索；⌘回车在 Finder 中显示
    NSInteger row = self.table.selectedRow;
    if (row >= 0 && (NSUInteger)row < self.rows.count && self.rows[(NSUInteger)row].isTerm) {
      [self searchForTerm:self.rows[(NSUInteger)row].term];
      return YES;
    }
    if ((NSApp.currentEvent.modifierFlags & NSEventModifierFlagCommand) != 0) {
      [self revealOrOpen:YES];
    }
    return YES;
  }
  if (command == @selector(cancelOperation:)) {
    [self.window orderOut:nil];
    return YES;
  }
  if (command == @selector(moveDown:) || command == @selector(moveUp:)) {
    NSUInteger nrows = self.rows.count;
    if (nrows == 0) return YES;
    NSInteger row = self.table.selectedRow;
    if (command == @selector(moveDown:)) {
      if (row >= 0 && (NSUInteger)row + 1 >= nrows) [self loadMoreSearchIfNeeded];
      row = row < 0 ? 0 : MIN(row + 1, (NSInteger)nrows - 1);
    } else {
      row = row <= 0 ? 0 : row - 1;
    }
    [self.table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row] byExtendingSelection:NO];
    [self.table scrollRowToVisible:row];
    return YES;
  }
  return NO;
}

- (void)controlTextDidChange:(NSNotification *)obj {
  (void)obj;
  [self scheduleSearch];
}

- (void)searchFieldDidEndSearching:(NSSearchField *)sender {
  (void)sender;
  [self scheduleSearch];
}

- (void)reindex {
  if (!self.rootPath.length || self.building || me_index_scanning(self.idx)) return;
  self.ready = NO;
  NSString *root = self.rootPath;
  NSString *indexPath = self.indexPath;
  MEIndex *old = self.idx;
  self.idx = NULL;
  dispatch_async(self.searchQ, ^{
    if (old) me_index_free(old);
    MEIndex *fresh = me_index_create();
    dispatch_async(dispatch_get_main_queue(), ^{
      self.idx = fresh;
    });
    MEScanStats stats;
    memset(&stats, 0, sizeof stats);
    dispatch_async(dispatch_get_main_queue(), ^{
      self.building = fresh;
    });
    me_index_scan(fresh, root.UTF8String, &stats);
    dispatch_sync(self.saveQ, ^{
      me_index_set_complete(fresh, 1);
      if (indexPath.length) me_index_save(fresh, indexPath.UTF8String);
    });
    BOOL skipped = stats.skipped_protected != 0;
    me_index_watch_start(fresh, OnIndexChange, NULL);
    dispatch_async(dispatch_get_main_queue(), ^{
      self.building = NULL;
      self.idx = fresh;
      self.ready = YES;
      self.skippedProtected = skipped;
      self.fdaGrantedCached = !skipped;
      self.fdaStateKnown = YES;
      [self tick];
      if (self.window.isVisible) [self scheduleSearch];
    });
  });
}

- (void)openPrivacy {
  /* 只负责跳转系统设置；用户拨动开关后，系统自己会弹出"退出并重新打开"提示 */
  NSURL *modern = [NSURL URLWithString:@"x-apple.systempreferences:com.apple.settings.PrivacySecurity.extension?Privacy_AllFiles"];
  NSURL *legacy = [NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles"];
  if (![NSWorkspace.sharedWorkspace openURL:modern]) [NSWorkspace.sharedWorkspace openURL:legacy];
  [self refreshFDAStateAsync];
}

static NSArray<NSString *> *VersionParts(NSString *s) {
  if (s.length && ([s hasPrefix:@"v"] || [s hasPrefix:@"V"])) s = [s substringFromIndex:1];
  return [s componentsSeparatedByString:@"."];
}

static NSComparisonResult CompareVersions(NSString *local, NSString *remote) {
  NSArray<NSString *> *a = VersionParts(local);
  NSArray<NSString *> *b = VersionParts(remote);
  NSUInteger n = MAX(a.count, b.count);
  for (NSUInteger i = 0; i < n; i++) {
    NSInteger av = i < a.count ? a[i].integerValue : 0;
    NSInteger bv = i < b.count ? b[i].integerValue : 0;
    if (av < bv) return NSOrderedAscending;
    if (av > bv) return NSOrderedDescending;
  }
  return NSOrderedSame;
}

static NSString *CurrentVersion(void) {
  NSString *v = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
  return v.length ? v : @"0";
}

- (void)showUpdateAlert:(NSString *)title detail:(NSString *)detail download:(NSString *)download {
  NSAlert *alert = [NSAlert new];
  alert.messageText = title;
  alert.informativeText = detail ?: @"";
  if (download.length) {
    [alert addButtonWithTitle:@"下载更新"];
    [alert addButtonWithTitle:@"稍后"];
  } else {
    [alert addButtonWithTitle:@"好"];
  }
  [NSApp activateIgnoringOtherApps:YES];
  NSModalResponse response = [alert runModal];
  if (download.length && response == NSAlertFirstButtonReturn) {
    NSURL *url = [NSURL URLWithString:download];
    if (url) [NSWorkspace.sharedWorkspace openURL:url];
  }
}

- (void)remindCachedUpdateForce:(BOOL)force {
  NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
  NSString *latest = [d stringForKey:@"MELatestVersion"];
  NSString *url = [d stringForKey:@"MELatestURL"];
  if (!latest.length || CompareVersions(CurrentVersion(), latest) != NSOrderedAscending) {
    if (force) {
      [self showUpdateAlert:@"当前已是最新版本" detail:[NSString stringWithFormat:@"正在使用 %@", CurrentVersion()] download:nil];
    }
    return;
  }
  if (!force) {
    NSDate *last = [d objectForKey:@"MELastUpdateRemind"];
    if (last && [NSDate.date timeIntervalSinceDate:last] < 22 * 3600) return;
  }
  [d setObject:NSDate.date forKey:@"MELastUpdateRemind"];
  NSString *page = url.length ? url : @"https://github.com/Marovlo/macEverything/releases/latest";
  [self showUpdateAlert:[NSString stringWithFormat:@"发现新版本 %@", latest]
                  detail:[NSString stringWithFormat:@"当前版本是 %@。下载安装包后，把新的 MacEverything 拖进「应用程序」替换旧的。", CurrentVersion()]
                download:page];
}

- (void)checkForUpdatesForce:(BOOL)force {
  if (self.updateCheckInFlight) return;
  NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
  NSDate *lastCheck = [d objectForKey:@"MELastUpdateCheck"];
  BOOL due = force || !lastCheck || [NSDate.date timeIntervalSinceDate:lastCheck] >= 22 * 3600;
  if (!due) {
    [self remindCachedUpdateForce:NO];
    return;
  }
  self.updateCheckInFlight = YES;
  NSURL *api = [NSURL URLWithString:@"https://api.github.com/repos/Marovlo/macEverything/releases/latest"];
  NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:api];
  [req setValue:@"MacEverything" forHTTPHeaderField:@"User-Agent"];
  [req setValue:@"application/vnd.github+json" forHTTPHeaderField:@"Accept"];
  [[[NSURLSession sharedSession] dataTaskWithRequest:req completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
    dispatch_async(dispatch_get_main_queue(), ^{
      self.updateCheckInFlight = NO;
      NSHTTPURLResponse *http = [response isKindOfClass:[NSHTTPURLResponse class]] ? (NSHTTPURLResponse *)response : nil;
      if (error || !data || http.statusCode != 200) {
        if (force) [self showUpdateAlert:@"暂时无法检查更新" detail:@"请稍后再试。" download:nil];
        else [self remindCachedUpdateForce:NO];
        return;
      }
      id json = [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL];
      if (![json isKindOfClass:[NSDictionary class]]) {
        if (force) [self showUpdateAlert:@"暂时无法检查更新" detail:@"请稍后再试。" download:nil];
        return;
      }
      [d setObject:NSDate.date forKey:@"MELastUpdateCheck"];
      NSString *tag = json[@"tag_name"];
      NSString *page = json[@"html_url"];
      NSString *dmg = [page isKindOfClass:[NSString class]] ? page : nil;
      NSArray *assets = json[@"assets"];
      if ([assets isKindOfClass:[NSArray class]]) {
        for (id asset in assets) {
          if (![asset isKindOfClass:[NSDictionary class]]) continue;
          NSString *name = asset[@"name"];
          NSString *file = asset[@"browser_download_url"];
          if ([name isKindOfClass:[NSString class]] && [file isKindOfClass:[NSString class]] &&
              [name.lowercaseString hasSuffix:@".dmg"]) {
            dmg = file;
            break;
          }
        }
      }
      if ([tag isKindOfClass:[NSString class]] && tag.length) {
        [d setObject:tag forKey:@"MELatestVersion"];
        if (dmg.length) [d setObject:dmg forKey:@"MELatestURL"];
      }
      [self remindCachedUpdateForce:force];
    });
  }] resume];
}

- (void)checkForUpdates {
  [self checkForUpdatesForce:NO];
}

- (void)checkForUpdatesNow:(id)sender {
  (void)sender;
  [self checkForUpdatesForce:YES];
}

- (void)quit {
  if (self.quitting) return;
  self.quitting = YES;
  MEIndex *idx = self.idx;
  MEIndex *building = self.building;
  NSString *path = self.indexPath;
  BOOL skip = self.skipSave || path.length == 0;
  dispatch_async(self.saveQ, ^{
    if (!skip) {
      if (building && me_index_scanning(building) && (!idx || idx == building || !me_index_complete(idx))) {
        me_index_save(building, path.UTF8String);
      } else if (idx) {
        me_index_save(idx, path.UTF8String);
      }
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      self.skipSave = YES;
      [NSApp terminate:nil];
    });
  });
}

- (void)saveIndex {
  if (self.skipSave || !self.indexPath.length) return;
  MEIndex *idx = self.idx;
  MEIndex *building = self.building;
  NSString *path = self.indexPath;
  dispatch_sync(self.saveQ, ^{
    if (building && me_index_scanning(building) && (!idx || idx == building || !me_index_complete(idx))) {
      me_index_save(building, path.UTF8String);
    } else if (idx) {
      me_index_save(idx, path.UTF8String);
    }
  });
}

- (void)applicationWillTerminate:(NSNotification *)note {
  (void)note;
  if (!self.skipSave) [self saveIndex];
  free(self.resultIds);
  self.resultIds = NULL;
  if (self.idx) {
    me_index_free(self.idx);
    self.idx = NULL;
  }
}

@end

static int Smoke(void) {
  const char *index = getenv("ME_INDEX");
  const char *root = getenv("ME_ROOT");
  const char *query = getenv("ME_QUERY");
  if (!query) query = "a";
  MEIndex *idx = NULL;
  if (index && access(index, R_OK) == 0) idx = me_index_load(index);
  if (!idx) {
    if (!root) {
      fprintf(stderr, "SMOKE missing index\n");
      return 1;
    }
    idx = me_index_create();
    if (me_index_scan(idx, root, NULL) != 0) {
      fprintf(stderr, "SMOKE scan failed: %s\n", me_last_error());
      return 1;
    }
  }
  uint32_t ids[8];
  uint32_t count = 0, total = 0;
  double seconds = 0;
  if (me_index_search(idx, query, 8, ids, &count, &total, &seconds) != 0) return 1;
  printf("SMOKE total=%u ms=%.3f mapped=%d\n", total, seconds * 1000.0, me_index_is_mapped(idx));
  fflush(stdout);
  me_index_free(idx);
  return total > 0 ? 0 : 1;
}

@implementation MEScrollView
- (void)reflectScrolledClipView:(NSClipView *)clipView {
  [super reflectScrolledClipView:clipView];
  [self.owner loadMoreSearchIfNeeded];
}
@end

@implementation METableView
- (void)copy:(id)sender {
  if (self.owner) [self.owner copyPath:sender];
}
/* 右键弹出菜单前，先选中鼠标所在行并同步菜单项可用状态。
   NSTableView 右键不保证改变选中行，且新系统的菜单校验时机不可靠。 */
- (NSMenu *)menuForEvent:(NSEvent *)event {
  if (self.owner) [self.owner prepareContextMenuForEvent:event inTable:self];
  return [super menuForEvent:event];
}
@end

int me_app_main(int argc, const char **argv) {
  (void)argc;
  (void)argv;
  @autoreleasepool {
    if (getenv("ME_SMOKE")) return Smoke();
    NSApplication *app = [NSApplication sharedApplication];
    AppDelegate *delegate = [AppDelegate new];
    app.delegate = delegate;
    [app run];
  }
  return 0;
}
