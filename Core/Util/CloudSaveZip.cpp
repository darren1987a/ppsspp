#include <algorithm>
#include <cstdio>
#include <utility>
#include <vector>

#include "ext/libzip/zip.h"
#include "Common/Crypto/sha256.h"
#include "Common/File/DirListing.h"
#include "Common/File/FileUtil.h"
#include "Common/Log.h"
#include "Core/Util/CloudSaveZip.h"
#include "Core/Util/PathUtil.h"

namespace CloudSave {

bool IsValidSaveId(std::string_view saveId) {
	if (saveId.size() < 9 || saveId.size() > 64)
		return false;
	for (char c : saveId) {
		bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
		if (!ok)
			return false;
	}
	return true;
}

std::string GameIdFromSaveId(std::string_view saveId) {
	return std::string(saveId.substr(0, 9));
}

std::string Sha256Hex(std::string_view data) {
	sha256_context ctx;
	sha256_starts(&ctx);
	// sha256_update takes a 32-bit length, so feed it in chunks.
	size_t offset = 0;
	while (offset < data.size()) {
		uint32_t chunk = (uint32_t)std::min<size_t>(data.size() - offset, 1 << 20);
		sha256_update(&ctx, (const uint8_t *)data.data() + offset, chunk);
		offset += chunk;
	}
	uint8_t digest[32];
	sha256_finish(&ctx, digest);
	static const char hex[] = "0123456789abcdef";
	std::string out;
	out.reserve(64);
	for (uint8_t b : digest) {
		out.push_back(hex[b >> 4]);
		out.push_back(hex[b & 15]);
	}
	return out;
}

// Collects regular, non-hidden files below dir as (relative path with '/', full path).
static void CollectFiles(const Path &dir, const std::string &prefix, std::vector<std::pair<std::string, Path>> *out) {
	std::vector<File::FileInfo> entries;
	File::GetFilesInDir(dir, &entries);
	for (const auto &entry : entries) {
		if (entry.name.empty() || entry.name[0] == '.')
			continue;
		std::string rel = prefix.empty() ? entry.name : prefix + "/" + entry.name;
		if (entry.isDirectory) {
			CollectFiles(entry.fullName, rel, out);
		} else {
			out->emplace_back(rel, entry.fullName);
		}
	}
}

// DOS date for 1980-01-01 (day 1, month 1, year 0) and time 00:00:00. Set directly rather than
// from a time_t, which libzip would convert through the local time zone.
static const zip_uint16_t FIXED_DOS_DATE = (1 << 5) | 1;
static const zip_uint16_t FIXED_DOS_TIME = 0;

bool ZipSaveFolder(const Path &saveDir, std::string *zipData) {
	std::vector<std::pair<std::string, Path>> files;
	CollectFiles(saveDir, "", &files);
	if (files.empty())
		return false;
	std::sort(files.begin(), files.end(), [](const auto &a, const auto &b) { return a.first < b.first; });

	// libzip reads the buffers in zip_close(), so the contents must stay alive until then.
	std::vector<std::string> contents(files.size());

	zip_error_t error;
	zip_error_init(&error);
	zip_source_t *memSource = zip_source_buffer_create(nullptr, 0, 0, &error);
	if (!memSource) {
		zip_error_fini(&error);
		return false;
	}
	zip_t *z = zip_open_from_source(memSource, ZIP_TRUNCATE, &error);
	zip_error_fini(&error);
	if (!z) {
		zip_source_free(memSource);
		return false;
	}
	// zip_close() releases the archive's reference; keep ours so we can read the result.
	zip_source_keep(memSource);

	for (size_t i = 0; i < files.size(); i++) {
		if (!File::ReadBinaryFileToString(files[i].second, &contents[i])) {
			ERROR_LOG(Log::IO, "CloudSave: failed reading %s", files[i].second.c_str());
			zip_discard(z);
			zip_source_free(memSource);
			return false;
		}
		zip_source_t *src = zip_source_buffer(z, contents[i].data(), contents[i].size(), 0);
		zip_int64_t index = src ? zip_file_add(z, files[i].first.c_str(), src, ZIP_FL_ENC_UTF_8) : -1;
		if (index < 0) {
			if (src)
				zip_source_free(src);
			zip_discard(z);
			zip_source_free(memSource);
			return false;
		}
		zip_set_file_compression(z, index, ZIP_CM_STORE, 0);
		zip_file_set_dostime(z, index, FIXED_DOS_TIME, FIXED_DOS_DATE, 0);
	}

	if (zip_close(z) != 0) {
		zip_discard(z);
		zip_source_free(memSource);
		return false;
	}

	bool ok = false;
	if (zip_source_open(memSource) == 0) {
		zip_source_seek(memSource, 0, SEEK_END);
		zip_int64_t size = zip_source_tell(memSource);
		zip_source_seek(memSource, 0, SEEK_SET);
		if (size > 0) {
			zipData->resize((size_t)size);
			ok = zip_source_read(memSource, zipData->data(), (zip_uint64_t)size) == size;
		}
		zip_source_close(memSource);
	}
	zip_source_free(memSource);
	return ok;
}

// Copy of PathUtil's HasParentDirComponent, which v1.20.4 doesn't have yet.
static bool HasParentDirComponent(std::string_view path) {
	for (size_t i = 0; i < path.size(); ) {
		size_t end = path.find_first_of("/\\", i);
		size_t len = end == std::string_view::npos ? path.size() - i : end - i;
		if (len == 2 && path[i] == '.' && path[i + 1] == '.')
			return true;
		if (end == std::string_view::npos)
			break;
		i = end + 1;
	}
	return false;
}

static bool IsSafeEntryName(const std::string &name) {
	if (name.empty() || name[0] == '/' || name[0] == '\\')
		return false;
	if (name.find(':') != std::string::npos)
		return false;
	return !HasParentDirComponent(name);
}

bool ExtractSaveZip(std::string_view zipData, const Path &savedataRoot, std::string_view saveId, std::string *error) {
	if (!IsValidSaveId(saveId)) {
		*error = "Invalid save ID";
		return false;
	}

	zip_error_t zerr;
	zip_error_init(&zerr);
	zip_source_t *src = zip_source_buffer_create(zipData.data(), zipData.size(), 0, &zerr);
	zip_t *z = src ? zip_open_from_source(src, ZIP_RDONLY, &zerr) : nullptr;
	zip_error_fini(&zerr);
	if (!z) {
		if (src)
			zip_source_free(src);
		*error = "Downloaded file is not a valid zip";
		return false;
	}

	const std::string id(saveId);
	const Path tmpDir = savedataRoot / (id + ".cloudtmp");
	const Path bakDir = savedataRoot / (id + ".bak");
	const Path finalDir = savedataRoot / id;
	if (File::Exists(bakDir) && !File::Exists(finalDir)) {
		// An earlier swap failed halfway and ".bak" is the only copy of the old save. Don't touch it.
		zip_discard(z);
		*error = "An earlier download left your old save in " + id + ".bak. Rename it back to " + id + " first.";
		return false;
	}
	File::DeleteDirRecursively(tmpDir);
	File::CreateFullPath(tmpDir);

	bool ok = true;
	const zip_int64_t count = zip_get_num_entries(z, 0);
	for (zip_int64_t i = 0; i < count && ok; i++) {
		const char *rawName = zip_get_name(z, i, 0);
		const std::string name = rawName ? rawName : "";
		if (!IsSafeEntryName(name)) {
			*error = "Unsafe path in zip: " + name;
			ok = false;
			break;
		}
		if (name.back() == '/') {
			File::CreateFullPath(tmpDir / name);
			continue;
		}
		zip_stat_t st;
		zip_stat_init(&st);
		if (zip_stat_index(z, i, 0, &st) != 0) {
			*error = "Corrupt zip entry: " + name;
			ok = false;
			break;
		}
		std::string data((size_t)st.size, '\0');
		zip_file_t *zf = zip_fopen_index(z, i, 0);
		if (!zf || zip_fread(zf, data.data(), st.size) != (zip_int64_t)st.size) {
			*error = "Failed reading zip entry: " + name;
			ok = false;
		}
		if (zf)
			zip_fclose(zf);
		if (!ok)
			break;
		const Path outPath = tmpDir / name;
		File::CreateFullPath(outPath.NavigateUp());
		if (!File::WriteDataToFile(false, data.data(), data.size(), outPath)) {
			*error = "Failed writing " + name;
			ok = false;
		}
	}
	zip_discard(z);  // Also frees src.

	if (!ok) {
		File::DeleteDirRecursively(tmpDir);
		return false;
	}

	File::DeleteDirRecursively(bakDir);
	const bool hadOld = File::Exists(finalDir);
	if (hadOld && !File::Rename(finalDir, bakDir)) {
		File::DeleteDirRecursively(tmpDir);
		*error = "Could not move the old save out of the way";
		return false;
	}
	if (!File::Rename(tmpDir, finalDir)) {
		File::DeleteDirRecursively(tmpDir);
		if (hadOld && !File::Rename(bakDir, finalDir)) {
			*error = "Could not move the downloaded save into place. Your old save is in " + id + ".bak";
			return false;
		}
		*error = "Could not move the downloaded save into place";
		return false;
	}
	if (hadOld)
		File::DeleteDirRecursively(bakDir);
	return true;
}

}  // namespace CloudSave
