#!/usr/bin/env python3
"""esm.py <plugin.esm> <TYPE> [substring] : form IDs and editor IDs of a plugin's records of one type (WEAP, AMMO, CREA,
NPC_, ...), optionally only those whose editor ID contains the substring. For console commands in test scenarios."""
import sys, struct, zlib
path, want, sub = sys.argv[1], sys.argv[2].encode(), (sys.argv[3].lower() if len(sys.argv) > 3 else '')
data = open(path, 'rb').read()
def walk(off, end):
    while off < end:
        typ, size = data[off:off + 4], struct.unpack_from('<I', data, off + 4)[0]
        if typ == b'GRUP': walk(off + 24, off + size); off += size; continue
        flags, fid = struct.unpack_from('<II', data, off + 8)
        if typ == want:
            body = data[off + 24:off + 24 + size]
            if flags & 0x40000: body = zlib.decompress(body[4:])
            p, edid = 0, ''
            while p + 6 <= len(body):
                st, sl = body[p:p + 4], struct.unpack_from('<H', body, p + 4)[0]
                if st == b'EDID': edid = body[p + 6:p + 6 + sl].rstrip(b'\0').decode('latin1'); break
                p += 6 + sl
            if sub in edid.lower(): print('%08X %s' % (fid, edid))
        off += 24 + size
walk(0, len(data))
