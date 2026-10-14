// SPDX-License-Identifier: GPL-2.0-or-later
#include "gui/folderman.h"
#include "gui/settingsdialog.h"
#include "libsync/account.h"
#include "libsync/configfile.h"
#include "libsync/theme.h"
#import <AppKit/AppKit.h>
#include <QTemporaryDir>
#include <QtTest>

using namespace OCC;

class TestMacDock : public QObject
{
    Q_OBJECT
    QTemporaryDir _config;
    std::unique_ptr<FolderMan> _folders;
private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(_config.isValid());
        ConfigFile::setConfDir(_config.path());
        Account::setCommonCacheDirectory(_config.path() + QStringLiteral("/cache"));
        _folders = FolderMan::createInstance();
    }
    void regularAppDuringWindowLifecycle()
    {
        if (QGuiApplication::platformName() != QStringLiteral("cocoa"))
            QSKIP("Requires the native Cocoa platform.");
        if (Theme::instance()->appNameGUI() != QStringLiteral("UDrive"))
            QSKIP("UDrive Dock behavior only.");
        QVERIFY(NSApp != nil);
        SettingsDialog window(nullptr);
        QCOMPARE(NSApp.activationPolicy, NSApplicationActivationPolicyRegular);
        window.show();
        QTest::qWait(50);
        QCOMPARE(NSApp.activationPolicy, NSApplicationActivationPolicyRegular);
        window.hide();
        QCOMPARE(NSApp.activationPolicy, NSApplicationActivationPolicyRegular);
        window.show();
        QTest::qWait(50);
        QCOMPARE(NSApp.activationPolicy, NSApplicationActivationPolicyRegular);
        window.hide();
    }
};

QTEST_MAIN(TestMacDock)
#include "testmacdock.moc"
