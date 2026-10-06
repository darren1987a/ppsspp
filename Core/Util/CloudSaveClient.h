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
