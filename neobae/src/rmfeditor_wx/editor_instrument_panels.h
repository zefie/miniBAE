/****************************************************************************
 *
 * editor_instrument_panels.h
 *
 * Reusable instrument editor panel widgets shared between the instrument
 * editor dialog and the bank editor panel.
 *
 * Contains:
 *   - FourCharLabel helpers and ADSR/LFO label tables
 *   - ADSRGraphPanel       (envelope visualisation)
 *   - LFOWaveformGraphPanel (LFO waveform visualisation)
 *   - WaveformPanelExt     (sample waveform display with loop editing)
 *   - PianoKeyboardPanel   (interactive piano keyboard for preview)
 *   - InstrumentParamsPanel (instrument ADSR/LFO/LPF/flags panel)
 *   - SampleParamsPanel    (sample properties, loops, codec, waveform)
 *
 ****************************************************************************/

#ifndef EDITOR_INSTRUMENT_PANELS_H
#define EDITOR_INSTRUMENT_PANELS_H

#include <stdint.h>
#include <algorithm>
#include <cmath>
#include <climits>
#include <functional>
#include <set>

#include <wx/dcbuffer.h>
#include <wx/timer.h>
#include <wx/wx.h>

extern "C" {
#include "NeoBAE.h"
}

/* ====================================================================== */
/*  FOUR_CHAR helpers for readable dropdown labels                        */
/* ====================================================================== */

struct FourCharLabel {
    int32_t value;
    char const *label;
};

static FourCharLabel const kADSRFlagLabels[] = {
    {0,                                           "OFF"},
    {(int32_t)FOUR_CHAR('L','I','N','E'),         "LINEAR_RAMP"},
    {(int32_t)FOUR_CHAR('S','U','S','T'),         "SUSTAIN"},
    {(int32_t)FOUR_CHAR('L','A','S','T'),         "TERMINATE"},
    {(int32_t)FOUR_CHAR('G','O','T','O'),         "GOTO"},
    {(int32_t)FOUR_CHAR('G','O','S','T'),         "GOTO_COND"},
    {(int32_t)FOUR_CHAR('R','E','L','S'),         "RELEASE"},
};
static int const kADSRFlagCount = (int)(sizeof(kADSRFlagLabels) / sizeof(kADSRFlagLabels[0]));

static FourCharLabel const kLFODestLabels[] = {
    {(int32_t)FOUR_CHAR('V','O','L','U'),         "VOLUME"},
    {(int32_t)FOUR_CHAR('P','I','T','C'),         "PITCH"},
    {(int32_t)FOUR_CHAR('S','P','A','N'),         "STEREO_PAN"},
    {(int32_t)FOUR_CHAR('P','A','N',' '),         "STEREO_PAN_2"},
    {(int32_t)FOUR_CHAR('L','P','F','R'),         "LPF_FREQUENCY"},
    {(int32_t)FOUR_CHAR('L','P','R','E'),         "LPF_DEPTH"},
    {(int32_t)FOUR_CHAR('L','P','A','M'),         "LOW_PASS_AMOUNT"},
};
static int const kLFODestCount = (int)(sizeof(kLFODestLabels) / sizeof(kLFODestLabels[0]));

static FourCharLabel const kLFOShapeLabels[] = {
    {0,                                           "None"},
    {(int32_t)FOUR_CHAR('S','I','N','E'),         "SINE"},
    {(int32_t)FOUR_CHAR('T','R','I','A'),         "TRIANGLE"},
    {(int32_t)FOUR_CHAR('S','Q','U','A'),         "SQUARE"},
    {(int32_t)FOUR_CHAR('S','Q','U','2'),         "SQUARE_SYNC"},
    {(int32_t)FOUR_CHAR('S','A','W','T'),         "SAWTOOTH"},
    {(int32_t)FOUR_CHAR('S','A','W','2'),         "SAWTOOTH_SYNC"},
};
static int const kLFOShapeCount = (int)(sizeof(kLFOShapeLabels) / sizeof(kLFOShapeLabels[0]));

static inline int FindLabelIndex(FourCharLabel const *table, int count, int32_t value) {
    for (int i = 0; i < count; i++) {
        if (table[i].value == value) return i;
    }
    return 0;
}

static inline void FillChoice(wxChoice *choice, FourCharLabel const *table, int count) {
    for (int i = 0; i < count; i++) {
        choice->Append(table[i].label);
    }
}

static inline char const *FourCharFlagName(int32_t flags) {
    if (flags == (int32_t)FOUR_CHAR('L','I','N','E')) return "LINEAR_RAMP";
    if (flags == (int32_t)FOUR_CHAR('S','U','S','T')) return "SUSTAIN";
    if (flags == (int32_t)FOUR_CHAR('L','A','S','T')) return "TERMINATE";
    if (flags == (int32_t)FOUR_CHAR('G','O','T','O')) return "GOTO";
    if (flags == (int32_t)FOUR_CHAR('G','O','S','T')) return "GOTO_COND";
    if (flags == (int32_t)FOUR_CHAR('R','E','L','S')) return "RELEASE";
    return "OFF";
}

static inline char const *LFODestName(int32_t dest) {
    for (int i = 0; i < kLFODestCount; i++) {
        if (kLFODestLabels[i].value == dest) return kLFODestLabels[i].label;
    }
    return "UNKNOWN";
}

static inline char const *LFOShapeName(int32_t shape) {
    for (int i = 0; i < kLFOShapeCount; i++) {
        if (kLFOShapeLabels[i].value == shape) return kLFOShapeLabels[i].label;
    }
    return "UNKNOWN";
}

/* ====================================================================== */
/*  WaveformPanelExt                                                       */
/* ====================================================================== */

class WaveformPanelExt : public wxPanel {
public:
    explicit WaveformPanelExt(wxWindow *parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(480, 150), wxBORDER_SIMPLE),
          m_waveData(nullptr), m_frameCount(0), m_bitSize(16), m_channels(1),
          m_loopStart(0), m_loopEnd(0), m_dragMode(DragMode::None) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &WaveformPanelExt::OnPaint, this);
        Bind(wxEVT_LEFT_DOWN, &WaveformPanelExt::OnLeftDown, this);
        Bind(wxEVT_LEFT_UP, &WaveformPanelExt::OnLeftUp, this);
        Bind(wxEVT_MOTION, &WaveformPanelExt::OnMouseMove, this);
        Bind(wxEVT_RIGHT_DOWN, &WaveformPanelExt::OnRightDown, this);
        Bind(wxEVT_MOUSE_CAPTURE_LOST, &WaveformPanelExt::OnMouseCaptureLost, this);
    }

    void SetWaveform(void const *waveData, uint32_t frameCount, uint16_t bitSize, uint16_t channels) {
        m_waveData = waveData;
        m_frameCount = frameCount;
        m_bitSize = bitSize;
        m_channels = std::max<uint16_t>(1, channels);
        ClampLoopToFrameCount();
        Refresh();
    }

    void SetLoopPoints(uint32_t loopStart, uint32_t loopEnd) {
        m_loopStart = loopStart;
        m_loopEnd = loopEnd;
        ClampLoopToFrameCount();
        Refresh();
    }

    void SetLoopChangedCallback(std::function<void(uint32_t, uint32_t)> callback) {
        m_loopChanged = std::move(callback);
    }

private:
    enum class DragMode {
        None,
        Start,
        End,
        Region,
    };

    void const *m_waveData;
    uint32_t m_frameCount;
    uint16_t m_bitSize, m_channels;
    uint32_t m_loopStart, m_loopEnd;
    DragMode m_dragMode;
    int m_dragAnchorX = 0;
    uint32_t m_dragAnchorLoopStart = 0;
    uint32_t m_dragAnchorLoopEnd = 0;
    std::function<void(uint32_t, uint32_t)> m_loopChanged;

    float SampleAt(uint32_t frame, int channel) const {
        if (!m_waveData || m_frameCount == 0 || frame >= m_frameCount || m_channels == 0) return 0.0f;
        int clampedChannel = std::clamp(channel, 0, (int)m_channels - 1);
        uint32_t sampleIndex = frame * (uint32_t)m_channels + (uint32_t)clampedChannel;
        if (m_bitSize == 8) {
            unsigned char const *pcm = static_cast<unsigned char const *>(m_waveData);
            return (static_cast<float>(pcm[sampleIndex]) - 128.0f) / 128.0f;
        }
        if (m_bitSize == 16) {
            int16_t const *pcm = static_cast<int16_t const *>(m_waveData);
            return static_cast<float>(pcm[sampleIndex]) / 32768.0f;
        }
        return 0.0f;
    }

    bool HasLoop() const {
        return m_frameCount > 0 && m_loopEnd > m_loopStart;
    }

    void ClampLoopToFrameCount() {
        if (m_frameCount == 0) {
            m_loopStart = 0;
            m_loopEnd = 0;
            return;
        }
        m_loopStart = std::min(m_loopStart, m_frameCount - 1);
        m_loopEnd = std::min(m_loopEnd, m_frameCount);
        if (m_loopEnd <= m_loopStart) {
            m_loopStart = 0;
            m_loopEnd = 0;
        }
    }

    uint32_t FrameFromX(int x) const {
        int width = std::max(1, GetClientSize().x - 1);
        int clampedX = std::clamp(x, 0, width);
        if (m_frameCount == 0) {
            return 0;
        }
        return (uint32_t)(((uint64_t)clampedX * (uint64_t)m_frameCount) / (uint64_t)width);
    }

    int XFromFrame(uint32_t frame) const {
        int width = std::max(1, GetClientSize().x - 1);
        if (m_frameCount == 0) {
            return 0;
        }
        uint32_t clampedFrame = std::min(frame, m_frameCount);
        return (int)(((uint64_t)clampedFrame * (uint64_t)width) / (uint64_t)m_frameCount);
    }

    void NotifyLoopChanged() {
        if (m_loopChanged) {
            m_loopChanged(m_loopStart, m_loopEnd);
        }
    }

    void DrawChannel(wxDC &dc,
                     wxSize const &size,
                     int channel,
                     int top,
                     int bottom,
                     wxColour const &waveColour,
                     wxColour const &centerColour) const {
        int laneHeight = std::max(1, bottom - top);
        int centerY = top + laneHeight / 2;
        dc.SetPen(wxPen(centerColour, 1));
        dc.DrawLine(0, centerY, size.x, centerY);

        if (m_frameCount == 0 || size.x <= 0) return;

        dc.SetPen(wxPen(waveColour, 1));
        int amp = std::max(1, (laneHeight / 2) - 3);

        // Use floating-point mapping so that samples smaller than the panel
        // width are drawn spread across all pixels (zoomed in), and large
        // samples are correctly min/max-bucketed into each pixel column.
        float framesPerPixel = (float)m_frameCount / (float)size.x;
        for (int x = 0; x < size.x; ++x) {
            float fStart = (float)x * framesPerPixel;
            float fEnd   = fStart + framesPerPixel;
            uint32_t iStart = (uint32_t)fStart;
            uint32_t iEnd   = std::min<uint32_t>(m_frameCount, (uint32_t)std::ceil(fEnd));
            if (iStart >= m_frameCount) break;
            if (iEnd <= iStart) iEnd = iStart + 1;

            float minV = 1.0f;
            float maxV = -1.0f;
            for (uint32_t f = iStart; f < iEnd; ++f) {
                float v = SampleAt(f, channel);
                if (v < minV) minV = v;
                if (v > maxV) maxV = v;
            }
            int y1 = centerY - (int)(maxV * amp);
            int y2 = centerY - (int)(minV * amp);
            dc.DrawLine(x, y1, x, y2);
        }
    }

    void OnPaint(wxPaintEvent &) {
        wxAutoBufferedPaintDC dc(this);
        wxSize size = GetClientSize();
        dc.SetBackground(wxBrush(wxColour(22, 22, 22)));
        dc.Clear();

        bool hasLoop = (m_loopEnd > m_loopStart && m_frameCount > 0 && size.x > 2);
        if (hasLoop) {
            int xS = (int)((int64_t)m_loopStart * size.x / m_frameCount);
            int xE = (int)((int64_t)m_loopEnd * size.x / m_frameCount);
            if (xE > xS) {
                dc.SetBrush(wxBrush(wxColour(0, 55, 22)));
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.DrawRectangle(xS, 0, xE - xS, size.y);
            }
        }
        if (!m_waveData || m_frameCount == 0 || size.x <= 2 || size.y <= 2) {
            dc.SetTextForeground(wxColour(200, 200, 200));
            dc.DrawText("No waveform data", 10, 10);
            return;
        }
        if (m_channels >= 2) {
            int splitY = size.y / 2;
            dc.SetPen(wxPen(wxColour(45, 45, 45), 1));
            dc.DrawLine(0, splitY, size.x, splitY);
            DrawChannel(dc, size, 0, 0, splitY, wxColour(98, 215, 255), wxColour(70, 70, 70));
            DrawChannel(dc, size, 1, splitY, size.y, wxColour(255, 188, 98), wxColour(70, 70, 70));
        } else {
            DrawChannel(dc, size, 0, 0, size.y, wxColour(98, 215, 255), wxColour(70, 70, 70));
        }
        if (hasLoop) {
            int xS = (int)((int64_t)m_loopStart * size.x / m_frameCount);
            int xE = (int)((int64_t)m_loopEnd * size.x / m_frameCount);
            dc.SetPen(wxPen(wxColour(0, 220, 80), 2));
            dc.DrawLine(xS, 0, xS, size.y);
            dc.SetPen(wxPen(wxColour(255, 160, 0), 2));
            dc.DrawLine(xE, 0, xE, size.y);

            dc.SetBrush(wxBrush(wxColour(0, 220, 80)));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawCircle(xS, 8, 4);
            dc.SetBrush(wxBrush(wxColour(255, 160, 0)));
            dc.DrawCircle(xE, 8, 4);
        }
    }

    void OnLeftDown(wxMouseEvent &event) {
        if (!HasLoop()) {
            event.Skip();
            return;
        }
        int x = event.GetX();
        int startX = XFromFrame(m_loopStart);
        int endX = XFromFrame(m_loopEnd);

        if (std::abs(x - startX) <= 6) {
            m_dragMode = DragMode::Start;
        } else if (std::abs(x - endX) <= 6) {
            m_dragMode = DragMode::End;
        } else if (x > startX + 6 && x < endX - 6) {
            m_dragMode = DragMode::Region;
            m_dragAnchorX = x;
            m_dragAnchorLoopStart = m_loopStart;
            m_dragAnchorLoopEnd = m_loopEnd;
        } else {
            event.Skip();
            return;
        }
        if (!HasCapture()) {
            CaptureMouse();
        }
    }

    void OnLeftUp(wxMouseEvent &) {
        if (m_dragMode != DragMode::None) {
            NotifyLoopChanged();
        }
        m_dragMode = DragMode::None;
        if (HasCapture()) {
            ReleaseMouse();
        }
    }

    void OnMouseMove(wxMouseEvent &event) {
        if (m_dragMode == DragMode::None || !event.LeftIsDown() || m_frameCount == 0) {
            event.Skip();
            return;
        }
        if (m_dragMode == DragMode::Start) {
            uint32_t frame = FrameFromX(event.GetX());
            uint32_t newStart = std::min(frame, (m_loopEnd > 0) ? (m_loopEnd - 1) : 0);
            if (newStart != m_loopStart) {
                m_loopStart = newStart;
                Refresh();
            }
        } else if (m_dragMode == DragMode::End) {
            uint32_t frame = FrameFromX(event.GetX());
            uint32_t newEnd = std::max(frame, m_loopStart + 1);
            newEnd = std::min(newEnd, m_frameCount);
            if (newEnd != m_loopEnd) {
                m_loopEnd = newEnd;
                Refresh();
            }
        } else if (m_dragMode == DragMode::Region) {
            int delta = event.GetX() - m_dragAnchorX;
            int width = std::max(1, GetClientSize().x - 1);
            uint32_t loopLen = m_dragAnchorLoopEnd - m_dragAnchorLoopStart;
            int64_t frameDelta = ((int64_t)delta * (int64_t)m_frameCount) / width;
            int64_t newStart = (int64_t)m_dragAnchorLoopStart + frameDelta;
            if (newStart < 0) newStart = 0;
            if ((uint64_t)newStart + loopLen > m_frameCount)
                newStart = (int64_t)m_frameCount - (int64_t)loopLen;
            uint32_t ns = (uint32_t)newStart;
            uint32_t ne = ns + loopLen;
            if (ns != m_loopStart || ne != m_loopEnd) {
                m_loopStart = ns;
                m_loopEnd = ne;
                Refresh();
            }
        }
    }

    void OnRightDown(wxMouseEvent &event) {
        wxMenu menu;
        int idSetStart = wxWindow::NewControlId();
        int idSetEnd = wxWindow::NewControlId();
        int idClearLoop = wxWindow::NewControlId();
        int selectedId;
        uint32_t frame;

        if (m_frameCount == 0) {
            return;
        }
        frame = FrameFromX(event.GetX());
        menu.Append(idSetStart, "Set Loop Start Here");
        menu.Append(idSetEnd, "Set Loop End Here");
        menu.AppendSeparator();
        menu.Append(idClearLoop, "Clear Loop");
        menu.Enable(idClearLoop, HasLoop());

        selectedId = GetPopupMenuSelectionFromUser(menu, event.GetPosition());
        if (selectedId == idSetStart) {
            uint32_t end = HasLoop() ? m_loopEnd : std::min<uint32_t>(m_frameCount, frame + 1);
            m_loopStart = std::min(frame, (end > 0) ? (end - 1) : 0);
            m_loopEnd = std::max(end, m_loopStart + 1);
            m_loopEnd = std::min(m_loopEnd, m_frameCount);
            NotifyLoopChanged();
            Refresh();
        } else if (selectedId == idSetEnd) {
            uint32_t start = HasLoop() ? m_loopStart : std::min<uint32_t>(frame, m_frameCount - 1);
            uint32_t end = std::max(frame, start + 1);
            m_loopStart = start;
            m_loopEnd = std::min(end, m_frameCount);
            NotifyLoopChanged();
            Refresh();
        } else if (selectedId == idClearLoop) {
            m_loopStart = 0;
            m_loopEnd = 0;
            NotifyLoopChanged();
            Refresh();
        }
    }

    void OnMouseCaptureLost(wxMouseCaptureLostEvent &) {
        if (m_dragMode != DragMode::None) {
            NotifyLoopChanged();
        }
        m_dragMode = DragMode::None;
    }
};

/* ====================================================================== */
/*  ADSR Envelope graph panel                                              */
/* ====================================================================== */

class ADSRGraphPanel : public wxPanel {
public:
    explicit ADSRGraphPanel(wxWindow *parent)
                : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(480, 96), wxBORDER_SIMPLE),
          m_stageCount(0) {
        memset(m_levels, 0, sizeof(m_levels));
        memset(m_times, 0, sizeof(m_times));
        memset(m_flags, 0, sizeof(m_flags));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &ADSRGraphPanel::OnPaint, this);
    }

    void SetEnvelope(int stageCount, int32_t const *levels, int32_t const *times, int32_t const *flags) {
        m_stageCount = std::clamp(stageCount, 0, BAE_EDITOR_MAX_ADSR_STAGES);
        for (int i = 0; i < m_stageCount; i++) {
            m_levels[i] = levels[i];
            m_times[i] = times[i];
            m_flags[i] = flags[i];
        }
        Refresh();
    }

private:
    int m_stageCount;
    int32_t m_levels[BAE_EDITOR_MAX_ADSR_STAGES];
    int32_t m_times[BAE_EDITOR_MAX_ADSR_STAGES];   /* microseconds */
    int32_t m_flags[BAE_EDITOR_MAX_ADSR_STAGES];

    static wxColour StageColour(int32_t flags) {
        if (flags == (int32_t)FOUR_CHAR('S','U','S','T')) return wxColour(80, 200, 80);
        if (flags == (int32_t)FOUR_CHAR('R','E','L','S')) return wxColour(200, 80, 80);
        if (flags == (int32_t)FOUR_CHAR('L','A','S','T')) return wxColour(200, 200, 60);
        if (flags == (int32_t)FOUR_CHAR('G','O','T','O')) return wxColour(140, 100, 220);
        if (flags == (int32_t)FOUR_CHAR('G','O','S','T')) return wxColour(100, 140, 220);
        return wxColour(98, 215, 255);  /* LINEAR_RAMP / default */
    }

    static char const *StageName(int32_t flags) {
        if (flags == (int32_t)FOUR_CHAR('L','I','N','E')) return "LIN";
        if (flags == (int32_t)FOUR_CHAR('S','U','S','T')) return "SUS";
        if (flags == (int32_t)FOUR_CHAR('L','A','S','T')) return "END";
        if (flags == (int32_t)FOUR_CHAR('G','O','T','O')) return "GOTO";
        if (flags == (int32_t)FOUR_CHAR('G','O','S','T')) return "GOST";
        if (flags == (int32_t)FOUR_CHAR('R','E','L','S')) return "REL";
        return "OFF";
    }

    void OnPaint(wxPaintEvent &) {
        wxAutoBufferedPaintDC dc(this);
        wxSize size = GetClientSize();
        int w = size.x;
        int h = size.y;

        dc.SetBackground(wxBrush(wxColour(22, 22, 22)));
        dc.Clear();

        if (m_stageCount <= 0 || w < 20 || h < 20) {
            dc.SetTextForeground(wxColour(120, 120, 120));
            dc.DrawText("No ADSR stages", 8, h / 2 - 6);
            return;
        }

        int const margin = 6;
        int const graphL = margin + 30;
        int const graphR = w - margin;
        int const graphT = margin;
        int const graphB = h - margin - 14;  /* leave room for labels */
        int const graphW = graphR - graphL;
        int const graphH = graphB - graphT;

        if (graphW < 10 || graphH < 10) return;

        /* Find level range and total time for scaling */
        int32_t minLevel = 0;
        int32_t maxLevel = 0;
        int64_t totalTime = 0;
        for (int i = 0; i < m_stageCount; i++) {
            if (m_levels[i] < minLevel) minLevel = m_levels[i];
            if (m_levels[i] > maxLevel) maxLevel = m_levels[i];
            totalTime += std::abs((int64_t)m_times[i]);
        }
        if (maxLevel <= minLevel) maxLevel = minLevel + 1;
        if (totalTime <= 0) totalTime = 1;

        /* Y-axis helper: level -> pixel y */
        auto levelToY = [&](int32_t level) -> int {
            double norm = (double)(level - minLevel) / (double)(maxLevel - minLevel);
            return graphB - (int)(norm * graphH);
        };

        /* Draw gridlines */
        dc.SetPen(wxPen(wxColour(50, 50, 50), 1));
        dc.DrawLine(graphL, graphT, graphL, graphB);
        dc.DrawLine(graphL, graphB, graphR, graphB);
        /* Zero level line */
        if (minLevel < 0 && maxLevel > 0) {
            int y0 = levelToY(0);
            dc.SetPen(wxPen(wxColour(60, 60, 60), 1, wxPENSTYLE_DOT));
            dc.DrawLine(graphL, y0, graphR, y0);
        }

        /* Y axis labels */
        dc.SetFont(wxFont(7, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL));
        dc.SetTextForeground(wxColour(140, 140, 140));
        dc.DrawText(wxString::Format("%d", maxLevel), margin, graphT - 2);
        dc.DrawText(wxString::Format("%d", minLevel), margin, graphB - 8);

        /* Draw envelope segments */
        int32_t prevLevel = 0;  /* starts at zero before first stage */
        int64_t cumulativeTime = 0;

        for (int i = 0; i < m_stageCount; i++) {
            int64_t stageTime = std::abs((int64_t)m_times[i]);
            int x1 = graphL + (int)((double)cumulativeTime / (double)totalTime * graphW);
            int x2 = graphL + (int)((double)(cumulativeTime + stageTime) / (double)totalTime * graphW);
            int y1 = levelToY(prevLevel);
            int y2 = levelToY(m_levels[i]);

            wxColour colour = StageColour(m_flags[i]);

            /* Stage background band */
            dc.SetBrush(wxBrush(wxColour(colour.Red() / 6, colour.Green() / 6, colour.Blue() / 6)));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(x1, graphT, std::max(1, x2 - x1), graphH);

            /* Envelope line */
            dc.SetPen(wxPen(colour, 2));
            if (m_flags[i] == (int32_t)FOUR_CHAR('S','U','S','T') ||
                m_flags[i] == 0) {
                /* Hold at level (sustain or OFF) */
                int yHold = levelToY(m_levels[i]);
                dc.DrawLine(x1, y1, x1, yHold);
                dc.DrawLine(x1, yHold, x2, yHold);
            } else {
                /* Ramp from previous level to target */
                dc.DrawLine(x1, y1, x2, y2);
            }

            /* Stage label at bottom */
            dc.SetFont(wxFont(7, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL));
            dc.SetTextForeground(colour);
            int labelX = (x1 + x2) / 2 - 8;
            dc.DrawText(StageName(m_flags[i]), std::max(graphL, labelX), graphB + 2);

            cumulativeTime += stageTime;
            prevLevel = m_levels[i];
        }

        /* Draw stage boundary ticks */
        cumulativeTime = 0;
        dc.SetPen(wxPen(wxColour(80, 80, 80), 1, wxPENSTYLE_DOT));
        for (int i = 0; i < m_stageCount - 1; i++) {
            cumulativeTime += std::abs((int64_t)m_times[i]);
            int x = graphL + (int)((double)cumulativeTime / (double)totalTime * graphW);
            dc.DrawLine(x, graphT, x, graphB);
        }
    }
};

/* ====================================================================== */
/*  LFO waveform graph panel                                               */
/* ====================================================================== */

class LFOWaveformGraphPanel : public wxPanel {
public:
    explicit LFOWaveformGraphPanel(wxWindow *parent)
                : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(480, 72), wxBORDER_SIMPLE),
          m_waveShape((int32_t)FOUR_CHAR('S','I','N','E')),
          m_period(0),
          m_dcFeed(0),
          m_level(0) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &LFOWaveformGraphPanel::OnPaint, this);
    }

    void SetLFO(int32_t waveShape, int32_t period, int32_t dcFeed, int32_t level) {
        m_waveShape = waveShape;
        m_period = period;
        m_dcFeed = dcFeed;
        m_level = level;
        Refresh();
    }

private:
    int32_t m_waveShape;
    int32_t m_period;
    int32_t m_dcFeed;
    int32_t m_level;

    static double EvaluateWave(int32_t waveShape, double phase) {
        phase -= std::floor(phase);
        if (phase < 0.0) {
            phase += 1.0;
        }

        switch (waveShape) {
            case (int32_t)FOUR_CHAR('T','R','I','A'):
            {
                double tri = 4.0 * std::fabs(phase - 0.5) - 1.0;
                return -tri;
            }
            case (int32_t)FOUR_CHAR('S','Q','U','A'):
                return (phase < 0.5) ? 1.0 : -1.0;
            case (int32_t)FOUR_CHAR('S','Q','U','2'):
                return (phase < 0.25) ? 1.0 : -1.0;
            case (int32_t)FOUR_CHAR('S','A','W','T'):
                return (2.0 * phase) - 1.0;
            case (int32_t)FOUR_CHAR('S','A','W','2'):
                return 1.0 - (2.0 * phase);
            case (int32_t)FOUR_CHAR('S','I','N','E'):
            default:
                return std::sin(phase * 2.0 * 3.14159265358979323846);
        }
    }

    void OnPaint(wxPaintEvent &) {
        wxAutoBufferedPaintDC dc(this);
        wxSize size = GetClientSize();
        int w = size.x;
        int h = size.y;

        dc.SetBackground(wxBrush(wxColour(22, 22, 22)));
        dc.Clear();

        if (w < 20 || h < 20) {
            return;
        }

        int const margin = 6;
        int const left = margin;
        int const right = w - margin;
        int const top = margin;
        int const bottom = h - margin;
        int const width = right - left;
        int const height = bottom - top;
        int const centerY = top + height / 2;

        dc.SetPen(wxPen(wxColour(55, 55, 55), 1));
        dc.DrawRectangle(left, top, width, height);
        dc.DrawLine(left, centerY, right, centerY);

        if (m_period == 0 && m_level == 0 && m_dcFeed == 0) {
            dc.SetTextForeground(wxColour(120, 120, 120));
            dc.DrawText("LFO preview", left + 8, centerY - 6);
            return;
        }

        int64_t span = std::max<int64_t>(1, std::llabs((int64_t)m_level) + std::llabs((int64_t)m_dcFeed));

        dc.SetPen(wxPen(wxColour(98, 215, 255), 2));
        wxPoint prev;
        bool hasPrev = false;

        for (int x = 0; x < width; ++x) {
            double phase = (width > 1) ? ((double)x / (double)(width - 1)) : 0.0;
            double wave = EvaluateWave(m_waveShape, phase);
            double value = wave * (double)m_level + (double)m_dcFeed;
            double normalized = value / (double)span;
            normalized = std::clamp(normalized, -1.0, 1.0);
            int y = centerY - (int)(normalized * (height / 2 - 2));
            wxPoint p(left + x, y);

            if (hasPrev) {
                dc.DrawLine(prev, p);
            }
            prev = p;
            hasPrev = true;
        }

        dc.SetTextForeground(wxColour(150, 150, 150));
        dc.SetFont(wxFont(7, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL));
        dc.DrawText(wxString::Format("period=%d  level=%d  dc=%d", m_period, m_level, m_dcFeed), left + 6, top + 4);
    }
};

/* ====================================================================== */
/*  Piano keyboard panel                                                   */
/* ====================================================================== */

class PianoKeyboardPanel : public wxPanel {
public:
    explicit PianoKeyboardPanel(wxWindow *parent,
                                std::function<void(int)> noteOnCallback,
                                std::function<void()> noteOffCallback)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(600, 60), wxBORDER_SIMPLE),
          m_noteOn(std::move(noteOnCallback)),
          m_noteOff(std::move(noteOffCallback)),
          m_baseNote(36),  /* C2 — scroll to reach 0-127 */
                    m_octaves(4),
                    m_pressedKey(-1),
                    m_noteIsOn(false),
                    m_releaseWatchdog(this) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &PianoKeyboardPanel::OnPaint, this);
        Bind(wxEVT_LEFT_DOWN, &PianoKeyboardPanel::OnMouseDown, this);
        Bind(wxEVT_LEFT_UP, &PianoKeyboardPanel::OnMouseUp, this);
        Bind(wxEVT_MOTION, &PianoKeyboardPanel::OnMouseMove, this);
        Bind(wxEVT_LEAVE_WINDOW, &PianoKeyboardPanel::OnMouseLeave, this);
        Bind(wxEVT_MOUSE_CAPTURE_LOST, &PianoKeyboardPanel::OnMouseCaptureLost, this);
        Bind(wxEVT_MOUSE_CAPTURE_CHANGED, &PianoKeyboardPanel::OnMouseCaptureChanged, this);
                Bind(wxEVT_TIMER, &PianoKeyboardPanel::OnReleaseWatchdog, this);
        Bind(wxEVT_SIZE, &PianoKeyboardPanel::OnSize, this);
        Bind(wxEVT_MOUSEWHEEL, &PianoKeyboardPanel::OnMouseWheel, this);
        UpdateVisibleRangeForWidth(GetClientSize().x);
    }

    void ForceStop() {
        StopPressedNote();
        if (HasCapture()) {
            ReleaseMouse();
        }
    }

    void SetBaseNote(int baseNote) {
        int maxBase = std::max(0, 127 - (m_octaves * 12));
        int clamped = std::clamp(baseNote, 0, maxBase);
        if (clamped != m_baseNote) {
            m_baseNote = clamped;
            Refresh();
        }
    }

    /* Scroll so that 'note' appears in the centre of the visible keyboard. */
    void CenterOnNote(int note) {
        SetBaseNote(note - (m_octaves * 12 / 2));
    }

    void SetExternalPressedNote(int note) {
        int clamped = std::clamp(note, 0, 127);
        m_externalPressedKeys = {clamped};
        Refresh();
    }

    void ClearExternalPressedNote() {
        if (!m_externalPressedKeys.empty()) {
            m_externalPressedKeys.clear();
            Refresh();
        }
    }

    void SetExternalPressedNotes(std::set<int> const &notes) {
        m_externalPressedKeys = notes;
        Refresh();
    }

    void UpdateVisibleRangeForWidth(int width) {
        int desiredWhiteKeys;
        int desiredOctaves;

        if (width <= 0) {
            return;
        }
        desiredWhiteKeys = std::clamp(width / 26, 21, 42);
        desiredOctaves = std::clamp(desiredWhiteKeys / 7, 3, 6);
        if (desiredOctaves != m_octaves) {
            m_octaves = desiredOctaves;
            m_baseNote = std::clamp(m_baseNote, 0, std::max(0, 127 - (m_octaves * 12)));
            Refresh();
        }
    }

private:
    std::function<void(int)> m_noteOn;
    std::function<void()> m_noteOff;
    int m_baseNote;
    int m_octaves;
    int m_pressedKey;
    std::set<int> m_externalPressedKeys;
    bool m_noteIsOn;
    wxTimer m_releaseWatchdog;

    static bool IsBlackKey(int noteInOctave) {
        return noteInOctave == 1 || noteInOctave == 3 || noteInOctave == 6 ||
               noteInOctave == 8 || noteInOctave == 10;
    }

    int VisibleEndNote() const {
        return std::min(127, m_baseNote + m_octaves * 12);
    }

    bool SafeInvokeNoteOn(int note) {
        try {
            if (m_noteOn) {
                m_noteOn(note);
            }
            return true;
        } catch (std::exception const &ex) {
            BAE_PRINTF( "[nbstudio] piano preview note-on exception: %s\n", ex.what());
        } catch (...) {
            BAE_PRINTF( "[nbstudio] piano preview note-on unknown exception\n");
        }

        if (m_releaseWatchdog.IsRunning()) {
            m_releaseWatchdog.Stop();
        }
        m_noteIsOn = false;
        if (HasCapture()) {
            ReleaseMouse();
        }
        return false;
    }

    void SafeInvokeNoteOff() {
        try {
            if (m_noteOff) {
                m_noteOff();
            }
        } catch (std::exception const &ex) {
            BAE_PRINTF( "[nbstudio] piano preview note-off exception: %s\n", ex.what());
        } catch (...) {
            BAE_PRINTF( "[nbstudio] piano preview note-off unknown exception\n");
        }
    }

    int WhiteIndexForNote(int note) const {
        int whiteIndex = 0;
        int end = std::min(note - 1, VisibleEndNote());
        for (int n = m_baseNote; n <= end; ++n) {
            if (!IsBlackKey(n % 12)) {
                ++whiteIndex;
            }
        }
        return whiteIndex;
    }

    bool WhiteKeyRectForNote(int note, wxSize const &size, float whiteKeyWidth, int *x1, int *x2) const {
        int end = VisibleEndNote();
        if (note < m_baseNote || note > end || IsBlackKey(note % 12)) return false;
        int wi = WhiteIndexForNote(note);
        *x1 = (int)(wi * whiteKeyWidth);
        *x2 = (int)((wi + 1) * whiteKeyWidth);
        return true;
    }

    void StopPressedNote() {
        if (m_noteIsOn) {
            SafeInvokeNoteOff();
            m_noteIsOn = false;
        }
        if (m_releaseWatchdog.IsRunning()) {
            m_releaseWatchdog.Stop();
        }
        m_pressedKey = -1;
    }

    int NoteFromPosition(wxPoint const &pos) const {
        wxSize size = GetClientSize();
        if (size.x <= 0 || size.y <= 0) return -1;
        int end = VisibleEndNote();
        int whiteCount = 0;
        for (int n = m_baseNote; n <= end; ++n) {
            if (!IsBlackKey(n % 12)) ++whiteCount;
        }
        if (whiteCount <= 0) return -1;
        float whiteKeyWidth = (float)size.x / (float)whiteCount;
        int blackKeyHeight = (int)(size.y * 0.6f);

        /* Check black keys first (they overlap white keys) */
        if (pos.y < blackKeyHeight) {
            for (int n = m_baseNote; n <= end; ++n) {
                if (!IsBlackKey(n % 12)) continue;
                int prevWhite = WhiteIndexForNote(n);
                float bw = whiteKeyWidth * 0.7f;
                float bx = prevWhite * whiteKeyWidth - bw / 2.0f;
                if (pos.x >= (int)bx && pos.x < (int)(bx + bw)) return n;
            }
        }
        /* White keys */
        for (int n = m_baseNote; n <= end; ++n) {
            if (IsBlackKey(n % 12)) continue;
            int wx1, wx2;
            if (WhiteKeyRectForNote(n, size, whiteKeyWidth, &wx1, &wx2)) {
                if (pos.x >= wx1 && pos.x < wx2) return n;
            }
        }
        return -1;
    }

    void OnMouseDown(wxMouseEvent &event) {
        int note = NoteFromPosition(event.GetPosition());
        if (note < 0) return;
        StopPressedNote();
        m_pressedKey = note;
        if (SafeInvokeNoteOn(note)) {
            m_noteIsOn = true;
            m_releaseWatchdog.StartOnce(5000);
        }
        if (!HasCapture()) CaptureMouse();
        Refresh();
    }

    void OnMouseUp(wxMouseEvent &) {
        StopPressedNote();
        if (HasCapture()) ReleaseMouse();
        Refresh();
    }

    void OnMouseMove(wxMouseEvent &event) {
        if (!event.LeftIsDown() || m_pressedKey < 0) return;
        int note = NoteFromPosition(event.GetPosition());
        if (note >= 0 && note != m_pressedKey) {
            StopPressedNote();
            m_pressedKey = note;
            if (SafeInvokeNoteOn(note)) {
                m_noteIsOn = true;
                m_releaseWatchdog.StartOnce(5000);
            }
            Refresh();
        }
    }

    void OnMouseLeave(wxMouseEvent &) {
        if (m_pressedKey >= 0 && !HasCapture()) {
            StopPressedNote();
            Refresh();
        }
    }

    void OnMouseCaptureLost(wxMouseCaptureLostEvent &) {
        StopPressedNote();
        Refresh();
    }

    void OnMouseCaptureChanged(wxMouseCaptureChangedEvent &) {
        StopPressedNote();
        Refresh();
    }

    void OnReleaseWatchdog(wxTimerEvent &) {
        /* If the mouse button is still physically held, restart the watchdog
           instead of killing the note.  The timer only fires as a safety net
           in case a mouse-up event was lost (e.g. focus steal). */
        if (m_pressedKey >= 0 && wxGetMouseState().LeftIsDown()) {
            m_releaseWatchdog.StartOnce(5000);
            return;
        }
        StopPressedNote();
        if (HasCapture()) ReleaseMouse();
        Refresh();
    }

    void OnSize(wxSizeEvent &event) {
        UpdateVisibleRangeForWidth(event.GetSize().x);
        event.Skip();
    }

    void OnMouseWheel(wxMouseEvent &event) {
        int rotation = event.GetWheelRotation();
        int delta = (rotation > 0) ? -12 : 12;  /* scroll up = lower notes */
        int maxBase = std::max(0, 127 - (m_octaves * 12));
        int newBase = std::clamp(m_baseNote + delta, 0, maxBase);
        if (newBase != m_baseNote) {
            m_baseNote = newBase;
            Refresh();
        }
    }

    void OnPaint(wxPaintEvent &) {
        wxAutoBufferedPaintDC dc(this);
        wxSize size = GetClientSize();
        dc.SetBackground(wxBrush(wxColour(40, 40, 40)));
        dc.Clear();
        if (size.x <= 0 || size.y <= 0) return;

        int end = VisibleEndNote();
        int whiteCount = 0;
        for (int n = m_baseNote; n <= end; ++n) {
            if (!IsBlackKey(n % 12)) ++whiteCount;
        }
        if (whiteCount <= 0) return;
        float whiteKeyWidth = (float)size.x / (float)whiteCount;
        int blackKeyHeight = (int)(size.y * 0.6f);

        /* Draw white keys */
        for (int n = m_baseNote; n <= end; ++n) {
            if (IsBlackKey(n % 12)) continue;
            int wx1, wx2;
            if (!WhiteKeyRectForNote(n, size, whiteKeyWidth, &wx1, &wx2)) continue;
            bool pressed = (n == m_pressedKey) || (m_externalPressedKeys.count(n) > 0);
            dc.SetBrush(wxBrush(pressed ? wxColour(180, 220, 255) : wxColour(245, 245, 245)));
            dc.SetPen(wxPen(wxColour(100, 100, 100), 1));
            dc.DrawRectangle(wx1, 0, wx2 - wx1, size.y);
        }
        /* Draw C labels on white keys */
        {
            int labelSize = std::clamp((int)(whiteKeyWidth * 0.38f), 6, 11);
            wxFont labelFont(wxFontInfo(labelSize).FaceName("Arial"));
            dc.SetFont(labelFont);
            for (int n = m_baseNote; n <= end; ++n) {
                if ((n % 12) != 0) continue;  /* only C notes */
                int wx1, wx2;
                if (!WhiteKeyRectForNote(n, size, whiteKeyWidth, &wx1, &wx2)) continue;
                int octave = (n / 12) - 1;  /* MIDI C0=12, C-1=0 */
                wxString label = wxString::Format("C%d", octave);
                wxSize textSize = dc.GetTextExtent(label);
                int tx = wx1 + ((wx2 - wx1) - textSize.x) / 2;
                int ty = size.y - textSize.y - 2;
                dc.SetTextForeground(wxColour(120, 120, 120));
                dc.DrawText(label, tx, ty);
            }
        }
        /* Draw black keys */
        for (int n = m_baseNote; n <= end; ++n) {
            if (!IsBlackKey(n % 12)) continue;
            int prevWhite = WhiteIndexForNote(n);
            float bw = whiteKeyWidth * 0.7f;
            float bx = prevWhite * whiteKeyWidth - bw / 2.0f;
            bool pressed = (n == m_pressedKey) || (m_externalPressedKeys.count(n) > 0);
            dc.SetBrush(wxBrush(pressed ? wxColour(100, 140, 200) : wxColour(30, 30, 30)));
            dc.SetPen(wxPen(wxColour(20, 20, 20), 1));
            dc.DrawRectangle((int)bx, 0, (int)bw, blackKeyHeight);
        }
    }
};

/* ====================================================================== */
/*  InstrumentParamsPanel                                                 */
/*  Shared instrument parameter editor (flags, LPF, ADSR, LFO) used by   */
/*  both the bank editor panel and the instrument editor dialog.          */
/* ====================================================================== */

class InstrumentParamsPanel : public wxPanel {
public:
    explicit InstrumentParamsPanel(wxWindow *parent)
        : wxPanel(parent, wxID_ANY),
          m_currentLfoIndex(-1),
          m_loading(false)
    {
        memset(&m_extInfo, 0, sizeof(m_extInfo));
        BuildUI();
    }

    /* Populate all controls from an ExtInfo struct plus name/program.
     * The caller must ensure extInfo.flags1/flags2 contain the desired
     * flag values before calling. */
    void LoadFromExtInfo(BAERmfEditorInstrumentExtInfo const &extInfo,
                         wxString const &instName, int program) {
        m_loading = true;
        m_extInfo = extInfo;
        m_instNameText->SetValue(instName);
        m_instProgramSpin->SetValue(program);

        /* Flags */
        m_panPlacementSpin->SetValue((int)extInfo.panPlacement);
        m_chkInterpolate->SetValue((extInfo.flags1 & 0x80) != 0);
        m_chkAvoidReverb->SetValue((extInfo.flags1 & 0x01) != 0);
        m_chkSampleAndHold->SetValue((extInfo.flags1 & 0x04) != 0);
        m_chkAdvancedInterpolation->SetValue((extInfo.flags2 & 0x80) != 0);
        m_chkPlayAtSampledFreq->SetValue((extInfo.flags2 & 0x40) != 0);
        m_chkNotPolyphonic->SetValue((extInfo.flags2 & 0x04) != 0);

        /* LPF */
        m_lpfFrequency->SetValue(extInfo.LPF_frequency);
        m_lpfResonance->SetValue(extInfo.LPF_resonance);
        m_lpfLowpassAmount->SetValue(extInfo.LPF_lowpassAmount);

        /* ADSR */
        m_adsrStageSpin->SetValue((int)extInfo.volumeADSR.stageCount);
        for (int i = 0; i < BAE_EDITOR_MAX_ADSR_STAGES; i++) {
            int lvl = (i < (int)extInfo.volumeADSR.stageCount) ? extInfo.volumeADSR.stages[i].level : 0;
            int32_t tmRaw = (i < (int)extInfo.volumeADSR.stageCount) ? extInfo.volumeADSR.stages[i].time : 0;
            int fl = (i < (int)extInfo.volumeADSR.stageCount) ? extInfo.volumeADSR.stages[i].flags : 0;
            m_adsrLevel[i]->SetValue(lvl);
            m_adsrTime[i]->SetValue((double)tmRaw / 1000000.0);
            m_adsrFlags[i]->SetSelection(FindLabelIndex(kADSRFlagLabels, kADSRFlagCount, fl));
            bool visible = (i < (int)extInfo.volumeADSR.stageCount);
            m_adsrRowLabels[i]->Show(visible);
            m_adsrLevel[i]->Show(visible);
            m_adsrTime[i]->Show(visible);
            m_adsrFlags[i]->Show(visible);
        }

        /* LFO */
        m_lfoCountSpin->SetValue((int)m_extInfo.lfoCount);
        if (m_extInfo.lfoCount > 0) {
            m_currentLfoIndex = 0;
            m_lfoSelector->SetSelection(0);
        } else {
            m_currentLfoIndex = -1;
            m_lfoSelector->SetSelection(wxNOT_FOUND);
        }
        if (m_currentLfoIndex >= 0) {
            LoadLFOIntoUI(m_currentLfoIndex);
        } else {
            LoadLFOIntoUI(-1);
        }
        m_loading = false;
        RefreshADSRGraph();
        RefreshFilterEnvelopeGraph();
        Layout();
    }

    /* Clear all controls to default empty state. */
    void ClearUI() {
        m_loading = true;
        memset(&m_extInfo, 0, sizeof(m_extInfo));
        m_instNameText->SetValue("");
        m_instProgramSpin->SetValue(0);
        m_panPlacementSpin->SetValue(0);
        m_chkInterpolate->SetValue(false);
        m_chkAvoidReverb->SetValue(false);
        m_chkSampleAndHold->SetValue(false);
        m_chkAdvancedInterpolation->SetValue(false);
        m_chkPlayAtSampledFreq->SetValue(false);
        m_chkNotPolyphonic->SetValue(false);
        m_lpfFrequency->SetValue(0);
        m_lpfResonance->SetValue(0);
        m_lpfLowpassAmount->SetValue(0);
        m_adsrStageSpin->SetValue(0);
        for (int i = 0; i < BAE_EDITOR_MAX_ADSR_STAGES; i++) {
            m_adsrRowLabels[i]->Show(false);
            m_adsrLevel[i]->Show(false);
            m_adsrTime[i]->Show(false);
            m_adsrFlags[i]->Show(false);
        }
        m_lfoCountSpin->SetValue(0);
        m_currentLfoIndex = -1;
        m_lfoSelector->SetSelection(wxNOT_FOUND);
        m_loading = false;
        RefreshADSRGraph();
        RefreshLFOGraph();
        RefreshLFOEnvelopeGraph();
        RefreshFilterEnvelopeGraph();
    }

    /* Write current UI state back into the given ExtInfo struct. */
    void SaveToExtInfo(BAERmfEditorInstrumentExtInfo &extInfo) {
        /* Save current LFO first */
        int lfoSel = (m_currentLfoIndex >= 0) ? m_currentLfoIndex : m_lfoSelector->GetSelection();
        if (lfoSel >= 0) SaveLFOFromUI(lfoSel);

        /* Pan placement */
        extInfo.panPlacement = (char)std::clamp(m_panPlacementSpin->GetValue(), -128, 127);

        /* Flags */
        extInfo.flags1 &= ~(0x80 | 0x04 | 0x01);
        if (m_chkInterpolate->GetValue()) extInfo.flags1 |= 0x80;
        if (m_chkAvoidReverb->GetValue()) extInfo.flags1 |= 0x01;
        if (m_chkSampleAndHold->GetValue()) extInfo.flags1 |= 0x04;
        extInfo.flags2 &= ~(0x80 | 0x40 | 0x04);
        if (m_chkAdvancedInterpolation->GetValue()) extInfo.flags2 |= 0x80;
        if (m_chkPlayAtSampledFreq->GetValue()) extInfo.flags2 |= 0x40;
        if (m_chkNotPolyphonic->GetValue()) extInfo.flags2 |= 0x04;

        /* LPF */
        extInfo.LPF_frequency = m_lpfFrequency->GetValue();
        extInfo.LPF_resonance = m_lpfResonance->GetValue();
        extInfo.LPF_lowpassAmount = m_lpfLowpassAmount->GetValue();

        /* ADSR */
        extInfo.volumeADSR.stageCount = (uint32_t)m_adsrStageSpin->GetValue();
        for (int i = 0; i < (int)extInfo.volumeADSR.stageCount && i < BAE_EDITOR_MAX_ADSR_STAGES; i++) {
            extInfo.volumeADSR.stages[i].level = m_adsrLevel[i]->GetValue();
            extInfo.volumeADSR.stages[i].time = (int32_t)(m_adsrTime[i]->GetValue() * 1000000.0);
            int sel = m_adsrFlags[i]->GetSelection();
            extInfo.volumeADSR.stages[i].flags = (sel >= 0 && sel < kADSRFlagCount) ? kADSRFlagLabels[sel].value : 0;
        }

        /* LFOs - copy from internal state */
        extInfo.lfoCount = (uint32_t)m_lfoCountSpin->GetValue();
        for (int i = 0; i < BAE_EDITOR_MAX_LFOS; i++) {
            extInfo.lfos[i] = m_extInfo.lfos[i];
        }

#if _DEBUG
        BAE_PRINTF( "[SaveToExtInfo] lfoCount=%u lfoSel=%d\n", extInfo.lfoCount, lfoSel);
        for (uint32_t dbg = 0; dbg < extInfo.lfoCount && dbg < BAE_EDITOR_MAX_LFOS; dbg++) {
            BAE_PRINTF( "  lfo[%u] dest=0x%x period=%d shape=0x%x dc=%d level=%d adsr.stages=%u\n",
                    dbg, extInfo.lfos[dbg].destination, extInfo.lfos[dbg].period,
                    extInfo.lfos[dbg].waveShape, extInfo.lfos[dbg].DC_feed,
                    extInfo.lfos[dbg].level, extInfo.lfos[dbg].adsr.stageCount);
        }
#endif

        extInfo.hasExtendedData = TRUE;
    }

    wxString GetInstName() const { return m_instNameText->GetValue(); }
    int GetProgram() const { return m_instProgramSpin->GetValue(); }
    int GetCurrentLfoIndex() const { return m_currentLfoIndex; }

    void SetOnParameterChanged(std::function<void()> callback) {
        m_onParameterChanged = std::move(callback);
    }

private:
    /* Controls */
    wxTextCtrl *m_instNameText;
    wxSpinCtrl *m_instProgramSpin;
    wxCheckBox *m_chkInterpolate;
    wxCheckBox *m_chkAvoidReverb;
    wxCheckBox *m_chkSampleAndHold;
    wxCheckBox *m_chkAdvancedInterpolation;
    wxCheckBox *m_chkPlayAtSampledFreq;
    wxCheckBox *m_chkNotPolyphonic;
    wxSpinCtrl *m_panPlacementSpin;
    wxSpinCtrl *m_lpfFrequency;
    wxSpinCtrl *m_lpfResonance;
    wxSpinCtrl *m_lpfLowpassAmount;
    ADSRGraphPanel *m_filterGraph;
    wxSpinCtrl *m_adsrStageSpin;
    wxSpinCtrl *m_adsrLevel[BAE_EDITOR_MAX_ADSR_STAGES];
    wxSpinCtrlDouble *m_adsrTime[BAE_EDITOR_MAX_ADSR_STAGES];
    wxChoice *m_adsrFlags[BAE_EDITOR_MAX_ADSR_STAGES];
    wxStaticText *m_adsrRowLabels[BAE_EDITOR_MAX_ADSR_STAGES];
    ADSRGraphPanel *m_adsrGraph;
    wxSpinCtrl *m_lfoCountSpin;
    wxChoice *m_lfoSelector;
    wxChoice *m_lfoDest;
    wxChoice *m_lfoShape;
    wxSpinCtrl *m_lfoPeriod;
    wxSpinCtrl *m_lfoDCFeed;
    wxSpinCtrl *m_lfoLevel;
    LFOWaveformGraphPanel *m_lfoGraph;
    ADSRGraphPanel *m_lfoAdsrGraph;

    /* State */
    BAERmfEditorInstrumentExtInfo m_extInfo;
    int m_currentLfoIndex;
    bool m_loading;
    std::function<void()> m_onParameterChanged;

    void NotifyParameterChanged() {
        if (!m_loading && m_onParameterChanged) m_onParameterChanged();
    }

    void RefreshADSRGraph() {
        int count = m_adsrStageSpin->GetValue();
        int32_t levels[BAE_EDITOR_MAX_ADSR_STAGES];
        int32_t times[BAE_EDITOR_MAX_ADSR_STAGES];
        int32_t flags[BAE_EDITOR_MAX_ADSR_STAGES];
        for (int i = 0; i < count && i < BAE_EDITOR_MAX_ADSR_STAGES; i++) {
            levels[i] = m_adsrLevel[i]->GetValue();
            times[i] = (int32_t)(m_adsrTime[i]->GetValue() * 1000000.0);
            int sel = m_adsrFlags[i]->GetSelection();
            flags[i] = (sel >= 0 && sel < kADSRFlagCount) ? kADSRFlagLabels[sel].value : 0;
        }
        m_adsrGraph->SetEnvelope(count, levels, times, flags);
        RefreshFilterEnvelopeGraph();
    }

    void RefreshLFOGraph() {
        int shapeSel;
        int32_t shape;
        if (!m_lfoGraph) return;
        shapeSel = m_lfoShape ? m_lfoShape->GetSelection() : 0;
        shape = (shapeSel >= 0 && shapeSel < kLFOShapeCount) ? kLFOShapeLabels[shapeSel].value : (int32_t)FOUR_CHAR('S','I','N','E');
        m_lfoGraph->SetLFO(shape,
                           m_lfoPeriod ? m_lfoPeriod->GetValue() : 0,
                           m_lfoDCFeed ? m_lfoDCFeed->GetValue() : 0,
                           m_lfoLevel ? m_lfoLevel->GetValue() : 0);
    }

    void RefreshLFOEnvelopeGraph() {
        int idx;
        int count;
        int32_t levels[BAE_EDITOR_MAX_ADSR_STAGES];
        int32_t times[BAE_EDITOR_MAX_ADSR_STAGES];
        int32_t flags[BAE_EDITOR_MAX_ADSR_STAGES];
        if (!m_lfoAdsrGraph) return;
        idx = m_currentLfoIndex;
        if (idx < 0 || idx >= (int)m_extInfo.lfoCount) {
            m_lfoAdsrGraph->SetEnvelope(0, nullptr, nullptr, nullptr);
            return;
        }
        count = std::clamp((int)m_extInfo.lfos[idx].adsr.stageCount, 0, BAE_EDITOR_MAX_ADSR_STAGES);
        for (int i = 0; i < count; ++i) {
            levels[i] = m_extInfo.lfos[idx].adsr.stages[i].level;
            times[i] = m_extInfo.lfos[idx].adsr.stages[i].time;
            flags[i] = m_extInfo.lfos[idx].adsr.stages[i].flags;
        }
        m_lfoAdsrGraph->SetEnvelope(count, levels, times, flags);
    }

    void RefreshFilterEnvelopeGraph() {
        int count;
        int32_t levels[BAE_EDITOR_MAX_ADSR_STAGES];
        int32_t times[BAE_EDITOR_MAX_ADSR_STAGES];
        int32_t flags[BAE_EDITOR_MAX_ADSR_STAGES];
        int32_t baseFreq;
        int32_t amount;
        int64_t sourceLevels[BAE_EDITOR_MAX_ADSR_STAGES];
        int64_t maxAbsLevel;
        BAERmfEditorADSRInfo const *lpfSourceAdsr;

        if (!m_filterGraph || !m_adsrStageSpin) return;
        baseFreq = m_lpfFrequency ? m_lpfFrequency->GetValue() : 0;
        amount = m_lpfLowpassAmount ? m_lpfLowpassAmount->GetValue() : 0;
        lpfSourceAdsr = nullptr;

        for (uint32_t i = 0; i < m_extInfo.lfoCount; ++i) {
            int32_t dest = m_extInfo.lfos[i].destination;
            if ((dest == (int32_t)FOUR_CHAR('L','P','F','R') ||
                 dest == (int32_t)FOUR_CHAR('L','P','A','M')) &&
                m_extInfo.lfos[i].adsr.stageCount > 0) {
                lpfSourceAdsr = &m_extInfo.lfos[i].adsr;
                break;
            }
        }

        if (lpfSourceAdsr) {
            count = std::clamp((int)lpfSourceAdsr->stageCount, 0, BAE_EDITOR_MAX_ADSR_STAGES);
            for (int i = 0; i < count; ++i) {
                sourceLevels[i] = lpfSourceAdsr->stages[i].level;
                times[i] = lpfSourceAdsr->stages[i].time;
                flags[i] = lpfSourceAdsr->stages[i].flags;
            }
        } else {
            count = std::clamp(m_adsrStageSpin->GetValue(), 0, BAE_EDITOR_MAX_ADSR_STAGES);
            for (int i = 0; i < count; ++i) {
                sourceLevels[i] = m_adsrLevel[i] ? (int64_t)m_adsrLevel[i]->GetValue() : 0;
                times[i] = (int32_t)((m_adsrTime[i] ? m_adsrTime[i]->GetValue() : 0.0) * 1000000.0);
                {
                    int sel = m_adsrFlags[i] ? m_adsrFlags[i]->GetSelection() : 0;
                    flags[i] = (sel >= 0 && sel < kADSRFlagCount) ? kADSRFlagLabels[sel].value : 0;
                }
            }
        }

        maxAbsLevel = 0;
        for (int i = 0; i < count; ++i) {
            maxAbsLevel = std::max<int64_t>(maxAbsLevel, std::llabs(sourceLevels[i]));
        }
        if (maxAbsLevel == 0) maxAbsLevel = 4096;

        for (int i = 0; i < count; ++i) {
            int64_t cutoff = (int64_t)baseFreq + (sourceLevels[i] * (int64_t)amount) / maxAbsLevel;
            cutoff = std::clamp<int64_t>(cutoff, 0, UINT32_MAX);
            levels[i] = (int32_t)cutoff;
        }
        m_filterGraph->SetEnvelope(count, levels, times, flags);
    }

    void LoadLFOIntoUI(int idx) {
        bool wasLoading = m_loading;
        m_loading = true;
        if (idx < 0 || idx >= (int)m_extInfo.lfoCount) {
            m_lfoDest->SetSelection(0);
            m_lfoShape->SetSelection(0);
            m_lfoPeriod->SetValue(0);
            m_lfoDCFeed->SetValue(0);
            m_lfoLevel->SetValue(0);
            m_loading = wasLoading;
            RefreshLFOGraph();
            RefreshLFOEnvelopeGraph();
            return;
        }
        BAERmfEditorLFOInfo const &lfo = m_extInfo.lfos[idx];
        m_lfoDest->SetSelection(FindLabelIndex(kLFODestLabels, kLFODestCount, lfo.destination));
        m_lfoShape->SetSelection(FindLabelIndex(kLFOShapeLabels, kLFOShapeCount, lfo.waveShape));
        m_lfoPeriod->SetValue(lfo.period);
        m_lfoDCFeed->SetValue(lfo.DC_feed);
        m_lfoLevel->SetValue(lfo.level);
        m_loading = wasLoading;
        RefreshLFOGraph();
        RefreshLFOEnvelopeGraph();
    }

    void SaveLFOFromUI(int idx) {
        if (idx < 0 || idx >= BAE_EDITOR_MAX_LFOS) return;
        BAERmfEditorLFOInfo &lfo = m_extInfo.lfos[idx];
        int destSel = m_lfoDest->GetSelection();
        if (destSel >= 0 && destSel < kLFODestCount) lfo.destination = kLFODestLabels[destSel].value;
        int shapeSel = m_lfoShape->GetSelection();
        if (shapeSel >= 0 && shapeSel < kLFOShapeCount) lfo.waveShape = kLFOShapeLabels[shapeSel].value;
        lfo.period = m_lfoPeriod->GetValue();
        if (lfo.period != 0 && lfo.period <= 512)
            lfo.period = 513; // engine requires period == 0 (disabled) or > 512
        lfo.DC_feed = m_lfoDCFeed->GetValue();
        lfo.level = m_lfoLevel->GetValue();
    }

    void BuildUI() {
        wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
        wxStaticBoxSizer *lpfBox = nullptr;
        wxStaticBoxSizer *lfoBox = nullptr;
        wxStaticBoxSizer *adsrBox = nullptr;

        /* Name + Program */
        {
            wxBoxSizer *row = new wxBoxSizer(wxHORIZONTAL);
            row->Add(new wxStaticText(this, wxID_ANY, "Name"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
            m_instNameText = new wxTextCtrl(this, wxID_ANY, "");
            row->Add(m_instNameText, 1, wxRIGHT, 15);
            row->Add(new wxStaticText(this, wxID_ANY, "Program"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
            m_instProgramSpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                               wxSP_ARROW_KEYS, 0, 127, 0);
            row->Add(m_instProgramSpin, 0);
            sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT, 8);
        }

        /* Pan Placement */
        {
            wxBoxSizer *row = new wxBoxSizer(wxHORIZONTAL);
            row->Add(new wxStaticText(this, wxID_ANY, "Pan Placement"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
            m_panPlacementSpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(70, -1),
                                                wxSP_ARROW_KEYS, -128, 127, 0);
            row->Add(m_panPlacementSpin, 0, wxRIGHT, 8);
            row->Add(new wxStaticText(this, wxID_ANY, "(-128=L, 0=center, 127=R)"), 0, wxALIGN_CENTER_VERTICAL);
            sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
            m_panPlacementSpin->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent &) { NotifyParameterChanged(); });
        }

        /* Flags */
        {
            wxStaticBoxSizer *box = new wxStaticBoxSizer(wxVERTICAL, this, "Flags");
            wxBoxSizer *row1 = new wxBoxSizer(wxHORIZONTAL);
            m_chkInterpolate = new wxCheckBox(box->GetStaticBox(), wxID_ANY, "Interpolate");
            m_chkAvoidReverb = new wxCheckBox(box->GetStaticBox(), wxID_ANY, "Avoid Reverb");
            m_chkSampleAndHold = new wxCheckBox(box->GetStaticBox(), wxID_ANY, "Sample && Hold");
            row1->Add(m_chkInterpolate, 0, wxRIGHT, 15);
            row1->Add(m_chkAvoidReverb, 0, wxRIGHT, 15);
            row1->Add(m_chkSampleAndHold, 0);
            box->Add(row1, 0, wxALL, 4);
            wxBoxSizer *row2 = new wxBoxSizer(wxHORIZONTAL);
            m_chkAdvancedInterpolation = new wxCheckBox(box->GetStaticBox(), wxID_ANY, "Advanced Interpolation (ZMF)");
            m_chkPlayAtSampledFreq = new wxCheckBox(box->GetStaticBox(), wxID_ANY, "Play at Sampled Freq");
            m_chkNotPolyphonic = new wxCheckBox(box->GetStaticBox(), wxID_ANY, "Not Polyphonic");
            m_chkAdvancedInterpolation->SetToolTip("Stores the advanced interpolation SND flag. Requires ZMF format.");
            row2->Add(m_chkAdvancedInterpolation, 0, wxRIGHT, 15);
            row2->Add(m_chkPlayAtSampledFreq, 0, wxRIGHT, 15);
            row2->Add(m_chkNotPolyphonic, 0);
            box->Add(row2, 0, wxALL, 4);
            sizer->Add(box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

            auto flagChanged = [this](wxCommandEvent &) { NotifyParameterChanged(); };
            m_chkInterpolate->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &event) {
                if (event.IsChecked()) {
                    m_chkAdvancedInterpolation->SetValue(false);
                }
                NotifyParameterChanged();
            });
            m_chkAvoidReverb->Bind(wxEVT_CHECKBOX, flagChanged);
            m_chkSampleAndHold->Bind(wxEVT_CHECKBOX, flagChanged);
            m_chkAdvancedInterpolation->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &event) {
                if (event.IsChecked()) {
                    m_chkInterpolate->SetValue(false);
                }
                NotifyParameterChanged();
            });
            m_chkPlayAtSampledFreq->Bind(wxEVT_CHECKBOX, flagChanged);
            m_chkNotPolyphonic->Bind(wxEVT_CHECKBOX, flagChanged);
        }

        /* LPF */
        {
            lpfBox = new wxStaticBoxSizer(wxHORIZONTAL, this, "Low-Pass Filter");
            wxBoxSizer *controlsRow = new wxBoxSizer(wxHORIZONTAL);

            controlsRow->Add(new wxStaticText(lpfBox->GetStaticBox(), wxID_ANY, "Frequency"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            m_lpfFrequency = new wxSpinCtrl(lpfBox->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(80, -1),
                                            wxSP_ARROW_KEYS, -100000, 100000, 0);
            controlsRow->Add(m_lpfFrequency, 0, wxRIGHT, 12);
            controlsRow->Add(new wxStaticText(lpfBox->GetStaticBox(), wxID_ANY, "Resonance"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            m_lpfResonance = new wxSpinCtrl(lpfBox->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(80, -1),
                                            wxSP_ARROW_KEYS, -100000, 100000, 0);
            controlsRow->Add(m_lpfResonance, 0, wxRIGHT, 12);
            controlsRow->Add(new wxStaticText(lpfBox->GetStaticBox(), wxID_ANY, "Amount"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            m_lpfLowpassAmount = new wxSpinCtrl(lpfBox->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(80, -1),
                                                wxSP_ARROW_KEYS, -100000, 100000, 0);
            controlsRow->Add(m_lpfLowpassAmount, 0);
            lpfBox->Add(controlsRow, 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);

            wxBoxSizer *previewCol = new wxBoxSizer(wxVERTICAL);
            previewCol->Add(new wxStaticText(lpfBox->GetStaticBox(), wxID_ANY, "Filter Envelope Preview"), 0, wxBOTTOM, 2);
            m_filterGraph = new ADSRGraphPanel(lpfBox->GetStaticBox());
            m_filterGraph->SetMinSize(wxSize(-1, 60));
            previewCol->Add(m_filterGraph, 1, wxEXPAND);
            lpfBox->Add(previewCol, 1, wxEXPAND | wxALL, 4);

            m_lpfFrequency->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent &) { RefreshFilterEnvelopeGraph(); NotifyParameterChanged(); });
            m_lpfResonance->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent &) { RefreshFilterEnvelopeGraph(); NotifyParameterChanged(); });
            m_lpfLowpassAmount->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent &) { RefreshFilterEnvelopeGraph(); NotifyParameterChanged(); });
        }

        /* Volume ADSR */
        {
            wxStaticBoxSizer *box = new wxStaticBoxSizer(wxVERTICAL, this, "Volume ADSR");
            adsrBox = box;
            wxBoxSizer *countRow = new wxBoxSizer(wxHORIZONTAL);
            countRow->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "Stages:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
            m_adsrStageSpin = new wxSpinCtrl(box->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(60, -1),
                                             wxSP_ARROW_KEYS, 0, BAE_EDITOR_MAX_ADSR_STAGES, 0);
            countRow->Add(m_adsrStageSpin, 0);
            box->Add(countRow, 0, wxALL, 4);

            m_adsrGraph = new ADSRGraphPanel(box->GetStaticBox());
            m_adsrGraph->SetMinSize(wxSize(-1, 84));
            box->Add(m_adsrGraph, 0, wxEXPAND | wxLEFT | wxRIGHT, 4);

            wxFlexGridSizer *grid = new wxFlexGridSizer(0, 4, 2, 6);
            grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "#"), 0, wxALIGN_CENTER);
            grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "Level"), 0, wxALIGN_CENTER);
            grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "Time (s)"), 0, wxALIGN_CENTER);
            grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "Type"), 0, wxALIGN_CENTER);

            for (int i = 0; i < BAE_EDITOR_MAX_ADSR_STAGES; i++) {
                m_adsrRowLabels[i] = new wxStaticText(box->GetStaticBox(), wxID_ANY, wxString::Format("%d", i));
                grid->Add(m_adsrRowLabels[i], 0, wxALIGN_CENTER_VERTICAL);
                m_adsrLevel[i] = new wxSpinCtrl(box->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(80, -1),
                                                wxSP_ARROW_KEYS, -1000000, 1000000, 0);
                grid->Add(m_adsrLevel[i], 0);
                m_adsrTime[i] = new wxSpinCtrlDouble(box->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(100, -1),
                                                     wxSP_ARROW_KEYS, -16.0, 16.0, 0.0, 0.001);
                m_adsrTime[i]->SetDigits(3);
                grid->Add(m_adsrTime[i], 0);
                m_adsrFlags[i] = new wxChoice(box->GetStaticBox(), wxID_ANY);
                FillChoice(m_adsrFlags[i], kADSRFlagLabels, kADSRFlagCount);
                m_adsrFlags[i]->SetSelection(0);
                grid->Add(m_adsrFlags[i], 0);

                m_adsrLevel[i]->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent &) { RefreshADSRGraph(); NotifyParameterChanged(); });
                m_adsrTime[i]->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent &) { RefreshADSRGraph(); NotifyParameterChanged(); });
                m_adsrFlags[i]->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) { RefreshADSRGraph(); NotifyParameterChanged(); });

                m_adsrRowLabels[i]->Show(false);
                m_adsrLevel[i]->Show(false);
                m_adsrTime[i]->Show(false);
                m_adsrFlags[i]->Show(false);
            }
            box->Add(grid, 0, wxALL, 4);

            m_adsrStageSpin->Bind(wxEVT_SPINCTRL, [this, box](wxCommandEvent &) {
                int count = m_adsrStageSpin->GetValue();
                for (int i = 0; i < BAE_EDITOR_MAX_ADSR_STAGES; i++) {
                    bool vis = (i < count);
                    m_adsrRowLabels[i]->Show(vis);
                    m_adsrLevel[i]->Show(vis);
                    m_adsrTime[i]->Show(vis);
                    m_adsrFlags[i]->Show(vis);
                }
                box->GetStaticBox()->GetParent()->Layout();
                RefreshADSRGraph();
                NotifyParameterChanged();
            });
        }

        /* LFO section */
        {
            lfoBox = new wxStaticBoxSizer(wxVERTICAL, this, "LFOs");
            wxBoxSizer *row = new wxBoxSizer(wxHORIZONTAL);
            row->Add(new wxStaticText(lfoBox->GetStaticBox(), wxID_ANY, "Count:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
            m_lfoCountSpin = new wxSpinCtrl(lfoBox->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(60, -1),
                                            wxSP_ARROW_KEYS, 0, BAE_EDITOR_MAX_LFOS, 0);
            row->Add(m_lfoCountSpin, 0, wxRIGHT, 15);
            row->Add(new wxStaticText(lfoBox->GetStaticBox(), wxID_ANY, "Edit LFO:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
            m_lfoSelector = new wxChoice(lfoBox->GetStaticBox(), wxID_ANY);
            for (int i = 0; i < BAE_EDITOR_MAX_LFOS; i++) m_lfoSelector->Append(wxString::Format("LFO %d", i));
            row->Add(m_lfoSelector, 0);
            lfoBox->Add(row, 0, wxALL, 4);

            wxFlexGridSizer *grid = new wxFlexGridSizer(2, 4, 6);
            grid->Add(new wxStaticText(lfoBox->GetStaticBox(), wxID_ANY, "Dest"), 0, wxALIGN_CENTER_VERTICAL);
            m_lfoDest = new wxChoice(lfoBox->GetStaticBox(), wxID_ANY);
            FillChoice(m_lfoDest, kLFODestLabels, kLFODestCount);
            grid->Add(m_lfoDest, 0);
            grid->Add(new wxStaticText(lfoBox->GetStaticBox(), wxID_ANY, "Shape"), 0, wxALIGN_CENTER_VERTICAL);
            m_lfoShape = new wxChoice(lfoBox->GetStaticBox(), wxID_ANY);
            FillChoice(m_lfoShape, kLFOShapeLabels, kLFOShapeCount);
            grid->Add(m_lfoShape, 0);
            grid->Add(new wxStaticText(lfoBox->GetStaticBox(), wxID_ANY, "Period"), 0, wxALIGN_CENTER_VERTICAL);
            m_lfoPeriod = new wxSpinCtrl(lfoBox->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(100, -1),
                                         wxSP_ARROW_KEYS, -1000000, 10000000, 0);
            grid->Add(m_lfoPeriod, 0);
            grid->Add(new wxStaticText(lfoBox->GetStaticBox(), wxID_ANY, "DC Feed"), 0, wxALIGN_CENTER_VERTICAL);
            m_lfoDCFeed = new wxSpinCtrl(lfoBox->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(100, -1),
                                         wxSP_ARROW_KEYS, -1000000, 1000000, 0);
            grid->Add(m_lfoDCFeed, 0);
            grid->Add(new wxStaticText(lfoBox->GetStaticBox(), wxID_ANY, "Level"), 0, wxALIGN_CENTER_VERTICAL);
            m_lfoLevel = new wxSpinCtrl(lfoBox->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(100, -1),
                                        wxSP_ARROW_KEYS, -1000000, 1000000, 0);
            grid->Add(m_lfoLevel, 0);
            lfoBox->Add(grid, 0, wxALL, 4);

            m_lfoGraph = new LFOWaveformGraphPanel(lfoBox->GetStaticBox());
            lfoBox->Add(m_lfoGraph, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

            lfoBox->Add(new wxStaticText(lfoBox->GetStaticBox(), wxID_ANY, "LFO Envelope (selected LFO)"), 0, wxLEFT | wxRIGHT | wxBOTTOM, 4);
            m_lfoAdsrGraph = new ADSRGraphPanel(lfoBox->GetStaticBox());
            m_lfoAdsrGraph->SetMinSize(wxSize(-1, 72));
            lfoBox->Add(m_lfoAdsrGraph, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

            m_lfoSelector->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
                if (m_loading) return;
                int sel = m_lfoSelector->GetSelection();
                if (m_currentLfoIndex >= 0) SaveLFOFromUI(m_currentLfoIndex);
                m_currentLfoIndex = sel;
                if (sel >= 0) LoadLFOIntoUI(sel);
                RefreshLFOGraph();
                RefreshLFOEnvelopeGraph();
                RefreshFilterEnvelopeGraph();
                NotifyParameterChanged();
            });

            m_lfoCountSpin->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent &) {
                if (m_loading) return;
                int count = m_lfoCountSpin->GetValue();
                if (count <= 0) {
                    m_currentLfoIndex = -1;
                    m_lfoSelector->SetSelection(wxNOT_FOUND);
                } else {
                    int sel = m_lfoSelector->GetSelection();
                    if (sel < 0 || sel >= count) {
                        sel = 0;
                        m_lfoSelector->SetSelection(sel);
                    }
                    m_currentLfoIndex = sel;
                    LoadLFOIntoUI(sel);
                }
                RefreshLFOGraph();
                RefreshLFOEnvelopeGraph();
                RefreshFilterEnvelopeGraph();
                NotifyParameterChanged();
            });

            auto lfoChanged = [this](wxCommandEvent &) {
                if (m_loading) return;
                int sel = (m_currentLfoIndex >= 0) ? m_currentLfoIndex : m_lfoSelector->GetSelection();
                if (sel >= 0) SaveLFOFromUI(sel);
                RefreshLFOGraph();
                NotifyParameterChanged();
            };
            m_lfoDest->Bind(wxEVT_CHOICE, lfoChanged);
            m_lfoShape->Bind(wxEVT_CHOICE, lfoChanged);
            m_lfoPeriod->Bind(wxEVT_SPINCTRL, lfoChanged);
            m_lfoDCFeed->Bind(wxEVT_SPINCTRL, lfoChanged);
            m_lfoLevel->Bind(wxEVT_SPINCTRL, lfoChanged);
        }

        /* Layout: LPF, then LFO+ADSR side-by-side */
        if (lpfBox) {
            sizer->Add(lpfBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
        }
        if (lfoBox && adsrBox) {
            wxBoxSizer *modulationRow = new wxBoxSizer(wxHORIZONTAL);
            modulationRow->Add(lfoBox, 1, wxEXPAND | wxRIGHT, 6);
            modulationRow->Add(adsrBox, 1, wxEXPAND | wxLEFT, 6);
            sizer->Add(modulationRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
        } else if (lfoBox) {
            sizer->Add(lfoBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
        } else if (adsrBox) {
            sizer->Add(adsrBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
        }

        SetSizer(sizer);

        /* Name and program change notifications */
        m_instNameText->Bind(wxEVT_TEXT, [this](wxCommandEvent &) { NotifyParameterChanged(); });
        m_instProgramSpin->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent &) { NotifyParameterChanged(); });
        m_instProgramSpin->Bind(wxEVT_TEXT, [this](wxCommandEvent &) { NotifyParameterChanged(); });
    }
};

/* ====================================================================== */
/* SampleParamsPanel                                                      */
/* ====================================================================== */

/* Data struct that both bank editor and instrument dialog can populate. */
struct SampleParamsPanelData {
    wxString displayName;
    unsigned char rootKey;
    unsigned char lowKey;
    unsigned char highKey;
    int16_t splitVolume;
    uint32_t sampleRate;      /* Hz */
    uint32_t waveFrames;
    uint32_t loopStart;
    uint32_t loopEnd;
    BAERmfEditorCompressionType compressionType;
    bool hasOriginalData;
    BAERmfEditorSndStorageType sndStorageType;
    BAERmfEditorOpusMode opusMode;
    bool opusRoundTripResample;
    wxString codecDescription;   /* e.g. "PCM 16-bit" from source query */
};

class SampleParamsPanel : public wxPanel {
public:
    explicit SampleParamsPanel(wxWindow *parent)
        : wxPanel(parent, wxID_ANY),
          m_loading(false)
    {
        BuildUI();
    }

    /* ------- Public data interface ------- */

    /* Load a sample's properties into the controls. */
    void LoadSample(SampleParamsPanelData const &data) {
        m_loading = true;
        m_nameText->SetValue(data.displayName);
        m_rootSpin->SetValue(data.rootKey);
        m_lowSpin->SetValue(data.lowKey);
        m_highSpin->SetValue(data.highKey);
        m_sampleRateSpin->SetValue(std::clamp<int>((int)data.sampleRate, 1, 65535));
        m_splitVolumeSpin->SetValue(std::clamp<int>(data.splitVolume, 0, 32767));

        /* Codec */
        {
            BAERmfEditorCompressionType effectiveComp = data.compressionType;
            auto [codecIdx, bitrateIdx] = CompressionTypeToCodecBitrate(effectiveComp);
            if (codecIdx == 6 && data.opusRoundTripResample) codecIdx = 7;
            m_codecChoice->SetSelection(codecIdx);
            m_codecChoice->SetString(0, data.hasOriginalData ? "Original" : "Don't Change");
            UpdateBitrateChoice(codecIdx, bitrateIdx);
            if (m_opusModeChoice) {
                m_opusModeChoice->SetSelection(data.opusMode == BAE_EDITOR_OPUS_MODE_VOICE ? 1 : 0);
                m_opusModeChoice->Enable(codecIdx == 6 || codecIdx == 7);
            }
        }

        /* Storage */
        {
            int sel = 0;
            if (data.sndStorageType == BAE_EDITOR_SND_STORAGE_CSND)     sel = 1;
            else if (data.sndStorageType == BAE_EDITOR_SND_STORAGE_SND) sel = 2;
            m_sndStorageChoice->SetSelection(sel);
        }

        /* Codec label */
        if (!data.codecDescription.empty())
            m_codecLabel->SetLabel(wxString::Format("Source: %s", data.codecDescription));
        else
            m_codecLabel->SetLabel(wxString());

        /* Loop */
        {
            bool loopEnabled = (data.loopStart < data.loopEnd);
            int maxFrame = (data.waveFrames > 0) ? (int)data.waveFrames : INT_MAX;
            m_loopEnableCheck->SetValue(loopEnabled);
            m_loopStartSpin->SetRange(0, maxFrame);
            m_loopEndSpin->SetRange(0, maxFrame);
            m_loopStartSpin->SetValue((int)data.loopStart);
            m_loopEndSpin->SetValue((int)data.loopEnd);
            m_loopStartSpin->Enable(loopEnabled);
            m_loopEndSpin->Enable(loopEnabled);
            m_savedLoopStart = data.loopStart;
            m_savedLoopEnd = data.loopEnd;
            m_currentWaveFrames = data.waveFrames;
            UpdateLoopInfoLabel();
        }
        m_loading = false;
    }

    /* Read current control state back into a data struct. */
    void SaveToData(SampleParamsPanelData &data) const {
        data.displayName = m_nameText->GetValue();
        data.rootKey = (unsigned char)m_rootSpin->GetValue();
        data.lowKey = (unsigned char)m_lowSpin->GetValue();
        data.highKey = (unsigned char)m_highSpin->GetValue();
        data.sampleRate = (uint32_t)std::max(1, m_sampleRateSpin->GetValue());
        data.splitVolume = (int16_t)m_splitVolumeSpin->GetValue();

        int codecIdx = m_codecChoice->GetSelection();
        int bitrateIdx = m_bitrateChoice->IsEnabled() ? m_bitrateChoice->GetSelection() : 0;
        data.compressionType = CodecBitrateToCompressionType(codecIdx, bitrateIdx);
        data.opusRoundTripResample = (codecIdx == 7);
        data.opusMode = (m_opusModeChoice->GetSelection() == 1) ?
            BAE_EDITOR_OPUS_MODE_VOICE : BAE_EDITOR_OPUS_MODE_AUDIO;

        {
            int sel = m_sndStorageChoice->GetSelection();
            if (sel == 1)      data.sndStorageType = BAE_EDITOR_SND_STORAGE_CSND;
            else if (sel == 2) data.sndStorageType = BAE_EDITOR_SND_STORAGE_SND;
            else               data.sndStorageType = BAE_EDITOR_SND_STORAGE_ESND;
        }

        if (m_loopEnableCheck->GetValue()) {
            data.loopStart = (uint32_t)std::max(0, m_loopStartSpin->GetValue());
            data.loopEnd = (uint32_t)std::max(0, m_loopEndSpin->GetValue());
        } else {
            data.loopStart = 0;
            data.loopEnd = 0;
        }
    }

    /* Set waveform independently (host fetches PCM from its data source). */
    void SetWaveform(void const *data, uint32_t frames, uint16_t bitSize, uint16_t channels) {
        if (m_waveformPanel) m_waveformPanel->SetWaveform(data, frames, bitSize, channels);
    }

    void SetLoopPoints(uint32_t start, uint32_t end) {
        if (m_waveformPanel) m_waveformPanel->SetLoopPoints(start, end);
    }

    void ClearUI() {
        m_loading = true;
        m_nameText->SetValue("");
        m_rootSpin->SetValue(60);
        m_lowSpin->SetValue(0);
        m_highSpin->SetValue(127);
        m_sampleRateSpin->SetValue(22050);
        m_splitVolumeSpin->SetValue(0);
        m_loopEnableCheck->SetValue(false);
        m_loopStartSpin->SetValue(0);
        m_loopEndSpin->SetValue(0);
        m_loopStartSpin->Enable(false);
        m_loopEndSpin->Enable(false);
        m_loopInfoLabel->SetLabel("");
        m_codecChoice->SetSelection(1);
        UpdateBitrateChoice(1);
        m_opusModeChoice->SetSelection(0);
        m_opusModeChoice->Enable(false);
        m_sndStorageChoice->SetSelection(0);
        m_codecLabel->SetLabel("");
        if (m_waveformPanel) m_waveformPanel->SetWaveform(nullptr, 0, 16, 1);
        m_currentWaveFrames = 0;
        m_loading = false;
    }

    wxString GetDisplayName() const { return m_nameText->GetValue(); }
    int GetRootKey() const { return m_rootSpin->GetValue(); }
    int GetLowKey() const { return m_lowSpin->GetValue(); }
    int GetHighKey() const { return m_highSpin->GetValue(); }
    int GetCodecSelection() const { return m_codecChoice ? m_codecChoice->GetSelection() : 0; }
    int GetBitrateSelection() const { return (m_bitrateChoice && m_bitrateChoice->IsEnabled()) ? m_bitrateChoice->GetSelection() : -1; }

    /* Enable or disable the New/Delete/Replace/Export buttons.
     * The bank editor sets write=false since banks are read-only;
     * the instrument dialog sets write=true.  */
    void SetWriteMode(bool canNew, bool canDelete, bool canReplace, bool canExport) {
        if (m_newSampleBtn) m_newSampleBtn->Show(canNew);
        if (m_deleteSampleBtn) m_deleteSampleBtn->Show(canDelete);
        if (m_replaceBtn) m_replaceBtn->Show(canReplace);
        if (m_exportBtn) m_exportBtn->Show(canExport);
        /* Codec/storage/action rows — hide entirely for read-only view */
        wxSizer *parentSizer = GetSizer();
        if (parentSizer) {
            if (m_codecRow) parentSizer->Show(m_codecRow, canNew || canReplace);
            if (m_storageRow) parentSizer->Show(m_storageRow, canNew || canReplace);
            if (m_actionRow) parentSizer->Show(m_actionRow, canNew || canDelete || canReplace || canExport);
        }
        Layout();
    }

    void SetCodecControlsVisible(bool visible) {
        wxSizer *parentSizer = GetSizer();

        if (parentSizer) {
            if (m_codecRow) parentSizer->Show(m_codecRow, visible);
            if (m_storageRow) parentSizer->Show(m_storageRow, visible);
        }
        Layout();
    }

    void SetDeleteEnabled(bool enabled) {
        if (m_deleteSampleBtn) m_deleteSampleBtn->Enable(enabled);
    }

    /* ------- Callbacks ------- */
    void SetOnParameterChanged(std::function<void()> cb) { m_onParameterChanged = std::move(cb); }
    void SetOnLoopChanged(std::function<void()> cb) { m_onLoopChanged = std::move(cb); }
    void SetOnNewSample(std::function<void()> cb) { m_onNewSample = std::move(cb); }
    void SetOnDeleteSample(std::function<void()> cb) { m_onDeleteSample = std::move(cb); }
    void SetOnReplaceSample(std::function<void()> cb) { m_onReplaceSample = std::move(cb); }
    void SetOnExportSample(std::function<void()> cb) { m_onExportSample = std::move(cb); }

    /* ------- Codec helpers (public for host use) ------- */
    static std::pair<int,int> CompressionTypeToCodecBitrate(BAERmfEditorCompressionType t) {
        switch (t) {
            case BAE_EDITOR_COMPRESSION_DONT_CHANGE: return {0, 0};
            case BAE_EDITOR_COMPRESSION_PCM:         return {1, 0};
            case BAE_EDITOR_COMPRESSION_ADPCM:       return {2, 0};
            case BAE_EDITOR_COMPRESSION_MP3_32K:     return {3, 0};
            case BAE_EDITOR_COMPRESSION_MP3_48K:     return {3, 1};
            case BAE_EDITOR_COMPRESSION_MP3_64K:     return {3, 2};
            case BAE_EDITOR_COMPRESSION_MP3_96K:     return {3, 3};
            case BAE_EDITOR_COMPRESSION_MP3_128K:    return {3, 4};
            case BAE_EDITOR_COMPRESSION_MP3_192K:    return {3, 5};
            case BAE_EDITOR_COMPRESSION_MP3_256K:    return {3, 6};
            case BAE_EDITOR_COMPRESSION_MP3_320K:    return {3, 7};
            case BAE_EDITOR_COMPRESSION_VORBIS_32K:  return {4, 0};
            case BAE_EDITOR_COMPRESSION_VORBIS_48K:  return {4, 1};
            case BAE_EDITOR_COMPRESSION_VORBIS_64K:  return {4, 2};
            case BAE_EDITOR_COMPRESSION_VORBIS_80K:  return {4, 3};
            case BAE_EDITOR_COMPRESSION_VORBIS_96K:  return {4, 4};
            case BAE_EDITOR_COMPRESSION_VORBIS_128K: return {4, 5};
            case BAE_EDITOR_COMPRESSION_VORBIS_160K: return {4, 6};
            case BAE_EDITOR_COMPRESSION_VORBIS_192K: return {4, 7};
            case BAE_EDITOR_COMPRESSION_VORBIS_256K: return {4, 8};
            case BAE_EDITOR_COMPRESSION_FLAC:        return {5, 0};
            case BAE_EDITOR_COMPRESSION_OPUS_12K:    return {6, 0};
            case BAE_EDITOR_COMPRESSION_OPUS_16K:    return {6, 1};
            case BAE_EDITOR_COMPRESSION_OPUS_24K:    return {6, 2};
            case BAE_EDITOR_COMPRESSION_OPUS_32K:    return {6, 3};
            case BAE_EDITOR_COMPRESSION_OPUS_48K:    return {6, 4};
            case BAE_EDITOR_COMPRESSION_OPUS_64K:    return {6, 5};
            case BAE_EDITOR_COMPRESSION_OPUS_80K:    return {6, 6};
            case BAE_EDITOR_COMPRESSION_OPUS_96K:    return {6, 7};
            case BAE_EDITOR_COMPRESSION_OPUS_128K:   return {6, 8};
            case BAE_EDITOR_COMPRESSION_OPUS_160K:   return {6, 9};
            case BAE_EDITOR_COMPRESSION_OPUS_192K:   return {6, 10};
            case BAE_EDITOR_COMPRESSION_OPUS_256K:   return {6, 11};
            default:                                  return {1, 0};
        }
    }

    static BAERmfEditorCompressionType CodecBitrateToCompressionType(int codec, int bitrate) {
        switch (codec) {
            case 0: return BAE_EDITOR_COMPRESSION_DONT_CHANGE;
            case 1: return BAE_EDITOR_COMPRESSION_PCM;
            case 2: return BAE_EDITOR_COMPRESSION_ADPCM;
            case 3:
                switch (bitrate) {
                    case 0: return BAE_EDITOR_COMPRESSION_MP3_32K;
                    case 1: return BAE_EDITOR_COMPRESSION_MP3_48K;
                    case 2: return BAE_EDITOR_COMPRESSION_MP3_64K;
                    case 3: return BAE_EDITOR_COMPRESSION_MP3_96K;
                    case 4: return BAE_EDITOR_COMPRESSION_MP3_128K;
                    case 5: return BAE_EDITOR_COMPRESSION_MP3_192K;
                    case 6: return BAE_EDITOR_COMPRESSION_MP3_256K;
                    case 7: return BAE_EDITOR_COMPRESSION_MP3_320K;
                    default: return BAE_EDITOR_COMPRESSION_MP3_128K;
                }
            case 4:
                switch (bitrate) {
                    case 0: return BAE_EDITOR_COMPRESSION_VORBIS_32K;
                    case 1: return BAE_EDITOR_COMPRESSION_VORBIS_48K;
                    case 2: return BAE_EDITOR_COMPRESSION_VORBIS_64K;
                    case 3: return BAE_EDITOR_COMPRESSION_VORBIS_80K;
                    case 4: return BAE_EDITOR_COMPRESSION_VORBIS_96K;
                    case 5: return BAE_EDITOR_COMPRESSION_VORBIS_128K;
                    case 6: return BAE_EDITOR_COMPRESSION_VORBIS_160K;
                    case 7: return BAE_EDITOR_COMPRESSION_VORBIS_192K;
                    case 8: return BAE_EDITOR_COMPRESSION_VORBIS_256K;
                    default: return BAE_EDITOR_COMPRESSION_VORBIS_128K;
                }
            case 5: return BAE_EDITOR_COMPRESSION_FLAC;
            case 7:
            case 6:
                switch (bitrate) {
                    case 0: return BAE_EDITOR_COMPRESSION_OPUS_12K;
                    case 1: return BAE_EDITOR_COMPRESSION_OPUS_16K;
                    case 2: return BAE_EDITOR_COMPRESSION_OPUS_24K;
                    case 3: return BAE_EDITOR_COMPRESSION_OPUS_32K;
                    case 4: return BAE_EDITOR_COMPRESSION_OPUS_48K;
                    case 5: return BAE_EDITOR_COMPRESSION_OPUS_64K;
                    case 6: return BAE_EDITOR_COMPRESSION_OPUS_80K;
                    case 7: return BAE_EDITOR_COMPRESSION_OPUS_96K;
                    case 8: return BAE_EDITOR_COMPRESSION_OPUS_128K;
                    case 9: return BAE_EDITOR_COMPRESSION_OPUS_160K;
                    case 10: return BAE_EDITOR_COMPRESSION_OPUS_192K;
                    case 11: return BAE_EDITOR_COMPRESSION_OPUS_256K;
                    default: return BAE_EDITOR_COMPRESSION_OPUS_48K;
                }
            default: return BAE_EDITOR_COMPRESSION_PCM;
        }
    }

private:
    bool m_loading;
    uint32_t m_savedLoopStart = 0, m_savedLoopEnd = 0;
    uint32_t m_currentWaveFrames = 0;

    /* Callbacks */
    std::function<void()> m_onParameterChanged;
    std::function<void()> m_onLoopChanged;
    std::function<void()> m_onNewSample;
    std::function<void()> m_onDeleteSample;
    std::function<void()> m_onReplaceSample;
    std::function<void()> m_onExportSample;

    /* Controls */
    wxTextCtrl *m_nameText;
    wxSpinCtrl *m_rootSpin;
    wxSpinCtrl *m_lowSpin;
    wxSpinCtrl *m_highSpin;
    wxSpinCtrl *m_sampleRateSpin;
    wxSpinCtrl *m_splitVolumeSpin;
    wxCheckBox *m_loopEnableCheck;
    wxSpinCtrl *m_loopStartSpin;
    wxSpinCtrl *m_loopEndSpin;
    wxStaticText *m_loopInfoLabel;
    wxChoice *m_codecChoice;
    wxChoice *m_bitrateChoice;
    wxChoice *m_opusModeChoice;
    wxStaticText *m_codecLabel;
    wxChoice *m_sndStorageChoice;
    WaveformPanelExt *m_waveformPanel;
    wxButton *m_newSampleBtn;
    wxButton *m_deleteSampleBtn;
    wxButton *m_replaceBtn;
    wxButton *m_exportBtn;
    wxSizer *m_codecRow;
    wxSizer *m_storageRow;
    wxSizer *m_actionRow;

    void NotifyParameterChanged() {
        if (!m_loading && m_onParameterChanged) m_onParameterChanged();
    }

    void NotifyLoopChanged() {
        if (!m_loading && m_onLoopChanged) m_onLoopChanged();
    }

    void UpdateLoopInfoLabel() {
        if (m_loopEnableCheck->GetValue()) {
            uint32_t start = (uint32_t)std::max(0, m_loopStartSpin->GetValue());
            uint32_t end = (uint32_t)std::max(0, m_loopEndSpin->GetValue());
            if (start < end) {
                uint32_t frames = end - start;
                uint32_t rateHz = (uint32_t)std::max(1, m_sampleRateSpin->GetValue());
                double ms = frames * 1000.0 / rateHz;
                m_loopInfoLabel->SetLabel(wxString::Format("%u frames (%.0f ms)", frames, ms));
                return;
            }
        }
        m_loopInfoLabel->SetLabel("");
    }

    void ApplyLoopFromUI() {
        uint32_t start = (uint32_t)std::max(0, m_loopStartSpin->GetValue());
        uint32_t end = (uint32_t)std::max(0, m_loopEndSpin->GetValue());
        if (m_currentWaveFrames > 0 && end > m_currentWaveFrames) end = m_currentWaveFrames;
        if (start >= end && end > 0) start = end - 1;
        if (m_waveformPanel) m_waveformPanel->SetLoopPoints(start, end);
        UpdateLoopInfoLabel();
    }

    void OnLoopEnableChanged() {
        bool en = m_loopEnableCheck->GetValue();
        m_loopStartSpin->Enable(en);
        m_loopEndSpin->Enable(en);
        if (!en) {
            m_savedLoopStart = (uint32_t)m_loopStartSpin->GetValue();
            m_savedLoopEnd = (uint32_t)m_loopEndSpin->GetValue();
            if (m_waveformPanel) m_waveformPanel->SetLoopPoints(0, 0);
            m_loopInfoLabel->SetLabel("");
        } else {
            uint32_t rs = m_savedLoopStart, re = m_savedLoopEnd;
            if (rs == 0 && re == 0) re = m_currentWaveFrames;
            m_loopStartSpin->SetValue((int)rs);
            m_loopEndSpin->SetValue((int)re);
            ApplyLoopFromUI();
        }
        NotifyLoopChanged();
        NotifyParameterChanged();
    }

    void OnLoopPointChanged() {
        if (m_loopEnableCheck->GetValue()) {
            ApplyLoopFromUI();
            NotifyLoopChanged();
            NotifyParameterChanged();
        }
    }

    void UpdateBitrateChoice(int codecIdx, int preferredSelection = -1) {
        auto chooseIndex = [&](int count, int defaultSelection) {
            int index = defaultSelection;
            if (preferredSelection >= 0 && preferredSelection < count) {
                index = preferredSelection;
            }
            if (index < 0 || index >= count) {
                index = 0;
            }
            m_bitrateChoice->SetSelection(index);
        };

        m_bitrateChoice->Clear();
        switch (codecIdx) {
            case 3:
                for (auto s : {"32k","48k","64k","96k","128k","192k","256k","320k"})
                    m_bitrateChoice->Append(s);
                chooseIndex((int)m_bitrateChoice->GetCount(), 0);
                m_bitrateChoice->Enable(true);
                break;
            case 4:
                for (auto s : {"32k","48k","64k","80k","96k","128k","160k","192k","256k"})
                    m_bitrateChoice->Append(s);
                chooseIndex((int)m_bitrateChoice->GetCount(), 0);
                m_bitrateChoice->Enable(true);
                break;
            case 7:
            case 6:
                for (auto s : {"12k","16k","24k","32k","48k","64k","80k","96k","128k","160k","192k","256k"})
                    m_bitrateChoice->Append(s);
                chooseIndex((int)m_bitrateChoice->GetCount(), 0);
                m_bitrateChoice->Enable(true);
                break;
            default:
                m_bitrateChoice->Append("---");
                m_bitrateChoice->SetSelection(0);
                m_bitrateChoice->Enable(false);
                break;
        }
    }

    void BuildUI() {
        wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);

        /* Sample selector row (host adds split choice above us if needed) */

        /* Sample properties grid */
        wxFlexGridSizer *grid = new wxFlexGridSizer(2, 4, 6);
        grid->Add(new wxStaticText(this, wxID_ANY, "Title"), 0, wxALIGN_CENTER_VERTICAL);
        m_nameText = new wxTextCtrl(this, wxID_ANY, "");
        grid->Add(m_nameText, 1, wxEXPAND);
        grid->Add(new wxStaticText(this, wxID_ANY, "Root Key"), 0, wxALIGN_CENTER_VERTICAL);
        m_rootSpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 127, 60);
        grid->Add(m_rootSpin, 0);
        grid->Add(new wxStaticText(this, wxID_ANY, "Low Key"), 0, wxALIGN_CENTER_VERTICAL);
        m_lowSpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 127, 0);
        grid->Add(m_lowSpin, 0);
        grid->Add(new wxStaticText(this, wxID_ANY, "High Key"), 0, wxALIGN_CENTER_VERTICAL);
        m_highSpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 127, 127);
        grid->Add(m_highSpin, 0);
        grid->Add(new wxStaticText(this, wxID_ANY, "Sample Rate (Hz)"), 0, wxALIGN_CENTER_VERTICAL);
        m_sampleRateSpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 65535, 22050);
        grid->Add(m_sampleRateSpin, 0);
        grid->Add(new wxStaticText(this, wxID_ANY, "Split Vol (0=default)"), 0, wxALIGN_CENTER_VERTICAL);
        m_splitVolumeSpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 32767, 100);
        grid->Add(m_splitVolumeSpin, 0);
        grid->AddGrowableCol(1, 1);
        sizer->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT, 8);

        /* Loop controls */
        {
            wxBoxSizer *row = new wxBoxSizer(wxHORIZONTAL);
            m_loopEnableCheck = new wxCheckBox(this, wxID_ANY, "Loop");
            row->Add(m_loopEnableCheck, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
            row->Add(new wxStaticText(this, wxID_ANY, "Start"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            m_loopStartSpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(100, -1), wxSP_ARROW_KEYS, 0, INT_MAX, 0);
            row->Add(m_loopStartSpin, 0, wxRIGHT, 10);
            row->Add(new wxStaticText(this, wxID_ANY, "End"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            m_loopEndSpin = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(100, -1), wxSP_ARROW_KEYS, 0, INT_MAX, 0);
            row->Add(m_loopEndSpin, 0, wxRIGHT, 10);
            m_loopInfoLabel = new wxStaticText(this, wxID_ANY, "");
            row->Add(m_loopInfoLabel, 0, wxALIGN_CENTER_VERTICAL);
            sizer->Add(row, 0, wxALL, 8);
        }

        /* Compression */
        {
            wxBoxSizer *row = new wxBoxSizer(wxHORIZONTAL);
            m_codecRow = row;
            row->Add(new wxStaticText(this, wxID_ANY, "Compression"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
            m_codecChoice = new wxChoice(this, wxID_ANY);
            m_codecChoice->Append("Don't Change");
            m_codecChoice->Append("RAW PCM");
            m_codecChoice->Append("ADPCM");
            m_codecChoice->Append("MP3");
            m_codecChoice->Append("VORBIS");
            m_codecChoice->Append("FLAC");
            m_codecChoice->Append("OPUS");
            m_codecChoice->Append("OPUS (Round-Trip)");
            row->Add(m_codecChoice, 0);
            row->Add(new wxStaticText(this, wxID_ANY, "Bitrate"), 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 8);
            m_bitrateChoice = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxSize(130, -1));
            m_bitrateChoice->Append("---");
            m_bitrateChoice->SetSelection(0);
            m_bitrateChoice->Enable(false);
            row->Add(m_bitrateChoice, 0);
            m_bitrateChoice->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) { NotifyParameterChanged(); });
            row->Add(new wxStaticText(this, wxID_ANY, "Opus Mode"), 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 8);
            m_opusModeChoice = new wxChoice(this, wxID_ANY);
            m_opusModeChoice->Append("Audio");
            m_opusModeChoice->Append("Voice");
            m_opusModeChoice->SetSelection(0);
            m_opusModeChoice->Enable(false);
            row->Add(m_opusModeChoice, 0);
            m_opusModeChoice->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) { NotifyParameterChanged(); });
            row->AddStretchSpacer();
            m_codecLabel = new wxStaticText(this, wxID_ANY, "");
            row->Add(m_codecLabel, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 12);
            sizer->Add(row, 0, wxLEFT | wxRIGHT | wxBOTTOM, 6);

            m_codecChoice->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
                int selectedCodec = m_codecChoice->GetSelection();
                int previousBitrate = m_bitrateChoice->IsEnabled() ? m_bitrateChoice->GetSelection() : -1;

                UpdateBitrateChoice(selectedCodec, previousBitrate);
                if (m_opusModeChoice) {
                    m_opusModeChoice->Enable(selectedCodec == 6 || selectedCodec == 7);
                }
                NotifyParameterChanged();
            });
        }

        /* Storage */
        {
            wxBoxSizer *row = new wxBoxSizer(wxHORIZONTAL);
            m_storageRow = row;
            row->Add(new wxStaticText(this, wxID_ANY, "Storage"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
            m_sndStorageChoice = new wxChoice(this, wxID_ANY);
            m_sndStorageChoice->Append("ESND (encrypted)");
            m_sndStorageChoice->Append("CSND (compressed)");
            m_sndStorageChoice->Append("SND (plain)");
            m_sndStorageChoice->SetSelection(0);
            row->Add(m_sndStorageChoice, 0);
            sizer->Add(row, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);

            m_sndStorageChoice->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) { NotifyParameterChanged(); });
        }

        /* Action buttons */
        {
            wxBoxSizer *row = new wxBoxSizer(wxHORIZONTAL);
            m_actionRow = row;
            m_replaceBtn = new wxButton(this, wxID_ANY, "Replace");
            m_exportBtn = new wxButton(this, wxID_ANY, "Save As...");
            m_newSampleBtn = new wxButton(this, wxID_ANY, "New Sample");
            m_deleteSampleBtn = new wxButton(this, wxID_ANY, "Delete Sample");
            row->Add(m_replaceBtn, 0, wxRIGHT, 8);
            row->Add(m_exportBtn, 0, wxRIGHT, 8);
            row->Add(m_newSampleBtn, 0);
            row->AddStretchSpacer();
            row->Add(m_deleteSampleBtn, 0, wxLEFT, 8);
            sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

            m_replaceBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { if (m_onReplaceSample) m_onReplaceSample(); });
            m_exportBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { if (m_onExportSample) m_onExportSample(); });
            m_newSampleBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { if (m_onNewSample) m_onNewSample(); });
            m_deleteSampleBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { if (m_onDeleteSample) m_onDeleteSample(); });
        }

        /* Waveform */
        sizer->Add(new wxStaticText(this, wxID_ANY, "Waveform"), 0, wxLEFT | wxRIGHT, 8);
        m_waveformPanel = new WaveformPanelExt(this);
        m_waveformPanel->SetLoopChangedCallback([this](uint32_t start, uint32_t end) {
            if (m_loading) return;
            m_loading = true;
            m_loopEnableCheck->SetValue(end > start);
            m_loopStartSpin->Enable(end > start);
            m_loopEndSpin->Enable(end > start);
            m_loopStartSpin->SetValue((int)start);
            m_loopEndSpin->SetValue((int)end);
            m_loading = false;
            if (end > start) {
                ApplyLoopFromUI();
            } else {
                if (m_waveformPanel) m_waveformPanel->SetLoopPoints(0, 0);
                m_loopInfoLabel->SetLabel("");
            }
            NotifyLoopChanged();
            NotifyParameterChanged();
        });
        sizer->Add(m_waveformPanel, 1, wxEXPAND | wxALL, 8);

        SetSizer(sizer);

        /* Bind change events */
        m_nameText->Bind(wxEVT_TEXT, [this](wxCommandEvent &) { NotifyParameterChanged(); });
        m_rootSpin->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent &) { NotifyParameterChanged(); });
        m_rootSpin->Bind(wxEVT_TEXT, [this](wxCommandEvent &) { NotifyParameterChanged(); });
        m_lowSpin->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent &) { NotifyParameterChanged(); });
        m_lowSpin->Bind(wxEVT_TEXT, [this](wxCommandEvent &) { NotifyParameterChanged(); });
        m_highSpin->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent &) { NotifyParameterChanged(); });
        m_highSpin->Bind(wxEVT_TEXT, [this](wxCommandEvent &) { NotifyParameterChanged(); });
        m_sampleRateSpin->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent &) { NotifyParameterChanged(); });
        m_sampleRateSpin->Bind(wxEVT_TEXT, [this](wxCommandEvent &) { NotifyParameterChanged(); });
        m_splitVolumeSpin->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent &) { NotifyParameterChanged(); });
        m_splitVolumeSpin->Bind(wxEVT_TEXT, [this](wxCommandEvent &) { NotifyParameterChanged(); });
        m_loopEnableCheck->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { OnLoopEnableChanged(); });
        m_loopStartSpin->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent &) { OnLoopPointChanged(); });
        m_loopEndSpin->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent &) { OnLoopPointChanged(); });
        m_loopStartSpin->Bind(wxEVT_TEXT, [this](wxCommandEvent &) { OnLoopPointChanged(); });
        m_loopEndSpin->Bind(wxEVT_TEXT, [this](wxCommandEvent &) { OnLoopPointChanged(); });
        m_lowSpin->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent &) {
            if (m_lowSpin->GetValue() > m_highSpin->GetValue()) m_highSpin->SetValue(m_lowSpin->GetValue());
        });
        m_highSpin->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent &) {
            if (m_lowSpin->GetValue() > m_highSpin->GetValue()) m_lowSpin->SetValue(m_highSpin->GetValue());
        });
    }
};

#endif /* EDITOR_INSTRUMENT_PANELS_H */
