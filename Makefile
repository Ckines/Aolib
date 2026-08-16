TARGET_NAME := aolib
DEPS_DIR    := deps

# Cross-compile to Windows: `make all USE_PSF_ENGINE=1 PLATFORM=windows`.
# PLATFORM=linux (default)  gcc/g++ as-is.
#
# ALWAYS `make clean` between builds of different PLATFORM: .o files are
# named the same on both platforms, so make will reuse objects from the
# PREVIOUS platform without knowing the compiler changed. Usually fails
# loud (unresolved Linux libstdc++ symbols under MinGW), but could
# silently produce a corrupt binary with a different flag combination.
PLATFORM ?= linux

ifeq ($(PLATFORM),windows)
CXX := x86_64-w64-mingw32-g++
CC  := x86_64-w64-mingw32-gcc
LIB_EXT := .dll
# Static libgcc/libstdc++:
LDFLAGS += -static-libgcc -static-libstdc++
# DO NOT define LONG_IS_64BIT here. Windows is LLP64
AOSDK_CFLAGS := -DLSB_FIRST=1 -DPATH_MAX=1024 -DHAS_PSXCPU=1
else
CXX ?= g++
CC  ?= gcc
LIB_EXT := .so
AOSDK_CFLAGS := -DLSB_FIRST=1 -DLONG_IS_64BIT=1 -DPATH_MAX=1024 -DHAS_PSXCPU=1
endif

CXXFLAGS := -std=c++17 -Wall -Wextra -fPIC -O2
CFLAGS   := -std=c99 -Wall -fPIC -O2 -DNOGUI=1

# Optional: `make <target> SANFLAGS="-fsanitize=address,undefined -g"`.
# Empty by default.
SANFLAGS ?=
CXXFLAGS += $(SANFLAGS)
CFLAGS   += $(SANFLAGS)

# REQUIRED for all aosdk code and any .cpp including ao.h. Mirrors
# deps/aosdk/Makefile exactly instead of relying on endianness
# autodetection.
#
# -DLSB_FIRST=1: without it, ao.h only takes its little-endian path when
# the compiler predefines __LITTLE_ENDIAN__ (Apple/Xcode only). On Linux
# GCC that macro doesn't exist, so LE32() silently corrupts every 32-bit
# field read from a PSF file.
#
# -DLONG_IS_64BIT=1 aligns aosdk's uint64/int64 with <cstdint> on LP64
# platforms. Must NOT be set on Windows/LLP64 -- hence the per-platform
# AOSDK_CFLAGS above, not a single fixed value.

# Same idea as LSB_FIRST for aosdk: libgme also needs explicit endianness,
# but fails LOUD (#error) instead of silently miscompiling. Value taken
# from libgme's own CMakeLists.txt.
GME_CFLAGS := -DBLARGG_LITTLE_ENDIAN=1 -DVGM_YM2612_GENS=1

INCLUDES := -Isrc -Isrc/engine \
            -I$(DEPS_DIR)/aosdk \
            -I$(DEPS_DIR)/aosdk/zlib \
            -I$(DEPS_DIR)/game-music-emu/gme \
            -I$(DEPS_DIR)/libvgm

# ─── libvgm: VGM/VGZ backend ────────────────────────────────────────────
# Vendored without CMake; source selection and defines live here, same as
# aosdk and libgme.
#
# TWO define families: SNDDEV_<chip> registers the DEVICE, EC_<chip>_<core>
# builds the actual emulation CORE. Declaring the first without the second
# links clean but is SILENT at runtime (no core found, no log). See
# LibvgmEngine::verify_all_devices_started() for the safety net.
#
# -DSNDDEV_SELECT is REQUIRED: without it, SoundEmu.c pulls in headers for
# all 40+ known chips regardless of what's actually compiled.
LIBVGM_CFLAGS := -DVGM_LITTLE_ENDIAN -DHAVE_STDINT_H -Dz_const= -D_POSIX_C_SOURCE=200809L \
                 -DSNDDEV_SELECT -I$(DEPS_DIR)/aosdk/zlib
# -DVGM_LITTLE_ENDIAN: libvgm doesn't autodetect this (playera.cpp errors
# out without it). Not platform-conditioned like LONG_IS_64BIT: this
# project only targets little-endian platforms.
# -DHAVE_STDINT_H: without it, emutypes.h redefines <stdint.h> types and
# clashes with headers that already include it.
# -Dz_const=: the vendored zlib is 1.2.1; 'z_const' only exists since
# 1.2.5.2, but MemoryLoader.c uses it. Upgrading that zlib is separate
# debt (also affects the PSF build).
# -D_POSIX_C_SOURCE=200809L: REQUIRED, and its absence isn't a compile
# error but a SEGV. Without it, glibc hides strdup/strncasecmp prototypes
# under -std=c99, so StrUtils-CPConv_IConv.c calls them with an IMPLICIT
# DECLARATION and truncates strdup's 64-bit return to int -- corrupting
# the first GD3 tag conversion, inside VGMPlayer's own constructor.
# Harmless on Windows (CPConv_Win.c uses neither iconv nor strdup).

# Infra (always, regardless of which chips are enabled)
LIBVGM_SOURCES := \
  $(DEPS_DIR)/libvgm/emu/Resampler.c      $(DEPS_DIR)/libvgm/emu/SoundEmu.c \
  $(DEPS_DIR)/libvgm/emu/dac_control.c    $(DEPS_DIR)/libvgm/emu/logging.c \
  $(DEPS_DIR)/libvgm/emu/panning.c \
  $(DEPS_DIR)/libvgm/utils/DataLoader.c   $(DEPS_DIR)/libvgm/utils/MemoryLoader.c \
  $(DEPS_DIR)/libvgm/player/dblk_compr.c  $(DEPS_DIR)/libvgm/player/helper.c
LIBVGM_CXX_SOURCES := \
  $(DEPS_DIR)/libvgm/player/playera.cpp   $(DEPS_DIR)/libvgm/player/playerbase.cpp \
  $(DEPS_DIR)/libvgm/player/vgmplayer.cpp $(DEPS_DIR)/libvgm/player/vgmplayer_cmdhandler.cpp

# GD3 tag codepage conversion (UTF-16 -> UTF-8): the only dependency that
# changes SOURCE FILE per platform, not just flags. Linux: iconv-based.
# Windows: MultiByteToWideChar-based.
ifeq ($(PLATFORM),windows)
  LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/utils/StrUtils-CPConv_Win.c
else
  LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/utils/StrUtils-CPConv_IConv.c
endif

# The three chips libgme already covers, kept separate so the backend
# switch is auditable. EC_YM2612_GENS is the SAME core libgme already
# uses -- any sound difference isn't coming from there.
LIBVGM_CFLAGS  += -DSNDDEV_SN76496 -DEC_SN76496_MAME
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/sn764intf.c $(DEPS_DIR)/libvgm/emu/cores/sn76496.c
LIBVGM_CFLAGS  += -DSNDDEV_YM2413 -DEC_YM2413_MAME
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/2413intf.c  $(DEPS_DIR)/libvgm/emu/cores/ym2413.c
LIBVGM_CFLAGS  += -DSNDDEV_YM2612 -DEC_YM2612_GENS
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/2612intf.c  $(DEPS_DIR)/libvgm/emu/cores/ym2612.c

# YM2151: MAME core (validated against real Sharp X68000/arcade content).
LIBVGM_CFLAGS  += -DSNDDEV_YM2151 -DEC_YM2151_MAME
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/2151intf.c  $(DEPS_DIR)/libvgm/emu/cores/ym2151.c

# MSM6258 (OKIM6258): Sharp X68000 ADPCM, almost always paired with
# YM2151 on that platform. Single-file core, no EC_* to choose.
LIBVGM_CFLAGS  += -DSNDDEV_MSM6258
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/okim6258.c

# Arcade chips that ride alongside YM2151 on specific boards: MSM6295
# (Toaplan 2), SegaPCM, GA20 (Irem), C140 (Namco), K053260 (Konami).
# Single-file cores, no EC_* to choose.
#
# WATCH OUT for C140: if the VGM header's banking byte (0x96) is 2, the
# player redirects the chip to DEVID_C219 (a DIFFERENT chip, not
# implemented separately here). Tested content uses banking 0x00.
LIBVGM_CFLAGS  += -DSNDDEV_MSM6295
# okiadpcm.c: shared ADPCM decoder okim6295.c needs via #include.
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/okim6295.c $(DEPS_DIR)/libvgm/emu/cores/okiadpcm.c
LIBVGM_CFLAGS  += -DSNDDEV_SEGAPCM
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/segapcm.c
LIBVGM_CFLAGS  += -DSNDDEV_C140
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/c140.c
LIBVGM_CFLAGS  += -DSNDDEV_K053260
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/k053260.c
LIBVGM_CFLAGS  += -DSNDDEV_GA20
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/iremga20.c

# YM2203 (OPN): unlike the chips above, opnintf.c/fmopn.c LINK to an
# internal AY8910/SSG device. That link silently drops unless
# SNDDEV_AY8910 + EC_AY8910_EMU2149 are also compiled -- otherwise the
# SSG channel of any real YM2203 VGM plays mute with no warning.
LIBVGM_CFLAGS  += -DSNDDEV_YM2203 -DSNDDEV_AY8910 -DEC_AY8910_EMU2149
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/opnintf.c $(DEPS_DIR)/libvgm/emu/cores/fmopn.c \
                   $(DEPS_DIR)/libvgm/emu/cores/ayintf.c $(DEPS_DIR)/libvgm/emu/cores/emu2149.c
# Side effect of -DSNDDEV_AY8910: standalone AY8910 VGMs (opcode 0xA0,
# no YM2203 involved) also work for free.

# YM3812 (OPL2): simple pattern, EC_YM3812_MAME chosen for consistency.
# YM3526 (OPL, OPL2's predecessor) shares fmopl.c with YM3812, no extra
# file needed. Common in Bubble Bobble (DEV_ID 0x0A).
LIBVGM_CFLAGS  += -DSNDDEV_YM3812 -DEC_YM3812_MAME -DSNDDEV_YM3526
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/oplintf.c $(DEPS_DIR)/libvgm/emu/cores/fmopl.c

# YM2608 (OPNA, PC-8801/9801) and YM2610 (OPNB, Neo Geo): same file as
# YM2203, but need ymdeltat.c (ADPCM). Both link their internal SSG to
# AY8910, already covered above. YM2608's rhythm ADPCM-A ROM ships
# pre-derived (fmopn_2608rom.h), no external .rom needed.
# KNOWN GAP: YM2608's external ADPCM-B memory (rare in PC-88/98 content)
# isn't wired in InitDevices(); doesn't crash, just silent for that path.
LIBVGM_CFLAGS  += -DSNDDEV_YM2608 -DSNDDEV_YM2610
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/ymdeltat.c

# QSound (Capcom DL-1425, CPS-II/ZN). Two cores available; libvgm itself
# prefers CTR by default for this chip (not MAME, unlike the rest) --
# that upstream preference is kept as-is. No LinkDevice. Clock quirk
# (old VGMs declare 4MHz vs the real 60MHz) is auto-corrected upstream.
LIBVGM_CFLAGS  += -DSNDDEV_QSOUND -DEC_QSOUND_CTR
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/qsoundintf.c $(DEPS_DIR)/libvgm/emu/cores/qsound_ctr.c

# K054539 (Konami PCM, dual-chip on many GX boards -- generic dual-chip
# mechanism handles that automatically). Simple pattern.
LIBVGM_CFLAGS  += -DSNDDEV_K054539
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/k054539.c

# C219 (Namco ASIC 219, C140 relative with different banking). The
# player rewrites chipType from C140 to C219 at load time when the
# banking byte == 2; without -DSNDDEV_C219 that resolved chip wouldn't
# be registered. Simple pattern otherwise.
LIBVGM_CFLAGS  += -DSNDDEV_C219
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/c219.c

# C352 (Namco System 22/23, Ridge Racer). Only chip in this group with a
# dedicated DEVID_C352 case (16-bit addr+data). Clock correction is
# automatic.
LIBVGM_CFLAGS  += -DSNDDEV_C352
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/c352.c

# RF5C68 (Ricoh, on-board RAM PCM -- Sega System 18/24/32, some Mega
# Drive/CD). Simple pattern, uses RAM instead of ROM.
LIBVGM_CFLAGS  += -DSNDDEV_RF5C68 -DEC_RF5C68_MAME
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/rf5c68.c $(DEPS_DIR)/libvgm/emu/cores/rf5cintf.c

# RF5C164 (Sega CD/Mega-CD variant of RF5C68). Needed separately: when
# devCfg.flags==1 (the RF5C164 variant), the player forces the GENS core
# instead of MAME, and without EC_RF5C68_GENS that fails silently. Added
# alongside MAME, not instead of it -- the player picks automatically.
LIBVGM_CFLAGS  += -DEC_RF5C68_GENS
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/scd_pcm.c

# YMF262 (OPL3, AWE/OPL3 PC sound cards). Separate core file from
# YM3812/YM3526 despite being OPL family. EC_YMF262_MAME chosen for
# consistency.
LIBVGM_CFLAGS  += -DSNDDEV_YMF262 -DEC_YMF262_MAME
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/262intf.c $(DEPS_DIR)/libvgm/emu/cores/ymf262.c

# YMW258 (MultiPCM, Sega Model 2/3). Simple pattern, no EC_* to choose.
# NOTE: enabled from the VGM header spec, not yet validated with real
# content.
LIBVGM_CFLAGS  += -DSNDDEV_YMW258
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/multipcm.c

# YMZ280B (Yamaha PCMD8, appears with QSound on some Capcom boards).
LIBVGM_CFLAGS  += -DSNDDEV_YMZ280B
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/ymz280b.c

# C6280 (HuC6280, PC Engine/TurboGrafx-16). Simple pattern.
LIBVGM_CFLAGS  += -DSNDDEV_C6280 -DEC_C6280_MAME
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/c6280intf.c $(DEPS_DIR)/libvgm/emu/cores/c6280_mame.c

# ES5503 (Ensoniq DOC, Apple IIGS / some Taito arcade). Single self-
# contained file, no LinkDevice.
LIBVGM_CFLAGS  += -DSNDDEV_ES5503
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/es5503.c

# K051649 (Konami SCC, MSX / old Konami arcade). Simple pattern.
LIBVGM_CFLAGS  += -DSNDDEV_K051649
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/k051649.c

# X1-010 (Seta, 90s arcade shooters). Simple pattern.
LIBVGM_CFLAGS  += -DSNDDEV_X1_010
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/x1_010.c

# 32X PWM (Sega 32X). WATCH OUT: "32X" doesn't automatically imply
# 32X_PWM -- check per game (e.g. Knuckles' Chaotix needs it, Virtua
# Racing Deluxe on the same board doesn't).
LIBVGM_CFLAGS  += -DSNDDEV_32X_PWM
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/pwm.c

# NES APU as a direct VGM chip (distinct from libgme's native .nsf
# support). EC_NES_MAME chosen for consistency.
LIBVGM_CFLAGS  += -DSNDDEV_NES_APU -DEC_NES_MAME
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/nesintf.c $(DEPS_DIR)/libvgm/emu/cores/nes_apu.c

# Virtual Boy VSU. Single self-contained file (symbol is sndDev_VBoyVSU,
# mind the capitalization). Mednafen-derived core.
LIBVGM_CFLAGS  += -DSNDDEV_VBOY_VSU
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/vsu.c

# WonderSwan (Bandai). Single self-contained file (sndDev_WSwan).
LIBVGM_CFLAGS  += -DSNDDEV_WSWAN
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/ws_audio.c

# ES5506 (Ensoniq OTIS) -- AUDITED, DELIBERATELY NOT ENABLED. This
# vendored libvgm snapshot's es5506.c is a stub with an empty core table.
# No emulator exists here to activate; would need writing or vendoring
# one from elsewhere. Real, permanent gap.

# src/libretro.cpp includes libvgm_engine.hpp UNCONDITIONALLY (like
# gme_engine.hpp): VGM/VGZ is the default backend, no separate flag.
LIBVGM_OBJS := $(LIBVGM_SOURCES:.c=.o) $(LIBVGM_CXX_SOURCES:.cpp=.o)

# LIBVGM_CFLAGS applies to any translation unit that includes libvgm
# headers, not just LIBVGM_SOURCES -- also src/libretro.cpp.
CXXFLAGS += $(LIBVGM_CFLAGS)
CFLAGS   += $(LIBVGM_CFLAGS)

# ─── Core's own objects ─────────────────────────────────────────────────
# The UI (software renderer + bitmap font) is part of the core, not a
# separate module. Header-only UI files need no entry here.
CORE_SOURCES := src/libretro.cpp \
                src/engine/aosdk_bridge.cpp \
                src/ui/ui_renderer.cpp \
                src/ui/ui_font.cpp

# ─── zlib (vendored with aosdk): ALWAYS compiled ─────────────────────────
# Needed for both PSF (corlett.c uses compress/uncompress) and .zip
# (unzip.c/ioapi.c) -- the latter is needed even with USE_PSF_ENGINE=0.
ZLIB_SOURCES := $(wildcard $(DEPS_DIR)/aosdk/zlib/*.c)

# PSF/SSF container parser + utility hash table, shared by all three
# aosdk engines. corlett.c is the format parser; utils.c provides the
# hashtable_* functions corlett.c depends on.
AOSDK_PARSE_SOURCES := \
  $(DEPS_DIR)/aosdk/corlett.c \
  $(DEPS_DIR)/aosdk/utils.c

# ─── aosdk: full engine (R3000A CPU + SPU) ───────────────────────────────
# Per license.txt: BSD (corlett/eng_psf/eng_psf2/eng_spu/psx_hw), MAME
# (psx.c, the CPU), GPL (peops/peops2, the SPU). Excludes main.c and the
# rest of the AO desktop app -- not part of the engine, and several use
# real fopen via ao_fopen (see src/engine/aosdk_host_glue.cpp, which
# supplies the symbols actually needed: ao_song_done, ao_fopen, ao_mkdir,
# ao_sleep).
AOSDK_ENGINE_SOURCES := \
  $(AOSDK_PARSE_SOURCES) \
  $(DEPS_DIR)/aosdk/eng_psf/eng_psf.c \
  $(DEPS_DIR)/aosdk/eng_psf/eng_spu.c \
  $(DEPS_DIR)/aosdk/eng_psf/psx.c \
  $(DEPS_DIR)/aosdk/eng_psf/psx_hw.c \
  $(DEPS_DIR)/aosdk/eng_psf/peops/spu.c \
  $(DEPS_DIR)/aosdk/eng_psf/eng_psf2.c \
  $(DEPS_DIR)/aosdk/eng_psf/peops2/spu.c \
  $(DEPS_DIR)/aosdk/eng_psf/peops2/dma.c \
  $(DEPS_DIR)/aosdk/eng_psf/peops2/registers.c \
  $(DEPS_DIR)/aosdk/eng_ssf/m68kcpu.c \
  $(DEPS_DIR)/aosdk/eng_ssf/m68kopac.c \
  $(DEPS_DIR)/aosdk/eng_ssf/m68kopdm.c \
  $(DEPS_DIR)/aosdk/eng_ssf/m68kopnz.c \
  $(DEPS_DIR)/aosdk/eng_ssf/m68kops.c \
  $(DEPS_DIR)/aosdk/eng_ssf/scsp.c \
  $(DEPS_DIR)/aosdk/eng_ssf/scspdsp.c \
  $(DEPS_DIR)/aosdk/eng_ssf/sat_hw.c \
  $(DEPS_DIR)/aosdk/eng_ssf/eng_ssf.c
# Saturn (SSF) uses the same corlett container as PSF, different engine
# (Musashi M68000 + SCSP, not SH-2 -- Saturn audio is generated by an
# auxiliary MC68EC000 inside the SCSP subsystem). The 9-file list above
# is copied exactly from deps/aosdk/Makefile. scsplfo.c is included by
# scsp.c via #include (unity build); m68kmake.c (opcode table generator)
# is never compiled, its output is pre-generated and vendored.
#
# eng_psf2.c + peops2/{spu,dma,registers}.c share psx.c/psx_hw.c with
# PSF1 -- intentional, both engines share the R3000A core and RAM at
# process level (see AosdkPsxCoreGuard in src/engine/aosdk_bridge.hpp).
#
# WATCH OUT: peops/spu.c (PSF1) is a unity build -- it #includes
# reverb.c/adsr.c/registers.c/dma.c directly. Don't compile those as
# separate objects (duplicate symbols). peops2 is only a PARTIAL unity
# build: spu.c includes reverb.c/adsr.c, but dma.c/registers.c compile
# separately, and xa.c is dead code (commented out include, never used).
# peops2 is covered by the same GPL verdict as peops in
# THIRD-PARTY-LICENSES.md (same author, same license).

GME_SOURCES := $(filter-out %/Ym2612_Nuked.cpp %/Ym2612_MAME.cpp, \
                 $(wildcard $(DEPS_DIR)/game-music-emu/gme/*.cpp))
# Only ONE YM2612 backend can be compiled at a time (GENS here, via
# -DVGM_YM2612_GENS). An unfiltered wildcard compiles all three and fails
# linking an executable (tests) with unresolved symbols from the unused
# backends -- a shared library wouldn't complain, since it tolerates
# unresolved symbols by default.
GME_EXT_SOURCES := $(DEPS_DIR)/game-music-emu/gme/ext/emu2413.c
# Nes_Vrc7_Apu.cpp (Konami VRC7 FM synth) depends on emu2413.c, which
# lives outside the plain gme/*.cpp wildcard.
# libgme is LGPL-2.1, no usage restriction -- see THIRD-PARTY-LICENSES.md.

OBJS := $(CORE_SOURCES:.cpp=.o) $(GME_SOURCES:.cpp=.o) $(GME_EXT_SOURCES:.c=.o) $(ZLIB_SOURCES:.c=.o) $(LIBVGM_OBJS)

# AOSDK_CFLAGS applies ALWAYS, not just when USE_PSF_ENGINE=1: even
# test-f2 (isolated parsing) compiles aosdk files and needs -DLSB_FIRST=1.
CXXFLAGS += $(AOSDK_CFLAGS) $(GME_CFLAGS)
CFLAGS   += $(AOSDK_CFLAGS)

ifeq ($(USE_PSF_ENGINE),1)
OBJS += $(AOSDK_ENGINE_SOURCES:.c=.o) src/engine/aosdk_host_glue.o
CXXFLAGS += -DAOLIB_WITH_PSF=1
endif

TARGET := $(TARGET_NAME)_libretro$(LIB_EXT)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(SANFLAGS) -shared -o $@ $^ $(LDFLAGS)

# -MMD -MP: per-object header dependency tracking. Without it, editing a
# .hpp doesn't trigger recompilation and the link reuses stale objects.
DEPFLAGS := -MMD -MP

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

# ═══════════════════════ 'make dist' ═══════════════════════════════
#
# dist/ doesn't update itself -- always rebuild from a clean tree and
# copy the result over dist/, never the other way around. The .info
# lives at the repo root (aolib_libretro.info, single source of truth)
# and is copied here; edit the root copy, not the ones under dist/.
#
# 'dist' builds Linux only. Windows is a separate target since it needs
# `make clean` in between and the MinGW toolchain isn't always available.
dist: clean
	$(MAKE) all USE_PSF_ENGINE=1
	mkdir -p dist-linux
	cp aolib_libretro.so dist-linux/aolib_libretro.so
	cp aolib_libretro.info dist-linux/aolib_libretro.info
	@echo "dist-linux/aolib_libretro.{so,info} updated (info copied from repo root)."
	@ls -la dist-linux/aolib_libretro.so dist-linux/aolib_libretro.info

dist-windows: clean
	$(MAKE) all USE_PSF_ENGINE=1 PLATFORM=windows
	mkdir -p dist-windows
	cp aolib_libretro.dll dist-windows/aolib_libretro.dll
	cp aolib_libretro.info dist-windows/aolib_libretro.info
	@echo "dist-windows/aolib_libretro.{dll,info} updated (info copied from repo root)."
	@ls -la dist-windows/aolib_libretro.dll dist-windows/aolib_libretro.info

dist-all: dist dist-windows

clean:
	rm -f $(OBJS) $(TARGET)
	rm -f $(AOSDK_ENGINE_SOURCES:.c=.o) src/engine/aosdk_host_glue.o
	rm -f deps/libvgm/emu/cores/c6280intf.o
	rm -f deps/libvgm/emu/cores/es5506.o
	rm -f deps/game-music-emu/gme/Ym2612_MAME.o deps/game-music-emu/gme/Ym2612_Nuked.o
	# .d files from -MMD live next to each .o
	find src tests deps -name '*.d' -delete 2>/dev/null || true

.PHONY: all clean dist dist-windows dist-all

# ─── one-off diagnostic probes (NOT part of the permanent suite) ────
# These need real, non-distributable content in /tmp and are run by hand.
