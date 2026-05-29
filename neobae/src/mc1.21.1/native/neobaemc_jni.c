/*
 * NeoBAE for Minecraft — JNI shim
 *
 * Exports the JNI symbols expected by com.zefie.neobaemc.audio.{Mixer,Song,Sound,LoadResult}.
 *
 * Scope: only the Mixer + Song + Sound calls used by the Etched decoder
 * override path (NeoBAEAudioStream + AbstractOnlineSoundInstanceMixin) are
 * actually wired up. Convenience accessors that the Java classes still
 * declare (volume queries, sample-position seek, etc.) are present as
 * stubs returning 0 / -1 so the shared library links cleanly.
 *
 *   © 2026 zefie. BSD-3-Clause (matches the rest of NeoBAE).
 */

#include <jni.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "X_API.h"
#include "BAE_API.h"
#include "NeoBAE.h"
#include "GenPriv.h"

extern BAEFileType X_DetermineFileTypeByData(const unsigned char *data, int32_t length);

/* The native BAEFileType enum values shift depending on which USE_*_DECODER /
 * USE_*_SUPPORT macros are defined at compile time (see NeoBAE.h). The Java
 * side hard-codes a stable numbering, so we translate native -> stable here.
 * Keep these magic numbers in sync with com.zefie.neobaemc.audio.Mixer. */
enum {
    NB_JT_INVALID = 0,
    NB_JT_AIFF    = 1,
    NB_JT_WAVE    = 2,
    NB_JT_MPEG    = 3,
    NB_JT_AU      = 4,
    NB_JT_MIDI    = 5,
    NB_JT_FLAC    = 6,
    NB_JT_VORBIS  = 7,
    NB_JT_OPUS    = 8,
    NB_JT_GROOVOID= 9,
    NB_JT_RMF     = 10,
    NB_JT_XMF     = 11,
    NB_JT_MTHC    = 12,
    NB_JT_RMI     = 13,
    NB_JT_ADP     = 14,
    NB_JT_RAW_PCM = 15
};

static jint nb_translate_filetype(BAEFileType ft)
{
    switch (ft) {
        case BAE_INVALID_TYPE: return NB_JT_INVALID;
        case BAE_AIFF_TYPE:    return NB_JT_AIFF;
        case BAE_WAVE_TYPE:    return NB_JT_WAVE;
        case BAE_MPEG_TYPE:    return NB_JT_MPEG;
        case BAE_AU_TYPE:      return NB_JT_AU;
        case BAE_MIDI_TYPE:    return NB_JT_MIDI;
#if USE_FLAC_DECODER == TRUE || USE_FLAC_ENCODER == TRUE
        case BAE_FLAC_TYPE:    return NB_JT_FLAC;
#endif
#if USE_VORBIS_DECODER == TRUE || USE_VORBIS_ENCODER == TRUE
        case BAE_VORBIS_TYPE:  return NB_JT_VORBIS;
#endif
#if USE_OPUS_DECODER == TRUE || USE_OPUS_ENCODER == TRUE
        case BAE_OPUS_TYPE:    return NB_JT_OPUS;
#endif
        case BAE_GROOVOID:     return NB_JT_GROOVOID;
        case BAE_RMF:          return NB_JT_RMF;
#if USE_XMF_SUPPORT == TRUE
        case BAE_XMF:          return NB_JT_XMF;
#endif
#if USE_MTHC_SUPPORT == TRUE
        case BAE_MTHC:         return NB_JT_MTHC;
#endif
        case BAE_RMI:          return NB_JT_RMI;
#if USE_ADP_SUPPORT == TRUE
        case BAE_ADP_TYPE:     return NB_JT_ADP;
#endif
        case BAE_RAW_PCM:      return NB_JT_RAW_PCM;
        default:               return NB_JT_INVALID;
    }
}

/* In this engine BAE_BOOL is `char`; the headers don't define BAE_FALSE/BAE_TRUE. */
#define NB_FALSE ((BAE_BOOL)0)
#define NB_TRUE  ((BAE_BOOL)1)

/* ============================ Mixer ===================================== */

JNIEXPORT jlong JNICALL Java_com_zefie_neobaemc_audio_Mixer__1newMixer(JNIEnv *env, jclass cls)
{ (void)env; (void)cls; return (jlong)(intptr_t)BAEMixer_New(); }

JNIEXPORT void JNICALL Java_com_zefie_neobaemc_audio_Mixer__1deleteMixer(JNIEnv *env, jclass cls, jlong ref)
{ (void)env; (void)cls; if (ref) BAEMixer_Delete((BAEMixer)(intptr_t)ref); }

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Mixer__1openMixer(
        JNIEnv *env, jclass cls, jlong ref,
        jint sampleRate, jint terpMode, jint songVoices, jint soundVoices, jint mixLevel)
{
    (void)env; (void)cls;
    if (!ref) return -1;
    BAEResult rc = BAEMixer_Open((BAEMixer)(intptr_t)ref,
                               (BAERate)sampleRate,
                               (BAETerpMode)terpMode,
                               (BAEAudioModifiers)(BAE_USE_16 | BAE_USE_STEREO),
                               (int16_t)songVoices,
                               (int16_t)soundVoices,
                               (int16_t)mixLevel,
                               NB_TRUE /* engage audio */);
#if _BUILT_IN_PATCHES == TRUE
    /* MIDI playback needs an instrument bank. Auto-load the built-in
     * patches the Makefile compiled in so the caller doesn't have to. */
    if (rc == BAE_NO_ERROR) {
        BAEResult bankRc = BAEMixer_LoadBuiltinBank((BAEMixer)(intptr_t)ref, NULL);
        if (bankRc != BAE_NO_ERROR) {
            fprintf(stderr, "[NeoBAE] LoadBuiltinBank failed: %d\n", (int)bankRc);
        }
    }
#endif
    return (jint)rc;
}

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Mixer__1disengageAudio(JNIEnv *e, jclass c, jlong ref)
{ (void)e; (void)c; return ref ? (jint)BAEMixer_DisengageAudio((BAEMixer)(intptr_t)ref) : -1; }

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Mixer__1reengageAudio(JNIEnv *e, jclass c, jlong ref)
{ (void)e; (void)c; return ref ? (jint)BAEMixer_ReengageAudio((BAEMixer)(intptr_t)ref) : -1; }

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Mixer__1isAudioEngaged(JNIEnv *e, jclass c, jlong ref)
{ (void)e; (void)c; if(!ref) return 0; BAE_BOOL b=NB_FALSE; BAEMixer_IsAudioEngaged((BAEMixer)(intptr_t)ref,&b); return b?1:0; }

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Mixer__1isAudioTailActive(JNIEnv *e, jclass c, jlong ref)
{ (void)e; (void)c; if(!ref) return 0; return BAEMixer_IsAudioTailActive((BAEMixer)(intptr_t)ref) ? 1 : 0; }

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Mixer__1setDefaultReverb(JNIEnv *e, jclass c, jlong ref, jint r)
{ (void)e; (void)c; (void)ref; (void)r; return -1; /* engine has no runtime reverb-type setter; pass on Open */ }

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Mixer__1getActiveVoiceCount(JNIEnv *e, jclass c, jlong ref)
{ (void)e; (void)c; (void)ref; return 0; /* no public accessor in this engine */ }

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Mixer__1addBankFromFile(JNIEnv *env, jclass c, jlong ref, jstring jpath)
{
    (void)c;
    if (!ref || !jpath) return -1;
    const char *p = (*env)->GetStringUTFChars(env, jpath, NULL);
    BAEResult rc = BAEMixer_AddBankFromFile((BAEMixer)(intptr_t)ref, (BAEPathName)p, NULL);
    (*env)->ReleaseStringUTFChars(env, jpath, p);
    return (jint)rc;
}

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Mixer__1addBankFromMemory(JNIEnv *env, jclass c, jlong ref, jbyteArray jdata)
{
    (void)c;
    if (!ref || !jdata) return -1;
    jsize n = (*env)->GetArrayLength(env, jdata);
    jbyte *p = (*env)->GetByteArrayElements(env, jdata, NULL);
    BAEResult rc = BAEMixer_AddBankFromMemory((BAEMixer)(intptr_t)ref, (void *)p, (uint32_t)n, NULL);
    (*env)->ReleaseByteArrayElements(env, jdata, p, JNI_ABORT);
    return (jint)rc;
}

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Mixer__1addBankFromMemoryWithFilename(JNIEnv *env, jclass c, jlong ref, jbyteArray jdata, jstring jname)
{ (void)jname; return Java_com_zefie_neobaemc_audio_Mixer__1addBankFromMemory(env, c, ref, jdata); }

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Mixer__1setMasterVolume(JNIEnv *e, jclass c, jlong ref, jint fixed)
{ (void)e; (void)c; return ref ? (jint)BAEMixer_SetMasterVolume((BAEMixer)(intptr_t)ref, (BAE_UNSIGNED_FIXED)fixed) : -1; }

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Mixer__1setGlobalVolume(JNIEnv *e, jclass c, jlong ref, jint fixed)
{ return Java_com_zefie_neobaemc_audio_Mixer__1setMasterVolume(e, c, ref, fixed); }

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Mixer__1getGlobalVolume(JNIEnv *e, jclass c, jlong ref)
{ (void)e; (void)c; if(!ref) return 0; BAE_UNSIGNED_FIXED v=0; BAEMixer_GetMasterVolume((BAEMixer)(intptr_t)ref,&v); return (jint)v; }

JNIEXPORT jstring JNICALL Java_com_zefie_neobaemc_audio_Mixer__1getBankFriendlyName(JNIEnv *env, jclass c, jlong ref)
{ (void)c; (void)ref; return (*env)->NewStringUTF(env, ""); /* engine has no friendly-name accessor */ }

JNIEXPORT jstring JNICALL Java_com_zefie_neobaemc_audio_Mixer__1getVersion(JNIEnv *env, jclass c)
{ (void)c; const char *v = BAE_GetVersion(); return (*env)->NewStringUTF(env, v ? v : ""); }

JNIEXPORT jstring JNICALL Java_com_zefie_neobaemc_audio_Mixer__1getCompileInfo(JNIEnv *env, jclass c)
{ (void)c; return (*env)->NewStringUTF(env, "NeoBAE for Minecraft (NeoForge 1.21.1)"); }

JNIEXPORT jstring JNICALL Java_com_zefie_neobaemc_audio_Mixer__1getFeatureString(JNIEnv *env, jclass c)
{ (void)c; return (*env)->NewStringUTF(env, "pull,midi,rmf,xmf,rmi,mp3,ogg,flac,wav,aiff,au"); }

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Mixer__1determineFileTypeByData(JNIEnv *env, jclass c, jbyteArray jdata, jint len)
{
    (void)c;
    if (!jdata || len <= 0) return 0;
    jsize cap = (*env)->GetArrayLength(env, jdata);
    if (len > cap) len = cap;
    jbyte *p = (*env)->GetByteArrayElements(env, jdata, NULL);
    BAEFileType ft = X_DetermineFileTypeByData((const unsigned char *)p, (int32_t)len);
    (*env)->ReleaseByteArrayElements(env, jdata, p, JNI_ABORT);
    return nb_translate_filetype(ft);
}

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Mixer__1loadFromMemory(JNIEnv *env, jclass c, jlong ref, jbyteArray jdata, jobject jresult)
{
    (void)c;
    if (!ref || !jdata || !jresult) return -1;
    jsize n = (*env)->GetArrayLength(env, jdata);
    jbyte *p = (*env)->GetByteArrayElements(env, jdata, NULL);

    BAELoadResult result; memset(&result, 0, sizeof(result));
    BAEResult rc = BAEMixer_LoadFromMemory((BAEMixer)(intptr_t)ref, (void const *)p, (uint32_t)n, &result);

    (*env)->ReleaseByteArrayElements(env, jdata, p, JNI_ABORT);

    jclass rcls = (*env)->GetObjectClass(env, jresult);
    jfieldID fType  = (*env)->GetFieldID(env, rcls, "type",            "I");
    jfieldID fFt    = (*env)->GetFieldID(env, rcls, "fileType",        "I");
    jfieldID fRes   = (*env)->GetFieldID(env, rcls, "result",          "I");
    jfieldID fSong  = (*env)->GetFieldID(env, rcls, "songReference",   "J");
    jfieldID fSound = (*env)->GetFieldID(env, rcls, "soundReference",  "J");

    (*env)->SetIntField(env, jresult, fType, (jint)result.type);
    (*env)->SetIntField(env, jresult, fFt,   nb_translate_filetype(result.fileType));
    (*env)->SetIntField(env, jresult, fRes,  (jint)result.result);
    if (result.type == BAE_LOAD_TYPE_SONG && result.data.song)
        (*env)->SetLongField(env, jresult, fSong,  (jlong)(intptr_t)result.data.song);
    else if (result.type == BAE_LOAD_TYPE_SOUND && result.data.sound)
        (*env)->SetLongField(env, jresult, fSound, (jlong)(intptr_t)result.data.sound);
    return (jint)rc;
}

/* Pull-mode renderer: produce `frames` of interleaved stereo S16 LE into `jout`.
 * Calls BAE_BuildMixerSlice in slice-sized chunks (engine-configured). */
JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Mixer__1renderSamples(JNIEnv *env, jclass c, jlong ref, jbyteArray jout, jint frames)
{
    (void)c; (void)ref;
    if (!jout || frames <= 0) return 0;
    int byteLen = frames * 4;
    jsize cap = (*env)->GetArrayLength(env, jout);
    if (cap < byteLen) return 0;

    jbyte *p = (*env)->GetByteArrayElements(env, jout, NULL);

    int sliceFrames = (int)BAE_GetMaxSamplePerSlice();
    if (sliceFrames <= 0) sliceFrames = 512;
    int produced = 0;
    while (produced < frames) {
        int want = frames - produced;
        if (want > sliceFrames) want = sliceFrames;
        int wantBytes = want * 4;
        BAE_BuildMixerSlice(NULL, (void *)(p + produced * 4), wantBytes, want);
        produced += want;
    }

    (*env)->ReleaseByteArrayElements(env, jout, p, 0);
    return produced;
}

/* ============================ Song ====================================== */

JNIEXPORT jlong JNICALL Java_com_zefie_neobaemc_audio_Song__1newNativeSong(JNIEnv *e, jobject self, jlong mref)
{ (void)e; (void)self; if(!mref) return 0; return (jlong)(intptr_t)BAESong_New((BAEMixer)(intptr_t)mref); }

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Song__1loadSong(JNIEnv *env, jobject self, jlong ref, jstring jpath)
{
    (void)self;
    if (!ref || !jpath) return -1;
    const char *p = (*env)->GetStringUTFChars(env, jpath, NULL);
    BAEResult rc = BAESong_LoadMidiFromFile((BAESong)(intptr_t)ref, (BAEPathName)p, NB_FALSE);
    (*env)->ReleaseStringUTFChars(env, jpath, p);
    return (jint)rc;
}

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Song__1loadSongFromMemory(JNIEnv *env, jobject self, jlong ref, jbyteArray jdata)
{
    (void)self;
    if (!ref || !jdata) return -1;
    jsize n = (*env)->GetArrayLength(env, jdata);
    jbyte *p = (*env)->GetByteArrayElements(env, jdata, NULL);
    BAEResult rc = BAESong_LoadMidiFromMemory((BAESong)(intptr_t)ref, (void*)p, (uint32_t)n, NB_FALSE);
    (*env)->ReleaseByteArrayElements(env, jdata, p, JNI_ABORT);
    return (jint)rc;
}

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Song__1loadRmiFromMemory(JNIEnv *env, jobject self, jlong ref, jbyteArray jdata, jboolean useEmbedded)
{ (void)useEmbedded; return Java_com_zefie_neobaemc_audio_Song__1loadSongFromMemory(env, self, ref, jdata); }

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Song__1prerollSong(JNIEnv *e, jobject self, jlong ref)
{ (void)e; (void)self; return ref ? (jint)BAESong_Preroll((BAESong)(intptr_t)ref) : -1; }

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Song__1startSong(JNIEnv *e, jobject self, jlong ref)
{ (void)e; (void)self; return ref ? (jint)BAESong_Start((BAESong)(intptr_t)ref, 0) : -1; }

JNIEXPORT void JNICALL Java_com_zefie_neobaemc_audio_Song__1stopSong(JNIEnv *e, jobject self, jlong ref, jboolean del)
{
    (void)e; (void)self;
    if (!ref) return;
    BAESong_Stop((BAESong)(intptr_t)ref, NB_FALSE);
    if (del) BAESong_Delete((BAESong)(intptr_t)ref);
}

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Song__1pauseSong(JNIEnv *e, jobject s, jlong ref)
{ (void)e; (void)s; return ref ? (jint)BAESong_Pause((BAESong)(intptr_t)ref) : -1; }

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Song__1resumeSong(JNIEnv *e, jobject s, jlong ref)
{ (void)e; (void)s; return ref ? (jint)BAESong_Resume((BAESong)(intptr_t)ref) : -1; }

JNIEXPORT jboolean JNICALL Java_com_zefie_neobaemc_audio_Song__1isSongPaused(JNIEnv *e, jobject s, jlong ref)
{ (void)e; (void)s; if(!ref) return JNI_TRUE; BAE_BOOL b=NB_FALSE; BAESong_IsPaused((BAESong)(intptr_t)ref,&b); return b?JNI_TRUE:JNI_FALSE; }

JNIEXPORT jboolean JNICALL Java_com_zefie_neobaemc_audio_Song__1isSongDone(JNIEnv *e, jobject s, jlong ref)
{ (void)e; (void)s; if(!ref) return JNI_TRUE; BAE_BOOL b=NB_FALSE; BAESong_IsDone((BAESong)(intptr_t)ref,&b); return b?JNI_TRUE:JNI_FALSE; }

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Song__1setSongVolume(JNIEnv *e, jclass c, jlong ref, jint fixed)
{ (void)e; (void)c; return ref ? (jint)BAESong_SetVolume((BAESong)(intptr_t)ref, (BAE_UNSIGNED_FIXED)fixed) : -1; }

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Song__1getSongVolume(JNIEnv *e, jclass c, jlong ref)
{ (void)e; (void)c; if(!ref) return 0; BAE_UNSIGNED_FIXED v=0; BAESong_GetVolume((BAESong)(intptr_t)ref,&v); return (jint)v; }

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Song__1getSongPositionUS(JNIEnv *e, jclass c, jlong ref)
{ (void)e; (void)c; if(!ref) return 0; uint32_t us=0; BAESong_GetMicrosecondPosition((BAESong)(intptr_t)ref,&us); return (jint)us; }

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Song__1setSongPositionUS(JNIEnv *e, jclass c, jlong ref, jint us)
{ (void)e; (void)c; return ref ? (jint)BAESong_SetMicrosecondPosition((BAESong)(intptr_t)ref, (uint32_t)us) : -1; }

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Song__1getSongLengthUS(JNIEnv *e, jclass c, jlong ref)
{ (void)e; (void)c; if(!ref) return 0; uint32_t us=0; BAESong_GetMicrosecondLength((BAESong)(intptr_t)ref,&us); return (jint)us; }

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Song__1setSongLoops(JNIEnv *e, jclass c, jlong ref, jint n)
{ (void)e; (void)c; return ref ? (jint)BAESong_SetLoops((BAESong)(intptr_t)ref, (int16_t)n) : -1; }

/* ============================ Sound ===================================== */

JNIEXPORT jlong JNICALL Java_com_zefie_neobaemc_audio_Sound__1newNativeSound(JNIEnv *e, jobject s, jlong mref)
{ (void)e; (void)s; if(!mref) return 0; return (jlong)(intptr_t)BAESound_New((BAEMixer)(intptr_t)mref); }

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Sound__1loadSoundFromMemory(JNIEnv *env, jobject self, jlong ref, jbyteArray jdata)
{
    (void)self;
    if (!ref || !jdata) return -1;
    jsize n = (*env)->GetArrayLength(env, jdata);
    jbyte *p = (*env)->GetByteArrayElements(env, jdata, NULL);
    /* fileType=0 -> let engine auto-detect via X_DetermineFileTypeByData */
    BAEResult rc = BAESound_LoadMemorySample((BAESound)(intptr_t)ref, (void*)p, (uint32_t)n, (BAEFileType)0);
    (*env)->ReleaseByteArrayElements(env, jdata, p, JNI_ABORT);
    return (jint)rc;
}

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Sound__1loadSoundFromFile(JNIEnv *env, jobject self, jlong ref, jstring jpath)
{
    (void)self;
    if (!ref || !jpath) return -1;
    const char *p = (*env)->GetStringUTFChars(env, jpath, NULL);
    BAEResult rc = BAESound_LoadFileSample((BAESound)(intptr_t)ref, (BAEPathName)p, (BAEFileType)0);
    (*env)->ReleaseStringUTFChars(env, jpath, p);
    return (jint)rc;
}

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Sound__1startSound(JNIEnv *e, jobject s, jlong ref, jint frames, jint vol)
{
    (void)e; (void)s;
    if (!ref) return -1;
    /* BAESound_Start(sound, priority, sampleVolume, startOffsetFrame) */
    return (jint)BAESound_Start((BAESound)(intptr_t)ref,
                                (int16_t)0,
                                (BAE_UNSIGNED_FIXED)vol,
                                (uint32_t)frames);
}

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Sound__1stopSound(JNIEnv *e, jobject s, jlong ref, jboolean del)
{
    (void)e; (void)s;
    if (!ref) return -1;
    BAESound_Stop((BAESound)(intptr_t)ref, NB_FALSE);
    if (del) BAESound_Delete((BAESound)(intptr_t)ref);
    return 0;
}

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Sound__1pauseSound(JNIEnv *e, jobject s, jlong ref)
{ (void)e; (void)s; return ref ? (jint)BAESound_Pause((BAESound)(intptr_t)ref) : -1; }

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Sound__1resumeSound(JNIEnv *e, jobject s, jlong ref)
{ (void)e; (void)s; return ref ? (jint)BAESound_Resume((BAESound)(intptr_t)ref) : -1; }

JNIEXPORT jboolean JNICALL Java_com_zefie_neobaemc_audio_Sound__1isSoundPaused(JNIEnv *e, jobject s, jlong ref)
{ (void)e; (void)s; if(!ref) return JNI_TRUE; BAE_BOOL b=NB_FALSE; BAESound_IsPaused((BAESound)(intptr_t)ref,&b); return b?JNI_TRUE:JNI_FALSE; }

JNIEXPORT jboolean JNICALL Java_com_zefie_neobaemc_audio_Sound__1isSoundDone(JNIEnv *e, jobject s, jlong ref)
{ (void)e; (void)s; if(!ref) return JNI_TRUE; BAE_BOOL b=NB_FALSE; BAESound_IsDone((BAESound)(intptr_t)ref,&b); return b?JNI_TRUE:JNI_FALSE; }

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Sound__1setSoundVolume(JNIEnv *e, jclass c, jlong ref, jint fixed)
{ (void)e; (void)c; return ref ? (jint)BAESound_SetVolume((BAESound)(intptr_t)ref, (BAE_UNSIGNED_FIXED)fixed) : -1; }

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Sound__1getSoundVolume(JNIEnv *e, jclass c, jlong ref)
{ (void)e; (void)c; if(!ref) return 0; BAE_UNSIGNED_FIXED v=0; BAESound_GetVolume((BAESound)(intptr_t)ref,&v); return (jint)v; }

/* The following four don't exist in this engine; expose as stubs so the
 * Java class can declare them without breaking link-time. */
JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Sound__1getSoundPositionFrames(JNIEnv *e, jclass c, jlong ref)
{ (void)e; (void)c; (void)ref; return 0; }

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Sound__1getSoundLengthFrames(JNIEnv *e, jclass c, jlong ref)
{ (void)e; (void)c; (void)ref; return 0; }

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Sound__1getSoundSampleRate(JNIEnv *e, jclass c, jlong ref)
{ (void)e; (void)c; if(!ref) return 0; BAE_UNSIGNED_FIXED r=0; BAESound_GetRate((BAESound)(intptr_t)ref,&r); return (jint)(r>>16); }

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Sound__1setSoundPositionFrames(JNIEnv *e, jclass c, jlong ref, jint frames)
{ (void)e; (void)c; (void)ref; (void)frames; return -1; }

JNIEXPORT jint JNICALL Java_com_zefie_neobaemc_audio_Sound__1setSoundLoops(JNIEnv *e, jclass c, jlong ref, jint n)
{ (void)e; (void)c; return ref ? (jint)BAESound_SetLoopCount((BAESound)(intptr_t)ref, (uint32_t)n) : -1; }
