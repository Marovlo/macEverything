#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>

#include "me.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static _Atomic uint64_t g_epoch = 0;

@interface MERow : NSObject
@property(copy) NSString *name;
@property(copy) NSString *path;
@property(copy) NSString *sizeText;
@property(copy) NSString *timeText;
@end

@implementation MERow
@end

@interface AppDelegate : NSObject <NSApplicationDelegate, NSTableViewDataSource, NSTableViewDelegate, NSWindowDelegate, NSTextFieldDelegate, NSSearchFieldDelegate>
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
@property BOOL skippedProtected;
@property(strong) NSScrollView *listScroll;
@property dispatch_queue_t listQ;
@property uint32_t *sortedIds;
@property uint32_t sortedCount;
@property uint64_t sortedGen;
@property(nonatomic, assign) MEIndex *sortedIdx;
@property BOOL sorting;
@property BOOL sortAgain;
@property BOOL showAll;
@property NSUInteger windowStart;
@property NSInteger scrollDelta;
@property BOOL scrollScheduled;
@property BOOL adjustingScroller;
@property uint64_t seenGeneration;
@property(strong) NSMenuItem *countItem;
@end

static AppDelegate *g_app = nil;

@class AppDelegate;
@interface MEScrollView : NSScrollView
@property(nonatomic, weak) AppDelegate *owner;
@end

@implementation AppDelegate

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
    fmt.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    fmt.dateFormat = @"yyyy-MM-dd HH:mm";
  });
  return [fmt stringFromDate:[NSDate dateWithTimeIntervalSince1970:sec]];
}

static void OnIndexChange(void *ctx) {
  (void)ctx;
  dispatch_async(dispatch_get_main_queue(), ^{
    [g_app refreshIfVisible];
  });
}

- (void)applicationDidFinishLaunching:(NSNotification *)note {
  (void)note;
  g_app = self;
  [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
  self.rows = [NSMutableArray array];
  self.searchQ = dispatch_queue_create("maceverything.search", DISPATCH_QUEUE_SERIAL);
  self.saveQ = dispatch_queue_create("maceverything.save", DISPATCH_QUEUE_SERIAL);
  self.listQ = dispatch_queue_create("maceverything.list", DISPATCH_QUEUE_SERIAL);
  [self buildStatus];
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

- (void)useIndex:(MEIndex *)idx {
  dispatch_async(dispatch_get_main_queue(), ^{
    self.idx = idx;
    self.ready = YES;
    self.seenGeneration = me_index_generation(idx);
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
    self.seenGeneration = me_index_generation(fresh);
    [self tick];
    if (self.window.isVisible) [self scheduleSearch];
    if (old && old != fresh) {
      dispatch_async(self.searchQ, ^{
        dispatch_sync(self.listQ, ^{});
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
  self.status = [NSStatusBar.systemStatusBar statusItemWithLength:NSSquareStatusItemLength];
  NSImage *appIcon = nil;
  NSString *iconPath = [NSBundle.mainBundle pathForResource:@"AppIcon" ofType:@"icns"];
  if (iconPath.length) appIcon = [[NSImage alloc] initWithContentsOfFile:iconPath];
  if (appIcon) [NSApp setApplicationIconImage:appIcon];
  NSImage *image = nil;
  NSString *barPath = [NSBundle.mainBundle pathForResource:@"MenubarIcon" ofType:@"png"];
  if (barPath.length) image = [[NSImage alloc] initWithContentsOfFile:barPath];
  if (!image) image = [NSImage imageWithSystemSymbolName:@"magnifyingglass" accessibilityDescription:@"MacEverything"];
  image.template = YES;
  image.size = NSMakeSize(18, 18);
  self.status.button.image = image;
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
  if (self.window.isVisible && self.window.isKeyWindow) [self.window orderOut:nil];
  else [self showWindow];
}

- (void)buildWindow {
  NSRect frame = NSMakeRect(0, 0, 920, 560);
  self.window = [[NSWindow alloc] initWithContentRect:frame
                                            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                      NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                              backing:NSBackingStoreBuffered
                                                defer:NO];
  self.window.title = @"MacEverything";
  self.window.level = NSFloatingWindowLevel;
  self.window.delegate = self;
  self.window.collectionBehavior = NSWindowCollectionBehaviorMoveToActiveSpace | NSWindowCollectionBehaviorFullScreenAuxiliary;
  [self.window center];
  NSView *content = self.window.contentView;
  self.field = [NSSearchField new];
  self.field.placeholderString = @"文件名";
  self.field.delegate = self;
  self.field.font = [NSFont systemFontOfSize:16];
  self.field.sendsSearchStringImmediately = NO;
  [content addSubview:self.field];

  MEScrollView *scroll = [MEScrollView new];
  scroll.owner = self;
  scroll.hasVerticalScroller = YES;
  scroll.autohidesScrollers = NO;
  self.listScroll = scroll;
  self.table = [[NSTableView alloc] initWithFrame:NSZeroRect];
  self.table.headerView = nil;
  self.table.rowHeight = 22;
  self.table.usesAlternatingRowBackgroundColors = YES;
  self.table.doubleAction = @selector(open:);
  self.table.target = self;
  self.table.dataSource = self;
  self.table.delegate = self;
  NSArray *titles = @[@"名称", @"路径", @"大小", @"修改时间"];
  NSArray *ids = @[@"name", @"path", @"size", @"time"];
  CGFloat widths[] = {180, 480, 80, 140};
  for (NSUInteger i = 0; i < titles.count; i++) {
    NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:ids[i]];
    col.title = titles[i];
    col.width = widths[i];
    [self.table addTableColumn:col];
  }
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
  RegisterEventHotKey(kVK_Space, controlKey | optionKey, hid, GetApplicationEventTarget(), 0, &ref);
  EventTypeSpec spec = {kEventClassKeyboard, kEventHotKeyPressed};
  InstallApplicationEventHandler(HotkeyHandler, 1, &spec, NULL, NULL);
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
  (void)sender;
  return NO;
}

- (BOOL)windowShouldClose:(NSWindow *)sender {
  [sender orderOut:nil];
  return NO;
}

- (void)showWindow {
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
  [self.window makeFirstResponder:self.field];
  [self scheduleSearch];
}

- (void)tick {
  MEIndex *idx = self.idx;
  if (!idx) return;
  MEIndex *building = self.building;
  if (building && me_index_scanning(building)) {
    unsigned long long n = (unsigned long long)me_index_progress(building);
    self.countItem.title = [NSString stringWithFormat:@"正在索引 %llu 个名称", n];
    self.status.button.title = [NSString stringWithFormat:@" %llu", n];
    if (self.window.isVisible && (!self.ready || self.idx != building)) {
      self.footer.stringValue = [NSString stringWithFormat:@"正在索引 %llu 个名称…", n];
    }
  } else if (me_index_scanning(idx)) {
    unsigned long long n = (unsigned long long)me_index_progress(idx);
    self.countItem.title = [NSString stringWithFormat:@"正在索引 %llu 个名称", n];
    self.status.button.title = [NSString stringWithFormat:@" %llu", n];
    if (self.window.isVisible) self.footer.stringValue = [NSString stringWithFormat:@"正在索引 %llu 个名称…", n];
  } else if (self.ready) {
    self.countItem.title = [NSString stringWithFormat:@"已索引 %u 个名称", me_index_live(idx)];
    self.status.button.title = @"";
  }
  if (self.window.isVisible) {
    uint64_t gen = me_index_generation(idx);
    if (gen != self.seenGeneration) {
      self.seenGeneration = gen;
      [self scheduleSearch];
    }
  }
}

- (void)refreshIfVisible {
  if (self.window.isVisible) {
    if (self.field.stringValue.length == 0) [self scheduleSort];
    else [self scheduleSearch];
  }
  [self tick];
}

- (NSUInteger)pageSize {
  CGFloat height = self.listScroll.contentSize.height;
  CGFloat row = self.table.rowHeight > 1 ? self.table.rowHeight : 22;
  NSUInteger n = height > 1 ? (NSUInteger)(height / row) : 24;
  if (n < 8) n = 8;
  return n + 1;
}

- (void)syncScroller {
  if (!self.showAll || self.adjustingScroller) return;
  NSScroller *scroller = self.listScroll.verticalScroller;
  scroller.target = self;
  scroller.action = @selector(scrollerMoved:);
  NSUInteger total = self.sortedCount;
  NSUInteger page = self.rows.count;
  self.adjustingScroller = YES;
  if (total <= page || total == 0) {
    scroller.knobProportion = 1;
    scroller.doubleValue = 0;
  } else {
    scroller.knobProportion = MIN(1.0, (CGFloat)page / (CGFloat)total);
    scroller.doubleValue = (CGFloat)self.windowStart / (CGFloat)(total - page);
  }
  self.adjustingScroller = NO;
}

- (MERow *)rowForNode:(uint32_t)node {
  MEInfo info;
  if (!self.idx || me_index_info(self.idx, node, &info) != 0) return nil;
  MERow *row = [MERow new];
  row.name = info.is_dir ? [NSString stringWithFormat:@"%@/", @(info.name)] : @(info.name);
  row.path = @(info.path);
  row.sizeText = info.is_dir ? @"" : FormatSize(info.size);
  row.timeText = FormatTime(info.mtime);
  return row;
}

- (void)fillPage {
  NSUInteger total = self.sortedCount;
  NSUInteger page = [self pageSize];
  if (self.windowStart >= total) self.windowStart = 0;
  if (total > page && self.windowStart + page > total) self.windowStart = total - page;
  NSUInteger end = MIN(total, self.windowStart + page);
  NSMutableArray<MERow *> *rows = [NSMutableArray arrayWithCapacity:end - self.windowStart];
  for (NSUInteger i = self.windowStart; i < end; i++) {
    MERow *row = [self rowForNode:self.sortedIds[i]];
    if (row) [rows addObject:row];
  }
  self.rows = rows;
  [self.table reloadData];
  NSString *extra = self.skippedProtected ? @"  · 桌面、文稿、下载需要完全磁盘访问" : @"";
  self.footer.stringValue = [NSString stringWithFormat:@"匹配 %lu 个%@", (unsigned long)total, extra];
  [self syncScroller];
}

- (void)ensureSorted {
  MEIndex *idx = self.idx;
  if (!idx || self.sorting) return;
  uint64_t gen = me_index_generation(idx);
  if (self.sortedIds && self.sortedIdx == idx && self.sortedGen == gen) return;
  self.sorting = YES;
  dispatch_async(self.listQ, ^{
    uint32_t live = me_index_live(idx);
    uint32_t *ids = live ? malloc((size_t)live * sizeof(uint32_t)) : NULL;
    uint32_t n = 0;
    if (live && (!ids || me_index_list(idx, ids, live, &n, 1) != 0)) {
      free(ids);
      ids = NULL;
      n = 0;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      self.sorting = NO;
      if (self.idx != idx) {
        free(ids);
        if (self.idx) [self ensureSorted];
        return;
      }
      BOOL showing = self.showAll && self.sortedIds != NULL && self.field.stringValue.length == 0;
      NSUInteger keep = self.windowStart;
      free(self.sortedIds);
      self.sortedIds = ids;
      self.sortedCount = n;
      self.sortedGen = gen;
      self.sortedIdx = idx;
      if (self.field.stringValue.length == 0) {
        self.showAll = YES;
        self.windowStart = showing ? keep : 0;
        [self fillPage];
      }
      if (me_index_generation(idx) != gen) [self scheduleSort];
    });
  });
}

- (void)scheduleSort {
  MEIndex *idx = self.idx;
  if (!idx || self.sorting || self.sortAgain) return;
  uint64_t gen = me_index_generation(idx);
  if (self.sortedIds && self.sortedIdx == idx && self.sortedGen == gen) return;
  if (!self.sortedIds || self.sortedIdx != idx) {
    [self ensureSorted];
    return;
  }
  self.sortAgain = YES;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
    self.sortAgain = NO;
    [self ensureSorted];
  });
}

- (void)showAllFiles {
  atomic_fetch_add(&g_epoch, 1);
  self.showAll = YES;
  MEIndex *idx = self.idx;
  uint64_t gen = idx ? me_index_generation(idx) : 0;
  if (!self.sortedIds || self.sortedIdx != idx) {
    self.footer.stringValue = @"正在按名称排列…";
    [self ensureSorted];
    return;
  }
  self.windowStart = 0;
  [self fillPage];
  if (self.sortedGen != gen) [self scheduleSort];
}

- (void)moveWindowBy:(NSInteger)delta {
  if (!self.showAll || self.sortedCount == 0) return;
  NSUInteger page = [self pageSize];
  NSUInteger maxStart = self.sortedCount > page ? self.sortedCount - page : 0;
  NSInteger next = (NSInteger)self.windowStart + delta;
  if (next < 0) next = 0;
  if (next > (NSInteger)maxStart) next = (NSInteger)maxStart;
  if ((NSUInteger)next == self.windowStart) return;
  self.windowStart = (NSUInteger)next;
  [self fillPage];
}

- (BOOL)handleListScroll:(NSEvent *)event {
  if (!self.showAll || self.sortedCount <= [self pageSize]) return NO;
  CGFloat dy = event.scrollingDeltaY;
  NSInteger step = 0;
  if (event.hasPreciseScrollingDeltas) step = (NSInteger)llround(-dy / self.table.rowHeight);
  else step = dy > 0 ? -3 : 3;
  if (step == 0) step = dy > 0 ? -1 : 1;
  self.scrollDelta += step;
  if (!self.scrollScheduled) {
    self.scrollScheduled = YES;
    dispatch_async(dispatch_get_main_queue(), ^{
      self.scrollScheduled = NO;
      NSInteger delta = self.scrollDelta;
      self.scrollDelta = 0;
      [self moveWindowBy:delta];
    });
  }
  return YES;
}

- (void)scrollerMoved:(NSScroller *)scroller {
  if (!self.showAll || self.adjustingScroller) return;
  NSUInteger page = [self pageSize];
  if (self.sortedCount <= page) return;
  NSUInteger maxStart = self.sortedCount - page;
  self.windowStart = (NSUInteger)(scroller.doubleValue * (double)maxStart);
  [self fillPage];
}

- (void)windowDidResize:(NSNotification *)note {
  (void)note;
  if (self.showAll && self.field.stringValue.length == 0 && self.sortedIds) [self fillPage];
}

- (void)scheduleSearch {
  if (!self.ready || !self.idx) {
    self.footer.stringValue = me_index_scanning(self.idx) ? @"正在建立索引…" : @"正在准备索引";
    return;
  }
  if (self.field.stringValue.length == 0) {
    [self showAllFiles];
    return;
  }
  self.showAll = NO;
  self.listScroll.verticalScroller.target = nil;
  self.listScroll.verticalScroller.action = NULL;
  uint64_t epoch = atomic_fetch_add(&g_epoch, 1) + 1;
  NSString *query = self.field.stringValue ?: @"";
  MEIndex *idx = self.idx;
  dispatch_async(self.searchQ, ^{
    if (atomic_load(&g_epoch) != epoch) return;
    uint32_t ids[300];
    uint32_t count = 0, total = 0;
    double seconds = 0;
    me_index_search(idx, query.UTF8String, 300, ids, &count, &total, &seconds);
    if (atomic_load(&g_epoch) != epoch) return;
    MEInfo *infos = count ? calloc(count, sizeof(MEInfo)) : NULL;
    if (count) me_index_infos(idx, ids, count, infos);
    NSMutableArray<MERow *> *rows = [NSMutableArray arrayWithCapacity:count];
    for (uint32_t i = 0; i < count; i++) {
      MERow *row = [MERow new];
      row.name = infos[i].is_dir ? [NSString stringWithFormat:@"%@/", @(infos[i].name)] : @(infos[i].name);
      row.path = @(infos[i].path);
      row.sizeText = infos[i].is_dir ? @"" : FormatSize(infos[i].size);
      row.timeText = FormatTime(infos[i].mtime);
      [rows addObject:row];
    }
    free(infos);
    dispatch_async(dispatch_get_main_queue(), ^{
      if (atomic_load(&g_epoch) != epoch) return;
      self.showAll = NO;
      self.rows = rows;
      [self.table reloadData];
      if (total > count) {
        self.footer.stringValue =
            [NSString stringWithFormat:@"匹配 %u 个，显示前 %u 个 · %.1f ms", total, count, seconds * 1000.0];
      } else {
        self.footer.stringValue = [NSString stringWithFormat:@"匹配 %u 个 · %.1f ms", total, seconds * 1000.0];
      }
      self.seenGeneration = me_index_generation(idx);
    });
  });
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tableView {
  (void)tableView;
  return (NSInteger)self.rows.count;
}

- (id)tableView:(NSTableView *)tableView objectValueForTableColumn:(NSTableColumn *)column row:(NSInteger)row {
  (void)tableView;
  if (row < 0 || (NSUInteger)row >= self.rows.count) return @"";
  MERow *item = self.rows[(NSUInteger)row];
  NSString *ident = column.identifier;
  if ([ident isEqualToString:@"name"]) return item.name;
  if ([ident isEqualToString:@"path"]) return item.path;
  if ([ident isEqualToString:@"size"]) return item.sizeText;
  return item.timeText;
}

- (void)open:(id)sender {
  (void)sender;
  [self revealOrOpen:NO];
}

- (void)revealOrOpen:(BOOL)reveal {
  NSInteger row = self.table.selectedRow;
  if (row < 0 && self.rows.count > 0) row = 0;
  if (row < 0 || (NSUInteger)row >= self.rows.count) return;
  NSString *path = self.rows[(NSUInteger)row].path;
  NSURL *url = [NSURL fileURLWithPath:path];
  if (reveal) [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[url]];
  else [NSWorkspace.sharedWorkspace openURL:url];
}

- (BOOL)control:(NSControl *)control textView:(NSTextView *)textView doCommandBySelector:(SEL)command {
  (void)control;
  (void)textView;
  if (command == @selector(insertNewline:)) {
    BOOL reveal = (NSApp.currentEvent.modifierFlags & NSEventModifierFlagCommand) != 0;
    [self revealOrOpen:reveal];
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
      if (row >= 0 && (NSUInteger)row + 1 >= nrows && self.showAll) {
        [self moveWindowBy:1];
        if (self.rows.count > 0) {
          [self.table selectRowIndexes:[NSIndexSet indexSetWithIndex:self.rows.count - 1] byExtendingSelection:NO];
        }
        return YES;
      }
      row = row < 0 ? 0 : MIN(row + 1, (NSInteger)nrows - 1);
    } else {
      if (row <= 0 && self.showAll) {
        [self moveWindowBy:-1];
        [self.table selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];
        return YES;
      }
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
  if (!self.rootPath.length) return;
  self.ready = NO;
  NSString *root = self.rootPath;
  NSString *indexPath = self.indexPath;
  MEIndex *old = self.idx;
  self.idx = NULL;
  dispatch_async(self.searchQ, ^{
    dispatch_sync(self.listQ, ^{});
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
      [self tick];
      if (self.window.isVisible) [self scheduleSearch];
    });
  });
}

- (void)openPrivacy {
  [self showWindow];
  NSURL *modern = [NSURL URLWithString:@"x-apple.systempreferences:com.apple.settings.PrivacySecurity.extension?Privacy_AllFiles"];
  NSURL *legacy = [NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles"];
  if (![NSWorkspace.sharedWorkspace openURL:modern]) [NSWorkspace.sharedWorkspace openURL:legacy];
  NSAlert *alert = [NSAlert new];
  alert.messageText = @"完全磁盘访问要重新打开才生效";
  alert.informativeText = @"系统不会把刚授予的权限交给已经在运行的 MacEverything。点「重新打开」会退出并再启动，然后重新索引主目录。桌面、文稿、下载才会进入索引。";
  [alert addButtonWithTitle:@"重新打开"];
  [alert addButtonWithTitle:@"只打开设置"];
  [alert beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse response) {
    if (response == NSAlertFirstButtonReturn) [self relaunch];
  }];
}

- (void)relaunch {
  self.skipSave = YES;
  if (self.indexPath.length) {
    unlink(self.indexPath.UTF8String);
    unlink([self.indexPath stringByAppendingString:@".eid"].UTF8String);
  }
  NSWorkspaceOpenConfiguration *cfg = [NSWorkspaceOpenConfiguration configuration];
  cfg.createsNewApplicationInstance = YES;
  cfg.arguments = @[@"--rescan"];
  NSURL *url = NSBundle.mainBundle.bundleURL;
  [NSWorkspace.sharedWorkspace openApplicationAtURL:url configuration:cfg completionHandler:^(NSRunningApplication *app, NSError *error) {
    dispatch_async(dispatch_get_main_queue(), ^{
      if (!app) {
        self.skipSave = NO;
        self.footer.stringValue = error.localizedDescription ?: @"重新打开失败";
        return;
      }
      [NSApp terminate:nil];
    });
  }];
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
  [self saveIndex];
  [NSApp terminate:nil];
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
  free(self.sortedIds);
  self.sortedIds = NULL;
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
- (void)scrollWheel:(NSEvent *)event {
  if (self.owner && [self.owner handleListScroll:event]) return;
  [super scrollWheel:event];
}
- (void)reflectScrolledClipView:(NSClipView *)clipView {
  [super reflectScrolledClipView:clipView];
  [self.owner syncScroller];
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
