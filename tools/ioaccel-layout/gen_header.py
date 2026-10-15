#!/usr/bin/env python3
"""Generate $C64_LAYOUT_OUT/IOAccelFamily2_decl.h (+ slots.json) from $C64_LAYOUT_OUT/model.json (written by dump_vtables.py).

For every IOAcceleratorFamily2 class (and IOAccelerator from IOGraphicsFamily) the header
declares, in vtable order, exactly the virtual methods the class INTRODUCES beyond its parent:
  * a real signature for slots that are pure in the class or its parent, and for slots that an
    AppleParavirtGPU subclass overrides (name/arguments recovered from the demangled symbol);
    RETURN TYPES ARE UNKNOWN (C++ mangling omits them): each such method returns
    N48_R_<Class>_<slot>, default uint64_t (rax-compatible superset), overridable by #define;
  * `virtual void _vslotN();` placeholders (N = absolute slot index) for everything else.
Bases IOService / IOUserClient / OSObject come from the MacKernelSDK headers; check_layout.py
proves their slot counts equal the kernel's. Nothing is hand-typed: run dump_vtables.py first.
"""
import json, os, re, sys
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from kcmod import OUT   # regenerating into a different C64_LAYOUT_OUT lets you diff two runs
import dump_vtables as D
import exports as X

SDK_BASES = {'IOService': 'IOKit/IOService.h', 'IOUserClient': 'IOKit/IOUserClient.h', 'OSObject': 'libkern/c++/OSObject.h'}
# Classes of IOGraphicsFamily that a leaf kext class derives from DIRECTLY (e.g. a framebuffer leaf : IOFramebuffer). They are NOT emitted (the SDK declares them); the header only carries their KC size / slot count and a static_assert
# that the SDK's instance size equals the running kernel's (a leaf's members sit after the parent's: a wrong parent size would corrupt memory). The leaf's vtable is proven slot by slot by gates A / the host twin / the runtime gate.
EXTRA_SDK = {'IOFramebuffer': 'IOKit/graphics/IOFramebuffer.h'}
BUILTIN = {'unsigned', 'signed', 'long', 'int', 'short', 'char', 'bool', 'void', 'float', 'double', 'const', 'volatile'}
PARAM_RE = re.compile(r'^(?:(?:const|volatile) )*(?:unsigned |signed )?(?:long long|long|int|short|char|bool|void|float|double|[A-Za-z_]\w*)(?: (?:const|volatile))*(?:\*+(?: const)?)*&?$')


def split_args(a):
    out, depth, cur = [], 0, ''
    for ch in a:
        if ch in '(<[':
            depth += 1
        elif ch in ')>]':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def san(c):
    return re.sub(r'\W', '_', c)


def usable(sig, family):
    """Signature usable as real declaration? returns (ok, list of identifiers, enums, byvalue_unknown)."""
    if sig is None:
        return False, set(), set(), set()
    ids, enums, bad = set(), set(), False
    for p in split_args(sig['args']):
        if p == 'void' or p == '':
            continue
        if not PARAM_RE.match(p) or '...' in p:
            return False, set(), set(), set()
        base = re.sub(r'\b(const|volatile|unsigned|signed|long|int|short|char|bool|void|float|double)\b', '', p).replace('*', '').replace('&', '').strip()
        byval = '*' not in p and '&' not in p
        for t in re.findall(r'[A-Za-z_]\w*', p):
            if t not in BUILTIN:
                ids.add(t)
        if base and byval:
            if re.match(r'^e[A-Z]', base):
                enums.add(base)
            else:
                return False, set(), set(), set()   # by-value struct: cannot declare safely
    return True, ids, enums, set()


# SDK-range slots (IOService / OSObject virtuals) that a FAMILY class overrides. A subclass must inherit the family's implementation
# (e.g. IOGraphicsAccelerator2::start), not IOService's, so each family class redeclares them with the SDK signature and the family's
# exact exported symbol as asm label. Keyed by absolute slot number (identical for OSObject- and IOService-derived classes).
# Compile-checked: `override` fails on a wrong signature, and gate A compares the linked vtable slot by slot.
# Slot numbers are NOT stored here: each entry is looked up by its method signature in the kernel's own decoded IOUserClient vtable
# (model.json), so the indices always come from the user's kernel. The declarations are the public IOKit/libkern API.
SDK_OVR_BY_NAME = {
    'getRetainCount() const': 'int getRetainCount() const', 'retain() const': 'void retain() const',
    'attach(IOService*)': 'bool attach(IOService *)',
    'externalMethod(unsigned int, IOExternalMethodArguments*, IOExternalMethodDispatch*, OSObject*, void*)':
        'IOReturn externalMethod(uint32_t, IOExternalMethodArguments *, IOExternalMethodDispatch *, OSObject *, void *)',
    'clientClose()': 'IOReturn clientClose()', 'connectClient(IOUserClient*)': 'IOReturn connectClient(IOUserClient *)',
    'clientMemoryForType(unsigned int, unsigned int*, IOMemoryDescriptor**)': 'IOReturn clientMemoryForType(UInt32, IOOptionBits *, IOMemoryDescriptor **)',
    'getTargetAndMethodForIndex(IOService**, unsigned int)': 'IOExternalMethod *getTargetAndMethodForIndex(IOService **, UInt32)',
    'release() const': 'void release() const', 'free()': 'void free()', 'setProperties(OSObject*)': 'IOReturn setProperties(OSObject *)',
    'requestTerminate(IOService*, unsigned int)': 'bool requestTerminate(IOService *, IOOptionBits)',
    'didTerminate(IOService*, unsigned int, bool*)': 'bool didTerminate(IOService *, IOOptionBits, bool *)',
    'configureReport(IOReportChannelList*, unsigned int, void*, void*)': 'IOReturn configureReport(IOReportChannelList *, IOReportConfigureAction, void *, void *)',
    'updateReport(IOReportChannelList*, unsigned int, void*, void*)': 'IOReturn updateReport(IOReportChannelList *, IOReportConfigureAction, void *, void *)',
    'start(IOService*)': 'bool start(IOService *)', 'stop(IOService*)': 'void stop(IOService *)',
    'terminate(unsigned int)': 'bool terminate(IOOptionBits)', 'finalize(unsigned int)': 'bool finalize(IOOptionBits)',
    'getWorkLoop() const': 'IOWorkLoop *getWorkLoop() const', 'requestProbe(unsigned int)': 'IOReturn requestProbe(IOOptionBits)',
    'message(unsigned int, IOService*, void*)': 'IOReturn message(UInt32, IOService *, void *)',
    'newUserClient(task*, void*, unsigned int, IOUserClient**)': 'IOReturn newUserClient(task_t, void *, UInt32, IOUserClient **)',
}
# dtors (D0/D1) and getMetaClass: OSDeclareAbstractStructors declares them (natural symbol == the family's). Resolved by name too.
SDK_SKIP_NAMES = ('~', 'getMetaClass() const')


def resolve_sdk_slots(C):
    """-> (SDK_OVR {slot: declaration}, SDK_SKIP {slot}) derived from the running kernel's IOUserClient vtable."""
    ovr, skip, found = {}, set(), set()
    for r in C['IOUserClient']['rows']:
        tail = r['name'].split('::', 1)[1] if '::' in r['name'] else r['name']
        if tail in SDK_OVR_BY_NAME:
            ovr[r['i']] = SDK_OVR_BY_NAME[tail]
            found.add(tail)
        if tail.startswith(SDK_SKIP_NAMES[0]) or tail == SDK_SKIP_NAMES[1]:
            skip.add(r['i'])
    missing = set(SDK_OVR_BY_NAME) - found
    assert not missing, 'SDK methods not found in the kernel IOUserClient vtable (macOS version too different?): %s' % sorted(missing)
    return ovr, skip


def label_for(c, i, row):
    """asm label for the declaration of slot i introduced/overridden in class c: the family's EXACT exported symbol for this class's
    vtable entry; else a never-defined marker (gate B fails the build if any of our vtables still references it)."""
    m = X.exported_name(row['mangled'])
    return m if m else '__n48_nonexported_%s_%d' % (san(c), i)


def main():
    global SDK_OVR, SDK_SKIP
    m = json.load(open(os.path.join(OUT, 'model.json')))
    C = m['classes']
    SDK_OVR, SDK_SKIP = resolve_sdk_slots(C)
    emit = [c for c, v in C.items() if v['size'] and v['super'] and (v['image'] == 'IOAcceleratorFamily2' or c == 'IOAccelerator')]
    # topological: parents first
    order, seen = [], set()

    def visit(c):
        if c in seen or c not in emit:
            return
        seen.add(c)
        p = C[c]['super']
        if p in emit:
            visit(p)
        order.append(c)
    for c in sorted(emit):
        visit(c)
    for c in order:
        p = C[c]['super']
        assert p in emit or p in SDK_BASES, 'unsupported base %s of %s' % (p, c)

    para = {}   # class -> set(slot) overridden by any paravirt descendant vs the class's own target
    for c in order:
        s = set()
        for d in D.descendants(m, c):
            if C[d]['image'] != 'AppleParavirtGPU':
                continue
            for r in C[d]['rows']:
                if r['i'] < C[c]['nslots'] and (r['va'], r['lvl']) != (C[c]['rows'][r['i']]['va'], C[c]['rows'][r['i']]['lvl']):
                    s.add(r['i'])
        para[c] = s

    ids_needed, enums_needed = set(), set()
    body = []
    slots_json = {}
    rep = {'real': 0, 'placeholder': 0, 'downgraded': []}
    declared_sigs = {}   # class -> set of (name,args,cv) declared along its chain
    declmap = {}         # class -> {slot: declaration entry visible in that class}
    for c in order:
        v = C[c]
        p = v['super']
        pn = C[p]['nslots'] if p in C else None
        psz = C[p]['size']
        chain_sigs = set(declared_sigs.get(p, set())) if p in emit else set()
        decls = []
        js = {}
        dm = dict(declmap.get(p, {})) if p in emit else {}
        k_new = 0
        sdk = next(b for b in v['chain'] if b in SDK_BASES)
        nsdk = C[sdk]['nslots']
        for r in v['rows']:
            i = r['i']
            parent_pure = (p in C and i < pn and C[p]['rows'][i]['pure'])
            is_new = i >= pn
            redeclare = parent_pure and not r['pure']       # implemented here, pure above
            differs = (not is_new) and (r['va'], r['lvl']) != (C[p]['rows'][i]['va'], C[p]['rows'][i]['lvl'])
            lab = label_for(c, i, r)
            asm = ' __asm__("%s")' % lab
            if i < nsdk and differs and i not in SDK_SKIP:
                # SDK-range slot this family class overrides: redeclare it with the SDK signature and the family's exact symbol
                if i not in SDK_OVR:
                    rep.setdefault('sdk_unhandled', []).append('%s slot %d (%s)' % (c, i, r['name']))
                    continue
                decls.append('    virtual %s override%s;   // slot %d  (SDK-range override, family symbol %s)' % (
                    SDK_OVR[i], asm, i, lab))
                js[i] = dict(kind='sdk', name=SDK_OVR[i], args='', cv='', pure=False, new=False, owner=c, label=lab)
                dm[i] = js[i]
                continue
            if not (is_new or redeclare):
                if differs and i >= nsdk and not r['pure'] and i in dm:
                    # family slot re-implemented here (not pure above): redeclare the visible declaration, family's exact symbol
                    e = dict(dm[i], pure=False, new=False, label=lab)
                    if e['kind'] == 'real':
                        macro = 'N48_R_%s_%d' % (san(e['owner']), i)
                        decls.append('    virtual %s %s(%s)%s override%s;   // slot %d  (re-implemented here)' % (macro, e['name'], e['args'], e['cv'], asm, i))
                    else:
                        decls.append('    virtual void _vslot%d() override%s;   // slot %d  (re-implemented here)' % (i, asm, i))
                    js[i] = e
                    dm[i] = e
                continue
            pure_txt = ' = 0' if r['pure'] else ''
            asm_u = '' if r['pure'] else asm
            if redeclare and i in dm:
                # implemented here but pure above: re-declare EXACTLY the parent's declaration (an override, not a new slot)
                e = dict(dm[i], pure=False, new=False, label=lab)
                ok = e['kind'] == 'real'
                if ok:
                    macro = 'N48_R_%s_%d' % (san(e['owner']), i)
                    decls.append('    virtual %s %s(%s)%s override%s;   // slot %d  (implemented here, pure in %s)' % (
                        macro, e['name'], e['args'], e['cv'], asm, i, e['owner']))
                else:
                    decls.append('    virtual void _vslot%d() override%s;   // slot %d (implemented here, pure above)' % (i, asm, i))
                js[i] = e
                dm[i] = e
                continue
            want_real = r['pure'] or parent_pure or (i in para[c])
            sig = D.slot_signature(m, c, i) if want_real else None
            ok, ids, enums, _ = usable(sig, emit)
            if want_real and ok:
                key = (sig['name'], sig['args'], sig['cv'] or '')
                if key in chain_sigs:
                    ok = False
                    rep['downgraded'].append('%s slot %d duplicate signature %s(%s)' % (c, i, sig['name'], sig['args']))
            elif want_real and not ok:
                rep['downgraded'].append('%s slot %d: no usable signature (%s)' % (c, i, sig['name'] + '(' + sig['args'] + ')' if sig else 'no descendant'))
            if ok:
                chain_sigs.add(key)
                ids_needed |= ids
                enums_needed |= enums
                macro = 'N48_R_%s_%d' % (san(c), i)
                decls.append('    virtual %s %s(%s)%s%s%s;   // slot %d  [%s]' % (
                    macro, sig['name'], sig['args'], sig['cv'] or '', asm_u, pure_txt, i, sig['source']))
                js[i] = dict(kind='real', name=sig['name'], args=sig['args'], cv=sig['cv'] or '', pure=r['pure'], new=is_new, owner=c, label=lab)
                rep['real'] += 1
            else:
                decls.append('    virtual void _vslot%d()%s%s;   // slot %d' % (i, asm_u, pure_txt, i))
                js[i] = dict(kind='placeholder', name='_vslot%d' % i, args='', cv='', pure=r['pure'], new=is_new, owner=c, label=lab)
                rep['placeholder'] += 1
            dm[i] = js[i]
            if is_new:
                k_new += 1
        declared_sigs[c] = chain_sigs
        declmap[c] = dm
        slots_json[c] = dict(super=p, size=v['size'], nslots=v['nslots'], parent_nslots=pn, parent_size=psz, slots=js)
        pad = v['size'] - psz
        assert pad >= 0 and pad % 8 == 0, (c, pad)
        b = []
        b.append('// ---- %s : %s   size 0x%x   %d slots (%d new, parent %d)   [%s]' % (c, p, v['size'], v['nslots'], k_new, pn, v['image']))
        b.append('#define N48_%s_SIZE 0x%xu' % (san(c), v['size']))
        b.append('#define N48_%s_NSLOTS %d' % (san(c), v['nslots']))
        b.append('class %s : public %s {' % (c, p))
        b.append('    OSDeclareAbstractStructors(%s)' % c)
        b.append('public:')
        b.extend(decls)
        if pad:
            b.append('private:')
            b.append('    alignas(8) uint8_t _n48_pad_%s[0x%x];' % (san(c), pad))
        b.append('};')
        b.append('static_assert(sizeof(%s) == N48_%s_SIZE, "instance size of %s differs from the KC");' % (c, san(c), c))
        b.append('static_assert(N48_%s_NSLOTS == N48_%s_NSLOTS + %d, "slot arithmetic for %s");' % (san(c), san(p), k_new, c))
        b.append('')
        body.append('\n'.join(b))

    # ---- forwarders: slots whose implementation the family does NOT export. A leaf subclass cannot reference them, so it overrides
    # them with a naked tail-jump into the parent's own vtable slot (read at run time from the exported __ZTV<Parent>): same behaviour
    # as inheriting, no unresolved symbol. N48_FWD_DECLS_<P> goes in the leaf class body; N48_FWD_DEFS_<P>(Leaf) at file scope.
    fwdlist = {}
    fwd_macros = []
    for c in order:
        v = C[c]
        sdk = next(b for b in v['chain'] if b in SDK_BASES)
        items = []
        for r in v['rows']:
            i = r['i']
            if r['pure'] or i in SDK_SKIP or X.exported_name(r['mangled']):
                continue
            if i < C[sdk]['nslots']:
                if i not in SDK_OVR:
                    rep.setdefault('sdk_unhandled', []).append('%s slot %d NOT EXPORTED (%s)' % (c, i, r['name']))
                    continue
                txt = SDK_OVR[i]
            else:
                e = declmap[c].get(i)
                if e is None:
                    continue
                ret = ('N48_R_%s_%d' % (san(e['owner']), i)) if e['kind'] == 'real' else 'void'
                txt = '%s %s(%s)%s' % (ret, e['name'], e['args'], e['cv'])
            items.append((i, txt))
        if not items:
            continue
        fwdlist[c] = [i for i, _ in items]
        vt = '__ZTV%d%s' % (len(c), c)
        L = ['#define N48_FWD_DECLS_%s \\' % san(c)]
        for k, (i, txt) in enumerate(items):
            L.append('    virtual %s override; /* slot %d: family symbol not exported -> forwarded */ \\' % (txt, i))
        L[-1] = L[-1].rstrip(' \\')
        fwd_macros.append('\n'.join(L))
        L = ['#define N48_FWD_DEFS_%s(Leaf) \\' % san(c)]
        for k, (i, txt) in enumerate(items):
            head, rest = txt.split('(', 1)
            name = re.search(r'(\w+)\s*$', head).group(1)
            ret = head[:head.rfind(name)]
            L.append('    __attribute__((naked)) %sLeaf::%s(%s { __asm__("movq %s@GOTPCREL(%%rip), %%rax\\n\\tmovq %d(%%rax), %%rax\\n\\tjmpq *%%rax"); } \\' % (ret, name, rest, vt, 16 + 8 * i))
        L[-1] = L[-1].rstrip(' \\')
        fwd_macros.append('\n'.join(L))
    json.dump(fwdlist, open(os.path.join(OUT, 'forwarded.json'), 'w'), indent=1, sort_keys=True)
    if rep.get('sdk_unhandled'):
        print('SDK-range overrides without a SDK_OVR entry (classes using them are not supported by the leaf build):')
        for x in sorted(set(rep['sdk_unhandled'])):
            print('   ' + x)

    # parents from the SDK have their size/slot constants defined from the KC too
    pre = []
    for sdk in sorted(SDK_BASES) + sorted(EXTRA_SDK):
        pre.append('#define N48_%s_SIZE 0x%xu\n#define N48_%s_NSLOTS %d' % (san(sdk), C[sdk]['size'], san(sdk), C[sdk]['nslots']))
        pre.append('static_assert(sizeof(%s) == N48_%s_SIZE, "SDK %s size differs from the running kernel");' % (sdk, san(sdk), sdk))

    fam_classes = set(order)
    fwd = sorted(t for t in ids_needed if t not in fam_classes and t not in enums_needed and t not in SDK_BASES)
    hdr = []
    hdr.append('// GENERATED by gen_header.py from model.json (decoded from the local kernel collection). DO NOT EDIT. Apple-derived: do not redistribute.')
    hdr.append('// Reconstructs the virtual-method ORDER (vtable layout) and instance sizes of IOAcceleratorFamily2 (macOS Tahoe, x86_64)')
    hdr.append('// for -fapple-kext subclasses without Apple headers. Vtable slot N of a class sits at vtable + 16 + 8*N.')
    hdr.append('// Real names/signatures: from AppleParavirtGPU overrides (SUSPECTED for slot identity, CONFIRMED for slot index).')
    hdr.append('// Return types are UNKNOWN (mangling omits them): N48_R_<Class>_<slot> defaults to uint64_t; define it before including to change.')
    hdr.append('#ifndef N48_IOACCELFAMILY2_DECL_H\n#define N48_IOACCELFAMILY2_DECL_H')
    hdr.append('#include <stdint.h>\n#include <IOKit/IOService.h>\n#include <IOKit/IOUserClient.h>\n#include <libkern/c++/OSObject.h>\n#include <IOKit/graphics/IOFramebuffer.h>')
    hdr.append('#pragma clang diagnostic push\n#pragma clang diagnostic ignored "-Wmismatched-tags"\n#pragma clang diagnostic ignored "-Woverloaded-virtual"')
    hdr.append('')
    hdr.extend(pre)
    hdr.append('')
    hdr.append('// forward declarations of argument types')
    for t in sorted(enums_needed):
        hdr.append('enum %s : unsigned int;' % t)
    for t in fwd:
        hdr.append('class %s;' % t)
    for c in order:
        hdr.append('class %s;' % c)
    hdr.append('')
    # return-type macros (defaults)
    macros = []
    for c in order:
        for i, e in slots_json[c]['slots'].items():
            if e['kind'] == 'real':
                macros.append('N48_R_%s_%d' % (san(c), i))
    hdr.append('// return type per real-signature slot (UNKNOWN; default uint64_t)')
    for mname in macros:
        hdr.append('#ifndef %s\n#define %s uint64_t\n#endif' % (mname, mname))
    hdr.append('')
    hdr.extend(body)
    hdr.append('// ---- forwarders for slots the family does not export (see gen_header.py)')
    hdr.extend(fwd_macros)
    hdr.append('')
    hdr.append('#pragma clang diagnostic pop\n#endif')
    text = '\n'.join(hdr)
    open(os.path.join(OUT, 'IOAccelFamily2_decl.h'), 'w').write(text + '\n')
    json.dump(dict(order=order, classes=slots_json), open(os.path.join(OUT, 'slots.json'), 'w'), indent=0, sort_keys=True)
    print('classes emitted: %d, real-signature slots: %d, placeholders: %d, downgraded: %d' % (len(order), rep['real'], rep['placeholder'], len(rep['downgraded'])))
    with open(os.path.join(OUT, 'downgraded.txt'), 'w') as f:
        f.write('\n'.join(rep['downgraded']) + '\n')


if __name__ == '__main__':
    main()
