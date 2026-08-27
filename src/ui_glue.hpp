// ui_glue.hpp — el puente entre el core y su interfaz de 320x240.
//
// La UI (src/ui/) no sabe nada de motores ni de .zip: dibuja un UiModel.
// Esto es lo que lo construye desde el contenido cargado, lo refresca cada
// frame y traduce el mando en cambios de pista, volumen y transporte.
//
// Salió de libretro.cpp al partirlo: ver la cabecera de ese fichero.

#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "core_globals.hpp"
#include "core_options.hpp"
#include "archive_playback.hpp"
#include "ui/transport.hpp"

namespace aolib {
// Vuelca al log la metadata de la pista. Se llama al completar una carga y
// en cada avance de pista dentro de retro_run().
static void log_now_playing() {
    if (!g_ctx || !g_ctx->engine) return;
    const auto& m = g_ctx->engine->metadata();

    // Los campos vacíos se omiten en vez de imprimirse igual. Los formatos
    // de streaming no traen tags -- no hay equivalente al bloque Corlett de
    // PSF ni a los campos de libgme -- y la línea salía como
    // 'Reproduciendo: "" () —', que parece un fallo y no lo es.
    std::string line = "[aolib] Reproduciendo: \"" + m.title + "\"";
    if (!m.game.empty())    line += " (" + m.game + ")";
    if (!m.artist.empty())  line += " — " + m.artist;
    if (!m.comment.empty()) line += " [" + m.comment + "]";

    char tail[64];
    std::snprintf(tail, sizeof(tail), " — %.1fs%s",
                  m.length_frames / 44100.0,
                  m.fade_frames > 0 ? " + fade" : "");
    line += tail;

    log_message(RETRO_LOG_INFO, "%s", line.c_str());
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
static std::string display_label_for(const std::string& raw) {
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
static void rebuild_ui_model() {
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
            if (!zip_entry_is_playable(e)) continue;   // dependencia, no pista
            // probe_failed = nunca va a sonar (ver ZipEntry::probe_failed):
            // fuera de la lista entera, no solo mostrada con "--:--" para
            // siempre. La UNICA excepción es el último recurso de una pista
            // de datos de .chd (cd_data_vfs>=0 && xa_sectors==0): ese sigue
            // probe_failed porque sondear su duración escanearía la pista
            // entera, pero es la ÚNICA entrada que llega a la música de un
            // disco cuyo directorio no marca ningún fichero XA -- ocultarla
            // dejaría ese disco sin nada seleccionable.
            const bool es_ultimo_recurso_xa =
                e.cd_data_vfs >= 0 && e.xa_sectors == 0;
            if (e.probe_failed && !es_ultimo_recurso_xa) continue;
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
static void refresh_ui_dynamic_state() {
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
static bool ui_play_visible_track(int visible_index) {
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
static void ui_activate_deck_button() {
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
static void apply_ui_input() {
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
static void apply_host_gain(int16_t* buf, std::size_t frames, int volume_0_100) {
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

}  // namespace aolib
