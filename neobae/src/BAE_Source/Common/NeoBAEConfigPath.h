/*
 * © 2021–2026 zefie
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Shared NeoBAE per-user config directory helpers:
 *   Linux/Unix: $XDG_CONFIG_HOME/neobae or ~/.config/neobae
 *   Windows:    %APPDATA%\NeoBAE
 */
#ifndef NEOBAE_CONFIG_PATH_H
#define NEOBAE_CONFIG_PATH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Config root without trailing slash. Returns 0 on success, -1 on failure. */
int BAE_GetConfigDirectory(char *out, size_t outSize);

/* Create config root (and parents). Returns 0 on success. */
int BAE_EnsureConfigDirectory(void);

/* out = <configRoot>/<relativeName> (relativeName has no directory separators). */
int BAE_GetConfigFilePath(char *out, size_t outSize, char const *relativeName);

/* out = <exeDir>/<relativeName>. */
int BAE_GetRuntimeFilePath(char *out, size_t outSize, char const *relativeName);

/*
 * Ensure the preferred config-root file is ready for use:
 *  1) If config-root file exists → do nothing (leave any runtime copy alone).
 *  2) Else if runtime/exeDir copy exists → copy to config root, then delete
 *     runtime copy (soft-fail if delete denied).
 *  3) Else if legacyPath is non-NULL and exists → copy to config root (do not
 *     delete the legacy home path).
 * Returns 1 if the config-root file exists afterwards, else 0.
 */
int BAE_PrepareConfigFile(char const *relativeName, char const *legacyPath);

#ifdef __cplusplus
}
#endif

#endif /* NEOBAE_CONFIG_PATH_H */
