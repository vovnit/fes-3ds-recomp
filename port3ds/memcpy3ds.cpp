// memcpy and memset for the ARM11. The library's take 14 instructions per 16 bytes, and 4 per byte when the two addresses are not
// both word-aligned; the game moves several megabytes per picture through them. This one uses load and store multiple, about ten
// times less. The compiler must not turn the loops below back into calls to these same functions.
#include <cstddef>
#include <cstdint>

namespace {
using word = std::uint32_t __attribute__((aligned(1), may_alias));
}

extern "C" __attribute__((optimize("no-tree-loop-distribute-patterns"))) void *memcpy(void *to, const void *from, std::size_t n) {
    char *d = static_cast<char *>(to);
    const char *s = static_cast<const char *>(from);
    if (n >= 64) {
        while (reinterpret_cast<std::uintptr_t>(d) & 3u) {
            *d++ = *s++;
            --n;
        }
        if (!(reinterpret_cast<std::uintptr_t>(s) & 3u)) {
            std::size_t blocks = n >> 5;
            n &= 31u;
            __asm__ volatile(
                "1:\n"
                "pld [%[s], #96]\n"
                "ldmia %[s]!, {r3, r4, r5, r6, r7, r8, r9, ip}\n"
                "subs %[b], %[b], #1\n"
                "stmia %[d]!, {r3, r4, r5, r6, r7, r8, r9, ip}\n"
                "bne 1b\n"
                : [d] "+r"(d), [s] "+r"(s), [b] "+r"(blocks)
                : : "r3", "r4", "r5", "r6", "r7", "r8", "r9", "ip", "cc", "memory");
        }
    }
    for (; n >= 4; n -= 4, d += 4, s += 4) *reinterpret_cast<word *>(d) = *reinterpret_cast<const word *>(s);   // the ARM11 reads and writes unaligned words
    for (; n; --n) *d++ = *s++;
    return to;
}

extern "C" __attribute__((optimize("no-tree-loop-distribute-patterns"))) void *memset(void *to, int value, std::size_t n) {
    char *d = static_cast<char *>(to);
    const std::uint32_t v = static_cast<std::uint8_t>(value) * 0x01010101u;
    if (n >= 64) {
        while (reinterpret_cast<std::uintptr_t>(d) & 3u) {
            *d++ = static_cast<char>(v);
            --n;
        }
        std::size_t blocks = n >> 5;
        n &= 31u;
        __asm__ volatile(
            "mov r3, %[v]\n"
            "mov r4, %[v]\n"
            "mov r5, %[v]\n"
            "mov r6, %[v]\n"
            "mov r7, %[v]\n"
            "mov r8, %[v]\n"
            "mov r9, %[v]\n"
            "mov ip, %[v]\n"
            "1:\n"
            "subs %[b], %[b], #1\n"
            "stmia %[d]!, {r3, r4, r5, r6, r7, r8, r9, ip}\n"
            "bne 1b\n"
            : [d] "+r"(d), [b] "+r"(blocks)
            : [v] "r"(v)
            : "r3", "r4", "r5", "r6", "r7", "r8", "r9", "ip", "cc", "memory");
    }
    for (; n >= 4; n -= 4, d += 4) *reinterpret_cast<word *>(d) = v;
    for (; n; --n) *d++ = static_cast<char>(v);
    return to;
}
