import os, shutil, subprocess, sys, tempfile
here = os.path.dirname(os.path.abspath(__file__))
tool = os.path.join(here, "..", "tools", "ps2run_3ds_patch.py")
FILES = {
    'runtime/include/ps2/vu_ops.hpp': 'PS2_VU_INLINE float fin(std::uint32_t x) {\nPS2_VU_INLINE std::uint32_t fout(float f) {\nPS2_VU_INLINE float norm(float f) { return fin(fout(f)); }\nPS2_VU_INLINE v4u vfix(v4u x) {\n    const v4i clamped = __builtin_elementwise_min((v4i)(x & 0x7FFFFFFFu), (v4i){0x7F7FFFFF, 0x7F7FFFFF, 0x7F7FFFFF, 0x7F7FFFFF});\n}\n    if (__builtin_reduce_or((v4i)(x + 0x01000000u > 0x02000000u))) {\n\nPS2_VU_INLINE std::uint8_t *qword(std::uint8_t *mem, std::uint32_t index) { return mem + (index & 1023u) * 16u; }\n',
    'runtime/src/iop/spu2.cpp': '        level = std::clamp(level + delta, 0, 0x7FFF);\n        sample = std::clamp(sample, -32768, 32767);\nstd::int16_t clamp16(std::int32_t v) { return static_cast<std::int16_t>(std::clamp(v, -32768, 32767)); }\nenum Phase { kOff, kAttack };\n#include "ps2/state.hpp"\nvoid tick() {\n',
    'runtime/src/iop/kernel.hpp': 'enum WaitKind { kWaitNone, kWaitSleep };\n',
    'runtime/src/system.cpp': '    std::ifstream file(path, std::ios::binary);\n    if (!file) return false;\n    const std::vector<std::uint8_t> data{std::istreambuf_iterator<char>(file), {}};\nbool load_state(const std::string &path) {\n    std::ifstream in(path, std::ios::binary);\n        iop::run(t - g_iop_synced);\n    next = std::min(next, now() + kEeClock / 2000);\n    g_base = std::max(now(), next);\n    g_given = 0;\nstd::uint64_t now() { return g_base + static_cast<std::uint64_t>(g_given - g_cpu.budget); }\nvoid idle(Cpu &c) {\n    std::uint64_t next = next_event_time();\nvoid run_due_events() {\n    sync_iop();\nvoid start_vu_thread() { gs::queue::vu().start(32u << 20); }\n                if (h.arg0 == 3) gif::vu_path3(payload, h.arg1);\n                else gs::queue::render().post(static_cast<gs::queue::Type>(h.type), h.arg0, h.arg1, payload, h.bytes);\n                break;\n            default: gs::queue::render().post(static_cast<gs::queue::Type>(h.type), h.arg0, h.arg1, payload, h.bytes); break;\n            }\n            in.pop(h);\n        }\n        const auto start = std::chrono::steady_clock::now();\n        in.wait_for_work(50);\n',
    'runtime/src/gs/gs.cpp': '#include "raster.hpp"\n    const std::uint32_t psm = static_cast<std::uint32_t>(tex0 >> 20) & 0x3Fu;\n    const std::uint32_t cpsm = static_cast<std::uint32_t>(tex0 >> 51) & 0xFu;\n    const std::uint32_t csm = static_cast<std::uint32_t>(tex0 >> 55) & 1u;\nvoid feed_image(const std::uint8_t *data, std::size_t bytes) {\n        if (cpu_raster()) raster::draw(s, s.queue, needed);\n        note_upload(s, value);\n        s.trxdir = value;\n        g_odd_field = !g_odd_field;\n        raise(8);\n    if ((g_state.trxdir & 3u) == 0) {\n        g_up_bytes += bytes;\n        for (std::size_t i = 0; i < bytes; ++i) g_up_nz += data[i] != 0;\n    }\nstd::uint8_t g_carry[4];\nstd::size_t g_carried = 0;\n    const std::uint32_t bits = bits_per_pixel(psm);\n    if (bits == 24) {\n        for (std::size_t i = 0; i < bytes && g_transfer.active; ++i) {\nvoid kick(bool draw) {\n    State &s = g_state;\n    const std::uint32_t prim = static_cast<std::uint32_t>(s.prim) & 7u;\n    const int needed = prim == 0 ? 1 : (prim == 1 || prim == 2 || prim == 6) ? 2 : prim == 7 ? 1 : 3;\n    if (s.count < needed) return;\n    if (draw) {\nvoid write_image(const std::uint8_t *data, std::uint32_t qwords) { feed_image(data, static_cast<std::size_t>(qwords) * 16); }\n        if (queue::active()) queue::post(queue::kVblank, 0, 0, nullptr, 0);\n#ifdef PS2_GPU\nvoid start_thread() { queue::render().start(32u << 20); }\nstd::uint64_t primitives_drawn() { return g_primitives; }\nvoid serve_state(const void *payload);\n\nstd::uint32_t g_setup_generation = 0;\nvoid vertex(std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint8_t f, bool draw) {\n    State &s = g_state;\n',
    'runtime/include/ps2/system.hpp': '#include "cpu.hpp"\n\n#include <cstdint>\n#include <functional>\nvoid start_vu_thread();\n',
    'runtime/src/vu_micro.cpp': 'void run_entry(int unit, std::uint32_t pc) {\nvoid xgkick(Unit &u, std::uint32_t address) {\n    PS2_PROF(ps2::prof::Gif);\n    std::uint8_t buffer[16 * 1024 + 16];\n            if (jit && jit != fn && ++sampled % 32 == 0) check_jit(fn, jit);\n            else fn(g_vu1, g_vu1_mem);\n            return;\n',
    'runtime/src/ipu/ipu.cpp': '#include "ps2/state.hpp"\nvoid step() {\nstruct DctLookup {\n    unsigned bits = 0;\n    std::vector<DctEntry> entries;\n};\n    for (const Dct &e : table) l.bits = std::max<unsigned>(l.bits, e.length);\n    l.entries.resize(std::size_t{1} << l.bits);\n    for (const Dct &e : table) {\n        const unsigned shift = l.bits - e.length;\n        for (std::uint32_t i = 0; i < (1u << shift); ++i) l.entries[(static_cast<std::uint32_t>(e.code) << shift) | i] = {e.length, e.run, e.level};\n    }\n    std::uint64_t v = 0;\n    const std::size_t byte = bit >> 3;\n    for (std::size_t i = 0; i < 8; ++i) v = (v << 8) | (byte + i < g_in.size() ? g_in[byte + i] : 0u);\n    const DctEntry &e = l.entries[peek_soft(l.bits, avail)];\nvoid decode_block(std::int16_t *out, int channel, bool intra, std::uint32_t qsc) {\n    int coef[64] = {};\n    std::memcpy(raw + 256, blocks[4], 128);\n    std::memcpy(raw + 320, blocks[5], 128);\n}\n    case kBdec: {\n        const bool ok = atomic([&] {\n    case kCsc:\n        while (g_count) {\n            std::uint8_t raw[384];\n            commit();\n            --g_count;\n        }\n        return true;\n    case kSetth:\nvoid drain() {\n    while (g_out_pos + 16 <= g_out.size() && dmac::ipu_push(g_out.data() + g_out_pos)) {\n        g_out_pos += 16;\n        ++g_out_qwords;\n    }\n',
    'recomp/vurecomp.cpp': 'constexpr int kAcc = 32;\n    const bool vector = kind == Add || kind == Sub || kind == Mul || kind == Madd || kind == Msub || kind == Max || kind == Min || kind == Abs || kind == Ftoi || kind == Itof;\n    const bool arithmetic = kind == Add || kind == Sub || kind == Mul || kind == Madd || kind == Msub;\n        case Itof: e = std::format("fp::from_fixed(r.vf[{}].sw[{}], {})", fs, i, frac); break;\n    const fs::path in_dir = argv[1], out_dir = argv[2];\n',
    'runtime/src/dmac.cpp': '            c.chcr = (c.chcr & ~0x30u) | (std::min(asp + 1, 2u) << 4);\n    const bool wrap = mfifo_vif1();\n    for (std::uint32_t i = 0; i < qwc; ++i) {\n        const std::uint32_t at = wrap ? ring(c.madr + i * 16) : c.madr + i * 16;\n        std::memcpy(target(at), g_spr + ((c.sadr + i * 16) & (kScratchpadSize - 1)), 16);\n    }\n    c.sadr = (c.sadr + qwc * 16) & (kScratchpadSize - 1);\nvoid reset() {\n    for (Channel &c : g_ch) c = Channel{};\nvoid start(int ch) {\n    Channel &c = g_ch[ch];\n    c.paused = false;\n',
    'runtime/include/ps2/memory.hpp': 'extern std::uint8_t g_ram[];\ninline void st32(std::uint32_t a, std::uint32_t v) {\n    if (a - g_swlo < g_swlen) [[unlikely]] store_watch_hit(a, 4, &v);\ninline void st64(std::uint32_t a, std::uint64_t v) {\n    if (a + 4u - g_swlo < g_swlen + 4u) [[unlikely]] store_watch_hit(a, 8, &v);\ninline void st128(std::uint32_t a, const Reg128 &v) {\n    if (a + 12u - g_swlo < g_swlen + 12u) [[unlikely]] store_watch_hit(a, 16, &v);\n    if ((a >> 14) == (kScratchpadBase >> 14)) return reinterpret_cast<T *>(g_spr + (a & (kScratchpadSize - 1)));\n    return nullptr;\nstd::uint8_t slow_ld8(std::uint32_t a);\nstd::uint16_t slow_ld16(std::uint32_t a);\nstd::uint32_t slow_ld32(std::uint32_t a);\nstd::uint64_t slow_ld64(std::uint32_t a);\nvoid slow_ld128(std::uint32_t a, Reg128 &out);\nvoid slow_st8(std::uint32_t a, std::uint8_t v);\nvoid slow_st16(std::uint32_t a, std::uint16_t v);\nvoid slow_st32(std::uint32_t a, std::uint32_t v);\nvoid slow_st64(std::uint32_t a, std::uint64_t v);\nvoid slow_st128(std::uint32_t a, const Reg128 &v);\n',
    'runtime/src/memory.cpp': 'alignas(64) std::uint8_t g_ram[kRamSize + 64];\n#include "hw.hpp"\n#include "vu_micro.hpp"\nstd::uint32_t slow_ld32(std::uint32_t a) { return read_slow<std::uint32_t>(a); }\nstd::uint64_t slow_ld64(std::uint32_t a) { return read_slow<std::uint64_t>(a); }\nvoid slow_st32(std::uint32_t a, std::uint32_t v) { write_slow(a, v); }\n',
    'runtime/src/gs/gs.hpp': 'void write_image(const std::uint8_t *data, std::uint32_t qwords);\nvoid render_reset();\n\nstd::uint64_t read_privileged',
    'runtime/src/gs/gif.cpp': 'template <typename Sink>\nvoid walk(Path &p, const std::uint8_t *data, std::uint32_t qwc, Sink &sink) {\n        switch (p.flg) {\n        case 0:\n            sink.packed(static_cast<std::uint32_t>(p.regs >> (p.reg * 4)) & 0xFu, q);\n        ++g_tags[path][p.flg];\n        gs::g_state.q = 1.0f;\nstd::vector<std::uint8_t> g_pend3;\nbool g_released = false;\nstd::uint32_t packet_qwords(const std::vector<std::uint8_t> &q) {\n    const std::uint32_t total = static_cast<std::uint32_t>(q.size() / 16);\n    std::uint32_t at = 0;\n    while (at < total) {\n        std::uint64_t tag;\n        std::memcpy(&tag, q.data() + static_cast<std::size_t>(at) * 16, 8);\n    while (!g_pend3.empty()) {\n        const std::uint32_t n = everything ? static_cast<std::uint32_t>(g_pend3.size() / 16) : packet_qwords(g_pend3);\n        if (!n) break;\n        std::vector<std::uint8_t> chunk(g_pend3.begin(), g_pend3.begin() + static_cast<std::size_t>(n) * 16);\n        g_pend3.erase(g_pend3.begin(), g_pend3.begin() + static_cast<std::ptrdiff_t>(n) * 16);\n        transfer_direct(3, chunk.data(), n);\n        if (!everything) break;\n    }\n}\n    if (path == 3 && (mask_up() || !g_pend3.empty())) {\n        g_pend3.insert(g_pend3.end(), data, data + static_cast<std::size_t>(qwc) * 16);\n    if (gs::queue::role() != gs::queue::Role::Vu) scan(g_scan[path], data, qwc);\n    gs::queue::post(gs::queue::kGif, static_cast<std::uint32_t>(path), qwc, data, qwc * 16);\n',
    'runtime/src/vif.cpp': '#include "ps2/state.hpp"\nvoid run_vu(int unit, std::uint32_t address) {\n    Unit &u = state(unit);\n        if (shadow(unit)) return;\n        vu::set_tops(u.top, u.itop);\n    std::uint32_t cycle = 0;\n    for (std::uint32_t n = 0; n < num; ++n) {\n        std::uint32_t value[4];\n            if (v2_keep()) {\n                std::uint32_t old[4];\n            } else if (v2_zero()) {\n                value[2] = value[3] = 0;\n            } else {\n                value[2] = value[0];\n                value[3] = value[1];\n            }\n        } else if constexpr (Components == 3) {\nvoid unpack(int unit, std::uint32_t cmd, const std::uint32_t *data) {\n    Unit &u = state(unit);\n    const bool dry = shadow(unit);\n    if (dry && u.mode != 2) {\n        kFastUnpack[vl][vn][usn ? 1 : 0](mem, wrap - 1, addr, wl, cl - wl, reinterpret_cast<const std::uint8_t *>(data), num);\nstd::uint32_t data_words(int unit, std::uint32_t cmd) {\n    const Unit &u = state(unit);\nvoid execute(int unit, std::uint32_t cmd, const std::uint32_t *data) {\n    Unit &u = state(unit);\n    if (!on_vu_thread()) ++g_commands[unit][op];\n    case 0x14: case 0x15: run_vu(unit, imm * 8u); break;\n    case 0x17: run_vu(unit, shadow(unit) ? 0u : vu::tpc(unit)); break;\n        if (shadow(unit)) break;\n        std::uint8_t *code = unit ? g_vu1_code : g_vu0_code;\n        const std::uint32_t count = num ? num : 256u;\n        bool changed = false;\n        if (shadow(unit)) gif::scan_signals(2, reinterpret_cast<const std::uint8_t *>(data), imm ? imm : 0x10000u);\n        if ((op & 0x60u) == 0x60u) unpack(unit, cmd, data);\n        if (!on_vu_thread()) hw::intc_raise(4u + static_cast<unsigned>(unit));\nstd::size_t run(int unit, const std::uint32_t *words, std::size_t count) {\n    Unit &u = state(unit);\n        const std::uint32_t cmd = words[pos];\n        const std::size_t start = pos + 1;\n        const std::uint32_t need = data_words(unit, cmd);\n        if (start + need > count) break;\n        execute(unit, cmd, words + start);\nvoid process(int unit) {\n    Unit &u = state(unit);\n    const std::size_t pos = run(unit, u.pending.data(), u.pending.size());\nvoid transfer(int unit, const std::uint8_t *data, std::uint32_t qwc) {\n    if (shadow(unit)) gs::queue::vu().post(gs::queue::kVif1, qwc, 0, data, qwc * 16);\n    Unit &u = state(unit);\n        const std::size_t pos = run(unit, words, count);\n    process(unit);\n}\n                process(unit);\n',
    'runtime/src/kernel.cpp': 'void service(Cpu &c) {\n    if (g_in_handler) return;\n',
    'runtime/src/gs/queue.hpp': 'enum Type : std::uint32_t { kWrap, kGif, kGifReset, kReset, kVertexReset, kVblank, kPresent, kDownload, kVif1, kVif1Write, kVuReset, kState, kVuFence, kDownloadAsync, kPath3Flush, kPath3ChainEnd };\n    std::uint64_t capacity_ = 0;\n    alignas(64) std::atomic<std::uint64_t> head_{0};\n    alignas(64) std::atomic<std::uint64_t> tail_{0};\nbool active();\nbool vu_active();\n',
    'runtime/src/gs/queue.cpp': '#include "queue.hpp"\n\n#include <algorithm>\n#include <climits>\n#include <cmath>\n#include <cstring>\n\n#ifdef __EMSCRIPTEN__\n#include <emscripten.h>\n#include <emscripten/threading.h>\n#elif defined(__linux__)\n#include <ctime>\n#include <linux/futex.h>\n#include <sys/syscall.h>\n#include <unistd.h>\n#else\n#include <chrono>\n#include <thread>\n#endif\n\nnamespace ps2::gs::queue {\n\nnamespace {\n\nconstexpr std::uint64_t align16(std::uint64_t n) { return (n + 15) & ~15ull; }\n\nRing g_render, g_vu;\nthread_local Role g_role = Role::Emulation;\n\n#if defined(__linux__) && !defined(__EMSCRIPTEN__)\nvoid futex_wait(std::atomic<std::uint32_t> &word, std::uint32_t seen, int timeout_ms) {\n    timespec limit{timeout_ms / 1000, static_cast<long>(timeout_ms % 1000) * 1000000L};\n    syscall(SYS_futex, reinterpret_cast<std::uint32_t *>(&word), FUTEX_WAIT_PRIVATE, seen, timeout_ms < 0 ? nullptr : &limit, nullptr, 0);\n}\n\nvoid futex_wake(std::atomic<std::uint32_t> &word) { syscall(SYS_futex, reinterpret_cast<std::uint32_t *>(&word), FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0); }\n#endif\n\n}\n\n#ifdef __EMSCRIPTEN__\nEM_ASYNC_JS(void, ps2_wait_async, (std::atomic<std::uint32_t> *word, std::uint32_t seen, int timeout_ms), {\n    const result = Atomics.waitAsync(HEAP32, word >> 2, seen, timeout_ms);\n    if (result.async) await result.value;\n});\n#endif\n\nvoid wait(std::atomic<std::uint32_t> &value, std::uint32_t seen) {\n#ifdef __EMSCRIPTEN__\n    emscripten_futex_wait(&value, seen, INFINITY);\n#elif defined(__linux__)\n    futex_wait(value, seen, -1);\n#else\n    value.wait(seen);\n#endif\n}\n\nvoid notify(std::atomic<std::uint32_t> &value) {\n#ifdef __EMSCRIPTEN__\n    emscripten_futex_wake(&value, INT_MAX);\n#elif defined(__linux__)\n    futex_wake(value);\n#else\n    value.notify_all();\n#endif\n}\n\nRing &render() { return g_render; }\nRing &vu() { return g_vu; }\nRole &role() { return g_role; }\n\nRing &outbox() {\n    if (g_role == Role::Emulation && g_vu.active()) return g_vu;\n    return g_render;\n}\n\nbool active() { return g_render.active(); }\nbool vu_active() { return g_vu.active(); }\n\nvoid post(Type type, std::uint32_t arg0, std::uint32_t arg1, const void *data, std::uint32_t bytes) { outbox().post(type, arg0, arg1, data, bytes); }\n\nvoid Ring::start(std::size_t capacity) {\n    buffer_ = std::make_unique<std::uint8_t[]>(capacity);\n    capacity_ = capacity;\n}\n\nvoid Ring::post(Type type, std::uint32_t arg0, std::uint32_t arg1, const void *data, std::uint32_t bytes) {\n    const std::uint64_t need = sizeof(Header) + align16(bytes);\n    std::uint64_t head = head_.load(std::memory_order_relaxed);\n    for (;;) {\n        const std::uint64_t pos = head % capacity_;\n        const std::uint64_t pad = pos + need > capacity_ ? capacity_ - pos : 0;\n        const std::uint32_t seen = freed_.load(std::memory_order_acquire);\n        if (head + pad + need - tail_.load(std::memory_order_acquire) <= capacity_) {\n            if (pad) {\n                const Header wrap{kWrap, 0, 0, 0};\n                std::memcpy(buffer_.get() + pos, &wrap, sizeof wrap);\n                head += pad;\n            }\n            break;\n        }\n        producer_waiting_.store(1);\n        if (head + pad + need - tail_.load() > capacity_) wait(freed_, seen);\n    }\n    std::uint8_t *at = buffer_.get() + head % capacity_;\n    const Header h{type, bytes, arg0, arg1};\n    std::memcpy(at, &h, sizeof h);\n    if (bytes) std::memcpy(at + sizeof h, data, bytes);\n    head_.store(head + need, std::memory_order_release);\n    posted_.fetch_add(1);\n    if (consumer_waiting_.exchange(0)) notify(posted_);\n}\n\nbool Ring::peek(Header &header, const std::uint8_t *&payload) {\n    std::uint64_t tail = tail_.load(std::memory_order_relaxed);\n    const std::uint64_t head = head_.load(std::memory_order_acquire);\n    while (tail != head) {\n        const std::uint64_t pos = tail % capacity_;\n        std::memcpy(&header, buffer_.get() + pos, sizeof header);\n        if (header.type != kWrap) {\n            payload = buffer_.get() + pos + sizeof header;\n            return true;\n        }\n        tail += capacity_ - pos;\n        tail_.store(tail, std::memory_order_release);\n    }\n    return false;\n}\n\nvoid Ring::pop(const Header &header) {\n    tail_.store(tail_.load(std::memory_order_relaxed) + sizeof(Header) + align16(header.bytes), std::memory_order_release);\n    freed_.fetch_add(1);\n    if (producer_waiting_.exchange(0)) notify(freed_);\n}\n\nvoid Ring::wake() {\n    posted_.fetch_add(1);\n    if (consumer_waiting_.exchange(0)) notify(posted_);\n}\n\nvoid Ring::wait_for_work(int timeout_ms) {\n    const std::uint32_t seen = posted_.load();\n    consumer_waiting_.store(1);\n    if (head_.load() != tail_.load()) {\n        consumer_waiting_.store(0);\n        return;\n    }\n#ifdef __EMSCRIPTEN__\n    if (emscripten_is_main_browser_thread()) ps2_wait_async(&posted_, seen, timeout_ms);\n    else emscripten_futex_wait(&posted_, seen, timeout_ms);\n#elif defined(__linux__)\n    futex_wait(posted_, seen, timeout_ms);\n#else\n    (void)seen;\n    std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeout_ms, 1)));\n#endif\n}\n\n}\n',
    'runtime/include/ps2/fpu.hpp': 'inline std::uint32_t add(std::uint32_t a, std::uint32_t b) {\n    a = flush(a);\n    return pack(sign, e, static_cast<std::uint32_t>(m));\n}\n\ninline double to_double(std::uint32_t x) {\ninline Result div(std::uint32_t a, std::uint32_t b) {\n    a = flush(a);\n    b = flush(b);\n    const std::uint32_t sign = (a ^ b) & kSign;\n    if (!(b & kAbs)) return {sign | kAbs, true};\n    if (!(a & kAbs)) return {sign, false};\n    return {from_double(to_double(a) / to_double(b)), false};\n}\n\ninline std::uint32_t sqrt(std::uint32_t a) {\n    a = flush(a) & kAbs;\n    if (!a) return 0;\n    return from_double(std::sqrt(to_double(a)));\n}\n    if (!(a & kAbs)) return {a & kSign, false};\n    return {from_double(to_double(a) / std::sqrt(to_double(b))), false};\ninline std::uint32_t to_fixed(std::uint32_t x, int fraction) {\n    const int e = static_cast<int>(exponent(x)) + fraction;\ninline std::uint32_t from_fixed(std::int32_t v, int fraction) {\n    return from_double(static_cast<double>(v) / static_cast<double>(1u << fraction));\n',
    'runtime/src/vu_micro.hpp': 'void reset();\n',
    'runtime/src/iop/cpu.cpp': '    static const bool probing = load_probes();\n    while (budget-- > 0) {\nvoid execute(Cpu &c, std::int64_t &budget) {\n        const std::uint32_t pc = c.pc;\n        if (probing) {\n            const std::uint32_t a = pc & (kRamSize - 1);\n            if (g_probes[a >> 5] & (1u << ((a >> 2) & 7u))) probe(c, pc);\n        }\n                kernel::syscall(c, pc);\n                break;\n        case 0x10:\n            if (rs == 0) r[rt] = rd == 12 ? c.status        r[0] = 0;\n        if (kernel::switch_pending()) return;\n    }\n}\n\n}\n',
    'runtime/src/dmac.hpp': '#include <cstdint>\nbool ipu_push(const std::uint8_t *qword);\n',
    'runtime/include/ps2/mmi.hpp': 'PS2_LANES(paddh, h, 8, static_cast<std::uint16_t>(s.h[i] + t.h[i]))\nPS2_LANES(psubh, h, 8, static_cast<std::uint16_t>(s.h[i] - t.h[i]))\nPS2_LANES(paddb, b, 16, static_cast<std::uint8_t>(s.b[i] + t.b[i]))\nPS2_LANES(psubb, b, 16, static_cast<std::uint8_t>(s.b[i] - t.b[i]))\nPS2_LANES(paddsh, sh, 8, sat_add(s.sh[i], t.sh[i]))\nPS2_LANES(psubsh, sh, 8, sat_sub(s.sh[i], t.sh[i]))\nPS2_LANES(paddsb, sb, 16, sat_add(s.sb[i], t.sb[i]))\nPS2_LANES(psubsb, sb, 16, sat_sub(s.sb[i], t.sb[i]))\nPS2_LANES(padduh, h, 8, sat_add(s.h[i], t.h[i]))\nPS2_LANES(psubuh, h, 8, sat_sub(s.h[i], t.h[i]))\nPS2_LANES(paddub, b, 16, sat_add(s.b[i], t.b[i]))\nPS2_LANES(psubub, b, 16, sat_sub(s.b[i], t.b[i]))\nPS2_LANES(pcgth, h, 8, static_cast<std::uint16_t>(s.sh[i] > t.sh[i] ? 0xFFFFu : 0u))\nPS2_LANES(pcgtb, b, 16, static_cast<std::uint8_t>(s.sb[i] > t.sb[i] ? 0xFFu : 0u))\nPS2_LANES(pminh, sh, 8, s.sh[i] < t.sh[i] ? s.sh[i] : t.sh[i])\nPS2_LANES(pmaxh, sh, 8, s.sh[i] > t.sh[i] ? s.sh[i] : t.sh[i])\nPS2_LANES(pextlb, b, 16, (i & 1) ? s.b[i >> 1] : t.b[i >> 1])\nPS2_LANES(pextub, b, 16, (i & 1) ? s.b[8 + (i >> 1)] : t.b[8 + (i >> 1)])\nPS2_LANES(ppacb, b, 16, i < 8 ? t.b[i * 2] : s.b[(i - 8) * 2])\nPS2_LANES(pcpyld, d, 2, i ? s.d[0] : t.d[0])\nPS2_LANES(pcpyud, d, 2, i ? t.d[1] : s.d[1])\ninline void psllh(Reg128 &d, const Reg128 &t, unsigned sa) { for (int i = 0; i < 8; ++i) d.h[i] = static_cast<std::uint16_t>(t.h[i] << (sa & 15)); }\ninline void psrlh(Reg128 &d, const Reg128 &t, unsigned sa) { for (int i = 0; i < 8; ++i) d.h[i] = static_cast<std::uint16_t>(t.h[i] >> (sa & 15)); }\ninline void psrah(Reg128 &d, const Reg128 &t, unsigned sa) { for (int i = 0; i < 8; ++i) d.sh[i] = static_cast<std::int16_t>(t.sh[i] >> (sa & 15)); }\ninline void qfsrv(Reg128 &d, const Reg128 &s, const Reg128 &t, std::uint32_t sa) {\n    std::uint8_t bytes[32];\n    __builtin_memcpy(bytes, &t, 16);\n    __builtin_memcpy(bytes + 16, &s, 16);\n    Reg128 r;\n    __builtin_memcpy(&r, bytes + (sa & 15), 16);\n    d = r;\n}\n',
    'recomp/codegen.hpp': '    std::set<std::uint32_t> probes;\n',
    'recomp/codegen.cpp': '            if (group_start(a)) {\n                emit_group(a);\n                falls = skip = false;\n                continue;\n            }\n',
    'recomp/main.cpp': '    std::string elf_path = argv[1], entries_path, out, probes_path, modules_dir, variants_path, vanilla_path;\n        else if (arg == "--probes" && i + 1 < argc) probes_path = argv[++i];\n            generate(translations, options);\n',
    'runtime/include/ps2/vu.hpp': '#include <cstdint>\n\nnamespace ps2::vu {\ninline void add(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, unsigned ft) {\ninline void max(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, unsigned ft) {\ninline void adda(Vu0Regs &v, unsigned dest, unsigned fs, unsigned ft) {\ninline void opmula(Vu0Regs &v, unsigned fs, unsigned ft) {\n',
}
# The I/O processor thread edits (PERF_EDITS in the tool) need these lines in the fixture.
FILES['runtime/include/ps2/system.hpp'] = FILES.get('runtime/include/ps2/system.hpp', "") + 'void sync_iop();\n'
FILES['runtime/src/system.cpp'] = FILES.get('runtime/src/system.cpp', "") + '    iop::sio2::flush_cards();\n    iop::vblank(true);\n    schedule(hw::kEventVblankEnd + 100, now() + hblank_cycles() * 22u, [] { iop::vblank(false); });\n    if (id == hw::kEventVblankEnd + 100) return [] { iop::vblank(false); };\n    iop::state(s);\nvoid sync_iop() {\n    const std::uint64_t t = now();\n    if (t > g_iop_synced) {\nvoid request_iop_sync() {\n    if (g_cpu.budget > 64) set_budget(g_cpu, 64);\n}\n'
FILES['runtime/src/iop/sif.hpp'] = FILES.get('runtime/src/iop/sif.hpp', "") + 'void pump();\nvoid dump();\n'
FILES['runtime/src/iop/sif.cpp'] = FILES.get('runtime/src/iop/sif.cpp', "") + 'std::uint64_t g_ee_dma_done = 0;\n#include "ps2/state.hpp"\n    g_outgoing.emplace_back(bytes, bytes + size);\n    pump();\n}\n    g_binds[server] = PendingBind{r, now() + kEeClock * 4};\nstd::uint32_t get_reg(std::uint32_t reg) {\n    switch (reg) {\nvoid set_reg(std::uint32_t reg, std::uint32_t value) {\n    if (reg == 1) g_mainaddr = value;\nstd::uint32_t set_dma(std::uint32_t transfers, std::uint32_t count) {\n    if (!count) count = 1;\nvoid packet_processed() {\n    g_processed = true;\n    pump();\n}\n\nvoid pump() {\n    const std::uint64_t t = now();\n'

# So do the edits that hand translated I/O processor code to the interpreter.
FILES['runtime/src/iop/cpu.cpp'] = FILES['runtime/src/iop/cpu.cpp'].replace('    while (budget-- > 0) {\n', '    while (budget-- > 0) {\n        const std::uint32_t pc = c.pc;\n', 1)
FILES['runtime/src/iop/iop.hpp'] = 'void set_trace(bool modules, bool hle);\n'
FILES['runtime/src/iop/kernel.cpp'] = '    link_imports(base, total);\n\n    const int id = g_next_module++;\n    hw::reset();\n    kernel::boot();\n    s.bytes(g_ram, kRamSize);\n    kernel::state(s);\n'

# So do the vector program speed edits, which change text the earlier edits leave in place.
with open(tool) as f:
    source = f.read()
perf = {}
exec(source[source.index("EDITS = {"):source.index("def fail(msg):")], perf)
perf_inserted = {}
for key, edits in perf["EDITS"].items():
    perf_inserted.setdefault(key.split("#")[0], []).append(edits)
for rel in ("recomp/vurecomp.cpp", "runtime/include/ps2/fpu.hpp", "runtime/include/ps2/vu_ops.hpp"):
    for old, _ in perf["PERF_EDITS"][rel]:
        inserted = any(old in new for edits in perf_inserted.get(rel, ()) for _, new in edits)
        if old not in FILES.get(rel, "") and not inserted:
            FILES[rel] = FILES.get(rel, "") + old
# And so does the cheaper path for a skipped picture's transfers. These edits are the last ones for their files.
for rel, first in (("runtime/src/gs/queue.hpp", 1), ("runtime/src/vif.hpp", 0), ("runtime/src/gs/gif.cpp", 0),
                   ("runtime/src/system.cpp", -1)):
    for old, _ in perf["PERF_EDITS"][rel][first:]:
        if old not in FILES.get(rel, ""):
            FILES[rel] = FILES.get(rel, "") + old
FILES["runtime/src/vif.cpp"] = FILES["runtime/src/vif.cpp"].replace('#include "ps2/state.hpp"\n', '#include "ps2/state.hpp"\n#include "ps2/system.hpp"\n', 1)
FILES["runtime/src/vif.cpp"] = FILES["runtime/src/vif.cpp"].replace(
    'std::size_t run(int unit, const std::uint32_t *words, std::size_t count) {\n    Unit &u = state(unit);\n',
    'std::size_t run(int unit, const std::uint32_t *words, std::size_t count) {\n    Unit &u = state(unit);\n'
    '    std::size_t pos = 0;\n    while (pos < count && !u.stalled) {\n')
FILES["runtime/src/vif.cpp"] = FILES["runtime/src/vif.cpp"].replace(
    '        execute(unit, cmd, words + start);\nvoid process(int unit) {',
    '        execute(unit, cmd, words + start);\n        pos = start + need;\n'
    '    }\n    return pos;\n}\nvoid process(int unit) {')
FILES["runtime/src/vif.cpp"] = FILES["runtime/src/vif.cpp"].replace(
    '    Unit &u = state(unit);\n        const std::size_t pos = run(unit, words, count);\n    process(unit);',
    '    Unit &u = state(unit);\n    if (u.pending.empty() && !u.stalled) {\n'
    '        const auto *words = reinterpret_cast<const std::uint32_t *>(data);\n'
    '        const std::size_t count = static_cast<std::size_t>(qwc) * 4;\n'
    '        const std::size_t pos = run(unit, words, count);\n'
    '        if (pos < count) u.pending.assign(words + pos, words + count);\n'
    '        return;\n    }\n    process(unit);')
FILES["runtime/src/vif.cpp"] += '    bool dbf = false, stalled = false;\n    std::vector<std::uint32_t> pending;\n};\nbool stalled(int unit) { return state(unit).stalled; }\n'
FILES["runtime/src/dmac.hpp"] += 'bool ipu_pull(std::uint8_t *qword);\n'
FILES["runtime/src/dmac.cpp"] += 'bool ipu_push(const std::uint8_t *qword) {\n'
FILES["runtime/src/system.cpp"] += ('void run_vu_thread() {\n    gs::stats();\n'
                                   '            case gs::queue::kVuFence:\n                g_vu_fence.fetch_add(1);\n')
FILES["runtime/src/gs/gif.cpp"] += '    GsSink sink{path};\n    walk(g_paths[path], data, qwc, sink);\n'


def run(folder):
    return subprocess.run([sys.executable, tool, folder], capture_output=True, text=True)
with tempfile.TemporaryDirectory() as d:
    for rel, text in FILES.items():
        os.makedirs(os.path.dirname(os.path.join(d, rel)), exist_ok=True)
        with open(os.path.join(d, rel), "w") as f:
            f.write(text)
    # An unknown source is refused, and nothing is changed in any file.
    dmac = os.path.join(d, "runtime/src/dmac.cpp")
    with open(dmac, "w") as f:
        f.write("int main() {}\n")
    r = run(d)
    assert "NOT APPLIED" in r.stdout and r.returncode != 0, r.stdout + r.stderr
    with open(os.path.join(d, "runtime/src/iop/spu2.cpp")) as f:
        assert f.read() == FILES["runtime/src/iop/spu2.cpp"]
    with open(dmac, "w") as f:
        f.write(FILES["runtime/src/dmac.cpp"])
    r = run(d)
    assert "patched" in r.stdout and "NOT" not in r.stdout, r.stdout + r.stderr
    def read(rel):
        with open(os.path.join(d, rel)) as f:
            return f.read()
    # Clang keeps its built-ins; only other compilers take the new path.
    vu = read("runtime/include/ps2/vu_ops.hpp")
    assert "#if defined(__clang__)\n    return __builtin_elementwise_min(a, b);" in vu and "return a < b ? a : b;" in vu
    assert "const bool any_large = __builtin_reduce_or" in vu and "if (any_large) {" in vu
    assert "#if defined(__3DS__) && defined(PS2_VU_FAST)" in vu and "__builtin_assume_aligned(mem, 16)" in vu
    # Vector programs: registers and memory do not overlap, unseen flags are left out, conversions stay in the FPU.
    gen = read("recomp/vurecomp.cpp")
    assert "Vu0Regs &__restrict r, std::uint8_t *__restrict mem" in gen and "flags_seen.count(pc)" in gen
    assert "static_cast<float>(1 << fraction)" in read("runtime/include/ps2/fpu.hpp")
    assert "field(3);" in vu
    # Desktop builds keep PS2 RAM in the program; the 3DS gets a fixed address.
    mem = read("runtime/include/ps2/memory.hpp")
    assert "#ifdef __3DS__" in mem and "kRamAddress3ds = 0x09C00000u" in mem and "#else\nextern std::uint8_t g_ram[];\n#endif" in mem
    assert "#ifndef __3DS__\nalignas(64) std::uint8_t g_ram[kRamSize + 64];\n#endif" in read("runtime/src/memory.cpp")
    # The boot program is read in one sized read, not byte by byte.
    sysc = read("runtime/src/system.cpp")
    assert "istreambuf_iterator" not in sysc and "std::fread(data.data(), 1, data.size(), file)" in sysc
    assert "class FileReader : public state::Stream" in sysc and "FileReader r(file);" in sysc
    assert "std::clamp<std::int32_t>(level + delta" in read("runtime/src/iop/spu2.cpp")
    assert "std::min<std::uint32_t>(asp + 1, 2u)" in read("runtime/src/dmac.cpp")
    assert "enum WaitKind : int {" in read("runtime/src/iop/kernel.hpp") and "enum Phase : int {" in read("runtime/src/iop/spu2.cpp")
    # The scalar compiler mode is off unless asked for, and the accurate float path stays for desktop builds.
    vur = read("recomp/vurecomp.cpp")
    assert "bool g_scalar = false;" in vur and "const bool vector = !g_scalar && (" in vur and "const bool arithmetic = !g_scalar && (" in vur
    assert '"--scalar"' in vur and "fixed_to_float(r.vf" in vur and "#ifdef PS2_VU_FAST" in vu
    # The GPU path only exists in a 3DS build; the desktop draw call is still there.
    gs = read("runtime/src/gs/gs.cpp")
    assert "#ifdef PS2_PICA\n#include \"pica.hpp\"\n#endif" in gs and "pica::draw(s, g_vertex_serial, clamp);" in gs
    assert "if (cpu_raster()) raster::draw(s, s.queue, needed);" in gs
    assert "#else\n#define PS2_PROF(counter)\n#endif" in read("runtime/include/ps2/system.hpp")
    assert "PS2_PROF(ps2::prof::Upload);" in gs
    assert "pica::note_transfer(s, value)" in gs and "pica::note_clut()" in gs and "pica::vblank(static_cast<std::uint32_t>(ps2::frame_count()), display_regs(), pica::next_picture())" in gs
    # Speed work: the PATH3 queue no longer shuffles its bytes, uploads go page by page, and floats use the FPU only on the 3DS.
    gif = read("runtime/src/gs/gif.cpp")
    assert "g_pend3_head" in gif and "chunk(g_pend3" not in gif and "gs::packed_run(" in gif
    assert "image_fast(psm, data, bytes)" in gs and "void packed_run(" in gs and "void serve(" in gs
    fp = read("runtime/include/ps2/fpu.hpp")
    assert "#ifdef __3DS__\n// ps2run_3ds_patch.py. The functions below" in fp and "#else\ninline std::uint32_t add(std::uint32_t a, std::uint32_t b) {\n    a = flush(a);" in fp
    queue = read("runtime/src/gs/queue.cpp")
    assert "#elif defined(__3DS__)\n    while (value.load() == seen) svcSleepThread(100000);" in queue
    assert "std::atomic<std::uint32_t> head_{0};" in read("runtime/src/gs/queue.hpp")
    assert "head % capacity_" not in queue and "tail % capacity_" not in queue
    vif = read("runtime/src/vif.cpp")
    assert "data_words(const Unit &u" in vif and "std::memcpy(mem + addr * 16, data, first * 16);" in vif
    # The I/O processor can run on its own core, and the old way stays for other builds.
    system_cpp = read("runtime/src/system.cpp")
    assert "iop_advance(t - g_iop_synced);" in system_cpp and "    iop::run(t - g_iop_synced);" in system_cpp
    assert "IopGuard guard;" in read("runtime/src/iop/sif.cpp") and "inline void iop_wait() {}" in read("runtime/include/ps2/system.hpp")
    assert "already patched" in run(d).stdout
    # Upgrade a tree that already has the original 3DS movie decoder.
    ipu = "runtime/src/ipu/ipu.cpp"
    ipu_edits = perf["PERF_EDITS"][ipu]
    old_reader, new_reader = next(edit for edit in ipu_edits if "bits_at(g_bit, 32)" in edit[0])
    old_prefix, new_prefix = next(edit for edit in ipu_edits if "const DctEntry &short_code" in edit[1])
    patched_ipu = read(ipu)
    assert new_prefix in patched_ipu
    before_prefix = patched_ipu.replace(new_prefix, old_prefix)
    assert new_reader in before_prefix
    with open(os.path.join(d, ipu), "w") as f:
        f.write(before_prefix.replace(new_reader, old_reader))
    upgraded = run(d)
    assert upgraded.returncode == 0 and upgraded.stdout.startswith("patched"), upgraded.stdout
    assert read(ipu) == patched_ipu
print("ok   3DS patch applies once, is idempotent, refuses unknown source and leaves desktop paths alone")
