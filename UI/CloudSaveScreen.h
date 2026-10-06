#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Common/File/Path.h"
#include "Core/Util/CloudSaveClient.h"
#include "Core/Util/CloudSaveState.h"
#include "UI/SimpleDialogScreen.h"

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
