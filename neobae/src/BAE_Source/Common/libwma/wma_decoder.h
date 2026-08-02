/*
 * WMA v1/v2 decoder — clean float-based port from FFmpeg
 * LGPL-2.1+
 */

#ifndef _WMA_DECODER_H
#define _WMA_DECODER_H

#include "X_API.h"
#include <stdint.h>

/* Types needed by wmadata.h */
typedef int32_t wma_fixed;
typedef int32_t fixed32;
typedef int64_t fixed64;
#define ICONST_ATTR_WMA_XL_IRAM const
#define NB_LSP_COEFS 10

/* CoefVLCTable for wmadata.h */
typedef struct {
    int n;
    const uint32_t *huffcodes;
    const uint8_t *huffbits;
    const uint16_t *levels;
} CoefVLCTable;

#define WMA_BLOCK_MIN_BITS  7
#define WMA_BLOCK_MAX_BITS  11
#define WMA_BLOCK_MAX_SIZE  (1 << WMA_BLOCK_MAX_BITS)
#define WMA_BLOCK_NB_SIZES  (WMA_BLOCK_MAX_BITS - WMA_BLOCK_MIN_BITS + 1)
#define WMA_MAX_CHANNELS    2
#define WMA_NB_LSP_COEFS    10
#define WMA_NOISE_TAB_SIZE  8192
#define WMA_LSP_POW_BITS    7
#define WMA_MAX_CODED_SF    32768
#define WMA_HIGH_BAND_MAX   16

#define WMA_VLC_BITS 9
#define WMA_VLC_SIZE (1 << WMA_VLC_BITS)

/* ---- VLC table ---- */
typedef struct {
    int16_t  table_a[WMA_VLC_SIZE];  /* (index << 6) | len */
    int16_t  table_b[WMA_VLC_SIZE];  /* actual index */
    const uint32_t *codes;           /* for fallback linear search */
    const uint8_t  *bits;            /* for fallback linear search */
    int      n;                       /* number of codes */
} WMAVLC;

/* ---- Bit reader ---- */
typedef struct {
    const uint8_t *buf;
    int            buf_bytes;
    int            bit_pos;
} WMAGetBitContext;

/* ---- Decoder context ---- */
typedef struct {
    WMAGetBitContext gb;
    int version;
    int sample_rate, nb_channels, bit_rate, block_align;
    int frame_len, frame_len_bits, nb_block_sizes;
    int use_exp_vlc, use_variable_block_len, use_noise_coding;
    int use_bit_reservoir;
    int byte_offset_bits, coefs_start;
    int skip_frames;
    int coefs_end[WMA_BLOCK_NB_SIZES];
    int exponent_sizes[WMA_BLOCK_NB_SIZES];
    uint16_t exponent_bands[WMA_BLOCK_NB_SIZES][25];
    int exponent_high_sizes[WMA_BLOCK_NB_SIZES];
    int exponent_high_bands[WMA_BLOCK_NB_SIZES][16];
    int high_band_start[WMA_BLOCK_NB_SIZES];

    /* VLCs */
    WMAVLC exp_vlc;
    WMAVLC coef_vlc[2];
    WMAVLC hgain_vlc;
    uint16_t *run_table[2];
    float    *level_table[2];

    /* Block state */
    int reset_block_lengths;
    int block_len_bits, next_block_len_bits, prev_block_len_bits;
    int block_len, block_num, block_pos;
    uint8_t ms_stereo, channel_coded[WMA_MAX_CHANNELS];
    int exponents_bsize[WMA_MAX_CHANNELS];
    float exponents[WMA_MAX_CHANNELS][WMA_BLOCK_MAX_SIZE];
    float max_exponent[WMA_MAX_CHANNELS];
    float coefs1[WMA_MAX_CHANNELS][WMA_BLOCK_MAX_SIZE];
    float coefs[WMA_MAX_CHANNELS][WMA_BLOCK_MAX_SIZE];

    /* MDCT windows + precomputed IMDCT rotators (size n/2 per block size) */
    float windows[WMA_BLOCK_NB_SIZES][WMA_BLOCK_MAX_SIZE];
    float mdct_tcos[WMA_BLOCK_NB_SIZES][WMA_BLOCK_MAX_SIZE / 2];
    float mdct_tsin[WMA_BLOCK_NB_SIZES][WMA_BLOCK_MAX_SIZE / 2];

    /* Frame output */
    float frame_out[WMA_MAX_CHANNELS][WMA_BLOCK_MAX_SIZE * 2];

    /* Superframe state */
    uint8_t last_superframe[WMA_MAX_CODED_SF + 4];
    int last_bitoffset, last_superframe_len;

    /* Noise */
    float noise_table[WMA_NOISE_TAB_SIZE];
    float noise_mult;
    int noise_index;

    /* High band */
    int high_band_coded[WMA_MAX_CHANNELS][WMA_HIGH_BAND_MAX];
    int high_band_values[WMA_MAX_CHANNELS][WMA_HIGH_BAND_MAX];

    /* LSP tables */
    float lsp_cos_table[WMA_BLOCK_MAX_SIZE];
    float lsp_pow_e_table[256];
    float lsp_pow_m_table1[1 << WMA_LSP_POW_BITS];
    float lsp_pow_m_table2[1 << WMA_LSP_POW_BITS];

    int nb_frames, current_frame;
    int eof_done;
} WMADecodeContext;

/* ---- WAVEFORMATEX info ---- */
typedef struct {
    uint16_t codec_id, channels;
    uint32_t rate, bitrate;
    uint16_t blockalign, bitspersample, datalen;
    const uint8_t *data;
} WMAFormatInfo;

/* ---- API ---- */
int  wma_decode_init(WMADecodeContext *s, const WMAFormatInfo *wfx);
int  wma_superframe_init(WMADecodeContext *s, const uint8_t *buf, int size, int bit_off);
int  wma_decode_frame(WMADecodeContext *s, float *out);
void wma_decode_close(WMADecodeContext *s);

static inline int wma_gb_bits_left(WMAGetBitContext *gb) {
    return gb->buf_bytes * 8 - gb->bit_pos;
}

#endif
