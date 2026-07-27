/*
 * © 2021–2026 zefie
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

/*****************************************************************************/
/*
** "GenRMI.h"
**
** RMI (RIFF MIDI) file format parser with DLS support - Header
**
****************************************************************************/

#ifndef GEN_RMI_H
#define GEN_RMI_H

#include "X_API.h"

#if USE_RMI_SUPPORT == TRUE

/**
 * GM_LoadRMIFromMemory
 * 
 * Load an RMI (RIFF MIDI) file from memory, extracting both the MIDI data
 * and any embedded DLS soundbank.
 * 
 * @param buf                  Pointer to RMI file data in memory
 * @param len                  Length of the RMI file data
 * @param outMidiData          Pointer to receive allocated MIDI data buffer
 *                             (caller must free with XDisposePtr)
 * @param outMidiLen           Pointer to receive MIDI data length
 * @param loadDLS              If TRUE, attempt to load embedded DLS data
 * @return OPErr error code (NO_ERR on success)
 */
OPErr GM_LoadRMIFromMemory(const unsigned char *buf, uint32_t len,
                           unsigned char **outMidiData, uint32_t *outMidiLen,
                           bool loadDLS);

/**
 * GM_IsRMIFile
 * 
 * Determine if a memory buffer contains a valid RMI file by checking
 * for the RIFF/RMID signature.
 * 
 * @param buf  Pointer to file data in memory
 * @param len  Length of the file data
 * @return TRUE if the data appears to be an RMI file, FALSE otherwise
 */
bool GM_IsRMIFile(const unsigned char *buf, uint32_t len);

/**
 * GM_LastRMIHadEmbeddedSoundbank
 * 
 * Query if the last loaded RMI file had an embedded soundbank.
 * 
 * @return TRUE if the last RMI had an embedded soundbank, FALSE otherwise
 */
bool GM_LastRMIHadEmbeddedSoundbank(void);

/**
 * GM_ClearRMISoundbankFlag
 * 
 * Clear the embedded soundbank flag.
 */
void GM_ClearRMISoundbankFlag(void);

#endif // USE_RMI_SUPPORT == TRUE

#endif // GEN_RMI_H
