// setup_ui.m: the first-run experience of a drag-and-drop recomp app.
//
// The app ships the runtime and launcher but no game. Until the game has been built on this Mac, this window asks for the
// player's own game folder: dropped on the app's icon, dropped on this window, chosen in a panel, found by Search, or
// put in ~/Games/<Game>/Put Game Files Here. Once a folder checks out it offers setup, runs setup.py with the bundled tools, and
// restarts into the normal launcher. An update that changes the game code runs the same setup again on its own.
#import <Cocoa/Cocoa.h>
#import <CommonCrypto/CommonDigest.h>
#import "theme.h"
#include <dlfcn.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

static NSString *sha256_of(NSString *path)
{
    NSFileHandle *f = [NSFileHandle fileHandleForReadingAtPath:path];
    if (!f) return nil;
    CC_SHA256_CTX c; CC_SHA256_Init(&c);
    for (;;) { @autoreleasepool {
        NSData *d = [f readDataOfLength:1 << 20];
        if (!d.length) break;
        CC_SHA256_Update(&c, d.bytes, (CC_LONG)d.length);
    } }
    unsigned char out[CC_SHA256_DIGEST_LENGTH]; CC_SHA256_Final(out, &c);
    NSMutableString *s = [NSMutableString string];
    for (int k = 0; k < CC_SHA256_DIGEST_LENGTH; k++) [s appendFormat:@"%02x", out[k]];
    return s;
}
/* a path under `folder`, matching each name without regard to case, as Windows would */
static NSString *find_ci(NSString *folder, NSString *relative)
{
    NSString *here = folder;
    for (NSString *part in [relative componentsSeparatedByString:@"/"]) {
        NSString *hit = nil;
        for (NSString *n in [NSFileManager.defaultManager contentsOfDirectoryAtPath:here error:nil])
            if ([n caseInsensitiveCompare:part] == NSOrderedSame) { hit = n; break; }
        if (!hit) return nil;
        here = [here stringByAppendingPathComponent:hit];
    }
    return here;
}
static NSString *config_path;   /* the settings game_config read: setup.py is given the same */
static NSDictionary *game_config(void)
{
    /* one app with several games (a suite: make_suite.sh) has setup/<binary>.json per game binary; one game, setup/game.json */
    NSString *dir = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"setup"];
    NSString *p = [dir stringByAppendingPathComponent:[NSBundle.mainBundle.executablePath.lastPathComponent stringByAppendingPathExtension:@"json"]];
    if (![NSFileManager.defaultManager fileExistsAtPath:p]) p = [dir stringByAppendingPathComponent:@"game.json"];
    NSData *d = [NSData dataWithContentsOfFile:p]; config_path = p;
    return d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:nil] : nil;
}
static NSString *game_root(NSDictionary *cfg)
{
    return [[NSHomeDirectory() stringByAppendingPathComponent:@"Games"] stringByAppendingPathComponent:cfg[@"folder"]];
}
/* game.json's "inside": where the game sits in a store's wrapper that `here` may be (the Mac Steam releases of GTA III and
 * San Andreas: <game>.app/Contents/Resources/transgaming/c_drive/...); the folder holding the executable, or nil */
static NSString *inside_game(NSString *here, NSDictionary *cfg)
{
    for (NSString *rel in cfg[@"inside"]) { NSString *d = rel.length ? find_ci(here, rel) : here; if (d && find_ci(d, cfg[@"exe"])) return d; }   /* "": the app is the game (Homeworld) */
    return nil;
}
/* the folder at or under `path` (3 levels at most) holding the game's executable */
static NSString *locate_game(NSString *path, NSDictionary *cfg)
{
    NSMutableArray *queue = [NSMutableArray arrayWithObject:@[path, @0]];
    NSFileManager *fm = NSFileManager.defaultManager;
    while (queue.count) {
        NSString *here = queue[0][0]; int depth = [queue[0][1] intValue]; [queue removeObjectAtIndex:0];
        if (find_ci(here, cfg[@"exe"])) return here;
        NSString *in = inside_game(here, cfg); if (in) return in;
        if (depth >= 3) continue;
        for (NSString *n in [[fm contentsOfDirectoryAtPath:here error:nil] sortedArrayUsingSelector:@selector(compare:)]) {
            NSString *sub = [here stringByAppendingPathComponent:n]; BOOL dir = NO;
            if ([fm fileExistsAtPath:sub isDirectory:&dir] && dir) [queue addObject:@[sub, @(depth + 1)]];
        }
    }
    return nil;
}
/* game.json's exe_sha256: each accepted executable's hash -> its version's name, or {"version": name, "patch": [steps]}
 * (a store's copy whose wrapper setup.py takes off, on this Mac, from the player's own copy) */
static NSString *version_of(NSDictionary *cfg, NSString *sha)
{
    id v = cfg[@"exe_sha256"][sha ?: @""];
    return [v isKindOfClass:NSDictionary.class] ? v[@"version"] : [v isKindOfClass:NSString.class] ? v : nil;
}
static NSString *found_version(NSString *folder, NSDictionary *cfg)
{
    NSString *exe = find_ci(folder, cfg[@"exe"]);
    return (exe ? version_of(cfg, sha256_of(exe)) : nil) ?: cfg[@"version"];
}
/* nil when `folder` is the right game and complete; otherwise a sentence for the player */
static NSString *check_game(NSString *folder, NSDictionary *cfg)
{
    NSString *exe = find_ci(folder, cfg[@"exe"]);
    if (!exe) return [NSString stringWithFormat:@"That folder doesn't have %@ in it.", cfg[@"exe"]];
    NSString *sha = sha256_of(exe);
    if (cfg[@"exe_refuse"][sha ?: @""]) return cfg[@"exe_refuse"][sha];   /* a copy this app can't use, with what to do instead */
    if (!version_of(cfg, sha))
        return [NSString stringWithFormat:@"This isn't the version of %@ this app works with. It needs %@.", cfg[@"name"], cfg[@"version"]];
    for (NSString *r in cfg[@"required"]) if (!find_ci(folder, r))
        return [NSString stringWithFormat:@"That copy of %@ is incomplete: %@ is missing.", cfg[@"name"], r];
    return nil;
}
/* Search: where a copy of the game is likely to be. Steam's libraries (the default one and each libraryfolders.vdf names:
 * installed games, and depots downloaded from Steam's console), CrossOver and Wine bottles, GOG's folders, the usual
 * places in the home folder and other disks. game.json's "search" adds folder names to look for first. */
static NSArray<NSString *> *search_roots(void)
{
    NSFileManager *fm = NSFileManager.defaultManager; NSString *home = NSHomeDirectory();
    NSMutableArray *roots = [NSMutableArray array];
    NSString *steam = [home stringByAppendingPathComponent:@"Library/Application Support/Steam"];
    NSMutableOrderedSet *libs = [NSMutableOrderedSet orderedSetWithObject:steam];
    NSString *vdf = [NSString stringWithContentsOfFile:[steam stringByAppendingPathComponent:@"steamapps/libraryfolders.vdf"] encoding:NSUTF8StringEncoding error:nil];
    if (vdf) {
        NSRegularExpression *re = [NSRegularExpression regularExpressionWithPattern:@"\"path\"\\s+\"([^\"]+)\"" options:0 error:nil];
        for (NSTextCheckingResult *m in [re matchesInString:vdf options:0 range:NSMakeRange(0, vdf.length)])
            [libs addObject:[[vdf substringWithRange:[m rangeAtIndex:1]] stringByReplacingOccurrencesOfString:@"\\\\" withString:@"/"]];
    }
    for (NSString *lib in libs) { [roots addObject:[lib stringByAppendingPathComponent:@"steamapps/common"]]; [roots addObject:[lib stringByAppendingPathComponent:@"steamapps/content"]]; }
    [roots addObject:[steam stringByAppendingPathComponent:@"Steam.AppBundle/Steam/Contents/MacOS/steamapps/content"]];   /* where the console's download_depot puts them on a Mac */
    for (NSString *b in [fm contentsOfDirectoryAtPath:[home stringByAppendingPathComponent:@"Library/Application Support/CrossOver/Bottles"] error:nil])
        for (NSString *pf in @[@"Program Files", @"Program Files (x86)", @"GOG Games", @"Games"])
            [roots addObject:[NSString stringWithFormat:@"%@/Library/Application Support/CrossOver/Bottles/%@/drive_c/%@", home, b, pf]];
    for (NSString *pf in @[@"Program Files", @"Program Files (x86)", @"GOG Games"]) [roots addObject:[NSString stringWithFormat:@"%@/.wine/drive_c/%@", home, pf]];
    for (NSString *d in @[@"Games", @"GOG Games", @"Downloads", @"Desktop", @"Documents", @"Applications"]) [roots addObject:[home stringByAppendingPathComponent:d]];
    [roots addObjectsFromArray:@[@"/Applications", @"/Users/Shared"]];
    for (NSString *v in [fm contentsOfDirectoryAtPath:@"/Volumes" error:nil]) {    /* other disks (the startup disk's link to / is skipped) */
        NSString *p = [@"/Volumes" stringByAppendingPathComponent:v];
        if ([[fm destinationOfSymbolicLinkAtPath:p error:nil] isEqualToString:@"/"]) continue;
        [roots addObject:p];
    }
    return [roots filteredArrayUsingPredicate:[NSPredicate predicateWithBlock:^BOOL(NSString *p, id b) { BOOL d = NO; return [fm fileExistsAtPath:p isDirectory:&d] && d; }]];
}
/* every folder under the search roots (5 levels down at most, 25 s, app bundles and hidden folders skipped) holding the
 * game's executable, hash-checked: {path, version} or {path, problem} */
static NSArray<NSDictionary *> *search_game(NSDictionary *cfg, volatile BOOL *stop)
{
    NSFileManager *fm = NSFileManager.defaultManager; NSMutableArray *found = [NSMutableArray array]; NSMutableSet *seen = [NSMutableSet set];
    NSString *exe = cfg[@"exe"], *first = [exe componentsSeparatedByString:@"/"].firstObject;
    NSDate *until = [NSDate dateWithTimeIntervalSinceNow:25];
    NSMutableArray *queue = [NSMutableArray array];
    for (NSString *r in search_roots()) [queue addObject:@[r, @0]];
    while (queue.count && !*stop && until.timeIntervalSinceNow > 0) {
        NSString *here = queue[0][0]; int depth = [queue[0][1] intValue]; [queue removeObjectAtIndex:0];
        NSString *real = here.stringByResolvingSymlinksInPath; if ([seen containsObject:real]) continue; [seen addObject:real];
        NSArray *names = [fm contentsOfDirectoryAtPath:here error:nil];
        BOOL hit = NO;
        for (NSString *n in names) if ([n caseInsensitiveCompare:first] == NSOrderedSame) { hit = YES; break; }
        if (hit && find_ci(here, exe)) {
            NSString *problem = check_game(here, cfg);
            [found addObject:problem ? @{@"path": here, @"problem": problem} : @{@"path": here, @"version": found_version(here, cfg)}];
            continue;
        }
        if (depth >= 5) continue;
        for (NSString *n in names) {
            if ([n.pathExtension isEqualToString:@"app"] && [cfg[@"inside"] count]) {   /* a store's wrapper app: only where the game would be */
                NSString *in = inside_game([here stringByAppendingPathComponent:n], cfg);
                if (in && ![seen containsObject:in.stringByResolvingSymlinksInPath]) {
                    [seen addObject:in.stringByResolvingSymlinksInPath]; NSString *problem = check_game(in, cfg);
                    [found addObject:problem ? @{@"path": in, @"problem": problem} : @{@"path": in, @"version": found_version(in, cfg)}];
                }
                continue;
            }
            if ([n hasPrefix:@"."] || [n.pathExtension isEqualToString:@"app"] || [n isEqualToString:@"Library"] || [n isEqualToString:@"node_modules"]) continue;
            NSString *sub = [here stringByAppendingPathComponent:n]; BOOL dir = NO;
            if ([fm fileExistsAtPath:sub isDirectory:&dir] && dir) [queue addObject:@[sub, @(depth + 1)]];
        }
    }
    return found;
}
static BOOL game_is_built(NSDictionary *cfg)
{
    NSString *rc = [game_root(cfg) stringByAppendingPathComponent:cfg[@"build_dir"] ?: @"recomp"];
    NSData *d = [NSData dataWithContentsOfFile:[rc stringByAppendingPathComponent:@"ready.json"]];
    NSDictionary *r = d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:nil] : nil;
    return r && [r[@"code"] isEqual:cfg[@"code"]] && [NSFileManager.defaultManager fileExistsAtPath:[rc stringByAppendingPathComponent:@"libgame.dylib"]];
}
static void restart_app(NSDictionary *cfg)
{
    const char *suite = getenv("DS_SUITE");   /* started by a suite's launch window: back to it, which now shows the game ready */
    if (suite && *suite) { char *const sv[] = {(char *)suite, NULL}; unsetenv("DS_SUITE"); unsetenv("DS_SETUP_FOLDER"); execv(suite, sv); }
    NSString *rc = [game_root(cfg) stringByAppendingPathComponent:cfg[@"build_dir"] ?: @"recomp"];
    setenv("DYLD_LIBRARY_PATH", rc.fileSystemRepresentation, 1); setenv("DS_EASY_READY", "1", 1);
    const char *self = NSBundle.mainBundle.executablePath.fileSystemRepresentation;
    char *const av[] = {(char *)self, NULL};
    execv(self, av);
}

@interface ArtView : NSView            /* the frosted stage: the picture, the game's name, and the glass panel everything sits on */
@property ThStyle style; @property (copy) NSString *gameTitle; @property NSImage *soft, *blurred;
- (void)usePicture:(NSImage *)img;
@end
@implementation ArtView
- (void)usePicture:(NSImage *)img
{
    self.soft = th_frost(img, 320) ?: img; self.blurred = th_frost(img, 70) ?: img; self.needsDisplay = YES;
    for (NSView *v in self.subviews) v.needsDisplay = YES;
}
- (void)drawRect:(NSRect)d
{
    NSRect b = self.bounds; th_stage(self.style, b, self.soft, NSMakePoint(NSMidX(b), 270));
    th_title(self.style, self.gameTitle, @"NATIVE EDITION", NSMakeRect(0, b.size.height - 110, b.size.width, 96), 44);
    th_glass(NSMakeRect(50, 100, b.size.width - 100, 334), 14, th_rgba(0.05, 0.04, 0.05, 0.26), th_accent(self.style, 0.8), 1.5, self.blurred, b);
}
@end
static NSRect window_in(NSView *v, ArtView *art) { return [v convertRect:art.bounds fromView:art]; }
@interface ThemeButton : NSControl     /* a launcher button on the glass */
@property ThStyle style; @property (copy) NSString *title; @property BOOL primary, hover, down; @property (copy) void (^onPress)(void); @property (weak) ArtView *art;
@end
@implementation ThemeButton
- (void)updateTrackingAreas { for (NSTrackingArea *a in self.trackingAreas) [self removeTrackingArea:a]; [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:self.bounds options:NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow owner:self userInfo:nil]]; }
- (void)mouseEntered:(NSEvent *)e { self.hover = YES; self.needsDisplay = YES; }
- (void)mouseExited:(NSEvent *)e { self.hover = NO; self.needsDisplay = YES; }
- (void)mouseDown:(NSEvent *)e { self.down = YES; self.needsDisplay = YES; }
- (void)mouseUp:(NSEvent *)e { self.down = NO; self.needsDisplay = YES; if (NSPointInRect([self convertPoint:e.locationInWindow fromView:nil], self.bounds) && self.onPress) self.onPress(); }
- (void)drawRect:(NSRect)d { th_button(self.style, self.bounds, self.title, self.primary, self.hover, self.down, self.art.blurred, window_in(self, self.art)); }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { return YES; }
@end
@interface ThemeBar : NSView           /* the progress bar */
@property ThStyle style; @property double doubleValue; @property (weak) ArtView *art;
@end
@implementation ThemeBar
- (void)drawRect:(NSRect)d
{
    th_glass(self.bounds, 8, th_rgba(1, 1, 1, 0.05), nil, 1, self.art.blurred, window_in(self, self.art));
    NSRect f = NSInsetRect(self.bounds, 2, 2); f.size.width *= self.doubleValue / 100; if (f.size.width < 8) return;
    NSBezierPath *p = [NSBezierPath bezierPathWithRoundedRect:f xRadius:6 yRadius:6]; [th_accent(self.style, 1) setFill]; [p fill];
    [NSGraphicsContext saveGraphicsState]; [p addClip]; [[[NSGradient alloc] initWithStartingColor:th_rgba(1, 1, 1, 0.28) endingColor:th_rgba(0, 0, 0, 0.14)] drawInRect:f angle:-90]; [NSGraphicsContext restoreGraphicsState];
}
@end
@interface DropZone : NSView
@property (copy) void (^onDrop)(NSString *path);
@property (copy) NSString *line;
@property BOOL hot, enabled; @property ThStyle style; @property (weak) ArtView *art;
@end
@implementation DropZone
- (instancetype)initWithFrame:(NSRect)r { if ((self = [super initWithFrame:r])) { [self registerForDraggedTypes:@[NSPasteboardTypeFileURL]]; _enabled = YES; } return self; }
- (void)drawRect:(NSRect)d
{
    NSRect g = NSInsetRect(self.bounds, 2, 2);
    th_glass(g, 10, self.hot ? [th_accent(self.style, 0.34) colorWithAlphaComponent:0.34] : th_rgba(1, 1, 1, 0.05), self.hot ? th_accent(self.style, 1) : th_rgba(1, 1, 1, 0.22), self.hot ? 2 : 1, self.art.blurred, window_in(self, self.art));
    NSMutableParagraphStyle *st = [NSMutableParagraphStyle new]; st.alignment = NSTextAlignmentCenter;
    NSShadow *sh = [NSShadow new]; sh.shadowColor = th_rgba(0, 0, 0, 0.7); sh.shadowBlurRadius = 4;
    NSDictionary *at = @{NSFontAttributeName: th_font(self.style, 20, YES), NSForegroundColorAttributeName: th_text(self.style), NSParagraphStyleAttributeName: st, NSShadowAttributeName: sh};
    NSRect b = self.bounds; CGFloat h = [self.line boundingRectWithSize:NSMakeSize(b.size.width - 60, 200) options:NSStringDrawingUsesLineFragmentOrigin attributes:at].size.height;
    [self.line drawInRect:NSMakeRect(30, (b.size.height - h) / 2, b.size.width - 60, h + 2) withAttributes:at];
}
- (NSString *)pathIn:(id<NSDraggingInfo>)info
{
    NSArray *urls = [info.draggingPasteboard readObjectsForClasses:@[NSURL.class] options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
    return ((NSURL *)urls.firstObject).path;
}
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)i { if (!self.enabled || ![self pathIn:i]) return NSDragOperationNone; self.hot = YES; self.needsDisplay = YES; return NSDragOperationCopy; }
- (void)draggingExited:(id<NSDraggingInfo>)i { self.hot = NO; self.needsDisplay = YES; }
- (BOOL)performDragOperation:(id<NSDraggingInfo>)i { self.hot = NO; self.needsDisplay = YES; NSString *p = [self pathIn:i]; if (p && self.onDrop) self.onDrop(p); return p != nil; }
@end

@interface SetupController : NSObject <NSApplicationDelegate>
@end
@implementation SetupController {
    NSDictionary *_cfg; NSString *_root, *_inbox, *_found, *_declined;
    NSWindow *_win; NSTextField *_title, *_body, *_note; DropZone *_zone; ThemeBar *_bar;
    ThemeButton *_primary, *_secondary, *_tertiary, *_fourth; ThStyle _st; ArtView *_art; NSTask *_task; NSTimer *_scan; BOOL _busy, _updating, _cancelled;
    NSDate *_began; void (^_onPrimary)(void), (^_onSecondary)(void), (^_onTertiary)(void), (^_onFourth)(void); volatile BOOL _stopSearch; BOOL _fromSearch;
}
- (NSTextField *)label:(NSRect)r font:(NSFont *)f color:(NSColor *)c
{
    NSTextField *t = [NSTextField wrappingLabelWithString:@""]; t.frame = r; t.font = f; t.textColor = c; t.alignment = NSTextAlignmentCenter; t.selectable = NO; t.drawsBackground = NO; return t;
}
- (ThemeButton *)themeButton:(NSRect)r primary:(BOOL)p
{
    ThemeButton *b = [[ThemeButton alloc] initWithFrame:r]; b.style = _st; b.primary = p; b.art = _art; return b;
}
- (void)buildWindow
{
    _st = th_style(_cfg[@"style"]);
    NSRect full = NSMakeRect(0, 0, 800, 560);
    _win = [[NSWindow alloc] initWithContentRect:full styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable backing:NSBackingStoreBuffered defer:NO];
    _win.title = _cfg[@"name"]; _win.backgroundColor = NSColor.blackColor; _win.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua]; _win.releasedWhenClosed = NO; [_win center];
    _art = [[ArtView alloc] initWithFrame:full]; _art.style = _st; _art.gameTitle = _cfg[@"title"] ?: [_cfg[@"name"] uppercaseString]; [_art usePicture:th_scene(_st, 800, 560, 2)]; _win.contentView = _art;
    NSView *v = _art;
    _title = [self label:NSMakeRect(80, 384, 640, 36) font:th_font(_st, 25, YES) color:th_text(_st)]; [v addSubview:_title];
    _body = [self label:NSMakeRect(90, 324, 620, 56) font:th_font(_st, 15, NO) color:th_text(_st)]; [v addSubview:_body];
    _zone = [[DropZone alloc] initWithFrame:NSMakeRect(90, 156, 620, 162)]; _zone.style = _st; _zone.art = _art; [v addSubview:_zone];
    __weak SetupController *me = self; _zone.onDrop = ^(NSString *p) { [me tryPath:p]; };
    _bar = [[ThemeBar alloc] initWithFrame:NSMakeRect(100, 222, 600, 22)]; _bar.style = _st; _bar.art = _art; _bar.hidden = YES; [v addSubview:_bar];
    _note = [self label:NSMakeRect(90, 108, 620, 42) font:th_font(_st, 13, NO) color:th_dim(_st)]; [v addSubview:_note];
    _tertiary = [self themeButton:NSMakeRect(50, 30, 200, 54) primary:NO]; [v addSubview:_tertiary];
    _secondary = [self themeButton:NSMakeRect(300, 30, 200, 54) primary:NO]; [v addSubview:_secondary];
    _primary = [self themeButton:NSMakeRect(550, 30, 200, 54) primary:YES]; [v addSubview:_primary];
    _fourth = [self themeButton:NSMakeRect(0, 30, 165, 54) primary:NO]; _fourth.hidden = YES; [v addSubview:_fourth];
    _fourth.onPress = ^{ SetupController *s = me; if (s->_onFourth) s->_onFourth(); };
    _primary.onPress = ^{ SetupController *s = me; if (s->_onPrimary) s->_onPrimary(); };
    _secondary.onPress = ^{ SetupController *s = me; if (s->_onSecondary) s->_onSecondary(); };
    _tertiary.onPress = ^{ SetupController *s = me; if (s->_onTertiary) s->_onTertiary(); };
}
/* the store's art, as the launcher uses it: from the file if an earlier run saved it, else fetched in the background. Offline, the drawn scene stays */
- (void)loadPicture
{
    NSString *url = _cfg[@"art_url"], *dest = [_root stringByAppendingPathComponent:@"art/banner.jpg"]; ArtView *art = _art;
    NSImage *have = [[NSImage alloc] initWithContentsOfFile:dest];
    if (have.isValid) { [art usePicture:have]; return; }
    if (!url.length) return;
    [[NSURLSession.sharedSession dataTaskWithURL:[NSURL URLWithString:url] completionHandler:^(NSData *d, NSURLResponse *r, NSError *e) {
        if (e || d.length < 50000 || ((NSHTTPURLResponse *)r).statusCode != 200) return;
        NSImage *img = [[NSImage alloc] initWithData:d]; if (!img.isValid) return;
        [NSFileManager.defaultManager createDirectoryAtPath:dest.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
        [d writeToFile:dest atomically:YES];
        dispatch_async(dispatch_get_main_queue(), ^{ [art usePicture:img]; });
    }] resume];
}
- (void)button:(ThemeButton *)b title:(NSString *)t
{
    b.title = t ?: @""; b.hidden = t == nil; b.needsDisplay = YES;
    /* three buttons sit where they always have; with the fourth (Search) shown, four across */
    BOOL four = !_fourth.hidden; NSArray *row = @[_tertiary, _fourth, _secondary, _primary];
    if (four) for (int k = 0; k < 4; k++) [row[k] setFrame:NSMakeRect(37 + 187 * k, 30, 165, 54)];
    else { _tertiary.frame = NSMakeRect(50, 30, 200, 54); _secondary.frame = NSMakeRect(300, 30, 200, 54); _primary.frame = NSMakeRect(550, 30, 200, 54); }
    for (ThemeButton *x in row) x.needsDisplay = YES;
}
- (void)showWaiting:(NSString *)problem
{
    _busy = NO; _found = nil; _zone.hidden = NO; _zone.enabled = YES; _bar.hidden = YES;
    _title.stringValue = @"Set Up";
    _body.stringValue = [NSString stringWithFormat:@"This app uses your own copy of the game. Nothing else to install.\n%@", _cfg[@"where"]];
    _zone.line = [NSString stringWithFormat:@"Drop your %@ folder here\nor on the app's icon", _cfg[@"name"]]; _zone.needsDisplay = YES;
    _note.stringValue = problem ?: [NSString stringWithFormat:@"You can also put it in ~/Games/%@/Put Game Files Here.", _cfg[@"folder"]];
    _note.textColor = problem ? th_rgb(1, 0.60, 0.50) : th_dim(_st);
    __weak SetupController *me = self;
    [self button:_fourth title:@"Search"]; _onFourth = ^{ [me search]; };
    [self button:_primary title:@"Choose..."]; _onPrimary = ^{ [me choose]; };
    [self button:_secondary title:@"Open Folder"]; _onSecondary = ^{ [me openInbox]; };
    [self button:_tertiary title:@"Quit"]; _onTertiary = ^{ [NSApp terminate:nil]; };
    if (problem) [self shot:@"problem"];
}
- (void)openInbox
{
    [NSFileManager.defaultManager createDirectoryAtPath:_inbox withIntermediateDirectories:YES attributes:nil error:nil];
    [NSWorkspace.sharedWorkspace openURL:[NSURL fileURLWithPath:_inbox]];
}
- (void)choose
{
    NSOpenPanel *p = [NSOpenPanel openPanel]; p.canChooseDirectories = YES; p.canChooseFiles = NO; p.prompt = @"Use This Folder";
    p.message = [NSString stringWithFormat:@"Choose your %@ folder", _cfg[@"name"]];
    [p beginSheetModalForWindow:_win completionHandler:^(NSModalResponse r) { if (r == NSModalResponseOK) [self tryPath:p.URL.path]; }];
}
/* Search: looks through the usual places (search_roots) on another thread; one copy that checks out goes straight to
 * Found, several are offered in a list, and what was found but can't be used says why */
- (void)search
{
    if (_busy) return;
    _busy = YES; _stopSearch = NO; _zone.enabled = NO;
    _note.stringValue = @"Searching Steam, GOG and CrossOver folders, your home folder and other disks..."; _note.textColor = th_dim(_st);
    __weak SetupController *me = self;
    [self button:_fourth title:@"Stop"]; _onFourth = ^{ SetupController *s = me; s->_stopSearch = YES; };
    NSDictionary *cfg = _cfg; volatile BOOL *stop = &_stopSearch;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSArray *found = search_game(cfg, stop);
        dispatch_async(dispatch_get_main_queue(), ^{ SetupController *s = me; if (!s) return; s->_busy = NO; s->_zone.enabled = YES; [s searched:found]; });
    });
}
- (void)searched:(NSArray<NSDictionary *> *)found
{
    NSArray *good = [found filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"version != nil"]];
    if (getenv("DS_SETUP_SEARCHLOG")) for (NSDictionary *f in found) fprintf(stderr, "setup: search found %s: %s\n", [f[@"path"] UTF8String], [(f[@"version"] ?: f[@"problem"]) UTF8String]);
    if (!good.count) {
        [self showWaiting:found.count ? [NSString stringWithFormat:@"Found %@, but: %@", [found[0][@"path"] stringByAbbreviatingWithTildeInPath], found[0][@"problem"]]
                                      : [NSString stringWithFormat:@"Couldn't find %@ on this Mac. Drop its folder here, or Choose... it.", _cfg[@"name"]]];
        return;
    }
    _fromSearch = YES;                                   /* found where it lives (a Steam library, another app): never offered to the Trash */
    if (good.count == 1) { [self showFound:good[0][@"path"]]; return; }
    NSAlert *a = [NSAlert new]; a.messageText = [NSString stringWithFormat:@"Found %lu copies of %@", (unsigned long)good.count, _cfg[@"name"]];
    a.informativeText = @"Which one should the app use?";
    NSPopUpButton *pop = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 0, 460, 26) pullsDown:NO];
    for (NSDictionary *f in good) [pop addItemWithTitle:[NSString stringWithFormat:@"%@ (%@)", [f[@"path"] stringByAbbreviatingWithTildeInPath], f[@"version"]]];
    a.accessoryView = pop; [a addButtonWithTitle:@"Use This One"]; [a addButtonWithTitle:@"Cancel"];
    [a beginSheetModalForWindow:_win completionHandler:^(NSModalResponse r) {
        if (r == NSAlertFirstButtonReturn) [self showFound:good[(NSUInteger)pop.indexOfSelectedItem][@"path"]]; else [self showWaiting:nil];
    }];
}
- (void)tryPath:(NSString *)path
{
    if (_busy) return;
    _fromSearch = NO;
    BOOL dir = NO;
    if (![NSFileManager.defaultManager fileExistsAtPath:path isDirectory:&dir]) return;
    if (!dir) path = path.stringByDeletingLastPathComponent;
    _busy = YES; _note.stringValue = @"Checking that folder..."; _note.textColor = th_dim(_st);
    NSDictionary *cfg = _cfg;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSString *game = locate_game(path, cfg);
        NSString *problem = game ? check_game(game, cfg) : [NSString stringWithFormat:@"That folder doesn't have %@ in it.", cfg[@"exe"]];
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_busy = NO;
            if (problem) [self showWaiting:problem]; else [self showFound:game];
        });
    });
}
- (void)scanInbox
{
    if (_busy || _found) return;
    NSFileManager *fm = NSFileManager.defaultManager;
    NSArray *items = [[fm contentsOfDirectoryAtPath:_inbox error:nil] filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"NOT SELF BEGINSWITH '.'"]];
    if (!items.count) { _declined = nil; return; }
    if (_declined) return;
    NSDictionary *cfg = _cfg; NSString *inbox = _inbox; _busy = YES;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        NSString *game = locate_game(inbox, cfg);
        BOOL ok = game && !check_game(game, cfg);                  /* still being copied in: not yet, and no complaint */
        dispatch_async(dispatch_get_main_queue(), ^{ self->_busy = NO; if (ok && !self->_found) [self showFound:game]; });
    });
}
- (void)showFound:(NSString *)game
{
    _found = game; _zone.hidden = YES; _bar.hidden = YES;
    _title.stringValue = [NSString stringWithFormat:@"Found Version %@", found_version(game, _cfg)];
    _body.stringValue = [NSString stringWithFormat:@"Set it up now? This builds the game for your Mac and takes about %@ minutes.\nYour files aren't changed.", _cfg[@"minutes"]];
    _note.stringValue = game; _note.textColor = th_dim(_st);
    __weak SetupController *me = self; NSString *inbox = _inbox;
    [self button:_fourth title:nil]; [self button:_primary title:@"Set Up"]; _onPrimary = ^{ [me runSetup:game]; };
    [self button:_secondary title:@"Not Now"]; _onSecondary = ^{ SetupController *s = me; if ([game hasPrefix:inbox]) s->_declined = game; [s showWaiting:nil]; };
    [self button:_tertiary title:nil]; _onTertiary = nil;
    [self autoPress:@"found"];
}
/* tests: DS_SETUP_SHOTS=<folder> saves the window as each screen appears; DS_SETUP_PRESS presses the main button on the Found and All Set screens */
- (void)shot:(NSString *)name
{
    const char *dir = getenv("DS_SETUP_SHOTS"); if (!dir) return;
    NSView *v = _win.contentView; [v layoutSubtreeIfNeeded]; [v displayIfNeeded];
    NSBitmapImageRep *r = [v bitmapImageRepForCachingDisplayInRect:v.bounds]; [v cacheDisplayInRect:v.bounds toBitmapImageRep:r];
    [[r representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:[[@(dir) stringByAppendingPathComponent:name] stringByAppendingPathExtension:@"png"] atomically:YES];
}
- (void)autoPress:(NSString *)name
{
    [self shot:name];
    if (getenv("DS_SETUP_PRESS")) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 2), dispatch_get_main_queue(), ^{ if (self->_onPrimary) self->_onPrimary(); });
}
- (void)fail:(NSString *)why
{
    _busy = NO; _found = nil; _bar.hidden = YES; _zone.hidden = YES;
    _title.stringValue = @"Setup Didn't Finish"; _body.stringValue = why; _note.stringValue = @"You can try again. It picks up where it stopped.";
    _note.textColor = th_dim(_st);
    __weak SetupController *me = self;
    [self button:_fourth title:nil]; [self button:_primary title:@"Start Over"]; _onPrimary = ^{ [me showWaiting:nil]; };
    [self button:_secondary title:nil]; _onSecondary = nil; [self button:_tertiary title:@"Quit"]; _onTertiary = ^{ [NSApp terminate:nil]; };
    [self shot:@"failed"]; if (getenv("DS_SETUP_PRESS")) { fprintf(stderr, "setup failed: %s\n", why.UTF8String); exit(3); }
}
- (void)runSetup:(NSString *)game
{
    _busy = YES; _cancelled = NO; _zone.hidden = YES; _bar.hidden = NO; _bar.doubleValue = 0; _bar.needsDisplay = YES; _began = [NSDate date];
    _title.stringValue = _updating ? @"Updating" : @"Setting Up";
    _body.stringValue = @"Your Mac is building the game. You can keep using it while this runs."; _note.stringValue = @"Getting ready..."; _note.textColor = th_dim(_st);
    __weak SetupController *me = self;
    [self button:_fourth title:nil]; [self button:_primary title:nil]; _onPrimary = nil; [self button:_secondary title:nil]; _onSecondary = nil;
    [self button:_tertiary title:@"Cancel"]; _onTertiary = ^{ SetupController *s = me; s->_cancelled = YES; if (s->_task.running) kill(-s->_task.processIdentifier, SIGTERM); };
    NSString *res = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"setup"], *root = _root, *inbox = _inbox;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSString *tools = [root stringByAppendingPathComponent:@".setup/tools"], *mark = [tools stringByAppendingPathComponent:@".ok"];
        NSFileManager *fm = NSFileManager.defaultManager;
        if (![fm fileExistsAtPath:mark]) {        /* unpacked here rather than run from inside the app */
            [fm removeItemAtPath:tools error:nil]; [fm createDirectoryAtPath:tools withIntermediateDirectories:YES attributes:nil error:nil];
            NSTask *x = [NSTask new]; x.executableURL = [NSURL fileURLWithPath:@"/usr/bin/tar"];
            x.arguments = @[@"-xf", [res stringByAppendingPathComponent:@"toolkit.tar.xz"], @"-C", tools];
            NSError *e = nil; [x launchAndReturnError:&e]; [x waitUntilExit];
            if (e || x.terminationStatus) { dispatch_async(dispatch_get_main_queue(), ^{ [self fail:@"Couldn't unpack the setup tools. Is your disk full?"]; }); return; }
            NSTask *q = [NSTask new]; q.executableURL = [NSURL fileURLWithPath:@"/usr/bin/xattr"];   /* tar carries the download's quarantine over: the app's own tools must run without a prompt each */
            q.arguments = @[@"-r", @"-d", @"com.apple.quarantine", tools]; q.standardError = [NSFileHandle fileHandleForWritingAtPath:@"/dev/null"];
            [q launchAndReturnError:nil]; [q waitUntilExit];
            [@"" writeToFile:mark atomically:NO encoding:NSUTF8StringEncoding error:nil];
        }
        NSTask *t = [NSTask new]; NSPipe *pipe = [NSPipe pipe];
        t.executableURL = [NSURL fileURLWithPath:[tools stringByAppendingPathComponent:@"python/bin/python3"]];
        t.arguments = @[[res stringByAppendingPathComponent:@"setup.py"], @"--game", game ?: @"", @"--root", root, @"--setup", res, @"--tools", tools, @"--config", config_path ?: [res stringByAppendingPathComponent:@"game.json"]];
        t.environment = @{@"PATH": @"/usr/bin:/bin", @"HOME": NSHomeDirectory(), @"LANG": @"en_US.UTF-8"};
        t.standardOutput = pipe; t.standardError = [NSFileHandle fileHandleForWritingAtPath:@"/dev/null"];
        self->_task = t;
        __block NSMutableData *buf = [NSMutableData data]; __block NSString *lastError = nil; __block BOOL done = NO;
        pipe.fileHandleForReading.readabilityHandler = ^(NSFileHandle *h) {
            NSData *d = h.availableData; if (!d.length) return;
            [buf appendData:d];
            for (;;) {
                NSRange r = [buf rangeOfData:[NSData dataWithBytes:"\n" length:1] options:0 range:NSMakeRange(0, buf.length)];
                if (r.location == NSNotFound) break;
                NSData *line = [buf subdataWithRange:NSMakeRange(0, r.location)]; [buf replaceBytesInRange:NSMakeRange(0, r.location + 1) withBytes:NULL length:0];
                NSDictionary *m = [NSJSONSerialization JSONObjectWithData:line options:0 error:nil]; if (![m isKindOfClass:NSDictionary.class]) continue;
                if ([m[@"stage"] isEqual:@"error"]) { lastError = m[@"text"]; continue; }
                if ([m[@"stage"] isEqual:@"done"]) done = YES;
                dispatch_async(dispatch_get_main_queue(), ^{ [self progress:[m[@"pct"] doubleValue] text:m[@"text"]]; });
            }
        };
        NSError *e = nil; [t launchAndReturnError:&e];
        if (!e) [t waitUntilExit];
        pipe.fileHandleForReading.readabilityHandler = nil;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (e) [self fail:@"Couldn't start the setup tools."];
            else if (self->_cancelled) [self showWaiting:nil];
            else if (done) [self finishedFrom:game inbox:inbox];
            else [self fail:lastError ?: @"Setup stopped unexpectedly."];
        });
    });
}
- (void)progress:(double)pct text:(NSString *)text
{
    _bar.doubleValue = pct; _bar.needsDisplay = YES;
    static BOOL once; if (pct > 30 && !once) { once = YES; [self shot:@"progress"]; }
    NSTimeInterval t = -_began.timeIntervalSinceNow;
    NSString *left = @"";
    if (pct > 8 && t > 8) { int m = (int)ceil(t * (100 - pct) / pct / 60); left = m <= 1 ? @"  ·  about a minute left" : [NSString stringWithFormat:@"  ·  about %d minutes left", m]; }
    _note.stringValue = [text stringByAppendingString:left];
}
/* the files that were dropped stay put unless the player says otherwise; what came through the app's own folder is removed */
- (void)finishedFrom:(NSString *)game inbox:(NSString *)inbox
{
    NSDictionary *cfg = _cfg; NSFileManager *fm = NSFileManager.defaultManager;
    if (game.length && [game hasPrefix:inbox]) { for (NSString *n in [fm contentsOfDirectoryAtPath:inbox error:nil]) [fm removeItemAtPath:[inbox stringByAppendingPathComponent:n] error:nil]; restart_app(cfg); return; }
    if (!game.length || _fromSearch) { restart_app(cfg); return; }
    if ([game.lowercaseString containsString:@"/steamapps/"] || [game containsString:@".app/"]) { restart_app(cfg); return; }   /* a store's own copy: Steam keeps it */
    _busy = YES; _bar.hidden = YES; _title.stringValue = @"All Set";
    _body.stringValue = @"The app has its own copy of the game now.\nWould you like to remove the folder you dropped, to save space?"; _note.stringValue = game; _note.textColor = th_dim(_st);
    [self button:_fourth title:nil]; [self button:_primary title:@"Keep It and Play"]; _onPrimary = ^{ restart_app(cfg); };
    [self button:_secondary title:@"Move to Trash"];
    _onSecondary = ^{ [NSWorkspace.sharedWorkspace recycleURLs:@[[NSURL fileURLWithPath:game]] completionHandler:^(NSDictionary *d, NSError *e) { restart_app(cfg); }]; };
    [self button:_tertiary title:nil]; _onTertiary = nil;
    [self autoPress:@"done"];
}
- (void)start:(NSDictionary *)cfg
{
    _cfg = cfg; _root = game_root(cfg); _inbox = [_root stringByAppendingPathComponent:@"Put Game Files Here"];
    [NSFileManager.defaultManager createDirectoryAtPath:_inbox withIntermediateDirectories:YES attributes:nil error:nil];
    [self buildWindow];
    NSMenu *bar = [NSMenu new]; NSMenuItem *it = [NSMenuItem new]; [bar addItem:it]; NSMenu *m = [NSMenu new];
    [m addItemWithTitle:[@"Quit " stringByAppendingString:cfg[@"name"]] action:@selector(terminate:) keyEquivalent:@"q"]; it.submenu = m; NSApp.mainMenu = bar;
}
- (void)applicationDidFinishLaunching:(NSNotification *)n
{
    NSString *installed = [_root stringByAppendingPathComponent:@"game-files"];
    [_win makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES];
    [self loadPicture];
    if (getenv("DS_SETUP_SHOTS")) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 7 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ [self shot:@"later"]; });   /* tests: the fetched picture in place */
    if ([NSFileManager.defaultManager fileExistsAtPath:[installed stringByAppendingPathComponent:_cfg[@"exe"]]]) { _updating = YES; [self runSetup:@""]; return; }   /* the game is here: an update changed its code */
    if (!_found && !_busy) [self showWaiting:nil];
    [self shot:@"waiting"];
    const char *given = getenv("DS_SETUP_FOLDER") ?: getenv("DS_SETUP_AUTO");  /* a folder a suite's launch window was given (tests: what a drop does) */
    if (given && *given) [self tryPath:@(given)];
    if (getenv("DS_SETUP_SEARCH")) [self search];                                   /* tests: what Search does */
    _scan = [NSTimer scheduledTimerWithTimeInterval:2 repeats:YES block:^(NSTimer *t) { [self scanInbox]; }];
}
- (void)application:(NSApplication *)app openFiles:(NSArray<NSString *> *)names
{
    [app replyToOpenOrPrint:NSApplicationDelegateReplySuccess];
    if (!_busy && names.count) { [self showWaiting:nil]; [self tryPath:names[0]]; }
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a { return YES; }
- (void)applicationWillTerminate:(NSNotification *)n { if (_task.running) kill(-_task.processIdentifier, SIGTERM); }
@end

/* Called from main() when the app is opened normally. Returns 1 with the three paths set once the game is ready; otherwise
 * it runs the setup window and never comes back (the app restarts into the launcher when setup is done). -1: not this kind
 * of build. */
int ds_easy_install(const char **exe, const char **game, const char **data)
{
    NSDictionary *cfg = game_config();
    if (!cfg) return -1;
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *root = game_root(cfg), *rc = [root stringByAppendingPathComponent:cfg[@"build_dir"] ?: @"recomp"];
    if (game_is_built(cfg)) {
        unsetenv("DS_SUITE"); unsetenv("DS_SETUP_FOLDER");                /* the game is ready: it runs, never back to a suite */
        if (!getenv("DS_EASY_READY")) restart_app(cfg);                   /* binds the game at load: see make_release.sh */
        if (!dlsym(RTLD_DEFAULT, "rt_fntab")) unsetenv("DS_EASY_READY");
        else {
            [fm createDirectoryAtPath:[root stringByAppendingPathComponent:@"drive_c/Users/player/Documents"] withIntermediateDirectories:YES attributes:nil error:nil];
            NSString *log = [root stringByAppendingPathComponent:[cfg[@"log"] stringByAppendingString:@".log"]];
            if ([[fm attributesOfItemAtPath:log error:nil] fileSize] > 5000000) { NSString *old = [[log stringByDeletingPathExtension] stringByAppendingString:@".old.log"]; [fm removeItemAtPath:old error:nil]; [fm moveItemAtPath:log toPath:old error:nil]; }
            freopen(log.fileSystemRepresentation, "a", stderr);
            setenv("DS_LOG_FILE", log.fileSystemRepresentation, 1);
            NSString *upd = [NSBundle.mainBundle.executablePath.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"update"];
            if ([fm isExecutableFileAtPath:upd]) { setenv("DS_UPDATER", upd.fileSystemRepresentation, 1); setenv("DS_UPDATE_BRANCH", "release", 1); setenv("DS_BUILT_COMMIT", [cfg[@"release"] ?: @"" UTF8String], 1); }
            *exe = strdup([rc stringByAppendingPathComponent:[cfg[@"exe"] lastPathComponent]].fileSystemRepresentation);
            *game = strdup([root stringByAppendingPathComponent:@"game-files"].fileSystemRepresentation);
            *data = strdup(root.fileSystemRepresentation);
            return 1;
        }
    }
    [NSApplication sharedApplication]; [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    SetupController *c = [SetupController new]; [c start:cfg]; NSApp.delegate = c;
    [NSApp run];
    exit(0);
}
