// SPDX-License-Identifier: GPL-2.0-or-later
#include "gui/udriveworkspaceactions.h"
#include "gui/accountstate.h"
#include "gui/folderman.h"
#include "gui/creds/credentials.h"
#include "libsync/account.h"
#include "libsync/configfile.h"
#include <QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonArray>
#include <QFile>

using namespace OCC;
namespace {
class TestCredentials : public Credentials { public: using Credentials::Credentials; void persist() override { } };
class WorkspaceServer : public QTcpServer
{
public:
    struct Request { QByteArray verb; QUrl url; QByteArray body; QMap<QByteArray, QByteArray> headers; };
    QList<Request> requests;
    int deny = 0, delay = 0, copies = 0, created = 0;
    bool failCopy = false, missingFileId = false, truncatedArchive = false, absoluteHrefs = false;
    QString hrefHost;
    QStringList children{QStringLiteral("notes.md"), QStringLiteral(".space")};
    QByteArray archive = QByteArray("PK\005\006", 4) + QByteArray(18, '\0');
    WorkspaceServer()
    {
        listen(QHostAddress::LocalHost, 0);
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                auto data = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, data] {
                    *data += socket->readAll();
                    const int end = data->indexOf("\r\n\r\n"); if (end < 0) return;
                    Request req;
                    const auto lines = data->left(end).split('\n');
                    const auto start = lines.first().trimmed().split(' '); req.verb = start.at(0); req.url = QUrl::fromEncoded(start.at(1));
                    for (const auto &line : lines) { const int colon = line.indexOf(':'); if (colon > 0) req.headers.insert(line.left(colon).toLower().trimmed(), line.mid(colon + 1).trimmed()); }
                    const int length = req.headers.value("content-length").toInt(); if (data->size() < end + 4 + length) return;
                    req.body = data->mid(end + 4, length); requests.append(req); socket->disconnect(this);
                    const QPointer<QTcpSocket> guard(socket);
                    QTimer::singleShot(delay, this, [this, guard, req] { if (guard && guard->state() == QAbstractSocket::ConnectedState) handle(guard, req); });
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }
    QUrl url(const QString &path = {}) const { QUrl u(QStringLiteral("http://127.0.0.1:%1").arg(serverPort())); u.setPath(path); return u; }
    OpenAPI::OAIDrive drive(const QString &id = QStringLiteral("team")) const
    {
        OpenAPI::OAIDrive d;
        d.fromJsonObject(QJsonObject{{"id", id}, {"name", "Research"}, {"driveType", "project"}, {"description", "Subtitle"},
            {"quota", QJsonObject{{"total", 15000000000LL}}},
            {"root", QJsonObject{{"id", QString(id + QStringLiteral("-root"))}, {"eTag", "root-tag"}, {"webDavUrl", url("/dav/" + id).toString()}}}});
        return d;
    }
    static void reply(QTcpSocket *s, int status, const QByteArray &body = {}, const QByteArray &headers = {})
    {
        s->write("HTTP/1.1 " + QByteArray::number(status) + " Result\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n" + headers + "\r\n" + body); s->disconnectFromHost();
    }
    void handle(QTcpSocket *s, const Request &req)
    {
        const auto path = req.url.path();
        if (deny) { reply(s, deny); return; }
        if (req.verb == "POST" && path == QStringLiteral("/graph/v1.0/drives")) {
            ++created; auto d = drive(QStringLiteral("copy%1").arg(created)).asJsonObject();
            d.insert("name", QJsonDocument::fromJson(req.body).object().value("name")); reply(s, 201, QJsonDocument(d).toJson());
        } else if (req.verb == "PATCH") reply(s, 200, "{}");
        else if (req.verb == "DELETE") reply(s, 204);
        else if (req.verb == "MKCOL") reply(s, 405);
        else if (req.verb == "PUT") reply(s, 201, {}, missingFileId ? QByteArray() : QByteArray("OC-FileId: uploaded-id\r\n"));
        else if (req.verb == "COPY") { ++copies; reply(s, failCopy ? 207 : 201, failCopy ? QByteArray("<d:multistatus xmlns:d=\"DAV:\"><d:response><d:status>HTTP/1.1 403 Forbidden</d:status></d:response></d:multistatus>") : QByteArray()); }
        else if (req.verb == "PROPFIND") {
            auto entry = [this](const QString &p, const QString &id) { auto target = url(p); if (!hrefHost.isEmpty()) target.setHost(hrefHost); const auto href = absoluteHrefs ? target.toString(QUrl::FullyEncoded) : target.path(QUrl::FullyEncoded); return QStringLiteral("<d:response><d:href>%1</d:href><d:propstat><d:prop><oc:fileid>%2</oc:fileid><d:resourcetype/></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>").arg(href.toHtmlEscaped(), id).toUtf8(); };
            QByteArray xml = "<d:multistatus xmlns:d=\"DAV:\" xmlns:oc=\"http://owncloud.org/ns\">";
            xml += entry(path, "new-file-id");
            if (req.headers.value("depth") == "1") for (const auto &child : children) xml += entry(path + "/" + child, "child");
            xml += "</d:multistatus>"; reply(s, 207, xml);
        } else if (path == QStringLiteral("/archive")) {
            if (truncatedArchive) {
                s->write("HTTP/1.1 200 OK\r\nContent-Length: 9999\r\nConnection: close\r\n\r\n" + archive); s->disconnectFromHost();
            } else reply(s, 200, archive);
        }
        else reply(s, 200, "# Workspace description\nPlain **Markdown**");
    }
};
}
class TestUDriveWorkspaceActions : public QObject
{
    Q_OBJECT
    QTemporaryDir config;
    QByteArray previous;
    std::unique_ptr<FolderMan> folders;
private Q_SLOTS:
    void initTestCase() { previous = qgetenv("UDRIVE_ALLOW_LOCAL_HTTP"); qputenv("UDRIVE_ALLOW_LOCAL_HTTP", "1"); ConfigFile::setConfDir(config.path()); Account::setCommonCacheDirectory(config.path() + "/cache"); folders = FolderMan::createInstance(); }
    void cleanupTestCase() { if (previous.isNull()) qunsetenv("UDRIVE_ALLOW_LOCAL_HTTP"); else qputenv("UDRIVE_ALLOW_LOCAL_HTTP", previous); }
    void mutationsAndErrors()
    {
        WorkspaceServer server;
        auto *account = new Account(QUuid::createUuid(), "test", server.url()); account->setCredentials(new TestCredentials("browser-test-token", "test-refresh-token", account)); account->credentials()->fetchFromKeychain();
        AccountState state(account); UDriveWorkspaceActions actions(&state); QSignalSpy done(&actions, &UDriveWorkspaceActions::finished);
        const auto drive = server.drive();
        QVERIFY(actions.patch({drive}, {{"name", "Renamed"}})); QTRY_COMPARE(done.size(), 1); QVERIFY(done.takeFirst().at(0).toBool());
        QCOMPARE(QJsonDocument::fromJson(server.requests.last().body).object().value("name").toString(), QStringLiteral("Renamed"));
        QVERIFY(actions.patch({drive, server.drive("second")}, {{"quota", QJsonObject{{"total", 2000000000LL}}}})); QTRY_COMPARE(done.size(), 1); QVERIFY(done.takeFirst().at(0).toBool());
        QCOMPARE(QJsonDocument::fromJson(server.requests.last().body).object().value("quota").toObject().value("total").toInteger(), 2000000000LL);
        QVERIFY(actions.disable({drive})); QTRY_COMPARE(done.size(), 1); QVERIFY(done.takeFirst().at(0).toBool());
        QCOMPARE(server.requests.last().verb, QByteArray("DELETE")); QVERIFY(!server.requests.last().headers.contains("purge"));
        QVERIFY(actions.disable({drive}, true)); QTRY_COMPARE(done.size(), 1); QVERIFY(done.takeFirst().at(0).toBool()); QCOMPARE(server.requests.last().headers.value("restore"), QByteArray("T"));
        server.deny = 403; QVERIFY(actions.patch({drive}, {{"description", "No"}})); QTRY_COMPARE(done.size(), 1); auto result = done.takeFirst(); QVERIFY(!result.at(0).toBool()); QVERIFY(result.at(1).toString().contains("Permission"));
        const int count = server.requests.size(); server.deny = 500;
        QVERIFY(actions.patch({drive}, {{"name", "No replay"}})); QTRY_COMPARE(done.size(), 1); QVERIFY(!done.takeFirst().at(0).toBool());
        QCOMPARE(server.requests.size(), count + 1);
    }
    void specialFilesAndDescription()
    {
        WorkspaceServer server;
        auto *account = new Account(QUuid::createUuid(), "test", server.url()); account->setCredentials(new TestCredentials("browser-test-token", "test-refresh-token", account)); account->credentials()->fetchFromKeychain();
        AccountState state(account); UDriveWorkspaceActions actions(&state); QSignalSpy done(&actions, &UDriveWorkspaceActions::finished);
        auto drive = server.drive();
        for (const auto &kind : {QStringLiteral("readme"), QStringLiteral("image")}) {
            server.missingFileId = kind == QStringLiteral("image");
            QVERIFY(actions.writeSpecial(drive, kind, "test-content")); QTRY_COMPARE(done.size(), 1); QVERIFY(done.takeFirst().at(0).toBool());
            const auto body = QJsonDocument::fromJson(server.requests.last().body).object();
            const auto special = body.value("special").toArray().first().toObject();
            QCOMPARE(special.value("specialFolder").toObject().value("name").toString(), kind);
            QCOMPARE(special.value("id").toString(), server.missingFileId ? QStringLiteral("new-file-id") : QStringLiteral("uploaded-id"));
        }
        auto data = drive.asJsonObject(); data.insert("special", QJsonArray{QJsonObject{{"id", "readme"}, {"name", "readme.md"}, {"specialFolder", QJsonObject{{"name", "readme"}}}}}); drive.fromJsonObject(data);
        QSignalSpy content(&actions, &UDriveWorkspaceActions::descriptionLoaded);
        QVERIFY(actions.readDescription(drive)); QTRY_COMPARE(content.size(), 1); QVERIFY(content.first().at(1).toString().contains("**Markdown**"));
        QVERIFY(!actions.busy());
    }
    void duplicateAndPartialFailure()
    {
        WorkspaceServer server;
        auto *account = new Account(QUuid::createUuid(), "test", server.url()); account->setCredentials(new TestCredentials("browser-test-token", "test-refresh-token", account)); account->credentials()->fetchFromKeychain();
        AccountState state(account); UDriveWorkspaceActions actions(&state); QSignalSpy done(&actions, &UDriveWorkspaceActions::finished);
        auto drive = server.drive(); auto data = drive.asJsonObject(); data.insert("special", QJsonArray{QJsonObject{{"id", "readme"}, {"name", "readme.md"}, {"specialFolder", QJsonObject{{"name", "readme"}}}}}); drive.fromJsonObject(data);
        QVERIFY(actions.duplicate({drive}, {"Research copy"})); QTRY_COMPARE(done.size(), 1); QVERIFY(done.takeFirst().at(0).toBool()); QCOMPARE(server.copies, 2);
        bool copied = false, linked = false;
        for (const auto &req : server.requests) {
            if (req.verb == "COPY") { copied = true; QCOMPARE(req.headers.value("overwrite"), QByteArray("F")); QVERIFY(req.headers.value("destination").contains("/dav/copy1/")); }
            if (req.verb == "PATCH") { linked = true; QCOMPARE(QJsonDocument::fromJson(req.body).object().value("special").toArray().first().toObject().value("id").toString(), QStringLiteral("new-file-id")); }
        }
        QVERIFY(copied && linked);
        server.failCopy = true; QVERIFY(actions.duplicate({drive}, {"Partial"})); QTRY_COMPARE(done.size(), 1); auto result = done.takeFirst(); QVERIFY(!result.at(0).toBool()); QVERIFY(result.at(1).toString().contains("incomplete"));
    }
    void duplicatePreservesEncodedNames_data()
    {
        QTest::addColumn<bool>("absolute");
        QTest::newRow("relative-hrefs") << false;
        QTest::newRow("absolute-hrefs") << true;
    }
    void duplicatePreservesEncodedNames()
    {
        QFETCH(bool, absolute);
        WorkspaceServer server;
        server.absoluteHrefs = absolute;
        server.children = {QStringLiteral("report#final.txt"), QStringLiteral("report?final.txt"), QStringLiteral("100%25.txt"),
            QStringLiteral("literal%2F.txt"), QStringLiteral("ឯកសារ & notes.txt")};
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new TestCredentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account); UDriveWorkspaceActions actions(&state);
        QSignalSpy done(&actions, &UDriveWorkspaceActions::finished);
        QVERIFY(actions.duplicate({server.drive()}, {QStringLiteral("Preserved names")}));
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(done.first().at(0).toBool());
        QStringList sourceNames, destinationNames;
        for (const auto &request : server.requests) if (request.verb == "COPY") {
            sourceNames.append(request.url.path().mid(QStringLiteral("/dav/team/").size()));
            const auto destination = QUrl::fromEncoded(request.headers.value("destination"));
            QVERIFY(destination.query().isEmpty()); QVERIFY(destination.fragment().isEmpty());
            destinationNames.append(destination.path().mid(QStringLiteral("/dav/copy1/").size()));
        }
        QCOMPARE(sourceNames, server.children);
        QCOMPARE(destinationNames, server.children);
    }

    void duplicateRejectsInvalidListing_data()
    {
        QTest::addColumn<QString>("child"); QTest::addColumn<bool>("foreignOrigin");
        QTest::newRow("traversal") << QStringLiteral("../escape.txt") << false;
        QTest::newRow("nested-entry") << QStringLiteral("folder/nested.txt") << false;
        QTest::newRow("foreign-origin") << QStringLiteral("notes.md") << true;
    }
    void duplicateRejectsInvalidListing()
    {
        QFETCH(QString, child); QFETCH(bool, foreignOrigin);
        WorkspaceServer server; server.children = {child};
        server.absoluteHrefs = foreignOrigin;
        if (foreignOrigin) server.hrefHost = QStringLiteral("other.invalid");
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new TestCredentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account); UDriveWorkspaceActions actions(&state);
        QSignalSpy done(&actions, &UDriveWorkspaceActions::finished);
        QVERIFY(actions.duplicate({server.drive()}, {QStringLiteral("Rejected listing")}));
        QTRY_COMPARE(done.size(), 1); QVERIFY(!done.first().at(0).toBool());
        QCOMPARE(server.copies, 0);
        for (const auto &request : server.requests) QVERIFY(request.verb != "COPY");
    }

    void archiveIsAtomicAndCancellationStopsChains()
    {
        WorkspaceServer server;
        auto *account = new Account(QUuid::createUuid(), "test", server.url()); account->setCredentials(new TestCredentials("browser-test-token", "test-refresh-token", account)); account->credentials()->fetchFromKeychain();
        account->setCapabilities(Capabilities(server.url(), QVariantMap{{"spaces", QVariantMap{{"enabled", true}}}, {"files", QVariantMap{{"archivers", QVariantList{QVariantMap{{"enabled", true}, {"version", "v2.0.0"}, {"archiver_url", "/archive"}}}}}}}));
        AccountState state(account); UDriveWorkspaceActions actions(&state); QSignalSpy done(&actions, &UDriveWorkspaceActions::finished);
        QTemporaryDir folder; const auto path = folder.filePath("archive.zip");
        QVERIFY(actions.archiveAvailable()); QVERIFY(actions.download({server.drive(), server.drive("second")}, path)); QTRY_COMPARE(done.size(), 1); QVERIFY(done.takeFirst().at(0).toBool());
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), server.archive); file.close();
        QCOMPARE(QUrlQuery(server.requests.last().url).allQueryItemValues("id"), QStringList({"team-root", "second-root"}));
        server.archive = "<html>not an archive</html>"; QVERIFY(actions.download({server.drive()}, path)); QTRY_COMPARE(done.size(), 1); QVERIFY(!done.takeFirst().at(0).toBool());
        QVERIFY(file.open(QIODevice::ReadOnly)); QVERIFY(file.readAll().startsWith("PK")); file.close();
        server.archive = QByteArray("PK\005\006", 4) + QByteArray(18, '\0'); server.truncatedArchive = true;
        QVERIFY(actions.download({server.drive()}, path)); QTRY_COMPARE(done.size(), 1); QVERIFY(!done.takeFirst().at(0).toBool());
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), server.archive); file.close();
        server.delay = 250; QVERIFY(actions.duplicate({server.drive()}, {"Canceled"}));
        QTRY_VERIFY(!server.requests.isEmpty() && server.requests.last().verb == "POST"); const int before = server.requests.size();
        state.signOutByUi(); QTRY_VERIFY(!actions.busy()); QTest::qWait(300); QCOMPARE(server.requests.size(), before);
        QVERIFY(!done.isEmpty() && !done.last().at(0).toBool());
    }
};
QTEST_MAIN(TestUDriveWorkspaceActions)
#include "testudriveworkspaceactions.moc"
