// chd_vfs_bridge.hpp
//
// Presenta la pista de DATOS de un .chd como un fichero virtual de
// sectores crudos, para que vgmstream (meta/xa.c) la lea directamente sin
// que nadie tenga que extraer nada a disco ni a RAM.
//
// Por que esto funciona sin escribir un parser nuevo: xa.c ya acepta
// sectores de CD crudos de 2352 bytes (detecta la palabra de sincronia
// 00 FF..FF 00 en el offset 0) y ya enumera los subsongs el solo,
// recorriendo los marcadores file+channel de la subcabecera de Modo 2.
// Solo hace falta entregarle el flujo de sectores; el resto es su trabajo.
//
// La vista que se expone es la RAW de ChdReader::read_track() (offset 0,
// 2352 bytes, sin intercambio de bytes -- eso solo aplica a CD-DA), NO la
// vista "cocida" de read_fs() que usa el lector de ISO9660. Son dos
// ventanas distintas sobre la MISMA pista: iso9660::Reader necesita los
// 2048 utiles de cada sector Form 1, xa.c necesita el sector entero
// (sincronia + cabecera + subcabecera + datos + ECC) porque es ahi donde
// vive la informacion de canal/fichero que usa para separar los streams
// XA entrelazados.
//
// Streaming de verdad, no materializacion: una pista de datos de PS1 puede
// pasar de 400 MB. stream_read() traduce cada peticion a un rango de
// sectores y llama a ChdReader::read_track(), que a su vez usa la cache de
// dos hunks del propio CHD -- ninguna capa de este camino carga la pista
// entera en memoria.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "chd_reader.hpp"
#include "vfs_bridge.hpp"

namespace chd_vfs_bridge {

// Cursor de una apertura. Solo existe uno por streamfile vivo en la
// practica (vgmstream cierra y reabre al cambiar de subsong, ver la nota 2
// de engine/vgmstream_engine.hpp), pero se soporta mas de uno a la vez sin
// coste extra: cada handle lleva su propia posicion.
struct Cursor {
    int64_t position = 0;
};

// Un solo fichero virtual por instancia: la pista de datos que se le pasó
// al construirla. 'path' en stream_open() no se comprueba -- no hay nada
// con lo que confundirlo, porque esta clase solo sirve para ESTA pista.
class ChdDataTrackVfs final : public IVFSBridge {
public:
    // La pista de datos ENTERA.
    ChdDataTrackVfs(chd_reader::ChdReader& chd, std::size_t track_index) noexcept
        : chd_(&chd), track_index_(track_index) {}

    // Una VENTANA de sectores dentro de la pista: los que ocupa un fichero
    // concreto del sistema de ficheros. Es lo que convierte "la pista de
    // datos entera, con todos los flujos XA del disco mezclados" en "este
    // fichero de audio", que es lo que el usuario ve como una pista.
    //
    // 'first' es el LBA del fichero y 'count' su tamano en SECTORES, que no
    // se saca dividiendo por 2352 sino por 2048: el registro de ISO9660
    // cuenta el tamano en bloques logicos de 2048 aunque los sectores sean
    // Form 2 de 2324 utiles. El llamante ya hace esa cuenta.
    ChdDataTrackVfs(chd_reader::ChdReader& chd, std::size_t track_index,
                    uint32_t first, uint32_t count) noexcept
        : chd_(&chd), track_index_(track_index),
          first_sector_(first), sector_count_(count) {}

    bool is_valid() const noexcept override {
        return chd_ && chd_->is_open() && track_index_ < chd_->tracks().size();
    }

    void* stream_open(const char* /*path*/) noexcept override {
        if (!is_valid()) return nullptr;
        return new (std::nothrow) Cursor();
    }

    int64_t stream_read(void* handle, void* buf, uint64_t len) noexcept override {
        auto* c = static_cast<Cursor*>(handle);
        if (!c || !buf || len == 0 || !is_valid()) return c ? 0 : -1;

        const int64_t total = total_bytes();
        if (c->position < 0 || c->position >= total) return 0;
        const uint64_t avail = uint64_t(total - c->position);
        const uint64_t want  = std::min<uint64_t>(len, avail);

        const auto& t = chd_->tracks()[track_index_];
        const uint32_t first_sector =
            uint32_t(c->position / chd_reader::kCdSectorSize);
        const uint32_t last_sector =
            uint32_t((c->position + int64_t(want) - 1) / chd_reader::kCdSectorSize);
        const uint32_t n_sectors = last_sector - first_sector + 1;
        const uint32_t skip = uint32_t(c->position % chd_reader::kCdSectorSize);

        // Buffer de trabajo reutilizado entre llamadas: vgmstream, envuelto
        // en libstreamfile_open_buffered() por quien construye este
        // adaptador (ver make() más abajo), pide en bloques razonables, no
        // byte a byte, así que esto no es el caso caliente de render().
        scratch_.resize(std::size_t(n_sectors) * chd_reader::kCdSectorSize);
        if (!chd_->read_track(t, first_sector_ + first_sector, n_sectors,
                              scratch_.data()))
            return -1;

        std::memcpy(buf, scratch_.data() + skip, std::size_t(want));
        c->position += int64_t(want);
        return int64_t(want);
    }

    int64_t stream_seek(void* handle, int64_t offset, int whence) noexcept override {
        auto* c = static_cast<Cursor*>(handle);
        if (!c) return -1;
        const int64_t total = total_bytes();
        int64_t base = 0;
        if (whence == RETRO_VFS_SEEK_POSITION_CURRENT) base = c->position;
        else if (whence == RETRO_VFS_SEEK_POSITION_END) base = total;
        const int64_t np = base + offset;
        if (np < 0 || np > total) return -1;
        c->position = np;
        return 0;
    }

    int64_t stream_tell(void* handle) noexcept override {
        auto* c = static_cast<Cursor*>(handle);
        return c ? c->position : -1;
    }

    int64_t stream_size(void* /*handle*/) noexcept override { return total_bytes(); }

    int stream_close(void* handle) noexcept override {
        delete static_cast<Cursor*>(handle);
        return 0;
    }

    // No lo usa nadie en este camino: xa.c no tiene dependencias externas
    // que resolver (a diferencia de un .psflib), y vgmstream siempre pide
    // el fichero por stream_open/read, nunca de una tacada.
    bool read_whole_file(const std::string&, std::vector<uint8_t>&) noexcept override {
        return false;
    }

    std::string resolve_relative(const std::string& base_dir,
                                 const std::string& rel) const override {
        return base_dir.empty() ? rel : base_dir + "/" + rel;
    }

private:
    int64_t total_bytes() const noexcept {
        if (!chd_ || track_index_ >= chd_->tracks().size()) return 0;
        const uint32_t frames = chd_->tracks()[track_index_].frames;
        // Sin ventana, la pista entera. Con ventana, lo que quepa dentro de
        // la pista: un extent declarado en el directorio puede mentir, y
        // pasarse del final produciria lecturas fallidas a mitad de
        // reproduccion en vez de una pista mas corta.
        if (sector_count_ == 0) return int64_t(frames) * chd_reader::kCdSectorSize;
        if (first_sector_ >= frames) return 0;
        const uint32_t cabe = std::min(sector_count_, frames - first_sector_);
        return int64_t(cabe) * int64_t(chd_reader::kCdSectorSize);
    }

    chd_reader::ChdReader* chd_ = nullptr;
    std::size_t            track_index_ = 0;
    uint32_t               first_sector_ = 0;
    uint32_t               sector_count_ = 0;   // 0 = la pista entera
    std::vector<uint8_t>   scratch_;
};

}  // namespace chd_vfs_bridge
