// SPDX-License-Identifier: GPL-2.0-or-later
#include "udriveadminapi.h"
#include "accountstate.h"
#include "creds/localdevelopment.h"
#include "libsync/account.h"
#include "libsync/creds/abstractcredentials.h"
#include "libsync/networkjobs.h"
#include "libsync/httplogger.h"
#include "common/utility.h"
#include <QJsonDocument>
#include <QBuffer>
#include <QNetworkReply>
#include <QUrlQuery>
#include <algorithm>

using namespace OCC;
namespace {
bool validId(const QString &id)
{
    return !id.isEmpty() && id.size() <= 512 && id != QStringLiteral(".") && id != QStringLiteral("..")
        && std::none_of(id.cbegin(), id.cend(), [](QChar c) {
            return c.unicode() < 33 || c.unicode() == 127 || QStringLiteral("/\\%?#").contains(c);
        });
}
bool sameOrigin(const QUrl &a, const QUrl &b)
{
    return a.scheme() == b.scheme() && a.host() == b.host() && a.port() == b.port()
        && a.userInfo().isEmpty() && b.userInfo().isEmpty() && !a.hasFragment();
}
class AdminJob : public SimpleNetworkJob
{
public:
    using SimpleNetworkJob::SimpleNetworkJob;
    using AbstractNetworkJob::setQuery;
    // Do not replay writes on timeouts, transport errors, or redirects. A single
    // explicit 401 may refresh the existing account's OAuth credentials.
    bool needsRetry() const override
    {
        return _attempts == 1 && reply() && httpStatusCode() == 401
            && reply()->error() == QNetworkReply::AuthenticationRequiredError
            && reply()->attribute(QNetworkRequest::RedirectionTargetAttribute).isNull() && SimpleNetworkJob::needsRetry();
    }
protected:
    void newReplyHook(QNetworkReply *reply) override { ++_attempts; SimpleNetworkJob::newReplyHook(reply); }
private:
    int _attempts = 0;
};
}

UDriveAdminApi::UDriveAdminApi(AccountState *state, QObject *parent, bool loadRoles) : QObject(parent), _state(state), _loadRoles(loadRoles)
{
    if (state && state->account()) {
        connect(state, &AccountState::stateChanged, this, [this] {
            if (!authenticated()) clear();
            else if (!_busy && _selfId.isEmpty()) refreshAccess();
        });
        connect(state->account(), &Account::credentialsFetched, this, [this] { if (!_busy) refreshAccess(); });
        connect(state, &QObject::destroyed, this, &UDriveAdminApi::clear);
    }
}
UDriveAdminApi::~UDriveAdminApi()
{
    // A parent widget may already have destroyed its controls. Cancel silently
    // during destruction rather than emitting view updates into that teardown.
    const auto job = _job; _job.clear();
    if (job) job->abort();
}
bool UDriveAdminApi::authenticated() const
{
    return _state && !_state->isSignedOut() && _state->account() && _state->account()->credentials()
        && _state->account()->credentials()->ready() && LocalDevelopment::allowsServerUrl(_state->account()->url());
}
bool UDriveAdminApi::has(const char *permission) const { return authenticated() && _permissions.contains(QString::fromLatin1(permission)); }
bool UDriveAdminApi::canRead(Resource resource) const
{
    return resource == Resource::Users ? has("Accounts.ReadWrite.all")
        : resource == Resource::Groups ? has("Groups.ReadWrite.all") : has("Drives.List.all");
}
bool UDriveAdminApi::available() const { return canRead(Resource::Users) || canRead(Resource::Groups) || canRead(Resource::Workspaces); }
QJsonObject UDriveAdminApi::userCapabilities() const
{
    return _state && _state->account() ? QJsonObject::fromVariantMap(_state->account()->capabilities().raw()
        .value(QStringLiteral("graph")).toMap().value(QStringLiteral("users")).toMap()) : QJsonObject();
}
bool UDriveAdminApi::userFieldWritable(const QString &field) const
{
    const QString attribute = QStringLiteral("user.") + field;
    return canRead(Resource::Users) && !userCapabilities().value(QStringLiteral("read_only_attributes")).toArray().contains(attribute);
}
bool UDriveAdminApi::canCreate(Resource resource) const
{
    return resource == Resource::Users ? canRead(resource) && !userCapabilities().value(QStringLiteral("create_disabled")).toBool()
        : resource == Resource::Groups ? canRead(resource) : canRead(resource) && has("Drives.Create.all");
}
bool UDriveAdminApi::canUpdate(Resource resource) const
{
    return resource == Resource::Workspaces ? canRead(resource) && has("Drives.ReadWriteEnabled.all") : canRead(resource);
}
bool UDriveAdminApi::canDelete(Resource resource) const
{
    return resource == Resource::Users ? canRead(resource) && !userCapabilities().value(QStringLiteral("delete_disabled")).toBool() : canUpdate(resource);
}
bool UDriveAdminApi::canSetQuota() const { return canRead(Resource::Workspaces) && has("Drives.ReadWriteProjectQuota.all"); }
bool UDriveAdminApi::canAssignRoles() const { return has("Roles.ReadWrite.all") && userFieldWritable(QStringLiteral("appRoleAssignments")); }
QString UDriveAdminApi::collectionName(Resource resource) const
{
    return resource == Resource::Users ? QStringLiteral("users") : resource == Resource::Groups ? QStringLiteral("groups") : QStringLiteral("drives");
}
QUrl UDriveAdminApi::graphUrl(const QString &path) const
{
    return _state && _state->account() ? Utility::concatUrlPath(_state->account()->url(), QStringLiteral("graph/v1.0/") + path) : QUrl();
}
bool UDriveAdminApi::begin()
{
    if (_busy || !authenticated()) return false;
    _busy = true; Q_EMIT busyChanged(); return true;
}
void UDriveAdminApi::finish(bool success, const QString &message)
{
    if (!success && _checkingAccess) {
        _permissions.clear(); _roles = {}; _selfId.clear(); Q_EMIT cleared(); Q_EMIT accessChanged();
    }
    _checkingAccess = false;
    _busy = false; Q_EMIT busyChanged(); Q_EMIT operationFinished(success, message);
}
void UDriveAdminApi::clear()
{
    const auto job = _job; _job.clear();
    if (job) { job->abort(); job->deleteLater(); }
    _busy = false; _checkingAccess = false; _permissions.clear(); _selfId.clear(); _roles = {};
    Q_EMIT cleared(); Q_EMIT busyChanged(); Q_EMIT accessChanged();
}
void UDriveAdminApi::request(const QByteArray &verb, const QUrl &url, const QJsonObject &body, Callback callback, bool restore)
{
    if (!authenticated() || !sameOrigin(url, _state->account()->url())) {
        finish(false, tr("The administration request could not be sent safely.")); return;
    }
    QNetworkRequest headers;
    headers.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    headers.setAttribute(HttpLogger::OmitBodyAttribute, true);
    headers.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    if (restore) headers.setRawHeader("Restore", "T");
    QUrl base = url; base.setQuery(QString());
    QBuffer *buffer = nullptr;
    if (verb != "GET" && verb != "DELETE") {
        buffer = new QBuffer;
        buffer->setData(QJsonDocument(body).toJson(QJsonDocument::Compact));
        buffer->open(QIODevice::ReadOnly);
    }
    auto *job = new AdminJob(_state->account(), base, QString(), verb, buffer, headers, this);
    if (buffer) buffer->setParent(job);
    job->setQuery(QUrlQuery(url));
    job->setCacheLoadControl(QNetworkRequest::AlwaysNetwork);
    job->setTimeout(std::chrono::seconds(30));
    _job = job;
    job->addNewReplyHook([job](QNetworkReply *reply) {
        QObject::connect(reply, &QIODevice::readyRead, job, [job, reply] { if (reply->bytesAvailable() > 8 * 1024 * 1024) job->abort(); });
    });
    connect(job, &SimpleNetworkJob::finishedSignal, this, [this, job, callback = std::move(callback)] {
        if (_job != job) return;
        _job.clear(); job->deleteLater();
        if (!authenticated()) { clear(); return; }
        const int status = job->httpStatusCode();
        if (!job->reply() || job->reply()->error() != QNetworkReply::NoError || status < 200 || status >= 300) {
            if (status == 401 || status == 403) { _permissions.clear(); _roles = {}; Q_EMIT cleared(); Q_EMIT accessChanged(); }
            finish(false, status == 403 ? tr("The server denied this administration request. Your permissions may have changed.")
                : status == 409 ? tr("This entry already exists or changed on the server. Refresh and try again.")
                : status >= 300 && status < 400 ? tr("The server redirected this request. No administration change was retried.")
                : tr("The administration request failed (HTTP %1). Refresh to check the server before trying again.").arg(status));
            return;
        }
        const auto bytes = job->reply()->readAll();
        QJsonParseError error;
        const auto document = bytes.isEmpty() ? QJsonDocument(QJsonObject()) : QJsonDocument::fromJson(bytes, &error);
        if (bytes.size() > 8 * 1024 * 1024 || !document.isObject() || error.error != QJsonParseError::NoError) {
            finish(false, tr("The server returned an invalid administration response.")); return;
        }
        callback(document.object());
    });
    job->start();
}
void UDriveAdminApi::refreshAccess()
{
    if (!begin()) return;
    _checkingAccess = true;
    request("GET", graphUrl(QStringLiteral("me")), {}, [this](const QJsonObject &me) {
        const QString id = me.value(QStringLiteral("id")).toString();
        if (!validId(id)) { clear(); finish(false, tr("Could not verify the signed-in account.")); return; }
        _selfId = id;
        const auto url = Utility::concatUrlPath(_state->account()->url(), QStringLiteral("api/v0/settings/permissions-list"));
        request("POST", url, {{QStringLiteral("account_uuid"), id}}, [this](const QJsonObject &data) {
            _permissions.clear(); _roles = {};
            if (!data.value(QStringLiteral("permissions")).isArray()) {
                Q_EMIT accessChanged(); finish(false, tr("The server did not provide administration permissions.")); return;
            }
            for (const auto &permission : data.value(QStringLiteral("permissions")).toArray())
                if (permission.isString()) _permissions.insert(permission.toString());
            Q_EMIT accessChanged();
            if (!_loadRoles || !canRead(Resource::Users)) { finish(true); return; }
            // App roles are supplied by the server; never infer an administrator
            // from a username, token payload, or a workspace Manager grant.
            request("GET", graphUrl(QStringLiteral("applications")), {}, [this](const QJsonObject &apps) {
                for (const auto &entry : apps.value(QStringLiteral("value")).toArray()) {
                    const auto application = entry.toObject();
                    const QString id = application.value(QStringLiteral("id")).toString();
                    if (!validId(id)) continue;
                    for (const auto &entry : application.value(QStringLiteral("appRoles")).toArray()) {
                        auto role = entry.toObject();
                        if (!validId(role.value(QStringLiteral("id")).toString()) || role.value(QStringLiteral("displayName")).toString().isEmpty()
                            || !role.value(QStringLiteral("isEnabled")).toBool(true) || _roles.size() >= 1000) continue;
                        role.insert(QStringLiteral("resourceId"), id); _roles.append(role);
                    }
                }
                finish(true);
            });
        });
    });
}
void UDriveAdminApi::collection(const QUrl &url, const QUrl &first, QJsonArray records, int page, std::function<void(const QJsonArray &)> done)
{
    if (page >= 100 || !sameOrigin(url, first) || url.path() != first.path()) {
        finish(false, tr("The server returned an unsafe or excessive pagination link.")); return;
    }
    request("GET", url, {}, [this, url, first, records, page, done](const QJsonObject &data) mutable {
        if (!data.value(QStringLiteral("value")).isArray()) { finish(false, tr("The server returned an invalid list.")); return; }
        for (const auto &entry : data.value(QStringLiteral("value")).toArray()) {
            if (!entry.isObject() || !validId(entry.toObject().value(QStringLiteral("id")).toString()) || records.size() >= 10000) {
                finish(false, tr("The administration list is invalid or exceeds 10,000 entries.")); return;
            }
            records.append(entry);
        }
        const QString next = data.value(QStringLiteral("@odata.nextLink")).toString();
        if (!next.isEmpty()) { collection(url.resolved(QUrl(next)), first, records, page + 1, done); return; }
        done(records);
    });
}
void UDriveAdminApi::load(Resource resource)
{
    if (!canRead(resource) || !begin()) return;
    QUrl url = graphUrl(collectionName(resource));
    if (resource == Resource::Users) {
        QUrlQuery query; query.addQueryItem(QStringLiteral("$expand"), QStringLiteral("appRoleAssignments,memberOf,drive")); url.setQuery(query);
    }
    collection(url, url, {}, 0, [this, resource](const QJsonArray &records) {
        finish(true); Q_EMIT recordsLoaded(resource, records);
    });
}
bool UDriveAdminApi::save(Resource resource, const QString &id, const QJsonObject &fields)
{
    if (fields.isEmpty() || (!id.isEmpty() && !validId(id)) || !(id.isEmpty() ? canCreate(resource) : canUpdate(resource) || (resource == Resource::Workspaces && canSetQuota()))) return false;
    const QStringList allowed = resource == Resource::Users
        ? QStringList{QStringLiteral("onPremisesSamAccountName"), QStringLiteral("displayName"), QStringLiteral("mail"), QStringLiteral("passwordProfile")}
        : resource == Resource::Groups ? QStringList{QStringLiteral("displayName"), QStringLiteral("description")}
        : QStringList{QStringLiteral("name"), QStringLiteral("description"), QStringLiteral("quota")};
    for (auto field = fields.begin(); field != fields.end(); ++field) {
        if (!allowed.contains(field.key()) || (resource == Resource::Users && !id.isEmpty() && !userFieldWritable(field.key()))
            || (resource == Resource::Workspaces && !id.isEmpty() && (field.key() == QStringLiteral("quota") ? !canSetQuota() : !canUpdate(resource)))) return false;
    }
    auto payload = fields;
    if (resource == Resource::Workspaces && id.isEmpty()) payload.insert(QStringLiteral("driveType"), QStringLiteral("project"));
    if (!begin()) return false;
    request(id.isEmpty() ? "POST" : "PATCH", graphUrl(collectionName(resource) + (id.isEmpty() ? QString() : QStringLiteral("/") + id)), payload,
        [this](const QJsonObject &) { finish(true, tr("Changes saved.")); });
    return true;
}
bool UDriveAdminApi::remove(Resource resource, const QString &id)
{
    if (!validId(id) || !canDelete(resource) || (resource == Resource::Users && id == _selfId) || !begin()) return false;
    request("DELETE", graphUrl(collectionName(resource) + QStringLiteral("/") + id), {},
        [this](const QJsonObject &) { finish(true, tr("Changes saved.")); }); return true;
}
bool UDriveAdminApi::setLogin(const QString &id, bool enabled)
{
    if (!validId(id) || id == _selfId || !userFieldWritable(QStringLiteral("accountEnabled")) || !begin()) return false;
    request("PATCH", graphUrl(QStringLiteral("users/") + id), {{QStringLiteral("accountEnabled"), enabled}},
        [this](const QJsonObject &) { finish(true, tr("Login access updated.")); }); return true;
}
bool UDriveAdminApi::assignRole(const QString &id, const QString &roleId, const QString &applicationId)
{
    if (!validId(id) || id == _selfId || !canAssignRoles()) return false;
    const bool known = std::any_of(_roles.cbegin(), _roles.cend(), [&](const QJsonValue &role) {
        return role.toObject().value(QStringLiteral("id")).toString() == roleId && role.toObject().value(QStringLiteral("resourceId")).toString() == applicationId;
    });
    if (!known || !begin()) return false;
    request("POST", graphUrl(QStringLiteral("users/") + id + QStringLiteral("/appRoleAssignments")),
        {{QStringLiteral("principalId"), id}, {QStringLiteral("appRoleId"), roleId}, {QStringLiteral("resourceId"), applicationId}},
        [this](const QJsonObject &) { finish(true, tr("User role updated.")); }); return true;
}
bool UDriveAdminApi::setGroupMember(const QString &groupId, const QString &userId, bool member)
{
    if (!validId(groupId) || !validId(userId) || !canUpdate(Resource::Groups) || !begin()) return false;
    const QString path = QStringLiteral("groups/") + groupId + QStringLiteral("/members/") + (member ? QString() : userId + QStringLiteral("/")) + QStringLiteral("$ref");
    request(member ? "POST" : "DELETE", graphUrl(path), member ? QJsonObject{{QStringLiteral("@odata.id"), graphUrl(QStringLiteral("users/") + userId).toString()}} : QJsonObject(),
        [this](const QJsonObject &) { finish(true, tr("Group membership updated.")); }); return true;
}
void UDriveAdminApi::loadMembers(const QString &groupId)
{
    if (!validId(groupId) || !canRead(Resource::Groups) || !begin()) return;
    const QUrl url = graphUrl(QStringLiteral("groups/") + groupId + QStringLiteral("/members"));
    collection(url, url, {}, 0, [this](const QJsonArray &records) { finish(true); Q_EMIT membersLoaded(records); });
}
bool UDriveAdminApi::restoreWorkspace(const QString &id, const QString &name)
{
    if (!validId(id) || !canUpdate(Resource::Workspaces) || !begin()) return false;
    request("PATCH", graphUrl(QStringLiteral("drives/") + id), {{QStringLiteral("name"), name}},
        [this](const QJsonObject &) { finish(true, tr("Workspace enabled.")); }, true); return true;
}
