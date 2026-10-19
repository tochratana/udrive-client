/*
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

#pragma once

#include "gui/owncloudguilib.h"

#include <QSet>
#include <QString>

class QApplication;

namespace OCC {

namespace Translations {

    /**
    * @return translation files' filename prefix
    */
    OWNCLOUDGUI_EXPORT const QString translationsFilePrefix();

    /**
    * @returntranslation files' filename suffix
    */
    OWNCLOUDGUI_EXPORT const QString translationsFileSuffix();

    /**
     * @return path to translation files
     */
    OWNCLOUDGUI_EXPORT QString translationsDirectoryPath();

    /** Canonical locale for the three UDrive UI languages; unknown locales use English. */
    OWNCLOUDGUI_EXPORT QString udriveLocale(QString locale);

    /**
     * @return list of locales for which translations are available
     */
    OWNCLOUDGUI_EXPORT QSet<QString> listAvailableTranslations();

    /**
     * Installs or switches application translations dynamically in real time.
     * @param locale Canonical or raw locale code (e.g., "km", "zh_CN", "en_US").
     * @param app Optional pointer to QApplication (defaults to qApp).
     * @return The canonical display language installed.
     */
    OWNCLOUDGUI_EXPORT QString installLanguage(const QString &locale, QApplication *app = nullptr);

} // namespace Translations

} // namespace OCC
