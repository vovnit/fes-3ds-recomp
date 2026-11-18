#!/usr/bin/env python3
"""Hot copies of the game's busiest functions, for the New 3DS build (see port3ds/README.md, "Hot copies").

eerecomp writes the game as about 300 very large C++ functions in which every guest register lives in memory, so most
ARM instructions the console runs only load and store those registers. This tool reads that generated code and a
profile of which blocks ran, and writes a second copy of each busy game function as a small C++ function of its own:

- guest registers are locals for the whole function, in two 32-bit halves, so GCC keeps them in ARM registers;
- they are written back only before something that may read them (calls, returns, anything this tool does not know)
  and read again only where a later instruction still needs them;
- the cycle budget is a local too;
- a call to another hot copy is a direct call.

The game's own code stays as it is. eerecomp puts a call to the copy at the top of the original function
(natives named hot_...), so anything that enters the function in the middle, after a time slice ended there for
example, simply runs the original. The copies are made from the same text as the original, statement by statement,
so both do exactly the same work: on the native build the cycle counts must match to the cycle.

Steps, in the ps2-web-recomp folder (NAT is the natives list, here ../fes-pc-recomp-main/port3ds/natives.txt):

  1. Source text for the copies, without loop promotion, and the list of function starts:
       EERECOMP_NOLOOPS=1 EERECOMP_FUNCTIONS=build/eehot-functions.txt build/tools/eerecomp games/p3fes/disc/SLUS_216.21 \\
           --entries games/p3fes/entries.txt --natives NAT --out build/eehot-src
  2. A profile per scene, in games/p3fes/hot/ (private files, they describe your copy of the game). To record one:
       python3 tools/p3fes_eehot.py instrument build/eehot-src build/eehot-count
     then make a native build of build/eehot-count and run the scene with PS2_EEHOT=games/p3fes/hot/NAME.txt set.
  3. The copies:
       python3 tools/p3fes_eehot.py make build/eehot-src generated/p3fes --profile games/p3fes/hot/*.txt \\
           --natives NAT --natives-out build/eehot-natives.txt --functions build/eehot-functions.txt --gp 007D2CF0 --lossy port3ds/lossy.txt \\
           --skip port3ds/hot_skip.txt
  4. The game with the calls to them:
       build/tools/eerecomp games/p3fes/disc/SLUS_216.21 --entries games/p3fes/entries.txt \\
           --natives build/eehot-natives.txt --out generated/p3fes

--gp is the value the game keeps in its gp register (Persona 3 FES, SLUS-21621: 007D2CF0). A copy then runs only while
gp has that value and uses fixed addresses for the game's globals. It assumes the functions it calls leave gp alone.
Check a new set of copies on the native build before the console: the cycle count after a fixed number of frames must be
the same with the copies as with -DPS2_NO_HOT. -DPS2_HOT_BISECT and PS2_HOT_RANGE=lo:hi find a faulty copy.

The output (hot_*.cpp, hot.hpp) comes from your own disc: keep it private.
"""
import argparse
import collections
import glob
import os
import re
import sys

H = r'0x[0-9A-F]{8}u'
RE_LABEL = re.compile(r'^(L_[0-9A-F]{8}):(?: __attribute__\(\(cold\)\);)?$')
RE_BUDGET = re.compile(r'^c\.budget -= (\d+);$')
JUMP = (r'(?:if \(c\.budget < 0 && preempt\(c\)\) \{ c\.pc = (' + H + r') \| kUnwind; return; \} )?'
        r'(?:goto (L_[0-9A-F]{8});|\[\[clang::musttail\]\] return (unit_[0-9A-F]{4})\(c, (\d+)u\);)$')
RE_JUMP = re.compile(JUMP)
RE_JUMPD = re.compile(r'c\.pc = (' + H + r'); \[\[clang::musttail\]\] return dispatch\(c, 0u\);$')
RE_CALL = re.compile(r'^\{ (c\.r\[31\]\.sd\[0\] = static_cast<std::int32_t>\(' + H + r'\);) (.*?) ?'
                     r'(?:if \(stack_ok\(\)\) (unit_[0-9A-F]{4})\(c, (\d+)u\); else \{ c\.pc = (' + H + r'); return; \}|call\(c, (' + H + r')\);)'
                     r' if \(c\.pc != (' + H + r')\) \[\[unlikely\]\] return; \}$')
RE_JALR = re.compile(r'^\{ (const std::uint32_t t = [^;]*;(?: c\.r\[\d+\]\.sd\[0\] = static_cast<std::int32_t>\(' + H + r'\);)?) (.*?) ?'
                     r'c\.pc = t; call\(c, t\); if \(c\.pc != (' + H + r')\) \[\[unlikely\]\] return; \}$')
RE_RET = re.compile(r'^\{ (const std::uint32_t t = c\.r\[31\]\.w\[0\];) (.*?) ?c\.pc = t; return; \}$')
RE_JR = re.compile(r'^\{ (const std::uint32_t t = [^;]*;) (.*?) ?c\.pc = t; if \(t - ' + H + r' < \d+u\) \{ entry = \w+\[\(t - ' + H +
                   r'\) >> 2\]; if \(entry\) goto dispatch_entry; \} \[\[clang::musttail\]\] return dispatch\(c, 0u\); \}$')
RE_REG = re.compile(r'c\.r\[(\d+)\]')
RE_FIELD = re.compile(r'\.(w|d|sd)\[0\]')
RE_CCALL = re.compile(r'([A-Za-z_][A-Za-z0-9_:]*)\(c[,)]')
RE_MEM = re.compile(r'(?<![A-Za-z0-9_])(ld8|ld16|ld32|ld64|ld128|st8|st16|st32|st64|st128|lwl|lwr|ldl|ldr|swl|swr|sdl|sdr)\(')
RE_HOOK = re.compile(r'^\{ bool (hot_[0-9A-F]{8})\(Cpu &\); if \(hot_[0-9A-F]{8}\(c\)\) return; \}$')

# Runtime calls that take the Cpu but never read or write the general registers ...
GPR_PURE = {'fpu_compare', 'fpu_div', 'fpu_rsqrt', 'cfc1', 'ctc1', 'cfc2', 'ctc2', 'mfc0', 'mtc0', 'cpcond0', 'vu0_busy',
            'vu0_wait', 'vcallms', 'set_interrupts', 'lqc2_vu', 'sqc2_vu'}
# ... and those that never read or change the cycle budget (the same list as local_budget in recomp/codegen.cpp).
BUD_PURE = {'fpu_compare', 'fpu_div', 'ctc1', 'cfc1'}


def lo(r):
    return 1 << r


def both(r):
    return (1 << r) | (1 << (32 + r))


ALL = sum(both(r) for r in range(1, 32))


def mask_of(regs):
    return sum(both(r) for r in regs)


# --trust: the calling convention says a function does not read these registers of its caller (assembler temporary, results,
# t4 to t9), and that its caller does not read these after it returns (the same without the results, plus the arguments).
# With trust on, a copy does not write them back there. That is no longer exactly what the original does: memory keeps an
# older value, which only code that breaks the convention can see. --trust-check writes a marker there instead, so that a
# native run shows any such read as a different cycle count.
CALL_TEMPS = mask_of([1, 2, 3, 12, 13, 14, 15, 24, 25])
RET_TEMPS = mask_of([1, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25])
TRUST = {'call': 0, 'ret': 0, 'check': False}


def poison_code(mask):
    return ''.join('c.r[%d].w[%d] = 0x5EED%02X%02Xu; ' % (r, h, r, h) for r, h in halves(mask)) if TRUST['check'] else ''


class Unsupported(Exception):
    pass


def expr_end(t, i):
    """The end of the expression that starts at i: the next ';' or unmatched ')' outside parentheses."""
    depth = 0
    while i < len(t):
        ch = t[i]
        if ch == '(':
            depth += 1
        elif ch == ')':
            if depth == 0:
                return i
            depth -= 1
        elif ch == ';' and depth == 0:
            return i
        i += 1
    return i


RE_PIN_WRITE = re.compile(r'ld128\(.*?, c\.r\[(\d+)\]\);|(?:lwl|lwr|ldl|ldr)\(c\.r\[(\d+)\]|'
                          r'mmi::(?!pmthl\b|pdivw\b|pdivuw\b|pdivbw\b)\w+\((?:c, )?c\.r\[(\d+)\]|c\.r\[(\d+)\] = ')
RE_MOVE = re.compile(r'^c\.r\[(\d+)\]\.d\[0\] = c\.r\[(\d+)\]\.d\[0\] \+ 0ull;$')


def scan(text):
    """What a piece of generated text does with the general registers: (uses, kills, pinned) as masks of halves, and
    the registers it leaves with an upper word that copies bit 31 of the lower one."""
    uses = kills = pinned = canon = other = 0
    for m in RE_REG.finditer(text):
        r = int(m.group(1))
        if r == 0:
            continue
        a, b = m.span()
        if a >= 6 and text.startswith('set32(', a - 6):
            kills |= both(r)
            canon |= 1 << r
            continue
        f = RE_FIELD.match(text, b)
        if not f:
            pinned |= both(r)
            continue
        half = lo(r) if f.group(1) == 'w' else both(r)
        if text.startswith(' = ', f.end()):
            kills |= half
            if f.group(1) == 'sd' and text.startswith(' = static_cast<std::int32_t>(', f.end()):
                canon |= 1 << r
            else:
                other |= 1 << r
        else:
            uses |= half
    return uses, kills, pinned, canon & ~other


SX64 = 'static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(x%d)))'
SXHI = 'static_cast<std::uint32_t>(static_cast<std::int32_t>(x%d) >> 31)'


def rewrite(t, keep=0, canon=0):
    """The same text with the registers as locals: xN is the low word of register N and yN the next one. For a register
    in canon the upper word is known to copy bit 31 of the lower one, so yN is not read."""
    out = []
    cur = 0
    while True:
        m = RE_REG.search(t, cur)
        if not m:
            break
        r = int(m.group(1))
        a, b = m.span()
        if r == 0 or (keep >> r) & 1:
            out.append(t[cur:b])
            cur = b
            continue
        if a >= 6 and a - 6 >= cur and t.startswith('set32(', a - 6):
            if not t.startswith(', ', b):
                raise Unsupported('set32 form: ' + t)
            x = b + 2
            xe = expr_end(t, x)
            out.append(t[cur:a - 6])
            out.append('put32(x%d, y%d, %s)' % (r, r, rewrite(t[x:xe], keep, canon)))
            cur = xe + 1
            continue
        f = RE_FIELD.match(t, b)
        if not f:
            raise Unsupported('whole register outside a pinned statement: ' + t)
        e = f.end()
        kind = f.group(1)
        out.append(t[cur:a])
        if t.startswith(' = ', e):
            x = e + 3
            xe = expr_end(t, x)
            v = rewrite(t[x:xe], keep, canon)
            if kind == 'w':
                out.append('x%d = %s' % (r, v))
            elif kind == 'd':
                out.append('put64(x%d, y%d, %s)' % (r, r, v))
            else:
                out.append('put64(x%d, y%d, static_cast<std::uint64_t>(static_cast<std::int64_t>(%s)))' % (r, r, v))
            cur = xe
        else:
            wide = (SX64 % r) if (canon >> r) & 1 else 'get64(x%d, y%d)' % (r, r)
            if kind == 'w':
                out.append('x%d' % r)
            elif kind == 'd':
                out.append(wide)
            else:
                out.append('static_cast<std::int64_t>(%s)' % wide)
            cur = e
    out.append(t[cur:])
    return ''.join(out)


def halves(mask):
    for r in range(1, 32):
        if (mask >> r) & 1:
            yield r, 0
        if (mask >> (32 + r)) & 1:
            yield r, 1


def flush_code(mask, canon=0):
    out = ''
    for r, h in halves(mask):
        if h and (canon >> r) & 1:
            out += 'c.r[%d].w[1] = %s; ' % (r, SXHI % r)
        else:
            out += 'c.r[%d].w[%d] = %s%d; ' % (r, h, 'xy'[h], r)
    return out


def load_code(mask):
    return ''.join('%s%d = c.r[%d].w[%d]; ' % ('xy'[h], r, r, h) for r, h in halves(mask))


def regs_of(mask):
    return (mask | (mask >> 32)) & 0xFFFFFFFF


class Node:
    __slots__ = ('uses', 'kills', 'maydefs', 'flush', 'inval', 'succ', 'kind', 'ccall', 'budsync', 'keep', 'canon', 'move')

    def __init__(self, kind):
        self.kind = kind          # 'nop', 'stmt', 'preempt', 'call', 'exit' (leaves the copy), 'bad'
        self.uses = self.kills = self.maydefs = self.flush = self.inval = self.keep = self.canon = 0
        self.succ = []
        self.ccall = self.budsync = False
        self.move = None          # (to, from) for a plain 64-bit register copy


def stmt_node(text):
    """A node for plain statements (one line, a delay slot, or the head of a control line)."""
    n = Node('stmt')
    if not text:
        n.kind = 'nop'
        return n
    names = RE_CCALL.findall(text)
    n.budsync = any(x not in BUD_PURE and not x.startswith('mmi::') for x in names)
    if any(x not in GPR_PURE and not x.startswith('mmi::') for x in names):
        n.ccall = True            # may read or write any register: everything goes back to memory around it
        n.flush = n.inval = ALL
        n.keep = 0xFFFFFFFF
        return n
    uses, kills, pinned, canon = scan(text)
    pin_regs = pinned & 0xFFFFFFFF
    n.keep = pin_regs
    n.flush = pinned              # a statement that uses a whole register reads it from memory
    written = 0                   # and only some of them write it
    for m in RE_PIN_WRITE.finditer(text):
        r = int(next(g for g in m.groups() if g))
        written |= both(r)
        if (m.group(1) or m.group(4)) and text.count('c.r[%d]' % r) == 1:
            n.flush &= ~both(r)   # replaced whole: what the local held does not matter
    n.inval = pinned & written
    keep_halves = pin_regs | (pin_regs << 32)
    uses &= ~keep_halves
    kills &= ~keep_halves
    if text.startswith('if ('):   # a conditional move or trap: the write may not happen
        n.maydefs = kills
        n.uses = uses | kills
    else:
        n.uses, n.kills = uses, kills
        n.canon = canon & ~pin_regs
        m = RE_MOVE.match(text)
        if m and int(m.group(1)) and int(m.group(2)) and m.group(1) != m.group(2):
            n.move = (int(m.group(1)), int(m.group(2)))
    return n


def stmt_text(node, text, canon=0):
    if not text:
        return ''
    if node.ccall:
        return text.replace('return;', 'return true;')
    if node.move and (canon >> node.move[1]) & 1:
        return 'put32(x%d, y%d, x%d);' % (node.move[0], node.move[0], node.move[1])
    t = rewrite(text, node.keep, canon)
    t = RE_MEM.sub(r'bud_\1(c, bud, ', t)
    if 'c.budget' in t or 'return' in t or 'goto' in t or 'entry' in t:
        raise Unsupported('statement: ' + text)
    return t


class Line:
    __slots__ = ('kind', 'text', 'label', 'n', 'head', 'slot', 'jump', 'callee', 'ret', 'head_node', 'slot_node', 'main',
                 'pre_node', 'exit_node', 'ordinal')


class Unit:
    """One generated unit_XXXX.cpp, parsed into lines and a flow graph."""

    def __init__(self, path):
        text = open(path).read()
        m = re.search(r'void (unit_[0-9A-F]{4})\(Cpu &c, std::uint32_t entry\) \{\n', text)
        if not m:
            raise SystemExit('not a generated unit: ' + path)
        self.name = m.group(1)
        self.path = path
        default = text.index('    default: bad_entry(c, ', m.end())
        self.ids = {int(k): v for k, v in re.findall(r'    case (\d+)u: goto (L_[0-9A-F]{8});', text[m.end():default])}
        start = text.index('\n    }\n', default) + 7
        body = text[start:].split('\n')
        while body and body[-1] in ('', '}'):
            body.pop()
        self.lines = []
        self.nodes = []
        self.label_line = {}
        self.parse(body)
        self.link()

    def node(self, n):
        self.nodes.append(n)
        return len(self.nodes) - 1

    def parse_jump(self, text):
        """(text before the jump, jump) where jump = (preempt pc or None, kind, target...)"""
        m = RE_JUMP.search(text)
        if m:
            pre = text[:m.start()].strip()
            if m.group(2):
                return pre, (m.group(1), 'goto', m.group(2))
            return pre, (m.group(1), 'unit', m.group(3), int(m.group(4)))
        m = RE_JUMPD.search(text)
        if m:
            return text[:m.start()].strip(), (None, 'dispatch', m.group(1))
        return None

    def parse(self, body):
        label = None
        ordinal = 0
        for raw in body:
            s = raw.strip()
            ln = Line()
            ln.text = s
            ln.label = ln.head = ln.slot = ln.jump = ln.callee = ln.ret = None
            ln.head_node = ln.slot_node = ln.main = ln.pre_node = ln.exit_node = None
            ln.n = 0
            ln.ordinal = None
            self.lines.append(ln)
            m = RE_LABEL.match(raw)
            if m:
                ln.kind = 'label'
                ln.label = m.group(1)
                self.label_line[ln.label] = len(self.lines) - 1
                label = ln.label
                ordinal = 0
                continue
            if raw.startswith('#') or not s:
                ln.kind = 'pp'
                continue
            m = RE_BUDGET.match(s)
            if m:
                ln.kind = 'budget'
                ln.n = int(m.group(1))
                ln.ordinal = (label, ordinal)
                ordinal += 1
                continue
            if not raw.startswith('    ') or s.startswith(('switch (', 'case ', 'default:')) or s == '}' or 'eret(c)' in s or '_v' in s[:14]:
                ln.kind = 'bad'
                continue
            m = RE_CALL.match(s)
            if m:
                ln.kind = 'call'
                ln.head, ln.slot = m.group(1), m.group(2)
                ln.callee = ('unit', m.group(3), int(m.group(4)), m.group(5)) if m.group(3) else ('call', m.group(6))
                ln.ret = m.group(7)
                continue
            m = RE_JALR.match(s)
            if m:
                ln.kind = 'jalr'
                ln.head, ln.slot, ln.ret = m.group(1), m.group(2), m.group(3)
                continue
            m = RE_RET.match(s)
            if m:
                ln.kind = 'ret'
                ln.head, ln.slot = m.group(1), m.group(2)
                continue
            m = RE_JR.match(s)
            if m:
                ln.kind = 'jr'
                ln.head, ln.slot = m.group(1), m.group(2)
                continue
            if s.startswith('{ const bool t = ') and s.endswith(' } }'):
                k = s.rfind(' if (t) { ')
                e = s.find('; ', 17)
                j = self.parse_jump(s[k + 10:-4]) if k > 0 and 0 < e <= k else None
                if not j or j[0]:
                    ln.kind = 'bad'       # a conditional call (bltzal) or something unknown
                    continue
                ln.kind = 'branch'
                ln.head, ln.slot, ln.jump = s[2:e + 1], s[e + 2:k].strip(), j[1]
                continue
            if s.startswith('{ const std::uint32_t t = '):
                ln.kind = 'bad'           # a jump whose target is read from memory (modules)
                continue
            if s.startswith('if (') and s.endswith(' }'):
                depth = 0
                for i in range(3, len(s)):
                    if s[i] == '(':
                        depth += 1
                    elif s[i] == ')':
                        depth -= 1
                        if depth == 0:
                            break
                j = self.parse_jump(s[i + 4:-2]) if s.startswith(' { ', i + 1) else None
                if j:
                    ln.kind = 'likely'
                    ln.head, ln.slot, ln.jump = 'const bool t = ' + s[4:i] + ';', j[0], j[1]
                    continue
            if s.startswith('{ ') and s.endswith(' }'):
                j = self.parse_jump(s[2:-2])
                if j:
                    ln.kind = 'jump'
                    ln.slot, ln.jump = j[0], j[1]
                    continue
            j = self.parse_jump(s)
            if j and not j[0]:
                ln.kind = 'goto'
                ln.jump = j[1]
                continue
            if 'goto ' in s or 'entry' in s or 'c.budget' in s or 'musttail' in s:
                ln.kind = 'bad'
                continue
            ln.kind = 'stmt'

    def link(self):
        """Makes the nodes of every line and joins them up."""
        first = []
        for ln in self.lines:
            k = ln.kind
            if k in ('label', 'pp', 'budget'):
                ln.main = self.node(Node('nop'))
                first.append(ln.main)
            elif k == 'bad':
                ln.main = self.node(Node('bad'))
                first.append(ln.main)
            elif k == 'stmt':
                ln.main = self.node(stmt_node(ln.text))
                first.append(ln.main)
            else:
                ln.head_node = self.node(stmt_node(ln.head or ''))
                ln.slot_node = self.node(stmt_node(ln.slot or ''))
                first.append(ln.head_node if k != 'likely' else ln.head_node)
                if k in ('call', 'jalr'):
                    n = Node('call')
                    n.inval = ALL
                    n.flush = ALL & ~TRUST['call']
                    ln.main = self.node(n)
                elif k in ('ret', 'jr'):
                    n = Node('exit')
                    n.flush = ALL & ~(TRUST['ret'] if k == 'ret' else 0)
                    ln.main = self.node(n)
                else:
                    if ln.jump[0]:
                        n = Node('preempt')
                        n.flush = ALL
                        ln.pre_node = self.node(n)
                    if ln.jump[1] != 'goto':
                        n = Node('exit')
                        n.flush = ALL
                        ln.exit_node = self.node(n)
        self.first = first
        count = len(self.lines)
        for i, ln in enumerate(self.lines):
            k = ln.kind
            nxt = [first[i + 1]] if i + 1 < count else []
            nodes = self.nodes
            if k in ('label', 'pp', 'budget'):
                nodes[ln.main].succ = nxt
            elif k == 'stmt':
                nodes[ln.main].succ = [] if re.match(r'^stop\(c, .*\); return;$', ln.text) else nxt
            elif k == 'bad':
                pass
            elif k in ('call', 'jalr'):
                nodes[ln.head_node].succ = [ln.slot_node]
                nodes[ln.slot_node].succ = [ln.main]
                nodes[ln.main].succ = nxt
            elif k in ('ret', 'jr'):
                nodes[ln.head_node].succ = [ln.slot_node]
                nodes[ln.slot_node].succ = [ln.main]
            else:
                if ln.jump[1] == 'goto':
                    at = self.label_line.get(ln.jump[2])
                    if at is None:
                        target = self.node(Node('bad'))
                    else:
                        target = first[at]
                else:
                    target = ln.exit_node
                taken = target
                if ln.pre_node is not None:
                    nodes[ln.pre_node].succ = [target]
                    taken = ln.pre_node
                if k == 'branch':      # condition, slot, then the jump or the next line
                    nodes[ln.head_node].succ = [ln.slot_node]
                    nodes[ln.slot_node].succ = [taken] + nxt
                elif k == 'likely':    # the slot only runs when the branch is taken
                    nodes[ln.head_node].succ = [ln.slot_node] + nxt
                    nodes[ln.slot_node].succ = [taken]
                elif k == 'jump':
                    nodes[ln.head_node].succ = [ln.slot_node]
                    nodes[ln.slot_node].succ = [taken]
                else:                  # a bare goto or exit
                    nodes[ln.head_node].succ = [ln.slot_node]
                    nodes[ln.slot_node].succ = [taken]

    def line_nodes(self, ln):
        return [x for x in (ln.main, ln.head_node, ln.slot_node, ln.pre_node, ln.exit_node) if x is not None]


class Clone:
    """The hot copy that starts at one label."""

    def __init__(self, unit, label, stops, limit):
        self.unit = unit
        self.label = label
        self.stops = stops            # labels where other copies start: reaching one hands over to that copy
        u = unit
        self.stop_nodes = {u.first[u.label_line[x]]: x for x in stops if x in u.label_line and x != label}
        entry = u.first[u.label_line[label]]
        self.entry = entry
        seen = {entry}
        todo = [entry]
        while todo:
            n = todo.pop()
            if u.nodes[n].kind == 'bad':
                raise Unsupported('reaches code this tool does not handle')
            if n in self.stop_nodes:
                continue
            for s in u.nodes[n].succ:
                if s not in seen:
                    seen.add(s)
                    todo.append(s)
        self.nodes = seen
        self.line_ids = [i for i, ln in enumerate(u.lines) if any(x in seen for x in u.line_nodes(ln))]
        if len(self.line_ids) > limit:
            raise Unsupported('too large (%d lines)' % len(self.line_ids))

    def labels(self):
        return [self.unit.lines[i].label for i in self.line_ids if self.unit.lines[i].kind == 'label']

    def succ(self, n):
        return [] if n in self.stop_nodes else self.unit.nodes[n].succ

    def flush_of(self, n):
        return ALL if n in self.stop_nodes else self.unit.nodes[n].flush

    def solve(self):
        nodes = self.unit.nodes
        order = sorted(self.nodes)
        entry = self.entry
        succ = {n: self.succ(n) for n in order}
        preds = collections.defaultdict(list)
        for n in order:
            for s in succ[n]:
                preds[s].append(n)
        flush_at = {n: self.flush_of(n) for n in order}
        force = dict.fromkeys(order, 0)     # halves a loop keeps in locals: read at its top as far as this analysis goes

        def backward(step):
            value = dict.fromkeys(order, 0)
            todo = collections.deque(reversed(order))
            queued = set(order)
            while todo:
                n = todo.popleft()
                queued.discard(n)
                out = 0
                for s in succ[n]:
                    out |= value[s]
                v = step(n, out)
                if v != value[n]:
                    value[n] = v
                    for p in preds[n]:
                        if p not in queued:
                            queued.add(p)
                            todo.append(p)
            return value

        def forward(step, meet_or, first, top):
            """step(n, in) -> out; the value into a node is the meet of what its predecessors give it (first for the entry)."""
            out = dict.fromkeys(order, 0 if meet_or else top)
            into = {}
            todo = collections.deque(order)
            queued = set(order)
            while todo:
                n = todo.popleft()
                queued.discard(n)
                if meet_or:
                    v = first if n == entry else 0
                    for p in preds[n]:
                        v |= self.edge_value(p, n, out[p])
                else:
                    v = first if n == entry else top
                    for p in preds[n]:
                        v &= out[p]
                into[n] = v
                o = step(n, v)
                if o != out[n]:
                    out[n] = o
                    for s in succ[n]:
                        if s not in queued:
                            queued.add(s)
                            todo.append(s)
            return into, out

        for _ in range(16):
            # halves a later instruction reads before they are written again
            def read_step(n, out):
                x = nodes[n]
                if x.kind == 'preempt':
                    return out | force[n]
                return x.uses | force[n] | (out & ~(x.kills | x.inval))
            live_in = backward(read_step)
            live_out = {n: 0 for n in order}
            for n in order:
                for s in succ[n]:
                    live_out[n] |= live_in[s]
            load = {n: (0 if nodes[n].kind == 'preempt' else nodes[n].inval & live_out[n]) for n in order}
            entry_load = live_in[entry]

            # halves whose local holds the register's value on every way here
            def valid_step(n, v):
                x = nodes[n]
                if x.kind == 'preempt':
                    return v
                return ((v | x.kills) & ~x.inval) | load[n]
            valid_in, valid_out = forward(valid_step, False, entry_load, ALL)

            # halves that may differ from memory. A half that is not valid at a join is written back on the way there.
            self.valid_in = valid_in

            def dirty_step(n, d):
                x = nodes[n]
                if x.kind == 'preempt':
                    return d
                return ((d & ~flush_at[n]) | x.kills | x.maydefs) & ~x.inval
            dirty_in, dirty_out = forward(dirty_step, True, 0, 0)

            added = False
            for n in order:
                for p in preds[n]:
                    if p >= n:           # a jump back: keep the half in its local round the loop instead
                        e = dirty_out[p] & ~valid_in[n] & ~force[n]
                        if e:
                            force[n] |= e
                            added = True
            if not added:
                break

        for n in order:
            x = nodes[n]
            if x.uses & ~valid_in[n]:
                raise Unsupported('internal: a read of a register that was not loaded')
        self.flush = {n: (dirty_in[n] if nodes[n].kind == 'preempt' else flush_at[n] & dirty_in[n]) for n in order}
        self.load = load
        self.skipped = {n: dirty_in[n] & ~flush_at[n] for n in order if nodes[n].kind in ('call', 'exit')}
        self.entry_load = entry_load
        self.edge = {}
        for n in order:
            for p in preds[n]:
                e = dirty_out[p] & ~valid_in[n]
                if e:
                    self.edge[(p, n)] = e

        # registers whose upper word is known to copy bit 31 of the lower one
        def canon_step(n, v):
            x = nodes[n]
            if x.kind == 'preempt':
                return v
            out = v & ~regs_of(x.kills | x.maydefs | x.inval) | x.canon
            if x.move and (v >> x.move[1]) & 1:
                out |= 1 << x.move[0]
            return out
        self.canon, canon_out = forward(canon_step, False, 0, 0xFFFFFFFE)
        self.canon_out = canon_out

        self.used = entry_load
        for n in order:
            x = nodes[n]
            self.used |= x.uses | x.kills | x.maydefs | self.flush[n] | self.load[n]
        for e in self.edge.values():
            self.used |= e

    def edge_value(self, p, n, d):
        return d & self.valid_in[n]

    # ---- text

    def piece3(self, n, text):
        """One statement node as text: what must be written back before it, the statement, what is read again after it."""
        x = self.unit.nodes[n]
        if not text:
            return '', '', ''
        canon = self.canon[n]
        pre = flush_code(self.flush[n], canon)
        post = load_code(self.load[n])
        if x.budsync:
            pre += 'c.budget = bud; '
            post = 'bud = c.budget; ' + post
        return pre, stmt_text(x, text, canon), post

    def piece(self, n, text):
        pre, body, post = self.piece3(n, text)
        return (pre + body + ' ' + post) if body else ''

    def target_call(self, label, unit_name, uid, hot):
        if label in hot:
            return 'if (!%s(c)) %s(c, %du);' % (hot[label], unit_name, uid)
        return '%s(c, %du);' % (unit_name, uid)

    def edge_code(self, p, n):
        e = self.edge.get((p, n), 0)
        return flush_code(e, self.canon_out[p]) if e else ''

    def jump_text(self, ln, hot, ids):
        u = self.unit
        pre_pc, kind = ln.jump[0], ln.jump[1]
        out = ''
        src = ln.slot_node
        if pre_pc:
            n = ln.pre_node
            out += ('if (bud < 0) [[unlikely]] { %sc.budget = bud; if (preempt(c)) { c.pc = %s | kUnwind; return true; } bud = c.budget; } '
                    % (flush_code(self.flush[n], self.canon[n]), pre_pc))
            src = n
        if kind == 'goto':
            return out + self.edge_code(src, u.first[u.label_line[ln.jump[2]]]) + 'goto %s;' % ln.jump[2]
        n = ln.exit_node
        out += flush_code(self.flush[n], self.canon[n]) + 'c.budget = bud; '
        if kind == 'unit':
            label = ids[ln.jump[2]][ln.jump[3]]
            return out + self.target_call(label, ln.jump[2], ln.jump[3], hot) + ' return true;'
        return out + 'c.pc = %s; dispatch(c, 0u); return true;' % ln.jump[2]

    def emit(self, hot, ids, hints, number, gp=None):
        u = self.unit
        out = []
        decl = ''.join('x%d, y%d, ' % (r, r) for r in range(1, 32) if (self.used >> r | self.used >> (32 + r)) & 1)
        out.append('bool %s(Cpu &c) {' % hot[self.label])
        if decl:
            out.append('    std::uint32_t %s;' % decl[:-2])
        out.append('    PS2_HOT_GATE(%d)' % number)
        if self.label in LOSSY:   # port3ds/lossy.txt: return at once when the next picture is skipped
            out.append('    PS2_HOT_LOSSY(%s)' % ('true' if LOSSY[self.label] else 'false'))
        out.append('    std::int32_t bud = c.budget;')
        if self.entry_load:
            out.append('    ' + load_code(self.entry_load).rstrip())
        if u.lines[self.line_ids[0]].label != self.label:   # code before the entry is part of the copy when a jump goes back to it
            out.append('    goto %s;' % self.label)
        after_guard = ''
        last = None          # the node the previous line falls out of, if it can fall into the next one
        for i in self.line_ids:
            ln = u.lines[i]
            k = ln.kind
            if k == 'label':
                n = ln.main
                if last is not None:
                    code = self.edge_code(last, n)
                    if code:
                        out.append('    ' + code.rstrip())
                out.append(ln.label + ':')
                if n in self.stop_nodes:
                    other = self.stop_nodes[n]
                    uid = [x for x, lab in u.ids.items() if lab == other]
                    if not uid:
                        raise Unsupported('a copy starts at a label that is not an entry')
                    out.append('    { %sc.budget = bud; %s return true; }' % (flush_code(self.flush[n], self.canon[n]), self.target_call(other, u.name, uid[0], hot)))
                    last = None
                else:
                    last = n
            elif k == 'pp':
                out.append(ln.text)
                if ln.text.startswith('#endif') and after_guard:
                    out.append('    ' + after_guard.rstrip())
                    after_guard = ''
                last = ln.main
            elif k == 'budget':
                out.append('    bud -= %d;' % ln.n)
                last = ln.main
            elif k == 'stmt':
                last = ln.main if u.nodes[ln.main].succ else None
                if RE_HOOK.match(ln.text):
                    continue
                pre, body, post = self.piece3(ln.main, ln.text)
                if out[-1].startswith('#if'):       # a native only some builds call: the write-backs are for every build
                    guard = out.pop()
                    if pre:
                        out.append('    ' + pre.rstrip())
                    out.append(guard)
                    out.append('    ' + body)
                    after_guard = post
                else:
                    out.append('    ' + (pre + body + ' ' + post).rstrip())
            elif k in ('call', 'jalr'):
                n = ln.main
                head = (self.piece(ln.head_node, ln.head) + self.piece(ln.slot_node, ln.slot) + flush_code(self.flush[n], self.canon[n])
                        + poison_code(self.skipped[n]) + 'c.budget = bud; ')
                if k == 'jalr':
                    call = 'c.pc = t; call(c, t);'
                elif ln.callee[0] == 'call':
                    call = 'call(c, %s);' % ln.callee[1]
                else:
                    label = ids[ln.callee[1]][ln.callee[2]]
                    call = 'if (stack_ok()) { %s } else { c.pc = %s; return true; }' % (self.target_call(label, ln.callee[1], ln.callee[2], hot), ln.callee[3])
                out.append('    { %s%s if (c.pc != %s) [[unlikely]] return true; bud = c.budget; %s}' % (head, call, ln.ret, load_code(self.load[n])))
                last = n
            elif k == 'ret':
                n = ln.main
                out.append('    { %s%s%s%sc.budget = bud; c.pc = t; return true; }'
                           % (self.piece(ln.head_node, ln.head), self.piece(ln.slot_node, ln.slot), flush_code(self.flush[n], self.canon[n]), poison_code(self.skipped[n])))
                last = None
            elif k == 'jr':
                n = ln.main
                out.append('    { %s%s%sc.budget = bud; c.pc = t; dispatch(c, 0u); return true; }'
                           % (self.piece(ln.head_node, ln.head), self.piece(ln.slot_node, ln.slot), flush_code(self.flush[n], self.canon[n])))
                last = None
            elif k == 'branch':
                hint = hints.get(i, '')
                out.append('    { %s%sif (t) %s{ %s } }' % (self.piece(ln.head_node, ln.head), self.piece(ln.slot_node, ln.slot), hint, self.jump_text(ln, hot, ids)))
                last = ln.slot_node
            elif k == 'likely':
                hint = hints.get(i, '')
                out.append('    { %sif (t) %s{ %s%s } }' % (self.piece(ln.head_node, ln.head), hint, self.piece(ln.slot_node, ln.slot), self.jump_text(ln, hot, ids)))
                last = ln.head_node
            elif k == 'jump':
                out.append('    { %s%s }' % (self.piece(ln.slot_node, ln.slot), self.jump_text(ln, hot, ids)))
                last = None
            elif k == 'goto':
                out.append('    ' + self.jump_text(ln, hot, ids))
                last = None
            else:
                raise Unsupported('line kind ' + k)
        out.append('}')
        text = '\n'.join(out) + '\n'
        if gp is not None and (self.used >> 28 | self.used >> 60) & 1 and self.gp_fixed():
            # The copy only runs when gp has the game's one value, so every gp-relative address is a constant in main
            # RAM: no range test, no slow path. It assumes that the functions it calls leave gp alone.
            text = re.sub(r'[xy]28 = c\.r\[28\]\.w\[[01]\]; ?', '', text)
            text = re.sub(r'(, )?\bx28, y28', '', text, count=1).replace('    std::uint32_t , ', '    std::uint32_t ').replace('    std::uint32_t ;\n', '')
            guard = '    if (c.r[28].w[0] != %#010xu%s) return false;\n    constexpr std::uint32_t x28 = %#010xu, y28 = 0u;\n' % (
                gp, ' || c.r[28].w[1] != 0u' if (self.used >> 60) & 1 else '', gp)
            at = text.index('    std::int32_t bud = c.budget;')
            text = text[:at] + guard + text[at:]
        return text

    def gp_fixed(self):
        for n in self.nodes:
            x = self.unit.nodes[n]
            if (x.kills | x.maydefs) & both(28) or (not x.ccall and (x.keep >> 28) & 1):
                return False
        return True


LOSSY = {}   # label -> True when v0 is set to a0

HEADER = '''// Made by tools/p3fes_eehot.py from your own disc: keep it private.
#pragma once

#include "units.hpp"

namespace gen {

[[gnu::always_inline]] inline void put32(std::uint32_t &lo, std::uint32_t &hi, std::uint32_t v) {
    lo = v;
    hi = static_cast<std::uint32_t>(static_cast<std::int32_t>(v) >> 31);
}
[[gnu::always_inline]] inline void put64(std::uint32_t &lo, std::uint32_t &hi, std::uint64_t v) {
    lo = static_cast<std::uint32_t>(v);
    hi = static_cast<std::uint32_t>(v >> 32);
}
[[gnu::always_inline]] inline std::uint64_t get64(std::uint32_t lo, std::uint32_t hi) { return (static_cast<std::uint64_t>(hi) << 32) | lo; }

// -DPS2_HOT_BISECT: only copies number lo to hi-1 run, from PS2_HOT_RANGE=lo:hi in the environment, to find a faulty one
#ifdef PS2_HOT_BISECT
extern int g_hot_lo, g_hot_hi;
#define PS2_HOT_GATE(k) if ((k) < g_hot_lo || (k) >= g_hot_hi) return false;
#elif defined(PS2_HOT_SAMPLE)
// SAMPLE=1 on the 3DS: the number of the copy running now, plus one, for the sampler in port3ds/main3ds.cpp ("sample 1")
extern volatile std::uint16_t g_hot_now;
struct HotNow {
    std::uint16_t outer;
    explicit HotNow(int k) : outer(g_hot_now) { g_hot_now = static_cast<std::uint16_t>(k + 1); }
    ~HotNow() { g_hot_now = outer; }
};
#define PS2_HOT_GATE(k) HotNow ps2_hot_now(k);
#else
#define PS2_HOT_GATE(k)
#endif

// --lossy: on the 3DS, port3ds/native3ds.cpp says whether the copy returns at once (a0: with v0 = a0, else v0 = 0)
#ifdef __3DS__
bool lossy_return(ps2::Cpu &c, bool keep_a0);
#define PS2_HOT_LOSSY(a0) if (lossy_return(c, a0)) return true;
#else
#define PS2_HOT_LOSSY(a0)
#endif

'''

BISECT = '''#ifdef PS2_HOT_BISECT
#include <cstdio>
#include <cstdlib>
namespace gen {
int g_hot_lo = 0, g_hot_hi = 1 << 30;
static const int g_hot_range = [] {
    if (const char *e = std::getenv("PS2_HOT_RANGE")) std::sscanf(e, "%d:%d", &g_hot_lo, &g_hot_hi);
    return 0;
}();
}
#endif

'''


def read_profile(paths):
    """label -> {ordinal: count}; a count is the guest instructions charged by that budget statement, summed over the files,
    each file scaled to the same total so that one long recording does not outweigh the others."""
    prof = collections.defaultdict(lambda: collections.defaultdict(float))
    for path in paths:
        rows = []
        for line in open(path):
            p = line.split()
            if len(p) == 3:
                rows.append((p[0], int(p[1]), int(p[2])))
        total = sum(r[2] for r in rows) or 1
        for label, k, v in rows:
            prof[label][k] += v * 1e9 / total
    return prof


def load_units(folder):
    units = {}
    for path in sorted(glob.glob(os.path.join(folder, 'unit_*.cpp'))):
        u = Unit(path)
        units[u.name] = u
    return units


def call_targets(units):
    """Labels that a direct call anywhere in the game goes to."""
    out = set()
    for u in units.values():
        for ln in u.lines:
            if ln.kind == 'call' and ln.callee[0] == 'unit':
                out.add(units[ln.callee[1]].ids[ln.callee[2]])
    return out


def cmd_make(args):
    if args.trust >= 1:
        TRUST['call'] = CALL_TEMPS
    if args.trust >= 2:
        TRUST['ret'] = RET_TEMPS
    TRUST['check'] = args.trust_check
    units = load_units(args.source)
    if not units:
        raise SystemExit('no unit_*.cpp in ' + args.source)
    prof = read_profile(args.profile)
    weight = {label: sum(v.values()) for label, v in prof.items()}
    total = sum(weight.values()) or 1.0
    where = {}
    for u in units.values():
        for label in u.label_line:
            where[label] = u
    natives = {}
    if args.natives:
        for line in open(args.natives):
            p = line.split()
            if len(p) == 2 and not p[1].startswith('hot_'):
                natives[int(p[0], 16)] = p[1]
    native_labels = {'L_%08X' % a for a in natives}
    ids = {u.name: u.ids for u in units.values()}

    # Candidates: called functions that ran, plus (below) busy entry points nothing covers, which are functions called
    # through a pointer. A copy is valid at any label: it is everything that can be reached from there.
    called = call_targets(units)
    entries = set()
    for u in units.values():
        entries.update(u.ids.values())
    return_points = set()
    for u in units.values():
        for i, ln in enumerate(u.lines):
            if ln.kind in ('call', 'jalr') and i + 1 < len(u.lines) and u.lines[i + 1].kind == 'label':
                return_points.add(u.lines[i + 1].label)
    floor = total * args.min_share / 100.0

    def build(chosen):
        clones = {}
        failed = {}
        for label in sorted(chosen):
            try:
                c = Clone(where[label], label, chosen & starts, args.max_lines)   # only a function's own copy takes over
                clones[label] = c
            except Unsupported as e:
                failed[label] = str(e)
        return clones, failed

    def region_weight(c):
        return sum(weight.get(x, 0.0) for x in c.labels())

    starts = set(called)
    if args.functions:      # where eerecomp says functions start (EERECOMP_FUNCTIONS=file), which includes those only called through a pointer
        starts |= {'L_' + line.strip() for line in open(args.functions) if line.strip()}
    chosen = {x for x in starts if weight.get(x, 0.0) > 0 and x not in native_labels and x in where}
    if args.skip:           # copies measured to be slower than the original
        native_labels |= {'L_' + line.split()[0] for line in open(args.skip) if line.strip()}
        chosen -= native_labels
    import bisect
    ordered = sorted(starts)

    def owner(label):
        k = bisect.bisect_right(ordered, label)
        return ordered[k - 1] if k else None
    for _ in range(6):
        clones, failed = build(chosen)
        keep = {x for x, c in clones.items() if region_weight(c) >= floor}
        covered = set()
        for x in keep:
            covered.update(clones[x].labels())
        extra = set()
        if args.pointers:
            # busy code no copy reaches: a loop in a function that was entered before the recording began, or the code
            # after a jump through a table. Such a copy runs on through the labels of others like it (see build).
            loose = sorted((x for x in weight if x not in covered and x in entries and x not in return_points and x not in native_labels
                            and x in where and x not in failed and weight[x] >= floor), key=lambda x: -weight[x])
            extra = set(loose[:200])
        if keep | extra == chosen:
            break
        chosen = keep | extra
    clones, failed = build(chosen)
    for label in list(clones):
        try:
            clones[label].solve()
        except Unsupported as e:
            failed[label] = str(e)
            del clones[label]
    hot = {label: 'hot_' + label[2:] for label in clones}
    # a copy that failed late must not be called by the others: build once more without it
    clones2 = {}
    for label in sorted(clones):
        c = Clone(where[label], label, set(clones) & starts, args.max_lines)
        c.solve()
        clones2[label] = c
    clones = clones2

    if args.lossy:
        for line in open(args.lossy):
            line = line.split('#')[0].split()
            if line:
                LOSSY['L_' + line[0].upper()] = line[1] == 'a0'
        lost = sorted(set(LOSSY) - set(clones))
        if lost:
            raise SystemExit('--lossy: these got no hot copy: ' + ', '.join(lost))
    texts = {}
    lines_total = 0
    for number, (label, c) in enumerate(sorted(clones.items())):
        hints = branch_hints(c, prof) if not args.no_hints else {}
        try:
            texts[label] = c.emit(hot, ids, hints, number, int(args.gp, 16) if args.gp else None)
            lines_total += len(c.line_ids)
        except Unsupported as e:
            failed[label] = str(e)
    if set(texts) != set(clones):
        raise SystemExit('some copies could not be written after they were chosen: ' + ', '.join(sorted(set(clones) - set(texts))))

    covered = set()
    for c in clones.values():
        covered.update(c.labels())
    cov = sum(weight.get(x, 0.0) for x in covered)
    os.makedirs(args.out, exist_ok=True)
    for old in glob.glob(os.path.join(args.out, 'hot_*.cpp')) + glob.glob(os.path.join(args.out, 'hot.hpp')):
        os.remove(old)
    with open(os.path.join(args.out, 'hot.hpp'), 'w') as f:
        f.write(HEADER + ''.join('bool %s(ps2::Cpu &c);\n' % hot[x] for x in sorted(texts)) + '\n}\n')
    order = sorted(texts)
    files = max(1, (len(order) + args.per_file - 1) // args.per_file)
    for k in range(files):
        with open(os.path.join(args.out, 'hot_%02d.cpp' % k), 'w') as f:
            f.write('// Made by tools/p3fes_eehot.py from your own disc: keep it private.\n#include "hot.hpp"\n\n' + (BISECT if k == 0 else '')
                    + 'namespace gen {\n\nusing namespace ps2;\n\n')
            for label in order[k * args.per_file:(k + 1) * args.per_file]:
                f.write(texts[label] + '\n')
            f.write('}\n')
    if args.natives_out:
        with open(args.natives_out, 'w') as f:
            for a, name in sorted(natives.items()):
                f.write('%08X %s\n' % (a, name))
            for label in order:
                f.write('%s %s\n' % (label[2:], hot[label]))
    if args.verbose:
        for number, label in enumerate(order):
            print('  copy %d: %s, %d lines, %.2f%%' % (number, label, len(clones[label].line_ids), 100.0 * sum(weight.get(x, 0.0) for x in clones[label].labels()) / total))
    print('%d hot copies, %d lines of generated code, covering %.1f%% of the profile (%d files)' % (len(texts), lines_total, 100.0 * cov / total, files))
    if args.verbose:
        rest = sorted(((weight[x], x) for x in weight if x not in covered), reverse=True)[:25]
        for w, x in rest:
            print('  not covered: %s %.2f%%%s' % (x, 100.0 * w / total, '  (' + failed[x] + ')' if x in failed else ''))
        for label, why in sorted(failed.items(), key=lambda kv: -weight.get(kv[0], 0.0))[:25]:
            print('  no copy at %s (%.2f%%): %s' % (label, 100.0 * weight.get(label, 0.0) / total, why))


def branch_hints(clone, prof):
    """[[likely]] or [[unlikely]] for branches that always or never went one way in the profile."""
    u = clone.unit
    hints = {}

    def runs(i):
        """How often the code from line i ran, if the profile can tell: the count of the first budget statement on the way."""
        while i < len(u.lines):
            ln = u.lines[i]
            if ln.kind == 'budget':
                return prof.get(ln.ordinal[0], {}).get(ln.ordinal[1], 0.0) / ln.n
            if ln.kind not in ('label', 'pp'):
                return None
            i += 1
        return None

    for i in clone.line_ids:
        ln = u.lines[i]
        if ln.kind not in ('branch', 'likely') or i == 0:
            continue
        before = u.lines[i - 1]
        if before.kind != 'budget':
            continue
        here = prof.get(before.ordinal[0], {}).get(before.ordinal[1], 0.0) / before.n
        if here <= 0:
            continue
        fall = runs(i + 1)
        taken = None
        if ln.jump[1] == 'goto' and ln.jump[2] in u.label_line:
            taken = runs(u.label_line[ln.jump[2]])
        elif ln.jump[1] != 'goto':
            taken = None
        if fall is not None and fall <= 0:
            hints[i] = '[[likely]] '
        elif taken is not None and taken <= 0:
            hints[i] = '[[unlikely]] '
    return hints


def cmd_instrument(args):
    """A copy of the generated code that counts the guest instructions each budget statement charges. Build it (the
    native build is enough), run a scene with PS2_EEHOT=file in the environment, and the profile is in that file."""
    os.makedirs(args.out, exist_ok=True)
    k = 0
    rows = []
    names = sorted(glob.glob(os.path.join(args.source, '*')))
    for path in names:
        base = os.path.basename(path)
        text = open(path).read()
        if base.startswith('unit_') and base.endswith('.cpp'):
            out = []
            label = '?'
            ordinal = 0
            for line in text.split('\n'):
                m = RE_LABEL.match(line)
                if m:
                    label = m.group(1)
                    ordinal = 0
                m = RE_BUDGET.match(line.strip())
                if m and line.startswith('    '):
                    rows.append((label, ordinal))
                    line = '    g_eehot[%d] += %s; c.budget -= %s;' % (k, m.group(1), m.group(1))
                    k += 1
                    ordinal += 1
                out.append(line)
            text = '\n'.join(out).replace('namespace gen {\n', 'namespace gen {\nextern unsigned long long g_eehot[];\n', 1)
        if base.startswith('hot'):
            continue
        with open(os.path.join(args.out, base), 'w') as f:
            f.write(text)
    with open(os.path.join(args.out, 'eehot_counts.cpp'), 'w') as f:
        f.write('#include <cstdio>\n#include <cstdlib>\n\nnamespace gen {\nunsigned long long g_eehot[%d];\n' % k)
        f.write('static const char *const kWhere[] = {\n')
        for label, ordinal in rows:
            f.write('    "%s %d",\n' % (label, ordinal))
        f.write('};\nstruct EehotDump {\n    ~EehotDump() {\n        const char *p = std::getenv("PS2_EEHOT");\n        std::FILE *f = p ? std::fopen(p, "w") : nullptr;\n'
                '        if (!f) return;\n        for (int k = 0; k < %d; ++k) if (g_eehot[k]) std::fprintf(f, "%%s %%llu\\n", kWhere[k], g_eehot[k]);\n'
                '        std::fclose(f);\n    }\n} g_eehot_dump;\n}\n' % k)
    print('%d counters in %s' % (k, args.out))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    mk = sub.add_parser('make', help='write the hot copies')
    mk.add_argument('source', help='generated code made with EERECOMP_NOLOOPS=1')
    mk.add_argument('out', help='folder for hot_*.cpp and hot.hpp (the generated game folder)')
    mk.add_argument('--profile', nargs='+', required=True)
    mk.add_argument('--natives', help='the natives list the game is translated with')
    mk.add_argument('--natives-out', help='that list plus one line per hot copy, for eerecomp --natives')
    mk.add_argument('--min-share', type=float, default=0.02, help='leave out copies with less than this percent of the profile')
    mk.add_argument('--max-lines', type=int, default=4000)
    mk.add_argument('--per-file', type=int, default=40)
    mk.add_argument('--no-pointers', dest='pointers', action='store_false', help='only functions that are called directly')
    mk.add_argument('--functions', help='function starts, written by eerecomp when EERECOMP_FUNCTIONS=file is set')
    mk.add_argument('--skip', help='a file of addresses that get no copy')
    mk.add_argument('--lossy', help='a file of addresses, then 0 or a0: their copies return at once for a skipped picture')
    mk.add_argument('--trust', type=int, default=0, help='1: do not write back registers a called function does not read; '
                    '2: also those a caller does not read after a return (not exact, see CALL_TEMPS in this file)')
    mk.add_argument('--trust-check', action='store_true', help='with --trust: write markers where a write-back was left out, for a native check run')
    mk.add_argument('--no-hints', action='store_true')
    mk.add_argument('--gp', help="the game's gp register value in hex: copies check it once and use fixed addresses")
    mk.add_argument('-v', '--verbose', action='store_true')
    mk.set_defaults(fn=cmd_make)
    ins = sub.add_parser('instrument', help='write a counting copy of the generated code, to record a profile')
    ins.add_argument('source')
    ins.add_argument('out')
    ins.set_defaults(fn=cmd_instrument)
    args = ap.parse_args()
    args.fn(args)


if __name__ == '__main__':
    main()
