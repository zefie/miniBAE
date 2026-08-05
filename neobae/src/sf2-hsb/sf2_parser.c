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

/* sf2_parser.c - SoundFont 2 RIFF parser implementation */

#include "sf2_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* ---------- Helpers ---------- */

static void set_error(char *buf, size_t sz, const char *msg)
{
    if (buf && sz > 0)
        snprintf(buf, sz, "%s", msg);
}

/* Read little-endian values from a raw byte pointer (no alignment requirement) */
static uint16_t rd_u16le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t rd_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* FourCC comparison */
static int fcc_eq(const uint8_t *p, const char *tag)
{
    return p[0] == (uint8_t)tag[0] && p[1] == (uint8_t)tag[1]
        && p[2] == (uint8_t)tag[2] && p[3] == (uint8_t)tag[3];
}

/* Copy at most 20 printable chars + NUL into a 21-byte dst buffer (SF2 name fields).
 * Strips NUL bytes, control characters, and trailing whitespace. */
static void copy_name(char *dst, const uint8_t *src)
{
    int i, out = 0;
    for (i = 0; i < 20 && src[i] != '\0'; ++i) {
        unsigned char c = src[i];
        if (c >= 0x20 && c < 0x7F)   /* printable ASCII only */
            dst[out++] = (char)c;
    }
    /* Trim trailing spaces */
    while (out > 0 && dst[out - 1] == ' ')
        out--;
    dst[out] = '\0';
}

/* ---------- RIFF traversal ---------- */

/* Find the first LIST chunk with the given form-type inside [base, base+size).
 * Returns the offset of the first sub-chunk inside that LIST, or 0 if not found.
 * *outRemaining is set to the number of bytes available from that point. */
static size_t find_list(const uint8_t *data, size_t base, size_t size,
                        const char *formType, size_t *outRemaining)
{
    size_t pos = base;
    while (pos + 12 <= base + size) {
        uint32_t chunkSize = rd_u32le(data + pos + 4);
        if (fcc_eq(data + pos, "LIST") && fcc_eq(data + pos + 8, formType)) {
            *outRemaining = chunkSize - 4;
            return pos + 12;
        }
        pos += 8 + chunkSize;
        if (chunkSize & 1) pos++; /* RIFF pad byte */
    }
    return 0;
}

/* Iterate sub-chunks of a LIST block.  Call with *pos = start of first sub-chunk.
 * Returns 1 while advancing *pos; returns 0 when block is exhausted. */
typedef struct {
    const uint8_t *data;
    size_t         blockStart;
    size_t         blockEnd;
    size_t         pos;
    /* Output fields filled on each successful call to SF2Iter_Next() */
    const char    *id;      /* 4-byte tag (pointer into data, NOT NUL-terminated) */
    const uint8_t *payload;
    uint32_t       payloadSize;
} SF2Iter;

static void SF2Iter_Init(SF2Iter *it, const uint8_t *data, size_t start, size_t remaining)
{
    it->data       = data;
    it->blockStart = start;
    it->blockEnd   = start + remaining;
    it->pos        = start;
    it->id         = NULL;
    it->payload    = NULL;
    it->payloadSize= 0;
}

static int SF2Iter_Next(SF2Iter *it)
{
    if (it->pos + 8 > it->blockEnd)
        return 0;
    it->id          = (const char *)(it->data + it->pos);
    it->payloadSize = rd_u32le(it->data + it->pos + 4);
    it->payload     = it->data + it->pos + 8;
    if (it->pos + 8 + it->payloadSize > it->blockEnd)
        return 0;
    it->pos += 8 + it->payloadSize;
    if (it->payloadSize & 1) it->pos++; /* RIFF pad */
    return 1;
}

/* ---------- pdta sub-chunk parsers ---------- */

static int parse_shdr(SF2Bank *b, const uint8_t *data, uint32_t size,
                      char *err, size_t errsz)
{
    uint32_t count = size / 46;
    uint32_t keep  = 0;
    uint32_t i;

    if (count == 0) { set_error(err, errsz, "SHDR chunk is empty"); return -1; }

    b->samples = malloc(count * sizeof(SF2SampleHdr));
    if (!b->samples) { set_error(err, errsz, "Out of memory (SHDR)"); return -1; }

    for (i = 0; i < count; ++i) {
        const uint8_t *p = data + i * 46;
        SF2SampleHdr *s  = &b->samples[keep];

        copy_name(s->name, p);
        if (strcmp(s->name, "EOS") == 0) continue; /* terminal record */

        s->start          = rd_u32le(p + 20);
        s->end            = rd_u32le(p + 24);
        s->loopStart      = rd_u32le(p + 28);
        s->loopEnd        = rd_u32le(p + 32);
        s->sampleRate     = rd_u32le(p + 36);
        s->originalPitch  = p[40];
        s->pitchCorrection= (int8_t)p[41];
        s->sampleLink     = rd_u16le(p + 42);
        s->sampleType     = rd_u16le(p + 44);
        ++keep;
    }
    b->sampleCount = keep;
    return 0;
}

static int parse_phdr(SF2Bank *b, const uint8_t *data, uint32_t size,
                      char *err, size_t errsz)
{
    uint32_t count = size / 38;
    uint32_t keep  = 0;
    uint32_t i;

    b->presets = malloc(count * sizeof(SF2PresetHdr));
    if (!b->presets) { set_error(err, errsz, "Out of memory (PHDR)"); return -1; }

    for (i = 0; i < count; ++i) {
        const uint8_t *p = data + i * 38;
        SF2PresetHdr  *h = &b->presets[keep];

        copy_name(h->name, p);
        if (strcmp(h->name, "EOP") == 0) continue;

        h->preset   = rd_u16le(p + 20);
        h->bank     = rd_u16le(p + 22);
        h->bagIndex = rd_u16le(p + 24);
        /* skip library, genre, morphology */
        ++keep;
    }
    b->presetCount = keep;
    return 0;
}

static int parse_inst(SF2Bank *b, const uint8_t *data, uint32_t size,
                      char *err, size_t errsz)
{
    uint32_t count = size / 22;
    uint32_t keep  = 0;
    uint32_t i;

    b->instruments = malloc(count * sizeof(SF2InstHdr));
    if (!b->instruments) { set_error(err, errsz, "Out of memory (INST)"); return -1; }

    for (i = 0; i < count; ++i) {
        const uint8_t *p = data + i * 22;
        SF2InstHdr    *h = &b->instruments[keep];

        copy_name(h->name, p);
        if (strcmp(h->name, "EOI") == 0) continue;

        h->bagIndex = rd_u16le(p + 20);
        ++keep;
    }
    b->instCount = keep;
    return 0;
}

static int parse_bags(SF2Bag **dst, uint32_t *dstCount,
                      const uint8_t *data, uint32_t size,
                      char *err, size_t errsz, const char *label)
{
    uint32_t count = size / 4;
    uint32_t i;

    *dst = malloc(count * sizeof(SF2Bag));
    if (!*dst) {
        snprintf(err ? err : (char[]){0}, errsz, "Out of memory (%s)", label);
        return -1;
    }
    *dstCount = count;

    for (i = 0; i < count; ++i) {
        (*dst)[i].genIndex = rd_u16le(data + i * 4);
        (*dst)[i].modIndex = rd_u16le(data + i * 4 + 2);
    }
    return 0;
}

static int parse_gens(SF2Gen **dst, uint32_t *dstCount,
                      const uint8_t *data, uint32_t size,
                      char *err, size_t errsz, const char *label)
{
    uint32_t count = size / 4;
    uint32_t i;

    *dst = malloc(count * sizeof(SF2Gen));
    if (!*dst) {
        snprintf(err ? err : (char[]){0}, errsz, "Out of memory (%s)", label);
        return -1;
    }
    *dstCount = count;

    for (i = 0; i < count; ++i) {
        (*dst)[i].oper   = rd_u16le(data + i * 4);
        (*dst)[i].amount = rd_u16le(data + i * 4 + 2);
    }
    return 0;
}

/* ---------- Public API ---------- */

static int SF2Bank_LoadOwnedData(const uint8_t *sourceData,
                                 size_t fileSize,
                                 SF2Bank *bank,
                                 char *errorBuf,
                                 size_t errorBufSize)
{
    uint8_t *fileData;
    size_t   pdtaStart, pdtaRemaining;
    size_t   sdtaStart, sdtaRemaining;
    SF2Iter  it;
    int      ok = 0;

    memset(bank, 0, sizeof(*bank));

    if (fileSize < 12) {
        set_error(errorBuf, errorBufSize, "File too small to be a valid SF2");
        return -1;
    }

    fileData = malloc(fileSize);
    if (!fileData) {
        set_error(errorBuf, errorBufSize, "Out of memory reading SF2");
        return -1;
    }

    memcpy(fileData, sourceData, fileSize);

    /* Verify RIFF sfbk header */
    if (!fcc_eq(fileData, "RIFF") || !fcc_eq(fileData + 8, "sfbk")) {
        free(fileData);
        set_error(errorBuf, errorBufSize, "Not a SoundFont 2 file (bad RIFF/sfbk header)");
        return -1;
    }

    bank->fileData = fileData;
    bank->fileSize = fileSize;

    /* Find pdta LIST */
    pdtaStart = find_list(fileData, 12, fileSize - 12, "pdta", &pdtaRemaining);
    if (!pdtaStart) {
        set_error(errorBuf, errorBufSize, "No pdta LIST found in SF2");
        goto cleanup;
    }

    /* Parse pdta sub-chunks */
    SF2Iter_Init(&it, fileData, pdtaStart, pdtaRemaining);
    while (SF2Iter_Next(&it)) {
        if (fcc_eq((const uint8_t *)it.id, "phdr")) {
            if (parse_phdr(bank, it.payload, it.payloadSize, errorBuf, errorBufSize) < 0)
                goto cleanup;
        } else if (fcc_eq((const uint8_t *)it.id, "inst")) {
            if (parse_inst(bank, it.payload, it.payloadSize, errorBuf, errorBufSize) < 0)
                goto cleanup;
        } else if (fcc_eq((const uint8_t *)it.id, "ibag")) {
            if (parse_bags(&bank->ibags, &bank->ibagCount, it.payload, it.payloadSize,
                           errorBuf, errorBufSize, "ibag") < 0)
                goto cleanup;
        } else if (fcc_eq((const uint8_t *)it.id, "pbag")) {
            if (parse_bags(&bank->pbags, &bank->pbagCount, it.payload, it.payloadSize,
                           errorBuf, errorBufSize, "pbag") < 0)
                goto cleanup;
        } else if (fcc_eq((const uint8_t *)it.id, "igen")) {
            if (parse_gens(&bank->igens, &bank->igenCount, it.payload, it.payloadSize,
                           errorBuf, errorBufSize, "igen") < 0)
                goto cleanup;
        } else if (fcc_eq((const uint8_t *)it.id, "pgen")) {
            if (parse_gens(&bank->pgens, &bank->pgenCount, it.payload, it.payloadSize,
                           errorBuf, errorBufSize, "pgen") < 0)
                goto cleanup;
        } else if (fcc_eq((const uint8_t *)it.id, "shdr")) {
            if (parse_shdr(bank, it.payload, it.payloadSize, errorBuf, errorBufSize) < 0)
                goto cleanup;
        }
        /* imod / pmod: skip (modulators not mapped) */
    }

    if (!bank->presets) {
        set_error(errorBuf, errorBufSize, "SF2 file has no preset (phdr) data");
        goto cleanup;
    }

    /* Find sdta/smpl */
    sdtaStart = find_list(fileData, 12, fileSize - 12, "sdta", &sdtaRemaining);
    if (sdtaStart) {
        SF2Iter smplIt;
        SF2Iter_Init(&smplIt, fileData, sdtaStart, sdtaRemaining);
        while (SF2Iter_Next(&smplIt)) {
            if (fcc_eq((const uint8_t *)smplIt.id, "smpl")) {
                bank->smplData = smplIt.payload;
                bank->smplSize = smplIt.payloadSize;
                break;
            }
        }
    }

    if (!bank->smplData) {
        set_error(errorBuf, errorBufSize, "SF2 file has no sample data (sdta/smpl)");
        goto cleanup;
    }

    ok = 1;

cleanup:
    if (!ok) {
        /* Free partial allocations but keep fileData so cleanup is safe */
        free(bank->samples);    bank->samples    = NULL;
        free(bank->presets);    bank->presets    = NULL;
        free(bank->instruments);bank->instruments= NULL;
        free(bank->ibags);      bank->ibags      = NULL;
        free(bank->pbags);      bank->pbags      = NULL;
        free(bank->igens);      bank->igens      = NULL;
        free(bank->pgens);      bank->pgens      = NULL;
        free(fileData);         bank->fileData   = NULL;
        return -1;
    }
    return 0;
}

int SF2Bank_Load(const char *path, SF2Bank *bank, char *errorBuf, size_t errorBufSize)
{
    FILE    *fp;
    size_t   fileSize;
    uint8_t *fileData;
    int      result;

    fp = fopen(path, "rb");
    if (!fp) {
        set_error(errorBuf, errorBufSize, "Cannot open input file");
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    fileSize = (size_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (fileSize < 12) {
        fclose(fp);
        set_error(errorBuf, errorBufSize, "File too small to be a valid SF2");
        return -1;
    }

    fileData = malloc(fileSize);
    if (!fileData) {
        fclose(fp);
        set_error(errorBuf, errorBufSize, "Out of memory reading SF2");
        return -1;
    }

    if (fread(fileData, 1, fileSize, fp) != fileSize) {
        fclose(fp);
        free(fileData);
        set_error(errorBuf, errorBufSize, "Read error on SF2 file");
        return -1;
    }
    fclose(fp);

    result = SF2Bank_LoadOwnedData(fileData, fileSize, bank, errorBuf, errorBufSize);
    free(fileData);
    return result;
}

int SF2Bank_LoadMemory(const void *data, size_t dataSize,
                       SF2Bank *bank,
                       char *errorBuf, size_t errorBufSize)
{
    if (!data || !bank) {
        set_error(errorBuf, errorBufSize, "Invalid in-memory SF2 input");
        return -1;
    }

    return SF2Bank_LoadOwnedData((const uint8_t *)data, dataSize, bank, errorBuf, errorBufSize);
}

void SF2Bank_Free(SF2Bank *bank)
{
    if (!bank) return;
    free(bank->samples);
    free(bank->presets);
    free(bank->instruments);
    free(bank->ibags);
    free(bank->pbags);
    free(bank->igens);
    free(bank->pgens);
    free(bank->fileData);
    memset(bank, 0, sizeof(*bank));
}

/* ---------- Zone resolution helper ---------- */

/* Apply a generator value to a zone (accumulate additive semantics).
 * This is a simplified accumulator that handles the most common generators. */
static void apply_gen(SF2Zone *z, uint16_t oper, int16_t val)
{
    switch (oper) {
    case SF2_GEN_KEY_RANGE:
        /* amount bytes: lo in low byte, hi in high byte (unsigned) */
        z->loKey = (uint8_t)(val & 0xFF);
        z->hiKey = (uint8_t)((val >> 8) & 0xFF);
        break;
    case SF2_GEN_VEL_RANGE:
        z->loVel = (uint8_t)(val & 0xFF);
        z->hiVel = (uint8_t)((val >> 8) & 0xFF);
        break;
    case SF2_GEN_OVERRIDING_ROOT_KEY:
        if (val >= 0 && val <= 127) z->overrideRootKey = val;
        break;
    case SF2_GEN_START_ADDRS_OFFSET: z->startAddrsOffset = val; break;
    case SF2_GEN_END_ADDRS_OFFSET:   z->endAddrsOffset   = val; break;
    case SF2_GEN_STARTLOOP_ADDRS_OFFSET: z->startloopAddrsOffset = val; break;
    case SF2_GEN_ENDLOOP_ADDRS_OFFSET: z->endloopAddrsOffset = val; break;
    case SF2_GEN_START_ADDRS_COARSE: z->startAddrsCoarse = val; break;
    case SF2_GEN_END_ADDRS_COARSE:   z->endAddrsCoarse   = val; break;
    case SF2_GEN_STARTLOOP_ADDRS_COARSE: z->startloopAddrsCoarse = val; break;
    case SF2_GEN_ENDLOOP_ADDRS_COARSE: z->endloopAddrsCoarse = val; break;
    case SF2_GEN_COARSE_TUNE:        z->coarseTune      = val; break;
    case SF2_GEN_FINE_TUNE:          z->fineTune        = val; break;
    case SF2_GEN_SAMPLE_MODES:       z->sampleModes      = (int)(uint16_t)val; break;
    case SF2_GEN_INITIAL_ATTENUATION:z->initialAttenuation = val; break;
    case SF2_GEN_REVERB_EFFECTS_SEND:z->reverbEffectsSend = val; break;
    case SF2_GEN_CHORUS_EFFECTS_SEND:z->chorusEffectsSend = val; break;
    case SF2_GEN_PAN:                z->pan             = val; break;
    case SF2_GEN_DELAY_VOL_ENV:      z->volDelayTc      = val; break;
    case SF2_GEN_ATTACK_VOL_ENV:     z->volAttackTc     = val; break;
    case SF2_GEN_HOLD_VOL_ENV:       z->volHoldTc       = val; break;
    case SF2_GEN_DECAY_VOL_ENV:      z->volDecayTc      = val; break;
    case SF2_GEN_SUSTAIN_VOL_ENV:    z->volSustainCb    = val; break;
    case SF2_GEN_RELEASE_VOL_ENV:    z->volReleaseTc    = val; break;
    case SF2_GEN_KEYNUM_TO_VOL_ENV_HOLD:  z->keynumToVolEnvHold  = val; break;
    case SF2_GEN_KEYNUM_TO_VOL_ENV_DECAY: z->keynumToVolEnvDecay = val; break;
    case SF2_GEN_DELAY_MOD_ENV:      z->modDelayTc      = val; break;
    case SF2_GEN_ATTACK_MOD_ENV:     z->modAttackTc     = val; break;
    case SF2_GEN_HOLD_MOD_ENV:       z->modHoldTc       = val; break;
    case SF2_GEN_DECAY_MOD_ENV:      z->modDecayTc      = val; break;
    case SF2_GEN_SUSTAIN_MOD_ENV:    z->modSustainPm    = val; break;
    case SF2_GEN_RELEASE_MOD_ENV:    z->modReleaseTc    = val; break;
    case SF2_GEN_KEYNUM_TO_MOD_ENV_HOLD:  z->keynumToModEnvHold  = val; break;
    case SF2_GEN_KEYNUM_TO_MOD_ENV_DECAY: z->keynumToModEnvDecay = val; break;
    case SF2_GEN_MOD_ENV_TO_PITCH:   z->modEnvToPitchCents = val; break;
    case SF2_GEN_MOD_ENV_TO_FILTER_FC:z->modEnvToFilterCents = val; break;
    case SF2_GEN_DELAY_MOD_LFO:      z->modLfoDelayTc   = val; break;
    case SF2_GEN_FREQ_MOD_LFO:       z->modLfoFreqCh    = val; break;
    case SF2_GEN_MOD_LFO_TO_PITCH:   z->modLfoPitchCents= val; break;
    case SF2_GEN_MOD_LFO_TO_VOLUME:  z->modLfoVolCb     = val; break;
    case SF2_GEN_MOD_LFO_TO_FILTER_FC:z->modLfoFilterCents=val; break;
    case SF2_GEN_DELAY_VIB_LFO:      z->vibLfoDelayTc   = val; break;
    case SF2_GEN_FREQ_VIB_LFO:       z->vibLfoFreqCh    = val; break;
    case SF2_GEN_VIB_LFO_TO_PITCH:   z->vibLfoPitchCents= val; break;
    case SF2_GEN_INITIAL_FILTER_FC:  z->initialFilterFc = val; break;
    case SF2_GEN_INITIAL_FILTER_Q:   z->initialFilterQ  = val; break;
    case SF2_GEN_SCALE_TUNING:       z->scaleTuning     = val; break;
    default: break;
    }
}

static void apply_gen_relative(SF2Zone *z, uint16_t oper, int16_t val)
{
    switch (oper) {
    case SF2_GEN_START_ADDRS_OFFSET: z->startAddrsOffset += val; break;
    case SF2_GEN_END_ADDRS_OFFSET:   z->endAddrsOffset   += val; break;
    case SF2_GEN_STARTLOOP_ADDRS_OFFSET: z->startloopAddrsOffset += val; break;
    case SF2_GEN_ENDLOOP_ADDRS_OFFSET: z->endloopAddrsOffset += val; break;
    case SF2_GEN_START_ADDRS_COARSE: z->startAddrsCoarse += val; break;
    case SF2_GEN_END_ADDRS_COARSE:   z->endAddrsCoarse   += val; break;
    case SF2_GEN_STARTLOOP_ADDRS_COARSE: z->startloopAddrsCoarse += val; break;
    case SF2_GEN_ENDLOOP_ADDRS_COARSE: z->endloopAddrsCoarse += val; break;
    case SF2_GEN_COARSE_TUNE:        z->coarseTune      += val; break;
    case SF2_GEN_FINE_TUNE:          z->fineTune        += val; break;
    case SF2_GEN_INITIAL_ATTENUATION:z->initialAttenuation += val; break;
    case SF2_GEN_REVERB_EFFECTS_SEND:z->reverbEffectsSend  += val; break;
    case SF2_GEN_CHORUS_EFFECTS_SEND:z->chorusEffectsSend  += val; break;
    case SF2_GEN_PAN:                z->pan             += val; break;
    case SF2_GEN_DELAY_VOL_ENV:      z->volDelayTc      += val; break;
    case SF2_GEN_ATTACK_VOL_ENV:     z->volAttackTc     += val; break;
    case SF2_GEN_HOLD_VOL_ENV:       z->volHoldTc       += val; break;
    case SF2_GEN_DECAY_VOL_ENV:      z->volDecayTc      += val; break;
    case SF2_GEN_SUSTAIN_VOL_ENV:    z->volSustainCb    += val; break;
    case SF2_GEN_RELEASE_VOL_ENV:    z->volReleaseTc    += val; break;
    case SF2_GEN_KEYNUM_TO_VOL_ENV_HOLD:  z->keynumToVolEnvHold  += val; break;
    case SF2_GEN_KEYNUM_TO_VOL_ENV_DECAY: z->keynumToVolEnvDecay += val; break;
    case SF2_GEN_DELAY_MOD_ENV:      z->modDelayTc      += val; break;
    case SF2_GEN_ATTACK_MOD_ENV:     z->modAttackTc     += val; break;
    case SF2_GEN_HOLD_MOD_ENV:       z->modHoldTc       += val; break;
    case SF2_GEN_DECAY_MOD_ENV:      z->modDecayTc      += val; break;
    case SF2_GEN_SUSTAIN_MOD_ENV:    z->modSustainPm    += val; break;
    case SF2_GEN_RELEASE_MOD_ENV:    z->modReleaseTc    += val; break;
    case SF2_GEN_KEYNUM_TO_MOD_ENV_HOLD:  z->keynumToModEnvHold  += val; break;
    case SF2_GEN_KEYNUM_TO_MOD_ENV_DECAY: z->keynumToModEnvDecay += val; break;
    case SF2_GEN_MOD_ENV_TO_PITCH:   z->modEnvToPitchCents += val; break;
    case SF2_GEN_MOD_ENV_TO_FILTER_FC:z->modEnvToFilterCents += val; break;
    case SF2_GEN_DELAY_MOD_LFO:      z->modLfoDelayTc   += val; break;
    case SF2_GEN_FREQ_MOD_LFO:       z->modLfoFreqCh    += val; break;
    case SF2_GEN_MOD_LFO_TO_PITCH:   z->modLfoPitchCents+= val; break;
    case SF2_GEN_MOD_LFO_TO_VOLUME:  z->modLfoVolCb     += val; break;
    case SF2_GEN_MOD_LFO_TO_FILTER_FC:z->modLfoFilterCents+= val; break;
    case SF2_GEN_DELAY_VIB_LFO:      z->vibLfoDelayTc   += val; break;
    case SF2_GEN_FREQ_VIB_LFO:       z->vibLfoFreqCh    += val; break;
    case SF2_GEN_VIB_LFO_TO_PITCH:   z->vibLfoPitchCents+= val; break;
    case SF2_GEN_INITIAL_FILTER_FC:  z->initialFilterFc += val; break;
    case SF2_GEN_SCALE_TUNING:       z->scaleTuning     += val; break;
    case SF2_GEN_INITIAL_FILTER_Q:   z->initialFilterQ  += val; break;
    default: break;
    }
}

static void zone_defaults(SF2Zone *z)
{
    memset(z, 0, sizeof(*z));
    z->loKey           = 0;
    z->hiKey           = 127;
    z->loVel           = 0;
    z->hiVel           = 127;
    z->sampleIdx       = -1;
    z->overrideRootKey = -1;
    z->startAddrsOffset = 0;
    z->startAddrsCoarse = 0;
    z->endAddrsOffset = 0;
    z->endAddrsCoarse = 0;
    z->startloopAddrsOffset = 0;
    z->startloopAddrsCoarse = 0;
    z->endloopAddrsOffset = 0;
    z->endloopAddrsCoarse = 0;
    /* SF2 spec default timecents */
    z->volDelayTc   = -12000;
    z->volAttackTc  = -12000;
    z->volHoldTc    = -12000;
    z->volDecayTc   = -12000;
    z->volSustainCb = 0;
    z->volReleaseTc = -12000;
    z->modDelayTc   = -12000;
    z->modAttackTc  = -12000;
    z->modHoldTc    = -12000;
    z->modDecayTc   = -12000;
    z->modSustainPm = 0;
    z->modReleaseTc = -12000;
    z->modLfoDelayTc = -12000;
    z->modLfoFreqCh  = 0;
    z->vibLfoDelayTc = -12000;
    z->vibLfoFreqCh  = 0;
    z->scaleTuning   = 100;
}

int SF2Bank_GetPresetZones(const SF2Bank *bank, uint32_t presetIdx,
                           SF2Zone **outZones, uint32_t *outCount)
{
    const SF2PresetHdr *preset;
    uint32_t pbagStart, pbagEnd;
    uint32_t maxZones;
    SF2Zone *zones;
    uint32_t nZones = 0;
    uint32_t bi;

    if (!bank || presetIdx >= bank->presetCount || !outZones || !outCount)
        return -1;

    preset     = &bank->presets[presetIdx];
    pbagStart  = preset->bagIndex;
    /* pbagEnd: start of next preset's bags */
    pbagEnd    = bank->pbagCount; /* default: end of pbag table */
    if (presetIdx + 1 < bank->presetCount)
        pbagEnd = bank->presets[presetIdx + 1].bagIndex;

    /* Upper bound: each preset bag can reference at most all ibags */
    maxZones = (pbagEnd > pbagStart) ? (pbagEnd - pbagStart) * 16 : 16;
    zones    = malloc(maxZones * sizeof(SF2Zone));
    if (!zones) return -1;

    int hasPresetGlobal = 0;
    uint32_t presetGlobalGenStart = 0;
    uint32_t presetGlobalGenEnd = 0;
    if (pbagStart < pbagEnd && pbagStart < bank->pbagCount) {
        uint32_t gi;
        int foundInst = 0;
        presetGlobalGenStart = bank->pbags[pbagStart].genIndex;
        presetGlobalGenEnd = (pbagStart + 1 < bank->pbagCount) ? bank->pbags[pbagStart + 1].genIndex : bank->pgenCount;
        for (gi = presetGlobalGenStart; gi < presetGlobalGenEnd && gi < bank->pgenCount; ++gi) {
            if (bank->pgens[gi].oper == SF2_GEN_INSTRUMENT) { foundInst = 1; break; }
        }
        if (!foundInst) {
            hasPresetGlobal = 1;
            pbagStart++;
        }
    }

    for (bi = pbagStart; bi < pbagEnd && bi < bank->pbagCount; ++bi) {
        uint32_t pgenStart = bank->pbags[bi].genIndex;
        uint32_t pgenEnd   = (bi + 1 < bank->pbagCount)
                             ? bank->pbags[bi + 1].genIndex
                             : bank->pgenCount;
        int32_t  instrID   = -1;
        uint32_t gi;
        uint8_t pLoKey = 0, pHiKey = 127, pLoVel = 0, pHiVel = 127;

        /* Scan preset generators for an instrument reference */
        for (gi = pgenStart; gi < pgenEnd && gi < bank->pgenCount; ++gi) {
            if (bank->pgens[gi].oper == SF2_GEN_INSTRUMENT) {
                instrID = (int32_t)(uint16_t)bank->pgens[gi].amount;
            } else if (bank->pgens[gi].oper == SF2_GEN_KEY_RANGE) {
                pLoKey = (uint8_t)(bank->pgens[gi].amount & 0xFF);
                pHiKey = (uint8_t)((bank->pgens[gi].amount >> 8) & 0xFF);
            } else if (bank->pgens[gi].oper == SF2_GEN_VEL_RANGE) {
                pLoVel = (uint8_t)(bank->pgens[gi].amount & 0xFF);
                pHiVel = (uint8_t)((bank->pgens[gi].amount >> 8) & 0xFF);
            }
        }
        if (instrID < 0 || (uint32_t)instrID >= bank->instCount) continue;

        {
            const SF2InstHdr *inst     = &bank->instruments[instrID];
            uint32_t          ibagStart = inst->bagIndex;
            uint32_t          ibagEnd   = bank->ibagCount;
            uint32_t          ibIdx;

            if ((uint32_t)instrID + 1 < bank->instCount)
                ibagEnd = bank->instruments[instrID + 1].bagIndex;

            /* Global instrument zone (first bag, no sampleID oper) */
            SF2Zone globalZ;
            zone_defaults(&globalZ);

            /* Check if first bag is a global zone (no SampleId generator) */
            int hasGlobal = 0;
            if (ibagStart < ibagEnd && ibagStart < bank->ibagCount) {
                uint32_t ig0 = bank->ibags[ibagStart].genIndex;
                uint32_t ig1 = (ibagStart + 1 < bank->ibagCount)
                               ? bank->ibags[ibagStart + 1].genIndex
                               : bank->igenCount;
                int foundSamp = 0;
                uint32_t gj;
                for (gj = ig0; gj < ig1 && gj < bank->igenCount; ++gj) {
                    if (bank->igens[gj].oper == SF2_GEN_SAMPLE_ID) { foundSamp = 1; break; }
                }
                if (!foundSamp) {
                    /* Global zone – accumulate generators into globalZ */
                    for (gj = ig0; gj < ig1 && gj < bank->igenCount; ++gj)
                        apply_gen(&globalZ, bank->igens[gj].oper,
                                  (int16_t)bank->igens[gj].amount);
                    hasGlobal = 1;
                    ibagStart++; /* skip global bag for split iteration */
                }
            }

            for (ibIdx = ibagStart; ibIdx < ibagEnd && ibIdx < bank->ibagCount; ++ibIdx) {
                uint32_t ig0 = bank->ibags[ibIdx].genIndex;
                uint32_t ig1 = (ibIdx + 1 < bank->ibagCount)
                               ? bank->ibags[ibIdx + 1].genIndex
                               : bank->igenCount;
                SF2Zone z;
                uint32_t gj;
                int foundSamp = 0;

                /* Start from global defaults, inherit global zone values */
                z = globalZ;   /* struct copy */
                (void)hasGlobal;

                for (gj = ig0; gj < ig1 && gj < bank->igenCount; ++gj) {
                    uint16_t oper = bank->igens[gj].oper;
                    int16_t  val  = (int16_t)bank->igens[gj].amount;
                    if (oper == SF2_GEN_SAMPLE_ID) {
                        z.sampleIdx = (int)(uint16_t)bank->igens[gj].amount;
                        foundSamp   = 1;
                    } else {
                        apply_gen(&z, oper, val);
                    }
                }

                if (!foundSamp) continue; /* skip articulaton-only zones */

                if (z.loKey < pLoKey) z.loKey = pLoKey;
                if (z.hiKey > pHiKey) z.hiKey = pHiKey;
                if (z.loKey > z.hiKey) continue;

                if (z.loVel < pLoVel) z.loVel = pLoVel;
                if (z.hiVel > pHiVel) z.hiVel = pHiVel;
                if (z.loVel > z.hiVel) continue;

                for (gj = pgenStart; gj < pgenEnd && gj < bank->pgenCount; ++gj) {
                    uint16_t oper = bank->pgens[gj].oper;
                    if (oper != SF2_GEN_INSTRUMENT && oper != SF2_GEN_KEY_RANGE && oper != SF2_GEN_VEL_RANGE) {
                        apply_gen_relative(&z, oper, (int16_t)bank->pgens[gj].amount);
                    }
                }
                if (hasPresetGlobal) {
                    for (gj = presetGlobalGenStart; gj < presetGlobalGenEnd && gj < bank->pgenCount; ++gj) {
                        uint16_t oper = bank->pgens[gj].oper;
                        if (oper != SF2_GEN_INSTRUMENT && oper != SF2_GEN_KEY_RANGE && oper != SF2_GEN_VEL_RANGE) {
                            apply_gen_relative(&z, oper, (int16_t)bank->pgens[gj].amount);
                        }
                    }
                }

                if (nZones >= maxZones) {
                    uint32_t newMax = (maxZones < 64u) ? 64u : (maxZones * 2u);
                    SF2Zone *grown = (SF2Zone *)realloc(zones, newMax * sizeof(SF2Zone));
                    if (!grown) {
                        free(zones);
                        return -1;
                    }
                    zones = grown;
                    maxZones = newMax;
                }
                zones[nZones++] = z;
            }
        }
    }

    *outZones = zones;
    *outCount = nZones;
    return (int)nZones;
}
