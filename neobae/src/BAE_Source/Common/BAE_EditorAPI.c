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
**  BAE_EditorAPI.c
**
**  Shared glue / small helpers. Perf: neobae/docs/BAE_EditorAPI_PERF.md
*/
/*****************************************************************************/

#include "BAE_EditorAPI_Internal.h"

void PV_CopyStringBounded(char *dst, uint32_t dstSize, char const *src)
{
    uint32_t i;

    if (!dst || dstSize == 0)
    {
        return;
    }
    if (!src)
    {
        dst[0] = 0;
        return;
    }
    i = 0;
    while (i + 1 < dstSize && src[i] != 0)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}


/* Beatnik: 0xFFFF (-1) means "no sample". ID 0 is a valid SND resource. */
bool PV_IsNoSampleSndID(XShortResourceID sid)
{
    return ((uint16_t)sid == 0xFFFFu) ? TRUE : FALSE;
}


BAEResult PV_GrowBuffer(void **buffer, uint32_t *capacity, uint32_t elementSize, uint32_t minimumCount)
{
    void *nextBuffer;
    uint32_t nextCapacity;
    if (*capacity >= minimumCount)
    {
        return BAE_NO_ERROR;
    }
    nextCapacity = *capacity ? (*capacity * 2) : 4;
    while (nextCapacity < minimumCount)
    {
        nextCapacity *= 2;
    }
    XPI_Memblock *mb = XGetHeaderIfOurs(*buffer);
    if (!mb) {
        nextBuffer = XNewPtr((int32_t)(nextCapacity * elementSize));
        if (!nextBuffer)
        {
            return BAE_MEMORY_ERR;
        }
        if (*buffer && *capacity)
        {
            XBlockMove(*buffer, nextBuffer, (int32_t)(*capacity * elementSize));
            XDisposePtr(*buffer);
        }
    } else {
        nextBuffer = XResizePtr(*buffer, (int32_t)(nextCapacity * elementSize));
        if (!nextBuffer) {
            return BAE_MEMORY_ERR;
        }
    }
    *buffer = nextBuffer;
    *capacity = nextCapacity;
    return BAE_NO_ERROR;
}


BAEResult PV_CreatePascalName(char const *source, char outName[256])
{
    uint32_t length;

    if (!outName)
    {
        return BAE_PARAM_ERR;
    }
    if (!source)
    {
        outName[0] = 0;
        return BAE_NO_ERROR;
    }
    length = (uint32_t)strlen(source);
    if (length > 255)
    {
        length = 255;
    }
    outName[0] = (char)length;
    if (length)
    {
        XBlockMove(source, outName + 1, (int32_t)length);
    }
    return BAE_NO_ERROR;
}


void PV_DecodeResourceName(char const *rawName, char outName[256])
{
    uint8_t len;

    if (!outName)
    {
        return;
    }
    outName[0] = 0;
    if (!rawName)
    {
        return;
    }
    /* Some paths return Pascal strings, others C strings. */
    len = (uint8_t)rawName[0];
    if (len > 0 && len < 64 && rawName[1] >= 32)
    {
        uint32_t copyLen = len;
        if (copyLen > 255)
        {
            copyLen = 255;
        }
        XBlockMove(rawName + 1, outName, (int32_t)copyLen);
        outName[copyLen] = 0;
        return;
    }
    XStrCpy(outName, rawName);
}

