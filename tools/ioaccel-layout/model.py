"""Builds the vtable/layout model from the KCs and carved kexts (deterministic)."""
import re, struct, json, os, sys
from kcmod import *

_sp = None


class Space:
    def __init__(self):
        self.fam = Image(FAMILY, 'IOAcceleratorFamily2')
        self.pv = Image(PARAVIRT, 'AppleParavirtGPU')
        self.sysk = Image(SYS_KC, 'SystemKC')
        self.boot = Image(BOOT_KC, 'BootKC')
        self.order = [self.fam, self.pv, self.sysk, self.boot]
        self.pure_va = self.boot.by_name['___cxa_pure_virtual']
        self.pure_off = self.pure_va - KERNEL_BASE
        self._dem = {}

    def norm(self, raw):
        if raw is None:
            return None, None
        if raw >= 1 << 63:
            return 0, raw                       # kernel's own plain pointer
        lvl = (raw >> 30) & 3
        off = raw & 0x3FFFFFFF
        if lvl == 0:
            return 0, KERNEL_BASE + off
        return lvl, off

    def symbols(self, lvl, va):
        """All symbols exactly at va (aliases), mangled."""
        img = self.boot if lvl == 0 else self.sysk
        return img.aliases_at(va)

    def find_vt(self, mangled_cls):
        vt = '__ZTV' + mangled_cls
        for img in self.order:
            if vt in img.by_name:
                return img, vt
        return None, vt

    def dem(self, s):
        if s not in self._dem:
            self._dem[s] = demangle([s])[0]
        return self._dem[s]


def mangle_cls(name):
    return '%d%s' % (len(name), name)


def _aliases_at(self, va):
    i = bisect.bisect_left(self.addrs, va)
    out = []
    while i < len(self.addrs) and self.addrs[i] == va:
        out.append(self.syms[i][1])
        i += 1
    return out


Image.aliases_at = _aliases_at


def class_info(sp, cls):
    """Parse <cls>::MetaClass::MetaClass() : superclass, instance size, name string."""
    m = mangle_cls(cls)
    for img in sp.order:
        for c in ('C2', 'C1'):
            nm = '__ZN%s9MetaClass%sEv' % (m, c)
            if nm in img.by_name:
                va = img.by_name[nm]
                code = img.rdbytes(va, 96)
                info = {'ctor': nm, 'ctor_va': va}
                # Instruction-aligned parse of the OSMetaClass ctor call setup:
                #   lea rsi,[rip+rel32]  (class-name string)      48 8d 35 rel32
                #   mov rdx,[rip+rel32] (GOT super) | lea rdx,... | xor edx,edx
                #   mov ecx, imm32 (instance size)                b9 imm32
                pos = None
                for m0 in re.finditer(rb'\x48\x8d\x35(....)', code, re.S):
                    s_ = img.cstr(va + m0.end() + struct.unpack('<i', m0.group(1))[0])
                    nxt = code[m0.end():m0.end() + 1]
                    if s_ and s_.isidentifier() and nxt in (b'\x48', b'\x31'):
                        pos = m0
                        info['str'] = s_
                        break
                if pos is None:
                    return info
                p = pos.end()
                sup_va = None
                lvl = None
                if code[p:p + 3] in (b'\x48\x8b\x15', b'\x48\x8d\x15'):
                    rel = struct.unpack('<i', code[p + 3:p + 7])[0]
                    ip = va + p + 7
                    if code[p + 1] == 0x8b:
                        lvl, sup_va = sp.norm(img.rd64(ip + rel))
                    else:
                        sup_va = ip + rel
                        lvl = 0 if img is sp.boot else 1
                    p += 7
                elif code[p:p + 2] == b'\x31\xd2':
                    info['super'] = None
                    p += 2
                if code[p] == 0xb9:
                    info['size'] = struct.unpack('<I', code[p + 1:p + 5])[0]
                if sup_va is not None:
                    names = [a for a in sp.symbols(lvl, sup_va) if a.endswith('10gMetaClassE')]
                    info['super'] = sp.dem(names[0]).replace('::gMetaClass', '') if names else '?0x%x' % sup_va
                return info
    return None


def decode_vtable(sp, cls):
    img, vt = sp.find_vt(mangle_cls(cls))
    if img is None:
        return None
    a = img.by_name[vt]
    end = img.vtable_end(vt)
    n = (end - a - 16) // 8
    slots = []
    for i in range(n):
        raw = img.rd64(a + 16 + 8 * i)
        lvl, va = sp.norm(raw)
        if raw == 0:
            slots.append(dict(i=i, off=16 + 8 * i, raw=0, lvl=None, va=0, syms=[], pure=False, null=True))
            continue
        syms = sp.symbols(lvl, va)
        if not syms:
            near = (sp.boot if lvl == 0 else sp.sysk).sym_at(va)
            syms = []
        slots.append(dict(i=i, off=16 + 8 * i, raw=raw, lvl=lvl, va=va, syms=syms,
                          pure=(lvl == 0 and va == sp.pure_va), null=False))
    # -fapple-kext vtables end with a zero terminator entry. The first null slot ends the
    # vtable; for the two carved kexts everything after it up to the next symbol must be
    # null padding (asserted). The kernel's next-symbol extent can run into unrelated data.
    first = next((k for k, x in enumerate(slots) if x.get('null')), len(slots))
    if img.name in ('IOAcceleratorFamily2', 'AppleParavirtGPU'):
        assert all(x.get('null') for x in slots[first:]), 'non-null data after terminator in ' + cls
    nterm = len(slots) - first
    slots = slots[:first]
    return dict(image=img.name, vt_sym=vt, vt_va=a, nslots=len(slots), nterm=nterm, slots=slots)


def slot_name(sp, sl):
    """Preferred display symbol of a slot (demangled) + all aliases."""
    if sl.get('null'):
        return '<null>', []
    if sl['pure']:
        return '__cxa_pure_virtual', []
    syms = sl['syms']
    if not syms:
        return '<no symbol @0x%x>' % sl['va'], []
    # deterministic preference: C1/D1 style complete-object first is irrelevant; take the
    # lexicographically smallest mangled name among aliases
    d = sorted(syms)
    return sp.dem(d[0]), [sp.dem(x) for x in d[1:]]


def chain_of(sp, cls, infos):
    out = [cls]
    while True:
        if cls == 'OSObject':
            cls = 'OSMetaClassBase'
        else:
            ci = infos.get(cls) or class_info(sp, cls)
            infos[cls] = ci
            if not ci or not ci.get('super'):
                break
            cls = ci['super']
        if cls.startswith('?'):
            break
        out.append(cls)
        if cls == 'OSMetaClassBase':
            break
    return out


def all_classes(sp):
    """Every non-metaclass, global (S) vtable in the two carved kexts."""
    res = []
    for img in (sp.fam, sp.pv):
        for a, n in img.syms:
            if n.startswith('__ZTV') and not n.startswith('__ZTVN') or n.startswith('__ZTVN') and False:
                pass
        names = sorted(n for a, n in img.syms if n.startswith('__ZTV') and n in img.by_name)
        for n in names:
            d = sp.dem(n).replace('vtable for ', '')
            if '::' in d or ' ' in d:
                continue  # MetaClass / nested
            # must be a defined global vtable (defined, not the 'U' import):
            if img.by_name[n] and (n, img.name) not in res:
                res.append((d, img.name))
    # de-duplicate by class name (family/paravirt disjoint), sorted
    return sorted(set(res))


def build(extra=()):
    sp = Space()
    infos = {}
    cls_list = [c for c, _ in all_classes(sp)]
    # the kernel/IOGraphicsFamily bases pulled in by chains
    want = list(cls_list)
    for c in list(want) + list(extra):
        for b in chain_of(sp, c, infos):
            if b not in want:
                want.append(b)
    model = {'pure_va': sp.pure_va, 'classes': {}}
    vts = {}
    for c in want:
        vts[c] = decode_vtable(sp, c)
    for c in want:
        vt = vts[c]
        if vt is None:
            continue
        ci = infos.get(c) or class_info(sp, c)
        chain = chain_of(sp, c, infos)
        parent = chain[1] if len(chain) > 1 else None
        pvt = vts.get(parent) if parent else None
        pn = pvt['nslots'] if pvt else 0
        rows = []
        for sl in vt['slots']:
            i = sl['i']
            name, aliases = slot_name(sp, sl)
            # introducer: most-basal class in chain with more than i slots
            intro = c
            for b in chain[1:]:
                bv = vts.get(b)
                if bv and bv['nslots'] > i:
                    intro = b
            pslot = pvt['slots'][i] if pvt and i < pn else None
            if pslot is None:
                kind = 'new'
            elif pslot['va'] == sl['va'] and pslot['lvl'] == sl['lvl']:
                kind = 'inherit'
            else:
                kind = 'override'
            rows.append(dict(i=i, off=sl['off'], raw=sl['raw'], va=sl['va'], lvl=sl['lvl'],
                             name=name, aliases=aliases, mangled=sorted(sl['syms']),
                             pure=sl['pure'], intro=intro, kind=kind))
        model['classes'][c] = dict(image=vt['image'], vt_sym=vt['vt_sym'], nslots=vt['nslots'], nterm=vt['nterm'],
                                   size=(ci or {}).get('size'), super=(ci or {}).get('super'),
                                   chain=chain, ctor=(ci or {}).get('ctor'), rows=rows)
    return model


if __name__ == '__main__':
    m = build()
    json.dump(m, open(sys.argv[1], 'w'))
