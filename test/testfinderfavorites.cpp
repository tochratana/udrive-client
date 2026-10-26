// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/utility.h"
#include <QtTest>
#include <QScopeGuard>
#include <QTemporaryDir>

using namespace OCC;
#ifdef Q_OS_MACOS
#include <CoreServices/CoreServices.h>

namespace {
QMap<QString, QStringList> finderEntries(const QString &within, bool remove = false)
{
    QMap<QString, QStringList> result;
    auto list = LSSharedFileListCreate(nullptr, kLSSharedFileListFavoriteItems, nullptr);
    if (!list) return result;
    const auto freeList = qScopeGuard([list] { CFRelease(list); });
    auto snapshot = LSSharedFileListCopySnapshot(list, nullptr);
    if (!snapshot) return result;
    const auto freeSnapshot = qScopeGuard([snapshot] { CFRelease(snapshot); });
    for (CFIndex i = 0; i < CFArrayGetCount(snapshot); ++i) {
        auto item = static_cast<LSSharedFileListItemRef>(const_cast<void *>(CFArrayGetValueAtIndex(snapshot, i)));
        CFURLRef url = nullptr;
        if (LSSharedFileListItemResolve(item, kLSSharedFileListNoUserInteraction | kLSSharedFileListDoNotMountVolumes, &url, nullptr) != noErr || !url) continue;
        CFStringRef path = CFURLCopyFileSystemPath(url, kCFURLPOSIXPathStyle); CFRelease(url);
        if (!path) continue;
        const auto local = QFileInfo(QString::fromCFString(path)).canonicalFilePath(); CFRelease(path);
        if (local != within && !local.startsWith(within + QLatin1Char('/'))) continue;
        auto name = LSSharedFileListItemCopyDisplayName(item);
        result[local].append(name ? QString::fromCFString(name) : QString());
        if (name) CFRelease(name);
        if (remove) LSSharedFileListItemRemove(list, item);
    }
    return result;
}
}
#endif
class TestFinderFavorites : public QObject
{
    Q_OBJECT
private Q_SLOTS:
#ifdef Q_OS_MACOS
    void testAccountFinderFavorites()
    {
        if (!qEnvironmentVariableIsSet("UDRIVE_TEST_FINDER_FAVORITES")) QSKIP("Opt-in test temporarily changes Finder Favorites");
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        const auto base = QFileInfo(temporary.path()).canonicalFilePath();
        const auto cleanup = qScopeGuard([base] { finderEntries(base, true); });
        const QString root = base + QStringLiteral("/Udrive"), personal = root + QStringLiteral("/Personal"), shares = root + QStringLiteral("/Shares");
        const QString other = base + QStringLiteral("/Udrive (2)"), outside = base + QStringLiteral("/Unrelated");
        QVERIFY(QDir().mkpath(personal)); QVERIFY(QDir().mkpath(shares)); QVERIFY(QDir().mkpath(other)); QVERIFY(QDir().mkpath(outside));
        Utility::setupFavLink(root); Utility::setupFavLink(personal); Utility::setupFavLink(shares); Utility::setupFavLink(outside);
        QTRY_VERIFY(finderEntries(base).contains(root) && finderEntries(base).contains(personal)
            && finderEntries(base).contains(shares) && finderEntries(base).contains(outside));
        const auto label = QStringLiteral("Udrive (ratana toch)");
        QVERIFY(Utility::setupAccountFavLink(root, label, {personal, shares}));
        QTRY_COMPARE(finderEntries(base).value(root), QStringList{label});
        QTRY_VERIFY(!finderEntries(base).contains(personal) && !finderEntries(base).contains(shares));
        auto entries = finderEntries(base);
        QCOMPARE(entries.value(root), QStringList{label}); QVERIFY(!entries.contains(personal)); QVERIFY(!entries.contains(shares)); QVERIFY(entries.contains(outside));
        QVERIFY(QFileInfo::exists(personal)); QVERIFY(QFileInfo::exists(shares));
        QVERIFY(Utility::setupAccountFavLink(root, label, {personal, shares}, false));
        QTRY_COMPARE(finderEntries(base).value(root), QStringList{label});
        const auto secondLabel = QStringLiteral("Udrive (Admin)");
        QVERIFY(Utility::setupAccountFavLink(other, secondLabel, {}));
        const auto updatedLabel = QStringLiteral("Udrive (Updated Name)");
        QVERIFY(Utility::setupAccountFavLink(root, updatedLabel, {personal, shares}, false));
        QTRY_COMPARE(finderEntries(base).value(root), QStringList{updatedLabel});
        QTRY_COMPARE(finderEntries(base).value(other), QStringList{secondLabel});
        // Deliberately named child shortcuts remain available to their owner.
        QVERIFY(Utility::setupAccountFavLink(personal, QStringLiteral("My personal shortcut"), {}));
        QTRY_COMPARE(finderEntries(base).value(personal), QStringList{QStringLiteral("My personal shortcut")});
        QVERIFY(Utility::setupAccountFavLink(root, label, {personal, shares}, false));
        QTRY_COMPARE(finderEntries(base).value(root), QStringList{label});
        QCOMPARE(finderEntries(base).value(personal), QStringList{QStringLiteral("My personal shortcut")});
        finderEntries(root, true);
        QTRY_VERIFY(!finderEntries(base).contains(root));
        QVERIFY(!Utility::setupAccountFavLink(root, label, {personal, shares}, false));
        QVERIFY(!finderEntries(base).contains(root));
        QVERIFY(QFileInfo::exists(root));
    }
#endif
};
QTEST_GUILESS_MAIN(TestFinderFavorites)
#include "testfinderfavorites.moc"
