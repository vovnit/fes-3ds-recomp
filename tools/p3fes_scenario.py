"""Prepare reproducible 3DS benchmark inputs and compare matching perf windows."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def prepare(args):
    settings = Path(args.settings).read_text()
    if not settings.strip() or any(len(line.encode('utf-8')) >= 127 for line in settings.splitlines()):
        raise ValueError('settings must be nonempty and each line shorter than 127 bytes')
    settings.encode('ascii')
    keys = [line.split()[0] for line in settings.splitlines() if line.strip()]
    if 'frames' not in keys or 'draw' not in keys:
        raise ValueError('explicit frames and draw settings are required')
    loads = [line.split()[1:] for line in settings.splitlines() if line.split()[:1] == ['load']]
    if loads and (not args.state or len(loads) != 1 or loads[0] != [Path(args.state).name]):
        raise ValueError('load must match the supplied local --state filename')
    if args.state and not loads:
        raise ValueError('--state requires a matching load setting')
    manifest = {'schema': 1, 'name': args.name, 'settings': settings,
                'binary_sha256': digest(args.binary),
                'state_sha256': digest(args.state) if args.state else None}
    out = Path(args.output)
    out.mkdir(parents=True, exist_ok=False)
    (out / 'args.txt').write_text(settings)
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(out / 'manifest.json')


def windows(path):
    result = {}
    for line in Path(path).read_text().splitlines():
        words = line.split()
        if not words or words[0] != 'frame':
            continue
        try:
            frame = int(words[1])
            elapsed = float(words[words.index('ms_30frames') + 1])
        except (ValueError, IndexError) as exc:
            raise ValueError('malformed performance row: ' + line) from exc
        if frame in result or elapsed <= 0:
            raise ValueError('duplicate frame or nonpositive timing')
        result[frame] = elapsed / 30
    return result


def compare(before, after):
    a, b = windows(before), windows(after)
    common = sorted(a.keys() & b.keys())
    if not common:
        raise ValueError('no matching frame windows')
    return {'matched_windows': len(common), 'unmatched_before': len(a)-len(common),
            'unmatched_after': len(b)-len(common),
            'median_before_ms_per_game_frame': statistics.median(a[f] for f in common),
            'median_after_ms_per_game_frame': statistics.median(b[f] for f in common),
            'median_paired_change_percent': statistics.median((b[f]/a[f]-1)*100 for f in common),
            'note': '30-frame averages, not individual-frame percentiles or displayed-picture cadence'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    p = commands.add_parser('prepare')
    for name in ('name', 'settings', 'binary', 'output'):
        p.add_argument('--' + name, required=True)
    p.add_argument('--state')
    p = commands.add_parser('compare')
    p.add_argument('before')
    p.add_argument('after')
    args = parser.parse_args()
    try:
        if args.command == 'prepare':
            prepare(args)
        else:
            print(json.dumps(compare(args.before, args.after), indent=2))
    except (OSError, ValueError) as exc:
        parser.exit(1, str(exc) + '\n')


if __name__ == '__main__':
    main()
