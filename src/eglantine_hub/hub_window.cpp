// SPDX-FileCopyrightText: Copyright 2026 Eglantine Hub contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "hub_window.h"

#include <algorithm>
#include <set>
#include <thread>

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace fs = std::filesystem;

namespace HubUi {

namespace {

constexpr int kTileSize = 168;
constexpr int kCoverSize = 256;

QString ToQString(const fs::path& path) {
    return QString::fromStdU16String(path.u16string());
}

fs::path ToPath(const QString& text) {
    return fs::path(text.toStdU16String());
}

QString JoinArguments(const std::vector<std::string>& args) {
    QStringList parts;
    for (const std::string& arg : args) {
        const QString q = QString::fromStdString(arg);
        parts << (q.contains(QLatin1Char(' ')) ? QStringLiteral("\"%1\"").arg(q) : q);
    }
    return parts.join(QLatin1Char(' '));
}

std::vector<std::string> SplitArguments(const QString& text) {
    std::vector<std::string> out;
    for (const QString& part : QProcess::splitCommand(text)) {
        out.push_back(part.toStdString());
    }
    return out;
}

QJsonObject OptionsToJson(const Hub::LaunchOptions& o) {
    QJsonArray extra;
    for (const std::string& arg : o.extra_args) {
        extra.append(QString::fromStdString(arg));
    }
    return QJsonObject{
        {QStringLiteral("fullscreen"), o.fullscreen},
        {QStringLiteral("high_priority"), o.high_priority},
        {QStringLiteral("performance_cores_only"), o.performance_cores_only},
        {QStringLiteral("disable_power_throttling"), o.disable_power_throttling},
        {QStringLiteral("user"), QString::fromStdString(o.user)},
        {QStringLiteral("extra_args"), extra},
    };
}

Hub::LaunchOptions OptionsFromJson(const QJsonObject& json, const Hub::LaunchOptions& fallback) {
    Hub::LaunchOptions o = fallback;
    o.fullscreen = json.value(QStringLiteral("fullscreen")).toBool(o.fullscreen);
    o.high_priority = json.value(QStringLiteral("high_priority")).toBool(o.high_priority);
    o.performance_cores_only =
        json.value(QStringLiteral("performance_cores_only")).toBool(o.performance_cores_only);
    o.disable_power_throttling =
        json.value(QStringLiteral("disable_power_throttling")).toBool(o.disable_power_throttling);
    o.user = json.value(QStringLiteral("user")).toString(QString::fromStdString(o.user)).toStdString();
    if (json.contains(QStringLiteral("extra_args"))) {
        o.extra_args.clear();
        for (const QJsonValue& value : json.value(QStringLiteral("extra_args")).toArray()) {
            o.extra_args.push_back(value.toString().toStdString());
        }
    }
    return o;
}

QString RecordKey(const Hub::GameFile& file) {
    if (file.title_id && file.kind == Hub::ContentKind::Base) {
        return QStringLiteral("tid:") + QString::fromStdString(Hub::FormatTitleId(*file.title_id));
    }
    return QStringLiteral("path:") + QDir::cleanPath(ToQString(file.path));
}

QString FormatPlayed(qint64 last_played, qint64 hub_seconds) {
    QString text = last_played == 0
                       ? QObject::tr("Never from the Hub")
                       : QDateTime::fromSecsSinceEpoch(last_played).toString(QStringLiteral("d MMM yyyy, HH:mm"));
    if (hub_seconds >= 60) {
        const qint64 minutes = hub_seconds / 60;
        text += QObject::tr("  ·  %1 h %2 min via Hub").arg(minutes / 60).arg(minutes % 60);
    }
    return text;
}

QString CpuSummary(const Hub::CpuTopology& cpu) {
    if (cpu.hybrid) {
        return QObject::tr("%1 threads: %2 on performance cores, %3 on efficiency cores")
            .arg(cpu.logical_processors)
            .arg(Hub::CountBits(cpu.performance_mask))
            .arg(Hub::CountBits(cpu.efficiency_mask));
    }
    return QObject::tr("%1 threads, all one core type (P-core pinning not needed)")
        .arg(cpu.logical_processors);
}

/// Checkboxes and fields for one set of launch options, shared by the details panel and the
/// settings dialog.
struct OptionWidgets {
    QCheckBox* fullscreen;
    QCheckBox* priority;
    QCheckBox* pcores;
    QCheckBox* throttle;
    QLineEdit* user;
    QLineEdit* args;
};

OptionWidgets MakeOptionWidgets(QWidget* parent, QFormLayout* form, const Hub::CpuTopology& cpu) {
    OptionWidgets w{};
    w.fullscreen = new QCheckBox(QObject::tr("Start in fullscreen"), parent);
    w.priority = new QCheckBox(QObject::tr("High process priority"), parent);
    w.priority->setToolTip(QObject::tr(
        "Windows gives Eglantine the CPU before normal apps. Helps when Discord, a browser or "
        "recording software run alongside the game."));
    w.pcores = new QCheckBox(QObject::tr("Performance cores only"), parent);
    w.pcores->setToolTip(QObject::tr(
        "Keeps Eglantine off the efficiency cores of 12th-gen and newer Intel CPUs. Can smooth out "
        "frame pacing; can also make first-time shader compilation slower. Try it per game."));
    w.pcores->setEnabled(cpu.hybrid);
    if (!cpu.hybrid) {
        w.pcores->setText(QObject::tr("Performance cores only (not a hybrid CPU)"));
    }
    w.throttle = new QCheckBox(QObject::tr("Never power-throttle"), parent);
    w.throttle->setToolTip(QObject::tr(
        "Opts Eglantine out of Windows power throttling (EcoQoS), so it keeps full clocks even when "
        "its window is in the background or minimised."));
    w.user = new QLineEdit(parent);
    w.user->setPlaceholderText(QObject::tr("Default profile"));
    w.args = new QLineEdit(parent);
    w.args->setPlaceholderText(QObject::tr("Extra command-line arguments"));
    form->addRow(w.fullscreen);
    form->addRow(w.priority);
    form->addRow(w.pcores);
    form->addRow(w.throttle);
    form->addRow(QObject::tr("Profile"), w.user);
    form->addRow(QObject::tr("Arguments"), w.args);
    return w;
}

void LoadOptionWidgets(const OptionWidgets& w, const Hub::LaunchOptions& o) {
    w.fullscreen->setChecked(o.fullscreen);
    w.priority->setChecked(o.high_priority);
    w.pcores->setChecked(o.performance_cores_only && w.pcores->isEnabled());
    w.throttle->setChecked(o.disable_power_throttling);
    w.user->setText(QString::fromStdString(o.user));
    w.args->setText(JoinArguments(o.extra_args));
}

Hub::LaunchOptions ReadOptionWidgets(const OptionWidgets& w) {
    Hub::LaunchOptions o;
    o.fullscreen = w.fullscreen->isChecked();
    o.high_priority = w.priority->isChecked();
    o.performance_cores_only = w.pcores->isChecked();
    o.disable_power_throttling = w.throttle->isChecked();
    o.user = w.user->text().trimmed().toStdString();
    o.extra_args = SplitArguments(w.args->text());
    return o;
}

QPixmap LoadCover(const QString& path) {
    QPixmap pixmap(path);
    if (pixmap.isNull()) {
        return pixmap;
    }
    return pixmap.scaled(kCoverSize, kCoverSize, Qt::KeepAspectRatioByExpanding,
                         Qt::SmoothTransformation)
        .copy(0, 0, kCoverSize, kCoverSize);
}

/// Cover art lying next to the game: "<game>.png", or cover/icon images in its folder.
QString FindSiblingCover(const Hub::GameFile& file) {
    const QFileInfo info(ToQString(file.path));
    const QDir dir = info.absoluteDir();
    QStringList candidates;
    if (file.format != Hub::GameFormat::ExeFS) {
        for (const char* ext : {".png", ".jpg", ".jpeg"}) {
            candidates << info.completeBaseName() + QLatin1String(ext);
        }
    }
    for (const char* name : {"cover.png", "cover.jpg", "icon.png", "icon.jpg", "icon_AmericanEnglish.dat"}) {
        candidates << QLatin1String(name);
    }
    for (const QString& candidate : candidates) {
        if (dir.exists(candidate)) {
            return dir.filePath(candidate);
        }
    }
    // ExeFS games usually sit in <game>/exefs/main, with art one level up.
    if (file.format == Hub::GameFormat::ExeFS) {
        QDir up = dir;
        if (up.cdUp()) {
            for (const char* name : {"cover.png", "cover.jpg", "icon.png", "icon.jpg"}) {
                if (up.exists(QLatin1String(name))) {
                    return up.filePath(QLatin1String(name));
                }
            }
        }
    }
    return {};
}

QPixmap PlaceholderCover(const QString& name, const QString& badge) {
    const QByteArray hash = QCryptographicHash::hash(name.toUtf8(), QCryptographicHash::Md5);
    const int hue = (static_cast<unsigned char>(hash[0]) * 360) / 256;
    QPixmap pixmap(kCoverSize, kCoverSize);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    QLinearGradient gradient(0, 0, kCoverSize, kCoverSize);
    gradient.setColorAt(0, QColor::fromHsl(hue, 150, 92));
    gradient.setColorAt(1, QColor::fromHsl((hue + 40) % 360, 120, 42));
    QPainterPath shape;
    shape.addRoundedRect(QRectF(0, 0, kCoverSize, kCoverSize), 22, 22);
    painter.fillPath(shape, gradient);

    QString initials;
    for (const QString& word : name.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        if (word.at(0).isLetterOrNumber()) {
            initials += word.at(0).toUpper();
        }
        if (initials.size() == 2) {
            break;
        }
    }
    QFont font = painter.font();
    font.setPixelSize(92);
    font.setBold(true);
    painter.setFont(font);
    painter.setPen(QColor(255, 255, 255, 230));
    painter.drawText(QRect(0, 0, kCoverSize, kCoverSize - 20), Qt::AlignCenter,
                     initials.isEmpty() ? QStringLiteral("?") : initials);

    font.setPixelSize(22);
    font.setBold(true);
    painter.setFont(font);
    painter.setPen(QColor(255, 255, 255, 170));
    painter.drawText(QRect(0, kCoverSize - 52, kCoverSize, 36), Qt::AlignCenter, badge);
    return pixmap;
}

QString SanitizeFileName(QString key) {
    for (QChar& c : key) {
        if (!c.isLetterOrNumber()) {
            c = QLatin1Char('_');
        }
    }
    return key.right(120);
}

} // namespace

// =============================================================================================

HubWindow::HubWindow(QWidget* parent) : QMainWindow(parent), cpu_(Hub::DetectCpuTopology()) {
    setWindowTitle(tr("Eglantine Hub"));
    setAcceptDrops(true);
    resize(1280, 800);
    LoadState();
    BuildUi();
    RefreshGrid();
    StartScan();
}

HubWindow::~HubWindow() {
    cancel_scan_ = true;
    if (scan_thread_) {
        scan_thread_->wait();
    }
}

void HubWindow::BuildUi() {
    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(14, 12, 14, 6);
    root->setSpacing(10);

    // --- Top bar
    auto* bar = new QHBoxLayout();
    auto* brand = new QLabel(tr("<b>Eglantine</b> Hub"), central);
    brand->setObjectName(QStringLiteral("brand"));
    search_ = new QLineEdit(central);
    search_->setPlaceholderText(tr("Search games or title IDs  (Ctrl+F)"));
    search_->setClearButtonEnabled(true);
    view_combo_ = new QComboBox(central);
    view_combo_->addItems({tr("All games"), tr("Favourites"), tr("Recently played")});
    view_combo_->setCurrentIndex(std::clamp(settings_.view, 0, 2));
    extras_check_ = new QCheckBox(tr("Updates && DLC"), central);
    extras_check_->setChecked(settings_.show_extras);
    auto* add_button = new QPushButton(tr("Add folder"), central);
    auto* rescan_button = new QPushButton(tr("Rescan"), central);
    auto* emulator_button = new QPushButton(tr("Open Eglantine"), central);
    auto* settings_button = new QPushButton(tr("Settings"), central);
    bar->addWidget(brand);
    bar->addSpacing(12);
    bar->addWidget(search_, 1);
    bar->addWidget(view_combo_);
    bar->addWidget(extras_check_);
    bar->addSpacing(8);
    bar->addWidget(add_button);
    bar->addWidget(rescan_button);
    bar->addWidget(emulator_button);
    bar->addWidget(settings_button);
    root->addLayout(bar);

    // --- Grid and details
    auto* splitter = new QSplitter(Qt::Horizontal, central);
    grid_stack_ = new QStackedWidget(splitter);
    grid_ = new QListWidget(grid_stack_);
    grid_->setViewMode(QListView::IconMode);
    grid_->setIconSize(QSize(kTileSize, kTileSize));
    grid_->setGridSize(QSize(kTileSize + 28, kTileSize + 52));
    grid_->setResizeMode(QListView::Adjust);
    grid_->setMovement(QListView::Static);
    grid_->setUniformItemSizes(true);
    grid_->setWordWrap(true);
    grid_->setSpacing(6);
    grid_->setSelectionMode(QAbstractItemView::SingleSelection);
    empty_label_ = new QLabel(grid_stack_);
    empty_label_->setAlignment(Qt::AlignCenter);
    empty_label_->setWordWrap(true);
    empty_label_->setObjectName(QStringLiteral("empty"));
    grid_stack_->addWidget(grid_);
    grid_stack_->addWidget(empty_label_);
    splitter->addWidget(grid_stack_);
    splitter->addWidget(BuildDetailsPanel());
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({880, 360});
    root->addWidget(splitter, 1);

    status_label_ = new QLabel(this);
    statusBar()->addWidget(status_label_, 1);
    auto* cpu_label = new QLabel(CpuSummary(cpu_), this);
    cpu_label->setObjectName(QStringLiteral("muted"));
    statusBar()->addPermanentWidget(cpu_label);
    setCentralWidget(central);

    connect(search_, &QLineEdit::textChanged, this, [this] { RefreshGrid(); });
    connect(view_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        settings_.view = index;
        SaveState();
        RefreshGrid();
    });
    connect(extras_check_, &QCheckBox::toggled, this, [this](bool on) {
        settings_.show_extras = on;
        SaveState();
        RefreshGrid();
    });
    connect(add_button, &QPushButton::clicked, this, [this] {
        const QString folder = QFileDialog::getExistingDirectory(this, tr("Choose a games folder"));
        if (!folder.isEmpty()) {
            AddFolder(folder);
        }
    });
    connect(rescan_button, &QPushButton::clicked, this, &HubWindow::StartScan);
    connect(emulator_button, &QPushButton::clicked, this, &HubWindow::LaunchEmulatorOnly);
    connect(settings_button, &QPushButton::clicked, this, &HubWindow::OpenSettings);
    connect(grid_, &QListWidget::itemSelectionChanged, this, &HubWindow::ShowDetails);
    connect(grid_, &QListWidget::itemActivated, this, [this] { Launch(SelectedRecord()); });

    auto* find_shortcut = new QShortcut(QKeySequence::Find, this);
    connect(find_shortcut, &QShortcut::activated, this, [this] {
        search_->setFocus();
        search_->selectAll();
    });
    auto* rescan_shortcut = new QShortcut(QKeySequence::Refresh, this);
    connect(rescan_shortcut, &QShortcut::activated, this, &HubWindow::StartScan);
}

QWidget* HubWindow::BuildDetailsPanel() {
    details_stack_ = new QStackedWidget(this);

    auto* placeholder = new QLabel(tr("Select a game to see its details.\n\nDouble-click or press "
                                      "Enter on a game to play it."),
                                   details_stack_);
    placeholder->setAlignment(Qt::AlignCenter);
    placeholder->setWordWrap(true);
    placeholder->setObjectName(QStringLiteral("muted"));
    details_stack_->addWidget(placeholder);

    auto* scroll = new QScrollArea(details_stack_);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* panel = new QWidget(scroll);
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(16, 8, 12, 8);
    layout->setSpacing(10);

    cover_label_ = new QLabel(panel);
    cover_label_->setFixedSize(kCoverSize, kCoverSize);
    cover_label_->setAlignment(Qt::AlignCenter);
    layout->addWidget(cover_label_, 0, Qt::AlignHCenter);

    title_label_ = new QLabel(panel);
    title_label_->setObjectName(QStringLiteral("title"));
    title_label_->setWordWrap(true);
    title_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(title_label_);

    play_button_ = new QPushButton(tr("▶  Play"), panel);
    play_button_->setObjectName(QStringLiteral("play"));
    play_button_->setMinimumHeight(44);
    layout->addWidget(play_button_);

    auto* actions = new QHBoxLayout();
    favorite_button_ = new QPushButton(panel);
    auto* folder_button = new QPushButton(tr("Show file"), panel);
    actions->addWidget(favorite_button_);
    actions->addWidget(folder_button);
    layout->addLayout(actions);
    auto* actions2 = new QHBoxLayout();
    auto* cover_button = new QPushButton(tr("Set cover…"), panel);
    auto* reset_cover_button = new QPushButton(tr("Reset cover"), panel);
    actions2->addWidget(cover_button);
    actions2->addWidget(reset_cover_button);
    layout->addLayout(actions2);
    shader_button_ = new QPushButton(tr("Clear shader cache"), panel);
    shader_button_->setToolTip(tr("Deletes this game's compiled shaders. Fixes graphics glitches "
                                  "and crashes after driver or emulator updates; the game will "
                                  "stutter for a while as it rebuilds them."));
    layout->addWidget(shader_button_);

    auto* info = new QFormLayout();
    info->setLabelAlignment(Qt::AlignLeft);
    info->setHorizontalSpacing(14);
    const auto add_info = [panel, info](const QString& label) {
        auto* value = new QLabel(panel);
        value->setWordWrap(true);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        info->addRow(label, value);
        return value;
    };
    kind_value_ = add_info(tr("Type"));
    format_value_ = add_info(tr("Format"));
    title_id_value_ = add_info(tr("Title ID"));
    version_value_ = add_info(tr("Version"));
    size_value_ = add_info(tr("Size"));
    integrity_value_ = add_info(tr("File check"));
    played_value_ = add_info(tr("Last played"));
    location_value_ = add_info(tr("Location"));
    layout->addLayout(info);

    custom_check_ = new QCheckBox(tr("Custom launch settings for this game"), panel);
    layout->addWidget(custom_check_);
    options_box_ = new QGroupBox(tr("Launch settings"), panel);
    auto* form = new QFormLayout(options_box_);
    const OptionWidgets w = MakeOptionWidgets(options_box_, form, cpu_);
    opt_fullscreen_ = w.fullscreen;
    opt_priority_ = w.priority;
    opt_pcores_ = w.pcores;
    opt_throttle_ = w.throttle;
    opt_user_ = w.user;
    opt_args_ = w.args;
    layout->addWidget(options_box_);
    layout->addStretch(1);

    scroll->setWidget(panel);
    details_stack_->addWidget(scroll);

    connect(play_button_, &QPushButton::clicked, this, [this] { Launch(SelectedRecord()); });
    connect(favorite_button_, &QPushButton::clicked, this, &HubWindow::ToggleFavorite);
    connect(folder_button, &QPushButton::clicked, this, &HubWindow::ShowInFolder);
    connect(cover_button, &QPushButton::clicked, this, &HubWindow::ChooseCover);
    connect(reset_cover_button, &QPushButton::clicked, this, &HubWindow::ResetCover);
    connect(shader_button_, &QPushButton::clicked, this, &HubWindow::ClearShaderCache);
    connect(custom_check_, &QCheckBox::toggled, this, [this](bool on) {
        options_box_->setEnabled(on);
        SaveDetailsOptions();
    });
    for (QCheckBox* box : {opt_fullscreen_, opt_priority_, opt_pcores_, opt_throttle_}) {
        connect(box, &QCheckBox::toggled, this, [this] { SaveDetailsOptions(); });
    }
    for (QLineEdit* edit : {opt_user_, opt_args_}) {
        connect(edit, &QLineEdit::editingFinished, this, [this] { SaveDetailsOptions(); });
    }
    return details_stack_;
}

// ---------------------------------------------------------------------------------------------
// State

QString HubWindow::StateFilePath() const {
    // Portable like Eglantine itself: a "user" folder next to the exe keeps everything local.
    const QDir app_dir(QApplication::applicationDirPath());
    if (app_dir.exists(QStringLiteral("user"))) {
        return app_dir.filePath(QStringLiteral("user/hub/hub.json"));
    }
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("hub.json"));
}

QString HubWindow::CoversDir() const {
    return QFileInfo(StateFilePath()).absoluteDir().filePath(QStringLiteral("covers"));
}

void HubWindow::LoadState() {
    QFile file(StateFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
    settings_.eglantine_path = json.value(QStringLiteral("eglantine_path")).toString();
    for (const QJsonValue& value : json.value(QStringLiteral("folders")).toArray()) {
        settings_.folders << value.toString();
    }
    settings_.recursive = json.value(QStringLiteral("recursive")).toBool(true);
    settings_.show_extras = json.value(QStringLiteral("show_extras")).toBool(false);
    settings_.view = json.value(QStringLiteral("view")).toInt(0);
    settings_.defaults =
        OptionsFromJson(json.value(QStringLiteral("defaults")).toObject(), Hub::LaunchOptions{});
    stored_games_ = json.value(QStringLiteral("games")).toObject();
}

void HubWindow::SaveState() const {
    QJsonObject games = stored_games_;
    for (const auto& record : records_) {
        QJsonObject entry;
        if (record->favorite) {
            entry.insert(QStringLiteral("favorite"), true);
        }
        if (record->last_played != 0) {
            entry.insert(QStringLiteral("last_played"), record->last_played);
        }
        if (record->hub_seconds != 0) {
            entry.insert(QStringLiteral("hub_seconds"), record->hub_seconds);
        }
        if (!record->custom_cover.isEmpty()) {
            entry.insert(QStringLiteral("cover"), record->custom_cover);
        }
        if (record->custom_options) {
            entry.insert(QStringLiteral("options"), OptionsToJson(record->options));
        }
        if (entry.isEmpty()) {
            games.remove(record->key);
        } else {
            games.insert(record->key, entry);
        }
    }
    QJsonArray folders;
    for (const QString& folder : settings_.folders) {
        folders.append(folder);
    }
    const QJsonObject json{
        {QStringLiteral("eglantine_path"), settings_.eglantine_path},
        {QStringLiteral("folders"), folders},
        {QStringLiteral("recursive"), settings_.recursive},
        {QStringLiteral("show_extras"), settings_.show_extras},
        {QStringLiteral("view"), settings_.view},
        {QStringLiteral("defaults"), OptionsToJson(settings_.defaults)},
        {QStringLiteral("games"), games},
    };
    const QString path = StateFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(json).toJson(QJsonDocument::Indented));
        file.commit();
    }
}

// ---------------------------------------------------------------------------------------------
// Scanning

void HubWindow::StartScan() {
    if (scan_thread_) {
        rescan_pending_ = true;
        return;
    }
    if (settings_.folders.isEmpty()) {
        ScanFinished({});
        return;
    }
    SetStatus(tr("Scanning %n folder(s)…", nullptr, static_cast<int>(settings_.folders.size())));
    cancel_scan_ = false;
    std::vector<fs::path> roots;
    for (const QString& folder : settings_.folders) {
        roots.push_back(ToPath(folder));
    }
    const bool recursive = settings_.recursive;
    QPointer<HubWindow> self(this);
    scan_thread_ = QThread::create([self, roots, recursive, cancel = &cancel_scan_] {
        std::vector<Hub::GameFile> found;
        std::set<fs::path> seen;
        for (const fs::path& folder : roots) {
            for (Hub::GameFile& game :
                 Hub::ScanFolder(folder, recursive, [cancel] { return cancel->load(); })) {
                std::error_code ec;
                const fs::path canonical = fs::weakly_canonical(game.path, ec);
                if (seen.insert(ec ? game.path : canonical).second) {
                    found.push_back(std::move(game));
                }
            }
        }
        if (cancel->load()) {
            return;
        }
        QMetaObject::invokeMethod(
            qApp,
            [self, games = std::move(found)]() mutable {
                if (self) {
                    self->ScanFinished(std::move(games));
                }
            },
            Qt::QueuedConnection);
    });
    connect(scan_thread_, &QThread::finished, this, [this] {
        scan_thread_->deleteLater();
        scan_thread_ = nullptr;
        if (rescan_pending_) {
            rescan_pending_ = false;
            StartScan();
        }
    });
    scan_thread_->start(QThread::LowPriority);
}

void HubWindow::ScanFinished(std::vector<Hub::GameFile> files) {
    // Keep the selection across rescans.
    const GameRecord* selected = SelectedRecord();
    const QString selected_key = selected ? selected->key : QString();
    SaveState(); // Persist per-game state of the old records before replacing them.
    {
        QFile file(StateFilePath());
        if (file.open(QIODevice::ReadOnly)) {
            stored_games_ = QJsonDocument::fromJson(file.readAll())
                                .object()
                                .value(QStringLiteral("games"))
                                .toObject();
        }
    }

    records_.clear();
    records_.reserve(files.size());
    for (Hub::GameFile& game : files) {
        auto record = std::make_unique<GameRecord>();
        record->file = std::move(game);
        record->key = RecordKey(record->file);
        record->display_name = QString::fromStdString(record->file.name);
        const QJsonObject entry = stored_games_.value(record->key).toObject();
        record->favorite = entry.value(QStringLiteral("favorite")).toBool(false);
        record->last_played = entry.value(QStringLiteral("last_played")).toInteger(0);
        record->hub_seconds = entry.value(QStringLiteral("hub_seconds")).toInteger(0);
        record->custom_cover = entry.value(QStringLiteral("cover")).toString();
        record->custom_options = entry.contains(QStringLiteral("options"));
        record->options =
            OptionsFromJson(entry.value(QStringLiteral("options")).toObject(), settings_.defaults);
        record->icon = MakeIcon(*record);
        records_.push_back(std::move(record));
    }
    std::sort(records_.begin(), records_.end(), [](const auto& a, const auto& b) {
        return QString::localeAwareCompare(a->display_name, b->display_name) < 0;
    });

    RefreshGrid();
    for (int i = 0; i < grid_->count(); ++i) {
        QListWidgetItem* item = grid_->item(i);
        if (item->data(Qt::UserRole).toString() == selected_key) {
            grid_->setCurrentItem(item);
            break;
        }
    }

    int games = 0;
    int problems = 0;
    for (const auto& record : records_) {
        games += record->file.kind == Hub::ContentKind::Base ||
                         record->file.kind == Hub::ContentKind::Unknown
                     ? 1
                     : 0;
        problems += record->file.integrity == Hub::Integrity::Truncated ||
                            record->file.integrity == Hub::Integrity::Invalid
                        ? 1
                        : 0;
    }
    QString status = tr("%n game(s)", nullptr, games);
    if (records_.size() > static_cast<std::size_t>(games)) {
        status += tr(", %n update/DLC file(s)", nullptr,
                     static_cast<int>(records_.size()) - games);
    }
    if (problems > 0) {
        status += tr("  ·  ⚠ %n damaged file(s)", nullptr, problems);
    }
    SetStatus(status);
}

QIcon HubWindow::MakeIcon(const GameRecord& record) const {
    QPixmap pixmap;
    if (!record.custom_cover.isEmpty()) {
        pixmap = LoadCover(record.custom_cover);
    }
    if (pixmap.isNull()) {
        const QString sibling = FindSiblingCover(record.file);
        if (!sibling.isEmpty()) {
            pixmap = LoadCover(sibling);
        }
    }
    if (pixmap.isNull()) {
        QString badge = QString::fromUtf8(Hub::FormatName(record.file.format).data(),
                                          static_cast<qsizetype>(Hub::FormatName(record.file.format).size()));
        if (record.file.kind == Hub::ContentKind::Update || record.file.kind == Hub::ContentKind::DLC) {
            badge = QString::fromUtf8(Hub::KindName(record.file.kind).data(),
                                      static_cast<qsizetype>(Hub::KindName(record.file.kind).size()));
        }
        pixmap = PlaceholderCover(record.display_name, badge);
    }
    return QIcon(pixmap);
}

void HubWindow::RefreshGrid() {
    const QString filter = search_ ? search_->text().trimmed() : QString();
    std::vector<const GameRecord*> visible;
    for (const auto& record : records_) {
        const bool extra = record->file.kind == Hub::ContentKind::Update ||
                           record->file.kind == Hub::ContentKind::DLC;
        if (extra && !settings_.show_extras) {
            continue;
        }
        if (settings_.view == 1 && !record->favorite) {
            continue;
        }
        if (settings_.view == 2 && record->last_played == 0) {
            continue;
        }
        if (!filter.isEmpty()) {
            const QString tid = record->file.title_id
                                    ? QString::fromStdString(Hub::FormatTitleId(*record->file.title_id))
                                    : QString();
            if (!record->display_name.contains(filter, Qt::CaseInsensitive) &&
                !tid.contains(filter, Qt::CaseInsensitive)) {
                continue;
            }
        }
        visible.push_back(record.get());
    }
    if (settings_.view == 2) {
        std::stable_sort(visible.begin(), visible.end(), [](const GameRecord* a, const GameRecord* b) {
            return a->last_played > b->last_played;
        });
    } else {
        // Favourites first.
        std::stable_sort(visible.begin(), visible.end(), [](const GameRecord* a, const GameRecord* b) {
            return a->favorite && !b->favorite;
        });
    }

    const QSignalBlocker blocker(grid_);
    const QListWidgetItem* current = grid_->currentItem();
    const QString current_key = current ? current->data(Qt::UserRole).toString() : QString();
    grid_->clear();
    QListWidgetItem* restore = nullptr;
    for (const GameRecord* record : visible) {
        auto* item = new QListWidgetItem(record->icon,
                                         (record->favorite ? QStringLiteral("★ ") : QString()) +
                                             record->display_name,
                                         grid_);
        item->setData(Qt::UserRole, record->key);
        item->setData(Qt::UserRole + 1, ToQString(record->file.path));
        item->setToolTip(record->display_name);
        item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
        if (record->file.integrity == Hub::Integrity::Truncated ||
            record->file.integrity == Hub::Integrity::Invalid) {
            item->setForeground(QColor(0xFF, 0x9A, 0x6B));
            item->setText(QStringLiteral("⚠ ") + item->text());
        }
        if (record->key == current_key) {
            restore = item;
        }
    }
    if (restore) {
        grid_->setCurrentItem(restore);
    }

    if (visible.empty()) {
        if (settings_.folders.isEmpty()) {
            empty_label_->setText(tr("<h2>Welcome to Eglantine Hub</h2>"
                                     "<p>Click <b>Add folder</b> or drag a folder here to find "
                                     "your NSP, XCI, NSZ, XCZ, NRO and ExeFS (main) games.</p>"));
        } else if (records_.empty()) {
            empty_label_->setText(tr("<h3>No games found yet</h3><p>Scanning your folders, or "
                                     "they hold no NSP, XCI, NSZ, XCZ, NRO or main files.</p>"));
        } else {
            empty_label_->setText(tr("<h3>Nothing matches</h3><p>Try another search or view.</p>"));
        }
        grid_stack_->setCurrentWidget(empty_label_);
    } else {
        grid_stack_->setCurrentWidget(grid_);
    }
    ShowDetails();
}

// ---------------------------------------------------------------------------------------------
// Details

GameRecord* HubWindow::SelectedRecord() {
    if (!grid_) {
        return nullptr;
    }
    const QList<QListWidgetItem*> items = grid_->selectedItems();
    if (items.isEmpty()) {
        return nullptr;
    }
    const QString path = items.first()->data(Qt::UserRole + 1).toString();
    for (const auto& record : records_) {
        if (ToQString(record->file.path) == path) {
            return record.get();
        }
    }
    return nullptr;
}

void HubWindow::ShowDetails() {
    const GameRecord* record = SelectedRecord();
    if (!record) {
        details_stack_->setCurrentIndex(0);
        return;
    }
    details_stack_->setCurrentIndex(1);
    loading_details_ = true;

    const Hub::GameFile& f = record->file;
    cover_label_->setPixmap(record->icon.pixmap(kCoverSize, kCoverSize));
    title_label_->setText(record->display_name);
    kind_value_->setText(QString::fromUtf8(Hub::KindName(f.kind).data(),
                                           static_cast<qsizetype>(Hub::KindName(f.kind).size())));
    format_value_->setText(QString::fromUtf8(Hub::FormatName(f.format).data(),
                                             static_cast<qsizetype>(Hub::FormatName(f.format).size())));
    QString tid = tr("Unknown (not in the file name and no ticket)");
    if (f.title_id) {
        tid = QString::fromStdString(Hub::FormatTitleId(*f.title_id));
        if (f.kind == Hub::ContentKind::Update || f.kind == Hub::ContentKind::DLC) {
            tid += tr("\nfor game %1").arg(QString::fromStdString(Hub::FormatTitleId(Hub::BaseTitleId(*f.title_id))));
        }
    }
    title_id_value_->setText(tid);
    version_value_->setText(f.version.empty() ? tr("—") : QString::fromStdString(f.version));
    size_value_->setText(QString::fromStdString(Hub::FormatSize(f.size_bytes)));
    QString check = QString::fromStdString(f.integrity_detail);
    switch (f.integrity) {
    case Hub::Integrity::Ok:
        check = QStringLiteral("✔ ") + check;
        integrity_value_->setStyleSheet(QStringLiteral("color: #7fd99a;"));
        break;
    case Hub::Integrity::Truncated:
    case Hub::Integrity::Invalid:
        check = QStringLiteral("⚠ ") + check;
        integrity_value_->setStyleSheet(QStringLiteral("color: #ff9a6b;"));
        break;
    case Hub::Integrity::Unknown:
        integrity_value_->setStyleSheet(QString());
        break;
    }
    integrity_value_->setText(check);
    played_value_->setText(FormatPlayed(record->last_played, record->hub_seconds));
    // Paths have no spaces to wrap at, so allow breaks after each separator.
    QString location = QDir::toNativeSeparators(ToQString(f.path));
    location.replace(QDir::separator(), QString(QDir::separator()) + QChar(0x200B));
    location_value_->setText(location);

    const bool launchable = f.kind != Hub::ContentKind::Update && f.kind != Hub::ContentKind::DLC;
    play_button_->setEnabled(launchable);
    play_button_->setText(launchable ? tr("▶  Play") : tr("Install it from Eglantine's File menu"));
    favorite_button_->setText(record->favorite ? tr("★ Favourite") : tr("☆ Favourite"));
    shader_button_->setEnabled(f.title_id.has_value());

    custom_check_->setChecked(record->custom_options);
    options_box_->setEnabled(record->custom_options);
    const OptionWidgets w{opt_fullscreen_, opt_priority_, opt_pcores_, opt_throttle_, opt_user_, opt_args_};
    LoadOptionWidgets(w, record->custom_options ? record->options : settings_.defaults);
    loading_details_ = false;
}

void HubWindow::SaveDetailsOptions() {
    if (loading_details_) {
        return;
    }
    GameRecord* record = SelectedRecord();
    if (!record) {
        return;
    }
    record->custom_options = custom_check_->isChecked();
    const OptionWidgets w{opt_fullscreen_, opt_priority_, opt_pcores_, opt_throttle_, opt_user_, opt_args_};
    if (record->custom_options) {
        record->options = ReadOptionWidgets(w);
    } else {
        loading_details_ = true;
        LoadOptionWidgets(w, settings_.defaults);
        loading_details_ = false;
    }
    SaveState();
}

// ---------------------------------------------------------------------------------------------
// Actions

void HubWindow::AddFolder(const QString& folder) {
    const QString clean = QDir::cleanPath(QDir(folder).absolutePath());
    if (!settings_.folders.contains(clean)) {
        settings_.folders << clean;
        SaveState();
    }
    StartScan();
}

QString HubWindow::ResolveEglantinePath() const {
    if (!settings_.eglantine_path.isEmpty() && QFileInfo::exists(settings_.eglantine_path)) {
        return settings_.eglantine_path;
    }
    const QDir app_dir(QApplication::applicationDirPath());
#ifdef _WIN32
    const QString name = QStringLiteral("eglantine.exe");
#else
    const QString name = QStringLiteral("eglantine");
#endif
    if (app_dir.exists(name)) {
        return app_dir.filePath(name);
    }
    return {};
}

QString HubWindow::EmulatorDataDir() const {
    // Mirrors Eglantine's own lookup: a "user" folder next to its exe wins over the global one.
    const QString emulator = ResolveEglantinePath();
    if (!emulator.isEmpty()) {
        const QDir dir = QFileInfo(emulator).absoluteDir();
        if (dir.exists(QStringLiteral("user"))) {
            return dir.filePath(QStringLiteral("user"));
        }
    }
#ifdef _WIN32
    return QDir(qEnvironmentVariable("APPDATA")).filePath(QStringLiteral("eden"));
#else
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath(QStringLiteral("eden"));
#endif
}

void HubWindow::Launch(GameRecord* record) {
    if (!record) {
        return;
    }
    if (record->file.kind == Hub::ContentKind::Update || record->file.kind == Hub::ContentKind::DLC) {
        QMessageBox::information(this, tr("Not a game"),
                                 tr("This is an update or DLC file. Install it in Eglantine with "
                                    "File → Install Files to NAND, then start the base game."));
        return;
    }
    if (record->file.integrity == Hub::Integrity::Truncated) {
        const auto answer = QMessageBox::warning(
            this, tr("Damaged file"),
            tr("%1\n\nIt will probably crash or fail to load. Start it anyway?")
                .arg(QString::fromStdString(record->file.integrity_detail)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            return;
        }
    }
    const QString emulator = ResolveEglantinePath();
    if (emulator.isEmpty()) {
        QMessageBox::warning(this, tr("Eglantine not found"),
                             tr("Put Eglantine Hub in the same folder as eglantine.exe, or choose "
                                "its location in Settings."));
        OpenSettings();
        return;
    }

    const Hub::LaunchOptions options = record->custom_options ? record->options : settings_.defaults;
    const Hub::LaunchResult result = Hub::LaunchEmulator(ToPath(emulator), record->file.path, options);
    if (!result.ok) {
        QMessageBox::critical(this, tr("Could not start"), QString::fromStdString(result.error));
        return;
    }
    record->last_played = QDateTime::currentSecsSinceEpoch();
    SaveState();
    ShowDetails();

    QStringList tweaks;
    if (options.high_priority) {
        tweaks << tr("high priority");
    }
    if (options.performance_cores_only && cpu_.hybrid) {
        tweaks << tr("P-cores only");
    }
    if (options.disable_power_throttling) {
        tweaks << tr("no power throttling");
    }
    SetStatus(tweaks.isEmpty() ? tr("Playing %1").arg(record->display_name)
                               : tr("Playing %1  (%2)").arg(record->display_name, tweaks.join(QStringLiteral(", "))));

    // Track the session length without blocking the UI.
    QPointer<HubWindow> self(this);
    const QString key = record->key;
    const qint64 started = record->last_played;
    std::thread([self, key, started, pid = result.pid] {
        if (!Hub::WaitForExit(pid)) {
            return;
        }
        const qint64 seconds = QDateTime::currentSecsSinceEpoch() - started;
        QMetaObject::invokeMethod(
            qApp,
            [self, key, seconds] {
                if (!self) {
                    return;
                }
                for (const auto& r : self->records_) {
                    if (r->key == key) {
                        r->hub_seconds += seconds;
                        self->SetStatus(tr("%1 closed after %2 min")
                                            .arg(r->display_name)
                                            .arg(seconds / 60));
                        break;
                    }
                }
                self->SaveState();
                self->ShowDetails();
            },
            Qt::QueuedConnection);
    }).detach();
}

void HubWindow::LaunchEmulatorOnly() {
    const QString emulator = ResolveEglantinePath();
    if (emulator.isEmpty()) {
        OpenSettings();
        return;
    }
    const Hub::LaunchResult result = Hub::LaunchEmulator(ToPath(emulator), {}, settings_.defaults);
    if (!result.ok) {
        QMessageBox::critical(this, tr("Could not start"), QString::fromStdString(result.error));
    }
}

void HubWindow::ToggleFavorite() {
    GameRecord* record = SelectedRecord();
    if (!record) {
        return;
    }
    record->favorite = !record->favorite;
    SaveState();
    RefreshGrid();
}

void HubWindow::ChooseCover() {
    GameRecord* record = SelectedRecord();
    if (!record) {
        return;
    }
    const QString source = QFileDialog::getOpenFileName(
        this, tr("Choose cover art"), QString(), tr("Images (*.png *.jpg *.jpeg *.bmp *.gif)"));
    if (source.isEmpty()) {
        return;
    }
    const QPixmap pixmap = LoadCover(source);
    if (pixmap.isNull()) {
        QMessageBox::warning(this, tr("Cover"), tr("That image could not be opened."));
        return;
    }
    // Keep a copy so the cover survives the original image being moved.
    QDir().mkpath(CoversDir());
    const QString target = QDir(CoversDir()).filePath(SanitizeFileName(record->key) + QStringLiteral(".png"));
    pixmap.save(target, "PNG");
    record->custom_cover = target;
    record->icon = MakeIcon(*record);
    SaveState();
    RefreshGrid();
}

void HubWindow::ResetCover() {
    GameRecord* record = SelectedRecord();
    if (!record || record->custom_cover.isEmpty()) {
        return;
    }
    QFile::remove(record->custom_cover);
    record->custom_cover.clear();
    record->icon = MakeIcon(*record);
    SaveState();
    RefreshGrid();
}

void HubWindow::ShowInFolder() {
    const GameRecord* record = SelectedRecord();
    if (!record) {
        return;
    }
    const QString path = QDir::toNativeSeparators(ToQString(record->file.path));
#ifdef _WIN32
    QProcess::startDetached(QStringLiteral("explorer.exe"), {QStringLiteral("/select,"), path});
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
}

void HubWindow::ClearShaderCache() {
    const GameRecord* record = SelectedRecord();
    if (!record || !record->file.title_id) {
        return;
    }
    const QString tid = QString::fromStdString(Hub::FormatTitleId(*record->file.title_id)).toLower();
    const QString data_dir = EmulatorDataDir();
    QStringList candidates{QDir(data_dir).filePath(QStringLiteral("cache/shader/") + tid),
                           QDir(data_dir).filePath(QStringLiteral("shader/") + tid)};
#ifndef _WIN32
    candidates << QDir(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation))
                      .filePath(QStringLiteral("eden/shader/") + tid);
#endif
    QString found;
    for (const QString& candidate : candidates) {
        if (QDir(candidate).exists()) {
            found = candidate;
            break;
        }
    }
    if (found.isEmpty()) {
        QMessageBox::information(this, tr("Shader cache"),
                                 tr("%1 has no shader cache yet.").arg(record->display_name));
        return;
    }
    const auto answer = QMessageBox::question(
        this, tr("Clear shader cache"),
        tr("Delete the compiled shaders for %1?\n\n%2\n\nClose the game first. It will stutter "
           "for a while as it rebuilds them.")
            .arg(record->display_name, QDir::toNativeSeparators(found)));
    if (answer != QMessageBox::Yes) {
        return;
    }
    if (QDir(found).removeRecursively()) {
        SetStatus(tr("Cleared the shader cache for %1").arg(record->display_name));
    } else {
        QMessageBox::warning(this, tr("Shader cache"),
                             tr("Some files could not be deleted. Is the game still running?"));
    }
}

void HubWindow::OpenDataFolder() {
    QDesktopServices::openUrl(QUrl::fromLocalFile(EmulatorDataDir()));
}

void HubWindow::OpenSettings() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Eglantine Hub settings"));
    dialog.setMinimumWidth(560);
    auto* layout = new QVBoxLayout(&dialog);

    auto* emulator_box = new QGroupBox(tr("Eglantine"), &dialog);
    auto* emulator_row = new QHBoxLayout(emulator_box);
    auto* path_edit = new QLineEdit(settings_.eglantine_path, emulator_box);
    path_edit->setPlaceholderText(ResolveEglantinePath().isEmpty()
                                      ? tr("Path to eglantine.exe")
                                      : tr("Auto: %1").arg(QDir::toNativeSeparators(ResolveEglantinePath())));
    auto* browse = new QPushButton(tr("Browse…"), emulator_box);
    auto* data_button = new QPushButton(tr("Open data folder"), emulator_box);
    emulator_row->addWidget(path_edit, 1);
    emulator_row->addWidget(browse);
    emulator_row->addWidget(data_button);
    layout->addWidget(emulator_box);

    auto* folders_box = new QGroupBox(tr("Game folders"), &dialog);
    auto* folders_layout = new QVBoxLayout(folders_box);
    auto* folder_list = new QListWidget(folders_box);
    folder_list->addItems(settings_.folders);
    folder_list->setMaximumHeight(140);
    auto* folder_buttons = new QHBoxLayout();
    auto* add = new QPushButton(tr("Add…"), folders_box);
    auto* remove = new QPushButton(tr("Remove"), folders_box);
    auto* recursive = new QCheckBox(tr("Include subfolders"), folders_box);
    recursive->setChecked(settings_.recursive);
    folder_buttons->addWidget(add);
    folder_buttons->addWidget(remove);
    folder_buttons->addStretch(1);
    folder_buttons->addWidget(recursive);
    folders_layout->addWidget(folder_list);
    folders_layout->addLayout(folder_buttons);
    layout->addWidget(folders_box);

    auto* defaults_box = new QGroupBox(tr("Default launch settings"), &dialog);
    auto* defaults_form = new QFormLayout(defaults_box);
    auto* cpu_info = new QLabel(CpuSummary(cpu_), defaults_box);
    cpu_info->setObjectName(QStringLiteral("muted"));
    cpu_info->setWordWrap(true);
    defaults_form->addRow(cpu_info);
    const OptionWidgets defaults = MakeOptionWidgets(defaults_box, defaults_form, cpu_);
    LoadOptionWidgets(defaults, settings_.defaults);
    layout->addWidget(defaults_box);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);

    connect(browse, &QPushButton::clicked, &dialog, [&dialog, path_edit] {
        const QString file = QFileDialog::getOpenFileName(&dialog, tr("Find eglantine.exe"), QString(),
#ifdef _WIN32
                                                          tr("Programs (*.exe)")
#else
                                                          QString()
#endif
        );
        if (!file.isEmpty()) {
            path_edit->setText(file);
        }
    });
    connect(data_button, &QPushButton::clicked, this, &HubWindow::OpenDataFolder);
    connect(add, &QPushButton::clicked, &dialog, [&dialog, folder_list] {
        const QString folder = QFileDialog::getExistingDirectory(&dialog, tr("Choose a games folder"));
        if (!folder.isEmpty()) {
            folder_list->addItem(QDir::cleanPath(folder));
        }
    });
    connect(remove, &QPushButton::clicked, &dialog, [folder_list] { delete folder_list->currentItem(); });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    settings_.eglantine_path = path_edit->text().trimmed();
    QStringList folders;
    for (int i = 0; i < folder_list->count(); ++i) {
        folders << folder_list->item(i)->text();
    }
    const bool folders_changed = folders != settings_.folders || recursive->isChecked() != settings_.recursive;
    settings_.folders = folders;
    settings_.recursive = recursive->isChecked();
    settings_.defaults = ReadOptionWidgets(defaults);
    for (const auto& record : records_) {
        if (!record->custom_options) {
            record->options = settings_.defaults;
        }
    }
    SaveState();
    ShowDetails();
    if (folders_changed) {
        StartScan();
    }
}

void HubWindow::SetStatus(const QString& text) {
    if (status_label_) {
        status_label_->setText(text);
    }
}

// ---------------------------------------------------------------------------------------------
// Events

void HubWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void HubWindow::dropEvent(QDropEvent* event) {
    for (const QUrl& url : event->mimeData()->urls()) {
        const QFileInfo info(url.toLocalFile());
        if (info.isDir()) {
            AddFolder(info.absoluteFilePath());
        } else if (info.isFile()) {
            AddFolder(info.absolutePath());
        }
    }
}

void HubWindow::closeEvent(QCloseEvent* event) {
    SaveState();
    QMainWindow::closeEvent(event);
}

} // namespace HubUi
