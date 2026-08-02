/*
 * BAE_ProbeSongLength.c — foobar2000 platform add-on
 *
 * Extract SMF from supported containers and integrate a tempo map to obtain
 * duration. No BAEMixer / MusicGlobals required.
 */

#include "BAE_ProbeSongLength.h"

#include "X_API.h"
#include "X_Formats.h"
#include "GenSnd.h"
#include "GenRMI.h"
#include "GenXMF.h"

#include <stdlib.h>
#include <string.h>

#if USE_MTHC_SUPPORT == TRUE
#include "mthc_decomp.h"
#endif

/* -------------------------------------------------------------------------- */
/* SMF tempo-map walker                                                       */
/* -------------------------------------------------------------------------- */

#define FB2K_DEFAULT_TEMPO_US  500000u /* 120 BPM */
#define FB2K_MAX_TEMPO_EVENTS  4096u
#define FB2K_MAX_TRACKS        256u

typedef struct FB2K_TempoEvent
{
	uint32_t tick;
	uint32_t usPerQuarter;
} FB2K_TempoEvent;

static int FB2K_ReadVLQ(const unsigned char *p, uint32_t remain, uint32_t *outVal, uint32_t *outConsumed)
{
	uint32_t value = 0;
	uint32_t i;

	if (!p || !outVal || !outConsumed)
		return 0;
	for (i = 0; i < 4 && i < remain; ++i)
	{
		unsigned char b = p[i];
		value = (value << 7) | (uint32_t)(b & 0x7Fu);
		if ((b & 0x80u) == 0)
		{
			*outVal = value;
			*outConsumed = i + 1;
			return 1;
		}
	}
	return 0;
}

static int FB2K_TempoCmp(const void *a, const void *b)
{
	const FB2K_TempoEvent *ea = (const FB2K_TempoEvent *)a;
	const FB2K_TempoEvent *eb = (const FB2K_TempoEvent *)b;
	if (ea->tick < eb->tick)
		return -1;
	if (ea->tick > eb->tick)
		return 1;
	return 0;
}

static uint64_t FB2K_TicksToMicros(uint32_t deltaTicks, uint32_t usPerQuarter, uint16_t ppqn)
{
	if (ppqn == 0 || usPerQuarter == 0)
		return 0;
	return ((uint64_t)deltaTicks * (uint64_t)usPerQuarter) / (uint64_t)ppqn;
}

static BAEResult FB2K_SmfDurationMicros(const unsigned char *smf, uint32_t smfLen, uint32_t *outMicros)
{
	uint16_t format;
	uint16_t ntrks;
	uint16_t division;
	uint16_t ppqn;
	uint32_t pos;
	uint32_t trackIndex;
	uint32_t endTick = 0;
	FB2K_TempoEvent *tempos = NULL;
	uint32_t tempoCount = 0;
	uint32_t tempoCap = 0;
	uint32_t currentTick;
	uint32_t currentTempo;
	uint64_t accumulated;
	uint32_t i;

	if (!smf || smfLen < 14 || !outMicros)
		return BAE_PARAM_ERR;
	*outMicros = 0;

	if (memcmp(smf, "MThd", 4) != 0)
		return BAE_BAD_FILE;

	{
		uint32_t hdrLen = ((uint32_t)smf[4] << 24) | ((uint32_t)smf[5] << 16) |
		                  ((uint32_t)smf[6] << 8) | (uint32_t)smf[7];
		if (hdrLen < 6 || smfLen < 8 + hdrLen)
			return BAE_BAD_FILE;
		format = (uint16_t)(((uint16_t)smf[8] << 8) | smf[9]);
		ntrks = (uint16_t)(((uint16_t)smf[10] << 8) | smf[11]);
		division = (uint16_t)(((uint16_t)smf[12] << 8) | smf[13]);
		(void)format;
		if (division & 0x8000u)
			return BAE_UNSUPPORTED_FORMAT; /* SMPTE timing */
		ppqn = division;
		if (ppqn == 0)
			return BAE_BAD_FILE;
		if (ntrks == 0 || ntrks > FB2K_MAX_TRACKS)
			return BAE_BAD_FILE;
		pos = 8 + hdrLen;
	}

	tempoCap = 64;
	tempos = (FB2K_TempoEvent *)malloc(sizeof(FB2K_TempoEvent) * tempoCap);
	if (!tempos)
		return BAE_MEMORY_ERR;

	for (trackIndex = 0; trackIndex < ntrks; ++trackIndex)
	{
		uint32_t trackLen;
		uint32_t trackEnd;
		uint32_t absTick = 0;
		unsigned char running = 0;
		const unsigned char *tp;
		uint32_t remain;

		if (pos + 8 > smfLen)
		{
			free(tempos);
			return BAE_BAD_FILE;
		}
		if (memcmp(smf + pos, "MTrk", 4) != 0)
		{
			free(tempos);
			return BAE_BAD_FILE;
		}
		trackLen = ((uint32_t)smf[pos + 4] << 24) | ((uint32_t)smf[pos + 5] << 16) |
		           ((uint32_t)smf[pos + 6] << 8) | (uint32_t)smf[pos + 7];
		pos += 8;
		if (pos + trackLen > smfLen)
		{
			free(tempos);
			return BAE_BAD_FILE;
		}
		trackEnd = pos + trackLen;
		tp = smf + pos;
		remain = trackLen;

		while (remain > 0)
		{
			uint32_t delta = 0;
			uint32_t consumed = 0;
			unsigned char status;
			unsigned char metaType;
			uint32_t metaLen = 0;

			if (!FB2K_ReadVLQ(tp, remain, &delta, &consumed))
			{
				free(tempos);
				return BAE_BAD_FILE;
			}
			tp += consumed;
			remain -= consumed;
			absTick += delta;

			if (remain == 0)
			{
				free(tempos);
				return BAE_BAD_FILE;
			}

			status = tp[0];
			if (status < 0x80u)
			{
				if (running == 0)
				{
					free(tempos);
					return BAE_BAD_FILE;
				}
				status = running;
			}
			else
			{
				tp += 1;
				remain -= 1;
				if (status < 0xF0u)
					running = status;
				else if (status == 0xF0u || status == 0xF7u || status == 0xFFu)
					running = 0;
			}

			if (status == 0xFFu)
			{
				if (remain < 1)
				{
					free(tempos);
					return BAE_BAD_FILE;
				}
				metaType = tp[0];
				tp += 1;
				remain -= 1;
				if (!FB2K_ReadVLQ(tp, remain, &metaLen, &consumed))
				{
					free(tempos);
					return BAE_BAD_FILE;
				}
				tp += consumed;
				remain -= consumed;
				if (metaLen > remain)
				{
					free(tempos);
					return BAE_BAD_FILE;
				}

				if (metaType == 0x2Fu)
				{
					if (absTick > endTick)
						endTick = absTick;
					/* consume rest of track */
					remain = 0;
					break;
				}
				if (metaType == 0x51u && metaLen == 3)
				{
					uint32_t us = ((uint32_t)tp[0] << 16) | ((uint32_t)tp[1] << 8) | (uint32_t)tp[2];
					if (us == 0)
						us = FB2K_DEFAULT_TEMPO_US;
					if (tempoCount >= FB2K_MAX_TEMPO_EVENTS)
					{
						free(tempos);
						return BAE_GENERAL_ERR;
					}
					if (tempoCount >= tempoCap)
					{
						uint32_t newCap = tempoCap * 2;
						FB2K_TempoEvent *grown = (FB2K_TempoEvent *)realloc(tempos, sizeof(FB2K_TempoEvent) * newCap);
						if (!grown)
						{
							free(tempos);
							return BAE_MEMORY_ERR;
						}
						tempos = grown;
						tempoCap = newCap;
					}
					tempos[tempoCount].tick = absTick;
					tempos[tempoCount].usPerQuarter = us;
					++tempoCount;
				}
				tp += metaLen;
				remain -= metaLen;
				continue;
			}

			if (status == 0xF0u || status == 0xF7u)
			{
				if (!FB2K_ReadVLQ(tp, remain, &metaLen, &consumed))
				{
					free(tempos);
					return BAE_BAD_FILE;
				}
				tp += consumed;
				remain -= consumed;
				if (metaLen > remain)
				{
					free(tempos);
					return BAE_BAD_FILE;
				}
				tp += metaLen;
				remain -= metaLen;
				continue;
			}

			switch (status & 0xF0u)
			{
			case 0xC0u:
			case 0xD0u:
				if (remain < 1)
				{
					free(tempos);
					return BAE_BAD_FILE;
				}
				tp += 1;
				remain -= 1;
				break;
			default:
				if (remain < 2)
				{
					free(tempos);
					return BAE_BAD_FILE;
				}
				tp += 2;
				remain -= 2;
				break;
			}
		}

		if (absTick > endTick)
			endTick = absTick;
		pos = trackEnd;
	}

	if (tempoCount > 1)
		qsort(tempos, tempoCount, sizeof(FB2K_TempoEvent), FB2K_TempoCmp);

	currentTick = 0;
	currentTempo = FB2K_DEFAULT_TEMPO_US;
	accumulated = 0;

	/* Apply tick-0 tempos first (last wins — same as typical SMF practice). */
	for (i = 0; i < tempoCount && tempos[i].tick == 0; ++i)
		currentTempo = tempos[i].usPerQuarter;

	for (; i < tempoCount; ++i)
	{
		uint32_t eventTick = tempos[i].tick;
		if (eventTick >= endTick)
			break;
		if (eventTick > currentTick)
		{
			accumulated += FB2K_TicksToMicros(eventTick - currentTick, currentTempo, ppqn);
			currentTick = eventTick;
		}
		currentTempo = tempos[i].usPerQuarter;
	}

	if (endTick > currentTick)
		accumulated += FB2K_TicksToMicros(endTick - currentTick, currentTempo, ppqn);

	free(tempos);

	if (accumulated == 0)
		accumulated = 1000; /* match BAE minimum */
	if (accumulated > 0xFFFFFFFFull)
		return BAE_GENERAL_ERR;

	*outMicros = (uint32_t)accumulated;
	return BAE_NO_ERROR;
}

/* -------------------------------------------------------------------------- */
/* Container → SMF                                                            */
/* -------------------------------------------------------------------------- */

static void *FB2K_DupToXPtr(const void *src, uint32_t len)
{
	void *dst;
	if (!src || len == 0)
		return NULL;
	dst = XNewPtr(len);
	if (dst)
		XBlockMove((XPTRC)src, dst, len);
	return dst;
}

static XPTR FB2K_DecodeMidiResource(XPTR raw, XResourceType rtype, int32_t *ioSize)
{
	XPTR dec;

	if (!raw || !ioSize)
		return NULL;
	if (rtype == ID_ECMI)
	{
		XDecryptData(raw, (uint32_t)*ioSize);
		dec = XDecompressPtr(raw, (uint32_t)*ioSize, TRUE);
		XDisposePtr(raw);
		if (dec)
			*ioSize = (int32_t)XGetPtrSize(dec);
		return dec;
	}
	if (rtype == ID_EMID)
	{
		XDecryptData(raw, (uint32_t)*ioSize);
		return raw;
	}
	if (rtype == ID_CMID)
	{
		dec = XDecompressPtr(raw, (uint32_t)*ioSize, TRUE);
		XDisposePtr(raw);
		if (dec)
			*ioSize = (int32_t)XGetPtrSize(dec);
		return dec;
	}
	return raw;
}

static BAEResult FB2K_ExtractRmfToSmf(const void *data, uint32_t size, void **outSmf, uint32_t *outSmfLen)
{
	XFILE fileRef;
	SongResource *songResource = NULL;
	SongResource_Info *songInfo = NULL;
	XLongResourceID songID = 0;
	XShortResourceID objectResourceID;
	int32_t songSize = 0;
	int32_t midiSize = 0;
	XPTR midiData = NULL;
	int32_t fallbackIndex;
	XLongResourceID fallbackID;

	if (!data || size == 0 || !outSmf || !outSmfLen)
		return BAE_PARAM_ERR;
	*outSmf = NULL;
	*outSmfLen = 0;

	fileRef = XFileOpenResourceFromMemory((XPTR)data, size, TRUE);
	if (!fileRef)
		return BAE_BAD_FILE;

	songResource = (SongResource *)XGetIndexedFileResource(fileRef, ID_SONG, &songID, 0, NULL, &songSize);
	if (!songResource)
	{
		XFileClose(fileRef);
		return BAE_BAD_FILE;
	}

	songInfo = XGetSongResourceInfo(songResource, songSize);
	if (!songInfo)
	{
		XDisposePtr(songResource);
		XFileClose(fileRef);
		return BAE_BAD_FILE;
	}
	objectResourceID = songInfo->objectResourceID;

	midiData = XGetFileResource(fileRef, ID_MIDI, objectResourceID, NULL, &midiSize);
	if (!midiData)
		midiData = XGetFileResource(fileRef, ID_MIDI_OLD, objectResourceID, NULL, &midiSize);
	if (!midiData)
	{
		midiData = XGetFileResource(fileRef, ID_ECMI, objectResourceID, NULL, &midiSize);
		midiData = FB2K_DecodeMidiResource(midiData, ID_ECMI, &midiSize);
	}
	if (!midiData)
	{
		midiData = XGetFileResource(fileRef, ID_EMID, objectResourceID, NULL, &midiSize);
		midiData = FB2K_DecodeMidiResource(midiData, ID_EMID, &midiSize);
	}
	if (!midiData)
	{
		midiData = XGetFileResource(fileRef, ID_CMID, objectResourceID, NULL, &midiSize);
		midiData = FB2K_DecodeMidiResource(midiData, ID_CMID, &midiSize);
	}

	/* Indexed fallbacks (same order as the editor loader). */
	fallbackIndex = 0;
	while (!midiData && fallbackIndex < 64)
	{
		midiData = XGetIndexedFileResource(fileRef, ID_MIDI, &fallbackID, fallbackIndex, NULL, &midiSize);
		if (midiData)
			break;
		++fallbackIndex;
	}
	fallbackIndex = 0;
	while (!midiData && fallbackIndex < 64)
	{
		midiData = XGetIndexedFileResource(fileRef, ID_MIDI_OLD, &fallbackID, fallbackIndex, NULL, &midiSize);
		if (midiData)
			break;
		++fallbackIndex;
	}
	fallbackIndex = 0;
	while (!midiData && fallbackIndex < 64)
	{
		XPTR raw = XGetIndexedFileResource(fileRef, ID_ECMI, &fallbackID, fallbackIndex, NULL, &midiSize);
		if (!raw)
			break;
		midiData = FB2K_DecodeMidiResource(raw, ID_ECMI, &midiSize);
		if (midiData)
			break;
		++fallbackIndex;
	}
	fallbackIndex = 0;
	while (!midiData && fallbackIndex < 64)
	{
		XPTR raw = XGetIndexedFileResource(fileRef, ID_EMID, &fallbackID, fallbackIndex, NULL, &midiSize);
		if (!raw)
			break;
		midiData = FB2K_DecodeMidiResource(raw, ID_EMID, &midiSize);
		if (midiData)
			break;
		++fallbackIndex;
	}
	fallbackIndex = 0;
	while (!midiData && fallbackIndex < 64)
	{
		XPTR raw = XGetIndexedFileResource(fileRef, ID_CMID, &fallbackID, fallbackIndex, NULL, &midiSize);
		if (!raw)
			break;
		midiData = FB2K_DecodeMidiResource(raw, ID_CMID, &midiSize);
		if (midiData)
			break;
		++fallbackIndex;
	}

	XDisposeSongResourceInfo(songInfo);
	XDisposePtr(songResource);
	XFileClose(fileRef);

	if (!midiData || midiSize <= 0)
		return BAE_BAD_FILE;

	/* Ensure MThd — some resources are already SMF. */
	if (midiSize >= 4 && memcmp(midiData, "MThd", 4) == 0)
	{
		*outSmf = midiData;
		*outSmfLen = (uint32_t)midiSize;
		return BAE_NO_ERROR;
	}

	/* Not SMF — dispose and fail (rare). */
	XDisposePtr(midiData);
	return BAE_BAD_FILE;
}

static BAEResult FB2K_ExtractToSmf(void const *data, uint32_t dataSize, BAEFileType ftype,
                                   void **outSmf, uint32_t *outSmfLen)
{
	if (!data || dataSize == 0 || !outSmf || !outSmfLen)
		return BAE_PARAM_ERR;
	*outSmf = NULL;
	*outSmfLen = 0;

	if (ftype == BAE_MIDI_TYPE)
	{
		*outSmf = FB2K_DupToXPtr(data, dataSize);
		if (!*outSmf)
			return BAE_MEMORY_ERR;
		*outSmfLen = dataSize;
		return BAE_NO_ERROR;
	}

#if USE_RMI_SUPPORT == TRUE
	if (ftype == BAE_RMI)
	{
		unsigned char *midi = NULL;
		uint32_t midiLen = 0;
		OPErr oerr = GM_LoadRMIFromMemory((const unsigned char *)data, dataSize, &midi, &midiLen, FALSE);
		if (oerr != NO_ERR || !midi || midiLen == 0)
			return BAE_BAD_FILE;
		*outSmf = midi; /* already XNewPtr from GenRMI */
		*outSmfLen = midiLen;
		return BAE_NO_ERROR;
	}
#endif

#if USE_MTHC_SUPPORT == TRUE
	if (ftype == BAE_MTHC || (dataSize >= 4 && memcmp(data, "MThc", 4) == 0))
	{
		unsigned char *midi = NULL;
		uint32_t midiLen = 0;
		if (mthc_decompress_memory(data, dataSize, &midi, &midiLen) != 0 || !midi || midiLen == 0)
			return BAE_BAD_FILE;
		*outSmf = FB2K_DupToXPtr(midi, midiLen);
		free(midi);
		if (!*outSmf)
			return BAE_MEMORY_ERR;
		*outSmfLen = midiLen;
		return BAE_NO_ERROR;
	}
#endif

	if (ftype == BAE_RMF)
		return FB2K_ExtractRmfToSmf(data, dataSize, outSmf, outSmfLen);

#if USE_XMF_SUPPORT == TRUE && USE_NATIVE_DLS == TRUE
	if (ftype == BAE_XMF)
	{
		/* XMF/MXMF = SMF + optional DLS — never embedded RMF. */
		return BAE_ExtractXmfSequenceFromMemory(data, dataSize, outSmf, outSmfLen);
	}
#endif

#if USE_RETRO_RINGTONE_SUPPORT == TRUE
	if (ftype == BAE_RINGTONE_IMY || ftype == BAE_RINGTONE_RNG || ftype == BAE_RINGTONE_RTX)
	{
		unsigned char *midi = NULL;
		uint32_t midiLen = 0;
		BAEResult rerr = BAERingtone_ConvertToMidiFromMemory(data, dataSize, ftype, &midi, &midiLen);
		if (rerr != BAE_NO_ERROR || !midi || midiLen == 0)
		{
			BAERingtone_FreeMidiBuffer(midi);
			return (rerr != BAE_NO_ERROR) ? rerr : BAE_BAD_FILE;
		}
		*outSmf = FB2K_DupToXPtr(midi, midiLen);
		BAERingtone_FreeMidiBuffer(midi);
		if (!*outSmf)
			return BAE_MEMORY_ERR;
		*outSmfLen = midiLen;
		return BAE_NO_ERROR;
	}
#endif

	return BAE_NOT_SETUP;
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

BAEResult BAE_ProbeSongLengthFromMemory(void const *data,
                                        uint32_t dataSize,
                                        uint32_t *outLengthMicros,
                                        BAEFileType *outFileType)
{
	BAEFileType ftype;
	void *smf = NULL;
	uint32_t smfLen = 0;
	BAEResult err;
	uint32_t micros = 0;

	if (!data || dataSize == 0 || !outLengthMicros)
		return BAE_PARAM_ERR;

	*outLengthMicros = 0;
	if (outFileType)
		*outFileType = BAE_INVALID_TYPE;

	ftype = BAE_INVALID_TYPE;
	if (dataSize >= 4)
	{
		const unsigned char *bytes = (const unsigned char *)data;
		if ((bytes[0] == 'I' && bytes[1] == 'R' && bytes[2] == 'E' && bytes[3] == 'Z') ||
		    (bytes[0] == 'Z' && bytes[1] == 'R' && bytes[2] == 'E' && bytes[3] == 'Z'))
		{
			ftype = BAE_RMF;
		}
	}
	if (ftype == BAE_INVALID_TYPE)
	{
		int32_t probeSize = (int32_t)dataSize;
		if (probeSize > 64)
			probeSize = 64;
		ftype = X_DetermineFileTypeByData((const unsigned char *)data, probeSize);
	}
	/* XMF magic is "XMF_" — may need a larger probe than 64 for some files, but
	 * the FOURCC is at offset 0 so 64 is enough for type detection. */
	if (ftype == BAE_INVALID_TYPE && dataSize >= 8 &&
	    memcmp(data, "XMF_", 4) == 0)
	{
#if USE_XMF_SUPPORT == TRUE
		ftype = BAE_XMF;
#endif
	}

	if (outFileType)
		*outFileType = ftype;
	if (ftype == BAE_INVALID_TYPE)
		return BAE_BAD_FILE;
	if (ftype == BAE_WAVE_TYPE || ftype == BAE_AIFF_TYPE || ftype == BAE_AU_TYPE)
		return BAE_INVALID_TYPE;

	err = FB2K_ExtractToSmf(data, dataSize, ftype, &smf, &smfLen);
	if (err != BAE_NO_ERROR || !smf || smfLen == 0)
	{
		if (smf)
			XDisposePtr(smf);
		return (err != BAE_NO_ERROR) ? err : BAE_BAD_FILE;
	}

	err = FB2K_SmfDurationMicros((const unsigned char *)smf, smfLen, &micros);
	XDisposePtr(smf);
	if (err != BAE_NO_ERROR)
		return err;

	*outLengthMicros = (micros == 0) ? 1000u : micros;
	return BAE_NO_ERROR;
}
