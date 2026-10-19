// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "gui/owncloudguilib.h"
#include <QObject>
#include <QPointer>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QUrl>
#include <functional>

namespace OCC {
class AccountState;
class SimpleNetworkJob;

// Server permissions, not a local role name, authorize each operation. No admin
// data is persisted, and each instance belongs to exactly one signed-in account.
class OWNCLOUDGUI_EXPORT UDriveAdminApi : public QObject
{
    Q_OBJECT
public:
    enum class Resource { Users, Groups, Workspaces };
    Q_ENUM(Resource)
    explicit UDriveAdminApi(AccountState *state, QObject *parent = nullptr);
    ~UDriveAdminApi() override;
    bool available() const;
    bool busy() const { return _busy; }
    bool canRead(Resource resource) const;
    bool canCreate(Resource resource) const;
    bool canUpdate(Resource resource) const;
    bool canDelete(Resource resource) const;
    bool canSetQuota() const;
    bool canAssignRoles() const;
    bool userFieldWritable(const QString &field) const;
    QString selfId() const { return _selfId; }
    QJsonArray roles() const { return _roles; }
    QUrl graphUrl(const QString &path) const;
    void refreshAccess();
    void clear();
    void load(Resource resource);
    bool save(Resource resource, const QString &id, const QJsonObject &fields);
    bool remove(Resource resource, const QString &id);
    bool setLogin(const QString &id, bool enabled);
    bool assignRole(const QString &id, const QString &roleId, const QString &applicationId);
    bool setGroupMember(const QString &groupId, const QString &userId, bool member);
    void loadMembers(const QString &groupId);
    bool restoreWorkspace(const QString &id, const QString &name);
Q_SIGNALS:
    void accessChanged();
    void busyChanged();
    void cleared();
    void recordsLoaded(OCC::UDriveAdminApi::Resource resource, const QJsonArray &records);
    void membersLoaded(const QJsonArray &records);
    void operationFinished(bool success, const QString &message);
private:
    using Callback = std::function<void(const QJsonObject &)>;
    bool authenticated() const;
    bool begin();
    void finish(bool success, const QString &message = {});
    void request(const QByteArray &verb, const QUrl &url, const QJsonObject &body, Callback callback, bool restore = false);
    void collection(const QUrl &url, const QUrl &first, QJsonArray records, int page, std::function<void(const QJsonArray &)> done);
    QJsonObject userCapabilities() const;
    QString collectionName(Resource resource) const;
    bool has(const char *permission) const;
    QPointer<AccountState> _state;
    QPointer<SimpleNetworkJob> _job;
    QSet<QString> _permissions;
    QString _selfId;
    QJsonArray _roles;
    bool _busy = false;
    bool _checkingAccess = false;
};
}
