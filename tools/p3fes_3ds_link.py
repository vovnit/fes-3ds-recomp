"""Reads test results from the New 3DS build and replaces it over the network while it runs (see port3ds/remote3ds.cpp).

    python3 p3fes_3ds_link.py status
    python3 p3fes_3ds_link.py fetch <folder>     copies log.txt, perf.txt, probe.txt, crash reports and screenshots
    python3 p3fes_3ds_link.py press a 4          holds a button (a b x y start select up down left right l r zl zr) for 4 game frames
    python3 p3fes_3ds_link.py shots 30           saves a screenshot every 30 game frames; 0 stops
    python3 p3fes_3ds_link.py draw 1             draws one picture in N; 0 draws what keeps up with the game
    python3 p3fes_3ds_link.py quit               leaves to the Homebrew Menu
    python3 p3fes_3ds_link.py install [file]     replaces the game (default build/3ds/p3fes.3dsx in ../ps2-web-recomp) and restarts it
    python3 p3fes_3ds_link.py settings "a;b"     writes args.txt, one setting per ; (an empty string clears it) and restarts
    python3 p3fes_3ds_link.py run                restarts the game

Anyone on the network can do this: the link has no password. The old game is kept as /3ds/p3fes.3dsx.bak on the card.
Use --host for another address and --port for another port.
"""
import argparse
import os
import socket
import sys
import zlib

PORT = 5577
HOST = "192.168.1.133"
BUTTONS = {
    "select": 1 << 0, "start": 1 << 3, "up": 1 << 4, "right": 1 << 5, "down": 1 << 6, "left": 1 << 7, "zl": 1 << 8, "zr": 1 << 9,
    "l": 1 << 10, "r": 1 << 11, "x": 1 << 12, "b": 1 << 13, "a": 1 << 14, "y": 1 << 15,
}


class Link:
    def __init__(self, host, port, timeout=15):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.buffer = b""

    def line(self):
        while b"\n" not in self.buffer:
            data = self.sock.recv(4096)
            if not data:
                raise OSError("the console closed the connection")
            self.buffer += data
        text, self.buffer = self.buffer.split(b"\n", 1)
        return text.decode("latin1")

    def ask(self, command):
        self.sock.sendall(command.encode("latin1") + b"\n")
        return self.line()

    def listing(self, folder):
        reply = self.ask("LS " + folder)
        if not reply.startswith("OK"):
            return None
        rows = []
        while True:
            row = self.line()
            if row == "END":
                return rows
            size, name = row.split(" ", 1)
            rows.append((int(size), name))

    def put(self, path, data):
        reply = self.ask("PUT %s %d %08x" % (path, len(data), zlib.crc32(data)))
        if reply != "GO":
            return reply
        self.sock.sendall(data)
        return self.line()

    def get(self, path, target):
        reply = self.ask("GET " + path)
        if not reply.startswith("OK "):
            return False
        left = int(reply[3:])
        with open(target, "wb") as out:
            while left:
                block = self.buffer[:left] or self.sock.recv(min(left, 1 << 16))
                if not block:
                    raise OSError("the console closed the connection")
                self.buffer = self.buffer[len(block):] if self.buffer else b""
                out.write(block)
                left -= len(block)
        return True


def fetch(link, folder):
    os.makedirs(folder, exist_ok=True)
    copied = 0
    for name in ("log.txt", "perf.txt", "probe.txt"):
        if link.get("/p3fes/" + name, os.path.join(folder, name)):
            copied += 1
    for remote, local in (("/luma/dumps/arm11", "dumps"), ("/p3fes/shots", "shots")):
        rows = link.listing(remote) or []
        if rows:
            os.makedirs(os.path.join(folder, local), exist_ok=True)
        for _, name in rows:
            if link.get(remote + "/" + name, os.path.join(folder, local, name)):
                copied += 1
    print("Copied %d files to %s" % (copied, folder))


def main():
    parser = argparse.ArgumentParser(description="Read test results from the New 3DS build over the network.")
    parser.add_argument("--host", default=HOST)
    parser.add_argument("--port", type=int, default=PORT)
    parser.add_argument("command", choices=("status", "fetch", "press", "shots", "draw", "quit", "install", "settings", "run"))
    parser.add_argument("values", nargs="*")
    args = parser.parse_args()
    try:
        link = Link(args.host, args.port, timeout=300 if args.command == "install" else 15)
        if args.command == "status":
            print(link.ask("PING"))
        elif args.command == "fetch":
            fetch(link, args.values[0] if args.values else "console")
        elif args.command == "press":
            frames = args.values[1] if len(args.values) > 1 else "4"
            print(link.ask("PAD %x %s" % (BUTTONS[args.values[0]], frames)))
        elif args.command in ("shots", "draw"):
            print(link.ask("%s %s" % (args.command.upper(), args.values[0])))
        elif args.command == "install":
            default = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "ps2-web-recomp", "build", "3ds", "p3fes.3dsx")
            with open(args.values[0] if args.values else default, "rb") as program:
                data = program.read()
            print("Sending %d bytes" % len(data), flush=True)
            reply = link.put("/3ds/p3fes.3dsx", data)
            print(reply)
            if reply != "OK":
                return 1
            print(link.ask("RUN"))
        elif args.command == "settings":
            text = "".join(item.strip() + "\n" for item in " ".join(args.values).split(";") if item.strip())
            reply = link.put("/p3fes/args.txt", text.encode("latin1"))
            print(reply)
            if reply != "OK":
                return 1
            print(link.ask("RUN"))
        elif args.command == "run":
            print(link.ask("RUN"))
        else:
            print(link.ask("QUIT"))
    except OSError as e:
        print("No link to %s:%d: %s. Is the game running?" % (args.host, args.port, e))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
