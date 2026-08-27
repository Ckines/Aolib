#include "meta.h"
#include "../coding/coding.h"


/* Headerless PS-ADPCM
 * Guesses interleave/channels/loops by testing data and using the file extension for sample rate.
 * This is an ugly crutch for older sets, use TXTH to properly play headerless data instead. */
VGMSTREAM * init_vgmstream_ps_headerless(STREAMFILE *streamFile) {
    VGMSTREAM * vgmstream = NULL;
    off_t start_offset = 0x00;
    char filename[PATH_LIMIT];

    uint8_t mibBuffer[0x10];
    uint8_t testBuffer[0x10];

    size_t  fileLength;
    off_t   loopStart = 0;
    off_t   loopEnd = 0;
    off_t   interleave = 0;

    off_t   readOffset = 0;

    off_t   loopStartPoints[0x10] = {0};
    int     loopStartPointsCount=0;
    off_t   loopEndPoints[0x10] = {0};
    int     loopEndPointsCount=0;
    int     loopToEnd=0;
    int     forceNoLoop=0;
    int     gotEmptyLine=0;

    /* Lectura DETERMINISTA del interleave a partir de las marcas de
     * loop/fin (ver el bloque tras el bucle). Si sale, manda sobre todo lo
     * que adivina la heuristica. */
    off_t   markerPrev = -1;
    off_t   markerGap = 0;
    int     interleaveFromFlags = 0;

    int i, channel_count=0;


    /* checks
     * .mib: common, but many ext-less files are renamed to this.
     * .mi4: fake .mib to force another sample rate */
    streamFile->get_name(streamFile,filename,sizeof(filename));
    if (strcasecmp("mib",filename_extension(filename)) && 
        strcasecmp("mi4",filename_extension(filename)))
        goto fail;

    /* test if raw PS-ADPCM */
    if (!ps_check_format(streamFile, 0x00, 0x2000))
        goto fail;


    fileLength = get_streamfile_size(streamFile);

    /* Search for interleave value (checking channel starts) and loop points (using PS-ADPCM flags).
     * Channel start will by 0x0000, 0x0002, 0x0006 followed by 12 zero values.
     * Interleave value is the offset where those repeat, and channels the number of times.
     * Loop flags in second byte are: 0x06 = start, 0x03 = end (per channel).
     * Interleave can be large (up to 0x20000 found so far) and is always a 0x10 multiple value. */
    readOffset+=(off_t)read_streamfile(mibBuffer,0,0x10,streamFile);
    mibBuffer[0]=0;
    {
        uint8_t doChannelUpdate=1;
        uint8_t bDoUpdateInterleave=1;

        readOffset=0;
        do {
            size_t got = read_streamfile(testBuffer,readOffset,0x10,streamFile);
            /* a short read leaves readOffset where it was and the loop
             * never advances; bail out instead of spinning */
            if (got == 0) goto fail;
            readOffset+=(off_t)got;
            // be sure to point to an interleave value
            if(readOffset<(int32_t)(fileLength*0.5)) {

                if(memcmp(testBuffer+2, mibBuffer+2,0x0e)) {
                    if(doChannelUpdate) {
                        doChannelUpdate=0;
                        channel_count++;
                    }
                    if(channel_count<2)
                        bDoUpdateInterleave=1;
                }

                testBuffer[0]=0;
                if(!memcmp(testBuffer,mibBuffer,0x10)) {
                    gotEmptyLine=1;

                    if(bDoUpdateInterleave) {
                        bDoUpdateInterleave=0;
                        interleave=readOffset-0x10;
                    }
                    if(interleave > 0 && readOffset-0x10 == channel_count*interleave) {
                        doChannelUpdate=1;
                    }
                }
            }

            /* Distancia MINIMA entre dos frames con marca (flag distinto de
             * 0x00 "nada" y 0x02 "normal"). El codificador escribe esas
             * marcas en la misma posicion relativa dentro del bloque de
             * CADA canal, asi que dos consecutivas caen a un bloque exacto:
             * la distancia minima ES el interleave, leida y no adivinada.
             * Va aqui dentro porque el bucle ya recorre todos los frames y
             * ya mira este byte -- no cuesta ni una lectura mas. */
            if(testBuffer[0x01]!=0x00 && testBuffer[0x01]!=0x02 &&
               !(testBuffer[0x01]==0x03 && testBuffer[0x03]==0x77)) {
                const off_t here = readOffset-0x10;
                if(markerPrev >= 0) {
                    const off_t gap = here - markerPrev;
                    if(markerGap == 0 || gap < markerGap) markerGap = gap;
                }
                markerPrev = here;
            }

            // Loop Start ...
            if(testBuffer[0x01]==0x06) {
                if(loopStartPointsCount<0x10) {
                    loopStartPoints[loopStartPointsCount] = readOffset-0x10;
                    loopStartPointsCount++;
                }
            }

            // Loop End ...
            if(testBuffer[0x01]==0x03 && testBuffer[0x03]!=0x77) {
                if(loopEndPointsCount<0x10) {
                    loopEndPoints[loopEndPointsCount] = readOffset;
                    loopEndPointsCount++;
                }
            }

            if(testBuffer[0x01]==0x04) {
                // 0x04 loop points flag can't be with a 0x03 loop points flag
                if(loopStartPointsCount<0x10) {
                    loopStartPoints[loopStartPointsCount] = readOffset-0x10;
                    loopStartPointsCount++;

                    // Loop end value is not set by flags ...
                    // go until end of file
                    loopToEnd=1;
                }
            }

        } while (readOffset<((int32_t)fileLength));
    }

    if(testBuffer[0]==0x0c && testBuffer[1]==0)
        forceNoLoop=1;

    if(channel_count==0)
        channel_count=1;

    /* EL INTERLEAVE, LEIDO DE LAS MARCAS EN VEZ DE ADIVINADO
     *
     * Todo lo que hay debajo de aqui es heuristica: busca bloques iguales al
     * primero y supone que el hueco entre dos de ellos es el limite de un
     * canal. Eso falla de dos maneras opuestas y las dos se han visto en
     * rips reales: un silencio incidental se confunde con un limite (sale
     * un interleave inventado) o no hay ningun bloque repetido y no sale
     * nada. La distancia entre marcas no adivina: las escribe el
     * codificador, una por bloque y canal, siempre en la misma posicion
     * relativa, asi que dos consecutivas estan separadas justo un bloque.
     *
     * Se exige que el candidato ademas CUADRE con el tamano del fichero:
     * multiplo de 0x10 y que parta el fichero en un numero entero y PAR de
     * bloques, que es lo que obliga la propia definicion de entrelazado a
     * dos canales. Un hueco casual no suele cuadrar; uno estructural,
     * siempre.
     *
     * Medido sobre las 147 pistas .MIB de la biblioteca, dos juegos con
     * estructuras que no se parecen en nada:
     *   OutRun 2006 (67 pistas): 0x2000 en las 67 -- el mismo valor al que
     *     llegaba el saneado de mas abajo, o sea que no cambia nada.
     *   TimeSplitters 2 (80 pistas): 52 valores DISTINTOS entre 0x61c0 y
     *     0x7480, ninguno potencia de dos, uno por pista. Aqui el saneado
     *     los tiraba y los sustituia por 0x2000, y ese corte cada 0x2000
     *     rompiendo la prediccion ADPCM es lo que se oia entrecortado.
     * Contrastado decodificando: con estos valores la correlacion entre los
     * dos canales sale 0,38-0,96 (musica estereo de verdad, los dos canales
     * comparten la senal); con cualquier otro candidato se queda en 0,0-0,17
     * (dos picadillos sin relacion). */
    if (markerGap >= 0x100 && (markerGap % 0x10) == 0 &&
        (fileLength % (size_t)markerGap) == 0 &&
        ((fileLength / (size_t)markerGap) % 2) == 0) {
        interleave = markerGap;
        channel_count = 2;
        interleaveFromFlags = 1;
    }

    // Calc Loop Points & Interleave ...
    if(loopStartPointsCount>=2) {
        // can't get more then 0x10 loop point !
        if(loopStartPointsCount<=0x0F) {
            // Always took the first 2 loop points
            /* el interleave leido de las marcas manda: esta resta es la
             * misma idea pero mirando solo el flag 0x06 y sin comprobar que
             * cuadre con el tamano del fichero */
            if(!interleaveFromFlags)
                interleave=loopStartPoints[1]-loopStartPoints[0];
            loopStart=loopStartPoints[1];

            // Can't be one channel .mib with interleave values
            if(interleave>0 && channel_count==1)
                channel_count=2;
        } else {
            loopStart=0;
        }
    }

    if(loopEndPointsCount>=2) {
        // can't get more then 0x10 loop point !
        if(loopEndPointsCount<=0x0F) {
            // No need to recalculate interleave value ...
            loopEnd=loopEndPoints[loopEndPointsCount-1];

            // Can't be one channel .mib with interleave values
            if(channel_count==1)
                channel_count=2;
        } else {
            loopToEnd=0;
            loopEnd=0;
        }
    }

    if (loopToEnd)
        loopEnd=fileLength;

    if(forceNoLoop)
        loopEnd=0;

    // Can't be one channel .mib with interleave values
    if(interleave>0x10 && channel_count==1)
        channel_count=2;

    if(interleave==0)
        interleave=0x10;

    // further check on channel_count ...
    /* con el interleave leido de las marcas el numero de canales ya sale de
     * que el fichero se parta en un numero par de bloques; este recuento de
     * bloques iguales al primero solo sabe contar los que van seguidos al
     * principio y en TimeSplitters 2 daria de mas (empiezan en silencio) */
    if(gotEmptyLine && !interleaveFromFlags) {
        int newChannelCount = 0;

        readOffset=0;

        /* count empty lines at interleave = channels */
        do {
            newChannelCount++;
            /* EOF/short read: testBuffer keeps the previous contents and
             * the comparison stays true forever. Bound by file size and by
             * a sane channel count. */
            if (readOffset + 0x10 > (off_t)fileLength) break;
            if (read_streamfile(testBuffer,readOffset,0x10,streamFile) != 0x10) break;
            readOffset+=interleave;
            if (interleave <= 0) break;
        } while(newChannelCount < 0x20 && !memcmp(testBuffer,mibBuffer,16));

        newChannelCount--;
        if(newChannelCount>channel_count)
            channel_count=newChannelCount;
    }

    /* Red de seguridad SOLO para el interleave ADIVINADO: sale de aritmetica
     * de offsets y puede caer en cualquier valor. Si no parece un bloque de
     * PS2 (potencia de dos, multiplo de 0x10) la adivinanza fallo, y seguir
     * con el destroza el audio; se cae a 0x2000, que es el tamano de bloque
     * que ps_check_format() ya da por canonico al principio de la funcion.
     *
     * NO se aplica al interleave leido de las marcas: ese no es una
     * adivinanza, ya viene comprobado contra el tamano del fichero, y
     * exigirle potencia de dos es justo lo que rompia TimeSplitters 2 --
     * sus 80 pistas traen 52 interleaves distintos entre 0x61c0 y 0x7480 y
     * ninguno lo es. Mono se deja en paz: con layout_none no se usa. */
    if (!interleaveFromFlags && channel_count > 1 &&
        (interleave < 0x800 || interleave > 0x20000 ||
         (interleave & (interleave - 1)) != 0))
        interleave = 0x2000;

    /* build the VGMSTREAM */
    vgmstream = allocate_vgmstream(channel_count,(loopEnd!=0));
    if (!vgmstream) goto fail;

    vgmstream->coding_type = coding_PSX;
    vgmstream->layout_type = (channel_count == 1) ? layout_none : layout_interleave;

    vgmstream->interleave_block_size = interleave;

    if(!strcasecmp("mib",filename_extension(filename)))
        vgmstream->sample_rate = 44100;

    if(!strcasecmp("mi4",filename_extension(filename)))
        vgmstream->sample_rate = 48000;

    vgmstream->num_samples = (int32_t)(fileLength/16/channel_count*28);

    if(loopEnd!=0) {
        if(vgmstream->channels==1) {
            vgmstream->loop_start_sample = loopStart/16*18; //todo 18 instead of 28 probably a bug
            vgmstream->loop_end_sample = loopEnd/16*28;
        } else {
            vgmstream->loop_start_sample = ((((loopStart/vgmstream->interleave_block_size)-1)*vgmstream->interleave_block_size)/16*14*channel_count)/channel_count;
            if(loopStart%vgmstream->interleave_block_size) {
                vgmstream->loop_start_sample += (((loopStart%vgmstream->interleave_block_size)-1)/16*14*channel_count);
            }

            if(loopEnd==fileLength) {
                vgmstream->loop_end_sample=(loopEnd/16*28)/channel_count;
            } else {
                vgmstream->loop_end_sample = ((((loopEnd/vgmstream->interleave_block_size)-1)*vgmstream->interleave_block_size)/16*14*channel_count)/channel_count;
                if(loopEnd%vgmstream->interleave_block_size) {
                    vgmstream->loop_end_sample += (((loopEnd%vgmstream->interleave_block_size)-1)/16*14*channel_count);
                }
            }
        }
    }

    if(loopToEnd) {
        // try to find if there's no empty line ...
        int emptySamples=0;

        for(i=0; i<16;i++) {
            mibBuffer[i]=0; //memset
        }

        readOffset=fileLength-0x10;
        do {
            /* same short-read trap as above, plus this one walks backwards
             * and could run past offset 0 */
            if (readOffset < 0) break;
            if (read_streamfile(testBuffer,readOffset,0x10,streamFile) != 0x10) break;
            if(!memcmp(mibBuffer,testBuffer,16)) {
                emptySamples+=28;
            }
            readOffset-=0x10;
        } while(!memcmp(testBuffer,mibBuffer,16));

        vgmstream->loop_end_sample-=(emptySamples*channel_count);
    }

    vgmstream->meta_type = meta_PS_HEADERLESS;
    vgmstream->allow_dual_stereo = 1;

    if (!vgmstream_open_stream(vgmstream,streamFile,start_offset))
        goto fail;
    return vgmstream;

fail:
    close_vgmstream(vgmstream);
    return NULL;
}
