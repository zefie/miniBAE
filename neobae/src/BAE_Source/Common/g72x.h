/*
 * This source code is a product of Sun Microsystems, Inc. and is provided
 * for unrestricted use.  Users may copy or modify this source code without
 * charge.
 *
 * SUN SOURCE CODE IS PROVIDED AS IS WITH NO WARRANTIES OF ANY KIND INCLUDING
 * THE WARRANTIES OF DESIGN, MERCHANTIBILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE, OR ARISING FROM A COURSE OF DEALING, USAGE OR TRADE PRACTICE.
 *
 * Sun source code is provided with no support and without any obligation on
 * the part of Sun Microsystems, Inc. to assist in its use, correction,
 * modification or enhancement.
 *
 * SUN MICROSYSTEMS, INC. SHALL HAVE NO LIABILITY WITH RESPECT TO THE
 * INFRINGEMENT OF COPYRIGHTS, TRADE SECRETS OR ANY PATENTS BY THIS SOFTWARE
 * OR ANY PART THEREOF.
 *
 * In no event will Sun Microsystems, Inc. be liable for any lost revenue
 * or profits or other special, indirect and consequential damages, even if
 * Sun has been advised of the possibility of such damages.
 *
 * Sun Microsystems, Inc.
 * 2550 Garcia Avenue
 * Mountain View, California  94043
 */

#include "X_API.h"
/*
 * g72x.h
 *
 * Header file for CCITT conversion routines.
 *
 */
#ifndef _G72X_H
#define _G72X_H

#define AUDIO_ENCODING_ULAW (1) /* ISDN u-law */
#define AUDIO_ENCODING_ALAW (2) /* ISDN A-law */
#define AUDIO_ENCODING_LINEAR   (3) /* PCM 2's-complement (0-center) */

/*
 * The following is the definition of the state structure
 * used by the G.721/G.723 encoder and decoder to preserve their internal
 * state between successive calls.  The meanings of the majority
 * of the state structure fields are explained in detail in the
 * CCITT Recommendation G.721.  The field names are essentially indentical
 * to variable names in the bit level description of the coding algorithm
 * included in this Recommendation.
 */
struct g72x_state {
    int32_t yl;    /* Locked or steady state step size multiplier. */
    int16_t yu;   /* Unlocked or non-steady state step size multiplier. */
    int16_t dms;  /* Short term energy estimate. */
    int16_t dml;  /* Long term energy estimate. */
    int16_t ap;   /* Linear weighting coefficient of 'yl' and 'yu'. */

    int16_t a[2]; /* Coefficients of pole portion of prediction filter. */
    int16_t b[6]; /* Coefficients of zero portion of prediction filter. */
    int16_t pk[2];    /*
             * Signs of previous two samples of a partially
             * reconstructed signal.
             */
    int16_t dq[6];    /*
             * Previous 6 samples of the quantized difference
             * signal represented in an internal floating point
             * format.
             */
    int16_t sr[2];    /*
             * Previous 2 samples of the quantized difference
             * signal represented in an internal floating point
             * format.
             */
    char td;    /* delayed tone detect, new in 1988 version */
};

/* External function definitions. */

extern void g72x_init_state(struct g72x_state *);
extern int g721_encoder(
        int sample,
        int in_coding,
        struct g72x_state *state_ptr);
extern int bae_g721_decoder(
        int code,
        int out_coding,
        struct g72x_state *state_ptr);
extern int bae_g723_24_encoder(
        int sample,
        int in_coding,
        struct g72x_state *state_ptr);
extern int bae_g723_24_decoder(
        int code,
        int out_coding,
        struct g72x_state *state_ptr);
extern int g723_40_encoder(
        int sample,
        int in_coding,
        struct g72x_state *state_ptr);
extern int bae_g723_40_decoder(
        int code,
        int out_coding,
        struct g72x_state *state_ptr);

extern int
bae_predictor_zero(
    struct g72x_state *state_ptr);

extern int
bae_predictor_pole(
    struct g72x_state *state_ptr);

extern int
bae_step_size(
    struct g72x_state *state_ptr);

extern int
bae_quantize(
    int     d,  /* Raw difference signal sample */
    int     y,  /* Step size multiplier */
    int16_t       *table, /* quantization table */
    int     size);  /* table size of short integers */

extern int
bae_reconstruct(
    int     sign,   /* 0 for non-negative value */
    int     dqln,   /* G.72x codeword */
    int     y); /* Step size multiplier */

extern void
bae_update(
    int     code_size,  /* distinguish 723_40 with others */
    int     y,      /* quantizer step size */
    int     wi,     /* scale factor multiplier */
    int     fi,     /* for long/short term energies */
    int     dq,     /* quantized prediction difference */
    int     sr,     /* reconstructed signal */
    int     dqsez,      /* difference from 2-pole predictor */
    struct g72x_state *state_ptr);  /* coder state pointer */

extern int
tandem_adjust_alaw(
    int     sr, /* decoder output linear PCM sample */
    int     se, /* predictor estimate sample */
    int     y,  /* quantizer step size */
    int     i,  /* decoder input code */
    int     sign,
    int16_t       *qtab);

int
tandem_adjust_ulaw(
    int     sr, /* decoder output linear PCM sample */
    int     se, /* predictor estimate sample */
    int     y,  /* quantizer step size */
    int     i,  /* decoder input code */
    int     sign,
    int16_t       *qtab);
    
#define abs(x)          (((x)<0) ? -(x) : (x))

// g711
/* 2's complement (16-bit range) */
extern unsigned char linear2alaw( int pcm_val);

// Convert an A-law value to 16-bit linear PCM
extern int alaw2linear(unsigned char    a_val);

extern int ulaw2linear(unsigned char    u_val);

/* A-law to u-law conversion */
extern unsigned char alaw2ulaw(unsigned char    aval);

/* u-law to A-law conversion */
extern unsigned char ulaw2alaw(unsigned char    uval);

/* 2's complement (16-bit range) */
extern unsigned char linear2ulaw(int        pcm_val);

#endif /* !_G72X_H */
