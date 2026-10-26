/*
 * Udrive Desktop — Unity Workspace
 * Udrive customization updated: 2026-10-26.
 *
 * Copyright (C) by Duncan Mac-Vicar P. <duncan@kde.org>
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
 * Copyright (C) by Daniel Molkentin <danimo@owncloud.com>
 * Copyright (C) by Fabian Müller <fmueller@owncloud.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include "translations.h"

#include <QApplication>
#include <QDirIterator>
#include <QLoggingCategory>
#include <QStandardPaths>

#include <QLibraryInfo>
#include <QLocale>
#include <QPointer>
#include <QTranslator>
#include "libsync/theme.h"
#include "libsync/configfile.h"

namespace OCC {

namespace Translations {

    Q_LOGGING_CATEGORY(lcTranslations, "gui.translations", QtInfoMsg)

    namespace {
    struct InstalledTranslators {
        QPointer<QTranslator> clientTranslator;
        QPointer<QTranslator> qtTranslator;
        QPointer<QTranslator> qtkeychainTranslator;
    };
    static InstalledTranslators s_installed;
    }

    const QString translationsFilePrefix()
    {
        return QStringLiteral("client_");
    }

    const QString translationsFileSuffix()
    {
        return QStringLiteral(".qm");
    }

    QString translationsDirectoryPath()
    {
        return QStringLiteral(":/client/translations/");
    }

    QString udriveLocale(QString locale)
    {
        locale.replace(QLatin1Char('-'), QLatin1Char('_'));
        const QString language = locale.section(QLatin1Char('_'), 0, 0).toLower();
        if (language == QStringLiteral("km"))
            return QStringLiteral("km");
        if (language == QStringLiteral("zh"))
            return QStringLiteral("zh_CN");
        return QStringLiteral("en_US");
    }

    QSet<QString> listAvailableTranslations()
    {
        QSet<QString> availableTranslations;

        // calculate a glob pattern which can be used in the iterator below to match only translations files
        const QString pattern = translationsFilePrefix() + QLatin1Char('*') + translationsFileSuffix();

        QDirIterator it(Translations::translationsDirectoryPath(), QStringList() << pattern);

        while (it.hasNext()) {
            // extract locale part from filename
            // of course, this method relies on the filenames to be accurate
            const auto fileName = QFileInfo(it.next()).fileName();
            const auto localePartLength = fileName.length() - translationsFileSuffix().length() - translationsFilePrefix().length();
            QString localeName = fileName.mid(translationsFilePrefix().length(), localePartLength);

            availableTranslations.insert(localeName);
        }

        return availableTranslations;
    }

    QString installLanguage(const QString &locale, QApplication *app)
    {
        auto *application = app ? app : qApp;
        if (!application)
            return QString();

        // 1. Remove previously installed translators
        if (s_installed.clientTranslator) {
            application->removeTranslator(s_installed.clientTranslator.data());
            delete s_installed.clientTranslator.data();
            s_installed.clientTranslator = nullptr;
        }
        if (s_installed.qtTranslator) {
            application->removeTranslator(s_installed.qtTranslator.data());
            delete s_installed.qtTranslator.data();
            s_installed.qtTranslator = nullptr;
        }
        if (s_installed.qtkeychainTranslator) {
            application->removeTranslator(s_installed.qtkeychainTranslator.data());
            delete s_installed.qtkeychainTranslator.data();
            s_installed.qtkeychainTranslator = nullptr;
        }

        // 2. Resolve preferred language code
        QString lang = locale;
        if (lang.isEmpty()) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
            const auto uiLanguages = QLocale::system().uiLanguages(QLocale::TagSeparator::Underscore);
#else
            QStringList uiLanguages;
            for (auto l : QLocale::system().uiLanguages()) {
                uiLanguages << l.replace(QLatin1Char('-'), QLatin1Char('_'));
            }
#endif
            lang = uiLanguages.value(0, QStringLiteral("en_US"));
        }

        if (Theme::instance()->appNameGUI() == QStringLiteral("Udrive")) {
            lang = udriveLocale(lang);
        } else {
            if (lang == QLatin1String("zh_Hans")) {
                lang = QStringLiteral("zh_CN");
            } else if (lang == QLatin1String("zh_Hant")) {
                lang = QStringLiteral("zh_TW");
            }
        }

        const auto trPath = translationsDirectoryPath();
        const QString trFile = translationsFilePrefix() + lang;
        auto *translator = new QTranslator(application);
        const bool clientLoaded = translator->load(trFile, trPath) || lang.startsWith(QLatin1String("en"));
        if (clientLoaded && !translator->isEmpty()) {
            if (application->installTranslator(translator)) {
                s_installed.clientTranslator = translator;
                qCInfo(lcTranslations) << "Installed client translation for" << lang;
            } else {
                delete translator;
            }
        } else {
            delete translator;
        }

        const QString qtTrPath = QLibraryInfo::path(QLibraryInfo::TranslationsPath);
        const QString qtTrFile = QLatin1String("qt_") + lang;
        const QString qtBaseTrFile = QLatin1String("qtbase_") + lang;
        auto *qtTranslator = new QTranslator(application);
        if (qtTranslator->load(qtTrFile, qtTrPath) || qtTranslator->load(qtTrFile, trPath)
            || qtTranslator->load(qtBaseTrFile, qtTrPath) || qtTranslator->load(qtBaseTrFile, trPath)) {
            if (!qtTranslator->isEmpty()) {
                if (application->installTranslator(qtTranslator)) {
                    s_installed.qtTranslator = qtTranslator;
                } else {
                    delete qtTranslator;
                }
            } else {
                delete qtTranslator;
            }
        } else {
            delete qtTranslator;
        }

        const QString qtkeychainTrFile = QLatin1String("qtkeychain_") + lang;
        auto *qtkeychainTranslator = new QTranslator(application);
        if (qtkeychainTranslator->load(qtkeychainTrFile, qtTrPath) || qtkeychainTranslator->load(qtkeychainTrFile, trPath)) {
            if (!qtkeychainTranslator->isEmpty()) {
                if (application->installTranslator(qtkeychainTranslator)) {
                    s_installed.qtkeychainTranslator = qtkeychainTranslator;
                } else {
                    delete qtkeychainTranslator;
                }
            } else {
                delete qtkeychainTranslator;
            }
        } else {
            delete qtkeychainTranslator;
        }

        QLocale newLocale(lang);
        QLocale::setDefault(newLocale);

        return lang;
    }

} // namespace Translations

} // namespace OCC
