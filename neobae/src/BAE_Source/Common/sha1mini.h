/*
 * © 2021–2026 zefie
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

// Minimal SHA1 implementation (compact) for bank file hashing.
// Public domain style (derivative of simplest SHA-1 refs). Not performance critical.
#ifndef MINIBAE_SHA1MINI_H
#define MINIBAE_SHA1MINI_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[5];
    uint64_t count; // bits processed
    unsigned char buffer[64];
} SHA1_CTX_MINI;

void sha1mini_init(SHA1_CTX_MINI *ctx);
void sha1mini_update(SHA1_CTX_MINI *ctx, const unsigned char *data, size_t len);
void sha1mini_final(unsigned char digest[20], SHA1_CTX_MINI *ctx);

static inline void sha1mini(const unsigned char *data, size_t len, unsigned char out[20]){
    SHA1_CTX_MINI c; sha1mini_init(&c); sha1mini_update(&c,data,len); sha1mini_final(out,&c);
}

int sha1mini_file(const char *path, unsigned char out[20]);
int sha1mini_file_hex(const char *path, char out_hex[41]); // out_hex gets NUL terminator

#endif // MINIBAE_SHA1MINI_H
