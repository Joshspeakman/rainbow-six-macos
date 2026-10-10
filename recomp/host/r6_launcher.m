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
#import <AppKit/AppKit.h>
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
enum { W = 760, H = 700, BANNER = 200, ROW_X = 70, ROW_W = W - 140, ROW_H = 46, LINE = 19 };
enum { HIT_NONE = -1, HIT_QUIT = 1000, HIT_PLAY = 1001, HIT_ROW = 2000, HIT_GROUP = 3000, HIT_ACTION = 4000 };
static NSColor *rgb(int r, int g, int b) { return [NSColor colorWithSRGBRed:r / 255.0 green:g / 255.0 blue:b / 255.0 alpha:1]; }
#define TEAL rgb(46, 196, 196)
#define TEAL_DIM rgb(24, 110, 112)
#define TEXT rgb(214, 236, 236)
#define NOTE rgb(120, 160, 160)
#define ACCENT rgb(255, 214, 58)
#define BG rgb(6, 12, 14)
#define PANEL rgb(10, 26, 30)
#define PANEL_HOT rgb(16, 42, 46)
/* the Linux window's Arial at 96 dpi: Qt's points are a third larger than AppKit's */
static NSFont *arial(CGFloat px, NSString *face) { return [NSFont fontWithName:face size:px] ?: [NSFont systemFontOfSize:px weight:NSFontWeightSemibold]; }
#define F_TITLE arial(40, @"Arial-Black")
#define F_BIG arial(17, @"Arial-BoldMT")
#define F_ROW arial(14.5, @"Arial-BoldMT")
#define F_SMALL arial(12, @"ArialMT")

static void draw_text(NSString *s, NSFont *f, NSColor *c, NSRect r, NSTextAlignment al)   /* one line, centred on the rect's height, elided */
{
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new]; ps.alignment = al; ps.lineBreakMode = NSLineBreakByTruncatingTail;
    CGFloat h = ceil(f.ascender - f.descender);
    [s ?: @"" drawInRect:NSMakeRect(r.origin.x, r.origin.y + floor((r.size.height - h) / 2), r.size.width, h)
          withAttributes:@{NSFontAttributeName: f, NSForegroundColorAttributeName: c, NSParagraphStyleAttributeName: ps}];
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
@property NSImage *art;
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
- (NSInteger)rowH   /* a panel's rows share the room between its title and the buttons: up to ROW_H tall, 4 apart */
{
    NSInteger n = (NSInteger)self.items.count;
    return self.open < 0 || !n ? ROW_H : MIN(ROW_H, ((H - 84) - (BANNER + 54)) / n - 4);
}
- (NSRect)rowRect:(NSInteger)k
{
    NSInteger h = self.rowH;
    return NSMakeRect(ROW_X, (self.open < 0 ? BANNER + 18 : BANNER + 54) + k * (h + (self.open < 0 ? 6 : 4)), ROW_W, h);
}
- (NSRect)groupRect:(NSInteger)g
{
    NSInteger y = BANNER + 18 + (NSInteger)top_ids.count * (ROW_H + 6) + 14;
    return NSMakeRect(ROW_X + (g % 2) * (ROW_W / 2 + 6), y + (g / 2) * 62, ROW_W / 2 - 6, 54);
}
- (NSRect)quitRect { return NSMakeRect(ROW_X, H - 66, 150, 44); }
- (NSRect)playRect { return NSMakeRect(W - ROW_X - 200, H - 66, 200, 44); }
- (NSRect)guideBox { return NSMakeRect(ROW_X, BANNER + 50, ROW_W, H - BANNER - 140); }
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
    if (self.guide) { NSInteger a = [self guideHit:p]; return a >= 0 ? HIT_ACTION + a : HIT_NONE; }
    NSInteger h = HIT_NONE;
    for (NSInteger k = 0; k < (NSInteger)self.items.count; k++) if (NSPointInRect(p, [self rowRect:k])) h = HIT_ROW + k;
    if (self.open < 0) for (NSInteger g = 0; g < (NSInteger)groups.count; g++) if (NSPointInRect(p, [self groupRect:g])) h = HIT_GROUP + g;
    return h;
}
// ---- drawing
- (void)drawButton:(NSRect)r title:(NSString *)title primary:(BOOL)primary hot:(BOOL)hot
{
    fill(r, primary && hot ? rgb(40, 150, 150) : primary ? rgb(24, 110, 112) : hot ? PANEL_HOT : PANEL);
    [(primary ? ACCENT : TEAL) setStroke]; NSBezierPath *b = [NSBezierPath bezierPathWithRect:NSInsetRect(r, 0.5, 0.5)]; b.lineWidth = 1; [b stroke];
    draw_text(title, F_BIG, primary ? NSColor.whiteColor : TEAL, r, NSTextAlignmentCenter);
}
- (void)drawRow:(NSInteger)k
{
    NSString *rid = self.items[k]; NSRect r = [self rowRect:k]; BOOL hot = self.focus == k || self.hover == HIT_ROW + k;
    fill(r, hot ? PANEL_HOT : PANEL); brackets(r, hot ? TEAL : TEAL_DIM);
    CGFloat h = r.size.height, x = r.origin.x, top = r.origin.y + floor((h - 38) / 2);
    draw_text(str(rows[rid][@"label"]), F_ROW, NOTE, NSMakeRect(x + 16, r.origin.y, 200, h), NSTextAlignmentLeft);
    NSString *val, *note; row_label(rid, &val, &note);
    NSRect vr = note.length ? NSMakeRect(x + 220, top + 2, r.size.width - 270, 20) : NSMakeRect(x + 220, r.origin.y, r.size.width - 270, h);   /* no note: the value centred */
    if (![self.editRid isEqualToString:rid]) draw_text(val, F_ROW, TEXT, vr, NSTextAlignmentLeft);
    else { [TEAL setStroke]; NSBezierPath *b = [NSBezierPath bezierPathWithRect:NSInsetRect(self.edit.frame, -1.5, -1.5)]; b.lineWidth = 1; [b stroke]; }
    draw_text(note, F_SMALL, NOTE, NSMakeRect(x + 220, top + 21, r.size.width - 270, 16), NSTextAlignmentLeft);
    if (![str(rows[rid][@"type"]) isEqualToString:@"text"]) draw_text(@"<  >", F_ROW, TEAL, NSMakeRect(NSMaxX(r) - 47, r.origin.y, 40, h), NSTextAlignmentCenter);
}
- (void)drawGuide
{
    NSRect b = self.guideBox; fill(b, PANEL); brackets(b, TEAL_DIM);
    NSString *lay = @{@"classic": @"Classic", @"modern": @"Modern", @"keyboard": @"Keyboard Only"}[layout_name()] ?: @"Custom";
    draw_text([NSString stringWithFormat:@"Layout: %@.  Click an action, then press a key or mouse button (Esc cancels, fn-Delete clears).", lay],
              F_SMALL, NOTE, NSMakeRect(b.origin.x + 12, b.origin.y + 6, b.size.width - 24 - (self.pages > 1 ? 80 : 0), 18), NSTextAlignmentLeft);
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
        draw_text(str(actions[i][@"name"]), F_SMALL, binding ? ACCENT : self.hover == HIT_ACTION + i ? TEXT : NOTE, NSMakeRect(x, y, 182, LINE), NSTextAlignmentLeft);
        draw_text(s, F_SMALL, binding ? ACCENT : TEXT, NSMakeRect(x + 186, y, cw - 24 - 186, LINE), NSTextAlignmentLeft);   /* Linux: 210 / 214; room for "RIGHT MOUSE" */
    }
}
- (void)drawRect:(NSRect)dirty
{
    fill(self.bounds, BG);
    if (self.art) [self.art drawInRect:NSMakeRect(0, 0, W, BANNER) fromRect:NSZeroRect operation:NSCompositingOperationCopy fraction:1 respectFlipped:YES
                                  hints:@{NSImageHintInterpolation: @(NSImageInterpolationHigh)}];
    else draw_text(@"RAINBOW SIX", F_TITLE, NSColor.whiteColor, NSMakeRect(0, 60, W, 50), NSTextAlignmentCenter);
    fill(NSMakeRect(0, BANNER - 24, W, 24), rgb(4, 8, 10));
    draw_text(have_spec ? [@"CO-OP EDITION  ·  " stringByAppendingString:version] : @"NATIVE EDITION FOR macOS", F_SMALL, ACCENT, NSMakeRect(0, BANNER - 22, W - 16, 20), NSTextAlignmentRight);
    fill(NSMakeRect(0, BANNER, W, 1), TEAL_DIM);
    if (self.open >= 0) {
        draw_text(groups[self.open][@"title"], F_BIG, TEAL, NSMakeRect(ROW_X, BANNER + 14, ROW_W, 30), NSTextAlignmentLeft);
        if (self.guide) [self drawGuide];
        else for (NSInteger k = 0; k < (NSInteger)self.items.count; k++) [self drawRow:k];
    } else {
        for (NSInteger k = 0; k < (NSInteger)top_ids.count; k++) [self drawRow:k];
        for (NSInteger g = 0; g < (NSInteger)groups.count; g++) {
            NSRect r = [self groupRect:g]; BOOL hot = self.focus == (NSInteger)top_ids.count + g || self.hover == HIT_GROUP + g;
            fill(r, hot ? PANEL_HOT : PANEL); brackets(r, hot ? TEAL : TEAL_DIM);
            draw_text(groups[g][@"title"], F_BIG, TEAL, NSMakeRect(r.origin.x + 14, r.origin.y + 4, r.size.width - 28, 26), NSTextAlignmentLeft);
            NSMutableArray *sum = [NSMutableArray array];
            for (NSString *rid in groups[g][@"rows"]) { NSString *v, *n; row_label(rid, &v, &n); [sum addObject:v]; }
            draw_text([groups[g][@"guide"] boolValue] ? @"Every action's keys; rebind any of them" : [sum componentsJoinedByString:@" · "],
                      F_SMALL, NOTE, NSMakeRect(r.origin.x + 14, r.origin.y + 30, r.size.width - 28, 18), NSTextAlignmentLeft);
        }
        if (!have_spec) draw_text(@"No r6coop-launcher.json next to RainbowSix.exe: the game's own Options screen has its settings.", F_SMALL, NOTE,
                                  NSMakeRect(ROW_X, groups.count ? NSMaxY([self groupRect:(NSInteger)groups.count - 1]) + 14 : BANNER + 30, ROW_W, 18), NSTextAlignmentLeft);
    }
    [self drawButton:self.quitRect title:self.open >= 0 ? @"BACK" : @"QUIT" primary:NO hot:self.hover == HIT_QUIT];
    [self drawButton:self.playRect title:@"PLAY" primary:YES hot:self.hover == HIT_PLAY];
}
// ---- text rows: a field over the value, committed when it loses focus or on Return; Esc keeps the old text
- (void)editBegin:(NSInteger)k
{
    NSString *rid = self.items[k]; NSRect r = [self rowRect:k];
    NSTextField *e = [NSTextField textFieldWithString:values[rid] ?: @""];
    e.frame = NSMakeRect(r.origin.x + 218, r.origin.y + floor((r.size.height - 24) / 2), r.size.width - 280, 24);
    e.bordered = NO; e.bezeled = NO; e.focusRingType = NSFocusRingTypeNone; e.drawsBackground = YES;
    e.backgroundColor = rgb(12, 34, 38); e.textColor = TEXT; e.font = arial(14.5, @"ArialMT"); e.delegate = self;
    [self addSubview:e]; self.edit = e; self.editRid = rid;
    [self.window makeFirstResponder:e]; [e selectText:nil];
    self.needsDisplay = YES;
}
- (void)editEnd:(BOOL)keep
{
    NSTextField *e = self.edit; NSString *rid = self.editRid; if (!e) return;
    self.edit = nil; self.editRid = nil;
    if (keep) { NSInteger max = [rows[rid][@"textmax"] integerValue] ?: 31; NSString *t = e.stringValue; values[rid] = t.length > (NSUInteger)max ? [t substringToIndex:max] : t; }
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
    if (self.guide) { NSInteger a = [self guideHit:p]; if (a >= 0) { self.bind = a; self.needsDisplay = YES; } return; }
    for (NSInteger k = 0; k < (NSInteger)self.items.count; k++) {
        NSRect r = [self rowRect:k]; if (!NSPointInRect(p, r)) continue;
        NSString *rid = self.items[k]; self.focus = k;
        if (![str(rows[rid][@"type"]) isEqualToString:@"text"] && p.x >= NSMaxX(r) - 47 && p.x < NSMaxX(r) - 27) cycle(rid, -1);   /* the "<" */
        else [self activate:k];
        self.needsDisplay = YES; return;
    }
    if (self.open < 0) for (NSInteger g = 0; g < (NSInteger)groups.count; g++) if (NSPointInRect(p, [self groupRect:g])) { [self activate:(NSInteger)top_ids.count + g]; return; }
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
static BOOL save_all(NSString *data, NSString *kmpName, NSString **why)
{
    NSString *regPath = [data stringByAppendingPathComponent:@"registry.txt"]; NSError *err = nil;
    if (have_spec && row_order.count) {
        reg_load(regPath);                                   /* again: the file as it is now */
        for (NSString *rid in row_order) write_value(rows[rid], values[rid] ?: @"");
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
    NSString *game = @(game_dir), *data = @(data_dir);
    rows = [NSMutableDictionary dictionary]; values = [NSMutableDictionary dictionary]; layouts = [NSMutableDictionary dictionary];
    key_names = [NSMutableDictionary dictionary]; vk_to_code = [NSMutableDictionary dictionary];
    row_order = [NSMutableArray array]; top_ids = @[]; groups = @[]; actions = @[]; mouse = @{}; version = @""; kmp_file = @"sherman.kmp";
    @try { have_spec = load_spec(game); }
    @catch (NSException *x) { (void)x; have_spec = NO; [rows removeAllObjects]; [row_order removeAllObjects]; [layouts removeAllObjects]; top_ids = @[]; groups = @[]; actions = @[]; }   /* a malformed spec: as none */
    if (!have_spec) { [rows removeAllObjects]; [row_order removeAllObjects]; [layouts removeAllObjects]; [key_names removeAllObjects]; [vk_to_code removeAllObjects]; top_ids = @[]; groups = @[]; actions = @[]; }
    add_modes(); if (have_spec) add_mac_rows();
    reg_load([data stringByAppendingPathComponent:@"registry.txt"]);
    for (NSString *rid in rows) values[rid] = read_value(rows[rid]);
    /* the key map now: the data folder's copy (the runtime reads it first), else the game folder's, else Classic */
    NSString *ovd = [data stringByAppendingPathComponent:@"game"];
    keys = read_kmp(find_ci(ovd, kmp_file)) ?: read_kmp(find_ci(game, kmp_file)) ?: [(layouts[@"classic"] ?: [NSMutableData dataWithLength:KMP_N]) mutableCopy];
    const char *shot = getenv("DS_LAUNCHER_SHOT");
    if (getenv("DS_NO_LAUNCHER") && !shot) { apply(); return 1; }   /* the stored settings, no window */

    DSLaunchView *v = [[DSLaunchView alloc] initWithFrame:NSMakeRect(0, 0, W, H)];
    v.open = -1; v.bind = -1; v.hover = HIT_NONE;
    v.art = read_rsb([game stringByAppendingPathComponent:@"data/bitmap/mm_bg.RSB"], 16, 186);   /* the menu's art without its top and bottom */
    if (!v.art) { NSString *b = find_ci([game stringByAppendingPathComponent:@"data/bitmap"], @"mm_bg.RSB"); if (b) v.art = read_rsb(b, 16, 186); }
    if (shot) {   /* development: the window as an image */
        const char *e;
        if ((e = getenv("DS_LAUNCHER_PICK")) && layouts[@(e)]) [keys setData:layouts[@(e)]];
        if ((e = getenv("DS_LAUNCHER_SET")))
            for (NSString *kv in [@(e) componentsSeparatedByString:@";"]) { NSRange eq = [kv rangeOfString:@"="]; NSString *id_ = eq.location != NSNotFound ? [kv substringToIndex:eq.location] : nil;
                if (id_ && rows[id_]) values[id_] = [kv substringFromIndex:eq.location + 1]; }
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
        NSBitmapImageRep *rep = [v bitmapImageRepForCachingDisplayInRect:v.bounds]; [v cacheDisplayInRect:v.bounds toBitmapImageRep:rep];
        [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(shot) atomically:YES];
        if (getenv("DS_LAUNCHER_APPLY")) {          /* development: the files written as Play would */
            NSString *why = nil; BOOL ok = save_all(data, kmp_file, &why);
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
        NSString *why = nil;
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
    [NSApp runModalForWindow:w];
    [w orderOut:nil];
    if (!result) return 0;
    apply();
    return 1;
}
