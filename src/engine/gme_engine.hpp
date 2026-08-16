// gme_engine.hpp
//
// Envuelve libgme (Game_Music_Emu, LGPL-2.1).
//
// CUIDADO con la unidad de 'count' en gme_play(emu, count, out): son
// MUESTRAS int16 entrelazadas (L,R,L,R...), no frames estéreo. Por eso
// render() pasa max_frames * 2; pasar max_frames rellenaría solo media
// petición y dejaría el resto del buffer con datos de la iteración
// anterior.

#pragma once

#include <cstring>
#include <string>

#include "iaudio_engine.hpp"

extern "C" {
#include "gme.h"
}

class GmeEngine final : public IAudioEngine {
public:
    explicit GmeEngine(int fade_length_msecs) noexcept
        : fade_length_msecs_(fade_length_msecs) {}

    ~GmeEngine() override {
        if (emu_) gme_delete(emu_);
    }

    bool open(const char* /*uri*/, const uint8_t* data, std::size_t size,
               IVFSBridge& /*vfs*/) override {
        // libgme acepta memoria directamente (need_fullpath=false para
        // estas extensiones) y hace su propia copia interna: no hace falta
        // retener 'data' más allá de esta llamada.
        const gme_err_t err = gme_open_data(data, static_cast<long>(size), &emu_, kSampleRate);
        if (err || !emu_) {
            emu_ = nullptr;
            return false;
        }

        track_count_ = gme_track_count(emu_);
        return select_track(0);
    }

    std::size_t render(int16_t* out, std::size_t max_frames) noexcept override {
        if (!emu_) return 0;
        // count = muestras entrelazadas, no frames. Ver cabecera.
        const gme_err_t err = gme_play(emu_, static_cast<int>(max_frames * 2), out);
        if (err) return 0; // error de emulación real; el llamante rellena silencio
        return max_frames; // gme_play siempre completa el buffer salvo error
    }

    bool end_of_track() const noexcept override {
        return !emu_ || gme_track_ended(emu_) != 0;
    }

    unsigned track_count() const noexcept override {
        return track_count_ > 0 ? static_cast<unsigned>(track_count_) : 1;
    }

    bool select_track(unsigned index) override {
        if (!emu_) return false;
        if (gme_start_track(emu_, static_cast<int>(index))) return false;
        current_track_ = index;
        refresh_metadata();
        return true;
    }

    const TrackMetadata& metadata() const noexcept override { return meta_; }
    const char* engine_name() const noexcept override { return "libgme"; }

private:
    static constexpr int kSampleRate = 44100;

    void refresh_metadata() noexcept {
        meta_ = TrackMetadata{};
        if (!emu_) return;

        gme_info_t* info = nullptr;
        if (gme_track_info(emu_, &info, static_cast<int>(current_track_)) || !info)
            return;

        meta_.title             = info->song    ? info->song    : "";
        meta_.game               = info->game    ? info->game    : "";
        meta_.artist             = info->author  ? info->author  : "";
        meta_.copyright_notice   = info->copyright ? info->copyright : "";
        meta_.comment            = info->comment ? info->comment : "";

        // info->play_length está en milisegundos ("-1 si se desconoce",
        // pero play_length siempre trae un valor razonable: length si lo
        // especifica el fichero, o intro+loop*2, o 150000 ms por defecto).
        if (info->play_length > 0) {
            meta_.length_frames =
                static_cast<uint64_t>(info->play_length) * kSampleRate / 1000;

            // gme_set_fade_msecs (>= 0.6.4) respeta la duración de fade
            // configurable (CoreOptions::default_fade_seconds); gme_set_fade
            // usaría la duración interna fija de cada emulador.
            gme_set_fade_msecs(emu_, info->play_length, fade_length_msecs_);
            meta_.fade_frames =
                static_cast<uint64_t>(fade_length_msecs_) * kSampleRate / 1000;

            // gme_set_fade_msecs(emu, start, length) arranca el fundido EN
            // play_length y lo estira 'length' más; gme_track_ended() no es
            // cierto hasta que ese fundido acaba (Music_Emu::play_ ->
            // track_ended_ tras fade_). La pista dura play_length + fade, y
            // eso es lo que debe mostrar la UI.
            meta_.total_frames = meta_.length_frames + meta_.fade_frames;
        }

        gme_free_info(info); // obligatorio: gme.h documenta que el llamante lo libera

        // libgme no expone el chip como dato: gme_type_system() devuelve la
        // MÁQUINA ("Nintendo NES"), no el chip. De ahí el mapeo manual,
        // indexado por la extensión canónica del tipo.
        meta_.chip = chip_for_type(gme_type(emu_));
    }

    // Extensión canónica -> chip realmente emulado por ese formato.
    static const char* chip_for_type(gme_type_t type) noexcept {
        const char* ext = type ? gme_type_extension(type) : nullptr;
        if (!ext) return "";
        // gme_type_extension() devuelve la extensión en mayúsculas.
        if (!std::strcmp(ext, "SPC"))  return "S-DSP";       // SNES: SPC700 + S-DSP
        if (!std::strcmp(ext, "NSF"))  return "RP2A03";      // NES APU
        if (!std::strcmp(ext, "NSFE")) return "RP2A03";
        if (!std::strcmp(ext, "GBS"))  return "DMG APU";     // Game Boy
        if (!std::strcmp(ext, "HES"))  return "HuC6280";     // PC Engine
        if (!std::strcmp(ext, "KSS"))  return "AY-3-8910";   // MSX (+SCC/FM segun el rip)
        if (!std::strcmp(ext, "SAP"))  return "POKEY";       // Atari 8-bit
        if (!std::strcmp(ext, "AY"))   return "AY-3-8910";   // ZX Spectrum / CPC
        if (!std::strcmp(ext, "GYM"))  return "YM2612";      // Mega Drive
        return "";
    }

    Music_Emu* emu_ = nullptr;
    int track_count_ = 0;
    unsigned current_track_ = 0;
    int fade_length_msecs_;
    TrackMetadata meta_;
};
