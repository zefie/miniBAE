/*******************************************************************************************
*
*   raylib [audio] example - stream callback
*
*   Example complexity rating: [★★★☆] 3/4
*
*   Example originally created with raylib 6.0, last time updated with raylib 6.0
*
*   Example created by Dan Hoang (@dan-hoang) and reviewed by Ramon Santamaria (@raysan5)
*
*   NOTE: Example sends a wave to the audio device,
*     user gets the choice of four waves: sine, square, triangle, and sawtooth
*     A stream is set up to play to the audio device; stream is hooked to a callback that
*     generates a wave, that is determined by user choice
*
*   Example licensed under an unmodified zlib/libpng license, which is an OSI-certified,
*   BSD-like license that allows static linking with closed source software
*
*   Copyright (c) 2026 Dan Hoang (@dan-hoang)
*
********************************************************************************************/

#include "raylib.h"
#include "NeoBAE.h"
#include "BAE_API.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "nocturnal.h"
#include "world1.h"

#define BUFFER_SIZE 4096
#define SAMPLE_RATE 44100

// Wave type
typedef enum {
    SINE,
    SQUARE,
    TRIANGLE,
    SAWTOOTH
} WaveType;

typedef enum {
    BAE_STATE_UNLOADED = 0,
    BAE_STATE_PLAYING = 1,
    BAE_STATE_STOPPED = 2,
    BAE_STATE_PAUSED = 3
} BaeState;

static int songIndex = 0;
// Buffer to keep the last second of uploaded audio,
// part of which will be drawn on the screen
static float buffer[SAMPLE_RATE] = { 0 };
static int scopeWritePos = 0;
static float scopeLastValue = 0.0f;
static char *songsAsString[] = { "Nocturnal", "World1" };
static int currentSong = -1;
static int currentState = BAE_STATE_UNLOADED;
static BAESong gSong = NULL;
static BAEMixer gMixer = NULL;
static BAEBankToken gBankToken = 0;

// Exposed by the raylib platform backend.
extern AudioStream BAE_GetAudioStream(void);

void raylib_audio_callback_(void *bufferData, unsigned int frames) {
    raylib_audio_callback(bufferData, frames);

    // BAE stream is opened as 16-bit stereo. Convert to mono for scope display.
    const int16_t *samples = (const int16_t *)bufferData;
    for (unsigned int i = 0; i < frames; ++i)
    {
        float left = (float)samples[i * 2] / 32768.0f;
        float right = (float)samples[i * 2 + 1] / 32768.0f;
        float mono = 0.5f * (left + right);

        // Low-pass filter to keep the scope trace visually smooth.
        scopeLastValue += (mono - scopeLastValue) * 0.18f;
        buffer[scopeWritePos] = scopeLastValue;
        scopeWritePos = (scopeWritePos + 1) % SAMPLE_RATE;
    }
}

//------------------------------------------------------------------------------------
// Program main entry point
//------------------------------------------------------------------------------------
int main(void)
{
    // Initialization
    //--------------------------------------------------------------------------------------
    const int screenWidth = 800;
    const int screenHeight = 450;

    InitWindow(screenWidth, screenHeight, "raylib [audio] example - stream callback");

    gMixer = BAEMixer_New();

    // Open the mixer: 44.1kHz, 16-bit stereo
    // engageAudio=TRUE lets BAE_API_Raylib handle the audio stream and callback
    BAEResult openErr = BAEMixer_Open(gMixer,
                                      BAE_RATE_44K,
                                      BAE_2_POINT_INTERPOLATION,
                                      BAE_USE_16 | BAE_USE_STEREO,
                                      24,     // max song voices
                                      4,      // max sound voices
                                      24,     // mix level (24 = unity gain)
                                      TRUE);  // engage hardware audio via BAE_API_Raylib
    if (openErr != BAE_NO_ERROR) {
        printf("BAEMixer_Open failed: %d\n", (int)openErr);
        return 1;        
    }
    BAEMixer_LoadBuiltinBank(gMixer, gBankToken);
    BAEMixer_SetMasterVolume(gMixer, FLOAT_TO_UNSIGNED_FIXED(1.0f)); 
    BAEMixer_SetGlobalVolume(gMixer, FLOAT_TO_UNSIGNED_FIXED(1.0f));

    SetAudioStreamCallback(BAE_GetAudioStream(), raylib_audio_callback_);

    SetTargetFPS(30);
    //--------------------------------------------------------------------------------------

    // Main game loop
    while (!WindowShouldClose())    // Detect window close button or ESC key
    {
        // Update        
        //----------------------------------------------------------------------------------

        // Load/switch song in the main thread (not the audio callback)
        if (currentSong != songIndex)
        {
            currentSong = songIndex;
            if (gSong != NULL)
            {
                BAESong_Stop(gSong, FALSE);
                BAESong_Delete(gSong);
                gSong = NULL;
            }
            gSong = BAESong_New(gMixer);
            BAEResult res = BAE_NO_ERROR;
            if (currentSong == 0)
            {
                printf("Loading Nocturnal...\n");
                res = BAESong_LoadRmfFromMemory(gSong, embedded_song_data_nocturnal, sizeof(embedded_song_data_nocturnal), 0, TRUE);
            }
            else if (currentSong == 1)
            {
                printf("Loading World1...\n");
                res = BAESong_LoadRmfFromMemory(gSong, embedded_song_data_world1, sizeof(embedded_song_data_world1), 0, TRUE);
            }
            if (res == BAE_NO_ERROR && gSong)
            {
                printf("Loaded OK, starting...\n");
                BAESong_Preroll(gSong);
                BAESong_Start(gSong, 0);
                BAESong_SetLoops(gSong, -1);
                currentState = BAE_STATE_PLAYING;
            }
            else
            {
                printf("Load failed: %d\n", (int)res);
            }
        }

        if (IsKeyPressed(KEY_UP))
        {
            if (currentState == BAE_STATE_PLAYING)
            {
                BAESong_Stop(gSong, FALSE);
                currentState = BAE_STATE_STOPPED;
            }
            else if (currentState == BAE_STATE_STOPPED)
            {
                BAESong_Start(gSong, 0);
                currentState = BAE_STATE_PLAYING;
            }
        }
        if (IsKeyPressed(KEY_LEFT))
        {
            songIndex--;
            if (songIndex < 0) songIndex = sizeof(songsAsString)/sizeof(songsAsString[0]) - 1;
        }

        if (IsKeyPressed(KEY_RIGHT))
        {
            songIndex++;
            if (songIndex >= sizeof(songsAsString)/sizeof(songsAsString[0])) songIndex = 0;
        }
        //----------------------------------------------------------------------------------

        // Draw
        //----------------------------------------------------------------------------------
        BeginDrawing();

            ClearBackground(BLACK);
            DrawText(TextFormat("song: %s", songsAsString[songIndex]), screenWidth - 220, 10, 20, PURPLE);
            DrawText("Left/right to change song", 10, 10, 20, DARKGRAY);

            // Draw the last 10 ms of uploaded audio
            int scopeFrames = SAMPLE_RATE / 100;
            int scopeStart = scopeWritePos - scopeFrames;
            if (scopeStart < 0) scopeStart += SAMPLE_RATE;

            for (int i = 0; i < screenWidth; i++)
            {
                int idxA = (scopeStart + (i * scopeFrames) / screenWidth) % SAMPLE_RATE;
                int idxB = (scopeStart + ((i + 1) * scopeFrames) / screenWidth) % SAMPLE_RATE;
                Vector2 startPos = { (float)i, 250.0f - 60.0f * buffer[idxA] };
                Vector2 endPos = { (float)(i + 1), 250.0f - 60.0f * buffer[idxB] };
                DrawLineV(startPos, endPos, PURPLE);
            }

        EndDrawing();
        //----------------------------------------------------------------------------------
    }

    // De-Initialization
    //--------------------------------------------------------------------------------------
    if (gSong) {
        BAESong_Stop(gSong, FALSE);
        BAESong_Delete(gSong);
    }
    if (gMixer) {
        BAEMixer_Delete(gMixer);
    }
    CloseAudioDevice();         // Close audio device (music streaming is automatically stopped)

    CloseWindow();              // Close window and OpenGL context
    //--------------------------------------------------------------------------------------

    return 0;
}

