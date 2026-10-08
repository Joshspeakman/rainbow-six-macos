// The launch window: resolution, frame rate and updates, remembered in <data>/launcher.plist, then Play.
// Its look comes from the game itself, read at launch from the player's own Resources/Objects.ds2res (the main menu's
// logo, red backdrop and wooden menu bars) and set in Copperplate. Nothing from the game is stored in this project;
// without the archive the window falls back to plain colours.
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <ImageIO/ImageIO.h>
#include <CommonCrypto/CommonDigest.h>
#include <zlib.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>

// ---------------------------------------------------------------- reading textures from a tank (.dsres) archive
static NSData *tank_read(NSString *tankPath, NSString *want)
{
    NSData *file = [NSData dataWithContentsOfFile:tankPath options:NSDataReadingMappedIfSafe error:nil];
    if (file.length < 64) return nil;
    const uint8_t *d = file.bytes; size_t n = file.length;
    #define U32(o) (*(const uint32_t *)(d + (o)))
    #define U16(o) (*(const uint16_t *)(d + (o)))
    if (memcmp(d, "DSigTank", 8) && memcmp(d, "DSg2Tank", 8)) return nil;   /* Dungeon Siege, Dungeon Siege II */
    uint32_t dirset = U32(12), fileset = U32(16), dataoff = U32(24);
    if (dirset >= n || fileset >= n) return nil;
    NSMutableDictionary<NSNumber *, NSString *> *dirs = [NSMutableDictionary dictionary];
    NSMutableDictionary<NSNumber *, NSNumber *> *parents = [NSMutableDictionary dictionary];
    for (uint32_t k = 0, nd = U32(dirset); k < nd; k++) {
        uint32_t off = U32(dirset + 4 + 4 * k), o = dirset + off; uint16_t len = U16(o + 16);
        dirs[@(off)] = [[NSString alloc] initWithBytes:d + o + 18 length:len encoding:NSISOLatin1StringEncoding];
        parents[@(off)] = @(U32(o));
    }
    NSString *(^path)(uint32_t) = ^NSString *(uint32_t off) {
        NSMutableArray *parts = [NSMutableArray array];
        for (int guard = 0; guard < 64 && dirs[@(off)].length; guard++) { [parts insertObject:dirs[@(off)] atIndex:0]; uint32_t p = parents[@(off)].unsignedIntValue; if (p == off) break; off = p; }
        return [parts componentsJoinedByString:@"/"];
    };
    for (uint32_t k = 0, nf = U32(fileset); k < nf; k++) {
        uint32_t o = fileset + U32(fileset + 4 + 4 * k), parent = U32(o), size = U32(o + 4), foff = U32(o + 8); uint16_t fmt = U16(o + 24), len = U16(o + 28);
        NSString *name = [[NSString alloc] initWithBytes:d + o + 30 length:len encoding:NSISOLatin1StringEncoding];
        if ([[NSString stringWithFormat:@"%@/%@", path(parent), name] caseInsensitiveCompare:want] != NSOrderedSame) continue;
        uint32_t o2 = (o + 30 + len + 1 + 3) & ~3u;
        if (fmt == 0) return dataoff + foff + size <= n ? [NSData dataWithBytes:d + dataoff + foff length:size] : nil;
        uint32_t chunk = U32(o2 + 4); o2 += 8;
        NSMutableData *out = [NSMutableData dataWithCapacity:size];
        for (uint32_t c = 0; c < (size + chunk - 1) / chunk; c++) {
            uint32_t usz = U32(o2 + 16 * c), csz = U32(o2 + 16 * c + 4), extra = U32(o2 + 16 * c + 8), coff = U32(o2 + 16 * c + 12);
            const uint8_t *src = d + dataoff + foff + coff;
            if (dataoff + foff + coff + csz + extra > n) return nil;
            if (csz < usz) { NSMutableData *u = [NSMutableData dataWithLength:usz]; uLongf ul = usz;
                if (uncompress(u.mutableBytes, &ul, src, csz) != Z_OK) return nil; u.length = ul; [out appendData:u]; }
            else [out appendBytes:src length:usz];
            if (extra) [out appendBytes:src + csz length:extra];
        }
        out.length = size; return out;
    }
    return nil;
    #undef U32
    #undef U16
}
/* a DDS texture (DXT-compressed or not) -> CGImage, through ImageIO */
static CGImageRef dds_image(NSData *d)
{
    if (d.length < 128 || memcmp(d.bytes, "DDS ", 4)) return NULL;
    CGImageSourceRef src = CGImageSourceCreateWithData((__bridge CFDataRef)d, NULL); if (!src) return NULL;
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, NULL); CFRelease(src); return img;
}
static NSImage *nsimg(CGImageRef img, CGRect crop)
{
    if (!img) return nil;
    CGImageRef c = CGRectIsEmpty(crop) ? CGImageRetain(img) : CGImageCreateWithImageInRect(img, crop);
    NSImage *r = [[NSImage alloc] initWithCGImage:c size:NSZeroSize]; CGImageRelease(c); return r;
}
/* the part of an image with any opacity */
static CGRect opaque_bounds(CGImageRef img)
{
    size_t w = CGImageGetWidth(img), h = CGImageGetHeight(img); uint8_t *px = calloc(w * h, 4);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB(); CGContextRef c = CGBitmapContextCreate(px, w, h, 8, w * 4, cs, (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
    CGContextDrawImage(c, CGRectMake(0, 0, w, h), img);
    size_t x0 = w, y0 = h, x1 = 0, y1 = 0;
    for (size_t y = 0; y < h; y++) for (size_t x = 0; x < w; x++) if (px[(y * w + x) * 4 + 3] > 8) { if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
    CGContextRelease(c); CGColorSpaceRelease(cs); free(px);
    return x1 >= x0 ? CGRectMake(x0, y0, x1 - x0 + 1, y1 - y0 + 1) : CGRectZero;   /* bitmap rows run top-down here */
}

// ---------------------------------------------------------------- settings
typedef struct { NSString *value, *label, *note; } Choice;
@interface DSRow : NSObject
@property NSString *title; @property NSArray<NSDictionary *> *choices; @property NSInteger index;
@property (copy) void (^activate)(void);     /* a row that opens something instead of cycling (Mods) */
@end
@implementation DSRow @end

static NSColor *gold(void) { return [NSColor colorWithSRGBRed:0.86 green:0.77 blue:0.55 alpha:1]; }
static NSColor *parchment(void) { return [NSColor colorWithSRGBRed:0.94 green:0.91 blue:0.84 alpha:1]; }
static NSColor *dim(void) { return [NSColor colorWithSRGBRed:0.66 green:0.61 blue:0.52 alpha:1]; }
static NSFont *cp(CGFloat size, BOOL bold)
{
    NSFont *f = [NSFont fontWithName:bold ? @"Copperplate-Bold" : @"Copperplate-Light" size:size] ?: [NSFont fontWithName:@"Copperplate" size:size];
    return f ?: [NSFont systemFontOfSize:size weight:bold ? NSFontWeightSemibold : NSFontWeightRegular];
}

// ---------------------------------------------------------------- the view
@interface DSLaunchView : NSView
@property NSArray<DSRow *> *rows; @property NSInteger focus; @property NSInteger hover; @property NSInteger pressed;
@property NSImage *stone, *plaque, *trim, *wood, *woodHover, *woodDown, *logo; @property NSString *note;
@property (copy) void (^onPlay)(void); @property (copy) void (^onQuit)(void); @property (copy) void (^onChange)(void);
@end

enum { HIT_NONE = -1, HIT_PLAY = 100, HIT_QUIT = 101 };   /* rows: 10*row + 0 (left arrow) / 1 (value) / 2 (right arrow) */

@implementation DSLaunchView
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { return YES; }
- (NSRect)rowRect:(NSInteger)k { return NSMakeRect(70, 196 + 74 * k, self.bounds.size.width - 140, 64); }
- (NSRect)arrow:(NSInteger)k right:(BOOL)right { NSRect r = [self rowRect:k]; return NSMakeRect(right ? NSMaxX(r) - 34 : NSMaxX(r) - 330, r.origin.y + 6, 30, 30); }
- (NSRect)valueRect:(NSInteger)k { NSRect r = [self rowRect:k]; return NSMakeRect(NSMaxX(r) - 300, r.origin.y + 9, 262, 30); }
- (NSRect)playRect { NSRect b = self.bounds; return NSMakeRect(NSMidX(b) + 12, b.size.height - 84, 220, 56); }
- (NSRect)quitRect { NSRect b = self.bounds; return NSMakeRect(NSMidX(b) - 232, b.size.height - 84, 220, 56); }
- (NSInteger)hit:(NSPoint)p
{
    if (NSPointInRect(p, self.playRect)) return HIT_PLAY;
    if (NSPointInRect(p, self.quitRect)) return HIT_QUIT;
    for (NSInteger k = 0; k < (NSInteger)self.rows.count; k++) {
        if (NSPointInRect(p, [self arrow:k right:NO])) return 10 * k;
        if (NSPointInRect(p, [self arrow:k right:YES])) return 10 * k + 2;
        if (NSPointInRect(p, [self valueRect:k])) return 10 * k + 1;
    }
    return HIT_NONE;
}
- (void)updateTrackingAreas
{
    for (NSTrackingArea *t in self.trackingAreas) [self removeTrackingArea:t];
    [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:self.bounds options:NSTrackingMouseMoved | NSTrackingActiveAlways | NSTrackingInVisibleRect owner:self userInfo:nil]];
}
- (void)mouseMoved:(NSEvent *)e { NSInteger h = [self hit:[self convertPoint:e.locationInWindow fromView:nil]]; if (h != self.hover) { self.hover = h; self.needsDisplay = YES; } }
- (void)mouseDown:(NSEvent *)e { self.pressed = [self hit:[self convertPoint:e.locationInWindow fromView:nil]]; self.needsDisplay = YES; }
- (void)mouseUp:(NSEvent *)e
{
    NSInteger h = [self hit:[self convertPoint:e.locationInWindow fromView:nil]], p = self.pressed; self.pressed = HIT_NONE; self.needsDisplay = YES;
    if (h != p || h == HIT_NONE) return;
    if (h == HIT_PLAY) { if (self.onPlay) self.onPlay(); return; }
    if (h == HIT_QUIT) { if (self.onQuit) self.onQuit(); return; }
    NSInteger k = h / 10; self.focus = k;
    if (self.rows[k].activate) { self.rows[k].activate(); self.needsDisplay = YES; return; }
    [self step:k by:(h % 10 == 0) ? -1 : 1];
}
- (void)step:(NSInteger)k by:(NSInteger)dir
{
    DSRow *r = self.rows[k]; NSInteger n = (NSInteger)r.choices.count;
    if (r.activate) { r.activate(); self.needsDisplay = YES; return; }
    r.index = (r.index + dir + n) % n; self.needsDisplay = YES;
    if (self.onChange) self.onChange();
}
- (void)keyDown:(NSEvent *)e
{
    switch (e.keyCode) {
    case 0x24: case 0x4c: if (self.rows[self.focus].activate) { self.rows[self.focus].activate(); self.needsDisplay = YES; return; }
              if (self.onPlay) self.onPlay(); return;      /* Return, Enter */
    case 0x35: if (self.onQuit) self.onQuit(); return;                  /* Escape */
    case 0x7e: self.focus = (self.focus + (NSInteger)self.rows.count - 1) % (NSInteger)self.rows.count; self.needsDisplay = YES; return;
    case 0x7d: self.focus = (self.focus + 1) % (NSInteger)self.rows.count; self.needsDisplay = YES; return;
    case 0x7b: [self step:self.focus by:-1]; return;
    case 0x7c: [self step:self.focus by:1]; return;
    }
    [super keyDown:e];
}
static void draw_text(NSString *s, NSFont *f, NSColor *c, NSRect r, NSTextAlignment al, NSShadow *sh)
{
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new]; ps.alignment = al; ps.lineBreakMode = NSLineBreakByTruncatingTail;
    NSMutableDictionary *a = [@{NSFontAttributeName: f, NSForegroundColorAttributeName: c, NSParagraphStyleAttributeName: ps} mutableCopy];
    if (sh) a[NSShadowAttributeName] = sh;
    [s drawInRect:r withAttributes:a];
}
- (void)drawTitle:(NSRect)r
{
    /* "DUNGEON SIEGE" in the logo's manner: brushed silver letters, a copper rim, a dark drop shadow */
    NSString *t = @"DUNGEON SIEGE II"; NSFont *f = cp(42, YES);
    NSDictionary *at = @{NSFontAttributeName: f, NSKernAttributeName: @3};
    NSSize sz = [t sizeWithAttributes:at]; NSPoint o = NSMakePoint(NSMidX(r) - sz.width / 2, NSMidY(r) - sz.height / 2 - 2);
    NSShadow *sh = [NSShadow new]; sh.shadowColor = [NSColor colorWithWhite:0 alpha:0.85]; sh.shadowOffset = NSMakeSize(0, -3); sh.shadowBlurRadius = 5;
    NSMutableDictionary *rim = [at mutableCopy]; rim[NSStrokeColorAttributeName] = [NSColor colorWithSRGBRed:0.62 green:0.33 blue:0.16 alpha:1]; rim[NSStrokeWidthAttributeName] = @(9); rim[NSShadowAttributeName] = sh;
    rim[NSForegroundColorAttributeName] = [NSColor colorWithSRGBRed:0.62 green:0.33 blue:0.16 alpha:1];
    [t drawAtPoint:o withAttributes:rim];
    /* silver fill: the glyphs as a clip, a vertical gradient through it */
    NSImage *mask = [NSImage imageWithSize:sz flipped:NO drawingHandler:^BOOL(NSRect dst) {
        NSMutableDictionary *m = [at mutableCopy]; m[NSForegroundColorAttributeName] = NSColor.whiteColor; [t drawAtPoint:NSZeroPoint withAttributes:m];
        NSGradient *g = [[NSGradient alloc] initWithColorsAndLocations:[NSColor colorWithWhite:0.98 alpha:1], 0.0, [NSColor colorWithWhite:0.72 alpha:1], 0.45,
                         [NSColor colorWithWhite:0.42 alpha:1], 0.5, [NSColor colorWithWhite:0.86 alpha:1], 0.8, [NSColor colorWithWhite:0.6 alpha:1], 1.0, nil];
        [[NSGraphicsContext currentContext] setCompositingOperation:NSCompositingOperationSourceIn];
        [g drawInRect:dst angle:90];
        return YES;
    }];
    [mask drawInRect:NSMakeRect(o.x, o.y, sz.width, sz.height) fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1 respectFlipped:YES hints:nil];
}
- (void)drawButton:(NSRect)r title:(NSString *)title code:(NSInteger)code primary:(BOOL)primary
{
    NSImage *img = self.pressed == code ? (self.woodDown ?: self.wood) : self.hover == code ? (self.woodHover ?: self.wood) : self.wood;
    if (img) [img drawInRect:r fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1 respectFlipped:YES hints:@{NSImageHintInterpolation: @(NSImageInterpolationHigh)}];
    else { [[NSColor colorWithSRGBRed:0.36 green:0.22 blue:0.12 alpha:1] setFill]; [[NSBezierPath bezierPathWithRoundedRect:r xRadius:6 yRadius:6] fill]; }
    NSShadow *sh = [NSShadow new]; sh.shadowColor = [NSColor colorWithWhite:0 alpha:0.9]; sh.shadowOffset = NSMakeSize(0, -1.5); sh.shadowBlurRadius = 2;
    NSColor *c = self.hover == code ? [NSColor colorWithSRGBRed:1 green:0.93 blue:0.72 alpha:1] : primary ? gold() : parchment();
    draw_text(title, cp(primary ? 24 : 20, YES), c, NSMakeRect(r.origin.x, NSMidY(r) - (primary ? 15 : 13), r.size.width, 32), NSTextAlignmentCenter, sh);
}
- (void)drawArrow:(NSRect)r right:(BOOL)right lit:(BOOL)lit
{
    NSBezierPath *p = [NSBezierPath bezierPath]; CGFloat x0 = r.origin.x + 7, x1 = NSMaxX(r) - 7, y0 = r.origin.y + 6, y1 = NSMaxY(r) - 6, ym = NSMidY(r);
    if (right) { [p moveToPoint:NSMakePoint(x0, y0)]; [p lineToPoint:NSMakePoint(x1, ym)]; [p lineToPoint:NSMakePoint(x0, y1)]; }
    else { [p moveToPoint:NSMakePoint(x1, y0)]; [p lineToPoint:NSMakePoint(x0, ym)]; [p lineToPoint:NSMakePoint(x1, y1)]; }
    [p closePath];
    NSGradient *g = [[NSGradient alloc] initWithStartingColor:[NSColor colorWithWhite:lit ? 1.0 : 0.88 alpha:1] endingColor:[NSColor colorWithWhite:lit ? 0.7 : 0.45 alpha:1]];
    [g drawInBezierPath:p angle:90];
    [[NSColor colorWithSRGBRed:0.55 green:0.3 blue:0.14 alpha:1] setStroke]; p.lineWidth = 1.5; [p stroke];
}
/* a riveted iron band around a plaque, like the frames of the game's menus */
- (void)drawIronFrame:(NSRect)r
{
    NSBezierPath *outer = [NSBezierPath bezierPathWithRect:NSInsetRect(r, 3, 3)];
    outer.lineWidth = 7; [[NSColor colorWithWhite:0.08 alpha:1] setStroke]; [outer stroke];
    outer.lineWidth = 5; [[NSColor colorWithSRGBRed:0.45 green:0.46 blue:0.47 alpha:1] setStroke]; [outer stroke];
    NSBezierPath *hi = [NSBezierPath bezierPathWithRect:NSInsetRect(r, 1.5, 1.5)]; hi.lineWidth = 1; [[NSColor colorWithWhite:0.78 alpha:0.8] setStroke]; [hi stroke];
    NSBezierPath *lo = [NSBezierPath bezierPathWithRect:NSInsetRect(r, 6, 6)]; lo.lineWidth = 1.2; [[NSColor colorWithWhite:0.05 alpha:0.9] setStroke]; [lo stroke];
    void (^rivet)(CGFloat, CGFloat) = ^(CGFloat x, CGFloat y) {
        NSRect d = NSMakeRect(x - 4, y - 4, 8, 8);
        NSGradient *g = [[NSGradient alloc] initWithColorsAndLocations:[NSColor colorWithSRGBRed:0.98 green:0.86 blue:0.55 alpha:1], 0.0,
                         [NSColor colorWithSRGBRed:0.62 green:0.42 blue:0.16 alpha:1], 0.6, [NSColor colorWithSRGBRed:0.25 green:0.15 blue:0.05 alpha:1], 1.0, nil];
        [g drawInBezierPath:[NSBezierPath bezierPathWithOvalInRect:d] relativeCenterPosition:NSMakePoint(-0.35, 0.35)];
    };
    for (CGFloat x = r.origin.x + 14; x <= NSMaxX(r) - 13; x += (r.size.width - 28) / 8) { rivet(x, r.origin.y + 3.5); rivet(x, NSMaxY(r) - 3.5); }
    rivet(r.origin.x + 3.5, NSMidY(r)); rivet(NSMaxX(r) - 3.5, NSMidY(r));
}
- (void)drawRect:(NSRect)dirty
{
    NSRect b = self.bounds;
    /* stone wall */
    if (self.stone) {                                   /* filling the window, the aspect kept (cropped at the sides) */
        CGFloat sc = MAX(b.size.width / self.stone.size.width, b.size.height / self.stone.size.height), w = self.stone.size.width * sc, h = self.stone.size.height * sc;
        [self.stone drawInRect:NSMakeRect(NSMidX(b) - w / 2, NSMidY(b) - h / 2, w, h) fromRect:NSZeroRect operation:NSCompositingOperationCopy fraction:1 respectFlipped:YES hints:@{NSImageHintInterpolation: @(NSImageInterpolationHigh)}];
    }
    else { [[NSColor colorWithSRGBRed:0.16 green:0.03 blue:0.02 alpha:1] setFill]; NSRectFill(b); }
    NSGradient *vig = [[NSGradient alloc] initWithColorsAndLocations:[NSColor colorWithWhite:0 alpha:0.15], 0.0, [NSColor colorWithWhite:0 alpha:0.62], 1.0, nil];
    [vig drawInRect:b relativeCenterPosition:NSMakePoint(0, 0.1)];
    /* title plaque */
    NSRect pr = NSMakeRect(NSMidX(b) - 290, 22, 580, 128);
    if (self.logo) [self.logo drawInRect:NSMakeRect(NSMidX(b) - 210, 8, 420, 420 * self.logo.size.height / self.logo.size.width) fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1 respectFlipped:YES hints:@{NSImageHintInterpolation: @(NSImageInterpolationHigh)}];
    else {
        [[NSColor colorWithSRGBRed:0.42 green:0.25 blue:0.15 alpha:1] setFill]; NSRectFill(NSInsetRect(pr, 6, 6));
        [self drawIronFrame:pr];
        [self drawTitle:NSMakeRect(pr.origin.x, pr.origin.y + 6, pr.size.width, pr.size.height - 30)];
    }
    NSShadow *sh = [NSShadow new]; sh.shadowColor = [NSColor colorWithWhite:0 alpha:0.9]; sh.shadowOffset = NSMakeSize(0, -1); sh.shadowBlurRadius = 2;
    draw_text(@"NATIVE EDITION FOR macOS", cp(13, NO), gold(), NSMakeRect(pr.origin.x, NSMaxY(pr) - 34, pr.size.width, 18), NSTextAlignmentCenter, sh);
    /* the settings */
    NSRect panel = NSMakeRect(54, 176, b.size.width - 108, 74 * self.rows.count + 30);
    [[NSColor colorWithWhite:0 alpha:0.5] setFill]; [[NSBezierPath bezierPathWithRoundedRect:panel xRadius:4 yRadius:4] fill];
    [[NSColor colorWithSRGBRed:0.55 green:0.38 blue:0.2 alpha:0.9] setStroke]; NSBezierPath *bp = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(panel, 0.5, 0.5) xRadius:4 yRadius:4]; bp.lineWidth = 1.5; [bp stroke];
    for (NSInteger k = 0; k < (NSInteger)self.rows.count; k++) {
        DSRow *row = self.rows[k]; NSRect r = [self rowRect:k]; NSDictionary *ch = row.choices[row.index];
        if (k == self.focus) { [[NSColor colorWithSRGBRed:0.86 green:0.6 blue:0.25 alpha:0.12] setFill]; [[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(r, -10, -2) xRadius:3 yRadius:3] fill]; }
        draw_text(row.title.uppercaseString, cp(19, YES), gold(), NSMakeRect(r.origin.x, r.origin.y + 8, 260, 28), NSTextAlignmentLeft, sh);
        [self drawArrow:[self arrow:k right:NO] right:NO lit:self.hover == 10 * k];
        [self drawArrow:[self arrow:k right:YES] right:YES lit:self.hover == 10 * k + 2];
        draw_text(ch[@"label"], cp(18, NO), self.hover == 10 * k + 1 ? NSColor.whiteColor : parchment(), [self valueRect:k], NSTextAlignmentCenter, sh);
        draw_text(ch[@"note"] ?: @"", cp(11, NO), dim(), NSMakeRect(NSMaxX(r) - 330, r.origin.y + 42, 330, 18), NSTextAlignmentCenter, nil);
        if (k + 1 < (NSInteger)self.rows.count) { [[NSColor colorWithSRGBRed:0.55 green:0.38 blue:0.2 alpha:0.35] setFill]; NSRectFill(NSMakeRect(r.origin.x, NSMaxY(r) + 5, r.size.width, 1)); }
    }
    if (self.note.length) draw_text(self.note, cp(11, NO), dim(), NSMakeRect(54, NSMaxY(panel) + 10, b.size.width - 108, 30), NSTextAlignmentCenter, nil);
    [self drawButton:self.quitRect title:@"QUIT" code:HIT_QUIT primary:NO];
    [self drawButton:self.playRect title:@"PLAY" code:HIT_PLAY primary:YES];
}
@end

// ---------------------------------------------------------------- choices, settings file, the window
static NSArray<NSDictionary *> *resolution_choices(void)
{
    NSScreen *s = NSScreen.mainScreen; int W = (int)s.frame.size.width, H = (int)s.frame.size.height; double sc = s.backingScaleFactor;
    NSMutableArray *a = [NSMutableArray array]; NSMutableSet *seen = [NSMutableSet set];
    void (^add)(int, int, NSString *) = ^(int w, int h, NSString *note) {
        NSString *v = [NSString stringWithFormat:@"%dx%d", w, h]; if ([seen containsObject:v]) return; [seen addObject:v];
        [a addObject:@{@"value": v, @"label": [NSString stringWithFormat:@"%d × %d", w, h], @"note": note}];
    };
    add(W, H, @"This display (recommended)");
    if (sc > 1.01) add((int)(W * sc + 0.5), (int)(H * sc + 0.5), @"Full Retina detail; the interface is drawn very small");
    static const int std[][2] = {{2560,1600},{2560,1440},{1920,1200},{1920,1080},{1680,1050},{1600,900},{1440,900},{1366,768},{1280,800},{1280,720},{1024,768},{800,600}};
    for (size_t k = 0; k < sizeof std / sizeof *std; k++) if (std[k][0] <= W && std[k][1] <= H) add(std[k][0], std[k][1], @"Scaled to fill the screen");
    return a;
}
static NSArray<NSDictionary *> *distance_choices(void)
{
    return @[@{@"value": @"100", @"label": @"Original", @"note": @"As the game shipped"},
             @{@"value": @"125", @"label": @"Far", @"note": @"125% of the original"},
             @{@"value": @"150", @"label": @"Farther", @"note": @"150%, like the SeeFar mods"},
             @{@"value": @"200", @"label": @"Very Far", @"note": @"200% of the original"},
             @{@"value": @"250", @"label": @"Horizon", @"note": @"250%; heavier in large outdoor areas"},
             @{@"value": @"300", @"label": @"Maximum", @"note": @"300%; may slow down in the biggest vistas"}];
}
static NSArray<NSDictionary *> *framerate_choices(void)
{
    NSInteger hz = NSScreen.mainScreen.maximumFramesPerSecond; if (hz <= 0) hz = 60;
    NSMutableArray *a = [NSMutableArray arrayWithObject:@{@"value": @"auto", @"label": @"Automatic",
        @"note": hz >= 120 ? @"Smooth 120 fps, 60 fps in the heaviest scenes" : @"Matches the display"}];
    if (hz >= 120) [a addObject:@{@"value": @"120", @"label": @"120 FPS", @"note": @"Locked to 120"}];
    [a addObject:@{@"value": @"60", @"label": @"60 FPS", @"note": @"Locked to 60; cooler and quieter"}];
    [a addObject:@{@"value": @"30", @"label": @"30 FPS", @"note": @"Saves battery"}];
    [a addObject:@{@"value": @"unlimited", @"label": @"Unlimited", @"note": @"No cap and no vertical sync (may tear)"}];
    return a;
}
/* ---- the CD key for online play ----
 * The game presents a "DoubleHash" of the player's product key to the GameSpy (now OpenSpy) lobby. The retail
 * installer derived it from the key; digital releases never ran that installer. As its Setup.exe does: the 25
 * characters read as a base-24 number over BCDFGHJKMPQRTVWXY2346789 into 15 little-endian bytes, SHA-1 of those,
 * SHA-1 of the digest, the first 8 bytes. Only the hash is stored (<data>/cdkey-hash.txt; the runtime puts it in the
 * registry, registry.c), never the key. Same computation as install.sh --cd-key. */
static BOOL cdkey_hash(NSString *key, uint8_t out[8])
{
    static const char A[] = "BCDFGHJKMPQRTVWXY2346789";
    NSString *k = [[[key uppercaseString] stringByReplacingOccurrencesOfString:@"-" withString:@""] stringByReplacingOccurrencesOfString:@" " withString:@""];
    if (k.length != 25) return NO;
    uint8_t buf[15] = {0}; int top = 0;
    for (NSUInteger i = 0; i < 25; i++) {
        unichar ch = [k characterAtIndex:i]; const char *p = ch < 128 ? strchr(A, (char)ch) : NULL;
        if (!p || !ch) return NO;
        unsigned c = (unsigned)(p - A);
        for (int j = 0; j <= top; j++) { unsigned v = buf[j] * 24u + c; buf[j] = (uint8_t)v; c = v >> 8; }
        if (c) { if (top + 1 >= 15) return NO; buf[++top] = (uint8_t)c; }
    }
    uint8_t d1[CC_SHA1_DIGEST_LENGTH], d2[CC_SHA1_DIGEST_LENGTH];
    CC_SHA1(buf, sizeof buf, d1); CC_SHA1(d1, sizeof d1, d2);
    memcpy(out, d2, 8); return YES;
}
static NSString *cdkey_path(NSString *data) { return [data stringByAppendingPathComponent:@"cdkey-hash.txt"]; }
static BOOL cdkey_present(NSString *data) { return [NSFileManager.defaultManager fileExistsAtPath:cdkey_path(data)]; }
static NSArray<NSDictionary *> *cdkey_choice(NSString *data)
{
    return cdkey_present(data) ? @[@{@"value": @"set", @"label": @"CD Key Entered", @"note": @"Online play ready · press to change it"}]
                               : @[@{@"value": @"none", @"label": @"No CD Key", @"note": @"Needed for online play · press to enter your own"}];
}
/* the entry box; returns YES when the stored key changed */
static BOOL cdkey_panel(NSString *data)
{
    for (NSString *problem = nil;;) {
        NSAlert *a = [NSAlert new];
        a.messageText = problem ?: @"Your Dungeon Siege II CD Key";
        a.informativeText = @"The 25-character product key of your own original retail copy (the sticker under disc 1 in the box), for online play. "
                             "The Steam and GOG releases come without one. "
                             "Each copy needs its own key: the same key can't be online twice at once. Only a hash of it is kept on this Mac.";
        NSSecureTextField *f = [[NSSecureTextField alloc] initWithFrame:NSMakeRect(0, 0, 320, 24)];
        f.placeholderString = @"XXXXX-XXXXX-XXXXX-XXXXX-XXXXX"; a.accessoryView = f;
        [a addButtonWithTitle:@"Save"]; [a addButtonWithTitle:@"Cancel"];
        BOOL had = cdkey_present(data); if (had) [a addButtonWithTitle:@"Remove Key"];
        a.window.initialFirstResponder = f;
        NSModalResponse r = [a runModal];
        if (r == NSAlertSecondButtonReturn) return NO;
        if (r == NSAlertThirdButtonReturn) { [NSFileManager.defaultManager removeItemAtPath:cdkey_path(data) error:nil]; return YES; }
        uint8_t h[8];
        if (!cdkey_hash(f.stringValue, h)) { problem = @"That isn't a product key: it has 25 letters and digits (no vowels, 0, 1 or 5)."; continue; }
        NSMutableString *hex = [NSMutableString string]; for (int i = 0; i < 8; i++) [hex appendFormat:@"%02x", h[i]];
        [NSFileManager.defaultManager createDirectoryAtPath:data withIntermediateDirectories:YES attributes:nil error:nil];
        [[hex stringByAppendingString:@"\n"] writeToFile:cdkey_path(data) atomically:YES encoding:NSASCIIStringEncoding error:nil];
        [NSFileManager.defaultManager setAttributes:@{NSFilePosixPermissions: @0600} ofItemAtPath:cdkey_path(data) error:nil];
        return YES;
    }
}
static NSInteger index_of(NSArray<NSDictionary *> *c, NSString *v, NSInteger dflt)
{
    for (NSUInteger k = 0; k < c.count; k++) if ([c[k][@"value"] isEqualToString:v ?: @""]) return (NSInteger)k;
    return dflt;
}
/* the chosen settings as environment for the runtime (read later at start-up) */
static void apply(NSString *res, NSString *dist, NSString *fps)
{
    setenv("DS_RESOLUTION", res.UTF8String, 1);
    setenv("DS_DRAW_DISTANCE", dist.UTF8String, 1);
    if ([fps isEqualToString:@"unlimited"]) { setenv("DSR_FPSCAP", "0", 1); setenv("DSR_VSYNC", "0", 1); }
    else setenv("DSR_FPSCAP", fps.UTF8String, 1);
}
@interface DSBlockTarget : NSObject      /* a button action as a block */
@property (copy) void (^fire)(NSInteger tag);
- (void)clicked:(NSButton *)b;
@end
@implementation DSBlockTarget
- (void)clicked:(NSButton *)b { if (self.fire) self.fire(b.tag); }
@end

// ---------------------------------------------------------------- updates
/* install.sh gives each app an updater (Contents/MacOS/update, passed in DS_UPDATER): it fetches the branch the app
 * follows (main, or nightly for Nightly) from GitHub into a worktree of the checkout the app was installed from and
 * rebuilds the app from it, keeping the game folder, saves and settings. The launcher asks GitHub (in the
 * background, each time it opens) whether that branch moved past the commit the app was built from. */
enum { UPD_NONE, UPD_CHECKING, UPD_CURRENT, UPD_AVAILABLE, UPD_ERROR };
static void on_main(dispatch_block_t b) { CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, b); CFRunLoopWakeUp(CFRunLoopGetMain()); }
static NSString *short_commit(NSString *c) { return c.length > 7 ? [c substringToIndex:7] : (c ?: @""); }
static NSString *update_check(NSString *updater)   /* the newest commit of the branch on GitHub; nil if it can't be reached */
{
    NSTask *t = [NSTask new]; t.executableURL = [NSURL fileURLWithPath:@"/bin/bash"]; t.arguments = @[updater, @"--check"];
    NSPipe *p = [NSPipe pipe]; t.standardOutput = p; t.standardError = [NSFileHandle fileHandleWithNullDevice];
    if (![t launchAndReturnError:nil]) return nil;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC), dispatch_get_global_queue(0, 0), ^{ if (t.running) [t terminate]; });
    NSData *d = [p.fileHandleForReading readDataToEndOfFile]; [t waitUntilExit];
    NSString *out = [[[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    return t.terminationStatus == 0 && out.length >= 40 ? [out substringToIndex:40] : nil;
}
static NSArray<NSDictionary *> *update_choice(int state, NSString *branch, NSString *built, NSString *latest)
{
    NSString *label = @"Not available", *note = @"This copy wasn't installed from the GitHub repository";
    switch (state) {
    case UPD_CHECKING: label = @"Checking..."; note = [NSString stringWithFormat:@"Asking GitHub for the newest %@", branch]; break;
    case UPD_CURRENT: label = @"Up to date"; note = [NSString stringWithFormat:@"%@ %@", branch, short_commit(built)]; break;
    case UPD_AVAILABLE: label = @"Update available"; note = [NSString stringWithFormat:@"%@ -> %@: press to install", short_commit(built), short_commit(latest)]; break;
    case UPD_ERROR: label = @"Couldn't check"; note = @"GitHub unreachable or not signed in: press to retry"; break;
    }
    return @[@{@"value": @"", @"label": label, @"note": note}];
}
/* the phase of install.sh's output, in words */
static NSString *update_phase(NSString *line, NSString *branch)
{
    if ([line hasPrefix:@"== fetching"]) return @"Downloading the update from GitHub";
    if ([line hasPrefix:[NSString stringWithFormat:@"== %@:", branch]]) return [@"Got " stringByAppendingString:[line substringFromIndex:branch.length + 4]];
    if ([line hasPrefix:@"== analysis"]) return @"Rebuilding from your game: analysing its code (1 of 4)";
    if ([line hasPrefix:@"== lifting"]) return @"Rebuilding from your game: translating to native code (2 of 4)";
    if ([line hasPrefix:@"== compiling"]) return @"Rebuilding from your game: compiling (3 of 4)";
    if ([line hasPrefix:@"== installing"]) return @"Installing the new app (4 of 4)";
    return nil;
}
/* runs the updater with a progress window (modal); returns YES when the app was rebuilt */
static BOOL update_run(NSWindow *parent, NSString *updater, NSString *branch, NSString *logPath)
{
    NSPanel *pn = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 520, 170) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    pn.title = @"Updating";
    NSTextField *title = [NSTextField labelWithString:@"Updating to the newest version"]; title.font = [NSFont boldSystemFontOfSize:14]; title.frame = NSMakeRect(20, 128, 480, 22);
    NSTextField *status = [NSTextField wrappingLabelWithString:@"Starting..."]; status.frame = NSMakeRect(20, 64, 480, 56);
    NSProgressIndicator *spin = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(20, 44, 480, 12)]; spin.indeterminate = YES; [spin startAnimation:nil];
    NSTextField *time = [NSTextField labelWithString:@"This takes a few minutes. Your saves and settings are kept."]; time.font = [NSFont systemFontOfSize:11]; time.textColor = NSColor.secondaryLabelColor; time.frame = NSMakeRect(20, 14, 360, 18);
    NSButton *btn = [NSButton buttonWithTitle:@"Close" target:nil action:nil]; btn.frame = NSMakeRect(400, 8, 100, 30); btn.hidden = YES;
    for (NSView *x in @[title, status, spin, time, btn]) [pn.contentView addSubview:x];
    if (parent) [pn setFrameOrigin:NSMakePoint(NSMidX(parent.frame) - 260, NSMidY(parent.frame) - 85)]; else [pn center];
    [[NSFileManager defaultManager] createFileAtPath:logPath contents:nil attributes:nil];
    NSFileHandle *log = [NSFileHandle fileHandleForWritingAtPath:logPath];
    NSTask *t = [NSTask new]; t.executableURL = [NSURL fileURLWithPath:@"/bin/bash"]; t.arguments = @[updater];
    NSPipe *pipe = [NSPipe pipe]; t.standardOutput = pipe; t.standardError = pipe;
    NSMutableString *pending = [NSMutableString new], *tail = [NSMutableString new];
    pipe.fileHandleForReading.readabilityHandler = ^(NSFileHandle *h) {
        NSData *d = h.availableData; if (!d.length) return;
        [log writeData:d];
        NSString *chunk = [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding] ?: @"";
        @synchronized (pending) {
            [pending appendString:chunk];
            NSRange nl;
            while ((nl = [pending rangeOfString:@"\n"]).location != NSNotFound) {
                NSString *line = [pending substringToIndex:nl.location]; [pending deleteCharactersInRange:NSMakeRange(0, nl.location + 1)];
                [tail appendFormat:@"%@\n", line]; if (tail.length > 600) [tail deleteCharactersInRange:NSMakeRange(0, tail.length - 600)];
                NSString *ph = update_phase(line, branch);
                if (ph) on_main(^{ status.stringValue = ph; });
            }
        }
    };
    __block BOOL ok = NO;
    __attribute__((objc_precise_lifetime)) DSBlockTarget *bt = [DSBlockTarget new];   /* a button's target is weak: kept here */
    bt.fire = ^(NSInteger tag) { (void)tag; [NSApp stopModal]; };
    btn.target = bt; btn.action = @selector(clicked:);
    t.terminationHandler = ^(NSTask *task) {
        int code = task.terminationStatus;
        on_main(^{
            pipe.fileHandleForReading.readabilityHandler = nil; [log closeFile];
            [spin stopAnimation:nil]; spin.hidden = YES; btn.hidden = NO;
            if (code == 0) { ok = YES; title.stringValue = @"Updated"; status.stringValue = @"The new version is installed. The app restarts when you press Restart."; btn.title = @"Restart"; }
            else {
                NSString *last; @synchronized (pending) { last = [[tail componentsSeparatedByString:@"\n"] componentsJoinedByString:@" "]; }
                title.stringValue = @"The update didn't finish";
                status.stringValue = [NSString stringWithFormat:@"Nothing was changed if it stopped before installing. Details: %@  (%@)", logPath,
                                      last.length > 160 ? [last substringFromIndex:last.length - 160] : last];
                time.stringValue = @"";
            }
        });
    };
    if (![t launchAndReturnError:nil]) { [log closeFile]; return NO; }
    [pn makeKeyAndOrderFront:nil];
    [NSApp runModalForWindow:pn];
    [pn orderOut:nil];
    return ok;
}

/* Shows the launch window (modal) unless DS_NO_LAUNCHER is set; returns 0 to quit. DS_LAUNCHER_SHOT=<png> renders it
 * to an image instead (development). */
int ds_launcher_run(const char *game_dir, const char *data_dir)
{
    NSString *data = @(data_dir), *plistPath = [data stringByAppendingPathComponent:@"launcher.plist"];
    NSDictionary *saved = [NSDictionary dictionaryWithContentsOfFile:plistPath] ?: @{};
    DSRow *res = [DSRow new], *dist = [DSRow new], *fps = [DSRow new];
    res.title = @"Resolution"; res.choices = resolution_choices(); res.index = index_of(res.choices, saved[@"resolution"], 0);
    dist.title = @"View Distance"; dist.choices = distance_choices(); dist.index = index_of(dist.choices, saved[@"viewDistance"], 0);
    fps.title = @"Frame Rate"; fps.choices = framerate_choices(); fps.index = index_of(fps.choices, saved[@"frameRate"], 0);
    const char *shot = getenv("DS_LAUNCHER_SHOT");
    if (getenv("DS_NO_LAUNCHER") && !shot) {
        if (saved.count) apply(res.choices[res.index][@"value"], dist.choices[dist.index][@"value"], fps.choices[fps.index][@"value"]);
        return 1;
    }

    NSString *updater = getenv("DS_UPDATER") ? @(getenv("DS_UPDATER")) : nil, *branch = getenv("DS_UPDATE_BRANCH") ? @(getenv("DS_UPDATE_BRANCH")) : @"main";
    NSString *built = getenv("DS_BUILT_COMMIT") ? @(getenv("DS_BUILT_COMMIT")) : @"";
    if (![[NSFileManager defaultManager] isExecutableFileAtPath:updater ?: @""]) updater = nil;
    DSRow *upd = [DSRow new]; upd.title = @"Updates";
    __block int ustate = updater ? UPD_CHECKING : UPD_NONE; __block NSString *latest = nil;
    upd.choices = update_choice(ustate, branch, built, nil);
    BOOL shotMode = shot != NULL;
    DSRow *online = [DSRow new]; online.title = @"Online"; online.choices = cdkey_choice(data);
    __weak DSRow *wo = online;
    online.activate = ^{ if (cdkey_panel(data)) wo.choices = cdkey_choice(data); };
    NSArray<DSRow *> *rows = @[res, dist, fps, online, upd];
    DSLaunchView *v = [[DSLaunchView alloc] initWithFrame:NSMakeRect(0, 0, 780, 700 + 74 * (NSInteger)(rows.count - 5))];
    v.rows = rows; v.hover = v.pressed = HIT_NONE;
    __weak DSLaunchView *wv = v; __weak DSRow *wu = upd;
    void (^check)(void) = ^{
        if (!updater) return;
        ustate = UPD_CHECKING; wu.choices = update_choice(ustate, branch, built, nil); wv.needsDisplay = YES;
        void (^done)(NSString *) = ^(NSString *l) {
            latest = l; ustate = !l ? UPD_ERROR : [l isEqualToString:built] ? UPD_CURRENT : UPD_AVAILABLE;
            wu.choices = update_choice(ustate, branch, built, l); wv.needsDisplay = YES;
        };
        if (shotMode) { done(update_check(updater)); return; }               /* development: the result in the image */
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ NSString *l = update_check(updater); on_main(^{ done(l); }); });
    };
    check();
    upd.activate = ^{
        if (ustate == UPD_CHECKING) return;
        if (ustate != UPD_AVAILABLE) { check(); return; }
        NSAlert *a = [NSAlert new];
        a.messageText = [NSString stringWithFormat:@"Update to the newest %@?", [branch isEqualToString:@"nightly"] ? @"Nightly" : @"version"];
        a.informativeText = [NSString stringWithFormat:@"The %@ branch (%@) is downloaded from GitHub and the app is rebuilt from your own copy of the game, "
                             "which takes a few minutes. Your game folder, saves and settings are kept. The app restarts when it is done.", branch, short_commit(latest)];
        [a addButtonWithTitle:@"Update"]; [a addButtonWithTitle:@"Cancel"];
        if ([a runModal] != NSAlertFirstButtonReturn) return;
        NSString *logPath = [data stringByAppendingPathComponent:@"update.log"];
        if (update_run(wv.window, updater, branch, logPath)) {   /* restart: the new app, then this one ends */
            NSString *app = [[[updater stringByDeletingLastPathComponent] stringByDeletingLastPathComponent] stringByDeletingLastPathComponent];
            NSTask *o = [NSTask new]; o.executableURL = [NSURL fileURLWithPath:@"/usr/bin/open"]; o.arguments = @[@"-n", app];
            [o launchAndReturnError:nil]; [o waitUntilExit];
            exit(0);
        }
        check();
    };
    NSString *tank = [@(game_dir) stringByAppendingPathComponent:@"Resources/Objects.ds2res"], *m = @"art/bitmaps/gui/front_end/main/b_gui_fe_mn_";
    NSImage *(^art)(NSString *, BOOL) = ^NSImage *(NSString *name, BOOL trim) {
        CGImageRef img = dds_image(tank_read(tank, [[m stringByAppendingString:name] stringByAppendingString:@".dds"]));
        NSImage *r = img ? nsimg(img, trim ? opaque_bounds(img) : CGRectZero) : nil; if (img) CGImageRelease(img); return r;
    };
    {   /* the main menu's backdrop: eight 256x256 tiles, four across and two down */
        NSImage *bg = [[NSImage alloc] initWithSize:NSMakeSize(1024, 512)]; BOOL any = NO;
        [bg lockFocusFlipped:YES];
        [[NSColor colorWithSRGBRed:0.13 green:0.02 blue:0.01 alpha:1] setFill]; NSRectFill(NSMakeRect(0, 0, 1024, 512));   /* under the tiles' see-through parts */
        for (int k = 0; k < 8; k++) { NSImage *t = art([NSString stringWithFormat:@"back%02d", k + 1], NO);
            if (t) { any = YES; [t drawInRect:NSMakeRect(256 * (k % 4), 256 * (k / 4), 256, 256) fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1 respectFlipped:YES hints:nil]; } }
        [bg unlockFocus];
        v.stone = any ? bg : nil;
    }
    v.logo = art(@"logo", YES);
    v.wood = art(@"menubar-up", YES); v.woodHover = art(@"menubar-over", YES); v.woodDown = art(@"menubar-down", YES);
    if (shot) {   /* development: the window as an image */
        NSBitmapImageRep *rep = [v bitmapImageRepForCachingDisplayInRect:v.bounds]; [v cacheDisplayInRect:v.bounds toBitmapImageRep:rep];
        [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(shot) atomically:YES];
        return 0;
    }

    NSWindow *w = [[NSWindow alloc] initWithContentRect:v.frame styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskFullSizeContentView
                                                backing:NSBackingStoreBuffered defer:NO];
    w.releasedWhenClosed = NO;   /* ARC owns it: AppKit's default would release it again when the close button closes it (a crash on exit) */
    w.titlebarAppearsTransparent = YES; w.titleVisibility = NSWindowTitleHidden; w.movableByWindowBackground = YES; w.title = @"Dungeon Siege II";
    w.backgroundColor = NSColor.blackColor; w.contentView = v; [w center]; [w makeFirstResponder:v];
    __block int result = 0;
    v.onPlay = ^{ result = 1; [NSApp stopModal]; };
    v.onQuit = ^{ result = 0; [NSApp stopModal]; };
    [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowWillCloseNotification object:w queue:nil usingBlock:^(NSNotification *n) { (void)n; [NSApp stopModal]; }];
    [NSApp activateIgnoringOtherApps:YES];
    [w makeKeyAndOrderFront:nil];
    [NSApp runModalForWindow:w];
    [w orderOut:nil];
    if (!result) return 0;
    NSString *rv = res.choices[res.index][@"value"], *fv = fps.choices[fps.index][@"value"];
    NSString *dv = dist.choices[dist.index][@"value"];
    [@{@"resolution": rv, @"viewDistance": dv, @"frameRate": fv} writeToFile:plistPath atomically:YES];
    apply(rv, dv, fv);
    return 1;
}
