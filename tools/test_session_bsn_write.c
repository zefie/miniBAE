/* Smoke-test Session IREZ write helpers used by nbeditor Save Session.
 * Build (from repo root, after neobae is built):
 *   cc -o /tmp/test_session_bsn_write tools/test_session_bsn_write.c \
 *      -Ineobae/src/BAE_Source/Common -Lbuild-mt32-test -lneobae -lpthread -lm -ldl
 */
#include "NeoBAE.h"
#include "X_API.h"
#include "X_Formats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void make_pascal(const char *c, char out[256])
{
    size_t n = c ? strlen(c) : 0;
    if (n > 255)
        n = 255;
    out[0] = (char)n;
    if (n)
        memcpy(out + 1, c, n);
}

int main(int argc, char **argv)
{
    const char *bank_path = (argc > 1) ? argv[1] : "reference/bsn/empty.bsn";
    const char *midi_path = (argc > 2) ? argv[2] : NULL;
    const char *out_path = (argc > 3) ? argv[3] : "/tmp/neobae_session_roundtrip.bsn";
    BAEMixer mixer = NULL;
    BAEBankToken token = 0;
    XFILE outFile;
    XPTR packed = NULL;
    int32_t packedSize = 0;
    XFILENAME xf;
    unsigned char *midiData = NULL;
    long midiSize = 0;

    if (BAE_Setup() != 0)
    {
        fprintf(stderr, "BAE_Setup failed\n");
        return 1;
    }
    mixer = BAEMixer_New();
    if (!mixer || BAEMixer_Open(mixer, 0, NULL) != BAE_NO_ERROR)
    {
        fprintf(stderr, "mixer open failed\n");
        return 1;
    }
    if (BAEMixer_AddBankFromFile(mixer, (BAEPathName)bank_path, &token) != BAE_NO_ERROR || !token)
    {
        fprintf(stderr, "bank load failed: %s\n", bank_path);
        return 1;
    }

    outFile = XFileOpenVirtualResource(XFILERESOURCE_ID);
    if (!outFile)
    {
        fprintf(stderr, "virtual open failed\n");
        return 1;
    }

    /* Copy INST/BANK/VERS only for a light smoke (not full bank types). */
    {
        static const XResourceType types[] = {ID_INST, ID_BANK, ID_VERS, ID_ESND, ID_EMID, ID_SONG, 0};
        XFILE bankFile = (XFILE)token;
        int t;
        for (t = 0; types[t]; ++t)
        {
            int32_t count = XCountFileResourcesOfType(bankFile, types[t]);
            int32_t i;
            for (i = 0; i < count; ++i)
            {
                XLongResourceID id = 0;
                int32_t size = 0;
                char name[256];
                XPTR data;
                name[0] = 0;
                /* Skip groovoid SONG that would collide conceptually; copy all for smoke. */
                data = XGetIndexedFileResource(bankFile, types[t], &id, i, name, &size);
                if (!data)
                    continue;
                if (types[t] == ID_SONG && size >= 8)
                {
                    /* Skip if points at Midi (none in empty). */
                }
                (void)XAddFileResource(outFile, types[t], id, name, data, size);
                XDisposePtr(data);
            }
        }
    }

    if (midi_path)
    {
        FILE *fp = fopen(midi_path, "rb");
        if (!fp)
        {
            fprintf(stderr, "midi open failed: %s\n", midi_path);
            return 1;
        }
        fseek(fp, 0, SEEK_END);
        midiSize = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        midiData = (unsigned char *)malloc((size_t)midiSize);
        if (!midiData || fread(midiData, 1, (size_t)midiSize, fp) != (size_t)midiSize)
        {
            fprintf(stderr, "midi read failed\n");
            return 1;
        }
        fclose(fp);
    }
    else
    {
        /* Minimal SMF Type 0 */
        static const unsigned char kMiniMid[] = {
            0x4d, 0x54, 0x68, 0x64, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x01, 0x01, 0xe0,
            0x4d, 0x54, 0x72, 0x6b, 0x00, 0x00, 0x00, 0x04, 0x00, 0xff, 0x2f, 0x00};
        midiSize = (long)sizeof(kMiniMid);
        midiData = (unsigned char *)malloc((size_t)midiSize);
        memcpy(midiData, kMiniMid, (size_t)midiSize);
    }

    {
        char pname[256];
        SongResource_Info *info;
        SongResource *song;
        make_pascal("Smoke Song", pname);
        if (XAddFileResource(outFile, ID_MIDI, 73, pname, midiData, (int32_t)midiSize) != 0)
        {
            fprintf(stderr, "add Midi failed\n");
            return 1;
        }
        info = XNewSongResourceInfo();
        XClearSongResourceInfo(info);
        info->songType = SONG_TYPE_RMF;
        info->objectResourceID = 73;
        info->maxMidiNotes = 56;
        info->maxEffects = 4;
        info->mixLevel = 8;
        info->songVolume = 127;
        info->reverbType = 1;
        info->title = (char *)XNewPtr(11);
        memcpy(info->title, "Smoke Song", 11);
        song = XNewSongFromSongResourceInfo(info);
        XDisposeSongResourceInfo(info);
        if (!song ||
            XAddFileResource(outFile, ID_SONG, 0, pname, song, XGetPtrSize(song)) != 0)
        {
            fprintf(stderr, "add SONG failed\n");
            return 1;
        }
        XDisposeSongPtr(song);
    }

    {
        static const unsigned char bepf[] = {'e', 'x', 'a', 'm', 'p', 'l', 'e'};
        unsigned char prefs[34];
        char empty[256];
        char prefName[256];
        char dateName[256];
        unsigned char dateBody[36];
        empty[0] = 0;
        memset(prefs, 0, sizeof(prefs));
        prefs[1] = 2;
        make_pascal("Session Prefs", prefName);
        make_pascal("datestamp list", dateName);
        (void)XAddFileResource(outFile, FOUR_CHAR('B', 'E', 'P', 'F'), 1, empty, (void *)bepf, 7);
        (void)XAddFileResource(outFile, FOUR_CHAR('B', 'e', 'P', 'f'), 1, prefName, prefs, 34);
        memset(dateBody, 0, sizeof(dateBody));
        (void)XAddFileResource(outFile, FOUR_CHAR('D', 'A', 'T', 'e'), 1, dateName, dateBody, 36);
    }

    if (XCleanResourceFile(outFile) == FALSE ||
        XFileGetMemoryFileAsData(outFile, &packed, &packedSize) != 0 || !packed)
    {
        fprintf(stderr, "finalize failed\n");
        return 1;
    }
    XFileClose(outFile);

    XConvertPathToXFILENAME((void *)out_path, &xf);
    {
        XFILE disk = XFileOpenForWrite(&xf, TRUE);
        if (!disk || XFileWrite(disk, packed, packedSize) != 0)
        {
            fprintf(stderr, "write failed\n");
            return 1;
        }
        XFileClose(disk);
    }
    XDisposePtr(packed);
    free(midiData);
    printf("Wrote %s (%d bytes)\n", out_path, (int)packedSize);
    BAEMixer_Delete(mixer);
    BAE_Cleanup();
    return 0;
}
