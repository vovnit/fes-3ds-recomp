# Builds the New 3DS test build. Run with port3ds/build.sh, which starts this inside the devkitpro/devkitarm image
# with the ps2-web-recomp folder at /w and this repository at /p. The output in /w/build/3ds comes from your own disc: keep it private.
DEVKITPRO ?= /opt/devkitpro
PREFIX    := $(DEVKITPRO)/devkitARM/bin/arm-none-eabi-
CTRU      := $(DEVKITPRO)/libctru
PORTLIBS  := $(DEVKITPRO)/portlibs/3ds
TOOLS     := $(DEVKITPRO)/tools/bin
OUT       := /w/build/3ds

ARCH      := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
OPT       ?= -O2
CXXFLAGS  := $(ARCH) $(OPT) -std=gnu++20 -D__3DS__ -fno-math-errno -ffunction-sections -Wno-psabi -MMD -MP \
             -DPS2_PICA -I$(CTRU)/include -I/w/runtime/include -I/w/runtime/src -I/p/port3ds -I$(OUT)/shader

# PROF=1 times every part of the program for "stats 1", which itself costs time.
PROF      ?= 0
ifeq ($(PROF),1)
CXXFLAGS  += -DPS2_PROF_ALL
endif

RUNTIME   := $(wildcard /w/runtime/src/*.cpp /w/runtime/src/gs/*.cpp /w/runtime/src/iop/*.cpp /w/runtime/src/ipu/*.cpp)
# VU=1 adds the vector unit programs from "vurecomp <recorded> generated/p3fes-vu3ds --scalar". Without them they run in the interpreter.
VU        ?= 0
GAME      := $(filter-out /w/generated/p3fes/vu1_%,$(wildcard /w/generated/p3fes/*.cpp))
ifeq ($(VU),1)
GAME      += $(wildcard /w/generated/p3fes-vu3ds/vu1_*.cpp)
endif
# The I/O processor code from tools/p3fes_iopgen.py, if you made it. Without it all of that code runs in the interpreter.
# IOPRECORD=1 makes a slow build that records what ran, for that tool; it leaves iopseen.bin and iopram.bin in sdmc:/p3fes.
IOPRECORD ?= 0
ifeq ($(IOPRECORD),1)
CXXFLAGS  += -DPS2_IOP_RECORD
else
IOP       := $(wildcard /w/generated/p3fes-iop/*.cpp)
ifneq ($(IOP),)
GAME      += $(IOP)
CXXFLAGS  += -DPS2_IOP_BLOCKS
endif
endif
HOST      := /p/port3ds/main3ds.cpp /p/port3ds/pica.cpp /p/port3ds/native3ds.cpp /p/port3ds/memcpy3ds.cpp /p/port3ds/remote3ds.cpp /p/port3ds/iop3ds.cpp
OBJS      := $(patsubst /%.cpp,$(OUT)/obj/%.o,$(RUNTIME) $(GAME) $(HOST)) $(OUT)/shader/gs_v_shbin.o
CONFIG    := $(OUT)/build-config
CONFIG_FLAGS = $(CXXFLAGS) VUOPT=$(VUOPT)

all: $(OUT)/p3fes.3dsx

.PHONY: FORCE
FORCE:

# Recheck the flags and input list on each run, but only invalidate outputs when they change.
$(CONFIG): FORCE
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(subst ','"'"',$(CONFIG_FLAGS))' $(OBJS) > $@.tmp
	@if cmp -s $@.tmp $@; then rm $@.tmp; else mv $@.tmp $@; fi

$(OUT)/p3fes.3dsx: $(OUT)/p3fes.elf
	$(TOOLS)/3dsxtool $< $@

$(OUT)/p3fes.elf: $(OBJS) $(CONFIG)
	$(PREFIX)g++ -specs=3dsx.specs $(ARCH) -Wl,--gc-sections -Wl,-Map,$(OUT)/p3fes.map $(OBJS) -L$(CTRU)/lib -L$(PORTLIBS)/lib -lcitro3d -lctru -lz -lm -o $@
	$(PREFIX)size $@

# The wireless link unpacks uploads with zlib from the portlibs.
$(OUT)/obj/p/port3ds/remote3ds.o: CXXFLAGS += -I$(PORTLIBS)/include

# Translated game code: exceptions never pass through it, and leaving them out makes it smaller.
$(OUT)/obj/w/generated/%.o: /w/generated/%.cpp $(CONFIG)
	@mkdir -p $(dir $@)
	$(PREFIX)g++ $(CXXFLAGS) -DPS2_VU_FAST -fno-exceptions -fno-rtti -I/w/generated/p3fes -Wno-unused-label -Wno-unused-variable -c $< -o $@

# Vector programs copy registers with "add 0", which is a real addition unless the sign of zero can be ignored.
# Loop peeling reduces repeated work in the compiled vector loops. VUOPT changes the flags for those programs alone.
VUOPT     ?= -fpeel-loops
$(OUT)/obj/w/generated/p3fes-vu3ds/%.o: CXXFLAGS += -fno-signed-zeros $(VUOPT)
VU_OBJS := $(filter $(OUT)/obj/w/generated/p3fes-vu3ds/%,$(OBJS))
ifneq ($(VU_OBJS),)
$(VU_OBJS): /p/port3ds/Makefile
endif

$(OUT)/obj/%.o: /%.cpp $(CONFIG)
	@mkdir -p $(dir $@)
	$(PREFIX)g++ $(CXXFLAGS) -c $< -o $@

# The shader becomes bytes inside the program.
$(OUT)/shader/gs_v.shbin: /p/port3ds/gs.v.pica
	@mkdir -p $(dir $@)
	$(TOOLS)/picasso -o $@ $<

$(OUT)/shader/gs_v_shbin.o $(OUT)/shader/gs_v_shbin.h: $(OUT)/shader/gs_v.shbin
	cd $(OUT)/shader && $(TOOLS)/bin2s gs_v.shbin > gs_v_shbin.s
	$(PREFIX)gcc -x assembler-with-cpp $(ARCH) -c $(OUT)/shader/gs_v_shbin.s -o $(OUT)/shader/gs_v_shbin.o
	printf 'extern "C" { extern const unsigned char gs_v_shbin[]; extern const unsigned char gs_v_shbin_end[]; extern const unsigned int gs_v_shbin_size; }\n' > $(OUT)/shader/gs_v_shbin.h

$(OUT)/obj/p/port3ds/pica.o: $(OUT)/shader/gs_v_shbin.h

# Its own definitions must not be replaced by calls to the library, and no compiler pass should see memcpy as a built-in here.
$(OUT)/obj/p/port3ds/memcpy3ds.o: CXXFLAGS += -fno-builtin

-include $(OBJS:.o=.d)
