/*
 * playbae_tui.c
 *
 * Linux ncurses textual UI for NeoBAE playback.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <signal.h>
#include <stdint.h>
#include <limits.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <ncurses.h>

#include <NeoBAE.h>
#include <X_Formats.h>
#include <BAE_API.h>
#ifdef SUPPORT_BAESCRIPT
#include "baescript.h"
#endif

#ifdef main
#undef main
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static volatile int gInterrupt = 0;
static int gVolumePct = 100;

#ifdef SUPPORT_KARAOKE
static int  gKaraokeEnabled = 1;
static int  gKaraokeDetected = 0;
static int  gKaraokeProbeMode = 0;
static int  gKaraokeAwaitLive = 0;
static int  gKaraokeUseMetaFallback = 0;
static char gKaraokeCurrent[256]  = {0};
static size_t gKaraokeCurrentLen  = 0;
static char gKaraokePrevious[256] = {0};
static char gKaraokeLastFrag[128] = {0};
static int  gKaraokeHaveMeta      = 0;
#endif

typedef enum {
    TRACK_NONE = 0,
    TRACK_SONG,
    TRACK_SOUND
} TrackKind;

typedef struct {
    BAEMixer mixer;
    BAESong song;
    BAESound sound;
    TrackKind kind;
    int paused;
    int sampleRate;
    uint32_t totalMs;
    char currentPath[PATH_MAX];
} PlayerState;

typedef struct {
    char status[256];
    int selectedChannel;
    int channelMuted[16];
    float vuLeftLevel;
    float vuRightLevel;
    int vuPeakLeft;
    int vuPeakRight;
    uint64_t vuPeakHoldUntilMs;
} UiState;

typedef struct {
    int quiet;
    int verbose;
    int karaoke;
    int volumePct;
    int sampleRate;
    int mono;
    int twoPoint;
    int maxVoices;
    int showHelp;
    char file[PATH_MAX];
    char bank[PATH_MAX];
    char script[PATH_MAX];
} TuiOptions;

static const float TUI_MAIN_VU_ALPHA = 0.12f;
static const float TUI_VU_GAIN = 6.0f;
static const uint64_t TUI_VU_PEAK_HOLD_MS = 600;

static const char *kUsage =
    "Usage: neobae-tui [options] -f <song-or-audio-file>\n"
    "       neobae-tui [options] <song-or-audio-file>\n"
    "Options:\n"
    "  -f <file>   Input song/audio file\n"
    "  -p <bank>   Load patches bank (hsb/zsb/sf2/dls depending on build)\n"
    "  -v <pct>    Master volume percent (0-400, default 100)\n"
    "  -mr <hz>    Mixer sample rate in Hz (default 44100)\n"
    "  -ns         Mono output\n"
    "  -2p         2-point interpolation\n"
    "  -mv <n>     Max MIDI voices (default 64)\n"
    "  --script <file>  Load BAEScript and apply directives during playback\n"
    "  -k          Enable karaoke lyric display\n"
    "  -dk         Disable karaoke lyric display\n"
    "  -q          Quiet startup\n"
    "  -d          Verbose startup\n"
    "  -h,--help   Show this help\n";

static void print_cli_help(FILE *out)
{
#ifdef _VERSION
    const char *appVersion = _VERSION;
#else
    const char *appVersion = "unknown";
#endif
    const char *features = BAE_GetFeatureString();

    fprintf(out, "neobae-tui version: %s\n", appVersion);
    fprintf(out, "features: %s\n\n", (features && features[0]) ? features : "(none)");
    fprintf(out, "%s", kUsage);
}

extern BAEResult BAESong_SetLyricCallback(BAESong song,
    GM_SongLyricCallbackProcPtr pCallback, void *callbackReference);

static void intHandler(int dummy)
{
    (void)dummy;
    gInterrupt = 1;
}

static int parse_arg_value(int argc, char **argv, int *idx, char *out, size_t outSize)
{
    if (*idx + 1 >= argc) return 0;
    (*idx)++;
    strncpy(out, argv[*idx], outSize - 1);
    out[outSize - 1] = '\0';
    return 1;
}

static void options_init(TuiOptions *opt)
{
    memset(opt, 0, sizeof(*opt));
    opt->karaoke = 1;
    opt->volumePct = 100;
    opt->sampleRate = 44100;
    opt->maxVoices = 64;
}

static int options_parse(int argc, char **argv, TuiOptions *opt)
{
    int i;
    char tmp[256];

    options_init(opt);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            opt->showHelp = 1;
            return 1;
        } else if (strcmp(argv[i], "-q") == 0) {
            opt->quiet = 1;
            opt->verbose = 0;
        } else if (strcmp(argv[i], "-d") == 0) {
            opt->verbose = 1;
            opt->quiet = 0;
        } else if (strcmp(argv[i], "-k") == 0) {
            opt->karaoke = 1;
        } else if (strcmp(argv[i], "-dk") == 0) {
            opt->karaoke = 0;
        } else if (strcmp(argv[i], "-f") == 0) {
            if (!parse_arg_value(argc, argv, &i, opt->file, sizeof(opt->file))) return 0;
        } else if (strcmp(argv[i], "-p") == 0) {
            if (!parse_arg_value(argc, argv, &i, opt->bank, sizeof(opt->bank))) return 0;
        } else if (strcmp(argv[i], "--script") == 0) {
            if (!parse_arg_value(argc, argv, &i, opt->script, sizeof(opt->script))) return 0;
        } else if (strcmp(argv[i], "-v") == 0) {
            if (!parse_arg_value(argc, argv, &i, tmp, sizeof(tmp))) return 0;
            opt->volumePct = atoi(tmp);
            if (opt->volumePct < 0) opt->volumePct = 0;
            if (opt->volumePct > 400) opt->volumePct = 400;
        } else if (strcmp(argv[i], "-mr") == 0) {
            if (!parse_arg_value(argc, argv, &i, tmp, sizeof(tmp))) return 0;
            opt->sampleRate = atoi(tmp);
            if (opt->sampleRate <= 0) opt->sampleRate = 44100;
        } else if (strcmp(argv[i], "-mv") == 0) {
            if (!parse_arg_value(argc, argv, &i, tmp, sizeof(tmp))) return 0;
            opt->maxVoices = atoi(tmp);
            if (opt->maxVoices < BAE_MIN_VOICES) opt->maxVoices = BAE_MIN_VOICES;
            if (opt->maxVoices > BAE_MAX_VOICES) opt->maxVoices = BAE_MAX_VOICES;
        } else if (strcmp(argv[i], "-ns") == 0) {
            opt->mono = 1;
        } else if (strcmp(argv[i], "-2p") == 0) {
            opt->twoPoint = 1;
        } else if (argv[i][0] == '-') {
            return 0;
        } else if (opt->file[0] == '\0') {
            strncpy(opt->file, argv[i], sizeof(opt->file) - 1);
            opt->file[sizeof(opt->file) - 1] = '\0';
        }
    }

    return 1;
}

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static BAE_UNSIGNED_FIXED volume_pct_to_fixed(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return FLOAT_TO_UNSIGNED_FIXED(pct / 100.0);
}

static void apply_output_gain(BAEMixer mixer)
{
    BAEMixer_SetOutputGain(mixer, gVolumePct);
}

static const char *BAE_GetErrorString(BAEResult err)
{
    switch (err) {
    case BAE_NO_ERROR:               return "No error";
    case BAE_PARAM_ERR:              return "Parameter error";
    case BAE_MEMORY_ERR:             return "Memory error";
    case BAE_BAD_INSTRUMENT:         return "Bad instrument";
    case BAE_BAD_MIDI_DATA:          return "Bad MIDI data";
    case BAE_ALREADY_PAUSED:         return "Already paused";
    case BAE_ALREADY_RESUMED:        return "Already resumed";
    case BAE_DEVICE_UNAVAILABLE:     return "Device unavailable";
    case BAE_NO_SONG_PLAYING:        return "No song playing";
    case BAE_STILL_PLAYING:          return "Still playing";
    case BAE_TOO_MANY_SONGS_PLAYING: return "Too many songs playing";
    case BAE_BAD_FILE:               return "Bad file";
    case BAE_BAD_FILE_TYPE:          return "Bad file type";
    case BAE_FILE_IO_ERROR:          return "File I/O error";
    case BAE_FILE_NOT_FOUND:         return "File not found";
    case BAE_UNSUPPORTED_FORMAT:     return "Unsupported format";
    default:                         return "Unknown error";
    }
}

#ifdef SUPPORT_KARAOKE
static void karaoke_clear_buffers(void)
{
    gKaraokeCurrent[0] = '\0';
    gKaraokeCurrentLen = 0;
    gKaraokePrevious[0] = '\0';
    gKaraokeLastFrag[0] = '\0';
    gKaraokeHaveMeta = 0;
}

static void karaoke_reset(void)
{
    gKaraokeDetected = 0;
    gKaraokeProbeMode = 0;
    gKaraokeAwaitLive = 0;
    gKaraokeUseMetaFallback = 0;
    karaoke_clear_buffers();
}

static void karaoke_newline(void)
{
    if (gKaraokeCurrentLen > 0) {
        strncpy(gKaraokePrevious, gKaraokeCurrent, sizeof(gKaraokePrevious) - 1);
        gKaraokePrevious[sizeof(gKaraokePrevious) - 1] = '\0';
        gKaraokeCurrent[0] = '\0';
        gKaraokeCurrentLen = 0;
    }
    gKaraokeLastFrag[0] = '\0';
}

static void karaoke_add_fragment(const char *frag)
{
    size_t fl;
    size_t ll;
    int cumulativeExtension;
    if (!frag || !frag[0]) return;

    fl = strlen(frag);
    ll = strlen(gKaraokeLastFrag);
    cumulativeExtension = (ll > 0 && fl > ll && strncmp(frag, gKaraokeLastFrag, ll) == 0);

    if (cumulativeExtension) {
        size_t copyLen = fl < sizeof(gKaraokeCurrent) - 1 ? fl : sizeof(gKaraokeCurrent) - 1;
        memcpy(gKaraokeCurrent, frag, copyLen);
        gKaraokeCurrentLen = copyLen;
        gKaraokeCurrent[gKaraokeCurrentLen] = '\0';
    } else {
        size_t space = sizeof(gKaraokeCurrent) - 1 - gKaraokeCurrentLen;
        size_t appendLen = fl < space ? fl : space;
        if (appendLen > 0) {
            memcpy(gKaraokeCurrent + gKaraokeCurrentLen, frag, appendLen);
            gKaraokeCurrentLen += appendLen;
            gKaraokeCurrent[gKaraokeCurrentLen] = '\0';
        }
    }

    strncpy(gKaraokeLastFrag, frag, sizeof(gKaraokeLastFrag) - 1);
    gKaraokeLastFrag[sizeof(gKaraokeLastFrag) - 1] = '\0';
}

static void karaoke_process(const char *text)
{
    const char *p = text;
    const char *seg = p;
    while (1) {
        if (*p == '/' || *p == '\\' || *p == '\0') {
            size_t len = (size_t)(p - seg);
            if (len > 0) {
                char buf[192];
                if (len >= sizeof(buf)) len = sizeof(buf) - 1;
                memcpy(buf, seg, len);
                buf[len] = '\0';
                karaoke_add_fragment(buf);
            }
            if (*p == '/' || *p == '\\') {
                karaoke_newline();
                p++;
                seg = p;
                continue;
            }
            break;
        }
        p++;
    }
}

static void tui_lyric_callback(struct GM_Song *song, const char *lyric,
    uint32_t t_us, void *ref)
{
    (void)song;
    (void)t_us;
    (void)ref;
    if (gKaraokeUseMetaFallback) return;
    if (!gKaraokeEnabled || !lyric) return;
    gKaraokeDetected = 1;
    if (gKaraokeProbeMode) return;
    if (lyric[0] == '\0') {
        karaoke_newline();
        return;
    }
    karaoke_process(lyric);
}

static void tui_meta_callback(void *ctx, struct GM_Song *song, char type,
    void *text, int32_t len, int16_t track)
{
    const char *t = (const char *)text;
    (void)ctx;
    (void)song;
    (void)len;
    (void)track;

    if (!gKaraokeUseMetaFallback) return;
    if (!gKaraokeEnabled || !text) return;

    if (type != 0x05 && type != 0x01) {
        return;
    }

    gKaraokeDetected = 1;
    if (type == 0x05) {
        gKaraokeHaveMeta = 1;
        if (gKaraokeProbeMode) return;
    } else if (type == 0x01) {
        if (gKaraokeHaveMeta) return;
        if (gKaraokeProbeMode) return;
        if (t[0] == '@') {
            karaoke_newline();
            return;
        }
    } else {
        return;
    }

    if (t[0] == '\0') {
        karaoke_newline();
        return;
    }
    karaoke_process(t);
}
#endif

static void ui_set_status(UiState *ui, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ui->status, sizeof(ui->status), fmt, ap);
    va_end(ap);
}

static int is_audio_type(BAEFileType ftype)
{
    switch (ftype) {
    case BAE_WAVE_TYPE:
    case BAE_AIFF_TYPE:
    case BAE_AU_TYPE:
        return 1;
#if USE_MPEG_DECODER == TRUE
    case BAE_MPEG_TYPE:
        return 1;
#endif
#if USE_FLAC_DECODER == TRUE
    case BAE_FLAC_TYPE:
        return 1;
#endif
#if USE_VORBIS_DECODER == TRUE
    case BAE_VORBIS_TYPE:
        return 1;
#endif
#if USE_OPUS_DECODER == TRUE
    case BAE_OPUS_TYPE:
        return 1;
#endif
#if USE_ADP_SUPPORT == TRUE
    case BAE_ADP_TYPE:
        return 1;
#endif
#if USE_QOA_SUPPORT == TRUE
    case BAE_QOA_TYPE:
        return 1;
#endif
    default:
        return 0;
    }
}

static void player_reset_channel_mutes(PlayerState *ps, UiState *ui)
{
    int i;
    for (i = 0; i < 16; i++) {
        ui->channelMuted[i] = 0;
        if (ps->song) BAESong_UnmuteChannel(ps->song, i);
    }
}

static void player_stop(PlayerState *ps, UiState *ui)
{
    if (ps->song) {
        BAESong_Stop(ps->song, FALSE);
        BAESong_Delete(ps->song);
        ps->song = NULL;
    }
    if (ps->sound) {
        BAESound_Stop(ps->sound, FALSE);
        BAESound_Delete(ps->sound);
        ps->sound = NULL;
    }
    ps->kind = TRACK_NONE;
    ps->paused = 0;
    ps->sampleRate = 0;
    ps->totalMs = 0;
    ps->currentPath[0] = '\0';
    ui->vuLeftLevel = 0.0f;
    ui->vuRightLevel = 0.0f;
    ui->vuPeakLeft = 0;
    ui->vuPeakRight = 0;
    ui->vuPeakHoldUntilMs = 0;
#ifdef SUPPORT_KARAOKE
    karaoke_reset();
#endif
}

static BAEResult load_song_from_file(BAEMixer mixer, BAESong song, const char *path, BAEFileType ftype)
{
    if (ftype == BAE_RMF) {
        return BAESong_LoadRmfFromFile(song, (BAEPathName)path, 0, TRUE);
    }
#if USE_XMF_SUPPORT == TRUE && _USING_FLUIDSYNTH == TRUE
    if (ftype == BAE_XMF) {
        return BAESong_LoadXmfFromFile(song, (BAEPathName)path, TRUE);
    }
#endif
#if USE_SF2_SUPPORT == TRUE && _USING_FLUIDSYNTH == TRUE
    if (ftype == BAE_RMI) {
        return BAESong_LoadRmiFromFile(song, (BAEPathName)path, TRUE, TRUE);
    }
#endif
#if USE_RETRO_RINGTONE_SUPPORT == TRUE
    if (ftype == BAE_RINGTONE_IMY || ftype == BAE_RINGTONE_RNG || ftype == BAE_RINGTONE_RTX) {
        unsigned char *midi = NULL;
        uint32_t midiSize = 0;
        BAEResult err = BAERingtone_ConvertToMidiFromFile((BAEPathName)path, ftype, &midi, &midiSize);
        if (err == BAE_NO_ERROR) {
            err = BAESong_LoadMidiFromMemory(song, midi, midiSize, TRUE);
        }
        BAERingtone_FreeMidiBuffer(midi);
        return err;
    }
#endif
    (void)mixer;
    return BAESong_LoadMidiFromFile(song, (BAEPathName)path, TRUE);
}

static BAEResult player_load_and_start(PlayerState *ps, UiState *ui,
    const char *path, char *outMsg, size_t outMsgSize)
{
    BAEFileType ftype;
    BAEResult err;

    player_stop(ps, ui);

    ftype = X_DetermineFileType((BAEPathName)path);
    if (ftype == BAE_INVALID_TYPE) {
        snprintf(outMsg, outMsgSize, "unsupported file type");
        return BAE_BAD_FILE_TYPE;
    }

    if (is_audio_type(ftype)) {
        BAESampleInfo info;
        ps->sound = BAESound_New(ps->mixer);
        if (!ps->sound) {
            snprintf(outMsg, outMsgSize, "BAESound_New failed");
            return BAE_MEMORY_ERR;
        }

        err = BAESound_LoadFileSample(ps->sound, (BAEPathName)path, ftype);
        if (err != BAE_NO_ERROR) {
            snprintf(outMsg, outMsgSize, "load failed: %s", BAE_GetErrorString(err));
            player_stop(ps, ui);
            return err;
        }

        BAESound_SetVolume(ps->sound, volume_pct_to_fixed(gVolumePct));
        err = BAESound_Start(ps->sound, 0, BAE_FIXED_1, 0);
        if (err != BAE_NO_ERROR) {
            snprintf(outMsg, outMsgSize, "start failed: %s", BAE_GetErrorString(err));
            player_stop(ps, ui);
            return err;
        }

        ps->sampleRate = 44100;
        if (BAESound_GetInfo(ps->sound, &info) == BAE_NO_ERROR) {
            ps->sampleRate = (int)(info.sampledRate / 65536);
            if (ps->sampleRate > 0) {
                ps->totalMs = (uint32_t)((uint64_t)info.waveFrames * 1000ull / (uint64_t)ps->sampleRate);
            }
        }
        ps->kind = TRACK_SOUND;
    } else {
        uint32_t lenUs = 0;

        ps->song = BAESong_New(ps->mixer);
        if (!ps->song) {
            snprintf(outMsg, outMsgSize, "BAESong_New failed");
            return BAE_MEMORY_ERR;
        }

        err = load_song_from_file(ps->mixer, ps->song, path, ftype);
        if (err != BAE_NO_ERROR) {
            snprintf(outMsg, outMsgSize, "load failed: %s", BAE_GetErrorString(err));
            player_stop(ps, ui);
            return err;
        }

#ifdef SUPPORT_KARAOKE
        karaoke_reset();
        if (gKaraokeEnabled) {
            if (BAESong_SetLyricCallback(ps->song, tui_lyric_callback, NULL) != BAE_NO_ERROR) {
                gKaraokeUseMetaFallback = 1;
                BAESong_SetMetaEventCallback(ps->song, tui_meta_callback, NULL);
            } else {
                gKaraokeUseMetaFallback = 0;
                BAESong_SetMetaEventCallback(ps->song, NULL, NULL);
            }
            gKaraokeProbeMode = 1;
            gKaraokeAwaitLive = 0;
        }
#endif

        BAESong_Preroll(ps->song);

#ifdef SUPPORT_KARAOKE
        if (gKaraokeEnabled) {
            karaoke_clear_buffers();
            gKaraokeProbeMode = 1;
            gKaraokeAwaitLive = 1;
        }
#endif

        err = BAESong_Start(ps->song, 0);
        if (err != BAE_NO_ERROR) {
            snprintf(outMsg, outMsgSize, "start failed: %s", BAE_GetErrorString(err));
            player_stop(ps, ui);
            return err;
        }

        BAESong_SetLoops(ps->song, 0);
        BAESong_SetVolume(ps->song, volume_pct_to_fixed(gVolumePct));
        BAESong_GetMicrosecondLength(ps->song, &lenUs);
        ps->totalMs = lenUs / 1000;
        ps->kind = TRACK_SONG;
        player_reset_channel_mutes(ps, ui);
    }

    strncpy(ps->currentPath, path, sizeof(ps->currentPath) - 1);
    ps->currentPath[sizeof(ps->currentPath) - 1] = '\0';
    ps->paused = 0;

    snprintf(outMsg, outMsgSize, "playing %s", path);
    return BAE_NO_ERROR;
}

static void player_toggle_pause(PlayerState *ps)
{
    if (ps->kind == TRACK_SONG && ps->song) {
        if (ps->paused) {
            if (BAESong_Resume(ps->song) == BAE_NO_ERROR) ps->paused = 0;
        } else {
            if (BAESong_Pause(ps->song) == BAE_NO_ERROR) ps->paused = 1;
        }
    } else if (ps->kind == TRACK_SOUND && ps->sound) {
        if (ps->paused) {
            if (BAESound_Resume(ps->sound) == BAE_NO_ERROR) ps->paused = 0;
        } else {
            if (BAESound_Pause(ps->sound) == BAE_NO_ERROR) ps->paused = 1;
        }
    }
}

static void player_seek(PlayerState *ps, int deltaSec)
{
    if (ps->kind == TRACK_SONG && ps->song) {
        uint32_t posUs = 0;
        int64_t nextUs;
        BAESong_GetMicrosecondPosition(ps->song, &posUs);
        nextUs = (int64_t)posUs + (int64_t)deltaSec * 1000000ll;
        if (nextUs < 0) nextUs = 0;
        BAESong_SetMicrosecondPosition(ps->song, (uint32_t)nextUs);
    } else if (ps->kind == TRACK_SOUND && ps->sound && ps->sampleRate > 0) {
        uint32_t frame = 0;
        int64_t nextFrame;
        BAESound_GetSamplePlaybackPosition(ps->sound, &frame);
        nextFrame = (int64_t)frame + (int64_t)deltaSec * (int64_t)ps->sampleRate;
        if (nextFrame < 0) nextFrame = 0;
        BAESound_SetSamplePlaybackPosition(ps->sound, (uint32_t)nextFrame);
    }
}

static void player_set_volume(PlayerState *ps, int newPct)
{
    gVolumePct = newPct;
    if (gVolumePct < 0) gVolumePct = 0;
    if (gVolumePct > 400) gVolumePct = 400;

    apply_output_gain(ps->mixer);
    if (ps->song) BAESong_SetVolume(ps->song, volume_pct_to_fixed(gVolumePct));
    if (ps->sound) BAESound_SetVolume(ps->sound, volume_pct_to_fixed(gVolumePct));
}

static void player_toggle_selected_channel(PlayerState *ps, UiState *ui)
{
    int idx = ui->selectedChannel;
    if (!ps->song) {
        ui_set_status(ui, "channel mute only applies to MIDI/RMF songs");
        return;
    }

    if (idx < 0 || idx > 15) return;

    if (ui->channelMuted[idx]) {
        BAESong_UnmuteChannel(ps->song, idx);
        ui->channelMuted[idx] = 0;
        ui_set_status(ui, "channel %d unmuted", idx + 1);
    } else {
        BAESong_MuteChannel(ps->song, idx);
        ui->channelMuted[idx] = 1;
        ui_set_status(ui, "channel %d muted", idx + 1);
    }
}

static uint32_t player_position_ms(PlayerState *ps)
{
    if (ps->kind == TRACK_SONG && ps->song) {
        uint32_t posUs = 0;
        BAESong_GetMicrosecondPosition(ps->song, &posUs);
        return posUs / 1000;
    }
    if (ps->kind == TRACK_SOUND && ps->sound && ps->sampleRate > 0) {
        uint32_t frame = 0;
        BAESound_GetSamplePlaybackPosition(ps->sound, &frame);
        return (uint32_t)((uint64_t)frame * 1000ull / (uint64_t)ps->sampleRate);
    }
    return 0;
}

static int player_is_done(PlayerState *ps)
{
    BAE_BOOL done = TRUE;
    if (ps->kind == TRACK_SONG && ps->song) {
        BAESong_IsDone(ps->song, &done);
        return done ? 1 : 0;
    }
    if (ps->kind == TRACK_SOUND && ps->sound) {
        BAESound_IsDone(ps->sound, &done);
        return done ? 1 : 0;
    }
    return 1;
}

static void ui_update_vu(PlayerState *ps, UiState *ui)
{
    int16_t sL = 0;
    int16_t sR = 0;
    static int16_t vuLeftBuf[16384];
    static int16_t vuRightBuf[16384];
    int16_t out = 0;
    uint64_t nowMs = now_us() / 1000ull;

    if (BAEMixer_GetAudioSampleFrame(ps->mixer, vuLeftBuf, vuRightBuf, &out) == BAE_NO_ERROR) {
        int sampleIndex = 0;
        if (out > 0 && out < (int16_t)(sizeof(vuLeftBuf) / sizeof(vuLeftBuf[0]))) {
            sampleIndex = out / 2;
        }
        sL = vuLeftBuf[sampleIndex];
        sR = vuRightBuf[sampleIndex];

        float rawL = fabsf((float)sL) / 32768.0f * TUI_VU_GAIN;
        float rawR = fabsf((float)sR) / 32768.0f * TUI_VU_GAIN;
        float fL = sqrtf(fminf(1.0f, rawL));
        float fR = sqrtf(fminf(1.0f, rawR));
        int il;
        int ir;

        ui->vuLeftLevel = ui->vuLeftLevel * (1.0f - TUI_MAIN_VU_ALPHA) + fL * TUI_MAIN_VU_ALPHA;
        ui->vuRightLevel = ui->vuRightLevel * (1.0f - TUI_MAIN_VU_ALPHA) + fR * TUI_MAIN_VU_ALPHA;

        il = (int)(ui->vuLeftLevel * 100.0f);
        ir = (int)(ui->vuRightLevel * 100.0f);

        if (il > ui->vuPeakLeft) {
            ui->vuPeakLeft = il;
            ui->vuPeakHoldUntilMs = nowMs + TUI_VU_PEAK_HOLD_MS;
        }
        if (ir > ui->vuPeakRight) {
            ui->vuPeakRight = ir;
            ui->vuPeakHoldUntilMs = nowMs + TUI_VU_PEAK_HOLD_MS;
        }
        if (nowMs > ui->vuPeakHoldUntilMs) {
            ui->vuPeakLeft = il;
            ui->vuPeakRight = ir;
        }
    } else {
        const float decay = (1.0f - TUI_MAIN_VU_ALPHA);
        ui->vuLeftLevel = ui->vuLeftLevel * (1.0f - decay);
        ui->vuRightLevel = ui->vuRightLevel * (1.0f - decay);
        if (ui->vuLeftLevel < 0.001f) ui->vuLeftLevel = 0.0f;
        if (ui->vuRightLevel < 0.001f) ui->vuRightLevel = 0.0f;
    }
}

static void format_ms(char *out, size_t outSize, uint32_t ms)
{
    uint32_t sec = ms / 1000;
    uint32_t min = sec / 60;
    sec %= 60;
    snprintf(out, outSize, "%02u:%02u", (unsigned)min, (unsigned)sec);
}

static void draw_vu_bar_with_peak(int y, int x, int width, const char *label, int pct, int peakPct)
{
    int i;
    int fill;
    int peak;

    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (peakPct < 0) peakPct = 0;
    if (peakPct > 100) peakPct = 100;
    if (width < 10) width = 10;

    fill = (pct * width) / 100;
    peak = (peakPct * width) / 100;
    if (peak >= width) peak = width - 1;

    mvprintw(y, x, "%s [", label);
    for (i = 0; i < width; i++) {
        if (i == peak) addch('|');
        else addch(i < fill ? '#' : '.');
    }
    printw("] %3d%%", pct);
}

static void render_ui(UiState *ui, PlayerState *ps)
{
    int h;
    int w;
    int y;
    int ch;
    uint32_t posMs = player_position_ms(ps);
    int leftPct;
    int rightPct;
    char posA[32];
    char posB[32];

    getmaxyx(stdscr, h, w);
    erase();

    format_ms(posA, sizeof(posA), posMs);
    format_ms(posB, sizeof(posB), ps->totalMs);

    mvprintw(0, 0, "neobae-tui | q quit | ? help");
    mvprintw(1, 0, "Now: %.*s", w - 6, ps->currentPath);

    if (ps->kind == TRACK_NONE) {
        mvprintw(2, 0, "State: idle");
    } else {
        const char *kind = (ps->kind == TRACK_SONG) ? "song" : "audio";
        const char *st = ps->paused ? "paused" : "playing";
        mvprintw(2, 0, "State: %s (%s) | Pos %s / %s | Vol %d%%",
            st, kind, posA, posB, gVolumePct);
    }

    leftPct = (int)(ui->vuLeftLevel * 100.0f);
    rightPct = (int)(ui->vuRightLevel * 100.0f);
    if (leftPct < 0) leftPct = 0;
    if (rightPct < 0) rightPct = 0;
    if (leftPct > 100) leftPct = 100;
    if (rightPct > 100) rightPct = 100;

    mvprintw(4, 0, "Stereo VU");
    draw_vu_bar_with_peak(5, 0, (w > 28 ? w - 28 : 12), "L", leftPct, ui->vuPeakLeft);
    draw_vu_bar_with_peak(6, 0, (w > 28 ? w - 28 : 12), "R", rightPct, ui->vuPeakRight);

    y = 8;
    mvprintw(y++, 0, "MIDI channel selection (Up/Down select, m mute/unmute):");

    for (ch = 0; ch < 16; ch++) {
        int row = y + (ch / 8);
        int col = (ch % 8) * 10;
        const char *flag = ui->channelMuted[ch] ? "M" : "-";
        if (ui->selectedChannel == ch) attron(A_REVERSE);
        mvprintw(row, col, "%2d[%s]", ch + 1, flag);
        if (ui->selectedChannel == ch) attroff(A_REVERSE);
    }

    if (ps->kind != TRACK_SONG) {
        mvprintw(y + 3, 0, "Channel mute applies to MIDI/RMF/XMF songs only.");
    }

#ifdef SUPPORT_KARAOKE
    if (gKaraokeEnabled && gKaraokeDetected) {
        mvprintw(h - 4, 0, "Karaoke:");
        mvprintw(h - 3, 0, "%.*s", w - 1, gKaraokePrevious);
        mvprintw(h - 2, 0, "%.*s", w - 1, gKaraokeCurrent);
    }
#endif

    mvprintw(h - 1, 0,
        "space pause/resume  s stop  <-/-> seek  +/- volume  %.*s",
        w - 58 > 0 ? w - 58 : 0, ui->status);

    refresh();
}

static void init_ncurses(void)
{
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);
    if (has_colors()) {
        start_color();
        use_default_colors();
    }
}

static void print_help_overlay(void)
{
    int h;
    int w;
    int y = 2;
    getmaxyx(stdscr, h, w);

    erase();
    mvprintw(y++, 2, "neobae-tui keys");
    mvprintw(y++, 2, "q, ESC            quit");
    mvprintw(y++, 2, "Space             pause/resume");
    mvprintw(y++, 2, "s                 stop current track");
    mvprintw(y++, 2, "Left/Right        seek -/+5 seconds");
    mvprintw(y++, 2, "+/-               volume up/down");
    mvprintw(y++, 2, "Up/Down, j/k      select MIDI channel");
    mvprintw(y++, 2, "m                 mute/unmute selected channel");
    mvprintw(y++, 2, "");
    mvprintw(y++, 2, "Press any key to return");
    mvprintw(h - 1, 0, "%*s", w, "");
    refresh();

    nodelay(stdscr, FALSE);
    getch();
    nodelay(stdscr, TRUE);
}

int main(int argc, char **argv)
{
    BAEResult err;
    PlayerState ps;
    UiState ui;
    TuiOptions opt;
    BAEBankToken bankToken = 0;
    uint64_t lastRender = 0;
    char msg[256];
#ifdef SUPPORT_BAESCRIPT
    BAEScript_Context *script = NULL;
    uint32_t scriptLenMs = 0;
#endif

    memset(&ps, 0, sizeof(ps));
    memset(&ui, 0, sizeof(ui));

    if (!options_parse(argc, argv, &opt)) {
        fprintf(stderr, "%s", kUsage);
        return 1;
    }

    if (opt.showHelp) {
        print_cli_help(stdout);
        return 0;
    }

    if (opt.file[0] == '\0') {
        fprintf(stderr, "%s", kUsage);
        return 1;
    }

    if (opt.script[0] != '\0') {
#ifdef SUPPORT_BAESCRIPT
        script = BAEScript_LoadFile(opt.script);
        if (!script) {
            fprintf(stderr, "neobae-tui: failed loading BAEScript '%s'\n", opt.script);
            return 1;
        }
#else
        fprintf(stderr, "neobae-tui: --script is not available in this build\n");
        return 1;
#endif
    }

    gVolumePct = opt.volumePct;
#ifdef SUPPORT_KARAOKE
    gKaraokeEnabled = opt.karaoke ? 1 : 0;
#endif

    if (!opt.quiet) {
        fprintf(stdout, "neobae-tui starting: file=%s\n", opt.file);
        if (opt.verbose) {
            fprintf(stdout, "  sampleRate=%d mono=%d interp2p=%d maxVoices=%d volume=%d%% karaoke=%d\n",
                opt.sampleRate, opt.mono, opt.twoPoint, opt.maxVoices, gVolumePct, opt.karaoke ? 1 : 0);
        }
    }

    signal(SIGINT, intHandler);

    ps.mixer = BAEMixer_New();
    if (!ps.mixer) {
        fprintf(stderr, "playbae-tui: BAEMixer_New failed\n");
        return 1;
    }

    err = BAEMixer_Open(ps.mixer,
        (BAERate)opt.sampleRate,
        opt.twoPoint ? BAE_2_POINT_INTERPOLATION : BAE_LINEAR_INTERPOLATION,
        BAE_USE_16 | (opt.mono ? 0 : BAE_USE_STEREO),
        (int16_t)opt.maxVoices,
        8,
        64,
        TRUE);
    if (err != BAE_NO_ERROR) {
        fprintf(stderr, "neobae-tui: BAEMixer_Open failed (%d: %s)\n",
            err, BAE_GetErrorString(err));
        BAEMixer_Delete(ps.mixer);
        return 1;
    }

    apply_output_gain(ps.mixer);

    if (opt.bank[0]) {
        err = BAEMixer_AddBankFromFile(ps.mixer, (BAEPathName)opt.bank, &bankToken);
        if (err != BAE_NO_ERROR) {
            fprintf(stderr, "neobae-tui: failed loading bank '%s' (%d: %s)\n",
                opt.bank, err, BAE_GetErrorString(err));
            BAEMixer_Delete(ps.mixer);
            return 1;
        }
    } else {
#ifdef _BUILT_IN_PATCHES
        err = BAEMixer_LoadBuiltinBank(ps.mixer, &bankToken);
        if (err != BAE_NO_ERROR) {
            fprintf(stderr, "neobae-tui: failed loading built-in bank (%d: %s)\n",
                err, BAE_GetErrorString(err));
            BAEMixer_Delete(ps.mixer);
            return 1;
        }
#else
        (void)bankToken;
#endif
    }

    ui.selectedChannel = 0;

    if (player_load_and_start(&ps, &ui, opt.file, msg, sizeof(msg)) != BAE_NO_ERROR) {
        fprintf(stderr, "neobae-tui: %s\n", msg);
        BAEMixer_Delete(ps.mixer);
#ifdef SUPPORT_BAESCRIPT
        if (script) BAEScript_Free(script);
#endif
        return 1;
    }
    ui_set_status(&ui, "%s", msg);

#ifdef SUPPORT_BAESCRIPT
    if (script && ps.song) {
        BAEScript_SetSong(script, ps.song);
        BAEScript_SetExporting(script, FALSE);
        scriptLenMs = ps.totalMs;
        BAEScript_Tick(script, 0, scriptLenMs);
    }
#endif

    init_ncurses();

    while (!gInterrupt) {
        int key;

        BAEMixer_ServiceStreams(ps.mixer);
        ui_update_vu(&ps, &ui);

#ifdef SUPPORT_KARAOKE
        if (gKaraokeEnabled && gKaraokeAwaitLive && ps.kind == TRACK_SONG && ps.song) {
            /* First post-start service pass: drop any queued preroll residue,
             * then allow normal karaoke events. */
            karaoke_clear_buffers();
            gKaraokeProbeMode = 0;
            gKaraokeAwaitLive = 0;
        }
#endif

    #ifdef SUPPORT_BAESCRIPT
        if (script && ps.song) {
            uint32_t posMs = player_position_ms(&ps);
            BAEScript_Tick(script, posMs, scriptLenMs);
        }
    #endif

        /* Auto-stop is intentionally disabled because some backends report
         * transient done states while playback is still active, which causes
         * false idle transitions and breaks controls. */

        while ((key = getch()) != ERR) {
            if (key == 'q' || key == 27) {
                gInterrupt = 1;
                break;
            } else if (key == '?') {
                print_help_overlay();
            } else if (key == KEY_UP || key == 'k') {
                ui.selectedChannel = (ui.selectedChannel + 15) % 16;
            } else if (key == KEY_DOWN || key == 'j') {
                ui.selectedChannel = (ui.selectedChannel + 1) % 16;
            } else if (key == 'm' || key == 'M') {
                player_toggle_selected_channel(&ps, &ui);
            } else if (key == ' ') {
                player_toggle_pause(&ps);
            } else if (key == 's') {
                player_stop(&ps, &ui);
                ui_set_status(&ui, "stopped");
            } else if (key == KEY_LEFT) {
                player_seek(&ps, -5);
            } else if (key == KEY_RIGHT) {
                player_seek(&ps, 5);
            } else if (key == '+' || key == '=') {
                player_set_volume(&ps, gVolumePct + 5);
                ui_set_status(&ui, "volume %d%%", gVolumePct);
            } else if (key == '-') {
                player_set_volume(&ps, gVolumePct - 5);
                ui_set_status(&ui, "volume %d%%", gVolumePct);
            } else if (key == KEY_RESIZE) {
                ui_set_status(&ui, "resized");
            }
        }

        if (now_us() - lastRender >= 80000) {
            render_ui(&ui, &ps);
            lastRender = now_us();
        }

        BAE_WaitMicroseconds(12000);
    }

    endwin();
    player_stop(&ps, &ui);
#ifdef SUPPORT_BAESCRIPT
    if (script) BAEScript_Free(script);
#endif
    BAEMixer_Delete(ps.mixer);
    return 0;
}
