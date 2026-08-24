# libxmp-lite — notas de vendorizado

## Origen

- Proyecto: https://github.com/libxmp/libxmp
- Release: **libxmp 4.7.1** (4 de julio de 2026)
- Tarball: `libxmp-lite-4.7.1.tar.gz`
- SHA256: `e5dcd937a931650047a01b7c6cebbb513f3c0e2182dd61f4801181771ccbcd97`

`libxmp-lite` es la variante reducida que publica el propio proyecto en
cada release: cuatro loaders (MOD, S3M, XM, IT), sin depackers, sin
ProWizard y sin carga de instrumentos externos. No es un recorte hecho
aquí.

## Qué se copió

- `src/*.c` y `src/*.h`
- `src/loaders/*.c` y `src/loaders/*.h`
- `include/libxmp-lite/xmp.h` -> `include/xmp.h`
- `README` (contiene el texto de licencia)

## Qué se dejó fuera, y por qué

- `configure`, `Makefile.*`, `CMakeLists.txt`, `cmake/`, `m4/`, `jni/`,
  `watcom.mif`, `*.def`, `*.pc.in`: AOLIB compila sus dependencias como
  fuentes desde su propio Makefile, igual que aosdk, libgme, libvgm,
  minizip y el SDK de 7-Zip. Un segundo sistema de build rompería
  `make dist-all` y la cross-compilación MinGW de un solo comando.
- `test/`: usa el runner de libxmp. La verificación de este backend vive
  en `tests/f21_xmp_engine.cpp`, contra la interfaz `IAudioEngine`.
- `src/Makefile`, `src/loaders/Makefile`: se borraron tras copiar.

## Parches locales

**Ninguno.** Los 31 ficheros están tal cual salen del tarball. La
selección de fuentes y las defines están en el Makefile raíz, no aquí.

## Defines obligatorias

`XMP_CFLAGS := -DLIBXMP_CORE_PLAYER -DLIBXMP_STATIC`

- **`-DLIBXMP_CORE_PLAYER`**: es lo que hace que esto sea "lite". Además
  de recortar loaders, deja fuera la rama de `src/loaders/mod_load.c`
  (línea 1067 del tarball original) que abre ficheros de instrumento
  sueltos con `fopen()`. Sin ella existiría un camino de E/S que no pasa
  por el VFS de Libretro.
- **`-DLIBXMP_STATIC`**: tiene que aplicar TAMBIÉN a `src/libretro.cpp` y
  `src/engine/xmp_engine.hpp`, no solo a los `.c` de aquí. Sin ella,
  `xmp.h` declara cada símbolo `__declspec(dllimport)` en el build de
  MinGW y el enlazado del `.dll` falla con referencias `__imp_xmp_*`.
  `xmp_engine.hpp` lleva un `#error` de guardia.

## Enlazado

En Linux hace falta `-lm` explícito: `period.c` usa `pow`/`log`/`floor` y
`filter.c` usa `powf`. Ya está en `LDFLAGS` para `PLATFORM=linux`.

## Comprobaciones hechas al vendorizar

- Compila sin un solo warning con las flags completas del proyecto
  (`-std=c99 -Wall` + `AOSDK_CFLAGS` + `GME_CFLAGS` + `LIBVGM_CFLAGS`).
  En particular `-DPATH_MAX=1024` no colisiona: libxmp-lite no usa
  `PATH_MAX` en ningún fichero.
- Sin estado global mutable: no hay ni una variable de ámbito de fichero
  no-`const` en `src/`. Cada `xmp_context` es independiente, así que
  varios motores pueden coexistir (lo hace `precompute_zip_track_durations()`)
  sin nada parecido a `AosdkPsxCoreGuard`.
- Sin asignaciones en el camino caliente: los únicos `calloc` están en
  `mixer.c` y `virtual.c`, alcanzables solo desde `xmp_start_player()`.
  `xmp_play_buffer()` es `memcpy` puro.
- Limpio bajo AddressSanitizer y UBSan en apertura, render completo y
  reinicios repetidos.

## Licencia

MIT (desde libxmp 4.6.1 la biblioteca completa está bajo MIT).
Copyright (C) 1996-2026 Claudio Matsuoka y Hipolito Carraro Jr.
Texto completo en `README`, sección LICENSE.
