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
 * rmfinfo.c
 *
 * A command line RMF file information utility
 *
 * Usage: rmfinfo [-c|-j] <rmffile>
 *   -c    Comma-separated values output
 *   -j    JSON output
 *
 * Based on NeoBAE audio engine
 *
 ****************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <NeoBAE.h>
#include <BAE_API.h>
#include <X_API.h>
#include <X_Formats.h>

// Copy of rmf_info_label function from playbae.c
const char *rmf_info_label(BAEInfoType t)
{
    switch (t)
    {
    case TITLE_INFO:
        return "Title";
    case PERFORMED_BY_INFO:
        return "Performed By";
    case COMPOSER_INFO:
        return "Composer";
    case COPYRIGHT_INFO:
        return "Copyright";
    case PUBLISHER_CONTACT_INFO:
        return "Publisher";
    case USE_OF_LICENSE_INFO:
        return "Use Of License";
    case LICENSED_TO_URL_INFO:
        return "Licensed URL";
    case LICENSE_TERM_INFO:
        return "License Term";
    case EXPIRATION_DATE_INFO:
        return "Expiration";
    case COMPOSER_NOTES_INFO:
        return "Composer Notes";
    case INDEX_NUMBER_INFO:
        return "Index Number";
    case GENRE_INFO:
        return "Genre";
    case SUB_GENRE_INFO:
        return "Sub-Genre";
    case TEMPO_DESCRIPTION_INFO:
        return "Tempo";
    case ORIGINAL_SOURCE_INFO:
        return "Source";
    default:
        return "Unknown";
    }
}

typedef struct {
    XResourceType type;
    XLongResourceID id;
    int32_t size;
    char name[256];
    char raw_format[8];
    char content[48];
    char details[256];
    bool is_object_resource;
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

static const char *bool_text(bool value)
{
    return value ? "Yes" : "No";
}

static const char *song_type_label(SongType type)
{
    switch (type)
    {
    case SONG_TYPE_SMS:
        return "SMS";
    case SONG_TYPE_RMF:
        return "Structured RMF";
    case SONG_TYPE_RMF_LINEAR:
        return "Linear RMF";
    default:
        return "Unknown";
    }
}

static bool is_midi_resource_type(XResourceType type)
{
    return (type == ID_MIDI || type == ID_MIDI_OLD || type == ID_CMID ||
            type == ID_EMID || type == ID_ECMI);
}

static bool is_sample_resource_type(XResourceType type)
{
    return (type == ID_SND || type == ID_CSND || type == ID_ESND);
}

static void format_resource_type(XResourceType type, char *buffer, size_t buffer_size)
{
    uint32_t display_type;
    unsigned char *bytes;

    if (buffer_size == 0)
    {
        return;
    }

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

static const char *song_info_value(const SongResource_Info *info, BAEInfoType type)
{
    if (!info)
    {
        return NULL;
    }

    switch (type)
    {
    case TITLE_INFO:
        return info->title;
    case PERFORMED_BY_INFO:
        return info->performed;
    case COMPOSER_INFO:
        return info->composer;
    case COPYRIGHT_INFO:
        return info->copyright;
    case PUBLISHER_CONTACT_INFO:
        return info->publisher_contact_info;
    case USE_OF_LICENSE_INFO:
        return info->use_license;
    case LICENSED_TO_URL_INFO:
        return info->licensed_to_URL;
    case LICENSE_TERM_INFO:
        return info->license_term;
    case EXPIRATION_DATE_INFO:
        return info->expire_date;
    case COMPOSER_NOTES_INFO:
        return info->compser_notes;
    case INDEX_NUMBER_INFO:
        return info->index_number;
    case GENRE_INFO:
        return info->genre;
    case SUB_GENRE_INFO:
        return info->sub_genre;
    case TEMPO_DESCRIPTION_INFO:
        return info->tempo_description;
    case ORIGINAL_SOURCE_INFO:
        return info->original_source;
    default:
        return NULL;
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

static XPTR decode_midi_resource(XPTR raw, XResourceType resource_type, int32_t *io_size)
{
    XPTR decoded;

    if (!raw)
    {
        return NULL;
    }

    if (resource_type == ID_ECMI)
    {
        XDecryptData(raw, (uint32_t)*io_size);
        decoded = XDecompressPtr(raw, (uint32_t)*io_size, TRUE);
        XDisposePtr(raw);
        if (decoded)
        {
            *io_size = (int32_t)XGetPtrSize(decoded);
        }
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
        {
            *io_size = (int32_t)XGetPtrSize(decoded);
        }
        return decoded;
    }
    return raw;
}

static XPTR decode_sample_resource(XPTR raw, XResourceType resource_type, int32_t *io_size)
{
    XPTR decoded;

    if (!raw)
    {
        return NULL;
    }

    if (resource_type == ID_CSND)
    {
        decoded = XDecompressPtr(raw, (uint32_t)*io_size, FALSE);
        XDisposePtr(raw);
        if (decoded)
        {
            *io_size = (int32_t)XGetPtrSize(decoded);
        }
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
    sample_data = decode_sample_resource(raw, entry->type, &sample_size);
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

static bool is_object_resource_for_song(const SongResource_Info *info, XResourceType type, XLongResourceID id)
{
    if (!info || id != info->objectResourceID)
    {
        return FALSE;
    }
    if (info->songType == SONG_TYPE_RMF)
    {
        return is_midi_resource_type(type);
    }
    if (info->songType == SONG_TYPE_RMF_LINEAR)
    {
        return is_sample_resource_type(type);
    }
    return FALSE;
}

static void free_report(RmfInspectionReport *report)
{
    if (report->song_info)
    {
        XDisposeSongResourceInfo(report->song_info);
    }
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
                if (raw)
                {
                    XDisposePtr(raw);
                }
            }
            else
            {
                snprintf(entry->content, sizeof(entry->content), "Resource");
                snprintf(entry->details, sizeof(entry->details), "%ld bytes",
                         (long)entry->size);
                if (raw)
                {
                    XDisposePtr(raw);
                }
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

// Check if file has RMF/ZMF magic header (IREZ or ZREZ)
int is_rmf_file(const char *filename)
{
    FILE *file = fopen(filename, "rb");
    if (!file) {
        return 0;
    }

    unsigned char header[4];
    if (fread(header, 1, 4, file) != 4) {
        fclose(file);
        return 0;
    }
    fclose(file);

    // Check for RMF magic bytes "IREZ" or ZMF magic bytes "ZREZ"
    if (header[1] == 0x52 && header[2] == 0x45 && header[3] == 0x5A) {
        return (header[0] == 0x49 || header[0] == 0x5A);  // 'I' or 'Z'
    }
    return 0;
}

// Escape a string for JSON output
void json_escape_string(const char *input, char *output, size_t output_size)
{
    const char *src = input;
    char *dst = output;
    char *end = output + output_size - 1;
    
    while (*src && dst < end - 1) {
        switch (*src) {
            case '"':
                if (dst < end - 2) {
                    *dst++ = '\\';
                    *dst++ = '"';
                }
                break;
            case '\\':
                if (dst < end - 2) {
                    *dst++ = '\\';
                    *dst++ = '\\';
                }
                break;
            case '\n':
                if (dst < end - 2) {
                    *dst++ = '\\';
                    *dst++ = 'n';
                }
                break;
            case '\r':
                if (dst < end - 2) {
                    *dst++ = '\\';
                    *dst++ = 'r';
                }
                break;
            case '\t':
                if (dst < end - 2) {
                    *dst++ = '\\';
                    *dst++ = 't';
                }
                break;
            default:
                *dst++ = *src;
                break;
        }
        src++;
    }
    *dst = '\0';
}

// Escape a string for CSV output
void csv_escape_string(const char *input, char *output, size_t output_size)
{
    const char *src = input;
    char *dst = output;
    char *end = output + output_size - 1;
    int needs_quotes = 0;
    
    // Check if we need quotes (contains comma, quote, or newline)
    for (const char *p = input; *p; p++) {
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') {
            needs_quotes = 1;
            break;
        }
    }
    
    if (needs_quotes && dst < end) {
        *dst++ = '"';
    }
    
    while (*src && dst < end - (needs_quotes ? 2 : 1)) {
        if (*src == '"' && needs_quotes) {
            *dst++ = '"';  // Escape quote with double quote
        }
        *dst++ = *src++;
    }
    
    if (needs_quotes && dst < end) {
        *dst++ = '"';
    }
    *dst = '\0';
}

typedef enum {
    OUTPUT_NORMAL,
    OUTPUT_CSV,
    OUTPUT_JSON
} OutputFormat;

static void print_metadata(OutputFormat output_format, const SongResource_Info *song_info,
                           int *first_field, int *has_any_info)
{
    BAEInfoType it;

    for (it = TITLE_INFO; it <= ORIGINAL_SOURCE_INFO; it = (BAEInfoType)(it + 1))
    {
        const char *value = song_info_value(song_info, it);
        if (!value || !value[0])
        {
            continue;
        }

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
            {
                printf(",\n");
            }
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
    {
        return;
    }

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

void print_usage(const char *program_name)
{
    printf("Usage: %s [-c|-j] <rmffile>\n", program_name);
    printf("  -c    Comma-separated values output (Section,Field,Value)\n");
    printf("  -j    JSON output with metadata and resource inventory\n");
    printf("  -h    Show this help\n");
}

int main(int argc, char *argv[])
{
    char *filename = NULL;
    OutputFormat output_format = OUTPUT_NORMAL;
    int i;
    RmfInspectionReport report;
    char inspection_error[256];
    
    // Parse command line arguments
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) {
            output_format = OUTPUT_CSV;
        } else if (strcmp(argv[i], "-j") == 0) {
            output_format = OUTPUT_JSON;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        } else {
            if (filename != NULL) {
                fprintf(stderr, "Multiple files specified. Only one file is supported.\n");
                print_usage(argv[0]);
                return 1;
            }
            filename = argv[i];
        }
    }
    
    if (filename == NULL) {
        fprintf(stderr, "No RMF file specified.\n");
        print_usage(argv[0]);
        return 1;
    }
    
    // Initialize BAE
    BAEResult err = BAE_Setup();
    if (err != BAE_NO_ERROR) {
        fprintf(stderr, "Error: Failed to initialize BAE audio engine (error %d)\n", err);
        return 1;
    }
    
    // Check if file exists and can be read
    FILE *test_file = fopen(filename, "rb");
    if (test_file == NULL) {
        fprintf(stderr, "Error: Cannot open file '%s'\n", filename);
        BAE_Cleanup();
        return 1;
    }
    fclose(test_file);
    
    // Validate that this is actually an RMF file
    if (!is_rmf_file(filename)) {
        fprintf(stderr, "Error: '%s' is not a valid RMF file (missing RMF magic header)\n", filename);
        BAE_Cleanup();
        return 1;
    }
    
    int first_field = 1;
    int has_any_info = 0;

    if (!inspect_rmf_file(filename, &report, inspection_error, sizeof(inspection_error))) {
        fprintf(stderr, "Error: %s\n", inspection_error);
        BAE_Cleanup();
        return 1;
    }
    
    // Output header based on format
    if (output_format == OUTPUT_CSV) {
        printf("Section,Field,Value\n");
    } else if (output_format == OUTPUT_JSON) {
        printf("{\n");
        printf("  \"file\": \"%s\",\n", filename);
        printf("  \"metadata\": {\n");
    } else {
        printf("RMF File Information: %s\n", filename);
        printf("===============================================\n");
        printf("Metadata:\n");
    }
    
    print_metadata(output_format, report.song_info, &first_field, &has_any_info);
    if (output_format == OUTPUT_JSON) {
        if (has_any_info) {
            printf("\n");
        }
        printf("  },\n");
    }

    print_song_section(output_format, &report);
    print_summary_section(output_format, &report);
    print_resources_section(output_format, &report);
    
    // Output footer based on format
    if (output_format == OUTPUT_JSON) {
        printf("}\n");
    } else if (output_format == OUTPUT_NORMAL && !has_any_info) {
        printf("  No RMF metadata found in file.\n");
    }
    
    // Cleanup
    free_report(&report);
    BAE_Cleanup();
    
    return 0;
}
