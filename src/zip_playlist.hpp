// zip_playlist.hpp
//
// Reproducción de un .zip con varias pistas "en orden y sin M3U", igual que
// hace el core oficial libretro-gme: el CORE abre el .zip él mismo con
// minizip, enumera cada entrada con extensión soportada y la lee entera a
// memoria. RetroArch no interviene, y por eso no hace falta ningún M3U ni
// que el frontend sepa nada de listas de reproducción.
//
// Diferencia deliberada con el core oficial: la E/S de minizip pasa por
// zip_vfs_adapter (IVFSBridge) y nunca por fopen() real, así que se usa
// unzOpen2() con callbacks propios en vez de unzOpen().

#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <functional>
#include <new>
#include <string>
#include <vector>

#include "vfs_bridge.hpp"
#include "zip_vfs_adapter.hpp"

extern "C" {
#include "unzip.h"
}

struct ZipEntry {
    std::string name;           // nombre del fichero dentro del .zip (con extensión)
    std::vector<uint8_t> data;  // contenido ya descomprimido, completo

    // Metadata calculada una sola vez por .zip cargado (ver
    // libretro.cpp::precompute_zip_track_durations()). 0/vacío = todavía no
    // calculado, que para la UI es lo mismo que desconocido ("--:--").
    uint64_t cached_length_frames = 0;
    std::string cached_title;
};

// Cotas de memoria OBLIGATORIAS: un .zip puede declarar en su directorio
// central un 'uncompressed_size' arbitrario, sin relación con los bytes
// realmente almacenados, y entry.data.resize() se lo creería. Con un .zip
// de ~120 bytes fabricado a mano: declarado 1 GiB reserva 1 GiB
// transitorio; 3,75 GiB acaba en el OOM killer; 4 GiB lanza un
// std::bad_alloc que cruza la frontera extern "C" de retro_load_game() y
// llama a terminate() -- muere RetroArch entero, no solo el core.
//
// 64 MiB por entrada es generoso para cualquier pista real, incluidas las
// de PSF2 con samples grandes; 512 MiB de presupuesto total dan margen para
// álbumes de cientos de pistas sin comprometer un proceso de 32 bits en
// Windows.
constexpr std::size_t kMaxZipEntryBytes = 64ull * 1024 * 1024;
constexpr std::size_t kMaxZipTotalBytes = 512ull * 1024 * 1024;

// Extensiones reconocidas dentro de un .zip. Incluye las reproducibles y
// también las dependencias puras (.psflib/.lib/.psf2lib/.ssflib): estas
// últimas nunca son pista, pero tienen que estar en memoria para poder
// resolver "_lib" entre entradas del mismo zip (ver
// AosdkLibResolver::SiblingLookup). Sin ellas, un álbum PSF cuyas pistas
// comparten un .psflib común -- el caso típico -- falla en cascada.
//
// MANTENER SINCRONIZADA a mano con valid_extensions y
// SET_CONTENT_INFO_OVERRIDE (libretro.cpp) y con los ficheros .info: son
// listas independientes y nada las cuadra automáticamente. Olvidar esta ha
// costado ya dos fallos idénticos en producción (PSF2 primero, SSF después):
// las entradas del .zip se descartan por "extensión no soportada" y el
// álbum carga vacío, sin ningún error que apunte a la causa. Cualquier
// formato nuevo pasa por aquí también.
inline bool zip_entry_extension_supported(const std::string& name) {
    static const char* const kExts[] = {
        ".spc", ".nsf", ".nsfe", ".vgm", ".vgz", ".gbs", ".hes",
        ".kss", ".sap", ".ay", ".gym", ".psf", ".minipsf",
        ".psf2", ".minipsf2",
        ".ssf", ".minissf",
        ".psflib", ".lib", ".psf2lib", ".ssflib"
    };
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = name.substr(dot);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* candidate : kExts) {
        if (ext == candidate) return true;
    }
    return false;
}

// Extensiones que son SOLO dependencias, nunca pistas. enumerate_zip() las
// acepta a propósito (hacen falta para resolver "_lib"), pero ninguna
// construye un motor por sí sola: un .psflib es la mitad compartida de un
// álbum, no una canción.
//
// La distinción vive aquí, y no en la capa de UI, porque es la misma
// cabecera que decide qué entra en el .zip. Sin ella, la numeración de
// pistas que ve el usuario contaría dependencias.
inline bool zip_entry_is_dependency_only(const std::string& name) {
    static const char* const kDepExts[] = {
        ".psflib", ".lib", ".psf2lib", ".ssflib"
    };
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = name.substr(dot);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* candidate : kDepExts) {
        if (ext == candidate) return true;
    }
    return false;
}

// True si la entrada es una pista seleccionable: soportada y no una
// dependencia.
inline bool zip_entry_is_playable(const std::string& name) {
    return zip_entry_extension_supported(name) && !zip_entry_is_dependency_only(name);
}

// Comparación NATURAL de nombres, insensible a mayúsculas: los tramos de
// dígitos se comparan como números, así que "9 Tema" va antes de "10 Tema"
// (lexicográficamente iría después, porque '1' < '9').
//
// Ante empate ignorando el caso se desempata con la comparación sensible,
// para que el orden sea TOTAL y determinista: si dos ficheros distintos
// comparasen "iguales", el resultado quedaría a merced de la
// implementación de std::sort.
inline bool natural_less(const std::string& a, const std::string& b) {
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[j]);

        if (std::isdigit(ca) && std::isdigit(cb)) {
            // Ceros a la izquierda: "007" y "7" valen lo mismo.
            std::size_t ia = i, jb = j;
            while (ia < a.size() && a[ia] == '0') ++ia;
            while (jb < b.size() && b[jb] == '0') ++jb;

            std::size_t ea = ia, eb = jb;
            while (ea < a.size() && std::isdigit(static_cast<unsigned char>(a[ea]))) ++ea;
            while (eb < b.size() && std::isdigit(static_cast<unsigned char>(b[eb]))) ++eb;

            const std::size_t len_a = ea - ia, len_b = eb - jb;
            // Más dígitos significativos = número mayor. No se convierte a
            // entero: un nombre con 40 dígitos desbordaría cualquier tipo.
            if (len_a != len_b) return len_a < len_b;
            for (std::size_t k = 0; k < len_a; ++k) {
                if (a[ia + k] != b[jb + k]) return a[ia + k] < b[jb + k];
            }
            i = ea; j = eb;
            continue;
        }

        const unsigned char la = static_cast<unsigned char>(std::tolower(ca));
        const unsigned char lb = static_cast<unsigned char>(std::tolower(cb));
        if (la != lb) return la < lb;
        ++i; ++j;
    }
    if (i < a.size() || j < b.size()) return (a.size() - i) < (b.size() - j);
    return a < b; // desempate estable: orden total garantizado
}

// Logging opcional: enumerate_zip() no tiene acceso a log_message(), así
// que el llamante inyecta cómo emitir un WARN puntual. nullptr = silencioso
// (lo que usan los arneses de test).
using ZipWarnLogger = std::function<void(const std::string&)>;

// Enumera 'zip_path' (vía 'vfs', sin fopen real) y devuelve en
// 'out_entries' cada fichero soportado, en el ORDEN en que aparece en el
// directorio central del .zip -- normalmente el orden de inserción del
// creador del archivo, que es exactamente lo que hace que "se reproduzcan
// en orden" sin necesitar ningún M3U.
inline bool enumerate_zip(const std::string& zip_path, IVFSBridge& vfs,
                           std::vector<ZipEntry>& out_entries,
                           const ZipWarnLogger& warn = nullptr) {
    out_entries.clear();
    std::size_t total_reserved = 0;   // bytes ya aceptados en out_entries
    bool budget_warned = false;       // un único WARN cuando se agota el presupuesto, no uno por entrada

    zlib_filefunc_def io = zip_vfs_adapter::make(vfs);
    unzFile zf = unzOpen2(zip_path.c_str(), &io);
    if (!zf) return false;

    unz_global_info gi{};
    if (unzGetGlobalInfo(zf, &gi) != UNZ_OK) {
        unzClose(zf);
        return false;
    }

    if (unzGoToFirstFile(zf) != UNZ_OK) {
        unzClose(zf);
        return gi.number_entry == 0; // zip vacío no es un error de E/S
    }

    for (uLong i = 0; i < gi.number_entry; ++i) {
        unz_file_info info{};
        char name_buf[512] = {0};
        if (unzGetCurrentFileInfo(zf, &info, name_buf, sizeof(name_buf) - 1,
                                    nullptr, 0, nullptr, 0) != UNZ_OK) {
            unzClose(zf);
            return false;
        }

        const std::string entry_name(name_buf);
        const bool is_dir = !entry_name.empty() &&
                             (entry_name.back() == '/' || entry_name.back() == '\\');

        if (!is_dir && zip_entry_extension_supported(entry_name)) {
            // Límite POR ENTRADA: rechaza solo esta entrada, no aborta el
            // resto del .zip.
            if (info.uncompressed_size > kMaxZipEntryBytes) {
                if (warn) {
                    warn("[aolib] " + entry_name + ": entrada de " +
                         std::to_string(info.uncompressed_size) +
                         " bytes supera el límite por entrada (" +
                         std::to_string(kMaxZipEntryBytes) + " bytes), omitida.");
                }
                if (i + 1 < gi.number_entry) {
                    if (unzGoToNextFile(zf) != UNZ_OK) break;
                }
                continue;
            }

            // Presupuesto TOTAL del .zip: un único WARN al agotarse, no uno
            // por entrada rechazada, para no inundar el log.
            if (total_reserved + info.uncompressed_size > kMaxZipTotalBytes) {
                if (!budget_warned && warn) {
                    warn("[aolib] " + zip_path + ": presupuesto total del .zip (" +
                         std::to_string(kMaxZipTotalBytes) +
                         " bytes) agotado, entradas restantes omitidas.");
                    budget_warned = true;
                }
                if (i + 1 < gi.number_entry) {
                    if (unzGoToNextFile(zf) != UNZ_OK) break;
                }
                continue;
            }

            if (unzOpenCurrentFile(zf) == UNZ_OK) {
                ZipEntry entry;
                entry.name = entry_name;
                // Defensa en profundidad: el límite por entrada debería
                // hacer esto inalcanzable. Si captura, la entrada se trata
                // como fallida.
                try {
                    entry.data.resize(info.uncompressed_size);
                } catch (const std::bad_alloc&) {
                    unzCloseCurrentFile(zf);
                    if (warn) {
                        warn("[aolib] " + entry_name +
                             ": std::bad_alloc reservando " +
                             std::to_string(info.uncompressed_size) +
                             " bytes, omitida.");
                    }
                    if (i + 1 < gi.number_entry) {
                        if (unzGoToNextFile(zf) != UNZ_OK) break;
                    }
                    continue;
                }

                uLong total_read = 0;
                bool read_ok = true;
                while (total_read < info.uncompressed_size) {
                    const int got = unzReadCurrentFile(
                        zf, entry.data.data() + total_read,
                        static_cast<unsigned int>(info.uncompressed_size - total_read));
                    if (got <= 0) { read_ok = (got == 0); break; }
                    total_read += static_cast<uLong>(got);
                }
                unzCloseCurrentFile(zf);

                if (read_ok && total_read == info.uncompressed_size) {
                    total_reserved += info.uncompressed_size; // solo lo realmente aceptado
                    out_entries.push_back(std::move(entry));
                }
                // Entradas que no se pueden leer se omiten en silencio del
                // listado (no abortan el .zip entero); se registran fuera
                // de esta función si el llamante quiere loguearlo.
            }
        }

        if (i + 1 < gi.number_entry) {
            if (unzGoToNextFile(zf) != UNZ_OK) break;
        }
    }

    unzClose(zf);

    // ORDENACIÓN OBLIGATORIA: hasta aquí las entradas están en el orden del
    // directorio central del .zip, que NO es el orden del álbum. Ese orden
    // lo fija la herramienta que creó el archivo (`zip -r` recorre el
    // directorio como se lo dé el sistema de ficheros), así que un álbum
    // real puede empezar por la pista 402 y seguir por la 103.
    //
    // Natural y no lexicográfica: con nombres sin rellenar a ceros, "10"
    // iría antes que "9".
    std::sort(out_entries.begin(), out_entries.end(),
              [](const ZipEntry& a, const ZipEntry& b) {
                  return natural_less(a.name, b.name);
              });

    return true;
}
