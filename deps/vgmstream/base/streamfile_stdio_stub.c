/* Replaces base/streamfile_stdio.c, which is not vendored: it is the only
 * real filesystem backend in vgmstream and this project routes every read
 * through the libretro VFS instead.
 *
 * These two symbols cannot simply be dropped. base/api_libsf.c still
 * references them from libstreamfile_open_from_stdio() and
 * libstreamfile_open_from_file(). A Linux .so links without them, because
 * shared objects tolerate undefined symbols; a MinGW .dll does not, and
 * fails with "undefined reference to open_stdio_streamfile".
 *
 * Returning NULL makes those two entry points fail cleanly. Nothing in
 * the core calls them: streams arrive through the VFS-backed STREAMFILE.
 */
#include "../streamfile.h"

STREAMFILE* open_stdio_streamfile(const char* filename) {
    (void)filename;
    return NULL;
}

STREAMFILE* open_stdio_streamfile_by_file(FILE* file, const char* filename) {
    (void)file; (void)filename;
    return NULL;
}
