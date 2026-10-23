/*
 * Copyright (C) by Daniel Molkentin <danimo@owncloud.com>
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

#include "generalsettings.h"
#include "ui_generalsettings.h"

#include "common/restartmanager.h"
#include "common/version.h"
#include "gui/application.h"
#include "gui/ignorelisteditor.h"
#include "gui/settingsdialog.h"
#include "gui/translations.h"
#include "appearancewidget.h"
#include "udrivetoast.h"
#include "libsync/configfile.h"
#include "libsync/theme.h"

#include <QMessageBox>
#include <QOperatingSystemVersion>
#include <QScopedValueRollback>
#include <QDialog>
#include <QLabel>
#include <QListView>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace OCC {

GeneralSettings::GeneralSettings(QWidget *parent)
    : QWidget(parent)
    , _ui(new Ui::GeneralSettings)
    , _currentlyLoading(false)
{
    _ui->setupUi(this);

    if (Theme::instance()->appNameGUI() == QStringLiteral("UDrive")) {
        _appearanceWidget = new AppearanceWidget(this);
        _appearanceWidget->setObjectName(QStringLiteral("appearanceSection"));
        _ui->verticalLayout_5->insertWidget(0, _appearanceWidget);

        connect(_appearanceWidget, &AppearanceWidget::themeChanged, this, [this](ConfigFile::ThemeMode mode) {
            ConfigFile cfg;
            cfg.setThemeMode(mode);
            Application::applyApplicationTheme(mode);
            UDriveToast::preferenceSaved(this);
        });

        auto *languages = _ui->languageDropdown;
        languages->setMinimumWidth(240);
        languages->setMaximumWidth(320);
        auto *list = new QListView(languages);
        list->setObjectName(QStringLiteral("driveLanguagePopup"));
        list->setFrameShape(QFrame::NoFrame);
        list->setUniformItemSizes(true);
        list->setSpacing(2);
        list->setMouseTracking(true);
        list->setTextElideMode(Qt::ElideNone);
        list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        languages->setView(list);
        languages->setItemDelegate(new QStyledItemDelegate(languages));
        languages->setMaxVisibleItems(3);
    }

    connect(_ui->desktopNotificationsCheckBox, &QAbstractButton::toggled, this, &GeneralSettings::slotToggleOptionalDesktopNotifications);

    reloadConfig();
    loadMiscSettings();

    // misc
    connect(_ui->monoIconsCheckBox, &QAbstractButton::toggled, this, &GeneralSettings::saveMiscSettings);
    connect(_ui->crashreporterCheckBox, &QAbstractButton::toggled, this, &GeneralSettings::saveMiscSettings);

    connect(_ui->languageDropdown, QOverload<int>::of(&QComboBox::activated), this, [this]() {
        // first, store selected language in config file
        saveMiscSettings();

        const QString pickedLocale = _ui->languageDropdown->currentData().toString();
        if (Theme::instance()->appNameGUI() == QStringLiteral("UDrive")) {
            if (auto *app = Application::instance()) {
                app->applyLanguage(pickedLocale);
            } else {
                Translations::installLanguage(pickedLocale);
            }
        }
        showLanguageRestartNotice();
    });

    /* handle the hidden file checkbox */

    /* the ignoreHiddenFiles flag is a folder specific setting, but for now, it is
     * handled globally. Save it to every folder that is defined.
     */
    connect(_ui->syncHiddenFilesCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
        FolderMan::instance()->setIgnoreHiddenFiles(!checked);
        if (!_currentlyLoading) UDriveToast::preferenceSaved(this);
    });

    _ui->crashreporterCheckBox->setVisible(Theme::instance()->withCrashReporter());

    _ui->moveToTrashCheckBox->setVisible(true);
    connect(_ui->moveToTrashCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
        ConfigFile().setMoveToTrash(checked);
        Q_EMIT syncOptionsChanged();
        if (!_currentlyLoading) UDriveToast::preferenceSaved(this);
    });
    connect(_ui->autostartCheckBox, &QAbstractButton::clicked, this, [this] { UDriveToast::preferenceSaved(this); });

    // OEM themes are not obliged to ship mono icons, so there
    // is no point in offering an option
    _ui->monoIconsCheckBox->setVisible(Resources::hasMonoTheme());

    connect(_ui->ignoredFilesButton, &QAbstractButton::clicked, this, &GeneralSettings::slotIgnoreFilesEditor);
    connect(_ui->logSettingsButton, &QPushButton::clicked, this, [] {
        // only access occApp after things are set up
        ocApp()->gui()->slotToggleLogBrowser();
    });

    connect(_ui->about_pushButton, &QPushButton::clicked, this, &GeneralSettings::showAbout);

    if (Theme::instance()->appNameGUI() == QStringLiteral("UDrive")) {
        _ui->copyrightLabel->setText(tr("Unity Workspace"));
        _ui->about_pushButton->setText(tr("About %1").arg(Theme::instance()->appNameGUI()));
    }

    if (!Theme::instance()->aboutShowCopyright()) {
        _ui->copyrightLabel->hide();
    }
}

void GeneralSettings::showLanguageRestartNotice()
{
    if (Theme::instance()->appNameGUI() != QStringLiteral("UDrive")) {
        QMessageBox::warning(this, tr("Warning"), tr("Language changes require a restart of this application to take effect."), QMessageBox::Ok);
        return;
    }
    if (_languageRestartNotice) {
        _languageRestartNotice->raise();
        _languageRestartNotice->activateWindow();
        return;
    }

    auto *dialog = new QDialog(this);
    _languageRestartNotice = dialog;
    dialog->setObjectName(QStringLiteral("languageRestartNotice"));
    dialog->setWindowTitle(tr("Language saved"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowFlags((dialog->windowFlags() | Qt::Dialog) & ~Qt::WindowContextHelpButtonHint);
    dialog->setMinimumWidth(400);
    dialog->setMaximumWidth(480);

    const bool dark = QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
    const QString surface = dark ? QStringLiteral("#292B2D") : QStringLiteral("#FFFFFF");
    const QString text = dark ? QStringLiteral("#EDEEF0") : QStringLiteral("#202923");
    const QString secondary = dark ? QStringLiteral("#B7C1BA") : QStringLiteral("#5D6B62");
    dialog->setStyleSheet(QStringLiteral(
        "QDialog#languageRestartNotice { background: %1; }"
        "QLabel#languageRestartTitle { color: %2; font-size: 15px; font-weight: 600; }"
        "QLabel#languageRestartMessage { color: %3; font-size: 13px; }"
        "QDialog#languageRestartNotice QPushButton#languageRestartOkay { min-width: 0; min-height: 32px; padding: 0 14px; background: #40D979; color: #12351D; border: 1px solid #40D979; border-radius: 8px; font-size: 13px; font-weight: 600; }"
        "QDialog#languageRestartNotice QPushButton#languageRestartOkay:hover { background: #63E493; border-color: #63E493; }"
        "QDialog#languageRestartNotice QPushButton#languageRestartOkay:pressed { background: #30C86A; border-color: #30C86A; }"
        "QDialog#languageRestartNotice QPushButton#languageRestartOkay:focus { border-color: #007C58; }")
        .arg(surface, text, secondary));

    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(20, 20, 20, 18);
    layout->setSpacing(18);
    auto *copy = new QVBoxLayout;
    copy->setSpacing(6);
    auto *title = new QLabel(tr("Language saved"), dialog);
    title->setObjectName(QStringLiteral("languageRestartTitle"));
    title->setTextFormat(Qt::PlainText);
    title->setWordWrap(true);
    title->setAlignment(Qt::AlignCenter);
    copy->addWidget(title);
    auto *message = new QLabel(tr("Restart UDrive to apply your new language."), dialog);
    message->setObjectName(QStringLiteral("languageRestartMessage"));
    message->setTextFormat(Qt::PlainText);
    message->setWordWrap(true);
    message->setAlignment(Qt::AlignCenter);
    copy->addWidget(message);
    layout->addLayout(copy);
    auto *okay = new QPushButton(tr("OK"), dialog);
    okay->setObjectName(QStringLiteral("languageRestartOkay"));
    okay->setFixedHeight(34);
    okay->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    okay->setDefault(true);
    okay->setCursor(Qt::PointingHandCursor);
    layout->addWidget(okay, 0, Qt::AlignHCenter);
    connect(okay, &QPushButton::clicked, dialog, &QDialog::accept);
    connect(dialog, &QDialog::finished, this, [this, dialog] {
        if (_languageRestartNotice == dialog) _languageRestartNotice.clear();
    });
    dialog->open();
}

GeneralSettings::~GeneralSettings()
{
    delete _ui;
}

void GeneralSettings::loadMiscSettings()
{
    QScopedValueRollback<bool> scope(_currentlyLoading, true);
    ConfigFile cfgFile;
    if (_appearanceWidget) {
        _appearanceWidget->setThemeMode(cfgFile.themeMode(), false);
        _appearanceWidget->updateSystemAppearance();
        _appearanceWidget->updateThemeStyles();
    }
    _ui->monoIconsCheckBox->setChecked(cfgFile.monoIcons());
    _ui->desktopNotificationsCheckBox->setChecked(cfgFile.optionalDesktopNotifications());
    _ui->crashreporterCheckBox->setChecked(cfgFile.crashReporter());
    _ui->monoIconsCheckBox->setChecked(cfgFile.monoIcons());

    // the dropdown has to be populated before we can can pick an entry below based on the stored setting
    loadLanguageNamesIntoDropdown();

    QString locale = cfgFile.uiLanguage();
    if (Theme::instance()->appNameGUI() == QStringLiteral("UDrive"))
        locale = Translations::udriveLocale(locale.isEmpty() ? QLocale::system().uiLanguages().value(0, QLocale::system().name()) : locale);
    const auto index = _ui->languageDropdown->findData(locale);
    _ui->languageDropdown->setCurrentIndex(index < 0 ? 0 : index);
}

void GeneralSettings::showEvent(QShowEvent *event)
{
    QScopedValueRollback<bool> loading(_currentlyLoading, true);
    reloadConfig();
    if (_appearanceWidget) {
        _appearanceWidget->setThemeMode(ConfigFile().themeMode(), false);
        _appearanceWidget->updateSystemAppearance();
        _appearanceWidget->updateThemeStyles();
    }
    QWidget::showEvent(event);
}

void GeneralSettings::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange) {
        _ui->retranslateUi(this);
        loadLanguageNamesIntoDropdown();
        ConfigFile cfg;
        QString locale = cfg.uiLanguage();
        if (Theme::instance()->appNameGUI() == QStringLiteral("UDrive"))
            locale = Translations::udriveLocale(locale.isEmpty() ? QLocale::system().uiLanguages().value(0, QLocale::system().name()) : locale);
        const auto index = _ui->languageDropdown->findData(locale);
        _ui->languageDropdown->setCurrentIndex(index < 0 ? 0 : index);
        if (_appearanceWidget) {
            _appearanceWidget->retranslate();
        }
    }
    QWidget::changeEvent(event);
}

void GeneralSettings::saveMiscSettings()
{
    if (_currentlyLoading)
        return;
    ConfigFile cfgFile;
    bool isChecked = _ui->monoIconsCheckBox->isChecked();
    cfgFile.setMonoIcons(isChecked);
    Theme::instance()->setSystrayUseMonoIcons(isChecked);
    cfgFile.setCrashReporter(_ui->crashreporterCheckBox->isChecked());

    // UDrive stores an explicit supported locale; upstream can also store its default entry.
    const QString pickedLocale = _ui->languageDropdown->currentData().toString();
    cfgFile.setUiLanguage(pickedLocale);
    UDriveToast::preferenceSaved(this);
}

void GeneralSettings::slotToggleLaunchOnStartup(bool enable)
{
    Theme *theme = Theme::instance();
    Utility::setLaunchOnStartup(theme->appName(), theme->appNameGUI(), enable);
}

void GeneralSettings::slotToggleOptionalDesktopNotifications(bool enable)
{
    ConfigFile cfgFile;
    cfgFile.setOptionalDesktopNotifications(enable);
    if (!_currentlyLoading) UDriveToast::preferenceSaved(this);
}

void GeneralSettings::slotIgnoreFilesEditor()
{
    if (_ignoreEditor.isNull()) {
        _ignoreEditor = new IgnoreListEditor(ocApp()->gui()->settingsDialog());
        _ignoreEditor->setAttribute(Qt::WA_DeleteOnClose, true);
        ownCloudGui::raise();
        _ignoreEditor->open();
    }
}

void GeneralSettings::reloadConfig()
{
    _ui->syncHiddenFilesCheckBox->setChecked(!FolderMan::instance()->ignoreHiddenFiles());
    _ui->moveToTrashCheckBox->setChecked(ConfigFile().moveToTrash());
    if (Utility::hasSystemLaunchOnStartup(Theme::instance()->appName())) {
        _ui->autostartCheckBox->setChecked(true);
        _ui->autostartCheckBox->setDisabled(true);
        _ui->autostartCheckBox->setToolTip(tr("You cannot disable autostart because system-wide autostart is enabled."));
    } else {
        const bool hasAutoStart = Utility::hasLaunchOnStartup(Theme::instance()->appName());
        // make sure the binary location is correctly set
        slotToggleLaunchOnStartup(hasAutoStart);
        _ui->autostartCheckBox->setChecked(hasAutoStart);
        connect(_ui->autostartCheckBox, &QAbstractButton::toggled, this, &GeneralSettings::slotToggleLaunchOnStartup);
    }
}

void GeneralSettings::loadLanguageNamesIntoDropdown()
{
    // allow method to be called more than once
    _ui->languageDropdown->clear();

    if (Theme::instance()->appNameGUI() == QStringLiteral("UDrive")) {
        _ui->languageDropdown->setProperty("udriveCombo", true);
        // Keep native language names recognizable regardless of the active translation.
        _ui->languageDropdown->addItem(QStringLiteral("English"), QStringLiteral("en_US"));
        _ui->languageDropdown->addItem(QStringLiteral("ភាសាខ្មែរ - Khmer"), QStringLiteral("km"));
        _ui->languageDropdown->addItem(QStringLiteral("中文 - Chinese"), QStringLiteral("zh_CN"));
        return;
    }

    // if no option has been chosen explicitly by the user, the first entry shall be used
    _ui->languageDropdown->addItem(tr("(use default)"));

    // initialize map of locales to language names
    const auto availableLocales = []() {
        auto rv = Translations::listAvailableTranslations().values();
        rv.sort(Qt::CaseInsensitive);
        return rv;
    }();

    for (const auto &availableLocale : availableLocales) {
        auto nativeLanguageName = QLocale(availableLocale).nativeLanguageName();

        // fallback if there's a locale whose name Qt doesn't know
        // this indicates a broken filename
        if (nativeLanguageName.isEmpty()) {
            qCDebug(lcApplication()) << "Warning: could not find native language name for locale" << availableLocale;
            nativeLanguageName = tr("unknown (%1)").arg(availableLocale);
        }

        QString entryText = QStringLiteral("%1 (%2)").arg(nativeLanguageName, availableLocale);
        _ui->languageDropdown->addItem(entryText, availableLocale);
    }
}

} // namespace OCC
