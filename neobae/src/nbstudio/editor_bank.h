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

/* Optional callback that returns staged, not-yet-committed edits for an
 * instrument so the panel can restore them without forcing a bank rewrite. */
void BankEditorPanel_SetPendingEditLookupCallback(
    BankEditorPanel *panel,
    std::function<bool(uint32_t instrumentIndex, BAERmfEditorInstrumentExtInfo *outInfo)> pendingEditLookupCallback);

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
    std::function<void(uint32_t instrumentIndex, uint32_t displayInstID)> deleteFromSongCallback,
    std::function<void(uint32_t instrumentIndex)> compressInstrumentSamplesCallback,
    std::function<void(uint32_t instIDBase)> addInstrumentCallback,
    std::function<void(uint32_t instrumentIndex)> cloneInstrumentCallback,
    std::function<void(uint32_t instrumentIndex)> aliasInstrumentCallback);

/* Set sample-list context-menu callbacks (right-click on sample rows). */
void BankEditorPanel_SetSampleContextCallbacks(
    BankEditorPanel *panel,
    std::function<void(uint32_t instrumentIndex, uint32_t sampleIndex)> addSampleCallback,
    std::function<void(uint32_t instrumentIndex, uint32_t sampleIndex)> deleteSampleCallback,
    std::function<void(uint32_t instrumentIndex, uint32_t sampleIndex)> aliasSampleToInstrumentCallback,
    std::function<void(uint32_t instrumentIndex, uint32_t sampleIndex)> copySampleToInstrumentCallback);

/* Set global sample-tree context-menu callbacks (for the new Samples tab).
 * sndID is the resource ID of the sample.
 * instrumentIndices/sampleIndices are the (instrument, sample) indices that reference this sndID.
 * These callbacks allow global sample operations across the entire bank. */
void BankEditorPanel_SetGlobalSampleContextCallbacks(
    BankEditorPanel *panel,
    std::function<void(uint16_t sndID)> addSampleToInstrumentCallback,
    std::function<void(uint16_t sndID)> copySampleToInstrumentCallback,
    std::function<void(uint16_t sndID, const std::vector<std::pair<uint32_t, uint32_t>> &referencingInstruments)> deleteSampleCallback,
    std::function<void(uint16_t sndID, const std::vector<std::pair<uint32_t, uint32_t>> &referencingInstruments)> editSampleCallback);

/* Return the index of the currently selected instrument (for dirty param commit). */
uint32_t BankEditorPanel_GetCurrentInstrumentIndex(BankEditorPanel *panel);

/* Select and display an instrument in the bank editor panel. */
void BankEditorPanel_SelectInstrument(BankEditorPanel *panel, uint32_t instrumentIndex);

/* Select and display a specific sample from the current instrument. */
void BankEditorPanel_SelectSample(BankEditorPanel *panel, uint32_t instrumentIndex, uint32_t sampleIndex);

#endif /* EDITOR_BANK_H */
