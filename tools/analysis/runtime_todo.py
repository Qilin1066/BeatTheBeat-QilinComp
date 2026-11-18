#!/usr/bin/env python3
"""The runtime's to-do list: what lifted code needs from the libraries and
hardware the runtime replaces.

  runtime_todo.py --qilin build/qilin --dol main.dol --gen gen [--symbols pal_symbols.txt] [--out todo.txt]

Joins `qilin cfg` (the call graph), `qilin hwaccess` (constant-address
loads and stores outside cached MEM1) and the library plan in
docs/code-analysis.md (each library's PAL range: lift, replace or drop), and
lists, for the code that is lifted:

  1. the replaced or dropped functions it calls directly: the API the
     runtime must provide, with how many lifted functions call each;
  2. its direct hardware accesses, by region (the GX FIFO above all: the
     SDK's inline GX functions write it straight from game and NW4R code);
  3. its indirect calls, which may reach replaced code through pointers;
  4. lift-side functions the lifter can't lift yet.

Names come from --symbols, the PAL symbol list tools/analysis/port_symbols.py
writes (it's derived from the game, like this report: never commit either).
"""
import argparse
import collections
import json
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ROW = re.compile(r"^\| (?P<lib>[^|]+?) \| `(?P<lo>[0-9A-F]{8})`–`(?P<hi>[0-9A-F]{8})` \|.*\| (?P<plan>lift|replace|drop)[^|]*\|\s*$")


def library_plan(path):
    """[(lo, hi, library, plan)] from the library table in docs/code-analysis.md."""
    rows = []
    for line in open(path, encoding="utf-8"):
        m = ROW.match(line)
        if m:
            rows.append((int(m["lo"], 16), int(m["hi"], 16), m["lib"].strip(), m["plan"]))
    if len(rows) < 10:
        sys.exit(f"runtime_todo: found only {len(rows)} library rows in {path}; has the table's format changed?")
    return sorted(rows)


def symbols(path):
    names = {}
    if path:
        for line in open(path, encoding="utf-8"):
            m = re.match(r"^(\S+) = \.\w+:0x([0-9A-Fa-f]{8});", line)
            if m:
                names[int(m[2], 16)] = m[1]
    return names


def run_json(qilin, *args):
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "out.json")
        r = subprocess.run([os.path.abspath(qilin), *args, "--json", out], capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f"runtime_todo: qilin {' '.join(args)} failed:\n{r.stderr}")
        return json.load(open(out, encoding="utf-8"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--qilin", required=True)
    ap.add_argument("--dol", required=True)
    ap.add_argument("--gen", required=True, help="qilin lift output, for which functions are lifted")
    ap.add_argument("--symbols", help="PAL symbol list from port_symbols.py (optional)")
    ap.add_argument("--plan", default=os.path.join(ROOT, "docs", "code-analysis.md"))
    ap.add_argument("--out", help="write the report here instead of stdout (keep it out of git)")
    args = ap.parse_args()

    plan = library_plan(args.plan)
    names = symbols(args.symbols)
    cfg = run_json(args.qilin, "cfg", args.dol)
    hw = run_json(args.qilin, "hwaccess", args.dol)
    stubs = open(os.path.join(args.gen, "stubs.cpp"), encoding="utf-8").read()
    not_lifted = {int(a, 16) for a in re.findall(r"rt::not_lifted\(0x([0-9A-F]{8})u\)", stubs)}

    def where(addr):
        for lo, hi, lib, p in plan:
            if lo <= addr < hi:
                return lib, p
        return "(no library)", "lift"  # outside the table: the game's own code is lifted

    def name(addr):
        return f"{addr:08X} {names[addr]}" if addr in names else f"{addr:08X}"

    funcs = {int(f["start"], 16): f for f in cfg["functions"]}
    lifted_side = {a for a in funcs if where(a)[1] == "lift"}

    api = collections.defaultdict(set)  # replaced function -> lift-side callers
    indirect = collections.Counter()    # library -> indirect calls from lift-side code
    for a in lifted_side:
        f = funcs[a]
        for callee in [int(x, 16) for x in f.get("calls", []) + f.get("tails", [])]:
            if where(callee)[1] != "lift":
                api[callee].add(a)
        indirect[where(a)[0]] += len(f.get("indirect_calls", [])) + len(f.get("indirect_tails", []))

    hw_by_region = collections.defaultdict(lambda: collections.defaultdict(int))  # region -> fn -> count
    hw_elsewhere = collections.Counter()
    for f in hw["functions"]:
        a = int(f["start"], 16)
        for acc in f["accesses"]:
            if a in lifted_side:
                hw_by_region[acc["region"]][a] += 1
            else:
                hw_elsewhere[acc["region"]] += 1

    lines = []
    say = lines.append
    say("Runtime to-do list (derived from the game: don't commit it)")
    say(f"{len(funcs)} functions; {len(lifted_side)} on the lifted side, {len(funcs) - len(lifted_side)} replaced or dropped")
    say("")
    by_lib = collections.defaultdict(list)
    for callee, callers in api.items():
        by_lib[where(callee)[0]].append((callee, callers))
    say(f"1. Replaced functions lifted code calls directly: {len(api)}, from {len(set().union(*api.values())) if api else 0} lifted functions")
    for lib in sorted(by_lib, key=lambda l: -sum(len(c) for _, c in by_lib[l])):
        entries = sorted(by_lib[lib], key=lambda e: (-len(e[1]), e[0]))
        say(f"  {lib} ({where(entries[0][0])[1]}): {len(entries)} functions")
        for callee, callers in entries:
            say(f"    {len(callers):5} callers  {name(callee)}")
    say("")
    say(f"2. Direct hardware accesses in lifted-side code ({sum(sum(v.values()) for v in hw_by_region.values())}):")
    for region in sorted(hw_by_region, key=lambda r: -sum(hw_by_region[r].values())):
        fns = hw_by_region[region]
        say(f"  {region}: {sum(fns.values())} accesses in {len(fns)} functions")
        for a, n in sorted(fns.items(), key=lambda kv: -kv[1]):
            say(f"    {n:5}  {name(a)} [{where(a)[0]}]")
    say("  (in replaced and dropped code, for comparison: "
        + ", ".join(f"{r} {n}" for r, n in hw_elsewhere.most_common()) + ")")
    say("")
    say(f"3. Indirect calls in lifted-side code: {sum(indirect.values())} (targets known only at run time; "
        "the function table resolves them, and one into replaced code must be provided too)")
    for lib, n in indirect.most_common(8):
        say(f"  {n:5}  {lib}")
    say("")
    blocked = sorted(a for a in lifted_side if a in not_lifted)
    say(f"4. Lifted-side functions not lifted yet: {len(blocked)}")
    for a in blocked:
        say(f"    {name(a)} [{where(a)[0]}]")

    text = "\n".join(lines) + "\n"
    if args.out:
        open(args.out, "w", encoding="utf-8").write(text)
        print(f"{args.out}: {len(api)} replaced functions called, "
              f"{sum(sum(v.values()) for v in hw_by_region.values())} hardware accesses, {len(blocked)} blocked")
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
