/*
 * Udrive Desktop — Unity Workspace
 * Udrive customization updated: 2026-10-26.
 *
 * Copyright (C) by Hannah von Reth <hannah.vonreth@owncloud.com>
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

#include "owncloudlib.h"

#include <OAIDrive.h>

#include <QIcon>
#include <QList>
#include <QtQmlIntegration>

class TestUDriveFileBrowser;

namespace OCC {
namespace GraphApi {
    class SpacesManager;
    class Space;

    class OWNCLOUDSYNC_EXPORT SpaceImage : public QObject
    {
        Q_OBJECT
        Q_PROPERTY(QUrl qmlImageUrl READ qmlImageUrl NOTIFY imageChanged)
        QML_ELEMENT
        QML_UNCREATABLE("C++ only")
    public:
        SpaceImage(Space *space);

        [[nodiscard]] QUrl url() const { return _url; }
        [[nodiscard]] QString etag() const { return _etag; }
        [[nodiscard]] QIcon image() const;

        [[nodiscard]] QUrl qmlImageUrl() const;

        void update();

    Q_SIGNALS:
        void imageChanged();

    private:
        quint64 _generation = 0;
        QUrl _url;
        QString _etag;
        QIcon _image;
        Space *_space = nullptr;
        bool _loading = false;

        friend class Space;
    };

    class OWNCLOUDSYNC_EXPORT Space : public QObject
    {
        Q_OBJECT
        Q_PROPERTY(SpaceImage *image READ image NOTIFY imageChanged)
        QML_ELEMENT
        QML_UNCREATABLE("Spaces can only be created by the SpacesManager")
    public:
        /***
         * Returns the display name of the drive.
         * This is identical to drive.getName() for most drives.
         * Exceptions: Personal spaces
         */
        QString displayName() const;
        QString defaultFolderName() const;
        QString description() const;
        QUrl webUrl() const;
        QString id() const;
        QString driveId() const;
        QUrl webDavUrl() const;
        QString eTag() const;
        OpenAPI::OAIQuota quota() const;
        const OpenAPI::OAIDrive &drive() const { return _drive; }

        QUuid accountId() const;

        /***
         * Assign a sortPriority to a drive, used for sorting
         */
        uint32_t sortPriority() const;

        /**
         * Whether a drive object has been deleted.
         */
        bool disabled() const;

        SpaceImage *image() const;


    Q_SIGNALS:
        void imageChanged();

    protected:
        QList<OpenAPI::OAIDriveItem> getSpecialItems() const;


    private:
        Space(SpacesManager *spaceManager, const OpenAPI::OAIDrive &drive, bool hasManyPersonalSpaces);
        bool setDrive(const OpenAPI::OAIDrive &drive);

        SpacesManager *_spaceManager;
        QUuid _accountId;
        OpenAPI::OAIDrive _drive;

        SpaceImage *_image;

        bool _hasManyPersonalSpaces;

        friend class SpacesManager;
        friend class SpaceImage;
        friend class ::TestUDriveFileBrowser;
    };

} // OCC
} // GraphApi
