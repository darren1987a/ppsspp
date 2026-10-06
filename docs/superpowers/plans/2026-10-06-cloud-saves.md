# Cloud Saves Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Back up and restore PSP game saves (`PSP/SAVEDATA`) between the owner's PPSSPP builds on iPhone, iPad and Mac through DHService.

**Architecture:** Three small Core units (`CloudSaveZip`: zip/hash/extract; `CloudSaveState`: status rules + sync-state file; `CloudSaveClient`: DHService API) and one UI screen opened from the game's page. PPSSPP's HTTP layer gains custom request headers. The backend (Go, separate repo) is built by the `dhservice` Claude session from the contract in the spec; this plan only hands it off (Task 6).

**Tech Stack:** C++17, PPSSPP `Common/Net` (naett on Apple for HTTPS, `http::Client` for plain HTTP), libzip (`ext/libzip`), `Common/Crypto/sha256`, gason JSON (`Common/Data/Format/JSONReader/Writer`), PPSSPP UI framework, CMake. Unit tests in `unittest/` (`PPSSPPUnitTest`).

**Spec:** `docs/superpowers/specs/2026-10-06-cloud-saves-design.md`

## Global Constraints

- Branch `custom` only. Never push without the user's approval. Commit after each task.
- Base URL default: `https://dhservice.crazydarren.com/api/v1`. Dev: `http://127.0.0.1:9001/api/v1`.
- DHService JSON always answers HTTP 200; the result is the in-body `code`. Success = `code == 200` and (`data` present or `token` present). Error text = `msg`, falling back to `message`. Unauthorized = code `401` or `6401`.
- saveId regex: `^[A-Za-z0-9_-]{9,64}$`. gameId = first 9 characters of saveId.
- Keep the last 10 versions per save (server side). Max upload 20 MB.
- Device auth header: `Authorization: Device <token>`. Admin JWT header: `Authorization: Bearer <jwt>`.
- Device token stored only via `NativeSaveSecret("cloudsave", token)`; sync state in `GetSysDirectory(DIRECTORY_SYSTEM) / "cloudsave.json"`. Do not edit `Core/Config.cpp`.
- Zip: uncompressed (`ZIP_CM_STORE`), entries sorted by path, DOS date/time fixed to 1980-01-01 00:00 via `zip_file_set_dostime`, hidden files (name starts with `.`) skipped.
- **Line endings:** never assume; preserve what is on disk (AGENTS.md rule 5). Prefer the Edit tool. Check `git diff --stat` before each commit; a whole-file diff means line endings were rewritten.
- New source files must be listed in every build file that lists their neighbours: `Core/CMakeLists.txt`, `Core/Core.vcxproj` + `.filters`, `UWP/CoreUWP/CoreUWP.vcxproj` + `.filters`, `android/jni/Android.mk` (Core); `UI/CMakeLists.txt`, `UI/UI.vcxproj` + `.filters`, `UWP/UI_UWP/UI_UWP.vcxproj` + `.filters`, `android/jni/Android.mk` (UI). libretro does not build UI, so nothing references these files there; leave `libretro/Makefile.common` alone.
- UI strings use `T("English text")`, which falls back to the key; do not edit `assets/lang`.

## Review Focus

1. **Screen closed while a request is in flight** → callback must not touch the deleted screen. Pinned by the `alive_` flag in `CloudSaveClient` and test `TestCloudSaveClientAlive` (Task 4).
2. **Mac Finder junk (`.DS_Store`) or other hidden files in a save folder** → must not change the hash or be uploaded. Pinned by `TestCloudSaveZipDeterministic` (Task 2).
3. **Download interrupted / bad zip / unsafe entry** → existing local save must be untouched, no `.cloudtmp`/`.bak` left behind. Pinned by `TestCloudSaveExtractUnsafe` and `TestCloudSaveExtractNotZip` (Task 2).
4. **New device whose local save differs from an existing cloud save** (empty base) → must be ⚠ Conflict, never a silent overwrite either way. Pinned by the `newDeviceDiffers` case in `TestCloudSaveStatus` (Task 3).
5. **Error envelope that looks like success** (`code` 200 with no `data`, or `code` as a string from the 404 route) → must be treated as an error. Pinned by `TestCloudSaveEnvelope` (Task 4).

---

### Task 1: Custom HTTP request headers

**Files:**
- Modify: `Common/Net/HTTPClient.h` (struct `RequestParams`, ~line 58)
- Modify: `Common/Net/HTTPClient.cpp` (`Client::SendRequestWithData` ~line 367, `HTTPRequest::Perform` ~line 590)
- Modify: `Common/Net/HTTPRequest.h` (class `Request`, class `RequestManager`)
- Modify: `Common/Net/HTTPRequest.cpp` (add `RequestManager::StartRequest`)
- Modify: `Common/Net/HTTPNaettRequest.cpp` (`HTTPSRequest::Start`, ~line 89)
- Create: `unittest/TestCloudSave.cpp`
- Modify: `unittest/UnitTest.cpp` (declaration near line 2942, `TEST_ITEM` near line 3165), `CMakeLists.txt` (unittest list ~line 1373), `unittest/UnitTests.vcxproj` + `.filters`, `android/jni/Android.mk` (~line 1064)

**Interfaces:**
- Produces:
  - `http::RequestParams::extraHeaders` (`std::string`, each header ends with `\r\n`)
  - `void http::Request::AddHeader(std::string_view name, std::string_view value)`
  - `std::shared_ptr<http::Request> http::RequestManager::StartRequest(http::RequestMethod method, std::string_view url, std::string_view body, std::string_view mime, const std::vector<std::pair<std::string, std::string>> &headers, http::RequestFlags flags, http::RequestCompletionCallback callback, std::string_view name = "")`
  - `bool TestCloudSave()` in `unittest/TestCloudSave.cpp`, registered as `TEST_ITEM(CloudSave)`; later tasks add sub-tests to it.

- [ ] **Step 1: Write the failing test**

Create `unittest/TestCloudSave.cpp`:

```cpp
#include <string>
#include <thread>

#include "Common/Buffer.h"
#include "Common/Net/HTTPClient.h"
#include "Common/Net/HTTPServer.h"

#include "UnitTest.h"

// An in-process server echoes the X-Cloud-Test request header, proving extraHeaders reach the wire.
static bool TestCloudSaveHttpHeaders() {
	http::Server server(new http::NewThreadExecutor());
	std::string received;
	server.RegisterHandler("/echo", [&received](const http::ServerRequest &request) {
		request.GetHeader("x-cloud-test", &received);
		request.WriteHttpResponseHeader("1.0", 200, received.size(), "text/plain");
		request.Out()->Push(received);
	});
	EXPECT_TRUE(server.Listen(0, "unittest", net::DNSType::IPV4));
	const int port = server.Port();
	std::thread serveThread([&server] { server.RunSlice(5.0); });

	http::Client client(nullptr);
	EXPECT_TRUE(client.Resolve("127.0.0.1", port));
	EXPECT_TRUE(client.Connect(2, 5.0));
	http::RequestParams req("/echo", "*/*");
	req.extraHeaders = "X-Cloud-Test: hello-header\r\n";
	Buffer output;
	int code = client.GET(req, &output, nullptr);
	serveThread.join();

	EXPECT_EQ_INT(code, 200);
	std::string body;
	output.TakeAll(&body);
	EXPECT_EQ_STR(body, std::string("hello-header"));
	return true;
}

bool TestCloudSave() {
	if (!TestCloudSaveHttpHeaders())
		return false;
	return true;
}
```

Register it:
- `unittest/UnitTest.cpp`: next to `bool TestZipSlip();` add `bool TestCloudSave();`; next to `TEST_ITEM(ZipSlip),` add `TEST_ITEM(CloudSave),`.
- `CMakeLists.txt`: after `unittest/TestZipSlip.cpp` add `unittest/TestCloudSave.cpp`.
- `android/jni/Android.mk`: after `$(SRC)/unittest/TestZipSlip.cpp \` add `    $(SRC)/unittest/TestCloudSave.cpp \` (copy the exact indentation of the neighbouring line).
- `unittest/UnitTests.vcxproj`: after `<ClCompile Include="TestZipSlip.cpp" />` add `<ClCompile Include="TestCloudSave.cpp" />` (same indentation).
- `unittest/UnitTests.vcxproj.filters`: after `<ClCompile Include="TestZipSlip.cpp" />` add `<ClCompile Include="TestCloudSave.cpp" />`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake -S . -B build-unittest -DUNITTEST=ON -DHEADLESS=ON -DCMAKE_BUILD_TYPE=Debug && cmake --build build-unittest --target PPSSPPUnitTest -j11`
Expected: compile error `no member named 'extraHeaders' in 'http::RequestParams'`.

- [ ] **Step 3: Implement**

`Common/Net/HTTPClient.h`, in `struct RequestParams` after `const char *acceptMime = "*/*";`:

```cpp
	// Extra request header lines, each ending in "\r\n". Sent as-is.
	std::string extraHeaders;
```

`Common/Net/HTTPClient.cpp`, in `Client::SendRequestWithData`, change the template and arguments so `extraHeaders` follows `otherHeaders`:

```cpp
	const char *tpl =
		"%s %s HTTP/%s\r\n"
		"Host: %s\r\n"
		"User-Agent: %s\r\n"
		"Accept: %s\r\n"
		"Connection: close\r\n"
		"%s"
		"%s"
		"\r\n";

	buffer.Printf(tpl,
		method, req.resource.c_str(), HTTP_VERSION,
		host_.c_str(),
		userAgent_.c_str(),
		req.acceptMime,
		otherHeaders ? otherHeaders : "",
		req.extraHeaders.c_str());
```

`Common/Net/HTTPRequest.h`, in class `Request` public section (after `SetUserAgent`):

```cpp
	// Must be called before Start().
	void AddHeader(std::string_view name, std::string_view value) {
		headers_.emplace_back(std::string(name), std::string(value));
	}
```

and in its protected section (after `std::string userAgent_;`):

```cpp
	std::vector<std::pair<std::string, std::string>> headers_;
```

In class `RequestManager` after `AsyncPostWithCallback(...)`:

```cpp
	// General request with custom headers. body/mime are only used for POST.
	std::shared_ptr<Request> StartRequest(
		RequestMethod method,
		std::string_view url,
		std::string_view body,
		std::string_view mime,
		const std::vector<std::pair<std::string, std::string>> &headers,
		RequestFlags flags,
		RequestCompletionCallback callback,
		std::string_view name = "");
```

Add `#include <utility>` and `#include <vector>` to `HTTPRequest.h` if not already included.

`Common/Net/HTTPRequest.cpp`, after `AsyncPostWithCallback`:

```cpp
std::shared_ptr<Request> RequestManager::StartRequest(
	RequestMethod method,
	std::string_view url,
	std::string_view body,
	std::string_view mime,
	const std::vector<std::pair<std::string, std::string>> &headers,
	RequestFlags flags,
	RequestCompletionCallback callback,
	std::string_view name) {
	std::shared_ptr<Request> dl = CreateRequest(method, url, body, mime, Path(), flags, nullptr, name);
	if (!dl)
		return dl;
	if (!userAgent_.empty())
		dl->SetUserAgent(userAgent_);
	for (const auto &[key, value] : headers)
		dl->AddHeader(key, value);
	dl->SetCallback(callback);
	newDownloads_.push_back(dl);
	dl->Start();
	return dl;
}
```

`Common/Net/HTTPNaettRequest.cpp`, in `HTTPSRequest::Start()` right after `options.push_back(naettUserAgent(userAgent_.c_str()));` (naett copies header strings with `strdup`):

```cpp
	for (const auto &[key, value] : headers_) {
		options.push_back(naettHeader(key.c_str(), value.c_str()));
	}
```

`Common/Net/HTTPClient.cpp`, in `HTTPRequest::Perform`, right after `RequestParams req(fileUrl.Resource(), acceptMime_);`:

```cpp
	for (const auto &[key, value] : headers_) {
		req.extraHeaders += key + ": " + value + "\r\n";
	}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build-unittest --target PPSSPPUnitTest -j11 && build-unittest/PPSSPPUnitTest CloudSave`
Expected: `CloudSave` passes. Then `build-unittest/PPSSPPUnitTest all` — no new failures compared to before the change.

- [ ] **Step 5: Build the Mac app to confirm nothing else broke**

Run: `./b.sh --release`
Expected: `[100%] Built target PPSSPPSDL`.

- [ ] **Step 6: Commit**

```bash
git diff --stat   # only the intended lines; no whole-file rewrites
git add Common/Net unittest CMakeLists.txt android/jni/Android.mk
git commit -m "Net: custom request headers and RequestManager::StartRequest"
```

---

### Task 2: CloudSaveZip — saveId rules, sha256, deterministic zip, safe extract

**Files:**
- Create: `Core/Util/CloudSaveZip.h`, `Core/Util/CloudSaveZip.cpp`
- Modify: `unittest/TestCloudSave.cpp`
- Modify build lists (Core): `Core/CMakeLists.txt` (after `Util/PathUtil.h`), `Core/Core.vcxproj` (after `<ClCompile Include="Util\PathUtil.cpp" />` and `<ClInclude Include="Util\PathUtil.h" />`), `Core/Core.vcxproj.filters` (copy the `Util\PathUtil.*` blocks with `<Filter>Util</Filter>`), `UWP/CoreUWP/CoreUWP.vcxproj` + `.filters` (after the `..\..\Core\Util\PathUtil.*` lines), `android/jni/Android.mk` (after `$(SRC)/Core/Util/PathUtil.cpp \`)

**Interfaces:**
- Consumes: `HasParentDirComponent(std::string_view)` from `Core/Util/PathUtil.h`.
- Produces (namespace `CloudSave`):
  - `bool IsValidSaveId(std::string_view saveId)`
  - `std::string GameIdFromSaveId(std::string_view saveId)`
  - `std::string Sha256Hex(std::string_view data)` — lowercase hex, 64 chars
  - `bool ZipSaveFolder(const Path &saveDir, std::string *zipData)` — false if the folder has no files or on error
  - `bool ExtractSaveZip(std::string_view zipData, const Path &savedataRoot, std::string_view saveId, std::string *error)`

- [ ] **Step 1: Write the failing tests**

Add to `unittest/TestCloudSave.cpp` (new includes at the top, functions above `TestCloudSave()`):

```cpp
#include "ext/libzip/zip.h"
#include "Common/File/FileUtil.h"
#include "Common/File/Path.h"
#include "Core/Util/CloudSaveZip.h"

static bool TestCloudSaveIds() {
	EXPECT_TRUE(CloudSave::IsValidSaveId("ULUS10336DATA00"));
	EXPECT_TRUE(CloudSave::IsValidSaveId("NPJH50012_save-1"));
	EXPECT_FALSE(CloudSave::IsValidSaveId("ULUS1033"));           // 8 chars
	EXPECT_FALSE(CloudSave::IsValidSaveId("../ULUS10336"));
	EXPECT_FALSE(CloudSave::IsValidSaveId("ULUS10336 DATA"));
	EXPECT_FALSE(CloudSave::IsValidSaveId(std::string(65, 'A')));
	EXPECT_EQ_STR(CloudSave::GameIdFromSaveId("ULUS10336DATA00"), std::string("ULUS10336"));
	EXPECT_EQ_STR(CloudSave::Sha256Hex("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
	return true;
}

static void WriteFile(const Path &path, const std::string &data) {
	File::CreateFullPath(path.NavigateUp());
	File::WriteStringToFile(false, data, path);
}

static bool TestCloudSaveZipDeterministic() {
	Path root("unittest_cloudsave_zip");
	File::DeleteDirRecursively(root);
	// Same content, created in a different order, one with Finder junk.
	Path a = root / "a" / "ULUS10336DATA00";
	WriteFile(a / "DATA.BIN", "save-bytes");
	WriteFile(a / "sub" / "ICON0.PNG", "icon");
	WriteFile(a / ".DS_Store", "junk");
	Path b = root / "b" / "ULUS10336DATA00";
	WriteFile(b / "sub" / "ICON0.PNG", "icon");
	WriteFile(b / "DATA.BIN", "save-bytes");

	std::string zipA1, zipA2, zipB;
	EXPECT_TRUE(CloudSave::ZipSaveFolder(a, &zipA1));
	EXPECT_TRUE(CloudSave::ZipSaveFolder(a, &zipA2));
	EXPECT_TRUE(CloudSave::ZipSaveFolder(b, &zipB));
	EXPECT_EQ_STR(CloudSave::Sha256Hex(zipA1), CloudSave::Sha256Hex(zipA2));
	EXPECT_EQ_STR(CloudSave::Sha256Hex(zipA1), CloudSave::Sha256Hex(zipB));

	// Different content gives a different hash.
	WriteFile(b / "DATA.BIN", "other-bytes");
	EXPECT_TRUE(CloudSave::ZipSaveFolder(b, &zipB));
	EXPECT_FALSE(CloudSave::Sha256Hex(zipA1) == CloudSave::Sha256Hex(zipB));

	// Empty folder can't be zipped.
	File::CreateFullPath(root / "empty");
	std::string zipEmpty;
	EXPECT_FALSE(CloudSave::ZipSaveFolder(root / "empty", &zipEmpty));
	File::DeleteDirRecursively(root);
	return true;
}

static bool TestCloudSaveExtractRoundTrip() {
	Path root("unittest_cloudsave_extract");
	File::DeleteDirRecursively(root);
	Path src = root / "src" / "ULUS10336DATA00";
	WriteFile(src / "DATA.BIN", "save-bytes");
	WriteFile(src / "sub" / "ICON0.PNG", "icon");
	std::string zip;
	EXPECT_TRUE(CloudSave::ZipSaveFolder(src, &zip));

	Path dest = root / "SAVEDATA";
	WriteFile(dest / "ULUS10336DATA00" / "OLD.BIN", "old");  // must be replaced
	std::string error;
	EXPECT_TRUE(CloudSave::ExtractSaveZip(zip, dest, "ULUS10336DATA00", &error));

	std::string data;
	EXPECT_TRUE(File::ReadBinaryFileToString(dest / "ULUS10336DATA00" / "DATA.BIN", &data));
	EXPECT_EQ_STR(data, std::string("save-bytes"));
	EXPECT_TRUE(File::ReadBinaryFileToString(dest / "ULUS10336DATA00" / "sub" / "ICON0.PNG", &data));
	EXPECT_EQ_STR(data, std::string("icon"));
	EXPECT_FALSE(File::Exists(dest / "ULUS10336DATA00" / "OLD.BIN"));
	EXPECT_FALSE(File::Exists(dest / "ULUS10336DATA00.bak"));
	EXPECT_FALSE(File::Exists(dest / "ULUS10336DATA00.cloudtmp"));
	File::DeleteDirRecursively(root);
	return true;
}

// Builds a zip in memory with a single entry of the given name.
static std::string MakeZipWithEntry(const Path &tmpZip, const std::string &entryName) {
	int errorp = 0;
	zip_t *z = zip_open(tmpZip.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &errorp);
	static const char contents[] = "evil";
	zip_source_t *source = zip_source_buffer(z, contents, 4, 0);
	zip_file_add(z, entryName.c_str(), source, ZIP_FL_ENC_UTF_8);
	zip_close(z);
	std::string data;
	File::ReadBinaryFileToString(tmpZip, &data);
	return data;
}

static bool TestCloudSaveExtractUnsafe() {
	Path root("unittest_cloudsave_unsafe");
	File::DeleteDirRecursively(root);
	Path dest = root / "SAVEDATA";
	WriteFile(dest / "ULUS10336DATA00" / "DATA.BIN", "keep-me");
	std::string zip = MakeZipWithEntry(root / "bad.zip", "../evil.txt");
	std::string error;
	EXPECT_FALSE(CloudSave::ExtractSaveZip(zip, dest, "ULUS10336DATA00", &error));
	EXPECT_FALSE(error.empty());
	EXPECT_FALSE(File::Exists(dest / "evil.txt"));
	std::string data;
	EXPECT_TRUE(File::ReadBinaryFileToString(dest / "ULUS10336DATA00" / "DATA.BIN", &data));
	EXPECT_EQ_STR(data, std::string("keep-me"));
	EXPECT_FALSE(File::Exists(dest / "ULUS10336DATA00.cloudtmp"));
	File::DeleteDirRecursively(root);
	return true;
}

static bool TestCloudSaveExtractNotZip() {
	Path root("unittest_cloudsave_notzip");
	File::DeleteDirRecursively(root);
	Path dest = root / "SAVEDATA";
	WriteFile(dest / "ULUS10336DATA00" / "DATA.BIN", "keep-me");
	std::string error;
	EXPECT_FALSE(CloudSave::ExtractSaveZip("hello, not a zip", dest, "ULUS10336DATA00", &error));
	EXPECT_FALSE(CloudSave::ExtractSaveZip("PK", dest, "../x", &error));  // bad saveId
	std::string data;
	EXPECT_TRUE(File::ReadBinaryFileToString(dest / "ULUS10336DATA00" / "DATA.BIN", &data));
	EXPECT_EQ_STR(data, std::string("keep-me"));
	EXPECT_FALSE(File::Exists(dest / "ULUS10336DATA00.cloudtmp"));
	File::DeleteDirRecursively(root);
	return true;
}
```

In `TestCloudSave()`, before `return true;`:

```cpp
	if (!TestCloudSaveIds())
		return false;
	if (!TestCloudSaveZipDeterministic())
		return false;
	if (!TestCloudSaveExtractRoundTrip())
		return false;
	if (!TestCloudSaveExtractUnsafe())
		return false;
	if (!TestCloudSaveExtractNotZip())
		return false;
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build-unittest --target PPSSPPUnitTest -j11`
Expected: compile error `'Core/Util/CloudSaveZip.h' file not found`.

- [ ] **Step 3: Implement**

`Core/Util/CloudSaveZip.h`:

```cpp
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
```

`Core/Util/CloudSaveZip.cpp`:

```cpp
#include <algorithm>
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
		if (hadOld)
			File::Rename(bakDir, finalDir);
		File::DeleteDirRecursively(tmpDir);
		*error = "Could not move the downloaded save into place";
		return false;
	}
	if (hadOld)
		File::DeleteDirRecursively(bakDir);
	return true;
}

}  // namespace CloudSave
```

Add both files to the Core build lists named in **Files** above, copying the exact format of the `PathUtil` neighbours (for `.vcxproj.filters`, use `<Filter>Util</Filter>`).

- [ ] **Step 4: Run tests to verify they pass**

Run: `cmake -S . -B build-unittest && cmake --build build-unittest --target PPSSPPUnitTest -j11 && build-unittest/PPSSPPUnitTest CloudSave`
Expected: PASS. If `TestCloudSaveZipDeterministic` fails, dump both zips with `xxd` and compare: the difference shows which header field still varies, and it must be pinned before moving on.

- [ ] **Step 5: Commit**

```bash
git diff --stat
git add Core/Util/CloudSaveZip.* Core/CMakeLists.txt Core/Core.vcxproj Core/Core.vcxproj.filters UWP/CoreUWP android/jni/Android.mk unittest/TestCloudSave.cpp
git commit -m "CloudSave: deterministic save zip, sha256 and safe extract"
```

---

### Task 3: CloudSaveState — status rules and sync-state file

**Files:**
- Create: `Core/Util/CloudSaveState.h`, `Core/Util/CloudSaveState.cpp`
- Modify: `unittest/TestCloudSave.cpp`
- Modify build lists (Core): same files and positions as Task 2, for `Util/CloudSaveState.cpp/.h`

**Interfaces:**
- Produces (namespace `CloudSave`):
  - `enum class SyncStatus { Synced, LocalNewer, CloudNewer, Conflict, LocalOnly, CloudOnly }`
  - `struct SyncBase { int version = 0; std::string sha256; }`
  - `SyncStatus ComputeStatus(bool hasLocal, const std::string &localSha256, const SyncBase &base, int cloudVersion, const std::string &cloudSha256)` — `cloudVersion == 0` means no cloud copy
  - `const char *DEFAULT_SERVER_URL` = `"https://dhservice.crazydarren.com/api/v1"`
  - `struct SyncState { std::string serverUrl = DEFAULT_SERVER_URL; std::string username; std::string deviceName; std::map<std::string, SyncBase> bases; bool Load(const Path &file); bool Save(const Path &file) const; }`
  - `Path SyncStateFile()` → `GetSysDirectory(DIRECTORY_SYSTEM) / "cloudsave.json"`

- [ ] **Step 1: Write the failing tests**

Add to `unittest/TestCloudSave.cpp`:

```cpp
#include "Core/Util/CloudSaveState.h"

static bool TestCloudSaveStatus() {
	using CloudSave::SyncStatus;
	using CloudSave::ComputeStatus;
	const CloudSave::SyncBase base{3, "aaa"};
	const CloudSave::SyncBase noBase{};

	EXPECT_TRUE(ComputeStatus(true, "aaa", base, 3, "aaa") == SyncStatus::Synced);
	EXPECT_TRUE(ComputeStatus(true, "bbb", base, 3, "aaa") == SyncStatus::LocalNewer);
	EXPECT_TRUE(ComputeStatus(true, "aaa", base, 4, "ccc") == SyncStatus::CloudNewer);
	EXPECT_TRUE(ComputeStatus(true, "bbb", base, 4, "ccc") == SyncStatus::Conflict);
	EXPECT_TRUE(ComputeStatus(true, "bbb", noBase, 0, "") == SyncStatus::LocalOnly);
	EXPECT_TRUE(ComputeStatus(false, "", noBase, 2, "ccc") == SyncStatus::CloudOnly);
	// Someone else uploaded the same bytes we have: nothing to do.
	EXPECT_TRUE(ComputeStatus(true, "ccc", base, 4, "ccc") == SyncStatus::Synced);
	// newDeviceDiffers: fresh device, its own save differs from the cloud's. Must ask, not overwrite.
	EXPECT_TRUE(ComputeStatus(true, "bbb", noBase, 2, "ccc") == SyncStatus::Conflict);
	return true;
}

static bool TestCloudSaveStateFile() {
	Path file("unittest_cloudsave_state.json");
	File::Delete(file);

	CloudSave::SyncState missing;
	EXPECT_FALSE(missing.Load(file));
	EXPECT_EQ_STR(missing.serverUrl, std::string(CloudSave::DEFAULT_SERVER_URL));

	CloudSave::SyncState state;
	state.serverUrl = "http://127.0.0.1:9001/api/v1";
	state.username = "darren";
	state.deviceName = "Darren \"Mac\"";  // needs escaping
	state.bases["ULUS10336DATA00"] = {3, "aaa"};
	state.bases["ULUS10336DATA01"] = {1, "bbb"};
	EXPECT_TRUE(state.Save(file));

	CloudSave::SyncState loaded;
	EXPECT_TRUE(loaded.Load(file));
	EXPECT_EQ_STR(loaded.serverUrl, state.serverUrl);
	EXPECT_EQ_STR(loaded.username, state.username);
	EXPECT_EQ_STR(loaded.deviceName, state.deviceName);
	EXPECT_EQ_INT((int)loaded.bases.size(), 2);
	EXPECT_EQ_INT(loaded.bases["ULUS10336DATA00"].version, 3);
	EXPECT_EQ_STR(loaded.bases["ULUS10336DATA01"].sha256, std::string("bbb"));
	File::Delete(file);
	return true;
}
```

In `TestCloudSave()`:

```cpp
	if (!TestCloudSaveStatus())
		return false;
	if (!TestCloudSaveStateFile())
		return false;
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build-unittest --target PPSSPPUnitTest -j11`
Expected: compile error `'Core/Util/CloudSaveState.h' file not found`.

- [ ] **Step 3: Implement**

`Core/Util/CloudSaveState.h`:

```cpp
#pragma once

#include <map>
#include <string>

#include "Common/File/Path.h"

namespace CloudSave {

extern const char *DEFAULT_SERVER_URL;

enum class SyncStatus {
	Synced,      // local == cloud latest
	LocalNewer,  // local changed since the last sync, cloud didn't
	CloudNewer,  // cloud got a newer version, local unchanged since the last sync
	Conflict,    // both changed (or a new device whose save differs from the cloud)
	LocalOnly,   // never uploaded
	CloudOnly,   // not on this device
};

// What this device last uploaded or downloaded for a save.
struct SyncBase {
	int version = 0;
	std::string sha256;
};

// cloudVersion == 0 means the cloud has no copy.
SyncStatus ComputeStatus(bool hasLocal, const std::string &localSha256, const SyncBase &base, int cloudVersion, const std::string &cloudSha256);

struct SyncState {
	std::string serverUrl = DEFAULT_SERVER_URL;
	std::string username;
	std::string deviceName;
	std::map<std::string, SyncBase> bases;  // by saveId

	// Returns false if the file is missing or invalid; fields then keep their defaults.
	bool Load(const Path &file);
	bool Save(const Path &file) const;
};

// PSP/SYSTEM/cloudsave.json
Path SyncStateFile();

}  // namespace CloudSave
```

`Core/Util/CloudSaveState.cpp`:

```cpp
#include "Common/Data/Format/JSONReader.h"
#include "Common/Data/Format/JSONWriter.h"
#include "Common/File/FileUtil.h"
#include "Core/Util/CloudSaveState.h"
#include "Core/Util/PathUtil.h"

namespace CloudSave {

const char *DEFAULT_SERVER_URL = "https://dhservice.crazydarren.com/api/v1";

SyncStatus ComputeStatus(bool hasLocal, const std::string &localSha256, const SyncBase &base, int cloudVersion, const std::string &cloudSha256) {
	if (!hasLocal)
		return SyncStatus::CloudOnly;
	if (cloudVersion == 0)
		return SyncStatus::LocalOnly;
	if (localSha256 == cloudSha256)
		return SyncStatus::Synced;
	if (cloudVersion > base.version) {
		return localSha256 == base.sha256 ? SyncStatus::CloudNewer : SyncStatus::Conflict;
	}
	return SyncStatus::LocalNewer;
}

bool SyncState::Load(const Path &file) {
	std::string data;
	if (!File::ReadTextFileToString(file, &data) || data.empty())
		return false;
	json::JsonReader reader(data.c_str(), data.size());
	const json::JsonGet root = reader.root();
	if (!root)
		return false;
	serverUrl = root.getStringOr("serverUrl", DEFAULT_SERVER_URL);
	username = root.getStringOr("username", "");
	deviceName = root.getStringOr("deviceName", "");
	bases.clear();
	const JsonNode *basesNode = root.get("bases", JSON_OBJECT);
	if (basesNode) {
		for (const JsonNode *entry : basesNode->value) {
			const json::JsonGet item(entry->value);
			SyncBase base;
			base.version = item.getInt("version", 0);
			base.sha256 = item.getStringOr("sha256", "");
			bases[entry->key] = base;
		}
	}
	return true;
}

bool SyncState::Save(const Path &file) const {
	json::JsonWriter writer;
	writer.begin();
	writer.writeString("serverUrl", serverUrl);
	writer.writeString("username", username);
	writer.writeString("deviceName", deviceName);
	writer.pushDict("bases");
	for (const auto &[saveId, base] : bases) {
		writer.pushDict(saveId);
		writer.writeInt("version", base.version);
		writer.writeString("sha256", base.sha256);
		writer.pop();
	}
	writer.pop();
	writer.end();
	return File::WriteStringToFile(true, writer.str(), file);
}

Path SyncStateFile() {
	return GetSysDirectory(DIRECTORY_SYSTEM) / "cloudsave.json";
}

}  // namespace CloudSave
```

Add `Util/CloudSaveState.cpp/.h` to the same Core build lists as Task 2.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cmake -S . -B build-unittest && cmake --build build-unittest --target PPSSPPUnitTest -j11 && build-unittest/PPSSPPUnitTest CloudSave`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git diff --stat
git add Core/Util/CloudSaveState.* Core/CMakeLists.txt Core/Core.vcxproj Core/Core.vcxproj.filters UWP/CoreUWP android/jni/Android.mk unittest/TestCloudSave.cpp
git commit -m "CloudSave: sync status rules and cloudsave.json state"
```

---

### Task 4: CloudSaveClient — DHService API and envelope parsing

**Files:**
- Create: `Core/Util/CloudSaveClient.h`, `Core/Util/CloudSaveClient.cpp`
- Modify: `unittest/TestCloudSave.cpp`
- Modify build lists (Core): same files and positions as Task 2, for `Util/CloudSaveClient.cpp/.h`

**Interfaces:**
- Consumes: `http::RequestManager::StartRequest` (Task 1), `g_DownloadManager` (`Core/Config.h`).
- Produces (namespace `CloudSave`):
  - `struct ApiError { int code = 0; std::string message; bool Unauthorized() const; }` — `code` 0 = transport failure
  - `struct CloudVersion { int version = 0; int64_t size = 0; std::string sha256, deviceName, uploadedAt; }`
  - `struct CloudSaveInfo { std::string saveId, gameId, title; int versionCount = 0; CloudVersion latest; }`
  - `enum class UploadOutcome { Ok, Conflict, Failed }`; `struct UploadResult { UploadOutcome outcome = UploadOutcome::Failed; int version = 0; CloudVersion latest; ApiError error; }`
  - Pure parsers: `bool ParseEnvelope(const std::string &body, ApiError *err)`; `bool ParseLogin(const std::string &body, std::string *jwt, ApiError *err)`; `bool ParseRegister(const std::string &body, std::string *deviceToken, ApiError *err)`; `bool ParseSaveList(const std::string &body, std::vector<CloudSaveInfo> *out, ApiError *err)`; `bool ParseVersionList(const std::string &body, std::vector<CloudVersion> *out, ApiError *err)`; `UploadResult ParseUpload(const std::string &body)`
  - `class Client` (callbacks run on the UI thread, never after `Client` is destroyed):
    - `Client(std::string baseUrl, std::string deviceToken)`
    - `void LoginAndRegister(const std::string &username, const std::string &password, const std::string &deviceName, std::function<void(bool ok, const std::string &deviceToken, const ApiError &err)> cb)`
    - `void ListSaves(const std::string &gameId, std::function<void(bool ok, const std::vector<CloudSaveInfo> &saves, const ApiError &err)> cb)`
    - `void ListVersions(const std::string &saveId, std::function<void(bool ok, const std::vector<CloudVersion> &versions, const ApiError &err)> cb)`
    - `void Upload(const std::string &saveId, const std::string &title, const std::string &zipData, int baseVersion, bool force, std::function<void(const UploadResult &result)> cb)`
    - `void Download(const std::string &saveId, int version, std::function<void(bool ok, const std::string &zipData, const ApiError &err)> cb)`

- [ ] **Step 1: Write the failing tests**

Add to `unittest/TestCloudSave.cpp`:

```cpp
#include "Core/Util/CloudSaveClient.h"

static bool TestCloudSaveEnvelope() {
	CloudSave::ApiError err;
	EXPECT_TRUE(CloudSave::ParseEnvelope(R"({"code":200,"data":[],"msg":""})", &err));
	// Error helper default: code 200 but no data key. Must be an error.
	EXPECT_FALSE(CloudSave::ParseEnvelope(R"({"code":200,"msg":"boom"})", &err));
	EXPECT_EQ_STR(err.message, std::string("boom"));
	// Unknown route: code is a string, message under "message".
	EXPECT_FALSE(CloudSave::ParseEnvelope(R"({"code":"404","message":"not found"})", &err));
	EXPECT_EQ_STR(err.message, std::string("not found"));
	EXPECT_FALSE(CloudSave::ParseEnvelope(R"({"code":6401,"message":"token is expired"})", &err));
	EXPECT_TRUE(err.Unauthorized());
	EXPECT_FALSE(CloudSave::ParseEnvelope("not json", &err));
	EXPECT_FALSE(CloudSave::ParseEnvelope("", &err));

	std::string jwt;
	EXPECT_TRUE(CloudSave::ParseLogin(R"({"code":200,"token":"abc.def","expire":"2026-11-05T10:00:00+08:00"})", &jwt, &err));
	EXPECT_EQ_STR(jwt, std::string("abc.def"));
	EXPECT_FALSE(CloudSave::ParseLogin(R"({"code":400,"message":"incorrect Username or Password"})", &jwt, &err));
	EXPECT_EQ_STR(err.message, std::string("incorrect Username or Password"));

	std::string token;
	EXPECT_TRUE(CloudSave::ParseRegister(R"({"code":200,"data":{"deviceId":3,"token":"dsv_xyz"},"msg":""})", &token, &err));
	EXPECT_EQ_STR(token, std::string("dsv_xyz"));
	return true;
}

static bool TestCloudSaveParseLists() {
	CloudSave::ApiError err;
	std::vector<CloudSave::CloudSaveInfo> saves;
	EXPECT_TRUE(CloudSave::ParseSaveList(R"({"code":200,"msg":"","data":[
		{"saveId":"ULUS10336DATA00","gameId":"ULUS10336","title":"Crisis Core","versionCount":2,
		 "latest":{"version":2,"size":1234,"sha256":"aaa","deviceName":"iPad","uploadedAt":"2026-10-06T10:00:00+08:00"}}]})", &saves, &err));
	EXPECT_EQ_INT((int)saves.size(), 1);
	EXPECT_EQ_STR(saves[0].saveId, std::string("ULUS10336DATA00"));
	EXPECT_EQ_INT(saves[0].latest.version, 2);
	EXPECT_EQ_STR(saves[0].latest.deviceName, std::string("iPad"));
	// null data is an empty list
	EXPECT_TRUE(CloudSave::ParseSaveList(R"({"code":200,"data":null,"msg":""})", &saves, &err));
	EXPECT_EQ_INT((int)saves.size(), 0);

	std::vector<CloudSave::CloudVersion> versions;
	EXPECT_TRUE(CloudSave::ParseVersionList(R"({"code":200,"msg":"","data":[
		{"version":2,"size":10,"sha256":"bbb","deviceName":"iPad","uploadedAt":"t2"},
		{"version":1,"size":9,"sha256":"aaa","deviceName":"Mac","uploadedAt":"t1"}]})", &versions, &err));
	EXPECT_EQ_INT((int)versions.size(), 2);
	EXPECT_EQ_INT(versions[1].version, 1);

	CloudSave::UploadResult ok = CloudSave::ParseUpload(R"({"code":200,"data":{"version":5},"msg":""})");
	EXPECT_TRUE(ok.outcome == CloudSave::UploadOutcome::Ok);
	EXPECT_EQ_INT(ok.version, 5);
	CloudSave::UploadResult conflict = CloudSave::ParseUpload(R"({"code":409,"msg":"conflict","data":{"latest":{"version":7,"sha256":"ccc","deviceName":"iPad","uploadedAt":"t7"}}})");
	EXPECT_TRUE(conflict.outcome == CloudSave::UploadOutcome::Conflict);
	EXPECT_EQ_INT(conflict.latest.version, 7);
	EXPECT_EQ_STR(conflict.latest.deviceName, std::string("iPad"));
	CloudSave::UploadResult tooBig = CloudSave::ParseUpload(R"({"code":413,"msg":"too large"})");
	EXPECT_TRUE(tooBig.outcome == CloudSave::UploadOutcome::Failed);
	EXPECT_EQ_INT(tooBig.error.code, 413);
	return true;
}

// A callback must not run after its Client is destroyed (the screen may close mid-request).
static bool TestCloudSaveClientAlive() {
	bool called = false;
	{
		CloudSave::Client client("http://127.0.0.1:1/api/v1", "dsv_test");  // port 1: connection refused
		client.ListSaves("ULUS10336", [&called](bool, const std::vector<CloudSave::CloudSaveInfo> &, const CloudSave::ApiError &) {
			called = true;
		});
	}
	// Drain the request manager like the main loop does.
	for (int i = 0; i < 200; i++) {
		g_DownloadManager.Update();
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	g_DownloadManager.CancelAll();
	EXPECT_FALSE(called);
	return true;
}
```

Add `#include <chrono>` and `#include "Core/Config.h"` at the top of the test file. In `TestCloudSave()`:

```cpp
	if (!TestCloudSaveEnvelope())
		return false;
	if (!TestCloudSaveParseLists())
		return false;
	if (!TestCloudSaveClientAlive())
		return false;
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build-unittest --target PPSSPPUnitTest -j11`
Expected: compile error `'Core/Util/CloudSaveClient.h' file not found`.

- [ ] **Step 3: Implement**

`Core/Util/CloudSaveClient.h`:

```cpp
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace CloudSave {

struct ApiError {
	int code = 0;  // 0 = network/transport failure; otherwise DHService's in-body code
	std::string message;
	bool Unauthorized() const { return code == 401 || code == 6401; }
};

struct CloudVersion {
	int version = 0;
	int64_t size = 0;
	std::string sha256;
	std::string deviceName;
	std::string uploadedAt;
};

struct CloudSaveInfo {
	std::string saveId;
	std::string gameId;
	std::string title;
	int versionCount = 0;
	CloudVersion latest;
};

enum class UploadOutcome { Ok, Conflict, Failed };

struct UploadResult {
	UploadOutcome outcome = UploadOutcome::Failed;
	int version = 0;       // Ok: the new version
	CloudVersion latest;   // Conflict: what the cloud has
	ApiError error;        // Failed
};

// Pure parsers for DHService's envelope ({"code":200,"data":...,"msg":""}, always HTTP 200).
bool ParseEnvelope(const std::string &body, ApiError *err);
bool ParseLogin(const std::string &body, std::string *jwt, ApiError *err);
bool ParseRegister(const std::string &body, std::string *deviceToken, ApiError *err);
bool ParseSaveList(const std::string &body, std::vector<CloudSaveInfo> *out, ApiError *err);
bool ParseVersionList(const std::string &body, std::vector<CloudVersion> *out, ApiError *err);
UploadResult ParseUpload(const std::string &body);

// Async DHService calls. Callbacks run on the thread that calls g_DownloadManager.Update() (the UI
// thread), and never after this object is destroyed.
class Client {
public:
	Client(std::string baseUrl, std::string deviceToken);
	~Client();

	void LoginAndRegister(const std::string &username, const std::string &password, const std::string &deviceName,
		std::function<void(bool ok, const std::string &deviceToken, const ApiError &err)> cb);
	void ListSaves(const std::string &gameId,
		std::function<void(bool ok, const std::vector<CloudSaveInfo> &saves, const ApiError &err)> cb);
	void ListVersions(const std::string &saveId,
		std::function<void(bool ok, const std::vector<CloudVersion> &versions, const ApiError &err)> cb);
	void Upload(const std::string &saveId, const std::string &title, const std::string &zipData, int baseVersion, bool force,
		std::function<void(const UploadResult &result)> cb);
	void Download(const std::string &saveId, int version,
		std::function<void(bool ok, const std::string &zipData, const ApiError &err)> cb);

private:
	std::string baseUrl_;
	std::string deviceToken_;
	std::shared_ptr<bool> alive_;
};

}  // namespace CloudSave
```

`Core/Util/CloudSaveClient.cpp`:

```cpp
#include "Common/Data/Format/JSONReader.h"
#include "Common/Data/Format/JSONWriter.h"
#include "Common/Net/HTTPRequest.h"
#include "Common/Net/URL.h"
#include "Common/StringUtils.h"
#include "Core/Config.h"
#include "Core/Util/CloudSaveClient.h"
#include "Core/Util/CloudSaveZip.h"

namespace CloudSave {

using Headers = std::vector<std::pair<std::string, std::string>>;

// Reads code/msg/message. Returns true on success (code 200 plus data or token).
static bool ReadEnvelope(const json::JsonGet &root, ApiError *err) {
	err->code = -1;
	err->message.clear();
	if (!root) {
		err->message = "Invalid response from server";
		return false;
	}
	const JsonNode *codeNode = root.get("code");
	if (codeNode && codeNode->value.getTag() == JSON_NUMBER) {
		err->code = (int)codeNode->value.toNumber();
	} else if (codeNode && codeNode->value.getTag() == JSON_STRING) {
		err->code = atoi(codeNode->value.toString());
	}
	err->message = root.getStringOr("msg", "");
	if (err->message.empty())
		err->message = root.getStringOr("message", "");
	const bool hasPayload = root.get("data") != nullptr || root.get("token") != nullptr;
	if (err->code == 200 && hasPayload) {
		err->message.clear();
		return true;
	}
	if (err->message.empty())
		err->message = StringFromFormat("Server error %d", err->code);
	return false;
}

bool ParseEnvelope(const std::string &body, ApiError *err) {
	json::JsonReader reader(body.c_str(), body.size());
	return ReadEnvelope(reader.root(), err);
}

bool ParseLogin(const std::string &body, std::string *jwt, ApiError *err) {
	json::JsonReader reader(body.c_str(), body.size());
	const json::JsonGet root = reader.root();
	if (!ReadEnvelope(root, err))
		return false;
	*jwt = root.getStringOr("token", "");
	return !jwt->empty();
}

bool ParseRegister(const std::string &body, std::string *deviceToken, ApiError *err) {
	json::JsonReader reader(body.c_str(), body.size());
	const json::JsonGet root = reader.root();
	if (!ReadEnvelope(root, err))
		return false;
	*deviceToken = root.getDict("data").getStringOr("token", "");
	return !deviceToken->empty();
}

static CloudVersion ReadVersion(const json::JsonGet &item) {
	CloudVersion v;
	v.version = item.getInt("version", 0);
	v.size = (int64_t)item.getFloat("size", 0.0);
	v.sha256 = item.getStringOr("sha256", "");
	v.deviceName = item.getStringOr("deviceName", "");
	v.uploadedAt = item.getStringOr("uploadedAt", "");
	return v;
}

bool ParseSaveList(const std::string &body, std::vector<CloudSaveInfo> *out, ApiError *err) {
	out->clear();
	json::JsonReader reader(body.c_str(), body.size());
	const json::JsonGet root = reader.root();
	if (!ReadEnvelope(root, err))
		return false;
	const JsonNode *data = root.getArray("data");
	if (!data)
		return true;  // null means empty
	for (const JsonNode *node : data->value) {
		const json::JsonGet item(node->value);
		CloudSaveInfo info;
		info.saveId = item.getStringOr("saveId", "");
		info.gameId = item.getStringOr("gameId", "");
		info.title = item.getStringOr("title", "");
		info.versionCount = item.getInt("versionCount", 0);
		info.latest = ReadVersion(item.getDict("latest"));
		if (IsValidSaveId(info.saveId))
			out->push_back(info);
	}
	return true;
}

bool ParseVersionList(const std::string &body, std::vector<CloudVersion> *out, ApiError *err) {
	out->clear();
	json::JsonReader reader(body.c_str(), body.size());
	const json::JsonGet root = reader.root();
	if (!ReadEnvelope(root, err))
		return false;
	const JsonNode *data = root.getArray("data");
	if (!data)
		return true;
	for (const JsonNode *node : data->value) {
		out->push_back(ReadVersion(json::JsonGet(node->value)));
	}
	return true;
}

UploadResult ParseUpload(const std::string &body) {
	UploadResult result;
	json::JsonReader reader(body.c_str(), body.size());
	const json::JsonGet root = reader.root();
	if (ReadEnvelope(root, &result.error)) {
		result.outcome = UploadOutcome::Ok;
		result.version = root.getDict("data").getInt("version", 0);
		return result;
	}
	if (result.error.code == 409) {
		result.outcome = UploadOutcome::Conflict;
		result.latest = ReadVersion(root.getDict("data").getDict("latest"));
	}
	return result;
}

Client::Client(std::string baseUrl, std::string deviceToken)
	: baseUrl_(std::move(baseUrl)), deviceToken_(std::move(deviceToken)), alive_(std::make_shared<bool>(true)) {}

Client::~Client() {
	*alive_ = false;
}

// Runs fn(body) on completion if the client still exists. Transport failures become ApiError code 0.
static void Send(const std::shared_ptr<bool> &alive, http::RequestMethod method, const std::string &url,
	const std::string &body, const std::string &mime, const Headers &headers,
	std::function<void(bool transportOk, const std::string &body)> fn) {
	std::weak_ptr<bool> weakAlive = alive;
	g_DownloadManager.StartRequest(method, url, body, mime, headers, http::RequestFlags::Default,
		[weakAlive, fn](http::Request &req) {
			auto aliveNow = weakAlive.lock();
			if (!aliveNow || !*aliveNow)
				return;
			std::string responseBody;
			req.buffer().TakeAll(&responseBody);
			fn(req.ResultCode() == 200, responseBody);
		}, "CloudSave");
}

static ApiError TransportError() {
	ApiError err;
	err.code = 0;
	err.message = "Could not reach the server";
	return err;
}

void Client::LoginAndRegister(const std::string &username, const std::string &password, const std::string &deviceName,
	std::function<void(bool, const std::string &, const ApiError &)> cb) {
	json::JsonWriter login;
	login.begin();
	login.writeString("username", username);
	login.writeString("password", password);
	login.end();
	const std::string baseUrl = baseUrl_;
	std::shared_ptr<bool> alive = alive_;
	Send(alive_, http::RequestMethod::POST, baseUrl + "/login", login.str(), "application/json", {},
		[alive, baseUrl, deviceName, cb](bool transportOk, const std::string &body) {
			if (!transportOk) {
				cb(false, "", TransportError());
				return;
			}
			std::string jwt;
			ApiError err;
			if (!ParseLogin(body, &jwt, &err)) {
				cb(false, "", err);
				return;
			}
			json::JsonWriter reg;
			reg.begin();
			reg.writeString("name", deviceName);
			reg.end();
			Send(alive, http::RequestMethod::POST, baseUrl + "/saves/devices", reg.str(), "application/json",
				{{"Authorization", "Bearer " + jwt}},
				[cb](bool transportOk, const std::string &body) {
					if (!transportOk) {
						cb(false, "", TransportError());
						return;
					}
					std::string token;
					ApiError err;
					bool ok = ParseRegister(body, &token, &err);
					cb(ok, token, err);
				});
		});
}

void Client::ListSaves(const std::string &gameId, std::function<void(bool, const std::vector<CloudSaveInfo> &, const ApiError &)> cb) {
	Send(alive_, http::RequestMethod::GET, baseUrl_ + "/saves?gameId=" + UriEncode(gameId), "", "",
		{{"Authorization", "Device " + deviceToken_}},
		[cb](bool transportOk, const std::string &body) {
			std::vector<CloudSaveInfo> saves;
			if (!transportOk) {
				cb(false, saves, TransportError());
				return;
			}
			ApiError err;
			bool ok = ParseSaveList(body, &saves, &err);
			cb(ok, saves, err);
		});
}

void Client::ListVersions(const std::string &saveId, std::function<void(bool, const std::vector<CloudVersion> &, const ApiError &)> cb) {
	Send(alive_, http::RequestMethod::GET, baseUrl_ + "/saves/" + saveId + "/versions", "", "",
		{{"Authorization", "Device " + deviceToken_}},
		[cb](bool transportOk, const std::string &body) {
			std::vector<CloudVersion> versions;
			if (!transportOk) {
				cb(false, versions, TransportError());
				return;
			}
			ApiError err;
			bool ok = ParseVersionList(body, &versions, &err);
			cb(ok, versions, err);
		});
}

void Client::Upload(const std::string &saveId, const std::string &title, const std::string &zipData, int baseVersion, bool force,
	std::function<void(const UploadResult &)> cb) {
	Headers headers = {
		{"Authorization", "Device " + deviceToken_},
		{"X-Save-Sha256", Sha256Hex(zipData)},
		{"X-Base-Version", StringFromFormat("%d", baseVersion)},
		{"X-Save-Title", UriEncode(title)},
	};
	std::string url = baseUrl_ + "/saves/" + saveId + "/versions" + (force ? "?force=1" : "");
	Send(alive_, http::RequestMethod::POST, url, zipData, "application/zip", headers,
		[cb](bool transportOk, const std::string &body) {
			if (!transportOk) {
				UploadResult result;
				result.error = TransportError();
				cb(result);
				return;
			}
			cb(ParseUpload(body));
		});
}

void Client::Download(const std::string &saveId, int version, std::function<void(bool, const std::string &, const ApiError &)> cb) {
	Send(alive_, http::RequestMethod::GET, baseUrl_ + "/saves/" + saveId + StringFromFormat("/versions/%d", version), "", "",
		{{"Authorization", "Device " + deviceToken_}},
		[cb](bool transportOk, const std::string &body) {
			if (!transportOk) {
				cb(false, "", TransportError());
				return;
			}
			// Success is the raw zip; errors are the JSON envelope.
			if (body.size() >= 2 && body[0] == 'P' && body[1] == 'K') {
				cb(true, body, ApiError());
				return;
			}
			ApiError err;
			ParseEnvelope(body, &err);
			cb(false, "", err);
		});
}

}  // namespace CloudSave
```

Add `Util/CloudSaveClient.cpp/.h` to the same Core build lists as Task 2.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cmake -S . -B build-unittest && cmake --build build-unittest --target PPSSPPUnitTest -j11 && build-unittest/PPSSPPUnitTest CloudSave`
Expected: PASS. If JSON accessors differ from what is written (`getTag`, `toNumber`, `toString`, `key`), check `ext/gason/gason.h` and adapt only the accessor call.

- [ ] **Step 5: Commit**

```bash
git diff --stat
git add Core/Util/CloudSaveClient.* Core/CMakeLists.txt Core/Core.vcxproj Core/Core.vcxproj.filters UWP/CoreUWP android/jni/Android.mk unittest/TestCloudSave.cpp
git commit -m "CloudSave: DHService client and response parsing"
```

---

### Task 5: Cloud Saves screen and GameScreen button

**Files:**
- Create: `UI/CloudSaveScreen.h`, `UI/CloudSaveScreen.cpp`
- Modify: `UI/GameScreen.cpp` (near the **Delete Save Data** button, ~line 635; add `#include "UI/CloudSaveScreen.h"`)
- Modify build lists (UI): `UI/CMakeLists.txt` (after `UploadScreen.cpp`), `UI/UI.vcxproj` (after the `UploadScreen.cpp` ClCompile and `UploadScreen.h` ClInclude lines), `UI/UI.vcxproj.filters` (copy the `UploadScreen` blocks with `<Filter>Screens</Filter>`), `UWP/UI_UWP/UI_UWP.vcxproj` + `.filters` (after the `..\..\UI\UploadScreen.*` lines), `android/jni/Android.mk` (after `$(SRC)/UI/UploadScreen.cpp \`)

**Interfaces:**
- Consumes: everything from Tasks 2–4; `NativeLoadSecret` / `NativeSaveSecret` (`Common/System/NativeApp.h`); `g_gameInfoCache->GetInfo(...)`, `GameInfo::GetSaveDataDirectories()`, `GameInfo::GetTitle()`, `GameInfo::id`; `UI::MessagePopupScreen`, `UI::ListPopupScreen`, `UI::PopupTextInputChoice`.
- Produces: `class CloudSaveScreen : public UISimpleBaseDialogScreen { public: explicit CloudSaveScreen(const Path &gamePath); }`

- [ ] **Step 1: Implement the screen**

`UI/CloudSaveScreen.h`:

```cpp
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Common/File/Path.h"
#include "Core/Util/CloudSaveClient.h"
#include "Core/Util/CloudSaveState.h"
#include "UI/BaseScreens.h"

// Upload/download this game's PSP/SAVEDATA folders to DHService. Opened from GameScreen.
class CloudSaveScreen : public UISimpleBaseDialogScreen {
public:
	explicit CloudSaveScreen(const Path &gamePath);
	void CreateDialogViews(UI::ViewGroup *parent) override;

protected:
	std::string_view GetTitle() const override;
	const char *tag() const override { return "CloudSave"; }

private:
	struct Row {
		std::string saveId;
		std::string title;
		bool hasLocal = false;
		std::string localSha256;
		std::string localZip;  // kept so upload sends exactly what was hashed
		CloudSave::CloudVersion cloudLatest;  // version 0 = not in the cloud
		CloudSave::SyncStatus status = CloudSave::SyncStatus::Synced;
	};

	void LoadLocal();
	void Refresh();
	void SetMessage(const std::string &message);
	void HandleError(const CloudSave::ApiError &err);
	void UploadRow(const Row &row, bool force);
	void UploadAll();
	void ChooseVersionAndDownload(const std::string &saveId);
	void DownloadVersion(const std::string &saveId, const CloudSave::CloudVersion &version);
	void LogOut();

	Path gamePath_;
	std::string gameId_;
	std::string gameTitle_;
	CloudSave::SyncState state_;
	std::string deviceToken_;
	std::unique_ptr<CloudSave::Client> client_;
	std::vector<Row> rows_;
	std::string message_;
	bool loading_ = false;

	// Login form fields.
	std::string username_;
	std::string password_;
	std::string deviceName_;
};
```

`UI/CloudSaveScreen.cpp`:

```cpp
#include <algorithm>

#include "Common/Data/Text/I18n.h"
#include "Common/StringUtils.h"
#include "Common/System/NativeApp.h"
#include "Common/System/System.h"
#include "Common/UI/PopupScreens.h"
#include "Common/UI/View.h"
#include "Common/UI/ViewGroup.h"
#include "Core/Util/CloudSaveZip.h"
#include "Core/Util/PathUtil.h"
#include "UI/CloudSaveScreen.h"
#include "UI/GameInfoCache.h"

static const char *SECRET_NAME = "cloudsave";

// Plain text: the UI font may not have arrow/check glyphs.
static const char *StatusLabel(CloudSave::SyncStatus status) {
	switch (status) {
	case CloudSave::SyncStatus::Synced: return "Synced";
	case CloudSave::SyncStatus::LocalNewer: return "Local is newer - upload";
	case CloudSave::SyncStatus::CloudNewer: return "Cloud is newer - download";
	case CloudSave::SyncStatus::Conflict: return "Both changed - conflict";
	case CloudSave::SyncStatus::LocalOnly: return "Not uploaded yet";
	case CloudSave::SyncStatus::CloudOnly: return "Only in the cloud";
	}
	return "";
}

CloudSaveScreen::CloudSaveScreen(const Path &gamePath)
	: UISimpleBaseDialogScreen(gamePath, SimpleDialogFlags::Default), gamePath_(gamePath) {
	state_.Load(CloudSave::SyncStateFile());
	if (state_.deviceName.empty())
		state_.deviceName = System_GetProperty(SYSPROP_NAME);
	username_ = state_.username;
	deviceName_ = state_.deviceName;
	deviceToken_ = NativeLoadSecret(SECRET_NAME);

	std::shared_ptr<GameInfo> info = g_gameInfoCache->GetInfo(nullptr, gamePath_, GameInfoFlags::PARAM_SFO);
	if (info && info->Ready(GameInfoFlags::PARAM_SFO)) {
		gameId_ = info->id;
		gameTitle_ = info->GetTitle();
	}
	if (!deviceToken_.empty())
		Refresh();
}

std::string_view CloudSaveScreen::GetTitle() const {
	return "Cloud Saves";
}

void CloudSaveScreen::SetMessage(const std::string &message) {
	message_ = message;
	RecreateViews();
}

void CloudSaveScreen::HandleError(const CloudSave::ApiError &err) {
	if (err.Unauthorized()) {
		LogOut();
		SetMessage("This device was signed out. Please log in again.");
		return;
	}
	SetMessage(err.message);
}

void CloudSaveScreen::LogOut() {
	NativeSaveSecret(SECRET_NAME, "");
	deviceToken_.clear();
	client_.reset();
	rows_.clear();
}

// Hashes every local save folder of this game.
void CloudSaveScreen::LoadLocal() {
	rows_.clear();
	std::shared_ptr<GameInfo> info = g_gameInfoCache->GetInfo(nullptr, gamePath_, GameInfoFlags::PARAM_SFO);
	if (!info || !info->Ready(GameInfoFlags::PARAM_SFO))
		return;
	for (const Path &dir : info->GetSaveDataDirectories()) {
		Row row;
		row.saveId = dir.GetFilename();
		if (!CloudSave::IsValidSaveId(row.saveId))
			continue;  // e.g. a leftover ".bak"
		row.title = gameTitle_;
		if (!CloudSave::ZipSaveFolder(dir, &row.localZip))
			continue;
		row.hasLocal = true;
		row.localSha256 = CloudSave::Sha256Hex(row.localZip);
		rows_.push_back(row);
	}
}

void CloudSaveScreen::Refresh() {
	if (gameId_.empty()) {
		SetMessage("Couldn't read this game's ID.");
		return;
	}
	LoadLocal();
	client_ = std::make_unique<CloudSave::Client>(state_.serverUrl, deviceToken_);
	loading_ = true;
	message_.clear();
	client_->ListSaves(gameId_, [this](bool ok, const std::vector<CloudSave::CloudSaveInfo> &saves, const CloudSave::ApiError &err) {
		loading_ = false;
		if (!ok) {
			HandleError(err);
			return;
		}
		for (const auto &cloud : saves) {
			auto it = std::find_if(rows_.begin(), rows_.end(), [&](const Row &r) { return r.saveId == cloud.saveId; });
			if (it == rows_.end()) {
				Row row;
				row.saveId = cloud.saveId;
				row.title = cloud.title;
				rows_.push_back(row);
				it = rows_.end() - 1;
			}
			it->cloudLatest = cloud.latest;
		}
		for (Row &row : rows_) {
			row.status = CloudSave::ComputeStatus(row.hasLocal, row.localSha256, state_.bases[row.saveId], row.cloudLatest.version, row.cloudLatest.sha256);
		}
		std::sort(rows_.begin(), rows_.end(), [](const Row &a, const Row &b) { return a.saveId < b.saveId; });
		RecreateViews();
	});
	RecreateViews();
}

void CloudSaveScreen::UploadRow(const Row &row, bool force) {
	const std::string saveId = row.saveId;
	const std::string sha = row.localSha256;
	client_->Upload(saveId, row.title, row.localZip, state_.bases[saveId].version, force, [this, saveId, sha, row](const CloudSave::UploadResult &result) {
		switch (result.outcome) {
		case CloudSave::UploadOutcome::Ok:
			state_.bases[saveId] = {result.version, sha};
			state_.Save(CloudSave::SyncStateFile());
			Refresh();
			break;
		case CloudSave::UploadOutcome::Conflict: {
			std::string prompt = StringFromFormat("The cloud has a newer version of %s from %s (%s).\nUpload yours anyway?",
				saveId.c_str(), result.latest.deviceName.c_str(), result.latest.uploadedAt.c_str());
			screenManager()->push(new UI::MessagePopupScreen("Conflict", prompt, "Upload anyway", "Cancel", [this, row](bool yes) {
				if (yes)
					UploadRow(row, true);
			}));
			break;
		}
		case CloudSave::UploadOutcome::Failed:
			HandleError(result.error);
			break;
		}
	});
}

void CloudSaveScreen::UploadAll() {
	for (const Row &row : rows_) {
		if (!row.hasLocal)
			continue;
		if (row.status == CloudSave::SyncStatus::LocalNewer || row.status == CloudSave::SyncStatus::LocalOnly) {
			UploadRow(row, false);
		} else if (row.status == CloudSave::SyncStatus::Conflict) {
			UploadRow(row, false);  // the server answers 409 and the conflict prompt asks
		}
	}
}

void CloudSaveScreen::ChooseVersionAndDownload(const std::string &saveId) {
	client_->ListVersions(saveId, [this, saveId](bool ok, const std::vector<CloudSave::CloudVersion> &versions, const CloudSave::ApiError &err) {
		if (!ok) {
			HandleError(err);
			return;
		}
		if (versions.empty()) {
			SetMessage("No cloud versions for " + saveId);
			return;
		}
		std::vector<std::string> items;
		for (const auto &v : versions) {
			items.push_back(StringFromFormat("v%d · %s · %s", v.version, v.deviceName.c_str(), v.uploadedAt.c_str()));
		}
		screenManager()->push(new UI::ListPopupScreen("Download which version?", items, 0, [this, saveId, versions](int index) {
			if (index < 0 || index >= (int)versions.size())
				return;
			const CloudSave::CloudVersion chosen = versions[index];
			auto it = std::find_if(rows_.begin(), rows_.end(), [&](const Row &r) { return r.saveId == saveId; });
			const bool localChanged = it != rows_.end() &&
				(it->status == CloudSave::SyncStatus::LocalNewer || it->status == CloudSave::SyncStatus::Conflict);
			if (localChanged) {
				screenManager()->push(new UI::MessagePopupScreen("Overwrite local save?",
					"This device has changes that aren't in the cloud. Replace them with the downloaded version?",
					"Replace", "Cancel", [this, saveId, chosen](bool yes) {
						if (yes)
							DownloadVersion(saveId, chosen);
					}));
			} else {
				DownloadVersion(saveId, chosen);
			}
		}));
	});
}

void CloudSaveScreen::DownloadVersion(const std::string &saveId, const CloudSave::CloudVersion &version) {
	client_->Download(saveId, version.version, [this, saveId, version](bool ok, const std::string &zipData, const CloudSave::ApiError &err) {
		if (!ok) {
			HandleError(err);
			return;
		}
		const std::string sha = CloudSave::Sha256Hex(zipData);
		if (sha != version.sha256) {
			SetMessage("Download of " + saveId + " was corrupted. Nothing was changed.");
			return;
		}
		std::string error;
		if (!CloudSave::ExtractSaveZip(zipData, GetSysDirectory(DIRECTORY_SAVEDATA), saveId, &error)) {
			SetMessage(error);
			return;
		}
		state_.bases[saveId] = {version.version, sha};
		state_.Save(CloudSave::SyncStateFile());
		g_gameInfoCache->Clear();  // save sizes/icons changed
		Refresh();
	});
}

void CloudSaveScreen::CreateDialogViews(UI::ViewGroup *parent) {
	using namespace UI;
	auto di = GetI18NCategory(I18NCat::DIALOG);

	if (!message_.empty()) {
		parent->Add(new TextView(message_, ALIGN_LEFT | FLAG_WRAP_TEXT, false, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT)));
	}

	if (deviceToken_.empty()) {
		parent->Add(new ItemHeader("Log in to DHService"));
		parent->Add(new PopupTextInputChoice(GetRequesterToken(), &username_, di->T("Username"), "", 128, screenManager()));
		parent->Add(new PopupTextInputChoice(GetRequesterToken(), &password_, di->T("Password"), "", 128, screenManager()))->SetPasswordDisplay();
		parent->Add(new PopupTextInputChoice(GetRequesterToken(), &deviceName_, "Device name", "", 64, screenManager()));
		Choice *login = parent->Add(new Choice(di->T("Log in")));
		login->SetEnabledFunc([this] { return !username_.empty() && !password_.empty() && !deviceName_.empty() && !loading_; });
		login->OnClick.Add([this](EventParams &) {
			loading_ = true;
			client_ = std::make_unique<CloudSave::Client>(state_.serverUrl, "");
			client_->LoginAndRegister(username_, password_, deviceName_, [this](bool ok, const std::string &token, const CloudSave::ApiError &err) {
				loading_ = false;
				std::fill(password_.begin(), password_.end(), '\0');
				password_.clear();
				if (!ok) {
					SetMessage(err.message);
					return;
				}
				deviceToken_ = token;
				NativeSaveSecret(SECRET_NAME, token);
				state_.username = username_;
				state_.deviceName = deviceName_;
				state_.Save(CloudSave::SyncStateFile());
				Refresh();
			});
		});
		return;
	}

	parent->Add(new ItemHeader(gameTitle_ + " (" + gameId_ + ")"));
	if (loading_) {
		parent->Add(new TextView("Loading…", ALIGN_LEFT, false));
		return;
	}

	Choice *uploadAll = parent->Add(new Choice("Upload all"));
	uploadAll->SetEnabledFunc([this] {
		return std::any_of(rows_.begin(), rows_.end(), [](const Row &r) {
			return r.status == CloudSave::SyncStatus::LocalNewer || r.status == CloudSave::SyncStatus::LocalOnly || r.status == CloudSave::SyncStatus::Conflict;
		});
	});
	uploadAll->OnClick.Add([this](EventParams &) { UploadAll(); });

	if (rows_.empty()) {
		parent->Add(new TextView("No saves for this game, here or in the cloud.", ALIGN_LEFT, false));
	}
	for (const Row &row : rows_) {
		parent->Add(new ItemHeader(row.saveId));
		parent->Add(new TextView(StatusLabel(row.status), ALIGN_LEFT, false));
		if (row.hasLocal) {
			const std::string saveId = row.saveId;
			parent->Add(new Choice("Upload"))->OnClick.Add([this, saveId](EventParams &) {
				auto it = std::find_if(rows_.begin(), rows_.end(), [&](const Row &r) { return r.saveId == saveId; });
				if (it != rows_.end())
					UploadRow(*it, false);
			});
		}
		if (row.cloudLatest.version > 0) {
			const std::string saveId = row.saveId;
			parent->Add(new Choice("Download…"))->OnClick.Add([this, saveId](EventParams &) {
				ChooseVersionAndDownload(saveId);
			});
		}
	}

	parent->Add(new ItemHeader("Account"));
	parent->Add(new TextView("Device: " + state_.deviceName, ALIGN_LEFT, false));
	parent->Add(new Choice(di->T("Log out")))->OnClick.Add([this](EventParams &) {
		LogOut();
		RecreateViews();
	});
}
```

Notes for the implementer:
- `UISimpleBaseDialogScreen`, `SimpleDialogFlags`, `GetRequesterToken()` are used exactly like `UI/UploadScreen.cpp`; `CreateDialogViews`/`GetTitle`/`tag` match its overrides. If a signature differs (e.g. `GetTitle` return type), copy it from `UI/UploadScreen.h`.
- `FLAG_WRAP_TEXT` and `TextView`'s constructor: match an existing `TextView` construction in `UI/GameScreen.cpp` (e.g. line ~413) if this one doesn't compile.
- `g_gameInfoCache->Clear()` exists in `UI/GameInfoCache.h` (line ~222).

- [ ] **Step 2: Add the GameScreen button**

In `UI/GameScreen.cpp`, add `#include "UI/CloudSaveScreen.h"` with the other `UI/` includes, and right after the `Delete Save Data` block (`if (info_->saveDataSize) { ... }`):

```cpp
	if (!inGame_ && (knownFlags_ & GameInfoFlags::PARAM_SFO)) {
		Choice *btnCloudSaves = parent->Add(new Choice("Cloud Saves", ImageID("I_FOLDER")));
		btnCloudSaves->OnClick.Add([this](UI::EventParams &) {
			screenManager()->push(new CloudSaveScreen(gamePath_));
		});
	}
```

Add the two new files to the UI build lists named in **Files**.

- [ ] **Step 3: Build Mac and iOS**

Run: `./b.sh --release && ./build-ios.sh`
Expected: both succeed. `git diff --stat` shows only the intended lines in `UI/GameScreen.cpp` and the build lists.

- [ ] **Step 4: Smoke test without a backend**

Run the Mac app, open any game's page → **Cloud Saves**. Expected: the login form. With the backend not yet deployed, **Log in** shows "Could not reach the server" (or the server's error text), and the app does not crash. Close the screen while a login is in flight; expected: no crash.

- [ ] **Step 5: Commit**

```bash
git diff --stat
git add UI/CloudSaveScreen.* UI/GameScreen.cpp UI/CMakeLists.txt UI/UI.vcxproj UI/UI.vcxproj.filters UWP/UI_UWP android/jni/Android.mk
git commit -m "CloudSave: per-game Cloud Saves screen"
```

---

### Task 6: Backend handoff to the dhservice session

Runs in parallel with Tasks 1–5. The backend lives in the DHService repo and is built by that session, following its own conventions.

- [ ] **Step 1: Send the contract**

SendMessage to `dhservice` with: the path of this repo's spec (`/Users/darrenhuang/Project/DarrenHuang/Emulator/ppsspp/docs/superpowers/specs/2026-10-06-cloud-saves-design.md`), asking it to implement the **API contract**, **Response envelope** and **Backend** sections exactly, and to:
- write the 409 response directly in the upload handler as HTTP 200 `{"code":409,"msg":"conflict","data":{"latest":{...}}}`;
- initialize every list slice so empty lists are `[]`;
- answer the download with raw `application/zip` bytes on success;
- run its curl flow on dev (`127.0.0.1:9001`): register → upload → 409 → force → list → download (hash matches) → 11th upload prunes the oldest → revoked token gets code 401;
- reply with: dev is ready, and later, prod is deployed. It must not deploy to prod without the user's approval.

- [ ] **Step 2: Wait for "dev is ready"**

Do not start Task 7 before it.

---

### Task 7: End-to-end check

- [ ] **Step 1: Mac against dev**

Point the Mac build at dev by editing `~/.config/ppsspp/PSP/SYSTEM/cloudsave.json` → `"serverUrl": "http://127.0.0.1:9001/api/v1"` (create it with that one key if it doesn't exist). Open a game with saves → Cloud Saves → log in. Expected: rows show `Not uploaded yet`.

- [ ] **Step 2: Upload, change, conflict, download**

1. **Upload all** → rows show `Synced`; `curl` the list endpoint and see version 1.
2. Play and save in-game (or edit a byte in the save's `DATA.BIN` copy) → row shows `Local is newer - upload`.
3. Upload a different zip for the same save with curl (`X-Base-Version: 1`) to simulate another device → refresh → `Both changed - conflict`. Tap **Upload** → conflict prompt appears; **Cancel** leaves both untouched.
4. **Download…** → choose v1 → overwrite prompt → **Replace** → folder contents equal v1; boot the game and confirm it loads the save.

- [ ] **Step 3: iPhone and iPad against prod**

After the dhservice session reports prod deployed (with the user's approval), reset `serverUrl` to the default (delete the key), rebuild with `./build-ios.sh`, install to DarreniPhone (`00008140-000479C234E2801C`) and DarrenDeriPad (`00008112-001944683ADA201E`) with `xcrun devicectl device install app`. Upload on Mac → download on iPhone → the game loads it. Repeat iPhone → iPad.

- [ ] **Step 4: Report**

Report results to the user with the exact outcomes of each step, including anything that failed.
