// core_context.hpp
//
// Objeto dueño de TODO el estado del contenido cargado. retro_load_game
// construye uno desde cero y retro_unload_game lo destruye entero, lo que
// elimina por construcción la clase de bug "estado residual de la pista A
// contamina la pista B" sin resetear variables sueltas a mano.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/iaudio_engine.hpp"
#include "vfs_bridge.hpp"
#include "zip_playlist.hpp"

// Frames por llamada a retro_run a 44100 Hz / 60 fps.
constexpr std::size_t kFramesPerRun = 735;

struct CoreOptions {
    bool   loop_infinite   = false;
    double default_fade_seconds   = 8.0;
    bool   spu_reverb_enabled = true;

    // Reverb de la CAPA HOST (src/dsp/reverb.hpp). No confundir con
    // 'spu_reverb_enabled' de arriba, que es el SPU emulado de PlayStation.
    //
    // UNA sola magnitud, 0..65, donde 0 == apagado: es la ÚNICA fuente de
    // verdad del estado. Solo el botón REB del deck escribe aquí (ver
    // dsp/reverb.hpp para el porqué). refresh_ui_dynamic_state() recompone
    // UiModel::reverb_enabled desde este campo, y el menú del frontend
    // ('aolib_reverb_amount') solo fija el NIVEL, nunca el encendido.
    int    host_reverb_amount  = 0;

    // Cantidad a la que vuelve el botón REB al reencender, para no tener
    // que pasar por el menú cada vez. Se actualiza sola con cada cantidad
    // > 0. Mismo patrón que 'repeat_when_not_all' en UiModel.
    int    host_reverb_last_on = 35;
};

class CoreContext {
public:
    // Aquí hubo un ring buffer entre el motor y la salida, y se quitó: sin
    // hilos que lo rellenaran de forma asíncrona, retro_run() escribía y
    // leía los mismos 735 frames en cada iteración y la ocupación medida
    // era siempre 0. Era una copia de ida y vuelta sobre el mismo array,
    // con el sobrecoste del wrap-around. Si algún día se introduce
    // producción asíncrona de audio, vuelve a hacer falta.
    CoreContext() {
        // Único punto de asignación del buffer de salida de retro_run.
        // Preasignado aquí; jamás reasignado durante la vida del contexto.
        scratch_output_.resize(kFramesPerRun * 2);
    }

    ~CoreContext() = default; // RAII: engine_ y buffers se liberan solos.

    CoreContext(const CoreContext&) = delete;
    CoreContext& operator=(const CoreContext&) = delete;

    std::unique_ptr<IAudioEngine> engine;
    std::vector<int16_t>          scratch_output_;
    CoreOptions                   options;

    // Frames rendidos desde el inicio de la pista. Vive aquí, no en el
    // motor, para poder aplicar una política de fade/duración uniforme si
    // algún motor no trajera la suya.
    uint64_t frames_rendered = 0;

    // Índice de la pista/subsong actual. Lo posee el contexto, no el
    // motor, porque decidir "qué pista viene después" es responsabilidad
    // del coordinador (retro_run), no de cada motor concreto -- así
    // GmeEngine y PsfEngine no necesitan saber nada el uno del otro.
    unsigned current_track_index = 0;

    // Lista de ficheros dentro de un .zip cuando el contenido cargado es
    // un archivo comprimido con varios ficheros soportados (zip_playlist.hpp).
    // Vacía si se cargó un fichero suelto. 'zip_entry_index' es la
    // entrada actualmente activa; al agotarse sus subsongs (o si no
    // tiene, directamente), se avanza aquí antes que en current_track_index.
    std::vector<ZipEntry> zip_entries;
    std::size_t zip_entry_index = 0;

    // Se pone a true la única vez que se decide "no queda nada más que
    // reproducir" (última pista, última entrada del zip y sin
    // loop_infinite). A partir de ahí retro_run() no vuelve a llamar a
    // render()/on_video_frame() sobre ese motor.
    //
    // No es una optimización cosmética: seguir llamando a render() sobre un
    // motor agotado es barato en casi todos los backends, pero en PSF2 no.
    // Si la CPU emulada queda atascada en la trampa null-state de
    // psx_bios_hle() (PC=0x80000000), cada render() reintenta hasta 104
    // iteraciones de mips_execute() sobre la misma trampa, indefinidamente
    // y sin que nada lo pare. Con las trazas de depuración activas eso
    // llegó a generar un log de 2 GB y a saturar CPU y disco.
    bool exhausted = false;
};
