// vfs_bridge.hpp
//
// Única puerta de entrada a E/S de archivos del core. Restricción del
// proyecto: prohibido fopen/fread/std::ifstream. Todo pasa por
// retro_vfs_interface, obtenido en retro_set_environment vía
// RETRO_ENVIRONMENT_GET_VFS_INTERFACE.
//
// CUIDADO al adaptar librerías de terceros sobre este bridge:
// retro_vfs_seek() sólo confirma éxito (0) o fallo (-1) -- así lo
// documenta libretro.h ("@return 0 on success, -1 on failure") -- y NO
// devuelve la posición resultante. Cualquier adaptador que necesite la
// posición absoluta (p.ej. sevenzip_vfs_adapter.hpp, que la usa para
// calcular el tamaño del archivo con Seek(0, SEEK_END)) debe pedirla
// aparte con stream_tell() tras cada seek exitoso.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "libretro.h"

// Interfaz virtual pura para poder testear sin RetroArch real (mock en tests/).
class IVFSBridge {
public:
    virtual ~IVFSBridge() = default;

    // Handle opaco de streaming. nullptr = fallo.
    virtual void*   stream_open(const char* path) noexcept = 0;
    virtual int64_t stream_read(void* handle, void* buf, uint64_t len) noexcept = 0;
    virtual int64_t stream_seek(void* handle, int64_t offset, int whence) noexcept = 0;
    virtual int64_t stream_tell(void* handle) noexcept = 0;
    virtual int64_t stream_size(void* handle) noexcept = 0;
    virtual int     stream_close(void* handle) noexcept = 0;

    virtual bool is_valid() const noexcept = 0;

    // Lee un fichero completo a un buffer nuevo. Usada por:
    //  - el cargador principal para PSF/MiniPSF (aosdk necesita el fichero
    //    entero antes de llamar a psf_start/psf2_start).
    //  - el callback ao_get_lib para resolver ficheros _libN.
    // Puede fallar (fichero no encontrado, I/O). No asigna en el hot path
    // de audio: solo se llama durante carga.
    virtual bool read_whole_file(const std::string& path,
                                  std::vector<uint8_t>& out) noexcept = 0;

    // Resuelve un nombre de librería (_lib, _lib2...) relativo al directorio
    // base del fichero principal cargado. Necesario porque ao_get_lib y
    // psf_file_callbacks reciben nombres relativos, no absolutos.
    virtual std::string resolve_relative(const std::string& base_dir,
                                          const std::string& relative_name) const = 0;
};

// RAII: cierra el stream automáticamente si algo lanza entre open y close.
class VFSStreamGuard {
public:
    VFSStreamGuard(IVFSBridge& vfs, const char* path) noexcept
        : vfs_(vfs), handle_(vfs.stream_open(path)) {}
    ~VFSStreamGuard() { if (handle_) vfs_.stream_close(handle_); }

    VFSStreamGuard(const VFSStreamGuard&) = delete;
    VFSStreamGuard& operator=(const VFSStreamGuard&) = delete;

    bool   valid() const noexcept { return handle_ != nullptr; }
    void*  get()   const noexcept { return handle_; }

private:
    IVFSBridge& vfs_;
    void* handle_;
};

// Objeto nulo: usado cuando el frontend no expone VFS. libgme no lo
// necesita (lee de memoria), pero la interfaz IAudioEngine::open() exige
// una referencia válida — mejor un objeto inerte y explícito que dejar
// una referencia potencialmente colgante si g_vfs es nullptr.
class NullVFSBridge final : public IVFSBridge {
public:
    bool is_valid() const noexcept override { return false; }
    void* stream_open(const char*) noexcept override { return nullptr; }
    int64_t stream_read(void*, void*, uint64_t) noexcept override { return -1; }
    int64_t stream_seek(void*, int64_t, int) noexcept override { return -1; }
    int64_t stream_tell(void*) noexcept override { return -1; }
    int64_t stream_size(void*) noexcept override { return -1; }
    int     stream_close(void*) noexcept override { return -1; }
    bool read_whole_file(const std::string&, std::vector<uint8_t>&) noexcept override { return false; }
    std::string resolve_relative(const std::string&, const std::string& rel) const override { return rel; }
};

class LibretroVFS final : public IVFSBridge {
public:
    explicit LibretroVFS(retro_vfs_interface* iface) noexcept : vfs_(iface) {}

    bool is_valid() const noexcept override { return vfs_ != nullptr; }

    void* stream_open(const char* path) noexcept override {
        if (!vfs_ || !path) return nullptr;
        return vfs_->open(path, RETRO_VFS_FILE_ACCESS_READ,
                                 RETRO_VFS_FILE_ACCESS_HINT_NONE);
    }

    int64_t stream_read(void* handle, void* buf, uint64_t len) noexcept override {
        if (!vfs_ || !handle) return -1;
        return vfs_->read(static_cast<retro_vfs_file_handle*>(handle), buf, len);
    }

    // retro_vfs_seek() solo confirma éxito (0) / fallo (-1) -- ver la
    // cabecera de este fichero. Esta clase reenvía ese resultado tal
    // cual; la posición absoluta resultante, si hace falta, se pide
    // aparte con stream_tell().
    int64_t stream_seek(void* handle, int64_t offset, int whence) noexcept override {
        if (!vfs_ || !handle) return -1;
        return vfs_->seek(static_cast<retro_vfs_file_handle*>(handle), offset, whence);
    }

    int64_t stream_tell(void* handle) noexcept override {
        if (!vfs_ || !handle) return -1;
        return vfs_->tell(static_cast<retro_vfs_file_handle*>(handle));
    }

    int64_t stream_size(void* handle) noexcept override {
        if (!vfs_ || !handle) return -1;
        return vfs_->size(static_cast<retro_vfs_file_handle*>(handle));
    }

    int stream_close(void* handle) noexcept override {
        if (!vfs_ || !handle) return -1;
        return vfs_->close(static_cast<retro_vfs_file_handle*>(handle));
    }

    bool read_whole_file(const std::string& path, std::vector<uint8_t>& out) noexcept override {
        out.clear();
        VFSStreamGuard guard(*this, path.c_str());
        if (!guard.valid()) return false;

        const int64_t sz = stream_size(guard.get());
        if (sz < 0) return false;

        try {
            out.resize(static_cast<size_t>(sz));
        } catch (...) {
            return false;
        }
        if (sz == 0) return true;

        uint64_t total_read = 0;
        while (total_read < static_cast<uint64_t>(sz)) {
            const int64_t got = stream_read(guard.get(), out.data() + total_read,
                                             static_cast<uint64_t>(sz) - total_read);
            if (got <= 0) return false; // error o EOF prematuro
            total_read += static_cast<uint64_t>(got);
        }
        return true;
    }

    std::string resolve_relative(const std::string& base_dir,
                                  const std::string& relative_name) const override {
        if (base_dir.empty()) return relative_name;
        const char sep =
#ifdef _WIN32
            '\\';
#else
            '/';
#endif
        if (!base_dir.empty() && (base_dir.back() == '/' || base_dir.back() == '\\'))
            return base_dir + relative_name;
        return base_dir + sep + relative_name;
    }

private:
    retro_vfs_interface* vfs_ = nullptr;
};

// Extrae el directorio base de una ruta completa (para resolver _libN).
// No usa <filesystem> con fopen implícito de ningún tipo; es manipulación
// de cadenas pura.
inline std::string vfs_dirname(const std::string& full_path) {
    const auto slash = full_path.find_last_of("/\\");
    if (slash == std::string::npos) return std::string();
    return full_path.substr(0, slash);
}
