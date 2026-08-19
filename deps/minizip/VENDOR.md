# minizip (vendored)

Upstream: `contrib/minizip` from zlib **1.3.1**
(<https://github.com/madler/zlib/tree/v1.3.1/contrib/minizip>).

Files taken verbatim except where noted: `unzip.c`, `unzip.h`, `ioapi.c`,
`ioapi.h`. Only the *reading* half is vendored; `zip.c`/`zip.h` are not,
because the core never writes archives. `crypt.h` is not vendored either:
`unzip.c` defines `NOUNCRYPT` at the top by default, so the decryption
paths are compiled out and encrypted entries are rejected.

## Why not the minizip that came with aosdk

aosdk ships zlib 1.2.1 (2003) with minizip **1.00** inside
(`deps/aosdk/zlib/unzip.c`, now removed). That version predates Zip64
entirely: it locates the central directory with the 32-bit
"end of central directory" record only and has no `unz*64` API. Any
archive written with a Zip64 central directory — which several archivers
emit even for small files, and which 7-Zip can be told to force — fails at
enumeration, and the failure surfaces as "no supported files found in the
.zip", pointing at the content instead of at the container.

zlib itself stays at 1.2.1: minizip only calls `inflateInit2`/`inflate`/
`inflateEnd` and `crc32`, all present and ABI-compatible in 1.2.1. The two
are versioned independently upstream as well.

## Local patch: `ioapi.c` stdio backend removed

Upstream `ioapi.c` supplies a default `FILE*` backend
(`fopen_file_func` and friends). This core routes **all** file I/O through
the Libretro VFS (`src/vfs_bridge.hpp`); `src/zip_vfs_adapter.hpp` installs
its own `zlib_filefunc64_def` and the archive is opened with
`unzOpen2_64()`, so that backend is never reached.

It is not merely unused, though: leaving it in links a real `fopen()` path
into the shipped binary, which this project forbids, and drags in the
`fopen64`/`ftello64`/`fseeko64` + `_LARGEFILE64_SOURCE` dance that upstream
needs to be portable — a needless cross-compilation risk under MinGW.

The patch therefore keeps the dispatchers (`call_zopen64`, `call_zseek64`,
`call_ztell64`, `fill_zlib_filefunc64_32_def_from_filefunc32`), which the
64/32 bridging in `unzip.c` requires, and replaces the stdio callbacks with
ones that always fail. `fill_fopen_filefunc()` and `fill_fopen64_filefunc()`
still exist because `unzOpen()`/`unzOpen64()` reference them, but a caller
reaching them gets a `NULL` handle rather than silent direct I/O.

`unzip.c`, `unzip.h` and `ioapi.h` are unmodified.

## Compression methods

zlib covers Store (0) and Deflate (8). Everything else a ZIP may legally
use — Deflate64 (9), BZip2 (12), LZMA (14), Zstandard (93), XZ (95),
PPMd (98) — is outside zlib and is rejected. `enumerate_zip()`
(`src/zip_playlist.hpp`) names the offending method in the log instead of
skipping the entry silently; see `zip_compression_method_supported()`.
