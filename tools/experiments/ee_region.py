"""Generate a private differential experiment from the owner's translated unit.
No game-derived source is stored in this repository. Output stays local.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re


def extract(source):
    function = source.split('L_004D59D0:\n', 1)[1]
    start = function.index('    c.budget -= 13;')
    end = function.index('    set32(c.r[4], ld32', start)
    region = function[start:end]
    # Fail closed if regeneration changes this experiment's structural contract.
    labels = re.findall(r'^L_([0-9A-F]+):', region, re.M)
    targets = set(re.findall(r'goto L_([0-9A-F]+)', region))
    if labels != ['004D5A08'] or targets != {'004D5A08', '004D5DC8', '004D5CA4'}:
        raise ValueError('unexpected control flow')
    if re.search(r'\b(?:ld\d+|st\d+|unit_\w+|return|while|for)\b', region):
        raise ValueError('region includes unsupported operations')
    calls = set(re.findall(r'\b(\w+)\s*\(', region))
    if calls - {'set32', 'if'}:
        raise ValueError('unexpected calls: ' + str(calls))
    if set(map(int, re.findall(r'c\.r\[(\d+)\]', region))) != {2, 3, 4, 8, 17}:
        raise ValueError('unexpected register set')
    if re.search(r'\.(?:d|sd|w)\[[1-9]', region):
        raise ValueError('upper register access is unsupported')
    writes = set(map(int, re.findall(r'set32\(c\.r\[(\d+)\]', region)))
    writes.update(map(int, re.findall(r'c\.r\[(\d+)\]\.\w+\[0\]\s*=(?!=)', region)))
    if writes != {2, 3, 17}:
        raise ValueError('unexpected written registers')
    return region


def functions(region):
    regs = sorted(set(map(int, re.findall(r'c\.r\[(\d+)\]', region))))
    original = region
    for target in ('004D5DC8', '004D5CA4'):
        original = original.replace('goto L_' + target, 'return 0x' + target)
    original += '\nreturn 0x004D5A24;\n'
    local = original
    for r in regs:
        local = local.replace(f'c.r[{r}]', f'r{r}')
    flush = ' '.join(f'c.r[{r}].d[0] = r{r}.d[0];' for r in (2, 3, 17))
    local = re.sub(r'return (0x[0-9A-F]+);', lambda m: '{ ' + flush + ' return ' + m[1] + '; }', local)
    local = '\n'.join(f'LowReg r{r}; r{r}.d[0] = c.r[{r}].d[0];' for r in regs) + '\n' + local
    return '\n'.join('__attribute__((noinline)) unsigned ' + name + '(Cpu &c) {\n' + body + '\n}' for name, body in [('baseline', original), ('candidate', local)])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('unit', type=Path)
    p.add_argument('output', type=Path)
    a = p.parse_args()
    body = functions(extract(a.unit.read_text()))
    a.output.mkdir(parents=True, exist_ok=False)
    header = '''#include <cstdint>
#include <cstring>
#include <cstdio>
#include <chrono>
union alignas(16) Reg { std::uint64_t d[2]; std::int64_t sd[2]; std::uint32_t w[4]; };
union LowReg { std::uint64_t d[1]; std::int64_t sd[1]; std::uint32_t w[2]; };
struct Cpu { Reg r[32]; std::int64_t budget; };
template<class R> inline void set32(R &r, std::uint32_t v) { r.sd[0] = static_cast<std::int32_t>(v); }
'''
    harness = r'''
int main() {
    std::uint64_t seed = 0x123456789abcdefULL;
    auto next = [&]() { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; };
    unsigned exits[3] = {};
    for (unsigned n=0; n<1000000; ++n) {
        Cpu a{};
        for (auto &r:a.r) { r.d[0]=next(); r.d[1]=next(); }
        a.budget = static_cast<std::int64_t>(n%101)-50;
        // Force all branches, including delay-slot overflow and noncanonical upper words.
        if (n%4==0) a.r[8].d[0]=0;
        if (n%4==1) a.r[4].d[0]=0;
        if (n%4==2) a.r[4].d[0]=4353;
        if (n%8==0) a.r[17].d[0]=0x7fffffff;
        if (n%8==1) a.r[17].d[0]=0xffffffff;
        Cpu b=a;
        auto x=baseline(a), y=candidate(b);
        if (x!=y || std::memcmp(&a,&b,sizeof a)) { std::printf("Mismatch %u\n",n); return 1; }
        ++exits[x==0x004D5DC8 ? 0 : x==0x004D5CA4 ? 1 : 2];
    }
    std::printf("Matched 1000000 cases; exits %u %u %u\n",exits[0],exits[1],exits[2]);
    for (int round=0; round<4; ++round) {
        for (auto fn : {baseline, candidate}) {
            Cpu c{}; c.r[4].d[0]=4353; c.r[8].d[0]=1; c.budget=1000000000;
            auto begin=std::chrono::steady_clock::now(); unsigned checksum=0;
            for (unsigned n=0;n<10000000;++n) checksum+=fn(c);
            auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count();
            std::printf("%s %.3f ns/call checksum %u\n", fn==baseline?"baseline":"candidate",ns/10000000.0,checksum);
        }
    }
}
'''
    (a.output/'experiment.cpp').write_text(header + '#include <initializer_list>\n' + body + harness)
    (a.output/'manifest.json').write_text(json.dumps({'unit_sha256': hashlib.sha256(a.unit.read_bytes()).hexdigest(), 'region_sha256': hashlib.sha256(extract(a.unit.read_text()).encode()).hexdigest(), 'kind': 'isolated low-register retention experiment'}, indent=2) + '\n')
    print(a.output/'experiment.cpp')

if __name__ == '__main__':
    main()
