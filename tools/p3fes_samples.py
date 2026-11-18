"""Reads sdmc:/p3fes/samples.txt from a SAMPLE=1 3DS build (see port3ds/Makefile) and ranks the game functions by time.

    python3 tools/p3fes_samples.py samples.txt [--from N] [--to N] [--names map.csv|symbol_addrs.txt ...] [--top 40]

Each sample says which copy from tools/p3fes_eehot.py the game thread was in (the innermost one; code without a copy counts for
the copy that called it) and which activity of port3ds/prof.hpp was running: "game" is the translated code itself, the others
are the runtime working for it (vif, dma, kernel and so on, and x0 to x9 for PS2_PROF_EXTRA blocks) while that copy waits. Periods are 10 seconds each; --from and
--to pick some, to leave out the loading at the start. --names takes a function list from a related project to name addresses:
a CSV with Name,Start,End columns, or lines like "name = 0x00100008; // type:func".
"""
import argparse, bisect, collections, csv, glob, os, re

ACTS = ['vu', 'gif', 'upload', 'draw', 'iop', 'spu', 'ipu', 'vif', 'dma', 'events', 'kernel', 'present', 'wait', 'full', 'game'] + ['x%d' % k for k in range(10)]
GAME = 14


def copies(gen):
    """Copy number -> start address, from the generated hot_*.cpp files."""
    out = {}
    for path in glob.glob(os.path.join(gen, 'hot_*.cpp')):
        name = None
        for line in open(path):
            m = re.match(r'bool hot_([0-9A-F]{8})\(Cpu &c\) \{', line)
            if m:
                name = int(m.group(1), 16)
                continue
            m = re.match(r'\s*PS2_HOT_GATE\((\d+)\)', line)
            if m and name is not None:
                out[int(m.group(1))] = name
                name = None
    return out


def names(paths):
    starts, ends, labels = [], [], []
    rows = []
    for p in paths:
        if p.endswith('.csv'):
            for r in csv.DictReader(open(p)):
                try:
                    rows.append((int(r['Start'], 16), int(r['End'], 16), r['Name']))
                except (KeyError, ValueError):
                    pass
        else:
            for line in open(p):
                m = re.match(r'\s*(\w+)\s*=\s*0x([0-9A-Fa-f]+);.*type:func', line)
                if m:
                    rows.append((int(m.group(2), 16), int(m.group(2), 16) + 1, m.group(1)))
    rows.sort()
    for a, e, n in rows:
        starts.append(a), ends.append(e), labels.append(n)

    def find(a):
        i = bisect.bisect_right(starts, a) - 1
        if i >= 0 and (starts[i] == a or a < ends[i]):
            return labels[i]
        return ''
    return find


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('samples')
    ap.add_argument('--gen', default=os.path.join(os.path.dirname(__file__), '..', '..', 'ps2-web-recomp', 'generated', 'p3fes'))
    ap.add_argument('--from', dest='first', type=int, default=0)
    ap.add_argument('--to', type=int, default=1 << 30)
    ap.add_argument('--names', nargs='*', default=[])
    ap.add_argument('--top', type=int, default=40)
    a = ap.parse_args()

    per = collections.Counter()
    acts = collections.Counter()
    by = collections.defaultdict(collections.Counter)
    period = -1
    for line in open(a.samples):
        x = line.split()
        if x[0] == 'period':
            period = int(x[1])
            continue
        if not a.first <= period <= a.to:
            continue
        k, act, n = int(x[0]), int(x[1]), int(x[2])
        per[k] += n
        acts[act] += n
        by[k][act] += n
    total = sum(per.values())
    if not total:
        raise SystemExit('no samples in those periods')
    num = copies(a.gen)
    find = names(a.names)
    print('%d samples (%.1f s)' % (total, total / 4000.0))
    print('by activity: ' + ', '.join('%s %.1f%%' % (ACTS[i] if i < len(ACTS) else str(i), 100.0 * v / total)
                                    for i, v in acts.most_common()))
    print('  share   game  copy      name / where the rest went')
    for k, v in per.most_common(a.top):
        if k == 0:
            where, nm = '-', '(outside every copy)'
        else:
            addr = num.get(k - 1)
            where = '%08X' % addr if addr is not None else '#%d' % (k - 1)
            nm = find(addr) if addr is not None else ''
        game = by[k].get(GAME, 0)
        rest = ', '.join('%s %.1f' % (ACTS[i] if i < len(ACTS) else str(i), 100.0 * n / total)
                         for i, n in by[k].most_common(3) if i != GAME)
        print('  %5.2f%%  %5.2f%%  %s  %-36s %s' % (100.0 * v / total, 100.0 * game / total, where, nm, rest))


if __name__ == '__main__':
    main()
