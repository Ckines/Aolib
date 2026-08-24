// psf2_engine.hpp — motor PSF2 (eng_psf2.c / psx.c / peops2).
//
// Estructura idéntica a PsfEngine, pero eng_psf2.c NO es simétrico a
// eng_psf.c en todo. Diferencias que importan:
//
//   - El contenido real de un PSF2 no vive en el programa comprimido, que
//     debe estar VACÍO, sino en el área reservada de la cabecera Corlett,
//     que psf2_lib() reinterpreta como un sistema de ficheros IOP propio
//     (formato reconstruido leyendo load_file_ex()/psf2_load_file(); no
//     está documentado en la SDK, y tests/build_fixtures2.py lo replica).
//     psf2_start() busca ahí un fichero llamado literalmente "psf2.irx" y
//     lo carga como ELF32.
//   - TRAMPA de aosdk: si "psf2.irx" no aparece, psf2_load_file() devuelve
//     0xffffffff y psf2_start() no llega a llamar a psf2_load_elf() -- pero
//     'initialPC' es un `static uint32` sin inicializador, así que en la
//     PRIMERA carga del proceso vale 0, no 0xffffffff, y la comprobación
//     `if (initialPC == 0xffffffff) return AO_FAIL;` no dispara:
//     psf2_start() devuelve AO_SUCCESS con la CPU arrancando en PC=0 en vez
//     de fallar limpiamente. Por eso las fixtures llevan un "psf2.irx"
//     real, en vez de confiar en que una vacía falle de forma audible.
//   - El fade lo aplica peops2/spu.c, no eng_psf2.c ni este motor (ver
//     render()).
//   - AosdkPsxCoreGuard se comparte con PsfEngine: psx_ram y mipscpu son
//     globales de proceso, no por tipo.

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

// 'iUseReverb' (peops2/spu.c) es el único interruptor global de reverb que
// expone esta versión de aosdk, y solo existe en el SPU de PSF2. Se declara
// aquí a mano en vez de incluir peops2/externals.h para no arrastrar sus
// structs y typedefs internos a esta unidad de traducción.
extern "C" { extern int iUseReverb; }

class Psf2Engine final : public IAudioEngine {
public:
    Psf2Engine() noexcept {
        has_core_guard_ = AosdkPsxCoreGuard::acquire();
    }

    ~Psf2Engine() override {
        if (!has_core_guard_) return;   // nunca tocó el estado global
        if (started_) {
            psf2_stop();
        }
        AosdkLibResolver::uninstall();
        AosdkPsxCoreGuard::release();
    }

    // Ver PsfEngine::set_sibling_lookup. El tag se resuelve dentro de
    // corlett_decode(), que es común a PSF1 y PSF2.
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

        // psf2_start() decodifica (corlett_decode -> psf2_lib, que
        // construye los sistemas de ficheros IOP) Y arranca CPU y SPU2 en
        // la misma llamada: mips_init/reset, SPU2init/open y carga con
        // relocación del ELF psf2.irx.
        const int32_t ok = psf2_start(main_file_buffer_.data(),
                                       static_cast<uint32_t>(main_file_buffer_.size()));
        started_ = (ok == AO_SUCCESS);
        if (!started_) {
            AosdkLibResolver::uninstall();
            return false;
        }

        ao_song_done = 0; // psf2_start() tampoco lo resetea por sí mismo
        refresh_metadata();
        return true;
    }

    std::size_t render(int16_t* out, std::size_t max_frames) noexcept override {
        if (!started_) return 0;
        for (std::size_t i = 0; i < max_frames; ++i) {
            stereo_sample_t s{};
            if (psf2_sample(&s) != AO_SUCCESS) return i;

            // NO llamar aquí a corlett_sample_fade(): ya lo hace
            // peops2/spu.c al producir cada muestra. Duplicarlo hace que
            // total_samples avance dos veces por frame, con lo que
            // length+fade se agota al doble de velocidad y la pista se
            // corta por la mitad.

            out[i * 2 + 0] = s.l;
            out[i * 2 + 1] = s.r;

            if (ao_song_done) return i + 1;
        }
        return max_frames;
    }

    // psf2_frame() envuelve ps2_hw_frame(): el "tick" del hardware de PS2
    // emulado (SPU2 + IOP). Una vez por retro_run(), fuera del bucle de
    // render().
    void on_video_frame() noexcept override {
        if (started_) psf2_frame();
    }

    bool end_of_track() const noexcept override {
        return !started_ || ao_song_done != 0;
    }

    // PSF2 tampoco tiene subsongs: select_track(0) significa "reinicia la
    // pista actual".
    bool select_track(unsigned index) override {
        if (!started_ || index != 0) return false;

        if (psf2_command(COMMAND_RESTART, 0) != AO_SUCCESS) return false;

        ao_song_done = 0;
        // psf2_command(COMMAND_RESTART) reinicia CPU, SPU2 y RAM desde el
        // snapshot 'initial_ram', pero no toca los contadores de fade de
        // corlett.c. Ver psf_engine.hpp::select_track().
        corlett_length_set(static_cast<double>(meta_.length_frames) / 44100.0,
                            static_cast<double>(meta_.fade_frames) / 44100.0);
        return true;
    }

    const TrackMetadata& metadata() const noexcept override { return meta_; }
    const char* engine_name() const noexcept override { return "aosdk-psf2"; }

    // 'iUseReverb' es estado global de proceso, igual que psx_ram/mipscpu.
    // Escribirlo desde aquí es seguro porque AosdkPsxCoreGuard garantiza
    // una única instancia viva.
    void set_reverb_enabled(bool enabled) noexcept override {
        iUseReverb = enabled ? 1 : 0;
    }

private:
    void refresh_metadata() noexcept {
        meta_ = TrackMetadata{};

        ao_display_info info{};
        if (psf2_fill_info(&info) != AO_SUCCESS) return;

        // Índices de eng_psf2.c::psf2_fill_info: 1=title, 2=game,
        // 3=artist, 4=copyright, 5=year, 6=length, 7=fade. Mismo layout
        // que psf_fill_info.
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
        meta_.chip = "SPU2";
    }

    // false = otro motor de esta familia ya tenía el guard. Se falla la
    // carga en open(); antes esto era un assert que abortaba RetroArch.
    bool has_core_guard_ = false;
    bool started_ = false;
    std::vector<uint8_t> main_file_buffer_;
    TrackMetadata meta_;
    AosdkLibResolver::SiblingLookup sibling_lookup_;
};
