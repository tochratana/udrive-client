// SPDX-License-Identifier: GPL-2.0-or-later
#include "libsync/httplogger.h"
#include <QBuffer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QtTest>
#include <cstring>

namespace {
QString captured;
void capture(QtMsgType, const QMessageLogContext &, const QString &message)
{
    captured += message + QLatin1Char('\n');
}
class Reply final : public QNetworkReply
{
public:
    Reply(const QNetworkRequest &request, const QByteArray &payload, const QString &mime)
        : _payload(payload)
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::PostOperation);
        setHeader(QNetworkRequest::ContentTypeHeader, mime);
        setRawHeader("Set-Cookie", "DUMMY_SESSION_SECRET");
        setRawHeader("Location", "https://example.invalid/?access_token=DUMMY_LOCATION_SECRET");
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
        open(QIODevice::ReadOnly);
    }
    void finish()
    {
        Q_EMIT requestSent();
        setFinished(true);
        Q_EMIT finished();
    }
    void fail()
    {
        setError(AuthenticationRequiredError, "DUMMY_ERROR_SECRET");
        Q_EMIT errorOccurred(error());
        finish();
    }
    void abort() override { }
    qint64 size() const override { return _payload.size(); }
    qint64 bytesAvailable() const override { return _payload.size() - _position + QNetworkReply::bytesAvailable(); }

protected:
    qint64 readData(char *out, qint64 maximum) override
    {
        const qint64 count = qMin(maximum, qint64(_payload.size() - _position));
        std::memcpy(out, _payload.constData() + _position, count);
        _position += count;
        return count;
    }

private:
    QByteArray _payload;
    qint64 _position = 0;
};
}

class TestHttpLogger : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void sensitiveAdministrationBodiesAreOmitted()
    {
        const QByteArray payload = R"({"mail":"PRIVATE_DIRECTORY_EMAIL","passwordProfile":{"password":"PRIVATE_ADMIN_PASSWORD"}})";
        QNetworkRequest request(QUrl(QStringLiteral("https://example.invalid/graph/v1.0/users")));
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        request.setAttribute(OCC::HttpLogger::OmitBodyAttribute, true);
        QBuffer body; body.setData(payload); QVERIFY(body.open(QIODevice::ReadOnly));
        Reply reply(request, payload, QStringLiteral("application/json"));
        captured.clear(); const auto handler = qInstallMessageHandler(capture);
        OCC::HttpLogger::logRequest(&reply, QNetworkAccessManager::PostOperation, &body); reply.finish();
        qInstallMessageHandler(handler);
        QVERIFY(!captured.contains(QStringLiteral("PRIVATE_")));
        QCOMPARE(captured.count(QStringLiteral("[sensitive body omitted]")), 2);
        QVERIFY(captured.contains(QStringLiteral("REQUEST"))); QVERIFY(captured.contains(QStringLiteral("RESPONSE")));
    }
    void payloads_data()
    {
        QTest::addColumn<QByteArray>("payload");
        QTest::addColumn<QString>("mime");
        QTest::addColumn<bool>("ordinaryBody");
        QTest::newRow("form") << QByteArray("grant_type=refresh_token&%72efresh_token=DUMMY_REFRESH_SECRET&client_secret=DUMMY_CLIENT_SECRET&code=DUMMY_CODE_"
                                            "SECRET&code_verifier=DUMMY_PKCE_SECRET")
                              << QStringLiteral("application/x-www-form-urlencoded") << false;
        QTest::newRow("json") << QByteArray(
            R"({"access_token":"DUMMY_ACCESS_SECRET","refresh_token":"DUMMY_REFRESH_SECRET","id_token":"DUMMY_ID_SECRET","ok":true})")
                              << QStringLiteral("application/json") << false;
        QTest::newRow("nested-json") << QByteArray(R"({"items":[{"ACCESS_TOKEN":"DUMMY_ACCESS_SECRET","password":"DUMMY_PASSWORD_SECRET"}],"ok":true})")
                                     << QStringLiteral("application/problem+json") << false;
        QTest::newRow("json-text-mime") << QByteArray(R"({"refresh_token":"DUMMY_REFRESH_SECRET"})") << QStringLiteral("text/plain") << false;
        QTest::newRow("malformed-json") << QByteArray(R"({"access_token":"DUMMY_ACCESS_SECRET")") << QStringLiteral("application/json") << false;
        QTest::newRow("large-json") << (QByteArray(R"({"access_token":"DUMMY_ACCESS_SECRET","padding":")") + QByteArray(1024 * 1024, 'x') + "\"}")
                                    << QStringLiteral("application/json") << false;
        QTest::newRow("ordinary-xml") << QByteArray("<d:multistatus xmlns:d=\"DAV:\"/>") << QStringLiteral("application/xml") << true;
        QTest::newRow("ordinary-json") << QByteArray("{\"count\":42}") << QStringLiteral("application/json") << true;
    }
    void payloads()
    {
        QFETCH(QByteArray, payload);
        QFETCH(QString, mime);
        QFETCH(bool, ordinaryBody);
        QNetworkRequest request(QUrl("https://DUMMY_USER_SECRET:DUMMY_PASSWORD_SECRET@example.invalid/token?code=DUMMY_QUERY_SECRET"));
        request.setRawHeader("aUtHoRiZaTiOn", "Bearer DUMMY_HEADER_SECRET");
        request.setRawHeader("Proxy-Authorization", "Basic DUMMY_PROXY_SECRET");
        request.setRawHeader("Cookie", "DUMMY_COOKIE_SECRET");
        request.setHeader(QNetworkRequest::ContentTypeHeader, mime);
        QBuffer body;
        body.setData(payload);
        QVERIFY(body.open(QIODevice::ReadOnly));
        Reply reply(request, payload, mime);
        // Even the historical unsafe override must not expose credentials.
        const auto oldFlag = qgetenv("OWNCLOUD_HTTPLOGGER_NO_REDACT");
        const bool wasSet = qEnvironmentVariableIsSet("OWNCLOUD_HTTPLOGGER_NO_REDACT");
        qputenv("OWNCLOUD_HTTPLOGGER_NO_REDACT", "1");
        captured.clear();
        const auto handler = qInstallMessageHandler(capture);
        OCC::HttpLogger::logRequest(&reply, QNetworkAccessManager::PostOperation, &body);
        reply.finish();
        qInstallMessageHandler(handler);
        if (wasSet)
            qputenv("OWNCLOUD_HTTPLOGGER_NO_REDACT", oldFlag);
        else
            qunsetenv("OWNCLOUD_HTTPLOGGER_NO_REDACT");
        QVERIFY(!captured.contains("DUMMY_"));
        QVERIFY(captured.contains("REQUEST"));
        QVERIFY(captured.contains("RESPONSE"));
        if (ordinaryBody) {
            const auto start = captured.indexOf(QLatin1Char('{'));
            const auto end = captured.indexOf(QLatin1Char('\n'), start);
            const auto logged = QJsonDocument::fromJson(captured.mid(start, end - start).toUtf8()).object();
            QCOMPARE(logged.value("request").toObject().value("body").toObject().value("data").toString(), QString::fromUtf8(payload));
        }
        QCOMPARE(body.pos(), qint64(0));
        QCOMPARE(body.readAll(), payload);
        QCOMPARE(reply.readAll(), payload);
        QCOMPARE(request.rawHeader("Authorization"), QByteArray("Bearer DUMMY_HEADER_SECRET"));
    }
    void nativeErrorDoesNotRepeatSecrets()
    {
        QNetworkRequest request(QUrl("https://example.invalid/?token=DUMMY_QUERY_SECRET"));
        Reply reply(request, {}, "application/json");
        captured.clear();
        const auto handler = qInstallMessageHandler(capture);
        OCC::HttpLogger::logRequest(&reply, QNetworkAccessManager::GetOperation, nullptr);
        reply.fail();
        qInstallMessageHandler(handler);
        QVERIFY(!captured.contains("DUMMY_"));
        QVERIFY(captured.contains("An error occurred"));
    }
};
QTEST_MAIN(TestHttpLogger)
#include "testhttplogger.moc"
