// nightly_icon <in.png> <out.png>: the Nightly app's icon (install.sh --nightly): the game's icon on a dark rounded tile
// with a "NIGHTLY" band, so it is told apart from the stable app at a glance. Build: clang -fobjc-arc -framework AppKit
#import <AppKit/AppKit.h>

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 3) { fprintf(stderr, "usage: nightly_icon <in.png> <out.png>\n"); return 2; }
        NSImage *src = [[NSImage alloc] initWithContentsOfFile:@(argv[1])];
        if (!src) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
        const CGFloat S = 1024;
        NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:(NSInteger)S pixelsHigh:(NSInteger)S
            bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
        [NSGraphicsContext saveGraphicsState];
        NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
        NSGraphicsContext.currentContext.imageInterpolation = NSImageInterpolationHigh;

        // the tile: macOS app-icon proportions (824 of 1024, corner radius ~185), night-sky gradient and a soft glow
        NSRect tile = NSMakeRect(100, 100, 824, 824);
        NSBezierPath *shape = [NSBezierPath bezierPathWithRoundedRect:tile xRadius:185 yRadius:185];
        NSShadow *drop = [NSShadow new]; drop.shadowColor = [NSColor colorWithWhite:0 alpha:0.45]; drop.shadowOffset = NSMakeSize(0, -10); drop.shadowBlurRadius = 24;
        [NSGraphicsContext saveGraphicsState]; [drop set]; [[NSColor blackColor] setFill]; [shape fill]; [NSGraphicsContext restoreGraphicsState];
        [NSGraphicsContext saveGraphicsState];
        [shape addClip];
        NSGradient *sky = [[NSGradient alloc] initWithColors:@[[NSColor colorWithSRGBRed:0.10 green:0.11 blue:0.24 alpha:1],
                                                                [NSColor colorWithSRGBRed:0.04 green:0.04 blue:0.10 alpha:1],
                                                                [NSColor colorWithSRGBRed:0.01 green:0.01 blue:0.03 alpha:1]]];
        [sky drawInRect:tile angle:-90];
        NSGradient *glow = [[NSGradient alloc] initWithStartingColor:[NSColor colorWithSRGBRed:0.45 green:0.40 blue:0.95 alpha:0.35]
                                                         endingColor:[NSColor colorWithSRGBRed:0.45 green:0.40 blue:0.95 alpha:0]];
        [glow drawInRect:NSMakeRect(212, 330, 600, 600) relativeCenterPosition:NSMakePoint(0, 0)];

        // the game's icon, slightly smaller, above the band
        [src drawInRect:NSMakeRect(222, 300, 580, 580) fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1];

        // the band
        NSRect band = NSMakeRect(100, 100, 824, 185);
        NSGradient *bandFill = [[NSGradient alloc] initWithStartingColor:[NSColor colorWithSRGBRed:0.36 green:0.20 blue:0.78 alpha:1]
                                                             endingColor:[NSColor colorWithSRGBRed:0.20 green:0.10 blue:0.52 alpha:1]];
        [bandFill drawInRect:band angle:-90];
        NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new]; ps.alignment = NSTextAlignmentCenter;
        NSShadow *ts = [NSShadow new]; ts.shadowColor = [NSColor colorWithWhite:0 alpha:0.6]; ts.shadowOffset = NSMakeSize(0, -3); ts.shadowBlurRadius = 6;
        NSDictionary *attrs = @{NSFontAttributeName: [NSFont systemFontOfSize:104 weight:NSFontWeightHeavy], NSForegroundColorAttributeName: NSColor.whiteColor,
                                NSParagraphStyleAttributeName: ps, NSKernAttributeName: @8, NSShadowAttributeName: ts};
        NSString *label = @"NIGHTLY"; NSSize ls = [label sizeWithAttributes:attrs];
        [label drawInRect:NSMakeRect(100, NSMidY(band) + 12 - ls.height / 2, 824, ls.height) withAttributes:attrs];   // a little above the rounded corners
        [NSGraphicsContext restoreGraphicsState];

        // a thin rim so the tile reads on dark backgrounds too
        [[NSColor colorWithSRGBRed:0.55 green:0.50 blue:1.0 alpha:0.35] setStroke]; shape.lineWidth = 6; [shape stroke];
        [NSGraphicsContext restoreGraphicsState];
        NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        if (![png writeToFile:@(argv[2]) atomically:YES]) { fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
    }
    return 0;
}
