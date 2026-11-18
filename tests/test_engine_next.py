"""The tool patches also fit the newer PS2 Web Recomp (12 Nov 2026, PIN_NEXT in p3fes_install.sh).

Small made-up copies of the places that changed upstream: each patch must take the newer text, apply once, say so the second time,
and the fixes that are now part of the engine (PATH3 mask, VU branch delay) must stand down instead of failing."""
import os, subprocess, sys, tempfile

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools")


def run(patch, cwd, *args):
    r = subprocess.run([sys.executable, os.path.join(TOOLS, patch), *args], cwd=cwd, capture_output=True, text=True)
    return r.returncode, (r.stdout + r.stderr).strip()


def tree(files):
    d = tempfile.mkdtemp()
    for rel, text in files.items():
        os.makedirs(os.path.dirname(os.path.join(d, rel)), exist_ok=True)
        open(os.path.join(d, rel), "w").write(text)
    return d


def read(d, rel):
    return open(os.path.join(d, rel)).read()


def twice(patch, d, *args):
    rc, out = run(patch, d, *args)
    assert rc == 0 and "NOT APPLIED" not in out, (patch, out)
    rc2, out2 = run(patch, d, *args)
    assert rc2 == 0 and "already patched" in out2, (patch, out2)
    return out


# fixes the engine has itself now
d = tree({"runtime/src/gs/gif.cpp": "struct Held {};\nvoid vif1_idle() {}\n", "runtime/src/gs/gif.hpp": "", "runtime/src/system.cpp": "",
          "runtime/src/dmac.cpp": ""})
rc, out = run("ps2run_fix_path3_mask.py", d)
assert rc == 0 and "not needed" in out and "e615191" in out, out
for patch, rel in (("ps2run_vubranch_patch.py", "runtime/src/vu_micro.cpp"), ("ps2web_vubranch_patch.py", "runtime/src/vu_jit.cpp"),
                   ("ps2recomp_vubranch_patch.py", "recomp/vurecomp.cpp")):
    d = tree({rel: "bool branch_delay_exempt(unsigned lo);\n"})
    rc, out = run(patch, d, os.path.join(d, rel))
    assert rc == 0 and "not needed" in out, (patch, out)
print("ok   PATH3 and VU branch patches stand down on an engine that has those fixes")

# keys: the newer engine moved the stick to I J K L already
KEYS = """    {SDL_SCANCODE_UP, kUp},         {SDL_SCANCODE_DOWN, kDown},     {SDL_SCANCODE_LEFT, kLeft},
    {SDL_SCANCODE_RIGHT, kRight},   {SDL_SCANCODE_Z, kCross},       {SDL_SCANCODE_X, kCircle},
    {SDL_SCANCODE_A, kSquare},      {SDL_SCANCODE_S, kTriangle},    {SDL_SCANCODE_Q, kL1},
    {SDL_SCANCODE_W, kR1},          {SDL_SCANCODE_1, kL2},          {SDL_SCANCODE_2, kR2},
    {SDL_SCANCODE_RETURN, kStart},  {SDL_SCANCODE_BACKSPACE, kSelect},
"""
STICK = """        if (keys[SDL_SCANCODE_J]) lx = 0x00;
        if (keys[SDL_SCANCODE_L]) lx = 0xFF;
        if (keys[SDL_SCANCODE_I]) ly = 0x00;
        if (keys[SDL_SCANCODE_K]) ly = 0xFF;
"""
RIGHT = "        rx = axis(pad, SDL_CONTROLLER_AXIS_RIGHTX);\n        ry = axis(pad, SDL_CONTROLLER_AXIS_RIGHTY);\n"
d = tree({"host/window.cpp": KEYS + STICK + RIGHT})
rc, out = run("ps2run_controls_patch.py", d)
w = read(d, "host/window.cpp")
assert rc == 0 and "SDL_SCANCODE_W]) ly = 0x00" in w and "SDL_SCANCODE_K, kCross" in w and "SDL_SCANCODE_J]) lx" not in w, out
print("ok   PC keys patch takes the newer stick keys")

# the no-handler message now also names the EE buffer
d = tree({"sif.cpp": '        if (it == g_cmd_handlers.end()) {\n'
                     '            std::fprintf(stderr, "[sif] command 0x%08X has no handler (from ee buffer 0x%08X)\\n", h->cid, src);\n'
                     '            break;\n'})
twice("ps2run_sifquiet_patch.py", d, os.path.join(d, "sif.cpp"))
s = read(d, "sif.cpp")
assert "(from ee buffer 0x%08X) (shown once)\\n\", h->cid, src);" in s and s.count("seenCommands[32]") == 1, s
print("ok   SIF message patch keeps the newer message words")

# memory pokes: a write watch sits between the copy and the clear
d = tree({"runtime/src/system.cpp": "#include <cstdlib>\nbool load() {\n    for (;;) {\n"
                                    "        std::memcpy(g_ram + vaddr, data.data() + offset, filesz);\n"
                                    '        watch_span(vaddr, filesz, "boot:load_exec");\n'
                                    "        std::memset(g_ram + vaddr + filesz, 0, memsz - filesz);\n    }\n    return true;\n}\n"})
twice("ps2run_poke_patch.py", d)
s = read(d, "runtime/src/system.cpp")
assert 'watch_span(vaddr, filesz, "boot:load_exec");' in s and s.index("watch_span") < s.index("PS2_POKE"), s
print("ok   memory poke patch fits the newer program loader")

# widescreen: the engine's own 16:9 switch; the code must end the same as on the older engine
GPU = ("std::uint32_t g_scale = 1, g_copy_words = 1u << 20, g_field_words = 1u << 19;\n"
       "void a() {\n        const float aspect = g_options.widescreen ? 16.0f / 9.0f : 4.0f / 3.0f;\n"
       "        const float vw = std::min(w, h * aspect), vh = vw / aspect;\n}\n"
       "void b() {\n    const float aspect = g_options.widescreen ? 16.0f / 9.0f : 4.0f / 3.0f;\n"
       "    const float vw = std::min(sw, sh * aspect), vh = vw / aspect;\n"
       '    if (const char *scale = std::getenv("PS2_SCALE")) g_scale = 2;\n}\n')
SYS = ("#include <algorithm>\nvoid vblank() {\n    apply_presses();\n    apply_patches(false);\n"
       "    if (g_options.on_vblank) g_options.on_vblank();\n}\n")
d = tree({"runtime/src/gs/gpu/gpu.cpp": GPU, "runtime/src/system.cpp": SYS})
twice("ps2run_widescreen_patch.py", d)
g, s = read(d, "runtime/src/gs/gpu/gpu.cpp"), read(d, "runtime/src/system.cpp")
assert g.count("vw = std::min(w, h * g_aspect), vh = vw / g_aspect;") == 1 and "if (g_options.widescreen) g_aspect = 16.0f / 9.0f;" in g, g
assert "ws_apply();\n#endif\n    apply_patches(false);" in s, s
twice("ps2run_widescreen_web_patch.py", d)
rc, out = run("ps2run_widescreen_patch.py", d)
assert "already patched" in out, out
print("ok   widescreen patches fit the engine's own 16:9 switch and leave the same code for the patches after them")

# browser page: buttons in a Menu (host-only), a <video> in the stage, the engine script in a template
SHELL = ('<style>\n</style>\n<div id="stage"><canvas id="canvas" tabindex="0"></canvas><video id="remote" playsinline></video></div>\n'
         '<template id="engine">{{{ SCRIPT }}}</template>\n<script>\nlet x = 1;\n</script>\n</body>\n</html>\n')
d = tree({"web/shell.html": SHELL})
twice("ps2web_speedrun_patch.py", d)
twice("ps2web_gamebeat_patch.py", d)
h = read(d, "web/shell.html")
assert '<video id="remote" playsinline></video></div>\n<div id="srclock" hidden>' in h, h
assert h.index("ps2-speedrun: real-time") < h.index("</body>") and h.index("ps2-gamebeat") < h.index("</body>"), h
assert '<template id="engine">{{{ SCRIPT }}}</template>' in h, h
print("ok   browser clock and launcher heartbeat go at the end of the newer page")

# translator start: port.sh passes more lists before --out
d = tree({"scripts/port.sh": 'translate() { "$tools/eerecomp" "$elf" --entries "$game/entries.txt" "${modules[@]}" "${overlays[@]}" '
                             '"${templates[@]}" --out "$generated" | tail -1; }\n'})
twice("ps2port_modswitch_patch.py", d)
assert '"${templates[@]}" "${variants[@]}" --out "$generated"' in read(d, "scripts/port.sh")
print("ok   mod switch table reaches the newer translator command line")

# the installer offers the newer engine and the launcher asks the newer page for 4:3
inst = open(os.path.join(TOOLS, "p3fes_install.sh")).read()
assert "PIN_NEXT=d8a0a452274447b8e1da984bba1a82f7716e3e8f" in inst and '--engine) case "$2" in tested) ;; next) PIN="$PIN_NEXT"' in inst
print("ok   installer has --engine tested|next|commit")
