# Third-Party Licenses

AOLIB's own code (everything under `src/`) is GPL-2.0-or-later — see
[LICENSE](LICENSE). This file covers the third-party libraries vendored
under `deps/`, and the practical restriction that comes out of mixing
them. If you only care about "can I use this," read the last section
first.

## libgme — `deps/game-music-emu`

LGPL-2.1. Author: Shay Green. Covers SPC, NSF, NSFE, GBS, HES, KSS, SAP,
AY, and GYM playback. No usage restriction beyond LGPL's own terms.

## libvgm — `deps/libvgm`

The player/loader code (ValleyBell) is GPL-2.0-or-later. The chip cores
it enables are a mix, inherited mostly from the MAME project:

- **BSD-3-Clause**: SN76496, OKIM6295, C140, K053260, GA20, RF5C68,
  C6280, ES5503, K051649, X1-010, C219, C352, SegaPCM, K054539, MultiPCM
  (YMW258), YMZ280B.
- **GPL-2.0-or-later**: YM2413, YM2151, YM3812/YM3526 (`fmopl.c`),
  YM2203/YM2608/YM2610 (`fmopn.c`), YMF262, NES APU, PWM/RF5C164 (from
  the Gens project — Stéphane Dallongeville, Stéphane Akhoun, David
  Korth), Virtual Boy VSU (from Mednafen).
- **MIT**: AY8910/YM2149 (`emu2149.c`, Mitsutaka Okazaki).
- **No license header in the vendored file**: YM2612 (Gens core, same
  author/project as PWM above), QSound (superctr), WonderSwan audio.

## libxmp-lite — `deps/libxmp-lite`

MIT. Copyright (C) 1996-2026 Claudio Matsuoka and Hipolito Carraro Jr.
Covers MOD, S3M, XM, and IT playback. Vendored from the official
`libxmp-lite-4.7.1.tar.gz` release with no local patches — see
[deps/libxmp-lite/VENDOR.md](deps/libxmp-lite/VENDOR.md). The full
license text ships in `deps/libxmp-lite/README`. MIT imposes no usage
restriction beyond preserving the copyright notice.

## aosdk — `deps/aosdk`

Powers PSF1, PSF2, and SSF/Saturn playback. Three different licenses
inside the same SDK:

- **BSD-3-Clause**: the SDK itself and most of the engine glue —
  `corlett.c`, `eng_psf.c`, `eng_psf2.c`, `eng_ssf.c`, `psx_hw.c`,
  `utils.c`, and Saturn's SCSP sound chip (`scsp.c` and friends).
- **GPL-2.0**: the PS1/PS2 SPU emulation, `peops`/`peops2` (Pete
  Bernert).
- **Non-commercial**: the PlayStation R3000A CPU core (`psx.c`, MAME
  license, copyright Nicola Salmoria and the MAME team) and Saturn's
  M68000 core, "Musashi" (Karl Stenerud).

Full authorship credits are in [README.md](README.md#credits).

## zlib — `deps/aosdk/zlib`

zlib license. Jean-loup Gailly and Mark Adler. Used for the raw inflate
that both PSF containers and `.zip` playlists rely on, so it is compiled
even without PSF/SSF support enabled.

## minizip — `deps/minizip`

zlib license. Gilles Vollant, with the Zip64 work by Even Rouault and
Mathias Svensson. Taken from `contrib/minizip` of zlib 1.3.1 and vendored
separately from the older zlib above; `ioapi.c` carries a local patch that
removes its stdio backend. See
[deps/minizip/VENDOR.md](deps/minizip/VENDOR.md). Reading `.zip` playlists
depends on this even without PSF/SSF support enabled.

## libchdr — `deps/libchdr`

BSD-3-Clause. Aaron Giles and contributors; `include/dr_libs/dr_flac.h` is
by David Reid, public domain / MIT-0. Reads CHD disc images: the hunk map,
the `zlib`/`lzma`/`huff` codecs and their CD variants (`cdzl`, `cdlz`,
`cdfl`).
Hunks compressed with `zstd` are rejected with a message rather than
supported. See [deps/libchdr/VENDOR.md](deps/libchdr/VENDOR.md).

## LZMA SDK — `deps/libchdr/deps/lzma-26.02`

Public domain. Igor Pavlov. Decode-only LZMA (`LzmaDec`), vendored inside
libchdr because that is its only consumer: CHD stores hunks with the `lzma`
and `cdlz` codecs. Only the five files libchdr includes are kept; the
encoder and the stdio-based `7zFile.c` are not vendored.

## vgmstream — `deps/vgmstream`

ISC-style permissive licence. Copyright (c) 2008-2025 Adam Gashlin,
Fastelbja, Ronny Elfert, bnnm, Christopher Snowhill, NicknineTheEagle,
bxaimc, Thealexbarney, CyberBotX, EdnessP et al, with portions by Marko
Kreen, jagarl / Kazunori Ueno, Nullsoft, Paul Hsieh, Entis-soft and Sun
Microsystems. Full text in `deps/vgmstream/COPYING`.

Covers streamed audio: CD-XA, Sony VAG and its PS2 derivatives, CRI ADX
and AHX, Sega Dreamcast/Naomi streams, Nintendo GC/Wii DSP and NDS STRM,
and the generic RIFF/GENH containers — see
[deps/vgmstream/VENDOR.md](deps/vgmstream/VENDOR.md) for the exact list
and for why `meta/` is pruned.

No optional codec backend is enabled, so nothing under `ext_libs/`
(FFmpeg, libvorbis, libmpg123, libatrac9, libcelt, libspeex) is vendored
or linked, and none of their licences apply here. The permissive terms
impose no usage restriction beyond preserving the copyright notice.

## What this means in practice

Everything above is either permissive (BSD, MIT, zlib, ISC) or standard
copyleft (LGPL, GPL) — except two files: `psx.c` and Musashi. Both carry
a **non-commercial** restriction that GPL can't override, and both are
required for PSF1/PSF2/SSF playback (`USE_PSF_ENGINE=1`).

- Building **with** `USE_PSF_ENGINE=1` (the default `dist`/`dist-windows`
  targets): the resulting binary cannot be sold or used commercially.
- Building **without** it: only BSD-3-Clause, MIT, LGPL-2.1, and
  GPL-2.0(-or-later) code is linked in, and that restriction doesn't
  apply — at the cost of losing PSF1, PSF2, and SSF.

See [LICENSE](LICENSE) for the full statement.
