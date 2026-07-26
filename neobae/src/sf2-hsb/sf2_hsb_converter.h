/*
 * © 2021–2026 zefie
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

/* sf2_hsb_converter.h - SF2 -> HSB/ZSB conversion using BAE authoring APIs. */

#ifndef SF2_HSB_CONVERTER_H
#define SF2_HSB_CONVERTER_H

#include <stddef.h>
#include <stdint.h>

#include <NeoBAE.h>
#include "mod2rmf_encoder.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int dryRun;
    int verbose;
    int strict;
    int forceHsb;
    int forceZsb;
    int extendedAdsr;  /* --extended-adsr / --ext-adsr: 8-segment exponential curve approx; forces ZSB */
    int conflateStereo;/* --conflate-stereo: merge linked SF2 L/R mono pairs into one stereo sample */
    int attnDiv;       /* --attn-div N: centibel divisor for initialAttenuation->volume mapping.
                        * 200 = SF2 spec-correct (default); 400 = legacy compressed range. */
    Mod2RmfEncoderSettings encoderSettings;  /* --codec/--encoding + optional --bitrate */
} SF2HSBConvertOptions;

typedef struct {
    uint32_t presetCount;
    uint32_t sampleCount;
    uint32_t skippedCount;
} SF2HSBConvertReport;

BAEResult SF2HSB_ConvertBankFile(BAEMixer mixer,
                                 const char *inputPath,
                                 const char *outputPath,
                                 const SF2HSBConvertOptions *options,
                                 SF2HSBConvertReport *report,
                                 char *errorBuffer,
                                 size_t errorBufferSize);

/* Convert an SF2 bank already in memory into Beatnik bank bytes kept in memory.
 * The returned data must be released with XDisposePtr when no longer needed. */
BAEResult SF2HSB_ConvertBankMemory(BAEMixer mixer,
                                   const void *inputData,
                                   size_t inputSize,
                                   const SF2HSBConvertOptions *options,
                                   SF2HSBConvertReport *report,
                                   char *errorBuffer,
                                   size_t errorBufferSize,
                                   unsigned char **outBankData,
                                   uint32_t *outBankSize);

#ifdef __cplusplus
}
#endif

#endif /* SF2_HSB_CONVERTER_H */
