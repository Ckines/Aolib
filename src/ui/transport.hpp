// transport.hpp — acciones del deck y lectura del mando.
//
// Este fichero declara QUÉ acciones existen y cómo se leen del mando; CÓMO
// se ejecutan vive en libretro.cpp, que es quien puede tocar el contexto y
// el motor. La separación es a propósito: el mapeo de entrada se puede
// probar sin motor, y las operaciones sobre el motor no dependen de que
// exista un mando.
#pragma once

#include <cstdint>

namespace ui {

enum class Action : int {
    None = 0,
    CursorUp,       // navega la lista sin cambiar de pista
    CursorDown,
    DeckLeft,       // mueve el foco entre botones del deck
    DeckRight,
    Activate,       // acciona el botón enfocado
    PlayTrack,      // reproduce la pista bajo el cursor
    VolumeDown,
    VolumeUp,
};

// Los ocho botones del deck, en el orden en que se dibujan.
//
// No hay botón de rebobinado: la emulación solo puede avanzar, así que un
// icono de "<<" prometería algo imposible. Stop (reinicia y pausa) cubre
// esa necesidad.
enum class DeckButton : int {
    Stop          = 0,   // ■   reinicia y pausa
    PreviousTrack = 1,   // |<  pista anterior
    PlayPause     = 2,   // >/❚❚
    FastForward   = 3,   // >>  avanza más deprisa (mantener pulsado)
    NextTrack     = 4,   // >|  pista siguiente
    Volume        = 5,   // alterna el modo de ajuste con A
    Reverb        = 6,   // alterna el reverb de la capa host con A
    Repeat        = 7,   // cicla OFF -> ALL -> ONE
};

// Estado de flanco del mando. Se guarda entre frames para que mantener un
// botón pulsado no dispare la acción 60 veces por segundo.
struct InputEdge {
    unsigned prev = 0;

    // Devuelve la máscara de botones recién pulsados en este frame.
    unsigned pressed(unsigned current) noexcept {
        const unsigned p = current & ~prev;
        prev = current;
        return p;
    }
};

// Bits de la máscara. No son los IDs de Libretro: se traducen en
// libretro.cpp para que este fichero no dependa de libretro.h y siga
// siendo comprobable en aislamiento.
namespace bits {
constexpr unsigned kUp     = 1u << 0;
constexpr unsigned kDown   = 1u << 1;
constexpr unsigned kLeft   = 1u << 2;
constexpr unsigned kRight  = 1u << 3;
constexpr unsigned kA      = 1u << 4;   // acciona el botón del deck
constexpr unsigned kB      = 1u << 5;   // reproduce la pista del cursor
constexpr unsigned kL      = 1u << 6;   // pista anterior
constexpr unsigned kR      = 1u << 7;   // pista siguiente
constexpr unsigned kStart  = 1u << 8;   // atajo de play/pausa
constexpr unsigned kL2     = 1u << 9;   // volumen -, con repetición al mantener
constexpr unsigned kR2     = 1u << 10;  // volumen +, con repetición al mantener
} // namespace bits

} // namespace ui
