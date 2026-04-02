#pragma once

#include <stdint.h>

#include <functional>

#include <wx/string.h>

extern "C" {
#include "NeoBAE.h"
}

class wxWindow;

class PianoRollPanel;

PianoRollPanel *CreatePianoRollPanel(wxWindow *parent);
wxWindow *PianoRollPanel_AsWindow(PianoRollPanel *panel);
void PianoRollPanel_SetDocument(PianoRollPanel *panel, BAERmfEditorDocument *document);
void PianoRollPanel_SetSelectedTrack(PianoRollPanel *panel, int trackIndex);
void PianoRollPanel_SetSelectionChangedCallback(PianoRollPanel *panel, std::function<void()> callback);
void PianoRollPanel_SetSeekRequestedCallback(PianoRollPanel *panel, std::function<void(uint32_t)> callback);
void PianoRollPanel_SetNotePreviewRequestedCallback(PianoRollPanel *panel,
                                                    std::function<void(uint16_t bank,
                                                                       unsigned char program,
                                                                       unsigned char channel,
                                                                       unsigned char note,
                                                                       uint32_t durationTicks,
                                                                       int trackIndex)> callback);
void PianoRollPanel_SetNotePreviewStopRequestedCallback(PianoRollPanel *panel,
                                                        std::function<void()> callback);
void PianoRollPanel_SetUndoCallbacks(PianoRollPanel *panel,
                                     std::function<void(wxString const &)> beginCallback,
                                     std::function<void(wxString const &)> commitCallback,
                                     std::function<void()> cancelCallback);
void PianoRollPanel_SetMidiLoopMarkers(PianoRollPanel *panel,
                                       bool enabled,
                                       uint32_t startTick,
                                       uint32_t endTick);
void PianoRollPanel_SetMidiLoopEditCallback(PianoRollPanel *panel,
                                            std::function<bool(bool enabled,
                                                               uint32_t startTick,
                                                               uint32_t endTick)> callback);
void PianoRollPanel_RefreshFromDocument(PianoRollPanel *panel, bool clearSelection);
bool PianoRollPanel_GetSelectedNoteInfo(PianoRollPanel *panel, BAERmfEditorNoteInfo *outNoteInfo);
void PianoRollPanel_SetSelectedNoteInstrument(PianoRollPanel *panel, uint16_t bank, unsigned char program);
void PianoRollPanel_SetNewNoteInstrument(PianoRollPanel *panel, uint16_t bank, unsigned char program);
void PianoRollPanel_SetPlayheadTick(PianoRollPanel *panel, uint32_t tick);
void PianoRollPanel_EnsurePlayheadVisible(PianoRollPanel *panel, uint32_t tick);
void PianoRollPanel_JumpToTick(PianoRollPanel *panel, uint32_t tick);
uint32_t PianoRollPanel_GetDocumentEndTick(PianoRollPanel *panel);
void PianoRollPanel_SetUserEndTick(PianoRollPanel *panel, uint32_t tick);
uint32_t PianoRollPanel_GetUserEndTick(PianoRollPanel *panel);
void PianoRollPanel_ClearPlayhead(PianoRollPanel *panel);
void PianoRollPanel_ScrollToC4Center(PianoRollPanel *panel);
void PianoRollPanel_Refresh(PianoRollPanel *panel);
void PianoRollPanel_SetAutoFollowPlayhead(PianoRollPanel *panel, bool enable);
bool PianoRollPanel_GetAutoFollowPlayhead(PianoRollPanel *panel);
void PianoRollPanel_SetAutoFollowChangedCallback(PianoRollPanel *panel, std::function<void(bool)> callback);
uint32_t PianoRollPanel_GetPlayheadTick(PianoRollPanel *panel);
