"""Rank captured branch outcomes; counts describe one instrumented replay, not timing."""
import argparse
import csv
import json
from pathlib import Path


def report(sites_path, counts_path):
    sites = json.loads(Path(sites_path).read_text())
    known = {s['site']: s for s in sites['sites']}
    rows = []
    seen = set()
    with Path(counts_path).open() as stream:
        for row in csv.DictReader(stream):
            i, no, yes = (int(row[k]) for k in ('site','not_taken','taken'))
            if i not in known or i in seen or min(no,yes)<0:
                raise ValueError('invalid or duplicate counter')
            seen.add(i)
            if no+yes:
                rows.append(dict(known[i], executions=no+yes, taken=yes, not_taken=no,
                                 taken_fraction=yes/(no+yes)))
    if seen != set(known):
        raise ValueError('incomplete counter file')
    rows.sort(key=lambda r: (-r['executions'],r['site']))
    return {'source_sha256': sites['source_sha256'], 'active_sites':len(rows),'branches':rows}

if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('sites'); p.add_argument('counts')
    a=p.parse_args()
    print(json.dumps(report(a.sites,a.counts),indent=2))
