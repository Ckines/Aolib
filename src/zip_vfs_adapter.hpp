// zip_vfs_adapter.hpp
//
// minizip (deps/minizip/unzip.c) admite un juego de callbacks de E/S propio
// vía unzOpen2_64() + zlib_filefunc64_def. Usarlo, en vez de unzOpen() a
// secas, es obligatorio aquí: unzOpen() usa los callbacks por defecto de
// ioapi.c, que en este proyecto están deliberadamente anulados (ver
// deps/minizip/VENDOR.md) porque toda la E/S del core debe pasar por el VFS.
//
// Se usa la variante de 64 bits (zlib_filefunc64_def, no
// zlib_filefunc_def): con los callbacks de 32 bits minizip trunca cualquier
// desplazamiento por encima de 4 GiB, que es parte de lo que impide leer un
// .zip en formato Zip64.
//
// A diferencia de ao_get_lib(), minizip sí pasa un puntero de contexto
// ('opaque') en cada llamada, así que no hace falta estado global.
//
// Los valores ZLIB_FILEFUNC_SEEK_SET/CUR/END (0/1/2) coinciden
// numéricamente con los RETRO_VFS_SEEK_POSITION_*, así que 'origin' se
// reenvía tal cual, sin tabla de traducción.

#pragma once

#include "vfs_bridge.hpp"

#include <cstring>
#include <new>
#include <vector>

extern "C" {
#include "zconf.h"
#include "zlib.h"
#include "ioapi.h"
}

namespace zip_vfs_adapter {

// LECTURA ANTICIPADA. minizip pide el .zip a trozos pequeños: medido sobre
// un álbum de 60 entradas y 251.382.484 bytes, 79.167 lecturas de 3.284
// bytes de media y 23.231 seeks. En Linux eso cuesta 2,4 s y no se nota,
// pero cada llamada cruza la frontera del core y llega al VFS del
// frontend, y en Windows ese coste por llamada domina: el mismo álbum
// tardaba minutos.
//
// Con un bloque de 256 KiB por stream, una lectura secuencial toca el VFS
// una vez cada 256 KiB en vez de cada 3 KiB, y un seek dentro del bloque
// ya cacheado no toca el VFS en absoluto -- que es la mitad barata del
// problema, porque minizip salta constantemente entre cabecera local y
// datos.
constexpr std::size_t kZipReadBlock = 64 * 1024;

// DOS bloques, no uno. minizip alterna entre el directorio central (para
// localizar la entrada) y los datos comprimidos, y con un solo bloque cada
// salto desalojaba el otro: medido sobre un .zip de 8.348.990 bytes con dos
// entradas, 29.884.416 bytes leídos, o sea el fichero 3,6 veces. Con dos
// bloques el directorio se queda residente mientras los datos pasan por el
// otro.
struct Block {
    std::vector<uint8_t> data;
    int64_t start = -1;   // -1 = vacío
    int64_t len   = 0;
    uint64_t stamp = 0;   // para desalojar el menos usado
};

struct BufferedStream {
    IVFSBridge* vfs    = nullptr;
    void*       handle = nullptr;
    int64_t     pos    = 0;   // posición lógica que ve minizip
    int64_t     vfs_pos = 0;  // posición real del handle subyacente
    Block       blocks[2];
    uint64_t    tick = 0;
};

inline voidpf ZCALLBACK zopen64(voidpf opaque, const void* filename, int /*mode*/) {
    auto* vfs = static_cast<IVFSBridge*>(opaque);
    // Solo lectura: el core nunca escribe archivos .zip.
    void* h = vfs->stream_open(static_cast<const char*>(filename));
    if (!h) return nullptr;
    auto* bs = new (std::nothrow) BufferedStream();
    if (!bs) { vfs->stream_close(h); return nullptr; }
    bs->vfs = vfs;
    bs->handle = h;
    return bs;
}

inline uLong ZCALLBACK zread(voidpf /*opaque*/, voidpf stream, void* buf, uLong size) {
    auto* bs = static_cast<BufferedStream*>(stream);
    if (!bs || !bs->handle) return 0;

    auto* out = static_cast<uint8_t*>(buf);
    uLong done = 0;

    while (done < size) {
        // Petición de al menos un bloque: DIRECTA, y se comprueba ANTES que
        // la caché a propósito. Al revés, una petición de 64 KiB se servía
        // en parte desde el bloque cacheado y el resto disparaba un relleno
        // de otros 64 KiB, leyendo el doble de bytes de los necesarios:
        // medido, 509 MB para un .zip de 251 MB.
        if (size - done >= kZipReadBlock) {
            if (bs->vfs_pos != bs->pos) {
                if (bs->vfs->stream_seek(bs->handle, bs->pos,
                                         RETRO_VFS_SEEK_POSITION_START) < 0) break;
                bs->vfs_pos = bs->pos;
            }
            const int64_t got = bs->vfs->stream_read(bs->handle, out + done, size - done);
            if (got <= 0) break;
            bs->vfs_pos += got;
            bs->pos     += got;
            done        += static_cast<uLong>(got);
            continue;
        }

        // ¿Cae la posición actual en alguno de los dos bloques?
        Block* hit = nullptr;
        for (Block& b : bs->blocks) {
            if (b.start >= 0 && bs->pos >= b.start && bs->pos < b.start + b.len) {
                hit = &b;
                break;
            }
        }
        if (hit) {
            hit->stamp = ++bs->tick;
            const int64_t off = bs->pos - hit->start;
            std::size_t n = static_cast<std::size_t>(hit->len - off);
            if (n > size - done) n = size - done;
            std::memcpy(out + done, hit->data.data() + off, n);
            done    += static_cast<uLong>(n);
            bs->pos += static_cast<int64_t>(n);
            continue;
        }

        // Rellenar: se desaloja el bloque usado hace más tiempo.
        Block* victim = &bs->blocks[0];
        for (Block& b : bs->blocks) {
            if (b.start < 0) { victim = &b; break; }
            if (b.stamp < victim->stamp) victim = &b;
        }
        if (victim->data.size() != kZipReadBlock) {
            victim->data.resize(kZipReadBlock);
            if (victim->data.size() != kZipReadBlock) break;   // sin memoria
        }
        // El bloque se ALINEA a un múltiplo de kZipReadBlock en vez de
        // empezar en la posición pedida. unzOpen() busca el End Of Central
        // Directory recorriendo el final del fichero HACIA ATRÁS a pasos de
        // 1 KiB; rellenando hacia delante desde 'pos', cada paso atrás caía
        // justo antes del bloque y fallaba. Medido sobre worms.zip
        // (8.349.212 bytes): la región del directorio central se leía 255
        // veces y el fichero entero 3,6 veces. Alineado, cualquier lectura
        // dentro de esos 64 KiB acierta, se vaya hacia delante o hacia atrás.
        const int64_t aligned = bs->pos & ~static_cast<int64_t>(kZipReadBlock - 1);
        if (bs->vfs_pos != aligned) {
            if (bs->vfs->stream_seek(bs->handle, aligned,
                                     RETRO_VFS_SEEK_POSITION_START) < 0) break;
            bs->vfs_pos = aligned;
        }
        const int64_t got = bs->vfs->stream_read(bs->handle, victim->data.data(), kZipReadBlock);
        if (got <= 0) { victim->start = -1; victim->len = 0; break; }
        bs->vfs_pos  += got;
        victim->start = aligned;
        victim->len   = got;
        victim->stamp = ++bs->tick;
        if (bs->pos >= aligned + got) break;   // EOF dentro del bloque
    }
    return done;
}

inline uLong ZCALLBACK zwrite(voidpf, voidpf, const void*, uLong) {
    return 0; // nunca se usa: solo lectura.
}

inline ZPOS64_T ZCALLBACK ztell64(voidpf /*opaque*/, voidpf stream) {
    auto* bs = static_cast<BufferedStream*>(stream);
    if (!bs) return static_cast<ZPOS64_T>(-1);
    return static_cast<ZPOS64_T>(bs->pos);
}

// El seek es sólo contable: mueve la posición lógica y no toca el VFS. El
// handle subyacente se recoloca de forma perezosa en zread(), y sólo si la
// lectura cae fuera del bloque cacheado. minizip salta mucho de un lado a
// otro, así que la mayoría de estos saltos acaban costando cero.
inline long ZCALLBACK zseek64(voidpf /*opaque*/, voidpf stream, ZPOS64_T offset, int origin) {
    auto* bs = static_cast<BufferedStream*>(stream);
    if (!bs || !bs->handle) return -1;

    int64_t target;
    if (origin == ZLIB_FILEFUNC_SEEK_SET) {
        target = static_cast<int64_t>(offset);
    } else if (origin == ZLIB_FILEFUNC_SEEK_CUR) {
        target = bs->pos + static_cast<int64_t>(offset);
    } else if (origin == ZLIB_FILEFUNC_SEEK_END) {
        const int64_t sz = bs->vfs->stream_size(bs->handle);
        if (sz < 0) return -1;
        target = sz + static_cast<int64_t>(offset);
    } else {
        return -1;
    }
    if (target < 0) return -1;
    bs->pos = target;
    return 0;
}

inline int ZCALLBACK zclose(voidpf /*opaque*/, voidpf stream) {
    auto* bs = static_cast<BufferedStream*>(stream);
    if (!bs) return -1;
    const int r = bs->handle ? bs->vfs->stream_close(bs->handle) : 0;
    delete bs;
    return r;
}

inline int ZCALLBACK zerror(voidpf, voidpf) {
    return 0; // ver ioapi.h: casi nunca se consulta; nuestros métodos ya señalan error por valor de retorno.
}

// Rellena una zlib_filefunc64_def que usa 'vfs' como backend de E/S.
inline zlib_filefunc64_def make(IVFSBridge& vfs) {
    zlib_filefunc64_def f{};
    f.zopen64_file = &zopen64;
    f.zread_file   = &zread;
    f.zwrite_file  = &zwrite;
    f.ztell64_file = &ztell64;
    f.zseek64_file = &zseek64;
    f.zclose_file  = &zclose;
    f.zerror_file  = &zerror;
    f.opaque       = &vfs;
    return f;
}

} // namespace zip_vfs_adapter
