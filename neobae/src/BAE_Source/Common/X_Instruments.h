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
**	X_Instruments.h
**
**	Tools for creating instruments. The structure enclosed here are used
**	for create expanded editable structures.
**
**	(c) Copyright 1996-1999 Beatnik, Inc, All Rights Reserved.
**	Written by Steve Hales
*/
/*****************************************************************************/
#ifndef X_INSTRUMENTS
#define X_INSTRUMENTS

#ifndef __X_API__
	#include "X_API.h"
#endif

#ifndef X_FORMATS
	#include "X_Formats.h"
#endif

#ifndef G_SOUND
	#include "GenSnd.h"
#endif


#ifdef __cplusplus
	extern "C" {
#endif

#define FULL_RANGE				(VOLUME_RANGE * 2)
#define SUSTAIN_DEFAULT_TIME	45000

/* Max curve control points in the editable instrument model.
 * Classic Beatnik/GM_TieTo used MAX_CURVES (4). Creation/editor builds expand
 * to ADSR_STAGES so ZMF instruments with longer curve tables can round-trip. */
#if USE_CREATION_API == TRUE
	#define X_INSTRUMENT_MAX_CURVE_POINTS	ADSR_STAGES
#else
	#define X_INSTRUMENT_MAX_CURVE_POINTS	MAX_CURVES
#endif


// Basic envelope structure used for LFO's and ADSR's
typedef struct
{
// Used for display
	int32_t		startScaleH;
	int32_t		endScaleH;

	int32_t		startScaleV;
	int32_t		endScaleV;

	int32_t		startRangeV;
	int32_t		endRangeV;

	int32_t		stageCount;
	int32_t		level[ADSR_STAGES+1];
	int32_t		time[ADSR_STAGES+1];
	int32_t		flags[ADSR_STAGES+1];
} XEnvelopeData;

/* Editable curve unit. Distinct from GM_TieTo so Creation/ZMF builds can
 * store more than MAX_CURVES control points. tieFrom/tieTo stay in FOUR_CHAR
 * (file) form — no PV_TranslateFromFileToMemoryID. */
typedef struct
{
	int32_t			tieFrom;
	int32_t			tieTo;
	int16_t			curveCount;
	unsigned char	from_Value[X_INSTRUMENT_MAX_CURVE_POINTS];
	int16_t			to_Scalar[X_INSTRUMENT_MAX_CURVE_POINTS];
} XTieToData;

typedef struct
{
	XEnvelopeData	envelopeLFO;

	int32_t			period;
	int32_t			waveShape;
	int32_t			DC_feed;			// amount to use as ADSR
	int32_t			depth;				// amount to use as LFO
} XLFOData;

typedef struct
{
	int32_t			LPF_frequency;
	int32_t			LPF_resonance;
	int32_t			LPF_lowpassAmount;
} XLowPassFilterData;

// Use one of these INST_XXXX for unitType in the structure XUnitData below. When there
// are multiple LFO types use a different unitID for tracking.
/*
	INST_ADSR_ENVELOPE
	INST_EXPONENTIAL_CURVE
	INST_LOW_PASS_FILTER
	INST_DEFAULT_MOD
	INST_REVERB_SEND
	INST_CHORUS_SEND
	INST_PITCH_LFO
	INST_VOLUME_LFO
	INST_STEREO_PAN_LFO
	INST_STEREO_PAN_NAME2
	INST_LOW_PASS_AMOUNT
	INST_LPF_DEPTH
	INST_LPF_FREQUENCY
*/
typedef uint32_t XUnitType;

typedef struct
{
	XUnitType		unitType;
	uint32_t		unitID;

	union	
	{
		XEnvelopeData		envelopeADSR;
		XLFOData			lfo;
		XLowPassFilterData	lpf;
		XTieToData			curve;
		bool				useDefaultModwheelAction;
		int32_t				sendAmount;		/* INST_REVERB_SEND / INST_CHORUS_SEND */
	} u;
} XUnitData;

// The XInstrumentData structure is a deconstruction of the InstrumentResource structure
typedef struct
{
// how many units in instrument, only used for reconstruction
	int32_t			unitCount;
/* TRUE if an extended unit block was located after the tremolo/name trailer. */
	bool			unitBlockPresent;
// seperate unit information:
	XUnitData		units[256];
}  XInstrumentData;

typedef enum XEnvelopeType
{
	NONE_E			=	0,
	FOUR_POINT_E,
	TWO_POINT_E,
	FLAT_FULL_E
} XEnvelopeType;

InstrumentResource*	XNewInstrumentResource(XShortResourceID leadSndID);
InstrumentResource* XNewInstrumentWithBasicEnvelopeResource(XShortResourceID leadSndID, XEnvelopeType type);
void				XDisposeInstrumentResource(InstrumentResource* theX);

/* Create editable unit list from an INST resource.
 * prepareForEditing=TRUE adds synthetic zero envelope points for UI editing
 * (classic Beatnik Editor behaviour). Pass FALSE for lossless parse/serialize. */
XInstrumentData*	XCreateXInstrumentEx(InstrumentResource* theX,
										  uint32_t theXSize,
										  bool prepareForEditing);

/* Same as XCreateXInstrumentEx(..., TRUE). */
XInstrumentData*	XCreateXInstrument(InstrumentResource* theX,
										uint32_t theXSize);

// pass in original instrument resource (theX) its size (theXSize) and the structure XInstrumentData (pXInstrument)
// to build a new instrument resource. The variable theX is not touched, and you can deallocate it after this
// function is used.
InstrumentResource*	XReconstructInstrument(InstrumentResource* theX,
											uint32_t theXSize,
											XInstrumentData* pXInstrument);

/* Serialize only the unit block (reserved prefix + unitCount + units).
 * reservedBytes is typically 12 (XReconstruct) or 10 (BAE_EditorAPI append path).
 * Caller frees with XDisposePtr. Returns NULL if there are no units. */
XPTR				XSerializeInstrumentUnits(XInstrumentData const* pXInstrument,
											   int32_t reservedBytes,
											   int32_t* outSize);

int32_t				XGetTotalEnvelopeTime(XEnvelopeData* pXEnvelope);

void				XEnvelopeAdjustSustainTime(XEnvelopeData* pXEnvelope);

int32_t				XFindType(XInstrumentData* pXInstrument, XUnitType type);
int32_t				XAddType(XInstrumentData* pXInstrument, XUnitType type);
void				XRemoveType(XInstrumentData* pXInstrument,
								XUnitType unitType,
								uint32_t unitID);

bool				XDeleteEnvelopePoint(XEnvelopeData* pEnvelope, int16_t whichPoint);

// Given an envelope, rebuild the ADSR type to one of XEnvelopeType type
void				XFillDefaultADSREnvelope(XEnvelopeData* pEnvelope, XEnvelopeType type);

// Given an envelope, shift volume plus or minus 
void				XShiftEnvelopeVolume(XEnvelopeData* pEnvelope, int32_t shift);

// add extra 0 time point for editing
void				XAddZeroEnvelope(XEnvelopeData* pEnvelope);

// remove extra 0 time point from editing
void				XRemoveZeroEnvelope(XEnvelopeData* pEnvelope);

void				XAddDefaultADSREnvelope(XInstrumentData* pXInstrument, XEnvelopeType type);


#ifdef __cplusplus
	}
#endif


#endif	// X_INSTRUMENTS
// EOF of X_Instruments.h
