// aosdk_bridge.cpp
//
// Define el símbolo que eng_psf.c / eng_psf2.c esperan enlazar:
//
//   int ao_get_lib(const char *filename, uint8 **buffer, uint64 *length);
//
// Se compila como C++ pero con extern "C", para que el nombre no se decore
// al enlazar contra los objetos en C de aosdk.

#include "aosdk_bridge.hpp"

// ao.h define AO_SUCCESS/AO_FAIL y los typedefs uint8/uint64 propios de la
// SDK (no los de <cstdint>).
extern "C" {
#include "ao.h"
}

IVFSBridge* AosdkLibResolver::active_vfs_ = nullptr;
std::string AosdkLibResolver::active_base_dir_;
AosdkLibResolver::SiblingLookup AosdkLibResolver::active_sibling_lookup_ = nullptr;
AosdkLibResolver::LogFn AosdkLibResolver::active_log_ = nullptr;

int AosdkLibResolver::get_lib_impl(const char* filename, uint8_t** buffer,
                                    uint64_t* length) noexcept {
    if (!filename || !buffer || !length) return AO_FAIL;
    *buffer = nullptr;
    *length = 0;

    std::vector<uint8_t> data;
    bool found = false;

    // Se prueba PRIMERO el lookup entre "hermanos" (p.ej. otras entradas
    // ya extraídas del mismo .zip): para una entrada de zip no hay
    // directorio base real, así que el VFS de disco con base_dir vacío
    // casi nunca encontrará nada -- y si por casualidad encontrara un
    // fichero de nombre coincidente en el directorio de trabajo actual
    // del proceso, sería la librería EQUIVOCADA, no un fallback válido.
    if (active_sibling_lookup_) {
        found = active_sibling_lookup_(filename, data);
    }

    if (!found && active_vfs_ && active_vfs_->is_valid()) {
        const std::string full_path =
            active_vfs_->resolve_relative(active_base_dir_, filename);
        // LIMITACIÓN conocida en sistemas sensibles a mayúsculas: el tag
        // _lib puede no coincidir en case con el fichero real. Listar el
        // directorio exigiría la VFS API v3 (opendir/readdir), que no
        // garantizan todos los frontends.
        found = active_vfs_->read_whole_file(full_path, data);
    }

    if (!found) {
        // Causa más frecuente: un .psf/.psf2 suelto que declara un tag
        // "_lib"/"_libN" cuyo fichero compartido no viaja junto a él en
        // disco (típico de rips de PS2), donde varias pistas comparten
        // un único _lib). Sin este aviso, psf_start()/psf2_start()
        // simplemente devuelven AO_FAIL y lo único que ve el usuario es
        // "psf_start()/psf2_start() falló", sin decir qué faltaba.
        if (active_log_) {
            active_log_("[aolib] no se pudo resolver \"_lib\": " +
                        std::string(filename) +
                        " -- ¿falta ese fichero en la misma carpeta que la pista?");
        }
        return AO_FAIL;
    }

    // aosdk libera este buffer con free() por su cuenta (ver corlett.c):
    // malloc y no new[], para no mezclar allocators.
    uint8_t* raw = static_cast<uint8_t*>(std::malloc(data.size()));
    if (!raw) return AO_FAIL;
    std::memcpy(raw, data.data(), data.size());

    *buffer = raw;
    *length = static_cast<uint64_t>(data.size());
    return AO_SUCCESS;
}

// Firma EXACTA declarada en ao.h. 'uint8'/'uint64' son los typedefs de la
// SDK, no los de <cstdint>; los static_assert cazan en compilación un
// cambio de ABI (por ejemplo, compilar sin -DLONG_IS_64BIT donde toca).
static_assert(sizeof(uint8) == sizeof(uint8_t), "ABI de aosdk::uint8 inesperada");
static_assert(sizeof(uint64) == sizeof(uint64_t), "ABI de aosdk::uint64 inesperada");

extern "C" int ao_get_lib(const char* filename, uint8** buffer, uint64* length) {
    // NO hacer reinterpret_cast<uint64_t*>(length) y escribir a través de
    // ese puntero: uint64_t y el uint64 de ao.h tienen el mismo tamaño pero
    // son tipos distintos a efectos de aliasing estricto, así que eso es
    // UB. Se manifestó como un fallo de CRC dentro de corlett_decode() sin
    // causa aparente. La copia por variable local de abajo siempre es
    // segura.
    //
    // uint8** / uint8_t** sí puede convertirse: unsigned char y sus alias
    // son la excepción del estándar a las reglas de aliasing estricto.
    uint8_t* buf = nullptr;
    uint64_t len = 0;
    const int ret = AosdkLibResolver::get_lib_impl(filename, &buf, &len);
    *buffer = reinterpret_cast<uint8*>(buf);
    *length = static_cast<uint64>(len);
    return ret;
}
