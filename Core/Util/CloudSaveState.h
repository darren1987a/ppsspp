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

// True if a download must ask before replacing the save currently on disk: it exists and holds
// changes that are neither the last synced version nor the version being downloaded, or it can't
// be read. Decided from disk at confirm time, never from a cached status.
bool NeedsOverwriteConfirm(bool localExists, bool localReadable, const std::string &localSha256, const SyncBase &base, const std::string &chosenSha256);

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
