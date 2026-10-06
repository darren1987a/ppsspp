#include <cstdlib>
#include <utility>

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
