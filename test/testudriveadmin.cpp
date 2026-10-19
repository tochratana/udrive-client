// SPDX-License-Identifier: GPL-2.0-or-later
#include "gui/udriveadminapi.h"
#include "gui/udriveadminwidget.h"
#include "gui/accountmanager.h"
#include "gui/accountstate.h"
#include "gui/settingsdialog.h"
#include "gui/folderman.h"
#include "gui/creds/credentials.h"
#include "libsync/account.h"
#include "libsync/configfile.h"
#include "resources/resources.h"
#include <QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QPushButton>
#include <QTreeView>
#include <QLineEdit>
#include <QTabBar>
#include <QDialog>
#include <QDialogButtonBox>
#include <QComboBox>
#include <QListWidget>
#include <QScopeGuard>
#include <QMenu>

using namespace OCC;
using Resource = UDriveAdminApi::Resource;
namespace {
class TestCredentials : public Credentials {
public:
    using Credentials::Credentials;
    void persist() override {}
    bool refreshAccessToken() override { return false; } // No external OAuth server in this fixture.
};
class AdminServer : public QTcpServer
{
public:
    struct Request { QByteArray verb; QUrl url; QJsonObject body; QMap<QByteArray, QByteArray> headers; };
    QList<Request> requests;
    QJsonArray permissions{"Accounts.ReadWrite.all", "Groups.ReadWrite.all", "Roles.ReadWrite.all", "Drives.List.all", "Drives.Create.all", "Drives.ReadWriteEnabled.all", "Drives.ReadWriteProjectQuota.all"};
    int accessStatus = 200, writeStatus = 200, delay = 0;
    bool malformedPermissions = false, malformedList = false;
    QString nextLink, redirect;
    QJsonArray users{
        QJsonObject{{"id", "self"}, {"onPremisesSamAccountName", "admin"}, {"displayName", "Administrator"}, {"mail", "admin@example.test"},
            {"appRoleAssignments", QJsonArray{QJsonObject{{"appRoleId", "admin-role"}, {"resourceId", "application"}}}}},
        QJsonObject{{"id", "other"}, {"onPremisesSamAccountName", "alice"}, {"displayName", "Alice"}, {"mail", "alice@example.test"},
            {"memberOf", QJsonArray{QJsonObject{{"id", "team"}, {"displayName", "Design"}}}}, {"accountEnabled", false},
            {"appRoleAssignments", QJsonArray{QJsonObject{{"appRoleId", "user-role"}, {"resourceId", "application"}}}}}
    };
    QJsonArray groupMembers;
    AdminServer()
    {
        groupMembers = {users.at(1)};
        listen(QHostAddress::LocalHost, 0);
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection(); auto bytes = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, bytes] {
                    *bytes += socket->readAll(); const int end = bytes->indexOf("\r\n\r\n"); if (end < 0) return;
                    const auto lines = bytes->left(end).split('\n'); const auto start = lines.first().trimmed().split(' ');
                    Request req; req.verb = start.at(0); req.url = QUrl::fromEncoded(start.at(1));
                    for (const auto &line : lines) { const int colon = line.indexOf(':'); if (colon > 0) req.headers.insert(line.left(colon).trimmed().toLower(), line.mid(colon + 1).trimmed()); }
                    const int size = req.headers.value("content-length").toInt(); if (bytes->size() < end + 4 + size) return;
                    req.body = QJsonDocument::fromJson(bytes->mid(end + 4, size)).object(); requests.append(req); socket->disconnect(this);
                    const QPointer<QTcpSocket> guard(socket);
                    QTimer::singleShot(delay, this, [this, guard, req] { if (guard && guard->state() == QAbstractSocket::ConnectedState) respond(guard, req); });
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }
    QUrl url(const QString &path = {}) const { QUrl result(QStringLiteral("http://127.0.0.1:%1").arg(serverPort())); result.setPath(path); return result; }
    Account *account()
    {
        auto *account = new Account(QUuid::createUuid(), QStringLiteral("admin-test"), url());
        account->setCredentials(new TestCredentials("admin-test-token", "test-refresh", account)); account->credentials()->fetchFromKeychain(); return account;
    }
    void reply(QTcpSocket *socket, int status, const QJsonObject &value = {}, const QByteArray &headers = {})
    {
        const auto body = status == 204 ? QByteArray() : QJsonDocument(value).toJson(QJsonDocument::Compact);
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " Result\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n" + headers + "\r\n" + body);
        socket->disconnectFromHost();
    }
    void respond(QTcpSocket *socket, const Request &request)
    {
        const auto path = request.url.path();
        if (path == QStringLiteral("/graph/v1.0/me")) { reply(socket, accessStatus, {{"id", "self"}}); return; }
        if (path == QStringLiteral("/api/v0/settings/permissions-list")) {
            reply(socket, accessStatus, malformedPermissions ? QJsonObject{{"role", "Admin"}} : QJsonObject{{"permissions", permissions}}); return;
        }
        if (path == QStringLiteral("/graph/v1.0/applications")) {
            reply(socket, 200, {{"value", QJsonArray{QJsonObject{{"id", "application"}, {"appRoles", QJsonArray{
                QJsonObject{{"id", "admin-role"}, {"displayName", "Admin"}}, QJsonObject{{"id", "user-role"}, {"displayName", "User"}}}}}}}}); return;
        }
        if (request.verb != "GET" && request.verb != "PROPFIND") {
            if (!redirect.isEmpty()) { reply(socket, 307, {}, "Location: " + redirect.toUtf8() + "\r\n"); return; }
            if (writeStatus == 200 && request.verb == "POST" && path == QStringLiteral("/graph/v1.0/users")) {
                auto created = request.body; created.remove(QStringLiteral("passwordProfile")); created.insert(QStringLiteral("id"), QStringLiteral("new-entry"));
                created.insert(QStringLiteral("appRoleAssignments"), QJsonArray{QJsonObject{{"appRoleId", "user-role"}, {"resourceId", "application"}}}); users.append(created);
            }
            if (writeStatus == 200 && request.verb == "POST" && path == QStringLiteral("/graph/v1.0/groups/team/members/$ref")) {
                const auto id = QUrl(request.body.value(QStringLiteral("@odata.id")).toString()).path().section(QLatin1Char('/'), -1);
                for (const auto &user : users) if (user.toObject().value(QStringLiteral("id")).toString() == id) groupMembers.append(user);
            }
            reply(socket, writeStatus, {{"id", "new-entry"}}); return;
        }
        if (path == QStringLiteral("/graph/v1.0/users")) {
            QJsonObject data{{"value", users}};
            if (!nextLink.isEmpty() && !request.url.hasQuery()) data.insert("@odata.nextLink", nextLink);
            else if (!nextLink.isEmpty() && !request.url.query().contains(QStringLiteral("skip"))) data.insert("@odata.nextLink", nextLink);
            if (malformedList) data.insert("value", QStringLiteral("invalid")); reply(socket, 200, data); return;
        }
        if (path == QStringLiteral("/graph/v1.0/groups")) { reply(socket, 200, {{"value", QJsonArray{QJsonObject{{"id", "team"}, {"displayName", "Design"}, {"description", "Design team"}}}}}); return; }
        if (path == QStringLiteral("/graph/v1.0/groups/team/members")) { reply(socket, 200, {{"value", groupMembers}}); return; }
        if (path == QStringLiteral("/graph/v1.0/drives")) {
            reply(socket, 200, {{"value", QJsonArray{QJsonObject{{"id", "workspace"}, {"name", "Design projects"}, {"driveType", "project"}, {"quota", QJsonObject{{"total", 15000000000LL}}}, {"root", QJsonObject{}}}}}}); return;
        }
        if (path == QStringLiteral("/graph/v1.0/me/drives")) { reply(socket, 200, {{"value", QJsonArray()}}); return; }
        reply(socket, 404);
    }
};
bool discovered(UDriveAdminApi &api)
{
    QSignalSpy done(&api, &UDriveAdminApi::operationFinished); api.refreshAccess();
    return (done.count() || done.wait(3000)) && !api.busy();
}
}
class TestUDriveAdmin : public QObject
{
    Q_OBJECT
    QTemporaryDir config;
    QByteArray previous;
    std::unique_ptr<FolderMan> folders;
private Q_SLOTS:
    void initTestCase()
    {
        previous = qgetenv("UDRIVE_ALLOW_LOCAL_HTTP"); qputenv("UDRIVE_ALLOW_LOCAL_HTTP", "1");
        ConfigFile::setConfDir(config.path()); Account::setCommonCacheDirectory(config.path() + QStringLiteral("/cache"));
        folders = FolderMan::createInstance(); Resources::loadUDriveFonts(); qRegisterMetaType<Resource>();
    }
    void cleanupTestCase() { if (previous.isNull()) qunsetenv("UDRIVE_ALLOW_LOCAL_HTTP"); else qputenv("UDRIVE_ALLOW_LOCAL_HTTP", previous); }
    void permissionsComeFromAuthenticatedServer()
    {
        AdminServer server; AccountState state(server.account()); UDriveAdminApi api(&state);
        QVERIFY(!api.available()); QVERIFY(!api.save(Resource::Users, {}, {{"displayName", "No"}}));
        QVERIFY(discovered(api)); QVERIFY(api.available()); QVERIFY(api.canAssignRoles()); QCOMPARE(api.selfId(), QStringLiteral("self"));
        const auto permissions = std::find_if(server.requests.cbegin(), server.requests.cend(), [](const AdminServer::Request &request) {
            return request.url.path() == QStringLiteral("/api/v0/settings/permissions-list");
        });
        QVERIFY(permissions != server.requests.cend()); QCOMPARE(permissions->body.value("account_uuid").toString(), QStringLiteral("self"));
        QCOMPARE(server.requests.first().headers.value("authorization"), QByteArray("Bearer admin-test-token"));
        server.permissions = {}; QVERIFY(discovered(api)); QVERIFY(!api.available());
        const int requests = server.requests.count();
        api.load(Resource::Users); QVERIFY(!api.save(Resource::Users, {}, {{"displayName", "No"}}));
        QVERIFY(!api.setLogin(QStringLiteral("other"), true)); QVERIFY(!api.assignRole(QStringLiteral("other"), QStringLiteral("admin-role"), QStringLiteral("application")));
        QCOMPARE(server.requests.count(), requests);
    }
    void cannotPromoteViaWorkspaceOrRoleLabel()
    {
        AdminServer server; server.permissions = {"Manager", "Admin", "Roles.ReadWrite.all"};
        AccountState state(server.account()); UDriveAdminApi api(&state); QVERIFY(discovered(api)); QVERIFY(!api.available());
        QVERIFY(!api.canRead(Resource::Users)); QVERIFY(!api.canCreate(Resource::Workspaces)); QVERIFY(api.roles().isEmpty());
    }
    void failedDiscoveryRevokesCachedAccess_data()
    {
        QTest::addColumn<int>("status"); for (int status : {401, 403, 404, 500}) QTest::newRow(qPrintable(QString::number(status))) << status;
    }
    void failedDiscoveryRevokesCachedAccess()
    {
        QFETCH(int, status); AdminServer server; AccountState state(server.account()); UDriveAdminApi api(&state); QVERIFY(discovered(api)); QVERIFY(api.available());
        server.accessStatus = status; QVERIFY(discovered(api)); QVERIFY(!api.available()); QVERIFY(api.roles().isEmpty());
    }
    void malformedPermissionResponseFailsClosed()
    {
        AdminServer server; server.malformedPermissions = true; AccountState state(server.account()); UDriveAdminApi api(&state);
        QVERIFY(discovered(api)); QVERIFY(!api.available());
    }
    void userChangesAndSelfProtection()
    {
        AdminServer server; AccountState state(server.account()); UDriveAdminApi api(&state); QVERIFY(discovered(api));
        QSignalSpy done(&api, &UDriveAdminApi::operationFinished);
        QVERIFY(api.save(Resource::Users, {}, {{"onPremisesSamAccountName", "new-user"}, {"displayName", "New User"}, {"passwordProfile", QJsonObject{{"password", "test-only-secret"}}}}));
        QTRY_COMPARE(done.count(), 1); QVERIFY(done.takeFirst().at(0).toBool()); QCOMPARE(server.requests.last().verb, QByteArray("POST"));
        QCOMPARE(server.requests.last().body.value("passwordProfile").toObject().value("password").toString(), QStringLiteral("test-only-secret"));
        QVERIFY(api.setLogin(QStringLiteral("other"), false)); QTRY_COMPARE(done.count(), 1); done.clear(); QCOMPARE(server.requests.last().body.value("accountEnabled").toBool(true), false);
        QVERIFY(!api.remove(Resource::Users, QStringLiteral("self"))); QVERIFY(!api.setLogin(QStringLiteral("self"), false));
        QVERIFY(!api.assignRole(QStringLiteral("self"), QStringLiteral("user-role"), QStringLiteral("application")));
        QVERIFY(!api.assignRole(QStringLiteral("other"), QStringLiteral("untrusted-role"), QStringLiteral("application")));
        QVERIFY(api.assignRole(QStringLiteral("other"), QStringLiteral("admin-role"), QStringLiteral("application"))); QTRY_COMPARE(done.count(), 1); done.clear();
        QCOMPARE(server.requests.last().body.value("principalId").toString(), QStringLiteral("other"));
        QCOMPARE(server.requests.last().url.path(), QStringLiteral("/graph/v1.0/users/other/appRoleAssignments"));
        QVERIFY(api.remove(Resource::Users, QStringLiteral("other"))); QTRY_COMPARE(done.count(), 1); QCOMPARE(server.requests.last().verb, QByteArray("DELETE"));
    }
    void readonlyServerCapabilitiesAreRespected()
    {
        AdminServer server; auto *account = server.account();
        account->setCapabilities(Capabilities(server.url(), {{"spaces", QVariantMap{{"enabled", true}}}, {"graph", QVariantMap{{"users", QVariantMap{
            {"create_disabled", true}, {"delete_disabled", true}, {"read_only_attributes", QStringList{QStringLiteral("user.mail"), QStringLiteral("user.accountEnabled"), QStringLiteral("user.appRoleAssignments")}}}}}}}));
        AccountState state(account); UDriveAdminApi api(&state); QVERIFY(discovered(api));
        QVERIFY(!api.canCreate(Resource::Users)); QVERIFY(!api.canDelete(Resource::Users)); QVERIFY(!api.canAssignRoles());
        QVERIFY(!api.save(Resource::Users, QStringLiteral("other"), {{"mail", "blocked@example.test"}})); QVERIFY(!api.setLogin(QStringLiteral("other"), true));
        QVERIFY(!api.save(Resource::Users, QStringLiteral("other"), {{"accountEnabled", false}}));
    }
    void groupsAndWorkspaceOperations()
    {
        AdminServer server; AccountState state(server.account()); UDriveAdminApi api(&state); QVERIFY(discovered(api));
        QSignalSpy done(&api, &UDriveAdminApi::operationFinished), members(&api, &UDriveAdminApi::membersLoaded);
        QVERIFY(api.setGroupMember(QStringLiteral("team"), QStringLiteral("other"), true)); QTRY_COMPARE(done.count(), 1); done.clear();
        QCOMPARE(server.requests.last().url.path(), QStringLiteral("/graph/v1.0/groups/team/members/$ref"));
        QCOMPARE(server.requests.last().body.value("@odata.id").toString(), server.url(QStringLiteral("/graph/v1.0/users/other")).toString());
        api.loadMembers(QStringLiteral("team")); QTRY_COMPARE(members.count(), 1); done.clear();
        QVERIFY(api.setGroupMember(QStringLiteral("team"), QStringLiteral("other"), false)); QTRY_COMPARE(done.count(), 1); done.clear();
        QCOMPARE(server.requests.last().url.path(), QStringLiteral("/graph/v1.0/groups/team/members/other/$ref"));
        QVERIFY(api.save(Resource::Workspaces, {}, {{"name", "Research"}})); QTRY_COMPARE(done.count(), 1); done.clear();
        QCOMPARE(server.requests.last().body.value("driveType").toString(), QStringLiteral("project"));
        QVERIFY(api.restoreWorkspace(QStringLiteral("workspace"), QStringLiteral("Research"))); QTRY_COMPARE(done.count(), 1); done.clear(); QCOMPARE(server.requests.last().headers.value("restore"), QByteArray("T"));
        QVERIFY(api.remove(Resource::Workspaces, QStringLiteral("workspace"))); QTRY_COMPARE(done.count(), 1); QVERIFY(!server.requests.last().headers.contains("purge"));
    }
    void workspaceQuotaPermissionIsSeparate()
    {
        AdminServer server; server.permissions = {"Drives.List.all", "Drives.ReadWriteProjectQuota.all"}; AccountState state(server.account()); UDriveAdminApi api(&state); QVERIFY(discovered(api));
        QVERIFY(api.available()); QVERIFY(!api.canRead(Resource::Users)); QVERIFY(!api.canUpdate(Resource::Workspaces)); QVERIFY(api.canSetQuota());
        QVERIFY(!api.save(Resource::Workspaces, QStringLiteral("workspace"), {{"name", "No permission"}}));
        QVERIFY(api.save(Resource::Workspaces, QStringLiteral("workspace"), {{"quota", QJsonObject{{"total", 1000000000LL}}}}));
        QTRY_VERIFY(!api.busy());
    }
    void unsafeIdsAndPaginationAreRejected()
    {
        AdminServer server, attacker; AccountState state(server.account()); UDriveAdminApi api(&state); QVERIFY(discovered(api));
        for (const auto &id : {QStringLiteral("../self"), QStringLiteral("%2Fself"), QStringLiteral("user?query"), QStringLiteral("."), QStringLiteral("a\\b")}) {
            QVERIFY(!api.remove(Resource::Users, id)); QVERIFY(!api.setLogin(id, true));
        }
        server.nextLink = attacker.url(QStringLiteral("/graph/v1.0/users")).toString();
        QSignalSpy done(&api, &UDriveAdminApi::operationFinished), loaded(&api, &UDriveAdminApi::recordsLoaded);
        api.load(Resource::Users); QTRY_COMPARE(done.count(), 1); QVERIFY(!done.takeFirst().at(0).toBool()); QCOMPARE(loaded.count(), 0); QVERIFY(attacker.requests.isEmpty());
        server.nextLink = server.url(QStringLiteral("/graph/v1.0/users/other")).toString(); api.load(Resource::Users); QTRY_COMPARE(done.count(), 1); QVERIFY(!done.takeFirst().at(0).toBool());
        server.nextLink = server.url(QStringLiteral("/graph/v1.0/users")).toString() + QStringLiteral("?$skip=2"); api.load(Resource::Users);
        QTRY_COMPARE(loaded.count(), 1); QCOMPARE(loaded.first().at(1).toJsonArray().size(), 4);
    }
    void writeRedirectsAndFailuresAreNotReplayed()
    {
        AdminServer server, attacker; AccountState state(server.account()); UDriveAdminApi api(&state); QVERIFY(discovered(api));
        // A redirect normally starts the global connectivity validator. This
        // fixture tests the admin transport independently of that other flow.
        disconnect(state.account(), &Account::unknownConnectionState, &state, nullptr);
        QSignalSpy done(&api, &UDriveAdminApi::operationFinished); server.redirect = attacker.url(QStringLiteral("/capture")).toString();
        int before = server.requests.count(); QVERIFY(api.setLogin(QStringLiteral("other"), true)); QTRY_COMPARE(done.count(), 1); QVERIFY(!done.takeFirst().at(0).toBool()); QCOMPARE(server.requests.count(), before + 1); QVERIFY(attacker.requests.isEmpty());
        server.redirect.clear(); server.writeStatus = 500; before = server.requests.count(); QVERIFY(api.setLogin(QStringLiteral("other"), true)); QTRY_COMPARE(done.count(), 1); QVERIFY(!done.takeFirst().at(0).toBool()); QCOMPARE(server.requests.count(), before + 1);
        server.writeStatus = 403; QVERIFY(api.setLogin(QStringLiteral("other"), true)); QTRY_COMPARE(done.count(), 1); QVERIFY(!api.available());
    }
    void signOutAndCancellationDiscardDelayedData()
    {
        AdminServer server; AccountState state(server.account()); UDriveAdminApi api(&state); QVERIFY(discovered(api));
        QSignalSpy loaded(&api, &UDriveAdminApi::recordsLoaded); server.delay = 150; api.load(Resource::Users); QTRY_VERIFY(api.busy()); api.clear();
        QTest::qWait(200); QVERIFY(!api.available()); QCOMPARE(loaded.count(), 0);
        server.delay = 0; QVERIFY(discovered(api)); server.delay = 150; api.load(Resource::Users); state.signOutByUi();
        QTest::qWait(200); QVERIFY(!api.available()); QCOMPARE(loaded.count(), 0); QVERIFY(api.roles().isEmpty());
    }
    void nativeUiAndAccountIsolation()
    {
        AdminServer server, normal; normal.permissions = {}; auto *manager = AccountManager::instance();
        auto *admin = server.account(); auto *user = normal.account(); auto *adminState = manager->addAccount(admin); auto *normalState = manager->addAccount(user);
        {
            SettingsDialog window(nullptr); window.resize(1400, 850); window.show();
            window.setCurrentAccount(admin); QTRY_VERIFY(window.canAdminister()); window.setCurrentPage(SettingsDialog::SettingsPage::Administration);
            UDriveAdminWidget *page = nullptr;
            for (auto *candidate : window.findChildren<UDriveAdminWidget *>()) if (candidate->api()->available()) page = candidate;
            QVERIFY(page); auto *table = page->findChild<QTreeView *>(QStringLiteral("driveAdminTable")); QVERIFY(table);
            QTRY_COMPARE(table->model()->rowCount(), 2); QVERIFY(page->searchField()->parent() != page);
            QVERIFY(window.canCreateFiles()); QCOMPARE(window.adminCreationLabel(), QStringLiteral("New user"));
            page->searchField()->setText(QStringLiteral("alice")); QTRY_COMPARE(table->model()->rowCount(), 1); page->searchField()->clear();
            auto *roles = page->findChild<QComboBox *>(QStringLiteral("driveAdminRoleFilter")); roles->setCurrentIndex(roles->findData(QStringLiteral("user-role"))); QCOMPARE(table->model()->rowCount(), 1); roles->setCurrentIndex(0);
            // Click Alice's menu while another row is selected and the view is sorted.
            // Opening Edit must still target Alice, not the previously selected account.
            table->sortByColumn(0, Qt::DescendingOrder);
            table->setCurrentIndex(table->model()->index(1, 0));
            bool sawMenu = false;
            QTimer::singleShot(80, &window, [&] {
                auto *menu = page->findChild<QMenu *>(QStringLiteral("driveAdminRowMenu"));
                if (!menu || !menu->isVisible()) return;
                sawMenu = true;
                if (qEnvironmentVariableIsSet("UDRIVE_ADMIN_CAPTURE")) menu->grab().save(QStringLiteral("/tmp/udrive-admin-user-menu.png"));
                menu->actions().first()->trigger(); menu->close();
            });
            const auto actionCell = table->model()->index(0, table->model()->columnCount() - 1);
            QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, table->visualRect(actionCell).center());
            QTRY_VERIFY(sawMenu);
            auto *rowDialog = page->findChild<QDialog *>(QStringLiteral("driveAdminEditDialog")); QVERIFY(rowDialog && rowDialog->isVisible());
            QCOMPARE(rowDialog->findChild<QLineEdit *>(QStringLiteral("adminField_onPremisesSamAccountName"))->text(), QStringLiteral("alice"));
            rowDialog->reject(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            // The same menu is keyboard accessible and preserves self-protection.
            table->sortByColumn(0, Qt::AscendingOrder);
            table->setCurrentIndex(table->model()->index(0, table->model()->columnCount() - 1));
            bool selfProtected = false;
            QTimer::singleShot(80, &window, [&] {
                auto *menu = page->findChild<QMenu *>(QStringLiteral("driveAdminRowMenu"));
                if (!menu || !menu->isVisible()) return;
                selfProtected = menu->actions().size() == 4 && !menu->actions().at(1)->isEnabled()
                    && !menu->actions().at(2)->isEnabled() && !menu->actions().at(3)->isEnabled();
                menu->close();
            });
            QTest::keyClick(table, Qt::Key_Return); QTRY_VERIFY(selfProtected);
            table->setCurrentIndex(table->model()->index(0, 0));
            QVERIFY(!page->findChild<QPushButton *>(QStringLiteral("driveAdminRemove"))->isEnabled());
            table->setCurrentIndex(table->model()->index(1, 0)); QVERIFY(page->findChild<QPushButton *>(QStringLiteral("driveAdminLogin"))->isEnabled());
            window.openNewFileMenu();
            auto *dialog = page->findChild<QDialog *>(QStringLiteral("driveAdminEditDialog")); QVERIFY(dialog && dialog->isVisible());
            dialog->findChild<QLineEdit *>(QStringLiteral("adminField_onPremisesSamAccountName"))->setText(QStringLiteral("bob"));
            dialog->findChild<QLineEdit *>(QStringLiteral("adminField_displayName"))->setText(QStringLiteral("Bob Example"));
            dialog->findChild<QLineEdit *>(QStringLiteral("adminField_mail"))->setText(QStringLiteral("bob@example.test"));
            dialog->findChild<QLineEdit *>(QStringLiteral("adminField_passwordProfile"))->setText(QStringLiteral("test-only-strong-password"));
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
            QTRY_COMPARE(table->model()->rowCount(), 3); QCOMPARE(server.users.size(), 3);
            auto *tabs = page->findChild<QTabBar *>(QStringLiteral("driveAdminTabs")); tabs->setCurrentIndex(1); QTRY_COMPARE(table->model()->rowCount(), 1);
            table->setCurrentIndex(table->model()->index(0, 0)); page->findChild<QPushButton *>(QStringLiteral("driveAdminMembers"))->click();
            auto *membership = page->findChild<QDialog *>(QStringLiteral("driveAdminMembersDialog")); QVERIFY(membership && membership->isVisible());
            auto *memberList = membership->findChild<QListWidget *>(QStringLiteral("adminMembersList"));
            auto *picker = membership->findChild<QComboBox *>(QStringLiteral("adminMemberUser")); QTRY_COMPARE(picker->count(), 3); QTRY_COMPARE(memberList->count(), 1);
            picker->setCurrentIndex(picker->findData(QStringLiteral("new-entry")));
            QPushButton *addMember = nullptr;
            for (auto *button : membership->findChildren<QPushButton *>()) if (button->text() == QStringLiteral("Add member")) addMember = button;
            QVERIFY(addMember && addMember->isEnabled()); addMember->click(); QTRY_COMPARE(memberList->count(), 2); membership->reject();
            tabs->setCurrentIndex(0); QTRY_COMPARE(table->model()->rowCount(), 3);
            if (qEnvironmentVariableIsSet("UDRIVE_ADMIN_CAPTURE")) {
                const auto previous = qApp->palette(); const auto restore = qScopeGuard([previous] { qApp->setPalette(previous); });
                for (bool dark : {true, false}) {
                    auto palette = previous; const QColor surface(dark ? "#292929" : "#FFFFFF"), color(dark ? "#EDEEF0" : "#2B3035");
                    for (auto role : {QPalette::Window, QPalette::Base, QPalette::Button}) palette.setColor(role, surface);
                    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) palette.setColor(role, color);
                    qApp->setPalette(palette); QTest::qWait(100); window.grab().save(QStringLiteral("/tmp/udrive-admin-%1.png").arg(dark ? QStringLiteral("dark") : QStringLiteral("light")));
                    const auto originalSize = window.size(); window.resize(950, 700); QTest::qWait(80);
                    const auto actionsRect = table->visualRect(table->model()->index(0, table->model()->columnCount() - 1));
                    QVERIFY(table->viewport()->rect().contains(actionsRect.center()));
                    window.grab().save(QStringLiteral("/tmp/udrive-admin-compact-%1.png").arg(dark ? QStringLiteral("dark") : QStringLiteral("light")));
                    window.resize(originalSize); QTest::qWait(80);
                    // Optional visual review of every Admin dialog, using only this local mock server.
                    auto capture = [&](const QString &name) {
                        QTest::qWait(80);
                        for (auto *popup : page->findChildren<QDialog *>()) if (popup->isVisible()) {
                            popup->grab().save(QStringLiteral("/tmp/udrive-admin-%1-%2.png").arg(name, dark ? QStringLiteral("dark") : QStringLiteral("light")));
                            popup->reject();
                        }
                        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                    };
                    for (int tab = 0; tab < 3; ++tab) {
                        tabs->setCurrentIndex(tab); QTRY_VERIFY(!page->api()->busy());
                        window.openNewFileMenu(); capture(QStringLiteral("create-%1").arg(tab));
                    }
                    table->setCurrentIndex(table->model()->index(0, 0));
                    page->findChild<QPushButton *>(QStringLiteral("driveAdminEdit"))->click(); capture(QStringLiteral("edit-workspace"));
                    page->findChild<QPushButton *>(QStringLiteral("driveAdminRemove"))->click(); capture(QStringLiteral("disable-workspace"));
                    tabs->setCurrentIndex(0); QTRY_VERIFY(!page->api()->busy());
                    table->setCurrentIndex(table->model()->index(1, 0));
                    page->findChild<QPushButton *>(QStringLiteral("driveAdminRole"))->click(); capture(QStringLiteral("role"));
                    page->findChild<QPushButton *>(QStringLiteral("driveAdminLogin"))->click(); capture(QStringLiteral("login"));
                    tabs->setCurrentIndex(1); QTRY_VERIFY(!page->api()->busy()); table->setCurrentIndex(table->model()->index(0, 0));
                    page->findChild<QPushButton *>(QStringLiteral("driveAdminMembers"))->click(); QTRY_VERIFY(!page->api()->busy()); capture(QStringLiteral("members"));
                    tabs->setCurrentIndex(0); QTRY_VERIFY(!page->api()->busy());
                }
            }
            server.permissions = {"Groups.ReadWrite.all"}; page->api()->refreshAccess();
            QTRY_COMPARE(page->findChild<QTabBar *>(QStringLiteral("driveAdminTabs"))->currentIndex(), 1);
            QTRY_COMPARE(table->model()->rowCount(), 1); QCOMPARE(table->model()->index(0, 0).data().toString(), QStringLiteral("Design"));
            page->findChild<QTabBar *>(QStringLiteral("driveAdminTabs"))->setCurrentIndex(1); QTRY_COMPARE(table->model()->rowCount(), 1);
            QCOMPARE(table->model()->index(0, 0).data().toString(), QStringLiteral("Design"));
            window.setCurrentAccount(user); QTRY_VERIFY(!window.canAdminister()); window.setCurrentPage(SettingsDialog::SettingsPage::Administration);
            QVERIFY(window.currentPage() != SettingsDialog::SettingsPage::Administration);
            window.setCurrentAccount(admin); QVERIFY(window.canAdminister()); window.setCurrentPage(SettingsDialog::SettingsPage::Administration);
            adminState->signOutByUi(); QTRY_VERIFY(!window.canAdminister()); QTRY_VERIFY(window.currentPage() != SettingsDialog::SettingsPage::Administration);
            QCOMPARE(table->model()->rowCount(), 0);
        }
        manager->deleteAccount(adminState); manager->deleteAccount(normalState);
    }
};
QTEST_MAIN(TestUDriveAdmin)
#include "testudriveadmin.moc"
