#!/usr/bin/env python3
"""Port the Rhythm Heaven Fever decomp's US function names to the PAL build.

The decomp (third_party/rhf, CC0) names the US build's (SOME01) functions with
their sizes and splits the code into translation units. The PAL build
(SOMP01) is the same program linked at different addresses, with a few
regional changes. Without the US executable there are no US bytes to compare,
so functions are matched by **size sequence**: qilin's functions (from
`qilin cfg`) against the decomp's, in address order, per section.

  1. difflib aligns the two size sequences. A match inside a run of at least
     three consecutive equal sizes is "anchored"; a lone equal size between
     differences is "single".
  2. In each gap between aligned runs, groups whose sizes add up to the same
     total are paired: one decomp function as several of ours is a "split"
     (our boundaries differ from the decomp's), several as one of ours is a
     "merge", one to one with a different size is "changed" (regional code).
     What's left is "us-only" or "pal-only".
  3. Each decomp function belongs to a translation unit (splits.txt); its
     library is the unit's directory (revolution/os, nw4r/snd, MSL, ...). A
     library's PAL range runs from its first to its last matched function.

Usage:
  port_symbols.py --qilin build/qilin --dol main.dol [--symbols-out pal_symbols.txt] [--list KIND]

The PAL symbol list is derived from the game binary: keep it out of git.
"""
import argparse
import collections
import difflib
import json
import os
import re
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RHF = os.path.join(ROOT, "third_party", "rhf", "SOME01")
SECTIONS = (".init", ".text")
PADDING = (0, 0x60000000)  # zero, nop

SYMBOL = re.compile(r"^(\S+) = (\.init|\.text):0x([0-9A-Fa-f]+); // type:function size:0x([0-9A-Fa-f]+)")


def load_dol(path):
    data = open(path, "rb").read()
    sections = []
    for i in range(18):
        off, addr, size = (struct.unpack(">I", data[o + 4 * i:o + 4 * i + 4])[0] for o in (0, 0x48, 0x90))
        if size:
            sections.append((i < 7, off, addr, size))

    def word(a):
        for _, off, addr, size in sections:
            if addr <= a < addr + size:
                return struct.unpack(">I", data[off + a - addr:off + a - addr + 4])[0]
        return None

    text = sorted((addr, addr + size) for is_text, _, addr, size in sections if is_text)
    return word, text


def section_name(addr, text):
    # CodeWarrior's layout: the first text section is .init, the second .text.
    for k, (lo, hi) in enumerate(text):
        if lo <= addr < hi:
            return SECTIONS[k] if k < 2 else f"text{k}", hi
    raise ValueError(f"{addr:08X} is not in a text section")


def pal_functions(qilin, dol):
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "cfg.json")
        subprocess.run([qilin, "cfg", dol, "--json", out], check=True, capture_output=True)
        cfg = json.load(open(out))
    word, text = load_dol(dol)
    starts = sorted(int(f["start"], 16) for f in cfg["functions"])
    funcs = []
    for i, s in enumerate(starts):
        sec, sec_end = section_name(s, text)
        nxt = starts[i + 1] if i + 1 < len(starts) and starts[i + 1] < sec_end else sec_end
        end = nxt
        while end > s + 4 and word(end - 4) in PADDING:  # trailing alignment
            end -= 4
        funcs.append({"sec": sec, "addr": s, "size": end - s})
    return funcs


def us_functions():
    funcs = []
    for line in open(os.path.join(RHF, "symbols.txt")):
        m = SYMBOL.match(line)
        if m:
            funcs.append({"sec": m[2], "addr": int(m[3], 16), "size": int(m[4], 16), "name": m[1]})
    funcs.sort(key=lambda f: f["addr"])
    # Translation units: "path:" then indented "section start:0x.. end:0x.." lines.
    units, unit = [], None
    for line in open(os.path.join(RHF, "splits.txt")):
        if line and not line[0].isspace() and line.rstrip().endswith(":") and line.strip() != "Sections:":
            unit = line.strip()[:-1]
            continue
        m = re.match(r"\s+(\.init|\.text)\s+start:0x([0-9A-Fa-f]+) end:0x([0-9A-Fa-f]+)", line)
        if m and unit:
            units.append((int(m[2], 16), int(m[3], 16), unit))
    units.sort()
    for f in funcs:
        f["unit"] = next((u for lo, hi, u in units if lo <= f["addr"] < hi), "(unsplit)")
        f["library"] = library_of(f["unit"])
    return funcs


def library_of(unit):
    parts = unit.split("/")
    if parts[0] == "Game":
        return "game"
    if parts[0] in ("revolution", "nw4r") and len(parts) > 1:
        return f"{parts[0]}/{parts[1]}"
    if parts[0] == "PowerPC_EABI_Support" and len(parts) > 1:
        return f"EABI/{parts[1]}"
    return parts[0]


def align(us, pal):
    """Returns (us_index or None, [pal indices], kind) for one section."""
    U = [f["size"] for f in us]
    P = [f["size"] for f in pal]
    out = []
    sm = difflib.SequenceMatcher(None, U, P, autojunk=False)
    for tag, i1, i2, k1, k2 in sm.get_opcodes():
        if tag == "equal":
            kind = "anchored" if i2 - i1 >= 3 else "single"
            out += [(i1 + n, [k1 + n], kind) for n in range(i2 - i1)]
            continue
        # Pair groups with equal totals, walking both sides together.
        i, k = i1, k1
        while i < i2 and k < k2:
            su, sp, ie, ke = U[i], P[k], i + 1, k + 1
            while su != sp and ie <= i2 and ke <= k2:
                if su < sp:
                    if ie == i2:
                        break
                    su += U[ie]
                    ie += 1
                else:
                    if ke == k2:
                        break
                    sp += P[ke]
                    ke += 1
            if su == sp:
                nu, np_ = ie - i, ke - k
                kind = "split" if nu == 1 and np_ > 1 else "merge" if np_ == 1 and nu > 1 else "regroup"
                if nu == 1:
                    out.append((i, list(range(k, ke)), kind))
                else:
                    out += [(i + n, list(range(k, ke)) if n == 0 else [], kind) for n in range(nu)]
                i, k = ie, ke
                continue
            break
        rest_u, rest_p = list(range(i, i2)), list(range(k, k2))
        if len(rest_u) == len(rest_p):
            out += [(a, [b], "changed") for a, b in zip(rest_u, rest_p)]
        else:
            out += [(a, [], "us-only") for a in rest_u]
            out += [(None, [b], "pal-only") for b in rest_p]
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--qilin", required=True)
    ap.add_argument("--dol", required=True, help="the PAL main.dol")
    ap.add_argument("--symbols-out", help="write PAL names here (derived from the game: don't commit it)")
    ap.add_argument("--list", help="list the pairs of one kind (anchored, single, split, merge, regroup, changed, us-only, pal-only)")
    args = ap.parse_args()

    pal = pal_functions(args.qilin, args.dol)
    us = us_functions()
    kinds = collections.Counter()
    pairs = []
    for sec in SECTIONS:
        U = [f for f in us if f["sec"] == sec]
        P = [f for f in pal if f["sec"] == sec]
        for ui, pis, kind in align(U, P):
            pairs.append((U[ui] if ui is not None else None, [P[k] for k in pis], kind))
            kinds[kind] += 1

    named = {k: v for k, v in kinds.items()}
    print(f"US functions {len(us)}, PAL functions {len(pal)}")
    for k in ("anchored", "single", "split", "merge", "regroup", "changed", "us-only", "pal-only"):
        if named.get(k):
            print(f"  {k:9} {named[k]}")

    # Library ranges in PAL, from matched functions.
    lib = collections.OrderedDict()
    for u, ps, kind in pairs:
        if not u or not ps:
            continue
        r = lib.setdefault((u["sec"], u["library"]), {"lo": 1 << 32, "hi": 0, "us": 0, "matched": 0, "anchored": 0})
        r["lo"] = min(r["lo"], ps[0]["addr"])
        r["hi"] = max(r["hi"], ps[-1]["addr"] + ps[-1]["size"])
        r["matched"] += 1
        r["anchored"] += kind == "anchored"
    for u in us:
        if (u["sec"], u["library"]) in lib:
            lib[(u["sec"], u["library"])]["us"] += 1
    print("\nsection library            PAL range          decomp functions  matched  anchored")
    order = sorted(lib.items(), key=lambda kv: kv[1]["lo"])
    for (sec, name), r in order:
        print(f"{sec:7} {name:18} {r['lo']:08X}-{r['hi']:08X}  {r['us']:16}  {r['matched']:7}  {r['anchored']:8}")
    # Ranges should be disjoint: each library is linked as a block.
    for (a, ra), (b, rb) in zip(order, order[1:]):
        if rb["lo"] < ra["hi"]:
            print(f"  note: {a[1]} and {b[1]} overlap ({rb['lo']:08X} < {ra['hi']:08X})")

    if args.list:
        print(f"\n{args.list}:")
        for u, ps, kind in pairs:
            if kind != args.list:
                continue
            us_s = f"{u['addr']:08X} {u['size']:6X} {u['name']}" if u else "-"
            pal_s = " + ".join(f"{p['addr']:08X}/{p['size']:X}" for p in ps) or "-"
            print(f"  US {us_s:60} PAL {pal_s}")

    if args.symbols_out:
        with open(args.symbols_out, "w") as f:
            for u, ps, kind in pairs:
                for n, p in enumerate(ps):
                    if u is None:
                        continue
                    name = u["name"] if n == 0 else f"{u['name']}.part{n + 1}"
                    f.write(f"{name} = {p['sec']}:0x{p['addr']:08X}; // size:0x{p['size']:X} us:0x{u['addr']:08X} "
                            f"match:{kind} unit:{u['unit']}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
