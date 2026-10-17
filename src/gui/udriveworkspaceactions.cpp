// SPDX-License-Identifier: GPL-2.0-or-later
#include "udriveworkspaceactions.h"
#include "accountstate.h"
#include "creds/localdevelopment.h"
#include "libsync/account.h"
#include "libsync/creds/abstractcredentials.h"
#include "libsync/networkjobs.h"
#include "common/utility.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QNetworkReply>
#include <QUrlQuery>
#include <QXmlStreamReader>
#include <QUuid>
#include <algorithm>

using namespace OCC;
namespace {
bool nameValid(const QString &name)
{
    return !name.isEmpty() && name != QStringLiteral(".") && name != QStringLiteral("..")
        && !name.contains(QLatin1Char('/')) && !name.contains(QLatin1Char('\\'))
        && std::none_of(name.cbegin(), name.cend(), [](QChar c) { return c.unicode() < 32 || c.unicode() == 127; });
}
bool active(const OpenAPI::OAIDrive &drive) { return drive.getRoot().getDeleted().getState() != QStringLiteral("trashed"); }
QUrl dav(const OpenAPI::OAIDrive &drive, const QString &path = {})
{
    return Utility::concatUrlPath(QUrl(drive.getRoot().getWebDavUrl()), path);
}
QByteArray json(const QJsonObject &value) { return QJsonDocument(value).toJson(QJsonDocument::Compact); }
QNetworkRequest jsonHeaders()
{
    QNetworkRequest headers;
    headers.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    return headers;
}
QNetworkRequest propHeaders(const QByteArray &depth)
{
    QNetworkRequest headers;
    headers.setRawHeader("Depth", depth);
    headers.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/xml; charset=utf-8"));
    return headers;
}
const QByteArray properties = R"(<?xml version="1.0"?><d:propfind xmlns:d="DAV:" xmlns:oc="http://owncloud.org/ns"><d:prop><d:resourcetype/><oc:fileid/></d:prop></d:propfind>)";
bool multiSucceeded(const QByteArray &body)
{
    QXmlStreamReader xml(body);
    if (!xml.readNextStartElement() || xml.name() != QLatin1String("multistatus") || xml.namespaceUri() != QLatin1String("DAV:")) return false;
    bool found = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QLatin1String("status") && xml.namespaceUri() == QLatin1String("DAV:")) {
            const auto parts = xml.readElementText().simplified().split(QLatin1Char(' '));
            const int status = parts.value(1).toInt();
            if (!parts.value(0).startsWith(QLatin1String("HTTP/")) || status < 200 || status >= 300) return false;
            found = true;
        }
    }
    return found && !xml.hasError();
}
// Do not replay ambiguous mutations or follow redirects with their payload.
class WorkspaceJob : public SimpleNetworkJob
{
public:
    using SimpleNetworkJob::SimpleNetworkJob;
    using AbstractNetworkJob::setQuery;
    bool needsRetry() const override
    {
        return reply() && httpStatusCode() == 401 && reply()->error() == QNetworkReply::AuthenticationRequiredError
            && reply()->attribute(QNetworkRequest::RedirectionTargetAttribute).isNull() && _attempts == 1 && SimpleNetworkJob::needsRetry();
    }
protected:
    void newReplyHook(QNetworkReply *reply) override { ++_attempts; SimpleNetworkJob::newReplyHook(reply); }
private:
    int _attempts = 0;
};
}

UDriveWorkspaceActions::UDriveWorkspaceActions(AccountState *state, QObject *parent) : QObject(parent), _state(state)
{
    if (state) {
        connect(state, &AccountState::stateChanged, this, [this] { if (!available()) cancel(); });
        connect(state, &QObject::destroyed, this, &UDriveWorkspaceActions::cancel);
    }
}
UDriveWorkspaceActions::~UDriveWorkspaceActions() { cancel(); }
bool UDriveWorkspaceActions::available() const
{
    return _state && !_state->isSignedOut() && _state->account() && _state->account()->credentials() && _state->account()->credentials()->ready();
}
bool UDriveWorkspaceActions::begin(const QList<OpenAPI::OAIDrive> &drives)
{
    if (_busy || !available() || drives.isEmpty()) return false;
    for (const auto &drive : drives)
        if (drive.getDriveType() != QStringLiteral("project") || !nameValid(drive.getId()) || !nameValid(drive.getRoot().getId())) return false;
    _busy = true; _drives = drives; _completed = 0; _partialCopy = false; _writeError.clear(); _signature.clear();
    Q_EMIT busyChanged();
    return true;
}
void UDriveWorkspaceActions::finish(bool success, const QString &message)
{
    if (!_busy) return;
    QString result = message;
    if (!success && _completed) result += tr(" %1 workspace(s) completed before the error.").arg(_completed);
    if (!success && _partialCopy) result += tr(" A new workspace or metadata file may be incomplete. Refresh before trying again.");
    _busy = false; _drives.clear(); _children.clear(); _special.clear(); _names.clear();
    if (_save) _save->cancelWriting();
    _save.reset();
    Q_EMIT busyChanged(); Q_EMIT finished(success, result);
}
void UDriveWorkspaceActions::cancel()
{
    auto job = _job; _job.clear();
    if (job) { job->abort(); job->deleteLater(); }
    finish(false, tr("Operation canceled. Refresh to check the server."));
}
QUrl UDriveWorkspaceActions::graph(const QString &id) const
{
    return Utility::concatUrlPath(_state->account()->url(), QStringLiteral("graph/v1.0/drives") + (id.isEmpty() ? QString() : QStringLiteral("/") + id));
}
void UDriveWorkspaceActions::request(const QByteArray &verb, const QUrl &url, const QByteArray &body,
    const QNetworkRequest &headers, Callback callback, const QList<int> &accepted)
{
    if (!_busy || !available()) { cancel(); return; }
    if (!LocalDevelopment::allowsAuthenticatedUrl(url, _state->account()->url()) || !url.userInfo().isEmpty() || !url.fragment().isEmpty()) {
        finish(false, tr("The server supplied an invalid workspace URL.")); return;
    }
    QNetworkRequest req(headers);
    QUrl root = url; root.setQuery(QString());
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    auto *job = new WorkspaceJob(_state->account(), root, QString(), verb, QByteArray(body), req, this);
    job->setQuery(QUrlQuery(url));
    _job = job;
    job->setTimeout(std::chrono::seconds(120));
    job->addNewReplyHook([this, job](QNetworkReply *reply) {
        connect(reply, &QIODevice::readyRead, this, [this, job, reply] {
            if (_job != job || job->reply() != reply) return;
            if (_save && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) {
                const auto bytes = reply->readAll();
                if (_signature.size() < 4) _signature += bytes.left(4 - _signature.size());
                if (_save->write(bytes) != bytes.size()) { _writeError = _save->errorString(); job->abort(); }
            } else if (reply->bytesAvailable() > 8 * 1024 * 1024) {
                _writeError = tr("The workspace response is too large."); job->abort();
            }
        });
    });
    connect(job, &SimpleNetworkJob::finishedSignal, this, [this, job, verb, accepted, callback = std::move(callback)] {
        if (_job != job) return;
        _job.clear(); job->deleteLater();
        if (!available()) { cancel(); return; }
        const int status = job->httpStatusCode();
        const bool ok = job->reply() && (job->reply()->error() == QNetworkReply::NoError || (status >= 400 && accepted.contains(status)))
            && (accepted.isEmpty() ? status >= 200 && status < 300 : accepted.contains(status));
        const auto body = job->reply() ? job->reply()->readAll() : QByteArray();
        if (!ok || !_writeError.isEmpty() || (status == 207 && verb != "PROPFIND" && !multiSucceeded(body))) {
            const QString message = !_writeError.isEmpty() ? _writeError : status == 403 ? tr("Permission denied by the server.")
                : status == 412 ? tr("The workspace changed. Refresh and try again.") : tr("Workspace operation failed (HTTP %1).").arg(status);
            finish(false, message); return;
        }
        callback({status, body, job->reply()->rawHeader("OC-FileId")});
    });
    job->start();
}
bool UDriveWorkspaceActions::patch(const QList<OpenAPI::OAIDrive> &drives, const QJsonObject &payload)
{
    if (payload.isEmpty() || !begin(drives)) return false;
    patchNext(payload, "PATCH", false); return true;
}
bool UDriveWorkspaceActions::disable(const QList<OpenAPI::OAIDrive> &drives, bool restore)
{
    if (!begin(drives)) return false;
    patchNext({}, restore ? "PATCH" : "DELETE", restore); return true;
}
void UDriveWorkspaceActions::patchNext(const QJsonObject &payload, const QByteArray &verb, bool restore)
{
    if (_drives.isEmpty()) { finish(true, tr("Workspace changes saved.")); return; }
    const auto drive = _drives.takeFirst();
    auto headers = jsonHeaders();
    if (!drive.getETag().isEmpty()) headers.setRawHeader("If-Match", drive.getETag().toUtf8());
    if (restore) headers.setRawHeader("Restore", "T");
    auto value = payload;
    if (!value.contains(QStringLiteral("name"))) value.insert(QStringLiteral("name"), drive.getName());
    request(verb, graph(drive.getId()), verb == "DELETE" ? QByteArray() : json(value), headers, [this, payload, verb, restore](const Reply &) {
        ++_completed; patchNext(payload, verb, restore);
    }, verb == "DELETE" ? QList<int>{200, 204} : QList<int>{200});
}
bool UDriveWorkspaceActions::readDescription(const OpenAPI::OAIDrive &drive)
{
    if (!begin({drive})) return false;
    for (const auto &special : drive.getSpecial()) {
        if (special.getSpecialFolder().getName() != QStringLiteral("readme")) continue;
        if (!nameValid(special.getName())) { finish(false, tr("The workspace description path is invalid.")); return true; }
        request("GET", dav(drive, QStringLiteral(".space/") + special.getName()), {}, {}, [this, drive](const Reply &reply) {
            if (reply.body.size() > 1024 * 1024) { finish(false, tr("The workspace description is too large.")); return; }
            const QString text = QString::fromUtf8(reply.body);
            finish(true, QString()); Q_EMIT descriptionLoaded(drive, text);
        }, {200});
        return true;
    }
    finish(true, QString()); Q_EMIT descriptionLoaded(drive, QString()); return true;
}
void UDriveWorkspaceActions::fileId(const OpenAPI::OAIDrive &drive, const QString &path, std::function<void(const QString &)> next)
{
    request("PROPFIND", dav(drive, path), properties, propHeaders("0"), [this, next](const Reply &reply) {
        QString id;
        LsColXMLParser parser;
        connect(&parser, &LsColXMLParser::directoryListingIterated, this, [&id](const QString &, const QMap<QString, QString> &props) { id = props.value(QStringLiteral("fileid")); });
        QHash<QString, qint64> sizes;
        if (!parser.parse(reply.body, &sizes, {}) || !nameValid(id)) { finish(false, tr("The server did not return a metadata file ID.")); return; }
        next(id);
    }, {207});
}
void UDriveWorkspaceActions::registerSpecial(const OpenAPI::OAIDrive &drive, const QString &kind, const QString &path, const QByteArray &id, std::function<void()> next)
{
    auto link = [this, drive, kind, next](const QString &file) {
        const QJsonObject value{{"name", drive.getName()}, {"special", QJsonArray{QJsonObject{{"id", file}, {"specialFolder", QJsonObject{{"name", kind}}}}}}};
        request("PATCH", graph(drive.getId()), json(value), jsonHeaders(), [next](const Reply &) { next(); }, {200});
    };
    if (nameValid(QString::fromUtf8(id))) link(QString::fromUtf8(id));
    else fileId(drive, path, link);
}
bool UDriveWorkspaceActions::writeSpecial(const OpenAPI::OAIDrive &drive, const QString &kind, const QByteArray &content)
{
    if ((kind != QStringLiteral("readme") && kind != QStringLiteral("image")) || content.size() > (kind == QStringLiteral("readme") ? 1024 * 1024 : 8 * 1024 * 1024) || !active(drive) || !begin({drive})) return false;
    const QString path = QStringLiteral(".space/udrive-") + QUuid::createUuid().toString(QUuid::WithoutBraces) + (kind == QStringLiteral("readme") ? QStringLiteral(".md") : QStringLiteral(".png"));
    request("MKCOL", dav(drive, QStringLiteral(".space")), {}, {}, [this, drive, kind, path, content](const Reply &) {
        QNetworkRequest headers; headers.setRawHeader("If-None-Match", "*");
        headers.setHeader(QNetworkRequest::ContentTypeHeader, kind == QStringLiteral("readme") ? QStringLiteral("text/markdown; charset=utf-8") : QStringLiteral("image/png"));
        request("PUT", dav(drive, path), content, headers, [this, drive, kind, path](const Reply &reply) {
            _partialCopy = true;
            registerSpecial(drive, kind, path, reply.fileId, [this] { _partialCopy = false; finish(true, tr("Workspace changes saved.")); });
        }, {200, 201, 204});
    }, {201, 405});
    return true;
}
bool UDriveWorkspaceActions::duplicate(const QList<OpenAPI::OAIDrive> &drives, const QStringList &names)
{
    if (drives.size() != names.size()) return false;
    for (const auto &name : names) if (!nameValid(name)) return false;
    for (const auto &drive : drives) if (!active(drive)) return false;
    if (!begin(drives)) return false;
    _names = names; duplicateNext(); return true;
}
void UDriveWorkspaceActions::duplicateNext()
{
    if (_drives.isEmpty()) { _partialCopy = false; finish(true, tr("Workspaces duplicated.")); return; }
    _source = _drives.takeFirst();
    QJsonObject value{{"name", _names.takeFirst()}, {"description", _source.getDescription()}};
    if (_source.getQuota().is_total_Valid() && _source.getQuota().getTotal() >= 0)
        value.insert(QStringLiteral("quota"), QJsonObject{{"total", _source.getQuota().getTotal()}});
    request("POST", graph(), json(value), jsonHeaders(), [this](const Reply &reply) {
        _partialCopy = true;
        _target.fromJsonObject(QJsonDocument::fromJson(reply.body).object());
        if (!nameValid(_target.getId()) || !nameValid(_target.getRoot().getId()) || _target.getRoot().getWebDavUrl().isEmpty()
            || dav(_target) == dav(_source)) { finish(false, tr("The server returned an invalid new workspace.")); return; }
        _children.clear(); _special = _source.getSpecial();
        request("PROPFIND", dav(_source), properties, propHeaders("1"), [this](const Reply &listing) {
            QString prefix = dav(_source).path(); if (!prefix.endsWith(QLatin1Char('/'))) prefix += QLatin1Char('/');
            bool valid = true;
            LsColXMLParser parser;
            connect(&parser, &LsColXMLParser::directoryListingIterated, this, [this, prefix, &valid](const QString &href, const QMap<QString, QString> &) {
                // LsColXMLParser already decoded the href. Parsing it as a URL
                // again would reinterpret literal %, # and ? in file names.
                QString path = href;
                if (!path.startsWith(QLatin1Char('/'))) {
                    const int scheme = path.indexOf(QStringLiteral("://"));
                    const int slash = scheme >= 0 ? path.indexOf(QLatin1Char('/'), scheme + 3) : -1;
                    const QUrl origin(slash >= 0 ? path.left(slash) : path);
                    const QUrl source = dav(_source);
                    const int defaultPort = source.scheme() == QStringLiteral("https") ? 443 : 80;
                    if (slash < 0 || !origin.isValid() || !origin.userInfo().isEmpty()
                        || origin.scheme() != source.scheme() || origin.host() != source.host()
                        || origin.port(defaultPort) != source.port(defaultPort)) { valid = false; return; }
                    path = path.mid(slash);
                }
                while (path.endsWith(QLatin1Char('/'))) path.chop(1);
                if (path + QLatin1Char('/') == prefix) return;
                if (!path.startsWith(prefix) || !nameValid(path.mid(prefix.size()))) { valid = false; return; }
                _children.append(path.mid(prefix.size()));
            });
            QHash<QString, qint64> sizes;
            if (!parser.parse(listing.body, &sizes, {}) || !valid) { finish(false, tr("Could not read all workspace contents.")); return; }
            _children.removeDuplicates(); copyNext();
        }, {207});
    }, {200, 201});
}
void UDriveWorkspaceActions::copyNext()
{
    if (_children.isEmpty()) { linkNextSpecial(); return; }
    const QString path = _children.takeFirst();
    const QUrl destination = dav(_target, path);
    if (!LocalDevelopment::allowsAuthenticatedUrl(destination, _state->account()->url()) || !destination.userInfo().isEmpty() || !destination.fragment().isEmpty()) {
        finish(false, tr("The server supplied an invalid workspace URL.")); return;
    }
    QNetworkRequest headers; headers.setRawHeader("Destination", destination.toEncoded());
    headers.setRawHeader("Overwrite", "F"); headers.setRawHeader("Depth", "infinity");
    request("COPY", dav(_source, path), {}, headers, [this](const Reply &) { copyNext(); }, {201, 204, 207});
}
void UDriveWorkspaceActions::linkNextSpecial()
{
    if (_special.isEmpty()) { ++_completed; _partialCopy = false; duplicateNext(); return; }
    const auto special = _special.takeFirst();
    const QString kind = special.getSpecialFolder().getName();
    if (kind != QStringLiteral("image") && kind != QStringLiteral("readme")) { linkNextSpecial(); return; }
    if (!nameValid(special.getName())) { finish(false, tr("The workspace metadata path is invalid.")); return; }
    registerSpecial(_target, kind, QStringLiteral(".space/") + special.getName(), {}, [this] { linkNextSpecial(); });
}
QUrl UDriveWorkspaceActions::archiveUrl() const
{
    if (!available()) return {};
    const auto files = _state->account()->capabilities().raw().value(QStringLiteral("files")).toMap();
    for (const auto &value : files.value(QStringLiteral("archivers")).toList()) {
        const auto archiver = value.toMap();
        if (!archiver.value(QStringLiteral("enabled")).toBool()) continue;
        QUrl url(archiver.value(QStringLiteral("archiver_url")).toString());
        if (url.isRelative()) url = Utility::concatUrlPath(_state->account()->url(), url.toString());
        const auto base = _state->account()->url();
        // The native client sends credentials directly instead of a signed
        // browser URL. Never send those credentials to a third-party archiver.
        if (url.scheme() == base.scheme() && url.host() == base.host() && url.port() == base.port()
            && url.userInfo().isEmpty() && url.fragment().isEmpty()) return url;
    }
    return {};
}
bool UDriveWorkspaceActions::archiveAvailable() const { return !archiveUrl().isEmpty(); }
bool UDriveWorkspaceActions::download(const QList<OpenAPI::OAIDrive> &drives, const QString &destination)
{
    if (destination.isEmpty() || archiveUrl().isEmpty()) return false;
    for (const auto &drive : drives) if (!active(drive)) return false;
    if (!begin(drives)) return false;
    _save = std::make_unique<QSaveFile>(destination);
    if (!_save->open(QIODevice::WriteOnly)) { finish(false, tr("Could not open the archive destination.")); return true; }
    QUrl url = archiveUrl(); QUrlQuery query(url);
    for (const auto &drive : drives) query.addQueryItem(QStringLiteral("id"), drive.getRoot().getId());
    url.setQuery(query);
    request("GET", url, {}, {}, [this](const Reply &reply) {
        if (_signature.size() < 4) _signature += reply.body.left(4 - _signature.size());
        const bool zip = _signature == QByteArray("PK\003\004", 4) || _signature == QByteArray("PK\005\006", 4) || _signature == QByteArray("PK\007\010", 4);
        if (!zip || _save->write(reply.body) != reply.body.size() || !_save->commit()) { finish(false, tr("The workspace archive could not be saved.")); return; }
        _save.reset(); finish(true, tr("Workspace archive saved."));
    }, {200});
    return true;
}
