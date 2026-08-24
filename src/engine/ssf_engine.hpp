// ssf_engine.hpp — Sega Saturn (SSF/MiniSSF, eng_ssf.c + Musashi M68000 +
// SCSP). Saturn no tiene VGM real: usa su propio formato PSF-like, con el
// mismo contenedor corlett y distinto motor de reproducción.
//
// NO hace falta emular el SH-2 principal de la Saturn. El audio lo genera
// un MC68EC000 auxiliar que vive DENTRO del subsistema de sonido, separado
// de los dos SH-2: los juegos escriben un driver de sonido y los datos de
// canción en la RAM del SCSP, y ese 68000 los ejecuta moviendo los
// registros del SCSP. Un fichero SSF captura exactamente ese driver de
// 68000. De ahí que eng_ssf.c solo necesite m68k_execute() (Musashi) y
// SCSP_Update(), ambos ya vendorizados, y ninguna imagen de BIOS.
// Arquitectónicamente es el mismo patrón que PSF1 (CPU + DSP + contenedor
// corlett) con Musashi/SCSP en lugar de R3000A/SPU.
//
// Particularidades de eng_ssf.c frente a los motores PSF:
//   - ssf_sample() SÍ llama a corlett_sample_fade() ella misma. render()
//     no debe volver a llamarla (ver psf_engine.hpp para lo que ocurre si
//     se duplica).
//   - ssf_command(COMMAND_RESTART) es un NO-OP literal: `return
//     AO_SUCCESS;` sin tocar nada de estado. Por eso select_track() no se
//     sobrescribe aquí.
//   - ssf_fill_info() usa los mismos índices que psf_fill_info().
//
// sat_ram (sat_hw.c) y el contexto de Musashi (variables de módulo, sin
// parámetro de instancia) son estado global de PROCESO: solo puede haber
// un SsfEngine vivo a la vez, y de eso se encarga AosdkSaturnCoreGuard.

#pragma once

#include <string>
#include <vector>

#include "iaudio_engine.hpp"
#include "aosdk_bridge.hpp"

extern "C" {
#include "ao.h"
#include "corlett.h"
#include "eng_protos.h"
}

class SsfEngine final : public IAudioEngine {
public:
    SsfEngine() noexcept {
        has_core_guard_ = AosdkSaturnCoreGuard::acquire();
    }

    ~SsfEngine() override {
        if (!has_core_guard_) return;   // nunca tocó el estado global
        if (started_) {
            ssf_stop();
        }
        AosdkLibResolver::uninstall();
        AosdkSaturnCoreGuard::release();
    }

    // Mismo mecanismo que PsfEngine/Psf2Engine: debe llamarse ANTES de
    // open() si el fichero principal viene de un .zip, para resolver
    // "_lib" contra otras entradas ya extraídas del mismo archivo.
    void set_sibling_lookup(AosdkLibResolver::SiblingLookup lookup) {
        sibling_lookup_ = std::move(lookup);
    }

    bool open(const char* uri, const uint8_t* data, std::size_t size,
               IVFSBridge& vfs) override {
        // Otro motor de esta familia sigue vivo: el estado de aosdk es de
        // PROCESO (psx_ram/mipscpu/sat_ram/Musashi), así que abrir aquí lo
        // corrompería. Se falla la carga y el llamante prueba con la
        // entrada siguiente; antes esto era un assert que abortaba el
        // proceso entero.
        if (!has_core_guard_) return false;
        if (uri) {
            if (!vfs.read_whole_file(uri, main_file_buffer_)) return false;
            AosdkLibResolver::install(&vfs, vfs_dirname(uri), sibling_lookup_);
        } else if (data && size > 0) {
            main_file_buffer_.assign(data, data + size);
            AosdkLibResolver::install(&vfs, std::string(), sibling_lookup_);
        } else {
            return false;
        }

        // Mínimo de cabecera Corlett; ver psf_engine.hpp::open().
        if (main_file_buffer_.size() < 16) {
            AosdkLibResolver::uninstall();
            return false;
        }

        const int32_t ok = ssf_start(main_file_buffer_.data(),
                                      static_cast<uint32_t>(main_file_buffer_.size()));
        started_ = (ok == AO_SUCCESS);
        if (!started_) {
            AosdkLibResolver::uninstall();
            return false;
        }

        ao_song_done = 0; // ssf_start() no lo resetea por sí mismo (igual que psf_start)
        refresh_metadata();
        return true;
    }

    std::size_t render(int16_t* out, std::size_t max_frames) noexcept override {
        if (!started_) return 0;
        for (std::size_t i = 0; i < max_frames; ++i) {
            stereo_sample_t s{};
            if (ssf_sample(&s) != AO_SUCCESS) return i;

            // NO llamar aquí a corlett_sample_fade(): ssf_sample() ya lo
            // hace ella misma (ver cabecera). Duplicarlo cortaría cada
            // pista por la mitad.

            out[i * 2 + 0] = s.l;
            out[i * 2 + 1] = s.r;

            if (ao_song_done) return i + 1;
        }
        return max_frames;
    }

    void on_video_frame() noexcept override {
        if (started_) ssf_frame();
    }

    bool end_of_track() const noexcept override {
        return !started_ || ao_song_done != 0;
    }

    // select_track() NO se sobrescribe a propósito:
    // ssf_command(COMMAND_RESTART) es un no-op en este motor vendorizado,
    // así que el 'false' por defecto de IAudioEngine es más honesto que
    // fingir un reinicio.

    const TrackMetadata& metadata() const noexcept override { return meta_; }
    const char* engine_name() const noexcept override { return "aosdk-ssf"; }

private:
    void refresh_metadata() noexcept {
        meta_ = TrackMetadata{};

        ao_display_info info{};
        if (ssf_fill_info(&info) != AO_SUCCESS) return;

        if (info.info[1]) meta_.title = info.info[1];
        if (info.info[2]) meta_.game = info.info[2];
        if (info.info[3]) meta_.artist = info.info[3];
        if (info.info[4]) meta_.copyright_notice = info.info[4];
        if (info.info[5]) meta_.year = info.info[5];

        const double length_s = psfTimeToSeconds(info.info[6]);
        const double fade_s   = psfTimeToSeconds(info.info[7]);
        meta_.length_frames = static_cast<uint64_t>(length_s * 44100.0);
        meta_.fade_frames   = static_cast<uint64_t>(fade_s * 44100.0);

        // Duracion REAL hasta ao_song_done: corlett_length_set() fija
        // decaybegin = length y decayend = length + fade, y ao_song_done se
        // levanta al llegar a decayend. Sin tag de duracion (length_s == 0)
        // se deja en 0 = desconocida, porque entonces decaybegin queda
        // practicamente infinito y la pista no termina sola.
        meta_.total_frames  = (meta_.length_frames != 0)
                                ? meta_.length_frames + meta_.fade_frames
                                : 0;

        // Chip fijo: este formato no tiene variantes.
        meta_.chip = "SCSP";
    }

    // false = otro motor de esta familia ya tenía el guard. Se falla la
    // carga en open(); antes esto era un assert que abortaba RetroArch.
    bool has_core_guard_ = false;
    bool started_ = false;
    std::vector<uint8_t> main_file_buffer_;
    TrackMetadata meta_;
    AosdkLibResolver::SiblingLookup sibling_lookup_;
};
