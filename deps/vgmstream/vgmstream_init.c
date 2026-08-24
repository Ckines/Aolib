#include "vgmstream_init.h"

//typedef VGMSTREAM* (*init_vgmstream_t)(STREAMFILE*);

/* list of metadata parser functions that will recognize files, used on init */
init_vgmstream_t init_vgmstream_functions[] = {
    init_vgmstream_xa,
    init_vgmstream_bnk_sony,
    init_vgmstream_vag,
    init_vgmstream_svag_kcet,
    init_vgmstream_npsf,
    init_vgmstream_exst,
    init_vgmstream_rxws,
    init_vgmstream_ild,
    init_vgmstream_vpk,
    init_vgmstream_vsv,
    init_vgmstream_vgs,
    init_vgmstream_mib_mih,
    init_vgmstream_vs_str,
    init_vgmstream_str_sega,
    init_vgmstream_spsd,
    init_vgmstream_naomi_adpcm,
    init_vgmstream_adx,
    init_vgmstream_ahx,
    init_vgmstream_ngc_dsp_std,
    init_vgmstream_ngc_dsp_std_le,
    init_vgmstream_brstm,
    init_vgmstream_ast,
    init_vgmstream_ydsp,
    init_vgmstream_caf,
    init_vgmstream_rstm_rockstar,
    init_vgmstream_sshd,
    init_vgmstream_ea_schl,
    init_vgmstream_halpst,
    init_vgmstream_dtk,
    init_vgmstream_cstr,
    init_vgmstream_rs03,
    init_vgmstream_nds_strm,
    init_vgmstream_ea_1snh,
    init_vgmstream_ws_aud,
    init_vgmstream_riff,
    init_vgmstream_genh,
    init_vgmstream_mtaf,
    /* Van DESPUÉS de riff: ubi_jade reclama .wav/.lwav además de sus
     * .waa/.wac/.wad/.wam, y un RIFF normal debe seguir yendo a riff.c. */
    init_vgmstream_ubi_jade,
    /* .mss de Free Radical (Second Sight, TimeSplitters) es DSP de GameCube
     * con dos cabeceras de 0x60 y datos entrelazados a 0x1000. Lo resuelve
     * init_vgmstream_dsp_interleaved, que es el despachador de las variantes
     * dspi_*: la que sirve, init_vgmstream_dspi_dsp_mss_gcm, es static y no
     * se puede poner aquí directamente. NO lo parsea mcss (que exige magia
     * "MCSS" y es el .mss de Guerrilla); las dos variantes comparten
     * extensión, así que hacen falta las dos entradas. */
    init_vgmstream_dsp_interleaved,
    init_vgmstream_mcss,
    init_vgmstream_gcub,
    init_vgmstream_fsb,
    /* ps_headerless SIEMPRE el último: no comprueba magia, adivina por
     * contenido, y delante de cualquier otro se los comería. */
    init_vgmstream_ps_headerless,
};

#define LOCAL_ARRAY_LENGTH(array) (sizeof(array) / sizeof(array[0]))
static const int init_vgmstream_count = LOCAL_ARRAY_LENGTH(init_vgmstream_functions);


VGMSTREAM* detect_vgmstream_format(STREAMFILE* sf) {
    if (!sf)
        return NULL;

    /* try a series of formats, see which works */
    for (int i = 0; i < init_vgmstream_count; i++) {
        init_vgmstream_t init_vgmstream_function = init_vgmstream_functions[i];
    
        /* call init function and see if valid VGMSTREAM was returned */
        VGMSTREAM* vgmstream = init_vgmstream_function(sf);
        if (!vgmstream)
            continue;

        vgmstream->format_id = i + 1;

        /* validate + setup vgmstream */
        if (!prepare_vgmstream(vgmstream, sf)) {
            /* keep trying if wasn't valid, as simpler formats may return a vgmstream by mistake */
            close_vgmstream(vgmstream);
            continue;
        }

        return vgmstream;
    }

    /* not supported */
    return NULL;
}

init_vgmstream_t get_vgmstream_format_init(int format_id) {
    // ID is expected to be from 1...N, to distinguish from 0 = not set
    if (format_id <= 0 || format_id > init_vgmstream_count)
        return NULL;

    return init_vgmstream_functions[format_id - 1];
}
