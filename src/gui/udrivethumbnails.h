// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "accountstate.h"
#include <QCache>
#include <QPersistentModelIndex>
#include <QPixmap>
#include <QSet>
#include <functional>

namespace OCC {
class SimpleNetworkJob;

// Private, in-memory previews belonging to one account and one current listing.
class UDriveThumbnails : public QObject
{
    Q_OBJECT
public:
    using Decoder = std::function<QImage(QByteArray, int)>;
    explicit UDriveThumbnails(AccountState *state, QObject *parent, Decoder decoder = {});
    ~UDriveThumbnails() override;
    void request(const QPersistentModelIndex &index, const QUrl &root, const QString &path, const QString &etag);
    void cancel(bool clearCache = false);
    void setPixels(int pixels);
    static bool supports(const QString &name);

Q_SIGNALS:
    void ready(const QPersistentModelIndex &index, const QPixmap &pixmap);

private:
    struct Request
    {
        QPersistentModelIndex index;
        QUrl url;
        QString etag;
        QString key;
        int pixels;
    };
    void pump();
    bool available() const;
    void finish(const Request &request, quint64 generation, const QImage &image);

    QPointer<AccountState> _state;
    QCache<QString, QPixmap> _cache{8 * 1024 * 1024};
    QList<Request> _queue;
    QList<QPointer<SimpleNetworkJob>> _jobs;
    QSet<QString> _pending;
    QSet<QString> _failed;
    quint64 _generation = 0;
    int _active = 0;
    int _decoding = 0;
    Decoder _decoder;
    int _pixels = 64;
};
}
