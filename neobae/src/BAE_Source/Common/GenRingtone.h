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

 #ifndef GENRINGTONE_H
#define GENRINGTONE_H

#include "NeoBAE.h"

#ifdef __cplusplus
extern "C" {
#endif

BAEResult BAERingtone_ConvertToMidiFromMemory(void const *pData,
                                              uint32_t dataSize,
                                              BAEFileType fileType,
                                              unsigned char **ppMidiOut,
                                              uint32_t *pMidiSizeOut);

BAEResult BAERingtone_ConvertToMidiFromFile(BAEPathName filePath,
                                            BAEFileType fileType,
                                            unsigned char **ppMidiOut,
                                            uint32_t *pMidiSizeOut);

void BAERingtone_SetIMYDefaultProgram(int program);

int BAERingtone_GetIMYDefaultProgram(void);

void BAERingtone_FreeMidiBuffer(unsigned char *pMidiData);

#ifdef __cplusplus
}
#endif

#endif
