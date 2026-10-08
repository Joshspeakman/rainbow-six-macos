"""Read the launcher's original menu artwork from the user's Objects.ds2res."""
import mmap
import struct
import zlib

PREFIX = 'art/bitmaps/gui/front_end/main/b_gui_fe_mn_'
NAMES = [f'back{i:02}.dds' for i in range(1, 9)] + ['logo.dds', 'menubar-up.dds', 'menubar-over.dds', 'menubar-down.dds']

def read_art(path):
    result = {}
    try:
        with open(path, 'rb') as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
            def u32(at): return struct.unpack_from('<I', data, at)[0]
            def u16(at): return struct.unpack_from('<H', data, at)[0]
            def read(at, count):
                if at < 0 or count < 0 or at + count > len(data):
                    raise ValueError('truncated tank')
                return data[at:at + count]
            if read(0, 8) not in (b'DSigTank', b'DSg2Tank'): return result
            dirs_at, files_at, payload_at = u32(12), u32(16), u32(24)
            dirs = {}
            for i in range(u32(dirs_at)):
                off = u32(dirs_at + 4 + 4 * i)
                at = dirs_at + off
                dirs[off] = (u32(at), read(at + 18, u16(at + 16)).decode('latin1'))
            def directory(off):
                parts = []
                for _ in range(64):
                    parent, name = dirs.get(off, (off, ''))
                    if not name: break
                    parts.insert(0, name)
                    if parent == off: break
                    off = parent
                return '/'.join(parts)
            wanted = {PREFIX + n: n for n in NAMES}
            for i in range(u32(files_at)):
                at = files_at + u32(files_at + 4 + 4 * i)
                parent, size, offset = u32(at), u32(at + 4), u32(at + 8)
                fmt, length = u16(at + 24), u16(at + 28)
                name = directory(parent) + '/' + read(at + 30, length).decode('latin1')
                if name.lower() not in wanted: continue
                if size > 32 * 1024 * 1024: raise ValueError('oversized launcher texture')
                if fmt == 0:
                    raw = read(payload_at + offset, size)
                else:
                    table = (at + 30 + length + 4) & ~3
                    chunk = u32(table + 4)
                    if not chunk: raise ValueError('zero chunk size')
                    chunks = []
                    for c in range((size + chunk - 1) // chunk):
                        row = table + 8 + 16 * c
                        usz, csz, extra, coff = (u32(row + j) for j in (0, 4, 8, 12))
                        start = payload_at + offset + coff
                        block = read(start, csz)
                        chunks.append(zlib.decompress(block) if csz < usz else block[:usz])
                        chunks.append(read(start + csz, extra))
                    raw = b''.join(chunks)[:size]
                if raw[:4] == b'DDS ': result[wanted[name.lower()]] = raw
    except (OSError, ValueError, struct.error, zlib.error) as error:
        print('Launcher artwork:', error)
    return result
