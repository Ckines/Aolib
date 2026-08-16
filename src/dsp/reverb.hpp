// reverb.hpp — reverb opcional de la CAPA HOST.
//
// ── Por qué aquí y no en los motores ──────────────────────────────────
//
// Como la ganancia de volumen, el efecto vive en el host y nunca dentro de
// un backend:
//
//   1. Una implementación en el host sirve para los cinco formatos.
//   2. No se confunde con el reverb del SPU ('aolib_spu_reverb_enabled',
//      IAudioEngine::set_reverb_enabled): aquel es hardware emulado de
//      PlayStation, parte de cómo sonaba la consola, y viene ACTIVADO.
//      Este es un efecto de reproductor añadido por encima, y viene
//      DESACTIVADO. Coexisten.
//
// ── El algoritmo ──────────────────────────────────────────────────────
//
// Schroeder en la variante de Freeverb (Jezar, dominio público): filtros de
// peine en paralelo con amortiguación en el lazo de realimentación,
// seguidos de paso-todo en serie. 4 peines y 2 paso-todo por canal en vez
// de los 8+4 del Freeverb completo: con música de chip, de contenido
// espectral mucho más pobre que una mezcla moderna, la densidad de ecos ya
// es suficiente.
//
// Las longitudes son las de Freeverb a 44100 Hz y NO se reescalan por
// frecuencia de muestreo: el core siempre emite a 44100
// (retro_get_system_av_info lo declara fijo y los cinco motores están
// configurados a esa tasa). Si eso cambia, hay que reescalarlas aquí.
//
// ── Bit-perfect con el efecto apagado ─────────────────────────────────
//
// El bypass es ESTRUCTURAL, no numérico: con el efecto apagado
// libretro.cpp no llama a process() en absoluto, así que el PCM no pasa
// por aquí ni se multiplica por 1,0 y no hay redondeo que pueda alterar
// una muestra. Comprobado en tests/f13_reverb_regression.cpp.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dsp {

// Un filtro de peine con amortiguación (paso bajo de un polo en el lazo).
class CombFilter {
public:
    void resize(std::size_t len) {
        buf_.assign(len, 0.0f);
        pos_ = 0;
        filter_store_ = 0.0f;
    }
    void clear() {
        std::fill(buf_.begin(), buf_.end(), 0.0f);
        pos_ = 0;
        filter_store_ = 0.0f;
    }
    inline float process(float input, float feedback, float damp) {
        const float output = buf_[pos_];
        // Paso bajo de un polo: cada eco sucesivo pierde agudos, que es lo
        // que hace que suene a sala y no a lata.
        filter_store_ = output * (1.0f - damp) + filter_store_ * damp;
        filter_store_ = flush_denormal(filter_store_);
        float v = input + filter_store_ * feedback;
        v = flush_denormal(v);
        buf_[pos_] = v;
        if (++pos_ >= buf_.size()) pos_ = 0;
        return output;
    }

    // Un lazo realimentado genera denormales SIEMPRE que la señal se va a
    // silencio: el valor decae hacia cero sin llegar nunca, y en x86 operar
    // con denormales cuesta decenas de ciclos. Sin esto, el coste por frame
    // subiría justo cuando no suena nada.
    static inline float flush_denormal(float v) {
        return (std::fabs(v) < 1.0e-18f) ? 0.0f : v;
    }

private:
    std::vector<float> buf_;
    std::size_t pos_ = 0;
    float filter_store_ = 0.0f;
};

// Filtro paso-todo de Schroeder: difumina los ecos sin colorear la
// respuesta en frecuencia.
class AllpassFilter {
public:
    void resize(std::size_t len) {
        buf_.assign(len, 0.0f);
        pos_ = 0;
    }
    void clear() {
        std::fill(buf_.begin(), buf_.end(), 0.0f);
        pos_ = 0;
    }
    inline float process(float input, float feedback) {
        const float bufout = buf_[pos_];
        const float output = -input + bufout;
        float v = input + bufout * feedback;
        v = CombFilter::flush_denormal(v);
        buf_[pos_] = v;
        if (++pos_ >= buf_.size()) pos_ = 0;
        return output;
    }

private:
    std::vector<float> buf_;
    std::size_t pos_ = 0;
};

class Reverb {
public:
    static constexpr int kCombs   = 4;
    static constexpr int kAllpass = 2;

    // ── Cantidades expuestas en el menú del frontend ──────────────────
    //
    // Encender y apagar es EXCLUSIVO del botón REB del deck. El menú solo
    // expone "Player Reverb Amount" (1|2|3), es decir el NIVEL al que se
    // encenderá la próxima vez -- y si el reverb ya está sonando, el nivel
    // se actualiza en vivo (ver apply_core_options()).
    //
    // No hay toggle equivalente en el menú A PROPÓSITO: la API de Libretro
    // es de solo lectura para el core (GET_VARIABLE; SET_CORE_OPTIONS_DISPLAY
    // solo afecta a la visibilidad de una opción, no a su valor). Un
    // booleano duplicado en menú y deck se desincronizaría en cuanto se
    // pulsara REB, sin forma de corregirlo desde aquí.
    //
    // Tres niveles fijos, no un porcentaje libre: 1=35%, 2=50%, 3=65%. El
    // techo del 65% es decisión de producto, no límite del DSP: la cota
    // aritmética de no-saturación (ver kInputGain) aguanta hasta el 100%,
    // y los tests ejercitan ese 100% a propósito para comprobar la
    // garantía completa, no solo el tramo alcanzable desde el menú.
    static constexpr int kMaxAmount     = 65;
    static constexpr int kLevel1Amount  = 35;   // "1" en el menú
    static constexpr int kLevel2Amount  = 50;   // "2" en el menú
    static constexpr int kLevel3Amount  = 65;   // "3" en el menú
    static constexpr int kDefaultOnAmount = kLevel1Amount; // al encender con el botón REB

    // "1"/"2"/"3" -> 35/50/65. Tolerante a propósito: cualquier otra cosa
    // cae a 'fallback', porque un frontend puede devolver un valor guardado
    // por una versión anterior del core.
    static int amount_from_level_option(const char* value, int fallback) {
        if (!value || !*value) return fallback;
        if (value[0] == '1') return kLevel1Amount;
        if (value[0] == '2') return kLevel2Amount;
        if (value[0] == '3') return kLevel3Amount;
        return fallback;
    }

    // Longitudes de Freeverb a 44100 Hz (primos entre sí a propósito: si
    // compartieran divisores, los ecos coincidirían y sonaría metálico).
    static constexpr std::size_t kCombLen[kCombs]     = {1116, 1188, 1277, 1356};
    static constexpr std::size_t kAllpassLen[kAllpass] = {556, 441};
    // Desfase del canal derecho, para que los dos canales no sean la misma
    // señal exacta (que es lo que colapsaría la imagen estéreo a mono).
    static constexpr std::size_t kStereoSpread = 23;

    Reverb() {
        for (int i = 0; i < kCombs; ++i) {
            comb_l_[i].resize(kCombLen[i]);
            comb_r_[i].resize(kCombLen[i] + kStereoSpread);
        }
        for (int i = 0; i < kAllpass; ++i) {
            ap_l_[i].resize(kAllpassLen[i]);
            ap_r_[i].resize(kAllpassLen[i] + kStereoSpread);
        }
    }

    // Toda la memoria se reserva en el constructor y no se vuelve a tocar:
    // process() no asigna. Una asignación dentro de retro_run() es lo que
    // produce picos por encima del presupuesto de 16,667 ms.
    std::size_t bytes_allocated() const {
        std::size_t n = 0;
        for (int i = 0; i < kCombs; ++i) n += (kCombLen[i] * 2 + kStereoSpread);
        for (int i = 0; i < kAllpass; ++i) n += (kAllpassLen[i] * 2 + kStereoSpread);
        return n * sizeof(float);
    }

    // Vacía la cola. OBLIGATORIO en cambio de pista y en reset: si no, la
    // cola de la pista anterior se oye encima de la siguiente. Va en el
    // mismo punto que AudioAnalyzer::reset().
    void clear() {
        for (int i = 0; i < kCombs; ++i)   { comb_l_[i].clear(); comb_r_[i].clear(); }
        for (int i = 0; i < kAllpass; ++i) { ap_l_[i].clear();   ap_r_[i].clear(); }
        clipped_ = 0;
    }

    // 'amount' 0..1. Procesa in-place un bloque intercalado L,R.
    //
    // No debe llamarse con amount == 0: el bypass es estructural y
    // responsabilidad del llamador (ver cabecera). Si aun así llega un 0,
    // se comporta como identidad exacta.
    void process(int16_t* buf, std::size_t frames, float amount) {
        if (amount <= 0.0f) return;
        amount = std::min(amount, 1.0f);

        // El seco se ATENÚA al subir el húmedo en vez de sumarse encima:
        // así la suma de ambas ramas no crece con 'amount' y no hace falta
        // limitador.
        const float wet = kWetScale * amount;
        const float dry = 1.0f - kDryDuck * amount;

        for (std::size_t i = 0; i < frames; ++i) {
            const float in_l = static_cast<float>(buf[i * 2 + 0]);
            const float in_r = static_cast<float>(buf[i * 2 + 1]);
            // Entrada mono al reverb (estándar en Freeverb): evita que una
            // señal muy lateralizada meta el doble de energía en la cola
            // que una centrada.
            const float input = (in_l + in_r) * 0.5f * kInputGain;

            float out_l = 0.0f, out_r = 0.0f;
            for (int c = 0; c < kCombs; ++c) {
                out_l += comb_l_[c].process(input, kFeedback, kDamp);
                out_r += comb_r_[c].process(input, kFeedback, kDamp);
            }
            for (int a = 0; a < kAllpass; ++a) {
                out_l = ap_l_[a].process(out_l, kApFeedback);
                out_r = ap_r_[a].process(out_r, kApFeedback);
            }

            buf[i * 2 + 0] = mix_to_i16(in_l, out_l, dry, wet);
            buf[i * 2 + 1] = mix_to_i16(in_r, out_r, dry, wet);
        }
    }

    // Muestras recortadas por saturación desde el último clear(). Debe
    // quedarse en 0: existe para que un test pueda demostrarlo, y para
    // cazarlo si alguien toca las ganancias.
    unsigned long clipped() const { return clipped_; }

private:
    inline int16_t mix_to_i16(float dry_in, float wet_in, float dry, float wet) {
        const float v = dry_in * dry + wet_in * wet;
        if (v > 32767.0f)  { ++clipped_; return  32767; }
        if (v < -32768.0f) { ++clipped_; return -32768; }
        return static_cast<int16_t>(v);
    }

    // Sala media-grande, amortiguación moderada. Fijos a propósito:
    // exponer roomsize/damp multiplicaría las combinaciones a validar sin
    // aportar nada que no se consiga moviendo la cantidad.
    static constexpr float kFeedback   = 0.84f;   // ~ roomsize 0,5 de Freeverb
    static constexpr float kDamp       = 0.40f;
    static constexpr float kApFeedback = 0.50f;   // valor clásico de Schroeder

    // ── Por qué estos tres números, y no otros ────────────────────────
    //
    // Están elegidos para que la saturación sea ARITMÉTICAMENTE imposible,
    // no ajustados a ojo hasta que dejara de oírse. Cota del peor caso:
    //
    //   ganancia de un peine en régimen permanente = 1/(1-kFeedback) = 6,25
    //   los 4 peines van en PARALELO y se suman ................. x4 = 25
    //   cota transitoria de un paso-todo = 1/(1-kApFeedback) = 2
    //   dos paso-todo en serie ............................... x4 = 100
    //
    // Es decir: la cadena puede multiplicar por 100 en el peor caso
    // absoluto. Con kInputGain = 0,004 la rama húmeda queda acotada a
    // 0,40 del fondo de escala, y con kDryDuck = 0,45 la seca vale 0,55 a
    // cantidad máxima. Suma máxima posible: 0,95 -- por debajo de 1,0
    // SIEMPRE, para cualquier entrada, sin necesidad de limitador.
    //
    // El recorte de mix_to_i16() se queda como red de seguridad y como
    // instrumento de medida (clipped()), no porque se espere que actúe: si
    // alguien toca estos números y rompe la cota, lo dice un test en vez de
    // aparecer como un crujido en algún rip raro.
    static constexpr float kInputGain  = 0.004f;  // atenuación de entrada al lazo
    static constexpr float kWetScale   = 1.00f;
    static constexpr float kDryDuck    = 0.45f;   // el seco baja 45% a cantidad máxima

    CombFilter    comb_l_[kCombs],   comb_r_[kCombs];
    AllpassFilter ap_l_[kAllpass],   ap_r_[kAllpass];
    unsigned long clipped_ = 0;
};

} // namespace dsp
