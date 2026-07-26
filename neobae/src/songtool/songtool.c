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

/****************************************************************************
 *
 * songtool.c
 *
 * RMF/ZMF utility:
 *  - Recompress all sample assets to a target codec/bitrate
 *  - Set MIDI loop markers from millisecond offsets
 *
 * Usage:
 *   songtool [options] <source.rmf|source.zmf> <dest.rmf|dest.zmf>
 *
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include <NeoBAE.h>

#include "mod2rmf_encoder.h"

#define SONGTOOL_MAX_SAMPLE_OVERRIDES 4096
#define SONGTOOL_MAX_INSTRUMENT_OVERRIDES 1024
#define SONGTOOL_MAX_METADATA_EDITS 128
#define SONGTOOL_MAX_RESAMPLE_TARGETS 4096

#ifndef SONGTOOL_ENABLE_MIDI_EXPORT
#define SONGTOOL_ENABLE_MIDI_EXPORT 1
#endif

#ifndef SONGTOOL_ENABLE_ROLLED_MIDI_UNROLL
#if defined(BAE_ENABLE_ROLLED_MIDI_UNROLL)
#define SONGTOOL_ENABLE_ROLLED_MIDI_UNROLL BAE_ENABLE_ROLLED_MIDI_UNROLL
#else
#define SONGTOOL_ENABLE_ROLLED_MIDI_UNROLL 0
#endif
#endif

typedef struct SongtoolSampleOverride
{
    uint32_t sampleIndex;
    Mod2RmfEncoderSettings settings;
    BAERmfEditorCompressionType compressionType;
} SongtoolSampleOverride;

typedef struct SongtoolInstrumentOverride
{
    uint32_t instID;
    Mod2RmfEncoderSettings settings;
    BAERmfEditorCompressionType compressionType;
} SongtoolInstrumentOverride;

typedef struct SongtoolMetadataField
{
    BAEInfoType type;
    const char *name;
    const char *label;
} SongtoolMetadataField;

typedef struct SongtoolMetadataEdit
{
    BAEInfoType type;
    const char *value;
    int clear;
} SongtoolMetadataEdit;

typedef struct SongtoolResampleTarget
{
    uint32_t sampleIndex;
    uint32_t rateHz;
} SongtoolResampleTarget;

static SongtoolMetadataField const kSongtoolMetadataFields[] = {
    { TITLE_INFO, "title", "Title" },
    { PERFORMED_BY_INFO, "performed-by", "Performed By" },
    { COMPOSER_INFO, "composer", "Composer" },
    { COPYRIGHT_INFO, "copyright", "Copyright" },
    { PUBLISHER_CONTACT_INFO, "publisher-contact", "Publisher Contact" },
    { USE_OF_LICENSE_INFO, "use-of-license", "Use Of License" },
    { LICENSED_TO_URL_INFO, "licensed-to-url", "Licensed To URL" },
    { LICENSE_TERM_INFO, "license-term", "License Term" },
    { EXPIRATION_DATE_INFO, "expiration-date", "Expiration Date" },
    { COMPOSER_NOTES_INFO, "composer-notes", "Composer Notes" },
    { INDEX_NUMBER_INFO, "index-number", "Index Number" },
    { GENRE_INFO, "genre", "Genre" },
    { SUB_GENRE_INFO, "sub-genre", "Sub-Genre" },
    { TEMPO_DESCRIPTION_INFO, "tempo-description", "Tempo Description" },
    { ORIGINAL_SOURCE_INFO, "original-source", "Original Source" }
};

static uint32_t songtool_metadata_field_count(void)
{
    return (uint32_t)(sizeof(kSongtoolMetadataFields) / sizeof(kSongtoolMetadataFields[0]));
}

static int songtool_str_equal_ignore_case(const char *a, const char *b)
{
    unsigned char ca;
    unsigned char cb;

    if (!a || !b)
    {
        return 0;
    }

    while (*a && *b)
    {
        ca = (unsigned char)*a;
        cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z')
        {
            ca = (unsigned char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z')
        {
            cb = (unsigned char)(cb - 'A' + 'a');
        }
        if (ca != cb)
        {
            return 0;
        }
        ++a;
        ++b;
    }
    return (*a == '\0' && *b == '\0') ? 1 : 0;
}

static int songtool_parse_metadata_field_name(const char *name,
                                              BAEInfoType *outType,
                                              const char **outLabel)
{
    uint32_t i;

    if (!name || !outType)
    {
        return 0;
    }

    for (i = 0; i < songtool_metadata_field_count(); ++i)
    {
        if (songtool_str_equal_ignore_case(name, kSongtoolMetadataFields[i].name))
        {
            *outType = kSongtoolMetadataFields[i].type;
            if (outLabel)
            {
                *outLabel = kSongtoolMetadataFields[i].label;
            }
            return 1;
        }
    }

    return 0;
}

static void print_metadata_field_names(void)
{
    uint32_t i;

    fprintf(stderr, "Supported metadata fields:\n");
    for (i = 0; i < songtool_metadata_field_count(); ++i)
    {
        fprintf(stderr, "  %s\n", kSongtoolMetadataFields[i].name);
    }
}

static int apply_metadata_edits(BAERmfEditorDocument *document,
                                SongtoolMetadataEdit const *edits,
                                uint32_t editCount)
{
    uint32_t i;

    if (!document || !edits)
    {
        return 0;
    }

    for (i = 0; i < editCount; ++i)
    {
        BAEResult result;
        const char *value;
        const char *label;
        uint32_t j;

        label = "Metadata";
        for (j = 0; j < songtool_metadata_field_count(); ++j)
        {
            if (kSongtoolMetadataFields[j].type == edits[i].type)
            {
                label = kSongtoolMetadataFields[j].label;
                break;
            }
        }

        value = edits[i].clear ? "" : edits[i].value;
        if (!value)
        {
            value = "";
        }

        result = BAERmfEditorDocument_SetInfo(document, edits[i].type, value);
        if (result != BAE_NO_ERROR)
        {
            fprintf(stderr,
                    "Error: failed to set metadata field %s (%d)\n",
                    label,
                    (int)result);
            return 0;
        }
    }

    return 1;
}

static void print_usage(const char *program_name)
{
    fprintf(stderr,
            "Usage: %s [options] <source.rmf|source.zmf> [dest.rmf|dest.zmf|dest.mid]\n"
            "\n"
            "Options:\n"
            "  --info               Show source info (title, song length, codecs, loop points)\n"
            "  --upgrade             Upgrade file to the current ZMF format version, or\n"
            "                        convert standard MIDI (.mid) to RMF/ZMF (standalone)\n"
            "  --codec N|NAME        Recompress all samples to codec number/name\n"
            "                        (pcm, adpcm, alaw, ulaw, mp3, vorbis, flac, opus, qoa)\n"
            "  --sndstorage T        Set all sample storage containers: ESND, CSND, or SND\n"
            "  --sample-codec SPEC   Override one sample codec (repeatable)\n"
            "                        SPEC: index:codec[@bitrate], e.g. 3:opus@96\n"
            "  --instrument-codec SPEC\n"
            "                        Override all samples for one instrument (repeatable)\n"
            "                        SPEC: instID:codec[@bitrate], e.g. 0x200:qoa\n"
            "  --bitrate N           Target bitrate in kbps (or bps) for lossy codecs\n"
            "  --gain DB             Apply gain (dB) to all sample split volumes (use with encoding, not after)\n"
            "  --resample SPEC       Resample samples to a target rate in Hz\n"
            "                        SPEC: rate (all), or index:rate (repeatable), e.g. 48000 or 3:32000\n"
            "  --set-meta F=V        Set metadata field F to value V (repeatable)\n"
            "  --clear-meta F        Clear metadata field F (repeatable)\n"
            "  --list-meta           List supported metadata fields\n"
            "  --loop-end N          Set MIDI loop end point (ms by default, or +N for ticks)\n"
            "  --loop-start N        Optional MIDI loop start (ms by default, or +N for ticks, default: 0)\n"
            "  --loop-count N        Loop count (-1=forever, default: -1)\n"
            "  --disable-loop        Disable MIDI loop markers\n"
            "  --trim N              Trim MIDI timeline to N (ms by default, or +N for ticks)\n"
            "  --unroll              Expand Beatnik/WebTV rolled MIDI loop playback to linear MIDI\n"
            "  --codecs              List available codecs and bitrates\n"
            "  --help, -h            Show this help\n",
            program_name);
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

static int is_zmf_path(const char *path)
{
    const char *ext;

    if (!path)
    {
        return 0;
    }

    ext = strrchr(path, '.');
    return (ext && (!strcmp(ext, ".zmf") || !strcmp(ext, ".ZMF"))) ? 1 : 0;
}

static int is_midi_path(const char *path)
{
    const char *ext;

    if (!path)
    {
        return 0;
    }

    ext = strrchr(path, '.');
    if (!ext)
    {
        return 0;
    }

    if (songtool_str_equal_ignore_case(ext, ".mid") ||
        songtool_str_equal_ignore_case(ext, ".midi"))
    {
        return 1;
    }

    return 0;
}

static int read_file_bytes(const char *path, unsigned char **outData, uint32_t *outSize)
{
    FILE *f;
    long fileSize;
    unsigned char *data;
    size_t bytesRead;

    if (!path || !outData || !outSize)
    {
        return 0;
    }

    *outData = NULL;
    *outSize = 0;

    f = fopen(path, "rb");
    if (!f)
    {
        return 0;
    }

    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return 0;
    }

    fileSize = ftell(f);
    if (fileSize <= 0)
    {
        fclose(f);
        return 0;
    }
    if ((uint64_t)fileSize > 0xFFFFFFFFULL)
    {
        fclose(f);
        return 0;
    }

    if (fseek(f, 0, SEEK_SET) != 0)
    {
        fclose(f);
        return 0;
    }

    data = (unsigned char *)malloc((size_t)fileSize);
    if (!data)
    {
        fclose(f);
        return 0;
    }

    bytesRead = fread(data, 1, (size_t)fileSize, f);
    fclose(f);
    if (bytesRead != (size_t)fileSize)
    {
        free(data);
        return 0;
    }

    *outData = data;
    *outSize = (uint32_t)fileSize;
    return 1;
}

static int write_file_bytes(const char *path, unsigned char const *data, uint32_t size)
{
    FILE *f;
    size_t bytesWritten;

    if (!path || !data || size == 0)
    {
        return 0;
    }

    f = fopen(path, "wb");
    if (!f)
    {
        return 0;
    }

    bytesWritten = fwrite(data, 1, (size_t)size, f);
    fclose(f);
    return (bytesWritten == (size_t)size) ? 1 : 0;
}

static int build_temp_midi_path(const char *destPath, char *outPath, size_t outPathSize)
{
    int written;

    if (!destPath || !outPath || outPathSize == 0)
    {
        return 0;
    }

    written = snprintf(outPath,
                       outPathSize,
                       "%s.songtool-unroll.tmp.mid",
                       destPath);
    if (written <= 0 || (size_t)written >= outPathSize)
    {
        return 0;
    }

    return 1;
}

#if SONGTOOL_ENABLE_ROLLED_MIDI_UNROLL == 1
static int unroll_midi_file_in_place(const char *path)
{
    unsigned char *srcMidi;
    uint32_t srcMidiSize;
    unsigned char *unrolledMidi;
    uint32_t unrolledMidiSize;
    BAE_BOOL wasRolled;
    BAEResult result;

    srcMidi = NULL;
    srcMidiSize = 0;
    unrolledMidi = NULL;
    unrolledMidiSize = 0;
    wasRolled = FALSE;

    if (!path)
    {
        return 0;
    }

    if (!read_file_bytes(path, &srcMidi, &srcMidiSize))
    {
        fprintf(stderr, "Error: failed to read MIDI output for unroll: %s\n", path);
        return 0;
    }

    result = BAEUtil_UnrollRolledMidiFromMemory(srcMidi,
                                                srcMidiSize,
                                                BAE_UNROLL_MIDI_OPTION_SPLIT_INSTRUMENTS,
                                                &unrolledMidi,
                                                &unrolledMidiSize,
                                                &wasRolled);
    free(srcMidi);

    if (result != BAE_NO_ERROR)
    {
        fprintf(stderr, "Error: MIDI unroll failed (%d)\n", (int)result);
        if (unrolledMidi)
        {
            BAERingtone_FreeMidiBuffer(unrolledMidi);
        }
        return 0;
    }

    if (!wasRolled || !unrolledMidi || unrolledMidiSize == 0)
    {
        if (unrolledMidi)
        {
            BAERingtone_FreeMidiBuffer(unrolledMidi);
        }
        fprintf(stderr, "Error: --unroll requested but no rolled MIDI loop controls were detected\n");
        return 0;
    }

    if (!write_file_bytes(path, unrolledMidi, unrolledMidiSize))
    {
        fprintf(stderr, "Error: failed to write unrolled MIDI: %s\n", path);
        BAERingtone_FreeMidiBuffer(unrolledMidi);
        return 0;
    }

    fprintf(stderr,
            "Unroll: expanded rolled MIDI (%u -> %u bytes)\n",
            (unsigned)srcMidiSize,
            (unsigned)unrolledMidiSize);
    BAERingtone_FreeMidiBuffer(unrolledMidi);
    return 1;
}

static int apply_unroll_to_document(BAERmfEditorDocument **ioDocument,
                                    const char *destPath)
{
    BAERmfEditorDocument *document;
    BAERmfEditorDocument *unrolledDocument;
    BAEResult result;
    char tempMidiPath[4096];

    if (!ioDocument || !*ioDocument || !destPath)
    {
        return 0;
    }

    document = *ioDocument;
    unrolledDocument = NULL;

    if (!BAERmfEditorDocument_CanSaveAsMidi(document))
    {
        fprintf(stderr,
                "Error: cannot apply --unroll because this document cannot be represented as standard MIDI\n");
        return 0;
    }

    if (!build_temp_midi_path(destPath, tempMidiPath, sizeof(tempMidiPath)))
    {
        fprintf(stderr, "Error: failed to build temporary path for MIDI unroll\n");
        return 0;
    }

    result = BAERmfEditorDocument_SaveAsMidi(document, (BAEPathName)tempMidiPath);
    if (result != BAE_NO_ERROR)
    {
        fprintf(stderr, "Error: failed to create temporary MIDI for unroll (%d)\n", (int)result);
        return 0;
    }

    if (!unroll_midi_file_in_place(tempMidiPath))
    {
        remove(tempMidiPath);
        return 0;
    }

    unrolledDocument = BAERmfEditorDocument_LoadFromFile((BAEPathName)tempMidiPath);
    remove(tempMidiPath);
    if (!unrolledDocument)
    {
        fprintf(stderr, "Error: failed to reload unrolled MIDI into editor document\n");
        return 0;
    }

    result = BAERmfEditorDocument_CopySamplesFrom(unrolledDocument, document);
    if (result != BAE_NO_ERROR)
    {
        fprintf(stderr,
                "Error: failed to copy RMF/ZMF samples after MIDI unroll (%d)\n",
                (int)result);
        BAERmfEditorDocument_Delete(unrolledDocument);
        return 0;
    }

    BAERmfEditorDocument_Delete(document);
    *ioDocument = unrolledDocument;
    return 1;
}
#endif

static uint32_t clamp_u64_to_u32(uint64_t v)
{
    return (v > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t)v;
}

static uint32_t ticks_to_microseconds(uint32_t ticks,
                                      uint32_t microsecondsPerQuarter,
                                      uint16_t ticksPerQuarter)
{
    uint64_t usec;

    if (ticksPerQuarter == 0)
    {
        return 0;
    }

    usec = ((uint64_t)ticks * (uint64_t)microsecondsPerQuarter) / (uint64_t)ticksPerQuarter;
    return clamp_u64_to_u32(usec);
}

static uint32_t microseconds_to_ticks(uint32_t microseconds,
                                      uint32_t microsecondsPerQuarter,
                                      uint16_t ticksPerQuarter)
{
    uint64_t ticks;

    if (microsecondsPerQuarter == 0)
    {
        return 0;
    }

    ticks = ((uint64_t)microseconds * (uint64_t)ticksPerQuarter) / (uint64_t)microsecondsPerQuarter;
    return clamp_u64_to_u32(ticks);
}

static int get_initial_tempo_us_per_quarter(BAERmfEditorDocument const *document,
                                            uint32_t *outTempo)
{
    uint32_t tempoEventCount;
    uint32_t i;
    int foundTickZeroTempo;
    uint32_t bpm;
    BAEResult result;

    if (!document || !outTempo)
    {
        return 0;
    }

    bpm = 120;
    result = BAERmfEditorDocument_GetTempoBPM(document, &bpm);
    if (result != BAE_NO_ERROR || bpm == 0)
    {
        bpm = 120;
    }

    *outTempo = 60000000U / bpm;
    if (*outTempo == 0)
    {
        *outTempo = 500000U;
    }

    /* Prefer explicit tempo events at tick 0 over document BPM, since
     * document BPM can drift from actual map after destructive edits. */
    tempoEventCount = 0;
    (void)BAERmfEditorDocument_GetTempoEventCount(document, &tempoEventCount);
    foundTickZeroTempo = 0;
    for (i = 0; i < tempoEventCount; ++i)
    {
        uint32_t eventTick;
        uint32_t eventTempo;

        result = BAERmfEditorDocument_GetTempoEvent(document, i, &eventTick, &eventTempo);
        if (result != BAE_NO_ERROR || eventTempo == 0)
        {
            continue;
        }
        if (eventTick == 0)
        {
            *outTempo = eventTempo;
            foundTickZeroTempo = 1;
        }
        else if (foundTickZeroTempo)
        {
            break;
        }
    }

    return 1;
}

static int milliseconds_to_song_ticks(BAERmfEditorDocument const *document,
                                      uint64_t milliseconds,
                                      uint32_t *outTick)
{
    uint16_t ticksPerQuarter;
    uint32_t tempoEventCount;
    uint32_t currentTick;
    uint32_t currentTempo;
    uint64_t remainingUsec;
    uint32_t i;
    BAEResult result;

    if (!document || !outTick)
    {
        return 0;
    }

    ticksPerQuarter = 0;
    result = BAERmfEditorDocument_GetTicksPerQuarter(document, &ticksPerQuarter);
    if (result != BAE_NO_ERROR || ticksPerQuarter == 0)
    {
        ticksPerQuarter = 480;
    }

    if (!get_initial_tempo_us_per_quarter(document, &currentTempo))
    {
        return 0;
    }

    remainingUsec = milliseconds * 1000ULL;
    currentTick = 0;

    tempoEventCount = 0;
    (void)BAERmfEditorDocument_GetTempoEventCount(document, &tempoEventCount);

    for (i = 0; i < tempoEventCount; ++i)
    {
        uint32_t eventTick;
        uint32_t eventTempo;

        result = BAERmfEditorDocument_GetTempoEvent(document, i, &eventTick, &eventTempo);
        if (result != BAE_NO_ERROR || eventTempo == 0)
        {
            continue;
        }

        if (eventTick < currentTick)
        {
            continue;
        }

        if (eventTick > currentTick)
        {
            uint32_t deltaTicks = eventTick - currentTick;
            uint64_t segUsec = (uint64_t)ticks_to_microseconds(deltaTicks,
                                                               currentTempo,
                                                               ticksPerQuarter);
            if (remainingUsec <= segUsec)
            {
                uint32_t advance = microseconds_to_ticks(clamp_u64_to_u32(remainingUsec),
                                                         currentTempo,
                                                         ticksPerQuarter);
                *outTick = currentTick + advance;
                return 1;
            }

            remainingUsec -= segUsec;
            currentTick = eventTick;
        }

        currentTempo = eventTempo;
    }

    *outTick = currentTick + microseconds_to_ticks(clamp_u64_to_u32(remainingUsec),
                                                   currentTempo,
                                                   ticksPerQuarter);
    return 1;
}

static int song_ticks_to_milliseconds(BAERmfEditorDocument const *document,
                                      uint32_t targetTick,
                                      uint64_t *outMs)
{
    uint16_t ticksPerQuarter;
    uint32_t tempoEventCount;
    uint32_t currentTick;
    uint32_t currentTempo;
    uint64_t accumulatedUsec;
    uint32_t i;
    BAEResult result;

    if (!document || !outMs)
    {
        return 0;
    }

    ticksPerQuarter = 0;
    result = BAERmfEditorDocument_GetTicksPerQuarter(document, &ticksPerQuarter);
    if (result != BAE_NO_ERROR || ticksPerQuarter == 0)
    {
        ticksPerQuarter = 480;
    }

    if (!get_initial_tempo_us_per_quarter(document, &currentTempo))
    {
        return 0;
    }

    accumulatedUsec = 0;
    currentTick = 0;

    tempoEventCount = 0;
    (void)BAERmfEditorDocument_GetTempoEventCount(document, &tempoEventCount);

    for (i = 0; i < tempoEventCount; ++i)
    {
        uint32_t eventTick;
        uint32_t eventTempo;

        result = BAERmfEditorDocument_GetTempoEvent(document, i, &eventTick, &eventTempo);
        if (result != BAE_NO_ERROR || eventTempo == 0)
        {
            continue;
        }

        if (eventTick < currentTick)
        {
            continue;
        }

        if (eventTick >= targetTick)
        {
            break;
        }

        accumulatedUsec += (uint64_t)ticks_to_microseconds(eventTick - currentTick,
                                                           currentTempo,
                                                           ticksPerQuarter);
        currentTick = eventTick;
        currentTempo = eventTempo;
    }

    if (targetTick > currentTick)
    {
        accumulatedUsec += (uint64_t)ticks_to_microseconds(targetTick - currentTick,
                                                           currentTempo,
                                                           ticksPerQuarter);
    }

    *outMs = accumulatedUsec / 1000ULL;
    return 1;
}

static uint32_t get_song_end_tick(BAERmfEditorDocument const *document)
{
    uint16_t trackCount;
    uint16_t t;
    uint32_t maxTick;
    BAEResult result;

    maxTick = 0;
    trackCount = 0;
    result = BAERmfEditorDocument_GetTrackCount(document, &trackCount);
    if (result != BAE_NO_ERROR || trackCount == 0)
    {
        return 0;
    }

    for (t = 0; t < trackCount; ++t)
    {
        uint32_t endTick;
        result = BAERmfEditorDocument_GetTrackEndOfTrackTick(document, t, &endTick);
        if (result == BAE_NO_ERROR && endTick > maxTick)
        {
            maxTick = endTick;
        }
    }

    return maxTick;
}

static int is_opus_compression_type(BAERmfEditorCompressionType ct)
{
    switch (ct)
    {
        case BAE_EDITOR_COMPRESSION_OPUS_12K:
        case BAE_EDITOR_COMPRESSION_OPUS_16K:
        case BAE_EDITOR_COMPRESSION_OPUS_24K:
        case BAE_EDITOR_COMPRESSION_OPUS_32K:
        case BAE_EDITOR_COMPRESSION_OPUS_48K:
        case BAE_EDITOR_COMPRESSION_OPUS_64K:
        case BAE_EDITOR_COMPRESSION_OPUS_80K:
        case BAE_EDITOR_COMPRESSION_OPUS_96K:
        case BAE_EDITOR_COMPRESSION_OPUS_128K:
        case BAE_EDITOR_COMPRESSION_OPUS_160K:
        case BAE_EDITOR_COMPRESSION_OPUS_192K:
        case BAE_EDITOR_COMPRESSION_OPUS_256K:
            return 1;
        default:
            break;
    }
    return 0;
}

static int parse_snd_storage_type(const char *str, BAERmfEditorSndStorageType *outType)
{
    if (!str || !outType)
    {
        return 0;
    }

    if (songtool_str_equal_ignore_case(str, "SND"))
    {
        *outType = BAE_EDITOR_SND_STORAGE_SND;
        return 1;
    }
    if (songtool_str_equal_ignore_case(str, "CSND"))
    {
        *outType = BAE_EDITOR_SND_STORAGE_CSND;
        return 1;
    }
    if (songtool_str_equal_ignore_case(str, "ESND"))
    {
        *outType = BAE_EDITOR_SND_STORAGE_ESND;
        return 1;
    }

    return 0;
}

static const char *songtool_snd_storage_name(BAERmfEditorSndStorageType storageType)
{
    switch (storageType)
    {
        case BAE_EDITOR_SND_STORAGE_SND:
            return "SND";
        case BAE_EDITOR_SND_STORAGE_CSND:
            return "CSND";
        case BAE_EDITOR_SND_STORAGE_ESND:
        default:
            return "ESND";
    }
}

static int apply_storage_to_all_samples(BAERmfEditorDocument *document,
                                        BAERmfEditorSndStorageType storageType)
{
    uint32_t sampleCount;
    uint32_t i;
    uint32_t applied;

    if (!document)
    {
        return 0;
    }

    sampleCount = 0;
    if (BAERmfEditorDocument_GetSampleCount(document, &sampleCount) != BAE_NO_ERROR)
    {
        return 0;
    }

    applied = 0;
    for (i = 0; i < sampleCount; ++i)
    {
        BAERmfEditorSampleInfo info;
        BAEResult result;

        result = BAERmfEditorDocument_GetSampleInfo(document, i, &info);
        if (result != BAE_NO_ERROR)
        {
            fprintf(stderr, "Warning: failed to get sample info for sample %u (%d)\n",
                    (unsigned)i, (int)result);
            continue;
        }

        info.sndStorageType = storageType;
        result = BAERmfEditorDocument_SetSampleInfo(document, i, &info);
        if (result != BAE_NO_ERROR)
        {
            fprintf(stderr, "Warning: failed to set sample storage for sample %u: %d\n",
                    (unsigned)i, (int)result);
            continue;
        }

        ++applied;
    }

    fprintf(stderr,
            "Storage: %s applied to %u/%u samples\n",
            songtool_snd_storage_name(storageType),
            (unsigned)applied,
            (unsigned)sampleCount);
    return 1;
}

static int apply_compression_to_all_samples(BAERmfEditorDocument *document,
                                            BAERmfEditorCompressionType compressionType)
{
    uint32_t sampleCount;
    uint32_t i;
    uint32_t applied;

    if (!document)
    {
        return 0;
    }

    sampleCount = 0;
    if (BAERmfEditorDocument_GetSampleCount(document, &sampleCount) != BAE_NO_ERROR)
    {
        return 0;
    }

    applied = 0;
    for (i = 0; i < sampleCount; ++i)
    {
        BAERmfEditorSampleInfo info;
        BAEResult result;

        result = BAERmfEditorDocument_GetSampleInfo(document, i, &info);
        if (result != BAE_NO_ERROR)
        {
            fprintf(stderr, "Warning: failed to get sample info for sample %u (%d)\n",
                    (unsigned)i, (int)result);
            continue;
        }

        info.compressionType = compressionType;
        if (is_opus_compression_type(compressionType))
        {
            info.opusMode = BAE_EDITOR_OPUS_MODE_AUDIO;
            info.opusRoundTripResample = TRUE;
        }
        else
        {
            info.opusRoundTripResample = FALSE;
        }

        result = BAERmfEditorDocument_SetSampleInfo(document, i, &info);
        if (result != BAE_NO_ERROR)
        {
            fprintf(stderr, "Warning: failed to set sample info for sample %u: %d\n",
                    (unsigned)i, (int)result);
            continue;
        }

        ++applied;
    }

    fprintf(stderr,
            "Compression: %s%s applied to %u/%u samples\n",
            mod2rmf_encoder_label(compressionType),
            is_opus_compression_type(compressionType) ? " (round-trip)" : "",
            (unsigned)applied,
            (unsigned)sampleCount);
    return 1;
}

static int parse_sample_codec_spec(const char *spec,
                                   SongtoolSampleOverride *outOverride)
{
    const char *colon;
    const char *at;
    char indexBuf[32];
    char codecBuf[64];
    char bitrateBuf[32];
    char *end;
    unsigned long idx;
    size_t indexLen;
    size_t codecLen;

    if (!spec || !outOverride)
    {
        return 0;
    }

    colon = strchr(spec, ':');
    if (!colon)
    {
        return 0;
    }

    indexLen = (size_t)(colon - spec);
    if (indexLen == 0 || indexLen >= sizeof(indexBuf))
    {
        return 0;
    }
    memcpy(indexBuf, spec, indexLen);
    indexBuf[indexLen] = '\0';

    idx = strtoul(indexBuf, &end, 10);
    if (end == indexBuf || *end != '\0')
    {
        return 0;
    }

    at = strchr(colon + 1, '@');
    if (at)
    {
        codecLen = (size_t)(at - (colon + 1));
    }
    else
    {
        codecLen = strlen(colon + 1);
    }
    if (codecLen == 0 || codecLen >= sizeof(codecBuf))
    {
        return 0;
    }
    memcpy(codecBuf, colon + 1, codecLen);
    codecBuf[codecLen] = '\0';

    mod2rmf_encoder_defaults(&outOverride->settings);
    if (mod2rmf_encoder_parse_codec(codecBuf, &outOverride->settings.codec) != 0)
    {
        return 0;
    }

    if (at)
    {
        size_t bitrateLen = strlen(at + 1);
        if (bitrateLen == 0 || bitrateLen >= sizeof(bitrateBuf))
        {
            return 0;
        }
        memcpy(bitrateBuf, at + 1, bitrateLen);
        bitrateBuf[bitrateLen] = '\0';

        if (mod2rmf_encoder_parse_bitrate(bitrateBuf, &outOverride->settings.bitrateKbps) != 0)
        {
            return 0;
        }
    }

    outOverride->sampleIndex = (uint32_t)idx;
    outOverride->compressionType = mod2rmf_encoder_resolve(&outOverride->settings);
    return 1;
}

static int parse_instrument_codec_spec(const char *spec,
                                       SongtoolInstrumentOverride *outOverride)
{
    const char *colon;
    const char *at;
    char instBuf[32];
    char codecBuf[64];
    char bitrateBuf[32];
    char *end;
    unsigned long instID;
    size_t instLen;
    size_t codecLen;

    if (!spec || !outOverride)
    {
        return 0;
    }

    colon = strchr(spec, ':');
    if (!colon)
    {
        return 0;
    }

    instLen = (size_t)(colon - spec);
    if (instLen == 0 || instLen >= sizeof(instBuf))
    {
        return 0;
    }
    memcpy(instBuf, spec, instLen);
    instBuf[instLen] = '\0';

    instID = strtoul(instBuf, &end, 0);
    if (end == instBuf || *end != '\0')
    {
        return 0;
    }

    at = strchr(colon + 1, '@');
    if (at)
    {
        codecLen = (size_t)(at - (colon + 1));
    }
    else
    {
        codecLen = strlen(colon + 1);
    }
    if (codecLen == 0 || codecLen >= sizeof(codecBuf))
    {
        return 0;
    }
    memcpy(codecBuf, colon + 1, codecLen);
    codecBuf[codecLen] = '\0';

    mod2rmf_encoder_defaults(&outOverride->settings);
    if (mod2rmf_encoder_parse_codec(codecBuf, &outOverride->settings.codec) != 0)
    {
        return 0;
    }

    if (at)
    {
        size_t bitrateLen = strlen(at + 1);
        if (bitrateLen == 0 || bitrateLen >= sizeof(bitrateBuf))
        {
            return 0;
        }
        memcpy(bitrateBuf, at + 1, bitrateLen);
        bitrateBuf[bitrateLen] = '\0';

        if (mod2rmf_encoder_parse_bitrate(bitrateBuf, &outOverride->settings.bitrateKbps) != 0)
        {
            return 0;
        }
    }

    outOverride->instID = (uint32_t)instID;
    outOverride->compressionType = mod2rmf_encoder_resolve(&outOverride->settings);
    return 1;
}

static int apply_sample_overrides(BAERmfEditorDocument *document,
                                  SongtoolSampleOverride const *overrides,
                                  uint32_t overrideCount)
{
    uint32_t i;
    uint32_t sampleCount;

    if (!document || !overrides)
    {
        return 0;
    }

    sampleCount = 0;
    if (BAERmfEditorDocument_GetSampleCount(document, &sampleCount) != BAE_NO_ERROR)
    {
        return 0;
    }

    for (i = 0; i < overrideCount; ++i)
    {
        BAERmfEditorSampleInfo info;
        BAEResult result;

        if (overrides[i].sampleIndex >= sampleCount)
        {
            fprintf(stderr,
                    "Error: sample index %u out of range (sample count: %u)\n",
                    (unsigned)overrides[i].sampleIndex,
                    (unsigned)sampleCount);
            return 0;
        }

        result = BAERmfEditorDocument_GetSampleInfo(document,
                                                    overrides[i].sampleIndex,
                                                    &info);
        if (result != BAE_NO_ERROR)
        {
            fprintf(stderr,
                    "Error: failed to get sample info for sample %u (%d)\n",
                    (unsigned)overrides[i].sampleIndex,
                    (int)result);
            return 0;
        }

        info.compressionType = overrides[i].compressionType;
        if (is_opus_compression_type(overrides[i].compressionType))
        {
            info.opusMode = BAE_EDITOR_OPUS_MODE_AUDIO;
            info.opusRoundTripResample = TRUE;
        }
        else
        {
            info.opusRoundTripResample = FALSE;
        }

        result = BAERmfEditorDocument_SetSampleInfo(document,
                                                    overrides[i].sampleIndex,
                                                    &info);
        if (result != BAE_NO_ERROR)
        {
            fprintf(stderr,
                    "Error: failed to set sample info for sample %u (%d)\n",
                    (unsigned)overrides[i].sampleIndex,
                    (int)result);
            return 0;
        }

        fprintf(stderr,
                "Sample %u codec override: %s%s\n",
                (unsigned)overrides[i].sampleIndex,
                mod2rmf_encoder_label(overrides[i].compressionType),
                is_opus_compression_type(overrides[i].compressionType) ? " (round-trip)" : "");
    }

    return 1;
}

static int apply_instrument_overrides(BAERmfEditorDocument *document,
                                      SongtoolInstrumentOverride const *overrides,
                                      uint32_t overrideCount)
{
    uint32_t sampleCount;
    uint32_t i;

    if (!document || !overrides)
    {
        return 0;
    }

    sampleCount = 0;
    if (BAERmfEditorDocument_GetSampleCount(document, &sampleCount) != BAE_NO_ERROR)
    {
        return 0;
    }

    for (i = 0; i < overrideCount; ++i)
    {
        uint32_t sampleIndex;
        uint32_t appliedCount;

        appliedCount = 0;
        for (sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
        {
            uint32_t instID;
            BAERmfEditorSampleInfo info;
            BAEResult result;

            result = BAERmfEditorDocument_GetInstIDForSample(document, sampleIndex, &instID);
            if (result != BAE_NO_ERROR)
            {
                fprintf(stderr,
                        "Error: failed to get instrument ID for sample %u (%d)\n",
                        (unsigned)sampleIndex,
                        (int)result);
                return 0;
            }
            if (instID != overrides[i].instID)
            {
                continue;
            }

            result = BAERmfEditorDocument_GetSampleInfo(document, sampleIndex, &info);
            if (result != BAE_NO_ERROR)
            {
                fprintf(stderr,
                        "Error: failed to get sample info for sample %u (%d)\n",
                        (unsigned)sampleIndex,
                        (int)result);
                return 0;
            }

            info.compressionType = overrides[i].compressionType;
            if (is_opus_compression_type(overrides[i].compressionType))
            {
                info.opusMode = BAE_EDITOR_OPUS_MODE_AUDIO;
                info.opusRoundTripResample = TRUE;
            }
            else
            {
                info.opusRoundTripResample = FALSE;
            }

            result = BAERmfEditorDocument_SetSampleInfo(document, sampleIndex, &info);
            if (result != BAE_NO_ERROR)
            {
                fprintf(stderr,
                        "Error: failed to set sample info for sample %u (%d)\n",
                        (unsigned)sampleIndex,
                        (int)result);
                return 0;
            }
            ++appliedCount;
        }

        if (appliedCount == 0)
        {
            fprintf(stderr,
                    "Error: instrument override target 0x%X matched no samples\n",
                    (unsigned)overrides[i].instID);
            return 0;
        }

        fprintf(stderr,
                "Instrument 0x%X codec override: %s%s applied to %u sample(s)\n",
                (unsigned)overrides[i].instID,
                mod2rmf_encoder_label(overrides[i].compressionType),
                is_opus_compression_type(overrides[i].compressionType) ? " (round-trip)" : "",
                (unsigned)appliedCount);
    }

    return 1;
}

static int apply_gain_to_all_samples(BAERmfEditorDocument *document,
                                     double gainDb)
{
    uint32_t sampleCount;
    uint32_t i;
    uint32_t appliedSamples;
    uint32_t skippedUnsupported;
    double linearScale;

    if (!document)
    {
        return 0;
    }

    sampleCount = 0;
    if (BAERmfEditorDocument_GetSampleCount(document, &sampleCount) != BAE_NO_ERROR)
    {
        return 0;
    }

    linearScale = pow(10.0, gainDb / 20.0);
    if (linearScale < 0.0)
    {
        linearScale = 0.0;
    }

    appliedSamples = 0;
    skippedUnsupported = 0;
    for (i = 0; i < sampleCount; ++i)
    {
        BAERmfEditorSampleInfo infoBefore;
        BAERmfEditorSampleInfo infoAfter;
        BAESampleInfo replacedInfo;
        void const *waveData;
        uint32_t frameCount;
        uint16_t bitSize;
        uint16_t channels;
        BAE_UNSIGNED_FIXED sampledRate;
        uint32_t sampleCountTotal;
        uint32_t bytesPerSample;
        uint32_t totalBytes;
        uint8_t *scaledPcm;
        uint32_t s;
        BAEResult result;

        result = BAERmfEditorDocument_GetSampleInfo(document, i, &infoBefore);
        if (result != BAE_NO_ERROR)
        {
            fprintf(stderr, "Warning: failed to get sample info for sample %u (%d)\n",
                    (unsigned)i, (int)result);
            continue;
        }

        result = BAERmfEditorDocument_GetSampleWaveformData(document,
                                                            i,
                                                            &waveData,
                                                            &frameCount,
                                                            &bitSize,
                                                            &channels,
                                                            &sampledRate);
        if (result != BAE_NO_ERROR)
        {
            fprintf(stderr, "Warning: failed to get waveform data for sample %u (%d)\n",
                    (unsigned)i, (int)result);
            continue;
        }

        if ((bitSize != 8 && bitSize != 16) || (channels != 1 && channels != 2) || frameCount == 0)
        {
            ++skippedUnsupported;
            continue;
        }

        sampleCountTotal = frameCount * (uint32_t)channels;
        bytesPerSample = (uint32_t)(bitSize / 8u);
        totalBytes = sampleCountTotal * bytesPerSample;
        if (sampleCountTotal == 0 || bytesPerSample == 0 || totalBytes / bytesPerSample != sampleCountTotal)
        {
            continue;
        }

        scaledPcm = (uint8_t *)malloc(totalBytes);
        if (!scaledPcm)
        {
            fprintf(stderr, "Error: out of memory while applying gain\n");
            return 0;
        }

        if (bitSize == 8)
        {
            uint8_t const *src = (uint8_t const *)waveData;
            uint8_t *dst = (uint8_t *)scaledPcm;
            for (s = 0; s < sampleCountTotal; ++s)
            {
                int centered = (int)src[s] - 128;
                int scaled = (centered >= 0)
                           ? (int)(centered * linearScale + 0.5)
                           : (int)(centered * linearScale - 0.5);
                int out = scaled + 128;
                if (out < 0) out = 0;
                if (out > 255) out = 255;
                dst[s] = (uint8_t)out;
            }
        }
        else
        {
            int16_t const *src = (int16_t const *)waveData;
            int16_t *dst = (int16_t *)scaledPcm;
            for (s = 0; s < sampleCountTotal; ++s)
            {
                int32_t out;
                if (src[s] >= 0)
                {
                    out = (int32_t)(src[s] * linearScale + 0.5);
                }
                else
                {
                    out = (int32_t)(src[s] * linearScale - 0.5);
                }
                if (out < -32768) out = -32768;
                if (out > 32767) out = 32767;
                dst[s] = (int16_t)out;
            }
        }

        result = BAERmfEditorDocument_ReplaceSampleFromPCM(document,
                                                           i,
                                                           scaledPcm,
                                                           frameCount,
                                                           bitSize,
                                                           channels,
                                                           sampledRate,
                                                           infoBefore.sampleInfo.startLoop,
                                                           infoBefore.sampleInfo.endLoop,
                                                           &replacedInfo);
        free(scaledPcm);

        if (result != BAE_NO_ERROR)
        {
            fprintf(stderr, "Warning: failed to replace PCM for sample %u (%d)\n",
                    (unsigned)i, (int)result);
            continue;
        }

        /* ReplaceSampleFromPCM resets target compression to PCM.
         * Restore the original per-sample encoding intent so save-time
         * encoding still uses the user's codec settings. */
        result = BAERmfEditorDocument_GetSampleInfo(document, i, &infoAfter);
        if (result == BAE_NO_ERROR)
        {
            infoAfter.compressionType = infoBefore.compressionType;
            infoAfter.opusMode = infoBefore.opusMode;
            infoAfter.opusRoundTripResample = infoBefore.opusRoundTripResample;
            (void)BAERmfEditorDocument_SetSampleInfo(document, i, &infoAfter);
        }

        ++appliedSamples;
    }

    fprintf(stderr,
            "Gain: %+0.2f dB applied to %u/%u samples (%u skipped unsupported formats)\n",
            gainDb,
            (unsigned)appliedSamples,
            (unsigned)sampleCount,
            (unsigned)skippedUnsupported);
    return 1;
}

static int parse_sample_rate_hz(const char *text, uint32_t *outRateHz)
{
    char *end = NULL;
    unsigned long value;

    if (!text || !outRateHz)
    {
        return 0;
    }

    value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value == 0 || value > 65535UL)
    {
        return 0;
    }

    *outRateHz = (uint32_t)value;
    return 1;
}

static int parse_resample_spec(const char *spec,
                               uint32_t *outSampleIndex,
                               uint32_t *outRateHz,
                               int *outHasSampleIndex)
{
    const char *colon;
    char indexBuf[32];
    char rateBuf[32];
    size_t indexLen;
    size_t rateLen;
    char *end = NULL;
    unsigned long indexValue;

    if (!spec || !outRateHz || !outHasSampleIndex)
    {
        return 0;
    }

    colon = strchr(spec, ':');
    if (!colon)
    {
        *outHasSampleIndex = 0;
        return parse_sample_rate_hz(spec, outRateHz);
    }

    indexLen = (size_t)(colon - spec);
    rateLen = strlen(colon + 1);
    if (indexLen == 0 || indexLen >= sizeof(indexBuf) ||
        rateLen == 0 || rateLen >= sizeof(rateBuf))
    {
        return 0;
    }

    memcpy(indexBuf, spec, indexLen);
    indexBuf[indexLen] = '\0';
    memcpy(rateBuf, colon + 1, rateLen);
    rateBuf[rateLen] = '\0';

    indexValue = strtoul(indexBuf, &end, 10);
    if (end == indexBuf || *end != '\0')
    {
        return 0;
    }

    if (!parse_sample_rate_hz(rateBuf, outRateHz))
    {
        return 0;
    }

    *outHasSampleIndex = 1;
    if (outSampleIndex)
    {
        *outSampleIndex = (uint32_t)indexValue;
    }
    return 1;
}

static int resample_one_sample(BAERmfEditorDocument *document,
                               uint32_t sampleIndex,
                               uint32_t targetRateHz,
                               int *outApplied,
                               int *outUnsupported)
{
    BAERmfEditorSampleInfo infoBefore;
    BAERmfEditorSampleInfo infoAfter;
    BAESampleInfo replacedInfo;
    void const *waveData;
    uint32_t frameCount;
    uint16_t bitSize;
    uint16_t channels;
    BAE_UNSIGNED_FIXED sampledRate;
    uint32_t sourceRateHz;
    uint32_t newFrameCount;
    uint32_t bytesPerSample;
    uint32_t totalSamples;
    uint32_t totalBytes;
    uint8_t *resampled;
    uint32_t outFrame;
    uint32_t c;
    uint32_t originalStartLoop;
    uint32_t originalEndLoop;
    uint32_t startLoop;
    uint32_t endLoop;
    BAEResult result;

    if (!document)
    {
        return 0;
    }
    if (outApplied)
    {
        *outApplied = 0;
    }
    if (outUnsupported)
    {
        *outUnsupported = 0;
    }

    result = BAERmfEditorDocument_GetSampleInfo(document, sampleIndex, &infoBefore);
    if (result != BAE_NO_ERROR)
    {
        fprintf(stderr,
                "Warning: failed to get sample info for sample %u (%d)\n",
                (unsigned)sampleIndex,
                (int)result);
        return 1;
    }

    result = BAERmfEditorDocument_GetSampleWaveformData(document,
                                                        sampleIndex,
                                                        &waveData,
                                                        &frameCount,
                                                        &bitSize,
                                                        &channels,
                                                        &sampledRate);
    if (result != BAE_NO_ERROR)
    {
        fprintf(stderr,
                "Warning: failed to get waveform data for sample %u (%d)\n",
                (unsigned)sampleIndex,
                (int)result);
        return 1;
    }

    if ((bitSize != 8 && bitSize != 16) || (channels != 1 && channels != 2) || frameCount == 0)
    {
        if (outUnsupported)
        {
            *outUnsupported = 1;
        }
        return 1;
    }

    sourceRateHz = (uint32_t)((sampledRate + 32768u) >> 16);
    if (sourceRateHz == 0 || sourceRateHz == targetRateHz)
    {
        return 1;
    }

    newFrameCount = (uint32_t)((((uint64_t)frameCount * (uint64_t)targetRateHz) +
                                 ((uint64_t)sourceRateHz / 2u)) /
                                (uint64_t)sourceRateHz);
    if (newFrameCount == 0)
    {
        newFrameCount = 1;
    }

    bytesPerSample = (uint32_t)(bitSize / 8u);
    totalSamples = newFrameCount * (uint32_t)channels;
    totalBytes = totalSamples * bytesPerSample;
    if (bytesPerSample == 0 || totalSamples == 0 || totalBytes / bytesPerSample != totalSamples)
    {
        fprintf(stderr,
                "Warning: resample size overflow for sample %u\n",
                (unsigned)sampleIndex);
        return 1;
    }

    resampled = (uint8_t *)malloc(totalBytes);
    if (!resampled)
    {
        fprintf(stderr, "Error: out of memory while resampling\n");
        return 0;
    }

    if (bitSize == 8)
    {
        uint8_t const *src = (uint8_t const *)waveData;
        uint8_t *dst = (uint8_t *)resampled;

        for (outFrame = 0; outFrame < newFrameCount; ++outFrame)
        {
            uint64_t posQ16;
            uint32_t srcIndex;
            uint32_t frac;

            if (newFrameCount <= 1 || frameCount <= 1)
            {
                srcIndex = 0;
                frac = 0;
            }
            else
            {
                posQ16 = ((uint64_t)outFrame * (uint64_t)(frameCount - 1u) << 16u) /
                         (uint64_t)(newFrameCount - 1u);
                srcIndex = (uint32_t)(posQ16 >> 16u);
                frac = (uint32_t)(posQ16 & 0xFFFFu);
            }

            for (c = 0; c < (uint32_t)channels; ++c)
            {
                uint32_t srcA = srcIndex;
                uint32_t srcB = (srcIndex + 1u < frameCount) ? (srcIndex + 1u) : srcIndex;
                int32_t a = (int32_t)src[srcA * (uint32_t)channels + c] - 128;
                int32_t b = (int32_t)src[srcB * (uint32_t)channels + c] - 128;
                int32_t mixed = (int32_t)((((int64_t)a * (int64_t)(65536u - frac)) +
                                           ((int64_t)b * (int64_t)frac) +
                                           32768) >> 16);
                int32_t out = mixed + 128;
                if (out < 0) out = 0;
                if (out > 255) out = 255;
                dst[outFrame * (uint32_t)channels + c] = (uint8_t)out;
            }
        }
    }
    else
    {
        int16_t const *src = (int16_t const *)waveData;
        int16_t *dst = (int16_t *)resampled;

        for (outFrame = 0; outFrame < newFrameCount; ++outFrame)
        {
            uint64_t posQ16;
            uint32_t srcIndex;
            uint32_t frac;

            if (newFrameCount <= 1 || frameCount <= 1)
            {
                srcIndex = 0;
                frac = 0;
            }
            else
            {
                posQ16 = ((uint64_t)outFrame * (uint64_t)(frameCount - 1u) << 16u) /
                         (uint64_t)(newFrameCount - 1u);
                srcIndex = (uint32_t)(posQ16 >> 16u);
                frac = (uint32_t)(posQ16 & 0xFFFFu);
            }

            for (c = 0; c < (uint32_t)channels; ++c)
            {
                uint32_t srcA = srcIndex;
                uint32_t srcB = (srcIndex + 1u < frameCount) ? (srcIndex + 1u) : srcIndex;
                int32_t a = (int32_t)src[srcA * (uint32_t)channels + c];
                int32_t b = (int32_t)src[srcB * (uint32_t)channels + c];
                int32_t mixed = (int32_t)((((int64_t)a * (int64_t)(65536u - frac)) +
                                           ((int64_t)b * (int64_t)frac) +
                                           32768) >> 16);
                if (mixed < -32768) mixed = -32768;
                if (mixed > 32767) mixed = 32767;
                dst[outFrame * (uint32_t)channels + c] = (int16_t)mixed;
            }
        }
    }

    originalStartLoop = infoBefore.sampleInfo.startLoop;
    originalEndLoop = infoBefore.sampleInfo.endLoop;
    startLoop = originalStartLoop;
    endLoop = originalEndLoop;
    if (startLoop > frameCount)
    {
        startLoop = frameCount;
    }
    if (endLoop > frameCount)
    {
        endLoop = frameCount;
    }
    if (frameCount > 0)
    {
        startLoop = (uint32_t)((((uint64_t)startLoop * (uint64_t)newFrameCount) +
                                ((uint64_t)frameCount / 2u)) /
                               (uint64_t)frameCount);
        endLoop = (uint32_t)((((uint64_t)endLoop * (uint64_t)newFrameCount) +
                              ((uint64_t)frameCount / 2u)) /
                             (uint64_t)frameCount);
    }

    if (startLoop > newFrameCount)
    {
        startLoop = newFrameCount;
    }
    if (endLoop > newFrameCount)
    {
        endLoop = newFrameCount;
    }

    /* Preserve a valid non-empty loop when source had one, even after heavy
     * downsampling where rounded points may collapse to the same frame. */
    if (originalEndLoop > originalStartLoop && endLoop <= startLoop)
    {
        if (startLoop < newFrameCount)
        {
            endLoop = startLoop + 1u;
        }
        else if (newFrameCount > 0)
        {
            startLoop = newFrameCount - 1u;
            endLoop = newFrameCount;
        }
    }

    result = BAERmfEditorDocument_ReplaceSampleFromPCM(document,
                                                       sampleIndex,
                                                       resampled,
                                                       newFrameCount,
                                                       bitSize,
                                                       channels,
                                                       (BAE_UNSIGNED_FIXED)(targetRateHz << 16u),
                                                       startLoop,
                                                       endLoop,
                                                       &replacedInfo);
    free(resampled);

    if (result != BAE_NO_ERROR)
    {
        fprintf(stderr,
                "Warning: failed to replace PCM for sample %u (%d)\n",
                (unsigned)sampleIndex,
                (int)result);
        return 1;
    }

    result = BAERmfEditorDocument_GetSampleInfo(document, sampleIndex, &infoAfter);
    if (result == BAE_NO_ERROR)
    {
        infoAfter.compressionType = infoBefore.compressionType;
        infoAfter.opusMode = infoBefore.opusMode;
        infoAfter.opusRoundTripResample = infoBefore.opusRoundTripResample;
        (void)BAERmfEditorDocument_SetSampleInfo(document, sampleIndex, &infoAfter);
    }

    if (outApplied)
    {
        *outApplied = 1;
    }
    return 1;
}

static int apply_resample_to_all_samples(BAERmfEditorDocument *document,
                                         uint32_t rateHz)
{
    uint32_t sampleCount;
    uint32_t i;
    uint32_t applied;
    uint32_t skippedUnsupported;

    if (!document)
    {
        return 0;
    }

    sampleCount = 0;
    if (BAERmfEditorDocument_GetSampleCount(document, &sampleCount) != BAE_NO_ERROR)
    {
        return 0;
    }

    applied = 0;
    skippedUnsupported = 0;
    for (i = 0; i < sampleCount; ++i)
    {
        int sampleApplied;
        int sampleUnsupported;

        if (!resample_one_sample(document, i, rateHz, &sampleApplied, &sampleUnsupported))
        {
            return 0;
        }
        if (sampleApplied)
        {
            ++applied;
        }
        if (sampleUnsupported)
        {
            ++skippedUnsupported;
        }
    }

    fprintf(stderr,
            "Resample: %u Hz applied to %u/%u samples (%u skipped unsupported formats)\n",
            (unsigned)rateHz,
            (unsigned)applied,
            (unsigned)sampleCount,
            (unsigned)skippedUnsupported);
    return 1;
}

static int apply_resample_targets(BAERmfEditorDocument *document,
                                  SongtoolResampleTarget const *targets,
                                  uint32_t targetCount)
{
    uint32_t sampleCount;
    uint32_t i;

    if (!document || !targets)
    {
        return 0;
    }

    sampleCount = 0;
    if (BAERmfEditorDocument_GetSampleCount(document, &sampleCount) != BAE_NO_ERROR)
    {
        return 0;
    }

    for (i = 0; i < targetCount; ++i)
    {
        int sampleApplied;
        int sampleUnsupported;

        if (targets[i].sampleIndex >= sampleCount)
        {
            fprintf(stderr,
                    "Error: sample index %u out of range (sample count: %u)\n",
                    (unsigned)targets[i].sampleIndex,
                    (unsigned)sampleCount);
            return 0;
        }

        if (!resample_one_sample(document,
                                 targets[i].sampleIndex,
                                 targets[i].rateHz,
                                 &sampleApplied,
                                 &sampleUnsupported))
        {
            return 0;
        }

        if (sampleUnsupported)
        {
            fprintf(stderr,
                    "Warning: sample %u not resampled (unsupported waveform format)\n",
                    (unsigned)targets[i].sampleIndex);
            continue;
        }

        if (sampleApplied)
        {
            fprintf(stderr,
                    "Sample %u resampled to %u Hz\n",
                    (unsigned)targets[i].sampleIndex,
                    (unsigned)targets[i].rateHz);
        }
    }

    return 1;
}

static int trim_document_to_tick(BAERmfEditorDocument *document,
                                 uint32_t boundaryTick)
{
    return BAERmfEditorDocument_TrimToTick(document, boundaryTick) == BAE_NO_ERROR;
}

static int add_trim_hard_stop_events(BAERmfEditorDocument *document,
                                     uint32_t boundaryTick)
{
    uint16_t trackCount;
    uint16_t trackIndex;
    uint32_t stopTick;
    BAEResult result;

    if (!document)
    {
        return 0;
    }

    if (boundaryTick == 0)
    {
        return 1;
    }

    stopTick = boundaryTick - 1;
    trackCount = 0;
    result = BAERmfEditorDocument_GetTrackCount(document, &trackCount);
    if (result != BAE_NO_ERROR)
    {
        return 0;
    }

    for (trackIndex = 0; trackIndex < trackCount; ++trackIndex)
    {
        /* Sustain off first, then all-notes-off and all-sound-off to ensure
         * trim boundaries stop promptly even with held pedals or long releases. */
        result = BAERmfEditorDocument_AddTrackCCEvent(document, trackIndex, 64, stopTick, 0);
        if (result != BAE_NO_ERROR)
        {
            return 0;
        }
        result = BAERmfEditorDocument_AddTrackCCEvent(document, trackIndex, 123, stopTick, 0);
        if (result != BAE_NO_ERROR)
        {
            return 0;
        }
        result = BAERmfEditorDocument_AddTrackCCEvent(document, trackIndex, 120, stopTick, 0);
        if (result != BAE_NO_ERROR)
        {
            return 0;
        }
    }

    return 1;
}

static void print_document_info(BAERmfEditorDocument const *document, const char *sourcePath)
{
    uint32_t sampleCount;
    uint32_t songEndTick;
    uint64_t songLengthMs;
    uint32_t i;
    int printedAnyCodec;
    int printedAnyRate;
    bool loopEnabled;
    uint32_t loopStartTick;
    uint32_t loopEndTick;
    int32_t loopCount;
    uint64_t loopStartMs;
    uint64_t loopEndMs;
    BAEResult loopResult;

    sampleCount = 0;
    (void)BAERmfEditorDocument_GetSampleCount(document, &sampleCount);

    songEndTick = get_song_end_tick(document);
    songLengthMs = 0;
    if (songEndTick > 0)
    {
        (void)song_ticks_to_milliseconds(document, songEndTick, &songLengthMs);
    }

    loopEnabled = FALSE;
    loopStartTick = 0;
    loopEndTick = 0;
    loopCount = 0;
    loopStartMs = 0;
    loopEndMs = 0;
    loopResult = BAERmfEditorDocument_GetMidiLoopMarkers(document,
                                                          &loopEnabled,
                                                          &loopStartTick,
                                                          &loopEndTick,
                                                          &loopCount);

    printf("Source: %s\n", sourcePath ? sourcePath : "(unknown)");
    {
        int printedMetadata;
        printedMetadata = 0;
        for (i = 0; i < songtool_metadata_field_count(); ++i)
        {
            const char *value;

            value = BAERmfEditorDocument_GetInfo(document, kSongtoolMetadataFields[i].type);
            if (value && value[0])
            {
                printf("%s: %s\n", kSongtoolMetadataFields[i].label, value);
                printedMetadata = 1;
            }
        }
        if (!printedMetadata)
        {
            printf("Metadata: (none)\n");
        }
    }

    printf("Song length: %llu ms (%.3f s)\n",
           (unsigned long long)songLengthMs,
           (double)songLengthMs / 1000.0);
    
    if (loopResult == BAE_NO_ERROR && loopEnabled)
    {
        if (song_ticks_to_milliseconds(document, loopStartTick, &loopStartMs) &&
            song_ticks_to_milliseconds(document, loopEndTick, &loopEndMs))
        {
            printf("Loop: enabled, start=%llu ms (%u ticks), end=%llu ms (%u ticks), count=%d\n",
                   (unsigned long long)loopStartMs,
                   (unsigned)loopStartTick,
                   (unsigned long long)loopEndMs,
                   (unsigned)loopEndTick,
                   (int)loopCount);
        }
        else
        {
            printf("Loop: enabled, start=%u ticks, end=%u ticks, count=%d\n",
                   (unsigned)loopStartTick,
                   (unsigned)loopEndTick,
                   (int)loopCount);
        }
    }
    else
    {
        printf("Loop: disabled\n");
    }
    
    printf("Samples: %u\n", (unsigned)sampleCount);
    if (sampleCount == 0)
    {
        printf("Sample rates: (none)\n");
        printf("Codecs: (none)\n");
        return;
    }

    printf("Sample rates:\n");
    {
        uint32_t *sampleRates;
        uint8_t *rateValid;
        uint32_t j;
        uint32_t k;

        sampleRates = (uint32_t *)malloc(sampleCount * sizeof(*sampleRates));
        rateValid = (uint8_t *)malloc(sampleCount * sizeof(*rateValid));
        if (!sampleRates || !rateValid)
        {
            printf("  (out of memory)\n");
        }
        else
        {
            for (i = 0; i < sampleCount; ++i)
            {
                void const *waveData;
                uint32_t frameCount;
                uint16_t bitSize;
                uint16_t channels;
                BAE_UNSIGNED_FIXED sampledRate;

                if (BAERmfEditorDocument_GetSampleWaveformData(document,
                                                                i,
                                                                &waveData,
                                                                &frameCount,
                                                                &bitSize,
                                                                &channels,
                                                                &sampledRate) == BAE_NO_ERROR)
                {
                    sampleRates[i] = (uint32_t)((sampledRate + 32768u) >> 16u);
                    rateValid[i] = (sampleRates[i] > 0) ? 1u : 0u;
                }
                else
                {
                    sampleRates[i] = 0;
                    rateValid[i] = 0;
                }
            }

            printedAnyRate = 0;
            for (i = 0; i < sampleCount; ++i)
            {
                int alreadyPrinted;

                if (!rateValid[i])
                {
                    continue;
                }

                alreadyPrinted = 0;
                for (j = 0; j < i; ++j)
                {
                    if (rateValid[j] && sampleRates[j] == sampleRates[i])
                    {
                        alreadyPrinted = 1;
                        break;
                    }
                }
                if (alreadyPrinted)
                {
                    continue;
                }

                printf("  %u Hz: ", (unsigned)sampleRates[i]);
                printedAnyRate = 1;

                {
                    int firstInGroup;
                    firstInGroup = 1;
                    for (k = 0; k < sampleCount; ++k)
                    {
                        if (rateValid[k] && sampleRates[k] == sampleRates[i])
                        {
                            if (!firstInGroup)
                            {
                                printf(", ");
                            }
                            printf("%u", (unsigned)k);
                            firstInGroup = 0;
                        }
                    }
                }
                printf("\n");
            }

            if (!printedAnyRate)
            {
                printf("  (unknown)\n");
            }
        }

        if (sampleRates)
        {
            free(sampleRates);
        }
        if (rateValid)
        {
            free(rateValid);
        }
    }

    printf("Codecs:");

    {
        char (*codecDescs)[96];
        uint32_t j;
        uint32_t k;

        codecDescs = (char (*)[96])malloc(sampleCount * sizeof(*codecDescs));
        if (!codecDescs)
        {
            printf("(out of memory)\n");
            return;
        }

        for (i = 0; i < sampleCount; ++i)
        {
            if (BAERmfEditorDocument_GetSampleCodecDescription(document, i, codecDescs[i], 96) != BAE_NO_ERROR)
            {
                codecDescs[i][0] = '\0';
            }
        }

        printf("\n");
        printedAnyCodec = 0;
        for (i = 0; i < sampleCount; ++i)
        {
            int alreadyPrinted;

            if (codecDescs[i][0] == '\0')
            {
                continue;
            }

            alreadyPrinted = 0;
            for (j = 0; j < i; ++j)
            {
                if (codecDescs[j][0] && strcmp(codecDescs[j], codecDescs[i]) == 0)
                {
                    alreadyPrinted = 1;
                    break;
                }
            }
            if (alreadyPrinted)
            {
                continue;
            }

            printf("  %-24s: ", codecDescs[i]);
            printedAnyCodec = 1;

            {
                int firstInGroup = 1;
                for (k = 0; k < sampleCount; ++k)
                {
                    if (codecDescs[k][0] && strcmp(codecDescs[k], codecDescs[i]) == 0)
                    {
                        if (!firstInGroup)
                        {
                            printf(", ");
                        }
                        printf("%u", k);
                        firstInGroup = 0;
                    }
                }
            }
            printf("\n");
        }

        if (!printedAnyCodec)
        {
            printf("  (unknown)\n");
        }

        free(codecDescs);
    }

    printf("Instruments:\n");
    for (i = 0; i < sampleCount; ++i)
    {
        uint32_t instID;
        uint32_t j;
        int alreadyPrinted;
        int firstInGroup;

        if (BAERmfEditorDocument_GetInstIDForSample(document, i, &instID) != BAE_NO_ERROR)
        {
            continue;
        }

        alreadyPrinted = 0;
        for (j = 0; j < i; ++j)
        {
            uint32_t otherInstID;

            if (BAERmfEditorDocument_GetInstIDForSample(document, j, &otherInstID) == BAE_NO_ERROR &&
                otherInstID == instID)
            {
                alreadyPrinted = 1;
                break;
            }
        }
        if (alreadyPrinted)
        {
            continue;
        }

        printf("  0x%X: ", (unsigned)instID);
        firstInGroup = 1;
        for (j = 0; j < sampleCount; ++j)
        {
            uint32_t otherInstID;

            if (BAERmfEditorDocument_GetInstIDForSample(document, j, &otherInstID) != BAE_NO_ERROR ||
                otherInstID != instID)
            {
                continue;
            }

            if (!firstInGroup)
            {
                printf(", ");
            }
            printf("%u", (unsigned)j);
            firstInGroup = 0;
        }
        printf("\n");
    }
}

int main(int argc, char *argv[])
{
    const char *sourcePath;
    const char *destPath;
    BAERmfEditorDocument *document;
    BAEResult result;
    Mod2RmfEncoderSettings encoderSettings;
    BAERmfEditorCompressionType compressionType;
    int argi;
    int doInfo;
    int doCompression;
    int doGlobalCompression;
    int doStorage;
    int doGain;
    int doResample;
    int doGlobalResample;
    int doMetadataEdit;
    int doDisableLoop;
    int doSetLoop;
    int loopEndExplicit;
    int doUpgrade;
    int doTrim;
    int trimExplicit;
    uint64_t loopStartValue;
    uint64_t loopEndValue;
    uint64_t trimValue;
    int loopStartIsTicks;
    int loopEndIsTicks;
    int trimIsTicks;
    int32_t loopCount;
    SongtoolSampleOverride sampleOverrides[SONGTOOL_MAX_SAMPLE_OVERRIDES];
    SongtoolInstrumentOverride instrumentOverrides[SONGTOOL_MAX_INSTRUMENT_OVERRIDES];
    SongtoolMetadataEdit metadataEdits[SONGTOOL_MAX_METADATA_EDITS];
    SongtoolResampleTarget resampleTargets[SONGTOOL_MAX_RESAMPLE_TARGETS];
    uint32_t sampleOverrideCount;
    uint32_t instrumentOverrideCount;
    uint32_t metadataEditCount;
    uint32_t resampleTargetCount;
    double gainDb;
    uint32_t globalResampleRateHz;
    int requiresZmf;
    BAERmfEditorSndStorageType storageType;
    int preserveMidi;
    int doUnroll;
    int outputAsMidi;

    sourcePath = NULL;
    destPath = NULL;
    doInfo = 0;
    doCompression = 0;
    doGlobalCompression = 0;
    doStorage = 0;
    doGain = 0;
    doResample = 0;
    doGlobalResample = 0;
    doMetadataEdit = 0;
    doDisableLoop = 0;
    doSetLoop = 0;
    loopEndExplicit = 0;
    doTrim = 0;
    trimExplicit = 0;
    doUpgrade = 0;
    loopStartValue = 0;
    loopEndValue = 0;
    trimValue = 0;
    loopStartIsTicks = 0;
    loopEndIsTicks = 0;
    trimIsTicks = 0;
    loopCount = -1;
    sampleOverrideCount = 0;
    instrumentOverrideCount = 0;
    metadataEditCount = 0;
    resampleTargetCount = 0;
    gainDb = 0.0;
    globalResampleRateHz = 0;
    requiresZmf = 0;
    storageType = BAE_EDITOR_SND_STORAGE_ESND;
    preserveMidi = 0;
    doUnroll = 0;
    outputAsMidi = 0;

    mod2rmf_encoder_defaults(&encoderSettings);

    if (argc < 2)
    {
        print_usage(argv[0]);
        return 1;
    }

    for (argi = 1; argi < argc; ++argi)
    {
        const char *arg = argv[argi];

        if (!strcmp(arg, "--help") || !strcmp(arg, "-h"))
        {
            print_usage(argv[0]);
            return 0;
        }
        if (!strcmp(arg, "--codecs"))
        {
            mod2rmf_encoder_print_codecs();
            return 0;
        }
        if (!strcmp(arg, "--list-meta"))
        {
            print_metadata_field_names();
            return 0;
        }
        if (!strcmp(arg, "--info"))
        {
            doInfo = 1;
            continue;
        }
        if (!strcmp(arg, "--upgrade"))
        {
            doUpgrade = 1;
            continue;
        }
        if (!strcmp(arg, "--sndstorage"))
        {
            if (argi + 1 >= argc)
            {
                fprintf(stderr, "Error: %s requires a value: ESND, CSND, or SND\n", arg);
                return 1;
            }
            ++argi;
            if (!parse_snd_storage_type(argv[argi], &storageType))
            {
                fprintf(stderr,
                        "Error: invalid %s value '%s' (expected ESND, CSND, or SND)\n",
                        arg,
                        argv[argi]);
                return 1;
            }
            doStorage = 1;
            continue;
        }
        if (!strcmp(arg, "--codec"))
        {
            if (argi + 1 >= argc)
            {
                fprintf(stderr, "Error: --codec requires an argument\n");
                return 1;
            }
            ++argi;
            if (mod2rmf_encoder_parse_codec(argv[argi], &encoderSettings.codec) != 0)
            {
                fprintf(stderr, "Error: unknown codec '%s'\n", argv[argi]);
                mod2rmf_encoder_print_codecs();
                return 1;
            }
            doCompression = 1;
            doGlobalCompression = 1;
            if (mod2rmf_encoder_requires_zmf(encoderSettings.codec))
            {
                requiresZmf = 1;
            }
            continue;
        }
        if (!strcmp(arg, "--sample-codec"))
        {
            if (argi + 1 >= argc)
            {
                fprintf(stderr, "Error: --sample-codec requires an argument\n");
                return 1;
            }
            if (sampleOverrideCount >= SONGTOOL_MAX_SAMPLE_OVERRIDES)
            {
                fprintf(stderr, "Error: too many --sample-codec entries (max %d)\n",
                        SONGTOOL_MAX_SAMPLE_OVERRIDES);
                return 1;
            }
            ++argi;
            if (!parse_sample_codec_spec(argv[argi], &sampleOverrides[sampleOverrideCount]))
            {
                fprintf(stderr,
                        "Error: invalid --sample-codec '%s' (expected index:codec[@bitrate])\n",
                        argv[argi]);
                mod2rmf_encoder_print_codecs();
                return 1;
            }
            if (mod2rmf_encoder_requires_zmf(sampleOverrides[sampleOverrideCount].settings.codec))
            {
                requiresZmf = 1;
            }
            ++sampleOverrideCount;
            doCompression = 1;
            continue;
        }
        if (!strcmp(arg, "--instrument-codec"))
        {
            if (argi + 1 >= argc)
            {
                fprintf(stderr, "Error: --instrument-codec requires an argument\n");
                return 1;
            }
            if (instrumentOverrideCount >= SONGTOOL_MAX_INSTRUMENT_OVERRIDES)
            {
                fprintf(stderr, "Error: too many --instrument-codec entries (max %d)\n",
                        SONGTOOL_MAX_INSTRUMENT_OVERRIDES);
                return 1;
            }
            ++argi;
            if (!parse_instrument_codec_spec(argv[argi],
                                             &instrumentOverrides[instrumentOverrideCount]))
            {
                fprintf(stderr,
                        "Error: invalid --instrument-codec '%s' (expected instID:codec[@bitrate])\n",
                        argv[argi]);
                mod2rmf_encoder_print_codecs();
                return 1;
            }
            if (mod2rmf_encoder_requires_zmf(
                    instrumentOverrides[instrumentOverrideCount].settings.codec))
            {
                requiresZmf = 1;
            }
            ++instrumentOverrideCount;
            doCompression = 1;
            continue;
        }
        if (!strcmp(arg, "--bitrate"))
        {
            if (argi + 1 >= argc)
            {
                fprintf(stderr, "Error: --bitrate requires an argument\n");
                return 1;
            }
            ++argi;
            if (mod2rmf_encoder_parse_bitrate(argv[argi], &encoderSettings.bitrateKbps) != 0)
            {
                fprintf(stderr, "Error: invalid bitrate '%s'\n", argv[argi]);
                return 1;
            }
            doCompression = 1;
            continue;
        }
        if (!strcmp(arg, "--gain"))
        {
            char *end = NULL;
            double v;

            if (argi + 1 >= argc)
            {
                fprintf(stderr, "Error: --gain requires a value in dB\n");
                return 1;
            }
            ++argi;
            v = strtod(argv[argi], &end);
            if (end == argv[argi] || *end != '\0')
            {
                fprintf(stderr, "Error: invalid --gain value '%s'\n", argv[argi]);
                return 1;
            }
            gainDb = v;
            doGain = 1;
            continue;
        }
        if (!strcmp(arg, "--resample"))
        {
            uint32_t sampleIndex;
            uint32_t rateHz;
            int hasSampleIndex;

            if (argi + 1 >= argc)
            {
                fprintf(stderr, "Error: --resample requires an argument\n");
                return 1;
            }
            ++argi;

            if (!parse_resample_spec(argv[argi], &sampleIndex, &rateHz, &hasSampleIndex))
            {
                fprintf(stderr,
                        "Error: invalid --resample '%s' (expected rate or index:rate)\n",
                        argv[argi]);
                return 1;
            }

            if (hasSampleIndex)
            {
                if (resampleTargetCount >= SONGTOOL_MAX_RESAMPLE_TARGETS)
                {
                    fprintf(stderr, "Error: too many --resample index:rate entries (max %d)\n",
                            SONGTOOL_MAX_RESAMPLE_TARGETS);
                    return 1;
                }
                resampleTargets[resampleTargetCount].sampleIndex = sampleIndex;
                resampleTargets[resampleTargetCount].rateHz = rateHz;
                ++resampleTargetCount;
            }
            else
            {
                doGlobalResample = 1;
                globalResampleRateHz = rateHz;
            }

            doResample = 1;
            continue;
        }
        if (!strcmp(arg, "--set-meta"))
        {
            const char *spec;
            const char *equals;
            char fieldName[64];
            size_t fieldLen;
            BAEInfoType infoType;

            if (argi + 1 >= argc)
            {
                fprintf(stderr, "Error: --set-meta requires an argument\n");
                return 1;
            }
            if (metadataEditCount >= SONGTOOL_MAX_METADATA_EDITS)
            {
                fprintf(stderr, "Error: too many metadata edits (max %d)\n", SONGTOOL_MAX_METADATA_EDITS);
                return 1;
            }

            ++argi;
            spec = argv[argi];
            equals = strchr(spec, '=');
            if (!equals)
            {
                fprintf(stderr, "Error: invalid --set-meta '%s' (expected field=value)\n", spec);
                print_metadata_field_names();
                return 1;
            }

            fieldLen = (size_t)(equals - spec);
            if (fieldLen == 0 || fieldLen >= sizeof(fieldName))
            {
                fprintf(stderr, "Error: invalid metadata field in --set-meta '%s'\n", spec);
                print_metadata_field_names();
                return 1;
            }
            memcpy(fieldName, spec, fieldLen);
            fieldName[fieldLen] = '\0';

            if (!songtool_parse_metadata_field_name(fieldName, &infoType, NULL))
            {
                fprintf(stderr, "Error: unknown metadata field '%s'\n", fieldName);
                print_metadata_field_names();
                return 1;
            }

            metadataEdits[metadataEditCount].type = infoType;
            metadataEdits[metadataEditCount].value = equals + 1;
            metadataEdits[metadataEditCount].clear = 0;
            ++metadataEditCount;
            doMetadataEdit = 1;
            continue;
        }
        if (!strcmp(arg, "--clear-meta"))
        {
            BAEInfoType infoType;

            if (argi + 1 >= argc)
            {
                fprintf(stderr, "Error: --clear-meta requires a field name\n");
                return 1;
            }
            if (metadataEditCount >= SONGTOOL_MAX_METADATA_EDITS)
            {
                fprintf(stderr, "Error: too many metadata edits (max %d)\n", SONGTOOL_MAX_METADATA_EDITS);
                return 1;
            }

            ++argi;
            if (!songtool_parse_metadata_field_name(argv[argi], &infoType, NULL))
            {
                fprintf(stderr, "Error: unknown metadata field '%s'\n", argv[argi]);
                print_metadata_field_names();
                return 1;
            }

            metadataEdits[metadataEditCount].type = infoType;
            metadataEdits[metadataEditCount].value = "";
            metadataEdits[metadataEditCount].clear = 1;
            ++metadataEditCount;
            doMetadataEdit = 1;
            continue;
        }
        if (!strcmp(arg, "--loop-end"))
        {
            char *end = NULL;
            unsigned long long v;
            const char *valStr;
            if (argi + 1 >= argc)
            {
                fprintf(stderr, "Error: --loop-end requires an argument\n");
                return 1;
            }
            ++argi;
            valStr = argv[argi];
            if (valStr[0] == '+')
            {
                loopEndIsTicks = 1;
                valStr++;
            }
            v = strtoull(valStr, &end, 10);
            if (end == valStr || *end != '\0')
            {
                fprintf(stderr, "Error: invalid --loop-end value '%s'\n", argv[argi]);
                return 1;
            }
            loopEndValue = (uint64_t)v;
            loopEndExplicit = 1;
            doSetLoop = 1;
            continue;
        }
        if (!strcmp(arg, "--loop-start"))
        {
            char *end = NULL;
            unsigned long long v;
            const char *valStr;
            if (argi + 1 >= argc)
            {
                fprintf(stderr, "Error: --loop-start requires an argument\n");
                return 1;
            }
            ++argi;
            valStr = argv[argi];
            if (valStr[0] == '+')
            {
                loopStartIsTicks = 1;
                valStr++;
            }
            v = strtoull(valStr, &end, 10);
            if (end == valStr || *end != '\0')
            {
                fprintf(stderr, "Error: invalid --loop-start value '%s'\n", argv[argi]);
                return 1;
            }
            loopStartValue = (uint64_t)v;
            doSetLoop = 1;
            continue;
        }
        if (!strcmp(arg, "--loop-count"))
        {
            char *end = NULL;
            long v;
            if (argi + 1 >= argc)
            {
                fprintf(stderr, "Error: --loop-count requires an argument\n");
                return 1;
            }
            ++argi;
            v = strtol(argv[argi], &end, 10);
            if (end == argv[argi] || *end != '\0')
            {
                fprintf(stderr, "Error: invalid --loop-count value '%s'\n", argv[argi]);
                return 1;
            }
            loopCount = (int32_t)v;
            doSetLoop = 1;
            continue;
        }
        if (!strcmp(arg, "--disable-loop"))
        {
            doDisableLoop = 1;
            continue;
        }
        if (!strcmp(arg, "--trim"))
        {
            char *end = NULL;
            unsigned long long v;
            const char *valStr;
            if (argi + 1 >= argc)
            {
                fprintf(stderr, "Error: --trim requires an argument\n");
                return 1;
            }
            ++argi;
            valStr = argv[argi];
            if (valStr[0] == '+')
            {
                trimIsTicks = 1;
                valStr++;
            }
            v = strtoull(valStr, &end, 10);
            if (end == valStr || *end != '\0')
            {
                fprintf(stderr, "Error: invalid --trim value '%s'\n", argv[argi]);
                return 1;
            }
            trimValue = (uint64_t)v;
            trimExplicit = 1;
            doTrim = 1;
            continue;
        }
        if (!strcmp(arg, "--unroll"))
        {
            doUnroll = 1;
            continue;
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

    if (!sourcePath)
    {
        print_usage(argv[0]);
        return 1;
    }

    if (!destPath && !doInfo)
    {
        print_usage(argv[0]);
        return 1;
    }

    if (!destPath && (doUpgrade || doCompression || doStorage || doGain || doResample || doMetadataEdit || doSetLoop || doDisableLoop || doTrim || doUnroll))
    {
        fprintf(stderr, "Error: destination path is required for edit operations\n");
        return 1;
    }

    if (!doInfo && !doUpgrade && !doCompression && !doStorage && !doGain && !doResample && !doMetadataEdit && !doSetLoop && !doDisableLoop && !doTrim && !doUnroll)
    {
        fprintf(stderr, "Error: no operation requested. Use --upgrade, --codec, --sndstorage, --gain, --resample, --set-meta/--clear-meta, --loop-*, --disable-loop, --trim, and/or --unroll.\n");
        return 1;
    }

    if (destPath)
    {
        outputAsMidi = is_midi_path(destPath);
    }

#if SONGTOOL_ENABLE_MIDI_EXPORT == 0
    if (outputAsMidi)
    {
        fprintf(stderr, "Error: MIDI output is disabled in this build\n");
        return 1;
    }
#endif

    preserveMidi = !doSetLoop && !doDisableLoop && !doTrim && !doUnroll;

    if (outputAsMidi && (doUpgrade || doCompression || doStorage || doGain || doResample || doMetadataEdit))
    {
        fprintf(stderr,
                "Error: .mid output only supports MIDI-only operations (--loop-*, --disable-loop, --trim, --unroll)\n");
        return 1;
    }

    if (doUpgrade && (doCompression || doStorage || doGain || doResample || doMetadataEdit || doSetLoop || doDisableLoop || doTrim || doUnroll))
    {
        fprintf(stderr, "Error: --upgrade cannot be combined with other edit operations\n");
        return 1;
    }

    if (doCompression && !doGlobalCompression &&
        sampleOverrideCount == 0 && instrumentOverrideCount == 0)
    {
        fprintf(stderr,
                "Error: --bitrate requires --codec, --sample-codec, or --instrument-codec\n");
        return 1;
    }

    if (!file_exists(sourcePath))
    {
        fprintf(stderr, "Error: source file not found: %s\n", sourcePath);
        return 1;
    }

    if (doSetLoop && !doDisableLoop && loopEndExplicit)
    {
        if ((loopEndIsTicks || loopStartIsTicks) ? (loopEndValue <= loopStartValue) : (loopEndValue <= loopStartValue))
        {
            fprintf(stderr, "Error: loop end must be greater than loop start (%llu <= %llu)\n",
                    (unsigned long long)loopEndValue,
                    (unsigned long long)loopStartValue);
            return 1;
        }
    }

    if (doCompression && requiresZmf && !is_zmf_path(destPath))
    {
        fprintf(stderr,
                "Error: selected codec requires ZMF container. Use a .zmf destination path.\n");
        return 1;
    }

    result = BAE_Setup();
    if (result != BAE_NO_ERROR)
    {
        fprintf(stderr, "Error: BAE_Setup failed (%d)\n", (int)result);
        return 1;
    }

    if (doUpgrade)
    {
        if (is_midi_path(sourcePath))
        {
            BAERmfEditorDocument *midiDoc;

            midiDoc = BAERmfEditorDocument_LoadFromFile((BAEPathName)sourcePath);
            if (!midiDoc)
            {
                fprintf(stderr, "Error: failed to load MIDI: %s\n", sourcePath);
                BAE_Cleanup();
                return 1;
            }
            result = BAERmfEditorDocument_SaveAsRmfPreserveMidi(midiDoc,
                                                                 (BAEPathName)destPath);
            BAERmfEditorDocument_Delete(midiDoc);
            if (result != BAE_NO_ERROR)
            {
                fprintf(stderr, "Error: MIDI to RMF/ZMF conversion failed (%d): %s\n",
                        (int)result, destPath);
                BAE_Cleanup();
                return 1;
            }
            fprintf(stdout, "Converted MIDI to RMF/ZMF: %s -> %s\n",
                    sourcePath, destPath);
            BAE_Cleanup();
            return 0;
        }

        {
            int32_t fromVersion;
            int i;

            result = BAERmfEditorDocument_UpgradeFile((BAEPathName)sourcePath,
                                                      (BAEPathName)destPath,
                                                      &fromVersion);
            if (result == BAE_ALREADY_EXISTS)
            {
                fprintf(stderr, "This file is already v%d, this tool can upgrade ", (int)fromVersion);
                for (i = 1; i < XFILERESOURCE_VERSION_ZMF; ++i)
                {
                    if (i > 1)
                    {
                        fprintf(stderr, ", ");
                    }
                    fprintf(stderr, "v%d", i);
                }
                fprintf(stderr, " to v%d\n", XFILERESOURCE_VERSION_ZMF);
                BAE_Cleanup();
                return 1;
            }
            if (result != BAE_NO_ERROR)
            {
                fprintf(stderr, "Error: upgrade failed (%d): %s\n", (int)result, sourcePath);
                BAE_Cleanup();
                return 1;
            }
            fprintf(stdout, "Upgraded v%d to v%d: %s -> %s\n",
                    (int)fromVersion,
                    XFILERESOURCE_VERSION_ZMF,
                    sourcePath,
                    destPath);
            BAE_Cleanup();
            return 0;
        }
    }

    document = BAERmfEditorDocument_LoadFromFile((BAEPathName)sourcePath);
    if (!document)
    {
        fprintf(stderr, "Error: failed to load RMF/ZMF: %s\n", sourcePath);
        BAE_Cleanup();
        return 1;
    }

    if (doInfo)
    {
        print_document_info(document, sourcePath);
    }

    if (!doCompression && !doStorage && !doGain && !doResample && !doMetadataEdit && !doSetLoop && !doDisableLoop && !doTrim && !doUnroll && !doUpgrade)
    {
        BAERmfEditorDocument_Delete(document);
        BAE_Cleanup();
        return 0;
    }

    if (doStorage)
    {
        if (!apply_storage_to_all_samples(document, storageType))
        {
            fprintf(stderr, "Error: failed to apply sample storage type\n");
            BAERmfEditorDocument_Delete(document);
            BAE_Cleanup();
            return 1;
        }
    }

    if (doGain)
    {
        if (!apply_gain_to_all_samples(document, gainDb))
        {
            fprintf(stderr, "Error: failed to apply gain\n");
            BAERmfEditorDocument_Delete(document);
            BAE_Cleanup();
            return 1;
        }
    }

    if (doResample)
    {
        if (doGlobalResample && !apply_resample_to_all_samples(document, globalResampleRateHz))
        {
            fprintf(stderr, "Error: global resample failed\n");
            BAERmfEditorDocument_Delete(document);
            BAE_Cleanup();
            return 1;
        }

        if (resampleTargetCount > 0 &&
            !apply_resample_targets(document, resampleTargets, resampleTargetCount))
        {
            fprintf(stderr, "Error: per-sample resample failed\n");
            BAERmfEditorDocument_Delete(document);
            BAE_Cleanup();
            return 1;
        }
    }

    if (doCompression)
    {
        if (doGlobalCompression)
        {
            compressionType = mod2rmf_encoder_resolve(&encoderSettings);

            if (!apply_compression_to_all_samples(document, compressionType))
            {
                fprintf(stderr, "Error: sample recompression failed\n");
                BAERmfEditorDocument_Delete(document);
                BAE_Cleanup();
                return 1;
            }
        }

        if (instrumentOverrideCount > 0 &&
            !apply_instrument_overrides(document,
                                        instrumentOverrides,
                                        instrumentOverrideCount))
        {
            fprintf(stderr, "Error: per-instrument compression overrides failed\n");
            BAERmfEditorDocument_Delete(document);
            BAE_Cleanup();
            return 1;
        }

        if (sampleOverrideCount > 0 &&
            !apply_sample_overrides(document, sampleOverrides, sampleOverrideCount))
        {
            fprintf(stderr, "Error: per-sample compression overrides failed\n");
            BAERmfEditorDocument_Delete(document);
            BAE_Cleanup();
            return 1;
        }
    }

    if (doMetadataEdit)
    {
        if (!apply_metadata_edits(document, metadataEdits, metadataEditCount))
        {
            BAERmfEditorDocument_Delete(document);
            BAE_Cleanup();
            return 1;
        }
    }

    if (doDisableLoop)
    {
        result = BAERmfEditorDocument_SetMidiLoopMarkers(document, FALSE, 0, 0, 0);
        if (result != BAE_NO_ERROR)
        {
            fprintf(stderr, "Error: failed to disable MIDI loop markers (%d)\n", (int)result);
            BAERmfEditorDocument_Delete(document);
            BAE_Cleanup();
            return 1;
        }
        fprintf(stderr, "Loop markers disabled\n");
    }
    else if (doSetLoop)
    {
        uint32_t startTick;
        uint32_t endTick;
        uint64_t loopStartMs;
        uint64_t loopEndMs;

        if (loopStartIsTicks)
        {
            startTick = (uint32_t)loopStartValue;
        }
        else
        {
            if (!milliseconds_to_song_ticks(document, loopStartValue, &startTick))
            {
                fprintf(stderr, "Error: failed to convert loop start milliseconds to MIDI ticks\n");
                BAERmfEditorDocument_Delete(document);
                BAE_Cleanup();
                return 1;
            }
        }

        if (loopEndIsTicks)
        {
            endTick = (uint32_t)loopEndValue;
        }
        else if (loopEndExplicit)
        {
            if (!milliseconds_to_song_ticks(document, loopEndValue, &endTick))
            {
                fprintf(stderr, "Error: failed to convert loop end milliseconds to MIDI ticks\n");
                BAERmfEditorDocument_Delete(document);
                BAE_Cleanup();
                return 1;
            }
        }
        else
        {
            uint32_t songEndTick = get_song_end_tick(document);
            if (songEndTick == 0)
            {
                fprintf(stderr, "Error: could not determine song length for default loop end\n");
                BAERmfEditorDocument_Delete(document);
                BAE_Cleanup();
                return 1;
            }
            endTick = songEndTick;
        }

        if (endTick <= startTick)
        {
            endTick = startTick + 1;
        }

        result = BAERmfEditorDocument_SetMidiLoopMarkers(document,
                                                         TRUE,
                                                         startTick,
                                                         endTick,
                                                         loopCount);
        if (result != BAE_NO_ERROR)
        {
            fprintf(stderr, "Error: failed to set MIDI loop markers (%d)\n", (int)result);
            BAERmfEditorDocument_Delete(document);
            BAE_Cleanup();
            return 1;
        }

        song_ticks_to_milliseconds(document, startTick, &loopStartMs);
        song_ticks_to_milliseconds(document, endTick, &loopEndMs);

        fprintf(stderr,
                "Loop markers set: start=%u ticks (%llu ms), end=%u ticks (%llu ms), count=%d\n",
                (unsigned)startTick,
                (unsigned long long)loopStartMs,
                (unsigned)endTick,
                (unsigned long long)loopEndMs,
                (int)loopCount);
    }

    if (doTrim)
    {
        uint32_t trimTick;
        uint32_t postTrimEndTick;
        uint64_t actualTrimMs;
        uint64_t postTrimMs;
        bool loopEnabled;
        uint32_t loopStartTick;
        uint32_t loopEndTick;
        int32_t loopCount;

        if (!trimExplicit)
        {
            fprintf(stderr, "Error: --trim requires a value\n");
            BAERmfEditorDocument_Delete(document);
            BAE_Cleanup();
            return 1;
        }

        if (trimIsTicks)
        {
            trimTick = (uint32_t)trimValue;
        }
        else
        {
            if (!milliseconds_to_song_ticks(document, trimValue, &trimTick))
            {
                fprintf(stderr, "Error: failed to convert trim milliseconds to MIDI ticks\n");
                BAERmfEditorDocument_Delete(document);
                BAE_Cleanup();
                return 1;
            }
        }

        /* Convert to milliseconds to show actual result */
        song_ticks_to_milliseconds(document, trimTick, &actualTrimMs);

        if (!trim_document_to_tick(document, trimTick))
        {
            fprintf(stderr, "Error: failed to trim MIDI timeline\n");
            BAERmfEditorDocument_Delete(document);
            BAE_Cleanup();
            return 1;
        }

        if (!add_trim_hard_stop_events(document, trimTick))
        {
            fprintf(stderr, "Error: failed to add hard-stop events at trim boundary\n");
            BAERmfEditorDocument_Delete(document);
            BAE_Cleanup();
            return 1;
        }

        /* Preserve existing MIDI loop markers when a trim does not
         * invalidate them, to avoid touching the MIDI portion unnecessarily. */
        if (!doSetLoop)
        {
            loopEnabled = FALSE;
            loopStartTick = 0;
            loopEndTick = 0;
            loopCount = 0;

            result = BAERmfEditorDocument_GetMidiLoopMarkers(document,
                                                             &loopEnabled,
                                                             &loopStartTick,
                                                             &loopEndTick,
                                                             &loopCount);
            if (result == BAE_NO_ERROR && loopEnabled)
            {
                if (loopStartTick >= trimTick || loopEndTick > trimTick || loopEndTick <= loopStartTick)
                {
                    result = BAERmfEditorDocument_SetMidiLoopMarkers(document, FALSE, 0, 0, 0);
                    if (result != BAE_NO_ERROR)
                    {
                        fprintf(stderr, "Error: failed to clear MIDI loop markers after trim (%d)\n", (int)result);
                        BAERmfEditorDocument_Delete(document);
                        BAE_Cleanup();
                        return 1;
                    }
                    fprintf(stderr,
                            "Cleared MIDI loop markers after trim (start=%u, end=%u, count=%d)\n",
                            (unsigned)loopStartTick,
                            (unsigned)loopEndTick,
                            (int)loopCount);
                }
                else
                {
                    fprintf(stderr,
                            "Preserved MIDI loop markers after trim (start=%u, end=%u, count=%d)\n",
                            (unsigned)loopStartTick,
                            (unsigned)loopEndTick,
                            (int)loopCount);
                }
            }
        }

        postTrimEndTick = get_song_end_tick(document);
        postTrimMs = 0;
        if (postTrimEndTick > 0)
        {
            (void)song_ticks_to_milliseconds(document, postTrimEndTick, &postTrimMs);
        }

        if (trimIsTicks)
        {
            fprintf(stderr,
                    "Trimmed MIDI timeline to %u ticks (%llu ms), end now %u ticks (%llu ms)\n",
                    (unsigned)trimTick,
                    (unsigned long long)actualTrimMs,
                    (unsigned)postTrimEndTick,
                    (unsigned long long)postTrimMs);
        }
        else
        {
            fprintf(stderr,
                    "Trimmed MIDI timeline: requested=%llu ms, actual=%llu ms (%u ticks), end now %u ticks (%llu ms)\n",
                    (unsigned long long)trimValue,
                    (unsigned long long)actualTrimMs,
                    (unsigned)trimTick,
                    (unsigned)postTrimEndTick,
                    (unsigned long long)postTrimMs);
            
            if (actualTrimMs != trimValue)
            {
                fprintf(stderr, "  (note: rounding precision loss due to MIDI tempo conversion)\n");
            }
        }
    }

#if SONGTOOL_ENABLE_ROLLED_MIDI_UNROLL == 1
    if (doUnroll && !outputAsMidi)
    {
        if (!apply_unroll_to_document(&document, destPath))
        {
            BAERmfEditorDocument_Delete(document);
            BAE_Cleanup();
            return 1;
        }
    }
#else
    if (doUnroll)
    {
        fprintf(stderr, "Error: --unroll is disabled in this build\n");
        BAERmfEditorDocument_Delete(document);
        BAE_Cleanup();
        return 1;
    }
#endif

    if (outputAsMidi)
    {
#if SONGTOOL_ENABLE_MIDI_EXPORT == 1
        if (!BAERmfEditorDocument_CanSaveAsMidi(document))
        {
            fprintf(stderr,
                    "Error: document contains RMF/ZMF resources that cannot be represented in standard MIDI\n");
            BAERmfEditorDocument_Delete(document);
            BAE_Cleanup();
            return 1;
        }

        result = BAERmfEditorDocument_SaveAsMidi(document, (BAEPathName)destPath);
        if (result != BAE_NO_ERROR)
        {
            fprintf(stderr, "Error: MIDI save failed (%d): %s\n", (int)result, destPath);
            BAERmfEditorDocument_Delete(document);
            BAE_Cleanup();
            return 1;
        }

#if SONGTOOL_ENABLE_ROLLED_MIDI_UNROLL == 1
        if (doUnroll)
        {
            if (!unroll_midi_file_in_place(destPath))
            {
                BAERmfEditorDocument_Delete(document);
                BAE_Cleanup();
                return 1;
            }
        }
#else
        if (doUnroll)
        {
            fprintf(stderr, "Error: --unroll is disabled in this build\n");
            BAERmfEditorDocument_Delete(document);
            BAE_Cleanup();
            return 1;
        }
#endif
#else
        fprintf(stderr, "Error: MIDI output is disabled in this build\n");
        BAERmfEditorDocument_Delete(document);
        BAE_Cleanup();
        return 1;
#endif
    }
    else if (preserveMidi)
    {
        result = BAERmfEditorDocument_SaveAsRmfPreserveMidi(document, (BAEPathName)destPath);
    }
    else
    {
        result = BAERmfEditorDocument_SaveAsRmf(document, (BAEPathName)destPath);
    }
    if (result != BAE_NO_ERROR)
    {
        fprintf(stderr, "Error: save failed (%d): %s\n", (int)result, destPath);
        BAERmfEditorDocument_Delete(document);
        BAE_Cleanup();
        return 1;
    }

    fprintf(stdout, "Saved %s -> %s\n", sourcePath, destPath);

    BAERmfEditorDocument_Delete(document);
    BAE_Cleanup();
    return 0;
}
