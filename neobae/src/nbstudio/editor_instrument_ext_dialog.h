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

#pragma once

#include <stdint.h>

#include <string>
#include <functional>
#include <vector>

#include <wx/string.h>

extern "C" {
#include "NeoBAE.h"
#include "X_Assert.h"
}

class wxWindow;

inline bool IsOpusCompressionType(BAERmfEditorCompressionType compressionType) {
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

struct InstrumentExtEditorResult {
    BAERmfEditorInstrumentExtInfo extInfo;
    bool extInfoModified;
    /* Sample edits are communicated via outSampleIndices/outEditedSamples
     * from the embedded sample tab (same structs as the old dialog). */
};

enum InstrumentPreviewInvalidateReason {
    kInstrumentPreviewInvalidate_None = 0,
    kInstrumentPreviewInvalidate_Ext = 1 << 0,
    kInstrumentPreviewInvalidate_SampleMeta = 1 << 1,
    kInstrumentPreviewInvalidate_SampleCodec = 1 << 2,
    kInstrumentPreviewInvalidate_SampleData = 1 << 3,
    kInstrumentPreviewInvalidate_All = 0xFFFFFFFFu
};

/* Show the combined instrument editor dialog with Instrument and Samples tabs,
 * plus a piano keyboard for preview. Returns true if user clicked OK or Apply
 * (and changes should be committed). */
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
    /* Output: sample edits (same as existing dialog) */
    std::vector<uint32_t> *outDeletedSampleIndices,
    std::vector<uint32_t> *outSampleIndices,
    std::vector<struct InstrumentEditorEditedSample> *outEditedSamples);


struct InstrumentEditorEditedSample {
    wxString displayName;
    wxString sourcePath;
    unsigned char program;
    unsigned char rootKey;
    unsigned char lowKey;
    unsigned char highKey;
    int16_t splitVolume;
    BAESampleInfo sampleInfo;
    BAERmfEditorCompressionType compressionType;
    bool hasOriginalData;
    BAERmfEditorSndStorageType sndStorageType;
    BAERmfEditorOpusMode opusMode;
    bool opusRoundTripResample;
};

bool ShowInstrumentEditorDialog(wxWindow *parent,
                                BAERmfEditorDocument const *document,
                                uint32_t primarySampleIndex,
                                std::function<void(uint32_t, int, BAESampleInfo const *, int16_t, unsigned char)> playCallback,
                                std::function<void()> stopCallback,
                                std::function<bool(uint32_t, wxString const &)> replaceCallback,
                                std::function<bool(uint32_t, wxString const &)> exportCallback,
                                std::vector<uint32_t> *outSampleIndices,
                                std::vector<InstrumentEditorEditedSample> *outEditedSamples);    
