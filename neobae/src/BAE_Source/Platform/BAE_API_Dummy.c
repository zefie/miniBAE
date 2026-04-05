/*****************************************************************************/
/*
**  BAE_API_Dummy.c
**
**  Minimal no-audio platform implementation of BAE_API.h.
**  Uses only standard C (stdio/stdlib/string/time). No audio output,
**  no capture, no threading library required. Mutexes are no-op stubs.
**
**  Intended for headless/test builds where audio output is not needed.
*/
/*****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#include <X_API.h>
#include <X_Assert.h>
#include "BAE_API.h"

static uint32_t  g_memory_buoy     = 0;
static uint32_t  g_memory_buoy_max = 0;
static int16_t   g_balance         = 0;
static int16_t   g_unscaled_volume = 256;
static int32_t   g_audioByteBufferSize = 2048;

/* ---- System ------------------------------------------------------------ */

int BAE_Setup(void)   { return 0; }
int BAE_Cleanup(void) { return 0; }

/* ---- Memory ------------------------------------------------------------ */

void *BAE_Allocate(uint32_t size)
{
    void *data = NULL;
    if (size)
    {
        data = malloc(size);
        if (data)
        {
            memset(data, 0, size);
            g_memory_buoy += size;
            if (g_memory_buoy > g_memory_buoy_max)
                g_memory_buoy_max = g_memory_buoy;
        }
    }
    return data;
}

void BAE_Deallocate(void *memoryBlock)
{
    if (memoryBlock)
        free(memoryBlock);
}

void     BAE_AllocDebug(int debug)              { (void)debug; }
uint32_t BAE_GetSizeOfMemoryUsed(void)          { return g_memory_buoy; }
uint32_t BAE_GetMaxSizeOfMemoryUsed(void)       { return g_memory_buoy_max; }
int      BAE_IsBadReadPointer(void *m, uint32_t s) { (void)m; (void)s; return 2; }
uint32_t BAE_SizeOfPointer(void *m)             { (void)m; return 0; }

void BAE_BlockMove(void *source, void *dest, uint32_t size)
{
    if (source && dest && size)
        memmove(dest, source, size);
}

void BAE_DisplayMemoryUsage(int detailLevel)    { (void)detailLevel; }
void BAE_PrintHexDump(void *address, int32_t length) { (void)address; (void)length; }

/* ---- Audio card modifiers ---------------------------------------------- */

int     BAE_IsStereoSupported(void)             { return 1; }
int     BAE_Is8BitSupported(void)               { return 1; }
int     BAE_Is16BitSupported(void)              { return 1; }
int16_t BAE_GetHardwareVolume(void)             { return g_unscaled_volume; }
int16_t BAE_GetHardwareBalance(void)            { return g_balance; }

void BAE_SetHardwareVolume(int16_t v)
{
    if (v > 256) v = 256;
    if (v < 0)   v = 0;
    g_unscaled_volume = v;
}

void BAE_SetHardwareBalance(int16_t b)
{
    if (b > 256)  b = 256;
    if (b < -256) b = -256;
    g_balance = b;
}

/* ---- Timing ------------------------------------------------------------ */

uint32_t BAE_Microseconds(void)
{
    return (uint32_t)((clock() * 1000000UL) / (unsigned long)CLOCKS_PER_SEC);
}

void BAE_WaitMicroseconds(uint32_t waitAmount) { (void)waitAmount; }

/* ---- File support ------------------------------------------------------ */

void BAE_CopyFileNameNative(void *fileNameSource, void *fileNameDest)
{
    char *src = (char *)fileNameSource;
    char *dst = (char *)fileNameDest;
    if (src && dst)
    {
        while (*src)
            *dst++ = *src++;
        *dst = 0;
    }
}

int32_t BAE_FileCreate(void *fileName)
{
    FILE *fp = fopen((char *)fileName, "wb");
    if (!fp) return -1;
    fclose(fp);
    return 0;
}

int32_t BAE_FileDelete(void *fileName)
{
    return (remove((char *)fileName) == 0) ? 0 : -1;
}

intptr_t BAE_FileOpenForRead(void *fileName)
{
    if (!fileName) return -1;
    return (intptr_t)fopen((char *)fileName, "rb");
}

intptr_t BAE_FileOpenForWrite(void *fileName)
{
    if (!fileName) return -1;
    return (intptr_t)fopen((char *)fileName, "wb");
}

intptr_t BAE_FileOpenForReadWrite(void *fileName)
{
    if (!fileName) return -1;
    return (intptr_t)fopen((char *)fileName, "r+b");
}

void BAE_FileClose(intptr_t ref)
{
    if (ref) fclose((FILE *)ref);
}

int32_t BAE_ReadFile(intptr_t ref, void *pBuffer, int32_t bufferLength)
{
    if (!pBuffer || !bufferLength) return -1;
    int32_t n = (int32_t)fread(pBuffer, 1, (size_t)bufferLength, (FILE *)ref);
    return (n <= 0) ? -1 : n;
}

int32_t BAE_WriteFile(intptr_t ref, void *pBuffer, int32_t bufferLength)
{
    if (!pBuffer || !bufferLength) return -1;
    fflush((FILE *)ref);
    int32_t n = (int32_t)fwrite(pBuffer, 1, (size_t)bufferLength, (FILE *)ref);
    fflush((FILE *)ref);
    return (n <= 0) ? -1 : n;
}

int32_t BAE_SetFilePosition(intptr_t ref, uint32_t filePosition)
{
    return (fseek((FILE *)ref, (long)filePosition, SEEK_SET) == 0) ? 0 : -1;
}

uint32_t BAE_GetFilePosition(intptr_t ref)
{
    return (uint32_t)ftell((FILE *)ref);
}

uint32_t BAE_GetFileLength(intptr_t ref)
{
    long pos, len;
    fseek((FILE *)ref, 0, SEEK_END);
    len = ftell((FILE *)ref);
    fseek((FILE *)ref, 0, SEEK_SET);
    return (uint32_t)(len > 0 ? len : 0);
}

int BAE_SetFileLength(intptr_t ref, uint32_t newSize)
{
    if (!ref) return -1;
#ifdef _WIN32
    {
        int fd = _fileno((FILE *)ref);
        if (fd < 0) return -1;
        return (_chsize_s(fd, (int64_t)newSize) == 0) ? 0 : -1;
    }
#else
    {
        int fd = fileno((FILE *)ref);
        if (fd < 0) return -1;
        return (ftruncate(fd, (off_t)newSize) == 0) ? 0 : -1;
    }
#endif
}

/* ---- Audio card (no-op) ------------------------------------------------ */

int BAE_AcquireAudioCard(void *threadContext, uint32_t sampleRate, uint32_t channels, uint32_t bits)
{
    (void)threadContext; (void)sampleRate; (void)channels; (void)bits;
    return 0;
}

int BAE_ReleaseAudioCard(void *threadContext)   { (void)threadContext; return 0; }

int      BAE_Mute(void)                         { return 0; }
int      BAE_Unmute(void)                       { return 0; }
int      BAE_IsMuted(void)                      { return 0; }

void BAE_ProcessRouteBus(int currentRoute, int32_t *pChannels, int count)
{
    (void)currentRoute; (void)pChannels; (void)count;
}

void BAE_Idle(void *userContext)                { (void)userContext; }

void     BAE_UnlockAudioFrameThread(void)       {}
void     BAE_LockAudioFrameThread(void)         {}
void     BAE_BlockAudioFrameThread(void)        {}

uint32_t BAE_GetDeviceSamplesPlayedPosition(void) { return 0; }
int      BAE_GetAudioBufferCount(void)          { return 1; }
int32_t  BAE_GetAudioByteBufferSize(void)       { return g_audioByteBufferSize; }

int32_t  BAE_MaxDevices(void)                   { return 1; }
void     BAE_SetDeviceID(int32_t d, void *p)    { (void)d; (void)p; }
int32_t  BAE_GetDeviceID(void *p)               { (void)p; return 0; }
void     BAE_GetDeviceName(int32_t d, char *n, uint32_t l) { (void)d; (void)n; (void)l; }

/* ---- Frame thread (no-op) ---------------------------------------------- */

int BAE_CreateFrameThread(void *ctx, BAE_FrameThreadProc proc)
{
    (void)ctx; (void)proc;
    return 0;
}

int BAE_SetFrameThreadPriority(void *ctx, int priority)
{
    (void)ctx; (void)priority;
    return 0;
}

int BAE_DestroyFrameThread(void *ctx)           { (void)ctx; return 0; }

int BAE_SleepFrameThread(void *ctx, int32_t msec)
{
    (void)ctx; (void)msec;
    return 0;
}

/* ---- Mutex (no-op stubs, no threading library required) ---------------- */

int BAE_NewMutex(BAE_Mutex *lock, char *name, char *file, int lineno)
{
    (void)name; (void)file; (void)lineno;
    /* Allocate a trivial token so the pointer is non-NULL */
    *lock = (BAE_Mutex)BAE_Allocate(sizeof(int));
    return (*lock != NULL) ? 1 : 0;
}

void BAE_AcquireMutex(BAE_Mutex lock)           { (void)lock; }
void BAE_ReleaseMutex(BAE_Mutex lock)           { (void)lock; }

void BAE_DestroyMutex(BAE_Mutex lock)
{
    if (lock) BAE_Deallocate(lock);
}

/* ---- Audio capture (unsupported) --------------------------------------- */

int BAE_AcquireAudioCapture(void *ctx, uint32_t sr, uint32_t ch, uint32_t bits, uint32_t *hnd)
{
    (void)ctx; (void)sr; (void)ch; (void)bits; (void)hnd;
    return -1;
}

int  BAE_ReleaseAudioCapture(void *ctx)         { (void)ctx; return -1; }
int  BAE_StartAudioCapture(BAE_CaptureDone d, void *ctx) { (void)d; (void)ctx; return -1; }
int  BAE_StopAudioCapture(void)                 { return -1; }
int  BAE_PauseAudioCapture(void)                { return -1; }
int  BAE_ResumeAudioCapture(void)               { return -1; }
int32_t BAE_MaxCaptureDevices(void)             { return 0; }
void BAE_SetCaptureDeviceID(int32_t d, void *p) { (void)d; (void)p; }
int32_t BAE_GetCaptureDeviceID(void *p)         { (void)p; return 0; }
void BAE_GetCaptureDeviceName(int32_t d, char *n, uint32_t l) { (void)d; (void)n; (void)l; }
uint32_t BAE_GetCaptureBufferSizeInFrames()     { return 0; }
int  BAE_GetCaptureBufferCount()                { return 0; }
uint32_t BAE_GetDeviceSamplesCapturedPosition() { return 0; }

/* ---- Platform recorder stubs (unsupported) ----------------------------- */

int BAE_Platform_PCMRecorder_Start(const char *path, uint32_t ch, uint32_t sr, uint32_t bits)
{
    (void)path; (void)ch; (void)sr; (void)bits;
    return -1;
}

void BAE_Platform_PCMRecorder_Stop(void) {}

int BAE_Platform_MP3Recorder_Start(const char *path, uint32_t ch, uint32_t sr, uint32_t bits, uint32_t br)
{
    (void)path; (void)ch; (void)sr; (void)bits; (void)br;
    return -1;
}

void BAE_Platform_MP3Recorder_Stop(void) {}

void BAE_Platform_SetFlacRecorderCallback(void (*cb)(int16_t *, int16_t *, int)) { (void)cb; }
void BAE_Platform_ClearFlacRecorderCallback(void) {}

#if USE_VORBIS_ENCODER == TRUE
void BAE_Platform_SetVorbisRecorderCallback(void (*cb)(int16_t *, int16_t *, int)) { (void)cb; }
void BAE_Platform_ClearVorbisRecorderCallback(void) {}
#endif

#if USE_OPUS_ENCODER == TRUE
void BAE_Platform_SetOpusRecorderCallback(void (*cb)(int16_t *, int16_t *, int)) { (void)cb; }
void BAE_Platform_ClearOpusRecorderCallback(void) {}
#endif

/* EOF of BAE_API_Dummy.c */
