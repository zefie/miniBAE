/*
 * © 2021–2026 zefie
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "NeoBAEConfigPath.h"

#include "X_Assert.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#endif

static int PV_PathExists(char const *path)
{
    FILE *f;

    if (!path || !path[0])
    {
        return 0;
    }
    f = fopen(path, "rb");
    if (!f)
    {
        return 0;
    }
    fclose(f);
    return 1;
}

static void PV_NormalizeSlashes(char *path)
{
    char *p;

    if (!path)
    {
        return;
    }
    for (p = path; *p; ++p)
    {
        if (*p == '\\')
        {
            *p = '/';
        }
    }
}

static int PV_MkdirOne(char const *path)
{
#if defined(_WIN32)
    if (_mkdir(path) == 0)
    {
        return 0;
    }
    if (errno == EEXIST)
    {
        return 0;
    }
    /* Also treat ERROR_ALREADY_EXISTS via GetLastError after CreateDirectory. */
    if (CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        return 0;
    }
    return -1;
#else
    if (mkdir(path, 0755) == 0 || errno == EEXIST)
    {
        return 0;
    }
    return -1;
#endif
}

static int PV_EnsureParentDirs(char const *filePath)
{
    char dir[1024];
    size_t len;
    size_t i;
    char *slash;

    if (!filePath || !filePath[0])
    {
        return -1;
    }
    len = strlen(filePath);
    if (len >= sizeof(dir))
    {
        return -1;
    }
    memcpy(dir, filePath, len + 1);
    PV_NormalizeSlashes(dir);
    slash = strrchr(dir, '/');
    if (!slash || slash == dir)
    {
        return 0;
    }
    *slash = '\0';

    for (i = 0; dir[i]; ++i)
    {
        if (dir[i] != '/')
        {
            continue;
        }
        if (i == 0)
        {
            continue;
        }
#if defined(_WIN32)
        /* Skip "C:" drive root. */
        if (i == 2 && dir[1] == ':')
        {
            continue;
        }
#endif
        {
            char saved = dir[i];
            dir[i] = '\0';
            if (dir[0] && PV_MkdirOne(dir) != 0)
            {
                dir[i] = saved;
                return -1;
            }
            dir[i] = saved;
        }
    }
    return PV_MkdirOne(dir);
}

static int PV_CopyFile(char const *src, char const *dst)
{
    FILE *in;
    FILE *out;
    char buf[4096];
    size_t n;

    if (PV_EnsureParentDirs(dst) != 0)
    {
        return -1;
    }
    in = fopen(src, "rb");
    if (!in)
    {
        return -1;
    }
    out = fopen(dst, "wb");
    if (!out)
    {
        fclose(in);
        return -1;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
    {
        if (fwrite(buf, 1, n, out) != n)
        {
            fclose(in);
            fclose(out);
            return -1;
        }
    }
    fclose(in);
    fclose(out);
    return 0;
}

static int PV_DeleteFileSoft(char const *path)
{
#if defined(_WIN32)
    if (DeleteFileA(path))
    {
        return 0;
    }
    /* Soft-fail: permission denied / read-only install dir. */
    return -1;
#else
    if (unlink(path) == 0)
    {
        return 0;
    }
    return -1;
#endif
}

int BAE_GetConfigDirectory(char *out, size_t outSize)
{
    if (!out || outSize < 8)
    {
        return -1;
    }
    out[0] = '\0';

#if defined(_WIN32)
    {
        char const *appdata = getenv("APPDATA");
        if (appdata && appdata[0])
        {
            if (snprintf(out, outSize, "%s/NeoBAE", appdata) < 0 || out[0] == '\0')
            {
                return -1;
            }
            PV_NormalizeSlashes(out);
            return 0;
        }
    }
#else
    {
        char const *xdg = getenv("XDG_CONFIG_HOME");
        if (xdg && xdg[0])
        {
            if (snprintf(out, outSize, "%s/neobae", xdg) < 0 || out[0] == '\0')
            {
                return -1;
            }
            return 0;
        }
        {
            char const *home = getenv("HOME");
            if (home && home[0])
            {
                if (snprintf(out, outSize, "%s/.config/neobae", home) < 0 || out[0] == '\0')
                {
                    return -1;
                }
                return 0;
            }
        }
    }
#endif
    return -1;
}

int BAE_EnsureConfigDirectory(void)
{
    char dir[1024];
    char marker[1100];

    if (BAE_GetConfigDirectory(dir, sizeof(dir)) != 0)
    {
        return -1;
    }
    /* Reuse parent-dir helper by appending a dummy file name. */
    if (snprintf(marker, sizeof(marker), "%s/.keep", dir) < 0)
    {
        return -1;
    }
    return PV_EnsureParentDirs(marker);
}

int BAE_GetConfigFilePath(char *out, size_t outSize, char const *relativeName)
{
    char dir[1024];

    if (!out || outSize == 0 || !relativeName || !relativeName[0])
    {
        return -1;
    }
    if (BAE_GetConfigDirectory(dir, sizeof(dir)) != 0)
    {
        return -1;
    }
    if (snprintf(out, outSize, "%s/%s", dir, relativeName) < 0 || out[0] == '\0')
    {
        return -1;
    }
    return 0;
}

int BAE_GetRuntimeFilePath(char *out, size_t outSize, char const *relativeName)
{
    char exe_dir[1024];

    if (!out || outSize == 0 || !relativeName || !relativeName[0])
    {
        return -1;
    }
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
    /* X_Assert.h does not define get_executable_directory for these targets. */
    (void)exe_dir;
    if (snprintf(out, outSize, "%s", relativeName) < 0)
    {
        return -1;
    }
    return 0;
#else
    get_executable_directory(exe_dir, sizeof(exe_dir));
    if (!exe_dir[0])
    {
        if (snprintf(out, outSize, "%s", relativeName) < 0)
        {
            return -1;
        }
        return 0;
    }
    PV_NormalizeSlashes(exe_dir);
    if (snprintf(out, outSize, "%s/%s", exe_dir, relativeName) < 0 || out[0] == '\0')
    {
        return -1;
    }
    return 0;
#endif
}

int BAE_PrepareConfigFile(char const *relativeName, char const *legacyPath)
{
    char config_path[1100];
    char runtime_path[1100];

    if (!relativeName || !relativeName[0])
    {
        return 0;
    }
    if (BAE_GetConfigFilePath(config_path, sizeof(config_path), relativeName) != 0)
    {
        return 0;
    }

    if (PV_PathExists(config_path))
    {
        return 1;
    }

    if (BAE_GetRuntimeFilePath(runtime_path, sizeof(runtime_path), relativeName) == 0 &&
        PV_PathExists(runtime_path))
    {
        if (PV_CopyFile(runtime_path, config_path) == 0)
        {
            (void)PV_DeleteFileSoft(runtime_path);
            return PV_PathExists(config_path) ? 1 : 0;
        }
        return 0;
    }

    if (legacyPath && legacyPath[0] && PV_PathExists(legacyPath))
    {
        if (PV_CopyFile(legacyPath, config_path) == 0)
        {
            return PV_PathExists(config_path) ? 1 : 0;
        }
        return 0;
    }

    return 0;
}
