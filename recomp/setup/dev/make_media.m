// make_media: a port's repository art, ours only (no publisher art), in the launchers' look (../theme.h), named for scrapers
// (dotFound reads media/boxart.png as the cover, media/hero.jpg as the banner, media/logo.png, media/icon.png):
//   make_media boxart <style> <title> <subtitle> <shot[,shot]> <out.png>  2:3 portrait, 1000 x 1500: one or two frames over the title
//   make_media hero   <style> <title> <subtitle> <screenshot> <out.jpg>   1920 x 620
//   make_media logo   <style> <title> <subtitle> <out.png>                transparent, trimmed to the type
// clang -fobjc-arc -framework AppKit -framework CoreImage -I .. make_media.m -o make_media
#import "theme.h"
static NSBitmapImageRep *canvas(int w, int h) { return [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:w pixelsHigh:h bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0 bitsPerPixel:0]; }
static int save(NSBitmapImageRep *r, NSString *p)
{
    BOOL jpg = [p.pathExtension.lowercaseString hasPrefix:@"jp"];
    NSData *d = jpg ? [r representationUsingType:NSBitmapImageFileTypeJPEG properties:@{NSImageCompressionFactor: @0.9}] : [r representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    return [d writeToFile:p atomically:YES] ? 0 : 1;
}
/* the title, as large as fits `w`, over a spaced subtitle and a rule; returns the height it took */
static BOOL logo_edge;   /* the logo: a dark edge under the type, so it reads on a light page too */
static CGFloat title_block(ThStyle st, NSString *t, NSString *sub, CGFloat cx, CGFloat top, CGFloat w, CGFloat max)
{
    CGFloat size = max; NSDictionary *at;
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new]; ps.alignment = NSTextAlignmentCenter;
    NSShadow *sh = [NSShadow new]; sh.shadowColor = th_rgba(0, 0, 0, 0.85);
    for (;; size -= 2) {
        sh.shadowBlurRadius = size / 3.4; sh.shadowOffset = NSMakeSize(0, -size / 30);
        at = @{NSFontAttributeName: th_title_font(st, size), NSForegroundColorAttributeName: th_text(st), NSParagraphStyleAttributeName: ps,
               NSShadowAttributeName: sh, NSKernAttributeName: @(st == ThTactical ? size * 0.04 : size * 0.06)};
        if ([t sizeWithAttributes:at].width <= w || size < 18) break;
    }
    CGFloat h = ceil([t sizeWithAttributes:at].height), y = top - h;
    if (logo_edge) {
        NSMutableDictionary *o = [at mutableCopy]; o[NSStrokeColorAttributeName] = th_rgba(0.04, 0.03, 0.04, 0.95); o[NSStrokeWidthAttributeName] = @(9);
        o[NSForegroundColorAttributeName] = th_rgba(0.04, 0.03, 0.04, 0.95);
        [t drawInRect:NSMakeRect(cx - w / 2, y, w, h + 2) withAttributes:o];
    }
    [t drawInRect:NSMakeRect(cx - w / 2, y, w, h + 2) withAttributes:at];
    if (sub.length) {
        CGFloat ss = MAX(14, size * 0.24);
        NSDictionary *as = @{NSFontAttributeName: th_font(st, ss, YES), NSForegroundColorAttributeName: th_accent(st, 1), NSParagraphStyleAttributeName: ps,
                             NSKernAttributeName: @(ss * 0.35), NSShadowAttributeName: sh};
        while ([sub sizeWithAttributes:as].width > w && ss > 10) { ss -= 1; as = @{NSFontAttributeName: th_font(st, ss, YES), NSForegroundColorAttributeName: th_accent(st, 1), NSParagraphStyleAttributeName: ps, NSKernAttributeName: @(ss * 0.35), NSShadowAttributeName: sh}; }
        y -= ss * 1.9;
        if (logo_edge) { NSMutableDictionary *o = [as mutableCopy]; o[NSStrokeColorAttributeName] = th_rgba(0.04, 0.03, 0.04, 0.95); o[NSStrokeWidthAttributeName] = @(12);
                       [sub drawInRect:NSMakeRect(cx - w / 2, y, w, ss * 1.5) withAttributes:o]; }
        [sub drawInRect:NSMakeRect(cx - w / 2, y, w, ss * 1.5) withAttributes:as];
        [th_accent(st, 1) setFill]; NSRectFill(NSMakeRect(cx - size * 1.1, y - ss * 0.55, size * 2.2, MAX(2, size / 40)));
        y -= ss * 0.6;
    }
    return top - y;
}
static void strap(ThStyle st, NSString *s, NSRect r, CGFloat size)
{
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new]; ps.alignment = NSTextAlignmentCenter;
    [s drawInRect:r withAttributes:@{NSFontAttributeName: th_font(st, size, YES), NSForegroundColorAttributeName: th_text(st), NSParagraphStyleAttributeName: ps, NSKernAttributeName: @(size * 0.3)}];
}
int main(int c, char **v) { @autoreleasepool {
    [NSApplication sharedApplication];
    if (c < 6) { fprintf(stderr, "usage: make_media boxart|hero <style> <title> <subtitle> <screenshot> <out> | logo <style> <title> <subtitle> <out>\n"); return 2; }
    NSString *kind = @(v[1]), *t = @(v[3]), *sub = @(v[4]); ThStyle st = th_style(@(v[2]));
    if ([kind isEqual:@"logo"]) {
        int W = 2000, H = 700; NSBitmapImageRep *rep = canvas(W, H);
        [NSGraphicsContext saveGraphicsState]; NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
        logo_edge = YES; title_block(st, t, sub, W / 2, H - 120, W - 160, 300); logo_edge = NO;
        [NSGraphicsContext restoreGraphicsState];
        /* trimmed to what was drawn, with a margin */
        int x0 = W, y0 = H, x1 = 0, y1 = 0;
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { NSUInteger p[4]; [rep getPixel:p atX:x y:y]; if (p[3] > 8) { x0 = MIN(x0, x); x1 = MAX(x1, x); y0 = MIN(y0, y); y1 = MAX(y1, y); } }
        int m = 24; x0 = MAX(0, x0 - m); y0 = MAX(0, y0 - m); x1 = MIN(W - 1, x1 + m); y1 = MIN(H - 1, y1 + m);
        NSBitmapImageRep *out = canvas(x1 - x0 + 1, y1 - y0 + 1);
        [NSGraphicsContext saveGraphicsState]; NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:out];
        NSImage *img = [[NSImage alloc] initWithSize:NSMakeSize(W, H)]; [img addRepresentation:rep];
        [img drawInRect:NSMakeRect(0, 0, out.pixelsWide, out.pixelsHigh) fromRect:NSMakeRect(x0, H - 1 - y1, out.pixelsWide, out.pixelsHigh) operation:NSCompositingOperationCopy fraction:1];
        [NSGraphicsContext restoreGraphicsState];
        return save(out, @(v[5]));
    }
    if (c < 7) return 2;
    /* the screenshots: one, or two for the cover ("a.png,b.png") */
    NSMutableArray<NSImage *> *shots = [NSMutableArray array];
    for (NSString *f in [@(v[5]) componentsSeparatedByString:@","]) {
        NSBitmapImageRep *r = (NSBitmapImageRep *)[NSBitmapImageRep imageRepWithContentsOfFile:f];
        if (!r) { fprintf(stderr, "no picture: %s\n", f.UTF8String); return 1; }
        NSImage *i = [[NSImage alloc] initWithSize:NSMakeSize(r.pixelsWide, r.pixelsHigh)]; [i addRepresentation:r]; [shots addObject:i];
    }
    BOOL box = [kind isEqual:@"boxart"];
    int W = box ? 1000 : 1920, H = box ? 1500 : 620; NSBitmapImageRep *rep = canvas(W, H); NSRect b = NSMakeRect(0, 0, W, H);
    [NSGraphicsContext saveGraphicsState]; NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
    NSGraphicsContext.currentContext.imageInterpolation = NSImageInterpolationHigh;
    /* behind: the first frame frosted over the whole picture; on it the frames themselves, sharp */
    NSImage *soft = th_frost(shots[0], box ? 90 : 60), *blur = th_frost(shots[0], 40);
    [th_rgb(0.03, 0.03, 0.035) setFill]; NSRectFill(b); th_cover(soft, b, 1);
    [[[NSGradient alloc] initWithColorsAndLocations:th_rgba(0, 0, 0, 0.5), 0.0, th_rgba(0, 0, 0, 0.2), 0.5, th_rgba(0, 0, 0, 0.55), 1.0, nil] drawInRect:b angle:-90];
    void (^frame)(NSImage *, NSRect) = ^(NSImage *img, NSRect pic) {
        NSShadow *ds = [NSShadow new]; ds.shadowColor = th_rgba(0, 0, 0, 0.75); ds.shadowBlurRadius = 30; ds.shadowOffset = NSMakeSize(0, -8);
        [NSGraphicsContext saveGraphicsState]; [ds set]; [th_rgb(0, 0, 0) setFill]; [[NSBezierPath bezierPathWithRoundedRect:pic xRadius:10 yRadius:10] fill]; [NSGraphicsContext restoreGraphicsState];
        [NSGraphicsContext saveGraphicsState]; [[NSBezierPath bezierPathWithRoundedRect:pic xRadius:10 yRadius:10] addClip];
        th_cover(img, pic, 1); [NSGraphicsContext restoreGraphicsState];
        NSBezierPath *edge = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(pic, 1, 1) xRadius:10 yRadius:10]; edge.lineWidth = 2; [th_accent(st, 0.9) setStroke]; [edge stroke];
    };
    if (box) {
        strap(st, @"NATIVE FOR APPLE SILICON", NSMakeRect(0, H - 92, W, 40), 26);
        [th_accent(st, 1) setFill]; NSRectFill(NSMakeRect(W / 2 - 100, H - 104, 200, 3));
        NSBitmapImageRep *probe = canvas(W, H); [NSGraphicsContext saveGraphicsState];
        NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:probe];
        CGFloat used = title_block(st, t, sub, W / 2, H, W - 190, 150); [NSGraphicsContext restoreGraphicsState];
        CGFloat ph = used + 110, py = 70;                                     /* the title panel at the foot */
        CGFloat top = H - 140, bottom = py + ph + 40, gap = 30, n = shots.count, fh = (top - bottom - gap * (n - 1)) / n, fw = W - 120;
        for (NSUInteger k = 0; k < shots.count; k++) {                         /* the frames, filling the space above it */
            NSSize sz = shots[k].size; CGFloat w = fw, h = w * sz.height / sz.width;
            if (h > fh) { h = fh; w = MIN(fw, h * sz.width / sz.height * (n > 1 ? 1.25 : 1)); }   /* two: cropped a little wider */
            frame(shots[k], NSMakeRect((W - w) / 2, top - (k + 1) * fh - k * gap + (fh - h) / 2, w, h));
        }
        th_glass(NSMakeRect(60, py, W - 120, ph), 26, th_rgba(0.04, 0.035, 0.04, 0.30), th_accent(st, 0.85), 2.5, blur, b);
        title_block(st, t, sub, W / 2, py + ph - 30, W - 190, 150);
        strap(st, @"RECOMPILED FROM YOUR OWN COPY", NSMakeRect(0, py + 22, W, 30), 17);
    } else {
        NSSize sz = shots[0].size; CGFloat fh = H - 110, fw = fh * sz.width / sz.height; if (fw > 1040) { fw = 1040; }
        NSRect pic = NSMakeRect(W - 70 - fw, (H - fh) / 2, fw, fh); frame(shots[0], pic);
        CGFloat lw = NSMinX(pic) - 70 - 40, used;
        NSBitmapImageRep *probe = canvas(W, H); [NSGraphicsContext saveGraphicsState];
        NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:probe];
        used = title_block(st, t, sub, 70 + lw / 2, H, lw, 110); [NSGraphicsContext restoreGraphicsState];
        title_block(st, t, sub, 70 + lw / 2, H / 2 + used / 2 + 20, lw, 110);
        strap(st, @"NATIVE FOR APPLE SILICON", NSMakeRect(70, H / 2 - used / 2 - 30, lw, 30), 18);
    }
    [NSGraphicsContext restoreGraphicsState];
    return save(rep, @(v[6]));
} }
