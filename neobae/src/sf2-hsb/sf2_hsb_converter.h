/* sf2_hsb_converter.h — SF2 -> HSB/ZSB conversion using BAE authoring APIs. */

#ifndef SF2_HSB_CONVERTER_H
#define SF2_HSB_CONVERTER_H

#include <stddef.h>
#include <stdint.h>

#include <NeoBAE.h>

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
