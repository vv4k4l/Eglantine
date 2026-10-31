// SPDX-FileCopyrightText: Copyright 2026 Eglantine Hub contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QApplication>
#include <QIcon>
#include <QPalette>
#include <QStyleFactory>

#include "hub_window.h"

namespace {

void ApplyTheme(QApplication& app) {
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    const QColor base(0x17, 0x15, 0x1a);
    const QColor window(0x1e, 0x1b, 0x22);
    const QColor text(0xEC, 0xE6, 0xEE);
    const QColor accent(0xE0, 0x60, 0x7E); // eglantine rose
    QPalette palette;
    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, base);
    palette.setColor(QPalette::AlternateBase, window);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Button, QColor(0x2a, 0x26, 0x30));
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::ToolTipBase, QColor(0x2a, 0x26, 0x30));
    palette.setColor(QPalette::ToolTipText, text);
    palette.setColor(QPalette::PlaceholderText, QColor(0x8c, 0x84, 0x92));
    palette.setColor(QPalette::Highlight, accent);
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::Link, accent);
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(0x6c, 0x66, 0x72));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x6c, 0x66, 0x72));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x6c, 0x66, 0x72));
    app.setPalette(palette);

    app.setStyleSheet(QStringLiteral(R"(
        QLabel#brand { font-size: 18px; color: #f2c6d2; }
        QLabel#title { font-size: 20px; font-weight: 600; }
        QLabel#muted { color: #9a92a0; }
        QLabel#empty { color: #b8b0be; font-size: 14px; }
        QListWidget { border: none; background: #17151a; padding: 6px; }
        QListWidget::item { border-radius: 10px; padding: 4px; color: #ece6ee; }
        QListWidget::item:hover { background: #2a2530; }
        QListWidget::item:selected { background: #46303a; }
        QLineEdit, QComboBox { padding: 6px 8px; border-radius: 6px; border: 1px solid #3a3440; background: #17151a; }
        QLineEdit:focus { border-color: #E0607E; }
        QPushButton { padding: 6px 12px; border-radius: 6px; border: 1px solid #3a3440; background: #2a2630; }
        QPushButton:hover { background: #343040; }
        QPushButton:disabled { color: #6c6672; }
        QPushButton#play { background: #E0607E; border: none; color: white; font-size: 15px; font-weight: 600; }
        QPushButton#play:hover { background: #ea7590; }
        QPushButton#play:disabled { background: #3a3440; color: #8c8492; }
        QGroupBox { border: 1px solid #3a3440; border-radius: 8px; margin-top: 14px; padding: 10px 8px 6px 8px; }
        QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: #c9c1cf; }
        QStatusBar { color: #b8b0be; }
        QSplitter::handle { background: #1e1b22; width: 4px; }
    )"));
}

} // namespace

int main(int argc, char* argv[]) {
    QApplication::setApplicationName(QStringLiteral("EglantineHub"));
    QApplication::setOrganizationName(QStringLiteral("Eglantine"));
    QApplication app(argc, argv);
    app.setApplicationDisplayName(QStringLiteral("Eglantine Hub"));
    app.setWindowIcon(QIcon(QStringLiteral(":/hub/icon.png")));
    ApplyTheme(app);

    HubUi::HubWindow window;
    window.show();
    return app.exec();
}
