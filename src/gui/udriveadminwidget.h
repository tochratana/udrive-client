// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "udriveadminapi.h"
#include <QWidget>

class QTabBar;
class QScrollArea;
class QResizeEvent;
class QStackedWidget;
class QTreeView;
class QStandardItemModel;
class QSortFilterProxyModel;
class QLineEdit;
class QComboBox;
class QLabel;
class QPushButton;
class QModelIndex;
namespace OCC {
class OWNCLOUDGUI_EXPORT UDriveAdminWidget : public QWidget
{
    Q_OBJECT
public:
    explicit UDriveAdminWidget(AccountState *state, QWidget *parent = nullptr);
    UDriveAdminApi *api() const { return _api; }
    void activate();
    QLineEdit *searchField() const { return _search; }
    bool canCreate() const;
    QString creationLabel() const;
    void createNew();
Q_SIGNALS:
    void availabilityChanged();
    void controlsChanged();
protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
private:
    UDriveAdminApi::Resource resource() const;
    QJsonObject selected() const;
    void refresh();
    void updateAccess();
    void updateActions();
    void render();
    void showDetails();
    void buildDetails();
    void updateDetails();
    void closeDetails();
    void positionDetails();
    void edit(bool create);
    void buildUserEditor(QWidget *parent);
    void openUserEditor();
    void resetUserEditor();
    void leaveUserEditor();
    void updateUserEditorAccess();
    void saveUserEditor();
    void sendNextUserEdit();
    void userEditFinished(bool success, const QString &message);
    void removeSelected();
    void toggleLogin();
    void editRole();
    void editMembers();
    void showActions(const QPoint &point);
    void showRowActions(const QModelIndex &index, const QPoint &globalPosition);
    UDriveAdminApi *_api;
    QPointer<AccountState> _state;
    QTabBar *_tabs;
    QPushButton *_section;
    QWidget *_filterLabel;
    QWidget *_selectionActions;
    QTreeView *_tree;
    QStandardItemModel *_model;
    QSortFilterProxyModel *_proxy;
    QLineEdit *_search;
    QComboBox *_groupFilter;
    QComboBox *_roleFilter;
    QLabel *_status;
    QLabel *_count;
    QWidget *_empty;
    QPushButton *_edit;
    QPushButton *_role;
    QPushButton *_members;
    QPushButton *_login;
    QPushButton *_remove;
    QPushButton *_refresh;
    QScrollArea *_detailsPane = nullptr;
    QLabel *_detailsAvatar;
    QLabel *_detailsUsername;
    QLabel *_detailsName;
    QList<QLabel *> _detailsValues;
    QPushButton *_detailsEdit;
    bool _detailsOpen = false;
    QLabel *_detailsTitle;
    QPushButton *_detailsBack;
    QStackedWidget *_detailsStack;
    QWidget *_userEditor;
    QMap<QString, QLineEdit *> _userInputs;
    QComboBox *_userRole;
    QComboBox *_userLogin;
    QLineEdit *_userQuota;
    QLabel *_userEditError;
    QPushButton *_userSave;
    QPushButton *_userCancel;
    QJsonObject _userEditRecord;
    QJsonArray _userEdits;
    QJsonObject _userEditPending;
    QString _reopenDetailsId;
    bool _userEditorOpen = false;
    bool _userSaving = false;
    int _userSavedSteps = 0;
    QJsonArray _records;
    bool _refreshAfterAccess = false;
    bool _reloadAfterWrite = false;
    bool _hadAccess = false;
};
}
