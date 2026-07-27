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
    Additional modifications © 2021-2026 zefie
    Licensed under the GNU Lesser General Public License v3.0 or later.
*/
/*****************************************************************************/
/*
**  BAEBuildOptions_foobar2000.h
**
**  Build options for NeoBAE compiled as a component of a foobar2000 input
**  plugin (e.g. foo_midi). The backend is pull-driven: foobar2000 calls
**  BAE_FB2K_RenderAudio() directly from the decoder's Render() method; there
**  is no background audio thread and no capture device.
**
**  Targets: Windows x86 / x86-64, MSVC toolchain.
**
**  © Copyright 1999-2000 Beatnik, Inc, All Rights Reserved.
**
**  Modification History:
**  10/19/99    MSD:    Created -- extracted from X_API.h
**  11/10/99            Allowed more defines to be overridden.
**  2/4/2000            Changed copyright. We're Y2K compliant!
**  2025-xx-xx          foobar2000 plugin variant -- pull render, no capture.
*/
/*****************************************************************************/

#ifndef WIN32_EXTRA_LEAN
    #define WIN32_EXTRA_LEAN
#endif
#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif


// #includes
// ----------------------------------------------
#include <windows.h>
#include <stdlib.h>
#include <windowsx.h>
#include <stdio.h>
#include <io.h>
#include <fcntl.h>


// Non-overwritable Flags
// ----------------------------------------------
#define COMPILER_TYPE                   DEFAULT_COMPILER
#define CPU_TYPE                        k80X86
#define X_WORD_ORDER                    TRUE    // little-endian
#define USE_FLOAT                       TRUE
#define USE_8_BIT_OUTPUT                FALSE   // foobar2000 always works in 16-bit or float
#define USE_16_BIT_OUTPUT               TRUE
#define USE_MONO_OUTPUT                 FALSE   // stereo output only for plugin use
#define USE_STEREO_OUTPUT               TRUE
#define LOOPS_USED                      U3232_LOOPS
#define USE_DROP_SAMPLE                 FALSE
#define USE_TERP1                       FALSE
#define USE_TERP2                       TRUE
#define USE_NEW_EFFECTS                 TRUE
#define FILE_NAME_LENGTH                _MAX_PATH
#define USE_DEVICE_ENUM_SUPPORT         TRUE
#define USE_CALLBACKS                   TRUE
#define USE_CREATION_API                TRUE
#define BAE_COMPLETE                    1
#define USE_STREAM_API                  TRUE
#define SUPPORT_IGOR_FEATURE            TRUE
#define USE_NEO_EFFECTS                 TRUE


// Overwritable Flags -- default values
// ----------------------------------------------

#ifndef REVERB_USED
    #define REVERB_USED                 VARIABLE_REVERB
#endif

#ifndef USE_FULL_RMF_SUPPORT
    #define USE_FULL_RMF_SUPPORT        TRUE
#endif

#ifndef USE_CREATION_API
    #define USE_CREATION_API            TRUE
#endif

#ifndef USE_HIGHLEVEL_FILE_API
    #define USE_HIGHLEVEL_FILE_API      TRUE
#endif

#ifndef USE_STREAM_API
    #define USE_STREAM_API              TRUE
#endif

// Capture is not applicable in a foobar2000 decoder plugin; foobar2000
// owns recording and encoding entirely.
#ifndef USE_CAPTURE_API
    #define USE_CAPTURE_API             FALSE
#endif

#ifndef USE_MOD_API
    #define USE_MOD_API                 FALSE
#endif

// MP3/MPEG encoding is handled externally by foobar2000; disable here.
#ifndef USE_MPEG_ENCODER
    #define USE_MPEG_ENCODER            FALSE
#endif

#ifndef USE_MPEG_DECODER
    #define USE_MPEG_DECODER            FALSE
#endif


// INLINE
// ----------------------------------------------
#define INLINE                          __inline


// DEBUG_STR
// ----------------------------------------------
#ifndef DEBUG_STR
    #if USE_DEBUG == 0
        #define DEBUG_STR(x)
    #endif
    #if USE_DEBUG == 1
        #define DEBUG_STR(x)            fprintf(stderr, x)
    #endif
#endif
