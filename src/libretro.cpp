// libretro.cpp — punto de entrada del core: las 25 funciones retro_* y
// nada más.
//
// Emite siempre 44100 Hz y 60 fps. Toda la E/S de fichero pasa por el VFS
// de Libretro (vfs_bridge.hpp); no se usa fopen ni equivalentes.
//
// EL RESTO DEL CORE, POR CAPAS. Esto era un solo fichero de 2.485 líneas
// con todo dentro de un namespace anónimo, y encontrar algo en él exigía
// saber ya dónde estaba. Cada pieza vive ahora en su cabecera, en orden de
// dependencia y sin ciclos:
//
//   core_globals.hpp      estado de PROCESO: callbacks del frontend,
//                         contexto cargado, modelo de UI, contabilidad de
//                         audio, log.
//   engine_dispatch.hpp   qué motor abre cada fichero (la cadena de
//                         candidatos y las reglas que la acotan).
//   archive_playback.hpp  .zip/.chd: entrada activa, prefetch por frames,
//                         sondeo de duraciones, cambio de pista.
//   core_options.hpp      lectura de las opciones del frontend.
//   ui_glue.hpp           puente hacia la interfaz de 320x240.
//
// Son cabeceras header-only como el resto de src/, y siguen siendo
// INTERNAS: se compilan en una única unidad de traducción, la de este
// fichero. El namespace anónimo se cambió por uno con nombre, 'aolib',
// porque un namespace anónimo dentro de una cabecera es exactamente el
// tipo de cosa que un día se incluye desde dos sitios y nadie se entera.
//
// Dentro, las definiciones son 'static', no 'inline', y eso está MEDIDO,
// no elegido por gusto. Con 'inline' cada función pasa a tener enlace
// externo y el compilador está obligado a dejar una copia fuera de línea
// en su propio comdat; en PE-COFF --gc-sections apenas borra nada (0,6 %
// medido, ver LEEME), así que esas copias se quedan dentro de la .dll:
//
//     static:  2.680.832 bytes   <- idéntico al fichero único de antes
//     inline:  2.712.576 bytes   (+31.744, +1,2 %)
//
// Mismo árbol, mismas flags, `make clean` de por medio en las dos
// medidas. 'static' devuelve el enlace interno que tenía el namespace
// anónimo, que es justo lo que este reparto NO quería cambiar.

#include <exception>

#include "libretro.h"
#include "core_globals.hpp"
#include "engine_dispatch.hpp"
#include "archive_playback.hpp"
#include "core_options.hpp"
#include "ui_glue.hpp"

// Los cuerpos de las funciones retro_* son los de siempre y hablan de
// g_ctx, g_ui, load_zip_entry_from()... sin calificar. El 'using' mantiene
// eso tal cual: la alternativa era anteponer 'aolib::' unas doscientas
// veces sin ganar nada.
using namespace aolib;


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
        // Módulos tracker: libxmp-lite carga desde memoria
        // (xmp_load_module_from_memory) y, al ser la variante CORE_PLAYER,
        // no abre ficheros de instrumento externos. No necesita fullpath.
        { "mod|s3m|xm|it",                               false, false },
        { "psf|minipsf|psf2|minipsf2|ssf|minissf",       true,  false },
#ifdef AOLIB_WITH_VGMSTREAM
        // need_fullpath = false: VgmstreamEngine acepta el buffer directo
        // (es como llegan las entradas de .zip) y cae al VFS por ruta
        // cuando no hay contenido en memoria. La cadena se construye desde
        // vgmstream_extensions.hpp, no se escribe a mano.
        { vgmstream_ext::pipe_separated().c_str(),        false, false },
#endif
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
        // Panorama por defecto de los módulos (XMP_PLAYER_DEFPAN). 100 es
        // el panorama Amiga duro que trae libxmp de fábrica: en
        // auriculares parte la mezcla en dos mitades. 50 es el valor que
        // recomiendan las notas de libxmp 4.7.1.
        { "aolib_xmp_stereo_separation",
          "Module stereo separation (MOD/S3M/XM/IT); 50|0|25|75|100" },
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

#ifdef AOLIB_WITH_PSF
    // Registrado UNA vez, no en cada carga: log_message() ya se convierte
    // en no-op si log_cb sigue sin asignar, así que no hace falta esperar
    // a nada más. Sin esto, un "_lib" sin resolver (típico: un .psf/.psf2
    // suelto sin su fichero compartido al lado) solo deja "psf_start()
    // falló" en el log, sin decir qué archivo faltaba.
    AosdkLibResolver::set_log([](const std::string& msg) {
        log_message(RETRO_LOG_WARN, "%s", msg.c_str());
    });
#endif

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
    info->library_version  = "1.4.0";
    // Se construye una vez y se conserva: RETRO_API no copia la cadena, y
    // devolver un temporal dejaría al frontend con un puntero colgante.
    static const std::string kValidExtensions = [] {
        std::string base = "spc|nsf|nsfe|vgm|vgz|gbs|hes|kss|sap|ay|gym|mod|s3m|xm|it|"
                           "psf|minipsf|psf2|minipsf2|ssf|minissf|zip";
#ifdef AOLIB_WITH_CHD
        base += "|chd";
#endif
#ifdef AOLIB_WITH_VGMSTREAM
        base += "|" + vgmstream_ext::pipe_separated();
#endif
        return base;
    }();
    info->valid_extensions = kValidExtensions.c_str();
    info->need_fullpath    = true;  // valor por defecto conservador; SET_CONTENT_INFO_OVERRIDE lo refina por extensión
    // true: el core abre los .zip/.chd él mismo (minizip vía
    // zip_playlist.hpp, libchdr vía chd_playlist.hpp), como el core oficial
    // libretro-gme -- si RetroArch los extrajera por su cuenta antes de
    // pasárnoslos, no podríamos enumerar el resto de ficheros del archivo ni
    // construir la lista de reproducción plana.
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
#ifdef AOLIB_WITH_CHD
    const bool is_chd = game->path && has_suffix(game->path, ".chd");
#endif


    // El cuerpo vive en archive_playback.hpp, con el resto de la logica
    // de contenedores: libretro.cpp son SOLO los puntos de entrada.
#ifdef AOLIB_WITH_CHD
    if (is_chd) {
        if (!load_chd_content(game->path)) { g_ctx.reset(); return false; }
        rebuild_ui_model();
        return true;
    }
#endif

    if (is_zip) {
        // El CORE abre el .zip él mismo (ver zip_playlist.hpp), por eso la
        // extensión se declara con need_fullpath=true: si RetroArch lo
        // extrajera por su cuenta antes de pasárnoslo no podríamos enumerar
        // el resto de ficheros del archivo ni construir la lista plana.
        if (!g_vfs || !g_vfs->is_valid()) {
            log_message(RETRO_LOG_ERROR,
                "[aolib] .zip sin VFS disponible, no se puede enumerar sin fopen real.");
            g_ctx.reset();
            return false;
        }

        // enumerate_zip() no conoce RETRO_LOG_*: se le inyecta cómo emitir
        // sus avisos de entrada o presupuesto rechazados.
        auto archive_warn = [](const std::string& msg) { archive_warn_log(msg); };
        g_ctx->archive_path = game->path;
        const bool enumerated =
            enumerate_zip(game->path, *g_vfs, g_ctx->zip_entries, archive_warn);
        if (!enumerated || g_ctx->zip_entries.empty()) {
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
        g_ctx->options.default_fade_seconds, g_ctx->options.loop_infinite,
        g_ctx->options.xmp_stereo_separation);

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
    // Acumulado de callbacks ANTES de este frame, para poder restar al
    // cierre lo que este frame gastó bloqueado en el frontend.
    const double prev_cb_us = g_diag.us_audiocb + g_diag.us_videocb;

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

    double t_mark = now_us();
    g_diag.us_input += t_mark - t_frame_start;

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

    {
        const double t = now_us();
        const double d = t - t_mark;
        g_diag.us_engine += d;
        if (d > g_diag.worst_engine_us) g_diag.worst_engine_us = d;
        t_mark = t;
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

    {
        const double t = now_us();
        g_diag.us_dsp += t - t_mark;
        t_mark = t;
    }

    // audio_batch_cb() se mide APARTE: con el sync de audio del frontend
    // activo bloquea hasta que el driver drena, y esa espera no es coste
    // del core (ver AudioDiag).
    if (audio_batch_cb)
        audio_batch_cb(g_ctx->scratch_output_.data(), kFramesPerRun);

    {
        const double t = now_us();
        g_diag.us_audiocb += t - t_mark;
        t_mark = t;
    }

    // Inflar por adelantado la entrada siguiente, con el audio de este frame
    // YA entregado. Va aquí y no antes de audio_batch_cb() para no retrasar
    // la entrega: el frontend marca el ritmo bloqueando en esa llamada, así
    // que el trabajo puesto después ocupa holgura que si no se iría en el
    // vsync.
    advance_zip_prefetch();
    advance_chd_prefetch();

    // Y las duraciones que falten, con menos presupuesto: tener lista la
    // pista siguiente manda sobre rellenar una columna de la lista.
    advance_duration_probe();

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
    {
        const double t = now_us();
        g_diag.us_advance += t - t_mark;
        t_mark = t;
    }

    if (video_cb && !g_framebuffer.empty()) {
        refresh_ui_dynamic_state();
        ui::render(g_framebuffer.data(), g_ui);

        {
            const double t = now_us();
            const double d = t - t_mark;
            g_diag.us_ui += d;
            if (d > g_diag.worst_ui_us) g_diag.worst_ui_us = d;
            t_mark = t;
        }

        // video_cb() se mide APARTE por el mismo motivo que audio_batch_cb():
        // con vsync el frontend puede bloquear aquí.
        video_cb(g_framebuffer.data(), kScreenWidth, kScreenHeight,
                 kScreenWidth * sizeof(uint32_t));

        g_diag.us_videocb += now_us() - t_mark;
    }

    // Cierre de la contabilidad del frame. 'total_us' es el retro_run()
    // completo, callbacks incluidos, porque es lo que se compara con el
    // reloj de pared. Pero 'over_budget' cuenta SOLO el trabajo del core:
    // audio_batch_cb() y video_cb() bloquean a propósito con sync o vsync
    // activos, así que contarlos aquí hacía que un core ocioso reportara
    // 40% de frames "por encima del presupuesto" -- medido en RetroArch:
    // video_cb se lleva 16.140 us de los 16.638 de media.
    {
        const double us = now_us() - t_frame_start;
        ++g_diag.frames_run;
        g_diag.total_us += us;
        if (us > g_diag.worst_us) g_diag.worst_us = us;
        const double core_us = us - (g_diag.us_audiocb + g_diag.us_videocb - prev_cb_us);
        if (core_us > kFrameBudgetUs) ++g_diag.over_budget;
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
