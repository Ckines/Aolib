# Nota de vendorización: win32_utf8 (F3)

`deps/aosdk` es un clon real (no un placeholder) desde F2. En F3, al
compilar para Windows (`PLATFORM=windows`), se añadió también:

    git submodule add https://github.com/thpatch/win32_utf8 deps/aosdk/win32_utf8

`ao.h` incluye incondicionalmente `"win32_utf8/src/entry.h"` bajo
`#ifdef WIN32`. No usamos su funcionalidad real (nuestro propio
`aosdk_host_glue.cpp` sustituye `ao_fopen`/`ao_mkdir`/`ao_sleep`/
`ao_song_done`), pero el `#include` es incondicional y hace falta que el
fichero exista para que `ao.h` compile en Windows.

Su macro `#define main win32_utf8_main` es inocua para la DLL de
producción (nunca define `main()`). Si en el futuro se cross-compilan los
arneses de test (`tests/f3_*.cpp`, que sí definen `main()`) para Windows,
revisar esto primero -- podría requerir compilar sin incluir `entry.h` en
esa configuración, o enlazar contra uno de los `entry_*.c` de win32_utf8.
