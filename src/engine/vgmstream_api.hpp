// vgmstream_api.hpp
//
// Thin C++ facade over vgmstream's public C API (libvgmstream.h). Exists
// so that exactly one translation unit includes the C headers: their
// names are generic enough (util.h, streamfile.h, log.h) to collide with
// the other vendored trees if they leak into libretro.cpp.
//
// This is also what keeps vgmstream linked in at all. --gc-sections walks
// from the exported retro_* symbols outwards, so anything no chain
// reaches is dropped; the handle type and these functions are that chain.
//
// Deliberately NOT an IAudioEngine subclass. The engine needs a VFS-backed
// STREAMFILE to hand to open(), and that adapter is a separate piece.
// Until it exists, open() takes the libstreamfile_t the caller built.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace aolib {
namespace vgms {

// Opaque handle. Really a libvgmstream_t*, kept as void* so callers never
// need libvgmstream.h.
using Handle = void*;

struct StreamInfo {
    int      channels     = 0;
    int      sample_rate  = 0;   // rate the decoder actually outputs; equals
                                 // OpenConfig::output_rate when that was set,
                                 // otherwise the stream's native rate
    int      native_rate  = 0;   // rate stored in the file: 37800 for XA,
                                 // 32000 for GameCube DSP, etc.
    int64_t  total_frames = 0;   // 0 = unknown or infinite
    int64_t  loop_start   = 0;
    int64_t  loop_end     = 0;
    bool     loops        = false;
    int      subsongs     = 1;
    std::string codec_name;      // "CD-XA", "PSX", "NGC DSP"...
    // Nombre del PARSER (meta_name), que no es lo mismo que el códec: CAF
    // de tri-Crescendo lleva ADPCM de GameCube dentro, así que un .cfn de
    // Baten Kaitos parseado por caf.c reporta códec "NGC DSP". Sin este
    // campo el log sólo enseñaba el códec y parecía un fallback a GC DSP.
    std::string meta_name;
};

struct OpenConfig {
    double loop_count   = 2.0;
    double fade_time    = 10.0;
    double fade_delay   = 0.0;
    bool   play_forever = false;
    bool   ignore_loop  = false;
    int    downmix_to   = 2;     // 0 = leave as-is
    // Resamples inside vgmstream so the core can stay pinned at one rate.
    // 0 = keep the stream's native rate (37800 for XA, 32000 for GC DSP...).
    int    output_rate  = 0;
};

// 'sf' is a libstreamfile_t* built by the caller. Returns nullptr on
// failure. 'subsong' is 1-based; 0 means "first/default".
Handle open(void* sf, int subsong, const OpenConfig& cfg);

void close(Handle h) noexcept;

// Fills 'buf' with up to 'frames' interleaved stereo int16 frames.
// Returns frames actually written. Does not allocate.
std::size_t fill(Handle h, int16_t* buf, std::size_t frames) noexcept;

bool done(Handle h) noexcept;
void reset(Handle h) noexcept;
void seek(Handle h, int64_t frame) noexcept;

bool info(Handle h, StreamInfo& out) noexcept;

// Title as vgmstream derives it (filename, or the subsong's own name for
// formats that carry one, e.g. XA's file+channel marker).
std::string title(Handle h);

// vgmstream's own version string, for the log line on load.
const char* version() noexcept;

} // namespace vgms
} // namespace aolib
