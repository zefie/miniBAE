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
**	X_EditorTools.c
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
**				Moved editor specific cross platform code from DriverTools.c
**	1/13/99		Changed XGatherAllSoundsFromAllInstruments & XGetSamplesFromInstruments
**				to allocate more memory
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
**	3/16/99		MOE:  Changed XCompressAndEncrypt() to use new XCompressPtr() parameters
**	3/25/99		MOE:  Added procData parameter to functions using XCompressStatusProc
**	5/15/99		Optimized XCheckValidInstrument by moving the sound searching smarts into
**				a seperate function
**				Added XRemoveUnusedSampleKeysplitFromInstrument
*/
/*****************************************************************************/

#include "X_EditorTools.h"


#if USE_CREATION_API == TRUE

// Set a keysplit entry. The result will be ordered for the 68k CPU
void XSetKeySplitFromPtr(InstrumentResource *theX, int16_t entry, KeySplit *keysplit)
{
	KeySplit	*pSplits;
	short int	count;

	count = (short)XGetShort(&theX->keySplitCount);
	if ( (count) && (entry < count) )
	{
		pSplits = (KeySplit *) ( ((unsigned char *)&theX->keySplitCount) + sizeof(int16_t));
		pSplits[entry] = *keysplit;

		XPutShort(&pSplits[entry].sndResourceID, (unsigned short)keysplit->sndResourceID);
		XPutShort(&pSplits[entry].miscParameter1, (unsigned short)keysplit->miscParameter1);
		XPutShort(&pSplits[entry].miscParameter2, (unsigned short)keysplit->miscParameter2);
	}
	else
	{
		XSetMemory(keysplit, (int32_t)sizeof(KeySplit), 0);
	}
}

// Increase the number of key splits. This will not clean the entry's that are added.
// This will add from the end. This will freeup the passed InstrumentResource
InstrumentResource * XAddKeySplit(InstrumentResource *theX, int16_t howMany)
{
	int32_t				size, moveSize, size2;
	short int			total;
	KeySplit 			*pSplits;
	char				*pSrc, *pDest;
	InstrumentResource	*theNewX;

	theNewX = NULL;
	if (theX)
	{
		total = (short)XGetShort(&theX->keySplitCount);
		size = XGetPtrSize(theX);
		size2 =  (int32_t)(sizeof(KeySplit) * howMany);
		theNewX = (InstrumentResource *)XNewPtr(size2 + size);
		if (theNewX)
		{
			XBlockMove(theX, theNewX, size);	// make copy
			XPutShort(&theNewX->keySplitCount, (unsigned short)(total + howMany));
			// must move all zones down
			pSplits = (KeySplit *) ( ((unsigned char *)&theX->keySplitCount) + sizeof(uint16_t));
			pSrc = (char *)&pSplits[total];

			pSplits = (KeySplit *) ( ((unsigned char *)&theNewX->keySplitCount) + sizeof(uint16_t));
			pDest = (char *)&pSplits[total + howMany];
			moveSize =  size - (pSrc - (char *)theX);
			// this move must handle overlapping data
			XBlockMove(pSrc, pDest, moveSize);
		}
	}
	return theNewX;
}


// Remove specific key split.
InstrumentResource * XRemoveThisKeySplit(InstrumentResource *theX, int16_t entry)
{
	short int			total, count, destCount;
	InstrumentResource	*theNewX;
	KeySplit			transfer;

	theNewX = NULL;
	if (theX)
	{
		total = (short)XGetShort(&theX->keySplitCount);
		if (entry > total)
		{
			entry = total;
		}
		theNewX = XRemoveKeySplit(theX, 1);		// remove one split
		if (theNewX)
		{
			// walk through all splits copying from source and dest, but skip the "entry"
			for (destCount = 0, count = 0; count < total; count++)
			{
				XGetKeySplitFromPtr(theX, count, &transfer);
				if (count != entry)
				{
					XSetKeySplitFromPtr(theNewX, destCount,  &transfer);
					destCount++;
				}
			}
		}
	}
	return theNewX;
}

// Decrease the number of key splits. This will delete from the end
InstrumentResource * XRemoveKeySplit(InstrumentResource *theX, int16_t howMany)
{
	int32_t				size, moveSize, size2;
	short int			total;
	KeySplit 			*pSplits;
	char				*pSrc, *pDest;
	InstrumentResource	*theNewX;

	theNewX = NULL;
	if (theX)
	{
		total = (short)XGetShort(&theX->keySplitCount);
		if (howMany > total)
		{
			howMany = total;
		}

		size = XGetPtrSize(theX);
		size2 =  (int32_t)(sizeof(KeySplit) * howMany);
		theNewX = (InstrumentResource *)XNewPtr(size - size2);
		if (theNewX)
		{
			XBlockMove(theX, theNewX, size - size2);	// make copy
			XPutShort(&theNewX->keySplitCount, (unsigned short)(total - howMany));
			// must move all zones down
			pSplits = (KeySplit *) ( ((unsigned char *)&theX->keySplitCount) + sizeof(uint16_t));
			pSrc = (char *)&pSplits[total];		// go to byte at the end

			pSplits = (KeySplit *) ( ((unsigned char *)&theNewX->keySplitCount) + sizeof(uint16_t));
			pDest = (char *)&pSplits[total - howMany];

			moveSize =  size - (pSrc - (char *)theX);
			// this move must handle overlapping data
			XBlockMove(pSrc, pDest, moveSize);
		}
	}
	return theNewX;
}

/*
** Return the number of INST's and an array of resource IDs
*/
int16_t XGetInstrumentArray(XShortResourceID *instArray, int16_t maxArraySize)
{
	short int	icount, totalCount;
	int32_t		size;
	XPTR		theRes;

	/* Collect INST resources
	*/
	totalCount = 0;
	if (instArray)
	{
		for (icount = 0; icount < maxArraySize; icount++)
		{
			theRes = XGetAndDetachResource(ID_INST, icount, &size);
			if (theRes)
			{
				XDisposePtr(theRes);
				instArray[totalCount++] = (XShortResourceID)icount;
				if (totalCount > maxArraySize)
				{
					totalCount = maxArraySize;
					break;
				}
			}
		}
		XBubbleSortArray((int16_t *)instArray, (int16_t)totalCount);
	}
	return totalCount;
}

bool XIsSoundUsedInInstrument(InstrumentResource *theX, XShortResourceID sampleSoundID)
{
	XShortResourceID	sndArray[MAX_SAMPLES];		/* Max samples per instrument */
	short int			count, total;
	bool				used;

	used = FALSE;
	total = XCollectSoundsFromInstrument(theX, sndArray, MAX_SAMPLES);
	for (count = 0; count < total; count++)
	{
		if (sndArray[count] == sampleSoundID)
		{
			used = TRUE;
			break;
		}
	}
	return used;
}

// Renumber sample ID that is used in and instrument
void XRenumberSampleInsideInstrument(InstrumentResource *theX, XShortResourceID originalSampleID, 
																XShortResourceID newSampleID)
{
	short int	count, total;
	KeySplit	split;

	total = (int16_t)XGetShort(&theX->keySplitCount);
	if (total == 0)
	{
		XPutShort(&theX->sndResourceID, (unsigned short)newSampleID);
	}
	else
	{
		for (count = 0; count < total; count++)
		{
			XGetKeySplitFromPtr(theX, count, &split);
			if (split.sndResourceID == originalSampleID)
			{
				split.sndResourceID = newSampleID;
				XSetKeySplitFromPtr(theX, count, &split);
			}
		}
	}
}

/*
**	Given an INST resource, this will return an array of snd resources
**	that are used for this instrument
*/
int16_t XCollectSoundsFromInstrument(InstrumentResource *theX, XShortResourceID *sndArray, int16_t maxArraySize)
{
	short int			sndOutCount, sndCount, splitCount, count, count2;
	KeySplit			theSplit;
	XShortResourceID	countSndArray[MAX_SAMPLES];		/* Max samples per instrument */
	bool				goodSound;
	XShortResourceID	soundID;

	sndCount = 0;
	sndOutCount = 0;
	if (theX)
	{
		splitCount = (short)XGetShort(&theX->keySplitCount);
		if ( splitCount == 0)
		{
			if (maxArraySize > 1)
			{
				sndCount = 1;
				countSndArray[0] = (XShortResourceID)XGetShort(&theX->sndResourceID);
				if (countSndArray[0] == (XShortResourceID)-1)
				{
					sndCount = 0;
				}
			}
		}
		else
		{
			if (maxArraySize > splitCount)
			{
				sndCount = 1;
				countSndArray[0] = (XShortResourceID)XGetShort(&theX->sndResourceID);
				if (countSndArray[0] == (XShortResourceID)-1)
				{
					sndCount = 0;
				}
				for (count = 0; count < splitCount; count++)
				{
					XGetKeySplitFromPtr(theX, count, &theSplit);
					countSndArray[sndCount] = theSplit.sndResourceID;
					if (countSndArray[sndCount] != (XShortResourceID)-1)
					{
						sndCount++;
					}
				}
			}
		}
		XBubbleSortArray((int16_t *)countSndArray, (int16_t)sndCount);

// Remove duplicates
		sndOutCount = 0;
		for (count = 0; count < sndCount; count++)
		{
			goodSound = TRUE;
			soundID = countSndArray[count];
			for (count2 = 0; count2 < sndOutCount; count2++)
			{
				if (soundID == sndArray[count2])
				{
					goodSound = FALSE;
					break;
				}
			}
			if (goodSound)
			{
				sndArray[sndOutCount++] = soundID;
			}
		}
	}
	return sndOutCount;
}

int16_t XCollectSoundsFromInstrumentID(XShortResourceID theID, XShortResourceID *sndArray, int16_t maxArraySize)
{
	InstrumentResource	*theX;
	short int			totalSnds;
	int32_t				size;

	totalSnds = 0;
	theX = (InstrumentResource *)XGetAndDetachResource(ID_INST, theID, &size);
	if (theX)
	{
		totalSnds = XCollectSoundsFromInstrument(theX, sndArray, maxArraySize);
		XDisposePtr((XPTR)theX);
	}
	return totalSnds;
}

// Verifiy each keysplit sample by accessing it through the currently open resource file chain, then
// remove sample references that are bad. This will return a new instrument even if there's no change.
// You must call XDispose on the resulting data.
InstrumentResource * XRemoveUnusedSampleKeysplitFromInstrument(InstrumentResource *theX, int32_t instrumentSize)
{
	short int			count, total;
	InstrumentResource	*theNewX, *theOldX;
	KeySplit			keys;
	XPTR				theSnd;
	short int			badKeys[MAX_INSTRUMENTS];
	short int			badKeyCount;

	theNewX = NULL;
	if (theX && instrumentSize)
	{
		theNewX = (InstrumentResource *)XNewPtr(instrumentSize);
		if (theNewX)
		{
			XBlockMove((XPTR)theX, (XPTR)theNewX, instrumentSize);

			badKeyCount = 0;
			total = (short int)XGetShort(&theX->keySplitCount);
			if (total)
			{
				for (count = 0; count < total; count++)
				{
					XGetKeySplitFromPtr(theX, count, &keys);
					theSnd = XGetRawSoundResourceByID(keys.sndResourceID, NULL, NULL);		// look for samples, but don't decompress
					XDisposePtr(theSnd);
					if (theSnd == NULL)
					{	// bad keysplit, mark it
						badKeys[badKeyCount] = count;
						badKeyCount++;
					}
				}
				// walk backwards and remove bad splits
				theOldX = theNewX;
				for (count = badKeyCount-1; count >= 0; count--)
				{
					theNewX = XRemoveThisKeySplit(theOldX, badKeys[count]);
					XDisposePtr(theOldX);
					theOldX = theNewX;
				}
			}
		}
	}
	return theNewX;
}

XShortResourceID XCheckValidInstrument(XShortResourceID theID)
{
	InstrumentResource	*theX;
	short int			totalSnds;
	XShortResourceID	sndArray[MAX_SAMPLES];
	short int			count;
	XShortResourceID	badLoad;
	XPTR				theSnd;
	int32_t				size;

	badLoad = 0;
	theX = (InstrumentResource *)XGetAndDetachResource(ID_INST, theID, &size);
	if (theX)
	{
		totalSnds = XCollectSoundsFromInstrument(theX, sndArray, MAX_SAMPLES);
		XDisposePtr((XPTR)theX);
		for (count = 0; count < totalSnds; count++)
		{
			theSnd = XGetRawSoundResourceByID(sndArray[count], NULL, NULL);		// look for samples, but don't decompress
			if (theSnd == NULL)
			{
				badLoad = sndArray[count];
			}
			XDisposePtr(theSnd);
			if (badLoad)
			{
				break;
			}
		}
	}
	return badLoad;
}

bool XCheckAllInstruments(XShortResourceID *badInstrument, XShortResourceID *badSnd)
{
	short int			count;
	XShortResourceID	bad;
	bool				badLoad;
	XShortResourceID	instArray[MAX_INSTRUMENTS * MAX_BANKS];
	short int			totalInstruments;

	badLoad = FALSE;
	if (badInstrument && badSnd)
	{
		totalInstruments = XGetInstrumentArray(instArray, MAX_INSTRUMENTS * MAX_BANKS);
		if (totalInstruments)
		{
			for (count = 0; count < totalInstruments; count++)
			{
				bad = XCheckValidInstrument(instArray[count]);
				if (bad)
				{
					*badInstrument = instArray[count];
					*badSnd = bad;
					badLoad = TRUE;
					break;
				}
			}
		}
	}
	return badLoad;
}

// Scan an array and return index
static short int PV_ConvertResourceID2Index(XShortResourceID *sndArray, short totalSnds, XShortResourceID theID)
{
	register short int count, indexValue;
	
	indexValue = -1;
	/* convert resourceID into instrument index */
	for (count = 0; count < totalSnds; count++)
	{
		if (theID == sndArray[count])
		{
			indexValue = count;
			break;
		}
	}
	return indexValue;
}

// Given an array of instruments, this will return an array of SND resources that are required to load
// these instruments
short int XGetTotalKeysplits(XShortResourceID *instArray, short int totalInstruments, 
								XShortResourceID *sndArray, short int totalSnds)
{
	register short int	count, count2, count3, count4, keyCount;
	KeySplit			theSplit;
	InstrumentResource	*theInstrument;
	char				*pLoaded;
	int32_t				size;

	keyCount = 0;
	if (instArray && totalInstruments && sndArray && totalSnds)
	{
		pLoaded = (char *)XNewPtr((int32_t)sizeof(char) * totalSnds);
		if (pLoaded)
		{
			for (count = 0; count < totalInstruments; count++)
			{
				theInstrument = (InstrumentResource *)XGetAndDetachResource(ID_INST, instArray[count], &size);
				if (theInstrument)
				{
					count3 = (short)XGetShort(&theInstrument->keySplitCount);
					for (count2 = 0; count2 < count3; count2++)
					{
						XGetKeySplitFromPtr(theInstrument, count2, &theSplit);
						count4 = PV_ConvertResourceID2Index(sndArray, totalSnds, theSplit.sndResourceID);
						if (count4)
						{
							if (pLoaded[count4] == 0)
							{
								pLoaded[count4] = 1;
								keyCount++;
							}
						}
					}
				}
			}
			XDisposePtr((XPTR)pLoaded);
		}
	}
	return keyCount;
}

/*
**	This will walk through all Instrument resources and collect all snd resources
**	that are used.
*/
int16_t XGatherAllSoundsFromAllInstruments(XShortResourceID *pSndArray, int16_t maxArraySize)
{
	short int			count, icount, sndCount, jcount;
	XShortResourceID	soundID;
	bool				goodSound;
	XShortResourceID	instArray[MAX_INSTRUMENTS * MAX_BANKS];
	short int			totalInstruments;
	XShortResourceID	sndArray[MAX_SAMPLES];
	short int			totalSnds;
	XShortResourceID	*completeSndArray;
	short int			completeSndCount;
	InstrumentResource	*theX;
	int32_t				size;

	completeSndCount = 0;
	sndCount = 0;
	completeSndArray = (XShortResourceID *)XNewPtr(MAX_INSTRUMENTS * MAX_BANKS * 128L * sizeof(XShortResourceID));
	if (completeSndArray)
	{
		sndCount = 0;
		totalInstruments = XGetInstrumentArray(instArray, MAX_INSTRUMENTS * MAX_BANKS);
		if (totalInstruments)
		{
			for (count = 0; count < totalInstruments; count++)
			{
				theX = (InstrumentResource *)XGetAndDetachResource(ID_INST, instArray[count], &size);
				if (theX)
				{
					totalSnds = XCollectSoundsFromInstrument(theX, sndArray, MAX_SAMPLES);
					XDisposePtr((XPTR)theX);

					for (icount = 0; icount < totalSnds; icount++)
					{
						completeSndArray[completeSndCount++] = sndArray[icount];
					}
				}
			}
			XBubbleSortArray((int16_t *)completeSndArray, (int16_t)completeSndCount);

// Remove duplicates
			for (jcount = 0; jcount < completeSndCount; jcount++)
			{
				goodSound = TRUE;
				soundID = completeSndArray[jcount];
				for (count = 0; count < sndCount; count++)
				{
					if (soundID == pSndArray[count])
					{
						goodSound = FALSE;
						break;
					}
				}
				if (goodSound)
				{
					if (sndCount < maxArraySize)
					{
						pSndArray[sndCount++] = soundID;
					}
				}
			}
		}
		XDisposePtr((XPTR)completeSndArray);
	}
	return sndCount;
}

// Given a list of instruments, this will return the sample ID's that are required to load
// all of these instruments
int16_t XGetSamplesFromInstruments(XShortResourceID *pInstArray, int16_t maxInstArraySize,
										XShortResourceID *pSndArray, int16_t maxSndArraySize)
{
	register int32_t		count, instCount, sndCount, newCount, completeSndCount;
	XShortResourceID	soundID;
	XShortResourceID	*completeSndArray;
	bool				goodSound;
	InstrumentResource	*theX;
	int32_t				size;

	sndCount = 0;
	completeSndArray = (XShortResourceID *)XNewPtr(sizeof(XShortResourceID) * MAX_INSTRUMENTS * 128L);
	if (completeSndArray)
	{
		if ( (pInstArray) && (pSndArray) )
		{
			instCount = 0;
			for (count = 0; count < maxInstArraySize; count++)
			{
				if (pInstArray[count] != (XShortResourceID)-1)
				{
					instCount++;
				}
				else
				{
					break;
				}
			}
			for (count = 0; count < maxSndArraySize; count++)
			{
				pSndArray[count] = (XShortResourceID)-1;
				completeSndArray[count] = (XShortResourceID)-1;
			}


			completeSndCount = 0;
			for (count = 0; count < instCount; count++)
			{
				theX = (InstrumentResource *)XGetAndDetachResource(ID_INST, pInstArray[count], &size);
				if (theX)
				{
					newCount = XCollectSoundsFromInstrument(theX, &completeSndArray[completeSndCount], 128);
					XDisposePtr((XPTR)theX);
					completeSndCount += newCount;
				}
			}
			
			// remove duplicates in snds
			for (newCount = 0; newCount < completeSndCount; newCount++)
			{
				goodSound = TRUE;
				soundID = completeSndArray[newCount];
				for (count = 0; count < sndCount; count++)
				{
					if (soundID == pSndArray[count])
					{
						goodSound = FALSE;
						break;
					}
				}
				if (goodSound)
				{
					pSndArray[sndCount++] = (XShortResourceID)soundID;
				}
			}
			XBubbleSortArray((int16_t *)pSndArray, (int16_t)sndCount);
		}
		XDisposePtr(completeSndArray);
	}
	return (short)sndCount;
}

// Given a song ID and two arrays, this will return the INST resources ID and the 'snd ' resource ID
// that are needed to load the song terminated with a -1.
// Will return 0 for success or 1 for failure
OPErr XGetSongInstrumentList(XShortResourceID theSongID, XShortResourceID *pInstArray, int16_t maxInstArraySize,
										XShortResourceID *pSndArray, int16_t maxSndArraySize)
{
	int32_t				count, instCount, sndCount, newCount, completeSndCount;
	XShortResourceID	soundID;
	SongResource		*theSong;
	XShortResourceID	completeSndArray[MAX_SAMPLES];
	XShortResourceID	completeInstArray[MAX_INSTRUMENTS * MAX_BANKS];
	bool				goodSound;
	OPErr				theErr;
	int32_t				size;

	theErr = NO_ERR;
	if ( (pInstArray) && (pSndArray) )
	{
		for (count = 0; count < maxInstArraySize; count++)
		{
			pInstArray[count] = (XShortResourceID)-1;
			completeInstArray[count] = (XShortResourceID)-1;
		}
		for (count = 0; count < maxSndArraySize; count++)
		{
			pSndArray[count] = (XShortResourceID)-1;
			completeSndArray[count] = (XShortResourceID)-1;
		}
		theSong = (SongResource *)XGetAndDetachResource(ID_SONG, theSongID, &size);
		if (theSong)
		{
			instCount = GM_GetUsedPatchlist(theSong, NULL, 0, completeInstArray, &theErr);
			if (instCount && (theErr == 0) )
			{
				// remove duplicates in inst
				sndCount = 0;
				for (newCount = 0; newCount < instCount; newCount++)
				{
					goodSound = TRUE;
					soundID = completeInstArray[newCount];
					for (count = 0; count < sndCount; count++)
					{
						if (soundID == pInstArray[count])
						{
							goodSound = FALSE;
							break;
						}
					}
					if (goodSound)
					{
						pInstArray[sndCount++] = (XShortResourceID)soundID;
					}
				}
				instCount = sndCount;
				XBubbleSortArray((int16_t *)pInstArray, (int16_t)instCount);

				completeSndCount = 0;
				for (count = 0; count < instCount; count++)
				{
					newCount = XCollectSoundsFromInstrumentID(pInstArray[count], 
												&completeSndArray[completeSndCount], 128);
					completeSndCount += newCount;
				}
				
				// remove duplicates in snds
				sndCount = 0;
				for (newCount = 0; newCount < completeSndCount; newCount++)
				{
					goodSound = TRUE;
					soundID = completeSndArray[newCount];
					for (count = 0; count < sndCount; count++)
					{
						if (soundID == pSndArray[count])
						{
							goodSound = FALSE;
							break;
						}
					}
					if (goodSound)
					{
						pSndArray[sndCount++] = (XShortResourceID)soundID;
					}
				}
				XBubbleSortArray((int16_t *)pSndArray, (int16_t)sndCount);
			}
			else
			{
				theErr = BAD_MIDI_DATA;
			}
		}
		XDisposePtr((XPTR)theSong);
	}
	return theErr;
}

// returns >0 if successful, 0 if aborted, -1 if failed
int32_t XCompressAndEncrypt(XPTR* newData, XPTR pData, uint32_t size,
							XCompressStatusProc proc, void* procData)
{
int32_t		newSize;

	newSize = XCompressPtr(newData, pData, size, X_RAW, proc, procData);
	if (newSize > 0)
	{
		XEncryptData(*newData, newSize);
	}
	return newSize;
}

int32_t XGetSongTempoFactor(SongResource *pSong)
{
	int32_t			tempo;

	tempo = 16667;	// 1.0
	if (pSong)
	{
		switch (((SongResource_SMS *)pSong)->songType)
		{
			case SONG_TYPE_SMS:
				tempo = XGetShort(&((SongResource_SMS *)pSong)->songTempo);
				break;
			case SONG_TYPE_RMF:
				tempo = XGetShort(&((SongResource_RMF *)pSong)->songTempo);
				break;
		}
		if (tempo == 0)
		{
			tempo = 16667;
		}
	}
	return tempo;
}

void XSetSongTempoFactor(SongResource *pSong, int32_t newTempo)
{
	if (pSong)
	{
		if (newTempo == 16667L)
		{
			newTempo = 0;
		}
		switch (((SongResource_SMS *)pSong)->songType)
		{
			case SONG_TYPE_SMS:
				XPutShort(&((SongResource_SMS *)pSong)->songTempo, (unsigned short)newTempo);
				break;
			case SONG_TYPE_RMF:
				XPutShort(&((SongResource_RMF *)pSong)->songTempo, (unsigned short)newTempo);
				break;
		}
	}
}

// allocate and return an list of ID's collected from ID_SND, ID_CSND, ID_ESND. pCount will
// be the number of ID's, and the long array will be the list. use XDisposePtr on the return
// pointer
XLongResourceID * XGetAllSoundID(int32_t *pCount)
{
	/* NeoBAE: XCountTypes/XGetIndexedType are unimplemented; scan known sound types. */
	static const XResourceType soundTypes[] = { ID_SND, ID_CSND, ID_ESND };
	int32_t			count, size, typeIndex, sampleCount;
	XResourceType	resType;
	XLongResourceID	theID;
	char			name[256];
	XPTR			data;
	XLongResourceID	*pArray;

	pArray = NULL;
	sampleCount = 0;
	if (pCount)
	{
		*pCount = 0;
		pArray = (XLongResourceID *)XNewPtr((int32_t)sizeof(XLongResourceID) * MAX_SAMPLES);
		if (pArray)
		{
			for (typeIndex = 0; typeIndex < (int32_t)(sizeof(soundTypes) / sizeof(soundTypes[0])); typeIndex++)
			{
				resType = soundTypes[typeIndex];
				for (count = 0; ; count++)
				{
					data = XGetIndexedResource(resType, &theID, count, name, &size);
					if (data)
					{
						XDisposePtr(data);			/* free pointer resource */
						pArray[sampleCount++] = theID;
						if (sampleCount == (MAX_SAMPLES - 1))
						{
							break;
						}
					}
					else
					{
						break;
					}
				}
				if (sampleCount == (MAX_SAMPLES - 1))
				{
					break;
				}
			}
			*pCount = sampleCount;
		}
	}
	return pArray;
}

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
								XResourceType *pMusicType, XLongResourceID *pMusicID, int32_t *pReturnedSize)
{
	XLongResourceID	musicID;
	SongType		songType;
	XPTR			data;
	XResourceType	midiTypes[] = {ID_MIDI, ID_MIDI_OLD, ID_CMID, ID_EMID, ID_ECMI};
	short int		count;

	data = NULL;
	if (pReturnedSize)
	{
		*pReturnedSize = 0;
	}
	if (pMusicName)
	{
		pMusicName[0] = 0;
	}
	if (theXSong)
	{
		*pMusicType = 0;
		*pMusicID = 0;
		musicID = XGetSongResourceObjectID(theXSong);
		songType = XGetSongResourceObjectType(theXSong);
		
		for (count = 0; count < (sizeof(midiTypes) / sizeof(midiTypes[0])); count++)
		{
			data = XGetAndDetachResource(midiTypes[count], musicID, pReturnedSize);
			if (data)
			{
				if (pMusicName)
				{
					XGetResourceName(midiTypes[count], musicID, pMusicName);
					XCtoPstr(pMusicName);
				}
				*pMusicType = midiTypes[count];
				*pMusicID = musicID;
				break;
			}
		}
	}
	return data;
}

bool XIsSampleUsedInAllInstruments(XShortResourceID soundSampleID, XShortResourceID *pWhichInstrument)
{
	InstrumentResource	*theX;
	short int			count, totalInstruments;
	bool				used;
	int32_t				size;
	XShortResourceID	instArray[MAX_INSTRUMENTS * MAX_BANKS];

	used = FALSE;
	totalInstruments = XGetInstrumentArray(instArray, MAX_INSTRUMENTS * MAX_BANKS);
	if (totalInstruments && pWhichInstrument)
	{
		for (count = 0; count < totalInstruments; count++)
		{
			theX = (InstrumentResource *)XGetAndDetachResource(ID_INST, instArray[count], &size);
			if (theX)
			{
				used = XIsSoundUsedInInstrument(theX, soundSampleID);
				XDisposePtr((XPTR)theX);
				if (used)
				{
					*pWhichInstrument = instArray[count];
					break;
				}
			}
		}
	}
	return used;
}


int32_t XCopySongMidiResources(XLongResourceID theSongID, XFILE readFileRef, 
								XFILE writeFileRef, bool protect, bool copyNames)
{
	XPTR			pData;
	SongResource	*theSong;
	int32_t			songSize;
	XResourceType	theDataType;
	short int		theID;
	char			theName[256], theSongName[256];
	int32_t			dataSize, newSize;
	XPTR			newMidiData;

	XFileUseThisResourceFile(readFileRef);		// from resource file
	theSong = (SongResource *)XGetFileResource(readFileRef, ID_SONG, theSongID, theSongName, &songSize);
	if (theSong)
	{
		theID = XGetSongResourceObjectID(theSong);

		if (protect)
		{
			pData = XGetMidiData(theID, &dataSize, &theDataType);
			if (pData)
			{
				newSize = XCompressAndEncrypt(&newMidiData, pData, dataSize, NULL, NULL);
				if (newSize > 0)
				{
					theDataType = ID_ECMI;
					XDisposePtr(pData);
					pData = newMidiData;
				}
				else
				{
					theDataType = ID_MIDI;
				}
			}
		}
		else
		{
			theDataType = ID_CMID;
			pData = XGetFileResource(readFileRef, ID_CMID, theID, theName, &dataSize);
			if (pData == NULL)
			{
				theDataType = ID_ECMI;
				pData = XGetFileResource(readFileRef, ID_ECMI, theID, theName, &dataSize);
			}
			if (pData == NULL)
			{
				theDataType = ID_EMID;
				pData = XGetFileResource(readFileRef, ID_EMID, theID, theName, &dataSize);
			}
			if (pData == NULL)
			{
				theDataType = ID_MIDI;
				pData = XGetFileResource(readFileRef, ID_MIDI, theID, theName, &dataSize);
			}
			if (pData == NULL)
			{
				theDataType = ID_MIDI;		// convert it to the new type
				pData = XGetFileResource(readFileRef, ID_MIDI_OLD, theID, theName, &dataSize);
			}
		}
		if (pData)
		{
			// write midi resource
			XFileUseThisResourceFile(writeFileRef);
			if (copyNames == FALSE)
			{
				theName[0] = 0;
			}
						XAddResource(theDataType, theID, theName, pData, dataSize);
			XDisposePtr(pData);

			// write song resource
			XFileUseThisResourceFile(readFileRef);

			if (protect)
			{
				XSetSongLocked(theSong, TRUE);
			}

			XFileUseThisResourceFile(writeFileRef);
			XAddResource(ID_SONG, theID, theName, theSong, songSize);
			XDisposePtr(theSong);
		}
	}
	return 0;
}

int32_t XCopySndResources(XShortResourceID *pSndCopy, int16_t sndCount, XFILE readFileRef, 
									XFILE writeFileRef, bool protect, bool copyNames)
{
	short int		count, resCount;
	XPTR			pData;
	short int		theID;
	char			theName[256];
	int32_t			size;
	XResourceType	soundTypes[] = {ID_SND, ID_ESND, ID_CSND};

	if (sndCount && pSndCopy)
	{
		XFileUseThisResourceFile(readFileRef);		/* from resource file */
		for (count = 0; count < sndCount; count++)
		{
			theID = pSndCopy[count];
			
			// determine if the resource is already in the written file
			XFileUseThisResourceFile(writeFileRef);
			pData = XGetAndDetachResource(ID_SND, theID, &size);
			if (pData == NULL)
			{
				pData = XGetAndDetachResource(ID_ESND, theID, &size);
			}
			if (pData == NULL)
			{
				pData = XGetAndDetachResource(ID_CSND, theID, &size);
			}
			XDisposePtr(pData);
			if (pData == NULL)			// check to see that its not there already!!
			{
				for (resCount = 0; resCount < 3; resCount++)
				{
					XFileUseThisResourceFile(readFileRef);		/* from resource file */
					pData = XGetFileResource(readFileRef, soundTypes[resCount], theID, theName, &size);
					if (pData)
					{
						if (copyNames == FALSE)
						{
							theName[0] = 0;
						}

						XFileUseThisResourceFile(writeFileRef);
						if (protect && (soundTypes[resCount] == ID_SND))
						{
							XEncryptData(pData, size);
							XAddResource(ID_ESND, theID, theName, pData, size);
						}
						else
						{
							XAddResource(soundTypes[resCount], theID, theName, pData, size);
						}
						XDisposePtr(pData);
					}
				}
			}
			else
			{
				XDisposePtr(pData);
			}
		}
	}
	return 0;
}

int32_t XCopyInstrumentResources(XShortResourceID *pInstCopy, int16_t instCount, 
										XFILE readFileRef, XFILE writeFileRef, bool copyNames)
{
	short int		count;
	XPTR			pData;
	short int		theID;
	int32_t			size;
	char			theName[256];

	if (instCount && pInstCopy)
	{
		XFileUseThisResourceFile(readFileRef);		/* from resource file */
		for (count = 0; count < instCount; count++)
		{
			theID = pInstCopy[count];
			XFileUseThisResourceFile(writeFileRef);
			pData = XGetFileResource(writeFileRef, ID_INST, theID, theName, &size);
			if (pData == NULL)		// check to see that its not there already!!
			{
				XFileUseThisResourceFile(readFileRef);		/* from resource file */
				pData = XGetFileResource(readFileRef, ID_INST, theID, theName, &size);
				if (pData)
				{
					XFileUseThisResourceFile(writeFileRef);
					if (copyNames == FALSE)
					{
						theName[0] = 0;
					}
					XAddResource(ID_INST, theID, theName, pData, size);
				}
			}
			XDisposePtr(pData);
		}
	}
	return 0;
}

int32_t XCompressAndEncryptWithType(XPTR* newData, XPTR pData, uint32_t size,
									XCOMPRESSION_TYPE type,
									XCompressStatusProc proc, void* procData)
{
	int32_t newSize;

#if USE_LZMA_COMPRESSION != TRUE
	/* LZMA compression types are unavailable in this build. */
	if ((type == (XCOMPRESSION_TYPE)0xFE) ||
		(((unsigned int)type) >= 0x80u && ((unsigned int)type) <= 0x83u))
	{
		return -1;
	}
#endif

	newSize = XCompressPtr(newData, pData, size, type, proc, procData);
	if (newSize > 0)
	{
		XEncryptData(*newData, (uint32_t)newSize);
	}
	return newSize;
}

#if USE_LZMA_COMPRESSION == TRUE
int32_t XCompressAndEncryptLZMA(XPTR* newData, XPTR pData, uint32_t size,
								XCompressStatusProc proc, void* procData)
{
	return XCompressAndEncryptWithType(newData, pData, size, X_LZMA_RAW, proc, procData);
}
#endif

static bool PV_IsCompiledInSndCompression(SndCompressionType compression)
{
	switch (compression)
	{
		case C_NONE:
		case C_IMA4:
		case C_IMA4_WAV:
		case C_ULAW:
		case C_ALAW:
		case C_LZSS:
			return TRUE;

#if USE_LZMA_COMPRESSION == TRUE
		case C_LZMA:
			return TRUE;
#endif

#if USE_FLAC_ENCODER == TRUE
		case C_FLAC:
			return TRUE;
#endif

#if USE_VORBIS_ENCODER == TRUE
		case C_VORBIS:
			return TRUE;
#endif

#if USE_OPUS_ENCODER == TRUE
		case C_OPUS:
			return TRUE;
#endif

#if USE_MPEG_ENCODER == TRUE
		case C_MPEG_32:
		case C_MPEG_40:
		case C_MPEG_48:
		case C_MPEG_56:
		case C_MPEG_64:
		case C_MPEG_80:
		case C_MPEG_96:
		case C_MPEG_112:
		case C_MPEG_128:
		case C_MPEG_160:
		case C_MPEG_192:
		case C_MPEG_224:
		case C_MPEG_256:
		case C_MPEG_320:
			return TRUE;
#endif

		default:
			return FALSE;
	}
}

OPErr XCreateEncodedSoundObject(XPTR* dst,
								GM_Waveform const* src,
								SndCompressionType compression,
								SndCompressionSubType subType,
								XCompressStatusProc proc, void* procData)
{
	if ((dst == NULL) || (src == NULL))
	{
		return PARAM_ERR;
	}
	if (PV_IsCompiledInSndCompression(compression) == FALSE)
	{
		return NOT_SETUP;
	}
	return XCreateSoundObjectFromData(dst, src, compression, subType, proc, procData);
}

#if 0
void XTestCompression(XPTR compressedAndEncryptedData, int32_t size, XPTR originalData, int32_t originalSize)
{
	XPTR	pData, pData2;
	short int	safe;

	if (compressedAndEncryptedData && originalData && size && originalSize)
	{
		// since this is encrypted, make a new copy and decrypt
		pData = XNewPtr(size);
		if (pData)
		{
			XBlockMove(compressedAndEncryptedData, pData, size);
			XDecryptData(pData, (uint32_t)size);				// decrypt first
			pData2 = pData;
			pData = XDecompressPtr(pData2, (uint32_t)size, TRUE);		// uncompress second
			if (pData)
			{
				size = XGetPtrSize(pData);	// get new size
				if (size == originalSize)
				{
					safe = XMemCmp(pData, originalData, size);
					if (safe)
					{
						DebugStr("\pmemcmp failed");
					}
					else
					{
						DebugStr("\pSAFE!");
					}
				}
				else
				{
					DebugStr("\psize failed");
				}
			}
			else
			{
				DebugStr("\pdecrypt failed");
			}
			XDisposePtr(pData);
			XDisposePtr(pData2);
		}
	}
}
#endif

#endif	// USE_CREATION_API == TRUE

// EOF of X_EditorTools.c



