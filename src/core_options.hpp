// core_options.hpp — lectura de las opciones del frontend.
//
// CoreOptions (core_context.hpp) es la única fuente de verdad del estado
// conmutable; esto es lo que la rellena desde RETRO_ENVIRONMENT_GET_VARIABLE,
// incluida la compatibilidad con los valores en español de versiones
// anteriores.
//
// Salió de libretro.cpp al partirlo: ver la cabecera de ese fichero.

#pragma once

#include <cstdlib>
#include <string>

#include "core_globals.hpp"

namespace aolib {
// Lee una variable booleana declarada como "disabled|enabled" (el orden
// solo fija cuál es el valor por defecto; GET_VARIABLE devuelve siempre la
// cadena elegida). 'true_value' es la cadena que significa 'true' hoy.
//
// 'legacy_true_value' (opcional, nullptr = no aplica) existe por
// compatibilidad: estas opciones se declararon en español
// ("habilitado"/"deshabilitado") en versiones anteriores, y RetroArch
// conserva el valor guardado aunque ya no aparezca en la lista declarada.
// Sin aceptarlo como sinónimo, una configuración antigua se leería como
// false en silencio.
static bool get_bool_variable(const char* key, bool current, const char* true_value,
                        const char* legacy_true_value = nullptr) {
    if (!environ_cb) return current;
    retro_variable var{key, nullptr};
    if (!environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || !var.value) return current;
    if (std::strcmp(var.value, true_value) == 0) return true;
    if (legacy_true_value && std::strcmp(var.value, legacy_true_value) == 0) return true;
    return false;
}

// Valor CRUDO de una opción, sin interpretar. Lo necesita
// "aolib_reverb_amount", cuyo "1"/"2"/"3" no es booleano ni un número que
// se use tal cual: el parseo vive en
// dsp::Reverb::amount_from_level_option() para poder probarlo aislado.
// Devuelve nullptr si el frontend no da valor.
static const char* get_raw_variable(const char* key) {
    if (!environ_cb) return nullptr;
    retro_variable var{key, nullptr};
    if (!environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) return nullptr;
    return var.value;
}

static double get_double_variable(const char* key, double current) {
    if (!environ_cb) return current;
    retro_variable var{key, nullptr};
    if (!environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || !var.value) return current;
    char* end = nullptr;
    const double parsed = std::strtod(var.value, &end);
    return (end && end != var.value) ? parsed : current;
}

// Lee las opciones de core y las aplica a g_ctx->options, propagando además
// a set_reverb_enabled() del motor activo (iUseReverb es estado global del
// SPU: hay que reaplicarlo cada vez que la opción cambia, no solo al
// cargar). Se llama tras cada retro_load_game() con éxito y en cada
// retro_run() en que el frontend señala GET_VARIABLE_UPDATE.
static void apply_core_options() {
    if (!g_ctx) return;
    g_ctx->options.loop_infinite = get_bool_variable(
        "aolib_loop_infinite", g_ctx->options.loop_infinite, "enabled", "habilitado");
    g_ctx->options.default_fade_seconds = get_double_variable(
        "aolib_default_fade_seconds", g_ctx->options.default_fade_seconds);
    g_ctx->options.spu_reverb_enabled = get_bool_variable(
        "aolib_spu_reverb_enabled", g_ctx->options.spu_reverb_enabled, "enabled", "habilitado");

    // Se lee aquí, pero solo la consultan los motores que se CONSTRUYEN
    // después: XMP_PLAYER_DEFPAN hay que fijarlo antes de cargar el módulo
    // (ver xmp_engine.hpp), así que cambiarlo con una pista sonando no la
    // afecta -- se nota en la siguiente. Libretro no permite avisar de eso
    // desde el core; queda documentado en el README.
    {
        const double sep = get_double_variable(
            "aolib_xmp_stereo_separation",
            static_cast<double>(g_ctx->options.xmp_stereo_separation));
        g_ctx->options.xmp_stereo_separation =
            static_cast<int>(sep < 0.0 ? 0.0 : (sep > 100.0 ? 100.0 : sep));
    }

    // El menú NO enciende ni apaga el reverb de la capa host: eso es
    // exclusivo del botón REB del deck (ver DeckButton::Reverb). La API de
    // Libretro va en una sola dirección -- el core LEE con GET_VARIABLE y no
    // existe ningún SET_VARIABLE; SET_CORE_OPTIONS_DISPLAY solo afecta a la
    // visibilidad de una opción, no a su valor -- así que un booleano con
    // dos controles quedaría desincronizado en cuanto se pulsara REB, con el
    // menú mostrando "disabled" para siempre y sin forma de corregirlo.
    //
    // Lo que sí hace el menú es fijar el NIVEL al que se encenderá la
    // próxima vez (host_reverb_last_on) y, si el reverb ya está sonando,
    // actualizarlo EN VIVO.
    {
        const char* raw_level = get_raw_variable("aolib_reverb_amount");
        // Un nivel ausente o ilegible cae al último activo, nunca fuerza un
        // apagado: apagado significa otra cosa (que se pulsó REB).
        const int level = dsp::Reverb::amount_from_level_option(
            raw_level, g_ctx->options.host_reverb_last_on);
        g_ctx->options.host_reverb_last_on = level;

        if (g_ctx->options.host_reverb_amount > 0 &&
            g_ctx->options.host_reverb_amount != level) {
            g_ctx->options.host_reverb_amount = level;
        }
    }

    if (g_ctx->engine) g_ctx->engine->set_reverb_enabled(g_ctx->options.spu_reverb_enabled);
}

}  // namespace aolib
