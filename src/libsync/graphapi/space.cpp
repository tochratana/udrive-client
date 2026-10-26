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

#include "space.h"

#include "common/utility.h"
#include "libsync/graphapi/spacesmanager.h"
#include "libsync/networkjobs.h"
#include "libsync/networkjobs/resources.h"

#include "resources/resources.h"

using namespace OCC;
using namespace GraphApi;

namespace {

const auto personalC = QLatin1String("personal");

// https://github.com/cs3org/reva/blob/0cde0a3735beaa14ebdfd8988c3eb77b3c2ab0e6/pkg/utils/utils.go#L56-L59

// important detail about this id: the "shares" folder always has this id, even across different accounts!
// this means that we can never assume that space id's are unique across accounts, to the contrary, if a share is
// in play they most definitely are not. This concept could theoretically extend to other folders so we need to index
// space id against account id for many operations.
const auto sharesIdC = QLatin1String("a0ca6a90-a365-4782-871e-d44447bbc668$a0ca6a90-a365-4782-871e-d44447bbc668");

}

Space::Space(SpacesManager *spacesManager, const OpenAPI::OAIDrive &drive, const bool hasManyPersonalSpaces)
    : QObject(spacesManager)
    , _spaceManager(spacesManager)
    , _image(new SpaceImage(this))
    , _hasManyPersonalSpaces(hasManyPersonalSpaces)
{
    // todo future refactoring: get this setDrive out of the ctr since it potentially kicks off a job for the SpaceImage before the Space is fully constructed
    // propose removing the drive arg from the ctr completely, and moving the call to setDrive to the spacesmanager such that any call to
    // "new" space is immediately followed by setDrive.
    if (_spaceManager->account())
        _accountId = _spaceManager->account()->uuid();
    setDrive(drive);
    connect(_image, &SpaceImage::imageChanged, this, &Space::imageChanged);
}

QUuid Space::accountId() const
{
    return _accountId;
}

bool Space::setDrive(const OpenAPI::OAIDrive &drive)
{
    // first config naturally has an empty drive - reality check that updated drives are always valid
    Q_ASSERT(drive.isValid());

    // Workspace metadata (including subtitles, permissions and special-file URLs)
    // can change without a new root-directory ETag.
    if (_drive.asJsonObject() == drive.asJsonObject()) {
        _image->update(); // Retry a failed image fetch on the next catalog refresh.
        return false;
    }

    _drive = drive;
    _image->update();
    return true;
}

QString Space::displayName() const
{
    if (_hasManyPersonalSpaces) {
        return _drive.getName();
    }

    // other systems like oCIS have one personal and one shared space and their names are hard coded
    if (_drive.getDriveType() == personalC) {
        return tr("Personal");
    }
    if (_drive.getId() == sharesIdC) {
        // don't call it ShareJail
        return tr("Shares");
    }
    return _drive.getName();
}

QString Space::defaultFolderName() const
{
    if (_hasManyPersonalSpaces) {
        return _drive.getName();
    }

    if (_drive.getDriveType() == personalC) {
        return QStringLiteral("Personal");
    }
    if (_drive.getId() == sharesIdC) {
        return QStringLiteral("Shares");
    }
    return _drive.getName();
}

QString Space::description() const
{
    return _drive.getDescription();
}


uint32_t Space::sortPriority() const
{
    if (_drive.getDriveType() == personalC) {
        return 100;
    } else if (_drive.getId() == sharesIdC) {
        return 50;
    }
    return 0;
}

bool Space::disabled() const
{
    // this is how disabled spaces are represented in the graph API
    return _drive.getRoot().getDeleted().getState() == QLatin1String("trashed");
}

SpaceImage *Space::image() const
{
    return _image;
}

QString Space::id() const
{
    return _drive.getRoot().getId();
}

QString Space::driveId() const
{
    return _drive.getId();
}

QUrl Space::webDavUrl() const
{
    return QUrl(_drive.getRoot().getWebDavUrl());
}

QUrl Space::webUrl() const
{
    return QUrl(_drive.getRoot().getWebUrl());
}

QString Space::eTag() const
{
    return _drive.getETag();
}

QList<OpenAPI::OAIDriveItem> Space::getSpecialItems() const
{
    return _drive.getSpecial();
}

OpenAPI::OAIQuota Space::quota() const
{
    return _drive.getQuota();
}

SpaceImage::SpaceImage(Space *space)
    : QObject(space)
    , _space(space)
{
}

QIcon SpaceImage::image() const
{
    return _image;
}

// this has to stay until we get rid of the folderWizard
QUrl SpaceImage::qmlImageUrl() const
{
    if (!_image.isNull()) {
        return QUrl(QStringLiteral("image://space/%1/%2").arg(etag(), _space->id()));
    } else {
        // invalid space id to display the placeholder
        return QUrl(QStringLiteral("image://space/placeholder"));
    }
}

// todo: ideally do the update WITH any other drive changes that may happen.
// the current problem is that we can't rely on spaceChanged to tell us the image changed because spaceChanged is emitted
// prior to calling this update (so if you get the image in response to spaceChanged you get the "old" image, not the new one).
// this means we have to do some fancy tricks to ensure we connect to the imageChanged signal "at just the right time"
// see the FolderItemUpdater for the example ick.
void SpaceImage::update()
{
    if (!_space || !_space->_spaceManager) {
        return;
    }

    const auto &special = _space->getSpecialItems();
    const auto img = std::find_if(special.cbegin(), special.cend(), [](const auto &it) {
        return it.getSpecialFolder().getName() == QLatin1String("image");
    });

    if (img == special.cend()) {
        if (!_image.isNull() || !_etag.isEmpty() || !_url.isEmpty()) {
            ++_generation; // Ignore any download still in flight for the removed image.
            _image = QIcon();
            _etag.clear();
            _url.clear();
            _loading = false;
            emit imageChanged();
        }
        return;
    }

    // Determine the new ETag / signature for this image
    QString newEtag = Utility::normalizeEtag(img->getETag());
    if (newEtag.isEmpty()) {
        newEtag = img->getId() + QLatin1Char(':') + img->getName();
    }

    auto *account = _space->_spaceManager->account();
    if (!account || !account->resourcesCache()) {
        return;
    }

    QUrl downloadUrl;
    if (!img->getWebDavUrl().isEmpty()) {
        downloadUrl = QUrl(img->getWebDavUrl());
        if (downloadUrl.isRelative()) {
            downloadUrl = Utility::concatUrlPath(account->url(), img->getWebDavUrl());
        }
    }

    if (!downloadUrl.isValid() || downloadUrl.isEmpty()) {
        QUrl baseDav = _space->webDavUrl();
        if (!baseDav.isValid() || baseDav.isEmpty() || baseDav.isRelative()) {
            const QString rootDavStr = _space->_drive.getRoot().getWebDavUrl().isEmpty()
                ? (QStringLiteral("dav/spaces/") + _space->id())
                : _space->_drive.getRoot().getWebDavUrl();
            baseDav = Utility::concatUrlPath(account->url(), rootDavStr);
        }

        const QString imageName = img->getName().isEmpty() ? QStringLiteral("image.png") : img->getName();
        const QString relativePath = imageName.startsWith(QLatin1String(".space/"))
            ? imageName
            : (QStringLiteral(".space/") + imageName);
        downloadUrl = Utility::concatUrlPath(baseDav, relativePath);
    }

    if (!downloadUrl.isValid() || downloadUrl.isEmpty()) {
        return;
    }

    if (_etag == newEtag && _url == downloadUrl && (_loading || !_image.isNull())) {
        return;
    }

    const auto generation = ++_generation;
    _etag = newEtag;
    _url = downloadUrl;
    _loading = true;

    auto job = account->resourcesCache()->makeGetJob(_url, this);
    // The catalog already supplies the revision; do not reuse cached bytes for a
    // URL whose contents changed under a new image ETag.
    job->setCacheLoadControl(QNetworkRequest::AlwaysNetwork);

    QObject::connect(job, &SimpleNetworkJob::finishedSignal, this, [job, this, generation] {
        job->deleteLater();
        if (generation != _generation) return;
        _loading = false;
        if (job->httpStatusCode() == 200) {
            const QIcon icon = job->asIcon();
            if (!icon.isNull()) {
                _image = icon;
                emit imageChanged();
            } else {
                _etag.clear();
            }
        } else {
            _etag.clear();
        }
    });
    job->start();
}
