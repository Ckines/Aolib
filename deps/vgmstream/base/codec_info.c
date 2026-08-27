#include "codec_info.h"

/* AOLIB: podado respecto a upstream.
 *
 * Este switch nombra cada decodificador por SIMBOLO, y el Makefile pasa los
 * .o sueltos al enlace (no como biblioteca), asi que una rama basta para
 * arrastrar el objeto entero a la .dll aunque ningun parser vendorizado
 * pueda producir nunca ese coding_type. Y en Windows no se puede confiar en
 * --gc-sections para limpiarlo: alli el Makefile desactiva -fdata-sections
 * a proposito (ver su comentario sobre el agujero de .bss en MinGW), de modo
 * que las tablas .rdata del objeto entran enteras. El caso extremo era
 * coding_AAC_raw: 188 KB de objeto anclados por tres lineas, sin un solo
 * meta capaz de pedirlo.
 *
 * Se han quitado las ramas cuyo coding_type no aparece en ningun meta/ ni
 * layout/ de los que quedan: CRI_HCA, KA1A, UBI_MPEG, TAC, COMPRESSWAVE,
 * IMUSE, MIO, BINKA, RELIC y AAC_raw.
 *
 * Los bloques bajo #ifdef (VORBIS, SPEEX, FFMPEG, ATRAC9, CELT, MPEG) se
 * dejan como estan: esas macros no se definen, asi que ya no compilan nada,
 * y conservarlos mantiene el fichero reconocible frente a upstream.
 *
 * Para volver a anadir un formato hay que reponer su meta Y su rama aqui.
 * tests/vgmstream_closure.py comprueba que meta/ y coding/ siguen cuadrando.
 */

const codec_info_t* codec_get_info(VGMSTREAM* v) {
    switch(v->coding_type) {
#ifdef VGM_USE_VORBIS
        case coding_OGG_VORBIS:
            extern const codec_info_t ogg_vorbis_decoder;
            return &ogg_vorbis_decoder;

        case coding_VORBIS_custom:
            extern const codec_info_t vorbis_custom_decoder;
            return &vorbis_custom_decoder;
#endif

#ifdef VGM_USE_SPEEX
        case coding_SPEEX:
            extern const codec_info_t speex_decoder;
            return &speex_decoder;
#endif

        case coding_PCM32LE:
            extern const codec_info_t pcm32_decoder;
            return &pcm32_decoder;

        case coding_PCM24LE:
        case coding_PCM24BE:
            extern const codec_info_t pcm24_decoder;
            return &pcm24_decoder;

        case coding_PCMFLOAT:
            extern const codec_info_t pcmfloat_decoder;
            return &pcmfloat_decoder;

#ifdef VGM_USE_FFMPEG
        case coding_FFmpeg:
            extern const codec_info_t ffmpeg_decoder;
            return &ffmpeg_decoder;
#endif
#ifdef VGM_USE_ATRAC9
        case coding_ATRAC9:
            extern const codec_info_t atrac9_decoder;
            return &atrac9_decoder;
#endif
#ifdef VGM_USE_CELT
        case coding_CELT_FSB:
            extern const codec_info_t celt_fsb_decoder;
            return &celt_fsb_decoder;
#endif

#ifdef VGM_USE_MPEG
        case coding_MPEG_custom:
        case coding_MPEG_ealayer3:
        case coding_MPEG_layer1:
        case coding_MPEG_layer2:
        case coding_MPEG_layer3:
            extern const codec_info_t mpeg_decoder;
            return &mpeg_decoder;
#endif

        case coding_CF_DF_ADPCM_V40:
            extern const codec_info_t cf_df_v40_decoder;
            return &cf_df_v40_decoder;

        case coding_CF_DF_DPCM_V41:
            extern const codec_info_t cf_df_v41_decoder;
            return &cf_df_v41_decoder;

        case coding_CF_DF_ADPCM_v5:
            extern const codec_info_t cf_df_v5_v40_decoder;
            return &cf_df_v5_v40_decoder;

        default:
            return NULL;
    }
}
