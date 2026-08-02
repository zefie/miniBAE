/*
 * Minimal CAST-128 (CAST5) for MobileBAE XMF CDCR unpacker.
 * Algorithm per RFC 2144.
 */
#ifndef NEOBAE_CAST5MINI_H
#define NEOBAE_CAST5MINI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int cast5mini_cbc_decrypt(unsigned char *out, const unsigned char *in, size_t length,
                          const unsigned char *key, size_t keyLen,
                          const unsigned char iv[8]);

#ifdef __cplusplus
}
#endif
#endif
