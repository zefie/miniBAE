/*
 * Minimal Win32 jni_md.h shim used when cross-compiling NeoBAE.dll on a
 * Linux host. The host JDK only ships a Linux jni_md.h, so we provide a
 * platform-correct one here and put this directory on the include path
 * BEFORE the host JDK's include/ when targeting Windows.
 *
 * The host JDK's jni.h itself is portable and works unchanged.
 *
 * Copied from the OpenJDK Win32 distribution. License: GPL v2 with the
 * Classpath exception, same as the rest of the JDK.
 */
#ifndef _JAVASOFT_JNI_MD_H_
#define _JAVASOFT_JNI_MD_H_

#define JNIEXPORT __declspec(dllexport)
#define JNIIMPORT __declspec(dllimport)
#define JNICALL   __stdcall

typedef long           jint;
typedef __int64        jlong;
typedef signed char    jbyte;

#endif /* _JAVASOFT_JNI_MD_H_ */
