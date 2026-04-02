#pragma once

#include <wx/wx.h>

extern "C" {
#include "NeoBAE.h"
}

// Shows the RMF metadata editor dialog modally.
// If the user clicks OK, all field values are written back to the document
// via BAERmfEditorDocument_SetInfo and the function returns true.
// Returns false if the user cancelled or if document is null.
bool ShowMetadataDialog(wxWindow *parent, BAERmfEditorDocument *document);
