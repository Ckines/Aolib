/* AOLIB PATCH: the stdio backend of upstream ioapi.c has been removed.
   All file I/O in this core goes through the Libretro VFS
   (src/zip_vfs_adapter.hpp installs its own zlib_filefunc64_def), so the
   default fopen()-based callbacks are dead weight -- and keeping them
   would leave a real-fopen path linked into the binary, which the project
   forbids. fill_fopen_filefunc()/fill_fopen64_filefunc() are still
   defined, because unzOpen()/unzOpen64() reference them, but they install
   open callbacks that always fail. See deps/minizip/VENDOR.md. */

/* ioapi.h -- IO base function header for compress/uncompress .zip
   part of the MiniZip project - ( http://www.winimage.com/zLibDll/minizip.html )

         Copyright (C) 1998-2010 Gilles Vollant (minizip) ( http://www.winimage.com/zLibDll/minizip.html )

         Modifications for Zip64 support
         Copyright (C) 2009-2010 Mathias Svensson ( http://result42.com )

         For more info read MiniZip_info.txt

*/

#if defined(_WIN32) && (!(defined(_CRT_SECURE_NO_WARNINGS)))
        #define _CRT_SECURE_NO_WARNINGS
#endif



#include "ioapi.h"

voidpf call_zopen64 (const zlib_filefunc64_32_def* pfilefunc, const void*filename, int mode) {
    if (pfilefunc->zfile_func64.zopen64_file != NULL)
        return (*(pfilefunc->zfile_func64.zopen64_file)) (pfilefunc->zfile_func64.opaque,filename,mode);
    else
    {
        return (*(pfilefunc->zopen32_file))(pfilefunc->zfile_func64.opaque,(const char*)filename,mode);
    }
}

long call_zseek64 (const zlib_filefunc64_32_def* pfilefunc,voidpf filestream, ZPOS64_T offset, int origin) {
    if (pfilefunc->zfile_func64.zseek64_file != NULL)
        return (*(pfilefunc->zfile_func64.zseek64_file)) (pfilefunc->zfile_func64.opaque,filestream,offset,origin);
    else
    {
        uLong offsetTruncated = (uLong)offset;
        if (offsetTruncated != offset)
            return -1;
        else
            return (*(pfilefunc->zseek32_file))(pfilefunc->zfile_func64.opaque,filestream,offsetTruncated,origin);
    }
}

ZPOS64_T call_ztell64 (const zlib_filefunc64_32_def* pfilefunc, voidpf filestream) {
    if (pfilefunc->zfile_func64.zseek64_file != NULL)
        return (*(pfilefunc->zfile_func64.ztell64_file)) (pfilefunc->zfile_func64.opaque,filestream);
    else
    {
        uLong tell_uLong = (uLong)(*(pfilefunc->ztell32_file))(pfilefunc->zfile_func64.opaque,filestream);
        if ((tell_uLong) == MAXU32)
            return (ZPOS64_T)-1;
        else
            return tell_uLong;
    }
}

void fill_zlib_filefunc64_32_def_from_filefunc32(zlib_filefunc64_32_def* p_filefunc64_32, const zlib_filefunc_def* p_filefunc32) {
    p_filefunc64_32->zfile_func64.zopen64_file = NULL;
    p_filefunc64_32->zopen32_file = p_filefunc32->zopen_file;
    p_filefunc64_32->zfile_func64.zread_file = p_filefunc32->zread_file;
    p_filefunc64_32->zfile_func64.zwrite_file = p_filefunc32->zwrite_file;
    p_filefunc64_32->zfile_func64.ztell64_file = NULL;
    p_filefunc64_32->zfile_func64.zseek64_file = NULL;
    p_filefunc64_32->zfile_func64.zclose_file = p_filefunc32->zclose_file;
    p_filefunc64_32->zfile_func64.zerror_file = p_filefunc32->zerror_file;
    p_filefunc64_32->zfile_func64.opaque = p_filefunc32->opaque;
    p_filefunc64_32->zseek32_file = p_filefunc32->zseek_file;
    p_filefunc64_32->ztell32_file = p_filefunc32->ztell_file;
}


/* AOLIB PATCH -- see banner at the top of this file. */

static voidpf ZCALLBACK aolib_no_open_file_func(voidpf opaque, const char* filename, int mode) {
    (void)opaque; (void)filename; (void)mode;
    return NULL;
}

static voidpf ZCALLBACK aolib_no_open64_file_func(voidpf opaque, const void* filename, int mode) {
    (void)opaque; (void)filename; (void)mode;
    return NULL;
}

static uLong ZCALLBACK aolib_no_read_file_func(voidpf opaque, voidpf stream, void* buf, uLong size) {
    (void)opaque; (void)stream; (void)buf; (void)size;
    return 0;
}

static uLong ZCALLBACK aolib_no_write_file_func(voidpf opaque, voidpf stream, const void* buf, uLong size) {
    (void)opaque; (void)stream; (void)buf; (void)size;
    return 0;
}

static long ZCALLBACK aolib_no_tell_file_func(voidpf opaque, voidpf stream) {
    (void)opaque; (void)stream;
    return -1;
}

static ZPOS64_T ZCALLBACK aolib_no_tell64_file_func(voidpf opaque, voidpf stream) {
    (void)opaque; (void)stream;
    return (ZPOS64_T)-1;
}

static long ZCALLBACK aolib_no_seek_file_func(voidpf opaque, voidpf stream, uLong offset, int origin) {
    (void)opaque; (void)stream; (void)offset; (void)origin;
    return -1;
}

static long ZCALLBACK aolib_no_seek64_file_func(voidpf opaque, voidpf stream, ZPOS64_T offset, int origin) {
    (void)opaque; (void)stream; (void)offset; (void)origin;
    return -1;
}

static int ZCALLBACK aolib_no_close_file_func(voidpf opaque, voidpf stream) {
    (void)opaque; (void)stream;
    return -1;
}

static int ZCALLBACK aolib_no_error_file_func(voidpf opaque, voidpf stream) {
    (void)opaque; (void)stream;
    return -1;
}

void fill_fopen_filefunc(zlib_filefunc_def* pzlib_filefunc_def) {
    pzlib_filefunc_def->zopen_file = aolib_no_open_file_func;
    pzlib_filefunc_def->zread_file = aolib_no_read_file_func;
    pzlib_filefunc_def->zwrite_file = aolib_no_write_file_func;
    pzlib_filefunc_def->ztell_file = aolib_no_tell_file_func;
    pzlib_filefunc_def->zseek_file = aolib_no_seek_file_func;
    pzlib_filefunc_def->zclose_file = aolib_no_close_file_func;
    pzlib_filefunc_def->zerror_file = aolib_no_error_file_func;
    pzlib_filefunc_def->opaque = NULL;
}

void fill_fopen64_filefunc(zlib_filefunc64_def* pzlib_filefunc_def) {
    pzlib_filefunc_def->zopen64_file = aolib_no_open64_file_func;
    pzlib_filefunc_def->zread_file = aolib_no_read_file_func;
    pzlib_filefunc_def->zwrite_file = aolib_no_write_file_func;
    pzlib_filefunc_def->ztell64_file = aolib_no_tell64_file_func;
    pzlib_filefunc_def->zseek64_file = aolib_no_seek64_file_func;
    pzlib_filefunc_def->zclose_file = aolib_no_close_file_func;
    pzlib_filefunc_def->zerror_file = aolib_no_error_file_func;
    pzlib_filefunc_def->opaque = NULL;
}
