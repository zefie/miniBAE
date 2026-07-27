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

#ifndef MTHC_DECOMP_H
#define MTHC_DECOMP_H

#include <stdbool.h>
#include <stdint.h>

/* Core MThc processing entrypoint for reuse in NeoBAE.
 * - inputPath: path to MThc input file
 * - extractPath: optional path to write raw MThp payload (NULL to skip)
 * - decompressPath: optional path to write decompressed MIDI (NULL to skip)
 * - verbose: when true, prints payload preview from container report
 * Returns 0 on success, non-zero on error.
 */
int mthc_process_file(char const *inputPath,
                      char const *extractPath,
                      char const *decompressPath,
                      bool verbose);

/* In-memory MThc -> Standard MIDI conversion.
 * On success, *outMidi points to malloc-allocated bytes of size *outMidiSize.
 * Caller must free(*outMidi).
 */
int mthc_decompress_memory(void const *inputData,
                           uint32_t inputSize,
                           unsigned char **outMidi,
                           uint32_t *outMidiSize);

#endif
