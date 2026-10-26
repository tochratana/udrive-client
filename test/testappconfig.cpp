// SPDX-License-Identifier: GPL-2.0-or-later

#include <QtTest>

#include "libsync/owncloudtheme.h"
#include "libsync/config/appconfig.h"

static const std::string_view iniData{
    "[Setup]\n"
    "ServerUrl=https://cloud.example.com\n"
    "AllowServerUrlChange=false\n"
    "MoveToTrash=true\n"
    "\n"
    "[Updater]\n"
    "SkipUpdateCheck=true\n"
    "\n"
    "[OpenIDConnect]\n"
    "ClientId=your-client-id\n"
    "ClientSecret=your-client-secret\n"
    "Ports=8080,8443\n"
    "Scopes=openid offline_access email profile\n"
    "Prompt=select_account consent\n"
};

class TestAppConfig : public QObject
{
    Q_OBJECT

    QString _configPath;

    bool createConfigFile(const QByteArray &data) {
        _configPath = OCC::AppConfig::configPath(QOperatingSystemVersion::Unknown, *OCC::Theme::instance());

        QString parent = QFileInfo(_configPath).path();
        if (!QDir().mkpath(parent))
            return false;

        QFile iniFile(_configPath);
        if (!iniFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            qDebug()<<iniFile.errorString();
            return false;
        }
        bool ok = iniFile.write(data) == data.length();
        iniFile.close();
        return ok;
    }

    void cleanupConfigFile() {
        QFile::remove(_configPath);
    }

private Q_SLOTS:
    void testConfigPath()
    {
        const auto &t = *OCC::Theme::instance();
        const auto windowsPath = t.appNameGUI() == QStringLiteral("Udrive")
            ? QStringLiteral("HKEY_LOCAL_MACHINE\\Software\\Policies\\Unity Workspace\\Udrive")
            : QStringLiteral("HKEY_LOCAL_MACHINE\\Software\\Policies\\ownCloud\\ownCloud");
        QCOMPARE(OCC::AppConfig::configPath(QOperatingSystemVersion::Windows, t), windowsPath);
        QCOMPARE(OCC::AppConfig::configPath(QOperatingSystemVersion::MacOS, t), QString("/Library/Preferences/com.owncloud.desktopclient/ownCloud.ini"));
        QCOMPARE(OCC::AppConfig::configPath(QOperatingSystemVersion::Unknown, t), QString("/etc/ownCloud/ownCloud.ini"));
    }

    void testFromFile()
    {
        if (!OCC::Utility::isLinux()) {
            QSKIP("This test only works on Linux");
        }

        auto cleanup = qScopeGuard([this]() { TestAppConfig::cleanupConfigFile(); });
        QVERIFY(createConfigFile(QByteArray(iniData)));

        OCC::AppConfig appConfig;
        QVERIFY(!appConfig.allowServerUrlChange());

        OCC::OpenIdConfig oidCfg = appConfig.openIdConfig();
        QCOMPARE(oidCfg.clientId(), "your-client-id");
        QCOMPARE(oidCfg.clientSecret(), "your-client-secret");
        QCOMPARE(oidCfg.ports(), (QList<quint16>{8080, 8443}));
        QCOMPARE(oidCfg.scopes(), "openid offline_access email profile");
        QCOMPARE(oidCfg.prompt(), "select_account consent");
    }
};

QTEST_GUILESS_MAIN(TestAppConfig)
#include "testappconfig.moc"
