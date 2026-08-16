// libretro.cpp — punto de entrada del core: las 25 funciones retro_*, el
// despacho a los motores de audio y el puente hacia la UI.
//
// Emite siempre 44100 Hz y 60 fps. Toda la E/S de fichero pasa por el VFS
// de Libretro (vfs_bridge.hpp); no se usa fopen ni equivalentes.

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <exception>
#include <memory>

#include "libretro.h"
#include "core_context.hpp"
#include "dsp/reverb.hpp"
#include "vfs_bridge.hpp"
#include "zip_playlist.hpp"
#include "engine/gme_engine.hpp"
#include "engine/libvgm_engine.hpp"
#include "ui/audio_analyzer.hpp"
#include "ui/transport.hpp"
#include "ui/ui_model.hpp"
#include "ui/ui_screen.hpp"
#ifdef AOLIB_WITH_PSF
#include "engine/psf_engine.hpp"
#include "engine/psf2_engine.hpp"
#include "engine/ssf_engine.hpp"
#endif
#ifdef AOLIB_TEST_HOOKS
// Motor fixture SOLO para tests (ver el target test-select-track-fail del
// Makefile). Este include y su bloque de despacho desaparecen por completo
// cuando AOLIB_TEST_HOOKS no está definido, que es siempre en 'make all'.
#include "../tests/f7b_fixture_engine.hpp"
#endif

extern "C" {
#include "gme.h"
}

namespace {

retro_environment_t        environ_cb  = nullptr;
retro_video_refresh_t      video_cb    = nullptr;
retro_audio_sample_batch_t audio_batch_cb = nullptr;
retro_input_poll_t         input_poll_cb  = nullptr;
retro_log_printf_t         log_cb      = nullptr;

std::unique_ptr<CoreContext> g_ctx;
std::unique_ptr<LibretroVFS> g_vfs;
NullVFSBridge g_null_vfs; // referencia inerte cuando no hay VFS real (ver vfs_bridge.hpp)
bool g_can_dupe = false;

// Framebuffer de la UI, en XRGB8888. El formato DEBE declararse con
// SET_PIXEL_FORMAT: sin esa llamada el frontend asume 0RGB1555, que es el
// default de Libretro, y los colores salen mal.
constexpr unsigned kScreenWidth  = ui::kScreenW;
constexpr unsigned kScreenHeight = ui::kScreenH;
std::vector<uint32_t> g_framebuffer;   // XRGB8888, kScreenWidth*kScreenHeight

// Estado de la UI. Vive fuera de CoreContext a propósito: sobrevive a
// retro_unload_game(), porque el volumen y el modo de repetición elegidos
// no deben resetearse al cambiar de álbum. Todo lo derivado del contenido
// se reconstruye en cada rebuild_ui_model().

// Bloques EXTRA rendidos y descartados por cada retro_run() mientras se
// mantiene >>. 3 extra + 1 emitido = velocidad 4x. Subirlo arriesga que un
// motor caro (PSF2/SSF, que emulan una CPU real) no termine dentro del
// presupuesto del frame y se oigan huecos.
constexpr int kFfExtraBlocks = 3;

// ═══════════════════ Diagnóstico de la salida de audio ═══════════════════
//
// Existe para poder responder a "¿el corte lo pone el core o la cadena
// frontend->driver?" con números tomados en la máquina donde ocurre: aquí
// el presupuesto de 16,667 ms se reparte entre el core, el resampler del
// frontend y el driver, cosa que un banco de pruebas sin driver ni
// contención de CPU no reproduce.
//
// Mide dos cosas: frames de relleno entregados de verdad (underruns) y
// coste por retro_run() frente al presupuesto, para cruzarlo con la
// estadística 'close_to_underrun' que ya calcula RetroArch. Cuesta dos
// lecturas de reloj por frame (~80 ns sobre 300-2000 us).
struct AudioDiag {
    unsigned long frames_run       = 0;
    unsigned long underrun_events  = 0;   // retro_run() que entregaron relleno
    unsigned long underrun_frames  = 0;   // total de frames de relleno
    unsigned long over_budget      = 0;   // frames por encima de 16,667 ms
    double        total_us         = 0.0;
    double        worst_us         = 0.0;
    int           warns_emitted    = 0;   // limitado, para no inundar el log
};
AudioDiag g_diag;

// Reloj monótono en microsegundos. std::chrono y no clock_gettime para que
// la compilación de Windows (mingw) use exactamente la misma fuente.
inline double now_us() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::micro>(clock::now().time_since_epoch()).count();
}

// Presupuesto de un frame a los 60 fps que declara retro_get_system_av_info.
constexpr double kFrameBudgetUs = 1000000.0 / 60.0;



ui::UiModel     g_ui;
ui::AudioAnalyzer g_analyzer;

// Reverb de la capa host. Vive FUERA de CoreContext, como g_analyzer y
// g_ui: sus ~30 KB de buffers se reservan una vez al arrancar el proceso y
// no se vuelven a pedir en cada carga, porque una asignación dentro de
// retro_run() es justo lo que produce un pico por encima del presupuesto de
// frame. Lo que sí es por sesión es su ESTADO (la cola), y eso se vacía con
// clear() en los mismos puntos donde se resetea el analizador.
dsp::Reverb     g_reverb;
ui::InputEdge   g_input_edge;
retro_input_state_t input_state_cb = nullptr;

void log_message(enum retro_log_level level, const char* fmt, ...) {
    if (log_cb) {
        va_list args;
        va_start(args, fmt);
        char buf[512];
        std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        log_cb(level, "%s\n", buf);
    }
}

void log_audio_diag_summary(const char* motivo) {
    if (g_diag.frames_run == 0) return;
    const double mean_us = g_diag.total_us / static_cast<double>(g_diag.frames_run);
    const bool   algo    = (g_diag.underrun_events > 0 || g_diag.over_budget > 0);
    log_message(algo ? RETRO_LOG_WARN : RETRO_LOG_INFO,
        "[aolib] audio (%s): %lu frames | underruns: %lu (%lu frames de relleno, %.1f ms) | "
        "coste por frame: media %.0f us, peor %.0f us, presupuesto %.0f us | "
        "por encima del presupuesto: %lu (%.3f %%)",
        motivo, g_diag.frames_run, g_diag.underrun_events, g_diag.underrun_frames,
        g_diag.underrun_frames / 44.1, mean_us, g_diag.worst_us, kFrameBudgetUs,
        g_diag.over_budget,
        100.0 * static_cast<double>(g_diag.over_budget) / static_cast<double>(g_diag.frames_run));
    if (algo) {
        log_message(RETRO_LOG_WARN,
            "[aolib] si se oyen cortes: 'underruns > 0' apunta al core (no llegó a producir "
            "audio a tiempo); 'underruns == 0' con cortes audibles apunta a la cadena "
            "frontend->driver (buffer de audio, resampler o Dynamic Rate Control).");
    }
}

IVFSBridge& active_vfs() {
    return (g_vfs && g_vfs->is_valid())
        ? static_cast<IVFSBridge&>(*g_vfs)
        : static_cast<IVFSBridge&>(g_null_vfs);
}

bool has_suffix(const std::string& name, const char* suffix) {
    const std::size_t sl = std::strlen(suffix);
    if (name.size() < sl) return false;
    return std::equal(name.end() - static_cast<long>(sl), name.end(), suffix,
                       [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; });
}

#ifdef AOLIB_WITH_PSF
// Busca un "_lib"/"_libN" entre las entradas hermanas del mismo .zip, por
// basename e insensible a mayúsculas: esos tags casi siempre son nombres
// sueltos sin ruta, y su case no tiene por qué coincidir con el del fichero
// real. Sirve para los tres motores aosdk porque quien resuelve "_lib" es
// corlett_decode(), que es común a todos.
AosdkLibResolver::SiblingLookup make_sibling_lookup(const std::vector<ZipEntry>* siblings) {
    return [siblings](const std::string& name, std::vector<uint8_t>& out) -> bool {
        auto to_lower = [](std::string s) {
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        const std::string wanted = to_lower(name);
        for (const auto& entry : *siblings) {
            std::string entry_basename = entry.name;
            const auto slash = entry_basename.find_last_of("/\\");
            if (slash != std::string::npos) entry_basename = entry_basename.substr(slash + 1);
            if (to_lower(entry_basename) == wanted) {
                out = entry.data;
                return true;
            }
        }
        return false;
    };
}

// Construye y abre un motor aosdk (PSF1, PSF2 o SSF): las tres ramas son
// idénticas salvo el tipo concreto. 'variant_label' es solo para el log.
template <typename EngineT>
std::unique_ptr<IAudioEngine> construct_psf_variant(
    const char* uri, const std::string& display_name,
    const uint8_t* data, std::size_t size,
    const std::vector<ZipEntry>* siblings, const char* variant_label) {
    if (uri && (!g_vfs || !g_vfs->is_valid())) {
        log_message(RETRO_LOG_ERROR,
            "[aolib] %s: fichero %s sin VFS disponible.",
            display_name.c_str(), variant_label);
        return nullptr;
    }
    auto engine = std::make_unique<EngineT>();
    if (siblings) {
        engine->set_sibling_lookup(make_sibling_lookup(siblings));
    }
    if (!engine->open(uri, data, size, active_vfs())) {
        log_message(RETRO_LOG_ERROR, "[aolib] %s: %s_start() falló.",
                    display_name.c_str(), variant_label);
        return nullptr;
    }
    log_message(RETRO_LOG_INFO, "[aolib] %s: cargado vía %s.",
                display_name.c_str(), engine->engine_name());
    return engine;
}
#endif // AOLIB_WITH_PSF

// Construye y abre el motor adecuado para una entrada de contenido.
// 'display_name' solo sirve para detectar la extensión, no necesita existir
// en disco. 'uri' es la ruta REAL en disco si la hay (ficheros sueltos), o
// nullptr para entradas ya extraídas en memoria. 'siblings', si no es
// nullptr, permite resolver "_lib" contra otras entradas del mismo .zip:
// sin eso, un álbum PSF con .psflib compartido -- el caso típico -- falla
// en cascada para todas las pistas que dependan de él.
std::unique_ptr<IAudioEngine> construct_engine_for(
    const char* uri, const std::string& display_name,
    const uint8_t* data, std::size_t size, double default_fade_seconds,
    bool loop_infinite, const std::vector<ZipEntry>* siblings = nullptr) {

    const bool looks_psf = has_suffix(display_name, ".psf") || has_suffix(display_name, ".minipsf");
    const bool looks_psf2 = has_suffix(display_name, ".psf2") || has_suffix(display_name, ".minipsf2");
    // Sega Saturn (SSF/MiniSSF): mismo contenedor corlett que PSF, motor
    // distinto (eng_ssf.c, Musashi M68000 + SCSP).
    const bool looks_ssf = has_suffix(display_name, ".ssf") || has_suffix(display_name, ".minissf");
    // .vgm/.vgz van a LibvgmEngine. Esta comprobación DEBE ir ANTES de la
    // rama de libgme: gme_identify_extension() sigue reclamando ambas
    // extensiones (libgme define gme_vgz_type) y no hay forma de
    // des-registrarlas sin parchear el código vendorizado.
    const bool looks_vgm = has_suffix(display_name, ".vgm") || has_suffix(display_name, ".vgz");

#ifdef AOLIB_TEST_HOOKS
    // Extensión reconocida SOLO en builds de test; nunca compite con una
    // extensión real.
    if (has_suffix(display_name, ".f7bfixture")) {
        return std::make_unique<F7bAlwaysFailFixtureEngine>();
    }
#endif

    if (looks_psf2) {
#ifdef AOLIB_WITH_PSF
        return construct_psf_variant<Psf2Engine>(uri, display_name, data, size, siblings, "psf2");
#else
        log_message(RETRO_LOG_WARN,
            "[aolib] %s: PSF2 reconocido pero este build no enlaza USE_PSF_ENGINE=1.",
            display_name.c_str());
        return nullptr;
#endif
    }

    if (looks_psf) {
#ifdef AOLIB_WITH_PSF
        return construct_psf_variant<PsfEngine>(uri, display_name, data, size, siblings, "psf");
#else
        log_message(RETRO_LOG_WARN,
            "[aolib] %s: PSF reconocido pero este build no enlaza USE_PSF_ENGINE=1.",
            display_name.c_str());
        return nullptr;
#endif
    }

    if (looks_ssf) {
#ifdef AOLIB_WITH_PSF
        return construct_psf_variant<SsfEngine>(uri, display_name, data, size, siblings, "ssf");
#else
        log_message(RETRO_LOG_WARN,
            "[aolib] %s: SSF reconocido pero este build no enlaza USE_PSF_ENGINE=1.",
            display_name.c_str());
        return nullptr;
#endif
    }

    if (looks_vgm) {
        // No exige fullpath ni VFS: LibvgmEngine acepta memoria
        // directamente vía MemoryLoader_Init.
        if (!data || size == 0) {
            log_message(RETRO_LOG_ERROR, "[aolib] %s: sin contenido en memoria.",
                        display_name.c_str());
            return nullptr;
        }
        const int fade_msecs = static_cast<int>(default_fade_seconds * 1000.0);
        auto vgm = std::make_unique<LibvgmEngine>(fade_msecs, loop_infinite);
        LibvgmEngine* vgm_raw = vgm.get(); // para last_open_error(), ver más abajo
        if (!vgm->open(uri, data, size, active_vfs())) {
            // last_open_error() no forma parte de IAudioEngine: es una
            // consulta sobre el puntero concreto, antes de devolverlo como
            // unique_ptr<IAudioEngine>. Da el detalle que hace falta cuando
            // el fichero declara un chip sin core de emulación compilado.
            if (!vgm_raw->last_open_error().empty()) {
                log_message(RETRO_LOG_ERROR, "[aolib] %s: %s.",
                            display_name.c_str(), vgm_raw->last_open_error().c_str());
            } else {
                log_message(RETRO_LOG_ERROR, "[aolib] %s: LibvgmEngine::open() falló.",
                            display_name.c_str());
            }
            return nullptr;
        }
        log_message(RETRO_LOG_INFO, "[aolib] %s: cargado vía %s.",
                    display_name.c_str(), vgm->engine_name());
        return vgm;
    }

    // libgme: identificación por extensión o por magic bytes. Aquí ya no
    // pueden llegar .vgm/.vgz, capturadas más arriba.
    gme_type_t type = gme_identify_extension(display_name.c_str());
    if (!type && data && size >= 4) {
        const char* suffix = gme_identify_header(data);
        if (suffix && suffix[0] != '\0') type = gme_identify_extension(suffix);
    }
    if (!type) {
        log_message(RETRO_LOG_WARN, "[aolib] %s: formato no reconocido, omitido.",
                    display_name.c_str());
        return nullptr;
    }
    if (!data || size == 0) {
        log_message(RETRO_LOG_ERROR, "[aolib] %s: sin contenido en memoria.",
                    display_name.c_str());
        return nullptr;
    }

    const int fade_msecs = static_cast<int>(default_fade_seconds * 1000.0);
    auto gme = std::make_unique<GmeEngine>(fade_msecs);
    if (!gme->open(uri, data, size, active_vfs())) {
        log_message(RETRO_LOG_ERROR, "[aolib] %s: gme_open_data falló.", display_name.c_str());
        return nullptr;
    }
    log_message(RETRO_LOG_INFO, "[aolib] %s: cargado vía %s (%u pistas).",
                display_name.c_str(), gme->engine_name(), gme->track_count());
    return gme;
}

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
void precompute_zip_track_durations() {
    if (!g_ctx) return;
    for (auto& entry : g_ctx->zip_entries) {
        if (!zip_entry_is_playable(entry.name)) continue;

        auto tmp = construct_engine_for(
            nullptr, entry.name, entry.data.data(), entry.data.size(),
            g_ctx->options.default_fade_seconds, g_ctx->options.loop_infinite,
            &g_ctx->zip_entries);
        // tmp muere al acabar la iteración, liberando el guard de aosdk
        // ANTES de que la siguiente construya el suyo: la misma secuencia
        // destruir-antes-de-construir que exige load_zip_entry_from().
        if (!tmp) continue; // se registra su propio log de error; esta
                             // entrada se queda con "--:--" y no bloquea
                             // el resto del barrido.

        const auto& m = tmp->metadata();
        // Duración REAL: incluye el fade y, en VGM, los bucles que se van a
        // reproducir (ver TrackMetadata::playback_frames()).
        entry.cached_length_frames = m.playback_frames();
        if (!m.title.empty()) entry.cached_title = m.title;
    }
}


// hacia adelante si esa entrada concreta falla (fichero corrupto, PSF con
// _lib no resoluble dentro del zip, etc.) en vez de abortar toda la
// lista. Devuelve el índice realmente cargado, o -1 si ninguna entrada a
// partir de 'index' funcionó.
long load_zip_entry_from(std::size_t index) {
    for (std::size_t i = index; i < g_ctx->zip_entries.size(); ++i) {
        // CRÍTICO: destruir el motor actual ANTES de construir el
        // siguiente, no después. Los motores aosdk comparten exclusión
        // mutua con un assert en el constructor (AosdkPsxCoreGuard, ver
        // aosdk_bridge.hpp), así que construir el nuevo mientras el anterior
        // sigue vivo -- aunque solo sea hasta el std::move() de más abajo, y
        // aunque sean de variantes distintas -- dispara ese assert.
        g_ctx->engine.reset();

        const ZipEntry& entry = g_ctx->zip_entries[i];
        auto engine = construct_engine_for(nullptr, entry.name, entry.data.data(),
                                            entry.data.size(), g_ctx->options.default_fade_seconds,
                                            g_ctx->options.loop_infinite, &g_ctx->zip_entries);
        if (engine) {
            g_ctx->engine = std::move(engine);
            g_ctx->zip_entry_index = i;
            g_ctx->current_track_index = 0;
            g_ctx->frames_rendered = 0;
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
bool restart_from_beginning() {
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

// Lee una variable booleana declarada como "disabled|enabled" (el orden
// solo fija cuál es el valor por defecto; GET_VARIABLE devuelve siempre la
// cadena elegida). 'true_value' es la cadena que significa 'true' hoy.
//
// 'legacy_true_value' (opcional, nullptr = no aplica) existe por
// compatibilidad: estas opciones se declararon en español
// ("habilitado"/"deshabilitado") en versiones anteriores, y RetroArch
// conserva el valor guardado aunque ya no aparezca en la lista declarada.
// Sin aceptarlo como sinónimo, una configuración antigua se leería como
// false en silencio.
bool get_bool_variable(const char* key, bool current, const char* true_value,
                        const char* legacy_true_value = nullptr) {
    if (!environ_cb) return current;
    retro_variable var{key, nullptr};
    if (!environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || !var.value) return current;
    if (std::strcmp(var.value, true_value) == 0) return true;
    if (legacy_true_value && std::strcmp(var.value, legacy_true_value) == 0) return true;
    return false;
}

// Valor CRUDO de una opción, sin interpretar. Lo necesita
// "aolib_reverb_amount", cuyo "1"/"2"/"3" no es booleano ni un número que
// se use tal cual: el parseo vive en
// dsp::Reverb::amount_from_level_option() para poder probarlo aislado.
// Devuelve nullptr si el frontend no da valor.
const char* get_raw_variable(const char* key) {
    if (!environ_cb) return nullptr;
    retro_variable var{key, nullptr};
    if (!environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) return nullptr;
    return var.value;
}

double get_double_variable(const char* key, double current) {
    if (!environ_cb) return current;
    retro_variable var{key, nullptr};
    if (!environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || !var.value) return current;
    char* end = nullptr;
    const double parsed = std::strtod(var.value, &end);
    return (end && end != var.value) ? parsed : current;
}

// Lee las opciones de core y las aplica a g_ctx->options, propagando además
// a set_reverb_enabled() del motor activo (iUseReverb es estado global del
// SPU: hay que reaplicarlo cada vez que la opción cambia, no solo al
// cargar). Se llama tras cada retro_load_game() con éxito y en cada
// retro_run() en que el frontend señala GET_VARIABLE_UPDATE.
void apply_core_options() {
    if (!g_ctx) return;
    g_ctx->options.loop_infinite = get_bool_variable(
        "aolib_loop_infinite", g_ctx->options.loop_infinite, "enabled", "habilitado");
    g_ctx->options.default_fade_seconds = get_double_variable(
        "aolib_default_fade_seconds", g_ctx->options.default_fade_seconds);
    g_ctx->options.spu_reverb_enabled = get_bool_variable(
        "aolib_spu_reverb_enabled", g_ctx->options.spu_reverb_enabled, "enabled", "habilitado");

    // El menú NO enciende ni apaga el reverb de la capa host: eso es
    // exclusivo del botón REB del deck (ver DeckButton::Reverb). La API de
    // Libretro va en una sola dirección -- el core LEE con GET_VARIABLE y no
    // existe ningún SET_VARIABLE; SET_CORE_OPTIONS_DISPLAY solo afecta a la
    // visibilidad de una opción, no a su valor -- así que un booleano con
    // dos controles quedaría desincronizado en cuanto se pulsara REB, con el
    // menú mostrando "disabled" para siempre y sin forma de corregirlo.
    //
    // Lo que sí hace el menú es fijar el NIVEL al que se encenderá la
    // próxima vez (host_reverb_last_on) y, si el reverb ya está sonando,
    // actualizarlo EN VIVO.
    {
        const char* raw_level = get_raw_variable("aolib_reverb_amount");
        // Un nivel ausente o ilegible cae al último activo, nunca fuerza un
        // apagado: apagado significa otra cosa (que se pulsó REB).
        const int level = dsp::Reverb::amount_from_level_option(
            raw_level, g_ctx->options.host_reverb_last_on);
        g_ctx->options.host_reverb_last_on = level;

        if (g_ctx->options.host_reverb_amount > 0 &&
            g_ctx->options.host_reverb_amount != level) {
            g_ctx->options.host_reverb_amount = level;
        }
    }

    if (g_ctx->engine) g_ctx->engine->set_reverb_enabled(g_ctx->options.spu_reverb_enabled);
}

// Vuelca al log la metadata de la pista. Se llama al completar una carga y
// en cada avance de pista dentro de retro_run().
void log_now_playing() {
    if (!g_ctx || !g_ctx->engine) return;
    const auto& m = g_ctx->engine->metadata();
    log_message(RETRO_LOG_INFO,
        "[aolib] Reproduciendo: \"%s\" (%s) — %s — %.1fs%s",
        m.title.c_str(), m.game.c_str(), m.artist.c_str(),
        m.length_frames / 44100.0,
        m.fade_frames > 0 ? " + fade" : "");
}

// ═══════════════════════ Puente core -> UI ═══════════════════════
//
// Tres funciones, con responsabilidades deliberadamente separadas:
//
//   rebuild_ui_model()          -- estado que solo cambia al cargar/avanzar
//   refresh_ui_dynamic_state()  -- estado que cambia en cada frame
//   apply_ui_input()            -- traduce el mando a acciones reales
//
// La razón de partirlo así es coste: reconstruir la lista de pistas
// implica recorrer zip_entries y consultar metadata de cada motor, y eso
// no tiene por qué pasar 60 veces por segundo. Lo que sí cambia cada
// frame (posición, VU, espectro) es barato y va aparte.

// Recorta la ruta de una entrada de .zip a su nombre de fichero, y le
// quita la extensión: "OST/03 Battle.vgm" -> "03 Battle". Los nombres
// completos no caben en un panel de 142 px y la extensión es ruido cuando
// todas las entradas del álbum comparten formato.
std::string display_label_for(const std::string& raw) {
    std::string out = raw;
    const auto slash = out.find_last_of("/\\");
    if (slash != std::string::npos) out = out.substr(slash + 1);
    const auto dot = out.find_last_of('.');
    if (dot != std::string::npos && dot > 0) out = out.substr(0, dot);
    return out;
}

// Reconstruye la lista visible y la metadata de "now playing".
//
// zip_entries mezcla a propósito pistas reproducibles con dependencias
// (.psflib/.lib/.psf2lib/.ssflib), porque estas hacen falta para resolver
// "_lib". En pantalla eso sería una lista que miente ("12 pistas" cuando
// hay 9 canciones y 3 librerías), así que aquí se filtra con
// zip_entry_is_playable() y se mantiene el mapeo entre el índice VISIBLE y
// el índice real en zip_entries, que ya no coinciden.
void rebuild_ui_model() {
    g_ui.tracks.clear();
    g_ui.playable_to_zip.clear();
    g_ui.title.clear();
    g_ui.game.clear();
    g_ui.artist.clear();
    g_ui.engine_name.clear();
    g_ui.chip.clear();
    g_ui.length_frames = 0;

    if (!g_ctx) {
        g_ui.current_track = 0;
        g_ui.cursor = 0;
        return;
    }

    if (!g_ctx->zip_entries.empty()) {
        for (std::size_t i = 0; i < g_ctx->zip_entries.size(); ++i) {
            const auto& e = g_ctx->zip_entries[i];
            if (!zip_entry_is_playable(e.name)) continue;   // dependencia, no pista
            ui::TrackListItem item;
            // Título real si precompute_zip_track_durations() lo consiguió
            // (tags Corlett/GD3 de ESA entrada); si no, el nombre de fichero
            // recortado. 0 en cached_length_frames significa "desconocida",
            // y la UI dibuja "--:--", nunca un cero que parecería duración
            // real.
            item.label = e.cached_title.empty() ? display_label_for(e.name) : e.cached_title;
            item.length_frames = e.cached_length_frames;
            g_ui.playable_to_zip.push_back(i);
            g_ui.tracks.push_back(std::move(item));
        }
        // Índice visible de la entrada que suena.
        g_ui.current_track = 0;
        for (std::size_t v = 0; v < g_ui.playable_to_zip.size(); ++v) {
            if (g_ui.playable_to_zip[v] == g_ctx->zip_entry_index) {
                g_ui.current_track = static_cast<int>(v);
                break;
            }
        }
    } else if (g_ctx->engine) {
        // Fichero suelto: la lista son sus subsongs (NSF/GBS/KSS/SAP/AY/HES
        // vía libgme). Un solo track para los formatos de una sola pista.
        const unsigned n = g_ctx->engine->track_count();
        for (unsigned i = 0; i < n; ++i) {
            ui::TrackListItem item;
            char buf[32];
            std::snprintf(buf, sizeof(buf), "Track %02u", i + 1);
            item.label = buf;
            item.length_frames = 0;
            g_ui.tracks.push_back(std::move(item));
        }
        g_ui.current_track = static_cast<int>(g_ctx->current_track_index);
    }

    if (g_ctx->engine) {
        const auto& m = g_ctx->engine->metadata();
        g_ui.title       = m.title;
        g_ui.game        = m.game;
        g_ui.artist      = m.artist;
        g_ui.engine_name = g_ctx->engine->engine_name();
        g_ui.chip        = m.chip;
        g_ui.length_frames = m.playback_frames();

        // La pista activa SÍ tiene duración conocida y título real: se
        // sobrescriben los placeholders para ella.
        const int cur = g_ui.current_track;
        if (cur >= 0 && cur < static_cast<int>(g_ui.tracks.size())) {
            auto& item = g_ui.tracks[static_cast<std::size_t>(cur)];
            item.length_frames = m.playback_frames();
            if (!m.title.empty()) item.label = m.title;
        }
    }

    g_ui.cursor = std::clamp(g_ui.cursor, 0,
                             std::max(0, static_cast<int>(g_ui.tracks.size()) - 1));
}

// Estado que cambia en cada frame. Barato: sin asignaciones, sin recorrer
// zip_entries, sin consultar metadata.
void refresh_ui_dynamic_state() {
    g_ui.analyzer  = &g_analyzer;
    // Reloj de la marquesina. Avanza SIEMPRE, tambien en pausa y sin
    // contenido cargado: un titulo largo tiene que poder leerse entero
    // estando en pausa, que es justo cuando se mira la lista.
    ++g_ui.anim_tick;
    if (!g_ctx) {
        g_ui.position_frames = 0;
        g_ui.exhausted = false;
        return;
    }
    g_ui.position_frames = g_ctx->frames_rendered;
    g_ui.exhausted       = g_ctx->exhausted;
    g_ui.repeat          = g_ctx->options.loop_infinite ? ui::RepeatMode::All
                                                        : g_ui.repeat_when_not_all;
    // El borde naranja del botón REB refleja el estado REAL del efecto.
    g_ui.reverb_enabled  = (g_ctx->options.host_reverb_amount > 0);
}

// Carga la pista visible 'visible_index' (índice de g_ui.tracks).
// Devuelve true si algo empezó a sonar.
bool ui_play_visible_track(int visible_index) {
    if (!g_ctx || g_ui.tracks.empty()) return false;
    visible_index = std::clamp(visible_index, 0,
                               static_cast<int>(g_ui.tracks.size()) - 1);

    bool ok = false;
    if (!g_ui.playable_to_zip.empty()) {
        // Entrada de .zip: load_zip_entry_from avanza HACIA ADELANTE si la
        // entrada pedida falla, así que el índice que acaba cargado puede
        // no ser el pedido -- se relee de g_ctx después, en vez de asumirlo.
        ok = load_zip_entry_from(
                 g_ui.playable_to_zip[static_cast<std::size_t>(visible_index)]) >= 0;
    } else if (g_ctx->engine) {
        ok = g_ctx->engine->select_track(static_cast<unsigned>(visible_index));
        if (ok) {
            g_ctx->current_track_index = static_cast<unsigned>(visible_index);
            g_ctx->frames_rendered = 0;
        }
    }

    if (ok) {
        g_ctx->exhausted = false;   // reproducir a mano revive un motor agotado
        g_analyzer.reset();         // el VU/espectro no debe arrastrar la pista anterior
        g_reverb.clear();           // ni la cola de reverb de la anterior
        rebuild_ui_model();
        log_now_playing();
    } else {
        log_message(RETRO_LOG_WARN,
            "[aolib] no se pudo reproducir la pista %d de la lista.", visible_index + 1);
    }
    return ok;
}

// Ejecuta el botón enfocado del deck.
void ui_activate_deck_button() {
    if (!g_ctx) return;
    switch (static_cast<ui::DeckButton>(g_ui.deck_focus)) {
        case ui::DeckButton::PreviousTrack:
            ui_play_visible_track(g_ui.current_track - 1);
            break;
        case ui::DeckButton::PlayPause:
            // Pausa REAL: retro_run() deja de pedir audio al motor. No se
            // toca el motor ni se descarta su estado, así que reanudar
            // continúa donde estaba en vez de reiniciar.
            g_ui.playing = !g_ui.playing;
            log_message(RETRO_LOG_INFO, "[aolib] %s",
                        g_ui.playing ? "reanudado" : "pausado");
            break;
        case ui::DeckButton::FastForward:
            // El avance rápido es de MANTENER pulsado, no de un disparo: se
            // gestiona entero en apply_ui_input() (lectura continua del
            // mando) y en retro_run(). Que aquí no haya nada es
            // intencionado, no un hueco olvidado.
            break;
        case ui::DeckButton::NextTrack:
            ui_play_visible_track(g_ui.current_track + 1);
            break;
        case ui::DeckButton::Stop:
            // Detener = volver al principio y quedarse en pausa, como en
            // cualquier reproductor. Reutiliza restart_from_beginning(), la
            // misma función que usa retro_reset().
            if (restart_from_beginning()) {
                g_ctx->exhausted = false;
                g_analyzer.reset();
                g_reverb.clear();
                rebuild_ui_model();
            }
            g_ui.playing = false;
            break;
        case ui::DeckButton::Repeat: {
            // Cicla OFF -> ALL -> ONE. ALL se mapea a options.loop_infinite,
            // que es la opción de core que ya existe y que retro_run() ya
            // respeta -- el botón no introduce un segundo mecanismo
            // paralelo para lo mismo.
            const int next = (static_cast<int>(g_ui.repeat) + 1) % 3;
            g_ui.repeat = static_cast<ui::RepeatMode>(next);
            g_ui.repeat_when_not_all = g_ui.repeat;
            g_ctx->options.loop_infinite = (g_ui.repeat == ui::RepeatMode::All);
            log_message(RETRO_LOG_INFO, "[aolib] repetición: %s",
                        ui::repeat_mode_label(g_ui.repeat));
            break;
        }
        case ui::DeckButton::Volume:
            // A ALTERNA un modo explícito de ajuste (borde naranja =
            // activo); mientras está activo, izquierda/derecha cambian el
            // volumen y no mueven el foco. Tiene que ser un modo explícito:
            // si bastara con tener el FOCO aquí, las flechas quedarían
            // secuestradas por el volumen y no habría forma de salir del
            // botón con el mando.
            g_ui.volume_adjust_active = !g_ui.volume_adjust_active;
            log_message(RETRO_LOG_INFO, "[aolib] ajuste de volumen: %s",
                        g_ui.volume_adjust_active ? "activado" : "desactivado");
            break;
        case ui::DeckButton::Reverb:
            // Escribe en options.host_reverb_amount, la única fuente de
            // verdad; g_ui.reverb_enabled se recompone desde ahí en
            // refresh_ui_dynamic_state(). Encender y apagar es EXCLUSIVO de
            // este botón: el menú del frontend solo fija el nivel (ver
            // apply_core_options()).
            //
            // Al APAGARLO se vacía la cola: si no, al volver a encenderlo
            // sonaría de golpe el reverb de lo que sonaba hace unos
            // segundos.
            if (g_ctx->options.host_reverb_amount > 0) {
                g_ctx->options.host_reverb_last_on = g_ctx->options.host_reverb_amount;
                g_ctx->options.host_reverb_amount = 0;
                g_reverb.clear();
            } else {
                g_ctx->options.host_reverb_amount = g_ctx->options.host_reverb_last_on;
            }
            log_message(RETRO_LOG_INFO, "[aolib] reverb del reproductor: %s",
                        g_ctx->options.host_reverb_amount > 0 ? "activado" : "desactivado");
            break;
    }
}

// Lee el mando y aplica las acciones. Se llama una vez por retro_run(),
// antes de rendir audio, para que un cambio de pista tenga efecto en el
// mismo frame en que se pulsa.
void apply_ui_input() {
    if (input_poll_cb) input_poll_cb();
    if (!input_state_cb) return;

    const auto down = [](unsigned id) {
        return input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, id) != 0;
    };
    unsigned cur = 0;
    if (down(RETRO_DEVICE_ID_JOYPAD_UP))    cur |= ui::bits::kUp;
    if (down(RETRO_DEVICE_ID_JOYPAD_DOWN))  cur |= ui::bits::kDown;
    if (down(RETRO_DEVICE_ID_JOYPAD_LEFT))  cur |= ui::bits::kLeft;
    if (down(RETRO_DEVICE_ID_JOYPAD_RIGHT)) cur |= ui::bits::kRight;
    if (down(RETRO_DEVICE_ID_JOYPAD_A))     cur |= ui::bits::kA;
    if (down(RETRO_DEVICE_ID_JOYPAD_B))     cur |= ui::bits::kB;
    if (down(RETRO_DEVICE_ID_JOYPAD_L))     cur |= ui::bits::kL;
    if (down(RETRO_DEVICE_ID_JOYPAD_R))     cur |= ui::bits::kR;
    if (down(RETRO_DEVICE_ID_JOYPAD_START)) cur |= ui::bits::kStart;
    if (down(RETRO_DEVICE_ID_JOYPAD_L2))    cur |= ui::bits::kL2;
    if (down(RETRO_DEVICE_ID_JOYPAD_R2))    cur |= ui::bits::kR2;

    const unsigned pressed = g_input_edge.pressed(cur);

    // Avance rápido: CONTINUO, no de flanco. Se lee 'cur' (estado del mando
    // en este frame) y no 'pressed' (solo cierto en el frame en que el botón
    // pasa de suelto a pulsado).
    //
    // Va ANTES del 'return' de más abajo a propósito: ese 'return' corta la
    // función cuando no hay ningún flanco nuevo, que es lo que ocurre en
    // todos los frames intermedios de un botón mantenido.
    g_ui.fast_forward_active =
        (static_cast<ui::DeckButton>(g_ui.deck_focus) == ui::DeckButton::FastForward) &&
        ((cur & ui::bits::kA) != 0);

    // Repetición automática del cursor de la lista: ~400 ms de umbral y
    // luego un paso cada 4 frames (~15 pistas/s), rápido para listas largas
    // sin volverse imposible de frenar en las cortas.
    //
    // ANTES del 'return' de más abajo por el mismo motivo que el avance
    // rápido: si no, nunca vería un frame de "mantenido".
    {
        constexpr int kCursorRepeatDelayFrames    = 24; // 400 ms a 60 fps
        constexpr int kCursorRepeatIntervalFrames = 4;  // ~15 pasos/s

        const bool up_held   = (cur & ui::bits::kUp)   != 0;
        const bool down_held = (cur & ui::bits::kDown) != 0;
        // Las dos a la vez, o ninguna: no hay dirección que repetir, y no
        // se adivina cuál "gana".
        const int held_dir = (up_held == down_held) ? 0 : (up_held ? -1 : 1);

        if (held_dir == 0) {
            g_ui.cursor_repeat_dir = 0;
            g_ui.cursor_repeat_frames = 0;
        } else if (g_ui.cursor_repeat_dir != held_dir) {
            // Primer frame de esta dirección (o cambio sin soltar): arma el
            // contador. El paso de ESTE frame lo da el camino de flanco de
            // más abajo; moverlo aquí también duplicaría el primer paso.
            g_ui.cursor_repeat_dir = held_dir;
            g_ui.cursor_repeat_frames = 0;
        } else {
            ++g_ui.cursor_repeat_frames;
            const int since_delay = g_ui.cursor_repeat_frames - kCursorRepeatDelayFrames;
            if (since_delay >= 0 && since_delay % kCursorRepeatIntervalFrames == 0) {
                const int track_count = static_cast<int>(g_ui.tracks.size());
                g_ui.cursor = held_dir < 0
                    ? std::max(0, g_ui.cursor - 1)
                    : std::min(std::max(0, track_count - 1), g_ui.cursor + 1);
            }
        }
    }

    // Lo mismo para Izquierda/Derecha en el volumen, con los mismos
    // tiempos y por el mismo motivo antes del 'return'.
    //
    // SOLO actúa con 'volume_adjust_active' encendido: fuera de ese modo
    // esas flechas mueven el foco del deck, y repetirlo sin querer sería
    // justo el secuestro de flechas que el modo explícito evita. Al salir
    // del modo, el contador se reinicia.
    if (g_ui.volume_adjust_active) {
        constexpr int kVolumeRepeatDelayFrames    = 24; // 400 ms a 60 fps
        constexpr int kVolumeRepeatIntervalFrames = 4;  // ~15 pasos/s

        const bool left_held  = (cur & ui::bits::kLeft)  != 0;
        const bool right_held = (cur & ui::bits::kRight) != 0;
        // Las dos a la vez, o ninguna: no hay dirección que repetir.
        const int held_dir = (left_held == right_held) ? 0 : (left_held ? -1 : 1);

        if (held_dir == 0) {
            g_ui.volume_repeat_dir = 0;
            g_ui.volume_repeat_frames = 0;
        } else if (g_ui.volume_repeat_dir != held_dir) {
            // Primer frame de esta dirección: arma el contador. El paso de
            // ESTE frame lo da el camino de flanco de más abajo; tocarlo
            // aquí duplicaría el primer paso.
            g_ui.volume_repeat_dir = held_dir;
            g_ui.volume_repeat_frames = 0;
        } else {
            ++g_ui.volume_repeat_frames;
            const int since_delay = g_ui.volume_repeat_frames - kVolumeRepeatDelayFrames;
            if (since_delay >= 0 && since_delay % kVolumeRepeatIntervalFrames == 0) {
                g_ui.volume = held_dir < 0
                    ? std::max(0,   g_ui.volume - 5)
                    : std::min(100, g_ui.volume + 5);
            }
        }
    } else {
        g_ui.volume_repeat_dir = 0;
        g_ui.volume_repeat_frames = 0;
    }

    // L2/R2 ajustan volumen directamente, sin pasar por volume_adjust_active:
    // así no dejan el D-pad secuestrado después de soltar el gatillo, que es
    // justo lo que ese modo evita. Mismos tiempos de repetición que el resto
    // del deck. Mientras se mantiene cualquiera de los dos, el foco pasa a
    // Volume para que el botón se ilumine con el gatillo, igual que si se
    // hubiera navegado hasta él a mano.
    {
        constexpr int kTriggerVolRepeatDelayFrames    = 24; // 400 ms a 60 fps
        constexpr int kTriggerVolRepeatIntervalFrames = 4;  // ~15 pasos/s

        const bool l2_held = (cur & ui::bits::kL2) != 0;
        const bool r2_held = (cur & ui::bits::kR2) != 0;
        const int held_dir = (l2_held == r2_held) ? 0 : (l2_held ? -1 : 1);

        if (held_dir != 0) g_ui.deck_focus = static_cast<int>(ui::DeckButton::Volume);

        if (held_dir == 0) {
            g_ui.trigger_vol_repeat_dir = 0;
            g_ui.trigger_vol_repeat_frames = 0;
        } else if (g_ui.trigger_vol_repeat_dir != held_dir) {
            g_ui.trigger_vol_repeat_dir = held_dir;
            g_ui.trigger_vol_repeat_frames = 0;
        } else {
            ++g_ui.trigger_vol_repeat_frames;
            const int since_delay = g_ui.trigger_vol_repeat_frames - kTriggerVolRepeatDelayFrames;
            if (since_delay >= 0 && since_delay % kTriggerVolRepeatIntervalFrames == 0) {
                g_ui.volume = held_dir < 0
                    ? std::max(0,   g_ui.volume - 5)
                    : std::min(100, g_ui.volume + 5);
            }
        }
    }

    if (!pressed) return;

    const int track_count = static_cast<int>(g_ui.tracks.size());

    if (pressed & ui::bits::kUp)    g_ui.cursor = std::max(0, g_ui.cursor - 1);
    if (pressed & ui::bits::kDown)  g_ui.cursor = std::min(std::max(0, track_count - 1),
                                                           g_ui.cursor + 1);

    // Izquierda/derecha mueven el foco del deck SALVO con el modo de ajuste
    // de volumen activo (ver DeckButton::Volume), en cuyo caso ajustan el
    // volumen y el foco se queda quieto. Como ese modo hay que activarlo
    // con A, el foco puede recorrer los ocho botones libremente el resto
    // del tiempo.
    if (g_ui.volume_adjust_active) {
        if (pressed & ui::bits::kLeft)  g_ui.volume = std::max(0,   g_ui.volume - 5);
        if (pressed & ui::bits::kRight) g_ui.volume = std::min(100, g_ui.volume + 5);
    } else {
        if (pressed & ui::bits::kLeft)  g_ui.deck_focus = std::max(0, g_ui.deck_focus - 1);
        if (pressed & ui::bits::kRight) g_ui.deck_focus = std::min(7, g_ui.deck_focus + 1);
    }
    // L/R: pista anterior/siguiente, tal cual A sobre esos botones -- se
    // mueve el foco primero para que el botón se ilumine, y ya enfocado se
    // reutiliza ui_activate_deck_button() en vez de duplicar la lógica de
    // cambio de pista.
    if (pressed & ui::bits::kL) {
        g_ui.deck_focus = static_cast<int>(ui::DeckButton::PreviousTrack);
        ui_activate_deck_button();
    }
    if (pressed & ui::bits::kR) {
        g_ui.deck_focus = static_cast<int>(ui::DeckButton::NextTrack);
        ui_activate_deck_button();
    }
    // L2/R2: primer paso de volumen; el resto lo da la repetición de más
    // arriba. El foco a Volume ya lo puso ese mismo bloque.
    if (pressed & ui::bits::kL2) g_ui.volume = std::max(0,   g_ui.volume - 5);
    if (pressed & ui::bits::kR2) g_ui.volume = std::min(100, g_ui.volume + 5);

    if (pressed & ui::bits::kA)     ui_activate_deck_button();
    if (pressed & ui::bits::kB)     ui_play_visible_track(g_ui.cursor);
    if (pressed & ui::bits::kStart) {
        // Mismo botón que PlayPause: se enfoca primero para que se
        // ilumine, y se reutiliza ui_activate_deck_button() en vez de
        // repetir el toggle y el log aquí.
        g_ui.deck_focus = static_cast<int>(ui::DeckButton::PlayPause);
        ui_activate_deck_button();
    }
}

// Ganancia de la capa host: ningún motor se entera de que existe un control
// de volumen. Se aplica sobre el bloque ya rendido, justo antes de
// entregarlo al frontend.
//
// Multiplicación en int32 con desplazamiento, sin flotantes y sin
// posibilidad de desbordar: vol_q8 <= 256 y la muestra cabe en 16 bits,
// así que el producto cabe de sobra en int32 antes del >>8. A volumen 100
// el factor es exactamente 256 -> >>8 devuelve la muestra intacta, bit a
// bit: el camino por defecto no altera el audio en absoluto.
void apply_host_gain(int16_t* buf, std::size_t frames, int volume_0_100) {
    if (volume_0_100 >= 100) return;               // sin tocar nada
    if (volume_0_100 <= 0) {
        std::fill(buf, buf + frames * 2, static_cast<int16_t>(0));
        return;
    }
    const int32_t vol_q8 = (volume_0_100 * 256) / 100;
    for (std::size_t i = 0; i < frames * 2; ++i) {
        buf[i] = static_cast<int16_t>((static_cast<int32_t>(buf[i]) * vol_q8) >> 8);
    }
}

} // namespace

// ───────────────────────── Entorno y callbacks ─────────────────────────

RETRO_API void retro_set_environment(retro_environment_t cb) {
    environ_cb = cb;

    bool no_content = false; // requiere fichero siempre
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_content);

    // Adquisición del VFS. Sin él el core arranca igual, pero rechazará
    // cualquier fichero PSF/MiniPSF en retro_load_game().
    retro_vfs_interface_info vfs_info{};
    vfs_info.required_interface_version = 3; // necesitamos v3 (stat/opendir) a futuro
    if (cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_info) && vfs_info.iface) {
        g_vfs = std::make_unique<LibretroVFS>(vfs_info.iface);
    }

// Extensiones de libgme y libvgm: se aceptan en memoria
    // (need_fullpath = false); tanto gme_open_data() como
    // PlayerA/MemoryLoader_Init trabajan directamente sobre un buffer.
    // Extensiones PSF: exigen fichero completo en disco (need_fullpath =
    // true) porque aosdk resuelve los _libN relativos al directorio base.
    // .zip: también need_fullpath, porque lo abre el propio core con
    // minizip a través del VFS (zip_playlist.hpp).

    // MANTENER SINCRONIZADA con kExts (zip_playlist.hpp) y con los ficheros
    // .info de dist/: son listas independientes.
    static const retro_system_content_info_override overrides[] = {
        { "spc|nsf|nsfe|vgm|vgz|gbs|hes|kss|sap|ay|gym", false, false },
        { "psf|minipsf|psf2|minipsf2|ssf|minissf",       true,  false },
        { "zip",                                         true,  false },
        { nullptr, false, false }
    };
    cb(RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE, (void*)overrides);

    // Rangos y valores por defecto alineados con CoreOptions
    // (core_context.hpp).
    static const retro_variable options[] = {
        { "aolib_loop_infinite", "Loop forever at end of album; disabled|enabled" },
        { "aolib_default_fade_seconds", "Fade duration (s); 8|0|1|2|3|5|10" },
        { "aolib_spu_reverb_enabled",
          "SPU Reverb (PSF2 only); enabled|disabled" },
        // Solo el NIVEL del reverb del reproductor, nunca su encendido:
        // eso es exclusivo del botón REB del deck (ver
        // apply_core_options() para el porqué). Tres escalones fijos:
        // 1=35%, 2=50%, 3=65%.
        { "aolib_reverb_amount",
          "Player Reverb Amount; 1|2|3" },
        { nullptr, nullptr }
    };
    cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)options);

    // OBLIGATORIO: sin esta declaración el frontend asume 0RGB1555, que es
    // el default de Libretro, y la UI se dibuja con los colores cambiados.
    enum retro_pixel_format pixfmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &pixfmt)) {
        // Ningún frontend moderno rechaza XRGB8888, pero si ocurre hay que
        // decirlo: los colores saldrían mal y sin este aviso parecería un
        // bug de la paleta.
        log_message(RETRO_LOG_WARN,
            "[aolib] el frontend rechazó XRGB8888; la UI se verá con colores incorrectos.");
    }
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t) { /* usamos solo el batch */ }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

// ───────────────────────── Ciclo de vida ─────────────────────────

RETRO_API void retro_init(void) {
    // stdout en modo línea a línea. psx_hw.c usa printf() para sus trazas
    // opcionales de depuración, y sin esto, si hay que matar el proceso a la
    // fuerza, el búfer (full-buffered al no ser una terminal) se pierde
    // entero justo cuando interesa leerlo. Inocuo en producción: cambia
    // CUÁNDO se vacía el búfer, no qué se escribe.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    retro_log_callback log_iface{};
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log_iface))
        log_cb = log_iface.log;

    if (g_vfs && g_vfs->is_valid())
        log_message(RETRO_LOG_INFO, "[aolib] VFS interface adquirida.");
    else
        log_message(RETRO_LOG_WARN,
            "[aolib] Sin interfaz VFS: los formatos PSF quedarán deshabilitados. "
            "libgme seguirá funcionando desde memoria.");

    bool dupe_supported = false;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &dupe_supported))
        g_can_dupe = dupe_supported;

    g_framebuffer.assign(static_cast<std::size_t>(kScreenWidth) * kScreenHeight, 0u);
}

RETRO_API void retro_deinit(void) {
    g_ctx.reset();
    g_vfs.reset();
    g_framebuffer.clear();
    g_framebuffer.shrink_to_fit();
}

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }

RETRO_API void retro_get_system_info(struct retro_system_info* info) {
    std::memset(info, 0, sizeof(*info));
    info->library_name     = "Aolib";
    // Mantener sincronizado con display_version en dist/aolib_libretro.info
    // y dist-windows/aolib_libretro.info.
    info->library_version  = "1.1.1";
    info->valid_extensions = "spc|nsf|nsfe|vgm|vgz|gbs|hes|kss|sap|ay|gym|psf|minipsf|psf2|minipsf2|ssf|minissf|zip";
    info->need_fullpath    = true;  // valor por defecto conservador; SET_CONTENT_INFO_OVERRIDE lo refina por extensión
    // true: el core abre los .zip él mismo con minizip (zip_playlist.hpp),
    // como el core oficial libretro-gme -- si RetroArch los extrajera por
    // su cuenta antes de pasárnoslos, no podríamos enumerar el resto de
    // ficheros del archivo ni construir la lista de reproducción plana.
    info->block_extract    = true;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info* info) {
    // 60 fps SIEMPRE, con independencia del VBlank interno del motor (50 Hz
    // en rips PAL): el fps de Libretro solo fija cuántas muestras se piden
    // por iteración de retro_run() (735 a 44100/60), no es el reloj de la
    // emulación.
    info->timing.fps = 60.0;
    info->timing.sample_rate = 44100.0;

    info->geometry.base_width   = kScreenWidth;
    info->geometry.base_height  = kScreenHeight;
    info->geometry.max_width    = kScreenWidth;
    info->geometry.max_height   = kScreenHeight;
    info->geometry.aspect_ratio = 4.0f / 3.0f;
}

RETRO_API void retro_set_controller_port_device(unsigned, unsigned) {}
// El frontend puede llamar a retro_reset() sin contenido cargado o después
// de retro_unload_game(): comprobar g_ctx no es opcional.
RETRO_API void retro_reset(void) {
    if (!g_ctx || !g_ctx->engine) return; // nada cargado, nada que reiniciar

    if (restart_from_beginning()) {
        g_ctx->exhausted = false; // única vía para revivir un motor agotado
        g_analyzer.reset();
        g_reverb.clear();   // sin cola, como en una carga nueva
    } else {
        log_message(RETRO_LOG_ERROR,
            "[aolib] retro_reset(): el reinicio falló (select_track(0) o recarga de .zip); "
            "el motor queda agotado.");
        g_ctx->exhausted = true;
    }
}

// ───────────────────────── Carga / descarga ─────────────────────────

// Cuerpo real de retro_load_game(), en una función aparte para que el punto
// de entrada extern "C" pueda envolverlo en try/catch sin duplicar lógica:
// una excepción que cruce esa frontera llama a terminate() y se lleva por
// delante al frontend entero.
static bool retro_load_game_impl(const struct retro_game_info* game) {
    if (!game) return false;

    g_ctx = std::make_unique<CoreContext>(); // estado nuevo, siempre desde cero

    // 'g_ui' es un global de PROCESO y sobrevive a la carga, al contrario
    // que g_ctx. repeat_when_not_all hay que resetearlo A MANO aquí:
    // refresh_ui_dynamic_state() lo usa para recomponer g_ui.repeat cuando
    // loop_infinite está apagado, así que sin este reset el modo de
    // repetición elegido en una pista anterior reaparece al cargar
    // contenido nuevo. Contenido nuevo, elección nueva.
    g_ui.repeat_when_not_all = ui::RepeatMode::Off;

    // Opciones leídas ANTES de construir ningún motor: default_fade_seconds
    // y loop_infinite se le pasan a construct_engine_for(), así que tienen
    // que traer ya lo configurado en el frontend.
    apply_core_options();

    log_message(RETRO_LOG_INFO, "[aolib] retro_load_game: %s",
                game->path ? game->path : "(sin path, contenido en memoria)");

    const bool is_zip = game->path && has_suffix(game->path, ".zip");

    if (is_zip) {
        // El CORE abre el .zip él mismo (ver zip_playlist.hpp), por eso
        // .zip se declara con need_fullpath=true.
        if (!g_vfs || !g_vfs->is_valid()) {
            log_message(RETRO_LOG_ERROR,
                "[aolib] .zip sin VFS disponible, no se puede enumerar sin fopen real.");
            g_ctx.reset();
            return false;
        }

        // enumerate_zip() no conoce RETRO_LOG_*: se le inyecta cómo emitir
        // sus avisos de entrada o presupuesto rechazados.
        auto zip_warn = [](const std::string& msg) { log_message(RETRO_LOG_WARN, "%s", msg.c_str()); };
        if (!enumerate_zip(game->path, *g_vfs, g_ctx->zip_entries, zip_warn) || g_ctx->zip_entries.empty()) {
            log_message(RETRO_LOG_ERROR,
                "[aolib] %s: no se encontró ningún fichero soportado dentro del .zip.",
                game->path);
            g_ctx.reset();
            return false;
        }

        log_message(RETRO_LOG_INFO, "[aolib] %s: %zu ficheros soportados encontrados en el .zip.",
                    game->path, g_ctx->zip_entries.size());

        // TODOS los sondeos de duración y título ANTES de construir el
        // motor real; el orden es obligatorio, ver la cabecera de la
        // función.
        precompute_zip_track_durations();

        if (load_zip_entry_from(0) < 0) {
            log_message(RETRO_LOG_ERROR,
                "[aolib] Ninguna de las entradas del .zip pudo cargarse.");
            g_ctx.reset();
            return false;
        }
        apply_core_options(); // propaga spu_reverb_enabled al motor recién construido
        rebuild_ui_model();
        log_now_playing();
        return true;
    }

    // Fichero suelto: 'uri' real si el frontend nos dio need_fullpath=true
    // para su extensión (PSF), o nullptr + game->data si no (libgme).
    const std::string display_name = game->path ? game->path : "(memoria)";
    auto engine = construct_engine_for(
        game->path, display_name,
        static_cast<const uint8_t*>(game->data), static_cast<std::size_t>(game->size),
        g_ctx->options.default_fade_seconds, g_ctx->options.loop_infinite);

    if (!engine) {
        g_ctx.reset();
        return false;
    }
    g_ctx->engine = std::move(engine);
    apply_core_options(); // propaga spu_reverb_enabled al motor recién construido
    rebuild_ui_model();
    log_now_playing(); // F7b (Lote 3.2)
    return true;
}

// Red de seguridad de última línea, ADEMÁS de los límites y el try/catch de
// enumerate_zip(), no en su lugar: garantiza que ninguna excepción, de esta
// ruta o de otra que se añada luego, cruce la frontera extern "C" y mate
// RetroArch entero vía terminate() en vez de solo fallar la carga.
RETRO_API bool retro_load_game(const struct retro_game_info* game) {
    try {
        // Contenido nuevo, cola nueva. Se limpia también en
        // retro_unload_game(), pero un frontend puede llamar a
        // retro_load_game() sin descargar antes, así que hacen falta los
        // dos extremos.
        g_reverb.clear();
        return retro_load_game_impl(game);
    } catch (const std::exception& e) {
        log_message(RETRO_LOG_ERROR,
            "[aolib] retro_load_game(): excepción no esperada: %s", e.what());
        g_ctx.reset();
        return false;
    } catch (...) {
        log_message(RETRO_LOG_ERROR,
            "[aolib] retro_load_game(): excepción no esperada (tipo desconocido).");
        g_ctx.reset();
        return false;
    }
}

RETRO_API bool retro_load_game_special(unsigned, const struct retro_game_info*, size_t) {
    return false;
}

RETRO_API void retro_unload_game(void) {
    // El resumen se emite ANTES de soltar el contexto, y la contabilidad se
    // reinicia: cada contenido cargado tiene su propia sesión de medida, y
    // así los números son comparables entre álbumes.
    log_audio_diag_summary("contenido descargado");
    g_diag = AudioDiag{};

    // La cola del reverb es estado de PROCESO: g_reverb vive fuera de
    // CoreContext, así que soltar el contexto NO la vacía y la cola de un
    // contenido sobreviviría al siguiente. No es teórico -- con el reverb al
    // máximo, dos cargas del MISMO fichero producían audio distinto. La ruta
    // del .zip ya quedaba cubierta por load_zip_entry_from(); la de fichero
    // suelto no pasa por ahí.
    g_reverb.clear();

    g_ctx.reset(); // RAII: motor y buffers se liberan solos.
}

RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

// ───────────────────────── Bucle principal ─────────────────────────

RETRO_API void retro_run(void) {
    if (input_poll_cb) input_poll_cb();

    if (!g_ctx) return;

    const double t_frame_start = now_us();

    // Releer las variables en cada iteración serían varias llamadas a
    // environ_cb por frame para nada; GET_VARIABLE_UPDATE dice si algo
    // cambió desde la última consulta.
    bool options_updated = false;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &options_updated) &&
        options_updated) {
        apply_core_options();
    }

    // Entrada del mando ANTES de rendir audio, para que un cambio de pista
    // tenga efecto en el mismo frame en que se pulsa el botón.
    apply_ui_input();

    // ═════════════════════ AVANCE RÁPIDO ═════════════════════
    //
    // RENDIR MAS Y EMITIR LO MISMO: se piden kFfExtraBlocks bloques
    // adicionales y se DESCARTAN, de modo que el bloque que emite el camino
    // normal de mas abajo ya esta cuatro bloques mas adelante en la cancion.
    // El frontend recibe los 735 frames de siempre (video a 60 fps, nada que
    // reajustar) y el motor ha avanzado 4x. Se oye a trozos, en ventanas de
    // 1/4, igual que el avance rapido de un reproductor de CD.
    //
    // NO sirve emitir mas muestras por llamada: el frontend consume 44100
    // muestras por segundo real, sean 735 o 1470 por llamada, asi que
    // entregar el doble solo hace que llame a retro_run() la mitad de veces
    // y la cancion sigue a velocidad 1x, con el video a 30 fps. Lo unico que
    // se aceleraria es 'frames_rendered', es decir el reloj y la barra de la
    // UI: la posicion correria al doble sin que cambie lo que se oye.
    //
    // Cada bloque extra necesita SU tick de vblank (on_video_frame() ->
    // psf_frame() -> psx_irq_set(1)), que es la interrupcion que usa el
    // secuenciador de musica de la PS1. El invariante no es "un tick por
    // retro_run()" sino "un tick por cada 735 muestras rendidas"; sin el, el
    // secuenciador avanza a distinto ritmo que las muestras.
    if (g_ui.fast_forward_active && g_ui.playing && !g_ctx->exhausted && g_ctx->engine) {
        for (int i = 0; i < kFfExtraBlocks; ++i) {
            if (g_ctx->engine->end_of_track()) break;  // deja que el camino
                                                        // normal decida que
                                                        // hacer al acabar
            g_ctx->engine->render(g_ctx->scratch_output_.data(), kFramesPerRun);
            g_ctx->engine->on_video_frame();   // un tick por bloque: ver arriba
            g_ctx->frames_rendered += kFramesPerRun;
        }
    }

    // Render DIRECTO: el motor escribe en 'scratch_output_', el mismo buffer
    // que se entrega a audio_batch_cb() unas líneas más abajo. Una escritura,
    // cero copias intermedias.
    //
    // Con 'exhausted' puesto NUNCA se vuelve a llamar a
    // render()/on_video_frame(), aunque el motor siga vivo (ver
    // core_context.hpp). Y 'g_ui.playing' es una pausa REAL: no se pide
    // audio y se entrega silencio, sin tocar el estado del motor, así que
    // reanudar continúa donde estaba.
    std::size_t real_frames = 0;
    if (g_ui.playing && !g_ctx->exhausted && g_ctx->engine) {
        real_frames = g_ctx->engine->render(g_ctx->scratch_output_.data(), kFramesPerRun);
        if (real_frames < kFramesPerRun) {
            // Motor devolvió menos de lo pedido (fin de pista o error):
            // el resto del scratch buffer puede contener basura de la
            // llamada anterior si el motor no lo tocó; se rellena con
            // silencio explícitamente antes de entregarlo al frontend.
            std::fill(g_ctx->scratch_output_.begin() + real_frames * 2,
                      g_ctx->scratch_output_.end(), 0);
        }
        g_ctx->engine->on_video_frame();
    } else {
        // Sin motor, en pausa o agotado: silencio EXPLÍCITO.
        // 'scratch_output_' se reutiliza sin resetear entre llamadas, así
        // que todavía lleva el bloque de la iteración anterior.
        std::fill(g_ctx->scratch_output_.begin(), g_ctx->scratch_output_.end(), 0);
    }

    // 'real_frames < kFramesPerRun' es la señal genuina de que el motor
    // produjo menos audio del pedido dentro del presupuesto de este
    // retro_run().
    if (real_frames < kFramesPerRun && g_ui.playing && !g_ctx->exhausted && g_ctx->engine) {
        ++g_diag.underrun_events;
        g_diag.underrun_frames += (kFramesPerRun - real_frames);
        if (g_diag.warns_emitted < 3) {
            ++g_diag.warns_emitted;
            log_message(RETRO_LOG_WARN,
                "[aolib] UNDERRUN: solo %zu de %zu frames disponibles; el resto se ha "
                "entregado como silencio. (aviso %d de 3; el total va en el resumen al "
                "descargar contenido)",
                real_frames, static_cast<std::size_t>(kFramesPerRun), g_diag.warns_emitted);
        }
    }

    // Reverb de la capa host, ANTES de la ganancia: el volumen debe gobernar
    // la mezcla completa (seco + cola). Al revés, subir el volumen dejaría
    // la cola atrás.
    //
    // BYPASS ESTRUCTURAL: con el efecto apagado no se llama a process() en
    // absoluto, así que no hay multiplicación por 1,0 ni conversión
    // float<->int16 de ida y vuelta -- el PCM ni siquiera entra en el DSP.
    // Es lo que garantiza el bit-perfect (tests/f13_reverb_regression.cpp).
    if (g_ctx->options.host_reverb_amount > 0) {
        g_reverb.process(g_ctx->scratch_output_.data(), kFramesPerRun,
                         static_cast<float>(g_ctx->options.host_reverb_amount) / 100.0f);
    }

    // Ganancia de la capa host (a volumen 100 no toca ni un bit) y análisis
    // de la señal REAL que se va a entregar. El orden importa: se analiza
    // DESPUÉS de la ganancia, porque el VU debe mostrar lo que se oye, no lo
    // que produjo el motor.
    apply_host_gain(g_ctx->scratch_output_.data(), kFramesPerRun, g_ui.volume);
    g_analyzer.feed(g_ctx->scratch_output_.data(), kFramesPerRun);

    if (audio_batch_cb)
        audio_batch_cb(g_ctx->scratch_output_.data(), kFramesPerRun);

    // En pausa el reloj no avanza: frames_rendered es la posición dentro de
    // la pista, y la lee la barra de progreso.
    if (g_ui.playing) g_ctx->frames_rendered += kFramesPerRun;

    // Modo de repetición ONE: al terminar, la pista se reinicia sin avanzar
    // la lista. Con una bandera y NO con un 'return' temprano: la emisión de
    // vídeo está al final de retro_run(), así que salir aquí congelaría la
    // UI un fotograma cada vez que una pista da la vuelta.
    bool repeat_one_handled = false;
    if (g_ui.repeat == ui::RepeatMode::One && g_ui.playing &&
        !g_ctx->exhausted && g_ctx->engine && g_ctx->engine->end_of_track()) {
        if (g_ctx->engine->select_track(g_ctx->current_track_index)) {
            g_ctx->frames_rendered = 0;
            g_analyzer.reset();
            g_reverb.clear();
            repeat_one_handled = true;
        } else {
            log_message(RETRO_LOG_WARN,
                "[aolib] repetición ONE: select_track(%u) falló; se pasa al avance normal.",
                g_ctx->current_track_index);
        }
    }

    if (!repeat_one_handled && !g_ctx->exhausted && g_ctx->engine &&
        g_ctx->engine->end_of_track()) {
        const unsigned track_count = g_ctx->engine->track_count();
        const unsigned next_index = g_ctx->current_track_index + 1;

        if (next_index < track_count) {
            // Subsongs dentro del MISMO fichero (NSF, GBS, KSS, SAP, AY,
            // HES vía libgme): avanza antes de tocar la lista de zip.
            if (g_ctx->engine->select_track(next_index)) {
                g_ctx->current_track_index = next_index;
                g_ctx->frames_rendered = 0;
                log_message(RETRO_LOG_INFO, "[aolib] Avanzando a pista %u/%u",
                            next_index + 1, track_count);
                rebuild_ui_model();
                log_now_playing();
            } else {
                // Esta rama NO puede faltar: si select_track() falla y no se
                // marca 'exhausted', end_of_track() sigue siendo true y todo
                // este bloque se reintenta en cada retro_run(),
                // indefinidamente y en silencio.
                log_message(RETRO_LOG_ERROR,
                    "[aolib] select_track(%u) falló; no queda nada más que hacer con este motor.",
                    next_index);
                g_ctx->exhausted = true;
            }
        } else if (!g_ctx->zip_entries.empty() &&
                   g_ctx->zip_entry_index + 1 < g_ctx->zip_entries.size()) {
            // Se agotaron las subsongs del fichero actual DENTRO del zip:
            // avanza al siguiente fichero del archivo, como una lista de
            // reproducción (zip_playlist.hpp). load_zip_entry_from() ya
            // reinicia los contadores y salta cualquier entrada rota, así que
            // no hay que repetirlo aquí.
            const long loaded = load_zip_entry_from(g_ctx->zip_entry_index + 1);
            if (loaded >= 0) {
                log_message(RETRO_LOG_INFO, "[aolib] Avanzando a %s (%zu/%zu del .zip)",
                            g_ctx->zip_entries[static_cast<std::size_t>(loaded)].name.c_str(),
                            static_cast<std::size_t>(loaded) + 1, g_ctx->zip_entries.size());
                rebuild_ui_model();
                log_now_playing();
            } else {
                // Ninguna entrada a partir de la actual pudo cargarse
                // (todas fallidas en cascada): no queda nada más que
                // hacer. Ver core_context.hpp::exhausted.
                log_message(RETRO_LOG_INFO,
                    "[aolib] agotado: ninguna entrada del .zip a partir de %zu pudo cargarse.", g_ctx->zip_entry_index + 1);
                g_ctx->exhausted = true;
            }
        } else if (g_ctx->options.loop_infinite) {
            // No quedan más subsongs ni más ficheros del zip (o no había
            // zip): si el bucle infinito está activo, se reinicia desde
            // el principio de TODO -- primer fichero del zip si lo hay,
            // o la pista 0 del fichero suelto. select_track(0) en
            // PsfEngine se reinterpreta como reinicio de la pista actual
            // (ver psf_engine.hpp); en GmeEngine reinicia el fichero
            // completo desde su primer subsong.
            if (!restart_from_beginning()) {
                // Mismo motivo que en la rama de subsongs: sin marcar
                // 'exhausted' al fallar, este bloque se reintentaría en cada
                // retro_run() para siempre.
                log_message(RETRO_LOG_ERROR,
                    "[aolib] agotado: loop_infinite activo pero el reinicio "
                    "desde el principio falló (select_track(0) o recarga de .zip).");
                g_ctx->exhausted = true;
            } else {
                rebuild_ui_model();
                log_now_playing();
            }
        } else {
            // No hay más pistas, ni más ficheros del zip, y el bucle está
            // desactivado: no queda NADA más que hacer con este motor.
            //
            // Marcar 'exhausted' es lo que hace que retro_run() deje de
            // tocarlo. "No hacer nada" no puede ser seguir llamando a
            // render() confiando en que un motor agotado devuelve silencio
            // barato: en PSF2 no lo es (ver core_context.hpp::exhausted).
            log_message(RETRO_LOG_INFO,
                "[aolib] agotado: sin más pistas/subsongs ni entradas del .zip, "
                "loop_infinite desactivado. zip_entry_index=%zu de %zu, current_track_index=%u/%u",
                g_ctx->zip_entry_index, g_ctx->zip_entries.size(),
                g_ctx->current_track_index, track_count);
            g_ctx->exhausted = true;
        }
    }

    // NO se puede usar GET_CAN_DUPE para enviar nullptr y ahorrarse el
    // frame: la pantalla cambia en cada iteración (VU, espectro, reloj,
    // barra), así que duplicar el frame anterior congelaría la UI.
    // g_can_dupe se sigue consultando en retro_load_game porque su valor
    // sigue siendo información útil sobre
    // el frontend, pero ya no gobierna esta rama.
    if (video_cb && !g_framebuffer.empty()) {
        refresh_ui_dynamic_state();
        ui::render(g_framebuffer.data(), g_ui);
        video_cb(g_framebuffer.data(), kScreenWidth, kScreenHeight,
                 kScreenWidth * sizeof(uint32_t));
    }

    // Cierre de la contabilidad del frame. Se mide el retro_run() COMPLETO
    // -- motor, ganancia, FFT y dibujado de la UI -- porque es el total lo
    // que compite con el presupuesto de 16,667 ms.
    {
        const double us = now_us() - t_frame_start;
        ++g_diag.frames_run;
        g_diag.total_us += us;
        if (us > g_diag.worst_us) g_diag.worst_us = us;
        if (us > kFrameBudgetUs) ++g_diag.over_budget;
    }
}

// ───────────────────────── Savestates (no soportados) ─────────────────────────

RETRO_API size_t retro_serialize_size(void) { return 0; }
RETRO_API bool   retro_serialize(void*, size_t) { return false; }
RETRO_API bool   retro_unserialize(const void*, size_t) { return false; }

RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned, bool, const char*) {}

RETRO_API void* retro_get_memory_data(unsigned) { return nullptr; }
RETRO_API size_t retro_get_memory_size(unsigned) { return 0; }
