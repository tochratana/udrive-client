// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QUrl>

namespace OCC::LocalDevelopment {

inline bool allowsHttp(const QUrl &url)
{
#ifdef QT_NO_DEBUG
    Q_UNUSED(url);
    return false;
#else
    // Explicit opt-in, Debug builds only. Do not resolve hostnames or permit
    // LAN/public addresses: this exception is for services on this computer.
    if (qgetenv("UDRIVE_ALLOW_LOCAL_HTTP") != QByteArrayLiteral("1")
        || !url.isValid() || url.scheme() != QStringLiteral("http")) return false;
    const QString host = url.host();
    return host == QStringLiteral("localhost") || host == QStringLiteral("127.0.0.1") || host == QStringLiteral("::1");
#endif
}

inline bool allowsServerUrl(const QUrl &url)
{
    return url.isValid() && !url.host().isEmpty() && (url.scheme() == QStringLiteral("https") || allowsHttp(url));
}

inline bool allowsAuthenticatedUrl(const QUrl &url, const QUrl &serverUrl)
{
    // Preserve the existing unit-test transport without extending HTTP to
    // remote accounts or server-advertised remote authentication endpoints.
    if (!allowsServerUrl(serverUrl) && serverUrl.scheme() != QStringLiteral("oauthtest")) return false;
    return url.isValid() && !url.host().isEmpty()
        && (url.scheme() == QStringLiteral("https") || url.scheme() == QStringLiteral("oauthtest")
            || (allowsHttp(serverUrl) && allowsHttp(url)));
}

inline bool allowsAuthenticationUrl(const QUrl &url, const QUrl &serverUrl)
{
    return allowsAuthenticatedUrl(url, serverUrl);
}

} // namespace OCC::LocalDevelopment
