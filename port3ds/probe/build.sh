#!/bin/bash
# Run inside the devkitpro/devkitarm image, with this repo at /p and the output folder at /out.
set -e
export PATH=/opt/devkitpro/devkitARM/bin:/opt/devkitpro/tools/bin:$PATH
C=/opt/devkitpro/libctru
FLAGS="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft"
arm-none-eabi-gcc $FLAGS -O2 -D__3DS__ -I$C/include -c /p/port3ds/probe/probe.c -o /out/probe.o
arm-none-eabi-gcc -specs=3dsx.specs $FLAGS /out/probe.o -L$C/lib -lctru -lm -o /out/probe.elf
3dsxtool /out/probe.elf /out/probe.3dsx
arm-none-eabi-size /out/probe.elf
