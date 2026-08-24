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
// unzOpen2_64() con callbacks propios en vez de unzOpen().
//
// Toda la API usada aquí es la de 64 bits (unzGetGlobalInfo64,
// unz_file_info64, unzGetCurrentFileInfo64). No es cosmético: las
// variantes de 32 bits rechazan cualquier archivo en formato Zip64 --
// incluidos los .zip pequeños que 7-Zip u otras herramientas marcan como
// Zip64 sin necesidad-- y el fallo se manifiesta como un .zip que
// "no contiene ningún fichero soportado".

#pragma once

#include <algorithm>

#ifdef AOLIB_WITH_VGMSTREAM
#include "vgmstream_extensions.hpp"
#endif

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

// Métodos de compresión que minizip sabe descomprimir. El resto (LZMA=14,
// BZIP2=12, PPMd=98, XZ=95, Zstandard=93...) es ZIP legal pero queda fuera
// de zlib. 7-Zip y WinRAR ofrecen esos métodos en su diálogo de "añadir a
// .zip", así que aparecen en archivos reales con más frecuencia de la que
// cabría esperar; se rechazan con un mensaje explícito en vez de dejar el
// álbum vacío sin causa aparente.
inline const char* zip_compression_method_name(uint16_t method) {
    switch (method) {
        case 0:  return "Store";
        case 8:  return "Deflate";
        case 9:  return "Deflate64";
        case 12: return "BZip2";
        case 14: return "LZMA";
        case 93: return "Zstandard";
        case 95: return "XZ";
        case 96: return "JPEG";
        case 98: return "PPMd";
        default: return "desconocido";
    }
}

inline bool zip_compression_method_supported(uint16_t method) {
    return method == 0 || method == 8;
}

struct ZipEntry {
    std::string name;           // nombre del fichero dentro del .zip (con extensión)

    // Contenido descomprimido. Para la mayoría de formatos está COMPLETO.
    // Para los de streaming (ver 'lazy') contiene solo los primeros
    // kZipHeaderPrefix bytes hasta que alguien pida materializarla.
    std::vector<uint8_t> data;

    // Tamaño real de la entrada descomprimida, lo tenga 'data' entero o no.
    // Es lo que hay que reportar como get_size(): un formato que deduce la
    // duración del tamaño del fichero daría una duración corta y falsa si
    // viera solo el prefijo.
    uint64_t full_size = 0;

    // true = 'data' solo tiene el prefijo. Los formatos de streaming pesan
    // entre 2 y 26 MB por pista y un álbum entero no cabe en RAM: Super
    // Mario Galaxy 2 son 784 MB descomprimidos. La cabecera, en cambio,
    // está al principio y con 32 KB basta para saber formato y duración de
    // todo salvo XA (medido en tests/z01_access_pattern.cpp).
    bool lazy = false;

    // Metadata calculada una sola vez por .zip cargado (ver
    // libretro.cpp::precompute_zip_track_durations()). 0/vacío = todavía no
    // calculado, que para la UI es lo mismo que desconocido ("--:--").
    uint64_t cached_length_frames = 0;
    std::string cached_title;

    // Posición de la entrada en el directorio central, guardada durante la
    // enumeración, para saltar a ella con unzGoToFilePos64() en vez de
    // buscarla por nombre.
    //
    // ESTO ES CORRECCIÓN, NO RENDIMIENTO. unzLocateFile() rechaza con
    // UNZ_PARAMERROR cualquier nombre de >= UNZ_MAXFILENAMEINZIP (256,
    // deps/minizip/unzip.c) mientras que enumerate_zip() acepta hasta 511:
    // una entrada con ruta larga se listaba y NUNCA podía materializarse.
    // Reproducido con un .zip de 6 carpetas anidadas (273 caracteres):
    // sin esto, "ya no está en el .zip" y retro_load_game falla; con esto,
    // carga y suena.
    //
    // unzLocateFile es además un escaneo lineal, así que se esperaba una
    // ganancia de tiempo. MEDIDO: no la hay. Álbum de 424 entradas 4,67 ->
    // 4,70 s, y 18 frames por encima del presupuesto antes y después. El
    // directorio central de 424 entradas son ~42 KB y cabe entero en la
    // caché de bloques de zip_vfs_adapter, así que el escaneo es memoria y
    // no E/S, y el inflate de la entrada lo enmascara por completo.
    //
    // 'dir_pos_valid' es false para las entradas de .7z, que no pasan por
    // minizip; ese camino sigue usando el nombre.
    unz64_file_pos dir_pos{};
    bool dir_pos_valid = false;

    bool complete() const { return !lazy || data.size() == full_size; }
};

// Cuánto se infla por adelantado de una entrada perezosa. 32.836 bytes es
// lo máximo que necesitó ningún formato medido para abrir y dar duración
// (SMG2_galaxy01_multi.ast); 64 KiB deja margen sin coste apreciable.
// Excepción conocida: XA lee el fichero entero, porque xa.c recorre todos
// los sectores para enumerar los canales file+channel. Eso no se
// preadivina: se detecta al leer más allá del prefijo y se materializa.
constexpr std::size_t kZipHeaderPrefix = 64 * 1024;

// Cotas de memoria OBLIGATORIAS: un .zip puede declarar en su directorio
// central un 'uncompressed_size' arbitrario, sin relación con los bytes
// realmente almacenados, y entry.data.resize() se lo creería. Con un .zip
// de ~120 bytes fabricado a mano: declarado 1 GiB reserva 1 GiB
// transitorio; 3,75 GiB acaba en el OOM killer; 4 GiB lanza un
// std::bad_alloc que cruza la frontera extern "C" de retro_load_game() y
// llama a terminate() -- muere RetroArch entero, no solo el core.
//
// 256 MiB por entrada. Era 64 MiB cuando el .zip se materializaba ENTERO
// al cargar y el tope por entrada acotaba, de hecho, el álbum. Con
// materialización perezosa lo que se retiene es UNA entrada, así que el
// tope acota una pista: emisoras de radio de GTA Vice City de ~140 MB
// (medido: 146.800.896 bytes) quedaban listadas pero no sonaban, porque
// materialize_zip_entry() las rechazaba en el cambio de pista.
//
// 512 MiB de presupuesto total dan margen para álbumes de cientos de
// pistas sin comprometer un proceso de 32 bits en Windows; para las
// entradas perezosas ese presupuesto cuenta el prefijo de 64 KiB, no los
// megabytes del fichero.
constexpr std::size_t kMaxZipEntryBytes = 256ull * 1024 * 1024;
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
#ifdef AOLIB_WITH_VGMSTREAM
    // Los formatos de streaming tienen su lista en vgmstream_extensions.hpp
    // y se consulta desde aquí en vez de duplicarla: son 32 formatos y
    // mantener dos copias a mano es exactamente el fallo que este comentario
    // lleva advirtiendo desde PSF2.
    if (vgmstream_ext::matches(name)) return true;
#endif
    static const char* const kExts[] = {
        ".spc", ".nsf", ".nsfe", ".vgm", ".vgz", ".gbs", ".hes",
        ".kss", ".sap", ".ay", ".gym",
        ".mod", ".s3m", ".xm", ".it",
        ".psf", ".minipsf",
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
        ".psflib", ".lib", ".psf2lib", ".ssflib",
        // .mih es la cabecera de un MIB+MIH: lleva rate, canales e
        // interleave, y sin ella el .mib no se puede describir. Está en la
        // lista de extensiones soportadas para que enumerate_zip la guarde
        // en memoria y el motor la encuentre como hermana, pero no es una
        // pista: sola no suena.
        ".mih"
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

// Qué entradas se inflan solo hasta la cabecera. Los formatos de
// streaming, y nada más: son los únicos que pesan megabytes por pista.
// Los .mih quedan fuera aunque sean de streaming -- son cabeceras
// completas de pocos bytes y el motor las busca como hermanas, así que
// tienen que estar enteras en memoria.
inline bool zip_entry_is_lazy(const std::string& name) {
#ifdef AOLIB_WITH_VGMSTREAM
    if (zip_entry_is_dependency_only(name)) return false;
    return vgmstream_ext::matches(name);
#else
    (void)name;
    return false;
#endif
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

// Inflado REANUDABLE de una entrada perezosa.
//
// Deflate no permite acceso aleatorio, así que inflar una entrada siempre
// cuesta lo mismo y siempre empieza por el principio: medido, 16,3 MB en
// 97 ms y 5,2 MB en 32 ms, o sea ~168 MB/s sin coste fijo apreciable. Ese
// tiempo no se puede reducir; solo se puede REPARTIR. Lo que no se puede
// es gastarlo entero dentro de un retro_run(), que es lo que hacía la
// versión de una sola tacada: 97 ms contra 16,7 de presupuesto y 64 ms de
// búfer de audio, o sea un corte audible en cada cambio de pista.
//
// El trabajo se parte en un ZipInflateJob que sobrevive entre frames: el
// unzFile se queda abierto y unzReadCurrentFile() continúa donde lo dejó.
// El llamante decide cuánto avanza en cada vuelta.
struct ZipInflateJob {
    unzFile              zf    = nullptr;   // nullptr = no hay trabajo vivo
    std::vector<uint8_t> buf;
    uint64_t             done  = 0;
    uint64_t             total = 0;
    std::string          name;              // para los avisos, y para verificar
                                            // que el trabajo sigue siendo el
                                            // que el llamante cree
    bool                 failed = false;

    bool active()   const { return zf != nullptr; }
    bool complete() const { return zf != nullptr && !failed && done >= total; }
};

// Cierra el trabajo y suelta el buffer a medias. Segura de llamar sobre un
// trabajo inactivo, que es lo que la hace usable desde un destructor.
inline void zip_inflate_abort(ZipInflateJob& job) {
    if (job.zf) {
        unzCloseCurrentFile(job.zf);
        unzClose(job.zf);
        job.zf = nullptr;
    }
    job.buf.clear();
    job.buf.shrink_to_fit();
    job.done = job.total = 0;
    job.failed = false;
    job.name.clear();
}

// Abre el .zip, se coloca en la entrada y reserva el buffer destino. No
// infla nada todavía. Devuelve false si la entrada es inservible.
inline bool zip_inflate_begin(const std::string& zip_path, IVFSBridge& vfs,
                              const ZipEntry& entry, ZipInflateJob& job,
                              const ZipWarnLogger& warn = nullptr) {
    zip_inflate_abort(job);

    if (entry.full_size > static_cast<uint64_t>(kMaxZipEntryBytes)) {
        if (warn) {
            warn("[aolib] " + entry.name + ": la entrada mide " +
                 std::to_string(entry.full_size) +
                 " bytes y supera el límite por entrada (" +
                 std::to_string(kMaxZipEntryBytes) + " bytes).");
        }
        return false;
    }

    zlib_filefunc64_def io = zip_vfs_adapter::make(vfs);
    job.zf = unzOpen2_64(zip_path.c_str(), &io);
    if (!job.zf) return false;

    // Salto directo con la posición guardada al enumerar. El respaldo por
    // nombre (búsqueda exacta, case-sensitive: así fue como se enumeró, y un
    // .zip puede legítimamente traer dos entradas que solo difieran en
    // mayúsculas) queda para las entradas que no la tengan.
    const bool located = entry.dir_pos_valid
                             ? (unzGoToFilePos64(job.zf, &entry.dir_pos) == UNZ_OK)
                             : (unzLocateFile(job.zf, entry.name.c_str(), 1) == UNZ_OK);
    if (!located) {
        unzClose(job.zf);
        job.zf = nullptr;
        if (warn) warn("[aolib] " + entry.name + ": ya no está en el .zip.");
        return false;
    }
    if (unzOpenCurrentFile(job.zf) != UNZ_OK) {
        unzClose(job.zf);
        job.zf = nullptr;
        return false;
    }

    try {
        job.buf.resize(static_cast<std::size_t>(entry.full_size));
    } catch (const std::bad_alloc&) {
        unzCloseCurrentFile(job.zf);
        unzClose(job.zf);
        job.zf = nullptr;
        if (warn) {
            warn("[aolib] " + entry.name + ": std::bad_alloc reservando " +
                 std::to_string(entry.full_size) + " bytes.");
        }
        return false;
    }

    job.done   = 0;
    job.total  = entry.full_size;
    job.name   = entry.name;
    job.failed = false;
    return true;
}

// Infla como mucho 'max_bytes' de salida. Devuelve false si el trabajo ha
// fallado; el llamante distingue "sigue" de "terminado" con complete().
inline bool zip_inflate_step(ZipInflateJob& job, uint64_t max_bytes,
                             const ZipWarnLogger& warn = nullptr) {
    if (!job.zf || job.failed) return false;

    const uint64_t target = (max_bytes >= job.total - job.done)
                                ? job.total
                                : job.done + max_bytes;
    // EOF antes de 'total' es entrada truncada, no fin de trabajo: se
    // detecta con el propio retorno de unzReadCurrentFile y no con
    // unzeof(), para no depender de más API vendorizada de la necesaria.
    bool hit_eof = false;
    while (job.done < target) {
        const uint64_t remaining = target - job.done;
        const unsigned int chunk = (remaining > 0xFFFFFFFFull)
                                       ? 0xFFFFFFFFu
                                       : static_cast<unsigned int>(remaining);
        const int got = unzReadCurrentFile(job.zf, job.buf.data() + job.done, chunk);
        if (got < 0)  { job.failed = true; break; }
        if (got == 0) { hit_eof = true;    break; }
        job.done += static_cast<uint64_t>(got);
    }

    if (job.done >= job.total) return true;   // completo, sin error

    if (job.failed || hit_eof) {
        job.failed = true;
        if (warn) {
            warn("[aolib] " + job.name + ": la descompresión falló tras " +
                 std::to_string(static_cast<unsigned long long>(job.done)) +
                 " de " + std::to_string(static_cast<unsigned long long>(job.total)) +
                 " bytes.");
        }
        return false;
    }
    return true;   // sigue vivo, queda trabajo
}

// Entrega el resultado a la entrada y cierra el trabajo.
inline bool zip_inflate_commit(ZipEntry& entry, ZipInflateJob& job) {
    if (!job.complete()) { zip_inflate_abort(job); return false; }
    entry.data = std::move(job.buf);
    job.buf.clear();
    zip_inflate_abort(job);
    return true;
}

// Infla una entrada perezosa entera, de una tacada. Idempotente: si ya
// está completa no hace nada. Es el camino síncrono de siempre, ahora
// escrito sobre el trabajo reanudable, y sigue siendo el respaldo cuando
// el prefetch no ha llegado a tiempo.
//
// Devuelve false si la entrada ya no está en el archivo o la
// descompresión falla; el llamante debe tratarlo como pista ilegible y
// seguir adelante, no como error fatal.
inline bool materialize_zip_entry(const std::string& zip_path, IVFSBridge& vfs,
                                  ZipEntry& entry,
                                  const ZipWarnLogger& warn = nullptr) {
    if (entry.complete()) return true;

    ZipInflateJob job;
    if (!zip_inflate_begin(zip_path, vfs, entry, job, warn)) return false;
    if (!zip_inflate_step(job, job.total, warn)) { zip_inflate_abort(job); return false; }
    return zip_inflate_commit(entry, job);
}

// Devuelve una entrada perezosa a su prefijo. Se llama al cambiar de
// pista: sin esto la materialización sería acumulativa y el álbum acabaría
// igual de cargado que antes, solo que más tarde.
inline void release_zip_entry(ZipEntry& entry) {
    if (!entry.lazy || entry.data.size() <= kZipHeaderPrefix) return;
    std::vector<uint8_t> prefix(entry.data.begin(),
                                entry.data.begin() +
                                    static_cast<std::ptrdiff_t>(kZipHeaderPrefix));
    entry.data.swap(prefix);   // swap y no assign: libera de verdad la
                               // capacidad grande en vez de conservarla
}

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

    zlib_filefunc64_def io = zip_vfs_adapter::make(vfs);
    unzFile zf = unzOpen2_64(zip_path.c_str(), &io);
    if (!zf) return false;

    unz_global_info64 gi{};
    if (unzGetGlobalInfo64(zf, &gi) != UNZ_OK) {
        unzClose(zf);
        return false;
    }

    if (unzGoToFirstFile(zf) != UNZ_OK) {
        unzClose(zf);
        return gi.number_entry == 0; // zip vacío no es un error de E/S
    }

    for (ZPOS64_T i = 0; i < gi.number_entry; ++i) {
        unz_file_info64 info{};
        char name_buf[512] = {0};
        if (unzGetCurrentFileInfo64(zf, &info, name_buf, sizeof(name_buf) - 1,
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
            // El límite por entrada acota lo que se RETIENE. Una entrada
            // perezosa retiene el prefijo, así que se mide contra el
            // tamaño que se materializaría al reproducirla.
            if (!zip_entry_is_lazy(entry_name) &&
                info.uncompressed_size > static_cast<ZPOS64_T>(kMaxZipEntryBytes)) {
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
            if (static_cast<ZPOS64_T>(total_reserved) + info.uncompressed_size >
                    static_cast<ZPOS64_T>(kMaxZipTotalBytes)) {
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

            // Método de compresión: se comprueba aquí, y no dentro del
            // fallo genérico de unzOpenCurrentFile(), para poder nombrarlo
            // en el aviso.
            const uint16_t method = static_cast<uint16_t>(info.compression_method);
            if (!zip_compression_method_supported(method)) {
                if (warn) {
                    warn("[aolib] " + entry_name + ": método de compresión " +
                         std::to_string(method) + " (" +
                         zip_compression_method_name(method) +
                         ") no soportado; solo Store y Deflate. Vuelve a crear el "
                         ".zip con compresión normal.");
                }
                if (i + 1 < gi.number_entry) {
                    if (unzGoToNextFile(zf) != UNZ_OK) break;
                }
                continue;
            }

            // Las entradas de streaming se inflan solo hasta la cabecera.
            // El resto (PSF y sus _lib, GME, XMP) se materializa entero
            // como siempre: son ficheros de cientos de KB, no dan problema
            // de memoria, y su resolución de dependencias asume tenerlo
            // todo en RAM. Acotar el cambio a los formatos que realmente
            // pesan deja intacto lo que ya funcionaba.
            const bool lazy_entry = zip_entry_is_lazy(entry_name);
            const ZPOS64_T want_bytes =
                lazy_entry ? std::min<ZPOS64_T>(info.uncompressed_size, kZipHeaderPrefix)
                           : info.uncompressed_size;

            if (unzOpenCurrentFile(zf) == UNZ_OK) {
                ZipEntry entry;
                entry.name      = entry_name;
                entry.full_size = info.uncompressed_size;
                entry.lazy      = lazy_entry;
                // Se pide ANTES de leer nada: unzGetFilePos64 describe la
                // entrada en la que está posicionado el cursor del
                // directorio, y materialize_zip_entry() vuelve aquí de un
                // salto en vez de recorrer el directorio comparando nombres.
                entry.dir_pos_valid = (unzGetFilePos64(zf, &entry.dir_pos) == UNZ_OK);
                // Defensa en profundidad: el límite por entrada debería
                // hacer esto inalcanzable. Si captura, la entrada se trata
                // como fallida.
                try {
                    entry.data.resize(want_bytes);
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

                ZPOS64_T total_read = 0;
                bool read_ok = true;
                while (total_read < want_bytes) {
                    const ZPOS64_T remaining = want_bytes - total_read;
                    // unzReadCurrentFile toma un unsigned de 32 bits: el
                    // resto se pide a trozos. Inalcanzable con
                    // kMaxZipEntryBytes actual, pero el recorte tiene que
                    // ser explícito o un futuro límite mayor truncaría en
                    // silencio.
                    const unsigned int chunk =
                        (remaining > 0xFFFFFFFFull) ? 0xFFFFFFFFu
                                                     : static_cast<unsigned int>(remaining);
                    const int got = unzReadCurrentFile(
                        zf, entry.data.data() + total_read, chunk);
                    if (got <= 0) { read_ok = (got == 0); break; }
                    total_read += static_cast<ZPOS64_T>(got);
                }
                unzCloseCurrentFile(zf);

                if (read_ok && total_read == want_bytes) {
                    if (!entry.lazy) entry.full_size = total_read;
                    // Solo cuenta contra el presupuesto lo que se RETIENE.
                    // Una entrada perezosa ocupa 64 KiB, no sus 26 MB, y
                    // por eso un álbum que antes no cabía ahora entra
                    // entero. Lo que se materialice después vive y muere
                    // con la pista en curso, fuera de esta cuenta.
                    total_reserved += static_cast<std::size_t>(want_bytes);
                    out_entries.push_back(std::move(entry));
                } else if (warn) {
                    warn("[aolib] " + entry_name +
                         ": la descompresión falló tras " +
                         std::to_string(static_cast<unsigned long long>(total_read)) +
                         " de " +
                         std::to_string(static_cast<unsigned long long>(info.uncompressed_size)) +
                         " bytes, omitida.");
                }
            } else if (warn) {
                warn("[aolib] " + entry_name +
                     ": unzOpenCurrentFile() falló (entrada corrupta o "
                     "cifrada), omitida.");
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
