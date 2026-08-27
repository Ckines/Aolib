// core_globals.hpp — el estado de PROCESO del core y las utilidades que
// lo miran: los callbacks del frontend, el contexto cargado, el modelo de
// la UI, la contabilidad de audio y el log.
//
// Todo lo de aquí sobrevive a retro_unload_game() a propósito. Lo que
// pertenece al contenido vive en CoreContext (core_context.hpp) y se
// destruye entero con él; lo que está en este fichero es lo que NO debe
// resetearse al cambiar de álbum -- volumen, modo de repetición, buffers
// de la UI y del reverb, que se reservan una vez y nunca dentro de
// retro_run().
//
// Salió de libretro.cpp al partirlo: ver la cabecera de ese fichero.

#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "libretro.h"
#include "core_context.hpp"
#include "vfs_bridge.hpp"
#include "dsp/reverb.hpp"
#include "ui/audio_analyzer.hpp"
#include "ui/ui_model.hpp"
#include "ui/ui_screen.hpp"

namespace aolib {
static retro_environment_t        environ_cb  = nullptr;
static retro_video_refresh_t      video_cb    = nullptr;
static retro_audio_sample_batch_t audio_batch_cb = nullptr;
static retro_input_poll_t         input_poll_cb  = nullptr;
static retro_log_printf_t         log_cb      = nullptr;

static std::unique_ptr<CoreContext> g_ctx;
static std::unique_ptr<LibretroVFS> g_vfs;
static NullVFSBridge g_null_vfs; // referencia inerte cuando no hay VFS real (ver vfs_bridge.hpp)

#ifndef AOLIB_WITH_PSF
// Sin los motores de aosdk enlazados no hay estado de corlett que
// preservar, pero los sondeos de duración se compilan igual: el alias
// inerte evita salpicar de #ifdef el cuerpo de esas funciones.
struct AosdkFadeScope {};
#endif
static bool g_can_dupe = false;

// Framebuffer de la UI, en XRGB8888. El formato DEBE declararse con
// SET_PIXEL_FORMAT: sin esa llamada el frontend asume 0RGB1555, que es el
// default de Libretro, y los colores salen mal.
constexpr unsigned kScreenWidth  = ui::kScreenW;
constexpr unsigned kScreenHeight = ui::kScreenH;
static std::vector<uint32_t> g_framebuffer;   // XRGB8888, kScreenWidth*kScreenHeight

// Estado de la UI. Vive fuera de CoreContext a propósito: sobrevive a
// retro_unload_game(), porque el volumen y el modo de repetición elegidos
// no deben resetearse al cambiar de álbum. Todo lo derivado del contenido
// se reconstruye en cada rebuild_ui_model().

// Bloques EXTRA rendidos y descartados por cada retro_run() mientras se
// mantiene >>. 7 extra + 1 emitido = velocidad 8x.
//
// Cota fijada por el motor más caro (PSF2, R3000A completo): peor caso
// medido ~10-13 ms de 8 bloques/llamada.
constexpr int kFfExtraBlocks = 7;

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
//
// DESGLOSE POR TRAMOS: 'total_us' incluye audio_batch_cb() y video_cb(),
// que son callbacks DEL FRONTEND y no coste del core. Con el sync de audio
// activo, audio_batch_cb() bloquea hasta que el driver drena su buffer, y
// como este core entrega exactamente 735 frames (1/60 s) por llamada, ese
// bloqueo es el regulador de velocidad del frontend. Medirlo dentro de
// 'total_us' hace que un core ocioso parezca consumir el presupuesto
// entero. Los acumuladores de abajo separan trabajo real de espera; la
// suma de los siete tramos es 'total_us' salvo el redondeo.
//
// Coste: doce lecturas de reloj por frame en vez de dos. Medido a 37 ns
// por lectura (steady_clock, x86-64), son 0,45 us sobre un frame de
// milisegundos.
struct AudioDiag {
    unsigned long frames_run       = 0;
    unsigned long underrun_events  = 0;   // retro_run() que entregaron relleno
    unsigned long underrun_frames  = 0;   // total de frames de relleno
    unsigned long over_budget      = 0;   // frames por encima de 16,667 ms
    double        total_us         = 0.0;
    double        worst_us         = 0.0;
    int           warns_emitted    = 0;   // limitado, para no inundar el log

    // Tramos, en orden de ejecución dentro de retro_run().
    double us_input   = 0.0;  // opciones + apply_ui_input()
    double us_engine  = 0.0;  // render() del motor + on_video_frame() + avance rápido
    double us_dsp     = 0.0;  // reverb + ganancia + analizador (FFT)
    double us_audiocb = 0.0;  // audio_batch_cb()  -- FRONTEND
    double us_advance = 0.0;  // fin de pista: select_track / load_zip_entry_from
    double us_ui      = 0.0;  // refresh_ui_dynamic_state() + ui::render()
    double us_videocb = 0.0;  // video_cb()        -- FRONTEND
    // Peor caso de los dos tramos que dependen del contenido, para
    // distinguir "caro siempre" de "caro solo al cambiar de pista".
    double worst_engine_us = 0.0;
    double worst_ui_us     = 0.0;
};
static AudioDiag g_diag;

// Reloj monótono en microsegundos. std::chrono y no clock_gettime para que
// la compilación de Windows (mingw) use exactamente la misma fuente.
static double now_us() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::micro>(clock::now().time_since_epoch()).count();
}

// Presupuesto de un frame a los 60 fps que declara retro_get_system_av_info.
constexpr double kFrameBudgetUs = 1000000.0 / 60.0;



static ui::UiModel     g_ui;
static ui::AudioAnalyzer g_analyzer;

// Reverb de la capa host. Vive FUERA de CoreContext, como g_analyzer y
// g_ui: sus ~30 KB de buffers se reservan una vez al arrancar el proceso y
// no se vuelven a pedir en cada carga, porque una asignación dentro de
// retro_run() es justo lo que produce un pico por encima del presupuesto de
// frame. Lo que sí es por sesión es su ESTADO (la cola), y eso se vacía con
// clear() en los mismos puntos donde se resetea el analizador.
static dsp::Reverb     g_reverb;
static ui::InputEdge   g_input_edge;
static retro_input_state_t input_state_cb = nullptr;

static void log_message(enum retro_log_level level, const char* fmt, ...) {
    if (log_cb) {
        va_list args;
        va_start(args, fmt);
        char buf[512];
        std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        log_cb(level, "%s\n", buf);
    }
}

static void log_audio_diag_summary(const char* motivo) {
    if (g_diag.frames_run == 0) return;
    const double mean_us = g_diag.total_us / static_cast<double>(g_diag.frames_run);
    const bool   algo    = (g_diag.underrun_events > 0 || g_diag.over_budget > 0);
    log_message(algo ? RETRO_LOG_WARN : RETRO_LOG_INFO,
        "[aolib] audio (%s): %lu frames | underruns: %lu (%lu frames de relleno, %.1f ms) | "
        "coste por frame: media %.0f us, peor %.0f us, presupuesto %.0f us | "
        "trabajo del core por encima del presupuesto: %lu (%.3f %%)",
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

    const double n = static_cast<double>(g_diag.frames_run);
    const double core_us = (g_diag.us_input + g_diag.us_engine + g_diag.us_dsp +
                            g_diag.us_advance + g_diag.us_ui) / n;
    const double wait_us = (g_diag.us_audiocb + g_diag.us_videocb) / n;

    log_message(RETRO_LOG_INFO,
        "[aolib] desglose por frame (us): entrada %.1f | motor %.1f | dsp+fft %.1f | "
        "audio_batch_cb %.1f | avance %.1f | ui %.1f | video_cb %.1f",
        g_diag.us_input / n, g_diag.us_engine / n, g_diag.us_dsp / n,
        g_diag.us_audiocb / n, g_diag.us_advance / n, g_diag.us_ui / n,
        g_diag.us_videocb / n);

    log_message(RETRO_LOG_INFO,
        "[aolib] trabajo del core %.1f us (%.1f %% del presupuesto) | espera en callbacks "
        "del frontend %.1f us | peor motor %.0f us, peor ui %.0f us",
        core_us, 100.0 * core_us / kFrameBudgetUs, wait_us,
        g_diag.worst_engine_us, g_diag.worst_ui_us);

    // Lectura del desglose: si 'espera en callbacks' se lleva casi todo, el
    // core no va justo -- lo que hay es el frontend regulando la velocidad,
    // y 'por encima del presupuesto' está midiendo el vsync, no el core.
    log_message(RETRO_LOG_INFO,
        "[aolib] los dos callbacks son del frontend: con sync de audio o vsync activos "
        "bloquean a propósito. Sólo 'trabajo del core' compite de verdad con los %.0f us.",
        kFrameBudgetUs);
}

static IVFSBridge& active_vfs() {
    return (g_vfs && g_vfs->is_valid())
        ? static_cast<IVFSBridge&>(*g_vfs)
        : static_cast<IVFSBridge&>(g_null_vfs);
}

static bool has_suffix(const std::string& name, const char* suffix) {
    const std::size_t sl = std::strlen(suffix);
    if (name.size() < sl) return false;
    return std::equal(name.end() - static_cast<long>(sl), name.end(), suffix,
                       [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; });
}

}  // namespace aolib
