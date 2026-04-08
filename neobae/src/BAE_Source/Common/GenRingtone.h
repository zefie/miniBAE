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
