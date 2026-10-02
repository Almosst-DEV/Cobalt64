#!/usr/bin/env python3
"""imgdump531.py - build 0.0.531 item 4b: the post-STOP image readback's host half.

The kext's switch 87 readback (`accel gfxneuter <arg>`, gfx_p87.h) prints a VRAM range as hex lines in the driver log:
    imgdump531: HDR dump N tag T vram 0x... bytes B W w H h bpp p swizzle s lines L
    imgdump531: D N <byte offset hex> <up to 256 hex characters>
    imgdump531: END dump N bytes B read-fail F fnv XXXXXXXX
This script packs the verb arguments and turns a scp'd driver log into raw image files.

usage:
  imgdump531.py pack geom <W> <H> <bytes-per-pixel 1|2|4|8|16> <swizzle 0..31> <tag 0..255>
  imgdump531.py pack dump <vram-offset (256-byte aligned)> <length (256-byte multiple, <= 65536)>
  imgdump531.py decode <driver-log> <out-dir>

Each decoded dump is written to <out-dir>/imgdump-<N>-tag<T>-<W>x<H>x<bpp>B-sw<s>-vram<off>.n48img: a 64-byte header
(magic b'N48IMG1\\0', then little-endian u32 version 1, W, H, bytes per pixel, swizzle, tag, dump number, byte count,
read failures, FNV-1a ok (1/0), then u64 VRAM offset, zero padding) followed by the raw bytes exactly as VRAM held them
(a tiled surface stays tiled: the swizzle field says how to detile it). Missing lines are zero-filled and reported.
"""
import os
import re
import struct
import sys

HDR = re.compile(r'imgdump531: HDR dump (\d+) tag (\d+) vram (0x[0-9a-f]+|0) bytes (\d+) W (\d+) H (\d+) bpp (\d+) swizzle (\d+) lines (\d+)')
DAT = re.compile(r'imgdump531: D (\d+) ([0-9a-f]+) ([0-9a-f]*)')
END = re.compile(r'imgdump531: END dump (\d+) bytes (\d+) read-fail (\d+) fnv ([0-9a-f]{8})')
MAGIC = b'N48IMG1\0'


def fnv1a(b):
    h = 2166136261
    for x in b:
        h ^= x
        h = (h * 16777619) & 0xffffffff
    return h


def pack_geom(w, h, bpp, sw, tag):
    l2 = {1: 0, 2: 1, 4: 2, 8: 3, 16: 4}.get(bpp)
    if l2 is None or not (0 < w < 16384 and 0 < h < 16384 and 0 <= sw < 32 and 0 <= tag < 256):
        raise SystemExit('geom: W, H < 16384; bpp 1/2/4/8/16; swizzle < 32; tag < 256')
    return 87 | (3 << 8) | (w << 16) | (h << 30) | (l2 << 44) | (sw << 48) | (tag << 56)


def pack_dump(off, length):
    if off & 0xff or length & 0xff or not (0 < length <= 0x10000) or off >> 40:
        raise SystemExit('dump: offset 256-byte aligned below 2^40; length a 256-byte multiple, 1..65536')
    return 87 | (4 << 8) | ((off >> 8) << 16) | ((length >> 8) << 48)


def decode(log, outdir):
    dumps = {}
    with open(log, 'r', errors='replace') as f:
        for line in f:
            m = HDR.search(line)
            if m:
                n = int(m.group(1))
                dumps[n] = dict(tag=int(m.group(2)), vram=int(m.group(3), 16), bytes=int(m.group(4)), w=int(m.group(5)),
                                h=int(m.group(6)), bpp=int(m.group(7)), sw=int(m.group(8)), lines=int(m.group(9)),
                                data={}, end=None)
                continue
            m = DAT.search(line)
            if m and int(m.group(1)) in dumps:
                dumps[int(m.group(1))]['data'][int(m.group(2), 16)] = bytes.fromhex(m.group(3))
                continue
            m = END.search(line)
            if m and int(m.group(1)) in dumps:
                dumps[int(m.group(1))]['end'] = (int(m.group(2)), int(m.group(3)), int(m.group(4), 16))
    os.makedirs(outdir, exist_ok=True)
    bad = 0
    for n in sorted(dumps):
        d = dumps[n]
        buf = bytearray(d['bytes'])
        missing = 0
        for at in range(0, d['bytes'], 128):
            chunk = d['data'].get(at)
            if chunk is None:
                missing += 1
                continue
            buf[at:at + len(chunk)] = chunk[:d['bytes'] - at]
        ok = d['end'] is not None and d['end'][0] == d['bytes'] and fnv1a(buf) == d['end'][2] and missing == 0
        rf = d['end'][1] if d['end'] else 0
        hdr = MAGIC + struct.pack('<10IQ', 1, d['w'], d['h'], d['bpp'], d['sw'], d['tag'], n, d['bytes'], rf, 1 if ok else 0,
                                  d['vram'])
        hdr += b'\0' * (64 - len(hdr))
        name = 'imgdump-%u-tag%u-%ux%ux%uB-sw%u-vram%x.n48img' % (n, d['tag'], d['w'], d['h'], d['bpp'], d['sw'], d['vram'])
        with open(os.path.join(outdir, name), 'wb') as f:
            f.write(hdr)
            f.write(bytes(buf))
        print('%s: %u bytes, %u line(s) missing, read failures %u, END %s, FNV %s' %
              (name, d['bytes'], missing, rf, 'seen' if d['end'] else 'MISSING', 'ok' if ok else 'MISMATCH'))
        bad += 0 if ok else 1
    print('%u dump(s), %u not clean' % (len(dumps), bad))
    return 0 if bad == 0 else 1


def main(a):
    if len(a) >= 2 and a[0] == 'pack' and a[1] == 'geom' and len(a) == 7:
        v = pack_geom(*[int(x, 0) for x in a[2:]])
    elif len(a) >= 2 and a[0] == 'pack' and a[1] == 'dump' and len(a) == 4:
        v = pack_dump(int(a[2], 0), int(a[3], 0))
    elif len(a) == 3 and a[0] == 'decode':
        return decode(a[1], a[2])
    else:
        sys.stderr.write(__doc__)
        return 2
    print('%u  (%#x)   ->  sudo navi48test accel gfxneuter %u' % (v, v, v))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
