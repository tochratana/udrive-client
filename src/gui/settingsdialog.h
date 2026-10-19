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

#pragma once

#include "accountstate.h"
#include "gui/qmlutils.h"
#include "owncloudgui.h"

#include <QMainWindow>
#include <QStyledItemDelegate>

class QStackedWidget;
class QPushButton;


namespace OCC {

namespace Ui {
    class SettingsDialog;
}
class AccountView;
class Application;
class FolderMan;
class ownCloudGui;
class ActivitySettings;
class GeneralSettings;
class UDriveFileBrowser;
class UDriveAdminWidget;
class NotificationBadgeButton;

/**
 * @brief The SettingsDialog class
 * @ingroup gui
 */
class OWNCLOUDGUI_EXPORT SettingsDialog : public QMainWindow
{
    Q_OBJECT
    Q_PROPERTY(SettingsPage currentPage READ currentPage WRITE setCurrentPage NOTIFY currentPageChanged)
    Q_PROPERTY(Account *currentAccount READ currentAccount WRITE setCurrentAccount NOTIFY currentAccountChanged)
    Q_PROPERTY(bool canCreateFiles READ canCreateFiles NOTIFY workspaceChanged)
    Q_PROPERTY(bool isWorkspacesOverview READ isWorkspacesOverview NOTIFY workspaceChanged)
    Q_PROPERTY(bool canAdminister READ canAdminister NOTIFY workspaceChanged)
    Q_PROPERTY(QString adminCreationLabel READ adminCreationLabel NOTIFY workspaceChanged)
    Q_PROPERTY(QString storageLabel READ storageLabel NOTIFY workspaceChanged)
    Q_PROPERTY(double storageFraction READ storageFraction NOTIFY workspaceChanged)
    Q_PROPERTY(bool sidebarVisible READ isSidebarVisible WRITE setSidebarVisible NOTIFY sidebarVisibilityChanged)
    QML_ELEMENT
    QML_UNCREATABLE("C++ only")
    OC_DECLARE_WIDGET_FOCUS
public:
    enum class SettingsPage { None, Activity, Settings, Account, Files, Workspaces, Shares, Favorites, Trash, Administration };
    Q_ENUM(SettingsPage)
    explicit SettingsDialog(ownCloudGui *gui, QWidget *parent = nullptr);
    ~SettingsDialog() override;

    void addModalWidget(QWidget *w);

    void requestModality(Account *account);
    void ceaseModality(Account *account);

    AccountView *accountView(Account *account) const;

    SettingsPage currentPage() const;

    void setCurrentPage(SettingsPage currentPage);

    void setCurrentAccount(Account *account);

    // todo: #37 - this is stickier to get rid of as it's used in the qml related to the account. That needs to go.
    Account *currentAccount() const;

public Q_SLOTS:
    // this is a direct call from QML
    void createNewAccount();
    void openNewFileMenu();
    bool canCreateFiles() const;
    bool isWorkspacesOverview() const;
    bool canAdminister() const;
    QString adminCreationLabel() const;
    QString storageLabel() const;
    double storageFraction() const;
    void runFolderWizard(Account *account);
    void toggleSidebar();
    void setSidebarVisible(bool visible);
    bool isSidebarVisible() const;


Q_SIGNALS:
    void workspaceChanged();
    void currentPageChanged();
    // I think this only goes to qml
    void currentAccountChanged();
    void sidebarVisibilityChanged(bool visible);

protected Q_SLOTS:
    void onAccountAdded(AccountState *state);
    void onAccountRemoved(AccountState *state);

protected:
    void setVisible(bool visible) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;


private:
    void updateUDriveAppearance();
    void createWorkspaceHeader();
    void updateWorkspaceHeader();
    void showAccountPopup();
    void showNotificationPopup();
    void updateNotificationBadge();
    QWidget *_brandWidget = nullptr;
    QPointer<QWidget> _accountPopup;
    QStackedWidget *_searchStack = nullptr;
    QPushButton *_profileButton = nullptr;
    NotificationBadgeButton *_notificationButton = nullptr;
    QPointer<QWidget> _notificationPopup;
    bool _sidebarVisible = true;
    Ui::SettingsDialog *const _ui;


    QHash<QUuid, AccountView *> _viewForAccount;
    QHash<QUuid, UDriveFileBrowser *> _filesForAccount;
    QHash<QUuid, UDriveAdminWidget *> _adminForAccount;
    QPointer<Account> _lastAccount;

    ActivitySettings *_activitySettings;
    QWidget *_activityPage = nullptr;
    ownCloudGui *_gui;
    QList<Account *> _modalStack;

    GeneralSettings *_generalSettings;
    QWidget *_settingsPage = nullptr;
    SettingsPage _currentPage = SettingsPage::None;
    // todo: #37
    Account *_currentAccount = nullptr;
};
}
