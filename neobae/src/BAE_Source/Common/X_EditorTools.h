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
**	X_EditorTools.h
**
**	Tools for editors create and manipulating RMF data
**
**	(c) Copyright 1998-1999 Beatnik, Inc, All Rights Reserved.
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
**	12/16/98	Created. Pulled from MacOS specific editor codebase
**	2/5/98		Added XCopySongMidiResources & XCopyInstrumentResources & XCopySndResources
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
**	3/16/99		MOE:  Changed parameters of XCompressAndEncrypt()
**	3/25/99		MOE:  Added procData parameter to functions using XCompressStatusProc
**	5/15/99		Added XRemoveThisKeySplit
*/
/*****************************************************************************/
#ifndef X_EDITOR_TOOLS
#define X_EDITOR_TOOLS

#ifndef __X_API__
	#include "X_API.h"
#endif

#ifndef X_FORMATS
	#include "X_Formats.h"
#endif

#ifndef G_SOUND
	#include "GenSnd.h"
	#include "GenPriv.h"
#endif

#ifdef __cplusplus
	extern "C" {
#endif


// Utillities for instruments
bool XIsSoundUsedInInstrument(InstrumentResource *theX, XShortResourceID sampleSoundID);
void XRenumberSampleInsideInstrument(InstrumentResource *theX, XShortResourceID originalSampleID, 
																XShortResourceID newSampleID);
int16_t XCollectSoundsFromInstrument(InstrumentResource *theX, XShortResourceID *sndArray, int16_t maxArraySize);
int16_t XCollectSoundsFromInstrumentID(XShortResourceID theID, XShortResourceID *sndArray, int16_t maxArraySize);
// Verifiy each keysplit sample by accessing it through the currently open resource file chain, then
// remove sample references that are bad. This will return a new instrument.
InstrumentResource * XRemoveUnusedSampleKeysplitFromInstrument(InstrumentResource *theX, int32_t instrumentSize);
bool XCheckAllInstruments(XShortResourceID *badInstrument, XShortResourceID *badSnd);
XShortResourceID XCheckValidInstrument(XShortResourceID theID);
int16_t XGetInstrumentArray(XShortResourceID *instArray, int16_t maxArraySize);

int16_t XGatherAllSoundsFromAllInstruments(XShortResourceID *pSndArray, int16_t maxArraySize);

bool XIsSampleUsedInAllInstruments(XShortResourceID soundSampleID, XShortResourceID *pWhichInstrument);

int16_t XGetTotalKeysplits(XShortResourceID *instArray, int16_t totalInstruments, 
								XShortResourceID *sndArray, int16_t totalSnds);

// Given a song ID and two arrays, this will return the INST resources ID and the 'snd ' resource ID
// that are needed to load the song terminated with a -1.
// Will return 0 for success or 1 for failure
OPErr XGetSongInstrumentList(XShortResourceID theSongID, XShortResourceID *pInstArray, int16_t maxInstArraySize, 
										XShortResourceID *pSndArray, int16_t maxSndArraySize);

int16_t XGetSamplesFromInstruments(XShortResourceID *pInstArray, int16_t maxInstArraySize, 
										XShortResourceID *pSndArray, int16_t maxSndArraySize);

void XSetKeySplitFromPtr(InstrumentResource *theX, int16_t entry, KeySplit *keysplit);

InstrumentResource * XAddKeySplit(InstrumentResource *theX, int16_t howMany);
InstrumentResource * XRemoveKeySplit(InstrumentResource *theX, int16_t howMany);
// Remove specific key split.
InstrumentResource * XRemoveThisKeySplit(InstrumentResource *theX, int16_t entry);

// returns >0 if successful, 0 if aborted, -1 if failed
int32_t XCompressAndEncrypt(XPTR* newData, XPTR pData, uint32_t size,
							XCompressStatusProc proc, void* procData);

int32_t XGetSongTempoFactor(SongResource *pSong);
void XSetSongTempoFactor(SongResource *pSong, int32_t newTempo);

// allocate and return an list of ID's collected from ID_SND, ID_CSND, ID_ESND. pCount will
// be the number of ID's, and the int32_t array will be the list. use XDisposePtr on the return
// pointer
XLongResourceID * XGetAllSoundID(int32_t *pCount);

// This will return a MIDI/CMID/EMID/ECMI object from an open resource file
//
// INPUT:
//	theXSong		is the SongResource structure
//
// OUTPUT:
//	pMusicName		is a pascal string
//	pMusicType		is the resource type
//	pMusicID		is the resource ID
//	pReturnedSize			is the resource size
XPTR XGetMusicObjectFromSong(SongResource *theXSong, char *pMusicName, 
								XResourceType *pMusicType, XLongResourceID *pMusicID, int32_t *pReturnedSize);

int32_t XCopySongMidiResources(XLongResourceID theSongID, XFILE readFileRef, 
								XFILE writeFileRef, bool protect, bool copyNames);
int32_t XCopyInstrumentResources(XShortResourceID *pInstCopy, int16_t instCount, 
										XFILE readFileRef, XFILE writeFileRef, bool copyNames);
int32_t XCopySndResources(XShortResourceID *pSndCopy, int16_t sndCount, XFILE readFileRef, 
									XFILE writeFileRef, bool protect, bool copyNames);



/* NeoBAE extensions: compression helpers with codec gating.
 * Classic XCompressAndEncrypt always uses LZSS (X_RAW) + encrypt.
 */

/* Compress with an explicit XCOMPRESSION_TYPE, then encrypt. */
int32_t XCompressAndEncryptWithType(XPTR* newData, XPTR pData, uint32_t size,
                                    XCOMPRESSION_TYPE type,
                                    XCompressStatusProc proc, void* procData);

#if USE_LZMA_COMPRESSION == TRUE
/* Convenience: LZMA compress + encrypt when LZMA support is compiled in. */
int32_t XCompressAndEncryptLZMA(XPTR* newData, XPTR pData, uint32_t size,
                                XCompressStatusProc proc, void* procData);
#endif

/* Create an encoded 'snd' object. Rejects compression types not compiled in. */
OPErr XCreateEncodedSoundObject(XPTR* dst,
                                GM_Waveform const* src,
                                SndCompressionType compression,
                                SndCompressionSubType subType,
                                XCompressStatusProc proc, void* procData);

// Test API's
void XTestCompression(XPTR compressedAndEncryptedData, int32_t size, XPTR originalData, int32_t originalSize);

#ifdef __cplusplus
	}
#endif


#endif	// X_EDITOR_TOOLS
// EOF of X_EditorTools.h


