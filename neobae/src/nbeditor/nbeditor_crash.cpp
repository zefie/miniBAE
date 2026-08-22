/*
 * Copyright 2021-2026 zefie
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Writes nbeditor_crash.log on fatal signals / unhandled exceptions.
 */

#include "nbeditor_crash.h"

#include "NeoBAEConfigPath.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <exception>

#ifndef _VERSION
#define _VERSION "unknown"
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <io.h>
#include <windows.h>
#include <dbghelp.h>
#else
#include <execinfo.h>
#include <unistd.h>
#endif

static char s_log_primary[1024];
static char s_log_fallback[1024];
static volatile sig_atomic_t s_handling = 0;

#if defined(_WIN32)
static LPTOP_LEVEL_EXCEPTION_FILTER s_prev_filter = NULL;
#endif
static std::terminate_handler s_prev_terminate = NULL;

static void CopyPath(char *dst, size_t dstSize, char const *src)
{
    size_t n;

    if (!dst || dstSize == 0)
    {
        return;
    }
    dst[0] = '\0';
    if (!src)
    {
        return;
    }
    n = strlen(src);
    if (n >= dstSize)
    {
        n = dstSize - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int ExtractParentDir(char const *filePath, char *dir, size_t dirSize)
{
    char const *slash;
    char const *bslash;
    char const *end;
    size_t n;

    if (!filePath || !dir || dirSize == 0)
    {
        return 0;
    }
    slash = strrchr(filePath, '/');
    bslash = strrchr(filePath, '\\');
    end = slash;
    if (bslash && (!end || bslash > end))
    {
        end = bslash;
    }
    if (!end || end == filePath)
    {
        CopyPath(dir, dirSize, ".");
        return 1;
    }
    n = (size_t)(end - filePath);
    if (n >= dirSize)
    {
        return 0;
    }
    memcpy(dir, filePath, n);
    dir[n] = '\0';
    return 1;
}

static int ParentDirWritable(char const *filePath)
{
    char dir[1024];

    if (!ExtractParentDir(filePath, dir, sizeof(dir)))
    {
        return 0;
    }
#if defined(_WIN32)
    return _access(dir, 02) == 0;
#else
    return access(dir, W_OK) == 0;
#endif
}

static FILE *OpenCrashLog(void)
{
    FILE *f;

    if (s_log_primary[0])
    {
        f = fopen(s_log_primary, "w");
        if (f)
        {
            return f;
        }
    }
    if (s_log_fallback[0])
    {
        f = fopen(s_log_fallback, "w");
        if (f)
        {
            return f;
        }
    }
    return fopen("nbeditor_crash.log", "w");
}

static void LogPrint(FILE *f, char const *fmt, ...)
{
    va_list ap;

    if (!f || !fmt)
    {
        return;
    }
    va_start(ap, fmt);
    (void)vfprintf(f, fmt, ap);
    va_end(ap);
}

static char const *DebugInfoLabel(void)
{
#if defined(BUILD_DEBUG_INFO) && BUILD_DEBUG_INFO
    return "yes";
#else
    return "no";
#endif
}

static void LogHeader(FILE *f, char const *reason, void const *faultAddr)
{
    time_t now;
    struct tm *tm_now;
    char when[64];

    now = time(NULL);
    tm_now = (now != (time_t)-1) ? localtime(&now) : NULL;
    if (tm_now && strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", tm_now) > 0)
    {
        LogPrint(f, "nbeditor crash log\n");
        LogPrint(f, "Time:    %s\n", when);
    }
    else
    {
        LogPrint(f, "nbeditor crash log\n");
        LogPrint(f, "Time:    (unknown)\n");
    }
    LogPrint(f, "Version: %s\n", _VERSION);
    LogPrint(f, "BUILD_DEBUG_INFO: %s\n", DebugInfoLabel());
    LogPrint(f, "Reason:  %s\n", reason ? reason : "(unknown)");
    if (faultAddr)
    {
        LogPrint(f, "Fault:   %p\n", faultAddr);
    }
    LogPrint(f, "\n");
}

#if defined(_WIN32)

static char const *ExceptionName(DWORD code)
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:
        return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_BREAKPOINT:
        return "EXCEPTION_BREAKPOINT";
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DENORMAL_OPERAND:
        return "EXCEPTION_FLT_DENORMAL_OPERAND";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INEXACT_RESULT:
        return "EXCEPTION_FLT_INEXACT_RESULT";
    case EXCEPTION_FLT_INVALID_OPERATION:
        return "EXCEPTION_FLT_INVALID_OPERATION";
    case EXCEPTION_FLT_OVERFLOW:
        return "EXCEPTION_FLT_OVERFLOW";
    case EXCEPTION_FLT_STACK_CHECK:
        return "EXCEPTION_FLT_STACK_CHECK";
    case EXCEPTION_FLT_UNDERFLOW:
        return "EXCEPTION_FLT_UNDERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:
        return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW:
        return "EXCEPTION_INT_OVERFLOW";
    case EXCEPTION_INVALID_DISPOSITION:
        return "EXCEPTION_INVALID_DISPOSITION";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
    case EXCEPTION_PRIV_INSTRUCTION:
        return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_SINGLE_STEP:
        return "EXCEPTION_SINGLE_STEP";
    case EXCEPTION_STACK_OVERFLOW:
        return "EXCEPTION_STACK_OVERFLOW";
    default:
        return "UNKNOWN_EXCEPTION";
    }
}

static BOOL CALLBACK LogLoadedModule(PCSTR moduleName, DWORD64 moduleBase, ULONG moduleSize, PVOID user)
{
    FILE *f = (FILE *)user;
    LogPrint(f, "  0x%016llx  %s  (%lu bytes)\n",
             (unsigned long long)moduleBase,
             moduleName ? moduleName : "?",
             (unsigned long)moduleSize);
    return TRUE;
}

static int InitStackFrame(STACKFRAME64 *frame, CONTEXT const *ctx, DWORD *machine)
{
    if (!frame || !ctx || !machine)
    {
        return 0;
    }
    memset(frame, 0, sizeof(*frame));
#if defined(_M_X64) || defined(__x86_64__)
    *machine = IMAGE_FILE_MACHINE_AMD64;
    frame->AddrPC.Offset = ctx->Rip;
    frame->AddrPC.Mode = AddrModeFlat;
    frame->AddrFrame.Offset = ctx->Rbp;
    frame->AddrFrame.Mode = AddrModeFlat;
    frame->AddrStack.Offset = ctx->Rsp;
    frame->AddrStack.Mode = AddrModeFlat;
#elif defined(_M_IX86) || defined(__i386__)
    *machine = IMAGE_FILE_MACHINE_I386;
    frame->AddrPC.Offset = ctx->Eip;
    frame->AddrPC.Mode = AddrModeFlat;
    frame->AddrFrame.Offset = ctx->Ebp;
    frame->AddrFrame.Mode = AddrModeFlat;
    frame->AddrStack.Offset = ctx->Esp;
    frame->AddrStack.Mode = AddrModeFlat;
#elif defined(_M_ARM64) || defined(__aarch64__)
    *machine = IMAGE_FILE_MACHINE_ARM64;
    frame->AddrPC.Offset = ctx->Pc;
    frame->AddrPC.Mode = AddrModeFlat;
    frame->AddrFrame.Offset = ctx->Fp;
    frame->AddrFrame.Mode = AddrModeFlat;
    frame->AddrStack.Offset = ctx->Sp;
    frame->AddrStack.Mode = AddrModeFlat;
#else
    (void)ctx;
    *machine = 0;
    return 0;
#endif
    return 1;
}

static void LogWindowsBacktrace(FILE *f, CONTEXT *ctx)
{
    HANDLE process;
    HANDLE thread;
    STACKFRAME64 frame;
    DWORD machine;
    int i;
    char exePath[MAX_PATH];
    char exeDir[MAX_PATH];
    char *slash;

    process = GetCurrentProcess();
    thread = GetCurrentThread();

    exeDir[0] = '\0';
    exePath[0] = '\0';
    if (GetModuleFileNameA(NULL, exePath, MAX_PATH) > 0)
    {
        CopyPath(exeDir, sizeof(exeDir), exePath);
        slash = strrchr(exeDir, '\\');
        if (!slash)
        {
            slash = strrchr(exeDir, '/');
        }
        if (slash)
        {
            *slash = '\0';
        }
    }

    (void)SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    if (!SymInitialize(process, exeDir[0] ? exeDir : NULL, TRUE))
    {
        LogPrint(f, "SymInitialize failed (err %lu); addresses only.\n\n",
                 (unsigned long)GetLastError());
    }

    LogPrint(f, "Modules:\n");
    if (!EnumerateLoadedModules64(process, LogLoadedModule, f))
    {
        LogPrint(f, "  (EnumerateLoadedModules64 failed)\n");
    }
    LogPrint(f, "\nBacktrace:\n");

    if (!InitStackFrame(&frame, ctx, &machine))
    {
        LogPrint(f, "  (unsupported architecture for StackWalk64)\n");
        (void)SymCleanup(process);
        return;
    }

    for (i = 0; i < 64; ++i)
    {
        char symBuf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *sym;
        IMAGEHLP_LINE64 line;
        DWORD displacement = 0;
        DWORD64 disp64 = 0;
        char modulePath[MAX_PATH];
        char const *moduleName;
        MEMORY_BASIC_INFORMATION mbi;

        if (!StackWalk64(machine, process, thread, &frame, ctx, NULL,
                         SymFunctionTableAccess64, SymGetModuleBase64, NULL))
        {
            break;
        }
        if (frame.AddrPC.Offset == 0)
        {
            break;
        }

        moduleName = "?";
        modulePath[0] = '\0';
        if (VirtualQuery((LPCVOID)(uintptr_t)frame.AddrPC.Offset, &mbi, sizeof(mbi)) &&
            mbi.AllocationBase)
        {
            if (GetModuleFileNameA((HMODULE)mbi.AllocationBase, modulePath, MAX_PATH) > 0)
            {
                char const *base = strrchr(modulePath, '\\');
                if (!base)
                {
                    base = strrchr(modulePath, '/');
                }
                moduleName = base ? base + 1 : modulePath;
            }
        }

        memset(symBuf, 0, sizeof(symBuf));
        sym = (SYMBOL_INFO *)symBuf;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;

        memset(&line, 0, sizeof(line));
        line.SizeOfStruct = sizeof(line);

        if (SymFromAddr(process, frame.AddrPC.Offset, &disp64, sym) && sym->Name[0])
        {
            LogPrint(f, "  #%02d 0x%016llx  %s!%s+0x%llx",
                     i,
                     (unsigned long long)frame.AddrPC.Offset,
                     moduleName,
                     sym->Name,
                     (unsigned long long)disp64);
        }
        else
        {
            LogPrint(f, "  #%02d 0x%016llx  %s!0x%016llx",
                     i,
                     (unsigned long long)frame.AddrPC.Offset,
                     moduleName,
                     (unsigned long long)frame.AddrPC.Offset);
        }
        if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &displacement, &line) &&
            line.FileName)
        {
            LogPrint(f, "  [%s:%lu]", line.FileName, (unsigned long)line.LineNumber);
        }
        LogPrint(f, "\n");
    }

    (void)SymCleanup(process);
}

static void WriteWindowsCrash(char const *reason, EXCEPTION_POINTERS *ep, void const *faultAddr)
{
    FILE *f;
    CONTEXT ctx;
    CONTEXT *ctxPtr;
    void const *addr;
    DWORD code;

    f = OpenCrashLog();
    if (!f)
    {
        return;
    }

    addr = faultAddr;
    code = 0;
    ctxPtr = NULL;
    if (ep && ep->ExceptionRecord)
    {
        code = ep->ExceptionRecord->ExceptionCode;
        addr = ep->ExceptionRecord->ExceptionAddress;
        if (code == EXCEPTION_ACCESS_VIOLATION &&
            ep->ExceptionRecord->NumberParameters >= 2)
        {
            addr = (void const *)(uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        }
    }
    if (ep && ep->ContextRecord)
    {
        ctxPtr = ep->ContextRecord;
    }
    else
    {
        memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_FULL;
        RtlCaptureContext(&ctx);
        ctxPtr = &ctx;
    }

    LogHeader(f, reason, addr);
    if (code)
    {
        LogPrint(f, "ExceptionCode: 0x%08lx (%s)\n",
                 (unsigned long)code, ExceptionName(code));
        if (ep && ep->ExceptionRecord &&
            ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
            ep->ExceptionRecord->NumberParameters >= 2)
        {
            ULONG_PTR op = ep->ExceptionRecord->ExceptionInformation[0];
            LogPrint(f, "Access:        %s at %p\n",
                     op == 0 ? "read" : (op == 1 ? "write" : "execute"),
                     (void const *)(uintptr_t)ep->ExceptionRecord->ExceptionInformation[1]);
        }
        LogPrint(f, "\n");
    }
#if defined(_M_X64) || defined(__x86_64__)
    if (ctxPtr)
    {
        LogPrint(f, "RIP=0x%016llx RSP=0x%016llx RBP=0x%016llx\n\n",
                 (unsigned long long)ctxPtr->Rip,
                 (unsigned long long)ctxPtr->Rsp,
                 (unsigned long long)ctxPtr->Rbp);
    }
#elif defined(_M_IX86) || defined(__i386__)
    if (ctxPtr)
    {
        LogPrint(f, "EIP=0x%08lx ESP=0x%08lx EBP=0x%08lx\n\n",
                 (unsigned long)ctxPtr->Eip,
                 (unsigned long)ctxPtr->Esp,
                 (unsigned long)ctxPtr->Ebp);
    }
#elif defined(_M_ARM64) || defined(__aarch64__)
    if (ctxPtr)
    {
        LogPrint(f, "PC=0x%016llx SP=0x%016llx FP=0x%016llx\n\n",
                 (unsigned long long)ctxPtr->Pc,
                 (unsigned long long)ctxPtr->Sp,
                 (unsigned long long)ctxPtr->Fp);
    }
#endif
    if (ctxPtr)
    {
        LogWindowsBacktrace(f, ctxPtr);
    }
    LogPrint(f, "\nEnd of crash log.\n");
    fflush(f);
    fclose(f);
}

static LONG WINAPI NbEditorUnhandledException(EXCEPTION_POINTERS *info)
{
    if (s_handling)
    {
        return s_prev_filter ? s_prev_filter(info) : EXCEPTION_CONTINUE_SEARCH;
    }
    s_handling = 1;
    WriteWindowsCrash("unhandled SEH exception", info, NULL);
    if (s_prev_filter)
    {
        return s_prev_filter(info);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void NbEditorWinSignal(int sig)
{
    char reason[64];

    if (s_handling)
    {
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    s_handling = 1;
    snprintf(reason, sizeof(reason), "signal %d", sig);
    WriteWindowsCrash(reason, NULL, NULL);
    signal(sig, SIG_DFL);
    raise(sig);
}

#endif /* _WIN32 */

#if !defined(_WIN32)

static char s_altstack[262144];

static char const *SignalName(int sig)
{
    switch (sig)
    {
    case SIGSEGV:
        return "SIGSEGV";
    case SIGILL:
        return "SIGILL";
    case SIGFPE:
        return "SIGFPE";
    case SIGABRT:
        return "SIGABRT";
    case SIGBUS:
        return "SIGBUS";
    default:
        return "signal";
    }
}

static void WritePosixCrash(int sig, void const *faultAddr)
{
    FILE *f;
    void *frames[64];
    int n;
    int i;
    char **syms;
    char reason[80];

    f = OpenCrashLog();
    if (!f)
    {
        return;
    }

    snprintf(reason, sizeof(reason), "%s (%d)", SignalName(sig), sig);
    LogHeader(f, reason, faultAddr);

    n = backtrace(frames, 64);
    LogPrint(f, "Backtrace (%d frames):\n", n);
    for (i = 0; i < n; ++i)
    {
        LogPrint(f, "  #%02d %p\n", i, frames[i]);
    }
    LogPrint(f, "\nSymbols:\n");
    syms = backtrace_symbols(frames, n);
    if (syms)
    {
        for (i = 0; i < n; ++i)
        {
            LogPrint(f, "  #%02d %s\n", i, syms[i] ? syms[i] : "?");
        }
        free(syms);
    }
    else
    {
        LogPrint(f, "  (backtrace_symbols failed)\n");
    }
    LogPrint(f, "\nEnd of crash log.\n");
    fflush(f);
    fclose(f);
}

static void NbEditorPosixSignal(int sig, siginfo_t *info, void *uctx)
{
    void const *fault = NULL;

    (void)uctx;

    if (s_handling)
    {
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    s_handling = 1;
    if (info)
    {
        fault = info->si_addr;
    }
    WritePosixCrash(sig, fault);
    signal(sig, SIG_DFL);
    raise(sig);
}

static void InstallPosixSignal(int sig)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = NbEditorPosixSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    (void)sigaction(sig, &sa, NULL);
}

#endif /* !_WIN32 */

static void NbEditorTerminate(void)
{
    if (!s_handling)
    {
        s_handling = 1;
#if defined(_WIN32)
        WriteWindowsCrash("std::terminate", NULL, NULL);
#else
        WritePosixCrash(SIGABRT, NULL);
#endif
    }
    if (s_prev_terminate)
    {
        s_prev_terminate();
    }
    abort();
}

static void ResolveLogPaths(void)
{
    char runtimePath[1024];
    char configPath[1024];

    s_log_primary[0] = '\0';
    s_log_fallback[0] = '\0';
    runtimePath[0] = '\0';
    configPath[0] = '\0';

    if (BAE_GetRuntimeFilePath(runtimePath, sizeof(runtimePath), "nbeditor_crash.log") != 0)
    {
        runtimePath[0] = '\0';
    }
    (void)BAE_EnsureConfigDirectory();
    if (BAE_GetConfigFilePath(configPath, sizeof(configPath), "nbeditor_crash.log") != 0)
    {
        configPath[0] = '\0';
    }

    if (runtimePath[0] && ParentDirWritable(runtimePath))
    {
        CopyPath(s_log_primary, sizeof(s_log_primary), runtimePath);
        CopyPath(s_log_fallback, sizeof(s_log_fallback), configPath);
    }
    else if (configPath[0])
    {
        CopyPath(s_log_primary, sizeof(s_log_primary), configPath);
        CopyPath(s_log_fallback, sizeof(s_log_fallback), runtimePath);
    }
    else if (runtimePath[0])
    {
        CopyPath(s_log_primary, sizeof(s_log_primary), runtimePath);
    }
}

void NbEditorInstallCrashHandler(void)
{
    ResolveLogPaths();

#if defined(_WIN32)
    s_prev_filter = SetUnhandledExceptionFilter(NbEditorUnhandledException);
    (void)signal(SIGABRT, NbEditorWinSignal);
    (void)signal(SIGSEGV, NbEditorWinSignal);
    (void)signal(SIGILL, NbEditorWinSignal);
    (void)signal(SIGFPE, NbEditorWinSignal);
#else
    {
        stack_t ss;
        memset(&ss, 0, sizeof(ss));
        ss.ss_sp = s_altstack;
        ss.ss_size = sizeof(s_altstack);
        ss.ss_flags = 0;
        (void)sigaltstack(&ss, NULL);
    }
    InstallPosixSignal(SIGSEGV);
    InstallPosixSignal(SIGILL);
    InstallPosixSignal(SIGFPE);
    InstallPosixSignal(SIGABRT);
    InstallPosixSignal(SIGBUS);
#endif
    s_prev_terminate = std::set_terminate(NbEditorTerminate);
}
