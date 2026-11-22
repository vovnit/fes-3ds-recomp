#!/usr/bin/env python3
"""
p3fes_3ds_movies.py - make the game's movies cheap for the New 3DS build, from your own disc image.

    python3 p3fes_3ds_movies.py make  GAME.iso -o FOLDER [--only NAME ...] [--fps 30] [--quality 5]
    python3 p3fes_3ds_movies.py check GAME.iso FOLDER/movies.cut
    python3 p3fes_3ds_movies.py patch GAME.iso FOLDER/movies.cut COPY.iso      (test aid: the same change written into a copy of the image)

The console cannot decode a 640-wide MPEG movie through the translated game code at full speed. So the build plays its own copy
of each picture, made here at the top screen's size, and the game underneath gets a movie that costs it almost nothing:

  FOLDER/movies.cut     for every video sector of every movie on the disc, what the 3DS build hands the game instead: a black
                        picture only one macroblock row high. Sound sectors, sector positions and
                        time stamps stay as they are, so the game's own player keeps its timing, its sound and its skip button.
                        The top row of each black picture carries a marker (which movie, which picture) as black and white blocks.
                        The build reads the marker when the game converts the picture for display and shows that picture of its own.
  FOLDER/NAME.p3v       the movie's pictures at 400x240 (see port3ds/movie3ds.cpp for the format: MPEG-1 video, I and P pictures)

Copy the folder's contents to sdmc:/p3fes/movies/. Nothing here leaves your computer. Needs ffmpeg.
"""
import argparse
import bisect
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from p3fes_subs import ffmpeg_exe, movies

SECTOR = 2048
FRAME = 3003                 # 90 kHz ticks a picture at 29.97 a second
MAGIC = 0xB2                 # first eight marker blocks
MARK_BITS = 32               # magic 8, movie 6, picture 16, parity 2
CUT_MAGIC = b"P3CUT2\0\0"
OLD_CUT_MAGIC = b"P3CUT1\0\0"
P3V_MAGIC = b"P3V1"


def marker_bits(movie, picture):
    v = (MAGIC << 22) | ((movie & 0x3F) << 16) | (picture & 0xFFFF)
    bits = [(v >> (29 - i)) & 1 for i in range(30)]
    return bits + [sum(bits[0::2]) & 1, sum(bits[1::2]) & 1]


def read_marker(bits):
    """(movie, picture) or None. The 3DS build has the same check."""
    if len(bits) < MARK_BITS or (sum(bits[0:30:2]) & 1) != bits[30] or (sum(bits[1:30:2]) & 1) != bits[31]:
        return None
    v = 0
    for b in bits[:30]:
        v = (v << 1) | b
    if v >> 22 != MAGIC:
        return None
    return (v >> 16) & 0x3F, v & 0xFFFF


def read_pts(b):
    return (((b[0] >> 1) & 7) << 30) | (b[1] << 22) | ((b[2] >> 1) << 15) | (b[3] << 7) | (b[4] >> 1)


def put_pts(marker, t):
    return bytes([(marker << 4) | (((t >> 30) & 7) << 1) | 1, (t >> 22) & 0xFF, (((t >> 15) & 0x7F) << 1) | 1, (t >> 7) & 0xFF, ((t & 0x7F) << 1) | 1])


def video_payload(sec):
    """(offset, length) of the video bytes of a sector, or None if it is not a video sector."""
    if sec[12:16] != b"\0\0\1\xe0":
        return None
    ln = struct.unpack(">H", sec[16:18])[0]
    return 30, ln - 12


def pes_header(pts):
    """The 12 bytes between a video packet's length and its payload."""
    if pts is None:
        return b"\xff" * 9 + b"\x60\x2e\x0f"
    return b"\xff" * 5 + b"\x60\x2e" + put_pts(2, pts)


def scan(data):
    """Video sectors [(sector, offset, length)], the video stream, the stream position each sector starts at, the lowest time stamp."""
    sectors, starts, es, low = [], [], bytearray(), None
    for k in range(len(data) // SECTOR):
        sec = data[k * SECTOR:(k + 1) * SECTOR]
        p = video_payload(sec)
        if not p:
            continue
        h = sec[18:30]
        i = 0
        while h[i] == 0xFF:
            i += 1
        if h[i] >> 6 == 1:
            i += 2
        if h[i] >> 4 in (2, 3):
            t = read_pts(h[i:i + 5])
            low = t if low is None else min(low, t)
        sectors.append((k, p[0], p[1]))
        starts.append(len(es))
        es += sec[p[0]:p[0] + p[1]]
    return sectors, bytes(es), starts, low


def find_all(es, code):
    out, pos = [], 0
    while True:
        i = es.find(code, pos)
        if i < 0:
            return out
        out.append(i)
        pos = i + 4


def picture_size(es):
    s = es.find(b"\0\0\1\xb3")
    if s < 0:
        raise SystemExit("no sequence header in the video")
    w = (es[s + 4] << 4) | (es[s + 5] >> 4)
    h = ((es[s + 5] & 15) << 8) | es[s + 6]
    mpeg2 = es.find(b"\0\0\1\xb5", s, s + 256) >= 0
    return w, h, es[s + 7], mpeg2


def units(es):
    """The stream cut into pictures, each with the headers that come before it."""
    pics = find_all(es, b"\0\0\1\0")
    out = []
    for i, p in enumerate(pics):
        start = p
        if i:
            for code in (b"\0\0\1\xb8", b"\0\0\1\xb3"):
                j = es.rfind(code, pics[i - 1] + 4, start)
                if j >= 0:
                    start = j
        else:
            start = 0
        out.append(start)
    return [es[a:b] for a, b in zip(out, out[1:] + [len(es)])]


def cheap_stream(width, height, rate_byte, mpeg2, movie, count, gop=12):
    """A black movie of `count` pictures with the marker in the top row, as an MPEG video stream with no B pictures."""
    codec = "mpeg2video" if mpeg2 else "mpeg1video"
    cmd = [ffmpeg_exe(), "-v", "error", "-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "%dx%d" % (width, height), "-r", "30000/1001", "-i", "-",
           "-c:v", codec, "-g", str(gop), "-bf", "0", "-q:v", "6", "-flags", "+cgop", "-sc_threshold", "1000000000", "-f", codec, "-"]
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    out = bytearray()
    import threading
    t = threading.Thread(target=lambda: out.extend(p.stdout.read()))
    t.start()
    luma = bytearray(b"\x10" * (width * height))
    chroma = b"\x80" * (width * height // 2)
    white, black = b"\xeb" * 16, b"\x10" * 16
    last = None
    for n in range(count):
        bits = marker_bits(movie, n)
        for i, b in enumerate(bits):
            if last is None or last[i] != b:
                for y in range(16):
                    luma[y * width + i * 16:y * width + i * 16 + 16] = white if b else black
        last = bits
        p.stdin.write(luma)
        p.stdin.write(chroma)
    p.stdin.close()
    t.join()
    if p.wait() != 0:
        raise SystemExit("ffmpeg failed")
    es = bytearray(out)
    for s in find_all(es, b"\0\0\1\xb3"):
        es[s + 7] = rate_byte                  # the original's shape and picture rate codes
    return bytes(es)


def cut_movie(data, movie):
    """([(sector, flags, pts, bytes)] for the video sectors that carry something, count of video sectors, pictures, (w, h))."""
    sectors, es, starts, low = scan(data)
    if not sectors:
        return [], 0, 0, (0, 0)
    w, h, rate_byte, mpeg2 = picture_size(es)
    pics = find_all(es, b"\0\0\1\0")
    for gop in (300, 3000, 0):
        if not gop:
            return None, len(sectors), len(pics), (w, h)       # a movie smaller than its cheap copy stays as it is
        new = units(cheap_stream(w, 16, rate_byte, mpeg2, movie, len(pics), gop))
        if len(new) != len(pics):
            raise SystemExit("made %d pictures for %d" % (len(new), len(pics)))
        entries = lay_out(sectors, starts, pics, new, low)
        if entries is not None:
            return entries, len(sectors), len(pics), (w, h)


def lay_out(sectors, starts, pics, new, low):
    # Each new picture starts where the original picture with that number in the stream started, or earlier when what
    # follows would not fit. Zero bytes between pictures are legal stuffing.
    cstart, total = [], 0
    for _k, _off, cap in sectors:
        cstart.append(total)
        total += cap
    at = [0] * len(new)
    limit = total
    for j in range(len(new) - 1, -1, -1):
        target = cstart[bisect.bisect_right(starts, pics[j]) - 1]
        at[j] = min(target, limit - len(new[j]))
        limit = at[j]
    if limit < 0:             # more small pictures at the very start than the first sectors hold: those come a little late instead
        end = 0
        for j, u in enumerate(new):
            at[j] = max(at[j], end)
            end = at[j] + len(u)
        if end > total:
            return None
    laid = bytearray(total)
    for j, u in enumerate(new):
        laid[at[j]:at[j] + len(u)] = u
    entries, j = [], 0
    for i, (k, _off, cap) in enumerate(sectors):
        pts = None
        while j < len(at) and at[j] < cstart[i] + cap:
            if pts is None:
                pts = low + j * FRAME
            j += 1
        last = j - 1                                  # the last picture that starts in this sector or before it
        used = min(at[last] + len(new[last]), cstart[i] + cap) - cstart[i] if last >= 0 else 0
        if used > 0:
            entries.append((k, 0 if pts is None else 1, pts or 0, bytes(laid[cstart[i]:cstart[i] + used])))
    return entries


def write_cut(path, table):
    """table: [(name, first sector, sector count, entries, pictures, w, h, number)]. The file: "P3CUT2", the movie count, then per
    movie 48 bytes (first sector, sector count, where its entries are, how many, pictures, width, height, name in 20 bytes, the
    number its markers and its .p3v carry), then 16 bytes per kept sector (sector within the movie, where its bytes are, how many,
    1 if a time stamp goes with it, the time stamp), then the bytes."""
    head = bytearray(CUT_MAGIC + struct.pack("<I", len(table)))
    at = len(head) + 48 * len(table)
    index, blobs = bytearray(), bytearray()
    blob_base = at + sum(16 * len(m[3]) for m in table)
    for name, first, count, entries, pictures, w, h, number in table:
        head += struct.pack("<IIIIIHH", first, count, at + len(index), len(entries), pictures, w, h) + name.encode()[:19].ljust(20, b"\0") + struct.pack("<I", number)
        for k, flags, pts, body in entries:
            index += struct.pack("<IIHHI", k, blob_base + len(blobs), len(body), flags, pts & 0xFFFFFFFF)
            blobs += body
    with open(path, "wb") as f:
        f.write(head + index + blobs)
    return len(head) + len(index) + len(blobs)


class Cut:
    """Reads movies.cut the way the 3DS build does."""

    def __init__(self, path):
        self.data = open(path, "rb").read()
        if self.data[:8] not in (CUT_MAGIC, OLD_CUT_MAGIC):
            raise SystemExit("%s is not a movies.cut" % path)
        n = struct.unpack_from("<I", self.data, 8)[0]
        self.movies = []
        for i in range(n):
            first, count, at, entries, pictures, w, h = struct.unpack_from("<IIIIIHH", self.data, 12 + 48 * i)
            name = self.data[12 + 48 * i + 24:12 + 48 * i + 44].split(b"\0")[0].decode()
            number = struct.unpack_from("<I", self.data, 12 + 48 * i + 44)[0]
            self.movies.append((name, first, count, at, entries, pictures, w, h, number))

    def sector(self, m, rel, sec):
        """The sector as the game should see it."""
        p = video_payload(sec)
        if not p:
            return sec
        _name, _first, _count, at, entries, _pictures, _w, _h, _number = self.movies[m]
        lo, hi = 0, entries
        while lo < hi:
            mid = (lo + hi) // 2
            if struct.unpack_from("<I", self.data, at + 16 * mid)[0] < rel:
                lo = mid + 1
            else:
                hi = mid
        out = bytearray(sec)
        k = blob = ln = flags = t = 0
        if lo < entries:
            k, blob, ln, flags, t = struct.unpack_from("<IIHHI", self.data, at + 16 * lo)
        if lo == entries or k != rel:
            out[15] = 0xBE                    # nothing for the game here: the whole packet becomes padding, which its reader drops
            return bytes(out)
        body = self.data[blob:blob + ln]
        out[16:18] = struct.pack(">H", 12 + ln)
        out[18:30] = pes_header(t if flags & 1 else None)
        out[30:30 + ln] = body
        rest = p[1] - ln
        if rest >= 7:                         # the room left over is a padding packet too
            out[30 + ln:30 + p[1]] = b"\0\0\1\xbe" + struct.pack(">H", rest - 6) + b"\x0f" + b"\xff" * (rest - 7)
        else:
            out[30 + ln:30 + p[1]] = b"\0" * rest
        return bytes(out)


def encode_p3v(src, out, fps, quality):
    """NAME.p3v: "P3V1", width, height, vblanks a picture and the key picture spacing (16 bits each), the picture count (32 bits),
    count + 1 file positions (32 bits each), then each picture's MPEG-1 video bytes with the headers that come before it."""
    key = 15
    # Every picture of the movie in turn, or every second one: the build finds a picture by its number in the game's movie.
    # Two steps, so that no picture is repeated or dropped to fit a clock.
    pick = "" if fps >= 30 else "select='not(mod(n\\,2))',"
    rate = "30000/1001" if fps >= 30 else "15000/1001"
    raw = subprocess.Popen([ffmpeg_exe(), "-v", "fatal", "-i", src, "-an", "-vf", pick + "scale=400:240:flags=lanczos", "-fps_mode", "passthrough",
                            "-pix_fmt", "yuv420p", "-f", "rawvideo", "-"], stdout=subprocess.PIPE)
    enc = subprocess.run([ffmpeg_exe(), "-v", "error", "-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "400x240", "-r", rate, "-i", "-", "-c:v", "mpeg1video",
                          "-g", str(key), "-bf", "0", "-q:v", str(quality), "-flags", "+cgop", "-sc_threshold", "1000000000", "-f", "mpeg1video", "-"],
                         stdin=raw.stdout, stdout=subprocess.PIPE, check=True)
    if raw.wait() != 0:
        raise SystemExit("ffmpeg could not read %s" % src)
    es = enc.stdout
    parts = units(es)
    for i, u in enumerate(parts):
        kind = (u[u.find(b"\0\0\1\0") + 5] >> 3) & 7
        if kind != (1 if i % key == 0 else 2):
            raise SystemExit("picture %d of %s is of kind %d" % (i, out, kind))
    at = 16 + 4 * (len(parts) + 1)
    with open(out, "wb") as f:
        f.write(P3V_MAGIC + struct.pack("<HHHHI", 400, 240, 2 if fps >= 30 else 4, key, len(parts)))
        for u in parts:
            f.write(struct.pack("<I", at))
            at += len(u)
        f.write(struct.pack("<I", at))
        for u in parts:
            f.write(u)
    return len(parts), os.path.getsize(out)


def make(a):
    os.makedirs(a.out, exist_ok=True)
    lst = sorted(movies(a.iso), key=lambda m: m[1])
    table = []
    with open(a.iso, "rb") as f:
        for number, (name, first, count) in enumerate(lst):
            if a.only and name not in a.only:
                continue
            f.seek(first * SECTOR)
            data = f.read(count * SECTOR)
            entries, video, pictures, (w, h) = cut_movie(data, number)
            if entries is None:
                print("%-14s left as it is: %d pictures in %d video sectors are smaller than a cheap copy" % (name, pictures, video), flush=True)
                continue
            table.append((name, first, count, entries, pictures, w, h, number))
            line = "%-14s %5d pictures %dx%d, %6d video sectors, %5d kept" % (name, pictures, w, h, video, len(entries))
            if not a.no_video:
                tmp = os.path.join(a.out, "tmp.sfd")
                with open(tmp, "wb") as t:
                    t.write(data)
                target = os.path.join(a.out, "%02d.p3v" % number)
                shown, size = encode_p3v(tmp, target, a.fps, a.quality)
                os.remove(tmp)
                line += ", %02d.p3v %d pictures %.1f MB" % (number, shown, size / 1e6)
            print(line, flush=True)
    size = write_cut(os.path.join(a.out, "movies.cut"), table)
    print("movies.cut: %d movies, %.1f MB" % (len(table), size / 1e6))


def patched(iso, cut):
    """Yields (sector of the image, new bytes) for every sector the cut changes."""
    with open(iso, "rb") as f:
        for m, (name, first, count, *_rest) in enumerate(cut.movies):
            f.seek(first * SECTOR)
            data = f.read(count * SECTOR)
            for k in range(count):
                sec = data[k * SECTOR:(k + 1) * SECTOR]
                new = cut.sector(m, k, sec)
                if new != sec:
                    yield first + k, new


def check(a):
    """Decodes what the game will be given and reads every marker back."""
    cut = Cut(a.cut)
    with open(a.iso, "rb") as f:
        for m, (name, first, count, _at, _entries, pictures, w, h, number) in enumerate(cut.movies):
            f.seek(first * SECTOR)
            data = f.read(count * SECTOR)
            new = b"".join(cut.sector(m, k, data[k * SECTOR:(k + 1) * SECTOR]) for k in range(count))
            raw = subprocess.run([ffmpeg_exe(), "-v", "error", "-f", "mpegvideo", "-i", "-", "-vf", "crop=%d:16:0:0" % w, "-pix_fmt", "gray", "-f", "rawvideo", "-"],
                                 input=scan(new)[1], stdout=subprocess.PIPE, check=True).stdout
            got = len(raw) // (w * 16)
            bad = 0
            for n in range(got):
                row = raw[n * w * 16 + 8 * w:n * w * 16 + 9 * w]
                if read_marker([1 if row[i * 16 + 8] > 128 else 0 for i in range(MARK_BITS)]) != (number, n):
                    bad += 1
            print("%-14s %d of %d pictures decode, %d wrong markers" % (name, got, pictures, bad))


def patch(a):
    cut = Cut(a.cut)
    n = 0
    with open(a.copy, "r+b") as f:
        for sector, new in patched(a.iso, cut):
            f.seek(sector * SECTOR)
            f.write(new)
            n += 1
    print("%d sectors written to %s" % (n, a.copy))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("make")
    m.add_argument("iso")
    m.add_argument("-o", "--out", required=True)
    m.add_argument("--only", nargs="*")
    m.add_argument("--fps", type=int, default=30)
    m.add_argument("--quality", type=int, default=5)
    m.add_argument("--no-video", action="store_true")
    c = sub.add_parser("check")
    c.add_argument("iso")
    c.add_argument("cut")
    p = sub.add_parser("patch")
    p.add_argument("iso")
    p.add_argument("cut")
    p.add_argument("copy")
    a = ap.parse_args()
    {"make": make, "check": check, "patch": patch}[a.cmd](a)


if __name__ == "__main__":
    main()
