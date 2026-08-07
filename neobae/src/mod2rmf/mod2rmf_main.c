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
 * mod2rmf.c
 *
 * Tracker module -> RMF/ZMF converter (via libxmp).
 * Supports all formats handled by libxmp (MOD, S3M, XM, IT, etc.).
 *
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include <xmp.h>

#include <NeoBAE.h>
#include <X_Formats.h>

#include "mod2rmf_encoder.h"
#include "mod2rmf_resampler.h"
#include "mod2rmf_common.h"
#include "mod2rmf_song.h"
#include "mod2rmf_rmfcreat.h"

static void print_usage(const char *program_name)
{
    fprintf(stderr,
            "Usage: %s [options] <source module> <dest.rmf|dest.zmf>\n"
            "\n"
            "Options:\n"
            "  --codec N|NAME        Set sample compression (number or name, default: 0/pcm)\n"
            "  --bitrate N           Set bitrate in kbps for lossy codecs\n"
            "  --original            Preserve original 8-bit sample data (forces PCM, ignores --codec/--bitrate)\n"
            "  --codecs              List available codecs and bitrates\n"
            "  --amiga-filter NAME   Amiga hardware LPF sim: none|a500|a1200 (default: none)\n"
            "  --resample-rate HZ    Upsample/downsample samples to HZ (0=native, default: 0)\n"
            "  --resample-filter N   Interpolation: nearest|linear|cubic|sinc (default: sinc)\n"
            "  --gain DB             Apply sample gain in dB before encoding (e.g. -3, +6)\n"
            "  --filters             List available filter/resample options\n"
            "  --stereo-separation N Stereo width 0-100%% (0=mono, 75=default, 100=hard L/R)\n"
            "  --it-v00-cut-rows N   Cut sustained note after N silent rows following explicit IT v00 (0=off, default: 6)\n"
            "  --down-octave-range   Lower sample root/rate by 1 octave for more low-note range\n"
            "                        (legacy headroom trick; prefer --ext-pitch for deep bass)\n"
            "  --avoid-midi-ch10     Avoid mapping tracker channels to MIDI ch10 (useful for some SPC2IT outputs)\n"
            "  --spread              [Experimental] Spread instruments across MIDI channels\n"
            "  --tempomap            Reserved for future tempo-map handling\n"
            "  --ext-pitch           [ZMF] Allow pitch down to -96 semitones (vs default -24)\n"
            "  --ext-adsr            [ZMF] Allow up to 32-stage envelopes (default: 8-stage RMF cap)\n"
            "  --help, -h            Show this help\n",
            program_name);
}

static int file_exists(const char *path)
{
    FILE *f;
    f = fopen(path, "rb");
    if (!f)
    {
        return 0;
    }
    fclose(f);
    return 1;
}

static bool is_zmf_path(const char *path)
{
    const char *ext;

    if (!path)
    {
        return FALSE;
    }

    ext = strrchr(path, '.');
    return (ext && (!strcmp(ext, ".zmf") || !strcmp(ext, ".ZMF"))) ? TRUE : FALSE;
}

int main(int argc, char *argv[])
{
    const char *sourcePath;
    const char *destPath;
    int argi;
    int tempoMap;
    bool useZmfContainer;
    Mod2RmfConverter *conv;
    ModSongModel song;
    BAEResult setupResult;
    Mod2RmfEncoderSettings encSettings;
    Mod2RmfResamplerSettings resamplerSettings;
    BAERmfEditorCompressionType compressionType;
    bool forceOriginalSamples;
    bool codecArgSeen;
    bool bitrateArgSeen;
    bool spreadChannels;
    bool useExtendedPitchRange;
    bool useExtendedAdsr;
    bool avoidMidiChannel10;
    bool downOctaveRange;
    double sampleGainDb;
    uint8_t stereoSeparation;
    uint8_t itV00CutRows;

    sourcePath = NULL;
    destPath = NULL;
    tempoMap = 0;
    mod2rmf_encoder_defaults(&encSettings);
    mod2rmf_resampler_defaults(&resamplerSettings);
    forceOriginalSamples = FALSE;
    codecArgSeen = FALSE;
    bitrateArgSeen = FALSE;
    spreadChannels = FALSE;
    useExtendedPitchRange = FALSE;
    useExtendedAdsr = FALSE;
    avoidMidiChannel10 = FALSE;
    downOctaveRange = FALSE;
    sampleGainDb = 0.0;
    stereoSeparation = 75;
    itV00CutRows = 6;
    mod2rmf_song_model_init(&song);

    if (argc < 2)
    {
        print_usage(argv[0]);
        return 1;
    }

    for (argi = 1; argi < argc; ++argi)
    {
        const char *arg;
        arg = argv[argi];

        if (!strcmp(arg, "--tempomap"))
        {
            tempoMap = 1;
            continue;
        }
        if (!strcmp(arg, "--spread"))
        {
            spreadChannels = TRUE;
            continue;
        }
        if (!strcmp(arg, "--avoid-midi-ch10"))
        {
            avoidMidiChannel10 = TRUE;
            continue;
        }
        if (!strcmp(arg, "--down-octave-range"))
        {
            downOctaveRange = TRUE;
            continue;
        }
        if (!strcmp(arg, "--original"))
        {
            forceOriginalSamples = TRUE;
            continue;
        }
        if (!strcmp(arg, "--codecs"))
        {
            mod2rmf_encoder_print_codecs();
            return 0;
        }
        if (!strcmp(arg, "--filters"))
        {
            mod2rmf_resampler_print_options();
            return 0;
        }
        if (!strcmp(arg, "--ext-pitch"))
        {
            useExtendedPitchRange = TRUE;
            continue;
        }
        if (!strcmp(arg, "--ext-adsr") || !strcmp(arg, "--extended-adsr"))
        {
            useExtendedAdsr = TRUE;
            continue;
        }
        if (!strcmp(arg, "--amiga-filter"))
        {
            if (argi + 1 >= argc)
            {
                fprintf(stderr, "Error: --amiga-filter requires an argument\n");
                return 1;
            }
            ++argi;
            if (mod2rmf_resampler_parse_amiga(argv[argi], &resamplerSettings.amigaFilter) != 0)
            {
                fprintf(stderr, "Error: unknown amiga filter '%s' (use --filters to list)\n", argv[argi]);
                return 1;
            }
            continue;
        }
        if (!strcmp(arg, "--resample-rate"))
        {
            if (argi + 1 >= argc)
            {
                fprintf(stderr, "Error: --resample-rate requires an argument\n");
                return 1;
            }
            ++argi;
            {
                long hz = strtol(argv[argi], NULL, 10);
                if (hz < 1000 || hz > 384000)
                {
                    fprintf(stderr, "Error: invalid resample rate '%s'\n", argv[argi]);
                    return 1;
                }
                resamplerSettings.targetRate = (uint32_t)hz;
            }
            continue;
        }
        if (!strcmp(arg, "--resample-filter"))
        {
            if (argi + 1 >= argc)
            {
                fprintf(stderr, "Error: --resample-filter requires an argument\n");
                return 1;
            }
            ++argi;
            if (mod2rmf_resampler_parse_filter(argv[argi], &resamplerSettings.resampleFilter) != 0)
            {
                fprintf(stderr, "Error: unknown resample filter '%s' (use --filters to list)\n", argv[argi]);
                return 1;
            }
            continue;
        }
        if (!strcmp(arg, "--gain"))
        {
            char *end;
            double gain;

            if (argi + 1 >= argc)
            {
                fprintf(stderr, "Error: --gain requires an argument\n");
                return 1;
            }
            ++argi;
            end = NULL;
            gain = strtod(argv[argi], &end);
            if (end == argv[argi] || *end != '\0' || !isfinite(gain))
            {
                fprintf(stderr, "Error: invalid gain '%s'\n", argv[argi]);
                return 1;
            }
            sampleGainDb = gain;
            continue;
        }
        if (!strcmp(arg, "--stereo-separation"))
        {
            if (argi + 1 >= argc)
            {
                fprintf(stderr, "Error: --stereo-separation requires an argument (0-100)\n");
                return 1;
            }
            ++argi;
            {
                long val = strtol(argv[argi], NULL, 10);
                if (val < 0 || val > 100)
                {
                    fprintf(stderr, "Error: --stereo-separation must be 0-100 (got '%s')\n", argv[argi]);
                    return 1;
                }
                stereoSeparation = (uint8_t)val;
            }
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
            if (mod2rmf_encoder_parse_codec(argv[argi], &encSettings.codec) != 0)
            {
                fprintf(stderr, "Error: unknown codec '%s' (use --codecs to list)\n", argv[argi]);
                return 1;
            }
            codecArgSeen = TRUE;
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
            if (mod2rmf_encoder_parse_bitrate(argv[argi], &encSettings.bitrateKbps) != 0)
            {
                fprintf(stderr, "Error: invalid bitrate '%s'\n", argv[argi]);
                return 1;
            }
            bitrateArgSeen = TRUE;
            continue;
        }
        if (!strcmp(arg, "--it-v00-cut-rows"))
        {
            long rows;
            if (argi + 1 >= argc)
            {
                fprintf(stderr, "Error: --it-v00-cut-rows requires an argument\n");
                return 1;
            }
            ++argi;
            rows = strtol(argv[argi], NULL, 10);
            if (rows < 0 || rows > 255)
            {
                fprintf(stderr, "Error: --it-v00-cut-rows must be 0-255 (got '%s')\n", argv[argi]);
                return 1;
            }
            itV00CutRows = (uint8_t)rows;
            continue;
        }
        if (!strcmp(arg, "--help") || !strcmp(arg, "-h"))
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
    }

    if (forceOriginalSamples)
    {
        if (codecArgSeen || bitrateArgSeen)
        {
            fprintf(stderr,
                    "Note: --original ignores --codec/--bitrate and forces PCM sample storage.\n");
        }
        encSettings.codec = MOD2RMF_CODEC_PCM;
        encSettings.bitrateKbps = 0;
        resamplerSettings.amigaFilter = MOD2RMF_AMIGA_FILTER_NONE;
        resamplerSettings.targetRate = 0;
    }
    /*
    if (resamplerSettings.targetRate == 11025u)
    {
        // 11025 Hz has shown pitch drift through some codec/decode paths.
        // Snap to 12000 Hz, which aligns with codec-native rate families
        // (notably Opus) and avoids the observed detune.
        fprintf(stderr,
                "Note: --resample-rate 11025 may detune after compression; using 12000 instead.\n");
        resamplerSettings.targetRate = 12000u;
    }
    */

    compressionType = mod2rmf_encoder_resolve(&encSettings);

    if (!sourcePath || !destPath || !file_exists(sourcePath))
    {
        fprintf(stderr, "Error: invalid source or destination path\n");
        return 1;
    }

    /* Vorbis, FLAC, Opus and QOA require the ZMF container. */
    if (mod2rmf_encoder_requires_zmf(encSettings.codec) && !is_zmf_path(destPath))
    {
        fprintf(stderr,
                "Error: %s codec requires ZMF format. "
                "Please use a .zmf output extension.\n",
                mod2rmf_encoder_label(compressionType));
        return 1;
    }

    setupResult = BAE_Setup();
    if (setupResult != BAE_NO_ERROR)
    {
        fprintf(stderr, "Error: BAE_Setup failed (%d)\n", (int)setupResult);
        return 1;
    }

    conv = mod2rmf_converter_create();
    if (!conv)
    {
        BAE_Cleanup();
        return 1;
    }

    (void)tempoMap; /* Reserved for future tempo-map handling. */
    useZmfContainer = is_zmf_path(destPath) || useExtendedPitchRange || useExtendedAdsr;
    conv->resamplerSettings = resamplerSettings;
    conv->forceOriginalSamples = forceOriginalSamples;
    conv->sampleGainDb = sampleGainDb;
    conv->isIt = mod2rmf_path_is_it(sourcePath);
    conv->avoidMidiChannel10 = avoidMidiChannel10;
    if (downOctaveRange)
    {
        /* Opt-in: drop root + sample rate by one octave (pitch-compensated). */
        conv->rootShiftSemitones = (uint8_t)(conv->rootShiftSemitones + 12u);
    }
    conv->stereoSeparation = stereoSeparation;
    conv->itV00CutRows = itV00CutRows;
    conv->useExtendedPitchRange = useExtendedPitchRange;
    conv->maxAdsrStages = useExtendedAdsr
                              ? (uint8_t)BAE_EDITOR_MAX_ADSR_STAGES
                              : (uint8_t)BAE_RMF_MAX_ADSR_STAGES;

    if (!mod2rmf_load_source_data(conv, sourcePath))
    {
        fprintf(stderr, "Error: failed to read source file\n");
        mod2rmf_song_model_dispose(&song);
        mod2rmf_converter_delete(conv);
        BAE_Cleanup();
        return 1;
    }

    {
        struct xmp_test_info testInfo;
        memset(&testInfo, 0, sizeof(testInfo));
        if (xmp_test_module_from_memory(conv->sourceData, (long)conv->sourceSize, &testInfo) != 0)
        {
            fprintf(stderr, "Error: unsupported or invalid tracker module\n");
            mod2rmf_song_model_dispose(&song);
            mod2rmf_converter_delete(conv);
            BAE_Cleanup();
            return 1;
        }
        fprintf(stderr, "Module detected by libxmp: %s (%s)\n",
                testInfo.name[0] ? testInfo.name : "(untitled)",
                testInfo.type[0] ? testInfo.type : "unknown");
    }

    if (!mod2rmf_build_song_model(conv, &song))
    {
        fprintf(stderr, "Error: failed to build song model\n");
        mod2rmf_song_model_dispose(&song);
        mod2rmf_converter_delete(conv);
        BAE_Cleanup();
        return 1;
    }

    /* Spread tracker channels by program so each instrument gets its own
     * MIDI channel where possible. Must run before setup_document because
     * it may increase song.channelCount. */
    if (spreadChannels)
    {
        if (!mod2rmf_spread_channels_by_program(&song, conv->isMod, conv->stereoSeparation))
        {
            fprintf(stderr, "Error: channel spreading failed\n");
            mod2rmf_song_model_dispose(&song);
            mod2rmf_converter_delete(conv);
            BAE_Cleanup();
            return 1;
        }
    }

    /* Ensure channels have correct CC state at loop start so the engine's
     * meta-marker loop-back (which doesn't reset CC state) works properly. */
    if (!mod2rmf_ensure_loop_cc_resets(&song))
    {
        fprintf(stderr, "Error: loop CC reset failed\n");
        mod2rmf_song_model_dispose(&song);
        mod2rmf_converter_delete(conv);
        BAE_Cleanup();
        return 1;
    }

    /* Same for pitch bend - engine keeps bend state across loop-back. */
    if (!mod2rmf_ensure_loop_pitch_bend_resets(&song))
    {
        fprintf(stderr, "Error: loop pitch bend reset failed\n");
        mod2rmf_song_model_dispose(&song);
        mod2rmf_converter_delete(conv);
        BAE_Cleanup();
        return 1;
    }

#if _DEBUG == TRUE
    /* Diagnostic dump: per-virtual-channel event summary (spread mode only) */
    if (spreadChannels)
    {
        uint32_t vch;
        uint32_t maxCh = song.channelCount;
        fprintf(stderr, "\n=== POST-SPREAD EVENT SUMMARY (loopStart=%u loopEnd=%u) ===\n",
                (unsigned)song.loopStartTick, (unsigned)song.loopEndTick);
        for (vch = 0; vch < maxCh; ++vch)
        {
            uint32_t ei;
            uint32_t firstNoteTick = UINT32_MAX, lastNoteTick = 0;
            uint32_t firstNoteCount = 0;
            uint32_t firstCC7Tick = UINT32_MAX, lastCC7Tick = 0;
            uint8_t firstCC7Val = 0, lastCC7Val = 0;
            bool hasCC7 = FALSE;
            uint32_t firstBendTick = UINT32_MAX, lastBendTick = 0;
            uint16_t firstBendVal = 0, lastBendVal = 0;
            bool hasBend = FALSE;

            /* Scan notes */
            for (ei = 0; ei < song.noteCount; ++ei) {
                if (song.notes[ei].sourceChannel == vch) {
                    firstNoteCount++;
                    if (song.notes[ei].startTick < firstNoteTick) firstNoteTick = song.notes[ei].startTick;
                    if (song.notes[ei].startTick > lastNoteTick) lastNoteTick = song.notes[ei].startTick;
                }
            }
            /* Scan CC7 */
            for (ei = 0; ei < song.ccCount; ++ei) {
                if (song.ccEvents[ei].sourceChannel == vch && song.ccEvents[ei].cc == 7) {
                    if (!hasCC7 || song.ccEvents[ei].tick < firstCC7Tick) {
                        firstCC7Tick = song.ccEvents[ei].tick;
                        firstCC7Val = song.ccEvents[ei].value;
                    }
                    if (!hasCC7 || song.ccEvents[ei].tick > lastCC7Tick) {
                        lastCC7Tick = song.ccEvents[ei].tick;
                        lastCC7Val = song.ccEvents[ei].value;
                    }
                    hasCC7 = TRUE;
                }
            }
            /* Scan pitch bend */
            for (ei = 0; ei < song.pitchBendCount; ++ei) {
                if (song.pitchBendEvents[ei].sourceChannel == vch) {
                    if (!hasBend || song.pitchBendEvents[ei].tick < firstBendTick) {
                        firstBendTick = song.pitchBendEvents[ei].tick;
                        firstBendVal = song.pitchBendEvents[ei].value;
                    }
                    if (!hasBend || song.pitchBendEvents[ei].tick > lastBendTick) {
                        lastBendTick = song.pitchBendEvents[ei].tick;
                        lastBendVal = song.pitchBendEvents[ei].value;
                    }
                    hasBend = TRUE;
                }
            }
            if (firstNoteCount == 0) continue;
            fprintf(stderr, "  vCh %u: notes=%u first@%u last@%u", vch, firstNoteCount, firstNoteTick, lastNoteTick);
            if (hasCC7) fprintf(stderr, "  CC7: first=%u@%u last=%u@%u", firstCC7Val, firstCC7Tick, lastCC7Val, lastCC7Tick);
            else fprintf(stderr, "  CC7: NONE");
            if (hasBend) fprintf(stderr, "  Bend: first=0x%04X@%u last=0x%04X@%u", firstBendVal, firstBendTick, lastBendVal, lastBendTick);
            else fprintf(stderr, "  Bend: NONE");
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "=== END EVENT SUMMARY ===\n\n");
    }
#endif

    if (!mod2rmf_setup_document(conv, &song, sourcePath))
    {
        fprintf(stderr, "Error: document setup failed\n");
        mod2rmf_song_model_dispose(&song);
        mod2rmf_converter_delete(conv);
        BAE_Cleanup();
        return 1;
    }

    /* Emit MIDI loop markers if the song has an infinite loop */
    if (song.loopEnabled)
    {
        BAERmfEditorDocument_SetMidiLoopMarkers(conv->document,
                                                TRUE,
                                                song.loopStartTick,
                                                song.loopEndTick,
                                                -1); /* -1 = loop forever */
        fprintf(stderr, "Loop detected: start=%u end=%u ticks\n",
                (unsigned)song.loopStartTick, (unsigned)song.loopEndTick);
    }

    if (!mod2rmf_setup_samples(conv, &song))
    {
        fprintf(stderr, "Error: sample setup failed\n");
        mod2rmf_song_model_dispose(&song);
        mod2rmf_converter_delete(conv);
        BAE_Cleanup();
        return 1;
    }

    /* Analyze channel usage and compute tracker→MIDI channel mapping */
    {
        ChannelProfile profiles[MOD2RMF_MAX_CHANNELS];

        mod2rmf_analyze_channel_usage(&song, profiles, song.channelCount);
        mod2rmf_compute_channel_map(profiles,
                        song.channelCount,
                        &conv->channelMap,
                        conv->avoidMidiChannel10);

        #if _DEBUG == TRUE
        {
            uint32_t ci;
            for (ci = 0; ci < song.channelCount; ++ci)
            {
                if (profiles[ci].used)
                {
                    fprintf(stderr, "[mod2rmf] Channel map: tracker ch %u -> MIDI ch %u (%u notes, %u ranges)\n",
                            ci, conv->channelMap.trackerToMidi[ci], profiles[ci].noteCount, profiles[ci].rangeCount);
                }
            }
        }
        #endif

        mod2rmf_channel_profile_cleanup(profiles, song.channelCount);
    }
    if (useExtendedPitchRange && conv->document)
    {
        int32_t engineConfig = 0;
        (void)BAERmfEditorDocument_GetEngineConfig(conv->document, &engineConfig);
        engineConfig |= (int32_t)(SONG_CONFIG_HAS_EXTENDED_PITCH_RANGE |
                                  SONG_CONFIG_EXTENDED_PITCH_RANGE_ON);
        BAERmfEditorDocument_SetEngineConfig(conv->document, engineConfig);
    }
    if (!mod2rmf_setup_tracks(conv, &song, &conv->channelMap) ||
        !mod2rmf_setup_instrument_ext(conv, &song, useZmfContainer) ||
        !mod2rmf_write_song_cc_events(conv, &song) ||
        !mod2rmf_write_song_pitch_bend_events(conv, &song) ||
        !mod2rmf_write_song_notes(conv, &song, useZmfContainer) ||
        !mod2rmf_write_song_tempo_events(conv, &song) ||
        !mod2rmf_encoder_apply(conv->document, &encSettings, compressionType) ||
        !mod2rmf_save_document(conv, destPath))
    {
        fprintf(stderr, "Error: conversion failed\n");
        mod2rmf_song_model_dispose(&song);
        mod2rmf_converter_delete(conv);
        BAE_Cleanup();
        return 1;
    }
    fprintf(stdout, "Conversion complete: %s -> %s\n", sourcePath, destPath);

    mod2rmf_song_model_dispose(&song);
    mod2rmf_converter_delete(conv);
    BAE_Cleanup();
    return 0;
}

