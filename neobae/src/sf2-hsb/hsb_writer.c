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

/* hsb_writer.c - IREZ/ZREZ resource file serialiser */

#include "hsb_writer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- Big-endian write helpers ---------- */

static void put_u32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

/* Write a big-endian uint32 directly to a FILE */
static void fwrite_u32be(FILE *fp, uint32_t v)
{
    uint8_t buf[4];
    put_u32be(buf, v);
    fwrite(buf, 1, 4, fp);
}

/* ---------- Public API ---------- */

void HSBWriter_Init(HSBWriter *w, int isZsb)
{
    memset(w, 0, sizeof(*w));
    w->isZsb = isZsb;
}

int HSBWriter_Add(HSBWriter *w, const char type[4], uint32_t id,
                  const char *name,
                  const uint8_t *data, uint32_t dataSize)
{
    HSBEntry *e;
    uint8_t  *dataCopy = NULL;

    if (w->count == w->capacity) {
        uint32_t newCap = (w->capacity == 0) ? 64 : w->capacity * 2;
        HSBEntry *newArr = realloc(w->entries, newCap * sizeof(HSBEntry));
        if (!newArr) return -1;
        w->entries  = newArr;
        w->capacity = newCap;
    }

    if (data && dataSize > 0) {
        dataCopy = malloc(dataSize);
        if (!dataCopy) return -1;
        memcpy(dataCopy, data, dataSize);
    }

    e = &w->entries[w->count++];
    memcpy(e->type, type, 4);
    e->id       = id;
    e->name[0]  = '\0';
    if (name && name[0]) {
        strncpy(e->name, name, 255);
        e->name[255] = '\0';
    }
    e->data     = dataCopy;
    e->dataSize = dataSize;
    return 0;
}

/* Resource container layout (all multi-byte fields big-endian):
 *
 *  File header (12 bytes):
 *    4  map_id       'IREZ' or 'ZREZ'
 *    4  version      1 for IREZ, 2 for ZREZ
 *    4  totalEntries
 *
 *  For each entry:
 *    4  nextOffset   absolute byte offset of the next entry (or EOF for last)
 *    4  resourceType FourCC
 *    4  resourceID   big-endian uint32
 *    1+ resourceName Pascal string (1 len byte + N name bytes)
 *    4  resourceLen
 *   N   resourceData
 */
int HSBWriter_WriteFile(const HSBWriter *w, const char *path)
{
    FILE    *fp;
    uint32_t i;

    /* Pre-compute each entry size so we can fill nextOffset correctly.
     * entrySize[i] = 4(next) + 4(type) + 4(id) + pascal_name + 4(len) + data */
    uint32_t *entrySizes = malloc(w->count * sizeof(uint32_t));
    if (!entrySizes) return -1;

    for (i = 0; i < w->count; ++i) {
        size_t nameLen = strlen(w->entries[i].name);
        if (nameLen > 255) nameLen = 255;
        /* nextOffset(4) + type(4) + id(4) + pascalLen(1) + name + dataLen(4) + data */
        entrySizes[i] = (uint32_t)(4 + 4 + 4 + 1 + nameLen + 4 + w->entries[i].dataSize);
    }

    /* Compute absolute offset of each entry from file start */
    uint32_t *offsets = malloc(w->count * sizeof(uint32_t));
    if (!offsets) { free(entrySizes); return -1; }

    uint32_t cursor = 12; /* after 12-byte file header */
    for (i = 0; i < w->count; ++i) {
        offsets[i] = cursor;
        cursor += entrySizes[i];
    }
    uint32_t fileEnd = cursor;

    fp = fopen(path, "wb");
    if (!fp) { free(entrySizes); free(offsets); return -1; }

    /* File header */
    fwrite(w->isZsb ? "ZREZ" : "IREZ", 1, 4, fp);
    fwrite_u32be(fp, w->isZsb ? 2 : 1);
    fwrite_u32be(fp, w->count);

    for (i = 0; i < w->count; ++i) {
        const HSBEntry *e       = &w->entries[i];
        size_t          nameLen = strlen(e->name);
        uint32_t        nextOff = (i + 1 < w->count) ? offsets[i + 1] : fileEnd;

        if (nameLen > 255) nameLen = 255;

        fwrite_u32be(fp, nextOff);
        fwrite(e->type, 1, 4, fp);
        fwrite_u32be(fp, e->id);
        /* Pascal string: 1-byte length + name bytes */
        fputc((int)nameLen, fp);
        if (nameLen > 0) fwrite(e->name, 1, nameLen, fp);
        fwrite_u32be(fp, e->dataSize);
        if (e->data && e->dataSize > 0)
            fwrite(e->data, 1, e->dataSize, fp);
    }

    fclose(fp);
    free(entrySizes);
    free(offsets);
    return 0;
}

void HSBWriter_Free(HSBWriter *w)
{
    uint32_t i;
    if (!w) return;
    for (i = 0; i < w->count; ++i)
        free(w->entries[i].data);
    free(w->entries);
    memset(w, 0, sizeof(*w));
}
