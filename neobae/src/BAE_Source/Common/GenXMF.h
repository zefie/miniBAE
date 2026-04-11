 /*
 * © 2021–2026 zefie
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * GenXMF.h
 *
 * XMF/MXMF loader entry points.
 */

#pragma once

#include "NeoBAE.h"

#if USE_XMF_SUPPORT == TRUE && _USING_FLUIDSYNTH == TRUE

// Loads the indicated BAESong from an XMF/MXMF container. Extracts the
// embedded Standard MIDI File and, if present, an embedded SF2/DLS bank to
// drive playback via the FluidSynth backend.
BAEResult BAESong_LoadXmfFromMemory(BAESong song,
                                  const void *data,
                                  uint32_t ulen,
                                  BAE_BOOL ignoreBadInstruments);

// A friendly wrapper to load from file path
BAEResult BAESong_LoadXmfFromFile(BAESong song,
                                  BAEPathName filePath,
                                  BAE_BOOL ignoreBadInstruments);

#endif // USE_XMF_SUPPORT && _USING_FLUIDSYNTH
