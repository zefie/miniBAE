/*
 * rmfify — embed bank instruments into a MIDI file and save as RMF/ZMF
 * © 2026 zefie
 *
 * 1) Load bank (.hsb/.zsb)
 * 2) Load song (.mid/.rmf/.zmf)
 * 3) Scan song MIDI for used instruments
 * 4) Clone instruments from bank into document
 * 5) Remap program references to bank 2 (0–127 pitched, 640+ for percussion)
 * 6) Save as RMF/ZMF
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "NeoBAE.h"

int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "rmfify — embed a bank's instruments into a song and save as RMF/ZMF\n"
            "\n"
            "Usage: %s <bank.hsb|bank.zsb> <song.mid|song.rmf> [output.rmf] [-q]\n"
            "\n"
            "  bank file       HSB or ZSB bank file containing instruments\n"
            "  song file       MIDI (.mid) or existing RMF/ZMF song\n"
            "  output file     Output path (default: <song>_rmfify.rmf)\n"
            "  -q              Quiet (suppress progress output)\n"
            "\n"
            "How it works:\n"
            "  1. Scans every track and note in the song for used instrument references\n"
            "  2. For each pitched (bank, program) pair, clones the matching bank instrument\n"
            "  3. For each percussion note (channel 9), clones that drum sound\n"
            "  4. Remaps all MIDI references to bank 2 (pitched) or bank 5 (percussion)\n"
            "  5. Saves the resulting document as RMF (.rmf) or ZMF (.zmf)\n",
            prog);
}

int main(int argc, char *argv[])
{
    const char *bankPath = NULL;
    const char *songPath = NULL;
    const char *destPath = NULL;
    BAEResult result;
    BAEMixer mixer;
    BAEBankToken bankToken;
    BAERmfEditorDocument *document;
    int quiet = 0;
    unsigned int i;

    for (i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))
        {
            print_usage(argv[0]);
            return 0;
        }
        if (!strcmp(argv[i], "-q"))
        {
            quiet = 1;
            continue;
        }
        if (!bankPath)      { bankPath = argv[i]; continue; }
        if (!songPath)      { songPath = argv[i]; continue; }
        if (!destPath)      { destPath = argv[i]; continue; }
        fprintf(stderr, "Error: unexpected argument '%s'\n", argv[i]);
        return 1;
    }

    if (!bankPath || !songPath)
    {
        print_usage(argv[0]);
        return 1;
    }

    if (!file_exists(bankPath))
    {
        fprintf(stderr, "Error: bank file not found: %s\n", bankPath);
        return 1;
    }
    if (!file_exists(songPath))
    {
        fprintf(stderr, "Error: song file not found: %s\n", songPath);
        return 1;
    }

    /* Build default output path if not given */
    static char defaultDest[4096];
    if (!destPath)
    {
        const char *dot = strrchr(songPath, '.');
        size_t baseLen;
        if (dot) baseLen = (size_t)(dot - songPath);
        else     baseLen = strlen(songPath);
        if (baseLen + 16 >= sizeof(defaultDest))
        {
            fprintf(stderr, "Error: song path too long for default output name\n");
            return 1;
        }
        memcpy(defaultDest, songPath, baseLen);
        strcpy(defaultDest + baseLen, "_rmfify.rmf");
        destPath = defaultDest;
    }

    /* Init */
    result = BAE_Setup();
    if (result != BAE_NO_ERROR)
    {
        fprintf(stderr, "Error: BAE_Setup failed (%d)\n", (int)result);
        return 1;
    }

    mixer = BAEMixer_New();
    if (!mixer)
    {
        fprintf(stderr, "Error: BAEMixer_New failed\n");
        BAE_Cleanup();
        return 1;
    }

    result = BAEMixer_Open(mixer,
                           BAE_RATE_44K,
                           BAE_LINEAR_INTERPOLATION,
                           BAE_USE_16 | BAE_USE_STEREO,
                           64, 16, 32, TRUE);
    if (result != BAE_NO_ERROR)
    {
        fprintf(stderr, "Error: BAEMixer_Open failed (%d)\n", (int)result);
        BAE_Cleanup();
        return 1;
    }

    /* Step 1: Load bank */
    if (!quiet) fprintf(stderr, "Loading bank: %s\n", bankPath);
    bankToken = NULL;
    result = BAEMixer_AddBankFromFile(mixer, (char *)bankPath, &bankToken);
    if (result != BAE_NO_ERROR)
    {
        fprintf(stderr, "Error: failed to load bank '%s' (error %d)\n", bankPath, (int)result);
        BAEMixer_UnloadBanks(mixer);
        BAE_Cleanup();
        return 1;
    }

    /* Step 2: Load song */
    if (!quiet) fprintf(stderr, "Loading song: %s\n", songPath);
    document = BAERmfEditorDocument_LoadFromFile((BAEPathName)songPath);
    if (!document)
    {
        fprintf(stderr, "Error: failed to load song '%s'\n", songPath);
        BAEMixer_UnloadBanks(mixer);
        BAE_Cleanup();
        return 1;
    }

    {
        BAERmfEditorCloneUsedResult cloneResult;

        result = BAERmfEditorDocument_CloneUsedInstrumentsFromBank(document,
                                                                   bankToken,
                                                                   &cloneResult);
        if (result != BAE_NO_ERROR)
        {
            fprintf(stderr, "Error: clone used instruments failed (%d)\n", (int)result);
            BAERmfEditorDocument_Delete(document);
            BAEMixer_UnloadBanks(mixer);
            BAE_Cleanup();
            return 1;
        }
        if (cloneResult.pitchedCount == 0 && cloneResult.percussionCount == 0)
        {
            fprintf(stderr, "Error: no matching instruments found in bank for this song\n");
            BAERmfEditorDocument_Delete(document);
            BAEMixer_UnloadBanks(mixer);
            BAE_Cleanup();
            return 1;
        }
        if (!quiet)
        {
            uint32_t mappingIndex;
            for (mappingIndex = 0; mappingIndex < cloneResult.mappingCount; ++mappingIndex)
            {
                BAERmfEditorCloneUsedMapping const *mapping = &cloneResult.mappings[mappingIndex];
                fprintf(stderr,
                        "  %s %u:%u INST %u%s -> INST %u (%u sample%s): %s\n",
                        mapping->isPercussion ? "drum" : "pitched",
                        (unsigned)mapping->sourceBank,
                        (unsigned)mapping->sourceProgram,
                        (unsigned)mapping->requestedInstID,
                        mapping->resolvedInstID != mapping->requestedInstID ? " (alias resolved)" : "",
                        (unsigned)mapping->targetInstID,
                        (unsigned)mapping->sampleCount,
                        mapping->sampleCount == 1 ? "" : "s",
                        mapping->resolvedName[0] ? mapping->resolvedName : "(unnamed)");
                if (mapping->resolvedInstID != mapping->requestedInstID)
                {
                    fprintf(stderr, "    resolved source INST: %u\n", (unsigned)mapping->resolvedInstID);
                }
            }
        }

        if (!quiet) fprintf(stderr, "Saving: %s\n", destPath);
        result = BAERmfEditorDocument_SaveAsRmf(document, (BAEPathName)destPath);
        if (result != BAE_NO_ERROR)
        {
            fprintf(stderr, "Error: save failed (%d): %s\n", (int)result, destPath);
            BAERmfEditorDocument_Delete(document);
            BAEMixer_UnloadBanks(mixer);
            BAE_Cleanup();
            return 1;
        }

        if (!quiet) fprintf(stdout, "Wrote %u pitched + %u percussion -> %s\n",
                            (unsigned)cloneResult.pitchedCount,
                            (unsigned)cloneResult.percussionCount,
                            destPath);
        BAERmfEditorDocument_Delete(document);
        BAEMixer_UnloadBanks(mixer);
        BAE_Cleanup();
        return 0;
    }
}
