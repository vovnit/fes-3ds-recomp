#!/usr/bin/env python3
"""Translates the I/O processor code the game actually runs into C++ for the 3DS build.

The I/O processor runs the game's sound driver, loaded from the disc into its 2 MB of memory, and the 3DS has to interpret it at
about 25 cycles per instruction. That makes it the limit on every light scene. Each block here becomes one function, which ends by
jumping straight to the next. Anything not translated stays with the interpreter, so a block missing here costs speed and nothing
else, and each group of blocks is switched on only if the words in memory match the ones it was made from.

    python3 p3fes_iopgen.py OUT.cpp SEEN RAM [SEEN RAM ...]

SEEN is one bit per word of I/O processor memory, set where an instruction ran, and RAM is that memory from the same run: iopseen.bin
and iopram.bin from a build with IOPRECORD=1, see port3ds/README.md. Both come from your own disc, and so does the output: keep them
private, like the rest of generated/.
"""
import sys

RAM_BYTES = 2 << 20
NO_DELAY = {0x0C, 0x0D}   # syscall, break


def word(ram, pc):
    return int.from_bytes(ram[pc:pc + 4], "little")


def is_branch(w):
    op = w >> 26
    if op == 0:
        return (w & 63) in (8, 9)
    return op == 1 or 2 <= op <= 7


def can_translate(w):
    op = w >> 26
    if op == 0:
        return (w & 63) in (0, 2, 3, 4, 6, 7, 8, 9, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x18, 0x19, 0x1A, 0x1B, 0x20, 0x21, 0x22, 0x23, 0x24,
                            0x25, 0x26, 0x27, 0x2A, 0x2B)
    if op == 1:
        return True
    return op in range(2, 0x10) or op in (0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x28, 0x29, 0x2A, 0x2B, 0x2E, 0x2F)


def simm(w):
    v = w & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def branch_target(pc, w):
    op = w >> 26
    if op in (2, 3):
        return (pc & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
    if op == 0:
        return None
    return (pc + 4 + (simm(w) << 2)) & 0xFFFFFFFF


def uses(w):
    """Registers an instruction reads and the one it writes (0 for none)."""
    op, rs, rt, rd = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
    if op == 0:
        f = w & 63
        if f in (0, 2, 3):
            return {rt}, rd
        if f in (4, 6, 7):
            return {rs, rt}, rd
        if f == 8:
            return {rs}, 0
        if f == 9:
            return {rs}, rd
        if f in (0x10, 0x12):
            return set(), rd
        if f in (0x11, 0x13):
            return {rs}, 0
        if f in (0x18, 0x19, 0x1A, 0x1B):
            return {rs, rt}, 0
        if f == 0x0F:
            return set(), 0
        return {rs, rt}, rd
    if op == 1:
        return {rs}, 31 if (rt & 0x1E) == 0x10 else 0
    if op == 2:
        return set(), 0
    if op == 3:
        return set(), 31
    if op in (4, 5):
        return {rs, rt}, 0
    if op in (6, 7):
        return {rs}, 0
    if op == 0x0F:
        return set(), rt
    if 8 <= op <= 0x0E:
        return {rs}, rt
    if op in (0x22, 0x26):
        return {rs, rt}, rt
    if 0x20 <= op <= 0x25:
        return {rs}, rt
    if op in (0x28, 0x29, 0x2A, 0x2B, 0x2E):
        return {rs, rt}, 0
    return set(), 0


class Block:
    def __init__(self, pc):
        self.pc, self.pcs, self.exit_pc, self.branch = pc, [], None, None


def find_blocks(ram, seen):
    def ran(pc):
        w = pc >> 2
        return 0 <= pc < RAM_BYTES and (seen[w >> 3] >> (w & 7)) & 1

    leaders = set()
    for pc in range(0, RAM_BYTES, 4):
        if not ran(pc):
            continue
        w = word(ram, pc)
        if not ran(pc - 4):
            leaders.add(pc)
        if is_branch(w):
            target = branch_target(pc, w)
            if target is not None and ran(target):
                leaders.add(target)
            if ran(pc + 8):
                leaders.add(pc + 8)
    blocks = {}
    for leader in sorted(leaders):
        if not can_translate(word(ram, leader)):
            continue
        block, pc = Block(leader), leader
        while True:
            w = word(ram, pc)
            if not ran(pc) or not can_translate(w):
                block.exit_pc = pc
                break
            if is_branch(w):
                delay = word(ram, pc + 4)
                if not ran(pc + 4) or not can_translate(delay) or is_branch(delay):
                    block.exit_pc = pc   # the interpreter takes the branch and its delay slot
                    break
                block.branch = pc
                block.pcs += [pc, pc + 4]
                break
            block.pcs.append(pc)
            pc += 4
            if pc in leaders:
                block.exit_pc = pc
                break
        if block.pcs:
            blocks[leader] = block
    return blocks


def reg(n, _):
    return "0u" if n == 0 else "r%d" % n


class Emit:
    def __init__(self, ram, block, blocks):
        self.ram, self.block, self.blocks = ram, block, blocks
        self.lines, self.dirty = [], []

    def put(self, text):
        self.lines.append("    " + text)

    def spill(self):
        return " ".join("c.r[%d] = r%d;" % (n, n) for n in self.dirty)

    def set_reg(self, n, value):
        if n == 0:
            self.put("(void)(%s);" % value)
            return
        self.put("r%d = %s;" % (n, value))
        if n not in self.dirty:
            self.dirty.append(n)

    def where(self, in_delay, pc):
        """Sets the program counters for a stop at the instruction after this one."""
        if in_delay:
            return "c.pc = next; c.npc = next + 4;"
        return "c.pc = 0x%Xu; c.npc = 0x%Xu;" % (pc + 4, pc + 8)

    def read(self, size, signed, address, pc, in_delay, rt=None, value="v"):
        """Reads into v. A read that has to stop finishes its instruction first, so rt and value say what it was loading."""
        kinds = {1: "std::uint8_t", 2: "std::uint16_t", 4: "std::uint32_t"}
        cast = {(1, True): "static_cast<std::uint32_t>(static_cast<std::int8_t>(%s))", (2, True): "static_cast<std::uint32_t>(static_cast<std::int16_t>(%s))"}.get((size, signed), "%s")
        slow = {1: "read8", 2: "read16", 4: "read32"}[size]
        stop = ""
        if rt is not None:
            stop = " if (kernel::switch_pending()) { %sreturn left; }" % ("c.r[%d] = %s; " % (rt, value) if rt else "")
        self.put("a = %s;" % address)
        self.put("if (a < kRamSize) [[likely]] v = %s;" % (cast % ("get<%s>(a)" % kinds[size])))
        self.put("else { %s %s v = %s;%s }" % (self.spill(), self.where(in_delay, pc), cast % (slow + "(a)"), stop))

    def write(self, size, value, address, pc, in_delay):
        kinds = {1: "std::uint8_t", 2: "std::uint16_t", 4: "std::uint32_t"}
        slow = {1: "write8", 2: "write16", 4: "write32"}[size]
        self.put("a = %s; v = %s;" % (address, value))
        self.put("if (a < kRamSize) [[likely]] put<%s>(a, static_cast<%s>(v));" % (kinds[size], kinds[size]))
        self.put("else { %s %s %s(a, static_cast<%s>(v)); if (kernel::switch_pending()) return left; }" % (self.spill(), self.where(in_delay, pc), slow, kinds[size]))

    def instruction(self, pc, in_delay):
        w = word(self.ram, pc)
        op, rs, rt, rd, sa = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
        S, T = reg(rs, 0), reg(rt, 0)
        imm = simm(w)
        addr = "%s + 0x%Xu" % (S, imm & 0xFFFFFFFF) if imm else S
        signed = lambda r: "static_cast<std::int32_t>(%s)" % r
        if op == 0:
            f = w & 63
            if w == 0:
                return
            if f == 0:
                self.set_reg(rd, "%s << %d" % (T, sa))
            elif f == 2:
                self.set_reg(rd, "%s >> %d" % (T, sa))
            elif f == 3:
                self.set_reg(rd, "static_cast<std::uint32_t>(%s >> %d)" % (signed(T), sa))
            elif f == 4:
                self.set_reg(rd, "%s << (%s & 31u)" % (T, S))
            elif f == 6:
                self.set_reg(rd, "%s >> (%s & 31u)" % (T, S))
            elif f == 7:
                self.set_reg(rd, "static_cast<std::uint32_t>(%s >> (%s & 31u))" % (signed(T), S))
            elif f == 0x0F:
                pass
            elif f == 0x10:
                self.set_reg(rd, "c.hi")
            elif f == 0x11:
                self.put("c.hi = %s;" % S)
            elif f == 0x12:
                self.set_reg(rd, "c.lo")
            elif f == 0x13:
                self.put("c.lo = %s;" % S)
            elif f == 0x18:
                self.put("{ const std::int64_t p = static_cast<std::int64_t>(%s) * %s; c.lo = static_cast<std::uint32_t>(p); c.hi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(p) >> 32); }" % (signed(S), signed(T)))
            elif f == 0x19:
                self.put("{ const std::uint64_t p = static_cast<std::uint64_t>(%s) * %s; c.lo = static_cast<std::uint32_t>(p); c.hi = static_cast<std::uint32_t>(p >> 32); }" % (S, T))
            elif f == 0x1A:
                self.put("divide(c, %s, %s);" % (S, T))
            elif f == 0x1B:
                self.put("divide_unsigned(c, %s, %s);" % (S, T))
            elif f in (0x20, 0x21):
                self.set_reg(rd, "%s + %s" % (S, T))
            elif f in (0x22, 0x23):
                self.set_reg(rd, "%s - %s" % (S, T))
            elif f == 0x24:
                self.set_reg(rd, "%s & %s" % (S, T))
            elif f == 0x25:
                self.set_reg(rd, "%s | %s" % (S, T))
            elif f == 0x26:
                self.set_reg(rd, "%s ^ %s" % (S, T))
            elif f == 0x27:
                self.set_reg(rd, "~(%s | %s)" % (S, T))
            elif f == 0x2A:
                self.set_reg(rd, "%s < %s ? 1u : 0u" % (signed(S), signed(T)))
            elif f == 0x2B:
                self.set_reg(rd, "%s < %s ? 1u : 0u" % (S, T))
            else:
                raise ValueError(hex(w))
        elif op in (8, 9):
            self.set_reg(rt, "%s + 0x%Xu" % (S, imm & 0xFFFFFFFF))
        elif op == 0x0A:
            self.set_reg(rt, "%s < %d ? 1u : 0u" % (signed(S), imm))
        elif op == 0x0B:
            self.set_reg(rt, "%s < 0x%Xu ? 1u : 0u" % (S, imm & 0xFFFFFFFF))
        elif op == 0x0C:
            self.set_reg(rt, "%s & 0x%Xu" % (S, w & 0xFFFF))
        elif op == 0x0D:
            self.set_reg(rt, "%s | 0x%Xu" % (S, w & 0xFFFF))
        elif op == 0x0E:
            self.set_reg(rt, "%s ^ 0x%Xu" % (S, w & 0xFFFF))
        elif op == 0x0F:
            self.set_reg(rt, "0x%Xu" % ((w << 16) & 0xFFFFFFFF))
        elif op in (0x20, 0x21, 0x23, 0x24, 0x25):
            self.read({0x20: 1, 0x21: 2, 0x23: 4, 0x24: 1, 0x25: 2}[op], op in (0x20, 0x21), addr, pc, in_delay, rt)
            self.set_reg(rt, "v")
        elif op == 0x22:
            value = "(v << shift) | (shift ? (%s & ((1u << shift) - 1u)) : 0u)" % T
            self.put("a2 = %s; shift = (3u - (a2 & 3u)) * 8u;" % addr)
            self.read(4, False, "a2 & ~3u", pc, in_delay, rt, value)
            self.set_reg(rt, value)
        elif op == 0x26:
            value = "(v >> shift) | (shift ? (%s & ~(0xFFFFFFFFu >> shift)) : 0u)" % T
            self.put("a2 = %s; shift = (a2 & 3u) * 8u;" % addr)
            self.read(4, False, "a2 & ~3u", pc, in_delay, rt, value)
            self.set_reg(rt, value)
        elif op in (0x28, 0x29, 0x2B):
            self.write({0x28: 1, 0x29: 2, 0x2B: 4}[op], T, addr, pc, in_delay)
        elif op == 0x2A:
            self.put("a2 = %s; shift = (3u - (a2 & 3u)) * 8u;" % addr)
            self.read(4, False, "a2 & ~3u", pc, in_delay)
            self.write(4, "(%s >> shift) | (shift ? (v & ~(0xFFFFFFFFu >> shift)) : 0u)" % T, "a2 & ~3u", pc, in_delay)
        elif op == 0x2E:
            self.put("a2 = %s; shift = (a2 & 3u) * 8u;" % addr)
            self.read(4, False, "a2 & ~3u", pc, in_delay)
            self.write(4, "(%s << shift) | (shift ? (v & ((1u << shift) - 1u)) : 0u)" % T, "a2 & ~3u", pc, in_delay)
        elif op == 0x2F:
            pass
        else:
            raise ValueError(hex(w))

    def go(self, target):
        """Ends the block at a fixed address."""
        self.put(self.spill())
        if target in self.blocks:
            self.put("if (left > 0) [[likely]] TAIL b_%05X(c, left);" % target)
        self.put("c.pc = 0x%Xu; c.npc = 0x%Xu; return left;" % (target, (target + 4) & 0xFFFFFFFF))

    def body(self):
        block = self.block
        reads, writes = set(), set()
        for pc in block.pcs:
            used, wrote = uses(word(self.ram, pc))
            for n in used:
                if n and n not in writes:
                    reads.add(n)
            if wrote:
                writes.add(wrote)
        self.put("left -= %d;" % len(block.pcs))
        for n in sorted(reads | writes):
            self.put("std::uint32_t r%d%s;" % (n, " = c.r[%d]" % n if n in reads else ""))
        self.put("[[maybe_unused]] std::uint32_t a, a2, v, shift, next;")
        if block.branch is None:
            for pc in block.pcs:
                self.instruction(pc, False)
            self.go(block.exit_pc)
            return
        pc = block.branch
        w = word(self.ram, pc)
        op, rs, rt, rd = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        for at in block.pcs[:-2]:
            self.instruction(at, False)
        S, T = reg(rs, 0), reg(rt, 0)
        signed = lambda r: "static_cast<std::int32_t>(%s)" % r
        target = branch_target(pc, w)
        fall = pc + 8
        if op == 0:
            self.put("next = %s;" % S)
            if (w & 63) == 9:
                self.set_reg(rd, "0x%Xu" % fall)
        elif op in (2, 3):
            self.put("next = 0x%Xu;" % target)
            if op == 3:
                self.set_reg(31, "0x%Xu" % fall)
        else:
            if op == 1:
                cond = "%s >= 0" % signed(S) if rt & 1 else "%s < 0" % signed(S)
                self.put("const bool taken = %s;" % cond)
                if (rt & 0x1E) == 0x10:
                    self.set_reg(31, "0x%Xu" % fall)
            else:
                cond = {4: "%s == %s" % (S, T), 5: "%s != %s" % (S, T), 6: "%s <= 0" % signed(S), 7: "%s > 0" % signed(S)}[op]
                self.put("const bool taken = %s;" % cond)
            self.put("next = taken ? 0x%Xu : 0x%Xu;" % (target, fall))
        self.instruction(pc + 4, True)
        if op == 0:
            self.put(self.spill())
            self.put("if (left > 0) [[likely]] {")
            self.put("    const std::uint32_t at = slot(next);")
            self.put("    if (at < kSlots) [[likely]] if (const Block block = g_table[at]) [[likely]] TAIL block(c, left);")
            self.put("}")
            self.put("c.pc = next; c.npc = next + 4; return left;")
        elif op in (2, 3):
            self.go(target)
        else:
            self.put(self.spill())
            self.put("if (left > 0) [[likely]] {")
            if target in self.blocks:
                self.put("    if (taken) TAIL b_%05X(c, left);" % target)
            if fall in self.blocks:
                self.put("    if (!taken) TAIL b_%05X(c, left);" % fall)
            self.put("}")
            self.put("c.pc = next; c.npc = next + 4; return left;")


HEAD = """// Made by tools/p3fes_iopgen.py from your own disc. Keep it private.
#include "iop/iop.hpp"
#include "iop/kernel.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>

#define TAIL __attribute__((musttail)) return

namespace ps2::iop {

namespace {

constexpr std::uint32_t kBase = 0x%X, kSlots = (0x%X - kBase) >> 2;
using Block = std::int32_t (*)(Cpu &, std::int32_t);
Block g_table[kSlots];

// Rotating sends a misaligned address out of range, where the interpreter deals with it.
inline std::uint32_t slot(std::uint32_t pc) { return std::rotr(pc - kBase, 2); }

template <typename T>
inline T get(std::uint32_t a) {
    T v;
    std::memcpy(&v, g_ram + a, sizeof v);
    return v;
}

template <typename T>
inline void put(std::uint32_t a, T value) {
    std::memcpy(g_ram + a, &value, sizeof value);
}

[[gnu::noinline]] void divide(Cpu &c, std::uint32_t a, std::uint32_t b) {
    const auto n = static_cast<std::int32_t>(a), d = static_cast<std::int32_t>(b);
    if (d == 0) { c.lo = n < 0 ? 1u : 0xFFFFFFFFu; c.hi = a; }
    else if (n == INT32_MIN && d == -1) { c.lo = static_cast<std::uint32_t>(INT32_MIN); c.hi = 0; }
    else { c.lo = static_cast<std::uint32_t>(n / d); c.hi = static_cast<std::uint32_t>(n %% d); }
}

[[gnu::noinline]] void divide_unsigned(Cpu &c, std::uint32_t a, std::uint32_t b) {
    if (b == 0) { c.lo = 0xFFFFFFFFu; c.hi = a; }
    else { c.lo = a / b; c.hi = a %% b; }
}

"""

TAIL = """
struct Entry {
    std::uint32_t pc;
    Block block;
    std::uint16_t words, group;
    std::uint32_t sum;
};

const Entry kEntries[] = {
%s};

constexpr std::size_t kGroups = %d;

}

// A group is the blocks that jump straight to each other, so it runs only if every one of them matches what is in memory.
void blocks_refresh() {
    static bool good[kGroups];
    for (bool &g : good) g = true;
    for (const Entry &e : kEntries) {
        std::uint32_t sum = 0;
        for (std::uint32_t i = 0; i < e.words; ++i) sum = (sum ^ ram32(e.pc + i * 4u)) * 31u;
        if (sum != e.sum) good[e.group] = false;
    }
    for (Block &b : g_table) b = nullptr;
    for (const Entry &e : kEntries)
        if (good[e.group]) g_table[slot(e.pc)] = e.block;
}

std::int32_t blocks_run(Cpu &c, std::int32_t left) {
    if (c.npc != c.pc + 4u) return kNoBlock;
    const std::uint32_t at = slot(c.pc);
    if (at >= kSlots) return kNoBlock;
    const Block block = g_table[at];
    return block ? block(c, left) : kNoBlock;
}

}
"""


def direct_links(block, ram):
    out = []
    if block.branch is None:
        out.append(block.exit_pc)
    else:
        w = word(ram, block.branch)
        target = branch_target(block.branch, w)
        if target is not None:
            out.append(target)
            if w >> 26 not in (2, 3):
                out.append(block.branch + 8)
    return out


def main():
    if len(sys.argv) < 4 or len(sys.argv) % 2:
        raise SystemExit(__doc__)
    out, pairs = sys.argv[1], sys.argv[2:]
    seen = bytearray(RAM_BYTES // 32)
    ram = None
    conflicts = 0
    for i in range(0, len(pairs), 2):
        s = open(pairs[i], "rb").read()
        r = bytearray(open(pairs[i + 1], "rb").read())
        if ram is None:
            ram = bytearray(r)
        for byte in range(len(seen)):
            bits = s[byte]
            if not bits:
                continue
            for bit in range(8):
                if bits >> bit & 1:
                    pc = (byte * 8 + bit) * 4
                    if seen[byte] >> bit & 1 and ram[pc:pc + 4] != r[pc:pc + 4]:
                        conflicts += 1
                    ram[pc:pc + 4] = r[pc:pc + 4]
                    seen[byte] |= 1 << bit
    blocks = find_blocks(ram, seen)
    if not blocks:
        raise SystemExit("nothing to translate")
    parent = {pc: pc for pc in blocks}

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    for pc, block in blocks.items():
        for link in direct_links(block, ram):
            if link in blocks:
                parent[find(pc)] = find(link)
    groups = {}
    for pc in blocks:
        groups.setdefault(find(pc), len(groups))
    base = min(blocks) & ~0xFFF
    end = (max(b.pcs[-1] for b in blocks.values()) + 0x1000) & ~0xFFF
    parts, entries = [HEAD % (base, end)], []
    for pc in sorted(blocks):
        parts.append("std::int32_t b_%05X(Cpu &c, std::int32_t left);\n" % pc)
    for pc in sorted(blocks):
        block = blocks[pc]
        emit = Emit(ram, block, blocks)
        emit.body()
        parts.append("[[gnu::noinline]] std::int32_t b_%05X(Cpu &c, std::int32_t left) {\n%s\n}\n\n" % (pc, "\n".join(l for l in emit.lines if l.strip())))
        sum_ = 0
        for at in range(block.pcs[0], block.pcs[-1] + 4, 4):
            sum_ = ((sum_ ^ word(ram, at)) * 31) & 0xFFFFFFFF
        count = (block.pcs[-1] + 4 - block.pcs[0]) // 4
        entries.append("    {0x%X, b_%05X, %d, %d, 0x%Xu},\n" % (pc, pc, count, groups[find(pc)], sum_))
    parts.append(TAIL % ("".join(entries), len(groups)))
    with open(out, "w") as f:
        f.write("".join(parts))
    words = sum(len(b.pcs) for b in blocks.values())
    print("%d blocks, %d instructions, %d groups, %d words differed between runs, 0x%X to 0x%X" % (len(blocks), words, len(groups), conflicts, base, end))


if __name__ == "__main__":
    main()
