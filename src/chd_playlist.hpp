// chd_playlist.hpp
//
// Convierte un .chd en la MISMA lista de ZipEntry que produce el .zip,
// para que a partir de ahi el resto del core no distinga de que contenedor
// vino cada pista.
//
// Un .chd puede traer musica por dos caminos, y los dos acaban en la misma
// lista:
//
//  1. PISTAS CD-DA. Un CD de juego con audio Red Book (Fantastic Four de
//     PS1: 27 pistas). No se materializan NUNCA -- una pista de cuatro
//     minutos son 42 MB y el disco entero pasa de 500 MB -- sino que
//     CdAudioEngine lee del CHD los sectores que necesita para cada
//     render(). Se marcan con ZipEntry::cd_track.
//  2. FICHEROS DEL SISTEMA DE FICHEROS. Es lo unico que hay en un album
//     guardado con hacer_album_chd.py, y en un CD de juego son los .VAG,
//     .XA y demas sueltos. Entran como entradas PEREZOSAS, igual que en un
//     .zip: prefijo de cabecera al enumerar y el resto bajo demanda.
//     Materializarlas es barato porque en ISO9660 un fichero es un extent
//     CONTIGUO: leerlo es un seek y ya.
//
// La memoria que se retiene es, como mucho, una entrada: el mapa de hunks
// da acceso aleatorio, asi que no hay nada que atravesar para llegar a una
// pista concreta.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cctype>
#include <functional>
#include <string>
#include <vector>

#include "chd_reader.hpp"
#include "iso9660.hpp"
#include "vfs_bridge.hpp"
#include "zip_playlist.hpp"

namespace chd_playlist {

// Tope defensivo por entrada, el mismo espiritu que el de .zip: un extent
// declarado en el directorio de ISO9660 no tiene por que ser cierto.
constexpr uint64_t kMaxEntryBytes = 256ull * 1024 * 1024;

// Centinela de "ninguna entrada" para PrefetchJob.
constexpr std::size_t kNoIndex = std::size_t(-1);

using WarnFn = std::function<void(const std::string&)>;

// Nombre visible de una pista CD-DA. No se puede sacar del disco: el
// CD-Text es opcional, casi ningun rip lo conserva y CHD no lo guarda, asi
// que se numera. La extension no es cosmetica -- el despacho la mira -- y
// por eso lleva una: .cda es lo que Windows le pone a una pista de CD.
inline std::string cd_track_name(int numero) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "Track %02d.cda", numero);
    return std::string(buf);
}

// ¿El nombre ya acaba en ".xa"? Sin distinguir mayusculas: los rips mezclan
// "BGM1.XA" y "bgm1.xa" en el mismo disco.
inline bool ya_es_xa(const std::string& nombre) {
    if (nombre.size() < 3) return false;
    const std::string cola = nombre.substr(nombre.size() - 3);
    return (cola[0] == '.') &&
           std::tolower(static_cast<unsigned char>(cola[1])) == 'x' &&
           std::tolower(static_cast<unsigned char>(cola[2])) == 'a';
}

// Indice de una pista dentro de ChdReader::tracks(). Los ficheros XA se
// leen de la pista de datos, y quien los reproduce necesita el indice, no
// el puntero.
inline std::size_t indice_de_pista(const chd_reader::ChdReader& chd,
                                   const chd_reader::ChdTrack* t) {
    if (!t) return 0;
    const std::vector<chd_reader::ChdTrack>& v = chd.tracks();
    for (std::size_t i = 0; i < v.size(); ++i)
        if (&v[i] == t) return i;
    return 0;
}

// Lee un fichero (o su prefijo) del sistema de ficheros de la imagen.
inline bool read_iso_file(chd_reader::ChdReader& chd,
                          const chd_reader::ChdTrack* pista,
                          uint64_t offset, void* dst, std::size_t len) {
    return pista ? chd.read_fs(*pista, offset, dst, len)
                 : chd.read(offset, dst, len);
}

// Enumera el .chd. Devuelve false si no hay NADA reproducible dentro.
//
// 'chd' se queda abierto y es del llamante: las pistas CD-DA leen de el
// durante toda la reproduccion, y las entradas perezosas al materializarse.
inline bool enumerate(chd_reader::ChdReader& chd,
                      std::vector<ZipEntry>& out,
                      const WarnFn& warn) {
    out.clear();
    if (!chd.is_open()) return false;

    const chd_reader::ChdTrack* fs_track = chd.filesystem_track();
    const uint64_t bytes_fs = fs_track
        ? uint64_t(fs_track->frames) * fs_track->fs_size
        : chd.logical_size();

    // ── 1. el sistema de ficheros ────────────────────────────────────────
    // Va primero para que, en un album, el orden de la lista sea el del
    // directorio y no quede detras de unas pistas de audio que no existen.
    iso9660::Reader iso;
    std::string why;
    auto leer = [&chd, fs_track](uint64_t off, void* dst, std::size_t len) {
        return read_iso_file(chd, fs_track, off, dst, len);
    };

    if (iso.open(leer, why)) {
        std::size_t aceptados = 0, rechazados = 0;
        const std::size_t fs_index = indice_de_pista(chd, fs_track);
        for (const iso9660::IsoFile& f : iso.files()) {
            if (f.size == 0) { ++rechazados; continue; }

            // ── ficheros de audio XA, reconocidos por el DIRECTORIO ──────
            //
            // Un disco de CD-XA marca cada registro de directorio con una
            // extension de 14 bytes que dice como estan grabados sus
            // sectores. Eso decide sin leer ni un sector de datos, que es
            // la diferencia entre enumerar un disco al instante y escanear
            // cientos de MB:
            //
            //   0x4000 CD-DA  el "fichero" es en realidad una pista de
            //                 audio del disco. Se salta: ya se enumera
            //                 abajo como pista CD-DA, y contarlo aqui la
            //                 duplicaria. Doom trae 7 asi y Die Hard
            //                 Trilogy 20.
            //   0x1000 Form 2 / 0x2000 entrelazado
            //                 hay audio XA dentro. Form 2 es la forma de
            //                 sector que usa el audio; "entrelazado" es
            //                 como se marcan los .STR que mezclan video y
            //                 audio. Cualquiera de las dos vale.
            //   0x0800 Form 1 datos normales, se sigue por el camino de
            //                 siempre (extension + contenido).
            //
            // Por que hace falta esto y no basta con la extension: los
            // ficheros de audio de Tarzan se llaman H/SY, H/TA... SIN
            // extension ninguna, y su musica es el 100 % del disco. Y por
            // que no basta con mirar el primer sector: el audio de Dragon
            // Valor esta dentro de MOVIE/MOVIE1.STR, cuyo primer sector es
            // video.
            if (f.has_xa && (f.xa_attr & 0x4000)) { ++rechazados; continue; }

            if (f.has_xa && (f.xa_form2() || f.xa_interleaved()) &&
                fs_track != nullptr) {
                // Los sectores del fichero, en crudo. NO se materializa:
                // BGM1.XA de G. Darius son 42.552 sectores = 100 MB.
                const uint64_t sectores = (uint64_t(f.size) + 2047) / 2048;
                if (sectores == 0 || uint64_t(f.lba) + sectores > fs_track->frames) {
                    warn("[aolib] " + f.path + ": su extent se sale de la "
                         "pista de datos; se omite.");
                    continue;
                }
                ZipEntry e;
                // El reparto elige motor por la extension, y estos ficheros
                // llegan con cualquier cosa: ".XA", ".STR", ".XAS" o NADA
                // (los de Tarzan se llaman H/SY, H/TA...). Se le pone ".xa"
                // para que el camino sea siempre el mismo, pero sin
                // duplicarla en los que ya la traen.
                e.name        = ya_es_xa(f.path) ? f.path : f.path + ".xa";
                e.full_size   = sectores * chd_reader::kCdSectorSize;
                e.cd_data_vfs = int(fs_index);
                e.xa_first_sector = f.lba;
                e.xa_sectors      = uint32_t(sectores);
                e.from_chd    = true;
                e.lazy        = false;   // streaming real, nada que inflar
                // OJO: aqui NO se pone probe_failed. Lo llevo desde 1.3.0,
                // cuando sondear la duracion obligaba a xa.c a recorrer el
                // fichero entero. Desde 1.3.2 la mide XaProbeJob por trozos,
                // y advance_duration_probe() comprueba esa bandera ANTES de
                // llegar al medidor: dejarla puesta deja el medidor MUERTO y
                // la lista entera en "--:--". Solo la pista de datos completa
                // (xa_sectors == 0) sigue siendo insondeable.
                out.push_back(std::move(e));
                ++aceptados;
                continue;
            }

            if (!zip_entry_extension_supported(f.name)) { ++rechazados; continue; }
            if (f.size > kMaxEntryBytes) {
                warn("[aolib] " + f.path + ": " + std::to_string(f.size) +
                     " bytes, por encima del tope por entrada; se omite.");
                continue;
            }
            const uint64_t fin = uint64_t(f.lba) * iso9660::kSectorSize + f.size;
            if (fin > bytes_fs) {
                warn("[aolib] " + f.path + ": su extent se sale de la imagen; "
                     "se omite.");
                continue;
            }

            ZipEntry e;
            e.name      = f.path;
            e.full_size = f.size;
            e.iso_lba   = f.lba;
            e.from_chd  = true;
            // Perezosa con el mismo criterio que en .zip: los formatos de
            // streaming se abren con la cabecera y el resto se lee cuando
            // haga falta. Lo demas entra entero, que es lo que esperan
            // libgme, libvgm, libxmp y aosdk.
            e.lazy = zip_entry_is_lazy(f.name) && f.size > kZipHeaderPrefix;
            const std::size_t leer_ahora =
                e.lazy ? kZipHeaderPrefix : std::size_t(f.size);
            e.data.resize(leer_ahora);
            if (!leer(uint64_t(f.lba) * iso9660::kSectorSize, e.data.data(),
                      leer_ahora)) {
                warn("[aolib] " + f.path + ": no se pudo leer de la imagen; "
                     "se omite.");
                continue;
            }
            out.push_back(std::move(e));
            ++aceptados;
        }
        if (aceptados == 0 && rechazados > 0) {
            warn("[aolib] La imagen tiene " + std::to_string(rechazados) +
                 " ficheros pero ninguno con una extension soportada.");
        }
    } else if (!chd.is_cd()) {
        // Un CHD crudo SIN sistema de ficheros no tiene por donde
        // entrarle: no hay pistas ni directorio.
        warn("[aolib] " + why);
        return false;
    }

    // ── 2. las pistas de CD-DA ───────────────────────────────────────────
    const std::vector<chd_reader::ChdTrack>& pistas = chd.tracks();
    for (std::size_t i = 0; i < pistas.size(); ++i) {
        const chd_reader::ChdTrack& t = pistas[i];
        if (!t.is_audio || t.frames == 0) continue;

        ZipEntry e;
        e.name      = cd_track_name(t.number);
        e.full_size = t.cooked_bytes();
        e.cd_track  = int(i);
        e.from_chd  = true;
        e.lazy      = false;   // no se materializa nunca: se lee al vuelo
        // La duracion es aritmetica pura, no hace falta abrir nada: un
        // sector de CD-DA son 2352 bytes = 588 frames a 44.100 Hz.
        e.cached_length_frames =
            uint64_t(t.frames) * (chd_reader::kCdSectorSize / 4);
        out.push_back(std::move(e));
    }

    // ── 3. la pista de DATOS, si puede llevar XA entrelazado ─────────────
    // MODE2_RAW/MODE2_FORM_MIX son las unicas formas de sector que dejan
    // sitio a Form 2 (el que usa CD-XA para audio); un disco MODE1 puro
    // (ISO9660 sin extension XA) nunca tiene audio embebido y escanearlo
    // solo gastaria tiempo para encontrar 0 subsongs. xa.c (deps/vgmstream)
    // hace el resto: recorre los sectores el solo y separa cada file+
    // channel en un subsong.
    //
    // Solo como ULTIMO RECURSO. Si el directorio ya declaro ficheros XA,
    // esto seria el mismo audio otra vez, entero y en una sola entrada: el
    // disco aparecerian dos veces. Se queda para los rips cuyo directorio
    // no marca nada (o cuyo audio no esta en ningun fichero listado).
    bool hay_ficheros_xa = false;
    for (const ZipEntry& e : out)
        if (e.cd_data_vfs >= 0 && e.xa_sectors > 0) { hay_ficheros_xa = true; break; }

    for (std::size_t i = 0; !hay_ficheros_xa && i < pistas.size(); ++i) {
        const chd_reader::ChdTrack& t = pistas[i];
        if (t.is_audio || t.frames == 0) continue;
        if (t.type != "MODE2_RAW" && t.type != "MODE2_FORM_MIX") continue;

        ZipEntry e;
        e.name        = "Track " + std::to_string(t.number) + " (datos).xa";
        e.full_size   = uint64_t(t.frames) * chd_reader::kCdSectorSize;
        e.cd_data_vfs = int(i);
        e.from_chd    = true;
        e.lazy        = false;   // el streaming es real, no hay nada que inflar
        // Sondear esto en segundo plano significaria que xa.c escanee la
        // pista ENTERA (cientos de MB) dentro de un frame de audio: se deja
        // sin sondear a proposito y se excluye del total del album. Ver el
        // comentario de ZipEntry::probe_failed.
        e.probe_failed = true;
        out.push_back(std::move(e));
    }

    return !out.empty();
}

// ---------------------------------------------------------------------------
// Lectura anticipada
// ---------------------------------------------------------------------------
//
// El mismo reparto por frames que hace advance_zip_prefetch() con minizip,
// y por el mismo motivo medido: leer una entrada entera dentro del cambio
// de pista mete el paron dentro del audio. Un .omu de 34 MB costaba un
// frame de 157 ms.
//
// Aqui es MUCHO mas simple que en .zip, porque no hay que reanudar ningun
// inflate: en ISO9660 el fichero es un extent contiguo, asi que basta con
// recordar por que byte se iba. Y por eso tampoco hace falta abortar nada
// si el usuario salta a otra pista: se tira el buffer y ya.
struct PrefetchJob {
    std::size_t          index = kNoIndex;
    std::vector<uint8_t> buf;
    uint64_t             done  = 0;
    uint64_t             total = 0;

    bool active() const { return index != kNoIndex; }
    bool complete() const { return active() && done >= total; }
    void reset() { index = kNoIndex; buf.clear(); buf.shrink_to_fit(); done = 0; total = 0; }
};

// Arranca (o continua) la lectura de 'entry'. Devuelve false si no se puede.
inline bool prefetch_begin(PrefetchJob& job, std::size_t index,
                           const ZipEntry& entry) {
    if (!entry.from_chd || entry.cd_track >= 0) return false;
    if (entry.full_size == 0 || entry.full_size > kMaxEntryBytes) return false;
    job.reset();
    job.index = index;
    job.total = entry.full_size;
    // reserve(), no assign(): pedir la memoria sin tocar las paginas. Poner
    // 34 MB a cero es ~20 ms en un solo frame, y ese fue el pico mas alto
    // de retro_run() en el camino equivalente del .zip.
    job.buf.reserve(std::size_t(entry.full_size));
    return true;
}

// Lee hasta 'max_bytes' mas. Devuelve false si la lectura falla.
inline bool prefetch_step(chd_reader::ChdReader& chd, PrefetchJob& job,
                          const ZipEntry& entry, std::size_t max_bytes) {
    if (!job.active() || job.complete()) return true;
    const std::size_t quiere =
        std::size_t(std::min<uint64_t>(max_bytes, job.total - job.done));
    const std::size_t antes = job.buf.size();
    job.buf.resize(antes + quiere);

    const chd_reader::ChdTrack* fs_track = chd.filesystem_track();
    if (!read_iso_file(chd, fs_track,
                       uint64_t(entry.iso_lba) * iso9660::kSectorSize + job.done,
                       job.buf.data() + antes, quiere)) {
        job.reset();
        return false;
    }
    job.done += quiere;
    return true;
}

// ---------------------------------------------------------------------------
// Duracion de una pista de audio XA, medida por trozos
// ---------------------------------------------------------------------------
//
// La duracion de un flujo XA sale del NUMERO DE SECTORES que son de su
// canal: es un formato de bitrate constante, cada sector de audio rinde
// siempre las mismas muestras. Contarlos de verdad obliga a recorrer el
// fichero entero -- 2,49 s medidos en uno de 100 MB, descomprimiendo hunks
// de LZMA a 38 MB/s -- y eso es justo lo que se saco del camino de carga en
// 1.3.1.
//
// Aqui se mide igual que en el modo rapido de deps/vgmstream/meta/xa.c, con
// dos medidas ACOTADAS, pero TROCEADO: aquella es una funcion C bloqueante
// de vgmstream y esta tiene que ceder el frame cada pocos sectores para no
// cortar el audio que ya esta sonando.
//
// Por que no vale una regla de tres sacada de una pista y aplicada al resto:
// la constante de proporcionalidad no es "bytes del fichero" sino "bytes de
// los sectores DE ESE CANAL", y esa fraccion cambia una barbaridad entre
// ficheros del mismo disco. Medido: aplicando la de BGM1.XA a BGM2.XA (G.
// Darius, misma carpeta) salen 38 s donde la pista dura 266. De los 112
// ficheros XA de la biblioteca, 28 son contenedores con varias canciones.
struct XaProbeJob {
    enum class Fase { Inactivo, Buscando, Muestreando, Acotando, Listo };

    std::size_t index  = kNoIndex;
    Fase        fase   = Fase::Inactivo;

    uint32_t    first_sector = 0;   // LBA del fichero dentro de la pista
    uint32_t    sectors      = 0;   // tamano del fichero en sectores

    uint32_t    cursor  = 0;        // sector relativo por el que va
    uint32_t    inicio  = 0;        // primer sector de audio (relativo)
    uint16_t    target  = 0;        // file+channel del canal objetivo
    uint32_t    vistos  = 0;
    uint32_t    propios = 0;
    bool        exacto  = false;    // se vio la marca de fin de canal

    uint32_t    lo = 0, hi = 0;     // busqueda binaria de la extension
    uint32_t    muestras_por_sector = 0;
    uint32_t    tasa_nativa = 0;

    uint64_t    frames = 0;         // resultado

    bool active()   const { return index != kNoIndex; }
    bool complete() const { return fase == Fase::Listo; }
    void reset() { *this = XaProbeJob(); }
};

// Topes, los mismos que valida xa_fast_scan() en xa.c.
constexpr uint32_t kXaProbeMaxFind  = 8192;   // buscando el primer audio
constexpr uint32_t kXaProbeMaxRatio = 2048;   // muestra para la proporcion
constexpr uint32_t kXaProbeWindow   = 64;     // ventana de la busqueda binaria

// Un sector de audio, con la misma prueba que usan xa.c y blocked_xa.c.
inline bool xa_sector_es_audio(uint8_t submode) {
    return !(submode & 0x08) && (submode & 0x04) && !(submode & 0x02);
}

// Muestras que rinde UN sector de audio, del byte de coding: igual que
// block_update_xa(). Solo Form 2, que es la forma que usa el audio.
inline uint32_t xa_muestras_por_sector(uint8_t coding) {
    const int canales   = ((coding >> 0) & 3) == 1 ? 2 : 1;
    const int subframes = ((coding >> 4) & 3) == 1 ? 4 : 8;   // 8 bits : 4 bits
    return uint32_t((28 * subframes / canales) * 18);
}

// Tasa NATIVA del flujo, del mismo byte: bits 2-3, 0 = 37800, 1 = 18900.
// Hace falta porque las muestras de arriba estan a esa tasa y la lista
// cuenta en frames de 44100 (ver ui_screen: len / 44100).
inline uint32_t xa_tasa_nativa(uint8_t coding) {
    return ((coding >> 2) & 3) == 1 ? 18900u : 37800u;
}

// Muestras a la tasa nativa -> frames a 44100, que es lo que espera la UI.
inline uint64_t xa_a_frames_44100(uint64_t muestras, uint32_t tasa) {
    if (tasa == 0) return muestras;
    return muestras * 44100ull / tasa;
}

inline bool xa_probe_begin(XaProbeJob& job, std::size_t index,
                           const ZipEntry& entry) {
    if (entry.cd_data_vfs < 0 || entry.xa_sectors == 0) return false;
    job.reset();
    job.index        = index;
    job.fase         = XaProbeJob::Fase::Buscando;
    job.first_sector = entry.xa_first_sector;
    job.sectors      = entry.xa_sectors;
    return true;
}

// Avanza como mucho 'max_sectors'. Devuelve false si la lectura falla.
inline bool xa_probe_step(chd_reader::ChdReader& chd, XaProbeJob& job,
                          uint32_t max_sectors) {
    if (!job.active() || job.complete()) return true;
    const chd_reader::ChdTrack* fs = chd.filesystem_track();
    if (!fs) { job.reset(); return false; }
    const chd_reader::ChdTrack& t = *fs;

    uint8_t sec[chd_reader::kCdSectorSize];
    auto lee = [&](uint32_t rel, uint16_t& cfg, uint8_t& sub, uint8_t& cod) {
        if (rel >= job.sectors) return false;
        if (!chd.read_track(t, job.first_sector + rel, 1, sec)) return false;
        cfg = uint16_t((uint16_t(sec[0x10]) << 8) | sec[0x11]);
        sub = sec[0x12];
        cod = sec[0x13];
        return true;
    };

    for (uint32_t n = 0; n < max_sectors; ++n) {
        uint16_t cfg = 0; uint8_t sub = 0, cod = 0;

        if (job.fase == XaProbeJob::Fase::Buscando) {
            if (job.cursor >= job.sectors || job.cursor >= kXaProbeMaxFind) {
                job.reset(); return true;      // no hay audio: no es una pista
            }
            if (!lee(job.cursor, cfg, sub, cod)) { job.reset(); return false; }
            if (xa_sector_es_audio(sub) && (cfg & 0xFF) != 0xFF) {
                job.target = cfg;
                job.inicio = job.cursor;
                job.muestras_por_sector = xa_muestras_por_sector(cod);
                job.tasa_nativa         = xa_tasa_nativa(cod);
                job.fase   = XaProbeJob::Fase::Muestreando;
                job.cursor = job.inicio;
            } else {
                ++job.cursor;
            }
            continue;
        }

        if (job.fase == XaProbeJob::Fase::Muestreando) {
            const uint32_t restantes = job.sectors - job.inicio;
            if (job.vistos >= kXaProbeMaxRatio || job.vistos >= restantes ||
                job.exacto) {
                // La muestra cubrio el fichero entero, o aparecio la marca
                // de fin: la cuenta es exacta y no hay nada que acotar.
                if (job.exacto || job.vistos >= restantes) {
                    job.frames = xa_a_frames_44100(
                        uint64_t(job.propios) * job.muestras_por_sector,
                        job.tasa_nativa);
                    job.fase = XaProbeJob::Fase::Listo;
                } else {
                    job.lo = 0; job.hi = restantes;
                    job.fase = XaProbeJob::Fase::Acotando;
                }
                continue;
            }
            if (!lee(job.cursor, cfg, sub, cod)) { job.reset(); return false; }
            ++job.vistos;
            if (xa_sector_es_audio(sub) && cfg == job.target) {
                ++job.propios;
                if (sub & 0x80) job.exacto = true;   // fin de este canal
            }
            ++job.cursor;
            continue;
        }

        if (job.fase == XaProbeJob::Fase::Acotando) {
            // Hasta donde llega el canal. Sin esto la proporcion se
            // extrapola al fichero ENTERO y un contenedor con varias
            // canciones multiplica la duracion por su numero de canciones.
            // Se mira una VENTANA y no un sector suelto porque los canales
            // van entrelazados: un sector concreto puede no ser suyo aun
            // estando dentro de su tramo.
            if (job.hi - job.lo <= kXaProbeWindow) {
                const uint32_t tramo = job.hi > 0 ? job.hi
                                                  : (job.sectors - job.inicio);
                const uint32_t sectores =
                    job.vistos >= tramo
                        ? job.propios
                        : uint32_t(uint64_t(job.propios) * tramo / job.vistos);
                job.frames = xa_a_frames_44100(
                    uint64_t(sectores) * job.muestras_por_sector,
                    job.tasa_nativa);
                job.fase = XaProbeJob::Fase::Listo;
                continue;
            }
            {
                const uint32_t mid = job.lo + (job.hi - job.lo) / 2;
                const uint32_t cuantos =
                    std::min<uint32_t>(kXaProbeWindow, job.hi - mid);
                bool visto_aqui = false;
                for (uint32_t k = 0; k < cuantos; ++k) {
                    if (!lee(job.inicio + mid + k, cfg, sub, cod)) break;
                    if (xa_sector_es_audio(sub) && cfg == job.target) {
                        visto_aqui = true; break;
                    }
                }
                if (visto_aqui) job.lo = mid; else job.hi = mid;
            }
            n += kXaProbeWindow;   // el paso ha costado una ventana entera
            continue;
        }

        break;
    }
    return true;
}

// Entrega el buffer a la entrada. Solo si esta ENTERO: nadie puede leer
// media entrada, el mismo invariante que en .zip.
inline bool prefetch_commit(PrefetchJob& job, ZipEntry& entry) {
    if (!job.complete() || job.buf.size() != entry.full_size) return false;
    entry.data = std::move(job.buf);
    job.reset();
    return true;
}


// Materializa una entrada perezosa que vino de un .chd. Barato: el fichero
// es un extent contiguo, asi que es un seek y una lectura seguida.
inline bool materialize(chd_reader::ChdReader& chd, ZipEntry& entry) {
    if (entry.complete()) return true;
    if (!entry.from_chd || entry.cd_track >= 0) return false;
    if (entry.full_size > kMaxEntryBytes) return false;

    const chd_reader::ChdTrack* fs_track = chd.filesystem_track();
    std::vector<uint8_t> completo(std::size_t(entry.full_size));
    if (!read_iso_file(chd, fs_track,
                       uint64_t(entry.iso_lba) * iso9660::kSectorSize,
                       completo.data(), completo.size())) {
        return false;
    }
    entry.data = std::move(completo);
    return true;
}

}  // namespace chd_playlist
