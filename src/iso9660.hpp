// iso9660.hpp
//
// Lector de ISO9660 (+ Joliet) sobre un flujo de sectores de 2048 bytes.
// Solo enumera: devuelve nombre, extent y tamano de cada fichero. Leer el
// contenido es un seek, porque en ISO9660 un fichero es un extent
// CONTIGUO -- no hay fragmentacion, ni tabla de asignacion, ni nada que
// recorrer.
//
// Sirve para las dos cosas que quiere el core:
//
//  - ABRIR UN ALBUM guardado como imagen comprimida con chdman (ver
//    herramientas_locales/chdman/hacer_album_chd.py).
//  - SACAR LA MUSICA DE UN CD DE JUEGO: los .VAG/.STR/.XA sueltos que hay
//    en el sistema de ficheros de un disco de PS1 y companyia.
//
// No depende de CHD: recibe un lector de sectores por funcion, asi que el
// mismo codigo vale para un .chd, un .iso suelto o un test con un buffer
// en memoria.
//
// Se leen las DOS jerarquias y gana Joliet cuando esta: ISO9660 a secas
// pone los nombres en mayusculas, los recorta y les pega un ';1', y en
// AOLIB el nombre es lo que ve el usuario en la tracklist. Si no hay
// Joliet se usa la jerarquia primaria y se limpia el ';1'.

#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace iso9660 {

constexpr uint32_t kSectorSize   = 2048;
constexpr uint32_t kFirstVdLba   = 16;    // los 16 primeros son "system area"
constexpr uint32_t kMaxVdLba     = 512;   // tope defensivo al buscar el terminador
constexpr uint32_t kMaxDepth     = 8;     // el estandar dice 8 niveles
constexpr uint32_t kMaxEntries   = 20000; // tope defensivo: un CD no tiene mas

// Un fichero del sistema de ficheros. 'lba' y 'size' son todo lo que hace
// falta para leerlo: los datos son contiguos desde ahi.
struct IsoFile {
    std::string name;      // nombre a secas, sin ruta
    std::string path;      // ruta completa dentro de la imagen, con '/'
    uint32_t    lba  = 0;
    uint32_t    size = 0;

    // Extension CD-XA del registro de directorio (los 14 bytes con la firma
    // "XA" en el area de uso de sistema). La ponen los discos de PS1 y dice
    // como estan grabados los sectores de este fichero SIN tener que ir a
    // mirarlos: es la diferencia entre clasificar un disco leyendo el
    // directorio y clasificarlo escaneando cientos de MB.
    bool     has_xa  = false;
    uint16_t xa_attr = 0;

    // Form 2 (0x1000) es la forma de sector que usa el audio XA; Form 1
    // (0x0800) es la de los datos normales. Un fichero puede traer las dos
    // si es un .STR con video y audio entrelazados.
    bool xa_form2()      const { return has_xa && (xa_attr & 0x1000) != 0; }
    bool xa_interleaved() const { return has_xa && (xa_attr & 0x2000) != 0; }
};

// Lee 'len' bytes desde el desplazamiento 'offset' del espacio plano de
// 2048 bytes por sector. Devuelve false si no se pudo.
using SectorReader = std::function<bool(uint64_t offset, void* dst,
                                        std::size_t len)>;

// ---------------------------------------------------------------------------
// Lectura de campos
// ---------------------------------------------------------------------------

inline uint16_t le16(const uint8_t* p) {
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}

inline uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// UCS-2BE -> UTF-8. Los nombres de Joliet vienen en UCS-2 big-endian y el
// resto del core trabaja en UTF-8 (asi llegan los nombres de un .zip).
inline std::string ucs2be_to_utf8(const uint8_t* p, std::size_t bytes) {
    std::string out;
    out.reserve(bytes);
    for (std::size_t i = 0; i + 1 < bytes; i += 2) {
        const uint32_t c = (uint32_t(p[i]) << 8) | p[i + 1];
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 0x3F));
        } else {
            // Los sustitutos de UTF-16 no se combinan: Joliet es UCS-2 y
            // ningun nombre de fichero real los usa. Se copian tal cual
            // codificados, que es reversible y no rompe la cadena.
            out += char(0xE0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }
    return out;
}

// Un identificador de la jerarquia primaria trae ';1' de version y, si el
// nombre no tenia extension, a veces un punto suelto al final.
inline std::string limpia_nombre_iso(std::string s) {
    const std::size_t punto_coma = s.rfind(';');
    if (punto_coma != std::string::npos) s.resize(punto_coma);
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

// ---------------------------------------------------------------------------
// El lector
// ---------------------------------------------------------------------------

class Reader {
public:
    // Enumera la imagen entera. 'why' explica el fallo si devuelve false.
    bool open(SectorReader read, std::string& why) {
        read_ = std::move(read);
        files_.clear();
        joliet_ = false;
        label_.clear();

        uint8_t raiz[34] = {0};
        if (!buscar_descriptores(raiz, why)) return false;

        // El registro de directorio raiz vive dentro del descriptor: su
        // extent y su tamano estan en los mismos campos que en cualquier
        // otro registro.
        const uint32_t lba  = le32(raiz + 2);
        const uint32_t size = le32(raiz + 10);
        if (size == 0) { why = "el directorio raiz esta vacio"; return false; }

        recorrer(lba, size, "", 0);
        if (files_.empty()) {
            why = "la imagen no tiene ningun fichero";
            return false;
        }
        return true;
    }

    const std::vector<IsoFile>& files() const { return files_; }

    // true si los nombres salieron de la jerarquia Joliet (o sea, tal cual
    // los escribio quien creo la imagen) y no de la primaria.
    bool has_joliet() const { return joliet_; }
    const std::string& label() const { return label_; }

private:
    // Recorre la cadena de descriptores de volumen y se queda con el mejor:
    // el suplementario de Joliet si existe, si no el primario.
    bool buscar_descriptores(uint8_t raiz_out[34], std::string& why) {
        bool hay_primario = false;
        uint8_t buf[kSectorSize];

        for (uint32_t lba = kFirstVdLba; lba < kMaxVdLba; ++lba) {
            if (!read_(uint64_t(lba) * kSectorSize, buf, kSectorSize)) break;
            if (std::memcmp(buf + 1, "CD001", 5) != 0) {
                // El primer sector TIENE que ser un descriptor; si no lo es,
                // esto no es una imagen ISO9660.
                if (lba == kFirstVdLba) {
                    why = "no hay 'CD001' en el sector 16: no es una imagen "
                          "ISO9660";
                    return false;
                }
                break;
            }
            const uint8_t tipo = buf[0];
            if (tipo == 255) break;                 // terminador

            if (tipo == 1 && !hay_primario) {       // primario
                hay_primario = true;
                if (!joliet_) {
                    std::memcpy(raiz_out, buf + 156, 34);
                    label_ = recorta(std::string(
                        reinterpret_cast<const char*>(buf + 40), 32));
                }
            } else if (tipo == 2 && es_joliet(buf + 88)) {
                joliet_ = true;
                std::memcpy(raiz_out, buf + 156, 34);
                label_ = recorta(ucs2be_to_utf8(buf + 40, 32));
            }
        }

        if (!hay_primario && !joliet_) {
            why = "no se encontro ningun descriptor de volumen";
            return false;
        }
        return true;
    }

    // Un descriptor suplementario es Joliet si su secuencia de escape es
    // una de las tres de UCS-2 (niveles 1, 2 y 3).
    static bool es_joliet(const uint8_t* escape) {
        static const char* kSecuencias[] = { "%/@", "%/C", "%/E" };
        for (const char* s : kSecuencias)
            if (std::memcmp(escape, s, 3) == 0) return true;
        return false;
    }

    static std::string recorta(std::string s) {
        while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
        return s;
    }

    void recorrer(uint32_t lba, uint32_t size, const std::string& prefijo,
                  uint32_t profundidad) {
        if (profundidad > kMaxDepth) return;
        if (size == 0 || size > 64u * 1024 * 1024) return;   // tope defensivo
        if (files_.size() >= kMaxEntries) return;

        std::vector<uint8_t> ext(size);
        if (!read_(uint64_t(lba) * kSectorSize, ext.data(), size)) return;

        // Los subdirectorios se recorren DESPUES de terminar este extent:
        // recursar en mitad del recorrido gastaria un buffer del tamano del
        // directorio por cada nivel.
        std::vector<std::pair<uint32_t, uint32_t>> subdirs;
        std::vector<std::string> nombres_subdir;

        std::size_t pos = 0;
        while (pos + 33 <= size) {
            const uint8_t largo = ext[pos];
            if (largo == 0) {
                // Relleno hasta el final del sector: un registro no puede
                // cruzar el limite, asi que el hueco se salta entero.
                const std::size_t siguiente =
                    (pos / kSectorSize + 1) * kSectorSize;
                if (siguiente <= pos || siguiente >= size) break;
                pos = siguiente;
                continue;
            }
            if (largo < 33 || pos + largo > size) break;

            const uint8_t* r = ext.data() + pos;
            const uint32_t hijo_lba  = le32(r + 2);
            const uint32_t hijo_size = le32(r + 10);
            const uint8_t  flags     = r[25];
            const uint8_t  len_id    = r[32];

            if (len_id == 0 || std::size_t(33) + len_id > largo) { pos += largo; continue; }
            // Multi-extent: el estandar permite partir un fichero en varios
            // registros seguidos. No se soporta -- ninguna herramienta
            // moderna los emite por debajo de 4 GiB -- pero hay que saltarse
            // el registro en vez de darlo por bueno a medias.
            const bool multi_extent = (flags & 0x80) != 0;

            const char* id = reinterpret_cast<const char*>(r + 33);
            const bool  yo_o_padre = (len_id == 1 && (id[0] == 0 || id[0] == 1));

            if (!yo_o_padre) {
                std::string nombre = joliet_
                    ? ucs2be_to_utf8(r + 33, len_id)
                    : limpia_nombre_iso(std::string(id, len_id));

                if (!nombre.empty() && !multi_extent) {
                    if (flags & 0x02) {                  // directorio
                        subdirs.emplace_back(hijo_lba, hijo_size);
                        nombres_subdir.push_back(nombre);
                    } else if (!(flags & 0x01)) {        // no oculto
                        IsoFile f;
                        f.name = nombre;
                        f.path = prefijo.empty() ? nombre : prefijo + "/" + nombre;
                        f.lba  = hijo_lba;
                        f.size = hijo_size;
                        // Area de uso de sistema: va detras del nombre, con
                        // un byte de relleno si el nombre mide par. La
                        // extension XA son 14 bytes con la firma en el 6.
                        {
                            std::size_t su = 33 + len_id + ((len_id % 2 == 0) ? 1 : 0);
                            if (su + 14 <= largo && r[su + 6] == 'X' && r[su + 7] == 'A') {
                                f.has_xa  = true;
                                f.xa_attr = uint16_t((uint16_t(r[su + 4]) << 8) |
                                                      uint16_t(r[su + 5]));
                            }
                        }
                        files_.push_back(std::move(f));
                        if (files_.size() >= kMaxEntries) return;
                    }
                }
            }
            pos += largo;
        }

        for (std::size_t i = 0; i < subdirs.size(); ++i) {
            const std::string sub = prefijo.empty()
                ? nombres_subdir[i]
                : prefijo + "/" + nombres_subdir[i];
            recorrer(subdirs[i].first, subdirs[i].second, sub, profundidad + 1);
        }
    }

    SectorReader          read_;
    std::vector<IsoFile>  files_;
    bool                  joliet_ = false;
    std::string           label_;
};

}  // namespace iso9660
