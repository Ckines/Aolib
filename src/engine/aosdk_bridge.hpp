// aosdk_bridge.hpp
//
// aosdk no usa un struct de callbacks como psflib, sino una única función
// GLOBAL sin puntero de contexto:
//
//   int ao_get_lib(const char *filename, uint8 **buffer, uint64 *length);
//
// Cuando eng_psf.c / eng_psf2.c encuentran un tag "_lib"/"_libN" durante
// corlett_decode(), la llaman tal cual y esperan el fichero COMPLETO en un
// buffer nuevo reservado con malloc, no un stream.
//
// Consecuencias:
//  1. Al no haber contexto, la instancia activa vive en una variable de
//     módulo. Es admisible porque un core Libretro carga un contenido a la
//     vez por proceso, pero es una restricción real.
//  2. El malloc() ocurre durante la carga, nunca en retro_run(), así que no
//     rompe la regla de cero asignaciones en el camino de audio.
//  3. El buffer entregado lo libera el motor de aosdk, por convención de la
//     propia SDK.
//
// aosdk aplica el fade internamente (corlett_length_set /
// corlett_sample_fade); el core no lo reimplementa.

#pragma once

#include <cassert>
#include <cstdint>

#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "../vfs_bridge.hpp"

extern "C" {
#include "ao.h"
#include "corlett.h"
}

// Aviso de violación de exclusión, para el DESARROLLADOR.
//
// Esto era `assert` a secas, y estaba mal por una razón que se midió: el
// Makefile compila con -O2 y **sin** -DNDEBUG, así que el assert está
// ACTIVO en el binario que se distribuye. Con el assert vivo, el
// `if (s_alive) return false;` que viene detrás es CÓDIGO MUERTO: el
// proceso aborta antes de llegar a él. O sea que el retorno defensivo que
// se añadió para "rechazar una pista en vez de llevarse RetroArch por
// delante" no protegía a nadie en el build real.
//
// Reproducido con las mismas flags del core (-std=c++17 -O2, sin NDEBUG):
// dos acquire() seguidos -> "Assertion failed", el proceso muere con
// código 3 y la línea siguiente no llega a ejecutarse.
//
// Ahora el aviso solo existe cuando AOLIB_TEST_HOOKS está definido, que es
// justo lo que 'make all' NO define. En el binario del usuario queda el
// retorno, que rechaza la carga; en las builds de test queda además el
// abort, que es donde sí interesa enterarse a gritos.
#ifdef AOLIB_TEST_HOOKS
#define AOLIB_AOSDK_EXCLUSION_ASSERT(cond, msg) assert((cond) && msg)
#else
#define AOLIB_AOSDK_EXCLUSION_ASSERT(cond, msg) ((void)0)
#endif

class AosdkLibResolver {
public:
    // Función opcional para resolver "_lib" entre "hermanos" ya cargados
    // en memoria (p.ej. otras entradas del mismo .zip), en vez de -- o
    // antes que -- ir al VFS de disco. Devuelve true y rellena 'out' si
    // encuentra 'name'; false si no.
    using SiblingLookup = std::function<bool(const std::string& name, std::vector<uint8_t>& out)>;

    // Aviso opcional cuando NINGUNA vía (ni sibling_lookup ni VFS de disco)
    // logra resolver un "_lib" pedido por corlett_decode(). Sin esto,
    // psf_start()/psf2_start() simplemente devuelven AO_FAIL y el único
    // mensaje que llega al usuario es "psf_start()/psf2_start() falló",
    // sin decir qué fichero faltaba -- el caso más común es un .psf/.psf2
    // suelto cuyo _lib compartido no se copió junto a él.
    using LogFn = std::function<void(const std::string&)>;

    // Instala este resolver como el activo para la llamada global ao_get_lib.
    // Debe llamarse antes de invocar psf_start/psf2_start y desinstalarse
    // (o sustituirse) en unload. No es reentrante: un core Libretro carga
    // un contenido a la vez, lo que es coherente con esa restricción.
    //
    // 'sibling_lookup' es opcional (por defecto ninguno): cuando el
    // fichero principal viene de un .zip (sin ruta real en disco de la
    // que derivar un directorio base), es la ÚNICA vía de resolver "_lib"
    // -- se prueba ANTES que el VFS de disco, porque para una entrada de
    // zip el VFS con base_dir vacío casi nunca tiene sentido. Para
    // ficheros sueltos en disco (base_dir real), se deja en nullptr y el
    // comportamiento es idéntico al de antes.
    static void install(IVFSBridge* vfs, const std::string& base_dir,
                         SiblingLookup sibling_lookup = nullptr) noexcept {
        active_vfs_ = vfs;
        active_base_dir_ = base_dir;
        active_sibling_lookup_ = std::move(sibling_lookup);
    }

    static void uninstall() noexcept {
        active_vfs_ = nullptr;
        active_base_dir_.clear();
        active_sibling_lookup_ = nullptr;
    }

    // Registra el emisor de avisos, UNA vez, típicamente desde
    // retro_set_environment(). A diferencia de 'sibling_lookup', no se
    // resetea en cada install()/uninstall(): log_message() no depende del
    // contenido cargado, y limpiarlo en cada ciclo de carga solo dejaría
    // la ruta de fallo muda otra vez sin motivo.
    static void set_log(LogFn log) noexcept {
        active_log_ = std::move(log);
    }

    // Implementación real de ao_get_lib(). Se define en aosdk_bridge.cpp
    // con enlazado C, coincidiendo con la firma exacta declarada en ao.h.
    static int get_lib_impl(const char* filename, uint8_t** buffer, uint64_t* length) noexcept;

private:
    static IVFSBridge* active_vfs_;
    static std::string active_base_dir_;
    static SiblingLookup active_sibling_lookup_;
    static LogFn active_log_;
};

// ─── La cabecera corlett se valida AQUÍ, antes de dársela a aosdk ─────
//
// corlett_decode_lib() (deps/aosdk/corlett.c) se cree dos campos de 32
// bits que vienen del fichero y no valida ninguno de los dos:
//
//     offset 0x04   reserved_size  -> res_area
//     offset 0x08   program_size   -> comp_length
//
//   - La única comprobación de longitud que hace es
//     `input_len < comp_length + 16`, que IGNORA res_area, y acto seguido
//     calcula el CRC leyendo desde `input + 16 + res_area`. Un res_area
//     mentiroso lee fuera del buffer.
//   - Con comp_length == 0 se salta CRC y descompresión, deja decomp_dat a
//     NULL y llama IGUAL al callback del motor. Eso NO se puede filtrar
//     aquí: un .minipsf2 sin sección de programa es contenido perfectamente
//     válido -- Dark Cloud son 60 entradas así, medido -- y rechazarlo se
//     carga el álbum entero. El NULL se para en el callback, con un guard
//     en eng_psf.c/eng_ssf.c (ver deps/patches/README.md).
//   - `input_len -= (comp_length + 16 + res_area)` puede dar la vuelta y
//     dejar a corlett_decode_tags() barriendo memoria ajena.
//
// MEDIDO, no razonado: con un buffer de 16 y de 64 bytes cuyo único
// contenido válido es la magia "PSF", las TRES familias mueren con
// violación de acceso -- PsfEngine, Psf2Engine y SsfEngine, SIGSEGV en
// Linux y 0xC0000005 en Windows. Y es alcanzable desde contenido real: una
// entrada truncada o corrupta dentro de un álbum se lleva RetroArch
// entero, sin log y sin la posibilidad de "prueba con la siguiente" que
// load_zip_entry_from() tiene para cualquier otro fallo.
//
// Se valida aquí y no parcheando corlett.c porque esto es una
// precondición del LLAMANTE, no un cambio de comportamiento de la SDK: el
// core no debe entregarle a aosdk nada que aosdk no sepa rechazar.
//
// NO se comprueba el byte de versión (0x01 PSF1, 0x02 PSF2, 0x11 SSF): de
// eso ya decide el despacho por extensión, y exigirlo aquí rechazaría
// rips válidos con la versión mal puesta que hoy suenan.
inline bool aosdk_corlett_header_ok(const uint8_t* data, std::size_t size,
                                     std::string* why = nullptr) {
    const auto no = [why](const char* motivo) {
        if (why) *why = motivo;
        return false;
    };
    if (!data) return no("sin datos");
    // 16 bytes: "PSF" + versión(1) + reserved_size(4) + program_size(4) +
    // crc32(4). Con menos, corlett_decode_lib() ya desreferencia input[0].
    if (size < 16) return no("cabecera corlett incompleta (menos de 16 bytes)");
    if (data[0] != 'P' || data[1] != 'S' || data[2] != 'F')
        return no("no empieza por la magia \"PSF\"");

    const auto le32 = [data](std::size_t at) -> uint64_t {
        return static_cast<uint64_t>(data[at]) |
               (static_cast<uint64_t>(data[at + 1]) << 8) |
               (static_cast<uint64_t>(data[at + 2]) << 16) |
               (static_cast<uint64_t>(data[at + 3]) << 24);
    };
    const uint64_t res_area    = le32(4);
    const uint64_t comp_length = le32(8);

    // Aritmética de 64 bits: los dos campos son de 32 y sumarlos en 32
    // bits podría dar la vuelta y "caber" en cualquier fichero.
    if (16 + res_area + comp_length > static_cast<uint64_t>(size))
        return no("los tamaños declarados en la cabecera no caben en el fichero");

    if (why) why->clear();
    return true;
}

// ───────────────────── Estado de PROCESO de aosdk ─────────────────────
//
// aosdk no tiene contexto: TODO su estado son variables de módulo, y hay
// dos capas distintas de él.
//
//  (a) Estado por FAMILIA de emulación: psx_ram/mipscpu para PSF1+PSF2,
//      sat_ram/Musashi para SSF. Lo protegen AosdkPsxCoreGuard y
//      AosdkSaturnCoreGuard, uno por familia, más abajo.
//
//  (b) Estado del CONTENEDOR corlett, que las TRES familias comparten:
//      los contadores de fade de corlett.c (total_samples, decaybegin,
//      decayend) y ao_song_done. corlett_decode() los reescribe al abrir
//      cualquier fichero PSF1/PSF2/SSF, y corlett_sample_fade() los lee
//      una vez POR MUESTRA para decidir el fade y el fin de pista.
//
// Los guards por familia no cubren (b), y ése era el agujero: PsfEngine
// pide el guard de PSX y SsfEngine el de Saturn, así que construir un PSF
// mientras suena un SSF pasaba las dos comprobaciones y le pisaba a la
// pista viva sus contadores de fade -- que no es un fallo ruidoso, es una
// pista que se corta antes de tiempo o que no termina nunca. Hoy no era
// alcanzable porque el sondeo de duraciones se salta las entradas de aosdk
// por extensión (entry_routes_to_aosdk, libretro.cpp), pero eso es una
// lista escrita a mano en otro fichero, no un invariante.
//
// AosdkCorlettGuard cierra (b): un único latch de proceso, pedido por
// CUALQUIER motor aosdk sea de la familia que sea.

class AosdkCorlettGuard {
public:
    // Mismo contrato que los guards por familia: false = ya hay un motor
    // aosdk vivo y hay que fallar la carga, nunca abortar el proceso.
    [[nodiscard]] static bool acquire() noexcept {
        AOLIB_AOSDK_EXCLUSION_ASSERT(!s_alive,
            "Solo puede existir UN motor aosdk vivo a la vez, de cualquier "
            "familia -- los contadores de fade de corlett.c son estado "
            "global de proceso compartido por PSF1, PSF2 y SSF.");
        if (s_alive) return false;
        s_alive = true;
        return true;
    }
    static void release() noexcept { s_alive = false; }

    // ¿Hay algún motor aosdk vivo AHORA MISMO? Lo consulta el despacho
    // antes de construir uno temporal: la respuesta no depende de la
    // extensión del fichero, que es justo lo que la hace fiable.
    static bool live() noexcept { return s_alive; }

private:
    static inline bool s_alive = false;
};

// Guarda y restaura los contadores de fade alrededor de un bloque.
//
// Es el complemento del latch: el latch impide que un motor temporal
// COEXISTA con otro, y esto impide que lo que ese motor temporal escribió
// SOBREVIVA a su destrucción. Hoy el motor real reescribe los contadores
// al abrir (corlett_decode() llama siempre a corlett_length_set() cuando
// el fichero principal se decodifica bien), así que es una red de
// seguridad y no un arreglo de un fallo alcanzable: lo que hace es que el
// orden "sondear ANTES de que exista ningún motor" deje de ser una regla
// escrita en un comentario y pase a dar igual.
class AosdkFadeScope {
public:
    AosdkFadeScope() noexcept
        : total_samples_(total_samples),
          decaybegin_(decaybegin),
          decayend_(decayend),
          song_done_(ao_song_done) {}

    ~AosdkFadeScope() noexcept {
        total_samples = total_samples_;
        decaybegin    = decaybegin_;
        decayend      = decayend_;
        ao_song_done  = song_done_;
    }

    AosdkFadeScope(const AosdkFadeScope&) = delete;
    AosdkFadeScope& operator=(const AosdkFadeScope&) = delete;

private:
    uint32 total_samples_;
    uint32 decaybegin_;
    uint32 decayend_;
    ao_bool song_done_;
};

// Pide el guard de la familia Y el latch de corlett, en ese orden: así el
// assert que salta es el de la familia cuando el choque es dentro de la
// familia (que es el mensaje útil) y el de corlett solo cuando el choque
// cruza familias. Si el segundo falla, el primero se suelta: un acquire()
// fallido NO puede dejar nada pedido.
template <typename FamilyGuard>
[[nodiscard]] inline bool aosdk_acquire_guards() noexcept {
    if (!FamilyGuard::acquire()) return false;
    if (!AosdkCorlettGuard::acquire()) {
        FamilyGuard::release();
        return false;
    }
    return true;
}

template <typename FamilyGuard>
inline void aosdk_release_guards() noexcept {
    AosdkCorlettGuard::release();
    FamilyGuard::release();
}

// Exclusión mutua COMPARTIDA entre PsfEngine (eng_psf.c) y Psf2Engine
// (eng_psf2.c): ambos usan el mismo núcleo R3000A y la misma RAM de
// trabajo, que son estado de PROCESO, no de instancia C++:
//
//   - deps/aosdk/eng_psf/psx.c     `static mips_cpu_context mipscpu;`
//   - deps/aosdk/eng_psf/psx_hw.c  `uint32 psx_ram[(2*1024*1024)/4];`
//
// psx.c se compila y enlaza una sola vez, y tanto psf_start() como
// psf2_start() llaman a las mismas mips_init()/mips_reset()/mips_execute()
// sobre ese 'mipscpu'. Dos motores vivos a la vez, del mismo tipo o no, se
// pisarían el estado sin que compilador ni enlazador lo detecten.
//
// CoreContext::engine ya garantiza la exclusión por diseño (un único
// unique_ptr, destruido siempre antes de construir el siguiente). Este
// assert es el backstop por si un camino futuro la rompe.
class AosdkPsxCoreGuard {
public:
    // Devuelve false si ya hay uno vivo, y el llamante FALLA LA CARGA.
    //
    // El aviso al desarrollador va por AOLIB_AOSDK_EXCLUSION_ASSERT, no por
    // assert() directo, y el porqué está medido arriba: con assert() a
    // secas el retorno defensivo de la línea siguiente era código muerto en
    // el binario que se distribuye.
    [[nodiscard]] static bool acquire() noexcept {
        AOLIB_AOSDK_EXCLUSION_ASSERT(!s_alive,
            "Solo puede existir UN motor aosdk basado en psx.c (PsfEngine O "
            "Psf2Engine, nunca ambos) activo a la vez -- psx_ram/mipscpu son "
            "estado global de proceso compartido entre PSF1 y PSF2.");
        if (s_alive) return false;
        s_alive = true;
        return true;
    }
    static void release() noexcept { s_alive = false; }

private:
    static inline bool s_alive = false;
};

// Equivalente para SsfEngine (eng_ssf.c). Saturn no comparte nada con
// PSF1/PSF2 -- M68000/Musashi + SCSP frente a R3000A + SPU -- pero dentro
// de la familia SSF el estado también es global de proceso:
//
//   - deps/aosdk/eng_ssf/sat_hw.c   `uint8 sat_ram[512*1024];`
//   - deps/aosdk/eng_ssf/eng_ssf.c  `static corlett_t c = {0};`
//   - Musashi (m68kcpu.c) guarda su contexto en variables de módulo: no
//     hay parámetro de instancia en m68k_init()/m68k_execute().
//
// Guard separado y no reutilizar el de PSX: compartirlo entre familias sin
// relación ocultaría el assert si algún día se permitieran PSF y SSF
// simultáneos por error.
class AosdkSaturnCoreGuard {
public:
    // Mismo contrato que AosdkPsxCoreGuard::acquire(): false = ya hay uno
    // vivo y hay que fallar la carga, no abortar el proceso.
    [[nodiscard]] static bool acquire() noexcept {
        AOLIB_AOSDK_EXCLUSION_ASSERT(!s_alive,
            "Solo puede existir UN motor SsfEngine activo a la vez -- "
            "sat_ram/el núcleo M68000 (Musashi) son estado global de "
            "proceso, no de instancia C++.");
        if (s_alive) return false;
        s_alive = true;
        return true;
    }
    static void release() noexcept { s_alive = false; }

private:
    static inline bool s_alive = false;
};
