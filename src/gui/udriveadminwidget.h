// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "udriveadminapi.h"
#include <QWidget>

class QTabBar;
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
    QLineEdit *takeSearchField();
    bool canCreate() const;
    QString creationLabel() const;
    void createNew();
Q_SIGNALS:
    void availabilityChanged();
    void controlsChanged();
protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
private:
    UDriveAdminApi::Resource resource() const;
    QJsonObject selected() const;
    void refresh();
    void updateAccess();
    void updateActions();
    void render();
    void edit(bool create);
    void removeSelected();
    void toggleLogin();
    void editRole();
    void editMembers();
    void showActions(const QPoint &point);
    void showRowActions(const QModelIndex &index, const QPoint &globalPosition);
    UDriveAdminApi *_api;
    QPointer<AccountState> _state;
    QTabBar *_tabs;
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
    QJsonArray _records;
    bool _refreshAfterAccess = false;
    bool _reloadAfterWrite = false;
    bool _hadAccess = false;
};
}
