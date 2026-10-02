#!/usr/bin/env python3
"""gen_fs85_fixture.py - build 0.0.530 (notes/design/SRCFILL85.md, "Replay"): build tests/fixture_fs85_runs.h from the
REAL captures of the four switch-33/35 boots the design cites - Z2 run11i, Z3 run11j, AB run11l, AB2 run11m - never by hand.

usage: gen_fs85_fixture.py <runs-dir> <shader-ids-header> > fixture_fs85_runs.h
   runs-dir           notes/logs/runs of the MAIN checkout (the captures are not in git)
   shader-ids-header  src/xlat12/xlat12_shader_ids.h (the ColorFill / GPUPass rows are read from it BY NAME)

Per frame 1..160 (the capdec window) it joins:
  - capdec/frame-NNNNN.json: IB count, the first CB0 region (the kext's tgtVa), every PGM-PS / PGM-VS region; each PS region's
    bytes are matched against the two identity rows exactly as xlat12_shader_id_match does (stage 0, head, ndw, s_endpgm, fnv);
    `uptime` (ns) gives the frame's time;
  - the kext's own `dpled841: fN` line: arm level, the live gate answer, the translator verdict, the dependency verdict;
  - `descriptor PROVENANCE REFUSED: surface VA S ... frame N` lines (the surfaces a frame could not prove);
  - `cyc80: SHADOW P fN seq S` (a frame the gate committed) joined to `keystone: VERDICT V ... gate seq S`.
Nothing here decides a verdict: each field is a reading. The replay's hypotheses live in gfx_fs85_test.cpp.
"""
import json
import os
import re
import sys

FRAMES = 160


def rows(hdr):
    out = {}
    for m in re.finditer(r'\{ "([^"]+)", (\d+)u, (\d+)u, 0x([0-9a-f]+)u, \{ 0x([0-9a-f]+)u, 0x([0-9a-f]+)u, 0x([0-9a-f]+)u, 0x([0-9a-f]+)u \}',
                         open(hdr).read()):
        out[m.group(1)] = (int(m.group(2)), int(m.group(3)), int(m.group(4), 16), [int(m.group(i), 16) for i in range(5, 9)])
    return out


def fnv(d, n):
    h = 2166136261
    for k in range(n):
        h ^= d[k]
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def match(code, row):
    stage, ndw, f, head = row
    if stage != 0 or len(code) < ndw or ndw < 4:
        return False
    if code[:4] != head or code[ndw - 1] != 0xBFB00000:
        return False
    return fnv(code, ndw) == f


def dwords(path):
    b = open(path, 'rb').read()
    return [int.from_bytes(b[i:i + 4], 'little') for i in range(0, len(b) - 3, 4)]


GATES = {'COMMIT': 0, 'RESERVED-FOR-FILL': 1, 'RESERVED-FOR-PLANE': 2}


def run(runs, name, idrows):
    d = os.path.join(runs, name)
    log = open(os.path.join(d, 'driverlog-stream-through-park.txt'), errors='replace').read().splitlines()
    dp, prov, seqf, ks = {}, {}, {}, {}
    for ln in log:
        m = re.search(r'dpled841: f(\d+) arm (\d+) .* gate (\S+) vrd (\S+) .* dep (\S+) d ', ln)
        if m and int(m.group(1)) not in dp:
            dp[int(m.group(1))] = (int(m.group(2)), m.group(3), m.group(4), m.group(5))
        m = re.search(r'PROVENANCE REFUSED: surface VA (0x[0-9a-f]+) .* frame (\d+) seg (\d+)', ln)
        if m:
            s = prov.setdefault(int(m.group(2)), [])
            v = int(m.group(1), 16)
            if v not in s:
                s.append(v)
        m = re.search(r'cyc80: SHADOW P f(\d+) seq (\d+) ', ln)
        if m and int(m.group(2)) != 0:
            seqf[int(m.group(1))] = int(m.group(2))
        m = re.search(r'keystone: VERDICT (\d+) .* gate seq (\d+);', ln)
        if m:
            ks[int(m.group(2))] = int(m.group(1))
    fill, plane = idrows['ws_B_ColorFill'], idrows['ws_D_GPUPass']
    recs, t0 = [], None
    for f in range(1, FRAMES + 1):
        p = os.path.join(d, 'capdec', 'frame-%05d.json' % f)
        if f not in dp:
            continue
        j = json.load(open(p)) if os.path.exists(p) else {'regions': [], 'ibs': [], 'uptime': None}
        cb0, nps, nvs, isfill, isplane = 0, 0, 0, 0, 0
        for r in sorted(j['regions'], key=lambda r: (r['ib'], r['dword'])):
            k = r['kind_name']
            if k == 'CB0' and cb0 == 0:
                cb0 = r['va']
            elif k == 'PGM-VS':
                nvs += 1
            elif k == 'PGM-PS':
                nps += 1
                code = dwords(os.path.join(d, 'capdec', r['path']))
                if match(code, fill):
                    isfill = 1
                if match(code, plane):
                    isplane = 1
        if t0 is None and j['uptime'] is not None:
            t0 = j['uptime']
        arm, gate, vrd, dep = dp[f]
        seq = seqf.get(f, 0)
        pv = prov.get(f, [])
        t_ms = 0xFFFFFFFF if j['uptime'] is None else (j['uptime'] - t0) // 1000000
        recs.append(dict(f=f, t_ms=t_ms, arm=arm, ws=0 if vrd == 'shape' else 1, cap=1 if j['uptime'] is not None else 0,
                         translate=1 if vrd == 'TRANSLATE' else 0, gate=GATES.get(gate, 3), gate_name=gate,
                         dep_clean=1 if dep == 'clean' else 0, nib=len(j['ibs']), nps=nps, nvs=nvs, fill=isfill,
                         plane=isplane, cb0=cb0, seq=seq, ks=ks.get(seq, 255) if seq else 255,
                         prov=pv[:4], nprov=len(pv), vrd=vrd, dep=dep))
    return recs


def emit(tag, recs):
    print('static const n48_fs85_fx k%s[] = {' % tag)
    for r in recs:
        pv = (r['prov'] + [0, 0, 0, 0])[:4]
        print('    { %3u, %#010xu, %u, %u, %u, %u, %u, %u, %u, %u, %2u, %u, %u, %#014xull, %3u, %3u, %u, { %s } },   /* gate %s vrd %s dep %s */' % (
            r['f'], r['t_ms'], r['arm'], r['ws'], r['cap'], r['translate'], r['gate'], r['dep_clean'], r['nib'], r['nps'], r['nvs'],
            r['fill'], r['plane'], r['cb0'], r['seq'], r['ks'], r['nprov'],
            ', '.join('%#014xull' % v for v in pv), r['gate_name'], r['vrd'], r['dep']))
    print('};')


def main():
    runs, hdr = sys.argv[1], sys.argv[2]
    idrows = rows(hdr)
    print('/* fixture_fs85_runs.h - GENERATED by gen_fs85_fixture.py from notes/logs/runs/run11{i,j,l,m} (capdec + the kext\'s own')
    print(' * dpled841 / PROVENANCE REFUSED / cyc80 / keystone lines). Do not edit: re-run the generator. build 0.0.530. */')
    print('#ifndef N48_FIXTURE_FS85_RUNS_H\n#define N48_FIXTURE_FS85_RUNS_H\n#include <stdint.h>')
    print('/* gate: 0 COMMIT, 1 RESERVED-FOR-FILL, 2 RESERVED-FOR-PLANE, 3 any other answer (a refusal, or the gate never ran).')
    print(' * ws: 1 for a WindowServer frame (dpled vrd is not `shape`); cap: 1 when capdec holds the frame (t_ms 0xffffffff = none).')
    print(' * ks: the keystone verdict for the committed seq (255 none). prov: up to 4 surfaces the frame could not prove. */')
    print('typedef struct { uint16_t f; uint32_t t_ms; uint8_t arm, ws, cap, translate, gate, dep_clean, nib, nps, nvs, fill, plane;')
    print('                 uint64_t cb0; uint16_t seq; uint8_t ks, nprov; uint64_t prov[4]; } n48_fs85_fx;')
    for tag, name in (('Z2', 'run11i'), ('Z3', 'run11j'), ('AB', 'run11l'), ('AB2', 'run11m')):
        emit(tag, run(runs, name, idrows))
    print('#endif')


if __name__ == '__main__':
    main()
