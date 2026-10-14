#pragma once

#include "owncloudtheme.h"

namespace OCC {

class UDriveTheme : public ownCloudTheme
{
public:
    // Preserve existing accounts, sync roots and Keychain entries during development.
    QString appName() const override { return QStringLiteral("ownCloud"); }
    QString orgDomainName() const override { return QStringLiteral("com.owncloud.desktopclient"); }
    QString configFileName() const override { return QStringLiteral("owncloud.cfg"); }

    QColor wizardHeaderBackgroundColor() const override { return QColor(QStringLiteral("#083D39")); }
    QColor avatarColor() const override { return QColor(QStringLiteral("#00856B")); }
    QColor avatarColorChecked() const override { return QColor(QStringLiteral("#006653")); }
    QIcon aboutIcon() const override { return applicationIcon(); }

    QString about() const override
    {
        return tr("<p>UDrive brings your Unity Workspace files to your desktop.</p>"
                  "<p><a href=\"https://drive.unity-workspace.com\">Open UDrive online</a></p>"
                  "<p>Built on the ownCloud Desktop Client. Copyright ownCloud GmbH "
                  "(A Kiteworks Company) and contributors.</p>"
                  "<p>Distributed under the GNU General Public License (GPL), version 2 or later.</p>")
            + QStringLiteral("<p>%1</p>").arg(aboutVersions(VersionFormat::RichText));
    }
};

} // namespace OCC
