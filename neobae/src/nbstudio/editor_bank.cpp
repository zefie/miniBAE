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
 * editor_bank.cpp
 *
 * Bank Editor Panel implementation for NeoBAE Studio.
 * Displays instruments organized by bank, samples per instrument,
 * and an embedded instrument editor with ADSR/LFO/LPF graphs.
 *
 ****************************************************************************/

#include <stdint.h>
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

#include <wx/listctrl.h>
#include <wx/notebook.h>
#include <wx/splitter.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/treectrl.h>
#include <wx/wx.h>

extern "C" {
#include "NeoBAE.h"
#include "X_API.h"
#include "X_Formats.h"
#include "X_Assert.h"
}

#include "editor_bank.h"
#include "editor_instrument_panels.h"

/* ------------------------------------------------------------------ */
/* Tree item data for instrument/sample nodes                         */
/* ------------------------------------------------------------------ */

class BankInstrumentItemData : public wxTreeItemData {
public:
    BankInstrumentItemData(uint32_t instrumentIndex, uint32_t instID)
        : m_instrumentIndex(instrumentIndex), m_instID(instID) {}
    uint32_t GetInstrumentIndex() const { return m_instrumentIndex; }
    uint32_t GetInstID() const { return m_instID; }
private:
    uint32_t m_instrumentIndex;
    uint32_t m_instID;
};

/* Tree item data for global sample nodes */
class BankSampleItemData : public wxTreeItemData {
public:
    BankSampleItemData(uint16_t sndID)
        : m_sndID(sndID) {}
    uint16_t GetSndID() const { return m_sndID; }
        BankSampleItemData(uint16_t sndID, const std::vector<std::pair<uint32_t, uint32_t>> &references)
            : m_sndID(sndID), m_references(references) {}
    void AddReference(uint32_t instrumentIndex, uint32_t sampleIndex) {
        m_references.push_back(std::make_pair(instrumentIndex, sampleIndex));
    }
    const std::vector<std::pair<uint32_t, uint32_t>> &GetReferences() const { return m_references; }
private:
    uint16_t m_sndID;
    std::vector<std::pair<uint32_t, uint32_t>> m_references;  /* (instrumentIndex, sampleIndex) pairs */
};

/* ------------------------------------------------------------------ */
/* Compression type name helper                                       */
/* ------------------------------------------------------------------ */

/* Return a human-readable sub-type bitrate suffix for Vorbis/Opus, or NULL if unknown */
static const char *CompressionSubTypeBitrate(uint32_t subType)
{
    switch (subType)
    {
        /* Vorbis */
        case FOUR_CHAR('v','0','3','2'):            return " 32k";
        case FOUR_CHAR('v','0','4','8'):            return " 48k";
        case FOUR_CHAR('v','0','6','4'):            return " 64k";
        case FOUR_CHAR('v','0','8','0'):            return " 80k";
        case FOUR_CHAR('v','0','9','6'):            return " 96k";
        case FOUR_CHAR('v','1','2','8'):            return " 128k";
        case FOUR_CHAR('v','1','6','0'):            return " 160k";
        case FOUR_CHAR('v','1','9','2'):            return " 192k";
        case FOUR_CHAR('v','2','5','6'):            return " 256k";
        /* Opus */
        case FOUR_CHAR('o','0','1','2'):            return " 12k";
        case FOUR_CHAR('o','0','1','6'):            return " 16k";
        case FOUR_CHAR('o','0','2','4'):            return " 24k";
        case FOUR_CHAR('o','0','3','2'):            return " 32k";
        case FOUR_CHAR('o','0','4','8'):            return " 48k";
        case FOUR_CHAR('o','0','6','4'):            return " 64k";
        case FOUR_CHAR('o','0','8','0'):            return " 80k";
        case FOUR_CHAR('o','0','9','6'):            return " 96k";
        case FOUR_CHAR('o','1','2','8'):            return " 128k";
        case FOUR_CHAR('o','1','6','0'):            return " 160k";
        case FOUR_CHAR('o','1','9','2'):            return " 192k";
        case FOUR_CHAR('o','2','5','6'):            return " 256k";
        default:                                    return NULL;
    }
}

static const char *CompressionTypeName(uint32_t ct, uint32_t subType,
                                       bool opusRoundTrip = false,
                                       uint16_t bitDepth = 0)
{
    static char combined[48];
    const char *base = NULL;
    const char *bitrate = NULL;

    switch (ct)
    {
        case 0:
        case FOUR_CHAR('n','o','n','e'):
            if (bitDepth == 8 || bitDepth == 16)
            {
                snprintf(combined, sizeof(combined), "PCM %u-bit", (unsigned)bitDepth);
                return combined;
            }
            return (ct == 0) ? "PCM (raw)" : "PCM";
        /* IMA ADPCM */
        case FOUR_CHAR('i','m','a','4'):            return "IMA4";
        case FOUR_CHAR('i','m','a','W'):            return "IMA4 (WAV)";
        case FOUR_CHAR('i','m','a','3'):            return "IMA3";
        /* MACE */
        case FOUR_CHAR('m','a','c','3'):            return "MACE3";
        case FOUR_CHAR('m','a','c','6'):            return "MACE6";
        /* uLaw / aLaw */
        case FOUR_CHAR('u','l','a','w'):            return "uLaw";
        case FOUR_CHAR('a','l','a','w'):            return "aLaw";
        /* FLAC */
        case FOUR_CHAR('f','L','a','C'):            return "FLAC";
        case FOUR_CHAR('F','L','A','C'):            return "FLAC";
        /* Vorbis - check sub-type for bitrate */
        case FOUR_CHAR('O','g','g','V'):            base = "Vorbis"; break;
        case FOUR_CHAR('V','O','R','B'):            base = "Vorbis"; break;
        /* Opus - check sub-type for bitrate */
        case FOUR_CHAR('O','g','g','O'):            base = opusRoundTrip ? "Opus RT" : "Opus"; break;
        case FOUR_CHAR('O','P','U','S'):            base = opusRoundTrip ? "Opus RT" : "Opus"; break;
        /* MPEG (MP3) - various bitrates */
        case FOUR_CHAR('m','p','g','n'):            return "MP3 32k";
        case FOUR_CHAR('m','p','g','a'):            return "MP3 40k";
        case FOUR_CHAR('m','p','g','b'):            return "MP3 48k";
        case FOUR_CHAR('m','p','g','c'):            return "MP3 56k";
        case FOUR_CHAR('m','p','g','d'):            return "MP3 64k";
        case FOUR_CHAR('m','p','g','e'):            return "MP3 80k";
        case FOUR_CHAR('m','p','g','f'):            return "MP3 96k";
        case FOUR_CHAR('m','p','g','g'):            return "MP3 112k";
        case FOUR_CHAR('m','p','g','h'):            return "MP3 128k";
        case FOUR_CHAR('m','p','g','i'):            return "MP3 160k";
        case FOUR_CHAR('m','p','g','j'):            return "MP3 192k";
        case FOUR_CHAR('m','p','g','k'):            return "MP3 224k";
        case FOUR_CHAR('m','p','g','l'):            return "MP3 256k";
        case FOUR_CHAR('m','p','g','m'):            return "MP3 320k";
        /* Legacy MPEG marker */
        case FOUR_CHAR('M','P','E','G'):            return "MPEG";
        default:
        {
            static char buf[32];
            if (ct > 0x20202020) {
                snprintf(buf, sizeof(buf), "'%c%c%c%c'",
                         (char)((ct >> 24) & 0xFF),
                         (char)((ct >> 16) & 0xFF),
                         (char)((ct >> 8) & 0xFF),
                         (char)(ct & 0xFF));
            } else {
                snprintf(buf, sizeof(buf), "0x%08X", ct);
            }
            return buf;
        }
    }

    /* If we reached here, base is set for Vorbis/Opus - append bitrate if available */
    bitrate = CompressionSubTypeBitrate(subType);
    if (bitrate)
    {
        snprintf(combined, sizeof(combined), "%s%s", base, bitrate);
        return combined;
    }
    return base;
}

static BAERmfEditorCompressionType EditorCompressionFromBankCodec(uint32_t compressionType,
                                                                  uint32_t compressionSubType)
{
    switch (compressionType)
    {
        case 0:
        case FOUR_CHAR('n','o','n','e'):
            return BAE_EDITOR_COMPRESSION_PCM;
        case FOUR_CHAR('i','m','a','4'):
        case FOUR_CHAR('i','m','a','W'):
        case FOUR_CHAR('i','m','a','3'):
            return BAE_EDITOR_COMPRESSION_ADPCM;
        case FOUR_CHAR('f','L','a','C'):
        case FOUR_CHAR('F','L','A','C'):
            return BAE_EDITOR_COMPRESSION_FLAC;
        case FOUR_CHAR('m','p','g','n'): return BAE_EDITOR_COMPRESSION_MP3_32K;
        case FOUR_CHAR('m','p','g','b'): return BAE_EDITOR_COMPRESSION_MP3_48K;
        case FOUR_CHAR('m','p','g','d'): return BAE_EDITOR_COMPRESSION_MP3_64K;
        case FOUR_CHAR('m','p','g','f'): return BAE_EDITOR_COMPRESSION_MP3_96K;
        case FOUR_CHAR('m','p','g','h'): return BAE_EDITOR_COMPRESSION_MP3_128K;
        case FOUR_CHAR('m','p','g','j'): return BAE_EDITOR_COMPRESSION_MP3_192K;
        case FOUR_CHAR('m','p','g','l'): return BAE_EDITOR_COMPRESSION_MP3_256K;
        case FOUR_CHAR('m','p','g','m'): return BAE_EDITOR_COMPRESSION_MP3_320K;
        case FOUR_CHAR('O','g','g','V'):
        case FOUR_CHAR('V','O','R','B'):
            switch (compressionSubType)
            {
                case FOUR_CHAR('v','0','3','2'): return BAE_EDITOR_COMPRESSION_VORBIS_32K;
                case FOUR_CHAR('v','0','4','8'): return BAE_EDITOR_COMPRESSION_VORBIS_48K;
                case FOUR_CHAR('v','0','6','4'): return BAE_EDITOR_COMPRESSION_VORBIS_64K;
                case FOUR_CHAR('v','0','8','0'): return BAE_EDITOR_COMPRESSION_VORBIS_80K;
                case FOUR_CHAR('v','0','9','6'): return BAE_EDITOR_COMPRESSION_VORBIS_96K;
                case FOUR_CHAR('v','1','2','8'): return BAE_EDITOR_COMPRESSION_VORBIS_128K;
                case FOUR_CHAR('v','1','6','0'): return BAE_EDITOR_COMPRESSION_VORBIS_160K;
                case FOUR_CHAR('v','1','9','2'): return BAE_EDITOR_COMPRESSION_VORBIS_192K;
                case FOUR_CHAR('v','2','5','6'): return BAE_EDITOR_COMPRESSION_VORBIS_256K;
                default:                         return BAE_EDITOR_COMPRESSION_VORBIS_128K;
            }
        case FOUR_CHAR('O','g','g','O'):
        case FOUR_CHAR('O','P','U','S'):
            switch (compressionSubType)
            {
                case FOUR_CHAR('o','0','1','2'): return BAE_EDITOR_COMPRESSION_OPUS_12K;
                case FOUR_CHAR('o','0','1','6'): return BAE_EDITOR_COMPRESSION_OPUS_16K;
                case FOUR_CHAR('o','0','2','4'): return BAE_EDITOR_COMPRESSION_OPUS_24K;
                case FOUR_CHAR('o','0','3','2'): return BAE_EDITOR_COMPRESSION_OPUS_32K;
                case FOUR_CHAR('o','0','4','8'): return BAE_EDITOR_COMPRESSION_OPUS_48K;
                case FOUR_CHAR('o','0','6','4'): return BAE_EDITOR_COMPRESSION_OPUS_64K;
                case FOUR_CHAR('o','0','8','0'): return BAE_EDITOR_COMPRESSION_OPUS_80K;
                case FOUR_CHAR('o','0','9','6'): return BAE_EDITOR_COMPRESSION_OPUS_96K;
                case FOUR_CHAR('o','1','2','8'): return BAE_EDITOR_COMPRESSION_OPUS_128K;
                case FOUR_CHAR('o','1','6','0'): return BAE_EDITOR_COMPRESSION_OPUS_160K;
                case FOUR_CHAR('o','1','9','2'): return BAE_EDITOR_COMPRESSION_OPUS_192K;
                case FOUR_CHAR('o','2','5','6'): return BAE_EDITOR_COMPRESSION_OPUS_256K;
                default:                         return BAE_EDITOR_COMPRESSION_OPUS_48K;
            }
        default:
            return BAE_EDITOR_COMPRESSION_PCM;
    }
}

/* ------------------------------------------------------------------ */
/* Bank group helpers                                                 */
/* ------------------------------------------------------------------ */

struct BankGroupKey {
    int sortOrder;
    bool isPercussion;
};

static BankGroupKey GetBankGroupKey(uint32_t instID)
{
    BankGroupKey key;
    if (instID < 128) {
        key.sortOrder = 0;
        key.isPercussion = false;
    } else if (instID < 256) {
        key.sortOrder = 1;
        key.isPercussion = true;
    } else if (instID < 384) {
        key.sortOrder = 2;
        key.isPercussion = false;
    } else if (instID < 512) {
        key.sortOrder = 3;
        key.isPercussion = true;
    } else {
        key.sortOrder = 4;
        key.isPercussion = false;
    }
    return key;
}

static wxString GetBankGroupName(uint32_t instID)
{
    if (instID < 128) {
        return "GM Melodic (Bank 0)";
    } else if (instID < 256) {
        return "GM Percussion (Bank 0)";
    } else if (instID < 384) {
        return "NeoBAE Special Melodic (Bank 1)";
    } else if (instID < 512) {
        return "NeoBAE Special Percussion (Bank 1)";
    } else {
        uint32_t bank = instID / 256;
        return wxString::Format("NeoBAE Extra (Bank %u)", static_cast<unsigned>(bank));
    }
}

/* ------------------------------------------------------------------ */
/* BankEditorPanel implementation                                     */
/* ------------------------------------------------------------------ */

struct BankEditorPanel {
    wxPanel *panel;
    wxSplitterWindow *splitter;
    
    /* Left side: tabbed interface with Instruments and Samples trees */
    wxNotebook *leftNotebook;
    
    /* Instruments tab (left/top) */
    wxPanel *instrumentsTabPanel;
    wxTreeCtrl *instrumentTree;
    wxListCtrl *sampleList;  /* Sample list within selected instrument */
    
    /* Samples tab (left/top) - global samples tree */
    wxPanel *samplesTabPanel;
    wxTreeCtrl *sampleTree;
    
    BAEBankToken bankToken;
    wxString bankPath;

    /* Right side: detail notebook with Instrument and Samples tabs */
    wxNotebook *detailNotebook;

    /* Instrument tab controls */
    wxPanel *instPage;
    wxStaticText *instHeaderLabel;
    InstrumentParamsPanel *instParamsPanel;

    /* Samples tab controls */
    wxPanel *samplesPage;
    wxStaticText *sampleHeaderLabel;
    SampleParamsPanel *sampleParamsPanel;
    PianoKeyboardPanel *pianoPanel;

    /* Preview state */
    std::function<void(unsigned char, unsigned char, int, int, bool)> playCallback;
    std::function<void(int)> stopCallback;
    std::function<void()> invalidateCallback;
    std::function<void(BAERmfEditorInstrumentExtInfo const *)> dirtyParamsCallback;
    std::function<bool(uint32_t, BAERmfEditorInstrumentExtInfo *)> pendingEditLookupCallback;
    std::function<bool(uint16_t, wxString &)> sourceCodecCallback;
    std::function<void(uint32_t)> cloneToSongCallback;
    std::function<void(uint32_t)> aliasToSongCallback;
    std::function<void(uint32_t, uint32_t)> deleteFromSongCallback;
    std::function<void(uint32_t)> compressInstrumentSamplesCallback;
    std::function<void(uint32_t instIDBase)> addInstrumentCallback;
    std::function<void(uint32_t instrumentIndex)> cloneInstrumentCallback;
    std::function<void(uint32_t instrumentIndex)> aliasInstrumentCallback;
    std::function<void(uint32_t, uint32_t)> addSampleCallback;
    std::function<void(uint32_t, uint32_t)> deleteSampleCallback;
    std::function<void(uint32_t, uint32_t)> aliasSampleToInstrumentCallback;
    std::function<void(uint32_t, uint32_t)> copySampleToInstrumentCallback;
    
    /* Global sample operations callbacks */
    std::function<void(uint16_t sndID)> globalAddSampleToInstrumentCallback;
    std::function<void(uint16_t sndID)> globalCopySampleToInstrumentCallback;
    std::function<void(uint16_t sndID, const std::vector<std::pair<uint32_t, uint32_t>> &)> globalDeleteSampleCallback;
    std::function<void(uint16_t sndID, const std::vector<std::pair<uint32_t, uint32_t>> &)> globalEditSampleCallback;
    bool mousePreviewActive;
    std::unordered_map<int, int> keyboardPreviewNotes;   /* keyCode -> MIDI note */

    /* Dirty instrument params (edits not yet saved to bank file) */
    BAERmfEditorInstrumentExtInfo dirtyExtInfo;
    bool hasDirtyExtInfo;
    bool hasPendingEdits;  /* true when user actually modified a control */

    /* Buttons visible across both tabs */
    wxButton *applyBtn;
    wxButton *stopAllBtn;

    /* Current sample selection for preview */
    uint32_t currentSampleIndex;
    BAERmfEditorBankSampleInfo currentSampleInfo;
    BAERmfEditorBankInstrumentInfo currentInstInfo;
    bool hasSampleSelection;
    void *cachedWaveformData;
    BAERmfEditorCompressionType lastUiCompressionType;
    BAERmfEditorSndStorageType lastUiStorageType;
    bool lastUiOpusRoundTrip;
    bool hasLastUiCodecState;

    /* State */
    uint32_t currentInstrumentIndex;
    BAERmfEditorInstrumentExtInfo currentExtInfo;
    bool hasInstrument;

    /* Per-sample applied codec cache: remembers codecs explicitly set via Apply
     * so navigating away and back shows the user's choice, not "Original". */
    struct AppliedCodecInfo {
        BAERmfEditorCompressionType compression;
        BAERmfEditorOpusMode opusMode;
        bool opusRoundTrip;
    };
    std::map<std::pair<uint32_t,uint32_t>, AppliedCodecInfo> appliedCodecs;

    struct CachedSndMetadata {
        uint32_t sampleRate = 0;
        uint32_t frameCount = 0;
        int16_t bitDepth = 0;
        int16_t channels = 0;
        uint32_t loopStart = 0;
        uint32_t loopEnd = 0;
        XResourceType compressionType = 0;
        uint32_t compressionSubType = 0;
        BAERmfEditorSndStorageType sndStorageType = BAE_EDITOR_SND_STORAGE_ESND;
        bool opusRoundTripResample = false;
    };
    std::unordered_map<uint16_t, CachedSndMetadata> sndMetadataCache;
};

static bool GetPendingInstrumentEdit(BankEditorPanel *bp,
                                     uint32_t instrumentIndex,
                                     BAERmfEditorInstrumentExtInfo *outInfo)
{
    if (!bp || !outInfo || !bp->pendingEditLookupCallback) {
        return false;
    }
    return bp->pendingEditLookupCallback(instrumentIndex, outInfo);
}

static void ApplyPendingSampleOverride(BAERmfEditorBankSampleInfo *sampleInfo,
                                       BAERmfEditorInstrumentExtInfo const *pendingInfo,
                                       uint32_t sampleIndex)
{
    if (!sampleInfo || !pendingInfo || pendingInfo->hasSampleOverride == FALSE ||
        pendingInfo->sampleOverrideIndex != sampleIndex) {
        return;
    }

    sampleInfo->rootKey = pendingInfo->sampleRootKey;
    sampleInfo->lowKey = pendingInfo->sampleLowKey;
    sampleInfo->highKey = pendingInfo->sampleHighKey;
    sampleInfo->splitVolume = pendingInfo->sampleSplitVolume;
    sampleInfo->sampleRate = pendingInfo->sampleRate;
    sampleInfo->loopStart = pendingInfo->sampleLoopStart;
    sampleInfo->loopEnd = pendingInfo->sampleLoopEnd;
}

static void CacheSndMetadata(BankEditorPanel *bp,
                             BAERmfEditorBankSampleInfo const &sampleInfo)
{
    if (!bp) {
        return;
    }

    BankEditorPanel::CachedSndMetadata metadata;
    metadata.sampleRate = sampleInfo.sampleRate;
    metadata.frameCount = sampleInfo.frameCount;
    metadata.bitDepth = sampleInfo.bitDepth;
    metadata.channels = sampleInfo.channels;
    metadata.loopStart = sampleInfo.loopStart;
    metadata.loopEnd = sampleInfo.loopEnd;
    metadata.compressionType = sampleInfo.compressionType;
    metadata.compressionSubType = sampleInfo.compressionSubType;
    metadata.sndStorageType = sampleInfo.sndStorageType;
    metadata.opusRoundTripResample = sampleInfo.opusRoundTripResample;
    bp->sndMetadataCache[sampleInfo.sndResourceID] = metadata;
}

static bool GetInstrumentSampleListInfoFast(BankEditorPanel *bp,
                                            uint32_t instrumentIndex,
                                            uint32_t sampleIndex,
                                            BAERmfEditorBankSampleInfo *outInfo)
{
    enum
    {
        kInstHeaderMinSize = 14
    };
    XFILE bankFile;
    XPTR instData;
    XLongResourceID instID;
    int32_t instSize;
    char rawName[256];
    InstrumentResource *inst;
    KeySplit split;
    int16_t splitCount;
    int16_t baseRootKey;
    int16_t baseVolume;
    bool useSoundModifierAsRootKey;
    int16_t miscParam1;

    if (!bp || !bp->bankToken || !outInfo) {
        return false;
    }
    XSetMemory(outInfo, (int32_t)sizeof(*outInfo), 0);

    bankFile = (XFILE)bp->bankToken;
    rawName[0] = 0;
    instData = XGetIndexedFileResource(bankFile, ID_INST, &instID,
                                       (int32_t)instrumentIndex, rawName, &instSize);
    if (!instData || instSize < kInstHeaderMinSize) {
        if (instData) {
            XDisposePtr(instData);
        }
        return false;
    }

    inst = (InstrumentResource *)instData;
    splitCount = (int16_t)XGetShort(&inst->keySplitCount);
    if (splitCount < 0) {
        splitCount = 0;
    }
    if ((splitCount > 0 && sampleIndex >= (uint32_t)splitCount) ||
        (splitCount == 0 && sampleIndex != 0)) {
        XDisposePtr(instData);
        return false;
    }

    outInfo->instID = (uint32_t)instID;
    outInfo->sampleIndex = sampleIndex;
    baseRootKey = (int16_t)XGetShort(&inst->midiRootKey);
    baseVolume = (int16_t)XGetShort(&inst->miscParameter2);
    useSoundModifierAsRootKey = TEST_FLAG_VALUE(inst->flags2, ZBF_useSoundModifierAsRootKey);
    miscParam1 = (int16_t)XGetShort(&inst->miscParameter1);

    if (splitCount > 0) {
        XGetKeySplitFromPtr(inst, (int16_t)sampleIndex, &split);
        outInfo->sndResourceID = split.sndResourceID;
        outInfo->lowKey = (unsigned char)split.lowMidi;
        outInfo->highKey = (unsigned char)split.highMidi;
        outInfo->splitVolume = split.miscParameter2;
        if (useSoundModifierAsRootKey) {
            int16_t splitRoot = split.miscParameter1;
            if (split.lowMidi == split.highMidi && splitRoot == 0) {
                splitRoot = (int16_t)split.lowMidi;
            }
            if (splitRoot < 0 || splitRoot > 127) {
                splitRoot = baseRootKey;
            }
            outInfo->rootKey = (unsigned char)splitRoot;
        } else {
            outInfo->rootKey = (unsigned char)baseRootKey;
        }
    } else {
        outInfo->sndResourceID = (XShortResourceID)XGetShort(&inst->sndResourceID);
        outInfo->lowKey = 0;
        outInfo->highKey = 127;
        outInfo->splitVolume = baseVolume;
        if (useSoundModifierAsRootKey) {
            if (miscParam1 > 0 && miscParam1 <= 127) {
                outInfo->rootKey = (unsigned char)miscParam1;
            } else {
                outInfo->rootKey = (unsigned char)baseRootKey;
            }
        } else {
            outInfo->rootKey = (unsigned char)baseRootKey;
        }
    }

    XDisposePtr(instData);

    auto cached = bp->sndMetadataCache.find(outInfo->sndResourceID);
    if (cached != bp->sndMetadataCache.end()) {
        outInfo->sampleRate = cached->second.sampleRate;
        outInfo->frameCount = cached->second.frameCount;
        outInfo->bitDepth = cached->second.bitDepth;
        outInfo->channels = cached->second.channels;
        outInfo->loopStart = cached->second.loopStart;
        outInfo->loopEnd = cached->second.loopEnd;
        outInfo->compressionType = cached->second.compressionType;
        outInfo->compressionSubType = cached->second.compressionSubType;
        outInfo->sndStorageType = cached->second.sndStorageType;
        outInfo->opusRoundTripResample = cached->second.opusRoundTripResample;
    }
    return true;
}

static const BAERmfEditorSndStorageType kBankOriginalStorageSentinel =
    (BAERmfEditorSndStorageType)0x7FFF;
static constexpr int kBankOpusRoundTripStorageFlag = 0x4000;

/* ------------------------------------------------------------------ */
/* Forward declarations                                               */
/* ------------------------------------------------------------------ */

static void PopulateInstrumentTree(BankEditorPanel *bp);
static void PopulateSampleTree(BankEditorPanel *bp);
static void PopulateSampleList(BankEditorPanel *bp, uint32_t instrumentIndex);
static void ShowInstrumentDetail(BankEditorPanel *bp, uint32_t instrumentIndex);
static void ShowSampleDetail(BankEditorPanel *bp,
                             uint32_t instrumentIndex,
                             uint32_t sampleIndex,
                             BAERmfEditorCompressionType preferredCompression,
                             BAERmfEditorOpusMode preferredOpusMode,
                             bool preferredOpusRoundTrip);
static void OnInstrumentSelected(BankEditorPanel *bp, wxTreeEvent &event);
static void OnSampleSelected(BankEditorPanel *bp, wxListEvent &event);
static void OnSampleItemRightClick(BankEditorPanel *bp, wxListEvent &event);
static void OnGlobalSampleSelected(BankEditorPanel *bp, wxTreeEvent &event);
static void OnInstrumentContextMenu(BankEditorPanel *bp, wxTreeEvent &event);
static void OnSampleContextMenu(BankEditorPanel *bp, wxContextMenuEvent &event);
static void OnGlobalSampleContextMenu(BankEditorPanel *bp, wxTreeEvent &event);
static void BuildInstrumentTab(BankEditorPanel *bp);
static void BuildSamplesTab(BankEditorPanel *bp);
static void InvalidateBankPreviewCache(BankEditorPanel *bp);
static void RefreshWaveform(BankEditorPanel *bp);
static void FreeCachedWaveform(BankEditorPanel *bp);

static bool CollectInstrumentSndRefsFast(BankEditorPanel *bp,
                                         uint32_t instrumentIndex,
                                         std::vector<uint16_t> *outSndIds)
{
    enum
    {
        kInstHeaderMinSize = 14,
        kInstKeySplitSize = 8
    };
    XLongResourceID instID = 0;
    int32_t instSize = 0;
    char instName[256] = {0};
    XPTR instData;
    int16_t splitCount;

    if (!bp || !bp->bankToken || !outSndIds) {
        return false;
    }
    outSndIds->clear();

    instData = XGetIndexedFileResource((XFILE)bp->bankToken,
                                       ID_INST,
                                       &instID,
                                       static_cast<int32_t>(instrumentIndex),
                                       instName,
                                       &instSize);
    if (!instData || instSize < kInstHeaderMinSize) {
        if (instData) {
            XDisposePtr(instData);
        }
        return false;
    }

    splitCount = static_cast<int16_t>(XGetShort(static_cast<unsigned char *>(instData) + 12));
    if (splitCount < 0) {
        splitCount = 0;
    }

    if (splitCount == 0) {
        uint16_t sndID = static_cast<uint16_t>(XGetShort(static_cast<unsigned char *>(instData) + 0));
        outSndIds->push_back(sndID);
        XDisposePtr(instData);
        return true;
    }

    {
        size_t splitBytes = static_cast<size_t>(splitCount) * static_cast<size_t>(kInstKeySplitSize);
        size_t requiredSize = static_cast<size_t>(kInstHeaderMinSize) + splitBytes;
        if (requiredSize > static_cast<size_t>(instSize)) {
            XDisposePtr(instData);
            return false;
        }
    }

    outSndIds->reserve(static_cast<size_t>(splitCount));
    for (int32_t splitIndex = 0; splitIndex < splitCount; ++splitIndex) {
        unsigned char *splitPtr = static_cast<unsigned char *>(instData) +
                                  kInstHeaderMinSize +
                                  (splitIndex * kInstKeySplitSize);
        uint16_t sndID = static_cast<uint16_t>(XGetShort(splitPtr + 2));
        outSndIds->push_back(sndID);
    }

    XDisposePtr(instData);
    return true;
}
static void StopBankKeyboardPreview(BankEditorPanel *bp);
static void ApplyDirtyParams(BankEditorPanel *bp);
static void StopAllBankNotes(BankEditorPanel *bp);
static void RefreshSampleListRow(BankEditorPanel *bp, uint32_t instrumentIndex, uint32_t sampleIndex);

static wxTreeItemId FindInstrumentTreeItemByInstID(BankEditorPanel *bp, uint32_t instID);
static wxTreeItemId FindInstrumentTreeItemByIndex(BankEditorPanel *bp, uint32_t instrumentIndex);
static wxTreeItemId FindSampleTreeItemBySndID(BankEditorPanel *bp, uint16_t sndID);

/* ------------------------------------------------------------------ */
/* Preview cache invalidation                                         */
/* ------------------------------------------------------------------ */

static void InvalidateBankPreviewCache(BankEditorPanel *bp)
{
    StopBankKeyboardPreview(bp);
    if (bp->invalidateCallback) {
        bp->invalidateCallback();
    }
}

/* Read current InstrumentParamsPanel state into dirty overlay and
 * notify the host so subsequent preview notes hear the edits. */
static void ApplyDirtyParams(BankEditorPanel *bp)
{
    if (!bp->hasInstrument || !bp->instParamsPanel) return;

    bp->dirtyExtInfo = bp->currentExtInfo;
    bp->instParamsPanel->SaveToExtInfo(bp->dirtyExtInfo);
    bp->hasDirtyExtInfo = true;

    /* Collect sample-level overrides when a sample is selected */
    if (bp->hasSampleSelection && bp->sampleParamsPanel) {
        SampleParamsPanelData sData;
        bp->sampleParamsPanel->SaveToData(sData);
        bp->lastUiCompressionType = sData.compressionType;
        bp->lastUiStorageType = sData.sndStorageType;
        bp->lastUiOpusRoundTrip = sData.opusRoundTripResample;
        bp->hasLastUiCodecState = true;

        /* Stage codec intent for later save/export. Preview paths consume this
         * intent by generating a minimal preview song on demand. */
        if (bp->sampleParamsPanel && bp->sampleParamsPanel->GetCodecSelection() == 0) {
            auto it = bp->appliedCodecs.find({bp->currentInstrumentIndex, bp->currentSampleIndex});
            bool wasChanged = (it != bp->appliedCodecs.end());
            
            bp->dirtyExtInfo.sampleTargetCompression = BAE_EDITOR_COMPRESSION_DONT_CHANGE;
            if (wasChanged) {
                /* User is reverting a previous explicit codec change this session. */
                bp->dirtyExtInfo.sampleTargetStorageType = kBankOriginalStorageSentinel;
            } else {
                /* It's already the original. Don't trigger a wasteful re-encode! */
                bp->dirtyExtInfo.sampleTargetStorageType = (BAERmfEditorSndStorageType)sData.sndStorageType;
            }
            
            /* User chose "Original" - forget any previously applied codec */
            if (wasChanged) {
                bp->appliedCodecs.erase(it);
            }
        } else {
            int storageWithFlags = (int)sData.sndStorageType;
            if (sData.opusRoundTripResample) {
                storageWithFlags |= kBankOpusRoundTripStorageFlag;
            }
            bp->dirtyExtInfo.sampleTargetCompression = sData.compressionType;
            bp->dirtyExtInfo.sampleTargetStorageType  = (BAERmfEditorSndStorageType)storageWithFlags;
        }
        bp->dirtyExtInfo.sampleTargetOpusMode     = sData.opusMode;

        bp->dirtyExtInfo.hasSampleOverride = TRUE;
        bp->dirtyExtInfo.sampleOverrideIndex = bp->currentSampleIndex;
        bp->dirtyExtInfo.sampleRootKey = sData.rootKey;
        bp->dirtyExtInfo.sampleLowKey = sData.lowKey;
        bp->dirtyExtInfo.sampleHighKey = sData.highKey;
        bp->dirtyExtInfo.sampleSplitVolume = sData.splitVolume;
        bp->dirtyExtInfo.sampleRate = sData.sampleRate;
        bp->dirtyExtInfo.sampleLoopStart = sData.loopStart;
        bp->dirtyExtInfo.sampleLoopEnd = sData.loopEnd;
        if (bp->sampleParamsPanel && bp->sampleParamsPanel->GetCodecSelection() != 0) {
            /* Remember this codec so navigating away and back preserves it */
            bp->appliedCodecs[{bp->currentInstrumentIndex, bp->currentSampleIndex}] = {
                sData.compressionType, sData.opusMode, sData.opusRoundTripResample
            };
        }
        BAE_PRINTF( "[bank] ApplyDirtyParams: sampleOverride idx=%u rootKey=%u rate=%u loop=%u-%u\n",
                bp->currentSampleIndex, (unsigned)sData.rootKey, sData.sampleRate,
                sData.loopStart, sData.loopEnd);
    } else {
        bp->dirtyExtInfo.hasSampleOverride = FALSE;
        bp->dirtyExtInfo.sampleTargetCompression = BAE_EDITOR_COMPRESSION_DONT_CHANGE;
        bp->hasLastUiCodecState = false;
    }

    if (bp->dirtyParamsCallback) {
        bp->dirtyParamsCallback(&bp->dirtyExtInfo);
    }

    /* Refresh the sample list row so columns immediately reflect the staged
     * values even though the bank token itself is not rewritten yet. */
    if (bp->hasSampleSelection) {
        RefreshSampleListRow(bp, bp->currentInstrumentIndex, bp->currentSampleIndex);
    }

    /* Tear down preview state so the next note picks up staged edits. */
    if (bp->invalidateCallback) {
        bp->invalidateCallback();
    }
}

/* Stop all currently-playing preview notes (keyboard + mouse).
 * Uses the invalidate callback which tears down the entire preview
 * song, forcefully killing voices even if the ADSR has a long release. */
static void StopAllBankNotes(BankEditorPanel *bp)
{
    StopBankKeyboardPreview(bp);
    if (bp->invalidateCallback) {
        bp->invalidateCallback();
    }
}

/* ------------------------------------------------------------------ */
/* Keyboard preview helpers                                           */
/* ------------------------------------------------------------------ */

static constexpr int kBankMousePreviewTag = -1000001;

static int NormalizeBankKeyCode(int keyCode)
{
    if (keyCode >= 'a' && keyCode <= 'z') {
        return keyCode - ('a' - 'A');
    }
    return keyCode;
}

static int BankKeyCodeToSemitoneOffset(int keyCode)
{
    switch (NormalizeBankKeyCode(keyCode)) {
        case 'A': return 0;
        case 'W': return 1;
        case 'S': return 2;
        case 'E': return 3;
        case 'D': return 4;
        case 'F': return 5;
        case 'T': return 6;
        case 'G': return 7;
        case 'Y': return 8;
        case 'H': return 9;
        case 'U': return 10;
        case 'J': return 11;
        case 'K': return 12;
        case 'O': return 13;
        default: return -1;
    }
}

static bool IsBankTextEditingFocus(wxWindow *focus)
{
    return wxDynamicCast(focus, wxTextCtrl) != nullptr ||
           wxDynamicCast(focus, wxSpinCtrl) != nullptr ||
           wxDynamicCast(focus, wxSpinCtrlDouble) != nullptr;
}

/* ------------------------------------------------------------------ */
/* Instrument tree population                                         */
/* ------------------------------------------------------------------ */

static wxTreeItemId FindInstrumentTreeItemByInstID(BankEditorPanel *bp, uint32_t instID)
{
    wxTreeItemId root;
    wxTreeItemIdValue cookie;
    wxTreeItemId group;

    if (!bp || !bp->instrumentTree) {
        return wxTreeItemId();
    }

    root = bp->instrumentTree->GetRootItem();
    if (!root.IsOk()) {
        return wxTreeItemId();
    }

    group = (bp->instrumentTree->GetFirstChild)(root, cookie);
    while (group.IsOk()) {
        wxTreeItemIdValue itemCookie;
        wxTreeItemId item = (bp->instrumentTree->GetFirstChild)(group, itemCookie);
        while (item.IsOk()) {
            BankInstrumentItemData *data = dynamic_cast<BankInstrumentItemData *>(
                bp->instrumentTree->GetItemData(item));
            if (data && data->GetInstID() == instID) {
                return item;
            }
            item = bp->instrumentTree->GetNextChild(group, itemCookie);
        }
        group = bp->instrumentTree->GetNextChild(root, cookie);
    }

    return wxTreeItemId();
}

static wxTreeItemId FindInstrumentTreeItemByIndex(BankEditorPanel *bp, uint32_t instrumentIndex)
{
    wxTreeItemId root;
    wxTreeItemIdValue cookie;
    wxTreeItemId group;

    if (!bp || !bp->instrumentTree) {
        return wxTreeItemId();
    }

    root = bp->instrumentTree->GetRootItem();
    if (!root.IsOk()) {
        return wxTreeItemId();
    }

    group = (bp->instrumentTree->GetFirstChild)(root, cookie);
    while (group.IsOk()) {
        wxTreeItemIdValue itemCookie;
        wxTreeItemId item = (bp->instrumentTree->GetFirstChild)(group, itemCookie);
        while (item.IsOk()) {
            BankInstrumentItemData *data = dynamic_cast<BankInstrumentItemData *>(
                bp->instrumentTree->GetItemData(item));
            if (data && data->GetInstrumentIndex() == instrumentIndex) {
                return item;
            }
            item = bp->instrumentTree->GetNextChild(group, itemCookie);
        }
        group = bp->instrumentTree->GetNextChild(root, cookie);
    }

    return wxTreeItemId();
}

static wxTreeItemId FindSampleTreeItemBySndID(BankEditorPanel *bp, uint16_t sndID)
{
    wxTreeItemId root;
    wxTreeItemIdValue cookie;
    wxTreeItemId item;

    if (!bp || !bp->sampleTree) {
        return wxTreeItemId();
    }

    root = bp->sampleTree->GetRootItem();
    if (!root.IsOk()) {
        return wxTreeItemId();
    }

    item = (bp->sampleTree->GetFirstChild)(root, cookie);
    while (item.IsOk()) {
        BankSampleItemData *data = dynamic_cast<BankSampleItemData *>(
            bp->sampleTree->GetItemData(item));
        if (data && data->GetSndID() == sndID) {
            return item;
        }
        item = bp->sampleTree->GetNextChild(root, cookie);
    }

    return wxTreeItemId();
}

static void PopulateInstrumentTree(BankEditorPanel *bp)
{
    uint32_t instCount = 0;
    wxTreeItemId root;
    bool hadSelectedInstrument = false;
    uint32_t selectedInstID = 0;
    uint32_t selectedInstrumentIndex = 0;

    {
        wxTreeItemId selected = bp->instrumentTree->GetSelection();
        if (selected.IsOk()) {
            BankInstrumentItemData *selectedData = dynamic_cast<BankInstrumentItemData *>(
                bp->instrumentTree->GetItemData(selected));
            if (selectedData) {
                hadSelectedInstrument = true;
                selectedInstID = selectedData->GetInstID();
                selectedInstrumentIndex = selectedData->GetInstrumentIndex();
            }
        }
    }

    bp->instrumentTree->DeleteAllItems();
    root = bp->instrumentTree->AddRoot("Instruments");

    if (!bp->bankToken) {
        BAE_PRINTF( "[BankEditor] PopulateInstrumentTree: no bank token\n");
        bp->instrumentTree->Expand(root);
        return;
    }

    if (BAERmfEditorBank_GetInstrumentCount(bp->bankToken, &instCount) != BAE_NO_ERROR) {
        BAE_PRINTF( "[BankEditor] PopulateInstrumentTree: GetInstrumentCount failed\n");
        bp->instrumentTree->Expand(root);
        return;
    }
    BAE_PRINTF( "[BankEditor] PopulateInstrumentTree: %u instruments\n", instCount);

    struct BankGroup {
        int sortOrder;
        wxTreeItemId node;
    };
    std::vector<BankGroup> groups;

    auto findOrCreateGroup = [&](uint32_t instID) -> wxTreeItemId {
        BankGroupKey key = GetBankGroupKey(instID);
        for (auto &g : groups) {
            if (g.sortOrder == key.sortOrder) {
                return g.node;
            }
        }
        wxString groupName = GetBankGroupName(instID);
        wxTreeItemId node = bp->instrumentTree->AppendItem(root, groupName);
        groups.push_back({key.sortOrder, node});
        return node;
    };

    struct InstrumentEntry {
        uint32_t instrumentIndex;
        uint32_t displayInstID;
        BAERmfEditorBankInstrumentInfo info;
        bool isPercussion;
        bool isAlias;
    };
    std::vector<InstrumentEntry> entries;

    for (uint32_t i = 0; i < instCount; ++i) {
        InstrumentEntry entry;
        entry.instrumentIndex = i;
        entry.isAlias = false;
        if (BAERmfEditorBank_GetInstrumentInfo(bp->bankToken, i, &entry.info) != BAE_NO_ERROR) {
            continue;
        }
        entry.displayInstID = entry.info.instID;
        entry.isPercussion = GetBankGroupKey(entry.info.instID).isPercussion;
        entries.push_back(entry);
    }

    /* Add aliased instruments */
    {
        XFILE bankFile = (XFILE)bp->bankToken;
        XAliasLinkResource *pAlias = XGetAliasLinkFromFile(bankFile);
        if (pAlias) {
            uint32_t aliasCount = (uint32_t)XGetLong(&pAlias->numberOfAliases);
            for (uint32_t a = 0; a < aliasCount; ++a) {
                XLongResourceID fromID = (XLongResourceID)XGetLong(&pAlias->list[a].aliasFrom);
                XLongResourceID toID = (XLongResourceID)XGetLong(&pAlias->list[a].aliasTo);

                for (auto const &e : entries) {
                    if (!e.isAlias && e.info.instID == (uint32_t)toID) {
                        InstrumentEntry aliasEntry;
                        aliasEntry.instrumentIndex = e.instrumentIndex;
                        aliasEntry.info = e.info;
                        aliasEntry.displayInstID = (uint32_t)fromID;
                        aliasEntry.info.instID = (uint32_t)fromID;
                        aliasEntry.info.bank = (uint16_t)(fromID / 256);
                        aliasEntry.info.program = (unsigned char)(fromID % 128);
                        aliasEntry.isPercussion = GetBankGroupKey((uint32_t)fromID).isPercussion;
                        aliasEntry.isAlias = true;
                        entries.push_back(aliasEntry);
                        break;
                    }
                }
            }
            XDisposePtr((XPTR)pAlias);
        }
    }

    std::sort(entries.begin(), entries.end(), [](InstrumentEntry const &a, InstrumentEntry const &b) {
        BankGroupKey ka = GetBankGroupKey(a.displayInstID);
        BankGroupKey kb = GetBankGroupKey(b.displayInstID);
        if (ka.sortOrder != kb.sortOrder) return ka.sortOrder < kb.sortOrder;
        return a.info.program < b.info.program;
    });

    for (auto const &entry : entries) {
        wxTreeItemId groupNode = findOrCreateGroup(entry.displayInstID);

        wxString label;
        wxString name = entry.info.name[0] ? wxString(entry.info.name) : wxString("(unnamed)");
        name.Replace("\r", "");
        name.Replace("\n", " ");
        name.Trim();
        if (entry.isAlias) {
            name += " (Alias)";
        }
        if (entry.isPercussion) {
            label = wxString::Format("%u: %s (Note %u)",
                                     static_cast<unsigned>(entry.info.program),
                                     name,
                                     static_cast<unsigned>(entry.info.program));
        } else {
            label = wxString::Format("%u: %s",
                                     static_cast<unsigned>(entry.info.program),
                                     name);
        }

        bp->instrumentTree->AppendItem(groupNode, label, -1, -1,
                                       new BankInstrumentItemData(entry.instrumentIndex, entry.displayInstID));
    }

    bp->instrumentTree->Expand(root);
    if (groups.size() <= 4) {
        for (auto &g : groups) {
            bp->instrumentTree->Expand(g.node);
        }
    }
    /* Scroll back to top - on Windows, Expand() can auto-scroll to the
     * last expanded node, leaving the view near the bottom of the tree. */
    bp->instrumentTree->EnsureVisible(root);

    if (hadSelectedInstrument) {
        wxTreeItemId restoreItem = FindInstrumentTreeItemByInstID(bp, selectedInstID);
        if (!restoreItem.IsOk()) {
            restoreItem = FindInstrumentTreeItemByIndex(bp, selectedInstrumentIndex);
        }
        if (restoreItem.IsOk()) {
            bp->instrumentTree->SelectItem(restoreItem);
            bp->instrumentTree->EnsureVisible(restoreItem);
            {
                BankInstrumentItemData *restoreData = dynamic_cast<BankInstrumentItemData *>(
                    bp->instrumentTree->GetItemData(restoreItem));
                if (restoreData) {
                    PopulateSampleList(bp, restoreData->GetInstrumentIndex());
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Global sample tree population                                      */
/* ------------------------------------------------------------------ */

static void PopulateSampleTree(BankEditorPanel *bp)
{
    /* Build a map of SNDID -> list of (instrumentIndex, sampleIndex) references */
    std::unordered_map<uint16_t, std::vector<std::pair<uint32_t, uint32_t>>> sampleMap;
    uint32_t instCount = 0;
    bool hadSelectedSample = false;
    uint16_t selectedSndID = 0;

    {
        wxTreeItemId selected = bp->sampleTree->GetSelection();
        if (selected.IsOk()) {
            BankSampleItemData *selectedData = dynamic_cast<BankSampleItemData *>(
                bp->sampleTree->GetItemData(selected));
            if (selectedData) {
                hadSelectedSample = true;
                selectedSndID = selectedData->GetSndID();
            }
        }
    }

    bp->sampleTree->DeleteAllItems();
    wxTreeItemId root = bp->sampleTree->AddRoot("Samples");

    if (!bp->bankToken) {
        bp->sampleTree->Expand(root);
        return;
    }

    if (BAERmfEditorBank_GetInstrumentCount(bp->bankToken, &instCount) != BAE_NO_ERROR) {
        bp->sampleTree->Expand(root);
        return;
    }

    /* Collect all unique samples and their referencing instruments */
    for (uint32_t i = 0; i < instCount; ++i) {
        std::vector<uint16_t> sndRefs;

        if (!CollectInstrumentSndRefsFast(bp, i, &sndRefs)) {
            continue;
        }
        for (uint32_t s = 0; s < sndRefs.size(); ++s) {
            sampleMap[sndRefs[s]].push_back(std::make_pair(i, s));
        }
    }

    /* Build list of samples with their names for sorting */
    struct SampleEntry {
        uint16_t sndID;
        wxString displayName;
        std::vector<std::pair<uint32_t, uint32_t>> references;
    };
    std::vector<SampleEntry> entries;

    for (auto &mapEntry : sampleMap) {
        SampleEntry entry;
        entry.sndID = mapEntry.first;
        entry.references = mapEntry.second;

        /* Get sample name from resource */
        char sampleName[256] = {0};
        XGetFileResourceName((XFILE)bp->bankToken, ID_SND, (XLongResourceID)entry.sndID, sampleName);
        if (sampleName[0] == 0) {
            XGetFileResourceName((XFILE)bp->bankToken, ID_CSND, (XLongResourceID)entry.sndID, sampleName);
        }
        if (sampleName[0] == 0) {
            XGetFileResourceName((XFILE)bp->bankToken, ID_ESND, (XLongResourceID)entry.sndID, sampleName);
        }
        
        wxString name = sampleName[0] ? wxString(sampleName) : wxString();
        name.Replace("\r", "");
        name.Replace("\n", " ");
        name.Trim();
        
        /* Check if name is generic or invalid */
        if (!name.IsEmpty() && name != "(null)" && name != "unknown" && name != "sample") {
            entry.displayName = name;
        } else {
            /* Fall back to SNDID */
            entry.displayName = wxString::Format("Sample 0x%04X", static_cast<unsigned>(entry.sndID));
        }
        entries.push_back(entry);
    }

    /* Sort by display name (which includes fallback to SNDID) */
    std::sort(entries.begin(), entries.end(), [](const SampleEntry &a, const SampleEntry &b) {
        return a.displayName < b.displayName;
    });

    /* Add to tree */
    for (auto const &entry : entries) {
        wxString label = wxString::Format("%s (sndID: %u) - %d refs", 
                                          entry.displayName,
                                          static_cast<unsigned>(entry.sndID),
                                          static_cast<int>(entry.references.size()));
        bp->sampleTree->AppendItem(root, label, -1, -1,
                                   new BankSampleItemData(entry.sndID, entry.references));
    }

    bp->sampleTree->Expand(root);
    bp->sampleTree->EnsureVisible(root);

    if (hadSelectedSample) {
        wxTreeItemId restoreItem = FindSampleTreeItemBySndID(bp, selectedSndID);
        if (restoreItem.IsOk()) {
            bp->sampleTree->SelectItem(restoreItem);
            bp->sampleTree->EnsureVisible(restoreItem);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Sample list population                                             */
/* ------------------------------------------------------------------ */

static void PopulateSampleList(BankEditorPanel *bp, uint32_t instrumentIndex)
{
    uint32_t sampleCount = 0;
    BAERmfEditorInstrumentExtInfo pendingExtInfo;
    bool hasPendingEdit = false;

    bp->sampleList->DeleteAllItems();

    if (!bp->bankToken) {
        return;
    }

    if (BAERmfEditorBank_GetInstrumentSampleCount(bp->bankToken, instrumentIndex, &sampleCount) != BAE_NO_ERROR) {
        return;
    }

    hasPendingEdit = GetPendingInstrumentEdit(bp, instrumentIndex, &pendingExtInfo);

    for (uint32_t s = 0; s < sampleCount; ++s) {
        BAERmfEditorBankSampleInfo sampleInfo;
        if (!GetInstrumentSampleListInfoFast(bp, instrumentIndex, s, &sampleInfo)) {
            continue;
        }
        if (hasPendingEdit) {
            ApplyPendingSampleOverride(&sampleInfo, &pendingExtInfo, s);
        }

        wxString sndLabel = wxString::Format("SND %d", static_cast<int>(sampleInfo.sndResourceID));
        wxString keyRange = wxString::Format("%u-%u", sampleInfo.lowKey, sampleInfo.highKey);
        wxString rootKey = (sampleInfo.rootKey == 0)
            ? wxString("60 (default)")
            : wxString::Format("%u", sampleInfo.rootKey);
        wxString rate = sampleInfo.sampleRate ? wxString::Format("%u Hz", sampleInfo.sampleRate) : wxString("-");
        wxString frames = sampleInfo.frameCount ? wxString::Format("%u", sampleInfo.frameCount) : wxString("-");
        wxString codec = sampleInfo.compressionType ?
            wxString::FromUTF8(CompressionTypeName((uint32_t)sampleInfo.compressionType,
                                                   sampleInfo.compressionSubType,
                                                   sampleInfo.opusRoundTripResample ? true : false,
                                                   sampleInfo.bitDepth)) :
            wxString("-");
        wxString bits = sampleInfo.bitDepth ?
            wxString::Format("%d-bit %s",
                             sampleInfo.bitDepth,
                             sampleInfo.channels == 2 ? "stereo" : "mono") :
            wxString("-");

        long idx = bp->sampleList->InsertItem(bp->sampleList->GetItemCount(), sndLabel);
        bp->sampleList->SetItem(idx, 1, keyRange);
        bp->sampleList->SetItem(idx, 2, rootKey);
        bp->sampleList->SetItem(idx, 3, rate);
        bp->sampleList->SetItem(idx, 4, bits);
        bp->sampleList->SetItem(idx, 5, codec);
        bp->sampleList->SetItem(idx, 6, frames);
    }
}

/* Refresh the columns of a single row in the sample list after edits. */
static void RefreshSampleListRow(BankEditorPanel *bp, uint32_t instrumentIndex, uint32_t sampleIndex)
{
    BAERmfEditorInstrumentExtInfo pendingExtInfo;
    bool hasPendingEdit = false;

    if (!bp->bankToken || !bp->sampleList) return;

    BAERmfEditorBankSampleInfo sampleInfo;
    if (!GetInstrumentSampleListInfoFast(bp, instrumentIndex, sampleIndex, &sampleInfo)) {
        return;
    }

    hasPendingEdit = GetPendingInstrumentEdit(bp, instrumentIndex, &pendingExtInfo);
    if (hasPendingEdit) {
        ApplyPendingSampleOverride(&sampleInfo, &pendingExtInfo, sampleIndex);
    }

    long row = static_cast<long>(sampleIndex);
    if (row >= bp->sampleList->GetItemCount()) return;

    wxString keyRange = wxString::Format("%u-%u", sampleInfo.lowKey, sampleInfo.highKey);
    wxString rootKey = (sampleInfo.rootKey == 0)
        ? wxString("60 (default)")
        : wxString::Format("%u", sampleInfo.rootKey);
    wxString rate = sampleInfo.sampleRate ? wxString::Format("%u Hz", sampleInfo.sampleRate) : wxString("-");
    wxString bits = sampleInfo.bitDepth ?
        wxString::Format("%d-bit %s",
                         sampleInfo.bitDepth,
                         sampleInfo.channels == 2 ? "stereo" : "mono") :
        wxString("-");
    wxString codec = sampleInfo.compressionType ?
        wxString::FromUTF8(CompressionTypeName((uint32_t)sampleInfo.compressionType,
                                               sampleInfo.compressionSubType,
                                               sampleInfo.opusRoundTripResample ? true : false,
                                               sampleInfo.bitDepth)) :
        wxString("-");
    wxString frames = sampleInfo.frameCount ? wxString::Format("%u", sampleInfo.frameCount) : wxString("-");

    bp->sampleList->SetItem(row, 1, keyRange);
    bp->sampleList->SetItem(row, 2, rootKey);
    bp->sampleList->SetItem(row, 3, rate);
    bp->sampleList->SetItem(row, 4, bits);
    bp->sampleList->SetItem(row, 5, codec);
    bp->sampleList->SetItem(row, 6, frames);
}

/* ------------------------------------------------------------------ */
/* Show instrument detail in the Instrument tab                       */
/* ------------------------------------------------------------------ */

static void ShowInstrumentDetail(BankEditorPanel *bp, uint32_t instrumentIndex)
{
    BAERmfEditorInstrumentExtInfo extInfo;
    BAERmfEditorBankInstrumentInfo instInfo;
    BAERmfEditorInstrumentExtInfo pendingExtInfo;

    if (!bp->bankToken) {
        bp->instHeaderLabel->SetLabel("No bank loaded.");
        bp->hasInstrument = false;
        return;
    }

    if (BAERmfEditorBank_GetInstrumentInfo(bp->bankToken, instrumentIndex, &instInfo) != BAE_NO_ERROR) {
        bp->instHeaderLabel->SetLabel("Failed to read instrument info.");
        bp->hasInstrument = false;
        return;
    }

    bp->currentInstrumentIndex = instrumentIndex;
    bp->hasInstrument = true;
    bp->currentInstInfo = instInfo;
    bp->hasSampleSelection = false;
    bp->hasLastUiCodecState = false;
    bp->hasPendingEdits = false;

    /* Clear dirty params from previous instrument */
    bp->hasDirtyExtInfo = false;
    if (bp->dirtyParamsCallback) {
        bp->dirtyParamsCallback(nullptr);
    }

    /* Clear sample panel - no sample is selected for the new instrument yet */
    FreeCachedWaveform(bp);
    bp->sampleHeaderLabel->SetLabel("Select a sample from the list to view details.");
    if (bp->sampleParamsPanel) {
        bp->sampleParamsPanel->ClearUI();
        bp->sampleParamsPanel->Hide();
    }

    {
        wxString headerText = wxString::Format(
            "INST ID: %u  Bank: %u  Program: %u  Key Splits: %d",
            instInfo.instID, instInfo.bank, instInfo.program, instInfo.keySplitCount);
        bp->instHeaderLabel->SetLabel(headerText);
    }

    /* Get extended info */
    memset(&extInfo, 0, sizeof(extInfo));
    if (BAERmfEditorBank_GetInstrumentExtInfo(bp->bankToken, instrumentIndex, &extInfo) == BAE_NO_ERROR) {
        bp->currentExtInfo = extInfo;
    } else {
        memset(&bp->currentExtInfo, 0, sizeof(bp->currentExtInfo));
        extInfo = bp->currentExtInfo;
    }

    if (GetPendingInstrumentEdit(bp, instrumentIndex, &pendingExtInfo)) {
        bp->currentExtInfo = pendingExtInfo;
        bp->hasDirtyExtInfo = true;
    }

    /* Use flags from instInfo (bank API provides them separately) */
    bp->currentExtInfo.flags1 = instInfo.flags1;
    bp->currentExtInfo.flags2 = instInfo.flags2;

    {
        wxString instName = instInfo.name[0] ? wxString(instInfo.name) : wxString("(unnamed)");
        instName.Replace("\r", "");
        instName.Replace("\n", " ");
        instName.Trim();
        bp->instParamsPanel->LoadFromExtInfo(bp->currentExtInfo, instName, instInfo.program);
        bp->instParamsPanel->Show();
    }

    bp->instPage->Layout();
    bp->detailNotebook->SetSelection(0);  /* Switch to Instrument tab */
}

/* ------------------------------------------------------------------ */
/* Show sample detail in the Samples tab                              */
/* ------------------------------------------------------------------ */

static void FreeCachedWaveform(BankEditorPanel *bp)
{
    if (bp->cachedWaveformData) {
        BAERmfEditorBank_FreeWaveformData(bp->cachedWaveformData);
        bp->cachedWaveformData = nullptr;
    }
}

static void RefreshWaveform(BankEditorPanel *bp)
{
    void *waveData = nullptr;
    uint32_t frameCount = 0;
    uint16_t bitSize = 16, channels = 1;
    BAE_UNSIGNED_FIXED sampleRate = 0;
    BAEResult waveResult;

    FreeCachedWaveform(bp);

    if (!bp->bankToken || !bp->hasSampleSelection) {
        BAE_PRINTF( "[bank] RefreshWaveform: skip (bankToken=%p hasSampleSel=%d)\n",
                (void *)bp->bankToken, bp->hasSampleSelection);
        if (bp->sampleParamsPanel) {
            bp->sampleParamsPanel->SetWaveform(nullptr, 0, 16, 1);
        }
        return;
    }

    waveResult = BAERmfEditorBank_GetSampleWaveformData(bp->bankToken,
                                                bp->currentInstrumentIndex,
                                                bp->currentSampleIndex,
                                                &waveData, &frameCount,
                                                &bitSize, &channels,
                                                &sampleRate);
    BAE_PRINTF( "[bank] RefreshWaveform: inst=%u sample=%u result=%d waveData=%p frames=%u bits=%u ch=%u\n",
            bp->currentInstrumentIndex, bp->currentSampleIndex,
            (int)waveResult, waveData, frameCount, bitSize, channels);
    if (waveResult == BAE_NO_ERROR && waveData) {
        bp->cachedWaveformData = waveData;
        if (bp->sampleParamsPanel) {
            bp->sampleParamsPanel->SetWaveform(waveData, frameCount, bitSize, channels);
            bp->sampleParamsPanel->SetLoopPoints(bp->currentSampleInfo.loopStart,
                                                  bp->currentSampleInfo.loopEnd);
        }
    } else {
        if (bp->sampleParamsPanel) {
            bp->sampleParamsPanel->SetWaveform(nullptr, 0, 16, 1);
        }
    }
}

static void StopBankKeyboardPreview(BankEditorPanel *bp)
{
    if (!bp->keyboardPreviewNotes.empty() && bp->stopCallback) {
        for (auto const &entry : bp->keyboardPreviewNotes) {
            bp->stopCallback(entry.first);
        }
    }
    bp->keyboardPreviewNotes.clear();
    if (bp->mousePreviewActive && bp->stopCallback) {
        bp->stopCallback(kBankMousePreviewTag);
        bp->mousePreviewActive = false;
    }
    if (bp->pianoPanel) {
        bp->pianoPanel->ClearExternalPressedNote();
    }
}

static void ShowSampleDetail(BankEditorPanel *bp,
                             uint32_t instrumentIndex,
                             uint32_t sampleIndex,
                             BAERmfEditorCompressionType preferredCompression,
                             BAERmfEditorOpusMode preferredOpusMode,
                             bool preferredOpusRoundTrip)
{
    BAERmfEditorBankSampleInfo sampleInfo;
    BAERmfEditorInstrumentExtInfo pendingExtInfo;
    bool hasPendingEdit;

    StopBankKeyboardPreview(bp);

    if (!bp->bankToken) {
        bp->sampleHeaderLabel->SetLabel("No bank loaded.");
        bp->hasSampleSelection = false;
        if (bp->sampleParamsPanel) {
            bp->sampleParamsPanel->ClearUI();
            bp->sampleParamsPanel->Hide();
        }
        return;
    }

    if (BAERmfEditorBank_GetInstrumentSampleInfo(bp->bankToken, instrumentIndex, sampleIndex, &sampleInfo) != BAE_NO_ERROR) {
        bp->sampleHeaderLabel->SetLabel("Failed to read sample info.");
        bp->hasSampleSelection = false;
        if (bp->sampleParamsPanel) {
            bp->sampleParamsPanel->ClearUI();
            bp->sampleParamsPanel->Hide();
        }
        return;
    }

    hasPendingEdit = GetPendingInstrumentEdit(bp, instrumentIndex, &pendingExtInfo);
    if (hasPendingEdit) {
        ApplyPendingSampleOverride(&sampleInfo, &pendingExtInfo, sampleIndex);
    }

    CacheSndMetadata(bp, sampleInfo);

    bp->currentSampleIndex = sampleIndex;
    bp->currentSampleInfo = sampleInfo;
    bp->hasSampleSelection = true;
    bp->hasLastUiCodecState = false;

    bp->sampleHeaderLabel->SetLabel(wxString::Format(
        "SND Resource: %d  |  Sample %u of instrument",
        static_cast<int>(sampleInfo.sndResourceID),
        static_cast<unsigned>(sampleIndex)));

    /* Populate the shared sample params panel */
    if (bp->sampleParamsPanel) {
        SampleParamsPanelData data;
        char sampleName[256] = {0};
        XGetFileResourceName((XFILE)bp->bankToken, ID_SND, (XLongResourceID)sampleInfo.sndResourceID, sampleName);
        if (sampleName[0] == 0) {
            XGetFileResourceName((XFILE)bp->bankToken, ID_CSND, (XLongResourceID)sampleInfo.sndResourceID, sampleName);
        }
        if (sampleName[0] == 0) {
            XGetFileResourceName((XFILE)bp->bankToken, ID_ESND, (XLongResourceID)sampleInfo.sndResourceID, sampleName);
        }
        wxString resolvedName = sampleName[0] ? wxString(sampleName) : wxString();
        resolvedName.Replace("\r", "");
        resolvedName.Replace("\n", " ");
        resolvedName.Trim();
        if (!resolvedName.IsEmpty() && resolvedName != "(null)" && resolvedName != "unknown" && resolvedName != "sample") {
            data.displayName = resolvedName;
        } else {
            data.displayName = wxString::Format("Sample %u", (unsigned)sampleIndex);
        }
        data.rootKey = (sampleInfo.rootKey == 0) ? 60 : sampleInfo.rootKey;
        data.lowKey = sampleInfo.lowKey;
        data.highKey = sampleInfo.highKey;
        data.splitVolume = sampleInfo.splitVolume;
        data.sampleRate = sampleInfo.sampleRate;
        data.waveFrames = sampleInfo.frameCount;
        data.loopStart = sampleInfo.loopStart;
        data.loopEnd = sampleInfo.loopEnd;
        /* When the caller specifies a preferred codec (after Apply re-encode),
         * use that so the dropdown stays on the user's choice.  Otherwise check
         * if we previously applied a codec to this sample and restore it.
         * Fall back to "Original" (DONT_CHANGE) for untouched samples. */
        if (preferredCompression != BAE_EDITOR_COMPRESSION_DONT_CHANGE) {
            data.compressionType = preferredCompression;
            data.opusRoundTripResample = preferredOpusRoundTrip;
        } else if (hasPendingEdit && pendingExtInfo.hasSampleOverride != FALSE &&
                   pendingExtInfo.sampleOverrideIndex == sampleIndex &&
                   (pendingExtInfo.sampleTargetCompression != BAE_EDITOR_COMPRESSION_DONT_CHANGE ||
                    pendingExtInfo.sampleTargetStorageType == kBankOriginalStorageSentinel)) {
            data.compressionType = pendingExtInfo.sampleTargetCompression;
            data.opusRoundTripResample =
                ((((int)pendingExtInfo.sampleTargetStorageType) & kBankOpusRoundTripStorageFlag) != 0);
            preferredOpusMode = pendingExtInfo.sampleTargetOpusMode;
        } else {
            auto key = std::make_pair(instrumentIndex, sampleIndex);
            auto it = bp->appliedCodecs.find(key);
            if (it != bp->appliedCodecs.end()) {
                data.compressionType = it->second.compression;
                preferredOpusMode = it->second.opusMode;
                data.opusRoundTripResample = it->second.opusRoundTrip;
            } else {
                data.compressionType = preferredCompression;
                data.opusRoundTripResample = preferredOpusRoundTrip;
            }
        }
        data.hasOriginalData = true;
        data.sndStorageType = sampleInfo.sndStorageType;
        data.opusMode = preferredOpusMode;
        data.codecDescription = wxString::FromUTF8(
            CompressionTypeName((uint32_t)sampleInfo.compressionType,
                                sampleInfo.compressionSubType,
                                sampleInfo.opusRoundTripResample ? true : false,
                                sampleInfo.bitDepth));
        if (bp->sourceCodecCallback) {
            wxString sourceCodec;
            if (bp->sourceCodecCallback((uint16_t)sampleInfo.sndResourceID, sourceCodec) && !sourceCodec.empty()) {
                data.codecDescription = sourceCodec;
            }
        }
        bp->sampleParamsPanel->LoadSample(data);
        bp->sampleParamsPanel->Show();
    }

    RefreshSampleListRow(bp, instrumentIndex, sampleIndex);

    RefreshWaveform(bp);
    bp->samplesPage->Layout();
    bp->detailNotebook->SetSelection(1);  /* Switch to Samples tab */
}

/* ------------------------------------------------------------------ */
/* Event handlers                                                     */
/* ------------------------------------------------------------------ */

static void OnInstrumentSelected(BankEditorPanel *bp, wxTreeEvent &event)
{
    wxTreeItemId item = event.GetItem();

    if (bp->hasInstrument && bp->hasPendingEdits) {
        ApplyDirtyParams(bp);
    }
    bp->hasPendingEdits = false;
    if (!item.IsOk()) {
        return;
    }

    BankInstrumentItemData *data = dynamic_cast<BankInstrumentItemData *>(
        bp->instrumentTree->GetItemData(item));
    if (!data) {
        /* Group node selected, not an instrument */
        bp->sampleList->DeleteAllItems();
        bp->sampleHeaderLabel->SetLabel("Select a sample from the list to view details.");
        if (bp->sampleParamsPanel) {
            bp->sampleParamsPanel->ClearUI();
            bp->sampleParamsPanel->Hide();
        }
        bp->instHeaderLabel->SetLabel("Select an instrument to view details.");
        if (bp->instParamsPanel) {
            bp->instParamsPanel->ClearUI();
            bp->instParamsPanel->Hide();
        }
        bp->hasInstrument = false;
        return;
    }

    uint32_t instrumentIndex = data->GetInstrumentIndex();
    InvalidateBankPreviewCache(bp);
    PopulateSampleList(bp, instrumentIndex);
    ShowInstrumentDetail(bp, instrumentIndex);
}

static void OnSampleSelected(BankEditorPanel *bp, wxListEvent &event)
{
    if (!bp->hasInstrument) {
        return;
    }
    if (bp->hasSampleSelection && bp->hasPendingEdits) {
        ApplyDirtyParams(bp);
    }
    bp->hasPendingEdits = false;
    long sel = event.GetIndex();
    if (sel < 0) {
        return;
    }
    ShowSampleDetail(bp,
                     bp->currentInstrumentIndex,
                     (uint32_t)sel,
                     BAE_EDITOR_COMPRESSION_DONT_CHANGE,
                     BAE_EDITOR_OPUS_MODE_AUDIO,
                     false);
}

static void OnInstrumentContextMenu(BankEditorPanel *bp, wxTreeEvent &event)
{
    enum {
        kCtxCloneToSong = wxID_HIGHEST + 200,
        kCtxAliasToSong,
        kCtxDeleteFromSong,
        kCtxCompressInstrumentSamples,
        kCtxAddInstrument,
        kCtxCloneInstrument,
        kCtxAliasInstrument
    };
    wxTreeItemId item = event.GetItem();
    BankInstrumentItemData *data;
    uint32_t instrumentIndex;
    wxMenu menu;

    if (!item.IsOk()) {
        return;
    }
    
    data = dynamic_cast<BankInstrumentItemData *>(bp->instrumentTree->GetItemData(item));
    if (!data) {
        /* This is a group node (no BankInstrumentItemData attached).
         * Check if it's a direct child of the root. */
        wxTreeItemId root = bp->instrumentTree->GetRootItem();
        wxTreeItemId parent = bp->instrumentTree->GetItemParent(item);
        
        if (!root.IsOk() || !parent.IsOk() || parent != root) {
            return;
        }

        /* This is a group node under root. Show "Add Instrument" option. */
        bool canAdd = false;
        
        /* Scan children to check if all programs (0-127) are used */
        bool usedPrograms[128];
        memset(usedPrograms, 0, sizeof(usedPrograms));
        
        wxTreeItemIdValue cookie;
        wxTreeItemId childItem = (bp->instrumentTree->GetFirstChild)(item, cookie);
        while (childItem.IsOk()) {
            BankInstrumentItemData *childData = dynamic_cast<BankInstrumentItemData *>(
                bp->instrumentTree->GetItemData(childItem));
            if (childData && bp->bankToken) {
                BAERmfEditorBankInstrumentInfo info;
                if (BAERmfEditorBank_GetInstrumentInfo(bp->bankToken, childData->GetInstrumentIndex(), &info) == BAE_NO_ERROR) {
                    if (info.program < 128) {
                        usedPrograms[info.program] = true;
                    }
                }
            }
            childItem = bp->instrumentTree->GetNextChild(item, cookie);
        }

        /* Check if all 128 programs are used */
        for (int i = 0; i < 128; ++i) {
            if (!usedPrograms[i]) {
                canAdd = true;
                break;
            }
        }

        menu.Append(kCtxAddInstrument, "Add Instrument");
        menu.Enable(kCtxAddInstrument, canAdd);
        /* Determine the instID base for this group from the first child with data. */
        uint32_t groupInstIDBase = 0;
        {
            wxTreeItemIdValue cookieBase;
            wxTreeItemId firstChild = (bp->instrumentTree->GetFirstChild)(item, cookieBase);
            if (firstChild.IsOk()) {
                BankInstrumentItemData *firstData = dynamic_cast<BankInstrumentItemData *>(
                    bp->instrumentTree->GetItemData(firstChild));
                if (firstData) {
                    uint32_t fid = firstData->GetInstID();
                    /* Round down to group boundary (0,128,256,384,512,...) */
                    groupInstIDBase = (fid / 128u) * 128u;
                }
            } else {
                /* Empty group - derive base from label by checking which range is empty */
                wxString label = bp->instrumentTree->GetItemText(item);
                if      (label.Contains("GM Melodic"))         groupInstIDBase = 0;
                else if (label.Contains("GM Percussion"))      groupInstIDBase = 128;
                else if (label.Contains("Special Melodic"))    groupInstIDBase = 256;
                else if (label.Contains("Special Percussion")) groupInstIDBase = 384;
                else                                            groupInstIDBase = 512;
            }
        }
        menu.Bind(wxEVT_MENU, [bp, groupInstIDBase](wxCommandEvent &) {
            if (bp->addInstrumentCallback) {
                bp->addInstrumentCallback(groupInstIDBase);
            }
        }, kCtxAddInstrument);
        bp->instrumentTree->PopupMenu(&menu);
        return;
    }
    instrumentIndex = data->GetInstrumentIndex();

    /* Ensure right-clicked item becomes active selection first. */
    if (bp->instrumentTree->GetSelection() != item) {
        bp->instrumentTree->SelectItem(item);
    }

    menu.Append(kCtxCloneToSong, "Clone Instrument to Song");
    menu.Append(kCtxAliasToSong, "Alias Instrument to Song");
    menu.Append(kCtxCompressInstrumentSamples, "Compress Instrument Samples");
    menu.AppendSeparator();

    /* Gate Clone/Alias Instrument by whether any free instID slot exists across all groups */
    /* Determine whether percussion slots are valid destinations:
     * Only instruments with 0 splits (single-sample drum note instruments)
     * can target the percussion ID ranges (128-255, 384-511). */
    int splitCount = 0;
    {
        BAERmfEditorBankInstrumentInfo thisInfo;
        if (BAERmfEditorBank_GetInstrumentInfo(bp->bankToken, instrumentIndex, &thisInfo) == BAE_NO_ERROR)
            splitCount = thisInfo.keySplitCount;
    }
    bool allowPerc = (splitCount == 0);

    bool anyFreeSlot = false;
    if (bp->bankToken) {
        uint32_t instCount = 0;
        BAERmfEditorBank_GetInstrumentCount(bp->bankToken, &instCount);
        bool usedInstIDs[512];
        memset(usedInstIDs, 0, sizeof(usedInstIDs));
        for (uint32_t i = 0; i < instCount; ++i) {
            BAERmfEditorBankInstrumentInfo info;
            if (BAERmfEditorBank_GetInstrumentInfo(bp->bankToken, i, &info) == BAE_NO_ERROR &&
                info.instID < 512u) {
                usedInstIDs[info.instID] = true;
            }
        }
        /* Also mark aliased-from IDs as occupied */
        XFILE bankFile = (XFILE)bp->bankToken;
        XAliasLinkResource *pAlias = XGetAliasLinkFromFile(bankFile);
        if (pAlias) {
            uint32_t cnt = (uint32_t)XGetLong(&pAlias->numberOfAliases);
            for (uint32_t i = 0; i < cnt; ++i) {
                uint32_t fromID = (uint32_t)XGetLong(&pAlias->list[i].aliasFrom);
                if (fromID < 512u) usedInstIDs[fromID] = true;
            }
            XDisposePtr((XPTR)pAlias);
        }
        /* Check melodic ranges always; percussion ranges only when splits == 0 */
        static const struct { int lo; int hi; bool isPerc; } kRanges[] = {
            {   0, 127, false },
            { 128, 255, true  },
            { 256, 383, false },
            { 384, 511, true  },
        };
        for (int r = 0; r < 4 && !anyFreeSlot; ++r) {
            if (kRanges[r].isPerc && !allowPerc) continue;
            for (int i = kRanges[r].lo; i <= kRanges[r].hi; ++i) {
                if (!usedInstIDs[i]) { anyFreeSlot = true; break; }
            }
        }
    }
    menu.Append(kCtxCloneInstrument, "Clone Instrument");
    menu.Enable(kCtxCloneInstrument, anyFreeSlot);
    menu.Append(kCtxAliasInstrument, "Alias Instrument");
    menu.Enable(kCtxAliasInstrument, anyFreeSlot);
    menu.AppendSeparator();
    menu.Append(kCtxDeleteFromSong, "Delete Instrument");

    menu.Bind(wxEVT_MENU, [bp, instrumentIndex](wxCommandEvent &) {
        if (bp->cloneToSongCallback) {
            bp->cloneToSongCallback(instrumentIndex);
        }
    }, kCtxCloneToSong);
    menu.Bind(wxEVT_MENU, [bp, instrumentIndex](wxCommandEvent &) {
        if (bp->aliasToSongCallback) {
            bp->aliasToSongCallback(instrumentIndex);
        }
    }, kCtxAliasToSong);
    uint32_t displayInstID = data->GetInstID();
    menu.Bind(wxEVT_MENU, [bp, instrumentIndex, displayInstID](wxCommandEvent &) {
        /* Confirm deletion before proceeding */
        wxMessageDialog confirmDlg(nullptr,
            "Are you sure you want to delete this instrument?",
            "Delete Instrument",
            wxYES_NO | wxICON_QUESTION);
        if (confirmDlg.ShowModal() != wxID_YES) {
            return;
        }
        
        if (bp->deleteFromSongCallback) {
            bp->deleteFromSongCallback(instrumentIndex, displayInstID);
        }
        
        /* Refresh the instrument tree to remove the deleted instrument */
        PopulateInstrumentTree(bp);
    }, kCtxDeleteFromSong);
    menu.Bind(wxEVT_MENU, [bp, instrumentIndex](wxCommandEvent &) {
        if (bp->compressInstrumentSamplesCallback) {
            bp->compressInstrumentSamplesCallback(instrumentIndex);
        }
    }, kCtxCompressInstrumentSamples);
    menu.Bind(wxEVT_MENU, [bp, instrumentIndex](wxCommandEvent &) {
        if (bp->cloneInstrumentCallback) {
            bp->cloneInstrumentCallback(instrumentIndex);
        }
    }, kCtxCloneInstrument);
    menu.Bind(wxEVT_MENU, [bp, instrumentIndex](wxCommandEvent &) {
        if (bp->aliasInstrumentCallback) {
            bp->aliasInstrumentCallback(instrumentIndex);
        }
    }, kCtxAliasInstrument);

    bp->instrumentTree->PopupMenu(&menu);
}

static void OnSampleContextMenu(BankEditorPanel *bp, wxContextMenuEvent &event)
{
    enum {
        kCtxAddSample = wxID_HIGHEST + 300,
        kCtxDeleteSample,
        kCtxAliasSampleToInstrument,
        kCtxCopySampleToInstrument
    };
    wxPoint screenPos;
    wxPoint clientPos;
    uint32_t sampleIndex;
    wxMenu menu;

    if (!bp || !bp->hasInstrument) {
        return;
    }

    /* Mouse right-clicks are handled by wxEVT_LIST_ITEM_RIGHT_CLICK so we can
     * use the exact row index from wxListEvent. Keep this handler for keyboard
     * menu invocation (Shift+F10 / Menu key). */
    screenPos = event.GetPosition();
    if (screenPos != wxDefaultPosition) {
        return;
    }

    if (bp->hasSampleSelection) {
        sampleIndex = bp->currentSampleIndex;
        long focused = bp->sampleList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_FOCUSED);
        if (focused >= 0) {
            sampleIndex = (uint32_t)focused;
        }
        clientPos = wxPoint(16, 16);
    } else {
        return;
    }

    menu.Append(kCtxAddSample, "Add Sample");
    menu.Append(kCtxDeleteSample, "Delete Sample");
    menu.AppendSeparator();
    menu.Append(kCtxAliasSampleToInstrument, "Alias Sample to Another Instrument");
    menu.Append(kCtxCopySampleToInstrument, "Copy Sample to Another Instrument");

    menu.Bind(wxEVT_MENU, [bp, sampleIndex](wxCommandEvent &) {
        if (bp->addSampleCallback) {
            bp->addSampleCallback(bp->currentInstrumentIndex, sampleIndex);
        }
    }, kCtxAddSample);
    menu.Bind(wxEVT_MENU, [bp, sampleIndex](wxCommandEvent &) {
        if (bp->deleteSampleCallback) {
            bp->deleteSampleCallback(bp->currentInstrumentIndex, sampleIndex);
        }
    }, kCtxDeleteSample);
    menu.Bind(wxEVT_MENU, [bp, sampleIndex](wxCommandEvent &) {
        if (bp->aliasSampleToInstrumentCallback) {
            bp->aliasSampleToInstrumentCallback(bp->currentInstrumentIndex, sampleIndex);
        }
    }, kCtxAliasSampleToInstrument);
    menu.Bind(wxEVT_MENU, [bp, sampleIndex](wxCommandEvent &) {
        if (bp->copySampleToInstrumentCallback) {
            bp->copySampleToInstrumentCallback(bp->currentInstrumentIndex, sampleIndex);
        }
    }, kCtxCopySampleToInstrument);

    bp->sampleList->PopupMenu(&menu, clientPos);
}

static void OnSampleItemRightClick(BankEditorPanel *bp, wxListEvent &event)
{
    enum {
        kCtxAddSample = wxID_HIGHEST + 300,
        kCtxDeleteSample,
        kCtxAliasSampleToInstrument,
        kCtxCopySampleToInstrument
    };
    long hit = event.GetIndex();
    uint32_t sampleIndex;
    wxPoint clientPos = event.GetPoint();
    wxMenu menu;

    if (!bp || !bp->hasInstrument || hit < 0) {
        return;
    }

    bp->sampleList->SetItemState(hit,
                                 wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                                 wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
    sampleIndex = (uint32_t)hit;

    menu.Append(kCtxAddSample, "Add Sample");
    menu.Append(kCtxDeleteSample, "Delete Sample");
    menu.AppendSeparator();
    menu.Append(kCtxAliasSampleToInstrument, "Alias Sample to Another Instrument");
    menu.Append(kCtxCopySampleToInstrument, "Copy Sample to Another Instrument");

    menu.Bind(wxEVT_MENU, [bp, sampleIndex](wxCommandEvent &) {
        if (bp->addSampleCallback) {
            bp->addSampleCallback(bp->currentInstrumentIndex, sampleIndex);
        }
    }, kCtxAddSample);
    menu.Bind(wxEVT_MENU, [bp, sampleIndex](wxCommandEvent &) {
        if (bp->deleteSampleCallback) {
            bp->deleteSampleCallback(bp->currentInstrumentIndex, sampleIndex);
        }
    }, kCtxDeleteSample);
    menu.Bind(wxEVT_MENU, [bp, sampleIndex](wxCommandEvent &) {
        if (bp->aliasSampleToInstrumentCallback) {
            bp->aliasSampleToInstrumentCallback(bp->currentInstrumentIndex, sampleIndex);
        }
    }, kCtxAliasSampleToInstrument);
    menu.Bind(wxEVT_MENU, [bp, sampleIndex](wxCommandEvent &) {
        if (bp->copySampleToInstrumentCallback) {
            bp->copySampleToInstrumentCallback(bp->currentInstrumentIndex, sampleIndex);
        }
    }, kCtxCopySampleToInstrument);

    bp->sampleList->PopupMenu(&menu, clientPos);
}

static void OnGlobalSampleSelected(BankEditorPanel *bp, wxTreeEvent &event)
{
    wxTreeItemId item = event.GetItem();
    if (!item.IsOk()) {
        return;
    }
    if (!dynamic_cast<BankSampleItemData *>(bp->sampleTree->GetItemData(item))) {
        /* Root node selected - clear both right-hand panels */
        bp->hasInstrument = false;
        bp->hasSampleSelection = false;
        bp->instHeaderLabel->SetLabel("Select an instrument to view details.");
        if (bp->instParamsPanel) {
            bp->instParamsPanel->ClearUI();
            bp->instParamsPanel->Hide();
            if (bp->instPage) bp->instPage->Layout();
        }
        bp->sampleHeaderLabel->SetLabel("Select a sample from the list to view details.");
        if (bp->sampleParamsPanel) {
            bp->sampleParamsPanel->ClearUI();
            bp->sampleParamsPanel->Hide();
            bp->samplesPage->Layout();
        }
        return;
    }

    /* Intentionally do not auto-load sample details on single-click.
     * Editing is triggered via double-click or the context menu's Edit action. */
}

static void OnGlobalSampleContextMenu(BankEditorPanel *bp, wxTreeEvent &event)
{
    enum {
        kCtxAddSampleToInstrument = wxID_HIGHEST + 400,
        kCtxCopySampleToInstrument,
        kCtxEditSample,
        kCtxDeleteSample
    };
    wxTreeItemId item = event.GetItem();
    BankSampleItemData *data;
    uint16_t sndID;
    wxMenu menu;

    if (!item.IsOk()) {
        return;
    }
    data = dynamic_cast<BankSampleItemData *>(bp->sampleTree->GetItemData(item));
    if (!data) {
        /* Root node selected, no context menu */
        return;
    }
    sndID = data->GetSndID();

    if (bp->sampleTree->GetSelection() != item) {
        bp->sampleTree->SelectItem(item);
    }

    menu.Append(kCtxAddSampleToInstrument, "Add Sample Pointer (Alias) to Instrument");
    menu.Append(kCtxCopySampleToInstrument, "Copy Sample to an Instrument");
    menu.Append(kCtxEditSample, "Edit Sample");
    menu.AppendSeparator();
    menu.Append(kCtxDeleteSample, "Delete Sample");

    menu.Bind(wxEVT_MENU, [bp, sndID](wxCommandEvent &) {
        if (bp->globalAddSampleToInstrumentCallback) {
            bp->globalAddSampleToInstrumentCallback(sndID);
        }
    }, kCtxAddSampleToInstrument);
    menu.Bind(wxEVT_MENU, [bp, sndID](wxCommandEvent &) {
        if (bp->globalCopySampleToInstrumentCallback) {
            bp->globalCopySampleToInstrumentCallback(sndID);
        }
    }, kCtxCopySampleToInstrument);
    menu.Bind(wxEVT_MENU, [bp, sndID, data](wxCommandEvent &) {
        if (bp->globalEditSampleCallback) {
            bp->globalEditSampleCallback(sndID, data->GetReferences());
        }
    }, kCtxEditSample);
    menu.Bind(wxEVT_MENU, [bp, sndID, data](wxCommandEvent &) {
        if (bp->globalDeleteSampleCallback) {
            bp->globalDeleteSampleCallback(sndID, data->GetReferences());
        }
    }, kCtxDeleteSample);

    bp->sampleTree->PopupMenu(&menu);
}

/* ------------------------------------------------------------------ */
/* Build Instrument Tab                                               */
/* ------------------------------------------------------------------ */

static void BuildInstrumentTab(BankEditorPanel *bp)
{
    bp->instPage = new wxPanel(bp->detailNotebook);
    wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);

    /* Header (bank-specific) */
    bp->instHeaderLabel = new wxStaticText(bp->instPage, wxID_ANY, "Select an instrument to view details.");
    sizer->Add(bp->instHeaderLabel, 0, wxALL, 8);

    /* Shared instrument parameters panel */
    bp->instParamsPanel = new InstrumentParamsPanel(bp->instPage);
    bp->instParamsPanel->SetOnParameterChanged([bp]() {
        bp->hasPendingEdits = true;
    });
    bp->instParamsPanel->Hide();
    sizer->Add(bp->instParamsPanel, 1, wxEXPAND);

    bp->instPage->SetSizerAndFit(sizer);
    bp->detailNotebook->AddPage(bp->instPage, "Instrument");
}

/* ------------------------------------------------------------------ */
/* Build Samples Tab                                                  */
/* ------------------------------------------------------------------ */

static void BuildSamplesTab(BankEditorPanel *bp)
{
    bp->samplesPage = new wxPanel(bp->detailNotebook);
    wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);

    bp->sampleHeaderLabel = new wxStaticText(bp->samplesPage, wxID_ANY, "Select a sample from the list to view details.");
    sizer->Add(bp->sampleHeaderLabel, 0, wxALL, 8);

    /* Shared sample params panel (read-only for bank viewer) */
    bp->sampleParamsPanel = new SampleParamsPanel(bp->samplesPage);
    bp->sampleParamsPanel->SetWriteMode(false, false, false, false);
    bp->sampleParamsPanel->SetCodecControlsVisible(true);
    bp->sampleParamsPanel->Hide();
    bp->sampleParamsPanel->SetOnParameterChanged([bp]() {
        bp->hasPendingEdits = true;
    });
    bp->sampleParamsPanel->SetOnLoopChanged([bp]() {
        bp->hasPendingEdits = true;
    });
    sizer->Add(bp->sampleParamsPanel, 1, wxEXPAND);

    bp->samplesPage->SetSizerAndFit(sizer);
    bp->detailNotebook->AddPage(bp->samplesPage, "Samples");
}

/* ------------------------------------------------------------------ */
/* Public C-style interface                                           */
/* ------------------------------------------------------------------ */

BankEditorPanel *CreateBankEditorPanel(wxWindow *parent)
{
    BankEditorPanel *bp = new BankEditorPanel();
    bp->bankToken = nullptr;
    bp->hasInstrument = false;
    bp->hasSampleSelection = false;
    bp->mousePreviewActive = false;
    bp->cachedWaveformData = nullptr;
    bp->lastUiCompressionType = BAE_EDITOR_COMPRESSION_DONT_CHANGE;
    bp->lastUiStorageType = BAE_EDITOR_SND_STORAGE_ESND;
    bp->lastUiOpusRoundTrip = false;
    bp->hasLastUiCodecState = false;
    bp->currentInstrumentIndex = 0;
    bp->currentSampleIndex = 0;
    bp->hasDirtyExtInfo = false;
    bp->hasPendingEdits = false;
    bp->applyBtn = nullptr;
    bp->stopAllBtn = nullptr;
    memset(&bp->currentExtInfo, 0, sizeof(bp->currentExtInfo));
    memset(&bp->dirtyExtInfo, 0, sizeof(bp->dirtyExtInfo));
    memset(&bp->currentSampleInfo, 0, sizeof(bp->currentSampleInfo));
    memset(&bp->currentInstInfo, 0, sizeof(bp->currentInstInfo));

    bp->panel = new wxPanel(parent);
    bp->splitter = new wxSplitterWindow(bp->panel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                        wxSP_LIVE_UPDATE | wxSP_3D);

    /* Left side: tabbed interface with Instruments and Samples tabs */
    wxPanel *leftPanel = new wxPanel(bp->splitter);
    wxBoxSizer *leftPanelSizer = new wxBoxSizer(wxVERTICAL);
    
    bp->leftNotebook = new wxNotebook(leftPanel, wxID_ANY);
    
    /* ===== Instruments Tab ===== */
    bp->instrumentsTabPanel = new wxPanel(bp->leftNotebook);
    wxBoxSizer *instTabSizer = new wxBoxSizer(wxVERTICAL);
    
    instTabSizer->Add(new wxStaticText(bp->instrumentsTabPanel, wxID_ANY, "Instruments"), 0, wxALL, 4);
    bp->instrumentTree = new wxTreeCtrl(bp->instrumentsTabPanel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                        wxTR_HAS_BUTTONS | wxTR_LINES_AT_ROOT | wxTR_SINGLE);
    instTabSizer->Add(bp->instrumentTree, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

    instTabSizer->Add(new wxStaticText(bp->instrumentsTabPanel, wxID_ANY, "Samples / Key Splits"), 0, wxLEFT | wxRIGHT, 4);
    bp->sampleList = new wxListCtrl(bp->instrumentsTabPanel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                    wxLC_REPORT | wxLC_SINGLE_SEL);
    bp->sampleList->SetMinSize(wxSize(-1, 150));
    bp->sampleList->InsertColumn(0, "Sample", wxLIST_FORMAT_LEFT, 80);
    bp->sampleList->InsertColumn(1, "Key Range", wxLIST_FORMAT_LEFT, 80);
    bp->sampleList->InsertColumn(2, "Root", wxLIST_FORMAT_LEFT, 50);
    bp->sampleList->InsertColumn(3, "Rate", wxLIST_FORMAT_LEFT, 80);
    bp->sampleList->InsertColumn(4, "Format", wxLIST_FORMAT_LEFT, 100);
    bp->sampleList->InsertColumn(5, "Codec", wxLIST_FORMAT_LEFT, 70);
    bp->sampleList->InsertColumn(6, "Frames", wxLIST_FORMAT_LEFT, 80);
    instTabSizer->Add(bp->sampleList, 0, wxEXPAND | wxALL, 4);
    bp->instrumentsTabPanel->SetSizer(instTabSizer);
    bp->leftNotebook->AddPage(bp->instrumentsTabPanel, "Instruments");
    
    /* ===== Samples Tab (Global) ===== */
    bp->samplesTabPanel = new wxPanel(bp->leftNotebook);
    wxBoxSizer *sampTabSizer = new wxBoxSizer(wxVERTICAL);
    
    sampTabSizer->Add(new wxStaticText(bp->samplesTabPanel, wxID_ANY, "All Samples (sorted by name)"), 0, wxALL, 4);
    bp->sampleTree = new wxTreeCtrl(bp->samplesTabPanel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                    wxTR_HAS_BUTTONS | wxTR_LINES_AT_ROOT | wxTR_SINGLE);
    sampTabSizer->Add(bp->sampleTree, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);
    bp->samplesTabPanel->SetSizer(sampTabSizer);
    bp->leftNotebook->AddPage(bp->samplesTabPanel, "Samples");
    
    leftPanelSizer->Add(bp->leftNotebook, 1, wxEXPAND);
    leftPanel->SetSizer(leftPanelSizer);

    /* Right side: detail notebook with Instrument and Samples tabs */
    wxPanel *rightPanel = new wxPanel(bp->splitter);
    wxBoxSizer *rightSizer = new wxBoxSizer(wxVERTICAL);

    bp->detailNotebook = new wxNotebook(rightPanel, wxID_ANY);
    BuildInstrumentTab(bp);
    BuildSamplesTab(bp);

    rightSizer->Add(bp->detailNotebook, 1, wxEXPAND);

    /* Apply / Stop All Notes buttons - visible for both tabs */
    {
        wxBoxSizer *btnSizer = new wxBoxSizer(wxHORIZONTAL);
        bp->applyBtn = new wxButton(rightPanel, wxID_ANY, "Apply");
        bp->applyBtn->SetToolTip("Write current instrument parameter edits so preview notes hear them");
        bp->stopAllBtn = new wxButton(rightPanel, wxID_ANY, "Stop All Notes");
        bp->stopAllBtn->SetToolTip("Silence all currently-playing preview notes");
        btnSizer->Add(bp->applyBtn, 0, wxRIGHT, 5);
        btnSizer->Add(bp->stopAllBtn, 0);
        rightSizer->Add(btnSizer, 0, wxLEFT | wxRIGHT | wxTOP, 5);

        bp->applyBtn->Bind(wxEVT_BUTTON, [bp](wxCommandEvent &) {
            ApplyDirtyParams(bp);
        });
        bp->stopAllBtn->Bind(wxEVT_BUTTON, [bp](wxCommandEvent &) {
            StopAllBankNotes(bp);
        });
    }

    /* Piano keyboard below the buttons - visible for both tabs */
    bp->pianoPanel = new PianoKeyboardPanel(rightPanel,
        [bp](int note) {
            if (!bp->playCallback || !bp->hasInstrument) return;
            bool isPerc = (bp->currentInstInfo.instID % 256) >= 128;
            bp->playCallback(
                static_cast<unsigned char>(std::clamp<uint32_t>(bp->currentInstInfo.bank, 0, 127)),
                static_cast<unsigned char>(std::clamp<uint32_t>(bp->currentInstInfo.program, 0, 127)),
                note, kBankMousePreviewTag, isPerc);
            bp->mousePreviewActive = true;
        },
        [bp]() {
            if (bp->mousePreviewActive && bp->stopCallback) {
                bp->stopCallback(kBankMousePreviewTag);
            }
            bp->mousePreviewActive = false;
        });
    rightSizer->Add(bp->pianoPanel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);

    rightPanel->SetSizer(rightSizer);

    bp->splitter->SplitVertically(leftPanel, rightPanel, 500);
    bp->splitter->SetMinimumPaneSize(200);

    wxBoxSizer *mainSizer = new wxBoxSizer(wxVERTICAL);
    mainSizer->Add(bp->splitter, 1, wxEXPAND);
    bp->panel->SetSizer(mainSizer);

    /* Bind events */
    bp->instrumentTree->Bind(wxEVT_TREE_SEL_CHANGED, [bp](wxTreeEvent &event) {
        OnInstrumentSelected(bp, event);
    });
    bp->instrumentTree->Bind(wxEVT_TREE_ITEM_RIGHT_CLICK, [bp](wxTreeEvent &event) {
        OnInstrumentContextMenu(bp, event);
    });
    
    /* Global sample tree events */
    bp->sampleTree->Bind(wxEVT_TREE_SEL_CHANGED, [bp](wxTreeEvent &event) {
        OnGlobalSampleSelected(bp, event);
    });
    bp->sampleTree->Bind(wxEVT_TREE_ITEM_RIGHT_CLICK, [bp](wxTreeEvent &event) {
        OnGlobalSampleContextMenu(bp, event);
    });
    bp->sampleTree->Bind(wxEVT_TREE_ITEM_ACTIVATED, [bp](wxTreeEvent &event) {
        // Double-click or Enter to edit sample
        wxTreeItemId id = event.GetItem();
        if (id.IsOk()) {
            BankSampleItemData *data = static_cast<BankSampleItemData *>(bp->sampleTree->GetItemData(id));
            if (data) {
                uint16_t sndID = data->GetSndID();
                if (bp->globalEditSampleCallback) {
                    bp->globalEditSampleCallback(sndID, data->GetReferences());
                }
            }
        }
    });

    bp->sampleList->Bind(wxEVT_LIST_ITEM_SELECTED, [bp](wxListEvent &event) {
        OnSampleSelected(bp, event);
    });
    bp->sampleList->Bind(wxEVT_LIST_ITEM_RIGHT_CLICK, [bp](wxListEvent &event) {
        OnSampleItemRightClick(bp, event);
    });
    bp->sampleList->Bind(wxEVT_CONTEXT_MENU, [bp](wxContextMenuEvent &event) {
        OnSampleContextMenu(bp, event);
    });

    /* Click-to-defocus: clicking on panel backgrounds moves focus away from
     * text/spin fields so the musical keyboard becomes active.
     * SetFocusIgnoringChildren() is used instead of SetFocus() so that GTK
     * reliably removes the keyboard focus from any active text/spin child. */
    auto defocusClick = [bp](wxMouseEvent &event) {
        bp->panel->SetFocusIgnoringChildren();
        event.Skip();
    };
    bp->panel->Bind(wxEVT_LEFT_DOWN, defocusClick);
    if (bp->instPage) {
        bp->instPage->Bind(wxEVT_LEFT_DOWN, defocusClick);
    }
    bp->samplesPage->Bind(wxEVT_LEFT_DOWN, defocusClick);
    if (bp->instParamsPanel) {
        bp->instParamsPanel->Bind(wxEVT_LEFT_DOWN, defocusClick);
    }
    if (bp->sampleParamsPanel) {
        bp->sampleParamsPanel->Bind(wxEVT_LEFT_DOWN, defocusClick);
    }

    return bp;
}

wxWindow *BankEditorPanel_AsWindow(BankEditorPanel *panel)
{
    return panel ? panel->panel : nullptr;
}

void BankEditorPanel_LoadBank(BankEditorPanel *panel,
                              BAEBankToken bankToken,
                              const char *bankPath)
{
    if (!panel) {
        return;
    }
    StopBankKeyboardPreview(panel);
    FreeCachedWaveform(panel);
    panel->bankToken = bankToken;
    panel->bankPath = bankPath ? bankPath : "";
    panel->hasInstrument = false;
    panel->hasSampleSelection = false;
    panel->hasDirtyExtInfo = false;
    panel->hasPendingEdits = false;
    panel->hasLastUiCodecState = false;
    panel->appliedCodecs.clear();
    panel->sndMetadataCache.clear();
    if (panel->dirtyParamsCallback) panel->dirtyParamsCallback(nullptr);
    PopulateInstrumentTree(panel);
    PopulateSampleTree(panel);
    panel->sampleList->DeleteAllItems();
    panel->instHeaderLabel->SetLabel("Select an instrument to view details.");
    if (panel->instParamsPanel) {
        panel->instParamsPanel->ClearUI();
        panel->instParamsPanel->Hide();
    }
    panel->sampleHeaderLabel->SetLabel("Select a sample from the list to view details.");
    if (panel->sampleParamsPanel) {
        panel->sampleParamsPanel->ClearUI();
        panel->sampleParamsPanel->Hide();
    }
}

void BankEditorPanel_Clear(BankEditorPanel *panel)
{
    if (!panel) {
        return;
    }
    StopBankKeyboardPreview(panel);
    FreeCachedWaveform(panel);
    panel->bankToken = nullptr;
    panel->bankPath.clear();
    panel->hasInstrument = false;
    panel->hasSampleSelection = false;
    panel->hasDirtyExtInfo = false;
    panel->hasPendingEdits = false;
    panel->hasLastUiCodecState = false;
    panel->appliedCodecs.clear();
    panel->sndMetadataCache.clear();
    if (panel->dirtyParamsCallback) panel->dirtyParamsCallback(nullptr);
    panel->instrumentTree->DeleteAllItems();
    panel->instrumentTree->AddRoot("Instruments");
    panel->sampleList->DeleteAllItems();
    panel->sampleTree->DeleteAllItems();
    panel->sampleTree->AddRoot("Samples");
    panel->instHeaderLabel->SetLabel("Open an HSB or ZSB file to begin editing.");
    if (panel->instParamsPanel) {
        panel->instParamsPanel->ClearUI();
        panel->instParamsPanel->Hide();
    }
    panel->sampleHeaderLabel->SetLabel("Select a sample from the list to view details.");
    if (panel->sampleParamsPanel) {
        panel->sampleParamsPanel->ClearUI();
        panel->sampleParamsPanel->Hide();
    }
}

void BankEditorPanel_SetPreviewCallbacks(
    BankEditorPanel *panel,
    std::function<void(unsigned char bank, unsigned char program, int note, int previewTag, bool isPercussion)> playCallback,
    std::function<void(int previewTag)> stopCallback,
    std::function<void()> invalidateCallback,
    std::function<void(BAERmfEditorInstrumentExtInfo const *info)> dirtyParamsCallback)
{
    if (!panel) {
        return;
    }
    panel->playCallback = std::move(playCallback);
    panel->stopCallback = std::move(stopCallback);
    panel->invalidateCallback = std::move(invalidateCallback);
    panel->dirtyParamsCallback = std::move(dirtyParamsCallback);
}

void BankEditorPanel_SetPendingEditLookupCallback(
    BankEditorPanel *panel,
    std::function<bool(uint32_t instrumentIndex, BAERmfEditorInstrumentExtInfo *outInfo)> pendingEditLookupCallback)
{
    if (!panel) {
        return;
    }
    panel->pendingEditLookupCallback = std::move(pendingEditLookupCallback);
}

void BankEditorPanel_SetSourceCodecCallback(
    BankEditorPanel *panel,
    std::function<bool(uint16_t sndID, wxString &outCodecDescription)> sourceCodecCallback)
{
    if (!panel) {
        return;
    }
    panel->sourceCodecCallback = std::move(sourceCodecCallback);
}

void BankEditorPanel_SetInstrumentContextCallbacks(
    BankEditorPanel *panel,
    std::function<void(uint32_t instrumentIndex)> cloneToSongCallback,
    std::function<void(uint32_t instrumentIndex)> aliasToSongCallback,
    std::function<void(uint32_t instrumentIndex, uint32_t displayInstID)> deleteFromSongCallback,
    std::function<void(uint32_t instrumentIndex)> compressInstrumentSamplesCallback,
    std::function<void(uint32_t instIDBase)> addInstrumentCallback,
    std::function<void(uint32_t instrumentIndex)> cloneInstrumentCallback,
    std::function<void(uint32_t instrumentIndex)> aliasInstrumentCallback)
{
    if (!panel) {
        return;
    }
    panel->cloneToSongCallback = std::move(cloneToSongCallback);
    panel->aliasToSongCallback = std::move(aliasToSongCallback);
    panel->deleteFromSongCallback = std::move(deleteFromSongCallback);
    panel->compressInstrumentSamplesCallback = std::move(compressInstrumentSamplesCallback);
    panel->addInstrumentCallback = std::move(addInstrumentCallback);
    panel->cloneInstrumentCallback = std::move(cloneInstrumentCallback);
    panel->aliasInstrumentCallback = std::move(aliasInstrumentCallback);
}

void BankEditorPanel_SetSampleContextCallbacks(
    BankEditorPanel *panel,
    std::function<void(uint32_t instrumentIndex, uint32_t sampleIndex)> addSampleCallback,
    std::function<void(uint32_t instrumentIndex, uint32_t sampleIndex)> deleteSampleCallback,
    std::function<void(uint32_t instrumentIndex, uint32_t sampleIndex)> aliasSampleToInstrumentCallback,
    std::function<void(uint32_t instrumentIndex, uint32_t sampleIndex)> copySampleToInstrumentCallback)
{
    if (!panel) {
        return;
    }
    panel->addSampleCallback = std::move(addSampleCallback);
    panel->deleteSampleCallback = std::move(deleteSampleCallback);
    panel->aliasSampleToInstrumentCallback = std::move(aliasSampleToInstrumentCallback);
    panel->copySampleToInstrumentCallback = std::move(copySampleToInstrumentCallback);
}

void BankEditorPanel_SetGlobalSampleContextCallbacks(
    BankEditorPanel *panel,
    std::function<void(uint16_t sndID)> addSampleToInstrumentCallback,
    std::function<void(uint16_t sndID)> copySampleToInstrumentCallback,
    std::function<void(uint16_t sndID, const std::vector<std::pair<uint32_t, uint32_t>> &referencingInstruments)> deleteSampleCallback,
    std::function<void(uint16_t sndID, const std::vector<std::pair<uint32_t, uint32_t>> &referencingInstruments)> editSampleCallback)
{
    if (!panel) {
        return;
    }
    panel->globalAddSampleToInstrumentCallback = std::move(addSampleToInstrumentCallback);
    panel->globalCopySampleToInstrumentCallback = std::move(copySampleToInstrumentCallback);
    panel->globalDeleteSampleCallback = std::move(deleteSampleCallback);
    panel->globalEditSampleCallback = std::move(editSampleCallback);
}

uint32_t BankEditorPanel_GetCurrentInstrumentIndex(BankEditorPanel *panel)
{
    if (!panel) {
        return 0;
    }
    return panel->currentInstrumentIndex;
}

void BankEditorPanel_SelectInstrument(BankEditorPanel *panel, uint32_t instrumentIndex)
{
    if (!panel || !panel->bankToken) {
        return;
    }

    /* Find the tree item with matching instrumentIndex */
    wxTreeItemIdValue cookie;
    wxTreeItemId root = panel->instrumentTree->GetRootItem();
    wxTreeItemId item = (panel->instrumentTree->GetFirstChild)(root, cookie);
    
    while (item.IsOk()) {
        wxTreeItemIdValue groupCookie;
        wxTreeItemId children = (panel->instrumentTree->GetFirstChild)(item, groupCookie);
        
        while (children.IsOk()) {
            BankInstrumentItemData *data = static_cast<BankInstrumentItemData *>(panel->instrumentTree->GetItemData(children));
            if (data && data->GetInstrumentIndex() == instrumentIndex) {
                panel->instrumentTree->SelectItem(children);
                panel->instrumentTree->EnsureVisible(children);
                // Trigger OnInstrumentSelected via selection event
                wxTreeEvent event(wxEVT_TREE_SEL_CHANGED, panel->instrumentTree, children);
                OnInstrumentSelected(panel, event);
                return;
            }
            children = panel->instrumentTree->GetNextChild(item, groupCookie);
        }
        item = panel->instrumentTree->GetNextChild(root, cookie);
    }
}

void BankEditorPanel_SelectSample(BankEditorPanel *panel, uint32_t instrumentIndex, uint32_t sampleIndex)
{
    if (!panel || !panel->bankToken) {
        return;
    }

    /* First ensure the instrument is loaded */
    BankEditorPanel_SelectInstrument(panel, instrumentIndex);

    /* Then show the sample detail */
    ShowSampleDetail(panel,
                     instrumentIndex,
                     sampleIndex,
                     BAE_EDITOR_COMPRESSION_DONT_CHANGE,
                     BAE_EDITOR_OPUS_MODE_AUDIO,
                     false);
}
