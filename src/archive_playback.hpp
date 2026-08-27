// archive_playback.hpp — reproducir un .zip/.chd: qué entrada suena, cuál
// se infla por adelantado y de dónde salen las duraciones de la lista.
//
// Aquí viven las tres cosas que reparten trabajo entre frames
// (advance_zip_prefetch, advance_duration_probe y el sondeo reanudable) y
// load_zip_entry_from(), que es la vía CENTRAL de cambio de pista.
//
// Salió de libretro.cpp al partirlo: ver la cabecera de ese fichero.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core_globals.hpp"
#include "engine_dispatch.hpp"
#include "zip_playlist.hpp"
#ifdef AOLIB_WITH_CHD
#include "chd_playlist.hpp"
#include "engine/cd_audio_engine.hpp"
#endif

namespace aolib {

// Definida en ui_glue.hpp, que incluye este fichero (nunca al revés): la
// declaración adelantada evita darle la vuelta al orden de inclusión solo
// para que publish_probed_duration() pueda reconstruir la lista visible
// cuando una entrada deja de ser una pista (ver más abajo).
static void rebuild_ui_model();

// Calcula duración y título de CADA entrada reproducible del .zip, no solo
// de la que va a sonar, abriendo y descartando un motor por entrada. Sin
// esto la lista mostraría "--:--" en todo lo que no fuera la pista activa.
//
// ORDEN OBLIGATORIO: solo puede llamarse UNA vez, para todo el .zip, y
// ANTES de que exista ningún motor PSF/PSF2/SSF real; es decir, entre
// enumerate_zip() y la primera load_zip_entry_from().
//
// El motivo es que corlett_decode(), que psf_start/psf2_start/ssf_start
// invocan para leer los tags, termina llamando siempre a
// corlett_length_set(), y este escribe total_samples/decaybegin/decayend,
// que son estáticas de corlett.c: estado de PROCESO compartido por los tres
// motores, y justo lo que corlett_sample_fade() consulta en cada muestra
// para decidir el fade y el final de la pista. Asomarse a una entrada
// mientras otra suena de verdad le corrompe el contador de fade a media
// reproducción. Hecho antes, no pasa nada: el psf_start() del motor real
// reescribe esos valores con los suyos.
//
// libvgm y libgme no tienen este problema, no comparten estado de proceso
// equivalente. Se usa el mismo construct_engine_for() que la carga real
// para que la duración de la lista no pueda divergir de la que se verá al
// activar la pista.
//
// Coste: una apertura por entrada (decodificación de cabecera y resolución
// de "_lib", sin ejecutar una sola instrucción de CPU emulada), una vez y
// de forma síncrona dentro de retro_load_game().
// Aviso de archivo compartido entre la enumeración y la materialización
// bajo demanda: las dos hablan de la misma entrada y deben sonar igual en
// el log.
static void archive_warn_log(const std::string& msg) {
    log_message(RETRO_LOG_WARN, "%s", msg.c_str());
}

// True si el motor abrió pero necesitó leer más allá del prefijo. Solo
// VgmstreamEngine puede darlo: es el único que recibe entradas parciales.
// Un dynamic_cast aquí es barato -- ocurre una vez por entrada al cargar
// el álbum, no por frame -- y evita ensuciar IAudioEngine con un método
// que ningún otro motor puede responder.
static bool engine_needs_full_data(const IAudioEngine* engine) {
#ifdef AOLIB_WITH_VGMSTREAM
    if (!engine) return true;   // no abrió: puede que le faltara cabecera
    if (const auto* v = dynamic_cast<const VgmstreamEngine*>(engine))
        return v->needs_full_data();
    return false;
#else
    (void)engine;
    return false;
#endif
}

// Materializa una entrada perezosa.
//
// Los dos contenedores la producen y cada uno la recupera a su manera:
// .zip volviendo a inflar por minizip, .chd con un seek sobre el extent
// contiguo del fichero dentro de la imagen ISO9660.
static bool materialize_entry(std::size_t index) {
    ZipEntry& entry = g_ctx->zip_entries[index];
    if (entry.complete()) return true;
#ifdef AOLIB_WITH_CHD
    if (entry.from_chd) {
        // Barato: en ISO9660 un fichero es un extent CONTIGUO, asi que
        // esto es un seek y una lectura seguida.
        if (chd_playlist::materialize(g_ctx->chd, entry)) return true;
        log_message(RETRO_LOG_ERROR,
            "[aolib] %s: no se pudo leer la entrada de la imagen del .chd.",
            entry.name.c_str());
        return false;
    }
#endif  // AOLIB_WITH_CHD
    return materialize_zip_entry(g_ctx->archive_path, *g_vfs, entry, archive_warn_log);
}

// ¿Esta entrada la abriría un motor de aosdk? Son los únicos que NO se
// pueden sondear mientras suena otra cosa: corlett_decode() escribe
// total_samples/decaybegin/decayend, estáticas de proceso de corlett.c que
// corlett_sample_fade() consulta por muestra, así que asomarse a una entrada
// PSF mientras suena otra corrompe el contador de fade a media reproducción.
//
// Las extensiones son las mismas que mira construct_engine_for(); se repiten
// aquí a propósito y no se factorizan con aquéllas porque allí el reparto
// decide QUÉ motor construir y aquí decide CUÁNDO se puede sondear: son dos
// preguntas distintas que hoy dan la misma respuesta.
static bool entry_routes_to_aosdk(const std::string& name) {
    return has_suffix(name, ".psf")   || has_suffix(name, ".minipsf")  ||
           has_suffix(name, ".psf2")  || has_suffix(name, ".minipsf2") ||
           has_suffix(name, ".ssf")   || has_suffix(name, ".minissf");
}

enum class ProbeResult { Done, Failed, NeedsFull };

// Suelta una entrada DESPUES de sondearla: se vuelve a materializar
// cuando toque, y no soltarla haria que el sondeo acumulara el album
// entero -- justo lo que la materializacion perezosa evita.
//
// En .zip se vuelve a inflar y en .chd es un seek sobre un extent
// contiguo, asi que en los dos casos recuperarla es barato.
static void release_entry_after_probe(ZipEntry& entry) {
    release_zip_entry(entry);
}

// Guarda la duración y el título de un motor recién abierto en su entrada.
static void cache_metadata_into(ZipEntry& entry, const IAudioEngine& engine) {
    const auto& m = engine.metadata();
    // Duración REAL: incluye el fade y, en VGM, los bucles que se van a
    // reproducir (ver TrackMetadata::playback_frames()).
    entry.cached_length_frames = m.playback_frames();
    if (!m.title.empty()) entry.cached_title = m.title;
}

// Sondea UNA entrada con lo que ya tiene en RAM. Para todos los formatos
// medidos salvo XA y EA SCHl, el prefijo de 64 KiB basta para dar formato y
// duración. Si el parser pide más allá del prefijo devuelve NeedsFull y NO
// materializa: inflar 16 MB de una tacada aquí metía el parón dentro del
// audio, que es peor que el problema que se venía a arreglar.
//
// El motor temporal muere al salir, liberando el guard de aosdk ANTES de que
// la siguiente llamada construya el suyo: la misma secuencia
// destruir-antes-de-construir que exige load_zip_entry_from().
static ProbeResult probe_zip_entry_from_prefix(ZipEntry& entry) {
    // Nada de lo que escriba el motor temporal sobrevive a esta función:
    // ver AosdkFadeScope en aosdk_bridge.hpp.
    AosdkFadeScope fade_scope;
    bool open_wants_full = false;
    auto tmp = construct_engine_for(
        nullptr, entry.name, entry.data.data(), entry.data.size(),
        g_ctx->options.default_fade_seconds, g_ctx->options.loop_infinite,
        g_ctx->options.xmp_stereo_separation, &g_ctx->zip_entries,
        static_cast<std::size_t>(entry.full_size), &open_wants_full);

    if (entry.lazy && !entry.complete() &&
        (open_wants_full || engine_needs_full_data(tmp.get()))) {
        tmp.reset();
        return ProbeResult::NeedsFull;
    }
    // El release NO puede saltarse en el camino de fallo, o la entrada se
    // quedaría inflada para el resto de la sesión y el sondeo acumularía el
    // álbum -- justo lo que la materialización perezosa evita.
    if (!tmp) { release_entry_after_probe(entry); return ProbeResult::Failed; }

    cache_metadata_into(entry, *tmp);
    tmp.reset();
    release_entry_after_probe(entry);
    return ProbeResult::Done;
}

// Segunda fase: la entrada ya está inflada entera. Se abre, se cachea y se
// suelta inmediatamente.
static bool probe_zip_entry_with_full_data(ZipEntry& entry) {
    AosdkFadeScope fade_scope;
    auto tmp = construct_engine_for(
        nullptr, entry.name, entry.data.data(), entry.data.size(),
        g_ctx->options.default_fade_seconds, g_ctx->options.loop_infinite,
        g_ctx->options.xmp_stereo_separation, &g_ctx->zip_entries);
    const bool ok = static_cast<bool>(tmp);
    if (ok) cache_metadata_into(entry, *tmp);
    tmp.reset();
    release_entry_after_probe(entry);
    return ok;
}

// Camino EAGER (solo aosdk): estas entradas nunca son perezosas, así que
// NeedsFull no puede darse y no hace falta nada reanudable.
static bool probe_zip_entry_duration(ZipEntry& entry) {
    return probe_zip_entry_from_prefix(entry) == ProbeResult::Done;
}

// Barrido EAGER, en retro_load_game, y SOLO de las entradas de aosdk.
//
// Antes barría el álbum entero, y eso era el 100 % del tiempo de
// retro_load_game: 4,67 s en un álbum de 424 entradas .asf, del que 363 se
// abrían DOS veces (el intento con prefijo tenía éxito y luego
// needs_full_data() obligaba a inflar la entrada entera y reabrir). Todo ese
// trabajo alimenta dos campos que solo lee rebuild_ui_model() para pintar la
// columna de duraciones: el audio no depende de ellos, y la UI ya sabe
// dibujar "--:--" cuando valen 0.
//
// Lo que NO se puede diferir es aosdk: su sondeo pisa las estáticas de
// corlett.c (ver entry_routes_to_aosdk), así que tiene que ocurrir antes de
// que exista ningún motor. Cuesta ≈0 porque esas entradas no son perezosas,
// ya están enteras en RAM y no hay inflate que pagar.
static void precompute_zip_track_durations() {
    if (!g_ctx) return;
    for (auto& entry : g_ctx->zip_entries) {
        if (!zip_entry_is_playable(entry)) continue;
        if (!entry_routes_to_aosdk(entry.name)) continue;
        probe_zip_entry_duration(entry);
    }
}


// Presupuesto de inflado por frame. 4 ms de los 16.667: el trabajo del core
// medido es ~0,5 ms por frame, así que queda holgura de sobra, y con esto una
// entrada de 16 MB (97 ms de inflate) está lista en ~24 frames = 0,4 s. Las
// pistas duran minutos: sobra tiempo.
constexpr double   kPrefetchBudgetUs   = 4000.0;
constexpr uint64_t kPrefetchChunkBytes = 256 * 1024;

// Tope de tamaño para el prefetch. Mientras dura, la entrada siguiente y la
// que suena están en RAM a la vez, así que el pico se dobla. Las entradas de
// streaming medidas no pasan de 26 MB; por encima de esto se renuncia al
// prefetch y se paga el parón, que es el comportamiento de siempre.
constexpr uint64_t kPrefetchMaxEntryBytes = 64ull * 1024 * 1024;

// Cuanto lee de golpe la lectura anticipada de un .chd antes de mirar el
// reloj. Un hunk tipico son 16-19 KiB, asi que 256 KiB son entre 13 y 16
// hunks: bastante trabajo por comprobacion de reloj, y lo bastante poco
// como para no pasarse del presupuesto de 4 ms por un hunk lento.
constexpr std::size_t kChdPrefetchChunk = 256 * 1024;

// Presupuesto del sondeo incremental de duraciones. Más apretado que el del
// prefetch porque esto es puramente cosmético: llenar la columna de
// duraciones no puede competir con tener lista la pista siguiente.
constexpr double kDurationProbeBudgetUs = 2000.0;

#ifdef AOLIB_WITH_CHD
// Sectores por frame del medidor de duracion de XA. Un sector cuesta lo
// que cueste descomprimir su hunk (2.448 bytes de 19.584, o sea uno de
// cada ocho paga la descompresion), y a 38 MB/s medidos eso son unos 64 us
// por hunk. 96 sectores = 12 hunks = ~0,8 ms, dentro del presupuesto de 2
// ms y dejando margen para el resto del frame.
constexpr uint32_t kXaProbeSectorsPerFrame = 96;
#endif

// Sondea las duraciones que faltan, un poco por frame.
//
// Esto era un barrido eager del álbum entero dentro de retro_load_game y era
// el 100 % de su coste. Aquí no hay prisa: la lista se rellena sola en los
// primeros segundos de reproducción y el usuario ve "--:--" mientras tanto,
// exactamente igual que ya veía en una entrada ilegible.
//
// NO toca las entradas de aosdk: ésas ya las sondeó precompute_zip_track_
// durations() antes de que existiera ningún motor, porque sondearlas con algo
// sonando corrompe las estáticas de corlett.c. Tampoco toca la entrada que
// suena: su duración ya la puso load_zip_entry_from() por la vía normal.
// Refleja en la lista visible la duración recién sondeada de la entrada i.
// Se actualiza la fila EN SITIO en vez de reconstruir el modelo entero, que
// recorrería las 424 entradas por cada duración que llega -- salvo cuando
// la entrada resulta irresoluble, que ahí sí hace falta el modelo entero
// para poder QUITAR la fila (ver dentro).
static void publish_probed_duration(std::size_t i, const ZipEntry& entry) {
    if (entry.probe_failed) {
        // Nunca va a sonar: no es una pista del álbum (ver
        // ZipEntry::probe_failed), así que se quita de la lista ENTERA, no
        // solo del total. Un parche en sitio no puede borrar una fila;
        // rebuild_ui_model() sí, y es barato -- solo relee campos ya
        // calculados de zip_entries, no abre ningún motor -- así que
        // llamarlo aquí, una vez por entrada que resulta vacía, no compite
        // con el presupuesto del frame. De paso recalcula current_track y
        // el cursor si la fila que desaparece iba antes de cualquiera de
        // los dos.
        rebuild_ui_model();
        return;
    }
    for (std::size_t v = 0; v < g_ui.playable_to_zip.size(); ++v) {
        if (g_ui.playable_to_zip[v] != i) continue;
        if (v < g_ui.tracks.size()) {
            g_ui.tracks[v].length_frames = entry.cached_length_frames;
            if (!entry.cached_title.empty()) g_ui.tracks[v].label = entry.cached_title;
        }
        return;
    }
}

static void advance_duration_probe() {
    if (!g_ctx || g_ctx->zip_entries.empty()) return;
    // El prefetch manda: tener lista la pista siguiente importa más que
    // rellenar una columna, y así solo hay UNA entrada extra inflándose a la
    // vez, que es lo que acota el pico de memoria.
    if (g_ctx->prefetch.active()) return;

    const double t0 = now_us();

    // Fase 2: hay una entrada inflándose/leyéndose SOLO para sondearla. Se
    // adelanta un trozo y, al completarse, se abre, se cachea y se suelta.
    // Dos trabajos posibles y NUNCA los dos a la vez: ZipInflateJob (minizip,
    // .zip) o PrefetchJob (chd_playlist, .chd) -- un .chd no es un .zip y
    // zip_inflate_begin() falla en silencio si se le da uno, dejando la
    // entrada en "--:--" para siempre. Ver chd_probe_job en core_context.hpp.
#ifdef AOLIB_WITH_CHD
    if (g_ctx->chd_probe_job.active()) {
        const std::size_t i = g_ctx->chd_probe_job.index;
        ZipEntry& entry = g_ctx->zip_entries[i];
        while (!g_ctx->chd_probe_job.complete()) {
            if (!chd_playlist::prefetch_step(g_ctx->chd, g_ctx->chd_probe_job, entry,
                                             kChdPrefetchChunk)) {
                g_ctx->chd_probe_job.reset();
                ++g_ctx->duration_probe_index;
                return;
            }
            if (now_us() - t0 >= kDurationProbeBudgetUs) return;
        }
        if (now_us() - t0 >= kDurationProbeBudgetUs) return;
        chd_playlist::prefetch_commit(g_ctx->chd_probe_job, entry);
        if (probe_zip_entry_with_full_data(entry)) publish_probed_duration(i, entry);
        else { entry.probe_failed = true; publish_probed_duration(i, entry); }
        ++g_ctx->duration_probe_index;
        return;
    }
#endif
    if (g_ctx->probe_job.active()) {
        const std::size_t i = g_ctx->probe_job_index;
        ZipEntry& entry = g_ctx->zip_entries[i];
        while (!g_ctx->probe_job.complete()) {
            if (!zip_inflate_step(g_ctx->probe_job, kPrefetchChunkBytes)) {
                zip_inflate_abort(g_ctx->probe_job);
                g_ctx->probe_job_index = CoreContext::kNoPrefetch;
                ++g_ctx->duration_probe_index;
                return;
            }
            if (now_us() - t0 >= kDurationProbeBudgetUs) return;   // sigue el frame que viene
        }
        // El trabajo ya está inflado, pero abrirlo, cachear y soltarlo NO es
        // gratis. Apilarlo sobre un presupuesto ya gastado daba un frame de
        // 22 ms (medido); si no queda margen se hace en el frame siguiente.
        if (now_us() - t0 >= kDurationProbeBudgetUs) return;
        zip_inflate_commit(entry, g_ctx->probe_job);
        g_ctx->probe_job_index = CoreContext::kNoPrefetch;
        if (probe_zip_entry_with_full_data(entry)) publish_probed_duration(i, entry);
        else { entry.probe_failed = true; publish_probed_duration(i, entry); }
        ++g_ctx->duration_probe_index;
        return;
    }

    if (g_ctx->duration_probe_index >= g_ctx->zip_entries.size()) return;

    // Fase 1: barrer entradas con lo que ya hay en RAM, hasta agotar el
    // presupuesto o topar con una que pida el fichero entero.
    while (g_ctx->duration_probe_index < g_ctx->zip_entries.size()) {
        const std::size_t i = g_ctx->duration_probe_index;
        ZipEntry& entry = g_ctx->zip_entries[i];

        // 'probe_failed' cubre dos casos y los dos significan lo mismo aqui:
        // no hay nada que sondear. O ya se intento con el fichero entero y
        // ningun motor lo abrio, o es la pista de datos completa presentada
        // como .xa, que se deja sin sondear a proposito -- hacerlo obligaria
        // a xa.c a escanear cientos de MB dentro de un frame de audio, y
        // ademas el sondeo la abriria con el VFS equivocado (data/size
        // vacios en vez de su chd_data_vfs), asi que fallaria siempre.
        const bool skip = !zip_entry_is_playable(entry) ||
                          entry_routes_to_aosdk(entry.name) ||
                          entry.cached_length_frames != 0 ||
                          entry.probe_failed ||
                          i == g_ctx->zip_entry_index;
        if (skip) { ++g_ctx->duration_probe_index; continue; }

#ifdef AOLIB_WITH_CHD
        // Audio XA de un CD: no se sondea abriendo un motor, se MIDE. Un
        // fichero XA no se materializa (100 MB los hay) y su duracion sale
        // de cuantos sectores son de su canal, que es una cuenta sobre las
        // subcabeceras. Va por trozos porque hacerla de una vez son cientos
        // de ms dentro de un frame de audio.
        if (entry.cd_data_vfs >= 0 && entry.xa_sectors > 0) {
            // El trabajo vivo dejo de ser el de esta entrada (el usuario
            // salto de pista y el cursor del sondeo se movio): a la basura.
            // Mismo criterio que advance_chd_prefetch().
            if (g_ctx->chd_xa_probe.active() && g_ctx->chd_xa_probe.index != i)
                g_ctx->chd_xa_probe.reset();
            if (!g_ctx->chd_xa_probe.active() &&
                !chd_playlist::xa_probe_begin(g_ctx->chd_xa_probe, i, entry)) {
                entry.probe_failed = true;
                publish_probed_duration(i, entry);
                ++g_ctx->duration_probe_index;
                continue;
            }
            if (!chd_playlist::xa_probe_step(g_ctx->chd, g_ctx->chd_xa_probe,
                                             kXaProbeSectorsPerFrame)) {
                entry.probe_failed = true;
                publish_probed_duration(i, entry);
                ++g_ctx->duration_probe_index;
                continue;
            }
            if (!g_ctx->chd_xa_probe.active()) {
                // El trabajo se rindio solo: el fichero no tiene audio.
                entry.probe_failed = true;
                publish_probed_duration(i, entry);
                ++g_ctx->duration_probe_index;
                continue;
            }
            if (g_ctx->chd_xa_probe.complete()) {
                entry.cached_length_frames = g_ctx->chd_xa_probe.frames;
                g_ctx->chd_xa_probe.reset();
                publish_probed_duration(i, entry);
                ++g_ctx->duration_probe_index;
            }
            return;   // este frame ya ha gastado lo suyo
        }
#endif  // AOLIB_WITH_CHD

        const ProbeResult r = probe_zip_entry_from_prefix(entry);
        if (r == ProbeResult::NeedsFull) {
            // NO se avanza el cursor: la entrada se retoma en fase 2. El
            // trabajo que arranca depende de qué contenedor la trajo.
            bool empezado = false;
#ifdef AOLIB_WITH_CHD
            if (entry.from_chd) {
                empezado = entry.full_size <= kPrefetchMaxEntryBytes &&
                          chd_playlist::prefetch_begin(g_ctx->chd_probe_job, i, entry);
            } else
#endif
            {
                empezado = entry.full_size <= kPrefetchMaxEntryBytes &&
                          zip_inflate_begin(g_ctx->archive_path, *g_vfs, entry, g_ctx->probe_job);
                if (empezado) g_ctx->probe_job_index = i;
            }
            if (!empezado) {
                zip_inflate_abort(g_ctx->probe_job);
                g_ctx->probe_job_index = CoreContext::kNoPrefetch;
#ifdef AOLIB_WITH_CHD
                g_ctx->chd_probe_job.reset();
#endif
                ++g_ctx->duration_probe_index;   // se queda con "--:--"
            }
            return;
        }
        if (r == ProbeResult::Done) publish_probed_duration(i, entry);
        else if (r == ProbeResult::Failed) { entry.probe_failed = true; publish_probed_duration(i, entry); }
        ++g_ctx->duration_probe_index;
        if (now_us() - t0 >= kDurationProbeBudgetUs) break;
    }
}

#ifdef AOLIB_WITH_CHD
// Lectura anticipada del .chd, el equivalente de advance_zip_prefetch()
// para el tercer contenedor.
//
// Es mas simple que aquel porque no hay inflate que reanudar: en ISO9660 un
// fichero es un extent contiguo, asi que reanudar es recordar por que byte
// se iba. Pero hace la misma falta, y por la misma razon medida: leer una
// entrada entera dentro del cambio de pista mete el paron dentro del audio.
// Un .omu de 34 MB costaba un frame de 157 ms.
static void advance_chd_prefetch() {
    if (!g_ctx || !g_ctx->archive_is_chd) return;
    if (g_ctx->zip_entries.empty()) return;
    if (!g_ctx->engine || g_ctx->exhausted) return;

    // El mismo predicado que usa load_zip_entry_from() para elegir la
    // siguiente: prefetchear a ciegas indice+1 se equivoca justo en el
    // salto por encima de una dependencia.
    std::size_t next = g_ctx->zip_entry_index + 1;
    while (next < g_ctx->zip_entries.size() &&
           !zip_entry_is_playable(g_ctx->zip_entries[next])) {
        ++next;
    }

    // El trabajo vivo dejo de ser el que toca (el usuario salto): a la
    // basura. Tirarlo es gratis, no hay estado de decodificador que perder.
    if (g_ctx->chd_prefetch.active() && g_ctx->chd_prefetch.index != next)
        g_ctx->chd_prefetch.reset();
    if (next >= g_ctx->zip_entries.size()) return;

    ZipEntry& entry = g_ctx->zip_entries[next];
    // Una pista CD-DA no se materializa nunca: no hay nada que adelantar.
    if (entry.cd_track >= 0 || !entry.lazy || entry.complete()) return;

    if (!g_ctx->chd_prefetch.active() &&
        !chd_playlist::prefetch_begin(g_ctx->chd_prefetch, next, entry)) return;

    const double t0 = now_us();
    while (!g_ctx->chd_prefetch.complete()) {
        if (!chd_playlist::prefetch_step(g_ctx->chd, g_ctx->chd_prefetch,
                                         entry, kChdPrefetchChunk)) return;
        if (now_us() - t0 >= kPrefetchBudgetUs) break;
    }
    if (g_ctx->chd_prefetch.complete())
        chd_playlist::prefetch_commit(g_ctx->chd_prefetch, entry);
}
#else
// Sin CHD no hay nada que adelantar; la llamada de retro_run() se
// queda y el compilador la borra.
static void advance_chd_prefetch() {}
#endif  // AOLIB_WITH_CHD


// Adelanta el inflado de la entrada SIGUIENTE, a trocitos, un poco por frame.
//
// Es especulativo y desechable: si el usuario salta a otra pista, el trabajo
// se tira sin más. Y es OPCIONAL por diseño -- si no llega a tiempo,
// load_zip_entry_from() encuentra la entrada incompleta y la infla de una
// tacada igual que antes. Nunca puede ser peor que no tenerlo.
//
// No emite avisos: un fallo aquí no es un fallo de reproducción todavía. Si
// la entrada es ilegible de verdad, lo dirá el camino síncrono cuando toque
// reproducirla, con su aviso y su "prueba con la siguiente".
static void advance_zip_prefetch() {
    if (!g_ctx || g_ctx->zip_entries.empty()) return;
    if (!g_ctx->engine || g_ctx->exhausted)   return;
    // En un .chd tampoco: materializar es un seek sobre un extent
    // contiguo, no hay parón que repartir, y ZipInflateJob es minizip
    // -- le estaría dando un .chd a unzOpen().
    if (g_ctx->archive_is_chd) return;

    // La entrada siguiente NO es zip_entry_index + 1: un .zip puede traer
    // entradas que son dependencias y no pistas (el .psflib de un álbum PSF
    // es el caso de libro), y load_zip_entry_from() las salta. Prefetchear a
    // ciegas el índice+1 acertaba en todo el álbum menos justo en el salto
    // por encima de la dependencia -- medido en R4: 92,80 ms en esa única
    // transición y 0,00 ms en las 26 restantes. Se usa el mismo predicado
    // que la enumeración para no volver a desincronizarse.
    std::size_t next = g_ctx->zip_entry_index + 1;
    while (next < g_ctx->zip_entries.size() &&
           !zip_entry_is_playable(g_ctx->zip_entries[next])) {
        ++next;
    }

    // El trabajo vivo dejó de ser el que toca (el usuario saltó): a la basura.
    if (g_ctx->prefetch.active() && g_ctx->prefetch_index != next) {
        zip_inflate_abort(g_ctx->prefetch);
        g_ctx->prefetch_index = CoreContext::kNoPrefetch;
    }
    if (next >= g_ctx->zip_entries.size()) return;

    ZipEntry& entry = g_ctx->zip_entries[next];
    if (!entry.lazy || entry.complete()) return;
    if (entry.full_size > kPrefetchMaxEntryBytes) return;

    if (!g_ctx->prefetch.active()) {
        if (!zip_inflate_begin(g_ctx->archive_path, *g_vfs, entry, g_ctx->prefetch)) {
            zip_inflate_abort(g_ctx->prefetch);
            g_ctx->prefetch_index = CoreContext::kNoPrefetch;
            return;
        }
        g_ctx->prefetch_index = next;
    }

    const double t0 = now_us();
    while (!g_ctx->prefetch.complete()) {
        if (!zip_inflate_step(g_ctx->prefetch, kPrefetchChunkBytes)) {
            zip_inflate_abort(g_ctx->prefetch);
            g_ctx->prefetch_index = CoreContext::kNoPrefetch;
            return;
        }
        if (now_us() - t0 >= kPrefetchBudgetUs) break;
    }

    if (g_ctx->prefetch.complete()) {
        zip_inflate_commit(entry, g_ctx->prefetch);
        g_ctx->prefetch_index = CoreContext::kNoPrefetch;
    }
}

// Carga la entrada 'index' del .zip activo, o la primera posterior que
// funcione: si esa entrada concreta falla (fichero corrupto, PSF con _lib no
// resoluble dentro del zip, dependencia que no es pista) se sigue hacia
// adelante en vez de abortar la lista entera. Devuelve el índice realmente
// cargado, o -1 si ninguna entrada a partir de 'index' funcionó.
static long load_zip_entry_from(std::size_t index) {
    for (std::size_t i = index; i < g_ctx->zip_entries.size(); ++i) {
        // CRÍTICO: destruir el motor actual ANTES de construir el
        // siguiente, no después. Los motores aosdk comparten exclusión
        // mutua con un assert en el constructor (AosdkPsxCoreGuard, ver
        // aosdk_bridge.hpp), así que construir el nuevo mientras el anterior
        // sigue vivo -- aunque solo sea hasta el std::move() de más abajo, y
        // aunque sean de variantes distintas -- dispara ese assert.
        g_ctx->engine.reset();

        ZipEntry& entry = g_ctx->zip_entries[i];

        // Una entrada perezosa se infla ENTERA para reproducirla: el motor
        // conserva un puntero al buffer durante toda la pista, así que
        // tiene que estar completo antes de construirlo. Cuesta unos 43 ms
        // (medido), dentro del cambio de pista y fuera del bucle de audio.
        if (entry.lazy && !entry.complete()) {
            // Si el prefetch iba a medias de ESTA entrada, se termina aquí en
            // vez de tirarlo y reinflar desde cero: deflate no deja reanudar
            // desde un punto arbitrario, pero el trabajo ya hecho sigue vivo
            // dentro del job y solo falta lo que quede. Un prefetch a medias
            // convierte el parón entero en el trozo que faltaba.
            bool from_prefetch = false;
#ifdef AOLIB_WITH_CHD
            // El .chd tiene su propio trabajo anticipado; si iba por ESTA
            // entrada y ya esta entera, el cambio de pista sale gratis.
            if (g_ctx->archive_is_chd) {
                if (g_ctx->chd_prefetch.index == i &&
                    g_ctx->chd_prefetch.complete()) {
                    from_prefetch = chd_playlist::prefetch_commit(
                        g_ctx->chd_prefetch, entry);
                } else {
                    g_ctx->chd_prefetch.reset();
                }
            }
#endif  // AOLIB_WITH_CHD
            if (g_ctx->prefetch.active()) {
                if (g_ctx->prefetch_index == i &&
                    zip_inflate_step(g_ctx->prefetch, g_ctx->prefetch.total, archive_warn_log)) {
                    from_prefetch = zip_inflate_commit(entry, g_ctx->prefetch);
                }
                if (!from_prefetch) zip_inflate_abort(g_ctx->prefetch);
                g_ctx->prefetch_index = CoreContext::kNoPrefetch;
            }
            if (!from_prefetch && !materialize_entry(i))
                continue;   // ilegible: se prueba con la siguiente entrada
        }

        // Una pista CD-DA no pasa por el reparto: no hay nada que
        // olfatear. No tiene bytes en 'data' y no los tendra nunca --
        // las 27 pistas de un disco de PS1 son 454 MB -- asi que el
        // motor lee del CHD los sectores que necesita en cada render().
        std::unique_ptr<IAudioEngine> engine;
#ifdef AOLIB_WITH_CHD
        if (entry.cd_track >= 0) {
            auto cda = std::make_unique<CdAudioEngine>(
                &g_ctx->chd, static_cast<std::size_t>(entry.cd_track));
            if (cda->open(entry.name.c_str(), nullptr, 0, active_vfs()))
                engine = std::move(cda);
        } else if (entry.cd_data_vfs >= 0) {
            // La pista de datos entera, presentada a vgmstream como un .xa
            // de sectores crudos. 'data'/'size' van a nullptr/0 a propósito
            // -- streaming real por el vfs de la pista, nunca en RAM -- y
            // el vfs no es active_vfs(): es el bridge de esta pista
            // concreta, construido una vez en load_chd_content().
            bool needs_full = false;
            engine = construct_engine_for(
                entry.name.c_str(), entry.name, nullptr, 0,
                g_ctx->options.default_fade_seconds, g_ctx->options.loop_infinite,
                g_ctx->options.xmp_stereo_separation, &g_ctx->zip_entries, 0,
                &needs_full,
                g_ctx->chd_data_vfs[static_cast<std::size_t>(entry.cd_data_vfs)].get());
        } else
#endif
        {
            engine = construct_engine_for(nullptr, entry.name, entry.data.data(),
                                          entry.data.size(),
                                          g_ctx->options.default_fade_seconds,
                                          g_ctx->options.loop_infinite,
                                          g_ctx->options.xmp_stereo_separation,
                                          &g_ctx->zip_entries);
        }
        if (engine) {
            // Devolver a su prefijo TODAS las demás entradas perezosas:
            // sin esto la memoria sería acumulativa y el álbum acabaría
            // igual de cargado que antes, solo que más tarde.
            for (std::size_t j = 0; j < g_ctx->zip_entries.size(); ++j) {
                if (j != i) release_zip_entry(g_ctx->zip_entries[j]);
            }

            g_ctx->engine = std::move(engine);
            g_ctx->zip_entry_index = i;
            g_ctx->current_track_index = 0;
            g_ctx->frames_rendered = 0;

            // Cachear aquí la duración y el título de la entrada que se
            // acaba de abrir. El sondeo incremental NO puede tocar la
            // entrada que suena -- termina en release_zip_entry(), que le
            // quitaría al motor vivo el buffer que está leyendo -- así que
            // sin esto la primera pista se quedaría en "--:--" en cuanto se
            // saliera de ella. Y es gratis: la metadata ya está construida.
            {
                const auto& m0 = g_ctx->engine->metadata();
                entry.cached_length_frames = m0.playback_frames();
                if (!m0.title.empty()) entry.cached_title = m0.title;
            }
            // 'iUseReverb' es estado global del SPU y cada motor nuevo nace
            // con el valor de fábrica: hay que reaplicarlo en CADA
            // construcción, no solo en la primera.
            g_ctx->engine->set_reverb_enabled(g_ctx->options.spu_reverb_enabled);
            // Vía CENTRAL de cambio de pista en un .zip (avance automático,
            // |< / >|, reinicio): vaciar aquí la cola del reverb cubre todos
            // esos casos de una vez.
            g_reverb.clear();
            return static_cast<long>(i);
        }
    }
    return -1;
}

// Reinicia desde el principio: recarga la entrada 0 del .zip si hay uno
// activo, o pide select_track(0) al motor si es un fichero suelto. La usan
// retro_reset() y la rama loop_infinite de retro_run(). Devuelve true si el
// reinicio tuvo éxito; no toca 'exhausted', eso lo decide cada llamante.
static bool restart_from_beginning() {
    if (!g_ctx->zip_entries.empty()) {
        return load_zip_entry_from(0) >= 0;
    }
    if (g_ctx->engine && g_ctx->engine->select_track(0)) {
        g_ctx->current_track_index = 0;
        g_ctx->frames_rendered = 0;
        return true;
    }
    return false;
}

#ifdef AOLIB_WITH_CHD
// Carga de un .chd, el tercer contenedor. Sale de retro_load_game() por
// la misma regla que todo lo demas: libretro.cpp son SOLO los 25 puntos
// de entrada, y decidir que hay dentro de un contenedor es logica de
//
// Devuelve false SIN tocar g_ctx: destruirlo y refrescar la UI son cosa
// de retro_load_game(), que es quien lo creo.
// contenedor.
static bool load_chd_content(const char* path) {
    // Tercer contenedor. Un .chd puede traer musica por dos caminos --
    // pistas CD-DA y ficheros dentro del sistema de ficheros de la
    // imagen -- y chd_playlist::enumerate() los mete en la MISMA lista
    // de ZipEntry, asi que a partir de aqui el core no distingue de
    // donde vino cada pista.
    //
    // El mapa de hunks va al principio del fichero y cada hunk se
    // comprime por separado, o sea que llegar al byte N es O(1): no hay
    // nada que decodificar por adelantado ni presupuesto que administrar.
    if (!g_vfs || !g_vfs->is_valid()) {
        log_message(RETRO_LOG_ERROR,
            "[aolib] .chd sin VFS disponible, no se puede leer sin fopen real.");
        return false;
    }

    std::string why;
    if (!g_ctx->chd.open(path, *g_vfs, why)) {
        log_message(RETRO_LOG_ERROR, "[aolib] %s: %s",
                    path, why.c_str());
        return false;
    }

    g_ctx->archive_path   = path;
    g_ctx->archive_is_chd = true;

    const chd_header& hd = g_ctx->chd.header();
    log_message(RETRO_LOG_INFO,
        "[aolib] %s: CHD v%u, %llu bytes logicos en %u hunks de %u%s.",
        path, hd.version,
        static_cast<unsigned long long>(hd.logicalbytes),
        hd.totalhunks, hd.hunkbytes,
        g_ctx->chd.is_cd() ? ", con tabla de pistas" : " (imagen cruda)");

    auto archive_warn2 = [](const std::string& msg) { archive_warn_log(msg); };
    if (!chd_playlist::enumerate(g_ctx->chd, g_ctx->zip_entries, archive_warn2) ||
        g_ctx->zip_entries.empty()) {
        log_message(RETRO_LOG_ERROR,
            "[aolib] %s: no se encontro nada reproducible dentro del .chd.",
            path);
        return false;
    }

    // Un ChdDataTrackVfs por pista de datos que trajo una entrada .xa
    // virtual. Se construye UNA vez aqui, no en load_zip_entry_from(): el
    // streamfile de vgmstream conserva el puntero durante toda la vida del
    // motor, incluidos los reaperturas de select_track() al cambiar de
    // subsong XA, asi que tiene que sobrevivir mas que cualquier motor.
    //
    // Uno por ENTRADA, no por pista: cada fichero XA del disco es una
    // ventana distinta de sectores sobre la MISMA pista de datos, asi que
    // comparten pista pero no bridge. 'cd_data_vfs' entra con el indice de
    // la pista y sale con el del bridge.
    // El motor anterior, si lo hubiera, se va ANTES que los bridges: guarda
    // el puntero a uno de ellos y lo usa al cerrar. Misma razon que en
    // ~CoreContext().
    g_ctx->engine.reset();
    g_ctx->chd_data_vfs.clear();
    std::size_t xa_tracks = 0;
    for (ZipEntry& e : g_ctx->zip_entries) {
        if (e.cd_data_vfs < 0) continue;
        const auto pista = static_cast<std::size_t>(e.cd_data_vfs);
        if (e.xa_sectors > 0)
            g_ctx->chd_data_vfs.push_back(
                std::make_unique<chd_vfs_bridge::ChdDataTrackVfs>(
                    g_ctx->chd, pista, e.xa_first_sector, e.xa_sectors));
        else
            g_ctx->chd_data_vfs.push_back(
                std::make_unique<chd_vfs_bridge::ChdDataTrackVfs>(g_ctx->chd, pista));
        e.cd_data_vfs = static_cast<int>(g_ctx->chd_data_vfs.size() - 1);
        ++xa_tracks;
    }

    std::size_t cd = 0;
    for (const ZipEntry& e : g_ctx->zip_entries) if (e.cd_track >= 0) ++cd;
    char xa_suffix[48] = "";
    if (xa_tracks == 1) std::snprintf(xa_suffix, sizeof(xa_suffix), ", 1 de audio XA");
    else if (xa_tracks > 1) std::snprintf(xa_suffix, sizeof(xa_suffix), ", %zu de audio XA", xa_tracks);
    log_message(RETRO_LOG_INFO,
        "[aolib] %s: %zu pistas (%zu CD-DA, %zu del sistema de ficheros%s).",
        path, g_ctx->zip_entries.size(), cd,
        g_ctx->zip_entries.size() - cd - xa_tracks, xa_suffix);

    // El barrido eager de duraciones solo existe por aosdk, que pisa
    // estaticas de proceso. Las pistas CD-DA ya traen su duracion
    // calculada (es aritmetica: un sector son 588 frames) y el resto lo
    // sondea retro_run() por frames, como en un .zip.
    precompute_zip_track_durations();

    if (load_zip_entry_from(0) < 0) {
        log_message(RETRO_LOG_ERROR,
            "[aolib] %s: ninguna pista del .chd pudo abrirse.", path);
        return false;
    }
    return true;
}
#endif  // AOLIB_WITH_CHD

}  // namespace aolib
