// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <OAIDrive.h>
#include <QObject>
#include <QPointer>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QSaveFile>
#include <functional>
#include <memory>

namespace OCC {
class AccountState;
class SimpleNetworkJob;

// One cancellable workspace operation at a time. All writes are confirmed by
// the server before the catalog is refreshed; partial copies are never hidden.
class UDriveWorkspaceActions : public QObject
{
    Q_OBJECT
public:
    explicit UDriveWorkspaceActions(AccountState *state, QObject *parent = nullptr);
    ~UDriveWorkspaceActions() override;
    bool busy() const { return _busy; }
    void cancel();
    bool patch(const QList<OpenAPI::OAIDrive> &drives, const QJsonObject &payload);
    bool disable(const QList<OpenAPI::OAIDrive> &drives, bool restore = false);
    bool duplicate(const QList<OpenAPI::OAIDrive> &drives, const QStringList &names);
    bool readDescription(const OpenAPI::OAIDrive &drive);
    bool writeSpecial(const OpenAPI::OAIDrive &drive, const QString &kind, const QByteArray &content);
    bool download(const QList<OpenAPI::OAIDrive> &drives, const QString &destination);
    bool archiveAvailable() const;
Q_SIGNALS:
    void finished(bool success, const QString &message);
    void descriptionLoaded(const OpenAPI::OAIDrive &drive, const QString &text);
    void busyChanged();
private:
    struct Reply { int status; QByteArray body; QByteArray fileId; };
    using Callback = std::function<void(const Reply &)>;
    bool begin(const QList<OpenAPI::OAIDrive> &drives);
    bool available() const;
    void finish(bool success, const QString &message);
    void request(const QByteArray &verb, const QUrl &url, const QByteArray &body, const QNetworkRequest &headers, Callback callback,
        const QList<int> &accepted = {});
    QUrl graph(const QString &driveId = {}) const;
    QUrl archiveUrl() const;
    void patchNext(const QJsonObject &payload, const QByteArray &verb, bool restore);
    void duplicateNext();
    void copyNext();
    void linkNextSpecial();
    void registerSpecial(const OpenAPI::OAIDrive &drive, const QString &kind, const QString &path, const QByteArray &fileId, std::function<void()> next);
    void fileId(const OpenAPI::OAIDrive &drive, const QString &path, std::function<void(const QString &)> next);
    QPointer<AccountState> _state;
    QPointer<SimpleNetworkJob> _job;
    bool _busy = false;
    QList<OpenAPI::OAIDrive> _drives;
    QStringList _names;
    int _completed = 0;
    bool _partialCopy = false;
    OpenAPI::OAIDrive _source, _target;
    QStringList _children;
    QList<OpenAPI::OAIDriveItem> _special;
    std::unique_ptr<QSaveFile> _save;
    QByteArray _signature;
    QString _writeError;
};
}
