// iaudio_engine.hpp
//
// Contrato común entre los motores de audio (libgme, libvgm, aosdk). El
// wrapper de Libretro (libretro.cpp) solo conoce esta interfaz; nunca toca
// Music_Emu* ni las funciones psf_*/psf2_* directamente.
//
// Invariantes:
//   - render() NO asigna memoria: está en el camino caliente. Cada motor
//     preasigna sus buffers internos en open().
//   - open() es el único punto donde se permite malloc/new.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "../vfs_bridge.hpp"

// Metadatos normalizados. No todos los motores rellenan todos los campos;
// los campos vacíos quedan en cadena vacía / 0.
struct TrackMetadata {
    std::string title;
    std::string artist;   // "artist" en libgme, "Artist" en tags Corlett
    std::string game;
    std::string year;
    std::string copyright_notice;
    std::string comment;

    // Duración/fade tal como los reporta el motor, ya en frames a 44100 Hz.
    // 0 = el motor no reporta un valor fiable; entonces manda su fallback
    // nativo (libgme: 150000 ms si el fichero no trae tag de duración;
    // PSF1/PSF2/SSF: corlett_length_set con length_seconds==0 deja
    // decaybegin=~0, es decir, la pista suena hasta que el frontend la
    // avance o la reinicie -- ver deps/aosdk/corlett.c). No hay duración
    // por defecto configurable a propósito: distinguir "tag ausente" de
    // "tag=0 real" exigiría parchear los tres motores aosdk.
    uint64_t length_frames = 0;
    uint64_t fade_frames   = 0;

    // Duración REAL de la pista, hasta que el motor señala end_of_track():
    // incluye el fade y, en VGM, los bucles que se van a reproducir.
    // 0 = desconocida o infinita.
    //
    // Campo aparte y no un cambio de significado de 'length_frames' porque
    // ese marca dónde EMPIEZA el fade y tiene un consumidor intocable:
    // PsfEngine/Psf2Engine se lo devuelven a corlett_length_set() al
    // reiniciar la pista, donde significa exactamente "decaybegin". Usar
    // ahí el total alargaría cada pista PSF un fade entero por reinicio.
    uint64_t total_frames  = 0;

    // Chip de sonido emulado, para la UI. Cadena corta en mayúsculas, tal
    // como lo nombra el hardware ("YM2612", "QSOUND", "SPU", "SCSP"...).
    // Vacía si el motor no puede determinarlo: la UI dibuja "--" antes que
    // inventarse un nombre.
    std::string chip;

    // Duración que la UI debe mostrar. Prefiere total_frames y cae a
    // length_frames si un motor no lo rellenara: añadir un motor nuevo no
    // puede hacer que la duración desaparezca de la pantalla.
    uint64_t playback_frames() const {
        return total_frames != 0 ? total_frames : length_frames;
    }
};

class IAudioEngine {
public:
    virtual ~IAudioEngine() = default;

    // 'uri' es el path completo (para motores que exigen need_fullpath=true).
    // 'data'/'size' son el contenido en memoria cuando el motor lo acepta
    // (libgme) o cuando el frontend ya lo entregó así.
    // 'vfs' se usa para resolver dependencias externas (ficheros _lib de
    // psflib/aosdk); el motor debe guardar una referencia si la necesita
    // durante toda la vida de la pista, no solo durante open().
    virtual bool open(const char* uri, const uint8_t* data, std::size_t size,
                       IVFSBridge& vfs) = 0;

    // Rinde hasta 'max_frames' frames estéreo entrelazados (L,R,L,R,...) en
    // 'out'. Devuelve los frames realmente escritos. NUNCA asigna memoria.
    // Puede devolver menos de max_frames; el llamante rellena el resto con
    // silencio.
    virtual std::size_t render(int16_t* out, std::size_t max_frames) noexcept = 0;

    // true si el motor señala fin de pista (EOT nativo, p. ej. gme_end_of_track
    // o ao_song_done). El core decide qué hacer (parar, hacer loop, avanzar
    // de subsong) — el motor solo informa.
    virtual bool end_of_track() const noexcept = 0;

    virtual unsigned track_count() const noexcept { return 1; }
    // El default es 'false' A PROPÓSITO: un motor de una sola pista sin
    // reinicio real no debe fingir que select_track(0) tuvo éxito. Es el
    // caso de SsfEngine, donde ssf_command(COMMAND_RESTART) es un no-op
    // literal en deps/aosdk/eng_ssf/eng_ssf.c. Los motores que sí soportan
    // reinicio o subsongs sobrescriben este método.
    virtual bool     select_track(unsigned /*index*/) { return false; }

    virtual const TrackMetadata& metadata() const noexcept = 0;

    // Una vez por iteración de retro_run(), FUERA del bucle de render()
    // muestra a muestra. No-op por defecto; PsfEngine lo sobrescribe para
    // invocar psf_frame(), el "tick" de temporizador/vídeo interno de
    // aosdk, distinto del muestreo de audio.
    virtual void on_video_frame() noexcept {}

    // Reverb del SPU emulado (CoreOptions::spu_reverb_enabled). No-op por
    // defecto: solo Psf2Engine lo sobrescribe, porque es el único motor
    // cuyo SPU vendorizado (peops2/reverb.c) expone un interruptor global
    // real (iUseReverb). En PSF1 el reverb depende solo de flags por canal
    // que pone el propio programa PSF, y el SCSP de Saturn es un DSP
    // distinto sin este concepto.
    virtual void set_reverb_enabled(bool /*enabled*/) noexcept {}

    // Nombre corto para logging ("libgme", "aosdk-psf1", "aosdk-psf2").
    virtual const char* engine_name() const noexcept = 0;
};
