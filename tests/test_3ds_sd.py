import os, struct, subprocess, sys, tempfile
here = os.path.dirname(os.path.abspath(__file__))
tool = os.path.join(here, "..", "tools", "p3fes_3ds_sd.py")
SECTOR = 2048
def record(name, lba, size):
    n = name.encode()
    length = 33 + len(n) + (0 if len(n) % 2 else 1)
    r = bytearray(length)
    r[0] = length
    r[2:6] = struct.pack("<I", lba)
    r[10:14] = struct.pack("<I", size)
    r[32] = len(n)
    r[33:33 + len(n)] = n
    return bytes(r)
def make_iso(path, boot):
    total = 40 * SECTOR
    img = bytearray(os.urandom(total))
    pvd = bytearray(SECTOR)
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[158:162] = struct.pack("<I", 20)
    pvd[166:170] = struct.pack("<I", SECTOR)
    img[16 * SECTOR:17 * SECTOR] = pvd
    folder = bytearray(SECTOR)
    entry = record("SLUS_216.21;1", 21, len(boot))
    folder[:len(entry)] = entry
    img[20 * SECTOR:21 * SECTOR] = folder
    img[21 * SECTOR:21 * SECTOR + len(boot)] = boot
    with open(path, "wb") as f:
        f.write(img)
    return bytes(img)
with tempfile.TemporaryDirectory() as d:
    iso, recomp, out = os.path.join(d, "disc.iso"), os.path.join(d, "recomp"), os.path.join(d, "sd")
    boot = b"\x7fELF" + os.urandom(500)
    img = make_iso(iso, boot)
    before = open(iso, "rb").read()
    run = lambda *extra: subprocess.run([sys.executable, tool, iso, recomp, "-o", out, "--part-size", "16384", *extra], capture_output=True, text=True)
    # Nothing built yet: a plain message, not a crash, and nothing written.
    r = run()
    assert r.returncode == 1 and "Build first" in r.stdout and not os.path.exists(out), r.stdout + r.stderr
    os.makedirs(os.path.join(recomp, "build", "3ds"))
    for name in ("p3fes.3dsx", "probe.3dsx"):
        with open(os.path.join(recomp, "build", "3ds", name), "wb") as f:
            f.write(name.encode())
    r = run()
    assert r.returncode == 0 and "match your disc image" in r.stdout, r.stdout + r.stderr
    assert open(os.path.join(out, "p3fes", "SLUS_216.21"), "rb").read() == boot
    assert open(os.path.join(out, "3ds", "p3fes.3dsx"), "rb").read() == b"p3fes.3dsx"
    assert open(os.path.join(out, "3ds", "p3fes-probe.3dsx"), "rb").read() == b"probe.3dsx"
    parts = sorted(p for p in os.listdir(os.path.join(out, "p3fes")) if p.startswith("disc."))
    assert parts == ["disc.%02d" % i for i in range(5)], parts
    assert b"".join(open(os.path.join(out, "p3fes", p), "rb").read() for p in parts) == img
    # A second run keeps finished parts, and a damaged one is caught instead of trusted.
    r = run()
    assert "already there" in r.stdout and r.returncode == 0, r.stdout
    with open(os.path.join(out, "p3fes", "disc.01"), "r+b") as f:
        f.write(b"\xff" * 16)
    r = run()
    assert r.returncode == 1 and "do NOT match" in r.stdout, r.stdout
    assert open(iso, "rb").read() == before
    # A different disc is refused.
    with open(iso, "wb") as f:
        f.write(bytes(40 * SECTOR))
    r = run()
    assert r.returncode == 1 and "does not look like" in r.stdout, r.stdout
print("ok   3DS SD tool splits the disc, verifies the parts, resumes, and refuses a wrong disc")
