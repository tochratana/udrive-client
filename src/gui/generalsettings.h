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

#ifndef MIRALL_GENERALSETTINGS_H
#define MIRALL_GENERALSETTINGS_H

#include "owncloudguilib.h"

#include <QMap>
#include <QWidget>
#include <QPointer>

class QDialog;

namespace OCC {
class IgnoreListEditor;
class SyncLogDialog;
class AppearanceWidget;

namespace Ui {
    class GeneralSettings;
}

/**
 * @brief The GeneralSettings class
 * @ingroup gui
 */
class OWNCLOUDGUI_EXPORT GeneralSettings : public QWidget
{
    Q_OBJECT

public:
    explicit GeneralSettings(QWidget *parent = nullptr);
    ~GeneralSettings() override;

Q_SIGNALS:
    void showAbout();
    void syncOptionsChanged();

private Q_SLOTS:
    void saveMiscSettings();
    void slotToggleLaunchOnStartup(bool);
    void slotToggleOptionalDesktopNotifications(bool);
    void slotIgnoreFilesEditor();
    void loadMiscSettings();

protected:
    void showEvent(QShowEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void reloadConfig();
    void loadLanguageNamesIntoDropdown();
    void showLanguageRestartNotice();

    Ui::GeneralSettings *_ui;
    QPointer<AppearanceWidget> _appearanceWidget;
    QPointer<IgnoreListEditor> _ignoreEditor;
    QPointer<QDialog> _languageRestartNotice;
    bool _currentlyLoading;
};


} // namespace OCC
#endif // MIRALL_GENERALSETTINGS_H
