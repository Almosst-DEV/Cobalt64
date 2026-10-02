"""The leaf classes of the Navi48Accel aux kext: one place, used by gen_gate.py (runtime gate table), gate_link.py (gates A/B) and layout_gate_host.py
(the host twin). (leaf, family parent, extra non-pure slots the leaf overrides itself, by demangled row-name prefix or absolute slot)."""
LEAVES = [
    ('Navi48Accelerator',   'IOGraphicsAccelerator2',   [183, 184, 18, 322, 348, 329]),   # IOService::probe, ::start, ::free, newSharedUserClient, newCommandQueue, newDisplayPipe (aux 0.0.3)
    ('Navi48EventMachine',  'IOAccelEventMachineFast2', [35]),             # init (the hooked slots are added from TRAMP)
    ('Navi48Task',          'IOAccelTask',              []),
    ('Navi48DisplayMachine','IOAccelDisplayMachine',    [267]),            # start(IOPCIDevice*) (aux 0.0.3: the framebuffer walk starts at the GPU's PCI device when display is on)
    ('Navi48SysMemory',     'IOAccelSysMemory',         []),
    ('Navi48MemoryMap',     'IOAccelMemoryMap',         []),
    ('Navi48VidMemory',     'IOAccelVidMemory',         []),
    ('Navi48Resource',      'IOAccelResource2',         []),
    ('Navi482DContext',     'IOAccel2DContext2',        []),
    ('Navi48SharedUserClient', 'IOAccelSharedUserClient2', []),
    ('Navi48CommandQueue',  'IOAccelCommandQueue',      []),
    ('Navi48DisplayPipe',   'IOAccelDisplayPipe',       [277]),            # aux 0.0.3: performTransaction, hand-written (its generated declaration is a placeholder)
]
# Trampoline leaves (gates/gen_tramp.py): every listed slot is overridden by a generated function that first asks the ops table's generic vhook and otherwise takes its
# default. mode: 'zero' = 0/false/NULL (pure slots: there is no base), a number = that constant, 'base' = the family's own implementation (read from its vtable).
# An optional third element names the hook function the trampolines ask (default n48_vhook, the ops table's generic vhook); the display pipe asks n48_disp_vhook,
# which calls the ABI-2 disp_hook only while display is on (n48accel_pure.h disp_enabled) and otherwise reports "not handled" (-> the family's own slot).
# The extra column of LEAVES above is completed from here (a hooked slot is an override).
TRAMP = {
    'Navi48EventMachine':      ('N48_VC_EVENTMACHINE', {68: 'base', 72: 'zero', 73: 'zero', 84: 'zero', 85: 'zero', 86: '1', 87: 'zero'}),
    'Navi48SharedUserClient':  ('N48_VC_SHAREDUC',     {266: 'base'}),
    'Navi48CommandQueue':      ('N48_VC_CMDQUEUE',     {300: 'base', 301: 'base', 302: 'base', 303: 'base', 305: 'base', 316: 'base', 321: 'base', 326: 'base', 335: 'base', 336: 'base'}),
    'Navi48VidMemory':         ('N48_VC_VIDMEMORY',    {43: 'zero', 61: 'zero', 62: 'zero'}),
    'Navi48Resource':          ('N48_VC_RESOURCE',     {58: 'zero', 60: 'zero', 61: 'zero', 62: 'zero', 63: 'zero', 64: 'zero', 65: 'zero'}),
    'Navi482DContext':         ('N48_VC_CTX2D',        {360: 'zero', 361: 'zero'}),
    # aux 0.0.3: initFramebufferResource, isTransactionComplete, submitTransaction. Slot 277 (performTransaction) is declared by the generated header as the
    # placeholder _vslot277() (no paravirt descendant implements it, so gen_header.py has no signature for it): it is HAND-written (a naked tail jump).
    'Navi48DisplayPipe':       ('N48_VC_DISPLAYPIPE',  {267: 'base', 278: 'base', 279: 'base'}, 'n48_disp_vhook'),
}
# slots overridden by hand in Navi48Accel.cpp (not generated), per tramp leaf
HAND = {'Navi48EventMachine': [35], 'Navi48DisplayPipe': [277]}

# The totals the gates must see (gate_link.py gate A and the host twin fail otherwise; the runtime gate logs "layout PASS <slots>/<slots>").
# aux 0.0.2: 11 classes / 2064 slots; aux 0.0.3 adds Navi48DisplayPipe vs IOAccelDisplayPipe (306 slots, size 0x318, 0 pure virtuals): 12 / 2370.
EXPECT_CLASSES = 12
EXPECT_SLOTS = 2370
# The negative controls of the host twin (layout_gate_host.py): for this class the twin re-runs the gate on corrupted copies of the tables and every one must FAIL.
NEGATIVE_CLASS = 'Navi48DisplayPipe'

# slots every OSDefineMetaClassAndStructors class overrides: the two destructors and getMetaClass
OSDEFINE_SLOTS = [0, 1, 7]
