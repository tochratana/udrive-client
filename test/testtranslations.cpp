// SPDX-License-Identifier: GPL-2.0-or-later
#include "gui/translations.h"
#include "libsync/theme.h"
#include <QTranslator>
#include <QtTest>

using namespace OCC;

class TestTranslations : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void supportedLocales_data()
    {
        QTest::addColumn<QString>("saved");
        QTest::addColumn<QString>("canonical");
        QTest::newRow("english-region") << QStringLiteral("en_GB") << QStringLiteral("en_US");
        QTest::newRow("english") << QStringLiteral("en") << QStringLiteral("en_US");
        QTest::newRow("khmer-region") << QStringLiteral("km_KH") << QStringLiteral("km");
        QTest::newRow("khmer") << QStringLiteral("km") << QStringLiteral("km");
        QTest::newRow("chinese-script") << QStringLiteral("zh-Hans-CN") << QStringLiteral("zh_CN");
        QTest::newRow("chinese-region") << QStringLiteral("zh_TW") << QStringLiteral("zh_CN");
        QTest::newRow("previously-supported-language") << QStringLiteral("fr") << QStringLiteral("en_US");
        QTest::newRow("empty") << QString() << QStringLiteral("en_US");
    }
    void supportedLocales()
    {
        QFETCH(QString, saved);
        QFETCH(QString, canonical);
        QCOMPARE(Translations::udriveLocale(saved), canonical);
    }
    void packagedLanguages()
    {
        if (Theme::instance()->appNameGUI() != QStringLiteral("UDrive"))
            QSKIP("The three-language package restriction is specific to UDrive.");
        const QSet<QString> expected{QStringLiteral("en_US"), QStringLiteral("km"), QStringLiteral("zh_CN")};
        QCOMPARE(Translations::listAvailableTranslations(), expected);
    }
    void translatedScreens_data()
    {
        QTest::addColumn<QString>("locale");
        QTest::addColumn<QString>("personal");
        QTest::newRow("khmer") << QStringLiteral("km") << QStringLiteral("ផ្ទាល់ខ្លួន");
        QTest::newRow("chinese") << QStringLiteral("zh_CN") << QStringLiteral("个人");
    }
    void translatedScreens()
    {
        QFETCH(QString, locale);
        QFETCH(QString, personal);
        QTranslator translator;
        QVERIFY(translator.load(Translations::translationsFilePrefix() + locale, Translations::translationsDirectoryPath()));
        QCOMPARE(translator.translate("UDriveSidebar", "Personal"), personal);
        QCOMPARE(translator.translate("OCC::UDriveFileBrowser", "Personal"), personal);
        for (const auto *label : {"Sort by", "Recent", "A-Z", "Z-A", "Newest", "Oldest", "Largest", "Smallest", "Select all", "Select all files in the current results."})
            QVERIFY2(!translator.translate("OCC::UDriveFileBrowser", label).isEmpty(), label);
        QVERIFY(translator.translate("OCC::UDriveFileBrowser", "%1 selected. Select one item to use file actions.").contains(QStringLiteral("%1")));
        for (const auto *source : {"Language saved", "Restart UDrive to apply your new language.", "OK"}) {
            const auto translated = translator.translate("OCC::GeneralSettings", source);
            QVERIFY2(!translated.isEmpty() && translated != QString::fromUtf8(source), source);
        }
        QVERIFY(translator.translate("OCC::UDriveFileBrowser", "Sort by: %1").contains(QStringLiteral("%1")));
        for (const auto *source : {"Delete permanently", "Item permanently deleted.", "Open on Web", "Open this item in your web browser",
                 "The selected item changed. Review it and confirm again."}) {
            const auto translated = translator.translate("OCC::UDriveFileBrowser", source);
            QVERIFY2(!translated.isEmpty() && translated != QString::fromUtf8(source), source);
        }
        for (const auto *source : {"Move to Deleted files", "Delete permanently", "Move “%1” to Deleted files?", "Permanently delete “%1”?", "Cancel", "Delete",
                 "All contents inside this folder will also be moved to Deleted files. You can restore them anytime.",
                 "This item will be moved to Deleted files. You can restore it anytime.",
                 "This folder and all of its contents will be permanently deleted. This action cannot be undone.",
                 "This item will be permanently deleted. This action cannot be undone."}) {
            const auto translated = translator.translate("OCC::DeleteConfirmDialog", source);
            QVERIFY2(!translated.isEmpty() && translated != QString::fromUtf8(source), source);
            if (QString::fromUtf8(source).contains(QStringLiteral("%1"))) QVERIFY(translated.contains(QStringLiteral("%1")));
        }
        for (auto pair : {qMakePair("OCC::GeneralSettings", "Language"), qMakePair("OCC::UDrivePdfPreview", "Back to files"),
                 qMakePair("OCC::UrlPageController", "Welcome to %1"), qMakePair("OCC::AccountView", "Connected")}) {
            const auto translated = translator.translate(pair.first, pair.second);
            QVERIFY2(!translated.isEmpty(), pair.second);
            QVERIFY(translated != QString::fromUtf8(pair.second));
        }
        for (const auto *source : {"Search users or groups…", "Viewer", "Editor", "Share", "Matching users and groups",
                 "Type at least 2 characters to find a user or group.", "Searching…",
                 "Could not search users and groups. Try again.", "User · %1", "Group · %1",
                 "No matching users or groups. Try another name or email.", "Select a user or group, then click Share.", "Selected: %1"}) {
            const auto translated = translator.translate("OCC::UDriveShareDialog", source);
            QVERIFY2(!translated.isEmpty() && translated != QString::fromUtf8(source), source);
            if (QString::fromUtf8(source).contains(QStringLiteral("%1"))) QVERIFY(translated.contains(QStringLiteral("%1")));
        }
        for (const auto *source : {"Share", "Rename", "Make a copy", "Move to folder", "Move to Deleted files", "Share this item with other users", "Shared by me", "Shared with me", "Share direction", "Search files shared by me",
                 "Loading files shared by you…", "No files shared by you yet.", "Could not load files shared by you. %1",
                 "%1 shared items shown; some items are no longer available."}) {
            const auto translated = translator.translate("OCC::UDriveFileBrowser", source);
            QVERIFY2(!translated.isEmpty() && translated != QString::fromUtf8(source), source);
            if (QString::fromUtf8(source).contains(QStringLiteral("%1"))) QVERIFY(translated.contains(QStringLiteral("%1")));
        }
        const auto storage = translator.translate("OCC::SettingsDialog", "%1 of %2 used");
        QVERIFY(storage.contains(QStringLiteral("%1")) && storage.contains(QStringLiteral("%2")));
        if (locale == QStringLiteral("km")) {
            QCOMPARE(translator.translate("OCC::ownCloudGui", "Quit %1"), QStringLiteral("Quit %1"));
            QCOMPARE(translator.translate("UDriveSidebar", "Quit UDrive"), QStringLiteral("Quit UDrive"));
        }
        QVERIFY(qApp->installTranslator(&translator));
        struct Cleanup
        {
            QTranslator *translator;
            ~Cleanup() { qApp->removeTranslator(translator); }
        } cleanup{&translator};
        const auto count = QCoreApplication::translate("OCC::UDriveFileBrowser", "%n item(s)", nullptr, 3);
        QVERIFY(count.contains(QLatin1Char('3')));
        QVERIFY(!count.contains(QStringLiteral("%n")));
        QCOMPARE(QCoreApplication::translate("UDriveSidebar", "Personal"), personal);
    }
};

QTEST_GUILESS_MAIN(TestTranslations)
#include "testtranslations.moc"
