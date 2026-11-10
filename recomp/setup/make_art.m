// make_art: the app icon and disk image background, in the installer's frosted look (theme.h).
//   make_art icon <style> <letters> <out.png>
//   make_art dmg <style> <title> <out.png> <scale>
#import "theme.h"
static NSBitmapImageRep *canvas(int w, int h) { return [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:w pixelsHigh:h bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSCalibratedRGBColorSpace bytesPerRow:0 bitsPerPixel:0]; }
static int save(NSBitmapImageRep *r, const char *p) { return [[r representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(p) atomically:YES] ? 0 : 1; }
int main(int c, char **v) { @autoreleasepool {
    [NSApplication sharedApplication];
    ThStyle st = th_style(@(v[2])); NSString *t = @(v[3]);
    if (!strcmp(v[1], "icon")) {                                   /* macOS's icon grid: 824 of 1024, corners 186 */
        NSBitmapImageRep *rep = canvas(1024, 1024); [NSGraphicsContext saveGraphicsState]; NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
        NSRect b = NSMakeRect(100, 100, 824, 824); NSBezierPath *clip = [NSBezierPath bezierPathWithRoundedRect:b xRadius:186 yRadius:186];
        NSImage *scene = th_scene(st, 824, 824, 1), *blur = th_frost(scene, 40);
        [NSGraphicsContext saveGraphicsState]; [clip addClip]; th_cover(scene, b, 1);
        [[[NSGradient alloc] initWithStartingColor:th_rgba(0, 0, 0, 0.15) endingColor:th_rgba(0, 0, 0, 0.6)] drawInRect:b angle:-90];
        th_glass(NSInsetRect(b, 120, 240), 46, th_rgba(1, 1, 1, 0.08), th_rgba(1, 1, 1, 0.3), 5, blur, b);
        [NSGraphicsContext restoreGraphicsState];
        NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new]; ps.alignment = NSTextAlignmentCenter; NSShadow *sh = [NSShadow new]; sh.shadowColor = th_rgba(0, 0, 0, 0.8); sh.shadowBlurRadius = 24;
        NSDictionary *at = @{NSFontAttributeName: th_title_font(st, t.length > 2 ? 230 : 290), NSForegroundColorAttributeName: th_text(st), NSParagraphStyleAttributeName: ps, NSShadowAttributeName: sh};
        CGFloat h = [t sizeWithAttributes:at].height; [t drawInRect:NSMakeRect(100, 512 - h / 2, 824, h + 4) withAttributes:at];
        [NSGraphicsContext restoreGraphicsState]; return save(rep, v[4]);
    }
    int sc = atoi(v[5]), W = 660, H = 420; NSBitmapImageRep *rep = canvas(W * sc, H * sc); rep.size = NSMakeSize(W, H);
    [NSGraphicsContext saveGraphicsState]; NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
    NSRect b = NSMakeRect(0, 0, W, H); NSImage *scene = th_scene(st, W, H, sc), *soft = th_frost(scene, 320), *blur = th_frost(scene, 70);
    th_stage(st, b, soft, NSMakePoint(W / 2, 200));
    th_title(st, t, @"NATIVE EDITION", NSMakeRect(0, 310, W, 92), 38);
    th_glass(NSMakeRect(60, 92, W - 120, 200), 14, th_rgba(0.05, 0.04, 0.05, 0.22), th_accent(st, 0.8), 1.5, blur, b);
    NSBezierPath *a = [NSBezierPath bezierPath]; CGFloat y = 190, x0 = 282, x1 = 378; a.lineWidth = 6; a.lineCapStyle = NSLineCapStyleRound; a.lineJoinStyle = NSLineJoinStyleRound;
    [a moveToPoint:NSMakePoint(x0, y)]; [a lineToPoint:NSMakePoint(x1, y)]; [a moveToPoint:NSMakePoint(x1 - 22, y + 20)]; [a lineToPoint:NSMakePoint(x1, y)]; [a lineToPoint:NSMakePoint(x1 - 22, y - 20)];
    [th_accent(st, 1) setStroke]; [a stroke];
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new]; ps.alignment = NSTextAlignmentCenter;
    NSDictionary *at = @{NSFontAttributeName: th_font(st, 14, NO), NSForegroundColorAttributeName: th_text(st), NSParagraphStyleAttributeName: ps};
    [@"Drag to Applications, then open it and drop your game folder on it." drawInRect:NSMakeRect(40, 56, W - 80, 22) withAttributes:at];
    [NSGraphicsContext restoreGraphicsState]; return save(rep, v[4]);
} }
