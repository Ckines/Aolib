// xmp_engine.hpp — motor de módulos tracker (MOD/S3M/XM/IT) sobre
// libxmp-lite (MIT; versión y notas de vendorizado en
// deps/libxmp-lite/VENDOR.md).
//
// libxmp-lite es el subconjunto de libxmp con LIBXMP_CORE_PLAYER: cuatro
// loaders, sin depackers y sin carga de instrumentos externos. Eso importa
// aquí por dos razones concretas:
//   - Ningún camino de código llama a fopen(). La rama de mod_load.c que
//     abre ficheros de instrumento sueltos (ProTracker "song files") está
//     dentro de #ifndef LIBXMP_CORE_PLAYER. Se cumple la regla de E/S del
//     proyecto sin puentear nada por el VFS.
//   - No hay estado global mutable: no existe una sola variable de fichero
//     no-const en src/. Cada xmp_context es independiente, así que varios
//     motores pueden coexistir (es justo lo que hace la precomputación de
//     metadatos por entrada del .zip) sin el problema de estado de proceso
//     que obliga a AosdkPsxCoreGuard.
//
// UNIDADES en la API, que no coinciden entre motores:
//   - gme_play()          -> MUESTRAS int16 entrelazadas
//   - PlayerA::Render()   -> BYTES
//   - xmp_play_buffer()   -> BYTES, igual que libvgm. Pasar max_frames
//     rellenaría un cuarto del bloque.
//
// ORDEN DE LLAMADAS obligatorio, con una trampa real:
//   xmp_create_context -> xmp_set_player(XMP_PLAYER_DEFPAN) ->
//   xmp_load_module_from_memory -> xmp_start_player -> [render...] ->
//   xmp_end_player -> xmp_release_module -> xmp_free_context.
//
//   DEFPAN TIENE que ir ANTES de cargar: control.c rechaza ese parámetro
//   con -XMP_ERROR_STATE en cuanto ctx->state >= XMP_STATE_LOADED, porque
//   el panorama por defecto se aplica al construir los canales durante la
//   carga. Puesto después compila, enlaza y no cambia nada -- falla en
//   silencio, que es el modo de fallo que este proyecto persigue.
//
// CAMPO 'chip': .MOD -> "PAULA"; .S3M/.XM/.IT -> el tracker detectado.
// Ver chip_label().
//
// FIN DE PISTA: libxmp NO para nunca por su cuenta con loop=0. Un módulo
// vuelve a su restart position indefinidamente. La duración la decide este
// motor a partir de xmp_frame_info::total_time (el escaneo que libxmp hace
// al cargar, una pasada completa) más el fade configurado, igual que
// GmeEngine hace con play_length + fade y LibvgmEngine con
// songLen + fadeSmpls. El fade es propio: libxmp no tiene ninguno.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <string>

#include "iaudio_engine.hpp"

extern "C" {
#include "xmp.h"
}

// Sin -DLIBXMP_STATIC, xmp.h declara cada símbolo __declspec(dllimport) en
// el build de Windows y el enlazado del .dll falla con referencias
// __imp_xmp_*. La define va en XMP_CFLAGS del Makefile, que se añade a
// CXXFLAGS: aplica también a esta cabecera y a quien la incluya.
#if defined(_WIN32) && !defined(LIBXMP_STATIC)
#error "xmp_engine.hpp: falta -DLIBXMP_STATIC (ver XMP_CFLAGS en el Makefile)."
#endif

class XmpEngine final : public IAudioEngine {
public:
    // Misma forma que LibvgmEngine: escalares ya resueltos, no una
    // referencia a CoreOptions.
    //
    // 'stereo_separation' es el XMP_PLAYER_DEFPAN de libxmp, 0..100. El
    // valor por defecto de la librería es 100 = panorama Amiga duro
    // (canales 1/4 a la izquierda, 2/3 a la derecha), que en auriculares
    // separa la mezcla en dos mitades. 50 es el valor que las propias
    // notas de la versión 4.7.1 recomiendan usar.
    // 'format' es lo que ya determinó el despachador por extensión. Solo
    // gobierna la etiqueta de TrackMetadata::chip; la reproducción es
    // idéntica en los cuatro formatos. Auto queda para la rama de
    // identificación por contenido, donde no hay extensión que mirar.
    enum class Format { Auto, Mod, S3m, Xm, It };

    explicit XmpEngine(int fade_length_msecs, bool loop_infinite,
                       Format format = Format::Auto,
                       int stereo_separation = kDefaultSeparation) noexcept
        : fade_length_msecs_(fade_length_msecs),
          loop_infinite_(loop_infinite),
          format_(format),
          separation_(clamp_separation(stereo_separation)) {}

    ~XmpEngine() override { close(); }

    XmpEngine(const XmpEngine&) = delete;
    XmpEngine& operator=(const XmpEngine&) = delete;

    bool open(const char* /*uri*/, const uint8_t* data, std::size_t size,
              IVFSBridge& /*vfs*/) override {
        if (!data || size == 0) return false;
        // xmp_load_module_from_memory toma un long: en Windows (LLP64) son
        // 32 bits. El límite real es kMaxZipEntryBytes (64 MB), muy por
        // debajo, pero la comprobación queda por si esa constante cambia.
        if (size > static_cast<std::size_t>(0x7FFFFFFF)) return false;

        ctx_ = xmp_create_context();
        if (!ctx_) return false;

        // ANTES de cargar. Ver cabecera.
        xmp_set_player(ctx_, XMP_PLAYER_DEFPAN, separation_);

        if (xmp_load_module_from_memory(ctx_, data, static_cast<long>(size)) != 0) {
            close();
            return false;
        }
        module_loaded_ = true;

        // format = 0 -> int16 con signo, estéreo, entrelazado: exactamente
        // el contrato de render(). Cero conversión de formato.
        if (xmp_start_player(ctx_, kSampleRate, 0) != 0) {
            close();
            return false;
        }
        player_started_ = true;

        refresh_metadata();
        reset_playback_state();
        return true;
    }

    // NO asigna memoria: xmp_play_buffer() solo hace memcpy desde el buffer
    // de tick que xmp_start_player() ya reservó (mixer.c: calloc dentro de
    // libxmp_mixer_on, llamado desde start_player y de ningún otro sitio).
    std::size_t render(int16_t* out, std::size_t max_frames) noexcept override {
        if (!ctx_ || !player_started_ || finished_ || max_frames == 0) return 0;

        // BYTES, no frames. Ver cabecera.
        const int bytes = static_cast<int>(max_frames) * kBytesPerFrame;

        // loop = 0 -> libxmp no corta nunca por su cuenta. El corte lo
        // decide fade_and_cut(). El valor negativo solo llega en módulos
        // degenerados (mod->len <= 0) o si el estado del player se rompió.
        if (xmp_play_buffer(ctx_, out, bytes, 0) < 0) {
            finished_ = true;
            return 0;
        }

        const std::size_t produced = fade_and_cut(out, max_frames);
        position_frames_ += produced;
        return produced;
    }

    bool end_of_track() const noexcept override { return finished_ || !ctx_; }

    // Un módulo es una sola pista. libxmp expone además las "secuencias"
    // (xmp_module_info::num_sequences), patrones alcanzables que no cuelgan
    // del orden principal; en la práctica casi siempre son restos de
    // edición, no canciones ocultas, y publicarlas como pistas llenaría la
    // lista de entradas fantasma. Se ignoran a propósito.
    unsigned track_count() const noexcept override { return 1; }

    // select_track(0) = reinicio, el mismo contrato que LibvgmEngine y
    // PsfEngine. Lo usan la repetición ONE y la vuelta al principio con
    // loop_infinite (ver retro_run).
    //
    // NO se usa xmp_restart_module(): control.c solo pone pos = -1,
    // loop_count = 0 y libxmp_reset_flow(). Deja intactos xc_data (memoria
    // de efectos por canal), las voces del mixer y las colas de anticlick,
    // así que el reinicio arrastra la cola de lo que sonaba. Medido: con
    // xmp_restart_module() el primer bloque tras reiniciar NO coincide con
    // el de un arranque limpio; con end_player+start_player coincide bit a
    // bit. Lo mismo vale para xmp_set_position(0).
    //
    // xmp_start_player() reasigna los buffers del mixer (~150 KB). Es
    // aceptable porque select_track() es camino FRÍO -- lo llama retro_run
    // al cambiar de pista, nunca render(). El invariante que importa
    // (render() no asigna) se mantiene.
    bool select_track(unsigned index) override {
        if (index != 0 || !ctx_ || !player_started_) return false;
        xmp_end_player(ctx_);
        player_started_ = false;
        if (xmp_start_player(ctx_, kSampleRate, 0) != 0) return false;
        player_started_ = true;
        reset_playback_state();
        return true;
    }

    const TrackMetadata& metadata() const noexcept override { return meta_; }
    const char* engine_name() const noexcept override { return "libxmp-lite"; }

private:
    static constexpr int kSampleRate    = 44100;
    static constexpr int kBytesPerFrame = 4;      // 2 canales * int16
    static constexpr int kDefaultSeparation = 50;

    static int clamp_separation(int v) noexcept {
        return v < 0 ? 0 : (v > 100 ? 100 : v);
    }

    void close() noexcept {
        if (!ctx_) return;
        // Orden inverso estricto al de open(): end_player antes que
        // release_module (libera xc_data, que apunta a datos del módulo) y
        // release_module antes que free_context.
        if (player_started_) xmp_end_player(ctx_);
        if (module_loaded_)  xmp_release_module(ctx_);
        xmp_free_context(ctx_);
        ctx_ = nullptr;
        player_started_ = false;
        module_loaded_  = false;
    }

    void reset_playback_state() noexcept {
        position_frames_ = 0;
        finished_        = false;
    }

    // Aplica el fundido y decide el final. Devuelve cuántos frames del
    // bloque son válidos; el resto lo rellena de silencio el llamante.
    //
    // Aritmética entera en punto fijo Q16, no float: la salida tiene que
    // ser reproducible bit a bit entre plataformas para poder seguir
    // verificando con hashes FNV-1a sobre rips reales.
    std::size_t fade_and_cut(int16_t* out, std::size_t frames) noexcept {
        // cut_frames_ == 0 significa "sin corte": loop_infinite activo, o
        // un módulo cuyo escaneo no dio duración fiable. En ambos casos
        // manda el frontend (botón de siguiente pista), no este motor.
        if (cut_frames_ == 0) return frames;

        const uint64_t start = position_frames_;
        if (start >= cut_frames_) { finished_ = true; return 0; }

        const uint64_t fade_begin = fade_start_frames_;
        if (start + frames <= fade_begin) return frames;   // aún en seco

        // Q32 para que el paso por frame no se trunque a cero en fades
        // largos (8 s = 352800 frames).
        const uint64_t step = (static_cast<uint64_t>(1) << 32) / fade_len_frames_;

        for (std::size_t i = 0; i < frames; ++i) {
            const uint64_t pos = start + i;
            if (pos < fade_begin) continue;
            if (pos >= cut_frames_) {
                std::memset(out + i * 2, 0, (frames - i) * kBytesPerFrame);
                finished_ = true;
                return i;
            }
            const uint64_t remaining = cut_frames_ - pos;
            const uint32_t gain_q16 =
                static_cast<uint32_t>((remaining * step) >> 16);
            out[i * 2]     = scale_q16(out[i * 2],     gain_q16);
            out[i * 2 + 1] = scale_q16(out[i * 2 + 1], gain_q16);
        }
        return frames;
    }

    static int16_t scale_q16(int16_t s, uint32_t gain_q16) noexcept {
        // gain_q16 <= 65536 y |s| <= 32768: el producto cabe en int64 sin
        // saturar, y el resultado nunca excede el rango de int16 porque la
        // ganancia es <= 1.0.
        const int64_t v = (static_cast<int64_t>(s) * gain_q16) >> 16;
        return static_cast<int16_t>(v);
    }

    void refresh_metadata() noexcept {
        meta_ = TrackMetadata{};
        if (!ctx_) return;

        struct xmp_module_info mi;
        std::memset(&mi, 0, sizeof(mi));
        xmp_get_module_info(ctx_, &mi);

        if (mi.mod) {
            meta_.title = trim(mi.mod->name);
            meta_.chip  = chip_label(mi.mod->type);
        }
        // IT/S3M/XM guardan un mensaje de texto libre; es el único campo
        // textual adicional que un módulo trae de verdad. No se deduce
        // autor de los nombres de instrumento: es convención, no dato.
        if (mi.comment) meta_.comment = mi.comment;

        // total_time sale del escaneo que hace el cargador; ya es válido
        // aquí, nada más xmp_start_player().
        struct xmp_frame_info fi;
        std::memset(&fi, 0, sizeof(fi));
        xmp_get_frame_info(ctx_, &fi);

        if (fi.total_time > 0) {
            meta_.length_frames =
                (static_cast<uint64_t>(fi.total_time) * kSampleRate) / 1000;
        }

        if (loop_infinite_ || meta_.length_frames == 0) {
            // Duración desconocida o deliberadamente infinita: sin corte y
            // sin fade. TrackMetadata documenta 0 como "el motor no reporta
            // un valor fiable".
            cut_frames_       = 0;
            fade_start_frames_ = 0;
            fade_len_frames_  = 1;   // nunca se usa; evita división por cero
            meta_.fade_frames  = 0;
            meta_.total_frames = 0;
            return;
        }

        meta_.fade_frames =
            (static_cast<uint64_t>(fade_length_msecs_ > 0 ? fade_length_msecs_ : 0)
             * kSampleRate) / 1000;

        fade_start_frames_ = meta_.length_frames;
        fade_len_frames_   = meta_.fade_frames > 0 ? meta_.fade_frames : 1;
        cut_frames_        = meta_.length_frames + meta_.fade_frames;
        meta_.total_frames = cut_frames_;
    }

    static std::string trim(const char* s) {
        if (!s) return {};
        std::string r(s);
        const auto end = r.find_last_not_of(" \t\r\n");
        if (end == std::string::npos) return {};
        r.erase(end + 1);
        const auto begin = r.find_first_not_of(" \t\r\n");
        return r.substr(begin);
    }

    // Regla de asignación del campo 'chip':
    //   .MOD           -> "PAULA", el chip real del Amiga. Un MOD se
    //                     escribió PARA ese hardware y libxmp reproduce su
    //                     comportamiento (incluido el mixer Paula opcional),
    //                     así que aquí sí hay un chip que nombrar.
    //   .S3M/.XM/.IT   -> el tracker que libxmp identifica en el fichero.
    //                     Esos formatos son mezcla por software (GUS, SB,
    //                     mixer propio); no hay chip, y el dato honesto es
    //                     el programa con el que se hizo.
    //
    // En Format::Auto (identificación por contenido, sin extensión) se
    // deduce el MOD por la cadena de tipo: bajo LIBXMP_CORE_PLAYER,
    // mod_load.c solo emite dos valores, "Protracker" y "Fasttracker"
    // (deps/libxmp-lite/src/loaders/mod_load.c). Ningún otro loader de los
    // cuatro produce esas cadenas exactas.
    std::string chip_label(const char* type) const {
        if (format_ == Format::Mod) return "PAULA";
        if (format_ == Format::Auto && is_lite_mod_type(type)) return "PAULA";

        std::string label = short_tracker_label(type);
        if (!label.empty()) return label;
        // Sin cadena de tipo utilizable: la extensión es mejor que "--".
        switch (format_) {
            case Format::S3m: return "S3M";
            case Format::Xm:  return "XM";
            case Format::It:  return "IT";
            default:          return {};
        }
    }

    static bool is_lite_mod_type(const char* type) {
        const std::string t = trim(type);
        return t == "Protracker" || t == "Fasttracker";
    }

    // El campo 'chip' de TrackMetadata no tiene equivalente exacto en un
    // módulo: no hay chip, hay un mixer por software. Se muestra el tracker
    // que libxmp identifica (xmp_module::type), que sí es un dato real del
    // fichero, quitándole el número de versión: "OpenMPT 1.31.07.00" ->
    // "OPENMPT", "Scream Tracker 3" -> "SCREAM TRACKER 3". Un token se
    // considera versión si empieza por dígito y contiene un punto, así que
    // el "3" de Scream Tracker 3 -- parte del nombre -- se conserva.
    static std::string short_tracker_label(const char* type) {
        const std::string t = trim(type);
        std::string out;
        std::size_t i = 0;
        while (i < t.size()) {
            const std::size_t end = t.find(' ', i);
            const std::string tok = t.substr(i, end == std::string::npos ? end : end - i);
            const bool starts_digit =
                !tok.empty() && std::isdigit(static_cast<unsigned char>(tok[0]));
            const bool starts_v_digit =
                tok.size() > 1 && (tok[0] == 'v' || tok[0] == 'V') &&
                std::isdigit(static_cast<unsigned char>(tok[1]));
            const bool is_version = (starts_digit || starts_v_digit) &&
                                     tok.find('.') != std::string::npos;
            if (!is_version) {
                if (!out.empty()) out += ' ';
                out += tok;
            }
            if (end == std::string::npos) break;
            i = end + 1;
        }
        return upper(out);
    }

    static std::string upper(std::string s) {
        for (auto& c : s)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return s;
    }

    xmp_context ctx_ = nullptr;
    bool module_loaded_  = false;
    bool player_started_ = false;

    int    fade_length_msecs_;
    bool   loop_infinite_;
    Format format_;
    int    separation_;

    uint64_t position_frames_   = 0;
    uint64_t fade_start_frames_ = 0;
    uint64_t fade_len_frames_   = 1;
    uint64_t cut_frames_        = 0;   // 0 = sin corte
    bool     finished_          = false;

    TrackMetadata meta_;
};
