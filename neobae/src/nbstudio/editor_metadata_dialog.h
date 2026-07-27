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

extern "C" {
#include "NeoBAE.h"
}

// Shows the RMF metadata editor dialog modally.
// If the user clicks OK, all field values are written back to the document
// via BAERmfEditorDocument_SetInfo and the function returns true.
// Returns false if the user cancelled or if document is null.
bool ShowMetadataDialog(wxWindow *parent, BAERmfEditorDocument *document);
