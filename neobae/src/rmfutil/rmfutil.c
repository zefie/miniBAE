/*
 * © 2021–2026 zefie
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

/****************************************************************************
 *
 * rmfutil.c — Universal RMF/ZMF manipulation tool
 *
 * Usage: rmfutil <command> [options...]
 *
 * Commands:
 *   extract <input.rmf> <output.mid>
 *       Extract MIDI data from an RMF/ZMF file.
 *
 *   create [<bank.hsb|bank.zsb>|-] <song.mid|song.rmf> [output.rmf] [-q]
 *       Embed bank instruments into a song and save as RMF/ZMF.
 *       Omit bank or use '-' to convert without embedding instruments.
 *
 *   dump <input.rmf> <output_dir>
 *       Extract MIDI and all sample resources as WAV files.
 *
 *   info [-c|-j] <input.rmf>
 *       Print structured information about an RMF/ZMF file.
 *       -c  CSV output
 *       -j  JSON output
 *
 ****************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <NeoBAE.h>
#include <BAE_API.h>
#include <X_API.h>
#include <X_Formats.h>

/*****************************************************************************
 * Shared helpers
 *****************************************************************************/

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static void write_le_u32(unsigned char *buf, uint32_t value)
{
    buf[0] = (unsigned char)(value & 0xFF);
    buf[1] = (unsigned char)((value >> 8) & 0xFF);
    buf[2] = (unsigned char)((value >> 16) & 0xFF);
    buf[3] = (unsigned char)((value >> 24) & 0xFF);
}

static void write_le_u16(unsigned char *buf, uint16_t value)
{
    buf[0] = (unsigned char)(value & 0xFF);
    buf[1] = (unsigned char)((value >> 8) & 0xFF);
}

static uint16_t read_be16(const unsigned char *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static uint32_t read_be32(const unsigned char *data)
{
    return ((uint32_t)data[0] << 24) |
           ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) |
           (uint32_t)data[3];
}

static int verify_rmf_header(const char *path)
{
    FILE *f = fopen(path, "rb");
    unsigned char header[4];
    if (!f) return 0;
    if (fread(header, 1, 4, f) != 4) { fclose(f); return 0; }
    fclose(f);
    return (header[1] == 0x52 && header[2] == 0x45 && header[3] == 0x5A &&
            (header[0] == 0x49 || header[0] == 0x5A));
}

static const char *bool_text(int value)
{
    return value ? "Yes" : "No";
}

/*****************************************************************************
 * extract — RMF/ZMF to MIDI
 *****************************************************************************/

static int cmd_extract(const char *input_rmf, const char *output_mid)
{
    XFILE rmf_file = NULL;
    XFILENAME xfilename;
    XLongResourceID resource_id;
    XPTR midi_data = NULL;
    int32_t midi_size = 0;
    XResourceType resource_type;
    FILE *output_file = NULL;
    int result = -1;

    if (!verify_rmf_header(input_rmf))
    {
        fprintf(stderr, "Error: '%s' is not a valid RMF/ZMF file (missing IREZ/ZREZ header)\n", input_rmf);
        return -1;
    }

    XConvertPathToXFILENAME((void *)input_rmf, &xfilename);

    rmf_file = XFileOpenResource(&xfilename, TRUE);
    if (!rmf_file)
    {
        fprintf(stderr, "Error: Cannot open RMF file '%s'\n", input_rmf);
        return -1;
    }

    SongResource *song_res = (SongResource *)XGetIndexedFileResource(rmf_file, ID_SONG, &resource_id, 0, NULL, &midi_size);

    if (song_res)
    {
        SongResource_RMF *rmf_song = (SongResource_RMF *)song_res;
        XShortResourceID midi_id = 0;

        if (rmf_song->songType == SONG_TYPE_RMF)
        {
            midi_id = XGetShort(&rmf_song->rmfResourceID);
        }
        else if (rmf_song->songType == SONG_TYPE_SMS)
        {
            SongResource_SMS *sms_song = (SongResource_SMS *)song_res;
            midi_id = XGetShort(&sms_song->midiResourceID);
        }

        XDisposePtr((XPTR)song_res);

        if (midi_id != 0)
        {
            midi_data = XGetMidiData((XLongResourceID)midi_id, &midi_size, &resource_type);
        }
    }

    if (!midi_data)
    {
        midi_data = XGetIndexedFileResource(rmf_file, ID_MIDI, &resource_id, 0, NULL, &midi_size);

        if (!midi_data)
            midi_data = XGetIndexedFileResource(rmf_file, ID_MIDI_OLD, &resource_id, 0, NULL, &midi_size);

        if (!midi_data)
        {
            XPTR encoded_data = XGetIndexedFileResource(rmf_file, ID_EMID, &resource_id, 0, NULL, &midi_size);
            if (encoded_data) { XDisposePtr(encoded_data); midi_data = XGetMidiData(resource_id, &midi_size, &resource_type); }
        }

        if (!midi_data)
        {
            XPTR encoded_data = XGetIndexedFileResource(rmf_file, ID_CMID, &resource_id, 0, NULL, &midi_size);
            if (encoded_data) { XDisposePtr(encoded_data); midi_data = XGetMidiData(resource_id, &midi_size, &resource_type); }
        }

        if (!midi_data)
        {
            XPTR encoded_data = XGetIndexedFileResource(rmf_file, ID_ECMI, &resource_id, 0, NULL, &midi_size);
            if (encoded_data) { XDisposePtr(encoded_data); midi_data = XGetMidiData(resource_id, &midi_size, &resource_type); }
        }
    }

    if (!midi_data || midi_size <= 0)
    {
        fprintf(stderr, "Error: No MIDI data found in RMF file\n");
        XFileClose(rmf_file);
        return -1;
    }

    if (midi_size < 14 || memcmp(midi_data, "MThd", 4) != 0)
    {
        fprintf(stderr, "Error: Extracted data is not valid MIDI format\n");
        XDisposePtr(midi_data);
        XFileClose(rmf_file);
        return -1;
    }

    output_file = fopen(output_mid, "wb");
    if (!output_file)
    {
        fprintf(stderr, "Error: Cannot create output file '%s'\n", output_mid);
        XDisposePtr(midi_data);
        XFileClose(rmf_file);
        return -1;
    }

    if (fwrite(midi_data, 1, (size_t)midi_size, output_file) != (size_t)midi_size)
    {
        fprintf(stderr, "Error: Failed to write MIDI data\n");
        fclose(output_file);
        XDisposePtr(midi_data);
        XFileClose(rmf_file);
        return -1;
    }

    printf("%s\n", output_mid);
    result = 0;

    fclose(output_file);
    XDisposePtr(midi_data);
    XFileClose(rmf_file);

    return result;
}

/*****************************************************************************
 * create — song + optional bank -> RMF/ZMF
 *****************************************************************************/

static int cmd_create(int argc, char *argv[])
{
    const char *bankPath = NULL;
    const char *songPath = NULL;
    const char *destPath = NULL;
    int skipBank = 0;
    int quiet = 0;
    BAEResult result;
    BAEMixer mixer = NULL;
    BAEBankToken bankToken = NULL;
    BAERmfEditorDocument *document;
    unsigned int i;

    for (i = 0; (int)i < argc; ++i)
    {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))
            return 0;

        if (!strcmp(argv[i], "-q"))
        {
            quiet = 1;
            continue;
        }
        if (!bankPath)
        {
            if (!strcmp(argv[i], "-") || !strcmp(argv[i], "--no-bank"))
            {
                skipBank = 1;
                bankPath = argv[i];
                continue;
            }
            bankPath = argv[i];
            continue;
        }
        if (!songPath)      { songPath = argv[i]; continue; }
        if (!destPath)      { destPath = argv[i]; continue; }
        fprintf(stderr, "Error: unexpected argument '%s'\n", argv[i]);
        return 1;
    }

    if (skipBank && !songPath)
    {
        fprintf(stderr, "rmfutil create: missing song file path\n");
        return 1;
    }

    if (!skipBank && bankPath && !songPath)
    {
        const char *dot = strrchr(bankPath, '.');
        if (dot && (strcasecmp(dot, ".mid") == 0 || strcasecmp(dot, ".midi") == 0 ||
                    strcasecmp(dot, ".rmf") == 0 || strcasecmp(dot, ".zmf") == 0 ||
                    strcasecmp(dot, ".rmi") == 0))
        {
            songPath = bankPath;
            bankPath = NULL;
            skipBank = 1;
        }
    }

    if (!skipBank && !bankPath)
    {
        skipBank = 1;
    }

    if (!songPath)
    {
        fprintf(stderr, "rmfutil create: missing song file path\n");
        return 1;
    }

    if (!skipBank)
    {
        if (!file_exists(bankPath))
        {
            fprintf(stderr, "Error: bank file not found: %s\n", bankPath);
            return 1;
        }
    }
    if (!file_exists(songPath))
    {
        fprintf(stderr, "Error: song file not found: %s\n", songPath);
        return 1;
    }

    static char defaultDest[4096];
    if (!destPath)
    {
        const char *dot = strrchr(songPath, '.');
        size_t baseLen;
        if (dot) baseLen = (size_t)(dot - songPath);
        else     baseLen = strlen(songPath);
        if (baseLen + 16 >= sizeof(defaultDest))
        {
            fprintf(stderr, "Error: song path too long for default output name\n");
            return 1;
        }
        memcpy(defaultDest, songPath, baseLen);
        strcpy(defaultDest + baseLen, "_rmfutil.rmf");
        destPath = defaultDest;
    }

    result = BAE_Setup();
    if (result != BAE_NO_ERROR)
    {
        fprintf(stderr, "Error: BAE_Setup failed (%d)\n", (int)result);
        return 1;
    }

    if (!skipBank)
    {
        mixer = BAEMixer_New();
        if (!mixer)
        {
            fprintf(stderr, "Error: BAEMixer_New failed\n");
            BAE_Cleanup();
            return 1;
        }

        result = BAEMixer_Open(mixer,
                               BAE_RATE_44K,
                               BAE_LINEAR_INTERPOLATION,
                               BAE_USE_16 | BAE_USE_STEREO,
                               64, 16, 32, TRUE);
        if (result != BAE_NO_ERROR)
        {
            fprintf(stderr, "Error: BAEMixer_Open failed (%d)\n", (int)result);
            BAE_Cleanup();
            return 1;
        }

        if (!quiet) fprintf(stderr, "Loading bank: %s\n", bankPath);
        bankToken = NULL;
        result = BAEMixer_AddBankFromFile(mixer, (char *)bankPath, &bankToken);
        if (result != BAE_NO_ERROR)
        {
            fprintf(stderr, "Error: failed to load bank '%s' (error %d)\n", bankPath, (int)result);
            BAEMixer_UnloadBanks(mixer);
            BAE_Cleanup();
            return 1;
        }
    }

    if (!quiet) fprintf(stderr, "Loading song: %s\n", songPath);
    document = BAERmfEditorDocument_LoadFromFile((BAEPathName)songPath);
    if (!document)
    {
        fprintf(stderr, "Error: failed to load song '%s'\n", songPath);
        if (!skipBank) BAEMixer_UnloadBanks(mixer);
        BAE_Cleanup();
        return 1;
    }

    if (!skipBank)
    {
        BAERmfEditorCloneUsedResult cloneResult;

        result = BAERmfEditorDocument_CloneUsedInstrumentsFromBank(document,
                                                                    bankToken,
                                                                    &cloneResult);
        if (result != BAE_NO_ERROR)
        {
            fprintf(stderr, "Error: clone used instruments failed (%d)\n", (int)result);
            BAERmfEditorDocument_Delete(document);
            BAEMixer_UnloadBanks(mixer);
            BAE_Cleanup();
            return 1;
        }
        if (cloneResult.pitchedCount == 0 && cloneResult.percussionCount == 0)
        {
            fprintf(stderr, "Error: no matching instruments found in bank for this song\n");
            BAERmfEditorDocument_Delete(document);
            BAEMixer_UnloadBanks(mixer);
            BAE_Cleanup();
            return 1;
        }
        if (!quiet)
        {
            uint32_t mappingIndex;
            for (mappingIndex = 0; mappingIndex < cloneResult.mappingCount; ++mappingIndex)
            {
                BAERmfEditorCloneUsedMapping const *mapping = &cloneResult.mappings[mappingIndex];
                fprintf(stderr,
                        "  %s %u:%u INST %u%s -> INST %u (%u sample%s): %s\n",
                        mapping->isPercussion ? "drum" : "pitched",
                        (unsigned)mapping->sourceBank,
                        (unsigned)mapping->sourceProgram,
                        (unsigned)mapping->requestedInstID,
                        mapping->resolvedInstID != mapping->requestedInstID ? " (alias resolved)" : "",
                        (unsigned)mapping->targetInstID,
                        (unsigned)mapping->sampleCount,
                        mapping->sampleCount == 1 ? "" : "s",
                        mapping->resolvedName[0] ? mapping->resolvedName : "(unnamed)");
                if (mapping->resolvedInstID != mapping->requestedInstID)
                {
                    fprintf(stderr, "    resolved source INST: %u\n", (unsigned)mapping->resolvedInstID);
                }
            }
        }

        if (!quiet) fprintf(stderr, "Saving: %s\n", destPath);
        result = BAERmfEditorDocument_SaveAsRmf(document, (BAEPathName)destPath);
        if (result != BAE_NO_ERROR)
        {
            fprintf(stderr, "Error: save failed (%d): %s\n", (int)result, destPath);
            BAERmfEditorDocument_Delete(document);
            BAEMixer_UnloadBanks(mixer);
            BAE_Cleanup();
            return 1;
        }

        if (!quiet) fprintf(stdout, "Wrote %u pitched + %u percussion -> %s\n",
                            (unsigned)cloneResult.pitchedCount,
                            (unsigned)cloneResult.percussionCount,
                            destPath);

        BAERmfEditorDocument_Delete(document);
        BAEMixer_UnloadBanks(mixer);
        BAE_Cleanup();
        return 0;
    }

    if (!quiet) fprintf(stderr, "Saving: %s\n", destPath);
    result = BAERmfEditorDocument_SaveAsRmf(document, (BAEPathName)destPath);
    if (result != BAE_NO_ERROR)
    {
        fprintf(stderr, "Error: save failed (%d): %s\n", (int)result, destPath);
        BAERmfEditorDocument_Delete(document);
        BAE_Cleanup();
        return 1;
    }

    if (!quiet) fprintf(stdout, "%s\n", destPath);
    BAERmfEditorDocument_Delete(document);
    BAE_Cleanup();
    return 0;
}

/*****************************************************************************
 * dump — extract MIDI + all samples as WAV
 *****************************************************************************/

static int write_wav_pcm16(const char *path,
                           uint32_t sampleRate,
                           uint16_t channels,
                           const int16_t *samples,
                           size_t frameCount)
{
    FILE *file;
    uint64_t dataSize64;
    uint32_t dataSize, riffSize, byteRate;
    uint16_t blockAlign;
    unsigned char hdr[44];

    if (channels == 0)
    {
        fprintf(stderr, "Error: WAV channel count must be non-zero\n");
        return 1;
    }

    dataSize64 = (uint64_t)frameCount * (uint64_t)channels * sizeof(int16_t);
    if (dataSize64 > 0xFFFFFFFFULL)
    {
        fprintf(stderr, "Error: WAV output too large for RIFF/WAV\n");
        return 1;
    }

    dataSize = (uint32_t)dataSize64;
    riffSize = 36U + dataSize;
    blockAlign = (uint16_t)(channels * 2U);
    byteRate = sampleRate * (uint32_t)blockAlign;

    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr + 0, "RIFF", 4);
    write_le_u32(hdr + 4, riffSize);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    write_le_u32(hdr + 16, 16);
    write_le_u16(hdr + 20, 1);
    write_le_u16(hdr + 22, channels);
    write_le_u32(hdr + 24, sampleRate);
    write_le_u32(hdr + 28, byteRate);
    write_le_u16(hdr + 32, blockAlign);
    write_le_u16(hdr + 34, 16);
    memcpy(hdr + 36, "data", 4);
    write_le_u32(hdr + 40, dataSize);

    file = fopen(path, "wb");
    if (!file)
    {
        fprintf(stderr, "Error: cannot create '%s'\n", path);
        return 1;
    }
    if (fwrite(hdr, 1, sizeof(hdr), file) != sizeof(hdr))
    {
        fprintf(stderr, "Error: failed writing WAV header\n");
        fclose(file);
        return 1;
    }
    if (dataSize > 0 && fwrite(samples, 1, dataSize, file) != dataSize)
    {
        fprintf(stderr, "Error: failed writing WAV data\n");
        fclose(file);
        return 1;
    }
    fclose(file);
    return 0;
}

static int write_wav_pcm8(const char *path,
                          uint32_t sampleRate,
                          uint16_t channels,
                          const unsigned char *samples,
                          size_t frameCount)
{
    FILE *file;
    uint64_t dataSize64;
    uint32_t dataSize, riffSize, byteRate;
    uint16_t blockAlign;
    unsigned char hdr[44];

    if (channels == 0)
    {
        fprintf(stderr, "Error: WAV channel count must be non-zero\n");
        return 1;
    }

    dataSize64 = (uint64_t)frameCount * (uint64_t)channels;
    if (dataSize64 > 0xFFFFFFFFULL)
    {
        fprintf(stderr, "Error: WAV output too large for RIFF/WAV\n");
        return 1;
    }

    dataSize = (uint32_t)dataSize64;
    riffSize = 36U + dataSize;
    blockAlign = (uint16_t)(channels);
    byteRate = sampleRate * (uint32_t)blockAlign;

    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr + 0, "RIFF", 4);
    write_le_u32(hdr + 4, riffSize);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    write_le_u32(hdr + 16, 16);
    write_le_u16(hdr + 20, 1);
    write_le_u16(hdr + 22, channels);
    write_le_u32(hdr + 24, sampleRate);
    write_le_u32(hdr + 28, byteRate);
    write_le_u16(hdr + 32, blockAlign);
    write_le_u16(hdr + 34, 8);
    memcpy(hdr + 36, "data", 4);
    write_le_u32(hdr + 40, dataSize);

    file = fopen(path, "wb");
    if (!file)
    {
        fprintf(stderr, "Error: cannot create '%s'\n", path);
        return 1;
    }
    if (fwrite(hdr, 1, sizeof(hdr), file) != sizeof(hdr))
    {
        fprintf(stderr, "Error: failed writing WAV header\n");
        fclose(file);
        return 1;
    }
    if (dataSize > 0 && fwrite(samples, 1, dataSize, file) != dataSize)
    {
        fprintf(stderr, "Error: failed writing WAV data\n");
        fclose(file);
        return 1;
    }
    fclose(file);
    return 0;
}

static int cmd_dump(const char *input_rmf, const char *output_dir)
{
    XFILE rmf_file = NULL;
    XFILENAME xfilename;
    char midi_path[4096];
    int result = -1;

    if (!verify_rmf_header(input_rmf))
    {
        fprintf(stderr, "Error: '%s' is not a valid RMF/ZMF file (missing IREZ/ZREZ header)\n", input_rmf);
        return -1;
    }

    XConvertPathToXFILENAME((void *)input_rmf, &xfilename);

    rmf_file = XFileOpenResource(&xfilename, TRUE);
    if (!rmf_file)
    {
        fprintf(stderr, "Error: Cannot open RMF file '%s'\n", input_rmf);
        return -1;
    }

    XFileUseThisResourceFile(rmf_file);

    {
        const char *slash = strrchr(input_rmf, '/');
        const char *bslash = strrchr(input_rmf, '\\');
        const char *name_start = (slash > bslash) ? slash : bslash;
        if (name_start) name_start++;
        else name_start = input_rmf;

        const char *dot = strrchr(name_start, '.');
        size_t name_len;
        if (dot && dot > name_start) name_len = (size_t)(dot - name_start);
        else name_len = strlen(name_start);

        if ((size_t)snprintf(midi_path, sizeof(midi_path), "%s/%.*s.mid",
                             output_dir, (int)name_len, name_start) >= sizeof(midi_path))
        {
            fprintf(stderr, "Error: output path too long\n");
            XFileClose(rmf_file);
            return -1;
        }
    }

    {
        XLongResourceID resource_id;
        XPTR midi_data = NULL;
        int32_t midi_size = 0;
        XResourceType resource_type;

        SongResource *song_res = (SongResource *)XGetIndexedFileResource(rmf_file, ID_SONG, &resource_id, 0, NULL, &midi_size);

        if (song_res)
        {
            SongResource_RMF *rmf_song = (SongResource_RMF *)song_res;
            XShortResourceID midi_id = 0;

            if (rmf_song->songType == SONG_TYPE_RMF)
                midi_id = XGetShort(&rmf_song->rmfResourceID);
            else if (rmf_song->songType == SONG_TYPE_SMS)
            {
                SongResource_SMS *sms_song = (SongResource_SMS *)song_res;
                midi_id = XGetShort(&sms_song->midiResourceID);
            }

            XDisposePtr((XPTR)song_res);

            if (midi_id != 0)
                midi_data = XGetMidiData((XLongResourceID)midi_id, &midi_size, &resource_type);
        }

        if (!midi_data)
        {
            midi_data = XGetIndexedFileResource(rmf_file, ID_MIDI, &resource_id, 0, NULL, &midi_size);

            if (!midi_data)
                midi_data = XGetIndexedFileResource(rmf_file, ID_MIDI_OLD, &resource_id, 0, NULL, &midi_size);

            if (!midi_data)
            {
                XPTR encoded_data = XGetIndexedFileResource(rmf_file, ID_EMID, &resource_id, 0, NULL, &midi_size);
                if (encoded_data) { XDisposePtr(encoded_data); midi_data = XGetMidiData(resource_id, &midi_size, &resource_type); }
            }

            if (!midi_data)
            {
                XPTR encoded_data = XGetIndexedFileResource(rmf_file, ID_CMID, &resource_id, 0, NULL, &midi_size);
                if (encoded_data) { XDisposePtr(encoded_data); midi_data = XGetMidiData(resource_id, &midi_size, &resource_type); }
            }

            if (!midi_data)
            {
                XPTR encoded_data = XGetIndexedFileResource(rmf_file, ID_ECMI, &resource_id, 0, NULL, &midi_size);
                if (encoded_data) { XDisposePtr(encoded_data); midi_data = XGetMidiData(resource_id, &midi_size, &resource_type); }
            }
        }

        if (!midi_data || midi_size <= 0)
        {
            fprintf(stderr, "Not dumping MIDI: No MIDI data found in RMF file\n");
        } else {

            if (midi_size < 14 || memcmp(midi_data, "MThd", 4) != 0)
            {
                fprintf(stderr, "Error: Extracted data is not valid MIDI format\n");
                XDisposePtr(midi_data);
                XFileClose(rmf_file);
                return -1;
            }

            {
                FILE *out = fopen(midi_path, "wb");
                if (!out)
                {
                    fprintf(stderr, "Error: Cannot create '%s'\n", midi_path);
                    XDisposePtr(midi_data);
                    XFileClose(rmf_file);
                    return -1;
                }
                if (fwrite(midi_data, 1, (size_t)midi_size, out) != (size_t)midi_size)
                {
                    fprintf(stderr, "Error: Failed to write MIDI file\n");
                    fclose(out);
                    XDisposePtr(midi_data);
                    XFileClose(rmf_file);
                    return -1;
                }
                fclose(out);
                printf("Wrote MIDI: %s\n", midi_path);
            }
        }
        XDisposePtr(midi_data);
    }

    {
        static const XResourceType sample_types[] = { ID_SND, ID_CSND, ID_ESND };
        int type_idx;
        int sample_index = 0;

        for (type_idx = 0; type_idx < 3; type_idx++)
        {
            XResourceType type = sample_types[type_idx];
            int32_t count = XCountFileResourcesOfType(rmf_file, type);
            int32_t ri;

            for (ri = 0; ri < count; ri++)
            {
                XLongResourceID res_id;
                int32_t raw_size = 0;
                XPTR raw = XGetIndexedFileResource(rmf_file, type, &res_id, ri, NULL, &raw_size);

                if (!raw || raw_size <= 0) continue;

                {
                    SampleDataInfo info;
                    memset(&info, 0, sizeof(info));
                    XPTR sample_data = XGetSamplePtrFromSnd(raw, &info);
                    XPTR pcm = info.pMasterPtr ? info.pMasterPtr : sample_data;
                    char wav_path[4096];
                    int sample_rate_int;

                    sample_rate_int = (int)(((double)info.rate / 65536.0) + 0.5);

                    snprintf(wav_path, sizeof(wav_path), "%s/sample_%04d_%dx%d_%dHz.wav",
                             output_dir, sample_index,
                             (int)info.channels, (int)info.bitSize,
                             sample_rate_int);

                    if (info.bitSize == 16)
                    {
                        write_wav_pcm16(wav_path, sample_rate_int, (uint16_t)info.channels,
                                       (const int16_t *)pcm, info.frames);
                    }
                    else if (info.bitSize == 8)
                    {
                        write_wav_pcm8(wav_path, sample_rate_int, (uint16_t)info.channels,
                                      (const unsigned char *)pcm, info.frames);
                    }

                    printf("Wrote sample: %s (%u frames)\n", wav_path, (unsigned)info.frames);

                    if (sample_data && sample_data != info.pMasterPtr)
                        XDisposePtr(sample_data);
                    sample_index++;
                }
                XDisposePtr(raw);
            }
        }

        if (sample_index == 0)
        {
            printf("No sample resources found.\n");
        }
        else
        {
            printf("Exported %d sample(s).\n", sample_index);
        }
    }

    result = 0;
    XFileClose(rmf_file);
    return result;
}

/*****************************************************************************
 * info — RMF/ZMF file inspection (was rmfinfo)
 *****************************************************************************/

typedef enum {
    OUTPUT_NORMAL,
    OUTPUT_CSV,
    OUTPUT_JSON
} OutputFormat;

typedef struct {
    XResourceType type;
    XLongResourceID id;
    int32_t size;
    char name[256];
    char raw_format[8];
    char content[48];
    char details[256];
    int is_object_resource;
} ResourceEntry;

typedef struct {
    SongResource_Info *song_info;
    XLongResourceID song_resource_id;
    int32_t song_resource_size;
    int32_t total_types;
    int32_t total_resources;
    int32_t midi_resources;
    int32_t sample_resources;
    int32_t instrument_resources;
    int32_t entry_count;
    ResourceEntry *entries;
} RmfInspectionReport;

static const XResourceType k_rmf_resource_types[] = {
    ID_SONG,
    ID_INST,
    ID_MIDI,
    ID_MIDI_OLD,
    ID_CMID,
    ID_EMID,
    ID_ECMI,
    ID_SND,
    ID_CSND,
    ID_ESND,
    ID_RMF,
    ID_BANK,
    ID_PASSWORD,
    ID_ALIAS,
    ID_VERS,
    ID_TEXT
};

static const char *rmf_info_label(BAEInfoType t)
{
    switch (t)
    {
    case TITLE_INFO:            return "Title";
    case PERFORMED_BY_INFO:     return "Performed By";
    case COMPOSER_INFO:         return "Composer";
    case COPYRIGHT_INFO:        return "Copyright";
    case PUBLISHER_CONTACT_INFO: return "Publisher";
    case USE_OF_LICENSE_INFO:   return "Use Of License";
    case LICENSED_TO_URL_INFO:  return "Licensed URL";
    case LICENSE_TERM_INFO:     return "License Term";
    case EXPIRATION_DATE_INFO:  return "Expiration";
    case COMPOSER_NOTES_INFO:   return "Composer Notes";
    case INDEX_NUMBER_INFO:     return "Index Number";
    case GENRE_INFO:            return "Genre";
    case SUB_GENRE_INFO:        return "Sub-Genre";
    case TEMPO_DESCRIPTION_INFO: return "Tempo";
    case ORIGINAL_SOURCE_INFO:  return "Source";
    default:                    return "Unknown";
    }
}

static const char *song_type_label(SongType type)
{
    switch (type)
    {
    case SONG_TYPE_SMS:         return "SMS";
    case SONG_TYPE_RMF:         return "Structured RMF";
    case SONG_TYPE_RMF_LINEAR:  return "Linear RMF";
    default:                    return "Unknown";
    }
}

static int is_midi_resource_type(XResourceType type)
{
    return (type == ID_MIDI || type == ID_MIDI_OLD || type == ID_CMID ||
            type == ID_EMID || type == ID_ECMI);
}

static int is_sample_resource_type(XResourceType type)
{
    return (type == ID_SND || type == ID_CSND || type == ID_ESND);
}

static void format_resource_type(XResourceType type, char *buffer, size_t buffer_size)
{
    uint32_t display_type;
    unsigned char *bytes;

    if (buffer_size == 0) return;

    bytes = (unsigned char *)&type;
    display_type = ((uint32_t)bytes[0] << 24) |
                   ((uint32_t)bytes[1] << 16) |
                   ((uint32_t)bytes[2] << 8) |
                   (uint32_t)bytes[3];

    if (buffer_size >= 5)
    {
        memcpy(buffer, &display_type, 4);
        buffer[4] = '\0';
    }
    else
    {
        snprintf(buffer, buffer_size, "?");
    }
}

static void describe_codec(XResourceType codec, char *buffer, size_t buffer_size)
{
    if (codec == C_NONE)
    {
        snprintf(buffer, buffer_size, "PCM");
        return;
    }
    format_resource_type(codec, buffer, buffer_size);
}

static const char *song_info_value(const SongResource_Info *info, BAEInfoType type)
{
    if (!info) return NULL;
    switch (type)
    {
    case TITLE_INFO:            return info->title;
    case PERFORMED_BY_INFO:     return info->performed;
    case COMPOSER_INFO:         return info->composer;
    case COPYRIGHT_INFO:        return info->copyright;
    case PUBLISHER_CONTACT_INFO: return info->publisher_contact_info;
    case USE_OF_LICENSE_INFO:   return info->use_license;
    case LICENSED_TO_URL_INFO:  return info->licensed_to_URL;
    case LICENSE_TERM_INFO:     return info->license_term;
    case EXPIRATION_DATE_INFO:  return info->expire_date;
    case COMPOSER_NOTES_INFO:   return info->compser_notes;
    case INDEX_NUMBER_INFO:     return info->index_number;
    case GENRE_INFO:            return info->genre;
    case SUB_GENRE_INFO:        return info->sub_genre;
    case TEMPO_DESCRIPTION_INFO: return info->tempo_description;
    case ORIGINAL_SOURCE_INFO:  return info->original_source;
    default:                    return NULL;
    }
}

static void json_escape_string(const char *input, char *output, size_t output_size)
{
    const char *src = input;
    char *dst = output;
    char *end = output + output_size - 1;

    while (*src && dst < end - 1)
    {
        switch (*src)
        {
        case '"':  if (dst < end - 2) { *dst++ = '\\'; *dst++ = '"'; }  break;
        case '\\': if (dst < end - 2) { *dst++ = '\\'; *dst++ = '\\'; } break;
        case '\n': if (dst < end - 2) { *dst++ = '\\'; *dst++ = 'n'; }  break;
        case '\r': if (dst < end - 2) { *dst++ = '\\'; *dst++ = 'r'; }  break;
        case '\t': if (dst < end - 2) { *dst++ = '\\'; *dst++ = 't'; }  break;
        default:   *dst++ = *src; break;
        }
        src++;
    }
    *dst = '\0';
}

static void csv_escape_string(const char *input, char *output, size_t output_size)
{
    const char *src = input;
    char *dst = output;
    char *end = output + output_size - 1;
    int needs_quotes = 0;

    for (const char *p = input; *p; p++)
    {
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r')
        {
            needs_quotes = 1;
            break;
        }
    }

    if (needs_quotes && dst < end)
        *dst++ = '"';

    while (*src && dst < end - (needs_quotes ? 2 : 1))
    {
        if (*src == '"' && needs_quotes)
            *dst++ = '"';
        *dst++ = *src++;
    }

    if (needs_quotes && dst < end)
        *dst++ = '"';
    *dst = '\0';
}

static XPTR decode_midi_resource(XPTR raw, XResourceType resource_type, int32_t *io_size)
{
    XPTR decoded;

    if (!raw) return NULL;

    if (resource_type == ID_ECMI)
    {
        XDecryptData(raw, (uint32_t)*io_size);
        decoded = XDecompressPtr(raw, (uint32_t)*io_size, TRUE);
        XDisposePtr(raw);
        if (decoded)
            *io_size = (int32_t)XGetPtrSize(decoded);
        return decoded;
    }
    if (resource_type == ID_EMID)
    {
        XDecryptData(raw, (uint32_t)*io_size);
        return raw;
    }
    if (resource_type == ID_CMID)
    {
        decoded = XDecompressPtr(raw, (uint32_t)*io_size, TRUE);
        XDisposePtr(raw);
        if (decoded)
            *io_size = (int32_t)XGetPtrSize(decoded);
        return decoded;
    }
    return raw;
}

static XPTR decode_sample_resource_info(XPTR raw, XResourceType resource_type, int32_t *io_size)
{
    XPTR decoded;

    if (!raw) return NULL;

    if (resource_type == ID_CSND)
    {
        decoded = XDecompressPtr(raw, (uint32_t)*io_size, FALSE);
        XDisposePtr(raw);
        if (decoded)
            *io_size = (int32_t)XGetPtrSize(decoded);
        return decoded;
    }
    if (resource_type == ID_ESND)
    {
        XDecryptData(raw, (uint32_t)*io_size);
        return raw;
    }
    return raw;
}

static void inspect_midi_resource(ResourceEntry *entry, XPTR raw)
{
    XPTR midi_data;
    int32_t midi_size;
    uint16_t midi_format;
    uint16_t track_count;
    uint16_t division;

    snprintf(entry->content, sizeof(entry->content), "Embedded MIDI");
    midi_size = entry->size;
    midi_data = decode_midi_resource(raw, entry->type, &midi_size);
    if (!midi_data)
    {
        snprintf(entry->details, sizeof(entry->details), "decode failed");
        return;
    }

    if (midi_size < 14 || memcmp(midi_data, "MThd", 4) != 0)
    {
        snprintf(entry->details, sizeof(entry->details), "decoded %ld bytes, invalid MIDI header",
                 (long)midi_size);
        XDisposePtr(midi_data);
        return;
    }

    midi_format = read_be16((unsigned char *)midi_data + 8);
    track_count = read_be16((unsigned char *)midi_data + 10);
    division = read_be16((unsigned char *)midi_data + 12);
    if (division & 0x8000)
    {
        snprintf(entry->details, sizeof(entry->details),
                 "format %u, tracks %u, SMPTE division 0x%04X, %ld decoded bytes",
                 (unsigned)midi_format, (unsigned)track_count, (unsigned)division, (long)midi_size);
    }
    else
    {
        snprintf(entry->details, sizeof(entry->details),
                 "format %u, tracks %u, %u ticks/quarter, %ld decoded bytes",
                 (unsigned)midi_format, (unsigned)track_count, (unsigned)division, (long)midi_size);
    }
    XDisposePtr(midi_data);
}

static void inspect_sample_resource(ResourceEntry *entry, XPTR raw)
{
    XPTR sample_data;
    int32_t sample_size;
    SampleDataInfo info;
    char codec[8];
    double sample_rate;

    snprintf(entry->content, sizeof(entry->content), "Embedded Sample");
    sample_size = entry->size;
    sample_data = decode_sample_resource_info(raw, entry->type, &sample_size);
    if (!sample_data)
    {
        snprintf(entry->details, sizeof(entry->details), "decode failed");
        return;
    }

    memset(&info, 0, sizeof(info));
    if (XGetSampleInfoFromSnd(sample_data, &info) == 0)
    {
        describe_codec(info.compressionType, codec, sizeof(codec));
        sample_rate = (double)info.rate / 65536.0;
        snprintf(entry->details, sizeof(entry->details),
                 "%d ch, %d-bit, %.1f Hz, %lu frames, codec %s",
                 (int)info.channels,
                 (int)info.bitSize,
                 sample_rate,
                 (unsigned long)info.frames,
                 codec);
    }
    else
    {
        snprintf(entry->details, sizeof(entry->details), "decoded %ld bytes, unsupported SND layout",
                 (long)sample_size);
    }
    XDisposePtr(sample_data);
}

static void inspect_song_resource(ResourceEntry *entry, XPTR raw, int32_t raw_size,
                                  RmfInspectionReport *report)
{
    SongResource_Info *info;

    snprintf(entry->content, sizeof(entry->content), "Song Resource");
    info = XGetSongResourceInfo((SongResource *)raw, raw_size);
    if (!info)
    {
        snprintf(entry->details, sizeof(entry->details), "unable to parse SONG resource");
        XDisposePtr(raw);
        return;
    }

    snprintf(entry->details, sizeof(entry->details),
             "%s, object ID %d, locked %s, embedded %s, tempo %ld",
             song_type_label(info->songType),
             (int)info->objectResourceID,
             bool_text(info->songLocked),
             bool_text(info->songEmbedded),
             (long)info->songTempo);

    if (!report->song_info)
    {
        report->song_info = info;
        report->song_resource_id = entry->id;
        report->song_resource_size = raw_size;
        XDisposePtr(raw);
        return;
    }

    XDisposeSongResourceInfo(info);
    XDisposePtr(raw);
}

static int is_object_resource_for_song(const SongResource_Info *info, XResourceType type, XLongResourceID id)
{
    if (!info || id != info->objectResourceID)
        return 0;
    if (info->songType == SONG_TYPE_RMF)
        return is_midi_resource_type(type);
    if (info->songType == SONG_TYPE_RMF_LINEAR)
        return is_sample_resource_type(type);
    return 0;
}

static void free_report(RmfInspectionReport *report)
{
    if (report->song_info)
        XDisposeSongResourceInfo(report->song_info);
    free(report->entries);
    memset(report, 0, sizeof(*report));
}

static int inspect_rmf_file(const char *filename, RmfInspectionReport *report,
                            char *error_buffer, size_t error_buffer_size)
{
    XFILENAME xfilename;
    XFILE file_ref;
    int32_t type_index;
    int32_t resource_index;
    int32_t total_resources;
    int32_t entry_index;

    memset(report, 0, sizeof(*report));
    XConvertPathToXFILENAME((void *)filename, &xfilename);
    file_ref = XFileOpenResource(&xfilename, TRUE);
    if (!file_ref)
    {
        snprintf(error_buffer, error_buffer_size, "Cannot open RMF resource map");
        return 0;
    }

    XFileUseThisResourceFile(file_ref);
    report->total_types = 0;
    total_resources = 0;

    for (type_index = 0; type_index < (int32_t)(sizeof(k_rmf_resource_types) / sizeof(k_rmf_resource_types[0])); type_index++)
    {
        XResourceType type = k_rmf_resource_types[type_index];
        int32_t resource_count = XCountFileResourcesOfType(file_ref, type);

        if (resource_count > 0)
        {
            report->total_types++;
            total_resources += resource_count;
        }
    }

    report->entries = (ResourceEntry *)calloc((size_t)(total_resources > 0 ? total_resources : 1), sizeof(ResourceEntry));
    if (!report->entries)
    {
        XFileClose(file_ref);
        snprintf(error_buffer, error_buffer_size, "Out of memory while collecting resources");
        return 0;
    }

    entry_index = 0;
    for (type_index = 0; type_index < (int32_t)(sizeof(k_rmf_resource_types) / sizeof(k_rmf_resource_types[0])); type_index++)
    {
        XResourceType type = k_rmf_resource_types[type_index];
        int32_t resource_count;

        resource_count = XCountFileResourcesOfType(file_ref, type);
        for (resource_index = 0; resource_index < resource_count; resource_index++)
        {
            XPTR raw;
            ResourceEntry *entry;

            entry = &report->entries[entry_index];
            memset(entry, 0, sizeof(*entry));
            entry->type = type;
            raw = XGetIndexedFileResource(file_ref,
                                          type,
                                          &entry->id,
                                          resource_index,
                                          entry->name,
                                          &entry->size);
            format_resource_type(type, entry->raw_format, sizeof(entry->raw_format));

            if (type == ID_SONG)
            {
                inspect_song_resource(entry, raw, entry->size, report);
            }
            else if (is_midi_resource_type(type))
            {
                report->midi_resources++;
                inspect_midi_resource(entry, raw);
            }
            else if (is_sample_resource_type(type))
            {
                report->sample_resources++;
                inspect_sample_resource(entry, raw);
            }
            else if (type == ID_INST)
            {
                report->instrument_resources++;
                snprintf(entry->content, sizeof(entry->content), "Embedded Instrument");
                snprintf(entry->details, sizeof(entry->details), "instrument definition, %ld bytes",
                         (long)entry->size);
                if (raw) XDisposePtr(raw);
            }
            else
            {
                snprintf(entry->content, sizeof(entry->content), "Resource");
                snprintf(entry->details, sizeof(entry->details), "%ld bytes",
                         (long)entry->size);
                if (raw) XDisposePtr(raw);
            }
            entry_index++;
        }
    }

    report->entry_count = entry_index;
    report->total_resources = entry_index;
    if (report->song_info)
    {
        for (entry_index = 0; entry_index < report->entry_count; entry_index++)
        {
            report->entries[entry_index].is_object_resource =
                is_object_resource_for_song(report->song_info,
                                            report->entries[entry_index].type,
                                            report->entries[entry_index].id);
        }
    }
    XFileClose(file_ref);
    return 1;
}

static void print_metadata(OutputFormat output_format, const SongResource_Info *song_info,
                           int *first_field, int *has_any_info)
{
    BAEInfoType it;

    for (it = TITLE_INFO; it <= ORIGINAL_SOURCE_INFO; it = (BAEInfoType)(it + 1))
    {
        const char *value = song_info_value(song_info, it);
        if (!value || !value[0])
            continue;

        *has_any_info = 1;
        if (output_format == OUTPUT_CSV)
        {
            char escaped_field[128];
            char escaped_value[1024];
            csv_escape_string(rmf_info_label(it), escaped_field, sizeof(escaped_field));
            csv_escape_string(value, escaped_value, sizeof(escaped_value));
            printf("metadata,%s,%s\n", escaped_field, escaped_value);
        }
        else if (output_format == OUTPUT_JSON)
        {
            char escaped_value[1024];
            json_escape_string(value, escaped_value, sizeof(escaped_value));
            if (!*first_field)
                printf(",\n");
            printf("    \"%s\": \"%s\"", rmf_info_label(it), escaped_value);
            *first_field = 0;
        }
        else
        {
            printf("  %-16s: %s\n", rmf_info_label(it), value);
        }
    }
}

static void print_song_section(OutputFormat output_format, const RmfInspectionReport *report)
{
    if (!report->song_info)
        return;

    if (output_format == OUTPUT_CSV)
    {
        printf("song,SONG Resource ID,%ld\n", (long)report->song_resource_id);
        printf("song,Song Type,%s\n", song_type_label(report->song_info->songType));
        printf("song,Object Resource ID,%d\n", (int)report->song_info->objectResourceID);
        printf("song,Locked,%s\n", bool_text(report->song_info->songLocked));
        printf("song,Embedded,%s\n", bool_text(report->song_info->songEmbedded));
        printf("song,Tempo,%ld\n", (long)report->song_info->songTempo);
        printf("song,Max MIDI Notes,%d\n", (int)report->song_info->maxMidiNotes);
        printf("song,Max Effects,%d\n", (int)report->song_info->maxEffects);
        printf("song,Mix Level,%d\n", (int)report->song_info->mixLevel);
        printf("song,Song Volume,%d\n", (int)report->song_info->songVolume);
        if (report->song_info->engineConfigFlags)
        {
            int32_t f = report->song_info->engineConfigFlags;
            if (f & SONG_CONFIG_HAS_CLASSIC_CHORUS)
                printf("song,Classic Chorus,%s\n", (f & SONG_CONFIG_CLASSIC_CHORUS_ON) ? "On" : "Off");
            if (f & SONG_CONFIG_HAS_PANFIX)
                printf("song,Pan Fix,%s\n", (f & SONG_CONFIG_PANFIX_ON) ? "On" : "Off");
        }
        return;
    }

    if (output_format == OUTPUT_JSON)
    {
        printf("  \"song\": {\n");
        printf("    \"songResourceId\": %ld,\n", (long)report->song_resource_id);
        printf("    \"songType\": \"%s\",\n", song_type_label(report->song_info->songType));
        printf("    \"objectResourceId\": %d,\n", (int)report->song_info->objectResourceID);
        printf("    \"locked\": %s,\n", report->song_info->songLocked ? "true" : "false");
        printf("    \"embedded\": %s,\n", report->song_info->songEmbedded ? "true" : "false");
        printf("    \"tempo\": %ld,\n", (long)report->song_info->songTempo);
        printf("    \"maxMidiNotes\": %d,\n", (int)report->song_info->maxMidiNotes);
        printf("    \"maxEffects\": %d,\n", (int)report->song_info->maxEffects);
        printf("    \"mixLevel\": %d,\n", (int)report->song_info->mixLevel);
        printf("    \"songVolume\": %d", (int)report->song_info->songVolume);
        if (report->song_info->engineConfigFlags)
        {
            int32_t f = report->song_info->engineConfigFlags;
            if (f & SONG_CONFIG_HAS_CLASSIC_CHORUS)
                printf(",\n    \"classicChorus\": %s", (f & SONG_CONFIG_CLASSIC_CHORUS_ON) ? "true" : "false");
            if (f & SONG_CONFIG_HAS_PANFIX)
                printf(",\n    \"panFix\": %s", (f & SONG_CONFIG_PANFIX_ON) ? "true" : "false");
        }
        printf("\n  },\n");
        return;
    }

    printf("\nSong:\n");
    printf("  %-16s: %ld\n", "SONG Resource ID", (long)report->song_resource_id);
    printf("  %-16s: %s\n", "Song Type", song_type_label(report->song_info->songType));
    printf("  %-16s: %d\n", "Object Resource ID", (int)report->song_info->objectResourceID);
    printf("  %-16s: %s\n", "Locked", bool_text(report->song_info->songLocked));
    printf("  %-16s: %s\n", "Embedded", bool_text(report->song_info->songEmbedded));
    printf("  %-16s: %ld\n", "Tempo", (long)report->song_info->songTempo);
    printf("  %-16s: %d\n", "Max MIDI Notes", (int)report->song_info->maxMidiNotes);
    printf("  %-16s: %d\n", "Max Effects", (int)report->song_info->maxEffects);
    printf("  %-16s: %d\n", "Mix Level", (int)report->song_info->mixLevel);
    printf("  %-16s: %d\n", "Song Volume", (int)report->song_info->songVolume);
    if (report->song_info->engineConfigFlags)
    {
        int32_t f = report->song_info->engineConfigFlags;
        if (f & SONG_CONFIG_HAS_CLASSIC_CHORUS)
            printf("  %-16s: %s\n", "Classic Chorus", (f & SONG_CONFIG_CLASSIC_CHORUS_ON) ? "On" : "Off");
        if (f & SONG_CONFIG_HAS_PANFIX)
            printf("  %-16s: %s\n", "Pan Fix", (f & SONG_CONFIG_PANFIX_ON) ? "On" : "Off");
    }
}

static void print_summary_section(OutputFormat output_format, const RmfInspectionReport *report)
{
    if (output_format == OUTPUT_CSV)
    {
        printf("summary,Resource Types,%ld\n", (long)report->total_types);
        printf("summary,Total Resources,%ld\n", (long)report->total_resources);
        printf("summary,MIDI Resources,%ld\n", (long)report->midi_resources);
        printf("summary,Sample Resources,%ld\n", (long)report->sample_resources);
        printf("summary,Instrument Resources,%ld\n", (long)report->instrument_resources);
        return;
    }

    if (output_format == OUTPUT_JSON)
    {
        printf("  \"summary\": {\n");
        printf("    \"resourceTypes\": %ld,\n", (long)report->total_types);
        printf("    \"totalResources\": %ld,\n", (long)report->total_resources);
        printf("    \"midiResources\": %ld,\n", (long)report->midi_resources);
        printf("    \"sampleResources\": %ld,\n", (long)report->sample_resources);
        printf("    \"instrumentResources\": %ld\n", (long)report->instrument_resources);
        printf("  },\n");
        return;
    }

    printf("\nContent Summary:\n");
    printf("  %-16s: %ld\n", "Resource Types", (long)report->total_types);
    printf("  %-16s: %ld\n", "Total Resources", (long)report->total_resources);
    printf("  %-16s: %ld\n", "MIDI Resources", (long)report->midi_resources);
    printf("  %-16s: %ld\n", "Sample Resources", (long)report->sample_resources);
    printf("  %-16s: %ld\n", "Instrument Resources", (long)report->instrument_resources);
}

static void print_resources_section(OutputFormat output_format, const RmfInspectionReport *report)
{
    int i;

    if (output_format == OUTPUT_CSV)
    {
        for (i = 0; i < report->entry_count; i++)
        {
            char value[1024];
            char escaped_field[64];
            char escaped_value[1024];
            snprintf(value,
                     sizeof(value),
                     "type=%s; id=%ld; name=%s; bytes=%ld; content=%s; details=%s%s",
                     report->entries[i].raw_format,
                     (long)report->entries[i].id,
                     report->entries[i].name[0] ? report->entries[i].name : "(unnamed)",
                     (long)report->entries[i].size,
                     report->entries[i].content,
                     report->entries[i].details,
                     report->entries[i].is_object_resource ? "; object=yes" : "");
            snprintf(escaped_field, sizeof(escaped_field), "Resource %d", i);
            csv_escape_string(value, escaped_value, sizeof(escaped_value));
            printf("resource,%s,%s\n", escaped_field, escaped_value);
        }
        return;
    }

    if (output_format == OUTPUT_JSON)
    {
        printf("  \"resources\": [\n");
        for (i = 0; i < report->entry_count; i++)
        {
            char escaped_name[512];
            char escaped_content[256];
            char escaped_details[512];
            json_escape_string(report->entries[i].name[0] ? report->entries[i].name : "", escaped_name, sizeof(escaped_name));
            json_escape_string(report->entries[i].content, escaped_content, sizeof(escaped_content));
            json_escape_string(report->entries[i].details, escaped_details, sizeof(escaped_details));
            printf("    {\"index\": %d, \"type\": \"%s\", \"id\": %ld, \"name\": \"%s\", \"size\": %ld, \"content\": \"%s\", \"details\": \"%s\", \"object\": %s}%s\n",
                   i,
                   report->entries[i].raw_format,
                   (long)report->entries[i].id,
                   escaped_name,
                   (long)report->entries[i].size,
                   escaped_content,
                   escaped_details,
                   report->entries[i].is_object_resource ? "true" : "false",
                   (i + 1 < report->entry_count) ? "," : "");
        }
        printf("  ]\n");
        return;
    }

    printf("\nResources:\n");
    for (i = 0; i < report->entry_count; i++)
    {
        printf("  [%d] %s id=%ld size=%ld%s\n",
               i,
               report->entries[i].raw_format,
               (long)report->entries[i].id,
               (long)report->entries[i].size,
               report->entries[i].is_object_resource ? " [object]" : "");
        printf("      name    : %s\n", report->entries[i].name[0] ? report->entries[i].name : "(unnamed)");
        printf("      content : %s\n", report->entries[i].content);
        printf("      details : %s\n", report->entries[i].details);
    }
}

static int cmd_info(const char *filename, OutputFormat output_format)
{
    RmfInspectionReport report;
    char inspection_error[256];
    int first_field = 1;
    int has_any_info = 0;

    if (!verify_rmf_header(filename))
    {
        fprintf(stderr, "Error: '%s' is not a valid RMF file (missing RMF magic header)\n", filename);
        return 1;
    }

    if (!inspect_rmf_file(filename, &report, inspection_error, sizeof(inspection_error)))
    {
        fprintf(stderr, "Error: %s\n", inspection_error);
        return 1;
    }

    if (output_format == OUTPUT_CSV)
    {
        printf("Section,Field,Value\n");
    }
    else if (output_format == OUTPUT_JSON)
    {
        printf("{\n");
        printf("  \"file\": \"%s\",\n", filename);
        printf("  \"metadata\": {\n");
    }
    else
    {
        printf("RMF File Information: %s\n", filename);
        printf("===============================================\n");
        printf("Metadata:\n");
    }

    print_metadata(output_format, report.song_info, &first_field, &has_any_info);
    if (output_format == OUTPUT_JSON)
    {
        if (has_any_info)
            printf("\n");
        printf("  },\n");
    }

    print_song_section(output_format, &report);
    print_summary_section(output_format, &report);
    print_resources_section(output_format, &report);

    if (output_format == OUTPUT_JSON)
    {
        printf("}\n");
    }
    else if (output_format == OUTPUT_NORMAL && !has_any_info)
    {
        printf("  No RMF metadata found in file.\n");
    }

    free_report(&report);
    return 0;
}

/*****************************************************************************
 * Usage
 *****************************************************************************/

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "rmfutil — Universal RMF/ZMF manipulation tool\n"
            "\n"
            "Usage: %s <command> [options...]\n"
            "\n"
            "Commands:\n"
            "  extract <input.rmf|input.zmf> <output.mid>\n"
            "      Extract MIDI data from an RMF/ZMF file.\n"
            "\n"
            "  create [<bank.hsb|bank.zsb>|-] <song.mid|song.rmf|song.zmf> [output.rmf] [-q]\n"
            "      Load song, optionally embed instruments from a bank, and save as RMF/ZMF.\n"
            "      Omit bank or use '-' to convert without embedding instruments.\n"
            "\n"
            "  dump <input.rmf|input.zmf> <output_dir>\n"
            "      Extract MIDI and all sample resources as WAV files.\n"
            "\n"
            "  info [-c|-j] <input.rmf>\n"
            "      Print structured information about an RMF/ZMF file.\n"
            "      -c  Output as CSV\n"
            "      -j  Output as JSON\n",
            prog);
}

static void print_create_usage(const char *prog)
{
    fprintf(stderr,
            "rmfutil create — embed a bank's instruments into a song and save as RMF/ZMF\n"
            "\n"
            "Usage: %s create [<bank.hsb|bank.zsb>|-] <song.mid|song.rmf> [output.rmf] [-q]\n"
            "\n"
            "  bank file       HSB or ZSB bank file containing instruments (or '-' to skip)\n"
            "  song file       MIDI (.mid) or existing RMF/ZMF song\n"
            "  output file     Output path (default: <song>_rmfutil.rmf)\n"
            "  -q              Quiet (suppress progress output)\n"
            "\n"
            "If no bank is given, the song is loaded and saved as RMF without embedding instruments.\n",
            prog);
}

/*****************************************************************************
 * main
 *****************************************************************************/

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        print_usage(argv[0]);
        return 1;
    }

    if (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))
    {
        print_usage(argv[0]);
        return 0;
    }

    if (!strcmp(argv[1], "extract"))
    {
        if (argc != 4)
        {
            fprintf(stderr, "Usage: %s extract <input.rmf> <output.mid>\n", argv[0]);
            return 1;
        }

        BAEResult err = BAE_Setup();
        if (err != BAE_NO_ERROR)
        {
            fprintf(stderr, "Error: Failed to initialize BAE audio engine (error %d)\n", err);
            return 1;
        }

        if (!file_exists(argv[2]))
        {
            fprintf(stderr, "Error: Cannot open input file '%s'\n", argv[2]);
            BAE_Cleanup();
            return 1;
        }

        int result = cmd_extract(argv[2], argv[3]);
        BAE_Cleanup();
        return (result == 0) ? 0 : 1;
    }

    if (!strcmp(argv[1], "create"))
    {
        if (argc < 3)
        {
            print_create_usage(argv[0]);
            return 1;
        }

        return cmd_create(argc - 2, argv + 2);
    }

    if (!strcmp(argv[1], "dump"))
    {
        if (argc != 4)
        {
            fprintf(stderr, "Usage: %s dump <input.rmf> <output_dir>\n", argv[0]);
            return 1;
        }

        BAEResult err = BAE_Setup();
        if (err != BAE_NO_ERROR)
        {
            fprintf(stderr, "Error: Failed to initialize BAE audio engine (error %d)\n", err);
            return 1;
        }

        if (!file_exists(argv[2]))
        {
            fprintf(stderr, "Error: Cannot open input file '%s'\n", argv[2]);
            BAE_Cleanup();
            return 1;
        }

        int result = cmd_dump(argv[2], argv[3]);
        BAE_Cleanup();
        return (result == 0) ? 0 : 1;
    }

    if (!strcmp(argv[1], "info"))
    {
        OutputFormat output_format = OUTPUT_NORMAL;
        const char *filename = NULL;
        int i;

        for (i = 2; i < argc; i++)
        {
            if (!strcmp(argv[i], "-c"))
            {
                output_format = OUTPUT_CSV;
            }
            else if (!strcmp(argv[i], "-j"))
            {
                output_format = OUTPUT_JSON;
            }
            else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help"))
            {
                fprintf(stderr, "Usage: %s info [-c|-j] <input.rmf>\n", argv[0]);
                return 0;
            }
            else if (argv[i][0] == '-')
            {
                fprintf(stderr, "Unknown option: %s\n", argv[i]);
                return 1;
            }
            else
            {
                if (filename)
                {
                    fprintf(stderr, "Multiple files specified. Only one file is supported.\n");
                    return 1;
                }
                filename = argv[i];
            }
        }

        if (!filename)
        {
            fprintf(stderr, "Usage: %s info [-c|-j] <input.rmf>\n", argv[0]);
            return 1;
        }

        BAEResult err = BAE_Setup();
        if (err != BAE_NO_ERROR)
        {
            fprintf(stderr, "Error: Failed to initialize BAE audio engine (error %d)\n", err);
            return 1;
        }

        if (!file_exists(filename))
        {
            fprintf(stderr, "Error: Cannot open file '%s'\n", filename);
            BAE_Cleanup();
            return 1;
        }

        int result = cmd_info(filename, output_format);
        BAE_Cleanup();
        return result;
    }

    fprintf(stderr, "Error: Unknown command '%s'\n", argv[1]);
    print_usage(argv[0]);
    return 1;
}
