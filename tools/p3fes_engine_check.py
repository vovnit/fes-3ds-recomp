#!/usr/bin/env python3
"""Check whether the tool patches fit a version of PS2 Web Recomp, before anyone installs it.

For each version given, it makes a clean copy (git worktree) of that version from your ps2-web-recomp clone, applies every patch
that p3fes_install.sh applies, in the same order, then the Windows patch (ps2run_windows_patch.py) on a second copy, and reports
which patches do not fit. With --build it also builds the patched engine (translator, runtime and the engine's own synthetic
tests, no game data) and runs those tests; that needs cmake, ninja, clang and wgpu-native in third_party/wgpu-native of the clone.

    python3 p3fes_engine_check.py ~/ps2-web-recomp tested next origin/main [--build] [--keep]

Versions: "tested" and "next" are the two named in p3fes_install.sh (PIN and PIN_NEXT); anything else is a git commit, tag or
branch of the clone (run "git fetch" in it first for the newest one). The copies are made in a temporary folder and removed
afterwards unless --keep is given. Your clone itself is not changed. No game data is read.
Exit status: 0 when every patch fits every version (and, with --build, everything builds and the tests pass).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
INSTALL = os.path.join(HERE, "p3fes_install.sh")


def installer():
    text = open(INSTALL).read()
    patches = re.search(r'\nPATCHES="\n(.*?)\n"', text, re.S).group(1).split()
    pins = dict(re.findall(r'^(PIN|PIN_NEXT)=([0-9a-f]{40})', text, re.M))
    return patches, pins


def git(clone, *args):
    return subprocess.run(["git", "-C", clone, *args], capture_output=True, text=True)


def apply(tree, patches):
    """Apply the patches in order; return the list of (patch, last line) that did not fit."""
    bad = []
    for name in patches:
        r = subprocess.run([sys.executable, os.path.join(HERE, name)], cwd=tree, capture_output=True, text=True)
        out = (r.stdout + r.stderr).strip()
        if r.returncode != 0 or "PATCH NOT APPLIED" in out:
            bad.append((name, out.splitlines()[-1] if out else "exit %d" % r.returncode))
    return bad


def build(tree, clone):
    wgpu = os.path.join(clone, "third_party", "wgpu-native")
    if not os.path.isdir(os.path.join(tree, "third_party", "wgpu-native")):
        if not os.path.isdir(wgpu):
            return "no wgpu-native in %s (run scripts/fetch_wgpu.sh there once)" % wgpu
        shutil.copytree(wgpu, os.path.join(tree, "third_party", "wgpu-native"))
    out = os.path.join(tree, "build", "check")
    cxx = os.environ.get("CXX") or shutil.which("clang++") or "clang++"   # the engine uses clang vector builtins
    steps = [["cmake", "-S", tree, "-B", out, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DPS2_BUILD_TESTS=ON", "-DCMAKE_CXX_COMPILER=" + cxx],
             ["ninja", "-C", out]]
    for step in steps:
        r = subprocess.run(step, capture_output=True, text=True)
        if r.returncode != 0:
            errors = [l for l in (r.stdout + r.stderr).splitlines() if "error" in l][:5]
            return "%s failed: %s" % (step[0], " | ".join(errors) or (r.stdout + r.stderr).strip()[-300:])
    if open(os.path.join(tree, "CMakeLists.txt")).read().count("PS2_BUILD_TESTS"):
        r = subprocess.run(["ctest", "--output-on-failure"], cwd=out, capture_output=True, text=True)
        if r.returncode != 0:
            return "engine tests failed: " + r.stdout.strip()[-400:]
    vu = os.path.join(tree, "scripts", "test_vu_codegen.py")
    if os.path.isfile(vu):
        r = subprocess.run([sys.executable, vu], cwd=tree, capture_output=True, text=True)
        if r.returncode != 0:
            return "VU code tests failed: " + (r.stdout + r.stderr).strip()[-400:]
    return ""


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("clone", help="your ps2-web-recomp git clone (for example ~/ps2-web-recomp)")
    ap.add_argument("versions", nargs="+", help="tested, next, or a commit / tag / branch of the clone")
    ap.add_argument("--build", action="store_true", help="also build the patched engine and run its own tests")
    ap.add_argument("--keep", action="store_true", help="keep the patched copies (their folder is printed)")
    a = ap.parse_args()
    clone = os.path.abspath(os.path.expanduser(a.clone))
    if git(clone, "rev-parse", "--git-dir").returncode != 0:
        print("%s is not a git clone of ps2-web-recomp" % clone)
        return 2
    patches, pins = installer()
    names = {"tested": pins.get("PIN"), "next": pins.get("PIN_NEXT")}
    work = tempfile.mkdtemp(prefix="p3fes-engine-")
    ok = True
    try:
        for v in a.versions:
            commit = names.get(v) or v
            sha = git(clone, "rev-parse", "--verify", commit + "^{commit}")
            if sha.returncode != 0:
                print("== %s: not found in the clone (git fetch first?)" % v)
                ok = False
                continue
            sha = sha.stdout.strip()
            day = git(clone, "log", "-1", "--format=%cd", "--date=short", sha).stdout.strip()
            tree = os.path.join(work, sha[:12])
            r = git(clone, "worktree", "add", "-f", "--detach", tree, sha)
            if r.returncode != 0:
                print("== %s: could not make a copy: %s" % (v, r.stderr.strip()))
                ok = False
                continue
            bad = apply(tree, patches)
            win = tree + "-windows"
            shutil.copytree(tree, win, symlinks=True)
            bad += apply(win, ["ps2run_windows_patch.py"])
            print("== %s (%s, %s): %d of %d patches fit" % (v, sha[:12], day, len(patches) + 1 - len(bad), len(patches) + 1))
            for name, line in bad:
                print("   DOES NOT FIT  %s: %s" % (name, line))
            ok = ok and not bad
            if a.build and not bad:
                problem = build(tree, clone)
                print("   build and engine tests: " + (problem or "ok"))
                ok = ok and not problem
            if a.keep:
                print("   patched copy: " + tree)
    finally:
        if not a.keep:
            for d in os.listdir(work):
                git(clone, "worktree", "remove", "-f", os.path.join(work, d))
            shutil.rmtree(work, ignore_errors=True)
            git(clone, "worktree", "prune")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
