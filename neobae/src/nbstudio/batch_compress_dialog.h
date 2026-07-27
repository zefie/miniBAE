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

#pragma once

#include <wx/wx.h>
#include <vector>
#include "NeoBAE.h"

class BatchCompressDialog : public wxDialog
{
public:
    BatchCompressDialog(wxWindow *parent,
                        const std::vector<uint32_t> &sampleIndices,
                        bool compressAll = false,
                        BAERmfEditorCompressionType initialCompressionType = BAE_EDITOR_COMPRESSION_DONT_CHANGE,
                        BAERmfEditorOpusMode initialOpusMode = BAE_EDITOR_OPUS_MODE_AUDIO,
                        bool initialOpusRoundTrip = false);
    ~BatchCompressDialog();

    BAERmfEditorCompressionType GetSelectedCompressionType() const;
    BAERmfEditorOpusMode GetSelectedOpusMode() const;
    bool GetSelectedOpusRoundTrip() const;

private:
    wxChoice *m_codecChoice = nullptr;
    wxChoice *m_bitrateChoice = nullptr;
    wxChoice *m_opusModeChoice = nullptr;
    wxStaticText *m_samplesLabel = nullptr;

    void OnCodecSelected(wxCommandEvent &event);
    void UpdateBitrateChoice(int codecIdx);
    
    std::vector<uint32_t> m_sampleIndices;
    bool m_compressAll;

    static const int CODEC_CHOICES_COUNT = 10;
    static const int DEFAULT_CODEC = 6; // OPUS
};
