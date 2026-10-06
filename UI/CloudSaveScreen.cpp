#include <algorithm>

#include "Common/Data/Text/I18n.h"
#include "Common/File/DirListing.h"
#include "Common/File/FileUtil.h"
#include "Common/StringUtils.h"
#include "Common/System/NativeApp.h"
#include "Common/System/System.h"
#include "Common/UI/PopupScreens.h"
#include "Common/UI/ScreenManager.h"
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
	if (!deviceToken_.empty())
		client_ = std::make_unique<CloudSave::Client>(state_.serverUrl, deviceToken_);

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

// Hashes every local save folder of this game. Reads the disk directly rather than GameInfoCache,
// which may not be ready again yet after a download clears it.
void CloudSaveScreen::LoadLocal() {
	rows_.clear();
	std::vector<File::FileInfo> dirs;
	File::GetFilesInDir(GetSysDirectory(DIRECTORY_SAVEDATA), &dirs, nullptr, 0, gameId_);
	for (const auto &dir : dirs) {
		if (!dir.isDirectory || !CloudSave::IsValidSaveId(dir.name))
			continue;  // e.g. a leftover ".bak" or ".cloudtmp"
		Row row;
		row.saveId = dir.name;
		row.title = gameTitle_;
		row.hasLocal = true;
		if (CloudSave::ZipSaveFolder(dir.fullName, &row.localZip)) {
			row.localSha256 = CloudSave::Sha256Hex(row.localZip);
		} else {
			row.localReadable = false;
		}
		rows_.push_back(row);
	}
}

bool CloudSaveScreen::DownloadNeedsConfirm(const std::string &saveId, const std::string &chosenSha256) {
	const Path dir = GetSysDirectory(DIRECTORY_SAVEDATA) / saveId;
	const bool exists = File::Exists(dir);
	std::string zip;
	const bool readable = exists && CloudSave::ZipSaveFolder(dir, &zip);
	const std::string sha = readable ? CloudSave::Sha256Hex(zip) : "";
	return CloudSave::NeedsOverwriteConfirm(exists, readable, sha, state_.bases[saveId], chosenSha256);
}

void CloudSaveScreen::Refresh() {
	if (gameId_.empty()) {
		SetMessage("Couldn't read this game's ID.");
		return;
	}
	if (!client_)
		return;
	LoadLocal();
	loading_ = true;
	message_.clear();
	const int generation = ++refreshGeneration_;
	client_->ListSaves(gameId_, [this, generation](bool ok, const std::vector<CloudSave::CloudSaveInfo> &saves, const CloudSave::ApiError &err) {
		if (generation != refreshGeneration_)
			return;  // a newer Refresh() is in flight
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
	if (!client_ || !row.hasLocal || !row.localReadable)
		return;  // e.g. signed out while a conflict prompt was open
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
		if (!row.hasLocal || !row.localReadable)
			continue;
		if (row.status == CloudSave::SyncStatus::LocalNewer || row.status == CloudSave::SyncStatus::LocalOnly) {
			UploadRow(row, false);
		} else if (row.status == CloudSave::SyncStatus::Conflict) {
			UploadRow(row, false);  // the server answers 409 and the conflict prompt asks
		}
	}
}

void CloudSaveScreen::ChooseVersionAndDownload(const std::string &saveId) {
	if (!client_)
		return;
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
			items.push_back(StringFromFormat("v%d - %s - %s", v.version, v.deviceName.c_str(), v.uploadedAt.c_str()));
		}
		screenManager()->push(new UI::ListPopupScreen("Download which version?", items, 0, [this, saveId, versions](int index) {
			if (index < 0 || index >= (int)versions.size())
				return;
			const CloudSave::CloudVersion chosen = versions[index];
			// Decide from what is on disk now, not from a row status that may be stale.
			if (DownloadNeedsConfirm(saveId, chosen.sha256)) {
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
	if (!client_)
		return;
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
				client_ = std::make_unique<CloudSave::Client>(state_.serverUrl, deviceToken_);
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
		parent->Add(new TextView("Loading...", ALIGN_LEFT, false));
		return;
	}

	Choice *uploadAll = parent->Add(new Choice("Upload all"));
	uploadAll->SetEnabledFunc([this] {
		return std::any_of(rows_.begin(), rows_.end(), [](const Row &r) {
			return r.localReadable && (r.status == CloudSave::SyncStatus::LocalNewer || r.status == CloudSave::SyncStatus::LocalOnly || r.status == CloudSave::SyncStatus::Conflict);
		});
	});
	uploadAll->OnClick.Add([this](EventParams &) { UploadAll(); });

	if (rows_.empty()) {
		parent->Add(new TextView("No saves for this game, here or in the cloud.", ALIGN_LEFT, false));
	}
	for (const Row &row : rows_) {
		parent->Add(new ItemHeader(row.saveId));
		parent->Add(new TextView(row.localReadable ? StatusLabel(row.status) : "Can't read the local save", ALIGN_LEFT, false));
		if (row.hasLocal && row.localReadable) {
			const std::string saveId = row.saveId;
			parent->Add(new Choice("Upload"))->OnClick.Add([this, saveId](EventParams &) {
				auto it = std::find_if(rows_.begin(), rows_.end(), [&](const Row &r) { return r.saveId == saveId; });
				if (it != rows_.end())
					UploadRow(*it, false);
			});
		}
		if (row.cloudLatest.version > 0) {
			const std::string saveId = row.saveId;
			parent->Add(new Choice("Download..."))->OnClick.Add([this, saveId](EventParams &) {
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
