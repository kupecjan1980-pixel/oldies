#!/usr/bin/env python3
"""Sound clips of a TopGun game (Ace Ventura) as WAV files.

    tgwave.py GAME_FOLDER OUT_FOLDER [FILE.BIN ...]

A clip: 28 bytes of header (data size, 3 words of flags, id, then a WAVEFORMAT of the decoded sound) and
IMA ADPCM, 4 bits a sample, the low nibble first, starting from zero.
Writes OUT/FILE-INDEX.wav and OUT/clips.tsv (file, index, offset in the data file, size, flags, seconds).
"""
import array
import glob
import os
import struct
import sys
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tg7l

STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
        107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
        876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
        4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
        22385, 24623, 27086, 29794, 32767]
IDX = [-1, -1, -1, -1, 2, 4, 6, 8]
# (nibble, step index) -> change of the sample
DELTA = [[(-1 if n & 8 else 1) * ((s >> 3) + (s >> 2 if n & 1 else 0) + (s >> 1 if n & 2 else 0) + (s if n & 4 else 0))
          for s in STEP] for n in range(16)]


def decode(data):
    out = array.array('h', bytes(4 * len(data)))
    pred = idx = 0
    k = 0
    for b in data:
        for n in (b & 15, b >> 4):
            pred += DELTA[n][idx]
            if pred > 32767:
                pred = 32767
            elif pred < -32768:
                pred = -32768
            idx += IDX[n & 7]
            if idx < 0:
                idx = 0
            elif idx > 88:
                idx = 88
            out[k] = pred
            k += 1
    return out


def wavetype(r):
    return 12 if r.hsize == 0x1a2 else 9      # (the first version numbers its resource types differently)


def clips(r):
    wt = wavetype(r)
    for i, (t, ext, off, size) in enumerate(r.res):
        if t == wt and size > 28:
            yield i, ext, off, size


def main():
    game, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    names = sys.argv[3:] or sorted(os.path.basename(p) for p in glob.glob(game + '/*.[Bb][Ii][Nn]'))
    with open(out + '/clips.tsv', 'a') as tsv:
        for name in names:
            r = tg7l.ResFile(os.path.join(game, name))
            base = os.path.splitext(name)[0].upper()
            for i, ext, off, size in clips(r):
                d = r.read(i)
                dsize, f1, f2, f3, rid, tag, ch, rate = struct.unpack_from('<IHHHHHHI', d, 0)
                pcm = decode(d[28:28 + dsize])
                w = wave.open('%s/%s-%d.wav' % (out, base, i), 'wb')
                w.setparams((ch, 2, rate, 0, 'NONE', ''))
                w.writeframes(pcm.tobytes())
                w.close()
                tsv.write('%s\t%d\t%d\t%d\t%d,%d,%d\t%.2f\n' % (base, i, off, size, f1, f2, f3, len(pcm) / rate))
            print(base, flush=True)


if __name__ == '__main__':
    main()
