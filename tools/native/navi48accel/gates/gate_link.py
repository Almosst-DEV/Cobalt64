#!/usr/bin/env python3
"""Gates A, B, C on the LINKED Navi48Accel kext (make FAILS on any mismatch).
  A  vtable order/identity: for every leaf class, in the linked image
       - the vtable has exactly the family parent's slot count (zero terminator right after; nothing extra),
       - every slot we DEFINE is in the expected override set (OSDefine dtors/getMetaClass + the parent's pure slots + the forwarders + the named extras),
         every expected override IS defined, and points into our __TEXT,
       - every other slot is an external reference to a symbol the family's own vtable of the parent holds at that slot (any alias of that address),
         i.e. the kext linker never has to patch anything and no placeholder / non-exported symbol is referenced.
  B  imports: every undefined symbol of the kext exists in the exports of IOAcceleratorFamily2 / the Boot KC / the System KC.
  C  the generated header is fresh (regenerated into a temp dir == the committed one) and check_layout.py (test subclasses of the 9 family classes
     against the KC vtables: slot count, slot order, exact symbols) still says MATCH.
Usage: gate_link.py <linked kext binary> <object file>"""
import os, re, subprocess, sys, tempfile, hashlib
from common import *
import kextimg
import exports

def gate_a(k, C, fwd):
    ext, loc = k.build_reloc_maps(0)
    text = [s for s in k.segs if s['name'] == '__TEXT'][0]
    problems, nchecked = [], 0
    for leaf, parent, extra in LEAVES:
        sym = k.by_name.get(ztv(leaf))
        if not sym:
            problems.append('%s: vtable symbol %s not defined in the linked kext' % (leaf, ztv(leaf))); continue
        va = sym['value']
        n = C[parent]['nslots']
        want_ovr = set(override_slots(leaf, parent, extra, C, fwd))
        got_ovr = set()
        for i in range(n):
            a = va + 16 + 8 * i
            row = C[parent]['rows'][i]
            nchecked += 1
            if a in ext:
                if i in want_ovr:
                    problems.append('%s slot %d: expected an override, found a reference to %s' % (leaf, i, ext[a])); continue
                if ext[a] not in row['mangled']:
                    problems.append('%s slot %d: references %s, the family vtable of %s holds %s' % (leaf, i, ext[a], parent, ' | '.join(row['mangled'])))
            elif a in loc or k.u64(a):
                got_ovr.add(i)
                tgt = k.u64(a)
                if not (text['vmaddr'] <= tgt < text['vmaddr'] + text['vmsize']):
                    problems.append('%s slot %d: defined slot points outside our __TEXT (%#x)' % (leaf, i, tgt))
                if i not in want_ovr:
                    problems.append('%s slot %d: UNEXPECTED override (%s) - not in the expected set' % (leaf, i, k.name_at(tgt)))
            else:
                problems.append('%s slot %d: null slot inside the vtable' % (leaf, i))
        for i in sorted(want_ovr - got_ovr):
            problems.append('%s slot %d: expected override missing' % (leaf, i))
        term = va + 16 + 8 * n
        if term in ext or term in loc or k.u64(term):
            problems.append('%s: vtable longer than the family parent (%d slots): slot %d is not the zero terminator' % (leaf, n, n))
        print('  A %-22s %3d slots vs %-26s: %3d own, %3d inherited by exact family symbol' % (leaf, n, parent, len(got_ovr), n - len(got_ovr)))
    return problems, nchecked

def gate_b(k):
    ex = exports.load()
    # undefined = N_UNDF with n_value == 0 (a non-zero n_value is a COMMON symbol: our own tentative definitions, e.g. Navi48X::gMetaClass)
    und = [s['name'] for s in k.syms if s['sect'] == 0 and (s['type'] & 0xee) == 0 and s['value'] == 0 and s['name']]
    missing = sorted(u for u in und if u not in ex)
    return und, missing

def gate_c():
    problems = []
    tmp = tempfile.mkdtemp(prefix='n48gen')
    env = dict(os.environ, N48_GEN_OUT=tmp)
    r = subprocess.run([sys.executable, os.path.join(LAYOUT, 'gen_header.py')], env=env, capture_output=True, text=True)
    if r.returncode:
        problems.append('gen_header.py failed: ' + r.stderr[-300:])
    else:
        a = open(os.path.join(tmp, 'IOAccelFamily2_decl.h'), 'rb').read()
        b = open(os.path.join(LAYOUT, 'generated', 'IOAccelFamily2_decl.h'), 'rb').read()
        if a != b:
            problems.append('generated/IOAccelFamily2_decl.h is STALE (regenerating gives a different file): run tools/native/ioaccel-layout/gen_header.py')
        else:
            print('  C generated header fresh: sha256 %s' % hashlib.sha256(a).hexdigest()[:16])
    cl = subprocess.run([sys.executable, os.path.join(LAYOUT, 'check_layout.py')], capture_output=True, text=True)
    last = [l for l in cl.stdout.splitlines() if l.startswith(('MATCH', 'MISMATCH', 'COMPILE'))]
    print('  C check_layout.py: ' + (last[-1] if last else 'no verdict'))
    if cl.returncode != 0:
        problems.append('check_layout.py did not MATCH: ' + ' / '.join(cl.stdout.splitlines()[-4:]))
    return problems

def main():
    kext = sys.argv[1]
    k = kextimg.Kext(kext)
    C = model(); fwd = forwarded()
    pa, n = gate_a(k, C, fwd)
    if len(LEAVES) != EXPECT_CLASSES or n != EXPECT_SLOTS:
        pa.append('totals: %d classes / %d slots, leaves.py expects %d / %d' % (len(LEAVES), n, EXPECT_CLASSES, EXPECT_SLOTS))
    und, missing = gate_b(k)
    print('  B %d undefined symbols, %d not exported by family/kernel/IOGraphicsFamily' % (len(und), len(missing)))
    pb = ['import not exported: %s' % m for m in missing]
    pc = gate_c()
    bad = pa + pb + pc
    if bad:
        print('GATE FAIL: %d problem(s); first:' % len(bad))
        for b in bad[:12]:
            print('   ' + b)
        return 1
    print('GATES A/B/C PASS: %d vtable slots checked against the family, %d imports resolved' % (n, len(und)))
    return 0

if __name__ == '__main__':
    sys.exit(main())
