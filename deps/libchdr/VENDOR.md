# libchdr (vendored, decode-only)

Upstream: <https://github.com/rtissera/libchdr>, rama `master`, tomada el
26/08/2026. Licencia BSD-3-Clause (Aaron Giles y colaboradores);
`include/dr_libs/dr_flac.h` es de David Reid, dominio público / MIT-0.

CHD (Compressed Hunks of Data) es el formato de imagen de disco de MAME:
un mapa de hunks al principio del fichero, cada hunk comprimido por
separado, con CRC por hunk y SHA1 global. Esa estructura es la razón de
que esté aquí — llegar al byte N es O(1), que es justo lo que no tiene un
`.7z` de un solo bloque sólido.

## Qué se tomó

Todo `src/` y `include/` salvo lo que se lista abajo, sin tocar **ni una
línea**. La adaptación al proyecto se hizo entera desde fuera, con defines
y rutas de inclusión, aprovechando que libchdr ya trae su propia
abstracción de fichero (`core_file_callbacks` en `include/libchdr/
coretypes.h`): `src/chd_reader.hpp` le pasa cuatro funciones que van a
`IVFSBridge`, así que se cumple la regla de que ningún `fopen()` vive
fuera del puente VFS sin parchear nada.

## Las tres dependencias de upstream, una a una

Upstream trae copias de zlib (miniz), del SDK de LZMA y de zstd en su
`deps/`. Solo una se copia, y recortada:

| dependencia | qué se hace | por qué |
|---|---|---|
| zlib | `-DCHDR_SYSTEM_ZLIB=1` → `deps/aosdk/zlib` | ya está en el árbol; vendorizar miniz serían 350 KB de fuente duplicada |
| LZMA | **sí** se vendoriza, en `deps/lzma-26.02/` | son 5 ficheros, no los 66 del SDK completo; ver abajo |
| zstd | `-DCHDR_SYSTEM_ZSTD=1` → shim con el tipo opaco, más un stub del codec | 984 KB de fuente para un codec que hay que pedir a mano |

LZMA merece una nota. Es uno de los códecs de hunk de CHD (`lzma` y
`cdlz`), así que hace falta de verdad. Hasta 1.2.2 se compartía con el SDK
de 7-Zip que este proyecto vendorizaba para leer `.7z`, mediante un shim que
redirigía allí. Al retirarse el `.7z` en 1.2.3, libchdr quedó como único
consumidor y los cinco ficheros que LzmaDec necesita

    LzmaDec.c  LzmaDec.h  7zTypes.h  Precomp.h  Compiler.h

se movieron aquí, que es **exactamente** donde upstream los pone. Así el
`#include "../deps/lzma-26.02/include/LzmaDec.h"` de `libchdr_chd.c` y
`codec_lzma.h` funciona sin shim y actualizar libchdr vuelve a ser una copia
directa.

Upstream además renombra los símbolos a `CHDR_LzmaDec_*`, para builds
estáticos de libretro donde varios cores comparten binario y cada uno trae su
LZMA. Aquí no hace falta: AOLIB es una `.dll` con un solo `LzmaDec`.

OJO al añadir fuentes a `LIBCHDR_SOURCES`: si algún target de test la nombraba
ya por su cuenta, entra dos veces en la línea de enlace y da
"multiple definition". Pasó justo con `LzmaDec.c` al hacer este movimiento.

Lo de zstd es una frontera declarada, no una carencia disimulada: en la
misma línea que el resto del proyecto, lo que no se soporta se rechaza con
un mensaje que dice qué hacer.
`deps/libchdr/src/libchdr_codec_zstd.c` (nuestro, sustituye al de
upstream) devuelve `CHDERR_UNSUPPORTED_FORMAT` desde `_init`, así que
falla `chd_open()` — el usuario se entera al cargar, no a mitad de un
álbum — y el mensaje de `chd_reader::explain()` dice cómo recrear el CHD.
El codec `cdzs` (CD zstd) no lleva stub: su fuente está vendorizada tal
cual y no llama a ninguna función `ZSTD_` directamente, sólo a las tres
del stub, así que hereda el rechazo.

## Qué NO se tomó

- `deps/miniz-3.1.2/` y `deps/zstd-1.5.7/` (ver arriba). De
  `deps/lzma-26.02/` solo los cinco ficheros que LzmaDec necesita: ni el
  resto del SDK ni los shims de renombrado de upstream.
- `src/libchdr_codec_zstd.c` (sustituido por el stub).
- `CMakeLists.txt`, `link.T` y el resto del andamiaje de compilación:
  la selección de fuentes y los defines viven en el `Makefile` del
  proyecto, igual que con aosdk, libgme y libvgm.
- El ensamblador de LZMA (`Asm/`), por la regla de portabilidad: ARM es un
  objetivo declarado y todo se compila desde C.

`libchdr_codec_avhuff.c` **sí** está, aunque AVHuff sea el codec de vídeo
de laserdisc y este core no vaya a abrir uno: no tiene dependencias
externas (usa el huffman y el FLAC que ya se compilan) y dejarlo cuesta
menos que el parche que haría falta para quitarlo de la tabla de codecs de
`libchdr_chd.c`.

## Cómo se verifica

`make test-c01 PLATFORM=windows CHD=<fichero.chd>`.

El oráculo está dentro del propio formato: la cabecera v5 lleva el SHA1 de
todo el flujo lógico y el mapa de hunks un CRC por hunk (que `chd_read()`
comprueba por dentro, con `VERIFY_BLOCK_CRC`). Si el lector descomprime
los N hunks y el SHA1 cuadra, es correcto sin depender de ninguna
herramienta externa ni de una lista de hashes escrita a mano.

Con `--oraculo <fichero.bin>` se compara además contra la salida de
`chdman extractcd`, que es lo único que valida la tabla de pistas — el
desplazamiento de cada pista, el relleno de 4 marcos y el orden de bytes.
Ahí se cazó que **CHD guarda las pistas de CD-DA en big-endian** mientras
que un `.bin/.cue` y el callback de audio de libretro son little-endian;
sin el intercambio la pista suena a ruido. Vive en
`ChdReader::read_track()`, nunca en `read()`, que tiene que seguir
entregando el flujo crudo para que el SHA1 cuadre.

## Cómo actualizar

La estructura de directorios es la de upstream, así que actualizar es
copiar `src/` e `include/` encima, volver a poner el stub de zstd y
recompilar. Si upstream añade un codec nuevo a la tabla de
`libchdr_chd.c`, aparecerá como símbolo sin definir al enlazar — es la
forma en que este arreglo avisa en vez de fallar en silencio.
