/****************************************************************************
*
* simple.c
*
* a command line audiofile player that supports RMF
*
* � Copyright 1999 Beatnik, Inc, All Rights Reserved.
* Written by Mark Deggeller (mark@beatnik.com)
*
* Legal Notice:
*   Beatnik products contain certain trade secrets and confidential and
*   proprietary information of Beatnik.  Use, reproduction, disclosure
*   and distribution by any means are prohibited, except pursuant to
*   a written license from Beatnik. Use of copyright notice is
*   precautionary and does not imply publication or disclosure.
*
* Restricted Rights Legend:
*   Use, duplication, or disclosure by the Government is subject to
*   restrictions as set forth in subparagraph (c)(1)(ii) of The
*   Rights in Technical Data and Computer Software clause in DFARS
*   252.227-7013 or subparagraphs (c)(1) and (2) of the Commercial
*   Computer Software--Restricted Rights at 48 CFR 52.227-19, as
*   applicable.
*
* Confidential - Internal use only
*
* History:
*   7/30/99     Created
*  8/11/99      Added PV_PrintRMFFields
*  9/21/99      Added support for wav, au, aiff, mpeg files.
*               Added BeatnikPlay(), PlaySound(), playbae()
*   10/26/99    Added PlayMIDI()
*
****************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "MiniBAE.h"
#include "BAE_API.h"
#include <GenSnd.h>
#include <signal.h>

static volatile bool interruptPlayBack = FALSE;
static volatile bool fadeOut = TRUE;

void intHandler(int dummy) {
    interruptPlayBack = TRUE;
}

// prototypes

char const usageString[] =
{
   "USAGE:  playbae  -p  {patches.hsb}\n"
   "                 -w  {Play a WAV file}\n"
   "                 -sw {Stream a WAV file}\n"
   "                 -sa {Stream a AIF file}\n"
   "                 -a  {Play a AIF file}\n"
   "                 -r  {Play a RMF file}\n"
   "                 -m  {Play a MID file}\n"
   "                 -o  {write output to file}\n"
   "                 -mr {mixer sample rate ie. 11025}\n"
   "                 -l  {# of times to loop}\n"
   "                 -v  {max volume (in percent, overdrive allowed)}\n"
   "                 -t  {max length in seconds to play midi (0 = forever)}\n"
   "                 -rv {set default reverb type}\n"
   "                 -rl {display reverb definitions}\n"
   "                 -nf {disable fade-out when stopping via time limit or CTRL-C}\n"
   "                 -2p {use 2-point Interpolation rather than default of Linear}\n"
   "                 -mv  {max voices (default: 64)}\n"
   "                 -h  {displays this message then exists}\n"
};

char const reverbTypeList[] =
{
  "Valid Reverb Types for -rv command:\n"
  "   0               Default\n"
  "   1               None\n"
  "   2               Igor's Closet\n"
  "   3               Igor's Garage\n"
  "   4               Igor's Acoustic Lab\n"
  "   5               Igor's Cavern\n"
  "   6               Igor's Dungeon\n"
  "   7               Small reflections (Reverb used for WebTV)\n"
  "   8               Early reflections (variable verb)\n"
  "   9               Basement (variable verb)\n"
  "   10              Banquet hall (variable verb)\n"
  "   11              Catacombs (variable verb)\n"
};


static int gWriteToFile = FALSE;

static void PV_Task(void *reference)
{
   if (reference)
   {
      BAEMixer_ServiceStreams(reference);
   }
}

static void PV_Idle(BAEMixer theMixer, unsigned long time)
{
   unsigned long count;
   unsigned long max;

   if (gWriteToFile)
   {
      BAEMixer_ServiceAudioOutputToFile(theMixer);
   }
   else
   {
      max = time / 12000;
      for (count = 0; count < max; count++)
      {
         BAE_WaitMicroseconds(12000);
      }
   }
}

static void PV_StreamCallback(BAEStream stream, unsigned long reference)
{
   printf("Stream %p reference %lx done\n", stream, reference);
}

static int PV_ParseCommands(int argc, char *argv[], char *command, int getResult, char *result)
{
   int count = 0;

   while (argc--)
   {
      if (strcmp(argv[count], command) == 0)
      {
         if (getResult)
         {
            strcpy(result, argv[count + 1]);
         }
         return(1);
      }
      count++;
   }
   return(0);
}


BAE_UNSIGNED_FIXED calculateVolume(BAE_UNSIGNED_FIXED volume) {
	BAE_UNSIGNED_FIXED temp = (volume / 100) *  MAX_SONG_VOLUME;

	printf("Debug volume calc: %lu\n",temp);
	return temp;
}


// PlayPCM()
// ---------------------------------------------------------------------
//
//
static BAEResult PlayPCM(BAEMixer theMixer, char *fileName, BAEFileType type, BAE_UNSIGNED_FIXED volume, unsigned int timeLimit)
{
   BAEResult err;
   BAESound  sound = BAESound_New(theMixer);
   BAESampleInfo songInfo;
   unsigned long currentPosition;
   int rate, m, s;
   BAE_BOOL  done;

   if (sound)
   {
      err = BAESound_LoadFileSample(sound, (BAEPathName)fileName, type);
      if (err == BAE_NO_ERROR)
      {
	 BAESound_SetVolume(sound, calculateVolume(volume));
         err = BAESound_Start(sound, 0, BAE_FIXED_1, 0);
         if (err == BAE_NO_ERROR)
         {
	    BAESound_GetInfo(sound, &songInfo);
	    rate = songInfo.sampledRate / 65536;
            printf("BAE memory used for everything %ld bytes\n\n", BAE_GetSizeOfMemoryUsed());
	    printf("Master sound volume set to %lu%%\n", volume / 1000);
            done = FALSE;
            while (done == FALSE)
            {
	       if (interruptPlayBack) {
		  printf("Stop requested... please wait for data flush...\n");
		  interruptPlayBack = FALSE;
		  BAESound_Stop(sound, fadeOut);
	       }
               BAESound_IsDone(sound, &done);
	       BAESound_GetSamplePlaybackPosition(sound, &currentPosition);
	       currentPosition = (currentPosition / rate);
	       m = (currentPosition / 60);
	       s = (currentPosition - (m*60));
	       if (s > 0 || m > 0) {
	          printf("Playback position: %02d:%02d\r",m,s);
	       }
               if (timeLimit > 0) {
 		  if (currentPosition >= timeLimit) {
			BAESound_Stop(sound, fadeOut);
		  }
	       }
               if (done == FALSE)
               {
                  PV_Idle(theMixer, 15000);
               }
            }
            PV_Idle(theMixer, 900000);
         }
         else
         {
            printf("playbae:  Couldn't start sound (BAE Error #%d)\n", err);
         }
      }
      else
      {
         printf("playbae:  Couldn't open sound file '%s' (BAE Error #%d)\n", fileName, err);
      }
   }
   else
   {
      err = BAE_MEMORY_ERR;
   }
   BAESound_Delete(sound);
   return(err);
}

// PlayPCMStreamed()
// ---------------------------------------------------------------------
//
//
static BAEResult PlayPCMStreamed(BAEMixer theMixer, char *fileName, BAEFileType type, BAE_UNSIGNED_FIXED volume)
{
   BAEResult err;
   BAEStream stream = BAEStream_New(theMixer);
   BAE_BOOL  done;

   if (stream)
   {
      err = BAEStream_SetupFile(stream, (BAEPathName)fileName,
                                type,
                                BAE_MIN_STREAM_BUFFER_SIZE,
                                FALSE);

      if (err == BAE_NO_ERROR)
      {
	 BAEStream_SetVolume(stream, calculateVolume(volume));
         BAEStream_SetCallback(stream, PV_StreamCallback, 0x1234);
         err = BAEStream_Start(stream);
         if (err == BAE_NO_ERROR)
         {
 	    printf("Master stream volume set to %lu%%\n", volume / 1000);
            printf("BAE memory used for everything %ld bytes\n\n", BAE_GetSizeOfMemoryUsed());
            done = FALSE;
            while (done == FALSE)
            {
	       if (interruptPlayBack) {
		  printf("Stop requested... please wait for data flush...\n");
		  interruptPlayBack = 0;
		  BAEStream_Stop(stream, fadeOut);
	       }
               BAEStream_IsDone(stream, &done);
               if (done == FALSE)
               {
                  PV_Idle(theMixer, 15000);
               }
            }
            PV_Idle(theMixer, 900000);
         }
         else
         {
            printf("playbae:  Couldn't start sound (BAE Error #%d)\n", err);
         }
      }
      else
      {
         printf("playbae:  Couldn't open sound file '%s' (BAE Error #%d)\n", fileName, err);
      }
   }
   else
   {
      err = BAE_MEMORY_ERR;
   }
   BAEStream_Delete(stream);
   return(err);
}


// PlayMidi()
// ---------------------------------------------------------------------
//
//
static BAEResult PlayMidi(BAEMixer theMixer, char *fileName, BAE_UNSIGNED_FIXED volume, unsigned int timeLimit, unsigned int loopCount, BAEReverbType reverbType)
{
   BAEResult err;
   BAESong   theSong = BAESong_New(theMixer);
   unsigned long currentPosition;
   int m, s, ms = 0;
   BAE_BOOL  done;
   if (theSong)
   {
#if 0
      err = BAESong_ControlChange(theSong, 0, 1, 1, 0);

#else
      err = BAESong_LoadMidiFromFile(theSong, (BAEPathName)fileName, TRUE);
      if (err == BAE_NO_ERROR)
      {
	 BAESong_SetVolume(theSong, calculateVolume(volume));
         err = BAESong_Start(theSong, 0);
         if (err == BAE_NO_ERROR)
         {
            BAESong_DisplayInfo(theSong);

            BAEMixer_SetDefaultReverb(theMixer, (BAEReverbType)reverbType);
            printf("Reverb Type set to %d\n", reverbType);

            BAESong_SetLoops(theSong, loopCount);
	    printf("Master song volume set to %lu%%\n", volume / 1000);
	    if (loopCount > 0) {
		printf("Will loop song %u times\n", loopCount);
	    }
            if (timeLimit > 0) {
		printf("Max Play Duration: %d seconds\n", timeLimit);
            }

            printf("BAE memory used for everything %ld bytes\n\n", BAE_GetSizeOfMemoryUsed());
            done = FALSE;
            while (done == FALSE)
            {
	       if (interruptPlayBack) {
		  printf("Stop requested... please wait for data flush...\n");
		  interruptPlayBack = 0;
		  BAESong_Stop(theSong, fadeOut);
	       }
               BAESong_IsDone(theSong, &done);
	       BAESong_GetMicrosecondPosition(theSong, &currentPosition);
               currentPosition = currentPosition / 1000;
               m = (currentPosition / 60000);
               s = (currentPosition - (m*60000)) / 1000;
               ms = (currentPosition - (60000*m) - (s*1000));
               if (ms > 1 || s > 0 || m > 0) {
                  printf("Playback position: %02d:%02d.%03d\r",m,s,ms);
               }
               if (timeLimit > 0) {
                  if (currentPosition > (timeLimit * 1000) - 750) {
                        BAESong_Stop(theSong, fadeOut);
                  }
               }
               if (done == FALSE)
               {
                  PV_Idle(theMixer, 15000);
               }
            }
            PV_Idle(theMixer, 900000);
         }
         else
         {
            printf("playbae:  Couldn't start song (BAE Error #%d)\n", err);
         }
      }
      else
      {
         printf("playbae:  Couldn't open Midi file '%s' (BAE Error #%d)\n", fileName, err);
      }
#endif
   }
   else
   {
      err = BAE_MEMORY_ERR;
   }
   printf("\n");
   BAESong_Delete(theSong);
   return(err);
}

// PlayRMF()
// ---------------------------------------------------------------------
//
//
static BAEResult PlayRMF(BAEMixer theMixer, char *fileName, BAE_UNSIGNED_FIXED volume,  unsigned int timeLimit, unsigned int loopCount, BAEReverbType reverbType)
{
   BAEResult err;
   BAESong   theSong = BAESong_New(theMixer);
   unsigned long currentPosition;
   int m, s, ms = 0;
   BAE_BOOL  done;

   if (theSong)
   {
      err = BAESong_LoadRmfFromFile(theSong, (BAEPathName)fileName, 0, TRUE);
      if (err == BAE_NO_ERROR)
      {
	 BAESong_SetVolume(theSong, calculateVolume(volume));
         err = BAESong_Start(theSong, 0);
         if (err == BAE_NO_ERROR)
         {
            BAESong_DisplayInfo(theSong);

            BAEMixer_SetDefaultReverb(theMixer, (BAEReverbType)reverbType);
            printf("Reverb Type set to %d\n", reverbType);

            BAESong_SetLoops(theSong, loopCount);
	    printf("Master song volume set to %lu%%\n", volume / 1000);
	    if (loopCount > 0) {
		printf("Will loop song %u times\n", loopCount);
	    }
	    if (timeLimit > 0) {
		printf("Max Play Duration: %d seconds\n", timeLimit);
            }
            printf("BAE memory used for everything %ld bytes\n\n", BAE_GetSizeOfMemoryUsed());
            done = FALSE;
	    while (done == FALSE)
            {
	       if (interruptPlayBack) {
		  printf("Stop requested... please wait for data flush...\n");
		  interruptPlayBack = 0;
		  BAESong_Stop(theSong, fadeOut);
	       }
               BAESong_IsDone(theSong, &done);
	       BAESong_GetMicrosecondPosition(theSong, &currentPosition);
               currentPosition = currentPosition / 1000;
               m = (currentPosition / 60000);
               s = (currentPosition - (m*60000)) / 1000;
               ms = (currentPosition - (60000*m) - (s*1000));
               if (ms > 1 || s > 0 || m > 0) {
                  printf("Playback position: %02d:%02d.%03d\r",m,s,ms);
               }
               if (timeLimit > 0) {
                  if (currentPosition > (timeLimit * 1000) - 750) {
                        BAESong_Stop(theSong, fadeOut);
                  }
               }
               if (done == FALSE)
               {
                  PV_Idle(theMixer, 15000);
               }
            }
            PV_Idle(theMixer, 900000);
         }
         else
         {
            printf("playbae:  Couldn't start song (BAE Error #%d)\n", err);
         }
      }
      else
      {
         printf("playbae:  Couldn't open RMF file '%s' (BAE Error #%d)\n", fileName, err);
      }
   }
   else
   {
      err = BAE_MEMORY_ERR;
   }
   printf("\n");
   BAESong_Delete(theSong);
   return(err);
}

// main()
// ---------------------------------------------------------------------
int main(int argc, char *argv[])
{
   BAEResult    err;
   BAEMixer     theMixer;
   short int    rmf, pcm, level;
   unsigned int loopCount = 0;
   unsigned int timeLimit = 0;
   BAE_UNSIGNED_FIXED volume = 100000; // (percent * 1000)
   BAETerpMode interpol = BAE_LINEAR_INTERPOLATION;
   int maxVoices = BAE_MAX_VOICES;
   BAEBankToken bank;
   int doneCommand = 0;
   short reverbType = 8; // early reflections
   char parmFile[1024];
   BAERate rate = BAE_RATE_44K;

   signal(SIGINT, intHandler);
   theMixer = BAEMixer_New();
   if (theMixer)
   {
       if (PV_ParseCommands(argc, argv, "-mv", TRUE, parmFile))
       {
           maxVoices = atoi(parmFile);
       }

       pcm   = 1;
       rmf   = maxVoices - pcm;
       level = rmf / 3;
       if (PV_ParseCommands(argc, argv, "-rl", FALSE, NULL))
       {
           printf(reverbTypeList);
           return 0;
       }
       if (PV_ParseCommands(argc, argv, "-h", FALSE, NULL))
       {
           printf(usageString);
           return 0;
       }

       if (PV_ParseCommands(argc, argv, "-mr", TRUE, parmFile))
       {
          rate = (BAERate)atoi(parmFile);
       }

       if (PV_ParseCommands(argc, argv, "-l", TRUE, parmFile))
       {
          loopCount = (unsigned int)atoi(parmFile);
       }

       if (PV_ParseCommands(argc, argv, "-v", TRUE, parmFile))
       {
          volume = (unsigned int)(atoi(parmFile) * 1000);
       }

       if (PV_ParseCommands(argc, argv, "-t", TRUE, parmFile))
       {
          timeLimit = (unsigned int)atoi(parmFile);
       }

       if (PV_ParseCommands(argc, argv, "-nf", FALSE, NULL))
       {
          fadeOut = FALSE;
       }

       if (PV_ParseCommands(argc, argv, "-2p", FALSE, NULL))
       {
          interpol = BAE_2_POINT_INTERPOLATION;
       }


      printf("Allocating mixer with %d voices for RMF/Midi playback\n"
             "and %d voices for PCM playback at %d sample rate\n",
             rmf, pcm,
             rate);

      err = BAEMixer_Open(theMixer,
                          rate,
                          interpol,
                          BAE_USE_STEREO | BAE_USE_16,
                          rmf,                                          // midi voices
                          pcm,                                          // pcm voices
                          level,
                          TRUE);
      if (err == BAE_NO_ERROR)
      {
         BAEMixer_SetAudioTask(theMixer, PV_Task, (void *)theMixer);

         // turn on nice verb
         if (PV_ParseCommands(argc, argv, "-rv", TRUE, parmFile))
         {
	    // user selected reverb
            reverbType = (short)atoi(parmFile);
	    if (reverbType > 11) {
		printf("Invalid reverbType %d, expected 1-11. Ignored.\n", reverbType);
		reverbType = 8;
  	    }
         }
         printf("BAE memory used during idle prior to SetBankToFile: %ld bytes\n\n", BAE_GetSizeOfMemoryUsed());

         if (PV_ParseCommands(argc, argv, "-p", TRUE, parmFile))
         {
            printf("Using bank '%s'\n", parmFile);
            err = BAEMixer_AddBankFromFile(theMixer, (BAEPathName)parmFile, &bank);
            if (err > 0) {
		printf("Error %d loading patch bank %s",err,parmFile);
		return(1);
	    }
            printf("BAE memory used during idle after SetBankToFile: %ld bytes\n\n", BAE_GetSizeOfMemoryUsed());
         }

         if (PV_ParseCommands(argc, argv, "-o", TRUE, parmFile))
         {
            err = BAEMixer_StartOutputToFile(theMixer,
                                             (BAEPathName)parmFile,
                                             BAE_WAVE_TYPE,
                                             BAE_COMPRESSION_NONE);
            if (err)
            {
               printf("BAEMixer_StartOutputToFile failed with error %d\n", err);
            }
            else
            {
               gWriteToFile = TRUE;
               printf("Writing to file %s\n", parmFile);
            }
         }

         if (PV_ParseCommands(argc, argv, "-a", TRUE, parmFile))
         {
            printf("Play AIFF\n");
            err         = PlayPCM(theMixer, parmFile, BAE_AIFF_TYPE, volume, timeLimit);
            doneCommand = 1;
         }
         if (PV_ParseCommands(argc, argv, "-sa", TRUE, parmFile))
         {
            printf("Stream AIFF\n");
            err         = PlayPCMStreamed(theMixer, parmFile, BAE_AIFF_TYPE, volume);
            doneCommand = 1;
         }
         if (PV_ParseCommands(argc, argv, "-w", TRUE, parmFile))
         {
            printf("Play WAVE\n");
            err         = PlayPCM(theMixer, parmFile, BAE_WAVE_TYPE, volume, timeLimit);
            doneCommand = 1;
         }
         if (PV_ParseCommands(argc, argv, "-sw", TRUE, parmFile))
         {
            printf("Stream WAVE\n");
            err         = PlayPCMStreamed(theMixer, parmFile, BAE_WAVE_TYPE, volume);
            doneCommand = 1;
         }
         if (PV_ParseCommands(argc, argv, "-r", TRUE, parmFile))
         {
            printf("PlayRMF\n");
            err         = PlayRMF(theMixer, parmFile, volume, timeLimit, loopCount, reverbType);
            doneCommand = 1;
         }
         if (PV_ParseCommands(argc, argv, "-m", TRUE, parmFile))
         {
            printf("PlayMidi\n");
            err         = PlayMidi(theMixer, parmFile, volume, timeLimit, loopCount, reverbType);
            doneCommand = 1;
         }

         if (gWriteToFile)
         {
            BAEMixer_StopOutputToFile();
         }
      }
      else
      {
         printf("playbae:  Couldn't open mixer (BAE Error #%d)\n", err);
      }
   }
   else
   {
      printf("playbae:  Memory error.\n");
   }

   if (err > 0) {
	return(1);
   }

   if (doneCommand == 0)
   {
      printf(usageString);
   }

   BAE_WaitMicroseconds(160000);
   BAEMixer_Delete(theMixer);
   return(0);
}


// EOF
