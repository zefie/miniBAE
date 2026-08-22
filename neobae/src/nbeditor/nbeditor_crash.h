/*
 * Copyright 2021-2026 zefie
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef NBEDITOR_CRASH_H
#define NBEDITOR_CRASH_H

#ifdef __cplusplus
extern "C" {
#endif

/* Install process-wide crash logging (segfault, abort, unhandled SEH).
 * Safe to call once at the start of main(), before SDL_Init. */
void NbEditorInstallCrashHandler(void);

#ifdef __cplusplus
}
#endif

#endif /* NBEDITOR_CRASH_H */
