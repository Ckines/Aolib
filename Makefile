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
# libxmp-lite usa pow/log/floor (period.c, filter.c). En Linux libm es una
# biblioteca aparte; enlazarla explícitamente evita depender de que el
# frontend ya la tenga cargada.
LDFLAGS += -lm
AOSDK_CFLAGS := -DLSB_FIRST=1 -DLONG_IS_64BIT=1 -DPATH_MAX=1024 -DHAS_PSXCPU=1
endif

# -MMD -MP: per-object header dependency tracking. Without it, editing a
# .hpp doesn't trigger recompilation and the link reuses stale objects.
# Defined here (not next to the %.o rules) because the vgmstream pattern
# rule further down also uses it and make expands it at rule-definition
# time.
DEPFLAGS := -MMD -MP

CXXFLAGS := -std=c++17 -Wall -Wextra -fPIC -O2
CFLAGS   := -std=c99 -Wall -fPIC -O2 -DNOGUI=1

# ─── link-time size optimization ────────────────────────────────────────
#
# -ffunction-sections + --gc-sections drop every function no symbol chain
# reaches from the exported retro_* entry points. -fvisibility=hidden is
# what makes that chain short: without it every non-static symbol is
# exported and therefore a GC root, and --gc-sections keeps almost
# everything.
#
# -fdata-sections is LINUX ONLY, and this is NOT a style choice.
#
# On PE-COFF (MinGW) GCC emits uninitialized arrays into named .data$<sym>
# sections (PROGBITS, file-backed) instead of .bss (NOBITS, zero-filled at
# load). Every large zeroed table in aosdk/libvgm then lands physically in
# the DLL. Measured on this tree, GCC 13.2, identical sources and flags
# except this one:
#
#   with    -fdata-sections:  11,698,688 bytes (stripped .dll)
#   without -fdata-sections:   1,775,104 bytes (stripped .dll)
#
# 6.6x. Originally found on GCC 14.2; reproduced on 13.2, so treat it as
# stable MinGW behaviour and not a single-version regression. ELF is
# unaffected: there -fdata-sections keeps .bss as NOBITS and is a net win.
ifeq ($(PLATFORM),windows)
SIZEFLAGS_C   := -ffunction-sections -fvisibility=hidden
SIZEFLAGS_CXX := -ffunction-sections -fvisibility=hidden -fvisibility-inlines-hidden
else
SIZEFLAGS_C   := -ffunction-sections -fdata-sections -fvisibility=hidden
SIZEFLAGS_CXX := -ffunction-sections -fdata-sections -fvisibility=hidden -fvisibility-inlines-hidden
endif

CXXFLAGS += $(SIZEFLAGS_CXX)
CFLAGS   += $(SIZEFLAGS_C)
LDFLAGS  += -Wl,--gc-sections

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
            -I$(DEPS_DIR)/minizip \
            -I$(DEPS_DIR)/sevenzip \
            -I$(DEPS_DIR)/aosdk/zlib \
            -I$(DEPS_DIR)/game-music-emu/gme \
            -I$(DEPS_DIR)/libxmp-lite/include \
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
# YM2413 (OPLL): 2413intf.c's own core array lists EC_YM2413_EMU2413
# FIRST, with an explicit "// default, because it's better than MAME"
# comment. Both cores are compiled; EMU2413 is what SndEmu_Start2()
# picks.
LIBVGM_CFLAGS  += -DSNDDEV_YM2413 -DEC_YM2413_EMU2413 -DEC_YM2413_MAME
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/2413intf.c  $(DEPS_DIR)/libvgm/emu/cores/ym2413.c \
                   $(DEPS_DIR)/libvgm/emu/cores/emu2413.c
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

# YM3812 (OPL2): oplintf.c's own core array lists EC_YM3812_ADLIBEMU
# FIRST, with "// default, because it's better than MAME". Both cores
# are compiled; AdLibEmu is what gets picked. adlibemu_opl2.c/opl3.c are
# two separately-compiled units of the SAME adlibemu_opl_inc.c (OPL2 vs
# OPL3 via a #define before the #include), not a duplicate.
# YM3526 (OPL, OPL2's predecessor) shares fmopl.c with YM3812, no extra
# file needed. Common in Bubble Bobble (DEV_ID 0x0A).
LIBVGM_CFLAGS  += -DSNDDEV_YM3812 -DEC_YM3812_ADLIBEMU -DEC_YM3812_MAME -DSNDDEV_YM3526
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/oplintf.c $(DEPS_DIR)/libvgm/emu/cores/fmopl.c \
                   $(DEPS_DIR)/libvgm/emu/cores/adlibemu_opl2.c

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
# YM3812/YM3526 despite being OPL family. 262intf.c's own core array
# lists EC_YMF262_ADLIBEMU FIRST, with "// default, because it's better
# than MAME". Both cores are compiled; AdLibEmu is what gets picked.
# adlibemu_opl3.c is the OPL3-flavored compile of the same
# adlibemu_opl_inc.c used for YM3812 above.
LIBVGM_CFLAGS  += -DSNDDEV_YMF262 -DEC_YMF262_ADLIBEMU -DEC_YMF262_MAME
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/262intf.c $(DEPS_DIR)/libvgm/emu/cores/ymf262.c \
                   $(DEPS_DIR)/libvgm/emu/cores/adlibemu_opl3.c

# YMW258 (MultiPCM, Sega Model 2/3). Simple pattern, no EC_* to choose.
# NOTE: enabled from the VGM header spec, not yet validated with real
# content.
LIBVGM_CFLAGS  += -DSNDDEV_YMW258
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/multipcm.c

# YMZ280B (Yamaha PCMD8, appears with QSound on some Capcom boards).
LIBVGM_CFLAGS  += -DSNDDEV_YMZ280B
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/ymz280b.c

# C6280 (HuC6280, PC Engine/TurboGrafx-16). c6280intf.c's own core array
# lists EC_C6280_OOTAKE FIRST -- first-is-default, same convention as
# every other multi-core chip in this file. Both cores are compiled;
# Ootake is what gets picked.
LIBVGM_CFLAGS  += -DSNDDEV_C6280 -DEC_C6280_OOTAKE -DEC_C6280_MAME
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/c6280intf.c $(DEPS_DIR)/libvgm/emu/cores/c6280_mame.c \
                   $(DEPS_DIR)/libvgm/emu/cores/Ootake_PSG.c

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
# support).
#
# BOTH cores are compiled, and the order matters: nesintf.c lists
# NSFPlay before MAME in sndDev_NES_APU's core array, so NSFPlay is what
# SndEmu_Start2() picks by default. Compiling EC_NES_MAME alone does not
# error out -- it just silently leaves MAME as the only candidate, which
# is upstream's *fallback*, not its default. MAME's core renders the
# triangle/DMC channels with audible clicking on real rips (e.g. the
# vgmrips Zelda NES pack); NSFPlay's does not. Every other multi-core
# chip in this file follows the same rule: compile the core the chip's
# own intf.c lists first, not whichever seems simplest.
#
# np_nes_fds.c + EC_NES_NSFP_FDS covers the FDS expansion audio
# (Famicom Disk System). It is a separate core inside the same device:
# without it, a VGM that declares the FDS gets the APU but silence on
# the FDS channel.
LIBVGM_CFLAGS  += -DSNDDEV_NES_APU -DEC_NES_NSFPLAY -DEC_NES_NSFP_FDS -DEC_NES_MAME
LIBVGM_SOURCES += $(DEPS_DIR)/libvgm/emu/cores/nesintf.c \
                  $(DEPS_DIR)/libvgm/emu/cores/nes_apu.c \
                  $(DEPS_DIR)/libvgm/emu/cores/np_nes_apu.c \
                  $(DEPS_DIR)/libvgm/emu/cores/np_nes_dmc.c \
                  $(DEPS_DIR)/libvgm/emu/cores/np_nes_fds.c

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
# (the minizip sources below inflate with it) -- required even with
# USE_PSF_ENGINE=0.
ZLIB_SOURCES := $(wildcard $(DEPS_DIR)/aosdk/zlib/*.c)

# ─── minizip 1.3.1 (vendored separately from zlib): ALWAYS compiled ──────
# The minizip that shipped inside aosdk's zlib was version 1.00 (2003),
# with no Zip64 support: it rejects any archive using the Zip64 central
# directory, which several archivers emit even for small files. Replaced
# with the version from zlib 1.3.1 contrib/minizip; see
# $(DEPS_DIR)/minizip/VENDOR.md for the vendoring notes and the local
# patch. -I$(DEPS_DIR)/minizip comes BEFORE -I$(DEPS_DIR)/aosdk/zlib in
# INCLUDES so that "unzip.h"/"ioapi.h" resolve here.
MINIZIP_SOURCES := \
  $(DEPS_DIR)/minizip/unzip.c \
  $(DEPS_DIR)/minizip/ioapi.c

# UNZ_BUFSIZE: 16 KiB por defecto, o sea que unzip.c pide el fichero a
# trozos de 16 KiB. Igualado al bloque de lectura anticipada de
# src/zip_vfs_adapter.hpp (kZipReadBlock) para que sus peticiones se
# sirvan DIRECTAS, sin rellenar la caché ni sobreleer. Se define aquí y no
# en unzip.c para no tocar la fuente vendorizada.
CFLAGS += -DUNZ_BUFSIZE=65536

# ─── 7-Zip SDK (vendored, decode-only subset): ALWAYS compiled ──────────
# Reads .7z archives directly (sevenzip_playlist.hpp), the same way
# minizip reads .zip: the core opens the container itself through the VFS
# and enumerates supported entries. Required even with USE_PSF_ENGINE=0,
# same rationale as minizip -- a .7z can just as well hold VGM/SPC/etc.
#
# Public-domain C sources from the official 7-Zip SDK (Igor Pavlov); see
# $(DEPS_DIR)/sevenzip/VENDOR.md for exactly which files were taken and
# which were deliberately left out (7zFile.c: fopen()-based, forbidden by
# this project's I/O rule, same reasoning as minizip's ioapi.c patch).
#
# Bra86.c/BraIA64.c/Bcj2.c: branch-filter and BCJ2 decoders. Not needed
# for the audio-only content this core actually plays, but 7zDec.c
# references them unconditionally for any solid block that used those
# filters (mainly .exe-heavy 7z archives), so they're included for
# correctness on archives this core didn't create.
SEVENZIP_SOURCES := \
  $(DEPS_DIR)/sevenzip/7zAlloc.c \
  $(DEPS_DIR)/sevenzip/7zArcIn.c \
  $(DEPS_DIR)/sevenzip/7zBuf.c \
  $(DEPS_DIR)/sevenzip/7zBuf2.c \
  $(DEPS_DIR)/sevenzip/7zCrc.c \
  $(DEPS_DIR)/sevenzip/7zCrcOpt.c \
  $(DEPS_DIR)/sevenzip/7zDec.c \
  $(DEPS_DIR)/sevenzip/7zStream.c \
  $(DEPS_DIR)/sevenzip/Bcj2.c \
  $(DEPS_DIR)/sevenzip/Bra.c \
  $(DEPS_DIR)/sevenzip/Bra86.c \
  $(DEPS_DIR)/sevenzip/BraIA64.c \
  $(DEPS_DIR)/sevenzip/CpuArch.c \
  $(DEPS_DIR)/sevenzip/Delta.c \
  $(DEPS_DIR)/sevenzip/Lzma2Dec.c \
  $(DEPS_DIR)/sevenzip/LzmaDec.c

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
# ─── libxmp-lite: MOD/S3M/XM/IT ─────────────────────────────────────────
# Vendorizado como fuentes, igual que el resto: sin CMake, sin .a externo,
# sin paso de build aparte. Lista completa y ordenada tomada de
# cmake/libxmp-sources.cmake del tarball oficial; ver
# $(DEPS_DIR)/libxmp-lite/VENDOR.md.
#
# DOS defines, y las DOS hacen falta:
#
# -DLIBXMP_CORE_PLAYER: es lo que convierte libxmp en libxmp-lite. Recorta
# a cuatro loaders y, lo importante aquí, deja fuera la rama de mod_load.c
# que abre ficheros de instrumento sueltos con fopen(). Sin ella habría un
# camino de E/S que no pasa por el VFS de Libretro.
#
# -DLIBXMP_STATIC: TIENE que aplicar también a src/libretro.cpp y a
# src/engine/xmp_engine.hpp, no solo a los .c de aquí. Sin ella, xmp.h
# declara cada símbolo __declspec(dllimport) en el build de MinGW y el
# enlazado del .dll falla con referencias __imp_xmp_*. Por eso va en
# XMP_CFLAGS y XMP_CFLAGS se añade a CXXFLAGS, no solo a CFLAGS.
# xmp_engine.hpp lleva un #error de guardia por si alguien las separa.
XMP_CFLAGS := -DLIBXMP_CORE_PLAYER -DLIBXMP_STATIC

XMP_SOURCES := \
  $(DEPS_DIR)/libxmp-lite/src/virtual.c      $(DEPS_DIR)/libxmp-lite/src/format.c \
  $(DEPS_DIR)/libxmp-lite/src/period.c       $(DEPS_DIR)/libxmp-lite/src/player.c \
  $(DEPS_DIR)/libxmp-lite/src/read_event.c   $(DEPS_DIR)/libxmp-lite/src/misc.c \
  $(DEPS_DIR)/libxmp-lite/src/dataio.c       $(DEPS_DIR)/libxmp-lite/src/lfo.c \
  $(DEPS_DIR)/libxmp-lite/src/scan.c         $(DEPS_DIR)/libxmp-lite/src/control.c \
  $(DEPS_DIR)/libxmp-lite/src/filter.c       $(DEPS_DIR)/libxmp-lite/src/effects.c \
  $(DEPS_DIR)/libxmp-lite/src/flow.c         $(DEPS_DIR)/libxmp-lite/src/mixer.c \
  $(DEPS_DIR)/libxmp-lite/src/mix_all.c      $(DEPS_DIR)/libxmp-lite/src/load_helpers.c \
  $(DEPS_DIR)/libxmp-lite/src/load.c         $(DEPS_DIR)/libxmp-lite/src/filetype.c \
  $(DEPS_DIR)/libxmp-lite/src/hio.c          $(DEPS_DIR)/libxmp-lite/src/smix.c \
  $(DEPS_DIR)/libxmp-lite/src/memio.c        $(DEPS_DIR)/libxmp-lite/src/rng.c \
  $(DEPS_DIR)/libxmp-lite/src/win32.c \
  $(DEPS_DIR)/libxmp-lite/src/loaders/common.c \
  $(DEPS_DIR)/libxmp-lite/src/loaders/itsex.c \
  $(DEPS_DIR)/libxmp-lite/src/loaders/sample.c \
  $(DEPS_DIR)/libxmp-lite/src/loaders/xm_load.c \
  $(DEPS_DIR)/libxmp-lite/src/loaders/mod_load.c \
  $(DEPS_DIR)/libxmp-lite/src/loaders/s3m_load.c \
  $(DEPS_DIR)/libxmp-lite/src/loaders/it_load.c
# win32.c compila a objeto vacío fuera de Windows (todo su contenido está
# bajo #ifdef _WIN32); se lista igual para no bifurcar la lista por
# plataforma, como sí hay que hacer con StrUtils-CPConv de libvgm.
# libxmp-lite es MIT -- ver THIRD-PARTY-LICENSES.md.

GME_EXT_SOURCES := $(DEPS_DIR)/game-music-emu/gme/ext/emu2413.c
# Nes_Vrc7_Apu.cpp (Konami VRC7 FM synth) depends on emu2413.c, which
# lives outside the plain gme/*.cpp wildcard.
# libgme is LGPL-2.1, no usage restriction -- see THIRD-PARTY-LICENSES.md.

# ─── vgmstream: XA/streaming backend (USE_VGMSTREAM=1) ──────────────────
#
# Vendored WHOLE (all 663 .c), on purpose. The size lever is NOT which
# files ship, it is which entries stay in init_vgmstream_functions[] in
# deps/vgmstream/vgmstream_init.c: --gc-sections drops every parser no
# table entry reaches. Measured on this tree: pruning meta/ from 454 to 2
# files produced a BYTE-IDENTICAL stripped .so. Deleting sources would
# only cost re-pruning work on every upstream update.
#
# -std=gnu99, not c99: meta/adx.c uses M_PI/M_SQRT2, which strict c99
# hides behind __STRICT_ANSI__.
#
# base/streamfile_stdio.c is EXCLUDED (project rule: no fopen). It is
# replaced by base/streamfile_stdio_stub.c, which is not optional:
# base/api_libsf.c still references open_stdio_streamfile{,_by_file}.
# A Linux .so links fine without them (shared objects tolerate undefined
# symbols); a MinGW .dll does NOT and fails the link.
#
# No VGM_USE_* define is set, so no external codec library is pulled in;
# vgmstream then needs only libc + libm.
VGMSTREAM_DIR     := $(DEPS_DIR)/vgmstream
# SELF-CONTAINED flag set, deliberately NOT the project-wide $(CFLAGS).
# CFLAGS carries -D_POSIX_C_SOURCE=200809L (required by libvgm, see the
# libvgm block), and that define switches glibc OFF of _DEFAULT_SOURCE,
# which is what declares M_PI/M_SQRT2 in <math.h>. meta/adx.c uses both,
# so inheriting CFLAGS makes vgmstream fail to compile. Isolating the
# flags also keeps libvgm's and vgmstream's defines from leaking into
# each other.
VGMSTREAM_CFLAGS  := -std=gnu99 -Wall -fPIC -O2 -DMINIZ_NO_STDIO \
                     $(SIZEFLAGS_C) $(SANFLAGS)
VGMSTREAM_SOURCES := $(shell find $(VGMSTREAM_DIR) -name '*.c')
VGMSTREAM_OBJS    := $(VGMSTREAM_SOURCES:.c=.o)

ifeq ($(USE_VGMSTREAM),1)
INCLUDES += -I$(VGMSTREAM_DIR) -I$(VGMSTREAM_DIR)/util
CXXFLAGS += -DAOLIB_WITH_VGMSTREAM=1
VGMSTREAM_ALL_OBJS := $(VGMSTREAM_OBJS) src/engine/vgmstream_api.o
else
VGMSTREAM_ALL_OBJS :=
endif

# vgmstream .c files need their own -std; the generic %.o rule would use
# the project-wide CFLAGS (-std=c99) and break meta/adx.c.
$(VGMSTREAM_DIR)/%.o: $(VGMSTREAM_DIR)/%.c
	$(CC) $(VGMSTREAM_CFLAGS) $(DEPFLAGS) -I$(VGMSTREAM_DIR) -I$(VGMSTREAM_DIR)/util -c $< -o $@

OBJS := $(CORE_SOURCES:.cpp=.o) $(GME_SOURCES:.cpp=.o) $(GME_EXT_SOURCES:.c=.o) $(ZLIB_SOURCES:.c=.o) $(MINIZIP_SOURCES:.c=.o) $(SEVENZIP_SOURCES:.c=.o) $(LIBVGM_OBJS) $(XMP_SOURCES:.c=.o) $(VGMSTREAM_ALL_OBJS)

# Ambas familias, no solo CFLAGS: libretro.cpp incluye xmp_engine.hpp y
# necesita LIBXMP_STATIC. Ver el bloque de libxmp-lite más arriba.
CXXFLAGS += $(XMP_CFLAGS)
CFLAGS   += $(XMP_CFLAGS)

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



# ═══════════════════════ 'make dist' ═══════════════════════════════
#
# dist/ doesn't update itself -- always rebuild from a clean tree and
# copy the result over dist/, never the other way around. The .info
# lives at the repo root (aolib_libretro.info, single source of truth)
# and is copied here; edit the root copy, not the ones under dist/.
#
# 'dist' builds Linux only. Windows is a separate target since it needs
# `make clean` in between and the MinGW toolchain isn't always available.
# `clean` is invoked from the RECIPE, not listed as a prerequisite: as a
# prerequisite it would run at most once per make invocation, so
# `make dist-all` would hand the Windows link the Linux .o files left by
# `dist` -- the exact stale-object trap warned about at the top of this
# file, and it fails with unresolved libstdc++ symbols.
# USE_VGMSTREAM is passed through so `make dist USE_VGMSTREAM=1` produces
# the shipped configuration; without it dist builds the core as before.
dist:
	$(MAKE) clean
	$(MAKE) all USE_PSF_ENGINE=1 USE_VGMSTREAM=$(USE_VGMSTREAM)
	mkdir -p dist-linux
	cp aolib_libretro.so dist-linux/aolib_libretro.so
	# strip ONLY here, never on the working build: `make all` must keep its
	# symbols so nm/addr2line still work when debugging a crash report.
	strip --strip-all dist-linux/aolib_libretro.so
	cp aolib_libretro.info dist-linux/aolib_libretro.info
	@echo "dist-linux/aolib_libretro.{so,info} updated (info copied from repo root)."
	@ls -la dist-linux/aolib_libretro.so dist-linux/aolib_libretro.info

dist-windows:
	$(MAKE) clean
	$(MAKE) all USE_PSF_ENGINE=1 PLATFORM=windows USE_VGMSTREAM=$(USE_VGMSTREAM)
	mkdir -p dist-windows
	cp aolib_libretro.dll dist-windows/aolib_libretro.dll
	# MinGW strip, not the host one: the host strip does not understand
	# PE-COFF and either refuses the file or corrupts the export table.
	x86_64-w64-mingw32-strip --strip-all dist-windows/aolib_libretro.dll
	cp aolib_libretro.info dist-windows/aolib_libretro.info
	@echo "dist-windows/aolib_libretro.{dll,info} updated (info copied from repo root)."
	@ls -la dist-windows/aolib_libretro.dll dist-windows/aolib_libretro.info

dist-all: dist dist-windows

clean:
	rm -f $(OBJS) $(TARGET)
	rm -f $(VGMSTREAM_OBJS) src/engine/vgmstream_api.o
	# StrUtils-CPConv_{Win,IConv}.o: solo UNO de los dos está en $(OBJS)
	# según PLATFORM, así que `make clean` en Linux dejaba atrás el objeto
	# de Windows y viceversa. Ese residuo se cuela en el enlace siguiente y
	# da "undefined reference" o un objeto de la arquitectura equivocada.
	# Se borran los dos siempre, independientemente de la plataforma.
	rm -f $(DEPS_DIR)/libvgm/utils/StrUtils-CPConv_Win.o \
	      $(DEPS_DIR)/libvgm/utils/StrUtils-CPConv_IConv.o
	rm -f $(AOSDK_ENGINE_SOURCES:.c=.o) src/engine/aosdk_host_glue.o
	rm -f deps/libvgm/emu/cores/c6280intf.o
	rm -f deps/libvgm/emu/cores/es5506.o
	rm -f deps/game-music-emu/gme/Ym2612_MAME.o deps/game-music-emu/gme/Ym2612_Nuked.o
	rm -f tests/f21_xmp_engine tests/f22_vgmstream_formats tests/f23_vgmstream_vfs tests/f24_vgmstream_engine tests/f25_vgmstream_dispatch tests/z03_zip_end_to_end
	# .d files from -MMD live next to each .o
	find src tests deps -name '*.d' -delete 2>/dev/null || true

# ─── test del backend libxmp-lite ───────────────────────────────────────
# Necesita módulos reales; se le pasan por MODULES=. Verifica metadatos,
# tamaño de bloque, reinicio bit-perfect (FNV-1a) y que el EOT cae en el
# frame calculado.
test-f21: tests/f21_xmp_engine.cpp $(XMP_SOURCES)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o tests/f21_xmp_engine \
	    tests/f21_xmp_engine.cpp $(XMP_SOURCES) -lm
	./tests/f21_xmp_engine $(MODULES)

# ─── test del backend vgmstream ─────────────────────────────────────────
# Necesita ficheros reales; se le pasan por STREAMS=. Sirve cada uno por
# un libstreamfile_t sobre memoria (sin stdio, igual que hará el adaptador
# del VFS) y verifica detección, decodificación no silenciosa y reinicio
# bit-perfect.
#
# Las fuentes de vgmstream se compilan aparte con VGMSTREAM_CFLAGS: pasar
# .c por $(CXX) rompe (`or` es palabra reservada en C++, y malloc necesita
# cast), y $(CFLAGS) oculta M_PI. Ver deps/vgmstream/VENDOR.md.
test-f22: tests/f22_vgmstream_formats.cpp $(VGMSTREAM_OBJS) src/engine/vgmstream_api.o
	$(CXX) $(CXXFLAGS) -I$(VGMSTREAM_DIR) -I$(VGMSTREAM_DIR)/util \
	    -o tests/f22_vgmstream_formats \
	    tests/f22_vgmstream_formats.cpp $(VGMSTREAM_OBJS) \
	    src/engine/vgmstream_api.o -lm
	./tests/f22_vgmstream_formats $(STREAMS)

# ─── test del adaptador VFS de vgmstream ────────────────────────────────
# Usa un IVFSBridge falso que reproduce las dos trampas de retro_vfs:
# seek() devuelve 0/-1 y no la posición, y read() puede quedarse corto.
# Comprueba que disco y memoria dan audio bit-idéntico.
test-f23: tests/f23_vgmstream_vfs.cpp $(VGMSTREAM_OBJS) src/engine/vgmstream_api.o
	$(CXX) $(CXXFLAGS) $(INCLUDES) -I$(VGMSTREAM_DIR) -I$(VGMSTREAM_DIR)/util \
	    -o tests/f23_vgmstream_vfs \
	    tests/f23_vgmstream_vfs.cpp $(VGMSTREAM_OBJS) \
	    src/engine/vgmstream_api.o -lm
	./tests/f23_vgmstream_vfs $(STREAMS)

# ─── test de VgmstreamEngine ────────────────────────────────────────────
# Contrato de IAudioEngine: bloque de 735 frames, remuestreo a 44100,
# subsongs, EOT y CERO asignaciones en render() (se cuentan de verdad,
# reemplazando operator new).
test-f24: tests/f24_vgmstream_engine.cpp $(VGMSTREAM_OBJS) src/engine/vgmstream_api.o
	$(CXX) $(CXXFLAGS) $(INCLUDES) -I$(VGMSTREAM_DIR) -I$(VGMSTREAM_DIR)/util \
	    -o tests/f24_vgmstream_engine \
	    tests/f24_vgmstream_engine.cpp $(VGMSTREAM_OBJS) \
	    src/engine/vgmstream_api.o -lm
	./tests/f24_vgmstream_engine $(STREAMS)

# ─── test del enrutado por extensión ────────────────────────────────────
# No necesita contenido: comprueba que la lista única no colisiona con
# otros motores y que el .info dice lo mismo que el código.
test-f25: tests/f25_vgmstream_dispatch.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -DAOLIB_WITH_VGMSTREAM=1 \
	    -o tests/f25_vgmstream_dispatch tests/f25_vgmstream_dispatch.cpp
	./tests/f25_vgmstream_dispatch aolib_libretro.info

# ─── prueba de extremo a extremo con un .zip real ───────────────────────
# Carga el archivo por el mismo camino que el core (minizip sobre el VFS)
# y reporta memoria retenida y tiempos. Se le pasa el .zip por ZIP=.
test-z03: tests/z03_zip_end_to_end.cpp $(VGMSTREAM_OBJS) src/engine/vgmstream_api.o \
          $(ZLIB_SOURCES:.c=.o) $(MINIZIP_SOURCES:.c=.o)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -I$(VGMSTREAM_DIR) -I$(VGMSTREAM_DIR)/util \
	    -o tests/z03_zip_end_to_end \
	    tests/z03_zip_end_to_end.cpp $(VGMSTREAM_OBJS) src/engine/vgmstream_api.o \
	    $(ZLIB_SOURCES:.c=.o) $(MINIZIP_SOURCES:.c=.o) -lm
	./tests/z03_zip_end_to_end $(ZIP)

# test-all encadena `clean` entre objetivos a propósito: f21 y f22 no
# comparten flags de compilación y reutilizar .o entre ellos da fallos
# silenciosos.
test-all:
	$(MAKE) clean
	$(MAKE) test-f21 MODULES="$(MODULES)"
	$(MAKE) clean
	$(MAKE) test-f22 USE_VGMSTREAM=1 STREAMS="$(STREAMS)"
	$(MAKE) clean
	$(MAKE) test-f23 USE_VGMSTREAM=1 STREAMS="$(STREAMS)"
	$(MAKE) clean
	$(MAKE) test-f24 USE_VGMSTREAM=1 STREAMS="$(STREAMS)"
	$(MAKE) clean
	$(MAKE) test-f25 USE_VGMSTREAM=1
	$(MAKE) clean
	$(MAKE) test-f26 USE_VGMSTREAM=1 STREAMS="$(STREAMS)"

.PHONY: all clean dist dist-windows dist-all test-f21 test-f22 test-f23 test-f24 test-f25 test-f26 test-z03 test-all

# ─── one-off diagnostic probes (NOT part of the permanent suite) ────
# These need real, non-distributable content in /tmp and are run by hand.

# ─── test de disposición de canales ─────────────────────────────────────
# Un stream de 1 canal debe llegar al core como estéreo entrelazado
# COMPLETO. libvgmstream sólo sabe bajar de canales, así que fill()
# escribía medio buffer y el core leía muestras mono como pares L/R.
# Detecta la regresión midiendo cuántos int16 se escriben de verdad.
test-f26: tests/f26_channel_layout.cpp $(VGMSTREAM_OBJS) src/engine/vgmstream_api.o
	$(CXX) $(CXXFLAGS) $(INCLUDES) -I$(VGMSTREAM_DIR) -I$(VGMSTREAM_DIR)/util \
	    -o tests/f26_channel_layout \
	    tests/f26_channel_layout.cpp $(VGMSTREAM_OBJS) \
	    src/engine/vgmstream_api.o -lm
	./tests/f26_channel_layout $(STREAMS)
