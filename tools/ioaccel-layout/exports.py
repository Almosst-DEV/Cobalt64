"""Exported (global, defined) symbols of the images a leaf kext links against:
IOAcceleratorFamily2 (the carved family kext), the Boot KC (kernel + boot kexts) and the System KC (IOGraphicsFamily,
IOSurface, ...). A vtable slot may only be REFERENCED by name if its symbol is in this set;
a slot whose family symbol is not exported must be overridden in the leaf class (see gen_header.py, N48_FWD_*)."""
import os, subprocess
from kcmod import FAMILY, SYS_KC, BOOT_KC

_cache = None


def load():
    global _cache
    if _cache is not None:
        return _cache
    ex = {}
    for tag, p in (('family', FAMILY), ('boot', BOOT_KC), ('system', SYS_KC)):
        for l in subprocess.run(['nm', '-gU', p], capture_output=True, text=True).stdout.splitlines():
            q = l.split(' ', 2)
            if len(q) == 3:
                ex.setdefault(q[2], tag)
    _cache = ex
    return ex


def exported_name(mangled_list):
    ex = load()
    for m in mangled_list:
        if m in ex:
            return m
    return None
