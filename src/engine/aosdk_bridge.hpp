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
    // El assert se conserva para las builds de test, pero no puede ser la
    // única defensa: el Makefile compila con -O2 y SIN -DNDEBUG, así que en
    // release el assert está activo y una violación aborta el proceso -- es
    // decir, se lleva por delante RetroArch entero en vez de rechazar una
    // pista. Para un core que aspira a ser de referencia eso es al revés de
    // como debe ser: el assert avisa al desarrollador, el retorno protege al
    // usuario.
    [[nodiscard]] static bool acquire() noexcept {
        assert(!s_alive &&
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
        assert(!s_alive &&
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
