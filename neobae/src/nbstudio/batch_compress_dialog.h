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
