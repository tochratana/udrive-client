/*
 * Udrive Desktop — Unity Workspace
 * Udrive customization updated: 2026-10-26.
 *
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include "logbrowser.h"

#include "stdio.h"
#include <iostream>

#include <QDesktopServices>
#include <QDir>
#include <QLayout>
#include <QMessageBox>
#include <QTextStream>
#include <optional>

#include "configfile.h"
#include "guiutility.h"
#include "logger.h"
#include "theme.h"
#include "udriveicons.h"
#include "ui_logbrowser.h"

#include "resources/resources.h"

namespace OCC {

// ==============================================================================

LogBrowser::LogBrowser(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::LogBrowser)
{
    ui->setupUi(this);

    ui->warningIcon->setPixmap(Resources::getCoreIcon(QStringLiteral("warning")).pixmap(ui->warningIcon->size()));
    const QString path = QDir::toNativeSeparators(Logger::instance()->temporaryFolderLogDirPath());
    ui->locationField->setText(path);
    ui->locationField->setCursorPosition(0);
    ui->locationField->setToolTip(path);
    ui->locationField->setAccessibleName(tr("Log folder"));
    auto *closeButton = ui->buttonBox->button(QDialogButtonBox::Close);
    closeButton->setText(tr("Close"));
    closeButton->setDefault(true);
    ui->openFolderButton->setAutoDefault(false);

    if (Theme::instance()->appNameGUI() == QStringLiteral("Udrive")) {
        const auto mode = ConfigFile().themeMode();
        const bool dark = mode == ConfigFile::ThemeMode::Dark
            || (mode == ConfigFile::ThemeMode::System && palette().color(QPalette::Window).lightness() < 128);
        const QString background = dark ? QStringLiteral("#202222") : QStringLiteral("#FFFFFF");
        const QString surface = dark ? QStringLiteral("#292C2B") : QStringLiteral("#F7F9F8");
        const QString text = dark ? QStringLiteral("#EDEEF0") : QStringLiteral("#242B27");
        const QString muted = dark ? QStringLiteral("#A8B2AC") : QStringLiteral("#626D66");
        const QString border = dark ? QStringLiteral("#414944") : QStringLiteral("#DDE4DF");
        setStyleSheet(QStringLiteral(
            "QDialog { background: %1; }"
            "QScrollArea, QWidget#scrollContents { background: transparent; border: none; }"
            "QScrollBar:vertical { background: transparent; width: 8px; margin: 2px 0; }"
            "QScrollBar::handle:vertical { background: %5; border-radius: 4px; min-height: 28px; }"
            "QScrollBar::handle:vertical:hover { background: %4; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; border: none; background: transparent; }"
            "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }"
            "QLabel { color: %3; font-size: 13px; background: transparent; }"
            "QLabel#titleLabel { font-size: 20px; font-weight: 600; }"
            "QLabel#descriptionLabel, QLabel#retentionDescription, QLabel#settingsNote { color: %4; }"
            "QLabel#retentionDescription, QLabel#settingsNote { font-size: 12px; }"
            "QLabel#optionsTitle, QLabel#folderLabel { font-weight: 600; }"
            "QFrame#optionsCard { background: %2; border: 1px solid %5; border-radius: 12px; }"
            "QFrame#privacyCard { background: %6; border: 1px solid %7; border-radius: 10px; }"
            "QFrame#privacyCard QLabel { color: %8; }"
            "QLabel#privacyTitle { font-weight: 600; }"
            "QLabel#privacyDescription { font-size: 12px; }"
            "QCheckBox { color: %3; font-size: 13px; spacing: 10px; min-height: 24px; background: transparent; }"
            "QCheckBox::indicator { width: 18px; height: 18px; border: 1px solid %5; border-radius: 5px; background: %1; }"
            "QCheckBox::indicator:hover, QCheckBox::indicator:focus { border-color: #40D979; }"
            "QCheckBox::indicator:checked { background: #40D979; border-color: #40D979; image: url(:/client/resources/core/checkbox-check.svg); }"
            "QLineEdit, QSpinBox { background: %1; color: %3; border: 1px solid %5; border-radius: 7px; padding: 7px 10px; font-size: 13px; selection-background-color: #2E6445; selection-color: white; }"
            "QLineEdit:focus, QSpinBox:focus { border-color: #40D979; }"
            "QPushButton { background: %2; color: %3; border: 1px solid %5; border-radius: 8px; padding: 8px 16px; min-height: 20px; font-size: 13px; font-weight: 500; }"
            "QPushButton:hover { border-color: #40D979; }"
            "QPushButton:focus { border: 1px solid #007C58; }"
            "QDialogButtonBox QPushButton { background: #40D979; color: #12351D; border-color: #40D979; min-width: 64px; font-weight: 600; }"
            "QDialogButtonBox QPushButton:hover { background: #63E493; }"
            "QDialogButtonBox QPushButton:pressed { background: #30C86A; }")
                .arg(background, surface, text, muted, border,
                    dark ? QStringLiteral("#332E22") : QStringLiteral("#FFF8E9"),
                    dark ? QStringLiteral("#605137") : QStringLiteral("#EBD7AD"),
                    dark ? QStringLiteral("#F2D8A7") : QStringLiteral("#775312")));
        ui->warningIcon->setPixmap(UDriveIcons::icon(UDriveIcons::Symbol::Lock, false,
            dark ? QColor("#F2D8A7") : QColor("#A97619")).pixmap(QSize(24, 24), devicePixelRatioF()));
        ui->openFolderButton->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Folder, false, QColor(text)));
    }

    ui->enableLoggingButton->setChecked(ConfigFile().automaticLogDir());
    connect(ui->enableLoggingButton, &QCheckBox::toggled, this, &LogBrowser::togglePermanentLogging);

    ui->httpLogButton->setChecked(ConfigFile().logHttp());
    connect(ui->httpLogButton, &QCheckBox::toggled, this, [](bool enable) {
        ConfigFile().configureHttpLogging(std::make_optional(enable));
    });

    ui->spinBox_numberOflogsToKeep->setValue(ConfigFile().automaticDeleteOldLogs());
    connect(ui->spinBox_numberOflogsToKeep, qOverload<int>(&QSpinBox::valueChanged), this, [](int i) {
        ConfigFile().setAutomaticDeleteOldLogs(i);
        Logger::instance()->setMaxLogFiles(i);
    });


    connect(ui->openFolderButton, &QPushButton::clicked, this, []() {
        QString path = Logger::instance()->temporaryFolderLogDirPath();
        QDir().mkpath(path);
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    });
    connect(ui->buttonBox->button(QDialogButtonBox::Close), &QPushButton::clicked, this, &QWidget::close);

    ConfigFile cfg;
    cfg.restoreGeometry(this);
}

LogBrowser::~LogBrowser()
{
}

void LogBrowser::setupLoggingFromConfig()
{
    ConfigFile config;
    auto logger = Logger::instance();

    if (config.automaticLogDir()) {
        // Don't override other configured logging
        if (logger->isLoggingToFile())
            return;

        logger->setupTemporaryFolderLogDir();
        Logger::instance()->setMaxLogFiles(config.automaticDeleteOldLogs());
    } else {
        logger->disableTemporaryFolderLogDir();
    }
}

void LogBrowser::togglePermanentLogging(bool enabled)
{
    ConfigFile config;
    config.setAutomaticLogDir(enabled);
    setupLoggingFromConfig();
}

} // namespace
