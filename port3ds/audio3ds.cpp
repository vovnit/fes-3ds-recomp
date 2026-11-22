#include "audio3ds.hpp"

#include "ps2/system.hpp"

#include <3ds.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace audio3ds {

namespace {

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::size_t kRingFrames = 16384;       // power of 2 (~341 ms at 48 kHz)
constexpr std::size_t kRingMask = kRingFrames - 1;
constexpr std::size_t kMaxQueued = 9600;         // ~200 ms latency cap (drops excess during FAST/uncapped)

// NDSP streaming parameters: 6 x 1024 stereo frames = 24 KB linear RAM.
constexpr std::size_t kNumBufs = 6;
constexpr std::size_t kBufFrames = 1024;         // 21.33 ms per wave buffer
constexpr std::size_t kTargetQueued = 3;         // ~64 ms kept in DSP hardware queue
constexpr std::size_t kPrebufferFrames = 1024;   // ~21.3 ms before starting from idle/underrun
constexpr std::size_t kMinFlushFrames = 128;     // 2.67 ms minimum partial submit before underrunning
constexpr std::size_t kRampOutFrames = 64;
constexpr std::size_t kRampInFrames = 16;

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

// NDSP state (touched only on NDSP callback thread once running).
std::int16_t *g_ndsp_linear = nullptr;
ndspWaveBuf g_bufs[kNumBufs];
std::size_t g_next_buf = 0;
bool g_playing = false;
bool g_ramp_in = false;
bool g_rate_fast = false;
std::int16_t g_last_l = 0, g_last_r = 0;

// CSND fallback state (touched only on main thread in poll()).
std::int16_t *g_csnd_left = nullptr;
std::int16_t *g_csnd_right = nullptr;
bool g_csnd_started = false;
u64 g_csnd_start_tick = 0;
std::uint64_t g_csnd_written = 0;

void copy_from_ring(std::int16_t *dst, std::uint64_t r, std::size_t frames) {
    const std::size_t start = static_cast<std::size_t>(r & kRingMask);
    const std::size_t first = std::min(frames, kRingFrames - start);
    std::memcpy(dst, &g_ring[start * 2], first * 4);
    if (first < frames) std::memcpy(dst + first * 2, g_ring, (frames - first) * 4);
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
    if (g_playing && active >= kTargetQueued) return;

    const std::uint64_t w = g_write.load(std::memory_order_acquire);
    std::uint64_t r = g_read.load(std::memory_order_relaxed);
    std::uint64_t avail = w - r;

    // Gentle clock-drift compensation: nudge DSP rate +0.5% when backlog grows past ~107 ms.
    if (!g_rate_fast && avail > 5120) {
        ndspChnSetRate(0, static_cast<float>(kSampleRate) * 1.005f);
        g_rate_fast = true;
    } else if (g_rate_fast && avail < 2560) {
        ndspChnSetRate(0, static_cast<float>(kSampleRate));
        g_rate_fast = false;
    }

    if (!g_playing) {
        if (avail < kPrebufferFrames) return;
        g_playing = true;
        g_ramp_in = true;
    }

    while (active < kTargetQueued) {
        ndspWaveBuf &buf = g_bufs[g_next_buf];
        if (buf.status != NDSP_WBUF_FREE && buf.status != NDSP_WBUF_DONE) break;

        avail = w - r;
        if (avail >= kBufFrames) {
            copy_from_ring(buf.data_pcm16, r, kBufFrames);
            r += kBufFrames;
            if (g_ramp_in) {
                for (std::size_t i = 0; i < kRampInFrames; ++i) {
                    const std::int32_t s = static_cast<std::int32_t>(i);
                    buf.data_pcm16[i * 2] = static_cast<std::int16_t>((buf.data_pcm16[i * 2] * s) / static_cast<std::int32_t>(kRampInFrames));
                    buf.data_pcm16[i * 2 + 1] = static_cast<std::int16_t>((buf.data_pcm16[i * 2 + 1] * s) / static_cast<std::int32_t>(kRampInFrames));
                }
                g_ramp_in = false;
            }
            g_last_l = buf.data_pcm16[(kBufFrames - 1) * 2];
            g_last_r = buf.data_pcm16[(kBufFrames - 1) * 2 + 1];
            submit_ndsp_buf(buf, kBufFrames);
            g_next_buf = (g_next_buf + 1) % kNumBufs;
            ++active;
        } else if (active == 0) {
            if (avail >= kMinFlushFrames) {
                const std::size_t n = static_cast<std::size_t>(avail);
                copy_from_ring(buf.data_pcm16, r, n);
                r += n;
                if (g_ramp_in) {
                    const std::size_t ramp = std::min(n, kRampInFrames);
                    for (std::size_t i = 0; i < ramp; ++i) {
                        const std::int32_t s = static_cast<std::int32_t>(i);
                        buf.data_pcm16[i * 2] = static_cast<std::int16_t>((buf.data_pcm16[i * 2] * s) / static_cast<std::int32_t>(ramp));
                        buf.data_pcm16[i * 2 + 1] = static_cast<std::int16_t>((buf.data_pcm16[i * 2 + 1] * s) / static_cast<std::int32_t>(ramp));
                    }
                    g_ramp_in = false;
                }
                g_last_l = buf.data_pcm16[(n - 1) * 2];
                g_last_r = buf.data_pcm16[(n - 1) * 2 + 1];
                submit_ndsp_buf(buf, n);
                g_next_buf = (g_next_buf + 1) % kNumBufs;
                ++active;
                break;
            }
            // True underrun: smoothly fade last sample to zero to avoid audible clicks.
            g_playing = false;
            if (g_last_l != 0 || g_last_r != 0) {
                for (std::size_t i = 0; i < kRampOutFrames; ++i) {
                    const std::int32_t rem = static_cast<std::int32_t>(kRampOutFrames - 1 - i);
                    buf.data_pcm16[i * 2] = static_cast<std::int16_t>((g_last_l * rem) / static_cast<std::int32_t>(kRampOutFrames));
                    buf.data_pcm16[i * 2 + 1] = static_cast<std::int16_t>((g_last_r * rem) / static_cast<std::int32_t>(kRampOutFrames));
                }
                g_last_l = g_last_r = 0;
                submit_ndsp_buf(buf, kRampOutFrames);
                g_next_buf = (g_next_buf + 1) % kNumBufs;
            }
            break;
        } else {
            break;
        }
    }

    g_read.store(r, std::memory_order_release);
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
    if (w - r + frames > kMaxQueued) return;
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
    g_playing = false;
    g_ramp_in = false;
    g_rate_fast = false;
    g_last_l = g_last_r = 0;
    g_next_buf = 0;

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
