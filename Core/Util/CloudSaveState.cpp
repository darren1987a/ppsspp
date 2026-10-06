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
