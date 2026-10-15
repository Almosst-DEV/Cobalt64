"""Read a class vtable straight out of an x86_64 MH_OBJECT: per slot either a defined
function (section+addend) or an extern relocation to a symbol. No linking involved."""
import struct, subprocess, bisect

def parse(path):
    f = open(path, 'rb').read()
    magic, cput, csub, ftype, ncmds, sz, flags, _ = struct.unpack_from('<IiiIIIII', f, 0)
    assert magic == 0xfeedfacf and ftype == 1, 'not an x86_64 MH_OBJECT'
    off = 32
    secs = []
    symtab = None
    for _ in range(ncmds):
        cmd, csz = struct.unpack_from('<II', f, off)
        if cmd == 0x19:
            nsects = struct.unpack_from('<I', f, off + 64)[0]
            so = off + 72
            for _ in range(nsects):
                sectname = f[so:so + 16].split(b'\0')[0].decode()
                segname = f[so + 16:so + 32].split(b'\0')[0].decode()
                addr, size, offset, align, reloff, nreloc = struct.unpack_from('<QQIIII', f, so + 32)
                secs.append(dict(name=segname + ',' + sectname, addr=addr, size=size, offset=offset,
                                 reloff=reloff, nreloc=nreloc))
                so += 80
        elif cmd == 2:
            symtab = struct.unpack_from('<IIII', f, off + 8)
        off += csz
    symoff, nsyms, stroff, strsize = symtab
    syms = []
    for i in range(nsyms):
        n_strx, n_type, n_sect, n_desc, n_value = struct.unpack_from('<IBBHQ', f, symoff + 16 * i)
        name = f[stroff + n_strx: f.index(b'\0', stroff + n_strx)].decode()
        syms.append(dict(name=name, type=n_type, sect=n_sect, value=n_value, ext=bool(n_type & 1)))
    return f, secs, syms


def vtable(path, vtsym):
    """-> list of slots [{'kind': 'ref'|'def', 'sym': mangled}] for vtsym (slot 0 = vtable+16)."""
    f, secs, syms = parse(path)
    v = [s for s in syms if s['name'] == vtsym and (s['type'] & 0xe) == 0xe]
    if not v:
        return None
    v = v[0]
    sec = secs[v['sect'] - 1]
    # vtable extent: next symbol in the same section above it, or section end
    above = sorted(s['value'] for s in syms if (s['type'] & 0xe) == 0xe and s['sect'] == v['sect'] and s['value'] > v['value'])
    end = above[0] if above else sec['addr'] + sec['size']
    base = v['value'] - sec['addr']
    n = (end - v['value'] - 16) // 8
    relocs = {}
    for r in range(sec['nreloc']):
        r_address, r_info = struct.unpack_from('<iI', f, sec['reloff'] + 8 * r)
        symnum = r_info & 0xffffff
        r_ext = (r_info >> 27) & 1
        r_type = (r_info >> 28) & 0xf
        relocs[r_address] = (symnum, r_ext, r_type)
    text_syms = sorted((s['value'], s['name'], s['sect']) for s in syms if (s['type'] & 0xe) == 0xe)
    tv = [t[0] for t in text_syms]
    out = []
    for i in range(n):
        a = base + 16 + 8 * i
        rl = relocs.get(a)
        raw = struct.unpack_from('<Q', f, sec['offset'] + a)[0]
        if rl and rl[1]:
            sy = syms[rl[0]]
            out.append(dict(kind='def' if (sy['type'] & 0xe) == 0xe else 'ref', sym=sy['name']))
        elif rl or raw:
            # non-extern: address inside a section of this object
            j = bisect.bisect_right(tv, raw) - 1
            nm = None
            if j >= 0 and text_syms[j][0] == raw:
                nm = text_syms[j][1]
            out.append(dict(kind='def', sym=nm or '<local 0x%x>' % raw))
        else:
            out.append(dict(kind='null', sym=''))
    return out
