// ui_renderer.hpp — renderer 2D software puro, sin dependencias externas
// (ni OpenGL/Vulkan/SDL/ImGui): escribe en el framebuffer uint32_t de
// 320x240 que se entrega a Libretro como XRGB8888.
#pragma once
#include <cstdint>

namespace ui {

// Píxel XRGB8888 nativo: 0x00RRGGBB. El byte más significativo (la "X")
// lo ignora el frontend -- los píxeles son opacos, no hay transparencia.
inline constexpr uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) |
           static_cast<uint32_t>(b);
}

class Renderer {
public:
    Renderer(uint32_t* framebuffer, int width, int height);

    void clear(uint32_t color);
    void fill_rect(int x, int y, int w, int h, uint32_t color);
    void draw_rect(int x, int y, int w, int h, uint32_t color); // borde 1 px
    void draw_line(int x0, int y0, int x1, int y1, uint32_t color);
    void draw_pixel(int x, int y, uint32_t color);
    int  draw_text(int x, int y, uint32_t color, const char* s); // devuelve x final
    int  draw_text8(int x, int y, uint32_t color, const char* s); // logo 8x8 (EuroPC CGA), avanza 9px/char

private:
    uint32_t* fb_;
    int w_;
    int h_;
};

} // namespace ui
