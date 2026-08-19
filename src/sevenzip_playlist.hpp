// sevenzip_playlist.hpp
//
// Lectura de un .7z con varias pistas, análoga a enumerate_zip()
// (zip_playlist.hpp): el CORE abre el .7z él mismo con el SDK de 7-Zip
// vendorizado (deps/sevenzip), enumera cada entrada con extensión
// soportada y la descomprime entera a memoria, en el mismo std::vector
// de ZipEntry que ya usa el resto del pipeline de carga -- desde
// libretro.cpp, un .7z y un .zip son indistinguibles después de
// enumerate_7z()/enumerate_zip().
//
// Toda la E/S pasa por sevenzip_vfs_adapter (IVFSBridge), nunca por
// fopen() real: ver deps/sevenzip/VENDOR.md.
//
// Diferencia de fondo con .zip: un .7z puede ser "sólido" (varios
// ficheros comprimidos juntos en un único flujo LZMA/LZMA2, sin límites
// de fichero individual dentro del flujo). SzArEx_Extract() ya cachea el
// bloque sólido decodificado entre llamadas sucesivas (parámetros
// blockIndex/outBuffer/outBufferSize) -- aquí se reutiliza esa caché a lo
// largo de todo el álbum en vez de descartarla entrada a entrada, así que
// un álbum sólido de N pistas descomprime su bloque una sola vez, no N
// veces.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <functional>
#include <new>
#include <string>
#include <vector>

#include "vfs_bridge.hpp"
#include "sevenzip_vfs_adapter.hpp"
#include "zip_playlist.hpp"  // ZipEntry, zip_entry_extension_supported(), natural_less()

extern "C" {
#include "7z.h"
#include "7zAlloc.h"
#include "7zCrc.h"
}

// Misma cota que zip_playlist.hpp y por el mismo motivo: un .7z declara
// los tamaños sin comprimir en su propia cabecera (aquí, en SzArEx),
// igual de manipulable que el 'uncompressed_size' de un directorio
// central de zip.
inline bool sevenzip_crc_table_ready() {
    static bool ready = []() { CrcGenerateTable(); return true; }();
    return ready;
}

inline bool enumerate_7z(const std::string& archive_path, IVFSBridge& vfs,
                          std::vector<ZipEntry>& out_entries,
                          const ZipWarnLogger& warn = nullptr) {
    out_entries.clear();
    sevenzip_crc_table_ready();

    void* handle = vfs.stream_open(archive_path.c_str());
    if (!handle) return false;

    sevenzip_vfs_adapter::VFSSeekInStream seek_stream{};
    sevenzip_vfs_adapter::init(seek_stream, vfs, handle);

    CLookToRead2 look_stream;
    LookToRead2_INIT(&look_stream)
    look_stream.realStream = &seek_stream.vt;
    // lookahead=1: SzArEx_Open necesita mirar hacia delante en la cabecera
    // sin consumir el flujo; ver el propio uso de esta bandera en 7zMain.c
    // del SDK.
    LookToRead2_CreateVTable(&look_stream, /*lookahead=*/1);
    Byte look_buf[1 << 14];
    look_stream.bufSize = sizeof(look_buf);
    look_stream.buf = look_buf;

    const ISzAlloc alloc_main{ &SzAlloc, &SzFree };
    const ISzAlloc alloc_temp{ &SzAllocTemp, &SzFreeTemp };

    CSzArEx db;
    SzArEx_Init(&db);

    const SRes open_res = SzArEx_Open(&db, &look_stream.vt, &alloc_main, &alloc_temp);
    if (open_res != SZ_OK) {
        SzArEx_Free(&db, &alloc_main);
        vfs.stream_close(handle);
        if (warn) {
            warn("[aolib] " + archive_path +
                 ": no se pudo abrir el .7z (código " + std::to_string(open_res) +
                 "; cabecera dañada, cifrada, o no es realmente un archivo 7z).");
        }
        return false;
    }

    // Caché de bloque sólido entre entradas -- ver cabecera del fichero.
    // Debe iniciarse en 0/nullptr/0 antes de la primera llamada, tal como
    // exige la documentación de SzArEx_Extract() en 7z.h.
    UInt32 block_index = 0xFFFFFFFF;
    Byte* out_buffer = nullptr;
    size_t out_buffer_size = 0;

    std::size_t total_reserved = 0;
    bool budget_warned = false;

    for (UInt32 i = 0; i < db.NumFiles; ++i) {
        if (SzArEx_IsDir(&db, i)) continue;

        const size_t name_len = SzArEx_GetFileNameUtf16(&db, i, nullptr);
        if (name_len == 0) continue;
        std::vector<UInt16> name_utf16(name_len);
        SzArEx_GetFileNameUtf16(&db, i, name_utf16.data());

        // UTF-16LE -> UTF-8. Los nombres reales de este proyecto (rips de
        // PSF/VGM/etc.) son casi siempre ASCII; esta conversión cubre el
        // resto sin arrastrar <codecvt> (obsoleto) ni ICU. Sustitutos "?"
        // para pares suplentes: ningún formato soportado los necesita en
        // el nombre de pista.
        std::string name_utf8;
        name_utf8.reserve(name_len);
        for (size_t k = 0; k + 1 < name_utf16.size(); ++k) {
            const uint32_t cp = name_utf16[k];
            if (cp == 0) break; // NUL terminador incluido por SzArEx_GetFileNameUtf16
            if (cp < 0x80) {
                name_utf8 += static_cast<char>(cp);
            } else if (cp < 0x800) {
                name_utf8 += static_cast<char>(0xC0 | (cp >> 6));
                name_utf8 += static_cast<char>(0x80 | (cp & 0x3F));
            } else if (cp >= 0xD800 && cp <= 0xDBFF) {
                name_utf8 += '?'; // par suplente: no se resuelve, ver comentario arriba
            } else {
                name_utf8 += static_cast<char>(0xE0 | (cp >> 12));
                name_utf8 += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                name_utf8 += static_cast<char>(0x80 | (cp & 0x3F));
            }
        }

        if (!zip_entry_extension_supported(name_utf8)) continue;

        const UInt64 declared_size = SzArEx_GetFileSize(&db, i);
        if (declared_size > kMaxZipEntryBytes) {
            if (warn) {
                warn("[aolib] " + name_utf8 + ": entrada de " +
                     std::to_string(declared_size) +
                     " bytes supera el límite por entrada (" +
                     std::to_string(kMaxZipEntryBytes) + " bytes), omitida.");
            }
            continue;
        }
        if (total_reserved + static_cast<std::size_t>(declared_size) > kMaxZipTotalBytes) {
            if (!budget_warned && warn) {
                warn("[aolib] " + archive_path + ": presupuesto total del .7z (" +
                     std::to_string(kMaxZipTotalBytes) +
                     " bytes) agotado, entradas restantes omitidas.");
                budget_warned = true;
            }
            continue;
        }

        size_t offset = 0;
        size_t out_size_processed = 0;
        const SRes extract_res = SzArEx_Extract(
            &db, &look_stream.vt, i, &block_index,
            &out_buffer, &out_buffer_size, &offset, &out_size_processed,
            &alloc_main, &alloc_temp);

        if (extract_res != SZ_OK) {
            if (warn) {
                warn("[aolib] " + name_utf8 +
                     ": la descompresión desde el .7z falló (código " +
                     std::to_string(extract_res) + "), omitida.");
            }
            continue;
        }

        ZipEntry entry;
        entry.name = std::move(name_utf8);
        try {
            entry.data.assign(out_buffer + offset, out_buffer + offset + out_size_processed);
        } catch (const std::bad_alloc&) {
            if (warn) {
                warn("[aolib] " + entry.name +
                     ": std::bad_alloc copiando la entrada descomprimida, omitida.");
            }
            continue;
        }
        total_reserved += entry.data.size();
        out_entries.push_back(std::move(entry));
    }

    if (out_buffer) {
        ISzAlloc_Free(&alloc_main, out_buffer);
    }
    SzArEx_Free(&db, &alloc_main);
    vfs.stream_close(handle);

    // Mismo motivo que en enumerate_zip(): el orden de SzArEx (orden de
    // aparición en la cabecera del .7z, que a su vez suele reflejar el
    // orden en que 7-Zip recorrió el sistema de ficheros al comprimir) no
    // es el orden del álbum.
    std::sort(out_entries.begin(), out_entries.end(),
              [](const ZipEntry& a, const ZipEntry& b) {
                  return natural_less(a.name, b.name);
              });

    return true;
}
