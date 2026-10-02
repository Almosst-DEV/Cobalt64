"""Read the LINKED MH_KEXT_BUNDLE x86_64 image: symbols, segments, and the vtable slots of our classes.
A slot is either (a) an external relocation against an undefined symbol -> ('ref', name), (b) a local relocation / plain pointer into our own image
-> ('def', address, name-of-function-if-known)."""
import struct, subprocess

class Kext:
    def __init__(self, path):
        f = self.f = open(path, 'rb').read()
        magic, cput, csub, ftype, ncmds, sz, flags, _ = struct.unpack_from('<IiiIIIII', f, 0)
        assert magic == 0xfeedfacf and cput == 0x01000007, 'not an x86_64 Mach-O'
        self.ftype = ftype
        off = 32
        self.segs = []
        self.syms = []
        for _ in range(ncmds):
            cmd, csz = struct.unpack_from('<II', f, off)
            if cmd == 0x19:
                segname = f[off + 8:off + 24].split(b'\0')[0].decode()
                vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<QQQQ', f, off + 24)
                self.segs.append(dict(name=segname, vmaddr=vmaddr, vmsize=vmsize, fileoff=fileoff, filesize=filesize))
            elif cmd == 2:
                self.symoff, self.nsyms, self.stroff, self.strsize = struct.unpack_from('<IIII', f, off + 8)
            elif cmd == 0xb:
                d = struct.unpack_from('<' + 'I' * 18, f, off + 8)
                self.dysym = dict(ilocalsym=d[0], nlocalsym=d[1], iextdefsym=d[2], nextdefsym=d[3], iundefsym=d[4], nundefsym=d[5],
                                  extreloff=d[14], nextrel=d[15], locreloff=d[16], nlocrel=d[17])
            off += csz
        for i in range(self.nsyms):
            n_strx, n_type, n_sect, n_desc, n_value = struct.unpack_from('<IBBHQ', f, self.symoff + 16 * i)
            name = f[self.stroff + n_strx: f.index(b'\0', self.stroff + n_strx)].decode()
            self.syms.append(dict(name=name, type=n_type, sect=n_sect, value=n_value))
        self.by_name = {s['name']: s for s in self.syms if (s['type'] & 0xe) == 0xe}
        self.defsyms = sorted((s['value'], s['name']) for s in self.syms if (s['type'] & 0xe) == 0xe and s['sect'])
        self._reloc()

    def _reloc(self):
        f = self.f
        self.ext = {}
        self.loc = {}
        d = self.dysym
        # relocation addresses: try both plausible bases; the caller validates by known-symbol probes
        self.ext_raw = [struct.unpack_from('<iI', f, d['extreloff'] + 8 * i) for i in range(d['nextrel'])]
        self.loc_raw = [struct.unpack_from('<iI', f, d['locreloff'] + 8 * i) for i in range(d['nlocrel'])]

    def file_off(self, va):
        for s in self.segs:
            if s['vmaddr'] <= va < s['vmaddr'] + s['filesize'] and s['filesize']:
                return s['fileoff'] + va - s['vmaddr']
        return None

    def u64(self, va):
        o = self.file_off(va)
        return struct.unpack_from('<Q', self.f, o)[0] if o is not None else None

    def build_reloc_maps(self, base):
        ext, loc = {}, {}
        for addr, info in self.ext_raw:
            symnum = info & 0xffffff
            ext[base + addr] = self.syms[symnum]['name']
        for addr, info in self.loc_raw:
            loc[base + addr] = True
        return ext, loc

    def name_at(self, va):
        import bisect
        vs = [v for v, _ in self.defsyms]
        j = bisect.bisect_right(vs, va) - 1
        return self.defsyms[j][1] if j >= 0 and self.defsyms[j][0] == va else None
