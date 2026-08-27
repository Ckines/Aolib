// ui_screen.hpp — la pantalla de AOLIB, dibujada a partir de UiModel.
//
// render() es de SOLO LECTURA sobre el modelo: dibujar no puede provocar
// efectos secundarios ni depender de estado propio. Por eso el peak-hold
// del VU y del espectro vive en AudioAnalyzer, que se actualiza en su
// propio paso, y la marquesina toma el tiempo de UiModel::anim_tick en vez
// de guardar su posición aquí. La consecuencia práctica es que dos
// llamadas con el mismo modelo producen el mismo framebuffer, que es lo
// que permite comprobar la pantalla en un test.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "ui_font.hpp"
#include "ui_model.hpp"
#include "ui_renderer.hpp"
#include "transport.hpp"

namespace ui {

constexpr int kScreenW = 320;
constexpr int kScreenH = 240;

// Nº de filas visibles del track list (el panel mide 132 px con paso de
// 12 px desde y=27). La lista real puede tener 1 pista o 400, así que la
// ventana se desplaza.
constexpr int kListVisibleRows = 10;

namespace detail {

// ───────────────────────────── Paleta ─────────────────────────────
inline constexpr uint32_t kColBg          = rgb(8, 13, 26);       // #080d1a
inline constexpr uint32_t kColLogo        = rgb(224, 232, 255);   // #e0e8ff
inline constexpr uint32_t kColSubtitle    = rgb(97, 115, 138);    // #61738a
inline constexpr uint32_t kColPanelBorder = rgb(26, 42, 64);      // #1a2a40
inline constexpr uint32_t kColPanelFill   = rgb(8, 17, 31);       // gradiente del HTML
inline constexpr uint32_t kColPanelTitle  = rgb(141, 164, 196);   // #8da4c4
inline constexpr uint32_t kColTitleBg     = rgb(26, 42, 64);      // fondo de la cabecera INFO (= kColPanelBorder)
inline constexpr uint32_t kColTrack       = rgb(122, 147, 178);   // #7a93b2
inline constexpr uint32_t kColActiveBg    = rgb(24, 51, 89);      // #183359
inline constexpr uint32_t kColActiveBd    = rgb(45, 85, 138);     // #2d558a
inline constexpr uint32_t kColWhite       = rgb(255, 255, 255);
inline constexpr uint32_t kColInfoLabel   = rgb(93, 117, 150);    // #5d7596
inline constexpr uint32_t kColNpSub       = rgb(67, 113, 166);    // #4371a6
inline constexpr uint32_t kColNpComp      = rgb(59, 156, 130);    // #3b9c82
inline constexpr uint32_t kColPeakWarm    = rgb(245, 158, 11);    // #f59e0b ámbar
inline constexpr uint32_t kColPeakRed     = rgb(225, 29, 72);     // #e11d48 rojo frío
inline constexpr uint32_t kColNpArr       = rgb(46, 122, 103);    // #2e7a67
inline constexpr uint32_t kColSpecBg      = rgb(3, 6, 13);        // #03060d
inline constexpr uint32_t kColSpecBorder  = rgb(13, 26, 45);      // #0d1a2d
inline constexpr uint32_t kColBarHi       = rgb(0, 230, 255);     // cian claro
inline constexpr uint32_t kColBarLo       = rgb(0, 120, 230);     // azul
inline constexpr uint32_t kColPeak        = rgb(220, 255, 255);   // línea del pico
inline constexpr uint32_t kColScan        = rgb(1, 3, 8);         // scanlines CRT
inline constexpr uint32_t kColPlayArrow   = rgb(34, 197, 94);     // #22c55e
inline constexpr uint32_t kColTimeTotal   = rgb(82, 99, 122);     // #52637a
inline constexpr uint32_t kColCounter     = rgb(97, 115, 138);    // #61738a
inline constexpr uint32_t kColCounterBig  = rgb(129, 140, 248);   // #818cf8
inline constexpr uint32_t kColProgBg      = rgb(8, 16, 29);       // #08101d
inline constexpr uint32_t kColProgBorder  = rgb(28, 45, 66);      // #1c2d42
inline constexpr uint32_t kColProgFill    = rgb(34, 211, 238);    // #22d3ee
inline constexpr uint32_t kColDeck        = rgb(20, 28, 42);      // gradiente del HTML
inline constexpr uint32_t kColDeckBorder  = rgb(45, 55, 71);      // #2d3747
inline constexpr uint32_t kColBtnBg       = rgb(32, 42, 61);      // gradiente #2a3445..#121721
inline constexpr uint32_t kColBtnBorder   = rgb(61, 75, 97);      // #3d4b61
inline constexpr uint32_t kColBtnOuter    = rgb(45, 55, 71);      // borde exterior #2d3747
inline constexpr uint32_t kColBtnText     = rgb(141, 164, 196);   // #8da4c4
inline constexpr uint32_t kColBtnActiveBg = rgb(15, 22, 36);      // #0f1624
inline constexpr uint32_t kColBtnActiveTx = rgb(34, 211, 238);    // #22d3ee
// Borde y texto de un botón TOGGLE encendido (Volume en modo ajuste,
// Reverb activo). Coincide con kColPeakWarm del VU meter, pero se declara
// aparte porque significa otra cosa (estado persistente, no un pico de
// señal) y cambiar uno no debe arrastrar al otro.
inline constexpr uint32_t kColBtnToggleOn = rgb(245, 158, 11);    // #f59e0b

// Sombra del altavoz mini en naranja (ver kSpeakerMiniArt). Es un valor
// medido, no derivado de kColBtnToggleOn: ninguna de las sombras de este
// fichero guarda una proporción fija con su tono principal.
inline constexpr uint32_t kColSpeakerOrangeShadow = rgb(171, 110, 7); // #ab6e07

// ──────────────────────── Layout (offsets) ────────────────────────
// Coordenadas de pantalla 320x240. EDITA AQUÍ los offsets a mano:
// cada constante mueve una zona concreta (los valores derivados se
// recalculan automáticamente en render_ui).
namespace layout {

// Margen general de paneles y deck
inline constexpr int kMarginX = 4;

// ── Header ──
inline constexpr int kHeaderPadX   = 5;   // margen izquierdo/derecho del texto
inline constexpr int kHeaderLineY  = 13;  // línea divisoria

// ── TRACK LIST (panel izquierdo superior) ──
inline constexpr int kListX      = 4;    // panel
inline constexpr int kListY      = 14;
inline constexpr int kListW      = 142;
inline constexpr int kListH      = 132;
inline constexpr int kListTitleX = 7;    // título "TRACK LIST"
inline constexpr int kListTitleY = 17;
inline constexpr int kListRow0Y  = 27;   // primera fila de canciones
inline constexpr int kListRowH   = 12;   // paso entre filas
inline constexpr int kListRowHgt = 9;    // alto de la fila resaltada
inline constexpr int kListNumX   = 7;     // columna nº (01..06)
inline constexpr int kListNameX  = 21;   // columna nombre
inline constexpr int kListTimeX  = 115;  // columna duración

// ── INFO (panel derecho superior) ──
inline constexpr int kInfoX      = 148;
inline constexpr int kInfoY      = 14;
inline constexpr int kInfoW      = 168;
inline constexpr int kInfoH      = 46;
inline constexpr int kInfoTitleX = 151;    // cabecera "INFO"
inline constexpr int kInfoTitleY = 17;
inline constexpr int kInfoRow0Y  = 27;     // primera fila de datos
inline constexpr int kInfoRowH   = 9;
inline constexpr int kInfoLabelX = 151;    // columna clave

// ── SPECTRUM ANALYZER (panel derecho medio) ──
inline constexpr int kSpecX         = 148;
// El hueco con INFO (que acaba en la fila 59) queda así en 2px, igual que
// entre cualquier otro par de paneles. Si se toca, kPlayY tiene que
// acompañar para no romper el hueco de más abajo.
inline constexpr int kSpecY         = 62;
inline constexpr int kSpecW         = 168;
inline constexpr int kSpecH         = 69;
inline constexpr int kSpecTitleX    = 151;
inline constexpr int kSpecTitleY    = 65;
inline constexpr int kSpecBoxX      = 152; // rejilla interna
inline constexpr int kSpecBoxY      = 76;
inline constexpr int kSpecBoxW      = 160;
inline constexpr int kSpecBoxH      = 53;
inline constexpr int kSpecBarX0     = 163; // primera barra (28 barras centradas)
inline constexpr int kSpecBarStep   = 5;   // separación entre barras (3px barra + 2px hueco)
inline constexpr int kSpecBarW      = 3;   // ancho de barra y pico
inline constexpr int kSpecBotY      = 127; // suelo de la cuadrícula
inline constexpr int kSpecTopY      = 77;  // tope (clamp del pico)

// ── VU METER (panel izquierdo inferior) ──
inline constexpr int kPkX        = 4;
inline constexpr int kPkY        = 148;
inline constexpr int kPkW        = 142;
inline constexpr int kPkH        = 50;
inline constexpr int kPkTitleX   = 7;
inline constexpr int kPkTitleY   = 151;
inline constexpr int kPkLabelX   = 9;      // "L" / "R"
inline constexpr int kPkRow1Y    = 164;    // fila L
inline constexpr int kPkRow2Y    = 179;    // fila R
inline constexpr int kPkBarX     = 20;     // primera celda
inline constexpr int kPkBars     = 15;     // celdas por barra
inline constexpr int kPkCellW    = 6;      // 5px bloque + 1px hueco
inline constexpr int kPkCellFill = 5;
inline constexpr int kPkBarH     = 10;
inline constexpr int kPkBoxX     = 6;      // pantalla interna (tipo CRT)
inline constexpr int kPkBoxY     = 160;
inline constexpr int kPkBoxW     = 138;
inline constexpr int kPkBoxH     = 35;     // y=160..195

// ── PLAYING (panel derecho inferior) ──
//
// kPlayY acompaña a kSpecY para mantener el hueco entre ambos en 2px, y
// kPlayH lo compensa para que el borde INFERIOR siga en la fila 197, donde
// coincide con el de VU METER en la columna izquierda. Si se mueve uno de
// los dos sin el otro, se rompe una de las dos alineaciones.
inline constexpr int kPlayX         = 148;
inline constexpr int kPlayY         = 133;
inline constexpr int kPlayW         = 168;
inline constexpr int kPlayH         = 65;
inline constexpr int kPlayTitleX    = 151;
inline constexpr int kPlayTitleY    = 136;
inline constexpr int kPlaySongX     = 151; // nombre de la canción
inline constexpr int kPlaySongY     = 146;
inline constexpr int kPlayFromX     = 151; // nombre del álbum/juego (ver kPlayFromY)
inline constexpr int kPlayFromY     = 155;
inline constexpr int kPlayArrowX    = 151; // ► verde
inline constexpr int kPlayArrowY    = 165;
inline constexpr int kPlayArrowW    = 7;
inline constexpr int kPlayArrowH    = 7;
inline constexpr int kPlayTimeX     = 160; // tiempo actual (blanco)
inline constexpr int kPlayTimeY     = 165;
inline constexpr int kPlayTotalX    = 192; // "/ 03:15" (gris)
// El bloque "TRACK NN / total" se desplaza como grupo: las tres columnas
// van juntas.
inline constexpr int kPlayTrackX    = 272; // "TRACK"
inline constexpr int kPlayTrackY    = 169;
inline constexpr int kPlayNumX      = 273; // "03"
inline constexpr int kPlayNumY      = 177;
// El "/ total" se ancla por la DERECHA, en el mismo pixel donde acaba la
// barra de progreso. Con una X fija, un album de 3 cifras (180 pistas) lo
// empujaba hasta el 319 y se salia del panel, que acaba en el 315.
inline constexpr int kPlayNumsRightX = 312; // = kPlayBarX + kPlayBarW - 1
inline constexpr int kPlayBarX      = 151; // barra de progreso
inline constexpr int kPlayBarY      = 188;
inline constexpr int kPlayBarW      = 162;
inline constexpr int kPlayBarH      = 5;
inline constexpr int kPlayFillX     = 152; // relleno
inline constexpr int kPlayFillY     = 189;
inline constexpr int kPlayFillW     = 160;
inline constexpr int kPlayFillH     = 3;
inline constexpr int kPlayPtrW      = 4;   // puntero vertical
inline constexpr int kPlayPtrH      = 7;
inline constexpr int kPlayPtrY      = kPlayBarY - 1; // centrado en la barra
inline constexpr int kSndBarX      = 163; // barra de sonido (volumen), debajo de los números de tiempo
inline constexpr int kSndBarY      = 179; // debajo de la fila de tiempo
inline constexpr int kSndBarW      = 41;  // barra de sonido (volumen)
inline constexpr int kSndBarH      = 6;
inline constexpr int kSndIconX     = kSndBarX - 12; // altavoz mini junto a la barra
inline constexpr int kSndIconY     = kSndBarY;
inline constexpr int kSndIconH     = 6;

// ── Deck de controles (abajo) ──
inline constexpr int kDeckX     = 4;
inline constexpr int kDeckY     = 200;
inline constexpr int kDeckW     = 312;
inline constexpr int kDeckH     = 27;
inline constexpr int kBtnX0     = 10;   // primer botón
inline constexpr int kBtnY      = 204;
inline constexpr int kBtnW      = 34;
inline constexpr int kBtnH      = 19;
inline constexpr int kBtnStep   = 38;  // separación entre botones (hueco 4px)
inline constexpr int kBtnIconY  = 209; // triángulos (|◄ ◄◄ ►► ►|)
inline constexpr int kBtnBarsY  = 208; // barras (pausa, █)
inline constexpr int kBtnLabelY = 205; // "REP" / "SHUF"

} // namespace layout


inline int text_width(const char* s) {
    return static_cast<int>(std::strlen(s)) * (kFontW + 1) - 1;
}

// Recorta 's' con "..." para que quepa en 'max_px' píxeles: los nombres
// reales (ficheros de .zip, títulos GD3) no tienen longitud acotada.
inline std::string ellipsize(const std::string& s, int max_px) {
    if (max_px <= 0) return std::string();
    const int per_char = kFontW + 1;
    const int fits = (max_px + 1) / per_char;
    if (static_cast<int>(s.size()) <= fits) return s;
    if (fits <= 3) return s.substr(0, static_cast<std::size_t>(std::max(0, fits)));
    return s.substr(0, static_cast<std::size_t>(fits - 3)) + "...";
}

// Marquesina horizontal lenta: devuelve la VENTANA de 's' que toca mostrar
// en el tick dado. Si el texto cabe en 'max_px' lo devuelve tal cual, sin
// moverlo.
//
// Función PURA de (texto, ancho, tick), sin estado propio: el reloj viene
// de fuera (UiModel::anim_tick) para que dibujar siga siendo idempotente.
//
// Ciclo: quieta al principio kHold frames para poder leer el comienzo, un
// carácter cada kStep frames, otra pausa al final y vuelta a empezar. Se
// desplaza por caracteres y no por píxeles porque la fuente es de ancho
// fijo: un desplazamiento sub-carácter exigiría recortar glifos a media
// columna, y a 6 px por carácter apenas se notaría.
inline std::string marquee(const std::string& s, int max_px, uint32_t tick) {
    constexpr uint32_t kHold = 96;   // ~1,6 s a 60 fps, parado en cada extremo
    constexpr uint32_t kStep = 10;   // 1 carácter cada ~0,17 s => 6 car/s
    if (max_px <= 0) return std::string();
    const int per_char = kFontW + 1;
    const int fits = (max_px + 1) / per_char;
    if (fits <= 0) return std::string();
    if (static_cast<int>(s.size()) <= fits) return s;

    const uint32_t extra = static_cast<uint32_t>(s.size()) - static_cast<uint32_t>(fits);
    const uint32_t travel = extra * kStep;
    const uint32_t cycle  = kHold + travel + kHold;
    const uint32_t phase  = tick % cycle;

    uint32_t offset;
    if (phase < kHold)                 offset = 0;
    else if (phase < kHold + travel)   offset = (phase - kHold) / kStep;
    else                               offset = extra;

    return s.substr(static_cast<std::size_t>(offset), static_cast<std::size_t>(fits));
}

inline void draw_panel(Renderer& r, int x, int y, int w, int h) {
    r.fill_rect(x, y, w, h, kColPanelFill);
    r.draw_rect(x, y, w, h, kColPanelBorder);
}

inline void draw_title(Renderer& r, int x, int y, const char* s) {
    const int tw = text_width(s);
    r.fill_rect(x + 1, y, tw - 1, kFontH, kColTitleBg);
    r.draw_text(x, y, kColPanelTitle, s);
}
// x centrado en [x, x+w)
inline void draw_centered(Renderer& r, int x, int y, int w, uint32_t color, const char* s) {
    r.draw_text(x + (w - text_width(s)) / 2, y, color, s);
}

// Aclara (f>1) u oscurece (f<1) un color, con clamp a 255.
inline uint32_t tint(uint32_t c, float f) {
    auto q = [f](int v) {
        const float t = static_cast<float>(v) * f;
        return t > 255.0f ? 255 : (t < 0.0f ? 0 : static_cast<int>(t));
    };
    return rgb(q((c >> 16) & 0xFF), q((c >> 8) & 0xFF), q(c & 0xFF));
}

// Celda rectangular con bisel: brillo arriba, sombra abajo/derecha.
inline void draw_cell(Renderer& r, int x, int y, int w, int h, uint32_t base) {
    r.fill_rect(x, y, w, h, tint(base, 0.55f));
    r.fill_rect(x, y, w, h - 1, base);
    r.fill_rect(x, y + 1, w, 1, tint(base, 1.7f));
    r.fill_rect(x, y + h - 2, w, 1, tint(base, 0.7f));
}


inline void draw_triangle(Renderer& r, int x, int y, int w, int h, bool right, uint32_t color) {
    if (right) {
        r.draw_line(x, y, x + w - 1, y + h / 2, color);
        r.draw_line(x, y, x, y + h - 1, color);
        r.draw_line(x, y + h - 1, x + w - 1, y + h / 2, color);
    } else {
        r.draw_line(x + w - 1, y, x, y + h / 2, color);
        r.draw_line(x + w - 1, y, x + w - 1, y + h - 1, color);
        r.draw_line(x + w - 1, y + h - 1, x, y + h / 2, color);
    }
}

// Triángulo relleno (misma geometría que draw_triangle).
inline void fill_triangle(Renderer& r, int x, int y, int w, int h, bool right, uint32_t color) {
    const int apex = h / 2;
    const int bot  = h - 1 - apex;
    for (int dy = 0; dy < h; ++dy) {
        int dw;
        if (dy <= apex) dw = dy * (w - 1) / (apex ? apex : 1);
        else            dw = (h - 1 - dy) * (w - 1) / (bot ? bot : 1);
        if (right) r.fill_rect(x, y + dy, dw + 1, 1, color);
        else       r.fill_rect(x + (w - 1) - dw, y + dy, dw + 1, 1, color);
    }
}

// Oscurece un color (pct = 0..100) para dar aspecto sombreado.
inline uint32_t darken(uint32_t c, int pct) {
    return rgb((c >> 16 & 0xFF) * pct / 100,
                   (c >> 8  & 0xFF) * pct / 100,
                   (c       & 0xFF) * pct / 100);
}

// Altavoz del deck 12x9: '#' tono principal, '.' sombra, '-' sombra más
// oscura de la cola del cono (kColBtnBorder, el mismo gris del anillo del
// botón). Tres tonos y no dos: con solo dos, la cola se confunde con el
// cuerpo a este tamaño.
static const char* kSpeakerDeckArt[9] = {
    "    ..   .  ",
    "   .#.    . ",
    "--.##.  .  .",
    ".####.   . .",
    ".####.   . .",
    ".####.   . .",
    "--.##.  .  .",
    "   .#.    . ",
    "    ..   .  ",
};

// Altavoz mini 10x6 (junto a la barra de sonido): 'A' = color 1, 'B' = color 2, ' ' = hueco.
static const char* kSpeakerMiniArt[6] = {
    "...AA...A.",
    ".AABB.A..A",
    "ABBBB..B.B",
    "ABBBB..B.B",
    ".AABB.A..A",
    "...AA...A.",
};

// Dibuja un mapa de pixel-art (1px por carácter) en (x,y).
inline void draw_pixel_art(Renderer& r, int x, int y, uint32_t c, uint32_t ci,
                    const char* const* art, int rows) {
    for (int yy = 0; yy < rows; ++yy)
        for (int xx = 0; art[yy][xx]; ++xx) {
            if (art[yy][xx] == '#') r.fill_rect(x + xx, y + yy, 1, 1, c);
            else if (art[yy][xx] == '.') r.fill_rect(x + xx, y + yy, 1, 1, ci);
        }
}

// Igual que draw_pixel_art, más un tercer tono para '-'.
inline void draw_pixel_art3(Renderer& r, int x, int y, uint32_t c, uint32_t ci,
                    uint32_t c3, const char* const* art, int rows) {
    for (int yy = 0; yy < rows; ++yy)
        for (int xx = 0; art[yy][xx]; ++xx) {
            if (art[yy][xx] == '#')      r.fill_rect(x + xx, y + yy, 1, 1, c);
            else if (art[yy][xx] == '.') r.fill_rect(x + xx, y + yy, 1, 1, ci);
            else if (art[yy][xx] == '-') r.fill_rect(x + xx, y + yy, 1, 1, c3);
        }
}

// Pixel-art con 2 colores ('A' y 'B'); el resto de caracteres es transparente.
inline void draw_pixel_art2(Renderer& r, int x, int y, uint32_t cA, uint32_t cB,
                     const char* const* art, int rows) {
    for (int yy = 0; yy < rows; ++yy)
        for (int xx = 0; art[yy][xx]; ++xx) {
            if (art[yy][xx] == 'A') r.fill_rect(x + xx, y + yy, 1, 1, cA);
            else if (art[yy][xx] == 'B') r.fill_rect(x + xx, y + yy, 1, 1, cB);
        }
}


// Suelo de la lectura del VU: por debajo se muestra "-inf" en vez de un
// número. La barra ya está vacía desde -48 dB, así que -60 dB deja 12 dB de
// lectura útil aun con la barra a cero.
inline constexpr float kVuFloorDb = -60.0f;

// ── VU METER: pico real por canal, medido a la salida de audio_batch_cb ──
inline void draw_vu_panel(Renderer& r, const UiModel& model) {
    draw_panel(r, layout::kPkX, layout::kPkY, layout::kPkW, layout::kPkH);
    draw_title(r, layout::kPkTitleX, layout::kPkTitleY, "VU METER");
    r.fill_rect(layout::kPkBoxX, layout::kPkBoxY, layout::kPkBoxW, layout::kPkBoxH, kColSpecBg);
    r.draw_rect(layout::kPkBoxX, layout::kPkBoxY, layout::kPkBoxW, layout::kPkBoxH, kColSpecBorder);

    for (int ch = 0; ch < 2; ++ch) {
        // Pico real del canal, ya suavizado por AudioAnalyzer.
        const float v = model.analyzer ? model.analyzer->vu(ch) : 0.0f;
        const float pk = model.analyzer ? model.analyzer->vu_peak(ch) : 0.0f;

        // Escala en dB, no lineal: en lineal casi todo el recorrido se
        // gasta en los últimos 6 dB y el medidor parece apagado.
        //
        // Acotado por abajo a kVuFloorDb, con "-inf" por debajo: en silencio,
        // un decaimiento multiplicativo nunca llega a cero exacto y el número
        // seguiría bajando indefinidamente.
        const bool  silent = (v <= 0.0f) || (20.0f * std::log10(v) <= kVuFloorDb);
        const float v_db = silent ? kVuFloorDb : (20.0f * std::log10(v));
        const float norm = std::clamp((v_db + 48.0f) / 48.0f, 0.0f, 1.0f);
        const int filled = static_cast<int>(norm * layout::kPkBars + 0.5f);

        const int y = (ch == 0) ? layout::kPkRow1Y : layout::kPkRow2Y;
        // La etiqueta L/R y la lectura en dB comparten fila (y+3); las
        // barras van 1px por debajo del origen de la fila. Con kPkBarH=10 el
        // borde inferior queda en 174/189, dentro de kPkBoxY+kPkBoxH=195.
        const int bar_y = y + 1;
        // L/R en el gris de los títulos de panel y no en el verde de las
        // barras: ese verde ya lo lleva el relleno y competía con él.
        r.draw_text(layout::kPkLabelX + 1, y + 3, kColPanelTitle, ch == 0 ? "L" : "R");
        for (int i = 0; i < layout::kPkBars; ++i) {
            const int x = layout::kPkBarX + i * layout::kPkCellW;
            if (i < filled) {
                draw_cell(r, x, bar_y, layout::kPkCellFill, layout::kPkBarH,
                          i >= 13 ? kColPeakRed : (i >= 10 ? kColPeakWarm : kColNpComp));
            } else {
                r.fill_rect(x, bar_y, layout::kPkCellFill, layout::kPkBarH, kColProgBg);
            }
        }

        // Pico retenido: la celda del máximo reciente.
        const float pk_db = (pk > 0.0f) ? std::max(kVuFloorDb, 20.0f * std::log10(pk))
                                        : kVuFloorDb;
        const int pk_cell = static_cast<int>(
            std::clamp((pk_db + 48.0f) / 48.0f, 0.0f, 1.0f) * layout::kPkBars + 0.5f) - 1;
        if (pk_cell >= 0 && pk_cell < layout::kPkBars) {
            r.fill_rect(layout::kPkBarX + pk_cell * layout::kPkCellW, bar_y, 1,
                        layout::kPkBarH, kColPeak);
        }

        // Lectura en dBFS; "-inf" para silencio o por debajo del suelo útil.
        char buf[16];
        if (silent) std::snprintf(buf, sizeof(buf), "-inf");
        else        std::snprintf(buf, sizeof(buf), "%ddB", static_cast<int>(v_db));
        r.draw_text(layout::kPkX + layout::kPkW - 5 - text_width(buf), y + 3, kColWhite, buf);
    }
}

// ── TRACK LIST: entradas reales, con ventana desplazable ──
inline void draw_track_list(Renderer& r, const UiModel& model) {
    draw_panel(r, layout::kListX, layout::kListY, layout::kListW, layout::kListH);
    draw_title(r, layout::kListTitleX, layout::kListTitleY, "TRACK LIST");

    const int total = static_cast<int>(model.tracks.size());
    if (total == 0) {
        r.draw_text(layout::kListNumX, layout::kListRow0Y, kColTimeTotal, "(sin pistas)");
        return;
    }

    // Ventana centrada en el cursor, acotada a los extremos.
    int first = model.cursor - kListVisibleRows / 2;
    first = std::clamp(first, 0, std::max(0, total - kListVisibleRows));

    for (int row = 0; row < kListVisibleRows; ++row) {
        const int i = first + row;
        if (i >= total) break;
        const int row_y = layout::kListRow0Y + row * layout::kListRowH;
        const bool is_cursor  = (i == model.cursor);
        const bool is_playing = (i == model.current_track);

        if (is_cursor) {
            r.fill_rect(layout::kListX + 1, row_y - 2, layout::kListW - 2,
                        layout::kListRowHgt + 2, kColActiveBg);
            r.draw_line(layout::kListX + 1, row_y - 2, layout::kListX + layout::kListW - 2,
                        row_y - 2, kColActiveBd);
            r.draw_line(layout::kListX + 1, row_y + layout::kListRowHgt - 1,
                        layout::kListX + layout::kListW - 2, row_y + layout::kListRowHgt - 1,
                        kColActiveBd);
        }

        char num[16]; // holgado: 'i' no está acotado a 2 dígitos
        std::snprintf(num, sizeof(num), "%02d", i + 1);
        const uint32_t c = is_cursor ? kColWhite : kColTrack;
        r.draw_text(layout::kListNumX, row_y, c, num);

        // El cursor y la pista que suena son cosas distintas: se puede
        // navegar la lista sin cambiar de pista.
        if (is_playing) {
            fill_triangle(r, layout::kListNumX + 12, row_y + 1, 4, 5, true, kColPlayArrow);
        }

        // Recorte por ancho en píxeles, no por nº de caracteres.
        const int name_x = layout::kListNameX + 5;
        const int avail  = layout::kListTimeX - name_x - 2;
        // Solo la fila del cursor usa marquesina; el resto se recorta con
        // "...". Diez marquesinas a la vez serían ilegibles, y esa es la
        // única fila cuyo nombre completo interesa leer.
        const std::string& raw_label = model.tracks[static_cast<std::size_t>(i)].label;
        const std::string shown = is_cursor ? marquee(raw_label, avail, model.anim_tick)
                                            : ellipsize(raw_label, avail);
        r.draw_text(name_x, row_y, c, shown.c_str());

        const auto& item = model.tracks[static_cast<std::size_t>(i)];
        const uint64_t len = item.length_frames;
        char tbuf[16];
        if (len == 0) std::snprintf(tbuf, sizeof(tbuf), "--:--");
        else {
            // No se distingue la duracion medida por muestreo (audio XA de
            // un .chd) de la contada de verdad: el error medido contra
            // vgmstream es <=1,5 % en 100 de 101 ficheros de prueba,
            // indistinguible en la practica.
            const int s = static_cast<int>(len / 44100);
            std::snprintf(tbuf, sizeof(tbuf), "%02d:%02d", s / 60, s % 60);
        }
        r.draw_text(layout::kListTimeX, row_y, is_cursor ? kColPanelTitle : kColTimeTotal, tbuf);
    }

    // Aquí NO va un contador "cursor/total": el panel PLAYING ya muestra
    // "TRACK NN / total", y dos números casi siempre idénticos en la misma
    // pantalla son ruido. La ventana desplazable ya indica que hay más
    // lista fuera de vista.
}

// ── INFO: datos reales del contenido cargado ──
inline void draw_info_panel(Renderer& r, const UiModel& model) {
    draw_panel(r, layout::kInfoX, layout::kInfoY, layout::kInfoW, layout::kInfoH);
    draw_title(r, layout::kInfoTitleX, layout::kInfoTitleY, "INFO");

    // Duración total del álbum: suma de lo conocido. Si alguna pista no
    // reporta duración, se marca con '+' en vez de mentir con un total
    // exacto que no lo es. Una entrada que nunca va a resolverse (ver
    // ZipEntry::probe_failed) ni siquiera llega a model.tracks -- se filtra
    // en rebuild_ui_model() -- así que aquí no hace falta distinguirla: la
    // única excepción (el último recurso de una pista de datos de .chd) SÍ
    // debe quedarse marcando '+' para siempre, que es justo lo que pasa al
    // no excluirla.
    uint64_t total_frames = 0;
    bool any_unknown = false;
    for (const auto& t : model.tracks) {
        if (t.length_frames == 0) any_unknown = true;
        // Las estimadas (audio XA de un .chd) suman igual que las
        // contadas: el error medido es <=1,5 %, así que no se
        // distinguen en el total.
        else total_frames += t.length_frames;
    }

    char total_buf[24];
    if (total_frames == 0) {
        std::snprintf(total_buf, sizeof(total_buf), "--:--:--");
    } else {
        const int s = static_cast<int>(total_frames / 44100);
        std::snprintf(total_buf, sizeof(total_buf), "%d:%02d:%02d%s",
                      s / 3600, (s / 60) % 60, s % 60, any_unknown ? "+" : "");
    }

    // Fila CHIP y no TRACKS: el número de pistas ya está en PLAYING,
    // mientras que el chip emulado no aparecería en ninguna parte -- el
    // header solo dice el motor, y "libvgm-vgm" no distingue un YM2612 de un
    // QSound.
    const std::string chip = model.chip.empty() ? std::string("--") : model.chip;
    const std::string artist = model.artist.empty() ? std::string("--") : model.artist;

    // CHIP y ARTIST usan marquesina si no caben: un VGM multichip
    // ("YM2612+SN76496") o varios compositores en un tag GD3 se salen.
    const int chip_avail   = layout::kInfoW - text_width("CHIP") - 14;
    const int artist_avail = layout::kInfoW - text_width("ARTIST") - 14;

    const struct { const char* k; std::string v; } rows[3] = {
        { "TOTAL TIME", total_buf },
        { "CHIP",       marquee(chip,   chip_avail,   model.anim_tick) },
        { "ARTIST",     marquee(artist, artist_avail, model.anim_tick) },
    };
    for (int i = 0; i < 3; ++i) {
        const int row_y = layout::kInfoRow0Y + i * layout::kInfoRowH;
        r.draw_text(layout::kInfoLabelX, row_y, kColInfoLabel, rows[i].k);
        r.draw_text(layout::kInfoX + layout::kInfoW - 4 - text_width(rows[i].v.c_str()), row_y,
                    kColPanelTitle, rows[i].v.c_str());
    }
}

// ── SPECTRUM ANALYZER: FFT del audio real ──
inline void draw_spectrum(Renderer& r, const UiModel& model) {
    draw_panel(r, layout::kSpecX, layout::kSpecY, layout::kSpecW, layout::kSpecH);
    draw_title(r, layout::kSpecTitleX, layout::kSpecTitleY, "SPECTRUM ANALYZER");
    r.fill_rect(layout::kSpecBoxX, layout::kSpecBoxY, layout::kSpecBoxW, layout::kSpecBoxH,
                kColSpecBg);
    r.draw_rect(layout::kSpecBoxX, layout::kSpecBoxY, layout::kSpecBoxW, layout::kSpecBoxH,
                kColSpecBorder);

    const int span = layout::kSpecBotY - layout::kSpecTopY; // altura útil

    for (int i = 0; i < kSpectrumBands; ++i) {
        const float v  = model.analyzer ? model.analyzer->band(i) : 0.0f;
        const float pk = model.analyzer ? model.analyzer->band_peak(i) : 0.0f;

        const int h = static_cast<int>(v * static_cast<float>(span - 4)) + 1;
        const int bx = layout::kSpecBarX0 + i * layout::kSpecBarStep;
        const int by = layout::kSpecBotY - h;
        // Degradado: azul abajo, cian arriba.
        r.fill_rect(bx, by, layout::kSpecBarW, h * 3 / 5, kColBarLo);
        r.fill_rect(bx, by + h * 3 / 5, layout::kSpecBarW, h - h * 3 / 5, kColBarHi);

        // El peak-hold lo mantiene AudioAnalyzer: dibujarlo no muta nada.
        int py = layout::kSpecBotY - static_cast<int>(pk * static_cast<float>(span - 4)) - 2;
        py = std::clamp(py, layout::kSpecTopY, layout::kSpecBotY - 1);
        r.fill_rect(bx, py, layout::kSpecBarW, 1, kColPeak);
    }

    // Scanlines CRT
    for (int y = layout::kSpecBoxY; y < layout::kSpecBoxY + layout::kSpecBoxH; y += 2)
        r.draw_line(layout::kSpecBoxX, y, layout::kSpecBoxX + layout::kSpecBoxW - 1, y, kColScan);
}

// ── PLAYING: metadata, posición y progreso reales ──
inline void draw_playing(Renderer& r, const UiModel& model) {
    draw_panel(r, layout::kPlayX, layout::kPlayY, layout::kPlayW, layout::kPlayH);
    draw_title(r, layout::kPlayTitleX, layout::kPlayTitleY, "PLAYING");

    const int text_avail = layout::kPlayW - 8;
    const std::string title = model.title.empty()
        ? (model.tracks.empty() ? std::string("(nada cargado)")
                                : model.tracks[static_cast<std::size_t>(
                                      std::clamp(model.current_track, 0,
                                                 static_cast<int>(model.tracks.size()) - 1))].label)
        : model.title;
    r.draw_text(layout::kPlaySongX, layout::kPlaySongY, kColWhite,
                ellipsize(title, text_avail).c_str());

    // Campo 'game' de los tags reales (GD3 en VGM, Corlett en PSF/SSF), sin
    // prefijo: va justo debajo del título y en un tono más apagado, así que
    // un "From " solo gastaría ancho.
    if (!model.game.empty()) {
        r.draw_text(layout::kPlayFromX, layout::kPlayFromY, kColNpSub,
                    ellipsize(model.game, text_avail).c_str());
    }

    // Estado de transporte: play verde, pausa ámbar, agotado gris.
    const uint32_t arrow_col = model.exhausted ? kColTimeTotal
                             : (model.playing ? kColPlayArrow : kColPeakWarm);
    if (model.playing && !model.exhausted) {
        fill_triangle(r, layout::kPlayArrowX, layout::kPlayArrowY,
                      layout::kPlayArrowW, layout::kPlayArrowH, true, arrow_col);
    } else {
        r.fill_rect(layout::kPlayArrowX, layout::kPlayArrowY, 2, layout::kPlayArrowH, arrow_col);
        r.fill_rect(layout::kPlayArrowX + 4, layout::kPlayArrowY, 2, layout::kPlayArrowH, arrow_col);
    }

    const int cur_s = UiModel::frames_to_seconds(model.position_frames);
    char line[40];
    std::snprintf(line, sizeof(line), "%02d:%02d", cur_s / 60, cur_s % 60);
    r.draw_text(layout::kPlayTimeX, layout::kPlayTimeY, kColWhite, line);

    if (model.length_frames > 0) {
        const int tot_s = UiModel::frames_to_seconds(model.length_frames);
        std::snprintf(line, sizeof(line), "/ %02d:%02d", tot_s / 60, tot_s % 60);
    } else {
        // Duración desconocida: se indica, no se inventa.
        std::snprintf(line, sizeof(line), "/ --:--");
    }
    r.draw_text(layout::kPlayTotalX, layout::kPlayTimeY, kColTimeTotal, line);

    r.draw_text(layout::kPlayTrackX, layout::kPlayTrackY, kColCounter, "TRACK");
    std::snprintf(line, sizeof(line), "%02d", model.current_track + 1);
    const int num_end = r.draw_text(layout::kPlayNumX, layout::kPlayNumY,
                                    kColCounterBig, line);

    // El numero grande se queda anclado bajo "TRACK"; el total se pega al
    // borde derecho. Con dos cifras eso cae en el mismo pixel de siempre.
    // Si no cabe el espacio tras la barra, se quita ese espacio antes que
    // empujar el total fuera del panel: "102/180", nunca "102/  180".
    char total[16];
    std::snprintf(total, sizeof(total), "/ %zu", model.tracks.size());
    int tx = layout::kPlayNumsRightX - text_width(total) + 1;
    if (tx < num_end) {
        std::snprintf(total, sizeof(total), "/%zu", model.tracks.size());
        tx = layout::kPlayNumsRightX - text_width(total) + 1;
    }
    r.draw_text(tx < num_end ? num_end : tx, layout::kPlayNumY, kColCounter, total);

    // ── Barra de progreso ──
    r.draw_rect(layout::kPlayBarX, layout::kPlayBarY, layout::kPlayBarW, layout::kPlayBarH,
                kColProgBorder);
    r.fill_rect(layout::kPlayFillX, layout::kPlayFillY, layout::kPlayFillW, layout::kPlayFillH,
                kColProgBg);

    const float p = model.progress();
    if (p < 0.0f) {
        // Duración desconocida: barra indeterminada (rayado tenue), no un 0%
        // que parecería "acaba de empezar".
        for (int x = layout::kPlayFillX; x < layout::kPlayFillX + layout::kPlayFillW; x += 4)
            r.fill_rect(x, layout::kPlayFillY, 2, layout::kPlayFillH, kColProgBorder);
    } else {
        const int fillw = static_cast<int>(p * static_cast<float>(layout::kPlayFillW));
        if (fillw > 0)
            r.fill_rect(layout::kPlayFillX, layout::kPlayFillY, fillw, layout::kPlayFillH,
                        kColProgFill);
        const int px = layout::kPlayFillX + fillw;
        r.fill_rect(px - layout::kPlayPtrW / 2, layout::kPlayPtrY, layout::kPlayPtrW,
                    layout::kPlayPtrH, kColProgBorder);
        r.fill_rect(px - layout::kPlayPtrW / 2 + 1, layout::kPlayPtrY + 1,
                    layout::kPlayPtrW - 2, layout::kPlayPtrH - 2, kColWhite);
    }

    // ── Volumen ──
    // Cian normalmente, naranja mientras el modo de ajuste de volumen está
    // activo: el mismo estado que pinta de naranja el borde del botón VOL.
    // Solo cambia la pareja de colores, el trazado no.
    const uint32_t speaker_shadow = model.volume_adjust_active ? kColSpeakerOrangeShadow : kColNpSub;
    const uint32_t speaker_main   = model.volume_adjust_active ? kColBtnToggleOn : kColProgFill;
    draw_pixel_art2(r, layout::kSndIconX, layout::kSndIconY, speaker_shadow, speaker_main,
                    kSpeakerMiniArt, layout::kSndIconH);
    const int sndw = (layout::kSndBarW - 2) * model.volume / 100;
    r.draw_rect(layout::kSndBarX, layout::kSndBarY, layout::kSndBarW, layout::kSndBarH,
                kColProgBorder);
    r.fill_rect(layout::kSndBarX + 1, layout::kSndBarY + 1, layout::kSndBarW - 2,
                layout::kSndBarH - 2, kColProgBg);
    if (sndw > 0) r.fill_rect(layout::kSndBarX + 1, layout::kSndBarY + 1, sndw,
                              layout::kSndBarH - 2, kColPlayArrow);
    for (int tx = layout::kSndBarX + 2; tx <= layout::kSndBarX + layout::kSndBarW - 2; tx += 2)
        r.fill_rect(tx, layout::kSndBarY + 1, 1, layout::kSndBarH - 2, kColProgBorder);
}

// ── DECK: 8 botones. Ver transport.hpp::DeckButton para la semántica de
// cada uno.
inline void draw_deck(Renderer& r, const UiModel& model) {
    r.fill_rect(layout::kDeckX, layout::kDeckY, layout::kDeckW, layout::kDeckH, kColDeck);
    r.draw_line(layout::kDeckX, layout::kDeckY, kScreenW - layout::kDeckX - 1, layout::kDeckY,
                kColDeckBorder);
    r.draw_line(layout::kDeckX, layout::kDeckY + layout::kDeckH - 1,
                kScreenW - layout::kDeckX - 1, layout::kDeckY + layout::kDeckH - 1, kColDeckBorder);
    r.draw_line(layout::kDeckX, layout::kDeckY, layout::kDeckX,
                layout::kDeckY + layout::kDeckH - 1, kColDeckBorder);
    r.draw_line(kScreenW - layout::kDeckX - 1, layout::kDeckY, kScreenW - layout::kDeckX - 1,
                layout::kDeckY + layout::kDeckH - 1, kColDeckBorder);

    for (int b = 0; b < 8; ++b) {
        const int x = layout::kBtnX0 + b * layout::kBtnStep;
        const auto btn = static_cast<DeckButton>(b);
        const bool focused = (b == model.deck_focus);

        // VOLUME, REB y REPEAT pueden quedarse "encendidos" con
        // independencia del foco. Ese estado MANDA sobre el foco: el botón
        // se ve naranja aunque el cursor ya no esté encima, y el cian del
        // foco solo se dibuja si no está encendido, para que los dos no
        // compitan por el mismo borde.
        const bool toggled_on = (btn == DeckButton::Volume && model.volume_adjust_active) ||
                                 (btn == DeckButton::Reverb && model.reverb_enabled) ||
                                 (btn == DeckButton::Repeat && model.repeat != ui::RepeatMode::Off);

        // El estado encendido NO se lleva al borde exterior: ese anillo es
        // siempre neutro y lo que se ilumina es el aro interior. Con el
        // exterior también encendido, el botón quedaba rodeado de tres
        // anillos de color y pesaba demasiado en una barra de ocho.
        // REB y REPEAT llevan el fondo invertido respecto al resto del
        // deck: el foco NO los oscurece por sí solo, aunque estén
        // encendidos -- solo se oscurecen cuando están encendidos Y SIN
        // foco. Así, moverse entre estos dos deja siempre claro el que
        // tiene el foco y oscuro el otro, sin que el foco por sí mismo
        // finja un estado que no es. El resto del deck no cambia.
        const bool is_toggle_btn = (btn == DeckButton::Reverb || btn == DeckButton::Repeat);
        const uint32_t bg = is_toggle_btn
                           ? ((!focused && toggled_on) ? kColBtnActiveBg : kColBtnBg)
                           : ((focused || toggled_on) ? kColBtnActiveBg : kColBtnBg);
        r.fill_rect(x, layout::kBtnY, layout::kBtnW, layout::kBtnH, bg);
        r.draw_rect(x, layout::kBtnY, layout::kBtnW, layout::kBtnH, kColBtnOuter);
        r.draw_rect(x + 1, layout::kBtnY + 1, layout::kBtnW - 2, layout::kBtnH - 2,
                    kColBtnBorder);

        const uint32_t c  = toggled_on ? kColBtnToggleOn : (focused ? kColBtnActiveTx : kColBtnText);
        const uint32_t ci = darken(c, 80);

        // Aro interior, 2px dentro del botón: es el que lleva el estado. A
        // pleno color cuando el botón tiene el foco, y en el tono apagado
        // ('ci') en cualquier otro caso -- incluido el encendido, que ya se
        // distingue por el naranja del icono y por el fondo oscuro.
        r.draw_rect(x + 2, layout::kBtnY + 2, layout::kBtnW - 4, layout::kBtnH - 4,
                    (focused && !toggled_on) ? c : ci);

        switch (btn) {
        case DeckButton::Stop: // ■ reinicia y pausa
            // Tres capas concéntricas: el cuadrado macizo de 9x9 era la
            // mancha más pesada de la barra.
            r.draw_rect(x + 13, layout::kBtnBarsY + 1, 9, 9, kColBtnBorder);
            r.draw_rect(x + 14, layout::kBtnBarsY + 2, 7, 7, ci);
            r.fill_rect(x + 15, layout::kBtnBarsY + 3, 5, 5, c);
            break;
        case DeckButton::PreviousTrack: // |< pista anterior
            r.fill_rect(x + 11, layout::kBtnIconY, 2, 9, ci);
            fill_triangle(r, x + 15, layout::kBtnIconY, 9, 9, false, c);
            draw_triangle(r, x + 15, layout::kBtnIconY, 9, 9, false, ci);
            break;
        case DeckButton::PlayPause: // refleja el estado REAL de reproducción
            if (model.playing) {
                r.fill_rect(x + 13, layout::kBtnBarsY + 1, 3, 9, ci);
                r.fill_rect(x + 18, layout::kBtnBarsY + 1, 3, 9, ci);
            } else {
                fill_triangle(r, x + 14, layout::kBtnIconY, 8, 9, true, c);
                draw_triangle(r, x + 14, layout::kBtnIconY, 8, 9, true, ci);
            }
            break;
        case DeckButton::FastForward: // >>
            fill_triangle(r, x + 9, layout::kBtnIconY, 8, 9, true, c);
            draw_triangle(r, x + 9, layout::kBtnIconY, 8, 9, true, ci);
            fill_triangle(r, x + 18, layout::kBtnIconY, 8, 9, true, c);
            draw_triangle(r, x + 18, layout::kBtnIconY, 8, 9, true, ci);
            break;
        case DeckButton::NextTrack: // >| pista siguiente
            fill_triangle(r, x + 10, layout::kBtnIconY, 9, 9, true, c);
            draw_triangle(r, x + 10, layout::kBtnIconY, 9, 9, true, ci);
            r.fill_rect(x + 21, layout::kBtnIconY, 2, 9, ci);
            break;
        case DeckButton::Volume:
            draw_pixel_art3(r, x + 11, layout::kBtnY + 5, c, ci, kColBtnBorder,
                            kSpeakerDeckArt, 9);
            break;
        case DeckButton::Reverb:
            draw_centered(r, x, layout::kBtnLabelY + 5, layout::kBtnW, c, "REB");
            break;
        case DeckButton::Repeat: // OFF / ALL / ONE
            draw_centered(r, x, layout::kBtnLabelY + 5, layout::kBtnW, c,
                          repeat_mode_label(model.repeat));
            break;
        }
    }

    // Línea de cierre bajo el deck: separa la barra del borde inferior de la
    // pantalla, igual que el resto de paneles llevan su propio remate.
    r.draw_line(layout::kDeckX, layout::kDeckY + layout::kDeckH,
                kScreenW - layout::kDeckX - 1, layout::kDeckY + layout::kDeckH,
                kColSpecBorder);
}

} // namespace detail

// Dibuja la pantalla completa en 'fb' (kScreenW*kScreenH píxeles
// XRGB8888) a partir del modelo. No modifica 'model'.
inline void render(uint32_t* fb, const UiModel& model) {
    using namespace detail;
    Renderer r(fb, kScreenW, kScreenH);
    r.clear(kColBg);

    // ── Header ──
    const int brand_x = r.draw_text8(layout::kHeaderPadX, 3, kColPeakWarm, "A");
    const int brand_o = r.draw_text8(brand_x - 1, 3, kColPeakWarm, "O");
    r.draw_text8(brand_o, 3, kColLogo, "LIB");
    // El motor que está sonando: responde a "¿por qué backend está pasando
    // esto?", que es lo útil al depurar.
    {
        const std::string right = model.engine_name.empty()
            ? std::string("AOLIB Core")
            : ("engine: " + model.engine_name);
        r.draw_text(kScreenW - layout::kHeaderPadX - text_width(right.c_str()), 3,
                    kColSubtitle, right.c_str());
    }
    // En DOS tramos, dejando sin pintar el hueco entre TRACK LIST e INFO:
    // de un tirón, la línea cruzaría esa separación de 2px y "soldaría"
    // visualmente los dos paneles.
    r.draw_line(layout::kMarginX, layout::kHeaderLineY, layout::kListX + layout::kListW - 1,
                layout::kHeaderLineY, kColSpecBorder);
    r.draw_line(layout::kInfoX, layout::kHeaderLineY, kScreenW - layout::kHeaderPadX,
                layout::kHeaderLineY, kColSpecBorder);

    draw_vu_panel(r, model);
    draw_track_list(r, model);
    draw_info_panel(r, model);
    draw_spectrum(r, model);
    draw_playing(r, model);
    draw_deck(r, model);
}

} // namespace ui
