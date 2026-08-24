// vgmstream_vfs_adapter.hpp
//
// vgmstream's I/O interface is libstreamfile_t (deps/vgmstream/
// libvgmstream_streamfile.h): six callbacks, no filesystem assumed. This
// maps it onto the two shapes content actually arrives in.
//
//   make_vfs(...)     a loose file on disk, read through IVFSBridge.
//   make_memory(...)  a buffer already in RAM, which is how .zip and .7z
//                     entries arrive (ZipEntry::data holds the whole
//                     decompressed member).
//
// TWO THINGS THAT BITE HERE:
//
// 1. libstreamfile_t::read is POSITIONAL -- it takes an absolute offset
//    and vgmstream jumps around non-linearly. IVFSBridge is sequential.
//    So the adapter keeps its own cursor and seeks only when the wanted
//    offset differs from it, which also avoids a syscall per read on the
//    common sequential case.
//
// 2. retro_vfs_seek() returns 0 on success / -1 on failure, NOT the
//    resulting position (see vfs_bridge.hpp). The cursor here is
//    maintained by the adapter and confirmed with stream_tell() after
//    each seek, never inferred from the seek return value.
//
// Callers should wrap the result in libstreamfile_open_buffered(): every
// header parse does dozens of small reads at scattered offsets, and
// unbuffered that is one VFS round trip each.
//
// Every allocation here is nothrow ON PURPOSE. cb_open() is called BY
// vgmstream's C code, and letting a bad_alloc unwind through C frames is
// undefined behaviour.

#pragma once

#include <cstdint>
#include <cstring>
#include <new>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "vfs_bridge.hpp"

extern "C" {
#include "libvgmstream.h"
#include "libvgmstream_streamfile.h"
}

namespace vgmstream_vfs_adapter {

// Resolves a companion file requested through libstreamfile_t::open.
// Returns nullptr when the name is unknown. Archive-backed content passes
// a lookup over the sibling entries, mirroring AosdkLibResolver's
// SiblingLookup; loose files pass nothing and fall back to the VFS.
// Buffer prestado: el adaptador NUNCA copia el contenido, sólo apunta a
// él. Un fichero de streaming son megabytes; copiarlos por pista sería un
// coste enorme y silencioso.
struct MemoryView {
    const uint8_t* data = nullptr;
    std::size_t    size = 0;      // bytes REALMENTE disponibles en 'data'

    // Tamaño real del fichero cuando 'data' solo tiene un prefijo (entrada
    // de .zip todavía sin materializar). 0 = 'data' está completo.
    //
    // NO se puede reportar 'size' como get_size() en ese caso: hay formatos
    // que deducen el número de muestras del tamaño del fichero, y con el
    // prefijo darían una duración corta y creíble, que es peor que un
    // fallo. Reportando el tamaño real, esos formatos aciertan y los que
    // necesiten leer más allá del prefijo obtienen una lectura corta que
    // el llamante detecta.
    std::size_t    full_size = 0;

    std::size_t reported_size() const { return full_size ? full_size : size; }
};

using SiblingLookup = std::function<MemoryView(const std::string&)>;

namespace detail {

struct Context {
    // Exactly one of these two is live.
    IVFSBridge*          vfs    = nullptr;   // disk-backed
    void*                handle = nullptr;
    MemoryView           mem;                // memory-backed

    // Copia COMPARTIDA cuando 'mem' describe un HERMANO. La entrada del
    // .zip que lo respalda se libera justo después de construir el motor
    // (release_zip_entry en load_zip_entry_from), así que prestarla daba
    // un use-after-free en el bucle de audio.
    //
    // Compartida y no una copia por contexto porque la cadena es larga:
    // find_dual_file() abre el hermano, vgmstream_open_stream() abre una
    // copia POR CANAL a partir de ese streamfile, y luego el hermano se
    // cierra. Las copias por canal piden el mismo nombre, así que sin
    // compartir la propiedad volverían a apuntar al buffer del padre ya
    // muerto. Nulo cuando 'mem' es la entrada en reproducción, cuyo
    // buffer vive tanto como el motor.
    std::shared_ptr<const std::vector<uint8_t>> owned;

    // Bandera DEL LLAMANTE, no del contexto: se marca si alguien intentó
    // leer más allá del prefijo disponible. Vive fuera porque el streamfile
    // se envuelve en libstreamfile_open_buffered() y el envoltorio no
    // reenvía nada propio; el llamante conserva el bool y lo consulta
    // después de abrir. Con esa señal no hay que adivinar por extensión
    // qué formatos necesitan el fichero entero: XA lo necesita, el resto
    // no, y se detecta en vez de asumirse.
    bool*                truncated = nullptr;

    std::string   name;        // what get_name reports; the EXTENSION MATTERS,
                               // vgmstream filters candidates on it
    int64_t       size   = 0;
    int64_t       cursor = 0;  // our own; see note 2 in the header
    SiblingLookup siblings;
};

libstreamfile_t* make(std::unique_ptr<Context> ctx);

inline int cb_read(void* user_data, uint8_t* dst, int64_t offset, int length) {
    auto* c = static_cast<Context*>(user_data);
    if (!dst || offset < 0 || length <= 0) return 0;
    if (offset >= c->size) return 0;

    int64_t want = length;
    if (offset + want > c->size) want = c->size - offset;


    if (c->mem.data) {
        const int64_t have = static_cast<int64_t>(c->mem.size);
        if (offset >= have) { if (c->truncated) *c->truncated = true; return 0; }
        if (offset + want > have) {
            want = have - offset;
            if (c->truncated) *c->truncated = true;
        }
        std::memcpy(dst, c->mem.data + offset, static_cast<std::size_t>(want));
        c->cursor = offset + want;
        return static_cast<int>(want);
    }

    if (!c->vfs || !c->handle) return 0;

    if (offset != c->cursor) {
        if (c->vfs->stream_seek(c->handle, offset, RETRO_VFS_SEEK_POSITION_START) < 0)
            return 0;
        // stream_seek only confirms success; ask for the position rather
        // than assuming it landed where we asked.
        const int64_t at = c->vfs->stream_tell(c->handle);
        if (at < 0) return 0;
        c->cursor = at;
        if (c->cursor != offset) return 0;
    }

    // A short read is not EOF here: the VFS is free to return fewer bytes
    // than asked. vgmstream treats a short read as EOF, so loop.
    int64_t done = 0;
    while (done < want) {
        const int64_t got = c->vfs->stream_read(c->handle, dst + done,
                                                static_cast<uint64_t>(want - done));
        if (got <= 0) break;
        done   += got;
        c->cursor += got;
    }
    return static_cast<int>(done);
}

inline int64_t cb_size(void* user_data) {
    return static_cast<Context*>(user_data)->size;
}

inline const char* cb_name(void* user_data) {
    return static_cast<Context*>(user_data)->name.c_str();
}

inline void cb_close(libstreamfile_t* libsf) {
    if (!libsf) return;
    auto* c = static_cast<Context*>(libsf->user_data);
    if (c) {
        if (c->vfs && c->handle) c->vfs->stream_close(c->handle);
        delete c;
    }
    delete libsf;
}

inline libstreamfile_t* cb_open(void* user_data, const char* filename) {
    auto* c = static_cast<Context*>(user_data);
    if (!filename) return nullptr;
    const std::string wanted(filename);

    // Reopening the same name is the common case (vgmstream does it to get
    // an independent cursor) and must always work.
    const bool same = (wanted == c->name);

    // Archive-backed: siblings live in RAM, there is no path to open.
    if (c->siblings) {
        const MemoryView view = same ? c->mem : c->siblings(wanted);
        if (!view.data || view.size == 0) return nullptr;
        auto n = std::unique_ptr<Context>(new (std::nothrow) Context());
        if (!n) return nullptr;

        if (same) {
            // Mismo fichero: o es la entrada en reproducción (buffer vivo
            // mientras viva el motor) o es una reapertura de un hermano ya
            // copiado, y entonces se comparte la propiedad en vez de
            // copiar otra vez.
            n->mem   = view;
            n->owned = c->owned;
        } else {
            // HERMANO: hay que COPIAR. El motor conserva este streamfile
            // durante toda la pista, pero load_zip_entry_from() libera las
            // demás entradas del .zip JUSTO DESPUÉS de construirlo
            // (release_zip_entry en el bucle de más abajo), así que un
            // puntero prestado al hermano queda colgando.
            //
            // Es el caso del dual-stereo de GC DSP: ngc_dsp_std marca
            // allow_dual_stereo y vgmstream.c:74 fusiona el canal 2 desde
            // el fichero pareja (arctic1L.dsp <-> arctic1R.dsp de Worms
            // 3D). AddressSanitizer sobre ese par:
            //   heap-use-after-free, READ of size 32768
            //   cb_read -> decode_ngc_dsp -> render_layout_flat
            //           -> libvgmstream_fill -> VgmstreamEngine::render
            // El coste es una copia por hermano y por pista, en el cambio
            // de pista y fuera del bucle de audio.
            n->owned = std::make_shared<const std::vector<uint8_t>>(
                view.data, view.data + view.size);
            n->mem.data      = n->owned->data();
            n->mem.size      = n->owned->size();
            n->mem.full_size = view.full_size;
        }
        n->name     = same ? c->name : wanted;
        n->size     = static_cast<int64_t>(view.reported_size());
        n->siblings = c->siblings;
        // La bandera se hereda SIEMPRE, también al abrir un hermano. Varios
        // parsers reabren el fichero para escanearlo (xa.c lo hace para
        // enumerar los canales file+channel) y ngc_dsp_std abre el fichero
        // pareja para el dual-stereo de GC DSP (arctic1L.dsp <-> arctic1R.dsp
        // en Worms 3D). Poniéndola a nullptr para el hermano, una lectura
        // truncada en él no la ve nadie: needs_full_data() sigue en falso,
        // el core no materializa el hermano y el segundo canal se decodifica
        // desde un prefijo de 64 KiB. Medido con un par L/R de 1.108.824
        // bytes sirviendo 65.536 del hermano: abre, needs_full_data()=NO y
        // el último bloque sale L=0 contra L=15 con el hermano completo.
        n->truncated = c->truncated;
        return make(std::move(n));
    }

    if (c->mem.data) {
        if (!same) return nullptr;   // no way to reach anything else
        auto n = std::unique_ptr<Context>(new (std::nothrow) Context());
        if (!n) return nullptr;
        n->mem       = c->mem;
        n->name      = c->name;
        n->size      = c->size;
        n->truncated = c->truncated;   // ver la nota de arriba
        return make(std::move(n));
    }

    // Disk-backed: vgmstream builds companion paths off get_name, so the
    // string it hands back is already a full path when the original was.
    if (!c->vfs) return nullptr;
    void* h = c->vfs->stream_open(wanted.c_str());
    if (!h) return nullptr;
    const int64_t sz = c->vfs->stream_size(h);
    if (sz < 0) { c->vfs->stream_close(h); return nullptr; }

    auto n = std::unique_ptr<Context>(new (std::nothrow) Context());
        if (!n) return nullptr;
    n->vfs    = c->vfs;
    n->handle = h;
    n->name   = wanted;
    n->size   = sz;
    return make(std::move(n));
}

inline libstreamfile_t* make(std::unique_ptr<Context> ctx) {
    auto* sf = new (std::nothrow) libstreamfile_t();
    if (!sf) return nullptr;
    sf->user_data = ctx.release();
    sf->read      = &cb_read;
    sf->get_size  = &cb_size;
    sf->get_name  = &cb_name;
    sf->open      = &cb_open;
    sf->close     = &cb_close;
    return sf;
}

} // namespace detail

// Loose file on disk. 'path' is what get_name reports, so it must keep the
// real extension: vgmstream's parsers filter on it before reading magic.
// Returns nullptr if the file cannot be opened.
inline libstreamfile_t* make_vfs(IVFSBridge& vfs, const std::string& path) {
    if (!vfs.is_valid()) return nullptr;
    void* h = vfs.stream_open(path.c_str());
    if (!h) return nullptr;
    const int64_t sz = vfs.stream_size(h);
    if (sz <= 0) { vfs.stream_close(h); return nullptr; }

    auto ctx = std::unique_ptr<detail::Context>(new (std::nothrow) detail::Context());
    if (!ctx) { vfs.stream_close(h); return nullptr; }
    ctx->vfs    = &vfs;
    ctx->handle = h;
    ctx->name   = path;
    ctx->size   = sz;

    libstreamfile_t* sf = detail::make(std::move(ctx));
    if (!sf) { vfs.stream_close(h); return nullptr; }
    return sf;
}

// Buffer already in RAM: a .zip/.7z entry, or anything read whole. BORROWED,
// never copied -- 'data' must outlive the streamfile. ZipEntry::data does:
// the playlist owns it for as long as the archive is loaded. 'name' needs
// the entry's extension for the same reason as above. 'siblings' may be
// empty for formats with no companion files.
inline libstreamfile_t* make_memory(const uint8_t* data, std::size_t size,
                                    const std::string& name,
                                    SiblingLookup siblings = SiblingLookup(),
                                    std::size_t full_size = 0,
                                    bool* truncated = nullptr) {
    if (!data || size == 0) return nullptr;
    auto ctx = std::unique_ptr<detail::Context>(new (std::nothrow) detail::Context());
    if (!ctx) return nullptr;
    ctx->mem       = MemoryView{data, size, full_size};
    ctx->name      = name;
    ctx->size      = static_cast<int64_t>(ctx->mem.reported_size());
    ctx->siblings  = std::move(siblings);
    ctx->truncated = truncated;
    return detail::make(std::move(ctx));
}

// Comodidad para quien ya tiene un vector; tampoco copia.
inline libstreamfile_t* make_memory(const std::vector<uint8_t>& data,
                                    const std::string& name,
                                    SiblingLookup siblings = SiblingLookup()) {
    return make_memory(data.data(), data.size(), name, std::move(siblings));
}

// Wraps a streamfile in vgmstream's own cache. Header parsing does dozens
// of small scattered reads; unbuffered that is one VFS round trip each.
// Takes ownership: on success the inner streamfile is owned by the wrapper
// and must not be closed separately. On failure the input is closed and
// nullptr is returned, so the caller never has to guess who owns what.
inline libstreamfile_t* buffered(libstreamfile_t* inner) {
    if (!inner) return nullptr;
    libstreamfile_t* wrapped = libstreamfile_open_buffered(inner);
    if (!wrapped) { libstreamfile_close(inner); return nullptr; }
    return wrapped;
}

} // namespace vgmstream_vfs_adapter
