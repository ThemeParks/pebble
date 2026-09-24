#!/usr/bin/env python3
"""
unit_tests.py — host-side unit tests, no SDK or emulator required.

Two families of suites:
  * tests/unit/test_*.c — compiled with plain gcc against a stubbed
    <pebble.h> (tests/unit/support/) plus the pure-logic sources from
    src/c, each into its own standalone binary, then run.
  * tests/js/test_*.js  — plain node scripts exercising src/pkjs/lib.js.

These are the fast inner loop; scripts/integration_tests.py remains the
emulator-driven safety net for runtime-only behaviour.

Usage:
  python3 scripts/unit_tests.py            # run everything
  python3 scripts/unit_tests.py -k sort    # only suites matching 'sort'
  python3 scripts/unit_tests.py -v         # show output of passing suites too

Exit codes: 0 all passed, 1 test failure, 2 missing toolchain.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
UNIT_DIR = REPO_ROOT / "tests" / "unit"
JS_DIR = REPO_ROOT / "tests" / "js"
SUPPORT_DIR = UNIT_DIR / "support"
SRC_C_DIR = REPO_ROOT / "src" / "c"
# Lives under build/ which .gitignore already excludes.
BUILD_DIR = REPO_ROOT / "build" / "unit_tests"

# Pure-logic sources compiled into every C test binary. UI files are
# deliberately absent — they need the real SDK. The support stubs
# provide persist_* (in-memory) and the g_parks fixture (test_config.c
# replaces src/c/config.c so tests don't depend on real park UUIDs).
C_COMMON_SOURCES = [
    SRC_C_DIR / "parkdata.c",
    SRC_C_DIR / "sort.c",
    SRC_C_DIR / "storage.c",
    SRC_C_DIR / "format.c",
    SRC_C_DIR / "dest_parse.c",
    SRC_C_DIR / "settings_parse.c",
    SRC_C_DIR / "favs_sync.c",
    SRC_C_DIR / "geom.c",
    SUPPORT_DIR / "persist_stub.c",
    SUPPORT_DIR / "test_config.c",
]

# -I order matters: the support dir first so #include <pebble.h>
# resolves to the stub rather than nothing (or a real SDK header).
# MULTI_DESTINATION is defined so the uber-only code (dest_parse.c,
# storage.c's last-destination helpers) compiles into the test binaries;
# it doesn't change the behaviour of anything else under test (the
# checked-in config.h has no MULTI_DESTINATION branches).
CFLAGS = [
    "-std=c11", "-Wall", "-Wextra", "-Werror",
    "-DMULTI_DESTINATION",
    "-I", str(SUPPORT_DIR),
    "-I", str(SRC_C_DIR),
]


def run_c_suite(cc: str, test_c: Path, verbose: bool) -> bool:
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    exe = BUILD_DIR / (test_c.stem + (".exe" if os.name == "nt" else ""))
    cmd = [cc, *CFLAGS,
           *[str(s) for s in C_COMMON_SOURCES], str(test_c),
           "-o", str(exe)]
    comp = subprocess.run(cmd, capture_output=True, text=True)
    if comp.returncode != 0:
        print(f"FAIL {test_c.stem} (compile)")
        print("  $ " + " ".join(cmd))
        sys.stdout.write(comp.stdout)
        sys.stdout.write(comp.stderr)
        return False

    run = subprocess.run([str(exe)], capture_output=True, text=True)
    ok = run.returncode == 0
    print(f"{'PASS' if ok else 'FAIL'} {test_c.stem}")
    if not ok or verbose:
        sys.stdout.write(run.stdout)
        sys.stdout.write(run.stderr)
    return ok


def run_js_suite(node: str, test_js: Path, verbose: bool) -> bool:
    run = subprocess.run([node, str(test_js)], capture_output=True, text=True)
    ok = run.returncode == 0
    print(f"{'PASS' if ok else 'FAIL'} {test_js.stem}")
    if not ok or verbose:
        sys.stdout.write(run.stdout)
        sys.stdout.write(run.stderr)
    return ok


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("-k", metavar="PATTERN", default=None,
                   help="only run suites whose filename contains PATTERN")
    p.add_argument("-v", "--verbose", action="store_true",
                   help="show test output even for passing suites")
    args = p.parse_args()

    c_tests = sorted(UNIT_DIR.glob("test_*.c"))
    js_tests = sorted(JS_DIR.glob("test_*.js"))
    if args.k:
        c_tests = [t for t in c_tests if args.k in t.stem]
        js_tests = [t for t in js_tests if args.k in t.stem]
    if not c_tests and not js_tests:
        print(f"no test suites match {args.k!r}")
        return 1

    cc = shutil.which("gcc") or shutil.which("cc")
    node = shutil.which("node")
    if c_tests and not cc:
        print("error: no C compiler found (need gcc or cc on PATH)")
        return 2
    if js_tests and not node:
        print("error: node not found on PATH")
        return 2

    results: list[tuple[str, bool]] = []

    if c_tests:
        print("== C unit tests ==")
        for t in c_tests:
            results.append((t.stem, run_c_suite(cc, t, args.verbose)))

    if js_tests:
        print("== JS unit tests ==")
        for t in js_tests:
            results.append((t.stem, run_js_suite(node, t, args.verbose)))

    failed = [name for name, ok in results if not ok]
    print("== Summary ==")
    print(f"{len(results) - len(failed)}/{len(results)} suites passed"
          + (f" — FAILED: {', '.join(failed)}" if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
