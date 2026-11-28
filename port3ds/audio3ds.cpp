#include "audio3ds.hpp"

#include "ps2/system.hpp"

#include <3ds.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace audio3ds {

namespace {

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::size_t kRingFrames = 16384;       // power of 2 (~341 ms at 48 kHz)
constexpr std::size_t kRingMask = kRingFrames - 1;
constexpr std::size_t kMaxQueued = 9600;         // ~200 ms latency cap (drops excess during FAST/uncapped)

// NDSP streaming: wave buffers of one stretch step each, 12 x 480 stereo frames = 23 KB linear RAM.
constexpr std::size_t kHop = 480;                // 10 ms: one stretch step, one wave buffer
constexpr std::size_t kNumBufs = 12;
constexpr std::size_t kBufFrames = kHop;
constexpr std::size_t kTargetQueued = 6;         // ~60 ms kept in the DSP's queue
// Time stretch (WSOLA): when the game makes sound slower than real time (it runs below full speed), the sound is played
// slower at the same pitch instead of running dry. Each step takes the next 10 ms window from where the tempo says, moved by
// up to kSeek to where it best continues the last one, and cross-fades the two.
constexpr std::size_t kSeek = 240;               // +-5 ms
// The tempo goes from kMinTempo at kFloorAhead frames of sound waiting ahead of the read point to full speed at
// kTargetAhead. The floor is what a step needs (kSeek + 2 * kHop) plus room for the game's bursts of about 800 frames.
constexpr std::size_t kFloorAhead = 2000;
constexpr std::size_t kTargetAhead = 3840;       // 80 ms
constexpr std::size_t kPrebufferFrames = 3840;   // before starting from idle or after running dry
constexpr float kMinTempo = 0.35f;

// CSND fallback parameters (only used when sdmc:/3ds/dspfirm.cdc is absent).
constexpr std::size_t kCsndFrames = 4096;        // ~85.3 ms circular buffer per mono channel
constexpr std::size_t kCsndMask = kCsndFrames - 1;
constexpr int kCsndChLeft = 8;
constexpr int kCsndChRight = 9;

enum class Backend { None, Ndsp, Csnd };

Backend g_backend = Backend::None;
std::int16_t g_ring[kRingFrames * 2];
std::atomic<std::uint64_t> g_write{0};
std::atomic<std::uint64_t> g_read{0};
std::atomic<bool> g_stopping{false};

// For report(): what came in, what went out, and how often the DSP ran dry.
std::atomic<std::uint32_t> g_pushed{0}, g_dropped{0}, g_underruns{0};

// NDSP state (touched only on NDSP callback thread once running).
std::int16_t *g_ndsp_linear = nullptr;
ndspWaveBuf g_bufs[kNumBufs];
std::size_t g_next_buf = 0;
std::int32_t g_fade[kHop];        // rising cross-fade weight, Q15 (sin squared: it and its mirror add up to 1)
bool g_have_prev = false;         // a window is playing: its second half is faded out by the next step
std::uint64_t g_prev = 0;         // ring position of that window
double g_nominal = 0.0;           // where the next window starts before the search
float g_tempo = 1.0f;
float g_tempo_sum = 0.0f, g_tempo_min = 1.0f;   // for report()
std::uint32_t g_steps = 0;

// CSND fallback state (touched only on main thread in poll()).
std::int16_t *g_csnd_left = nullptr;
std::int16_t *g_csnd_right = nullptr;
bool g_csnd_started = false;
u64 g_csnd_start_tick = 0;
std::uint64_t g_csnd_written = 0;

inline std::int32_t at(std::uint64_t pos, int ch) { return g_ring[static_cast<std::size_t>(pos & kRingMask) * 2 + ch]; }

inline std::int16_t mix(std::int32_t out, std::int32_t in, std::int32_t f) {
    return static_cast<std::int16_t>((out * (32768 - f) + in * f) >> 15);
}

// One step of kHop frames into dst: 1 made, 0 not enough sound yet (wait), 2 ran dry (dst fades the last window out).
// Moves g_read up to the oldest frame still needed.
int stretch_step(std::int16_t *dst, std::uint64_t w, std::size_t active) {
    if (!g_have_prev) {
        const std::uint64_t p = g_read.load(std::memory_order_relaxed);
        if (w - p < kPrebufferFrames) return 0;
        for (std::size_t i = 0; i < kHop; ++i) {   // fade in
            dst[i * 2] = mix(0, at(p + i, 0), g_fade[i]);
            dst[i * 2 + 1] = mix(0, at(p + i, 1), g_fade[i]);
        }
        g_have_prev = true;
        g_prev = p;
        g_tempo = 1.0f;
        g_nominal = static_cast<double>(p) + kHop;
        g_read.store(p, std::memory_order_relaxed);
        return 1;
    }
    const std::uint64_t ref = g_prev + kHop;   // the playing window's second half, and what the next should resemble
    const double ahead = static_cast<double>(w) - g_nominal;
    if (ahead < static_cast<double>(kSeek + 2 * kHop)) {
        if (active > 1) return 0;   // the DSP still has sound: wait for more
        for (std::size_t i = 0; i < kHop; ++i) {
            dst[i * 2] = mix(at(ref + i, 0), 0, g_fade[i]);
            dst[i * 2 + 1] = mix(at(ref + i, 1), 0, g_fade[i]);
        }
        g_have_prev = false;
        g_read.store(std::min<std::uint64_t>(ref + kHop, w), std::memory_order_relaxed);
        g_underruns.fetch_add(1, std::memory_order_relaxed);
        return 2;
    }
    // The tempo follows how much sound waits: full speed above kTargetAhead, slower below, a little faster when far ahead.
    const double share = (ahead - static_cast<double>(kFloorAhead)) / static_cast<double>(kTargetAhead - kFloorAhead);
    float want = share >= 1.0 ? 1.0f : std::max(kMinTempo, kMinTempo + (1.0f - kMinTempo) * static_cast<float>(share));
    if (ahead > 2.0 * kTargetAhead) want = 1.02f;
    g_tempo += (want - g_tempo) * 0.15f;
    const bool at_full_speed = (want == 1.0f && std::fabs(g_tempo - 1.0f) < 0.005f);
    if (at_full_speed) g_tempo = 1.0f;

    // Fast path at 100% speed once aligned: copy straight through without WSOLA search or cross-fade.
    const std::int64_t base = static_cast<std::int64_t>(g_nominal);
    if (at_full_speed && base == static_cast<std::int64_t>(ref)) {
        const std::size_t start = static_cast<std::size_t>(ref & kRingMask);
        const std::size_t first = std::min(kHop, kRingFrames - start);
        std::memcpy(dst, &g_ring[start * 2], first * 4);
        if (first < kHop) std::memcpy(dst + first * 2, g_ring, (kHop - first) * 4);
        g_prev = ref;
        g_nominal = static_cast<double>(ref + kHop);
        const std::uint64_t lowest = (ref + kHop > kSeek) ? (ref + kHop - kSeek) : 0;
        g_read.store(lowest, std::memory_order_relaxed);
        g_tempo_sum += g_tempo;
        g_tempo_min = std::min(g_tempo_min, g_tempo);
        ++g_steps;
        return 1;
    }

    // The window start within +-kSeek of the nominal one that best continues ref (normalised cross-correlation of the
    // mono sums, pre-extracted into contiguous buffers with coarse-to-fine search).
    constexpr std::size_t kRefLen = kHop / 4;
    constexpr std::size_t kCandLen = kHop + 2 * kSeek;
    static std::int16_t ref_m[kRefLen];
    static std::int16_t cand_m[kCandLen];
    for (std::size_t k = 0; k < kRefLen; ++k)
        ref_m[k] = static_cast<std::int16_t>((at(ref + k * 4, 0) + at(ref + k * 4, 1)) >> 5);
    const std::uint64_t cand_base = static_cast<std::uint64_t>(base - static_cast<std::int64_t>(kSeek));
    for (std::size_t j = 0; j < kCandLen; ++j)
        cand_m[j] = static_cast<std::int16_t>((at(cand_base + j, 0) + at(cand_base + j, 1)) >> 5);

    auto score_at = [&](std::size_t idx) -> float {
        const std::int16_t *cp = cand_m + idx;
        std::int32_t xy = 0, yy = 0;
        for (std::size_t k = 0; k < kRefLen; ++k) {
            const std::int32_t x = ref_m[k], y = cp[k * 4];
            xy += x * y;
            yy += y * y;
        }
        return static_cast<float>(xy) / std::sqrt(static_cast<float>(yy) + 1.0f);
    };

    std::size_t best_idx = kSeek;
    float best_score = -1e30f;
    for (std::size_t idx = 0; idx <= 2 * kSeek; idx += 8) {
        const float score = score_at(idx);
        if (score > best_score) {
            best_score = score;
            best_idx = idx;
        }
    }
    const std::size_t lo = best_idx > 6 ? best_idx - 6 : 0;
    const std::size_t hi = std::min<std::size_t>(2 * kSeek, best_idx + 6);
    for (std::size_t idx = lo; idx <= hi; idx += 2) {
        if ((idx & 7u) == 0u) continue;
        const float score = score_at(idx);
        if (score > best_score) {
            best_score = score;
            best_idx = idx;
        }
    }
    const std::uint64_t pos = cand_base + best_idx;
    for (std::size_t i = 0; i < kHop; ++i) {
        dst[i * 2] = mix(at(ref + i, 0), at(pos + i, 0), g_fade[i]);
        dst[i * 2 + 1] = mix(at(ref + i, 1), at(pos + i, 1), g_fade[i]);
    }
    g_prev = pos;
    g_nominal = at_full_speed ? static_cast<double>(pos + kHop) : (g_nominal + static_cast<double>(g_tempo) * kHop);
    const double lowest = g_nominal - static_cast<double>(kSeek);
    g_read.store(std::min<std::uint64_t>(pos + kHop, static_cast<std::uint64_t>(lowest)), std::memory_order_relaxed);
    g_tempo_sum += g_tempo;
    g_tempo_min = std::min(g_tempo_min, g_tempo);
    ++g_steps;
    return 1;
}

void submit_ndsp_buf(ndspWaveBuf &buf, std::size_t frames) {
    buf.nsamples = static_cast<u32>(frames);
    DSP_FlushDataCache(buf.data_pcm16, static_cast<u32>(frames * 4));
    ndspChnWaveBufAdd(0, &buf);
}

void ndsp_callback(void *) {
    if (g_stopping.load(std::memory_order_acquire)) return;
    std::size_t active = 0;
    for (std::size_t i = 0; i < kNumBufs; ++i) {
        const u8 st = g_bufs[i].status;
        if (st == NDSP_WBUF_QUEUED || st == NDSP_WBUF_PLAYING) ++active;
    }
    const std::uint64_t w = g_write.load(std::memory_order_acquire);
    while (active < kTargetQueued) {
        ndspWaveBuf &buf = g_bufs[g_next_buf];
        if (buf.status != NDSP_WBUF_FREE && buf.status != NDSP_WBUF_DONE) break;
        const int made = stretch_step(buf.data_pcm16, w, active);
        if (!made) break;
        submit_ndsp_buf(buf, kHop);
        g_next_buf = (g_next_buf + 1) % kNumBufs;
        ++active;
        if (made == 2) break;
    }
}

void poll_csnd() {
    const std::uint64_t w = g_write.load(std::memory_order_acquire);
    std::uint64_t r = g_read.load(std::memory_order_relaxed);
    std::uint64_t avail = w - r;

    if (!g_csnd_started) {
        if (avail < kPrebufferFrames) return;
        std::memset(g_csnd_left, 0, kCsndFrames * sizeof(std::int16_t));
        std::memset(g_csnd_right, 0, kCsndFrames * sizeof(std::int16_t));
        const std::size_t init_n = std::min<std::size_t>(static_cast<std::size_t>(avail), kCsndFrames / 2);
        for (std::size_t i = 0; i < init_n; ++i) {
            const std::size_t src = static_cast<std::size_t>((r + i) & kRingMask) * 2;
            g_csnd_left[i] = g_ring[src];
            g_csnd_right[i] = g_ring[src + 1];
        }
        r += init_n;
        g_read.store(r, std::memory_order_release);
        g_csnd_written = init_n;
        GSPGPU_FlushDataCache(g_csnd_left, kCsndFrames * sizeof(std::int16_t));
        GSPGPU_FlushDataCache(g_csnd_right, kCsndFrames * sizeof(std::int16_t));
        const u32 flags = SOUND_FORMAT_16BIT | SOUND_REPEAT | SOUND_LINEAR_INTERP;
        const u32 bytes = static_cast<u32>(kCsndFrames * sizeof(std::int16_t));
        csndPlaySound(kCsndChLeft, flags, kSampleRate, 1.0f, -1.0f, g_csnd_left, g_csnd_left, bytes);
        csndPlaySound(kCsndChRight, flags, kSampleRate, 1.0f, 1.0f, g_csnd_right, g_csnd_right, bytes);
        g_csnd_start_tick = svcGetSystemTick();
        g_csnd_started = true;
        return;
    }

    const u64 elapsed = svcGetSystemTick() - g_csnd_start_tick;
    const std::uint64_t play_pos = (elapsed * kSampleRate) / SYSCLOCK_ARM11;
    const std::uint64_t target = play_pos + kCsndFrames / 2;
    if (g_csnd_written + kCsndFrames < play_pos) g_csnd_written = play_pos;
    if (g_csnd_written >= target) return;

    const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(target - g_csnd_written, kCsndFrames));
    const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(want, avail));
    for (std::size_t i = 0; i < take; ++i) {
        const std::size_t src = static_cast<std::size_t>((r + i) & kRingMask) * 2;
        const std::size_t dst = static_cast<std::size_t>((g_csnd_written + i) & kCsndMask);
        g_csnd_left[dst] = g_ring[src];
        g_csnd_right[dst] = g_ring[src + 1];
    }
    for (std::size_t i = take; i < want; ++i) {
        const std::size_t dst = static_cast<std::size_t>((g_csnd_written + i) & kCsndMask);
        g_csnd_left[dst] = 0;
        g_csnd_right[dst] = 0;
    }
    r += take;
    g_csnd_written += want;
    g_read.store(r, std::memory_order_release);
    GSPGPU_FlushDataCache(g_csnd_left, kCsndFrames * sizeof(std::int16_t));
    GSPGPU_FlushDataCache(g_csnd_right, kCsndFrames * sizeof(std::int16_t));
}

// libctru's ndspInit() creates its internal 4.88 ms event thread with processor_id = -2 (current core)
// at priority 0x18. Running ndspInit() on core 1 pins the NDSP thread to core 1 instead of preempting core 0.
struct NdspInitArg {
    Result rc = -1;
    int core = 0;
};

void ndsp_init_worker(void *arg) {
    auto *a = static_cast<NdspInitArg *>(arg);
    a->core = 1;
    a->rc = ndspInit();
}

Result init_ndsp_on_core1(int &core_used) {
    core_used = 0;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    if (R_SUCCEEDED(APT_SetAppCpuTimeLimit(80))) {
        NdspInitArg arg{};
        if (Thread t = threadCreate(ndsp_init_worker, &arg, 16 * 1024, prio, 1, false)) {
            threadJoin(t, U64_MAX);
            threadFree(t);
            core_used = arg.core;
            return arg.rc;
        }
    }
    return ndspInit();
}

}   // namespace

void push(const std::int16_t *samples, std::size_t frames) {
    if (!frames || g_stopping.load(std::memory_order_relaxed)) return;
    const std::uint64_t w = g_write.load(std::memory_order_relaxed);
    const std::uint64_t r = g_read.load(std::memory_order_acquire);
    g_pushed.fetch_add(static_cast<std::uint32_t>(frames), std::memory_order_relaxed);
    if (w - r + frames > kMaxQueued) {
        g_dropped.fetch_add(static_cast<std::uint32_t>(frames), std::memory_order_relaxed);
        return;
    }
    const std::size_t start = static_cast<std::size_t>(w & kRingMask);
    const std::size_t first = std::min(frames, kRingFrames - start);
    std::memcpy(&g_ring[start * 2], samples, first * 4);
    if (first < frames) std::memcpy(g_ring, samples + first * 2, (frames - first) * 4);
    g_write.store(w + frames, std::memory_order_release);
}

bool start() {
    g_write.store(0, std::memory_order_relaxed);
    g_read.store(0, std::memory_order_relaxed);
    g_stopping.store(false, std::memory_order_relaxed);
    g_have_prev = false;
    g_tempo = 1.0f;
    g_next_buf = 0;
    for (std::size_t i = 0; i < kHop; ++i) {
        const double sn = std::sin(3.14159265358979 * (static_cast<double>(i) + 0.5) / (2.0 * kHop));
        g_fade[i] = static_cast<std::int32_t>(sn * sn * 32768.0 + 0.5);
    }

    int ndsp_core = 0;
    const Result rc = init_ndsp_on_core1(ndsp_core);
    if (R_SUCCEEDED(rc)) {
        g_ndsp_linear = static_cast<std::int16_t *>(linearAlloc(kNumBufs * kBufFrames * 2 * sizeof(std::int16_t)));
        if (g_ndsp_linear) {
            std::memset(g_bufs, 0, sizeof g_bufs);
            for (std::size_t i = 0; i < kNumBufs; ++i) {
                g_bufs[i].data_pcm16 = g_ndsp_linear + i * kBufFrames * 2;
                g_bufs[i].nsamples = kBufFrames;
                g_bufs[i].status = NDSP_WBUF_FREE;
            }
            ndspSetOutputMode(NDSP_OUTPUT_STEREO);
            ndspChnReset(0);
            ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);
            ndspChnSetRate(0, static_cast<float>(kSampleRate));
            ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
            float mix[12] = {};
            mix[0] = 1.0f;
            mix[1] = 1.0f;
            ndspChnSetMix(0, mix);
            g_backend = Backend::Ndsp;
            ps2::set_audio_sink(push);
            ndspSetCallback(ndsp_callback, nullptr);
            std::fprintf(stderr, "[audio] NDSP ready on core %d (%lu Hz stereo, %u x %u frames)\n",
                         ndsp_core, static_cast<unsigned long>(kSampleRate), static_cast<unsigned>(kNumBufs),
                         static_cast<unsigned>(kBufFrames));
            return true;
        }
        ndspExit();
        std::fprintf(stderr, "[audio] linearAlloc failed for NDSP buffers\n");
    } else {
        std::fprintf(stderr, "[audio] ndspInit failed (0x%08lX); dump sdmc:/3ds/dspfirm.cdc for DSP audio\n",
                     static_cast<unsigned long>(rc));
    }

    const Result crc = csndInit();
    if (R_SUCCEEDED(crc)) {
        g_csnd_left = static_cast<std::int16_t *>(linearAlloc(kCsndFrames * sizeof(std::int16_t)));
        g_csnd_right = static_cast<std::int16_t *>(linearAlloc(kCsndFrames * sizeof(std::int16_t)));
        if (g_csnd_left && g_csnd_right) {
            g_csnd_started = false;
            g_csnd_written = 0;
            g_backend = Backend::Csnd;
            ps2::set_audio_sink(push);
            std::fprintf(stderr, "[audio] CSND fallback ready (%lu Hz stereo)\n",
                         static_cast<unsigned long>(kSampleRate));
            return true;
        }
        if (g_csnd_left) linearFree(g_csnd_left);
        if (g_csnd_right) linearFree(g_csnd_right);
        g_csnd_left = g_csnd_right = nullptr;
        csndExit();
    } else {
        std::fprintf(stderr, "[audio] csndInit failed (0x%08lX); sound off\n", static_cast<unsigned long>(crc));
    }
    return false;
}

void report(double seconds) {
    if (g_backend == Backend::None || seconds <= 0.0) return;
    const std::uint32_t pushed = g_pushed.exchange(0), dropped = g_dropped.exchange(0), under = g_underruns.exchange(0);
    // The stretch figures are the callback thread's; read without a lock, they are only a log line.
    const std::uint32_t steps = g_steps;
    const float tempo = steps ? g_tempo_sum / static_cast<float>(steps) : 0.0f, low = g_tempo_min;
    g_steps = 0;
    g_tempo_sum = 0.0f;
    g_tempo_min = 1.0f;
    const std::uint64_t queued = g_write.load() - g_read.load();
    std::fprintf(stderr, "[audio] %.0f%% of real time made (%lu frames in %.1f s), %lu dropped, %lu underruns, tempo %.0f%% (lowest %.0f%%), "
                 "%lu queued\n", 100.0 * pushed / (seconds * kSampleRate), static_cast<unsigned long>(pushed), seconds,
                 static_cast<unsigned long>(dropped), static_cast<unsigned long>(under), 100.0 * tempo, 100.0 * low,
                 static_cast<unsigned long>(queued));
}

void poll() {
    if (g_backend == Backend::Csnd && !g_stopping.load(std::memory_order_relaxed)) poll_csnd();
}

void stop(bool clear_sink) {
    g_stopping.store(true, std::memory_order_release);
    if (clear_sink) ps2::set_audio_sink(nullptr);
    if (g_backend == Backend::Ndsp) {
        ndspSetCallback(nullptr, nullptr);
        ndspChnWaveBufClear(0);
        ndspChnReset(0);
        ndspExit();
        if (g_ndsp_linear) linearFree(g_ndsp_linear);
        g_ndsp_linear = nullptr;
    } else if (g_backend == Backend::Csnd) {
        if (g_csnd_started) {
            CSND_SetPlayState(kCsndChLeft, 0);
            CSND_SetPlayState(kCsndChRight, 0);
            csndExecCmds(true);
            g_csnd_started = false;
        }
        csndExit();
        if (g_csnd_left) linearFree(g_csnd_left);
        if (g_csnd_right) linearFree(g_csnd_right);
        g_csnd_left = g_csnd_right = nullptr;
    }
    g_backend = Backend::None;
}

}   // namespace audio3ds
