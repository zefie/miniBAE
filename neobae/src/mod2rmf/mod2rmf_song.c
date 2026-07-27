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

#include "mod2rmf_common.h"

int mod2rmf_channel_profile_add_range(ChannelProfile *p, uint32_t start, uint32_t end)
{
    if (p->rangeCount >= p->rangeCapacity)
    {
        uint32_t newCap = (p->rangeCapacity == 0) ? 64 : p->rangeCapacity * 2;
        TickRange *tmp = (TickRange *)realloc(p->activeRanges, newCap * sizeof(TickRange));
        if (!tmp) return 0;
        p->activeRanges = tmp;
        p->rangeCapacity = newCap;
    }
    p->activeRanges[p->rangeCount].startTick = start;
    p->activeRanges[p->rangeCount].endTick = end;
    p->rangeCount++;
    return 1;
}

void mod2rmf_channel_profile_add_program(ChannelProfile *p, uint8_t program)
{
    uint32_t i;
    for (i = 0; i < p->programCount; ++i)
    {
        if (p->programs[i] == program) return;
    }
    if (p->programCount < CHANNEL_PROFILE_MAX_PROGRAMS)
    {
        p->programs[p->programCount++] = program;
    }
}

int mod2rmf_song_model_append_cc_event(ModSongModel *song,
                                      uint16_t sourceChannel,
                                      uint32_t tick,
                                      unsigned char cc,
                                      unsigned char value,
                                      unsigned char program)
{
    ModCCEvent *newEvents;
    uint32_t newCapacity;

    if (!song)
    {
        return 0;
    }

    if (song->ccCount > 0)
    {
        ModCCEvent *last;
        last = &song->ccEvents[song->ccCount - 1];
        if (last->sourceChannel == sourceChannel && last->cc == cc && last->value == value)
        {
            return 1;
        }
    }

    if (song->ccCount >= song->ccCapacity)
    {
        newCapacity = (song->ccCapacity == 0) ? 1024U : (song->ccCapacity * 2U);
        newEvents = (ModCCEvent *)realloc(song->ccEvents, newCapacity * sizeof(ModCCEvent));
        if (!newEvents)
        {
            return 0;
        }
        song->ccEvents = newEvents;
        song->ccCapacity = newCapacity;
    }

    song->ccEvents[song->ccCount].sourceChannel = sourceChannel;
    song->ccEvents[song->ccCount].tick = tick;
    song->ccEvents[song->ccCount].cc = cc;
    song->ccEvents[song->ccCount].value = value;
    song->ccEvents[song->ccCount].program = program;
    song->ccCount++;
    return 1;
}

int mod2rmf_song_model_append_pitch_bend(ModSongModel *song,
                                        uint16_t sourceChannel,
                                        uint32_t tick,
                                        uint16_t value,
                                        unsigned char program)
{
    ModPitchBendEvent *newEvents;
    uint32_t newCapacity;

    if (!song)
    {
        return 0;
    }

    if (song->pitchBendCount >= song->pitchBendCapacity)
    {
        newCapacity = (song->pitchBendCapacity == 0) ? 1024U : (song->pitchBendCapacity * 2U);
        newEvents = (ModPitchBendEvent *)realloc(song->pitchBendEvents, newCapacity * sizeof(ModPitchBendEvent));
        if (!newEvents)
        {
            return 0;
        }
        song->pitchBendEvents = newEvents;
        song->pitchBendCapacity = newCapacity;
    }

    song->pitchBendEvents[song->pitchBendCount].sourceChannel = sourceChannel;
    song->pitchBendEvents[song->pitchBendCount].tick = tick;
    song->pitchBendEvents[song->pitchBendCount].value = value;
    song->pitchBendEvents[song->pitchBendCount].program = program;
    song->pitchBendCount++;
    return 1;
}

int mod2rmf_song_model_append_tempo_change(ModSongModel *song,
                                          uint32_t tick,
                                          uint32_t bpm)
{
    ModTempoChange *newChanges;
    uint32_t newCapacity;

    if (!song)
    {
        return 0;
    }

    if (song->tempoChangeCount >= song->tempoChangeCapacity)
    {
        newCapacity = (song->tempoChangeCapacity == 0) ? 64U : (song->tempoChangeCapacity * 2U);
        newChanges = (ModTempoChange *)realloc(song->tempoChanges, newCapacity * sizeof(ModTempoChange));
        if (!newChanges)
        {
            return 0;
        }
        song->tempoChanges = newChanges;
        song->tempoChangeCapacity = newCapacity;
    }

    song->tempoChanges[song->tempoChangeCount].tick = tick;
    song->tempoChanges[song->tempoChangeCount].bpm = bpm;
    song->tempoChangeCount++;
    return 1;
}

void mod2rmf_analyze_channel_usage(const ModSongModel *song,
                                  ChannelProfile profiles[],
                                  uint32_t maxChannels)
{
    uint32_t i;

    memset(profiles, 0, maxChannels * sizeof(ChannelProfile));

    /* Scan notes to build active ranges and program sets */
    for (i = 0; i < song->noteCount; ++i)
    {
        const ModNoteEvent *n = &song->notes[i];
        uint16_t ch = n->sourceChannel;
        if (ch >= maxChannels) continue;

        profiles[ch].used = TRUE;
        profiles[ch].noteCount++;
        mod2rmf_channel_profile_add_program(&profiles[ch], n->program);
        mod2rmf_channel_profile_add_range(&profiles[ch], n->startTick,
                                  n->startTick + n->durationTicks);
    }
}

 /* --- Loop CC state reset ------------------------------------------------ */

/* When a song loops via meta markers, the engine kills active notes but does
 * NOT reset channel state (CC7, CC10, pitch bend, etc.).  If a channel's last
 * CC7 value at the end of the song differs from what was in effect at the loop
 * start, the channel will have the wrong volume until its first CC7 event
 * replays.
 *
 * This function finds the effective CC7 value at loopStartTick (the last CC7
 * at or before that tick, or 127 if none) and the last CC7 at or before
 * loopEndTick.  If they differ, it inserts a CC7 at loopStartTick with the
 * correct first-playthrough value so that the engine's channel state is
 * restored on loop-back.  A re-sort of CCs is performed afterwards to keep
 * chronological order. */

int mod2rmf_ensure_loop_cc_resets(ModSongModel *song)
{
    uint32_t ch, i;
    bool needsSort = FALSE;

    if (!song || !song->loopEnabled)
    {
        return 1;
    }

    for (ch = 0; ch < song->channelCount; ++ch)
    {
        bool hasNotes = FALSE;
        uint8_t channelProgram = 0xFF;
        /* Track the effective CC7 at loopStartTick and loopEndTick.
         * Engine default is 127 (MAX_NOTE_VOLUME). */
        uint8_t cc7AtLoopStart = 127; /* engine default */
        uint8_t cc7AtLoopEnd = 127;   /* engine default */
        bool hasCC7 = FALSE;
        bool hasCC7AtLoopStart = FALSE;

        /* Check if this channel has notes */
        for (i = 0; i < song->noteCount; ++i)
        {
            if (song->notes[i].sourceChannel == ch)
            {
                hasNotes = TRUE;
                if (channelProgram == 0xFF)
                {
                    channelProgram = song->notes[i].program;
                }
                break;
            }
        }
        if (!hasNotes) continue;

        /* Scan CC7 events (sorted by tick) to find:
         * 1. Effective CC7 at loopStartTick (last CC7 at or before that tick)
         * 2. Effective CC7 at loopEndTick (last CC7 at or before that tick)
         * 3. Whether an explicit CC7 exists exactly at loopStartTick */
        for (i = 0; i < song->ccCount; ++i)
        {
            if (song->ccEvents[i].sourceChannel == ch && song->ccEvents[i].cc == 7)
            {
                hasCC7 = TRUE;
                if (song->ccEvents[i].tick <= song->loopStartTick)
                {
                    cc7AtLoopStart = song->ccEvents[i].value;
                    if (song->ccEvents[i].tick == song->loopStartTick)
                    {
                        hasCC7AtLoopStart = TRUE;
                    }
                }
                if (song->ccEvents[i].tick <= song->loopEndTick)
                {
                    cc7AtLoopEnd = song->ccEvents[i].value;
                }
            }
        }

        /* Only insert a reset if:
         * 1. The channel has CC7 events (otherwise engine default applies)
         * 2. No explicit CC7 already exists at loopStartTick
         * 3. The CC7 at loopEnd differs from CC7 at loopStart - meaning the
         *    engine will have the wrong volume when it loops back */
        if (hasCC7 && !hasCC7AtLoopStart && cc7AtLoopEnd != cc7AtLoopStart)
        {
            if (!mod2rmf_song_model_append_cc_event(song, (uint16_t)ch,
                                            song->loopStartTick, 7,
                                            cc7AtLoopStart,
                                            channelProgram))
            {
                return 0;
            }
            needsSort = TRUE;
            #ifdef _DEBUG
            fprintf(stderr, "[mod2rmf] Loop CC7 reset: ch %u -> CC7=%u @ tick %u (end was %u)\n",
                    ch, cc7AtLoopStart, (unsigned)song->loopStartTick, cc7AtLoopEnd);
            #endif
        }
    }

    if (needsSort && song->ccCount > 1)
    {
        qsort(song->ccEvents, song->ccCount,
              sizeof(ModCCEvent), mod2rmf_compare_cc_by_tick);
    }

    return 1;
}

/* Same idea as ensure_loop_cc_resets, but for pitch bend.  The engine keeps
 * channel pitch-bend state across loop-back, so if the last bend before
 * loopEnd differs from what was active at loopStart, notes will play at the
 * wrong pitch on subsequent loops (and it compounds each iteration). */

int mod2rmf_ensure_loop_pitch_bend_resets(ModSongModel *song)
{
    uint32_t ch, i;
    bool needsSort = FALSE;

    if (!song || !song->loopEnabled)
    {
        return 1;
    }

    for (ch = 0; ch < song->channelCount; ++ch)
    {
        bool hasNotes = FALSE;
        uint8_t channelProgram = 0xFF;
        /* Engine default pitch bend is center */
        uint16_t bendAtLoopStart = MOD2RMF_PITCH_BEND_CENTER;
        uint16_t bendAtLoopEnd = MOD2RMF_PITCH_BEND_CENTER;
        bool hasBend = FALSE;
        bool hasBendAtLoopStart = FALSE;

        /* Check if this channel has notes */
        for (i = 0; i < song->noteCount; ++i)
        {
            if (song->notes[i].sourceChannel == ch)
            {
                hasNotes = TRUE;
                if (channelProgram == 0xFF)
                {
                    channelProgram = song->notes[i].program;
                }
                break;
            }
        }
        if (!hasNotes) continue;

        /* Scan pitch bend events to find effective values at loop boundaries */
        for (i = 0; i < song->pitchBendCount; ++i)
        {
            if (song->pitchBendEvents[i].sourceChannel == ch)
            {
                hasBend = TRUE;
                if (song->pitchBendEvents[i].tick <= song->loopStartTick)
                {
                    bendAtLoopStart = song->pitchBendEvents[i].value;
                    if (song->pitchBendEvents[i].tick == song->loopStartTick)
                    {
                        hasBendAtLoopStart = TRUE;
                    }
                }
                if (song->pitchBendEvents[i].tick <= song->loopEndTick)
                {
                    bendAtLoopEnd = song->pitchBendEvents[i].value;
                }
            }
        }

        /* Insert a reset if bends exist, no explicit bend at loop start,
         * and the end-of-song bend differs from the loop-start bend */
        if (hasBend && !hasBendAtLoopStart && bendAtLoopEnd != bendAtLoopStart)
        {
            if (!mod2rmf_song_model_append_pitch_bend(song, (uint16_t)ch,
                                              song->loopStartTick,
                                              bendAtLoopStart,
                                              channelProgram))
            {
                return 0;
            }
            needsSort = TRUE;
            #ifdef _DEBUG
            fprintf(stderr, "[mod2rmf] Loop pitch bend reset: ch %u -> bend=%u @ tick %u (end was %u)\n",
                    ch, bendAtLoopStart, (unsigned)song->loopStartTick, bendAtLoopEnd);
            #endif
        }
    }

    if (needsSort && song->pitchBendCount > 1)
    {
        qsort(song->pitchBendEvents, song->pitchBendCount,
              sizeof(ModPitchBendEvent), mod2rmf_compare_pitch_bend_by_tick);
    }

    return 1;
}

/* --- Channel spreading by program --------------------------------------- */

/* Spread tracker channels so each unique program (instrument/sample) gets its
 * own virtual channel.  The BAE engine is fully polyphonic (note-on over
 * note-on is supported), so there is no need to split the same program across
 * multiple channels even if it plays on several tracker channels at once.
 *
 * After spreading:
 *  - note->sourceChannel is rewritten to the program's virtual channel
 *  - CC and pitch-bend events are routed via their program tag
 *  - song->channelCount is updated to reflect the new virtual channel count
 *
 * If unique programs > 16, programs are packed into 16 channels using an
 * overlap-minimizing algorithm. */


int mod2rmf_spread_channels_by_program(ModSongModel *song, bool isMod,
                                      uint8_t stereoSep)
{
    uint8_t virtualChanMap[MOD2RMF_MAX_CHANNELS][MOD2RMF_MAX_SAMPLES];
    uint8_t originalChMapped[MOD2RMF_MAX_CHANNELS];
    uint8_t nextVirtualChan;
    uint32_t i;
    uint32_t preBendCount;

    (void)isMod;
    (void)stereoSep;

    if (!song || song->noteCount == 0)
    {
        return 1;
    }

    memset(virtualChanMap, 0xFF, sizeof(virtualChanMap));
    memset(originalChMapped, 0, sizeof(originalChMapped));
    
    nextVirtualChan = (uint8_t)song->channelCount;

    /* Phase 1: Assign a virtual channel to each (tracker channel, program) tuple */
    for (i = 0; i < song->noteCount; ++i)
    {
        uint8_t ch = song->notes[i].sourceChannel;
        uint8_t prog = song->notes[i].program;

        if (ch >= MOD2RMF_MAX_CHANNELS) continue;

        if (virtualChanMap[ch][prog] == 0xFF)
        {
            if (originalChMapped[ch] == 0)
            {
                /* First program seen on this tracker channel keeps the original channel ID */
                virtualChanMap[ch][prog] = ch;
                originalChMapped[ch] = 1;
            }
            else if (nextVirtualChan < MOD2RMF_MAX_CHANNELS)
            {
                /* Subsequent programs on this channel get a new virtual channel */
                virtualChanMap[ch][prog] = nextVirtualChan++;
            }
            else
            {
                /* Fallback if we run out of virtual channels */
                virtualChanMap[ch][prog] = ch;
            }
        }
        
        song->notes[i].sourceChannel = virtualChanMap[ch][prog];
    }
    
    /* Phase 2: Route CC events */
    for (i = 0; i < song->ccCount; ++i)
    {
        uint8_t ch = song->ccEvents[i].sourceChannel;
        uint8_t prog = song->ccEvents[i].program;
        if (ch < MOD2RMF_MAX_CHANNELS)
        {
            if (virtualChanMap[ch][prog] == 0xFF)
            {
                if (originalChMapped[ch] == 0) { virtualChanMap[ch][prog] = ch; originalChMapped[ch] = 1; }
                else if (nextVirtualChan < MOD2RMF_MAX_CHANNELS) { virtualChanMap[ch][prog] = nextVirtualChan++; }
                else { virtualChanMap[ch][prog] = ch; }
            }
            song->ccEvents[i].sourceChannel = virtualChanMap[ch][prog];
        }
    }

    /* Phase 3: Route pitch bend events */
    preBendCount = song->pitchBendCount;
    for (i = 0; i < preBendCount; ++i)
    {
        uint8_t ch = song->pitchBendEvents[i].sourceChannel;
        uint8_t prog = song->pitchBendEvents[i].program;
        if (ch < MOD2RMF_MAX_CHANNELS)
        {
            if (virtualChanMap[ch][prog] == 0xFF)
            {
                if (originalChMapped[ch] == 0) { virtualChanMap[ch][prog] = ch; originalChMapped[ch] = 1; }
                else if (nextVirtualChan < MOD2RMF_MAX_CHANNELS) { virtualChanMap[ch][prog] = nextVirtualChan++; }
                else { virtualChanMap[ch][prog] = ch; }
            }
            song->pitchBendEvents[i].sourceChannel = virtualChanMap[ch][prog];
        }
    }

    song->channelCount = nextVirtualChan;

    /* Phase 4: Sort events by tick so the write-phase dedup processes them chronologically. */
    if (song->pitchBendCount > 1)
    {
        qsort(song->pitchBendEvents, song->pitchBendCount,
              sizeof(ModPitchBendEvent), mod2rmf_compare_pitch_bend_by_tick);
    }
    if (song->ccCount > 1)
    {
        qsort(song->ccEvents, song->ccCount,
              sizeof(ModCCEvent), mod2rmf_compare_cc_by_tick);
    }

    return 1;
}

void mod2rmf_channel_profile_cleanup(ChannelProfile *profiles, uint32_t count)
{
    uint32_t i;
    for (i = 0; i < count; ++i)
    {
        free(profiles[i].activeRanges);
        profiles[i].activeRanges = NULL;
        profiles[i].rangeCount = 0;
        profiles[i].rangeCapacity = 0;
    }
}

void mod2rmf_song_model_init(ModSongModel *song)
{
    if (song)
    {
        memset(song, 0, sizeof(*song));
        song->pitchBendRangeSemitones = MOD2RMF_PITCH_BEND_RANGE_ST;
    }
}

void mod2rmf_song_model_dispose(ModSongModel *song)
{
    if (!song)
    {
        return;
    }
    free(song->playables);
    song->playables = NULL;
    free(song->notes);
    song->notes = NULL;
    free(song->ccEvents);
    song->ccEvents = NULL;
    free(song->pitchBendEvents);
    song->pitchBendEvents = NULL;
    free(song->tempoChanges);
    song->tempoChanges = NULL;
    song->playableCount = 0;
    song->noteCount = 0;
    song->noteCapacity = 0;
    song->ccCount = 0;
    song->ccCapacity = 0;
    song->pitchBendCount = 0;
    song->pitchBendCapacity = 0;
    song->tempoChangeCount = 0;
    song->tempoChangeCapacity = 0;
}

int mod2rmf_song_model_append_note(ModSongModel *song,
                                  uint16_t sourceChannel,
                                  uint32_t startTick,
                                  uint32_t durationTicks,
                                  unsigned char note,
                                  unsigned char velocity,
                                  unsigned char program)
{
    ModNoteEvent *newNotes;
    uint32_t newCapacity;

    if (!song)
    {
        return 0;
    }
    if (song->noteCount >= song->noteCapacity)
    {
        newCapacity = (song->noteCapacity == 0) ? 1024U : (song->noteCapacity * 2U);
        newNotes = (ModNoteEvent *)realloc(song->notes, newCapacity * sizeof(ModNoteEvent));
        if (!newNotes)
        {
            return 0;
        }
        song->notes = newNotes;
        song->noteCapacity = newCapacity;
    }

    song->notes[song->noteCount].sourceChannel = sourceChannel;
    song->notes[song->noteCount].startTick = startTick;
    song->notes[song->noteCount].durationTicks = durationTicks;
    song->notes[song->noteCount].note = note;
    song->notes[song->noteCount].velocity = velocity;
    song->notes[song->noteCount].program = program;
    song->noteCount++;
    return 1;
}
