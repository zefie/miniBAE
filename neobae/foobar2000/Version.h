#pragma once

// FOO_NEOBAE_VERSION_STRING is generated at build time by gen_version.bat
// (same git rules as CMakeLists.txt / Makefile.versioning).
#if defined(__has_include)
#  if __has_include("Version.gen.h")
#    include "Version.gen.h"
#  endif
#endif

#ifndef FOO_NEOBAE_VERSION_STRING
#define FOO_NEOBAE_VERSION_STRING "unknown"
#endif
