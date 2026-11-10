// theme.h: the installer's look. Frosted glass over a full-bleed picture, in each game's own colours and typefaces.
//
// The picture is the store's art when it can be fetched on the player's Mac (as the launchers do) and otherwise an original
// scene drawn here. Nothing from the games is in the app: their logos and backdrops only exist once the player's files do.
#import <Cocoa/Cocoa.h>
#import <CoreImage/CoreImage.h>

typedef NS_ENUM(int, ThStyle) { ThStone, ThIron, ThBronze, ThTactical };

static ThStyle th_style(NSString *s)
{
    return [s isEqual:@"iron"] ? ThIron : [s isEqual:@"bronze"] ? ThBronze : [s isEqual:@"tactical"] ? ThTactical : ThStone;
}
static NSColor *th_rgba(CGFloat r, CGFloat g, CGFloat b, CGFloat a) { return [NSColor colorWithSRGBRed:r green:g blue:b alpha:a]; }
static NSColor *th_rgb(CGFloat r, CGFloat g, CGFloat b) { return th_rgba(r, g, b, 1); }
static NSColor *th_accent(ThStyle s, CGFloat a)
{
    switch (s) {
    case ThStone: return th_rgba(0.80, 0.16, 0.10, a);
    case ThIron: return th_rgba(0.85, 0.58, 0.20, a);
    case ThBronze: return th_rgba(0.92, 0.42, 0.10, a);
    default: return th_rgba(0.16, 0.70, 0.70, a);
    }
}
static NSColor *th_text(ThStyle s) { return s == ThTactical ? th_rgb(0.90, 0.94, 0.94) : th_rgb(0.95, 0.92, 0.85); }
static NSColor *th_dim(ThStyle s) { return s == ThTactical ? th_rgb(0.58, 0.68, 0.69) : th_rgb(0.68, 0.64, 0.57); }
static NSFont *th_font(ThStyle s, CGFloat size, BOOL bold)
{
    NSFont *f = s == ThTactical ? [NSFont fontWithName:bold ? @"Arial-BoldMT" : @"ArialMT" size:size]
                                : ([NSFont fontWithName:bold ? @"Copperplate-Bold" : @"Copperplate-Light" size:size] ?: [NSFont fontWithName:@"Copperplate" size:size]);
    return f ?: [NSFont systemFontOfSize:size weight:bold ? NSFontWeightSemibold : NSFontWeightRegular];
}
static NSFont *th_title_font(ThStyle s, CGFloat size)
{
    return (s == ThTactical ? [NSFont fontWithName:@"Arial-Black" size:size] : [NSFont fontWithName:@"Copperplate-Bold" size:size]) ?: [NSFont boldSystemFontOfSize:size];
}

/* ---- the picture ---- */
static inline float th_hash(int x, int y) { uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u; h = (h ^ (h >> 13)) * 1274126177u; return (float)((h ^ (h >> 16)) & 0xffff) / 65535.0f; }
static float th_vnoise(float x, float y)
{
    int xi = (int)floorf(x), yi = (int)floorf(y); float fx = x - xi, fy = y - yi; fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
    float a = th_hash(xi, yi), b = th_hash(xi + 1, yi), c = th_hash(xi, yi + 1), d = th_hash(xi + 1, yi + 1);
    return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}
static float th_noise(float x, float y) { return 0.55f * th_vnoise(x, y) + 0.3f * th_vnoise(x * 2.1f, y * 2.1f) + 0.15f * th_vnoise(x * 4.3f, y * 4.3f); }
static void th_glow(CGFloat x, CGFloat y, CGFloat rad, NSColor *c)
{
    NSGradient *g = [[NSGradient alloc] initWithColors:@[c, [c colorWithAlphaComponent:c.alphaComponent * 0.35], [c colorWithAlphaComponent:0]]];
    [g drawFromCenter:NSMakePoint(x, y) radius:0 toCenter:NSMakePoint(x, y) radius:rad options:0];
}
/* a ridge line across the frame, filled down to the bottom */
static void th_ridge(CGFloat w, CGFloat base, CGFloat rise, CGFloat rough, int seed, NSColor *c)
{
    NSBezierPath *p = [NSBezierPath bezierPath]; [p moveToPoint:NSMakePoint(0, 0)];
    for (CGFloat x = 0; x <= w + 8; x += 8) [p lineToPoint:NSMakePoint(x, base + rise * th_noise((float)(x / rough) + seed * 17.3f, seed * 3.1f))];
    [p lineToPoint:NSMakePoint(w + 8, 0)]; [p closePath]; [c setFill]; [p fill];
}
static void th_embers(CGFloat w, CGFloat h, int n, int seed, NSColor *c)
{
    [c setFill];
    for (int k = 0; k < n; k++) {
        CGFloat x = th_hash(k, seed) * w, y = pow(th_hash(k * 3, seed + 1), 1.8) * h, s = 0.8 + 2.6 * pow(th_hash(k * 5, seed + 2), 3);
        [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(x, y, s, s)] fill];
    }
}
/* gothic spires on a roofline */
static void th_cathedral(CGFloat cx, CGFloat ground, CGFloat u, NSColor *c)
{
    [c setFill];
    NSBezierPath *p = [NSBezierPath bezierPath]; [p moveToPoint:NSMakePoint(cx - 12 * u, ground)];
    CGFloat pts[][2] = {{-12, 3.2}, {-10.4, 3.2}, {-10.4, 4.8}, {-9.8, 6.4}, {-9.2, 4.8}, {-9.2, 3.2}, {-5.6, 3.2}, {-5.6, 7.4}, {-4.4, 11}, {-3.2, 7.4}, {-3.2, 4.4},
                       {-1.3, 4.4}, {-1.3, 6}, {0, 9.2}, {1.3, 6}, {1.3, 4.4}, {3.2, 4.4}, {3.2, 7.4}, {4.4, 11}, {5.6, 7.4}, {5.6, 3.2}, {9.2, 3.2}, {9.2, 4.8}, {9.8, 6.4}, {10.4, 4.8}, {10.4, 3.2}, {12, 3.2}};
    for (unsigned k = 0; k < sizeof pts / sizeof pts[0]; k++) [p lineToPoint:NSMakePoint(cx + pts[k][0] * u, ground + pts[k][1] * u)];
    [p lineToPoint:NSMakePoint(cx + 12 * u, ground)]; [p closePath]; [p fill];
}
static void th_keep(CGFloat cx, CGFloat ground, CGFloat u, NSColor *c)
{
    [c setFill]; NSRectFill(NSMakeRect(cx - 3 * u, ground, 6 * u, 7 * u)); NSRectFill(NSMakeRect(cx - 9 * u, ground, 18 * u, 3.4 * u));
    for (int k = -4; k <= 4; k++) NSRectFill(NSMakeRect(cx + k * 2 * u - 0.5 * u, ground + 3.4 * u, u, 0.9 * u));
    for (int k = -1; k <= 1; k++) NSRectFill(NSMakeRect(cx + k * 2 * u - 0.5 * u, ground + 7 * u, u, 0.9 * u));
}
static void th_skyline(CGFloat w, CGFloat ground, int seed, NSColor *c, NSColor *lit)
{
    CGFloat x = -10;
    for (int k = 0; x < w; k++) {
        CGFloat bw = 26 + 46 * th_hash(k, seed), bh = 40 + 170 * th_hash(k * 3, seed + 4);
        [c setFill]; NSRectFill(NSMakeRect(x, ground, bw, bh));
        if (lit) { [lit setFill]; for (CGFloat wy = ground + 8; wy < ground + bh - 6; wy += 9) for (CGFloat wx = x + 4; wx < x + bw - 4; wx += 7) if (th_hash((int)wx * 7, (int)wy * 11) > 0.84f) NSRectFill(NSMakeRect(wx, wy, 3, 4)); }
        x += bw + 2;
    }
}
/* an original scene for each game, in points; store art replaces it when it can be had */
static NSImage *th_scene(ThStyle st, CGFloat W, CGFloat H, int scale)
{
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:(NSInteger)W * scale pixelsHigh:(NSInteger)H * scale bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSCalibratedRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
    rep.size = NSMakeSize(W, H);
    [NSGraphicsContext saveGraphicsState]; NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
    if (st == ThStone) {
        [[[NSGradient alloc] initWithColorsAndLocations:th_rgb(0.03, 0.01, 0.015), 0.0, th_rgb(0.20, 0.03, 0.03), 0.55, th_rgb(0.62, 0.14, 0.04), 1.0, nil] drawInRect:NSMakeRect(0, 0, W, H) angle:-90];
        th_glow(W * 0.5, H * 0.26, W * 0.62, th_rgba(1.0, 0.42, 0.08, 0.75)); th_glow(W * 0.2, H * 0.12, W * 0.36, th_rgba(0.9, 0.18, 0.04, 0.5));
        th_ridge(W, H * 0.16, H * 0.16, 170, 2, th_rgb(0.10, 0.02, 0.02)); th_cathedral(W * 0.5, H * 0.16, W / 62, th_rgb(0.02, 0.008, 0.012));
        th_ridge(W, 0, H * 0.20, 120, 5, th_rgb(0.012, 0.006, 0.008)); th_embers(W, H * 0.8, 240, 4, th_rgba(1, 0.62, 0.2, 0.85));
    } else if (st == ThIron) {
        [[[NSGradient alloc] initWithColorsAndLocations:th_rgb(0.03, 0.05, 0.08), 0.0, th_rgb(0.14, 0.19, 0.20), 0.5, th_rgb(0.72, 0.46, 0.18), 1.0, nil] drawInRect:NSMakeRect(0, 0, W, H) angle:-90];
        th_glow(W * 0.66, H * 0.30, W * 0.5, th_rgba(1.0, 0.70, 0.28, 0.75));
        th_ridge(W, H * 0.30, H * 0.20, 200, 3, th_rgb(0.20, 0.20, 0.17)); th_keep(W * 0.28, H * 0.42, W / 92, th_rgb(0.06, 0.07, 0.07));
        th_ridge(W, H * 0.18, H * 0.20, 90, 7, th_rgb(0.08, 0.11, 0.08)); th_ridge(W, 0, H * 0.22, 46, 11, th_rgb(0.02, 0.03, 0.025)); th_embers(W, H * 0.5, 70, 8, th_rgba(1, 0.8, 0.45, 0.7));
    } else if (st == ThBronze) {
        [[[NSGradient alloc] initWithColorsAndLocations:th_rgb(0.05, 0.005, 0.01), 0.0, th_rgb(0.40, 0.03, 0.03), 0.55, th_rgb(0.96, 0.30, 0.04), 1.0, nil] drawInRect:NSMakeRect(0, 0, W, H) angle:-90];
        for (int k = 0; k < 7; k++) th_glow(W * (0.08 + 0.14 * k), H * (0.22 + 0.2 * th_hash(k, 5)), W * (0.14 + 0.1 * th_hash(k, 6)), th_rgba(1, 0.5 + 0.2 * th_hash(k, 7), 0.06, 0.55));
        th_ridge(W, H * 0.22, H * 0.34, 62, 13, th_rgb(0.12, 0.012, 0.012)); th_ridge(W, H * 0.06, H * 0.26, 38, 17, th_rgb(0.03, 0.005, 0.008)); th_embers(W, H, 300, 9, th_rgba(1, 0.68, 0.22, 0.9));
    } else {
        [[[NSGradient alloc] initWithColorsAndLocations:th_rgb(0.012, 0.03, 0.05), 0.0, th_rgb(0.03, 0.14, 0.17), 0.65, th_rgb(0.09, 0.34, 0.36), 1.0, nil] drawInRect:NSMakeRect(0, 0, W, H) angle:-90];
        th_glow(W * 0.72, H * 0.34, W * 0.44, th_rgba(0.2, 0.86, 0.82, 0.42));
        th_skyline(W, H * 0.13, 21, th_rgb(0.05, 0.13, 0.16), th_rgba(0.45, 0.85, 0.82, 0.32)); th_skyline(W, 0, 33, th_rgb(0.012, 0.035, 0.05), th_rgba(0.9, 0.72, 0.3, 0.4));
        th_glow(W * 0.16, H * 0.46, W * 0.14, th_rgba(0.9, 0.12, 0.08, 0.26));
    }
    [NSGraphicsContext restoreGraphicsState];
    NSImage *im = [[NSImage alloc] initWithSize:NSMakeSize(W, H)]; [im addRepresentation:rep]; return im;
}

/* ---- frosted glass ---- */
static void th_cover(NSImage *img, NSRect r, CGFloat alpha)
{
    NSSize s = img.size; if (s.width <= 0 || s.height <= 0) return;
    CGFloat k = MAX(r.size.width / s.width, r.size.height / s.height), w = r.size.width / k, h = r.size.height / k;
    [img drawInRect:r fromRect:NSMakeRect((s.width - w) / 2, (s.height - h) / 2, w, h) operation:NSCompositingOperationSourceOver fraction:alpha respectFlipped:YES hints:nil];
}
/* a blurred, slightly darker copy */
static NSImage *th_frost(NSImage *img, CGFloat soft)
{
    CGImageRef cg = [img CGImageForProposedRect:NULL context:nil hints:nil]; if (!cg) return nil;
    CIImage *in = [CIImage imageWithCGImage:cg]; CGRect ext = in.extent;
    CIImage *b = [[in imageByClampingToExtent] imageByApplyingGaussianBlurWithSigma:MAX(ext.size.width, ext.size.height) / soft];
    static CIContext *cx; if (!cx) cx = [CIContext contextWithOptions:nil];
    /* brighter pictures come down further, so light art stays as readable behind the glass as dark art */
    CIImage *avg = [in imageByApplyingFilter:@"CIAreaAverage" withInputParameters:@{kCIInputExtentKey: [CIVector vectorWithCGRect:ext]}];
    uint8_t px[4] = {0}; [cx render:avg toBitmap:px rowBytes:4 bounds:CGRectMake(0, 0, 1, 1) format:kCIFormatRGBA8 colorSpace:nil];
    double luma = (0.2126 * px[0] + 0.7152 * px[1] + 0.0722 * px[2]) / 255, dim = -0.06 - MAX(0.0, luma - 0.16) * 0.95;
    b = [[b imageByApplyingFilter:@"CIColorControls" withInputParameters:@{@"inputSaturation": @1.15, @"inputBrightness": @(dim)}] imageByCroppingToRect:ext];
    CGImageRef out = [cx createCGImage:b fromRect:ext]; if (!out) return nil;
    NSImage *o = [[NSImage alloc] initWithCGImage:out size:img.size]; CGImageRelease(out); return o;
}
/* the window behind the glass: the soft picture, darkened toward the foot, a vignette, a bloom, a grain of scan lines */
static void th_stage(ThStyle st, NSRect b, NSImage *soft, NSPoint bloom)
{
    [th_rgb(0.035, 0.03, 0.035) setFill]; NSRectFill(b);
    th_cover(soft, b, 1);
    [[[NSGradient alloc] initWithColorsAndLocations:th_rgba(0, 0, 0, 0.42), 0.0, th_rgba(0, 0, 0, 0.10), 0.40, th_rgba(0, 0, 0, 0.62), 1.0, nil] drawInRect:b angle:-90];
    [[[NSGradient alloc] initWithStartingColor:th_rgba(0, 0, 0, 0) endingColor:th_rgba(0, 0, 0, 0.34)] drawInRect:b relativeCenterPosition:NSZeroPoint];
    th_glow(bloom.x, bloom.y, 430, th_accent(st, 0.30));
    [th_rgba(1, 1, 1, 0.018) setFill];
    for (CGFloat y = 0; y < b.size.height; y += 3) NSRectFillUsingOperation(NSMakeRect(0, y, b.size.width, 1), NSCompositingOperationSourceOver);
}
/* the blurred picture seen through r, smoked, with a light tint, a top highlight and a fine edge. `full`: the window, in this view's coordinates */
static void th_glass(NSRect r, CGFloat rad, NSColor *tint, NSColor *edge, CGFloat ew, NSImage *blurred, NSRect full)
{
    NSBezierPath *p = [NSBezierPath bezierPathWithRoundedRect:r xRadius:rad yRadius:rad];
    [NSGraphicsContext saveGraphicsState]; [p addClip];
    if (blurred) th_cover(blurred, full, 1); else { [th_rgb(0.08, 0.07, 0.08) setFill]; NSRectFill(r); }
    [th_rgba(0.02, 0.015, 0.02, 0.36) setFill]; NSRectFillUsingOperation(r, NSCompositingOperationSourceOver);
    [tint ?: th_rgba(1, 1, 1, 0.07) setFill]; NSRectFillUsingOperation(r, NSCompositingOperationSourceOver);
    [[[NSGradient alloc] initWithStartingColor:th_rgba(1, 1, 1, 0.12) endingColor:th_rgba(1, 1, 1, 0)] drawInRect:NSMakeRect(r.origin.x, NSMaxY(r) - MIN(40, r.size.height), r.size.width, MIN(40, r.size.height)) angle:-90];
    [NSGraphicsContext restoreGraphicsState];
    NSBezierPath *e = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(r, ew / 2, ew / 2) xRadius:rad yRadius:rad]; e.lineWidth = ew;
    [edge ?: th_rgba(1, 1, 1, 0.16) setStroke]; [e stroke];
}
/* the game's name over a spaced line and a short rule */
static void th_title(ThStyle st, NSString *t, NSString *sub, NSRect r, CGFloat size)
{
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new]; ps.alignment = NSTextAlignmentCenter;
    NSShadow *sh = [NSShadow new]; sh.shadowColor = th_rgba(0, 0, 0, 0.8); sh.shadowBlurRadius = size / 3.4; sh.shadowOffset = NSMakeSize(0, -1);
    NSDictionary *at = @{NSFontAttributeName: th_title_font(st, size), NSForegroundColorAttributeName: th_text(st), NSParagraphStyleAttributeName: ps, NSShadowAttributeName: sh, NSKernAttributeName: @(st == ThTactical ? 0.5 : size / 22)};
    CGFloat h = ceil([t sizeWithAttributes:at].height);
    [t drawInRect:NSMakeRect(r.origin.x, NSMaxY(r) - h, r.size.width, h + 2) withAttributes:at];
    if (sub.length) {
        NSDictionary *as = @{NSFontAttributeName: th_font(st, size * 0.33, YES), NSForegroundColorAttributeName: th_accent(st, 1), NSParagraphStyleAttributeName: ps, NSKernAttributeName: @(size * 0.13)};
        [sub drawInRect:NSMakeRect(r.origin.x, NSMaxY(r) - h - size * 0.58, r.size.width, size * 0.5) withAttributes:as];
        [th_accent(st, 1) setFill]; NSRectFill(NSMakeRect(NSMidX(r) - 62, NSMaxY(r) - h - size * 0.76, 124, 2));
    }
}
static void th_button(ThStyle st, NSRect r, NSString *title, BOOL primary, BOOL hover, BOOL down, NSImage *blurred, NSRect full)
{
    if (primary) {
        NSBezierPath *p = [NSBezierPath bezierPathWithRoundedRect:r xRadius:9 yRadius:9];
        CGFloat a = hover ? 1 : 0.92; [NSGraphicsContext saveGraphicsState]; [p addClip];
        [th_accent(st, 1) setFill]; NSRectFill(r);
        [[[NSGradient alloc] initWithColorsAndLocations:th_rgba(1, 1, 1, hover ? 0.22 : 0.15), 0.0, th_rgba(0, 0, 0, 0), 0.45, th_rgba(0, 0, 0, 1 - a + 0.20), 1.0, nil] drawInRect:r angle:-90];
        [NSGraphicsContext restoreGraphicsState];
    } else th_glass(r, 9, th_rgba(1, 1, 1, hover ? 0.11 : 0.06), hover ? th_accent(st, 0.85) : nil, 1, blurred, full);
    NSShadow *ts = [NSShadow new]; ts.shadowColor = th_rgba(0, 0, 0, 0.75); ts.shadowOffset = NSMakeSize(0, -1); ts.shadowBlurRadius = 2;
    CGFloat fs = primary ? 21 : 17, room = r.size.width - 24;
    NSDictionary *at; NSSize sz;
    for (;;) { at = @{NSFontAttributeName: th_font(st, fs, YES), NSForegroundColorAttributeName: th_text(st), NSShadowAttributeName: ts, NSKernAttributeName: @0.6}; sz = [title sizeWithAttributes:at]; if (sz.width <= room || fs <= 11) break; fs -= 0.5; }
    [title drawAtPoint:NSMakePoint(NSMidX(r) - sz.width / 2, NSMidY(r) - sz.height / 2 + (down ? -1 : 0)) withAttributes:at];
}
