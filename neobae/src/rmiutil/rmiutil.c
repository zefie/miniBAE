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
 * rmiutil.c
 *
 * RMID (RIFF RMID) utility: create RMID files from MIDI + soundbank,
 * and extract MIDI + embedded bank from existing RMID files.
 *
 * Usage:
 *   rmiutil mid2rmi <input.mid> <bank.dls|bank.sf2> <output.rmi>
 *   rmiutil extract <input.rmi> [output_dir]
 *
 * mid2rmi behavior:
 *  - Parses the MIDI file to detect which instruments are actually used
 *    (bank select + program changes observed at note-on time).
 *  - Extracts only the required instruments and the referenced waves
 *    and writes a minimal DLS/SF2.
 *  - Embeds the (minimal) bank as a nested RIFF chunk inside a RIFF RMID.
 *
 * extract behavior:
 *  - Parses the RMID file to find the MIDI data chunk and any embedded
 *    soundbank RIFF chunk (DLS or SF2).
 *  - Writes the MIDI to <basename>.mid and the bank to <basename>.dls or
 *    <basename>.sf2 in the output directory.
 *
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <libgen.h>

// -----------------------------
// Small helpers
// -----------------------------

static uint16_t read_be16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t read_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t read_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t read_le16(const uint8_t *p)
{
	return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static void write_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xFF);
	p[1] = (uint8_t)((v >> 8) & 0xFF);
	p[2] = (uint8_t)((v >> 16) & 0xFF);
	p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void write_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xFF);
	p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static int fourcc_eq(const uint8_t *p, const char *fcc)
{
	return (p[0] == (uint8_t)fcc[0] && p[1] == (uint8_t)fcc[1] && p[2] == (uint8_t)fcc[2] && p[3] == (uint8_t)fcc[3]);
}

typedef struct
{
	uint8_t *data;
	size_t size;
	size_t cap;
} ByteBuf;

static int bb_reserve(ByteBuf *b, size_t add)
{
	if (!b) return 0;
	if (b->size + add <= b->cap) return 1;
	size_t need = b->size + add;
	size_t newCap = b->cap ? b->cap : 4096;
	while (newCap < need)
	{
		newCap *= 2;
	}
	uint8_t *p = (uint8_t *)realloc(b->data, newCap);
	if (!p) return 0;
	b->data = p;
	b->cap = newCap;
	return 1;
}

static int bb_append(ByteBuf *b, const void *data, size_t len)
{
	if (!len) return 1;
	if (!bb_reserve(b, len)) return 0;
	memcpy(b->data + b->size, data, len);
	b->size += len;
	return 1;
}

static int bb_append_u32le(ByteBuf *b, uint32_t v)
{
	uint8_t tmp[4];
	write_le32(tmp, v);
	return bb_append(b, tmp, sizeof(tmp));
}

static int bb_append_fourcc(ByteBuf *b, const char *fcc)
{
	return bb_append(b, fcc, 4);
}

static int bb_append_pad_even(ByteBuf *b)
{
	if (!b) return 0;
	if (b->size & 1)
	{
		const uint8_t z = 0;
		return bb_append(b, &z, 1);
	}
	return 1;
}

static void bb_free(ByteBuf *b)
{
	if (!b) return;
	free(b->data);
	b->data = NULL;
	b->size = 0;
	b->cap = 0;
}

static int read_file_all(const char *path, uint8_t **outData, size_t *outLen)
{
	if (!path || !outData || !outLen) return 0;

	FILE *f = fopen(path, "rb");
	if (!f)
	{
		fprintf(stderr, "Error: cannot open '%s': %s\n", path, strerror(errno));
		return 0;
	}
	if (fseek(f, 0, SEEK_END) != 0)
	{
		fprintf(stderr, "Error: fseek failed for '%s'\n", path);
		fclose(f);
		return 0;
	}
	long sz = ftell(f);
	if (sz < 0)
	{
		fprintf(stderr, "Error: ftell failed for '%s'\n", path);
		fclose(f);
		return 0;
	}
	if (fseek(f, 0, SEEK_SET) != 0)
	{
		fprintf(stderr, "Error: fseek failed for '%s'\n", path);
		fclose(f);
		return 0;
	}

	uint8_t *buf = (uint8_t *)malloc((size_t)sz);
	if (!buf)
	{
		fprintf(stderr, "Error: out of memory reading '%s'\n", path);
		fclose(f);
		return 0;
	}
	size_t rd = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	if (rd != (size_t)sz)
	{
		fprintf(stderr, "Error: short read for '%s'\n", path);
		free(buf);
		return 0;
	}

	*outData = buf;
	*outLen = (size_t)sz;
	return 1;
}

static int write_file_all(const char *path, const uint8_t *data, size_t len)
{
	FILE *f = fopen(path, "wb");
	if (!f)
	{
		fprintf(stderr, "Error: cannot write '%s': %s\n", path, strerror(errno));
		return 0;
	}
	size_t wr = fwrite(data, 1, len, f);
	fclose(f);
	if (wr != len)
	{
		fprintf(stderr, "Error: short write for '%s'\n", path);
		return 0;
	}
	return 1;
}

// -----------------------------
// MIDI scan: detect used instruments
// -----------------------------

typedef struct
{
	uint16_t bank14;      // 0..16383 from Bank Select MSB/LSB
	uint8_t program;      // 0..127
	uint8_t isPerc;       // 1 if percussion
} UsedInst;

typedef struct
{
	UsedInst *items;
	size_t count;
	size_t cap;
} UsedInstList;

static int usedinst_push_unique(UsedInstList *l, UsedInst inst)
{
	if (!l) return 0;
	for (size_t i = 0; i < l->count; i++)
	{
		UsedInst *e = &l->items[i];
		if (e->bank14 == inst.bank14 && e->program == inst.program && e->isPerc == inst.isPerc)
		{
			return 1;
		}
	}
	if (l->count == l->cap)
	{
		size_t newCap = l->cap ? l->cap * 2 : 32;
		UsedInst *p = (UsedInst *)realloc(l->items, newCap * sizeof(UsedInst));
		if (!p) return 0;
		l->items = p;
		l->cap = newCap;
	}
	l->items[l->count++] = inst;
	return 1;
}

static void usedinst_free(UsedInstList *l)
{
	if (!l) return;
	free(l->items);
	l->items = NULL;
	l->count = 0;
	l->cap = 0;
}

static int midi_read_vlq(const uint8_t *buf, size_t len, size_t *ioPos, uint32_t *outVal)
{
	if (!buf || !ioPos || !outVal) return 0;
	size_t pos = *ioPos;
	uint32_t v = 0;
	for (int i = 0; i < 4; i++)
	{
		if (pos >= len) return 0;
		uint8_t b = buf[pos++];
		v = (v << 7) | (uint32_t)(b & 0x7F);
		if ((b & 0x80) == 0)
		{
			*ioPos = pos;
			*outVal = v;
			return 1;
		}
	}
	return 0;
}

static int scan_midi_used_instruments(const uint8_t *midi, size_t midiLen, UsedInstList *outUsed)
{
	if (!midi || midiLen < 14 || !outUsed) return 0;
	if (memcmp(midi, "MThd", 4) != 0) return 0;
	uint32_t hdrLen = read_be32(midi + 4);
	if (hdrLen < 6 || 8 + hdrLen > midiLen) return 0;

	uint16_t nTracks = read_be16(midi + 10);

	// Channel state
	uint8_t bankMSB[16];
	uint8_t bankLSB[16];
	uint8_t program[16];
	memset(bankMSB, 0, sizeof(bankMSB));
	memset(bankLSB, 0, sizeof(bankLSB));
	memset(program, 0, sizeof(program));

	size_t pos = 8 + hdrLen;
	for (uint16_t t = 0; t < nTracks; t++)
	{
		if (pos + 8 > midiLen) return 0;
		if (memcmp(midi + pos, "MTrk", 4) != 0) return 0;
		uint32_t trkLen = read_be32(midi + pos + 4);
		pos += 8;
		if (pos + trkLen > midiLen) return 0;
		size_t end = pos + trkLen;

		uint8_t running = 0;
		while (pos < end)
		{
			uint32_t delta = 0;
			(void)delta;
			if (!midi_read_vlq(midi, end, &pos, &delta)) return 0;
			if (pos >= end) return 0;

			uint8_t status = midi[pos];
			if (status & 0x80)
			{
				pos++;
				running = status;
			}
			else
			{
				// running status
				if (!running) return 0;
				status = running;
			}

			if (status == 0xFF)
			{
				if (pos + 1 > end) return 0;
				uint8_t metaType = midi[pos++];
				uint32_t mlen = 0;
				if (!midi_read_vlq(midi, end, &pos, &mlen)) return 0;
				if (pos + mlen > end) return 0;
				pos += mlen;
				(void)metaType;
				continue;
			}
			if (status == 0xF0 || status == 0xF7)
			{
				uint32_t slen = 0;
				if (!midi_read_vlq(midi, end, &pos, &slen)) return 0;
				if (pos + slen > end) return 0;
				pos += slen;
				continue;
			}

			uint8_t type = status & 0xF0;
			uint8_t ch = status & 0x0F;

			// Determine parameter count
			uint8_t p1 = 0, p2 = 0;
			if (type == 0xC0 || type == 0xD0)
			{
				if (pos + 1 > end) return 0;
				p1 = midi[pos++];
			}
			else
			{
				if (pos + 2 > end) return 0;
				p1 = midi[pos++];
				p2 = midi[pos++];
			}

			if (type == 0xB0)
			{
				// CC
				if (p1 == 0) bankMSB[ch] = p2;
				else if (p1 == 32) bankLSB[ch] = p2;
			}
			else if (type == 0xC0)
			{
				program[ch] = p1 & 0x7F;
			}
			else if (type == 0x90)
			{
				// Note on (vel>0)
				if ((p2 & 0x7F) != 0)
				{
					UsedInst inst;
					inst.bank14 = (uint16_t)(((uint16_t)(bankMSB[ch] & 0x7F) << 7) | (uint16_t)(bankLSB[ch] & 0x7F));
					inst.program = (uint8_t)(program[ch] & 0x7F);
					inst.isPerc = (ch == 9) ? 1 : 0;
					if (!usedinst_push_unique(outUsed, inst)) return 0;
				}
			}
		}
	}
	return 1;
}

// -----------------------------
// RIFF child scanner
// -----------------------------

static int parse_riff_children(const uint8_t *buf, size_t len, size_t start, size_t end,
							   const uint8_t **outChunk, uint32_t *outChunkSize,
							   const char *wantId, const char *wantListType)
{
	// Generic scanner: find first matching chunk in [start,end)
	size_t i = start;
	while (i + 8 <= end && i + 8 <= len)
	{
		const uint8_t *chunk = buf + i;
		uint32_t sz = read_le32(chunk + 4);
		size_t chunkTotal = 8 + (size_t)sz;
		if (i + chunkTotal > end || i + chunkTotal > len) break;

		if (wantId && fourcc_eq(chunk, wantId))
		{
			if (wantListType)
			{
				if ((fourcc_eq(chunk, "RIFF") || fourcc_eq(chunk, "LIST")) && sz >= 4)
				{
					if (fourcc_eq(chunk + 8, wantListType))
					{
						if (outChunk) *outChunk = chunk;
						if (outChunkSize) *outChunkSize = (uint32_t)chunkTotal;
						return 1;
					}
				}
			}
			else
			{
				if (outChunk) *outChunk = chunk;
				if (outChunkSize) *outChunkSize = (uint32_t)chunkTotal;
				return 1;
			}
		}

		i += chunkTotal;
		if (i & 1) i++; // word align
	}
	return 0;
}

// -----------------------------
// DLS trimming
// -----------------------------

typedef struct
{
	uint32_t offset;  // offset inside wvpl payload (after listType)
	uint32_t size;    // total bytes including chunk header
} WaveEntry;

typedef struct
{
	WaveEntry *items;
	size_t count;
	size_t cap;
} WaveEntryList;

static void wave_list_free(WaveEntryList *l)
{
	if (!l) return;
	free(l->items);
	l->items = NULL;
	l->count = 0;
	l->cap = 0;
}

static int wave_list_push(WaveEntryList *l, WaveEntry e)
{
	if (!l) return 0;
	if (l->count == l->cap)
	{
		size_t nc = l->cap ? l->cap * 2 : 64;
		WaveEntry *p = (WaveEntry *)realloc(l->items, nc * sizeof(WaveEntry));
		if (!p) return 0;
		l->items = p;
		l->cap = nc;
	}
	l->items[l->count++] = e;
	return 1;
}

static int dls_parse_ptbl(const uint8_t *ptblChunk, uint32_t ptblTotal, uint32_t **outOffsets, uint32_t *outCount)
{
	if (!ptblChunk || ptblTotal < 8 + 8) return 0;
	uint32_t sz = read_le32(ptblChunk + 4);
	if (sz + 8 != ptblTotal) return 0;
	const uint8_t *p = ptblChunk + 8;
	if (sz < 8) return 0;
	uint32_t cbSize = read_le32(p);
	uint32_t cCues = read_le32(p + 4);
	// DLS ptbl stores an array of POOLCUE records.
	// In theory: entry size is cbSize; in practice, some banks lie about cbSize.
	// Example: Windows_gm.dls reports cbSize=8 but actually stores 4-byte offsets.
	// We only need the offset; it is always the first 4 bytes of each entry.
	if (cCues > 0x100000) return 0; // sanity limit
	if (cCues == 0) {
		// Valid but useless.
		uint32_t *offs = (uint32_t *)malloc(0);
		*outOffsets = offs;
		*outCount = 0;
		return 1;
	}
	if (sz < 8) return 0;
	uint32_t dataBytes = sz - 8;
	uint32_t entrySize = cbSize;
	if (entrySize < 4)
		entrySize = 0;
	// If the payload divides evenly, trust the derived size.
	if (dataBytes / cCues >= 4 && (dataBytes % cCues) == 0)
	{
		uint32_t derived = dataBytes / cCues;
		if (derived >= 4)
			entrySize = derived;
	}
	if (entrySize < 4) return 0;
	if ((uint64_t)entrySize * (uint64_t)cCues > (uint64_t)dataBytes) return 0;
	uint32_t *offs = (uint32_t *)malloc((size_t)cCues * sizeof(uint32_t));
	if (!offs) return 0;
	const uint8_t *arr = p + 8;
	for (uint32_t i = 0; i < cCues; i++)
	{
		offs[i] = read_le32(arr + (size_t)i * (size_t)entrySize);
	}
	*outOffsets = offs;
	*outCount = cCues;
	return 1;
}

static int dls_parse_wvpl_entries(const uint8_t *wvplChunk, uint32_t wvplTotal, WaveEntryList *outEntries)
{
	if (!wvplChunk || wvplTotal < 12) return 0;
	if (!fourcc_eq(wvplChunk, "LIST")) return 0;
	uint32_t sz = read_le32(wvplChunk + 4);
	if (sz + 8 != wvplTotal) return 0;
	if (!fourcc_eq(wvplChunk + 8, "wvpl")) return 0;

	size_t payloadStart = 12;
	size_t payloadEnd = 8 + (size_t)sz;
	size_t i = payloadStart;
	while (i + 8 <= payloadEnd)
	{
		const uint8_t *c = wvplChunk + i;
		uint32_t csz = read_le32(c + 4);
		size_t total = 8 + (size_t)csz;
		if (i + total > payloadEnd) break;

		// Record all subchunks (most are LIST 'wave')
		WaveEntry e;
		e.offset = (uint32_t)(i - payloadStart);
		e.size = (uint32_t)total;
		if (!wave_list_push(outEntries, e)) return 0;

		i += total;
		if (i & 1) i++;
	}
	return 1;
}

static int dls_find_instrument_insh(const uint8_t *insListChunk, uint32_t insTotal,
								   uint32_t *outBank, uint32_t *outProg)
{
	// insListChunk is LIST 'ins '
	if (!insListChunk || insTotal < 12) return 0;
	if (!fourcc_eq(insListChunk, "LIST")) return 0;
	uint32_t sz = read_le32(insListChunk + 4);
	if (sz + 8 != insTotal) return 0;
	if (!fourcc_eq(insListChunk + 8, "ins ")) return 0;

	size_t payloadStart = 12;
	size_t payloadEnd = 8 + (size_t)sz;
	size_t i = payloadStart;
	while (i + 8 <= payloadEnd)
	{
		const uint8_t *c = insListChunk + i;
		uint32_t csz = read_le32(c + 4);
		size_t total = 8 + (size_t)csz;
		if (i + total > payloadEnd) break;

		if (fourcc_eq(c, "insh") && csz >= 12)
		{
			const uint8_t *d = c + 8;
			// uint32_t cRegions = read_le32(d);
			uint32_t bank = read_le32(d + 4);
			uint32_t prog = read_le32(d + 8);
			if (outBank) *outBank = bank;
			if (outProg) *outProg = prog;
			return 1;
		}

		i += total;
		if (i & 1) i++;
	}
	return 0;
}

static int patch_wlnk_table_indices(uint8_t *buf, size_t len, const uint32_t *mapOldToNew, uint32_t mapCount)
{
	// Recursively scan for 'wlnk' chunks and patch the last 4 bytes to new table index.
	// This assumes WAVELINK ends with ulTableIndex, which is true for typical DLS.
	if (!buf || len < 8) return 0;

	size_t i = 0;
	while (i + 8 <= len)
	{
		uint8_t *c = buf + i;
		uint32_t csz = read_le32(c + 4);
		size_t total = 8 + (size_t)csz;
		if (i + total > len) break;

		if (fourcc_eq(c, "wlnk") && csz >= 4)
		{
			uint32_t oldIdx = read_le32(c + 8 + csz - 4);
			uint32_t newIdx = oldIdx;
			if (oldIdx < mapCount)
				newIdx = mapOldToNew[oldIdx];
			write_le32(c + 8 + csz - 4, newIdx);
		}
		else if ((fourcc_eq(c, "LIST") || fourcc_eq(c, "RIFF")) && csz >= 4)
		{
			// Recurse into container payload
			size_t childStart = i + 12;
			size_t childLen = (size_t)csz - 4;
			if (childStart + childLen <= len)
			{
				(void)patch_wlnk_table_indices(buf + childStart, childLen, mapOldToNew, mapCount);
			}
		}

		i += total;
		if (i & 1) i++;
	}
	return 1;
}

static int u32_set_push_unique(uint32_t **arr, size_t *count, size_t *cap, uint32_t v)
{
	for (size_t i = 0; i < *count; i++)
		if ((*arr)[i] == v) return 1;
	if (*count == *cap)
	{
		size_t nc = *cap ? (*cap) * 2 : 64;
		uint32_t *p = (uint32_t *)realloc(*arr, nc * sizeof(uint32_t));
		if (!p) return 0;
		*arr = p;
		*cap = nc;
	}
	(*arr)[(*count)++] = v;
	return 1;
}

static int dls_collect_needed_table_indices_from_ins(const uint8_t *insChunk, uint32_t insTotal, uint32_t **ioIdx, size_t *ioCount, size_t *ioCap)
{
	// Scan for wlnk chunks inside this instrument and collect their table indices.
	if (!insChunk || insTotal < 12) return 0;
	if (!fourcc_eq(insChunk, "LIST")) return 0;
	uint32_t sz = read_le32(insChunk + 4);
	if (sz + 8 != insTotal) return 0;
	// payload starts at 12
	size_t payloadStart = 12;
	size_t payloadEnd = 8 + (size_t)sz;
	size_t i = payloadStart;
	while (i + 8 <= payloadEnd)
	{
		const uint8_t *c = insChunk + i;
		uint32_t csz = read_le32(c + 4);
		size_t total = 8 + (size_t)csz;
		if (i + total > payloadEnd) break;
		if (fourcc_eq(c, "wlnk") && csz >= 4)
		{
			uint32_t idx = read_le32(c + 8 + csz - 4);
			if (!u32_set_push_unique(ioIdx, ioCount, ioCap, idx)) return 0;
		}
		else if ((fourcc_eq(c, "LIST") || fourcc_eq(c, "RIFF")) && csz >= 4)
		{
			// Recurse into container
			size_t childStart = i + 12;
			size_t childLen = (size_t)csz - 4;
			if (childStart + childLen <= payloadEnd)
			{
				// use a temporary view
				if (!dls_collect_needed_table_indices_from_ins(insChunk + i, (uint32_t)total, ioIdx, ioCount, ioCap))
					return 0;
			}
		}
		i += total;
		if (i & 1) i++;
	}
	return 1;
}

static int dls_trim_minimal(const uint8_t *dls, size_t dlsLen, const UsedInstList *used, uint8_t **outDls, size_t *outDlsLen)
{
	if (!dls || dlsLen < 12 || !used || !outDls || !outDlsLen) return 0;
	if (!fourcc_eq(dls, "RIFF") || !fourcc_eq(dls + 8, "DLS ")) return 0;

	uint32_t riffSize = read_le32(dls + 4);
	size_t riffEnd = 8 + (size_t)riffSize;
	if (riffEnd > dlsLen) riffEnd = dlsLen;

	const uint8_t *ptbl = NULL, *wvpl = NULL, *lins = NULL;
	uint32_t ptblTotal = 0, wvplTotal = 0, linsTotal = 0;

	if (!parse_riff_children(dls, dlsLen, 12, riffEnd, &ptbl, &ptblTotal, "ptbl", NULL))
	{
		fprintf(stderr, "Error: DLS missing ptbl\n");
		return 0;
	}
	if (!parse_riff_children(dls, dlsLen, 12, riffEnd, &wvpl, &wvplTotal, "LIST", "wvpl"))
	{
		fprintf(stderr, "Error: DLS missing LIST wvpl\n");
		return 0;
	}
	if (!parse_riff_children(dls, dlsLen, 12, riffEnd, &lins, &linsTotal, "LIST", "lins"))
	{
		fprintf(stderr, "Error: DLS missing LIST lins\n");
		return 0;
	}

	uint32_t *ptblOffsets = NULL;
	uint32_t ptblCount = 0;
	if (!dls_parse_ptbl(ptbl, ptblTotal, &ptblOffsets, &ptblCount))
	{
		fprintf(stderr, "Error: unsupported ptbl\n");
		return 0;
	}

	WaveEntryList waves = {0};
	if (!dls_parse_wvpl_entries(wvpl, wvplTotal, &waves))
	{
		fprintf(stderr, "Error: failed to parse wvpl\n");
		free(ptblOffsets);
		return 0;
	}

	// Build a mapping from ptbl index -> wave entry bytes by matching ptbl offset to a wave entry start offset.
	// Since some wvpl may contain non-wave chunks, we match offset numerically.
	// If not found, trimming will fail.
	//
	// Parse instruments in lins
	const uint8_t *linsChunk = lins;
	uint32_t linsSz = read_le32(linsChunk + 4);
	size_t linsPayloadStart = 12;
	size_t linsPayloadEnd = 8 + (size_t)linsSz;

	// Determine which instrument chunks are needed, and collect wave table indices referenced.
	const uint8_t **needInsPtr = NULL;
	uint32_t *needInsSize = NULL;
	size_t needInsCount = 0, needInsCap = 0;

	uint32_t *neededTableIdx = NULL;
	size_t neededTableCount = 0, neededTableCap = 0;

	size_t i = linsPayloadStart;
	while (i + 8 <= linsPayloadEnd)
	{
		const uint8_t *c = linsChunk + i;
		uint32_t csz = read_le32(c + 4);
		size_t total = 8 + (size_t)csz;
		if (i + total > linsPayloadEnd) break;

		if (fourcc_eq(c, "LIST") && csz >= 4 && fourcc_eq(c + 8, "ins "))
		{
			uint32_t bank = 0, prog = 0;
			if (dls_find_instrument_insh(c, (uint32_t)total, &bank, &prog))
			{
				uint8_t isPerc = (bank & 0x80000000U) ? 1 : 0;
				uint16_t bank14 = (uint16_t)(bank & 0x3FFFU);
				uint8_t programNum = (uint8_t)(prog & 0x7FU);

				int matched = 0;
				for (size_t u = 0; u < used->count; u++)
				{
					const UsedInst *ui = &used->items[u];
					if (ui->isPerc == isPerc && ui->bank14 == bank14 && ui->program == programNum)
					{
						matched = 1;
						break;
					}
				}
				if (matched)
				{
				if (needInsCount == needInsCap)
				{
					size_t nc = needInsCap ? needInsCap * 2 : 32;
					const uint8_t **tmpPtr = (const uint8_t **)realloc(needInsPtr, nc * sizeof(uint8_t *));
					if (!tmpPtr)
					{
						free(needInsPtr); free(needInsSize);
						free(ptblOffsets); wave_list_free(&waves);
						free(neededTableIdx);
						return 0;
					}
					uint32_t *tmpSize = (uint32_t *)realloc(needInsSize, nc * sizeof(uint32_t));
					if (!tmpSize)
					{
						free(needInsSize);
						free(ptblOffsets); wave_list_free(&waves);
						free(tmpPtr);
						free(neededTableIdx);
						return 0;
					}
					needInsPtr = tmpPtr;
					needInsSize = tmpSize;
					needInsCap = nc;
				}
					needInsPtr[needInsCount] = c;
					needInsSize[needInsCount] = (uint32_t)total;
					needInsCount++;

					if (!dls_collect_needed_table_indices_from_ins(c, (uint32_t)total, &neededTableIdx, &neededTableCount, &neededTableCap))
					{
						free(ptblOffsets);
						wave_list_free(&waves);
						free(needInsPtr); free(needInsSize);
						free(neededTableIdx);
						return 0;
					}
				}
			}
		}

		i += total;
		if (i & 1) i++;
	}

	if (needInsCount == 0)
	{
		fprintf(stderr, "Warning: no matching DLS instruments found; embedding original DLS\n");
		free(ptblOffsets);
		wave_list_free(&waves);
		free(needInsPtr); free(needInsSize);
		free(neededTableIdx);
		uint8_t *cpy = (uint8_t *)malloc(dlsLen);
		if (!cpy) return 0;
		memcpy(cpy, dls, dlsLen);
		*outDls = cpy;
		*outDlsLen = dlsLen;
		return 1;
	}

	// Build oldIdx -> newIdx map (dense 0..ptblCount-1). Default maps to 0.
	uint32_t *mapOldToNew = (uint32_t *)malloc((size_t)ptblCount * sizeof(uint32_t));
	if (!mapOldToNew)
	{
		free(ptblOffsets);
		wave_list_free(&waves);
		free(needInsPtr); free(needInsSize);
		free(neededTableIdx);
		return 0;
	}
	for (uint32_t k = 0; k < ptblCount; k++) mapOldToNew[k] = 0;

	// Keep table indices in a stable order (as collected).
	size_t newWaveCount = 0;
	for (size_t k = 0; k < neededTableCount; k++)
	{
		uint32_t old = neededTableIdx[k];
		if (old < ptblCount)
		{
			// assign sequential new index if first time
			if (mapOldToNew[old] == 0 && !(old == neededTableIdx[0] && k == 0 && mapOldToNew[old] == 0))
			{
				// can't distinguish unset from mapping-to-0; do a scan
			}
		}
	}
	// Do a robust assignment by tracking which old indices are included.
	uint8_t *oldUsed = (uint8_t *)calloc((size_t)ptblCount, 1);
	if (!oldUsed)
	{
		free(mapOldToNew);
		free(ptblOffsets);
		wave_list_free(&waves);
		free(needInsPtr); free(needInsSize);
		free(neededTableIdx);
		return 0;
	}
	for (size_t k = 0; k < neededTableCount; k++)
	{
		uint32_t old = neededTableIdx[k];
		if (old < ptblCount)
			oldUsed[old] = 1;
	}
	for (uint32_t old = 0; old < ptblCount; old++)
	{
		if (oldUsed[old])
		{
			mapOldToNew[old] = (uint32_t)newWaveCount;
			newWaveCount++;
		}
	}

	// Build list of old table indices in new order
	uint32_t *newToOld = (uint32_t *)malloc(newWaveCount * sizeof(uint32_t));
	if (!newToOld)
	{
		free(oldUsed);
		free(mapOldToNew);
		free(ptblOffsets);
		wave_list_free(&waves);
		free(needInsPtr); free(needInsSize);
		free(neededTableIdx);
		return 0;
	}
	for (uint32_t old = 0, j = 0; old < ptblCount; old++)
	{
		if (oldUsed[old])
			newToOld[j++] = old;
	}

	// Helper to locate a wave entry by ptbl offset.
	const uint8_t *wvplPayload = wvpl + 12;
	size_t wvplPayloadLen = (size_t)wvplTotal - 12;
	(void)wvplPayloadLen;
	(void)waves;

	// Build minimal DLS
	ByteBuf out = {0};
	// RIFF header
	bb_append_fourcc(&out, "RIFF");
	size_t riffSizePos = out.size;
	bb_append_u32le(&out, 0);
	bb_append_fourcc(&out, "DLS ");

	// Copy through top-level chunks we want to preserve (excluding colh/ptbl/wvpl/lins).
	size_t top = 12;
	while (top + 8 <= riffEnd)
	{
		const uint8_t *c = dls + top;
		uint32_t csz = read_le32(c + 4);
		size_t total = 8 + (size_t)csz;
		if (top + total > riffEnd) break;
		if (fourcc_eq(c, "colh") || fourcc_eq(c, "ptbl") || (fourcc_eq(c, "LIST") && csz >= 4 && (fourcc_eq(c + 8, "wvpl") || fourcc_eq(c + 8, "lins"))))
		{
			// skip
		}
		else
		{
			bb_append(&out, c, total);
			bb_append_pad_even(&out);
		}
		top += total;
		if (top & 1) top++;
	}

	// colh
	bb_append_fourcc(&out, "colh");
	bb_append_u32le(&out, 4);
	bb_append_u32le(&out, (uint32_t)needInsCount);
	bb_append_pad_even(&out);

	// ptbl
	bb_append_fourcc(&out, "ptbl");
	// FluidSynth requires cbSize >= 8. But it expects entries to be 4 bytes (offsets only).
	uint32_t ptblDataSize = 8 + (uint32_t)(newWaveCount * 4);
	bb_append_u32le(&out, ptblDataSize);
	bb_append_u32le(&out, 8); // cbSize
	bb_append_u32le(&out, (uint32_t)newWaveCount);
	size_t ptblOffsetsPos = out.size;
	for (size_t k = 0; k < newWaveCount; k++)
	{
		bb_append_u32le(&out, 0); // offset
	}
	bb_append_pad_even(&out);

	// wvpl
	bb_append_fourcc(&out, "LIST");
	size_t wvplSizePos = out.size;
	bb_append_u32le(&out, 0);
	bb_append_fourcc(&out, "wvpl");
	size_t wvplPayloadStartOut = out.size;

	// write waves and compute offsets/lengths
	uint32_t *newOffsets = (uint32_t *)malloc(newWaveCount * sizeof(uint32_t));
	uint32_t *newLengths = (uint32_t *)malloc(newWaveCount * sizeof(uint32_t));
	if (!newOffsets || !newLengths)
	{
		free(newToOld);
		free(oldUsed);
		free(mapOldToNew);
		free(ptblOffsets);
		wave_list_free(&waves);
		free(needInsPtr); free(needInsSize);
		free(neededTableIdx);
		free(newOffsets);
		free(newLengths);
		bb_free(&out);
		return 0;
	}
	for (size_t k = 0; k < newWaveCount; k++)
	{
		uint32_t oldIdx = newToOld[k];
		uint32_t off = ptblOffsets[oldIdx];
		// locate wave entry beginning at off
		const uint8_t *waveChunk = NULL;
		uint32_t waveTotal = 0;
		for (size_t wi = 0; wi < waves.count; wi++)
		{
			if (waves.items[wi].offset == off)
			{
				waveChunk = wvplPayload + waves.items[wi].offset;
				waveTotal = waves.items[wi].size;
				break;
			}
		}
		if (!waveChunk || waveTotal < 8)
		{
			fprintf(stderr, "Error: cannot locate wave for ptbl index %u (offset %u)\n", oldIdx, off);
			free(newOffsets);
			free(newToOld);
			free(oldUsed);
			free(mapOldToNew);
			free(ptblOffsets);
			wave_list_free(&waves);
			free(needInsPtr); free(needInsSize);
			free(neededTableIdx);
			bb_free(&out);
			return 0;
		}

		// offset from start of wvpl payload (after listType)
		newOffsets[k] = (uint32_t)(out.size - wvplPayloadStartOut);
		newLengths[k] = waveTotal;
		bb_append(&out, waveChunk, waveTotal);
		bb_append_pad_even(&out);
	}
	// patch wvpl size
	uint32_t wvplDataSize = (uint32_t)(out.size - (wvplSizePos + 4));
	write_le32(out.data + wvplSizePos, wvplDataSize);
	bb_append_pad_even(&out);

	// patch ptbl offsets
	for (size_t k = 0; k < newWaveCount; k++)
	{
		write_le32(out.data + ptblOffsetsPos + k * 4, newOffsets[k]);
	}

	free(newOffsets);
	free(newLengths);

	// lins
	bb_append_fourcc(&out, "LIST");
	size_t linsSizePosOut = out.size;
	bb_append_u32le(&out, 0);
	bb_append_fourcc(&out, "lins");
	size_t linsPayloadStartOut = out.size;
	(void)linsPayloadStartOut;

	for (size_t n = 0; n < needInsCount; n++)
	{
		uint32_t insTotal = needInsSize[n];
		uint8_t *tmp = (uint8_t *)malloc(insTotal);
		if (!tmp)
		{
			free(newToOld);
			free(oldUsed);
			free(mapOldToNew);
			free(ptblOffsets);
			wave_list_free(&waves);
			free(needInsPtr); free(needInsSize);
			free(neededTableIdx);
			bb_free(&out);
			return 0;
		}
		memcpy(tmp, needInsPtr[n], insTotal);
		patch_wlnk_table_indices(tmp, insTotal, mapOldToNew, ptblCount);
		bb_append(&out, tmp, insTotal);
		bb_append_pad_even(&out);
		free(tmp);
	}
	uint32_t linsDataSize = (uint32_t)(out.size - (linsSizePosOut + 4));
	write_le32(out.data + linsSizePosOut, linsDataSize);
	bb_append_pad_even(&out);

	// Patch RIFF size
	uint32_t outRiffSize = (uint32_t)(out.size - 8);
	write_le32(out.data + riffSizePos, outRiffSize);

	free(newToOld);
	free(oldUsed);
	free(mapOldToNew);
	free(ptblOffsets);
	wave_list_free(&waves);
	free(needInsPtr); free(needInsSize);
	free(neededTableIdx);

	*outDls = out.data;
	*outDlsLen = out.size;
	return 1;
}

// -----------------------------
// SF2 trimming
// -----------------------------

// SoundFont 2.0 record sizes
#define SF2_PHDR_SIZE 38
#define SF2_PBAG_SIZE 4
#define SF2_PMOD_SIZE 10
#define SF2_PGEN_SIZE 4
#define SF2_INST_SIZE 22
#define SF2_IBAG_SIZE 4
#define SF2_IMOD_SIZE 10
#define SF2_IGEN_SIZE 4
#define SF2_SHDR_SIZE 46

// Generator operators
#define SF2_GEN_INSTRUMENT 41
#define SF2_GEN_SAMPLEID 53

typedef struct
{
	const uint8_t *riff;
	size_t riffLen;

	const uint8_t *listINFO;
	uint32_t listINFOTotal;

	const uint8_t *listSDTA;
	uint32_t listSDTATotal;
	const uint8_t *smpl;
	uint32_t smplTotal;
	const uint8_t *sm24;
	uint32_t sm24Total;

	const uint8_t *listPDTA;
	uint32_t listPDTATotal;
	// pdta subchunks (payload pointers)
	const uint8_t *phdr; uint32_t phdrSize;
	const uint8_t *pbag; uint32_t pbagSize;
	const uint8_t *pmod; uint32_t pmodSize;
	const uint8_t *pgen; uint32_t pgenSize;
	const uint8_t *inst; uint32_t instSize;
	const uint8_t *ibag; uint32_t ibagSize;
	const uint8_t *imod; uint32_t imodSize;
	const uint8_t *igen; uint32_t igenSize;
	const uint8_t *shdr; uint32_t shdrSize;
} SF2View;

static int list_find_child_chunk(const uint8_t *listChunk, uint32_t listTotal, const char *wantId, const uint8_t **outChunk, uint32_t *outTotal)
{
	if (!listChunk || listTotal < 12 || !wantId) return 0;
	if (!fourcc_eq(listChunk, "LIST")) return 0;
	uint32_t sz = read_le32(listChunk + 4);
	if (sz + 8 != listTotal) return 0;
	// payload starts after listType
	size_t payloadStart = 12;
	size_t payloadEnd = 8 + (size_t)sz;
	size_t i = payloadStart;
	while (i + 8 <= payloadEnd)
	{
		const uint8_t *c = listChunk + i;
		uint32_t csz = read_le32(c + 4);
		size_t total = 8 + (size_t)csz;
		if (i + total > payloadEnd) break;
		if (fourcc_eq(c, wantId))
		{
			if (outChunk) *outChunk = c;
			if (outTotal) *outTotal = (uint32_t)total;
			return 1;
		}
		i += total;
		if (i & 1) i++;
	}
	return 0;
}

static int list_find_child_list(const uint8_t *listChunk, uint32_t listTotal, const char *wantListType, const uint8_t **outList, uint32_t *outTotal)
{
	if (!listChunk || listTotal < 12 || !wantListType) return 0;
	if (!fourcc_eq(listChunk, "LIST")) return 0;
	uint32_t sz = read_le32(listChunk + 4);
	if (sz + 8 != listTotal) return 0;
	size_t payloadStart = 12;
	size_t payloadEnd = 8 + (size_t)sz;
	size_t i = payloadStart;
	while (i + 12 <= payloadEnd)
	{
		const uint8_t *c = listChunk + i;
		uint32_t csz = read_le32(c + 4);
		size_t total = 8 + (size_t)csz;
		if (i + total > payloadEnd) break;
		if (fourcc_eq(c, "LIST") && csz >= 4 && fourcc_eq(c + 8, wantListType))
		{
			if (outList) *outList = c;
			if (outTotal) *outTotal = (uint32_t)total;
			return 1;
		}
		i += total;
		if (i & 1) i++;
	}
	return 0;
}

static int sf2_parse_view(const uint8_t *sf2, size_t sf2Len, SF2View *out)
{
	if (!sf2 || sf2Len < 12 || !out) return 0;
	memset(out, 0, sizeof(*out));
	if (!fourcc_eq(sf2, "RIFF") || !fourcc_eq(sf2 + 8, "sfbk")) return 0;
	uint32_t riffSize = read_le32(sf2 + 4);
	size_t riffEnd = 8 + (size_t)riffSize;
	if (riffEnd > sf2Len) riffEnd = sf2Len;

	out->riff = sf2;
	out->riffLen = sf2Len;

	// Find top-level LIST chunks (INFO/sdta/pdta)
	const uint8_t *listINFO = NULL, *listSDTA = NULL, *listPDTA = NULL;
	uint32_t infoTotal = 0, sdtaTotal = 0, pdtaTotal = 0;
	if (!parse_riff_children(sf2, sf2Len, 12, riffEnd, &listSDTA, &sdtaTotal, "LIST", "sdta"))
		return 0;
	if (!parse_riff_children(sf2, sf2Len, 12, riffEnd, &listPDTA, &pdtaTotal, "LIST", "pdta"))
		return 0;
	(void)parse_riff_children(sf2, sf2Len, 12, riffEnd, &listINFO, &infoTotal, "LIST", "INFO");

	out->listINFO = listINFO;
	out->listINFOTotal = infoTotal;
	out->listSDTA = listSDTA;
	out->listSDTATotal = sdtaTotal;
	out->listPDTA = listPDTA;
	out->listPDTATotal = pdtaTotal;

	// sdta children: smpl (required), sm24 (optional)
	const uint8_t *smplChunk = NULL, *sm24Chunk = NULL;
	uint32_t smplTotal = 0, sm24Total = 0;
	if (!list_find_child_chunk(listSDTA, sdtaTotal, "smpl", &smplChunk, &smplTotal))
		return 0;
	(void)list_find_child_chunk(listSDTA, sdtaTotal, "sm24", &sm24Chunk, &sm24Total);
	out->smpl = smplChunk;
	out->smplTotal = smplTotal;
	out->sm24 = sm24Chunk;
	out->sm24Total = sm24Total;

	// pdta children
	const uint8_t *c = NULL; uint32_t ct = 0;
	if (!list_find_child_chunk(listPDTA, pdtaTotal, "phdr", &c, &ct)) return 0;
	out->phdr = c + 8; out->phdrSize = read_le32(c + 4);
	if (!list_find_child_chunk(listPDTA, pdtaTotal, "pbag", &c, &ct)) return 0;
	out->pbag = c + 8; out->pbagSize = read_le32(c + 4);
	if (!list_find_child_chunk(listPDTA, pdtaTotal, "pmod", &c, &ct)) return 0;
	out->pmod = c + 8; out->pmodSize = read_le32(c + 4);
	if (!list_find_child_chunk(listPDTA, pdtaTotal, "pgen", &c, &ct)) return 0;
	out->pgen = c + 8; out->pgenSize = read_le32(c + 4);
	if (!list_find_child_chunk(listPDTA, pdtaTotal, "inst", &c, &ct)) return 0;
	out->inst = c + 8; out->instSize = read_le32(c + 4);
	if (!list_find_child_chunk(listPDTA, pdtaTotal, "ibag", &c, &ct)) return 0;
	out->ibag = c + 8; out->ibagSize = read_le32(c + 4);
	if (!list_find_child_chunk(listPDTA, pdtaTotal, "imod", &c, &ct)) return 0;
	out->imod = c + 8; out->imodSize = read_le32(c + 4);
	if (!list_find_child_chunk(listPDTA, pdtaTotal, "igen", &c, &ct)) return 0;
	out->igen = c + 8; out->igenSize = read_le32(c + 4);
	if (!list_find_child_chunk(listPDTA, pdtaTotal, "shdr", &c, &ct)) return 0;
	out->shdr = c + 8; out->shdrSize = read_le32(c + 4);

	// Basic sanity: record multiples and required terminal records
	if (out->phdrSize < SF2_PHDR_SIZE * 2 || (out->phdrSize % SF2_PHDR_SIZE) != 0) return 0;
	if (out->pbagSize < SF2_PBAG_SIZE * 2 || (out->pbagSize % SF2_PBAG_SIZE) != 0) return 0;
	if ((out->pmodSize % SF2_PMOD_SIZE) != 0) return 0;
	if ((out->pgenSize % SF2_PGEN_SIZE) != 0) return 0;
	if (out->instSize < SF2_INST_SIZE * 2 || (out->instSize % SF2_INST_SIZE) != 0) return 0;
	if (out->ibagSize < SF2_IBAG_SIZE * 2 || (out->ibagSize % SF2_IBAG_SIZE) != 0) return 0;
	if ((out->imodSize % SF2_IMOD_SIZE) != 0) return 0;
	if ((out->igenSize % SF2_IGEN_SIZE) != 0) return 0;
	if (out->shdrSize < SF2_SHDR_SIZE * 2 || (out->shdrSize % SF2_SHDR_SIZE) != 0) return 0;

	return 1;
}

static int sf2_any_used_percussion(const UsedInstList *used)
{
	if (!used) return 0;
	for (size_t i = 0; i < used->count; i++)
		if (used->items[i].isPerc) return 1;
	return 0;
}

static int sf2_select_presets(const SF2View *v, const UsedInstList *used, uint8_t **outKeepPreset, uint32_t *outPresetCount)
{
	if (!v || !used || !outKeepPreset || !outPresetCount) return 0;
	uint32_t phdrRecs = v->phdrSize / SF2_PHDR_SIZE;
	if (phdrRecs < 2) return 0;
	uint32_t presetCount = phdrRecs - 1; // excluding EOP
	uint8_t *keep = (uint8_t *)calloc((size_t)presetCount, 1);
	if (!keep) return 0;

	// First pass: exact match on full bank14
	for (uint32_t i = 0; i < presetCount; i++)
	{
		const uint8_t *rec = v->phdr + (size_t)i * SF2_PHDR_SIZE;
		uint16_t wPreset = read_le16(rec + 20);
		uint16_t wBank = read_le16(rec + 22);
		for (size_t u = 0; u < used->count; u++)
		{
			uint16_t wantBank = used->items[u].bank14;
			uint16_t wantPreset = used->items[u].program;
			if (wPreset == wantPreset && wBank == wantBank)
			{
				keep[i] = 1;
				break;
			}
		}
	}

	// If nothing matched, try MSB-only mapping (bank14>>7)
	uint32_t kept = 0;
	for (uint32_t i = 0; i < presetCount; i++) if (keep[i]) kept++;
	if (kept == 0)
	{
		for (uint32_t i = 0; i < presetCount; i++)
		{
			const uint8_t *rec = v->phdr + (size_t)i * SF2_PHDR_SIZE;
			uint16_t wPreset = read_le16(rec + 20);
			uint16_t wBank = read_le16(rec + 22);
			for (size_t u = 0; u < used->count; u++)
			{
				uint16_t wantPreset = used->items[u].program;
				uint16_t wantBankMSB = (uint16_t)(used->items[u].bank14 >> 7);
				if (wPreset == wantPreset && wBank == wantBankMSB)
				{
					keep[i] = 1;
					break;
				}
			}
		}
		for (uint32_t i = 0; i < presetCount; i++) if (keep[i]) kept++;
	}

	// Percussion mapping:
	// MIDI percussion is on channel 10, but many files do NOT send bank select 128.
	// For SF2, percussion kits are conventionally at bank 128.
	// Preserve bank 128 preset 0 (Standard Kit) whenever percussion is used,
	// plus bank 128 preset == program for any used percussion program changes.
	if (sf2_any_used_percussion(used))
	{
		for (uint32_t i = 0; i < presetCount; i++)
		{
			const uint8_t *rec = v->phdr + (size_t)i * SF2_PHDR_SIZE;
			uint16_t wPreset = read_le16(rec + 20);
			uint16_t wBank = read_le16(rec + 22);
			if (wBank != 128) continue;

			if (wPreset == 0)
			{
				keep[i] = 1;
				continue;
			}

			for (size_t u = 0; u < used->count; u++)
			{
				if (!used->items[u].isPerc) continue;
				if (wPreset == (uint16_t)used->items[u].program)
				{
					keep[i] = 1;
					break;
				}
			}
		}
	}

	*outKeepPreset = keep;
	*outPresetCount = presetCount;
	return 1;
}

static int sf2_collect_needed_instruments(const SF2View *v, const uint8_t *keepPreset, uint32_t presetCount, uint8_t **outKeepInst, uint32_t *outInstCount)
{
	if (!v || !keepPreset || !outKeepInst || !outInstCount) return 0;
	uint32_t instRecs = v->instSize / SF2_INST_SIZE;
	if (instRecs < 2) return 0;
	uint32_t instCount = instRecs - 1;
	uint8_t *keepInst = (uint8_t *)calloc((size_t)instCount, 1);
	if (!keepInst) return 0;

	uint32_t pbagCount = v->pbagSize / SF2_PBAG_SIZE;
	uint32_t pgenCount = v->pgenSize / SF2_PGEN_SIZE;

	for (uint32_t pi = 0; pi < presetCount; pi++)
	{
		if (!keepPreset[pi]) continue;
		const uint8_t *prec = v->phdr + (size_t)pi * SF2_PHDR_SIZE;
		uint16_t bagStart = read_le16(prec + 24);
		const uint8_t *precNext = v->phdr + (size_t)(pi + 1) * SF2_PHDR_SIZE;
		uint16_t bagEnd = read_le16(precNext + 24);
		if (bagStart >= pbagCount || bagEnd > pbagCount || bagEnd < bagStart) continue;

		for (uint16_t bj = bagStart; bj < bagEnd && (uint32_t)bj + 1 < pbagCount; bj++)
		{
			const uint8_t *b = v->pbag + (size_t)bj * SF2_PBAG_SIZE;
			uint16_t genStart = read_le16(b);
			const uint8_t *bNext = v->pbag + (size_t)(bj + 1) * SF2_PBAG_SIZE;
			uint16_t genEnd = read_le16(bNext);
			if (genStart > pgenCount || genEnd > pgenCount || genEnd < genStart) continue;

			for (uint16_t gi = genStart; gi < genEnd; gi++)
			{
				const uint8_t *g = v->pgen + (size_t)gi * SF2_PGEN_SIZE;
				uint16_t oper = read_le16(g);
				uint16_t amount = read_le16(g + 2);
				if (oper == SF2_GEN_INSTRUMENT)
				{
					if (amount < instCount)
						keepInst[amount] = 1;
				}
			}
		}
	}

	*outKeepInst = keepInst;
	*outInstCount = instCount;
	return 1;
}

static int sf2_collect_needed_samples(const SF2View *v, const uint8_t *keepInst, uint32_t instCount, uint8_t **outKeepSample, uint32_t *outSampleCount)
{
	if (!v || !keepInst || !outKeepSample || !outSampleCount) return 0;
	uint32_t shdrRecs = v->shdrSize / SF2_SHDR_SIZE;
	if (shdrRecs < 2) return 0;
	uint32_t sampleCount = shdrRecs - 1;
	uint8_t *keepSample = (uint8_t *)calloc((size_t)sampleCount, 1);
	if (!keepSample) return 0;

	uint32_t ibagCount = v->ibagSize / SF2_IBAG_SIZE;
	uint32_t igenCount = v->igenSize / SF2_IGEN_SIZE;

	for (uint32_t ii = 0; ii < instCount; ii++)
	{
		if (!keepInst[ii]) continue;
		const uint8_t *irec = v->inst + (size_t)ii * SF2_INST_SIZE;
		uint16_t bagStart = read_le16(irec + 20);
		const uint8_t *irecNext = v->inst + (size_t)(ii + 1) * SF2_INST_SIZE;
		uint16_t bagEnd = read_le16(irecNext + 20);
		if (bagStart >= ibagCount || bagEnd > ibagCount || bagEnd < bagStart) continue;

		for (uint16_t bj = bagStart; bj < bagEnd && (uint32_t)bj + 1 < ibagCount; bj++)
		{
			const uint8_t *b = v->ibag + (size_t)bj * SF2_IBAG_SIZE;
			uint16_t genStart = read_le16(b);
			const uint8_t *bNext = v->ibag + (size_t)(bj + 1) * SF2_IBAG_SIZE;
			uint16_t genEnd = read_le16(bNext);
			if (genStart > igenCount || genEnd > igenCount || genEnd < genStart) continue;
			for (uint16_t gi = genStart; gi < genEnd; gi++)
			{
				const uint8_t *g = v->igen + (size_t)gi * SF2_IGEN_SIZE;
				uint16_t oper = read_le16(g);
				uint16_t amount = read_le16(g + 2);
				if (oper == SF2_GEN_SAMPLEID)
				{
					if (amount < sampleCount)
						keepSample[amount] = 1;
				}
			}
		}
	}

	// Closure over sample links
	int changed;
	do {
		changed = 0;
		for (uint32_t si = 0; si < sampleCount; si++)
		{
			if (!keepSample[si]) continue;
			const uint8_t *srec = v->shdr + (size_t)si * SF2_SHDR_SIZE;
			uint16_t link = read_le16(srec + 42);
			if (link < sampleCount && !keepSample[link])
			{
				keepSample[link] = 1;
				changed = 1;
			}
		}
	} while (changed);

	*outKeepSample = keepSample;
	*outSampleCount = sampleCount;
	return 1;
}

static int sf2_build_trimmed_sfbk(const SF2View *v,
										const uint8_t *keepPreset, uint32_t presetCount,
										const uint8_t *keepInst, uint32_t instCount,
										const uint8_t *keepSample, uint32_t sampleCount,
										uint8_t **outSf2, size_t *outSf2Len)
{
	if (!v || !outSf2 || !outSf2Len) return 0;

	// Build maps
	int32_t *instMap = (int32_t *)malloc((size_t)instCount * sizeof(int32_t));
	int32_t *sampMap = (int32_t *)malloc((size_t)sampleCount * sizeof(int32_t));
	if (!instMap || !sampMap)
	{
		free(instMap);
		free(sampMap);
		return 0;
	}
	for (uint32_t i = 0; i < instCount; i++) instMap[i] = -1;
	for (uint32_t i = 0; i < sampleCount; i++) sampMap[i] = -1;

	uint32_t newInstCount = 0;
	for (uint32_t i = 0; i < instCount; i++)
		if (keepInst[i]) instMap[i] = (int32_t)newInstCount++;

	uint32_t newSampleCount = 0;
	for (uint32_t i = 0; i < sampleCount; i++)
		if (keepSample[i]) sampMap[i] = (int32_t)newSampleCount++;

	if (newInstCount == 0 || newSampleCount == 0)
	{
		free(instMap);
		free(sampMap);
		return 0;
	}

	// Build sdta: smpl (+ optional sm24)
	const uint8_t *smplPayload = v->smpl + 8;
	uint32_t smplSize = read_le32(v->smpl + 4);
	const uint8_t *sm24Payload = v->sm24 ? (v->sm24 + 8) : NULL;
	uint32_t sm24Size = v->sm24 ? read_le32(v->sm24 + 4) : 0;

	ByteBuf smplOut = {0};
	ByteBuf sm24Out = {0};
	ByteBuf shdrOut = {0};

	uint32_t sampleCursor = 0; // in sample points
	for (uint32_t si = 0; si < sampleCount; si++)
	{
		if (!keepSample[si]) continue;
		const uint8_t *srec = v->shdr + (size_t)si * SF2_SHDR_SIZE;
		uint32_t start = read_le32(srec + 20);
		uint32_t end = read_le32(srec + 24);
		uint32_t startLoop = read_le32(srec + 28);
		uint32_t endLoop = read_le32(srec + 32);
		if (end < start) continue;
		uint32_t lenPts = end - start;
		uint64_t needBytes = (uint64_t)end * 2;
		if (needBytes > smplSize) continue;

		// Copy sample data
		if (!bb_append(&smplOut, smplPayload + (size_t)start * 2, (size_t)lenPts * 2))
		{
			bb_free(&smplOut);
			bb_free(&sm24Out);
			bb_free(&shdrOut);
			free(instMap);
			free(sampMap);
			return 0;
		}
		if (sm24Payload && sm24Size >= end)
		{
			if (!bb_append(&sm24Out, sm24Payload + (size_t)start, (size_t)lenPts))
			{
				bb_free(&smplOut);
				bb_free(&sm24Out);
				bb_free(&shdrOut);
				free(instMap);
				free(sampMap);
				return 0;
			}
		}

		// Emit shdr record with adjusted offsets
		uint8_t rec[SF2_SHDR_SIZE];
		memcpy(rec, srec, SF2_SHDR_SIZE);
		write_le32(rec + 20, sampleCursor);
		write_le32(rec + 24, sampleCursor + lenPts);
		if (startLoop >= start && startLoop <= end)
			write_le32(rec + 28, sampleCursor + (startLoop - start));
		else
			write_le32(rec + 28, sampleCursor);
		if (endLoop >= start && endLoop <= end)
			write_le32(rec + 32, sampleCursor + (endLoop - start));
		else
			write_le32(rec + 32, sampleCursor + lenPts);

		uint16_t link = read_le16(rec + 42);
		if (link < sampleCount && keepSample[link])
			write_le16(rec + 42, (uint16_t)sampMap[link]);
		else
			write_le16(rec + 42, 0);

		if (!bb_append(&shdrOut, rec, sizeof(rec)))
		{
			bb_free(&smplOut);
			bb_free(&sm24Out);
			bb_free(&shdrOut);
			free(instMap);
			free(sampMap);
			return 0;
		}
		sampleCursor += lenPts;
	}

	// Append EOS shdr terminal record
	{
		const uint8_t *eosRec = v->shdr + (size_t)sampleCount * SF2_SHDR_SIZE;
		uint8_t rec[SF2_SHDR_SIZE];
		memcpy(rec, eosRec, SF2_SHDR_SIZE);
		write_le32(rec + 20, sampleCursor);
		write_le32(rec + 24, sampleCursor);
		write_le32(rec + 28, sampleCursor);
		write_le32(rec + 32, sampleCursor);
		write_le16(rec + 42, 0);
		write_le16(rec + 44, 0);
		if (!bb_append(&shdrOut, rec, sizeof(rec)))
		{
			bb_free(&smplOut);
			bb_free(&sm24Out);
			bb_free(&shdrOut);
			free(instMap);
			free(sampMap);
			return 0;
		}
	}

	// Build pdta chunks
	ByteBuf phdrOut = {0}, pbagOut = {0}, pmodOut = {0}, pgenOut = {0};
	ByteBuf instOut = {0}, ibagOut = {0}, imodOut = {0}, igenOut = {0};

	uint32_t oldPbagCount = v->pbagSize / SF2_PBAG_SIZE;
	uint32_t oldPgenCount = v->pgenSize / SF2_PGEN_SIZE;
	uint32_t oldPmodCount = v->pmodSize / SF2_PMOD_SIZE;
	uint32_t oldIbagCount = v->ibagSize / SF2_IBAG_SIZE;
	uint32_t oldIgenCount = v->igenSize / SF2_IGEN_SIZE;
	uint32_t oldImodCount = v->imodSize / SF2_IMOD_SIZE;

	uint32_t newPbagCount = 0, newPgenCount = 0, newPmodCount = 0;
	for (uint32_t pi = 0; pi < presetCount; pi++)
	{
		if (!keepPreset[pi]) continue;
		const uint8_t *prec = v->phdr + (size_t)pi * SF2_PHDR_SIZE;
		const uint8_t *precNext = v->phdr + (size_t)(pi + 1) * SF2_PHDR_SIZE;
		uint16_t bagStart = read_le16(prec + 24);
		uint16_t bagEnd = read_le16(precNext + 24);
		if (bagStart >= oldPbagCount || bagEnd > oldPbagCount || bagEnd < bagStart) continue;

		uint8_t rec[SF2_PHDR_SIZE];
		memcpy(rec, prec, SF2_PHDR_SIZE);
		write_le16(rec + 24, (uint16_t)newPbagCount);
		if (!bb_append(&phdrOut, rec, sizeof(rec))) goto sf2_build_fail;

		for (uint16_t bj = bagStart; bj < bagEnd && (uint32_t)bj + 1 < oldPbagCount; bj++)
		{
			const uint8_t *b = v->pbag + (size_t)bj * SF2_PBAG_SIZE;
			const uint8_t *bNext = v->pbag + (size_t)(bj + 1) * SF2_PBAG_SIZE;
			uint16_t genStart = read_le16(b);
			uint16_t modStart = read_le16(b + 2);
			uint16_t genEnd = read_le16(bNext);
			uint16_t modEnd = read_le16(bNext + 2);
			if (genStart > oldPgenCount || genEnd > oldPgenCount || genEnd < genStart) continue;
			if (modStart > oldPmodCount || modEnd > oldPmodCount || modEnd < modStart) continue;

			uint8_t bagRec[SF2_PBAG_SIZE];
			write_le16(bagRec, (uint16_t)newPgenCount);
			write_le16(bagRec + 2, (uint16_t)newPmodCount);
			if (!bb_append(&pbagOut, bagRec, sizeof(bagRec))) goto sf2_build_fail;
			newPbagCount++;

			for (uint16_t gi = genStart; gi < genEnd; gi++)
			{
				const uint8_t *g = v->pgen + (size_t)gi * SF2_PGEN_SIZE;
				uint16_t oper = read_le16(g);
				uint16_t amt = read_le16(g + 2);
				uint8_t outRec[SF2_PGEN_SIZE];
				write_le16(outRec, oper);
				if (oper == SF2_GEN_INSTRUMENT)
				{
					if (amt < instCount && keepInst[amt] && instMap[amt] >= 0)
						write_le16(outRec + 2, (uint16_t)instMap[amt]);
					else
						write_le16(outRec + 2, 0);
				}
				else
				{
					memcpy(outRec + 2, g + 2, 2);
				}
				if (!bb_append(&pgenOut, outRec, sizeof(outRec))) goto sf2_build_fail;
				newPgenCount++;
			}

			for (uint16_t mi = modStart; mi < modEnd; mi++)
			{
				const uint8_t *m = v->pmod + (size_t)mi * SF2_PMOD_SIZE;
				if (!bb_append(&pmodOut, m, SF2_PMOD_SIZE)) goto sf2_build_fail;
				newPmodCount++;
			}
		}
	}

	// terminal pbag
	{
		uint8_t bagRec[SF2_PBAG_SIZE];
		write_le16(bagRec, (uint16_t)newPgenCount);
		write_le16(bagRec + 2, (uint16_t)newPmodCount);
		if (!bb_append(&pbagOut, bagRec, sizeof(bagRec))) goto sf2_build_fail;
		newPbagCount++;
	}

	// EOP phdr
	{
		const uint8_t *eopRec = v->phdr + (size_t)presetCount * SF2_PHDR_SIZE;
		uint8_t rec[SF2_PHDR_SIZE];
		memcpy(rec, eopRec, SF2_PHDR_SIZE);
		write_le16(rec + 24, (uint16_t)(newPbagCount - 1));
		if (!bb_append(&phdrOut, rec, sizeof(rec))) goto sf2_build_fail;
	}

	// Instruments + ibag/igen/imod
	uint32_t newIbagCount = 0, newIgenCount = 0, newImodCount = 0;
	for (uint32_t ii = 0; ii < instCount; ii++)
	{
		if (!keepInst[ii]) continue;
		const uint8_t *irec = v->inst + (size_t)ii * SF2_INST_SIZE;
		const uint8_t *irecNext = v->inst + (size_t)(ii + 1) * SF2_INST_SIZE;
		uint16_t bagStart = read_le16(irec + 20);
		uint16_t bagEnd = read_le16(irecNext + 20);
		if (bagStart >= oldIbagCount || bagEnd > oldIbagCount || bagEnd < bagStart) continue;

		uint8_t rec[SF2_INST_SIZE];
		memcpy(rec, irec, SF2_INST_SIZE);
		write_le16(rec + 20, (uint16_t)newIbagCount);
		if (!bb_append(&instOut, rec, sizeof(rec))) goto sf2_build_fail;

		for (uint16_t bj = bagStart; bj < bagEnd && (uint32_t)bj + 1 < oldIbagCount; bj++)
		{
			const uint8_t *b = v->ibag + (size_t)bj * SF2_IBAG_SIZE;
			const uint8_t *bNext = v->ibag + (size_t)(bj + 1) * SF2_IBAG_SIZE;
			uint16_t genStart = read_le16(b);
			uint16_t modStart = read_le16(b + 2);
			uint16_t genEnd = read_le16(bNext);
			uint16_t modEnd = read_le16(bNext + 2);
			if (genStart > oldIgenCount || genEnd > oldIgenCount || genEnd < genStart) continue;
			if (modStart > oldImodCount || modEnd > oldImodCount || modEnd < modStart) continue;

			uint8_t bagRec[SF2_IBAG_SIZE];
			write_le16(bagRec, (uint16_t)newIgenCount);
			write_le16(bagRec + 2, (uint16_t)newImodCount);
			if (!bb_append(&ibagOut, bagRec, sizeof(bagRec))) goto sf2_build_fail;
			newIbagCount++;

			for (uint16_t gi = genStart; gi < genEnd; gi++)
			{
				const uint8_t *g = v->igen + (size_t)gi * SF2_IGEN_SIZE;
				uint16_t oper = read_le16(g);
				uint16_t amt = read_le16(g + 2);
				uint8_t outRec[SF2_IGEN_SIZE];
				write_le16(outRec, oper);
				if (oper == SF2_GEN_SAMPLEID)
				{
					if (amt < sampleCount && keepSample[amt] && sampMap[amt] >= 0)
						write_le16(outRec + 2, (uint16_t)sampMap[amt]);
					else
						write_le16(outRec + 2, 0);
				}
				else
				{
					memcpy(outRec + 2, g + 2, 2);
				}
				if (!bb_append(&igenOut, outRec, sizeof(outRec))) goto sf2_build_fail;
				newIgenCount++;
			}

			for (uint16_t mi = modStart; mi < modEnd; mi++)
			{
				const uint8_t *m = v->imod + (size_t)mi * SF2_IMOD_SIZE;
				if (!bb_append(&imodOut, m, SF2_IMOD_SIZE)) goto sf2_build_fail;
				newImodCount++;
			}
		}
	}
	// terminal ibag
	{
		uint8_t bagRec[SF2_IBAG_SIZE];
		write_le16(bagRec, (uint16_t)newIgenCount);
		write_le16(bagRec + 2, (uint16_t)newImodCount);
		if (!bb_append(&ibagOut, bagRec, sizeof(bagRec))) goto sf2_build_fail;
		newIbagCount++;
	}
	// EOI inst
	{
		const uint8_t *eoiRec = v->inst + (size_t)instCount * SF2_INST_SIZE;
		uint8_t rec[SF2_INST_SIZE];
		memcpy(rec, eoiRec, SF2_INST_SIZE);
		write_le16(rec + 20, (uint16_t)(newIbagCount - 1));
		if (!bb_append(&instOut, rec, sizeof(rec))) goto sf2_build_fail;
	}

	// Build full SF2 RIFF
	ByteBuf out = {0};
	bb_append_fourcc(&out, "RIFF");
	size_t riffSizePos = out.size;
	bb_append_u32le(&out, 0);
	bb_append_fourcc(&out, "sfbk");

	// Copy INFO as-is if present
	if (v->listINFO && v->listINFOTotal >= 12)
	{
		bb_append(&out, v->listINFO, v->listINFOTotal);
		bb_append_pad_even(&out);
	}

	// sdta LIST
	bb_append_fourcc(&out, "LIST");
	size_t sdtaSizePos = out.size;
	bb_append_u32le(&out, 0);
	bb_append_fourcc(&out, "sdta");
	// smpl
	bb_append_fourcc(&out, "smpl");
	bb_append_u32le(&out, (uint32_t)smplOut.size);
	bb_append(&out, smplOut.data, smplOut.size);
	bb_append_pad_even(&out);
	// sm24 (optional)
	if (v->sm24)
	{
		bb_append_fourcc(&out, "sm24");
		bb_append_u32le(&out, (uint32_t)sm24Out.size);
		bb_append(&out, sm24Out.data, sm24Out.size);
		bb_append_pad_even(&out);
	}
	uint32_t sdtaDataSize = (uint32_t)(out.size - (sdtaSizePos + 4));
	write_le32(out.data + sdtaSizePos, sdtaDataSize);
	bb_append_pad_even(&out);

	// pdta LIST
	bb_append_fourcc(&out, "LIST");
	size_t pdtaSizePos = out.size;
	bb_append_u32le(&out, 0);
	bb_append_fourcc(&out, "pdta");
	// helper macro to append a pdta subchunk
	{
		// phdr
		bb_append_fourcc(&out, "phdr");
		bb_append_u32le(&out, (uint32_t)phdrOut.size);
		bb_append(&out, phdrOut.data, phdrOut.size);
		bb_append_pad_even(&out);
		// pbag
		bb_append_fourcc(&out, "pbag");
		bb_append_u32le(&out, (uint32_t)pbagOut.size);
		bb_append(&out, pbagOut.data, pbagOut.size);
		bb_append_pad_even(&out);
		// pmod
		bb_append_fourcc(&out, "pmod");
		bb_append_u32le(&out, (uint32_t)pmodOut.size);
		bb_append(&out, pmodOut.data, pmodOut.size);
		bb_append_pad_even(&out);
		// pgen
		bb_append_fourcc(&out, "pgen");
		bb_append_u32le(&out, (uint32_t)pgenOut.size);
		bb_append(&out, pgenOut.data, pgenOut.size);
		bb_append_pad_even(&out);
		// inst
		bb_append_fourcc(&out, "inst");
		bb_append_u32le(&out, (uint32_t)instOut.size);
		bb_append(&out, instOut.data, instOut.size);
		bb_append_pad_even(&out);
		// ibag
		bb_append_fourcc(&out, "ibag");
		bb_append_u32le(&out, (uint32_t)ibagOut.size);
		bb_append(&out, ibagOut.data, ibagOut.size);
		bb_append_pad_even(&out);
		// imod
		bb_append_fourcc(&out, "imod");
		bb_append_u32le(&out, (uint32_t)imodOut.size);
		bb_append(&out, imodOut.data, imodOut.size);
		bb_append_pad_even(&out);
		// igen
		bb_append_fourcc(&out, "igen");
		bb_append_u32le(&out, (uint32_t)igenOut.size);
		bb_append(&out, igenOut.data, igenOut.size);
		bb_append_pad_even(&out);
		// shdr
		bb_append_fourcc(&out, "shdr");
		bb_append_u32le(&out, (uint32_t)shdrOut.size);
		bb_append(&out, shdrOut.data, shdrOut.size);
		bb_append_pad_even(&out);
	}
	uint32_t pdtaDataSize = (uint32_t)(out.size - (pdtaSizePos + 4));
	write_le32(out.data + pdtaSizePos, pdtaDataSize);
	bb_append_pad_even(&out);

	uint32_t riffSize = (uint32_t)(out.size - 8);
	write_le32(out.data + riffSizePos, riffSize);

	bb_free(&smplOut);
	bb_free(&sm24Out);
	bb_free(&shdrOut);
	bb_free(&phdrOut);
	bb_free(&pbagOut);
	bb_free(&pmodOut);
	bb_free(&pgenOut);
	bb_free(&instOut);
	bb_free(&ibagOut);
	bb_free(&imodOut);
	bb_free(&igenOut);
	free(instMap);
	free(sampMap);

	*outSf2 = out.data;
	*outSf2Len = out.size;
	return 1;

sf2_build_fail:
	bb_free(&smplOut);
	bb_free(&sm24Out);
	bb_free(&shdrOut);
	bb_free(&phdrOut);
	bb_free(&pbagOut);
	bb_free(&pmodOut);
	bb_free(&pgenOut);
	bb_free(&instOut);
	bb_free(&ibagOut);
	bb_free(&imodOut);
	bb_free(&igenOut);
	bb_free(&out);
	free(instMap);
	free(sampMap);
	return 0;
}

static int sf2_trim_minimal(const uint8_t *sf2, size_t sf2Len, const UsedInstList *used, uint8_t **outSf2, size_t *outSf2Len)
{
	if (!sf2 || sf2Len < 12 || !used || !outSf2 || !outSf2Len) return 0;
	SF2View v;
	if (!sf2_parse_view(sf2, sf2Len, &v)) return 0;

	uint8_t *keepPreset = NULL;
	uint32_t presetCount = 0;
	if (!sf2_select_presets(&v, used, &keepPreset, &presetCount)) return 0;
	uint32_t keptPresets = 0;
	for (uint32_t i = 0; i < presetCount; i++) if (keepPreset[i]) keptPresets++;
	if (keptPresets == 0)
	{
		free(keepPreset);
		return 0;
	}

	uint8_t *keepInst = NULL;
	uint32_t instCount = 0;
	if (!sf2_collect_needed_instruments(&v, keepPreset, presetCount, &keepInst, &instCount))
	{
		free(keepPreset);
		return 0;
	}
	uint32_t keptInst = 0;
	for (uint32_t i = 0; i < instCount; i++) if (keepInst[i]) keptInst++;
	if (keptInst == 0)
	{
		free(keepPreset);
		free(keepInst);
		return 0;
	}

	uint8_t *keepSample = NULL;
	uint32_t sampleCount = 0;
	if (!sf2_collect_needed_samples(&v, keepInst, instCount, &keepSample, &sampleCount))
	{
		free(keepPreset);
		free(keepInst);
		return 0;
	}
	uint32_t keptSamples = 0;
	for (uint32_t i = 0; i < sampleCount; i++) if (keepSample[i]) keptSamples++;
	if (keptSamples == 0)
	{
		free(keepPreset);
		free(keepInst);
		free(keepSample);
		return 0;
	}

	int ok = sf2_build_trimmed_sfbk(&v, keepPreset, presetCount, keepInst, instCount, keepSample, sampleCount, outSf2, outSf2Len);
	free(keepPreset);
	free(keepInst);
	free(keepSample);
	return ok;
}

// -----------------------------
// RMID writer
// -----------------------------

static int build_rmid_with_bank(const uint8_t *midi, size_t midiLen, const uint8_t *bank, size_t bankLen,
								uint8_t **outRmi, size_t *outRmiLen)
{
	if (!midi || !bank || !outRmi || !outRmiLen) return 0;

	ByteBuf b = {0};
	bb_append_fourcc(&b, "RIFF");
	size_t riffSizePos = b.size;
	bb_append_u32le(&b, 0);
	bb_append_fourcc(&b, "RMID");

	// data chunk
	bb_append_fourcc(&b, "data");
	bb_append_u32le(&b, (uint32_t)midiLen);
	bb_append(&b, midi, midiLen);
	bb_append_pad_even(&b);

	// nested soundbank RIFF chunk (SF2 or DLS)
	bb_append(&b, bank, bankLen);
	bb_append_pad_even(&b);

	uint32_t riffSize = (uint32_t)(b.size - 8);
	write_le32(b.data + riffSizePos, riffSize);

	*outRmi = b.data;
	*outRmiLen = b.size;
	return 1;
}

// -----------------------------
// RMID extraction
// -----------------------------

static const char *get_file_ext(const char *path)
{
	const char *dot = strrchr(path, '.');
	if (!dot || dot == path) return "";
	return dot;
}

static int rmid_extract(const char *rmiPath, const char *outDir)
{
	uint8_t *rmi = NULL;
	size_t rmiLen = 0;
	if (!read_file_all(rmiPath, &rmi, &rmiLen)) return 0;

	if (rmiLen < 12 || !fourcc_eq(rmi, "RIFF") || !fourcc_eq(rmi + 8, "RMID"))
	{
		fprintf(stderr, "Error: '%s' is not a valid RIFF RMID file\n", rmiPath);
		free(rmi);
		return 0;
	}

	uint32_t riffSize = read_le32(rmi + 4);
	size_t riffEnd = 8 + (size_t)riffSize;
	if (riffEnd > rmiLen) riffEnd = rmiLen;

	// Find the data chunk (MIDI)
	const uint8_t *dataChunk = NULL;
	uint32_t dataTotal = 0;
	if (!parse_riff_children(rmi, rmiLen, 12, riffEnd, &dataChunk, &dataTotal, "data", NULL))
	{
		fprintf(stderr, "Error: RMID missing 'data' chunk\n");
		free(rmi);
		return 0;
	}

	uint32_t midiLen = read_le32(dataChunk + 4);
	const uint8_t *midiData = dataChunk + 8;

	// Derive base output name from input filename
	const char *base = strrchr(rmiPath, '/');
	if (!base) base = strrchr(rmiPath, '\\');
	if (!base) base = rmiPath;
	else base++; // skip slash

	// Strip extension to get basename, then add our output extensions
	char nameBuf[4096];
	{
		const char *ext = get_file_ext(base);
		size_t nameLen = (size_t)(ext - base);
		if (nameLen >= sizeof(nameBuf)) nameLen = sizeof(nameBuf) - 1;
		memcpy(nameBuf, base, nameLen);
		nameBuf[nameLen] = '\0';
	}

	char midiOutPath[8192];
	if (outDir && outDir[0])
	{
		snprintf(midiOutPath, sizeof(midiOutPath), "%s/%s.mid", outDir, nameBuf);
	}
	else
	{
		snprintf(midiOutPath, sizeof(midiOutPath), "%s.mid", nameBuf);
	}

	// Write MIDI
	if (!write_file_all(midiOutPath, midiData, (size_t)midiLen))
	{
		free(rmi);
		return 0;
	}
	fprintf(stderr, "Extracted MIDI: %s (%u bytes)\n", midiOutPath, midiLen);

	// Look for an embedded bank RIFF chunk after the data chunk
	const uint8_t *bankChunk = NULL;
	uint32_t bankTotal = 0;

	// Find the data chunk again to know where it ends in RMID space
	size_t dataStart = (size_t)(dataChunk - rmi);
	size_t dataEnd = dataStart + dataTotal;
	if (dataEnd & 1) dataEnd++; // word align

	// Scan remaining top-level children for a DLS or sfbk RIFF/LIST
	size_t pos = dataEnd;
	while (pos + 8 <= riffEnd)
	{
		const uint8_t *c = rmi + pos;
		uint32_t csz = read_le32(c + 4);
		size_t ctotal = 8 + (size_t)csz;
		if (pos + ctotal > riffEnd) break;

		if ((fourcc_eq(c, "RIFF") || fourcc_eq(c, "LIST")) && csz >= 4)
		{
			const uint8_t *formType = c + 8;
			if (fourcc_eq(formType, "DLS ") || fourcc_eq(formType, "sfbk"))
			{
				bankChunk = c;
				bankTotal = (uint32_t)ctotal;
				break;
			}
		}

		pos += ctotal;
		if (pos & 1) pos++;
	}

	if (bankChunk && bankTotal >= 12)
	{
		const char *ext = NULL;
		const uint8_t *formType = bankChunk + 8;
		if (fourcc_eq(formType, "DLS "))
			ext = ".dls";
		else if (fourcc_eq(formType, "sfbk"))
			ext = ".sf2";

		if (ext)
		{
			char fullBankPath[8192];
			if (outDir && outDir[0])
				snprintf(fullBankPath, sizeof(fullBankPath), "%s/%s%s", outDir, nameBuf, ext);
			else
				snprintf(fullBankPath, sizeof(fullBankPath), "%s%s", nameBuf, ext);
			if (!write_file_all(fullBankPath, bankChunk, (size_t)bankTotal))
			{
				free(rmi);
				return 0;
			}
			fprintf(stderr, "Extracted bank: %s (%u bytes)\n", fullBankPath, bankTotal);
		}
		else
		{
			fprintf(stderr, "Warning: unrecognized embedded bank type, skipping\n");
		}
	}
	else
	{
		fprintf(stderr, "No embedded soundbank found in RMID\n");
	}

	free(rmi);
	return 1;
}

// -----------------------------
// CLI
// -----------------------------

static void print_usage(const char *argv0)
{
	printf("rmiutil - RMID utility (create and extract RMID files)\n");
	printf("\n");
	printf("Usage:\n");
	printf("  %s mid2rmi <input.mid> <bank.dls|bank.sf2> <output.rmi>\n", argv0);
	printf("  %s extract <input.rmi> [output_dir]\n", argv0);
	printf("\n");
	printf("mid2rmi:\n");
	printf("  Parse MIDI for used instruments, trim bank to only required\n");
	printf("  instruments/waves, and embed both into a RIFF RMID file.\n");
	printf("\n");
	printf("extract:\n");
	printf("  Extract MIDI (.mid) and embedded soundbank (.dls or .sf2)\n");
	printf("  from a RIFF RMID file. Outputs to current directory or\n");
	printf("  specified output_dir.\n");
}

static int cmd_mid2rmi(int argc, char **argv)
{
	if (argc != 4)
	{
		fprintf(stderr, "Usage: rmiutil mid2rmi <input.mid> <bank.dls|bank.sf2> <output.rmi>\n");
		return 1;
	}

	const char *midiPath = argv[1];
	const char *bankPath = argv[2];
	const char *outPath = argv[3];

	uint8_t *midi = NULL;
	size_t midiLen = 0;
	if (!read_file_all(midiPath, &midi, &midiLen)) return 1;

	UsedInstList used = {0};
	if (!scan_midi_used_instruments(midi, midiLen, &used))
	{
		fprintf(stderr, "Error: failed to parse MIDI file\n");
		free(midi);
		return 1;
	}
	fprintf(stderr, "Detected %zu used instrument(s) in MIDI\n", used.count);

	uint8_t *bank = NULL;
	size_t bankLen = 0;
	if (!read_file_all(bankPath, &bank, &bankLen))
	{
		free(midi);
		usedinst_free(&used);
		return 1;
	}

	uint8_t *embedBank = NULL;
	size_t embedBankLen = 0;

	int isRIFF = (bankLen >= 12 && fourcc_eq(bank, "RIFF"));
	int isDLS = (isRIFF && fourcc_eq(bank + 8, "DLS "));
	int isSF2 = (isRIFF && fourcc_eq(bank + 8, "sfbk"));

	if (isDLS)
	{
		if (!dls_trim_minimal(bank, bankLen, &used, &embedBank, &embedBankLen))
		{
			fprintf(stderr, "Warning: DLS trimming failed; embedding original DLS\n");
			embedBank = (uint8_t *)malloc(bankLen);
			if (!embedBank)
			{
				free(bank);
				free(midi);
				usedinst_free(&used);
				return 1;
			}
			memcpy(embedBank, bank, bankLen);
			embedBankLen = bankLen;
		}
		fprintf(stderr, "Original bank: DLS %zu bytes\n", bankLen);
		fprintf(stderr, "Embedded bank: DLS (trimmed) %zu bytes\n", embedBankLen);
	}
	else if (isSF2)
	{
		if (!sf2_trim_minimal(bank, bankLen, &used, &embedBank, &embedBankLen))
		{
			fprintf(stderr, "Warning: SF2 trimming failed; embedding original SF2\n");
			embedBank = (uint8_t *)malloc(bankLen);
			if (!embedBank)
			{
				free(bank);
				free(midi);
				usedinst_free(&used);
				return 1;
			}
			memcpy(embedBank, bank, bankLen);
			embedBankLen = bankLen;
		}
		fprintf(stderr, "Original bank: SF2 %zu bytes\n", bankLen);
		fprintf(stderr, "Embedded bank: SF2 (trimmed) %zu bytes\n", embedBankLen);
	}
	else
	{
		fprintf(stderr, "Error: bank must be RIFF DLS or RIFF sfbk (SF2)\n");
		free(bank);
		free(midi);
		usedinst_free(&used);
		return 1;
	}

	uint8_t *rmi = NULL;
	size_t rmiLen = 0;
	if (!build_rmid_with_bank(midi, midiLen, embedBank, embedBankLen, &rmi, &rmiLen))
	{
		fprintf(stderr, "Error: failed to build RMID\n");
		free(embedBank);
		free(bank);
		free(midi);
		usedinst_free(&used);
		return 1;
	}

	int ok = write_file_all(outPath, rmi, rmiLen);
	free(rmi);
	free(embedBank);
	free(bank);
	free(midi);
	usedinst_free(&used);

	if (!ok) return 1;
	fprintf(stderr, "Wrote %s (%zu bytes)\n", outPath, rmiLen);
	return 0;
}

static int cmd_extract(int argc, char **argv)
{
	if (argc < 2 || argc > 3)
	{
		fprintf(stderr, "Usage: rmiutil extract <input.rmi> [output_dir]\n");
		return 1;
	}

	const char *rmiPath = argv[1];
	const char *outDir = (argc >= 3) ? argv[2] : NULL;

	if (!rmid_extract(rmiPath, outDir)) return 1;

	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		print_usage(argv[0]);
		return 1;
	}

	const char *cmd = argv[1];

	if (strcmp(cmd, "mid2rmi") == 0)
	{
		return cmd_mid2rmi(argc - 1, argv + 1);
	}
	else if (strcmp(cmd, "extract") == 0)
	{
		return cmd_extract(argc - 1, argv + 1);
	}
	else if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0)
	{
		print_usage(argv[0]);
		return 0;
	}
	else
	{
		fprintf(stderr, "Unknown command: %s\n", cmd);
		print_usage(argv[0]);
		return 1;
	}
}
