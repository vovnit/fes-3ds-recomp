#include <3ds.h>
#include <citro3d.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>

extern "C" {
volatile int g_fes_trace = 0;   // where the drawing thread is, for the stall report in main3ds.cpp
}
#define FES_AT(n) (g_fes_trace = (n))

namespace {

constexpr u8 kProcessCommandList = 1;

gxCmdQueue_s *display_queue = nullptr;
void (*display_callback)(gxCmdQueue_s *);
LightEvent display_finished;
std::atomic<bool> awaiting_display{false};

gxCmdEntry_s recorded_entries[32];
gxCmdQueue_s recording_queue = [] {
    gxCmdQueue_s queue{};
    queue.entries = recorded_entries;
    queue.maxEntries = sizeof recorded_entries / sizeof *recorded_entries;
    return queue;
}();

thread_local bool bypass_begin_wait = false;
unsigned submitted_halves = 3;
u32 *command_memory = nullptr;
u32 *recording_begin = nullptr;
u32 recording_words = 0;
u32 original_words = 0;
u32 peak_words = 0;
unsigned expansions = 0;
u64 wait_ticks = 0;

void frame_finished(gxCmdQueue_s *queue) {
    display_callback(queue);
    LightEvent_Signal(&display_finished);
}

}

extern "C" bool __real_gxCmdQueueWait(gxCmdQueue_s *, s64);
extern "C" void __real_gxCmdQueueStop(gxCmdQueue_s *);
extern "C" void __real_gxCmdQueueClear(gxCmdQueue_s *);
extern "C" void __real_gxCmdQueueRun(gxCmdQueue_s *);
extern "C" void __real_GPUCMD_Add(u32, const u32 *, u32);
extern "C" void __real_GPUCMD_AddRawCommands(const u32 *, u32);
extern "C" void __real_GPUCMD_Split(u32 **, u32 *);

extern "C" bool __wrap_gxCmdQueueWait(gxCmdQueue_s *queue, s64 timeout) {
    if (queue == display_queue) {
        if (bypass_begin_wait) return true;
        if (awaiting_display && timeout < 0) { FES_AT(60); LightEvent_Wait(&display_finished); }
    }
    const int was = g_fes_trace;
    FES_AT(61);
    const bool done = __real_gxCmdQueueWait(queue, timeout);
    FES_AT(was);
    return done;
}

extern "C" void __wrap_gxCmdQueueStop(gxCmdQueue_s *queue) {
    if (!bypass_begin_wait || queue != display_queue) __real_gxCmdQueueStop(queue);
}

extern "C" void __wrap_gxCmdQueueClear(gxCmdQueue_s *queue) {
    if (!bypass_begin_wait || queue != display_queue) __real_gxCmdQueueClear(queue);
}

extern "C" void __wrap_gxCmdQueueRun(gxCmdQueue_s *queue) {
    if ((!display_queue || display_queue == queue) && queue->callback) {
        if (queue->callback != frame_finished) {
            display_queue = queue;
            display_callback = queue->callback;
            queue->callback = frame_finished;
        }
        awaiting_display = queue->numEntries != queue->lastEntry;
        if (awaiting_display) LightEvent_Clear(&display_finished);
    }
    __real_gxCmdQueueRun(queue);
}

extern "C" bool FesOverlapInit(std::size_t bytes) {
    LightEvent_Init(&display_finished, RESET_STICKY);
    return display_queue && !(bytes & 63);
}

extern "C" void FesOverlapWait() {
    if (!awaiting_display) return;
    const u64 start = svcGetSystemTick();
    const int was = g_fes_trace;
    FES_AT(40);
    LightEvent_Wait(&display_finished);
    FES_AT(41);
    __real_gxCmdQueueWait(display_queue, -1);
    FES_AT(was);
    __real_gxCmdQueueStop(display_queue);
    __real_gxCmdQueueClear(display_queue);
    awaiting_display = false;
    wait_ticks += svcGetSystemTick() - start;
}

extern "C" u64 FesOverlapWaitTicks() { return wait_ticks; }

// For the stall report in main3ds.cpp: the display queue's state and the GX commands it holds (type, first three arguments).
extern "C" void FesOverlapDescribe(char *out, std::size_t size) {
    if (!display_queue) {
        std::snprintf(out, size, "no display queue");
        return;
    }
    const gxCmdQueue_s &q = *display_queue;
    int at = std::snprintf(out, size, "queue %u entries, %u sent, %u done, awaiting %d, halves %u:", q.numEntries, q.curEntry, q.lastEntry,
                           awaiting_display.load() ? 1 : 0, submitted_halves);
    for (unsigned i = 0; i < q.numEntries && at > 0 && static_cast<std::size_t>(at) < size; ++i) {
        const gxCmdEntry_s &e = q.entries[i];
        at += std::snprintf(out + at, size - at, " [%u %08lX %08lX %08lX]", e.type, static_cast<unsigned long>(e.args[0]),
                            static_cast<unsigned long>(e.args[1]), static_cast<unsigned long>(e.args[2]));
    }
}

extern "C" bool FesOverlapBegin(bool wait_for_geometry, bool force_small) {
    if (!display_queue || display_queue->callback != frame_finished) return false;
    if (wait_for_geometry || submitted_halves == 3) FesOverlapWait();
    // Citro3D opens its CPU state while the previous display queue keeps running.
    bypass_begin_wait = true;
    FES_AT(16);
    const bool opened = C3D_FrameBegin(0);
    FES_AT(17);
    bypass_begin_wait = false;
    if (!opened) return false;

    command_memory = gpuCmdBuf;
    original_words = gpuCmdBufSize;
    const u32 half = original_words / 2;
    recording_begin = command_memory + (submitted_halves == 1 ? half : 0);
    recording_words = force_small ? 3072u : half;
    __real_gxCmdQueueClear(&recording_queue);
    GX_BindQueue(&recording_queue);
    GPUCMD_SetBuffer(recording_begin, recording_words, 0);
    return true;
}

namespace {

void expand_recording() {
    if (!recording_begin || recording_words == original_words) return;
    FesOverlapWait();
    ++expansions;
    const std::size_t completed = gpuCmdBuf - recording_begin;
    if (recording_begin != command_memory) {
        std::memcpy(command_memory, recording_begin, (completed + gpuCmdBufOffset) * sizeof(u32));
        for (unsigned i = 0; i < recording_queue.numEntries; ++i) {
            auto &entry = recorded_entries[i];
            if (entry.type == kProcessCommandList) {
                const std::size_t offset = reinterpret_cast<u32 *>(entry.args[0]) - recording_begin;
                entry.args[0] = reinterpret_cast<u32>(command_memory + offset);
            }
        }
    }
    recording_begin = command_memory;
    recording_words = original_words;
    GPUCMD_SetBuffer(recording_begin + completed, recording_words - completed, gpuCmdBufOffset);
}

}

extern "C" void __wrap_GPUCMD_Add(u32 header, const u32 *params, u32 words) {
    if (gpuCmdBufSize - gpuCmdBufOffset < words + (words >> 7) + 8) expand_recording();
    __real_GPUCMD_Add(header, params, words);
}

extern "C" void __wrap_GPUCMD_AddRawCommands(const u32 *commands, u32 words) {
    if (gpuCmdBufSize - gpuCmdBufOffset < words + 4) expand_recording();
    __real_GPUCMD_AddRawCommands(commands, words);
}

extern "C" void __wrap_GPUCMD_Split(u32 **buffer, u32 *words) {
    if (gpuCmdBufSize - gpuCmdBufOffset < 4) expand_recording();
    __real_GPUCMD_Split(buffer, words);
}

extern "C" void FesOverlapEnd(u8 flags) {
    if (!recording_begin) {
        submitted_halves = 3;
        FES_AT(53);
        C3D_FrameEnd(flags);
        FES_AT(54);
        return;
    }
    C3D_FrameSplit(flags);
    const u32 words = gpuCmdBuf - recording_begin;
    const u32 half = original_words / 2;
    peak_words = std::max(peak_words, words);
    submitted_halves = recording_begin != command_memory ? 2 : (words > half ? 3 : 1);

    const bool queue_has_finished = !awaiting_display;
    FesOverlapWait();
    if (queue_has_finished) {
        // A frame without transfers can leave the display queue armed while idle.
        __real_gxCmdQueueStop(display_queue);
        __real_gxCmdQueueClear(display_queue);
    }
    GX_BindQueue(display_queue);
    // Preserve the order of clears, copies and draw lists before Citro3D
    // appends the LCD transfers and starts the display queue.
    for (unsigned i = 0; i < recording_queue.numEntries; ++i) {
        auto &entry = recorded_entries[i];
        if (entry.type == kProcessCommandList) entry.args[6] = 1; // Flush the list before GX reads it.
        gxCmdQueueAdd(display_queue, &entry);
    }
    recording_begin = nullptr;
    FES_AT(51);
    C3D_FrameEnd(flags);
    FES_AT(52);
}

extern "C" void FesOverlapFlushMidFrame() {
    FesOverlapEnd(GX_CMDLIST_FLUSH);
    FesOverlapWait();
    submitted_halves = 3;
    FES_AT(38);
    C3D_FrameBegin(0);
    FES_AT(39);
}

// Command words still free this frame, counting the growth expand_recording can still give the recording.
extern "C" u32 FesOverlapWordsLeft() {
    if (recording_begin && recording_queue.numEntries >= 28) return 0;
    if (!recording_begin && display_queue && display_queue->numEntries >= 28) return 0;
    u32 left = gpuCmdBufSize - gpuCmdBufOffset;
    if (recording_begin && recording_words != original_words) left += original_words - recording_words;
    return left;
}

extern "C" void FesOverlapCommandStats(u32 *peak, unsigned *expanded) {
    *peak = peak_words;
    *expanded = expansions;
    peak_words = 0;
    expansions = 0;
}
