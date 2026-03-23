/****************************************************************************
 *
 * editor_bank.h
 *
 * Bank Editor Panel for NeoBAE Studio.
 * Provides a wxPanel-based UI for browsing and editing instruments
 * and samples within a loaded bank file (HSB/ZSB).
 *
 * C-style interface matching the project pattern used by other editor panels.
 *
 ****************************************************************************/

#ifndef EDITOR_BANK_H
#define EDITOR_BANK_H

#include <functional>

#include <wx/panel.h>

extern "C" {
#include "NeoBAE.h"
}

/* Opaque handle to the bank editor panel */
typedef struct BankEditorPanel BankEditorPanel;

/* Create a new BankEditorPanel as a child of the given parent window. */
BankEditorPanel *CreateBankEditorPanel(wxWindow *parent);

/* Return the underlying wxWindow* for layout purposes. */
wxWindow *BankEditorPanel_AsWindow(BankEditorPanel *panel);

/* Load a bank into the editor panel for display and editing.
 * bankToken must be a valid loaded bank token from BAEMixer_AddBankFromFile.
 * bankPath is the file path for display purposes. */
void BankEditorPanel_LoadBank(BankEditorPanel *panel,
                              BAEBankToken bankToken,
                              const char *bankPath);

/* Clear the bank editor panel (no bank loaded). */
void BankEditorPanel_Clear(BankEditorPanel *panel);

/* Set preview callbacks for piano and keyboard note preview.
 * playCallback: (bank, program, note, previewTag, isPercussion) - triggers a note-on
 * stopCallback: (previewTag) - triggers note-off for the given tag, or all if tag < 0
 * invalidateCallback: called when instrument parameters change so the engine reloads
 * dirtyParamsCallback: called with updated ExtInfo when the user edits instrument
 *     parameters (or nullptr to clear).  The host should store the info and apply
 *     it after BAESong_LoadInstrument via BAESong_PatchLoadedInstrumentExtInfo. */
void BankEditorPanel_SetPreviewCallbacks(
    BankEditorPanel *panel,
    std::function<void(unsigned char bank, unsigned char program, int note, int previewTag, bool isPercussion)> playCallback,
    std::function<void(int previewTag)> stopCallback,
    std::function<void()> invalidateCallback,
    std::function<void(BAERmfEditorInstrumentExtInfo const *info)> dirtyParamsCallback);

/* Optional callback to provide source codec text from cached original SND.
 * Returns true and fills outCodecDescription when a cached source exists. */
void BankEditorPanel_SetSourceCodecCallback(
    BankEditorPanel *panel,
    std::function<bool(uint16_t sndID, wxString &outCodecDescription)> sourceCodecCallback);

/* Set instrument context-menu callbacks (right-click on instrument tree items). */
void BankEditorPanel_SetInstrumentContextCallbacks(
    BankEditorPanel *panel,
    std::function<void(uint32_t instrumentIndex)> cloneToSongCallback,
    std::function<void(uint32_t instrumentIndex)> aliasToSongCallback,
    std::function<void(uint32_t instrumentIndex)> deleteFromSongCallback);

/* Return the index of the currently selected instrument (for dirty param commit). */
uint32_t BankEditorPanel_GetCurrentInstrumentIndex(BankEditorPanel *panel);

#endif /* EDITOR_BANK_H */
