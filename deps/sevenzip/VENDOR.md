# 7-Zip SDK (vendored, decode-only subset)

Upstream: the official 7-Zip SDK, C sources, taken from the
`C/` directory of the `mcmilk/7-Zip-zstd` mirror on GitHub (a fork of the
official 7-Zip sources plus an optional zstd codec this project does not
use or vendor). All files are public domain, written by Igor Pavlov.

## What was taken and why

Reading a `.7z` needs three things: parsing the archive's own header
format (`7zArcIn.c`/`7z.h`), decoding whichever coders a given folder
used, and a stream abstraction to read through (`7zStream.c`). This
project only ever *reads* archives created by someone else, so the
encoder side of the SDK (`7zEnc*`, `LzmaEnc.c`, `Lzma2Enc.c`, `LzFind*`,
`BwtSort.c`...) is not vendored at all.

Files taken:

- `7z.h`, `7zArcIn.c` — archive header format (`CSzArEx`, `SzArEx_Open`,
  `SzArEx_Extract`).
- `7zStream.c` — `ISeekInStream` → `ILookInStream` bridge
  (`CLookToRead2`), used as-is; see `src/sevenzip_vfs_adapter.hpp` for
  the VFS-backed `ISeekInStream` this core supplies underneath it.
- `7zAlloc.c/.h`, `7zBuf.c/.h`, `7zBuf2.c` — allocation and growable
  buffer helpers `SzArEx_Open`/`SzArEx_Extract` depend on internally.
- `7zCrc.c/.h`, `7zCrcOpt.c` — CRC32, needed to verify decoded folders.
- `7zDec.c` — folder decoding dispatcher: routes each coder in a folder
  to the right decoder (Copy, LZMA, LZMA2, BCJ2, and the branch filters
  below).
- `LzmaDec.c/.h`, `Lzma2Dec.c/.h` — the actual LZMA/LZMA2 decoders. This
  is the whole reason `.7z` support exists: minizip (`deps/minizip`)
  only ever covered Store/Deflate, and several archivers default to
  LZMA for `.7z`.
- `Bra.c/.h`, `Bra86.c`, `BraIA64.c`, `Delta.c/.h`, `Bcj2.c/.h` — branch
  and delta filters. Not needed for the audio-only content this core
  actually plays, but `7zDec.c` references them unconditionally for any
  solid block that used those filters, so they're included for
  correctness on `.7z` archives this project didn't create.
- `CpuArch.c/.h`, `RotateDefs.h`, `7zCrcEmu.h`, `7zWindows.h`,
  `Compiler.h`, `Precomp.h`, `7zTypes.h`, `7zVersion.h` — shared type
  definitions and CPU-feature detection the above depend on.

`Ppmd7.h` is deliberately not vendored: `7zDec.c` only includes it under
`Z7_PPMD_SUPPORT`, which is `#define`-commented-out upstream and left
that way here — PPMd is not a codec any PSF/VGM/etc. rip in the wild
uses, and pulling in the PPMd decoder for content that will never
exercise it isn't worth the extra surface.

## What was deliberately left out: `7zFile.c`/`7zFile.h`

The SDK's own `7zFile.c` is a `fopen()`/`fread()`-based `ISeekInStream`
implementation — the equivalent of what `ioapi.c` was for minizip before
its own patch (see `deps/minizip/VENDOR.md`). This project routes all
file I/O through the Libretro VFS, so `7zFile.c` is simply not vendored
at all: `src/sevenzip_vfs_adapter.hpp` supplies the VFS-backed
`ISeekInStream` in its place, and nothing in this tree ever references
`7zFile.h`.

## No local patches

Unlike minizip's `ioapi.c`, none of the files above needed patching:
there was no default-fopen path to neutralize, because that path
(`7zFile.c`) was never vendored in the first place.

## A note on the seek/tell contract

`ISeekInStream::Seek` is documented to return the *resulting absolute
position* in its in/out `Int64*` parameter — the SDK relies on this to
find a `.7z`'s size via `Seek(0, SEEK_END)`. `retro_vfs_seek()`, however,
only confirms success or failure (0 / -1), per `libretro.h`'s own
documentation. `src/sevenzip_vfs_adapter.hpp::vfs_seek()` asks for the
real position separately with `stream_tell()` after a successful seek.
This is the same VFS whose `stream_seek()` a stale comment in
`vfs_bridge.hpp` used to describe backwards — see that file's current
comment for the corrected explanation.
