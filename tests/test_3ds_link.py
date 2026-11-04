"""The Mac side of the New 3DS test link talks the game's protocol, and the game's side writes only what it should."""
import importlib.util
import os
import socket
import tempfile
import threading
import zlib

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
spec = importlib.util.spec_from_file_location("link", os.path.join(ROOT, "tools", "p3fes_3ds_link.py"))
link = importlib.util.module_from_spec(spec)
spec.loader.exec_module(link)

FILES = {"/p3fes/log.txt": b"line\n" * 3000, "/p3fes/perf.txt": b"frame 30\n", "/p3fes/shots/frame_00030.ppm": b"P6\x00\xff" * 40000}


received = []


def serve(listener, seen):
    client, _ = listener.accept()
    stream = client.makefile("rb")
    for raw in stream:
        line = raw.decode().strip()
        seen.append(line)
        word, _, rest = line.partition(" ")
        if word == "GET":
            if rest in FILES:
                client.sendall(b"OK %d\n" % len(FILES[rest]) + FILES[rest])
            else:
                client.sendall(b"ERR cannot read that file\n")
        elif word == "PUT":
            path, size, crc = rest.split(" ")
            client.sendall(b"GO\n")
            data = b""
            while len(data) < int(size):
                data += stream.read(int(size) - len(data))
            received.append((path, data, int(crc, 16)))
            client.sendall(b"OK\n")
        elif word == "LS":
            if rest == "/p3fes/shots":
                client.sendall(b"OK\n%d frame_00030.ppm\nEND\n" % len(FILES["/p3fes/shots/frame_00030.ppm"]))
            else:
                client.sendall(b"ERR no such folder\n")
        else:
            client.sendall(b"OK\n")
    client.close()


def main():
    listener = socket.create_server(("127.0.0.1", 0))
    seen = []
    threading.Thread(target=serve, args=(listener, seen), daemon=True).start()
    with tempfile.TemporaryDirectory() as folder:
        connection = link.Link("127.0.0.1", listener.getsockname()[1])
        assert connection.ask("PAD %x %s" % (link.BUTTONS["a"], 3)) == "OK"
        link.fetch(connection, folder)
        assert open(os.path.join(folder, "log.txt"), "rb").read() == FILES["/p3fes/log.txt"]
        assert open(os.path.join(folder, "perf.txt"), "rb").read() == FILES["/p3fes/perf.txt"]
        assert open(os.path.join(folder, "shots", "frame_00030.ppm"), "rb").read() == FILES["/p3fes/shots/frame_00030.ppm"]
        assert not os.path.exists(os.path.join(folder, "probe.txt"))
        program = b"3DSX" + bytes(range(256)) * 400
        assert connection.put("/3ds/p3fes.3dsx", program) == "OK"
    assert seen[0] == "PAD 4000 3", seen[0]
    assert received == [("/3ds/p3fes.3dsx", program, zlib.crc32(program))]

    source = open(os.path.join(ROOT, "port3ds", "remote3ds.cpp")).read()
    code = "\n".join(line for line in source.splitlines() if not line.startswith("//"))
    assert code.count('fopen(g_partial, "wb")') == 1 and code.count('"wb"') == 1
    assert 'kProgram = "/3ds/p3fes.3dsx"' in code and 'kSettings = "/p3fes/args.txt"' in code
    for banned in ('"ab"', "unlink(", "/luma/backups", "/boot"):
        assert banned not in code, banned
    print("ok   3DS test link: the Mac tool fetches, installs and sends buttons, and the game's side writes only two files")


if __name__ == "__main__":
    main()
