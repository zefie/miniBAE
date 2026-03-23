#include <stdint.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>
#include <iostream>

#include <zlib.h>
#include <lzma.h>

#include <wx/dcbuffer.h>
#include <wx/button.h>
#include <wx/choicdlg.h>
#include <wx/file.h>
#include <wx/fileconf.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/choice.h>
#include <wx/listbox.h>
#include <wx/msgdlg.h>
#include <wx/notebook.h>
#include <wx/numdlg.h>
#include <wx/scrolwin.h>
#include <wx/slider.h>
#include <wx/spinctrl.h>
#include <wx/splitter.h>
#include <wx/textdlg.h>
#include <wx/timer.h>
#include <wx/treectrl.h>
#include <wx/wx.h>

extern "C" {
#include "NeoBAE.h"
#include "GenSnd.h"
#include "X_Formats.h"
}

#include "editor_bank.h"
#include "editor_instrument_ext_dialog.h"
#include "editor_metadata_dialog.h"
#include "editor_pianoroll_panel.h"
#include "batch_compress_dialog.h"

#define STR2(x) #x
#define STR(x) STR2(x)

#ifdef __WXMSW__
#include <wx/msw/winundef.h>
#endif // __WXMSW__

#ifdef __WXGTK__
#include <gtk/gtk.h>
#endif // __WXGTK__

#define VERSION "0.10a"

namespace {

#ifdef __WXMSW__
#ifdef GIT_VERSION
        constexpr char const* kVersionString = VERSION " (" STR(GIT_VERSION) ")";
#else
    constexpr char const* kVersionString = VERSION;
#endif // GIT_VERSION    
#else
    constexpr char const* kVersionString = VERSION " (" _VERSION ")";
#endif

static bool IsOpusCompressionType(BAERmfEditorCompressionType compressionType) {
    switch (compressionType) {
        case BAE_EDITOR_COMPRESSION_OPUS_12K:
        case BAE_EDITOR_COMPRESSION_OPUS_16K:
        case BAE_EDITOR_COMPRESSION_OPUS_24K:
        case BAE_EDITOR_COMPRESSION_OPUS_32K:
        case BAE_EDITOR_COMPRESSION_OPUS_48K:
        case BAE_EDITOR_COMPRESSION_OPUS_64K:
        case BAE_EDITOR_COMPRESSION_OPUS_80K:
        case BAE_EDITOR_COMPRESSION_OPUS_96K:
        case BAE_EDITOR_COMPRESSION_OPUS_128K:
        case BAE_EDITOR_COMPRESSION_OPUS_160K:
        case BAE_EDITOR_COMPRESSION_OPUS_192K:
        case BAE_EDITOR_COMPRESSION_OPUS_256K:
            return true;
        default:
            return false;
    }
}

static bool IsMpegCompressionType(BAERmfEditorCompressionType compressionType) {
    switch (compressionType) {
        case BAE_EDITOR_COMPRESSION_MP3_32K:
        case BAE_EDITOR_COMPRESSION_MP3_48K:
        case BAE_EDITOR_COMPRESSION_MP3_64K:
        case BAE_EDITOR_COMPRESSION_MP3_96K:
        case BAE_EDITOR_COMPRESSION_MP3_128K:
        case BAE_EDITOR_COMPRESSION_MP3_192K:
        case BAE_EDITOR_COMPRESSION_MP3_256K:
        case BAE_EDITOR_COMPRESSION_MP3_320K:
            return true;
        default:
            return false;
    }
}

static bool IsMidiSaveExtension(wxString const &path) {
    wxString ext = wxFileName(path).GetExt().Lower();
    return ext == "mid" || ext == "midi";
}

enum {
    ID_TrackAdd = wxID_HIGHEST + 200,
    ID_TrackDelete,
    ID_TrackRename,
    ID_SampleAdd,
    ID_SampleNewInstrument,
    ID_InstrumentEdit,
    ID_SampleDelete,
    ID_LoadBank,
    ID_ReloadInternalBank,
    ID_PlaybackTimer,
    ID_NotePreviewTimer,
    ID_CompressInstrument,
    ID_CompressAllInstruments,
    ID_DeleteAllInstruments,
    ID_CurrentBankDisplay,
    ID_CloneFromBank,
    ID_CloneAllUsedFromBank,
    ID_AliasFromBank,
    ID_SaveSession,
    ID_SaveSessionAs,
    ID_BankSave,
    ID_BankSaveAs,
    ID_BankLoadBuiltin,
    ID_SettingsPanFix,
    ID_SettingsClassicChorus,
};

static constexpr uint8_t  kNbsMagic[4] = {'N', 'B', 'S', '\0'};
static constexpr uint16_t kNbsVersion = 2;       /* v2: LZMA compression (v1 used zlib) */
static constexpr uint16_t kNbsVersionZlib = 1;   /* for reading old sessions */
static constexpr uint16_t kNbsFieldRmfBlob      = 0x0001;
static constexpr uint16_t kNbsFieldSettings     = 0x0002;
static constexpr uint16_t kNbsFieldOriginalPath = 0x0003;
static constexpr uint16_t kNbsFieldUserEndTick  = 0x0004;

/* Default timeline length for a new document: 30 s at 120 BPM / 480 TPQ
 * 30 s * 2 beats/s * 480 ticks/beat = 28 800 ticks */
static constexpr uint32_t kDefaultNewDocumentEndTicks = 28800;

#pragma pack(push, 1)
struct NbsSessionSettings {
    uint8_t  settingsVersion;
    int32_t  previewVolume;
    int32_t  previewReverbType;
    uint8_t  previewLoopEnabled;
    int32_t  midiStorageMode;
    int32_t  selectedTrack;
    /* v2 fields */
    uint8_t  panFix;
    uint8_t  classicChorus;
    uint8_t  savePreviewToSong;
};
#pragma pack(pop)

static void AppendLE16(std::vector<unsigned char> &buf, uint16_t val) {
    buf.push_back(static_cast<unsigned char>(val & 0xFF));
    buf.push_back(static_cast<unsigned char>((val >> 8) & 0xFF));
}

static void AppendLE32(std::vector<unsigned char> &buf, uint32_t val) {
    buf.push_back(static_cast<unsigned char>(val & 0xFF));
    buf.push_back(static_cast<unsigned char>((val >> 8) & 0xFF));
    buf.push_back(static_cast<unsigned char>((val >> 16) & 0xFF));
    buf.push_back(static_cast<unsigned char>((val >> 24) & 0xFF));
}

static uint16_t ReadLE16(unsigned char const *p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

static uint32_t ReadLE32(unsigned char const *p) {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

struct UndoTempoEventState {
    uint32_t tick;
    uint32_t microsecondsPerQuarter;
};

struct UndoCCEventState {
    unsigned char controller;
    uint32_t tick;
    unsigned char value;
};

struct UndoTrackState {
    std::vector<BAERmfEditorNoteInfo> notes;
    std::vector<UndoCCEventState> ccEvents;
};

struct UndoDocumentState {
    uint32_t tempoBpm;
    std::vector<UndoTempoEventState> tempoEvents;
    std::vector<UndoTrackState> tracks;
    std::vector<unsigned char> serializedDocument;
    uint32_t documentHash = 0;  // Simple hash for dirty-flag optimization
    int pianoScrollVX = 0;      // Piano roll horizontal scroll position
    int pianoScrollVY = 0;      // Piano roll vertical scroll position
};

static uint32_t ComputeDocumentHash(UndoDocumentState const &state) {
    uint32_t hash = 5381;  // djb2 hash seed
    hash = ((hash << 5) + hash) ^ state.tempoBpm;
    for (auto const &te : state.tempoEvents) {
        hash = ((hash << 5) + hash) ^ te.tick;
        hash = ((hash << 5) + hash) ^ te.microsecondsPerQuarter;
    }
    for (auto const &track : state.tracks) {
        for (auto const &note : track.notes) {
            hash = ((hash << 5) + hash) ^ note.startTick;
            hash = ((hash << 5) + hash) ^ note.durationTicks;
            hash = ((hash << 5) + hash) ^ note.note;
            hash = ((hash << 5) + hash) ^ note.velocity;
            hash = ((hash << 5) + hash) ^ note.channel;
            hash = ((hash << 5) + hash) ^ note.bank;
            hash = ((hash << 5) + hash) ^ note.program;
        }
        for (auto const &cc : track.ccEvents) {
            hash = ((hash << 5) + hash) ^ cc.controller;
            hash = ((hash << 5) + hash) ^ cc.tick;
            hash = ((hash << 5) + hash) ^ cc.value;
        }
    }
    return hash;
}

struct UndoEntry {
    wxString label;
    UndoDocumentState before;
    UndoDocumentState after;
};

class SampleTreeItemData final : public wxTreeItemData {
public:
    SampleTreeItemData(uint32_t assetID, uint32_t sampleIndex, bool isAssetNode)
        : m_assetID(assetID),
          m_sampleIndex(sampleIndex),
          m_isAssetNode(isAssetNode) {}

    uint32_t GetAssetID() const { return m_assetID; }
    uint32_t GetSampleIndex() const { return m_sampleIndex; }
    bool IsAssetNode() const { return m_isAssetNode; }

private:
    uint32_t m_assetID;
    uint32_t m_sampleIndex;
    bool m_isAssetNode;
};

static unsigned char NormalizeRootKeyForSingleKeySplit(unsigned char rootKey,
                                                        unsigned char lowKey,
                                                        unsigned char highKey) {
    if (rootKey == 0 && lowKey <= 127 && highKey <= 127 && lowKey == highKey) {
        return lowKey;
    }
    return rootKey;
}

static bool NoteInfoEquals(BAERmfEditorNoteInfo const &left, BAERmfEditorNoteInfo const &right) {
    return left.startTick == right.startTick &&
           left.durationTicks == right.durationTicks &&
           left.note == right.note &&
           left.velocity == right.velocity &&
           left.channel == right.channel &&
           left.bank == right.bank &&
           left.program == right.program;
}

static bool UndoSnapshotsEqual(UndoDocumentState const &left, UndoDocumentState const &right) {
    if (!left.serializedDocument.empty() && !right.serializedDocument.empty()) {
        if (left.serializedDocument.size() != right.serializedDocument.size()) {
            return false;
        }
        return std::equal(left.serializedDocument.begin(),
                          left.serializedDocument.end(),
                          right.serializedDocument.begin());
    }
    if (left.tempoBpm != right.tempoBpm || left.tempoEvents.size() != right.tempoEvents.size() || left.tracks.size() != right.tracks.size()) {
        return false;
    }
    for (size_t index = 0; index < left.tempoEvents.size(); ++index) {
        if (left.tempoEvents[index].tick != right.tempoEvents[index].tick ||
            left.tempoEvents[index].microsecondsPerQuarter != right.tempoEvents[index].microsecondsPerQuarter) {
            return false;
        }
    }
    for (size_t trackIndex = 0; trackIndex < left.tracks.size(); ++trackIndex) {
        UndoTrackState const &leftTrack = left.tracks[trackIndex];
        UndoTrackState const &rightTrack = right.tracks[trackIndex];

        if (leftTrack.notes.size() != rightTrack.notes.size()) {
            return false;
        }
        for (size_t noteIndex = 0; noteIndex < leftTrack.notes.size(); ++noteIndex) {
            if (!NoteInfoEquals(leftTrack.notes[noteIndex], rightTrack.notes[noteIndex])) {
                return false;
            }
        }
        if (leftTrack.ccEvents.size() != rightTrack.ccEvents.size()) {
            return false;
        }
        for (size_t eventIndex = 0; eventIndex < leftTrack.ccEvents.size(); ++eventIndex) {
            if (leftTrack.ccEvents[eventIndex].controller != rightTrack.ccEvents[eventIndex].controller ||
                leftTrack.ccEvents[eventIndex].tick != rightTrack.ccEvents[eventIndex].tick ||
                leftTrack.ccEvents[eventIndex].value != rightTrack.ccEvents[eventIndex].value) {
                return false;
            }
        }
    }
    return true;
}

class MainFrame final : public wxFrame {
public:
    MainFrame()
        : wxFrame(nullptr, wxID_ANY, wxString::Format("NeoBAE Studio v%s", kVersionString), wxDefaultPosition, wxSize(1400, 940)),
          m_document(nullptr),
                    m_updatingControls(false),
                    m_editorNotebook(nullptr),
                    m_bankEditorPanel(nullptr),
                    m_editorMode(kEditorModeMidi),
                    m_bankEditorWarningAccepted(false),
                    m_playbackMixer(nullptr),
                    m_playbackSong(nullptr),
                    m_notePreviewSong(nullptr),
                    m_keyboardPreviewSong(nullptr),
                    m_bankPreviewSong(nullptr),
                    m_bankPreviewSongStarted(false),
                    m_bankPreviewLoadedInst(static_cast<BAE_INSTRUMENT>(-1)),
                    m_bankPreviewDirtyApplied(false),
                    m_hasBankDirtyExtInfo(false),
                    m_previewSound(nullptr),
                    m_playbackTimer(this, ID_PlaybackTimer),
                    m_notePreviewTimer(this, ID_NotePreviewTimer),
                    m_notePreviewActive(false),
                    m_notePreviewChannel(0),
                    m_notePreviewNote(0),
                    m_playbackChannelMask(0xFFFFu),
                    m_ignoreSeekEvent(false),
                    m_autoFollowPlayhead(true),
                    m_bankToken(nullptr),
                    m_bankLoaded(false),
                    m_currentBankMenuItem(nullptr),
                    m_reloadInternalBankMenuItem(nullptr),
                    m_settingsMenu(nullptr),
                    m_savePreviewToSongCheck(nullptr),
                    m_pendingLoopHotReload(false),
                    m_pendingLoopHotReloadPosUsec(0),
                    m_pendingLoopHotReloadPaused(false),
                    m_loadingLoopControls(false),
                    m_hasPendingUndo(false),
                    m_restoringUndo(false),
                    m_hasUnsavedChanges(false),
                    m_bankHasUnsavedChanges(false) {
                SetMinSize(wxSize(1280, 800));
#ifdef __WXMSW__
        SetIcon(wxICON(APPICON));
#endif

        wxMenu *fileMenu;
        wxMenu *soundBankMenu;
        wxMenu *editMenu;
        wxMenuBar *menuBar;
        wxSplitterWindow *splitter;
        wxPanel *sidebar;
        wxPanel *editorPanel;
        wxBoxSizer *sidebarSizer;
        wxBoxSizer *editorSizer;
        wxFlexGridSizer *controlsSizer;
        wxBoxSizer *transportSizer;
        wxBoxSizer *sampleButtonSizer;

        /* MIDI Editor File menu */
        fileMenu = new wxMenu();
        fileMenu->Append(wxID_NEW, "&New\tCtrl+N");
        fileMenu->Append(wxID_OPEN, "&Open\tCtrl+O");
        fileMenu->Append(wxID_SAVEAS, "&Export as...\tCtrl+Shift+S");
        fileMenu->AppendSeparator();
        fileMenu->Append(ID_SaveSession, "Save &Session\tCtrl+S");
        fileMenu->Append(ID_SaveSessionAs, "Save S&ession as...");
        fileMenu->AppendSeparator();
        soundBankMenu = new wxMenu();
        m_currentBankMenuItem = soundBankMenu->Append(ID_CurrentBankDisplay, "Current: Built-in");
        if (m_currentBankMenuItem) {
            m_currentBankMenuItem->Enable(false);
        }
        soundBankMenu->AppendSeparator();
        soundBankMenu->Append(ID_LoadBank, "Load a &Bank (HSB/ZSB)...\tCtrl+B");
        soundBankMenu->Append(ID_CloneFromBank, "&Clone an Instrument from currently loaded Bank...");
        soundBankMenu->Append(ID_CloneAllUsedFromBank, "Clone &All Used Instruments from MIDI Stream...");
        soundBankMenu->Append(ID_AliasFromBank, "&Alias an Instrument from currently loaded Bank...");
        m_reloadInternalBankMenuItem = soundBankMenu->Append(ID_ReloadInternalBank, "&Unload All Banks and Reload Internal Bank");
        fileMenu->AppendSubMenu(soundBankMenu, "Sound &Bank");
        fileMenu->AppendSeparator();
        fileMenu->Append(wxID_EXIT, "E&xit");
        m_midiFileMenu = fileMenu;

        /* Bank Editor File menu */
        {
            wxMenu *bankMenu = new wxMenu();
            bankMenu->Append(wxID_OPEN, "&Open\tCtrl+O");
            bankMenu->Append(ID_LoadBank, "Load a &Bank (HSB/ZSB)...\tCtrl+B");
            bankMenu->AppendSeparator();
            bankMenu->Append(ID_BankSave, "&Save Bank\tCtrl+S");
            bankMenu->Append(ID_BankSaveAs, "Save Bank &As...\tCtrl+Shift+S");
            bankMenu->AppendSeparator();
            bankMenu->Append(ID_BankLoadBuiltin, "Load &Built-in Bank");
            bankMenu->AppendSeparator();
            bankMenu->Append(ID_SaveSession, "Save S&ession\tCtrl+Alt+S");
            bankMenu->Append(ID_SaveSessionAs, "Save Se&ssion as...");
            bankMenu->AppendSeparator();
            bankMenu->Append(wxID_EXIT, "E&xit");
            m_bankFileMenu = bankMenu;
        }

        editMenu = new wxMenu();
        editMenu->Append(wxID_UNDO, "&Undo\tCtrl+Z");
        editMenu->Append(wxID_REDO, "&Redo\tCtrl+Y");
        m_settingsMenu = new wxMenu();
        m_settingsMenu->AppendCheckItem(ID_SettingsPanFix, "STEREO_PAN LFO DC &Fix");
        m_settingsMenu->AppendCheckItem(ID_SettingsClassicChorus, "&Classic Chorus Order");
        m_settingsMenu->Check(ID_SettingsPanFix, true);
        m_settingsMenu->Check(ID_SettingsClassicChorus, false);
        menuBar = new wxMenuBar();
        menuBar->Append(m_midiFileMenu, "&File");
        menuBar->Append(editMenu, "&Edit");
        menuBar->Append(m_settingsMenu, "&Settings");
        SetMenuBar(menuBar);

        m_editorNotebook = new wxNotebook(this, wxID_ANY);
        splitter = new wxSplitterWindow(m_editorNotebook, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxSP_LIVE_UPDATE | wxSP_3D);
        sidebar = new wxPanel(splitter);
        editorPanel = new wxPanel(splitter);
        sidebarSizer = new wxBoxSizer(wxVERTICAL);
        editorSizer = new wxBoxSizer(wxVERTICAL);

        m_trackList = new wxListBox(sidebar, wxID_ANY);
        m_sampleTree = new wxTreeCtrl(sidebar, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                          wxTR_HAS_BUTTONS | wxTR_LINES_AT_ROOT | wxTR_SINGLE);
        m_sampleAddButton = new wxButton(sidebar, ID_SampleNewInstrument, "Add Instrument");
        sidebarSizer->Add(new wxStaticText(sidebar, wxID_ANY, "Tracks"), 0, wxALL, 8);
        sidebarSizer->Add(m_trackList, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
        sidebarSizer->Add(new wxStaticText(sidebar, wxID_ANY, "Sample Tree"), 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
        sidebarSizer->Add(m_sampleTree, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
        sampleButtonSizer = new wxBoxSizer(wxHORIZONTAL);
        sampleButtonSizer->Add(m_sampleAddButton, 1, 0, 0);
        sidebarSizer->Add(sampleButtonSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
        sidebar->SetSizer(sidebarSizer);

        controlsSizer = new wxFlexGridSizer(2, 6, 8, 10);
        controlsSizer->AddGrowableCol(1, 1);
        controlsSizer->AddGrowableCol(3, 1);
        controlsSizer->AddGrowableCol(5, 1);

        m_tempoSpin = new wxSpinCtrl(editorPanel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 20, 400, 120);
        m_bankSpin = new wxSpinCtrl(editorPanel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 127, 0);
        m_programSpin = new wxSpinCtrl(editorPanel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 127, 0);
        m_transposeSpin = new wxSpinCtrl(editorPanel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, -48, 48, 0);
        m_panSpin = new wxSpinCtrl(editorPanel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 127, 64);
        m_volumeSpin = new wxSpinCtrl(editorPanel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 127, 100);
        m_playButton = new wxButton(editorPanel, wxID_ANY, "Play");
        m_pauseButton = new wxButton(editorPanel, wxID_ANY, "Pause/Resume");
        m_stopButton = new wxButton(editorPanel, wxID_ANY, "Stop");
        m_metadataButton = new wxButton(editorPanel, wxID_ANY, "Metadata");
        m_playScopeChoice = new wxChoice(editorPanel, wxID_ANY);
        m_midiStorageChoice = new wxChoice(editorPanel, wxID_ANY);
        m_previewVolumeSlider = new wxSlider(editorPanel, wxID_ANY, 100, 0, 100, wxDefaultPosition, wxSize(120, -1), wxSL_HORIZONTAL);
        m_previewReverbChoice = new wxChoice(editorPanel, wxID_ANY);
        m_previewLoopCheck = new wxCheckBox(editorPanel, wxID_ANY, "Loop");
        m_midiLoopEnableCheck = new wxCheckBox(editorPanel, wxID_ANY, "MIDI Loop Markers");
        m_midiLoopStartText = new wxTextCtrl(editorPanel, wxID_ANY, "0:00.000", wxDefaultPosition, wxSize(110, -1));
        m_midiLoopEndText = new wxTextCtrl(editorPanel, wxID_ANY, "0:00.000", wxDefaultPosition, wxSize(110, -1));
        m_channelsConfigButton = new wxButton(editorPanel, wxID_ANY, "Channels");
        m_playScopeChoice->Append("All Tracks");
        m_playScopeChoice->Append("Current Track");
        m_playScopeChoice->Append("Channels");
        m_playScopeChoice->SetSelection(0);
        m_midiStorageChoice->Append("CMID (best effort)");
        m_midiStorageChoice->Append("ECMI (encrypt + compress)");
        m_midiStorageChoice->Append("EMID (encrypt)");
        m_midiStorageChoice->Append("MIDI (plain)");
        m_midiStorageChoice->SetSelection(1);
        m_previewReverbChoice->Append("None");
        m_previewReverbChoice->Append("Igor's Closet");
        m_previewReverbChoice->Append("Igor's Garage");
        m_previewReverbChoice->Append("Igor's Acoustic Lab");
        m_previewReverbChoice->Append("Igor's Cavern");
        m_previewReverbChoice->Append("Igor's Dungeon");
        m_previewReverbChoice->Append("Small Reflections");
        m_previewReverbChoice->Append("Early Reflections");
        m_previewReverbChoice->Append("Basement");
        m_previewReverbChoice->Append("Banquet Hall");
        m_previewReverbChoice->Append("Catacombs");
        m_previewReverbChoice->SetSelection(0);

        {
            wxSize compactSpinSize(128, -1);

            m_tempoSpin->SetMinSize(compactSpinSize);
            m_bankSpin->SetMinSize(compactSpinSize);
            m_programSpin->SetMinSize(compactSpinSize);
            m_transposeSpin->SetMinSize(compactSpinSize);
            m_panSpin->SetMinSize(compactSpinSize);
            m_volumeSpin->SetMinSize(compactSpinSize);
        }
        m_midiStorageChoice->SetMinSize(wxSize(220, -1));
        m_metadataButton->SetMinSize(wxSize(110, -1));
        m_previewReverbChoice->SetMinSize(wxSize(170, -1));
        m_previewLoopCheck->SetValue(false);

        controlsSizer->Add(new wxStaticText(editorPanel, wxID_ANY, "Tempo"), 0, wxALIGN_CENTER_VERTICAL);
        controlsSizer->Add(m_tempoSpin, 1, wxEXPAND);
        controlsSizer->Add(new wxStaticText(editorPanel, wxID_ANY, "Bank"), 0, wxALIGN_CENTER_VERTICAL);
        controlsSizer->Add(m_bankSpin, 1, wxEXPAND);
        controlsSizer->Add(new wxStaticText(editorPanel, wxID_ANY, "Program"), 0, wxALIGN_CENTER_VERTICAL);
        controlsSizer->Add(m_programSpin, 1, wxEXPAND);
        controlsSizer->Add(new wxStaticText(editorPanel, wxID_ANY, "Transpose"), 0, wxALIGN_CENTER_VERTICAL);
        controlsSizer->Add(m_transposeSpin, 1, wxEXPAND);
        controlsSizer->Add(new wxStaticText(editorPanel, wxID_ANY, "Pan"), 0, wxALIGN_CENTER_VERTICAL);
        controlsSizer->Add(m_panSpin, 1, wxEXPAND);
        controlsSizer->Add(new wxStaticText(editorPanel, wxID_ANY, "Volume"), 0, wxALIGN_CENTER_VERTICAL);
        controlsSizer->Add(m_volumeSpin, 1, wxEXPAND);

        transportSizer = new wxBoxSizer(wxHORIZONTAL);
        transportSizer->Add(m_playButton, 0, wxRIGHT, 8);
        transportSizer->Add(m_pauseButton, 0, wxRIGHT, 8);
        transportSizer->Add(m_stopButton, 0, wxRIGHT, 8);
        transportSizer->Add(new wxStaticText(editorPanel, wxID_ANY, "Playback"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        transportSizer->Add(m_playScopeChoice, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
        transportSizer->Add(new wxStaticText(editorPanel, wxID_ANY, "Preview Vol"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        transportSizer->Add(m_previewVolumeSlider, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
        transportSizer->Add(new wxStaticText(editorPanel, wxID_ANY, "Reverb"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        transportSizer->Add(m_previewReverbChoice, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
        transportSizer->Add(m_previewLoopCheck, 0, wxALIGN_CENTER_VERTICAL, 0);

        {
            wxBoxSizer *topRowSizer;
            wxBoxSizer *leftControlsSizer;
            wxBoxSizer *extraControlsSizer;
            wxBoxSizer *midiStorageRow;
            wxBoxSizer *midiLoopRow;

            topRowSizer = new wxBoxSizer(wxHORIZONTAL);
            leftControlsSizer = new wxBoxSizer(wxVERTICAL);
            extraControlsSizer = new wxBoxSizer(wxVERTICAL);
            midiStorageRow = new wxBoxSizer(wxHORIZONTAL);

            midiStorageRow->Add(new wxStaticText(editorPanel, wxID_ANY, "MIDI Type"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
            midiStorageRow->Add(m_midiStorageChoice, 0, wxALIGN_CENTER_VERTICAL, 0);
            extraControlsSizer->Add(midiStorageRow, 0, wxALIGN_LEFT | wxBOTTOM, 8);
            m_savePreviewToSongCheck = new wxCheckBox(editorPanel, wxID_ANY, "Save Settings to Song (ZMF)");
            m_savePreviewToSongCheck->SetValue(false);
            m_savePreviewToSongCheck->SetToolTip("When checked, the current Pan Fix and Classic Chorus\nsettings will be saved into the song resource.\nRequires ZMF format.");
            extraControlsSizer->Add(m_savePreviewToSongCheck, 0, wxALIGN_LEFT | wxBOTTOM, 8);
            midiLoopRow = new wxBoxSizer(wxHORIZONTAL);
            midiLoopRow->Add(m_midiLoopEnableCheck, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
            midiLoopRow->Add(new wxStaticText(editorPanel, wxID_ANY, "Start Time"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            midiLoopRow->Add(m_midiLoopStartText, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
            midiLoopRow->Add(new wxStaticText(editorPanel, wxID_ANY, "End Time"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            midiLoopRow->Add(m_midiLoopEndText, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
            midiLoopRow->Add(m_channelsConfigButton, 0, wxALIGN_CENTER_VERTICAL, 0);
            extraControlsSizer->Add(m_metadataButton, 0, wxEXPAND, 0);

            leftControlsSizer->Add(controlsSizer, 0, wxEXPAND | wxBOTTOM, 8);
            leftControlsSizer->Add(midiLoopRow, 0, wxALIGN_LEFT, 0);

            topRowSizer->Add(leftControlsSizer, 1, wxEXPAND | wxRIGHT, 12);
            topRowSizer->Add(extraControlsSizer, 0, wxALIGN_TOP, 0);
            editorSizer->Add(topRowSizer, 0, wxEXPAND | wxALL, 10);
        }

        m_positionSlider = new wxSlider(editorPanel, wxID_ANY, 0, 0, 1000, wxDefaultPosition, wxDefaultSize, wxSL_HORIZONTAL);
        m_positionLabel = new wxStaticText(editorPanel, wxID_ANY, "0:00.0 / 0:00.0");

        m_pianoRoll = CreatePianoRollPanel(editorPanel);
        editorSizer->Add(transportSizer, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
        {
            wxBoxSizer *positionSizer;

            positionSizer = new wxBoxSizer(wxHORIZONTAL);
            positionSizer->Add(m_positionSlider, 1, wxEXPAND | wxRIGHT, 10);
            positionSizer->Add(m_positionLabel, 0, wxALIGN_CENTER_VERTICAL);
            editorSizer->Add(positionSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        }
        editorSizer->Add(PianoRollPanel_AsWindow(m_pianoRoll), 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        editorPanel->SetSizer(editorSizer);

        PianoRollPanel_SetSelectionChangedCallback(m_pianoRoll, [this]() { UpdateControlsFromSelection(); });
        PianoRollPanel_SetSeekRequestedCallback(m_pianoRoll, [this](uint32_t tick) { SeekToTickFromPianoRoll(tick); });
        PianoRollPanel_SetNotePreviewRequestedCallback(m_pianoRoll,
                                                       [this](uint16_t bank,
                                                              unsigned char program,
                                                                                  unsigned char channel,
                                                              unsigned char note,
                                                              uint32_t durationTicks,
                                                              int trackIndex) {
                                                                              PreviewPianoRollNote(bank, program, channel, note, durationTicks, trackIndex);
                                                       });
        PianoRollPanel_SetNotePreviewStopRequestedCallback(m_pianoRoll,
                                                           [this]() {
                                                               StopPianoRollPreview(false);
                                                           });
        PianoRollPanel_SetUndoCallbacks(m_pianoRoll,
                        [this](wxString const &label) { BeginUndoAction(label); },
                        [this](wxString const &label) { CommitUndoAction(label); },
                        [this]() { CancelUndoAction(); });
        PianoRollPanel_SetMidiLoopEditCallback(m_pianoRoll,
                        [this](bool enabled, uint32_t startTick, uint32_t endTick) {
                            return ApplyMidiLoopMarkersFromPianoRoll(enabled, startTick, endTick);
                        });

        splitter->SplitVertically(sidebar, editorPanel, 240);
        splitter->SetMinimumPaneSize(180);

        m_editorNotebook->AddPage(splitter, "MIDI Editor", true);
        {
            memset(&m_bankDirtyExtInfo, 0, sizeof(m_bankDirtyExtInfo));
            m_bankEditorPanel = CreateBankEditorPanel(m_editorNotebook);
            m_editorNotebook->AddPage(BankEditorPanel_AsWindow(m_bankEditorPanel), "Bank Editor", false);
            BankEditorPanel_SetPreviewCallbacks(m_bankEditorPanel,
                [this](unsigned char bank, unsigned char program, int note, int previewTag, bool isPercussion) {
                    PlayBankPreviewNote(bank, program, note, previewTag, isPercussion);
                },
                [this](int previewTag) {
                    StopBankPreviewNote(previewTag);
                },
                [this]() {
                    StopBankPreview();
                },
                [this](BAERmfEditorInstrumentExtInfo const *info) {
                    if (info) {
                        m_bankDirtyExtInfo = *info;
                        m_hasBankDirtyExtInfo = true;
                        m_bankPreviewDirtyApplied = false;
                        /* Force instrument reload so the patch is applied
                         * on top of a fresh copy from the bank. */
                        m_bankPreviewLoadedInst = static_cast<BAE_INSTRUMENT>(-1);
                    } else {
                        m_hasBankDirtyExtInfo = false;
                        m_bankPreviewDirtyApplied = false;
                        m_bankPreviewLoadedInst = static_cast<BAE_INSTRUMENT>(-1);
                    }
                });
        }

        CreateStatusBar(2);
        SetStatusText("Welcome to NeoBAE Studio!");
        UpdateLoadedBankStatus();

        Bind(wxEVT_MENU, &MainFrame::OnNew, this, wxID_NEW);
        Bind(wxEVT_MENU, &MainFrame::OnOpen, this, wxID_OPEN);
        Bind(wxEVT_MENU, &MainFrame::OnSaveAs, this, wxID_SAVEAS);
        Bind(wxEVT_MENU, &MainFrame::OnSaveSession, this, ID_SaveSession);
        Bind(wxEVT_MENU, &MainFrame::OnSaveSessionAs, this, ID_SaveSessionAs);
        Bind(wxEVT_MENU, &MainFrame::OnUndo, this, wxID_UNDO);
        Bind(wxEVT_MENU, &MainFrame::OnRedo, this, wxID_REDO);
        Bind(wxEVT_MENU, [this](wxCommandEvent &) {
            bool checked = m_settingsMenu->IsChecked(ID_SettingsPanFix);
            BAE_SetSpanDCFix(checked ? TRUE : FALSE);
            ApplyEngineConfigToDocument();
        }, ID_SettingsPanFix);
        Bind(wxEVT_MENU, [this](wxCommandEvent &) {
            bool checked = m_settingsMenu->IsChecked(ID_SettingsClassicChorus);
            BAE_SetClassicChorus(checked ? TRUE : FALSE);
            ApplyEngineConfigToDocument();
        }, ID_SettingsClassicChorus);
        Bind(wxEVT_MENU, [this](wxCommandEvent &) { Close(); }, wxID_EXIT);
        Bind(wxEVT_CLOSE_WINDOW, &MainFrame::OnCloseWindow, this);
        m_trackList->Bind(wxEVT_LISTBOX, &MainFrame::OnTrackSelected, this);
        m_trackList->Bind(wxEVT_CONTEXT_MENU, &MainFrame::OnTrackContextMenu, this);
        m_sampleTree->Bind(wxEVT_CONTEXT_MENU, &MainFrame::OnSampleContextMenu, this);
        m_tempoSpin->Bind(wxEVT_SPINCTRL, &MainFrame::OnTempoChanged, this);
        m_bankSpin->Bind(wxEVT_SPINCTRL, &MainFrame::OnTrackSettingsChanged, this);
        m_programSpin->Bind(wxEVT_SPINCTRL, &MainFrame::OnTrackSettingsChanged, this);
        m_transposeSpin->Bind(wxEVT_SPINCTRL, &MainFrame::OnTrackSettingsChanged, this);
        m_panSpin->Bind(wxEVT_SPINCTRL, &MainFrame::OnTrackSettingsChanged, this);
        m_volumeSpin->Bind(wxEVT_SPINCTRL, &MainFrame::OnTrackSettingsChanged, this);
        m_bankSpin->Bind(wxEVT_TEXT, &MainFrame::OnTrackSettingsChanged, this);
        m_programSpin->Bind(wxEVT_TEXT, &MainFrame::OnTrackSettingsChanged, this);
        m_transposeSpin->Bind(wxEVT_TEXT, &MainFrame::OnTrackSettingsChanged, this);
        m_panSpin->Bind(wxEVT_TEXT, &MainFrame::OnTrackSettingsChanged, this);
        m_volumeSpin->Bind(wxEVT_TEXT, &MainFrame::OnTrackSettingsChanged, this);
        m_playButton->Bind(wxEVT_BUTTON, &MainFrame::OnPlay, this);
        m_pauseButton->Bind(wxEVT_BUTTON, &MainFrame::OnPauseResume, this);
        m_stopButton->Bind(wxEVT_BUTTON, &MainFrame::OnStop, this);
        m_playScopeChoice->Bind(wxEVT_CHOICE, [this](wxCommandEvent &event) {
            UpdateChannelsConfigButtonState();
            event.Skip();
        });
        m_channelsConfigButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
            OpenPlaybackChannelsDialog();
        });
        m_previewVolumeSlider->Bind(wxEVT_SLIDER, &MainFrame::OnPreviewVolumeChanged, this);
        m_previewReverbChoice->Bind(wxEVT_CHOICE, &MainFrame::OnPreviewReverbChanged, this);
        m_previewLoopCheck->Bind(wxEVT_CHECKBOX, &MainFrame::OnPreviewLoopChanged, this);
        m_midiLoopEnableCheck->Bind(wxEVT_CHECKBOX, &MainFrame::OnMidiLoopMarkersChanged, this);
        m_midiLoopStartText->Bind(wxEVT_TEXT, &MainFrame::OnMidiLoopMarkersChanged, this);
        m_midiLoopEndText->Bind(wxEVT_TEXT, &MainFrame::OnMidiLoopMarkersChanged, this);
        m_positionSlider->Bind(wxEVT_SLIDER, &MainFrame::OnSeekSlider, this);
        m_metadataButton->Bind(wxEVT_BUTTON, &MainFrame::OnMetadata, this);
        Bind(wxEVT_MENU, &MainFrame::OnTrackAdd, this, ID_TrackAdd);
        Bind(wxEVT_MENU, &MainFrame::OnTrackDelete, this, ID_TrackDelete);
        Bind(wxEVT_MENU, &MainFrame::OnTrackRename, this, ID_TrackRename);
        Bind(wxEVT_MENU, &MainFrame::OnSampleAdd, this, ID_SampleAdd);
        Bind(wxEVT_BUTTON, &MainFrame::OnSampleNewInstrument, this, ID_SampleNewInstrument);
        Bind(wxEVT_MENU, &MainFrame::OnSampleNewInstrument, this, ID_SampleNewInstrument);
        Bind(wxEVT_MENU, &MainFrame::OnInstrumentEdit, this, ID_InstrumentEdit);
        Bind(wxEVT_MENU, &MainFrame::OnSampleDelete, this, ID_SampleDelete);
        m_sampleTree->Bind(wxEVT_TREE_ITEM_ACTIVATED, &MainFrame::OnInstrumentEditDblClick, this);
        Bind(wxEVT_MENU, &MainFrame::OnLoadBank, this, ID_LoadBank);
        Bind(wxEVT_MENU, &MainFrame::OnReloadInternalBank, this, ID_ReloadInternalBank);
        Bind(wxEVT_MENU, &MainFrame::OnCompressInstrument, this, ID_CompressInstrument);
        Bind(wxEVT_MENU, &MainFrame::OnCompressAllInstruments, this, ID_CompressAllInstruments);
        Bind(wxEVT_MENU, &MainFrame::OnDeleteAllInstruments, this, ID_DeleteAllInstruments);
        Bind(wxEVT_MENU, &MainFrame::OnCloneFromBank, this, ID_CloneFromBank);
        Bind(wxEVT_MENU, &MainFrame::OnCloneAllUsedFromBank, this, ID_CloneAllUsedFromBank);
        Bind(wxEVT_MENU, &MainFrame::OnAliasFromBank, this, ID_AliasFromBank);
        Bind(wxEVT_TIMER, &MainFrame::OnPlaybackTimer, this, ID_PlaybackTimer);
        Bind(wxEVT_TIMER, &MainFrame::OnNotePreviewTimer, this, ID_NotePreviewTimer);
        m_editorNotebook->Bind(wxEVT_NOTEBOOK_PAGE_CHANGED, &MainFrame::OnEditorTabChanged, this);
        Bind(wxEVT_MENU, &MainFrame::OnBankSave, this, ID_BankSave);
        Bind(wxEVT_MENU, &MainFrame::OnBankSaveAs, this, ID_BankSaveAs);
        Bind(wxEVT_MENU, &MainFrame::OnBankLoadBuiltin, this, ID_BankLoadBuiltin);
        m_savePreviewToSongCheck->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) {
            ApplyEngineConfigToDocument();
        });
        LoadIniSettings();
        EnsurePlaybackEngine();
        /* Load the built-in bank into the bank editor so it is always
         * populated, even before the user explicitly opens a bank file. */
#ifdef _BUILT_IN_PATCHES
        if (m_bankEditorPanel && m_bankLoaded && m_bankToken) {
            BankEditorPanel_LoadBank(m_bankEditorPanel, m_bankToken, "(built-in)");
        }
#endif
        UpdateLoadedBankStatus();
        RefreshMidiLoopControlsFromDocument();
        UpdateUndoMenuState();
        UpdateChannelsConfigButtonState();
        /* Start with a blank document so the UI is immediately interactive */
        DoNewDocument(false);
        /* Defer the initial C5 scroll until after the window is laid out */
        CallAfter([this]() {
            PianoRollPanel_ScrollToC4Center(m_pianoRoll);
        });
    }

    ~MainFrame() override {
        SaveIniSettings();
        if (m_document) {
            BAERmfEditorDocument_Delete(m_document);
            m_document = nullptr;
        }
        StopPlayback(true);
        StopPianoRollPreview(true);
        StopKeyboardPreview();
        StopBankPreview();
        StopPreviewSample();
        ShutdownPlaybackEngine();
    }

    void OpenPathFromCli(wxString const &path) {
        wxString ext;

        if (path.empty()) {
            return;
        }
        ext = wxFileName(path).GetExt().Lower();
        if (ext == "nbs") {
            LoadSession(path);
        } else {
            LoadDocument(path);
        }
    }

private:
    enum EditorMode {
        kEditorModeMidi = 0,
        kEditorModeBank = 1
    };

    BAERmfEditorDocument *m_document;
    bool m_updatingControls;
    wxString m_currentPath;
    wxString m_sessionPath;
    wxNotebook *m_editorNotebook;
    BankEditorPanel *m_bankEditorPanel;
    EditorMode m_editorMode;
    bool m_bankEditorWarningAccepted;
    wxMenu *m_midiFileMenu;
    wxMenu *m_bankFileMenu;
    wxListBox *m_trackList;
    PianoRollPanel *m_pianoRoll;
    wxSpinCtrl *m_tempoSpin;
    wxSpinCtrl *m_bankSpin;
    wxSpinCtrl *m_programSpin;
    wxSpinCtrl *m_transposeSpin;
    wxSpinCtrl *m_panSpin;
    wxSpinCtrl *m_volumeSpin;
    wxButton *m_playButton;
    wxButton *m_pauseButton;
    wxButton *m_stopButton;
    wxButton *m_metadataButton;
    wxChoice *m_playScopeChoice;
    wxChoice *m_midiStorageChoice;
    wxSlider *m_previewVolumeSlider;
    wxChoice *m_previewReverbChoice;
    wxCheckBox *m_previewLoopCheck;
    wxCheckBox *m_midiLoopEnableCheck;
    wxTextCtrl *m_midiLoopStartText;
    wxTextCtrl *m_midiLoopEndText;
    wxButton *m_channelsConfigButton;
    wxSlider *m_positionSlider;
    wxStaticText *m_positionLabel;
    wxTreeCtrl *m_sampleTree;
    wxButton *m_sampleAddButton;
    BAEMixer m_playbackMixer;
    BAESong m_playbackSong;
    std::vector<unsigned char> m_playbackSongBlob;
    BAESong m_notePreviewSong;
    std::vector<unsigned char> m_notePreviewSongBlob;
    BAESong m_keyboardPreviewSong;
    std::vector<unsigned char> m_keyboardPreviewSongBlob;
    BAESong m_bankPreviewSong;
    bool m_bankPreviewSongStarted;
    BAE_INSTRUMENT m_bankPreviewLoadedInst;
    bool m_bankPreviewDirtyApplied;
    std::vector<unsigned char> m_bankPreviewSongBlob;
    struct BankPreviewNote {
        unsigned char channel;
        unsigned char note;
        unsigned char program;
        unsigned char bank;
    };
    std::unordered_map<int, BankPreviewNote> m_bankPreviewNotes;
    BAERmfEditorInstrumentExtInfo m_bankDirtyExtInfo;
    bool m_hasBankDirtyExtInfo;
    BAESound m_previewSound;
    std::unordered_map<int, BAESound> m_taggedPreviewSounds;
    struct TaggedInstrumentPreviewNote {
        unsigned char channel;
        unsigned char note;
        unsigned char program;
        unsigned char bank;
    };
    std::unordered_map<int, TaggedInstrumentPreviewNote> m_taggedInstrumentPreviewNotes;
    wxString m_playbackTempPath;
    wxString m_previewSampleTempPath;
    wxTimer m_playbackTimer;
    wxTimer m_notePreviewTimer;
    bool m_notePreviewActive;
    unsigned char m_notePreviewChannel;
    unsigned char m_notePreviewNote;
    uint16_t m_playbackChannelMask;
    bool m_openingPlaybackChannelsDialog = false;
    bool m_ignoreSeekEvent;
    bool m_autoFollowPlayhead;
    BAEBankToken m_bankToken;
    bool m_bankLoaded;
    std::vector<BAEBankToken> m_bankTokens; /* all loaded banks for enumeration */
    wxString m_loadedBankPath;
    wxMenuItem *m_currentBankMenuItem;
    wxMenuItem *m_reloadInternalBankMenuItem;
    wxMenu *m_settingsMenu;
    wxCheckBox *m_savePreviewToSongCheck;
    bool m_pendingLoopHotReload;
    uint32_t m_pendingLoopHotReloadPosUsec;
    bool m_pendingLoopHotReloadPaused;
    bool m_loadingLoopControls;
    std::vector<UndoEntry> m_undoStack;
    std::vector<UndoEntry> m_redoStack;
    UndoDocumentState m_pendingUndoState;
    wxString m_pendingUndoLabel;
    bool m_hasPendingUndo;
    bool m_restoringUndo;
    bool m_hasUnsavedChanges;
    bool m_bankHasUnsavedChanges;
    uint32_t m_cleanStateHash = 0;  // Hash of document when last saved

    static wxString GetIniPath() {
        return wxFileName::GetHomeDir() + "/.nbstudio.ini";
    }

    BAEReverbType GetSelectedPreviewReverbType() const {
        int selection;

        selection = m_previewReverbChoice ? m_previewReverbChoice->GetSelection() : 0;
        selection = std::clamp(selection, 0, 10);
        return static_cast<BAEReverbType>(static_cast<int>(BAE_REVERB_TYPE_1) + selection);
    }

    void SetSelectedPreviewReverbType(BAEReverbType reverbType) {
        int selection;

        selection = static_cast<int>(reverbType) - static_cast<int>(BAE_REVERB_TYPE_1);
        selection = std::clamp(selection, 0, 10);
        if (m_previewReverbChoice) {
            m_previewReverbChoice->SetSelection(selection);
        }
    }

    void ApplyPreviewReverbToMixer() {
        if (m_playbackMixer) {
            BAEMixer_SetDefaultReverb(m_playbackMixer, GetSelectedPreviewReverbType());
        }
    }

    // Apply Settings menu state to the global engine mixer flags.
    void ApplyMixerEngineSettings() {
        bool panFix = m_settingsMenu && m_settingsMenu->IsChecked(ID_SettingsPanFix);
        bool classicChorus = m_settingsMenu && m_settingsMenu->IsChecked(ID_SettingsClassicChorus);
        BAE_SetSpanDCFix(panFix ? TRUE : FALSE);
        BAE_SetClassicChorus(classicChorus ? TRUE : FALSE);
    }

    // If "Save Settings to Song" is checked, build and write engineConfigFlags
    // to the current document.
    void ApplyEngineConfigToDocument() {
        if (!m_document || !m_savePreviewToSongCheck) {
            return;
        }
        if (m_savePreviewToSongCheck->GetValue()) {
            bool panFix = m_settingsMenu && m_settingsMenu->IsChecked(ID_SettingsPanFix);
            bool classicChorus = m_settingsMenu && m_settingsMenu->IsChecked(ID_SettingsClassicChorus);
            int32_t flags = 0;
            flags |= SONG_CONFIG_HAS_PANFIX;
            if (panFix) {
                flags |= SONG_CONFIG_PANFIX_ON;
            }
            flags |= SONG_CONFIG_HAS_CLASSIC_CHORUS;
            if (classicChorus) {
                flags |= SONG_CONFIG_CLASSIC_CHORUS_ON;
            }
            BAERmfEditorDocument_SetEngineConfig(m_document, flags);
        } else {
            // Clear per-song engine config so it uses global defaults.
            BAERmfEditorDocument_SetEngineConfig(m_document, 0);
        }
    }

    void UpdateLoadedBankStatus() {
        wxString label;
        bool isInternalBankCurrent;

        label = "Current: Built-in";
        isInternalBankCurrent = m_loadedBankPath.empty();
        if (m_bankLoaded && m_bankToken && m_playbackMixer) {
            char friendlyBuf[128] = {0};
            bool hasFriendlyName;

            hasFriendlyName = (BAE_GetBankFriendlyName(m_playbackMixer,
                                                       m_bankToken,
                                                       friendlyBuf,
                                                       sizeof(friendlyBuf)) == BAE_NO_ERROR &&
                               friendlyBuf[0] != '\0');

            if (isInternalBankCurrent) {
                if (hasFriendlyName) {
                    label = wxString::Format("Current: Built-in (%s)", friendlyBuf);
                } else {
                    label = "Current: Built-in";
                }
            } else if (hasFriendlyName) {
                label = wxString::Format("Current: %s", friendlyBuf);
            } else if (!m_loadedBankPath.empty()) {
                label = wxString::Format("Current: %s", wxFileNameFromPath(m_loadedBankPath));
            }
        } else if (!m_loadedBankPath.empty()) {
            label = wxString::Format("Current: %s", wxFileNameFromPath(m_loadedBankPath));
        }

        if (m_currentBankMenuItem) {
            m_currentBankMenuItem->SetItemLabel(label);
        }
        if (m_reloadInternalBankMenuItem) {
            m_reloadInternalBankMenuItem->Enable(!isInternalBankCurrent);
        }
    }

    bool IsPreviewLoopEnabled() const {
        return m_previewLoopCheck ? m_previewLoopCheck->GetValue() : false;
    }

    void SyncPianoRollMidiLoopMarkersFromDocument() {
        XBOOL enabled;
        uint32_t startTick;
        uint32_t endTick;
        int32_t loopCount;

        if (!m_pianoRoll || !m_document) {
            if (m_pianoRoll) {
                PianoRollPanel_SetMidiLoopMarkers(m_pianoRoll, false, 0, 0);
            }
            return;
        }

        enabled = FALSE;
        startTick = 0;
        endTick = 0;
        loopCount = 0;
        if (BAERmfEditorDocument_GetMidiLoopMarkers(m_document,
                                                    &enabled,
                                                    &startTick,
                                                    &endTick,
                                                    &loopCount) == BAE_NO_ERROR &&
            enabled &&
            endTick > startTick) {
            PianoRollPanel_SetMidiLoopMarkers(m_pianoRoll, true, startTick, endTick);
        } else {
            PianoRollPanel_SetMidiLoopMarkers(m_pianoRoll, false, 0, 0);
        }
    }

    bool ApplyMidiLoopMarkersFromPianoRoll(bool enabled, uint32_t startTick, uint32_t endTick) {
        if (!m_document) {
            return false;
        }
        if (!enabled) {
            startTick = 0;
            endTick = 0;
        } else if (endTick <= startTick) {
            endTick = startTick + 1;
        }

        if (BAERmfEditorDocument_SetMidiLoopMarkers(m_document,
                                                    enabled ? TRUE : FALSE,
                                                    startTick,
                                                    endTick,
                                                    -1) != BAE_NO_ERROR) {
            return false;
        }

        m_loadingLoopControls = true;
        if (m_midiLoopEnableCheck) {
            m_midiLoopEnableCheck->SetValue(enabled);
        }
        if (m_midiLoopStartText) {
            m_midiLoopStartText->SetValue(FormatLoopTimeUsec(TicksToMicroseconds(startTick)));
            m_midiLoopStartText->Enable(enabled);
        }
        if (m_midiLoopEndText) {
            m_midiLoopEndText->SetValue(FormatLoopTimeUsec(TicksToMicroseconds(endTick)));
            m_midiLoopEndText->Enable(enabled);
        }
        m_loadingLoopControls = false;

        SyncPianoRollMidiLoopMarkersFromDocument();
        InvalidatePianoRollPreviewSong();
        RefreshPlaybackAtCurrentPosition();
        return true;
    }

    bool IsChannelsPlaybackScopeSelected() const {
        return m_playScopeChoice && m_playScopeChoice->GetSelection() == 2;
    }

    void UpdateChannelsConfigButtonState() {
        if (m_channelsConfigButton) {
            m_channelsConfigButton->Enable(IsChannelsPlaybackScopeSelected());
        }
    }

    void ApplyPreviewLoopSettingToSong() {
        if (m_playbackSong) {
            BAESong_SetLoops(m_playbackSong, IsPreviewLoopEnabled() ? 32767 : 0);
        }
    }

    void RefreshMidiLoopControlsFromDocument() {
        XBOOL enabled;
        uint32_t startTick;
        uint32_t endTick;
        int32_t loopCount;

        m_loadingLoopControls = true;
        if (!m_document ||
            BAERmfEditorDocument_GetMidiLoopMarkers(m_document,
                                                    &enabled,
                                                    &startTick,
                                                    &endTick,
                                                    &loopCount) != BAE_NO_ERROR) {
            if (m_midiLoopEnableCheck) m_midiLoopEnableCheck->SetValue(false);
            if (m_midiLoopStartText) m_midiLoopStartText->SetValue("0:00.000");
            if (m_midiLoopEndText) m_midiLoopEndText->SetValue("0:00.000");
        } else {
            if (m_midiLoopEnableCheck) m_midiLoopEnableCheck->SetValue(enabled != FALSE);
            if (m_midiLoopStartText) m_midiLoopStartText->SetValue(FormatLoopTimeUsec(TicksToMicroseconds(startTick)));
            if (m_midiLoopEndText) {
                uint32_t displayEnd = endTick;
                if (displayEnd == 0 && startTick > 0) {
                    displayEnd = startTick + 1;
                }
                m_midiLoopEndText->SetValue(FormatLoopTimeUsec(TicksToMicroseconds(displayEnd)));
            }
        }
        if (m_midiLoopStartText) m_midiLoopStartText->Enable(m_midiLoopEnableCheck && m_midiLoopEnableCheck->GetValue());
        if (m_midiLoopEndText) m_midiLoopEndText->Enable(m_midiLoopEnableCheck && m_midiLoopEnableCheck->GetValue());
        m_loadingLoopControls = false;
        SyncPianoRollMidiLoopMarkersFromDocument();
    }

    void LoadIniSettings() {
        wxFileConfig config(wxEmptyString,
                            wxEmptyString,
                            GetIniPath(),
                            wxEmptyString,
                            wxCONFIG_USE_LOCAL_FILE);
        long previewVolume;
        long previewReverb;
        bool previewLoop;
        long midiStorage;

        previewVolume = 100;
        if (config.Read("audio/preview_volume", &previewVolume) && m_previewVolumeSlider) {
            m_previewVolumeSlider->SetValue(std::clamp<long>(previewVolume, 0, 100));
        }
        previewReverb = static_cast<long>(BAE_REVERB_TYPE_1);
        if (config.Read("audio/preview_reverb", &previewReverb)) {
            SetSelectedPreviewReverbType(static_cast<BAEReverbType>(previewReverb));
        } else {
            SetSelectedPreviewReverbType(BAE_REVERB_TYPE_1);
        }
        previewLoop = false;
        if (config.Read("audio/preview_loop", &previewLoop) && m_previewLoopCheck) {
            m_previewLoopCheck->SetValue(previewLoop);
        }
        midiStorage = 1;
        if (config.Read("rmf/midi_storage_mode", &midiStorage) && m_midiStorageChoice) {
            m_midiStorageChoice->SetSelection(std::clamp<long>(midiStorage, 0, 3));
        }
        {
            if (m_settingsMenu) {
                m_settingsMenu->Check(ID_SettingsPanFix, true);
                m_settingsMenu->Check(ID_SettingsClassicChorus, false);
            }
            if (m_savePreviewToSongCheck) {
                m_savePreviewToSongCheck->SetValue(false);
            }
            ApplyMixerEngineSettings();
        }
    }

    void SaveIniSettings() {
        wxFileConfig config(wxEmptyString,
                            wxEmptyString,
                            GetIniPath(),
                            wxEmptyString,
                            wxCONFIG_USE_LOCAL_FILE);

        config.Write("audio/preview_volume", static_cast<long>(m_previewVolumeSlider ? m_previewVolumeSlider->GetValue() : 100));
        config.Write("audio/preview_reverb", static_cast<long>(GetSelectedPreviewReverbType()));
        config.Write("audio/preview_loop", IsPreviewLoopEnabled());
        config.Write("rmf/midi_storage_mode", static_cast<long>(m_midiStorageChoice ? m_midiStorageChoice->GetSelection() : 1));
        config.Flush();
    }

    BAERmfEditorMidiStorageType GetSelectedMidiStorageType() const {
        int selection;

        selection = m_midiStorageChoice ? m_midiStorageChoice->GetSelection() : 1;
        switch (selection) {
            case 0:
                return BAE_EDITOR_MIDI_STORAGE_CMID_BEST_EFFORT;
            case 2:
                return BAE_EDITOR_MIDI_STORAGE_EMID;
            case 3:
                return BAE_EDITOR_MIDI_STORAGE_MIDI;
            case 1:
            default:
                return BAE_EDITOR_MIDI_STORAGE_ECMI;
        }
    }

    void SetSelectedMidiStorageType(BAERmfEditorMidiStorageType storageType) {
        int selection;

        selection = 1;
        switch (storageType) {
            case BAE_EDITOR_MIDI_STORAGE_CMID_BEST_EFFORT:
                selection = 0;
                break;
            case BAE_EDITOR_MIDI_STORAGE_EMID:
                selection = 2;
                break;
            case BAE_EDITOR_MIDI_STORAGE_MIDI:
                selection = 3;
                break;
            case BAE_EDITOR_MIDI_STORAGE_ECMI:
            default:
                selection = 1;
                break;
        }
        if (m_midiStorageChoice) {
            m_midiStorageChoice->SetSelection(selection);
        }
    }

    BAE_UNSIGNED_FIXED GetPreviewVolumeFixed() const {
        int percent = 100;
        if (m_previewVolumeSlider) {
            percent = m_previewVolumeSlider->GetValue();
        }
        percent = std::clamp(percent, 0, 100);
        return static_cast<BAE_UNSIGNED_FIXED>((static_cast<double>(percent) / 100.0) * 65536.0);
    }

    double GetPreviewVolumeScale() const {
        int percent = 100;
        if (m_previewVolumeSlider) {
            percent = m_previewVolumeSlider->GetValue();
        }
        return static_cast<double>(std::clamp(percent, 0, 100)) / 100.0;
    }

    void ClearUndoHistory() {
        m_undoStack.clear();
        m_redoStack.clear();
        m_pendingUndoState = UndoDocumentState();
        m_pendingUndoLabel.clear();
        m_hasPendingUndo = false;
        // Capture the current document state hash as the "clean" state
        UndoDocumentState currentState;
        if (CaptureUndoState(&currentState)) {
            m_cleanStateHash = ComputeDocumentHash(currentState);
        }
        InvalidatePianoRollPreviewSong();
        UpdateUndoMenuState();
    }

    void MarkDocumentClean() {
        m_hasUnsavedChanges = false;
        // Update clean hash whenever saved
        UndoDocumentState currentState;
        if (CaptureUndoState(&currentState)) {
            m_cleanStateHash = ComputeDocumentHash(currentState);
        }
        UpdateFrameTitle();
    }

    void MarkDocumentDirty() {
        // Check if we're actually back at the clean state (user may have undone/reversed edits)
        if (m_cleanStateHash != 0) {
            UndoDocumentState currentState;
            if (CaptureUndoState(&currentState)) {
                uint32_t currentHash = ComputeDocumentHash(currentState);
                if (currentHash == m_cleanStateHash) {
                    // We're back at the original saved state, so mark clean
                    m_hasUnsavedChanges = false;
                    UpdateFrameTitle();
                    return;
                }
            }
        }
        
        if (!m_hasUnsavedChanges) {
            m_hasUnsavedChanges = true;
            UpdateFrameTitle();
        }
    }

    bool ConfirmDiscardUnsavedChanges(wxString const &action) {
        if (!m_hasUnsavedChanges || !m_document) {
            return true;
        }
        int choice = wxMessageBox(
            "You have unsaved changes.\n\nWould you like to save your session?",
            "Unsaved Changes",
            wxYES_NO | wxCANCEL | wxICON_WARNING,
            this);
        if (choice == wxCANCEL) {
            return false;
        }
        if (choice == wxYES) {
            if (m_sessionPath.empty()) {
                wxFileDialog dialog(this,
                                    "Save Session",
                                    wxEmptyString,
                                    GetDefaultSessionName(),
                                    "NeoBAE Session (*.nbs)|*.nbs",
                                    wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
                if (dialog.ShowModal() != wxID_OK) {
                    return false;
                }
                if (!WriteSessionToFile(dialog.GetPath())) {
                    return false;
                }
            } else {
                if (!WriteSessionToFile(m_sessionPath)) {
                    return false;
                }
            }
        }
        return true;
    }

    /* Prompt the user if the bank editor has unsaved changes before
     * replacing it with a different bank.  Returns true if safe to
     * proceed, false if the user cancelled. */
    bool ConfirmDiscardBankChanges() {
        if (!m_bankHasUnsavedChanges) {
            return true;
        }
        int choice = wxMessageBox(
            "The current bank has unsaved changes.\n\n"
            "Would you like to save the bank before loading a new one?",
            "Unsaved Bank Changes",
            wxYES_NO | wxCANCEL | wxICON_WARNING,
            this);
        if (choice == wxCANCEL) {
            return false;
        }
        if (choice == wxYES) {
            wxCommandEvent dummy;
            OnBankSave(dummy);
            /* If the path was empty, OnBankSave redirects to SaveAs.
             * If the user cancelled SaveAs, the flag is still set. */
            if (m_bankHasUnsavedChanges) {
                return false;
            }
        }
        /* User chose No, or save succeeded — discard is OK */
        m_bankHasUnsavedChanges = false;
        return true;
    }

    void OnCloseWindow(wxCloseEvent &event) {
        if (event.CanVeto() && m_hasUnsavedChanges && m_document) {
            int choice = wxMessageBox(
                "You have unsaved changes.\n\nSave session before exiting?",
                "Unsaved Changes",
                wxYES_NO | wxCANCEL | wxICON_WARNING,
                this);
            if (choice == wxCANCEL) {
                event.Veto();
                return;
            }
            if (choice == wxYES) {
                if (m_sessionPath.empty()) {
                    wxFileDialog dialog(this,
                                        "Save Session",
                                        wxEmptyString,
                                        GetDefaultSessionName(),
                                        "NeoBAE Session (*.nbs)|*.nbs",
                                        wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
                    if (dialog.ShowModal() != wxID_OK) {
                        event.Veto();
                        return;
                    }
                    if (!WriteSessionToFile(dialog.GetPath())) {
                        event.Veto();
                        return;
                    }
                } else {
                    if (!WriteSessionToFile(m_sessionPath)) {
                        event.Veto();
                        return;
                    }
                }
            }
        }
        if (event.CanVeto() && m_bankHasUnsavedChanges) {
            int choice = wxMessageBox(
                "The bank has unsaved changes.\n\nSave bank before exiting?",
                "Unsaved Bank Changes",
                wxYES_NO | wxCANCEL | wxICON_WARNING,
                this);
            if (choice == wxCANCEL) {
                event.Veto();
                return;
            }
            if (choice == wxYES) {
                wxCommandEvent dummy;
                OnBankSave(dummy);
                if (m_bankHasUnsavedChanges) {
                    event.Veto();
                    return;
                }
            }
        }
        Destroy();
    }

    void UpdateUndoMenuState() {
        wxMenuBar *menuBar;

        menuBar = GetMenuBar();
        if (menuBar) {
            menuBar->Enable(wxID_UNDO, !m_undoStack.empty() && m_document != nullptr);
            menuBar->Enable(wxID_REDO, !m_redoStack.empty() && m_document != nullptr);
        }
    }

    bool CaptureSerializedDocument(std::vector<unsigned char> *outBytes) const {
        unsigned char *data;
        uint32_t size;
        XBOOL useZmf;

        if (!outBytes || !m_document) {
            return false;
        }

        data = nullptr;
        size = 0;
        useZmf = BAERmfEditorDocument_RequiresZmf(m_document) ? TRUE : FALSE;
        if (BAERmfEditorDocument_SaveAsRmfToMemory(m_document,
                                                    useZmf,
                                                    &data,
                                                    &size) != BAE_NO_ERROR || !data || size == 0) {
            return false;
        }

        outBytes->assign(data, data + size);
        XDisposePtr((XPTR)data);
        return true;
    }

    bool RestoreSerializedDocument(std::vector<unsigned char> const &bytes) {
        BAERmfEditorDocument *loadedDocument;

        if (bytes.empty()) {
            return false;
        }

        loadedDocument = BAERmfEditorDocument_LoadFromMemory(bytes.data(),
                                                             static_cast<uint32_t>(bytes.size()),
                                                             BAE_RMF);
        if (!loadedDocument) {
            return false;
        }

        if (m_document) {
            BAERmfEditorDocument_Delete(m_document);
        }
        m_document = loadedDocument;
        /* Keep the piano roll's document pointer in sync so it doesn't
         * access the freed document the next time it redraws or receives
         * a SetSelectedTrack call. */
        PianoRollPanel_SetDocument(m_pianoRoll, m_document);
        return true;
    }

    bool CaptureUndoState(UndoDocumentState *outState) const {
        uint16_t trackCount;
        uint32_t tempoCount;

        if (!outState || !m_document) {
            return false;
        }
        outState->tempoBpm = 120;
        BAERmfEditorDocument_GetTempoBPM(m_document, &outState->tempoBpm);
        outState->tempoEvents.clear();
        outState->tracks.clear();
        outState->serializedDocument.clear();

        tempoCount = 0;
        if (BAERmfEditorDocument_GetTempoEventCount(m_document, &tempoCount) == BAE_NO_ERROR) {
            outState->tempoEvents.reserve(tempoCount);
            for (uint32_t eventIndex = 0; eventIndex < tempoCount; ++eventIndex) {
                UndoTempoEventState eventState;

                if (BAERmfEditorDocument_GetTempoEvent(m_document,
                                                       eventIndex,
                                                       &eventState.tick,
                                                       &eventState.microsecondsPerQuarter) == BAE_NO_ERROR) {
                    outState->tempoEvents.push_back(eventState);
                }
            }
        }

        trackCount = 0;
        if (BAERmfEditorDocument_GetTrackCount(m_document, &trackCount) != BAE_NO_ERROR) {
            return false;
        }
        outState->tracks.resize(trackCount);
        for (uint16_t trackIndex = 0; trackIndex < trackCount; ++trackIndex) {
            UndoTrackState &trackState = outState->tracks[trackIndex];
            uint32_t noteCount;

            noteCount = 0;
            if (BAERmfEditorDocument_GetNoteCount(m_document, trackIndex, &noteCount) == BAE_NO_ERROR) {
                trackState.notes.reserve(noteCount);
                for (uint32_t noteIndex = 0; noteIndex < noteCount; ++noteIndex) {
                    BAERmfEditorNoteInfo noteInfo;

                    if (BAERmfEditorDocument_GetNoteInfo(m_document, trackIndex, noteIndex, &noteInfo) == BAE_NO_ERROR) {
                        trackState.notes.push_back(noteInfo);
                    }
                }
            }
            for (int controllerValue = 0; controllerValue < 128; ++controllerValue) {
                uint32_t eventCount;
                unsigned char controller;

                controller = static_cast<unsigned char>(controllerValue);
                eventCount = 0;
                if (BAERmfEditorDocument_GetTrackCCEventCount(m_document, trackIndex, controller, &eventCount) != BAE_NO_ERROR) {
                    continue;
                }
                trackState.ccEvents.reserve(trackState.ccEvents.size() + eventCount);
                for (uint32_t eventIndex = 0; eventIndex < eventCount; ++eventIndex) {
                    UndoCCEventState eventState;

                    if (BAERmfEditorDocument_GetTrackCCEvent(m_document,
                                                             trackIndex,
                                                             controller,
                                                             eventIndex,
                                                             &eventState.tick,
                                                             &eventState.value) == BAE_NO_ERROR) {
                        eventState.controller = controller;
                        trackState.ccEvents.push_back(eventState);
                    }
                }
            }
        }
        if (!CaptureSerializedDocument(&outState->serializedDocument)) {
            outState->serializedDocument.clear();
        }
        // Capture current piano roll scroll position
        outState->pianoScrollVX = 0;
        outState->pianoScrollVY = 0;
        if (wxScrolledWindow *sw = dynamic_cast<wxScrolledWindow *>(PianoRollPanel_AsWindow(m_pianoRoll))) {
            sw->GetViewStart(&outState->pianoScrollVX, &outState->pianoScrollVY);
        }
        return true;
    }

    bool RestoreUndoState(UndoDocumentState const &state) {
        uint16_t trackCount;

        if (!m_document) {
            return false;
        }
        if (!state.serializedDocument.empty()) {
            return RestoreSerializedDocument(state.serializedDocument);
        }
        trackCount = 0;
        if (BAERmfEditorDocument_GetTrackCount(m_document, &trackCount) != BAE_NO_ERROR || trackCount != state.tracks.size()) {
            return false;
        }
        if (BAERmfEditorDocument_SetTempoBPM(m_document, state.tempoBpm) != BAE_NO_ERROR) {
            return false;
        }
        {
            uint32_t tempoCount;

            tempoCount = 0;
            BAERmfEditorDocument_GetTempoEventCount(m_document, &tempoCount);
            for (uint32_t eventIndex = tempoCount; eventIndex > 0; --eventIndex) {
                BAERmfEditorDocument_DeleteTempoEvent(m_document, eventIndex - 1);
            }
            for (UndoTempoEventState const &eventState : state.tempoEvents) {
                if (BAERmfEditorDocument_AddTempoEvent(m_document,
                                                       eventState.tick,
                                                       eventState.microsecondsPerQuarter) != BAE_NO_ERROR) {
                    return false;
                }
            }
        }
        for (uint16_t trackIndex = 0; trackIndex < trackCount; ++trackIndex) {
            UndoTrackState const &trackState = state.tracks[trackIndex];
            uint32_t noteCount;

            noteCount = 0;
            BAERmfEditorDocument_GetNoteCount(m_document, trackIndex, &noteCount);
            for (uint32_t noteIndex = noteCount; noteIndex > 0; --noteIndex) {
                BAERmfEditorDocument_DeleteNote(m_document, trackIndex, noteIndex - 1);
            }
            for (BAERmfEditorNoteInfo const &noteInfo : trackState.notes) {
                if (BAERmfEditorDocument_AddNote(m_document,
                                                 trackIndex,
                                                 noteInfo.startTick,
                                                 noteInfo.durationTicks,
                                                 noteInfo.note,
                                                 noteInfo.velocity) != BAE_NO_ERROR) {
                    return false;
                }
            }
            for (uint32_t noteIndex = 0; noteIndex < trackState.notes.size(); ++noteIndex) {
                if (BAERmfEditorDocument_SetNoteInfo(m_document,
                                                     trackIndex,
                                                     noteIndex,
                                                     &trackState.notes[noteIndex]) != BAE_NO_ERROR) {
                    return false;
                }
            }
            for (int controllerValue = 0; controllerValue < 128; ++controllerValue) {
                uint32_t eventCount;
                unsigned char controller;

                controller = static_cast<unsigned char>(controllerValue);
                eventCount = 0;
                BAERmfEditorDocument_GetTrackCCEventCount(m_document, trackIndex, controller, &eventCount);
                for (uint32_t eventIndex = eventCount; eventIndex > 0; --eventIndex) {
                    BAERmfEditorDocument_DeleteTrackCCEvent(m_document, trackIndex, controller, eventIndex - 1);
                }
            }
            for (UndoCCEventState const &eventState : trackState.ccEvents) {
                if (eventState.controller > 127) {
                    continue;
                }
                    if (BAERmfEditorDocument_AddTrackCCEvent(m_document,
                                                             trackIndex,
                                                             eventState.controller,
                                                             eventState.tick,
                                                             eventState.value) != BAE_NO_ERROR) {
                        return false;
                    }
                }
        }
        return true;
    }

    void BeginUndoAction(wxString const &label) {
        if (!m_document || m_restoringUndo) {
            return;
        }
        if (!CaptureUndoState(&m_pendingUndoState)) {
            return;
        }
        m_pendingUndoLabel = label;
        m_hasPendingUndo = true;
    }

    void CommitUndoAction(wxString const &label) {
        UndoDocumentState afterState;
        UndoEntry entry;

        if (!m_document || !m_hasPendingUndo || m_restoringUndo) {
            return;
        }
        if (!CaptureUndoState(&afterState)) {
            CancelUndoAction();
            return;
        }
        if (UndoSnapshotsEqual(m_pendingUndoState, afterState)) {
            CancelUndoAction();
            return;
        }
        entry.label = label.empty() ? m_pendingUndoLabel : label;
        entry.before = m_pendingUndoState;
        entry.after = afterState;
        entry.before.documentHash = ComputeDocumentHash(entry.before);
        entry.after.documentHash = ComputeDocumentHash(entry.after);
        m_undoStack.push_back(entry);
        m_redoStack.clear();
        if (m_undoStack.size() > 128) {
            m_undoStack.erase(m_undoStack.begin());
        }
        m_pendingUndoState = UndoDocumentState();
        m_pendingUndoLabel.clear();
        m_hasPendingUndo = false;
        MarkDocumentDirty();
        UpdateUndoMenuState();
    }

    void CancelUndoAction() {
        m_pendingUndoState = UndoDocumentState();
        m_pendingUndoLabel.clear();
        m_hasPendingUndo = false;
    }

    void OnUndo(wxCommandEvent &) {
        UndoEntry entry;

        if (!m_document || m_undoStack.empty()) {
            return;
        }
        entry = m_undoStack.back();
        m_undoStack.pop_back();
        m_restoringUndo = true;
        if (!RestoreUndoState(entry.before)) {
            m_restoringUndo = false;
            m_undoStack.push_back(entry);
            wxMessageBox("Failed to restore the previous note/event state.", "Undo Failed", wxOK | wxICON_ERROR, this);
            UpdateUndoMenuState();
            return;
        }
        m_restoringUndo = false;
        m_redoStack.push_back(entry);
        if (m_redoStack.size() > 128) {
            m_redoStack.erase(m_redoStack.begin());
        }
        {
            int savedTrack = GetSelectedTrack();
            PopulateTrackList();
            if (savedTrack >= 0 && savedTrack < static_cast<int>(m_trackList->GetCount())) {
                m_trackList->SetSelection(savedTrack);
                PianoRollPanel_SetSelectedTrack(m_pianoRoll, savedTrack);
            }
        }
        PopulateSampleList();
        RefreshMidiLoopControlsFromDocument();
        PianoRollPanel_RefreshFromDocument(m_pianoRoll, true);
        // Restore scroll position that was captured when this undo entry was created
        if (wxScrolledWindow *sw = dynamic_cast<wxScrolledWindow *>(PianoRollPanel_AsWindow(m_pianoRoll))) {
            sw->Scroll(entry.before.pianoScrollVX, entry.before.pianoScrollVY);
        }
        InvalidatePianoRollPreviewSong();
        UpdateControlsFromSelection();
        // MarkDocumentDirty() checks if we're back at the clean hash and clears dirty if so
        MarkDocumentDirty();
        SetStatusText(entry.label.empty() ? "Undo" : wxString::Format("Undid %s", entry.label), 0);
        UpdateUndoMenuState();
    }

    void OnRedo(wxCommandEvent &) {
        UndoEntry entry;

        if (!m_document || m_redoStack.empty()) {
            return;
        }
        entry = m_redoStack.back();
        m_redoStack.pop_back();
        m_restoringUndo = true;
        if (!RestoreUndoState(entry.after)) {
            m_restoringUndo = false;
            m_redoStack.push_back(entry);
            wxMessageBox("Failed to restore the redone note/event state.", "Redo Failed", wxOK | wxICON_ERROR, this);
            UpdateUndoMenuState();
            return;
        }
        m_restoringUndo = false;
        m_undoStack.push_back(entry);
        if (m_undoStack.size() > 128) {
            m_undoStack.erase(m_undoStack.begin());
        }
        {
            int savedTrack = GetSelectedTrack();
            PopulateTrackList();
            if (savedTrack >= 0 && savedTrack < static_cast<int>(m_trackList->GetCount())) {
                m_trackList->SetSelection(savedTrack);
                PianoRollPanel_SetSelectedTrack(m_pianoRoll, savedTrack);
            }
        }
        PopulateSampleList();
        RefreshMidiLoopControlsFromDocument();
        PianoRollPanel_RefreshFromDocument(m_pianoRoll, true);
        // Restore scroll position that was captured when this undo entry was created
        if (wxScrolledWindow *sw = dynamic_cast<wxScrolledWindow *>(PianoRollPanel_AsWindow(m_pianoRoll))) {
            sw->Scroll(entry.after.pianoScrollVX, entry.after.pianoScrollVY);
        }
        InvalidatePianoRollPreviewSong();
        UpdateControlsFromSelection();
        // MarkDocumentDirty() checks if we're back at the clean hash and clears dirty if so
        MarkDocumentDirty();
        SetStatusText(entry.label.empty() ? "Redo" : wxString::Format("Redid %s", entry.label), 0);
        UpdateUndoMenuState();
    }

    bool EnsurePlaybackEngine() {
        if (m_playbackMixer) {
            return true;
        }
        fprintf(stderr, "[nbstudio] Creating mixer...\n");
        m_playbackMixer = BAEMixer_New();
        if (!m_playbackMixer) {
            fprintf(stderr, "[nbstudio] BAEMixer_New() failed\n");
            return false;
        }
        BAEResult openResult = BAEMixer_Open(m_playbackMixer,
                                             BAE_RATE_44K,
                                             BAE_LINEAR_INTERPOLATION,
                                             BAE_USE_16 | BAE_USE_STEREO,
                                             64,
                                             16,
                                             32,
                                             TRUE);
        fprintf(stderr, "[nbstudio] BAEMixer_Open result=%d\n", static_cast<int>(openResult));
        if (openResult != BAE_NO_ERROR) {
            BAEMixer_Delete(m_playbackMixer);
            m_playbackMixer = nullptr;
            return false;
        }
        ApplyPreviewReverbToMixer();
        ApplyMixerEngineSettings();
        {
            BAERate actualRate;
            if (BAEMixer_GetRate(m_playbackMixer, &actualRate) == BAE_NO_ERROR) {
                fprintf(stderr, "[nbstudio] Mixer active rate=%ld\n", static_cast<long>(actualRate));
            }
        }
        if (!m_bankLoaded) {
#ifdef _BUILT_IN_PATCHES
            fprintf(stderr, "[nbstudio] Loading built-in bank...\n");
            BAEResult bankResult = BAEMixer_LoadBuiltinBank(m_playbackMixer, &m_bankToken);
            fprintf(stderr, "[nbstudio] BAEMixer_LoadBuiltinBank result=%d\n", static_cast<int>(bankResult));
            if (bankResult == BAE_NO_ERROR) {
                m_bankLoaded = true;
                m_bankTokens.push_back(m_bankToken);
                m_loadedBankPath.clear();
            }
#else
            fprintf(stderr, "[nbstudio] WARNING: _BUILT_IN_PATCHES not defined, no bank loaded!\n");
#endif
        }
        UpdateLoadedBankStatus();
        return true;
    }

    bool LoadBankFromFile(wxString const &path) {
        BAEBankToken newToken;
        wxScopedCharBuffer utf8;

        if (!EnsurePlaybackEngine()) {
            return false;
        }
        /* Unload existing banks before loading new one */
        StopPlayback(true);
        BAEMixer_UnloadBanks(m_playbackMixer);
        m_bankToken = nullptr;
        m_bankLoaded = false;
        m_bankTokens.clear();
        m_loadedBankPath.clear();
        /* Reload internal bank first so it remains available */
#ifdef _BUILT_IN_PATCHES
        {
            BAEBankToken builtinToken;
            if (BAEMixer_LoadBuiltinBank(m_playbackMixer, &builtinToken) == BAE_NO_ERROR) {
                m_bankTokens.push_back(builtinToken);
            }
        }
#endif
        utf8 = path.utf8_str();
        if (BAEMixer_AddBankFromFile(m_playbackMixer, const_cast<char *>(utf8.data()), &newToken) != BAE_NO_ERROR) {
            /* Restore internal bank state even if external load fails */
            if (!m_bankTokens.empty()) {
                m_bankToken = m_bankTokens[0];
                m_bankLoaded = true;
            }
            UpdateLoadedBankStatus();
            return false;
        }
        m_bankToken = newToken;
        m_bankLoaded = true;
        m_bankTokens.push_back(newToken);
        m_loadedBankPath = path;
        char friendlyBuf[128];
        wxString bankName;
        if (BAE_GetBankFriendlyName(m_playbackMixer, newToken, friendlyBuf, sizeof(friendlyBuf)) == BAE_NO_ERROR) {
            bankName = wxString::Format("Bank loaded: %s", friendlyBuf);
        } else {
            bankName = wxString::Format("Bank loaded: %s", wxFileNameFromPath(path));
        }
        SetStatusText(bankName, 0);
        UpdateLoadedBankStatus();
        return true;
    }

    void OnBankSave(wxCommandEvent &) {
        if (!m_bankToken || m_loadedBankPath.empty()) {
            wxCommandEvent dummy;
            OnBankSaveAs(dummy);
            return;
        }
        wxScopedCharBuffer utf8 = m_loadedBankPath.utf8_str();
        BAEResult result = BAERmfEditorBank_SaveToFile(m_bankToken, const_cast<char *>(utf8.data()));
        if (result != BAE_NO_ERROR) {
            wxMessageBox(wxString::Format("Failed to save bank (error %d).", static_cast<int>(result)),
                         "Save Bank", wxOK | wxICON_ERROR, this);
        } else {
            m_bankHasUnsavedChanges = false;
            SetStatusText(wxString("Bank saved: ") + wxFileNameFromPath(m_loadedBankPath));
        }
    }

    void OnBankSaveAs(wxCommandEvent &) {
        if (!m_bankToken) {
            wxMessageBox("No bank is loaded.", "Save Bank As", wxOK | wxICON_INFORMATION, this);
            return;
        }
        wxFileDialog dialog(this,
                            "Save Bank As",
                            wxEmptyString,
                            wxEmptyString,
                            "HSB Bank (*.hsb)|*.hsb|ZSB Bank (*.zsb)|*.zsb|All files (*.*)|*.*",
                            wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dialog.ShowModal() == wxID_OK) {
            wxString savePath = dialog.GetPath();
            wxScopedCharBuffer utf8 = savePath.utf8_str();
            BAEResult result = BAERmfEditorBank_SaveToFile(m_bankToken, const_cast<char *>(utf8.data()));
            if (result != BAE_NO_ERROR) {
                wxMessageBox(wxString::Format("Failed to save bank (error %d).", static_cast<int>(result)),
                             "Save Bank As", wxOK | wxICON_ERROR, this);
            } else {
                m_loadedBankPath = savePath;
                m_bankHasUnsavedChanges = false;
                SetTitle(wxString("NeoBAE Studio - ") + wxFileNameFromPath(savePath));
                SetStatusText(wxString("Bank saved: ") + wxFileNameFromPath(savePath));
            }
        }
    }

    void OnBankLoadBuiltin(wxCommandEvent &) {
#ifdef _BUILT_IN_PATCHES
        if (!ConfirmDiscardBankChanges()) {
            return;
        }
        if (!EnsurePlaybackEngine()) {
            wxMessageBox("Failed to initialize audio engine.",
                         "Load Built-in Bank", wxOK | wxICON_ERROR, this);
            return;
        }
        /* Unload existing banks */
        StopPlayback(true);
        BAEMixer_UnloadBanks(m_playbackMixer);
        m_bankToken = nullptr;
        m_bankLoaded = false;
        m_bankTokens.clear();
        m_loadedBankPath.clear();

        BAEBankToken builtinToken;
        BAEResult result = BAEMixer_LoadBuiltinBank(m_playbackMixer, &builtinToken);
        if (result != BAE_NO_ERROR) {
            wxMessageBox(wxString::Format("Failed to load built-in bank (error %d).", static_cast<int>(result)),
                         "Load Built-in Bank", wxOK | wxICON_ERROR, this);
            return;
        }
        m_bankToken = builtinToken;
        m_bankLoaded = true;
        m_bankTokens.push_back(builtinToken);
        UpdateLoadedBankStatus();

        SetTitle("NeoBAE Studio - Built-in Bank");
        if (m_bankEditorPanel) {
            BankEditorPanel_LoadBank(m_bankEditorPanel, m_bankToken, "(built-in)");
        }
        m_bankHasUnsavedChanges = false;
        SwitchToEditorTab(kEditorModeBank);
#else
        wxMessageBox("Built-in patches are not available in this build.",
                     "Load Built-in Bank", wxOK | wxICON_INFORMATION, this);
#endif
    }

    /* Load a bank file into BOTH the mixer (for playback) and the bank
     * editor (for browsing/editing).  If switchToBank is true the UI
     * switches to the Bank Editor tab; otherwise the current tab is kept
     * (useful when loading from the MIDI editor's Sound Bank menu). */
    void LoadBankForEditing(wxString const &path, bool switchToBank = true) {
        fprintf(stderr, "[nbstudio] LoadBankForEditing: %s\n", static_cast<const char *>(path.utf8_str()));
        if (!EnsurePlaybackEngine()) {
            wxMessageBox("Failed to initialize audio engine.",
                         "Bank Load Error", wxOK | wxICON_ERROR, this);
            return;
        }
        fprintf(stderr, "[nbstudio] LoadBankForEditing: engine ready, loading bank...\n");
        if (!LoadBankFromFile(path)) {
            wxMessageBox(wxString::Format("Failed to load bank file:\n%s", path),
                         "Bank Load Error", wxOK | wxICON_ERROR, this);
            return;
        }
        fprintf(stderr, "[nbstudio] LoadBankForEditing: bank loaded, bankToken=%p\n", static_cast<void *>(m_bankToken));
        SetTitle(wxString::Format("NeoBAE Studio v%s - %s", kVersionString, wxFileNameFromPath(path)));
        fprintf(stderr, "[nbstudio] LoadBankForEditing: populating bank editor...\n");
        if (m_bankEditorPanel) {
            wxScopedCharBuffer utf8Path = path.utf8_str();
            BankEditorPanel_LoadBank(m_bankEditorPanel, m_bankToken, utf8Path.data());
        }
        m_bankHasUnsavedChanges = false;
        if (switchToBank) {
            fprintf(stderr, "[nbstudio] LoadBankForEditing: switching to bank tab...\n");
            SwitchToEditorTab(kEditorModeBank);
        }
        fprintf(stderr, "[nbstudio] LoadBankForEditing: done\n");
    }

    void PopulateSampleList() {
        uint32_t assetCount;
        wxTreeItemId root;

        m_sampleTree->DeleteAllItems();
        root = m_sampleTree->AddRoot("Sample Assets");
        if (!m_document) {
            return;
        }
        assetCount = 0;
        if (BAERmfEditorDocument_GetSampleAssetCount(m_document, &assetCount) != BAE_NO_ERROR) {
            return;
        }
        for (uint32_t assetIndex = 0; assetIndex < assetCount; ++assetIndex) {
            BAERmfEditorSampleAssetInfo assetInfo;
            wxTreeItemId assetNode;
            wxString assetName;
            wxString assetLabel;

            if (BAERmfEditorDocument_GetSampleAssetInfo(m_document, assetIndex, &assetInfo) != BAE_NO_ERROR) {
                continue;
            }
            assetName = assetInfo.displayName ? assetInfo.displayName : "(unnamed sample)";
            assetLabel = wxString::Format("A%u: %s (%u uses)",
                                          static_cast<unsigned>(assetInfo.assetID),
                                          assetName,
                                          static_cast<unsigned>(assetInfo.usageCount));
            assetNode = m_sampleTree->AppendItem(root,
                                                 assetLabel,
                                                 -1,
                                                 -1,
                                                 new SampleTreeItemData(assetInfo.assetID,
                                                                        static_cast<uint32_t>(-1),
                                                                        true));

            for (uint32_t usageIndex = 0; usageIndex < assetInfo.usageCount; ++usageIndex) {
                uint32_t sampleIndex;
                BAERmfEditorSampleInfo sampleInfo;
                uint32_t instID;
                BAERmfEditorInstrumentExtInfo instInfo;
                wxString usageLabel;
                wxString instName;

                if (BAERmfEditorDocument_GetSampleAssetSampleIndex(m_document,
                                                                   assetInfo.assetID,
                                                                   usageIndex,
                                                                   &sampleIndex) != BAE_NO_ERROR) {
                    continue;
                }
                if (BAERmfEditorDocument_GetSampleInfo(m_document, sampleIndex, &sampleInfo) != BAE_NO_ERROR) {
                    continue;
                }

                instName = sampleInfo.displayName ? sampleInfo.displayName : "(unnamed instrument)";
                if (BAERmfEditorDocument_GetInstIDForSample(m_document, sampleIndex, &instID) == BAE_NO_ERROR &&
                    BAERmfEditorDocument_GetInstrumentExtInfo(m_document, instID, &instInfo) == BAE_NO_ERROR &&
                    instInfo.displayName && instInfo.displayName[0]) {
                    instName = instInfo.displayName;
                }

                {
                    XBOOL isAlias = FALSE;
                    BAERmfEditorDocument_IsSampleBankAlias(m_document, sampleIndex, &isAlias);
                    usageLabel = wxString::Format("%sP%u %s [%u-%u] rk=%u",
                                                  isAlias ? "[Alias] " : "",
                                                  static_cast<unsigned>(sampleInfo.program),
                                                  instName,
                                                  static_cast<unsigned>(sampleInfo.lowKey),
                                                  static_cast<unsigned>(sampleInfo.highKey),
                                                  static_cast<unsigned>(sampleInfo.rootKey));
                }
                m_sampleTree->AppendItem(assetNode,
                                         usageLabel,
                                         -1,
                                         -1,
                                         new SampleTreeItemData(assetInfo.assetID,
                                                                sampleIndex,
                                                                false));
            }
            m_sampleTree->Expand(assetNode);
        }
        m_sampleTree->Expand(root);
    }

    SampleTreeItemData *GetSelectedSampleTreeData() const {
        wxTreeItemId selected;

        selected = m_sampleTree->GetSelection();
        if (!selected.IsOk()) {
            return nullptr;
        }
        return dynamic_cast<SampleTreeItemData *>(m_sampleTree->GetItemData(selected));
    }

    bool GetSelectedSampleIndexFromTree(uint32_t *outSampleIndex) const {
        SampleTreeItemData *data;

        if (!outSampleIndex) {
            return false;
        }
        data = GetSelectedSampleTreeData();
        if (!data) {
            return false;
        }

        if (!data->IsAssetNode()) {
            *outSampleIndex = data->GetSampleIndex();
            return true;
        }

        {
            uint32_t sampleIndex;
            if (BAERmfEditorDocument_GetSampleAssetSampleIndex(m_document,
                                                               data->GetAssetID(),
                                                               0,
                                                               &sampleIndex) == BAE_NO_ERROR) {
                *outSampleIndex = sampleIndex;
                return true;
            }
        }
        return false;
    }

    void SelectTreeItemForSample(uint32_t sampleIndex) {
        wxTreeItemId root;
        wxTreeItemIdValue assetCookie;
        wxTreeItemId assetNode;

        root = m_sampleTree->GetRootItem();
        if (!root.IsOk()) {
            return;
        }

        assetNode = m_sampleTree->GetFirstChild(root, assetCookie);
        while (assetNode.IsOk()) {
            wxTreeItemIdValue usageCookie;
            wxTreeItemId usageNode = m_sampleTree->GetFirstChild(assetNode, usageCookie);
            while (usageNode.IsOk()) {
                SampleTreeItemData *data;

                data = dynamic_cast<SampleTreeItemData *>(m_sampleTree->GetItemData(usageNode));
                if (data && !data->IsAssetNode() && data->GetSampleIndex() == sampleIndex) {
                    m_sampleTree->SelectItem(usageNode);
                    return;
                }
                usageNode = m_sampleTree->GetNextChild(assetNode, usageCookie);
            }
            assetNode = m_sampleTree->GetNextChild(root, assetCookie);
        }
    }

    bool PreviewInstrumentDialogNote(uint32_t sampleIndex,
                                     int midiKey,
                                     int previewTag,
                                     BAESampleInfo const *overrideInfo,
                                     int16_t splitVolumeOverride,
                                     unsigned char rootKeyOverride,
                                     BAERmfEditorCompressionType compressionOverride,
                                     bool opusRoundTripOverride,
                                     BAERmfEditorInstrumentExtInfo const *extOverride) {
        BAERmfEditorSampleInfo sampleInfo;
        unsigned char channel;
        BAEResult result;
        BAEResult loadInstrumentResult;
        uint32_t previewEventTime;
        uint32_t previewNoteOnTime;
        bool armPreview;
        uint32_t instID;
        uint32_t selectedInstID;
        uint32_t bankId;
        uint32_t progId;
        uint32_t noteId;
        bool hasSelectedInstID;
        BAE_INSTRUMENT instrumentID;
        BAE_INSTRUMENT translatedInstrumentID;
        unsigned char previewProgram;
        unsigned char previewBank;
        bool hasDialogOverrides;
        auto mapPreviewTagToChannel = [](int tag) -> unsigned char {
            switch (tag) {
                /* Match InstrumentExtEditorDialog musical typing order:
                 * A W S E D F T G Y H U J K O */
                case 'A': return 0;
                case 'W': return 1;
                case 'S': return 2;
                case 'E': return 3;
                case 'D': return 4;
                case 'F': return 5;
                case 'T': return 6;
                case 'G': return 7;
                case 'Y': return 8;
                case 'H': return 15;
                case 'U': return 11;
                case 'J': return 12;
                case 'K': return 13;
                case 'O': return 14;
                default: return 0;
            }
        };

        if (!m_document) {
            return false;
        }
        if (BAERmfEditorDocument_GetSampleInfo(m_document, sampleIndex, &sampleInfo) != BAE_NO_ERROR) {
            return false;
        }

        auto resolvePreviewInstrumentID = [](BAERmfEditorDocument *document,
                                             uint32_t targetSampleIndex,
                                             unsigned char fallbackProgram,
                                             uint32_t *outInstID) -> bool {
            uint32_t resolvedInstID;

            if (!document || !outInstID) {
                return false;
            }
            if (BAERmfEditorDocument_GetInstIDForSample(document,
                                                        targetSampleIndex,
                                                        &resolvedInstID) != BAE_NO_ERROR) {
                return false;
            }
            if (resolvedInstID == 0) {
                resolvedInstID = 512U + static_cast<uint32_t>(fallbackProgram);
            }
            *outInstID = resolvedInstID;
            return true;
        };

        auto buildMinimalInstrumentPreviewDocument = [&](uint32_t sourceSampleIndex,
                                                         uint32_t resolvedInstID,
                                                         unsigned char resolvedProgram,
                                                         unsigned char resolvedBank,
                                                         unsigned char preloadNote,
                                                         uint32_t *outPreviewSampleIndex) -> BAERmfEditorDocument * {
            BAERmfEditorDocument *previewDocument;
            unsigned char programFlags[128];
            uint32_t copiedCount;
            uint32_t previewSampleCount;
            uint32_t mappedSampleIndex;
            BAERmfEditorSampleInfo sourceSampleInfo;
            BAERmfEditorTrackSetup preloadTrackSetup;
            uint16_t preloadTrackIndex;

            previewDocument = BAERmfEditorDocument_New();
            if (!previewDocument) {
                return nullptr;
            }

            XSetMemory(programFlags, sizeof(programFlags), 0);
            programFlags[resolvedProgram] = 1;
            copiedCount = 0;
            if (BAERmfEditorDocument_CopySamplesForPrograms(previewDocument,
                                                            m_document,
                                                            programFlags,
                                                            &copiedCount) != BAE_NO_ERROR ||
                copiedCount == 0) {
                BAERmfEditorDocument_Delete(previewDocument);
                return nullptr;
            }

            /* Keep only samples for the current instrument in this minimal preview doc. */
            previewSampleCount = 0;
            if (BAERmfEditorDocument_GetSampleCount(previewDocument, &previewSampleCount) != BAE_NO_ERROR) {
                BAERmfEditorDocument_Delete(previewDocument);
                return nullptr;
            }
            if (resolvedInstID != 0) {
                for (uint32_t sampleDeleteIndex = previewSampleCount; sampleDeleteIndex > 0; --sampleDeleteIndex) {
                    uint32_t sampleInstID;

                    sampleInstID = 0;
                    if (BAERmfEditorDocument_GetInstIDForSample(previewDocument,
                                                                sampleDeleteIndex - 1,
                                                                &sampleInstID) != BAE_NO_ERROR) {
                        continue;
                    }
                    if (sampleInstID != 0 && sampleInstID != resolvedInstID) {
                        BAERmfEditorDocument_DeleteSample(previewDocument, sampleDeleteIndex - 1);
                    }
                }
            }

            mappedSampleIndex = 0;
            if (BAERmfEditorDocument_GetSampleInfo(m_document,
                                                   sourceSampleIndex,
                                                   &sourceSampleInfo) == BAE_NO_ERROR) {
                BAERmfEditorSampleInfo candidateInfo;

                previewSampleCount = 0;
                BAERmfEditorDocument_GetSampleCount(previewDocument, &previewSampleCount);
                for (uint32_t candidateIndex = 0; candidateIndex < previewSampleCount; ++candidateIndex) {
                    if (BAERmfEditorDocument_GetSampleInfo(previewDocument,
                                                           candidateIndex,
                                                           &candidateInfo) != BAE_NO_ERROR) {
                        continue;
                    }
                    if (candidateInfo.program == sourceSampleInfo.program &&
                        candidateInfo.rootKey == sourceSampleInfo.rootKey &&
                        candidateInfo.lowKey == sourceSampleInfo.lowKey &&
                        candidateInfo.highKey == sourceSampleInfo.highKey) {
                        mappedSampleIndex = candidateIndex;
                        break;
                    }
                }
            }
            if (outPreviewSampleIndex) {
                *outPreviewSampleIndex = mappedSampleIndex;
            }

            /* Build a ~10-minute silent preload track so the engine
             * keeps the song alive for real-time NoteOn injection.
             * At 120 BPM / 480 tpq: 1 tick ≈ 1.042 ms.
             * 10 min ≈ 576000 ticks, 10 s ≈ 9600 ticks. */
            constexpr uint32_t kPreviewSongLengthTicks = 576000;
            constexpr uint32_t kPreviewLoopStartTick   = 9600;

            XSetMemory(&preloadTrackSetup, sizeof(preloadTrackSetup), 0);
            preloadTrackSetup.channel = (noteId > 0) ? static_cast<unsigned char>(9) : static_cast<unsigned char>(15);
            preloadTrackSetup.bank = InternalBankFromDisplay(static_cast<uint16_t>(resolvedBank));
            preloadTrackSetup.program = resolvedProgram;
            preloadTrackSetup.name = const_cast<char *>("PreviewPreload");
            if (BAERmfEditorDocument_AddTrack(previewDocument,
                                              &preloadTrackSetup,
                                              &preloadTrackIndex) == BAE_NO_ERROR) {
                (void)BAERmfEditorDocument_AddTrackCCEvent(previewDocument, preloadTrackIndex, 7, 0, 0);
                (void)BAERmfEditorDocument_AddTrackCCEvent(previewDocument, preloadTrackIndex, 11, 0, 0);
                /* Silent note at tick 0 to force instrument load */
                (void)BAERmfEditorDocument_AddNote(previewDocument,
                                                   preloadTrackIndex,
                                                   0,
                                                   1,
                                                   preloadNote,
                                                   1);
                /* Pad to 10 minutes with a second silent note at the end */
                (void)BAERmfEditorDocument_AddNote(previewDocument,
                                                   preloadTrackIndex,
                                                   kPreviewSongLengthTicks - 1,
                                                   1,
                                                   preloadNote,
                                                   1);
            }
            /* Loop from 10 s to EOF so the song never stops */
            (void)BAERmfEditorDocument_SetMidiLoopMarkers(previewDocument,
                                                          TRUE,
                                                          kPreviewLoopStartTick,
                                                          kPreviewSongLengthTicks,
                                                          99);

            return previewDocument;
        };

        if (!EnsurePlaybackEngine()) {
            return false;
        }

        hasDialogOverrides = (extOverride != nullptr) ||
                             (overrideInfo != nullptr) ||
                             (rootKeyOverride <= 127) ||
                             (compressionOverride != BAE_EDITOR_COMPRESSION_DONT_CHANGE) ||
                             (splitVolumeOverride != 0) ||
                             opusRoundTripOverride;

        previewProgram = sampleInfo.program;
        previewBank = 0;
        instID = 0;
        selectedInstID = 0;
        bankId = 0;
        progId = 0;
        noteId = 0;
        hasSelectedInstID = false;
        instrumentID = 0;
        translatedInstrumentID = 0;
        if (resolvePreviewInstrumentID(m_document, sampleIndex, sampleInfo.program, &instID)) {
            hasSelectedInstID = true;
            selectedInstID = instID;
            if (TranslateInstrumentToBankProgram(instID, &bankId, &progId, &noteId) == BAE_NO_ERROR) {
                previewProgram = static_cast<unsigned char>(std::clamp<uint32_t>(progId, 0, 127));
                previewBank = static_cast<unsigned char>(std::clamp<uint32_t>(bankId, 0, 127));
            }
        }

        channel = 0;

        /* Only tear down and rebuild the preview song when we don't already
         * have one.  For rapid key presses within the same dialog session the
         * instruments haven't changed, so reusing the existing song avoids
         * the expensive serialise→load→preroll cycle that causes intermittent
         * LoadInstrument 10002 failures under fast input.
         * The song is set to null by StopKeyboardPreview (called by the
         * dialog when all keys are released or the dialog closes), so the
         * next press after an edit will always get a fresh build. */
        armPreview = (m_keyboardPreviewSong == nullptr);
        if (armPreview) {
            StopKeyboardPreview();
        }

        if (previewTag != -1) {
            auto existing = m_taggedInstrumentPreviewNotes.find(previewTag);
#if _DEBUG
            fprintf(stderr, "[nbstudio] preview-tag=%d (char=%c) existing=%s\n",
                    previewTag, (char)previewTag, existing != m_taggedInstrumentPreviewNotes.end() ? "yes" : "no");
#endif
            if (existing != m_taggedInstrumentPreviewNotes.end()) {
                channel = existing->second.channel;
            } else {
                channel = mapPreviewTagToChannel(previewTag);
#if _DEBUG
                fprintf(stderr, "[nbstudio]   -> mapped to channel=%u\n", channel);
#endif
            }
        }

        if (armPreview) {
            /* For keyboard preview, use isolated minimal song to avoid timeline interference.
             * Ensure keyboard preview song and stop any existing playback. */
            if (hasDialogOverrides) {
                BAERmfEditorDocument *previewDoc;
                BAELoadResult loadInfo;
                BAEResult loadResult;
                uint32_t previewSampleIndex;

                previewSampleIndex = 0;
                previewDoc = buildMinimalInstrumentPreviewDocument(sampleIndex,
                                                                   selectedInstID,
                                                                   previewProgram,
                                                                   previewBank,
                                                                   static_cast<unsigned char>(std::clamp((noteId > 0 && noteId <= 127U) ? static_cast<int>(noteId) : midiKey, 0, 127)),
                                                                   &previewSampleIndex);
                if (!previewDoc) {
                    return false;
                }

                if (overrideInfo || rootKeyOverride <= 127 ||
                    compressionOverride != BAE_EDITOR_COMPRESSION_DONT_CHANGE ||
                    splitVolumeOverride != 0 || opusRoundTripOverride) {
                    BAERmfEditorSampleInfo previewSampleInfo;

                    if (BAERmfEditorDocument_GetSampleInfo(previewDoc, previewSampleIndex, &previewSampleInfo) != BAE_NO_ERROR) {
                        BAERmfEditorDocument_Delete(previewDoc);
                        return false;
                    }
                    if (overrideInfo) {
                        previewSampleInfo.sampleInfo = *overrideInfo;
                    }
                    if (splitVolumeOverride != 0) {
                        previewSampleInfo.splitVolume = splitVolumeOverride;
                    }
                    if (rootKeyOverride <= 127) {
                        previewSampleInfo.rootKey = rootKeyOverride;
                    }
                    if (compressionOverride != BAE_EDITOR_COMPRESSION_DONT_CHANGE) {
                        previewSampleInfo.compressionType = compressionOverride;
                    }
                    if (opusRoundTripOverride) {
                        previewSampleInfo.opusRoundTripResample = TRUE;
                    }
                    if (BAERmfEditorDocument_SetSampleInfo(previewDoc, previewSampleIndex, &previewSampleInfo) != BAE_NO_ERROR) {
                        BAERmfEditorDocument_Delete(previewDoc);
                        return false;
                    }
                }

                if (extOverride && hasSelectedInstID) {
                    BAERmfEditorInstrumentExtInfo previewExt;

                    previewExt = *extOverride;
                    previewExt.instID = selectedInstID;
                    if (BAERmfEditorDocument_SetInstrumentExtInfo(previewDoc,
                                                                  selectedInstID,
                                                                  &previewExt) != BAE_NO_ERROR) {
                        BAERmfEditorDocument_Delete(previewDoc);
                        return false;
                    }
                }

                m_keyboardPreviewSongBlob.clear();
                if (!BuildPreviewPlaybackBlob(previewDoc, &m_keyboardPreviewSongBlob)) {
                    BAERmfEditorDocument_Delete(previewDoc);
                    return false;
                }
                BAERmfEditorDocument_Delete(previewDoc);

                memset(&loadInfo, 0, sizeof(loadInfo));
                loadResult = BAEMixer_LoadFromMemory(m_playbackMixer,
                                                     m_keyboardPreviewSongBlob.data(),
                                                     static_cast<uint32_t>(m_keyboardPreviewSongBlob.size()),
                                                     &loadInfo);
                if (loadResult != BAE_NO_ERROR || loadInfo.type != BAE_LOAD_TYPE_SONG || !loadInfo.data.song) {
                    BAELoadResult_Cleanup(&loadInfo);
                    m_keyboardPreviewSongBlob.clear();
                    return false;
                }
                m_keyboardPreviewSong = loadInfo.data.song;
                loadInfo.type = BAE_LOAD_TYPE_NONE;
                loadInfo.data.song = nullptr;
                BAESong_Preroll(m_keyboardPreviewSong);
                BAESong_SetVolume(m_keyboardPreviewSong, GetPreviewVolumeFixed());
                BAESong_SetLoops(m_keyboardPreviewSong, 32767);
                if (BAESong_Start(m_keyboardPreviewSong, 0) != BAE_NO_ERROR) {
                    BAESong_Delete(m_keyboardPreviewSong);
                    m_keyboardPreviewSong = nullptr;
                    m_keyboardPreviewSongBlob.clear();
                    return false;
                }
                /* Seek past the preload note so real-time events
                 * land in the looped region (~10 s in). */
                BAESong_SetMicrosecondPosition(m_keyboardPreviewSong, 10000000);
            } else if (!EnsureKeyboardPreviewSong()) {
                return false;
            }
            BAESong_AllNotesOff(m_keyboardPreviewSong, 0);
        } else if (!m_keyboardPreviewSong && !EnsureKeyboardPreviewSong()) {
            return false;
        }

        /* NoteOnWithLoad reads current channel program/bank immediately,
         * so program/bank changes must also be immediate. */
        previewEventTime = 0;
        previewNoteOnTime = 0;

        BAESong_UnmuteChannel(m_keyboardPreviewSong, channel);
        BAESong_ControlChange(m_keyboardPreviewSong, channel, 7, 127, previewEventTime);
        BAESong_ControlChange(m_keyboardPreviewSong, channel, 11, 127, previewEventTime);
        BAESong_ControlChange(m_keyboardPreviewSong, channel, 10, 64, previewEventTime);
        BAESong_ControlChange(m_keyboardPreviewSong, channel, 121, 0, previewEventTime);
        BAESong_ControlChange(m_keyboardPreviewSong, channel, 127, 0, previewEventTime);
        BAESong_ControlChange(m_keyboardPreviewSong, channel, 64, 0, previewEventTime);
        BAESong_ControlChange(m_keyboardPreviewSong, channel, 66, 0, previewEventTime);
        if (channel == 9 && noteId > 0 && noteId <= 127U) {
            midiKey = static_cast<int>(noteId);
        }
        result = BAESong_ProgramBankChange(m_keyboardPreviewSong,
                                           channel,
                                           previewProgram,
                                           previewBank,
                                           previewEventTime);
        if (result != BAE_NO_ERROR) {
            return false;
        }

        /* The non-dialog path reuses a cached song, so a fresh
         * Preroll/Start/Seek cycle is needed to pick up any program
         * changes.  The dialog path keeps the song alive via a long
         * looped MIDI track — no restart needed. */
        if (!hasDialogOverrides) {
            BAESong_Preroll(m_keyboardPreviewSong);
            BAESong_Start(m_keyboardPreviewSong, 0);
            SeekKeyboardPreviewToSongEnd();
        }

        translatedInstrumentID = TranslateBankProgramToInstrument(previewBank,
                                                                  previewProgram,
                                                                  channel,
                                                                  static_cast<unsigned char>(std::clamp(midiKey, 0, 127)));
        instrumentID = hasSelectedInstID ? static_cast<BAE_INSTRUMENT>(selectedInstID) : translatedInstrumentID;
#if _DEBUG
        fprintf(stderr,
            "[nbstudio] instrument-lookup: ch=%u prog=%u bank=%u key=%d hasSelected=%d selected=%u translated=%u using=%u\n",
                channel,
                previewProgram,
                previewBank,
                midiKey,
            hasSelectedInstID ? 1 : 0,
                static_cast<unsigned>(selectedInstID),
                static_cast<unsigned>(translatedInstrumentID),
                static_cast<unsigned>(instrumentID));
#endif
        loadInstrumentResult = BAESong_LoadInstrument(m_keyboardPreviewSong, instrumentID);
        if (loadInstrumentResult != BAE_NO_ERROR && translatedInstrumentID != instrumentID) {
            instrumentID = translatedInstrumentID;
            loadInstrumentResult = BAESong_LoadInstrument(m_keyboardPreviewSong, instrumentID);
        }
        if (loadInstrumentResult != BAE_NO_ERROR) {
#if _DEBUG
            fprintf(stderr,
                "[nbstudio] BAESong_LoadInstrument failed: instID=%u translated=%u err=%d\n",
                static_cast<unsigned>(instrumentID),
                static_cast<unsigned>(translatedInstrumentID),
                static_cast<int>(loadInstrumentResult));
            return false;
#endif
        } 
        result = BAESong_NoteOn(m_keyboardPreviewSong,
                                    channel,
                                    static_cast<unsigned char>(std::clamp(midiKey, 0, 127)),
                                    100,
                                    previewNoteOnTime);
#if _DEBUG
        fprintf(stderr,
            "[nbstudio] note-on: ch=%u key=%u vel=100 t_prog=%u t_on=%u result=%d\n",
            channel,
            std::clamp(midiKey, 0, 127),
            previewEventTime,
            previewNoteOnTime,
            result);
#endif
        if (result != BAE_NO_ERROR) {
            return false;
        }
        if (previewTag != -1) {
            TaggedInstrumentPreviewNote tagged;

            tagged.channel = channel;
            tagged.note = static_cast<unsigned char>(std::clamp(midiKey, 0, 127));
            tagged.program = previewProgram;
            tagged.bank = previewBank;
            m_taggedInstrumentPreviewNotes[previewTag] = tagged;
#if _DEBUG
            fprintf(stderr,
                    "[nbstudio] note-preview on tag=%d ch=%u note=%u prog=%u bank=%u active=%zu\n",
                    previewTag,
                    static_cast<unsigned>(tagged.channel),
                    static_cast<unsigned>(tagged.note),
                    static_cast<unsigned>(tagged.program),
                    static_cast<unsigned>(tagged.bank),
                    m_taggedInstrumentPreviewNotes.size());
#endif
        }
        return true;
    }

    bool PreviewSampleAtKey(uint32_t sampleIndex,
                            int midiKey,
                            BAESampleInfo const *overrideInfo = nullptr,
                            int16_t splitVolumeOverride = 0,
                            unsigned char rootKeyOverride = 0xFF,
                            BAERmfEditorCompressionType compressionOverride = BAE_EDITOR_COMPRESSION_DONT_CHANGE,
                            bool opusRoundTripOverride = false,
                            int previewTag = -1,
                            BAERmfEditorInstrumentExtInfo const *extOverride = nullptr) {
        if (extOverride) {
            return PreviewInstrumentDialogNote(sampleIndex,
                                               midiKey,
                                               previewTag,
                                               overrideInfo,
                                               splitVolumeOverride,
                                               rootKeyOverride,
                                               compressionOverride,
                                               opusRoundTripOverride,
                                               extOverride);
        }

        BAERmfEditorSampleInfo sampleInfo;
        BAEResult loadResult;
        BAEResult startResult;
        BAEResult rateResult;
        BAE_UNSIGNED_FIXED baseSampleRate;
        BAE_UNSIGNED_FIXED intendedSampleRate;
        int previewRoot;
        int semitones;
        int loadRateOctaveShift;
        bool preserveIntendedSampleRate;
        bool sourceIsOpus;
        bool sourceIsOpusRoundTrip;
        bool effectiveIsOpusRoundTrip;
        bool retimeDecodedPreviewToIntendedRate;
        bool previewNeedsFrameRateComp;
        BAEFileType previewCompressedFileType;
        std::function<BAEResult()> trimDecodedPreviewToSourceWindow;
        uint32_t previewSourceFramesForRate;
        uint32_t previewDecodedFramesForRate;
        constexpr double kMinRateFixed = static_cast<double>(4000U << 16);
        constexpr double kMaxRateFixed = static_cast<double>(0xFFFF0000u);

        if (!m_document) {
            return false;
        }
        if (BAERmfEditorDocument_GetSampleInfo(m_document, sampleIndex, &sampleInfo) != BAE_NO_ERROR) {
            return false;
        }

        previewRoot = static_cast<int>(NormalizeRootKeyForSingleKeySplit(sampleInfo.rootKey,
                                                                          sampleInfo.lowKey,
                                                                          sampleInfo.highKey));
        if (rootKeyOverride <= 127) {
            previewRoot = static_cast<int>(rootKeyOverride);
        }
        if (previewRoot < 0 || previewRoot > 127) {
            previewRoot = static_cast<int>((overrideInfo ? overrideInfo->baseMidiPitch : sampleInfo.sampleInfo.baseMidiPitch) & 0x7F);
        }
        semitones = midiKey - previewRoot;
        loadRateOctaveShift = 0;

        intendedSampleRate = sampleInfo.sampleInfo.sampledRate;
        if (overrideInfo && overrideInfo->sampledRate > 0) {
            intendedSampleRate = overrideInfo->sampledRate;
        }
        baseSampleRate = intendedSampleRate;
        sourceIsOpus = false;
        sourceIsOpusRoundTrip = (sampleInfo.opusRoundTripResample == TRUE);
        effectiveIsOpusRoundTrip = sourceIsOpusRoundTrip || opusRoundTripOverride;
        {
            char codecName[64] = {};
            if (BAERmfEditorDocument_GetSampleCodecDescription(m_document,
                                                               sampleIndex,
                                                               codecName,
                                                               sizeof(codecName)) == BAE_NO_ERROR) {
                sourceIsOpus = wxString::FromUTF8(codecName).Lower().Contains("opus");
            }
        }
        preserveIntendedSampleRate = IsOpusCompressionType(compressionOverride) ||
                                     effectiveIsOpusRoundTrip;
        retimeDecodedPreviewToIntendedRate = (preserveIntendedSampleRate && !effectiveIsOpusRoundTrip) ||
                                             (sourceIsOpus &&
                                              compressionOverride != BAE_EDITOR_COMPRESSION_DONT_CHANGE &&
                                              compressionOverride != BAE_EDITOR_COMPRESSION_PCM &&
                                              !IsOpusCompressionType(compressionOverride));
        previewNeedsFrameRateComp = (sourceIsOpus || IsOpusCompressionType(compressionOverride)) &&
                                    !effectiveIsOpusRoundTrip;
        previewCompressedFileType = BAE_INVALID_TYPE;
        previewSourceFramesForRate = 0;
        previewDecodedFramesForRate = 0;
        if (!EnsurePlaybackEngine()) {
            return false;
        }

        BAESound previousPreviewSound = m_previewSound;
        BAESound *previewSoundSlot = &m_previewSound;
        if (previewTag != -1) {
            previewSoundSlot = &m_taggedPreviewSounds[previewTag];
            m_previewSound = *previewSoundSlot;
        }
        struct PreviewSoundScope final {
            BAESound &activePreviewSound;
            BAESound *slot;
            BAESound originalPreviewSound;
            bool shouldRestore;

            ~PreviewSoundScope() {
                if (!shouldRestore) {
                    return;
                }
                *slot = activePreviewSound;
                activePreviewSound = originalPreviewSound;
            }
        } previewSoundScope{m_previewSound, previewSoundSlot, previousPreviewSound, previewTag != -1};

        if (m_previewSound) {
            BAESound_Stop(m_previewSound, FALSE);
            BAESound_Delete(m_previewSound);
            m_previewSound = nullptr;
        }
        m_previewSound = BAESound_New(m_playbackMixer);
        if (!m_previewSound) {
            return false;
        }

        loadResult = BAE_GENERAL_ERR;

        /* BAESound_LoadCustomSample expects signed 8-bit PCM input; it internally
           applies XPhase (subtract 128) to convert to the engine's unsigned format.
           However, waveform->theWaveform already holds unsigned 8-bit data (engine
           internal format, 0x80 = silence) from the RMF SND resource.  Passing it
           directly causes a double XPhase that shifts every sample by 128, producing
           severe DC-offset distortion ("hot/squelchy" audio).
           Pre-apply the same subtraction so the two applications cancel out. */
        auto loadCustomSampleFromEngineData = [&](void const *data, uint32_t frames,
                                                  uint16_t bits, uint16_t chans,
                                                  BAE_UNSIGNED_FIXED rate,
                                                  uint32_t loopS, uint32_t loopE) -> BAEResult {
            if (bits == 8) {
                /* Modify in-place: XPhase (subtract 128) converts unsigned engine format
                   to the signed format LoadCustomSample expects.  LoadCustomSample applies
                   XPhase again internally, and two subtractions of 128 mod 256 = identity,
                   so the document waveform is restored after the call.  No heap allocation
                   needed.  Safe because BAESound_Stop above released the audio thread's
                   access to this buffer. */
                uint8_t *data8 = const_cast<uint8_t *>(static_cast<uint8_t const *>(data));
                uint32_t dataSize = frames * static_cast<uint32_t>(chans);
                for (uint32_t i = 0; i < dataSize; ++i) data8[i] -= 128u;
                BAEResult r = BAESound_LoadCustomSample(m_previewSound, data8, frames, bits, chans, rate, loopS, loopE);
                for (uint32_t i = 0; i < dataSize; ++i) data8[i] -= 128u;  /* restore */
                return r;
            }
            return BAESound_LoadCustomSample(m_previewSound, const_cast<void *>(data), frames, bits, chans, rate, loopS, loopE);
        };

        auto normalizePreviewRate = [](BAE_UNSIGNED_FIXED rate) -> BAE_UNSIGNED_FIXED {
            if (rate == 0) {
                return (44100U << 16);
            }
            if (rate >= 4000U && rate <= 384000U) {
                return (rate << 16);
            }
            return rate;
        };

        auto scanPreviewOnsetFrames = [](void const *data,
                                         uint32_t frames,
                                         uint16_t bits,
                                         uint16_t channels) -> uint32_t {
            if (!data || frames == 0 || (bits != 8 && bits != 16) || (channels != 1 && channels != 2)) {
                return 0;
            }

            if (bits == 16) {
                int16_t const *samples = static_cast<int16_t const *>(data);

                for (uint32_t frameIndex = 0; frameIndex < frames; ++frameIndex) {
                    for (uint16_t channelIndex = 0; channelIndex < channels; ++channelIndex) {
                        int32_t sampleValue = samples[frameIndex * channels + channelIndex];

                        if (sampleValue > 256 || sampleValue < -256) {
                            return frameIndex;
                        }
                    }
                }
            } else {
                uint8_t const *samples = static_cast<uint8_t const *>(data);

                for (uint32_t frameIndex = 0; frameIndex < frames; ++frameIndex) {
                    for (uint16_t channelIndex = 0; channelIndex < channels; ++channelIndex) {
                        int32_t sampleValue = static_cast<int32_t>(samples[frameIndex * channels + channelIndex]) - 128;

                        if (sampleValue > 2 || sampleValue < -2) {
                            return frameIndex;
                        }
                    }
                }
            }

            return frames;
        };

        baseSampleRate = normalizePreviewRate(baseSampleRate);

        auto adaptLoadRateForPitch = [&](BAE_UNSIGNED_FIXED &ioRate,
                                         int &outOctaveShift) {
            double desiredRate;
            int maxSemitones;
            int octaveShift;
            BAE_UNSIGNED_FIXED shiftedRate;

            if (ioRate == 0) {
                outOctaveShift = 0;
                return;
            }

            maxSemitones = std::max(0, 127 - previewRoot);
            desiredRate = static_cast<double>(ioRate) * std::pow(2.0, static_cast<double>(maxSemitones) / 12.0);
            shiftedRate = ioRate;
            octaveShift = 0;

            while (desiredRate > kMaxRateFixed && shiftedRate > (8000U << 16)) {
                shiftedRate >>= 1;
                desiredRate *= 0.5;
                ++octaveShift;
            }
            if (shiftedRate < (4000U << 16)) {
                shiftedRate = (4000U << 16);
            }

            ioRate = shiftedRate;
            outOctaveShift = octaveShift;
        };

        auto rebuildPreviewAtIntendedRate = [&](BAE_UNSIGNED_FIXED targetRate) -> BAEResult {
            BAESampleInfo decodedInfo;
            uint32_t sourceFrames;
            uint32_t targetFrames;
            uint16_t bitSize;
            uint16_t channels;
            uint32_t bytesPerSample;
            uint32_t bytesPerFrame;
            std::vector<unsigned char> sourceData;
            std::vector<unsigned char> resampledData;
            BAEResult infoResult;
            BAEResult dataResult;
            BAEResult loadCustomResult;

            targetRate = normalizePreviewRate(targetRate);
            if (targetRate == 0) {
                return BAE_PARAM_ERR;
            }

            memset(&decodedInfo, 0, sizeof(decodedInfo));
            infoResult = BAESound_GetInfo(m_previewSound, &decodedInfo);
            if (infoResult != BAE_NO_ERROR) {
                return infoResult;
            }

            decodedInfo.sampledRate = normalizePreviewRate(decodedInfo.sampledRate);
            if (decodedInfo.sampledRate == 0 || decodedInfo.sampledRate == targetRate) {
                return BAE_NO_ERROR;
            }

            sourceFrames = decodedInfo.waveFrames;
            bitSize = decodedInfo.bitSize;
            channels = decodedInfo.channels;
            if ((bitSize != 8 && bitSize != 16) || (channels != 1 && channels != 2)) {
                return BAE_UNSUPPORTED_FORMAT;
            }

            bytesPerSample = static_cast<uint32_t>(bitSize / 8);
            bytesPerFrame = bytesPerSample * static_cast<uint32_t>(channels);
            if (sourceFrames == 0 && bytesPerFrame > 0) {
                sourceFrames = decodedInfo.waveSize / bytesPerFrame;
            }
            if (sourceFrames == 0 || bytesPerFrame == 0) {
                return BAE_BAD_FILE;
            }

            targetFrames = static_cast<uint32_t>((((uint64_t)sourceFrames * (uint64_t)targetRate) +
                                                  ((uint64_t)decodedInfo.sampledRate / 2ULL)) /
                                                 (uint64_t)decodedInfo.sampledRate);
            if (targetFrames == 0) {
                targetFrames = 1;
            }

            sourceData.resize(static_cast<size_t>(sourceFrames) * bytesPerFrame);
            dataResult = BAESound_GetRawPCMData(m_previewSound,
                                                reinterpret_cast<char *>(sourceData.data()),
                                                static_cast<uint32_t>(sourceData.size()));
            if (dataResult != BAE_NO_ERROR) {
                return dataResult;
            }

            resampledData.resize(static_cast<size_t>(targetFrames) * bytesPerFrame);
            if (bitSize == 16) {
                int16_t const *src = reinterpret_cast<int16_t const *>(sourceData.data());
                int16_t *dst = reinterpret_cast<int16_t *>(resampledData.data());

                for (uint32_t frameIndex = 0; frameIndex < targetFrames; ++frameIndex) {
                    double srcPos;
                    uint32_t srcIndex;
                    uint32_t nextIndex;
                    double frac;

                    if (targetFrames <= 1 || sourceFrames <= 1) {
                        srcPos = 0.0;
                    } else {
                        srcPos = (static_cast<double>(frameIndex) * static_cast<double>(sourceFrames - 1)) /
                                 static_cast<double>(targetFrames - 1);
                    }
                    srcIndex = static_cast<uint32_t>(srcPos);
                    nextIndex = std::min(srcIndex + 1, sourceFrames - 1);
                    frac = srcPos - static_cast<double>(srcIndex);

                    for (uint16_t channelIndex = 0; channelIndex < channels; ++channelIndex) {
                        int32_t left = src[srcIndex * channels + channelIndex];
                        int32_t right = src[nextIndex * channels + channelIndex];
                        double value = static_cast<double>(left) +
                                       (static_cast<double>(right - left) * frac);
                        dst[frameIndex * channels + channelIndex] = static_cast<int16_t>(std::lround(value));
                    }
                }
            } else {
                unsigned char const *src = sourceData.data();
                unsigned char *dst = resampledData.data();

                for (uint32_t frameIndex = 0; frameIndex < targetFrames; ++frameIndex) {
                    double srcPos;
                    uint32_t srcIndex;
                    uint32_t nextIndex;
                    double frac;

                    if (targetFrames <= 1 || sourceFrames <= 1) {
                        srcPos = 0.0;
                    } else {
                        srcPos = (static_cast<double>(frameIndex) * static_cast<double>(sourceFrames - 1)) /
                                 static_cast<double>(targetFrames - 1);
                    }
                    srcIndex = static_cast<uint32_t>(srcPos);
                    nextIndex = std::min(srcIndex + 1, sourceFrames - 1);
                    frac = srcPos - static_cast<double>(srcIndex);

                    for (uint16_t channelIndex = 0; channelIndex < channels; ++channelIndex) {
                        uint32_t left = src[srcIndex * channels + channelIndex];
                        uint32_t right = src[nextIndex * channels + channelIndex];
                        double value = static_cast<double>(left) +
                                       (static_cast<double>(static_cast<int32_t>(right) - static_cast<int32_t>(left)) * frac);
                        dst[frameIndex * channels + channelIndex] = static_cast<unsigned char>(std::clamp(std::lround(value), 0l, 255l));
                    }
                }

                for (unsigned char &sampleByte : resampledData) {
                    sampleByte = static_cast<unsigned char>(sampleByte - 128u);
                }
            }

            loadCustomResult = BAESound_LoadCustomSample(m_previewSound,
                                                         resampledData.data(),
                                                         targetFrames,
                                                         bitSize,
                                                         channels,
                                                         targetRate,
                                                         0,
                                                         0);
            return loadCustomResult;
        };

        trimDecodedPreviewToSourceWindow = [&]() -> BAEResult {
            BAESampleInfo decodedInfo;
            void const *sourceWaveData;
            uint32_t sourceFrames;
            uint16_t sourceBits;
            uint16_t sourceChannels;
            BAE_UNSIGNED_FIXED sourceRate;
            uint32_t decodedFrames;
            uint32_t sourceOnset;
            uint32_t decodedOnset;
            uint32_t extraLeadFrames;
            uint32_t trimStartFrames;
            uint32_t trimmedFrames;
            uint32_t bytesPerFrame;
            std::vector<unsigned char> decodedData;
            constexpr uint32_t kMpegPreviewLeadTrimFrames = 576U + 529U;

            sourceWaveData = nullptr;
            sourceFrames = (overrideInfo && overrideInfo->waveFrames > 0)
                ? overrideInfo->waveFrames
                : sampleInfo.sampleInfo.waveFrames;
            sourceBits = 16;
            sourceChannels = 1;
            sourceRate = 0;

            if (BAERmfEditorDocument_GetSampleWaveformData(m_document,
                                                           sampleIndex,
                                                           &sourceWaveData,
                                                           &sourceFrames,
                                                           &sourceBits,
                                                           &sourceChannels,
                                                           &sourceRate) != BAE_NO_ERROR) {
                sourceWaveData = nullptr;
            }

            memset(&decodedInfo, 0, sizeof(decodedInfo));
            if (BAESound_GetInfo(m_previewSound, &decodedInfo) != BAE_NO_ERROR) {
                return BAE_GENERAL_ERR;
            }
            if ((decodedInfo.bitSize != 8 && decodedInfo.bitSize != 16) ||
                (decodedInfo.channels != 1 && decodedInfo.channels != 2)) {
                return BAE_UNSUPPORTED_FORMAT;
            }

            bytesPerFrame = (static_cast<uint32_t>(decodedInfo.bitSize) / 8U) * static_cast<uint32_t>(decodedInfo.channels);
            if (bytesPerFrame == 0) {
                return BAE_BAD_FILE;
            }

            decodedFrames = decodedInfo.waveFrames;
            if (decodedFrames == 0) {
                decodedFrames = decodedInfo.waveSize / bytesPerFrame;
            }
            if (decodedFrames == 0) {
                return BAE_BAD_FILE;
            }

            decodedData.resize(static_cast<size_t>(decodedFrames) * bytesPerFrame);
            if (BAESound_GetRawPCMData(m_previewSound,
                                       reinterpret_cast<char *>(decodedData.data()),
                                       static_cast<uint32_t>(decodedData.size())) != BAE_NO_ERROR) {
                return BAE_GENERAL_ERR;
            }

            sourceOnset = 0;
            if (sourceWaveData && sourceFrames > 0 &&
                (sourceBits == 8 || sourceBits == 16) &&
                (sourceChannels == 1 || sourceChannels == 2)) {
                sourceOnset = scanPreviewOnsetFrames(sourceWaveData,
                                                     sourceFrames,
                                                     sourceBits,
                                                     sourceChannels);
            }
            decodedOnset = scanPreviewOnsetFrames(decodedData.data(),
                                                  decodedFrames,
                                                  decodedInfo.bitSize,
                                                  decodedInfo.channels);
            extraLeadFrames = (decodedOnset > sourceOnset) ? (decodedOnset - sourceOnset) : 0;
            trimStartFrames = extraLeadFrames;
            if (previewCompressedFileType == BAE_MPEG_TYPE && trimStartFrames == 0) {
                trimStartFrames = kMpegPreviewLeadTrimFrames;
            }
            if (trimStartFrames > decodedFrames) {
                trimStartFrames = decodedFrames;
            }

            trimmedFrames = decodedFrames - trimStartFrames;
            if (sourceFrames > 0 && trimmedFrames > sourceFrames) {
                trimmedFrames = sourceFrames;
            }
            if (trimStartFrames == 0 && trimmedFrames == decodedFrames) {
                return BAE_NO_ERROR;
            }
            if (trimmedFrames == 0) {
                return BAE_BAD_FILE;
            }

            return BAESound_LoadCustomSample(m_previewSound,
                                             decodedData.data() + (static_cast<size_t>(trimStartFrames) * bytesPerFrame),
                                             trimmedFrames,
                                             decodedInfo.bitSize,
                                             decodedInfo.channels,
                                             normalizePreviewRate(decodedInfo.sampledRate),
                                             0,
                                             0);
        };

        if (overrideInfo &&
            compressionOverride != BAE_EDITOR_COMPRESSION_DONT_CHANGE &&
            compressionOverride != BAE_EDITOR_COMPRESSION_PCM) {
            wxString compressedPath;

            /* For MPEG preview, prefer the in-memory reloaded waveform path so
             * decoded loop geometry matches the post-encode sample state used by
             * instrument audition. */
            if (IsMpegCompressionType(compressionOverride)) {
                std::vector<unsigned char> reloadedWaveData;
                uint32_t reloadedFrameCount;
                uint16_t reloadedBitSize;
                uint16_t reloadedChannels;
                BAE_UNSIGNED_FIXED reloadedSampleRate;
                uint32_t reloadedLoopStart;
                uint32_t reloadedLoopEnd;

                reloadedFrameCount = 0;
                reloadedBitSize = 16;
                reloadedChannels = 1;
                reloadedSampleRate = 0;
                reloadedLoopStart = 0;
                reloadedLoopEnd = 0;
                if (BuildCompressedPreviewReloadedWaveform(sampleIndex,
                                                           compressionOverride,
                                                           &reloadedWaveData,
                                                           &reloadedFrameCount,
                                                           &reloadedBitSize,
                                                           &reloadedChannels,
                                                           &reloadedSampleRate,
                                                           &reloadedLoopStart,
                                                           &reloadedLoopEnd)) {
                    previewCompressedFileType = BAE_MPEG_TYPE;
                    if (overrideInfo->sampledRate > 0 && !retimeDecodedPreviewToIntendedRate) {
                        reloadedSampleRate = overrideInfo->sampledRate;
                    }
                    reloadedSampleRate = normalizePreviewRate(reloadedSampleRate);
                    adaptLoadRateForPitch(reloadedSampleRate, loadRateOctaveShift);
                    loadResult = loadCustomSampleFromEngineData(reloadedWaveData.data(),
                                                                reloadedFrameCount,
                                                                reloadedBitSize,
                                                                reloadedChannels,
                                                                reloadedSampleRate,
                                                                reloadedLoopStart,
                                                                reloadedLoopEnd);
                }
            }

            if (loadResult != BAE_NO_ERROR && BuildCompressedPreviewSampleFile(sampleIndex, compressionOverride, &compressedPath)) {
                wxScopedCharBuffer utf8CompressedPath = compressedPath.utf8_str();
                BAEFileType compressedType = X_DetermineFileTypeByPath(utf8CompressedPath.data());
                if (compressedType == BAE_INVALID_TYPE) {
                    compressedType = X_DetermineFileType(utf8CompressedPath.data());
                }
                if (compressedType != BAE_INVALID_TYPE) {
                    previewCompressedFileType = compressedType;
                    loadResult = BAESound_LoadFileSample(m_previewSound,
                                                         const_cast<char *>(utf8CompressedPath.data()),
                                                         compressedType);
                    if (loadResult == BAE_NO_ERROR && previewCompressedFileType == BAE_MPEG_TYPE) {
                        BAEResult trimResult = trimDecodedPreviewToSourceWindow();

                        if (trimResult != BAE_NO_ERROR) {
                            fprintf(stderr,
                                    "[nbstudio] Preview sample %u MP3 trim-to-source failed result=%d\n",
                                    static_cast<unsigned>(sampleIndex),
                                    static_cast<int>(trimResult));
                        }
                    }
                }
                if (wxFileExists(compressedPath)) {
                    wxRemoveFile(compressedPath);
                }
            }
        }

          /* When the caller provides live loop overrides and compressed preview is
              not requested, use raw PCM so loop settings from the UI are honoured. */
          if (loadResult != BAE_NO_ERROR && overrideInfo) {
            void const *waveData;
            uint32_t frameCount;
            uint16_t bitSize;
            uint16_t channels;
            BAE_UNSIGNED_FIXED sampleRate;
            uint32_t loopStart;
            uint32_t loopEnd;

            waveData = nullptr;
            frameCount = 0;
            bitSize = 16;
            channels = 1;
            sampleRate = 0;
            if (BAERmfEditorDocument_GetSampleWaveformData(m_document,
                                                           sampleIndex,
                                                           &waveData,
                                                           &frameCount,
                                                           &bitSize,
                                                           &channels,
                                                           &sampleRate) == BAE_NO_ERROR &&
                frameCount > 0 &&
                (bitSize == 8 || bitSize == 16) &&
                (channels == 1 || channels == 2)) {
                if (overrideInfo->sampledRate > 0 && !retimeDecodedPreviewToIntendedRate) {
                    sampleRate = overrideInfo->sampledRate;
                }
                sampleRate = normalizePreviewRate(sampleRate);
                loopStart = overrideInfo->startLoop;
                loopEnd   = overrideInfo->endLoop;
                if (loopStart >= frameCount || loopEnd > frameCount || loopEnd <= loopStart) {
                    loopStart = 0;
                    loopEnd   = 0;
                }
                adaptLoadRateForPitch(sampleRate, loadRateOctaveShift);
                loadResult = loadCustomSampleFromEngineData(waveData, frameCount,
                                                           bitSize, channels,
                                                           sampleRate,
                                                           loopStart, loopEnd);
                if (loadResult != BAE_NO_ERROR) {
                    loadResult = BAE_GENERAL_ERR; /* fall through to file paths */
                }
            }
        }

        if (loadResult == BAE_NO_ERROR && retimeDecodedPreviewToIntendedRate) {
            BAEResult resampleResult = rebuildPreviewAtIntendedRate(intendedSampleRate);
            if (resampleResult != BAE_NO_ERROR) {
                fprintf(stderr,
                        "[nbstudio] Preview sample %u resample-to-intended-rate failed result=%d intended=%lu\n",
                        static_cast<unsigned>(sampleIndex),
                        static_cast<int>(resampleResult),
                        static_cast<unsigned long>(normalizePreviewRate(intendedSampleRate)));
            }
        }

        if (loadResult == BAE_NO_ERROR) {
            goto preview_loaded;
        }
        loadResult = BAE_GENERAL_ERR;

        if (sampleInfo.sourcePath && sampleInfo.sourcePath[0]) {
            BAEFileType sourceType;

            sourceType = X_DetermineFileTypeByPath(sampleInfo.sourcePath);
            if (sourceType == BAE_WAVE_TYPE || sourceType == BAE_AIFF_TYPE || sourceType == BAE_AU_TYPE) {
                loadResult = BAESound_LoadFileSample(m_previewSound,
                                                     const_cast<char *>(sampleInfo.sourcePath),
                                                     sourceType);
            } else {
                fprintf(stderr,
                        "[nbstudio] Preview sample %u source='%s' unsupported type=%d\n",
                        static_cast<unsigned>(sampleIndex),
                        sampleInfo.sourcePath,
                        static_cast<int>(sourceType));
            }
        }

        if (EnsurePreviewSampleTempPath()) {
            wxScopedCharBuffer utf8Path;
            BAEFileType exportedType;

            utf8Path = m_previewSampleTempPath.utf8_str();
            if (loadResult != BAE_NO_ERROR && BAERmfEditorDocument_ExportSampleToFile(m_document,
                                                        sampleIndex,
                                                        const_cast<char *>(utf8Path.data())) == BAE_NO_ERROR) {
                exportedType = X_DetermineFileTypeByPath(utf8Path.data());
                if (exportedType == BAE_INVALID_TYPE) {
                    exportedType = X_DetermineFileType(utf8Path.data());
                }
                if (exportedType == BAE_INVALID_TYPE) {
                    exportedType = BAE_WAVE_TYPE;
                }
                loadResult = BAESound_LoadFileSample(m_previewSound,
                                                     const_cast<char *>(utf8Path.data()),
                                                     exportedType);
            }
        }

        if (loadResult != BAE_NO_ERROR) {
            void const *waveData;
            uint32_t frameCount;
            uint16_t bitSize;
            uint16_t channels;
            BAE_UNSIGNED_FIXED sampleRate;
            uint32_t loopStart;
            uint32_t loopEnd;

            waveData = nullptr;
            frameCount = 0;
            bitSize = 16;
            channels = 1;
            sampleRate = 0;
            if (BAERmfEditorDocument_GetSampleWaveformData(m_document,
                                                           sampleIndex,
                                                           &waveData,
                                                           &frameCount,
                                                           &bitSize,
                                                           &channels,
                                                           &sampleRate) != BAE_NO_ERROR) {
                fprintf(stderr,
                        "[nbstudio] Preview sample %u GetSampleWaveformData failed\n",
                        static_cast<unsigned>(sampleIndex));
                return false;
            }
            fprintf(stderr,
                    "[nbstudio] Preview sample %u raw format: frames=%u bits=%u channels=%u rate=0x%08lx loop=%u-%u root=%u basePitch=%u\n",
                    static_cast<unsigned>(sampleIndex),
                    static_cast<unsigned>(frameCount),
                    static_cast<unsigned>(bitSize),
                    static_cast<unsigned>(channels),
                    static_cast<unsigned long>(sampleRate),
                    static_cast<unsigned>(sampleInfo.sampleInfo.startLoop),
                    static_cast<unsigned>(sampleInfo.sampleInfo.endLoop),
                    static_cast<unsigned>(sampleInfo.rootKey),
                    static_cast<unsigned>(sampleInfo.sampleInfo.baseMidiPitch));
            if (overrideInfo && overrideInfo->sampledRate > 0 && !retimeDecodedPreviewToIntendedRate) {
                sampleRate = overrideInfo->sampledRate;
            }
            sampleRate = normalizePreviewRate(sampleRate);
            if (frameCount == 0 || (bitSize != 8 && bitSize != 16) || (channels != 1 && channels != 2)) {
                return false;
            }
            loopStart = sampleInfo.sampleInfo.startLoop;
            loopEnd = sampleInfo.sampleInfo.endLoop;
            if (overrideInfo) {
                loopStart = overrideInfo->startLoop;
                loopEnd   = overrideInfo->endLoop;
            }
            if (loopStart >= frameCount || loopEnd > frameCount || loopEnd <= loopStart) {
                loopStart = 0;
                loopEnd = 0;
            }
            adaptLoadRateForPitch(sampleRate, loadRateOctaveShift);
            loadResult = loadCustomSampleFromEngineData(waveData, frameCount,
                                                        bitSize, channels,
                                                        sampleRate,
                                                        loopStart, loopEnd);
            if (loadResult != BAE_NO_ERROR) {
                fprintf(stderr,
                        "[nbstudio] Preview sample %u LoadCustomSample failed result=%d\n",
                        static_cast<unsigned>(sampleIndex),
                        static_cast<int>(loadResult));
                return false;
            }
            if (retimeDecodedPreviewToIntendedRate) {
                BAEResult resampleResult = rebuildPreviewAtIntendedRate(intendedSampleRate);
                if (resampleResult != BAE_NO_ERROR) {
                    fprintf(stderr,
                            "[nbstudio] Preview sample %u post-load resample-to-intended-rate failed result=%d intended=%lu\n",
                            static_cast<unsigned>(sampleIndex),
                            static_cast<int>(resampleResult),
                            static_cast<unsigned long>(normalizePreviewRate(intendedSampleRate)));
                }
            }
            fprintf(stderr,
                    "[nbstudio] Preview sample %u LoadCustomSample ok\n",
                    static_cast<unsigned>(sampleIndex));
        }

        preview_loaded:
        {
            BAESampleInfo loadedInfo;
            uint32_t loopStart;
            uint32_t loopEnd;
            uint32_t sourceFrames;
            uint32_t targetFrames;

            loopStart = overrideInfo ? overrideInfo->startLoop : sampleInfo.sampleInfo.startLoop;
            loopEnd = overrideInfo ? overrideInfo->endLoop : sampleInfo.sampleInfo.endLoop;
            sourceFrames = overrideInfo && overrideInfo->waveFrames > 0
                ? overrideInfo->waveFrames
                : sampleInfo.sampleInfo.waveFrames;
            targetFrames = 0;
            if (BAESound_GetInfo(m_previewSound, &loadedInfo) == BAE_NO_ERROR) {
                if (!preserveIntendedSampleRate && loadedInfo.sampledRate >= (4000U << 16)) {
                    baseSampleRate = loadedInfo.sampledRate;
                }
                targetFrames = loadedInfo.waveFrames;
                fprintf(stderr,
                        "[nbstudio] Preview sample %u loaded info: frames=%u bits=%u channels=%u rate=%lu basePitch=%u\n",
                        static_cast<unsigned>(sampleIndex),
                        static_cast<unsigned>(loadedInfo.waveFrames),
                        static_cast<unsigned>(loadedInfo.bitSize),
                        static_cast<unsigned>(loadedInfo.channels),
                        static_cast<unsigned long>(loadedInfo.sampledRate),
                        static_cast<unsigned>(loadedInfo.baseMidiPitch));
            }

            if (sourceFrames == 0) {
                sourceFrames = sampleInfo.sampleInfo.waveFrames;
            }
            if (targetFrames == 0) {
                targetFrames = sourceFrames;
            }
            previewSourceFramesForRate = sourceFrames;
            previewDecodedFramesForRate = targetFrames;
            if (loopEnd > loopStart && sourceFrames > 0 && targetFrames > 0) {
                if (sourceFrames != targetFrames) {
                    uint32_t mappedStart;
                    uint32_t mappedEnd;
                    bool isAdpcmOverride;

                    isAdpcmOverride = compressionOverride == BAE_EDITOR_COMPRESSION_ADPCM;

                    if (isAdpcmOverride)
                    {
                        /* Keep ADPCM preview loops in source-frame coordinates. */
                        mappedStart = loopStart;
                        mappedEnd = loopEnd;
                    }
                    else
                    {
                        mappedStart = static_cast<uint32_t>((((uint64_t)loopStart * (uint64_t)targetFrames) +
                                                             ((uint64_t)sourceFrames / 2ULL)) /
                                                            (uint64_t)sourceFrames);
                        mappedEnd = static_cast<uint32_t>((((uint64_t)loopEnd * (uint64_t)targetFrames) +
                                                           ((uint64_t)sourceFrames / 2ULL)) /
                                                          (uint64_t)sourceFrames);
                    }
                    if (mappedStart > targetFrames) mappedStart = targetFrames;
                    if (mappedEnd > targetFrames) mappedEnd = targetFrames;
                    if (mappedEnd <= mappedStart) {
                        if (mappedStart < targetFrames) {
                            mappedEnd = mappedStart + 1;
                        } else if (targetFrames > 0) {
                            mappedStart = targetFrames - 1;
                            mappedEnd = targetFrames;
                        } else {
                            mappedStart = 0;
                            mappedEnd = 0;
                        }
                    }
                    loopStart = mappedStart;
                    loopEnd = mappedEnd;
                }
                if (loopStart < targetFrames && loopEnd <= targetFrames && loopEnd > loopStart) {
                    BAESound_SetSampleLoopPoints(m_previewSound, loopStart, loopEnd);
                    BAESound_SetLoopCount(m_previewSound, 0xFFFFFFFFu);
                } else {
                    BAESound_SetLoopCount(m_previewSound, 0);
                }
            } else {
                BAESound_SetLoopCount(m_previewSound, 0);
            }
        }

        /* Check whether this instrument has playAtSampledFreq set.  When the
         * flag is active the engine plays every note at the sample's native
         * rate with no pitch transposition, so the preview must do the same. */
        bool playAtSampledFreq = false;
        {
            uint32_t instID = 0;
            BAERmfEditorInstrumentExtInfo extInfo;
            if (BAERmfEditorDocument_GetInstIDForSample(m_document, sampleIndex, &instID) == BAE_NO_ERROR) {
                memset(&extInfo, 0, sizeof(extInfo));
                if (BAERmfEditorDocument_GetInstrumentExtInfo(m_document, instID, &extInfo) == BAE_NO_ERROR) {
                    playAtSampledFreq = (extInfo.flags2 & 0x40) != 0; /* ZBF_playAtSampledFreq */
                }
            }
        }

        previewRoot = static_cast<int>(NormalizeRootKeyForSingleKeySplit(sampleInfo.rootKey,
                                                                          sampleInfo.lowKey,
                                                                          sampleInfo.highKey));
        if (rootKeyOverride <= 127) {
            previewRoot = static_cast<int>(rootKeyOverride);
        }
        if (previewRoot < 0 || previewRoot > 127) {
            previewRoot = static_cast<int>((overrideInfo ? overrideInfo->baseMidiPitch : sampleInfo.sampleInfo.baseMidiPitch) & 0x7F);
        }
        BAE_UNSIGNED_FIXED rate;
        if (playAtSampledFreq) {
            /* Play at the sample's native rate with no transposition, matching
             * the engine's behaviour (GenSynth: NotePitch = 1.0 * sampleRate/22050).
             * Use intendedSampleRate so live instrument-editor overrides (including
             * Opus Round-Trip preview rate edits) are honoured. */
            BAE_UNSIGNED_FIXED nativeRate = intendedSampleRate;
            if (nativeRate < (4000U << 16)) {
                if (nativeRate >= 4000U && nativeRate <= 384000U) {
                    nativeRate <<= 16;
                } else {
                    nativeRate = (44100U << 16);
                }
            }
            if (previewNeedsFrameRateComp &&
                previewSourceFramesForRate > 0 &&
                previewDecodedFramesForRate > 0 &&
                previewDecodedFramesForRate != previewSourceFramesForRate) {
                double adjustedRate = (static_cast<double>(nativeRate) *
                                       static_cast<double>(previewSourceFramesForRate)) /
                                      static_cast<double>(previewDecodedFramesForRate);
                BAE_UNSIGNED_FIXED scaledRate = static_cast<BAE_UNSIGNED_FIXED>(std::clamp(adjustedRate,
                                                                                            kMinRateFixed,
                                                                                            kMaxRateFixed));
                fprintf(stderr,
                        "[nbstudio] Preview sample %u playAtSampledFreq rate adjust %lu -> %lu (sourceFrames=%u decodedFrames=%u)\n",
                        static_cast<unsigned>(sampleIndex),
                        static_cast<unsigned long>(nativeRate),
                        static_cast<unsigned long>(scaledRate),
                        static_cast<unsigned>(previewSourceFramesForRate),
                        static_cast<unsigned>(previewDecodedFramesForRate));
                nativeRate = scaledRate;
            }
            rate = nativeRate;
        } else {
            semitones = midiKey - previewRoot + loadRateOctaveShift * 12;
            double ratio = std::pow(2.0, static_cast<double>(semitones) / 12.0);
            rate = static_cast<BAE_UNSIGNED_FIXED>(std::clamp(static_cast<double>(baseSampleRate) * ratio,
                                 kMinRateFixed,
                                 kMaxRateFixed));
        }
        int splitVolume = sampleInfo.splitVolume;
        if (overrideInfo) {
            splitVolume = splitVolumeOverride;
        }
        if (splitVolume <= 0) {
            splitVolume = 100;
        }
        double volumeScale = static_cast<double>(splitVolume) / 100.0;
        volumeScale *= GetPreviewVolumeScale();
        BAE_UNSIGNED_FIXED volume = static_cast<BAE_UNSIGNED_FIXED>(std::clamp(0.7 * volumeScale,
                                             0.0,
                                             4.0) * 65536.0);

        rateResult = BAESound_SetRate(m_previewSound, rate);
        if (rateResult != BAE_NO_ERROR) {
            fprintf(stderr,
                    "[nbstudio] Preview sample %u SetRate failed result=%d rate=%lu\n",
                    static_cast<unsigned>(sampleIndex),
                    static_cast<int>(rateResult),
                    static_cast<unsigned long>(rate));
            return false;
        }
        startResult = BAESound_Start(m_previewSound, 0, volume, 0);
        if (startResult != BAE_NO_ERROR) {
            return false;
        }
        return true;
    }

    void StopPreviewSampleForTag(int previewTag) {
        auto found = m_taggedPreviewSounds.find(previewTag);

        if (found != m_taggedPreviewSounds.end()) {
            if (found->second) {
                BAESound_Stop(found->second, FALSE);
                BAESound_Delete(found->second);
            }
            m_taggedPreviewSounds.erase(found);
        }

        {
            auto noteFound = m_taggedInstrumentPreviewNotes.find(previewTag);
            if (noteFound != m_taggedInstrumentPreviewNotes.end()) {
                    if (m_keyboardPreviewSong) {
#if _DEBUG
                    fprintf(stderr,
                            "[nbstudio] note-preview off-start tag=%d ch=%u note=%u prog=%u bank=%u active=%zu\n",
                            previewTag,
                            static_cast<unsigned>(noteFound->second.channel),
                            static_cast<unsigned>(noteFound->second.note),
                            static_cast<unsigned>(noteFound->second.program),
                            static_cast<unsigned>(noteFound->second.bank),
                            m_taggedInstrumentPreviewNotes.size());
#endif
                        (void)BAESong_ProgramBankChange(m_keyboardPreviewSong,
                                                    noteFound->second.channel,
                                                    noteFound->second.program,
                                                    noteFound->second.bank,
                                                    0);
                        BAESong_NoteOff(m_keyboardPreviewSong,
                                    noteFound->second.channel,
                                    noteFound->second.note,
                                    0,
                                    0);
                        BAESong_ControlChange(m_keyboardPreviewSong,
                                              noteFound->second.channel,
                                              64,
                                              0,
                                              0);
                        BAESong_ControlChange(m_keyboardPreviewSong,
                                              noteFound->second.channel,
                                              66,
                                              0,
                                              0);
#if _DEBUG
                    fprintf(stderr,
                            "[nbstudio] note-preview off-sent tag=%d ch=%u note=%u + channel-panic\n",
                            previewTag,
                            static_cast<unsigned>(noteFound->second.channel),
                            static_cast<unsigned>(noteFound->second.note));
#endif
                }
                m_taggedInstrumentPreviewNotes.erase(noteFound);
            }
#if _DEBUG
            else {
                fprintf(stderr,
                        "[nbstudio] note-preview off-miss tag=%d active=%zu\n",
                        previewTag,
                        m_taggedInstrumentPreviewNotes.size());
            }
#endif
        }
    }

    void StopPreviewSample() {
        if (m_previewSound) {
            BAESound_Stop(m_previewSound, FALSE);
            BAESound_Delete(m_previewSound);
            m_previewSound = nullptr;
        }
        for (auto &entry : m_taggedPreviewSounds) {
            if (!entry.second) {
                continue;
            }
            BAESound_Stop(entry.second, FALSE);
            BAESound_Delete(entry.second);
        }
        m_taggedPreviewSounds.clear();
        if (m_keyboardPreviewSong) {
            for (auto const &entry : m_taggedInstrumentPreviewNotes) {
                (void)BAESong_ProgramBankChange(m_keyboardPreviewSong,
                                                entry.second.channel,
                                                entry.second.program,
                                                entry.second.bank,
                                                0);
                BAESong_NoteOff(m_keyboardPreviewSong,
                                entry.second.channel,
                                entry.second.note,
                                0,
                                0);
            }
        }
        m_taggedInstrumentPreviewNotes.clear();
    }

    void StopAndDestroyInstrumentPreviewSession() {
        StopPreviewSample();
        StopKeyboardPreview();
    }

    bool ExportSampleToPath(uint32_t sampleIndex, wxString const &path) {
        wxScopedCharBuffer utf8Path;

        if (!m_document) {
            return false;
        }
        utf8Path = path.utf8_str();
        return BAERmfEditorDocument_ExportSampleToFile(m_document,
                                                       sampleIndex,
                                                       const_cast<char *>(utf8Path.data())) == BAE_NO_ERROR;
    }

    bool ReplaceSampleFromPath(uint32_t sampleIndex, wxString const &path) {
        BAEResult result;
        BAESampleInfo sampleInfo;
        wxScopedCharBuffer utf8Path;
        uint32_t assetID;
        uint32_t usageCount;
        bool cloneFirst = false;

        if (!m_document) {
            return false;
        }

        /* Check if this sample's asset is shared across multiple instruments. */
        assetID = 0;
        usageCount = 0;
        if (BAERmfEditorDocument_GetSampleAssetIDForSample(m_document, sampleIndex, &assetID) == BAE_NO_ERROR &&
            BAERmfEditorDocument_GetSampleAssetUsageCount(m_document, assetID, &usageCount) == BAE_NO_ERROR &&
            usageCount > 1) {
            wxMessageDialog choiceDialog(
                this,
                wxString::Format("This sample is shared by %u instrument(s).\n\n"
                                 "\"Replace Sample\" will replace the audio for all instruments sharing this sample.\n"
                                 "\"New Sample\" will create a new sample for this instrument only.",
                                 static_cast<unsigned>(usageCount)),
                "Shared Sample",
                wxYES_NO | wxCANCEL | wxICON_QUESTION | wxYES_DEFAULT);
            choiceDialog.SetYesNoCancelLabels("Replace Sample", "New Sample", "Cancel");
            int choice = choiceDialog.ShowModal();
            if (choice == wxID_CANCEL) {
                return false;
            }
            cloneFirst = (choice == wxID_NO);
        }

        /* If the user chose to create a new sample for this instrument only,
         * clone the asset first so it gets its own independent copy. */
        if (cloneFirst) {
            if (BAERmfEditorDocument_CloneSampleAssetForSample(m_document, sampleIndex, NULL) != BAE_NO_ERROR) {
                wxMessageBox("Failed to create new sample asset.", "Embedded Instruments", wxOK | wxICON_ERROR, this);
                return false;
            }
        }

        utf8Path = path.utf8_str();
        result = BAERmfEditorDocument_ReplaceSampleFromFile(m_document,
                                                            sampleIndex,
                                                            const_cast<char *>(utf8Path.data()),
                                                            &sampleInfo);
        if (result == BAE_BAD_FILE_TYPE) {
            wxMessageBox("Unsupported sample codec. Supported imports are PCM WAV/AIFF, MP3, Ogg Vorbis, FLAC, and Opus.",
                         "Embedded Instruments",
                         wxOK | wxICON_ERROR,
                         this);
            return false;
        }
        if (result != BAE_NO_ERROR) {
            wxMessageBox("Failed to replace sample.", "Embedded Instruments", wxOK | wxICON_ERROR, this);
            return false;
        }

        /* If replacing for all shared uses, propagate the new waveform data
         * to every other sample sharing this asset. */
        if (!cloneFirst && usageCount > 1) {
            result = BAERmfEditorDocument_PropagateReplacementToAsset(m_document, sampleIndex);
            if (result != BAE_NO_ERROR) {
                wxMessageBox("Replaced this sample but failed to update all shared uses.",
                             "Embedded Instruments", wxOK | wxICON_WARNING, this);
            }
        }

        return true;
    }

    static uint16_t DisplayBankFromInternal(uint16_t internalBank) {
        if (internalBank >= 128) {
            return static_cast<uint16_t>((internalBank >> 7) & 0x7F);
        }
        return internalBank;
    }

    static uint16_t InternalBankFromDisplay(uint16_t displayBank) {
        return static_cast<uint16_t>((displayBank & 0x7F) << 7);
    }

    static unsigned char RawMidiBankFromInternal(uint16_t internalBank) {
        if (internalBank >= 128) {
            return static_cast<unsigned char>((internalBank >> 7) & 0x7F);
        }
        return static_cast<unsigned char>(internalBank & 0x7F);
    }

    wxString BuildTrackLabel(int trackZeroBased, BAERmfEditorTrackInfo const &trackInfo) const {
        if (trackInfo.name && trackInfo.name[0]) {
            return wxString::Format("%u: %s", static_cast<unsigned>(trackZeroBased + 1), trackInfo.name);
        }
        return wxString::Format("%u: Ch %u Prog %u", static_cast<unsigned>(trackZeroBased + 1), static_cast<unsigned>(trackInfo.channel + 1), static_cast<unsigned>(trackInfo.program));
    }

    BAERmfEditorDocument *BuildSingleTrackPlaybackDocument(int trackIndex) {
        BAERmfEditorDocument *playDoc;
        uint16_t trackCount;
        unsigned char requiredPrograms[128];
        unsigned char *rmfData;
        uint32_t rmfSize;
        XBOOL useZmf;

        if (!m_document || trackIndex < 0) {
            return nullptr;
        }
        trackCount = 0;
        if (BAERmfEditorDocument_GetTrackCount(m_document, &trackCount) != BAE_NO_ERROR ||
            trackIndex >= static_cast<int>(trackCount)) {
            return nullptr;
        }

        /* Clone from a serialized full document so track-internal data (including
         * conductor/meta setup on track 0) survives in single-track exports. */
        rmfData = nullptr;
        rmfSize = 0;
        useZmf = BAERmfEditorDocument_RequiresZmf(m_document) ? TRUE : FALSE;
        if (BAERmfEditorDocument_SaveAsRmfToMemory(m_document,
                                                    useZmf,
                                                    &rmfData,
                                                    &rmfSize) != BAE_NO_ERROR || !rmfData || rmfSize == 0) {
            return nullptr;
        }

        playDoc = BAERmfEditorDocument_LoadFromMemory(rmfData, rmfSize, BAE_RMF);
        XDisposePtr((XPTR)rmfData);
        if (!playDoc) {
            return nullptr;
        }

        if (BAERmfEditorDocument_GetTrackCount(playDoc, &trackCount) != BAE_NO_ERROR) {
            BAERmfEditorDocument_Delete(playDoc);
            return nullptr;
        }

        /* Keep conductor track 0 plus the selected track. */
        for (int idx = static_cast<int>(trackCount) - 1; idx >= 0; --idx) {
            bool keepTrack;

            keepTrack = (idx == 0) || (idx == trackIndex);
            if (!keepTrack) {
                if (BAERmfEditorDocument_DeleteTrack(playDoc, static_cast<uint16_t>(idx)) != BAE_NO_ERROR) {
                    BAERmfEditorDocument_Delete(playDoc);
                    return nullptr;
                }
            }
        }

        /* Re-apply single-track sample filtering now that the track set is final. */
        std::fill(requiredPrograms, requiredPrograms + 128, static_cast<unsigned char>(0));
        trackCount = 0;
        if (BAERmfEditorDocument_GetTrackCount(playDoc, &trackCount) != BAE_NO_ERROR) {
            BAERmfEditorDocument_Delete(playDoc);
            return nullptr;
        }
        for (uint16_t ti = 0; ti < trackCount; ++ti) {
            BAERmfEditorTrackInfo info;
            uint32_t noteCount;

            if (BAERmfEditorDocument_GetTrackInfo(playDoc, ti, &info) == BAE_NO_ERROR) {
                if (info.program < 128) {
                    requiredPrograms[info.program] = 1;
                }
            }
            noteCount = 0;
            if (BAERmfEditorDocument_GetNoteCount(playDoc, ti, &noteCount) != BAE_NO_ERROR) {
                continue;
            }
            for (uint32_t ni = 0; ni < noteCount; ++ni) {
                BAERmfEditorNoteInfo noteInfo;

                if (BAERmfEditorDocument_GetNoteInfo(playDoc, ti, ni, &noteInfo) == BAE_NO_ERROR &&
                    noteInfo.program < 128) {
                    requiredPrograms[noteInfo.program] = 1;
                }
            }
        }

        {
            uint32_t sampleCount;

            sampleCount = 0;
            if (BAERmfEditorDocument_GetSampleCount(playDoc, &sampleCount) != BAE_NO_ERROR) {
                BAERmfEditorDocument_Delete(playDoc);
                return nullptr;
            }
            for (int si = static_cast<int>(sampleCount) - 1; si >= 0; --si) {
                BAERmfEditorSampleInfo sampleInfo;

                if (BAERmfEditorDocument_GetSampleInfo(playDoc, static_cast<uint32_t>(si), &sampleInfo) != BAE_NO_ERROR) {
                    continue;
                }
                if (sampleInfo.program >= 128 || !requiredPrograms[sampleInfo.program]) {
                    if (BAERmfEditorDocument_DeleteSample(playDoc, static_cast<uint32_t>(si)) != BAE_NO_ERROR) {
                        BAERmfEditorDocument_Delete(playDoc);
                        return nullptr;
                    }
                }
            }
        }

        fprintf(stderr,
                "[nbstudio] Single-track build keepTrack0=1 selectedTrack=%d (program filter restored)\n",
                trackIndex);
        return playDoc;
    }

    static bool IsMidiChannelEnabled(uint16_t channelMask, unsigned char channel) {
        if (channel > 15) {
            return false;
        }
        return (channelMask & static_cast<uint16_t>(1u << channel)) != 0;
    }

    void OpenPlaybackChannelsDialog() {
        uint16_t mask;

        if (m_openingPlaybackChannelsDialog) {
            return;
        }
        m_openingPlaybackChannelsDialog = true;
        (void)PromptForPlaybackChannelMask(&mask);
        m_openingPlaybackChannelsDialog = false;
    }

    BAERmfEditorDocument *BuildMidiExportDocument(BAERmfEditorDocument *sourceDoc) {
        BAERmfEditorDocument *midiDoc;
        unsigned char *rmfData;
        uint32_t rmfSize;
        XBOOL useZmf;
        uint32_t sampleCount;

        if (!sourceDoc) {
            return nullptr;
        }

        rmfData = nullptr;
        rmfSize = 0;
        useZmf = BAERmfEditorDocument_RequiresZmf(sourceDoc) ? TRUE : FALSE;
        if (BAERmfEditorDocument_SaveAsRmfToMemory(sourceDoc,
                                                   useZmf,
                                                   &rmfData,
                                                   &rmfSize) != BAE_NO_ERROR ||
            !rmfData ||
            rmfSize == 0) {
            return nullptr;
        }

        midiDoc = BAERmfEditorDocument_LoadFromMemory(rmfData, rmfSize, BAE_RMF);
        XDisposePtr((XPTR)rmfData);
        if (!midiDoc) {
            return nullptr;
        }

        sampleCount = 0;
        if (BAERmfEditorDocument_GetSampleCount(midiDoc, &sampleCount) != BAE_NO_ERROR) {
            BAERmfEditorDocument_Delete(midiDoc);
            return nullptr;
        }
        while (sampleCount > 0) {
            if (BAERmfEditorDocument_DeleteSample(midiDoc, sampleCount - 1) != BAE_NO_ERROR) {
                BAERmfEditorDocument_Delete(midiDoc);
                return nullptr;
            }
            --sampleCount;
        }

        return midiDoc;
    }

    static bool DocumentHasTrackEvents(BAERmfEditorDocument *document) {
        uint16_t trackCount;

        if (!document) {
            return false;
        }
        trackCount = 0;
        if (BAERmfEditorDocument_GetTrackCount(document, &trackCount) != BAE_NO_ERROR) {
            return false;
        }
        for (uint16_t trackIndex = 0; trackIndex < trackCount; ++trackIndex) {
            uint32_t noteCount;
            uint32_t pitchBendCount;

            noteCount = 0;
            if (BAERmfEditorDocument_GetNoteCount(document, trackIndex, &noteCount) == BAE_NO_ERROR && noteCount > 0) {
                return true;
            }

            for (int controller = 0; controller < 128; ++controller) {
                uint32_t ccEventCount;

                ccEventCount = 0;
                if (BAERmfEditorDocument_GetTrackCCEventCount(document,
                                                              trackIndex,
                                                              static_cast<unsigned char>(controller),
                                                              &ccEventCount) == BAE_NO_ERROR &&
                    ccEventCount > 0) {
                    return true;
                }
            }

            pitchBendCount = 0;
            if (BAERmfEditorDocument_GetTrackPitchBendEventCount(document, trackIndex, &pitchBendCount) == BAE_NO_ERROR &&
                pitchBendCount > 0) {
                return true;
            }
        }
        return false;
    }

    bool PromptForPlaybackChannelMask(uint16_t *outMask) {
        wxDialog dialog(this,
                        wxID_ANY,
                        "Playback Channels",
                        wxDefaultPosition,
                        wxDefaultSize,
                        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
        wxBoxSizer *rootSizer;
        wxFlexGridSizer *grid;
        wxBoxSizer *quickActionSizer;
        wxCheckBox *checkboxes[16] = {nullptr};
        int index;

        if (!outMask) {
            return false;
        }

        rootSizer = new wxBoxSizer(wxVERTICAL);
        rootSizer->Add(new wxStaticText(&dialog, wxID_ANY, "Select channels to include in playback:"),
                       0,
                       wxALL,
                       10);

        grid = new wxFlexGridSizer(4, 8, 4, 10);
        for (index = 0; index < 8; ++index) {
            checkboxes[index] = new wxCheckBox(&dialog, wxID_ANY, wxEmptyString);
            checkboxes[index]->SetValue(IsMidiChannelEnabled(m_playbackChannelMask, static_cast<unsigned char>(index)));
            grid->Add(checkboxes[index], 0, wxALIGN_CENTER_HORIZONTAL);
        }
        for (index = 0; index < 8; ++index) {
            grid->Add(new wxStaticText(&dialog, wxID_ANY, wxString::Format("Ch %d", index + 1)),
                      0,
                      wxALIGN_CENTER_HORIZONTAL);
        }
        for (index = 8; index < 16; ++index) {
            checkboxes[index] = new wxCheckBox(&dialog, wxID_ANY, wxEmptyString);
            checkboxes[index]->SetValue(IsMidiChannelEnabled(m_playbackChannelMask, static_cast<unsigned char>(index)));
            grid->Add(checkboxes[index], 0, wxALIGN_CENTER_HORIZONTAL);
        }
        for (index = 8; index < 16; ++index) {
            grid->Add(new wxStaticText(&dialog, wxID_ANY, wxString::Format("Ch %d", index + 1)),
                      0,
                      wxALIGN_CENTER_HORIZONTAL);
        }

        rootSizer->Add(grid, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

        quickActionSizer = new wxBoxSizer(wxHORIZONTAL);
        {
            wxButton *allButton = new wxButton(&dialog, wxID_ANY, "All");
            wxButton *noneButton = new wxButton(&dialog, wxID_ANY, "None");
            wxButton *invertButton = new wxButton(&dialog, wxID_ANY, "Invert");

            quickActionSizer->Add(allButton, 0, wxRIGHT, 8);
            quickActionSizer->Add(noneButton, 0, wxRIGHT, 8);
            quickActionSizer->Add(invertButton, 0);

            allButton->Bind(wxEVT_BUTTON, [&checkboxes](wxCommandEvent &) {
                for (int i = 0; i < 16; ++i) {
                    if (checkboxes[i]) {
                        checkboxes[i]->SetValue(true);
                    }
                }
            });
            noneButton->Bind(wxEVT_BUTTON, [&checkboxes](wxCommandEvent &) {
                for (int i = 0; i < 16; ++i) {
                    if (checkboxes[i]) {
                        checkboxes[i]->SetValue(false);
                    }
                }
            });
            invertButton->Bind(wxEVT_BUTTON, [&checkboxes](wxCommandEvent &) {
                for (int i = 0; i < 16; ++i) {
                    if (checkboxes[i]) {
                        checkboxes[i]->SetValue(!checkboxes[i]->GetValue());
                    }
                }
            });
        }
        rootSizer->Add(quickActionSizer, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
        rootSizer->Add(dialog.CreateButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 10);

        dialog.SetSizerAndFit(rootSizer);
        dialog.SetMinSize(dialog.GetSize());

        if (dialog.ShowModal() != wxID_OK) {
            return false;
        }

        {
            uint16_t newMask = 0;
            for (index = 0; index < 16; ++index) {
                if (checkboxes[index] && checkboxes[index]->GetValue()) {
                    newMask |= static_cast<uint16_t>(1u << index);
                }
            }
            if (newMask == 0) {
                wxMessageBox("Select at least one channel for playback.",
                             "Playback Channels",
                             wxOK | wxICON_INFORMATION,
                             this);
                return false;
            }
            m_playbackChannelMask = newMask;
            *outMask = newMask;
        }
        return true;
    }

    BAERmfEditorDocument *BuildChannelPlaybackDocument(uint16_t channelMask) {
        BAERmfEditorDocument *playDoc;
        uint16_t trackCount;
        unsigned char requiredPrograms[128];
        unsigned char *rmfData;
        uint32_t rmfSize;
        XBOOL useZmf;

        if (!m_document || channelMask == 0) {
            return nullptr;
        }

        rmfData = nullptr;
        rmfSize = 0;
        useZmf = BAERmfEditorDocument_RequiresZmf(m_document) ? TRUE : FALSE;
        if (BAERmfEditorDocument_SaveAsRmfToMemory(m_document,
                                                   useZmf,
                                                   &rmfData,
                                                   &rmfSize) != BAE_NO_ERROR ||
            !rmfData ||
            rmfSize == 0) {
            return nullptr;
        }

        playDoc = BAERmfEditorDocument_LoadFromMemory(rmfData, rmfSize, BAE_RMF);
        XDisposePtr((XPTR)rmfData);
        if (!playDoc) {
            return nullptr;
        }

        trackCount = 0;
        if (BAERmfEditorDocument_GetTrackCount(playDoc, &trackCount) != BAE_NO_ERROR) {
            BAERmfEditorDocument_Delete(playDoc);
            return nullptr;
        }

        /* Keep conductor track 0, then only tracks whose default channel is selected. */
        for (int idx = static_cast<int>(trackCount) - 1; idx >= 1; --idx) {
            BAERmfEditorTrackInfo trackInfo;

            if (BAERmfEditorDocument_GetTrackInfo(playDoc, static_cast<uint16_t>(idx), &trackInfo) != BAE_NO_ERROR) {
                continue;
            }
            if (!IsMidiChannelEnabled(channelMask, trackInfo.channel)) {
                if (BAERmfEditorDocument_DeleteTrack(playDoc, static_cast<uint16_t>(idx)) != BAE_NO_ERROR) {
                    BAERmfEditorDocument_Delete(playDoc);
                    return nullptr;
                }
            }
        }

        trackCount = 0;
        if (BAERmfEditorDocument_GetTrackCount(playDoc, &trackCount) != BAE_NO_ERROR) {
            BAERmfEditorDocument_Delete(playDoc);
            return nullptr;
        }

        /* Remove note events on channels not selected (supports mixed-channel tracks). */
        for (uint16_t ti = 0; ti < trackCount; ++ti) {
            uint32_t noteCount;

            noteCount = 0;
            if (BAERmfEditorDocument_GetNoteCount(playDoc, ti, &noteCount) != BAE_NO_ERROR) {
                continue;
            }
            for (int ni = static_cast<int>(noteCount) - 1; ni >= 0; --ni) {
                BAERmfEditorNoteInfo noteInfo;

                if (BAERmfEditorDocument_GetNoteInfo(playDoc, ti, static_cast<uint32_t>(ni), &noteInfo) != BAE_NO_ERROR) {
                    continue;
                }
                if (!IsMidiChannelEnabled(channelMask, noteInfo.channel)) {
                    if (BAERmfEditorDocument_DeleteNote(playDoc, ti, static_cast<uint32_t>(ni)) != BAE_NO_ERROR) {
                        BAERmfEditorDocument_Delete(playDoc);
                        return nullptr;
                    }
                }
            }
        }

        /* Re-apply sample filtering against the kept tracks/notes. */
        std::fill(requiredPrograms, requiredPrograms + 128, static_cast<unsigned char>(0));
        for (uint16_t ti = 0; ti < trackCount; ++ti) {
            BAERmfEditorTrackInfo info;
            uint32_t noteCount;

            if (BAERmfEditorDocument_GetTrackInfo(playDoc, ti, &info) == BAE_NO_ERROR) {
                if (info.program < 128) {
                    requiredPrograms[info.program] = 1;
                }
            }
            noteCount = 0;
            if (BAERmfEditorDocument_GetNoteCount(playDoc, ti, &noteCount) != BAE_NO_ERROR) {
                continue;
            }
            for (uint32_t ni = 0; ni < noteCount; ++ni) {
                BAERmfEditorNoteInfo noteInfo;

                if (BAERmfEditorDocument_GetNoteInfo(playDoc, ti, ni, &noteInfo) == BAE_NO_ERROR &&
                    noteInfo.program < 128) {
                    requiredPrograms[noteInfo.program] = 1;
                }
            }
        }

        {
            uint32_t sampleCount;

            sampleCount = 0;
            if (BAERmfEditorDocument_GetSampleCount(playDoc, &sampleCount) != BAE_NO_ERROR) {
                BAERmfEditorDocument_Delete(playDoc);
                return nullptr;
            }
            for (int si = static_cast<int>(sampleCount) - 1; si >= 0; --si) {
                BAERmfEditorSampleInfo sampleInfo;

                if (BAERmfEditorDocument_GetSampleInfo(playDoc, static_cast<uint32_t>(si), &sampleInfo) != BAE_NO_ERROR) {
                    continue;
                }
                if (sampleInfo.program >= 128 || !requiredPrograms[sampleInfo.program]) {
                    if (BAERmfEditorDocument_DeleteSample(playDoc, static_cast<uint32_t>(si)) != BAE_NO_ERROR) {
                        BAERmfEditorDocument_Delete(playDoc);
                        return nullptr;
                    }
                }
            }
        }

        return playDoc;
    }

    void InvalidatePianoRollPreviewSong() {
        StopPianoRollPreview(true);
    }

    bool EnsurePianoRollPreviewSong() {
        BAEResult loadResult;
        BAELoadResult loadInfo;

        if (m_notePreviewSong) {
            return true;
        }
        if (!m_document) {
            return false;
        }
        if (!EnsurePlaybackEngine()) {
            return false;
        }
        if (!BuildPreviewPlaybackBlob(m_document, &m_notePreviewSongBlob)) {
            return false;
        }
        memset(&loadInfo, 0, sizeof(loadInfo));
        loadResult = BAEMixer_LoadFromMemory(m_playbackMixer,
                                             m_notePreviewSongBlob.data(),
                                             static_cast<uint32_t>(m_notePreviewSongBlob.size()),
                                             &loadInfo);
        if (loadResult != BAE_NO_ERROR || loadInfo.type != BAE_LOAD_TYPE_SONG || !loadInfo.data.song) {
            BAELoadResult_Cleanup(&loadInfo);
            m_notePreviewSongBlob.clear();
            return false;
        }
        m_notePreviewSong = loadInfo.data.song;
        loadInfo.type = BAE_LOAD_TYPE_NONE;
        loadInfo.data.song = nullptr;
        BAESong_Preroll(m_notePreviewSong);
        BAESong_SetVolume(m_notePreviewSong, GetPreviewVolumeFixed());
        BAESong_SetLoops(m_notePreviewSong, 0);
        return true;
    }

    void StopPianoRollPreview(bool releaseSong) {
        if (m_notePreviewTimer.IsRunning()) {
            m_notePreviewTimer.Stop();
        }
        if (m_notePreviewSong && m_notePreviewActive) {
            BAESong_NoteOff(m_notePreviewSong,
                            m_notePreviewChannel,
                            m_notePreviewNote,
                            0,
                            0);
            BAESong_Stop(m_notePreviewSong, FALSE);
        }
        m_notePreviewActive = false;
        if (releaseSong && m_notePreviewSong) {
            BAESong_AllNotesOff(m_notePreviewSong, 0);
            BAESong_Stop(m_notePreviewSong, FALSE);
            BAESong_Delete(m_notePreviewSong);
            m_notePreviewSong = nullptr;
            m_notePreviewSongBlob.clear();
        }
    }

    void StopKeyboardPreview() {
        if (m_keyboardPreviewSong) {
            BAESong_AllNotesOff(m_keyboardPreviewSong, 0);
            BAESong_Stop(m_keyboardPreviewSong, FALSE);
            BAESong_Delete(m_keyboardPreviewSong);
            m_keyboardPreviewSong = nullptr;
            m_keyboardPreviewSongBlob.clear();
            m_taggedInstrumentPreviewNotes.clear();
        }
    }

    void SeekKeyboardPreviewToSongEnd() {
        uint32_t songLengthUsec;

        if (!m_keyboardPreviewSong) {
            return;
        }
        songLengthUsec = 0;
        if (BAESong_GetMicrosecondLength(m_keyboardPreviewSong, &songLengthUsec) == BAE_NO_ERROR &&
            songLengthUsec > 0) {
            BAESong_SetMicrosecondPosition(m_keyboardPreviewSong, songLengthUsec - 1);
        }
    }

    /* ---- Bank editor preview ---- */

    bool EnsureBankPreviewSong() {
        if (m_bankPreviewSong) {
            return true;
        }
        if (!EnsurePlaybackEngine()) {
            return false;
        }
        /* Build a minimal empty MIDI to host the preview song.
         * Format 0, 1 track, 120 ppqn, empty track with End-of-Track meta. */
        static const unsigned char kMinimalMidi[] = {
            'M','T','h','d', 0,0,0,6, 0,0, 0,1, 0,120,
            'M','T','r','k', 0,0,0,4, 0,0xFF,0x2F,0x00
        };
        m_bankPreviewSongBlob.assign(kMinimalMidi, kMinimalMidi + sizeof(kMinimalMidi));
        BAESong song = BAESong_New(m_playbackMixer);
        if (!song) {
            m_bankPreviewSongBlob.clear();
            return false;
        }
        if (BAESong_LoadMidiFromMemory(song,
                                        m_bankPreviewSongBlob.data(),
                                        static_cast<uint32_t>(m_bankPreviewSongBlob.size()),
                                        TRUE) != BAE_NO_ERROR) {
            BAESong_Delete(song);
            m_bankPreviewSongBlob.clear();
            return false;
        }
        m_bankPreviewSong = song;
        BAESong_Preroll(m_bankPreviewSong);
        BAESong_SetVolume(m_bankPreviewSong, GetPreviewVolumeFixed());
        BAESong_SetLoops(m_bankPreviewSong, 0);
        return true;
    }

    void PlayBankPreviewNote(unsigned char bank, unsigned char program, int note, int previewTag, bool isPercussion) {
        unsigned char channel;

        if (!EnsureBankPreviewSong()) {
            return;
        }
        if (isPercussion) {
            /* Percussion instruments must use channel 9 (MIDI channel 10).
             * The 'program' from the bank editor is instID%128, which is the
             * note offset within the percussion bank.  Use it as the MIDI note
             * and set the drum-kit program to 0. */
            channel = 9;
            note = static_cast<int>(program);  /* percussion note */
            program = 0;                       /* drum kit 0 */
        } else {
            /* Map preview tags to channels like the keyboard preview does */
            if (previewTag >= 'A' && previewTag <= 'Z') {
                /* Musical typing key codes use alternating channels */
                static const unsigned char kTagChannels[] = {
                    0,1,2,3,4,5,6,7,8,15,10,11,12,13,14
                };
                int idx = previewTag - 'A';
                channel = (idx < (int)(sizeof(kTagChannels)/sizeof(kTagChannels[0])))
                    ? kTagChannels[idx] : 0;
            } else {
                channel = 0;
            }
            /* Avoid drums channel for melodic instruments */
            if (channel == 9) {
                channel = 10;
            }
        }

        /* Stop any existing note on this tag */
        StopBankPreviewNote(previewTag);

        /* Set up channel state BEFORE the Preroll/Start cycle so the engine
         * picks up the correct program and its ADSR/LFO/filter parameters. */
        BAESong_UnmuteChannel(m_bankPreviewSong, channel);
        BAESong_ControlChange(m_bankPreviewSong, channel, 7, 127, 0);
        BAESong_ControlChange(m_bankPreviewSong, channel, 11, 127, 0);
        BAESong_ControlChange(m_bankPreviewSong, channel, 10, 64, 0);
        BAESong_ControlChange(m_bankPreviewSong, channel, 121, 0, 0);
        BAESong_ControlChange(m_bankPreviewSong, channel, 127, 0, 0);
        BAESong_ControlChange(m_bankPreviewSong, channel, 64, 0, 0);
        BAESong_ControlChange(m_bankPreviewSong, channel, 66, 0, 0);
        BAESong_ProgramBankChange(m_bankPreviewSong, channel, program,
                                  RawMidiBankFromInternal(static_cast<uint16_t>(bank)), 0);

        /* Only Preroll/Start the song once for the first note.  Doing it
         * for every note resets all channel programs back to defaults via
         * PV_ResetControlers, which causes a race where concurrent notes
         * hear program 0 (standard piano) instead of the correct patch. */
        if (!m_bankPreviewSongStarted) {
            uint32_t songLengthUsec = 0;
            BAESong_GetMicrosecondLength(m_bankPreviewSong, &songLengthUsec);
            if (songLengthUsec > 0) {
                BAESong_SetMicrosecondPosition(m_bankPreviewSong, songLengthUsec - 1);
            }
            BAESong_Preroll(m_bankPreviewSong);
            BAESong_Start(m_bankPreviewSong, 0);
            if (songLengthUsec > 0) {
                BAESong_SetMicrosecondPosition(m_bankPreviewSong, songLengthUsec - 1);
            }
            m_bankPreviewSongStarted = true;
        }

        /* Pre-load the instrument once; reuse the cached version for
         * subsequent keypresses of the same instrument.  This avoids
         * the occasional load failure when the engine is busy. */
        {
            BAE_INSTRUMENT inst = TranslateBankProgramToInstrument(
                static_cast<uint16_t>(bank), program, channel,
                static_cast<unsigned char>(std::clamp(note, 0, 127)));
            bool needsLoad = (inst != m_bankPreviewLoadedInst);
            bool needsPatch = m_hasBankDirtyExtInfo && (!m_bankPreviewDirtyApplied || needsLoad);

            if (needsLoad) {
                if (BAESong_LoadInstrument(m_bankPreviewSong, inst) == BAE_NO_ERROR) {
                    m_bankPreviewLoadedInst = inst;
                    m_bankPreviewDirtyApplied = false;
                }
            }

            if (needsPatch) {
                BAESong_PatchLoadedInstrumentExtInfo(m_bankPreviewSong,
                                                     inst, &m_bankDirtyExtInfo);
                m_bankPreviewDirtyApplied = true;
            }
        }
        BAESong_NoteOn(m_bankPreviewSong, channel,
                        static_cast<unsigned char>(std::clamp(note, 0, 127)),
                        100, 0);

        BankPreviewNote bpn;
        bpn.channel = channel;
        bpn.note = static_cast<unsigned char>(std::clamp(note, 0, 127));
        bpn.program = program;
        bpn.bank = bank;
        m_bankPreviewNotes[previewTag] = bpn;
    }

    void StopBankPreviewNote(int previewTag) {
        if (!m_bankPreviewSong) {
            return;
        }
        if (previewTag < 0) {
            /* Stop all */
            for (auto const &entry : m_bankPreviewNotes) {
                BAESong_NoteOff(m_bankPreviewSong, entry.second.channel,
                                entry.second.note, 0, 0);
            }
            m_bankPreviewNotes.clear();
        } else {
            auto it = m_bankPreviewNotes.find(previewTag);
            if (it != m_bankPreviewNotes.end()) {
                BAESong_NoteOff(m_bankPreviewSong, it->second.channel,
                                it->second.note, 0, 0);
                m_bankPreviewNotes.erase(it);
            }
        }
    }

    void StopBankPreview() {
        if (m_bankPreviewSong) {
            BAESong_Panic(m_bankPreviewSong);
            BAESong_Delete(m_bankPreviewSong);
            m_bankPreviewSong = nullptr;
            m_bankPreviewSongStarted = false;
            m_bankPreviewLoadedInst = static_cast<BAE_INSTRUMENT>(-1);
            m_bankPreviewDirtyApplied = false;
            m_bankPreviewSongBlob.clear();
            m_bankPreviewNotes.clear();
        }
    }

    void PreviewPianoRollNote(uint16_t bank,
                              unsigned char program,
                              unsigned char noteChannel,
                              unsigned char note,
                              uint32_t durationTicks,
                              int trackIndex) {
        BAERmfEditorTrackInfo trackInfo;
        uint64_t noteDurationUsec;
        uint32_t noteDurationMs;
        uint32_t songLengthUsec;
        BAEResult startResult;
        BAEResult programResult;
        unsigned char channel;

        if (!m_document) {
            return;
        }
        if (trackIndex < 0 || BAERmfEditorDocument_GetTrackInfo(m_document, static_cast<uint16_t>(trackIndex), &trackInfo) != BAE_NO_ERROR) {
            return;
        }
        if (!EnsurePianoRollPreviewSong()) {
            return;
        }

        channel = (noteChannel <= 15) ? noteChannel : trackInfo.channel;
        StopPianoRollPreview(false);

        /* Start from the song end so tick-0 events do not fire during manual note preview. */
        songLengthUsec = 0;
        if (BAESong_GetMicrosecondLength(m_notePreviewSong, &songLengthUsec) == BAE_NO_ERROR && songLengthUsec > 0) {
            BAESong_SetMicrosecondPosition(m_notePreviewSong, songLengthUsec - 1);
        }

        /* Keep preview simple and deterministic: run the preview song only while auditioning. */
        BAESong_Preroll(m_notePreviewSong);
        if (BAESong_Start(m_notePreviewSong, 0) != BAE_NO_ERROR) {
            return;
        }
        if (songLengthUsec > 0) {
            BAESong_SetMicrosecondPosition(m_notePreviewSong, songLengthUsec - 1);
        }

        /* Ensure the preview channel is audible regardless of prior track automation state. */
        BAESong_UnmuteChannel(m_notePreviewSong, channel);
        BAESong_ControlChange(m_notePreviewSong, channel, 7, 127, 0);
        BAESong_ControlChange(m_notePreviewSong, channel, 11, 127, 0);

        if (channel != 9) {
            programResult = BAESong_ProgramBankChange(m_notePreviewSong,
                                                      channel,
                                                      program,
                                                      RawMidiBankFromInternal(bank),
                                                      0);
            if (programResult != BAE_NO_ERROR) {
                return;
            }
        }

        /* Load the instrument ourselves and use NoteOn (not NoteOnWithLoad)
         * to avoid the race where NoteOnWithLoad reads stale channel state
         * from the queued ProgramBankChange above. */
        {
            BAE_INSTRUMENT previewInst = TranslateBankProgramToInstrument(
                RawMidiBankFromInternal(bank), program, channel, note);
            BAESong_LoadInstrument(m_notePreviewSong, previewInst);
        }

        startResult = BAESong_NoteOn(m_notePreviewSong, channel, note, 100, 0);
        if (startResult != BAE_NO_ERROR) {
            return;
        }

        m_notePreviewActive = true;
        m_notePreviewChannel = channel;
        m_notePreviewNote = note;

        noteDurationUsec = TicksToMicroseconds(std::max<uint32_t>(durationTicks, 1));
        noteDurationMs = static_cast<uint32_t>(std::clamp<uint64_t>((noteDurationUsec + 999ULL) / 1000ULL, 40ULL, 15000ULL));
        m_notePreviewTimer.StartOnce(static_cast<int>(noteDurationMs));
    }

    void OnNotePreviewTimer(wxTimerEvent &) {
        StopPianoRollPreview(false);
    }

    void StopPlayback(bool releaseSong) {
        if (!m_playbackSong) {
            m_playbackTimer.Stop();
            if (releaseSong) {
                m_playbackSongBlob.clear();
            }
            return;
        }
        BAESong_Stop(m_playbackSong, FALSE);
        if (releaseSong) {
            BAESong_Delete(m_playbackSong);
            m_playbackSong = nullptr;
            m_playbackTimer.Stop();
            m_playbackSongBlob.clear();
        }
    }

    void ShutdownPlaybackEngine() {
        StopPianoRollPreview(true);
        if (m_playbackMixer) {
            BAEMixer_Close(m_playbackMixer);
            BAEMixer_Delete(m_playbackMixer);
            m_playbackMixer = nullptr;
        }
    }

    bool EnsurePlaybackTempPath(bool asMidi) {
        wxString targetPath;

        targetPath = wxFileName::GetTempDir() + (asMidi ? "/rmfeditwx_playback.mid" : "/rmfeditwx_playback.rmf");
        if (m_playbackTempPath == targetPath) {
            return true;
        }
        m_playbackTempPath = targetPath;
        return !m_playbackTempPath.empty();
    }

    bool EnsurePreviewSampleTempPath() {
        if (!m_previewSampleTempPath.empty()) {
            return true;
        }
        m_previewSampleTempPath = wxFileName::GetTempDir() + "/rmfeditwx_preview_sample.wav";
        return !m_previewSampleTempPath.empty();
    }

    bool BuildCompressedPreviewSampleFile(uint32_t sampleIndex,
                                          BAERmfEditorCompressionType compressionType,
                                          wxString *outPath) {
        wxString sampleBasePath;
        wxString samplePath;
        wxScopedCharBuffer utf8SamplePath;
        unsigned char *rmfData;
        uint32_t rmfSize;
        BAERmfEditorDocument *tempDoc;
        BAERmfEditorDocument *exportDoc;
        BAERmfEditorSampleInfo info;
        std::string displayName;
        BAEResult result;

        if (!m_document || !outPath) {
            return false;
        }
        if (compressionType == BAE_EDITOR_COMPRESSION_DONT_CHANGE ||
            compressionType == BAE_EDITOR_COMPRESSION_PCM) {
            return false;
        }

        rmfData = nullptr;
        rmfSize = 0;
        if (BAERmfEditorDocument_SaveAsRmfToMemory(m_document,
                                                    FALSE,
                                                    &rmfData,
                                                    &rmfSize) != BAE_NO_ERROR || !rmfData || rmfSize == 0) {
            return false;
        }

        tempDoc = BAERmfEditorDocument_LoadFromMemory(rmfData, rmfSize, BAE_RMF);
        XDisposePtr((XPTR)rmfData);
        if (!tempDoc) {
            return false;
        }

        if (BAERmfEditorDocument_GetSampleInfo(tempDoc, sampleIndex, &info) != BAE_NO_ERROR) {
            BAERmfEditorDocument_Delete(tempDoc);
            return false;
        }

        displayName = info.displayName ? info.displayName : "";
        info.displayName = const_cast<char *>(displayName.c_str());
        info.compressionType = compressionType;
        if (BAERmfEditorDocument_SetSampleInfo(tempDoc, sampleIndex, &info) != BAE_NO_ERROR) {
            BAERmfEditorDocument_Delete(tempDoc);
            return false;
        }

        /* Materialize the updated target compression into a fresh document so
         * ExportSampleToFile cannot passthrough stale original compressed data. */
        rmfData = nullptr;
        rmfSize = 0;
        if (BAERmfEditorDocument_SaveAsRmfToMemory(tempDoc,
                                                    FALSE,
                                                    &rmfData,
                                                    &rmfSize) != BAE_NO_ERROR || !rmfData || rmfSize == 0) {
            BAERmfEditorDocument_Delete(tempDoc);
            return false;
        }
        BAERmfEditorDocument_Delete(tempDoc);

        exportDoc = BAERmfEditorDocument_LoadFromMemory(rmfData, rmfSize, BAE_RMF);
        XDisposePtr((XPTR)rmfData);
        if (!exportDoc) {
            return false;
        }

        sampleBasePath = wxFileName::CreateTempFileName("nbstudio_cmp_sample");
        if (sampleBasePath.empty()) {
            BAERmfEditorDocument_Delete(exportDoc);
            return false;
        }

        switch (compressionType) {
            case BAE_EDITOR_COMPRESSION_MP3_32K:
            case BAE_EDITOR_COMPRESSION_MP3_48K:
            case BAE_EDITOR_COMPRESSION_MP3_64K:
            case BAE_EDITOR_COMPRESSION_MP3_96K:
            case BAE_EDITOR_COMPRESSION_MP3_128K:
            case BAE_EDITOR_COMPRESSION_MP3_192K:
            case BAE_EDITOR_COMPRESSION_MP3_256K:
            case BAE_EDITOR_COMPRESSION_MP3_320K:
                samplePath = sampleBasePath + ".mp3";
                break;
            case BAE_EDITOR_COMPRESSION_VORBIS_32K:
            case BAE_EDITOR_COMPRESSION_VORBIS_48K:
            case BAE_EDITOR_COMPRESSION_VORBIS_64K:
            case BAE_EDITOR_COMPRESSION_VORBIS_80K:
            case BAE_EDITOR_COMPRESSION_VORBIS_96K:
            case BAE_EDITOR_COMPRESSION_VORBIS_128K:
            case BAE_EDITOR_COMPRESSION_VORBIS_160K:
            case BAE_EDITOR_COMPRESSION_VORBIS_192K:
            case BAE_EDITOR_COMPRESSION_VORBIS_256K:
                samplePath = sampleBasePath + ".ogg";
                break;
            case BAE_EDITOR_COMPRESSION_OPUS_12K:
            case BAE_EDITOR_COMPRESSION_OPUS_16K:
            case BAE_EDITOR_COMPRESSION_OPUS_24K:
            case BAE_EDITOR_COMPRESSION_OPUS_32K:
            case BAE_EDITOR_COMPRESSION_OPUS_48K:
            case BAE_EDITOR_COMPRESSION_OPUS_64K:
            case BAE_EDITOR_COMPRESSION_OPUS_80K:
            case BAE_EDITOR_COMPRESSION_OPUS_96K:
            case BAE_EDITOR_COMPRESSION_OPUS_128K:
            case BAE_EDITOR_COMPRESSION_OPUS_160K:
            case BAE_EDITOR_COMPRESSION_OPUS_192K:
            case BAE_EDITOR_COMPRESSION_OPUS_256K:
                samplePath = sampleBasePath + ".opus";
                break;
            case BAE_EDITOR_COMPRESSION_FLAC:
                samplePath = sampleBasePath + ".flac";
                break;
            case BAE_EDITOR_COMPRESSION_ADPCM:
                samplePath = sampleBasePath + ".aif";
                break;
            default:
                samplePath = sampleBasePath + ".wav";
                break;
        }

        utf8SamplePath = samplePath.utf8_str();
        result = BAERmfEditorDocument_ExportSampleToFile(exportDoc,
                                                         sampleIndex,
                                                         const_cast<char *>(utf8SamplePath.data()));
        BAERmfEditorDocument_Delete(exportDoc);

        if (wxFileExists(sampleBasePath)) {
            wxRemoveFile(sampleBasePath);
        }
        if (result != BAE_NO_ERROR) {
            if (wxFileExists(samplePath)) {
                wxRemoveFile(samplePath);
            }
            return false;
        }

        *outPath = samplePath;
        return true;
    }

    bool BuildCompressedPreviewReloadedWaveform(uint32_t sampleIndex,
                                                BAERmfEditorCompressionType compressionType,
                                                std::vector<unsigned char> *outWaveData,
                                                uint32_t *outFrameCount,
                                                uint16_t *outBitSize,
                                                uint16_t *outChannels,
                                                BAE_UNSIGNED_FIXED *outSampleRate,
                                                uint32_t *outLoopStart,
                                                uint32_t *outLoopEnd) {
        unsigned char *rmfData;
        uint32_t rmfSize;
        BAERmfEditorDocument *tempDoc;
        BAERmfEditorDocument *previewDoc;
        BAERmfEditorSampleInfo info;
        void const *waveData;
        uint32_t frameCount;
        uint16_t bitSize;
        uint16_t channels;
        BAE_UNSIGNED_FIXED sampleRate;
        uint32_t bytesPerFrame;
        std::string displayName;

        if (!m_document || !outWaveData || !outFrameCount || !outBitSize || !outChannels ||
            !outSampleRate || !outLoopStart || !outLoopEnd) {
            return false;
        }
        if (compressionType == BAE_EDITOR_COMPRESSION_DONT_CHANGE ||
            compressionType == BAE_EDITOR_COMPRESSION_PCM) {
            return false;
        }

        /* Clone the document via memory so we can modify compression settings
         * without affecting the original. */
        rmfData = nullptr;
        rmfSize = 0;
        if (BAERmfEditorDocument_SaveAsRmfToMemory(m_document,
                                                    FALSE,
                                                    &rmfData,
                                                    &rmfSize) != BAE_NO_ERROR || !rmfData || rmfSize == 0) {
            return false;
        }

        tempDoc = BAERmfEditorDocument_LoadFromMemory(rmfData, rmfSize, BAE_RMF);
        XDisposePtr((XPTR)rmfData);
        if (!tempDoc) {
            return false;
        }

        if (BAERmfEditorDocument_GetSampleInfo(tempDoc, sampleIndex, &info) != BAE_NO_ERROR) {
            BAERmfEditorDocument_Delete(tempDoc);
            return false;
        }

        displayName = info.displayName ? info.displayName : "";
        info.displayName = const_cast<char *>(displayName.c_str());
        info.compressionType = compressionType;
        if (BAERmfEditorDocument_SetSampleInfo(tempDoc, sampleIndex, &info) != BAE_NO_ERROR) {
            BAERmfEditorDocument_Delete(tempDoc);
            return false;
        }

        /* Re-serialize with the new compression applied, then reload so the
         * waveform data reflects the encode/decode round-trip. */
        rmfData = nullptr;
        rmfSize = 0;
        if (BAERmfEditorDocument_SaveAsRmfToMemory(tempDoc,
                                                    FALSE,
                                                    &rmfData,
                                                    &rmfSize) != BAE_NO_ERROR || !rmfData || rmfSize == 0) {
            BAERmfEditorDocument_Delete(tempDoc);
            return false;
        }
        BAERmfEditorDocument_Delete(tempDoc);

        previewDoc = BAERmfEditorDocument_LoadFromMemory(rmfData, rmfSize, BAE_RMF);
        XDisposePtr((XPTR)rmfData);
        if (!previewDoc) {
            return false;
        }

        waveData = nullptr;
        frameCount = 0;
        bitSize = 16;
        channels = 1;
        sampleRate = 0;
        if (BAERmfEditorDocument_GetSampleWaveformData(previewDoc,
                                                       sampleIndex,
                                                       &waveData,
                                                       &frameCount,
                                                       &bitSize,
                                                       &channels,
                                                       &sampleRate) != BAE_NO_ERROR ||
            !waveData || frameCount == 0) {
            BAERmfEditorDocument_Delete(previewDoc);
            return false;
        }
        if (BAERmfEditorDocument_GetSampleInfo(previewDoc, sampleIndex, &info) != BAE_NO_ERROR) {
            BAERmfEditorDocument_Delete(previewDoc);
            return false;
        }

        bytesPerFrame = (static_cast<uint32_t>(bitSize) / 8U) * static_cast<uint32_t>(channels);
        if (bytesPerFrame == 0) {
            BAERmfEditorDocument_Delete(previewDoc);
            return false;
        }

        outWaveData->assign(static_cast<unsigned char const *>(waveData),
                            static_cast<unsigned char const *>(waveData) + (static_cast<size_t>(frameCount) * bytesPerFrame));
        *outFrameCount = frameCount;
        *outBitSize = bitSize;
        *outChannels = channels;
        *outSampleRate = sampleRate;
        *outLoopStart = info.sampleInfo.startLoop;
        *outLoopEnd = info.sampleInfo.endLoop;

        BAERmfEditorDocument_Delete(previewDoc);
        return true;
    }

    bool BuildMinimalKeyboardPreviewMidi(std::vector<unsigned char> *outData) {
        // Builds a minimal MIDI file with just one empty track for keyboard preview
        // This isolates keyboard events from document timeline events
        outData->clear();
        
        // MIDI Header: MThd
        outData->push_back('M');
        outData->push_back('T');
        outData->push_back('h');
        outData->push_back('d');
        
        // Header length: 6 bytes
        outData->push_back(0x00);
        outData->push_back(0x00);
        outData->push_back(0x00);
        outData->push_back(0x06);
        
        // Format: 0 (single track)
        outData->push_back(0x00);
        outData->push_back(0x00);
        
        // Number of tracks: 1
        outData->push_back(0x00);
        outData->push_back(0x01);
        
        // Division (PPQ): 480
        outData->push_back(0x01);
        outData->push_back(0xE0);
        
        // Track header: MTrk
        outData->push_back('M');
        outData->push_back('T');
        outData->push_back('r');
        outData->push_back('k');
        
        // Track length: 4 bytes (delta-time + end-of-track meta event)
        outData->push_back(0x00);
        outData->push_back(0x00);
        outData->push_back(0x00);
        outData->push_back(0x04);
        
        // End of track: delta-time (0) + FF 2F 00
        outData->push_back(0x00);  // delta-time = 0
        outData->push_back(0xFF);  // Meta event
        outData->push_back(0x2F);  // End of track
        outData->push_back(0x00);  // Length = 0
        
        return true;
    }

    bool BuildKeyboardPreviewRmfWithInstruments(std::vector<unsigned char> *outData) {
        // Build an RMF from the document that includes all instruments and samples
        // for keyboard preview. This allows instrument resolution to work properly
        // instead of falling back to GM on bank notes.
        unsigned char *blobData;
        uint32_t blobSize;
        BAEResult saveResult;
        bool requiresZmf;

        if (!m_document || !outData) {
            return false;
        }

        outData->clear();

        // Use the document directly to save as RMF - it includes all instruments
        requiresZmf = (BAERmfEditorDocument_RequiresZmf(m_document) != 0);
        blobData = nullptr;
        blobSize = 0;
        saveResult = BAERmfEditorDocument_SaveAsRmfToMemory(m_document,
                                                            requiresZmf ? TRUE : FALSE,
                                                            &blobData,
                                                            &blobSize);
        if (saveResult != BAE_NO_ERROR || !blobData || blobSize == 0) {
            return false;
        }

        outData->assign(blobData, blobData + blobSize);
        XDisposePtr((XPTR)blobData);
        return true;
    }

    bool EnsureKeyboardPreviewSong() {
        BAEResult loadResult;
        BAELoadResult loadInfo;

        if (m_keyboardPreviewSong) {
            return true;
        }
        if (!EnsurePlaybackEngine()) {
            return false;
        }
        if (!BuildKeyboardPreviewRmfWithInstruments(&m_keyboardPreviewSongBlob)) {
            return false;
        }
        memset(&loadInfo, 0, sizeof(loadInfo));
        loadResult = BAEMixer_LoadFromMemory(m_playbackMixer,
                                             m_keyboardPreviewSongBlob.data(),
                                             static_cast<uint32_t>(m_keyboardPreviewSongBlob.size()),
                                             &loadInfo);
        if (loadResult != BAE_NO_ERROR || loadInfo.type != BAE_LOAD_TYPE_SONG || !loadInfo.data.song) {
            BAELoadResult_Cleanup(&loadInfo);
            m_keyboardPreviewSongBlob.clear();
            return false;
        }
        m_keyboardPreviewSong = loadInfo.data.song;
        loadInfo.type = BAE_LOAD_TYPE_NONE;
        loadInfo.data.song = nullptr;
        BAESong_Preroll(m_keyboardPreviewSong);
        BAESong_SetVolume(m_keyboardPreviewSong, GetPreviewVolumeFixed());
        BAESong_SetLoops(m_keyboardPreviewSong, 0);
        if (BAESong_Start(m_keyboardPreviewSong, 0) != BAE_NO_ERROR) {
            BAESong_Delete(m_keyboardPreviewSong);
            m_keyboardPreviewSong = nullptr;
            m_keyboardPreviewSongBlob.clear();
            return false;
        }
        SeekKeyboardPreviewToSongEnd();
        return true;
    }

    bool BuildPreviewPlaybackBlob(BAERmfEditorDocument *playDoc, std::vector<unsigned char> *outData) {
        unsigned char *blobData;
        uint32_t blobSize;
        BAEResult saveResult;
        bool requiresZmf;

        if (!playDoc || !outData) {
            return false;
        }
        requiresZmf = (BAERmfEditorDocument_RequiresZmf(playDoc) != 0);
        blobData = nullptr;
        blobSize = 0;
        saveResult = BAERmfEditorDocument_SaveAsRmfToMemory(playDoc,
                                                            requiresZmf ? TRUE : FALSE,
                                                            &blobData,
                                                            &blobSize);
        if (saveResult != BAE_NO_ERROR || !blobData || blobSize == 0) {
            return false;
        }

        outData->assign(blobData, blobData + blobSize);
        XDisposePtr((XPTR)blobData);
        return true;
    }

    bool CopyExtendedInstrumentDataForPrograms(BAERmfEditorDocument *dest,
                                               BAERmfEditorDocument const *src,
                                               unsigned char const *programFlags128) {
        uint32_t sampleCount;
        std::vector<uint32_t> copiedInstIds;

        if (!dest || !src || !programFlags128) {
            return false;
        }
        sampleCount = 0;
        if (BAERmfEditorDocument_GetSampleCount(src, &sampleCount) != BAE_NO_ERROR) {
            return false;
        }
        for (uint32_t i = 0; i < sampleCount; ++i) {
            BAERmfEditorSampleInfo sampleInfo;
            uint32_t instID;
            BAERmfEditorInstrumentExtInfo extInfo;

            if (BAERmfEditorDocument_GetSampleInfo(src, i, &sampleInfo) != BAE_NO_ERROR) {
                continue;
            }
            if (sampleInfo.program >= 128 || !programFlags128[sampleInfo.program]) {
                continue;
            }
            if (BAERmfEditorDocument_GetInstIDForSample(src, i, &instID) != BAE_NO_ERROR) {
                continue;
            }
            if (std::find(copiedInstIds.begin(), copiedInstIds.end(), instID) != copiedInstIds.end()) {
                continue;
            }
            if (BAERmfEditorDocument_GetInstrumentExtInfo(src, instID, &extInfo) != BAE_NO_ERROR) {
                continue;
            }
            if (BAERmfEditorDocument_SetInstrumentExtInfo(dest, instID, &extInfo) != BAE_NO_ERROR) {
                return false;
            }
            copiedInstIds.push_back(instID);
        }
        return true;
    }

    uint64_t TicksToMicroseconds(uint32_t tick) const {
        uint16_t tpq;
        uint32_t baseBpm;
        uint32_t tempoCount;
        uint32_t currentTick;
        uint32_t currentMicrosecondsPerQuarter;
        uint32_t tempoIndex;
        uint64_t usec;

        if (!m_document) {
            return 0;
        }
        tpq = 480;
        BAERmfEditorDocument_GetTicksPerQuarter(m_document, &tpq);
        if (tpq == 0) {
            tpq = 480;
        }
        baseBpm = 120;
        BAERmfEditorDocument_GetTempoBPM(m_document, &baseBpm);
        if (baseBpm == 0) {
            baseBpm = 120;
        }
        tempoCount = 0;
        currentTick = 0;
        currentMicrosecondsPerQuarter = 60000000UL / baseBpm;
        usec = 0;
        if (BAERmfEditorDocument_GetTempoEventCount(m_document, &tempoCount) != BAE_NO_ERROR) {
            return (static_cast<uint64_t>(tick) * static_cast<uint64_t>(currentMicrosecondsPerQuarter)) / static_cast<uint64_t>(tpq);
        }
        for (tempoIndex = 0; tempoIndex < tempoCount; ++tempoIndex) {
            uint32_t eventTick;
            uint32_t eventMicrosecondsPerQuarter;

            if (BAERmfEditorDocument_GetTempoEvent(m_document, tempoIndex, &eventTick, &eventMicrosecondsPerQuarter) != BAE_NO_ERROR) {
                continue;
            }
            if (eventTick > tick) {
                break;
            }
            if (eventTick > currentTick) {
                usec += (static_cast<uint64_t>(eventTick - currentTick) * static_cast<uint64_t>(currentMicrosecondsPerQuarter)) / static_cast<uint64_t>(tpq);
            }
            currentTick = eventTick;
            if (eventMicrosecondsPerQuarter > 0) {
                currentMicrosecondsPerQuarter = eventMicrosecondsPerQuarter;
            }
        }
        if (tick > currentTick) {
            usec += (static_cast<uint64_t>(tick - currentTick) * static_cast<uint64_t>(currentMicrosecondsPerQuarter)) / static_cast<uint64_t>(tpq);
        }
        return usec;
    }

    uint32_t MicrosecondsToTicks(uint32_t usec) const {
        uint16_t tpq;
        uint32_t baseBpm;
        uint32_t tempoCount;
        uint32_t currentTick;
        uint32_t currentMicrosecondsPerQuarter;
        uint32_t tempoIndex;
        uint64_t remainingUsec;

        if (!m_document) {
            return 0;
        }
        tpq = 480;
        BAERmfEditorDocument_GetTicksPerQuarter(m_document, &tpq);
        if (tpq == 0) {
            tpq = 480;
        }
        baseBpm = 120;
        BAERmfEditorDocument_GetTempoBPM(m_document, &baseBpm);
        if (baseBpm == 0) {
            baseBpm = 120;
        }
        tempoCount = 0;
        currentTick = 0;
        currentMicrosecondsPerQuarter = 60000000UL / baseBpm;
        remainingUsec = usec;
        if (BAERmfEditorDocument_GetTempoEventCount(m_document, &tempoCount) != BAE_NO_ERROR) {
            return static_cast<uint32_t>((remainingUsec * static_cast<uint64_t>(tpq)) / static_cast<uint64_t>(currentMicrosecondsPerQuarter));
        }
        for (tempoIndex = 0; tempoIndex < tempoCount; ++tempoIndex) {
            uint32_t eventTick;
            uint32_t eventMicrosecondsPerQuarter;

            if (BAERmfEditorDocument_GetTempoEvent(m_document, tempoIndex, &eventTick, &eventMicrosecondsPerQuarter) != BAE_NO_ERROR) {
                continue;
            }
            if (eventTick <= currentTick) {
                if (eventMicrosecondsPerQuarter > 0) {
                    currentMicrosecondsPerQuarter = eventMicrosecondsPerQuarter;
                }
                continue;
            }
            {
                uint32_t segmentTicks;
                uint64_t segmentUsec;

                segmentTicks = eventTick - currentTick;
                segmentUsec = (static_cast<uint64_t>(segmentTicks) * static_cast<uint64_t>(currentMicrosecondsPerQuarter)) / static_cast<uint64_t>(tpq);
                if (remainingUsec < segmentUsec) {
                    return currentTick + static_cast<uint32_t>((remainingUsec * static_cast<uint64_t>(tpq)) / static_cast<uint64_t>(currentMicrosecondsPerQuarter));
                }
                remainingUsec -= segmentUsec;
                currentTick = eventTick;
            }
            if (eventMicrosecondsPerQuarter > 0) {
                currentMicrosecondsPerQuarter = eventMicrosecondsPerQuarter;
            }
        }
        return currentTick + static_cast<uint32_t>((remainingUsec * static_cast<uint64_t>(tpq)) / static_cast<uint64_t>(currentMicrosecondsPerQuarter));
    }

    wxString FormatUsecClock(uint64_t usec) const {
        uint64_t totalSeconds;
        uint64_t minutes;
        uint64_t seconds;
        uint64_t tenths;

        totalSeconds = usec / 1000000ULL;
        minutes = totalSeconds / 60ULL;
        seconds = totalSeconds % 60ULL;
        tenths = (usec % 1000000ULL) / 100000ULL;
        return wxString::Format("%llu:%02llu.%llu",
                                static_cast<unsigned long long>(minutes),
                                static_cast<unsigned long long>(seconds),
                                static_cast<unsigned long long>(tenths));
    }

    wxString FormatLoopTimeUsec(uint64_t usec) const {
        uint64_t totalSeconds;
        uint64_t minutes;
        uint64_t seconds;
        uint64_t millis;

        totalSeconds = usec / 1000000ULL;
        minutes = totalSeconds / 60ULL;
        seconds = totalSeconds % 60ULL;
        millis = (usec % 1000000ULL) / 1000ULL;
        return wxString::Format("%llu:%02llu.%03llu",
                                static_cast<unsigned long long>(minutes),
                                static_cast<unsigned long long>(seconds),
                                static_cast<unsigned long long>(millis));
    }

    bool ParseLoopTimeToUsec(wxString const &text, uint64_t *outUsec) const {
        wxString trimmed;
        wxString minutesPart;
        wxString secondsPart;
        long minutes;
        double secondsValue;
        double totalSeconds;
        int colonPos;
        size_t i;
        uint64_t usec;

        if (!outUsec) {
            return false;
        }
        trimmed = text;
        trimmed.Trim(true);
        trimmed.Trim(false);
        if (trimmed.empty()) {
            return false;
        }

        colonPos = trimmed.Find(':');
        if (colonPos != wxNOT_FOUND) {
            if (trimmed.Mid(static_cast<size_t>(colonPos + 1)).Find(':') != wxNOT_FOUND) {
                return false;
            }
            minutesPart = trimmed.SubString(0, colonPos - 1);
            secondsPart = trimmed.SubString(colonPos + 1, static_cast<int>(trimmed.length()) - 1);
            if (minutesPart.empty() || secondsPart.empty()) {
                return false;
            }
            for (i = 0; i < minutesPart.length(); ++i) {
                if (minutesPart[i] < '0' || minutesPart[i] > '9') {
                    return false;
                }
            }
            {
                int dotPos;

                dotPos = secondsPart.Find('.');
                if (dotPos != wxNOT_FOUND && secondsPart.Mid(static_cast<size_t>(dotPos + 1)).Find('.') != wxNOT_FOUND) {
                    return false;
                }
            }
            for (i = 0; i < secondsPart.length(); ++i) {
                if (!((secondsPart[i] >= '0' && secondsPart[i] <= '9') || secondsPart[i] == '.')) {
                    return false;
                }
            }
            if (!minutesPart.ToLong(&minutes) || !secondsPart.ToDouble(&secondsValue)) {
                return false;
            }
            if (minutes < 0 || secondsValue < 0.0 || secondsValue >= 60.0) {
                return false;
            }
            usec = static_cast<uint64_t>(std::llround((static_cast<double>(minutes) * 60.0 + secondsValue) * 1000000.0));
            if (usec > 0xFFFFFFFFULL) {
                return false;
            }
            *outUsec = usec;
            return true;
        } else {
            for (i = 0; i < trimmed.length(); ++i) {
                char c;

                c = static_cast<char>(trimmed[i]);
                if (!((c >= '0' && c <= '9') || c == '.')) {
                    return false;
                }
            }
            if (!trimmed.ToDouble(&totalSeconds)) {
                return false;
            }
            if (totalSeconds < 0.0) {
                return false;
            }
            usec = static_cast<uint64_t>(std::llround(totalSeconds * 1000000.0));
            if (usec > 0xFFFFFFFFULL) {
                return false;
            }
            *outUsec = usec;
            return true;
        }
    }

    void UpdatePositionLabel(uint64_t posUsec, uint64_t lenUsec) {
        if (!m_positionLabel) {
            return;
        }
        m_positionLabel->SetLabel(wxString::Format("%s / %s",
                                                   FormatUsecClock(posUsec),
                                                   FormatUsecClock(lenUsec)));
    }

    void UpdatePositionLabelFromDocumentTick(uint32_t tick) {
        uint32_t endTick;
        uint64_t posUsec;
        uint64_t lenUsec;

        endTick = PianoRollPanel_GetDocumentEndTick(m_pianoRoll);
        posUsec = TicksToMicroseconds(tick);
        lenUsec = TicksToMicroseconds(endTick);
        if (lenUsec < posUsec) {
            lenUsec = posUsec;
        }
        UpdatePositionLabel(posUsec, lenUsec);
    }

    int GetSelectedTrack() const {
        return m_trackList->GetSelection();
    }

    void LoadDocument(wxString const &path) {
        wxFile file;
        wxFileOffset length;
        std::vector<unsigned char> data;
        wxString ext;
        BAEFileType fileTypeHint;
        BAERmfEditorDocument *document;

        fileTypeHint = BAE_INVALID_TYPE;
        ext = wxFileName(path).GetExt().Lower();
        if (ext == "rmf" || ext == "zmf") {
            fileTypeHint = BAE_RMF;
        } else if (ext == "rmi") {
            fileTypeHint = BAE_RMI;
        } else if (ext == "mid" || ext == "midi" || ext == "kar") {
            fileTypeHint = BAE_MIDI_TYPE;
        }

        document = nullptr;
        if (file.Open(path, wxFile::read)) {
            length = file.Length();
            if (length > 0) {
                data.assign(static_cast<size_t>(length), 0);
                if (file.Read(data.data(), static_cast<size_t>(length)) == length) {
                    document = BAERmfEditorDocument_LoadFromMemory(data.data(),
                                                                   static_cast<uint32_t>(data.size()),
                                                                   fileTypeHint);
                }
            }
            file.Close();
        }
        if (!document) {
            wxScopedCharBuffer utf8Path;

            utf8Path = path.utf8_str();
            document = BAERmfEditorDocument_LoadFromFile(const_cast<char *>(utf8Path.data()));
        }
        if (!document) {
            wxMessageBox("Failed to open file as RMF or MIDI.", "Open Failed", wxOK | wxICON_ERROR, this);
            return;
        }
        if (m_document) {
        if (!m_previewSampleTempPath.empty() && wxFileExists(m_previewSampleTempPath)) {
            wxRemoveFile(m_previewSampleTempPath);
        }
            BAERmfEditorDocument_Delete(m_document);
        }
        m_document = document;
        {
            BAERmfEditorMidiStorageType storageType;

            if (BAERmfEditorDocument_GetMidiStorageType(m_document, &storageType) == BAE_NO_ERROR) {
                SetSelectedMidiStorageType(storageType);
            }
        }
        {
            // If the document has per-song engine config, load it into the settings UI.
            int32_t engineFlags = 0;
            if (BAERmfEditorDocument_GetEngineConfig(m_document, &engineFlags) == BAE_NO_ERROR && engineFlags != 0) {
                bool docHasPanFix    = (engineFlags & SONG_CONFIG_HAS_PANFIX) != 0;
                bool docHasChorus    = (engineFlags & SONG_CONFIG_HAS_CLASSIC_CHORUS) != 0;
                bool docPanFix       = (engineFlags & SONG_CONFIG_PANFIX_ON) != 0;
                bool docClassicChorus= (engineFlags & SONG_CONFIG_CLASSIC_CHORUS_ON) != 0;
                if (m_savePreviewToSongCheck) {
                    m_savePreviewToSongCheck->SetValue(true);
                }
                if (m_settingsMenu) {
                    if (docHasPanFix) {
                        m_settingsMenu->Check(ID_SettingsPanFix, docPanFix);
                    }
                    if (docHasChorus) {
                        m_settingsMenu->Check(ID_SettingsClassicChorus, docClassicChorus);
                    }
                }
                ApplyMixerEngineSettings();
            }
        }
        InvalidatePianoRollPreviewSong();
        ClearUndoHistory();
        m_currentPath = path;
        m_sessionPath.clear();
        MarkDocumentClean();
        PianoRollPanel_SetDocument(m_pianoRoll, m_document);
        PianoRollPanel_ClearPlayhead(m_pianoRoll);
        m_ignoreSeekEvent = true;
        m_positionSlider->SetValue(0);
        m_ignoreSeekEvent = false;
        UpdatePositionLabelFromDocumentTick(0);
        StopPlayback(true);
        PopulateTrackList();
        PopulateSampleList();
        RefreshMidiLoopControlsFromDocument();
        UpdateFrameTitle();
        SetStatusText(path, 1);
    }

    void PopulateTrackList() {
        uint16_t trackCount;
        uint16_t trackIndex;

        m_trackList->Clear();
        if (!m_document) {
            UpdateControlsFromSelection();
            return;
        }
        trackCount = 0;
        BAERmfEditorDocument_GetTrackCount(m_document, &trackCount);
        for (trackIndex = 0; trackIndex < trackCount; ++trackIndex) {
            BAERmfEditorTrackInfo trackInfo;
            wxString label;

            if (BAERmfEditorDocument_GetTrackInfo(m_document, trackIndex, &trackInfo) != BAE_NO_ERROR) {
                continue;
            }
            label = trackInfo.name && trackInfo.name[0]
                        ? wxString::Format("%u: %s", static_cast<unsigned>(trackIndex + 1), trackInfo.name)
                        : wxString::Format("%u: Ch %u Prog %u", static_cast<unsigned>(trackIndex + 1), static_cast<unsigned>(trackInfo.channel + 1), static_cast<unsigned>(trackInfo.program));
            m_trackList->Append(label);
        }
        if (!m_trackList->IsEmpty()) {
            m_trackList->SetSelection(0);
            PianoRollPanel_SetSelectedTrack(m_pianoRoll, 0);
        } else {
            PianoRollPanel_SetSelectedTrack(m_pianoRoll, -1);
        }
        UpdateControlsFromSelection();
    }

    void UpdateControlsFromSelection() {
        uint32_t tempoBpm;
        BAERmfEditorTrackInfo trackInfo;
        int selectedTrack;

        m_updatingControls = true;
        selectedTrack = GetSelectedTrack();
        if (!m_document) {
            m_tempoSpin->SetValue(120);
            m_bankSpin->SetValue(0);
            m_programSpin->SetValue(0);
            m_transposeSpin->SetValue(0);
            m_panSpin->SetValue(64);
            m_volumeSpin->SetValue(100);
            m_updatingControls = false;
            return;
        }
        tempoBpm = 120;
        BAERmfEditorDocument_GetTempoBPM(m_document, &tempoBpm);
        m_tempoSpin->SetValue(static_cast<int>(tempoBpm));
        if (selectedTrack >= 0 && BAERmfEditorDocument_GetTrackInfo(m_document, static_cast<uint16_t>(selectedTrack), &trackInfo) == BAE_NO_ERROR) {
            BAERmfEditorNoteInfo selectedNote;

            if (PianoRollPanel_GetSelectedNoteInfo(m_pianoRoll, &selectedNote)) {
                m_bankSpin->SetValue(DisplayBankFromInternal(selectedNote.bank));
                m_programSpin->SetValue(selectedNote.program);
            } else {
                m_bankSpin->SetValue(DisplayBankFromInternal(trackInfo.bank));
                m_programSpin->SetValue(trackInfo.program);
            }
            m_transposeSpin->SetValue(trackInfo.transpose);
            m_panSpin->SetValue(trackInfo.pan);
            m_volumeSpin->SetValue(trackInfo.volume);
            PianoRollPanel_SetNewNoteInstrument(m_pianoRoll,
                                                InternalBankFromDisplay(static_cast<uint16_t>(m_bankSpin->GetValue())),
                                                static_cast<unsigned char>(m_programSpin->GetValue()));
        }
        m_updatingControls = false;
    }

    void UpdateFrameTitle() {
        wxString title;
        wxString displayPath;

        title = "NeoBAE Studio v";
        title += kVersionString;
        displayPath = !m_sessionPath.empty() ? m_sessionPath : m_currentPath;
        if (!displayPath.empty()) {
            title += " - ";
            if (m_hasUnsavedChanges) {
                title += "*";
            }
            title += wxFileNameFromPath(displayPath);
        } else if (m_hasUnsavedChanges && m_document) {
            title += " - *untitled";
        }
        SetTitle(title);
    }

    void OnEditorTabChanged(wxBookCtrlEvent &event) {
        int page = event.GetSelection();
        if (page == kEditorModeMidi) {
            m_editorMode = kEditorModeMidi;
            SwapFileMenu(m_midiFileMenu);
            /* TODO Phase 4: hot-reload bank if dirty */
        } else if (page == kEditorModeBank) {
            if (!m_bankEditorWarningAccepted) {
                int answer = wxMessageBox(
                    "The Bank Editor is incomplete and many functions are not yet "
                    "implemented or not yet functioning correctly.\n\n"
                    "By continuing you agree that you understand this.",
                    "Bank Editor — Incomplete Feature",
                    wxOK | wxCANCEL | wxICON_WARNING,
                    this);
                if (answer != wxOK) {
                    event.Veto();
                    /* Restore the notebook selection to the previous tab without
                     * re-triggering this handler. */
                    m_editorNotebook->Unbind(wxEVT_NOTEBOOK_PAGE_CHANGED, &MainFrame::OnEditorTabChanged, this);
                    m_editorNotebook->SetSelection((size_t)kEditorModeMidi);
                    m_editorNotebook->Bind(wxEVT_NOTEBOOK_PAGE_CHANGED, &MainFrame::OnEditorTabChanged, this);
                    m_editorMode = kEditorModeMidi;
                    SwapFileMenu(m_midiFileMenu);
                    return;
                }
                m_bankEditorWarningAccepted = true;
            }
            m_editorMode = kEditorModeBank;
            SwapFileMenu(m_bankFileMenu);
        }
        event.Skip();
    }

    void SwapFileMenu(wxMenu *newFileMenu) {
        wxMenuBar *menuBar = GetMenuBar();
        if (!menuBar || !newFileMenu) {
            return;
        }
        /* The File menu is always at index 0 */
        wxMenu *oldMenu = menuBar->Replace(0, newFileMenu, "&File");
        (void)oldMenu; /* We keep both menus alive as members; don't delete */
    }

    void SwitchToEditorTab(EditorMode mode) {
        if (m_editorNotebook && (int)mode < m_editorNotebook->GetPageCount()) {
            m_editorNotebook->SetSelection((size_t)mode);
            m_editorMode = mode;
        }
    }

    /* Create a fresh empty document and reset the UI to match.
     * resetSettings: when true (File->New), also reset the Settings menu
     * toggles to their defaults.  Pass false on first startup so the INI
     * settings that were just loaded are not overwritten. */
    void DoNewDocument(bool resetSettings = true) {
        StopPlayback(true);
        if (!m_previewSampleTempPath.empty() && wxFileExists(m_previewSampleTempPath)) {
            wxRemoveFile(m_previewSampleTempPath);
            m_previewSampleTempPath.clear();
        }
        if (m_document) {
            BAERmfEditorDocument_Delete(m_document);
        }
        m_document = BAERmfEditorDocument_New();
        if (m_document) {
            BAERmfEditorTrackSetup setup;
            uint16_t trackIndex;
            char conductorName[] = "Conductor";
            char trackName[] = "Track 1";

            /* Add an explicit conductor (tempo) track first so that MIDI export
             * doesn't silently prepend a synthetic tempo track. */
            memset(&setup, 0, sizeof(setup));
            setup.channel = 0;
            setup.bank    = 0;
            setup.program = 0;
            setup.name    = conductorName;
            BAERmfEditorDocument_AddTrack(m_document, &setup, &trackIndex);

            memset(&setup, 0, sizeof(setup));
            setup.channel = 0;
            setup.bank    = 0;
            setup.program = 0;
            setup.name    = trackName;
            BAERmfEditorDocument_AddTrack(m_document, &setup, &trackIndex);
        }
        m_currentPath.clear();
        m_sessionPath.clear();
        if (resetSettings) {
            if (m_settingsMenu) {
                m_settingsMenu->Check(ID_SettingsPanFix, true);
                m_settingsMenu->Check(ID_SettingsClassicChorus, false);
            }
            if (m_savePreviewToSongCheck) {
                m_savePreviewToSongCheck->SetValue(false);
            }
            ApplyMixerEngineSettings();
        }
        InvalidatePianoRollPreviewSong();
        ClearUndoHistory();
        MarkDocumentClean();
        PianoRollPanel_SetDocument(m_pianoRoll, m_document);
        PianoRollPanel_SetUserEndTick(m_pianoRoll, kDefaultNewDocumentEndTicks);
        PianoRollPanel_ClearPlayhead(m_pianoRoll);
        m_ignoreSeekEvent = true;
        m_positionSlider->SetValue(0);
        m_ignoreSeekEvent = false;
        UpdatePositionLabelFromDocumentTick(0);
        PopulateTrackList();
        /* Select the music track (index 1) rather than the conductor track so
         * the user can start placing notes immediately. */
        if (m_trackList->GetCount() > 1) {
            m_trackList->SetSelection(1);
            PianoRollPanel_SetSelectedTrack(m_pianoRoll, 1);
            UpdateControlsFromSelection();
        }
        PopulateSampleList();
        RefreshMidiLoopControlsFromDocument();
        UpdateFrameTitle();
        SetStatusText(wxEmptyString, 1);
        SwitchToEditorTab(kEditorModeMidi);
    }

    void OnNew(wxCommandEvent &) {
        if (!ConfirmDiscardUnsavedChanges("Create a new document")) {
            return;
        }
        DoNewDocument();
    }

    void OnOpen(wxCommandEvent &) {
        if (!ConfirmDiscardUnsavedChanges("Open a new file")) {
            return;
        }
        wxFileDialog dialog(this,
                            "Open RMF, MIDI, Bank, or Session",
                            wxEmptyString,
                            wxEmptyString,
                            "All supported files (*.rmf;*.zmf;*.mid;*.midi;*.kar;*.rmi;*.hsb;*.zsb;*.nbs)|*.rmf;*.zmf;*.mid;*.midi;*.kar;*.rmi;*.hsb;*.zsb;*.nbs|NeoBAE Session (*.nbs)|*.nbs|RMF files (*.rmf;*.zmf)|*.rmf;*.zmf|MIDI files (*.mid;*.midi;*.kar;*.rmi)|*.mid;*.midi;*.kar;*.rmi|Bank files (*.hsb;*.zsb)|*.hsb;*.zsb|All files (*.*)|*.*",
                            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() == wxID_OK) {
            wxString selectedPath = dialog.GetPath();
            wxString ext = wxFileName(selectedPath).GetExt().Lower();
            if (ext == "nbs") {
                LoadSession(selectedPath);
            } else if (ext == "hsb" || ext == "zsb") {
                if (!ConfirmDiscardBankChanges()) {
                    return;
                }
                LoadBankForEditing(selectedPath);
            } else {
                LoadDocument(selectedPath);
                SwitchToEditorTab(kEditorModeMidi);
            }
        }
    }

    void OnSaveAs(wxCommandEvent &) {
        BAERmfEditorDocument *saveDoc;
        BAERmfEditorDocument *midiSaveDoc;
        bool saveCurrentTrack;
        bool saveChannels;
        int selectedTrack;
        bool requiresZmf;
        bool canSaveAsMidi;
        bool midiRequiresCustomDataDrop;
        wxString sourcePath;
        wxString preferredExt;
        wxString defaultDir;
        wxString defaultFile;

        if (!m_document) {
            wxMessageBox("Nothing is loaded.", "Save As RMF", wxOK | wxICON_INFORMATION, this);
            return;
        }
        ApplyEngineConfigToDocument();
        saveDoc = m_document;
        saveCurrentTrack = (m_playScopeChoice && m_playScopeChoice->GetSelection() == 1);
        saveChannels = (m_playScopeChoice && m_playScopeChoice->GetSelection() == 2);
        selectedTrack = GetSelectedTrack();
        if (saveCurrentTrack) {
            uint16_t trackCount;

            if (selectedTrack < 0) {
                trackCount = 0;
                if (BAERmfEditorDocument_GetTrackCount(m_document, &trackCount) == BAE_NO_ERROR && trackCount > 0) {
                    selectedTrack = 0;
                    if (m_trackList && m_trackList->GetCount() > 0) {
                        m_trackList->SetSelection(0);
                    }
                    PianoRollPanel_SetSelectedTrack(m_pianoRoll, 0);
                }
            }
            saveDoc = BuildSingleTrackPlaybackDocument(selectedTrack);
            if (!saveDoc) {
                wxMessageBox("Failed to build selected track for RMF save.", "Save Failed", wxOK | wxICON_ERROR, this);
                return;
            }
            fprintf(stderr,
                    "[nbstudio] SaveAs using single-track document selectedTrack=%d\n",
                    selectedTrack);
        } else if (saveChannels) {
            if (m_playbackChannelMask == 0) {
                wxMessageBox("No playback channels are enabled.", "Export", wxOK | wxICON_INFORMATION, this);
                return;
            }
            saveDoc = BuildChannelPlaybackDocument(m_playbackChannelMask);
            if (!saveDoc) {
                wxMessageBox("Failed to build channel-filtered export document.", "Save Failed", wxOK | wxICON_ERROR, this);
                return;
            }
            if (!DocumentHasTrackEvents(saveDoc)) {
                BAERmfEditorDocument_Delete(saveDoc);
                wxMessageBox("Selected channels contain no note/CC/pitch events to export.",
                             "Export",
                             wxOK | wxICON_INFORMATION,
                             this);
                return;
            }
            fprintf(stderr,
                    "[nbstudio] SaveAs using channel-filtered document channelMask=0x%04x\n",
                    static_cast<unsigned>(m_playbackChannelMask));
        }

        requiresZmf = BAERmfEditorDocument_RequiresZmf(saveDoc) != 0;
        canSaveAsMidi = (BAERmfEditorDocument_CanSaveAsMidi(saveDoc) != 0);
        midiRequiresCustomDataDrop = !canSaveAsMidi;
        BAERmfEditorDocument_SetMidiStorageType(saveDoc, GetSelectedMidiStorageType());

        sourcePath = !m_sessionPath.empty() ? m_sessionPath : m_currentPath;
        preferredExt = requiresZmf ? "zmf" : (canSaveAsMidi ? "mid" : "rmf");
        defaultFile = "untitled." + preferredExt;
        if (!sourcePath.empty()) {
            wxFileName sourceName(sourcePath);
            wxString baseName = sourceName.GetName();

            if (!sourceName.GetPath().empty()) {
                defaultDir = sourceName.GetPath();
            }
            if (!baseName.empty()) {
                defaultFile = baseName + "." + preferredExt;
            }
        }

        wxFileDialog dialog(this,
                            requiresZmf ? "Save As ZMF/MIDI"
                                                     : "Save As RMF/ZMF/MIDI",
                            defaultDir,
                            defaultFile,
                            requiresZmf ? "ZMF and MIDI files (*.zmf;*.mid;*.midi)|*.zmf;*.mid;*.midi|ZMF files (*.zmf)|*.zmf|MIDI files (*.mid;*.midi)|*.mid;*.midi"
                                                     : "RMF, ZMF, and MIDI files (*.rmf;*.zmf;*.mid;*.midi)|*.rmf;*.zmf;*.mid;*.midi|RMF and ZMF files (*.rmf;*.zmf)|*.rmf;*.zmf|MIDI files (*.mid;*.midi)|*.mid;*.midi",
                            wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dialog.ShowModal() != wxID_OK) {
            if (saveDoc != m_document) {
                BAERmfEditorDocument_Delete(saveDoc);
            }
            return;
        }
        {
            wxString targetPath = dialog.GetPath();
            BAEResult saveResult;
            midiSaveDoc = nullptr;

            if (IsMidiSaveExtension(targetPath)) {
                wxFileName fn(targetPath);
                if (fn.GetExt().Lower() != "mid" && fn.GetExt().Lower() != "midi") {
                    fn.SetExt("mid");
                    targetPath = fn.GetFullPath();
                }
                if (midiRequiresCustomDataDrop) {
                    int warningChoice = wxMessageBox(
                        "This export will write standard MIDI only.\n\n"
                        "Custom instruments and embedded samples from RMF/ZMF cannot be stored in MIDI and will be omitted.",
                        "Export MIDI",
                        wxYES_NO | wxICON_WARNING,
                        this);
                    if (warningChoice != wxYES) {
                        if (saveDoc != m_document) {
                            BAERmfEditorDocument_Delete(saveDoc);
                        }
                        return;
                    }
                    midiSaveDoc = BuildMidiExportDocument(saveDoc);
                    if (!midiSaveDoc) {
                        if (saveDoc != m_document) {
                            BAERmfEditorDocument_Delete(saveDoc);
                        }
                        wxMessageBox("Failed to prepare MIDI export document.",
                                     "Save Failed",
                                     wxOK | wxICON_ERROR,
                                     this);
                        return;
                    }
                }
                wxScopedCharBuffer utf8TargetPath = targetPath.utf8_str();
                saveResult = BAERmfEditorDocument_SaveAsMidi(midiSaveDoc ? midiSaveDoc : saveDoc,
                                                             const_cast<char *>(utf8TargetPath.data()));
                if (saveResult != BAE_NO_ERROR) {
                    if (midiSaveDoc) {
                        BAERmfEditorDocument_Delete(midiSaveDoc);
                    }
                    if (saveDoc != m_document) {
                        BAERmfEditorDocument_Delete(saveDoc);
                    }
                    wxMessageBox("Failed to save MIDI file.", "Save Failed", wxOK | wxICON_ERROR, this);
                    return;
                }
                if (midiSaveDoc) {
                    BAERmfEditorDocument_Delete(midiSaveDoc);
                }
                if (saveDoc != m_document) {
                    BAERmfEditorDocument_Delete(saveDoc);
                }
                SetStatusText(targetPath, 1);
                return;
            }

            /* Enforce .zmf extension when document contains RMF-incompatible sample data */
            if (requiresZmf) {
                wxFileName fn(targetPath);
                if (fn.GetExt().Lower() != "zmf") {
                    fn.SetExt("zmf");
                    targetPath = fn.GetFullPath();
                }
            }
            wxScopedCharBuffer utf8TargetPath = targetPath.utf8_str();
            bool wroteTarget;

            saveResult = BAERmfEditorDocument_SaveAsRmf(saveDoc, const_cast<char *>(utf8TargetPath.data()));
            wroteTarget = (saveResult == BAE_NO_ERROR) && wxFileExists(targetPath) && (wxFileName(targetPath).GetSize().GetValue() > 0);

            if (!wroteTarget) {
                wxString tempBase = wxFileName::CreateTempFileName("nbstudio_save");
                wxString tempRmf = tempBase + ".rmf";
                wxScopedCharBuffer utf8TempPath = tempRmf.utf8_str();

                if (saveResult != BAE_NO_ERROR || BAERmfEditorDocument_SaveAsRmf(saveDoc, const_cast<char *>(utf8TempPath.data())) != BAE_NO_ERROR) {
                    if (wxFileExists(tempBase)) {
                        wxRemoveFile(tempBase);
                    }
                    if (saveDoc != m_document) {
                        BAERmfEditorDocument_Delete(saveDoc);
                    }
                    wxMessageBox("Failed to save RMF file.", "Save Failed", wxOK | wxICON_ERROR, this);
                    return;
                }
                if (wxFileExists(targetPath)) {
                    wxRemoveFile(targetPath);
                }
                if (!wxCopyFile(tempRmf, targetPath, true)) {
                    if (wxFileExists(tempRmf)) {
                        wxRemoveFile(tempRmf);
                    }
                    if (wxFileExists(tempBase)) {
                        wxRemoveFile(tempBase);
                    }
                    if (saveDoc != m_document) {
                        BAERmfEditorDocument_Delete(saveDoc);
                    }
                    wxMessageBox("Failed to write target file path.", "Save Failed", wxOK | wxICON_ERROR, this);
                    return;
                }
                if (wxFileExists(tempRmf)) {
                    wxRemoveFile(tempRmf);
                }
                if (wxFileExists(tempBase)) {
                    wxRemoveFile(tempBase);
                }
            }
            if (saveDoc != m_document) {
                BAERmfEditorDocument_Delete(saveDoc);
            }
            SetStatusText(targetPath, 1);
        }
    }

    bool WriteSessionToFile(wxString const &path) {
        std::vector<unsigned char> rmfBlob;
        std::vector<unsigned char> payload;
        NbsSessionSettings settings;
        size_t compBound;
        size_t compPos;
        std::vector<unsigned char> compressed;
        std::vector<unsigned char> header;
        wxFile file;

        ApplyEngineConfigToDocument();
        if (!CaptureSerializedDocument(&rmfBlob)) {
            wxMessageBox("Failed to serialize document.", "Save Session", wxOK | wxICON_ERROR, this);
            return false;
        }

        memset(&settings, 0, sizeof(settings));
        settings.settingsVersion = 2;
        settings.previewVolume = m_previewVolumeSlider ? m_previewVolumeSlider->GetValue() : 100;
        settings.previewReverbType = static_cast<int32_t>(GetSelectedPreviewReverbType());
        settings.previewLoopEnabled = IsPreviewLoopEnabled() ? 1 : 0;
        settings.midiStorageMode = m_midiStorageChoice ? m_midiStorageChoice->GetSelection() : 1;
        settings.selectedTrack = GetSelectedTrack();
        settings.panFix          = (m_settingsMenu && m_settingsMenu->IsChecked(ID_SettingsPanFix)) ? 1 : 0;
        settings.classicChorus   = (m_settingsMenu && m_settingsMenu->IsChecked(ID_SettingsClassicChorus)) ? 1 : 0;
        settings.savePreviewToSong = (m_savePreviewToSongCheck && m_savePreviewToSongCheck->GetValue()) ? 1 : 0;

        /* Build TLV payload: RMF blob */
        AppendLE16(payload, kNbsFieldRmfBlob);
        AppendLE32(payload, static_cast<uint32_t>(rmfBlob.size()));
        payload.insert(payload.end(), rmfBlob.begin(), rmfBlob.end());

        /* Build TLV payload: settings */
        AppendLE16(payload, kNbsFieldSettings);
        AppendLE32(payload, static_cast<uint32_t>(sizeof(settings)));
        {
            unsigned char const *p = reinterpret_cast<unsigned char const *>(&settings);
            payload.insert(payload.end(), p, p + sizeof(settings));
        }

        /* Build TLV payload: original path */
        if (!m_currentPath.empty()) {
            wxScopedCharBuffer utf8 = m_currentPath.utf8_str();
            size_t len = strlen(utf8.data());
            AppendLE16(payload, kNbsFieldOriginalPath);
            AppendLE32(payload, static_cast<uint32_t>(len));
            payload.insert(payload.end(),
                           reinterpret_cast<unsigned char const *>(utf8.data()),
                           reinterpret_cast<unsigned char const *>(utf8.data()) + len);
        }

        /* Build TLV payload: user end tick */
        {
            uint32_t userEndTick;

            userEndTick = PianoRollPanel_GetUserEndTick(m_pianoRoll);
            if (userEndTick > 0) {
                AppendLE16(payload, kNbsFieldUserEndTick);
                AppendLE32(payload, 4);
                AppendLE32(payload, userEndTick);
            }
        }

        /* Compress with LZMA */
        compBound = lzma_stream_buffer_bound(payload.size());
        compressed.resize(compBound);
        compPos = 0;
        if (lzma_easy_buffer_encode(LZMA_PRESET_DEFAULT, LZMA_CHECK_CRC64, NULL,
                                    payload.data(), payload.size(),
                                    compressed.data(), &compPos, compBound) != LZMA_OK) {
            wxMessageBox("Compression failed.", "Save Session", wxOK | wxICON_ERROR, this);
            return false;
        }
        compressed.resize(compPos);

        /* Write file: 10-byte header + compressed data */
        if (!file.Open(path, wxFile::write)) {
            wxMessageBox("Could not open file for writing.", "Save Session", wxOK | wxICON_ERROR, this);
            return false;
        }

        header.insert(header.end(), kNbsMagic, kNbsMagic + 4);
        AppendLE16(header, kNbsVersion);
        AppendLE32(header, static_cast<uint32_t>(payload.size()));

        if (file.Write(header.data(), header.size()) != header.size() ||
            file.Write(compressed.data(), compressed.size()) != compressed.size()) {
            file.Close();
            wxMessageBox("Failed to write session file.", "Save Session", wxOK | wxICON_ERROR, this);
            return false;
        }
        file.Close();

        m_sessionPath = path;
        MarkDocumentClean();
        UpdateFrameTitle();
        SetStatusText(path, 1);
        return true;
    }

    void OnSaveSession(wxCommandEvent &evt) {
        if (!m_document) {
            wxMessageBox("Nothing is loaded.", "Save Session", wxOK | wxICON_INFORMATION, this);
            return;
        }
        if (m_sessionPath.empty()) {
            OnSaveSessionAs(evt);
            return;
        }
        WriteSessionToFile(m_sessionPath);
    }

    wxString GetDefaultSessionName() {
        if (!m_sessionPath.empty()) {
            return wxFileNameFromPath(m_sessionPath);
        }
        if (!m_currentPath.empty()) {
            wxFileName fn(m_currentPath);
            fn.SetExt("nbs");
            return fn.GetFullName();
        }
        return "New Project.nbs";
    }

    void OnSaveSessionAs(wxCommandEvent &) {
        wxFileDialog dialog(this,
                            "Save Session",
                            wxEmptyString,
                            GetDefaultSessionName(),
                            "NeoBAE Session (*.nbs)|*.nbs",
                            wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

        if (!m_document) {
            wxMessageBox("Nothing is loaded.", "Save Session", wxOK | wxICON_INFORMATION, this);
            return;
        }
        if (dialog.ShowModal() == wxID_OK) {
            wxString path = dialog.GetPath();
            if (wxFileName(path).GetExt().IsEmpty()) {
                path += ".nbs";
            }
            WriteSessionToFile(path);
        }
    }

    bool LoadSession(wxString const &path) {
        wxFile file;
        wxFileOffset fileLen;
        std::vector<unsigned char> fileData;
        uint32_t uncompressedSize;
        std::vector<unsigned char> payload;
        std::vector<unsigned char> rmfBlob;
        NbsSessionSettings settings;
        bool haveSettings;
        size_t offset;
        uint32_t userEndTick;
        uint16_t fileVersion;

        memset(&settings, 0, sizeof(settings));
        settings.selectedTrack = -1;
        haveSettings = false;
        userEndTick = 0;

        if (!file.Open(path, wxFile::read)) {
            wxMessageBox("Could not open session file.", "Open Session", wxOK | wxICON_ERROR, this);
            return false;
        }
        fileLen = file.Length();
        if (fileLen < 10) {
            file.Close();
            wxMessageBox("File is too small to be a valid session.", "Open Session", wxOK | wxICON_ERROR, this);
            return false;
        }
        fileData.resize(static_cast<size_t>(fileLen));
        if (file.Read(fileData.data(), static_cast<size_t>(fileLen)) != fileLen) {
            file.Close();
            wxMessageBox("Failed to read session file.", "Open Session", wxOK | wxICON_ERROR, this);
            return false;
        }
        file.Close();

        /* Validate header */
        if (memcmp(fileData.data(), kNbsMagic, 4) != 0) {
            wxMessageBox("Not a valid NeoBAE session file.", "Open Session", wxOK | wxICON_ERROR, this);
            return false;
        }
        fileVersion = ReadLE16(fileData.data() + 4);
        if (fileVersion > kNbsVersion) {
            wxMessageBox("Session file version is newer than this editor supports.", "Open Session", wxOK | wxICON_ERROR, this);
            return false;
        }
        uncompressedSize = ReadLE32(fileData.data() + 6);

        /* Decompress (v2 = LZMA, v1 = zlib) */
        payload.resize(uncompressedSize);
        if (fileVersion >= 2) {
            uint64_t memlimit = UINT64_MAX;
            size_t inPos = 0;
            size_t outPos = 0;
            if (lzma_stream_buffer_decode(&memlimit, 0, NULL,
                                          fileData.data() + 10, &inPos, fileData.size() - 10,
                                          payload.data(), &outPos, uncompressedSize) != LZMA_OK) {
                wxMessageBox("Failed to decompress session data (LZMA).", "Open Session", wxOK | wxICON_ERROR, this);
                return false;
            }
        } else {
            uLongf destLen = static_cast<uLongf>(uncompressedSize);
            if (uncompress(payload.data(), &destLen, fileData.data() + 10,
                           static_cast<uLong>(fileData.size() - 10)) != Z_OK) {
                wxMessageBox("Failed to decompress session data (zlib).", "Open Session", wxOK | wxICON_ERROR, this);
                return false;
            }
        }

        /* Parse TLV fields */
        offset = 0;
        while (offset + 6 <= payload.size()) {
            uint16_t fieldId = ReadLE16(payload.data() + offset);
            uint32_t fieldLen = ReadLE32(payload.data() + offset + 2);
            offset += 6;
            if (offset + fieldLen > payload.size()) {
                break;
            }

            if (fieldId == kNbsFieldRmfBlob) {
                rmfBlob.assign(payload.data() + offset, payload.data() + offset + fieldLen);
            } else if (fieldId == kNbsFieldSettings) {
                if (fieldLen >= 1) {
                    size_t copyLen = std::min(static_cast<size_t>(fieldLen), sizeof(NbsSessionSettings));
                    memcpy(&settings, payload.data() + offset, copyLen);
                    haveSettings = true;
                }
            } else if (fieldId == kNbsFieldUserEndTick) {
                if (fieldLen >= 4) {
                    userEndTick = ReadLE32(payload.data() + offset);
                }
            }
            /* Unknown fields are silently skipped */
            offset += fieldLen;
        }

        if (rmfBlob.empty()) {
            wxMessageBox("Session file contains no document data.", "Open Session", wxOK | wxICON_ERROR, this);
            return false;
        }

        /* Restore document */
        if (!RestoreSerializedDocument(rmfBlob)) {
            wxMessageBox("Failed to restore document from session.", "Open Session", wxOK | wxICON_ERROR, this);
            return false;
        }

        /* Apply settings */
        if (haveSettings) {
            if (m_previewVolumeSlider) {
                m_previewVolumeSlider->SetValue(std::clamp(settings.previewVolume, 0, 100));
            }
            SetSelectedPreviewReverbType(static_cast<BAEReverbType>(settings.previewReverbType));
            if (m_previewLoopCheck) {
                m_previewLoopCheck->SetValue(settings.previewLoopEnabled != 0);
            }
            if (m_midiStorageChoice) {
                m_midiStorageChoice->SetSelection(std::clamp(settings.midiStorageMode, 0, 3));
            }
            if (settings.settingsVersion >= 2) {
                if (m_settingsMenu) {
                    m_settingsMenu->Check(ID_SettingsPanFix, settings.panFix != 0);
                    m_settingsMenu->Check(ID_SettingsClassicChorus, settings.classicChorus != 0);
                }
                if (m_savePreviewToSongCheck) {
                    m_savePreviewToSongCheck->SetValue(settings.savePreviewToSong != 0);
                }
            } else {
                if (m_settingsMenu) {
                    m_settingsMenu->Check(ID_SettingsPanFix, true);
                    m_settingsMenu->Check(ID_SettingsClassicChorus, false);
                }
                if (m_savePreviewToSongCheck) {
                    m_savePreviewToSongCheck->SetValue(false);
                }
            }
            ApplyMixerEngineSettings();
        }

        /* Post-load UI refresh (mirrors LoadDocument) */
        {
            BAERmfEditorMidiStorageType storageType;
            if (BAERmfEditorDocument_GetMidiStorageType(m_document, &storageType) == BAE_NO_ERROR) {
                SetSelectedMidiStorageType(storageType);
            }
        }
        InvalidatePianoRollPreviewSong();
        ClearUndoHistory();
        m_currentPath = path;
        m_sessionPath = path;
        MarkDocumentClean();
        PianoRollPanel_SetDocument(m_pianoRoll, m_document);
        if (userEndTick > 0) {
            PianoRollPanel_SetUserEndTick(m_pianoRoll, userEndTick);
        }
        PianoRollPanel_ClearPlayhead(m_pianoRoll);
        m_ignoreSeekEvent = true;
        m_positionSlider->SetValue(0);
        m_ignoreSeekEvent = false;
        UpdatePositionLabelFromDocumentTick(0);
        StopPlayback(true);
        PopulateTrackList();
        PopulateSampleList();
        RefreshMidiLoopControlsFromDocument();

        /* Restore selected track from session */
        if (haveSettings && settings.selectedTrack >= 0 &&
            settings.selectedTrack < static_cast<int32_t>(m_trackList->GetCount())) {
            m_trackList->SetSelection(settings.selectedTrack);
            PianoRollPanel_SetSelectedTrack(m_pianoRoll, settings.selectedTrack);
            UpdateControlsFromSelection();
        }

        UpdateFrameTitle();
        SetStatusText(path, 1);
        return true;
    }

    void OnTrackSelected(wxCommandEvent &) {
        PianoRollPanel_SetSelectedTrack(m_pianoRoll, GetSelectedTrack());
        UpdateControlsFromSelection();
    }

    void OnTempoChanged(wxCommandEvent &) {
        if (!m_document || m_updatingControls) {
            return;
        }
        BeginUndoAction("Change Tempo");
        if (BAERmfEditorDocument_SetTempoBPM(m_document, static_cast<uint32_t>(m_tempoSpin->GetValue())) == BAE_NO_ERROR) {
            CommitUndoAction("Change Tempo");
            PianoRollPanel_RefreshFromDocument(m_pianoRoll, false);
        } else {
            CancelUndoAction();
        }
    }

    void OnTrackSettingsChanged(wxCommandEvent &event) {
        BAERmfEditorTrackInfo trackInfo;
        BAERmfEditorNoteInfo selectedNote;
        int selectedTrack;
        wxObject *source;
        bool bankProgramControl;
        bool defaultInstrumentUpdated;

        if (!m_document || m_updatingControls) {
            return;
        }
        source = event.GetEventObject();
        bankProgramControl = (source == m_bankSpin || source == m_programSpin);
        selectedTrack = GetSelectedTrack();
        if (selectedTrack < 0 || BAERmfEditorDocument_GetTrackInfo(m_document, static_cast<uint16_t>(selectedTrack), &trackInfo) != BAE_NO_ERROR) {
            return;
        }
        if (bankProgramControl && PianoRollPanel_GetSelectedNoteInfo(m_pianoRoll, &selectedNote)) {
            selectedNote.bank = InternalBankFromDisplay(static_cast<uint16_t>(m_bankSpin->GetValue()));
            selectedNote.program = static_cast<unsigned char>(m_programSpin->GetValue());
            PianoRollPanel_SetSelectedNoteInstrument(m_pianoRoll, selectedNote.bank, selectedNote.program);
            PianoRollPanel_SetNewNoteInstrument(m_pianoRoll, selectedNote.bank, selectedNote.program);
            InvalidatePianoRollPreviewSong();
            return;
        }
        defaultInstrumentUpdated = false;
        if (bankProgramControl) {
            uint16_t bank;
            unsigned char program;

            bank = InternalBankFromDisplay(static_cast<uint16_t>(m_bankSpin->GetValue()));
            program = static_cast<unsigned char>(m_programSpin->GetValue());
            if (BAERmfEditorDocument_SetTrackDefaultInstrument(m_document,
                                                               static_cast<uint16_t>(selectedTrack),
                                                               bank,
                                                               program) == BAE_NO_ERROR) {
                trackInfo.bank = bank;
                trackInfo.program = program;
                defaultInstrumentUpdated = true;
            }
        }
        trackInfo.transpose = static_cast<int16_t>(m_transposeSpin->GetValue());
        trackInfo.pan = static_cast<unsigned char>(m_panSpin->GetValue());
        trackInfo.volume = static_cast<unsigned char>(m_volumeSpin->GetValue());
        if (!bankProgramControl && BAERmfEditorDocument_SetTrackInfo(m_document, static_cast<uint16_t>(selectedTrack), &trackInfo) == BAE_NO_ERROR) {
            if (selectedTrack >= 0 && selectedTrack < static_cast<int>(m_trackList->GetCount())) {
                m_trackList->SetString(static_cast<unsigned>(selectedTrack), BuildTrackLabel(selectedTrack, trackInfo));
            }
            PianoRollPanel_SetNewNoteInstrument(m_pianoRoll, trackInfo.bank, trackInfo.program);
            PianoRollPanel_Refresh(m_pianoRoll);
        } else if (defaultInstrumentUpdated) {
            if (selectedTrack >= 0 && selectedTrack < static_cast<int>(m_trackList->GetCount())) {
                m_trackList->SetString(static_cast<unsigned>(selectedTrack), BuildTrackLabel(selectedTrack, trackInfo));
            }
            PianoRollPanel_SetNewNoteInstrument(m_pianoRoll, trackInfo.bank, trackInfo.program);
            PianoRollPanel_Refresh(m_pianoRoll);
        }
        if (bankProgramControl) {
            InvalidatePianoRollPreviewSong();
        }
    }

    void OnTrackContextMenu(wxContextMenuEvent &event) {
        wxMenu menu;
        bool hasTrackSelection;

        {
            wxPoint screenPos = event.GetPosition();
            if (screenPos != wxDefaultPosition) {
                wxPoint clientPos = m_trackList->ScreenToClient(screenPos);
                int hitIndex = m_trackList->HitTest(clientPos);
                if (hitIndex != wxNOT_FOUND && hitIndex != m_trackList->GetSelection()) {
                    m_trackList->SetSelection(hitIndex);
                    wxCommandEvent selEvent(wxEVT_LISTBOX, m_trackList->GetId());
                    OnTrackSelected(selEvent);
                }
            }
        }
        hasTrackSelection = GetSelectedTrack() >= 0;

        menu.Append(ID_TrackAdd, "Add Track");
        menu.Append(ID_TrackRename, "Rename Track...");
        menu.Append(ID_TrackDelete, "Delete Track");
        menu.Enable(ID_TrackDelete, hasTrackSelection);
        menu.Enable(ID_TrackRename, hasTrackSelection);
        PopupMenu(&menu);
    }

    void OnSampleContextMenu(wxContextMenuEvent &) {
        wxMenu menu;
        SampleTreeItemData *selectedData;
        bool hasDocument;
        bool hasMenuItemNewInst = false;

        selectedData = GetSelectedSampleTreeData();
        hasDocument = (m_document != nullptr);
        if (selectedData) {
            menu.Append(ID_InstrumentEdit, "Edit Instrument...");
            menu.AppendSeparator();
            if (selectedData->IsAssetNode()) {
                menu.Append(ID_CompressInstrument, "Compress Instrument");
                menu.Append(ID_SampleDelete, "Delete Sample Asset");
            } else {
                menu.Append(ID_CompressInstrument, "Compress Instrument");
                menu.Append(ID_SampleDelete, "Delete Instrument");
            }
            menu.AppendSeparator();
            menu.Append(ID_CompressAllInstruments, "Compress All Instruments");
            menu.Append(ID_DeleteAllInstruments, "Delete All Instruments");
        } else {
            menu.Append(ID_SampleNewInstrument, "Add Instrument");
            menu.AppendSeparator();
            menu.Append(ID_CompressAllInstruments, "Compress All Instruments");
            menu.Append(ID_DeleteAllInstruments, "Delete All Instruments");
            hasMenuItemNewInst = true;
        }
        if (hasMenuItemNewInst) {
            menu.Enable(ID_SampleNewInstrument, hasDocument);
        }
        menu.Enable(ID_CompressAllInstruments, hasDocument);
        menu.Enable(ID_DeleteAllInstruments, hasDocument);
        PopupMenu(&menu);
    }

    void OnTrackAdd(wxCommandEvent &) {
        BAERmfEditorTrackSetup setup;
        BAERmfEditorTrackInfo firstTrackInfo;
        uint16_t trackCount;
        uint16_t newTrackIndex;
        int defaultChannel;
        long channelValue;
        long programValue;

        if (!m_document) {
            return;
        }
        trackCount = 0;
        BAERmfEditorDocument_GetTrackCount(m_document, &trackCount);
        defaultChannel = 0;
        if (trackCount > 0 && BAERmfEditorDocument_GetTrackInfo(m_document, 0, &firstTrackInfo) == BAE_NO_ERROR) {
            defaultChannel = firstTrackInfo.channel;
        }
        channelValue = wxGetNumberFromUser("MIDI channel (1-16)", "Channel", "Add Track", defaultChannel + 1, 1, 16, this);
        if (channelValue < 0) {
            return;
        }
        programValue = wxGetNumberFromUser("Program (0-127)", "Program", "Add Track", 0, 0, 127, this);
        if (programValue < 0) {
            return;
        }
        setup.channel = static_cast<unsigned char>(channelValue - 1);
        setup.bank = 0;
        setup.program = static_cast<unsigned char>(programValue);
        wxString trackName = wxString::Format("Track %u", static_cast<unsigned>(trackCount + 1));
        wxScopedCharBuffer utf8 = trackName.utf8_str();
        setup.name = const_cast<char *>(utf8.data());
        if (BAERmfEditorDocument_AddTrack(m_document, &setup, &newTrackIndex) == BAE_NO_ERROR) {
            ClearUndoHistory();
            PopulateTrackList();
            m_trackList->SetSelection(newTrackIndex);
            PianoRollPanel_SetSelectedTrack(m_pianoRoll, newTrackIndex);
            UpdateControlsFromSelection();
        }
    }

    void OnTrackDelete(wxCommandEvent &) {
        int selectedTrack;

        if (!m_document) {
            return;
        }
        selectedTrack = GetSelectedTrack();
        if (selectedTrack < 0) {
            return;
        }
        if (wxMessageBox("Delete selected track?", "Confirm Delete", wxYES_NO | wxICON_QUESTION, this) != wxYES) {
            return;
        }
        if (BAERmfEditorDocument_DeleteTrack(m_document, static_cast<uint16_t>(selectedTrack)) == BAE_NO_ERROR) {
            ClearUndoHistory();
            PopulateTrackList();
        }
    }

    void OnTrackRename(wxCommandEvent &) {
        int selectedTrack;
        BAERmfEditorTrackInfo trackInfo;
        wxString currentName;
        wxTextEntryDialog dialog(this,
                                 "Enter a track name (leave blank to clear)",
                                 "Rename Track");

        if (!m_document) {
            return;
        }
        selectedTrack = GetSelectedTrack();
        if (selectedTrack < 0) {
            return;
        }
        if (BAERmfEditorDocument_GetTrackInfo(m_document,
                                              static_cast<uint16_t>(selectedTrack),
                                              &trackInfo) != BAE_NO_ERROR) {
            return;
        }

        currentName = wxString::FromUTF8(trackInfo.name && trackInfo.name[0] ? trackInfo.name : nullptr);
        dialog.SetValue(currentName);
        if (dialog.ShowModal() != wxID_OK) {
            return;
        }

        {
            wxString newName = dialog.GetValue();
            wxScopedCharBuffer utf8Name = newName.utf8_str();

            if (!newName.empty() && utf8Name.data() && utf8Name.data()[0] != '\0') {
                trackInfo.name = const_cast<char *>(utf8Name.data());
            } else {
                trackInfo.name = nullptr;
            }
            if (BAERmfEditorDocument_SetTrackInfo(m_document,
                                                  static_cast<uint16_t>(selectedTrack),
                                                  &trackInfo) == BAE_NO_ERROR) {
                if (selectedTrack >= 0 && selectedTrack < static_cast<int>(m_trackList->GetCount())) {
                    m_trackList->SetString(static_cast<unsigned>(selectedTrack),
                                           BuildTrackLabel(selectedTrack, trackInfo));
                }
                SetStatusText("Track renamed", 0);
            }
        }
    }

    void OnPlay(wxCommandEvent &) {
        wxScopedCharBuffer utf8CurrentPath;
        BAERmfEditorDocument *playDoc;
        bool singleTrackMode;
        bool channelsMode;
        int selectedTrack;
        BAEResult loadResult;
        std::vector<unsigned char> playbackBlob;
        BAELoadResult loadInfo;

        if (!m_document) {
            wxMessageBox("Nothing is loaded.", "Playback", wxOK | wxICON_INFORMATION, this);
            return;
        }
        if (!EnsurePlaybackEngine()) {
            wxMessageBox("Failed to initialize audio engine.", "Playback Error", wxOK | wxICON_ERROR, this);
            return;
        }
        BAERmfEditorDocument_DebugReportMidiRoundTripDiff(m_document);
        selectedTrack = GetSelectedTrack();
        singleTrackMode = (m_playScopeChoice->GetSelection() == 1);
        channelsMode = (m_playScopeChoice->GetSelection() == 2);
        fprintf(stderr,
            "[nbstudio] OnPlay scope=%d selectedTrack=%d channelMask=0x%04x currentPath='%s'\n",
            channelsMode ? 2 : (singleTrackMode ? 1 : 0),
            selectedTrack,
            static_cast<unsigned>(m_playbackChannelMask),
            m_currentPath.empty() ? "" : static_cast<char const *>(m_currentPath.utf8_str()));
        if (singleTrackMode && selectedTrack < 0) {
            uint16_t trackCount;

            trackCount = 0;
            if (BAERmfEditorDocument_GetTrackCount(m_document, &trackCount) == BAE_NO_ERROR && trackCount > 0) {
                selectedTrack = 0;
                if (m_trackList && m_trackList->GetCount() > 0) {
                    m_trackList->SetSelection(0);
                }
                PianoRollPanel_SetSelectedTrack(m_pianoRoll, 0);
            }
        }
        if (!m_currentPath.empty()) {
            utf8CurrentPath = m_currentPath.utf8_str();
        }
        // Use the same document-building path as OnSaveAs so preview and output are
        // serialized identically. The only difference is that the bytes are intercepted
        // from a temp file instead of written to a user-chosen path.
        bool ownPlayDoc = false;
        if (singleTrackMode) {
            playDoc = BuildSingleTrackPlaybackDocument(selectedTrack);
            ownPlayDoc = true;
            if (!playDoc) {
                wxMessageBox("Failed to build selected-track playback document.", "Playback Error", wxOK | wxICON_ERROR, this);
                return;
            }
        } else if (channelsMode) {
            if (m_playbackChannelMask == 0) {
                wxMessageBox("No playback channels are enabled.", "Playback", wxOK | wxICON_INFORMATION, this);
                return;
            }
            playDoc = BuildChannelPlaybackDocument(m_playbackChannelMask);
            ownPlayDoc = true;
            if (!playDoc) {
                wxMessageBox("Failed to build channel-filtered playback document.", "Playback Error", wxOK | wxICON_ERROR, this);
                return;
            }
        } else {
            playDoc = m_document;
        }
        if (!BuildPreviewPlaybackBlob(playDoc, &playbackBlob)) {
            if (ownPlayDoc) {
                BAERmfEditorDocument_Delete(playDoc);
            }
            wxMessageBox("Failed to build in-memory playback ZMF.", "Playback Error", wxOK | wxICON_ERROR, this);
            return;
        }
        if (ownPlayDoc) {
            BAERmfEditorDocument_Delete(playDoc);
        }
        StopPlayback(true);
        memset(&loadInfo, 0, sizeof(loadInfo));
        m_playbackSongBlob = std::move(playbackBlob);
        loadResult = BAEMixer_LoadFromMemory(m_playbackMixer,
                             m_playbackSongBlob.data(),
                             static_cast<uint32_t>(m_playbackSongBlob.size()),
                                             &loadInfo);
        fprintf(stderr,
                "[nbstudio] BAEMixer_LoadFromMemory result=%d type=%d fileType=%d\n",
                static_cast<int>(loadResult),
                static_cast<int>(loadInfo.type),
                static_cast<int>(loadInfo.fileType));
        if (loadResult != BAE_NO_ERROR || loadInfo.type != BAE_LOAD_TYPE_SONG || !loadInfo.data.song) {
            BAELoadResult_Cleanup(&loadInfo);
            m_playbackSongBlob.clear();
            wxMessageBox("Failed to load playback song from memory.", "Playback Error", wxOK | wxICON_ERROR, this);
            return;
        }
        m_playbackSong = loadInfo.data.song;
        loadInfo.type = BAE_LOAD_TYPE_NONE;
        loadInfo.data.song = nullptr;
        {
            BAEResult seekResult;
            BAEResult prerollResult;
            BAEResult loopResult;

            loopResult = BAESong_SetLoops(m_playbackSong, IsPreviewLoopEnabled() ? 32767 : 0);
            seekResult = BAESong_SetMicrosecondPosition(m_playbackSong, 0);
            prerollResult = BAESong_Preroll(m_playbackSong);
            fprintf(stderr,
                    "[nbstudio] Pre-start prep loops=%d seek0=%d preroll=%d\n",
                    static_cast<int>(loopResult),
                    static_cast<int>(seekResult),
                    static_cast<int>(prerollResult));
        }
        BAEResult startResult = BAESong_Start(m_playbackSong, 0);
        fprintf(stderr, "[nbstudio] BAESong_Start result=%d\n", static_cast<int>(startResult));
        if (startResult != BAE_NO_ERROR) {
            BAESong_Delete(m_playbackSong);
            m_playbackSong = nullptr;
            m_playbackSongBlob.clear();
            wxMessageBox("Failed to start playback.", "Playback Error", wxOK | wxICON_ERROR, this);
            return;
        }
        ApplyPreviewReverbToMixer();
        BAESong_SetVolume(m_playbackSong, GetPreviewVolumeFixed());
        {
            BAE_UNSIGNED_FIXED tempoFactor;
            BAEResult tempoSetResult;

            /* Ensure editor playback always starts at engine-normal 1.0x speed. */
            tempoSetResult = BAESong_SetMasterTempo(m_playbackSong, static_cast<BAE_UNSIGNED_FIXED>(65536));
            fprintf(stderr, "[nbstudio] BAESong_SetMasterTempo(1.0) result=%d\n", static_cast<int>(tempoSetResult));
            tempoFactor = 0;
            if (BAESong_GetMasterTempo(m_playbackSong, &tempoFactor) == BAE_NO_ERROR) {
                fprintf(stderr, "[nbstudio] Song master tempo factor=%lu\n", static_cast<unsigned long>(tempoFactor));
            }
        }
        m_playbackTimer.Start(40);
        m_ignoreSeekEvent = true;
        m_positionSlider->SetValue(0);
        m_ignoreSeekEvent = false;
        m_autoFollowPlayhead = true;
        PianoRollPanel_SetPlayheadTick(m_pianoRoll, 0);
        {
            uint32_t lenUsec;

            lenUsec = 0;
            BAESong_GetMicrosecondLength(m_playbackSong, &lenUsec);
            UpdatePositionLabel(0, lenUsec);
        }
        SetStatusText("Playing", 0);
    }

    void OnPauseResume(wxCommandEvent &) {
        BAE_BOOL isPaused;

        if (!m_playbackSong) {
            return;
        }
        isPaused = FALSE;
        BAESong_IsPaused(m_playbackSong, &isPaused);
        if (isPaused) {
            BAESong_Resume(m_playbackSong);
            SetStatusText("Playing", 0);
        } else {
            BAESong_Pause(m_playbackSong);
            SetStatusText("Paused", 0);
        }
    }

    void OnPreviewVolumeChanged(wxCommandEvent &) {
        if (m_playbackSong) {
            BAESong_SetVolume(m_playbackSong, GetPreviewVolumeFixed());
        }
        if (m_bankPreviewSong) {
            BAESong_SetVolume(m_bankPreviewSong, GetPreviewVolumeFixed());
        }
    }

    void OnPreviewReverbChanged(wxCommandEvent &) {
        ApplyPreviewReverbToMixer();
    }

    void OnPreviewLoopChanged(wxCommandEvent &) {
        ApplyPreviewLoopSettingToSong();
    }

    void RefreshPlaybackAtCurrentPosition() {
        uint32_t posUsec;
        BAE_BOOL wasPaused;

        if (!m_playbackSong || !m_playButton) {
            return;
        }
        posUsec = 0;
        BAESong_GetMicrosecondPosition(m_playbackSong, &posUsec);
        wasPaused = FALSE;
        BAESong_IsPaused(m_playbackSong, &wasPaused);

        m_pendingLoopHotReloadPosUsec = posUsec;
        m_pendingLoopHotReloadPaused = (wasPaused != FALSE);
        if (m_pendingLoopHotReload) {
            return;
        }
        m_pendingLoopHotReload = true;

        /* Marker loop changes require rebuilding/reloading song data; restarting the same
           BAESong instance does not re-parse updated marker metadata. Queue this to run
           after the current text/change event completes so it mirrors an actual Play click. */
        CallAfter([this]() {
            wxCommandEvent playEvent(wxEVT_BUTTON, m_playButton ? m_playButton->GetId() : wxID_ANY);
            uint32_t restorePosUsec = m_pendingLoopHotReloadPosUsec;
            bool restorePaused = m_pendingLoopHotReloadPaused;
            XBOOL loopEnabled;
            uint32_t loopStartTick;
            uint32_t loopEndTick;
            int32_t loopCount;

            m_pendingLoopHotReload = false;
            OnPlay(playEvent);
            if (!m_playbackSong) {
                return;
            }
            loopEnabled = FALSE;
            loopStartTick = 0;
            loopEndTick = 0;
            loopCount = -1;
            if (BAERmfEditorDocument_GetMidiLoopMarkers(m_document,
                                                        &loopEnabled,
                                                        &loopStartTick,
                                                        &loopEndTick,
                                                        &loopCount) == BAE_NO_ERROR &&
                loopEnabled && loopEndTick > loopStartTick) {
                uint32_t loopStartUsec;

                loopStartUsec = static_cast<uint32_t>(TicksToMicroseconds(loopStartTick));
                if (restorePosUsec > loopStartUsec) {
                    /* Ensure transport passes loopstart after reload so marker-based
                       looping is armed before we continue playback. */
                    restorePosUsec = loopStartUsec;
                }
            }
            BAESong_SetMicrosecondPosition(m_playbackSong, restorePosUsec);
            if (restorePaused) {
                BAESong_Pause(m_playbackSong);
            }
        });
    }

    void OnMidiLoopMarkersChanged(wxCommandEvent &) {
        XBOOL enabled;
        uint32_t startTick;
        uint32_t endTick;
        uint64_t startUsec;
        uint64_t endUsec;

        if (m_loadingLoopControls || !m_document) {
            return;
        }
        enabled = (m_midiLoopEnableCheck && m_midiLoopEnableCheck->GetValue()) ? TRUE : FALSE;

        if (enabled) {
            if (!m_midiLoopStartText || !m_midiLoopEndText) {
                return;
            }
            if (!ParseLoopTimeToUsec(m_midiLoopStartText->GetValue(), &startUsec) ||
                !ParseLoopTimeToUsec(m_midiLoopEndText->GetValue(), &endUsec)) {
                return;
            }
            startTick = MicrosecondsToTicks(static_cast<uint32_t>(std::min<uint64_t>(startUsec, 0xFFFFFFFFULL)));
            endTick = MicrosecondsToTicks(static_cast<uint32_t>(std::min<uint64_t>(endUsec, 0xFFFFFFFFULL)));
        } else {
            startTick = 0;
            endTick = 0;
        }

        if (enabled && endTick <= startTick) {
            endTick = startTick + 1;
            if (m_midiLoopEndText) {
                m_loadingLoopControls = true;
                m_midiLoopEndText->SetValue(FormatLoopTimeUsec(TicksToMicroseconds(endTick)));
                m_loadingLoopControls = false;
            }
        }

        if (BAERmfEditorDocument_SetMidiLoopMarkers(m_document,
                                                    enabled,
                                                    startTick,
                                                    endTick,
                                                    -1) == BAE_NO_ERROR) {
            SyncPianoRollMidiLoopMarkersFromDocument();
            InvalidatePianoRollPreviewSong();
            RefreshPlaybackAtCurrentPosition();
        }

        if (m_midiLoopStartText) m_midiLoopStartText->Enable(enabled != FALSE);
        if (m_midiLoopEndText) m_midiLoopEndText->Enable(enabled != FALSE);
    }

    void OnStop(wxCommandEvent &) {
        StopPlayback(true);
        m_ignoreSeekEvent = true;
        m_positionSlider->SetValue(0);
        m_ignoreSeekEvent = false;
        PianoRollPanel_ClearPlayhead(m_pianoRoll);
        UpdatePositionLabelFromDocumentTick(0);
        SetStatusText("Stopped", 0);
    }

    void OnPlaybackTimer(wxTimerEvent &) {
        BAE_BOOL isDone;

        if (!m_playbackSong) {
            m_playbackTimer.Stop();
            return;
        }
        {
            uint32_t posUsec;
            uint32_t lenUsec;

            posUsec = 0;
            lenUsec = 0;
            if (BAESong_GetMicrosecondPosition(m_playbackSong, &posUsec) == BAE_NO_ERROR &&
                BAESong_GetMicrosecondLength(m_playbackSong, &lenUsec) == BAE_NO_ERROR &&
                lenUsec > 0) {
                int sliderPos;

                sliderPos = static_cast<int>((static_cast<uint64_t>(posUsec) * 1000ULL) / static_cast<uint64_t>(lenUsec));
                m_ignoreSeekEvent = true;
                m_positionSlider->SetValue(std::clamp(sliderPos, 0, 1000));
                m_ignoreSeekEvent = false;
                UpdatePositionLabel(posUsec, lenUsec);
                {
                    uint32_t playheadTick;

                    playheadTick = MicrosecondsToTicks(posUsec);
                    PianoRollPanel_SetPlayheadTick(m_pianoRoll, playheadTick);
                    if (m_autoFollowPlayhead) {
                        PianoRollPanel_EnsurePlayheadVisible(m_pianoRoll, playheadTick);
                    }
                }
            }
        }
        isDone = FALSE;
        if (BAESong_IsDone(m_playbackSong, &isDone) == BAE_NO_ERROR && isDone) {
            StopPlayback(true);
            m_ignoreSeekEvent = true;
            m_positionSlider->SetValue(1000);
            m_ignoreSeekEvent = false;
            UpdatePositionLabelFromDocumentTick(PianoRollPanel_GetDocumentEndTick(m_pianoRoll));
            SetStatusText("Playback complete", 0);
        }
    }

    void OnSeekSlider(wxCommandEvent &) {
        uint64_t lenUsec;
        uint64_t targetUsec;
        uint32_t seekTick;

        if (m_ignoreSeekEvent) {
            return;
        }
        m_autoFollowPlayhead = false;
        seekTick = 0;
        if (m_playbackSong) {
            uint32_t lenUsec32;

            lenUsec = 0;
            lenUsec32 = 0;
            if (BAESong_GetMicrosecondLength(m_playbackSong, &lenUsec32) == BAE_NO_ERROR && lenUsec32 > 0) {
                lenUsec = lenUsec32;
                targetUsec = (lenUsec * static_cast<uint64_t>(m_positionSlider->GetValue())) / 1000ULL;
                BAESong_SetMicrosecondPosition(m_playbackSong, static_cast<uint32_t>(targetUsec));
                seekTick = MicrosecondsToTicks(static_cast<uint32_t>(targetUsec));
                UpdatePositionLabel(targetUsec, lenUsec);
            }
        } else {
            uint32_t endTick;

            endTick = PianoRollPanel_GetDocumentEndTick(m_pianoRoll);
            lenUsec = TicksToMicroseconds(endTick);
            targetUsec = (lenUsec * static_cast<uint64_t>(m_positionSlider->GetValue())) / 1000ULL;
            seekTick = MicrosecondsToTicks(static_cast<uint32_t>(targetUsec));
            UpdatePositionLabel(targetUsec, lenUsec);
        }
        PianoRollPanel_SetPlayheadTick(m_pianoRoll, seekTick);
        PianoRollPanel_JumpToTick(m_pianoRoll, seekTick);
    }

    void SeekToTickFromPianoRoll(uint32_t tick) {
        uint32_t endTick;
        uint32_t playheadTick;
        uint64_t totalUsec;
        uint64_t targetUsec;
        int sliderPos;

        if (m_ignoreSeekEvent || !m_document) {
            return;
        }
        endTick = PianoRollPanel_GetDocumentEndTick(m_pianoRoll);
        if (endTick == 0) {
            return;
        }
        tick = std::min(tick, endTick);
        playheadTick = tick;
        totalUsec = TicksToMicroseconds(endTick);
        targetUsec = TicksToMicroseconds(tick);
        if (totalUsec == 0) {
            totalUsec = 1;
        }
        if (m_playbackSong) {
            uint32_t lenUsec;

            lenUsec = 0;
            if (BAESong_GetMicrosecondLength(m_playbackSong, &lenUsec) == BAE_NO_ERROR && lenUsec > 0) {
                if (targetUsec > lenUsec) {
                    targetUsec = lenUsec;
                }
                sliderPos = static_cast<int>((targetUsec * 1000ULL) / static_cast<uint64_t>(lenUsec));
                sliderPos = std::clamp(sliderPos, 0, 1000);
                m_autoFollowPlayhead = false;
                m_ignoreSeekEvent = true;
                m_positionSlider->SetValue(sliderPos);
                m_ignoreSeekEvent = false;
                BAESong_SetMicrosecondPosition(m_playbackSong, static_cast<uint32_t>(targetUsec));
                playheadTick = MicrosecondsToTicks(static_cast<uint32_t>(targetUsec));
                UpdatePositionLabel(targetUsec, lenUsec);
            }
        } else {
            sliderPos = static_cast<int>((targetUsec * 1000ULL) / totalUsec);
            sliderPos = std::clamp(sliderPos, 0, 1000);
            m_autoFollowPlayhead = false;
            m_ignoreSeekEvent = true;
            m_positionSlider->SetValue(sliderPos);
            m_ignoreSeekEvent = false;
            UpdatePositionLabel(targetUsec, totalUsec);
        }
        PianoRollPanel_SetPlayheadTick(m_pianoRoll, playheadTick);
    }

    void OnMetadata(wxCommandEvent &) {
        if (!m_document) {
            wxMessageBox("Nothing is loaded.", "Metadata", wxOK | wxICON_INFORMATION, this);
            return;
        }
        ShowMetadataDialog(this, m_document);
    }

    void OnLoadBank(wxCommandEvent &) {
        if (!ConfirmDiscardBankChanges()) {
            return;
        }
        wxFileDialog dialog(this,
                            "Load Sound Bank",
                            wxEmptyString,
                            wxEmptyString,
                            "Sound banks (*.hsb;*.zsb)|*.hsb;*.zsb|HSB banks (*.hsb)|*.hsb|ZSB banks (*.zsb)|*.zsb|All files (*.*)|*.*",
                            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK) {
            return;
        }
        /* Load into both the mixer and the bank editor.
         * Stay on the current tab (don't force-switch to bank editor). */
        LoadBankForEditing(dialog.GetPath(), false);
    }

    void OnReloadInternalBank(wxCommandEvent &) {
        if (!m_playbackMixer) {
            return;
        }
        if (!ConfirmDiscardBankChanges()) {
            return;
        }
        StopPlayback(true);
        BAEMixer_UnloadBanks(m_playbackMixer);
        m_bankToken = nullptr;
        m_bankLoaded = false;
        m_bankTokens.clear();
        m_loadedBankPath.clear();
#ifdef _BUILT_IN_PATCHES
        {
            BAEResult bankResult = BAEMixer_LoadBuiltinBank(m_playbackMixer, &m_bankToken);
            if (bankResult == BAE_NO_ERROR) {
                m_bankLoaded = true;
                m_bankTokens.push_back(m_bankToken);
                SetStatusText("Internal bank reloaded", 0);
            } else {
                SetStatusText("Failed to reload internal bank", 0);
            }
        }
        /* Reload the built-in bank into the bank editor too */
        if (m_bankEditorPanel && m_bankLoaded && m_bankToken) {
            BankEditorPanel_LoadBank(m_bankEditorPanel, m_bankToken, "(built-in)");
        }
#else
        SetStatusText("All banks unloaded", 0);
        if (m_bankEditorPanel) {
            BankEditorPanel_Clear(m_bankEditorPanel);
        }
#endif
        m_bankHasUnsavedChanges = false;
        UpdateLoadedBankStatus();
    }

    void OnCloneFromBank(wxCommandEvent &) {
        if (!m_document) {
            wxMessageBox("Open or create a document first.", "Clone from Bank", wxOK | wxICON_INFORMATION, this);
            return;
        }
        /* Ensure engine + built-in bank are loaded */
        if (!EnsurePlaybackEngine()) {
            wxMessageBox("Failed to initialize playback engine.", "Clone from Bank", wxOK | wxICON_ERROR, this);
            return;
        }
        if (m_bankTokens.empty()) {
            wxMessageBox("No banks loaded.", "Clone from Bank", wxOK | wxICON_INFORMATION, this);
            return;
        }

        /* Enumerate instruments from ALL loaded banks */
        struct BankInstEntry {
            BAEBankToken token;
            uint32_t bankIndex;     /* index within the bank */
        };
        wxArrayString choices;
        std::vector<BankInstEntry> entries;

        for (size_t bk = 0; bk < m_bankTokens.size(); ++bk) {
            BAEBankToken token = m_bankTokens[bk];
            uint32_t instCount = 0;
            if (BAERmfEditorBank_GetInstrumentCount(token, &instCount) != BAE_NO_ERROR) {
                continue;
            }
            /* Get bank friendly name for display */
            char friendlyBuf[128];
            wxString bankLabel;
            if (BAE_GetBankFriendlyName(m_playbackMixer, token, friendlyBuf, sizeof(friendlyBuf)) == BAE_NO_ERROR) {
                bankLabel = wxString::FromUTF8(friendlyBuf);
            } else if (m_bankTokens.size() > 1) {
                bankLabel = wxString::Format("Bank %u", static_cast<unsigned>(bk + 1));
            }

            for (uint32_t i = 0; i < instCount; ++i) {
                BAERmfEditorBankInstrumentInfo info;
                if (BAERmfEditorBank_GetInstrumentInfo(token, i, &info) != BAE_NO_ERROR) {
                    continue;
                }
                wxString instLabel;
                if (info.name[0]) {
                    instLabel = wxString::Format("P%u B%u: %s (%d splits)",
                                                 static_cast<unsigned>(info.program),
                                                 static_cast<unsigned>(info.bank),
                                                 wxString::FromUTF8(info.name),
                                                 static_cast<int>(info.keySplitCount));
                } else {
                    instLabel = wxString::Format("P%u B%u: (unnamed) (%d splits)",
                                                 static_cast<unsigned>(info.program),
                                                 static_cast<unsigned>(info.bank),
                                                 static_cast<int>(info.keySplitCount));
                }
                if (!bankLabel.empty()) {
                    instLabel = wxString::Format("[%s] %s", bankLabel, instLabel);
                }
                choices.Add(instLabel);
                entries.push_back({token, i});
            }
        }
        if (choices.IsEmpty()) {
            wxMessageBox("No instruments found in loaded banks.", "Clone from Bank", wxOK | wxICON_INFORMATION, this);
            return;
        }

        /* Show instrument picker */
        wxSingleChoiceDialog pickDlg(this,
                                     "Select an instrument to clone from the bank:",
                                     "Clone Instrument from Bank",
                                     choices);
        if (pickDlg.ShowModal() != wxID_OK) {
            return;
        }
        int selection = pickDlg.GetSelection();
        if (selection < 0 || static_cast<size_t>(selection) >= entries.size()) {
            return;
        }
        BankInstEntry const &chosen = entries[static_cast<size_t>(selection)];
        BAEBankToken chosenBank = chosen.token;
        uint32_t bankInstIndex = chosen.bankIndex;

        /* Find first unused program number */
        bool usedPrograms[128];
        long defaultProgram = 0;
        memset(usedPrograms, 0, sizeof(usedPrograms));
        {
            uint32_t sampleCount = 0;
            BAERmfEditorDocument_GetSampleCount(m_document, &sampleCount);
            for (uint32_t i = 0; i < sampleCount; ++i) {
                BAERmfEditorSampleInfo sInfo;
                if (BAERmfEditorDocument_GetSampleInfo(m_document, i, &sInfo) == BAE_NO_ERROR &&
                    sInfo.program < 128) {
                    usedPrograms[sInfo.program] = true;
                }
            }
        }
        for (int i = 0; i < 128; ++i) {
            if (!usedPrograms[i]) {
                defaultProgram = i;
                break;
            }
        }

        long targetProgram = wxGetNumberFromUser(
            "MIDI program number for cloned instrument (0-127):",
            "Program",
            "Clone Instrument from Bank",
            defaultProgram,
            0,
            127,
            this);
        if (targetProgram < 0) {
            return;
        }

        /* Perform clone */
        BeginUndoAction("Clone Instrument from Bank");
        BAEResult result = BAERmfEditorDocument_CloneInstrumentFromBank(
            m_document,
            chosenBank,
            bankInstIndex,
            static_cast<unsigned char>(targetProgram));
        if (result != BAE_NO_ERROR) {
            CancelUndoAction();
            wxMessageBox("Failed to clone instrument from bank.", "Clone from Bank", wxOK | wxICON_ERROR, this);
            return;
        }
        CommitUndoAction("Clone Instrument from Bank");

        PopulateSampleList();
        StopKeyboardPreview();
        /* Select the last added sample */
        {
            uint32_t sampleCount = 0;
            if (BAERmfEditorDocument_GetSampleCount(m_document, &sampleCount) == BAE_NO_ERROR && sampleCount > 0) {
                SelectTreeItemForSample(sampleCount - 1);
            }
        }
        SetStatusText(wxString::Format("Cloned instrument to program %ld", targetProgram));
    }

    void OnCloneAllUsedFromBank(wxCommandEvent &) {
          /* Editor stores banks as 14-bit values (MSB<<7 | LSB). We want MIDI bank MSB=2,
              so internal target bank must be 2<<7. */
          static const uint16_t kClonedInstrumentBank = (2u << 7);

        struct SourcePair {
            uint16_t bank;
            unsigned char program;
        };
        struct ClonePlan {
            uint16_t sourceBank;
            unsigned char sourceProgram;
            uint32_t instrumentIndex;
            unsigned char targetProgram;
        };
        struct PercClonePlan {
            unsigned char note;            /* MIDI note number on channel 9 */
            uint32_t instrumentIndex;      /* index of the source INST in the bank */
            uint32_t targetInstID;         /* 640 + note */
        };

        std::unordered_map<uint32_t, SourcePair> usedPairs;
        std::unordered_map<uint32_t, uint32_t> bankInstByPair;
        std::unordered_map<uint32_t, uint32_t> instIdToIdx; /* INST ID -> bank instrument index */
        std::vector<ClonePlan> plans;
        std::vector<PercClonePlan> percPlans;
        std::set<unsigned char> usedPercNotes;             /* unique note numbers on channel 9 */
        std::set<uint32_t> percSourceBankProgKeys;         /* bank/program keys from channel 9 tracks */
        uint16_t trackCount;
        char friendlyBuf[128];
        wxString bankLabel;
        uint32_t availableInstCount;
        uint32_t nextProgram;

        if (!m_document) {
            wxMessageBox("Open or create a document first.", "Clone All Used Instruments", wxOK | wxICON_INFORMATION, this);
            return;
        }
        if (!EnsurePlaybackEngine()) {
            wxMessageBox("Failed to initialize playback engine.", "Clone All Used Instruments", wxOK | wxICON_ERROR, this);
            return;
        }
        if (!m_bankToken) {
            wxMessageBox("No active bank loaded. Load a bank first.", "Clone All Used Instruments", wxOK | wxICON_INFORMATION, this);
            return;
        }

        availableInstCount = 0;
        if (BAERmfEditorBank_GetInstrumentCount(m_bankToken, &availableInstCount) != BAE_NO_ERROR || availableInstCount == 0) {
            wxMessageBox("No instruments found in the active bank.", "Clone All Used Instruments", wxOK | wxICON_INFORMATION, this);
            return;
        }

        if (BAE_GetBankFriendlyName(m_playbackMixer, m_bankToken, friendlyBuf, sizeof(friendlyBuf)) == BAE_NO_ERROR) {
            bankLabel = wxString::FromUTF8(friendlyBuf);
        }

        /* Build lookup tables from bank instruments. */
        for (uint32_t i = 0; i < availableInstCount; ++i) {
            BAERmfEditorBankInstrumentInfo info;
            uint32_t key;

            if (BAERmfEditorBank_GetInstrumentInfo(m_bankToken, i, &info) != BAE_NO_ERROR) {
                continue;
            }
            key = (static_cast<uint32_t>(info.bank) << 8) | static_cast<uint32_t>(info.program);
            if (bankInstByPair.find(key) == bankInstByPair.end()) {
                bankInstByPair[key] = i;
            }
            if (instIdToIdx.find(info.instID) == instIdToIdx.end()) {
                instIdToIdx[info.instID] = i;
            }
        }

        /* Gather pitched bank/program pairs and percussion notes from the MIDI data. */
        trackCount = 0;
        BAERmfEditorDocument_GetTrackCount(m_document, &trackCount);
        for (uint16_t trackIndex = 0; trackIndex < trackCount; ++trackIndex) {
            BAERmfEditorTrackInfo trackInfo;
            uint32_t noteCount;
            bool isPercTrack = false;

            if (BAERmfEditorDocument_GetTrackInfo(m_document, trackIndex, &trackInfo) == BAE_NO_ERROR) {
                isPercTrack = (trackInfo.channel == 9);
                uint32_t key = (static_cast<uint32_t>(trackInfo.bank) << 8) | static_cast<uint32_t>(trackInfo.program);
                if (isPercTrack) {
                    percSourceBankProgKeys.insert(key);
                } else {
                    usedPairs.emplace(key, SourcePair{trackInfo.bank, trackInfo.program});
                }
            }

            noteCount = 0;
            if (BAERmfEditorDocument_GetNoteCount(m_document, trackIndex, &noteCount) != BAE_NO_ERROR) {
                continue;
            }
            for (uint32_t noteIndex = 0; noteIndex < noteCount; ++noteIndex) {
                BAERmfEditorNoteInfo noteInfo;

                if (BAERmfEditorDocument_GetNoteInfo(m_document, trackIndex, noteIndex, &noteInfo) != BAE_NO_ERROR) {
                    continue;
                }
                if (noteInfo.channel == 9) {
                    usedPercNotes.insert(noteInfo.note);
                    uint32_t key = (static_cast<uint32_t>(noteInfo.bank) << 8) | static_cast<uint32_t>(noteInfo.program);
                    percSourceBankProgKeys.insert(key);
                } else {
                    uint32_t key = (static_cast<uint32_t>(noteInfo.bank) << 8) | static_cast<uint32_t>(noteInfo.program);
                    usedPairs.emplace(key, SourcePair{noteInfo.bank, noteInfo.program});
                }
            }
        }

        if (usedPairs.empty() && usedPercNotes.empty()) {
            wxMessageBox("No instrument references were found in the current document.", "Clone All Used Instruments", wxOK | wxICON_INFORMATION, this);
            return;
        }

        /* ---- Build pitched clone plans (with alias resolution) ---- */
        nextProgram = 0;
        {
            std::vector<uint32_t> sortedKeys;
            sortedKeys.reserve(usedPairs.size());
            for (auto const &entry : usedPairs) {
                sortedKeys.push_back(entry.first);
            }
            std::sort(sortedKeys.begin(), sortedKeys.end());

            for (uint32_t key : sortedKeys) {
                auto bankIt = bankInstByPair.find(key);
                if (bankIt == bankInstByPair.end()) {
                    /* Direct lookup failed — try alias resolution.
                     * Compute the expected INST ID for this bank/program pair.
                     * Editor internal bank = (MSB<<7)|LSB. For pitched channels:
                     * internalBankIndex = MSB * 2; instID = internalBankIndex * 128 + program. */
                    SourcePair const &sp = usedPairs[key];
                    uint16_t midiMSB = (sp.bank >> 7) & 0x7F;
                    uint32_t expectedInstID = static_cast<uint32_t>(midiMSB) * 256u + static_cast<uint32_t>(sp.program);
                    uint32_t resolvedInstID = 0;
                    uint32_t resolvedIdx = 0;
                    if (BAERmfEditorBank_ResolveInstID(m_bankToken, expectedInstID,
                                                       &resolvedInstID, &resolvedIdx) == BAE_NO_ERROR) {
                        bankIt = instIdToIdx.find(resolvedInstID);
                        if (bankIt == instIdToIdx.end()) {
                            continue; /* alias target not found in our index */
                        }
                        /* Use the resolved index. bankIt now points to the real INST. */
                    } else {
                        continue; /* no match, no alias */
                    }
                }
                if (nextProgram >= 128) {
                    wxMessageBox("Clone-all supports up to 128 deterministic pitched slots (INST 512-639).",
                                 "Clone All Used Instruments",
                                 wxOK | wxICON_ERROR,
                                 this);
                    return;
                }

                plans.push_back(ClonePlan{
                    usedPairs[key].bank,
                    usedPairs[key].program,
                    bankIt->second,
                    static_cast<unsigned char>(nextProgram)
                });
                ++nextProgram;
            }
        }

        /* ---- Build percussion clone plans ---- */
        for (unsigned char percNote : usedPercNotes) {
            /* For GM Default mode on bank 0: percussion INST ID = 128 + note.
             * For higher banks: (MSB*2+1)*128 + note. We assume bank 0 for now
             * since that's the standard case; the bank select for channel 9 is
             * typically 0. */
            uint32_t percInstID = 128u + static_cast<uint32_t>(percNote);
            uint32_t resolvedInstID = percInstID;
            uint32_t bankIdx = 0;

            /* Direct lookup first. */
            auto directIt = instIdToIdx.find(percInstID);
            if (directIt != instIdToIdx.end()) {
                bankIdx = directIt->second;
            } else {
                /* Try alias resolution. */
                if (BAERmfEditorBank_ResolveInstID(m_bankToken, percInstID,
                                                   &resolvedInstID, &bankIdx) != BAE_NO_ERROR) {
                    continue; /* percussion instrument not available in bank */
                }
            }
            /* Target: user percussion bank (bank index 5) = 640 + note. */
            percPlans.push_back(PercClonePlan{
                percNote,
                bankIdx,
                640u + static_cast<uint32_t>(percNote)
            });
        }

        if (plans.empty() && percPlans.empty()) {
            wxString msg = "No used bank/program pairs matched instruments in the active bank.";
            if (!bankLabel.empty()) {
                msg += "\nActive bank: ";
                msg += bankLabel;
            }
            wxMessageBox(msg, "Clone All Used Instruments", wxOK | wxICON_INFORMATION, this);
            return;
        }

        {
            wxString confirmMsg = wxString::Format("Clone and remap %u pitched + %u percussion instrument(s)",
                                                   static_cast<unsigned>(plans.size()),
                                                   static_cast<unsigned>(percPlans.size()));
            if (!bankLabel.empty()) {
                confirmMsg += wxString::Format(" from bank '%s'", bankLabel);
            }
            confirmMsg += "?";
            if (wxMessageBox(confirmMsg,
                             "Clone All Used Instruments",
                             wxYES_NO | wxICON_QUESTION,
                             this) != wxYES) {
                return;
            }
        }

        BeginUndoAction("Clone All Used Instruments");

        /* ---- Execute pitched clones and remaps ---- */
        for (ClonePlan const &plan : plans) {
            BAEResult cloneResult;
            BAEResult remapResult;

            cloneResult = BAERmfEditorDocument_CloneInstrumentFromBank(m_document,
                                                                        m_bankToken,
                                                                        plan.instrumentIndex,
                                                                        plan.targetProgram);
            if (cloneResult != BAE_NO_ERROR) {
                CancelUndoAction();
                wxMessageBox(wxString::Format("Failed to clone pitched source %u:%u from bank.",
                                              static_cast<unsigned>(plan.sourceBank),
                                              static_cast<unsigned>(plan.sourceProgram)),
                             "Clone All Used Instruments",
                             wxOK | wxICON_ERROR,
                             this);
                return;
            }

            remapResult = BAERmfEditorDocument_RemapInstrumentReferences(m_document,
                                                                          plan.sourceBank,
                                                                          plan.sourceProgram,
                                                                          kClonedInstrumentBank,
                                                                          plan.targetProgram);
            if (remapResult != BAE_NO_ERROR) {
                CancelUndoAction();
                wxMessageBox(wxString::Format("Failed to remap source %u:%u to %u:%u.",
                                              static_cast<unsigned>(plan.sourceBank),
                                              static_cast<unsigned>(plan.sourceProgram),
                                              static_cast<unsigned>(DisplayBankFromInternal(kClonedInstrumentBank)),
                                              static_cast<unsigned>(plan.targetProgram)),
                             "Clone All Used Instruments",
                             wxOK | wxICON_ERROR,
                             this);
                return;
            }
        }

        /* ---- Execute percussion clones ---- */
        for (PercClonePlan const &pp : percPlans) {
            BAEResult cloneResult;

            cloneResult = BAERmfEditorDocument_CloneInstrumentFromBankToInstID(
                m_document,
                m_bankToken,
                pp.instrumentIndex,
                pp.targetInstID,
                pp.note);
            if (cloneResult != BAE_NO_ERROR) {
#if _DEBUG                
                printf("[CloneAll] Failed to clone percussion note %d (INST %u -> %u)\n",
                           (int)pp.note, 128u + (unsigned)pp.note, (unsigned)pp.targetInstID);
                /* Non-fatal: skip this drum sound and continue. */
#endif
            }
        }

        /* ---- Remap percussion track banks ----
         * For any channel-9 bank/program pairs that weren't already remapped by
         * the pitched phase, remap the bank to kClonedInstrumentBank (keep program). */
        for (uint32_t percKey : percSourceBankProgKeys) {
            uint16_t percBank = static_cast<uint16_t>((percKey >> 8) & 0xFFFF);
            unsigned char percProg = static_cast<unsigned char>(percKey & 0xFF);
            if (percBank == kClonedInstrumentBank) {
                continue; /* already in the target bank */
            }
            /* Check if this pair was already remapped by the pitched phase. */
            bool alreadyRemapped = false;
            for (ClonePlan const &plan : plans) {
                if (plan.sourceBank == percBank && plan.sourceProgram == percProg) {
                    alreadyRemapped = true;
                    break;
                }
            }
            if (!alreadyRemapped) {
                BAERmfEditorDocument_RemapInstrumentReferences(m_document,
                                                               percBank,
                                                               percProg,
                                                               kClonedInstrumentBank,
                                                               percProg);
            }
        }

        CommitUndoAction("Clone All Used Instruments");

        PopulateSampleList();
        StopKeyboardPreview();
        PianoRollPanel_RefreshFromDocument(m_pianoRoll, false);
        InvalidatePianoRollPreviewSong();
        unsigned totalCloned = static_cast<unsigned>(plans.size() + percPlans.size());
        SetStatusText(wxString::Format("Cloned and remapped %u instrument(s) (%u pitched, %u percussion)",
                                       totalCloned,
                                       static_cast<unsigned>(plans.size()),
                                       static_cast<unsigned>(percPlans.size())), 0);
    }

    void OnAliasFromBank(wxCommandEvent &) {
        if (!m_document) {
            wxMessageBox("Open or create a document first.", "Alias from Bank", wxOK | wxICON_INFORMATION, this);
            return;
        }
        if (!EnsurePlaybackEngine()) {
            wxMessageBox("Failed to initialize playback engine.", "Alias from Bank", wxOK | wxICON_ERROR, this);
            return;
        }
        if (m_bankTokens.empty()) {
            wxMessageBox("No banks loaded.", "Alias from Bank", wxOK | wxICON_INFORMATION, this);
            return;
        }

        struct BankInstEntry {
            BAEBankToken token;
            uint32_t bankIndex;
        };
        wxArrayString choices;
        std::vector<BankInstEntry> entries;

        for (size_t bk = 0; bk < m_bankTokens.size(); ++bk) {
            BAEBankToken token = m_bankTokens[bk];
            uint32_t instCount = 0;
            if (BAERmfEditorBank_GetInstrumentCount(token, &instCount) != BAE_NO_ERROR) {
                continue;
            }
            char friendlyBuf[128];
            wxString bankLabel;
            if (BAE_GetBankFriendlyName(m_playbackMixer, token, friendlyBuf, sizeof(friendlyBuf)) == BAE_NO_ERROR) {
                bankLabel = wxString::FromUTF8(friendlyBuf);
            } else if (m_bankTokens.size() > 1) {
                bankLabel = wxString::Format("Bank %u", static_cast<unsigned>(bk + 1));
            }

            for (uint32_t i = 0; i < instCount; ++i) {
                BAERmfEditorBankInstrumentInfo info;
                if (BAERmfEditorBank_GetInstrumentInfo(token, i, &info) != BAE_NO_ERROR) {
                    continue;
                }
                wxString instLabel;
                if (info.name[0]) {
                    instLabel = wxString::Format("P%u B%u: %s (%d splits)",
                                                 static_cast<unsigned>(info.program),
                                                 static_cast<unsigned>(info.bank),
                                                 wxString::FromUTF8(info.name),
                                                 static_cast<int>(info.keySplitCount));
                } else {
                    instLabel = wxString::Format("P%u B%u: (unnamed) (%d splits)",
                                                 static_cast<unsigned>(info.program),
                                                 static_cast<unsigned>(info.bank),
                                                 static_cast<int>(info.keySplitCount));
                }
                if (!bankLabel.empty()) {
                    instLabel = wxString::Format("[%s] %s", bankLabel, instLabel);
                }
                choices.Add(instLabel);
                entries.push_back({token, i});
            }
        }
        if (choices.IsEmpty()) {
            wxMessageBox("No instruments found in loaded banks.", "Alias from Bank", wxOK | wxICON_INFORMATION, this);
            return;
        }

        wxSingleChoiceDialog pickDlg(this,
                                     "Select an instrument to alias from the bank:\n"
                                     "(Samples will reference the bank, not be embedded)",
                                     "Alias Instrument from Bank",
                                     choices);
        if (pickDlg.ShowModal() != wxID_OK) {
            return;
        }
        int selection = pickDlg.GetSelection();
        if (selection < 0 || static_cast<size_t>(selection) >= entries.size()) {
            return;
        }
        BankInstEntry const &chosen = entries[static_cast<size_t>(selection)];
        BAEBankToken chosenBank = chosen.token;
        uint32_t bankInstIndex = chosen.bankIndex;

        bool usedPrograms[128];
        long defaultProgram = 0;
        memset(usedPrograms, 0, sizeof(usedPrograms));
        {
            uint32_t sampleCount = 0;
            BAERmfEditorDocument_GetSampleCount(m_document, &sampleCount);
            for (uint32_t i = 0; i < sampleCount; ++i) {
                BAERmfEditorSampleInfo sInfo;
                if (BAERmfEditorDocument_GetSampleInfo(m_document, i, &sInfo) == BAE_NO_ERROR &&
                    sInfo.program < 128) {
                    usedPrograms[sInfo.program] = true;
                }
            }
        }
        for (int i = 0; i < 128; ++i) {
            if (!usedPrograms[i]) {
                defaultProgram = i;
                break;
            }
        }

        long targetProgram = wxGetNumberFromUser(
            "MIDI program number for aliased instrument (0-127):",
            "Program",
            "Alias Instrument from Bank",
            defaultProgram,
            0,
            127,
            this);
        if (targetProgram < 0) {
            return;
        }

        BeginUndoAction("Alias Instrument from Bank");
        BAEResult result = BAERmfEditorDocument_AliasInstrumentFromBank(
            m_document,
            chosenBank,
            bankInstIndex,
            static_cast<unsigned char>(targetProgram));
        if (result != BAE_NO_ERROR) {
            CancelUndoAction();
            wxMessageBox("Failed to alias instrument from bank.", "Alias from Bank", wxOK | wxICON_ERROR, this);
            return;
        }
        CommitUndoAction("Alias Instrument from Bank");

        PopulateSampleList();
        StopKeyboardPreview();
        {
            uint32_t sampleCount = 0;
            if (BAERmfEditorDocument_GetSampleCount(m_document, &sampleCount) == BAE_NO_ERROR && sampleCount > 0) {
                SelectTreeItemForSample(sampleCount - 1);
            }
        }
        SetStatusText(wxString::Format("Aliased instrument to program %ld", targetProgram));
    }

    void OnSampleAdd(wxCommandEvent &) {
        wxFileDialog dialog(this,
                    "Add Embedded Instrument Sample",
                            wxEmptyString,
                            wxEmptyString,
                            "Supported audio (*.wav;*.aif;*.aiff;*.mp3;*.ogg;*.flac;*.opus)|*.wav;*.aif;*.aiff;*.mp3;*.ogg;*.flac;*.opus|All files (*.*)|*.*",
                            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        BAERmfEditorSampleSetup setup;
        BAESampleInfo info;
        BAEResult addResult;
        long program;
        long root;

        if (!m_document) {
            return;
        }
        if (dialog.ShowModal() != wxID_OK) {
            return;
        }
        program = wxGetNumberFromUser("MIDI program (0-127)", "Program", "Embedded Instrument Mapping", 0, 0, 127, this);
        if (program < 0) {
            return;
        }
        root = wxGetNumberFromUser("Root key (0-127)", "Root Key", "Embedded Instrument Mapping", 60, 0, 127, this);
        if (root < 0) {
            return;
        }
        wxString displayName = wxFileNameFromPath(dialog.GetPath());
        wxScopedCharBuffer utf8Name = displayName.utf8_str();
        wxScopedCharBuffer utf8Path = dialog.GetPath().utf8_str();
        setup.program = static_cast<unsigned char>(program);
        setup.rootKey = static_cast<unsigned char>(root);
        setup.lowKey = 0;
        setup.highKey = 127;
        setup.displayName = const_cast<char *>(utf8Name.data());
        addResult = BAERmfEditorDocument_AddSampleFromFile(m_document,
                                                            const_cast<char *>(utf8Path.data()),
                                                            &setup,
                                                            &info);
        if (addResult != BAE_NO_ERROR) {
            if (addResult == BAE_BAD_FILE_TYPE) {
                wxMessageBox("Unsupported sample codec. Supported imports are PCM WAV/AIFF, MP3, Ogg Vorbis, FLAC, and Opus.",
                             "Embedded Instruments",
                             wxOK | wxICON_ERROR,
                             this);
                return;
            }
            wxMessageBox("Failed to add sample.", "Embedded Instruments", wxOK | wxICON_ERROR, this);
            return;
        }
        PopulateSampleList();
        {
            uint32_t sampleCount = 0;
            if (BAERmfEditorDocument_GetSampleCount(m_document, &sampleCount) == BAE_NO_ERROR && sampleCount > 0) {
                SelectTreeItemForSample(sampleCount - 1);
            }
        }
    }

    void OnSampleNewInstrument(wxCommandEvent &) {
        BAERmfEditorSampleSetup setup;
        BAESampleInfo info;
        uint32_t newSampleIndex;
        uint32_t sampleCount;
        bool usedPrograms[128];
        long defaultProgram;
        long program;
        long root;

        if (!m_document) {
            return;
        }

        sampleCount = 0;
        memset(usedPrograms, 0, sizeof(usedPrograms));
        BAERmfEditorDocument_GetSampleCount(m_document, &sampleCount);
        for (uint32_t i = 0; i < sampleCount; ++i) {
            BAERmfEditorSampleInfo sampleInfo;
            if (BAERmfEditorDocument_GetSampleInfo(m_document, i, &sampleInfo) == BAE_NO_ERROR &&
                sampleInfo.program < 128) {
                usedPrograms[sampleInfo.program] = true;
            }
        }
        defaultProgram = 0;
        for (int i = 0; i < 128; ++i) {
            if (!usedPrograms[i]) {
                defaultProgram = i;
                break;
            }
        }

        program = wxGetNumberFromUser("MIDI program for new instrument (0-127)",
                                      "Program",
                                      "New Instrument",
                                      defaultProgram,
                                      0,
                                      127,
                                      this);
        if (program < 0) {
            return;
        }
        root = wxGetNumberFromUser("Root key (0-127)",
                                   "Root Key",
                                   "New Instrument",
                                   60,
                                   0,
                                   127,
                                   this);
        if (root < 0) {
            return;
        }

        setup.program = static_cast<unsigned char>(program);
        setup.rootKey = static_cast<unsigned char>(root);
        setup.lowKey = 0;
        setup.highKey = 127;
        setup.displayName = const_cast<char *>("New Instrument");

        if (BAERmfEditorDocument_AddEmptySample(m_document, &setup, &newSampleIndex, &info) != BAE_NO_ERROR) {
            wxMessageBox("Failed to create new instrument.", "Embedded Instruments", wxOK | wxICON_ERROR, this);
            return;
        }

        PopulateSampleList();
        SelectTreeItemForSample(newSampleIndex);
        OpenInstrumentExtEditor();

        {
            uint32_t checkIndex;
            if (GetSelectedSampleIndexFromTree(&checkIndex)) {
                BAERmfEditorSampleInfo verifyInfo;
                if (BAERmfEditorDocument_GetSampleInfo(m_document, checkIndex, &verifyInfo) == BAE_NO_ERROR &&
                    verifyInfo.sourcePath == NULL && verifyInfo.sampleInfo.waveFrames <= 1) {
                    /* User likely canceled without replacing; remove placeholder sample. */
                    BAERmfEditorDocument_DeleteSample(m_document, checkIndex);
                    PopulateSampleList();
                }
            }
        }
    }

    void OnSampleDelete(wxCommandEvent &) {
        SampleTreeItemData *selectedData;
        std::vector<uint32_t> toDelete;

        if (!m_document) {
            return;
        }
        selectedData = GetSelectedSampleTreeData();
        if (!selectedData) {
            return;
        }

        if (selectedData->IsAssetNode()) {
            uint32_t selectedAssetID;
            uint32_t usageCount;

            selectedAssetID = selectedData->GetAssetID();
            if (BAERmfEditorDocument_GetSampleAssetUsageCount(m_document, selectedAssetID, &usageCount) != BAE_NO_ERROR) {
                return;
            }
            for (uint32_t usageIndex = 0; usageIndex < usageCount; ++usageIndex) {
                uint32_t sampleIndex;
                if (BAERmfEditorDocument_GetSampleAssetSampleIndex(m_document,
                                                                   selectedAssetID,
                                                                   usageIndex,
                                                                   &sampleIndex) == BAE_NO_ERROR) {
                    toDelete.push_back(sampleIndex);
                }
            }
            {
                wxString msg = toDelete.size() > 1
                    ? wxString::Format("Delete sample asset A%u and all %u usages?",
                                       static_cast<unsigned>(selectedAssetID),
                                       static_cast<unsigned>(toDelete.size()))
                    : wxString::Format("Delete sample asset A%u?", static_cast<unsigned>(selectedAssetID));
                if (wxMessageBox(msg, "Confirm Delete", wxYES_NO | wxICON_QUESTION, this) != wxYES) {
                    return;
                }
            }
        } else {
            uint32_t firstSampleIndex;
            BAERmfEditorSampleInfo firstInfo;
            uint32_t sampleCount;
            unsigned char program;

            firstSampleIndex = selectedData->GetSampleIndex();
            if (BAERmfEditorDocument_GetSampleInfo(m_document, firstSampleIndex, &firstInfo) != BAE_NO_ERROR) {
                return;
            }
            program = firstInfo.program;
            sampleCount = 0;
            BAERmfEditorDocument_GetSampleCount(m_document, &sampleCount);
            for (uint32_t i = 0; i < sampleCount; ++i) {
                BAERmfEditorSampleInfo info;
                if (BAERmfEditorDocument_GetSampleInfo(m_document, i, &info) == BAE_NO_ERROR &&
                    info.program == program) {
                    toDelete.push_back(i);
                }
            }
            {
                wxString msg = toDelete.size() > 1
                    ? wxString::Format("Delete instrument P%u and all %u of its splits?",
                                       static_cast<unsigned>(program),
                                       static_cast<unsigned>(toDelete.size()))
                    : wxString::Format("Delete instrument P%u?", static_cast<unsigned>(program));
                if (wxMessageBox(msg, "Confirm Delete", wxYES_NO | wxICON_QUESTION, this) != wxYES) {
                    return;
                }
            }
        }

        /* Delete in reverse order so earlier indices stay valid. */
        for (auto it = toDelete.rbegin(); it != toDelete.rend(); ++it) {
            BAERmfEditorDocument_DeleteSample(m_document, *it);
        }
        PopulateSampleList();
    }

    void OnCompressInstrument(wxCommandEvent &) {
        SampleTreeItemData *selectedData;
        std::vector<uint32_t> sampleIndices;

        if (!m_document) {
            return;
        }
        selectedData = GetSelectedSampleTreeData();
        if (!selectedData) {
            return;
        }

        // Collect all samples for the selected instrument/asset
        if (selectedData->IsAssetNode()) {
            uint32_t selectedAssetID;
            uint32_t usageCount;

            selectedAssetID = selectedData->GetAssetID();
            if (BAERmfEditorDocument_GetSampleAssetUsageCount(m_document, selectedAssetID, &usageCount) != BAE_NO_ERROR) {
                return;
            }
            for (uint32_t usageIndex = 0; usageIndex < usageCount; ++usageIndex) {
                uint32_t sampleIndex;
                if (BAERmfEditorDocument_GetSampleAssetSampleIndex(m_document,
                                                                   selectedAssetID,
                                                                   usageIndex,
                                                                   &sampleIndex) == BAE_NO_ERROR) {
                    sampleIndices.push_back(sampleIndex);
                }
            }
        } else {
            uint32_t firstSampleIndex;
            BAERmfEditorSampleInfo firstInfo;
            uint32_t sampleCount;
            unsigned char program;

            firstSampleIndex = selectedData->GetSampleIndex();
            if (BAERmfEditorDocument_GetSampleInfo(m_document, firstSampleIndex, &firstInfo) != BAE_NO_ERROR) {
                return;
            }
            program = firstInfo.program;
            sampleCount = 0;
            BAERmfEditorDocument_GetSampleCount(m_document, &sampleCount);
            for (uint32_t i = 0; i < sampleCount; ++i) {
                BAERmfEditorSampleInfo info;
                if (BAERmfEditorDocument_GetSampleInfo(m_document, i, &info) == BAE_NO_ERROR &&
                    info.program == program) {
                    sampleIndices.push_back(i);
                }
            }
        }

        if (sampleIndices.empty()) {
            return;
        }

        // Show compression dialog
        BAERmfEditorCompressionType initialCompressionType = BAE_EDITOR_COMPRESSION_OPUS_128K;
        BAERmfEditorOpusMode initialOpusMode = BAE_EDITOR_OPUS_MODE_AUDIO;
        bool initialOpusRoundTrip = false;
        {
            BAERmfEditorSampleInfo initialInfo;
            if (BAERmfEditorDocument_GetSampleInfo(m_document, sampleIndices[0], &initialInfo) == BAE_NO_ERROR)
            {
                initialCompressionType = initialInfo.compressionType;
                initialOpusMode = initialInfo.opusMode;
                initialOpusRoundTrip = (initialInfo.opusRoundTripResample == TRUE);
            }
        }
        BatchCompressDialog dlg(this,
                                sampleIndices,
                                false,
                                initialCompressionType,
                                initialOpusMode,
                                initialOpusRoundTrip);
        if (dlg.ShowModal() == wxID_OK) {
            BAERmfEditorCompressionType compressionType = dlg.GetSelectedCompressionType();
            BAERmfEditorOpusMode opusMode = dlg.GetSelectedOpusMode();
            bool opusRoundTrip = dlg.GetSelectedOpusRoundTrip();
            // Apply compression to all samples
            for (uint32_t sampleIndex : sampleIndices) {
                BAERmfEditorSampleInfo info;
                if (BAERmfEditorDocument_GetSampleInfo(m_document, sampleIndex, &info) == BAE_NO_ERROR) {
                    info.compressionType = compressionType;
                    info.opusMode = opusMode;
                    info.opusRoundTripResample = opusRoundTrip ? TRUE : FALSE;
                    BAERmfEditorDocument_SetSampleInfo(m_document, sampleIndex, &info);
                }
            }
            PopulateSampleList();
        }
    }

    void OnCompressAllInstruments(wxCommandEvent &) {
        std::vector<uint32_t> allSampleIndices;
        uint32_t sampleCount = 0;

        if (!m_document) {
            return;
        }

        BAERmfEditorDocument_GetSampleCount(m_document, &sampleCount);
        for (uint32_t i = 0; i < sampleCount; ++i) {
            allSampleIndices.push_back(i);
        }

        if (allSampleIndices.empty()) {
            wxMessageBox("No samples to compress.", "No Samples", wxOK | wxICON_INFORMATION, this);
            return;
        }

        // Show compression dialog
        BAERmfEditorCompressionType initialCompressionType = BAE_EDITOR_COMPRESSION_OPUS_128K;
        BAERmfEditorOpusMode initialOpusMode = BAE_EDITOR_OPUS_MODE_AUDIO;
        bool initialOpusRoundTrip = false;
        {
            BAERmfEditorSampleInfo initialInfo;
            if (BAERmfEditorDocument_GetSampleInfo(m_document, allSampleIndices[0], &initialInfo) == BAE_NO_ERROR)
            {
                initialCompressionType = initialInfo.compressionType;
                initialOpusMode = initialInfo.opusMode;
                initialOpusRoundTrip = (initialInfo.opusRoundTripResample == TRUE);
            }
        }
        BatchCompressDialog dlg(this,
                                allSampleIndices,
                                true,
                                initialCompressionType,
                                initialOpusMode,
                                initialOpusRoundTrip);
        if (dlg.ShowModal() == wxID_OK) {
            BAERmfEditorCompressionType compressionType = dlg.GetSelectedCompressionType();
            BAERmfEditorOpusMode opusMode = dlg.GetSelectedOpusMode();
            bool opusRoundTrip = dlg.GetSelectedOpusRoundTrip();
            // Apply compression to all samples
            for (uint32_t sampleIndex : allSampleIndices) {
                BAERmfEditorSampleInfo info;
                if (BAERmfEditorDocument_GetSampleInfo(m_document, sampleIndex, &info) == BAE_NO_ERROR) {
                    info.compressionType = compressionType;
                    info.opusMode = opusMode;
                    info.opusRoundTripResample = opusRoundTrip ? TRUE : FALSE;
                    BAERmfEditorDocument_SetSampleInfo(m_document, sampleIndex, &info);
                }
            }
            PopulateSampleList();
        }
    }

    void OnDeleteAllInstruments(wxCommandEvent &) {
        uint32_t sampleCount;
        bool allDeleted;

        if (!m_document) {
            return;
        }

        sampleCount = 0;
        if (BAERmfEditorDocument_GetSampleCount(m_document, &sampleCount) != BAE_NO_ERROR) {
            wxMessageBox("Failed to enumerate instruments.", "Delete All Instruments", wxOK | wxICON_ERROR, this);
            return;
        }
        if (sampleCount == 0) {
            wxMessageBox("No instruments to delete.", "Delete All Instruments", wxOK | wxICON_INFORMATION, this);
            return;
        }

        if (wxMessageBox(wxString::Format("Delete all %u instruments?", static_cast<unsigned>(sampleCount)),
                         "Confirm Delete All",
                         wxYES_NO | wxICON_WARNING,
                         this) != wxYES) {
            return;
        }

        allDeleted = true;
        for (uint32_t sampleIndex = sampleCount; sampleIndex > 0; --sampleIndex) {
            if (BAERmfEditorDocument_DeleteSample(m_document, sampleIndex - 1) != BAE_NO_ERROR) {
                allDeleted = false;
                break;
            }
        }

        PopulateSampleList();
        if (allDeleted) {
            SetStatusText("Deleted all instruments", 0);
        } else {
            wxMessageBox("Failed while deleting instruments. Some items may remain.",
                         "Delete All Instruments",
                         wxOK | wxICON_ERROR,
                         this);
        }
    }

    void OnInstrumentEdit(wxCommandEvent &) {
        OpenInstrumentExtEditor();
    }

    void OnInstrumentEditDblClick(wxTreeEvent &) {
        OpenInstrumentExtEditor();
    }

    void OpenInstrumentExtEditor() {
        uint32_t primarySampleIndex;
        uint32_t selectedAssetID;
        uint32_t instID;
        BAERmfEditorInstrumentExtInfo extInfo;
        std::vector<uint32_t> deletedSampleIndices;
        std::vector<uint32_t> sampleIndices;
        std::vector<InstrumentEditorEditedSample> editedSamples;
        bool accepted;

        if (!m_document) {
            return;
        }
        if (!GetSelectedSampleIndexFromTree(&primarySampleIndex)) {
            return;
        }
        if (BAERmfEditorDocument_GetSampleAssetIDForSample(m_document, primarySampleIndex, &selectedAssetID) != BAE_NO_ERROR) {
            selectedAssetID = 0;
        }

        /* Look up the instID and fetch extended info */
        memset(&extInfo, 0, sizeof(extInfo));
        if (BAERmfEditorDocument_GetInstIDForSample(m_document, primarySampleIndex, &instID) == BAE_NO_ERROR) {
            extInfo.instID = instID;
            BAERmfEditorDocument_GetInstrumentExtInfo(m_document, instID, &extInfo);
        }

        accepted = ShowInstrumentExtEditorDialog(
            this,
            m_document,
            primarySampleIndex,
            &extInfo,
            [this](wxString const &label) { BeginUndoAction(label); },
            [this](wxString const &label) { CommitUndoAction(label); },
            [this]() { CancelUndoAction(); },
            [this](uint32_t sampleIndex, int key, BAESampleInfo const *overrideInfo, int16_t splitVolumeOverride, unsigned char rootKeyOverride, BAERmfEditorCompressionType compressionOverride, bool opusRoundTripOverride, int previewTag, BAERmfEditorInstrumentExtInfo const *extOverride) {
                if (!PreviewSampleAtKey(sampleIndex,
                                        key,
                                        overrideInfo,
                                        splitVolumeOverride,
                                        rootKeyOverride,
                                        compressionOverride,
                                        opusRoundTripOverride,
                                        previewTag,
                                        extOverride)) {
                    wxMessageBox("Preview playback failed for this sample.",
                                 "Sample Preview", wxOK | wxICON_ERROR, this);
                }
            },
            [this]() {
                StopPreviewSample();
            },
            [this](int previewTag) {
                StopPreviewSampleForTag(previewTag);
            },
            [this]() {
                StopKeyboardPreview();
            },
            [this](uint32_t sampleIndex, wxString const &path) {
                return ReplaceSampleFromPath(sampleIndex, path);
            },
            [this](uint32_t sampleIndex, wxString const &path) {
                return ExportSampleToPath(sampleIndex, path);
            },
            &deletedSampleIndices,
            &sampleIndices,
            &editedSamples);

        if (!accepted) {
            StopAndDestroyInstrumentPreviewSession();
            return;
        }
        StopAndDestroyInstrumentPreviewSession();

        /* Apply sample edits */
        {
            bool ok = true;
            std::vector<uint32_t> affectAllAssets;
            std::vector<uint32_t> cloneAssets;

            for (size_t i = 0; i < sampleIndices.size() && i < editedSamples.size(); ++i) {
                BAERmfEditorCompressionType oldCompression;

                /* A sentinel index means the sample was created inside the dialog and needs
                 * to be added to the document first. */
                if (sampleIndices[i] == static_cast<uint32_t>(-1)) {
                    BAERmfEditorSampleSetup setup;
                    BAESampleInfo dummyInfo;
                    uint32_t newIdx = static_cast<uint32_t>(-1);
                    wxScopedCharBuffer setupName = editedSamples[i].displayName.utf8_str();
                    setup.program = editedSamples[i].program;
                    setup.rootKey = editedSamples[i].rootKey;
                    setup.lowKey = editedSamples[i].lowKey;
                    setup.highKey = editedSamples[i].highKey;
                    setup.displayName = const_cast<char *>(setupName.data());
                    if (BAERmfEditorDocument_AddEmptySample(m_document, &setup, &newIdx, &dummyInfo) != BAE_NO_ERROR) {
                        ok = false;
                        break;
                    }
                    sampleIndices[i] = newIdx;
                }

                oldCompression = BAE_EDITOR_COMPRESSION_PCM;
                {
                    BAERmfEditorSampleInfo oldInfo;
                    if (BAERmfEditorDocument_GetSampleInfo(m_document, sampleIndices[i], &oldInfo) == BAE_NO_ERROR) {
                        oldCompression = oldInfo.compressionType;
                    }
                }

                if (oldCompression != editedSamples[i].compressionType) {
                    uint32_t assetID;
                    uint32_t usageCount;

                    assetID = 0;
                    usageCount = 0;
                    if (BAERmfEditorDocument_GetSampleAssetIDForSample(m_document, sampleIndices[i], &assetID) == BAE_NO_ERROR &&
                        BAERmfEditorDocument_GetSampleAssetUsageCount(m_document, assetID, &usageCount) == BAE_NO_ERROR &&
                        usageCount > 1 &&
                        std::find(affectAllAssets.begin(), affectAllAssets.end(), assetID) == affectAllAssets.end()) {
                        if (std::find(cloneAssets.begin(), cloneAssets.end(), assetID) != cloneAssets.end()) {
                            if (BAERmfEditorDocument_CloneSampleAssetForSample(m_document, sampleIndices[i], NULL) != BAE_NO_ERROR) {
                                ok = false;
                                break;
                            }
                        } else {
                            wxMessageDialog choiceDialog(
                                this,
                                wxString::Format("Sample asset A%u is used by %u instrument usages.\n\n"
                                                 "Choose how to apply this compression change.\n"
                                                 "(\"This Instrument Only\" will clone the sample asset, creating a new sample asset with the new compression, and only this instrument will use it. \"All Shared Uses\" will change the compression for all instruments that use this sample asset.)",
                                                 static_cast<unsigned>(assetID),
                                                 static_cast<unsigned>(usageCount)),
                                "Shared Sample Asset",
                                wxYES_NO | wxCANCEL | wxICON_QUESTION | wxYES_DEFAULT);
                            choiceDialog.SetYesNoLabels("This Instrument Only", "All Shared Uses");
                            int choice = choiceDialog.ShowModal();
                            if (choice == wxCANCEL) {
                                ok = false;
                                break;
                            }
                            if (choice == wxYES) {
                                cloneAssets.push_back(assetID);
                                if (BAERmfEditorDocument_CloneSampleAssetForSample(m_document, sampleIndices[i], NULL) != BAE_NO_ERROR) {
                                    ok = false;
                                    break;
                                }
                            } else {
                                affectAllAssets.push_back(assetID);
                            }
                        }
                    }
                }

                BAERmfEditorSampleInfo info;
                wxScopedCharBuffer nameUtf8 = editedSamples[i].displayName.utf8_str();
                info.displayName = const_cast<char *>(nameUtf8.data());
                info.sourcePath = NULL;
                info.program = editedSamples[i].program;
                info.rootKey = editedSamples[i].rootKey;
                info.lowKey = editedSamples[i].lowKey;
                info.highKey = editedSamples[i].highKey;
                info.splitVolume = editedSamples[i].splitVolume;
                info.sampleInfo = editedSamples[i].sampleInfo;
                info.compressionType = editedSamples[i].compressionType;
                info.hasOriginalData = editedSamples[i].hasOriginalData ? TRUE : FALSE;
                info.sndStorageType = editedSamples[i].sndStorageType;
                info.opusMode = editedSamples[i].opusMode;
                info.opusRoundTripResample = editedSamples[i].opusRoundTripResample ? TRUE : FALSE;
                if (BAERmfEditorDocument_SetSampleInfo(m_document, sampleIndices[i], &info) != BAE_NO_ERROR) {
                    ok = false;
                    break;
                }
            }
            if (!ok) {
                wxMessageBox("Failed to apply instrument sample edits.",
                             "Edit Instrument", wxOK | wxICON_ERROR, this);
            }
        }

        if (!deletedSampleIndices.empty()) {
            std::sort(deletedSampleIndices.begin(), deletedSampleIndices.end());
            deletedSampleIndices.erase(std::unique(deletedSampleIndices.begin(), deletedSampleIndices.end()), deletedSampleIndices.end());
            for (auto it = deletedSampleIndices.rbegin(); it != deletedSampleIndices.rend(); ++it) {
                BAERmfEditorDocument_DeleteSample(m_document, *it);
            }
        }

        PopulateSampleList();
        if (!sampleIndices.empty() && sampleIndices[0] != static_cast<uint32_t>(-1)) {
            SelectTreeItemForSample(sampleIndices[0]);
        } else if (selectedAssetID != 0) {
            uint32_t usageCount = 0;
            if (BAERmfEditorDocument_GetSampleAssetUsageCount(m_document, selectedAssetID, &usageCount) == BAE_NO_ERROR && usageCount > 0) {
                uint32_t sampleIndex;
                if (BAERmfEditorDocument_GetSampleAssetSampleIndex(m_document, selectedAssetID, 0, &sampleIndex) == BAE_NO_ERROR) {
                    SelectTreeItemForSample(sampleIndex);
                }
            }
        }

        StopKeyboardPreview();
        InvalidatePianoRollPreviewSong();

    }
};

}  // namespace

class EditorApp final : public wxApp {
public:
    bool OnInit() override {
        bool darkMode = false;

#ifdef __WXMSW__
        /* Windows: detect dark mode from registry, same as zefidi. */
        {
            HKEY key;
            if (RegOpenKeyExA(HKEY_CURRENT_USER,
                              "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                              0, KEY_READ, &key) == ERROR_SUCCESS) {
                DWORD value;
                DWORD size = sizeof(value);
                DWORD type;
                if (RegQueryValueExA(key, "AppsUseLightTheme", NULL, &type,
                                     reinterpret_cast<BYTE *>(&value), &size) == ERROR_SUCCESS &&
                    type == REG_DWORD) {
                    darkMode = (value == 0);
                }
                RegCloseKey(key);
            }
            if (darkMode) {
                /* wxWidgets 3.3+: enable dark mode for all controls and title bar. */
                MSWEnableDarkMode();
            }
        }
#endif // __WXMSW__

#ifdef __WXGTK__
        /* Linux/GTK: honour --dark command-line flag. */
        {
            wxString startupFile;
            for (int i = 1; i < argc; ++i) {
                if (argv[i] == "--dark") {
                    darkMode = true;
                }
            }
            if (darkMode) {
                g_object_set(gtk_settings_get_default(),
                             "gtk-application-prefer-dark-theme", TRUE, NULL);
            }
        }
#endif // __WXGTK__

        auto *frame = new MainFrame();
        frame->Show(true);

        wxString startupPath;
        for (int i = 1; i < argc; ++i) {
            if (argv[i] == "--dark") continue;
            if (startupPath.IsEmpty()) {
                startupPath = argv[i];
            }
        }
        if (!startupPath.IsEmpty()) {
            frame->CallAfter([frame, startupPath]() {
                frame->OpenPathFromCli(startupPath);
            });
        }

        return true;
    }
};

wxIMPLEMENT_APP(EditorApp);