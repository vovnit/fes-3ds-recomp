"""tools/p3fes_eehot.py on a made-up unit: the copy keeps registers in locals and writes them back where it must."""
import os, subprocess, sys, tempfile
here = os.path.dirname(os.path.abspath(__file__))
tool = os.path.join(here, "..", "tools", "p3fes_eehot.py")
UNIT = '''#include "units.hpp"

namespace gen {

using namespace ps2;

const std::uint16_t entries_0000[4] = {
    1, 0, 2, 3,
};

void unit_0000(Cpu &c, std::uint32_t entry) {
dispatch_entry:
    switch (entry) {
    case 1u: goto L_00100000;
    case 2u: goto L_00100020;
    case 3u: goto L_00100040;
    default: bad_entry(c, 0x00100000u); return;
    }
L_00100000:
    set32(c.r[29], c.r[29].w[0] - 16u);
    sp_st64(c.r[29].w[0], 0u, c.r[31].d[0]);
    c.r[16].d[0] = c.r[4].d[0] + 0ull;
    set32(c.r[2], ld32((c.r[28].w[0] - 100u)));
    c.budget -= 6;
    { c.r[31].sd[0] = static_cast<std::int32_t>(0x00100020u); set32(c.r[4], c.r[2].w[0] + 1u);  if (stack_ok()) unit_0000(c, 3u); else { c.pc = 0x00100040u; return; } if (c.pc != 0x00100020u) [[unlikely]] return; }
L_00100020:
    set32(c.r[2], c.r[2].w[0] + c.r[16].w[0]);
    c.r[31].d[0] = sp_ld64(c.r[29].w[0], 0u);
    c.budget -= 4;
    { const std::uint32_t t = c.r[31].w[0]; set32(c.r[29], c.r[29].w[0] + 16u);  c.pc = t; return; }
L_00100040:
    c.budget -= 2;
    { const std::uint32_t t = c.r[31].w[0]; set32(c.r[2], c.r[4].w[0] << 1);  c.pc = t; return; }
}

}
'''
with tempfile.TemporaryDirectory() as d:
    src, out = os.path.join(d, "src"), os.path.join(d, "out")
    os.makedirs(src)
    with open(os.path.join(src, "unit_0000.cpp"), "w") as f:
        f.write(UNIT)
    with open(os.path.join(d, "prof.txt"), "w") as f:
        f.write("L_00100000 0 600\nL_00100020 0 400\nL_00100040 0 200\n")
    with open(os.path.join(d, "fn.txt"), "w") as f:
        f.write("00100000\n00100040\n")
    r = subprocess.run([sys.executable, tool, "make", src, out, "--profile", os.path.join(d, "prof.txt"), "--functions", os.path.join(d, "fn.txt"),
                        "--natives-out", os.path.join(d, "nat.txt"), "--gp", "007D2CF0"], capture_output=True, text=True)
    assert r.returncode == 0 and "2 hot copies" in r.stdout, r.stdout + r.stderr
    text = open(os.path.join(out, "hot_00.cpp")).read()
    assert open(os.path.join(d, "nat.txt")).read().split() == ["00100000", "hot_00100000", "00100040", "hot_00100040"]
    first = text[text.index("bool hot_00100000"):text.index("bool hot_00100040")]
    assert "if (c.r[28].w[0] != 0x007d2cf0u) return false;" in first            # fixed gp, checked once
    assert "put32(x2, y2, bud_ld32(c, bud, (x28 - 100u)));" in first               # registers and the budget are locals
    call = [l for l in first.split("\n") if "hot_00100040(c)" in l][0]
    assert "c.r[4].w[0] = x4;" in call and "c.r[31].w[0] = x31;" in call and "c.budget = bud;" in call   # written back before the call
    assert "c.r[2].w[0] = x2;" in call                                             # also a temporary: the callee might read it
    assert "x2 = c.r[2].w[0];" in call and "x16 = c.r[16].w[0];" in call          # and read again after it, where still used
    assert "return true;" in first and "return;" not in first.replace("return true;", "").replace("return false;", "")
    r = subprocess.run([sys.executable, tool, "make", src, out, "--profile", os.path.join(d, "prof.txt"), "--functions", os.path.join(d, "fn.txt"),
                        "--trust", "2", "--trust-check"], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    text = open(os.path.join(out, "hot_00.cpp")).read()
    call = [l for l in text.split("\n") if "hot_00100040(c)" in l][0]
    assert "c.r[2].w[0] = x2;" not in call and "c.r[2].w[0] = 0x5EED0200u;" in call and "c.r[4].w[0] = x4;" in call   # a temporary is marked, an argument written
print("ok   p3fes_eehot.py writes hot copies that write registers back before calls and read them after")
