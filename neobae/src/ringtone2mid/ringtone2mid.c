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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <NeoBAE.h>

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [--format=auto|imy|rng|rtx] [--program-default=flute|saw|<program>] <source> <dest.mid>\n",
            program);
}

static int parse_imy_default(const char *s, int *outProgram)
{
    char *endptr;
    long value;

    if (!s || !outProgram)
        return 0;

    if (strcasecmp(s, "flute") == 0)
    {
        *outProgram = 73;
        return 1;
    }
    if (strcasecmp(s, "saw") == 0 ||
        strcasecmp(s, "lead-saw") == 0 ||
        strcasecmp(s, "sawlead") == 0)
    {
        *outProgram = 81;
        return 1;
    }

    value = strtol(s, &endptr, 10);
    if (!*s || (endptr && *endptr != '\0'))
        return 0;
    if (value < 0 || value > 127)
        return 0;

    *outProgram = (int)value;
    return 1;
}

static BAEFileType parse_format(const char *s)
{
    if (!s || strcmp(s, "auto") == 0)
        return BAE_INVALID_TYPE;
    if (strcmp(s, "imy") == 0)
        return BAE_RINGTONE_IMY;
    if (strcmp(s, "rng") == 0)
        return BAE_RINGTONE_RNG;
    if (strcmp(s, "rtx") == 0 || strcmp(s, "rtttl") == 0)
        return BAE_RINGTONE_RTX;
    return BAE_INVALID_TYPE;
}

static BAEFileType detect_from_path(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext)
        return BAE_INVALID_TYPE;
    if (strcasecmp(ext, ".imy") == 0 || strcasecmp(ext, ".emy") == 0)
        return BAE_RINGTONE_IMY;
    if (strcasecmp(ext, ".rng") == 0)
        return BAE_RINGTONE_RNG;
    if (strcasecmp(ext, ".rtx") == 0 || strcasecmp(ext, ".rtttl") == 0)
        return BAE_RINGTONE_RTX;
    return BAE_INVALID_TYPE;
}

int main(int argc, char **argv)
{
    const char *src = NULL;
    const char *dst = NULL;
    BAEFileType ftype = BAE_INVALID_TYPE;
    int imyDefaultProgram = -1;
    int i;
    BAEResult err;
    unsigned char *midi = NULL;
    uint32_t midiSize = 0;
    FILE *out;

    if (argc < 3)
    {
        print_usage(argv[0]);
        return 1;
    }

    for (i = 1; i < argc; ++i)
    {
        const char *arg = argv[i];
        if (strncmp(arg, "--format=", 9) == 0)
        {
            ftype = parse_format(arg + 9);
            if (ftype == BAE_INVALID_TYPE && strcmp(arg + 9, "auto") != 0)
            {
                fprintf(stderr, "Unsupported format: %s\n", arg + 9);
                return 1;
            }
            continue;
        }
        if (strncmp(arg, "--program-default=", 18) == 0)
        {
            if (!parse_imy_default(arg + 18, &imyDefaultProgram))
            {
                fprintf(stderr, "Unsupported --program-default value: %s\n", arg + 18);
                return 1;
            }
            continue;
        }
        if (strncmp(arg, "--imy-default=", 14) == 0)
        {
            /* Backward-compatible alias for --program-default. */
            if (!parse_imy_default(arg + 14, &imyDefaultProgram))
            {
                fprintf(stderr, "Unsupported --imy-default value: %s\n", arg + 14);
                return 1;
            }
            continue;
        }
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0)
        {
            print_usage(argv[0]);
            return 0;
        }

        if (!src)
            src = arg;
        else if (!dst)
            dst = arg;
        else
        {
            fprintf(stderr, "Unexpected argument: %s\n", arg);
            return 1;
        }
    }

    if (!src || !dst)
    {
        print_usage(argv[0]);
        return 1;
    }

    if (ftype == BAE_INVALID_TYPE)
        ftype = detect_from_path(src);

    if (ftype == BAE_INVALID_TYPE)
    {
        fprintf(stderr, "Could not infer ringtone format from path. Use --format=...\n");
        return 1;
    }

    err = BAE_Setup();
    if (err != BAE_NO_ERROR)
    {
        fprintf(stderr, "BAE_Setup failed: %d\n", (int)err);
        return 1;
    }

    if (imyDefaultProgram >= 0)
        BAERingtone_SetIMYDefaultProgram(imyDefaultProgram);

    err = BAERingtone_ConvertToMidiFromFile((BAEPathName)src, ftype, &midi, &midiSize);
    if (err != BAE_NO_ERROR)
    {
        fprintf(stderr, "Conversion failed: %d\n", (int)err);
        BAE_Cleanup();
        return 1;
    }

    out = fopen(dst, "wb");
    if (!out)
    {
        fprintf(stderr, "Failed to open output: %s\n", dst);
        BAERingtone_FreeMidiBuffer(midi);
        BAE_Cleanup();
        return 1;
    }

    if (fwrite(midi, 1, midiSize, out) != midiSize)
    {
        fprintf(stderr, "Failed to write output MIDI\n");
        fclose(out);
        BAERingtone_FreeMidiBuffer(midi);
        BAE_Cleanup();
        return 1;
    }

    fclose(out);
    BAERingtone_FreeMidiBuffer(midi);
    BAE_Cleanup();

    fprintf(stdout, "Saved %s -> %s (%u bytes)\n", src, dst, (unsigned)midiSize);
    return 0;
}
