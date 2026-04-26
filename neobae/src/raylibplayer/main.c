/* Prevent wingdi.h / winuser.h from clashing with raylib's Rectangle,
   CloseWindow, ShowCursor etc. on Windows builds. */
#if defined(_WIN32) || defined(__CYGWIN__)
#define Rectangle WinRectangle
#define CloseWindow WinCloseWindow
#define ShowCursor WinShowCursor
#define PlaySound WinPlaySound
#define LoadImage WinLoadImage
#define DrawText WinDrawText
#define DrawTextEx WinDrawTextEx
#endif

#include "NeoBAE.h"
#include "GenSF2_FluidSynth.h"

#if defined(_WIN32) || defined(__CYGWIN__)
#undef Rectangle
#undef CloseWindow
#undef ShowCursor
#undef PlaySound
#undef LoadImage
#undef DrawText
#undef DrawTextEx
#endif

#include "raylib.h"

#ifdef EMBED_TTF_FONT
#include "embedded_font.h"
#endif

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WINDOW_WIDTH 1280
#define WINDOW_HEIGHT 760
#define CHANNEL_COUNT 16
#define CHANNELS_PER_ROW 8
#define TRACE_POINTS 96
#define PI_F 3.14159265358979323846f

typedef enum PlayerState {
    PLAYER_EMPTY = 0,
    PLAYER_PLAYING,
    PLAYER_PAUSED,
    PLAYER_STOPPED
} PlayerState;

typedef struct ChannelViz {
    float phase;
    float level;
    float history[TRACE_POINTS];
    int writeIndex;
} ChannelViz;

typedef struct Button {
    Rectangle bounds;
    const char *label;
} Button;

typedef struct MiniPlayerApp {
    BAEMixer mixer;
    BAESong song;
    BAESound sound;
    BAEBankToken bankToken;
    PlayerState state;

    ChannelViz channels[CHANNEL_COUNT];
    unsigned char activeNotes[CHANNEL_COUNT][128];
    unsigned char mutedChannels[CHANNEL_COUNT];

    char songPath[1024];
    char songName[256];
    char statusText[256];
    char bankPath[1024];
    char bankName[256];

    float masterVolume;
    float uiPulse;

    int loopEnabled;
    BAESampleInfo audioInfo;
    int audioIsStereo;

    Font uiFont;
    int uiFontLoaded;
} MiniPlayerApp;

typedef struct SongResumeState {
    int hadSong;
    int wasPlaying;
    int wasPaused;
    char songPath[1024];
    uint32_t posUs;
    unsigned char muted[CHANNEL_COUNT];
} SongResumeState;

static void set_status(MiniPlayerApp *app, const char *text);
static BAEResult load_song_file(MiniPlayerApp *app, const char *path);

static float clampf(float value, float minValue, float maxValue)
{
    if (value < minValue) {
        return minValue;
    }
    if (value > maxValue) {
        return maxValue;
    }
    return value;
}

static float midi_note_to_hz(int note)
{
    return 440.0f * powf(2.0f, ((float)note - 69.0f) / 12.0f);
}

static void path_to_filename(const char *path, char *outName, size_t outNameSize)
{
    const char *base = strrchr(path, '/');
    if (base == NULL) {
        base = strrchr(path, '\\');
    }
    if (base == NULL) {
        base = path;
    } else {
        base += 1;
    }

    snprintf(outName, outNameSize, "%s", base);
}

static int ext_equals(const char *path, const char *ext)
{
    const char *dot = strrchr(path, '.');
    if (dot == NULL) {
        return 0;
    }

    char a[16] = {0};
    char b[16] = {0};
    size_t i = 0;
    for (; dot[i] != '\0' && i < sizeof(a) - 1; ++i) {
        a[i] = (char)tolower((unsigned char)dot[i]);
    }
    for (i = 0; ext[i] != '\0' && i < sizeof(b) - 1; ++i) {
        b[i] = (char)tolower((unsigned char)ext[i]);
    }

    return strcmp(a, b) == 0;
}

static void reset_channel_viz(MiniPlayerApp *app)
{
    memset(app->activeNotes, 0, sizeof(app->activeNotes));
    for (int ch = 0; ch < CHANNEL_COUNT; ++ch) {
        app->channels[ch].phase = 0.0f;
        app->channels[ch].level = 0.0f;
        app->channels[ch].writeIndex = 0;
        memset(app->channels[ch].history, 0, sizeof(app->channels[ch].history));
    }
}

static void reset_channel_mutes(MiniPlayerApp *app)
{
    memset(app->mutedChannels, 0, sizeof(app->mutedChannels));
}

static void apply_channel_mutes(MiniPlayerApp *app)
{
    if (app->song == NULL) {
        return;
    }

    for (int ch = 0; ch < CHANNEL_COUNT; ++ch) {
        if (app->mutedChannels[ch]) {
            BAESong_MuteChannel(app->song, (uint16_t)ch);
        } else {
            BAESong_UnmuteChannel(app->song, (uint16_t)ch);
        }
    }
}

static void toggle_channel_mute(MiniPlayerApp *app, int channel)
{
    if (app->song == NULL || channel < 0 || channel >= CHANNEL_COUNT) {
        return;
    }

    app->mutedChannels[channel] = (unsigned char)!app->mutedChannels[channel];
    if (app->mutedChannels[channel]) {
        BAESong_MuteChannel(app->song, (uint16_t)channel);
        set_status(app, TextFormat("CH%02d muted", channel + 1));
    } else {
        BAESong_UnmuteChannel(app->song, (uint16_t)channel);
        set_status(app, TextFormat("CH%02d unmuted", channel + 1));
    }
}

static void set_status(MiniPlayerApp *app, const char *text)
{
    snprintf(app->statusText, sizeof(app->statusText), "%s", text);
}

static SongResumeState capture_song_resume_state(MiniPlayerApp *app)
{
    SongResumeState state;
    memset(&state, 0, sizeof(state));

    if (app->song != NULL && app->songPath[0] != '\0') {
        state.hadSong = 1;
        state.wasPlaying = (app->state == PLAYER_PLAYING) ? 1 : 0;
        state.wasPaused = (app->state == PLAYER_PAUSED) ? 1 : 0;
        snprintf(state.songPath, sizeof(state.songPath), "%s", app->songPath);
        BAESong_GetMicrosecondPosition(app->song, &state.posUs);
        memcpy(state.muted, app->mutedChannels, sizeof(state.muted));
    }

    return state;
}

static void restore_song_after_bank_change(MiniPlayerApp *app, const SongResumeState *state)
{
    if (!state->hadSong) {
        set_status(app, "Bank loaded");
        return;
    }

    BAEResult reloadResult = load_song_file(app, state->songPath);
    if (reloadResult != BAE_NO_ERROR) {
        set_status(app, TextFormat("Bank loaded, song reload failed (%d)", (int)reloadResult));
        return;
    }

    if (state->posUs > 0) {
        BAESong_SetMicrosecondPosition(app->song, state->posUs);
    }

    memcpy(app->mutedChannels, state->muted, sizeof(state->muted));
    apply_channel_mutes(app);

    if (state->wasPaused) {
        BAESong_Pause(app->song);
        app->state = PLAYER_PAUSED;
        set_status(app, "Bank loaded, song reloaded (paused)");
    } else {
        if (state->wasPlaying) {
            app->state = PLAYER_PLAYING;
        }
        set_status(app, "Bank loaded, song reloaded");
    }
}

static int is_bank_file(const char *path)
{
    return ext_equals(path, ".hsb") ||
           ext_equals(path, ".zsb") ||
           ext_equals(path, ".sf2") ||
           ext_equals(path, ".sf3") ||
           ext_equals(path, ".sfo") ||
           ext_equals(path, ".dls");
}

static const char *get_bank_friendly_name(MiniPlayerApp *app, char *buffer, size_t bufferSize)
{
    if (app->mixer == NULL || app->bankToken == 0) {
        return NULL;
    }

    buffer[0] = '\0';
    if (BAE_GetBankFriendlyName(app->mixer, app->bankToken, buffer, bufferSize) == BAE_NO_ERROR && buffer[0] != '\0') {
        return buffer;
    }
    return NULL;
}

static void set_bank_display_name(MiniPlayerApp *app, const char *path)
{
    char friendlyName[256] = {0};
    char filename[256] = {0};
    const char *displayName = NULL;

    int preferFilename = 0;
    if (path != NULL && path[0] != '\0') {
        if (ext_equals(path, ".sf2") ||
            ext_equals(path, ".sf3") ||
            ext_equals(path, ".sfo") ||
            ext_equals(path, ".dls")) {
            preferFilename = 1;
        }
    }

    if (!preferFilename) {
        displayName = get_bank_friendly_name(app, friendlyName, sizeof(friendlyName));
    }

    if ((displayName == NULL || displayName[0] == '\0') && path != NULL && path[0] != '\0') {
        path_to_filename(path, filename, sizeof(filename));
        displayName = filename;
    }

    if (displayName == NULL || displayName[0] == '\0') {
        displayName = "(built-in)";
    }

    snprintf(app->bankName, sizeof(app->bankName), "%s", displayName);
    if (path != NULL && path[0] != '\0') {
        snprintf(app->bankPath, sizeof(app->bankPath), "%s", path);
    } else {
        app->bankPath[0] = '\0';
    }
}

static int is_audio_file_type(BAEFileType fileType)
{
    switch (fileType) {
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

static void apply_loop_setting(MiniPlayerApp *app)
{
    if (app->song != NULL) {
        BAESong_SetLoops(app->song, app->loopEnabled ? 30000 : 0);
    }
    if (app->sound != NULL) {
        BAESound_SetLoopCount(app->sound, app->loopEnabled ? 0xFFFFFFFFu : 0u);
    }
}

static void apply_master_volume(MiniPlayerApp *app)
{
    if (app->mixer == NULL) {
        return;
    }

    app->masterVolume = clampf(app->masterVolume, 0.0f, 1.0f);

    // Global volume is the effective gain path used by song playback.
    BAEMixer_SetGlobalVolume(app->mixer, FLOAT_TO_UNSIGNED_FIXED(app->masterVolume));
    BAEMixer_SetMasterVolume(app->mixer, FLOAT_TO_UNSIGNED_FIXED(app->masterVolume));

    if (app->song != NULL) {
        BAESong_SetVolume(app->song, FLOAT_TO_UNSIGNED_FIXED(app->masterVolume));
    }
    if (app->sound != NULL) {
        BAESound_SetVolume(app->sound, FLOAT_TO_UNSIGNED_FIXED(app->masterVolume));
    }
}

static void unload_media(MiniPlayerApp *app)
{
    if (app->song != NULL) {
        BAESong_Stop(app->song, FALSE);
        BAESong_Delete(app->song);
        app->song = NULL;
    }

    if (app->sound != NULL) {
        BAESound_Stop(app->sound, FALSE);
        BAESound_Delete(app->sound);
        app->sound = NULL;
    }

    memset(&app->audioInfo, 0, sizeof(app->audioInfo));
    app->audioIsStereo = 0;
}

static BAEResult load_bank_file(MiniPlayerApp *app, const char *path)
{
    SongResumeState resumeState = capture_song_resume_state(app);

    BAEMixer_UnloadBanks(app->mixer);
    app->bankToken = 0;

#if USE_SF2_SUPPORT == TRUE
    GM_UnloadSF2Soundfont();
    GM_SetMixerSF2Mode(FALSE);

    if (ext_equals(path, ".sf2") ||
#if _USING_FLUIDSYNTH == TRUE
        ext_equals(path, ".dls") ||
#endif
#if USE_VORBIS_DECODER == TRUE
        ext_equals(path, ".sf3") ||
        ext_equals(path, ".sfo") ||
#endif
        0) {
#if _BUILT_IN_PATCHES == TRUE && _LOAD_BUILTIN_PATCHES_FOR_SF2 == TRUE
        BAEBankToken builtinToken = 0;
        if (BAEMixer_LoadBuiltinBank(app->mixer, &builtinToken) == BAE_NO_ERROR && builtinToken) {
            BAEMixer_SendBankToBack(app->mixer, builtinToken);
        }
#endif

        OPErr sfErr = GM_LoadSF2Soundfont(path);
        if (sfErr != NO_ERR) {
            set_status(app, TextFormat("SF2 bank load failed (%d)", (int)sfErr));
            return BAE_BAD_BANK;
        }

        GM_SetMixerSF2Mode(TRUE);
        set_bank_display_name(app, path);
        restore_song_after_bank_change(app, &resumeState);
        return BAE_NO_ERROR;
    }
#endif

    BAEBankToken token = 0;
    BAEResult result = BAEMixer_AddBankFromFile(app->mixer, (BAEPathName)path, &token);
    if (result != BAE_NO_ERROR) {
        set_status(app, TextFormat("Bank load failed (%d)", (int)result));
        return result;
    }

    app->bankToken = token;
    set_bank_display_name(app, path);
    restore_song_after_bank_change(app, &resumeState);
    return BAE_NO_ERROR;
}

static BAEResult load_builtin_bank(MiniPlayerApp *app)
{
    SongResumeState resumeState = capture_song_resume_state(app);

    BAEMixer_UnloadBanks(app->mixer);
    app->bankToken = 0;

#if USE_SF2_SUPPORT == TRUE
    GM_UnloadSF2Soundfont();
    GM_SetMixerSF2Mode(FALSE);
#endif

    BAEBankToken token = 0;
    BAEResult result = BAEMixer_LoadBuiltinBank(app->mixer, &token);
    if (result != BAE_NO_ERROR || token == 0) {
        set_status(app, TextFormat("Internal bank load failed (%d)", (int)result));
        return (result == BAE_NO_ERROR) ? BAE_BAD_BANK : result;
    }

    app->bankToken = token;
    set_bank_display_name(app, NULL);
    restore_song_after_bank_change(app, &resumeState);
    return BAE_NO_ERROR;
}

static BAEResult load_sound_file(MiniPlayerApp *app, const char *path, BAEFileType fileType)
{
    unload_media(app);
    reset_channel_mutes(app);

    app->sound = BAESound_New(app->mixer);
    if (app->sound == NULL) {
        app->state = PLAYER_EMPTY;
        set_status(app, "Failed to create sound object");
        return BAE_MEMORY_ERR;
    }

    BAEResult result = BAESound_LoadFileSample(app->sound, (BAEPathName)path, fileType);
    if (result != BAE_NO_ERROR) {
        BAESound_Delete(app->sound);
        app->sound = NULL;
        app->state = PLAYER_EMPTY;
        set_status(app, TextFormat("Audio load failed (%d)", (int)result));
        return result;
    }

    BAESound_GetInfo(app->sound, &app->audioInfo);
    app->audioIsStereo = (app->audioInfo.channels >= 2) ? 1 : 0;

    apply_loop_setting(app);

    BAEResult startResult = BAESound_Start(app->sound, 0, FLOAT_TO_UNSIGNED_FIXED(app->masterVolume), 0);
    if (startResult != BAE_NO_ERROR) {
        BAESound_Delete(app->sound);
        app->sound = NULL;
        app->state = PLAYER_EMPTY;
        set_status(app, TextFormat("Audio start failed (%d)", (int)startResult));
        return startResult;
    }

    app->state = PLAYER_PLAYING;
    snprintf(app->songPath, sizeof(app->songPath), "%s", path);
    path_to_filename(path, app->songName, sizeof(app->songName));
    set_status(app, app->audioIsStereo ? "Playing stereo audio (CH1/CH2)" : "Playing mono audio (CH1)");
    reset_channel_viz(app);
    apply_master_volume(app);
    return BAE_NO_ERROR;
}

static BAEResult load_song_file(MiniPlayerApp *app, const char *path)
{
    unload_media(app);
    reset_channel_mutes(app);

    app->song = BAESong_New(app->mixer);
    if (app->song == NULL) {
        set_status(app, "Failed to create song object");
        app->state = PLAYER_EMPTY;
        return BAE_MEMORY_ERR;
    }

    BAEResult result = BAE_BAD_FILE;

    if (ext_equals(path, ".mid") || ext_equals(path, ".midi") || ext_equals(path, ".kar")) {
        result = BAESong_LoadMidiFromFile(app->song, (char *)path, TRUE);
    } else if (ext_equals(path, ".rmf") || ext_equals(path, ".zmf") || ext_equals(path, ".xmf") || ext_equals(path, ".mxmf")) {
        result = BAESong_LoadRmfFromFile(app->song, (char *)path, 0, TRUE);
    } else if (ext_equals(path, ".rmi")) {
        result = BAESong_LoadRmiFromFile(app->song, (char *)path, TRUE, FALSE);
    } else {
        result = BAESong_LoadMidiFromFile(app->song, (char *)path, TRUE);
        if (result != BAE_NO_ERROR) {
            result = BAESong_LoadRmfFromFile(app->song, (char *)path, 0, TRUE);
        }
        if (result != BAE_NO_ERROR) {
            result = BAESong_LoadRmiFromFile(app->song, (char *)path, TRUE, FALSE);
        }
    }

    if (result != BAE_NO_ERROR) {
        BAESong_Delete(app->song);
        app->song = NULL;
        app->state = PLAYER_EMPTY;
        set_status(app, TextFormat("Load failed (%d)", (int)result));
        return result;
    }

    apply_loop_setting(app);
    apply_channel_mutes(app);
    BAESong_Preroll(app->song);
    BAESong_Start(app->song, 0);
    app->state = PLAYER_PLAYING;

    snprintf(app->songPath, sizeof(app->songPath), "%s", path);
    path_to_filename(path, app->songName, sizeof(app->songName));
    set_status(app, "Playing");
    reset_channel_viz(app);
    apply_master_volume(app);

    return BAE_NO_ERROR;
}

static void update_channel_waveforms(MiniPlayerApp *app, float dt)
{
    if (app->song == NULL && app->sound == NULL) {
        return;
    }

    if (app->sound != NULL) {
        float decay = (app->state == PLAYER_PAUSED) ? 0.92f : 0.82f;
        for (int ch = 0; ch < CHANNEL_COUNT; ++ch) {
            app->channels[ch].level *= decay;
            if (app->channels[ch].level < 0.0002f) {
                app->channels[ch].level = 0.0f;
            }
        }

        if (app->state == PLAYER_PLAYING) {
            uint32_t posFrames = 0;
            BAESound_GetSamplePlaybackPosition(app->sound, &posFrames);
            float step = 2.0f * PI_F * 0.0015f;
            float leftWave = sinf((float)posFrames * step);
            float rightWave = sinf((float)posFrames * step * 1.13f + 0.45f);

            app->channels[0].level += (0.72f - app->channels[0].level) * 0.30f;
            if (app->audioIsStereo) {
                app->channels[1].level += (0.72f - app->channels[1].level) * 0.30f;
            }

            app->channels[0].phase += 2.0f * PI_F * 3.2f * dt;
            if (app->channels[0].phase > 2.0f * PI_F) {
                app->channels[0].phase = fmodf(app->channels[0].phase, 2.0f * PI_F);
            }
            app->channels[0].history[app->channels[0].writeIndex] = leftWave * app->channels[0].level;
            app->channels[0].writeIndex = (app->channels[0].writeIndex + 1) % TRACE_POINTS;

            if (app->audioIsStereo) {
                app->channels[1].phase += 2.0f * PI_F * 3.6f * dt;
                if (app->channels[1].phase > 2.0f * PI_F) {
                    app->channels[1].phase = fmodf(app->channels[1].phase, 2.0f * PI_F);
                }
                app->channels[1].history[app->channels[1].writeIndex] = rightWave * app->channels[1].level;
                app->channels[1].writeIndex = (app->channels[1].writeIndex + 1) % TRACE_POINTS;
            }
        }

        for (int ch = 2; ch < CHANNEL_COUNT; ++ch) {
            app->channels[ch].history[app->channels[ch].writeIndex] = 0.0f;
            app->channels[ch].writeIndex = (app->channels[ch].writeIndex + 1) % TRACE_POINTS;
        }

        if (!app->audioIsStereo) {
            app->channels[1].history[app->channels[1].writeIndex] = 0.0f;
            app->channels[1].writeIndex = (app->channels[1].writeIndex + 1) % TRACE_POINTS;
            app->channels[1].level *= 0.80f;
        }
        return;
    }

    for (int ch = 0; ch < CHANNEL_COUNT; ++ch) {
        if (app->state == PLAYER_PLAYING) {
            BAESong_GetActiveNotes(app->song, (unsigned char)ch, app->activeNotes[ch]);
        } else {
            memset(app->activeNotes[ch], 0, sizeof(app->activeNotes[ch]));
        }

        int dominantNote = 48 + ch;
        int dominantVelocity = 0;
        float energy = 0.0f;
        int activeCount = 0;

        for (int n = 0; n < 128; ++n) {
            int velocity = app->activeNotes[ch][n];
            if (velocity > 0) {
                activeCount++;
                energy += ((float)velocity / 127.0f);
                if (velocity > dominantVelocity) {
                    dominantVelocity = velocity;
                    dominantNote = n;
                }
            }
        }

        float targetLevel = 0.0f;
        if (activeCount > 0) {
            targetLevel = clampf(energy * 0.13f, 0.0f, 1.0f);
        }

        float blend = (activeCount > 0) ? 0.30f : 0.08f;
        app->channels[ch].level += (targetLevel - app->channels[ch].level) * blend;
        app->channels[ch].level = clampf(app->channels[ch].level, 0.0f, 1.0f);

        float frequency = midi_note_to_hz(dominantNote);
        app->channels[ch].phase += (2.0f * PI_F * frequency * dt);
        if (app->channels[ch].phase > (2.0f * PI_F)) {
            app->channels[ch].phase = fmodf(app->channels[ch].phase, 2.0f * PI_F);
        }

        float harmonic = sinf(app->channels[ch].phase * 0.5f + (float)ch * 0.6f) * 0.28f;
        float sample = (sinf(app->channels[ch].phase) + harmonic) * app->channels[ch].level;

        app->channels[ch].history[app->channels[ch].writeIndex] = sample;
        app->channels[ch].writeIndex = (app->channels[ch].writeIndex + 1) % TRACE_POINTS;
    }
}

static int song_is_done(MiniPlayerApp *app)
{
    if (app->song != NULL) {
        BAE_BOOL done = FALSE;
        if (BAESong_IsDone(app->song, &done) != BAE_NO_ERROR) {
            return 0;
        }
        return done ? 1 : 0;
    }

    if (app->sound == NULL) {
        return 0;
    }

    BAE_BOOL done = FALSE;
    if (BAESound_IsDone(app->sound, &done) != BAE_NO_ERROR) {
        return 0;
    }
    return done ? 1 : 0;
}

static Color color_blend(Color a, Color b, float t)
{
    t = clampf(t, 0.0f, 1.0f);
    Color c;
    c.r = (unsigned char)((1.0f - t) * (float)a.r + t * (float)b.r);
    c.g = (unsigned char)((1.0f - t) * (float)a.g + t * (float)b.g);
    c.b = (unsigned char)((1.0f - t) * (float)a.b + t * (float)b.b);
    c.a = (unsigned char)((1.0f - t) * (float)a.a + t * (float)b.a);
    return c;
}

static void draw_channel_box(const MiniPlayerApp *app, int ch, Rectangle box)
{
    const ChannelViz *viz = &app->channels[ch];
    int muted = app->mutedChannels[ch] ? 1 : 0;

    Color tileOuter = (Color){26, 34, 48, 255};
    Color tileInnerA = muted ? (Color){38, 24, 31, 255} : (Color){16, 36, 75, 255};
    Color tileInnerB = muted ? (Color){34, 14, 20, 255} : (Color){20, 13, 37, 255};
    Color pulseColorA = (Color){29, 209, 161, 255};
    Color pulseColorB = (Color){255, 155, 68, 255};
    Color strokeColor = muted
        ? (Color){170, 82, 92, 255}
        : color_blend((Color){61, 74, 103, 255}, (Color){255, 174, 84, 255}, viz->level);

    DrawRectangleRounded(box, 0.12f, 8, tileOuter);

    Rectangle inner = {box.x + 5, box.y + 5, box.width - 10, box.height - 10};
    DrawRectangleGradientV((int)inner.x, (int)inner.y, (int)inner.width, (int)inner.height, tileInnerA, tileInnerB);

    DrawRectangleRoundedLines(box, 0.12f, 8, strokeColor);

    Rectangle topBand = {inner.x, inner.y, inner.width, 24};
    DrawRectangleGradientH((int)topBand.x, (int)topBand.y, (int)topBand.width, (int)topBand.height,
                           (Color){13, 18, 29, 220}, (Color){18, 31, 42, 220});

    DrawText(TextFormat("CH%02d", ch + 1), (int)inner.x + 8, (int)inner.y + 6, 12, (Color){219, 225, 232, 255});
    if (muted) {
        DrawText("M", (int)(inner.x + inner.width - 14.0f), (int)inner.y + 6, 12, (Color){255, 144, 144, 255});
    }

    Rectangle waveArea = {inner.x + 6, inner.y + 30, inner.width - 12, inner.height - 38};

    Color baseLine = (Color){69, 91, 133, 130};
    DrawLine((int)waveArea.x, (int)(waveArea.y + waveArea.height * 0.5f),
             (int)(waveArea.x + waveArea.width), (int)(waveArea.y + waveArea.height * 0.5f), baseLine);

    Color waveColor = muted
        ? (Color){148, 95, 108, 220}
        : color_blend(pulseColorA, pulseColorB, 0.35f + viz->level * 0.65f);
    for (int i = 0; i < TRACE_POINTS - 1; ++i) {
        int aIdx = (viz->writeIndex + i) % TRACE_POINTS;
        int bIdx = (viz->writeIndex + i + 1) % TRACE_POINTS;
        float x0 = waveArea.x + (waveArea.width * (float)i) / (float)(TRACE_POINTS - 1);
        float x1 = waveArea.x + (waveArea.width * (float)(i + 1)) / (float)(TRACE_POINTS - 1);
        float y0 = waveArea.y + waveArea.height * 0.5f - viz->history[aIdx] * (waveArea.height * 0.46f);
        float y1 = waveArea.y + waveArea.height * 0.5f - viz->history[bIdx] * (waveArea.height * 0.46f);
        DrawLineEx((Vector2){x0, y0}, (Vector2){x1, y1}, 1.8f, waveColor);
    }

    float meterHeight = waveArea.height * clampf(viz->level, 0.0f, 1.0f);
    Rectangle meter = {waveArea.x + waveArea.width - 5.0f, waveArea.y + waveArea.height - meterHeight, 3.0f, meterHeight};
    DrawRectangleRec(meter,
                     muted
                         ? (Color){115, 70, 80, 190}
                         : color_blend((Color){44, 96, 150, 190}, pulseColorB, viz->level));
}

static int button_clicked(Button button)
{
    Vector2 mouse = GetMousePosition();
    if (!CheckCollisionPointRec(mouse, button.bounds)) {
        return 0;
    }
    return IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
}

static void draw_button(Button button, Color base, Color hover)
{
    Vector2 mouse = GetMousePosition();
    int hot = CheckCollisionPointRec(mouse, button.bounds);
    Color fill = hot ? hover : base;

    DrawRectangleRounded(button.bounds, 0.25f, 8, fill);
    DrawRectangleRoundedLines(button.bounds, 0.25f, 8, (Color){228, 234, 245, 120});

    int tw = MeasureText(button.label, 20);
    DrawText(button.label,
             (int)(button.bounds.x + (button.bounds.width - (float)tw) * 0.5f),
             (int)(button.bounds.y + (button.bounds.height - 20.0f) * 0.5f),
             20,
             (Color){245, 247, 255, 255});
}

static int measure_app_text(const MiniPlayerApp *app, const char *text, float fontSize)
{
    if (app->uiFontLoaded) {
        return (int)MeasureTextEx(app->uiFont, text, fontSize, 1.0f).x;
    }
    return MeasureText(text, (int)fontSize);
}

static void draw_app_text(const MiniPlayerApp *app, const char *text, float x, float y, float fontSize, Color color)
{
    // Snap to integer pixels to reduce subpixel blur when using TTF glyph atlases.
    x = floorf(x + 0.5f);
    y = floorf(y + 0.5f);

    if (app->uiFontLoaded) {
        DrawTextEx(app->uiFont, text, (Vector2){x, y}, fontSize, 1.0f, color);
        return;
    }
    DrawText(text, (int)x, (int)y, (int)fontSize, color);
}

static void draw_progress_bar(Rectangle r, float progress)
{
    DrawRectangleRounded(r, 0.48f, 12, (Color){18, 26, 40, 220});
    DrawRectangleRoundedLines(r, 0.48f, 12, (Color){85, 106, 136, 180});

    Rectangle fill = r;
    fill.width = r.width * clampf(progress, 0.0f, 1.0f);
    DrawRectangleRounded(fill, 0.48f, 12, (Color){55, 175, 255, 220});
}

static int checkbox_clicked(Rectangle box)
{
    Vector2 mouse = GetMousePosition();
    if (!CheckCollisionPointRec(mouse, box)) {
        return 0;
    }
    return IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
}

static void draw_checkbox(const MiniPlayerApp *app, Rectangle box, int checked, const char *label)
{
    DrawRectangleRounded(box, 0.25f, 6, (Color){24, 36, 56, 230});
    DrawRectangleRoundedLines(box, 0.25f, 6, (Color){129, 154, 193, 190});

    if (checked) {
        DrawLineEx((Vector2){box.x + 4.0f, box.y + box.height * 0.55f},
                   (Vector2){box.x + box.width * 0.45f, box.y + box.height - 4.0f},
                   2.2f,
                   (Color){88, 230, 174, 255});
        DrawLineEx((Vector2){box.x + box.width * 0.45f, box.y + box.height - 4.0f},
                   (Vector2){box.x + box.width - 4.0f, box.y + 4.0f},
                   2.2f,
                   (Color){88, 230, 174, 255});
    }

    draw_app_text(app, label, box.x + box.width + 10.0f, box.y + 1.0f, 20.0f, (Color){219, 231, 247, 255});
}

static const char *state_label(PlayerState state)
{
    switch (state) {
        case PLAYER_PLAYING: return "PLAYING";
        case PLAYER_PAUSED: return "PAUSED";
        case PLAYER_STOPPED: return "STOPPED";
        default: return "READY";
    }
}

int main(int argc, char **argv)
{
    InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "NeoBAE Raylib Player");
    SetTargetFPS(60);

    MiniPlayerApp app;
    memset(&app, 0, sizeof(app));
    app.masterVolume = 1.0f;
    app.loopEnabled = 1;
    app.state = PLAYER_EMPTY;
    app.uiFont = GetFontDefault();
    app.uiFontLoaded = 0;
#ifdef EMBED_TTF_FONT
    // Load a larger base atlas so larger headings don't upscale and blur.
    app.uiFont = LoadFontFromMemory(".ttf", embedded_font_data, (int)embedded_font_size, 56, NULL, 0);
    if (app.uiFont.texture.id != 0) {
        app.uiFontLoaded = 1;
        SetTextureFilter(app.uiFont.texture, TEXTURE_FILTER_POINT);
    } else {
        app.uiFont = GetFontDefault();
    }
#endif
    snprintf(app.songName, sizeof(app.songName), "Drop MIDI/RMF/XMF/RMI file here");
    set_status(&app, "Ready");

    app.mixer = BAEMixer_New();
    if (app.mixer == NULL) {
        CloseWindow();
        return 1;
    }

    BAEResult openErr = BAEMixer_Open(app.mixer,
                                      BAE_RATE_44K,
                                      BAE_2_POINT_INTERPOLATION,
                                      BAE_USE_16 | BAE_USE_STEREO,
                                      64,
                                      8,
                                      24,
                                      TRUE);
    if (openErr != BAE_NO_ERROR) {
        BAEMixer_Delete(app.mixer);
        CloseWindow();
        return 1;
    }

    BAEMixer_LoadBuiltinBank(app.mixer, &app.bankToken);
    set_bank_display_name(&app, NULL);
    apply_master_volume(&app);

    if (argc > 1) {
        load_song_file(&app, argv[1]);
    }

    while (!WindowShouldClose()) {
        float dt = GetFrameTime();
        app.uiPulse += dt;

        if (IsFileDropped()) {
            FilePathList dropped = LoadDroppedFiles();
            if (dropped.count > 0) {
                const char *dropPath = dropped.paths[0];
                BAEFileType fileType = X_DetermineFileType(dropPath);

                if (is_bank_file(dropPath)) {
                    load_bank_file(&app, dropPath);
                } else if (is_audio_file_type(fileType)) {
                    load_sound_file(&app, dropPath, fileType);
                } else {
                    load_song_file(&app, dropPath);
                }
            }
            UnloadDroppedFiles(dropped);
        }

        if (app.song != NULL && app.state == PLAYER_PLAYING && song_is_done(&app)) {
            app.state = PLAYER_STOPPED;
            set_status(&app, "Playback complete");
        }

        if (app.song != NULL || app.sound != NULL) {
            update_channel_waveforms(&app, dt);
        }

        int sw = GetScreenWidth();
        int sh = GetScreenHeight();

        Rectangle shell = {24, 22, (float)sw - 48, (float)sh - 44};
        Rectangle channelPanel = {shell.x + 24, shell.y + 94, shell.width - 48, shell.height * 0.53f};
        Rectangle transportPanel = {shell.x + 24, shell.y + shell.height - 160, shell.width - 48, 128};

        Button playBtn = {{transportPanel.x + 22, transportPanel.y + 66, 90, 42}, "Play"};
        Button pauseBtn = {{transportPanel.x + 124, transportPanel.y + 66, 90, 42}, "Pause"};
        Button stopBtn = {{transportPanel.x + 226, transportPanel.y + 66, 90, 42}, "Stop"};
        Rectangle loopBox = {transportPanel.x + 338, transportPanel.y + 67, 26, 26};
        Rectangle progressRect = {transportPanel.x + 20, transportPanel.y + 20, transportPanel.width - 40, 28};
        float statusTextX = loopBox.x + 128.0f;

        if (checkbox_clicked(loopBox)) {
            app.loopEnabled = !app.loopEnabled;
            apply_loop_setting(&app);
            set_status(&app, app.loopEnabled ? "Loop enabled" : "Loop disabled");
        }

        if (button_clicked(playBtn) && (app.song != NULL || app.sound != NULL)) {
            if (app.song != NULL) {
                if (app.state == PLAYER_PAUSED) {
                    BAESong_Resume(app.song);
                } else {
                    BAESong_Start(app.song, 0);
                }
            } else if (app.sound != NULL) {
                if (app.state == PLAYER_PAUSED) {
                    BAESound_Resume(app.sound);
                } else {
                    BAESound_Start(app.sound, 0, FLOAT_TO_UNSIGNED_FIXED(app.masterVolume), 0);
                }
            }
            app.state = PLAYER_PLAYING;
            set_status(&app, "Playing");
        }

        if (button_clicked(pauseBtn) && (app.song != NULL || app.sound != NULL)) {
            if (app.song != NULL) {
                BAESong_Pause(app.song);
            } else if (app.sound != NULL) {
                BAESound_Pause(app.sound);
            }
            app.state = PLAYER_PAUSED;
            set_status(&app, "Paused");
        }

        if (button_clicked(stopBtn) && (app.song != NULL || app.sound != NULL)) {
            if (app.song != NULL) {
                BAESong_Stop(app.song, FALSE);
            } else if (app.sound != NULL) {
                BAESound_Stop(app.sound, FALSE);
                BAESound_SetSamplePlaybackPosition(app.sound, 0);
            }
            app.state = PLAYER_STOPPED;
            set_status(&app, "Stopped");
            reset_channel_viz(&app);
        }

        Rectangle volTrack = {transportPanel.x + transportPanel.width - 240, transportPanel.y + 74, 180, 16};
        Rectangle volKnob = {volTrack.x + app.masterVolume * (volTrack.width - 18), volTrack.y - 6, 18, 28};

        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) &&
            CheckCollisionPointRec(GetMousePosition(),
            (Rectangle){volTrack.x - 6, volTrack.y - 12, volTrack.width + 12, 40})) {
            float t = (GetMouseX() - volTrack.x) / volTrack.width;
            app.masterVolume = clampf(t, 0.0f, 1.0f);
            apply_master_volume(&app);
        }

        uint32_t posUs = 0;
        uint32_t lenUs = 0;
        float progress = 0.0f;
        if (app.song != NULL) {
            BAESong_GetMicrosecondPosition(app.song, &posUs);
            BAESong_GetMicrosecondLength(app.song, &lenUs);
            if (lenUs > 0) {
                progress = (float)posUs / (float)lenUs;
            }
        } else if (app.sound != NULL) {
            uint32_t posFrames = 0;
            if (BAESound_GetSamplePlaybackPosition(app.sound, &posFrames) == BAE_NO_ERROR) {
                double hz = UNSIGNED_FIXED_TO_FLOAT(app.audioInfo.sampledRate);
                if (hz > 1.0) {
                    posUs = (uint32_t)(((double)posFrames * 1000000.0) / hz);
                    lenUs = (uint32_t)(((double)app.audioInfo.waveFrames * 1000000.0) / hz);
                }
                if (app.audioInfo.waveFrames > 0) {
                    progress = (float)posFrames / (float)app.audioInfo.waveFrames;
                }
            }
        }

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
            CheckCollisionPointRec(GetMousePosition(),
                                   (Rectangle){progressRect.x, progressRect.y - 6.0f, progressRect.width, progressRect.height + 12.0f})) {
            float t = (GetMouseX() - progressRect.x) / progressRect.width;
            t = clampf(t, 0.0f, 1.0f);

            if (app.song != NULL && lenUs > 0) {
                uint32_t targetUs = (uint32_t)(t * (float)lenUs);
                BAESong_SetMicrosecondPosition(app.song, targetUs);
                posUs = targetUs;
                progress = t;
            } else if (app.sound != NULL && app.audioInfo.waveFrames > 0) {
                uint32_t targetFrames = (uint32_t)(t * (float)app.audioInfo.waveFrames);
                BAESound_SetSamplePlaybackPosition(app.sound, targetFrames);
                progress = t;
            }
        }

        BeginDrawing();

        ClearBackground((Color){10, 14, 24, 255});

        DrawRectangleGradientV(0, 0, sw, sh, (Color){11, 18, 35, 255}, (Color){27, 17, 34, 255});
        DrawCircleGradient((Vector2){(float)(sw - 120), 90.0f}, 180.0f, (Color){48, 173, 255, 80}, (Color){48, 173, 255, 0});
        DrawCircleGradient((Vector2){70.0f, (float)(sh - 40)}, 240.0f, (Color){255, 132, 62, 65}, (Color){255, 132, 62, 0});

        DrawRectangleRounded(shell, 0.05f, 8, (Color){17, 22, 35, 232});
        DrawRectangleRoundedLines(shell, 0.05f, 8, (Color){70, 89, 124, 170});

        draw_app_text(&app, "NeoBAE Raylib Player", shell.x + 24.0f, shell.y + 20.0f, 34.0f, (Color){230, 237, 248, 255});
        char bankSubtitle[384];
        snprintf(bankSubtitle, sizeof(bankSubtitle), "Current Bank: %s", (app.bankName[0] != '\0') ? app.bankName : "(none)");
        float bankSubtitleX = shell.x + 26.0f;
        float bankSubtitleY = shell.y + 58.0f;
        int bankSubtitleW = measure_app_text(&app, bankSubtitle, 18.0f);
        Rectangle bankSubtitleRect = {bankSubtitleX, bankSubtitleY, (float)bankSubtitleW, 22.0f};

        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), bankSubtitleRect)) {
            load_builtin_bank(&app);
        }

        draw_app_text(&app,
                      bankSubtitle,
                      bankSubtitleX,
                      bankSubtitleY,
                      18.0f,
                      (Color){144, 178, 214, 255});

        draw_app_text(&app, app.songName, shell.x + shell.width - 380.0f, shell.y + 24.0f, 17.0f, (Color){199, 213, 233, 255});
        draw_app_text(&app,
                  state_label(app.state),
                  shell.x + shell.width - 380.0f,
                  shell.y + 48.0f,
                  20.0f,
                  color_blend((Color){132, 149, 177, 255}, (Color){255, 177, 100, 255}, 0.5f + 0.5f * sinf(app.uiPulse * 2.2f)));

        DrawRectangleRounded(channelPanel, 0.04f, 8, (Color){15, 20, 33, 215});
        DrawRectangleRoundedLines(channelPanel, 0.04f, 8, (Color){66, 84, 114, 155});

        float innerPad = 12.0f;
        float rowGap = 34.0f;
        float cellGap = 10.0f;
        float topPad = 26.0f;
        float rowHeight = (channelPanel.height - topPad - innerPad - rowGap) * 0.5f;
        float cellWidth = (channelPanel.width - innerPad * 2.0f - cellGap * (CHANNELS_PER_ROW - 1)) / CHANNELS_PER_ROW;
        Rectangle channelBoxes[CHANNEL_COUNT];

        for (int row = 0; row < 2; ++row) {
            float rowY = channelPanel.y + topPad + row * (rowHeight + rowGap);
            for (int col = 0; col < CHANNELS_PER_ROW; ++col) {
                int ch = row * CHANNELS_PER_ROW + col;
                channelBoxes[ch] = (Rectangle){
                    channelPanel.x + innerPad + col * (cellWidth + cellGap),
                    rowY,
                    cellWidth,
                    rowHeight
                };
            }
        }

        if (app.song != NULL && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            Vector2 mouse = GetMousePosition();
            for (int ch = 0; ch < CHANNEL_COUNT; ++ch) {
                if (CheckCollisionPointRec(mouse, channelBoxes[ch])) {
                    toggle_channel_mute(&app, ch);
                    break;
                }
            }
        }

        for (int row = 0; row < 2; ++row) {
            float rowY = channelPanel.y + topPad + row * (rowHeight + rowGap);
            if (row == 0) {
                draw_app_text(&app, "CH 1-8", channelPanel.x + innerPad, rowY - 18.0f, 14.0f, (Color){112, 202, 255, 240});
            } else {
                draw_app_text(&app, "CH 9-16", channelPanel.x + innerPad, rowY - 18.0f, 14.0f, (Color){255, 177, 98, 240});
            }

            for (int col = 0; col < CHANNELS_PER_ROW; ++col) {
                int ch = row * CHANNELS_PER_ROW + col;
                draw_channel_box(&app, ch, channelBoxes[ch]);
            }
        }

        DrawRectangleRounded(transportPanel, 0.08f, 8, (Color){14, 22, 38, 235});
        DrawRectangleRoundedLines(transportPanel, 0.08f, 8, (Color){72, 95, 132, 160});

        draw_progress_bar(progressRect, progress);

        char leftTime[32] = "00:00.000";
        char rightTime[32] = "-00:00.000";
        if (app.song != NULL) {
            int posMs = (int)(posUs / 1000);
            int lenMs = (int)(lenUs / 1000);
            int remMs = lenMs - posMs;
            if (remMs < 0) remMs = 0;
            snprintf(leftTime, sizeof(leftTime), "%02d:%02d.%03d", posMs / 60000, (posMs / 1000) % 60, posMs % 1000);
            snprintf(rightTime, sizeof(rightTime), "-%02d:%02d.%03d", remMs / 60000, (remMs / 1000) % 60, remMs % 1000);
        }

        Rectangle leftTimeRect = {
            progressRect.x,
            progressRect.y - 18.0f,
            (float)measure_app_text(&app, leftTime, 14.0f),
            18.0f
        };
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), leftTimeRect)) {
            if (app.song != NULL) {
                BAESong_SetMicrosecondPosition(app.song, 0);
                posUs = 0;
                progress = 0.0f;
            } else if (app.sound != NULL) {
                BAESound_SetSamplePlaybackPosition(app.sound, 0);
                progress = 0.0f;
            }
        }

        draw_app_text(&app, leftTime, progressRect.x, progressRect.y - 16.0f, 14.0f, (Color){173, 190, 218, 255});
        draw_app_text(&app,
                  rightTime,
                  progressRect.x + progressRect.width - (float)measure_app_text(&app, rightTime, 14.0f),
                  progressRect.y - 16.0f,
                  14.0f,
                  (Color){173, 190, 218, 255});

        draw_button(playBtn, (Color){39, 116, 183, 230}, (Color){62, 149, 223, 240});
        draw_button(pauseBtn, (Color){102, 82, 180, 225}, (Color){129, 104, 218, 240});
        draw_button(stopBtn, (Color){176, 72, 94, 225}, (Color){214, 95, 120, 240});
        draw_checkbox(&app, loopBox, app.loopEnabled, "Loop");

        draw_app_text(&app, "Volume", volTrack.x - 74.0f, volTrack.y - 2.0f, 18.0f, (Color){206, 218, 236, 255});
        DrawRectangleRounded(volTrack, 0.9f, 8, (Color){28, 39, 58, 220});
        DrawRectangleRounded((Rectangle){volTrack.x, volTrack.y, volTrack.width * app.masterVolume, volTrack.height}, 0.9f, 8, (Color){82, 204, 186, 240});
        DrawRectangleRounded(volKnob, 0.8f, 8, (Color){234, 239, 246, 245});

        draw_app_text(&app, app.statusText, statusTextX, transportPanel.y + 77.0f, 22.0f, (Color){218, 228, 242, 255});
        draw_app_text(&app,
                  "Tip: Drop a file anywhere in this window",
                  statusTextX,
                  transportPanel.y + 103.0f,
                  14.0f,
                  (Color){144, 171, 204, 255});

        EndDrawing();
    }

    unload_media(&app);

    if (app.uiFontLoaded) {
        UnloadFont(app.uiFont);
        app.uiFontLoaded = 0;
    }

    if (app.mixer != NULL) {
        BAEMixer_Delete(app.mixer);
        app.mixer = NULL;
    }

    CloseWindow();
    return 0;
}
