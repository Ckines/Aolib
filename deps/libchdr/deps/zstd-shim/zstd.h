/* Sustituto de <zstd.h> para compilar libchdr SIN vendorizar zstd.
 *
 * codec_zstd.h solo necesita el tipo para declarar un puntero dentro de
 * zstd_codec_data; nadie desreferencia un ZSTD_DStream fuera de
 * libchdr_codec_zstd.c, que en este arbol es un stub que rechaza el
 * formato (ver ese fichero). Vendorizar zstd de verdad costaria 984 KB de
 * fuente para un codec que solo aparece en CHD creados a proposito con
 * `chdman -c zstd`, que no es el valor por defecto de ninguna herramienta.
 *
 * Se activa con -DCHDR_SYSTEM_ZSTD y -Ideps/libchdr/deps/zstd-shim, asi
 * que ni una linea de libchdr cambia.
 */

#ifndef AOLIB_ZSTD_SHIM_H
#define AOLIB_ZSTD_SHIM_H

typedef struct ZSTD_DStream_s ZSTD_DStream;

#endif /* AOLIB_ZSTD_SHIM_H */
