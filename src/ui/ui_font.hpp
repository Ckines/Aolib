// ui_font.hpp — fuente bitmap 5x7 embebida (patrón clásico de dominio
// público). Solo ASCII imprimible 0x20..0x7E; cualquier otro carácter
// cae al glifo '?'.
#pragma once
#include <cstdint>

namespace ui {

constexpr int kFontW     = 5;
constexpr int kFontH     = 7;
constexpr int kFontFirst = 0x20;
constexpr int kFontCount = 95; // 0x20..0x7E

// kFont5x7[c][r]: fila r (0 = arriba) del glifo (c + kFontFirst).
// Bit 4 (0x10) = columna izquierda, bit 0 (0x01) = columna derecha.
extern const uint8_t kFont5x7[kFontCount][kFontH];

// Devuelve las 7 filas del glifo de 'c'; '?' si no es imprimible.
inline const uint8_t* glyph_rows(unsigned char c) {
    const unsigned idx = (c >= kFontFirst && c < kFontFirst + kFontCount)
                             ? (c - kFontFirst)
                             : static_cast<unsigned>('?' - kFontFirst);
    return kFont5x7[idx];
}

// ── Fuente ancha 8x8 del logotipo "AOLIB" ──────────────────────────
// EuroPC CGA (Schneider EuroPC/II, modo texto CGA 40 col). Bitmap 8x8
// de la int10h.org "Ultimate Oldschool PC Font Pack" v2.2 (CC BY-SA 4.0,
// (c) int10h.org / VileR), extraído del archivo Bm437_EuroPC_CGA.otb
// (strike ppem 8). Bit 7 (0x80) = columna izquierda.
constexpr int kLogoFontW = 8;
constexpr int kLogoFontH = 8;

// kLogoFont[c][r]: fila r del glifo c (orden: A O L I B).
extern const uint8_t kLogoFont[5][kLogoFontH];

// Índice del glifo en kLogoFont para la letra 'c'; -1 si no está.
inline int logo_glyph_index(unsigned char c) {
    switch (c) {
        case 'A': return 0;
        case 'O': return 1;
        case 'L': return 2;
        case 'I': return 3;
        case 'B': return 4;
        default:  return -1;
    }
}

// Devuelve las 8 filas del glifo de logo; nullptr si 'c' no es A/O/L/I/B.
inline const uint8_t* logo_glyph_rows(unsigned char c) {
    const int i = logo_glyph_index(c);
    return (i >= 0) ? kLogoFont[i] : nullptr;
}

} // namespace ui
