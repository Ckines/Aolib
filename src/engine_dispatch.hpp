// engine_dispatch.hpp — qué motor abre cada fichero.
//
// Es la pieza con más reglas escritas con sangre del core: el orden de los
// candidatos, la lista blanca de vgmstream y las tres excepciones que las
// acotan están documentadas ahí abajo, junto al código que las aplica.
//
// Salió de libretro.cpp al partirlo: ver la cabecera de ese fichero.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core_globals.hpp"
#include "zip_playlist.hpp"
#include "engine/iaudio_engine.hpp"
#include "engine/gme_engine.hpp"
#include "engine/libvgm_engine.hpp"
#include "engine/xmp_engine.hpp"
#ifdef AOLIB_WITH_VGMSTREAM
#include "engine/vgmstream_engine.hpp"
#include "vgmstream_extensions.hpp"
#endif
#ifdef AOLIB_WITH_PSF
#include "engine/psf_engine.hpp"
#include "engine/psf2_engine.hpp"
#include "engine/ssf_engine.hpp"
#endif
#ifdef AOLIB_TEST_HOOKS
// Motor fixture SOLO para tests (ver el target test-select-track-fail del
// Makefile). Este include y su bloque de despacho desaparecen por completo
// cuando AOLIB_TEST_HOOKS no está definido, que es siempre en 'make all'.
#include "../tests/f7b_fixture_engine.hpp"
#endif

extern "C" {
#include "gme.h"
}

namespace aolib {

#ifndef AOLIB_WITH_PSF
// Sin los motores de aosdk enlazados no hay estado de corlett que
// preservar, pero los sondeos de duración se compilan igual: el alias
// inerte evita salpicar de #ifdef el cuerpo de esas funciones.
struct AosdkFadeScope {};
#endif
#ifdef AOLIB_WITH_PSF
// Busca un "_lib"/"_libN" entre las entradas hermanas del mismo .zip, por
// basename e insensible a mayúsculas: esos tags casi siempre son nombres
// sueltos sin ruta, y su case no tiene por qué coincidir con el del fichero
// real. Sirve para los tres motores aosdk porque quien resuelve "_lib" es
// corlett_decode(), que es común a todos.
static AosdkLibResolver::SiblingLookup make_sibling_lookup(const std::vector<ZipEntry>* siblings) {
    return [siblings](const std::string& name, std::vector<uint8_t>& out) -> bool {
        auto to_lower = [](std::string s) {
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        const std::string wanted = to_lower(name);
        for (const auto& entry : *siblings) {
            std::string entry_basename = entry.name;
            const auto slash = entry_basename.find_last_of("/\\");
            if (slash != std::string::npos) entry_basename = entry_basename.substr(slash + 1);
            if (to_lower(entry_basename) == wanted) {
                out = entry.data;
                return true;
            }
        }
        return false;
    };
}

// Construye y abre un motor aosdk (PSF1, PSF2 o SSF): las tres ramas son
// idénticas salvo el tipo concreto. 'variant_label' es solo para el log.
template <typename EngineT>
static std::unique_ptr<IAudioEngine> construct_psf_variant(
    const char* uri, const std::string& display_name,
    const uint8_t* data, std::size_t size,
    const std::vector<ZipEntry>* siblings, const char* variant_label) {
    if (uri && (!g_vfs || !g_vfs->is_valid())) {
        log_message(RETRO_LOG_ERROR,
            "[aolib] %s: fichero %s sin VFS disponible.",
            display_name.c_str(), variant_label);
        return nullptr;
    }
    // El constructor ya lo comprobaría -- aosdk_acquire_guards() fallaría y
    // open() devolvería false -- pero el mensaje sería "psf_start() falló",
    // que apunta al fichero cuando el problema es que hay otro motor aosdk
    // vivo. Se dice aquí, y se dice bien.
    if (AosdkCorlettGuard::live()) {
        log_message(RETRO_LOG_ERROR,
            "[aolib] %s: ya hay un motor aosdk vivo; su estado (RAM emulada y "
            "contadores de fade de corlett) es global de proceso y no admite "
            "dos motores a la vez. Entrada omitida.",
            display_name.c_str());
        return nullptr;
    }
    auto engine = std::make_unique<EngineT>();
    if (siblings) {
        engine->set_sibling_lookup(make_sibling_lookup(siblings));
    }
    if (!engine->open(uri, data, size, active_vfs())) {
        // last_open_error() no forma parte de IAudioEngine: se consulta
        // sobre el tipo concreto, igual que en la rama de libvgm. Distingue
        // "cabecera corlett corrupta" -- que antes mataba el proceso y
        // ahora se rechaza -- de un psf_start() que falló por otra cosa.
        if (!engine->last_open_error().empty()) {
            log_message(RETRO_LOG_ERROR, "[aolib] %s: %s.",
                        display_name.c_str(), engine->last_open_error().c_str());
        } else {
            log_message(RETRO_LOG_ERROR, "[aolib] %s: %s_start() falló.",
                        display_name.c_str(), variant_label);
        }
        return nullptr;
    }
    log_message(RETRO_LOG_INFO, "[aolib] %s: cargado vía %s.",
                display_name.c_str(), engine->engine_name());
    return engine;
}
#endif // AOLIB_WITH_PSF

// ─────────────────────── Reparto entre backends ───────────────────────
//
// Hay siete motores posibles y una entrada de contenido. Hasta 1.2.0 el
// reparto era una cadena de if: el PRIMERO que reclamaba la extensión se
// quedaba con el fichero, y si su open() fallaba la entrada se descartaba
// sin que nadie más la mirara. Eso convierte cualquier extensión mal
// puesta -- que en rips descargados es lo normal, no la excepción -- en
// una pista que no suena, aunque otro motor del mismo binario la hubiera
// abierto sin problema.
//
// Ahora el reparto construye una LISTA de candidatos y prueba en orden. El
// primero sigue siendo exactamente el de antes, así que cuando acierta (el
// caso normal) no cambian ni el comportamiento ni el log; los demás solo
// existen para lo que antes se tiraba a la basura.
//
// Tres reglas acotan la lista, y ninguna es cosmética:
//
//  1. vgmstream sigue yendo EL ÚLTIMO y sigue mirando solo extensiones de
//     su lista blanca (vgmstream_ext::matches). Reconoce cientos de
//     formatos y varios reclaman extensiones genéricas (.str, .snd, .dat)
//     que también usan los motores de arriba: si entrara como candidato
//     universal, se quedaría con ficheros que no son suyos.
//
//  2. Los candidatos de respaldo se filtran por CONTENIDO, no por "prueba
//     a ver": libvgm solo si los bytes empiezan por la magia de VGM o por
//     gzip, libgme solo si gme_identify_header() reconoce la cabecera,
//     libxmp solo si xmp_test_module_from_memory() acepta. Un respaldo que
//     se intenta a ciegas es un respaldo que algún día abre el fichero
//     equivocado.
//
//  3. A la familia aosdk NO se cae por respaldo genérico. Sus motores
//     escriben 2 MB de RAM emulada y los contadores de fade de proceso
//     (ver aosdk_bridge.hpp), y abrirlos es lo más caro que hace el core.
//     La única excepción es dentro de la propia familia: si la extensión
//     dice .psf pero el byte de versión de la cabecera corlett dice PSF2 o
//     SSF, se prueba también la variante que dice el fichero. Eso es leer
//     el contenido, no adivinar.
// 'Xmp' es libxmp con el formato que dice la EXTENSIÓN; 'XmpAuto' es
// libxmp dejándole identificar él. No son el mismo candidato: un .mod que
// en realidad es un S3M falla con el cargador de MOD forzado y abre con el
// automático, y ese es justo el caso que el respaldo existe para cubrir.
enum class Backend { Psf, Psf2, Ssf, Libvgm, Xmp, XmpAuto, Gme, Vgmstream };

static const char* backend_label(Backend b) {
    switch (b) {
        case Backend::Psf:       return "aosdk-psf1";
        case Backend::Psf2:      return "aosdk-psf2";
        case Backend::Ssf:       return "aosdk-ssf";
        case Backend::Libvgm:    return "libvgm";
        case Backend::Xmp:       return "libxmp";
        case Backend::XmpAuto:   return "libxmp (por contenido)";
        case Backend::Gme:       return "libgme";
        case Backend::Vgmstream: return "vgmstream";
    }
    return "?";
}

// Todo lo que un motor necesita para intentar abrir una entrada. Va en un
// struct porque son nueve parámetros y se pasan tal cual a cada candidato:
// como lista suelta, intercambiar dos del mismo tipo compila sin quejarse.
struct DispatchRequest {
    const char*                  uri = nullptr;
    const std::string*           display_name = nullptr;
    const uint8_t*               data = nullptr;
    std::size_t                  size = 0;
    double                       default_fade_seconds = 0.0;
    bool                         loop_infinite = false;
    int                          xmp_stereo_separation = 50;
    const std::vector<ZipEntry>* siblings = nullptr;
    // Tamaño real del fichero cuando 'data' es solo un prefijo de una
    // entrada de .zip todavía sin materializar. 0 o <= size = completo.
    // Solo la rama de vgmstream lo usa: los demás motores nunca reciben
    // entradas parciales, porque zip_entry_is_lazy() las excluye.
    std::size_t                  full_size = 0;
    XmpEngine::Format            xmp_format = XmpEngine::Format::Auto;

    // Casi siempre nullptr: el motor lee de active_vfs(), el VFS que dio el
    // frontend. Distinto solo para un fichero VIRTUAL que no vive en disco
    // ni en un .zip -- la pista de datos de un .chd, presentada a
    // vgmstream como un .xa de sectores crudos (ver chd_vfs_bridge.hpp).
    // Solo lo usa la rama Vgmstream de try_backend(); el resto de motores
    // no leen de disco por su cuenta.
    IVFSBridge*                   vfs = nullptr;
};

// Byte de versión de una cabecera corlett ("PSF" + versión): 0x01 PSF1,
// 0x02 PSF2, 0x11 SSF. 0 = aquí no hay cabecera corlett.
static uint8_t corlett_version_of(const uint8_t* data, std::size_t size) {
    if (!data || size < 4) return 0;
    if (data[0] != 'P' || data[1] != 'S' || data[2] != 'F') return 0;
    return data[3];
}

// ¿Empiezan estos bytes por la magia de un VGM? "Vgm " en claro, o la
// magia de gzip para un .vgz.
static bool looks_like_vgm_data(const uint8_t* data, std::size_t size) {
    if (!data || size < 4) return false;
    if (data[0] == 'V' && data[1] == 'g' && data[2] == 'm' && data[3] == ' ') return true;
    return data[0] == 0x1F && data[1] == 0x8B;   // gzip: .vgz
}

// ¿Acepta libxmp estos bytes como módulo? Es el mismo comprobador que ya
// usaba el reparto para la convención Amiga "mod.nombre" y para los rips
// renombrados. Se consulta ANTES de construir nada.
static bool looks_like_xmp_module(const uint8_t* data, std::size_t size, char* type_out,
                            std::size_t type_out_len) {
    if (!data || size == 0 || size > static_cast<std::size_t>(0x7FFFFFFF)) return false;
    struct xmp_test_info ti;
    std::memset(&ti, 0, sizeof(ti));
    if (xmp_test_module_from_memory(data, static_cast<long>(size), &ti) != 0) return false;
    if (type_out && type_out_len) {
        std::snprintf(type_out, type_out_len, "%s", ti.type);
    }
    return true;
}

// Intenta UN motor concreto.
//
// 'quiet' es para los candidatos que todavía tienen a alguien detrás: su
// fallo no es un fallo de la entrada y no debe salir por el log como si lo
// fuera. El último candidato se intenta con quiet=false y es el que da el
// mensaje detallado, que es el que le sirve al usuario.
static std::unique_ptr<IAudioEngine> try_backend(Backend backend, const DispatchRequest& r,
                                           bool quiet, bool* out_needs_full) {
    const std::string& display_name = *r.display_name;
    const int fade_msecs = static_cast<int>(r.default_fade_seconds * 1000.0);

    switch (backend) {
#ifdef AOLIB_WITH_PSF
        case Backend::Psf2:
            return construct_psf_variant<Psf2Engine>(r.uri, display_name, r.data, r.size,
                                                      r.siblings, "psf2");
        case Backend::Psf:
            return construct_psf_variant<PsfEngine>(r.uri, display_name, r.data, r.size,
                                                     r.siblings, "psf");
        case Backend::Ssf:
            return construct_psf_variant<SsfEngine>(r.uri, display_name, r.data, r.size,
                                                     r.siblings, "ssf");
#else
        case Backend::Psf2:
        case Backend::Psf:
        case Backend::Ssf:
            if (!quiet) {
                log_message(RETRO_LOG_WARN,
                    "[aolib] %s: %s reconocido pero este build no enlaza "
                    "USE_PSF_ENGINE=1.", display_name.c_str(), backend_label(backend));
            }
            return nullptr;
#endif

        case Backend::Libvgm: {
            // No exige fullpath ni VFS: LibvgmEngine acepta memoria
            // directamente vía MemoryLoader_Init.
            if (!r.data || r.size == 0) {
                if (!quiet) {
                    log_message(RETRO_LOG_ERROR, "[aolib] %s: sin contenido en memoria.",
                                display_name.c_str());
                }
                return nullptr;
            }
            auto vgm = std::make_unique<LibvgmEngine>(fade_msecs, r.loop_infinite);
            LibvgmEngine* vgm_raw = vgm.get(); // para last_open_error(), ver más abajo
            if (!vgm->open(r.uri, r.data, r.size, active_vfs())) {
                // last_open_error() no forma parte de IAudioEngine: es una
                // consulta sobre el puntero concreto, antes de devolverlo como
                // unique_ptr<IAudioEngine>. Da el detalle que hace falta cuando
                // el fichero declara un chip sin core de emulación compilado.
                if (!quiet) {
                    if (!vgm_raw->last_open_error().empty()) {
                        log_message(RETRO_LOG_ERROR, "[aolib] %s: %s.",
                                    display_name.c_str(), vgm_raw->last_open_error().c_str());
                    } else {
                        log_message(RETRO_LOG_ERROR, "[aolib] %s: LibvgmEngine::open() falló.",
                                    display_name.c_str());
                    }
                }
                return nullptr;
            }
            log_message(RETRO_LOG_INFO, "[aolib] %s: cargado vía %s.",
                        display_name.c_str(), vgm->engine_name());
            return vgm;
        }

        case Backend::XmpAuto:
        case Backend::Xmp: {
            const XmpEngine::Format fmt = (backend == Backend::XmpAuto)
                                            ? XmpEngine::Format::Auto : r.xmp_format;
            if (!r.data || r.size == 0) {
                if (!quiet) {
                    log_message(RETRO_LOG_ERROR, "[aolib] %s: sin contenido en memoria.",
                                display_name.c_str());
                }
                return nullptr;
            }
            auto xmp = std::make_unique<XmpEngine>(fade_msecs, r.loop_infinite, fmt,
                                                    r.xmp_stereo_separation);
            if (!xmp->open(r.uri, r.data, r.size, active_vfs())) {
                if (!quiet) {
                    log_message(RETRO_LOG_ERROR,
                        "[aolib] %s: xmp_load_module_from_memory() falló (módulo corrupto o "
                        "formato fuera de MOD/S3M/XM/IT).", display_name.c_str());
                }
                return nullptr;
            }
            log_message(RETRO_LOG_INFO, "[aolib] %s: cargado vía %s.",
                        display_name.c_str(), xmp->engine_name());
            return xmp;
        }

        case Backend::Gme: {
            if (!r.data || r.size == 0) {
                if (!quiet) {
                    log_message(RETRO_LOG_ERROR, "[aolib] %s: sin contenido en memoria.",
                                display_name.c_str());
                }
                return nullptr;
            }
            auto gme = std::make_unique<GmeEngine>(fade_msecs);
            if (!gme->open(r.uri, r.data, r.size, active_vfs())) {
                if (!quiet) {
                    log_message(RETRO_LOG_ERROR, "[aolib] %s: gme_open_data falló.",
                                display_name.c_str());
                }
                return nullptr;
            }
            log_message(RETRO_LOG_INFO, "[aolib] %s: cargado vía %s (%u pistas).",
                        display_name.c_str(), gme->engine_name(), gme->track_count());
            return gme;
        }

        case Backend::Vgmstream: {
#ifdef AOLIB_WITH_VGMSTREAM
            // Búsqueda de hermanos contra las entradas ya descomprimidas
            // del archivo: MIB+MIH y compañía no tienen rutas que abrir
            // cuando el contenido vive dentro de un .zip.
            vgmstream_vfs_adapter::SiblingLookup lookup;
            if (r.siblings) {
                const std::vector<ZipEntry>* entries = r.siblings;
                lookup = [entries](const std::string& want)
                        -> vgmstream_vfs_adapter::MemoryView {
                    for (const ZipEntry& e : *entries) {
                        if (vgmstream_ext::same_name(e.name, want) && !e.data.empty())
                            return { e.data.data(), e.data.size() };
                    }
                    return {};
                };
            }

            // display_name, std::move(lookup): las entradas de .zip llegan
            // con uri nulo y vgmstream necesita la extensión para elegir
            // parser.
            auto vgms = std::make_unique<VgmstreamEngine>(r.loop_infinite ? -1.0 : 2.0,
                                                           static_cast<int>(r.default_fade_seconds),
                                                           display_name, std::move(lookup));
            if (r.full_size > r.size) vgms->set_partial(r.full_size);
            // r.vfs solo viene relleno para una entrada que se lee como
            // sectores crudos de la pista de datos de un .chd, que es
            // justo el caso donde enumerar los subsongs cuesta
            // descomprimir el fichero entero antes de la primera muestra.
            if (r.vfs) vgms->set_xa_fast_open(true);
            if (vgms->open(r.uri, r.data, r.size, r.vfs ? *r.vfs : active_vfs())) {
                log_message(RETRO_LOG_INFO,
                    "[aolib] %s: cargado vía %s (%s, %u subsong%s).",
                    display_name.c_str(), vgms->engine_name(),
                    vgms->metadata().chip.c_str(), vgms->track_count(),
                    vgms->track_count() == 1 ? "" : "s");
                return vgms;
            }
            if (out_needs_full && vgms->needs_full_data()) {
                // Lectura truncada: no es un fichero malo, es que hace falta
                // entero. El llamante lo materializa y reintenta, sin log.
                *out_needs_full = true;
                return nullptr;
            }
            // Mensaje accionable: el genérico manda a mirar el fichero,
            // que casi siempre está bien. La causa real suele ser un
            // hermano que falta, y eso hay que decirlo.
            if (!quiet) {
                if (has_suffix(display_name, ".mib")) {
                    log_message(RETRO_LOG_ERROR,
                        "[aolib] %s: ni el .mih hermano ni la heurística de "
                        "PS-ADPCM sin cabecera pudieron describir este fichero.",
                        display_name.c_str());
                } else {
                    log_message(RETRO_LOG_ERROR,
                        "[aolib] %s: extensión de streaming reconocida pero ningún "
                        "parser de vgmstream aceptó el contenido.", display_name.c_str());
                }
            }
            return nullptr;
#else
            (void)quiet; (void)out_needs_full;
            return nullptr;
#endif
        }
    }
    return nullptr;
}

// Construye y abre el motor adecuado para una entrada de contenido.
// 'display_name' solo sirve para detectar la extensión, no necesita existir
// en disco. 'uri' es la ruta REAL en disco si la hay (ficheros sueltos), o
// nullptr para entradas ya extraídas en memoria. 'siblings', si no es
// nullptr, permite resolver "_lib" contra otras entradas del mismo .zip:
// sin eso, un álbum PSF con .psflib compartido -- el caso típico -- falla
// en cascada para todas las pistas que dependan de él.
static std::unique_ptr<IAudioEngine> construct_engine_for(
    const char* uri, const std::string& display_name,
    const uint8_t* data, std::size_t size, double default_fade_seconds,
    bool loop_infinite, int xmp_stereo_separation,
    const std::vector<ZipEntry>* siblings = nullptr,
    // Ver DispatchRequest::full_size.
    std::size_t full_size = 0,
    // Se pone a true si open() falló habiendo leído más allá del prefijo.
    // Sin esto la bandera se perdía con el motor al devolver nullptr, y el
    // barrido no reintentaba: los formatos que necesitan el fichero ENTERO
    // sólo para abrir (caf.c lo necesita, medido con prefijos de 64 KiB a
    // 2 MiB) se quedaban sin duración y soltaban un ERROR por pista.
    bool* out_needs_full = nullptr,
    // Ver DispatchRequest::vfs.
    IVFSBridge* vfs = nullptr) {
    if (out_needs_full) *out_needs_full = false;

    const bool looks_psf = has_suffix(display_name, ".psf") || has_suffix(display_name, ".minipsf");
    const bool looks_psf2 = has_suffix(display_name, ".psf2") || has_suffix(display_name, ".minipsf2");
    // Sega Saturn (SSF/MiniSSF): mismo contenedor corlett que PSF, motor
    // distinto (eng_ssf.c, Musashi M68000 + SCSP).
    const bool looks_ssf = has_suffix(display_name, ".ssf") || has_suffix(display_name, ".minissf");
    // .vgm/.vgz van a LibvgmEngine. Esta comprobación DEBE ir ANTES de la
    // rama de libgme: gme_identify_extension() sigue reclamando ambas
    // extensiones (libgme define gme_vgz_type) y no hay forma de
    // des-registrarlas sin parchear el código vendorizado.
    const bool looks_vgm = has_suffix(display_name, ".vgm") || has_suffix(display_name, ".vgz");
#ifdef AOLIB_WITH_VGMSTREAM
    // No decide nada por sí sola: el candidato que la consulta va el
    // último, después de que PSF/VGM/XMP/GME hayan tenido su turno.
    const bool looks_streamed = vgmstream_ext::matches(display_name);
#else
    const bool looks_streamed = false;
#endif
    // Módulos tracker (libxmp-lite). Los cuatro formatos que compila la
    // variante "lite"; añadir otro (STM, MTM, MED...) exige el libxmp
    // completo, no basta con listar la extensión aquí.
    const XmpEngine::Format xmp_format =
        has_suffix(display_name, ".mod") ? XmpEngine::Format::Mod :
        has_suffix(display_name, ".s3m") ? XmpEngine::Format::S3m :
        has_suffix(display_name, ".xm")  ? XmpEngine::Format::Xm  :
        has_suffix(display_name, ".it")  ? XmpEngine::Format::It  :
                                            XmpEngine::Format::Auto;

#ifdef AOLIB_TEST_HOOKS
    // Extensión reconocida SOLO en builds de test; nunca compite con una
    // extensión real.
    if (has_suffix(display_name, ".f7bfixture")) {
        return std::make_unique<F7bAlwaysFailFixtureEngine>();
    }
#endif

    DispatchRequest req;
    req.uri                   = uri;
    req.display_name          = &display_name;
    req.data                  = data;
    req.size                  = size;
    req.default_fade_seconds  = default_fade_seconds;
    req.loop_infinite         = loop_infinite;
    req.xmp_stereo_separation = xmp_stereo_separation;
    req.siblings              = siblings;
    req.full_size             = full_size;
    req.xmp_format            = xmp_format;
    req.vfs                   = vfs;

    // libgme: identificación por extensión o por magic bytes. Un .vgm/.vgz
    // no puede quedarse con esto de PRIMER candidato -- looks_vgm se mira
    // antes -- pero sí puede aparecer de respaldo, que es donde libgme
    // tiene su propio soporte de VGM.
    gme_type_t gme_type = gme_identify_extension(display_name.c_str());
    const char* gme_header_suffix = (data && size >= 4) ? gme_identify_header(data) : nullptr;
    if (!gme_type && gme_header_suffix && gme_header_suffix[0] != '\0') {
        gme_type = gme_identify_extension(gme_header_suffix);
    }

    // La identificación por contenido de libxmp se calcula como MUCHO una
    // vez: es un barrido de heurísticas sobre el buffer entero y el
    // sondeo de duraciones llama aquí una vez por entrada del álbum.
    char xmp_detected_type[XMP_NAME_SIZE] = {0};
    bool xmp_content_checked = false, xmp_content_ok = false;
    const auto xmp_accepts_content = [&]() {
        if (!xmp_content_checked) {
            xmp_content_checked = true;
            xmp_content_ok = looks_like_xmp_module(data, size, xmp_detected_type,
                                                    sizeof(xmp_detected_type));
        }
        return xmp_content_ok;
    };

    // ── La lista de candidatos, en orden ──
    Backend cands[6];
    int n = 0;
    const auto add = [&cands, &n](Backend b) {
        for (int i = 0; i < n; ++i) if (cands[i] == b) return;
        if (n < 6) cands[n++] = b;
    };

    // 1) Quien reclama la EXTENSIÓN. Salvo dentro de la familia aosdk
    //    (ver justo debajo), este primer candidato es exactamente el que
    //    elegía la cadena de if de antes, y por eso el camino normal no
    //    cambia ni de comportamiento ni de log.
    Backend ext_primary = Backend::Gme;
    bool has_ext_primary = true;
    if (looks_psf2)      ext_primary = Backend::Psf2;
    else if (looks_psf)  ext_primary = Backend::Psf;
    else if (looks_ssf)  ext_primary = Backend::Ssf;
    else if (looks_vgm)  ext_primary = Backend::Libvgm;
    else if (xmp_format != XmpEngine::Format::Auto) ext_primary = Backend::Xmp;
    else if (gme_type)   ext_primary = Backend::Gme;
    else                 has_ext_primary = false;

    // 2) DENTRO de la familia aosdk manda el byte de versión de la
    //    cabecera corlett, no la extensión, y va PRIMERO.
    //
    //    No es un respaldo, es un cambio de orden, y está medido: en las
    //    1.392 entradas PSF1/PSF2/SSF de la biblioteca de referencia la
    //    extensión y el byte de versión coinciden SIEMPRE (.psf/.minipsf/
    //    .psflib -> 0x01, .psf2/.minipsf2/.psf2lib -> 0x02, .ssf -> 0x11),
    //    así que sobre contenido real esto no cambia nada. Solo cambia
    //    algo cuando la extensión miente, y ahí el fichero tiene razón y
    //    el nombre no. Importa porque psf2_start() ACEPTA un PSF1 sin
    //    mirar la versión: sin esto, un .psf renombrado a .psf2 "carga" en
    //    el motor equivocado y suena a lo que sea.
    if (looks_psf || looks_psf2 || looks_ssf) {
        switch (corlett_version_of(data, size)) {
            case 0x01: add(Backend::Psf);  break;
            case 0x02: add(Backend::Psf2); break;
            case 0x11: add(Backend::Ssf);  break;
            default: break;   // sin cabecera corlett legible: manda la extensión
        }
    }
    if (has_ext_primary) add(ext_primary);

    // 3) Respaldos por contenido, en el mismo orden de prioridad global
    //    que tenía la cadena de if: libvgm, libgme, libxmp y, el último de
    //    todos, vgmstream.
    if (looks_like_vgm_data(data, size)) add(Backend::Libvgm);
    if (gme_type)                        add(Backend::Gme);
    if (xmp_accepts_content())           add(Backend::XmpAuto);
    if (looks_streamed) add(Backend::Vgmstream);

    if (n == 0) {
        log_message(RETRO_LOG_WARN, "[aolib] %s: formato no reconocido, omitido.",
                    display_name.c_str());
        return nullptr;
    }

    for (int i = 0; i < n; ++i) {
        const bool last = (i + 1 == n);
        auto engine = try_backend(cands[i], req, /*quiet=*/!last, out_needs_full);
        if (engine) {
            if (has_ext_primary && cands[i] != ext_primary) {
                // Que abra otro NO es rutina: casi siempre significa que la
                // extensión del fichero miente. Se dice, con los dos
                // nombres, para que se pueda arreglar el rip.
                log_message(RETRO_LOG_WARN,
                    "[aolib] %s: por la extensión le tocaba a %s y lo ha abierto %s; "
                    "la extensión no corresponde al contenido.",
                    display_name.c_str(), backend_label(ext_primary),
                    backend_label(cands[i]));
            }
            if (cands[i] == Backend::XmpAuto && xmp_detected_type[0]) {
                log_message(RETRO_LOG_INFO,
                    "[aolib] %s: identificado por contenido como \"%s\".",
                    display_name.c_str(), xmp_detected_type);
            }
            return engine;
        }
        // Lectura truncada: no es un fallo, es que hace falta el fichero
        // entero. El llamante lo materializa y vuelve a llamar; seguir
        // probando candidatos sobre un prefijo solo puede dar un acierto
        // falso.
        if (out_needs_full && *out_needs_full) return nullptr;
    }
    return nullptr;
}

}  // namespace aolib
