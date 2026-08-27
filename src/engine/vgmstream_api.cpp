// vgmstream_api.cpp
//
// The ONLY translation unit that includes vgmstream's headers. See
// vgmstream_api.hpp for why.

#include "vgmstream_api.hpp"

extern "C" {
#include "libvgmstream.h"
#include "base/resampler.h"

// Interruptor del modo sin enumerar de meta/xa.c. No esta en ninguna
// cabecera de vgmstream porque no es de vgmstream: es un anadido de AOLIB
// sobre la copia vendorizada (ver el bloque AOLIB de ese fichero).
void xa_set_fast_open(int on);
}

namespace aolib {
namespace vgms {

namespace {

inline libvgmstream_t* as_lib(Handle h) noexcept {
    return static_cast<libvgmstream_t*>(h);
}

} // namespace

Handle open(void* sf, int subsong, const OpenConfig& cfg) {
    libvgmstream_t* lib = libvgmstream_init();
    if (!lib) return nullptr;

    libvgmstream_config_t vcfg = {};
    vcfg.loop_count   = cfg.loop_count;
    vcfg.fade_time    = cfg.fade_time;
    vcfg.fade_delay   = cfg.fade_delay;
    vcfg.ignore_loop  = cfg.ignore_loop;
    // play_forever is refused by vgmstream unless explicitly allowed, and
    // it only takes effect on files that actually declare loop points.
    vcfg.allow_play_forever = cfg.play_forever;
    vcfg.play_forever       = cfg.play_forever;
    vcfg.auto_downmix_channels = cfg.downmix_to;
    // The core's mixer is int16 stereo end to end; asking vgmstream for
    // anything else would mean converting on the hot path.
    vcfg.force_sfmt = LIBVGMSTREAM_SFMT_PCM16;
    // Resampling happens inside vgmstream's mixer, which is why
    // retro_get_system_av_info() can stay pinned at 44100 for every
    // format: XA arrives at 37800 or 18900 Hz and is converted here, not
    // by the frontend. SINC because this runs once per block on two
    // channels and the cost is not on the critical path.
    vcfg.output_sample_rate = cfg.output_rate;
    vcfg.resampler_type     = cfg.output_rate > 0 ? RESAMPLER_TYPE_SINC : 0;
    libvgmstream_setup(lib, &vcfg);

    // El interruptor de xa.c es una global de proceso -- no hay por donde
    // pasarle opciones a un parser de vgmstream -- asi que se pone y se
    // quita AQUI, alrededor de la unica llamada que la mira, igual que
    // AosdkFadeScope hace con las estaticas de corlett.c. Sin la
    // envoltura, un fallo a medias dejaria el modo puesto para la
    // siguiente apertura, que podria no ser de un CD.
    struct XaFastOpenScope {
        explicit XaFastOpenScope(bool on) : on_(on) { if (on_) xa_set_fast_open(1); }
        ~XaFastOpenScope() { if (on_) xa_set_fast_open(0); }
        bool on_;
    } xa_scope(cfg.xa_fast_open);

    if (libvgmstream_open_stream(lib, static_cast<libstreamfile_t*>(sf), subsong) < 0) {
        libvgmstream_free(lib);
        return nullptr;
    }
    return lib;
}

void close(Handle h) noexcept {
    if (h) libvgmstream_free(as_lib(h));
}

std::size_t fill(Handle h, int16_t* buf, std::size_t frames) noexcept {
    if (!h || !buf || frames == 0) return 0;
    libvgmstream_t* lib = as_lib(h);
    const int got = libvgmstream_fill(lib, buf, static_cast<int>(frames));
    if (got < 0) return 0;
    return static_cast<std::size_t>(lib->decoder ? lib->decoder->buf_samples : 0);
}

bool done(Handle h) noexcept {
    if (!h) return true;
    libvgmstream_t* lib = as_lib(h);
    return lib->decoder ? lib->decoder->done : true;
}

void reset(Handle h) noexcept {
    if (h) libvgmstream_reset(as_lib(h));
}

void seek(Handle h, int64_t frame) noexcept {
    if (h) libvgmstream_seek(as_lib(h), frame);
}

bool info(Handle h, StreamInfo& out) noexcept {
    if (!h) return false;
    const libvgmstream_format_t* f = as_lib(h)->format;
    if (!f) return false;

    out.channels     = f->channels;
    out.sample_rate  = f->sample_rate;
    out.native_rate  = f->native_sample_rate;
    // play_samples is the total AFTER loop/fade config, which is what the
    // UI should show; stream_samples is the raw stream length.
    out.total_frames = f->play_forever ? 0 : f->play_samples;
    out.loop_start   = f->loop_start;
    out.loop_end     = f->loop_end;
    out.loops        = f->loop_flag;
    // subsong_count is 0 for formats with no concept of subsongs.
    out.subsongs     = f->subsong_count > 0 ? f->subsong_count : 1;
    out.codec_name   = f->codec_name;
    out.meta_name    = f->meta_name;
    return true;
}

std::string title(Handle h) {
    if (!h) return std::string();
    char buf[256] = {0};
    libvgmstream_title_t cfg = {};
    if (libvgmstream_get_title(as_lib(h), &cfg, buf, static_cast<int>(sizeof(buf))) < 0)
        return std::string();
    return std::string(buf);
}

const char* version() noexcept {
    // Numeric 0xMMmmpppp; formatted once into a static buffer because the
    // caller only ever logs it.
    static char buf[32];
    const uint32_t v = libvgmstream_get_version();
    snprintf(buf, sizeof(buf), "%u.%u.%u",
             (v >> 24) & 0xFF, (v >> 16) & 0xFF, v & 0xFFFF);
    return buf;
}

} // namespace vgms
} // namespace aolib
