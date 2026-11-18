#!/usr/bin/env python3
"""Checks tools/p3fes_3ds_movies.py and the 3DS build's movie decoder (port3ds/mpeg1.cpp) with a made-up movie: no game data.
Needs ffmpeg and a C++ compiler; without them it says so and passes.    python3 tests/test_3ds_movies.py"""
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import p3fes_3ds_movies as M

RUNTIME = os.path.join(HERE, "..", "..", "ps2-web-recomp", "runtime", "src")


def fake_movie(folder, pictures=45):
    """A movie laid out like the game's: header sectors, then video packets with a sound sector after every third, then the end."""
    es = subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", "testsrc2=s=640x368:r=30000/1001", "-frames:v", str(pictures), "-c:v", "mpeg1video",
                         "-g", "12", "-bf", "2", "-b:v", "3000k", "-f", "mpeg1video", "-"], stdout=subprocess.PIPE, check=True).stdout
    starts = M.find_all(es, b"\0\0\1\0")
    sectors = [b"\0\0\1\xba" + bytes(8) + b"\0\0\1\xbe" + struct.pack(">H", 2030) + b"\x0f" + b"\xff" * 2029] * 2
    at, n = 0, 0
    while at < len(es):
        body = es[at:at + 2018]
        first = [i for i, s in enumerate(starts) if at <= s < at + 2018]
        head = b"\xff" * 5 + b"\x60\x2e" + M.put_pts(2, 9000 + first[0] * M.FRAME) if first else M.pes_header(None)
        sec = b"\0\0\1\xba" + bytes(8) + b"\0\0\1\xe0" + struct.pack(">H", 12 + len(body)) + head + body
        if len(body) < 2018:
            rest = 2018 - len(body)
            sec += (b"\0\0\1\xbe" + struct.pack(">H", rest - 6) + b"\x0f" + b"\xff" * (rest - 7)) if rest >= 7 else bytes(rest)
        sectors.append(sec)
        at += 2018
        n += 1
        if n % 3 == 0:
            sectors.append(b"\0\0\1\xba" + bytes(8) + b"\0\0\1\xc0" + struct.pack(">H", 2030) + bytes([n & 255]) * 2030)
    sectors.append(b"\0\0\1\xb9" + b"\xff" * 2044)
    assert all(len(s) == 2048 for s in sectors)
    return b"".join(sectors), len(starts)


def main():
    assert M.read_marker(M.marker_bits(41, 10937)) == (41, 10937)
    bad = M.marker_bits(3, 77)
    bad[20] ^= 1
    assert M.read_marker(bad) is None
    if not shutil.which("ffmpeg") or not shutil.which("c++") or not os.path.isdir(RUNTIME):
        print("ok   3DS movies: markers (ffmpeg, c++ or ../ps2-web-recomp missing: the rest was not run)")
        return
    with tempfile.TemporaryDirectory() as folder:
        data, pictures = fake_movie(folder)
        entries, video, found, size = M.cut_movie(data, 7)
        assert found == pictures and size == (640, 368) and entries
        cut_path = os.path.join(folder, "movies.cut")
        M.write_cut(cut_path, [("TEST", 100, len(data) // 2048, entries, pictures, 640, 368, 7)])
        cut = M.Cut(cut_path)
        new = b"".join(cut.sector(0, k, data[k * 2048:(k + 1) * 2048]) for k in range(len(data) // 2048))
        # Sound, header and end sectors are untouched, and every sector still starts as a pack.
        for k in range(len(data) // 2048):
            old, now = data[k * 2048:(k + 1) * 2048], new[k * 2048:(k + 1) * 2048]
            assert now[:12] == old[:12]
            if old[12:16] != b"\0\0\1\xe0":
                assert now == old
        raw = subprocess.run(["ffmpeg", "-v", "error", "-f", "mpegvideo", "-i", "-", "-vf", "crop=640:16:0:0", "-pix_fmt", "gray", "-f", "rawvideo", "-"],
                             input=M.scan(new)[1], stdout=subprocess.PIPE, check=True).stdout
        assert len(raw) == pictures * 640 * 16, "the cheap movie has another number of pictures"
        for n in range(pictures):
            row = raw[n * 640 * 16 + 8 * 640:n * 640 * 16 + 9 * 640]
            assert M.read_marker([1 if row[i * 16 + 8] > 128 else 0 for i in range(M.MARK_BITS)]) == (7, n), "marker of picture %d" % n
        assert len(M.scan(new)[1]) * 4 < len(M.scan(data)[1]), "the cheap movie is not much smaller"

        # The 400x240 copy, decoded by the 3DS build's decoder and by ffmpeg.
        src = os.path.join(folder, "movie.sfd")
        with open(src, "wb") as f:
            f.write(data)
        p3v = os.path.join(folder, "07.p3v")
        shown, _size = M.encode_p3v(src, p3v, 30, 5)
        assert shown == pictures
        exe = os.path.join(folder, "mpeg1_check")
        subprocess.run(["c++", "-std=c++20", "-O2", "-I" + RUNTIME, os.path.join(HERE, "mpeg1_check.cpp"), os.path.join(HERE, "..", "port3ds", "mpeg1.cpp"), "-o", exe], check=True)
        mine_path = os.path.join(folder, "mine.yuv")
        subprocess.run([exe, p3v, mine_path], check=True, stderr=subprocess.DEVNULL)
        blob = open(p3v, "rb").read()
        first = struct.unpack_from("<I", blob, 16)[0]
        ref = subprocess.run(["ffmpeg", "-v", "error", "-f", "mpegvideo", "-i", "-", "-pix_fmt", "yuv420p", "-f", "rawvideo", "-"], input=blob[first:], stdout=subprocess.PIPE, check=True).stdout
        mine = open(mine_path, "rb").read()
        frame = 400 * 240 * 3 // 2
        assert len(mine) == len(ref) == pictures * frame
        worst = 99.0
        for n in range(pictures):
            a, b = ref[n * frame:(n + 1) * frame:5], mine[n * frame:(n + 1) * frame:5]
            error = sum((x - y) * (x - y) for x, y in zip(a, b)) / len(a)
            worst = min(worst, 10 * math.log10(255 * 255 / error) if error else 99.0)
        assert worst > 45, "the decoder is %.1f dB from ffmpeg" % worst
    print("ok   3DS movies: markers, the cheap movie keeps sound and layout, the decoder matches ffmpeg (worst picture %.0f dB)" % worst)


if __name__ == "__main__":
    main()
