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

#include <NeoBAE.h>

#include "mod2rmf_encoder.h"

static void print_usage(const char *program_name)
{
    fprintf(stderr,
            "Usage: %s [options] <source.rmf|source.zmf> [dest.rmf|dest.zmf]\n"
            "\n"
            "Options:\n"
            "  --info               Show source info (title, song length, codecs)\n"
            "  --codec N|NAME        Recompress all samples to codec number/name\n"
            "                        (pcm, adpcm, mp3, vorbis, flac, opus, opus-rt)\n"
            "  --bitrate N           Target bitrate in kbps (or bps) for lossy codecs\n"
            "  --loop-ms N           Set MIDI loop end point in milliseconds\n"
            "  --loop-start-ms N     Optional MIDI loop start in milliseconds (default: 0)\n"
            "  --loop-count N        Loop count (-1=forever, default: -1)\n"
            "  --disable-loop        Disable MIDI loop markers\n"
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

static int apply_compression_to_all_samples(BAERmfEditorDocument *document,
                                            BAERmfEditorCompressionType compressionType,
                                            int useOpusRoundTrip)
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
        uint32_t assetID;
        BAEResult result;

        result = BAERmfEditorDocument_GetSampleAssetIDForSample(document, i, &assetID);
        if (result != BAE_NO_ERROR)
        {
            fprintf(stderr, "Warning: failed to get asset ID for sample %u (%d)\n",
                    (unsigned)i, (int)result);
            continue;
        }

        result = BAERmfEditorDocument_SetSampleAssetCompression(document, assetID, compressionType);
        if (result != BAE_NO_ERROR)
        {
            fprintf(stderr, "Warning: failed to set compression for sample %u (asset %u): %d\n",
                    (unsigned)i, (unsigned)assetID, (int)result);
            continue;
        }

        if (useOpusRoundTrip && is_opus_compression_type(compressionType))
        {
            BAERmfEditorSampleInfo info;
            result = BAERmfEditorDocument_GetSampleInfo(document, i, &info);
            if (result == BAE_NO_ERROR)
            {
                info.opusRoundTripResample = TRUE;
                (void)BAERmfEditorDocument_SetSampleInfo(document, i, &info);
            }
        }

        ++applied;
    }

    fprintf(stderr,
            "Compression: %s%s applied to %u/%u samples\n",
            mod2rmf_encoder_label(compressionType),
            (useOpusRoundTrip && is_opus_compression_type(compressionType)) ? " (round-trip)" : "",
            (unsigned)applied,
            (unsigned)sampleCount);
    return 1;
}

static void print_document_info(BAERmfEditorDocument const *document, const char *sourcePath)
{
    const char *title;
    uint32_t sampleCount;
    uint32_t songEndTick;
    uint64_t songLengthMs;
    uint32_t i;
    int printedAnyCodec;

    title = BAERmfEditorDocument_GetInfo(document, TITLE_INFO);
    sampleCount = 0;
    (void)BAERmfEditorDocument_GetSampleCount(document, &sampleCount);

    songEndTick = get_song_end_tick(document);
    songLengthMs = 0;
    if (songEndTick > 0)
    {
        (void)song_ticks_to_milliseconds(document, songEndTick, &songLengthMs);
    }

    printf("Source: %s\n", sourcePath ? sourcePath : "(unknown)");
    printf("Title: %s\n", (title && title[0]) ? title : "(none)");
    printf("Song length: %llu ms (%.3f s)\n",
           (unsigned long long)songLengthMs,
           (double)songLengthMs / 1000.0);
    printf("Samples: %u\n", (unsigned)sampleCount);
    printf("Codecs: ");

    if (sampleCount == 0)
    {
        printf("(none)\n");
        return;
    }

    printedAnyCodec = 0;
    for (i = 0; i < sampleCount; ++i)
    {
        char codecBuf[96];
        int isUnique;
        uint32_t j;

        if (BAERmfEditorDocument_GetSampleCodecDescription(document, i, codecBuf, sizeof(codecBuf)) != BAE_NO_ERROR)
        {
            continue;
        }

        isUnique = 1;
        for (j = 0; j < i; ++j)
        {
            char prevCodecBuf[96];
            if (BAERmfEditorDocument_GetSampleCodecDescription(document, j, prevCodecBuf, sizeof(prevCodecBuf)) != BAE_NO_ERROR)
            {
                continue;
            }
            if (strcmp(codecBuf, prevCodecBuf) == 0)
            {
                isUnique = 0;
                break;
            }
        }

        if (!isUnique)
        {
            continue;
        }

        if (printedAnyCodec)
        {
            printf(", ");
        }
        printf("%s", codecBuf);
        printedAnyCodec = 1;
    }

    if (!printedAnyCodec)
    {
        printf("(unknown)");
    }
    printf("\n");
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
    int doDisableLoop;
    int doSetLoop;
    int loopEndExplicit;
    uint64_t loopStartMs;
    uint64_t loopEndMs;
    int32_t loopCount;
    int useOpusRoundTrip;

    sourcePath = NULL;
    destPath = NULL;
    doInfo = 0;
    doCompression = 0;
    doDisableLoop = 0;
    doSetLoop = 0;
    loopEndExplicit = 0;
    loopStartMs = 0;
    loopEndMs = 0;
    loopCount = -1;
    useOpusRoundTrip = 0;

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
        if (!strcmp(arg, "--info"))
        {
            doInfo = 1;
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
        if (!strcmp(arg, "--loop-ms"))
        {
            char *end = NULL;
            unsigned long long v;
            if (argi + 1 >= argc)
            {
                fprintf(stderr, "Error: --loop-ms requires an argument\n");
                return 1;
            }
            ++argi;
            v = strtoull(argv[argi], &end, 10);
            if (end == argv[argi] || *end != '\0')
            {
                fprintf(stderr, "Error: invalid --loop-ms value '%s'\n", argv[argi]);
                return 1;
            }
            loopEndMs = (uint64_t)v;
            loopEndExplicit = 1;
            doSetLoop = 1;
            continue;
        }
        if (!strcmp(arg, "--loop-start-ms"))
        {
            char *end = NULL;
            unsigned long long v;
            if (argi + 1 >= argc)
            {
                fprintf(stderr, "Error: --loop-start-ms requires an argument\n");
                return 1;
            }
            ++argi;
            v = strtoull(argv[argi], &end, 10);
            if (end == argv[argi] || *end != '\0')
            {
                fprintf(stderr, "Error: invalid --loop-start-ms value '%s'\n", argv[argi]);
                return 1;
            }
            loopStartMs = (uint64_t)v;
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

    if (!doInfo && !doCompression && !doSetLoop && !doDisableLoop)
    {
        fprintf(stderr, "Error: no operation requested. Use --codec and/or --loop-ms/--disable-loop.\n");
        return 1;
    }

    if (!file_exists(sourcePath))
    {
        fprintf(stderr, "Error: source file not found: %s\n", sourcePath);
        return 1;
    }

    if (doSetLoop && !doDisableLoop && loopEndExplicit && loopEndMs <= loopStartMs)
    {
        fprintf(stderr, "Error: loop end must be greater than loop start (%llu <= %llu)\n",
                (unsigned long long)loopEndMs,
                (unsigned long long)loopStartMs);
        return 1;
    }

    if (doCompression && mod2rmf_encoder_requires_zmf(encoderSettings.codec) && !is_zmf_path(destPath))
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

    if (!doCompression && !doSetLoop && !doDisableLoop)
    {
        BAERmfEditorDocument_Delete(document);
        BAE_Cleanup();
        return 0;
    }

    if (doCompression)
    {
        compressionType = mod2rmf_encoder_resolve(&encoderSettings);
        useOpusRoundTrip = (encoderSettings.codec == MOD2RMF_CODEC_OPUS_RT) ? 1 : 0;

        if (!apply_compression_to_all_samples(document, compressionType, useOpusRoundTrip))
        {
            fprintf(stderr, "Error: sample recompression failed\n");
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

        if (!loopEndExplicit)
        {
            uint32_t songEndTick = get_song_end_tick(document);
            if (songEndTick == 0 ||
                !song_ticks_to_milliseconds(document, songEndTick, &loopEndMs))
            {
                fprintf(stderr, "Error: could not determine song length for default --loop-ms\n");
                BAERmfEditorDocument_Delete(document);
                BAE_Cleanup();
                return 1;
            }
            fprintf(stderr, "Loop end defaulting to song length: %llu ms\n",
                    (unsigned long long)loopEndMs);
        }

        if (!milliseconds_to_song_ticks(document, loopStartMs, &startTick) ||
            !milliseconds_to_song_ticks(document, loopEndMs, &endTick))
        {
            fprintf(stderr, "Error: failed to convert loop milliseconds to MIDI ticks\n");
            BAERmfEditorDocument_Delete(document);
            BAE_Cleanup();
            return 1;
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

        fprintf(stderr,
                "Loop markers set: start=%llu ms (%u ticks), end=%llu ms (%u ticks), count=%d\n",
                (unsigned long long)loopStartMs,
                (unsigned)startTick,
                (unsigned long long)loopEndMs,
                (unsigned)endTick,
                (int)loopCount);
    }

    result = BAERmfEditorDocument_SaveAsRmf(document, (BAEPathName)destPath);
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
