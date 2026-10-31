// SPDX-FileCopyrightText: Copyright 2026 Eglantine Hub contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include <QIcon>
#include <QJsonObject>
#include <QMainWindow>
#include <QString>
#include <QStringList>
#include <QThread>

#include "hub_core.h"

class QCheckBox;
class QComboBox;
class QFormLayout;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QStackedWidget;

namespace HubUi {

struct GameRecord {
    Hub::GameFile file;
    QString key;           ///< Stable ID for stored settings: title ID if known, else the path.
    QString display_name;
    bool favorite = false;
    qint64 last_played = 0; ///< Seconds since epoch, 0 if never.
    qint64 hub_seconds = 0; ///< Time played when started from the Hub.
    QString custom_cover;
    bool custom_options = false;
    Hub::LaunchOptions options;
    QIcon icon;
};

struct HubSettings {
    QString eglantine_path;
    QStringList folders;
    bool recursive = true;
    bool show_extras = false; ///< Show updates and DLC in the grid.
    int view = 0;             ///< 0 all, 1 favorites, 2 recently played.
    Hub::LaunchOptions defaults;
};

class HubWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit HubWindow(QWidget* parent = nullptr);
    ~HubWindow() override;

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void BuildUi();
    QWidget* BuildDetailsPanel();
    void LoadState();
    void SaveState() const;
    QString StateFilePath() const;
    QString CoversDir() const;

    void StartScan();
    void ScanFinished(std::vector<Hub::GameFile> files);
    void RefreshGrid();
    void ShowDetails();
    void SaveDetailsOptions();
    GameRecord* SelectedRecord();
    QIcon MakeIcon(const GameRecord& record) const;

    void AddFolder(const QString& folder);
    void OpenSettings();
    void Launch(GameRecord* record);
    void LaunchEmulatorOnly();
    void ToggleFavorite();
    void ChooseCover();
    void ResetCover();
    void ShowInFolder();
    void ClearShaderCache();
    void OpenDataFolder();

    QString ResolveEglantinePath() const;
    QString EmulatorDataDir() const;
    void SetStatus(const QString& text);

    HubSettings settings_;
    std::vector<std::unique_ptr<GameRecord>> records_;
    QJsonObject stored_games_; ///< Per-game state, kept even for games not currently found.
    Hub::CpuTopology cpu_;

    QThread* scan_thread_ = nullptr;
    std::atomic_bool cancel_scan_{false};
    bool rescan_pending_ = false;
    bool loading_details_ = false;

    // Widgets
    QLineEdit* search_ = nullptr;
    QComboBox* view_combo_ = nullptr;
    QCheckBox* extras_check_ = nullptr;
    QStackedWidget* grid_stack_ = nullptr;
    QListWidget* grid_ = nullptr;
    QLabel* empty_label_ = nullptr;
    QStackedWidget* details_stack_ = nullptr;
    QLabel* cover_label_ = nullptr;
    QLabel* title_label_ = nullptr;
    QLabel* kind_value_ = nullptr;
    QLabel* format_value_ = nullptr;
    QLabel* title_id_value_ = nullptr;
    QLabel* version_value_ = nullptr;
    QLabel* size_value_ = nullptr;
    QLabel* integrity_value_ = nullptr;
    QLabel* location_value_ = nullptr;
    QLabel* played_value_ = nullptr;
    QPushButton* play_button_ = nullptr;
    QPushButton* favorite_button_ = nullptr;
    QPushButton* shader_button_ = nullptr;
    QCheckBox* custom_check_ = nullptr;
    QGroupBox* options_box_ = nullptr;
    QCheckBox* opt_fullscreen_ = nullptr;
    QCheckBox* opt_priority_ = nullptr;
    QCheckBox* opt_pcores_ = nullptr;
    QCheckBox* opt_throttle_ = nullptr;
    QLineEdit* opt_user_ = nullptr;
    QLineEdit* opt_args_ = nullptr;
    QLabel* status_label_ = nullptr;
};

} // namespace HubUi
