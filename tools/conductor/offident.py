#!/usr/bin/env python3
"""Per-function OFF-identity diff of two x86_64 kext binaries: disassemble every function (objdump), normalise addresses, compare per symbol.
usage: offident.py <before-binary> <after-binary>   -> prints counts + the differing / added / removed function names"""
import re, subprocess, sys, hashlib, collections

def disasm(path):
    out = subprocess.run(['objdump', '-d', '--no-show-raw-insn', '--print-imm-hex', path], capture_output=True, text=True).stdout
    funcs = collections.OrderedDict()
    cur = None
    hdr = re.compile(r'^([0-9a-f]+) <(.+)>:$')
    for l in out.split('\n'):
        m = hdr.match(l)
        if m:
            cur = m.group(2); funcs[cur] = []; continue
        if cur is None or not l.strip() or ':' not in l: continue
        funcs[cur].append(l)
    return funcs

def cstrings(path):
    """addr -> C string for every address inside __TEXT,__cstring (so a changed log message shows up as a diff, a moved one does not)."""
    out = subprocess.run(['otool', '-l', path], capture_output=True, text=True).stdout.split('\n')
    secs = []; cur = None
    for l in out:
        l = l.strip()
        if l.startswith('sectname'): cur = {'n': l.split()[1]}
        elif cur is not None and l.startswith('addr') and 'a' not in cur: cur['a'] = int(l.split()[1], 16)
        elif cur is not None and l.startswith('size') and 's' not in cur: cur['s'] = int(l.split()[1], 16)
        elif cur is not None and l.startswith('offset') and 'o' not in cur: cur['o'] = int(l.split()[1]); secs.append(cur); cur = None
    f = open(path, 'rb').read()
    return [(x['a'], x['s'], x['o']) for x in secs if x['n'] == '__cstring'], f

CS = {}
def cstr_at(path, addr):
    if path not in CS: CS[path] = cstrings(path)
    secs, f = CS[path]
    for a, sz, o in secs:
        if a <= addr < a + sz:
            b = f[o + addr - a: o + addr - a + 400]
            return b.split(b'\0')[0].decode('latin1')
    return None

def norm(lines, path=None):
    o = []
    for l in lines:
        l = re.sub(r'^\s*[0-9a-f]+:\s*', '', l)
        l = re.sub(r'\s+', ' ', l)
        m = re.search(r'## (0x[0-9a-f]+)', l)
        if m and path:
            s_ = cstr_at(path, int(m.group(1), 16))
            if s_ is not None:
                l = l[:m.start()] + '## "%s"' % s_
        
        # branch / call targets: keep only the symbol part
        l = re.sub(r'\b0x[0-9a-f]+ (<[^>]+>)', r'\1', l)
        # rip-relative displacements move with the layout
        l = re.sub(r'-?0x[0-9a-f]+\(%rip\)', 'D(%rip)', l)
        l = re.sub(r'## 0x[0-9a-f]+ <([^>+]+)>', r'## <\1>', l)          # a named symbol: keep the name
        l = re.sub(r'## 0x[0-9a-f]+ <[^>]+\+0x[0-9a-f]+>', '## X', l)    # an offset into another symbol (constants, tables): layout only
        l = re.sub(r'## <[^>]+\+0x[0-9a-f]+>', '## X', l)
        l = re.sub(r'## 0x[0-9a-f]+', '## X', l)
        # a call / jump whose target has no symbol of its own is printed relative to the nearest data anchor (_d_tc_g12+0x...): layout only
        l = re.sub(r'<_d_tc_g12\+0x[0-9a-f]+>', '<X>', l)
        # jumps inside the function: <sym+0xNN> is stable (relative to the function start)
        o.append(l.strip())
    # trailing alignment padding decodes as garbage (addb ..., nop ...) and its length depends on the layout: drop it
    while o and (o[-1].startswith('addb ') or o[-1].startswith('nop') or o[-1].startswith('int3') or o[-1] == 'xchgw %ax, %ax'):
        o.pop()
    return o

def main():
    a, b = disasm(sys.argv[1]), disasm(sys.argv[2])
    ha = {k: hashlib.sha1('\n'.join(norm(v, sys.argv[1])).encode()).hexdigest() for k, v in a.items()}
    hb = {k: hashlib.sha1('\n'.join(norm(v, sys.argv[2])).encode()).hexdigest() for k, v in b.items()}
    same = [k for k in ha if k in hb and ha[k] == hb[k]]
    diff = [k for k in ha if k in hb and ha[k] != hb[k]]
    gone = [k for k in ha if k not in hb]
    new = [k for k in hb if k not in ha]
    print('functions before %d, after %d; identical %d; differing %d; removed %d; added %d' % (len(ha), len(hb), len(same), len(diff), len(gone), len(new)))
    for tag, L in (('DIFF', diff), ('REMOVED', gone), ('ADDED', new)):
        for k in L: print('%-8s %s (%d -> %d insns)' % (tag, k, len(a.get(k, [])), len(b.get(k, []))))
main()
