// ui_renderer.cpp — implementación software de las primitivas 2D.
// Todas las primitivas hacen bounds-check sobre el framebuffer.
#include "ui_renderer.hpp"

#include <algorithm>
#include <cstdlib>

#include "ui_font.hpp"

namespace ui {

Renderer::Renderer(uint32_t* fb, int w, int h) : fb_(fb), w_(w), h_(h) {}

void Renderer::clear(uint32_t c) {
    std::fill(fb_, fb_ + w_ * h_, c);
}

void Renderer::draw_pixel(int x, int y, uint32_t c) {
    if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
    fb_[y * w_ + x] = c;
}

void Renderer::fill_rect(int x, int y, int w, int h, uint32_t c) {
    const int x0 = std::max(x, 0);
    const int y0 = std::max(y, 0);
    const int x1 = std::min(x + w, w_);
    const int y1 = std::min(y + h, h_);
    if (x1 <= x0 || y1 <= y0) return;
    for (int yy = y0; yy < y1; ++yy) {
        uint32_t* row = fb_ + yy * w_ + x0;
        std::fill(row, row + (x1 - x0), c);
    }
}

void Renderer::draw_rect(int x, int y, int w, int h, uint32_t c) {
    if (w <= 0 || h <= 0) return;
    fill_rect(x, y, w, 1, c);
    fill_rect(x, y + h - 1, w, 1, c);
    fill_rect(x, y, 1, h, c);
    fill_rect(x + w - 1, y, 1, h, c);
}

void Renderer::draw_line(int x0, int y0, int x1, int y1, uint32_t c) {
    const int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        draw_pixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

int Renderer::draw_text(int x, int y, uint32_t c, const char* s) {
    int cx = x;
    for (; *s != '\0'; ++s) {
        // La 'g' se dibuja 1px más abajo que el resto, para que su
        // descendente quede alineada con las letras sin cola. Se desplaza
        // el punto de dibujo y no el bitmap: la celda del glifo tiene 7
        // filas fijas y la 'g' ya ocupa 6, así que moverlo dentro de la
        // celda recortaría la curva inferior.
        const int glyph_y = (*s == 'g') ? y + 1 : y;
        const uint8_t* rows = glyph_rows(static_cast<unsigned char>(*s));
        for (int r = 0; r < kFontH; ++r) {
            const uint8_t bits = rows[r];
            for (int b = 0; b < kFontW; ++b) {
                if (bits & (0x10 >> b)) draw_pixel(cx + b, glyph_y + r, c);
            }
        }
        cx += kFontW + 1;
    }
    return cx;
}

int Renderer::draw_text8(int x, int y, uint32_t c, const char* s) {
    int cx = x;
    for (; *s != '\0'; ++s) {
        const uint8_t* rows = logo_glyph_rows(static_cast<unsigned char>(*s));
        if (rows) {
            for (int r = 0; r < kLogoFontH; ++r) {
                const uint8_t bits = rows[r];
                for (int b = 0; b < kLogoFontW; ++b) {
                    if (bits & (0x80 >> b)) draw_pixel(cx + b, y + r, c);
                }
            }
        }
        cx += kLogoFontW + 1;
    }
    return cx;
}

} // namespace ui
