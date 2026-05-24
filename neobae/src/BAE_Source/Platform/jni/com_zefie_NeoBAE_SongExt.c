// Added for playback position preservation across bank changes
#include "NeoBAE.h"
#include "baescript.h"
#include <jni.h>
#include <android/log.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "miniBAE"

typedef struct ScriptBinding {
    BAESong song;
    BAEScript_Context *ctx;
    struct ScriptBinding *next;
} ScriptBinding;

static ScriptBinding *g_scriptBindings = NULL;
static pthread_mutex_t g_scriptBindingsMutex = PTHREAD_MUTEX_INITIALIZER;

static ScriptBinding *find_script_binding(BAESong song)
{
    ScriptBinding *cur = g_scriptBindings;
    while (cur) {
        if (cur->song == song) {
            return cur;
        }
        cur = cur->next;
    }
    return NULL;
}

static void clear_script_binding(BAESong song)
{
    ScriptBinding *prev = NULL;
    ScriptBinding *cur = g_scriptBindings;

    while (cur) {
        if (cur->song == song) {
            if (cur->ctx) {
                BAEScript_Free(cur->ctx);
                cur->ctx = NULL;
            }
            if (prev) {
                prev->next = cur->next;
            } else {
                g_scriptBindings = cur->next;
            }
            free(cur);
            return;
        }
        prev = cur;
        cur = cur->next;
    }
}

static ScriptBinding *set_script_binding(BAESong song, BAEScript_Context *ctx)
{
    ScriptBinding *binding = find_script_binding(song);
    if (binding) {
        if (binding->ctx) {
            BAEScript_Free(binding->ctx);
        }
        binding->ctx = ctx;
        return binding;
    }

    binding = (ScriptBinding *)calloc(1, sizeof(ScriptBinding));
    if (!binding) {
        if (ctx) {
            BAEScript_Free(ctx);
        }
        return NULL;
    }

    binding->song = song;
    binding->ctx = ctx;
    binding->next = g_scriptBindings;
    g_scriptBindings = binding;
    return binding;
}

JNIEXPORT jint JNICALL Java_com_zefie_NeoBAE_Song__1getSongPositionUS(JNIEnv* env, jclass clazz, jlong songRef){
    (void)env; (void)clazz;
    if(songRef == 0){ return 0; }
    BAESong song = (BAESong)(intptr_t)songRef;
    uint32_t us = 0;
    BAEResult r = BAESong_GetMicrosecondPosition(song, &us);
    if(r != BAE_NO_ERROR){ __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "BAESong_GetMicrosecondPosition err=%d", r); return 0; }
    return (jint)us;
}

JNIEXPORT jint JNICALL Java_com_zefie_NeoBAE_Song__1setSongPositionUS(JNIEnv* env, jclass clazz, jlong songRef, jint us){
    (void)env; (void)clazz;
    if(songRef == 0){ return (jint)BAE_NULL_OBJECT; }
    BAESong song = (BAESong)(intptr_t)songRef;
    BAEResult r = BAESong_SetMicrosecondPosition(song, (uint32_t)us);
    if(r != BAE_NO_ERROR){ __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "BAESong_SetMicrosecondPosition err=%d", r); }
    return (jint)r;
}

// Retrieve total song length (microseconds). Returns 0 if unavailable or error.
JNIEXPORT jint JNICALL Java_com_zefie_NeoBAE_Song__1getSongLengthUS(JNIEnv* env, jclass clazz, jlong songRef){
    (void)env; (void)clazz;
    if(songRef == 0){ return 0; }
    BAESong song = (BAESong)(intptr_t)songRef;
    uint32_t us = 0;
    BAEResult r = BAESong_GetMicrosecondLength(song, &us);
    if(r != BAE_NO_ERROR){ __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "BAESong_GetMicrosecondLength err=%d", r); return 0; }
    return (jint)us;
}

// Pause song playback
JNIEXPORT jint JNICALL Java_com_zefie_NeoBAE_Song__1pauseSong(JNIEnv* env, jclass clazz, jlong songRef){
    (void)env; (void)clazz;
    if(songRef == 0){ return (jint)BAE_NULL_OBJECT; }
    BAESong song = (BAESong)(intptr_t)songRef;
    BAEResult r = BAESong_Pause(song);
    if(r != BAE_NO_ERROR){ __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "BAESong_Pause err=%d", r); }
    return (jint)r;
}

// Resume song playback
JNIEXPORT jint JNICALL Java_com_zefie_NeoBAE_Song__1resumeSong(JNIEnv* env, jclass clazz, jlong songRef){
    (void)env; (void)clazz;
    if(songRef == 0){ return (jint)BAE_NULL_OBJECT; }
    BAESong song = (BAESong)(intptr_t)songRef;
    BAEResult r = BAESong_Resume(song);
    if(r != BAE_NO_ERROR){ __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "BAESong_Resume err=%d", r); }
    return (jint)r;
}

// Check if song is paused
JNIEXPORT jboolean JNICALL Java_com_zefie_NeoBAE_Song__1isSongPaused(JNIEnv* env, jclass clazz, jlong songRef){
    (void)env; (void)clazz;
    if(songRef == 0){ return JNI_FALSE; }
    BAESong song = (BAESong)(intptr_t)songRef;
    BAE_BOOL isPaused = FALSE;
    BAEResult r = BAESong_IsPaused(song, &isPaused);
    if(r != BAE_NO_ERROR){ __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "BAESong_IsPaused err=%d", r); return JNI_FALSE; }
    return (isPaused == TRUE) ? JNI_TRUE : JNI_FALSE;
}

// Check if song is done
JNIEXPORT jboolean JNICALL Java_com_zefie_NeoBAE_Song__1isSongDone(JNIEnv* env, jclass clazz, jlong songRef){
    (void)env; (void)clazz;
    if(songRef == 0){ return JNI_TRUE; }
    BAESong song = (BAESong)(intptr_t)songRef;
    BAE_BOOL isDone = FALSE;
    BAEResult r = BAESong_IsDone(song, &isDone);
    if(r != BAE_NO_ERROR){ __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "BAESong_IsDone err=%d", r); return JNI_TRUE; }
    return (isDone == TRUE) ? JNI_TRUE : JNI_FALSE;
}

// Set song loop count
JNIEXPORT jint JNICALL Java_com_zefie_NeoBAE_Song__1setSongLoops(JNIEnv* env, jclass clazz, jlong songRef, jint numLoops){
    (void)env; (void)clazz;
    if(songRef == 0){ return (jint)BAE_NULL_OBJECT; }
    BAESong song = (BAESong)(intptr_t)songRef;
    BAEResult r = BAESong_SetLoops(song, (int16_t)numLoops);
    if(r != BAE_NO_ERROR){ __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "BAESong_SetLoops err=%d", r); }
    return (jint)r;
}

JNIEXPORT jint JNICALL Java_com_zefie_NeoBAE_Song__1loadScriptFromString(JNIEnv* env, jobject jsong, jlong songRef, jstring source)
{
    (void)jsong;
    if (songRef == 0 || !source) return (jint)BAE_PARAM_ERR;

    BAESong song = (BAESong)(intptr_t)songRef;
    const char *src = (*env)->GetStringUTFChars(env, source, NULL);
    if (!src) return (jint)BAE_MEMORY_ERR;

    BAEScript_Context *ctx = BAEScript_LoadString(src);
    (*env)->ReleaseStringUTFChars(env, source, src);

    if (!ctx) {
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "BAEScript_LoadString failed");
        return (jint)BAE_BAD_FILE;
    }

    BAEScript_SetSong(ctx, song);
    pthread_mutex_lock(&g_scriptBindingsMutex);
    if (!set_script_binding(song, ctx)) {
        pthread_mutex_unlock(&g_scriptBindingsMutex);
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "Failed to store BAEScript context");
        return (jint)BAE_MEMORY_ERR;
    }
    pthread_mutex_unlock(&g_scriptBindingsMutex);

    return (jint)BAE_NO_ERROR;
}

JNIEXPORT void JNICALL Java_com_zefie_NeoBAE_Song__1clearScript(JNIEnv* env, jobject jsong, jlong songRef)
{
    (void)env;
    (void)jsong;
    if (songRef == 0) return;

    // Avoid blocking UI-driven stop paths if script tick currently holds the mutex.
    if (pthread_mutex_trylock(&g_scriptBindingsMutex) != 0) {
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "clearScript skipped: script mutex busy");
        return;
    }
    clear_script_binding((BAESong)(intptr_t)songRef);
    pthread_mutex_unlock(&g_scriptBindingsMutex);
}

JNIEXPORT jint JNICALL Java_com_zefie_NeoBAE_Song__1tickScript(JNIEnv* env, jobject jsong, jlong songRef, jint timestampMs, jint lengthMs, jboolean exporting)
{
    (void)env;
    (void)jsong;
    if (songRef == 0) return (jint)BAE_NULL_OBJECT;

    BAESong song = (BAESong)(intptr_t)songRef;
    pthread_mutex_lock(&g_scriptBindingsMutex);
    ScriptBinding *binding = find_script_binding(song);
    if (!binding || !binding->ctx) {
        pthread_mutex_unlock(&g_scriptBindingsMutex);
        return (jint)BAE_NO_ERROR;
    }

    BAEScript_SetSong(binding->ctx, song);
    BAEScript_SetExporting(binding->ctx, exporting ? 1 : 0);
    BAEScript_Tick(binding->ctx,
                   (uint32_t)((timestampMs < 0) ? 0 : timestampMs),
                   (uint32_t)((lengthMs < 0) ? 0 : lengthMs));
    pthread_mutex_unlock(&g_scriptBindingsMutex);

    return (jint)BAE_NO_ERROR;
}

JNIEXPORT void JNICALL Java_com_zefie_NeoBAE_Song__1resetScriptExporterOptions(JNIEnv* env, jobject jsong, jlong songRef)
{
    (void)env;
    (void)jsong;
    if (songRef == 0) return;

    pthread_mutex_lock(&g_scriptBindingsMutex);
    ScriptBinding *binding = find_script_binding((BAESong)(intptr_t)songRef);
    if (!binding || !binding->ctx) {
        pthread_mutex_unlock(&g_scriptBindingsMutex);
        return;
    }

#if BAESCRIPT_EXPORTER_LOOPCOUNT == TRUE
    BAEScript_ResetExporterOptions(binding->ctx);
#endif
    pthread_mutex_unlock(&g_scriptBindingsMutex);
}

JNIEXPORT jint JNICALL Java_com_zefie_NeoBAE_Song__1getScriptExporterLoopCount(JNIEnv* env, jobject jsong, jlong songRef)
{
    (void)env;
    (void)jsong;
    if (songRef == 0) return -1;

    pthread_mutex_lock(&g_scriptBindingsMutex);
    ScriptBinding *binding = find_script_binding((BAESong)(intptr_t)songRef);
    if (!binding || !binding->ctx) {
        pthread_mutex_unlock(&g_scriptBindingsMutex);
        return -1;
    }

#if BAESCRIPT_EXPORTER_LOOPCOUNT == TRUE
    {
        int loopCount = 0;
        if (BAEScript_GetExporterLoopCount(binding->ctx, &loopCount)) {
            pthread_mutex_unlock(&g_scriptBindingsMutex);
            return (jint)loopCount;
        }
    }
#endif
    pthread_mutex_unlock(&g_scriptBindingsMutex);
    return -1;
}

extern JavaVM* gJavaVM;

// Callback from engine
void myMetaEventCallback(void *threadContext, struct GM_Song *pSong, char markerType, void *pMetaText, int32_t metaTextLength, short currentTrack)
{
    (void)pSong;
    (void)currentTrack;

    if (!gJavaVM) return;

    JNIEnv *env;
    int getEnvStat = (*gJavaVM)->GetEnv(gJavaVM, (void**)&env, JNI_VERSION_1_6);
    int attached = 0;

    if (getEnvStat == JNI_EDETACHED) {
        if ((*gJavaVM)->AttachCurrentThread(gJavaVM, &env, NULL) != 0) {
            return;
        }
        attached = 1;
    } else if (getEnvStat != JNI_OK) {
        return;
    }

    jobject listenerObj = (jobject)threadContext;

    if (listenerObj) {
        jclass cls = (*env)->GetObjectClass(env, listenerObj);
        // void onMetaEvent(int markerType, byte[] data)
        jmethodID mid = (*env)->GetMethodID(env, cls, "onMetaEvent", "(I[B)V");
        if (mid) {
            jbyteArray arr = (*env)->NewByteArray(env, metaTextLength);
            if (arr) {
                (*env)->SetByteArrayRegion(env, arr, 0, metaTextLength, (const jbyte*)pMetaText);
                (*env)->CallVoidMethod(env, listenerObj, mid, (jint)markerType, arr);
                (*env)->DeleteLocalRef(env, arr);
            }
        }
        (*env)->DeleteLocalRef(env, cls);
    }

    if (attached) {
        (*gJavaVM)->DetachCurrentThread(gJavaVM);
    }
}

JNIEXPORT jlong JNICALL Java_com_zefie_NeoBAE_Song__1setMetaEventCallback(JNIEnv* env, jclass clazz, jlong songRef, jobject listener)
{
    (void)clazz;
    if(songRef == 0){ return 0; }
    BAESong song = (BAESong)(intptr_t)songRef;
    
    jobject globalRef = (*env)->NewGlobalRef(env, listener);
    
    BAESong_SetMetaEventCallback(song, (GM_SongMetaCallbackProcPtr)myMetaEventCallback, globalRef);
    
    return (jlong)(intptr_t)globalRef;
}

JNIEXPORT void JNICALL Java_com_zefie_NeoBAE_Song__1cleanupMetaEventCallback(JNIEnv* env, jclass clazz, jlong callbackRef)
{
    (void)clazz;
    if (callbackRef != 0) {
        jobject globalRef = (jobject)(intptr_t)callbackRef;
        (*env)->DeleteGlobalRef(env, globalRef);
    }
}

// Set velocity curve
JNIEXPORT jint JNICALL Java_com_zefie_NeoBAE_Song__1setSongVelocityCurve(JNIEnv* env, jclass clazz, jlong songRef, jint curve){
    (void)env; (void)clazz;
    if(songRef == 0){ return (jint)BAE_NULL_OBJECT; }
    BAESong song = (BAESong)(intptr_t)songRef;
    BAEResult r = BAESong_SetVelocityCurve(song, (int)curve);
    if(r != BAE_NO_ERROR){ __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "BAESong_SetVelocityCurve err=%d", r); }
    return (jint)r;
}

// Check if song is SF2
JNIEXPORT jboolean JNICALL Java_com_zefie_NeoBAE_Song__1isSF2Song(JNIEnv* env, jclass clazz, jlong songRef){
    (void)env; (void)clazz;
    if(songRef == 0){ return JNI_FALSE; }
    BAESong song = (BAESong)(intptr_t)songRef;
    bool isSF2 = BAESong_IsSF2Song(song);
    return (isSF2) ? JNI_TRUE : JNI_FALSE;
}
