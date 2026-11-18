"""p3fes_symbols.py: the script function table of a made-up executable gets handler names from both list formats."""
import os, struct, subprocess, sys, tempfile

TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "p3fes_symbols.py")
d = tempfile.mkdtemp()

# a 32-bit little-endian ELF with one loadable segment holding the table at 0x7B92B0
BASE, TABLE = 0x7B9000, 0x7B92B0
recs = [(0, 0)] * 13
recs[7] = (0x35AEB0, 1)    # a real name in the csv list
recs[12] = (0x35B030, 1)   # a real name in the csv list, ours already has wait_frames
recs[8] = (0x16EE00, 2)    # a real name in the decomp list only
recs[9] = (0x35AF60, 1)    # inside a function, not at its start
recs[10] = (0x1000, 0)     # in no list
body = bytearray(TABLE - BASE) + b"".join(struct.pack("<2I", *r) for r in recs)
ehdr = b"\x7fELF" + bytes([1, 1, 1]) + bytes(9) + struct.pack("<HHIIIIIHHHHHH", 2, 8, 1, 0x100008, 52, 0, 0, 52, 32, 1, 40, 0, 0)
phdr = struct.pack("<8I", 1, 0x100, BASE, BASE, len(body), len(body), 5, 16)
elf = ehdr + phdr + bytes(0x100 - len(ehdr) - len(phdr)) + bytes(body)
exe = os.path.join(d, "SLUS_000.00")
open(exe, "wb").write(elf)

csvp = os.path.join(d, "map.csv")
open(csvp, "w").write("Name,Start,End,Size\nscrCommand_BIT_CHK,0x0035AEB0,0x0035AEF0,64\nscrCommand_BIT_OFF,0x0035AF50,0x0035AFB0,96\n"
                      "scrCommand_WAIT,0x0035B030,0x0035B090,96\ng_data_0016ee00,0x0016EE00,0x0016EE90,144\n")
txtp = os.path.join(d, "symbol_addrs.txt")
open(txtp, "w").write("// header\ndatScrCmd_ADD_MONEY = 0x0016EE00; // type:func\nscrCommand_BIT_CHK = 0x0035AEB0; // type:func\n")

out, names = os.path.join(d, "report.txt"), os.path.join(d, "names.txt")
r = subprocess.run([sys.executable, TOOL, "--symbols", csvp, "--symbols", txtp, "comm", exe, "--count", "13", "-o", out, "--names-out", names], capture_output=True, text=True)
assert r.returncode == 0, r.stdout + r.stderr
rep = open(out).read()
assert "  7 | 1 | 0x35AEB0 | scrCommand_BIT_CHK | flag_test?" in rep, rep
assert "  8 | 2 | 0x16EE00 | datScrCmd_ADD_MONEY |" in rep, rep          # the real name wins over the placeholder
assert "  9 | 1 | 0x35AF60 | (inside scrCommand_BIT_OFF+0x10) |" in rep, rep
assert " 10 | 0 | 0x001000 | - |" in rep and "\n  0 |" not in rep, rep    # empty records are left out
sug = open(names).read()
assert "7 bit_chk   # from the symbol lists: scrCommand_BIT_CHK at 0x35AEB0; ours: flag_test?" in sug, sug
assert "8 add_money" in sug and "\n12 " not in sug, sug                    # 12 already has a sure name (wait_frames)
print("ok   symbol lists name the script function handlers of an executable, and suggest names only where ours are missing or guessed")

r = subprocess.run([sys.executable, TOOL, "--symbols", csvp, "--symbols", txtp, "find", "0x35B040", "--name", "money"], capture_output=True, text=True)
assert "0x16EE00 datScrCmd_ADD_MONEY" in r.stdout and "0x35B040 inside scrCommand_WAIT" in r.stdout, r.stdout
r = subprocess.run([sys.executable, TOOL, "--dir", os.path.join(d, "empty"), "find", "0x1"], capture_output=True, text=True)
assert r.returncode != 0 and "fetch" in r.stdout + r.stderr
print("ok   symbol lookup by address and by word; a missing list says to fetch first")
