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

/* hsb_writer.h — Build and serialise IREZ/ZREZ (HSB/ZSB) resource files.
 *
 * The resource container used by miniBAE is a minimalist big-endian record
 * store.  We build the entire file in memory, then flush it once.
 */

#ifndef HSB_WRITER_H
#define HSB_WRITER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Resource type tags (4-byte big-endian FourCC stored as byte array) */
#define HSB_TYPE_SND  "snd "
#define HSB_TYPE_INST "INST"
#define HSB_TYPE_BANK "BANK"
#define HSB_TYPE_VERS "VERS"

/* One resource entry */
typedef struct {
    uint8_t  type[4];
    uint32_t id;
    char     name[256];  /* resource name (may be empty) */
    uint8_t *data;       /* owned by the writer; do not free externally */
    uint32_t dataSize;
} HSBEntry;

typedef struct {
    HSBEntry  *entries;
    uint32_t   count;
    uint32_t   capacity;
    int        isZsb;    /* 0 = IREZ header, 1 = ZREZ header */
} HSBWriter;

/* Initialise a writer.  isZsb: 0 for .hsb (IREZ), 1 for .zsb (ZREZ). */
void HSBWriter_Init(HSBWriter *w, int isZsb);

/* Add a resource.  data is deep-copied.
 * Returns 0 on success, -1 on out-of-memory. */
int HSBWriter_Add(HSBWriter *w, const char type[4], uint32_t id,
                  const char *name,
                  const uint8_t *data, uint32_t dataSize);

/* Serialise to file.  Returns 0 on success, -1 on error. */
int HSBWriter_WriteFile(const HSBWriter *w, const char *path);

/* Free all owned memory. */
void HSBWriter_Free(HSBWriter *w);

#ifdef __cplusplus
}
#endif

#endif /* HSB_WRITER_H */
