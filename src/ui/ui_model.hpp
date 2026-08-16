// ui_model.hpp — el estado que la UI dibuja.
//
// UiModel NO consulta al motor ni al contexto por su cuenta: libretro.cpp
// lo rellena una vez por retro_run() y en cada carga o cambio de pista. Así
// dibujar es de solo lectura, no puede provocar efectos secundarios en el
// motor, y un test puede construir el modelo a mano sin levantar un motor
// entero.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "audio_analyzer.hpp"

namespace ui {

// Modos de repetición del botón REP del deck.
enum class RepeatMode : int {
    Off = 0,     // al acabar el álbum, se detiene (comportamiento por defecto)
    All = 1,     // al acabar el álbum, vuelve a empezar (= options.loop_infinite)
    One = 2      // repite la pista actual indefinidamente
};

inline const char* repeat_mode_label(RepeatMode m) {
    switch (m) {
        case RepeatMode::Off: return "OFF";
        case RepeatMode::All: return "ALL";
        case RepeatMode::One: return "ONE";
    }
    return "OFF";
}

// Una entrada de la lista visible. Puede ser una entrada de .zip o una
// subsong del fichero cargado; la UI no necesita distinguirlas, pero sí
// necesita saber cuál está sonando.
struct TrackListItem {
    std::string label;          // nombre a mostrar (ya recortado de ruta)
    uint64_t    length_frames = 0;  // 0 = duración desconocida
};

struct UiModel {
    // ── Now playing (de TrackMetadata del motor activo) ──
    std::string title;
    std::string game;
    std::string artist;
    std::string engine_name;    // "libvgm-vgm", "aosdk-psf1"...
    // Chip que se está emulando ("QSOUND", "YM2612", "SPU"...), tal como lo
    // rellena cada motor. Vacío = el motor no sabe decirlo, y la UI dibuja
    // "--" en vez de inventarse un nombre.
    std::string chip;

    // ── Posición ──
    uint64_t position_frames = 0;   // de CoreContext::frames_rendered
    uint64_t length_frames   = 0;   // 0 = desconocida -> la barra no miente, ver progress()

    // ── Lista ──
    std::vector<TrackListItem> tracks;
    int  current_track = 0;     // índice en 'tracks' de lo que suena
    int  cursor        = 0;     // índice resaltado, que puede no ser el que suena

    // Mapeo índice VISIBLE -> índice real en CoreContext::zip_entries.
    // No coinciden: zip_entries contiene también dependencias
    // (.psflib/.lib/.psf2lib/.ssflib) que no son pistas seleccionables.
    // Vacío cuando el contenido es un fichero suelto (sin .zip).
    std::vector<std::size_t> playable_to_zip;

    // ── Transporte ──
    bool       playing = true;
    bool       exhausted = false;
    RepeatMode repeat  = RepeatMode::Off;
    // Modo elegido cuando NO es 'All'. Hace falta porque 'All' se
    // implementa sobre CoreOptions::loop_infinite, que el frontend también
    // puede cambiar desde su menú: sin este campo,
    // refresh_ui_dynamic_state() no podría distinguir "Off" de "One" al
    // recomponer el estado a partir de loop_infinite.
    RepeatMode repeat_when_not_all = RepeatMode::Off;
    int        volume  = 100;   // 0..100, ganancia de capa host (ver nota en libretro.cpp)
    int        deck_focus = 2;  // botón enfocado del deck (0..7)

    // El botón VOLUME no ajusta nada por tener el foco: hay que accionarlo
    // (A) para entrar en modo ajuste. Mientras esto es true,
    // izquierda/derecha cambian el volumen en vez de mover el foco, y el
    // botón se dibuja con borde naranja. Es a propósito: si las flechas
    // quedaran secuestradas por el foco, no habría forma de salir del
    // botón.
    bool volume_adjust_active = false;

    // Alterna con A sobre el botón REB (borde naranja = activado). Es
    // independiente del foco: mover el foco a otro botón no lo apaga, igual
    // que REP tampoco se resetea. Lo recompone
    // refresh_ui_dynamic_state() desde CoreOptions::host_reverb_amount, que
    // es la fuente de verdad.
    bool reverb_enabled = false;

    // True mientras se mantiene pulsado el avance rápido
    // (deck_focus==FastForward y A mantenido). retro_run() lo lee cada frame
    // para decidir si empuja un bloque extra de audio. Se escribe desde el
    // estado CONTINUO del mando, no desde un flanco: "mantener pulsado" es
    // lo contrario de un evento de un solo disparo.
    bool fast_forward_active = false;

    // Repetición automática del cursor Up/Down de la lista: 'dir' es la
    // dirección mantenida (-1 arriba, +1 abajo, 0 = ninguna) y 'frames'
    // cuenta los frames consecutivos que lleva así. Vive en el modelo, y no
    // en una local de apply_ui_input(), porque tiene que sobrevivir entre
    // llamadas a retro_run().
    int cursor_repeat_dir    = 0;
    int cursor_repeat_frames = 0;

    // Lo mismo para Izquierda/Derecha en el modo de ajuste de volumen.
    int volume_repeat_dir    = 0;
    int volume_repeat_frames = 0;

    // Lo mismo para L2/R2 del mando, que ajustan volumen directamente sin
    // pasar por el modo de ajuste (ver apply_ui_input en libretro.cpp).
    int trigger_vol_repeat_dir    = 0;
    int trigger_vol_repeat_frames = 0;

    // Reloj de animación de la UI, en frames de vídeo; lo incrementa
    // refresh_ui_dynamic_state() una vez por retro_run(). Único consumidor
    // hoy: la marquesina de los títulos largos (ui_screen.hpp::marquee).
    // Vive en el modelo, y no en el dibujado, para que render() siga siendo
    // idempotente: dos llamadas con el mismo modelo dan el mismo
    // framebuffer.
    uint32_t anim_tick = 0;

    // ── Señal ──
    const AudioAnalyzer* analyzer = nullptr;  // no propietario; nunca nulo en runtime

    // Progreso 0..1, o -1 si la duración es desconocida: la barra debe
    // dibujar entonces un estado indeterminado, no un 0% que parece "acaba
    // de empezar". Es un caso normal, no excepcional: varios formatos no
    // traen tag de duración (ver TrackMetadata::length_frames).
    float progress() const {
        if (length_frames == 0) return -1.0f;
        const double p = static_cast<double>(position_frames) /
                         static_cast<double>(length_frames);
        return static_cast<float>(p < 0.0 ? 0.0 : (p > 1.0 ? 1.0 : p));
    }

    static int frames_to_seconds(uint64_t frames) {
        return static_cast<int>(frames / 44100);
    }
};

} // namespace ui
