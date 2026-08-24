// vgmstream_extensions.hpp
//
// Extensiones que se enrutan a VgmstreamEngine. UNA sola lista, incluida
// desde libretro.cpp y zip_playlist.hpp.
//
// El resto de formatos del core mantienen su lista repetida en cuatro
// sitios (valid_extensions, SET_CONTENT_INFO_OVERRIDE, zip_playlist.hpp y
// los .info), y olvidar uno ya ha costado dos fallos idénticos en
// producción: las entradas del .zip se descartan por "extensión no
// soportada" y el álbum carga vacío, sin ningún error que apunte a la
// causa. Con 32 formatos nuevos ese riesgo se multiplica, así que aquí la
// lista vive una sola vez. Los .info siguen fuera (son datos, no código);
// verificar_antes_de_subir.sh comprueba que coinciden.
//
// NO están todas las extensiones que vgmstream reconoce para estos 32
// formatos, sólo las que aparecen en rips reales. Añadir una es seguro:
// la rama de vgmstream va la última en construct_engine_for(), así que
// una extensión de más sólo llega a vgmstream cuando ningún otro motor la
// ha reclamado, y vgmstream aún tiene que validar la cabecera.
//
// CUIDADO con las genéricas. ".str" SÍ está: lo reclaman a la vez el XA
// de PS1 y el STR de Sega, pero vgmstream los distingue por contenido y es
// la extensión habitual de los rips de PS1, así que compensa. ".snd",
// ".dat", ".wav" y ".pcm" NO están, aunque vgmstream también las reconoce:
// las usa media docena de formatos ajenos al core y el riesgo de secuestrar
// un fichero que no nos toca supera lo que aportan. Cualquier extensión
// nueva de ese tipo hay que pensarla dos veces.
//
// ".bnk" es el caso límite que sí está: es genérica, pero aquí sólo llega
// tras pasar por todos los demás motores, y bnk_sony valida cabecera.
//
// ".asf" choca de nombre con el Advanced Systems Format de Microsoft, que
// el core NO reproduce. Está igualmente porque es la extensión que EA usa
// para su SCHl en los rips de PS2/GameCube (007: Agent Under Fire trae 424
// entradas, todas SCHl) y porque ea_schl_standard.c ya la lista y exige la
// magia "SCHl" en 0x00. Un .asf de Microsoft se rechaza ahí, así que lo
// peor que pasa es que aparezca como soportado y luego no abra.
//
// ".mib"/".mi4" las reclaman DOS parsers: mib_mih, que exige el hermano
// .mih, y ps_headerless, que adivina interleave y canales analizando los
// datos. El orden de la tabla de init importa: mib_mih va antes, así que
// un .mib con su .mih se decodifica con los valores exactos y sólo cae en
// la heurística cuando no hay hermano.

#pragma once

#include <cctype>
#include <cstring>
#include <string>

namespace vgmstream_ext {

// Ordenadas por sistema, igual que la tabla de init en
// deps/vgmstream/vgmstream_init.c, para poder cotejarlas de un vistazo.
inline const char* const* list(std::size_t& count) {
    static const char* const kExts[] = {
        // PS1 / CD-i
        ".xa", ".str", ".pxa", ".xai",
        // Sony VAG y derivados de PS1/PS2
        ".vag", ".svag", ".vas", ".khv", ".vsv", ".psh",
        ".npsf", ".exst", ".sts", ".rxws", ".vpk", ".vgs",
        ".mib", ".mi4", ".mih", ".ild", ".bnk",
        // Konami
        ".mtaf",
        // Sega: Dreamcast, Naomi
        ".adx", ".ahx", ".spsd", ".adpcm",
        // Nintendo: GameCube, Wii, DS
        ".dsp", ".brstm", ".ast", ".hps", ".dtk", ".strm",
        // Genéricos de la época
        ".genh", ".aifc", ".aud",
        ".cfn", ".ydsp", ".rstm", ".rsm", ".sng", ".asf", ".adp",
        ".ss2", ".ads",
        // Traídos de upstream en la ronda del 24/08 (ver LEEME.txt)
        ".fsb", ".gcub", ".mss", ".wam", ".wac",
    };
    count = sizeof(kExts) / sizeof(kExts[0]);
    return kExts;
}

// Para valid_extensions y los .info: mismas extensiones, sin punto,
// separadas por '|'. Se construye una vez.
inline const std::string& pipe_separated() {
    static const std::string joined = [] {
        std::size_t n = 0;
        const char* const* exts = list(n);
        std::string out;
        for (std::size_t i = 0; i < n; ++i) {
            if (i) out += '|';
            out += exts[i] + 1; // salta el '.'
        }
        return out;
    }();
    return joined;
}

// Comparación de nombres insensible a mayúsculas. Los .zip mezclan
// "BGM01.MIB" con "bgm01.mih" según quién hiciera el rip, y una búsqueda
// sensible al caso no encontraría al hermano.
inline bool same_name(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const unsigned char ca = static_cast<unsigned char>(std::tolower(
            static_cast<unsigned char>(a[i])));
        const unsigned char cb = static_cast<unsigned char>(std::tolower(
            static_cast<unsigned char>(b[i])));
        if (ca != cb) return false;
    }
    return true;
}

inline bool matches(const std::string& name) {
    std::size_t n = 0;
    const char* const* exts = list(n);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t len = std::strlen(exts[i]);
        if (name.size() < len) continue;
        // Comparación insensible a mayúsculas: los rips vienen con .XA,
        // .VPK y .adx mezclados en el mismo álbum.
        bool eq = true;
        for (std::size_t k = 0; k < len; ++k) {
            const char a = static_cast<char>(
                std::tolower(static_cast<unsigned char>(name[name.size() - len + k])));
            if (a != exts[i][k]) { eq = false; break; }
        }
        if (eq) return true;
    }
    return false;
}

} // namespace vgmstream_ext
