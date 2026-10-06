#pragma once

#include <string>
#include <string_view>

#include "Common/File/Path.h"

namespace CloudSave {

// A PSP save folder name: 9-64 chars of [A-Za-z0-9_-]. Safe to use as a path component.
bool IsValidSaveId(std::string_view saveId);
// The game ID is the first 9 characters of the save folder name, e.g. ULUS10336.
std::string GameIdFromSaveId(std::string_view saveId);
// Lowercase hex sha256.
std::string Sha256Hex(std::string_view data);

// Zips every non-hidden file below saveDir into memory. The output is byte-identical for identical
// folder contents (sorted entries, no compression, fixed timestamps), so its hash can be compared
// across devices. Returns false if there are no files or on error.
bool ZipSaveFolder(const Path &saveDir, std::string *zipData);

// Replaces savedataRoot/saveId with the zip's contents. Extracts into "<saveId>.cloudtmp" first and
// swaps it in, so a bad or unsafe zip leaves the existing save untouched.
bool ExtractSaveZip(std::string_view zipData, const Path &savedataRoot, std::string_view saveId, std::string *error);

}  // namespace CloudSave
