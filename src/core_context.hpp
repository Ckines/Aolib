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
#ifdef AOLIB_WITH_CHD
#include "chd_playlist.hpp"
#include "chd_reader.hpp"
#include "chd_vfs_bridge.hpp"
#endif

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

    // Panorama por defecto de los módulos tracker (XMP_PLAYER_DEFPAN,
    // 0..100). Solo lo lee XmpEngine, y solo al abrir: libxmp aplica el
    // panorama al construir los canales durante la carga del módulo.
    int    xmp_stereo_separation = 50;

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

    // El trabajo de prefetch tiene un unzFile abierto, y eso NO es RAII:
    // hay que cerrarlo a mano o se filtra el handle del VFS del frontend
    // en cada retro_unload_game(). zip_inflate_abort() es segura sobre un
    // trabajo inactivo, así que no hace falta comprobar nada aquí.
    //
    // Y EL MOTOR SE DESTRUYE EL PRIMERO, a mano. Los miembros se destruyen
    // en orden INVERSO al de declaracion, asi que 'engine' -- que es el
    // primero -- seria el ULTIMO en irse, cuando el .chd y sus puentes de
    // VFS ya no existen. Un motor de vgmstream sobre una pista de datos
    // guarda el puntero al ChdDataTrackVfs y lo usa AL CERRAR, para
    // stream_close(): con el bridge ya liberado eso es un uso despues de
    // liberar, y peta al descargar el contenido. Lo mismo vale para
    // CdAudioEngine, que guarda el puntero al ChdReader.
    //
    // Estuvo latente mientras la unica entrada que usaba un bridge era la
    // pista de datos entera, que nunca llegaba a sonar (iba la ultima de
    // la lista y sin sondear). Con los ficheros de audio XA enumerados uno
    // a uno, la primera pista del disco ya es una de ellas y salta
    // siempre.
    ~CoreContext() {
        engine.reset();
        zip_inflate_abort(prefetch);
        zip_inflate_abort(probe_job);
    }

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

    // Ruta del archivo del que salieron 'zip_entries'. Hace falta para
    // reabrirlo y materializar las entradas perezosas: los formatos de
    // streaming se enumeran leyendo solo su cabecera y el resto se infla
    // al seleccionar la pista (ver zip_playlist.hpp).
    std::string archive_path;

    // Lector del .chd cuando el contenido es uno. Vive lo que dura el
    // contexto y MAS que cualquier motor: las pistas CD-DA no se
    // materializan nunca, CdAudioEngine lee de aqui los sectores que
    // necesita en cada render(). Ver chd_playlist.hpp.
#ifdef AOLIB_WITH_CHD
    chd_reader::ChdReader chd;

    // Lectura anticipada de la entrada siguiente, repartida por frames.
    // Ver chd_playlist::PrefetchJob.
    chd_playlist::PrefetchJob chd_prefetch;

    // Medidor de la duracion de una pista de audio XA, troceado por
    // frames. Ver chd_playlist::XaProbeJob.
    chd_playlist::XaProbeJob chd_xa_probe;

    // Un ChdDataTrackVfs por pista de datos con posible XA (tipicamente
    // una sola, la pista 1). Indexado por indice de pista, como
    // ZipEntry::cd_data_vfs; nullptr en los indices que no aplican. Vive
    // aqui y no dentro del bucle que lo usa porque el streamfile de
    // vgmstream conserva el puntero al vfs durante TODA la vida del motor,
    // incluidos los reaperturas de select_track() al cambiar de subsong.
    std::vector<std::unique_ptr<chd_vfs_bridge::ChdDataTrackVfs>> chd_data_vfs;
#endif

    // true = 'archive_path' es un .chd. El otro contenedor, al lado del
    // .zip; a partir de la enumeracion las entradas son iguales.
#ifdef AOLIB_WITH_CHD
    bool archive_is_chd = false;
#else
    static constexpr bool archive_is_chd = false;
#endif

    // Inflado por adelantado de la entrada SIGUIENTE, repartido entre
    // frames (ver libretro.cpp::advance_zip_prefetch()).
    //
    // Inflar una entrada cuesta lo que cuesta -- ~168 MB/s medidos, y
    // deflate obliga a empezar por el principio -- así que la única forma
    // de que un cambio de pista no se coma el frame es tener la entrada ya
    // inflada ANTES de necesitarla. Con pistas de minutos y entradas de
    // decenas de MB sobra tiempo de sobra para hacerlo a trocitos.
    //
    // 'prefetch_index' es la entrada que se está inflando; kNoPrefetch
    // cuando no hay ninguna. Si el usuario salta a otra pista, el trabajo
    // se descarta: es trabajo especulativo y perderlo solo cuesta lo ya
    // gastado, nunca corrección.
    static constexpr std::size_t kNoPrefetch = static_cast<std::size_t>(-1);
    ZipInflateJob prefetch;
    std::size_t   prefetch_index = kNoPrefetch;

    // Cursor del sondeo INCREMENTAL de duraciones (ver
    // libretro.cpp::advance_duration_probe()). Empieza en 0 y avanza un
    // poco por frame hasta agotar la lista; mientras tanto la UI dibuja
    // "--:--" en lo que aún no se ha sondeado, que es lo que ya hacía para
    // las entradas ilegibles.
    std::size_t duration_probe_index = 0;

    // Los formatos que no dan duración con el prefijo de 64 KiB (XA, EA
    // SCHl) hay que inflarlos enteros SOLO para sondearlos. Eso son ~97 ms
    // por entrada, así que necesita su propio trabajo reanudable: hacerlo
    // de una tacada metía 26 frames por encima del presupuesto en los
    // primeros segundos de reproducción -- medido, y peor que el problema
    // original porque cae con el audio ya sonando.
    ZipInflateJob probe_job;
    std::size_t   probe_job_index = kNoPrefetch;

#ifdef AOLIB_WITH_CHD
    // El mismo sondeo NeedsFull, pero para una entrada que vino de un
    // .chd: ZipInflateJob es minizip y no sabe leer un .chd, asi que
    // necesita su propio trabajo reanudable en vez de reutilizar
    // probe_job. Nunca estan los dos activos a la vez: el sondeo es
    // secuencial, una entrada detras de otra.
    chd_playlist::PrefetchJob chd_probe_job;
#endif

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
