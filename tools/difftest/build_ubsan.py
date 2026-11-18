#!/usr/bin/env python3
"""Build qilin_difftest under UBSan with zig's Clang, on any host.

MSVC has no UBSan, so on Windows this is the way to run the differential
test with every case of undefined behaviour in lifted code or the runtime
stopping the harness (and so the test) with the report. On Linux and macOS
`-DQILIN_UBSAN=ON` does the same through CMake.

  pip install ziglang
  build_ubsan.py --gen gen --out build-ubsan [--jobs N]
  difftest.py --harness build-ubsan/qilin_difftest_ubsan[.exe] --dol main.dol --gen gen

Compiles with the project's warnings as errors (-Wall -Wextra -Wpedantic
-Wshadow -Werror), its floating-point rules and checked memory, as the
CMake build of qilin_difftest does.
"""
import argparse
import concurrent.futures
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FLAGS = ["-std=c++23", "-O2", "-g0", "-Wall", "-Wextra", "-Wpedantic", "-Wshadow", "-Werror",
         "-ffp-contract=off", "-fno-fast-math", "-fsanitize=undefined", "-fno-sanitize-recover=all",
         "-DQILIN_CHECKED_MEMORY", "-DQILIN_DETERMINISTIC_TIME"]


def zig(*args):
    return [sys.executable, "-m", "ziglang", "c++", *args]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gen", required=True, help="qilin lift output directory")
    ap.add_argument("--out", required=True, help="build directory")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    args = ap.parse_args()

    gen = os.path.abspath(args.gen)
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    includes = ["-I" + gen, "-I" + os.path.join(ROOT, "recomp", "runtime"), "-I" + os.path.join(ROOT, "recomp", "src")]
    sources = sorted(os.path.join(gen, f) for f in os.listdir(gen) if f.endswith(".cpp"))
    sources += [os.path.join(ROOT, *p) for p in (("recomp", "runtime", "dispatch.cpp"), ("recomp", "src", "dol.cpp"),
                                                  ("recomp", "src", "sha256.cpp"), ("tools", "difftest", "harness.cpp"))]
    if not any(os.path.basename(s).startswith("lifted_") for s in sources):
        sys.exit(f"build_ubsan: no lifted code in {gen} (run qilin lift first)")

    # One file first: zig fills its cache, which parallel first runs race on.
    def compile_one(src):
        obj = os.path.join(out, os.path.basename(src) + ".o")
        r = subprocess.run(zig(*FLAGS, *includes, "-c", src, "-o", obj), capture_output=True, text=True)
        noise = "cached ZIR"
        msg = "\n".join(l for l in (r.stdout + r.stderr).splitlines() if noise not in l)
        return src, obj, r.returncode, msg

    results = [compile_one(sources[-1])]
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        results += list(pool.map(compile_one, sources[:-1]))
    failed = [(src, msg) for src, _, code, msg in results if code != 0]
    for src, msg in failed:
        print(f"build_ubsan: {src} failed:\n{msg}", file=sys.stderr)
    if failed:
        sys.exit(1)

    exe = os.path.join(out, "qilin_difftest_ubsan" + (".exe" if os.name == "nt" else ""))
    r = subprocess.run(zig("-fsanitize=undefined", *[obj for _, obj, _, _ in results], "-o", exe))
    if r.returncode != 0:
        sys.exit("build_ubsan: link failed")
    print(exe)


if __name__ == "__main__":
    main()
