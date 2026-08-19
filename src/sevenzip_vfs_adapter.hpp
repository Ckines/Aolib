// sevenzip_vfs_adapter.hpp
//
// El SDK de 7-Zip (deps/sevenzip) lee a través de un ISeekInStream (Read +
// Seek) que el propio SDK envuelve en un ILookInStream con búfer de
// lectura anticipada (CLookToRead2, ver 7zStream.c) -- ese envoltorio es
// el que necesitan SzArEx_Open() y SzArEx_Extract(), y no hace falta
// reimplementarlo aquí.
//
// Igual que zip_vfs_adapter.hpp para minizip: el ISeekInStream que se le
// entrega usa el VFS Bridge (IVFSBridge) y nunca fopen() real. El SDK no
// trae por sí mismo ninguna implementación basada en VFS -- su propio
// 7zFile.c es fopen()/fread() puro y por eso no se vendoriza (ver
// deps/sevenzip/VENDOR.md).
//
// Semántica de Seek: ISeekInStream::Seek recibe un Int64* que es a la vez
// entrada (desplazamiento pedido) y salida (posición absoluta resultante
// -- el SDK la usa para conocer el tamaño del archivo con Seek(0,
// SEEK_END)). retro_vfs_seek() solo devuelve 0 en éxito / -1 en fallo
// ("@return 0 on success, -1 on failure", libretro.h), nunca la posición.
// La posición real se pide aparte con stream_tell() tras cada seek
// exitoso -- ver vfs_seek() más abajo.

#pragma once

#include "vfs_bridge.hpp"

extern "C" {
#include "7zTypes.h"
}

namespace sevenzip_vfs_adapter {

// Contexto de la implementación: un ISeekInStream normal en C es "un
// struct cuyo primer miembro es la vtable", así que el propio struct hace
// de contexto -- no hace falta un 'opaque' aparte como en zip_vfs_adapter.
struct VFSSeekInStream {
    ISeekInStream vt;   // DEBE ser el primer miembro: el SDK hace cast a este puntero.
    IVFSBridge* vfs;
    void* handle;
};

inline SRes vfs_read(ISeekInStreamPtr pp, void* buf, size_t* size) {
    auto* p = const_cast<VFSSeekInStream*>(reinterpret_cast<const VFSSeekInStream*>(pp));
    if (*size == 0) return SZ_OK;
    const int64_t got = p->vfs->stream_read(p->handle, buf, static_cast<uint64_t>(*size));
    if (got < 0) { *size = 0; return SZ_ERROR_READ; }
    *size = static_cast<size_t>(got);
    return SZ_OK;
}

inline SRes vfs_seek(ISeekInStreamPtr pp, Int64* pos, ESzSeek origin) {
    auto* p = const_cast<VFSSeekInStream*>(reinterpret_cast<const VFSSeekInStream*>(pp));
    // ESzSeek (SZ_SEEK_SET/CUR/END = 0/1/2) coincide numéricamente con
    // RETRO_VFS_SEEK_POSITION_*, así que 'origin' se reenvía tal cual.
    //
    // retro_vfs_seek() solo confirma éxito/fallo (ver cabecera del
    // fichero): la posición absoluta resultante -- lo que el SDK espera
    // encontrar en '*pos' al volver -- se pide aparte con stream_tell().
    const int64_t seek_status = p->vfs->stream_seek(p->handle, *pos, static_cast<int>(origin));
    if (seek_status < 0) return SZ_ERROR_READ;
    const int64_t actual_pos = p->vfs->stream_tell(p->handle);
    if (actual_pos < 0) return SZ_ERROR_READ;
    *pos = actual_pos;
    return SZ_OK;
}

inline void init(VFSSeekInStream& s, IVFSBridge& vfs, void* handle) {
    s.vt.Read = &vfs_read;
    s.vt.Seek = &vfs_seek;
    s.vfs = &vfs;
    s.handle = handle;
}

} // namespace sevenzip_vfs_adapter
