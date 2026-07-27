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

/****************************************************************************
 *
 * mid2rmf.c
 *
 * MIDI -> RMF/ZMF converter using the RMF editor document pipeline.
 *
 * Usage:
 *   mid2rmf [--midi-format=ecmi|cmid|emid|midi] <source.mid> <dest.rmf>
 *
 * Notes:
 * - Default MIDI storage format is ECMI.
 * - Output container header is determined by destination extension:
 *   .rmf => IREZ, .zmf => ZREZ
 *
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <NeoBAE.h>

static void print_usage(const char *program_name)
{
    fprintf(stderr,
            "Usage: %s [--midi-format=ecmi|cmid|emid|midi] <source.mid> <dest.rmf|dest.zmf>\n",
            program_name);
}

static int parse_midi_storage(const char *value, BAERmfEditorMidiStorageType *outType)
{
    if (!value || !outType)
    {
        return 0;
    }

    if (strcmp(value, "ecmi") == 0)
    {
        *outType = BAE_EDITOR_MIDI_STORAGE_ECMI;
        return 1;
    }
    if (strcmp(value, "cmid") == 0)
    {
        *outType = BAE_EDITOR_MIDI_STORAGE_CMID_BEST_EFFORT;
        return 1;
    }
    if (strcmp(value, "emid") == 0)
    {
        *outType = BAE_EDITOR_MIDI_STORAGE_EMID;
        return 1;
    }
    if (strcmp(value, "midi") == 0)
    {
        *outType = BAE_EDITOR_MIDI_STORAGE_MIDI;
        return 1;
    }

    return 0;
}

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        return 0;
    }
    fclose(f);
    return 1;
}

int main(int argc, char *argv[])
{
    BAERmfEditorMidiStorageType midiStorageType = BAE_EDITOR_MIDI_STORAGE_ECMI;
    const char *sourcePath = NULL;
    const char *destPath = NULL;
    BAEResult result;
    BAERmfEditorDocument *document;
    uint32_t sampleCount;
    int i;

    if (argc < 3)
    {
        print_usage(argv[0]);
        return 1;
    }

    for (i = 1; i < argc; ++i)
    {
        const char *arg = argv[i];

        if (strncmp(arg, "--midi-format=", 14) == 0)
        {
            if (!parse_midi_storage(arg + 14, &midiStorageType))
            {
                fprintf(stderr, "Error: unsupported --midi-format value '%s'\n", arg + 14);
                print_usage(argv[0]);
                return 1;
            }
            continue;
        }
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0)
        {
            print_usage(argv[0]);
            return 0;
        }

        if (!sourcePath)
        {
            sourcePath = arg;
            continue;
        }
        if (!destPath)
        {
            destPath = arg;
            continue;
        }

        fprintf(stderr, "Error: unexpected argument '%s'\n", arg);
        print_usage(argv[0]);
        return 1;
    }

    if (!sourcePath || !destPath)
    {
        print_usage(argv[0]);
        return 1;
    }

    if (!file_exists(sourcePath))
    {
        fprintf(stderr, "Error: source file not found: %s\n", sourcePath);
        return 1;
    }

    result = BAE_Setup();
    if (result != BAE_NO_ERROR)
    {
        fprintf(stderr, "Error: BAE_Setup failed (%d)\n", (int)result);
        return 1;
    }

    document = BAERmfEditorDocument_LoadFromFile((BAEPathName)sourcePath);
    if (!document)
    {
        fprintf(stderr, "Error: failed to load source as MIDI/RMI/KAR: %s\n", sourcePath);
        BAE_Cleanup();
        return 1;
    }

    sampleCount = 0;
    result = BAERmfEditorDocument_GetSampleCount(document, &sampleCount);
    if (result != BAE_NO_ERROR)
    {
        fprintf(stderr, "Error: failed to inspect source document (%d)\n", (int)result);
        BAERmfEditorDocument_Delete(document);
        BAE_Cleanup();
        return 1;
    }

    if (sampleCount != 0)
    {
        fprintf(stderr,
                "Error: source contains embedded samples/custom instruments; mid2rmf accepts raw MIDI-style sources only.\n");
        BAERmfEditorDocument_Delete(document);
        BAE_Cleanup();
        return 1;
    }

    result = BAERmfEditorDocument_SetMidiStorageType(document, midiStorageType);
    if (result != BAE_NO_ERROR)
    {
        fprintf(stderr, "Error: failed to set MIDI storage type (%d)\n", (int)result);
        BAERmfEditorDocument_Delete(document);
        BAE_Cleanup();
        return 1;
    }

    result = BAERmfEditorDocument_SaveAsRmf(document, (BAEPathName)destPath);
    if (result != BAE_NO_ERROR)
    {
        fprintf(stderr, "Error: save failed (%d): %s\n", (int)result, destPath);
        BAERmfEditorDocument_Delete(document);
        BAE_Cleanup();
        return 1;
    }

    fprintf(stdout,
            "Saved %s -> %s (midi-format=%s)\n",
            sourcePath,
            destPath,
            (midiStorageType == BAE_EDITOR_MIDI_STORAGE_ECMI) ? "ecmi" :
            (midiStorageType == BAE_EDITOR_MIDI_STORAGE_CMID_BEST_EFFORT) ? "cmid" :
            (midiStorageType == BAE_EDITOR_MIDI_STORAGE_EMID) ? "emid" : "midi");

    BAERmfEditorDocument_Delete(document);
    BAE_Cleanup();
    return 0;
}
