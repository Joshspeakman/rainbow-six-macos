#!/usr/bin/env python3
"""dev/launcher_release.py <launcher.m>: the Updates row reads a release's tag (the downloaded app's updater), as diablo-2-macos 0a543d4"""
import re, sys
p = sys.argv[1]; s = open(p).read()
o = "    return t.terminationStatus == 0 && out.length >= 40 ? [out substringToIndex:40] : nil;"
assert s.count(o) == 1, "commit return"
s = s.replace(o, """    if (t.terminationStatus != 0 || !out.length) return nil;
    if (out.length >= 40) return [out substringToIndex:40];   /* a commit (an app built from source) */
    return [out rangeOfCharacterFromSet:NSCharacterSet.whitespaceAndNewlineCharacterSet].location == NSNotFound ? out : nil;   /* a release's tag (the downloaded app) */""")
o = '    if ([line hasPrefix:@"== fetching"]) return @"Downloading the update from GitHub";'
assert s.count(o) == 1, "fetching"
s = s.replace(o, """    if ([branch isEqualToString:@"release"]) {   /* the downloaded app's updater (setup/update.sh) */
        if ([line hasPrefix:@"== downloading "]) return [NSString stringWithFormat:@"Downloading %@ from GitHub", [line substringFromIndex:15]];
        if ([line hasPrefix:@"== unpacking"]) return @"Unpacking it";
        if ([line hasPrefix:@"== installing"]) return @"Installing the new app";
        return nil;
    }
""" + o)
m = re.search(r'( +)a\.informativeText = (\[NSString stringWithFormat:@"The %@ branch \(%@\) is downloaded.*?short_commit\(latest\)\]);', s, re.S)
if not m: print("no informativeText form: edit the update alert by hand")
else:
  ind = m.group(1)
  s = s[:m.start()] + ind + 'a.informativeText = [branch isEqualToString:@"release"] ? [NSString stringWithFormat:@"%@ is downloaded from GitHub and replaces this app. Your game, saves and settings are kept; if the game\'s code changed, it is rebuilt from your copy when the app starts again.", latest]\n' + ind + '                  : ' + m.group(2) + ';' + s[m.end():]
open(p, 'w').write(s); print("patched", p)
