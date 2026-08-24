# vgmstream — vendored source

## Origin

- Upstream: https://github.com/vgmstream/vgmstream
- Commit: `79bc65b52cddc61b859679c1c47dc57092872ae1` (2026-08-20)
- License: ISC-style permissive, see `COPYING`.

Taken from `src/` only. The CLI, the Winamp/foobar2000/Audacious/XMPlay
plugins, `ext_libs/` and `ext_includes/` are NOT vendored.

## What is compiled

No `VGM_USE_*` define is set, so every optional codec backend (Vorbis,
MPEG, FFmpeg, ATRAC9, CELT, Speex, G.719, G.722.1) compiles out. What
remains needs only libc and libm — no external library, no `.a`, no
CMake, exactly like the other dependencies here.

This is not a limitation to work around later: ATRAC3 (`.at3`) is
reachable *only* through `coding_FFmpeg` (see `meta/riff.c:437`), and
FFmpeg cannot be built from this Makefile. Any future ATRAC3 support has
to come from a standalone vendorable decoder, not from enabling
`VGM_USE_FFMPEG`.

## Removed files

### `base/streamfile_stdio.c` — removed

Project rule: no real filesystem I/O, everything goes through the
libretro VFS. This file is the only `fopen()` backend in the tree.

It is replaced by `base/streamfile_stdio_stub.c`, which is **not
optional**: `base/api_libsf.c` still references `open_stdio_streamfile`
and `open_stdio_streamfile_by_file` from
`libstreamfile_open_from_stdio()` / `_from_file()`. Without the stub a
Linux `.so` still links (shared objects tolerate undefined symbols) but
a MinGW `.dll` does not. The stub returns `NULL`, so those two entry
points fail cleanly and no `fopen` is reachable from any code path.

The remaining stdio references live in `util/miniz.c` and are compiled
out by `-DMINIZ_NO_STDIO`.

### `meta/` pruned from 454 to 47 files

See "The size lever" below. `Makefile`, `Makefile.autotools.am`,
`CMakeLists.txt`, `*.vcxproj*` and `vgmstream-config.cmake.in` were also
dropped; the build is driven entirely by the project Makefile.

## The size lever

Two things control how much of vgmstream ends up in the binary, and they
behave differently per platform. Both were measured on this tree, not
assumed.

**Linux / ELF** — the lever is `init_vgmstream_functions[]` in
`vgmstream_init.c`. `--gc-sections` drops every parser no table entry
reaches. Deleting source files on top of that changes nothing:

| build | stripped `.so` |
|---|---|
| table pruned to 1 format, all 454 `meta/*.c` present | 2,190,264 |
| same table, `meta/` cut to 2 files | 2,190,264 |

Byte-identical.

**Windows / PE-COFF** — `--gc-sections` is essentially inert (measured:
0.6% on the baseline core, 0.17% between a 570-entry and a 1-entry
table). There the only lever is physically removing files:

| build | stripped `.dll` |
|---|---|
| table pruned to 1 format, all `meta/*.c` present | 3,248,128 |
| same table, `meta/` cut to 2 files | 2,482,688 |

So `meta/` is pruned for Windows' sake, and the init table is pruned for
Linux's. Both are needed; neither alone is enough.

## Keeping the two in sync

`meta/` must always hold the exact closure of the init table. Do not edit
it by hand — `tests/vgmstream_closure.py` computes it from the table and
verifies the result links with `-Wl,--no-undefined`:

    python3 tests/vgmstream_closure.py --check     # CI / pre-commit
    python3 tests/vgmstream_closure.py --apply     # after editing the table

Three files in the current closure are not obvious and must not be
"cleaned up" by hand:

- `meta/ngc_adpdtk.c` — defines `init_vgmstream_dtk`; the file name does
  not match the function name, so it is found by definition, not by name.
- `meta/xvag.c` — nothing in the table references it, but `meta/bnk_sony.c`
  does. Removing it breaks the link. On Linux `--gc-sections` discards it;
  on Windows it stays as dead weight already accounted for in the measured
  size.
- `meta/silence.c` — pulled in by the layered/segmented layouts.
- `meta/msf.c` and `meta/xma.c` — called by `meta/ubi_jade.c` for its PS3 and
  X360 variants, never by the table. Neither is reachable from the extensions
  this core accepts, but the link needs the symbols.
- `meta/ps_headerless.c` — added for `.mib`/`.mi4` distributed without their
  `.mih` header, which is how most rips of them circulate. It guesses
  interleave and channel count by inspecting the data and takes the sample
  rate from the extension. Upstream calls it "an ugly crutch"; it is kept
  because the alternative for those files is nothing at all, and it sits
  AFTER `mib_mih` in the init table, so a `.mib` that does have its `.mih`
  still gets the exact values rather than the guess. Cost: 32 bytes.

These are listed in the script's `FORCED` set, each with its reason. They are the two the table cannot discover on its own; if a future
table change adds another, the link probe reports the unresolved symbol
and the file goes in the same set.

## Local patches

Nine changes, all documented with their evidence in
[deps/patches/README.md](../patches/README.md):

- `base/streamfile_stdio_stub.c` — new file, described above.
- `meta/vag.c` — accept the `.khv` extension.
- `meta/caf.c` — the block sweep walked backwards on a short read.
- `meta/ps_headerless.c` — bound the interleave it guesses.
- `meta/ubi_jade.c` — the RIFF size check demanded an exact match.
- `meta/svag_kcet.c` — relax an over-strict bad-rip heuristic.
- `base/api_libsf_cache.c` — `cache_read()` ignored the requested offset.
- `libvgmstream.h` + `base/api_decode_open.c` — expose resampling through
  the public config.
- `base/mixer.c` + `base/resampler.c` — free the resampler.

The last three are only reachable once the public API is driven from a
host, which is why vgmstream's own CLI never hits them. Updating to a
newer vgmstream means re-copying `src/`, re-applying these, re-applying
the init table, and re-running the closure script.

## Known upstream UB (not patched)

Building with `-fsanitize=undefined` and decoding real content reports two
left-shifts of negative values in the hot ADPCM paths:

    coding/psx_decoder.c:106      left shift of negative value
    coding/ngc_dsp_decoder.c:43   left shift of negative value

Both are formally undefined but well-defined on every two's-complement
target, and both predate this integration. Decoded audio is bit-identical
with and without sanitizers. They are recorded here so a future
sanitizer run does not read as a regression introduced locally; patching
hot decode loops for zero measurable gain is not worth the risk.

## Build flags

`VGMSTREAM_CFLAGS` in the Makefile is deliberately self-contained and does
**not** inherit the project-wide `$(CFLAGS)`. Two reasons:

- `-std=gnu99`, not `-std=c99`: `meta/adx.c` uses `M_PI` and `M_SQRT2`,
  which strict C99 hides behind `__STRICT_ANSI__`.
- `$(CFLAGS)` carries `-D_POSIX_C_SOURCE=200809L` (required by libvgm).
  That define switches glibc off `_DEFAULT_SOURCE`, which is what declares
  `M_PI`/`M_SQRT2` in `<math.h>` — inheriting it makes vgmstream fail to
  compile even with `-std=gnu99`.

Isolating the flags also stops libvgm's and vgmstream's defines from
leaking into each other.

## Warning baseline

Compiling this closure with `-Wall` emits **14 warnings**, all upstream,
none in `src/`. The count is unchanged by the local patches. They are unused variables and one `-Wformat-truncation`
in `util/meta_utils.c:75`. The count is recorded here so a future update
that raises it is visible rather than absorbed into the noise; it is
checked by `verificar_antes_de_subir.sh`.

## meta/ closure grown from 34 to 40 files

Five formats added, each identified by header magic rather than by
extension:

| ext | magic | parser | content used to verify |
|---|---|---|---|
| `.cfn` | `CAF ` | `meta/caf.c` | Baten Kaitos (GC), 60 files |
| `.sng` | `SCHl` | `meta/ea_schl_standard.c` + `meta/ea_schl.c` | Boom Blox, 93 files |
| `.rstm`, `.rsm` | `RSTM` | `meta/rstm_rockstar.c` | Bully (PS2), 227 files |
| `.ydsp` | `YDSP` | `meta/ydsp.c` | — |
| `.adp` | none | `meta/ngc_adpdtk.c` (already vendored) | Crazy Taxi (GC), 9 files |

`.rstm` shares its magic with Nintendo BRSTM. They are told apart at
0x04: BRSTM carries a `FEFF` BOM, Rockstar leaves it zero. The Rockstar
reading was cross-checked arithmetically (file size - 0x800 equals the
declared stream_size exactly).

`.adp` is claimed by nine upstream parsers and `.sng` by four. Rather
than vendoring all of them, the six `.adp` candidates (`adp_konami`,
`adp_ongakukan`, `adp_qd`, `adp_wildfire`, `derf`, `nxap`) were added
and then removed one at a time: none was required. `meta/ngc_adpdtk.c`,
already in the tree, is the one that reads it — confirmed by dropping
`init_vgmstream_dtk` and watching the file stop opening. All six were
removed again.

`caf.c` and `ydsp.c` pulled in no new dependencies: both use
`coding_NGC_DSP` and layouts (`blocked_caf`, `interleave`) that were
already linked.

`meta/ea_schl.c` defines `load_vgmstream_ea_schl/_bnk/_pt` and is not
reachable through an `init_vgmstream_*` name, so it lives in the
closure tool's `FORCED` set.

Measured with `make dist`: `.so` 2,305,344 -> 2,321,760 bytes,
+16,416 (+0.71%).
