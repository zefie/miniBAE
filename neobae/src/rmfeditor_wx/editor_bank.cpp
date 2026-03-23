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

static const char *CompressionTypeName(uint32_t ct, uint32_t subType)
{
    static char combined[48];
    const char *base = NULL;
    const char *bitrate = NULL;

    switch (ct)
    {
        case 0:                                     return "PCM (raw)";
        case FOUR_CHAR('n','o','n','e'):            return "PCM";
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
        /* Vorbis — check sub-type for bitrate */
        case FOUR_CHAR('O','g','g','V'):            base = "Vorbis"; break;
        case FOUR_CHAR('V','O','R','B'):            base = "Vorbis"; break;
        /* Opus — check sub-type for bitrate */
        case FOUR_CHAR('O','g','g','O'):            base = "Opus"; break;
        case FOUR_CHAR('O','P','U','S'):            base = "Opus"; break;
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

    /* If we reached here, base is set for Vorbis/Opus — append bitrate if available */
    bitrate = CompressionSubTypeBitrate(subType);
    if (bitrate)
    {
        snprintf(combined, sizeof(combined), "%s%s", base, bitrate);
        return combined;
    }
    return base;
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
    wxTreeCtrl *instrumentTree;
    wxListCtrl *sampleList;
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
    bool mousePreviewActive;
    std::unordered_map<int, int> keyboardPreviewNotes;   /* keyCode -> MIDI note */

    /* Dirty instrument params (edits not yet saved to bank file) */
    BAERmfEditorInstrumentExtInfo dirtyExtInfo;
    bool hasDirtyExtInfo;

    /* Buttons visible across both tabs */
    wxButton *applyBtn;
    wxButton *stopAllBtn;

    /* Current sample selection for preview */
    uint32_t currentSampleIndex;
    BAERmfEditorBankSampleInfo currentSampleInfo;
    BAERmfEditorBankInstrumentInfo currentInstInfo;
    bool hasSampleSelection;
    void *cachedWaveformData;

    /* State */
    uint32_t currentInstrumentIndex;
    BAERmfEditorInstrumentExtInfo currentExtInfo;
    bool hasInstrument;
};

/* ------------------------------------------------------------------ */
/* Forward declarations                                               */
/* ------------------------------------------------------------------ */

static void PopulateInstrumentTree(BankEditorPanel *bp);
static void PopulateSampleList(BankEditorPanel *bp, uint32_t instrumentIndex);
static void ShowInstrumentDetail(BankEditorPanel *bp, uint32_t instrumentIndex);
static void ShowSampleDetail(BankEditorPanel *bp, uint32_t instrumentIndex, uint32_t sampleIndex);
static void OnInstrumentSelected(BankEditorPanel *bp, wxTreeEvent &event);
static void OnSampleSelected(BankEditorPanel *bp, wxListEvent &event);
static void BuildInstrumentTab(BankEditorPanel *bp);
static void BuildSamplesTab(BankEditorPanel *bp);
static void InvalidateBankPreviewCache(BankEditorPanel *bp);
static void RefreshWaveform(BankEditorPanel *bp);
static void FreeCachedWaveform(BankEditorPanel *bp);
static void StopBankKeyboardPreview(BankEditorPanel *bp);
static void ApplyDirtyParams(BankEditorPanel *bp);
static void StopAllBankNotes(BankEditorPanel *bp);

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
        bp->dirtyExtInfo.hasSampleOverride = TRUE;
        bp->dirtyExtInfo.sampleOverrideIndex = bp->currentSampleIndex;
        bp->dirtyExtInfo.sampleRootKey = sData.rootKey;
        bp->dirtyExtInfo.sampleLowKey = sData.lowKey;
        bp->dirtyExtInfo.sampleHighKey = sData.highKey;
        bp->dirtyExtInfo.sampleSplitVolume = sData.splitVolume;
        bp->dirtyExtInfo.sampleRate = sData.sampleRate;
        bp->dirtyExtInfo.sampleLoopStart = sData.loopStart;
        bp->dirtyExtInfo.sampleLoopEnd = sData.loopEnd;
        fprintf(stderr, "[bank] ApplyDirtyParams: sampleOverride idx=%u rootKey=%u rate=%u loop=%u-%u\n",
                bp->currentSampleIndex, (unsigned)sData.rootKey, sData.sampleRate,
                sData.loopStart, sData.loopEnd);
    } else {
        bp->dirtyExtInfo.hasSampleOverride = FALSE;
    }

    if (bp->dirtyParamsCallback) {
        bp->dirtyParamsCallback(&bp->dirtyExtInfo);
    }

    /* Tear down the preview song so the next note forces a full
     * instrument reload + patch cycle (matching instrument dialog
     * behaviour where the entire RMF/ZMF is regenerated). */
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

static void PopulateInstrumentTree(BankEditorPanel *bp)
{
    uint32_t instCount = 0;
    wxTreeItemId root;

    bp->instrumentTree->DeleteAllItems();
    root = bp->instrumentTree->AddRoot("Instruments");

    if (!bp->bankToken) {
        fprintf(stderr, "[BankEditor] PopulateInstrumentTree: no bank token\n");
        bp->instrumentTree->Expand(root);
        return;
    }

    if (BAERmfEditorBank_GetInstrumentCount(bp->bankToken, &instCount) != BAE_NO_ERROR) {
        fprintf(stderr, "[BankEditor] PopulateInstrumentTree: GetInstrumentCount failed\n");
        bp->instrumentTree->Expand(root);
        return;
    }
    fprintf(stderr, "[BankEditor] PopulateInstrumentTree: %u instruments\n", instCount);

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
}

/* ------------------------------------------------------------------ */
/* Sample list population                                             */
/* ------------------------------------------------------------------ */

static void PopulateSampleList(BankEditorPanel *bp, uint32_t instrumentIndex)
{
    uint32_t sampleCount = 0;

    bp->sampleList->DeleteAllItems();

    if (!bp->bankToken) {
        return;
    }

    if (BAERmfEditorBank_GetInstrumentSampleCount(bp->bankToken, instrumentIndex, &sampleCount) != BAE_NO_ERROR) {
        return;
    }

    for (uint32_t s = 0; s < sampleCount; ++s) {
        BAERmfEditorBankSampleInfo sampleInfo;
        if (BAERmfEditorBank_GetInstrumentSampleInfo(bp->bankToken, instrumentIndex, s, &sampleInfo) != BAE_NO_ERROR) {
            continue;
        }

        wxString sndLabel = wxString::Format("SND %d", static_cast<int>(sampleInfo.sndResourceID));
        wxString keyRange = wxString::Format("%u-%u", sampleInfo.lowKey, sampleInfo.highKey);
        wxString rootKey = (sampleInfo.rootKey == 0)
            ? wxString("60 (default)")
            : wxString::Format("%u", sampleInfo.rootKey);
        wxString rate = wxString::Format("%u Hz", sampleInfo.sampleRate);
        wxString frames = wxString::Format("%u", sampleInfo.frameCount);
        wxString codec = CompressionTypeName((uint32_t)sampleInfo.compressionType,
                                               sampleInfo.compressionSubType);
        wxString bits = wxString::Format("%d-bit %s",
                                         sampleInfo.bitDepth,
                                         sampleInfo.channels == 2 ? "stereo" : "mono");

        long idx = bp->sampleList->InsertItem(bp->sampleList->GetItemCount(), sndLabel);
        bp->sampleList->SetItem(idx, 1, keyRange);
        bp->sampleList->SetItem(idx, 2, rootKey);
        bp->sampleList->SetItem(idx, 3, rate);
        bp->sampleList->SetItem(idx, 4, bits);
        bp->sampleList->SetItem(idx, 5, codec);
        bp->sampleList->SetItem(idx, 6, frames);
    }
}

/* ------------------------------------------------------------------ */
/* Show instrument detail in the Instrument tab                       */
/* ------------------------------------------------------------------ */

static void ShowInstrumentDetail(BankEditorPanel *bp, uint32_t instrumentIndex)
{
    BAERmfEditorInstrumentExtInfo extInfo;
    BAERmfEditorBankInstrumentInfo instInfo;

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

    /* Clear dirty params from previous instrument */
    bp->hasDirtyExtInfo = false;
    if (bp->dirtyParamsCallback) {
        bp->dirtyParamsCallback(nullptr);
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

    /* Use flags from instInfo (bank API provides them separately) */
    bp->currentExtInfo.flags1 = instInfo.flags1;
    bp->currentExtInfo.flags2 = instInfo.flags2;

    {
        wxString instName = instInfo.name[0] ? wxString(instInfo.name) : wxString("(unnamed)");
        instName.Replace("\r", "");
        instName.Replace("\n", " ");
        instName.Trim();
        bp->instParamsPanel->LoadFromExtInfo(bp->currentExtInfo, instName, instInfo.program);
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
        fprintf(stderr, "[bank] RefreshWaveform: skip (bankToken=%p hasSampleSel=%d)\n",
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
    fprintf(stderr, "[bank] RefreshWaveform: inst=%u sample=%u result=%d waveData=%p frames=%u bits=%u ch=%u\n",
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

static void ShowSampleDetail(BankEditorPanel *bp, uint32_t instrumentIndex, uint32_t sampleIndex)
{
    BAERmfEditorBankSampleInfo sampleInfo;

    StopBankKeyboardPreview(bp);

    if (!bp->bankToken) {
        bp->sampleHeaderLabel->SetLabel("No bank loaded.");
        bp->hasSampleSelection = false;
        if (bp->sampleParamsPanel) bp->sampleParamsPanel->ClearUI();
        return;
    }

    if (BAERmfEditorBank_GetInstrumentSampleInfo(bp->bankToken, instrumentIndex, sampleIndex, &sampleInfo) != BAE_NO_ERROR) {
        bp->sampleHeaderLabel->SetLabel("Failed to read sample info.");
        bp->hasSampleSelection = false;
        if (bp->sampleParamsPanel) bp->sampleParamsPanel->ClearUI();
        return;
    }

    bp->currentSampleIndex = sampleIndex;
    bp->currentSampleInfo = sampleInfo;
    bp->hasSampleSelection = true;

    bp->sampleHeaderLabel->SetLabel(wxString::Format(
        "SND Resource: %d  |  Sample %u of instrument",
        static_cast<int>(sampleInfo.sndResourceID),
        static_cast<unsigned>(sampleIndex)));

    /* Populate the shared sample params panel */
    if (bp->sampleParamsPanel) {
        SampleParamsPanelData data;
        data.displayName = wxString::Format("Sample %u", (unsigned)sampleIndex);
        data.rootKey = (sampleInfo.rootKey == 0) ? 60 : sampleInfo.rootKey;
        data.lowKey = sampleInfo.lowKey;
        data.highKey = sampleInfo.highKey;
        data.splitVolume = sampleInfo.splitVolume;
        data.sampleRate = sampleInfo.sampleRate;
        data.waveFrames = sampleInfo.frameCount;
        data.loopStart = sampleInfo.loopStart;
        data.loopEnd = sampleInfo.loopEnd;
        data.compressionType = (BAERmfEditorCompressionType)0;
        data.hasOriginalData = false;
        data.sndStorageType = (BAERmfEditorSndStorageType)0;
        data.opusMode = BAE_EDITOR_OPUS_MODE_AUDIO;
        data.opusRoundTripResample = false;
        data.codecDescription = wxString::FromUTF8(
            CompressionTypeName((uint32_t)sampleInfo.compressionType,
                                sampleInfo.compressionSubType));
        bp->sampleParamsPanel->LoadSample(data);
    }

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
    if (!item.IsOk()) {
        return;
    }

    BankInstrumentItemData *data = dynamic_cast<BankInstrumentItemData *>(
        bp->instrumentTree->GetItemData(item));
    if (!data) {
        /* Group node selected, not an instrument */
        bp->sampleList->DeleteAllItems();
        bp->instHeaderLabel->SetLabel("Select an instrument to view details.");
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
    long sel = event.GetIndex();
    if (sel < 0) {
        return;
    }
    ShowSampleDetail(bp, bp->currentInstrumentIndex, (uint32_t)sel);
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
        ApplyDirtyParams(bp);
    });
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
    bp->sampleParamsPanel->SetOnParameterChanged([bp]() {
        ApplyDirtyParams(bp);
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
    bp->currentInstrumentIndex = 0;
    bp->currentSampleIndex = 0;
    bp->hasDirtyExtInfo = false;
    bp->applyBtn = nullptr;
    bp->stopAllBtn = nullptr;
    memset(&bp->currentExtInfo, 0, sizeof(bp->currentExtInfo));
    memset(&bp->dirtyExtInfo, 0, sizeof(bp->dirtyExtInfo));
    memset(&bp->currentSampleInfo, 0, sizeof(bp->currentSampleInfo));
    memset(&bp->currentInstInfo, 0, sizeof(bp->currentInstInfo));

    bp->panel = new wxPanel(parent);
    bp->splitter = new wxSplitterWindow(bp->panel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                        wxSP_LIVE_UPDATE | wxSP_3D);

    /* Left side: instrument tree + sample list */
    wxPanel *leftPanel = new wxPanel(bp->splitter);
    wxBoxSizer *leftSizer = new wxBoxSizer(wxVERTICAL);

    leftSizer->Add(new wxStaticText(leftPanel, wxID_ANY, "Instruments"), 0, wxALL, 4);
    bp->instrumentTree = new wxTreeCtrl(leftPanel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                        wxTR_HAS_BUTTONS | wxTR_LINES_AT_ROOT | wxTR_SINGLE);
    leftSizer->Add(bp->instrumentTree, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

    leftSizer->Add(new wxStaticText(leftPanel, wxID_ANY, "Samples / Key Splits"), 0, wxLEFT | wxRIGHT, 4);
    bp->sampleList = new wxListCtrl(leftPanel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                    wxLC_REPORT | wxLC_SINGLE_SEL);
    bp->sampleList->SetMinSize(wxSize(-1, 150));
    bp->sampleList->InsertColumn(0, "Sample", wxLIST_FORMAT_LEFT, 80);
    bp->sampleList->InsertColumn(1, "Key Range", wxLIST_FORMAT_LEFT, 80);
    bp->sampleList->InsertColumn(2, "Root", wxLIST_FORMAT_LEFT, 50);
    bp->sampleList->InsertColumn(3, "Rate", wxLIST_FORMAT_LEFT, 80);
    bp->sampleList->InsertColumn(4, "Format", wxLIST_FORMAT_LEFT, 100);
    bp->sampleList->InsertColumn(5, "Codec", wxLIST_FORMAT_LEFT, 70);
    bp->sampleList->InsertColumn(6, "Frames", wxLIST_FORMAT_LEFT, 80);
    leftSizer->Add(bp->sampleList, 0, wxEXPAND | wxALL, 4);
    leftPanel->SetSizer(leftSizer);

    /* Right side: detail notebook with Instrument and Samples tabs */
    wxPanel *rightPanel = new wxPanel(bp->splitter);
    wxBoxSizer *rightSizer = new wxBoxSizer(wxVERTICAL);

    bp->detailNotebook = new wxNotebook(rightPanel, wxID_ANY);
    BuildInstrumentTab(bp);
    BuildSamplesTab(bp);

    rightSizer->Add(bp->detailNotebook, 1, wxEXPAND);

    /* Apply / Stop All Notes buttons — visible for both tabs */
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

    bp->sampleList->Bind(wxEVT_LIST_ITEM_SELECTED, [bp](wxListEvent &event) {
        OnSampleSelected(bp, event);
    });

    /* Keyboard preview: AWSEDFTGYHUJKO keys map to semitones around root key.
     * We use wxEVT_CHAR_HOOK on the top-level panel so that musical keys are
     * intercepted before child controls (tree, list, spin) process them. */
    auto handleKeyDown = [bp](wxKeyEvent &event) {
        int keyCode;
        int semitoneOffset;
        int root;
        int note;
        constexpr int kKeyboardCenterSemitone = 7;

        if (!bp->playCallback || !bp->hasSampleSelection || !bp->hasInstrument) {
            event.Skip();
            return;
        }
        /* Allow normal typing in text/spin fields for non-musical keys,
         * but intercept musical keys even there. */
        keyCode = NormalizeBankKeyCode(event.GetKeyCode());
        semitoneOffset = BankKeyCodeToSemitoneOffset(keyCode);
        if (semitoneOffset < 0) {
            event.Skip();
            return;
        }
        if (bp->keyboardPreviewNotes.find(keyCode) != bp->keyboardPreviewNotes.end()) {
            return;
        }
        root = static_cast<int>(bp->currentSampleInfo.rootKey);
        note = std::clamp(root + (semitoneOffset - kKeyboardCenterSemitone), 0, 127);
        {
            bool isPerc = (bp->currentInstInfo.instID % 256) >= 128;
            bp->playCallback(
                static_cast<unsigned char>(std::clamp<uint32_t>(bp->currentInstInfo.bank, 0, 127)),
                static_cast<unsigned char>(std::clamp<uint32_t>(bp->currentInstInfo.program, 0, 127)),
                note, keyCode, isPerc);
        }
        bp->keyboardPreviewNotes[keyCode] = note;
        if (bp->pianoPanel) {
            std::set<int> activeNotes;
            for (auto const &kv : bp->keyboardPreviewNotes) {
                activeNotes.insert(kv.second);
            }
            bp->pianoPanel->SetExternalPressedNotes(activeNotes);
        }
    };

    auto handleKeyUp = [bp](wxKeyEvent &event) {
        int keyCode = NormalizeBankKeyCode(event.GetKeyCode());
        auto it = bp->keyboardPreviewNotes.find(keyCode);
        if (it == bp->keyboardPreviewNotes.end()) {
            event.Skip();
            return;
        }
        if (bp->stopCallback) {
            bp->stopCallback(keyCode);
        }
        bp->keyboardPreviewNotes.erase(it);
        if (bp->pianoPanel) {
            if (bp->keyboardPreviewNotes.empty()) {
                bp->pianoPanel->ClearExternalPressedNote();
            } else {
                std::set<int> activeNotes;
                for (auto const &kv : bp->keyboardPreviewNotes) {
                    activeNotes.insert(kv.second);
                }
                bp->pianoPanel->SetExternalPressedNotes(activeNotes);
            }
        }
    };

    /* Bind CHAR_HOOK on the panel and on focusable child widgets.  On GTK
     * wxEVT_CHAR_HOOK is delivered only to the focused window, so a
     * panel-level binding alone is not enough to intercept keystrokes
     * when a tree or list control has focus. */
    bp->panel->Bind(wxEVT_CHAR_HOOK, handleKeyDown);
    bp->panel->Bind(wxEVT_KEY_UP, handleKeyUp);
    bp->instrumentTree->Bind(wxEVT_CHAR_HOOK, handleKeyDown);
    bp->instrumentTree->Bind(wxEVT_KEY_UP, handleKeyUp);
    bp->sampleList->Bind(wxEVT_CHAR_HOOK, handleKeyDown);
    bp->sampleList->Bind(wxEVT_KEY_UP, handleKeyUp);
    if (bp->pianoPanel) {
        bp->pianoPanel->Bind(wxEVT_CHAR_HOOK, handleKeyDown);
        bp->pianoPanel->Bind(wxEVT_KEY_UP, handleKeyUp);
    }
    if (bp->sampleParamsPanel) {
        bp->sampleParamsPanel->Bind(wxEVT_CHAR_HOOK, handleKeyDown);
        bp->sampleParamsPanel->Bind(wxEVT_KEY_UP, handleKeyUp);
    }
    if (bp->instPage) {
        bp->instPage->Bind(wxEVT_CHAR_HOOK, handleKeyDown);
        bp->instPage->Bind(wxEVT_KEY_UP, handleKeyUp);
    }
    bp->samplesPage->Bind(wxEVT_CHAR_HOOK, handleKeyDown);
    bp->samplesPage->Bind(wxEVT_KEY_UP, handleKeyUp);

    /* Click-to-defocus: clicking on panel backgrounds moves focus away from
     * text/spin fields so the musical keyboard becomes active. */
    auto defocusClick = [bp](wxMouseEvent &event) {
        bp->panel->SetFocus();
        event.Skip();
    };
    bp->panel->Bind(wxEVT_LEFT_DOWN, defocusClick);
    if (bp->instPage) {
        bp->instPage->Bind(wxEVT_LEFT_DOWN, defocusClick);
    }
    bp->samplesPage->Bind(wxEVT_LEFT_DOWN, defocusClick);

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
    if (panel->dirtyParamsCallback) panel->dirtyParamsCallback(nullptr);
    PopulateInstrumentTree(panel);
    panel->sampleList->DeleteAllItems();
    panel->instHeaderLabel->SetLabel("Select an instrument to view details.");
    panel->sampleHeaderLabel->SetLabel("Select a sample from the list to view details.");
    if (panel->sampleParamsPanel) {
        panel->sampleParamsPanel->ClearUI();
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
    if (panel->dirtyParamsCallback) panel->dirtyParamsCallback(nullptr);
    panel->instrumentTree->DeleteAllItems();
    panel->instrumentTree->AddRoot("Instruments");
    panel->sampleList->DeleteAllItems();
    panel->instHeaderLabel->SetLabel("Open an HSB or ZSB file to begin editing.");
    panel->sampleHeaderLabel->SetLabel("Select a sample from the list to view details.");
    if (panel->sampleParamsPanel) {
        panel->sampleParamsPanel->ClearUI();
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

uint32_t BankEditorPanel_GetCurrentInstrumentIndex(BankEditorPanel *panel)
{
    if (!panel) {
        return 0;
    }
    return panel->currentInstrumentIndex;
}
