// zip_vfs_adapter.hpp
//
// minizip (deps/aosdk/zlib/unzip.c) admite un juego de callbacks de E/S
// propio vía unzOpen2() + zlib_filefunc_def. Usarlo, en vez de unzOpen() a
// secas, es obligatorio aquí: unzOpen() llama a fopen() real por dentro
// (ioapi.c::fopen_file_func), y toda la E/S del core debe pasar por el VFS.
//
// A diferencia de ao_get_lib(), minizip sí pasa un puntero de contexto
// ('opaque') en cada llamada, así que no hace falta estado global.
//
// Los valores ZLIB_FILEFUNC_SEEK_SET/CUR/END (0/1/2) coinciden
// numéricamente con los RETRO_VFS_SEEK_POSITION_*, así que 'origin' se
// reenvía tal cual, sin tabla de traducción.

#pragma once

#include "vfs_bridge.hpp"

extern "C" {
#include "zconf.h"
#include "zlib.h"
#include "ioapi.h"
}

namespace zip_vfs_adapter {

inline voidpf ZCALLBACK zopen(voidpf opaque, const char* filename, int /*mode*/) {
    auto* vfs = static_cast<IVFSBridge*>(opaque);
    // Solo lectura: el core nunca escribe archivos .zip.
    return vfs->stream_open(filename);
}

inline uLong ZCALLBACK zread(voidpf opaque, voidpf stream, void* buf, uLong size) {
    auto* vfs = static_cast<IVFSBridge*>(opaque);
    const int64_t got = vfs->stream_read(stream, buf, size);
    return (got < 0) ? 0 : static_cast<uLong>(got);
}

inline uLong ZCALLBACK zwrite(voidpf, voidpf, const void*, uLong) {
    return 0; // nunca se usa: solo lectura.
}

inline long ZCALLBACK ztell(voidpf opaque, voidpf stream) {
    auto* vfs = static_cast<IVFSBridge*>(opaque);
    return static_cast<long>(vfs->stream_tell(stream));
}

inline long ZCALLBACK zseek(voidpf opaque, voidpf stream, uLong offset, int origin) {
    auto* vfs = static_cast<IVFSBridge*>(opaque);
    const int64_t r = vfs->stream_seek(stream, static_cast<int64_t>(offset), origin);
    return (r < 0) ? -1 : 0; // minizip espera 0/-1 aquí, no la posición (a diferencia de retro_vfs_seek)
}

inline int ZCALLBACK zclose(voidpf opaque, voidpf stream) {
    auto* vfs = static_cast<IVFSBridge*>(opaque);
    return vfs->stream_close(stream);
}

inline int ZCALLBACK zerror(voidpf, voidpf) {
    return 0; // ver ioapi.h: casi nunca se consulta; nuestros métodos ya señalan error por valor de retorno.
}

// Rellena una zlib_filefunc_def que usa 'vfs' como backend de E/S.
inline zlib_filefunc_def make(IVFSBridge& vfs) {
    zlib_filefunc_def f{};
    f.zopen_file  = &zopen;
    f.zread_file  = &zread;
    f.zwrite_file = &zwrite;
    f.ztell_file  = &ztell;
    f.zseek_file  = &zseek;
    f.zclose_file = &zclose;
    f.zerror_file = &zerror;
    f.opaque      = &vfs;
    return f;
}

} // namespace zip_vfs_adapter
