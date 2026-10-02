#!/usr/bin/env python3
"""Host twin of the runtime vtable gate (MUST-FIX 3). Simulates what the kernel linker does with OUR linked kext and compares it, with the SAME C++ function
the kext runs in probe(), against the family's real vtables read out of the EXTRACTED family binary (~/navi48-native/re-s3/kexts/com.apple.iokit.IOAcceleratorFamily2):
  ours[i]   = a reference to symbol S  -> the address of S in the family / kernel image (exact, not by name);  a defined slot -> a fake address inside a fake text window;
  family[i] = the target address the family's own __ZTV<Parent> holds (chained-fixup decoded by ioaccel-layout/model.py).
Prints MATCH per class. Usage: layout_gate_host.py <linked kext binary>"""
import os, subprocess, sys, tempfile
from common import *
import kextimg, exports

def sym_addr_table():
    """name -> (lvl, va) for every exported symbol (family kext: level 1; boot KC / kernel: level 0), the same encoding the model uses."""
    import model as M
    from kcmod import FAMILY, BOOT_KC
    tab = {}
    for lvl, p in ((1, FAMILY), (0, BOOT_KC)):
        for l in subprocess.run(['nm', '-gU', p], capture_output=True, text=True).stdout.splitlines():
            q = l.split(' ', 2)
            if len(q) == 3 and all(c in '0123456789abcdef' for c in q[0]) and q[0]:
                tab.setdefault(q[2], (lvl, int(q[0], 16)))
    return tab

def enc(lvl, va):
    return (lvl << 60) ^ (va & 0x0fffffffffffffff)

def main():
    k = kextimg.Kext(sys.argv[1])
    C = model(); fwd = forwarded()
    ext, loc = k.build_reloc_maps(0)
    tab = sym_addr_table()
    TEXT_LO, TEXT_HI = 0x100000, 0x200000       # the fake window our defined slots map into
    lines = []
    neg = None
    for leaf, parent, extra in LEAVES:
        n = C[parent]['nslots']
        ovr = override_slots(leaf, parent, extra, C, fwd)
        va = k.by_name[ztv(leaf)]['value']
        ours, fam = [], []
        for i in range(n):
            a = va + 16 + 8 * i
            if a in ext:
                s = ext[a]
                if s not in tab:
                    print('FAIL   %s slot %d: reference %s does not resolve in the family/kernel image' % (leaf, i, s)); return 1
                ours.append(enc(*tab[s]))
            else:
                ours.append(TEXT_LO + 16 * (i + 1))     # a defined slot (loc reloc / plain pointer into our image)
            r = C[parent]['rows'][i]
            fam.append(enc(r['lvl'], r['va']))
        ours.append(0)                                   # the terminator our vtable must end with
        hasterm = not (va + 16 + 8 * n in ext or va + 16 + 8 * n in loc or k.u64(va + 16 + 8 * n))
        if not hasterm:
            ours[-1] = 1
        assert C[parent]['nterm'] >= 1, 'family vtable of %s has no zero terminator in the model' % parent
        fam.append(0)                                    # the family's own terminator (model: nterm)
        lines.append('CLASS %s %s %d %x %x %d' % (leaf, parent, n, TEXT_LO, TEXT_HI, len(ovr)))
        lines.append(' '.join(str(x) for x in ovr))
        lines.append(' '.join('%x' % x for x in ours))
        lines.append(' '.join('%x' % x for x in fam))
        if leaf == NEGATIVE_CLASS:
            neg = (leaf, parent, n, ovr, list(ours), list(fam))
    d = tempfile.mkdtemp(prefix='n48twin')
    inp, exe = os.path.join(d, 'in.txt'), os.path.join(d, 'twin')
    open(inp, 'w').write('\n'.join(lines) + '\n')
    r = subprocess.run(['clang++', '-std=c++17', '-O1', '-I' + os.path.join(HERE, '..', 'src'), os.path.join(HERE, 'layout_gate_host.cpp'), '-o', exe], capture_output=True, text=True)
    if r.returncode:
        print(r.stderr); return 2
    p = subprocess.run([exe, inp], capture_output=True, text=True)
    print(p.stdout.rstrip())
    if p.returncode:
        return p.returncode
    last = p.stdout.rstrip().splitlines()[-1] if p.stdout.strip() else ''
    want = 'ALL MATCH: %d classes, %d slots compared' % (EXPECT_CLASSES, EXPECT_SLOTS)
    if last != want:
        print('TOTALS MISMATCH: the twin said %r, leaves.py expects %r' % (last, want)); return 1
    # Negative controls (aux 0.0.3): the gate must REFUSE a family IOAccelDisplayPipe that differs from the one we were built against. Each corrupted copy is run
    # through the same gate_compare; a single MATCH among them means the gate would accept a mismatched pipe class.
    if neg is None:
        print('NEGATIVE CONTROLS: class %s not found' % NEGATIVE_CLASS); return 1
    leaf, parent, n, ovr, ours, fam = neg
    inh = [i for i in range(n) if i not in ovr]
    cases = []
    assert 282 in inh, 'slot 282 (copyCapabilities) must be inherited: the capabilities come from the accelerator property'
    f1 = list(fam); f1[282] ^= 0x10; cases.append(('the family moved copyCapabilities (slot 282)', ours, f1))
    f2 = list(fam); f2[inh[len(inh) // 2]] ^= 0x10; cases.append(('an inherited slot (%d) differs' % inh[len(inh) // 2], ours, f2))
    f3 = list(fam); f3[n] = 0x1234; cases.append(('the family vtable is longer (307 slots)', ours, f3))
    o4 = list(ours); o4[277] = TEXT_HI + 0x10; cases.append(('our slot 277 points outside our text', o4, fam))
    f5 = list(fam); f5[n - 1] = 0; cases.append(('the family vtable is shorter (a NULL at slot %d)' % (n - 1), ours, f5))
    escaped = 0
    for what, o, f in cases:
        body = ['CLASS %s %s %d %x %x %d' % (leaf, parent, n, TEXT_LO, TEXT_HI, len(ovr)), ' '.join(str(x) for x in ovr), ' '.join('%x' % x for x in o), ' '.join('%x' % x for x in f)]
        open(inp, 'w').write('\n'.join(body) + '\n')
        q = subprocess.run([exe, inp], capture_output=True, text=True)
        refused = q.returncode != 0 and q.stdout.startswith('FAIL')
        print('NEGATIVE CONTROL %-50s %s' % (what + ':', 'REFUSED' if refused else '*** ACCEPTED ***'))
        if not refused: escaped += 1
    if escaped:
        print('NEGATIVE CONTROLS: %d of %d mismatched %s tables ACCEPTED' % (escaped, len(cases), parent)); return 1
    print('NEGATIVE CONTROLS: all %d mismatched %s tables refused' % (len(cases), parent))
    return 0

if __name__ == '__main__':
    sys.exit(main())
