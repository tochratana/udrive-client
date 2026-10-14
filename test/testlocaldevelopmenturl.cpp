// SPDX-License-Identifier: GPL-2.0-or-later
#include "gui/creds/localdevelopment.h"
#include <QtTest>

using namespace OCC;

class TestLocalDevelopmentUrl : public QObject
{
    Q_OBJECT
    QByteArray _originalFlag;
    bool _wasSet = false;

    void setFlag(const QByteArray &flag)
    {
        if (flag.isEmpty()) qunsetenv("UDRIVE_ALLOW_LOCAL_HTTP");
        else qputenv("UDRIVE_ALLOW_LOCAL_HTTP", flag);
    }

private Q_SLOTS:
    void initTestCase()
    {
        _wasSet = qEnvironmentVariableIsSet("UDRIVE_ALLOW_LOCAL_HTTP");
        _originalFlag = qgetenv("UDRIVE_ALLOW_LOCAL_HTTP");
    }

    void cleanup()
    {
        if (_wasSet) qputenv("UDRIVE_ALLOW_LOCAL_HTTP", _originalFlag);
        else qunsetenv("UDRIVE_ALLOW_LOCAL_HTTP");
    }

    void serverUrls_data()
    {
        QTest::addColumn<QString>("address");
        QTest::addColumn<QByteArray>("flag");
        QTest::addColumn<bool>("allowed");
        QTest::addColumn<bool>("debugOnly");
        QTest::newRow("https-default") << QStringLiteral("https://drive.example.com") << QByteArray() << true << false;
        QTest::newRow("localhost-default") << QStringLiteral("http://localhost:9200") << QByteArray() << false << false;
        QTest::newRow("localhost-enabled") << QStringLiteral("http://localhost:9200") << QByteArray("1") << true << true;
        QTest::newRow("ipv4-enabled") << QStringLiteral("http://127.0.0.1:9200") << QByteArray("1") << true << true;
        QTest::newRow("ipv6-enabled") << QStringLiteral("http://[::1]:9200") << QByteArray("1") << true << true;
        QTest::newRow("uppercase-localhost") << QStringLiteral("http://LOCALHOST:9200/") << QByteArray("1") << true << true;
        QTest::newRow("flag-disabled") << QStringLiteral("http://localhost:9200") << QByteArray("0") << false << false;
        QTest::newRow("invalid-flag") << QStringLiteral("http://localhost:9200") << QByteArray("yes") << false << false;
        QTest::newRow("public-http") << QStringLiteral("http://drive.example.com") << QByteArray("1") << false << false;
        QTest::newRow("lan-http") << QStringLiteral("http://192.168.1.10:9200") << QByteArray("1") << false << false;
        QTest::newRow("localhost-suffix") << QStringLiteral("http://localhost.example.com:9200") << QByteArray("1") << false << false;
        QTest::newRow("localhost-userinfo") << QStringLiteral("http://localhost@evil.example:9200") << QByteArray("1") << false << false;
        QTest::newRow("hostless") << QStringLiteral("http:///files") << QByteArray("1") << false << false;
        QTest::newRow("unsupported-scheme") << QStringLiteral("file:///tmp/test") << QByteArray("1") << false << false;
    }

    void serverUrls()
    {
        QFETCH(QString, address);
        QFETCH(QByteArray, flag);
        QFETCH(bool, allowed);
        QFETCH(bool, debugOnly);
        setFlag(flag);
#ifdef QT_NO_DEBUG
        if (debugOnly) allowed = false;
#else
        Q_UNUSED(debugOnly);
#endif
        QCOMPARE(LocalDevelopment::allowsServerUrl(QUrl(address)), allowed);
    }

    void authenticationUrls_data()
    {
        QTest::addColumn<QString>("server");
        QTest::addColumn<QString>("endpoint");
        QTest::addColumn<QByteArray>("flag");
        QTest::addColumn<bool>("allowed");
        QTest::addColumn<bool>("debugOnly");
        QTest::newRow("remote-https") << QStringLiteral("https://drive.example.com") << QStringLiteral("https://auth.example.com/auth") << QByteArray() << true << false;
        QTest::newRow("local-http-auth") << QStringLiteral("http://localhost:9200") << QStringLiteral("http://localhost:8080/auth") << QByteArray("1") << true << true;
        QTest::newRow("local-ip-auth") << QStringLiteral("http://127.0.0.1:9200") << QStringLiteral("http://[::1]:8080/auth") << QByteArray("1") << true << true;
        QTest::newRow("local-https-auth") << QStringLiteral("http://localhost:9200") << QStringLiteral("https://auth.example.com/auth") << QByteArray("1") << true << true;
        QTest::newRow("local-without-flag") << QStringLiteral("http://localhost:9200") << QStringLiteral("https://auth.example.com/auth") << QByteArray() << false << false;
        QTest::newRow("remote-http-auth") << QStringLiteral("http://localhost:9200") << QStringLiteral("http://auth.example.com/auth") << QByteArray("1") << false << false;
        QTest::newRow("remote-account-local-http") << QStringLiteral("https://drive.example.com") << QStringLiteral("http://localhost:8080/auth") << QByteArray("1") << false << false;
        QTest::newRow("public-http-account") << QStringLiteral("http://drive.example.com") << QStringLiteral("https://auth.example.com/auth") << QByteArray("1") << false << false;
        QTest::newRow("auth-host-lookalike") << QStringLiteral("http://localhost:9200") << QStringLiteral("http://localhost.evil.example/auth") << QByteArray("1") << false << false;
        QTest::newRow("auth-hostless") << QStringLiteral("https://drive.example.com") << QStringLiteral("https:///auth") << QByteArray("1") << false << false;
        QTest::newRow("existing-test-transport") << QStringLiteral("oauthtest://auth.example.com") << QStringLiteral("oauthtest://auth.example.com/auth") << QByteArray() << true << false;
    }

    void authenticationUrls()
    {
        QFETCH(QString, server);
        QFETCH(QString, endpoint);
        QFETCH(QByteArray, flag);
        QFETCH(bool, allowed);
        QFETCH(bool, debugOnly);
        setFlag(flag);
#ifdef QT_NO_DEBUG
        if (debugOnly) allowed = false;
#else
        Q_UNUSED(debugOnly);
#endif
        QCOMPARE(LocalDevelopment::allowsAuthenticationUrl(QUrl(endpoint), QUrl(server)), allowed);
    }
};

QTEST_MAIN(TestLocalDevelopmentUrl)
#include "testlocaldevelopmenturl.moc"
