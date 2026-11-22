import os, re, sys
here = os.path.dirname(os.path.abspath(__file__))
folder = os.path.join(here, "..", "port3ds")
listed = []
with open(os.path.join(folder, "natives.txt")) as f:
    for line in f:
        address, name = line.split()
        assert re.fullmatch(r"[0-9A-F]{8}", address), address
        listed.append((address, name))
assert len({a for a, _ in listed}) == len(listed), "a guest address is listed twice"
# Both SDK cache operations use the same native and charge the same walk.
assert {a for a, name in listed if name == "cache_walk"} == {"005039B0", "00503AF0"}
unique_names = [n for _, n in listed if n != "cache_walk" and not n.startswith("loop_")]
assert len(set(unique_names)) == len(unique_names), "a native name is listed twice"
with open(os.path.join(folder, "native3ds.cpp")) as f:
    source = f.read()
defined = set(re.findall(r"^(?:bool|void) native_(\w+)\(Cpu &c\)", source, re.M)) | set(re.findall(r"^MOTION\((\w+),", source, re.M))
missing = [n for _, n in listed if n not in defined]
assert not missing, missing
unused = {n for n in defined - {n for _, n in listed} if not n.endswith("_check")}   # check hooks are listed only for a check build
assert not unused, unused
print("ok   every native function in natives.txt exists, and every one that exists is listed")
