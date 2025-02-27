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
/*****************************************************************************/
/*
**  X_PackStructures.h
**
**      Include this to pack structures on a byte boundery.
**
**  © Copyright 2001 Beatnik, Inc, All Rights Reserved.
**  Written by Steve Hales
**
**  Beatnik products contain certain trade secrets and confidential and
**  proprietary information of Beatnik.  Use, reproduction, disclosure
**  and distribution by any means are prohibited, except pursuant to
**  a written license from Beatnik. Use of copyright notice is
**  precautionary and does not imply publication or disclosure.
**
**  Restricted Rights Legend:
**  Use, duplication, or disclosure by the Government is subject to
**  restrictions as set forth in subparagraph (c)(1)(ii) of The
**  Rights in Technical Data and Computer Software clause in DFARS
**  252.227-7013 or subparagraphs (c)(1) and (2) of the Commercial
**  Computer Software--Restricted Rights at 48 CFR 52.227-19, as
**  applicable.
**
**  Confidential-- Internal use only
**
**  History -
**  5/23/2001   Created
*/
/*****************************************************************************/

#ifndef __X_API__
    #include "X_API.h"
#endif

#undef X_PACKBY1
#undef X_BF_1
#undef X_BF_2
#undef X_BF_3
#undef X_BF_4
#undef X_BF_5


#ifndef X_PACK_FAST
// controls to enable structure packing by 1 byte
    #if (X_PLATFORM == X_MACINTOSH) || (X_PLATFORM == X_IOS) || (X_PLATFORM == X_ANDROID) || (X_PLATFORM == X_ANSI) || (X_PLATFORM == X_WIN95)
	#if (COMPILER_TYPE == GCC_COMPILER) || (__MINGW32__)
	    #define X_PACKBY1 __attribute__((packed,aligned(__alignof__(short))))
	#else
	    #define X_PACKBY1   __attribute__ ((packed))
	#endif
        #define X_BF_1  :1
        #define X_BF_2  :2
        #define X_BF_3  :3
        #define X_BF_4  :4
        #define X_BF_5  :5

        #ifdef __cplusplus
            // gcc++ bug
            #pragma pack(1)
        #endif
    #else
        #if CPU_TYPE == kRISC
            #pragma options align=mac68k
        #endif

        #if ((CPU_TYPE == k80X86) || (CPU_TYPE == kSPARC) || (CPU_TYPE == kARM))
            #pragma pack (1)
        #endif

        #error unknown X_PLATFORM type

        // This define is used when declaring the structures. Some compilers, like GCC
        // need to use '__attribute__ ((packed))' at each structure to pack by a byte.

	#if (COMPILER_TYPE == GCC_COMPILER) || (__MINGW32__)
	   #define X_PACKBY1 __attribute__((packed,aligned(__alignof__(short))))
	#else
	   #define X_PACKBY1
	#endif

        #define X_BF_1
        #define X_BF_2
        #define X_BF_3
        #define X_BF_4
        #define X_BF_5
    #endif

#else
// controls to enable structure packing by 4/8 bytes.
    #if X_PLATFORM == X_BE
        #pragma pack (4)
    #endif
    #if (((X_PLATFORM == X_MACINTOSH) || (X_PLATFORM == X_IOS)) && (COMPILER_TYPE == GCC_COMPILER))
        #pragma pack (4)
    #else
        #if CPU_TYPE == kRISC
            #pragma options align=power
        #endif
        /* Change pack(8) to pack(4) for SPARC.             -Liang */
        #if (CPU_TYPE == kSPARC)
            #pragma pack (4)
        #endif
        /* $$kk: pack(4) for solaris x86 */

        #if (CPU_TYPE == k80X86)
            #if (X_PLATFORM == X_SOLARIS)
                #pragma pack (4)
            #else
                #pragma pack (8)
            #endif
        #endif
    #endif

    #define X_BF_1
    #define X_BF_2
    #define X_BF_3
    #define X_BF_4
    #define X_BF_5
    #if (COMPILER_TYPE == GCC_COMPILER)
	   #define X_PACKBY1 __attribute__ ((packed))
    #else
	   #define X_PACKBY1
    #endif

#endif
