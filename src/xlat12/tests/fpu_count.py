#!/usr/bin/env python3
"""Count SSE/x87 instructions in Mach-O x86_64 objects, skipping data-in-code (0.0.218, an earlier analysis).

kextcheck's old count was `llvm-objdump -d | grep -c 'xmm|ymm|zmm|%st'`. A linear sweep also "decodes" switch jump
tables that clang places inside __text (LC_DATA_IN_CODE, kind JUMP_TABLE32): 0.0.218's d_reg table entry 0xffffeedd
disassembles as `dd ee  fucomp %st(6)`, a false positive. This counts only lines whose address lies outside every
data-in-code range llvm-objdump reports for the object, and prints on stderr how many pattern lines were excluded,
so an exclusion can never hide silently.

usage: fpu_count.py <llvm-objdump> <obj.o> [...]   -> stdout: the count; stderr: per-object detail
"""
import re
import subprocess
import sys

PAT = re.compile(r'xmm|ymm|zmm|%st')
INSN = re.compile(r'^\s*([0-9a-f]+):\s')
DIC = re.compile(r'^0x([0-9a-f]+)\s+(\d+)\s+\S+')


def ranges(od, obj):
    out = subprocess.run([od, '--macho', '--data-in-code', obj], capture_output=True, text=True, check=True).stdout
    r = []
    for line in out.splitlines():
        m = DIC.match(line.strip())
        if m:
            start = int(m.group(1), 16)
            r.append((start, start + int(m.group(2))))
    return r


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    od, objs = sys.argv[1], sys.argv[2:]
    total = 0
    for obj in objs:
        rs = ranges(od, obj)
        dis = subprocess.run([od, '-d', obj], capture_output=True, text=True, check=True).stdout
        real = excluded = 0
        for line in dis.splitlines():
            m = INSN.match(line)
            if not m or not PAT.search(line):
                continue
            a = int(m.group(1), 16)
            if any(lo <= a < hi for lo, hi in rs):
                excluded += 1
                print(f'  {obj}: data-in-code, not counted: {line.strip()}', file=sys.stderr)
            else:
                real += 1
                print(f'  {obj}: SSE/x87: {line.strip()}', file=sys.stderr)
        print(f'{obj}: {len(rs)} data-in-code range(s); SSE/x87 lines {real}, excluded as data {excluded}', file=sys.stderr)
        total += real
    print(total)
    return 0


if __name__ == '__main__':
    sys.exit(main())
