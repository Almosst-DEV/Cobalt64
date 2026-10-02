#!/usr/bin/env python3
"""dcn41lib.py - shared paths and the memory guard for the DCN 4.1 display groundwork (tools/dcn41/).

Every compiler or test child this directory starts runs through run() below, which is tools/m4-shaders/m4lib.py's
guarded_run IMPORTED read-only (not copied): background QoS (taskpolicy -c background), the child's process-tree
physical footprint polled every 0.5 s, SIGKILL at 1.5 GB or 90 s. Before each child, require_memory() reads
/usr/bin/memory_pressure and STOPS (never pushes through) when system-wide free memory is under 40%.

Why (notes:: the small-memory host Mac panicked from memory exhaustion at 11:53 and later had one sequential llc at an
11,418 MB footprint. So: one child at a time, guard on every child, outputs streamed to files in BUILD.

Build outputs never land in the repo: BUILD defaults to $TMPDIR/dcn41-build (override with DCN41_BUILD).
"""
import os, pathlib, subprocess, sys

sys.dont_write_bytecode = True

HERE = pathlib.Path(__file__).resolve().parent
TOOLS = HERE.parent
ROOT = TOOLS.parent
SRC = ROOT / "src/dcn41"
AMD = ROOT / "re/linux-dc/drivers/gpu/drm/amd"          # gitignored Linux clone, commit 238650ef6c7c
DISPLAY = AMD / "display"
DC = DISPLAY / "dc"
ASIC_DCN = AMD / "include/asic_reg/dcn"
OFFSET_H = ASIC_DCN / "dcn_4_1_0_offset.h"
SHMASK_H = ASIC_DCN / "dcn_4_1_0_sh_mask.h"
BUILD = pathlib.Path(os.environ.get("DCN41_BUILD", pathlib.Path(os.environ.get("TMPDIR", "/tmp")) / "dcn41-build"))
LINUX_COMMIT = "238650ef6c7c"

sys.path.insert(0, str(TOOLS / "m4-shaders"))
import m4lib  # noqa: E402  (read-only import: guarded_run, require_memory, GuardKilled)

GuardKilled = m4lib.GuardKilled
CLANG = "/usr/bin/clang"


def run(cmd, what, log=None, **kw):
    """One guarded child. `what` names it for the memory gate and the log. stdout+stderr go to `log` (a path) when
    given, so large compiler output never sits in memory. Returns the CompletedProcess; raises GuardKilled on a kill."""
    pct = m4lib.require_memory(what)
    if log is None:
        r = m4lib.guarded_run(cmd, capture_output=True, text=True, **kw)
    else:
        log = pathlib.Path(log)
        log.parent.mkdir(parents=True, exist_ok=True)
        r = m4lib.guarded_run(cmd, capture_output=True, text=True, **kw)
        log.write_text(f"$ {' '.join(str(c) for c in cmd)}\n# memory free before: {pct}%\n"
                       f"# rc={r.returncode}\n--- stdout\n{r.stdout}\n--- stderr\n{r.stderr}\n")
    r.mem_free_before = pct
    return r


def check(r, what):
    if r.returncode != 0:
        sys.stderr.write(f"{what}: rc={r.returncode}\n{r.stdout[-4000:]}\n{r.stderr[-4000:]}\n")
        raise SystemExit(f"{what}: FAILED")
    return r


def need_linux():
    if not OFFSET_H.exists():
        raise SystemExit(f"missing {OFFSET_H}: the gitignored Linux clone re/linux-dc (commit {LINUX_COMMIT}) is needed")
