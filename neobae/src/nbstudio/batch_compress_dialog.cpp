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

#include "batch_compress_dialog.h"

namespace {

static std::pair<int, int> CompressionTypeToCodecBitrate(BAERmfEditorCompressionType t, bool opusRoundTrip)
{
    switch (t)
    {
        case BAE_EDITOR_COMPRESSION_DONT_CHANGE: return std::make_pair(0, 0);
        case BAE_EDITOR_COMPRESSION_PCM:         return std::make_pair(1, 0);
        case BAE_EDITOR_COMPRESSION_ADPCM:       return std::make_pair(2, 0);
        case BAE_EDITOR_COMPRESSION_MP3_32K:     return std::make_pair(3, 0);
        case BAE_EDITOR_COMPRESSION_MP3_48K:     return std::make_pair(3, 1);
        case BAE_EDITOR_COMPRESSION_MP3_64K:     return std::make_pair(3, 2);
        case BAE_EDITOR_COMPRESSION_MP3_96K:     return std::make_pair(3, 3);
        case BAE_EDITOR_COMPRESSION_MP3_128K:    return std::make_pair(3, 4);
        case BAE_EDITOR_COMPRESSION_MP3_192K:    return std::make_pair(3, 5);
        case BAE_EDITOR_COMPRESSION_MP3_256K:    return std::make_pair(3, 6);
        case BAE_EDITOR_COMPRESSION_MP3_320K:    return std::make_pair(3, 7);
        case BAE_EDITOR_COMPRESSION_VORBIS_32K:  return std::make_pair(4, 0);
        case BAE_EDITOR_COMPRESSION_VORBIS_48K:  return std::make_pair(4, 1);
        case BAE_EDITOR_COMPRESSION_VORBIS_64K:  return std::make_pair(4, 2);
        case BAE_EDITOR_COMPRESSION_VORBIS_80K:  return std::make_pair(4, 3);
        case BAE_EDITOR_COMPRESSION_VORBIS_96K:  return std::make_pair(4, 4);
        case BAE_EDITOR_COMPRESSION_VORBIS_128K: return std::make_pair(4, 5);
        case BAE_EDITOR_COMPRESSION_VORBIS_160K: return std::make_pair(4, 6);
        case BAE_EDITOR_COMPRESSION_VORBIS_192K: return std::make_pair(4, 7);
        case BAE_EDITOR_COMPRESSION_VORBIS_256K: return std::make_pair(4, 8);
        case BAE_EDITOR_COMPRESSION_FLAC:        return std::make_pair(5, 0);
        case BAE_EDITOR_COMPRESSION_OPUS_12K:    return std::make_pair(6, 0);
        case BAE_EDITOR_COMPRESSION_OPUS_16K:    return std::make_pair(6, 1);
        case BAE_EDITOR_COMPRESSION_OPUS_24K:    return std::make_pair(6, 2);
        case BAE_EDITOR_COMPRESSION_OPUS_32K:    return std::make_pair(6, 3);
        case BAE_EDITOR_COMPRESSION_OPUS_48K:    return std::make_pair(6, 4);
        case BAE_EDITOR_COMPRESSION_OPUS_64K:    return std::make_pair(6, 5);
        case BAE_EDITOR_COMPRESSION_OPUS_80K:    return std::make_pair(6, 6);
        case BAE_EDITOR_COMPRESSION_OPUS_96K:    return std::make_pair(6, 7);
        case BAE_EDITOR_COMPRESSION_OPUS_128K:   return std::make_pair(6, 8);
        case BAE_EDITOR_COMPRESSION_OPUS_160K:   return std::make_pair(6, 9);
        case BAE_EDITOR_COMPRESSION_OPUS_192K:   return std::make_pair(6, 10);
        case BAE_EDITOR_COMPRESSION_OPUS_256K:   return std::make_pair(6, 11);
#if USE_QOA_SUPPORT == TRUE
    case BAE_EDITOR_COMPRESSION_QOA:         return std::make_pair(7, 0);
#endif
        default:                                  return std::make_pair(6, 8);
    }
}

}

BatchCompressDialog::BatchCompressDialog(wxWindow *parent,
                                         const std::vector<uint32_t> &sampleIndices,
                                         bool compressAll,
                                         BAERmfEditorCompressionType initialCompressionType,
                                         BAERmfEditorOpusMode initialOpusMode,
                                         bool initialOpusRoundTrip)
    : wxDialog(parent, wxID_ANY, compressAll ? "Compress All Instruments" : "Compress Instrument",
               wxDefaultPosition, wxSize(450, 320), wxDEFAULT_DIALOG_STYLE),
      m_sampleIndices(sampleIndices),
      m_compressAll(compressAll)
{
    wxBoxSizer *mainSizer = new wxBoxSizer(wxVERTICAL);

    // Title/Description
    wxString descText = compressAll 
        ? wxString::Format("Compress all %zu samples", m_sampleIndices.size())
        : wxString::Format("Compress %zu sample(s)", m_sampleIndices.size());
    m_samplesLabel = new wxStaticText(this, wxID_ANY, descText);
    mainSizer->Add(m_samplesLabel, 0, wxALL | wxEXPAND, 10);

    // Codec selection
    wxBoxSizer *codecSizer = new wxBoxSizer(wxHORIZONTAL);
    wxStaticText *codecLabel = new wxStaticText(this, wxID_ANY, "Codec:");
    m_codecChoice = new wxChoice(this, wxID_ANY);
    m_codecChoice->Append("Original");
    m_codecChoice->Append("RAW PCM");
    m_codecChoice->Append("ADPCM");
    m_codecChoice->Append("MP3");
    m_codecChoice->Append("VORBIS");
    m_codecChoice->Append("FLAC");
    m_codecChoice->Append("OPUS (Round-Trip)");
#if USE_QOA_SUPPORT == TRUE
    m_codecChoice->Append("QOA");
#endif
    {
        std::pair<int, int> sel = CompressionTypeToCodecBitrate(initialCompressionType, initialOpusRoundTrip);
        int codecSel = sel.first;
        if (codecSel < 0 || codecSel >= (int)m_codecChoice->GetCount())
        {
            codecSel = DEFAULT_CODEC;
        }
        m_codecChoice->SetSelection(codecSel);
    }
    codecSizer->Add(codecLabel, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
    codecSizer->Add(m_codecChoice, 1, wxALL | wxEXPAND, 5);
    mainSizer->Add(codecSizer, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);

    // Bitrate selection
    wxBoxSizer *bitrateSizer = new wxBoxSizer(wxHORIZONTAL);
    wxStaticText *bitrateLabel = new wxStaticText(this, wxID_ANY, "Bitrate:");
    m_bitrateChoice = new wxChoice(this, wxID_ANY);
    bitrateSizer->Add(bitrateLabel, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
    bitrateSizer->Add(m_bitrateChoice, 1, wxALL | wxEXPAND, 5);
    mainSizer->Add(bitrateSizer, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);

    {
        std::pair<int, int> sel = CompressionTypeToCodecBitrate(initialCompressionType, initialOpusRoundTrip);
        int codecSel = m_codecChoice->GetSelection();
        int bitrateSel = sel.second;

        UpdateBitrateChoice(codecSel);
        if (m_bitrateChoice && m_bitrateChoice->IsEnabled() &&
            bitrateSel >= 0 && bitrateSel < (int)m_bitrateChoice->GetCount())
        {
            m_bitrateChoice->SetSelection(bitrateSel);
        }
    }

    wxBoxSizer *opusModeSizer = new wxBoxSizer(wxHORIZONTAL);
    wxStaticText *opusModeLabel = new wxStaticText(this, wxID_ANY, "Opus Mode:");
    m_opusModeChoice = new wxChoice(this, wxID_ANY);
    m_opusModeChoice->Append("Audio");
    m_opusModeChoice->Append("Music");
    m_opusModeChoice->Append("Voice");
    {
        int modeSel = 0;
        if (initialOpusMode == BAE_EDITOR_OPUS_MODE_VOICE)
        {
            modeSel = 1;
        }
        m_opusModeChoice->SetSelection(modeSel);
    }
    m_opusModeChoice->Enable(m_codecChoice->GetSelection() == 6);
    opusModeSizer->Add(opusModeLabel, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
    opusModeSizer->Add(m_opusModeChoice, 1, wxALL | wxEXPAND, 5);
    mainSizer->Add(opusModeSizer, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);

    // Buttons
    wxBoxSizer *buttonSizer = new wxBoxSizer(wxHORIZONTAL);
    wxButton *okBtn = new wxButton(this, wxID_OK, "OK");
    wxButton *cancelBtn = new wxButton(this, wxID_CANCEL, "Cancel");
    buttonSizer->Add(okBtn, 0, wxALL, 5);
    buttonSizer->Add(cancelBtn, 0, wxALL, 5);
    mainSizer->Add(buttonSizer, 0, wxALIGN_CENTER | wxALL, 10);

    SetSizer(mainSizer);

    // Bind events
    m_codecChoice->Bind(wxEVT_CHOICE, &BatchCompressDialog::OnCodecSelected, this);

    Centre();
}

BatchCompressDialog::~BatchCompressDialog()
{
}

void BatchCompressDialog::OnCodecSelected(wxCommandEvent &event)
{
    int codecIdx = m_codecChoice->GetSelection();
    UpdateBitrateChoice(codecIdx);
    if (m_opusModeChoice)
    {
        m_opusModeChoice->Enable(codecIdx == 6);
    }
}

void BatchCompressDialog::UpdateBitrateChoice(int codecIdx)
{
    m_bitrateChoice->Clear();

    switch (codecIdx)
    {
    case 0: // Original
    case 1: // RAW PCM
    case 2: // ADPCM
    case 5: // FLAC
#if USE_QOA_SUPPORT == TRUE
    case 7: // QOA
#endif
        m_bitrateChoice->Append("---");
        m_bitrateChoice->SetSelection(0);
        m_bitrateChoice->Enable(false);
        break;

    case 3: // MP3
        m_bitrateChoice->Append("32k");
        m_bitrateChoice->Append("48k");
        m_bitrateChoice->Append("64k");
        m_bitrateChoice->Append("96k");
        m_bitrateChoice->Append("128k");
        m_bitrateChoice->Append("192k");
        m_bitrateChoice->Append("256k");
        m_bitrateChoice->Append("320k");
        m_bitrateChoice->SetSelection(4); // 128k default
        m_bitrateChoice->Enable(true);
        break;

    case 4: // VORBIS
        m_bitrateChoice->Append("32k");
        m_bitrateChoice->Append("48k");
        m_bitrateChoice->Append("64k");
        m_bitrateChoice->Append("80k");
        m_bitrateChoice->Append("96k");
        m_bitrateChoice->Append("128k");
        m_bitrateChoice->Append("160k");
        m_bitrateChoice->Append("192k");
        m_bitrateChoice->Append("256k");
        m_bitrateChoice->SetSelection(5); // 128k default
        m_bitrateChoice->Enable(true);
        break;

    case 6: // OPUS (Round-Trip)
        m_bitrateChoice->Append("12k");
        m_bitrateChoice->Append("16k");
        m_bitrateChoice->Append("24k");
        m_bitrateChoice->Append("32k");
        m_bitrateChoice->Append("48k");
        m_bitrateChoice->Append("64k");
        m_bitrateChoice->Append("80k");
        m_bitrateChoice->Append("96k");
        m_bitrateChoice->Append("128k");
        m_bitrateChoice->Append("160k");
        m_bitrateChoice->Append("192k");
        m_bitrateChoice->Append("256k");
        m_bitrateChoice->SetSelection(8); // 128k default
        m_bitrateChoice->Enable(true);
        break;

    default:
        m_bitrateChoice->Append("---");
        m_bitrateChoice->SetSelection(0);
        m_bitrateChoice->Enable(false);
    }
}

BAERmfEditorCompressionType BatchCompressDialog::GetSelectedCompressionType() const
{
    int codecIdx = m_codecChoice->GetSelection();
    int bitrateIdx = m_bitrateChoice->GetSelection();

    switch (codecIdx)
    {
    case 0: return BAE_EDITOR_COMPRESSION_DONT_CHANGE;
    case 1: return BAE_EDITOR_COMPRESSION_PCM;
    case 2: return BAE_EDITOR_COMPRESSION_ADPCM;
    case 3: // MP3
    {
        switch (bitrateIdx)
        {
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
    }
    case 4: // VORBIS
    {
        switch (bitrateIdx)
        {
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
    }
    case 5: return BAE_EDITOR_COMPRESSION_FLAC;
    case 6: // OPUS (Round-Trip)
    {
        switch (bitrateIdx)
        {
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
        default: return BAE_EDITOR_COMPRESSION_OPUS_128K;
        }
    }
#if USE_QOA_SUPPORT == TRUE
    case 7: return BAE_EDITOR_COMPRESSION_QOA;
#endif
    default: return BAE_EDITOR_COMPRESSION_DONT_CHANGE;
    }
}

BAERmfEditorOpusMode BatchCompressDialog::GetSelectedOpusMode() const
{
    if (!m_opusModeChoice)
    {
        return BAE_EDITOR_OPUS_MODE_AUDIO;
    }
    switch (m_opusModeChoice->GetSelection())
    {
        case 1: return BAE_EDITOR_OPUS_MODE_VOICE;
        default:
            return BAE_EDITOR_OPUS_MODE_AUDIO;
    }
}

bool BatchCompressDialog::GetSelectedOpusRoundTrip() const
{
    if (!m_codecChoice)
    {
        return false;
    }
    return m_codecChoice->GetSelection() == 6;
}
