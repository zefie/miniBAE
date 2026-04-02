/*
 * msvc_compat.h - MSVC compatibility shims for GCC-ism used in BAE sources.
 *
 * Force-included via <ForcedIncludeFiles> in the .vcxproj so that no
 * source files need to be modified.
 *
 * __attribute__((packed)) is a GCC extension.  MSVC uses #pragma pack()
 * instead, which the BAE headers already emit, so we simply swallow the
 * attribute.
 */
#ifdef _MSC_VER

#ifndef __attribute__
#define __attribute__(x)
#endif

/* GCC's __alignof__ → MSVC's __alignof */
#ifndef __alignof__
#define __alignof__(x) __alignof(x)
#endif

#endif /* _MSC_VER */
