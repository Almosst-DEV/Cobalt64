"""Shared Mach-O / kernel-collection reader for the ioaccel-layout tools.

Vtable slots in a KC are chained fixups (DYLD_CHAINED_PTR_X86_64_KERNEL_CACHE):
  bits 0-29  target offset, bits 30-31 cache level (0 = boot KC / kernel,
  1 = system KC), higher bits: diversity / next / auth flags.
Target VA = offset (level 1: system KC vmaddrs are 0-based) or
KERNEL_BASE + offset (level 0: the kernel, whose own vtables hold plain VAs).
Nothing here is hand-typed per class.

Inputs come from the USER'S OWN macOS install (nothing Apple-derived ships with this tool):
  $C64_WORK/kc/BootKernelExtensions.kc, $C64_WORK/kc/SystemKernelExtensions.kc
  $C64_WORK/kexts/com.apple.iokit.IOAcceleratorFamily2, $C64_WORK/kexts/com.apple.driver.AppleParavirtGPU
    (the two kexts carved out of the kernel collection as standalone Mach-O files)
Outputs go to $C64_LAYOUT_OUT (default ./layout-out) and must NOT be redistributed (Apple-derived).
"""
import struct, subprocess, bisect, os, sys

KERNEL_BASE = 0xffffff8000100000   # level-0 target offset base (verified: pure-virtual offset 0xab4820 -> ___cxa_pure_virtual at 0xffffff8000bb4820)
WORK = os.environ.get('C64_WORK')
if not WORK:
    sys.exit('set C64_WORK to the directory holding kc/ and kexts/ (see README.md)')
WORK = os.path.expanduser(WORK)
OUT = os.path.abspath(os.environ.get('C64_LAYOUT_OUT') or 'layout-out')
KEXTDIR = WORK + '/kexts'
BOOT_KC = WORK + '/kc/BootKernelExtensions.kc'
SYS_KC = WORK + '/kc/SystemKernelExtensions.kc'
FAMILY = KEXTDIR + '/com.apple.iokit.IOAcceleratorFamily2'
PARAVIRT = KEXTDIR + '/com.apple.driver.AppleParavirtGPU'


def demangle(names):
    """Batch c++filt (names keep their leading underscore -> use -_)."""
    if not names:
        return []
    out = subprocess.run(['c++filt', '-_'], input='\n'.join(names) + '\n',
                         capture_output=True, text=True).stdout.split('\n')
    return out[:len(names)]


class Image:
    """One Mach-O (carved kext or whole KC): segments + symbols by VA."""

    def __init__(self, path, name=None, va_bias=0):
        self.path = path
        self.name = name or os.path.basename(path)
        self.bias = va_bias          # subtract from nm VA to get the offset space
        self.f = open(path, 'rb').read()
        n = struct.unpack_from('<I', self.f, 16)[0]
        off = 32
        self.segs = []
        for _ in range(n):
            cmd, sz = struct.unpack_from('<II', self.f, off)
            if cmd == 0x19:
                vm, vs, fo, fs = struct.unpack_from('<QQQQ', self.f, off + 24)
                self.segs.append((vm, vs, fo, fs))
            off += sz
        self.syms = []
        for l in subprocess.run(['nm', '-n', path], capture_output=True, text=True).stdout.splitlines():
            p = l.split(' ', 2)
            if len(p) == 3 and p[0].strip() and p[1] in 'STtDdsdbB':
                self.syms.append((int(p[0], 16) - va_bias, p[2]))
        self.syms.sort()
        self.addrs = [a for a, _ in self.syms]
        self.by_name = {}
        for a, n_ in self.syms:
            self.by_name.setdefault(n_, a)

    def rd64(self, va):
        for vm, vs, fo, fs in self.segs:
            va2 = va + self.bias
            if vm <= va2 < vm + fs:
                return struct.unpack_from('<Q', self.f, fo + va2 - vm)[0]
        return None

    def sym_at(self, va):
        i = bisect.bisect_right(self.addrs, va) - 1
        if i >= 0:
            return self.syms[i]
        return None

    def sym_exact(self, va):
        s = self.sym_at(va)
        return s[1] if s and s[0] == va else None

    def vtable_end(self, name):
        """End of a vtable = next symbol above it (vtables are contiguous S syms)."""
        a = self.by_name[name]
        i = bisect.bisect_right(self.addrs, a)
        return self.addrs[i] if i < len(self.addrs) else a + 0x2000

    def rdbytes(self, va, n):
        for vm, vs, fo, fs in self.segs:
            va2 = va + self.bias
            if vm <= va2 < vm + fs:
                return self.f[fo + va2 - vm: fo + va2 - vm + n]
        return None

    def cstr(self, va):
        b = self.rdbytes(va, 128)
        return b.split(b'\0')[0].decode() if b else None
