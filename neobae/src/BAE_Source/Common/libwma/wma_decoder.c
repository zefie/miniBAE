/*
 * WMA v1/v2 decoder — float port from FFmpeg
 * LGPL-2.1+
 *
 * Standalone WMA decoder using the FFmpeg algorithm and wmadata.h tables.
 */

#include <math.h>
#include <string.h>
#include "X_API.h"
#include "wma_decoder.h"
#include "wmadata.h"

#define ff_aac_scalefactor_code  scale_huffcodes
#define ff_aac_scalefactor_bits  scale_huffbits

#ifndef M_LN2
#define M_LN2 0.6931471805599453f
#endif

static float exp2f_repl(float x) {
    return expf(x * M_LN2);
}

#ifndef exp2f
#define exp2f exp2f_repl
#endif

#define WMA_PI 3.14159265358979323846f

/* ---- Bit reader ---- */
static inline void wma_gb_init(WMAGetBitContext *gb, const uint8_t *buf, int bytes) {
    gb->buf = buf; gb->buf_bytes = bytes; gb->bit_pos = 0;
}
static inline unsigned int wma_get_bits(WMAGetBitContext *gb, int n) {
    unsigned int v = 0; int i;
    for (i = 0; i < n; i++) {
        int byte = gb->bit_pos >> 3, bit = 7 - (gb->bit_pos & 7);
        if (byte >= gb->buf_bytes) {
            gb->bit_pos++;
            continue;
        }
        v = (v << 1) | ((gb->buf[byte] >> bit) & 1);
        gb->bit_pos++;
    }
    return v;
}
static inline unsigned int wma_get_bits1(WMAGetBitContext *gb) {
    return wma_get_bits(gb, 1);
}
static inline void wma_skip_bits(WMAGetBitContext *gb, int n) { gb->bit_pos += n; }
static inline unsigned int wma_show_bits(WMAGetBitContext *gb, int n) {
    int saved = gb->bit_pos;
    unsigned int v = wma_get_bits(gb, n);
    gb->bit_pos = saved;
    return v;
}
static inline void wma_align_bits(WMAGetBitContext *gb) {
    if (gb->bit_pos & 7) gb->bit_pos = (gb->bit_pos + 7) & ~7;
}

/* ---- VLC ---- */
static void wma_init_vlc(WMAVLC *vlc, const uint32_t *codes, const uint8_t *bits, int n) {
    int i, k;
    for (i = 0; i < WMA_VLC_SIZE; i++) { vlc->table_a[i] = -1; vlc->table_b[i] = 0; }
    vlc->codes = codes;
    vlc->bits  = bits;
    vlc->n     = n;
    for (i = 0; i < n; i++) {
        int len = bits[i];
        if (len == 0 || len > WMA_VLC_BITS) continue;
        uint32_t prefix = codes[i] << (WMA_VLC_BITS - len);
        int step = 1 << (WMA_VLC_BITS - len);
        for (k = 0; k < step; k++) {
            int idx = (int)(prefix + k);
            if (idx >= 0 && idx < WMA_VLC_SIZE) {
                vlc->table_a[idx] = (int16_t)(((i & 0x3FF) << 6) | len);
                vlc->table_b[idx] = (int16_t)i;
            }
        }
    }
}

static int wma_get_vlc(WMAGetBitContext *gb, WMAVLC *vlc) {
    int peak = (int)wma_show_bits(gb, WMA_VLC_BITS);
    int16_t e = vlc->table_a[peak];
    if (e >= 0) {
        int len = e & 0x3F;
        if (len == 0) return 0;
        wma_skip_bits(gb, len);
        return vlc->table_b[peak];
    }
    {
        int j;
        for (j = 0; j < vlc->n; j++) {
            int cb = vlc->bits[j];
            if (cb == 0) continue;
            if (wma_gb_bits_left(gb) < cb) continue;
            int prefix = (int)wma_show_bits(gb, cb);
            if (prefix == (int)vlc->codes[j]) {
                wma_skip_bits(gb, cb);
                return j;
            }
        }
        wma_skip_bits(gb, 1);
        return 0;
    }
}

static void wma_init_coef_vlc(WMAVLC *vlc, uint16_t **prun, float **plevel,
                               const CoefVLCTable *ct)
{
    int n = ct->n, i, j, l, level;
    const uint16_t *p;
    uint16_t *run_table;
    float *level_table;

    wma_init_vlc(vlc, ct->huffcodes, ct->huffbits, n);
    run_table  = *prun;
    level_table = *plevel;

    p = ct->levels;
    i = 2;
    level = 1;
    while (i < n) {
        l = *p++;
        for (j = 0; j < l; j++) {
            run_table[i] = (uint16_t)j;
            level_table[i] = (float)level;
            i++;
        }
        level++;
    }
}

/* ---- LSP-to-curve ---- */
static void wma_lsp_to_curve_init(WMADecodeContext *s) {
    int i, e, m;
    float wdel = (float)WMA_PI / s->frame_len;
    float a, b;
    for (i = 0; i < s->frame_len; i++)
        s->lsp_cos_table[i] = 2.0f * cosf(wdel * i);
    for (i = 0; i < 256; i++) {
        e = i - 126;
        s->lsp_pow_e_table[i] = exp2f(e * -0.25f);
    }
    b = 1.0f;
    for (i = (1 << WMA_LSP_POW_BITS) - 1; i >= 0; i--) {
        m = (1 << WMA_LSP_POW_BITS) + i;
        a = (float)m * (0.5f / (1 << WMA_LSP_POW_BITS));
        a = 1.0f / sqrtf(sqrtf(a));
        s->lsp_pow_m_table1[i] = 2.0f * a - b;
        s->lsp_pow_m_table2[i] = b - a;
        b = a;
    }
}

static float wma_pow_m1_4(WMADecodeContext *s, float x) {
    union { float f; unsigned int v; } u, t;
    unsigned int e, m;
    float a, b;
    u.f = x;
    e = u.v >> 23;
    m = (u.v >> (23 - WMA_LSP_POW_BITS)) & ((1 << WMA_LSP_POW_BITS) - 1);
    t.v = ((u.v << WMA_LSP_POW_BITS) & ((1 << 23) - 1)) | (127 << 23);
    a = s->lsp_pow_m_table1[m];
    b = s->lsp_pow_m_table2[m];
    return s->lsp_pow_e_table[e] * (a + b * t.f);
}

static void wma_lsp_to_curve(WMADecodeContext *s, float *out, float *val_max,
                              int n, float *lsp) {
    int i, j;
    float p, q, w, v, vm = 0;
    for (i = 0; i < n; i++) {
        p = 0.5f; q = 0.5f;
        w = s->lsp_cos_table[i];
        for (j = 1; j < WMA_NB_LSP_COEFS; j += 2) {
            q *= w - lsp[j - 1];
            p *= w - lsp[j];
        }
        p *= p * (2.0f - w);
        q *= q * (2.0f + w);
        v = p + q;
        v = wma_pow_m1_4(s, v);
        if (v > vm) vm = v;
        out[i] = v;
    }
    *val_max = vm;
}

static float wma_pow10_tab[156];
static int wma_pow10_init_done = 0;
static void init_pow10_table(void) {
    if (wma_pow10_init_done) return;
    int i;
    for (i = -60; i <= 95; i++)
        wma_pow10_tab[i + 60] = powf(10.0f, (float)i / 16.0f);
    wma_pow10_init_done = 1;
}

static void wma_decode_exp_vlc(WMADecodeContext *s, int ch) {
    const uint16_t *bands = s->exponent_bands[s->frame_len_bits - s->block_len_bits];
    int last, pos = 0;
    float max_scale = 0.0f;

    if (s->version == 1) {
        int count;
        last = (int)wma_get_bits(&s->gb, 5) + 10;
        if (last < -60 || last > 95) return;
        count = *bands++;
        while (count-- > 0 && pos < s->block_len)
            s->exponents[ch][pos++] = wma_pow10_tab[last + 60];
        max_scale = wma_pow10_tab[last + 60];
    } else {
        last = 36;
    }

    /* v1 already consumed the first band above; remaining bands until block_len. */
    while (pos < s->block_len) {
        int code = wma_get_vlc(&s->gb, &s->exp_vlc);
        int count;
        float scale;
        last += code - 60;
        if (last < -60 || last > 95) return;
        scale = wma_pow10_tab[last + 60];
        if (scale > max_scale) max_scale = scale;
        count = *bands++;
        while (count-- > 0 && pos < s->block_len)
            s->exponents[ch][pos++] = scale;
    }
    s->max_exponent[ch] = max_scale;
}

static void wma_decode_exp_lsp(WMADecodeContext *s, int ch) {
    float lsp_coefs[WMA_NB_LSP_COEFS];
    int i, val;
    for (i = 0; i < WMA_NB_LSP_COEFS; i++) {
        val = wma_get_bits(&s->gb, (i == 0 || i >= 8) ? 3 : 4);
        lsp_coefs[i] = (float)lsp_codebook[i][val] / 65536.0f;
    }
    wma_lsp_to_curve(s, s->exponents[ch], &s->max_exponent[ch],
                     s->block_len, lsp_coefs);
}

static int wma_run_level_decode(WMADecodeContext *s, WMAVLC *vlc,
                                 const float *level_table,
                                 const uint16_t *run_table,
                                 float *ptr, int num_coefs,
                                 int block_len, int coef_nb_bits)
{
    int offset;
    unsigned int coef_mask = (unsigned int)(block_len - 1);

    for (offset = 0; offset < num_coefs; offset++) {
        int code = wma_get_vlc(&s->gb, vlc);
        if (code > 1) {
            offset += run_table[code];
            ptr[offset & coef_mask] = wma_get_bits1(&s->gb)
                                    ? level_table[code]
                                    : -level_table[code];
        } else if (code == 1) {
            break;
        } else {
            int level = (int)wma_get_bits(&s->gb, coef_nb_bits);
            offset += (int)wma_get_bits(&s->gb, s->frame_len_bits);
            if (offset >= num_coefs) break;
            ptr[offset & coef_mask] = wma_get_bits1(&s->gb)
                                    ? (float)level : (float)-level;
        }
    }
    return 0;
}

/* ---- Inverse complex FFT (radix-2 DIT) for IMDCT ---- */
typedef struct { float re, im; } WMAComplex;

static void wma_ifft(WMAComplex *a, int n)
{
    int i, j, k, m;

    j = 0;
    for (i = 0; i < n; i++) {
        if (i < j) {
            WMAComplex t = a[i];
            a[i] = a[j];
            a[j] = t;
        }
        m = n >> 1;
        while (m && j >= m) {
            j -= m;
            m >>= 1;
        }
        j += m;
    }

    for (m = 2; m <= n; m <<= 1) {
        float ang = 2.0f * (float)WMA_PI / (float)m; /* inverse FFT */
        float wr0 = cosf(ang), wi0 = sinf(ang);
        for (k = 0; k < n; k += m) {
            float ur = 1.0f, ui = 0.0f;
            for (j = 0; j < (m >> 1); j++) {
                int i0 = k + j, i1 = i0 + (m >> 1);
                float tr = ur * a[i1].re - ui * a[i1].im;
                float ti = ur * a[i1].im + ui * a[i1].re;
                a[i1].re = a[i0].re - tr;
                a[i1].im = a[i0].im - ti;
                a[i0].re += tr;
                a[i0].im += ti;
                {
                    float nur = ur * wr0 - ui * wi0;
                    ui = ur * wi0 + ui * wr0;
                    ur = nur;
                }
            }
        }
    }
}

/*
 * Full IMDCT: N coefficients -> 2N samples.
 * FFmpeg ff_imdct_half + ff_imdct_calc (AV_TX_FULL_IMDCT, scale 1/32768).
 * bsize selects precomputed rotators for this block length.
 */
static void wma_imdct(WMADecodeContext *s, int n, int bsize, float *in, float *out)
{
    int n_full = n << 1;
    int n2 = n;          /* == n_full/2 */
    int n4 = n >> 1;     /* == n_full/4 */
    int n8 = n >> 2;     /* == n_full/8 */
    int k;
    const float *tcos;
    const float *tsin;
    /* n_full=2N, FFT size n4=N/2; max N=2048 => n4=1024. */
    WMAComplex z[WMA_BLOCK_MAX_SIZE / 2];
    float half[WMA_BLOCK_MAX_SIZE];

    if (n4 < 1 || n4 > (WMA_BLOCK_MAX_SIZE / 2) ||
        bsize < 0 || bsize >= WMA_BLOCK_NB_SIZES) {
        memset(out, 0, sizeof(float) * (size_t)n_full);
        return;
    }

    tcos = s->mdct_tcos[bsize];
    tsin = s->mdct_tsin[bsize];

    for (k = 0; k < n4; k++) {
        float a = in[n2 - 1 - 2 * k];
        float b = in[2 * k];
        z[k].re = a * tcos[k] - b * tsin[k];
        z[k].im = a * tsin[k] + b * tcos[k];
    }
    wma_ifft(z, n4);

    for (k = 0; k < n8; k++) {
        float r0, i0, r1, i1;
        WMAComplex z0 = z[n8 - k - 1];
        WMAComplex z1 = z[n8 + k];
        r0 = z0.im * tsin[n8 - k - 1] - z0.re * tcos[n8 - k - 1];
        i1 = z0.im * tcos[n8 - k - 1] + z0.re * tsin[n8 - k - 1];
        r1 = z1.im * tsin[n8 + k] - z1.re * tcos[n8 + k];
        i0 = z1.im * tcos[n8 + k] + z1.re * tsin[n8 + k];
        z[n8 - k - 1].re = r0;
        z[n8 - k - 1].im = i0;
        z[n8 + k].re = r1;
        z[n8 + k].im = i1;
    }

    for (k = 0; k < n4; k++) {
        half[2 * k] = z[k].re;
        half[2 * k + 1] = z[k].im;
    }

    memset(out, 0, sizeof(float) * (size_t)n_full);
    memcpy(out + n4, half, sizeof(float) * (size_t)n2);
    for (k = 0; k < n4; k++) {
        out[k] = -out[n2 - k - 1];
        out[n_full - k - 1] = out[n2 + k];
    }
}

static int wma_total_gain_to_bits(int total_gain)
{
    if (total_gain < 15) return 13;
    if (total_gain < 32) return 12;
    if (total_gain < 40) return 11;
    if (total_gain < 45) return 10;
    return 9;
}

/* @return 0 = more blocks in frame, 1 = frame complete, -1 = fatal error */
static int wma_decode_block(WMADecodeContext *s) {
    int ch, bsize, n, any_coded = 0;
    int total_gain = 1;
    int coef_nb_bits;
    int nb_coefs[WMA_MAX_CHANNELS];

    if (s->use_variable_block_len) {
        int v;
        /* Match FFmpeg: n = av_log2(nb_block_sizes - 1) + 1 */
        n = 0;
        {
            int t = s->nb_block_sizes - 1;
            if (t > 0) {
                while (t > 1) { n++; t >>= 1; }
                n++;
            }
        }
        if (s->reset_block_lengths) {
            s->reset_block_lengths = 0;
            v = (int)wma_get_bits(&s->gb, n);
            if (v < 0 || v >= s->nb_block_sizes) return -1;
            s->prev_block_len_bits = s->frame_len_bits - v;
            v = (int)wma_get_bits(&s->gb, n);
            if (v < 0 || v >= s->nb_block_sizes) return -1;
            s->block_len_bits = s->frame_len_bits - v;
        } else {
            s->prev_block_len_bits = s->block_len_bits;
            s->block_len_bits      = s->next_block_len_bits;
        }
        v = (int)wma_get_bits(&s->gb, n);
        if (v < 0 || v >= s->nb_block_sizes) return -1;
        s->next_block_len_bits = s->frame_len_bits - v;
    } else {
        s->prev_block_len_bits = s->frame_len_bits;
        s->block_len_bits      = s->frame_len_bits;
        s->next_block_len_bits = s->frame_len_bits;
    }
    if (s->block_len_bits < WMA_BLOCK_MIN_BITS ||
        s->block_len_bits > s->frame_len_bits ||
        (s->frame_len_bits - s->block_len_bits) >= s->nb_block_sizes)
        return -1;
    s->block_len = 1 << s->block_len_bits;
    bsize = s->frame_len_bits - s->block_len_bits;

    memset(s->channel_coded, 0, sizeof(s->channel_coded));
    if (s->nb_channels == 2)
        s->ms_stereo = (uint8_t)(wma_get_bits1(&s->gb) ? 1 : 0);
    for (ch = 0; ch < s->nb_channels; ch++)
        any_coded |= s->channel_coded[ch] = (uint8_t)(wma_get_bits1(&s->gb) ? 1 : 0);

    if (!any_coded)
        goto transform;

    do {
        int gain_part = (int)wma_get_bits(&s->gb, 7);
        total_gain += gain_part;
        if (gain_part != 127) break;
    } while (wma_gb_bits_left(&s->gb) >= 7);
    coef_nb_bits = wma_total_gain_to_bits(total_gain);

    n = s->coefs_end[bsize] - s->coefs_start;
    for (ch = 0; ch < s->nb_channels; ch++)
        nb_coefs[ch] = n;

    /* high-band noise flags / values */
    if (s->use_noise_coding) {
        for (ch = 0; ch < s->nb_channels; ch++) {
            if (s->channel_coded[ch]) {
                int i, hn = s->exponent_high_sizes[bsize];
                for (i = 0; i < hn; i++) {
                    int a = (int)wma_get_bits1(&s->gb);
                    s->high_band_coded[ch][i] = a;
                    if (a)
                        nb_coefs[ch] -= s->exponent_high_bands[bsize][i];
                }
            }
        }
        for (ch = 0; ch < s->nb_channels; ch++) {
            if (s->channel_coded[ch]) {
                int i, hn = s->exponent_high_sizes[bsize];
                int val = (int)0x80000000;
                for (i = 0; i < hn; i++) {
                    if (s->high_band_coded[ch][i]) {
                        if (val == (int)0x80000000)
                            val = (int)wma_get_bits(&s->gb, 7) - 19;
                        else
                            /* FFmpeg hgain VLC symbols are table index - 18 */
                            val += wma_get_vlc(&s->gb, &s->hgain_vlc) - 18;
                        s->high_band_values[ch][i] = val;
                    }
                }
            }
        }
    }

    /* exponents */
    if ((s->block_len_bits == s->frame_len_bits) || wma_get_bits1(&s->gb)) {
        for (ch = 0; ch < s->nb_channels; ch++) {
            if (s->channel_coded[ch]) {
                if (s->use_exp_vlc)
                    wma_decode_exp_vlc(s, ch);
                else
                    wma_decode_exp_lsp(s, ch);
                s->exponents_bsize[ch] = bsize;
            }
        }
    }

    for (ch = 0; ch < s->nb_channels; ch++) {
        float *coef_ptr = s->coefs1[ch];
        int vlc_table;
        memset(coef_ptr, 0, sizeof(float) * (size_t)s->block_len);
        if (s->channel_coded[ch]) {
            vlc_table = (ch == 1 && s->ms_stereo) ? 1 : 0;
            wma_run_level_decode(s, &s->coef_vlc[vlc_table],
                                  s->level_table[vlc_table],
                                  s->run_table[vlc_table],
                                  coef_ptr, nb_coefs[ch], s->block_len,
                                  coef_nb_bits);
        }
        if (s->version == 1 && s->nb_channels >= 2)
            wma_align_bits(&s->gb);
    }

    /* dequantize */
    {
        int n4 = s->block_len / 2;
        float mdct_norm = 1.0f / (float)n4;
        if (s->version == 1)
            mdct_norm *= sqrtf((float)n4);

        for (ch = 0; ch < s->nb_channels; ch++) {
            float *coefs1, *coefs, *exponents, mult, mult1, noise;
            int i, j, n1, last_high_band, esize;
            float exp_power[WMA_HIGH_BAND_MAX];

            if (!s->channel_coded[ch]) {
                memset(s->coefs[ch], 0, (size_t)s->block_len * sizeof(float));
                continue;
            }

            coefs1 = s->coefs1[ch];
            exponents = s->exponents[ch];
            esize = s->exponents_bsize[ch];
            mult = powf(10.0f, total_gain * 0.05f) /
                   (s->max_exponent[ch] > 0.0f ? s->max_exponent[ch] : 1.0f);
            mult *= mdct_norm;
            coefs = s->coefs[ch];

            if (s->use_noise_coding) {
                mult1 = mult;
                for (i = 0; i < s->coefs_start; i++) {
                    *coefs++ = s->noise_table[s->noise_index] *
                               exponents[i << bsize >> esize] * mult1;
                    s->noise_index = (s->noise_index + 1) & (WMA_NOISE_TAB_SIZE - 1);
                }

                n1 = s->exponent_high_sizes[bsize];
                exponents = s->exponents[ch] + (s->high_band_start[bsize] << bsize >> esize);
                last_high_band = 0;
                for (j = 0; j < n1; j++) {
                    int hn = s->exponent_high_bands[bsize][j];
                    if (s->high_band_coded[ch][j]) {
                        float e2 = 0, v;
                        for (i = 0; i < hn; i++) {
                            v = exponents[i << bsize >> esize];
                            e2 += v * v;
                        }
                        exp_power[j] = e2 / (float)hn;
                        last_high_band = j;
                    }
                    exponents += hn << bsize >> esize;
                }

                exponents = s->exponents[ch] + (s->coefs_start << bsize >> esize);
                for (j = -1; j < n1; j++) {
                    int hn;
                    if (j < 0)
                        hn = s->high_band_start[bsize] - s->coefs_start;
                    else
                        hn = s->exponent_high_bands[bsize][j];
                    if (j >= 0 && s->high_band_coded[ch][j]) {
                        mult1 = sqrtf(exp_power[j] / exp_power[last_high_band]);
                        mult1 *= powf(10.0f, s->high_band_values[ch][j] * 0.05f);
                        mult1 /= (s->max_exponent[ch] * s->noise_mult);
                        mult1 *= mdct_norm;
                        for (i = 0; i < hn; i++) {
                            noise = s->noise_table[s->noise_index];
                            s->noise_index = (s->noise_index + 1) & (WMA_NOISE_TAB_SIZE - 1);
                            *coefs++ = noise * exponents[i << bsize >> esize] * mult1;
                        }
                        exponents += hn << bsize >> esize;
                    } else {
                        for (i = 0; i < hn; i++) {
                            noise = s->noise_table[s->noise_index];
                            s->noise_index = (s->noise_index + 1) & (WMA_NOISE_TAB_SIZE - 1);
                            *coefs++ = ((*coefs1++) + noise) *
                                       exponents[i << bsize >> esize] * mult;
                        }
                        exponents += hn << bsize >> esize;
                    }
                }

                n = s->block_len - s->coefs_end[bsize];
                mult1 = mult * exponents[(-(1 << bsize)) >> esize];
                for (i = 0; i < n; i++) {
                    *coefs++ = s->noise_table[s->noise_index] * mult1;
                    s->noise_index = (s->noise_index + 1) & (WMA_NOISE_TAB_SIZE - 1);
                }
            } else {
                for (i = 0; i < s->coefs_start; i++)
                    *coefs++ = 0.0f;
                n = nb_coefs[ch];
                for (i = 0; i < n; i++)
                    *coefs++ = coefs1[i] * exponents[i << bsize >> esize] * mult;
                n = s->block_len - s->coefs_end[bsize];
                for (i = 0; i < n; i++)
                    *coefs++ = 0.0f;
            }
        }
    }

    if (s->ms_stereo && s->channel_coded[1]) {
        int i;
        if (!s->channel_coded[0]) {
            memset(s->coefs[0], 0, (size_t)s->block_len * sizeof(float));
            s->channel_coded[0] = 1;
        }
        for (i = 0; i < s->block_len; i++) {
            float mid = s->coefs[0][i];
            float side = s->coefs[1][i];
            s->coefs[0][i] = mid + side;
            s->coefs[1][i] = mid - side;
        }
    }

transform:
    /* Shared IMDCT buffer like FFmpeg's s->output: when ms-stereo has only
     * channel 0 coded, channel 1 reuses the same time-domain data (mono→L=R). */
    {
    float tmp[WMA_BLOCK_MAX_SIZE * 2];
    memset(tmp, 0, sizeof(tmp));
    for (ch = 0; ch < s->nb_channels; ch++) {
        int j, blk = s->block_len;
        float *window = s->windows[bsize];
        /* For fixed block lengths this lands at frame_out[0]; for short
           blocks it matches FFmpeg's (frame_len/2)+block_pos-(block_len/2). */
        int index = (s->frame_len / 2) + s->block_pos - (blk / 2);
        float *out;

        if (index < 0 || index + blk > s->frame_len * 2)
            return -1;
        out = s->frame_out[ch] + index;

        if (s->channel_coded[ch])
            wma_imdct(s, blk, bsize, s->coefs[ch], tmp);
        else if (!(s->ms_stereo && ch == 1))
            memset(tmp, 0, sizeof(float) * (size_t)blk * 2);
        /* else: ms_stereo && ch==1 && !coded → keep ch0's IMDCT in tmp */

        /* left: add; right: overwrite (FFmpeg wma_window fixed-block case) */
        if (s->block_len_bits <= s->prev_block_len_bits) {
            for (j = 0; j < blk; j++)
                out[j] += tmp[j] * window[j];
        } else {
            int prev = 1 << s->prev_block_len_bits;
            int mid = (blk - prev) / 2;
            int pbsize = s->frame_len_bits - s->prev_block_len_bits;
            float *wprev;
            if (pbsize < 0 || pbsize >= s->nb_block_sizes || mid < 0)
                return -1;
            wprev = s->windows[pbsize];
            for (j = 0; j < prev; j++)
                out[mid + j] += tmp[mid + j] * wprev[j];
            for (j = 0; j < mid; j++)
                out[mid + prev + j] = tmp[mid + prev + j];
        }

        out += blk;
        if (s->block_len_bits <= s->next_block_len_bits) {
            for (j = 0; j < blk; j++)
                out[j] = tmp[blk + j] * window[blk - 1 - j];
        } else {
            int next = 1 << s->next_block_len_bits;
            int mid = (blk - next) / 2;
            int nbsize = s->frame_len_bits - s->next_block_len_bits;
            float *wnext;
            if (nbsize < 0 || nbsize >= s->nb_block_sizes || mid < 0)
                return -1;
            wnext = s->windows[nbsize];
            for (j = 0; j < mid; j++)
                out[j] = tmp[blk + j];
            for (j = 0; j < next; j++)
                out[mid + j] = tmp[blk + mid + j] * wnext[next - 1 - j];
            for (j = 0; j < mid; j++)
                out[mid + next + j] = 0.0f;
        }
    }
    }

    s->block_num++;
    s->block_pos += s->block_len;
    if (s->block_pos > s->frame_len)
        return -1;
    return s->block_pos >= s->frame_len;
}

/* Decode one MDCT frame into interleaved float PCM at out. */
static int wma_decode_frame(WMADecodeContext *s, float *out) {
    int ch, i, nc, ret;

    s->block_num = 0;
    s->block_pos = 0;
    for (;;) {
        ret = wma_decode_block(s);
        if (ret < 0) return -1;
        if (ret > 0) break;
    }

    nc = s->nb_channels;
    for (i = 0; i < s->frame_len; i++)
        for (ch = 0; ch < nc; ch++)
            out[i * nc + ch] = s->frame_out[ch][i];

    for (ch = 0; ch < nc; ch++) {
        memmove(s->frame_out[ch], s->frame_out[ch] + s->frame_len,
                (size_t)s->frame_len * sizeof(float));
        memset(s->frame_out[ch] + s->frame_len, 0,
               (size_t)s->frame_len * sizeof(float));
    }

    return s->frame_len;
}

int wma_decode_superframe(WMADecodeContext *s, const uint8_t *buf, int size,
                          float *out, int out_cap_samples_per_ch)
{
    int nb_frames, bit_offset, pos, len, ret, i;
    int samples_written = 0;
    int nc;
    uint8_t *q;

    if (!s || !buf || size <= 0 || !out || out_cap_samples_per_ch <= 0)
        return -1;

    if (s->block_align > 0 && size > s->block_align)
        size = s->block_align;

    nc = s->nb_channels;
    wma_gb_init(&s->gb, buf, size);

    if (s->use_bit_reservoir) {
        /* superframe header: 4-bit index, 4-bit frame count */
        wma_skip_bits(&s->gb, 4);
        nb_frames = (int)wma_get_bits(&s->gb, 4) - (s->last_superframe_len <= 0 ? 1 : 0);
        if (nb_frames < 0)
            goto fail;
        if (nb_frames == 0) {
            /* Rare: stash packet body into reservoir, emit no PCM. */
            if (s->last_superframe_len + size - 1 > WMA_MAX_CODED_SF)
                goto fail;
            q = s->last_superframe + s->last_superframe_len;
            len = size - 1;
            while (len > 0) {
                *q++ = (uint8_t)wma_get_bits(&s->gb, 8);
                len--;
            }
            s->last_superframe_len += size - 1;
            return 0;
        }

        bit_offset = (int)wma_get_bits(&s->gb, s->byte_offset_bits + 3);
        if (bit_offset < 0 || bit_offset > wma_gb_bits_left(&s->gb))
            goto fail;

        if (s->last_superframe_len > 0) {
            int prior_len = s->last_superframe_len;

            /* Append bit_offset bits after the saved reservoir bytes.
             * Do not bump last_superframe_len; bit length is prior_len*8+bit_offset. */
            if (prior_len + ((bit_offset + 7) >> 3) > WMA_MAX_CODED_SF)
                goto fail;
            q = s->last_superframe + prior_len;
            len = bit_offset;
            while (len > 7) {
                *q++ = (uint8_t)wma_get_bits(&s->gb, 8);
                len -= 8;
            }
            if (len > 0)
                *q++ = (uint8_t)(wma_get_bits(&s->gb, len) << (8 - len));

            /* Readable size covers the appended partial byte. */
            wma_gb_init(&s->gb, s->last_superframe,
                        prior_len + ((bit_offset + 7) >> 3));
            if (s->last_bitoffset > 0)
                wma_skip_bits(&s->gb, s->last_bitoffset);
            {
                int total_bits = prior_len * 8 + bit_offset;
                int bytes_needed = (total_bits + 7) >> 3;
                s->gb.buf_bytes = bytes_needed;
                if (s->gb.bit_pos > total_bits)
                    goto fail;
            }

            if (samples_written + s->frame_len > out_cap_samples_per_ch)
                goto fail;
            ret = wma_decode_frame(s, out + samples_written * nc);
            if (ret < 0) goto fail;
            samples_written += ret;
            nb_frames--;
        }

        /* Decode frames that begin in this packet at bit_offset. */
        pos = bit_offset + 4 + 4 + s->byte_offset_bits + 3;
        if (pos > size * 8)
            goto fail;
        wma_gb_init(&s->gb, buf + (pos >> 3), size - (pos >> 3));
        if (pos & 7)
            wma_skip_bits(&s->gb, pos & 7);

        s->reset_block_lengths = 1;
        for (i = 0; i < nb_frames; i++) {
            if (samples_written + s->frame_len > out_cap_samples_per_ch)
                goto fail;
            ret = wma_decode_frame(s, out + samples_written * nc);
            if (ret < 0) goto fail;
            samples_written += ret;
        }

        /* Save unused tail of this packet into the bit reservoir. */
        pos = s->gb.bit_pos + ((bit_offset + 4 + 4 + s->byte_offset_bits + 3) & ~7);
        s->last_bitoffset = pos & 7;
        pos >>= 3;
        len = size - pos;
        if (len < 0 || len > WMA_MAX_CODED_SF)
            goto fail;
        s->last_superframe_len = len;
        if (len > 0)
            memcpy(s->last_superframe, buf + pos, (size_t)len);
    } else {
        s->reset_block_lengths = 1;
        if (s->frame_len > out_cap_samples_per_ch)
            goto fail;
        ret = wma_decode_frame(s, out);
        if (ret < 0) goto fail;
        samples_written = ret;
    }

    return samples_written;

fail:
    s->last_superframe_len = 0;
    s->last_bitoffset = 0;
    return -1;
}

int wma_decode_init(WMADecodeContext *s, const WMAFormatInfo *wfx) {
    int i, flags2, sample_rate, channels;
    float bps, bps1;
    int sample_rate1;
    int high_freq;

    memset(s, 0, sizeof(*s));

    s->version = (wfx->codec_id == 0x160 || wfx->codec_id == 0x0160) ? 1 : 2;
    s->sample_rate = (int)wfx->rate;
    s->nb_channels = (int)wfx->channels;
    /* wfx->bitrate is WAVEFORMATEX nAvgBytesPerSec */
    s->bit_rate = (int)wfx->bitrate * 8;
    s->block_align = (int)wfx->blockalign;

    channels = s->nb_channels;
    if (channels < 1 || channels > WMA_MAX_CHANNELS) return -1;
    if (s->sample_rate <= 0 || s->bit_rate <= 0 || s->block_align <= 0) return -1;

    flags2 = 0;
    if (s->version == 1 && wfx->datalen >= 4)
        flags2 = (int)(wfx->data[2] | (wfx->data[3] << 8));
    else if (s->version == 2 && wfx->datalen >= 6)
        flags2 = (int)(wfx->data[4] | (wfx->data[5] << 8));

    s->use_exp_vlc = (flags2 & 0x0001) ? 1 : 0;
    s->use_bit_reservoir = (flags2 & 0x0002) ? 1 : 0;
    s->use_variable_block_len = (flags2 & 0x0004) ? 1 : 0;
    s->use_noise_coding = 1;

    for (i = 0; i < WMA_MAX_CHANNELS; i++) s->max_exponent[i] = 1.0f;

    sample_rate = s->sample_rate;
    if (sample_rate <= 16000)
        s->frame_len_bits = 9;
    else if (sample_rate <= 22050 || (sample_rate <= 32000 && s->version == 1))
        s->frame_len_bits = 10;
    else
        s->frame_len_bits = 11;
    s->frame_len = 1 << s->frame_len_bits;

    if (s->use_variable_block_len) {
        int nb = ((flags2 >> 3) & 3) + 1;
        if ((s->bit_rate / channels) >= 32000) nb += 2;
        if (nb > s->frame_len_bits - WMA_BLOCK_MIN_BITS)
            nb = s->frame_len_bits - WMA_BLOCK_MIN_BITS;
        s->nb_block_sizes = nb + 1;
    } else {
        s->nb_block_sizes = 1;
    }

    bps = (float)s->bit_rate / (float)(channels * s->sample_rate);
    s->byte_offset_bits = 0;
    {
        int tmp = (int)(bps * (float)s->frame_len / 8.0f + 0.5f);
        while (tmp > 1) { tmp >>= 1; s->byte_offset_bits++; }
        s->byte_offset_bits += 2;
    }

    high_freq = s->sample_rate >> 1;
    bps1 = bps;
    if (channels == 2)
        bps1 = bps * 1.6f;
    sample_rate1 = s->sample_rate;
    if (s->version == 2) {
        if (sample_rate1 >= 44100) sample_rate1 = 44100;
        else if (sample_rate1 >= 22050) sample_rate1 = 22050;
        else if (sample_rate1 >= 16000) sample_rate1 = 16000;
        else if (sample_rate1 >= 11025) sample_rate1 = 11025;
        else if (sample_rate1 >= 8000) sample_rate1 = 8000;
    }

    if (sample_rate1 == 44100) {
        if (bps1 >= 0.61f) s->use_noise_coding = 0;
        else high_freq = (int)(high_freq * 0.4f);
    } else if (sample_rate1 == 22050) {
        if (bps1 >= 1.16f) s->use_noise_coding = 0;
        else if (bps1 >= 0.72f) high_freq = (int)(high_freq * 0.7f);
        else high_freq = (int)(high_freq * 0.6f);
    } else if (sample_rate1 == 16000) {
        if (bps > 0.5f) high_freq = (int)(high_freq * 0.5f);
        else high_freq = (int)(high_freq * 0.3f);
    } else if (sample_rate1 == 11025) {
        high_freq = (int)(high_freq * 0.7f);
    } else if (sample_rate1 == 8000) {
        if (bps <= 0.625f) high_freq = (int)(high_freq * 0.5f);
        else if (bps > 0.75f) s->use_noise_coding = 0;
        else high_freq = (int)(high_freq * 0.65f);
    } else {
        if (bps >= 0.8f) high_freq = (int)(high_freq * 0.75f);
        else if (bps >= 0.6f) high_freq = (int)(high_freq * 0.6f);
        else high_freq = (int)(high_freq * 0.5f);
    }
    if (high_freq < 100) high_freq = 100;

    {
        for (i = 0; i < s->nb_block_sizes; i++) {
            int block_len = s->frame_len >> i;
            int lpos = 0, j, pos;

            if (s->version == 1) {
                lpos = 0;
                for (j = 0; j < 25; j++) {
                    int a = (int)wma_critical_freqs[j];
                    int b = s->sample_rate;
                    pos = ((block_len * 2 * a) + (b >> 1)) / b;
                    if (pos > block_len) pos = block_len;
                    s->exponent_bands[i][j] = (uint16_t)(pos - lpos);
                    if (pos >= block_len) { j++; break; }
                    lpos = pos;
                }
                s->exponent_sizes[i] = j;
            } else {
                /* FFmpeg uses hardcoded tables for shorter blocks. */
                int a = s->frame_len_bits - WMA_BLOCK_MIN_BITS - i;
                const uint8_t *table = NULL;
                if (a < 3) {
                    if (s->sample_rate >= 44100)
                        table = exponent_band_44100[a];
                    else if (s->sample_rate >= 32000)
                        table = exponent_band_32000[a];
                    else if (s->sample_rate >= 22050)
                        table = exponent_band_22050[a];
                }
                if (table) {
                    int nband = table[0];
                    for (j = 0; j < nband; j++)
                        s->exponent_bands[i][j] = table[j + 1];
                    s->exponent_sizes[i] = nband;
                } else {
                    j = 0; lpos = 0;
                    for (int k = 0; k < 25; k++) {
                        int aa = (int)wma_critical_freqs[k];
                        int b = s->sample_rate;
                        pos = ((block_len * 2 * aa) + (b << 1)) / (4 * b);
                        pos <<= 2;
                        if (pos > block_len) pos = block_len;
                        if (pos > lpos)
                            s->exponent_bands[i][j++] = (uint16_t)(pos - lpos);
                        if (pos >= block_len) break;
                        lpos = pos;
                    }
                    s->exponent_sizes[i] = j;
                }
            }

            s->coefs_end[i] = (s->frame_len - ((s->frame_len * 9) / 100)) >> i;
            s->high_band_start[i] = (int)((float)(block_len * 2 * high_freq) /
                                          (float)s->sample_rate + 0.5f);
            {
                int nband = s->exponent_sizes[i];
                j = 0; pos = 0;
                for (int k = 0; k < nband; k++) {
                    int start = pos, end;
                    pos += s->exponent_bands[i][k];
                    end = pos;
                    if (start < s->high_band_start[i]) start = s->high_band_start[i];
                    if (end > s->coefs_end[i]) end = s->coefs_end[i];
                    if (end > start) s->exponent_high_bands[i][j++] = end - start;
                }
                s->exponent_high_sizes[i] = j;
            }
        }
    }

    for (i = 0; i < s->nb_block_sizes; i++) {
        int n = s->frame_len >> i;
        int n_full = n << 1;
        int n4 = n >> 1;
        int j;
        float sc = sqrtf(1.0f / 32768.0f);
        for (j = 0; j < n; j++)
            s->windows[i][j] = sinf((float)WMA_PI * (2.0f * (float)j + 1.0f) / (4.0f * (float)n));
        for (j = 0; j < n4; j++) {
            float alpha = 2.0f * (float)WMA_PI * ((float)j + 0.125f) / (float)n_full;
            s->mdct_tcos[i][j] = -cosf(alpha) * sc;
            s->mdct_tsin[i][j] = -sinf(alpha) * sc;
        }
    }

    s->reset_block_lengths = 1;

    if (s->use_noise_coding) {
        s->noise_mult = s->use_exp_vlc ? 0.02f : 0.04f;
        {
            unsigned int seed = 1;
            float norm = (1.0f / (float)(1LL << 31)) * 1.7320508f * s->noise_mult;
            for (i = 0; i < WMA_NOISE_TAB_SIZE; i++) {
                seed = seed * 314159 + 1;
                s->noise_table[i] = (float)((int)seed) * norm;
            }
        }
        {
            /* hgain codes are 16-bit in table; widen for wma_init_vlc */
            static uint32_t hgain_codes32[37];
            static int hgain_codes_init = 0;
            if (!hgain_codes_init) {
                for (i = 0; i < 37; i++)
                    hgain_codes32[i] = hgain_huffcodes[i];
                hgain_codes_init = 1;
            }
            wma_init_vlc(&s->hgain_vlc, hgain_codes32, hgain_huffbits, 37);
        }
    }

    if (s->use_exp_vlc) {
        s->exp_vlc.codes = ff_aac_scalefactor_code;
        s->exp_vlc.bits  = ff_aac_scalefactor_bits;
        s->exp_vlc.n     = (int)(sizeof(ff_aac_scalefactor_bits));
        for (i = 0; i < WMA_VLC_SIZE; i++)
            s->exp_vlc.table_a[i] = -1;
        /* also try table init for short codes */
        wma_init_vlc(&s->exp_vlc, ff_aac_scalefactor_code, ff_aac_scalefactor_bits,
                     (int)(sizeof(ff_aac_scalefactor_bits)));
    } else {
        wma_lsp_to_curve_init(s);
    }

    {
        int tbl = 2;
        if (s->sample_rate >= 32000) {
            if (bps1 < 0.72f) tbl = 0;
            else if (bps1 < 1.16f) tbl = 1;
        }

        s->run_table[0] = (uint16_t *)XNewPtr(1336 * sizeof(uint16_t));
        s->level_table[0] = (float *)XNewPtr(1336 * sizeof(float));
        s->run_table[1] = (uint16_t *)XNewPtr(1336 * sizeof(uint16_t));
        s->level_table[1] = (float *)XNewPtr(1336 * sizeof(float));

        if (!s->run_table[0] || !s->level_table[0] || !s->run_table[1] || !s->level_table[1])
            return -1;

        wma_init_coef_vlc(&s->coef_vlc[0], &s->run_table[0], &s->level_table[0],
                          &coef_vlcs[tbl * 2]);
        wma_init_coef_vlc(&s->coef_vlc[1], &s->run_table[1], &s->level_table[1],
                          &coef_vlcs[tbl * 2 + 1]);
    }

    s->coefs_start = (s->version == 1) ? 3 : 0;
    s->last_superframe_len = 0;
    s->last_bitoffset = 0;
    s->skip_frames = 2; /* FFmpeg skip_samples = frame_len * 2 */
    init_pow10_table();

    return 0;
}

void wma_decode_close(WMADecodeContext *s) {
    if (s->run_table[0]) XDisposePtr((XPTR)s->run_table[0]);
    if (s->level_table[0]) XDisposePtr((XPTR)s->level_table[0]);
    if (s->run_table[1]) XDisposePtr((XPTR)s->run_table[1]);
    if (s->level_table[1]) XDisposePtr((XPTR)s->level_table[1]);
    s->run_table[0] = s->run_table[1] = NULL;
    s->level_table[0] = s->level_table[1] = NULL;
}
