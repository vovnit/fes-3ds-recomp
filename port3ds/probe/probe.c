// Reports what this console (or emulator) actually grants, and a rough CPU speed.
// Writes sdmc:/p3fes/probe.txt and prints the same line to the debug log.
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// libctru would otherwise take all free memory as heap, which a real console refuses: the heap area ends at 0x0E000000.
u32 __ctru_heap_size = 16 * 1024 * 1024;
u32 __ctru_linear_heap_size = 8 * 1024 * 1024;

static volatile int g_core2_ran = 0;

static void on_core2(void *arg) {
    (void)arg;
    g_core2_ran = 1 + svcGetProcessorID();
}

int main(void) {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    osSetSpeedupEnable(true);
    mkdir("sdmc:/p3fes", 0777);
    FILE *f = fopen("sdmc:/p3fes/probe.txt", "w");
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    s64 used = 0;
    svcGetSystemInfo(&used, 0, 1);
    const u32 app = osGetMemRegionSize(MEMREGION_APPLICATION);
    const u32 free_now = osGetMemRegionFree(MEMREGION_APPLICATION);
    // The same request the game makes for the PS2 RAM.
    u32 mapped = 0;
    const Result ram = svcControlMemory(&mapped, 0x0B000000u, 0, (32u << 20) + 0x1000u, MEMOP_ALLOC,
                                        (MemPerm)(MEMPERM_READ | MEMPERM_WRITE));
    Thread other = threadCreate(on_core2, NULL, 16 * 1024, 0x30, 2, false);
    if (other) {
        threadJoin(other, 1000000000ull);
        threadFree(other);
    }
    const u64 t0 = svcGetSystemTick();
    volatile u32 x = 1;
    for (u32 i = 0; i < 100000000u; ++i) x = x * 3u + i;
    const u64 t1 = svcGetSystemTick();
    volatile float fx = 1.0f;
    for (u32 i = 0; i < 20000000u; ++i) fx = fx * 0.999f + 0.5f;
    const u64 t2 = svcGetSystemTick();
    char line[256];
    snprintf(line, sizeof line, "new3ds=%d appmem_mb=%lu free_mb=%lu used_kb=%lld heap_mb=%lu ram_rc=%08lX ram_at=%08lX core2=%d int100M_ms=%llu fp20M_ms=%llu core=%d\n",
             n3ds, app >> 20, free_now >> 20, used >> 10, (u32)(envGetHeapSize() >> 20), ram, mapped, g_core2_ran,
             (t1 - t0) / (u64)CPU_TICKS_PER_MSEC, (t2 - t1) / (u64)CPU_TICKS_PER_MSEC, svcGetProcessorID());
    puts(line);
    svcOutputDebugString(line, strlen(line));
    if (f) {
        fputs(line, f);
        fclose(f);
    } else {
        puts("sd write FAILED");
    }
    for (int i = 0; i < 300 && aptMainLoop(); ++i) {
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    gfxExit();
    return 0;
}
