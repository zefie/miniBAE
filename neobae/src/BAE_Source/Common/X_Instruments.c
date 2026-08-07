/*
    Copyright (c) 2009 Beatnik, Inc All rights reserved.
    
    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are
    met:
    
    Redistributions of source code must retain the above copyright notice,
    this list of conditions and the following disclaimer.
    
    Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.
    
    Neither the name of the Beatnik, Inc nor the names of its contributors
    may be used to endorse or promote products derived from this software
    without specific prior written permission.
    
    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
    IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
    TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
    PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
    HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
    TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
    PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
    LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
    NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
    SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
/*
    Additional modifications (c) 2021-2026 zefie
    Licensed under the GNU Lesser General Public License v3.0 or later.

    Imported from Arquivotheca/dano-src
    commit 4281ae895a14b867ef487ca045c71ffc8cadaa05
    (R6 Source/src/kit/bae) and adapted for NeoBAE.
*/
/*****************************************************************************/
/*
**	X_Instruments.c
**
**	Tools for creating instruments
**
**	(c) Copyright 1996-1998 Beatnik, Inc, All Rights Reserved.
**	Written by Steve Hales
**
**	Beatnik products contain certain trade secrets and confidential and
**	proprietary information of Beatnik.  Use, reproduction, disclosure
**	and distribution by any means are prohibited, except pursuant to
**	a written license from Beatnik. Use of copyright notice is
**	precautionary and does not imply publication or disclosure.
**
**	Restricted Rights Legend:
**	Use, duplication, or disclosure by the Government is subject to
**	restrictions as set forth in subparagraph (c)(1)(ii) of The
**	Rights in Technical Data and Computer Software clause in DFARS
**	252.227-7013 or subparagraphs (c)(1) and (2) of the Commercial
**	Computer Software--Restricted Rights at 48 CFR 52.227-19, as
**	applicable.
**
**	Confidential-- Internal use only
**
**	History	-
**	2/16/98		Created. Pulled from MacOS specific editor codebase
**				Moved XNewInstrument & XDisposeInstrument to DriverTools.c
**				Renamed XNewInstrument to XNewInstrumentResource, and XDisposeInstrument
**				to XDisposeInstrumentResource
**
**	6/5/98		Jim Nitchals RIP	1/15/62 - 6/5/98
**				I'm going to miss your irreverent humor. Your coding style and desire
**				to make things as fast as possible. Your collaboration behind this entire
**				codebase. Your absolute belief in creating the best possible relationships 
**				from honesty and integrity. Your ability to enjoy conversation. Your business 
**				savvy in understanding the big picture. Your gentleness. Your willingness 
**				to understand someone else's way of thinking. Your debates on the latest 
**				political issues. Your generosity. Your great mimicking of cartoon voices. 
**				Your friendship. - Steve Hales
**
**	11/10/98	Added XNewInstrumentWithBasicEnvelopeResource. Fixed memory bug
**				with XReconstructInstrument
**	5/28/99		MOE:  Changed "offset" in XReconstructInstrument() to unsigned
**				to eliminate signed/unsigned comparison warning
**	7/29/99		Fixed warning with XDeleteEnvelopePoint
*/
/*****************************************************************************/

#include "GenSnd.h"
#include "GenPriv.h"
#include "X_Formats.h"
#include "X_Instruments.h"


#if USE_CREATION_API == TRUE

// Create a new instrument. Free with XDisposeInstrument. This creates a basic
// instrument with no evelope or extra data.
InstrumentResource * XNewInstrumentResource(XShortResourceID leadSndID)
{
	InstrumentResource *theX;

	theX = (InstrumentResource *)XNewPtr((int32_t)sizeof(InstrumentResource));
	if (theX)
	{
		XPutShort(&theX->sndResourceID, leadSndID);
		XPutShort(&theX->tremoloEnd, 0x8000);

		theX->flags1 |= ZBF_enableInterpolate | ZBF_enableAmpScale | ZBF_useSampleRate;
	}
	return theX;
}

InstrumentResource * XNewInstrumentWithBasicEnvelopeResource(XShortResourceID leadSndID, XEnvelopeType type)
{
	InstrumentResource	*tempX, *finalX;
	XInstrumentData		*theI;
	int32_t				size;

	finalX = NULL;
	tempX = XNewInstrumentResource(leadSndID);
	if (tempX)
	{
		size = XGetPtrSize(tempX);
		theI = XCreateXInstrument(tempX, size);
		if (theI)
		{
			XAddDefaultADSREnvelope(theI, type);

			finalX = XReconstructInstrument(tempX, size, theI);
			XDisposePtr((XPTR)theI);
		}
		XDisposeInstrumentResource(tempX);
	}

	return finalX;
}

void XDisposeInstrumentResource(InstrumentResource *theX)
{
	XDisposePtr((XPTR)theX);
}


int32_t XGetTotalEnvelopeTime(XEnvelopeData *pXEnvelope)
{
	int32_t	totalTime, count;

	totalTime = 0;
	if (pXEnvelope)
	{
		for (count = 0; count < pXEnvelope->stageCount; count++)
		{
			totalTime += pXEnvelope->time[count];
		}
	}
	return totalTime;
}

void XEnvelopeAdjustSustainTime(XEnvelopeData *pXEnvelope)
{
	int32_t	count, scale;

	if (pXEnvelope)
	{
		scale = pXEnvelope->endScaleH - pXEnvelope->startScaleH;
		for (count = 0; count < pXEnvelope->stageCount; count++)
		{
			if (pXEnvelope->flags[count] == ADSR_SUSTAIN)
			{
				pXEnvelope->time[count] = scale / 5;
				break;
			}
		}
	}
}

int32_t XFindType(XInstrumentData *pXInstrument, XUnitType type)
{
	int32_t		count;

	for (count = 0; count < pXInstrument->unitCount; count++)
	{
		if (pXInstrument->units[count].unitType == type)
		{
			return count;
		}
	}
	return -1;
}

int32_t XAddType(XInstrumentData *pXInstrument, XUnitType type)
{
	int32_t		count;

	count = pXInstrument->unitCount++;
	pXInstrument->units[count].unitType = type;
	pXInstrument->units[count].unitID = count;
	return count;
}

void XRemoveType(XInstrumentData *pXInstrument, XUnitType unitType, uint32_t unitID)
{
	int32_t		count, count2;

	for (count = 0; count < pXInstrument->unitCount; count++)
	{
		if ( (pXInstrument->units[count].unitType == unitType) && (pXInstrument->units[count].unitID == unitID) )
		{
			for (count2 = count; count2 < (pXInstrument->unitCount-1); count2++)
			{
				pXInstrument->units[count2] = pXInstrument->units[count2+1];
			}

			pXInstrument->unitCount--;
			break;
		}
	}
}

bool XDeleteEnvelopePoint(XEnvelopeData *pEnvelope, int16_t whichPoint)
{
	int32_t		count;
	bool		deletePoint;
	int32_t		prevTime;

	deletePoint = FALSE;
	if (pEnvelope)
	{
		// delete select point
		if (whichPoint)		// can't delete point zero
		{
			// can't delete last point
			if (pEnvelope->flags[whichPoint] != ADSR_TERMINATE)
			{
				deletePoint = TRUE;
			}
		}
		// can't delete sustain point if it will become point zero
		if ( (pEnvelope->flags[whichPoint] == ADSR_SUSTAIN) && (whichPoint == 1) )
		{
			deletePoint = FALSE;
		}
			
		if (deletePoint)
		{
			prevTime = pEnvelope->time[whichPoint];
			for (count = whichPoint; count < ADSR_STAGES; count++)
			{
				pEnvelope->time[count] = pEnvelope->time[count+1];
				pEnvelope->level[count] = pEnvelope->level[count+1];
				pEnvelope->flags[count] = pEnvelope->flags[count+1];
			}
			pEnvelope->time[whichPoint] += prevTime;
			pEnvelope->stageCount--;
		}
	}
	return deletePoint;
}

void XFillDefaultADSREnvelope(XEnvelopeData *pEnvelope, XEnvelopeType type)
{
	switch (type)
	{
		case NONE_E:	// none
			pEnvelope->stageCount = 0;
			break;
		case FOUR_POINT_E:
			pEnvelope->level[0] = 0;
			pEnvelope->flags[0] = ADSR_LINEAR_RAMP;
			pEnvelope->time[0] = 0;
		
			pEnvelope->level[1] = VOLUME_RANGE;
			pEnvelope->flags[1] = ADSR_LINEAR_RAMP;
			pEnvelope->time[1] = 15000 * 2;
		
			pEnvelope->level[2] = 3200;
			pEnvelope->flags[2] = ADSR_LINEAR_RAMP;
			pEnvelope->time[2] = 30000 * 2;
		
			pEnvelope->level[3] = 3200;
			pEnvelope->flags[3] = ADSR_SUSTAIN;
			pEnvelope->time[3] = SUSTAIN_DEFAULT_TIME;
		
			pEnvelope->level[4] = 0;
			pEnvelope->flags[4] = ADSR_TERMINATE;
			pEnvelope->time[4] = 30000 * 2;
			pEnvelope->stageCount = 5;
			break;
		case FLAT_FULL_E:
			pEnvelope->level[0] = VOLUME_RANGE;
			pEnvelope->flags[0] = ADSR_LINEAR_RAMP;
			pEnvelope->time[0] = 0;
		
			pEnvelope->level[1] = VOLUME_RANGE;
			pEnvelope->flags[1] = ADSR_LINEAR_RAMP;
			pEnvelope->time[1] = 30000 * 2;
		
			pEnvelope->level[2] = VOLUME_RANGE;
			pEnvelope->flags[2] = ADSR_SUSTAIN;
			pEnvelope->time[2] = SUSTAIN_DEFAULT_TIME;
		
			pEnvelope->level[3] = 0;
			pEnvelope->flags[3] = ADSR_TERMINATE;
			pEnvelope->time[3] = 30000 * 2;
			pEnvelope->stageCount = 4;
			break;		
		case TWO_POINT_E:
			pEnvelope->level[0] = 0;
			pEnvelope->flags[0] = ADSR_LINEAR_RAMP;
			pEnvelope->time[0] = 0;
		
			pEnvelope->level[1] = VOLUME_RANGE;
			pEnvelope->flags[1] = ADSR_LINEAR_RAMP;
			pEnvelope->time[1] = 15000 * 2;
		
			pEnvelope->level[2] = 0;
			pEnvelope->flags[2] = ADSR_TERMINATE;
			pEnvelope->time[2] = 30000 * 2;
			pEnvelope->stageCount = 3;
			break;
	}
}

void XShiftEnvelopeVolume(XEnvelopeData *pEnvelope, int32_t shift)
{
	int32_t		count, stage;

	stage = pEnvelope->stageCount;
	for (count = 0; count < stage; count++)
	{
		pEnvelope->level[count] += shift;
	}
}


void XAddZeroEnvelope(XEnvelopeData *pEnvelope)
{
	int32_t		count, stage;

	stage = pEnvelope->stageCount;
	for (count = 0; count < stage; count++)
	{
		if (pEnvelope->flags[count] == ADSR_SUSTAIN)
		{
			pEnvelope->time[count] = SUSTAIN_DEFAULT_TIME;
		}
	}

	if (pEnvelope->time[0])
	{
		pEnvelope->stageCount++;		// add extra
		for (count = ADSR_STAGES; count > 0; count--)
		{
			pEnvelope->level[count] = pEnvelope->level[count-1];
			pEnvelope->flags[count] = pEnvelope->flags[count-1];
			pEnvelope->time[count] = pEnvelope->time[count-1];
		}
		pEnvelope->level[0] = 0;
		pEnvelope->flags[0] = ADSR_LINEAR_RAMP;
		pEnvelope->time[0] = 0;
	}
}

void XRemoveZeroEnvelope(XEnvelopeData *pEnvelope)
{
	int32_t		count, stage;

	stage = pEnvelope->stageCount;
	for (count = 0; count < stage; count++)
	{
		if (pEnvelope->flags[count] == ADSR_SUSTAIN)
		{
			pEnvelope->time[count] = 0;
		}
	}

	if ((pEnvelope->level[0] == 0) &&
		(pEnvelope->flags[0] == ADSR_LINEAR_RAMP) && 
		(pEnvelope->time[0] == 0) )
	{
		pEnvelope->stageCount--;
		for (count = 0; count < ADSR_STAGES; count++)
		{
			pEnvelope->level[count] = pEnvelope->level[count+1];
			pEnvelope->flags[count] = pEnvelope->flags[count+1];
			pEnvelope->time[count] = pEnvelope->time[count+1];
		}
	}
}

void XAddDefaultADSREnvelope(XInstrumentData *pXInstrument, XEnvelopeType type)
{
	int32_t		count;

	count = XFindType(pXInstrument, INST_ADSR_ENVELOPE);
	if (count == -1)
	{
		// not there, so use first slot
		count = pXInstrument->unitCount;
		pXInstrument->unitCount++;
	}
	pXInstrument->units[count].unitType = INST_ADSR_ENVELOPE;
	XFillDefaultADSREnvelope(&pXInstrument->units[count].u.envelopeADSR, type);
}

XInstrumentData *XCreateXInstrument(InstrumentResource *theX, uint32_t theXSize)
{
	return XCreateXInstrumentEx(theX, theXSize, TRUE);
}

/* Packed on-disk INST layout (matches GenPatch KEY_SPLIT_FILE_SIZE). */
enum
{
	kXInstOffset_flags1 = 5,
	kXInstOffset_keySplitCount = 12,
	kXInstKeySplitFileSize = 8
};

XInstrumentData *XCreateXInstrumentEx(InstrumentResource *theX, uint32_t theXSize,
										bool prepareForEditing)
{
	int32_t				count, count2;
	int32_t				size, unitCount, unitSubCount;
	XUnitType			unitType;
	unsigned short int	data;
	char 				*pData, *pData2;
	char 				*pUnit;
	unsigned char const	*pBase;
	XLFOData			*pLFO;
	XEnvelopeData		*pENV;
	XInstrumentData		*pXInstrument;
	XTieToData			*pCurve;
	int32_t				parsedUnits;
	int16_t				keySplitCount;

	pXInstrument = (XInstrumentData *)XNewPtr((int32_t)sizeof(XInstrumentData));
	if (pXInstrument)
	{
		pXInstrument->unitCount = 0;
		pXInstrument->unitBlockPresent = FALSE;

		pUnit = NULL;
		size = theXSize;
		if (theX && size)
		{
			pBase = (unsigned char const *)theX;
			if (pBase[kXInstOffset_flags1] & ZBF_extendedFormat)
			{
				// search for end of tremlo data $8000. If not there, don't walk past end of instrument
				keySplitCount = (int16_t)XGetShort(pBase + kXInstOffset_keySplitCount);
				if (keySplitCount < 0)
				{
					keySplitCount = 0;
				}
				pData = (char *)(pBase + kXInstOffset_keySplitCount + sizeof(int16_t) +
								 ((int32_t)keySplitCount * kXInstKeySplitFileSize));
				pData2 = (char *)pBase;
				size -= (pData - pData2);
				for (count = 0; count < size; count++)
				{
					data = XGetShort(&pData[count]);
					if (data == 0x8000)
					{
						count += 4;								// skip past end token and extra word
						data = (unsigned short)pData[count] + 1;			// get first string length;
						count2 = (long)pData[count+data] + 1;			// get second string length
						pUnit = (char *) (&pData[count + data + count2]);
						// NOTE: src will be non aligned, possibly on a byte boundry.
						break;
					}
				}
				if (pUnit)
				{
					pXInstrument->unitBlockPresent = TRUE;
					pUnit += 12;		// reserved global space

					unitCount = *pUnit;		// how many unit records?
					pUnit++;					// byte
					parsedUnits = 0;
					if (unitCount)
					{
						for (count = 0; count < unitCount; count++)
						{
							unitType = (XUnitType)XGetLong(pUnit) & 0x5F5F5F5F;
							pUnit += 4;	// long
							pXInstrument->units[parsedUnits].unitType = unitType;
							pXInstrument->units[parsedUnits].unitID = (uint32_t)parsedUnits;
							switch (unitType)
							{
								case INST_ADSR_ENVELOPE:
									unitSubCount = *pUnit;		// how many unit records?
									pUnit++;					// byte
									if (unitSubCount > ADSR_STAGES)
									{	// can't have more than ADSR_STAGES stages
										XDisposePtr(pXInstrument);
										pXInstrument = NULL;
										goto bailoninstrument;
									}
									pENV = &pXInstrument->units[parsedUnits].u.envelopeADSR;
									XSetMemory(pENV, (int32_t)sizeof(*pENV), 0);
									pENV->stageCount = unitSubCount;
									for (count2 = 0; count2 < unitSubCount; count2++)
									{
										pENV->level[count2] = XGetLong(pUnit);
										pUnit += 4;

										pENV->time[count2] = XGetLong(pUnit);
										pUnit += 4;

										pENV->flags[count2] = XGetLong(pUnit) & 0x5F5F5F5F;
										pUnit += 4;
									}

									if (prepareForEditing)
									{
										XAddZeroEnvelope(pENV);		// add extra 0 time point for editing
									}
									parsedUnits++;
									break;

								case INST_DEFAULT_MOD:
									pXInstrument->units[parsedUnits].u.useDefaultModwheelAction = TRUE;
									parsedUnits++;
									break;

								/* NeoBAE send units (also handled in GenPatch); needed for ZMF banks. */
								case INST_REVERB_SEND:
								case INST_CHORUS_SEND:
									pXInstrument->units[parsedUnits].u.sendAmount = (int32_t)XGetLong(pUnit);
									pUnit += 4;
									parsedUnits++;
									break;

								case INST_EXPONENTIAL_CURVE:					// curve entry
									pCurve = &pXInstrument->units[parsedUnits].u.curve;
									XSetMemory(pCurve, (int32_t)sizeof(*pCurve), 0);
									pCurve->tieFrom = XGetLong(pUnit);
									pUnit += 4;
									pCurve->tieTo = XGetLong(pUnit);
									pUnit += 4;
									unitSubCount = *pUnit++;
									if (unitSubCount > X_INSTRUMENT_MAX_CURVE_POINTS)
									{
										XDisposePtr(pXInstrument);
										pXInstrument = NULL;
										goto bailoninstrument;
									}
									pCurve->curveCount = (int16_t)unitSubCount;
									for (count2 = 0; count2 < unitSubCount; count2++)
									{
										pCurve->from_Value[count2] = *pUnit++;
										pCurve->to_Scalar[count2] = XGetShort(pUnit);
										pUnit += 2;
									}
									parsedUnits++;
									break;

								case INST_LOW_PASS_FILTER:		// low pass global filter parameters
									pXInstrument->units[parsedUnits].u.lpf.LPF_frequency = XGetLong(pUnit);
									pUnit += 4;
									pXInstrument->units[parsedUnits].u.lpf.LPF_resonance = XGetLong(pUnit);
									pUnit += 4;
									pXInstrument->units[parsedUnits].u.lpf.LPF_lowpassAmount = XGetLong(pUnit);
									pUnit += 4;
									parsedUnits++;
									break;

#if USE_ZMF_SUPPORT == TRUE
								case INST_OSCILLATOR:
									pXInstrument->units[parsedUnits].u.osc.waveShape = XGetLong(pUnit) & 0x5F5F5F5F;
									pUnit += 4;
									pXInstrument->units[parsedUnits].u.osc.pulseWidth = (int32_t)XGetLong(pUnit);
									pUnit += 4;
									/* Optional third DLNG: volume (0..65536). Absent in older OSCL. */
									{
										int32_t peekVol = (int32_t)XGetLong(pUnit);
										if (peekVol >= 0 && peekVol <= 65536)
										{
											pXInstrument->units[parsedUnits].u.osc.volume = peekVol;
											pUnit += 4;
										}
										else
											pXInstrument->units[parsedUnits].u.osc.volume = OSC_VOLUME_DEFAULT;
									}
									parsedUnits++;
									break;
#endif

								// LFO types
								case INST_VOLUME_LFO:
								case INST_PITCH_LFO:
								case INST_STEREO_PAN_LFO:
								case INST_STEREO_PAN_NAME2:
								case INST_LOW_PASS_AMOUNT:
								case INST_LPF_DEPTH:
								case INST_LPF_FREQUENCY:
#if USE_ZMF_SUPPORT == TRUE
								case INST_PULSE_WIDTH_LFO:
								case INST_WAVE_INDEX_LFO:
#endif
									unitSubCount = *pUnit;		// how many unit records?
									pUnit++;					// byte
									if (unitSubCount > ADSR_STAGES)
									{	// can't have more than ADSR_STAGES stages
										XDisposePtr(pXInstrument);
										pXInstrument = NULL;
										goto bailoninstrument;
									}
									pLFO = &pXInstrument->units[parsedUnits].u.lfo;
									XSetMemory(pLFO, (int32_t)sizeof(*pLFO), 0);
									pLFO->envelopeLFO.stageCount = unitSubCount;
									for (count2 = 0; count2 < unitSubCount; count2++)
									{
										pLFO->envelopeLFO.level[count2] = XGetLong(pUnit);
										pUnit += 4;
										pLFO->envelopeLFO.time[count2] = XGetLong(pUnit);
										pUnit += 4;
										pLFO->envelopeLFO.flags[count2] = XGetLong(pUnit) & 0x5F5F5F5F;
										pUnit += 4;
									}
									if (prepareForEditing)
									{
										XAddZeroEnvelope(&pLFO->envelopeLFO);		// add extra 0 time point for editing
									}
									pLFO->period = XGetLong(pUnit);
									pUnit += 4;
									pLFO->waveShape = XGetLong(pUnit);
									pUnit += 4;
									pLFO->DC_feed = XGetLong(pUnit);
									pUnit += 4;
									pLFO->depth = XGetLong(pUnit);
									pUnit += 4;
									parsedUnits++;
									break;

								default:
									/* Unknown unit type: size unknown, cannot skip safely.
									 * BE2 sometimes overstates unitCount and pads with zeros
									 * after real units (e.g. Bank 2 customs with ADSR only).
									 * Keep units already parsed instead of discarding all. */
									if (parsedUnits > 0)
									{
										goto units_done;
									}
									XDisposePtr(pXInstrument);
									pXInstrument = NULL;
									goto bailoninstrument;
							}
						}
					}
units_done:
					pXInstrument->unitCount = parsedUnits;
				}
			}
		}
	}
bailoninstrument:
	return pXInstrument;
}

/* Large enough for ADSR_STAGES envelopes / LFOs / curves in one INST. */
#define TEMP_INSTRUMENT_BUILD_SPACE		16384

/* Write reservedBytes + unitCount + units into *ioPtr; advances *ioPtr.
 * When stripEditPadding is TRUE, removes synthetic zero-envelope edit points. */
static int32_t PV_WriteInstrumentUnits(XInstrumentData *pXInstrument,
										char **ioPtr,
										int32_t reservedBytes,
										bool stripEditPadding)
{
	char				*pUnit;
	int32_t				count, count2;
	int32_t				unitCount, unitSubCount;
	XUnitType			unitType;
	XLFOData			*pLFO;
	XEnvelopeData		*pENV;
	XTieToData			*pCurve;
	int32_t				i;

	if (!pXInstrument || !ioPtr || !*ioPtr || reservedBytes < 0)
	{
		return -1;
	}

	pUnit = *ioPtr;
	for (i = 0; i < reservedBytes; i++)
	{
		*pUnit++ = 0;
	}

	unitCount = pXInstrument->unitCount;
	*pUnit = (char)unitCount;
	pUnit++;					// byte
	if (unitCount)
	{
		for (count = 0; count < unitCount; count++)
		{
			unitType = pXInstrument->units[count].unitType;
			XPutLong(pUnit, unitType);
			pUnit += 4;	// long
			switch (unitType)
			{
				case INST_ADSR_ENVELOPE:
					pENV = &pXInstrument->units[count].u.envelopeADSR;

					if (stripEditPadding)
					{
						XRemoveZeroEnvelope(pENV);			// remove extra 0 time point from editing
					}

					unitSubCount = pENV->stageCount;		// how many unit records?
					*pUnit = (char)unitSubCount;
					pUnit++;					// byte
					for (count2 = 0; count2 < unitSubCount; count2++)
					{
						XPutLong(pUnit, pENV->level[count2]);
						pUnit += 4;

						XPutLong(pUnit, pENV->time[count2]);
						pUnit += 4;

						XPutLong(pUnit, pENV->flags[count2]);
						pUnit += 4;
					}
					break;

				case INST_DEFAULT_MOD:
					/* tag only */
					break;

				case INST_REVERB_SEND:
				case INST_CHORUS_SEND:
					XPutLong(pUnit, pXInstrument->units[count].u.sendAmount);
					pUnit += 4;
					break;

				case INST_EXPONENTIAL_CURVE:					// curve entry
					pCurve = &pXInstrument->units[count].u.curve;
					XPutLong(pUnit, pCurve->tieFrom);
					pUnit += 4;
					XPutLong(pUnit, pCurve->tieTo);
					pUnit += 4;
					unitSubCount = pCurve->curveCount;
					if (unitSubCount > X_INSTRUMENT_MAX_CURVE_POINTS)
					{
						unitSubCount = X_INSTRUMENT_MAX_CURVE_POINTS;
					}
					*pUnit++ = (char)unitSubCount;

					for (count2 = 0; count2 < unitSubCount; count2++)
					{
						*pUnit++ = pCurve->from_Value[count2];
						XPutShort(pUnit, pCurve->to_Scalar[count2]);
						pUnit += 2;
					}
					break;

				case INST_LOW_PASS_FILTER:		// low pass global filter parameters
					XPutLong(pUnit, pXInstrument->units[count].u.lpf.LPF_frequency);
					pUnit += 4;
					XPutLong(pUnit,pXInstrument->units[count].u.lpf.LPF_resonance);
					pUnit += 4;
					XPutLong(pUnit, pXInstrument->units[count].u.lpf.LPF_lowpassAmount);
					pUnit += 4;
					break;

#if USE_ZMF_SUPPORT == TRUE
				case INST_OSCILLATOR:
					XPutLong(pUnit, pXInstrument->units[count].u.osc.waveShape);
					pUnit += 4;
					XPutLong(pUnit, pXInstrument->units[count].u.osc.pulseWidth);
					pUnit += 4;
					{
						int32_t vol = pXInstrument->units[count].u.osc.volume;
						if (vol < 0)
							vol = 0;
						if (vol > 65536)
							vol = 65536;
						/* Always write volume so mute (0) round-trips; readers peek 0..65536. */
						XPutLong(pUnit, (uint32_t)vol);
						pUnit += 4;
					}
					break;
#endif

				// LFO types
				case INST_VOLUME_LFO:
				case INST_PITCH_LFO:
				case INST_STEREO_PAN_LFO:
				case INST_STEREO_PAN_NAME2:
				case INST_LOW_PASS_AMOUNT:
				case INST_LPF_DEPTH:
				case INST_LPF_FREQUENCY:
#if USE_ZMF_SUPPORT == TRUE
				case INST_PULSE_WIDTH_LFO:
				case INST_WAVE_INDEX_LFO:
#endif
					pLFO = &pXInstrument->units[count].u.lfo;
					if (stripEditPadding)
					{
						XRemoveZeroEnvelope(&pLFO->envelopeLFO);		// remove extra 0 time point from editing
					}
					unitSubCount = pLFO->envelopeLFO.stageCount;
					*pUnit = (char)unitSubCount;
					pUnit++;					// byte
					for (count2 = 0; count2 < unitSubCount; count2++)
					{
						XPutLong(pUnit, pLFO->envelopeLFO.level[count2]);
						pUnit += 4;
						XPutLong(pUnit, pLFO->envelopeLFO.time[count2]);
						pUnit += 4;
						XPutLong(pUnit, pLFO->envelopeLFO.flags[count2]);
						pUnit += 4;
					}

					XPutLong(pUnit, pLFO->period);
					pUnit += 4;
					XPutLong(pUnit, pLFO->waveShape);
					pUnit += 4;
					XPutLong(pUnit, pLFO->DC_feed);
					pUnit += 4;
					XPutLong(pUnit, pLFO->depth);
					pUnit += 4;
					break;

				default:
					/* Skip unknown units on write rather than aborting the whole build. */
					break;
			}
		}
	}

	*ioPtr = pUnit;
	return 0;
}

XPTR XSerializeInstrumentUnits(XInstrumentData const* pXInstrument,
								int32_t reservedBytes,
								int32_t* outSize)
{
	char				*tempSpace;
	char				*pUnit;
	int32_t				tempSpaceSize;
	XPTR				result;
	XInstrumentData		mutableCopy;

	if (outSize)
	{
		*outSize = 0;
	}
	if (!pXInstrument || !outSize || pXInstrument->unitCount <= 0 || reservedBytes < 0)
	{
		return NULL;
	}

	/* Copy so stripEditPadding can mutate envelopes without touching the caller's model. */
	mutableCopy = *pXInstrument;

	tempSpace = (char *)XNewPtr(TEMP_INSTRUMENT_BUILD_SPACE);
	if (!tempSpace)
	{
		return NULL;
	}

	pUnit = tempSpace;
	if (PV_WriteInstrumentUnits(&mutableCopy, &pUnit, reservedBytes, FALSE) != 0)
	{
		XDisposePtr(tempSpace);
		return NULL;
	}

	tempSpaceSize = (int32_t)(pUnit - tempSpace);
	result = XNewPtr(tempSpaceSize);
	if (result)
	{
		XBlockMove(tempSpace, result, tempSpaceSize);
		*outSize = tempSpaceSize;
	}
	XDisposePtr(tempSpace);
	return result;
}

// pass in original instrument resource (theX) its size (theXSize) and the structure XInstrumentData (pXInstrument)
// to build a new instrument resource. The variable theX is not touched, and you can deallocate it after this
// function is used.
InstrumentResource * XReconstructInstrument(InstrumentResource *theX, uint32_t theXSize,
								XInstrumentData *pXInstrument)
{
	InstrumentResource	*newInstrument;
	int32_t				count, count2;
	int32_t				size;
	uint32_t		offset;
	unsigned short int	data;
	char 				*pData, *pData2;
	char 				*pUnit;
	char				*tempSpace;		// used to build the X instrument stuff before copying
	int32_t				tempSpaceSize;
	XInstrumentData		mutableCopy;

	newInstrument = NULL;
	if (!pXInstrument)
	{
		return NULL;
	}

	mutableCopy = *pXInstrument;
	tempSpace = (char *)XNewPtr(TEMP_INSTRUMENT_BUILD_SPACE);
	if (tempSpace)
	{
		pUnit = tempSpace;
		if (PV_WriteInstrumentUnits(&mutableCopy, &pUnit, 12, TRUE) != 0)
		{
			XDisposePtr(tempSpace);
			return NULL;
		}

		if (theX)
		{
			tempSpaceSize = (int32_t)(pUnit - tempSpace);		// determine size of new block of units

			// search for end of tremlo data $8000. If not there, don't walk past end of instrument
			{
				unsigned char const *pBase = (unsigned char const *)theX;
				int16_t keySplitCount = (int16_t)XGetShort(pBase + kXInstOffset_keySplitCount);
				if (keySplitCount < 0)
				{
					keySplitCount = 0;
				}
				pData = (char *)(pBase + kXInstOffset_keySplitCount + sizeof(int16_t) +
								 ((int32_t)keySplitCount * kXInstKeySplitFileSize));
			}
			pData2 = (char *)theX;
			size = (int32_t)(pData - pData2);
			offset = 0;
			for (count = 0; count < size; count++)
			{
				data = XGetShort(&pData[count]);
				if (data == 0x8000)
				{
					count += 4;								// skip past end token and extra word
					data = (unsigned short)pData[count] + 1;			// get first string length;
					count2 = (long)pData[count+data] + 1;			// get second string length
					pUnit = (char *) (&pData[count + data + count2]);
					offset = (uint32_t)(pUnit - (char *)theX);
					// NOTE: src will be non aligned, possibly on a byte boundry.
					break;
				}
			}
			if (offset == 0L)
			{
				// somethings bad so bail
				XDisposePtr((XPTR)newInstrument);
				newInstrument = NULL;
			}
			else
			{
				if (offset < theXSize)
				{
					newInstrument = (InstrumentResource *)XNewPtr(offset + tempSpaceSize);
					if (newInstrument)
					{
						XBlockMove((XPTR)theX, (XPTR)newInstrument, offset);
						// tack onto end the new block of information
						XBlockMove(tempSpace, (char *)newInstrument + offset, tempSpaceSize);

						newInstrument->flags1 |= ZBF_extendedFormat;
					}
				}
				else
				{
					// too big!?
					XDisposePtr((XPTR)newInstrument);
					newInstrument = NULL;
				}
			}
		}
	}
	XDisposePtr(tempSpace);
	return newInstrument;
}

#endif	// USE_CREATION_API == TRUE

// EOF of X_Instruments.c



