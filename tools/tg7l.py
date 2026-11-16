"""7th Level "TopGun" resource files (7L ... .BIN, with data in .000 for the later version).
Layout after github.com/Helco/TopGun (ResourceFile.cs)."""
import os
import struct

TYPES = ['?', 'Bitmap', 'Data', 'File', 'Frame', 'Ground', 'Midi', 'Model', 'MProto', 'Obj3D', 'OProto', 'Table',
         'Wave', 'Movie', 'Array', 'Cell', 'Group', 'Palette', 'Queue', 'Script', 'Sprite', 'Text', 'Tile', 'Title',
         'Subtitle', 'Local', 'Entry']
KEYS = ['Resources', 'Entries', 'IndexBuffers', 'Variables', 'ConstStrings', 'Scripts', 'Palette', 'NameTable',
        'Unknown8', 'Unknown9', 'Plugins', 'PluginProcs', 'PluginIndexPerProc', 'Unknown13', 'SourceFile']


class ResFile:
    def __init__(self, path):
        self.path = path
        d = self.head = open(path, 'rb').read()
        magic, self.hsize, self.arch = struct.unpack_from('<HHH', d, 0)
        if magic != 0x4c37:
            raise ValueError('not a 7L file: ' + path)
        self.title = d[7:0x56].split(b'\0')[0].decode('latin1')
        self.version = struct.unpack_from('<H', d, 0x56)[0]
        o = 0x58
        if self.arch == 0x3631:      # 16 bit
            n = 48 if self.hsize == 0x1a2 else 32      # (the later 16-bit version: more script sections)
            self.scriptEnds = struct.unpack_from('<%dI' % n, d, o + 10)
            p = o + 10 + 4 * n
            self.scriptCount, _, _, _, self.nStrings, self.nVars = struct.unpack_from('<6H', d, p)
            p += 12 + 4
            nkeys = 14
        elif self.arch == 0x3233:    # 32 bit
            self.scriptEnds = struct.unpack_from('<48I', d, o + 4)
            p = o + 4 + 192
            self.scriptCount = struct.unpack_from('<I', d, p)[0]
            p += 7 * 4 + 32 + 12
            nkeys = 15
        else:
            raise ValueError('architecture %x' % self.arch)
        self.keys = {KEYS[i]: struct.unpack_from('<II', d, p + 8 * i) for i in range(nkeys)}
        off, size = self.keys['Resources']
        self.res = [struct.unpack_from('<BBII', d, off + 10 * i) for i in range(size // 10)]

    def key(self, name):
        off, size = self.keys.get(name, (0, 0))
        return self.head[off:off + size]

    def datapath(self, ext):
        base = os.path.splitext(self.path)[0]
        for p in ('%s.%03d' % (base, ext), '%s.%03d' % (base.lower(), ext)):
            if os.path.exists(p):
                return p
        return self.path

    def read(self, i):
        t, ext, off, size = self.res[i]
        if 13 <= t <= 22:            # in the script sections
            so = self.keys['Scripts'][0]
            return self.head[so + off:so + off + size]
        with open(self.datapath(ext), 'rb') as f:
            f.seek(off)
            return f.read(size)


if __name__ == '__main__':
    import collections
    import sys
    for p in sys.argv[1:]:
        r = ResFile(p)
        c = collections.Counter(TYPES[t] if t < len(TYPES) else t for t, _, _, _ in r.res)
        print(os.path.basename(p), repr(r.title), 'v%d' % r.version, hex(r.arch), len(r.res), dict(c))
