#include "GenRingtone.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RINGTONE_PPQ 96
#define RINGTONE_DEFAULT_OCTAVE 5
#define RINGTONE_DEFAULT_DURATION 4
#define RINGTONE_DEFAULT_BPM 120
#define RINGTONE_DEFAULT_PROGRAM_FLUTE 73

static int g_imyDefaultProgram = RINGTONE_DEFAULT_PROGRAM_FLUTE;

typedef struct ByteBuf
{
    unsigned char *data;
    uint32_t size;
    uint32_t capacity;
} ByteBuf;

typedef struct RingtoneState
{
    int defaultDuration;
    int defaultOctave;
    int bpm;
    int program;
    uint32_t pendingTicks;
    ByteBuf track;
} RingtoneState;

static int PV_AsciiEqualNoCase(char a, char b)
{
    if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
    if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
    return a == b;
}

static int PV_StrnEqNoCase(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i)
    {
        if (a[i] == '\0' || b[i] == '\0')
            return 0;
        if (!PV_AsciiEqualNoCase(a[i], b[i]))
            return 0;
    }
    return 1;
}

static const char *PV_FindNoCase(const char *hay, const char *needle)
{
    size_t nlen;
    size_t i;
    size_t hlen;

    if (!hay || !needle)
        return NULL;

    nlen = strlen(needle);
    hlen = strlen(hay);
    if (nlen == 0 || hlen < nlen)
        return NULL;

    for (i = 0; i + nlen <= hlen; ++i)
    {
        if (PV_StrnEqNoCase(&hay[i], needle, nlen))
            return &hay[i];
    }
    return NULL;
}

static int PV_BufEnsure(ByteBuf *b, uint32_t extra)
{
    uint32_t needed;
    uint32_t newCap;
    unsigned char *newData;

    if (!b)
        return 0;

    needed = b->size + extra;
    if (needed <= b->capacity)
        return 1;

    newCap = b->capacity ? b->capacity : 256;
    while (newCap < needed)
    {
        if (newCap > 0x7FFFFFFFu)
            return 0;
        newCap *= 2;
    }

    newData = (unsigned char *)realloc(b->data, newCap);
    if (!newData)
        return 0;

    b->data = newData;
    b->capacity = newCap;
    return 1;
}

static int PV_BufPutU8(ByteBuf *b, unsigned char v)
{
    if (!PV_BufEnsure(b, 1))
        return 0;
    b->data[b->size++] = v;
    return 1;
}

static int PV_BufPutData(ByteBuf *b, const unsigned char *p, uint32_t len)
{
    if (!PV_BufEnsure(b, len))
        return 0;
    memcpy(b->data + b->size, p, len);
    b->size += len;
    return 1;
}

static int PV_BufPutBE16(ByteBuf *b, uint16_t v)
{
    unsigned char bytes[2];
    bytes[0] = (unsigned char)((v >> 8) & 0xFF);
    bytes[1] = (unsigned char)(v & 0xFF);
    return PV_BufPutData(b, bytes, 2);
}

static int PV_BufPutBE32(ByteBuf *b, uint32_t v)
{
    unsigned char bytes[4];
    bytes[0] = (unsigned char)((v >> 24) & 0xFF);
    bytes[1] = (unsigned char)((v >> 16) & 0xFF);
    bytes[2] = (unsigned char)((v >> 8) & 0xFF);
    bytes[3] = (unsigned char)(v & 0xFF);
    return PV_BufPutData(b, bytes, 4);
}

static int PV_BufPutVLQ(ByteBuf *b, uint32_t value)
{
    unsigned char out[5];
    int count = 0;

    out[count++] = (unsigned char)(value & 0x7F);
    value >>= 7;
    while (value)
    {
        out[count++] = (unsigned char)((value & 0x7F) | 0x80);
        value >>= 7;
    }

    while (count > 0)
    {
        if (!PV_BufPutU8(b, out[--count]))
            return 0;
    }
    return 1;
}

static void PV_BufFree(ByteBuf *b)
{
    if (!b)
        return;
    free(b->data);
    b->data = NULL;
    b->size = 0;
    b->capacity = 0;
}

static uint32_t PV_DurationToTicks(int durationDiv, int dotted)
{
    uint32_t ticks;

    if (durationDiv <= 0)
        durationDiv = RINGTONE_DEFAULT_DURATION;

    ticks = (uint32_t)((4 * RINGTONE_PPQ) / durationDiv);
    if (ticks == 0)
        ticks = 1;

    if (dotted)
        ticks = ticks + (ticks / 2);

    return ticks;
}

static uint32_t PV_IMYDurationToTicks(int durationValue, char durationSpecifier)
{
    uint32_t ticks;

    switch (durationValue)
    {
        case 0: ticks = 4 * RINGTONE_PPQ; break;
        case 1: ticks = 2 * RINGTONE_PPQ; break;
        case 2: ticks = 1 * RINGTONE_PPQ; break;
        case 3: ticks = RINGTONE_PPQ / 2; break;
        case 4: ticks = RINGTONE_PPQ / 4; break;
        case 5: ticks = RINGTONE_PPQ / 8; break;
        default: ticks = 1 * RINGTONE_PPQ; break;
    }

    if (durationSpecifier == '.')
    {
        ticks = ticks + (ticks / 2);
    }
    else if (durationSpecifier == ':')
    {
        ticks = ticks + (ticks / 2) + (ticks / 4);
    }
    else if (durationSpecifier == ';')
    {
        ticks = (ticks * 2) / 3;
    }

    if (ticks == 0)
        ticks = 1;

    return ticks;
}

static int PV_NoteToMidi(char note, int sharp, int octave)
{
    int semitone = -1;

    if (note >= 'A' && note <= 'Z')
        note = (char)(note + ('a' - 'A'));

    switch (note)
    {
        case 'c': semitone = 0; break;
        case 'd': semitone = 2; break;
        case 'e': semitone = 4; break;
        case 'f': semitone = 5; break;
        case 'g': semitone = 7; break;
        case 'a': semitone = 9; break;
        case 'b':
        case 'h': semitone = 11; break;
        default: return -1;
    }

    if (sharp)
        semitone += 1;

    if (octave < 0) octave = RINGTONE_DEFAULT_OCTAVE;
    if (octave > 9) octave = 9;

    return (octave + 1) * 12 + semitone;
}

static int PV_IMYNoteToMidi(char note, int sharp, int flat, int octavePrefix)
{
    int semitone = -1;

    if (note >= 'A' && note <= 'Z')
        note = (char)(note + ('a' - 'A'));

    switch (note)
    {
        case 'c': semitone = 0; break;
        case 'd': semitone = 2; break;
        case 'e': semitone = 4; break;
        case 'f': semitone = 5; break;
        case 'g': semitone = 7; break;
        case 'a': semitone = 9; break;
        case 'b': semitone = 11; break;
        default: return -1;
    }

    if (sharp)
        semitone += 1;
    if (flat)
        semitone -= 1;

    if (octavePrefix < 0)
        octavePrefix = 4;
    if (octavePrefix > 8)
        octavePrefix = 8;

    return (octavePrefix + 2) * 12 + semitone;
}

static int PV_TrackWriteTempoAndProgram(RingtoneState *st)
{
    uint32_t mpqn;
    int program;

    if (st->bpm <= 0)
        st->bpm = RINGTONE_DEFAULT_BPM;

    mpqn = (uint32_t)(60000000UL / (uint32_t)st->bpm);

    if (!PV_BufPutVLQ(&st->track, 0)) return 0;
    if (!PV_BufPutU8(&st->track, 0xFF)) return 0;
    if (!PV_BufPutU8(&st->track, 0x51)) return 0;
    if (!PV_BufPutU8(&st->track, 0x03)) return 0;
    if (!PV_BufPutU8(&st->track, (unsigned char)((mpqn >> 16) & 0xFF))) return 0;
    if (!PV_BufPutU8(&st->track, (unsigned char)((mpqn >> 8) & 0xFF))) return 0;
    if (!PV_BufPutU8(&st->track, (unsigned char)(mpqn & 0xFF))) return 0;

    program = st->program;
    if (program < 0)
        program = 0;
    if (program > 127)
        program = 127;

    if (!PV_BufPutVLQ(&st->track, 0)) return 0;
    if (!PV_BufPutU8(&st->track, 0xC0)) return 0;
    if (!PV_BufPutU8(&st->track, (unsigned char)program)) return 0;

    return 1;
}

static int PV_TrackWriteNoteOrRest(RingtoneState *st, int isRest, int midiNote, uint32_t durTicks)
{
    if (durTicks == 0)
        durTicks = 1;

    if (isRest)
    {
        st->pendingTicks += durTicks;
        return 1;
    }

    if (!PV_BufPutVLQ(&st->track, st->pendingTicks)) return 0;
    if (!PV_BufPutU8(&st->track, 0x90)) return 0;
    if (!PV_BufPutU8(&st->track, (unsigned char)midiNote)) return 0;
    if (!PV_BufPutU8(&st->track, 96)) return 0;

    if (!PV_BufPutVLQ(&st->track, durTicks)) return 0;
    if (!PV_BufPutU8(&st->track, 0x80)) return 0;
    if (!PV_BufPutU8(&st->track, (unsigned char)midiNote)) return 0;
    if (!PV_BufPutU8(&st->track, 0)) return 0;

    st->pendingTicks = 0;
    return 1;
}

static int PV_TrackFinalizeToSmf(RingtoneState *st, unsigned char **ppOut, uint32_t *pOutSize)
{
    ByteBuf smf;
    static const unsigned char mthd[4] = {'M','T','h','d'};
    static const unsigned char mtrk[4] = {'M','T','r','k'};

    memset(&smf, 0, sizeof(smf));

    if (!PV_BufPutVLQ(&st->track, st->pendingTicks))
        goto fail;
    if (!PV_BufPutU8(&st->track, 0xFF))
        goto fail;
    if (!PV_BufPutU8(&st->track, 0x2F))
        goto fail;
    if (!PV_BufPutU8(&st->track, 0x00))
        goto fail;

    if (!PV_BufPutData(&smf, mthd, 4)) goto fail;
    if (!PV_BufPutBE32(&smf, 6)) goto fail;
    if (!PV_BufPutBE16(&smf, 0)) goto fail;
    if (!PV_BufPutBE16(&smf, 1)) goto fail;
    if (!PV_BufPutBE16(&smf, RINGTONE_PPQ)) goto fail;

    if (!PV_BufPutData(&smf, mtrk, 4)) goto fail;
    if (!PV_BufPutBE32(&smf, st->track.size)) goto fail;
    if (!PV_BufPutData(&smf, st->track.data, st->track.size)) goto fail;

    *ppOut = smf.data;
    *pOutSize = smf.size;
    return 1;

fail:
    PV_BufFree(&smf);
    return 0;
}

static void PV_InitState(RingtoneState *st)
{
    memset(st, 0, sizeof(*st));
    st->defaultDuration = RINGTONE_DEFAULT_DURATION;
    st->defaultOctave = RINGTONE_DEFAULT_OCTAVE;
    st->bpm = RINGTONE_DEFAULT_BPM;
    /* Default to configured GM program when iMelody metadata does not provide a vendor instrument. */
    st->program = g_imyDefaultProgram;
}

static void PV_ParseIMYComposerProgram(RingtoneState *st, const char *text)
{
    const char *composer;
    const char *open;
    const char *close;
    const char *p;
    int value;

    if (!st || !text)
        return;

    composer = PV_FindNoCase(text, "COMPOSER:");
    if (!composer)
        return;

    composer += 9;
    while (*composer && isspace((unsigned char)*composer))
        composer++;

    open = strchr(composer, '(');
    if (!open)
        return;

    close = strchr(open + 1, ')');
    if (!close)
        return;

    p = open + 1;
    while (*p && isspace((unsigned char)*p))
        p++;

    if (p >= close || !isdigit((unsigned char)*p))
        return;

    value = atoi(p);
    if (value <= 0)
        return;

    /* Vendor composer instrument numbers are handled as 1-indexed -> MIDI 0-127. */
    value -= 1;
    if (value < 0)
        value = 0;
    if (value > 127)
        value = 127;
    st->program = value;
}

static void PV_SkipSpaces(const char **pp)
{
    while (**pp && isspace((unsigned char)**pp))
        (*pp)++;
}

static int PV_ParseInt(const char **pp)
{
    int v = 0;
    int any = 0;
    while (**pp >= '0' && **pp <= '9')
    {
        v = v * 10 + (**pp - '0');
        any = 1;
        (*pp)++;
    }
    return any ? v : -1;
}

static BAEResult PV_ApplyControl(RingtoneState *st, const char *tok)
{
    const char *eq = strchr(tok, '=');
    int value;

    if (!eq || eq == tok)
        return BAE_NO_ERROR;

    value = atoi(eq + 1);
    if (PV_AsciiEqualNoCase(tok[0], 'd'))
        st->defaultDuration = value;
    else if (PV_AsciiEqualNoCase(tok[0], 'o'))
        st->defaultOctave = value;
    else if (PV_AsciiEqualNoCase(tok[0], 'b'))
        st->bpm = value;

    return BAE_NO_ERROR;
}

static BAEResult PV_ParseRtttlToken(RingtoneState *st, const char *tok)
{
    const char *p = tok;
    int duration = -1;
    int dotted = 0;
    int octave = -1;
    char note;
    int sharp = 0;
    int midi;
    uint32_t ticks;

    PV_SkipSpaces(&p);
    if (!*p)
        return BAE_NO_ERROR;

    if (strchr(p, '='))
        return PV_ApplyControl(st, p);

    if (isdigit((unsigned char)*p))
        duration = PV_ParseInt(&p);

    if (!*p)
        return BAE_NO_ERROR;

    note = *p++;
    if (*p == '#')
    {
        sharp = 1;
        p++;
    }

    if (isdigit((unsigned char)*p))
        octave = PV_ParseInt(&p);

    if (*p == '.')
    {
        dotted = 1;
        p++;
    }
    if (*p == '.')
    {
        dotted = 1;
    }

    if (duration <= 0)
        duration = st->defaultDuration;
    if (octave < 0)
        octave = st->defaultOctave;

    ticks = PV_DurationToTicks(duration, dotted);

    if (note == 'p' || note == 'P' || note == 'r' || note == 'R')
    {
        if (!PV_TrackWriteNoteOrRest(st, 1, 0, ticks))
            return BAE_MEMORY_ERR;
        return BAE_NO_ERROR;
    }

    midi = PV_NoteToMidi(note, sharp, octave);
    if (midi < 0 || midi > 127)
        return BAE_NO_ERROR;

    if (!PV_TrackWriteNoteOrRest(st, 0, midi, ticks))
        return BAE_MEMORY_ERR;

    return BAE_NO_ERROR;
}

static BAEResult PV_ParseRTTTLBuffer(const char *text, unsigned char **ppOut, uint32_t *pOutSize)
{
    const char *firstColon;
    const char *secondColon;
    RingtoneState st;
    char *defaults;
    char *notes;
    char *tmp;
    char *tok;
    BAEResult err;

    if (!text)
        return BAE_PARAM_ERR;

    firstColon = strchr(text, ':');
    if (!firstColon)
        return BAE_BAD_FILE;
    secondColon = strchr(firstColon + 1, ':');
    if (!secondColon)
        return BAE_BAD_FILE;

    PV_InitState(&st);

    defaults = (char *)malloc((size_t)(secondColon - (firstColon + 1)) + 1);
    if (!defaults)
        return BAE_MEMORY_ERR;
    memcpy(defaults, firstColon + 1, (size_t)(secondColon - (firstColon + 1)));
    defaults[secondColon - (firstColon + 1)] = '\0';

    notes = strdup(secondColon + 1);
    if (!notes)
    {
        free(defaults);
        return BAE_MEMORY_ERR;
    }

    tok = strtok(defaults, ",");
    while (tok)
    {
        PV_ApplyControl(&st, tok);
        tok = strtok(NULL, ",");
    }

    if (!PV_TrackWriteTempoAndProgram(&st))
    {
        free(defaults);
        free(notes);
        PV_BufFree(&st.track);
        return BAE_MEMORY_ERR;
    }

    tmp = notes;
    tok = strtok(tmp, ",");
    while (tok)
    {
        err = PV_ParseRtttlToken(&st, tok);
        if (err != BAE_NO_ERROR)
        {
            free(defaults);
            free(notes);
            PV_BufFree(&st.track);
            return err;
        }
        tok = strtok(NULL, ",");
    }

    free(defaults);
    free(notes);

    if (!PV_TrackFinalizeToSmf(&st, ppOut, pOutSize))
    {
        PV_BufFree(&st.track);
        return BAE_MEMORY_ERR;
    }

    PV_BufFree(&st.track);
    return BAE_NO_ERROR;
}

static BAEResult PV_ParseIMYMelody(RingtoneState *st, const char *melody)
{
    const char *p = melody;

    while (*p)
    {
        int duration = 2;
        int octave = -1;
        char note;
        int sharp = 0;
        int flat = 0;
        int midi;
        uint32_t ticks;
        char durationSpecifier = '\0';

        while (*p && (isspace((unsigned char)*p) || *p == ',' || *p == ';' || *p == '(' || *p == ')'))
            p++;
        if (!*p)
            break;

        if (*p == 'V' || *p == 'v')
        {
            p++;
            if (*p == '+' || *p == '-')
                p++;
            else
                (void)PV_ParseInt(&p);
            continue;
        }

        if (PV_StrnEqNoCase(p, "ledon", 5) || PV_StrnEqNoCase(p, "ledoff", 6) ||
            PV_StrnEqNoCase(p, "vibeon", 6) || PV_StrnEqNoCase(p, "vibeoff", 7) ||
            PV_StrnEqNoCase(p, "backon", 6) || PV_StrnEqNoCase(p, "backoff", 7))
        {
            while (*p && isalpha((unsigned char)*p))
                p++;
            continue;
        }

        if (*p == '*')
        {
            p++;
            octave = PV_ParseInt(&p);
        }

        if (*p == '@')
        {
            p++;
            (void)PV_ParseInt(&p);
            continue;
        }

        if (*p == '#')
        {
            sharp = 1;
            p++;
        }
        else if (*p == '&')
        {
            flat = 1;
            p++;
        }

        if (isalpha((unsigned char)*p))
        {
            note = *p++;
        }
        else
        {
            p++;
            continue;
        }

        if (isdigit((unsigned char)*p))
            duration = PV_ParseInt(&p);

        if (*p == '.' || *p == ':' || *p == ';')
        {
            durationSpecifier = *p;
            p++;
        }

        if (octave < 0)
            octave = 4;

        if (note == 'p' || note == 'P' || note == 'r' || note == 'R')
        {
            ticks = PV_IMYDurationToTicks(duration, durationSpecifier);
            if (!PV_TrackWriteNoteOrRest(st, 1, 0, ticks))
                return BAE_MEMORY_ERR;
            continue;
        }

        if ((note != 'a' && note != 'b' && note != 'c' && note != 'd' && note != 'e' && note != 'f' && note != 'g') &&
            (note != 'A' && note != 'B' && note != 'C' && note != 'D' && note != 'E' && note != 'F' && note != 'G'))
        {
            while (*p && isalpha((unsigned char)*p))
                p++;
            continue;
        }

        ticks = PV_IMYDurationToTicks(duration, durationSpecifier);
        midi = PV_IMYNoteToMidi(note, sharp, flat, octave);
        if (midi >= 0 && midi <= 127)
        {
            if (!PV_TrackWriteNoteOrRest(st, 0, midi, ticks))
                return BAE_MEMORY_ERR;
        }
    }

    return BAE_NO_ERROR;
}

static BAEResult PV_ParseIMYBuffer(const char *text, unsigned char **ppOut, uint32_t *pOutSize)
{
    const char *melodyStart;
    const char *end;
    char *melody;
    const char *beatPos;
    RingtoneState st;
    BAEResult err;

    if (!text)
        return BAE_PARAM_ERR;

    PV_InitState(&st);
    st.defaultOctave = 4;
    PV_ParseIMYComposerProgram(&st, text);

    beatPos = PV_FindNoCase(text, "BEAT:");
    if (beatPos)
    {
        beatPos += 5;
        while (*beatPos && isspace((unsigned char)*beatPos)) beatPos++;
        st.bpm = atoi(beatPos);
        if (st.bpm <= 0)
            st.bpm = RINGTONE_DEFAULT_BPM;
    }

    melodyStart = PV_FindNoCase(text, "MELODY:");
    if (!melodyStart)
        return BAE_BAD_FILE;
    melodyStart += 7;

    end = PV_FindNoCase(melodyStart, "END:IMELODY");
    if (!end)
        end = text + strlen(text);

    melody = (char *)malloc((size_t)(end - melodyStart) + 1);
    if (!melody)
        return BAE_MEMORY_ERR;

    memcpy(melody, melodyStart, (size_t)(end - melodyStart));
    melody[end - melodyStart] = '\0';

    if (!PV_TrackWriteTempoAndProgram(&st))
    {
        free(melody);
        return BAE_MEMORY_ERR;
    }

    err = PV_ParseIMYMelody(&st, melody);
    free(melody);
    if (err != BAE_NO_ERROR)
    {
        PV_BufFree(&st.track);
        return err;
    }

    if (!PV_TrackFinalizeToSmf(&st, ppOut, pOutSize))
    {
        PV_BufFree(&st.track);
        return BAE_MEMORY_ERR;
    }

    PV_BufFree(&st.track);
    return BAE_NO_ERROR;
}

static int PV_IsMostlyText(const unsigned char *p, uint32_t len)
{
    uint32_t i;
    uint32_t sample;
    uint32_t bad;

    if (!p || len == 0)
        return 0;

    sample = len > 256 ? 256 : len;
    bad = 0;
    for (i = 0; i < sample; ++i)
    {
        unsigned char c = p[i];
        if ((c < 9) || (c > 13 && c < 32))
            bad++;
    }

    return bad < (sample / 16 + 1);
}

typedef struct BitReader
{
    const unsigned char *data;
    uint32_t sizeBytes;
    uint32_t bitPos;
} BitReader;

static int PV_BitReaderRead(BitReader *br, uint32_t bits, uint32_t *out)
{
    uint32_t value;
    uint32_t i;

    if (!br || !out || bits == 0 || bits > 24)
        return 0;

    if (br->bitPos + bits > br->sizeBytes * 8)
        return 0;

    value = 0;
    for (i = 0; i < bits; ++i)
    {
        uint32_t absBit = br->bitPos + i;
        uint32_t byteIndex = absBit / 8;
        uint32_t bitInByte = 7 - (absBit % 8);
        uint32_t bit = (br->data[byteIndex] >> bitInByte) & 1u;
        value = (value << 1) | bit;
    }

    br->bitPos += bits;
    *out = value;
    return 1;
}

static uint32_t PV_RNGDurationToTicks(uint32_t durationCode, uint32_t durationSpecifier)
{
    uint32_t ticks;

    switch (durationCode)
    {
        case 0: ticks = 4 * RINGTONE_PPQ; break;     /* 1/1 */
        case 1: ticks = 2 * RINGTONE_PPQ; break;     /* 1/2 */
        case 2: ticks = 1 * RINGTONE_PPQ; break;     /* 1/4 */
        case 3: ticks = RINGTONE_PPQ / 2; break;     /* 1/8 */
        case 4: ticks = RINGTONE_PPQ / 4; break;     /* 1/16 */
        case 5: ticks = RINGTONE_PPQ / 8; break;     /* 1/32 */
        case 6: ticks = RINGTONE_PPQ / 16; break;    /* 1/64 */
        default: ticks = 1 * RINGTONE_PPQ; break;    /* Fallback quarter */
    }

    switch (durationSpecifier)
    {
        case 1:
            ticks = ticks + (ticks / 2);             /* dotted */
            break;
        case 2:
            ticks = ticks + (ticks / 2) + (ticks / 4); /* double dotted */
            break;
        case 3:
            ticks = (ticks * 2) / 3;                 /* triplet feel */
            break;
        default:
            break;
    }

    if (ticks == 0)
        ticks = 1;
    return ticks;
}

static int PV_RNGNoteToMidi(uint32_t noteValue, int octave)
{
    static const int semitoneLut[16] = {
        -1, 0, 1, 2, 3, 4, 5, 6,
         7, 8, 9, 10, 11, -1, -1, -1
    };
    int semitone;

    if (noteValue >= 16)
        return -1;

    if (noteValue == 0)
        return -1;

    semitone = semitoneLut[noteValue];
    if (semitone < 0)
        return -1;

    if (octave < 0)
        octave = 4;
    if (octave > 9)
        octave = 9;

    return (octave + 1) * 12 + semitone;
}

static int PV_RNGTempoCodeToBpm(uint32_t tempoCode)
{
    static const int sRngTempoTable[32] = {
         25,  28,  31,  35,  40,  45,  50,  56,
         63,  70,  80,  90, 100, 112, 125, 140,
        160, 180, 200, 225, 250, 285, 320, 355,
        400, 450, 500, 565, 635, 715, 800, 900
    };

    if (tempoCode > 31)
        tempoCode = 31;
    return sRngTempoTable[tempoCode];
}

static int PV_RNGScaleToOctave(uint32_t scale)
{
    switch (scale)
    {
        case 0: return 4;
        case 1: return 5;
        case 2: return 6;
        default: return 5;
    }
}

static BAEResult PV_ParseRNGBinaryBuffer(const unsigned char *data,
                                         uint32_t dataSize,
                                         unsigned char **ppOut,
                                         uint32_t *pOutSize)
{
    BitReader br;
    RingtoneState st;
    uint32_t commandCount;
    uint32_t command;
    uint32_t songType;
    uint32_t titleLen;
    uint32_t sequenceLen;
    uint32_t i;
    int currentOctave;
    int tempoWritten;

    if (!data || dataSize < 4 || !ppOut || !pOutSize)
        return BAE_PARAM_ERR;

    if (!(data[0] == 0x02 && data[1] == 0x4A && data[2] == 0x3A))
        return BAE_BAD_FILE;

    memset(&br, 0, sizeof(br));
    br.data = data;
    br.sizeBytes = dataSize;

    if (!PV_BitReaderRead(&br, 8, &commandCount))
        return BAE_BAD_FILE;

    for (i = 0; i < commandCount; ++i)
    {
        uint32_t filler;
        if (!PV_BitReaderRead(&br, 7, &command))
            return BAE_BAD_FILE;

        /* The first command part has one filler bit to align to byte boundary. */
        if (i == 0)
        {
            if (!PV_BitReaderRead(&br, 1, &filler))
                return BAE_BAD_FILE;
        }
    }
    (void)command;

    PV_InitState(&st);
    st.bpm = 160;
    currentOctave = 5;
    tempoWritten = 0;

    if (!PV_BitReaderRead(&br, 3, &songType))
    {
        PV_BufFree(&st.track);
        return BAE_BAD_FILE;
    }

    if (songType != 1)
    {
        PV_BufFree(&st.track);
        return BAE_UNSUPPORTED_FORMAT;
    }

    if (!PV_BitReaderRead(&br, 4, &titleLen))
    {
        PV_BufFree(&st.track);
        return BAE_BAD_FILE;
    }

    if (titleLen > 0)
    {
        uint32_t skipBits = titleLen * 8;
        uint32_t dummy;
        while (skipBits > 0)
        {
            uint32_t chunk = (skipBits > 24) ? 24 : skipBits;
            if (!PV_BitReaderRead(&br, chunk, &dummy))
            {
                PV_BufFree(&st.track);
                return BAE_BAD_FILE;
            }
            skipBits -= chunk;
        }
    }

    if (!PV_BitReaderRead(&br, 8, &sequenceLen))
    {
        PV_BufFree(&st.track);
        return BAE_BAD_FILE;
    }

    if (!PV_TrackWriteTempoAndProgram(&st))
    {
        PV_BufFree(&st.track);
        return BAE_MEMORY_ERR;
    }
    tempoWritten = 1;

    for (i = 0; i < sequenceLen; ++i)
    {
        uint32_t patternHeader;
        uint32_t patternId;
        uint32_t loopValue;
        uint32_t instructionCount;
        uint32_t instrIndex;

        if (!PV_BitReaderRead(&br, 3, &patternHeader) ||
            !PV_BitReaderRead(&br, 2, &patternId) ||
            !PV_BitReaderRead(&br, 4, &loopValue) ||
            !PV_BitReaderRead(&br, 8, &instructionCount))
        {
            PV_BufFree(&st.track);
            return BAE_BAD_FILE;
        }

        (void)patternId;
        (void)loopValue;
        if (patternHeader != 0)
            break;

        for (instrIndex = 0; instrIndex < instructionCount; ++instrIndex)
        {
            uint32_t instr;
            if (!PV_BitReaderRead(&br, 3, &instr))
            {
                PV_BufFree(&st.track);
                return BAE_BAD_FILE;
            }

            if (instr == 1)
            {
                uint32_t noteValue;
                uint32_t durationCode;
                uint32_t durationSpecifier;
                uint32_t ticks;
                int midi;

                if (!PV_BitReaderRead(&br, 4, &noteValue) ||
                    !PV_BitReaderRead(&br, 3, &durationCode) ||
                    !PV_BitReaderRead(&br, 2, &durationSpecifier))
                {
                    PV_BufFree(&st.track);
                    return BAE_BAD_FILE;
                }

                ticks = PV_RNGDurationToTicks(durationCode, durationSpecifier);
                if (noteValue == 0)
                {
                    if (!PV_TrackWriteNoteOrRest(&st, 1, 0, ticks))
                    {
                        PV_BufFree(&st.track);
                        return BAE_MEMORY_ERR;
                    }
                }
                else
                {
                    midi = PV_RNGNoteToMidi(noteValue, currentOctave);
                    if (midi >= 0 && midi <= 127)
                    {
                        if (!PV_TrackWriteNoteOrRest(&st, 0, midi, ticks))
                        {
                            PV_BufFree(&st.track);
                            return BAE_MEMORY_ERR;
                        }
                    }
                }
            }
            else if (instr == 2)
            {
                uint32_t scale;
                if (!PV_BitReaderRead(&br, 2, &scale))
                {
                    PV_BufFree(&st.track);
                    return BAE_BAD_FILE;
                }
                currentOctave = PV_RNGScaleToOctave(scale);
            }
            else if (instr == 3)
            {
                uint32_t style;
                if (!PV_BitReaderRead(&br, 2, &style))
                {
                    PV_BufFree(&st.track);
                    return BAE_BAD_FILE;
                }
                (void)style;
            }
            else if (instr == 4)
            {
                uint32_t tempoCode;
                if (!PV_BitReaderRead(&br, 5, &tempoCode))
                {
                    PV_BufFree(&st.track);
                    return BAE_BAD_FILE;
                }
                st.bpm = PV_RNGTempoCodeToBpm(tempoCode);
                if (tempoWritten)
                {
                    uint32_t mpqn = (uint32_t)(60000000UL / (uint32_t)st.bpm);
                    if (!PV_BufPutVLQ(&st.track, st.pendingTicks) ||
                        !PV_BufPutU8(&st.track, 0xFF) ||
                        !PV_BufPutU8(&st.track, 0x51) ||
                        !PV_BufPutU8(&st.track, 0x03) ||
                        !PV_BufPutU8(&st.track, (unsigned char)((mpqn >> 16) & 0xFF)) ||
                        !PV_BufPutU8(&st.track, (unsigned char)((mpqn >> 8) & 0xFF)) ||
                        !PV_BufPutU8(&st.track, (unsigned char)(mpqn & 0xFF)))
                    {
                        PV_BufFree(&st.track);
                        return BAE_MEMORY_ERR;
                    }
                    st.pendingTicks = 0;
                }
            }
            else if (instr == 5)
            {
                uint32_t volume;
                if (!PV_BitReaderRead(&br, 4, &volume))
                {
                    PV_BufFree(&st.track);
                    return BAE_BAD_FILE;
                }
                (void)volume;
            }
            else
            {
                PV_BufFree(&st.track);
                return BAE_UNSUPPORTED_FORMAT;
            }
        }
    }

    if (!PV_TrackFinalizeToSmf(&st, ppOut, pOutSize))
    {
        PV_BufFree(&st.track);
        return BAE_MEMORY_ERR;
    }

    PV_BufFree(&st.track);
    return BAE_NO_ERROR;
}

BAEResult BAERingtone_ConvertToMidiFromMemory(void const *pData,
                                              uint32_t dataSize,
                                              BAEFileType fileType,
                                              unsigned char **ppMidiOut,
                                              uint32_t *pMidiSizeOut)
{
    char *text;
    BAEResult err;

    if (!pData || dataSize == 0 || !ppMidiOut || !pMidiSizeOut)
        return BAE_PARAM_ERR;

    *ppMidiOut = NULL;
    *pMidiSizeOut = 0;

    if (fileType == BAE_RINGTONE_IMY)
    {
        text = (char *)malloc(dataSize + 1);
        if (!text)
            return BAE_MEMORY_ERR;
        memcpy(text, pData, dataSize);
        text[dataSize] = '\0';

        err = PV_ParseIMYBuffer(text, ppMidiOut, pMidiSizeOut);
        free(text);
        return err;
    }

    if (fileType == BAE_RINGTONE_RTX)
    {
        text = (char *)malloc(dataSize + 1);
        if (!text)
            return BAE_MEMORY_ERR;
        memcpy(text, pData, dataSize);
        text[dataSize] = '\0';

        err = PV_ParseRTTTLBuffer(text, ppMidiOut, pMidiSizeOut);
        free(text);
        return err;
    }

    if (fileType == BAE_RINGTONE_RNG)
    {
        if (dataSize >= 3 &&
            ((const unsigned char *)pData)[0] == 0x02 &&
            ((const unsigned char *)pData)[1] == 0x4A &&
            ((const unsigned char *)pData)[2] == 0x3A)
        {
            return PV_ParseRNGBinaryBuffer((const unsigned char *)pData,
                                           dataSize,
                                           ppMidiOut,
                                           pMidiSizeOut);
        }

        if (PV_IsMostlyText((const unsigned char *)pData, dataSize))
        {
            text = (char *)malloc(dataSize + 1);
            if (!text)
                return BAE_MEMORY_ERR;
            memcpy(text, pData, dataSize);
            text[dataSize] = '\0';

            err = PV_ParseRTTTLBuffer(text, ppMidiOut, pMidiSizeOut);
            free(text);
            return err;
        }
        return BAE_UNSUPPORTED_FORMAT;
    }

    return BAE_UNSUPPORTED_FORMAT;
}

BAEResult BAERingtone_ConvertToMidiFromFile(BAEPathName filePath,
                                            BAEFileType fileType,
                                            unsigned char **ppMidiOut,
                                            uint32_t *pMidiSizeOut)
{
    FILE *f;
    unsigned char *data;
    long size;
    BAEResult err;

    if (!filePath || !ppMidiOut || !pMidiSizeOut)
        return BAE_PARAM_ERR;

    f = fopen((const char *)filePath, "rb");
    if (!f)
        return BAE_FILE_NOT_FOUND;

    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return BAE_FILE_IO_ERROR;
    }

    size = ftell(f);
    if (size <= 0)
    {
        fclose(f);
        return BAE_BAD_FILE;
    }

    if (fseek(f, 0, SEEK_SET) != 0)
    {
        fclose(f);
        return BAE_FILE_IO_ERROR;
    }

    data = (unsigned char *)malloc((size_t)size);
    if (!data)
    {
        fclose(f);
        return BAE_MEMORY_ERR;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size)
    {
        free(data);
        fclose(f);
        return BAE_FILE_IO_ERROR;
    }

    fclose(f);
    err = BAERingtone_ConvertToMidiFromMemory(data, (uint32_t)size, fileType, ppMidiOut, pMidiSizeOut);
    free(data);
    return err;
}

void BAERingtone_SetIMYDefaultProgram(int program)
{
    if (program < 0)
        program = 0;
    if (program > 127)
        program = 127;
    g_imyDefaultProgram = program;
}

int BAERingtone_GetIMYDefaultProgram(void)
{
    return g_imyDefaultProgram;
}

void BAERingtone_FreeMidiBuffer(unsigned char *pMidiData)
{
    free(pMidiData);
}
