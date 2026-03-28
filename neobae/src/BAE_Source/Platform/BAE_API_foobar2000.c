// BAE_API_foobar2000.c - foobar2000 plugin backend for NeoBAE/miniBAE
//
// This platform backend is designed for use inside a foobar2000 input plugin
// (e.g. foo_midi). Unlike hardware-oriented backends (WinOS, SDL) there is no
// audio device or background rendering thread: foobar2000 owns playback and
// pulls audio by calling BAE_FB2K_RenderAudio() from the decoder's Render()
// method. BAE_BuildMixerSlice() is driven on demand from that call.
//
// Concretely, a foo_midi NeoBAE player would:
//   1. Call BAE_Setup() once.
//   2. Call BAE_AcquireAudioCard(NULL, sampleRate, 2, 16) on Startup().
//   3. In Render(audio_sample *buf, uint32_t frames):
//         Call BAE_FB2K_RenderAudio(tmp, frames * 4, frames) to fill int16 PCM,
//         then convert tmp (interleaved int16 stereo) to audio_sample floats.
//   4. Call BAE_ReleaseAudioCard(NULL) on Shutdown().
//   5. Call BAE_Cleanup() once.
//
// All Windows-specific APIs are used: CRITICAL_SECTION for mutexes,
// QueryPerformanceCounter for timing, and Win32 file I/O. The capture API and
// platform recorder helpers are stubbed out (foobar2000 handles output encoding).

#ifndef WIN32_EXTRA_LEAN
    #define WIN32_EXTRA_LEAN
#endif
#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <io.h>

#include "BAE_API.h"
#include <X_API.h>
#include <X_Assert.h>

// ---------------------------------------------------------------------------
// Engine forward declarations (provided by NeoBAE core, not this file)
// ---------------------------------------------------------------------------
extern void    BAE_BuildMixerSlice(void *threadContext, void *pAudioBuffer,
                                   int32_t bufferByteLength, int32_t sampleFrames);
extern int16_t BAE_GetMaxSamplePerSlice(void);

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------
static uint32_t g_sampleRate        = 44100;
static uint32_t g_channels          = 2;
static uint32_t g_bits              = 16;
static int32_t  g_audioByteBufferSize = 0;   // bytes per BAE slice
static uint32_t g_framesPerSlice    = 0;
static int      g_acquired          = 0;     // 1 after BAE_AcquireAudioCard
static int      g_muted             = 0;

static int16_t  g_unscaled_volume   = 256;   // 0..256
static int16_t  g_balance           = 0;     // -256..256

static uint32_t g_mem_used          = 0;
static uint32_t g_mem_used_max      = 0;

// High-resolution timer state
static LARGE_INTEGER g_perfFreq     = {0};
static LARGE_INTEGER g_startTicks   = {0};
static int           g_timerInit    = 0;

// Slice buffer for internal rendering
static int16_t *g_sliceBuf          = NULL;
static int32_t  g_sliceBufBytes     = 0;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static void PV_EnsureTimerInit(void)
{
    if (!g_timerInit)
    {
        QueryPerformanceFrequency(&g_perfFreq);
        QueryPerformanceCounter(&g_startTicks);
        g_timerInit = 1;
    }
}

static void PV_ComputeSliceSize(void)
{
    int16_t maxFrames = BAE_GetMaxSamplePerSlice();
    if (maxFrames <= 0) maxFrames = 512;
    uint32_t frames = (uint32_t)maxFrames;
    if (frames < 64) frames = 64;
    g_framesPerSlice = frames;
    g_audioByteBufferSize = (int32_t)(frames * g_channels * (g_bits / 8));
    g_audioByteBufferSize = (g_audioByteBufferSize + 63) & ~63; // align to 64 bytes
}

static void PV_EnsureSliceBuf(void)
{
    if (g_audioByteBufferSize <= 0) return;
    if (g_sliceBufBytes < g_audioByteBufferSize)
    {
        free(g_sliceBuf);
        g_sliceBuf = (int16_t *)calloc(1, (size_t)g_audioByteBufferSize);
        g_sliceBufBytes = g_sliceBuf ? g_audioByteBufferSize : 0;
    }
}

// ---------------------------------------------------------------------------
// BAE_FB2K_RenderAudio - the integration entry point for foo_midi
//
// Fills pBuffer with up to sampleFrames stereo int16 samples produced by the
// BAE mixer. The caller (NeoBAE player Render()) casts to audio_sample floats.
//
// pBuffer          : destination int16 interleaved PCM buffer
// bufferByteLength : byte capacity of pBuffer (must be >= sampleFrames * 4 for
//                    16-bit stereo)
// sampleFrames     : number of sample frames to render
//
// Returns the number of bytes written, or 0 on error / when muted.
// ---------------------------------------------------------------------------
int32_t BAE_FB2K_RenderAudio(void *pBuffer, int32_t bufferByteLength, int32_t sampleFrames)
{
    if (!pBuffer || bufferByteLength <= 0 || sampleFrames <= 0) return 0;
    if (!g_acquired || g_muted) { memset(pBuffer, 0, (size_t)bufferByteLength); return bufferByteLength; }

    int32_t bytesNeeded = sampleFrames * (int32_t)g_channels * (int32_t)(g_bits / 8);
    if (bytesNeeded > bufferByteLength) bytesNeeded = bufferByteLength;

    // If the request fits within one slice, call the engine once directly into
    // the caller's buffer (zero-copy fast path).
    if (bytesNeeded <= g_audioByteBufferSize)
    {
        BAE_BuildMixerSlice(NULL, pBuffer, bytesNeeded, sampleFrames);
        return bytesNeeded;
    }

    // Otherwise render in slice-sized chunks and copy.
    PV_EnsureSliceBuf();
    if (!g_sliceBuf) return 0;

    uint8_t *dst         = (uint8_t *)pBuffer;
    int32_t  bytesLeft   = bytesNeeded;
    int32_t  frameBytes  = (int32_t)(g_channels * (g_bits / 8));
    int32_t  bytesWritten = 0;

    while (bytesLeft > 0)
    {
        int32_t chunkBytes  = (bytesLeft < g_audioByteBufferSize) ? bytesLeft : g_audioByteBufferSize;
        int32_t chunkFrames = chunkBytes / frameBytes;
        if (chunkFrames <= 0) break;

        BAE_BuildMixerSlice(NULL, g_sliceBuf, chunkBytes, chunkFrames);
        memcpy(dst + bytesWritten, g_sliceBuf, (size_t)chunkBytes);
        bytesWritten += chunkBytes;
        bytesLeft    -= chunkBytes;
    }
    return bytesWritten;
}

// ---------------------------------------------------------------------------
// System setup / cleanup
// ---------------------------------------------------------------------------
int BAE_Setup(void)
{
    PV_EnsureTimerInit();
    return 0;
}

int BAE_Cleanup(void)
{
    if (g_sliceBuf) { free(g_sliceBuf); g_sliceBuf = NULL; g_sliceBufBytes = 0; }
    return 0;
}

// ---------------------------------------------------------------------------
// Memory management
// ---------------------------------------------------------------------------
void *BAE_Allocate(uint32_t size)
{
    void *p = size ? calloc(1, size) : NULL;
    if (p) { g_mem_used += size; if (g_mem_used > g_mem_used_max) g_mem_used_max = g_mem_used; }
    return p;
}
void BAE_Deallocate(void *p)         { if (p) free(p); }
void BAE_AllocDebug(int debug)       { (void)debug; }
uint32_t BAE_GetSizeOfMemoryUsed(void)    { return g_mem_used; }
uint32_t BAE_GetMaxSizeOfMemoryUsed(void) { return g_mem_used_max; }
int BAE_IsBadReadPointer(void *p, uint32_t size)
{
    (void)size;
    return (p == NULL) ? 1 : 2; // 2 = not checked (safe fallback)
}
uint32_t BAE_SizeOfPointer(void *p)  { (void)p; return 0; }
void BAE_BlockMove(void *src, void *dst, uint32_t size)
{
    if (src && dst && size) memmove(dst, src, size);
}

// ---------------------------------------------------------------------------
// Audio capabilities
// ---------------------------------------------------------------------------
int BAE_IsStereoSupported(void) { return 1; }
int BAE_Is8BitSupported(void)   { return 1; }
int BAE_Is16BitSupported(void)  { return 1; }

int16_t BAE_GetHardwareVolume(void)             { return g_unscaled_volume; }
void    BAE_SetHardwareVolume(int16_t v)
{
    if (v < 0) v = 0; if (v > 256) v = 256;
    g_unscaled_volume = v;
}
int16_t BAE_GetHardwareBalance(void)            { return g_balance; }
void    BAE_SetHardwareBalance(int16_t b)
{
    if (b < -256) b = -256; if (b > 256) b = 256;
    g_balance = b;
}

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
uint32_t BAE_Microseconds(void)
{
    PV_EnsureTimerInit();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    LONGLONG delta = now.QuadPart - g_startTicks.QuadPart;
    double us = (double)delta * 1000000.0 / (double)g_perfFreq.QuadPart;
    return (uint32_t)us;
}
uint32_t BAE_GetMicroseconds(void) { return BAE_Microseconds(); }

void BAE_WaitMicroseconds(uint32_t wait)
{
    Sleep((wait + 999) / 1000);
}

// ---------------------------------------------------------------------------
// File I/O  (Win32 HANDLE based, mapped through an index table)
// ---------------------------------------------------------------------------
#define MAX_OPEN_FILES 64
static HANDLE g_file_table[MAX_OPEN_FILES];
static int    g_file_table_init = 0;

static void PV_InitFileTable(void)
{
    if (!g_file_table_init)
    {
        for (int i = 0; i < MAX_OPEN_FILES; i++) g_file_table[i] = INVALID_HANDLE_VALUE;
        g_file_table_init = 1;
    }
}

static intptr_t PV_AllocFileHandle(HANDLE h)
{
    PV_InitFileTable();
    if (h == INVALID_HANDLE_VALUE) return -1;
    for (int i = 1; i < MAX_OPEN_FILES; i++)
    {
        if (g_file_table[i] == INVALID_HANDLE_VALUE) { g_file_table[i] = h; return (intptr_t)i; }
    }
    CloseHandle(h);
    return -1;
}

static HANDLE PV_GetHandle(intptr_t ref)
{
    PV_InitFileTable();
    if (ref > 0 && ref < MAX_OPEN_FILES) return g_file_table[ref];
    return INVALID_HANDLE_VALUE;
}

static void PV_FreeHandle(intptr_t ref)
{
    if (ref > 0 && ref < MAX_OPEN_FILES) g_file_table[ref] = INVALID_HANDLE_VALUE;
}

void BAE_CopyFileNameNative(void *src, void *dst)
{
    if (src && dst) strcpy((char *)dst, (char *)src);
}

int32_t BAE_FileCreate(void *fileName)
{
    HANDLE h = CreateFileA((LPCSTR)fileName, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    CloseHandle(h);
    return 0;
}

int32_t BAE_FileDelete(void *fileName)
{
    return DeleteFileA((LPCSTR)fileName) ? 0 : -1;
}

intptr_t BAE_FileOpenForRead(void *fileName)
{
    HANDLE h = CreateFileA((LPCSTR)fileName, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    return PV_AllocFileHandle(h);
}

intptr_t BAE_FileOpenForWrite(void *fileName)
{
    HANDLE h = CreateFileA((LPCSTR)fileName, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    return PV_AllocFileHandle(h);
}

intptr_t BAE_FileOpenForReadWrite(void *fileName)
{
    HANDLE h = CreateFileA((LPCSTR)fileName, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    return PV_AllocFileHandle(h);
}

void BAE_FileClose(intptr_t ref)
{
    HANDLE h = PV_GetHandle(ref);
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); PV_FreeHandle(ref); }
}

int32_t BAE_ReadFile(intptr_t ref, void *pBuf, int32_t len)
{
    HANDLE h = PV_GetHandle(ref);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD read = 0;
    return ReadFile(h, pBuf, (DWORD)len, &read, NULL) ? (int32_t)read : -1;
}

int32_t BAE_WriteFile(intptr_t ref, void *pBuf, int32_t len)
{
    HANDLE h = PV_GetHandle(ref);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD written = 0;
    return WriteFile(h, pBuf, (DWORD)len, &written, NULL) ? (int32_t)written : -1;
}

int32_t BAE_SetFilePosition(intptr_t ref, uint32_t pos)
{
    HANDLE h = PV_GetHandle(ref);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD r = SetFilePointer(h, (LONG)pos, NULL, FILE_BEGIN);
    return (r == INVALID_SET_FILE_POINTER) ? -1 : 0;
}

uint32_t BAE_GetFilePosition(intptr_t ref)
{
    HANDLE h = PV_GetHandle(ref);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD pos = SetFilePointer(h, 0, NULL, FILE_CURRENT);
    return (pos == INVALID_SET_FILE_POINTER) ? 0 : (uint32_t)pos;
}

uint32_t BAE_GetFileLength(intptr_t ref)
{
    HANDLE h = PV_GetHandle(ref);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD hi = 0;
    DWORD lo = GetFileSize(h, &hi);
    if (lo == INVALID_FILE_SIZE) return 0;
    // Cap at UINT32_MAX; 64-bit lengths unsupported here
    if (hi) return 0xFFFFFFFFu;
    return (uint32_t)lo;
}

int BAE_SetFileLength(intptr_t ref, uint32_t newSize)
{
    HANDLE h = PV_GetHandle(ref);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD cur = SetFilePointer(h, (LONG)newSize, NULL, FILE_BEGIN);
    if (cur == INVALID_SET_FILE_POINTER) return -1;
    return SetEndOfFile(h) ? 0 : -1;
}

// ---------------------------------------------------------------------------
// Audio buffer metrics
// ---------------------------------------------------------------------------
int      BAE_GetAudioBufferCount(void)     { return 1; }
int32_t  BAE_GetAudioByteBufferSize(void)  { return g_audioByteBufferSize; }

uint32_t BAE_GetSliceTimeInMicroseconds(void)
{
    if (!g_sampleRate || !g_framesPerSlice) return 11000;
    return (uint32_t)((uint64_t)g_framesPerSlice * 1000000ULL / g_sampleRate);
}

// ---------------------------------------------------------------------------
// Audio card acquire / release  (no real device - foobar2000 owns output)
// ---------------------------------------------------------------------------
int BAE_AcquireAudioCard(void *threadContext, uint32_t sampleRate, uint32_t channels, uint32_t bits)
{
    (void)threadContext;
    g_sampleRate = sampleRate;
    g_channels   = channels ? channels : 2;
    g_bits       = (bits == 8) ? 8 : 16;
    g_acquired   = 1;

    PV_ComputeSliceSize();
    PV_EnsureSliceBuf();
    return 0;
}

int BAE_ReleaseAudioCard(void *threadContext)
{
    (void)threadContext;
    g_acquired = 0;
    if (g_sliceBuf) { free(g_sliceBuf); g_sliceBuf = NULL; g_sliceBufBytes = 0; }
    return 0;
}

int BAE_Mute(void)   { g_muted = 1; return 0; }
int BAE_Unmute(void) { g_muted = 0; return 0; }
int BAE_IsMuted(void){ return g_muted; }

void BAE_ProcessRouteBus(int currentRoute, int32_t *pChannels, int count)
{
    (void)currentRoute; (void)pChannels; (void)count;
}

// In the pull model there is no idle loop; this is a no-op.
void BAE_Idle(void *userContext) { (void)userContext; }

// Frame thread lock/block - no-ops (no background thread).
void BAE_UnlockAudioFrameThread(void) {}
void BAE_LockAudioFrameThread(void)   {}
void BAE_BlockAudioFrameThread(void)  { Sleep(1); }

uint32_t BAE_GetDeviceSamplesPlayedPosition(void) { return 0; }

// ---------------------------------------------------------------------------
// Device enumeration  (single virtual device)
// ---------------------------------------------------------------------------
int32_t BAE_MaxDevices(void) { return 1; }

void BAE_SetDeviceID(int32_t deviceID, void *deviceParameter)
{
    (void)deviceID; (void)deviceParameter;
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
        _snprintf_s(cName, cNameLength, _TRUNCATE, "WinOS,foobar2000,pull");
}

// ---------------------------------------------------------------------------
// Frame thread stubs  (foobar2000 calls Render() directly; no engine thread)
// ---------------------------------------------------------------------------
int BAE_CreateFrameThread(void *threadContext, BAE_FrameThreadProc proc)
{
    (void)threadContext; (void)proc;
    return 0;
}
int BAE_SetFrameThreadPriority(void *threadContext, int priority)
{
    (void)threadContext; (void)priority;
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
    Sleep((DWORD)msec);
    return 0;
}

// ---------------------------------------------------------------------------
// Mutex  (backed by CRITICAL_SECTION)
// ---------------------------------------------------------------------------
typedef struct { CRITICAL_SECTION cs; } sFB2KMutex;

int BAE_NewMutex(BAE_Mutex *lock, char *name, char *file, int lineno)
{
    (void)name; (void)file; (void)lineno;
    sFB2KMutex *m = (sFB2KMutex *)BAE_Allocate(sizeof(sFB2KMutex));
    if (!m) return 0;
    InitializeCriticalSection(&m->cs);
    *lock = (BAE_Mutex)m;
    return 1;
}

void BAE_AcquireMutex(BAE_Mutex lock)
{
    if (!lock) return;
    EnterCriticalSection(&((sFB2KMutex *)lock)->cs);
}

void BAE_ReleaseMutex(BAE_Mutex lock)
{
    if (!lock) return;
    LeaveCriticalSection(&((sFB2KMutex *)lock)->cs);
}

void BAE_DestroyMutex(BAE_Mutex lock)
{
    if (!lock) return;
    sFB2KMutex *m = (sFB2KMutex *)lock;
    DeleteCriticalSection(&m->cs);
    BAE_Deallocate(m);
}

// ---------------------------------------------------------------------------
// Debug helpers
// ---------------------------------------------------------------------------
void BAE_DisplayMemoryUsage(int detailLevel) { (void)detailLevel; }

void BAE_PrintHexDump(void *address, int32_t length)
{
    unsigned char *p = (unsigned char *)address;
    for (int32_t i = 0; i < length; i++)
    {
        if ((i % 16) == 0) BAE_PRINTF("\n%08lx: ", (unsigned long)i);
        BAE_PRINTF("%02X ", p[i]);
    }
    BAE_PRINTF("\n");
}

// ---------------------------------------------------------------------------
// Capture API stubs  (not applicable inside a foobar2000 decoder plugin)
// ---------------------------------------------------------------------------
int BAE_AcquireAudioCapture(void *tc, uint32_t sr, uint32_t ch, uint32_t bits, uint32_t *ph)
{
    (void)tc; (void)sr; (void)ch; (void)bits; (void)ph;
    return -1;
}
int BAE_ReleaseAudioCapture(void *tc)               { (void)tc; return -1; }
int BAE_StartAudioCapture(BAE_CaptureDone d, void *c){ (void)d; (void)c; return -1; }
int BAE_StopAudioCapture(void)                       { return -1; }
int BAE_PauseAudioCapture(void)                      { return -1; }
int BAE_ResumeAudioCapture(void)                     { return -1; }
int32_t BAE_MaxCaptureDevices(void)                  { return 0; }
void BAE_SetCaptureDeviceID(int32_t id, void *p)     { (void)id; (void)p; }
int32_t BAE_GetCaptureDeviceID(void *p)              { (void)p; return -1; }
void BAE_GetCaptureDeviceName(int32_t id, char *n, uint32_t l)
{
    (void)id;
    if (n && l) *n = '\0';
}
uint32_t BAE_GetCaptureBufferSizeInFrames(void) { return 0; }
int      BAE_GetCaptureBufferCount(void)        { return 0; }
uint32_t BAE_GetDeviceSamplesCapturedPosition(void) { return 0; }

// ---------------------------------------------------------------------------
// Platform recorder stubs  (foobar2000 handles encoding externally)
// ---------------------------------------------------------------------------
int  BAE_Platform_PCMRecorder_Start(const char *path, uint32_t ch, uint32_t sr, uint32_t b)
{
    (void)path; (void)ch; (void)sr; (void)b;
    return -1; // not supported in this backend
}
void BAE_Platform_PCMRecorder_Stop(void) {}

int  BAE_Platform_MP3Recorder_Start(const char *path, uint32_t ch, uint32_t sr, uint32_t b, uint32_t br)
{
    (void)path; (void)ch; (void)sr; (void)b; (void)br;
    return -1;
}
void BAE_Platform_MP3Recorder_Stop(void) {}

void BAE_Platform_SetFlacRecorderCallback(void (*cb)(int16_t *, int16_t *, int)) { (void)cb; }
void BAE_Platform_ClearFlacRecorderCallback(void) {}

// (End of BAE_API_foobar2000.c)
