#!/usr/bin/env python3
"""scissorcap.py - build 0.0.499: feed build/scissorcap the frames of one or more runs.

  python3 tests/scissorcap.py [--all] RUNDIR [RUNDIR ...]

Default: the frames the kext's own driver log judged `-> TRANSLATE` (its `gfx-xlat: frame N ws#... stamp 0xS ...` line),
matched to the capture's capdec/frame-*.json by stamp - the committed frames are a subset of these. --all: every captured
frame. Exit status is scissorcap's (0 = no draw exceeds its target)."""
import glob, json, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.environ.get('SCISSORCAP_TOOL') or os.path.join(HERE, '..', 'build', 'scissorcap')   # plant.py builds elsewhere

def frames(run, every):
    stamps = None
    if not every:
        stamps = set()
        rx = re.compile(r'gfx-xlat: frame (\d+) ws#\S+ .*? stamp (0x[0-9a-f]+) .*?-> TRANSLATE')
        with open(os.path.join(run, 'driverlog-stream.txt'), errors='replace') as f:
            for line in f:
                m = rx.search(line)
                if m: stamps.add(int(m.group(2), 16))
    out = []
    for fj in sorted(glob.glob(os.path.join(run, 'capdec', 'frame-*.json'))):
        j = json.load(open(fj))
        if stamps is not None and j.get('stamp') not in stamps: continue
        ibs = [os.path.join(run, 'capdec', ib['path']) for ib in j.get('ibs', []) if ib.get('path')]
        if not ibs or not all(os.path.exists(p) for p in ibs): continue
        if any(ib.get('got') != ib.get('len') for ib in j.get('ibs', [])): continue
        out.append('%s:F%05d %s' % (os.path.basename(run.rstrip('/')), j['frame'], ' '.join(ibs)))
    return out, (len(stamps) if stamps is not None else None)

def main():
    args = sys.argv[1:]
    every = '--all' in args
    runs = [a for a in args if a != '--all']
    if not runs: print(__doc__); return 2
    lines = []
    for r in runs:
        fl, ns = frames(r, every)
        print('%s: %d frame(s) with full IBs%s' % (r, len(fl), '' if ns is None else ' of %d TRANSLATE-judged stamps' % ns))
        lines += fl
    p = subprocess.run([TOOL], input='\n'.join(lines) + '\n', text=True, capture_output=True)
    sys.stdout.write(p.stdout); sys.stderr.write(p.stderr)
    return p.returncode

if __name__ == '__main__':
    sys.exit(main())
