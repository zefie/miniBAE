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

#include <stdint.h>

#include <algorithm>
#include <cmath>
#include <climits>
#include <exception>
#include <string>
#include <set>
#include <unordered_map>
#include <utility>

#include <wx/dcbuffer.h>
#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/notebook.h>
#include <wx/numdlg.h>
#include <wx/spinctrl.h>
#include <wx/timer.h>
#include <wx/wx.h>

#include "editor_instrument_ext_dialog.h"
#include "editor_instrument_panels.h"

extern "C" {
#include "X_API.h"
}

namespace {

static unsigned char NormalizeRootKeyForSingleKeySplit(unsigned char rootKey,
                                                        unsigned char lowKey,
                                                        unsigned char highKey) {
    if (rootKey == 0 && lowKey <= 127 && highKey <= 127 && lowKey == highKey) {
        return lowKey;
    }
    return rootKey;
}

// IsOpusCompressionType is now defined in editor_instrument_ext_dialog.h

class InstrumentExtEditorDialog final : public wxDialog {
public:
    using EditedSample = InstrumentEditorEditedSample;
    static constexpr int kMousePreviewTag = -1000000;

    InstrumentExtEditorDialog(wxWindow *parent,
                                                            BAERmfEditorDocument *document,
                              uint32_t primarySampleIndex,
                              BAERmfEditorInstrumentExtInfo const *initialExtInfo,
                              std::function<void(wxString const &)> beginUndoCallback,
                              std::function<void(wxString const &)> commitUndoCallback,
                              std::function<void()> cancelUndoCallback,
                              std::function<void(uint32_t, int, BAESampleInfo const *, int16_t, unsigned char, BAERmfEditorCompressionType, bool, int, BAERmfEditorInstrumentExtInfo const *)> playCallback,
                              std::function<void()> stopCallback,
                              std::function<void(int)> stopTaggedCallback,
                              std::function<void(uint32_t)> invalidatePreviewCallback,
                              std::function<bool(uint32_t, wxString const &)> replaceCallback,
                              std::function<bool(uint32_t, wxString const &)> exportCallback)
        : wxDialog(parent, wxID_ANY, "Edit Instrument", wxDefaultPosition, wxSize(1180, 560),
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          m_document(document),
                    m_instID(0),
          m_extInfo(*initialExtInfo),
          m_extInfoModified(false),
          m_currentLocalIndex(-1),
          m_beginUndoCallback(std::move(beginUndoCallback)),
          m_commitUndoCallback(std::move(commitUndoCallback)),
          m_cancelUndoCallback(std::move(cancelUndoCallback)),
          m_playCallback(std::move(playCallback)),
          m_stopCallback(std::move(stopCallback)),
          m_stopTaggedCallback(std::move(stopTaggedCallback)),
          m_invalidatePreviewCallback(std::move(invalidatePreviewCallback)),
          m_replaceCallback(std::move(replaceCallback)),
          m_exportCallback(std::move(exportCallback)) {

                BAERmfEditorDocument_GetInstIDForSample(m_document, primarySampleIndex, &m_instID);
                if (m_instID != 0) {
                        m_extInfo.instID = m_instID;
                }

        BuildSampleGroup(primarySampleIndex);

        wxBoxSizer *rootSizer = new wxBoxSizer(wxVERTICAL);

        /* Notebook with two tabs */
        m_notebook = new wxNotebook(this, wxID_ANY);
        BuildInstrumentTab();
        BuildSamplesTab();

        rootSizer->Add(m_notebook, 1, wxEXPAND | wxALL, 5);

        /* Piano keyboard (always visible) */
        m_pianoPanel = new PianoKeyboardPanel(this,
            [this](int note) {
                if (!m_playCallback || m_sampleIndices.empty()) return;
                SaveCurrentSampleFromUI();
                SaveExtInfoFromUI();
                /* Find the sample whose key range contains the played note */
                int best = -1;
                for (size_t i = 0; i < m_samples.size(); i++) {
                    if (note >= (int)m_samples[i].lowKey && note <= (int)m_samples[i].highKey) {
                        best = (int)i;
                        break;
                    }
                }
                if (best < 0) return;
                m_playCallback(m_sampleIndices[(size_t)best], note,
                               &m_samples[(size_t)best].sampleInfo,
                               m_samples[(size_t)best].splitVolume,
                               m_samples[(size_t)best].rootKey,
                               m_samples[(size_t)best].compressionType,
                               m_samples[(size_t)best].opusRoundTripResample,
                               kMousePreviewTag,
                               &m_extInfo);
                m_mousePreviewActive = true;
            },
            [this]() {
                if (m_mousePreviewActive && m_stopTaggedCallback) {
                    m_stopTaggedCallback(kMousePreviewTag);
                } else if (m_stopCallback) {
                    m_stopCallback();
                }
                m_mousePreviewActive = false;
            });
        rootSizer->Add(m_pianoPanel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);

        /* Apply / OK / Cancel */
        {
            wxBoxSizer *btnSizer = new wxBoxSizer(wxHORIZONTAL);
            btnSizer->AddStretchSpacer();
            m_applyButton = new wxButton(this, wxID_APPLY, "Apply");
            btnSizer->Add(m_applyButton, 0, wxRIGHT, 5);
            btnSizer->Add(new wxButton(this, wxID_OK, "OK"), 0, wxRIGHT, 5);
            btnSizer->Add(new wxButton(this, wxID_CANCEL, "Cancel"), 0);
            rootSizer->Add(btnSizer, 0, wxEXPAND | wxALL, 10);
        }

        SetSizerAndFit(rootSizer);
        SetSize(wxSize(1180, 560));

        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &evt) {
            if (m_pianoPanel) {
                m_pianoPanel->ForceStop();
            }
            StopKeyboardPreviewIfActive();
            evt.Skip();
        });

        Bind(wxEVT_BUTTON, &InstrumentExtEditorDialog::OnApply, this, wxID_APPLY);
        Bind(wxEVT_BUTTON, &InstrumentExtEditorDialog::OnOk, this, wxID_OK);
        Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent &event) {
            StopKeyboardPreviewIfActive();
            event.Skip();
        });
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent &event) {
            StopKeyboardPreviewIfActive();
            event.Skip();
        });
        Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent &event) {
            int keyCode = event.GetKeyCode();
            bool ctrlDown = event.ControlDown() || event.CmdDown();
            wxWindow *focus = wxWindow::FindFocus();

            if (keyCode == WXK_ESCAPE && IsTextEditingFocus(focus)) {
                FocusKeyboardPreviewTarget();
                return;
            }

            if (ctrlDown && !event.ShiftDown() && !event.AltDown() &&
                (keyCode == 'Z' || keyCode == 'z')) {
                wxTextCtrl *textCtrl = wxDynamicCast(focus, wxTextCtrl);
                if (textCtrl && textCtrl->CanUndo()) {
                    textCtrl->Undo();
                    return;
                }
                wxCommandEvent cmd(wxEVT_MENU, wxID_UNDO);
                wxWindow *target = wxGetTopLevelParent(this);
                if (target && target != this) {
                    if (target->GetEventHandler()->ProcessEvent(cmd)) {
                        return;
                    }
                }
            }
            if (ctrlDown && !event.AltDown() &&
                (keyCode == 'Y' || keyCode == 'y' ||
                 (event.ShiftDown() && (keyCode == 'Z' || keyCode == 'z')))) {
                wxTextCtrl *textCtrl = wxDynamicCast(focus, wxTextCtrl);
                if (textCtrl && textCtrl->CanRedo()) {
                    textCtrl->Redo();
                    return;
                }
                wxCommandEvent cmd(wxEVT_MENU, wxID_REDO);
                wxWindow *target = wxGetTopLevelParent(this);
                if (target && target != this) {
                    if (target->GetEventHandler()->ProcessEvent(cmd)) {
                        return;
                    }
                }
            }
            event.Skip();
        });
        BindKeyboardReleaseHandlers(this);
        BindBackgroundFocusHandlers(this);
        if (m_notebook) {
            m_notebook->Bind(wxEVT_NOTEBOOK_PAGE_CHANGED, [this](wxBookCtrlEvent &event) {
                StopKeyboardPreviewIfActive();
                event.Skip();
            });
        }
        Bind(wxEVT_SHOW, [this](wxShowEvent &event) {
            if (event.IsShown() && m_notebook) {
                m_notebook->SetFocus();
            }
            event.Skip();
        });

        /* Load initial sample data; center piano on C4 regardless of root key. */
        if (!m_samples.empty()) {
            m_splitChoice->SetSelection(0);
            LoadLocalSample(0);
            if (m_pianoPanel) {
                m_pianoPanel->CenterOnNote(60);  /* C4 */
            }
        }

        if (m_sampleParamsPanel) {
            m_sampleParamsPanel->SetOnParameterChanged([this]() {
                RefreshPianoRangeFromRootUI();
                RefreshWaveformIfCodecChanged();
            });
        }
    }

    BAERmfEditorInstrumentExtInfo const &GetExtInfo() {
        SaveExtInfoFromUI();
        return m_extInfo;
    }

    std::vector<uint32_t> const &GetDeletedSampleIndices() const { return m_deletedSampleIndices; }

    std::vector<uint32_t> const &GetSampleIndices() const { return m_sampleIndices; }

    std::vector<EditedSample> const &GetEditedSamples() {
        SaveCurrentSampleFromUI();
        return m_samples;
    }

private:
    BAERmfEditorDocument *m_document;
    uint32_t m_instID;
    BAERmfEditorInstrumentExtInfo m_extInfo;
    bool m_extInfoModified;
    std::vector<uint32_t> m_deletedSampleIndices;
    std::vector<uint32_t> m_sampleIndices;
    std::vector<EditedSample> m_samples;
    int m_currentLocalIndex;
    std::function<void(wxString const &)> m_beginUndoCallback;
    std::function<void(wxString const &)> m_commitUndoCallback;
    std::function<void()> m_cancelUndoCallback;
    std::function<void(uint32_t, int, BAESampleInfo const *, int16_t, unsigned char, BAERmfEditorCompressionType, bool, int, BAERmfEditorInstrumentExtInfo const *)> m_playCallback;
    std::function<void()> m_stopCallback;
    std::function<void(int)> m_stopTaggedCallback;
    std::function<void(uint32_t)> m_invalidatePreviewCallback;
    std::function<bool(uint32_t, wxString const &)> m_replaceCallback;
    std::function<bool(uint32_t, wxString const &)> m_exportCallback;

    wxNotebook *m_notebook;
    PianoKeyboardPanel *m_pianoPanel;
    wxButton *m_applyButton;
    bool m_loadingUiValues = false;
    bool m_inInstantApply = false;

    /* Instrument tab controls */
    InstrumentParamsPanel *m_instParamsPanel;

    /* Samples tab controls */
    wxChoice *m_splitChoice;
    SampleParamsPanel *m_sampleParamsPanel;
    std::unordered_map<int, int> m_keyboardPreviewNotes;
    bool m_mousePreviewActive = false;
    int m_lastPreviewedCodecIdx = -1;
    int m_lastPreviewedBitrateIdx = -1;
    std::vector<unsigned char> m_compressedPreviewBuffer;

    static int NormalizeAsciiKeyCode(int keyCode) {
        if (keyCode >= 'a' && keyCode <= 'z') {
            return keyCode - ('a' - 'A');
        }
        return keyCode;
    }

    static int KeyCodeToSemitoneOffset(int keyCode) {
        switch (NormalizeAsciiKeyCode(keyCode)) {
            /* Match GUI keyboard sequence: a w s e d f t g y h u j k o */
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

    static bool IsTextEditingFocus(wxWindow *focus) {
        return wxDynamicCast(focus, wxTextCtrl) != nullptr ||
               wxDynamicCast(focus, wxSpinCtrl) != nullptr ||
               wxDynamicCast(focus, wxSpinCtrlDouble) != nullptr;
    }

    void FocusKeyboardPreviewTarget() {
        if (m_pianoPanel && m_pianoPanel->IsShown()) {
            m_pianoPanel->SetFocus();
            return;
        }
        if (m_notebook) {
            m_notebook->SetFocus();
            return;
        }
        SetFocus();
    }

    bool ShouldReturnFocusFromClickTarget(wxWindow *target) const {
        if (!target) {
            return false;
        }
        if (target == m_pianoPanel || target == m_sampleParamsPanel) {
            return false;
        }
        if (wxDynamicCast(target, wxTextCtrl) != nullptr ||
            wxDynamicCast(target, wxSpinCtrl) != nullptr ||
            wxDynamicCast(target, wxSpinCtrlDouble) != nullptr ||
            wxDynamicCast(target, wxChoice) != nullptr ||
            wxDynamicCast(target, wxCheckBox) != nullptr ||
            wxDynamicCast(target, wxButton) != nullptr ||
            wxDynamicCast(target, wxNotebook) != nullptr) {
            return false;
        }
        return target == this ||
               wxDynamicCast(target, wxPanel) != nullptr ||
               wxDynamicCast(target, wxStaticText) != nullptr ||
               wxDynamicCast(target, wxStaticBox) != nullptr;
    }

    void HandleBackgroundLeftDown(wxMouseEvent &event) {
        wxWindow *target = wxDynamicCast((wxObject *)event.GetEventObject(), wxWindow);
        if (ShouldReturnFocusFromClickTarget(target)) {
            FocusKeyboardPreviewTarget();
        }
        event.Skip();
    }

    void BindBackgroundFocusHandlers(wxWindow *window) {
        wxWindowList::compatibility_iterator node;

        if (!window) {
            return;
        }
        window->Bind(wxEVT_LEFT_DOWN, &InstrumentExtEditorDialog::HandleBackgroundLeftDown, this);
        node = window->GetChildren().GetFirst();
        while (node) {
            BindBackgroundFocusHandlers(node->GetData());
            node = node->GetNext();
        }
    }

    void BindKeyboardReleaseHandlers(wxWindow *window) {
        wxWindowList::compatibility_iterator node;

        if (!window) {
            return;
        }
        window->Bind(wxEVT_KEY_DOWN, &InstrumentExtEditorDialog::HandleKeyboardPreviewKeyDown, this);
        window->Bind(wxEVT_KEY_UP, &InstrumentExtEditorDialog::HandleKeyboardPreviewKeyUp, this);
        node = window->GetChildren().GetFirst();
        while (node) {
            BindKeyboardReleaseHandlers(node->GetData());
            node = node->GetNext();
        }
    }

    void InvalidatePreviewCache(uint32_t reasonFlags = kInstrumentPreviewInvalidate_All) {
        if (m_invalidatePreviewCallback) {
            m_invalidatePreviewCallback(reasonFlags);
        }
    }

    bool TriggerPreviewNote(int note, int previewTag) {
        int best = -1;

        for (size_t i = 0; i < m_samples.size(); i++) {
            if (note >= (int)m_samples[i].lowKey && note <= (int)m_samples[i].highKey) {
                best = (int)i;
                break;
            }
        }
        if (best < 0) {
            return false;
        }

        m_playCallback(m_sampleIndices[(size_t)best], note,
                       &m_samples[(size_t)best].sampleInfo,
                       m_samples[(size_t)best].splitVolume,
                       m_samples[(size_t)best].rootKey,
                       m_samples[(size_t)best].compressionType,
                       m_samples[(size_t)best].opusRoundTripResample,
                       previewTag,
                       &m_extInfo);
        return true;
    }

    void UpdateKeyboardPreviewHighlight() {
        if (!m_pianoPanel) {
            return;
        }
        if (m_keyboardPreviewNotes.empty()) {
            m_pianoPanel->ClearExternalPressedNote();
            return;
        }
        std::set<int> activeNotes;
        for (auto const &kv : m_keyboardPreviewNotes) {
            activeNotes.insert(kv.second);
        }
        m_pianoPanel->SetExternalPressedNotes(activeNotes);
    }

    void StopKeyboardPreviewIfActive() {
        if (!m_keyboardPreviewNotes.empty()) {
            if (m_stopTaggedCallback) {
                for (auto const &entry : m_keyboardPreviewNotes) {
                    m_stopTaggedCallback(entry.first);
                }
            } else if (m_stopCallback) {
                m_stopCallback();
            }
        }
        m_keyboardPreviewNotes.clear();
        if (m_mousePreviewActive) {
            if (m_stopTaggedCallback) {
                m_stopTaggedCallback(kMousePreviewTag);
            } else if (m_stopCallback) {
                m_stopCallback();
            }
            m_mousePreviewActive = false;
        }
        if (m_pianoPanel) {
            m_pianoPanel->ClearExternalPressedNote();
        }
    }

    bool StartKeyboardPreviewForKey(int keyCode) {
        int semitoneOffset;
        int root;
        int note;
        constexpr int kKeyboardCenterSemitone = 7;

        if (!m_playCallback || m_sampleIndices.empty() || IsTextEditingFocus(wxWindow::FindFocus())) {
            return false;
        }
        keyCode = NormalizeAsciiKeyCode(keyCode);
        semitoneOffset = KeyCodeToSemitoneOffset(keyCode);
        if (semitoneOffset < 0) {
            return false;
        }
        if (m_keyboardPreviewNotes.find(keyCode) != m_keyboardPreviewNotes.end()) {
            return true;
        }

        root = m_sampleParamsPanel ? m_sampleParamsPanel->GetRootKey() : 60;
        note = std::clamp(root + (semitoneOffset - kKeyboardCenterSemitone), 0, 127);

        SaveCurrentSampleFromUI();
        SaveExtInfoFromUI();
        if (!TriggerPreviewNote(note, keyCode)) {
    #if _DEBUG
            BAE_PRINTF( "[nbstudio] key-preview down key=%d note=%d trigger=miss active=%zu\n",
                keyCode,
                note,
                m_keyboardPreviewNotes.size());
    #endif
            return true;
        }

        m_keyboardPreviewNotes[keyCode] = note;
    #if _DEBUG
        BAE_PRINTF( "[nbstudio] key-preview down key=%d note=%d trigger=ok active=%zu\n",
            keyCode,
            note,
            m_keyboardPreviewNotes.size());
    #endif
        UpdateKeyboardPreviewHighlight();
        return true;
    }

    void HandleKeyboardPreviewKeyDown(wxKeyEvent &event) {
        int keyCode = event.GetKeyCode();
        bool ctrlDown = event.ControlDown() || event.CmdDown();
        if (!ctrlDown && !event.AltDown()) {
            if (StartKeyboardPreviewForKey(keyCode)) {
                event.Skip();
                return;
            }
        }
        event.Skip();
    }

    void HandleKeyboardPreviewKeyUp(wxKeyEvent &event) {
        int keyCode = NormalizeAsciiKeyCode(event.GetKeyCode());
        auto found = m_keyboardPreviewNotes.find(keyCode);

        if (found == m_keyboardPreviewNotes.end()) {
#if _DEBUG
            BAE_PRINTF( "[nbstudio] key-preview up key=%d state=missing active=%zu\n",
                    keyCode,
                    m_keyboardPreviewNotes.size());
#endif
            event.Skip();
            return;
        }

        m_keyboardPreviewNotes.erase(found);
#if _DEBUG
        BAE_PRINTF( "[nbstudio] key-preview up key=%d state=found active=%zu\n",
                keyCode,
                m_keyboardPreviewNotes.size());
#endif

        if (m_stopTaggedCallback) {
            m_stopTaggedCallback(keyCode);
        } else if (m_stopCallback) {
            m_stopCallback();
            for (auto const &entry : m_keyboardPreviewNotes) {
                TriggerPreviewNote(entry.second, entry.first);
            }
        }

        UpdateKeyboardPreviewHighlight();
    }

    /* ---- Instrument Tab ---- */
    void BuildInstrumentTab() {
        wxString instName;
        int program;

        if (m_extInfo.displayName && m_extInfo.displayName[0]) {
            instName = SanitizeDisplayName(wxString::FromUTF8(m_extInfo.displayName));
        } else if (!m_samples.empty()) {
            instName = m_samples[0].displayName;
        }
        program = m_samples.empty() ? 0 : m_samples[0].program;

        m_instParamsPanel = new InstrumentParamsPanel(m_notebook);
        m_instParamsPanel->LoadFromExtInfo(m_extInfo, instName, program);
        m_instParamsPanel->SetOnParameterChanged([this]() {
        });
        m_notebook->AddPage(m_instParamsPanel, "Instrument");
    }

    wxString GetInstrumentDisplayName() const {
        return SanitizeDisplayName(m_instParamsPanel->GetInstName());
    }

    void SaveExtInfoFromUI() {
        if (m_instID != 0) {
            m_extInfo.instID = m_instID;
        }
        m_instParamsPanel->SaveToExtInfo(m_extInfo);
        m_extInfoModified = true;
    }

    bool ApplyInstrumentChanges(bool showError) {
        BAERmfEditorInstrumentExtInfo infoToWrite;
        BAEResult result;
        wxString instName;

        SaveExtInfoFromUI();
        if (!m_document) {
            if (showError) {
                wxMessageBox("Failed to access instrument document.",
                             "Edit Instrument", wxOK | wxICON_ERROR, this);
            }
            return false;
        }
        infoToWrite = m_extInfo;
        instName = GetInstrumentDisplayName();
        wxScopedCharBuffer instNameUtf8 = instName.utf8_str();
        infoToWrite.displayName = instNameUtf8.data();
        result = BAERmfEditorDocument_SetInstrumentExtInfo(m_document, infoToWrite.instID, &infoToWrite);
        if (result == BAE_NO_ERROR) {
            return true;
        }
        if (showError) {
            wxMessageBox(wxString::Format("Failed to apply instrument extended data (error %d).",
                                          static_cast<int>(result)),
                         "Edit Instrument", wxOK | wxICON_ERROR, this);
        }
        return false;
    }

    /* ---- Samples Tab ---- */
    void BuildSamplesTab() {
        wxPanel *page = new wxPanel(m_notebook);
        wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);

        /* Sample selector */
        {
            wxBoxSizer *row = new wxBoxSizer(wxHORIZONTAL);
            row->Add(new wxStaticText(page, wxID_ANY, "Sample"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
            m_splitChoice = new wxChoice(page, wxID_ANY);
            for (size_t i = 0; i < m_samples.size(); i++) {
                m_splitChoice->Append(BuildSplitLabel((int)i));
            }
            row->Add(m_splitChoice, 1, wxEXPAND);
            sizer->Add(row, 0, wxEXPAND | wxALL, 8);
        }

        /* Shared sample params panel */
        m_sampleParamsPanel = new SampleParamsPanel(page);
        m_sampleParamsPanel->SetWriteMode(true, true, true, true);
        m_sampleParamsPanel->SetOnParameterChanged([this]() {
            RefreshPianoRangeFromRootUI();
            RefreshWaveformIfCodecChanged();
        });
        m_sampleParamsPanel->SetOnLoopChanged([this]() {
            CommitLoopChangeToUndo();
        });
        m_sampleParamsPanel->SetOnNewSample([this]() { OnNewSample(); });
        m_sampleParamsPanel->SetOnDeleteSample([this]() { OnDeleteSample(); });
        m_sampleParamsPanel->SetOnReplaceSample([this]() { OnReplaceSample(); });
        m_sampleParamsPanel->SetOnExportSample([this]() { OnExportSample(); });
        sizer->Add(m_sampleParamsPanel, 1, wxEXPAND);

        page->SetSizerAndFit(sizer);
        m_notebook->AddPage(page, "Samples");

        /* Bind split selector */
        m_splitChoice->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
            SaveCurrentSampleFromUI();
            int sel = m_splitChoice->GetSelection();
            if (sel >= 0) LoadLocalSample(sel);
            
        });
        UpdateSampleButtonState();
    }

    void OnNewSample() {
        SaveCurrentSampleFromUI();

        unsigned char prog = m_samples.empty() ? 0 : m_samples[0].program;
        if (m_instParamsPanel) prog = (unsigned char)m_instParamsPanel->GetProgram();

        long root = wxGetNumberFromUser("Root key (0-127)", "Root Key", "New Sample", 60, 0, 127, this);
        if (root < 0) return;

        EditedSample s;
        s.displayName = "New Sample";
        s.sourcePath = wxString();
        s.program = prog;
        s.rootKey = (unsigned char)root;
        s.lowKey = 0;
        s.highKey = 127;
        s.splitVolume = 0;
        memset(&s.sampleInfo, 0, sizeof(s.sampleInfo));
        s.sampleInfo.sampledRate = (BAE_UNSIGNED_FIXED)(22050u << 16);
        s.compressionType = BAE_EDITOR_COMPRESSION_PCM;
        s.opusMode = BAE_EDITOR_OPUS_MODE_AUDIO;
        s.opusRoundTripResample = false;
        s.sndStorageType = BAE_EDITOR_SND_STORAGE_ESND;
        s.hasOriginalData = false;

        m_samples.push_back(s);
        m_sampleIndices.push_back(static_cast<uint32_t>(-1));

        int newIdx = (int)m_samples.size() - 1;
        m_splitChoice->Append(BuildSplitLabel(newIdx));
        m_splitChoice->SetSelection(newIdx);
        LoadLocalSample(newIdx);
        UpdateSampleButtonState();
    }

    void OnDeleteSample() {
        int localIndex;
        uint32_t sampleIndex;
        wxString confirmMessage;

        if (m_samples.size() <= 1) {
            wxMessageBox("Cannot delete the last sample from this editor. Use Delete Instrument instead.",
                         "Delete Sample",
                         wxOK | wxICON_INFORMATION,
                         this);
            return;
        }
        if (m_currentLocalIndex < 0 || m_currentLocalIndex >= (int)m_samples.size()) {
            return;
        }

        SaveCurrentSampleFromUI();
        localIndex = m_currentLocalIndex;
        sampleIndex = m_sampleIndices[(size_t)localIndex];
        confirmMessage = wxString::Format("Delete sample '%s' (%d-%d)?",
                                          m_samples[(size_t)localIndex].displayName,
                                          (int)m_samples[(size_t)localIndex].lowKey,
                                          (int)m_samples[(size_t)localIndex].highKey);
        if (wxMessageBox(confirmMessage, "Delete Sample", wxYES_NO | wxICON_WARNING, this) != wxYES) {
            return;
        }

        if (sampleIndex != static_cast<uint32_t>(-1)) {
            m_deletedSampleIndices.push_back(sampleIndex);
        }
        m_samples.erase(m_samples.begin() + localIndex);
        m_sampleIndices.erase(m_sampleIndices.begin() + localIndex);
        m_splitChoice->Delete(localIndex);

        if (localIndex >= (int)m_samples.size()) {
            localIndex = (int)m_samples.size() - 1;
        }
        m_splitChoice->SetSelection(localIndex);
        LoadLocalSample(localIndex);
        UpdateSampleButtonState();
    }

    /* ---- Sample data management (mirrors existing dialog logic) ---- */

    static wxString SanitizeDisplayName(wxString const &name) {
        wxString clean;
        for (wxUniChar ch : name) {
            if (ch >= 32 && ch != 127) clean.Append(ch);
        }
        return clean.IsEmpty() ? wxString("Embedded Sample") : clean;
    }

    void UpdateSampleButtonState() {
        if (m_sampleParamsPanel) {
            m_sampleParamsPanel->SetDeleteEnabled(m_samples.size() > 1 &&
                                                  m_currentLocalIndex >= 0 &&
                                                  m_currentLocalIndex < (int)m_samples.size());
        }
    }

    void BuildSampleGroup(uint32_t primarySampleIndex) {
        uint32_t sampleCount = 0;
        uint32_t primaryInstID = 0;
        BAERmfEditorSampleInfo primaryInfo;
        if (!m_document ||
            BAERmfEditorDocument_GetSampleInfo(m_document, primarySampleIndex, &primaryInfo) != BAE_NO_ERROR ||
            BAERmfEditorDocument_GetSampleCount(m_document, &sampleCount) != BAE_NO_ERROR) return;

        BAERmfEditorDocument_GetInstIDForSample(m_document, primarySampleIndex, &primaryInstID);

        for (uint32_t i = 0; i < sampleCount; i++) {
            BAERmfEditorSampleInfo info;
            uint32_t instID = 0;
            if (BAERmfEditorDocument_GetSampleInfo(m_document, i, &info) != BAE_NO_ERROR) continue;
            BAERmfEditorDocument_GetInstIDForSample(m_document, i, &instID);
            if ((primaryInstID != 0 && instID == primaryInstID && info.program == primaryInfo.program) ||
                (primaryInstID == 0 && info.program == primaryInfo.program)) {
                EditedSample edited;
                edited.displayName = SanitizeDisplayName(info.displayName ? wxString::FromUTF8(info.displayName) : wxString());
                edited.sourcePath = info.sourcePath ? wxString::FromUTF8(info.sourcePath) : wxString();
                edited.program = info.program;
                edited.rootKey = NormalizeRootKeyForSingleKeySplit(info.rootKey, info.lowKey, info.highKey);
                edited.lowKey = info.lowKey;
                edited.highKey = info.highKey;
                edited.splitVolume = info.splitVolume;
                edited.sampleInfo = info.sampleInfo;
                edited.compressionType = info.compressionType;
                edited.hasOriginalData = (info.hasOriginalData == TRUE);
                edited.sndStorageType = info.sndStorageType;
                edited.opusMode = info.opusMode;
                edited.opusRoundTripResample = (info.opusRoundTripResample == TRUE);
                m_sampleIndices.push_back(i);
                m_samples.push_back(edited);
            }
        }
    }

    wxString BuildSplitLabel(int localIndex) const {
        EditedSample const &s = m_samples[(size_t)localIndex];
        return wxString::Format("%d-%d: %s", (int)s.lowKey, (int)s.highKey, s.displayName);
    }

    void SaveCurrentSampleFromUI() {
        if (m_currentLocalIndex < 0 || m_currentLocalIndex >= (int)m_samples.size()) return;
        EditedSample &s = m_samples[(size_t)m_currentLocalIndex];
        SampleParamsPanelData data;
        m_sampleParamsPanel->SaveToData(data);
        s.displayName = SanitizeDisplayName(data.displayName);
        s.rootKey = data.rootKey;
        s.lowKey = data.lowKey;
        s.highKey = data.highKey;
        s.sampleInfo.sampledRate = (BAE_UNSIGNED_FIXED)(data.sampleRate << 16);
        s.splitVolume = data.splitVolume;
        s.program = (unsigned char)m_instParamsPanel->GetProgram();
        BAERmfEditorCompressionType chosen = data.compressionType;
        if (chosen == BAE_EDITOR_COMPRESSION_DONT_CHANGE && !s.hasOriginalData) chosen = BAE_EDITOR_COMPRESSION_PCM;
        s.compressionType = chosen;
        s.opusRoundTripResample = data.opusRoundTripResample;
        s.opusMode = data.opusMode;
        s.sndStorageType = data.sndStorageType;
        s.sampleInfo.startLoop = data.loopStart;
        s.sampleInfo.endLoop = data.loopEnd;

        if (m_splitChoice && m_currentLocalIndex < (int)m_splitChoice->GetCount()) {
            m_splitChoice->SetString(m_currentLocalIndex, BuildSplitLabel(m_currentLocalIndex));
        }
    }

    void LoadLocalSample(int localIndex) {
        if (localIndex < 0 || localIndex >= (int)m_samples.size()) return;
        m_loadingUiValues = true;
        m_currentLocalIndex = localIndex;
        EditedSample const &s = m_samples[(size_t)localIndex];
        if (m_pianoPanel) {
            m_pianoPanel->CenterOnNote((int)s.rootKey);
        }
        SampleParamsPanelData data;
        data.displayName = s.displayName;
        data.rootKey = s.rootKey;
        data.lowKey = s.lowKey;
        data.highKey = s.highKey;
        data.sampleRate = (uint32_t)(s.sampleInfo.sampledRate >> 16);
        data.splitVolume = s.splitVolume;
        data.waveFrames = s.sampleInfo.waveFrames;
        data.loopStart = s.sampleInfo.startLoop;
        data.loopEnd = s.sampleInfo.endLoop;
        data.compressionType = s.compressionType;
        data.hasOriginalData = s.hasOriginalData;
        data.sndStorageType = s.sndStorageType;
        data.opusMode = s.opusMode;
        data.opusRoundTripResample = s.opusRoundTripResample;
        {
            char codecBuf[64] = {};
            uint32_t si = m_sampleIndices[(size_t)localIndex];
            if (BAERmfEditorDocument_GetSampleCodecDescription(m_document, si, codecBuf, sizeof(codecBuf)) == BAE_NO_ERROR) {
                data.codecDescription = wxString::FromUTF8(codecBuf);
            }
        }
        m_sampleParamsPanel->LoadSample(data);
        m_lastPreviewedCodecIdx = -1;
        m_lastPreviewedBitrateIdx = -1;
        RefreshWaveform();
        UpdateSampleButtonState();
        m_loadingUiValues = false;
    }

    void RefreshPianoRangeFromRootUI() {
        if (!m_pianoPanel || !m_sampleParamsPanel) return;
        m_pianoPanel->CenterOnNote(m_sampleParamsPanel->GetRootKey());
    }

    void CommitLoopChangeToUndo() {
        if (m_loadingUiValues || m_inInstantApply) {
            return;
        }
        /* Keep loop edits staged in-dialog. They should only commit to the
         * document on Apply/OK so Cancel can discard post-Apply changes. */
        SaveCurrentSampleFromUI();
    }

    void RefreshWaveform() {
        if (m_currentLocalIndex < 0 || m_currentLocalIndex >= (int)m_sampleIndices.size()) return;
        uint32_t si = m_sampleIndices[(size_t)m_currentLocalIndex];
        uint32_t displayedFrameCount = 0;

        if (m_sampleParamsPanel) {
            m_lastPreviewedCodecIdx   = m_sampleParamsPanel->GetCodecSelection();
            m_lastPreviewedBitrateIdx = m_sampleParamsPanel->GetBitrateSelection();
        }

        /* Try to show the post-encode waveform for compressed codecs.
         * Save the document to RMF in memory (which encodes), reload
         * (which decodes), then grab the decoded waveform. */
        bool previewShown = false;
        if (m_currentLocalIndex < (int)m_samples.size()) {
            EditedSample const &s = m_samples[(size_t)m_currentLocalIndex];
            BAERmfEditorCompressionType ct = s.compressionType;
            if (ct != BAE_EDITOR_COMPRESSION_PCM && ct != BAE_EDITOR_COMPRESSION_DONT_CHANGE) {
                unsigned char *rmfData = nullptr;
                uint32_t rmfSize = 0;
                if (BAERmfEditorDocument_SaveAsRmfToMemory(m_document, FALSE, &rmfData, &rmfSize) == BAE_NO_ERROR
                    && rmfData && rmfSize > 0) {
                    BAERmfEditorDocument *tempDoc = BAERmfEditorDocument_LoadFromMemory(rmfData, rmfSize, BAE_RMF);
                    XDisposePtr((XPTR)rmfData);
                    if (tempDoc) {
                        void const *waveData = nullptr;
                        uint32_t frameCount = 0;
                        uint16_t bitSize = 16, channels = 1;
                        BAE_UNSIGNED_FIXED sampleRate = 0;
                        if (BAERmfEditorDocument_GetSampleWaveformData(tempDoc, si, &waveData,
                                &frameCount, &bitSize, &channels, &sampleRate) == BAE_NO_ERROR
                            && waveData && frameCount > 0) {
                            uint32_t bytesPerFrame = (static_cast<uint32_t>(bitSize) / 8U) * static_cast<uint32_t>(channels);
                            size_t totalBytes = static_cast<size_t>(frameCount) * bytesPerFrame;
                            m_compressedPreviewBuffer.assign(
                                static_cast<unsigned char const *>(waveData),
                                static_cast<unsigned char const *>(waveData) + totalBytes);
                            displayedFrameCount = frameCount;
                            previewShown = true;
                        }
                        BAERmfEditorDocument_Delete(tempDoc);
                        if (previewShown) {
                            m_sampleParamsPanel->SetWaveform(m_compressedPreviewBuffer.data(), frameCount, bitSize, channels);
                        }
                    }
                }
            }
        }

        /* Fall back to the raw decoded waveform from the document */
        if (!previewShown) {
            void const *waveData = nullptr;
            uint32_t frameCount = 0;
            uint16_t bitSize = 16, channels = 1;
            BAE_UNSIGNED_FIXED sampleRate = 0;
            if (BAERmfEditorDocument_GetSampleWaveformData(m_document, si, &waveData, &frameCount, &bitSize, &channels, &sampleRate) == BAE_NO_ERROR) {
                m_sampleParamsPanel->SetWaveform(waveData, frameCount, bitSize, channels);
                displayedFrameCount = frameCount;
            }
        }

        if (m_currentLocalIndex >= 0 && m_currentLocalIndex < (int)m_samples.size()) {
            EditedSample const &s = m_samples[(size_t)m_currentLocalIndex];
            bool isMpegCompression = false;
            switch (s.compressionType) {
                case BAE_EDITOR_COMPRESSION_MP3_32K:
                case BAE_EDITOR_COMPRESSION_MP3_48K:
                case BAE_EDITOR_COMPRESSION_MP3_64K:
                case BAE_EDITOR_COMPRESSION_MP3_96K:
                case BAE_EDITOR_COMPRESSION_MP3_128K:
                case BAE_EDITOR_COMPRESSION_MP3_192K:
                case BAE_EDITOR_COMPRESSION_MP3_256K:
                case BAE_EDITOR_COMPRESSION_MP3_320K:
                    isMpegCompression = true;
                    break;
                default:
                    break;
            }
            bool mapLoopToDisplay = previewShown &&
                                    ((IsOpusCompressionType(s.compressionType) && !s.opusRoundTripResample) ||
                                     isMpegCompression);

            m_sampleParamsPanel->SetLoopDisplayFromSource(s.sampleInfo.startLoop,
                                                           s.sampleInfo.endLoop,
                                                           s.sampleInfo.waveFrames,
                                                           displayedFrameCount,
                                                           mapLoopToDisplay);
        }
    }

    void RefreshWaveformIfCodecChanged() {
        if (!m_sampleParamsPanel || m_loadingUiValues) return;
        int codecIdx   = m_sampleParamsPanel->GetCodecSelection();
        int bitrateIdx = m_sampleParamsPanel->GetBitrateSelection();
        if (codecIdx != m_lastPreviewedCodecIdx || bitrateIdx != m_lastPreviewedBitrateIdx) {
            RefreshWaveform();
        }
    }

    void OnReplaceSample() {
        if (!m_replaceCallback || m_currentLocalIndex < 0 || m_currentLocalIndex >= (int)m_sampleIndices.size()) return;
        wxFileDialog dialog(this, "Replace Embedded Sample", wxEmptyString, wxEmptyString,
            "Supported audio (*.wav;*.aif;*.aiff;*.mp3;*.ogg;*.flac;*.opus)|*.wav;*.aif;*.aiff;*.mp3;*.ogg;*.flac;*.opus|All files (*.*)|*.*",
            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK) return;
        uint32_t si = m_sampleIndices[(size_t)m_currentLocalIndex];
        /* Preserve the user's codec/bitrate selection across the replace,
         * since the document will reset to the decoded format. */
        SampleParamsPanelData uiState;
        m_sampleParamsPanel->SaveToData(uiState);

        if (!m_replaceCallback(si, dialog.GetPath())) return;
        BAERmfEditorSampleInfo info;
        if (BAERmfEditorDocument_GetSampleInfo(m_document, si, &info) == BAE_NO_ERROR) {
            EditedSample &s = m_samples[(size_t)m_currentLocalIndex];
            s.sourcePath = info.sourcePath ? wxString::FromUTF8(info.sourcePath) : wxString();
            s.sampleInfo = info.sampleInfo;
            s.compressionType = uiState.compressionType;
            s.hasOriginalData = (info.hasOriginalData == TRUE);
            s.sndStorageType = info.sndStorageType;
            s.opusMode = uiState.opusMode;
        }
        LoadLocalSample(m_currentLocalIndex);
    }

    void OnExportSample() {
        if (!m_exportCallback || m_currentLocalIndex < 0 || m_currentLocalIndex >= (int)m_sampleIndices.size()) return;
        uint32_t si = m_sampleIndices[(size_t)m_currentLocalIndex];
        wxString codec;
        char codecBuf[64];
        wxString suggestedName;
        wxString defaultExt;
        wxString filter;

        codecBuf[0] = 0;
        if (BAERmfEditorDocument_GetSampleCodecDescription(m_document, si, codecBuf, sizeof(codecBuf)) == BAE_NO_ERROR) {
            codec = wxString::FromUTF8(codecBuf);
        }

        {
            wxString codecLower = codec.Lower();
            if (codecLower.Contains("flac")) {
                defaultExt = "flac";
                filter = "FLAC files (*.flac)|*.flac|All files (*.*)|*.*";
            } else if (codecLower.Contains("opus")) {
                defaultExt = "opus";
                filter = "Ogg Opus files (*.opus)|*.opus|All files (*.*)|*.*";
            } else if (codecLower.Contains("vorbis")) {
                defaultExt = "ogg";
                filter = "Ogg Vorbis files (*.ogg)|*.ogg|All files (*.*)|*.*";
            } else if (codecLower.Contains("mpeg")) {
                defaultExt = "mp3";
                filter = "MP3 files (*.mp3)|*.mp3|All files (*.*)|*.*";
            } else if (codecLower.Contains("ima")) {
                defaultExt = "aif";
                filter = "AIFF files (*.aif)|*.aif|WAV files (*.wav)|*.wav|All files (*.*)|*.*";
            } else {
                defaultExt = "wav";
                filter = "WAV files (*.wav)|*.wav|AIFF files (*.aif)|*.aif|All files (*.*)|*.*";
            }
        }

        suggestedName = SanitizeDisplayName(m_sampleParamsPanel->GetDisplayName()) + "." + defaultExt;
        wxFileDialog dialog(this, "Save Embedded Sample", wxEmptyString, suggestedName,
            filter, wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dialog.ShowModal() != wxID_OK) return;
        if (!m_exportCallback(si, dialog.GetPath())) {
            wxMessageBox("Failed to save sample.", "Embedded Instruments", wxOK | wxICON_ERROR, this);
        }
    }

    void OnApply(wxCommandEvent &) {
        if (m_loadingUiValues || m_inInstantApply) {
            return;
        }
        m_inInstantApply = true;
        if (m_beginUndoCallback) {
            m_beginUndoCallback("Edit Instrument");
        }
        if (!ApplyInstrumentChanges(true)) {
            if (m_cancelUndoCallback) {
                m_cancelUndoCallback();
            }
            m_inInstantApply = false;
            return;
        }
        SaveCurrentSampleFromUI();
        /* Update program for all samples in the group */
        unsigned char prog = (unsigned char)m_instParamsPanel->GetProgram();
        for (auto &s : m_samples) s.program = prog;
        for (size_t i = 0; i < m_samples.size() && i < m_sampleIndices.size(); ++i) {
            BAERmfEditorSampleInfo info;
            uint32_t sampleIndex = m_sampleIndices[i];
            wxScopedCharBuffer nameUtf8 = m_samples[i].displayName.utf8_str();

            if (sampleIndex == static_cast<uint32_t>(-1)) {
                continue;
            }
            info.displayName = const_cast<char *>(nameUtf8.data());
            info.sourcePath = NULL;
            info.program = m_samples[i].program;
            info.rootKey = m_samples[i].rootKey;
            info.lowKey = m_samples[i].lowKey;
            info.highKey = m_samples[i].highKey;
            info.splitVolume = m_samples[i].splitVolume;
            info.sampleInfo = m_samples[i].sampleInfo;
            info.compressionType = m_samples[i].compressionType;
            info.hasOriginalData = m_samples[i].hasOriginalData ? TRUE : FALSE;
            info.sndStorageType = m_samples[i].sndStorageType;
            info.opusMode = m_samples[i].opusMode;
            info.opusRoundTripResample = m_samples[i].opusRoundTripResample ? TRUE : FALSE;
            if (BAERmfEditorDocument_SetSampleInfo(m_document, sampleIndex, &info) != BAE_NO_ERROR) {
                if (m_cancelUndoCallback) {
                    m_cancelUndoCallback();
                }
                m_inInstantApply = false;
                return;
            }
        }
        if (m_commitUndoCallback) {
            m_commitUndoCallback("Edit Instrument");
        }

        /* The sample waveform preview is generated from the current document
         * state (save/reload path). After Apply writes sample metadata and
         * codec settings, refresh immediately so the dialog reflects the
         * re-encoded preview without reopening. */
        RefreshWaveform();

        InvalidatePreviewCache(kInstrumentPreviewInvalidate_All);
        m_inInstantApply = false;
    }

    void OnOk(wxCommandEvent &evt) {
        SaveCurrentSampleFromUI();
        unsigned char prog = (unsigned char)m_instParamsPanel->GetProgram();
        for (auto &s : m_samples) s.program = prog;
        if (!ApplyInstrumentChanges(true)) {
            return;
        }
        evt.Skip();
    }
};

}  // namespace

bool ShowInstrumentExtEditorDialog(
    wxWindow *parent,
    BAERmfEditorDocument *document,
    uint32_t primarySampleIndex,
    BAERmfEditorInstrumentExtInfo const *initialExtInfo,
    std::function<void(wxString const &)> beginUndoCallback,
    std::function<void(wxString const &)> commitUndoCallback,
    std::function<void()> cancelUndoCallback,
    std::function<void(uint32_t, int, BAESampleInfo const *, int16_t, unsigned char, BAERmfEditorCompressionType, bool, int, BAERmfEditorInstrumentExtInfo const *)> playCallback,
    std::function<void()> stopCallback,
    std::function<void(int)> stopTaggedCallback,
    std::function<void(uint32_t)> invalidatePreviewCallback,
    std::function<bool(uint32_t, wxString const &)> replaceCallback,
    std::function<bool(uint32_t, wxString const &)> exportCallback,
    std::vector<uint32_t> *outDeletedSampleIndices,
    std::vector<uint32_t> *outSampleIndices,
    std::vector<InstrumentEditorEditedSample> *outEditedSamples) {

    InstrumentExtEditorDialog dialog(parent, document, primarySampleIndex,
                                     initialExtInfo,
                                     std::move(beginUndoCallback),
                                     std::move(commitUndoCallback),
                                     std::move(cancelUndoCallback),
                                     std::move(playCallback),
                                     std::move(stopCallback),
                                     std::move(stopTaggedCallback),
                                     std::move(invalidatePreviewCallback),
                                     std::move(replaceCallback),
                                     std::move(exportCallback));

    int result = dialog.ShowModal();
    if (result != wxID_OK && result != wxID_APPLY) {
        return false;
    }

    if (outDeletedSampleIndices) *outDeletedSampleIndices = dialog.GetDeletedSampleIndices();
    if (outSampleIndices) *outSampleIndices = dialog.GetSampleIndices();
    if (outEditedSamples) *outEditedSamples = dialog.GetEditedSamples();
    return true;
}
