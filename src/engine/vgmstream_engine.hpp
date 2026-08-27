// vgmstream_engine.hpp
//
// Envuelve vgmstream (ISC). Cubre formatos de streaming: CD-XA, VAG y sus
// derivados de PS2, CRI ADX/AHX, streams de Dreamcast/Naomi, DSP de
// GameCube/Wii, STRM de NDS y los contenedores genéricos RIFF/GENH.
//
// TRES COSAS QUE NO SE PARECEN A LOS OTROS MOTORES:
//
// 1. La frecuencia nativa casi nunca es 44100. XA son 37800 o 18900 Hz,
//    el DSP de GameCube 32000, GENH lo que diga la cabecera. El core está
//    clavado a 44100 (retro_get_system_av_info) y no puede cambiarlo por
//    pista, así que se le pide a vgmstream que remuestree él mismo:
//    OpenConfig::output_rate. La tasa original se conserva en
//    TrackMetadata para que la UI la enseñe.
//
// 2. Los subsongs no son pistas independientes que se puedan seleccionar
//    en caliente. En vgmstream el subsong se fija al ABRIR el stream, así
//    que select_track() reabre desde el mismo streamfile. Por eso el
//    motor conserva el contenido de la pista y sabe reconstruir su
//    streamfile.
//
// 3. Un fichero de streaming es grande (un XA típico son 8-16 MB) y su
//    contenido se sirve desde memoria cuando viene de un .zip. El motor
//    NO copia ese buffer: guarda un puntero. El llamante debe mantenerlo
//    vivo mientras el motor exista, igual que hace zip_playlist con
//    ZipEntry::data.
//
// 4. EL NOMBRE ES OBLIGATORIO, no decorativo. vgmstream filtra candidatos
//    por extensión antes de mirar el magic, así que un nombre vacío hace
//    que TODO se rechace salvo lo que acepte extensión vacía -- que son
//    exactamente dos parsers, xa.c y vag.c, los únicos cuya lista incluye
//    la cadena vacía (el ",," de su check_extensions).
//
//    Las entradas de .zip llegan con uri = nullptr y el nombre sólo en
//    display_name, así que hay que pasarlo por el constructor. Sin eso,
//    dentro de un .zip sólo funcionan XA y VAG y todo lo demás falla con
//    ficheros perfectamente válidos.

#pragma once

#include <cstring>
#include <string>
#include <vector>

#include "iaudio_engine.hpp"
#include "vgmstream_api.hpp"
#include "../vgmstream_vfs_adapter.hpp"

class VgmstreamEngine final : public IAudioEngine {
public:
    // display_name: nombre CON EXTENSIÓN. Obligatorio para las entradas de
    // archivo, donde open() recibe uri = nullptr. Ver la nota 4.
    //
    // siblings: resuelve los ficheros acompañantes que algunos formatos
    // exigen. MIB+MIH es el caso vivo: meta/mib_mih.c abre el .mih hermano
    // y falla sin él, igual que upstream. Dentro de un .zip no hay rutas
    // que abrir, así que la búsqueda tiene que ir contra las entradas ya
    // descomprimidas. nullptr = sin hermanos (fichero suelto).
    VgmstreamEngine(double loop_count, int fade_seconds,
                    std::string display_name = std::string(),
                    vgmstream_vfs_adapter::SiblingLookup siblings =
                        vgmstream_vfs_adapter::SiblingLookup())
        : loop_count_(loop_count), fade_seconds_(fade_seconds),
          display_name_(std::move(display_name)),
          siblings_(std::move(siblings)) {}

    ~VgmstreamEngine() override { close_stream(); }

    VgmstreamEngine(const VgmstreamEngine&) = delete;
    VgmstreamEngine& operator=(const VgmstreamEngine&) = delete;

    // Tamaño real del fichero cuando 'data' es solo un prefijo (entrada de
    // .zip sin materializar). 0 = 'data' está completo. Hay que fijarlo
    // ANTES de open().
    void set_partial(std::size_t full_size) noexcept { full_size_ = full_size; }

    // Audio XA leido de la pista de datos de un .chd: abrir sin enumerar
    // los subsongs. Ver OpenConfig::xa_fast_open.
    void set_xa_fast_open(bool on) noexcept { xa_fast_open_ = on; }

    // True si al abrir hizo falta leer más allá del prefijo: la cabecera no
    // bastaba y hay que materializar la entrada entera y reintentar.
    bool needs_full_data() const noexcept { return truncated_; }

    bool open(const char* uri, const uint8_t* data, std::size_t size,
              IVFSBridge& vfs) override {
        close_stream();

        vfs_  = &vfs;
        path_ = uri ? uri : "";
        // El display_name manda sobre el uri: dentro de un .zip el uri es
        // nulo y el nombre es lo único que identifica la extensión.
        name_ = basename_of(!display_name_.empty() ? display_name_ : path_);

        if (data && size > 0) {
            // Entrada de .zip/.chd: ya descomprimida por el llamante. NO se
            // copia; ver la nota 3 de la cabecera.
            mem_      = data;
            mem_size_ = size;
        } else {
            mem_      = nullptr;
            mem_size_ = 0;
        }

        return select_track(0);
    }

    std::size_t render(int16_t* out, std::size_t max_frames) noexcept override {
        if (!handle_ || !out || max_frames == 0) return 0;
        // fill() escribe canales * frames int16 y no asigna: los buffers
        // internos se dimensionaron al abrir.
        const std::size_t frames = aolib::vgms::fill(handle_, out, max_frames);

        // MONO -> ESTÉREO. libvgmstream sólo sabe BAJAR de canales
        // (auto_downmix_channels), así que un fichero de 1 canal llega con
        // 1 canal y fill() escribe la MITAD del buffer que espera el core,
        // que es estéreo entrelazado de punta a punta. Sin esto se leen
        // muestras mono como pares L/R: el tono sube una octava, los
        // canales se cruzan y la mitad alta del bloque es el contenido
        // anterior. Afecta a NDS STRM mono, entre otros.
        //
        // La expansión va HACIA ATRÁS y en el mismo sitio: escribir de
        // izquierda a derecha pisaría muestras aún sin leer.
        if (channels_ == 1 && frames > 0) {
            for (std::size_t i = frames; i-- > 0; ) {
                const int16_t s = out[i];
                out[i * 2 + 0] = s;
                out[i * 2 + 1] = s;
            }
        }
        return frames;
    }

    bool end_of_track() const noexcept override {
        return !handle_ || aolib::vgms::done(handle_);
    }

    unsigned track_count() const noexcept override {
        return subsongs_ > 0 ? static_cast<unsigned>(subsongs_) : 1;
    }

    // El subsong se fija al abrir; cambiarlo obliga a reabrir. Es barato:
    // el contenido ya está en memoria o accesible por el VFS, y sólo se
    // reparsea la cabecera.
    bool select_track(unsigned index) override {
        if (index >= track_count() && index != 0) return false;

        libstreamfile_t* sf = make_streamfile();
        if (!sf) return false;

        aolib::vgms::OpenConfig cfg;
        cfg.loop_count  = loop_count_;
        cfg.fade_time   = static_cast<double>(fade_seconds_);
        cfg.downmix_to  = 2;
        cfg.output_rate = kSampleRate;
        cfg.xa_fast_open = xa_fast_open_ && index == 0;

        // subsong es 1-based en vgmstream; 0 significa "el primero".
        aolib::vgms::Handle h = aolib::vgms::open(sf, static_cast<int>(index) + 1, cfg);
        if (!h) { libstreamfile_close(sf); return false; }

        close_stream();
        handle_ = h;
        sf_     = sf;
        current_track_ = index;
        refresh_metadata();
        return true;
    }

    const TrackMetadata& metadata() const noexcept override { return meta_; }
    const char* engine_name() const noexcept override { return "vgmstream"; }

private:
    static constexpr int kSampleRate = 44100;

    static std::string basename_of(const std::string& p) {
        const std::size_t s = p.find_last_of("/\\");
        return s == std::string::npos ? p : p.substr(s + 1);
    }

    static std::string stem_of(const std::string& n) {
        const std::size_t d = n.find_last_of('.');
        return d == std::string::npos ? n : n.substr(0, d);
    }

    libstreamfile_t* make_streamfile() {
        libstreamfile_t* inner = nullptr;
        if (mem_) {
            // Prestado, sin copia: ver la nota 3 de la cabecera.
            truncated_ = false;
            inner = vgmstream_vfs_adapter::make_memory(mem_, mem_size_, name_, siblings_,
                                                       full_size_, &truncated_);
        } else if (vfs_ && vfs_->is_valid()) {
            inner = vgmstream_vfs_adapter::make_vfs(*vfs_, path_);
        }
        // Sin envolver en cache, cada parseo de cabecera son decenas de
        // lecturas pequeñas dispersas, una ida y vuelta al VFS cada una.
        return vgmstream_vfs_adapter::buffered(inner);
    }

    void close_stream() noexcept {
        if (handle_) { aolib::vgms::close(handle_); handle_ = nullptr; }
        if (sf_) { libstreamfile_close(sf_); sf_ = nullptr; }
    }

    void refresh_metadata() noexcept {
        meta_ = TrackMetadata{};
        if (!handle_) return;

        aolib::vgms::StreamInfo si;
        if (!aolib::vgms::info(handle_, si)) return;

        subsongs_ = si.subsongs;
        channels_ = si.channels;

        // total_frames ya viene en la tasa de SALIDA (44100), porque el
        // remuestreo se configura antes de abrir. No hay que reescalar.
        meta_.total_frames = static_cast<uint64_t>(si.total_frames);

        // length_frames marca dónde EMPIEZA el fade, que es lo que la UI
        // usa para el desplome del VU. vgmstream no lo expone por separado,
        // así que se deduce restando el fade configurado; si la pista es
        // más corta que el fade, no hay tramo previo que marcar.
        const uint64_t fade = static_cast<uint64_t>(fade_seconds_) * kSampleRate;
        if (si.loops && meta_.total_frames > fade) {
            meta_.fade_frames   = fade;
            meta_.length_frames = meta_.total_frames - fade;
        } else {
            meta_.length_frames = meta_.total_frames;
        }

        // Los formatos de streaming casi nunca traen tags: no hay
        // equivalente al bloque Corlett de PSF ni a los campos de libgme.
        // Dejar el título vacío hace que la UI muestre 'Reproduciendo: ""',
        // así que se usa el nombre interno cuando existe (el marcador
        // file+channel de XA, el nombre de banco de BNK) y si no el nombre
        // del fichero sin extensión, que es lo que el oyente reconoce.
        std::string title = aolib::vgms::title(handle_);
        if (title.empty() || title == name_) title = stem_of(name_);
        meta_.title = title;

        // Con varios subsongs el nombre del fichero se repetiría en todas
        // las pistas; se numera para poder distinguirlas en la lista.
        if (subsongs_ > 1) {
            meta_.title += " [" + std::to_string(current_track_ + 1) + "/" +
                           std::to_string(subsongs_) + "]";
        }

        // 'chip' es el decodificador, no una máquina: es lo más parecido a
        // "qué está sonando" que tiene un formato de streaming, donde no
        // hay chip emulado. La cadena de vgmstream es descriptiva
        // ("CD-ROM XA 4-bit ADPCM"); la UI dispone de un ancho corto.
        // "PARSER / códec": son cosas distintas y confundirlas ya costó un
        // informe de bug. caf.c (CAF de tri-Crescendo, Baten Kaitos) lleva
        // ADPCM de GameCube dentro, así que sin el parser delante el log
        // decía sólo "GC DSP" y parecía un fallback.
        meta_.chip = si.meta_name.empty()
                   ? short_codec_name(si.codec_name)
                   : si.meta_name + " / " + short_codec_name(si.codec_name);

        // La tasa nativa es dato de pista, no del core: el core siempre
        // saca 44100. Se guarda en el comentario porque TrackMetadata no
        // tiene campo propio y es información que el oyente quiere ver.
        if (si.native_rate > 0 && si.native_rate != kSampleRate) {
            meta_.comment = std::to_string(si.native_rate) + " Hz";
        }
    }

    // "CD-ROM XA 4-bit ADPCM" -> "CD-XA". Mapeo explícito y no un recorte
    // de la cadena: los nombres de vgmstream no comparten prefijo útil y
    // truncar daría "CD-ROM XA 4-b".
    static std::string short_codec_name(const std::string& full) {
        struct Entry { const char* needle; const char* shortname; };
        static const Entry kMap[] = {
            {"CD-ROM XA",      "CD-XA"},
            {"PlayStation",    "SPU"},
            {"Nintendo DSP",   "GC DSP"},
            {"CRI ADX",        "ADX"},
            {"CRI AHX",        "AHX"},
            {"Konami MTAF",    "MTAF"},
            {"Yamaha",         "AICA"},
            {"Big Endian PCM", "PCM"},
            {"PCM",            "PCM"},
            {"IMA",            "IMA"},
            {"Westwood",       "WS ADPCM"},
            {"Electronic Arts","EA ADPCM"},
        };
        for (const Entry& e : kMap) {
            if (full.find(e.needle) != std::string::npos) return e.shortname;
        }
        // Sin coincidencia: primera palabra, mejor que una cadena vacía o
        // un truncado a media palabra.
        const std::size_t sp = full.find(' ');
        return sp == std::string::npos ? full : full.substr(0, sp);
    }

    double      loop_count_   = 2.0;
    int         fade_seconds_ = 10;
    std::string display_name_;
    vgmstream_vfs_adapter::SiblingLookup siblings_;

    IVFSBridge* vfs_ = nullptr;
    std::string path_;
    std::string name_;

    const uint8_t* mem_       = nullptr;
    std::size_t    mem_size_  = 0;
    std::size_t    full_size_ = 0;   // 0 = mem_ completo
    bool           truncated_ = false;

    aolib::vgms::Handle handle_ = nullptr;
    libstreamfile_t*    sf_     = nullptr;

    // Solo lo pone el reparto para una entrada que se lee del puente de
    // una pista de datos de .chd: es ahi donde enumerar cuesta descomprimir
    // el fichero entero. Un .xa suelto o de un .zip sigue enumerando.
    bool     xa_fast_open_  = false;

    int      subsongs_      = 1;
    int      channels_      = 2;   // 1 obliga a expandir en render()
    unsigned current_track_ = 0;
    TrackMetadata meta_;
};
