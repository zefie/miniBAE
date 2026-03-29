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

#ifdef __cplusplus
}
#endif

#endif /* SF2_HSB_CONVERTER_H */
