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

extern "C" {
#include "zconf.h"
#include "zlib.h"
#include "ioapi.h"
}

namespace zip_vfs_adapter {

inline voidpf ZCALLBACK zopen64(voidpf opaque, const void* filename, int /*mode*/) {
    auto* vfs = static_cast<IVFSBridge*>(opaque);
    // Solo lectura: el core nunca escribe archivos .zip.
    return vfs->stream_open(static_cast<const char*>(filename));
}

inline uLong ZCALLBACK zread(voidpf opaque, voidpf stream, void* buf, uLong size) {
    auto* vfs = static_cast<IVFSBridge*>(opaque);
    const int64_t got = vfs->stream_read(stream, buf, size);
    return (got < 0) ? 0 : static_cast<uLong>(got);
}

inline uLong ZCALLBACK zwrite(voidpf, voidpf, const void*, uLong) {
    return 0; // nunca se usa: solo lectura.
}

inline ZPOS64_T ZCALLBACK ztell64(voidpf opaque, voidpf stream) {
    auto* vfs = static_cast<IVFSBridge*>(opaque);
    const int64_t pos = vfs->stream_tell(stream);
    // minizip señala "posición desconocida" con (ZPOS64_T)-1, no con 0.
    return (pos < 0) ? static_cast<ZPOS64_T>(-1) : static_cast<ZPOS64_T>(pos);
}

inline long ZCALLBACK zseek64(voidpf opaque, voidpf stream, ZPOS64_T offset, int origin) {
    auto* vfs = static_cast<IVFSBridge*>(opaque);
    const int64_t r = vfs->stream_seek(stream, static_cast<int64_t>(offset), origin);
    return (r < 0) ? -1 : 0; // minizip espera 0/-1; retro_vfs_seek() ya devuelve exactamente eso (ver vfs_bridge.hpp)
}

inline int ZCALLBACK zclose(voidpf opaque, voidpf stream) {
    auto* vfs = static_cast<IVFSBridge*>(opaque);
    return vfs->stream_close(stream);
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
