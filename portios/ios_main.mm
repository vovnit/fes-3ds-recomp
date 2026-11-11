// iOS / iPadOS front end for the recompiled game (ps2-web-recomp runtime + the C++ generated from your own disc).
// It does what host/main.cpp and host/window.cpp do on the desktop, with UIKit in place of SDL:
//   - the disc image is the first .iso in the app's Documents folder (Files app > On My iPhone > P3FES, or Finder file sharing)
//   - the memory card is Documents/memcard.ps2, optionally synced with your own WebDAV server (webdav.mm)
//   - save states in Documents/states, saved and loaded from the menu at any time
//   - the GPU renderer presents straight into a CAMetalLayer from the render thread; the CPU renderer's frames are shown
//     through the layer's contents instead
//   - sound goes through AVAudioEngine; a steady 59.94 Hz clock paces the game, nudged by the sound queue so the two never drift
//   - the game's threads run at the highest priority iOS gives apps, which iOS favours for the performance cores
//   - input: any MFi / Xbox / PlayStation controller, or the app's own see-through on-screen controls (touch.mm)
#import <AVFAudio/AVFAudio.h>
#import <GameController/GameController.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>

#include "frontend.h"
#import "touch.h"
#import "webdav.h"

#include "ps2/memory.hpp"
#include "ps2/system.hpp"

#include <mach/mach_time.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

void register_generated_code();
void register_generated_vu();

namespace ps2 {
extern std::uintptr_t g_stack_floor;   // generated.hpp: translated calls nest on the host stack down to here
// the iOS copy of system.cpp (CMakeLists.txt): run() calls g_safe_point between two slices of the game once g_safe_point_due is set
extern std::atomic<bool> g_safe_point_due;
extern void (*g_safe_point)();
}
extern std::atomic<float> g_portios_aspect;   // wide.cpp: shape of the picture; flat drawing is narrowed to match

using namespace fe;

namespace {

std::string g_documents;

// ---- disc image: find the boot program through SYSTEM.CNF and copy it out for load_elf

struct Iso {
    std::FILE *f = nullptr;
    bool sector(std::uint32_t lba, std::uint8_t *out, std::uint32_t count = 1) {
        if (fseeko(f, static_cast<off_t>(lba) * 2048, SEEK_SET) != 0) return false;
        return std::fread(out, 2048, count, f) == count;
    }
    // looks a name up in one directory; names on the disc are upper case and end in ";1"
    bool find(std::uint32_t dir_lba, std::uint32_t dir_size, const std::string &name, std::uint32_t &lba, std::uint32_t &size) {
        std::vector<std::uint8_t> dir((dir_size + 2047) / 2048 * 2048);
        if (dir.empty() || !sector(dir_lba, dir.data(), static_cast<std::uint32_t>(dir.size() / 2048))) return false;
        for (std::size_t at = 0; at < dir.size();) {
            const std::uint8_t len = dir[at];
            if (len == 0) {
                at = (at / 2048 + 1) * 2048;
                continue;
            }
            std::string entry(reinterpret_cast<const char *>(&dir[at + 33]), dir[at + 32]);
            if (const auto semi = entry.find(';'); semi != std::string::npos) entry.resize(semi);
            if (strcasecmp(entry.c_str(), name.c_str()) == 0) {
                std::memcpy(&lba, &dir[at + 2], 4);
                std::memcpy(&size, &dir[at + 10], 4);
                return true;
            }
            at += len;
        }
        return false;
    }
};

bool extract_boot(const std::string &iso_path, const std::string &out_path, std::string &boot_name) {
    Iso iso;
    iso.f = std::fopen(iso_path.c_str(), "rb");
    if (!iso.f) return false;
    std::uint8_t pvd[2048];
    bool ok = iso.sector(16, pvd) && std::memcmp(pvd + 1, "CD001", 5) == 0;
    std::uint32_t root_lba = 0, root_size = 0, lba = 0, size = 0;
    if (ok) {
        std::memcpy(&root_lba, pvd + 156 + 2, 4);
        std::memcpy(&root_size, pvd + 156 + 10, 4);
    }
    boot_name = "SLUS_216.21";
    if (ok && iso.find(root_lba, root_size, "SYSTEM.CNF", lba, size) && size < 4096) {
        std::vector<std::uint8_t> cnf((size + 2047) / 2048 * 2048);
        if (iso.sector(lba, cnf.data(), static_cast<std::uint32_t>(cnf.size() / 2048))) {
            const std::string text(reinterpret_cast<const char *>(cnf.data()), size);
            const auto boot = text.find("BOOT2");
            const auto slash = boot == std::string::npos ? boot : text.find('\\', boot);
            const auto semi = slash == std::string::npos ? slash : text.find(';', slash);
            if (semi != std::string::npos) boot_name = text.substr(slash + 1, semi - slash - 1);
        }
    }
    ok = ok && iso.find(root_lba, root_size, boot_name, lba, size) && size > 0 && size < 64u << 20;
    std::vector<std::uint8_t> elf(ok ? (size + 2047) / 2048 * 2048 : 0);
    ok = ok && iso.sector(lba, elf.data(), static_cast<std::uint32_t>(elf.size() / 2048));
    std::fclose(iso.f);
    if (!ok) return false;
    std::FILE *out = std::fopen(out_path.c_str(), "wb");
    if (!out) return false;
    ok = std::fwrite(elf.data(), 1, size, out) == size;
    std::fclose(out);
    return ok;
}


// ---- threads: the game, VU and render threads run at the top app priority, so iOS keeps them on the performance cores

void high_priority() { pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0); }

// ---- sound: the runtime hands over 48 kHz stereo 16-bit frames, the audio thread pulls them from this ring

constexpr std::size_t kRingFrames = 48000;   // one second
std::int16_t g_ring[kRingFrames * 2];
std::atomic<std::uint64_t> g_ring_write{0}, g_ring_read{0};
std::atomic<bool> g_audio_running{false};
AVAudioEngine *g_engine = nil;

std::size_t queued_frames() { return static_cast<std::size_t>(g_ring_write.load(std::memory_order_acquire) - g_ring_read.load(std::memory_order_acquire)); }

void queue_audio(const std::int16_t *samples, std::size_t frames) {
    if (queued_frames() > 48 * 500) return;   // more than half a second waiting: drop, as the desktop build does
    std::uint64_t w = g_ring_write.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < frames; ++i, ++w) {
        const std::size_t at = static_cast<std::size_t>(w % kRingFrames) * 2;
        g_ring[at] = samples[i * 2];
        g_ring[at + 1] = samples[i * 2 + 1];
    }
    g_ring_write.store(w, std::memory_order_release);
}

void start_audio() {
    static const bool mute = std::getenv("P3FES_MUTE") != nullptr;   // test switch: no sound at all
    if (mute || (g_engine && g_engine.isRunning)) return;
    NSError *error = nil;
    AVAudioSession *session = [AVAudioSession sharedInstance];
    [session setCategory:AVAudioSessionCategoryPlayback mode:AVAudioSessionModeDefault options:AVAudioSessionCategoryOptionMixWithOthers error:&error];
    [session setPreferredSampleRate:48000 error:nil];
    [session setPreferredIOBufferDuration:0.01 error:nil];
    [session setActive:YES error:nil];
    if (!g_engine) {
        g_engine = [[AVAudioEngine alloc] init];
        AVAudioFormat *format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:48000 channels:2];
        AVAudioSourceNode *source = [[AVAudioSourceNode alloc] initWithFormat:format
                                                                  renderBlock:^OSStatus(BOOL *silence, const AudioTimeStamp *, AVAudioFrameCount count, AudioBufferList *buffers) {
            float *left = static_cast<float *>(buffers->mBuffers[0].mData);
            float *right = buffers->mNumberBuffers > 1 ? static_cast<float *>(buffers->mBuffers[1].mData) : nullptr;
            std::uint64_t r = g_ring_read.load(std::memory_order_relaxed);
            const std::uint64_t w = g_ring_write.load(std::memory_order_acquire);
            for (AVAudioFrameCount i = 0; i < count; ++i) {
                float l = 0.0f, rr = 0.0f;
                if (r < w) {
                    const std::size_t at = static_cast<std::size_t>(r % kRingFrames) * 2;
                    l = g_ring[at] / 32768.0f;
                    rr = g_ring[at + 1] / 32768.0f;
                    ++r;
                }
                left[i] = l;
                if (right) right[i] = rr;
            }
            g_ring_read.store(r, std::memory_order_release);
            *silence = NO;
            return noErr;
        }];
        [g_engine attachNode:source];
        [g_engine connect:source to:g_engine.mainMixerNode format:format];
    }
    if ([g_engine startAndReturnError:&error]) g_audio_running = true;
    else NSLog(@"[audio] cannot start: %@", error);
}

// ---- pacing: one vblank every 1/59.94 s on a precise clock, so frames come out evenly spaced. The sound queue only nudges the
// clock (at most 5% either way) to stay near kTargetQueueMs, instead of the game running in bursts whenever the sound
// device takes a block, which spaced frames 10 and 20 ms apart and made the picture judder.

constexpr double kTargetQueueMs = 70.0;
std::atomic<bool> g_paused{false};
std::mutex g_pause_lock;
std::condition_variable g_pause_cv;

void set_paused(bool paused) {
    {
        std::lock_guard<std::mutex> lock(g_pause_lock);
        g_paused.store(paused);
    }
    g_pause_cv.notify_all();
}

double ticks_per_second() {
    static const double t = [] {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        return 1e9 * tb.denom / tb.numer;
    }();
    return t;
}

void pace() {
    static std::uint64_t next = 0;
    if (g_paused.load()) {   // the menu is open or the app is in the background
        std::unique_lock<std::mutex> lock(g_pause_lock);
        // a save state request lets the game go on to the point where states are saved; it stops again at the next frame
        g_pause_cv.wait(lock, [] { return !g_paused.load() || ps2::g_safe_point_due.load(); });
        next = 0;
    }
    double period = ps2::g_options.pal ? 0.02 : 1001.0 / 60000.0;
    if (g_audio_running.load()) {
        const double queued_ms = static_cast<double>(queued_frames()) / 48.0;
        period *= 1.0 + 0.05 * std::clamp((queued_ms - kTargetQueueMs) / kTargetQueueMs, -1.0, 1.0);
    }
    const auto step = static_cast<std::uint64_t>(period * ticks_per_second());
    const std::uint64_t now = mach_absolute_time();
    if (!next || now > next + 4 * step) next = now;   // far behind (a slow scene, or after a pause): go on from now, don't rush
    next += step;
    if (next > now) mach_wait_until(next);
}

// ---- screen shape (wide.cpp has the rest). The game's 3D view is widened by rewriting its x field of view, a float at 007cad1c
// that holds 4/3 (as the PC window's widescreen mode does), every frame, to the shape of the picture. It is rewritten only
// while it holds 4/3 or a widened value, so other values the game may put there are left alone. A save state keeps whatever
// the address held; the next frame sets it again.

enum class Screen : int { classic = 0, wide = 1, full = 2 };
constexpr std::uint32_t kFovAddress = 0x007cad1c;
constexpr float kWideAspect = 1.70f;   // "16:9": the PC window's value, a little under 16/9 = 1.78, at which no scene is known to stop
std::atomic<std::uint32_t> g_fov_bits{std::bit_cast<std::uint32_t>(4.0f / 3.0f)};
std::atomic<bool> g_gpu_drawing{false};   // the 2D narrowing needs the GPU renderer; the CPU renderer stays 4:3

void set_picture_aspect(float aspect) {
    if (!g_gpu_drawing.load()) aspect = 4.0f / 3.0f;
    g_portios_aspect.store(aspect);
    g_fov_bits.store(std::bit_cast<std::uint32_t>(aspect));
}

void write_fov() {
    std::uint32_t cur;
    std::memcpy(&cur, ps2::g_ram + kFovAddress, 4);
    const std::uint32_t want = g_fov_bits.load(std::memory_order_relaxed);
    if (cur == want) return;
    const float f = std::bit_cast<float>(cur);
    if (cur == std::bit_cast<std::uint32_t>(4.0f / 3.0f) || cur == std::bit_cast<std::uint32_t>(kWideAspect) || (f >= 1.75f && f <= 3.0f))
        std::memcpy(ps2::g_ram + kFovAddress, &want, 4);
}

// ---- frames. GPU renderer: presented into this CAMetalLayer by the render thread. CPU renderer: the render thread copies each
// finished frame here and the main thread shows the newest one.

void *g_layer = nullptr;   // the CAMetalLayer of the game view (the view keeps it alive)
std::atomic<std::uint32_t> g_layer_w{0}, g_layer_h{0};   // its size in pixels, from the main thread
std::atomic<bool> g_gpu_present{false}, g_foreground{true};
std::atomic<std::uint32_t> g_presented{0};

std::mutex g_frame_lock;
std::vector<std::uint32_t> g_frame;
std::uint32_t g_frame_w = 0, g_frame_h = 0;
bool g_frame_new = false;

void copy_frame(const std::uint32_t *rgba, std::uint32_t w, std::uint32_t h) {
    std::lock_guard<std::mutex> lock(g_frame_lock);
    g_frame.assign(rgba, rgba + static_cast<std::size_t>(w) * h);
    g_frame_w = w;
    g_frame_h = h;
    g_frame_new = true;
}

// ---- input

std::uint8_t stick(float v, bool flip) {
    if (flip) v = -v;
    return static_cast<std::uint8_t>(std::clamp(static_cast<int>((v + 1.0f) * 127.5f), 0, 255));
}

std::atomic<bool> g_pad_resend{false};   // a loaded state holds the pad as it was then: send the current one again

void poll_pads() {
    std::uint16_t pressed = g_touch_buttons.load();
    std::uint8_t l2 = 0xFF, r2 = 0xFF, lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
    if (const std::uint16_t touch = g_touch_stick.load(); touch != 0x8080) {
        lx = static_cast<std::uint8_t>(touch & 0xFF);
        ly = static_cast<std::uint8_t>(touch >> 8);
    }
    for (GCController *c in GCController.controllers) {
        GCExtendedGamepad *p = c.extendedGamepad;
        if (!p) continue;
        const struct { GCControllerButtonInput *b; std::uint16_t mask; } map[] = {
            {p.buttonA, kCross}, {p.buttonB, kCircle}, {p.buttonX, kSquare}, {p.buttonY, kTriangle},
            {p.leftShoulder, kL1}, {p.rightShoulder, kR1}, {p.leftThumbstickButton, kL3}, {p.rightThumbstickButton, kR3},
            {p.buttonMenu, kStart}, {p.buttonOptions, kSelect},
            {p.dpad.up, kUp}, {p.dpad.down, kDown}, {p.dpad.left, kLeft}, {p.dpad.right, kRight},
        };
        for (const auto &m : map)
            if (m.b && m.b.isPressed) pressed |= m.mask;
        if (p.leftTrigger.value > 0.06f) {
            pressed |= kL2;
            l2 = static_cast<std::uint8_t>(std::max(1, static_cast<int>(p.leftTrigger.value * 255)));
        }
        if (p.rightTrigger.value > 0.06f) {
            pressed |= kR2;
            r2 = static_cast<std::uint8_t>(std::max(1, static_cast<int>(p.rightTrigger.value * 255)));
        }
        const float dead = 0.12f;
        const GCControllerDirectionPad *ls = p.leftThumbstick, *rs = p.rightThumbstick;
        if (lx == 0x80 && std::fabs(ls.xAxis.value) > dead) lx = stick(ls.xAxis.value, false);
        if (ly == 0x80 && std::fabs(ls.yAxis.value) > dead) ly = stick(ls.yAxis.value, true);
        if (rx == 0x80 && std::fabs(rs.xAxis.value) > dead) rx = stick(rs.xAxis.value, false);
        if (ry == 0x80 && std::fabs(rs.yAxis.value) > dead) ry = stick(rs.yAxis.value, true);
    }
    static std::uint64_t last = ~0ull;
    if (g_pad_resend.exchange(false)) last = ~0ull;
    const std::uint64_t state = pressed | (std::uint64_t(rx) << 16) | (std::uint64_t(ry) << 24) | (std::uint64_t(lx) << 32) | (std::uint64_t(ly) << 40) |
                                (std::uint64_t(l2) << 48) | (std::uint64_t(r2) << 56);
    if (state == last) return;
    last = state;
    ps2::set_pad_pressure(8, l2);
    ps2::set_pad_pressure(9, r2);
    ps2::set_pad_buttons(pressed);
    ps2::set_pad_axis(0, rx);
    ps2::set_pad_axis(1, ry);
    ps2::set_pad_axis(2, lx);
    ps2::set_pad_axis(3, ly);
}

// ---- save states: the whole machine in Documents/states/slot-N.state, about 46 MB each. The game thread saves or loads one at
// the point between two slices of the game where run() saves its own (the iOS copy of system.cpp, CMakeLists.txt). Before a
// load the game as it is goes to before-load.state: the load can be undone, and a broken file leaves the game as it was.
// The memory card is not part of a state, so loading one never takes back a save made in the game.

constexpr int kStateSlots = kSaveSyncSlots;

std::string states_dir() { return g_documents + "/states"; }
std::string slot_path(int slot) { return states_dir() + "/slot-" + std::to_string(slot) + ".state"; }
std::string undo_path() { return states_dir() + "/before-load.state"; }

struct StateJob {
    bool load = false;
    std::string path;
    NSString *name;
    void (^done)(NSString *message, bool ok);
};
StateJob g_state_job;   // written by the main thread before it sets g_safe_point_due, read by the game thread at that point

void run_state_job() {
    const StateJob job = g_state_job;
    NSString *message;
    bool ok = false;
    if (!job.load) {
        const std::string part = job.path + ".part";   // the slot keeps its old state until the new one is complete
        ok = ps2::save_state(part) && std::rename(part.c_str(), job.path.c_str()) == 0;
        if (ok) message = [NSString stringWithFormat:@"Saved in %@.", job.name];
        else {
            std::remove(part.c_str());
            message = @"The state couldn't be saved. Is the device full?";
        }
    } else {
        const std::string now = undo_path() + ".new";
        if (!ps2::save_state(now)) {
            std::remove(now.c_str());
            message = @"Not loaded: there's no room to keep the game as it is first. Is the device full?";
        } else if ((ok = ps2::load_state(job.path))) {
            std::rename(now.c_str(), undo_path().c_str());   // after loading, as the job may be loading the old before-load.state
            message = [NSString stringWithFormat:@"Loaded %@.", job.name];
        } else if (ps2::load_state(now)) {
            std::remove(now.c_str());
            NSString *what = [[job.name substringToIndex:1].uppercaseString stringByAppendingString:[job.name substringFromIndex:1]];
            message = [NSString stringWithFormat:@"%@ couldn't be loaded (a damaged file, or a state from another version of the app). "
                                                 @"The game goes on as it was.", what];
        } else {
            message = @"The state couldn't be loaded and the game may not work now. Close the app and start it again.";
        }
        g_pad_resend = true;
    }
    void (^done)(NSString *, bool) = job.done;
    dispatch_async(dispatch_get_main_queue(), ^{ done(message, ok); });
}

// the game also leaves the pause for this (pace); done gets a message for the screen, on the main thread
void request_state(bool load, const std::string &path, NSString *name, void (^done)(NSString *message, bool ok)) {
    [[NSFileManager defaultManager] createDirectoryAtPath:@(states_dir().c_str()) withIntermediateDirectories:YES attributes:nil error:nil];
    g_state_job = {load, path, name, done};
    ps2::g_safe_point = run_state_job;
    {
        std::lock_guard<std::mutex> lock(g_pause_lock);
        ps2::g_safe_point_due.store(true, std::memory_order_release);
    }
    g_pause_cv.notify_all();
}

// a request the game thread has not taken yet (the game stopped): withdrawn, so it can't run later
bool withdraw_state_request() {
    bool due = true;
    return ps2::g_safe_point_due.compare_exchange_strong(due, false);
}

NSDate *state_date(const std::string &path) {
    return [[NSFileManager defaultManager] attributesOfItemAtPath:@(path.c_str()) error:nil].fileModificationDate;
}

// ---- the game itself, on its own threads like the desktop window's threaded mode

std::atomic<bool> g_started{false};
int g_scale_running = 0;   // the scale the game was started with (main thread)

void start_game(const std::string &iso, int scale) {
    if (g_started.exchange(true)) return;
    std::thread([iso, scale] {
        high_priority();   // this thread draws
        const std::string elf = g_documents + "/.boot.elf";
        std::string boot;
        if (!extract_boot(iso, elf, boot)) {
            std::fprintf(stderr, "[ios] cannot read the boot program from %s\n", iso.c_str());
            return;
        }
        ps2::g_options.disc_path = iso;
        ps2::g_options.memcard_path = g_documents + "/memcard.ps2";
        ps2::g_options.on_memcard_saved = [] { [[SaveSync shared] cardWritten]; };
        ps2::set_audio_sink(queue_audio);
        register_generated_code();
        register_generated_vu();
        setenv("PS2_SCALE", std::to_string(scale).c_str(), 1);   // read once, when the GPU renderer starts
        ps2::init_graphics();
        bool surface = false;
        std::uint32_t surface_w = std::max(1u, g_layer_w.load()), surface_h = std::max(1u, g_layer_h.load());
        if (g_layer) {
            ps2::WindowTarget target;
            target.kind = ps2::WindowTarget::Canvas;   // the iOS copy of gpu.cpp takes this kind with a CAMetalLayer (CMakeLists.txt)
            target.surface = g_layer;
            target.width = surface_w;
            target.height = surface_h;
            surface = ps2::attach_window(target);
        }
        g_gpu_present = surface;
        g_gpu_drawing = surface;
        dispatch_async(dispatch_get_main_queue(), ^{ [[NSNotificationCenter defaultCenter] postNotificationName:@"P3FESScreen" object:nil]; });
        std::fprintf(stderr, "[ios] %s\n", surface ? "presenting through Metal" : "frames copied to the screen (CPU renderer)");
        ps2::g_options.on_vblank = [] {
            write_fov();
            ps2::post_present();
            pace();
        };
        ps2::start_render_thread();
        ps2::start_vu_thread();
        std::thread([] {
            high_priority();
            ps2::run_vu_thread();
        }).detach();
        static std::atomic<bool> done{false};
        static std::string s_elf, s_boot;
        s_elf = elf;
        s_boot = boot;
        // the game's main thread gets a big stack, as on the web build, and translated calls may use most of it before they
        // unwind to the dispatcher (run() would otherwise allow 256 KB)
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 64u << 20);
        pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
        pthread_t emu;
        pthread_create(&emu, &attr, [](void *) -> void * {
            ps2::g_stack_floor = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0)) - (48u << 20);
            std::uint32_t entry = 0;
            if (!ps2::load_elf(s_elf, entry)) std::fprintf(stderr, "[ios] cannot load %s\n", s_elf.c_str());
            else {
                ps2::boot(entry, {s_boot});
                ps2::run();
            }
            done = true;
            ps2::wake_render_thread();
            return nullptr;
        }, nullptr);
        pthread_attr_destroy(&attr);
        const std::function<void(const std::uint32_t *, std::uint32_t, std::uint32_t)> copy = copy_frame, none;
        const std::function<void()> count = [] { g_presented.fetch_add(1, std::memory_order_relaxed); };
        while (!done.load()) {
            if (!g_foreground.load()) {   // iOS does not let a background app use the GPU
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            if (surface) {
                const std::uint32_t w = g_layer_w.load(), h = g_layer_h.load();
                if (w && h && (w != surface_w || h != surface_h)) {
                    ps2::resize_window(w, h);
                    surface_w = w;
                    surface_h = h;
                }
            }
            if (ps2::render_pending(surface, surface ? none : copy, count, 8.0) == ps2::Rendered::idle) ps2::wait_render_work(4);
        }
        std::fprintf(stderr, "[ios] the game stopped\n");
    }).detach();
}

std::string find_disc() {
    NSArray<NSString *> *files = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:@(g_documents.c_str()) error:nil];
    for (NSString *name in [files sortedArrayUsingSelector:@selector(compare:)])
        if ([name.pathExtension caseInsensitiveCompare:@"iso"] == NSOrderedSame) return g_documents + "/" + name.UTF8String;
    return {};
}

NSString *const kOpacityKey = @"controls.opacity";
NSString *const kScreenKey = @"screen.shape";   // Screen: 0 4:3, 1 wide (default), 2 full screen
NSString *const kScaleKey = @"screen.scale";    // Resolution: 1 to 4 times the PS2's 640x448 (not set: kDefaultScale)
NSString *const kSpeedKey = @"screen.speed";    // Show speed: frames a second in the top left corner
constexpr int kDefaultScale = 2;

// the smallest scale at which the game's 448 lines are at least as many as the screen's (3x on an iPhone, 4x on an iPad).
// The GPU renderer's work grows with the square of the scale: in the Simulator on an M1 Pro (14 GPU cores) the lockers
// scene runs at 60 fps at 1x and 2x, 42 at 3x and 26 at 4x, limited by the GPU.
int native_scale() {
    const CGSize px = UIScreen.mainScreen.nativeBounds.size;
    const double lines = std::min(px.width, px.height);
    return std::clamp(static_cast<int>(std::ceil(lines / 448.0 - 0.05)), 1, 4);
}

int chosen_scale() {
    const NSInteger v = [NSUserDefaults.standardUserDefaults integerForKey:kScaleKey];
    return v >= 1 && v <= 4 ? static_cast<int>(v) : kDefaultScale;
}

}

// ---- UIKit

@interface MetalView : UIView
@end
@implementation MetalView
+ (Class)layerClass { return CAMetalLayer.class; }
@end

// The save sync form: address, user name, password and the self-signed switch, in a scroll view that keeps the field being
// typed in above the keyboard (an alert with three text fields has no room for them on a phone held sideways).
enum class SyncSettingsResult { saved, off, cancelled };

@interface SyncSettingsController : UIViewController <UITextFieldDelegate>
@property(nonatomic, copy) NSString *note;
@property(nonatomic, copy) void (^done)(SyncSettingsResult result);
@end

@implementation SyncSettingsController {
    UITextField *_url, *_user, *_password;
    UISwitch *_insecure;
    UIScrollView *_scroll;
}

- (UITextField *)field:(NSString *)placeholder text:(NSString *)text {
    UITextField *f = [[UITextField alloc] init];
    f.placeholder = placeholder;
    f.text = text;
    f.borderStyle = UITextBorderStyleRoundedRect;
    f.autocapitalizationType = UITextAutocapitalizationTypeNone;
    f.autocorrectionType = UITextAutocorrectionTypeNo;
    f.spellCheckingType = UITextSpellCheckingTypeNo;
    f.clearButtonMode = UITextFieldViewModeWhileEditing;
    f.delegate = self;
    [f.heightAnchor constraintEqualToConstant:40].active = YES;
    return f;
}

- (UILabel *)label:(NSString *)text style:(UIFontTextStyle)style {
    UILabel *l = [[UILabel alloc] init];
    l.text = text;
    l.font = [UIFont preferredFontForTextStyle:style];
    l.numberOfLines = 0;
    return l;
}

- (UIButton *)button:(NSString *)title filled:(BOOL)filled action:(SEL)action {
    UIButtonConfiguration *c = filled ? [UIButtonConfiguration filledButtonConfiguration] : [UIButtonConfiguration grayButtonConfiguration];
    c.title = title;
    UIButton *b = [UIButton buttonWithConfiguration:c primaryAction:nil];
    [b addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
    return b;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;
    self.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
    self.modalInPresentation = YES;   // closed only by its buttons, which resume the game
    SaveSync *sync = [SaveSync shared];

    UIScrollView *scroll = _scroll = [[UIScrollView alloc] init];
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    scroll.keyboardDismissMode = UIScrollViewKeyboardDismissModeInteractive;
    [self.view addSubview:scroll];

    UIStackView *stack = [[UIStackView alloc] init];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 10;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [scroll addSubview:stack];

    [stack addArrangedSubview:[self label:@"Save sync (WebDAV)" style:UIFontTextStyleTitle2]];
    if (_note.length) {
        UILabel *n = [self label:_note style:UIFontTextStyleSubheadline];
        n.textColor = UIColor.systemOrangeColor;
        [stack addArrangedSubview:n];
    }
    UILabel *about = [self label:@"The folder on your own WebDAV server (Nextcloud, ownCloud, a NAS, Apache, nginx, rclone serve webdav...) "
                                 @"where the memory card is kept as memcard.ps2. The password is stored in the iOS keychain."
                           style:UIFontTextStyleFootnote];
    about.textColor = UIColor.secondaryLabelColor;
    [stack addArrangedSubview:about];

    _url = [self field:@"https://example.com/dav/p3fes/" text:sync.url];
    _url.keyboardType = UIKeyboardTypeURL;
    _url.textContentType = UITextContentTypeURL;
    _url.returnKeyType = UIReturnKeyNext;
    _user = [self field:@"user name" text:sync.user];
    _user.textContentType = UITextContentTypeUsername;
    _user.returnKeyType = UIReturnKeyNext;
    _password = [self field:@"password" text:sync.password];
    _password.secureTextEntry = YES;
    _password.textContentType = UITextContentTypePassword;
    _password.returnKeyType = UIReturnKeyDone;
    [stack addArrangedSubview:_url];
    [stack addArrangedSubview:_user];
    [stack addArrangedSubview:_password];

    UIStackView *row = [[UIStackView alloc] init];
    row.spacing = 10;
    row.alignment = UIStackViewAlignmentCenter;
    _insecure = [[UISwitch alloc] init];
    _insecure.on = sync.insecure;
    [row addArrangedSubview:_insecure];
    [row addArrangedSubview:[self label:@"Accept a self-signed certificate (a home server's own)" style:UIFontTextStyleSubheadline]];
    [stack addArrangedSubview:row];

    UIStackView *buttons = [[UIStackView alloc] init];
    buttons.spacing = 10;
    buttons.distribution = UIStackViewDistributionFillEqually;
    [buttons addArrangedSubview:[self button:@"Save and check" filled:YES action:@selector(save)]];
    if (sync.configured) [buttons addArrangedSubview:[self button:@"Turn sync off" filled:NO action:@selector(turnOff)]];
    [buttons addArrangedSubview:[self button:@"Cancel" filled:NO action:@selector(cancel)]];
    [stack setCustomSpacing:18 afterView:row];
    [stack addArrangedSubview:buttons];

    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [scroll.topAnchor constraintEqualToAnchor:safe.topAnchor],
        [scroll.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
        [scroll.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
        [stack.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor constant:16],
        [stack.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor constant:-16],
        [stack.centerXAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.centerXAnchor],
        [stack.widthAnchor constraintLessThanOrEqualToConstant:560],
        [stack.widthAnchor constraintLessThanOrEqualToAnchor:scroll.frameLayoutGuide.widthAnchor constant:-32],
    ]];
    NSLayoutConstraint *wide = [stack.widthAnchor constraintEqualToConstant:560];
    wide.priority = UILayoutPriorityDefaultHigh;
    wide.active = YES;
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(keyboardMoved:) name:UIKeyboardWillChangeFrameNotification object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(keyboardMoved:) name:UIKeyboardWillHideNotification object:nil];
}

// the part of the form the keyboard covers becomes scrollable space, and the field being typed in is scrolled into view
- (void)keyboardMoved:(NSNotification *)note {
    CGFloat covered = 0;
    if (![note.name isEqualToString:UIKeyboardWillHideNotification]) {
        const CGRect keyboard = [self.view convertRect:[note.userInfo[UIKeyboardFrameEndUserInfoKey] CGRectValue] fromCoordinateSpace:UIScreen.mainScreen.coordinateSpace];
        covered = std::max<CGFloat>(0, CGRectGetMaxY(_scroll.frame) - CGRectGetMinY(keyboard));
    }
    UIEdgeInsets inset = _scroll.contentInset;
    inset.bottom = covered;
    _scroll.contentInset = inset;
    _scroll.verticalScrollIndicatorInsets = inset;
    [self revealEditing];
}

- (void)revealEditing {
    for (UITextField *f in @[ _url, _user, _password ])
        if (f.isFirstResponder) [_scroll scrollRectToVisible:CGRectInset([_scroll convertRect:f.bounds fromView:f], 0, -12) animated:YES];
}

- (void)textFieldDidBeginEditing:(UITextField *)f { [self revealEditing]; }

- (BOOL)textFieldShouldReturn:(UITextField *)f {
    if (f == _url) [_user becomeFirstResponder];
    else if (f == _user) [_password becomeFirstResponder];
    else [self save];
    return NO;
}

- (void)finish:(SyncSettingsResult)result {
    void (^done)(SyncSettingsResult) = _done;
    [self.presentingViewController dismissViewControllerAnimated:YES completion:^{
        if (done) done(result);
    }];
}

- (void)save {
    [[SaveSync shared] setURL:_url.text user:_user.text password:_password.text insecure:_insecure.on];
    [self finish:SyncSettingsResult::saved];
}

- (void)turnOff {
    [[SaveSync shared] setURL:@"" user:@"" password:@"" insecure:NO];
    [self finish:SyncSettingsResult::off];
}

- (void)cancel { [self finish:SyncSettingsResult::cancelled]; }

- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }

@end

@interface GameViewController : UIViewController
@end

@implementation GameViewController {
    MetalView *_screen;     // the GPU renderer presents here
    UIView *_picture;       // the CPU renderer's frames
    UILabel *_message, *_toast, *_speed;
    TouchControls *_controls;
    CADisplayLink *_link;
    NSTimer *_discTimer;
    CGColorSpaceRef _rgb;
    BOOL _menuOpen, _active, _syncing, _syncAsking, _stateBusy, _checkingStates;
    NSUInteger _stateJobs;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;
    _active = YES;
    _rgb = CGColorSpaceCreateDeviceRGB();

    _screen = [[MetalView alloc] init];
    CAMetalLayer *metal = (CAMetalLayer *)_screen.layer;
    metal.contentsScale = UIScreen.mainScreen.nativeScale;
    metal.opaque = YES;
    metal.backgroundColor = UIColor.blackColor.CGColor;
    g_layer = (__bridge void *)metal;
    [self.view addSubview:_screen];

    _picture = [[UIView alloc] init];
    _picture.userInteractionEnabled = NO;
    _picture.layer.magnificationFilter = kCAFilterLinear;
    _picture.layer.contentsGravity = kCAGravityResizeAspect;
    [self.view addSubview:_picture];

    _message = [[UILabel alloc] init];
    _message.textColor = UIColor.whiteColor;
    _message.numberOfLines = 0;
    _message.textAlignment = NSTextAlignmentCenter;
    _message.text = @"Put your Persona 3 FES disc image (.iso) in this app's folder:\n"
                    @"Files app > On My iPhone/iPad > P3FES, or Finder > your device > Files > P3FES.\n"
                    @"The game starts as soon as it is there.\n\n"
                    @"To sync saves with your own WebDAV server, tap MENU (bottom left) > Save sync.";
    [self.view addSubview:_message];

    _controls = [[TouchControls alloc] initWithFrame:self.view.bounds];
    NSNumber *opacity = [NSUserDefaults.standardUserDefaults objectForKey:kOpacityKey];
    _controls.opacity = opacity ? opacity.doubleValue : 0.3;
    __weak GameViewController *weakSelf = self;
    _controls.onMenu = ^{ [weakSelf showMenu]; };
    [self.view addSubview:_controls];

    _speed = [[UILabel alloc] init];
    _speed.textColor = UIColor.whiteColor;
    _speed.font = [UIFont monospacedDigitSystemFontOfSize:12 weight:UIFontWeightMedium];
    _speed.backgroundColor = [UIColor colorWithWhite:0 alpha:0.5];
    _speed.textAlignment = NSTextAlignmentCenter;
    _speed.layer.cornerRadius = 6;
    _speed.layer.masksToBounds = YES;
    _speed.userInteractionEnabled = NO;
    _speed.hidden = ![NSUserDefaults.standardUserDefaults boolForKey:kSpeedKey];
    [self.view addSubview:_speed];

    _toast = [[UILabel alloc] init];
    _toast.textColor = UIColor.whiteColor;
    _toast.font = [UIFont systemFontOfSize:13 weight:UIFontWeightMedium];
    _toast.numberOfLines = 0;
    _toast.textAlignment = NSTextAlignmentCenter;
    _toast.backgroundColor = [UIColor colorWithWhite:0 alpha:0.6];
    _toast.layer.cornerRadius = 10;
    _toast.layer.masksToBounds = YES;
    _toast.alpha = 0;
    _toast.userInteractionEnabled = NO;
    [self.view addSubview:_toast];

    SaveSync *sync = [SaveSync shared];
    sync.cardPath = @((g_documents + "/memcard.ps2").c_str());
    sync.statesDir = @(states_dir().c_str());
    sync.presenter = self;
    sync.status = ^(NSString *message) { [weakSelf toast:message]; };
    sync.asking = ^(BOOL asking) { [weakSelf syncAsking:asking]; };

    _link = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick)];
    _link.preferredFrameRateRange = CAFrameRateRangeMake(60, 60, 60);
    [_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];

    NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
    [nc addObserver:self selector:@selector(controllersChanged) name:GCControllerDidConnectNotification object:nil];
    [nc addObserver:self selector:@selector(controllersChanged) name:GCControllerDidDisconnectNotification object:nil];
    [nc addObserver:self selector:@selector(becameActive) name:UIApplicationDidBecomeActiveNotification object:nil];
    [nc addObserver:self selector:@selector(resignActive) name:UIApplicationWillResignActiveNotification object:nil];
    [nc addObserver:self selector:@selector(enteredBackground) name:UIApplicationDidEnterBackgroundNotification object:nil];
    [nc addObserver:self selector:@selector(enteringForeground) name:UIApplicationWillEnterForegroundNotification object:nil];
    [nc addObserver:self selector:@selector(audioInterrupted:) name:AVAudioSessionInterruptionNotification object:nil];
    [nc addObserver:self selector:@selector(applyScreen) name:@"P3FESScreen" object:nil];
    [self controllersChanged];

    _discTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 target:self selector:@selector(lookForDisc) userInfo:nil repeats:YES];
    dispatch_async(dispatch_get_main_queue(), ^{ [self lookForDisc]; });   // after the view is on screen, so a sync question can show
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    const CGRect bounds = self.view.bounds;
    // the Metal view covers the screen and the runtime fits the picture in it (4:3, or wider with the widescreen patch)
    _screen.frame = bounds;
    const CGFloat scale = ((CAMetalLayer *)_screen.layer).contentsScale;
    g_layer_w = static_cast<std::uint32_t>(bounds.size.width * scale);
    g_layer_h = static_cast<std::uint32_t>(bounds.size.height * scale);
    const CGRect safe = UIEdgeInsetsInsetRect(bounds, self.view.safeAreaInsets);
    _picture.frame = safe;
    _message.frame = CGRectInset(safe, 40, 24);
    _controls.frame = bounds;
    _speed.frame = CGRectMake(safe.origin.x + 8, safe.origin.y + 6, 92, 20);
    [self placeToast];
    [self applyScreen];
}

- (Screen)screen {
    NSNumber *saved = [NSUserDefaults.standardUserDefaults objectForKey:kScreenKey];
    const int v = saved ? saved.intValue : static_cast<int>(Screen::wide);
    return v >= 0 && v <= 2 ? static_cast<Screen>(v) : Screen::wide;
}

// the picture's shape for the Screen setting; full screen is the shape of the whole display, notch area included
- (void)applyScreen {
    const CGSize size = self.view.bounds.size;
    float aspect = 4.0f / 3.0f;
    switch ([self screen]) {
    case Screen::classic: break;
    case Screen::wide:   // never wider than the screen: on an iPad (about 1.43:1) Wide and Full screen are the same
        aspect = kWideAspect;
        if (size.width > 0 && size.height > 0) aspect = std::min(aspect, static_cast<float>(size.width / size.height));
        break;
    case Screen::full:
        if (size.width > 0 && size.height > 0) aspect = static_cast<float>(size.width / size.height);
        break;
    }
    set_picture_aspect(std::clamp(aspect, 4.0f / 3.0f, 2.5f));
}

- (void)showScale {
    UIAlertController *a = [UIAlertController
        alertControllerWithTitle:@"Resolution"
                         message:[NSString stringWithFormat:@"%dx matches this screen. Each step needs much more of the GPU: "
                                                            @"if the game slows down, go back one. Used from the next start.", native_scale()]
                  preferredStyle:UIAlertControllerStyleAlert];
    const int now = chosen_scale(), native = native_scale();
    for (int v = 1; v <= 4; ++v) {
        NSString *title = [NSString stringWithFormat:@"%dx (%dx%d)%@%@", v, 640 * v, 448 * v, v == kDefaultScale ? @", default" : @"",
                                                     v == native ? @", this screen" : @""];
        if (v == now) title = [title stringByAppendingString:@"  \u2713"];
        [a addAction:[self action:title style:UIAlertActionStyleDefault run:^{
               [NSUserDefaults.standardUserDefaults setInteger:v forKey:kScaleKey];
               [self menuClosed];
               if (g_started.load() && chosen_scale() != g_scale_running)
                   [self toast:@"The new resolution is used the next time the app starts. Close it from the app switcher to restart now."];
           }]];
    }
    [a addAction:[self action:@"Cancel" style:UIAlertActionStyleCancel run:^{ [self menuClosed]; }]];
    [self presentViewController:a animated:YES completion:nil];
}

- (void)showScreen {
    UIAlertController *a = [UIAlertController
        alertControllerWithTitle:@"Screen"
                         message:@"How much of the screen the 3D scenes fill. Menus, text and movies keep their 4:3 place. "
                                 @"In Full screen a scene that waits for someone to walk out of view may get stuck: "
                                 @"pick Wide or 4:3 until it has passed."
                  preferredStyle:UIAlertControllerStyleAlert];
    const Screen now = [self screen];
    const struct { Screen screen; NSString *title; } choices[] = {
        {Screen::classic, @"4:3 (as on the PS2)"}, {Screen::wide, @"Wide, 16:9 (default)"}, {Screen::full, @"Full screen"}};
    for (const auto &choice : choices) {
        const Screen pick = choice.screen;
        NSString *title = pick == now ? [choice.title stringByAppendingString:@"  \u2713"] : choice.title;
        [a addAction:[self action:title style:UIAlertActionStyleDefault run:^{
               [NSUserDefaults.standardUserDefaults setInteger:static_cast<int>(pick) forKey:kScreenKey];
               [self applyScreen];
               [self menuClosed];
           }]];
    }
    [a addAction:[self action:@"Cancel" style:UIAlertActionStyleCancel run:^{ [self menuClosed]; }]];
    [self presentViewController:a animated:YES completion:nil];
}

- (void)placeToast {
    const CGRect safe = UIEdgeInsetsInsetRect(self.view.bounds, self.view.safeAreaInsets);
    const CGFloat w = std::min<CGFloat>(460, safe.size.width - 180);
    const CGSize fit = [_toast sizeThatFits:CGSizeMake(w - 24, 200)];
    _toast.frame = CGRectMake(CGRectGetMidX(safe) - (fit.width + 24) / 2, safe.origin.y + 10, fit.width + 24, fit.height + 14);
}

- (void)toast:(NSString *)message {
    _toast.text = message;
    [self placeToast];
    [_toast.layer removeAllAnimations];
    _toast.alpha = 1;
    [UIView animateWithDuration:0.5 delay:3.5 options:UIViewAnimationOptionBeginFromCurrentState animations:^{ self->_toast.alpha = 0; } completion:nil];
}

- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures { return UIRectEdgeAll; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }

- (void)lookForDisc {
    if (_syncing || g_started.load() || _menuOpen) return;
    const std::string iso = find_disc();
    if (iso.empty()) return;
    [_discTimer invalidate];
    _discTimer = nil;
    _syncing = YES;
    _message.text = @"Syncing saves with your server...";
    [[SaveSync shared] syncAtStart:^{
        self->_syncing = NO;
        self->_message.hidden = YES;
        start_audio();
        g_scale_running = chosen_scale();
        start_game(iso, g_scale_running);
        // test switch: P3FES_TEST_STATE=<path> loads that save state a few seconds after the game starts
        if (const char *test = std::getenv("P3FES_TEST_STATE")) {
            const std::string path = test;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 4 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
                [self runState:YES slot:0 path:path name:@"the test state"];
            });
        }
    }];
}

// ---- pause: while the menu is open, and while the app is not in front

- (void)updatePause { set_paused(_menuOpen || _syncAsking || _stateBusy || !_active); }

- (void)syncAsking:(BOOL)asking {
    _syncAsking = asking;
    [self updatePause];
}

- (void)becameActive {
    _active = YES;
    [self updatePause];
    if (g_started.load()) start_audio();
}

- (void)resignActive {
    _active = NO;
    [self updatePause];
    [_controls releaseAll];
    [[SaveSync shared] uploadNow];
}

- (void)enteredBackground { g_foreground = false; }
- (void)enteringForeground { g_foreground = true; }

- (void)audioInterrupted:(NSNotification *)note {
    if ([note.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue] == AVAudioSessionInterruptionTypeEnded && g_started.load()) start_audio();
}

// a real controller hides the on-screen controls, all but the menu button
- (void)controllersChanged {
    bool physical = false;
    for (GCController *c in GCController.controllers)
        if (c.extendedGamepad && ![c.vendorName containsString:@"Touch Controller"] && ![c.productCategory containsString:@"Touch Controller"]) {
            physical = true;
            std::fprintf(stderr, "[ios] controller: %s\n", c.vendorName.UTF8String ?: "?");
        }
    if (std::getenv("P3FES_TOUCH")) physical = false;   // testing: keep the on-screen controls even with a real controller
    _controls.menuOnly = physical;
}

// ---- menu

- (void)present:(UIAlertController *)alert {
    _menuOpen = YES;
    [self updatePause];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)menuClosed {
    _menuOpen = NO;
    [self updatePause];
    if (!g_started.load() && !_syncing && !_discTimer)
        _discTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 target:self selector:@selector(lookForDisc) userInfo:nil repeats:YES];
}

- (UIAlertAction *)action:(NSString *)title style:(UIAlertActionStyle)style run:(void (^)(void))run {
    return [UIAlertAction actionWithTitle:title style:style handler:^(UIAlertAction *) { run(); }];
}

- (void)showMenu {
    if (self.presentedViewController || _checkingStates || _stateBusy) return;
    SaveSync *sync = [SaveSync shared];
    NSString *state = sync.configured ? [NSString stringWithFormat:@"Save sync: %@", sync.fileURL.host] : @"Save sync: off";
    UIAlertController *menu = [UIAlertController alertControllerWithTitle:g_started.load() ? @"Paused" : @"P3FES" message:state
                                                           preferredStyle:UIAlertControllerStyleAlert];
    [menu addAction:[self action:[NSString stringWithFormat:@"On-screen controls: %d%%", static_cast<int>(std::lround(_controls.opacity * 100))]
                           style:UIAlertActionStyleDefault
                             run:^{ [self showOpacity]; }]];
    static NSString *const kScreenNames[] = {@"4:3", @"Wide", @"Full screen"};
    [menu addAction:[self action:[NSString stringWithFormat:@"Screen: %@", kScreenNames[static_cast<int>([self screen])]]
                           style:UIAlertActionStyleDefault
                             run:^{ [self showScreen]; }]];
    [menu addAction:[self action:[NSString stringWithFormat:@"Resolution: %dx", chosen_scale()]
                           style:UIAlertActionStyleDefault
                             run:^{ [self showScale]; }]];
    const BOOL speed = [NSUserDefaults.standardUserDefaults boolForKey:kSpeedKey];
    [menu addAction:[self action:speed ? @"Show speed: on" : @"Show speed: off"
                           style:UIAlertActionStyleDefault
                             run:^{
                                 [NSUserDefaults.standardUserDefaults setBool:!speed forKey:kSpeedKey];
                                 self->_speed.hidden = speed;
                                 [self menuClosed];
                             }]];
    [menu addAction:[self action:@"Save sync settings..." style:UIAlertActionStyleDefault run:^{ [self showSyncSettings:nil]; }]];
    if (g_started.load()) {
        [menu addAction:[self action:@"Save state..." style:UIAlertActionStyleDefault run:^{ [self showStates:NO]; }]];
        [menu addAction:[self action:@"Load state..." style:UIAlertActionStyleDefault run:^{ [self showStates:YES]; }]];
    }
    if (sync.configured && g_started.load()) {
        [menu addAction:[self action:@"Upload save now" style:UIAlertActionStyleDefault run:^{
                  [self menuClosed];
                  [sync uploadNow];
              }]];
        [menu addAction:[self action:@"Use the server's save (closes the app)" style:UIAlertActionStyleDestructive run:^{
                  [self menuClosed];
                  [sync useServerCopyAndQuit];
              }]];
    }
    [menu addAction:[self action:g_started.load() ? @"Resume" : @"Close" style:UIAlertActionStyleCancel run:^{ [self menuClosed]; }]];
    [self present:menu];
}

// ---- save states

// with sync on, the lists first ask the server which slots it has a newer copy of (from another device)
- (void)showStates:(BOOL)load {
    SaveSync *sync = [SaveSync shared];
    if (!sync.configured) return [self showStates:load onServer:@{}];
    _checkingStates = YES;
    [self toast:@"Checking your server..."];
    [sync checkStates:^(NSDictionary<NSNumber *, NSDate *> *onServer) {
        self->_checkingStates = NO;
        [self showStates:load onServer:onServer];
    }];
}

- (void)showStates:(BOOL)load onServer:(NSDictionary<NSNumber *, NSDate *> *)onServer {
    NSDateFormatter *when = [[NSDateFormatter alloc] init];
    when.dateStyle = NSDateFormatterShortStyle;
    when.timeStyle = NSDateFormatterShortStyle;
    UIAlertController *a = [UIAlertController
        alertControllerWithTitle:load ? @"Load state" : @"Save state"
                         message:load ? @"The game goes back to the moment the state was saved. The memory card stays as it is."
                                      : @"Keeps the whole game as it is now. Saving in a slot replaces what it held."
                  preferredStyle:UIAlertControllerStyleAlert];
    for (int slot = 1; slot <= kStateSlots; ++slot) {
        NSDate *saved = state_date(slot_path(slot)), *server = onServer[@(slot)];
        NSString *name = [NSString stringWithFormat:@"slot %d", slot];
        NSString *title = server ? [NSString stringWithFormat:@"Slot %d: %@ (on your server)", slot, [when stringFromDate:server]]
                                 : [NSString stringWithFormat:@"Slot %d: %@", slot, saved ? [when stringFromDate:saved] : @"empty"];
        UIAlertAction *pick = [self action:title style:UIAlertActionStyleDefault run:^{
            if (load && server) [self fetchState:slot];
            else [self runState:load slot:slot path:slot_path(slot) name:name];
        }];
        pick.enabled = saved || server || !load;
        [a addAction:pick];
    }
    if (load)
        if (NSDate *saved = state_date(undo_path()))
            [a addAction:[self action:[NSString stringWithFormat:@"Undo the last load (back to %@)", [when stringFromDate:saved]]
                                style:UIAlertActionStyleDefault
                                  run:^{ [self runState:YES slot:0 path:undo_path() name:@"the game from before the last load"]; }]];
    [a addAction:[self action:@"Cancel" style:UIAlertActionStyleCancel run:^{ [self menuClosed]; }]];
    UIViewController *top = self;
    while (top.presentedViewController) top = top.presentedViewController;
    [top presentViewController:a animated:YES completion:nil];
}

// a slot whose newest copy is on the server: download it, then load it (the game stays paused meanwhile)
- (void)fetchState:(int)slot {
    _stateBusy = YES;
    _menuOpen = NO;
    [self updatePause];
    [self toast:[NSString stringWithFormat:@"Downloading slot %d from your server...", slot]];
    [[SaveSync shared] fetchState:slot done:^(NSString *problem) {
        if (problem) {
            self->_stateBusy = NO;
            [self updatePause];
            return [self toast:[NSString stringWithFormat:@"Slot %d couldn't be downloaded: %@.", slot, problem]];
        }
        [self runState:YES slot:slot path:slot_path(slot) name:[NSString stringWithFormat:@"slot %d", slot]];
    }];
}

// the game stays paused until the state is saved or loaded; a saved slot then goes to the server too
- (void)runState:(BOOL)load slot:(int)slot path:(std::string)path name:(NSString *)name {
    _stateBusy = YES;
    _menuOpen = NO;
    [self updatePause];
    [self toast:load ? @"Loading..." : @"Saving..."];
    const NSUInteger job = ++_stateJobs;
    __weak GameViewController *weakSelf = self;
    request_state(load, path, name, ^(NSString *message, bool ok) {
        GameViewController *me = weakSelf;
        if (!me || me->_stateJobs != job) return;
        me->_stateBusy = NO;
        [me updatePause];
        [me toast:message];
        if (ok && !load && slot) [[SaveSync shared] stateSaved:slot];
    });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        GameViewController *me = weakSelf;
        if (!me || me->_stateJobs != job || !me->_stateBusy || !withdraw_state_request()) return;
        me->_stateBusy = NO;
        [me updatePause];
        [me toast:@"The game didn't respond, so nothing was saved or loaded."];
    });
}

- (void)showOpacity {
    UIAlertController *a = [UIAlertController alertControllerWithTitle:@"On-screen controls"
                                                               message:@"How visible the controls are when not pressed."
                                                        preferredStyle:UIAlertControllerStyleAlert];
    for (const int percent : {10, 20, 30, 45, 60, 80}) {
        NSString *title = [NSString stringWithFormat:@"%d%%%@", percent, percent == 30 ? @" (default)" : @""];
        [a addAction:[self action:title style:UIAlertActionStyleDefault run:^{
               self->_controls.opacity = percent / 100.0;
               [NSUserDefaults.standardUserDefaults setDouble:percent / 100.0 forKey:kOpacityKey];
               [self menuClosed];
           }]];
    }
    [a addAction:[self action:@"Cancel" style:UIAlertActionStyleCancel run:^{ [self menuClosed]; }]];
    [self presentViewController:a animated:YES completion:nil];
}

- (void)showSyncSettings:(NSString *)note {
    SyncSettingsController *form = [[SyncSettingsController alloc] init];
    form.note = note;
    __weak GameViewController *weakSelf = self;
    form.done = ^(SyncSettingsResult result) {
        GameViewController *me = weakSelf;
        SaveSync *sync = [SaveSync shared];
        if (result == SyncSettingsResult::cancelled) return [me menuClosed];
        if (result == SyncSettingsResult::off) {
            [me toast:@"Save sync is off."];
            return [me menuClosed];
        }
        if (!sync.configured) return [me showSyncSettings:@"That address doesn't look right. It should start with https:// or http://."];
        [me toast:@"Checking the server..."];
        [sync test:^(NSString *message) { [me syncChecked:message]; }];
    };
    _menuOpen = YES;
    [self updatePause];
    UIViewController *top = self;
    while (top.presentedViewController) top = top.presentedViewController;
    [top presentViewController:form animated:YES completion:nil];
}

- (void)syncChecked:(NSString *)result {
    SaveSync *sync = [SaveSync shared];
    const bool ok = [result hasPrefix:@"Connected"];
    const bool server_has_card = [result hasPrefix:@"Connected. The server has"];
    UIAlertController *a = [UIAlertController alertControllerWithTitle:ok ? @"Save sync is on" : @"Save sync" message:result
                                                        preferredStyle:UIAlertControllerStyleAlert];
    if (!ok) {
        [a addAction:[self action:@"Change settings" style:UIAlertActionStyleDefault run:^{ [self showSyncSettings:nil]; }]];
        [a addAction:[self action:@"Close" style:UIAlertActionStyleCancel run:^{ [self menuClosed]; }]];
    } else if (!g_started.load()) {
        a.message = [result stringByAppendingString:@"\n\nThe saves are synced each time the game starts."];
        [a addAction:[self action:@"OK" style:UIAlertActionStyleCancel run:^{ [self menuClosed]; }]];
    } else if (server_has_card) {
        a.message = [result stringByAppendingString:@"\n\nThe game is already running with this device's save. Play with the server's save "
                                                    @"instead (the app closes; start it again), or send this device's save to the server?"];
        [a addAction:[self action:@"Use the server's save (closes the app)" style:UIAlertActionStyleDestructive run:^{
               [self menuClosed];
               [sync useServerCopyAndQuit];
           }]];
        [a addAction:[self action:@"Upload this device's save" style:UIAlertActionStyleDefault run:^{
               [self menuClosed];
               [sync uploadNow];
           }]];
        [a addAction:[self action:@"Decide later" style:UIAlertActionStyleCancel run:^{ [self menuClosed]; }]];
    } else {
        a.message = [result stringByAppendingString:@"\n\nThis device's save is uploaded now, and after every save in the game."];
        [a addAction:[self action:@"OK" style:UIAlertActionStyleCancel run:^{
               [self menuClosed];
               [sync uploadNow];
           }]];
    }
    UIViewController *top = self;
    while (top.presentedViewController) top = top.presentedViewController;
    _menuOpen = YES;
    [self updatePause];
    [top presentViewController:a animated:YES completion:nil];
}

// ---- every screen refresh: input, the speed in the log, and the CPU renderer's newest frame

- (void)tick {
    poll_pads();
    static CFTimeInterval since = 0;
    static std::uint32_t presented_before = 0;
    static int shown = 0;
    const CFTimeInterval now = CACurrentMediaTime();
    if (!since) since = now;
    static CFTimeInterval speed_since = 0;
    static std::uint32_t speed_before = 0;
    if (!speed_since) speed_since = now;
    if (now - speed_since >= 1.0) {   // Show speed
        const std::uint32_t presented = g_presented.load();
        if (!_speed.hidden && g_started.load() && g_gpu_present.load() && !g_paused.load())
            _speed.text = [NSString stringWithFormat:@"%.0f fps, %dx", (presented - speed_before) / (now - speed_since), g_scale_running];
        speed_before = presented;
        speed_since = now;
    }
    if (now - since >= 5.0) {
        const std::uint32_t presented = g_presented.load();
        if (g_started.load() && !g_paused.load())
            std::fprintf(stderr, "[ios] %.1f fps shown, sound queued %zu ms, GPU %.1f ms, %dx\n",
                         (g_gpu_present.load() ? presented - presented_before : shown) / (now - since), queued_frames() / 48,
                         g_gpu_present.load() ? ps2::gpu_busy_ms() : 0.0, g_scale_running);
        presented_before = presented;
        since = now;
        shown = 0;
    }
    if (g_gpu_present.load()) return;
    std::vector<std::uint32_t> pixels;
    std::uint32_t w = 0, h = 0;
    {
        std::lock_guard<std::mutex> lock(g_frame_lock);
        if (!g_frame_new) return;
        g_frame_new = false;
        pixels.swap(g_frame);
        w = g_frame_w;
        h = g_frame_h;
    }
    if (!w || !h || pixels.size() < static_cast<std::size_t>(w) * h) return;
    NSData *data = [NSData dataWithBytes:pixels.data() length:pixels.size() * 4];
    CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
    // the runtime's pixels are R, G, B, A bytes; the alpha byte is not used for the picture
    CGImageRef image = CGImageCreate(w, h, 8, 32, w * 4, _rgb, static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipLast), provider, nullptr, false,
                                     kCGRenderingIntentDefault);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _picture.layer.contents = (__bridge id)image;
    ++shown;
    [CATransaction commit];
    CGImageRelease(image);
    CGDataProviderRelease(provider);
}

@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation AppDelegate
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options {
    NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    g_documents = docs.UTF8String;
    chdir(g_documents.c_str());   // quick saves, RAM dumps and other files the runtime writes go next to the disc
    // the runtime logs to stderr: keep a copy in Documents/log.txt so it can be read from the Files app
    std::freopen((g_documents + "/log.txt").c_str(), "w", stderr);
    setvbuf(stderr, nullptr, _IOLBF, 0);
    application.idleTimerDisabled = YES;
    return YES;
}

- (UISceneConfiguration *)application:(UIApplication *)application
    configurationForConnectingSceneSession:(UISceneSession *)session
                                   options:(UISceneConnectionOptions *)options {
    UISceneConfiguration *config = [[UISceneConfiguration alloc] initWithName:@"Game" sessionRole:session.role];
    config.delegateClass = NSClassFromString(@"SceneDelegate");
    return config;
}
@end

// iOS 27 requires the scene life cycle: the one window lives in the scene
@interface SceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end

@implementation SceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = [[GameViewController alloc] init];
    [self.window makeKeyAndVisible];
}
@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(AppDelegate.class));
    }
}
