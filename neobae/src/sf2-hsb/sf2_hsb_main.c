#include <stdio.h>
#include <string.h>

#include <NeoBAE.h>
#include <BAE_API.h>

#include "sf2_hsb_converter.h"

static void print_usage(const char *programName)
{
    printf("Usage: %s [options] <input.sf2> [output.hsb|output.zsb]\n", programName);
    printf("\n");
    printf("Options:\n");
    printf("  --dry-run      Analyse SF2 without writing output\n");
    printf("  --verbose      Print per-preset conversion details\n");
    printf("  --strict       Fail fast on conversion warnings\n");
    printf("  --force-hsb    Force .hsb output\n");
    printf("  --force-zsb    Force .zsb output\n");
    printf("  --extended-adsr / --ext-adsr\n");
    printf("                 Approximate SF2 exponential ADSR curves with 8-segment\n");
    printf("                 piecewise linear interpolation.  Implies --force-zsb.\n");
    printf("  -h, --help     Show this help\n");
}

int main(int argc, char **argv)
{
    const char *inputPath = NULL;
    const char *outputPath = NULL;
    SF2HSBConvertOptions options;
    SF2HSBConvertReport report;
    BAEResult result;
    BAEMixer mixer;
    int i;
    char errorBuffer[512];

    memset(&options, 0, sizeof(options));
    memset(&report, 0, sizeof(report));
    memset(errorBuffer, 0, sizeof(errorBuffer));

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--dry-run") == 0) {
            options.dryRun = 1;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            options.verbose = 1;
        } else if (strcmp(argv[i], "--strict") == 0) {
            options.strict = 1;
        } else if (strcmp(argv[i], "--force-hsb") == 0) {
            options.forceHsb = 1;
        } else if (strcmp(argv[i], "--force-zsb") == 0) {
            options.forceZsb = 1;
        } else if (strcmp(argv[i], "--extended-adsr") == 0 || strcmp(argv[i], "--ext-adsr") == 0) {
            options.extendedAdsr = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        } else if (!inputPath) {
            inputPath = argv[i];
        } else if (!outputPath) {
            outputPath = argv[i];
        } else {
            fprintf(stderr, "Too many positional arguments.\n");
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!inputPath) {
        fprintf(stderr, "Missing input SF2 path.\n");
        print_usage(argv[0]);
        return 1;
    }

    if (!options.dryRun && !outputPath) {
        fprintf(stderr, "Missing output path (required unless --dry-run).\n");
        print_usage(argv[0]);
        return 1;
    }

    result = BAE_Setup();
    if (result != BAE_NO_ERROR) {
        fprintf(stderr, "BAE_Setup failed: %d\n", result);
        return 1;
    }

    mixer = BAEMixer_New();
    if (!mixer) {
        fprintf(stderr, "BAEMixer_New failed.\n");
        BAE_Cleanup();
        return 1;
    }

    printf("Converting SF2 '%s' to %s '%s', this may take a few moments...\n",
           inputPath,
           options.forceHsb ? "HSB" : (options.forceZsb ? "ZSB" : "HSB/ZSB"),
           outputPath ? outputPath : "(dry run)");
    result = SF2HSB_ConvertBankFile(mixer,
                                    inputPath,
                                    outputPath,
                                    &options,
                                    &report,
                                    errorBuffer,
                                    sizeof(errorBuffer));

    BAEMixer_Delete(mixer);
    BAE_Cleanup();

    if (result != BAE_NO_ERROR) {
        fprintf(stderr, "Conversion failed: %s (code %d)\n",
                errorBuffer[0] ? errorBuffer : "Unknown error",
                result);
        return 2;
    }

    if (options.dryRun) {
        printf("Dry-run complete.\n");
    } else {
        printf("Conversion successful.\n");
    }

    printf("Presets: %u\n", (unsigned)report.presetCount);
    printf("Samples: %u\n", (unsigned)report.sampleCount);
    if (report.skippedCount) {
        printf("Skipped presets: %u\n", (unsigned)report.skippedCount);
    }

    return 0;
}
