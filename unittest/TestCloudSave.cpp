#include <string>
#include <thread>

#include "Common/Buffer.h"
#include "Common/Net/HTTPClient.h"
#include "Common/Net/HTTPServer.h"
#include "Common/Net/Sinks.h"
#include "ext/libzip/zip.h"
#include "Common/File/FileUtil.h"
#include "Common/File/Path.h"
#include "Core/Util/CloudSaveZip.h"
#include "Core/Util/CloudSaveState.h"

#include "UnitTest.h"

// An in-process server echoes the X-Cloud-Test request header, proving extraHeaders reach the wire.
static bool TestCloudSaveHttpHeaders() {
	http::Server server(new NewThreadExecutor());
	std::string received;
	server.RegisterHandler("/echo", [&received](const http::ServerRequest &request) {
		request.GetHeader("x-cloud-test", &received);
		request.WriteHttpResponseHeader("1.0", 200, received.size(), "text/plain");
		request.Out()->Push(received);
	});
	EXPECT_TRUE(server.Listen(0, "unittest", net::DNSType::IPV4));
	const int port = server.Port();
	std::thread serveThread([&server] { server.RunSlice(5.0); });

	bool cancelled = false;
	http::Client client(nullptr);
	EXPECT_TRUE(client.Resolve("127.0.0.1", port));
	EXPECT_TRUE(client.Connect(2, 5.0, &cancelled));
	http::RequestParams req("/echo", "*/*");
	req.extraHeaders = "X-Cloud-Test: hello-header\r\n";
	Buffer output;
	net::RequestProgress progress(&cancelled);  // Client::GET requires one.
	int code = client.GET(req, &output, &progress);
	serveThread.join();

	EXPECT_EQ_INT(code, 200);
	std::string body;
	output.TakeAll(&body);
	EXPECT_EQ_STR(body, std::string("hello-header"));
	return true;
}

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
	Path root = File::GetCurDirectory() / "unittest_cloudsave_zip";
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
	Path root = File::GetCurDirectory() / "unittest_cloudsave_extract";
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

// Builds a zip with a single entry of the given name and returns its bytes.
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
	Path root = File::GetCurDirectory() / "unittest_cloudsave_unsafe";
	File::DeleteDirRecursively(root);
	Path dest = root / "SAVEDATA";
	WriteFile(dest / "ULUS10336DATA00" / "DATA.BIN", "keep-me");
	std::string zip = MakeZipWithEntry(root / "bad.zip", "../evil.txt");
	EXPECT_FALSE(zip.empty());
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
	Path root = File::GetCurDirectory() / "unittest_cloudsave_notzip";
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
	Path file = File::GetCurDirectory() / "unittest_cloudsave_state.json";
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

bool TestCloudSave() {
	if (!TestCloudSaveHttpHeaders())
		return false;
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
	if (!TestCloudSaveStatus())
		return false;
	if (!TestCloudSaveStateFile())
		return false;
	return true;
}
