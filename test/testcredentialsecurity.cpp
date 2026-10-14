// SPDX-License-Identifier: GPL-2.0-or-later
#include "gui/creds/credentials.h"
#include "gui/creds/localdevelopment.h"
#include "gui/creds/requestauthenticationcontroller.h"
#include "libsync/creds/credentialmanager.h"
#include <QCborValue>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtTest>

using namespace OCC;
namespace {
class ManualRead final : public CredentialJob
{
public:
    using CredentialJob::CredentialJob;
    void respond(QKeychain::Error error, const QByteArray &bytes) { handleReadResult(error, QStringLiteral("dummy backend error"), bytes); }
};
class HeldReply final : public QNetworkReply
{
public:
    HeldReply(QNetworkAccessManager::Operation op, const QNetworkRequest &request, QObject *parent)
        : QNetworkReply(parent)
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(op);
        open(QIODevice::ReadOnly);
    }
    void abort() override { }

protected:
    qint64 readData(char *, qint64) override { return -1; }
};
class HeldManager final : public OCC::AccessManager
{
public:
    HeldManager()
        : OCC::AccessManager(nullptr)
    {
    }

protected:
    QNetworkReply *createRequest(Operation op, const QNetworkRequest &request, QIODevice *) override { return new HeldReply(op, request, this); }
};
class MemoryCredentials final : public Credentials
{
public:
    using Credentials::Credentials;
    mutable int managers = 0;
    int persists = 0;
    QString saved;
    QPointer<ManualRead> read;
    OCC::AccessManager *createAccessManager() const override
    {
        ++managers;
        return new HeldManager;
    }
    void persist() override
    {
        ++persists;
        saved = _refreshToken;
    }
    AccountBasedOAuth *refreshJob() const { return _oAuthJob; }
    QString token() const { return _accessToken; }
    QString refreshToken() const { return _refreshToken; }
    void accept(const QString &token, const QString &refresh) { askFromUserSucceeded(token, refresh); }
    void beginInteractive(RequestAuthenticationController *controller) { connectAuthenticationController(controller); }

protected:
    CredentialJob *createKeychainReadJob() override
    {
        read = new ManualRead(_account->credentialManager(), "dummy");
        return read;
    }
};
}

class TestCredentialSecurity : public QObject
{
    Q_OBJECT
    QByteArray _previousFlag;
    bool _wasSet = false;
private Q_SLOTS:
    void init()
    {
        _wasSet = qEnvironmentVariableIsSet("UDRIVE_ALLOW_LOCAL_HTTP");
        _previousFlag = qgetenv("UDRIVE_ALLOW_LOCAL_HTTP");
        qunsetenv("UDRIVE_ALLOW_LOCAL_HTTP");
    }
    void cleanup()
    {
        if (_wasSet)
            qputenv("UDRIVE_ALLOW_LOCAL_HTTP", _previousFlag);
        else
            qunsetenv("UDRIVE_ALLOW_LOCAL_HTTP");
    }
    void rejectsInsecureRequests_data()
    {
        QTest::addColumn<QString>("url");
        QTest::addColumn<bool>("explicitCredentials");
        for (const auto &url : {QStringLiteral("http://127.0.0.1:9200/dav"), QStringLiteral("http://remote.example.invalid/dav"),
                 QStringLiteral("http://localhost@remote.example.invalid/dav"), QStringLiteral("ftp://example.invalid/file")}) {
            QTest::newRow(qPrintable(url + "-automatic")) << url << false;
            QTest::newRow(qPrintable(url + "-explicit")) << url << true;
        }
    }
    void rejectsInsecureRequests()
    {
        QFETCH(QString, url);
        QFETCH(bool, explicitCredentials);
        Account account(QUuid::createUuid(), "test", QUrl("https://drive.example.invalid"));
        account.setCredentials(new Credentials("DUMMY_ACCESS", "DUMMY_REFRESH", &account));
        qputenv("UDRIVE_ALLOW_LOCAL_HTTP", "1"); // A remote account never gets this exception.
        QNetworkRequest request{QUrl(url)};
        if (explicitCredentials) {
            request.setAttribute(DontAddCredentialsAttribute, true);
            request.setRawHeader("Authorization", "Basic DUMMY_EXPLICIT");
            request.setRawHeader("Cookie", "DUMMY_COOKIE");
        }
        auto *reply = account.accessManager()->post(request, "DUMMY_BODY");
        QSignalSpy finished(reply, &QNetworkReply::finished);
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(reply->error(), QNetworkReply::ProtocolInvalidOperationError);
        QCOMPARE(reply->bytesAvailable(), qint64(0));
        reply->deleteLater();
    }
    void localHttpRequiresOptIn()
    {
        Account account(QUuid::createUuid(), "test", QUrl("http://localhost:9200"));
        account.setCredentials(new Credentials("DUMMY_ACCESS", "DUMMY_REFRESH", &account));
        auto *reply = account.accessManager()->get(QNetworkRequest(QUrl("http://localhost:9200/dav")));
        QSignalSpy finished(reply, &QNetworkReply::finished);
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(reply->error(), QNetworkReply::ProtocolInvalidOperationError);
        reply->deleteLater();
    }
    void localHttpControl()
    {
#ifdef QT_NO_DEBUG
        QSKIP("Loopback HTTP is deliberately unavailable in Release builds.");
#endif
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QByteArray received;
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            auto *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                received += socket->readAll();
                if (received.contains("\r\n\r\n")) {
                    socket->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok");
                    socket->disconnectFromHost();
                }
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        });
        const QUrl url(QStringLiteral("http://127.0.0.1:%1/dav").arg(server.serverPort()));
        Account account(QUuid::createUuid(), "test", url);
        account.setCredentials(new Credentials("DUMMY_ACCESS", "DUMMY_REFRESH", &account));
        qputenv("UDRIVE_ALLOW_LOCAL_HTTP", "1");
        QNetworkRequest request(url);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
        auto *reply = account.accessManager()->get(request);
        QSignalSpy finished(reply, &QNetworkReply::finished);
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(reply->error(), QNetworkReply::NoError);
        QCOMPARE(reply->readAll(), QByteArray("ok"));
        QVERIFY(received.toLower().contains("authorization: bearer dummy_access\r\n"));
        QCOMPARE(reply->request().attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(), int(QNetworkRequest::ManualRedirectPolicy));
        reply->deleteLater();

        account.credentials()->forgetSensitiveData();
        // Logout clears QNAM's cache on the next event turn. Let that cleanup
        // finish before simulating a later request made from the UI.
        QTest::qWait(20);
        received.clear();
        request.setRawHeader("Authorization", "Bearer DUMMY_ACCESS");
        auto *afterLogout = account.accessManager()->get(request);
        QSignalSpy logoutFinished(afterLogout, &QNetworkReply::finished);
        QTRY_COMPARE(logoutFinished.count(), 1);
        QCOMPARE(afterLogout->error(), QNetworkReply::NoError);
        QVERIFY(!received.toLower().contains("authorization:"));
        afterLogout->deleteLater();
    }
    void credentialCompletion_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::addColumn<bool>("valid");
        QTest::newRow("empty") << QByteArray() << false;
        QTest::newRow("invalid-cbor") << QByteArray::fromHex("ff") << false;
        QTest::newRow("truncated-cbor") << QByteArray::fromHex("633132") << false;
        QTest::newRow("undefined-cbor") << QByteArray::fromHex("f7") << false;
        QTest::newRow("token") << QCborValue("DUMMY_REFRESH").toCbor() << true;
        QTest::newRow("map") << QCborValue::fromVariant(QVariantMap{{"key", 42}}).toCbor() << true;
    }
    void credentialCompletion()
    {
        QFETCH(QByteArray, bytes);
        QFETCH(bool, valid);
        auto *job = new ManualRead(nullptr, "dummy");
        QPointer<ManualRead> guard(job);
        QSignalSpy finished(job, &CredentialJob::finished);
        job->respond(QKeychain::NoError, bytes);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(job->error(), valid ? QKeychain::NoError : QKeychain::OtherError);
        if (!valid)
            QVERIFY(!job->errorString().isEmpty());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(guard.isNull());
    }
    void staleKeychainRead()
    {
        Account account(QUuid::createUuid(), "test", QUrl("https://drive.example.invalid"));
        auto *creds = new MemoryCredentials(&account);
        account.setCredentials(creds);
        creds->fetchFromKeychain();
        auto oldRead = creds->read;
        QVERIFY(oldRead);
        creds->forgetSensitiveData();
        creds->accept("NEW_ACCESS", "NEW_REFRESH");
        const int before = creds->persists;
        oldRead->respond(QKeychain::NoError, QCborValue("OLD_REFRESH").toCbor());
        QCOMPARE(creds->token(), QString("NEW_ACCESS"));
        QCOMPARE(creds->refreshToken(), QString("NEW_REFRESH"));
        QCOMPARE(creds->persists, before);
    }
    void staleRefreshAndNewLogin()
    {
        Account account(QUuid::createUuid(), "test", QUrl("https://drive.example.invalid"));
        auto *creds = new MemoryCredentials("OLD_ACCESS", "OLD_REFRESH", &account);
        account.setCredentials(creds);
        QVERIFY(creds->refreshAccessToken());
        QPointer<AccountBasedOAuth> oldJob = creds->refreshJob();
        QVERIFY(oldJob);
        creds->forgetSensitiveData();
        QVERIFY(!creds->ready());
        QVERIFY(!creds->refreshAccessToken());
        creds->accept("NEW_ACCESS", "NEW_REFRESH");
        const int before = creds->persists;
        QVERIFY(QMetaObject::invokeMethod(oldJob, "refreshFinished", Q_ARG(QString, "OLD_ACCESS"), Q_ARG(QString, "OLD_REFRESH")));
        QVERIFY(QMetaObject::invokeMethod(
            oldJob, "refreshError", Q_ARG(QNetworkReply::NetworkError, QNetworkReply::TimeoutError), Q_ARG(QString, "dummy timeout")));
        QCOMPARE(creds->token(), QString("NEW_ACCESS"));
        QCOMPARE(creds->saved, QString("NEW_REFRESH"));
        QCOMPARE(creds->persists, before);
        QVERIFY(creds->refreshAccessToken());
        auto *newJob = creds->refreshJob();
        QVERIFY(newJob);
        QVERIFY(QMetaObject::invokeMethod(newJob, "refreshFinished", Q_ARG(QString, "LATEST_ACCESS"), Q_ARG(QString, "LATEST_REFRESH")));
        QCOMPARE(creds->saved, QString("LATEST_REFRESH"));
        QVERIFY(creds->ready());
    }
    void logoutDuringRefreshStart()
    {
        Account account(QUuid::createUuid(), "test", QUrl("https://drive.example.invalid"));
        auto *creds = new MemoryCredentials("OLD_ACCESS", "OLD_REFRESH", &account);
        account.setCredentials(creds);
        connect(creds, &Credentials::authenticationStarted, creds, &Credentials::forgetSensitiveData);
        const int before = creds->managers;
        creds->refreshAccessToken();
        QCOMPARE(creds->managers, before);
        QVERIFY(!creds->refreshJob());
        QVERIFY(!creds->ready());
    }
    void staleRefreshRetry()
    {
        Account account(QUuid::createUuid(), "test", QUrl("https://drive.example.invalid"));
        auto *creds = new MemoryCredentials("OLD_ACCESS", "OLD_REFRESH", &account);
        account.setCredentials(creds);
        QVERIFY(creds->refreshAccessToken());
        QVERIFY(QMetaObject::invokeMethod(creds->refreshJob(), "refreshError", Q_ARG(QNetworkReply::NetworkError, QNetworkReply::ContentNotFoundError),
            Q_ARG(QString, "dummy missing endpoint")));
        creds->forgetSensitiveData();
        creds->accept("NEW_ACCESS", "NEW_REFRESH");
        const int before = creds->managers;
        QTest::qWait(20);
        QCOMPARE(creds->managers, before);
        QVERIFY(!creds->refreshJob());
    }
    void staleInteractiveLogin()
    {
        Account account(QUuid::createUuid(), "test", QUrl("https://drive.example.invalid"));
        auto *creds = new MemoryCredentials(&account);
        account.setCredentials(creds);
        auto *old = new RequestAuthenticationController(nullptr, creds);
        creds->beginInteractive(old);
        creds->forgetSensitiveData();
        auto *current = new RequestAuthenticationController(nullptr, creds);
        creds->beginInteractive(current);
        Q_EMIT old->authenticationSucceeded("OLD_ACCESS", "OLD_REFRESH");
        QVERIFY(!creds->ready());
        QCOMPARE(creds->persists, 0);
        Q_EMIT current->authenticationSucceeded("NEW_ACCESS", "NEW_REFRESH");
        QVERIFY(creds->ready());
        QCOMPARE(creds->saved, QString("NEW_REFRESH"));
    }
};
QTEST_MAIN(TestCredentialSecurity)
#include "testcredentialsecurity.moc"
