/* Stub del codec zstd de CHD. Sustituye a libchdr_codec_zstd.c de
 * upstream; el resto de libchdr esta sin tocar.
 *
 * Por que: el decodificador de zstd son 984 KB de fuente, y ninguna
 * herramienta produce hunks zstd por defecto -- hay que pedirlo a mano
 * (`chdman createcd -c zstd,...`). Frontera declarada de la v1, igual que
 * "un solo coder LZMA" lo es en el camino reanudable de .7z: lo que no se
 * soporta se rechaza con un mensaje claro, no se intenta a medias.
 *
 * Devolver el error desde _init hace que falle chd_open(), no la primera
 * lectura: el usuario se entera al cargar, no a mitad de un album.
 *
 * El codec cdzs (CD zstd) NO lleva stub: su fuente de upstream esta
 * vendorizada tal cual y no llama a ninguna funcion ZSTD_ directamente,
 * solo a estas tres, asi que hereda el rechazo.
 */

#include "codec_zstd.h"

chd_error zstd_codec_init(void *codec, uint32_t hunkbytes)
{
	zstd_codec_data *zstd_codec = (zstd_codec_data *)codec;
	(void)hunkbytes;
	zstd_codec->dstream = NULL;
	return CHDERR_UNSUPPORTED_FORMAT;
}

void zstd_codec_free(void *codec)
{
	(void)codec;
}

chd_error zstd_codec_decompress(void *codec, const uint8_t *src,
                                uint32_t complen, uint8_t *dest,
                                uint32_t destlen)
{
	(void)codec; (void)src; (void)complen; (void)dest; (void)destlen;
	return CHDERR_UNSUPPORTED_FORMAT;
}
