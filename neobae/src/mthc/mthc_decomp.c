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

/****************************************************************************
 *
 * middecomp.c
 *
 * Reverse-engineering helper for Nokia MThc/MThp MIDI containers.
 *
 ****************************************************************************/

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mthc_decomp.h"

#define DEFAULT_MIN_MATCH 6
#define DEFAULT_MAX_MATCHES 32
#define DEFAULT_MAX_GAPS 48
#define DEFAULT_MAX_PATTERNS 24
#define DEFAULT_TRACK_MATCHES 8
#define DEFAULT_SUBST_SAMPLE_LIMIT 16
#define DEFAULT_SUBST_MIN_COUNT 2
#define DEFAULT_MAX_PHRASE_CANDIDATES 512
#define DEFAULT_PHRASE_INPUT_MAX GAP_PREVIEW_BYTES
#define DEFAULT_PHRASE_OUTPUT_MAX 24
#define DEFAULT_EXPERIMENTAL_GROWTH 4
#define HEX_PREVIEW_BYTES 96
#define GAP_PREVIEW_BYTES 12

typedef struct FileBuffer
{
    unsigned char *data;
    size_t size;
} FileBuffer;

typedef struct MthcContainer
{
    unsigned char headerBytes[6];
    unsigned char mthpControl[2];
    unsigned char const *payload;
    size_t payloadSize;
} MthcContainer;

typedef struct MidiTrackInfo
{
    size_t offset;
    size_t length;
} MidiTrackInfo;

typedef struct MidiFileInfo
{
    uint16_t format;
    uint16_t trackCount;
    uint16_t division;
    MidiTrackInfo *tracks;
} MidiFileInfo;

typedef struct MatchInfo
{
    size_t payloadOffset;
    size_t referenceOffset;
    size_t length;
} MatchInfo;

typedef struct GapInfo
{
    size_t payloadOffset;
    size_t payloadLength;
    size_t referenceOffset;
    size_t referenceLength;
} GapInfo;

typedef struct GapPatternInfo
{
    unsigned char bytes[GAP_PREVIEW_BYTES];
    size_t length;
    size_t count;
    size_t firstGapIndex;
} GapPatternInfo;

typedef struct ByteSubstitutionCandidate
{
    bool seen;
    bool conflict;
    unsigned char outputByte;
    size_t count;
} ByteSubstitutionCandidate;

typedef struct PhraseSubstitutionCandidate
{
    bool used;
    bool conflict;
    unsigned char inputBytes[DEFAULT_PHRASE_INPUT_MAX];
    unsigned char outputBytes[DEFAULT_PHRASE_OUTPUT_MAX];
    size_t inputLength;
    size_t outputLength;
    size_t count;
} PhraseSubstitutionCandidate;

static uint16_t read_be_u16(unsigned char const *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8) | (uint16_t)data[1]);
}

static uint32_t read_be_u32(unsigned char const *data)
{
    return ((uint32_t)data[0] << 24) |
           ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) |
           (uint32_t)data[3];
}

static void free_file_buffer(FileBuffer *buffer)
{
    if (buffer && buffer->data)
    {
        free(buffer->data);
        buffer->data = NULL;
        buffer->size = 0;
    }
}

static void free_midi_info(MidiFileInfo *info)
{
    if (info && info->tracks)
    {
        free(info->tracks);
        info->tracks = NULL;
        info->trackCount = 0;
    }
}

static int read_entire_file(char const *path, FileBuffer *buffer)
{
    FILE *file;
    long fileSize;

    buffer->data = NULL;
    buffer->size = 0;

    file = fopen(path, "rb");
    if (!file)
    {
        fprintf(stderr, "Error: cannot open '%s': %s\n", path, strerror(errno));
        return 1;
    }

    if (fseek(file, 0, SEEK_END) != 0)
    {
        fprintf(stderr, "Error: cannot seek '%s': %s\n", path, strerror(errno));
        fclose(file);
        return 1;
    }

    fileSize = ftell(file);
    if (fileSize < 0)
    {
        fprintf(stderr, "Error: cannot determine size of '%s': %s\n", path, strerror(errno));
        fclose(file);
        return 1;
    }

    if (fseek(file, 0, SEEK_SET) != 0)
    {
        fprintf(stderr, "Error: cannot rewind '%s': %s\n", path, strerror(errno));
        fclose(file);
        return 1;
    }

    buffer->data = (unsigned char *)malloc((size_t)fileSize);
    if (!buffer->data && fileSize > 0)
    {
        fprintf(stderr, "Error: out of memory allocating %ld bytes\n", fileSize);
        fclose(file);
        return 1;
    }

    if (fileSize > 0 && fread(buffer->data, 1, (size_t)fileSize, file) != (size_t)fileSize)
    {
        fprintf(stderr, "Error: failed reading '%s'\n", path);
        free_file_buffer(buffer);
        fclose(file);
        return 1;
    }

    fclose(file);
    buffer->size = (size_t)fileSize;
    return 0;
}

static int write_entire_file(char const *path, unsigned char const *data, size_t size)
{
    FILE *file = fopen(path, "wb");
    if (!file)
    {
        fprintf(stderr, "Error: cannot create '%s': %s\n", path, strerror(errno));
        return 1;
    }

    if (size > 0 && fwrite(data, 1, size, file) != size)
    {
        fprintf(stderr, "Error: failed writing '%s'\n", path);
        fclose(file);
        return 1;
    }

    if (fclose(file) != 0)
    {
        fprintf(stderr, "Error: failed closing '%s': %s\n", path, strerror(errno));
        return 1;
    }

    return 0;
}

static void print_hex_preview(unsigned char const *data, size_t size, size_t previewBytes)
{
    size_t limit = size < previewBytes ? size : previewBytes;
    size_t i;

    for (i = 0; i < limit; i += 16)
    {
        size_t j;
        printf("%04lx:", (unsigned long)i);
        for (j = 0; j < 16 && (i + j) < limit; ++j)
        {
            printf(" %02x", data[i + j]);
        }
        printf("\n");
    }
}

static void print_hex_bytes(unsigned char const *data, size_t size, size_t maxBytes)
{
    size_t limit = size < maxBytes ? size : maxBytes;
    size_t i;

    if (size == 0)
    {
        printf("-\n");
        return;
    }

    for (i = 0; i < limit; ++i)
    {
        printf("%02x", data[i]);
    }
    if (limit < size)
    {
        printf("...");
    }
    printf("\n");
}

static int compare_match_info(void const *left, void const *right)
{
    MatchInfo const *a = (MatchInfo const *)left;
    MatchInfo const *b = (MatchInfo const *)right;

    if (a->payloadOffset < b->payloadOffset)
    {
        return -1;
    }
    if (a->payloadOffset > b->payloadOffset)
    {
        return 1;
    }
    return 0;
}

/* LZ77 decompression for MThc payloads - exact port of Python reference */
static size_t decompress_mthc_lz77(unsigned char const *input,
                                   size_t input_size,
                                   unsigned char *output,
                                   size_t output_capacity)
{
    size_t out_pos = 0;
    size_t inp_pos = 0;
    uint32_t info_bits = 0;
    uint32_t offset = 1;

    if (!input || !output || input_size == 0 || output_capacity == 0)
    {
        return 0;
    }

    while (inp_pos < input_size)
    {
        /* Read literal bytes */
        while (1)
        {
            if (info_bits & 0x7F)
            {
                info_bits <<= 1;
            }
            else
            {
                if (inp_pos >= input_size)
                {
                    return out_pos;
                }
                uint32_t read_val = input[inp_pos];
                inp_pos++;
                info_bits = (read_val << 1) | 1;
            }

            if (!(info_bits & 0x100))
            {
                break;
            }
            
            if (inp_pos >= input_size || out_pos >= output_capacity)
            {
                return out_pos;
            }
            
            output[out_pos] = input[inp_pos];
            out_pos++;
            inp_pos++;
        }

        /* Decode match offset */
        uint32_t match_off_work = 1;
        while ((info_bits & 0x100) == 0)
        {
            if (info_bits & 0x7F)
            {
                info_bits <<= 1;
            }
            else
            {
                if (inp_pos >= input_size)
                {
                    return out_pos;
                }
                uint32_t read_val = input[inp_pos];
                inp_pos++;
                info_bits = (read_val << 1) | 1;
            }

            match_off_work = ((info_bits >> 8) & 1) + (match_off_work << 1);

            if (info_bits & 0x7F)
            {
                info_bits <<= 1;
            }
            else
            {
                if (inp_pos >= input_size)
                {
                    return out_pos;
                }
                uint32_t read_val = input[inp_pos];
                inp_pos++;
                info_bits = (read_val << 1) | 1;
            }
        }

        if (match_off_work != 2)
        {
            if (inp_pos >= input_size)
            {
                return out_pos;
            }
            uint32_t read_val = input[inp_pos];
            inp_pos++;
            offset = (match_off_work << 8) + read_val - 0x2ff;
            if (offset == 0)
            {
                break;
            }
        }

        /* Decode match count */
        if (info_bits & 0x7F)
        {
            info_bits <<= 1;
        }
        else
        {
            if (inp_pos >= input_size)
            {
                return out_pos;
            }
            uint32_t read_val = input[inp_pos];
            inp_pos++;
            info_bits = (read_val << 1) | 1;
        }

        uint32_t match_count_work_last2 = (info_bits >> 7) & 2;

        if (info_bits & 0x7F)
        {
            info_bits <<= 1;
        }
        else
        {
            if (inp_pos >= input_size)
            {
                return out_pos;
            }
            uint32_t read_val = input[inp_pos];
            inp_pos++;
            info_bits = (read_val << 1) | 1;
        }

        uint32_t match_count_work = ((info_bits >> 8) & 1) + match_count_work_last2;

        if (match_count_work == 0)
        {
            uint32_t match_count_work_read = 1;
            while ((info_bits & 0x100) == 0)
            {
                if (info_bits & 0x7F)
                {
                    info_bits <<= 1;
                }
                else
                {
                    if (inp_pos >= input_size)
                    {
                        return out_pos;
                    }
                    uint32_t read_val = input[inp_pos];
                    inp_pos++;
                    info_bits = (read_val << 1) | 1;
                }

                match_count_work_read = ((info_bits >> 8) & 1) + (match_count_work_read << 1);

                if (info_bits & 0x7F)
                {
                    info_bits <<= 1;
                }
                else
                {
                    if (inp_pos >= input_size)
                    {
                        return out_pos;
                    }
                    uint32_t read_val = input[inp_pos];
                    inp_pos++;
                    info_bits = (read_val << 1) | 1;
                }
            }
            
            match_count_work = match_count_work_read + 2;
        }

        uint32_t count = match_count_work + 1;
        if (offset > 0xD00)
        {
            count += 1;
        }

        /* Copy match */
        if (offset > 0 && offset <= (uint32_t)out_pos)
        {
            size_t start_pos = out_pos - offset;
            size_t i;
            for (i = 0; i < count && out_pos < output_capacity; i++)
            {
                output[out_pos] = output[start_pos];
                out_pos++;
                start_pos++;
            }
        }
    }

    return out_pos;
}

/* Decompress full MThc file with multi-chunk format */
static unsigned char *decompress_mthc_file(FileBuffer const *file,
                                           size_t *out_size,
                                           uint16_t **out_tsizes,
                                           size_t *out_tsize_count)
{
    unsigned char *decompressed = NULL;
    unsigned char *temp_buffer = NULL;
    size_t decompressed_pos = 0;
    size_t decompressed_capacity = 0;
    size_t file_pos = 0;
    uint8_t chunk_count;
    uint8_t chunk_idx;

    if (!file || !out_size || file->size < 5)
    {
        return NULL;
    }

    /* Check header */
    if (memcmp(file->data, "MThc", 4) != 0)
    {
        return NULL;
    }

    file_pos = 4;
    chunk_count = file->data[file_pos++];

    /* Allocate initial buffer (will grow as needed) */
    decompressed_capacity = 65536;
    decompressed = (unsigned char *)malloc(decompressed_capacity);
    if (!decompressed)
    {
        return NULL;
    }

    if (out_tsizes && out_tsize_count) {
        *out_tsize_count = chunk_count;
        *out_tsizes = (uint16_t *)malloc(chunk_count * sizeof(uint16_t));
    }

    /* Decompress each chunk */
    for (chunk_idx = 0; chunk_idx < chunk_count && file_pos + 4 <= file->size; ++chunk_idx)
    {
        uint16_t tsize;  /* target decompressed size */
        uint16_t psize;  /* packed compressed size */
        size_t decomp_size;

        /* Read chunk header */
        tsize = read_be_u16(file->data + file_pos);
        psize = read_be_u16(file->data + file_pos + 2);
        file_pos += 4;
        
        if (out_tsizes && *out_tsizes) {
            (*out_tsizes)[chunk_idx] = tsize;
        }

        if (file_pos + psize > file->size)
        {
            /* Truncated file */
            break;
        }

        /* Grow buffer if necessary */
        while (decompressed_pos + tsize > decompressed_capacity)
        {
            size_t new_capacity = decompressed_capacity * 2;
            unsigned char *new_buffer = (unsigned char *)realloc(decompressed, new_capacity);
            if (!new_buffer)
            {
                free(decompressed);
                return NULL;
            }
            decompressed = new_buffer;
            decompressed_capacity = new_capacity;
        }

        /* Decompress this chunk */
        decomp_size = decompress_mthc_lz77(file->data + file_pos,
                                           psize,
                                           decompressed + decompressed_pos,
                                           decompressed_capacity - decompressed_pos);

        decompressed_pos += decomp_size;
        file_pos += psize;
    }

    *out_size = decompressed_pos;
    return decompressed;
}

/* Assemble a proper MIDI file from decompressed MThc data.
 * 
 * Format of decompressed MThc multi-chunk data:
 * - First chunk starts with MThp header containing MIDI format info
 * - All chunks have: [2-byte LE size] [size bytes of track data]
 * - Accumulated track data starts with: [2-byte LE track_count]
 * - Followed by each track: [2-byte BE size] [size bytes of track event data]
 */
static unsigned char *assemble_midi_from_decompressed(unsigned char const *decompressed,
                                                       size_t decomp_size,
                                                       uint16_t *tsizes,
                                                       size_t chunk_count,
                                                       size_t *out_size)
{
    unsigned char *midi_out = NULL;
    size_t midi_pos = 0;
    size_t midi_capacity = 0;
    size_t pos = 0;
    uint16_t pack_track_count;
    uint16_t track_idx;

    if (!decompressed || !out_size || decomp_size < 18)
    {
        return NULL;
    }

    /* Allocate output MIDI buffer (with some headroom) */
    midi_capacity = decomp_size + 2048;
    midi_out = (unsigned char *)malloc(midi_capacity);
    if (!midi_out)
    {
        return NULL;
    }

    /* Check for MThp marker in first chunk */
    if (memcmp(decompressed, "MThp", 4) != 0)
    {
        free(midi_out);
        return NULL;
    }

    /* Write MThd marker and copy BASIC 10 bytes from MThp */
    memcpy(midi_out, "MThd", 4);
    midi_pos = 4;
    
    /* Copy header length (4 bytes, already big-endian) */
    memcpy(midi_out + midi_pos, decompressed + 4, 4);
    midi_pos += 4;
    
    /* Copy format from MThp header */
    midi_out[midi_pos++] = decompressed[8];
    midi_out[midi_pos++] = decompressed[9];
    
    /* Extract track count from decompressed header at offset 10-11 */
    pack_track_count = read_be_u16(decompressed + 10);
    midi_out[midi_pos++] = (pack_track_count >> 8) & 0xFF;
    midi_out[midi_pos++] = pack_track_count & 0xFF;
    
    /* Copy division (2 bytes, big-endian) */
    memcpy(midi_out + midi_pos, decompressed + 12, 2);
    midi_pos += 2;
    
    /* Extract payload data from each decompressed block.
     * The Multi-Track Chunk format:
     * - [2-byte native Endian size of chunk]
     * - [2-byte native Endian number of tracks in this chunk]
     * - For each track:
     *   - [2-byte Big Endian size of track data]
     *   - [data bytes]
     * padding to tsizes[i] follows.
     */
    
    unsigned char **tracks_data = (unsigned char **)calloc(pack_track_count, sizeof(unsigned char *));
    size_t *tracks_size = (size_t *)calloc(pack_track_count, sizeof(size_t));
    size_t *tracks_capacity = (size_t *)calloc(pack_track_count, sizeof(size_t));

    if (!tracks_data || !tracks_size || !tracks_capacity)
    {
        free(midi_out);
        if (tracks_data) free(tracks_data);
        if (tracks_size) free(tracks_size);
        if (tracks_capacity) free(tracks_capacity);
        return NULL;
    }

    size_t block_offset = 0;
    for (size_t ci = 0; ci < chunk_count; ++ci)
    {
        size_t ptr = block_offset;
        if (ptr >= decomp_size) break;

        /* Skip MThp header in first block */
        if (ci == 0)
        {
            ptr += 16;
        }

        if (ptr + 4 > decomp_size) break;

        /* Detect endianness using track count */
        uint16_t le_tc = decompressed[ptr + 2] | (decompressed[ptr + 3] << 8);
        uint16_t be_tc = (decompressed[ptr + 2] << 8) | decompressed[ptr + 3];
        
        bool is_le = false;
        if (le_tc == pack_track_count)
            is_le = true;
        else if (be_tc == pack_track_count)
            is_le = false;
        else
            is_le = (decompressed[ptr + 3] == 0); /* fallback heuristic */

        uint16_t tc = is_le ? le_tc : be_tc;
        uint16_t c_size = is_le ? (decompressed[ptr] | (decompressed[ptr + 1] << 8)) : ((decompressed[ptr] << 8) | decompressed[ptr + 1]);
        (void)c_size;
        
        ptr += 4;
        
        for (uint16_t t = 0; t < tc; ++t)
        {
            if (ptr + 2 > decomp_size) break;
            /* Track length is ALWAYS Big Endian */
            uint16_t t_len = (decompressed[ptr] << 8) | decompressed[ptr + 1];
            ptr += 2;
            
            if (ptr + t_len > decomp_size) break;
            if (t >= pack_track_count)
            {
                ptr += t_len;
                continue;
            }

            /* Allocate or grow track buffer */
            if (tracks_size[t] + t_len > tracks_capacity[t])
            {
                size_t new_cap = tracks_capacity[t] == 0 ? (t_len + 1024) : (tracks_capacity[t] * 2 + t_len);
                unsigned char *new_buf = (unsigned char *)realloc(tracks_data[t], new_cap);
                if (!new_buf) break;
                tracks_data[t] = new_buf;
                tracks_capacity[t] = new_cap;
            }
            
            memcpy(tracks_data[t] + tracks_size[t], decompressed + ptr, t_len);
            tracks_size[t] += t_len;
            ptr += t_len;
        }

        block_offset += tsizes[ci];
    }

    /* Extract tracks and write output */
    for (track_idx = 0; track_idx < pack_track_count; ++track_idx)
    {
        uint32_t track_size = tracks_size[track_idx] ? (uint32_t)tracks_size[track_idx] : 4;
        bool synthetic_eot = (tracks_size[track_idx] == 0);
        /* Ensure space in output */
        if (midi_pos + 8 + track_size > midi_capacity)
        {
            size_t new_cap = midi_capacity * 2 + track_size + 8;
            unsigned char *new_midi = (unsigned char *)realloc(midi_out, new_cap);
            if (!new_midi)
            {
                for (uint16_t i = 0; i < pack_track_count; ++i) if (tracks_data[i]) free(tracks_data[i]);
                free(tracks_data); free(tracks_size); free(tracks_capacity);
                free(midi_out);
                return NULL;
            }
            midi_out = new_midi;
            midi_capacity = new_cap;
        }

        /* Write MTrk header */
        memcpy(midi_out + midi_pos, "MTrk", 4);
        midi_pos += 4;

        /* Write track size (big-endian 32-bit) */
        midi_out[midi_pos++] = (track_size >> 24) & 0xFF;
        midi_out[midi_pos++] = (track_size >> 16) & 0xFF;
        midi_out[midi_pos++] = (track_size >> 8) & 0xFF;
        midi_out[midi_pos++] = track_size & 0xFF;

        /* Copy track data */
        if (synthetic_eot)
        {
            midi_out[midi_pos + 0] = 0x00;
            midi_out[midi_pos + 1] = 0xFF;
            midi_out[midi_pos + 2] = 0x2F;
            midi_out[midi_pos + 3] = 0x00;
        }
        else
        {
            memcpy(midi_out + midi_pos, tracks_data[track_idx], track_size);
        }
        midi_pos += track_size;
    }

    for (uint16_t i = 0; i < pack_track_count; ++i)
    {
        if (tracks_data[i]) free(tracks_data[i]);
    }
    free(tracks_data);
    free(tracks_size);
    free(tracks_capacity);

    *out_size = midi_pos;
    return midi_out;
}

static int parse_mthc_container(FileBuffer const *file, MthcContainer *container)
{
    if (!file || !container || file->size < 16)
    {
        fprintf(stderr, "Error: file is too small to be an MThc container\n");
        return 1;
    }

    if (memcmp(file->data, "MThc", 4) != 0)
    {
        fprintf(stderr, "Error: file does not start with MThc\n");
        return 1;
    }
    if (memcmp(file->data + 10, "MThp", 4) != 0)
    {
        fprintf(stderr, "Error: expected MThp tag at offset 10\n");
        return 1;
    }

    memcpy(container->headerBytes, file->data + 4, sizeof(container->headerBytes));
    memcpy(container->mthpControl, file->data + 14, sizeof(container->mthpControl));
    container->payload = file->data + 16;
    container->payloadSize = file->size - 16;
    return 0;
}

static void print_mthc_report(MthcContainer const *container, FileBuffer const *file, bool verbose)
{
    printf("MThc container\n");
    printf("  file size      : %lu bytes\n", (unsigned long)file->size);
    printf("  outer header   : %02x %02x %02x %02x %02x %02x\n",
           container->headerBytes[0], container->headerBytes[1], container->headerBytes[2],
           container->headerBytes[3], container->headerBytes[4], container->headerBytes[5]);
    printf("  MThp control   : %02x %02x\n",
           container->mthpControl[0], container->mthpControl[1]);
    printf("  payload offset : 16\n");
    printf("  payload size   : %lu bytes\n", (unsigned long)container->payloadSize);

    if (verbose)
    {
        printf("  payload preview\n");
        print_hex_preview(container->payload, container->payloadSize, HEX_PREVIEW_BYTES);
    }
}

static int parse_standard_midi(FileBuffer const *file, MidiFileInfo *info)
{
    size_t offset;
    uint32_t headerLength;
    uint16_t trackIndex;

    if (!file || !info || file->size < 14)
    {
        fprintf(stderr, "Error: MIDI reference is too small\n");
        return 1;
    }
    if (memcmp(file->data, "MThd", 4) != 0)
    {
        fprintf(stderr, "Error: reference file is not a standard MIDI file\n");
        return 1;
    }

    headerLength = read_be_u32(file->data + 4);
    if (headerLength < 6 || file->size < 8 + headerLength)
    {
        fprintf(stderr, "Error: invalid MIDI header length\n");
        return 1;
    }

    info->format = read_be_u16(file->data + 8);
    info->trackCount = read_be_u16(file->data + 10);
    info->division = read_be_u16(file->data + 12);
    info->tracks = NULL;

    if (info->trackCount == 0)
    {
        return 0;
    }

    info->tracks = (MidiTrackInfo *)calloc(info->trackCount, sizeof(MidiTrackInfo));
    if (!info->tracks)
    {
        fprintf(stderr, "Error: out of memory allocating %u track records\n", info->trackCount);
        return 1;
    }

    offset = 8 + headerLength;
    for (trackIndex = 0; trackIndex < info->trackCount; ++trackIndex)
    {
        uint32_t trackLength;

        if ((offset + 8) > file->size || memcmp(file->data + offset, "MTrk", 4) != 0)
        {
            fprintf(stderr, "Error: invalid or missing MTrk chunk at track %u\n", trackIndex);
            free_midi_info(info);
            return 1;
        }

        trackLength = read_be_u32(file->data + offset + 4);
        offset += 8;
        if ((offset + trackLength) > file->size)
        {
            fprintf(stderr, "Error: track %u exceeds file size\n", trackIndex);
            free_midi_info(info);
            return 1;
        }

        info->tracks[trackIndex].offset = offset;
        info->tracks[trackIndex].length = trackLength;
        offset += trackLength;
    }

    return 0;
}

static void print_midi_report(MidiFileInfo const *info)
{
    uint16_t i;

    printf("Reference MIDI\n");
    printf("  format         : %u\n", info->format);
    printf("  track count    : %u\n", info->trackCount);
    printf("  division       : %u\n", info->division);
    for (i = 0; i < info->trackCount; ++i)
    {
        printf("  track %u length : %lu bytes\n",
               (unsigned)i,
               (unsigned long)info->tracks[i].length);
    }
}

static unsigned char *flatten_track_payloads(FileBuffer const *file,
                                             MidiFileInfo const *info,
                                             size_t *outSize)
{
    unsigned char *flat;
    size_t totalSize = 0;
    uint16_t i;
    size_t cursor = 0;

    *outSize = 0;

    for (i = 0; i < info->trackCount; ++i)
    {
        totalSize += info->tracks[i].length;
    }

    flat = (unsigned char *)malloc(totalSize);
    if (!flat && totalSize > 0)
    {
        return NULL;
    }

    for (i = 0; i < info->trackCount; ++i)
    {
        memcpy(flat + cursor,
               file->data + info->tracks[i].offset,
               info->tracks[i].length);
        cursor += info->tracks[i].length;
    }

    *outSize = totalSize;
    return flat;
}

static size_t find_subsequence(unsigned char const *haystack,
                               size_t haystackSize,
                               unsigned char const *needle,
                               size_t needleSize,
                               size_t startOffset)
{
    size_t i;

    if (needleSize == 0 || haystackSize < needleSize || startOffset >= haystackSize)
    {
        return (size_t)-1;
    }

    for (i = startOffset; (i + needleSize) <= haystackSize; ++i)
    {
        if (memcmp(haystack + i, needle, needleSize) == 0)
        {
            return i;
        }
    }

    return (size_t)-1;
}

static size_t extend_match(unsigned char const *left,
                           size_t leftSize,
                           size_t leftOffset,
                           unsigned char const *right,
                           size_t rightSize,
                           size_t rightOffset)
{
    size_t length = 0;

    while ((leftOffset + length) < leftSize &&
           (rightOffset + length) < rightSize &&
           left[leftOffset + length] == right[rightOffset + length])
    {
        ++length;
    }
    return length;
}

static size_t collect_literal_matches(unsigned char const *payload,
                                      size_t payloadSize,
                                      unsigned char const *reference,
                                      size_t referenceSize,
                                      size_t minMatch,
                                      size_t maxMatches,
                                      MatchInfo *matches)
{
    size_t found = 0;
    size_t referenceOffset = 0;
    size_t payloadSearchOffset = 0;

    while (referenceOffset + minMatch <= referenceSize && found < maxMatches)
    {
        size_t payloadOffset = find_subsequence(payload,
                                                payloadSize,
                                                reference + referenceOffset,
                                                minMatch,
                                                payloadSearchOffset);
        if (payloadOffset == (size_t)-1)
        {
            ++referenceOffset;
            continue;
        }

        matches[found].payloadOffset = payloadOffset;
        matches[found].referenceOffset = referenceOffset;
        matches[found].length = extend_match(payload,
                                             payloadSize,
                                             payloadOffset,
                                             reference,
                                             referenceSize,
                                             referenceOffset);
        payloadSearchOffset = payloadOffset + matches[found].length;
        referenceOffset += matches[found].length;
        ++found;
    }

    return found;
}

static size_t collect_gap_info(MatchInfo const *matches,
                               size_t matchCount,
                               size_t payloadSize,
                               size_t referenceSize,
                               GapInfo *gaps,
                               size_t maxGaps)
{
    size_t gapCount = 0;
    size_t previousPayloadEnd = 0;
    size_t previousReferenceEnd = 0;
    size_t i;

    for (i = 0; i < matchCount && gapCount < maxGaps; ++i)
    {
        size_t payloadGapLength = 0;
        size_t referenceGapLength = 0;

        if (matches[i].payloadOffset >= previousPayloadEnd)
        {
            payloadGapLength = matches[i].payloadOffset - previousPayloadEnd;
        }
        if (matches[i].referenceOffset >= previousReferenceEnd)
        {
            referenceGapLength = matches[i].referenceOffset - previousReferenceEnd;
        }

        if (payloadGapLength != 0 || referenceGapLength != 0)
        {
            gaps[gapCount].payloadOffset = previousPayloadEnd;
            gaps[gapCount].payloadLength = payloadGapLength;
            gaps[gapCount].referenceOffset = previousReferenceEnd;
            gaps[gapCount].referenceLength = referenceGapLength;
            ++gapCount;
        }

        previousPayloadEnd = matches[i].payloadOffset + matches[i].length;
        previousReferenceEnd = matches[i].referenceOffset + matches[i].length;
    }

    if ((previousPayloadEnd < payloadSize || previousReferenceEnd < referenceSize) && gapCount < maxGaps)
    {
        gaps[gapCount].payloadOffset = previousPayloadEnd;
        gaps[gapCount].payloadLength = payloadSize - previousPayloadEnd;
        gaps[gapCount].referenceOffset = previousReferenceEnd;
        gaps[gapCount].referenceLength = referenceSize - previousReferenceEnd;
        ++gapCount;
    }

    return gapCount;
}

static void print_gap_report(GapInfo const *gaps,
                             size_t gapCount,
                             unsigned char const *payload,
                             unsigned char const *reference)
{
    size_t i;

    if (gapCount == 0)
    {
        printf("Control gaps\n");
        printf("  none found between literal matches\n");
        return;
    }

    printf("Control gaps\n");
    for (i = 0; i < gapCount; ++i)
    {
        printf("  gap %lu payload@%lu len=%lu -> ",
               (unsigned long)i,
               (unsigned long)gaps[i].payloadOffset,
               (unsigned long)gaps[i].payloadLength);
        print_hex_bytes(payload + gaps[i].payloadOffset,
                        gaps[i].payloadLength,
                        GAP_PREVIEW_BYTES);
        printf("       ref@%lu len=%lu -> ",
               (unsigned long)gaps[i].referenceOffset,
               (unsigned long)gaps[i].referenceLength);
        print_hex_bytes(reference + gaps[i].referenceOffset,
                        gaps[i].referenceLength,
                        GAP_PREVIEW_BYTES);
    }
}

static size_t collect_gap_patterns(GapInfo const *gaps,
                                   size_t gapCount,
                                   unsigned char const *payload,
                                   GapPatternInfo *patterns,
                                   size_t maxPatterns)
{
    size_t patternCount = 0;
    size_t i;

    for (i = 0; i < gapCount; ++i)
    {
        size_t j;
        size_t storedLength;

        if (gaps[i].payloadLength == 0 || gaps[i].payloadLength > GAP_PREVIEW_BYTES)
        {
            continue;
        }

        for (j = 0; j < patternCount; ++j)
        {
            if (patterns[j].length == gaps[i].payloadLength &&
                memcmp(patterns[j].bytes,
                       payload + gaps[i].payloadOffset,
                       gaps[i].payloadLength) == 0)
            {
                patterns[j].count += 1;
                break;
            }
        }

        if (j < patternCount)
        {
            continue;
        }
        if (patternCount >= maxPatterns)
        {
            continue;
        }

        storedLength = gaps[i].payloadLength;
        memset(patterns[patternCount].bytes, 0, sizeof(patterns[patternCount].bytes));
        memcpy(patterns[patternCount].bytes,
               payload + gaps[i].payloadOffset,
               storedLength);
        patterns[patternCount].length = storedLength;
        patterns[patternCount].count = 1;
        patterns[patternCount].firstGapIndex = i;
        ++patternCount;
    }

    return patternCount;
}

static void print_gap_patterns(GapPatternInfo const *patterns,
                               size_t patternCount,
                               GapInfo const *gaps,
                               unsigned char const *reference)
{
    size_t i;

    if (patternCount == 0)
    {
        printf("Repeated payload patterns\n");
        printf("  no repeated short payload gaps found\n");
        return;
    }

    printf("Repeated payload patterns\n");
    for (i = 0; i < patternCount; ++i)
    {
        GapInfo const *exampleGap = &gaps[patterns[i].firstGapIndex];

        printf("  count=%lu bytes=",
               (unsigned long)patterns[i].count);
        print_hex_bytes(patterns[i].bytes, patterns[i].length, GAP_PREVIEW_BYTES);
        printf("       example ref len=%lu -> ",
               (unsigned long)exampleGap->referenceLength);
        print_hex_bytes(reference + exampleGap->referenceOffset,
                        exampleGap->referenceLength,
                        GAP_PREVIEW_BYTES);
    }
}

static void add_substitution_candidates(ByteSubstitutionCandidate *table,
                                        unsigned char const *payload,
                                        unsigned char const *reference,
                                        GapInfo const *gaps,
                                        size_t gapCount,
                                        size_t maxEqualLengthGap)
{
    size_t i;

    for (i = 0; i < gapCount; ++i)
    {
        size_t j;

        if (gaps[i].payloadLength == 0 || gaps[i].payloadLength != gaps[i].referenceLength)
        {
            continue;
        }
        if (gaps[i].payloadLength > maxEqualLengthGap)
        {
            continue;
        }

        for (j = 0; j < gaps[i].payloadLength; ++j)
        {
            unsigned char inputByte = payload[gaps[i].payloadOffset + j];
            unsigned char outputByte = reference[gaps[i].referenceOffset + j];

            if (!table[inputByte].seen)
            {
                table[inputByte].seen = true;
                table[inputByte].outputByte = outputByte;
                table[inputByte].count = 1;
            }
            else if (table[inputByte].outputByte == outputByte)
            {
                table[inputByte].count += 1;
            }
            else
            {
                table[inputByte].conflict = true;
            }
        }
    }
}

static void print_substitution_candidates(ByteSubstitutionCandidate const *table)
{
    size_t uniqueCount = 0;
    size_t conflictCount = 0;
    size_t i;

    printf("Substitution candidates\n");
    for (i = 0; i < 256; ++i)
    {
        if (!table[i].seen)
        {
            continue;
        }
        if (table[i].conflict)
        {
            conflictCount += 1;
            continue;
        }
        uniqueCount += 1;
    }

    printf("  unique mappings  : %lu\n", (unsigned long)uniqueCount);
    printf("  conflicting bytes: %lu\n", (unsigned long)conflictCount);
    for (i = 0; i < 256; ++i)
    {
        if (!table[i].seen || table[i].conflict)
        {
            continue;
        }
        printf("  %02lx -> %02x (%lu samples)\n",
               (unsigned long)i,
               table[i].outputByte,
               (unsigned long)table[i].count);
    }
}

static void add_phrase_substitution_candidates(PhraseSubstitutionCandidate *table,
                                               size_t tableSize,
                                               unsigned char const *payload,
                                               unsigned char const *reference,
                                               GapInfo const *gaps,
                                               size_t gapCount)
{
    size_t i;

    for (i = 0; i < gapCount; ++i)
    {
        size_t slot;

        if (gaps[i].payloadLength == 0 || gaps[i].referenceLength == 0)
        {
            continue;
        }
        if (gaps[i].payloadLength > DEFAULT_PHRASE_INPUT_MAX ||
            gaps[i].referenceLength > DEFAULT_PHRASE_OUTPUT_MAX)
        {
            continue;
        }

        for (slot = 0; slot < tableSize; ++slot)
        {
            if (!table[slot].used)
            {
                break;
            }
            if (table[slot].inputLength == gaps[i].payloadLength &&
                table[slot].outputLength == gaps[i].referenceLength &&
                memcmp(table[slot].inputBytes,
                       payload + gaps[i].payloadOffset,
                       gaps[i].payloadLength) == 0)
            {
                if (memcmp(table[slot].outputBytes,
                           reference + gaps[i].referenceOffset,
                           gaps[i].referenceLength) == 0)
                {
                    table[slot].count += 1;
                }
                else
                {
                    table[slot].conflict = true;
                }
                break;
            }
        }

        if (slot >= tableSize || table[slot].used)
        {
            continue;
        }

        table[slot].used = true;
        table[slot].conflict = false;
         table[slot].inputLength = gaps[i].payloadLength;
         table[slot].outputLength = gaps[i].referenceLength;
        table[slot].count = 1;
         memset(table[slot].inputBytes, 0, sizeof(table[slot].inputBytes));
         memset(table[slot].outputBytes, 0, sizeof(table[slot].outputBytes));
        memcpy(table[slot].inputBytes,
               payload + gaps[i].payloadOffset,
               gaps[i].payloadLength);
        memcpy(table[slot].outputBytes,
               reference + gaps[i].referenceOffset,
               gaps[i].referenceLength);
    }
}

static size_t apply_experimental_substitutions(unsigned char const *input,
                                               size_t inputSize,
                                               unsigned char *output,
                                 size_t outputCapacity,
                                               ByteSubstitutionCandidate const *byteTable,
                                               PhraseSubstitutionCandidate const *phraseTable,
                                               size_t phraseTableSize,
                                               size_t byteMinCount,
                                               size_t phraseMinCount)
{
    size_t inPos = 0;
    size_t outPos = 0;

    while (inPos < inputSize)
    {
        size_t bestSlot = (size_t)-1;
        size_t bestLen = 0;
        size_t slot;

        for (slot = 0; slot < phraseTableSize; ++slot)
        {
            if (!phraseTable[slot].used || phraseTable[slot].conflict)
            {
                continue;
            }
            if (phraseTable[slot].count < phraseMinCount)
            {
                continue;
            }
            if (phraseTable[slot].inputLength < 1)
            {
                continue;
            }
            if (phraseTable[slot].inputLength > bestLen &&
                (inPos + phraseTable[slot].inputLength) <= inputSize &&
                (outPos + phraseTable[slot].outputLength) <= outputCapacity &&
                memcmp(input + inPos,
                       phraseTable[slot].inputBytes,
                       phraseTable[slot].inputLength) == 0)
            {
                bestSlot = slot;
                bestLen = phraseTable[slot].inputLength;
            }
        }

        if (bestSlot != (size_t)-1)
        {
            memcpy(output + outPos,
                   phraseTable[bestSlot].outputBytes,
                   phraseTable[bestSlot].outputLength);
            inPos += phraseTable[bestSlot].inputLength;
            outPos += phraseTable[bestSlot].outputLength;
            continue;
        }

        if (outPos >= outputCapacity)
        {
            break;
        }
        if (byteTable[input[inPos]].seen &&
            !byteTable[input[inPos]].conflict &&
            byteTable[input[inPos]].count >= byteMinCount)
        {
            output[outPos] = byteTable[input[inPos]].outputByte;
        }
        else
        {
            output[outPos] = input[inPos];
        }
        inPos += 1;
        outPos += 1;
    }

    return outPos;
}

static void print_phrase_candidates(PhraseSubstitutionCandidate const *phraseTable,
                                    size_t phraseTableSize,
                                    size_t phraseMinCount)
{
    size_t i;
    size_t shown = 0;

    printf("Phrase candidates\n");
    for (i = 0; i < phraseTableSize; ++i)
    {
        if (!phraseTable[i].used || phraseTable[i].conflict)
        {
            continue;
        }
        if (phraseTable[i].count < phraseMinCount)
        {
            continue;
        }

        printf("  count=%lu in=",
               (unsigned long)phraseTable[i].count);
        print_hex_bytes(phraseTable[i].inputBytes,
                        phraseTable[i].inputLength,
                        DEFAULT_PHRASE_INPUT_MAX);
        printf("       out=");
        print_hex_bytes(phraseTable[i].outputBytes,
                        phraseTable[i].outputLength,
                        DEFAULT_PHRASE_OUTPUT_MAX);
        shown += 1;
        if (shown >= 40)
        {
            break;
        }
    }
    if (shown == 0)
    {
        printf("  none at current confidence threshold\n");
    }
}

static void print_experimental_decode_report(unsigned char const *payload,
                                             size_t payloadSize,
                                             unsigned char const *reference,
                                             size_t referenceSize,
                                             ByteSubstitutionCandidate const *byteTable,
                                             PhraseSubstitutionCandidate const *phraseTable,
                                             size_t phraseTableSize,
                                             size_t prefixBytes,
                                             size_t byteMinCount,
                                             size_t phraseMinCount,
                                             char const *decodedOutputPath,
                                             size_t payloadStartOffset,
                                             size_t referenceStartOffset)
{
    unsigned char *decoded;
    size_t decodeSize;
    size_t decodedSize;
    size_t decodeCapacity;
    size_t compareSize;
    size_t baselineEqual = 0;
    size_t decodedEqual = 0;
    size_t i;

    if (payloadStartOffset >= payloadSize || referenceStartOffset >= referenceSize)
    {
        printf("Experimental decode\n");
        printf("  skipped (anchor offsets out of range)\n");
        return;
    }

    decodeSize = prefixBytes;
    if (decodeSize > (payloadSize - payloadStartOffset))
    {
        decodeSize = payloadSize - payloadStartOffset;
    }
    if (decodeSize == 0)
    {
        printf("Experimental decode\n");
        printf("  skipped (prefix size is zero)\n");
        return;
    }

    decodeCapacity = decodeSize * DEFAULT_EXPERIMENTAL_GROWTH;
    if (decodeCapacity < decodeSize)
    {
        decodeCapacity = decodeSize;
    }
    decoded = (unsigned char *)malloc(decodeCapacity);
    if (!decoded)
    {
        printf("Experimental decode\n");
        printf("  skipped (out of memory)\n");
        return;
    }

    decodedSize = apply_experimental_substitutions(payload + payloadStartOffset,
                                                   decodeSize,
                                                   decoded,
                                                   decodeCapacity,
                                                   byteTable,
                                                   phraseTable,
                                                   phraseTableSize,
                                                   byteMinCount,
                                                   phraseMinCount);

    compareSize = decodeSize < (referenceSize - referenceStartOffset)
        ? decodeSize
        : (referenceSize - referenceStartOffset);
    for (i = 0; i < compareSize; ++i)
    {
        if (payload[payloadStartOffset + i] == reference[referenceStartOffset + i])
        {
            baselineEqual += 1;
        }
        if (i < decodedSize && decoded[i] == reference[referenceStartOffset + i])
        {
            decodedEqual += 1;
        }
    }

    printf("Experimental decode\n");
    printf("  payload start    : %lu\n", (unsigned long)payloadStartOffset);
    printf("  reference start  : %lu\n", (unsigned long)referenceStartOffset);
    printf("  prefix bytes     : %lu\n", (unsigned long)decodeSize);
    printf("  decoded bytes    : %lu\n", (unsigned long)decodedSize);
    printf("  baseline matches : %lu / %lu\n",
           (unsigned long)baselineEqual,
           (unsigned long)compareSize);
    printf("  decoded matches  : %lu / %lu\n",
           (unsigned long)decodedEqual,
           (unsigned long)compareSize);
    printf("  payload preview  : ");
    print_hex_bytes(payload + payloadStartOffset, decodeSize, GAP_PREVIEW_BYTES);
    printf("  decoded preview  : ");
    print_hex_bytes(decoded, decodedSize, GAP_PREVIEW_BYTES);
    printf("  reference preview: ");
    print_hex_bytes(reference + referenceStartOffset, compareSize, GAP_PREVIEW_BYTES);

    if (decodedOutputPath)
    {
        if (write_entire_file(decodedOutputPath, decoded, decodedSize) == 0)
        {
            printf("  decoded output   : %s (%lu bytes)\n",
                   decodedOutputPath,
                   (unsigned long)decodedSize);
        }
        else
        {
            printf("  decoded output   : failed to write %s\n", decodedOutputPath);
        }
    }

    free(decoded);
}

static void print_track_aware_report(unsigned char const *payload,
                                     size_t payloadSize,
                                     FileBuffer const *reference,
                                     MidiFileInfo const *midiInfo)
{
    size_t payloadCursor = 0;
    uint16_t trackIndex;

    printf("Track-aware comparison\n");
    for (trackIndex = 0; trackIndex < midiInfo->trackCount; ++trackIndex)
    {
        unsigned char const *trackData = reference->data + midiInfo->tracks[trackIndex].offset;
        size_t trackSize = midiInfo->tracks[trackIndex].length;
        MatchInfo matches[DEFAULT_TRACK_MATCHES + 1];
        size_t matchCount;
        size_t shown;

        if (payloadCursor >= payloadSize)
        {
            printf("  track %u: payload exhausted\n", trackIndex);
            break;
        }

        matchCount = collect_literal_matches(payload + payloadCursor,
                                             payloadSize - payloadCursor,
                                             trackData,
                                             trackSize,
                                             4,
                                             DEFAULT_TRACK_MATCHES,
                                             matches);
        if (matchCount == 0)
        {
            printf("  track %u: no >=4-byte match found from payload offset %lu\n",
                   (unsigned)trackIndex,
                   (unsigned long)payloadCursor);
            continue;
        }

        qsort(matches, matchCount, sizeof(MatchInfo), compare_match_info);
        printf("  track %u length=%lu\n",
               (unsigned)trackIndex,
               (unsigned long)trackSize);
        shown = matchCount < DEFAULT_TRACK_MATCHES ? matchCount : DEFAULT_TRACK_MATCHES;
        if (shown > 0)
        {
            size_t matchIdx;
            for (matchIdx = 0; matchIdx < shown; ++matchIdx)
            {
                size_t absolutePayloadOffset = payloadCursor + matches[matchIdx].payloadOffset;
                printf("    match payload=%lu track=%lu len=%lu bytes=",
                       (unsigned long)absolutePayloadOffset,
                       (unsigned long)matches[matchIdx].referenceOffset,
                       (unsigned long)matches[matchIdx].length);
                print_hex_bytes(trackData + matches[matchIdx].referenceOffset,
                                matches[matchIdx].length,
                                GAP_PREVIEW_BYTES);
            }
            payloadCursor += matches[0].payloadOffset + matches[0].length;
        }
    }
}

static void print_literal_matches(MatchInfo const *matches,
                                  size_t matchCount,
                                  unsigned char const *reference,
                                  size_t referenceSize)
{
    size_t i;

    if (matchCount == 0)
    {
        printf("Literal matches\n");
        printf("  none found with the current minimum match length\n");
        return;
    }

    printf("Literal matches\n");
    for (i = 0; i < matchCount; ++i)
    {
        size_t previewLength = matches[i].length < 8 ? matches[i].length : 8;
        size_t j;

        printf("  payload %lu <-> reference %lu, %lu bytes, preview ",
               (unsigned long)matches[i].payloadOffset,
               (unsigned long)matches[i].referenceOffset,
               (unsigned long)matches[i].length);
        for (j = 0; j < previewLength && matches[i].referenceOffset + j < referenceSize; ++j)
        {
            printf("%02x", reference[matches[i].referenceOffset + j]);
        }
        printf("\n");
    }
}

/* --------------------------------------------------------------------------
 * Full-trace: greedy byte-by-byte alignment of payload vs full decomp file.
 *
 * Walks the payload sequentially.  For each byte it checks:
 *   1. Does this byte match the next expected reference byte?  → literal
 *   2. Otherwise → mark as control, then scan ahead in both streams to find
 *      the next synchronisation point (next literal run of >= syncLen bytes).
 *
 * Produces a detailed log of literal runs and control regions that shows
 * exactly which payload bytes are opcodes and what reference bytes they
 * correspond to (if any).
 * ------------------------------------------------------------------------ */

#define TRACE_SYNC_LEN 4        /* minimum literal run to re-sync */
#define TRACE_SCAN_LIMIT 256    /* how far ahead to look for sync */
#define TRACE_MAX_EVENTS 4096   /* max trace events to report */

typedef struct TraceEvent
{
    size_t payloadOffset;
    size_t payloadLength;
    size_t referenceOffset;
    size_t referenceLength;
    bool isLiteral;
} TraceEvent;

/* Find the next position where payload[pOff..] matches reference[rOff..] for
 * at least syncLen bytes.  Searches by advancing the reference cursor first
 * (since the payload is typically denser than the reference due to control
 * bytes), then falls back to scanning both.  Returns 1 on success. */
static int find_sync_point(unsigned char const *payload, size_t payloadSize,
                           unsigned char const *ref, size_t refSize,
                           size_t pStart, size_t rStart,
                           size_t syncLen, size_t scanLimit,
                           size_t *pSync, size_t *rSync)
{
    size_t pd, rd;

    /* Try advancing only the reference (payload byte is a control byte that
     * consumed no payload but emitted reference bytes). */
    for (rd = 0; rd < scanLimit && (rStart + rd + syncLen) <= refSize; ++rd)
    {
        if ((pStart + syncLen) <= payloadSize &&
            memcmp(payload + pStart, ref + rStart + rd, syncLen) == 0)
        {
            *pSync = pStart;
            *rSync = rStart + rd;
            return 1;
        }
    }

    /* Try advancing only the payload (control bytes that produce no output). */
    for (pd = 1; pd < scanLimit && (pStart + pd + syncLen) <= payloadSize; ++pd)
    {
        if ((rStart + syncLen) <= refSize &&
            memcmp(payload + pStart + pd, ref + rStart, syncLen) == 0)
        {
            *pSync = pStart + pd;
            *rSync = rStart;
            return 1;
        }
    }

    /* Try advancing both. */
    for (pd = 1; pd < scanLimit && (pStart + pd + syncLen) <= payloadSize; ++pd)
    {
        for (rd = 1; rd < scanLimit && (rStart + rd + syncLen) <= refSize; ++rd)
        {
            if (memcmp(payload + pStart + pd, ref + rStart + rd, syncLen) == 0)
            {
                *pSync = pStart + pd;
                *rSync = rStart + rd;
                return 1;
            }
        }
    }

    return 0;
}

static void print_full_trace(unsigned char const *payload, size_t payloadSize,
                              unsigned char const *ref, size_t refSize,
                              size_t traceLimit)
{
    TraceEvent events[TRACE_MAX_EVENTS];
    size_t eventCount = 0;
    size_t pPos = 0;
    size_t rPos = 0;
    size_t tracedPayloadBytes = 0;
    size_t i;

    printf("Full trace (payload %lu bytes, reference %lu bytes, limit %lu)\n",
           (unsigned long)payloadSize, (unsigned long)refSize,
           (unsigned long)traceLimit);

    while (pPos < payloadSize && rPos < refSize && tracedPayloadBytes < traceLimit &&
           eventCount < TRACE_MAX_EVENTS)
    {
        /* Check for literal run. */
        size_t litLen = 0;
        while ((pPos + litLen) < payloadSize && (rPos + litLen) < refSize &&
               payload[pPos + litLen] == ref[rPos + litLen])
        {
            ++litLen;
        }

        if (litLen > 0)
        {
            events[eventCount].payloadOffset = pPos;
            events[eventCount].payloadLength = litLen;
            events[eventCount].referenceOffset = rPos;
            events[eventCount].referenceLength = litLen;
            events[eventCount].isLiteral = true;
            ++eventCount;
            pPos += litLen;
            rPos += litLen;
            tracedPayloadBytes += litLen;
            continue;
        }

        /* Mismatch: find next sync point. */
        {
            size_t pSync = 0, rSync = 0;
            if (find_sync_point(payload, payloadSize, ref, refSize,
                                pPos, rPos, TRACE_SYNC_LEN, TRACE_SCAN_LIMIT,
                                &pSync, &rSync))
            {
                if (eventCount < TRACE_MAX_EVENTS)
                {
                    events[eventCount].payloadOffset = pPos;
                    events[eventCount].payloadLength = pSync - pPos;
                    events[eventCount].referenceOffset = rPos;
                    events[eventCount].referenceLength = rSync - rPos;
                    events[eventCount].isLiteral = false;
                    ++eventCount;
                }
                tracedPayloadBytes += (pSync - pPos);
                pPos = pSync;
                rPos = rSync;
            }
            else
            {
                /* Cannot re-sync; dump remaining as one control region. */
                if (eventCount < TRACE_MAX_EVENTS)
                {
                    events[eventCount].payloadOffset = pPos;
                    events[eventCount].payloadLength = payloadSize - pPos;
                    events[eventCount].referenceOffset = rPos;
                    events[eventCount].referenceLength = refSize - rPos;
                    events[eventCount].isLiteral = false;
                    ++eventCount;
                }
                break;
            }
        }
    }

    /* Print the trace. */
    for (i = 0; i < eventCount; ++i)
    {
        TraceEvent const *e = &events[i];
        if (e->isLiteral)
        {
            printf("  LIT  p@%-5lu r@%-5lu len=%-4lu ",
                   (unsigned long)e->payloadOffset,
                   (unsigned long)e->referenceOffset,
                   (unsigned long)e->payloadLength);
            print_hex_bytes(payload + e->payloadOffset, e->payloadLength,
                            GAP_PREVIEW_BYTES);
        }
        else
        {
            printf("  CTRL p@%-5lu len=%-4lu -> r@%-5lu len=%-4lu\n",
                   (unsigned long)e->payloadOffset,
                   (unsigned long)e->payloadLength,
                   (unsigned long)e->referenceOffset,
                   (unsigned long)e->referenceLength);
            printf("       pay: ");
            print_hex_bytes(payload + e->payloadOffset, e->payloadLength,
                            GAP_PREVIEW_BYTES);
            printf("       ref: ");
            print_hex_bytes(ref + e->referenceOffset, e->referenceLength,
                            GAP_PREVIEW_BYTES);
        }
    }

    printf("  --- traced %lu events, payload pos=%lu/%lu, ref pos=%lu/%lu\n",
           (unsigned long)eventCount,
           (unsigned long)pPos, (unsigned long)payloadSize,
           (unsigned long)rPos, (unsigned long)refSize);
}

int mthc_process_file(char const *inputPath,
                      char const *extractPath,
                      char const *decompressPath,
                      bool verbose)
{
    FileBuffer input = {0};
    MthcContainer container;
    int result = 1;

    memset(&container, 0, sizeof(container));
    if (!inputPath)
    {
        fprintf(stderr, "Error: no input path provided\n");
        goto cleanup;
    }
    if (read_entire_file(inputPath, &input) != 0)
    {
        goto cleanup;
    }
    if (parse_mthc_container(&input, &container) != 0)
    {
        goto cleanup;
    }

    print_mthc_report(&container, &input, verbose);

    if (extractPath)
    {
        if (write_entire_file(extractPath, container.payload, container.payloadSize) != 0)
        {
            goto cleanup;
        }
        printf("Extracted payload to %s\n", extractPath);
    }

    if (decompressPath)
    {
        unsigned char *decompressed = NULL;
        unsigned char *midi_output = NULL;
        uint16_t *tsizes = NULL;
        size_t tsize_count = 0;
        size_t decompressed_size = 0;
        size_t midi_size = 0;

        decompressed = decompress_mthc_file(&input, &decompressed_size, &tsizes, &tsize_count);
        if (!decompressed)
        {
            fprintf(stderr, "Error: failed to decompress MThc file\n");
            goto cleanup;
        }

        /* Assemble proper MIDI file from decompressed data */
        midi_output = assemble_midi_from_decompressed(decompressed, decompressed_size, tsizes, tsize_count, &midi_size);
        if (!midi_output)
        {
            fprintf(stderr, "Error: failed to assemble MIDI file from decompressed data\n");
            free(decompressed);
            free(tsizes);
            goto cleanup;
        }

        if (write_entire_file(decompressPath, midi_output, midi_size) != 0)
        {
            free(midi_output);
            free(decompressed);
            free(tsizes);
            goto cleanup;
        }

        printf("Decompressed MThc to MIDI %s (%lu bytes)\n", decompressPath, (unsigned long)midi_size);

        free(midi_output);
        free(decompressed);
        free(tsizes);
    }

    result = 0;

cleanup:
    free_file_buffer(&input);
    return result;
}

int mthc_decompress_memory(void const *inputData,
                           uint32_t inputSize,
                           unsigned char **outMidi,
                           uint32_t *outMidiSize)
{
    FileBuffer input = {0};
    MthcContainer container;
    unsigned char *decompressed = NULL;
    unsigned char *midi_output = NULL;
    uint16_t *tsizes = NULL;
    size_t tsize_count = 0;
    size_t decompressed_size = 0;
    size_t midi_size = 0;
    int result = 1;

    if (!inputData || inputSize < 16 || !outMidi || !outMidiSize)
    {
        return 1;
    }

    *outMidi = NULL;
    *outMidiSize = 0;

    input.data = (unsigned char *)inputData;
    input.size = (size_t)inputSize;
    memset(&container, 0, sizeof(container));

    if (parse_mthc_container(&input, &container) != 0)
    {
        goto cleanup;
    }

    decompressed = decompress_mthc_file(&input, &decompressed_size, &tsizes, &tsize_count);
    if (!decompressed)
    {
        goto cleanup;
    }

    midi_output = assemble_midi_from_decompressed(decompressed, decompressed_size, tsizes, tsize_count, &midi_size);
    if (!midi_output)
    {
        goto cleanup;
    }

    if (midi_size > UINT32_MAX)
    {
        goto cleanup;
    }

    *outMidi = midi_output;
    *outMidiSize = (uint32_t)midi_size;
    midi_output = NULL;
    result = 0;

cleanup:
    if (midi_output)
    {
        free(midi_output);
    }
    if (decompressed)
    {
        free(decompressed);
    }
    if (tsizes)
    {
        free(tsizes);
    }
    return result;
}