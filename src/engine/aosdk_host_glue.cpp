// aosdk_host_glue.cpp
//
// aosdk (ao.h) declara símbolos que espera que defina el programa
// anfitrión: `ao_song_done`, `ao_fopen`, `ao_mkdir` y `ao_sleep`. En la
// SDK original los define main.c, la app de escritorio de Audio Overload,
// que este Makefile no compila. Sin este fichero el enlace falla.
//
// Los stubs NO reenvían a fopen() real (lo que sí hace ao.c): toda la E/S
// del core pasa obligatoriamente por el VFS Bridge. Fallan de forma
// RUIDOSA si se llegan a invocar, para que un cambio futuro en aosdk no
// reintroduzca E/S directa en silencio.
//
// ao_mkdir solo lo necesita dump_files() (eng_psf2.c), que vive bajo
// `#ifdef DEBUG` y nunca se ejecuta aquí -- pero se compila igual y el
// enlazador exige el símbolo.

#include <cstdio>
#include <cstdlib>

extern "C" {
#include "ao.h"
}

extern "C" {

// Definición REAL (no solo declaración) del flag que corlett.c y
// eng_psf.c/eng_psf2.c escriben para señalar fin de pista. Los motores PSF
// lo leen desde end_of_track() y lo ponen a 0 en cada open()/select_track().
volatile ao_bool ao_song_done = 0;

FILE* ao_fopen(const char* /*fn*/, const char* /*mode*/) {
    std::fprintf(stderr,
        "[aolib] BUG: ao_fopen() invocado -- esta ruta de aosdk requeriría "
        "E/S directa de archivo, prohibida por diseño: todo debe pasar por "
        "el VFS Bridge.\n");
    // Devuelve un FILE* válido, no nullptr, a propósito: psx_hw.c tiene un
    // volcado de depuración (manejador KTTY, bajo -DDEBUG) que hace
    // `f = ao_fopen("psxram.bin","wb"); fwrite(...); fclose(f);` sin
    // comprobar NULL. Con nullptr, cualquier build con DEBUG activo
    // acabaría en segfault. tmpfile() es efímero, se borra al cerrarse y
    // existe en Linux y Windows.
    return std::tmpfile();
}

int ao_mkdir(const char* /*dirname*/) {
    std::fprintf(stderr,
        "[aolib] BUG: ao_mkdir() invocado -- solo debería ocurrir con "
        "-DDEBUG (dump_files() en eng_psf2.c), que este proyecto nunca "
        "define. Revisar el build.\n");
    return -1;
}

void ao_sleep(unsigned int /*seconds*/) {
    // No-op: no hay ningún bucle de tiempo real que dormir dentro de un
    // core Libretro (el frontend controla el ritmo vía retro_run()).
}

} // extern "C"
