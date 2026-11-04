"""Copies the New 3DS test build and your disc to an SD card, or to a folder laid out like one (see port3ds/README.md).

    python3 p3fes_3ds_sd.py <disc.iso> <ps2-web-recomp folder> -o <SD card or folder>

Writes:
    <output>/3ds/p3fes.3dsx          the game, for the Homebrew Launcher
    <output>/3ds/p3fes-probe.3dsx    a small test that reports the console's memory and speed
    <output>/p3fes/SLUS_216.21       the boot program
    <output>/p3fes/disc.00, .01 ...  the disc in 1 GB parts, because FAT32 cannot hold a file of 4 GB or more

Your disc image is never changed, and nothing is written outside the output folder. A part that already has the right size is
kept, so a stopped copy can be started again. Everything written here comes from your own disc: keep it private.
"""
import argparse
import hashlib
import os
import shutil
import struct
import sys

PART_SIZE = 1 << 30
SECTOR = 2048
CHUNK = 1 << 22


def find_boot_file(iso):
    """Returns (offset, size) of the boot program in the disc's root folder, or None."""
    iso.seek(16 * SECTOR)
    pvd = iso.read(SECTOR)
    if pvd[1:6] != b"CD001":
        return None
    extent = struct.unpack("<I", pvd[158:162])[0]
    size = struct.unpack("<I", pvd[166:170])[0]
    iso.seek(extent * SECTOR)
    data = iso.read(size)
    at = 0
    while at < len(data):
        length = data[at]
        if length == 0:
            at = (at // SECTOR + 1) * SECTOR
            continue
        name = data[at + 33:at + 33 + data[at + 32]].decode("latin1")
        if name.startswith("SLUS_216.21"):
            return struct.unpack("<I", data[at + 2:at + 6])[0] * SECTOR, struct.unpack("<I", data[at + 10:at + 14])[0]
        at += length
    return None


def write_parts(iso_path, folder, part_size):
    total = os.path.getsize(iso_path)
    digest = hashlib.sha1()
    count = (total + part_size - 1) // part_size
    with open(iso_path, "rb") as iso:
        for index in range(count):
            path = os.path.join(folder, "disc.%02d" % index)
            want = min(part_size, total - index * part_size)
            if os.path.exists(path) and os.path.getsize(path) == want:
                print("  %s is already there" % os.path.basename(path))
                continue
            iso.seek(index * part_size)
            left = want
            with open(path + ".part", "wb") as out:
                while left:
                    block = iso.read(min(CHUNK, left))
                    if not block:
                        raise OSError("the disc image ended early")
                    out.write(block)
                    left -= len(block)
            os.replace(path + ".part", path)
            print("  wrote %s (%d of %d)" % (os.path.basename(path), index + 1, count))
    # Check what is really in the output, including parts kept from an earlier run.
    with open(iso_path, "rb") as iso:
        original = hashlib.sha1()
        for block in iter(lambda: iso.read(CHUNK), b""):
            original.update(block)
    for index in range(count):
        with open(os.path.join(folder, "disc.%02d" % index), "rb") as part:
            for block in iter(lambda: part.read(CHUNK), b""):
                digest.update(block)
    return digest.hexdigest() == original.hexdigest()


def main():
    parser = argparse.ArgumentParser(description="Copy the New 3DS test build and your disc to an SD card or folder.")
    parser.add_argument("iso", help="your disc image")
    parser.add_argument("recomp", help="the ps2-web-recomp folder holding build/3ds")
    parser.add_argument("-o", "--output", required=True, help="the SD card or folder to write to")
    parser.add_argument("--no-disc", action="store_true", help="copy only the programs, for when the disc is already on the card")
    parser.add_argument("--part-size", type=int, default=PART_SIZE, help=argparse.SUPPRESS)
    args = parser.parse_args()

    game = os.path.join(args.recomp, "build", "3ds", "p3fes.3dsx")
    probe = os.path.join(args.recomp, "build", "3ds", "probe.3dsx")
    for path in (args.iso, game, probe):
        if not os.path.isfile(path):
            print("Cannot find %s. Build first with port3ds/build.sh and port3ds/probe/build.sh." % path)
            return 1
    with open(args.iso, "rb") as iso:
        boot = find_boot_file(iso)
        if boot is None:
            print("This does not look like the Persona 3 FES (USA) disc: no SLUS_216.21 on it.")
            return 1
        iso.seek(boot[0])
        program = iso.read(boot[1])
    needed = os.path.getsize(args.iso) + os.path.getsize(game) + len(program)
    os.makedirs(args.output, exist_ok=True)
    free = shutil.disk_usage(args.output).free
    if not args.no_disc and free < needed:
        print("Not enough room: %.1f GB needed, %.1f GB free." % (needed / 2**30, free / 2**30))
        return 1

    apps = os.path.join(args.output, "3ds")
    data = os.path.join(args.output, "p3fes")
    os.makedirs(apps, exist_ok=True)
    os.makedirs(data, exist_ok=True)
    shutil.copyfile(game, os.path.join(apps, "p3fes.3dsx"))
    shutil.copyfile(probe, os.path.join(apps, "p3fes-probe.3dsx"))
    with open(os.path.join(data, "SLUS_216.21"), "wb") as out:
        out.write(program)
    print("Programs copied.")
    if args.no_disc:
        return 0
    print("Copying the disc in parts (this takes a while):")
    try:
        same = write_parts(args.iso, data, args.part_size)
    except OSError as e:
        print("Copy stopped: %s. Run this again to carry on." % e)
        return 1
    if not same:
        print("The parts do NOT match your disc image. Delete them and run this again.")
        return 1
    print("Done. The parts match your disc image.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
