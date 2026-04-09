// BAE_API_Raylib.c - raylib audio backend for miniBAE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include <time.h>
#include <pthread.h>
#if defined(__has_include)
    #if __has_include(<raylib.h>)
        #include <raylib.h>
        #define BAE_HAVE_RAYLIB_HEADER 1
    #endif
#endif
#ifndef BAE_HAVE_RAYLIB_HEADER
typedef void (*AudioCallback)(void *bufferData, unsigned int frames);
typedef struct rAudioBuffer rAudioBuffer;
typedef struct rAudioProcessor rAudioProcessor;
typedef struct AudioStream {
    rAudioBuffer *buffer;
    rAudioProcessor *processor;
    unsigned int sampleRate;
    unsigned int sampleSize;
    unsigned int channels;
} AudioStream;
extern void InitAudioDevice(void);
extern void CloseAudioDevice(void);
extern bool IsAudioDeviceReady(void);
extern AudioStream LoadAudioStream(unsigned int sampleRate, unsigned int sampleSize, unsigned int channels);
extern void UnloadAudioStream(AudioStream stream);
extern void PlayAudioStream(AudioStream stream);
extern void StopAudioStream(AudioStream stream);
extern void SetAudioStreamBufferSizeDefault(int size);
extern void SetAudioStreamCallback(AudioStream stream, AudioCallback callback);
extern void SetAudioStreamVolume(AudioStream stream, float volume);
extern void SetAudioStreamPan(AudioStream stream, float pan);
#endif
#include "BAE_API.h"
#include <X_API.h>
#include <X_Assert.h>

#ifdef _WIN32
#include <io.h>

#ifndef __stdcall
    #define __stdcall
#endif

#if defined(_MSC_VER)
__declspec(dllimport) void __stdcall Sleep(unsigned long dwMilliseconds);
__declspec(dllimport) unsigned long long __stdcall GetTickCount64(void);
#else
extern __attribute__((dllimport)) void __stdcall Sleep(unsigned long dwMilliseconds);
extern __attribute__((dllimport)) unsigned long long __stdcall GetTickCount64(void);
#endif
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/time.h>
#endif

static AudioStream g_audioStream = {0};
static int g_initialized = 0;
static void *g_threadContext = NULL;
static uint32_t g_sampleRate = 44100;
static uint32_t g_channels = 2;
static uint32_t g_bits = 16;
static int32_t g_audioByteBufferSize = 0;
static uint32_t g_framesPerSlice = 0;
static uint64_t g_totalSamplesPlayed = 0;
static uint8_t *g_sliceStatic = NULL;
static size_t g_sliceStaticSize = 0;
static int16_t g_unscaled_volume = 256;
static int16_t g_balance = 0;
static int g_muted = 0;
static uint64_t g_startMicros = 0;
static pthread_mutex_t g_stateMutex = PTHREAD_MUTEX_INITIALIZER;

static FILE *g_pcm_rec_fp = NULL;
static uint64_t g_pcm_rec_data_bytes = 0;
static uint32_t g_pcm_rec_channels = 0;
static uint32_t g_pcm_rec_sample_rate = 0;
static uint32_t g_pcm_rec_bits = 0;

#if USE_FLAC_ENCODER == TRUE
typedef void (*FlacRecorderCallback)(int16_t *left, int16_t *right, int frames);
static FlacRecorderCallback g_flac_recorder_callback = NULL;
#endif

#if USE_VORBIS_ENCODER == TRUE
typedef void (*VorbisRecorderCallback)(int16_t *left, int16_t *right, int frames);
static VorbisRecorderCallback g_vorbis_recorder_callback = NULL;
#endif

#if USE_OPUS_ENCODER == TRUE
typedef void (*OpusRecorderCallback)(int16_t *left, int16_t *right, int frames);
static OpusRecorderCallback g_opus_recorder_callback = NULL;
#endif

static bool pcm_wav_write_header_local(FILE *file, uint32_t channels, uint32_t sample_rate, uint32_t bits, uint64_t data_bytes)
{
    uint32_t byte_rate;
    uint16_t block_align;
    uint32_t chunk_size;
    uint32_t subchunk1_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sr;
    uint16_t bits_per_sample;
    uint32_t data_size_32;

    if (!file)
    {
        return FALSE;
    }

    byte_rate = sample_rate * channels * (bits / 8);
    block_align = (uint16_t)(channels * (bits / 8));
    chunk_size = (uint32_t)(36 + data_bytes);
    subchunk1_size = 16;
    audio_format = 1;
    num_channels = (uint16_t)channels;
    sr = sample_rate;
    bits_per_sample = (uint16_t)bits;
    data_size_32 = (uint32_t)data_bytes;

    fwrite("RIFF", 1, 4, file);
    fwrite(&chunk_size, 4, 1, file);
    fwrite("WAVE", 1, 4, file);
    fwrite("fmt ", 1, 4, file);
    fwrite(&subchunk1_size, 4, 1, file);
    fwrite(&audio_format, 2, 1, file);
    fwrite(&num_channels, 2, 1, file);
    fwrite(&sr, 4, 1, file);
    fwrite(&byte_rate, 4, 1, file);
    fwrite(&block_align, 2, 1, file);
    fwrite(&bits_per_sample, 2, 1, file);
    fwrite("data", 1, 4, file);
    fwrite(&data_size_32, 4, 1, file);
    return TRUE;
}

static void PV_ComputeSliceSizeFromEngine(void)
{
    uint32_t frames;
    int16_t max_frames = BAE_GetMaxSamplePerSlice();

    if (max_frames <= 0)
    {
        max_frames = 512;
    }

    frames = (uint32_t)max_frames;
    if (frames < 64)
    {
        frames = 64;
    }

    g_framesPerSlice = frames;
    g_audioByteBufferSize = (int32_t)(frames * g_channels * (g_bits / 8));
    g_audioByteBufferSize = (g_audioByteBufferSize + 63) & ~63;

    if (g_sliceStaticSize < (size_t)g_audioByteBufferSize)
    {
        free(g_sliceStatic);
        g_sliceStatic = (uint8_t *)calloc(1, (size_t)g_audioByteBufferSize);
        if (g_sliceStatic)
        {
            g_sliceStaticSize = (size_t)g_audioByteBufferSize;
        }
    }
}

static uint64_t PV_MonotonicMicros(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount64() * 1000ULL;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000ULL) + ((uint64_t)ts.tv_nsec / 1000ULL);
#endif
}

static void PV_PrepareSplitBuffers(int16_t **left, int16_t **right, uint32_t *capacity, uint32_t frames)
{
    if (frames > *capacity)
    {
        int16_t *new_left = (int16_t *)realloc(*left, frames * sizeof(int16_t));
        int16_t *new_right = (int16_t *)realloc(*right, frames * sizeof(int16_t));
        if (new_left && new_right)
        {
            *left = new_left;
            *right = new_right;
            *capacity = frames;
        }
        else
        {
            free(new_left);
            free(new_right);
        }
    }
}

void raylib_audio_callback(void *bufferData, unsigned int frames)
{
    int sampleBytes;
    static int16_t *split_left = NULL;
    static int16_t *split_right = NULL;
    static uint32_t split_capacity = 0;
    static int32_t sliceValidBytes = 0;
    static int32_t sliceConsumedBytes = 0;

    if (!bufferData || frames == 0)
    {
        return;
    }

    sampleBytes = (int)((g_bits / 8) * g_channels);
    if (sampleBytes <= 0)
    {
        return;
    }

    int remaining = (int)(frames * (uint32_t)sampleBytes);
    uint8_t *out = (uint8_t *)bufferData;

    if (g_muted)
    {
        memset(bufferData, (g_bits == 8) ? 0x80 : 0, (size_t)remaining);
        return;
    }

    while (remaining > 0)
    {
        int32_t sliceBytes = g_audioByteBufferSize;
        if (sliceBytes <= 0 || !g_sliceStatic)
        {
            memset(out, (g_bits == 8) ? 0x80 : 0, (size_t)remaining);
            g_totalSamplesPlayed += (uint64_t)(remaining / sampleBytes);
            break;
        }

        /* If the previous slice has been fully consumed, generate a new one */
        if (sliceConsumedBytes >= sliceValidBytes)
        {
            int32_t sliceFrames = sliceBytes / sampleBytes;
            if (sliceFrames <= 0)
            {
                memset(out, 0, (size_t)remaining);
                break;
            }

            BAE_BuildMixerSlice(g_threadContext, g_sliceStatic, sliceBytes, sliceFrames);

            pthread_mutex_lock(&g_stateMutex);

            if (g_pcm_rec_fp)
            {
                size_t wrote = fwrite(g_sliceStatic, 1, (size_t)sliceBytes, g_pcm_rec_fp);
                if (wrote == (size_t)sliceBytes)
                {
                    g_pcm_rec_data_bytes += (uint64_t)wrote;
                }
            }

            if (g_bits == 16)
            {
                int16_t *samples = (int16_t *)g_sliceStatic;

                if (g_channels == 1)
                {
#if USE_FLAC_ENCODER == TRUE
                    if (g_flac_recorder_callback)
                    {
                        g_flac_recorder_callback(samples, samples, (int)sliceFrames);
                    }
#endif
#if USE_VORBIS_ENCODER == TRUE
                    if (g_vorbis_recorder_callback)
                    {
                        g_vorbis_recorder_callback(samples, samples, (int)sliceFrames);
                    }
#endif
#if USE_OPUS_ENCODER == TRUE
                    if (g_opus_recorder_callback)
                    {
                        g_opus_recorder_callback(samples, samples, (int)sliceFrames);
                    }
#endif
                }
                else if (g_channels == 2)
                {
                    PV_PrepareSplitBuffers(&split_left, &split_right, &split_capacity, (uint32_t)sliceFrames);
                    if (split_left && split_right)
                    {
                        int32_t frameIndex;
                        for (frameIndex = 0; frameIndex < sliceFrames; ++frameIndex)
                        {
                            split_left[frameIndex] = samples[frameIndex * 2];
                            split_right[frameIndex] = samples[frameIndex * 2 + 1];
                        }
#if USE_FLAC_ENCODER == TRUE
                        if (g_flac_recorder_callback)
                        {
                            g_flac_recorder_callback(split_left, split_right, (int)sliceFrames);
                        }
#endif
#if USE_VORBIS_ENCODER == TRUE
                        if (g_vorbis_recorder_callback)
                        {
                            g_vorbis_recorder_callback(split_left, split_right, (int)sliceFrames);
                        }
#endif
#if USE_OPUS_ENCODER == TRUE
                        if (g_opus_recorder_callback)
                        {
                            g_opus_recorder_callback(split_left, split_right, (int)sliceFrames);
                        }
#endif
                    }
                }
            }

            pthread_mutex_unlock(&g_stateMutex);

            sliceValidBytes = sliceBytes;
            sliceConsumedBytes = 0;
        }

        /* Copy from the current (possibly partially consumed) slice */
        int32_t available = sliceValidBytes - sliceConsumedBytes;
        int32_t toCopy = (available < remaining) ? available : remaining;

        memcpy(out, g_sliceStatic + sliceConsumedBytes, (size_t)toCopy);
        sliceConsumedBytes += toCopy;
        out += toCopy;
        remaining -= toCopy;
        g_totalSamplesPlayed += (uint64_t)(toCopy / sampleBytes);
    }
}

int BAE_Setup(void)
{
    return 0;
}

int BAE_Cleanup(void)
{
    if (g_audioStream.buffer)
    {
        StopAudioStream(g_audioStream);
        UnloadAudioStream(g_audioStream);
        memset(&g_audioStream, 0, sizeof(g_audioStream));
    }
    if (g_initialized)
    {
        CloseAudioDevice();
        g_initialized = 0;
    }
    free(g_sliceStatic);
    g_sliceStatic = NULL;
    g_sliceStaticSize = 0;
    return 0;
}

static uint32_t g_mem_used = 0;
static uint32_t g_mem_used_max = 0;

void *BAE_Allocate(uint32_t size)
{
    void *ptr = size ? calloc(1, size) : NULL;
    if (ptr)
    {
        g_mem_used += size;
        if (g_mem_used > g_mem_used_max)
        {
            g_mem_used_max = g_mem_used;
        }
    }
    return ptr;
}

void BAE_Deallocate(void *memoryBlock)
{
    if (memoryBlock)
    {
        free(memoryBlock);
    }
}

void BAE_AllocDebug(int debug)
{
    (void)debug;
}

uint32_t BAE_GetSizeOfMemoryUsed(void)
{
    return g_mem_used;
}

uint32_t BAE_GetMaxSizeOfMemoryUsed(void)
{
    return g_mem_used_max;
}

int BAE_IsBadReadPointer(void *memoryBlock, uint32_t size)
{
    (void)memoryBlock;
    (void)size;
    return 2;
}

uint32_t BAE_SizeOfPointer(void *memoryBlock)
{
    (void)memoryBlock;
    return 0;
}

void BAE_BlockMove(void *source, void *dest, uint32_t size)
{
    if (source && dest && size)
    {
        memmove(dest, source, size);
    }
}

int BAE_IsStereoSupported(void)
{
    return 1;
}

int BAE_Is8BitSupported(void)
{
    return 1;
}

int BAE_Is16BitSupported(void)
{
    return 1;
}

int16_t BAE_GetHardwareVolume(void)
{
    return g_unscaled_volume;
}

void BAE_SetHardwareVolume(int16_t volume)
{
    if (volume < 0)
    {
        volume = 0;
    }
    if (volume > 256)
    {
        volume = 256;
    }
    g_unscaled_volume = volume;
    if (g_audioStream.buffer)
    {
        SetAudioStreamVolume(g_audioStream, (float)g_unscaled_volume / 256.0f);
    }
}

int16_t BAE_GetHardwareBalance(void)
{
    return g_balance;
}

void BAE_SetHardwareBalance(int16_t balance)
{
    if (balance < -256)
    {
        balance = -256;
    }
    if (balance > 256)
    {
        balance = 256;
    }
    g_balance = balance;
    if (g_audioStream.buffer)
    {
        SetAudioStreamPan(g_audioStream, (float)g_balance / 256.0f);
    }
}

uint32_t BAE_Microseconds(void)
{
    uint64_t now = PV_MonotonicMicros();
    if (g_startMicros == 0)
    {
        g_startMicros = now;
    }
    return (uint32_t)(now - g_startMicros);
}

uint32_t BAE_GetMicroseconds(void)
{
    return BAE_Microseconds();
}

void BAE_WaitMicroseconds(uint32_t wait)
{
#ifdef _WIN32
    Sleep((wait + 999) / 1000);
#else
    usleep(wait);
#endif
}

#define MAX_OPEN_FILES 64
static FILE *g_file_table[MAX_OPEN_FILES] = {0};

static int PV_AllocateFileHandle(FILE *file)
{
    int index;
    if (!file)
    {
        return -1;
    }
    for (index = 1; index < MAX_OPEN_FILES; ++index)
    {
        if (!g_file_table[index])
        {
            g_file_table[index] = file;
            return index;
        }
    }
    fclose(file);
    return -1;
}

static FILE *PV_GetFileFromHandle(intptr_t handle)
{
    if (handle > 0 && handle < MAX_OPEN_FILES)
    {
        return g_file_table[handle];
    }
    return NULL;
}

static void PV_FreeFileHandle(intptr_t handle)
{
    if (handle > 0 && handle < MAX_OPEN_FILES)
    {
        g_file_table[handle] = NULL;
    }
}

void BAE_CopyFileNameNative(void *source, void *dest)
{
    if (source && dest)
    {
        strcpy((char *)dest, (char *)source);
    }
}

int32_t BAE_FileCreate(void *fileName)
{
    FILE *file = fopen((char *)fileName, "wb");
    if (!file)
    {
        return -1;
    }
    fclose(file);
    return 0;
}

int32_t BAE_FileDelete(void *fileName)
{
    return remove((char *)fileName) == 0 ? 0 : -1;
}

intptr_t BAE_FileOpenForRead(void *fileName)
{
    FILE *file = fopen((char *)fileName, "rb");
    return file ? PV_AllocateFileHandle(file) : -1;
}

intptr_t BAE_FileOpenForWrite(void *fileName)
{
    FILE *file = fopen((char *)fileName, "wb");
    return file ? PV_AllocateFileHandle(file) : -1;
}

intptr_t BAE_FileOpenForReadWrite(void *fileName)
{
    FILE *file = fopen((char *)fileName, "rb+");
    if (!file)
    {
        file = fopen((char *)fileName, "wb+");
    }
    return file ? PV_AllocateFileHandle(file) : -1;
}

void BAE_FileClose(intptr_t ref)
{
    FILE *file = PV_GetFileFromHandle(ref);
    if (file)
    {
        fclose(file);
        PV_FreeFileHandle(ref);
    }
}

int32_t BAE_ReadFile(intptr_t ref, void *buffer, int32_t length)
{
    FILE *file = PV_GetFileFromHandle(ref);
    size_t bytes_read;
    if (!file)
    {
        return -1;
    }
    bytes_read = fread(buffer, 1, (size_t)length, file);
    return (bytes_read == 0 && ferror(file)) ? -1 : (int32_t)bytes_read;
}

int32_t BAE_WriteFile(intptr_t ref, void *buffer, int32_t length)
{
    FILE *file = PV_GetFileFromHandle(ref);
    size_t bytes_written;
    if (!file)
    {
        return -1;
    }
    bytes_written = fwrite(buffer, 1, (size_t)length, file);
    fflush(file);
    return (bytes_written == 0 && ferror(file)) ? -1 : (int32_t)bytes_written;
}

int32_t BAE_SetFilePosition(intptr_t ref, uint32_t pos)
{
    FILE *file = PV_GetFileFromHandle(ref);
    if (!file)
    {
        return -1;
    }
    return fseek(file, (long)pos, SEEK_SET) == 0 ? 0 : -1;
}

uint32_t BAE_GetFilePosition(intptr_t ref)
{
    FILE *file = PV_GetFileFromHandle(ref);
    long pos;
    if (!file)
    {
        return 0;
    }
    pos = ftell(file);
    if (pos < 0)
    {
        return 0;
    }
    if (pos > UINT32_MAX)
    {
        return UINT32_MAX;
    }
    return (uint32_t)pos;
}

uint32_t BAE_GetFileLength(intptr_t ref)
{
    FILE *file = PV_GetFileFromHandle(ref);
    long current;
    long end;
    if (!file)
    {
        return 0;
    }
    current = ftell(file);
    if (current < 0)
    {
        return 0;
    }
    fseek(file, 0, SEEK_END);
    end = ftell(file);
    fseek(file, current, SEEK_SET);
    if (end < 0)
    {
        return 0;
    }
    if (end > UINT32_MAX)
    {
        return UINT32_MAX;
    }
    return (uint32_t)end;
}

int BAE_SetFileLength(intptr_t ref, uint32_t newSize)
{
    FILE *file = PV_GetFileFromHandle(ref);
    if (!file)
    {
        return -1;
    }
#if defined(_WIN32)
    {
        int fd = _fileno(file);
        if (fd < 0)
        {
            return -1;
        }
        return (_chsize(fd, (long)newSize) == 0) ? 0 : -1;
    }
#else
    {
        int fd = fileno(file);
        if (fd < 0)
        {
            return -1;
        }
        return (ftruncate(fd, (off_t)newSize) == 0) ? 0 : -1;
    }
#endif
}

int BAE_GetAudioBufferCount(void)
{
    return 1;
}

int32_t BAE_GetAudioByteBufferSize(void)
{
    return g_audioByteBufferSize;
}

int BAE_AcquireAudioCard(void *threadContext, uint32_t sampleRate, uint32_t channels, uint32_t bits)
{
    if (g_audioStream.buffer)
    {
        return 0;
    }

    if (!g_initialized)
    {
        InitAudioDevice();
        if (!IsAudioDeviceReady())
        {
            BAE_PRINTF("raylib audio init failed\n");
            return -1;
        }
        g_initialized = 1;
    }

    g_threadContext = threadContext;
    g_sampleRate = sampleRate;
    g_channels = channels;
    g_bits = bits;

    PV_ComputeSliceSizeFromEngine();
    SetAudioStreamBufferSizeDefault((int)g_framesPerSlice);
    g_audioStream = LoadAudioStream(g_sampleRate, g_bits, g_channels);
    if (!g_audioStream.buffer)
    {
        BAE_PRINTF("LoadAudioStream failed for raylib backend\n");
        return -1;
    }

    SetAudioStreamVolume(g_audioStream, (float)g_unscaled_volume / 256.0f);
    SetAudioStreamPan(g_audioStream, (float)g_balance / 256.0f);
    //SetAudioStreamCallback(g_audioStream, raylib_audio_callback);
    PlayAudioStream(g_audioStream);
    return 0;
}

int BAE_ReleaseAudioCard(void *threadContext)
{
    (void)threadContext;
    if (g_audioStream.buffer)
    {
        StopAudioStream(g_audioStream);
        UnloadAudioStream(g_audioStream);
        memset(&g_audioStream, 0, sizeof(g_audioStream));
    }
    return 0;
}

int BAE_Mute(void)
{
    g_muted = 1;
    return 0;
}

int BAE_Unmute(void)
{
    g_muted = 0;
    return 0;
}

int BAE_IsMuted(void)
{
    return g_muted;
}

void BAE_ProcessRouteBus(int currentRoute, int32_t *pChannels, int count)
{
    (void)currentRoute;
    (void)pChannels;
    (void)count;
}

void BAE_Idle(void *userContext)
{
    (void)userContext;
    BAE_WaitMicroseconds(1000);
}

void BAE_UnlockAudioFrameThread(void) {}
void BAE_LockAudioFrameThread(void) {}
void BAE_BlockAudioFrameThread(void)
{
    BAE_WaitMicroseconds(1000);
}

uint32_t BAE_GetDeviceSamplesPlayedPosition(void)
{
    return (uint32_t)g_totalSamplesPlayed;
}

int32_t BAE_MaxDevices(void)
{
    return 1;
}

void BAE_SetDeviceID(int32_t deviceID, void *deviceParameter)
{
    (void)deviceID;
    (void)deviceParameter;
}

int32_t BAE_GetDeviceID(void *deviceParameter)
{
    (void)deviceParameter;
    return 0;
}

void BAE_GetDeviceName(int32_t deviceID, char *cName, uint32_t cNameLength)
{
    (void)deviceID;
    if (cName && cNameLength)
    {
        snprintf(cName, cNameLength, "raylib,stream,callback");
    }
}

int BAE_Platform_PCMRecorder_Start(const char *path, uint32_t channels, uint32_t sample_rate, uint32_t bits)
{
    FILE *file;
    if (!path || g_pcm_rec_fp)
    {
        return -1;
    }

    file = fopen(path, "wb+");
    if (!file)
    {
        return -1;
    }

    pthread_mutex_lock(&g_stateMutex);
    g_pcm_rec_fp = file;
    g_pcm_rec_channels = channels;
    g_pcm_rec_sample_rate = sample_rate;
    g_pcm_rec_bits = bits;
    g_pcm_rec_data_bytes = 0;
    pcm_wav_write_header_local(g_pcm_rec_fp, channels, sample_rate, bits, 0);
    fflush(g_pcm_rec_fp);
    pthread_mutex_unlock(&g_stateMutex);
    return 0;
}

void BAE_Platform_PCMRecorder_Stop(void)
{
    pthread_mutex_lock(&g_stateMutex);
    if (g_pcm_rec_fp)
    {
        fseek(g_pcm_rec_fp, 0, SEEK_SET);
        pcm_wav_write_header_local(g_pcm_rec_fp, g_pcm_rec_channels, g_pcm_rec_sample_rate, g_pcm_rec_bits, g_pcm_rec_data_bytes);
        fflush(g_pcm_rec_fp);
        fclose(g_pcm_rec_fp);
        g_pcm_rec_fp = NULL;
        g_pcm_rec_data_bytes = 0;
        g_pcm_rec_channels = 0;
        g_pcm_rec_sample_rate = 0;
        g_pcm_rec_bits = 0;
    }
    pthread_mutex_unlock(&g_stateMutex);
}

#if USE_MPEG_ENCODER == TRUE
int BAE_Platform_MP3Recorder_Start(const char *path, uint32_t channels, uint32_t sample_rate, uint32_t bits, uint32_t bitrate)
{
    (void)path;
    (void)channels;
    (void)sample_rate;
    (void)bits;
    (void)bitrate;
    return -1;
}

void BAE_Platform_MP3Recorder_Stop(void) {}
#endif

#if USE_FLAC_ENCODER == TRUE
void BAE_Platform_SetFlacRecorderCallback(void (*callback)(int16_t *left, int16_t *right, int frames))
{
    pthread_mutex_lock(&g_stateMutex);
    g_flac_recorder_callback = (FlacRecorderCallback)callback;
    pthread_mutex_unlock(&g_stateMutex);
}

void BAE_Platform_ClearFlacRecorderCallback(void)
{
    pthread_mutex_lock(&g_stateMutex);
    g_flac_recorder_callback = NULL;
    pthread_mutex_unlock(&g_stateMutex);
}
#endif

#if USE_VORBIS_ENCODER == TRUE
void BAE_Platform_SetVorbisRecorderCallback(void (*callback)(int16_t *left, int16_t *right, int frames))
{
    pthread_mutex_lock(&g_stateMutex);
    g_vorbis_recorder_callback = (VorbisRecorderCallback)callback;
    pthread_mutex_unlock(&g_stateMutex);
}

void BAE_Platform_ClearVorbisRecorderCallback(void)
{
    pthread_mutex_lock(&g_stateMutex);
    g_vorbis_recorder_callback = NULL;
    pthread_mutex_unlock(&g_stateMutex);
}
#endif

#if USE_OPUS_ENCODER == TRUE
void BAE_Platform_SetOpusRecorderCallback(void (*callback)(int16_t *left, int16_t *right, int frames))
{
    pthread_mutex_lock(&g_stateMutex);
    g_opus_recorder_callback = (OpusRecorderCallback)callback;
    pthread_mutex_unlock(&g_stateMutex);
}

void BAE_Platform_ClearOpusRecorderCallback(void)
{
    pthread_mutex_lock(&g_stateMutex);
    g_opus_recorder_callback = NULL;
    pthread_mutex_unlock(&g_stateMutex);
}
#endif

typedef struct
{
    pthread_mutex_t mutex;
} sPthreadMutex;

int BAE_NewMutex(BAE_Mutex *lock, char *name, char *file, int lineno)
{
    sPthreadMutex *mutex;
    pthread_mutexattr_t attr;
    (void)name;
    (void)file;
    (void)lineno;
    mutex = (sPthreadMutex *)BAE_Allocate(sizeof(sPthreadMutex));
    if (!mutex)
    {
        return 0;
    }
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&mutex->mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    *lock = (BAE_Mutex)mutex;
    return 1;
}

void BAE_AcquireMutex(BAE_Mutex lock)
{
    if (lock)
    {
        sPthreadMutex *mutex = (sPthreadMutex *)lock;
        pthread_mutex_lock(&mutex->mutex);
    }
}

void BAE_ReleaseMutex(BAE_Mutex lock)
{
    if (lock)
    {
        sPthreadMutex *mutex = (sPthreadMutex *)lock;
        pthread_mutex_unlock(&mutex->mutex);
    }
}

void BAE_DestroyMutex(BAE_Mutex lock)
{
    if (lock)
    {
        sPthreadMutex *mutex = (sPthreadMutex *)lock;
        pthread_mutex_destroy(&mutex->mutex);
        BAE_Deallocate(mutex);
    }
}

int BAE_CreateFrameThread(void *threadContext, BAE_FrameThreadProc proc)
{
    (void)threadContext;
    (void)proc;
    return 0;
}

int BAE_SetFrameThreadPriority(void *threadContext, int priority)
{
    (void)threadContext;
    (void)priority;
    return 0;
}

int BAE_DestroyFrameThread(void *threadContext)
{
    (void)threadContext;
    return 0;
}

int BAE_SleepFrameThread(void *threadContext, int32_t msec)
{
    (void)threadContext;
    BAE_WaitMicroseconds((uint32_t)msec * 1000U);
    return 0;
}

int BAE_AcquireAudioCapture(void *threadContext, uint32_t sampleRate, uint32_t channels, uint32_t bits, uint32_t *pCaptureHandle)
{
    (void)threadContext;
    (void)sampleRate;
    (void)channels;
    (void)bits;
    (void)pCaptureHandle;
    return -1;
}

int BAE_ReleaseAudioCapture(void *threadContext)
{
    (void)threadContext;
    return -1;
}

int BAE_StartAudioCapture(BAE_CaptureDone done, void *callbackContext)
{
    (void)done;
    (void)callbackContext;
    return -1;
}

int BAE_StopAudioCapture(void) { return -1; }
int BAE_PauseAudioCapture(void) { return -1; }
int BAE_ResumeAudioCapture(void) { return -1; }
int32_t BAE_MaxCaptureDevices(void) { return 0; }
void BAE_SetCaptureDeviceID(int32_t deviceID, void *deviceParameter)
{
    (void)deviceID;
    (void)deviceParameter;
}
int32_t BAE_GetCaptureDeviceID(void *deviceParameter)
{
    (void)deviceParameter;
    return -1;
}
void BAE_GetCaptureDeviceName(int32_t deviceID, char *cName, uint32_t cNameLength)
{
    (void)deviceID;
    if (cName && cNameLength)
    {
        *cName = '\0';
    }
}
uint32_t BAE_GetCaptureBufferSizeInFrames(void) { return 0; }
int BAE_GetCaptureBufferCount(void) { return 0; }
uint32_t BAE_GetDeviceSamplesCapturedPosition(void) { return 0; }

void BAE_DisplayMemoryUsage(int detailLevel)
{
    (void)detailLevel;
}

void BAE_PrintHexDump(void *address, int32_t length)
{
    int32_t index;
    unsigned char *bytes = (unsigned char *)address;
    for (index = 0; index < length; ++index)
    {
        if ((index % 16) == 0)
        {
            BAE_PRINTF("\n%08x: ", (uint32_t)index);
        }
        BAE_PRINTF("%02X ", bytes[index]);
    }
    BAE_PRINTF("\n");
}

AudioStream BAE_GetAudioStream(void)
{
    return g_audioStream;
}