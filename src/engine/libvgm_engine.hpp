// libvgm_engine.hpp — motor VGM/VGZ sobre libvgm (ValleyBell/libvgm; el
// commit exacto vendorizado está en deps/libvgm/VENDOR.md).
//
// Sustituye a libgme para .vgm/.vgz. libgme no sabe descomprimir .vgz por
// la ruta que usa este core (need_fullpath=false, gme_open_data() sin zlib
// enlazado), desincroniza el parser ante comandos VGM >= 1.60 (le falta el
// case 0x09 en command_len()) y falla EN SILENCIO ante chips que no
// reconoce (con YM2151 da pico=0 y ni siquiera un gme_warning()).
//
// IAudioEngine no cambia por esto: ningún otro fichero del core incluye
// cabeceras de libvgm.
//
// Detalles de la API que no son evidentes:
//   - open() exige este orden: RegisterPlayerEngine -> SetOutputSettings ->
//     MemoryLoader_Init -> DataLoader_Load -> LoadFile -> Start.
//     DataLoader_Load() TIENE que ir antes de LoadFile(), porque
//     VGMPlayer::LoadFile() lee de un loader que ya debe estar en
//     DLSTAT_LOADED.
//   - PlayerA::Render(bufSize, data) trabaja en BYTES, no en frames, tanto
//     en el argumento como en el valor devuelto (smplCount = bufSize /
//     _outSmplSizeA, con _outSmplSizeA = 2 canales * 2 bytes = 4).
//   - PLR_SONG_INFO::songLen ya viene en frames a 44100 Hz: VGMPlayer fija
//     tickRateMul=1 y tickRateDiv=44100, así que para este formato "tick" y
//     "muestra" son la misma unidad.
//
// TRAMPA que motiva verify_all_devices_started(): con SNDDEV_SELECT,
// declarar SNDDEV_<chip> sin compilar ningún EC_<chip>_* hace que
// SndEmu_Start2() devuelva EERR_NOT_FOUND y que VGMPlayer::InitDevices() se
// lo trague sin ningún log -- el mismo fallo silencioso que se quería
// evitar cambiando de libgme, reintroducido por un define olvidado.
// GetSongDeviceInfo() reporta smplRate=0 para esos chips, pero solo DESPUÉS
// de Start(): antes, todos los dispositivos reportan 0 sin que signifique
// nada.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "iaudio_engine.hpp"

extern "C" {
#include "utils/DataLoader.h"
#include "utils/MemoryLoader.h"
}
#include "emu/EmuStructs.h"
#include "emu/SoundEmu.h"
#include "player/playera.hpp"
#include "player/playerbase.hpp"
#include "player/vgmplayer.hpp"

// playera.cpp ya da '#error unknown endianness' si no hay ninguno de los
// dos definidos; esto comprueba que el definido sea el CORRECTO. Este
// proyecto solo apunta a plataformas little-endian, así que VGM_BIG_ENDIAN
// aquí sería un error de configuración del Makefile.
#if defined(VGM_BIG_ENDIAN)
#error "libvgm_engine.hpp: VGM_BIG_ENDIAN definido -- este proyecto solo apunta a plataformas little-endian (ver LIBVGM_CFLAGS en el Makefile)."
#endif
#if !defined(VGM_LITTLE_ENDIAN)
#error "libvgm_engine.hpp: falta -DVGM_LITTLE_ENDIAN (ver LIBVGM_CFLAGS en el Makefile)."
#endif

class LibvgmEngine final : public IAudioEngine {
public:
    // Configuración ya resuelta, como en GmeEngine: no se guarda una
    // referencia a CoreOptions entera porque estos dos campos son los
    // únicos que este motor necesita.
    explicit LibvgmEngine(int fade_length_msecs, bool loop_infinite) noexcept
        : fade_length_msecs_(fade_length_msecs), loop_infinite_(loop_infinite) {}

    ~LibvgmEngine() override {
        // El destructor de PlayerA hace Stop()+UnloadFile()+
        // UnregisterAllPlayers(), lo que borra el VGMPlayer* registrado en
        // open(). El DATA_LOADER, en cambio, NO es suyo: MemoryLoader_Init()
        // no copia, y la CLI de referencia de libvgm siempre llama a
        // DataLoader_Deinit() desde el llamante. Liberarlo aquí es seguro
        // porque VGMPlayer::UnloadFile() solo pone _dLoad/_fileData a NULL
        // sin desreferenciarlos.
        if (loader_) {
            DataLoader_Deinit(loader_);
            loader_ = nullptr;
        }
    }

    bool open(const char* /*uri*/, const uint8_t* data, std::size_t size,
              IVFSBridge& /*vfs*/) override {
        // VGM no necesita fullpath ni VFS: PlayerA acepta memoria
        // directamente. 'uri' y 'vfs' se ignoran a propósito.
        last_error_.clear();
        if (!data || size == 0) return false;

        // open() es el único punto donde se permite asignar (ver
        // iaudio_engine.hpp), y 'new VGMPlayer()' asigna: por eso está aquí
        // y no en el constructor.
        player_.RegisterPlayerEngine(new VGMPlayer());

        // Copia SIEMPRE, venga de donde venga: una entrada de .zip vive más
        // que el motor y no la necesitaría, pero retro_game_info::data no
        // está garantizado más allá de retro_load_game(). Distinguir el
        // origen aquí no compensa: un VGM es pequeño incluso sin
        // comprimir.
        owned_.assign(data, data + size);

        loader_ = MemoryLoader_Init(owned_.data(), static_cast<UINT32>(owned_.size()));
        if (!loader_) return false;

        // smplBufferLen debe ser >= kFramesPerRun (735) o Render() recorta
        // en silencio.
        static_assert(kLibvgmBufferFrames >= 735,
                      "kLibvgmBufferFrames debe ser >= kFramesPerRun (735)");
        if (player_.SetOutputSettings(kSampleRate, 2, 16, kLibvgmBufferFrames) != 0x00) {
            last_error_ = "SetOutputSettings() fallo (formato de salida no soportado)";
            return false;
        }

        // Orden obligatorio: DataLoader_Load() ANTES de LoadFile().
        // VGMPlayer::LoadFile() lee de los datos ya cargados; con el loader
        // todavia en DLSTAT_EMPTY no hay nada que leer.
        if (DataLoader_Load(loader_) != 0x00) {
            last_error_ = "DataLoader_Load() fallo (E/S de memoria)";
            return false;
        }

        if (player_.LoadFile(loader_) != 0x00) {
            last_error_ = "PlayerA::LoadFile() fallo (no es un VGM/VGZ valido, "
                          "o VGMPlayer::CanLoadFile() lo rechazo)";
            return false;
        }

        apply_config();

        if (player_.Start() != 0x00) {
            last_error_ = "PlayerA::Start() fallo";
            return false;
        }

        // Ver cabecera: detecta chips declarados sin core compilado. Puebla
        // last_error_ con el nombre del chip si falla.
        if (!verify_all_devices_started()) return false;

        refresh_metadata();
        return true;
    }

    std::size_t render(int16_t* out, std::size_t max_frames) noexcept override {
        // PlayerA::Render() opera en BYTES, no en frames, tanto a la
        // entrada como en el retorno (ver cabecera).
        const UINT32 bytes_requested = static_cast<UINT32>(max_frames) * kBytesPerFrame;
        const UINT32 bytes_written = player_.Render(bytes_requested, out);
        return static_cast<std::size_t>(bytes_written) / kBytesPerFrame;
    }

    bool end_of_track() const noexcept override {
        return (player_.GetState() & PLAYSTATE_FIN) != 0;
    }

    // VGM no tiene subsongs: select_track(0) es "reinicia la misma pista",
    // igual que en PsfEngine/Psf2Engine.
    bool select_track(unsigned index) override {
        if (index != 0) return false;
        return player_.Reset() == 0x00;
    }

    const TrackMetadata& metadata() const noexcept override { return meta_; }
    const char* engine_name() const noexcept override { return "libvgm-vgm"; }

    // Fuera de IAudioEngine a propósito. Solo la consulta
    // construct_engine_for(), sobre el puntero concreto y antes de subirlo
    // a unique_ptr<IAudioEngine>, para que el log pueda decir QUÉ chip
    // faltaba en vez de solo "open() falló".
    const std::string& last_open_error() const noexcept { return last_error_; }

private:
    static constexpr UINT32 kSampleRate = 44100;
    static constexpr UINT32 kBytesPerFrame = 4; // 2 canales * 16 bits
    // Muy por encima del mínimo de 735 frames, para que Render() no recorte
    // ni aunque algún día se pida más de un frame de vídeo de golpe. Es un
    // buffer interno de PlayerA, sin relación con ningún tamaño del core.
    static constexpr UINT32 kLibvgmBufferFrames = 8192;

    void apply_config() noexcept {
        // Sin SetFadeSamples(), PlayerA::Config::fadeSmpls se queda en 0 y
        // el final del bucle corta en seco en vez de fundir.
        const UINT32 fade_smpls = static_cast<UINT32>(
            (static_cast<uint64_t>(fade_length_msecs_) * kSampleRate) / 1000);
        player_.SetFadeSamples(fade_smpls);

        // loopCount > 0 es lo que habilita el corte, así que 0 = infinito.
        // Si no, se deja el valor por defecto de PlayerA (2 bucles), que es
        // también el de VGMPlay.
        if (loop_infinite_) {
            player_.SetLoopCount(0);
        }
    }

    // Detecta chips declarados por el fichero cuyo core de emulación no
    // arrancó. Debe llamarse DESPUÉS de Start(): antes, todos los
    // dispositivos reportan smplRate=0 sin que signifique nada.
    bool verify_all_devices_started() {
        // GetTags()/GetSongInfo() no son 'const' aunque GetSongDeviceInfo()
        // sí lo sea: se usa el puntero no-const en todo el fichero.
        PlayerBase* player = player_.GetPlayer();
        if (!player) {
            last_error_ = "verify_all_devices_started(): sin PlayerBase activo tras Start()";
            return false;
        }

        std::vector<PLR_DEV_INFO> dev_info;
        // CUIDADO: GetSongDeviceInfo() tiene DOS códigos de éxito, 0x00
        // (datos de la cabecera del fichero) y 0x01 (datos en vivo del
        // dispositivo, que es lo que devuelve siempre tras Start(), con la
        // pista en PLAYSTATE_PLAY). Tratar todo lo que no sea 0x00 como
        // fallo, que es la convención habitual en el resto de libvgm,
        // rechazaría aquí un éxito legítimo. Solo 0xFF es error real
        // (_dLoad == NULL).
        if (player->GetSongDeviceInfo(dev_info) == 0xFF) {
            last_error_ = "verify_all_devices_started(): GetSongDeviceInfo() fallo (sin DATA_LOADER activo)";
            return false;
        }

        for (const PLR_DEV_INFO& dev : dev_info) {
            if (dev.smplRate != 0) continue;

            // Chip declarado por la cabecera VGM cuyo core no arrancó.
            // SndEmu_GetDevName() resuelve el nombre con solo SNDDEV_<chip>
            // definido, sin necesitar el EC_<chip>_*; si aun así falla, se
            // reporta el DEV_ID en hexadecimal.
            const char* name = SndEmu_GetDevName(dev.type, 0x01, dev.devCfg);
            char buf[160];
            if (name) {
                std::snprintf(buf, sizeof(buf),
                    "chip declarado sin core compilado: %s (DEV_ID 0x%02X) -- "
                    "revisar EC_<chip>_* en LIBVGM_CFLAGS",
                    name, static_cast<unsigned>(dev.type));
            } else {
                std::snprintf(buf, sizeof(buf),
                    "chip declarado sin core compilado: DEV_ID 0x%02X (nombre no resuelto) -- "
                    "revisar EC_<chip>_* en LIBVGM_CFLAGS",
                    static_cast<unsigned>(dev.type));
            }
            last_error_ = buf;
            return false;
        }
        return true;
    }

    void refresh_metadata() noexcept {
        meta_ = TrackMetadata{};

        PlayerBase* player = player_.GetPlayer();
        if (!player) return;

        // GD3: pares clave/valor terminados en NULL. GetTags() es pura
        // virtual en PlayerBase, no hace falta downcast a VGMPlayer*.
        const char* const* tags = player->GetTags();
        if (tags) {
            for (std::size_t i = 0; tags[i] != nullptr && tags[i + 1] != nullptr; i += 2) {
                const std::string key = tags[i];
                const std::string value = tags[i + 1];
                if (value.empty()) continue;
                // Las variantes -JPN solo se usan si la no-JPN vino vacía.
                if (key == "TITLE" || (key == "TITLE-JPN" && meta_.title.empty()))
                    meta_.title = value;
                else if (key == "GAME" || (key == "GAME-JPN" && meta_.game.empty()))
                    meta_.game = value;
                else if (key == "ARTIST" || (key == "ARTIST-JPN" && meta_.artist.empty()))
                    meta_.artist = value;
                else if (key == "DATE")
                    meta_.year = value;
                else if (key == "COMMENT")
                    meta_.comment = value;
                // SYSTEM/SYSTEM-JPN/ENCODED_BY no tienen campo en
                // TrackMetadata; se omiten.
            }
        }
        // GD3 no tiene campo de copyright, a diferencia de los tags
        // Corlett: meta_.copyright_notice se queda vacío.

        PLR_SONG_INFO song_info{};
        if (player->GetSongInfo(song_info) == 0x00) {
            // songLen ya viene en frames a 44100 Hz (ver cabecera).
            meta_.length_frames = song_info.songLen;
        }
        // VGM/GD3 no declara fade propio: esto refleja lo configurado, no
        // algo leído del fichero.
        meta_.fade_frames =
            (static_cast<uint64_t>(fade_length_msecs_) * kSampleRate) / 1000;

        // Duración REAL. En VGM la diferencia no es solo el fade: songLen es
        // UNA pasada del fichero (intro + un bucle), pero PlayerA reproduce
        // Config::loopCount bucles (2 por defecto) y encima añade el fade.
        // Para un tema que loopea, lo que suena es casi el doble.
        //
        // GetTotalTime() ya hace esa cuenta completa, así que se usa esa API
        // en vez de repetir la aritmética: si cambia el nº de bucles, el
        // total sigue cuadrando solo.
        //
        // Devuelve < 0 si la duración es infinita (loop_infinite activo con
        // un fichero que loopea): entonces total_frames = 0 = desconocida, y
        // la UI dibuja "--:--" con barra indeterminada.
        const double total_secs = player_.GetTotalTime(
            PLAYTIME_LOOP_INCL | PLAYTIME_WITH_FADE | PLAYTIME_WITH_SLNC);
        meta_.total_frames = (total_secs > 0.0)
            ? static_cast<uint64_t>(total_secs * kSampleRate)
            : 0;

        // Chips declarados por la cabecera del VGM. Un VGM puede declarar
        // varios (Mega Drive = YM2612 + SN76496): se listan hasta tres
        // nombres distintos unidos por '+', y el resto se resume como "+N"
        // para no desbordar el panel.
        meta_.chip = collect_chip_names(player);
    }

    // Nombres de chip del VGM cargado, deduplicados y en orden de
    // aparición. Cadena vacía si no se puede determinar (la UI dibuja "--").
    static std::string collect_chip_names(PlayerBase* player) {
        std::vector<PLR_DEV_INFO> dev_info;
        if (!player || player->GetSongDeviceInfo(dev_info) == 0xFF) return std::string();

        std::vector<std::string> names;
        for (const PLR_DEV_INFO& dev : dev_info) {
            const char* name = SndEmu_GetDevName(dev.type, 0x01, dev.devCfg);
            if (!name || !name[0]) continue;
            std::string n(name);
            bool dup = false;
            for (const auto& existing : names) if (existing == n) { dup = true; break; }
            if (!dup) names.push_back(std::move(n));
        }
        if (names.empty()) return std::string();

        std::string out;
        const std::size_t shown = names.size() < 3 ? names.size() : 3;
        for (std::size_t i = 0; i < shown; ++i) {
            if (i) out += '+';
            out += names[i];
        }
        if (names.size() > shown) {
            char buf[24];   // holgado: %zu puede escribir hasta 20 dígitos
            std::snprintf(buf, sizeof(buf), "+%zu", names.size() - shown);
            out += buf;
        }
        return out;
    }

    PlayerA player_;
    DATA_LOADER* loader_ = nullptr;
    std::vector<uint8_t> owned_;   // copia propia del fichero, ver open()
    TrackMetadata meta_;
    std::string last_error_;
    int fade_length_msecs_;
    bool loop_infinite_;
};
