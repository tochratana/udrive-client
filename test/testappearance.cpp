// SPDX-License-Identifier: GPL-2.0-or-later
#include <QtTest>
#include <QSignalSpy>
#include <QApplication>
#include <QKeyEvent>

#include "libsync/configfile.h"
#include "gui/appearancewidget.h"
#include "gui/application.h"

namespace OCC {

class TestAppearance : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        // Ensure a known initial config state
        ConfigFile cfg;
        cfg.setThemeMode(ConfigFile::ThemeMode::System);
    }

    void testConfigFileThemeModePersistence()
    {
        ConfigFile cfg;
        QCOMPARE(cfg.themeMode(), ConfigFile::ThemeMode::System);

        cfg.setThemeMode(ConfigFile::ThemeMode::Light);
        QCOMPARE(cfg.themeMode(), ConfigFile::ThemeMode::Light);

        cfg.setThemeMode(ConfigFile::ThemeMode::Dark);
        QCOMPARE(cfg.themeMode(), ConfigFile::ThemeMode::Dark);

        cfg.setThemeMode(ConfigFile::ThemeMode::System);
        QCOMPARE(cfg.themeMode(), ConfigFile::ThemeMode::System);
    }

    void testAppearanceWidgetStructureAndSelection()
    {
        ConfigFile cfg;
        cfg.setThemeMode(ConfigFile::ThemeMode::Light);

        AppearanceWidget appearance;
        QCOMPARE(appearance.currentMode(), ConfigFile::ThemeMode::Light);

        auto cards = appearance.findChildren<ThemeCardWidget *>();
        QCOMPARE(cards.size(), 3);

        ThemeCardWidget *lightCard = nullptr;
        ThemeCardWidget *darkCard = nullptr;
        ThemeCardWidget *systemCard = nullptr;

        for (auto *card : cards) {
            if (card->mode() == ConfigFile::ThemeMode::Light)
                lightCard = card;
            else if (card->mode() == ConfigFile::ThemeMode::Dark)
                darkCard = card;
            else if (card->mode() == ConfigFile::ThemeMode::System)
                systemCard = card;
        }

        QVERIFY(lightCard != nullptr);
        QVERIFY(darkCard != nullptr);
        QVERIFY(systemCard != nullptr);

        QVERIFY(lightCard->isSelected());
        QVERIFY(!darkCard->isSelected());
        QVERIFY(!systemCard->isSelected());

        QSignalSpy spy(&appearance, &AppearanceWidget::themeChanged);

        // Switch to Dark
        appearance.setThemeMode(ConfigFile::ThemeMode::Dark, true);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.takeFirst().at(0).value<ConfigFile::ThemeMode>(), ConfigFile::ThemeMode::Dark);
        QVERIFY(!lightCard->isSelected());
        QVERIFY(darkCard->isSelected());
        QVERIFY(!systemCard->isSelected());

        // Switch to System
        appearance.setThemeMode(ConfigFile::ThemeMode::System, true);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.takeFirst().at(0).value<ConfigFile::ThemeMode>(), ConfigFile::ThemeMode::System);
        QVERIFY(!lightCard->isSelected());
        QVERIFY(!darkCard->isSelected());
        QVERIFY(systemCard->isSelected());
    }

    void testCardKeyboardInteraction()
    {
        ThemeCardWidget card(ConfigFile::ThemeMode::Light, QStringLiteral("Light"), QStringLiteral("Bright look"));
        QSignalSpy spy(&card, &ThemeCardWidget::clicked);

        QKeyEvent spaceEvent(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
        QApplication::sendEvent(&card, &spaceEvent);
        QCOMPARE(spy.count(), 1);

        QKeyEvent enterEvent(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(&card, &enterEvent);
        QCOMPARE(spy.count(), 2);
    }

    void testSystemCardDynamicLabel()
    {
        ThemeCardWidget systemCard(ConfigFile::ThemeMode::System, QStringLiteral("System"), QStringLiteral("Follows OS"));
        
        systemCard.updateSystemState(true);
        auto labels = systemCard.findChildren<QLabel *>();
        bool foundDark = false;
        for (auto *l : labels) {
            if (l->text().contains(QStringLiteral("Dark"))) {
                foundDark = true;
                break;
            }
        }
        QVERIFY(foundDark);

        systemCard.updateSystemState(false);
        bool foundLight = false;
        for (auto *l : labels) {
            if (l->text().contains(QStringLiteral("Light"))) {
                foundLight = true;
                break;
            }
        }
        QVERIFY(foundLight);
    }

    void testApplicationThemeModePaletteSwitching()
    {
        Application::applyApplicationTheme(ConfigFile::ThemeMode::Light);
        QVERIFY(qApp->palette().color(QPalette::Window).lightness() >= 128);

        Application::applyApplicationTheme(ConfigFile::ThemeMode::Dark);
        QVERIFY(qApp->palette().color(QPalette::Window).lightness() < 128);

        Application::applyApplicationTheme(ConfigFile::ThemeMode::System);
        // Cleanly restores without crashing or invalid state
        QVERIFY(qApp->palette().color(QPalette::Window).isValid());
    }

    void cleanupTestCase()
    {
        ConfigFile cfg;
        cfg.setThemeMode(ConfigFile::ThemeMode::System);
        Application::applyApplicationTheme(ConfigFile::ThemeMode::System);
    }
};

} // namespace OCC

QTEST_MAIN(OCC::TestAppearance)
#include "testappearance.moc"
