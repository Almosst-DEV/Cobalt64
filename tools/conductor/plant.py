#!/usr/bin/env python3
"""plant.py: run the reviewer's planted breaks against a build, one plant at a time.

usage:
  tools/conductor/plant.py --tree <tree-root> --work <scratch-dir> <plants-file> [<name-prefix> ...]
  tools/conductor/plant.py --tree <tree-root> --work <scratch-dir> <plants-file> --check
  tools/conductor/plant.py --tree <tree-root> --work <scratch-dir> <plants-file> --list

<plants-file> is a Python file that defines
  PLANTS = [(name, path, old, new, [suite, ...]), ...]
where `path` is relative to <tree-root> (or absolute), `old` must occur exactly once in the file,
and `new` replaces it. It may also define or extend SUITES = {name: (build_cmd, run_cmd)}.
These names are predefined for it: WT (tree root), A, T, X, AHH (source dirs and the hook file,
absolute), OUT (<scratch-dir>/bin), SAN, W, Wn (the compile flags the host suites use), and
SUITES (the default table below, which it may extend).

For each selected plant: copy the file to <scratch-dir>/bak/<path with / as __>.bak, plant, build
and run each named suite, then restore from the .bak in `finally`, prove the restore with a byte
compare, and delete the .bak. The run prints `git diff | md5` of the tree before and after.
Verdict per plant: CAUGHT (a suite built and exited non-zero), NOT CAUGHT (every suite built and
exited 0), or INCONCLUSIVE (a suite failed to build or timed out and none caught it).

STARTUP CHECK (why: on an earlier run a killed plant run left a break planted in xlat12_ib.c,
because `finally` does not run on SIGKILL): before planting anything, the runner refuses if
  - a .bak for any known target remains and the file matches neither that .bak nor its git HEAD
    version (an interrupted run, or an edit since: resolve by hand, then delete the .bak), or
  - any plant's `new` text is present in its file where the pristine file could not hold it
    (a break left planted), or a selected plant's `old` text is absent (planted, or the code moved).
A leftover .bak whose file equals it is deleted; one whose file equals git HEAD is reported as
stale and ignored. `--check` runs only this check. SIGTERM and SIGHUP are turned into a normal
exit so `finally` restores; SIGKILL cannot be caught, which is what the check is for.

Run it one plant at a time when delegating (pass one name prefix per invocation), keep the work
dir per build (for example <scratchpad>/plant448), and never run two instances on one tree.
"""
import argparse
import filecmp
import hashlib
import os
import runpy
import shutil
import signal
import subprocess
import sys

SUITE_TIMEOUT_S = 900


def default_suites(ns):
    f = lambda s: s.format(**ns)  # noqa: E731
    return {
        'flightring': (f('clang++ {W} {SAN} -I {A} {T}gfx_flightring_test.cpp -o {OUT}/fr'), f('{OUT}/fr')),
        'keystone':   (f('clang++ {W} {SAN} -I {A} {T}gfx_keystone_test.cpp -o {OUT}/ks'), f('{OUT}/ks {AHH}')),
        'desc_port':  (f('clang++ {W} {SAN} -I {A} {T}gfx_desc_port_test.cpp -o {OUT}/dp'), f('{OUT}/dp {AHH}')),
        'dep':        (f('clang++ {W} {SAN} -I {A} -I {X} -I {X}tests -x c++ {T}gfx_dep_test.cpp {X}xlat12.c {X}xlat12_ib.c -o {OUT}/dep'), f('{OUT}/dep {AHH}')),
        'mib':        (f('clang++ {W} {SAN} -I {A} -I {X} -x c++ {T}gfx_mib_test.cpp {X}xlat12_ib.c {X}xlat12.c -o {OUT}/mib'), f('{OUT}/mib {AHH}')),
        'memdst':     (f('clang++ {W} {SAN} -I {A} -I {X} -I {X}tests -x c++ {T}gfx_memdst_test.cpp {X}xlat12.c {X}xlat12_ib.c {X}xlat12_headless.c -o {OUT}/md'), f('{OUT}/md')),
        'xlat':       (f('make -s -C {X} BUILD={OUT}/x12 {OUT}/x12/test_xlat12_ib'), f('cd {X} && {OUT}/x12/test_xlat12_ib')),
        'capscan':    (f('clang++ {Wn} -fsanitize=address,undefined -I {A} {T}gfx_capture_scan_test.cpp -o {OUT}/cs'), f('{OUT}/cs')),
        'mmprio':     (f('clang++ {W} {SAN} -I {A} {T}gfx_mmprio_test.cpp -o {OUT}/mm'), f('{OUT}/mm {WT}/src/navi48-bringup/src/Navi48Bringup.cpp {AHH} {A}gfx_desc_port.h {X}xlat12_ib.c')),
    }


def sh(cmd, cwd, timeout=None):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, cwd=cwd, timeout=timeout)


def diff_md5(tree):
    r = subprocess.run(['git', '-C', tree, 'diff'], capture_output=True)
    return hashlib.md5(r.stdout).hexdigest()


def head_bytes(tree, path):
    rel = os.path.relpath(path, tree)
    if rel.startswith('..'):
        return None
    r = subprocess.run(['git', '-C', tree, 'show', 'HEAD:' + rel], capture_output=True)
    return r.stdout if r.returncode == 0 else None


def bak_name(tree, bakdir, path):
    rel = os.path.relpath(path, tree)
    if rel.startswith('..'):
        rel = path.lstrip('/')
    return os.path.join(bakdir, rel.replace('/', '__') + '.bak')


def startup_check(tree, work, plants, selected):
    bakdir = os.path.join(work, 'bak')
    problems = []
    warn = lambda m: print('check: WARNING ' + m)  # noqa: E731
    targets = sorted({p[1] for p in plants})
    for path in targets:
        # the runner's own .bak, plus the legacy scratchpad name (basename.bak in <work>/)
        for bak in (bak_name(tree, bakdir, path), os.path.join(work, os.path.basename(path) + '.bak')):
            if not os.path.exists(bak):
                continue
            if not os.path.exists(path):
                problems.append(f'{path}: missing, but {bak} exists')
                continue
            if filecmp.cmp(bak, path, shallow=False):
                if bak.startswith(bakdir):
                    os.remove(bak)
                    print(f'check: {os.path.basename(path)} equals its leftover .bak; .bak removed')
                continue
            cur = open(path, 'rb').read()
            if head_bytes(tree, path) == cur:
                print(f'check: {bak} is stale (the file equals git HEAD); ignored')
                continue
            problems.append(f'{path}: differs from its leftover {bak} AND from git HEAD '
                            f'(an interrupted plant, or an edit since the .bak was taken)')
    for name, path, old, new, _ in plants:
        sel = name in selected
        if not os.path.exists(path):
            (problems.append if sel else warn)(f'[{name}] target does not exist: {path}')
            continue
        src = open(path).read()
        has_old = old in src
        has_new = bool(new) and new not in old and new in src
        if has_new and (not has_old or old in new):
            problems.append(f'[{name}] its planted text is IN {os.path.relpath(path, tree)}: '
                            f'a break was left planted')
        elif has_new:
            warn(f'[{name}] its planted text also occurs in {os.path.relpath(path, tree)} beside the '
                 f'original; check by hand')
        elif not has_old:
            (problems.append if sel else warn)(
                f'[{name}] its original text is absent from {os.path.relpath(path, tree)} '
                f'(planted, or the code moved: fix the plant)')
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--tree', required=True, help='the checkout or worktree under test')
    ap.add_argument('--work', required=True, help='scratch dir for binaries and .bak files (outside the tree)')
    ap.add_argument('--check', action='store_true', help='run the startup check only')
    ap.add_argument('--list', action='store_true', help='list the plants and exit')
    ap.add_argument('plants_file')
    ap.add_argument('only', nargs='*', help='run only plants whose name starts with one of these')
    args = ap.parse_args()

    tree = os.path.realpath(args.tree)
    work = os.path.realpath(args.work)
    if (work + '/').startswith(tree + '/'):
        sys.exit(f'refusing: --work {work} is inside the tree')
    out = os.path.join(work, 'bin')
    os.makedirs(out, exist_ok=True)
    os.makedirs(os.path.join(work, 'bak'), exist_ok=True)

    ns = {
        'WT': tree,
        'A': tree + '/src/navi48-bringup/src/apple/',
        'T': tree + '/src/navi48-bringup/tests/',
        'X': tree + '/src/xlat12/',
        'OUT': out,
        'SAN': '-fsanitize=address,undefined -fno-sanitize-recover=all',
        'W': '-std=c++17 -Wall -Wextra -Werror -O1',
        'Wn': '-std=c++17 -Wall -Wextra -O1',
    }
    ns['AHH'] = ns['A'] + 'AppleHardwareHook.cpp'
    ns['SUITES'] = default_suites(ns)
    g = runpy.run_path(args.plants_file, init_globals=dict(ns))
    suites = g.get('SUITES', ns['SUITES'])
    plants = []
    for p in g['PLANTS']:
        name, path, old, new, sl = p
        path = path if os.path.isabs(path) else os.path.join(tree, path)
        for s in sl:
            if s not in suites:
                sys.exit(f'[{name}] names unknown suite {s!r}; known: {", ".join(sorted(suites))}')
        plants.append((name, path, old, new, sl))

    if args.list:
        for name, path, _, _, sl in plants:
            print(f'{name}  ::  {os.path.relpath(path, tree)}  ::  {",".join(sl)}')
        return 0

    selected = [p for p in plants if not args.only or any(p[0].startswith(o) for o in args.only)]
    problems = startup_check(tree, work, plants, {p[0] for p in selected})
    if problems:
        print('REFUSING TO RUN. Compare each file against its .bak and grep the original strings '
              'before anything else:')
        for p in problems:
            print('  - ' + p)
        return 4
    print('check: no leftover plant found')
    if args.check:
        return 0

    def as_exit(signum, _frame):
        raise SystemExit(f'signal {signum}: restoring')
    signal.signal(signal.SIGTERM, as_exit)
    signal.signal(signal.SIGHUP, as_exit)

    before = diff_md5(tree)
    print(f'git diff md5 before: {before}')
    if not selected:
        print('no plant matches ' + ' '.join(args.only))
        return 2
    verdicts = []
    for name, path, old, new, sl in selected:
        src = open(path).read()
        n = src.count(old)
        if n != 1:
            print(f'SKIPPED [{name}]: the original text occurs {n} times in {path} (must be 1)')
            verdicts.append((name, 'SKIPPED'))
            continue
        bak = bak_name(tree, os.path.join(work, 'bak'), path)
        shutil.copy2(path, bak)
        caught, inconclusive = False, False
        try:
            with open(path, 'w') as fh:
                fh.write(src.replace(old, new))
            for su in sl:
                cc, run = suites[su]
                try:
                    b = sh(cc, tree, SUITE_TIMEOUT_S)
                except subprocess.TimeoutExpired:
                    print(f'PLANTED [{name}] {su}: BUILD TIMEOUT')
                    inconclusive = True
                    continue
                if b.returncode:
                    print(f'PLANTED [{name}] {su}: BUILD FAILED (not a catch) {b.stderr[-300:]}')
                    inconclusive = True
                    continue
                try:
                    r = sh(run, tree, SUITE_TIMEOUT_S)
                except subprocess.TimeoutExpired:
                    print(f'PLANTED [{name}] {su}: RUN TIMEOUT')
                    inconclusive = True
                    continue
                outp = (r.stdout + r.stderr).strip().splitlines()
                tail = outp[-1][:110] if outp else ''
                print(f'PLANTED [{name}] {su}: rc={r.returncode} :: {tail}')
                for line in [x for x in outp if 'FAIL' in x and 'CAUGHT' not in x][:3]:
                    print('       ' + line.strip()[:150])
                if r.returncode != 0:
                    caught = True
        finally:
            shutil.copy2(bak, path)
            same = filecmp.cmp(bak, path, shallow=False)
            print(f'   restored {os.path.relpath(path, tree)}: identical={same}')
            if same:
                os.remove(bak)
            else:
                print(f'   RESTORE FAILED: the .bak is kept at {bak}; stop and restore by hand')
                raise SystemExit(5)
        v = 'CAUGHT' if caught else ('INCONCLUSIVE' if inconclusive else 'NOT CAUGHT')
        print(f'VERDICT [{name}]: {v}')
        verdicts.append((name, v))

    after = diff_md5(tree)
    print(f'git diff md5 after:  {after}  ({"unchanged" if after == before else "CHANGED: investigate"})')
    for name, v in verdicts:
        print(f'  {v:12s} {name}')
    return 0 if after == before else 6


if __name__ == '__main__':
    sys.exit(main())
