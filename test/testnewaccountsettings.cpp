// SPDX-License-Identifier: GPL-2.0-or-later
#include "gui/newaccountwizard/advancedsettingspagecontroller.h"
#include "gui/folderman.h"
#include "libsync/theme.h"
#include "testutils/testutils.h"

#include <QFileInfo>
#include <QLineEdit>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QWizardPage>
#include <QtTest>

using namespace OCC;

class TestNewAccountSettings : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void accountIdentityPreservesCustomDownloadLocation()
    {
        if (Theme::instance()->appNameGUI() != QStringLiteral("Udrive")) QSKIP("Udrive account naming");
        TestUtils::folderMan();
        QWizardPage page;
        AdvancedSettingsPageController controller(&page, &page);
        auto *location = page.findChild<QLineEdit *>();
        QVERIFY(location);
        controller.setAccountIdentity(QStringLiteral("Ratana"), QUrl("https://work.example.org"));
        QCOMPARE(location->text(), controller.defaultResult().syncRoot);
        QVERIFY(QFileInfo(location->text()).fileName().contains(QStringLiteral("Ratana")));
        QVERIFY(!QFileInfo(location->text()).fileName().contains(QStringLiteral("work.example.org")));
        const auto previousDefault = location->text();
        controller.setAccountIdentity(QStringLiteral("Admin"), QUrl("https://home.example.org"));
        QCOMPARE(location->text(), controller.defaultResult().syncRoot);
        QVERIFY(location->text() != previousDefault);

        QTemporaryDir custom;
        QVERIFY(custom.isValid());
        location->setText(custom.path());
        controller.setAccountIdentity(QStringLiteral("Another account"), QUrl("https://other.example.org"));
        QCOMPARE(location->text(), custom.path());
        QSignalSpy saved(&controller, &AdvancedSettingsPageController::success);
        QVERIFY(controller.validate());
        QCOMPARE(saved.size(), 1);
        QCOMPARE(saved.first().first().value<AdvancedSettingsResult>().syncRoot, custom.path());
    }
};

QTEST_MAIN(TestNewAccountSettings)
#include "testnewaccountsettings.moc"
