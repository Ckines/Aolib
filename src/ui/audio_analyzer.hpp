// audio_analyzer.hpp — medidor VU y analizador de espectro, calculados
// sobre el audio que el core acaba de entregar a audio_batch_cb.
//
// Miden la señal real, nunca una animación: unas barras que se mueven sin
// relación con el audio son peores que no tener barras, porque parece que
// suena algo cuando no suena. Así el VU y el espectro sirven además como
// instrumento de diagnóstico.
//
// Coste fijo y sin asignaciones: una llamada por retro_run() sobre 735
// frames, en el mismo hilo. Ningún buffer crece.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace ui {

// Nº de bandas del espectro; fijado por el layout de la pantalla.
constexpr int kSpectrumBands = 28;

// Tamaño de la ventana de análisis. 2048 puntos = 21,53 Hz por bin y 46 ms
// de ventana.
//
// NO bajarlo: con 512 puntos la resolución es 86,13 Hz por bin y las bandas
// logarítmicas graves (la escala va de 40 Hz a 16 kHz) son más estrechas
// que un bin hasta pasados los 400 Hz, así que hasta siete bandas
// consecutivas leen el MISMO bin y dibujan el mismo número siete veces.
//
// A cambio, la ventana cubre ~2,8 frames de vídeo y el espectro "recuerda"
// algo de lo anterior: es el compromiso habitual entre resolución en
// frecuencia y en tiempo, y aquí interesa la frecuencia.
//
// Potencia de dos: la FFT de abajo es radix-2.
constexpr int kFftSize = 2048;
constexpr int kFftBins = kFftSize / 2;

class AudioAnalyzer {
public:
    // Alimenta el analizador con el bloque estéreo entrelazado que se
    // acaba de enviar al frontend. 'frames' puede ser menor que kFftSize;
    // la ventana interna es circular y conserva lo anterior.
    void feed(const int16_t* interleaved, std::size_t frames) noexcept {
        if (!interleaved || frames == 0) {
            // Sin audio este frame: el VU decae, no se queda clavado.
            decay_only();
            return;
        }

        int peak_l = 0, peak_r = 0;
        for (std::size_t i = 0; i < frames; ++i) {
            const int l = interleaved[i * 2 + 0];
            const int r = interleaved[i * 2 + 1];
            peak_l = std::max(peak_l, l < 0 ? -l : l);
            peak_r = std::max(peak_r, r < 0 ? -r : r);

            // Mezcla a mono para el espectro (>>1 antes de sumar: no
            // puede desbordar int16 ni con dos canales a fondo de escala).
            window_[write_pos_] = static_cast<float>((l >> 1) + (r >> 1));
            write_pos_ = (write_pos_ + 1) % kFftSize;
        }

        update_vu(peak_l / 32768.0f, peak_r / 32768.0f);
        compute_spectrum();
    }

    // Nivel VU 0..1 por canal, ya suavizado (ataque rápido, caída lenta:
    // el comportamiento de un VU real, y lo que hace legible el
    // movimiento a 60 fps).
    float vu(int channel) const noexcept {
        return vu_[channel == 0 ? 0 : 1];
    }

    // Pico retenido 0..1 por canal (el "peak hold" clásico).
    float vu_peak(int channel) const noexcept {
        return vu_peak_[channel == 0 ? 0 : 1];
    }

    // Altura normalizada 0..1 de la banda 'band' del espectro.
    float band(int b) const noexcept {
        if (b < 0 || b >= kSpectrumBands) return 0.0f;
        return bands_[static_cast<std::size_t>(b)];
    }

    // Pico retenido 0..1 de la banda (línea que cae despacio).
    float band_peak(int b) const noexcept {
        if (b < 0 || b >= kSpectrumBands) return 0.0f;
        return band_peak_[static_cast<std::size_t>(b)];
    }

    // ¿Hay señal real ahora mismo? Umbral deliberadamente bajo
    // (-72 dBFS): distingue "silencio digital exacto" de "muy bajo".
    bool has_signal() const noexcept {
        return vu_[0] > 0.00025f || vu_[1] > 0.00025f;
    }

    void reset() noexcept {
        window_.fill(0.0f);
        write_pos_ = 0;
        vu_ = {0.0f, 0.0f};
        vu_peak_ = {0.0f, 0.0f};
        vu_peak_hold_ = {0, 0};
        bands_.fill(0.0f);
        band_peak_.fill(0.0f);
        band_peak_hold_.fill(0);
    }

private:
    static constexpr float kAttack      = 0.55f;  // subida: rápida
    static constexpr float kRelease     = 0.09f;  // caída: lenta
    static constexpr int   kPeakHoldFrames = 36;  // ~0,6 s a 60 fps
    static constexpr float kPeakFall    = 0.020f;

    // Suelo de silencio. La caída del VU es MULTIPLICATIVA (*0,91 por
    // frame), así que sin suelo nunca llega a cero exacto: sigue bajando por
    // los denormales durante miles de frames, y como el panel imprime
    // 20*log10(v) mientras v>0, en pausa acaba mostrando valores absurdos
    // (-883 dB para v = 1e-44). Por debajo de -80 dBFS no hay nada que
    // medir: se fija a cero exacto y el panel dibuja "-inf".
    static constexpr float kSilenceFloor = 1.0e-4f;   // -80 dBFS

    void decay_only() noexcept {
        for (int ch = 0; ch < 2; ++ch) {
            vu_[static_cast<std::size_t>(ch)] *= (1.0f - kRelease);
        }
        clamp_silence();
        fall_peaks();
        for (auto& b : bands_) b *= (1.0f - kRelease);
    }

    // Fija a cero exacto todo lo que ya está por debajo del suelo audible.
    void clamp_silence() noexcept {
        for (std::size_t ch = 0; ch < 2; ++ch) {
            if (vu_[ch]      < kSilenceFloor) vu_[ch]      = 0.0f;
            if (vu_peak_[ch] < kSilenceFloor) vu_peak_[ch] = 0.0f;
        }
    }

    void update_vu(float raw_l, float raw_r) noexcept {
        const float raw[2] = {raw_l, raw_r};
        for (std::size_t ch = 0; ch < 2; ++ch) {
            const float target = raw[ch];
            const float coeff = (target > vu_[ch]) ? kAttack : kRelease;
            vu_[ch] += (target - vu_[ch]) * coeff;
            if (vu_[ch] >= vu_peak_[ch]) {
                vu_peak_[ch] = vu_[ch];
                vu_peak_hold_[ch] = kPeakHoldFrames;
            }
        }
        clamp_silence();
        fall_peaks();
    }

    void fall_peaks() noexcept {
        for (std::size_t ch = 0; ch < 2; ++ch) {
            if (vu_peak_hold_[ch] > 0) --vu_peak_hold_[ch];
            else vu_peak_[ch] = std::max(0.0f, vu_peak_[ch] - kPeakFall);
        }
        for (std::size_t b = 0; b < kSpectrumBands; ++b) {
            if (band_peak_hold_[b] > 0) --band_peak_hold_[b];
            else band_peak_[b] = std::max(0.0f, band_peak_[b] - kPeakFall);
        }
    }

    // FFT radix-2 in-place, iterativa (Cooley-Tukey). Coste acotado y
    // despreciable frente a emular un YM2612 o un R3000A.
    void compute_spectrum() noexcept {
        // Copia la ventana circular en orden temporal correcto y aplica
        // Hann (sin ventana, un tono puro se esparce por todas las bandas
        // y el espectro parece ruido).
        for (int i = 0; i < kFftSize; ++i) {
            const int src = (write_pos_ + i) % kFftSize;
            const float hann = 0.5f - 0.5f * std::cos(2.0f * 3.14159265f * i / (kFftSize - 1));
            re_[static_cast<std::size_t>(i)] = window_[static_cast<std::size_t>(src)] * hann / 32768.0f;
            im_[static_cast<std::size_t>(i)] = 0.0f;
        }

        // Reordenación bit-reversa.
        for (int i = 1, j = 0; i < kFftSize; ++i) {
            int bit = kFftSize >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) {
                std::swap(re_[static_cast<std::size_t>(i)], re_[static_cast<std::size_t>(j)]);
                std::swap(im_[static_cast<std::size_t>(i)], im_[static_cast<std::size_t>(j)]);
            }
        }

        for (int len = 2; len <= kFftSize; len <<= 1) {
            const float ang = -2.0f * 3.14159265f / static_cast<float>(len);
            const float wr = std::cos(ang), wi = std::sin(ang);
            for (int i = 0; i < kFftSize; i += len) {
                float cur_wr = 1.0f, cur_wi = 0.0f;
                for (int k = 0; k < len / 2; ++k) {
                    const std::size_t a = static_cast<std::size_t>(i + k);
                    const std::size_t b = static_cast<std::size_t>(i + k + len / 2);
                    const float tr = re_[b] * cur_wr - im_[b] * cur_wi;
                    const float ti = re_[b] * cur_wi + im_[b] * cur_wr;
                    re_[b] = re_[a] - tr;  im_[b] = im_[a] - ti;
                    re_[a] += tr;          im_[a] += ti;
                    const float next_wr = cur_wr * wr - cur_wi * wi;
                    cur_wi = cur_wr * wi + cur_wi * wr;
                    cur_wr = next_wr;
                }
            }
        }

        // Magnitud NORMALIZADA por bin, en unidades de fondo de escala.
        //
        // La normalización es OBLIGATORIA: una FFT de N puntos no devuelve
        // la amplitud de la señal. Para un tono de amplitud A dentro de un
        // bin, con ventana Hann (ganancia coherente 0,5), el módulo del bin
        // vale A*N/4 -- decenas de dB de offset. Sin dividir por N/4,
        // cualquier banda por encima de ese offset satura a 1,0 y las barras
        // se quedan clavadas en el tope con música normal, moviéndose solo
        // al bajar el volumen.
        for (int k = 0; k < kFftBins; ++k) {
            const std::size_t kk = static_cast<std::size_t>(k);
            mag_[kk] = std::sqrt(re_[kk] * re_[kk] + im_[kk] * im_[kk]) * kMagScale;
        }

        // Agrupación logarítmica: una escala lineal pondría 24 de 28 bandas
        // por encima de 5 kHz, donde la música de chip casi no tiene
        // energía, y el espectro se vería plano. Log reparte por octavas,
        // que es como se oye.
        //
        // Los bordes de banda son FRACCIONARIOS. En las bandas anchas
        // (agudos, varios bins dentro) se toma el pico de los bins
        // contenidos; en las estrechas (graves, menos de un bin de ancho) se
        // interpola la magnitud en el centro de la banda. Truncar a un
        // índice entero haría que varias bandas consecutivas leyeran el
        // mismo bin y devolvieran el mismo valor exacto.
        for (int b = 0; b < kSpectrumBands; ++b) {
            const float x0 = band_edge(b)     * kBinsPerHz;
            const float x1 = band_edge(b + 1) * kBinsPerHz;

            float acc;
            const int k0 = static_cast<int>(std::ceil(x0));
            const int k1 = static_cast<int>(std::floor(x1));
            if (k1 > k0) {
                acc = 0.0f;
                for (int k = std::max(1, k0); k <= std::min(k1, kFftBins - 1); ++k)
                    acc = std::max(acc, mag_[static_cast<std::size_t>(k)]);
            } else {
                acc = mag_at((x0 + x1) * 0.5f);
            }

            // A dB y normalizado sobre kRangeDb dB de rango útil por
            // debajo de fondo de escala.
            const float db = 20.0f * std::log10(acc + 1e-9f);
            float norm = (db + kRangeDb) / kRangeDb;
            norm = std::clamp(norm, 0.0f, 1.0f);

            const std::size_t bi = static_cast<std::size_t>(b);
            const float coeff = (norm > bands_[bi]) ? kAttack : kRelease;
            bands_[bi] += (norm - bands_[bi]) * coeff;
            if (bands_[bi] >= band_peak_[bi]) {
                band_peak_[bi] = bands_[bi];
                band_peak_hold_[bi] = kPeakHoldFrames;
            }
        }
    }

    // Bordes de banda de 40 Hz a 16 kHz, espaciados logarítmicamente.
    static float band_edge(int b) noexcept {
        constexpr float kLo = 40.0f, kHi = 16000.0f;
        const float t = static_cast<float>(b) / static_cast<float>(kSpectrumBands);
        return kLo * std::pow(kHi / kLo, t);
    }

    // Magnitud en una posición de bin FRACCIONARIA, por interpolación
    // lineal entre los dos bins vecinos. El bin 0 (DC) se excluye a
    // propósito: un offset de continua no es sonido y llenaría la primera
    // barra permanentemente.
    float mag_at(float x) const noexcept {
        x = std::clamp(x, 1.0f, static_cast<float>(kFftBins - 2));
        const int i = static_cast<int>(x);
        const float f = x - static_cast<float>(i);
        const float a = mag_[static_cast<std::size_t>(i)];
        const float b = mag_[static_cast<std::size_t>(i + 1)];
        return a + (b - a) * f;
    }

    // 4/N: convierte el módulo de la FFT (ventana Hann) en amplitud relativa
    // a fondo de escala; ver compute_spectrum().
    static constexpr float kMagScale  = 4.0f / static_cast<float>(kFftSize);
    // Hz -> posición de bin (fraccionaria).
    static constexpr float kBinsPerHz = static_cast<float>(kFftSize) / 44100.0f;
    // Rango dinámico visible del espectro, en dB por debajo de fondo de
    // escala. 66 dB deja las barras a media altura para una banda a
    // -33 dBFS, que es un nivel típico de un armónico de chip.
    static constexpr float kRangeDb   = 66.0f;

    std::array<float, kFftSize> window_{};
    std::array<float, kFftBins> mag_{};
    std::array<float, kFftSize> re_{};
    std::array<float, kFftSize> im_{};
    int write_pos_ = 0;

    std::array<float, 2> vu_{};
    std::array<float, 2> vu_peak_{};
    std::array<int, 2>   vu_peak_hold_{};

    std::array<float, kSpectrumBands> bands_{};
    std::array<float, kSpectrumBands> band_peak_{};
    std::array<int, kSpectrumBands>   band_peak_hold_{};
};

} // namespace ui
