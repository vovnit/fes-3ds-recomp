#!/usr/bin/env python3
"""Names for the game's functions from two public research projects, matched against YOUR boot executable.

Two independent projects publish function names for the NTSC-U executable (SLUS_216.21):
  * snaku/Persona3-FES-Decompilation (a matching decompilation): config/SLUS_216.21/symbol_addrs.txt
  * ppkkkkk/p3fes-recompiled (a static recompilation on PS2Recomp, GPL-3.0): p3fes/p3fes_map.csv
Most entries are placeholders named after the source file (h_maestro_0010cdd0); a few thousand are real names, among them the
handlers of some script functions (scrCommand_WAIT, datScrCmd_ADD_MONEY ...). This tool never copies those files into our
repository: "fetch" downloads them to a folder on your computer, and the other commands read them from there.

  python3 p3fes_symbols.py fetch                         download both lists (into ~/.p3fes-symbols, or --dir)
  python3 p3fes_symbols.py comm SLUS_216.21 [-o report]  the script function table (COMM n) with the name of each handler
  python3 p3fes_symbols.py comm SLUS_216.21 --names-out new_names.txt
                                                         also write suggestions in the script_names.txt format, for the
                                                         functions that have no name there yet (or only a guess with ?)
  python3 p3fes_symbols.py find 0x35B030 [more...]       which function holds an address
  python3 p3fes_symbols.py find --name WAIT              functions whose name contains a word

The table is read from your own executable (502 records of function address and value count at 0x7B92B0, see
docs/script-functions.md), so no game code is needed here. Check a suggested name against the scripts before relying on it:
a name says what the original programmers called the function, not exactly how a script uses it.
"""
import argparse
import bisect
import csv
import os
import re
import struct
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import p3fes_bf  # noqa: E402  (elf_segments, addr_to_off, COMM_TABLE, script names)

SOURCES = [
    ("snaku_symbol_addrs.txt", "snaku/Persona3-FES-Decompilation (matching decompilation, no licence file: names used as facts only)",
     "https://raw.githubusercontent.com/snaku/Persona3-FES-Decompilation/59d284ee21fe166bd46252e38e1a08874c4a1a66/config/SLUS_216.21/symbol_addrs.txt"),
    ("ppkkkkk_p3fes_map.csv", "ppkkkkk/p3fes-recompiled (static recompilation, GPL-3.0)",
     "https://raw.githubusercontent.com/ppkkkkk/p3fes-recompiled/b53437748ded7e11a955ae79e94414d147a1ba17/p3fes/p3fes_map.csv"),
]
DEFAULT_DIR = os.path.expanduser("~/.p3fes-symbols")
PLACEHOLDER = re.compile(r"_[0-9a-fA-F]{8}$|^(FUN|func|sub)_[0-9a-fA-F]+$")


def fetch(folder):
    os.makedirs(folder, exist_ok=True)
    ok = True
    for name, what, url in SOURCES:
        path = os.path.join(folder, name)
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "p3fes-tools"})
            data = urllib.request.urlopen(req, timeout=60).read()
        except Exception as e:  # noqa: BLE001  (any network trouble: say which list and go on)
            print("could not download %s: %s" % (name, e))
            ok = False
            continue
        open(path, "wb").write(data)
        print("%s: %d bytes from %s" % (path, len(data), what))
    return ok


def load(folder, paths=None):
    """Read every list found: {address: [names]} and sorted (start, end, name) ranges where the list gives an end."""
    names, ranges = {}, []
    paths = paths or [os.path.join(folder, n) for n, _, _ in SOURCES]
    used = []
    for path in paths:
        if not os.path.isfile(path):
            continue
        used.append(path)
        text = open(path, encoding="utf-8", errors="replace").read()
        if text.startswith("Name,Start"):
            for row in csv.DictReader(text.splitlines()):
                try:
                    a, e = int(row["Start"], 16), int(row["End"], 16)
                except (KeyError, ValueError):
                    continue
                names.setdefault(a, []).append(row["Name"])
                ranges.append((a, e, row["Name"]))
        else:
            for m in re.finditer(r"^\s*([A-Za-z_][\w.$]*)\s*=\s*0x([0-9A-Fa-f]+)\s*;", text, re.M):
                names.setdefault(int(m.group(2), 16), []).append(m.group(1))
    for a in names:
        seen = []
        for n in names[a]:
            if n not in seen:
                seen.append(n)
        names[a] = sorted(seen, key=lambda n: bool(PLACEHOLDER.search(n)))   # real names first
    ranges.sort()
    return names, ranges, used


def best(names, a):
    lst = names.get(a) or []
    return lst[0] if lst else ""


def is_real(name):
    return bool(name) and not PLACEHOLDER.search(name)


def holder(ranges, a):
    starts = [r[0] for r in ranges]
    i = bisect.bisect_right(starts, a) - 1
    while i >= 0 and ranges[i][0] <= a:
        if a < ranges[i][1]:
            return ranges[i]
        i -= 1
    return None


def short(name):
    """A script-name style word from a handler name: scrCommand_WAIT -> wait, datScrCmd_ADD_MONEY -> add_money."""
    m = re.search(r"(?:ScrCmd|ScrCommand|Command)_([A-Z0-9_]+)$", name)
    return m.group(1).lower() if m else ""


def comm(exe, folder, out, names_out, table, count, paths=None):
    d = open(exe, "rb").read()
    segs = p3fes_bf.elf_segments(d)
    if not segs:
        sys.exit("%s is not a 32-bit little-endian ELF file (give the boot executable, SLUS_216.21)" % exe)
    o = p3fes_bf.addr_to_off(segs, table)
    if o is None or o + 8 * count > len(d):
        sys.exit("the table address 0x%X is not inside %s" % (table, exe))
    names, ranges, used = load(folder, paths)
    if not used:
        sys.exit("no symbol lists in %s: run  python3 %s fetch  first" % (folder, os.path.basename(__file__)))
    ours = dict(p3fes_bf.NAMES)
    lines = ["Script function table at 0x%X of %s, %d records; names from: %s" % (table, os.path.basename(exe), count, ", ".join(map(os.path.basename, used))),
             "", "n | values | handler | name in the symbol lists | ours (tools/script_names.txt)"]
    real = 0
    suggest = []
    for n in range(count):
        ptr, values = struct.unpack_from("<2I", d, o + 8 * n)
        if not ptr:
            continue
        name = best(names, ptr)
        if not name:
            r = holder(ranges, ptr)
            name = "(inside %s+0x%X)" % (r[2], ptr - r[0]) if r else ""
        real += is_real(name)
        mine = ours.get(n, "")
        lines.append("%3d | %d | 0x%06X | %s | %s" % (n, values, ptr, name or "-", mine or "-"))
        if is_real(name) and (not mine or mine.endswith("?")):
            word = short(name) or name
            suggest.append("%d %s   # from the symbol lists: %s at 0x%06X%s" % (n, word, name, ptr, "; ours: " + mine if mine else ""))
    lines += ["", "%d handlers have a real name in the lists; %d of them are new or only guessed in tools/script_names.txt" % (real, len(suggest))]
    open(out, "w", encoding="utf-8").write("\n".join(lines) + "\n")
    print("Wrote %s (%d handlers with a real name, %d suggestions)" % (out, real, len(suggest)))
    if names_out:
        open(names_out, "w", encoding="utf-8").write(
            "# Suggested script function names from the public symbol lists (see p3fes_symbols.py). Check each against the scripts,\n"
            "# then copy the good ones into tools/script_names.txt (a ? at the end marks a guess).\n" + "\n".join(suggest) + "\n")
        print("Wrote %s" % names_out)


def find(folder, addrs, word, paths=None):
    names, ranges, used = load(folder, paths)
    if not used:
        sys.exit("no symbol lists in %s: run  python3 %s fetch  first" % (folder, os.path.basename(__file__)))
    if word:
        hits = sorted((a, n) for a, lst in names.items() for n in lst if word.lower() in n.lower())
        for a, n in hits[:200]:
            print("0x%06X %s" % (a, n))
        print("%d matches" % len(hits))
    for s in addrs:
        a = int(s, 16)
        exact = names.get(a)
        r = holder(ranges, a)
        if exact:
            print("0x%06X %s" % (a, ", ".join(exact)))
        elif r:
            print("0x%06X inside %s (0x%06X-0x%06X) +0x%X" % (a, r[2], r[0], r[1], a - r[0]))
        else:
            print("0x%06X not in the lists" % a)


def main():
    ap = argparse.ArgumentParser(description="Function names from public symbol lists, matched against your executable.")
    ap.add_argument("--dir", default=DEFAULT_DIR, help="folder for the downloaded lists (default ~/.p3fes-symbols)")
    ap.add_argument("--symbols", action="append", help="read this list file instead of the downloaded ones (repeat for more)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("fetch", help="download the two lists")
    c = sub.add_parser("comm", help="the script function table with handler names")
    c.add_argument("exe", help="your boot executable (SLUS_216.21 from the extracted disc)")
    c.add_argument("-o", "--output", default="comm_symbols.txt")
    c.add_argument("--names-out", help="also write suggested names in the script_names.txt format")
    c.add_argument("--table", default=hex(p3fes_bf.COMM_TABLE))
    c.add_argument("--count", type=int, default=p3fes_bf.COMM_COUNT)
    f = sub.add_parser("find", help="which function holds an address, or names containing a word")
    f.add_argument("addr", nargs="*")
    f.add_argument("--name", help="list functions whose name contains this word")
    a = ap.parse_args()
    if a.cmd == "fetch":
        return 0 if fetch(a.dir) else 1
    if a.cmd == "comm":
        comm(a.exe, a.dir, a.output, a.names_out, int(a.table, 16), a.count, a.symbols)
    else:
        find(a.dir, a.addr, a.name, a.symbols)
    return 0


if __name__ == "__main__":
    sys.exit(main())
