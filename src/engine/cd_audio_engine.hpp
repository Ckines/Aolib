// cd_audio_engine.hpp
//
// Reproduce una pista CD-DA de un CHD. Es el motor mas sencillo del core y
// eso no es casualidad: el Libro Rojo define el audio de un CD como PCM de
// 16 bits, estereo, a 44.100 Hz, que es exactamente el formato de salida
// del core. No hay codec ni emulacion; hay que leer y entregar.
//
// Dos cosas que si hay que hacer bien:
//
//  1. EL ORDEN DE BYTES. CHD guarda las pistas de CD-DA en big-endian
//     (convenio de MAME desde siempre) y el callback de audio de libretro
//     es little-endian. El intercambio lo hace ChdReader::read_track(),
//     que es la vista "como un .bin" de la pista; este motor no toca
//     bytes. Sin eso la pista suena a ruido blanco -- lo destapo
//     tests/c01_chd_reader.cpp comparando contra `chdman extractcd`.
//
//  2. NO ASIGNAR EN render(). El buffer de sectores se reserva en open(),
//     como exige IAudioEngine.
//
// No se materializa la pista: se leen los sectores que hagan falta para
// cada render(). Una pista de CD-DA de cuatro minutos son 42 MB y un disco
// entero pasa de 500 MB, asi que meterlas en RAM esta descartado -- y no
// hace falta, porque el mapa de hunks de CHD da acceso aleatorio O(1).

#pragma once

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "iaudio_engine.hpp"
#include "../chd_reader.hpp"

class CdAudioEngine final : public IAudioEngine {
public:
    // Cuantos sectores se leen de golpe. 16 sectores son 37.632 bytes,
    // 9.408 frames: mas que los ~735 que pide un frame de video a 60 fps,
    // asi que la mayoria de las llamadas a render() salen del buffer sin
    // tocar el CHD. Subirlo mas solo aumentaria el pico de trabajo de la
    // llamada que si toca leer.
    static constexpr std::size_t kSectorsPerRead = 16;

    // El motor NO es dueno del lector: el CHD lo posee CoreContext y vive
    // mas que cualquier motor. Se guarda el indice de la pista, no un
    // puntero a ella, porque el vector de pistas pertenece al lector.
    CdAudioEngine(chd_reader::ChdReader* chd, std::size_t track_index) noexcept
        : chd_(chd), track_index_(track_index) {}

    bool open(const char* uri, const uint8_t* /*data*/, std::size_t /*size*/,
              IVFSBridge& /*vfs*/) override {
        if (!chd_ || !chd_->is_open()) return false;
        if (track_index_ >= chd_->tracks().size()) return false;

        const chd_reader::ChdTrack& t = chd_->tracks()[track_index_];
        if (!t.is_audio || t.frames == 0) return false;
        if (t.data_size != chd_reader::kCdSectorSize) return false;

        buffer_.assign(kSectorsPerRead * chd_reader::kCdSectorSize, 0);
        buffer_frames_ = 0;
        buffer_pos_    = 0;
        sector_        = 0;
        done_          = false;

        // Una pista de CD-DA no trae ni titulo ni artista: el CD-Text es
        // opcional, casi ningun rip lo conserva y CHD no lo guarda. Se usa
        // el nombre que le puso la lista (chd_playlist.hpp), sin la
        // extension, por el mismo motivo que VgmstreamEngine: dejarlo vacio
        // hace que la UI muestre 'Reproduciendo: ""' y parece un fallo.
        meta_ = TrackMetadata{};
        if (uri && *uri) {
            meta_.title = uri;
            const std::size_t punto = meta_.title.rfind('.');
            if (punto != std::string::npos && punto > 0)
                meta_.title.resize(punto);
        }
        meta_.length_frames = frames_total();
        meta_.total_frames  = frames_total();
        meta_.chip = "CD-DA";
        return true;
    }

    std::size_t render(int16_t* out, std::size_t max_frames) noexcept override {
        if (!chd_ || done_) return 0;

        std::size_t escritos = 0;
        while (escritos < max_frames) {
            if (buffer_pos_ >= buffer_frames_ && !rellenar()) break;

            const std::size_t hay  = buffer_frames_ - buffer_pos_;
            const std::size_t toma = std::min(hay, max_frames - escritos);
            std::memcpy(out + escritos * 2,
                        buffer_.data() + buffer_pos_ * kBytesPerFrame,
                        toma * kBytesPerFrame);
            buffer_pos_ += toma;
            escritos    += toma;
        }
        if (escritos == 0) done_ = true;
        return escritos;
    }

    bool end_of_track() const noexcept override { return done_; }

    // Una pista de CD-DA es una sola pista y SI se puede reiniciar: basta
    // con volver el cabezal al sector 0. Devolver true aqui (al contrario
    // que el default de la interfaz) es correcto y es lo que permite que
    // el modo repetir funcione sin reabrir el motor.
    bool select_track(unsigned index) override {
        if (index != 0) return false;
        sector_        = 0;
        buffer_frames_ = 0;
        buffer_pos_    = 0;
        done_          = false;
        return true;
    }

    const TrackMetadata& metadata() const noexcept override { return meta_; }
    const char* engine_name() const noexcept override { return "cd-audio"; }

private:
    static constexpr std::size_t kBytesPerFrame = 4;   // 16 bits x 2 canales

    uint64_t frames_total() const {
        const chd_reader::ChdTrack& t = chd_->tracks()[track_index_];
        return uint64_t(t.frames) * (chd_reader::kCdSectorSize / kBytesPerFrame);
    }

    bool rellenar() noexcept {
        const chd_reader::ChdTrack& t = chd_->tracks()[track_index_];
        if (sector_ >= t.frames) return false;

        const uint32_t sectores =
            std::min<uint32_t>(uint32_t(kSectorsPerRead), t.frames - sector_);
        if (!chd_->read_track(t, sector_, sectores, buffer_.data())) {
            done_ = true;              // un hunk ilegible acaba la pista
            return false;
        }
        sector_       += sectores;
        buffer_frames_ = std::size_t(sectores) *
                         (chd_reader::kCdSectorSize / kBytesPerFrame);
        buffer_pos_    = 0;
        return true;
    }

    chd_reader::ChdReader* chd_ = nullptr;
    std::size_t            track_index_ = 0;

    std::vector<uint8_t>   buffer_;
    std::size_t            buffer_frames_ = 0;
    std::size_t            buffer_pos_    = 0;
    uint32_t               sector_        = 0;
    bool                   done_          = false;

    TrackMetadata          meta_;
};
