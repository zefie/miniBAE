#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/scrolwin.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/wx.h>

#include "editor_metadata_dialog.h"

namespace {

struct FieldDef {
    BAEInfoType type;
    char const *label;
    bool multiline;
};

static FieldDef const kFields[] = {
    { TITLE_INFO,             "Title",             false },
    { PERFORMED_BY_INFO,      "Performed By",      false },
    { COMPOSER_INFO,          "Composer",          false },
    { COPYRIGHT_INFO,         "Copyright",         false },
    { PUBLISHER_CONTACT_INFO, "Publisher Contact", false },
    { USE_OF_LICENSE_INFO,    "Use of License",    false },
    { LICENSED_TO_URL_INFO,   "Licensed to URL",   false },
    { LICENSE_TERM_INFO,      "License Term",      false },
    { EXPIRATION_DATE_INFO,   "Expiration Date",   false },
    { COMPOSER_NOTES_INFO,    "Composer Notes",    true  },
    { INDEX_NUMBER_INFO,      "Index Number",      false },
    { GENRE_INFO,             "Genre",             false },
    { SUB_GENRE_INFO,         "Sub-Genre",         false },
    { TEMPO_DESCRIPTION_INFO, "Tempo Description", false },
    { ORIGINAL_SOURCE_INFO,   "Original Source",   false },
};

static constexpr int kFieldCount = static_cast<int>(sizeof(kFields) / sizeof(kFields[0]));

class MetadataDialog final : public wxDialog {
public:
    MetadataDialog(wxWindow *parent, BAERmfEditorDocument *document)
        : wxDialog(parent,
                   wxID_ANY,
                   "RMF Metadata",
                   wxDefaultPosition,
                   wxSize(560, 560),
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          m_document(document) {
        wxBoxSizer *outerSizer;
        wxScrolledWindow *scrollWin;
        wxBoxSizer *scrollSizer;
        wxFlexGridSizer *gridSizer;
        wxStdDialogButtonSizer *btnSizer;

        outerSizer = new wxBoxSizer(wxVERTICAL);

        scrollWin = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
        scrollWin->SetScrollRate(0, 10);

        scrollSizer = new wxBoxSizer(wxVERTICAL);
        gridSizer = new wxFlexGridSizer(2, 10, 8);
        gridSizer->AddGrowableCol(1, 1);

        for (int i = 0; i < kFieldCount; ++i) {
            wxString initialValue;
            char const *stored;
            wxTextCtrl *ctrl;
            long style;

            stored = document ? BAERmfEditorDocument_GetInfo(document, kFields[i].type) : nullptr;
            if (stored && stored[0]) {
                initialValue = wxString::FromUTF8(stored);
            }

            style = 0;
            if (kFields[i].multiline) {
                style = wxTE_MULTILINE;
            }

            gridSizer->Add(new wxStaticText(scrollWin, wxID_ANY, kFields[i].label),
                           0, wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT);

            if (kFields[i].multiline) {
                ctrl = new wxTextCtrl(scrollWin,
                                      wxID_ANY,
                                      initialValue,
                                      wxDefaultPosition,
                                      wxSize(-1, 80),
                                      style);
            } else {
                ctrl = new wxTextCtrl(scrollWin,
                                      wxID_ANY,
                                      initialValue,
                                      wxDefaultPosition,
                                      wxDefaultSize,
                                      style);
            }
            gridSizer->Add(ctrl, 1, wxEXPAND);
            m_fields[i] = ctrl;
        }

        scrollSizer->Add(gridSizer, 1, wxEXPAND | wxALL, 10);
        scrollWin->SetSizer(scrollSizer);
        scrollSizer->FitInside(scrollWin);

        outerSizer->Add(scrollWin, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);

        btnSizer = CreateStdDialogButtonSizer(wxOK | wxCANCEL);
        outerSizer->Add(btnSizer, 0, wxEXPAND | wxALL, 8);

        SetSizer(outerSizer);
        Layout();
    }

    bool ApplyToDocument() {
        if (!m_document) {
            return false;
        }
        for (int i = 0; i < kFieldCount; ++i) {
            wxString val = m_fields[i]->GetValue();
            wxScopedCharBuffer utf8 = val.utf8_str();
            BAERmfEditorDocument_SetInfo(m_document, kFields[i].type, utf8.data());
        }
        return true;
    }

private:
    BAERmfEditorDocument *m_document;
    wxTextCtrl *m_fields[kFieldCount];
};

} // namespace

bool ShowMetadataDialog(wxWindow *parent, BAERmfEditorDocument *document) {
    MetadataDialog dlg(parent, document);

    if (dlg.ShowModal() != wxID_OK) {
        return false;
    }
    return dlg.ApplyToDocument();
}
