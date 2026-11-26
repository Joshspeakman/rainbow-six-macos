// Rainbow Six's launch window on macOS (the co-op mod's launcher), the counterpart of linux_launcher/r6_launcher.py and
// laid out the same: the game's own main-menu art as a banner, the Controls row (Classic / Modern / Keyboard Only /
// Custom), and a button per group of settings that opens it as a panel of rows (click or arrow keys to change; text
// rows edit in place). KEY GUIDE lists every action's keys and rebinds any of them (click, then press a key or a mouse
// button). Built instead of launcher.m for Rainbow Six (tools/build_app.sh).
//
// Everything is written, on Play only, where the game reads it: options in <data>/registry.txt (the runtime's registry:
// key<TAB>name<TAB>type<TAB>hex), controls in <data>/game/sherman.kmp. The rows, actions, key names and the layouts
// come from r6coop-launcher.json, which the mod ships next to its RainbowSix.exe (shared with the Windows and Linux
// launchers); without it the window has only Quit and Play (and the game its own Options screen). The banner
// is <game>/data/bitmap/mm_bg.RSB from the player's own copy.
//
// Dressed as the game is: the game's own title keyed out of its main menu (mm_bg.RSB) over frames of its intro and
// ending, the menu's operatives and the store's screenshots drifting and crossfading behind the window, dust drifting,
// far shots flashing, a portrait (the box's operative, else the menu's) in a teal sight's frame with a scan sweeping
// down it, the menus' teal brackets on translucent panels. The still part is drawn once into a picture and again only
// when what it shows changes; the glows are sprites drawn once; the timer runs in the modal loop's mode: 60 fps.
// Art: <data>/art (tools/r6_art.py at install: intro-<n>.jpg, the movies' frames; store-portrait.jpg, store-shot-<n>.jpg,
// GOG's box and screenshots), else Contents/Resources/art. DS_LAUNCHER_TIME=<s> (the clock), DS_LAUNCHER_ART=<folder>,
// DS_LAUNCHER_TIMING=1 and DS_LAUNCHER_HOME=<folder> (the settings and art from there, not the data folder) for tests.
#import <AppKit/AppKit.h>
#import <GameController/GameController.h>
#import <QuartzCore/QuartzCore.h>
#import <CommonCrypto/CommonDigest.h>
#include "../runtime/edition.h"

static NSString *const REG_KEY = @"hklm\\software\\red storm entertainment\\tom clancy's rainbow six";
enum { REG_SZ = 1, REG_DWORD = 4, KMP_N = 257 };
uint8_t ds_mac_vk(uint16_t code);                 /* main.m: a Mac key code's Windows virtual key */

// ---------------------------------------------------------------- settings: registry.txt and sherman.kmp
/* the runtime's registry.txt, kept whole: values of the game's key read and replaced, every other line as it was */
static NSMutableArray<NSString *> *reg_lines;
static NSArray<NSString *> *reg_fields(NSString *line, NSString *name)   /* the line's fields if it is the game's value `name` */
{
    NSArray<NSString *> *p = [line componentsSeparatedByString:@"\t"];
    if (p.count != 4 || [p[0] caseInsensitiveCompare:REG_KEY] != NSOrderedSame || [p[1] caseInsensitiveCompare:name] != NSOrderedSame) return nil;
    return p;
}
static void reg_load(NSString *path)
{
    NSData *d = [NSData dataWithContentsOfFile:path];   /* Latin-1: every byte round-trips */
    NSString *s = d ? [[NSString alloc] initWithData:d encoding:NSISOLatin1StringEncoding] : @"";
    reg_lines = [[s componentsSeparatedByString:@"\n"] mutableCopy];
    if ([reg_lines.lastObject isEqualToString:@""]) [reg_lines removeLastObject];
}
static NSData *unhex(NSString *h)
{
    h = [h stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    NSMutableData *d = [NSMutableData dataWithCapacity:h.length / 2];
    for (NSUInteger k = 0; k + 1 < h.length; k += 2) { unsigned v; if (sscanf([h substringWithRange:NSMakeRange(k, 2)].UTF8String, "%2x", &v) != 1) break; uint8_t b = (uint8_t)v; [d appendBytes:&b length:1]; }
    return d;
}
static BOOL reg_get(NSString *name, uint32_t *type, NSData **data)
{
    for (NSString *line in reg_lines) { NSArray *p = reg_fields(line, name); if (!p) continue; *type = (uint32_t)[p[2] intValue]; *data = unhex(p[3]); return YES; }
    return NO;
}
static void reg_delete(NSString *name)
{
    for (NSInteger k = (NSInteger)reg_lines.count - 1; k >= 0; k--) if (reg_fields(reg_lines[k], name)) [reg_lines removeObjectAtIndex:k];
}
static void reg_set(NSString *name, uint32_t type, NSData *data)
{
    NSMutableString *hex = [NSMutableString stringWithCapacity:data.length * 2];
    for (NSUInteger k = 0; k < data.length; k++) [hex appendFormat:@"%02x", ((const uint8_t *)data.bytes)[k]];
    NSString *line = [NSString stringWithFormat:@"%@\t%@\t%u\t%@", REG_KEY, name, type, hex];
    for (NSUInteger k = 0; k < reg_lines.count; k++) if (reg_fields(reg_lines[k], name)) { reg_lines[k] = line; return; }
    [reg_lines addObject:line];
}
static BOOL reg_save(NSString *path, NSError **err)
{
    [NSFileManager.defaultManager createDirectoryAtPath:path.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *s = [[reg_lines componentsJoinedByString:@"\n"] stringByAppendingString:@"\n"];
    return [[s dataUsingEncoding:NSISOLatin1StringEncoding allowLossyConversion:YES] writeToFile:path options:NSDataWritingAtomic error:err];   /* a temporary file, then renamed */
}

/* a spec value as text (the JSON may give a number) */
static NSString *str(id v) { return [v isKindOfClass:NSString.class] ? v : [v isKindOfClass:NSNumber.class] ? [v stringValue] : nil; }
static NSArray *arr(id v) { return [v isKindOfClass:NSArray.class] ? v : @[]; }
static NSDictionary *dict(id v) { return [v isKindOfClass:NSDictionary.class] ? v : @{}; }
static NSData *latin1(NSString *s) { return [s ?: @"" dataUsingEncoding:NSISOLatin1StringEncoding allowLossyConversion:YES]; }

static NSString *read_value(NSDictionary *row)
{
    uint32_t type; NSData *d; NSString *reg = str(row[@"reg"]);
    if (!reg || !reg_get(reg, &type, &d)) return str(row[@"default"]) ?: @"";
    if ([str(row[@"store"]) isEqualToString:@"dword"] && type == REG_DWORD && d.length == 4) { uint32_t v; memcpy(&v, d.bytes, 4); return [NSString stringWithFormat:@"%u", v]; }
    NSUInteger n = 0; while (n < d.length && ((const uint8_t *)d.bytes)[n]) n++;   /* REG_SZ, or the game's number-as-text; the NUL may be missing ("TRUE" in 4 bytes) */
    return [[NSString alloc] initWithBytes:d.bytes length:n encoding:NSISOLatin1StringEncoding] ?: @"";
}
static void write_value(NSDictionary *row, NSString *value)
{
    NSString *reg = str(row[@"reg"]), *store = str(row[@"store"]) ?: @"sz";
    if (!reg || [str(row[@"type"]) isEqualToString:@"controls"]) return;
    if ([store isEqualToString:@"dword"]) { uint32_t v = (uint32_t)strtoll(value.UTF8String ?: "0", NULL, 10); reg_set(reg, REG_DWORD, [NSData dataWithBytes:&v length:4]); }
    else if ([store isEqualToString:@"dwordtext"]) reg_set(reg, REG_DWORD, latin1(value));    /* the game's own form: text, type 4 */
    else { NSMutableData *d = [latin1(value) mutableCopy]; [d appendBytes:"" length:1]; reg_set(reg, REG_SZ, d); }
}

/* a file in a folder, whatever the case of its name on disk (the game writes some with capitals) */
static NSString *find_ci(NSString *dir, NSString *name)
{
    for (NSString *f in [NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil])
        if ([f caseInsensitiveCompare:name] == NSOrderedSame) return [dir stringByAppendingPathComponent:f];
    return nil;
}
/* the whole 257-byte sherman.kmp: byte 0 the mouselook flag, then the action of each key code. A short file reads as
 * the game reads it: its last byte repeated to the end. nil when there is none. */
static NSMutableData *read_kmp(NSString *path)
{
    NSData *d = path ? [NSData dataWithContentsOfFile:path] : nil; if (!d.length) return nil;
    NSMutableData *t = [NSMutableData dataWithLength:KMP_N]; uint8_t *p = t.mutableBytes; const uint8_t *s = d.bytes;
    for (NSUInteger k = 0; k < KMP_N; k++) p[k] = s[MIN(k, d.length - 1)];
    return t;
}

/* RSB: u32 version, width, height, 0, bit counts R G B A (5 6 5 0), then 16-bit pixels, top row first. Rows y0..y1
 * of it as an image (Core Graphics has no 5-6-5 format: widened to 8 bits a channel). */
static NSImage *read_rsb(NSString *path, uint32_t y0, uint32_t y1)
{
    NSData *d = [NSData dataWithContentsOfFile:path]; if (d.length < 32) return nil;
    uint32_t h[8]; memcpy(h, d.bytes, 32); uint32_t w = h[1], ht = h[2];
    if (h[4] != 5 || h[5] != 6 || h[6] != 5 || h[7] != 0 || !w || !ht || w > 4096 || ht > 4096 || d.length < 32 + (uint64_t)w * ht * 2) return nil;
    if (y1 > ht || y0 >= y1) { y0 = 0; y1 = ht; }
    size_t n = (size_t)w * (y1 - y0); NSMutableData *px = [NSMutableData dataWithLength:n * 4];
    const uint8_t *src = (const uint8_t *)d.bytes + 32 + (size_t)w * y0 * 2; uint8_t *o = px.mutableBytes;
    for (size_t k = 0; k < n; k++) {
        uint16_t v = (uint16_t)(src[2 * k] | src[2 * k + 1] << 8); uint8_t r = v >> 11, g = (v >> 5) & 63, b = v & 31;
        o[4 * k] = (uint8_t)(r << 3 | r >> 2); o[4 * k + 1] = (uint8_t)(g << 2 | g >> 4); o[4 * k + 2] = (uint8_t)(b << 3 | b >> 2); o[4 * k + 3] = 255;
    }
    CGDataProviderRef dp = CGDataProviderCreateWithCFData((__bridge CFDataRef)px); CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGImageRef img = CGImageCreate(w, y1 - y0, 8, 32, w * 4, cs, (CGBitmapInfo)kCGImageAlphaNoneSkipLast, dp, NULL, false, kCGRenderingIntentDefault);
    CGColorSpaceRelease(cs); CGDataProviderRelease(dp);
    NSImage *r = img ? [[NSImage alloc] initWithCGImage:img size:NSMakeSize(w, y1 - y0)] : nil; if (img) CGImageRelease(img);
    return r;
}

// ---------------------------------------------------------------- the spec
static NSMutableDictionary<NSString *, NSDictionary *> *rows;    /* id -> row */
static NSMutableArray<NSString *> *row_order;                     /* the spec's order (new registry lines are added in it) */
static NSArray<NSString *> *top_ids; static NSArray<NSDictionary *> *groups, *actions;
static NSMutableDictionary<NSString *, NSString *> *values;      /* id -> the value shown (and written on Play) */
static NSMutableDictionary<NSString *, NSData *> *layouts;       /* classic, modern, keyboard: 257 bytes each */
static NSMutableData *keys;                                       /* the key map being edited */
static NSMutableDictionary<NSNumber *, NSString *> *key_names; static NSMutableDictionary<NSNumber *, NSNumber *> *vk_to_code;
static NSDictionary *mouse; static NSString *version, *kmp_file; static BOOL have_spec;

static NSString *layout_name(void)
{
    for (NSString *n in layouts) if ([layouts[n] isEqualToData:keys]) return n;
    return @"custom";
}
static BOOL number(NSString *s, double *out)   /* float() of the Linux launcher: the whole text a number */
{
    const char *c = s.UTF8String; char *e; if (!c || !*c) return NO;
    double v = strtod(c, &e); while (*e == ' ') e++;
    if (e == c || *e) return NO;
    *out = v; return YES;
}
static NSArray<NSDictionary *> *choices(NSString *rid) { return arr(rows[rid][@"choices"]); }
static NSInteger choice_index(NSString *rid)
{
    NSArray<NSDictionary *> *ch = choices(rid);
    if ([str(rows[rid][@"type"]) isEqualToString:@"controls"]) {
        NSString *n = layout_name();
        for (NSUInteger i = 0; i < ch.count; i++) if ([str(ch[i][@"value"]) isEqualToString:n]) return (NSInteger)i;
        return (NSInteger)ch.count - 1;
    }
    NSString *v = values[rid] ?: @"";
    for (NSUInteger i = 0; i < ch.count; i++) { NSString *c = str(dict(ch[i])[@"value"]); if (c && [c caseInsensitiveCompare:v] == NSOrderedSame) return (NSInteger)i; }
    double x, c0, best = INFINITY; NSInteger bi = -1;
    if (ch.count && number(v, &x) && number(str(ch[0][@"value"]), &c0))   /* decimals (volumes, brightness, mouse speed): the nearest step */
        for (NSUInteger i = 0; i < ch.count; i++) { double c; if (number(str(ch[i][@"value"]), &c) && fabs(c - x) < best) { best = fabs(c - x); bi = (NSInteger)i; } }
    return bi;
}
static void row_label(NSString *rid, NSString **val, NSString **note)
{
    NSDictionary *r = rows[rid]; *note = str(r[@"note"]) ?: @"";
    if ([str(r[@"type"]) isEqualToString:@"text"]) { *val = values[rid].length ? values[rid] : @"(none)"; return; }
    NSInteger i = choice_index(rid);
    if (i < 0) { *val = values[rid] ?: @""; return; }
    NSDictionary *c = dict(choices(rid)[i]);
    *val = str(c[@"label"]) ?: str(c[@"value"]) ?: @""; if ([str(c[@"note"]) length]) *note = str(c[@"note"]);
}
static void cycle(NSString *rid, NSInteger d)
{
    NSDictionary *r = rows[rid]; NSArray<NSDictionary *> *ch = choices(rid); NSString *type = str(r[@"type"]);
    if ([type isEqualToString:@"text"] || !ch.count) return;
    NSInteger i = choice_index(rid);
    if ([type isEqualToString:@"controls"]) {
        NSMutableArray<NSString *> *presets = [NSMutableArray array];   /* Custom comes from editing keys */
        for (NSDictionary *c in ch) if (layouts[str(c[@"value"]) ?: @""]) [presets addObject:str(c[@"value"])];
        if (!presets.count) return;
        NSInteger n = (NSInteger)presets.count; i = (i >= 0 && i < n) ? i : (d > 0 ? -1 : 0);
        [keys setData:layouts[presets[((i + d) % n + n) % n]]];
        return;
    }
    NSInteger n = (NSInteger)ch.count;
    i = i >= 0 ? ((i + d) % n + n) % n : (d > 0 ? 0 : n - 1);
    values[rid] = str(ch[i][@"value"]) ?: @"";
    void refresh_top(void); refresh_top();         /* rows shown only for some values (Play Online's room codes) */
}

/* a "modes" row (Mission Resolution) also offers this display's own sizes, the ones main.m gives the game's mode list
 * (DS_DISPLAY_MODES: its pixels, then on a MacBook with a notch the 16:10 area below it); the host shows any size
 * letterboxed. Sorted by width, then height, as the Linux launcher. */
static void add_modes(void)
{
    NSMutableArray *sizes = [NSMutableArray array]; const char *e = getenv("DS_DISPLAY_MODES");
    for (NSString *m in e ? [@(e) componentsSeparatedByString:@","] : @[]) {
        NSArray *p = [m componentsSeparatedByString:@"x"]; if (p.count != 2) continue;
        [sizes addObject:@[@([p[0] intValue]), @([p[1] intValue]), sizes.count ? @"This display, below the notch" : @"This display"]];
    }
    for (NSString *rid in row_order) {
        if (![rows[rid][@"modes"] boolValue]) continue;
        NSMutableArray *ch = [choices(rid) mutableCopy];
        for (NSArray *z in sizes) {
            long w = [z[0] longValue], h = [z[1] longValue]; NSString *v = [NSString stringWithFormat:@"%ldx%ld", w, h];
            if (w < 640 || h < 480 || w > 8192 || h > 8192) continue;
            BOOL have = NO; for (NSDictionary *c in ch) if ([str(dict(c)[@"value"]) isEqualToString:v]) have = YES;
            if (!have) [ch addObject:@{@"value": v, @"label": [NSString stringWithFormat:@"%ld x %ld", w, h], @"note": z[2]}];
        }
        [ch sortUsingComparator:^NSComparisonResult(NSDictionary *a, NSDictionary *b) {
            NSArray *pa = [str(dict(a)[@"value"]) componentsSeparatedByString:@"x"], *pb = [str(dict(b)[@"value"]) componentsSeparatedByString:@"x"];
            long aw = pa.count == 2 ? [pa[0] intValue] : 0, ah = pa.count == 2 ? [pa[1] intValue] : 0, bw = pb.count == 2 ? [pb[0] intValue] : 0, bh = pb.count == 2 ? [pb[1] intValue] : 0;
            return aw != bw ? (aw < bw ? NSOrderedAscending : NSOrderedDescending) : ah != bh ? (ah < bh ? NSOrderedAscending : NSOrderedDescending) : NSOrderedSame;
        }];
        NSMutableDictionary *r = [rows[rid] mutableCopy]; r[@"choices"] = ch; rows[rid] = r;
    }
}
/* The Mac's own rows, not in the mod's spec (shared with Windows and Linux): Mouse Input after the game's Mouse Speed,
 * kept in registry.txt under a name the game never reads. Raw is a mouse's own counts (main.m, GameController);
 * macOS Pointer is the pointer's motion, with the Mac's tracking speed. */
static void add_mac_rows(void)
{
    NSString *rid = @"mac_mouse_input";
    rows[rid] = @{@"id": rid, @"label": @"Mouse Input", @"type": @"choice", @"reg": @"R6MacMouseInput", @"store": @"sz", @"default": @"raw",
                  @"note": @"", @"choices": @[
                      @{@"value": @"raw", @"label": @"Raw", @"note": @"The mouse's own movement, in even steps"},
                      @{@"value": @"macos", @"label": @"macOS Pointer", @"note": @"The pointer's movement, at this Mac's tracking speed"}]};
    [row_order addObject:rid];
    NSMutableArray *g = [groups mutableCopy]; BOOL placed = NO;
    for (NSUInteger k = 0; k < g.count && !placed; k++) {
        NSMutableArray *ids = [g[k][@"rows"] mutableCopy]; NSUInteger at = [ids indexOfObject:@"mouse_speed"];
        if (at == NSNotFound) continue;
        [ids insertObject:rid atIndex:at + 1]; NSMutableDictionary *d = [g[k] mutableCopy]; d[@"rows"] = ids; g[k] = d; placed = YES;
    }
    if (!placed) [rows removeObjectForKey:rid], [row_order removeObject:rid];   /* no Mouse Speed row: no panel for it */
    groups = g;
}
/* the settings the host reads, as environment (before main.m makes the Metal layer and the game starts): Frame Rate
 * (R6CoopFrameRate; the mod leaves pacing to the host) as DSR_FPSCAP, 0 the host's own, 1000 unlimited without vsync;
 * Screen (FullScreen; the game itself always draws full screen natively) as DS_WINDOW_MODE. One the player set
 * already (a terminal) wins. */
static NSString *value_of_reg(NSString *reg)
{
    for (NSString *rid in row_order) { NSString *r = str(rows[rid][@"reg"]); if (r && [r caseInsensitiveCompare:reg] == NSOrderedSame) return values[rid]; }   /* (nil compares as the same) */
    return nil;
}
static void apply(void)
{
    NSString *fps = value_of_reg(@"R6CoopFrameRate"), *full = value_of_reg(@"FullScreen");
    if ([fps isEqualToString:@"1000"]) { setenv("DSR_FPSCAP", "0", 0); setenv("DSR_VSYNC", "0", 0); }   /* overwrite 0: the player's stays */
    else if (fps.intValue > 0) setenv("DSR_FPSCAP", [NSString stringWithFormat:@"%d", fps.intValue].UTF8String, 0);
    if (full) setenv("DS_WINDOW_MODE", [full caseInsensitiveCompare:@"FALSE"] == NSOrderedSame ? "windowed" : "fullscreen", 0);
    if ([value_of_reg(@"R6MacMouseInput") isEqualToString:@"macos"]) setenv("DS_MOUSE", "macos", 0);
}

// ---------------------------------------------------------------- Play Online (docs/R6-RELAY.md)
/* Internet co-op through the relay, as linux_launcher/r6_online.py (Windows and Linux): the launcher's own rows (not in
 * the mod's spec), room codes, and what Play sets: the runtime's R6_RELAY* environment (relay.c) and the game options
 * relay play needs, each player's own value kept under R6RelaySaved.<name> and put back once Play Online is Off. */
const char *r6_launcher_exe;                                      /* main.m: the mod's RainbowSix.exe (--exe) */
static NSString *const CODE_ALPHABET = @"BCDFGHJKMPQRTVWXY2346789";   /* as r6relay.py: no vowels, no 0/O, 1/I/L or 5/S */
enum { CODE_LEN = 6 };
static NSString *const DEFAULT_RELAY = @"r6coop-relay.fly.dev:2346", *const LOCAL_PORT = @"2356", *const SAVED = @"R6RelaySaved.";
static NSArray<NSString *> *top_all;                              /* the top rows; top_ids: those shown now */
static NSString *canon(NSString *c)                               /* a code as typed: any case, dashes and spaces */
{
    NSArray *parts = [c ?: @"" componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    return [[[parts componentsJoinedByString:@""] uppercaseString] stringByReplacingOccurrencesOfString:@"-" withString:@""];
}
static BOOL valid_code(NSString *c)
{
    if (c.length != CODE_LEN) return NO;
    for (NSUInteger k = 0; k < c.length; k++) if ([CODE_ALPHABET rangeOfString:[c substringWithRange:NSMakeRange(k, 1)]].location == NSNotFound) return NO;
    return YES;
}
static NSString *pretty(NSString *c) { return valid_code(c) ? [NSString stringWithFormat:@"%@-%@", [c substringToIndex:3], [c substringFromIndex:3]] : c; }
static NSString *new_code(void)
{
    NSMutableString *c = [NSMutableString string];
    for (int k = 0; k < CODE_LEN; k++) [c appendString:[CODE_ALPHABET substringWithRange:NSMakeRange(arc4random_uniform((uint32_t)CODE_ALPHABET.length), 1)]];
    return c;
}
static BOOL row_visible(NSString *rid)                            /* "visible_if": {row: value}, all of them */
{
    NSDictionary *w = dict(rows[rid][@"visible_if"]);
    for (NSString *k in w) if (![values[k] ?: @"" isEqualToString:str(w[k]) ?: @""]) return NO;
    return YES;
}
void refresh_top(void)
{
    if (!top_all) return;
    NSMutableArray *t = [NSMutableArray array]; for (NSString *rid in top_all) if (row_visible(rid)) [t addObject:rid];
    top_ids = t;
}
static void add_online_rows(void)
{
    NSArray<NSDictionary *> *add = @[
        @{@"id": @"online_mode", @"label": @"Play Online", @"type": @"choice", @"reg": @"R6RelayMode", @"store": @"sz", @"default": @"off", @"note": @"",
          @"choices": @[@{@"value": @"off", @"label": @"Off", @"note": @"LAN games and addresses, as set in Multiplayer"},
                        @{@"value": @"host", @"label": @"Host a Room", @"note": @"Friends join with your room code; no ports or firewall to set up"},
                        @{@"value": @"join", @"label": @"Join a Room", @"note": @"Type the host's room code below"}]},
        @{@"id": @"host_code", @"label": @"Your Room Code", @"type": @"text", @"reg": @"R6RelayHostCode", @"store": @"sz", @"default": @"", @"textmax": @7,
          @"note": @"Give it to your friends. In the game: Multiplayer, Create Game", @"visible_if": @{@"online_mode": @"host"}},
        @{@"id": @"join_code", @"label": @"Room Code", @"type": @"text", @"reg": @"R6RelayJoinCode", @"store": @"sz", @"default": @"", @"textmax": @7,
          @"note": @"In the game: Multiplayer, Manual Join, Join", @"visible_if": @{@"online_mode": @"join"}},
        @{@"id": @"relay_server", @"label": @"Relay", @"type": @"text", @"reg": @"R6RelayServer", @"store": @"sz", @"default": DEFAULT_RELAY, @"textmax": @63,
          @"note": @"Only change this to use your own relay"}];
    for (NSDictionary *r in add) if (!rows[r[@"id"]]) { rows[r[@"id"]] = r; [row_order addObject:r[@"id"]]; }
    NSMutableArray *t = [top_ids mutableCopy];
    for (NSString *rid in @[@"online_mode", @"host_code", @"join_code"]) if (![t containsObject:rid]) [t addObject:rid];
    top_all = t;
    NSMutableArray *g = [groups mutableCopy]; NSInteger mp = -1;
    for (NSUInteger k = 0; k < g.count; k++) if ([g[k][@"rows"] containsObject:@"player_name"]) { mp = (NSInteger)k; break; }
    if (mp < 0) { mp = MAX(0, (NSInteger)g.count - 1); [g insertObject:@{@"title": @"ONLINE", @"rows": @[], @"guide": @NO} atIndex:(NSUInteger)mp]; }   /* before KEY GUIDE */
    NSMutableArray *ids = [g[mp][@"rows"] mutableCopy];
    if (![ids containsObject:@"relay_server"]) [ids addObject:@"relay_server"];
    NSMutableDictionary *d = [g[mp] mutableCopy]; d[@"rows"] = ids; g[mp] = d; groups = g;
}
static void prepare_online(void)                                  /* at start-up: a host's code is made once, then kept */
{
    if (rows[@"host_code"] && !valid_code(canon(values[@"host_code"]))) values[@"host_code"] = pretty(new_code());
    refresh_top();
}
static NSString *online_check(void)                               /* nil, or what is wrong with the room settings */
{
    NSString *mode = values[@"online_mode"] ?: @"off";
    if (!rows[@"online_mode"] || [mode isEqualToString:@"off"]) return nil;
    if (![values[@"relay_server"] ?: @"" stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet].length) return @"Enter the relay address in Multiplayer (Relay).";
    NSString *code = canon(values[[mode isEqualToString:@"host"] ? @"host_code" : @"join_code"]);
    if ([mode isEqualToString:@"join"] && !code.length) return @"Enter the host's room code.";
    if (!valid_code(code)) return [NSString stringWithFormat:@"A room code is %d letters and digits (no vowels, and no 0, 1 or 5), like %@.", CODE_LEN, pretty(@"K7QM4X")];
    return nil;
}
/* after the rows are written, before the file is saved: with a room, the options relay play needs (everything over the
 * one TCP connection; no UDP game list, no router mapping; a joiner's Manual Join at its bridge), each player's own
 * value saved first, once; with Play Online off, the saved values back */
static void apply_online_registry(void)
{
    if (!rows[@"online_mode"]) return;
    NSString *mode = values[@"online_mode"] ?: @"off";
    NSDictionary *always = @{@"MultiplayerBehindFirewall": @"TRUE", @"R6CoopOnline": @"FALSE", @"R6CoopRouter": @"FALSE"};
    NSDictionary *join = @{@"MultiplayerManualHostAddress": @"127.0.0.1", @"MultiplayerManualJoinPort": LOCAL_PORT};
    NSMutableDictionary *forced = [NSMutableDictionary dictionary];
    if (![mode isEqualToString:@"off"]) { [forced addEntriesFromDictionary:always]; if ([mode isEqualToString:@"join"]) [forced addEntriesFromDictionary:join]; }
    for (NSString *name in [always.allKeys arrayByAddingObjectsFromArray:join.allKeys]) {
        uint32_t st, ct; NSData *sd, *cd; BOOL saved = reg_get([SAVED stringByAppendingString:name], &st, &sd);
        if (forced[name]) {
            if (!saved) { if (reg_get(name, &ct, &cd)) reg_set([SAVED stringByAppendingString:name], ct, cd); else reg_set([SAVED stringByAppendingString:name], 0, [NSData data]); }
            NSMutableData *v = [latin1(forced[name]) mutableCopy]; [v appendBytes:"" length:1]; reg_set(name, REG_SZ, v);
        } else if (saved) {                                       /* not forced now: the player's own value back */
            if (st == 0) reg_delete(name); else reg_set(name, st, sd);
            reg_delete([SAVED stringByAppendingString:name]);
        }
    }
}
/* what the relay compares between host and joiners: the mod's RainbowSix.exe, the same file on every platform (never
 * the Mac's own binary): the first 16 hex digits of its SHA-256 */
static NSString *build_id(void)
{
    NSData *d = r6_launcher_exe ? [NSData dataWithContentsOfFile:@(r6_launcher_exe) options:NSDataReadingMappedIfSafe error:nil] : nil;
    if (!d) return @"unknown";
    uint8_t h[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(d.bytes, (CC_LONG)d.length, h);
    NSMutableString *s = [NSMutableString string]; for (int k = 0; k < 8; k++) [s appendFormat:@"%02x", h[k]];
    return s;
}
/* Play: the runtime's relay settings (relay.c), none with Play Online off */
static void relay_env(NSString *data)
{
    for (NSString *n in @[@"R6_RELAY", @"R6_ROOM", @"R6_RELAY_CODE", @"R6_RELAY_NAME", @"R6_RELAY_BUILD", @"R6_RELAY_STATUS", @"R6_RELAY_LOCAL_PORT"]) unsetenv(n.UTF8String);
    NSString *mode = values[@"online_mode"] ?: @"off";
    if (!rows[@"online_mode"] || [mode isEqualToString:@"off"]) return;
    setenv("R6_RELAY", [values[@"relay_server"] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet].UTF8String, 1);
    setenv("R6_RELAY_BUILD", build_id().UTF8String, 1);
    setenv("R6_RELAY_NAME", ([values[@"player_name"] length] ? values[@"player_name"] : @"player").UTF8String, 1);
    setenv("R6_RELAY_STATUS", [data stringByAppendingPathComponent:@"relay-status.txt"].UTF8String, 1);
    if ([mode isEqualToString:@"host"]) { setenv("R6_ROOM", "host", 1); setenv("R6_RELAY_CODE", canon(values[@"host_code"]).UTF8String, 1); }
    else { setenv("R6_ROOM", canon(values[@"join_code"]).UTF8String, 1); setenv("R6_RELAY_LOCAL_PORT", LOCAL_PORT.UTF8String, 1); }
}

/* reads r6coop-launcher.json: the game folder's, else a copy in the app; NO when there is none (or it can't be read) */
static BOOL load_spec(NSString *game)
{
    NSData *d = nil;
    for (NSString *p in @[[game stringByAppendingPathComponent:@"r6coop-launcher.json"], [NSBundle.mainBundle.resourcePath ?: @"" stringByAppendingPathComponent:@"r6coop-launcher.json"]])
        if ((d = [NSData dataWithContentsOfFile:p])) break;
    NSDictionary *s = d ? dict([NSJSONSerialization JSONObjectWithData:d options:0 error:nil]) : @{};
    if (!s.count) return NO;
    for (id r in arr(s[@"rows"])) if (str(dict(r)[@"id"]) && !rows[str(r[@"id"])]) { rows[str(r[@"id"])] = r; [row_order addObject:str(r[@"id"])]; }
    for (id t in arr(s[@"actions"])) if ([dict(t)[@"id"] isKindOfClass:NSNumber.class]) actions = [actions arrayByAddingObject:t];
    for (NSString *n in dict(s[@"layouts"])) {
        NSArray *l = arr(s[@"layouts"][n]); if (l.count != KMP_N) continue;
        NSMutableData *t = [NSMutableData dataWithLength:KMP_N]; for (NSUInteger k = 0; k < KMP_N; k++) ((uint8_t *)t.mutableBytes)[k] = (uint8_t)[str(l[k]) intValue];
        layouts[n] = t;
    }
    NSDictionary *kd = dict(s[@"keys"]);
    for (NSString *c in kd) {
        NSDictionary *k = dict(kd[c]); int code = c.intValue; if (code <= 0 || code > 255) continue;
        key_names[@(code)] = str(k[@"name"]) ?: [NSString stringWithFormat:@"#%02x", code];
        if ([k[@"vk"] isKindOfClass:NSNumber.class]) vk_to_code[k[@"vk"]] = @(code);
    }
    mouse = dict(s[@"mouse"]); version = str(s[@"version"]) ?: @"";
    if ([str(s[@"kmp_file"]) lastPathComponent].length && ![[str(s[@"kmp_file"]) lastPathComponent] hasPrefix:@"."]) kmp_file = [str(s[@"kmp_file"]) lastPathComponent];   /* a name only: never outside the folder */
    NSMutableArray *t = [NSMutableArray array];
    for (id rid in arr(s[@"top"])) if (rows[str(rid) ?: @""] && ([str(rows[rid][@"type"]) isEqualToString:@"controls"] ? layouts.count > 0 : YES)) [t addObject:rid];
    top_ids = t;
    NSMutableArray *g = [NSMutableArray array];
    for (id gd in arr(s[@"groups"])) {
        NSMutableArray *ids = [NSMutableArray array]; for (id rid in arr(dict(gd)[@"rows"])) if (rows[str(rid) ?: @""]) [ids addObject:rid];
        BOOL guide = [dict(gd)[@"guide"] boolValue];
        if (guide && !actions.count) continue;
        [g addObject:@{@"title": str(dict(gd)[@"title"]) ?: @"", @"rows": ids, @"guide": @(guide)}];
    }
    groups = g;
    return YES;
}

// ---------------------------------------------------------------- the view
enum { W = 980, H = 700, BANNER = 200, ROW_X = 48, ROW_W = 588, ROW_H = 46, LINE = 19 };   /* (the portrait to the rows' right) */
enum { HIT_NONE = -1, HIT_QUIT = 1000, HIT_PLAY = 1001, HIT_HOME = 1002, HIT_ROW = 2000, HIT_GROUP = 3000, HIT_ACTION = 4000 };
static NSColor *rgb(int r, int g, int b) { return [NSColor colorWithSRGBRed:r / 255.0 green:g / 255.0 blue:b / 255.0 alpha:1]; }
#define TEAL rgb(46, 196, 196)
#define TEAL_DIM rgb(24, 110, 112)
#define TEXT rgb(214, 236, 236)
#define NOTE rgb(120, 160, 160)
#define ACCENT rgb(255, 214, 58)
#define BG rgb(6, 12, 14)
#define PANEL [NSColor colorWithSRGBRed:10 / 255.0 green:26 / 255.0 blue:30 / 255.0 alpha:0.86]       /* (translucent over the backdrop) */
#define PANEL_HOT [NSColor colorWithSRGBRed:16 / 255.0 green:42 / 255.0 blue:46 / 255.0 alpha:0.92]
/* the Linux window's Arial at 96 dpi: Qt's points are a third larger than AppKit's */
static NSFont *arial(CGFloat px, NSString *face) { return [NSFont fontWithName:face size:px] ?: [NSFont systemFontOfSize:px weight:NSFontWeightSemibold]; }
#define F_TITLE arial(40, @"Arial-Black")
#define F_BIG arial(17, @"Arial-BoldMT")
#define F_ROW arial(14.5, @"Arial-BoldMT")
#define F_SMALL arial(12, @"ArialMT")
/* the window's type: a condensed DIN for values (the planning screens' stencilled look), condensed Avenir for the rest */
static NSFont *face(NSArray<NSString *> *names, CGFloat px) { for (NSString *n in names) { NSFont *f = [NSFont fontWithName:n size:px]; if (f) return f; } return [NSFont systemFontOfSize:px weight:NSFontWeightSemibold]; }
#define F_DISP(px) face(@[@"DINCondensed-Bold", @"AvenirNextCondensed-Bold"], px)
#define F_LBL(px) face(@[@"AvenirNextCondensed-DemiBold", @"Arial-BoldMT"], px)
#define F_BODY(px) face(@[@"AvenirNextCondensed-Medium", @"ArialMT"], px)
#define RED rgb(212, 34, 34)
/* Rainbow's four teams, as the planning screens colour them */
static NSArray<NSColor *> *team_colors(void) { return @[rgb(214, 44, 40), rgb(64, 186, 92), rgb(232, 188, 60), rgb(64, 132, 232)]; }

static void ktext(NSString *s, NSFont *f, NSColor *c, CGFloat kern, NSRect r, NSTextAlignment al)   /* one line, centred on the rect's height, elided; letter-spaced */
{
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new]; ps.alignment = al; ps.lineBreakMode = NSLineBreakByTruncatingTail;
    CGFloat h = ceil(f.ascender - f.descender);
    [s ?: @"" drawInRect:NSMakeRect(r.origin.x, r.origin.y + floor((r.size.height - h) / 2), r.size.width, h)
          withAttributes:@{NSFontAttributeName: f, NSForegroundColorAttributeName: c, NSParagraphStyleAttributeName: ps, NSKernAttributeName: @(kern)}];
}
static void draw_text(NSString *s, NSFont *f, NSColor *c, NSRect r, NSTextAlignment al) { ktext(s, f, c, 0, r, al); }
static void tri(NSPoint c, CGFloat s, BOOL right, NSColor *col)   /* the arrows of a row */
{
    CGFloat d = right ? 1 : -1; NSBezierPath *p = [NSBezierPath bezierPath];
    [p moveToPoint:NSMakePoint(c.x - d * s * 0.6, c.y - s)]; [p lineToPoint:NSMakePoint(c.x + d * s * 0.7, c.y)]; [p lineToPoint:NSMakePoint(c.x - d * s * 0.6, c.y + s)]; [p closePath];
    [col setFill]; [p fill];
}
/* corner brackets, like the game's own menus */
static void brackets(NSRect r, NSColor *c)
{
    [c setStroke]; NSBezierPath *p = [NSBezierPath bezierPath]; p.lineWidth = 1; CGFloat l = 10;
    CGFloat x0 = r.origin.x + 0.5, y0 = r.origin.y + 0.5, x1 = NSMaxX(r) - 0.5, y1 = NSMaxY(r) - 0.5;
    [p moveToPoint:NSMakePoint(x0, y0 + l)]; [p lineToPoint:NSMakePoint(x0, y0)]; [p lineToPoint:NSMakePoint(x0 + l, y0)];
    [p moveToPoint:NSMakePoint(x1 - l, y0)]; [p lineToPoint:NSMakePoint(x1, y0)]; [p lineToPoint:NSMakePoint(x1, y0 + l)];
    [p moveToPoint:NSMakePoint(x0, y1 - l)]; [p lineToPoint:NSMakePoint(x0, y1)]; [p lineToPoint:NSMakePoint(x0 + l, y1)];
    [p moveToPoint:NSMakePoint(x1 - l, y1)]; [p lineToPoint:NSMakePoint(x1, y1)]; [p lineToPoint:NSMakePoint(x1, y1 - l)];
    [p stroke];
}
static void fill(NSRect r, NSColor *c) { [c setFill]; NSRectFill(r); }

@interface DSLaunchView : NSView <NSTextFieldDelegate>
@property NSInteger open, focus, hover, bind, scroll;     /* the open group (-1 none); the action waiting for a key (-1 none) */
@property NSTextField *edit; @property NSString *editRid; @property CGFloat wheel;
@property NSImage *art, *logo, *portrait; @property BOOL logoHD; @property NSArray<NSImage *> *scenes; @property CGFloat portraitFy, portraitZ, portraitFx;
@property double t; @property NSMutableDictionary *cache;          /* the animation's clock; pictures drawn once */
@property (copy) void (^onPlay)(void); @property (copy) void (^onQuit)(void);
@end

@implementation DSLaunchView
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { return YES; }
// ---- model
- (NSArray<NSString *> *)items { return self.open < 0 ? top_ids : groups[self.open][@"rows"]; }
- (BOOL)guide { return self.open >= 0 && [groups[self.open][@"guide"] boolValue]; }
- (NSInteger)perColumn { return ((NSInteger)self.guideBox.size.height - 34) / LINE; }
- (NSInteger)pages { NSInteger per = self.perColumn * 2; return MAX(1, ((NSInteger)actions.count + per - 1) / per); }
// ---- geometry (the Linux window's)
- (NSInteger)rowH   /* the page's rows share the column under its title: up to 70 tall (the mission page), 6 apart */
{
    NSInteger n = (NSInteger)self.items.count;
    return !n ? ROW_H : MIN(self.open < 0 ? 70 : 64, (626 - 112) / n - 6);
}
- (NSRect)rowRect:(NSInteger)k { NSInteger h = self.rowH; return NSMakeRect(650, 112 + k * (h + 6), W - 650 - 20, h); }
- (NSRect)homeRect { return NSMakeRect(20, 96, 178, 42); }                       /* the rail: the mission page, then a tab per group */
- (NSRect)groupRect:(NSInteger)g { return NSMakeRect(20, 96 + (g + 1) * 48, 178, 42); }
- (NSRect)mapRect { return NSMakeRect(212, 78, 424, 548); }
- (NSRect)fileRect { NSRect last = [self groupRect:(NSInteger)groups.count - 1]; CGFloat y = groups.count ? NSMaxY(last) + 18 : 160; return NSMakeRect(20, y, 178, 626 - y); }
- (NSRect)quitRect { return NSMakeRect(20, H - 56, 150, 42); }
- (NSRect)playRect { return NSMakeRect(W - 20 - 300, H - 56, 300, 42); }
- (NSRect)guideBox { return NSMakeRect(212, 112, W - 212 - 20, 514); }
- (NSInteger)guideHit:(NSPoint)p
{
    NSRect b = self.guideBox; NSInteger per = self.perColumn, x0 = (NSInteger)b.origin.x + 12, y0 = (NSInteger)b.origin.y + 30, x = (NSInteger)p.x, y = (NSInteger)p.y;
    if (x < x0 || y < y0 || y >= y0 + per * LINE) return -1;
    NSInteger col = (x - x0) / ((NSInteger)b.size.width / 2), i = col * per + (y - y0) / LINE + self.scroll * per * 2;
    return col <= 1 && i < (NSInteger)actions.count ? i : -1;
}
- (NSInteger)hit:(NSPoint)p
{
    if (NSPointInRect(p, self.quitRect)) return HIT_QUIT;
    if (NSPointInRect(p, self.playRect)) return HIT_PLAY;
    if (NSPointInRect(p, self.homeRect)) return HIT_HOME;
    for (NSInteger g = 0; g < (NSInteger)groups.count; g++) if (NSPointInRect(p, [self groupRect:g])) return HIT_GROUP + g;
    if (self.guide) { NSInteger a = [self guideHit:p]; return a >= 0 ? HIT_ACTION + a : HIT_NONE; }
    for (NSInteger k = 0; k < (NSInteger)self.items.count; k++) if (NSPointInRect(p, [self rowRect:k])) return HIT_ROW + k;
    return HIT_NONE;
}
// ---- drawing
- (void)plate:(NSRect)r hot:(BOOL)hot   /* a panel: dark teal glass, a lit top edge, the menus' brackets; a red tab when chosen */
{
    [[[NSGradient alloc] initWithStartingColor:[NSColor colorWithSRGBRed:15 / 255.0 green:36 / 255.0 blue:40 / 255.0 alpha:hot ? 0.95 : 0.86]
                                   endingColor:[NSColor colorWithSRGBRed:5 / 255.0 green:13 / 255.0 blue:15 / 255.0 alpha:hot ? 0.95 : 0.86]] drawInRect:r angle:-90];
    [[TEAL_DIM colorWithAlphaComponent:hot ? 0.95 : 0.5] setStroke]; NSBezierPath *b = [NSBezierPath bezierPathWithRect:NSInsetRect(r, 0.5, 0.5)]; b.lineWidth = 1; [b stroke];
    [[NSColor colorWithWhite:1 alpha:hot ? 0.10 : 0.05] setFill]; NSRectFillUsingOperation(NSMakeRect(r.origin.x + 1, r.origin.y + 1, r.size.width - 2, 1), NSCompositingOperationSourceOver);
    brackets(NSInsetRect(r, -2, -2), hot ? TEAL : [TEAL_DIM colorWithAlphaComponent:0.7]);
    if (hot) fill(NSMakeRect(r.origin.x + 1, r.origin.y + 7, 3, r.size.height - 14), RED);
}
- (NSString *)playNote   /* under Play: how the game will be played */
{
    NSString *m = values[@"online_mode"];
    return [m isEqual:@"host"] ? @"HOST A ROOM" : [m isEqual:@"join"] ? @"JOIN A ROOM" : have_spec ? @"CAMPAIGN · LAN" : @"THE GAME";
}
- (void)drawButton:(NSRect)r title:(NSString *)title primary:(BOOL)primary hot:(BOOL)hot
{
    if (!primary) {
        [self plate:r hot:hot];
        ktext(title, F_DISP(21), hot ? NSColor.whiteColor : TEXT, 3, NSMakeRect(r.origin.x, r.origin.y + 3, r.size.width, r.size.height - 2), NSTextAlignmentCenter);
        return;
    }
    NSShadow *g = [NSShadow new]; g.shadowColor = [RED colorWithAlphaComponent:hot ? 0.7 : 0.45]; g.shadowBlurRadius = hot ? 18 : 12;
    [NSGraphicsContext saveGraphicsState]; [g set]; fill(r, rgb(120, 12, 14)); [NSGraphicsContext restoreGraphicsState];
    [[[NSGradient alloc] initWithStartingColor:hot ? rgb(236, 52, 48) : rgb(210, 34, 34) endingColor:hot ? rgb(150, 18, 20) : rgb(128, 12, 16)] drawInRect:r angle:-90];
    [[NSColor colorWithWhite:1 alpha:0.18] setFill]; NSRectFillUsingOperation(NSMakeRect(r.origin.x + 1, r.origin.y + 1, r.size.width - 2, 1), NSCompositingOperationSourceOver);
    [rgb(255, 120, 110) setStroke]; NSBezierPath *b = [NSBezierPath bezierPathWithRect:NSInsetRect(r, 0.5, 0.5)]; b.lineWidth = 1; [b stroke];
    CGFloat split = r.origin.x + r.size.width * 0.58;
    tri(NSMakePoint(r.origin.x + 34, NSMidY(r)), 9, YES, NSColor.whiteColor);
    ktext(title, F_DISP(27), NSColor.whiteColor, 4, NSMakeRect(r.origin.x + 48, r.origin.y + 4, split - r.origin.x - 56, r.size.height - 2), NSTextAlignmentCenter);
    fill(NSMakeRect(split, r.origin.y + 10, 1, r.size.height - 20), [NSColor colorWithWhite:1 alpha:0.45]);
    ktext(self.playNote, F_LBL(11.5), [NSColor colorWithWhite:1 alpha:0.9], 1.6, NSMakeRect(split + 8, r.origin.y, NSMaxX(r) - split - 14, r.size.height), NSTextAlignmentCenter);
}
- (void)drawRow:(NSInteger)k   /* a setting on the page: its name, the arrows, the value large, its note */
{
    NSString *rid = self.items[k]; NSRect r = [self rowRect:k]; BOOL hot = self.focus == k || self.hover == HIT_ROW + k;
    [self plate:r hot:hot];
    CGFloat x = r.origin.x, y = r.origin.y, h = r.size.height, w = r.size.width;
    BOOL text = [str(rows[rid][@"type"]) isEqualToString:@"text"];
    ktext([str(rows[rid][@"label"]) uppercaseString], F_LBL(11), hot ? TEAL : NOTE, 1.6, NSMakeRect(x + 14, y + 6, w - 96, 15), NSTextAlignmentLeft);
    NSString *val, *note; row_label(rid, &val, &note);
    if (![self.editRid isEqualToString:rid]) draw_text(val, F_DISP(h >= 56 ? 21 : 18), hot ? NSColor.whiteColor : TEXT, NSMakeRect(x + 14, y + 20, w - 28, 26), NSTextAlignmentLeft);
    else { [TEAL setStroke]; NSBezierPath *b = [NSBezierPath bezierPathWithRect:NSInsetRect(self.edit.frame, -1.5, -1.5)]; b.lineWidth = 1; [b stroke]; }
    if (h >= 54) draw_text(note, F_BODY(11.5), NOTE, NSMakeRect(x + 14, y + h - 20, w - 28, 15), NSTextAlignmentLeft);
    if (!text) {
        NSColor *ac = hot ? TEAL : TEAL_DIM; tri(NSMakePoint(NSMaxX(r) - 44, y + 14), 5.5, NO, ac); tri(NSMakePoint(NSMaxX(r) - 16, y + 14), 5.5, YES, ac);
        NSInteger n = (NSInteger)choices(rid).count, sel = choice_index(rid);
        if (n >= 2 && n <= 6) for (NSInteger i = 0; i < n; i++)
            fill(NSMakeRect(NSMaxX(r) - 60 - (n - i) * 7, y + 12, 4, 4), i == sel ? (hot ? RED : TEAL) : [TEAL_DIM colorWithAlphaComponent:0.55]);
    } else ktext(@"EDIT", F_LBL(10), TEAL_DIM, 1.5, NSMakeRect(NSMaxX(r) - 52, y + 6, 40, 15), NSTextAlignmentRight);
}
- (void)drawGuide
{
    NSRect b = self.guideBox; [self plate:b hot:NO];
    NSString *lay = @{@"classic": @"Classic", @"modern": @"Modern", @"keyboard": @"Keyboard Only"}[layout_name()] ?: @"Custom";
    draw_text([NSString stringWithFormat:@"Layout: %@.  Click an action, then press a key or mouse button (Esc cancels, fn-Delete clears).", lay],
              F_BODY(12.5), NOTE, NSMakeRect(b.origin.x + 12, b.origin.y + 6, b.size.width - 24 - (self.pages > 1 ? 80 : 0), 18), NSTextAlignmentLeft);
    if (self.pages > 1) draw_text([NSString stringWithFormat:@"page %ld of %ld", (long)self.scroll + 1, (long)self.pages], F_SMALL, TEAL,
                                  NSMakeRect(NSMaxX(b) - 92, b.origin.y + 6, 80, 18), NSTextAlignmentRight);
    NSInteger per = self.perColumn; const uint8_t *km = keys.bytes; CGFloat cw = b.size.width / 2;
    for (NSInteger i = 0; i < (NSInteger)actions.count; i++) {
        NSInteger k = i - self.scroll * per * 2; if (k < 0 || k >= per * 2) continue;
        NSInteger col = k / per, line = k % per; CGFloat x = b.origin.x + 12 + col * cw, y = b.origin.y + 30 + line * LINE;
        int aid = [actions[i][@"id"] intValue]; NSMutableArray *bound = [NSMutableArray array];
        for (int c = 0; c < 256; c++) if (km[1 + c] == aid) [bound addObject:key_names[@(c)] ?: [NSString stringWithFormat:@"#%02x", c]];
        BOOL binding = self.bind == i;
        NSString *s = binding ? @"press a key..." : bound.count ? [bound componentsJoinedByString:@", "] : @"-";
        draw_text(str(actions[i][@"name"]), F_BODY(13), binding ? ACCENT : self.hover == HIT_ACTION + i ? TEXT : NOTE, NSMakeRect(x, y, 182, LINE), NSTextAlignmentLeft);
        draw_text(s, F_DISP(14), binding ? ACCENT : TEXT, NSMakeRect(x + 186, y, cw - 24 - 186, LINE), NSTextAlignmentLeft);   /* Linux: 210 / 214; room for "RIGHT MOUSE" */
    }
}
// ---- the moving part (see the top of the file)
static double hash01(double x) { double s = sin(x * 12.9898) * 43758.5453; return s - floor(s); }
static NSImage *offscreen(NSSize size, CGFloat scale, void (^draw)(void))
{
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:(NSInteger)ceil(size.width * scale) pixelsHigh:(NSInteger)ceil(size.height * scale)
                                                                 bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSCalibratedRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
    rep = [rep bitmapImageRepByRetaggingWithColorSpace:NSColorSpace.sRGBColorSpace] ?: rep;
    rep.size = size;
    CGContextRef cg = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep].CGContext;
    [NSGraphicsContext saveGraphicsState];
    CGContextTranslateCTM(cg, 0, size.height); CGContextScaleCTM(cg, 1, -1);
    [NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithCGContext:cg flipped:YES]];
    draw();
    [NSGraphicsContext restoreGraphicsState];
    NSImage *img = [[NSImage alloc] initWithSize:size]; [img addRepresentation:rep];
    return img;
}
static void draw_fill_z(NSImage *img, NSRect r, CGFloat alpha, CGFloat z, CGFloat fx, CGFloat fy, NSImageInterpolation q)
{
    if (!img) return; NSSize s = img.size; if (s.width <= 0 || s.height <= 0) return;
    CGFloat k = MAX(r.size.width / s.width, r.size.height / s.height) * z, w = r.size.width / k, h = r.size.height / k;
    [img drawInRect:r fromRect:NSMakeRect((s.width - w) * fx, (s.height - h) * (1 - fy), w, h) operation:NSCompositingOperationSourceOver fraction:alpha respectFlipped:YES hints:@{NSImageHintInterpolation: @(q)}];
}
- (CGFloat)scale { return self.window ? self.window.backingScaleFactor : 2; }
- (NSRect)portraitRect { NSRect f = self.fileRect; return NSMakeRect(f.origin.x + 10, f.origin.y + 26, f.size.width - 20, f.size.height - 58); }   /* the dossier's photo */
- (NSImage *)sprite:(NSString *)key colors:(NSArray<NSColor *> *)cols
{
    NSImage *c = self.cache[key]; if (c) return c;
    c = offscreen(NSMakeSize(128, 128), 1, ^{ [[[NSGradient alloc] initWithColors:cols] drawFromCenter:NSMakePoint(64, 64) radius:0 toCenter:NSMakePoint(64, 64) radius:64 options:0]; });
    self.cache[key] = c; return c;
}
static void draw_sprite(NSImage *img, NSPoint c, CGFloat rx, CGFloat ry, CGFloat alpha)
{
    if (alpha <= 0.002) return;
    [img drawInRect:NSMakeRect(c.x - rx, c.y - ry, 2 * rx, 2 * ry) fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:MIN(1, alpha) respectFlipped:YES hints:@{NSImageHintInterpolation: @(NSImageInterpolationLow)}];
}
- (void)backdrop
{
    NSArray<NSImage *> *sc = self.scenes; NSUInteger n = sc.count; NSRect b = self.bounds; if (!n) return; double tt = MAX(0.0, self.t);
    double per = 8.0, fadeT = 1.6;
    void (^slide)(NSUInteger, double, CGFloat) = ^(NSUInteger j, double local, CGFloat a) {
        double p = local / (per + fadeT); CGFloat dir = (j % 2) ? 1 : -1;
        draw_fill_z(sc[j % n], b, a, (CGFloat)(1.04 + 0.07 * p), (CGFloat)(0.5 + dir * 0.10 * (p - 0.5)), (CGFloat)(0.45 + 0.05 * (p - 0.5)), NSImageInterpolationMedium);
    };
    if (n == 1) { draw_fill_z(sc[0], b, 1, (CGFloat)(1.08 + 0.035 * sin(tt * 0.11)), (CGFloat)(0.5 + 0.06 * sin(tt * 0.07)), 0.45, NSImageInterpolationMedium); return; }
    NSUInteger k = (NSUInteger)(tt / per); double local = tt - (double)k * per;
    if (local < fadeT && k > 0) { slide(k - 1, local + per, 1); slide(k, local, (CGFloat)(local / fadeT)); }
    else slide(k, local, 1);
}
- (NSImage *)gradeImage   /* drawn once: the banner lighter, the rows' side dark, the edges darker */
{
    NSImage *c = self.cache[@"grade"]; if (c) return c;
    NSRect b = self.bounds;
    c = offscreen(b.size, 1, ^{
        [[NSColor colorWithSRGBRed:0.02 green:0.10 blue:0.11 alpha:0.18] setFill]; NSRectFillUsingOperation(b, NSCompositingOperationSourceOver);
        NSGradient *down = [[NSGradient alloc] initWithColorsAndLocations:[NSColor colorWithSRGBRed:0.01 green:0.03 blue:0.04 alpha:0.10], 0.0, [NSColor colorWithSRGBRed:0.01 green:0.03 blue:0.04 alpha:0.18], 0.24,
                            [NSColor colorWithSRGBRed:0.01 green:0.03 blue:0.04 alpha:0.62], 0.36, [NSColor colorWithSRGBRed:0.01 green:0.03 blue:0.04 alpha:0.80], 0.86, [NSColor colorWithSRGBRed:0.01 green:0.03 blue:0.04 alpha:0.93], 1.0, nil];
        [down drawInRect:b angle:-90];
        NSGradient *side = [[NSGradient alloc] initWithColorsAndLocations:[NSColor colorWithWhite:0 alpha:0.65], 0.0, [NSColor colorWithWhite:0 alpha:0.30], 0.45, [NSColor colorWithWhite:0 alpha:0.0], 0.8, nil];
        [side drawInRect:NSMakeRect(0, 0, W, BANNER) angle:0];
    });
    self.cache[@"grade"] = c; return c;
}
- (NSString *)signature
{
    return [NSString stringWithFormat:@"%ld|%ld|%ld|%ld|%ld|%@|%lu|%lu|%.0f|%lu", (long)self.open, (long)self.focus, (long)self.hover, (long)self.bind, (long)self.scroll,
            self.editRid ?: @"", (unsigned long)values.description.hash, (unsigned long)keys.hash, self.scale, (unsigned long)top_ids.count];
}
static int frames_drawn;
/* the floor plan, in the map's own 0..1 coordinates: rooms (x, y, w, h), and each team's route through them */
static const CGFloat ROOMS[][4] = {
    {0.06, 0.06, 0.40, 0.22}, {0.46, 0.06, 0.24, 0.22}, {0.70, 0.06, 0.24, 0.30}, {0.06, 0.28, 0.20, 0.30}, {0.26, 0.28, 0.30, 0.16},
    {0.26, 0.44, 0.30, 0.14}, {0.56, 0.28, 0.14, 0.30}, {0.70, 0.36, 0.24, 0.22}, {0.06, 0.58, 0.34, 0.20}, {0.40, 0.58, 0.30, 0.20},
    {0.70, 0.58, 0.24, 0.20}, {0.06, 0.78, 0.22, 0.16}, {0.28, 0.78, 0.42, 0.16}, {0.70, 0.78, 0.24, 0.16}};
static NSString *const ROOM_NAMES[] = {@"LOBBY", @"SECURITY", @"OFFICES", @"ARCHIVE", @"HALL", @"HALL", @"STAIRS", @"CONFERENCE", @"KITCHEN", @"SERVER ROOM",
                                       @"VAULT", @"GARAGE", @"LOADING DOCK", @"GENERATOR"};
static const CGFloat ROUTE[4][7][2] = {
    {{0.02, 0.17}, {0.20, 0.17}, {0.36, 0.36}, {0.48, 0.36}, {0.63, 0.43}, {0.80, 0.47}, {0.82, 0.68}},          /* Red: lobby to the vault */
    {{0.17, 0.98}, {0.17, 0.86}, {0.48, 0.86}, {0.55, 0.68}, {0.55, 0.68}, {0.55, 0.68}, {0.55, 0.68}},          /* Green: the dock to the servers */
    {{0.98, 0.20}, {0.82, 0.20}, {0.58, 0.17}, {0.63, 0.32}, {0.63, 0.50}, {0.82, 0.47}, {0.82, 0.47}},          /* Gold: over the roof, down the stairs */
    {{0.02, 0.68}, {0.22, 0.68}, {0.22, 0.44}, {0.16, 0.36}, {0.16, 0.36}, {0.16, 0.36}, {0.16, 0.36}}};         /* Blue: the archive */
static NSPoint mpt(NSRect m, const CGFloat *q) { return NSMakePoint(m.origin.x + q[0] * m.size.width, m.origin.y + q[1] * m.size.height); }
static double route_len(int t, int upto, NSRect m)
{ double L = 0; for (int i = 1; i <= upto && i < 7; i++) { NSPoint a = mpt(m, ROUTE[t][i - 1]), b = mpt(m, ROUTE[t][i]); L += hypot(b.x - a.x, b.y - a.y); } return L; }
static NSPoint route_at(int t, double d, NSRect m)   /* the point d along team t's route */
{
    for (int i = 1; i < 7; i++) { NSPoint a = mpt(m, ROUTE[t][i - 1]), b = mpt(m, ROUTE[t][i]); double l = hypot(b.x - a.x, b.y - a.y);
        if (d <= l && l > 0) return NSMakePoint(a.x + (b.x - a.x) * d / l, a.y + (b.y - a.y) * d / l); d -= l; }
    return mpt(m, ROUTE[t][6]);
}
- (NSImage *)planImage   /* drawn once: the blueprint, its grid, the building's walls and doors, rooms named, compass and scale */
{
    NSImage *c = self.cache[@"plan"]; if (c) return c;
    NSRect m = self.mapRect;
    c = offscreen(self.bounds.size, self.scale, ^{
        [[[NSGradient alloc] initWithStartingColor:rgb(8, 28, 38) endingColor:rgb(4, 14, 20)] drawInRect:m relativeCenterPosition:NSMakePoint(0, 0.2)];
        for (CGFloat gx = 0; gx <= m.size.width; gx += 14) fill(NSMakeRect(m.origin.x + gx, m.origin.y, 1, m.size.height), [TEAL colorWithAlphaComponent:fmod(gx, 70) < 1 ? 0.13 : 0.05]);
        for (CGFloat gy = 0; gy <= m.size.height; gy += 14) fill(NSMakeRect(m.origin.x, m.origin.y + gy, m.size.width, 1), [TEAL colorWithAlphaComponent:fmod(gy, 70) < 1 ? 0.13 : 0.05]);
        int nr = (int)(sizeof ROOMS / sizeof *ROOMS);
        for (int i = 0; i < nr; i++) {
            NSRect r = NSMakeRect(m.origin.x + ROOMS[i][0] * m.size.width, m.origin.y + ROOMS[i][1] * m.size.height, ROOMS[i][2] * m.size.width, ROOMS[i][3] * m.size.height);
            [[TEAL colorWithAlphaComponent:0.06] setFill]; NSRectFillUsingOperation(NSInsetRect(r, 2, 2), NSCompositingOperationSourceOver);
            NSBezierPath *wall = [NSBezierPath bezierPathWithRect:NSInsetRect(r, 1, 1)]; wall.lineWidth = 2; [[rgb(150, 220, 226) colorWithAlphaComponent:0.55] setStroke]; [wall stroke];
            [rgb(6, 20, 28) setFill];                                     /* doors: gaps in the walls, their swing drawn */
            NSRect door = i % 2 ? NSMakeRect(NSMaxX(r) - 2.5, r.origin.y + r.size.height * 0.4, 4, 14) : NSMakeRect(r.origin.x + r.size.width * 0.45, NSMaxY(r) - 2.5, 14, 4);
            NSRectFill(door);
            NSBezierPath *sw = [NSBezierPath bezierPath]; sw.lineWidth = 0.75; [[TEAL colorWithAlphaComponent:0.35] setStroke];
            if (i % 2) [sw appendBezierPathWithArcWithCenter:NSMakePoint(door.origin.x + 2, door.origin.y) radius:14 startAngle:90 endAngle:180 clockwise:NO];
            else [sw appendBezierPathWithArcWithCenter:NSMakePoint(door.origin.x, door.origin.y + 2) radius:14 startAngle:270 endAngle:360 clockwise:NO];
            [sw stroke];
            if (i == 6) for (CGFloat sy = r.origin.y + 6; sy < NSMaxY(r) - 4; sy += 6) fill(NSMakeRect(r.origin.x + 5, sy, r.size.width - 10, 1), [TEAL colorWithAlphaComponent:0.30]);   /* the stairs' treads */
            ktext(ROOM_NAMES[i], F_LBL(8.5), [TEAL colorWithAlphaComponent:0.55], 1.4, NSMakeRect(r.origin.x + 6, r.origin.y + 5, r.size.width - 10, 11), NSTextAlignmentLeft);
        }
        ktext(@"PLANNING  ·  EMBASSY  ·  FLOOR 1", F_LBL(10), TEAL, 2, NSMakeRect(m.origin.x + 10, NSMaxY(m) - 18, 260, 13), NSTextAlignmentLeft);
        NSPoint cc = NSMakePoint(NSMaxX(m) - 26, m.origin.y + 28);   /* the compass */
        NSBezierPath *ring = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(cc.x - 14, cc.y - 14, 28, 28)]; ring.lineWidth = 1; [[TEAL colorWithAlphaComponent:0.6] setStroke]; [ring stroke];
        NSBezierPath *nd = [NSBezierPath bezierPath]; [nd moveToPoint:NSMakePoint(cc.x, cc.y - 12)]; [nd lineToPoint:NSMakePoint(cc.x + 4, cc.y)]; [nd lineToPoint:NSMakePoint(cc.x - 4, cc.y)]; [nd closePath]; [RED setFill]; [nd fill];
        ktext(@"N", F_LBL(9), TEAL, 0, NSMakeRect(cc.x - 6, cc.y - 27, 12, 11), NSTextAlignmentCenter);
        for (int k = 0; k < 4; k++) fill(NSMakeRect(NSMaxX(m) - 96 + k * 18, NSMaxY(m) - 14, 18, 3), k % 2 ? [TEAL colorWithAlphaComponent:0.3] : TEAL);   /* the scale bar */
        ktext(@"10 M", F_LBL(8.5), TEAL, 1, NSMakeRect(NSMaxX(m) - 96, NSMaxY(m) - 27, 72, 11), NSTextAlignmentRight);
        brackets(NSInsetRect(m, -3, -3), TEAL);
        NSBezierPath *edge = [NSBezierPath bezierPathWithRect:NSInsetRect(m, 0.5, 0.5)]; edge.lineWidth = 1; [[TEAL_DIM colorWithAlphaComponent:0.9] setStroke]; [edge stroke];
    });
    self.cache[@"plan"] = c; return c;
}
- (void)drawRect:(NSRect)dirty
{
    NSRect b = self.bounds; double T = self.t; frames_drawn++;
    if (!self.cache) self.cache = [NSMutableDictionary dictionary];
    if (![self.cache[@"scale"] isEqual:@(self.scale)]) { [self.cache removeAllObjects]; self.cache[@"scale"] = @(self.scale); }
    fill(b, rgb(3, 9, 12));
    [NSGraphicsContext saveGraphicsState]; [[NSColor colorWithWhite:1 alpha:1] set];   /* the movie's frames, far back and dim */
    [self backdrop]; [NSGraphicsContext restoreGraphicsState];
    [[NSColor colorWithSRGBRed:0.01 green:0.04 blue:0.05 alpha:0.86] setFill]; NSRectFillUsingOperation(b, NSCompositingOperationSourceOver);
    NSRect m = self.mapRect;
    if (!self.guide) {
        [self.planImage drawInRect:b fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1 respectFlipped:YES hints:nil];
        [NSGraphicsContext saveGraphicsState]; NSRectClip(m);
        {   /* a scan sweeping across the plan */
            double ph = fmod(T * 0.12, 1.0); CGFloat x = m.origin.x + ph * (m.size.width + 120) - 60;
            [[[NSGradient alloc] initWithColors:@[[TEAL colorWithAlphaComponent:0], [TEAL colorWithAlphaComponent:0.10], [TEAL colorWithAlphaComponent:0]]] drawInRect:NSMakeRect(x - 60, m.origin.y, 120, m.size.height) angle:0];
        }
        NSArray<NSColor *> *tc = team_colors(); static NSString *const GO[] = {@"ALPHA", @"BRAVO", @"CHARLIE", @"DELTA"};
        for (int t = 0; t < 4; t++) {   /* each team's route, its waypoints, the team moving along it, its go code at the last */
            int last = 6; while (last > 0 && ROUTE[t][last][0] == ROUTE[t][last - 1][0] && ROUTE[t][last][1] == ROUTE[t][last - 1][1]) last--;
            double L = route_len(t, last, m), cyc = 16 + 3 * t, ph = fmod(T + t * 2.3, cyc) / cyc, d = MIN(1.0, ph / 0.8) * L, fade = ph < 0.9 ? 1 : 1 - (ph - 0.9) / 0.1;
            NSBezierPath *path = [NSBezierPath bezierPath]; path.lineWidth = 1.6; CGFloat dash[] = {5, 4}; [path setLineDash:dash count:2 phase:(CGFloat)(-T * 10)];
            NSPoint a0 = mpt(m, ROUTE[t][0]); [path moveToPoint:a0];
            for (int i = 1; i <= last; i++) [path lineToPoint:mpt(m, ROUTE[t][i])];
            [[tc[t] colorWithAlphaComponent:0.45] setStroke]; [path stroke];
            for (int i = 1; i <= last; i++) {
                NSPoint c = mpt(m, ROUTE[t][i]); BOOL reached = route_len(t, i, m) <= d;
                NSBezierPath *dm = [NSBezierPath bezierPath]; [dm moveToPoint:NSMakePoint(c.x, c.y - 4.5)]; [dm lineToPoint:NSMakePoint(c.x + 4.5, c.y)]; [dm lineToPoint:NSMakePoint(c.x, c.y + 4.5)]; [dm lineToPoint:NSMakePoint(c.x - 4.5, c.y)]; [dm closePath];
                [[tc[t] colorWithAlphaComponent:reached ? 0.95 : 0.4] setFill]; [dm fill];
                draw_text([NSString stringWithFormat:@"%d", i], F_LBL(8), [NSColor colorWithWhite:1 alpha:reached ? 0.75 : 0.35], NSMakeRect(c.x + 5, c.y - 12, 14, 10), NSTextAlignmentLeft);
            }
            NSPoint g = mpt(m, ROUTE[t][last]); BOOL hold = d >= L;   /* the go code: waiting, then flashing when they arrive */
            NSRect fl = NSMakeRect(g.x + 8, g.y + 2, 44, 12);
            [[tc[t] colorWithAlphaComponent:hold ? 0.55 + 0.4 * (0.5 + 0.5 * sin(T * 8)) : 0.18] setFill]; NSRectFillUsingOperation(fl, NSCompositingOperationSourceOver);
            ktext(GO[t], F_LBL(8), NSColor.whiteColor, 1, NSMakeRect(fl.origin.x, fl.origin.y, fl.size.width, 12), NSTextAlignmentCenter);
            for (int k = 6; k >= 0; k--) {                               /* the team: four dots in file, a fading trail */
                NSPoint q = route_at(t, MAX(0.0, d - k * 7), m); CGFloat r = k == 0 ? 4 : 2.6;
                [[tc[t] colorWithAlphaComponent:(k == 0 ? 1 : 0.55 - 0.07 * k) * fade] setFill];
                [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(q.x - r, q.y - r, 2 * r, 2 * r)] fill];
            }
            NSPoint lead = route_at(t, d, m);
            draw_sprite([self sprite:[NSString stringWithFormat:@"led%d", t] colors:@[[tc[t] colorWithAlphaComponent:0.8], [tc[t] colorWithAlphaComponent:0.2], [NSColor colorWithWhite:0 alpha:0]]], lead, 12, 12, 0.7 * fade);
        }
        {   /* the surveillance feed: the game's own movie frames on a monitor, live */
            NSRect cam = NSMakeRect(m.origin.x + 12, NSMaxY(m) - 36 - 112, 182, 112), in = NSInsetRect(cam, 4, 4);
            fill(cam, rgb(2, 6, 8)); NSArray<NSImage *> *sc = self.scenes;
            if (sc.count) {
                NSUInteger k = (NSUInteger)(T / 3.5) % sc.count; double f = fmod(T, 3.5);
                [NSGraphicsContext saveGraphicsState]; NSRectClip(in);
                draw_fill_z(sc[k], in, 1, (CGFloat)(1.1 + 0.03 * f), 0.5, 0.5, NSImageInterpolationMedium);
                if (f < 0.25) { [[NSColor colorWithWhite:1 alpha:(0.25 - f) * 1.6] setFill]; NSRectFillUsingOperation(in, NSCompositingOperationSourceOver); }   /* the cut: a flash of static */
                [[NSColor colorWithSRGBRed:0.2 green:1 blue:0.8 alpha:0.10] setFill]; NSRectFillUsingOperation(in, NSCompositingOperationSourceOver);
                [[NSColor colorWithWhite:0 alpha:0.22] setFill]; for (CGFloat ly = in.origin.y; ly < NSMaxY(in); ly += 2) NSRectFillUsingOperation(NSMakeRect(in.origin.x, ly, in.size.width, 1), NSCompositingOperationSourceOver);
                [NSGraphicsContext restoreGraphicsState];
            }
            brackets(cam, TEAL);
            if (fmod(T, 1.0) < 0.6) { [RED setFill]; [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(in.origin.x + 6, in.origin.y + 6, 6, 6)] fill]; }
            ktext(@"CAM 02  ·  LIVE", F_LBL(8.5), NSColor.whiteColor, 1.4, NSMakeRect(in.origin.x + 16, in.origin.y + 3, 100, 11), NSTextAlignmentLeft);
            int secs = (int)(T * 1) + 41520; ktext([NSString stringWithFormat:@"%02d:%02d:%02d", secs / 3600 % 24, secs / 60 % 60, secs % 60], F_LBL(8.5), NSColor.whiteColor, 1, NSMakeRect(NSMaxX(in) - 64, NSMaxY(in) - 14, 58, 11), NSTextAlignmentRight);
        }
        [NSGraphicsContext restoreGraphicsState];
    }
    {   /* the dossier's photo (its frame is in the still part), a scan sweeping down it */
        NSRect in = self.portraitRect;
        [NSGraphicsContext saveGraphicsState]; NSRectClip(in); fill(in, BG);
        draw_fill_z(self.portrait, in, 1, (CGFloat)(self.portraitZ + 0.02 * sin(T * 0.21)), (CGFloat)(self.portraitFx + 0.01 * sin(T * 0.13)), self.portraitFy, NSImageInterpolationHigh);
        double ph = fmod(T * 0.22, 1.0); CGFloat y = in.origin.y + ph * in.size.height;
        [[[NSGradient alloc] initWithColors:@[[TEAL colorWithAlphaComponent:0], [TEAL colorWithAlphaComponent:0.18], [TEAL colorWithAlphaComponent:0]]] drawInRect:NSMakeRect(in.origin.x, y - 20, in.size.width, 40) angle:90];
        [[NSColor colorWithWhite:0 alpha:0.12] setFill]; for (CGFloat ly = in.origin.y; ly < NSMaxY(in); ly += 3) NSRectFillUsingOperation(NSMakeRect(in.origin.x, ly, in.size.width, 1), NSCompositingOperationSourceOver);
        [NSGraphicsContext restoreGraphicsState];
    }
    NSString *sig = self.signature;
    if (!self.cache[@"ui"] || ![self.cache[@"uiSig"] isEqual:sig]) { self.cache[@"ui"] = offscreen(b.size, self.scale, ^{ [self drawStill]; }); self.cache[@"uiSig"] = sig; }
    [(NSImage *)self.cache[@"ui"] drawInRect:b fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1 respectFlipped:YES hints:nil];
    {   /* the teams' lights in the top bar, pulsing out of step; the mission clock */
        NSArray<NSColor *> *tc = team_colors();
        for (NSInteger i = 0; i < 4; i++) {
            NSPoint c = NSMakePoint(W - 20 - 400 + 14 + i * 70, 34); double a = 0.55 + 0.45 * (0.5 + 0.5 * sin(T * 2.4 + i * 1.3));
            draw_sprite([self sprite:[NSString stringWithFormat:@"led%ld", (long)i] colors:@[[tc[i] colorWithAlphaComponent:0.9], [tc[i] colorWithAlphaComponent:0.25], [NSColor colorWithWhite:0 alpha:0]]], c, 9, 9, a);
            fill(NSMakeRect(c.x - 2.5, c.y - 2.5, 5, 5), [tc[i] blendedColorWithFraction:0.35 ofColor:NSColor.whiteColor]);
        }
        int secs = (int)MAX(0.0, T);
        ktext([NSString stringWithFormat:@"%02d:%02d:%02d", secs / 3600, secs / 60 % 60, secs % 60], F_DISP(17), TEAL, 1.5, NSMakeRect(W - 20 - 80, 24, 80, 22), NSTextAlignmentRight);
    }
    {   /* Execute: its brackets breathing */
        CGFloat d = 4 + 2 * sin(T * 2.2); brackets(NSInsetRect(self.playRect, -d, -d), [rgb(255, 96, 86) colorWithAlphaComponent:0.35 + 0.3 * (0.5 + 0.5 * sin(T * 2.2))]);
    }
}
- (void)drawStill
{
    {   /* the top bar: the title, the credit, the teams */
        [[[NSGradient alloc] initWithStartingColor:rgb(10, 26, 30) endingColor:rgb(4, 12, 15)] drawInRect:NSMakeRect(0, 0, W, 64) angle:-90];
        fill(NSMakeRect(0, 64, W, 1), TEAL_DIM); fill(NSMakeRect(20, 64, 60, 2), RED);
        if (self.logo) {
            NSSize ls = self.logo.size; CGFloat lh = 50, lw = lh * ls.width / MAX(1, ls.height);
            NSShadow *sh = [NSShadow new]; sh.shadowColor = [NSColor colorWithWhite:0 alpha:0.9]; sh.shadowBlurRadius = 6;
            [NSGraphicsContext saveGraphicsState]; [sh set];
            [self.logo drawInRect:NSMakeRect(16, 7, lw, lh) fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1 respectFlipped:YES hints:@{NSImageHintInterpolation: @(NSImageInterpolationHigh)}];
            [NSGraphicsContext restoreGraphicsState];
        } else ktext(@"RAINBOW SIX", F_DISP(28), NSColor.whiteColor, 3, NSMakeRect(20, 18, 200, 30), NSTextAlignmentLeft);
        fill(NSMakeRect(140, 16, 1, 34), [TEAL_DIM colorWithAlphaComponent:0.8]);
        ktext(@"RED STORM ENTERTAINMENT  ·  1998", F_LBL(11.5), TEAL, 2.4, NSMakeRect(154, 15, 330, 15), NSTextAlignmentLeft);
        ktext(have_spec ? [NSString stringWithFormat:@"CO-OP EDITION %@  ·  NATIVE FOR APPLE SILICON  ·  METAL", version] : @"NATIVE FOR APPLE SILICON  ·  METAL", F_LBL(9.5), NOTE, 1.8,
              NSMakeRect(154, 34, 400, 13), NSTextAlignmentLeft);
        NSArray *names = @[@"RED", @"GREEN", @"GOLD", @"BLUE"];
        for (NSInteger i = 0; i < 4; i++) ktext(names[i], F_DISP(14), TEXT, 1.8, NSMakeRect(W - 20 - 400 + 24 + i * 70, 26, 46, 18), NSTextAlignmentLeft);
        fill(NSMakeRect(W - 20 - 96, 18, 1, 30), [TEAL_DIM colorWithAlphaComponent:0.8]);
        ktext(@"MISSION", F_LBL(8.5), NOTE, 1.6, NSMakeRect(W - 20 - 80, 13, 80, 11), NSTextAlignmentRight);
    }
    {   /* the rail: the mission page, a tab per group */
        ktext(@"PLANNING", F_LBL(10), [TEAL colorWithAlphaComponent:0.8], 2.4, NSMakeRect(20, 78, 178, 13), NSTextAlignmentLeft);
        NSMutableArray *tabs = [NSMutableArray arrayWithObject:@"MISSION"]; for (NSDictionary *g in groups) [tabs addObject:[g[@"title"] uppercaseString]];
        for (NSInteger i = 0; i < (NSInteger)tabs.count; i++) {
            NSRect r = i ? [self groupRect:i - 1] : self.homeRect;
            BOOL on = i ? self.open == i - 1 : self.open < 0, hot = i ? (self.hover == HIT_GROUP + i - 1 || (self.open < 0 && self.focus == (NSInteger)top_ids.count + i - 1)) : self.hover == HIT_HOME;
            [self plate:r hot:on || hot];
            if (on) { fill(NSMakeRect(NSMaxX(r) - 4, r.origin.y + 6, 4, r.size.height - 12), RED);
                      NSBezierPath *nub = [NSBezierPath bezierPath]; [nub moveToPoint:NSMakePoint(NSMaxX(r) + 2, NSMidY(r) - 7)]; [nub lineToPoint:NSMakePoint(NSMaxX(r) + 10, NSMidY(r))]; [nub lineToPoint:NSMakePoint(NSMaxX(r) + 2, NSMidY(r) + 7)]; [nub closePath]; [RED setFill]; [nub fill]; }
            ktext([NSString stringWithFormat:@"%02ld", (long)i + 1], F_DISP(15), on ? RED : TEAL_DIM, 1, NSMakeRect(r.origin.x + 12, r.origin.y + 12, 26, 20), NSTextAlignmentLeft);
            ktext(tabs[i], F_DISP(17), on ? NSColor.whiteColor : hot ? TEXT : NOTE, 2.2, NSMakeRect(r.origin.x + 40, r.origin.y + 12, r.size.width - 50, 22), NSTextAlignmentLeft);
        }
        NSRect f = self.fileRect;   /* the dossier under the tabs: the key art's operative, clipped in */
        if (f.size.height > 90) {
            [self plate:f hot:NO];
            ktext(@"OPERATIVE FILE", F_LBL(9.5), TEAL, 2, NSMakeRect(f.origin.x + 10, f.origin.y + 8, f.size.width - 20, 12), NSTextAlignmentLeft);
            ktext(@"CLASSIFIED", F_LBL(8.5), RED, 2, NSMakeRect(f.origin.x + 10, f.origin.y + 8, f.size.width - 20, 12), NSTextAlignmentRight);
            NSRect ph = self.portraitRect; NSRectFillUsingOperation(ph, NSCompositingOperationClear);
            brackets(NSInsetRect(ph, -2, -2), TEAL);
            ktext(@"TEAM RAINBOW", F_DISP(16), NSColor.whiteColor, 2.4, NSMakeRect(f.origin.x + 10, NSMaxY(f) - 27, f.size.width - 20, 20), NSTextAlignmentLeft);
        }
    }
    {   /* the page: its title, its rows (or the key guide over the map and the page) */
        NSString *title = self.open < 0 ? @"MISSION SETUP" : [groups[self.open][@"title"] uppercaseString];
        if (!self.guide) {
            ktext(title, F_DISP(20), NSColor.whiteColor, 2.6, NSMakeRect(650, 80, W - 670, 24), NSTextAlignmentLeft);
            fill(NSMakeRect(650, 104, 40, 2), RED); fill(NSMakeRect(694, 104.5, W - 714 - 694 + 650, 1), [TEAL_DIM colorWithAlphaComponent:0.8]);
            for (NSInteger k = 0; k < (NSInteger)self.items.count; k++) [self drawRow:k];
            if (!have_spec) draw_text(@"No r6coop-launcher.json next to RainbowSix.exe: the game's own Options screen has its settings.", F_BODY(12), NOTE, NSMakeRect(650, 120, W - 670, 36), NSTextAlignmentLeft);
            if (self.open < 0) {   /* the mission page: a briefing under the rows */
                CGFloat y0 = self.items.count ? NSMaxY([self rowRect:(NSInteger)self.items.count - 1]) + 16 : 130;
                NSRect br = NSMakeRect(650, y0, W - 670, 626 - y0);
                if (br.size.height > 150) {
                    [self plate:br hot:NO];
                    ktext(@"BRIEFING", F_LBL(10.5), TEAL, 2.4, NSMakeRect(br.origin.x + 14, br.origin.y + 10, 120, 13), NSTextAlignmentLeft);
                    ktext(@"JOHN CLARK  ·  RAINBOW SIX", F_LBL(9.5), NOTE, 1.6, NSMakeRect(br.origin.x + 14, br.origin.y + 10, br.size.width - 28, 13), NSTextAlignmentRight);
                    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new]; ps.lineSpacing = 2;
                    [@"A multinational counter-terror unit. Every assault is planned on the map to the second: waypoints, go codes and rules of engagement for up to four teams, then executed. One shot can end a mission."
                        drawWithRect:NSMakeRect(br.origin.x + 14, br.origin.y + 32, br.size.width - 28, 92) options:NSStringDrawingUsesLineFragmentOrigin
                        attributes:@{NSFontAttributeName: F_BODY(13.5), NSForegroundColorAttributeName: rgb(196, 218, 216), NSParagraphStyleAttributeName: ps}];
                    NSArray *obj = @[@"Rescue the hostages", @"Neutralise every tango", @"Bring the team home"];
                    CGFloat oy = br.origin.y + 132; ktext(@"OBJECTIVES", F_LBL(10), TEAL, 2.2, NSMakeRect(br.origin.x + 14, oy, 140, 13), NSTextAlignmentLeft);
                    for (NSUInteger i = 0; i < obj.count && oy + 22 * (i + 1) + 14 < NSMaxY(br); i++) {
                        NSRect bx = NSMakeRect(br.origin.x + 16, oy + 22 + 22 * i, 9, 9); NSBezierPath *bp = [NSBezierPath bezierPathWithRect:NSInsetRect(bx, 0.5, 0.5)]; [TEAL setStroke]; [bp stroke];
                        ktext(obj[i], F_DISP(16), TEXT, 1.2, NSMakeRect(br.origin.x + 32, oy + 18 + 22 * i, br.size.width - 46, 18), NSTextAlignmentLeft);
                    }
                }
            }
        } else { ktext(title, F_DISP(20), NSColor.whiteColor, 2.6, NSMakeRect(212, 80, 400, 24), NSTextAlignmentLeft); [self drawGuide]; }
    }
    {   /* the bottom bar */
        fill(NSMakeRect(0, H - 70, W, 1), [TEAL_DIM colorWithAlphaComponent:0.6]);
        NSString *hint = self.guide ? @"Click an action, then press its key  ·  Esc goes back" : self.open >= 0 ? @"↑↓ choose  ·  ←→ change  ·  Esc: the mission page" : @"↑↓ choose  ·  ←→ change  ·  Return opens a tab  ·  Esc quits";
        ktext(hint, F_BODY(12), [NOTE colorWithAlphaComponent:0.85], 0.3, NSMakeRect(NSMaxX(self.quitRect) + 16, H - 43, NSMinX(self.playRect) - NSMaxX(self.quitRect) - 32, 16), NSTextAlignmentCenter);
    }
    [self drawButton:self.quitRect title:self.open >= 0 ? @"BACK" : @"QUIT" primary:NO hot:self.hover == HIT_QUIT];
    [self drawButton:self.playRect title:@"EXECUTE" primary:YES hot:self.hover == HIT_PLAY];
}
// ---- text rows: a field over the value, committed when it loses focus or on Return; Esc keeps the old text
- (void)editBegin:(NSInteger)k
{
    NSString *rid = self.items[k]; NSRect r = [self rowRect:k];
    NSTextField *e = [NSTextField textFieldWithString:values[rid] ?: @""];
    e.frame = NSMakeRect(r.origin.x + 12, r.origin.y + 21, r.size.width - 70, 24);
    e.bordered = NO; e.bezeled = NO; e.focusRingType = NSFocusRingTypeNone; e.drawsBackground = YES;
    e.backgroundColor = rgb(12, 34, 38); e.textColor = TEXT; e.font = F_DISP(18); e.delegate = self;
    [self addSubview:e]; self.edit = e; self.editRid = rid;
    [self.window makeFirstResponder:e]; [e selectText:nil];
    self.needsDisplay = YES;
}
- (void)editEnd:(BOOL)keep
{
    NSTextField *e = self.edit; NSString *rid = self.editRid; if (!e) return;
    self.edit = nil; self.editRid = nil;
    if (keep) { NSInteger max = [rows[rid][@"textmax"] integerValue] ?: 31; NSString *t = e.stringValue; values[rid] = t.length > (NSUInteger)max ? [t substringToIndex:max] : t; refresh_top(); }
    [e removeFromSuperview]; [self.window makeFirstResponder:self]; self.needsDisplay = YES;
}
- (void)controlTextDidChange:(NSNotification *)n
{
    NSInteger max = [rows[self.editRid][@"textmax"] integerValue] ?: 31; NSTextField *e = self.edit;
    if (e && e.stringValue.length > (NSUInteger)max) e.stringValue = [e.stringValue substringToIndex:max];
}
- (BOOL)control:(NSControl *)c textView:(NSTextView *)tv doCommandBySelector:(SEL)sel
{
    if (sel == @selector(cancelOperation:)) { [self editEnd:NO]; return YES; }
    if (sel == @selector(insertNewline:) || sel == @selector(insertTab:)) { [self editEnd:YES]; return YES; }
    return NO;
}
- (void)controlTextDidEndEditing:(NSNotification *)n { if (self.edit) [self editEnd:YES]; }
// ---- input
- (void)updateTrackingAreas
{
    for (NSTrackingArea *t in self.trackingAreas) [self removeTrackingArea:t];
    [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:self.bounds options:NSTrackingMouseMoved | NSTrackingActiveAlways | NSTrackingInVisibleRect owner:self userInfo:nil]];
}
- (void)mouseMoved:(NSEvent *)e { NSInteger h = [self hit:[self convertPoint:e.locationInWindow fromView:nil]]; if (h != self.hover) { self.hover = h; self.needsDisplay = YES; } }
- (void)bindMouse:(NSString *)button { NSNumber *c = mouse[button]; if ([c isKindOfClass:NSNumber.class] && c.intValue > 0 && c.intValue < 256) [self doBind:c.intValue]; }
- (void)rightMouseDown:(NSEvent *)e { if (self.bind >= 0) [self bindMouse:@"right"]; else [super rightMouseDown:e]; }
- (void)otherMouseDown:(NSEvent *)e { if (self.bind >= 0 && e.buttonNumber == 2) [self bindMouse:@"middle"]; else if (self.bind < 0) [super otherMouseDown:e]; }
- (void)mouseDown:(NSEvent *)e
{
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    if (self.bind >= 0) { [self bindMouse:@"left"]; return; }    /* while waiting for a key, a click is the left button */
    [self editEnd:YES];
    if (NSPointInRect(p, self.quitRect)) { [self back]; return; }
    if (NSPointInRect(p, self.playRect)) { [self play]; return; }
    if (NSPointInRect(p, self.homeRect)) { if (self.open >= 0) [self back]; return; }
    for (NSInteger g = 0; g < (NSInteger)groups.count; g++) if (NSPointInRect(p, [self groupRect:g])) {
        if (self.open >= 0) { self.open = g; self.focus = 0; self.bind = -1; self.scroll = 0; self.hover = HIT_NONE; self.needsDisplay = YES; }
        else [self activate:(NSInteger)top_ids.count + g];
        return;
    }
    if (self.guide) { NSInteger a = [self guideHit:p]; if (a >= 0) { self.bind = a; self.needsDisplay = YES; } return; }
    for (NSInteger k = 0; k < (NSInteger)self.items.count; k++) {
        NSRect r = [self rowRect:k]; if (!NSPointInRect(p, r)) continue;
        NSString *rid = self.items[k]; self.focus = k;
        if (![str(rows[rid][@"type"]) isEqualToString:@"text"] && p.x >= NSMaxX(r) - 56 && p.x < NSMaxX(r) - 30 && p.y < r.origin.y + 28) cycle(rid, -1);   /* the "<" */
        else [self activate:k];
        self.needsDisplay = YES; return;
    }
}
- (void)scrollPage:(NSInteger)d { NSInteger s = MAX(0, MIN(self.pages - 1, self.scroll + d)); if (s != self.scroll) { self.scroll = s; self.needsDisplay = YES; } }
- (void)scrollWheel:(NSEvent *)e
{
    if (!self.guide) return;
    if (e.phase == NSEventPhaseBegan) self.wheel = 0;
    self.wheel += e.hasPreciseScrollingDeltas ? e.scrollingDeltaY : e.scrollingDeltaY * 40;   /* a page per notch, or per 40 points of a swipe */
    if (self.wheel <= -40) { self.wheel = 0; [self scrollPage:1]; }
    else if (self.wheel >= 40) { self.wheel = 0; [self scrollPage:-1]; }
}
- (void)flagsChanged:(NSEvent *)e   /* Shift, Control, Option and Caps Lock come only as this: bound when pressed */
{
    if (self.bind < 0) return;
    NSEventModifierFlags f = e.modifierFlags; uint16_t c = e.keyCode;
    BOOL down = c == 0x39 || ((c == 0x38 || c == 0x3c) && (f & NSEventModifierFlagShift)) || ((c == 0x3b || c == 0x3e) && (f & NSEventModifierFlagControl))
             || ((c == 0x3a || c == 0x3d) && (f & NSEventModifierFlagOption));
    NSNumber *code = down ? vk_to_code[@(ds_mac_vk(c))] : nil;
    if (code) [self doBind:code.intValue];
}
- (void)keyDown:(NSEvent *)e
{
    uint16_t c = e.keyCode;
    if (self.bind >= 0) {
        if (c == 0x35) { self.bind = -1; self.needsDisplay = YES; }        /* Esc: cancel */
        else if (c == 0x75) [self doBind:0];                                 /* forward delete (fn-Delete): clear; Delete itself is Backspace */
        else { NSNumber *code = vk_to_code[@(ds_mac_vk(c))]; if (code) [self doBind:code.intValue]; }
        return;
    }
    NSInteger n = self.open < 0 ? (NSInteger)(top_ids.count + groups.count) : (NSInteger)self.items.count; if (n < 1) n = 1;
    switch (c) {
    case 0x35: [self back]; return;
    case 0x7e: if (!self.guide) { self.focus = (self.focus + n - 1) % n; self.needsDisplay = YES; } return;
    case 0x7d: if (!self.guide) { self.focus = (self.focus + 1) % n; self.needsDisplay = YES; } return;
    case 0x7b: case 0x7c: if (!self.guide && self.focus < (NSInteger)self.items.count) { cycle(self.items[self.focus], c == 0x7c ? 1 : -1); self.needsDisplay = YES; } return;
    case 0x24: case 0x4c: if (!self.guide) [self activate:self.focus]; return;
    case 0x74: case 0x79: if (self.guide) [self scrollPage:c == 0x79 ? 1 : -1]; return;
    }
    [super keyDown:e];
}
- (void)doBind:(int)code   /* the key now does this action, whatever it did before; 0 clears the action's keys */
{
    if (self.bind < 0 || self.bind >= (NSInteger)actions.count) return;
    uint8_t aid = (uint8_t)[actions[self.bind][@"id"] intValue], *km = keys.mutableBytes;
    if (code) km[1 + code] = aid;
    else for (int k = 0; k < 256; k++) if (km[1 + k] == aid) km[1 + k] = 0;
    self.bind = -1; self.needsDisplay = YES;
}
- (void)activate:(NSInteger)k
{
    NSArray *it = self.items;
    if (k < (NSInteger)it.count && !self.guide) {
        if ([str(rows[it[k]][@"type"]) isEqualToString:@"text"]) [self editBegin:k]; else cycle(it[k], 1);
    } else if (self.open < 0 && k >= (NSInteger)top_ids.count && k < (NSInteger)(top_ids.count + groups.count)) {
        self.open = k - (NSInteger)top_ids.count; self.focus = 0; self.bind = -1; self.scroll = 0; self.hover = HIT_NONE;
    }
    self.needsDisplay = YES;
}
- (void)back
{
    [self editEnd:YES];
    if (self.open >= 0) { self.focus = (NSInteger)top_ids.count + self.open; self.open = -1; self.bind = -1; self.hover = HIT_NONE; self.needsDisplay = YES; }
    else if (self.onQuit) self.onQuit();
}
- (void)play { [self editEnd:YES]; if (self.onPlay) self.onPlay(); }
@end

// ---------------------------------------------------------------- the window
/* Play: every option into registry.txt (only the game's key's values the spec names are replaced or added), the whole
 * key map into sherman.kmp. NO (and why) if a file couldn't be written. */
/* the title out of GOG's key art (store-key.png, 1600 wide): the crosshair's red and the letters' white kept, the dark
 * picture behind them gone (alpha from how red or how white a pixel is), the operative's pistol at the top right cut */
static NSImage *key_title(NSImage *src)
{
    CGImageRef cg = [src CGImageForProposedRect:NULL context:nil hints:nil]; if (!cg || CGImageGetWidth(cg) < 800) return nil;
    CGFloat k = CGImageGetWidth(cg) / 1600.0;
    CGImageRef part = CGImageCreateWithImageInRect(cg, CGRectMake(round(320 * k), round(100 * k), round(865 * k), round(460 * k))); if (!part) return nil;
    size_t w = CGImageGetWidth(part), h = CGImageGetHeight(part);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(NULL, w, h, 8, w * 4, cs, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), part); CGImageRelease(part);
    uint8_t *px = CGBitmapContextGetData(ctx);
    for (size_t y = 0; y < h; y++) for (size_t x = 0; x < w; x++) {
        uint8_t *q = px + 4 * (y * w + x); int r = q[0], g = q[1], b = q[2];
        int mx = MAX(r, MAX(g, b)), mn = MIN(r, MIN(g, b)), white = mx - mn < 45 && g <= r + 8 ? mn : 0;   /* (white: neutral, not the glove's green) */
        int lg = MAX(r - MAX(g, b), white), a = MIN(255, MAX(0, (lg - 80) * 255 / 60));
        if (x > 650 * k && y < 140 * k) a = 0;                                      /* (the pistol, above the letters) */
        q[0] = (uint8_t)(r * a / 255); q[1] = (uint8_t)(g * a / 255); q[2] = (uint8_t)(b * a / 255); q[3] = (uint8_t)a;
    }
    CGImageRef out = CGBitmapContextCreateImage(ctx); CGContextRelease(ctx); CGColorSpaceRelease(cs);
    NSImage *img = [[NSImage alloc] initWithCGImage:out size:NSMakeSize(w, h)]; CGImageRelease(out); return img;
}
static NSString *const KEY_ART_URL = @"https://images.gog-statics.com/eeef55fbde07676b7c9570b0fad4ba38a9a41c17b9575d5b24c69d503c87affa.png";   /* GOG's key art */
static BOOL save_all(NSString *data, NSString *kmpName, NSString **why)
{
    NSString *regPath = [data stringByAppendingPathComponent:@"registry.txt"]; NSError *err = nil;
    if (have_spec && row_order.count) {
        reg_load(regPath);                                   /* again: the file as it is now */
        for (NSString *rid in row_order) write_value(rows[rid], values[rid] ?: @"");
        apply_online_registry();                             /* Play Online: the game options relay play needs, or the player's back */
        if (!reg_save(regPath, &err)) { *why = [NSString stringWithFormat:@"%@: %@", regPath, err.localizedDescription]; return NO; }
        if (layouts.count) {
            NSString *dir = [data stringByAppendingPathComponent:@"game"], *path = find_ci(dir, kmpName) ?: [dir stringByAppendingPathComponent:kmpName];
            [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
            if (![[keys subdataWithRange:NSMakeRange(0, KMP_N)] writeToFile:path options:NSDataWritingAtomic error:&err]) {   /* always all 257 bytes */
                *why = [NSString stringWithFormat:@"%@: %@", path, err.localizedDescription]; return NO;
            }
        }
    }
    return YES;
}

/* Shows the launch window (modal) unless DS_NO_LAUNCHER is set; returns 0 to quit. DS_LAUNCHER_SHOT=<png> renders it
 * to an image instead (development): DS_LAUNCHER_OPEN=<group title> with that group's panel open, DS_LAUNCHER_PICK=<
 * layout> that layout chosen, DS_LAUNCHER_SET="id=value;..." those values, DS_LAUNCHER_BIND=<n> action n waiting for a
 * key (DS_LAUNCHER_KEY=<Mac key code> then presses one), DS_LAUNCHER_ROWS=1 prints every row's choices; DS_LAUNCHER_APPLY=1 then writes the files as Play would (without starting the game). */
int ds_launcher_run(const char *game_dir, const char *data_dir)
{
    NSString *game = @(game_dir), *data = getenv("DS_LAUNCHER_HOME") ? @(getenv("DS_LAUNCHER_HOME")) : @(data_dir);
    rows = [NSMutableDictionary dictionary]; values = [NSMutableDictionary dictionary]; layouts = [NSMutableDictionary dictionary];
    key_names = [NSMutableDictionary dictionary]; vk_to_code = [NSMutableDictionary dictionary];
    row_order = [NSMutableArray array]; top_ids = @[]; groups = @[]; actions = @[]; mouse = @{}; version = @""; kmp_file = @"sherman.kmp";
    @try { have_spec = load_spec(game); }
    @catch (NSException *x) { (void)x; have_spec = NO; [rows removeAllObjects]; [row_order removeAllObjects]; [layouts removeAllObjects]; top_ids = @[]; groups = @[]; actions = @[]; }   /* a malformed spec: as none */
    if (!have_spec) { [rows removeAllObjects]; [row_order removeAllObjects]; [layouts removeAllObjects]; [key_names removeAllObjects]; [vk_to_code removeAllObjects]; top_ids = @[]; groups = @[]; actions = @[]; }
    add_modes(); if (have_spec) { add_mac_rows(); add_online_rows(); }
    reg_load([data stringByAppendingPathComponent:@"registry.txt"]);
    for (NSString *rid in rows) values[rid] = read_value(rows[rid]);
    prepare_online();
    /* the key map now: the data folder's copy (the runtime reads it first), else the game folder's, else Classic */
    NSString *ovd = [data stringByAppendingPathComponent:@"game"];
    keys = read_kmp(find_ci(ovd, kmp_file)) ?: read_kmp(find_ci(game, kmp_file)) ?: [(layouts[@"classic"] ?: [NSMutableData dataWithLength:KMP_N]) mutableCopy];
    const char *shot = getenv("DS_LAUNCHER_SHOT");
    if (getenv("DS_NO_LAUNCHER") && !shot) { apply(); return 1; }   /* the stored settings, no window */
    if (getenv("DS_LAUNCHER_PLAY") && !shot) {             /* tests: Play pressed at once, through the real Play path */
        NSString *bad = online_check(), *why = nil;
        if (bad) { fprintf(stderr, "launcher: %s\n", bad.UTF8String); exit(1); }
        if (!save_all(data, kmp_file, &why)) { fprintf(stderr, "launcher: couldn't save the settings: %s\n", why.UTF8String); exit(1); }
        relay_env(data); apply(); return 1;
    }

    DSLaunchView *v = [[DSLaunchView alloc] initWithFrame:NSMakeRect(0, 0, W, H)];
    v.open = -1; v.bind = -1; v.hover = HIT_NONE;
    v.art = read_rsb([game stringByAppendingPathComponent:@"data/bitmap/mm_bg.RSB"], 16, 186);   /* the menu's art without its top and bottom */
    if (!v.art) { NSString *b = find_ci([game stringByAppendingPathComponent:@"data/bitmap"], @"mm_bg.RSB"); if (b) v.art = read_rsb(b, 16, 186); }
    {   /* the title keyed out of the menu's picture (its brightness the opacity: it sits on black there); the slides; the portrait */
        NSString *mm = [game stringByAppendingPathComponent:@"data/bitmap/mm_bg.RSB"];
        if (![NSFileManager.defaultManager fileExistsAtPath:mm]) mm = find_ci([game stringByAppendingPathComponent:@"data/bitmap"], @"mm_bg.RSB");
        NSImage *menu = mm ? read_rsb(mm, 0, 0) : nil;
        CGImageRef cg = [menu CGImageForProposedRect:NULL context:nil hints:nil];
        if (cg) {
            size_t w = CGImageGetWidth(cg), h = CGImageGetHeight(cg), lw = w * 52 / 100; NSMutableData *px = [NSMutableData dataWithLength:w * h * 4];
            CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB(); CGContextRef c = CGBitmapContextCreate(px.mutableBytes, w, h, 8, w * 4, cs, (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
            CGContextDrawImage(c, CGRectMake(0, 0, w, h), cg); CGContextRelease(c);
            uint8_t *d = px.mutableBytes; size_t y0 = h, y1 = 0;
            for (size_t y = 16; y < MIN(h, (size_t)186); y++) for (size_t x = 0; x < lw; x++) { uint8_t *q = d + (y * w + x) * 4; if (MAX(q[0], MAX(q[1], q[2])) > 90) { if (y < y0) y0 = y; if (y > y1) y1 = y; } }   /* (the title, at the menu's top left) */
            if (y1 > y0) {
                y0 = y0 > 4 ? y0 - 4 : 0; y1 = MIN(h - 1, y1 + 4); size_t oh = y1 - y0 + 1; NSMutableData *o = [NSMutableData dataWithLength:lw * oh * 4]; uint8_t *od = o.mutableBytes;
                for (size_t y = 0; y < oh; y++) for (size_t x = 0; x < lw; x++) {
                    const uint8_t *q = d + ((y0 + y) * w + x) * 4; double m = MAX(q[0], MAX(q[1], q[2])) / 255.0, a = m < 0.06 ? 0 : MIN(1.0, m * 1.5);
                    double edge = MIN((double)MIN(x, lw - 1 - x), 24.0) / 24.0; a *= edge;                  /* (feathered at the cut) */
                    uint8_t *r = od + (y * lw + x) * 4; r[0] = (uint8_t)MIN(q[0], a * 255); r[1] = (uint8_t)MIN(q[1], a * 255); r[2] = (uint8_t)MIN(q[2], a * 255); r[3] = (uint8_t)(a * 255);
                }
                CGDataProviderRef dp = CGDataProviderCreateWithCFData((__bridge CFDataRef)o);
                CGImageRef li = CGImageCreate(lw, oh, 8, 32, lw * 4, cs, (CGBitmapInfo)kCGImageAlphaPremultipliedLast, dp, NULL, false, kCGRenderingIntentDefault);
                if (li) { v.logo = [[NSImage alloc] initWithCGImage:li size:NSMakeSize(lw, oh)]; CGImageRelease(li); }
                CGDataProviderRelease(dp);
            }
            CGColorSpaceRelease(cs);
        }
        NSString *artHome = getenv("DS_LAUNCHER_ART") ? @(getenv("DS_LAUNCHER_ART")) : [data stringByAppendingPathComponent:@"art"];
        NSArray *dirs = getenv("DS_LAUNCHER_ART") ? @[artHome] : @[artHome, [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"art"]];
        NSImage *(^pic)(NSString *) = ^NSImage *(NSString *name) {
            for (NSString *dd in dirs) { NSImage *i = [[NSImage alloc] initWithContentsOfFile:[dd stringByAppendingPathComponent:name]]; if (i.size.width > 16) return i; }
            return nil;
        };
        NSMutableArray *own = [NSMutableArray array], *store = [NSMutableArray array], *sc = [NSMutableArray array];
        for (int k = 1; k <= 12; k++) { NSImage *i = pic([NSString stringWithFormat:@"intro-%d.jpg", k]); if (i) [own addObject:i]; }
        for (int k = 1; k <= 6; k++) { NSImage *i = pic([NSString stringWithFormat:@"store-shot-%d.jpg", k]); if (i) [store addObject:i]; }
        if (menu) [own insertObject:menu atIndex:MIN(own.count, (NSUInteger)2)];
        for (NSUInteger k = 0; k < MAX(own.count, store.count); k++) { if (k < own.count) [sc addObject:own[k]]; if (k < store.count) [sc addObject:store[k]]; }
        v.scenes = sc;
        NSImage *box = pic(@"store-portrait.jpg");
        if (box) { v.portrait = box; v.portraitFx = 0.55; v.portraitFy = 1.0; v.portraitZ = 1.48; }   /* the box's operative (its title cut off above) */
        else { v.portrait = menu ?: v.art; v.portraitFx = 0.92; v.portraitFy = 0.4; v.portraitZ = 1.0; }   /* the menu's operative in his balaclava */
        __weak DSLaunchView *wv = v;
        void (^useKey)(NSImage *) = ^(NSImage *key) {   /* GOG's key art: the operative in his goggles (sharp at the portrait's size) and the title */
            DSLaunchView *sv = wv; if (!sv || key.size.width < 800) return;
            sv.portrait = key; sv.portraitFx = 1.0; sv.portraitFy = 0.12; sv.portraitZ = 1.75;   /* (the dossier's tall photo: his goggles and face, past the title's letters) */
            NSImage *t = key_title(key); if (t) { sv.logo = t; sv.logoHD = YES; }
            [sv.cache removeObjectForKey:@"uiSig"]; sv.needsDisplay = YES;
        };
        NSImage *key = pic(@"store-key.png");
        if (key) useKey(key);
        else if (!getenv("DS_LAUNCHER_OFFLINE") && !getenv("DS_NO_DOWNLOADS")) {   /* not there yet (an older install): fetched once into the art folder */
            dispatch_semaphore_t done = shot ? dispatch_semaphore_create(0) : nil;
            [[NSURLSession.sharedSession dataTaskWithURL:[NSURL URLWithString:KEY_ART_URL] completionHandler:^(NSData *d, NSURLResponse *r, NSError *err) {
                NSImage *got = !err && [(NSHTTPURLResponse *)r statusCode] == 200 && d.length > 100000 ? [[NSImage alloc] initWithData:d] : nil;
                if (got) {
                    [NSFileManager.defaultManager createDirectoryAtPath:artHome withIntermediateDirectories:YES attributes:nil error:nil];
                    [d writeToFile:[artHome stringByAppendingPathComponent:@"store-key.png"] atomically:YES];
                    if (!done) dispatch_async(dispatch_get_main_queue(), ^{ useKey(got); }); else useKey(got);
                }
                if (done) dispatch_semaphore_signal(done);
            }] resume];
            if (done) dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 25 * NSEC_PER_SEC));
        }
    }
    if (shot) {   /* development: the window as an image */
        const char *e;
        if ((e = getenv("DS_LAUNCHER_PICK")) && layouts[@(e)]) [keys setData:layouts[@(e)]];
        if ((e = getenv("DS_LAUNCHER_SET")))
            for (NSString *kv in [@(e) componentsSeparatedByString:@";"]) { NSRange eq = [kv rangeOfString:@"="]; NSString *id_ = eq.location != NSNotFound ? [kv substringToIndex:eq.location] : nil;
                if (id_ && rows[id_]) values[id_] = [kv substringFromIndex:eq.location + 1]; }
        refresh_top();
        if ((e = getenv("DS_LAUNCHER_OPEN"))) for (NSInteger i = 0; i < (NSInteger)groups.count; i++) if ([groups[i][@"title"] caseInsensitiveCompare:@(e)] == NSOrderedSame) v.open = i;
        if ((e = getenv("DS_LAUNCHER_BIND")) && v.guide && atoi(e) < (NSInteger)actions.count) v.bind = atoi(e);
        if ((e = getenv("DS_LAUNCHER_SCROLL"))) [v scrollPage:atoi(e)];
        if (getenv("DS_LAUNCHER_ROWS")) for (NSString *rid in row_order) {   /* each row's choices, as the window has them */
            NSMutableArray *l = [NSMutableArray array]; for (NSDictionary *c in choices(rid)) [l addObject:str(dict(c)[@"value"]) ?: @"?"];
            printf("row %s = %s: %s\n", rid.UTF8String, values[rid].UTF8String, [l componentsJoinedByString:@" "].UTF8String);
        }
        if ((e = getenv("DS_LAUNCHER_KEY")) && v.bind >= 0)        /* that Mac key code pressed, as the window would get it */
            [v keyDown:[NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0 context:nil
                                      characters:@"" charactersIgnoringModifiers:@"" isARepeat:NO keyCode:(unsigned short)strtol(e, NULL, 0)]];
        v.t = getenv("DS_LAUNCHER_TIME") ? atof(getenv("DS_LAUNCHER_TIME")) : 4.0;
        NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:(NSInteger)v.bounds.size.width * 2 pixelsHigh:(NSInteger)v.bounds.size.height * 2
                                                                    bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSCalibratedRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
        rep.size = v.bounds.size;
        [v displayRectIgnoringOpacity:v.bounds inContext:[NSGraphicsContext graphicsContextWithBitmapImageRep:rep]];
        if (getenv("DS_LAUNCHER_TIMING")) {
            double t0 = CACurrentMediaTime();
            for (int k = 0; k < 60; k++) { v.t += 1.0 / 60; [v displayRectIgnoringOpacity:v.bounds inContext:[NSGraphicsContext graphicsContextWithBitmapImageRep:rep]]; }
            fprintf(stderr, "frame: %.1f ms\n", (CACurrentMediaTime() - t0) * 1000 / 60);
        }
        [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(shot) atomically:YES];
        if (getenv("DS_LAUNCHER_APPLY")) {          /* development: the files written as Play would */
            NSString *bad = online_check(); if (bad) printf("launcher: %s\n", bad.UTF8String);
            NSString *why = nil; BOOL ok = !bad && save_all(data, kmp_file, &why);
            if (ok) relay_env(data);
            for (NSString *n in @[@"R6_RELAY", @"R6_ROOM", @"R6_RELAY_CODE", @"R6_RELAY_NAME", @"R6_RELAY_BUILD", @"R6_RELAY_STATUS", @"R6_RELAY_LOCAL_PORT"])
                printf("launcher: %s=%s\n", n.UTF8String, getenv(n.UTF8String) ?: "(unset)");
            apply(); const char *f = getenv("DSR_FPSCAP"), *vs = getenv("DSR_VSYNC"), *wm = getenv("DS_WINDOW_MODE");
            printf("launcher: %s (controls %s; DSR_FPSCAP=%s DSR_VSYNC=%s DS_WINDOW_MODE=%s)\n", ok ? "saved" : why.UTF8String, layout_name().UTF8String,
                   f ? f : "(unset)", vs ? vs : "(unset)", wm ? wm : "(unset)");
        }
        return 0;
    }

    NSWindow *w = [[NSWindow alloc] initWithContentRect:v.frame styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskFullSizeContentView
                                                backing:NSBackingStoreBuffered defer:NO];
    w.releasedWhenClosed = NO;   /* ARC owns it: AppKit's default would release it again when the close button closes it (a crash on exit) */
    w.titlebarAppearsTransparent = YES; w.titleVisibility = NSWindowTitleHidden; w.movableByWindowBackground = NO; w.title = @"Rainbow Six co-op";
    w.backgroundColor = BG; w.contentView = v; [w center]; [w makeFirstResponder:v];
    __block int result = 0;
    v.onPlay = ^{
        NSString *why = nil, *bad = online_check();
        if (bad) {                                    /* Play Online's settings first: nothing is saved or started */
            NSAlert *a = [NSAlert new]; a.messageText = @"Play Online"; a.informativeText = bad; [a runModal];
            return;
        }
        if (!save_all(data, kmp_file, &why)) {        /* the window stays: nothing is started with half the settings */
            NSAlert *a = [NSAlert new]; a.messageText = @"Couldn't save the settings"; a.informativeText = why ?: @""; [a runModal];
            return;
        }
        result = 1; [NSApp stopModal];
    };
    v.onQuit = ^{ result = 0; [NSApp stopModal]; };
    [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowWillCloseNotification object:w queue:nil usingBlock:^(NSNotification *n) { (void)n; [NSApp stopModal]; }];
    [NSApp activateIgnoringOtherApps:YES];
    [w makeKeyAndOrderFront:nil];
    /* the animation, 60 times a second, in the modal loop's mode too */
    double t0 = CACurrentMediaTime(); __weak DSLaunchView *wv = v;
    NSTimer *anim = [NSTimer timerWithTimeInterval:1.0 / 60 repeats:YES block:^(NSTimer *tm) {
        DSLaunchView *sv = wv; if (!sv) return;
        double t = sv.t = CACurrentMediaTime() - t0; sv.needsDisplay = YES;
        static double lastT; if (getenv("DS_LAUNCHER_TIMING") && t - lastT >= 1) { fprintf(stderr, "fps: %.0f\n", frames_drawn / (t - lastT)); frames_drawn = 0; lastT = t; if (t > 6) [NSApp stopModal]; }
    }];
    [[NSRunLoop currentRunLoop] addTimer:anim forMode:NSRunLoopCommonModes];
    [NSApp runModalForWindow:w];
    [anim invalidate];
    [w orderOut:nil];
    if (!result) return 0;
    relay_env(data); apply();
    return 1;
}
