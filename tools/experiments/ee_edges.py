"""Instrument conditional outcomes in one private generated unit (host research only)."""
import argparse
import hashlib
import json
from pathlib import Path
import re


def instrument(source):
    sites = []
    label = None
    lines = []
    for number, line in enumerate(source.splitlines(), 1):
        match = re.match(r'L_([0-9A-F]{8}):', line)
        if match:
            label = match[1]
        if 'if (t)' in line:
            if label is None or line.count('if (t)') != 1:
                raise ValueError('unsupported branch shape')
            index = len(sites)
            sites.append({'site': index, 'label': label, 'line': number, 'within_label': sum(s['label'] == label for s in sites)})
            line = line.replace('if (t)', f'if (ee_edge({index}, t))')
        lines.append(line)
    if not sites:
        raise ValueError('no conditional branches')
    header = '''#include <cstdio>
#include <cstdlib>
namespace {
static unsigned long long ee_counts[COUNT][2] = {};
static bool ee_edge(unsigned i, bool taken) { ++ee_counts[i][taken]; return taken; }
struct EeEdgeDump {
    ~EeEdgeDump() {
        const char *path = std::getenv("PS2_EE_EDGES");
        if (!path) return;
        FILE *f = std::fopen(path, "w");
        if (!f) return;
        std::fprintf(f, "site,not_taken,taken\\n");
        for (unsigned i=0; i<COUNT; ++i)
            std::fprintf(f, "%u,%llu,%llu\\n", i, ee_counts[i][0], ee_counts[i][1]);
        std::fclose(f);
    }
} ee_dump;
}
'''.replace('COUNT', str(len(sites)))
    return header + '\n'.join(lines) + '\n', sites


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('unit', type=Path)
    p.add_argument('output', type=Path)
    a = p.parse_args()
    source = a.unit.read_text()
    patched, sites = instrument(source)
    a.output.mkdir(parents=True, exist_ok=False)
    (a.output/a.unit.name).write_text(patched)
    (a.output/'sites.json').write_text(json.dumps({'source_sha256': hashlib.sha256(source.encode()).hexdigest(), 'sites': sites}, indent=2)+'\n')
    print(f'{len(sites)} branches instrumented')

if __name__ == '__main__':
    main()
