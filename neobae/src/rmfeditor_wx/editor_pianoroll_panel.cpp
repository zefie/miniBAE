#include <stdint.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#include <wx/dcbuffer.h>
#include <wx/msgdlg.h>
#include <wx/scrolwin.h>
#include <wx/spinctrl.h>
#include <wx/wx.h>

#include "editor_pianoroll_panel.h"

constexpr int kPianoRollLeftGutter = 64;
constexpr int kPianoRollTopGutter = 30;
constexpr int kNoteHeight = 12;
constexpr int kBasePixelsPerQuarter = 96;
constexpr int kResizeHandlePixels = 6;
constexpr int kAutomationLaneHeight = 44;
constexpr int kAutomationLaneGap = 6;
constexpr int kAutomationNodeHitRadius = 6;
constexpr uint32_t kDefaultNoteDuration = 480;
constexpr int kTempoLaneIndex = 0;

enum class DragMode {
    None,
    Move,
    ResizeLeft,
    ResizeRight,
    SelectBox,
    TimelineEnd,
};

enum class MidiLoopDragMode {
    None,
    StartHandle,
    EndHandle,
};

enum {
    ID_PianoRollEdit = wxID_HIGHEST + 2000,
    ID_PianoRollDelete,
    ID_PianoRollToggleRamp,
};

enum class PianoRollSelectionKind {
    None,
    Note,
    Automation,
};

enum class AutomationLaneKind {
    Tempo,
    Controller,
    PitchBend,
};

struct AutomationLaneDescriptor {
    wxString label;
    AutomationLaneKind kind;
    unsigned char controller;
    wxColour color;
    int maxValue;
    bool bipolar;
};

/* ---------- Theme palette for the piano roll grid area ---------- */
struct PianoRollTheme {
    /* Note grid */
    wxColour whiteKeyFill;
    wxColour blackKeyFill;
    wxColour noteSeparator;
    wxColour noteLabelText;
    wxColour majorGridLine;
    wxColour minorGridLine;
    wxColour gutterSeparator;
    wxColour endRegionShade;   /* translucent overlay beyond end marker */

    /* Ruler */
    wxColour rulerBg;
    wxColour rulerGutterBg;
    wxColour rulerGutterBeatText;
    wxColour rulerGutterTimeText;
    wxColour rulerSubTick;
    wxColour rulerBarTick;
    wxColour rulerBeatTick;
    wxColour rulerBarText;
    wxColour rulerBeatText;
    wxColour rulerTimeText;
    wxColour rulerBorder;
    wxColour rulerGutterBorder;

    /* Automation lanes */
    wxColour automationLaneBg;
    wxColour automationLaneGutterBg;
    wxColour automationLaneBorder;
    wxColour automationLaneLabel;
    wxColour automationLaneCenterLine;
    wxColour automationSelectedBorder;
};

static PianoRollTheme MakeLightTheme() {
    PianoRollTheme t;
    t.whiteKeyFill          = wxColour(248, 248, 246);
    t.blackKeyFill          = wxColour(235, 238, 240);
    t.noteSeparator         = wxColour(220, 224, 226);
    t.noteLabelText         = wxColour(90, 90, 90);
    t.majorGridLine         = wxColour(180, 186, 190);
    t.minorGridLine         = wxColour(222, 226, 228);
    t.gutterSeparator       = wxColour(60, 60, 60);
    t.endRegionShade        = wxColour(0, 0, 0, 28);
    t.rulerBg               = wxColour(42, 44, 50);
    t.rulerGutterBg         = wxColour(30, 32, 36);
    t.rulerGutterBeatText   = wxColour(130, 135, 140);
    t.rulerGutterTimeText   = wxColour(100, 105, 110);
    t.rulerSubTick          = wxColour(70, 75, 82);
    t.rulerBarTick          = wxColour(190, 195, 200);
    t.rulerBeatTick         = wxColour(130, 135, 140);
    t.rulerBarText          = wxColour(240, 242, 245);
    t.rulerBeatText         = wxColour(195, 198, 202);
    t.rulerTimeText         = wxColour(140, 145, 150);
    t.rulerBorder           = wxColour(70, 75, 82);
    t.rulerGutterBorder     = wxColour(100, 105, 110);
    t.automationLaneBg      = wxColour(32, 34, 38);
    t.automationLaneGutterBg = wxColour(42, 44, 50);
    t.automationLaneBorder  = wxColour(68, 72, 78);
    t.automationLaneLabel   = wxColour(214, 217, 222);
    t.automationLaneCenterLine = wxColour(84, 88, 96);
    t.automationSelectedBorder = wxColour(255, 232, 190);
    return t;
}

static PianoRollTheme MakeDarkTheme() {
    PianoRollTheme t;
    t.whiteKeyFill          = wxColour(28, 30, 34);
    t.blackKeyFill          = wxColour(22, 24, 28);
    t.noteSeparator         = wxColour(48, 52, 58);
    t.noteLabelText         = wxColour(160, 165, 170);
    t.majorGridLine         = wxColour(80, 85, 92);
    t.minorGridLine         = wxColour(42, 46, 52);
    t.gutterSeparator       = wxColour(100, 105, 110);
    t.endRegionShade        = wxColour(0, 0, 0, 60);
    t.rulerBg               = wxColour(32, 34, 38);
    t.rulerGutterBg         = wxColour(22, 24, 28);
    t.rulerGutterBeatText   = wxColour(130, 135, 140);
    t.rulerGutterTimeText   = wxColour(100, 105, 110);
    t.rulerSubTick          = wxColour(60, 65, 72);
    t.rulerBarTick          = wxColour(170, 175, 180);
    t.rulerBeatTick         = wxColour(110, 115, 120);
    t.rulerBarText          = wxColour(220, 222, 225);
    t.rulerBeatText         = wxColour(170, 173, 178);
    t.rulerTimeText         = wxColour(120, 125, 130);
    t.rulerBorder           = wxColour(60, 65, 72);
    t.rulerGutterBorder     = wxColour(90, 95, 100);
    t.automationLaneBg      = wxColour(24, 26, 30);
    t.automationLaneGutterBg = wxColour(32, 34, 38);
    t.automationLaneBorder  = wxColour(56, 60, 66);
    t.automationLaneLabel   = wxColour(200, 203, 208);
    t.automationLaneCenterLine = wxColour(72, 76, 84);
    t.automationSelectedBorder = wxColour(255, 232, 190);
    return t;
}

static bool DetectSystemDarkMode() {
    wxColour bg = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
    int luminance = (bg.Red() * 299 + bg.Green() * 587 + bg.Blue() * 114) / 1000;
    return luminance < 128;
}

struct AutomationHitInfo {
    int laneIndex;
    uint32_t eventIndex;
    uint32_t tick;
    uint32_t endTick;
    int laneTop;
    int laneBottom;
    int leftX;
    int rightX;
    int value;
    bool hasFollowingEvent;
};

enum class AutomationInterpolation {
    Step,
    Linear,
};

struct AutomationCurveNode {
    uint32_t tick;
    int value;
    uint32_t sourceEventIndex;
    bool implicitNode;
};

struct AutomationCurveSegment {
    uint32_t startTick;
    uint32_t endTick;
    int startValue;
    int endValue;
    uint32_t sourceEventIndex;
    bool hasFollowingEvent;
    AutomationInterpolation interpolation;
};

struct AutomationLaneCache {
    bool valid = false;
    uint64_t revision = 0;
    uint32_t documentEndTick = 0;
    std::vector<AutomationCurveNode> nodes;
    std::vector<AutomationCurveSegment> segments;
};

struct AutomationDisplayBucket {
    int x = 0;
    int minY = 0;
    int maxY = 0;
    int firstY = 0;
    int lastY = 0;
    bool hasData = false;
};

struct AutomationDisplayCache {
    bool valid = false;
    uint64_t revision = 0;
    int laneIndex = -1;
    int visibleLeft = 0;
    int visibleRight = 0;
    int pixelsPerQuarter = 0;
    std::vector<AutomationDisplayBucket> buckets;
};

struct AutomationRampSegmentKey {
    int laneIndex;
    uint32_t tick;
    int value;
};

struct NoteCacheEntry {
    BAERmfEditorNoteInfo noteInfo;
    uint32_t sourceIndex;
    uint32_t endTick;
};

struct NoteTrackCache {
    bool valid = false;
    int trackIndex = -1;
    uint32_t ticksPerBin = 0;
    std::vector<NoteCacheEntry> notes;
    std::vector<std::vector<std::vector<uint32_t>>> pitchBins;
};

struct RenderCache {
    wxPen   penNoteNormal;
    wxPen   penNoteSelected;
    wxBrush brushNoteNormal;
    wxBrush brushNoteSelected;

    wxPen   penAutomationBorder;
    wxPen   penAutomationSelectedBorder;
    wxBrush brushAutomationBg;
    wxBrush brushAutomationGutter;

    wxPen   penTimelineEnd;
    wxBrush brushTimelineEnd;

    wxPen   penLoopLine;
    wxBrush brushLoopFill;

    wxBrush brushWhiteKey;
    wxBrush brushBlackKey;
    wxPen   penNoteSeparator;
    wxPen   penGutterSeparator;
    wxBrush brushRulerBg;
    wxBrush brushRulerGutterBg;
    wxPen   penRulerSubTick;
    wxPen   penRulerBarTick;
    wxPen   penRulerBeatTick;
    wxPen   penRulerBorder;
    wxPen   penRulerGutterBorder;
    wxPen   penPlayhead;
    wxBrush brushSelectionBox;
    wxPen   penSelectionBoxBorder;
    wxPen   penTimelineHandle;
    wxBrush brushTimelineHandleFill;
    wxBrush brushMidiLoopShade;
    wxPen   penAutomationCenterLine;
};

static wxString GetControllerLaneLabel(unsigned char controller) {
    switch (controller) {
        case 1: return "Mod Wheel (CC1)";
        case 2: return "Breath (CC2)";
        case 4: return "Foot Ctrl (CC4)";
        case 5: return "Portamento Time (CC5)";
        case 6: return "Data Entry (CC6)";
        case 7: return "Volume (CC7)";
        case 8: return "Balance (CC8)";
        case 10: return "Pan (CC10)";
        case 11: return "Expression (CC11)";
        case 64: return "Sustain (CC64)";
        case 65: return "Portamento (CC65)";
        case 66: return "Sostenuto (CC66)";
        case 67: return "Soft Pedal (CC67)";
        case 71: return "Resonance (CC71)";
        case 72: return "Release (CC72)";
        case 73: return "Attack (CC73)";
        case 74: return "Brightness (CC74)";
        case 91: return "Reverb Send (CC91)";
        case 93: return "Chorus Send (CC93)";
        default:
            return wxString::Format("CC%u", static_cast<unsigned>(controller));
    }
}

static std::vector<AutomationLaneDescriptor> const &GetAutomationLanes() {
    static std::vector<AutomationLaneDescriptor> const lanes = {
        { "Tempo", AutomationLaneKind::Tempo, 0, wxColour(202, 128, 52), 240, false },
        { "Mod Wheel (CC1)", AutomationLaneKind::Controller, 1, wxColour(74, 136, 208), 127, false },
        { "Breath (CC2)", AutomationLaneKind::Controller, 2, wxColour(166, 96, 196), 127, false },
        { "Foot Ctrl (CC4)", AutomationLaneKind::Controller, 4, wxColour(76, 170, 92), 127, false },
        { "Portamento Time (CC5)", AutomationLaneKind::Controller, 5, wxColour(228, 148, 56), 127, false },
        { "Data Entry (CC6)", AutomationLaneKind::Controller, 6, wxColour(82, 172, 170), 127, false },
        { "Volume (CC7)", AutomationLaneKind::Controller, 7, wxColour(74, 136, 208), 127, false },
        { "Balance (CC8)", AutomationLaneKind::Controller, 8, wxColour(166, 96, 196), 127, true },
        { "Pan (CC10)", AutomationLaneKind::Controller, 10, wxColour(76, 170, 92), 127, true },
        { "Expression (CC11)", AutomationLaneKind::Controller, 11, wxColour(228, 148, 56), 127, false },
        { "Sustain (CC64)", AutomationLaneKind::Controller, 64, wxColour(82, 172, 170), 127, false },
        { "Portamento (CC65)", AutomationLaneKind::Controller, 65, wxColour(210, 96, 118), 127, false },
        { "Sostenuto (CC66)", AutomationLaneKind::Controller, 66, wxColour(74, 136, 208), 127, false },
        { "Soft Pedal (CC67)", AutomationLaneKind::Controller, 67, wxColour(166, 96, 196), 127, false },
        { "Resonance (CC71)", AutomationLaneKind::Controller, 71, wxColour(76, 170, 92), 127, false },
        { "Release (CC72)", AutomationLaneKind::Controller, 72, wxColour(228, 148, 56), 127, false },
        { "Attack (CC73)", AutomationLaneKind::Controller, 73, wxColour(82, 172, 170), 127, false },
        { "Brightness (CC74)", AutomationLaneKind::Controller, 74, wxColour(210, 96, 118), 127, false },
        { "Reverb Send (CC91)", AutomationLaneKind::Controller, 91, wxColour(74, 136, 208), 127, false },
        { "Chorus Send (CC93)", AutomationLaneKind::Controller, 93, wxColour(166, 96, 196), 127, false },
        { "Pitch Bend", AutomationLaneKind::PitchBend, 0, wxColour(188, 120, 214), 16383, true },
    };

    return lanes;
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

class NoteEditDialog final : public wxDialog {
public:
    explicit NoteEditDialog(wxWindow *parent,
                            BAERmfEditorNoteInfo const &noteInfo,
                            bool hasResonance,
                            unsigned char resonanceValue,
                            bool hasBrightness,
                            unsigned char brightnessValue)
        : wxDialog(parent, wxID_ANY, "Edit Note", wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
        wxBoxSizer *rootSizer = new wxBoxSizer(wxVERTICAL);
        wxFlexGridSizer *gridSizer = new wxFlexGridSizer(0, 2, 8, 8);

        m_startTickText = new wxTextCtrl(this, wxID_ANY, wxString::Format("%u", static_cast<unsigned>(noteInfo.startTick)));
        m_durationText = new wxTextCtrl(this, wxID_ANY, wxString::Format("%u", static_cast<unsigned>(noteInfo.durationTicks)));
        m_noteSpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 127, noteInfo.note);
        m_velocitySpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 127, noteInfo.velocity);
        m_channelSpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 16, static_cast<int>(noteInfo.channel) + 1);
        m_bankSpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 127, DisplayBankFromInternal(noteInfo.bank));
        m_programSpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 127, noteInfo.program);
        m_resonanceEnable = new wxCheckBox(this, wxID_ANY, "Set Resonance (CC71)");
        m_resonanceEnable->SetValue(hasResonance);
        m_resonanceSpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 127, resonanceValue);
        m_resonanceSpin->Enable(hasResonance);
        m_brightnessEnable = new wxCheckBox(this, wxID_ANY, "Set Brightness (CC74)");
        m_brightnessEnable->SetValue(hasBrightness);
        m_brightnessSpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 127, brightnessValue);
        m_brightnessSpin->Enable(hasBrightness);

        gridSizer->Add(new wxStaticText(this, wxID_ANY, "Start Tick"), 0, wxALIGN_CENTER_VERTICAL);
        gridSizer->Add(m_startTickText, 1, wxEXPAND);
        gridSizer->Add(new wxStaticText(this, wxID_ANY, "Duration"), 0, wxALIGN_CENTER_VERTICAL);
        gridSizer->Add(m_durationText, 1, wxEXPAND);
        gridSizer->Add(new wxStaticText(this, wxID_ANY, "Note"), 0, wxALIGN_CENTER_VERTICAL);
        gridSizer->Add(m_noteSpin, 1, wxEXPAND);
        gridSizer->Add(new wxStaticText(this, wxID_ANY, "Velocity"), 0, wxALIGN_CENTER_VERTICAL);
        gridSizer->Add(m_velocitySpin, 1, wxEXPAND);
        gridSizer->Add(new wxStaticText(this, wxID_ANY, "Channel"), 0, wxALIGN_CENTER_VERTICAL);
        gridSizer->Add(m_channelSpin, 1, wxEXPAND);
        gridSizer->Add(new wxStaticText(this, wxID_ANY, "Bank"), 0, wxALIGN_CENTER_VERTICAL);
        gridSizer->Add(m_bankSpin, 1, wxEXPAND);
        gridSizer->Add(new wxStaticText(this, wxID_ANY, "Program"), 0, wxALIGN_CENTER_VERTICAL);
        gridSizer->Add(m_programSpin, 1, wxEXPAND);
        gridSizer->Add(m_resonanceEnable, 0, wxALIGN_CENTER_VERTICAL);
        gridSizer->Add(m_resonanceSpin, 1, wxEXPAND);
        gridSizer->Add(m_brightnessEnable, 0, wxALIGN_CENTER_VERTICAL);
        gridSizer->Add(m_brightnessSpin, 1, wxEXPAND);
        gridSizer->AddGrowableCol(1, 1);

        m_resonanceEnable->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) {
            if (m_resonanceSpin) {
                m_resonanceSpin->Enable(m_resonanceEnable->GetValue());
            }
        });
        m_brightnessEnable->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) {
            if (m_brightnessSpin) {
                m_brightnessSpin->Enable(m_brightnessEnable->GetValue());
            }
        });

        rootSizer->Add(gridSizer, 1, wxEXPAND | wxALL, 12);
        rootSizer->Add(CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
        SetSizerAndFit(rootSizer);
    }

    bool GetNoteInfo(BAERmfEditorNoteInfo *outNoteInfo) const {
        unsigned long long startTick;
        unsigned long long durationTicks;

        if (!outNoteInfo) {
            return false;
        }
        if (!m_startTickText->GetValue().ToULongLong(&startTick) || !m_durationText->GetValue().ToULongLong(&durationTicks) || durationTicks == 0) {
            return false;
        }
        outNoteInfo->startTick = static_cast<uint32_t>(startTick);
        outNoteInfo->durationTicks = static_cast<uint32_t>(durationTicks);
        outNoteInfo->note = static_cast<unsigned char>(m_noteSpin->GetValue());
        outNoteInfo->velocity = static_cast<unsigned char>(m_velocitySpin->GetValue());
        outNoteInfo->channel = static_cast<unsigned char>(std::clamp(m_channelSpin->GetValue() - 1, 0, 15));
        outNoteInfo->bank = InternalBankFromDisplay(static_cast<uint16_t>(m_bankSpin->GetValue()));
        outNoteInfo->program = static_cast<unsigned char>(m_programSpin->GetValue());
        return true;
    }

    void GetFilterSettings(bool *outSetResonance,
                           unsigned char *outResonance,
                           bool *outSetBrightness,
                           unsigned char *outBrightness) const {
        if (outSetResonance) {
            *outSetResonance = m_resonanceEnable && m_resonanceEnable->GetValue();
        }
        if (outResonance) {
            *outResonance = static_cast<unsigned char>(m_resonanceSpin ? m_resonanceSpin->GetValue() : 0);
        }
        if (outSetBrightness) {
            *outSetBrightness = m_brightnessEnable && m_brightnessEnable->GetValue();
        }
        if (outBrightness) {
            *outBrightness = static_cast<unsigned char>(m_brightnessSpin ? m_brightnessSpin->GetValue() : 0);
        }
    }

private:
    wxTextCtrl *m_startTickText;
    wxTextCtrl *m_durationText;
    wxSpinCtrl *m_noteSpin;
    wxSpinCtrl *m_velocitySpin;
    wxSpinCtrl *m_channelSpin;
    wxSpinCtrl *m_bankSpin;
    wxSpinCtrl *m_programSpin;
    wxCheckBox *m_resonanceEnable;
    wxSpinCtrl *m_resonanceSpin;
    wxCheckBox *m_brightnessEnable;
    wxSpinCtrl *m_brightnessSpin;
};

class AutomationEditDialog final : public wxDialog {
public:
    AutomationEditDialog(wxWindow *parent,
                         wxString const &title,
                         wxString const &valueLabel,
                         uint32_t tick,
                         int value,
                         int maxValue,
                         uint32_t durationTicks,
                         bool allowDurationEdit)
        : wxDialog(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
        wxBoxSizer *rootSizer = new wxBoxSizer(wxVERTICAL);
        wxFlexGridSizer *gridSizer = new wxFlexGridSizer(2, 6, 8, 8);

        m_tickText = new wxTextCtrl(this, wxID_ANY, wxString::Format("%u", static_cast<unsigned>(tick)));
        m_valueSpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, maxValue, value);
        m_durationText = new wxTextCtrl(this, wxID_ANY, wxString::Format("%u", static_cast<unsigned>(durationTicks)));
        m_durationText->Enable(allowDurationEdit);

        gridSizer->Add(new wxStaticText(this, wxID_ANY, "Tick"), 0, wxALIGN_CENTER_VERTICAL);
        gridSizer->Add(m_tickText, 1, wxEXPAND);
        gridSizer->Add(new wxStaticText(this, wxID_ANY, valueLabel), 0, wxALIGN_CENTER_VERTICAL);
        gridSizer->Add(m_valueSpin, 1, wxEXPAND);
        gridSizer->Add(new wxStaticText(this, wxID_ANY, "Duration"), 0, wxALIGN_CENTER_VERTICAL);
        gridSizer->Add(m_durationText, 1, wxEXPAND);
        gridSizer->AddGrowableCol(1, 1);

        rootSizer->Add(gridSizer, 1, wxEXPAND | wxALL, 12);
        rootSizer->Add(CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
        SetSizerAndFit(rootSizer);
    }

    bool GetValues(uint32_t *outTick, int *outValue, uint32_t *outDurationTicks) const {
        unsigned long long tick;
        unsigned long long durationTicks;

        if (!outTick || !outValue || !outDurationTicks) {
            return false;
        }
        if (!m_tickText->GetValue().ToULongLong(&tick) ||
            !m_durationText->GetValue().ToULongLong(&durationTicks) ||
            durationTicks == 0) {
            return false;
        }
        *outTick = static_cast<uint32_t>(tick);
        *outValue = m_valueSpin->GetValue();
        *outDurationTicks = static_cast<uint32_t>(durationTicks);
        return true;
    }

private:
    wxTextCtrl *m_tickText;
    wxSpinCtrl *m_valueSpin;
    wxTextCtrl *m_durationText;
};

class PianoRollPanel final : public wxScrolledWindow {
public:
    explicit PianoRollPanel(wxWindow *parent)
        : wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                           wxBORDER_SIMPLE | wxVSCROLL | wxHSCROLL),
          m_document(nullptr),
          m_selectedTrack(-1),
          m_selectedNote(-1),
          m_dragging(false),
          m_dragMode(DragMode::None),
          m_showPlayhead(false),
          m_playheadTick(0),
          m_selectedItemKind(PianoRollSelectionKind::None),
          m_selectedAutomationLane(-1),
          m_selectedAutomationEvent(-1),
          m_hoverAutomationLane(-1),
          m_hoverAutomationEvent(-1),
          m_dragAutomationValid(false),
          m_dragStartTick(0),
          m_dragStartNote(0),
          m_newNoteBank(0),
                    m_newNoteProgram(0),
                    m_zoomScale(1.0f),
          m_lastPreviewDragNote(-1),
          m_userEndTick(0),
          m_draggingTimelineEnd(false),
          m_timelineEndDragTick(0),
          m_theme(DetectSystemDarkMode() ? MakeDarkTheme() : MakeLightTheme()) {
                m_rc.penNoteNormal    = wxPen(wxColour(34, 72, 120), 1);
        
        m_rc.penNoteSelected  = wxPen(wxColour(110, 42, 17), 2);
        m_rc.brushNoteNormal  = wxBrush(wxColour(73, 135, 210));
        m_rc.brushNoteSelected= wxBrush(wxColour(216, 106, 58));
        m_rc.penAutomationBorder = wxPen(m_theme.automationLaneBorder);
        m_rc.penAutomationSelectedBorder = wxPen(m_theme.automationSelectedBorder, 2);
        m_rc.brushAutomationBg = wxBrush(m_theme.automationLaneBg);
        m_rc.brushAutomationGutter = wxBrush(m_theme.automationLaneGutterBg);
        m_rc.penTimelineEnd = wxPen(wxColour(180,130,50), 2);
        m_rc.brushTimelineEnd = wxBrush(wxColour(200,155,60));
        m_rc.penLoopLine = wxPen(wxColour(72,160,105), 1);
        m_rc.brushLoopFill = wxBrush(wxColour(72,160,105));
        m_rc.brushWhiteKey = wxBrush(m_theme.whiteKeyFill);
        m_rc.brushBlackKey = wxBrush(m_theme.blackKeyFill);
        m_rc.penNoteSeparator = wxPen(m_theme.noteSeparator);
        m_rc.penGutterSeparator = wxPen(m_theme.gutterSeparator);
        m_rc.brushRulerBg = wxBrush(m_theme.rulerBg);
        m_rc.brushRulerGutterBg = wxBrush(m_theme.rulerGutterBg);
        m_rc.penRulerSubTick = wxPen(m_theme.rulerSubTick);
        m_rc.penRulerBarTick = wxPen(m_theme.rulerBarTick);
        m_rc.penRulerBeatTick = wxPen(m_theme.rulerBeatTick);
        m_rc.penRulerBorder = wxPen(m_theme.rulerBorder);
        m_rc.penRulerGutterBorder = wxPen(m_theme.rulerGutterBorder);
        m_rc.penPlayhead = wxPen(wxColour(210, 40, 40), 2);
        m_rc.brushSelectionBox = wxBrush(wxColour(73, 135, 210, 48));
        m_rc.penSelectionBoxBorder = wxPen(wxColour(45, 98, 168), 1);
        m_rc.penTimelineHandle = wxPen(wxColour(140, 100, 35));
        m_rc.brushTimelineHandleFill = wxBrush(wxColour(200, 155, 60));
        m_rc.brushMidiLoopShade = wxBrush(wxColour(88, 180, 120, 26));
        
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetScrollRate(16, 16);
        Bind(wxEVT_PAINT, &PianoRollPanel::OnPaint, this);
        Bind(wxEVT_LEFT_DOWN, &PianoRollPanel::OnLeftDown, this);
        Bind(wxEVT_LEFT_UP, &PianoRollPanel::OnLeftUp, this);
        Bind(wxEVT_LEFT_DCLICK, &PianoRollPanel::OnLeftDoubleClick, this);
        Bind(wxEVT_RIGHT_DOWN, &PianoRollPanel::OnRightDown, this);
        Bind(wxEVT_MOTION, &PianoRollPanel::OnMotion, this);
        Bind(wxEVT_LEAVE_WINDOW, &PianoRollPanel::OnMouseLeave, this);
        Bind(wxEVT_MOUSEWHEEL, &PianoRollPanel::OnMouseWheel, this);
        Bind(wxEVT_CHAR_HOOK, &PianoRollPanel::OnCharHook, this);
        Bind(wxEVT_MENU, &PianoRollPanel::OnEditSelectedItem, this, ID_PianoRollEdit);
        Bind(wxEVT_MENU, &PianoRollPanel::OnDeleteSelectedItem, this, ID_PianoRollDelete);
        Bind(wxEVT_MENU, &PianoRollPanel::OnToggleRampSegment, this, ID_PianoRollToggleRamp);
        UpdateVirtualSize();
    }

    void SetDocument(BAERmfEditorDocument *document) {
        m_document = document;
        m_selectedTrack = -1;
        m_selectedNote = -1;
        m_selectedNotes.clear();
        m_selectedItemKind = PianoRollSelectionKind::None;
        m_selectedAutomationLane = -1;
        m_selectedAutomationEvent = -1;
        m_hoverAutomationLane = -1;
        m_hoverAutomationEvent = -1;
        m_rampSegments.clear();
        m_dragAutomationValid = false;
        m_dragging = false;
        m_dragMode = DragMode::None;
        m_selectBoxActive = false;
        m_dragOriginalNoteIndices.clear();
        m_dragOriginalNotes.clear();
        m_userEndTick = 0;
        m_draggingTimelineEnd = false;
        InvalidateNoteCache();
        InvalidateAutomationCaches();
        UpdateVirtualSize();
        ScrollToMidiContentCenter();
        Refresh();
    }

    void SetSelectedTrack(int trackIndex) {
        m_selectedTrack = trackIndex;
        m_selectedNote = -1;
        m_selectedNotes.clear();
        m_selectedItemKind = PianoRollSelectionKind::None;
        m_selectedAutomationLane = -1;
        m_selectedAutomationEvent = -1;
        m_hoverAutomationLane = -1;
        m_hoverAutomationEvent = -1;
        m_rampSegments.clear();
        m_dragAutomationValid = false;
        m_dragging = false;
        m_dragMode = DragMode::None;
        m_selectBoxActive = false;
        m_dragOriginalNoteIndices.clear();
        m_dragOriginalNotes.clear();
        InvalidateNoteCache();
        InvalidateAutomationCaches();
        UpdateVirtualSize();
        ScrollToMidiContentCenter();
        Refresh();
    }

    void SetSelectionChangedCallback(std::function<void()> callback) {
        m_selectionChangedCallback = std::move(callback);
    }

    void SetSeekRequestedCallback(std::function<void(uint32_t)> callback) {
        m_seekRequestedCallback = std::move(callback);
    }

    void SetNotePreviewRequestedCallback(std::function<void(uint16_t,
                                                            unsigned char,
                                                            unsigned char,
                                                            unsigned char,
                                                            uint32_t,
                                                            int)> callback) {
        m_notePreviewRequestedCallback = std::move(callback);
    }

    void SetNotePreviewStopRequestedCallback(std::function<void()> callback) {
        m_notePreviewStopRequestedCallback = std::move(callback);
    }

    void SetUndoCallbacks(std::function<void(wxString const &)> beginCallback,
                          std::function<void(wxString const &)> commitCallback,
                          std::function<void()> cancelCallback) {
        m_beginUndoCallback = std::move(beginCallback);
        m_commitUndoCallback = std::move(commitCallback);
        m_cancelUndoCallback = std::move(cancelCallback);
    }

    void SetMidiLoopMarkers(bool enabled, uint32_t startTick, uint32_t endTick) {
        if (!enabled) {
            if (m_midiLoopEnabled) {
                m_midiLoopEnabled = false;
                Refresh();
            }
            return;
        }
        if (endTick <= startTick) {
            endTick = startTick + 1;
        }
        if (!m_midiLoopEnabled || m_midiLoopStartTick != startTick || m_midiLoopEndTick != endTick) {
            m_midiLoopEnabled = true;
            m_midiLoopStartTick = startTick;
            m_midiLoopEndTick = endTick;
            Refresh();
        }
    }

    void SetMidiLoopEditCallback(std::function<bool(bool, uint32_t, uint32_t)> callback) {
        m_midiLoopEditCallback = std::move(callback);
    }

    void RefreshFromDocument(bool clearSelection) {
        if (clearSelection) {
            m_selectedItemKind = PianoRollSelectionKind::None;
            m_selectedNote = -1;
            m_selectedNotes.clear();
            m_selectedAutomationLane = -1;
            m_selectedAutomationEvent = -1;
            m_hoverAutomationLane = -1;
            m_hoverAutomationEvent = -1;
        }
        m_dragging = false;
        m_dragAutomationValid = false;
        m_dragMode = DragMode::None;
        m_selectBoxActive = false;
        m_dragOriginalNoteIndices.clear();
        m_dragOriginalNotes.clear();
        m_dragUndoActive = false;
        m_dragUndoLabel.clear();
        InvalidateNoteCache();
        InvalidateAutomationCaches();
        UpdateVirtualSize();
        Refresh();
        if (m_selectionChangedCallback) {
            m_selectionChangedCallback();
        }
    }

    bool GetSelectedNoteInfo(BAERmfEditorNoteInfo *outNoteInfo) const {
        long noteIndex;

        if (!outNoteInfo || !HasTrack()) {
            return false;
        }
        noteIndex = m_selectedNote;
        if (noteIndex < 0 && !m_selectedNotes.empty()) {
            noteIndex = m_selectedNotes.front();
        }
        if (noteIndex < 0) {
            return false;
        }
        return BAERmfEditorDocument_GetNoteInfo(m_document,
                                                static_cast<uint16_t>(m_selectedTrack),
                                                static_cast<uint32_t>(noteIndex),
                                                outNoteInfo) == BAE_NO_ERROR;
    }

    void SetSelectedNoteInstrument(uint16_t bank, unsigned char program) {
        std::vector<long> noteIndices;
        bool anyChanged;

        if (!HasTrack()) {
            return;
        }
        noteIndices = m_selectedNotes;
        if (noteIndices.empty() && m_selectedNote >= 0) {
            noteIndices.push_back(m_selectedNote);
        }
        if (noteIndices.empty()) {
            return;
        }
        BeginUndoAction("Change Note Instrument");
        anyChanged = false;
        for (long noteIndex : noteIndices) {
            BAERmfEditorNoteInfo noteInfo;

            if (noteIndex < 0) {
                continue;
            }
            if (BAERmfEditorDocument_GetNoteInfo(m_document,
                                                 static_cast<uint16_t>(m_selectedTrack),
                                                 static_cast<uint32_t>(noteIndex),
                                                 &noteInfo) != BAE_NO_ERROR) {
                continue;
            }
            noteInfo.bank = bank;
            noteInfo.program = program;
            if (BAERmfEditorDocument_SetNoteInfo(m_document,
                                                 static_cast<uint16_t>(m_selectedTrack),
                                                 static_cast<uint32_t>(noteIndex),
                                                 &noteInfo) == BAE_NO_ERROR) {
                anyChanged = true;
            }
        }
        if (anyChanged) {
            CommitUndoAction("Change Note Instrument");
            InvalidateNoteCache();
            Refresh();
        } else {
            CancelUndoAction();
        }
    }

    void SetNewNoteInstrument(uint16_t bank, unsigned char program) {
        m_newNoteBank = bank;
        m_newNoteProgram = program;
    }

    void SetPlayheadTick(uint32_t tick) {
        m_showPlayhead = true;
        m_playheadTick = tick;
        Refresh();
    }

    void EnsurePlayheadVisible(uint32_t tick) {
        int scrollPixelsX;
        int scrollPixelsY;
        int viewUnitsX;
        int viewUnitsY;
        int viewLeft;
        int visibleWidth;
        int playheadX;
        int followPadding;
        int followBoundary;
        int newViewLeft;
        int maxViewLeft;
        wxSize virtualSize;

        scrollPixelsX = 0;
        scrollPixelsY = 0;
        GetScrollPixelsPerUnit(&scrollPixelsX, &scrollPixelsY);
        if (scrollPixelsX <= 0) {
            return;
        }
        GetViewStart(&viewUnitsX, &viewUnitsY);
        viewLeft = viewUnitsX * scrollPixelsX;
        visibleWidth = GetClientSize().GetWidth();
        if (visibleWidth <= 0) {
            return;
        }

        playheadX = TickToX(tick);
        followPadding = std::max(48, visibleWidth / 5);
        followBoundary = viewLeft + visibleWidth - followPadding;
        if (playheadX <= followBoundary) {
            return;
        }

        virtualSize = GetVirtualSize();
        maxViewLeft = std::max(0, virtualSize.GetWidth() - visibleWidth);
        newViewLeft = std::min(maxViewLeft, playheadX - (visibleWidth - followPadding));
        Scroll(newViewLeft / scrollPixelsX, viewUnitsY);
    }

    void JumpToTick(uint32_t tick) {
        int scrollPixelsX;
        int scrollPixelsY;
        int viewUnitsX;
        int viewUnitsY;
        int visibleWidth;
        int targetX;
        int targetViewLeft;
        wxSize virtualSize;

        GetScrollPixelsPerUnit(&scrollPixelsX, &scrollPixelsY);
        if (scrollPixelsX <= 0) {
            return;
        }
        GetViewStart(&viewUnitsX, &viewUnitsY);
        visibleWidth = GetClientSize().GetWidth();
        if (visibleWidth <= 0) {
            return;
        }
        targetX = TickToX(tick);
        targetViewLeft = targetX - visibleWidth / 4;
        virtualSize = GetVirtualSize();
        targetViewLeft = std::clamp(targetViewLeft, 0, std::max(0, virtualSize.GetWidth() - visibleWidth));
        /* Preserve vertical scroll position; only change horizontal. */
        Scroll(targetViewLeft / scrollPixelsX, viewUnitsY);
    }

    void ClearPlayhead() {
        m_showPlayhead = false;
        Refresh();
    }

    uint32_t GetVisibleDocumentEndTick() const {
        return GetDocumentEndTick();
    }

    void SetUserEndTick(uint32_t tick) {
        m_userEndTick = tick;
        UpdateVirtualSize();
        Refresh();
    }

    uint32_t GetUserEndTick() const {
        return m_userEndTick;
    }

private:
    BAERmfEditorDocument *m_document;
    int m_selectedTrack;
    long m_selectedNote;
    std::vector<long> m_selectedNotes;
    bool m_dragging;
    DragMode m_dragMode;
    bool m_showPlayhead;
    uint32_t m_playheadTick;
    PianoRollSelectionKind m_selectedItemKind;
    int m_selectedAutomationLane;
    long m_selectedAutomationEvent;
    int m_hoverAutomationLane;
    long m_hoverAutomationEvent;
    bool m_dragAutomationValid;
    AutomationHitInfo m_dragAutomationHit;
    BAERmfEditorNoteInfo m_dragOriginalNote;
    uint32_t m_dragStartTick;
    int m_dragStartNote;
    bool m_midiLoopEnabled = false;
    uint32_t m_midiLoopStartTick = 0;
    uint32_t m_midiLoopEndTick = 0;
    bool m_draggingMidiLoop = false;
    MidiLoopDragMode m_midiLoopDragMode = MidiLoopDragMode::None;
    uint32_t m_originalMidiLoopStartTick = 0;
    uint32_t m_originalMidiLoopEndTick = 0;
    uint16_t m_newNoteBank;
    unsigned char m_newNoteProgram;
    std::function<void()> m_selectionChangedCallback;
    std::function<void(uint32_t)> m_seekRequestedCallback;
    std::function<void(uint16_t, unsigned char, unsigned char, unsigned char, uint32_t, int)> m_notePreviewRequestedCallback;
    std::function<void()> m_notePreviewStopRequestedCallback;
    std::function<void(wxString const &)> m_beginUndoCallback;
    std::function<void(wxString const &)> m_commitUndoCallback;
    std::function<void()> m_cancelUndoCallback;
    std::function<bool(bool, uint32_t, uint32_t)> m_midiLoopEditCallback;
    bool m_dragUndoActive = false;
    wxString m_dragUndoLabel;
    float m_zoomScale;
    bool m_selectBoxActive = false;
    wxPoint m_selectBoxStart;
    wxPoint m_selectBoxCurrent;
    std::vector<long> m_dragOriginalNoteIndices;
    std::vector<BAERmfEditorNoteInfo> m_dragOriginalNotes;
    int m_lastPreviewDragNote;
    uint32_t m_userEndTick;
    bool m_draggingTimelineEnd;
    uint32_t m_timelineEndDragTick;
    PianoRollTheme m_theme;
    NoteTrackCache m_noteTrackCache;
    uint64_t m_automationRevision = 1;
    std::vector<AutomationLaneCache> m_automationLaneCaches;
    std::vector<AutomationDisplayCache> m_automationDisplayCaches;
    std::vector<AutomationRampSegmentKey> m_rampSegments;
    RenderCache m_rc;
    
    void InvalidateNoteCache() {
        m_noteTrackCache.valid = false;
        m_noteTrackCache.trackIndex = -1;
        m_noteTrackCache.ticksPerBin = 0;
        m_noteTrackCache.notes.clear();
        m_noteTrackCache.pitchBins.clear();
    }

    bool BuildNoteTrackCache() {
        if (!HasTrack()) { InvalidateNoteCache(); return false; }

        uint32_t noteCount = 0;
        if (BAERmfEditorDocument_GetNoteCount(m_document,
                                              static_cast<uint16_t>(m_selectedTrack),
                                              &noteCount) != BAE_NO_ERROR) {
            InvalidateNoteCache(); return false;
        }

        m_noteTrackCache.valid = false;
        m_noteTrackCache.trackIndex = m_selectedTrack;
        m_noteTrackCache.ticksPerBin = std::max<uint32_t>(GetTicksPerQuarter(), 1);
        m_noteTrackCache.notes.clear();
        m_noteTrackCache.notes.reserve(noteCount);
        uint32_t maxEndTick = m_noteTrackCache.ticksPerBin;

        for (uint32_t i = 0; i < noteCount; ++i) {
            BAERmfEditorNoteInfo info;
            if (!GetNoteInfo(i, &info)) continue;
            NoteCacheEntry e{info, i,
                             info.startTick + std::max<uint32_t>(1, info.durationTicks)};
            maxEndTick = std::max(maxEndTick, e.endTick);
            m_noteTrackCache.notes.push_back(e);
        }

        // ---- Pitch‑bin allocation – reserve once, then fill ----
        const uint32_t binCount = std::max<uint32_t>(1,
                    (maxEndTick / m_noteTrackCache.ticksPerBin) + 1);
        m_noteTrackCache.pitchBins.assign(128, {});
        for (auto &vec : m_noteTrackCache.pitchBins)
            vec.resize(binCount);

        for (uint32_t idx = 0; idx < m_noteTrackCache.notes.size(); ++idx) {
            const auto &e = m_noteTrackCache.notes[idx];
            const uint32_t startBin = e.noteInfo.startTick / m_noteTrackCache.ticksPerBin;
            const uint32_t endBin   = (e.endTick - 1) / m_noteTrackCache.ticksPerBin;
            for (uint32_t b = startBin; b <= endBin; ++b)
                m_noteTrackCache.pitchBins[e.noteInfo.note][b].push_back(idx);
        }

        m_noteTrackCache.valid = true;
        return true;
    }


    NoteTrackCache const *GetNoteTrackCache() {
        if (!HasTrack()) {
            return nullptr;
        }
        if (!m_noteTrackCache.valid || m_noteTrackCache.trackIndex != m_selectedTrack) {
            if (!BuildNoteTrackCache()) {
                return nullptr;
            }
        }
        return &m_noteTrackCache;
    }

    void GatherNoteEntriesInRect(wxRect const &rect, std::vector<uint32_t> *outEntryIndices) {
        NoteTrackCache const *cache;
        uint32_t visibleStartTick;
        uint32_t visibleEndTick;
        uint32_t startBin;
        uint32_t endBin;
        int topNote;
        int bottomNote;
        int lowNote;
        int highNote;
        std::vector<unsigned char> seen;

        if (!outEntryIndices) {
            return;
        }
        outEntryIndices->clear();
        cache = GetNoteTrackCache();
        if (!cache || cache->notes.empty() || cache->pitchBins.empty()) {
            return;
        }

        visibleStartTick = XToTick(std::max(kPianoRollLeftGutter, rect.GetLeft()));
        visibleEndTick = XToTick(std::max(kPianoRollLeftGutter, rect.GetRight()));
        startBin = std::min<uint32_t>(visibleStartTick / cache->ticksPerBin,
                                      static_cast<uint32_t>(cache->pitchBins[0].size() - 1));
        endBin = std::min<uint32_t>(visibleEndTick / cache->ticksPerBin,
                                    static_cast<uint32_t>(cache->pitchBins[0].size() - 1));
        topNote = std::clamp(YToNote(rect.GetTop()), 0, 127);
        bottomNote = std::clamp(YToNote(rect.GetBottom()), 0, 127);
        lowNote = std::min(topNote, bottomNote);
        highNote = std::max(topNote, bottomNote);
        
        for (int pitch = lowNote; pitch <= highNote; ++pitch) {
            for (uint32_t binIndex = startBin; binIndex <= endBin; ++binIndex) {
                for (uint32_t entryIndex : cache->pitchBins[static_cast<size_t>(pitch)][binIndex]) {
                    NoteCacheEntry const &entry = cache->notes[entryIndex];
                    if (entry.endTick <= visibleStartTick || entry.noteInfo.startTick > visibleEndTick)
                        continue;
                    outEntryIndices->push_back(entryIndex);
                }
            }
        }
    }

    bool IsRampSegmentKey(int laneIndex, uint32_t tick, int value) const {
        for (AutomationRampSegmentKey const &key : m_rampSegments) {
            if (key.laneIndex == laneIndex && key.tick == tick && key.value == value) {
                return true;
            }
        }
        return false;
    }

    void ToggleRampSegmentKey(int laneIndex, uint32_t tick, int value) {
        for (size_t i = 0; i < m_rampSegments.size(); ++i) {
            AutomationRampSegmentKey const &key = m_rampSegments[i];
            if (key.laneIndex == laneIndex && key.tick == tick && key.value == value) {
                m_rampSegments.erase(m_rampSegments.begin() + static_cast<long>(i));
                return;
            }
        }
        m_rampSegments.push_back({ laneIndex, tick, value });
    }

    bool ToggleRampForSelectedAutomationSegment() {
        uint32_t documentEndTick;
        AutomationLaneCache const *laneCache;

        if (m_selectedItemKind != PianoRollSelectionKind::Automation ||
            m_selectedAutomationLane < 0 || m_selectedAutomationEvent < 0) {
            return false;
        }
        documentEndTick = GetDocumentEndTick();
        laneCache = GetAutomationLaneCache(m_selectedAutomationLane, documentEndTick);
        if (!laneCache) {
            return false;
        }
        for (AutomationCurveSegment const &segment : laneCache->segments) {
            if (segment.sourceEventIndex != static_cast<uint32_t>(m_selectedAutomationEvent)) {
                continue;
            }
            if (!segment.hasFollowingEvent) {
                return false;
            }
            ToggleRampSegmentKey(m_selectedAutomationLane, segment.startTick, segment.startValue);
            InvalidateAutomationCaches();
            RefreshAutomationLane(m_selectedAutomationLane);
            return true;
        }
        return false;
    }

    void EnsureAutomationCacheStorage() {
        size_t laneCount;

        laneCount = GetAutomationLanes().size();
        if (m_automationLaneCaches.size() != laneCount) {
            m_automationLaneCaches.clear();
            m_automationLaneCaches.resize(laneCount);
        }
        if (m_automationDisplayCaches.size() != laneCount) {
            m_automationDisplayCaches.clear();
            m_automationDisplayCaches.resize(laneCount);
            for (size_t laneIndex = 0; laneIndex < m_automationDisplayCaches.size(); ++laneIndex) {
                m_automationDisplayCaches[laneIndex].laneIndex = static_cast<int>(laneIndex);
            }
        }
    }

    void InvalidateAutomationCaches() {
        ++m_automationRevision;
        EnsureAutomationCacheStorage();
        for (AutomationLaneCache &cache : m_automationLaneCaches) {
            cache.valid = false;
            cache.nodes.clear();
            cache.segments.clear();
        }
        for (AutomationDisplayCache &cache : m_automationDisplayCaches) {
            cache.valid = false;
            cache.buckets.clear();
        }
    }

    AutomationDisplayCache const *GetAutomationDisplayCache(int laneIndex,
                                                            int visibleLeft,
                                                            int visibleRight,
                                                            int laneTop,
                                                            int laneBottom,
                                                            AutomationLaneDescriptor const &lane,
                                                            AutomationLaneCache const *laneCache) {
        AutomationDisplayCache *cache;
        int bucketCount;

        EnsureAutomationCacheStorage();
        if (laneIndex < 0 || laneIndex >= static_cast<int>(m_automationDisplayCaches.size()) || !laneCache) {
            return nullptr;
        }
        cache = &m_automationDisplayCaches[static_cast<size_t>(laneIndex)];
        
        if (cache->valid &&
            cache->revision == m_automationRevision &&
            cache->visibleLeft == visibleLeft &&
            cache->visibleRight == visibleRight &&
            cache->pixelsPerQuarter == GetPixelsPerQuarter()) {
            return cache;
        }

        bucketCount = std::max(1, visibleRight - visibleLeft + 1);
        cache->buckets.resize(bucketCount, {});
        for (int bucketIndex = 0; bucketIndex < bucketCount; ++bucketIndex) {
            AutomationDisplayBucket &bucket = cache->buckets[static_cast<size_t>(bucketIndex)];

            bucket.x = visibleLeft + bucketIndex;
            bucket.hasData = false;
        }

        auto valueToY = [&](int value) -> int {
            int clampedValue;

            clampedValue = std::clamp(value, 0, lane.maxValue);
            return laneBottom - 1 - ((clampedValue * (kAutomationLaneHeight - 1)) / std::max(1, lane.maxValue));
        };

        uint32_t visibleStartTick;
        uint32_t visibleEndTick;

        visibleStartTick = XToTick(std::max(kPianoRollLeftGutter, visibleLeft));
        visibleEndTick = XToTick(std::max(kPianoRollLeftGutter, visibleRight));
        auto segmentIt = std::lower_bound(laneCache->segments.begin(),
                                          laneCache->segments.end(),
                                          visibleStartTick,
                                          [](AutomationCurveSegment const &segment, uint32_t tick) {
                                              return segment.startTick < tick;
                                          });
        if (segmentIt != laneCache->segments.begin()) {
            --segmentIt;
        }
        while (segmentIt != laneCache->segments.end() && segmentIt->endTick <= visibleStartTick) {
            ++segmentIt;
        }

        for (; segmentIt != laneCache->segments.end(); ++segmentIt) {
            AutomationCurveSegment const &segment = *segmentIt;
            int x0;
            int x1;
            int y0;
            int y1;
            int startBucket;
            int endBucket;

            if (segment.startTick > visibleEndTick) {
                break;
            }
            x0 = TickToX(segment.startTick);
            x1 = TickToX(std::max(segment.startTick + 1, segment.endTick));
            if (x1 < visibleLeft || x0 > visibleRight) {
                continue;
            }
            y0 = valueToY(segment.startValue);
            y1 = valueToY(segment.endValue);
            startBucket = std::clamp(x0 - visibleLeft, 0, bucketCount - 1);
            endBucket = std::clamp(x1 - visibleLeft, 0, bucketCount - 1);
            for (int bucketIndex = startBucket; bucketIndex <= endBucket; ++bucketIndex) {
                AutomationDisplayBucket &bucket = cache->buckets[static_cast<size_t>(bucketIndex)];
                int sampleY;

                if (segment.interpolation == AutomationInterpolation::Linear && x1 != x0) {
                    sampleY = y0 + (((bucket.x - x0) * (y1 - y0)) / (x1 - x0));
                } else {
                    sampleY = y0;
                    if (segment.hasFollowingEvent && bucket.x == x1) {
                        sampleY = y1;
                    }
                }
                if (!bucket.hasData) {
                    bucket.minY = sampleY;
                    bucket.maxY = sampleY;
                    bucket.firstY = sampleY;
                    bucket.lastY = sampleY;
                    bucket.hasData = true;
                } else {
                    bucket.minY = std::min(bucket.minY, sampleY);
                    bucket.maxY = std::max(bucket.maxY, sampleY);
                    bucket.lastY = sampleY;
                }
            }
        }

        cache->valid = true;
        cache->revision = m_automationRevision;
        cache->visibleLeft = visibleLeft;
        cache->visibleRight = visibleRight;
        cache->pixelsPerQuarter = GetPixelsPerQuarter();
        return cache;
    }

    bool BuildAutomationLaneCache(int laneIndex, uint32_t documentEndTick) {
        std::vector<AutomationLaneDescriptor> const &lanes = GetAutomationLanes();
        AutomationLaneCache *cache;
        uint32_t eventCount;

        EnsureAutomationCacheStorage();
        if (!m_document || !IsAutomationLaneVisible(laneIndex) ||
            laneIndex < 0 || laneIndex >= static_cast<int>(m_automationLaneCaches.size())) {
            return false;
        }
        cache = &m_automationLaneCaches[static_cast<size_t>(laneIndex)];
        cache->nodes.clear();
        cache->segments.clear();

        eventCount = 0;
        if (!GetAutomationEventCount(laneIndex, &eventCount)) {
            cache->valid = false;
            return false;
        }

        if (lanes[static_cast<size_t>(laneIndex)].kind == AutomationLaneKind::Tempo && eventCount == 0) {
            uint32_t defaultBpm;
            AutomationCurveNode implicitNode;

            defaultBpm = 120;
            BAERmfEditorDocument_GetTempoBPM(m_document, &defaultBpm);
            implicitNode.tick = 0;
            implicitNode.value = static_cast<int>(defaultBpm);
            implicitNode.sourceEventIndex = 0;
            implicitNode.implicitNode = true;
            cache->nodes.push_back(implicitNode);
        } else {
            for (uint32_t eventIndex = 0; eventIndex < eventCount; ++eventIndex) {
                uint32_t tick;
                int value;
                AutomationCurveNode node;

                if (!GetAutomationEvent(laneIndex, eventIndex, &tick, &value)) {
                    continue;
                }
                node.tick = tick;
                node.value = value;
                node.sourceEventIndex = eventIndex;
                node.implicitNode = false;
                cache->nodes.push_back(node);
            }
            std::stable_sort(cache->nodes.begin(),
                             cache->nodes.end(),
                             [](AutomationCurveNode const &left, AutomationCurveNode const &right) {
                                 if (left.tick != right.tick) {
                                     return left.tick < right.tick;
                                 }
                                 return left.sourceEventIndex < right.sourceEventIndex;
                             });
        }

        for (size_t nodeIndex = 0; nodeIndex < cache->nodes.size(); ++nodeIndex) {
            AutomationCurveNode const &node = cache->nodes[nodeIndex];
            AutomationCurveSegment segment;

            segment.startTick = node.tick;
            segment.endTick = documentEndTick;
            segment.startValue = node.value;
            segment.endValue = node.value;
            segment.sourceEventIndex = node.sourceEventIndex;
            segment.hasFollowingEvent = false;
            segment.interpolation = IsRampSegmentKey(laneIndex, node.tick, node.value)
                                        ? AutomationInterpolation::Linear
                                        : AutomationInterpolation::Step;

            if (nodeIndex + 1 < cache->nodes.size()) {
                AutomationCurveNode const &nextNode = cache->nodes[nodeIndex + 1];

                segment.endTick = nextNode.tick;
                segment.endValue = nextNode.value;
                segment.hasFollowingEvent = !nextNode.implicitNode;
            }
            if (segment.endTick <= segment.startTick) {
                segment.endTick = segment.startTick + 1;
            }
            cache->segments.push_back(segment);
        }

        cache->documentEndTick = documentEndTick;
        cache->revision = m_automationRevision;
        cache->valid = true;
        return true;
    }

    AutomationLaneCache const *GetAutomationLaneCache(int laneIndex, uint32_t documentEndTick) {
        EnsureAutomationCacheStorage();
        if (laneIndex < 0 || laneIndex >= static_cast<int>(m_automationLaneCaches.size())) {
            return nullptr;
        }
        AutomationLaneCache &cache = m_automationLaneCaches[static_cast<size_t>(laneIndex)];
        if (!cache.valid || cache.revision != m_automationRevision || cache.documentEndTick != documentEndTick) {
            if (!BuildAutomationLaneCache(laneIndex, documentEndTick)) {
                return nullptr;
            }
        }
        return &cache;
    }

    bool ScrollHorizontallyWithWheel(wxMouseEvent const &event, int stepMultiplier) {
        int wheelDelta;
        int wheelRotation;
        int steps;
        int linesPerStep;
        int scrollPixelsX;
        int scrollPixelsY;
        int viewUnitsX;
        int viewUnitsY;
        int currentLeft;
        int clientWidth;
        int maxLeft;
        int newLeft;
        wxSize virtualSize;

        wheelDelta = event.GetWheelDelta();
        wheelRotation = event.GetWheelRotation();
        if (wheelDelta == 0 || wheelRotation == 0) {
            return false;
        }
        steps = wheelRotation / wheelDelta;
        if (steps == 0) {
            steps = (wheelRotation > 0) ? 1 : -1;
        }
        linesPerStep = std::max(1, event.GetLinesPerAction());
        GetScrollPixelsPerUnit(&scrollPixelsX, &scrollPixelsY);
        if (scrollPixelsX <= 0) {
            return false;
        }
        GetViewStart(&viewUnitsX, &viewUnitsY);
        currentLeft = viewUnitsX * scrollPixelsX;
        clientWidth = std::max(1, GetClientSize().GetWidth());
        virtualSize = GetVirtualSize();
        maxLeft = std::max(0, virtualSize.GetWidth() - clientWidth);
        newLeft = currentLeft - (steps * linesPerStep * scrollPixelsX * std::max(1, stepMultiplier));
        newLeft = std::clamp(newLeft, 0, maxLeft);
        Scroll(newLeft / scrollPixelsX, viewUnitsY);
        return true;
    }

    void RequestSingleNotePreview(BAERmfEditorNoteInfo const &noteInfo) {
        if (!m_notePreviewRequestedCallback || !HasTrack()) {
            return;
        }
        m_notePreviewRequestedCallback(noteInfo.bank,
                                       noteInfo.program,
                                       noteInfo.channel,
                                       noteInfo.note,
                                       noteInfo.durationTicks,
                                       m_selectedTrack);
    }

    void RequestStopNotePreview() {
        if (m_notePreviewStopRequestedCallback) {
            m_notePreviewStopRequestedCallback();
        }
    }

    static wxRect NormalizeRect(wxPoint const &a, wxPoint const &b) {
        int left;
        int top;
        int right;
        int bottom;

        left = std::min(a.x, b.x);
        top = std::min(a.y, b.y);
        right = std::max(a.x, b.x);
        bottom = std::max(a.y, b.y);
        return wxRect(left, top, std::max(1, right - left), std::max(1, bottom - top));
    }

    bool IsNoteSelected(long noteIndex) const {
        return std::find(m_selectedNotes.begin(), m_selectedNotes.end(), noteIndex) != m_selectedNotes.end();
    }

    void SelectSingleNote(long noteIndex) {
        m_selectedNotes.clear();
        if (noteIndex >= 0) {
            m_selectedNotes.push_back(noteIndex);
            m_selectedNote = noteIndex;
            m_selectedItemKind = PianoRollSelectionKind::Note;
        } else {
            m_selectedNote = -1;
            m_selectedItemKind = PianoRollSelectionKind::None;
        }
    }

    void UpdatePrimarySelectedNote() {
        if (!m_selectedNotes.empty()) {
            m_selectedNote = m_selectedNotes.front();
            m_selectedItemKind = PianoRollSelectionKind::Note;
        } else {
            m_selectedNote = -1;
            if (m_selectedItemKind == PianoRollSelectionKind::Note) {
                m_selectedItemKind = PianoRollSelectionKind::None;
            }
        }
    }

    void UpdateAreaSelection(wxRect const &selectionRect) {
        std::vector<uint32_t> visibleEntries;
        NoteTrackCache const *cache;

        m_selectedNotes.clear();
        if (!HasTrack()) {
            UpdatePrimarySelectedNote();
            return;
        }
        cache = GetNoteTrackCache();
        if (!cache) {
            UpdatePrimarySelectedNote();
            return;
        }
        GatherNoteEntriesInRect(selectionRect, &visibleEntries);
        for (uint32_t entryIndex : visibleEntries) {
            wxRect noteRect;

            noteRect = BuildNoteRect(cache->notes[entryIndex].noteInfo);
            if (noteRect.Intersects(selectionRect)) {
                m_selectedNotes.push_back(static_cast<long>(cache->notes[entryIndex].sourceIndex));
            }
        }
        UpdatePrimarySelectedNote();
    }

    void BeginUndoAction(wxString const &label) {
        if (m_beginUndoCallback) {
            m_beginUndoCallback(label);
        }
    }

    void CommitUndoAction(wxString const &label) {
        if (m_commitUndoCallback) {
            m_commitUndoCallback(label);
        }
    }

    void CancelUndoAction() {
        if (m_cancelUndoCallback) {
            m_cancelUndoCallback();
        }
    }

    void ApplyZoomStep(int wheelRotation, wxPoint clientPoint) {
        double oldScale;
        uint32_t anchorTick;
        int scrollPixelsX;
        int scrollPixelsY;
        int viewUnitsX;
        int viewUnitsY;
        int oldViewLeft;
        int oldViewTop;
        int logicalAnchorX;
        int newAnchorX;
        int newViewLeft;
        int maxViewLeft;
        int clientWidth;
        wxSize virtualSize;

        oldScale = m_zoomScale;
        if (wheelRotation > 0) {
            m_zoomScale = std::min(8.0f, m_zoomScale * 1.15f);
        } else {
            m_zoomScale = std::max(0.2f, m_zoomScale / 1.15f);
        }
        if (std::abs(m_zoomScale - oldScale) < 0.0001f) {
            return;
        }

        GetScrollPixelsPerUnit(&scrollPixelsX, &scrollPixelsY);
        if (scrollPixelsX <= 0) {
            scrollPixelsX = 1;
        }
        if (scrollPixelsY <= 0) {
            scrollPixelsY = 1;
        }
        GetViewStart(&viewUnitsX, &viewUnitsY);
        oldViewLeft = viewUnitsX * scrollPixelsX;
        oldViewTop = viewUnitsY * scrollPixelsY;
        logicalAnchorX = oldViewLeft + clientPoint.x;
        anchorTick = XToTick(logicalAnchorX);

        UpdateVirtualSize();
        newAnchorX = TickToX(anchorTick);
        newViewLeft = std::max(0, newAnchorX - clientPoint.x);
        clientWidth = GetClientSize().GetWidth();
        virtualSize = GetVirtualSize();
        maxViewLeft = std::max(0, virtualSize.GetWidth() - std::max(1, clientWidth));
        newViewLeft = std::clamp(newViewLeft, 0, maxViewLeft);
        Scroll(newViewLeft / scrollPixelsX, oldViewTop / scrollPixelsY);
        Refresh();
    }

    wxRect BuildNoteRect(BAERmfEditorNoteInfo const &noteInfo) const {
        return wxRect(TickToX(noteInfo.startTick),
                      NoteToY(noteInfo.note),
                      std::max(8, TickToX(noteInfo.startTick + noteInfo.durationTicks) - TickToX(noteInfo.startTick)),
                      kNoteHeight - 1);
    }

    DragMode GetDragModeForPoint(wxRect const &noteRect, wxPoint point) const {
        if (std::abs(point.x - noteRect.GetLeft()) <= kResizeHandlePixels) {
            return DragMode::ResizeLeft;
        }
        if (std::abs(point.x - noteRect.GetRight()) <= kResizeHandlePixels) {
            return DragMode::ResizeRight;
        }
        return DragMode::Move;
    }

    void UpdateHoverCursor(wxPoint point) {
        BAERmfEditorNoteInfo noteInfo;
        AutomationHitInfo automationNodeHit;
        AutomationHitInfo automationHit;
        long hitNote;
        MidiLoopDragMode loopDragMode;

        if (HitTestTimelineEndHandle(point)) {
            SetCursor(wxCursor(wxCURSOR_SIZEWE));
            return;
        }
        loopDragMode = MidiLoopDragMode::None;
        if (HitTestMidiLoopHandle(point, &loopDragMode)) {
            SetCursor(wxCursor(wxCURSOR_SIZEWE));
            return;
        }

        if (!HasTrack()) {
            SetCursor(wxNullCursor);
            return;
        }
        hitNote = HitTestNote(point, &noteInfo);
        if (hitNote < 0) {
            if (HitTestAutomationNode(point, &automationNodeHit)) {
                SetCursor(wxCursor(wxCURSOR_SIZING));
                return;
            }
            if (HitTestAutomation(point, &automationHit)) {
                DragMode mode;

                mode = GetDragModeForAutomation(automationHit, point);
                if (mode == DragMode::ResizeLeft || mode == DragMode::ResizeRight) {
                    SetCursor(wxCursor(wxCURSOR_SIZEWE));
                } else {
                    SetCursor(wxCursor(wxCURSOR_HAND));
                }
                return;
            }
            SetCursor(wxNullCursor);
            return;
        }
        wxRect noteRect = BuildNoteRect(noteInfo);
        DragMode mode = GetDragModeForPoint(noteRect, point);
        if (mode == DragMode::ResizeLeft || mode == DragMode::ResizeRight) {
            SetCursor(wxCursor(wxCURSOR_SIZEWE));
        } else {
            SetCursor(wxCursor(wxCURSOR_HAND));
        }
    }


    uint16_t GetTicksPerQuarter() const {
        uint16_t ticksPerQuarter;

        ticksPerQuarter = 480;
        if (m_document) {
            BAERmfEditorDocument_GetTicksPerQuarter(m_document, &ticksPerQuarter);
        }
        return ticksPerQuarter ? ticksPerQuarter : 480;
    }

    int GetPixelsPerQuarter() const {
        int pixels;

        /* Keep grid spacing tied to musical time (ticks), not playback tempo. */
        pixels = static_cast<int>(static_cast<double>(kBasePixelsPerQuarter) * m_zoomScale);
        return std::clamp(pixels, 16, 768);
    }

    int TickToX(uint32_t tick) const {
        return kPianoRollLeftGutter + static_cast<int>((static_cast<uint64_t>(tick) * GetPixelsPerQuarter()) / GetTicksPerQuarter());
    }

    uint32_t XToTick(int x) const {
        int localX;

        localX = std::max(0, x - kPianoRollLeftGutter);
        return static_cast<uint32_t>((static_cast<uint64_t>(localX) * GetTicksPerQuarter()) / GetPixelsPerQuarter());
    }

    int NoteToY(int note) const {
        return kPianoRollTopGutter + (127 - note) * kNoteHeight;
    }

    int YToNote(int y) const {
        int index;

        index = (y - kPianoRollTopGutter) / kNoteHeight;
        index = std::clamp(index, 0, 127);
        return 127 - index;
    }

    uint32_t SnapTick(uint32_t tick) const {
        uint32_t snapTicks;

        snapTicks = GetSnapTicks();
        return (tick / snapTicks) * snapTicks;
    }

    uint32_t GetSnapTicks() const {
        uint16_t ticksPerQuarter;

        ticksPerQuarter = GetTicksPerQuarter();
        return std::max<uint32_t>(1, static_cast<uint32_t>(ticksPerQuarter) / 4);
    }

    bool HasTrack() const {
        return m_document && m_selectedTrack >= 0;
    }

    bool GetNoteInfo(uint32_t noteIndex, BAERmfEditorNoteInfo *noteInfo) const {
        if (!HasTrack() || !noteInfo) {
            return false;
        }
        return BAERmfEditorDocument_GetNoteInfo(m_document, static_cast<uint16_t>(m_selectedTrack), noteIndex, noteInfo) == BAE_NO_ERROR;
    }

    bool IsAutomationLaneVisible(int laneIndex) const {
        std::vector<AutomationLaneDescriptor> const &lanes = GetAutomationLanes();

        return m_document && laneIndex >= 0 && laneIndex < static_cast<int>(lanes.size()) && (laneIndex == kTempoLaneIndex || HasTrack());
    }

    int AutomationValueFromY(int laneIndex, int y) const {
        std::vector<AutomationLaneDescriptor> const &lanes = GetAutomationLanes();
        AutomationLaneDescriptor const &lane = lanes[laneIndex];
        int laneTop;
        int laneBottom;
        int value;

        laneTop = GetAutomationLaneY(laneIndex);
        laneBottom = laneTop + kAutomationLaneHeight;
        y = std::clamp(y, laneTop, laneBottom - 1);
        value = ((laneBottom - 1 - y) * lane.maxValue) / std::max(1, kAutomationLaneHeight - 1);
        return std::clamp(value, 0, lane.maxValue);
    }

    int AutomationValueToY(int laneIndex, int value) const {
        std::vector<AutomationLaneDescriptor> const &lanes = GetAutomationLanes();
        AutomationLaneDescriptor const &lane = lanes[laneIndex];
        int laneBottom;
        int clampedValue;

        laneBottom = GetAutomationLaneY(laneIndex) + kAutomationLaneHeight;
        clampedValue = std::clamp(value, 0, lane.maxValue);
        return laneBottom - 1 - ((clampedValue * (kAutomationLaneHeight - 1)) / std::max(1, lane.maxValue));
    }

    bool HitTestAutomationNode(wxPoint pt, AutomationHitInfo *out) {
        if (!m_document || pt.y < GetAutomationAreaTop()) return false;

        const uint32_t docEnd = GetDocumentEndTick();
        const auto &lanes = GetAutomationLanes();

        for (int laneIdx = 0; laneIdx < static_cast<int>(lanes.size()); ++laneIdx) {
            if (!IsAutomationLaneVisible(laneIdx)) continue;
            const int laneTop = GetAutomationLaneY(laneIdx);
            const int laneBottom = laneTop + kAutomationLaneHeight;
            if (pt.y < laneTop - kAutomationNodeHitRadius ||
                pt.y >= laneBottom + kAutomationNodeHitRadius) continue;

            const AutomationLaneCache *cache = GetAutomationLaneCache(laneIdx, docEnd);
            if (!cache) continue;

            const auto toX = [&](uint32_t t){ return TickToX(t); };
            const auto toY = [&](int v){ return AutomationValueToY(laneIdx, v); };

            for (const auto &node : cache->nodes) {
                const int nx = toX(node.tick);
                const int ny = toY(node.value);
                if (std::abs(pt.x - nx) <= kAutomationNodeHitRadius &&
                    std::abs(pt.y - ny) <= kAutomationNodeHitRadius) {
                    if (out) {
                        out->laneIndex = laneIdx;
                        out->eventIndex = node.sourceEventIndex;
                        out->tick = node.tick;
                        out->endTick = docEnd;
                        out->laneTop = laneTop;
                        out->laneBottom = laneBottom;
                        out->leftX = nx;
                        out->rightX = nx;
                        out->value = node.value;
                        out->hasFollowingEvent = false;
                        for (const auto &seg : cache->segments) {
                            if (seg.sourceEventIndex == node.sourceEventIndex) {
                                out->endTick = seg.endTick;
                                out->hasFollowingEvent = seg.hasFollowingEvent;
                                break;
                            }
                        }
                    }
                    return true;
                }
            }
        }
        return false;
    }

    bool FindAutomationSegmentAtTick(AutomationLaneCache const *laneCache,
                                     uint32_t tick,
                                     AutomationCurveSegment const **outSegment) const {
        auto it = std::upper_bound(laneCache->segments.begin(),
                                   laneCache->segments.end(),
                                   tick,
                                   [](uint32_t needleTick, AutomationCurveSegment const &segment) {
                                       return needleTick < segment.startTick;
                                   });

        while (it != laneCache->segments.begin()) {
            --it;
            if (it->startTick <= tick && tick < it->endTick) {
                if (outSegment) {
                    *outSegment = &(*it);
                }
                return true;
            }
            if (it->endTick <= tick) {
                break;
            }
        }
        return false;
    }

    bool HitTestAutomation(wxPoint point, AutomationHitInfo *outHitInfo = nullptr) {
        std::vector<AutomationLaneDescriptor> const &lanes = GetAutomationLanes();
        uint32_t tick;
        uint32_t documentEndTick;
        int laneIndex;

        if (!m_document || point.y < GetAutomationAreaTop()) {
            return false;
        }
        tick = XToTick(point.x);
        documentEndTick = GetDocumentEndTick();
        for (laneIndex = 0; laneIndex < static_cast<int>(lanes.size()); ++laneIndex) {
            AutomationLaneCache const *laneCache;
            AutomationCurveSegment const *segment;
            int laneTop;
            int laneBottom;

            if (!IsAutomationLaneVisible(laneIndex)) {
                continue;
            }
            laneTop = GetAutomationLaneY(laneIndex);
            laneBottom = laneTop + kAutomationLaneHeight;
            if (point.y < laneTop || point.y >= laneBottom) {
                continue;
            }
            laneCache = GetAutomationLaneCache(laneIndex, documentEndTick);
            if (!laneCache) {
                continue;
            }
            segment = nullptr;
            if (FindAutomationSegmentAtTick(laneCache, tick, &segment) && segment) {
                int leftX;
                int rightX;

                leftX = TickToX(segment->startTick);
                rightX = TickToX(std::max(segment->startTick + 1, segment->endTick));
                if (outHitInfo) {
                    outHitInfo->laneIndex = laneIndex;
                    outHitInfo->eventIndex = segment->sourceEventIndex;
                    outHitInfo->tick = segment->startTick;
                    outHitInfo->endTick = segment->endTick;
                    outHitInfo->laneTop = laneTop;
                    outHitInfo->laneBottom = laneBottom;
                    outHitInfo->leftX = leftX;
                    outHitInfo->rightX = rightX;
                    outHitInfo->value = segment->startValue;
                    outHitInfo->hasFollowingEvent = segment->hasFollowingEvent;
                }
                return true;
            }
        }
        return false;
    }

    int GetAutomationLaneIndexFromY(int y) const {
        std::vector<AutomationLaneDescriptor> const &lanes = GetAutomationLanes();
        int laneIndex;

        for (laneIndex = 0; laneIndex < static_cast<int>(lanes.size()); ++laneIndex) {
            int laneTop;
            int laneBottom;

            if (!IsAutomationLaneVisible(laneIndex)) {
                continue;
            }
            laneTop = GetAutomationLaneY(laneIndex);
            laneBottom = laneTop + kAutomationLaneHeight;
            if (y >= laneTop && y < laneBottom) {
                return laneIndex;
            }
        }
        return -1;
    }

    bool GetAutomationEventCount(int laneIndex, uint32_t *outCount) const {
        std::vector<AutomationLaneDescriptor> const &lanes = GetAutomationLanes();

        if (!outCount || !m_document || !IsAutomationLaneVisible(laneIndex)) {
            return false;
        }
        if (lanes[laneIndex].kind == AutomationLaneKind::Tempo) {
            return BAERmfEditorDocument_GetTempoEventCount(m_document, outCount) == BAE_NO_ERROR;
        }
        if (lanes[laneIndex].kind == AutomationLaneKind::Controller) {
            return BAERmfEditorDocument_GetTrackCCEventCount(m_document,
                                                             static_cast<uint16_t>(m_selectedTrack),
                                                             lanes[laneIndex].controller,
                                                             outCount) == BAE_NO_ERROR;
        }
        return BAERmfEditorDocument_GetTrackPitchBendEventCount(m_document,
                                                                static_cast<uint16_t>(m_selectedTrack),
                                                                outCount) == BAE_NO_ERROR;
    }

    bool GetAutomationEvent(int laneIndex, uint32_t eventIndex, uint32_t *outTick, int *outValue) const {
        if (!outTick || !outValue || !m_document || !IsAutomationLaneVisible(laneIndex)) {
            return false;
        }
        std::vector<AutomationLaneDescriptor> const &lanes = GetAutomationLanes();

        if (lanes[laneIndex].kind == AutomationLaneKind::Tempo) {
            uint32_t microsecondsPerQuarter;

            if (BAERmfEditorDocument_GetTempoEvent(m_document, eventIndex, outTick, &microsecondsPerQuarter) != BAE_NO_ERROR || microsecondsPerQuarter == 0) {
                return false;
            }
            *outValue = static_cast<int>(60000000UL / microsecondsPerQuarter);
            return true;
        }
        if (lanes[laneIndex].kind == AutomationLaneKind::Controller) {
            unsigned char value;

            if (BAERmfEditorDocument_GetTrackCCEvent(m_document,
                                                     static_cast<uint16_t>(m_selectedTrack),
                                                     lanes[laneIndex].controller,
                                                     eventIndex,
                                                     outTick,
                                                     &value) != BAE_NO_ERROR) {
                return false;
            }
            *outValue = value;
            return true;
        }
        {
            uint16_t value;

            if (BAERmfEditorDocument_GetTrackPitchBendEvent(m_document,
                                                            static_cast<uint16_t>(m_selectedTrack),
                                                            eventIndex,
                                                            outTick,
                                                            &value) != BAE_NO_ERROR) {
                return false;
            }
            *outValue = static_cast<int>(value);
            return true;
        }
    }

    bool AddAutomationEvent(int laneIndex, uint32_t tick, int value) {
        if (!m_document || !IsAutomationLaneVisible(laneIndex)) {
            return false;
        }
        std::vector<AutomationLaneDescriptor> const &lanes = GetAutomationLanes();

        if (lanes[laneIndex].kind == AutomationLaneKind::Tempo) {
            if (BAERmfEditorDocument_AddTempoEvent(m_document,
                                                   tick,
                                                   static_cast<uint32_t>(60000000UL / std::max(1, value))) == BAE_NO_ERROR) {
                InvalidateAutomationCaches();
                return true;
            }
            return false;
        }
        if (lanes[laneIndex].kind == AutomationLaneKind::Controller) {
            if (BAERmfEditorDocument_AddTrackCCEvent(m_document,
                                                     static_cast<uint16_t>(m_selectedTrack),
                                                     lanes[laneIndex].controller,
                                                     tick,
                                                     static_cast<unsigned char>(value)) == BAE_NO_ERROR) {
                InvalidateAutomationCaches();
                return true;
            }
            return false;
        }
        if (BAERmfEditorDocument_AddTrackPitchBendEvent(m_document,
                                                        static_cast<uint16_t>(m_selectedTrack),
                                                        tick,
                                                        static_cast<uint16_t>(value)) == BAE_NO_ERROR) {
            InvalidateAutomationCaches();
            return true;
        }
        return false;
    }

    bool SetAutomationEvent(int laneIndex, uint32_t eventIndex, uint32_t tick, int value) {
        if (!m_document || !IsAutomationLaneVisible(laneIndex)) {
            return false;
        }
        std::vector<AutomationLaneDescriptor> const &lanes = GetAutomationLanes();

        if (lanes[laneIndex].kind == AutomationLaneKind::Tempo) {
            if (BAERmfEditorDocument_SetTempoEvent(m_document,
                                                   eventIndex,
                                                   tick,
                                                   static_cast<uint32_t>(60000000UL / std::max(1, value))) == BAE_NO_ERROR) {
                InvalidateAutomationCaches();
                return true;
            }
            return false;
        }
        if (lanes[laneIndex].kind == AutomationLaneKind::Controller) {
            if (BAERmfEditorDocument_SetTrackCCEvent(m_document,
                                                     static_cast<uint16_t>(m_selectedTrack),
                                                     lanes[laneIndex].controller,
                                                     eventIndex,
                                                     tick,
                                                     static_cast<unsigned char>(value)) == BAE_NO_ERROR) {
                InvalidateAutomationCaches();
                return true;
            }
            return false;
        }
        if (BAERmfEditorDocument_SetTrackPitchBendEvent(m_document,
                                                        static_cast<uint16_t>(m_selectedTrack),
                                                        eventIndex,
                                                        tick,
                                                        static_cast<uint16_t>(value)) == BAE_NO_ERROR) {
            InvalidateAutomationCaches();
            return true;
        }
        return false;
    }

    bool GetTrackCCValueAtTick(unsigned char cc, uint32_t tick, unsigned char *outValue) const {
        uint32_t count;

        if (!m_document || !HasTrack()) {
            return false;
        }
        count = 0;
        if (BAERmfEditorDocument_GetTrackCCEventCount(m_document,
                                                      static_cast<uint16_t>(m_selectedTrack),
                                                      cc,
                                                      &count) != BAE_NO_ERROR) {
            return false;
        }
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t eventTick;
            unsigned char eventValue;

            if (BAERmfEditorDocument_GetTrackCCEvent(m_document,
                                                     static_cast<uint16_t>(m_selectedTrack),
                                                     cc,
                                                     i,
                                                     &eventTick,
                                                     &eventValue) != BAE_NO_ERROR) {
                continue;
            }
            if (eventTick == tick) {
                if (outValue) {
                    *outValue = eventValue;
                }
                return true;
            }
        }
        return false;
    }

    bool UpsertTrackCCAtTick(unsigned char cc, uint32_t tick, unsigned char value) {
        uint32_t count;

        if (!m_document || !HasTrack()) {
            return false;
        }
        count = 0;
        if (BAERmfEditorDocument_GetTrackCCEventCount(m_document,
                                                      static_cast<uint16_t>(m_selectedTrack),
                                                      cc,
                                                      &count) != BAE_NO_ERROR) {
            return false;
        }
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t eventTick;
            unsigned char eventValue;

            if (BAERmfEditorDocument_GetTrackCCEvent(m_document,
                                                     static_cast<uint16_t>(m_selectedTrack),
                                                     cc,
                                                     i,
                                                     &eventTick,
                                                     &eventValue) != BAE_NO_ERROR) {
                continue;
            }
            if (eventTick == tick) {
                if (BAERmfEditorDocument_SetTrackCCEvent(m_document,
                                                         static_cast<uint16_t>(m_selectedTrack),
                                                         cc,
                                                         i,
                                                         tick,
                                                         value) == BAE_NO_ERROR) {
                    InvalidateAutomationCaches();
                    return true;
                }
                return false;
            }
        }
        if (BAERmfEditorDocument_AddTrackCCEvent(m_document,
                                                 static_cast<uint16_t>(m_selectedTrack),
                                                 cc,
                                                 tick,
                                                 value) == BAE_NO_ERROR) {
            InvalidateAutomationCaches();
            return true;
        }
        return false;
    }

    DragMode GetDragModeForAutomation(AutomationHitInfo const &hitInfo, wxPoint point) const {
        if (std::abs(point.x - hitInfo.leftX) <= kResizeHandlePixels) {
            return DragMode::ResizeLeft;
        }
        if (hitInfo.hasFollowingEvent && std::abs(point.x - hitInfo.rightX) <= kResizeHandlePixels) {
            return DragMode::ResizeRight;
        }
        return DragMode::Move;
    }

    bool EditSelectedNote() {
        BAERmfEditorNoteInfo noteInfo;
        BAERmfEditorTrackInfo trackInfo;
        bool hasResonance;
        unsigned char resonanceValue;
        bool hasBrightness;
        unsigned char brightnessValue;
        bool setResonance;
        bool setBrightness;

        if (!GetSelectedNoteInfo(&noteInfo)) {
            return false;
        }
        hasResonance = GetTrackCCValueAtTick(71, noteInfo.startTick, &resonanceValue);
        if (!hasResonance) {
            resonanceValue = 64;
        }
        hasBrightness = GetTrackCCValueAtTick(74, noteInfo.startTick, &brightnessValue);
        if (!hasBrightness) {
            brightnessValue = 64;
        }
        NoteEditDialog dialog(this,
                              noteInfo,
                              hasResonance,
                              resonanceValue,
                              hasBrightness,
                              brightnessValue);
        if (dialog.ShowModal() != wxID_OK) {
            return false;
        }
        if (!dialog.GetNoteInfo(&noteInfo)) {
            wxMessageBox("Invalid note values.", "Edit Note", wxOK | wxICON_ERROR, this);
            return false;
        }
        dialog.GetFilterSettings(&setResonance, &resonanceValue, &setBrightness, &brightnessValue);
        if ((setResonance || setBrightness) &&
            BAERmfEditorDocument_GetTrackInfo(m_document, static_cast<uint16_t>(m_selectedTrack), &trackInfo) == BAE_NO_ERROR &&
            trackInfo.channel != noteInfo.channel) {
            int answer;

            answer = wxMessageBox(wxString::Format("This note is on channel %u but the track channel is %u.\n"
                                                   "Filter CC events are track-channel events and will be applied to channel %u at this note tick.\n\n"
                                                   "Continue?",
                                                   static_cast<unsigned>(noteInfo.channel + 1),
                                                   static_cast<unsigned>(trackInfo.channel + 1),
                                                   static_cast<unsigned>(trackInfo.channel + 1)),
                                  "Edit Note",
                                  wxYES_NO | wxICON_WARNING,
                                  this);
            if (answer != wxYES) {
                return false;
            }
        }
        BeginUndoAction("Edit Note");
        if (BAERmfEditorDocument_SetNoteInfo(m_document,
                                             static_cast<uint16_t>(m_selectedTrack),
                                             static_cast<uint32_t>(m_selectedNote),
                                             &noteInfo) != BAE_NO_ERROR) {
            CancelUndoAction();
            wxMessageBox("Failed to update note.", "Edit Note", wxOK | wxICON_ERROR, this);
            return false;
        }
        if (setResonance && !UpsertTrackCCAtTick(71, noteInfo.startTick, resonanceValue)) {
            CancelUndoAction();
            wxMessageBox("Failed to set resonance automation (CC71).", "Edit Note", wxOK | wxICON_ERROR, this);
            return false;
        }
        if (setBrightness && !UpsertTrackCCAtTick(74, noteInfo.startTick, brightnessValue)) {
            CancelUndoAction();
            wxMessageBox("Failed to set brightness automation (CC74).", "Edit Note", wxOK | wxICON_ERROR, this);
            return false;
        }
        CommitUndoAction("Edit Note");
        InvalidateNoteCache();
        UpdateVirtualSize();
        Refresh();
        return true;
    }

    bool EditAutomationEvent(int laneIndex, uint32_t eventIndex) {
        std::vector<AutomationLaneDescriptor> const &lanes = GetAutomationLanes();
        AutomationLaneDescriptor const &lane = lanes[laneIndex];
        uint32_t eventCount;
        uint32_t durationTicks;
        bool hasFollowingEvent;
        uint32_t tick;
        int value;
        wxString valueLabel;
        wxString title;

        if (!m_document || !IsAutomationLaneVisible(laneIndex)) {
            return false;
        }
        eventCount = 0;
        durationTicks = GetSnapTicks();
        hasFollowingEvent = false;
        GetAutomationEventCount(laneIndex, &eventCount);
        if (lane.kind == AutomationLaneKind::Tempo) {
            uint32_t microsecondsPerQuarter;

            if (eventCount == 0) {
                /* No explicit tempo events: edit the implicit default tempo.
                 * On confirmation this converts it to an explicit tick-0 event. */
                uint32_t defaultBpm = 120;
                uint32_t newTick;
                int newBpm;
                uint32_t newDuration;

                BAERmfEditorDocument_GetTempoBPM(m_document, &defaultBpm);
                AutomationEditDialog dialog(this,
                                            "Edit Default Tempo",
                                            "BPM",
                                            0,
                                            static_cast<int>(defaultBpm),
                                            lane.maxValue,
                                            GetDocumentEndTick(),
                                            false);
                if (dialog.ShowModal() != wxID_OK) {
                    return false;
                }
                if (!dialog.GetValues(&newTick, &newBpm, &newDuration)) {
                    wxMessageBox("Invalid tempo values.", "Edit Default Tempo", wxOK | wxICON_ERROR, this);
                    return false;
                }
                BeginUndoAction("Edit Default Tempo");
                if (BAERmfEditorDocument_AddTempoEvent(m_document,
                                                       newTick,
                                                       static_cast<uint32_t>(60000000UL / std::max(1, newBpm))) == BAE_NO_ERROR) {
                    CommitUndoAction("Edit Default Tempo");
                    m_selectedAutomationEvent = 0;
                    if (m_selectionChangedCallback) {
                        m_selectionChangedCallback();
                    }
                    UpdateVirtualSize();
                    RefreshAutomationLane(laneIndex);
                    return true;
                }
                CancelUndoAction();
                wxMessageBox("Failed to set default tempo.", "Edit Default Tempo", wxOK | wxICON_ERROR, this);
                return false;
            }

            if (BAERmfEditorDocument_GetTempoEvent(m_document, eventIndex, &tick, &microsecondsPerQuarter) != BAE_NO_ERROR || microsecondsPerQuarter == 0) {
                return false;
            }
            value = static_cast<int>(60000000UL / microsecondsPerQuarter);
            valueLabel = "BPM";
            title = "Edit Tempo Event";
        } else if (lane.kind == AutomationLaneKind::Controller) {
            unsigned char controllerValue;

            if (BAERmfEditorDocument_GetTrackCCEvent(m_document,
                                                     static_cast<uint16_t>(m_selectedTrack),
                                                     lane.controller,
                                                     eventIndex,
                                                     &tick,
                                                     &controllerValue) != BAE_NO_ERROR) {
                return false;
            }
            value = controllerValue;
            valueLabel = "Value";
            title = wxString::Format("Edit %s Event", lane.label);
        } else {
            uint16_t bendValue;

            if (BAERmfEditorDocument_GetTrackPitchBendEvent(m_document,
                                                            static_cast<uint16_t>(m_selectedTrack),
                                                            eventIndex,
                                                            &tick,
                                                            &bendValue) != BAE_NO_ERROR) {
                return false;
            }
            value = static_cast<int>(bendValue);
            valueLabel = "Value";
            title = "Edit Pitch Bend Event";
        }
        if (eventIndex + 1 < eventCount) {
            uint32_t nextTick;
            int nextValue;

            if (GetAutomationEvent(laneIndex, eventIndex + 1, &nextTick, &nextValue) && nextTick > tick) {
                durationTicks = nextTick - tick;
                hasFollowingEvent = true;
            }
        }
        AutomationEditDialog dialog(this, title, valueLabel, tick, value, lane.maxValue, durationTicks, hasFollowingEvent);
        if (dialog.ShowModal() != wxID_OK) {
            return false;
        }
        if (!dialog.GetValues(&tick, &value, &durationTicks)) {
            wxMessageBox("Invalid automation values.", title, wxOK | wxICON_ERROR, this);
            return false;
        }
        BeginUndoAction(title);
        if (lane.kind == AutomationLaneKind::Tempo) {
            if (BAERmfEditorDocument_SetTempoEvent(m_document,
                                                   eventIndex,
                                                   tick,
                                                   static_cast<uint32_t>(60000000UL / std::max(1, value))) != BAE_NO_ERROR) {
                CancelUndoAction();
                wxMessageBox("Failed to update tempo event.", title, wxOK | wxICON_ERROR, this);
                return false;
            }
        } else if (lane.kind == AutomationLaneKind::Controller) {
            if (BAERmfEditorDocument_SetTrackCCEvent(m_document,
                                                     static_cast<uint16_t>(m_selectedTrack),
                                                     lane.controller,
                                                     eventIndex,
                                                     tick,
                                                     static_cast<unsigned char>(value)) != BAE_NO_ERROR) {
                CancelUndoAction();
                wxMessageBox("Failed to update automation event.", title, wxOK | wxICON_ERROR, this);
                return false;
            }
        } else {
            if (BAERmfEditorDocument_SetTrackPitchBendEvent(m_document,
                                                            static_cast<uint16_t>(m_selectedTrack),
                                                            eventIndex,
                                                            tick,
                                                            static_cast<uint16_t>(value)) != BAE_NO_ERROR) {
                CancelUndoAction();
                wxMessageBox("Failed to update pitch bend event.", title, wxOK | wxICON_ERROR, this);
                return false;
            }
        }
        if (hasFollowingEvent) {
            uint32_t nextTickCurrent;
            uint32_t nextNextTick;
            uint32_t minNextTick;
            uint32_t maxNextTick;
            uint32_t requestedNextTick;
            int nextValueCurrent;

            if (!GetAutomationEvent(laneIndex, eventIndex + 1, &nextTickCurrent, &nextValueCurrent)) {
                CancelUndoAction();
                wxMessageBox("Failed to read following event.", title, wxOK | wxICON_ERROR, this);
                return false;
            }

            nextNextTick = GetDocumentEndTick();
            if (eventIndex + 2 < eventCount) {
                int nextNextValue;
                GetAutomationEvent(laneIndex, eventIndex + 2, &nextNextTick, &nextNextValue);
            }

            uint32_t snapTicks;

            snapTicks = GetSnapTicks();
            minNextTick = tick + snapTicks;
            maxNextTick = minNextTick;
            if (nextNextTick > snapTicks) {
                maxNextTick = std::max(minNextTick, nextNextTick - snapTicks);
            }
            requestedNextTick = tick + std::max<uint32_t>(snapTicks, durationTicks);
            requestedNextTick = std::clamp(requestedNextTick, minNextTick, maxNextTick);

            if (!SetAutomationEvent(laneIndex, eventIndex + 1, requestedNextTick, nextValueCurrent)) {
                CancelUndoAction();
                wxMessageBox("Failed to update event duration.", title, wxOK | wxICON_ERROR, this);
                return false;
            }
        }
        CommitUndoAction(title);
        UpdateVirtualSize();
        RefreshAutomationLane(laneIndex);
        return true;
    }

    void DeleteCurrentSelection() {
        if (m_selectedItemKind == PianoRollSelectionKind::Note) {
            DeleteSelectedNote();
            return;
        }
        if (m_selectedItemKind == PianoRollSelectionKind::Automation && m_selectedAutomationLane >= 0 && m_selectedAutomationEvent >= 0) {
            BAEResult result;

            BeginUndoAction("Delete Event");
            std::vector<AutomationLaneDescriptor> const &lanes = GetAutomationLanes();

            if (lanes[m_selectedAutomationLane].kind == AutomationLaneKind::Tempo) {
                uint32_t tempoCount = 0;
                BAERmfEditorDocument_GetTempoEventCount(m_document, &tempoCount);
                if (tempoCount == 0) {
                    CancelUndoAction();
                    return; /* Cannot delete the implicit default tempo. */
                }
                result = BAERmfEditorDocument_DeleteTempoEvent(m_document, static_cast<uint32_t>(m_selectedAutomationEvent));
            } else if (lanes[m_selectedAutomationLane].kind == AutomationLaneKind::Controller) {
                result = BAERmfEditorDocument_DeleteTrackCCEvent(m_document,
                                                                 static_cast<uint16_t>(m_selectedTrack),
                                                                 lanes[m_selectedAutomationLane].controller,
                                                                 static_cast<uint32_t>(m_selectedAutomationEvent));
            } else {
                result = BAERmfEditorDocument_DeleteTrackPitchBendEvent(m_document,
                                                                        static_cast<uint16_t>(m_selectedTrack),
                                                                        static_cast<uint32_t>(m_selectedAutomationEvent));
            }
            if (result == BAE_NO_ERROR) {
                InvalidateAutomationCaches();
                CommitUndoAction("Delete Event");
                m_selectedItemKind = PianoRollSelectionKind::None;
                RefreshAutomationLane(m_selectedAutomationLane);
                m_selectedAutomationLane = -1;
                m_selectedAutomationEvent = -1;
                UpdateVirtualSize();
            } else {
                CancelUndoAction();
            }
        }
    }

    void OnEditSelectedItem(wxCommandEvent &) {
        if (m_selectedItemKind == PianoRollSelectionKind::Note) {
            EditSelectedNote();
        } else if (m_selectedItemKind == PianoRollSelectionKind::Automation && m_selectedAutomationLane >= 0 && m_selectedAutomationEvent >= 0) {
            EditAutomationEvent(m_selectedAutomationLane, static_cast<uint32_t>(m_selectedAutomationEvent));
        }
    }

    void OnDeleteSelectedItem(wxCommandEvent &) {
        DeleteCurrentSelection();
    }

    void OnToggleRampSegment(wxCommandEvent &) {
        if (ToggleRampForSelectedAutomationSegment()) {
            return;
        }
        wxBell();
    }

    int GetAutomationAreaTop() const {
        return kPianoRollTopGutter + (128 * kNoteHeight) + 10;
    }

    int GetAutomationLaneY(int laneIndex) const {
        return GetAutomationAreaTop() + laneIndex * (kAutomationLaneHeight + kAutomationLaneGap);
    }

    void ScrollToBottom() {
        int scrollPixelsX;
        int scrollPixelsY;
        int viewUnitsX;
        int viewUnitsY;
        int clientHeight;
        int targetTop;
        wxSize virtualSize;

        GetScrollPixelsPerUnit(&scrollPixelsX, &scrollPixelsY);
        if (scrollPixelsY <= 0) {
            return;
        }
        GetViewStart(&viewUnitsX, &viewUnitsY);
        clientHeight = GetClientSize().GetHeight();
        if (clientHeight <= 0) {
            return;
        }
        virtualSize = GetVirtualSize();
        targetTop = std::max(0, virtualSize.GetHeight() - clientHeight);
        Scroll(0, targetTop / scrollPixelsY);
    }

    void RefreshAutomationLane(int laneIndex, int padding = 8) {
        int scrollPixelsX;
        int scrollPixelsY;
        int viewUnitsX;
        int viewUnitsY;
        int viewTop;
        int clientWidth;
        int clientHeight;
        int laneTop;
        int laneBottom;
        int clientTop;
        int refreshTop;
        int refreshBottom;

        if (!IsAutomationLaneVisible(laneIndex)) {
            return;
        }
        GetScrollPixelsPerUnit(&scrollPixelsX, &scrollPixelsY);
        if (scrollPixelsY <= 0) {
            scrollPixelsY = 1;
        }
        GetViewStart(&viewUnitsX, &viewUnitsY);
        viewTop = viewUnitsY * scrollPixelsY;
        clientWidth = GetClientSize().GetWidth();
        clientHeight = GetClientSize().GetHeight();
        if (clientWidth <= 0 || clientHeight <= 0) {
            return;
        }
        laneTop = GetAutomationLaneY(laneIndex);
        laneBottom = laneTop + kAutomationLaneHeight;
        clientTop = laneTop - viewTop;
        refreshTop = std::max(0, clientTop - padding);
        refreshBottom = std::min(clientHeight, (laneBottom - viewTop) + padding);
        if (refreshBottom <= refreshTop) {
            return;
        }
        RefreshRect(wxRect(0, refreshTop, clientWidth, refreshBottom - refreshTop), false);
    }

    void RefreshAutomationLanes(int firstLane, int secondLane = -1) {
        if (firstLane >= 0) {
            RefreshAutomationLane(firstLane);
        }
        if (secondLane >= 0 && secondLane != firstLane) {
            RefreshAutomationLane(secondLane);
        }
    }

public:
    void ScrollToC4Center() {
        // C4 = MIDI note 60; scroll so it appears vertically centred
        int scrollPixelsX;
        int scrollPixelsY;
        int viewUnitsX;
        int clientHeight;
        int noteY;
        int targetTop;
        wxSize virtualSize;

        GetScrollPixelsPerUnit(&scrollPixelsX, &scrollPixelsY);
        if (scrollPixelsY <= 0) {
            return;
        }
        GetViewStart(&viewUnitsX, NULL);
        clientHeight = GetClientSize().GetHeight();
        if (clientHeight <= 0) {
            return;
        }
        noteY = NoteToY(60); // C4
        targetTop = noteY - (clientHeight / 2) + (kNoteHeight / 2);
        virtualSize = GetVirtualSize();
        targetTop = std::clamp(targetTop, 0, std::max(0, virtualSize.GetHeight() - clientHeight));
        Scroll(viewUnitsX, targetTop / scrollPixelsY);
    }

private:
    void ScrollToMidiContentCenter() {
        uint32_t noteCount;
        int minNote;
        int maxNote;
        int scrollPixelsX;
        int scrollPixelsY;
        int viewUnitsX;
        int viewUnitsY;
        int clientHeight;
        int contentTop;
        int contentBottom;
        int contentCenter;
        int targetTop;
        wxSize virtualSize;

        if (!HasTrack()) {
            ScrollToC4Center();
            return;
        }

        noteCount = 0;
        if (BAERmfEditorDocument_GetNoteCount(m_document, static_cast<uint16_t>(m_selectedTrack), &noteCount) != BAE_NO_ERROR || noteCount == 0) {
            ScrollToC4Center();
            return;
        }

        minNote = 127;
        maxNote = 0;
        for (uint32_t i = 0; i < noteCount; ++i) {
            BAERmfEditorNoteInfo noteInfo;
            if (GetNoteInfo(i, &noteInfo)) {
                minNote = std::min(minNote, static_cast<int>(noteInfo.note));
                maxNote = std::max(maxNote, static_cast<int>(noteInfo.note));
            }
        }

        GetScrollPixelsPerUnit(&scrollPixelsX, &scrollPixelsY);
        if (scrollPixelsY <= 0) {
            return;
        }
        GetViewStart(&viewUnitsX, &viewUnitsY);
        clientHeight = GetClientSize().GetHeight();
        if (clientHeight <= 0) {
            return;
        }

        contentTop = NoteToY(maxNote);
        contentBottom = NoteToY(minNote) + kNoteHeight;
        contentCenter = (contentTop + contentBottom) / 2;
        targetTop = contentCenter - (clientHeight / 2);

        virtualSize = GetVirtualSize();
        targetTop = std::clamp(targetTop, 0, std::max(0, virtualSize.GetHeight() - clientHeight));
        Scroll(viewUnitsX, targetTop / scrollPixelsY);
    }

    uint32_t GetContentEndTick() const {
        uint32_t lastTick;

        lastTick = GetTicksPerQuarter() * 8;
        if (!m_document) {
            return lastTick;
        }
        if (HasTrack()) {
            uint32_t noteCount;
            uint32_t noteIndex;

            noteCount = 0;
            if (BAERmfEditorDocument_GetNoteCount(m_document, static_cast<uint16_t>(m_selectedTrack), &noteCount) == BAE_NO_ERROR) {
                for (noteIndex = 0; noteIndex < noteCount; ++noteIndex) {
                    BAERmfEditorNoteInfo noteInfo;

                    if (GetNoteInfo(noteIndex, &noteInfo)) {
                        lastTick = std::max(lastTick, noteInfo.startTick + noteInfo.durationTicks + GetTicksPerQuarter());
                    }
                }
            }
            std::vector<AutomationLaneDescriptor> const &lanes = GetAutomationLanes();

            for (AutomationLaneDescriptor const &lane : lanes) {
                uint32_t eventCount;
                uint32_t eventIndex;

                if (lane.kind == AutomationLaneKind::Tempo) {
                    continue;
                }
                eventCount = 0;
                if (lane.kind == AutomationLaneKind::Controller) {
                    if (BAERmfEditorDocument_GetTrackCCEventCount(m_document,
                                                                  static_cast<uint16_t>(m_selectedTrack),
                                                                  lane.controller,
                                                                  &eventCount) != BAE_NO_ERROR) {
                        continue;
                    }
                } else {
                    if (BAERmfEditorDocument_GetTrackPitchBendEventCount(m_document,
                                                                         static_cast<uint16_t>(m_selectedTrack),
                                                                         &eventCount) != BAE_NO_ERROR) {
                        continue;
                    }
                }
                for (eventIndex = 0; eventIndex < eventCount; ++eventIndex) {
                    uint32_t tick;

                    if (lane.kind == AutomationLaneKind::Controller) {
                        unsigned char value;

                        if (BAERmfEditorDocument_GetTrackCCEvent(m_document,
                                                                 static_cast<uint16_t>(m_selectedTrack),
                                                                 lane.controller,
                                                                 eventIndex,
                                                                 &tick,
                                                                 &value) == BAE_NO_ERROR) {
                            lastTick = std::max(lastTick, tick + GetTicksPerQuarter());
                        }
                    } else {
                        uint16_t value;

                        if (BAERmfEditorDocument_GetTrackPitchBendEvent(m_document,
                                                                        static_cast<uint16_t>(m_selectedTrack),
                                                                        eventIndex,
                                                                        &tick,
                                                                        &value) == BAE_NO_ERROR) {
                            lastTick = std::max(lastTick, tick + GetTicksPerQuarter());
                        }
                    }
                }
            }
        }
        {
            uint32_t tempoCount;
            uint32_t tempoIndex;

            tempoCount = 0;
            if (BAERmfEditorDocument_GetTempoEventCount(m_document, &tempoCount) == BAE_NO_ERROR) {
                for (tempoIndex = 0; tempoIndex < tempoCount; ++tempoIndex) {
                    uint32_t tick;
                    uint32_t microsecondsPerQuarter;

                    if (BAERmfEditorDocument_GetTempoEvent(m_document, tempoIndex, &tick, &microsecondsPerQuarter) == BAE_NO_ERROR) {
                        lastTick = std::max(lastTick, tick + GetTicksPerQuarter());
                    }
                }
            }
        }
        return lastTick;
    }

    uint32_t GetDocumentEndTick() const {
        uint32_t contentEnd;

        contentEnd = GetContentEndTick();
        if (m_userEndTick > 0) {
            return std::max(contentEnd, m_userEndTick);
        }
        return contentEnd;
    }

    void ApplyTimelineResize(uint32_t newEndTick) {
        uint32_t minTick;
        uint16_t trackCount;
        bool applyAll;

        minTick = GetTicksPerQuarter() * 4;
        if (newEndTick < minTick) {
            newEndTick = minTick;
        }
        if (!m_document) {
            return;
        }
        trackCount = 0;
        BAERmfEditorDocument_GetTrackCount(m_document, &trackCount);

        /* Determine scope: all tracks or selected track only */
        applyAll = true;
        if (trackCount > 1 && HasTrack()) {
            wxMessageDialog dlg(this,
                                "Apply the new duration to all tracks, or only the selected track?",
                                "Resize Timeline",
                                wxYES_NO | wxCANCEL | wxICON_QUESTION);
            dlg.SetYesNoCancelLabels("All Tracks", "Selected Track", "Cancel");
            int choice = dlg.ShowModal();
            if (choice == wxID_CANCEL) {
                return;
            }
            applyAll = (choice == wxID_YES);
        }

        /* Shrink confirmation and trim for affected tracks */
        if (applyAll) {
            uint32_t contentEnd;

            contentEnd = GetContentEndTick();
            if (newEndTick < contentEnd) {
                int confirm;

                confirm = wxMessageBox(
                    "Shortening the timeline will delete or truncate notes and events "
                    "beyond the new end point.\n\nContinue?",
                    "Resize Timeline",
                    wxYES_NO | wxICON_WARNING);
                if (confirm != wxYES) {
                    return;
                }
                TrimContentToTick(newEndTick);
            }
            for (uint16_t i = 0; i < trackCount; ++i) {
                BAERmfEditorDocument_SetTrackEndOfTrackTick(m_document, i, newEndTick);
            }
        } else {
            uint32_t selectedTrackContentEnd;

            selectedTrackContentEnd = GetSelectedTrackContentEndTick();
            if (newEndTick < selectedTrackContentEnd) {
                int confirm;

                confirm = wxMessageBox(
                    "Shortening the timeline will delete or truncate notes and events "
                    "on the selected track beyond the new end point.\n\nContinue?",
                    "Resize Timeline",
                    wxYES_NO | wxICON_WARNING);
                if (confirm != wxYES) {
                    return;
                }
                TrimSingleTrackToTick(static_cast<uint16_t>(m_selectedTrack), newEndTick);
            }
            BAERmfEditorDocument_SetTrackEndOfTrackTick(m_document,
                                                        static_cast<uint16_t>(m_selectedTrack),
                                                        newEndTick);
        }
        m_userEndTick = newEndTick;
        InvalidateNoteCache();
        InvalidateAutomationCaches();
    }

    uint32_t GetSelectedTrackContentEndTick() const {
        uint32_t lastTick;
        uint32_t noteCount;
        uint32_t noteIndex;

        lastTick = 0;
        if (!HasTrack()) {
            return lastTick;
        }
        noteCount = 0;
        if (BAERmfEditorDocument_GetNoteCount(m_document, static_cast<uint16_t>(m_selectedTrack), &noteCount) == BAE_NO_ERROR) {
            for (noteIndex = 0; noteIndex < noteCount; ++noteIndex) {
                BAERmfEditorNoteInfo noteInfo;

                if (GetNoteInfo(noteIndex, &noteInfo)) {
                    lastTick = std::max(lastTick, noteInfo.startTick + noteInfo.durationTicks);
                }
            }
        }
        return lastTick;
    }

    void TrimSingleTrackToTick(uint16_t trackIndex, uint32_t boundary) {
        if (!m_document) {
            return;
        }
        BeginUndoAction("Resize Timeline");
        TrimTrackNotesToTick(trackIndex, boundary);
        TrimTrackCCEventsToBoundary(trackIndex, boundary);
        TrimTrackPitchBendEventsToBoundary(trackIndex, boundary);
        CommitUndoAction("Resize Timeline");
    }

    void TrimContentToTick(uint32_t boundary) {
        uint16_t trackCount;
        uint16_t trackIndex;

        if (!m_document) {
            return;
        }
        BeginUndoAction("Resize Timeline");
        trackCount = 0;
        BAERmfEditorDocument_GetTrackCount(m_document, &trackCount);
        for (trackIndex = 0; trackIndex < trackCount; ++trackIndex) {
            TrimTrackNotesToTick(trackIndex, boundary);
            TrimTrackCCEventsToBoundary(trackIndex, boundary);
            TrimTrackPitchBendEventsToBoundary(trackIndex, boundary);
        }
        TrimTempoEventsToBoundary(boundary);
        CommitUndoAction("Resize Timeline");
    }

    void TrimTrackNotesToTick(uint16_t trackIndex, uint32_t boundary) {
        uint32_t noteCount;
        uint32_t noteIndex;
        std::vector<uint32_t> toDelete;

        noteCount = 0;
        if (BAERmfEditorDocument_GetNoteCount(m_document, trackIndex, &noteCount) != BAE_NO_ERROR) {
            return;
        }
        for (noteIndex = 0; noteIndex < noteCount; ++noteIndex) {
            BAERmfEditorNoteInfo noteInfo;

            if (BAERmfEditorDocument_GetNoteInfo(m_document, trackIndex, noteIndex, &noteInfo) != BAE_NO_ERROR) {
                continue;
            }
            if (noteInfo.startTick >= boundary) {
                /* Entirely past boundary: mark for deletion */
                toDelete.push_back(noteIndex);
            } else if (noteInfo.startTick + noteInfo.durationTicks > boundary) {
                /* Clips boundary: truncate */
                noteInfo.durationTicks = boundary - noteInfo.startTick;
                if (noteInfo.durationTicks == 0) {
                    toDelete.push_back(noteIndex);
                } else {
                    BAERmfEditorDocument_SetNoteInfo(m_document, trackIndex, noteIndex, &noteInfo);
                }
            }
        }
        /* Delete in reverse order to preserve indices */
        for (auto it = toDelete.rbegin(); it != toDelete.rend(); ++it) {
            BAERmfEditorDocument_DeleteNote(m_document, trackIndex, *it);
        }
    }

    void TrimTrackCCEventsToBoundary(uint16_t trackIndex, uint32_t boundary) {
        std::vector<AutomationLaneDescriptor> const &lanes = GetAutomationLanes();

        for (AutomationLaneDescriptor const &lane : lanes) {
            uint32_t eventCount;
            std::vector<uint32_t> toDelete;

            if (lane.kind != AutomationLaneKind::Controller) {
                continue;
            }
            eventCount = 0;
            if (BAERmfEditorDocument_GetTrackCCEventCount(m_document, trackIndex, lane.controller, &eventCount) != BAE_NO_ERROR) {
                continue;
            }
            for (uint32_t i = 0; i < eventCount; ++i) {
                uint32_t tick;
                unsigned char value;

                if (BAERmfEditorDocument_GetTrackCCEvent(m_document, trackIndex, lane.controller, i, &tick, &value) == BAE_NO_ERROR) {
                    if (tick >= boundary) {
                        toDelete.push_back(i);
                    }
                }
            }
            for (auto it = toDelete.rbegin(); it != toDelete.rend(); ++it) {
                BAERmfEditorDocument_DeleteTrackCCEvent(m_document, trackIndex, lane.controller, *it);
            }
        }
    }

    void TrimTrackPitchBendEventsToBoundary(uint16_t trackIndex, uint32_t boundary) {
        uint32_t eventCount;
        std::vector<uint32_t> toDelete;

        eventCount = 0;
        if (BAERmfEditorDocument_GetTrackPitchBendEventCount(m_document, trackIndex, &eventCount) != BAE_NO_ERROR) {
            return;
        }
        for (uint32_t i = 0; i < eventCount; ++i) {
            uint32_t tick;
            uint16_t value;

            if (BAERmfEditorDocument_GetTrackPitchBendEvent(m_document, trackIndex, i, &tick, &value) == BAE_NO_ERROR) {
                if (tick >= boundary) {
                    toDelete.push_back(i);
                }
            }
        }
        for (auto it = toDelete.rbegin(); it != toDelete.rend(); ++it) {
            BAERmfEditorDocument_DeleteTrackPitchBendEvent(m_document, trackIndex, *it);
        }
    }

    void TrimTempoEventsToBoundary(uint32_t boundary) {
        uint32_t tempoCount;
        std::vector<uint32_t> toDelete;

        tempoCount = 0;
        if (BAERmfEditorDocument_GetTempoEventCount(m_document, &tempoCount) != BAE_NO_ERROR) {
            return;
        }
        for (uint32_t i = 0; i < tempoCount; ++i) {
            uint32_t tick;
            uint32_t microsecondsPerQuarter;

            if (BAERmfEditorDocument_GetTempoEvent(m_document, i, &tick, &microsecondsPerQuarter) == BAE_NO_ERROR) {
                if (tick >= boundary) {
                    toDelete.push_back(i);
                }
            }
        }
        for (auto it = toDelete.rbegin(); it != toDelete.rend(); ++it) {
            BAERmfEditorDocument_DeleteTempoEvent(m_document, *it);
        }
    }

    double TickToSeconds(uint32_t tick) const {
        uint16_t ticksPerQuarter;
        uint32_t tempoCount;
        uint32_t baseBpm;
        uint32_t currentTick;
        uint32_t currentMicrosecondsPerQuarter;
        double seconds;
        uint32_t tempoIndex;

        ticksPerQuarter = GetTicksPerQuarter();
        if (ticksPerQuarter == 0) {
            return 0.0;
        }
        baseBpm = 120;
        if (m_document) {
            BAERmfEditorDocument_GetTempoBPM(m_document, &baseBpm);
        }
        if (baseBpm == 0) {
            baseBpm = 120;
        }
        currentTick = 0;
        currentMicrosecondsPerQuarter = 60000000UL / baseBpm;
        seconds = 0.0;
        tempoCount = 0;
        if (!m_document || BAERmfEditorDocument_GetTempoEventCount(m_document, &tempoCount) != BAE_NO_ERROR) {
            return (static_cast<double>(tick) * static_cast<double>(currentMicrosecondsPerQuarter)) /
                   (static_cast<double>(ticksPerQuarter) * 1000000.0);
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
                seconds += (static_cast<double>(eventTick - currentTick) * static_cast<double>(currentMicrosecondsPerQuarter)) /
                           (static_cast<double>(ticksPerQuarter) * 1000000.0);
            }
            currentTick = eventTick;
            if (eventMicrosecondsPerQuarter > 0) {
                currentMicrosecondsPerQuarter = eventMicrosecondsPerQuarter;
            }
        }
        if (tick > currentTick) {
            seconds += (static_cast<double>(tick - currentTick) * static_cast<double>(currentMicrosecondsPerQuarter)) /
                       (static_cast<double>(ticksPerQuarter) * 1000000.0);
        }
        return seconds;
    }

    void DrawAutomationLanes(wxDC &dc, wxSize const &virtualSize, wxRect const &visibleRect) {
        std::vector<AutomationLaneDescriptor> const &lanes = GetAutomationLanes();
        uint32_t documentEndTick;
        int laneIndex;
        int visibleLeft;
        int visibleRight;

        visibleLeft  = std::max(0, visibleRect.GetLeft());
        visibleRight = std::min(virtualSize.GetWidth(), visibleRect.GetRight());
        documentEndTick = GetDocumentEndTick();

        for (laneIndex = 0; laneIndex < static_cast<int>(lanes.size()); ++laneIndex) {
            AutomationLaneDescriptor const &lane = lanes[laneIndex];
            int laneTop;
            int laneBottom;
            int laneVisibleLeft;
            int laneVisibleWidth;

            laneTop = GetAutomationLaneY(laneIndex);
            laneBottom = laneTop + kAutomationLaneHeight;
            if (laneBottom < visibleRect.GetTop() ||
                laneTop   > visibleRect.GetBottom())
                continue;

            laneVisibleLeft  = std::max(kPianoRollLeftGutter, visibleLeft);
            laneVisibleWidth = std::max(0, visibleRight - laneVisibleLeft);

            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(m_rc.brushAutomationBg);
            if (laneVisibleWidth > 0)
                dc.DrawRectangle(laneVisibleLeft, laneTop,
                                 laneVisibleWidth, kAutomationLaneHeight);

            dc.SetBrush(m_rc.brushAutomationGutter);
            dc.DrawRectangle(0, laneTop,
                             kPianoRollLeftGutter, kAutomationLaneHeight);

            dc.SetPen(m_rc.penAutomationBorder);
            dc.DrawLine(0, laneBottom, visibleRight, laneBottom);

            dc.SetTextForeground(m_theme.automationLaneLabel);
            dc.DrawText(lane.label, 8,
                        laneTop + (kAutomationLaneHeight / 2) - 8);

            if (lane.bipolar) {
                dc.SetPen(wxPen(m_theme.automationLaneCenterLine));
                dc.DrawLine(kPianoRollLeftGutter,
                            laneTop + (kAutomationLaneHeight / 2),
                            visibleRight,
                            laneTop + (kAutomationLaneHeight / 2));
            }
            if (!m_document) {
                continue;
            }
            AutomationLaneCache const *laneCache;
            AutomationDisplayCache const *displayCache;
            bool useDownsampledDisplay;

            laneCache = GetAutomationLaneCache(laneIndex, documentEndTick);
            if (!laneCache) {
                continue;
            }
            uint32_t visibleStartTick;
            uint32_t visibleEndTick;

            visibleStartTick = XToTick(std::max(kPianoRollLeftGutter, visibleLeft));
            visibleEndTick = XToTick(std::max(kPianoRollLeftGutter, visibleRight));
            auto valueToY = [&](int value) -> int {
                int clampedValue;

                clampedValue = std::clamp(value, 0, lane.maxValue);
                return laneBottom - 1 - ((clampedValue * (kAutomationLaneHeight - 1)) / std::max(1, lane.maxValue));
            };
            useDownsampledDisplay = static_cast<int>(laneCache->segments.size()) > std::max(64, laneVisibleWidth * 2);
            displayCache = nullptr;
            if (useDownsampledDisplay) {
                displayCache = GetAutomationDisplayCache(laneIndex,
                                                         laneVisibleLeft,
                                                         visibleRight,
                                                         laneTop,
                                                         laneBottom,
                                                         lane,
                                                         laneCache);
            }
            if (displayCache) {
                dc.SetPen(wxPen(lane.color, 1));
                for (AutomationDisplayBucket const &bucket : displayCache->buckets) {
                    if (!bucket.hasData) {
                        continue;
                    }
                    if (bucket.minY == bucket.maxY) {
                        dc.DrawPoint(bucket.x, bucket.minY);
                    } else {
                        dc.DrawLine(bucket.x, bucket.minY, bucket.x, bucket.maxY);
                    }
                }
            } else {
            auto segmentIt = std::lower_bound(laneCache->segments.begin(),
                                              laneCache->segments.end(),
                                              visibleStartTick,
                                              [](AutomationCurveSegment const &segment, uint32_t tick) {
                                                  return segment.startTick < tick;
                                              });
            if (segmentIt != laneCache->segments.begin()) {
                --segmentIt;
            }
            while (segmentIt != laneCache->segments.end() && segmentIt->endTick <= visibleStartTick) {
                ++segmentIt;
            }

            for (; segmentIt != laneCache->segments.end(); ++segmentIt) {
                AutomationCurveSegment const &segment = *segmentIt;
                int x0;
                int x1;
                int y0;
                int y1;
                bool segmentSelected;

                if (segment.startTick > visibleEndTick) {
                    break;
                }
                x0 = TickToX(segment.startTick);
                x1 = TickToX(std::max(segment.startTick + 1, segment.endTick));
                if (x1 < visibleLeft || x0 > visibleRight) {
                    continue;
                }
                y0 = valueToY(segment.startValue);
                y1 = valueToY(segment.endValue);
                segmentSelected = (m_selectedItemKind == PianoRollSelectionKind::Automation &&
                                   m_selectedAutomationLane == laneIndex &&
                                   m_selectedAutomationEvent == static_cast<long>(segment.sourceEventIndex));
                dc.SetPen(wxPen(segmentSelected ? m_theme.automationSelectedBorder : lane.color,
                                segmentSelected ? 2 : 1));
                if (segment.interpolation == AutomationInterpolation::Linear && segment.hasFollowingEvent) {
                    dc.DrawLine(x0, y0, x1, y1);
                } else {
                    dc.DrawLine(x0, y0, x1, y0);
                    if (segment.hasFollowingEvent) {
                        dc.DrawLine(x1, y0, x1, y1);
                    }
                }
            }
            }

            if (m_selectedAutomationLane == laneIndex || m_hoverAutomationLane == laneIndex) {
                int visibleNodeCount;
                int maxVisibleNodeMarkers;
                bool suppressDenseNodeMarkers;

                visibleNodeCount = 0;
                for (AutomationCurveNode const &node : laneCache->nodes) {
                    int nodeX;

                    nodeX = TickToX(node.tick);
                    if (nodeX + 6 < visibleLeft || nodeX - 6 > visibleRight) {
                        continue;
                    }
                    ++visibleNodeCount;
                }
                maxVisibleNodeMarkers = std::max(24, laneVisibleWidth / 12);
                suppressDenseNodeMarkers = visibleNodeCount > maxVisibleNodeMarkers;
                for (AutomationCurveNode const &node : laneCache->nodes) {
                    int nodeX;
                    int nodeY;
                    int radius;
                    bool nodeSelected;
                    bool nodeHovered;

                    nodeX = TickToX(node.tick);
                    if (nodeX + 6 < visibleLeft || nodeX - 6 > visibleRight) {
                        continue;
                    }
                    nodeY = valueToY(node.value);
                    nodeSelected = (m_selectedItemKind == PianoRollSelectionKind::Automation &&
                                    m_selectedAutomationLane == laneIndex &&
                                    m_selectedAutomationEvent == static_cast<long>(node.sourceEventIndex));
                    nodeHovered = (m_hoverAutomationLane == laneIndex &&
                                   m_hoverAutomationEvent == static_cast<long>(node.sourceEventIndex));
                    if (suppressDenseNodeMarkers && !nodeSelected && !nodeHovered) {
                        continue;
                    }
                    radius = nodeSelected ? 4 : (nodeHovered ? 4 : 3);
                    dc.SetPen(wxPen(nodeSelected ? m_theme.automationSelectedBorder : lane.color,
                                    nodeSelected ? 2 : 1));
                    dc.SetBrush(wxBrush(nodeSelected ? m_theme.automationSelectedBorder
                                                    : (nodeHovered ? wxColour(240, 240, 240)
                                                                   : lane.color)));
                    dc.DrawCircle(nodeX, nodeY, radius);
                }
            }
        }
    }

    void UpdateVirtualSize() {
        int width;
        int height;
        uint32_t lastTick;
        int laneCount;
        std::vector<AutomationLaneDescriptor> const &lanes = GetAutomationLanes();

        width = 2400;
        laneCount = 0;
        for (int laneIndex = 0; laneIndex < static_cast<int>(lanes.size()); ++laneIndex) {
            if (IsAutomationLaneVisible(laneIndex)) {
                laneCount++;
            }
        }
        height = GetAutomationAreaTop() + (laneCount * kAutomationLaneHeight) + ((std::max(0, laneCount - 1)) * kAutomationLaneGap) + kPianoRollTopGutter;
        lastTick = GetDocumentEndTick();
        if (m_draggingTimelineEnd && m_timelineEndDragTick > lastTick) {
            lastTick = m_timelineEndDragTick;
        }
        width = TickToX(lastTick) + 200;
        SetVirtualSize(width, height);
    }

    void DrawStickyRuler(wxDC &dc)
    {
        int scrollX = GetViewStartX();
        int scrollY = GetViewStartY();
        int screenW = GetClientSize().GetWidth();

        int rulerH  = kPianoRollTopGutter;
        int rulerTop = scrollY;
        int rulerBot = scrollY + rulerH;

        // Background
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(m_rc.brushRulerBg);
        dc.DrawRectangle(scrollX, rulerTop, screenW, rulerH);

        // Left gutter corner (darker)
        dc.SetBrush(m_rc.brushRulerGutterBg);
        dc.DrawRectangle(scrollX, rulerTop, kPianoRollLeftGutter, rulerH);

        // Gutter labels (text – unchanged)
        dc.SetTextForeground(m_theme.rulerGutterBeatText);
        dc.DrawText("Beat", scrollX + 3, rulerTop + 1);
        dc.SetTextForeground(m_theme.rulerGutterTimeText);
        dc.DrawText("Time", scrollX + 3, rulerTop + rulerH / 2);

        int relLeft  = std::max(0, scrollX - kPianoRollLeftGutter);
        int relRight = scrollX + screenW - kPianoRollLeftGutter;
        uint32_t quarterTicks = GetTicksPerQuarter();
        uint32_t stepTicks    = std::max<uint32_t>(1, quarterTicks / 4);
        uint32_t tickStart    = (XToTick(kPianoRollLeftGutter + relLeft) / stepTicks) * stepTicks;
        uint32_t tickEnd      = XToTick(kPianoRollLeftGutter + relRight);

        // Sub‑beat (16th‑note) small ticks
        for (uint32_t t = tickStart; t <= tickEnd; t += stepTicks) {
            if ((t % quarterTicks) == 0) continue;
            int vx = TickToX(t);
            dc.SetPen(m_rc.penRulerSubTick);
            dc.DrawLine(vx, rulerBot - 5, vx, rulerBot - 1);
            if (tickEnd - t < stepTicks) break;
        }

        // Quarter‑note beat labels and taller ticks
        tickStart = (tickStart / quarterTicks) * quarterTicks;
        for (uint32_t t = tickStart; t <= tickEnd; t += quarterTicks) {
            int vx = TickToX(t);
            bool isBar = ((t % (quarterTicks * 4)) == 0);
            dc.SetPen(isBar ? m_rc.penRulerBarTick : m_rc.penRulerBeatTick);
            dc.DrawLine(vx, rulerTop + (isBar ? 2 : rulerH / 2), vx, rulerBot - 1);

            // Beat number (top row)
            dc.SetTextForeground(isBar ? m_theme.rulerBarText : m_theme.rulerBeatText);
            int beatNum = static_cast<int>(t / quarterTicks) + 1;
            dc.DrawText(wxString::Format("%d", beatNum), vx + 2, rulerTop + 1);

            // Time code (bottom row)
            dc.SetTextForeground(m_theme.rulerTimeText);
            double seconds = TickToSeconds(t);
            int mm = static_cast<int>(seconds / 60.0);
            int ss = static_cast<int>(seconds) % 60;
            int ds = static_cast<int>((seconds - std::floor(seconds)) * 10.0);
            dc.DrawText(wxString::Format("%d:%02d.%d", mm, ss, ds), vx + 2,
                        rulerTop + rulerH / 2);
            if (tickEnd - t < quarterTicks) break;
        }

        // Bottom border
        dc.SetPen(m_rc.penRulerBorder);
        dc.DrawLine(scrollX, rulerBot - 1, scrollX + screenW, rulerBot - 1);

        // Left gutter right‑edge (re‑draw on top of ruler)
        dc.SetPen(m_rc.penRulerGutterBorder);
        dc.DrawLine(scrollX + kPianoRollLeftGutter, rulerTop,
                    scrollX + kPianoRollLeftGutter, rulerBot);
    }


    bool IsPointInStickyRuler(wxPoint logicalPoint) const {
        int scrollPixelsX;
        int scrollPixelsY;
        int viewUnitsX;
        int viewUnitsY;
        int scrollY;

        GetScrollPixelsPerUnit(&scrollPixelsX, &scrollPixelsY);
        if (scrollPixelsY <= 0) {
            scrollPixelsY = 1;
        }
        GetViewStart(&viewUnitsX, &viewUnitsY);
        scrollY = viewUnitsY * scrollPixelsY;
        return logicalPoint.y >= scrollY && logicalPoint.y < (scrollY + kPianoRollTopGutter);
    }

    bool HitTestTimelineEndHandle(wxPoint point) const {
        int endX;

        if (!IsPointInStickyRuler(point) || point.x < kPianoRollLeftGutter) {
            return false;
        }
        endX = TickToX(GetDocumentEndTick());
        return std::abs(point.x - endX) <= kResizeHandlePixels;
    }

    bool HitTestMidiLoopHandle(wxPoint point, MidiLoopDragMode *outMode = nullptr) const {
        int startX;
        int endX;

        if (outMode) {
            *outMode = MidiLoopDragMode::None;
        }
        if (!m_midiLoopEnabled || m_midiLoopEndTick <= m_midiLoopStartTick) {
            return false;
        }
        if (!IsPointInStickyRuler(point) || point.x < kPianoRollLeftGutter) {
            return false;
        }
        startX = TickToX(m_midiLoopStartTick);
        endX = TickToX(m_midiLoopEndTick);
        if (std::abs(point.x - startX) <= 6) {
            if (outMode) {
                *outMode = MidiLoopDragMode::StartHandle;
            }
            return true;
        }
        if (std::abs(point.x - endX) <= 6) {
            if (outMode) {
                *outMode = MidiLoopDragMode::EndHandle;
            }
            return true;
        }
        return false;
    }

    void DrawMidiLoopOverlay(wxDC &dc, wxRect const &visibleRect, wxSize const &virtualSize) {
        if (!m_midiLoopEnabled || m_midiLoopEndTick <= m_midiLoopStartTick)
            return;

        int startX = TickToX(m_midiLoopStartTick);
        int endX   = TickToX(m_midiLoopEndTick);
        if (endX < visibleRect.GetLeft() - 2 || startX > visibleRect.GetRight() + 2)
            return;

        int shadeLeft  = std::max(startX, kPianoRollLeftGutter);
        int shadeRight = std::max(shadeLeft + 1, endX);

        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(m_rc.brushMidiLoopShade);
        dc.DrawRectangle(shadeLeft,
                         kPianoRollTopGutter,
                         shadeRight - shadeLeft,
                         std::max(1, virtualSize.GetHeight() - kPianoRollTopGutter));
    }


    void DrawMidiLoopRulerOverlay(wxDC &dc, wxRect const &visibleRect, wxSize const &virtualSize) {
        int startX;
        int endX;
        int scrollPixelsX;
        int scrollPixelsY;
        int viewUnitsX;
        int viewUnitsY;
        int rulerTop;
        int rulerBottom;

        if (!m_midiLoopEnabled || m_midiLoopEndTick <= m_midiLoopStartTick) {
            return;
        }
        startX = TickToX(m_midiLoopStartTick);
        endX = TickToX(m_midiLoopEndTick);
        if (endX < visibleRect.GetLeft() - 2 || startX > visibleRect.GetRight() + 2) {
            return;
        }

        dc.SetPen(m_rc.penLoopLine);
        dc.DrawLine(startX, kPianoRollTopGutter, startX, virtualSize.GetHeight());
        dc.DrawLine(endX, kPianoRollTopGutter, endX, virtualSize.GetHeight());

        GetScrollPixelsPerUnit(&scrollPixelsX, &scrollPixelsY);
        if (scrollPixelsY <= 0) {
            scrollPixelsY = 1;
        }
        GetViewStart(&viewUnitsX, &viewUnitsY);
        rulerTop = viewUnitsY * scrollPixelsY;
        rulerBottom = rulerTop + kPianoRollTopGutter;

        dc.SetBrush(m_rc.brushLoopFill);
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(startX - 4, rulerBottom - 12, 8, 10);
        dc.DrawRectangle(endX - 4, rulerBottom - 12, 8, 10);

        dc.SetTextForeground(wxColour(46, 120, 79));
        dc.DrawText("Loop", std::max(kPianoRollLeftGutter + 4, startX + 6), rulerTop + 2);
    }

    long HitTestNote(wxPoint point, BAERmfEditorNoteInfo *noteInfoOut = nullptr) const {
        NoteTrackCache const *cache;
        uint32_t tick;
        uint32_t binIndex;
        int note;

        if (!HasTrack()) {
            return -1;
        }
        cache = const_cast<PianoRollPanel *>(this)->GetNoteTrackCache();
        if (!cache || cache->pitchBins.empty()) {
            return -1;
        }
        tick = XToTick(point.x);
        note = std::clamp(YToNote(point.y), 0, 127);
        binIndex = std::min<uint32_t>(tick / cache->ticksPerBin,
                                      static_cast<uint32_t>(cache->pitchBins[static_cast<size_t>(note)].size() - 1));
        for (uint32_t entryIndex : cache->pitchBins[static_cast<size_t>(note)][binIndex]) {
            NoteCacheEntry const &entry = cache->notes[entryIndex];
            wxRect noteRect;

            noteRect = BuildNoteRect(entry.noteInfo);
            if (noteRect.Contains(point)) {
                if (noteInfoOut) {
                    *noteInfoOut = entry.noteInfo;
                }
                return static_cast<long>(entry.sourceIndex);
            }
        }
        return -1;
    }

    void DeleteSelectedNote() {
        std::vector<long> noteIndices;

        if (!HasTrack()) {
            return;
        }
        noteIndices = m_selectedNotes;
        if (noteIndices.empty() && m_selectedNote >= 0) {
            noteIndices.push_back(m_selectedNote);
        }
        if (noteIndices.empty()) {
            return;
        }
        std::sort(noteIndices.begin(), noteIndices.end());
        noteIndices.erase(std::unique(noteIndices.begin(), noteIndices.end()), noteIndices.end());
        BeginUndoAction("Delete Note");
        bool anyDeleted = false;
        for (auto it = noteIndices.rbegin(); it != noteIndices.rend(); ++it) {
            if (*it < 0) {
                continue;
            }
            if (BAERmfEditorDocument_DeleteNote(m_document,
                                                static_cast<uint16_t>(m_selectedTrack),
                                                static_cast<uint32_t>(*it)) == BAE_NO_ERROR) {
                anyDeleted = true;
            }
        }
        if (anyDeleted) {
            CommitUndoAction("Delete Note");
            m_selectedNotes.clear();
            m_selectedNote = -1;
            InvalidateNoteCache();
            UpdateVirtualSize();
            Refresh();
        } else {
            CancelUndoAction();
        }
    }
    
    inline int GetScrollX() const {
        int sx, sy;
        GetScrollPixelsPerUnit(&sx, &sy);
        return sx > 0 ? sx : 1;
    }
    inline int GetScrollY() const {
        int sx, sy;
        GetScrollPixelsPerUnit(&sx, &sy);
        return sy > 0 ? sy : 1;
    }
    inline int GetViewStartX() const {
        int ux, uy;
        GetViewStart(&ux, &uy);
        return ux * GetScrollX();
    }
    inline int GetViewStartY() const {
        int ux, uy;
        GetViewStart(&ux, &uy);
        return uy * GetScrollY();
    }

    void OnPaint(wxPaintEvent &) {
        wxAutoBufferedPaintDC dc(this);
        PrepareDC(dc);

        // Cache frequently used values once per paint
        const int pixelsX   = GetPixelsPerQuarter();
        const uint16_t tpq = GetTicksPerQuarter();
        const int leftGutter = kPianoRollLeftGutter;
        const int topGutter  = kPianoRollTopGutter;
        const int noteH      = kNoteHeight;
        const int automHeight = kAutomationLaneHeight;
        const int automGap   = kAutomationLaneGap;
        const int gutterW   = leftGutter;

        dc.SetBackground(m_rc.brushWhiteKey);
        dc.Clear();

        const wxSize clientSize = GetVirtualSize();
        const int viewLeft   = GetViewStartX();
        const int viewTop    = GetViewStartY();
        const int viewRight  = viewLeft + GetClientSize().GetWidth();
        const int viewBottom = viewTop  + GetClientSize().GetHeight();

        // Pre‑compute note‑Y positions once
        static int noteY[128];
        static bool noteYValid = false;
        if (!noteYValid) {
            for (int n = 0; n < 128; ++n)
                noteY[n] = NoteToY(n);
            noteYValid = true;
        }

        for (int note = 0; note < 128; ++note) {
            const int y = noteY[note];
            if (y + noteH < viewTop || y > viewBottom) continue;

            const bool black = (note % 12 == 1) || (note % 12 == 3) ||
                               (note % 12 == 6) || (note % 12 == 8) ||
                               (note % 12 == 10);
            dc.SetBrush(black ? m_rc.brushBlackKey : m_rc.brushWhiteKey);
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(viewLeft, y, viewRight - viewLeft, noteH);
            dc.SetPen(m_rc.penNoteSeparator);
            dc.DrawLine(gutterW, y, viewRight, y);

            if (note % 12 == 0) {
                dc.SetTextForeground(m_theme.noteLabelText);
                dc.DrawText(wxString::Format("C%d", (note / 12) - 1), 6, y - 1);
            }
        }

        const uint32_t quarterTicks = tpq;
        const uint32_t stepTicks    = std::max<uint32_t>(1, quarterTicks / 4);
        const uint32_t tickStart    = (XToTick(viewLeft) / stepTicks) * stepTicks;
        const uint32_t tickEnd      = XToTick(viewRight);
        const int gridTop            = std::max(topGutter, viewTop);
        const int gridBottom         = viewBottom;

        for (uint32_t t = tickStart; t <= tickEnd; t += stepTicks) {
            const int x = TickToX(t);
            dc.SetPen((t % quarterTicks) == 0
                      ? m_rc.penRulerBarTick
                      : m_rc.penRulerSubTick);
            dc.DrawLine(x, gridTop, x, gridBottom);
            if (tickEnd - t < stepTicks) break;
        }

        dc.SetPen(m_rc.penGutterSeparator);
        dc.DrawLine(gutterW, viewTop, gutterW, viewBottom);

        DrawMidiLoopOverlay(dc, wxRect(viewLeft, viewTop,
                                        viewRight - viewLeft, viewBottom - viewTop),
                            clientSize);

        if (HasTrack()) {
            const NoteTrackCache *cache = GetNoteTrackCache();
            if (cache) {
                std::vector<uint32_t> visibleEntries;
                GatherNoteEntriesInRect(wxRect(viewLeft, viewTop,
                                                viewRight - viewLeft, viewBottom - viewTop),
                                        &visibleEntries);
                for (uint32_t idx : visibleEntries) {
                    const auto &entry = cache->notes[idx];
                    const wxRect r = BuildNoteRect(entry.noteInfo);
                    const bool sel = IsNoteSelected(static_cast<long>(entry.sourceIndex));

                    dc.SetBrush(sel ? m_rc.brushNoteSelected : m_rc.brushNoteNormal);
                    dc.SetPen(sel   ? m_rc.penNoteSelected   : m_rc.penNoteNormal);
                    dc.DrawRectangle(r);

                    if (sel && static_cast<long>(entry.sourceIndex) == m_selectedNote) {
                        const int h = std::max(2, r.GetHeight() - 2);
                        dc.SetBrush(wxBrush(wxColour(242, 225, 214)));
                        dc.SetPen(*wxTRANSPARENT_PEN);
                        dc.DrawRectangle(r.GetLeft(), r.GetTop() + 1, 3, h);
                        dc.DrawRectangle(r.GetRight() - 2, r.GetTop() + 1, 3, h);
                    }
                }
            }
        }

        DrawAutomationLanes(dc, clientSize,
                            wxRect(viewLeft, viewTop,
                                   viewRight - viewLeft, viewBottom - viewTop));

        if (m_showPlayhead) {
            const int phX = TickToX(m_playheadTick);
            if (phX >= viewLeft - 2 && phX <= viewRight + 2) {
                dc.SetPen(m_rc.penPlayhead);
                dc.DrawLine(phX, topGutter, phX, clientSize.GetHeight());
            }
        }

        if (m_selectBoxActive) {
            const wxRect sel = NormalizeRect(m_selectBoxStart, m_selectBoxCurrent);
            dc.SetBrush(m_rc.brushSelectionBox);
            dc.SetPen(m_rc.penSelectionBoxBorder);
            dc.DrawRectangle(sel);
        }

        {
            const uint32_t endTick = m_draggingTimelineEnd ? m_timelineEndDragTick
                                                           : GetDocumentEndTick();
            const int endX = TickToX(endTick);
            if (endX >= viewLeft - 2 && endX <= viewRight + 2) {
                // Shaded region beyond end
                if (endX < viewRight) {
                    dc.SetPen(*wxTRANSPARENT_PEN);
                    dc.SetBrush(m_rc.brushTimelineEnd);
                    dc.DrawRectangle(endX + 1,
                                     topGutter,
                                     viewRight - endX,
                                     std::max(1, clientSize.GetHeight() - topGutter));
                }
                // End marker line
                dc.SetPen(m_rc.penTimelineEnd);
                dc.DrawLine(endX, topGutter, endX, clientSize.GetHeight());

                // Small triangular tab at bottom of ruler
                wxPoint tri[3] = {
                    {endX, topGutter - 2},
                    {endX - 5, topGutter - 10},
                    {endX + 5, topGutter - 10}
                };
                dc.SetPen(m_rc.penTimelineHandle);
                dc.SetBrush(m_rc.brushTimelineHandleFill);
                dc.DrawPolygon(3, tri);
            }
        }

        DrawStickyRuler(dc);
        DrawMidiLoopRulerOverlay(dc,
                                 wxRect(viewLeft, viewTop,
                                        viewRight - viewLeft, viewBottom - viewTop),
                                 clientSize);
    }

    void OnLeftDown(wxMouseEvent &event) {
        wxPoint logicalPoint;
        AutomationHitInfo automationNodeHit;
        AutomationHitInfo automationHit;
        BAERmfEditorNoteInfo noteInfo;
        long hitNote;
        int previousAutomationLane;

        SetFocus();
        previousAutomationLane = (m_selectedItemKind == PianoRollSelectionKind::Automation) ? m_selectedAutomationLane : -1;
        logicalPoint = CalcUnscrolledPosition(event.GetPosition());
        if (IsPointInStickyRuler(logicalPoint) && logicalPoint.x >= kPianoRollLeftGutter) {
            if (HitTestTimelineEndHandle(logicalPoint)) {
                m_draggingTimelineEnd = true;
                m_timelineEndDragTick = GetDocumentEndTick();
                if (!HasCapture()) {
                    CaptureMouse();
                }
                return;
            }
            MidiLoopDragMode loopDragMode;

            loopDragMode = MidiLoopDragMode::None;
            if (HitTestMidiLoopHandle(logicalPoint, &loopDragMode)) {
                m_draggingMidiLoop = true;
                m_midiLoopDragMode = loopDragMode;
                m_originalMidiLoopStartTick = m_midiLoopStartTick;
                m_originalMidiLoopEndTick = m_midiLoopEndTick;
                if (!HasCapture()) {
                    CaptureMouse();
                }
                return;
            }
            uint32_t tick;

            tick = XToTick(logicalPoint.x);
            if (m_seekRequestedCallback) {
                m_seekRequestedCallback(tick);
            } else {
                SetPlayheadTick(tick);
            }
            return;
        }
        hitNote = HitTestNote(logicalPoint, &noteInfo);
        if (hitNote >= 0) {
            bool clickedAlreadySelected;
            bool shouldPreviewSingle;

            clickedAlreadySelected = IsNoteSelected(hitNote);
            m_dragMode = GetDragModeForPoint(BuildNoteRect(noteInfo), logicalPoint);
            if (!clickedAlreadySelected || m_dragMode != DragMode::Move) {
                SelectSingleNote(hitNote);
            }
            UpdatePrimarySelectedNote();
            m_selectedItemKind = PianoRollSelectionKind::Note;
            m_selectedAutomationLane = -1;
            m_selectedAutomationEvent = -1;
            m_dragging = true;
            m_selectBoxActive = false;
            m_dragAutomationValid = false;
            m_dragStartTick = SnapTick(XToTick(logicalPoint.x));
            m_dragStartNote = YToNote(logicalPoint.y);
            m_dragOriginalNoteIndices.clear();
            m_dragOriginalNotes.clear();
            if (m_dragMode == DragMode::Move && !m_selectedNotes.empty()) {
                for (long selectedIndex : m_selectedNotes) {
                    BAERmfEditorNoteInfo selectedNoteInfo;

                    if (selectedIndex < 0) {
                        continue;
                    }
                    if (GetNoteInfo(static_cast<uint32_t>(selectedIndex), &selectedNoteInfo)) {
                        m_dragOriginalNoteIndices.push_back(selectedIndex);
                        m_dragOriginalNotes.push_back(selectedNoteInfo);
                    }
                }
            }
            if (m_dragOriginalNotes.empty()) {
                m_dragOriginalNote = noteInfo;
                m_dragOriginalNoteIndices.push_back(hitNote);
                m_dragOriginalNotes.push_back(noteInfo);
            } else {
                m_dragOriginalNote = m_dragOriginalNotes.front();
            }
            shouldPreviewSingle = (m_selectedNotes.size() == 1 && m_dragOriginalNotes.size() == 1);
            if (shouldPreviewSingle) {
                RequestSingleNotePreview(m_dragOriginalNotes.front());
                m_lastPreviewDragNote = static_cast<int>(m_dragOriginalNotes.front().note);
            } else {
                m_lastPreviewDragNote = -1;
            }
            m_dragUndoLabel = (m_dragMode == DragMode::Move) ? "Move Note" : "Resize Note";
            BeginUndoAction(m_dragUndoLabel);
            m_dragUndoActive = true;
            CaptureMouse();
        } else {
            bool nodeHit;

            nodeHit = HitTestAutomationNode(logicalPoint, &automationNodeHit);
            if (!(nodeHit || HitTestAutomation(logicalPoint, &automationHit))) {
                m_selectedAutomationLane = -1;
                m_selectedAutomationEvent = -1;
                m_dragging = true;
                m_dragAutomationValid = false;
                m_dragMode = DragMode::SelectBox;
                m_dragUndoActive = false;
                m_dragUndoLabel.clear();
                m_selectBoxActive = true;
                m_selectBoxStart = logicalPoint;
                m_selectBoxCurrent = logicalPoint;
                m_selectedNotes.clear();
                m_selectedNote = -1;
                m_selectedItemKind = PianoRollSelectionKind::None;
                m_lastPreviewDragNote = -1;
                if (HasCapture()) {
                    ReleaseMouse();
                }
                CaptureMouse();
            } else {
            if (nodeHit) {
                automationHit = automationNodeHit;
            }
            m_selectedNotes.clear();
            m_selectedItemKind = PianoRollSelectionKind::Automation;
            m_selectedNote = -1;
            m_selectedAutomationLane = automationHit.laneIndex;
            m_selectedAutomationEvent = static_cast<long>(automationHit.eventIndex);
            m_dragging = true;
            m_selectBoxActive = false;
            m_dragAutomationValid = true;
            m_dragAutomationHit = automationHit;
            m_dragMode = nodeHit ? DragMode::Move : GetDragModeForAutomation(automationHit, logicalPoint);
            m_dragUndoLabel = nodeHit ? "Move Node" : ((m_dragMode == DragMode::ResizeRight) ? "Resize Event" : "Edit Event");
            BeginUndoAction(m_dragUndoLabel);
            m_dragUndoActive = true;
            m_lastPreviewDragNote = -1;
            CaptureMouse();
            }
        }
        if (m_selectionChangedCallback) {
            m_selectionChangedCallback();
        }
        if (m_selectedItemKind == PianoRollSelectionKind::Automation && m_selectedAutomationLane >= 0) {
            RefreshAutomationLanes(previousAutomationLane, m_selectedAutomationLane);
        } else if (previousAutomationLane >= 0) {
            RefreshAutomationLane(previousAutomationLane);
        } else {
            Refresh();
        }
    }

    void OnLeftUp(wxMouseEvent &) {
        if (m_draggingMidiLoop) {
            bool changed;
            bool saved;

            if (HasCapture()) {
                ReleaseMouse();
            }
            changed = (m_midiLoopStartTick != m_originalMidiLoopStartTick) ||
                      (m_midiLoopEndTick != m_originalMidiLoopEndTick);
            saved = false;
            if (changed && m_midiLoopEditCallback) {
                BeginUndoAction("Edit MIDI Loop Markers");
                saved = m_midiLoopEditCallback(true, m_midiLoopStartTick, m_midiLoopEndTick);
                if (saved) {
                    CommitUndoAction("Edit MIDI Loop Markers");
                } else {
                    CancelUndoAction();
                    m_midiLoopStartTick = m_originalMidiLoopStartTick;
                    m_midiLoopEndTick = m_originalMidiLoopEndTick;
                }
            }
            m_draggingMidiLoop = false;
            m_midiLoopDragMode = MidiLoopDragMode::None;
            Refresh();
            return;
        }

        if (m_draggingTimelineEnd) {
            if (HasCapture()) {
                ReleaseMouse();
            }
            ApplyTimelineResize(m_timelineEndDragTick);
            m_draggingTimelineEnd = false;
            UpdateVirtualSize();
            Refresh();
            return;
        }

        RequestStopNotePreview();
        if (m_dragging && HasCapture()) {
            ReleaseMouse();
        }
        if (m_dragMode == DragMode::SelectBox && m_selectBoxActive) {
            wxRect selectionRect;

            selectionRect = NormalizeRect(m_selectBoxStart, m_selectBoxCurrent);
            UpdateAreaSelection(selectionRect);
            m_selectBoxActive = false;
            if (m_selectionChangedCallback) {
                m_selectionChangedCallback();
            }
            Refresh();
        } else if (m_dragUndoActive) {
            CommitUndoAction(m_dragUndoLabel);
        }
        m_dragging = false;
        m_dragAutomationValid = false;
        m_dragMode = DragMode::None;
        m_dragUndoActive = false;
        m_dragUndoLabel.clear();
        m_lastPreviewDragNote = -1;
    }

    void OnLeftDoubleClick(wxMouseEvent &event) {
        wxPoint logicalPoint;
        int laneIndex;
        uint32_t startTick;
        int eventValue;
        unsigned char noteValue;

        if (!m_document) {
            return;
        }
        logicalPoint = CalcUnscrolledPosition(event.GetPosition());
        if (HitTestNote(logicalPoint) >= 0) {
            return;
        }
        laneIndex = GetAutomationLaneIndexFromY(logicalPoint.y);
        if (laneIndex >= 0) {
            std::vector<AutomationLaneDescriptor> const &lanes = GetAutomationLanes();
            startTick = SnapTick(XToTick(logicalPoint.x));
            eventValue = AutomationValueFromY(laneIndex, logicalPoint.y);
            m_dragUndoLabel = wxString::Format("Add %s Event", lanes[laneIndex].label);
            BeginUndoAction(m_dragUndoLabel);
            if (AddAutomationEvent(laneIndex, startTick, eventValue)) {
                AutomationHitInfo automationHit;

                if (HitTestAutomation(logicalPoint, &automationHit)) {
                    m_selectedItemKind = PianoRollSelectionKind::Automation;
                    m_selectedAutomationLane = automationHit.laneIndex;
                    m_selectedAutomationEvent = static_cast<long>(automationHit.eventIndex);
                    m_selectedNote = -1;
                }
                CommitUndoAction(m_dragUndoLabel);
                UpdateVirtualSize();
                RefreshAutomationLane(laneIndex);
            } else {
                CancelUndoAction();
            }
            return;
        }
        if (!HasTrack()) {
            return;
        }
        startTick = SnapTick(XToTick(logicalPoint.x));
        noteValue = static_cast<unsigned char>(YToNote(logicalPoint.y));
        BeginUndoAction("Add Note");
        if (BAERmfEditorDocument_AddNote(m_document,
                                         static_cast<uint16_t>(m_selectedTrack),
                                         startTick,
                                         kDefaultNoteDuration,
                                         noteValue,
                                         100) == BAE_NO_ERROR) {
            uint32_t noteCount;

            noteCount = 0;
            BAERmfEditorDocument_GetNoteCount(m_document, static_cast<uint16_t>(m_selectedTrack), &noteCount);
            if (noteCount > 0) {
                BAERmfEditorNoteInfo noteInfo;

                if (BAERmfEditorDocument_GetNoteInfo(m_document,
                                                     static_cast<uint16_t>(m_selectedTrack),
                                                     noteCount - 1,
                                                     &noteInfo) == BAE_NO_ERROR) {
                    noteInfo.bank = m_newNoteBank;
                    noteInfo.program = m_newNoteProgram;
                    BAERmfEditorDocument_SetNoteInfo(m_document,
                                                     static_cast<uint16_t>(m_selectedTrack),
                                                     noteCount - 1,
                                                     &noteInfo);
                    m_selectedItemKind = PianoRollSelectionKind::Note;
                    m_selectedNote = static_cast<long>(noteCount - 1);
                    m_selectedNotes.clear();
                    m_selectedNotes.push_back(m_selectedNote);
                    m_selectedAutomationLane = -1;
                    m_selectedAutomationEvent = -1;
                }
            }
            CommitUndoAction("Add Note");
            InvalidateNoteCache();
            UpdateVirtualSize();
            Refresh();
        } else {
            CancelUndoAction();
        }
    }

    void OnRightDown(wxMouseEvent &event) {
        wxPoint logicalPoint;
        AutomationHitInfo automationNodeHit;
        AutomationHitInfo automationHit;
        long hitNote;
        wxMenu menu;
        int previousAutomationLane;

        if (!m_document) {
            return;
        }
        if (event.LeftIsDown() || m_dragging || HasCapture()) {
            return;
        }
        previousAutomationLane = (m_selectedItemKind == PianoRollSelectionKind::Automation) ? m_selectedAutomationLane : -1;
        logicalPoint = CalcUnscrolledPosition(event.GetPosition());
        hitNote = HitTestNote(logicalPoint);
        if (hitNote >= 0) {
            if (!IsNoteSelected(hitNote)) {
                SelectSingleNote(hitNote);
            } else {
                UpdatePrimarySelectedNote();
            }
            m_selectedAutomationLane = -1;
            m_selectedAutomationEvent = -1;
        } else {
            bool nodeHit;

            nodeHit = HitTestAutomationNode(logicalPoint, &automationNodeHit);
            if (!(nodeHit || HitTestAutomation(logicalPoint, &automationHit))) {
                return;
            }
            if (nodeHit) {
                automationHit = automationNodeHit;
            }
            m_selectedNotes.clear();
            m_selectedItemKind = PianoRollSelectionKind::Automation;
            m_selectedNote = -1;
            m_selectedAutomationLane = automationHit.laneIndex;
            m_selectedAutomationEvent = static_cast<long>(automationHit.eventIndex);
        }
        menu.Append(ID_PianoRollEdit, "Edit");
        if (m_selectedItemKind == PianoRollSelectionKind::Automation) {
            menu.Append(ID_PianoRollToggleRamp, "Toggle Ramp To Next\tR");
        }
        menu.Append(ID_PianoRollDelete, "Delete");
        PopupMenu(&menu, event.GetPosition());
        if (m_selectionChangedCallback) {
            m_selectionChangedCallback();
        }
        if (m_selectedItemKind == PianoRollSelectionKind::Automation && m_selectedAutomationLane >= 0) {
            RefreshAutomationLanes(previousAutomationLane, m_selectedAutomationLane);
        } else if (previousAutomationLane >= 0) {
            RefreshAutomationLane(previousAutomationLane);
        } else {
            Refresh();
        }
    }

    void OnMotion(wxMouseEvent &event) {
        wxPoint logicalPoint;
        int automationValue;
        uint32_t automationTick;
        uint32_t snappedTick;
        int snappedNote;
        BAERmfEditorNoteInfo updatedNote;

        if (!m_document) {
            return;
        }
        if (m_draggingMidiLoop) {
            uint32_t snappedTick;

            if (!event.Dragging() || !event.LeftIsDown()) {
                return;
            }
            logicalPoint = CalcUnscrolledPosition(event.GetPosition());
            snappedTick = SnapTick(XToTick(logicalPoint.x));
            if (m_midiLoopDragMode == MidiLoopDragMode::StartHandle) {
                if (snappedTick >= m_midiLoopEndTick) {
                    snappedTick = m_midiLoopEndTick - 1;
                }
                m_midiLoopStartTick = snappedTick;
            } else if (m_midiLoopDragMode == MidiLoopDragMode::EndHandle) {
                if (snappedTick <= m_midiLoopStartTick) {
                    snappedTick = m_midiLoopStartTick + 1;
                }
                m_midiLoopEndTick = snappedTick;
            }
            Refresh();
            return;
        }
        if (m_draggingTimelineEnd) {
            uint32_t snappedTick;
            uint32_t minTick;

            if (!event.Dragging() || !event.LeftIsDown()) {
                return;
            }
            logicalPoint = CalcUnscrolledPosition(event.GetPosition());
            snappedTick = SnapTick(XToTick(logicalPoint.x));
            minTick = GetTicksPerQuarter() * 4;
            if (snappedTick < minTick) {
                snappedTick = minTick;
            }
            m_timelineEndDragTick = snappedTick;
            UpdateVirtualSize();
            Refresh();
            return;
        }
        if (!m_dragging) {
            AutomationHitInfo hoverHit;
            bool hasHover;
            int newHoverLane;
            long newHoverEvent;
            int previousHoverLane;

            logicalPoint = CalcUnscrolledPosition(event.GetPosition());
            UpdateHoverCursor(logicalPoint);
            hasHover = HitTestAutomationNode(logicalPoint, &hoverHit);
            if (!hasHover) {
                hasHover = HitTestAutomation(logicalPoint, &hoverHit);
            }
            previousHoverLane = m_hoverAutomationLane;
            newHoverLane = hasHover ? hoverHit.laneIndex : -1;
            newHoverEvent = hasHover ? static_cast<long>(hoverHit.eventIndex) : -1;
            if (newHoverLane != m_hoverAutomationLane || newHoverEvent != m_hoverAutomationEvent) {
                m_hoverAutomationLane = newHoverLane;
                m_hoverAutomationEvent = newHoverEvent;
                RefreshAutomationLanes(previousHoverLane, newHoverLane);
            }
            return;
        }
        if (!event.Dragging() || !event.LeftIsDown()) {
            return;
        }
        logicalPoint = CalcUnscrolledPosition(event.GetPosition());
        if (m_dragMode == DragMode::SelectBox && m_selectBoxActive) {
            wxRect selectionRect;

            m_selectBoxCurrent = logicalPoint;
            selectionRect = NormalizeRect(m_selectBoxStart, m_selectBoxCurrent);
            UpdateAreaSelection(selectionRect);
            Refresh();
            return;
        }
        if (m_selectedItemKind == PianoRollSelectionKind::Note) {
            if (m_selectedNote < 0 || !HasTrack() || m_dragOriginalNoteIndices.empty() || m_dragOriginalNotes.empty()) {
                return;
            }
            if (m_dragMode == DragMode::Move) {
                snappedTick = SnapTick(XToTick(logicalPoint.x));
                snappedNote = YToNote(logicalPoint.y);
                for (size_t i = 0; i < m_dragOriginalNotes.size(); ++i) {
                    BAERmfEditorNoteInfo movedNote;
                    long noteIndex;

                    movedNote = m_dragOriginalNotes[i];
                    noteIndex = m_dragOriginalNoteIndices[i];
                    if (noteIndex < 0) {
                        continue;
                    }
                    if (snappedTick >= m_dragStartTick) {
                        movedNote.startTick = m_dragOriginalNotes[i].startTick + (snappedTick - m_dragStartTick);
                    } else {
                        uint32_t deltaTicks;

                        deltaTicks = m_dragStartTick - snappedTick;
                        movedNote.startTick = (deltaTicks > m_dragOriginalNotes[i].startTick) ? 0 : (m_dragOriginalNotes[i].startTick - deltaTicks);
                    }
                    movedNote.note = static_cast<unsigned char>(std::clamp(static_cast<int>(m_dragOriginalNotes[i].note) + (snappedNote - m_dragStartNote), 0, 127));
                    BAERmfEditorDocument_SetNoteInfo(m_document,
                                                     static_cast<uint16_t>(m_selectedTrack),
                                                     static_cast<uint32_t>(noteIndex),
                                                     &movedNote);
                }
                if (m_selectedNotes.size() == 1 && !m_dragOriginalNotes.empty()) {
                    BAERmfEditorNoteInfo previewNote;

                    previewNote = m_dragOriginalNotes.front();
                    if (snappedTick >= m_dragStartTick) {
                        previewNote.startTick = m_dragOriginalNotes.front().startTick + (snappedTick - m_dragStartTick);
                    } else {
                        uint32_t deltaTicks;

                        deltaTicks = m_dragStartTick - snappedTick;
                        previewNote.startTick = (deltaTicks > m_dragOriginalNotes.front().startTick)
                                                    ? 0
                                                    : (m_dragOriginalNotes.front().startTick - deltaTicks);
                    }
                    previewNote.note = static_cast<unsigned char>(std::clamp(static_cast<int>(m_dragOriginalNotes.front().note) + (snappedNote - m_dragStartNote), 0, 127));
                    if (static_cast<int>(previewNote.note) != m_lastPreviewDragNote) {
                        RequestSingleNotePreview(previewNote);
                        m_lastPreviewDragNote = static_cast<int>(previewNote.note);
                    }
                }
            } else if (m_dragMode == DragMode::ResizeLeft) {
                uint32_t originalEndTick;
                uint32_t snappedStartTick;
                uint32_t maxStartTick;
                uint32_t snapTicks;

                updatedNote = m_dragOriginalNote;
                snapTicks = GetSnapTicks();
                originalEndTick = m_dragOriginalNote.startTick + m_dragOriginalNote.durationTicks;
                snappedStartTick = SnapTick(XToTick(logicalPoint.x));
                maxStartTick = (originalEndTick > snapTicks) ? (originalEndTick - snapTicks) : 0;
                updatedNote.startTick = std::min(snappedStartTick, maxStartTick);
                updatedNote.durationTicks = std::max<uint32_t>(snapTicks, originalEndTick - updatedNote.startTick);
                BAERmfEditorDocument_SetNoteInfo(m_document,
                                                 static_cast<uint16_t>(m_selectedTrack),
                                                 static_cast<uint32_t>(m_selectedNote),
                                                 &updatedNote);
            } else if (m_dragMode == DragMode::ResizeRight) {
                uint32_t snappedEndTick;
                uint32_t snapTicks;

                updatedNote = m_dragOriginalNote;
                snapTicks = GetSnapTicks();
                snappedEndTick = SnapTick(XToTick(logicalPoint.x));
                if (snappedEndTick <= m_dragOriginalNote.startTick + snapTicks) {
                    snappedEndTick = m_dragOriginalNote.startTick + snapTicks;
                }
                updatedNote.durationTicks = snappedEndTick - m_dragOriginalNote.startTick;
                BAERmfEditorDocument_SetNoteInfo(m_document,
                                                 static_cast<uint16_t>(m_selectedTrack),
                                                 static_cast<uint32_t>(m_selectedNote),
                                                 &updatedNote);
            }
            InvalidateNoteCache();
            UpdateVirtualSize();
            Refresh();
            return;
        }
        if (m_selectedItemKind != PianoRollSelectionKind::Automation || !m_dragAutomationValid || m_selectedAutomationLane < 0 || m_selectedAutomationEvent < 0) {
            return;
        }
        automationTick = SnapTick(XToTick(logicalPoint.x));
        automationValue = AutomationValueFromY(m_selectedAutomationLane, logicalPoint.y);
        if (m_dragMode == DragMode::ResizeRight && m_dragAutomationHit.hasFollowingEvent) {
            uint32_t currentTick;
            uint32_t nextTickCurrent;
            int currentValue;
            int nextValueCurrent;
            uint32_t nextNextTick;
            uint32_t eventCount;
            uint32_t snapTicks;

            if (!GetAutomationEvent(m_selectedAutomationLane, static_cast<uint32_t>(m_selectedAutomationEvent), &currentTick, &currentValue)) {
                return;
            }
            if (!GetAutomationEvent(m_selectedAutomationLane,
                                    static_cast<uint32_t>(m_selectedAutomationEvent) + 1,
                                    &nextTickCurrent,
                                    &nextValueCurrent)) {
                return;
            }
            nextNextTick = GetDocumentEndTick();
            eventCount = 0;
            GetAutomationEventCount(m_selectedAutomationLane, &eventCount);
            if (static_cast<uint32_t>(m_selectedAutomationEvent) + 2 < eventCount) {
                int nextNextValue;

                GetAutomationEvent(m_selectedAutomationLane,
                                   static_cast<uint32_t>(m_selectedAutomationEvent) + 2,
                                   &nextNextTick,
                                   &nextNextValue);
            }
              snapTicks = GetSnapTicks();
            automationTick = std::clamp(automationTick,
                                    currentTick + snapTicks,
                                    nextNextTick > snapTicks ? (nextNextTick - snapTicks) : currentTick + snapTicks);
            if (SetAutomationEvent(m_selectedAutomationLane,
                                   static_cast<uint32_t>(m_selectedAutomationEvent) + 1,
                                   automationTick,
                                   nextValueCurrent)) {
                RefreshAutomationLane(m_selectedAutomationLane);
            }
            return;
        }
        {
            uint32_t previousTick;
            uint32_t nextTick;
            uint32_t eventCount;
            int previousValue;
            int nextValue;
            uint32_t snapTicks;

            previousTick = 0;
            nextTick = GetDocumentEndTick();
            eventCount = 0;
            snapTicks = GetSnapTicks();
            GetAutomationEventCount(m_selectedAutomationLane, &eventCount);
            if (m_selectedAutomationEvent > 0) {
                GetAutomationEvent(m_selectedAutomationLane,
                                   static_cast<uint32_t>(m_selectedAutomationEvent) - 1,
                                   &previousTick,
                                   &previousValue);
                previousTick += snapTicks;
            }
            if (static_cast<uint32_t>(m_selectedAutomationEvent) + 1 < eventCount) {
                GetAutomationEvent(m_selectedAutomationLane,
                                   static_cast<uint32_t>(m_selectedAutomationEvent) + 1,
                                   &nextTick,
                                   &nextValue);
                if (nextTick > snapTicks) {
                    nextTick -= snapTicks;
                }
            }
            automationTick = std::clamp(automationTick, previousTick, nextTick);
            if (SetAutomationEvent(m_selectedAutomationLane,
                                   static_cast<uint32_t>(m_selectedAutomationEvent),
                                   automationTick,
                                   automationValue)) {
                RefreshAutomationLane(m_selectedAutomationLane);
            }
        }
    }

    void OnCharHook(wxKeyEvent &event) {
        if (event.GetKeyCode() == 'R' || event.GetKeyCode() == 'r') {
            if (ToggleRampForSelectedAutomationSegment()) {
                return;
            }
        }
        if (event.GetKeyCode() == WXK_DELETE || event.GetKeyCode() == WXK_BACK) {
            DeleteCurrentSelection();
            return;
        }
        event.Skip();
    }

    void OnMouseWheel(wxMouseEvent &event) {
        wxPoint logicalPoint;
        bool wheelOnRuler;

        logicalPoint = CalcUnscrolledPosition(event.GetPosition());
        wheelOnRuler = IsPointInStickyRuler(logicalPoint) && logicalPoint.x >= kPianoRollLeftGutter;
        if ((wheelOnRuler || event.AltDown()) && !event.ControlDown()) {
            if (ScrollHorizontallyWithWheel(event, 2)) {
                return;
            }
        }
        if (wheelOnRuler && !event.ControlDown()) {
            if (ScrollHorizontallyWithWheel(event, 1)) {
                return;
            }
        }
        if (event.ControlDown()) {
            ApplyZoomStep(event.GetWheelRotation(), event.GetPosition());
            return;
        }
        event.Skip();
    }

    void OnMouseLeave(wxMouseEvent &) {
        if (!m_dragging) {
            bool hadHover;
            int previousHoverLane;

            hadHover = (m_hoverAutomationLane >= 0 || m_hoverAutomationEvent >= 0);
            previousHoverLane = m_hoverAutomationLane;
            m_hoverAutomationLane = -1;
            m_hoverAutomationEvent = -1;
            SetCursor(wxNullCursor);
            if (hadHover) {
                RefreshAutomationLane(previousHoverLane);
            }
        }
    }
};


PianoRollPanel *CreatePianoRollPanel(wxWindow *parent) {
    return new PianoRollPanel(parent);
}

wxWindow *PianoRollPanel_AsWindow(PianoRollPanel *panel) {
    return panel;
}

void PianoRollPanel_SetDocument(PianoRollPanel *panel, BAERmfEditorDocument *document) {
    if (panel) {
        panel->SetDocument(document);
    }
}

void PianoRollPanel_SetSelectedTrack(PianoRollPanel *panel, int trackIndex) {
    if (panel) {
        panel->SetSelectedTrack(trackIndex);
    }
}

void PianoRollPanel_SetSelectionChangedCallback(PianoRollPanel *panel, std::function<void()> callback) {
    if (panel) {
        panel->SetSelectionChangedCallback(std::move(callback));
    }
}

void PianoRollPanel_SetSeekRequestedCallback(PianoRollPanel *panel, std::function<void(uint32_t)> callback) {
    if (panel) {
        panel->SetSeekRequestedCallback(std::move(callback));
    }
}

void PianoRollPanel_SetNotePreviewRequestedCallback(PianoRollPanel *panel,
                                                    std::function<void(uint16_t,
                                                                       unsigned char,
                                                                       unsigned char,
                                                                       unsigned char,
                                                                       uint32_t,
                                                                       int)> callback) {
    if (panel) {
        panel->SetNotePreviewRequestedCallback(std::move(callback));
    }
}

void PianoRollPanel_SetNotePreviewStopRequestedCallback(PianoRollPanel *panel,
                                                        std::function<void()> callback) {
    if (panel) {
        panel->SetNotePreviewStopRequestedCallback(std::move(callback));
    }
}

void PianoRollPanel_SetUndoCallbacks(PianoRollPanel *panel,
                                     std::function<void(wxString const &)> beginCallback,
                                     std::function<void(wxString const &)> commitCallback,
                                     std::function<void()> cancelCallback) {
    if (panel) {
        panel->SetUndoCallbacks(std::move(beginCallback), std::move(commitCallback), std::move(cancelCallback));
    }
}

void PianoRollPanel_SetMidiLoopMarkers(PianoRollPanel *panel,
                                       bool enabled,
                                       uint32_t startTick,
                                       uint32_t endTick) {
    if (panel) {
        panel->SetMidiLoopMarkers(enabled, startTick, endTick);
    }
}

void PianoRollPanel_SetMidiLoopEditCallback(PianoRollPanel *panel,
                                            std::function<bool(bool, uint32_t, uint32_t)> callback) {
    if (panel) {
        panel->SetMidiLoopEditCallback(std::move(callback));
    }
}

void PianoRollPanel_RefreshFromDocument(PianoRollPanel *panel, bool clearSelection) {
    if (panel) {
        panel->RefreshFromDocument(clearSelection);
    }
}

bool PianoRollPanel_GetSelectedNoteInfo(PianoRollPanel *panel, BAERmfEditorNoteInfo *outNoteInfo) {
    if (!panel) {
        return false;
    }
    return panel->GetSelectedNoteInfo(outNoteInfo);
}

void PianoRollPanel_SetSelectedNoteInstrument(PianoRollPanel *panel, uint16_t bank, unsigned char program) {
    if (panel) {
        panel->SetSelectedNoteInstrument(bank, program);
    }
}

void PianoRollPanel_SetNewNoteInstrument(PianoRollPanel *panel, uint16_t bank, unsigned char program) {
    if (panel) {
        panel->SetNewNoteInstrument(bank, program);
    }
}

void PianoRollPanel_SetPlayheadTick(PianoRollPanel *panel, uint32_t tick) {
    if (panel) {
        panel->SetPlayheadTick(tick);
    }
}

void PianoRollPanel_EnsurePlayheadVisible(PianoRollPanel *panel, uint32_t tick) {
    if (panel) {
        panel->EnsurePlayheadVisible(tick);
    }
}

void PianoRollPanel_JumpToTick(PianoRollPanel *panel, uint32_t tick) {
    if (panel) {
        panel->JumpToTick(tick);
    }
}

uint32_t PianoRollPanel_GetDocumentEndTick(PianoRollPanel *panel) {
    if (!panel) {
        return 0;
    }
    return panel->GetVisibleDocumentEndTick();
}

void PianoRollPanel_SetUserEndTick(PianoRollPanel *panel, uint32_t tick) {
    if (panel) {
        panel->SetUserEndTick(tick);
    }
}

uint32_t PianoRollPanel_GetUserEndTick(PianoRollPanel *panel) {
    if (!panel) {
        return 0;
    }
    return panel->GetUserEndTick();
}

void PianoRollPanel_ClearPlayhead(PianoRollPanel *panel) {
    if (panel) {
        panel->ClearPlayhead();
    }
}

void PianoRollPanel_ScrollToC4Center(PianoRollPanel *panel) {
    if (panel) {
        panel->ScrollToC4Center();
    }
}

void PianoRollPanel_Refresh(PianoRollPanel *panel) {
    if (panel) {
        panel->Refresh();
    }
}
