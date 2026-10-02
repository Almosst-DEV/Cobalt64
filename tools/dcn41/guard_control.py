#!/usr/bin/env python3
"""guard_control.py - prove the memory guard dcn41lib.run() uses, BEFORE any compile in tools/dcn41 is trusted.

  python3 tools/dcn41/guard_control.py [--quick]

Controls, each through dcn41lib.run (the exact wrapper every compile here goes through):
 1 negative: /bin/echo returns normally with its output.
 2 memory: a python child that allocates 100 MB every 0.2 s up to 4 GB must be killed-mem at the 1.5 GB cap.
 3 grandchild memory: the same hog one level down (sh -> python3) must be killed-mem (tree footprint, not the pid).
 4 time: /bin/sleep 200 must be killed-time at the 90 s cap (--quick: a 5 s cap, proving the path only).
 5 compiler: a trivial clang compile runs to completion under the guard (the guard does not break the toolchain).
Each result line is printed verbatim; the last line is GUARD CONTROL PASS or FAIL.
"""
import sys, pathlib, time

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import dcn41lib as L  # noqa: E402

HOG = ("import time\nb=[]\nfor i in range(40):\n    b.append(b'\\x01' * (100 << 20))\n    time.sleep(0.2)\n"
       "print('hog finished unkilled')\n")


def trial(name, cmd, **kw):
    t0 = time.monotonic()
    try:
        r = L.run(cmd, f"guard_control {name}", **kw)
        print(f"{name}: NOT KILLED rc={r.returncode} after {time.monotonic() - t0:.1f} s stdout={r.stdout.strip()!r} "
              f"(memory free before {r.mem_free_before}%)")
        return None
    except L.GuardKilled as e:
        print(f"{name}: {e.kind} peak {e.rss_kb // 1024} MB after {e.seconds:.1f} s cmd={pathlib.Path(str(e.cmd[0])).name} "
              f"(memory free after {L.m4lib.memory_free_pct()}%)")
        return e.kind


def main():
    quick = "--quick" in sys.argv
    ok = True
    r = L.run(["/bin/echo", "guard negative control"], "guard_control negative")
    print(f"negative: rc={r.returncode} stdout={r.stdout.strip()!r} (memory free before {r.mem_free_before}%)")
    ok &= r.returncode == 0 and r.stdout.strip() == "guard negative control"
    ok &= trial("memory", [sys.executable, "-c", HOG]) == "killed-mem"
    ok &= trial("grandchild-memory", ["/bin/sh", "-c", f"{sys.executable} -c \"$HOG\"; echo shell-done"],
                env={"HOG": HOG, "PATH": "/usr/bin:/bin"}) == "killed-mem"
    ok &= trial("time", ["/bin/sleep", "200"], **({"wall_s": 5.0} if quick else {})) == "killed-time"
    L.BUILD.mkdir(parents=True, exist_ok=True)
    src = L.BUILD / "guard_cc.c"
    src.write_text("int main(void) { return 0; }\n")
    r = L.run([L.CLANG, "-c", str(src), "-o", str(L.BUILD / "guard_cc.o")], "guard_control compiler")
    print(f"compiler: rc={r.returncode} stderr={r.stderr.strip()!r} (memory free before {r.mem_free_before}%)")
    ok &= r.returncode == 0 and (L.BUILD / "guard_cc.o").exists()
    print("GUARD CONTROL", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
