// psf_engine.hpp — motor PSF1 (eng_psf.c / psx.c / peops).
//
// Notas de integración con el código vendorizado:
//
//   - stereo_sample_t { int16 l; int16 r; } es el tipo de ao.h y se usa
//     directamente, sin réplica local.
//   - El fade NO se aplica desde aquí: peops/spu.c llama a
//     corlett_sample_fade() por cada muestra estéreo que produce. Llamarlo
//     también en render() lo DUPLICA -- total_samples avanza dos veces por
//     frame y toda pista se corta a la mitad de su duración. Es un fallo
//     que las fixtures sintéticas no detectan (sin tag `length`, decayend
//     queda enorme y ao_song_done no dispara nunca); hace falta contenido
//     real. Clavado en tests/f5_fade_count_probe.cpp.
//   - corlett_length_set() se llama sola al decodificar el fichero
//     principal.
//   - aosdk trae su propia HLE del kernel de PS1 (psx_bios_hle() en
//     psx_hw.c): no hace falta una imagen de BIOS real.
//   - El estado de aosdk es global de PROCESO (la corlett_t estática de
//     eng_psf.c, psx_ram, los contadores de fade) y además compartido con
//     PSF2. De ahí AosdkPsxCoreGuard, que impide dos motores vivos a la
//     vez.

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

class PsfEngine final : public IAudioEngine {
public:
    PsfEngine() noexcept {
        has_core_guard_ = AosdkPsxCoreGuard::acquire();
    }

    ~PsfEngine() override {
        if (!has_core_guard_) return;   // nunca tocó el estado global
        if (started_) {
            psf_stop();
        }
        AosdkLibResolver::uninstall();
        AosdkPsxCoreGuard::release();
    }

    // Debe llamarse ANTES de open() si el fichero principal viene de un
    // .zip (uri==nullptr en open()): permite resolver "_lib" contra otras
    // entradas ya extraídas del mismo archivo en vez de contra un
    // directorio base que no existe. Ver AosdkLibResolver::SiblingLookup.
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
            // Fichero suelto en disco (need_fullpath=true).
            if (!vfs.read_whole_file(uri, main_file_buffer_)) return false;
            AosdkLibResolver::install(&vfs, vfs_dirname(uri), sibling_lookup_);
        } else if (data && size > 0) {
            // Entrada de un .zip (zip_playlist.hpp): ya está en memoria.
            // Si 'sibling_lookup_' se configuró (ver set_sibling_lookup),
            // "_lib" se resuelve contra otras entradas del mismo zip; si
            // no, la resolución fallará para cualquier fichero que use
            // "_lib" -- se registra y se omite esa entrada, no crashea el
            // resto de la lista (ver libretro.cpp::load_zip_entry_from).
            main_file_buffer_.assign(data, data + size);
            AosdkLibResolver::install(&vfs, std::string(), sibling_lookup_);
        } else {
            return false;
        }

        // Longitud mínima de una cabecera Corlett: 16 bytes ("PSF" +
        // versión(1) + reserved_size(4) + program_size(4) + crc32(4)).
        // Imprescindible: con 0 bytes exactos, corlett_decode_lib()
        // desreferencia input[0] sin comprobar la longitud (código
        // vendorizado, no se toca). read_whole_file() no puede filtrarlo
        // porque para él leer 0 bytes es un éxito trivial.
        if (main_file_buffer_.size() < 16) {
            AosdkLibResolver::uninstall();
            return false;
        }

        const int32_t ok = psf_start(main_file_buffer_.data(),
                                      static_cast<uint32_t>(main_file_buffer_.size()));
        started_ = (ok == AO_SUCCESS);
        if (!started_) {
            AosdkLibResolver::uninstall();
            return false;
        }

        ao_song_done = 0; // psf_start() no lo resetea por sí mismo
        refresh_metadata();
        return true;
    }

    std::size_t render(int16_t* out, std::size_t max_frames) noexcept override {
        if (!started_) return 0;
        for (std::size_t i = 0; i < max_frames; ++i) {
            stereo_sample_t s{};
            if (psf_sample(&s) != AO_SUCCESS) return i;

            // NO llamar aquí a corlett_sample_fade(): ya lo hace
            // peops/spu.c al producir cada muestra. Duplicarlo parte la
            // duración de toda pista por la mitad (ver cabecera).

            out[i * 2 + 0] = s.l;
            out[i * 2 + 1] = s.r;

            if (ao_song_done) return i + 1;
        }
        return max_frames;
    }

    // Debe llamarse una vez por iteración de retro_run(), fuera del
    // bucle de render() muestra a muestra (psf_frame() representa un
    // "tick" de vídeo/temporizador interno, no de audio).
    void on_video_frame() noexcept override {
        if (started_) psf_frame();
    }

    bool end_of_track() const noexcept override {
        return !started_ || ao_song_done != 0;
    }

    // PSF1 no tiene subsongs: track_count() es siempre 1 y select_track(0)
    // se reinterpreta como reinicio/loop de la pista actual.
    bool select_track(unsigned index) override {
        if (!started_ || index != 0) return false;

        if (psf_command(COMMAND_RESTART, 0) != AO_SUCCESS) return false;

        ao_song_done = 0;
        // psf_command(COMMAND_RESTART) reinicia CPU y SPU pero NO toca los
        // contadores de fade de corlett.c: total_samples se queda donde lo
        // dejó corlett_sample_fade(). Sin esta llamada, la pista
        // "reiniciada" saldría en silencio, con el fade ya agotado desde la
        // primera muestra. corlett_length_set() pone total_samples a 0 como
        // efecto secundario; se reutilizan los segundos parseados en
        // open().
        corlett_length_set(static_cast<double>(meta_.length_frames) / 44100.0,
                            static_cast<double>(meta_.fade_frames) / 44100.0);
        return true;
    }

    const TrackMetadata& metadata() const noexcept override { return meta_; }
    const char* engine_name() const noexcept override { return "aosdk-psf1"; }

private:
    void refresh_metadata() noexcept {
        meta_ = TrackMetadata{};

        ao_display_info info{};
        if (psf_fill_info(&info) != AO_SUCCESS) return;

        // Índices de eng_psf.c::psf_fill_info: 1=title, 2=game, 3=artist,
        // 4=copyright, 5=year, 6=length, 7=fade (el 0 lo reserva la propia
        // función). Los punteros apuntan al tag_buffer interno de la
        // corlett_t estática: se copian a std::string, nunca se retienen.
        if (info.info[1]) meta_.title = info.info[1];
        if (info.info[2]) meta_.game = info.info[2];
        if (info.info[3]) meta_.artist = info.info[3];
        if (info.info[4]) meta_.copyright_notice = info.info[4];
        if (info.info[5]) meta_.year = info.info[5];

        // psfTimeToSeconds() es una utilidad pura de corlett.c, no ligada a
        // la corlett_t estática.
        const double length_s = psfTimeToSeconds(info.info[6]);
        const double fade_s   = psfTimeToSeconds(info.info[7]);
        meta_.length_frames = static_cast<uint64_t>(length_s * 44100.0);
        meta_.fade_frames   = static_cast<uint64_t>(fade_s * 44100.0);

        // Duración REAL hasta ao_song_done: corlett_length_set() fija
        // decaybegin = length y decayend = length + fade, y ao_song_done se
        // levanta al llegar a decayend. Sin tag de duración (length_s == 0)
        // se deja en 0 = desconocida, porque entonces decaybegin queda
        // prácticamente infinito y la pista no termina sola.
        meta_.total_frames  = (meta_.length_frames != 0)
                                ? meta_.length_frames + meta_.fade_frames
                                : 0;

        // Chip fijo: este formato no tiene variantes.
        meta_.chip = "SPU";
    }

    // false = otro motor de esta familia ya tenía el guard. Se falla la
    // carga en open(); antes esto era un assert que abortaba RetroArch.
    bool has_core_guard_ = false;
    bool started_ = false;
    std::vector<uint8_t> main_file_buffer_;
    TrackMetadata meta_;
    AosdkLibResolver::SiblingLookup sibling_lookup_;
};
