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

/****************************************************************************
 * instdump.c — Dump INST resources from two RMF files and show differences
 *
 * Usage: instdump <original.rmf> <saved.rmf>
 *
 * For each INST resource ID present in either file, shows a side-by-side
 * comparison of the header fields and extended data, highlighting differences.
 ****************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <X_API.h>
#include <X_Formats.h>
#include <GenSnd.h>

/* Byte offsets into packed InstrumentResource */
enum {
    OFF_sndResourceID   = 0,
    OFF_midiRootKey     = 2,
    OFF_panPlacement    = 4,
    OFF_flags1          = 5,
    OFF_flags2          = 6,
    OFF_smodResourceID  = 7,
    OFF_miscParameter1  = 8,
    OFF_miscParameter2  = 10,
    OFF_keySplitCount   = 12,
    OFF_keySplitData    = 14,
    KEY_SPLIT_SIZE      = 8
};

static const char *fourcc_str(uint32_t v, char buf[5])
{
    buf[0] = (char)((v >> 24) & 0xFF);
    buf[1] = (char)((v >> 16) & 0xFF);
    buf[2] = (char)((v >>  8) & 0xFF);
    buf[3] = (char)( v        & 0xFF);
    buf[4] = 0;
    for (int i = 0; i < 4; i++)
        if (buf[i] < 32 || buf[i] > 126) buf[i] = '?';
    return buf;
}

static const char *adsr_flag_name(uint32_t f)
{
    switch (f & 0x5F5F5F5F) {
        case 0: return "OFF";
        case FOUR_CHAR('L','I','N','E'): return "LINE";
        case FOUR_CHAR('S','U','S','T'): return "SUST";
        case FOUR_CHAR('L','A','S','T'): return "LAST";
        case FOUR_CHAR('G','O','T','O'): return "GOTO";
        case FOUR_CHAR('G','O','S','T'): return "GOST";
        case FOUR_CHAR('R','E','L','S'): return "RELS";
        default: return "????";
    }
}

static const char *flags1_str(unsigned char f, char *buf, size_t sz)
{
    snprintf(buf, sz, "%s%s%s%s%s%s",
             (f & 0x80) ? "Interp " : "",
             (f & 0x20) ? "NoLoop " : "",
             (f & 0x08) ? "UseSR " : "",
             (f & 0x04) ? "S&H " : "",
             (f & 0x02) ? "ExtFmt " : "",
             (f & 0x01) ? "NoRev " : "");
    return buf;
}

static const char *flags2_str(unsigned char f, char *buf, size_t sz)
{
    snprintf(buf, sz, "%s%s%s%s%s",
             (f & 0x40) ? "PlaySampled " : "",
             (f & 0x10) ? "SMOD " : "",
             (f & 0x08) ? "RootKey " : "",
             (f & 0x04) ? "NoPoly " : "",
             (f & 0x02) ? "PitchRand " : "");
    return buf;
}

static void dump_inst_header(const unsigned char *p, int32_t size, const char *label)
{
    char f1[128], f2[128];
    if (size < 14) {
        printf("  [%s] Too small (%d bytes)\n", label, size);
        return;
    }
    printf("  [%s] %d bytes\n", label, size);
    printf("    sndResourceID  : %d\n", (int)XGetShort(p + OFF_sndResourceID));
    printf("    midiRootKey    : %d\n", (int)(int16_t)XGetShort(p + OFF_midiRootKey));
    printf("    panPlacement   : %d\n", (int)(int8_t)p[OFF_panPlacement]);
    printf("    flags1         : 0x%02X  %s\n", p[OFF_flags1], flags1_str(p[OFF_flags1], f1, sizeof(f1)));
    printf("    flags2         : 0x%02X  %s\n", p[OFF_flags2], flags2_str(p[OFF_flags2], f2, sizeof(f2)));
    printf("    smodResourceID : %d\n", (int)(int8_t)p[OFF_smodResourceID]);
    printf("    miscParameter1 : %d\n", (int)(int16_t)XGetShort(p + OFF_miscParameter1));
    printf("    miscParameter2 : %d\n", (int)(int16_t)XGetShort(p + OFF_miscParameter2));

    int16_t splitCount = (int16_t)XGetShort(p + OFF_keySplitCount);
    printf("    keySplitCount  : %d\n", splitCount);

    if (splitCount > 0 && size >= OFF_keySplitData + splitCount * KEY_SPLIT_SIZE) {
        for (int i = 0; i < splitCount; i++) {
            const unsigned char *sp = p + OFF_keySplitData + i * KEY_SPLIT_SIZE;
            printf("    split[%d]: low=%d high=%d snd=%d rootKey=%d vol=%d\n",
                   i, sp[0], sp[1],
                   (int)XGetShort(sp + 2),
                   (int)(int16_t)XGetShort(sp + 4),
                   (int)(int16_t)XGetShort(sp + 6));
        }
    }

    /* Find extended data */
    if (!(p[OFF_flags1] & 0x02)) {
        printf("    [no extended format]\n");
        return;
    }

    const unsigned char *pData = p + OFF_keySplitData + (splitCount > 0 ? splitCount : 0) * KEY_SPLIT_SIZE;
    int32_t remaining = size - (int32_t)(pData - p);
    const unsigned char *pUnit = NULL;
    int32_t count;

    for (count = 0; count < remaining - 1; count++) {
        uint16_t d = (uint16_t)XGetShort(pData + count);
        if (d == 0x8000) {
            count += 4;
            if (count >= remaining) break;
            int32_t s1 = (int32_t)pData[count] + 1;
            if (count + s1 >= remaining) break;
            int32_t s2 = (int32_t)pData[count + s1] + 1;
            if (count + s1 + s2 > remaining) break;
            pUnit = &pData[count + s1 + s2];
            break;
        }
    }

    if (!pUnit || pUnit + 13 > p + size) {
        printf("    [extended format flag set but no valid ext data found]\n");
        return;
    }

    /* Dump the 12 reserved bytes */
    printf("    reserved[12]   :");
    for (int i = 0; i < 12; i++) printf(" %02X", pUnit[i]);
    printf("\n");
    pUnit += 12;

    int unitCount = *pUnit++;
    printf("    unitCount      : %d\n", unitCount);

    const unsigned char *pEnd = p + size;
    int lfoIdx = 0;
    char fc[5];

    for (int u = 0; u < unitCount; u++) {
        if (pUnit + 4 > pEnd) { printf("    [truncated]\n"); break; }
        uint32_t unitType = XGetLong(pUnit) & 0x5F5F5F5F;
        printf("    unit[%d] type: 0x%08X '%s'\n", u, unitType, fourcc_str(unitType, fc));
        pUnit += 4;

        switch (unitType) {
            case FOUR_CHAR('A','D','S','R'): { /* ADSR */
                if (pUnit >= pEnd) goto bail;
                int sc = *pUnit++;
                printf("      ADSR stages: %d\n", sc);
                for (int s = 0; s < sc; s++) {
                    if (pUnit + 12 > pEnd) goto bail;
                    int32_t lvl = (int32_t)XGetLong(pUnit); pUnit += 4;
                    int32_t tm  = (int32_t)XGetLong(pUnit); pUnit += 4;
                    uint32_t fl = XGetLong(pUnit) & 0x5F5F5F5F; pUnit += 4;
                    printf("        [%d] level=%d time=%d flags=0x%08X(%s)\n",
                           s, lvl, tm, fl, adsr_flag_name(fl));
                }
                break;
            }
            case FOUR_CHAR('L','P','G','F'): { /* LPF */
                if (pUnit + 12 > pEnd) goto bail;
                int32_t freq = (int32_t)XGetLong(pUnit); pUnit += 4;
                int32_t res  = (int32_t)XGetLong(pUnit); pUnit += 4;
                int32_t amt  = (int32_t)XGetLong(pUnit); pUnit += 4;
                printf("      LPF freq=%d res=%d amount=%d\n", freq, res, amt);
                break;
            }
            case FOUR_CHAR('D','M','O','D'): { /* DEFAULT_MOD */
                printf("      DEFAULT_MOD (disable auto mod-wheel curve)\n");
                break;
            }
            case FOUR_CHAR('C','U','R','V'): { /* Curve */
                if (pUnit + 9 > pEnd) goto bail;
                uint32_t from = XGetLong(pUnit) & 0x5F5F5F5F; pUnit += 4;
                uint32_t to   = XGetLong(pUnit) & 0x5F5F5F5F; pUnit += 4;
                int cc = *pUnit++;
                printf("      CURVE from='%s'", fourcc_str(from, fc));
                printf(" to='%s' points=%d\n", fourcc_str(to, fc), cc);
                for (int c = 0; c < cc; c++) {
                    if (pUnit + 3 > pEnd) goto bail;
                    uint8_t fv = *pUnit++;
                    int16_t ts = (int16_t)XGetShort(pUnit); pUnit += 2;
                    printf("        [%d] from=%d to=%d\n", c, fv, ts);
                }
                break;
            }
            default: { /* LFO types */
                if (pUnit >= pEnd) goto bail;
                int sc = *pUnit++;
                printf("      LFO[%d] ADSR stages: %d\n", lfoIdx, sc);
                for (int s = 0; s < sc; s++) {
                    if (pUnit + 12 > pEnd) goto bail;
                    int32_t lvl = (int32_t)XGetLong(pUnit); pUnit += 4;
                    int32_t tm  = (int32_t)XGetLong(pUnit); pUnit += 4;
                    uint32_t fl = XGetLong(pUnit) & 0x5F5F5F5F; pUnit += 4;
                    printf("          [%d] level=%d time=%d flags=0x%08X(%s)\n",
                           s, lvl, tm, fl, adsr_flag_name(fl));
                }
                if (pUnit + 16 > pEnd) goto bail;
                int32_t period = (int32_t)XGetLong(pUnit); pUnit += 4;
                uint32_t shape = XGetLong(pUnit); pUnit += 4;
                int32_t dc     = (int32_t)XGetLong(pUnit); pUnit += 4;
                int32_t level  = (int32_t)XGetLong(pUnit); pUnit += 4;
                printf("      LFO[%d] period=%d shape='%s' dc=%d level=%d\n",
                       lfoIdx, period, fourcc_str(shape, fc), dc, level);
                lfoIdx++;
                break;
            }
        }
    }
bail:
    ;
}

static void dump_hex(const unsigned char *p, int32_t size, int32_t maxBytes)
{
    int32_t show = size < maxBytes ? size : maxBytes;
    for (int32_t i = 0; i < show; i++) {
        if (i % 16 == 0) printf("    %04X:", i);
        printf(" %02X", p[i]);
        if (i % 16 == 15 || i == show - 1) printf("\n");
    }
    if (show < size) printf("    ... (%d more bytes)\n", size - show);
}

typedef struct {
    XLongResourceID id;
    XPTR data;
    int32_t size;
} InstEntry;

static int load_insts(const char *path, InstEntry **outEntries, int *outCount)
{
    XFILENAME name;
    XFILE fileRef;
    int count = 0;
    int capacity = 32;
    InstEntry *entries;

    XConvertPathToXFILENAME((void *)path, &name);
    fileRef = XFileOpenResource(&name, TRUE);
    if (!fileRef) {
        fprintf(stderr, "Cannot open: %s\n", path);
        return -1;
    }

    entries = (InstEntry *)malloc(capacity * sizeof(InstEntry));

    for (int32_t idx = 0; ; idx++) {
        XLongResourceID id;
        int32_t size;
        char rawName[256];
        XPTR data = XGetIndexedFileResource(fileRef, ID_INST, &id, idx, rawName, &size);
        if (!data) break;
        if (count >= capacity) {
            capacity *= 2;
            entries = (InstEntry *)realloc(entries, capacity * sizeof(InstEntry));
        }
        entries[count].id = id;
        entries[count].data = data;
        entries[count].size = size;
        count++;
    }

    XFileClose(fileRef);
    *outEntries = entries;
    *outCount = count;
    return 0;
}

static InstEntry *find_inst(InstEntry *entries, int count, XLongResourceID id)
{
    for (int i = 0; i < count; i++) {
        if (entries[i].id == id) return &entries[i];
    }
    return NULL;
}

int main(int argc, char **argv)
{
    InstEntry *entriesA = NULL, *entriesB = NULL;
    int countA = 0, countB = 0;

    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: %s <original.rmf> [saved.rmf]\n", argv[0]);
        fprintf(stderr, "  With one file: dumps all INST resources\n");
        fprintf(stderr, "  With two files: compares INST resources side by side\n");
        return 1;
    }

    if (load_insts(argv[1], &entriesA, &countA) != 0) return 1;

    if (argc == 2) {
        /* Single file: just dump */
        printf("=== %s: %d INST resources ===\n", argv[1], countA);
        for (int i = 0; i < countA; i++) {
            printf("\nINST id=%ld\n", (long)entriesA[i].id);
            dump_inst_header((unsigned char *)entriesA[i].data, entriesA[i].size, argv[1]);
            printf("  Raw hex:\n");
            dump_hex((unsigned char *)entriesA[i].data, entriesA[i].size, 256);
        }
    } else {
        if (load_insts(argv[2], &entriesB, &countB) != 0) return 1;

        printf("=== Comparing: %s (%d INSTs) vs %s (%d INSTs) ===\n",
               argv[1], countA, argv[2], countB);

        /* Dump all IDs from file A, comparing with B */
        for (int i = 0; i < countA; i++) {
            XLongResourceID id = entriesA[i].id;
            InstEntry *b = find_inst(entriesB, countB, id);

            printf("\n========== INST id=%ld ==========\n", (long)id);
            dump_inst_header((unsigned char *)entriesA[i].data, entriesA[i].size, "ORIGINAL");

            if (!b) {
                printf("  [SAVED] *** MISSING ***\n");
            } else {
                dump_inst_header((unsigned char *)b->data, b->size, "SAVED");

                /* Byte-by-byte diff */
                int32_t maxLen = entriesA[i].size > b->size ? entriesA[i].size : b->size;
                int32_t minLen = entriesA[i].size < b->size ? entriesA[i].size : b->size;
                int diffs = 0;
                for (int32_t j = 0; j < minLen; j++) {
                    if (((unsigned char *)entriesA[i].data)[j] != ((unsigned char *)b->data)[j]) {
                        if (diffs < 20) {
                            printf("  DIFF at offset %d: orig=0x%02X saved=0x%02X\n", j,
                                   ((unsigned char *)entriesA[i].data)[j],
                                   ((unsigned char *)b->data)[j]);
                        }
                        diffs++;
                    }
                }
                if (entriesA[i].size != b->size) {
                    printf("  SIZE DIFF: orig=%d saved=%d\n", entriesA[i].size, b->size);
                }
                if (diffs > 20) {
                    printf("  ... and %d more byte differences\n", diffs - 20);
                }
                if (diffs == 0 && entriesA[i].size == b->size) {
                    printf("  IDENTICAL\n");
                } else {
                    printf("  Total byte differences: %d\n", diffs);
                }
            }
        }

        /* Check for IDs in B not in A */
        for (int i = 0; i < countB; i++) {
            if (!find_inst(entriesA, countA, entriesB[i].id)) {
                printf("\n========== INST id=%ld ==========\n", (long)entriesB[i].id);
                printf("  [ORIGINAL] *** MISSING ***\n");
                dump_inst_header((unsigned char *)entriesB[i].data, entriesB[i].size, "SAVED");
            }
        }

        for (int i = 0; i < countB; i++) XDisposePtr(entriesB[i].data);
        free(entriesB);
    }

    for (int i = 0; i < countA; i++) XDisposePtr(entriesA[i].data);
    free(entriesA);
    return 0;
}
