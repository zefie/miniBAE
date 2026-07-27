/*
 * © 2021–2026 zefie
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <NeoBAE.h>
#include <BAE_API.h>

#include "sf2_hsb_converter.h"
#include "mod2rmf_encoder.h"

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
    printf("  --attn-div N   Centibel divisor for initialAttenuation->volume mapping.\n");
    printf("                 200 = SF2 spec-correct (default); 400 = legacy compressed range.\n");
    printf("  --codec NAME   Sample encoding codec (or numeric id). Alias: --encoding\n");
    printf("  --bitrate N    Codec bitrate in kbps (or bps). Used by MP3/Vorbis/Opus.\n");
    printf("  --codecs       Print available codec/bitrate options and exit\n");
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
    BAERmfEditorCompressionType resolvedCompression;
    int usedCodecOptions;
    int i;
    char errorBuffer[512];

    memset(&options, 0, sizeof(options));
    memset(&report, 0, sizeof(report));
    memset(errorBuffer, 0, sizeof(errorBuffer));
    mod2rmf_encoder_defaults(&options.encoderSettings);
    usedCodecOptions = 0;

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
        } else if (strcmp(argv[i], "--attn-div") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--attn-div requires a numeric argument.\n");
                print_usage(argv[0]);
                return 1;
            }
            ++i;
            options.attnDiv = atoi(argv[i]);
            if (options.attnDiv <= 0) {
                fprintf(stderr, "--attn-div must be a positive integer.\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--codec") == 0 || strcmp(argv[i], "--encoding") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s requires an argument.\n", argv[i]);
                print_usage(argv[0]);
                return 1;
            }
            ++i;
            if (mod2rmf_encoder_parse_codec(argv[i], &options.encoderSettings.codec) != 0) {
                fprintf(stderr, "Unknown codec: %s\n", argv[i]);
                mod2rmf_encoder_print_codecs();
                return 1;
            }
            usedCodecOptions = 1;
        } else if (strcmp(argv[i], "--bitrate") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--bitrate requires an argument.\n");
                print_usage(argv[0]);
                return 1;
            }
            ++i;
            if (mod2rmf_encoder_parse_bitrate(argv[i], &options.encoderSettings.bitrateKbps) != 0) {
                fprintf(stderr, "Invalid bitrate: %s\n", argv[i]);
                return 1;
            }
            usedCodecOptions = 1;
        } else if (strcmp(argv[i], "--codecs") == 0) {
            mod2rmf_encoder_print_codecs();
            return 0;
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

    resolvedCompression = mod2rmf_encoder_resolve(&options.encoderSettings);

    if (mod2rmf_encoder_requires_zmf(options.encoderSettings.codec)) {
        if (options.forceHsb) {
            fprintf(stderr, "Selected codec requires ZSB output and cannot be used with --force-hsb.\n");
            return 1;
        }
        options.forceZsb = 1;
    }

    if (usedCodecOptions) {
        fprintf(stderr, "Encoding: %s\n", mod2rmf_encoder_label(resolvedCompression));
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
