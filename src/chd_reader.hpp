// chd_reader.hpp
//
// Lector de CHD (Compressed Hunks of Data) sobre IVFSBridge. Envuelve
// deps/libchdr y le da al resto del core una vista de FLUJO LOGICO: pide
// bytes en un desplazamiento y ya, sin que nadie fuera de aqui sepa que
// hay hunks debajo.
//
// Por que CHD: su mapa de hunks va al principio del fichero y cada hunk
// se comprime por separado, asi que llegar al byte N es O(1). No hay nada
// que decodificar por adelantado ni presupuesto de memoria que administrar:
// se lee el hunk que toque.
//
// Dos formas de CHD llegan al core y las dos se leen igual por debajo:
//
//  - CHD de CD (`chdman createcd`): lleva metadatos de pistas (CHT2/CHTR/
//    CHGD) y su unidad son marcos de 2448 bytes (2352 de sector + 96 de
//    subcanal). Es lo que es cualquier rip de PS1/Saturn/Mega CD.
//  - CHD crudo (`chdman createraw`): sin tabla de pistas; el flujo logico
//    ES la imagen. Es lo que produce herramientas_locales/chdman/
//    hacer_album_chd.py para guardar un album, porque con unidades de 2048
//    el PCM queda contiguo y el codec FLAC gana (medido: 43.072.143 bytes
//    contra 62.053.651 del mismo album por createcd).
//
// NO se usa chd_precache(): carga el fichero entero en RAM y hay albumes
// de 1,4 GB.

#pragma once

#include "vfs_bridge.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "libchdr/chd.h"
}

namespace chd_reader {

// Marco de CD tal y como lo guarda CHD: sector + subcanal.
constexpr uint32_t kCdFrameSize    = 2448;
constexpr uint32_t kCdSectorSize   = 2352;
constexpr uint32_t kCdSubcodeSize  = 96;
constexpr uint32_t kCdCookedSize   = 2048;   // datos de usuario en MODE1/MODE2_FORM1

// chdman rellena cada pista hasta un multiplo de 4 marcos dentro del
// flujo. Sin esto, la pista 2 en adelante se lee desplazada.
constexpr uint32_t kCdTrackPadding = 4;

// Una entrada de la tabla de pistas. Los nombres de campo siguen a los de
// los metadatos CHT2 para que cotejar contra `chdman info -v` sea directo.
struct ChdTrack {
    int         number      = 0;
    std::string type;                 // MODE1, MODE1_RAW, MODE2_FORM1, AUDIO...
    std::string subtype;              // NONE, RW, RW_RAW
    uint32_t    frames      = 0;      // marcos de datos de la pista
    uint32_t    pregap      = 0;
    uint32_t    postgap     = 0;
    uint32_t    padding     = 0;      // relleno hasta multiplo de 4

    // Donde empieza la pista dentro del flujo logico del CHD, en marcos.
    uint64_t    chd_frame   = 0;

    // Vista "cocida" del sector: cuantos bytes utiles hay y en que
    // desplazamiento dentro de los 2352. En MODE1_RAW los datos van tras
    // 12 bytes de sincronia y 4 de cabecera; en MODE1 (2048) no hay marco.
    uint32_t    data_offset = 0;
    uint32_t    data_size   = kCdCookedSize;

    // Vista de SISTEMA DE FICHEROS: donde estan los 2048 bytes de datos de
    // usuario de un sector Form 1. NO es lo mismo que data_offset/data_size,
    // que describen lo que escribiria `chdman extractcd` en un .bin.
    // En MODE2_RAW las dos difieren: el .bin lleva los 2352 crudos (que es
    // lo que quiere vgmstream para enumerar los subsongs de XA), mientras
    // que ISO9660 vive en los 2048 que hay tras 12 de sincronia, 4 de
    // cabecera y 8 de subcabecera.
    uint32_t    fs_offset   = 0;
    uint32_t    fs_size     = 0;      // 0 = la pista no lleva sistema de ficheros

    bool        is_audio    = false;

    uint64_t cooked_bytes() const { return uint64_t(frames) * data_size; }
};

// Traduccion de los codigos de error de libchdr a algo que se pueda poner
// en el log del core. chd_error_string() existe, pero devuelve textos en
// ingles pensados para chdman; estos dicen ademas que hacer.
inline std::string explain(chd_error e) {
    switch (e) {
    case CHDERR_NONE:                return "sin error";
    case CHDERR_INVALID_FILE:        return "no es un CHD valido";
    case CHDERR_UNSUPPORTED_VERSION: return "version de CHD no soportada "
                                            "(este core lee v5)";
    case CHDERR_REQUIRES_PARENT:     return "es un CHD delta: necesita su "
                                            "CHD padre, que no se puede "
                                            "resolver desde aqui";
    case CHDERR_UNSUPPORTED_FORMAT:  return "usa un codec no soportado "
                                            "(probablemente zstd; recrealo "
                                            "con chdman -c cdlz,cdzl,cdfl)";
    case CHDERR_OUT_OF_MEMORY:       return "sin memoria";
    case CHDERR_READ_ERROR:          return "error de lectura";
    case CHDERR_DECOMPRESSION_ERROR: return "error al descomprimir un hunk";
    case CHDERR_CODEC_ERROR:         return "el codec fallo al inicializarse";
    case CHDERR_HUNK_OUT_OF_RANGE:   return "hunk fuera de rango";
    default:                         return chd_error_string(e);
    }
}

// ---------------------------------------------------------------------------
// Puente de E/S
// ---------------------------------------------------------------------------
//
// libchdr trae su propia abstraccion de fichero (core_file_callbacks), asi
// que no hace falta parchear nada para cumplir la regla de que toda la E/S
// pasa por IVFSBridge. Ojo: firma estilo stdio -- fread(ptr, size, nmemb).
struct VfsFile {
    IVFSBridge* vfs    = nullptr;
    void*       handle = nullptr;
    int64_t     size   = 0;
};

inline uint64_t vfs_fsize(void* argp) {
    VfsFile* f = static_cast<VfsFile*>(argp);
    if (!f || !f->handle) return uint64_t(-1);
    return uint64_t(f->size);
}

inline size_t vfs_fread(void* ptr, size_t size, size_t nmemb, void* argp) {
    VfsFile* f = static_cast<VfsFile*>(argp);
    if (!f || !f->handle || size == 0) return 0;
    const int64_t want = int64_t(size) * int64_t(nmemb);
    const int64_t got  = f->vfs->stream_read(f->handle, ptr, uint64_t(want));
    if (got <= 0) return 0;
    return size_t(got) / size;   // stdio devuelve elementos completos
}

inline int vfs_fclose(void* argp) {
    VfsFile* f = static_cast<VfsFile*>(argp);
    if (f && f->handle) { f->vfs->stream_close(f->handle); f->handle = nullptr; }
    return 0;
}

inline int vfs_fseek(void* argp, int64_t offset, int whence) {
    VfsFile* f = static_cast<VfsFile*>(argp);
    if (!f || !f->handle) return -1;
    // retro_vfs_seek confirma exito o fallo pero NO devuelve la posicion
    // (ver la advertencia de vfs_bridge.hpp); aqui no se necesita, pero
    // por eso no se puede usar su retorno como offset.
    int origin = RETRO_VFS_SEEK_POSITION_START;
    if (whence == SEEK_CUR) origin = RETRO_VFS_SEEK_POSITION_CURRENT;
    else if (whence == SEEK_END) origin = RETRO_VFS_SEEK_POSITION_END;
    return f->vfs->stream_seek(f->handle, offset, origin) < 0 ? -1 : 0;
}

// ---------------------------------------------------------------------------
// El lector
// ---------------------------------------------------------------------------

class ChdReader {
public:
    ChdReader() = default;
    ~ChdReader() { close(); }

    ChdReader(const ChdReader&)            = delete;
    ChdReader& operator=(const ChdReader&) = delete;

    bool open(const std::string& path, IVFSBridge& vfs, std::string& why) {
        close();

        file_.vfs    = &vfs;
        file_.handle = vfs.stream_open(path.c_str());
        if (!file_.handle) { why = "no se pudo abrir el fichero"; return false; }
        file_.size = vfs.stream_size(file_.handle);
        if (file_.size <= 0) {
            why = "fichero vacio o de tamano desconocido";
            close();
            return false;
        }

        callbacks_.fsize  = vfs_fsize;
        callbacks_.fread  = vfs_fread;
        callbacks_.fclose = vfs_fclose;
        callbacks_.fseek  = vfs_fseek;

        const chd_error e = chd_open_core_file_callbacks(
            &callbacks_, &file_, CHD_OPEN_READ, nullptr, &chd_);
        if (e != CHDERR_NONE || !chd_) {
            why  = explain(e);
            chd_ = nullptr;
            close();
            return false;
        }
        // A partir de aqui libchdr es el dueno del stream: chd_close() llama
        // a fclose, o sea a vfs_fclose. close() no debe cerrarlo dos veces.
        owns_stream_ = false;

        header_ = *chd_get_header(chd_);
        if (header_.hunkbytes == 0 || header_.totalhunks == 0) {
            why = "cabecera incoherente (hunkbytes o totalhunks a cero)";
            close();
            return false;
        }

        for (auto& s : slot_) {
            s.data.assign(header_.hunkbytes, 0);
            s.hunk = kNoHunk;
        }
        parse_tracks();
        return true;
    }

    void close() {
        if (chd_) { chd_close(chd_); chd_ = nullptr; }
        else if (owns_stream_ && file_.handle) vfs_fclose(&file_);
        file_    = VfsFile{};
        tracks_.clear();
        for (auto& s : slot_) { s.hunk = kNoHunk; s.data.clear(); }
        std::memset(&header_, 0, sizeof(header_));
        next_slot_ = 0;
        hunk_reads_ = 0;
        cache_hits_ = 0;
        owns_stream_ = true;
    }

    bool is_open() const { return chd_ != nullptr; }

    const chd_header& header() const { return header_; }
    uint64_t logical_size() const { return header_.logicalbytes; }
    uint32_t hunk_bytes()   const { return header_.hunkbytes; }
    uint32_t hunk_count()   const { return header_.totalhunks; }

    // Un CHD de createcd trae tabla de pistas; uno de createraw, no.
    bool is_cd() const { return !tracks_.empty(); }
    const std::vector<ChdTrack>& tracks() const { return tracks_; }

    // Contadores, para los tests y el log: cuantos hunks se han
    // descomprimido de verdad y cuantas lecturas resolvio la cache.
    uint64_t hunk_reads() const { return hunk_reads_; }
    uint64_t cache_hits() const { return cache_hits_; }

    // Lectura sobre el flujo logico. Devuelve false si se sale del final o
    // si un hunk no se pudo descomprimir; en ese caso 'dst' no es de fiar.
    bool read(uint64_t offset, void* dst, std::size_t len) {
        if (!chd_) return false;
        if (len == 0) return true;
        if (offset > header_.logicalbytes ||
            len > header_.logicalbytes - offset) return false;

        uint8_t* out = static_cast<uint8_t*>(dst);
        while (len > 0) {
            const uint32_t hunk = uint32_t(offset / header_.hunkbytes);
            const uint32_t skip = uint32_t(offset % header_.hunkbytes);
            const std::size_t take =
                std::min<std::size_t>(len, header_.hunkbytes - skip);

            const uint8_t* h = hunk_data(hunk);
            if (!h) return false;
            std::memcpy(out, h + skip, take);

            out    += take;
            offset += take;
            len    -= take;
        }
        return true;
    }


    // Vista de PISTA: los bytes utiles de 'frames' marcos consecutivos, ya
    // sin el marco de CD y con el orden de bytes corregido. Es lo que
    // contendria un .bin extraido con `chdman extractcd`, y lo que espera
    // cualquier motor de audio.
    //
    // OJO, esto no es cosmetico: CHD guarda las pistas de CD-DA en
    // BIG-ENDIAN (asi lo hace MAME desde siempre; el propio codec cdfl
    // lleva un campo swap_endian), mientras que un .bin/.cue y el callback
    // de audio de libretro son little-endian. Sin el intercambio, la pista
    // suena a ruido blanco -- comprobado contra `chdman extractcd` en
    // tests/c01_chd_reader.cpp, que fue quien lo destapo.
    //
    // read() NO hace esto a proposito: entrega el flujo logico en crudo,
    // que es lo unico contra lo que cuadra el SHA1 de la cabecera.
    bool read_track(const ChdTrack& t, uint32_t first_frame, uint32_t frames,
                    void* dst) {
        if (!chd_) return false;
        if (uint64_t(first_frame) + frames > t.frames) return false;

        uint8_t* out = static_cast<uint8_t*>(dst);
        for (uint32_t i = 0; i < frames; ++i) {
            const uint64_t off = (t.chd_frame + first_frame + i) * kCdFrameSize
                               + t.data_offset;
            if (!read(off, out, t.data_size)) return false;
            if (t.is_audio) swap16(out, t.data_size);
            out += t.data_size;
        }
        return true;
    }

    static void swap16(uint8_t* p, std::size_t n) {
        for (std::size_t i = 0; i + 1 < n; i += 2) {
            const uint8_t tmp = p[i];
            p[i] = p[i + 1];
            p[i + 1] = tmp;
        }
    }


    // Vista de SISTEMA DE FICHEROS: el flujo plano de 2048 bytes por sector
    // que ve un lector de ISO9660, sin marcos de CD ni ECC. 'offset' va en
    // ese espacio plano, o sea que el sector logico N empieza en N*2048 sea
    // cual sea el tipo de pista.
    //
    // Es una vista DISTINTA de read_track(): en MODE2_RAW aquella entrega
    // los 2352 crudos (que es lo que quiere vgmstream para los subsongs de
    // XA) y esta los 2048 utiles de un sector Form 1.
    bool read_fs(const ChdTrack& t, uint64_t offset, void* dst,
                 std::size_t len) {
        if (!chd_ || t.fs_size == 0) return false;
        uint8_t* out = static_cast<uint8_t*>(dst);
        while (len > 0) {
            const uint64_t sector = offset / t.fs_size;
            const uint32_t skip   = uint32_t(offset % t.fs_size);
            if (sector >= t.frames) return false;
            const std::size_t take =
                std::min<std::size_t>(len, t.fs_size - skip);
            const uint64_t off = (t.chd_frame + sector) * kCdFrameSize
                               + t.fs_offset + skip;
            if (!read(off, out, take)) return false;
            out += take; offset += take; len -= take;
        }
        return true;
    }

    // La pista que lleva el sistema de ficheros, o nullptr. En un CD de
    // juego es la 1; en un CHD crudo no hay tabla de pistas y el flujo
    // logico ya ES la imagen, asi que esto devuelve nullptr y el llamante
    // usa read() directamente.
    const ChdTrack* filesystem_track() const {
        for (const ChdTrack& t : tracks_) if (t.fs_size) return &t;
        return nullptr;
    }

    // Descomprime un hunk entero a un buffer del llamante, SIN pasar por la
    // cache. Es lo que usa el test de integridad, que recorre los hunks una
    // vez y no quiere desalojar nada ni medir aciertos falsos.
    bool read_hunk_uncached(uint32_t hunk, void* dst) {
        if (!chd_ || hunk >= header_.totalhunks) return false;
        return chd_read(chd_, hunk, dst) == CHDERR_NONE;
    }

    // Metadatos en crudo. tag es CDROM_TRACK_METADATA2_TAG y companyia.
    bool metadata(uint32_t tag, uint32_t index, std::string& out) const {
        if (!chd_) return false;
        char buf[512];
        uint32_t len = 0, rtag = 0;
        uint8_t flags = 0;
        const chd_error e = chd_get_metadata(chd_, tag, index, buf,
                                             uint32_t(sizeof(buf)) - 1,
                                             &len, &rtag, &flags);
        if (e != CHDERR_NONE) return false;
        if (len >= sizeof(buf)) len = uint32_t(sizeof(buf)) - 1;
        buf[len] = '\0';
        out.assign(buf);
        return true;
    }

private:
    static constexpr uint32_t kNoHunk = 0xFFFFFFFFu;

    // DOS ranuras, por el mismo motivo que los dos bloques de
    // zip_vfs_adapter.hpp: quien lee un sistema de ficheros alterna entre la
    // zona del directorio y la de los datos, y con una sola ranura cada
    // salto desaloja la otra y obliga a descomprimir el mismo hunk una y
    // otra vez. Con dos, el directorio se queda residente.
    struct Slot {
        std::vector<uint8_t> data;
        uint32_t             hunk = kNoHunk;
    };

    const uint8_t* hunk_data(uint32_t hunk) {
        if (hunk >= header_.totalhunks) return nullptr;
        for (auto& s : slot_) {
            if (s.hunk == hunk) { ++cache_hits_; return s.data.data(); }
        }
        Slot& s = slot_[next_slot_];
        next_slot_ = (next_slot_ + 1) % kSlots;
        s.hunk = kNoHunk;                    // no dejar datos viejos si falla
        if (chd_read(chd_, hunk, s.data.data()) != CHDERR_NONE) return nullptr;
        s.hunk = hunk;
        ++hunk_reads_;
        return s.data.data();
    }

    void parse_tracks();

    static constexpr int kSlots = 2;

    chd_file*            chd_ = nullptr;
    chd_header           header_{};
    VfsFile              file_;
    core_file_callbacks  callbacks_{};
    bool                 owns_stream_ = true;

    Slot                 slot_[kSlots];
    int                  next_slot_ = 0;
    uint64_t             hunk_reads_ = 0;
    uint64_t             cache_hits_ = 0;

    std::vector<ChdTrack> tracks_;
};

// ---------------------------------------------------------------------------
// Tabla de pistas
// ---------------------------------------------------------------------------

// Que hay dentro de un sector segun el tipo de pista. En MODE1 y
// MODE2_FORM1 chdman ya guarda los 2048 utiles; en las variantes _RAW hay
// que saltarse la sincronia (12) y la cabecera (4).
inline void cooked_layout(const std::string& type,
                          uint32_t& offset, uint32_t& size, bool& is_audio,
                          uint32_t& fs_offset, uint32_t& fs_size) {
    is_audio  = (type == "AUDIO");
    fs_offset = 0;
    fs_size   = 0;                       // por defecto, sin sistema de ficheros

    if (is_audio)              { offset = 0;  size = kCdSectorSize; return; }

    // Los datos de usuario de un sector Form 1 estan tras la sincronia (12),
    // la cabecera (4) y, solo en Modo 2, la subcabecera (8).
    if (type == "MODE1")       { offset = 0;  size = 2048;
                                 fs_offset = 0;  fs_size = 2048; return; }
    if (type == "MODE1_RAW")   { offset = 16; size = 2048;
                                 fs_offset = 16; fs_size = 2048; return; }
    if (type == "MODE2_FORM1") { offset = 0;  size = 2048;
                                 fs_offset = 0;  fs_size = 2048; return; }
    if (type == "MODE2_FORM2") { offset = 0;  size = 2324; return; }
    if (type == "MODE2")       { offset = 0;  size = 2336;
                                 fs_offset = 8;  fs_size = 2048; return; }
    if (type == "MODE2_RAW" ||
        type == "MODE2_FORM_MIX") {
        offset = 0; size = kCdSectorSize;
        fs_offset = 24; fs_size = 2048;  return;
    }
    // Desconocido: entregar el sector entero es lo menos destructivo, y
    // vgmstream sabe reconocer sectores crudos de 2352 por su sincronia.
    offset = 0; size = kCdSectorSize;
}

inline bool parse_track_line(const char* s, ChdTrack& t) {
    char type[32] = {0}, subtype[32] = {0};
    char pgtype[32] = {0}, pgsub[32] = {0};
    int  number = 0, frames = 0, pregap = 0, postgap = 0, pad = 0;

    if (std::sscanf(s, CDROM_TRACK_METADATA2_FORMAT, &number, type, subtype,
                    &frames, &pregap, pgtype, pgsub, &postgap) == 8) {
        // formato CHT2
    } else if (std::sscanf(s, GDROM_TRACK_METADATA_FORMAT, &number, type,
                           subtype, &frames, &pad, &pregap, pgtype, pgsub,
                           &postgap) == 9) {
        // formato CHGD (GD-ROM de Dreamcast)
    } else if (std::sscanf(s, CDROM_TRACK_METADATA_FORMAT, &number, type,
                           subtype, &frames) == 4) {
        // formato CHTR, el viejo: sin pregap ni postgap
        pregap = postgap = 0;
    } else {
        return false;
    }

    t.number  = number;
    t.type    = type;
    t.subtype = subtype;
    t.frames  = uint32_t(frames < 0 ? 0 : frames);
    t.pregap  = uint32_t(pregap  < 0 ? 0 : pregap);
    t.postgap = uint32_t(postgap < 0 ? 0 : postgap);
    cooked_layout(t.type, t.data_offset, t.data_size, t.is_audio,
                  t.fs_offset, t.fs_size);
    return true;
}

inline void ChdReader::parse_tracks() {
    tracks_.clear();
    if (!chd_) return;

    static const uint32_t kTags[] = {
        CDROM_TRACK_METADATA2_TAG,   // CHT2, el que escribe chdman moderno
        CDROM_TRACK_METADATA_TAG,    // CHTR
        GDROM_TRACK_METADATA_TAG,    // CHGD
    };

    for (uint32_t index = 0; ; ++index) {
        ChdTrack t;
        bool encontrada = false;
        for (uint32_t tag : kTags) {
            std::string linea;
            if (metadata(tag, index, linea) && parse_track_line(linea.c_str(), t)) {
                encontrada = true;
                break;
            }
        }
        if (!encontrada) break;
        tracks_.push_back(t);
        if (tracks_.size() > 99) break;   // un CD tiene 99 pistas como mucho
    }

    // Desplazamiento de cada pista DENTRO del flujo del CHD. chdman rellena
    // cada pista hasta un multiplo de 4 marcos; sin tenerlo en cuenta, todo
    // lo que no sea la pista 1 se lee desplazado.
    uint64_t frame = 0;
    for (ChdTrack& t : tracks_) {
        t.chd_frame = frame;
        const uint32_t total = t.frames;
        const uint32_t redondeado =
            ((total + kCdTrackPadding - 1) / kCdTrackPadding) * kCdTrackPadding;
        t.padding = redondeado - total;
        frame += redondeado;
    }
}

}  // namespace chd_reader
