// SPDX-License-Identifier: GPL-2.0-or-later
#include "gui/udrivefilebrowser.h"
#include "gui/aboutdialog.h"
#include "gui/udrivetoast.h"
#include "gui/udrivefilepreview.h"
#include "gui/udriveworkspaceactions.h"
#include "gui/udrivecombobox.h"
#include "gui/settingsdialog.h"
#include "gui/accountmanager.h"
#include "gui/accountview.h"
#include "gui/logbrowser.h"
#include "gui/translations.h"
#include <QTranslator>
#include <QUrlQuery>
#include <QScrollBar>
#include <QScrollArea>
#include <QHeaderView>
#include <QTabBar>
#include <QTabWidget>
#include <QProcess>
#include <QElapsedTimer>
#include <QSemaphore>
#include <QThreadPool>
#include <QScopeGuard>
#include <atomic>
#include <QListView>
#include <QMouseEvent>
#include <QProxyStyle>
#include <QBuffer>
#include <QStandardItemModel>
#include "gui/generalsettings.h"
#include "gui/udrivethumbnails.h"
#include <QAbstractButton>
#include <QScreen>
#include <QStackedWidget>
#include <QSplitter>
#include <QPainter>
#include <QClipboard>
#include <QQuickWidget>
#include <QQuickItem>
#ifdef Q_OS_MACOS
#include "gui/udrivepdfpreview.h"
#include <QBuffer>
#include <QPdfWriter>
#include <QPainter>
#include <QSpinBox>
#include <signal.h>
#include <cerrno>
#endif
#include "gui/creds/credentials.h"
#include "gui/folderman.h"
#include "libsync/configfile.h"
#include "libsync/graphapi/spacesmanager.h"
#include "libsync/jobqueue.h"
#include "libsync/theme.h"
#include "resources/resources.h"
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QFontDatabase>
#include <QGlyphRun>
#include <QTextLayout>
#include <QImage>
#include <QDesktopServices>
#ifdef UDRIVE_HAS_MULTIMEDIA
#include <QMediaPlayer>
#include <QSlider>
#include <QVideoSink>
#include <QVideoFrame>
#include <QVideoWidget>
#endif
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QDateEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QCalendarWidget>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QLockFile>
#include <QTreeView>
#include <QTimer>
#include <QAction>
#include <QComboBox>
#include <QCompleter>
#include <QCheckBox>
#include <QRadioButton>
#include <QGroupBox>
#include <QPlainTextEdit>
#include <QDialogButtonBox>
#include <QAbstractSpinBox>
#include <QSet>
#include <QtTest>
#include <algorithm>

using namespace OCC;

class FileOpenReceiver : public QObject
{
    Q_OBJECT
public:
    QUrl opened;
    QByteArray openedContents;
public Q_SLOTS:
    void record(const QUrl &url)
    {
        opened = url;
        QFile file(url.toLocalFile());
        if (file.open(QIODevice::ReadOnly)) openedContents = file.readAll();
    }
};

namespace {
class RefreshingBrowserCredentials : public Credentials
{
public:
    using Credentials::Credentials;
    int refreshes = 0;
    void persist() override { }
    bool stillValid(QNetworkReply *reply) override
    {
        if (reply->error() == QNetworkReply::AuthenticationRequiredError) {
            ++refreshes;
            Q_EMIT authenticationStarted();
            QTimer::singleShot(20, this, [this] { _accessToken = QStringLiteral("refreshed-test-token"); Q_EMIT fetched(); });
        }
        return true;
    }
};

class DriveServer : public QTcpServer
{
public:
    QList<QPair<QByteArray, QString>> requests;
    bool authenticated = true;
    QJsonObject personalQuota;
    QJsonArray outgoingShares;
    QJsonArray incomingShares;
    int incomingStatus = 404;
    int incomingDelay = 0;
    QJsonArray extraDrives;
    QJsonArray globalPermissions{"Drives.Create.all", "Drives.ReadWriteEnabled.all", "Drives.ReadWriteProjectQuota.all"};
    int outgoingDelay = 0;
    int outgoingStatus = 200;
    int itemPermissionDelay = 0;
    int itemPermissionStatus = 207;
    QString itemPermissionHref;
    QMap<QString, QString> itemPermissions;
    QJsonObject workspacePermissions{{"value", QJsonArray{}},
        {"@libre.graph.permissions.actions.allowedValues", QJsonArray{"libre.graph/driveItem/permissions/create", "libre.graph/driveItem/permissions/delete",
            "libre.graph/driveItem/permissions/read", "libre.graph/driveItem/content/read", "libre.graph/driveItem/deleted/read"}},
        {"@libre.graph.permissions.roles.allowedValues", QJsonArray{QJsonObject{{"id", "viewer-role"}, {"displayName", "Viewer"}},
            QJsonObject{{"id", "editor-role"}, {"displayName", "Editor"}}}}};
    QString teamRootId = QStringLiteral("team");
    int workspaceMutationStatus = 200;
    int workspaceMutationDelay = 0;
    int workspaceStatus = 200;
    int workspaceDelay = 0;
    int workspaceInviteStatus = 201;
    QList<QJsonObject> workspaceInvitations;
    QStringList workspaceRemovals;
    QJsonObject recipientData;
    QList<QUrlQuery> recipientQueries;
    int recipientDelay = 0;
    int recipientStatus = 200;
    int sharePosts = 0;
    QString expectedShareReference;
    QString expectedSharePath;
    int sharePostDelay = 0;
    QList<QUrlQuery> shareQueries;
    QJsonArray existingShares;
    int shareGetStatus = 200;
    QStringList shareDeletions;
    QList<QUrlQuery> sharePuts;
    int sharePutStatus = 200;
    bool rejectUnknownShareId = false;
    QString sharePutMessage = QStringLiteral("Access denied");
    int sharePutDelay = 0;
    bool slowStarted = false;
    bool rejectFavorite = false;
    bool readOnly = false;
    bool holdUpload = false;
    bool holdTusPatch = false;
    int trashDeleteDelay = 0;
    QSet<QString> deniedTrash;
    QMap<QString, QString> tusSessions;
    QMap<QString, QByteArray> tusUploaded;
    int thumbnailDelay = 0;
    int activeThumbnails = 0;
    int maximumThumbnails = 0;
    QMap<QString, QByteArray> thumbnailResponses;
    QMap<QString, QUrlQuery> thumbnailQueries;
    int pdfResponse = 200;
    bool holdPdf = false;
    qint64 heldPdfSize = 1048576;
    // Override a mutation's response without applying it to the fixture state.
    QByteArray mutationVerb;
    QByteArray mutationResponse;
    QByteArray rejectVerb;
    int rejectStatus = 401;
    int rejectionsLeft = 0;
    QSet<QString> folders;
    QMap<QString, QByteArray> uploaded;
    QMap<QString, qint64> sizes;
    QMap<QString, QString> modifiedDates;
    QSet<QString> removed;
    QSet<QString> favorites;
    QByteArray favoritesReport;
    QString graphDavPrefix = QStringLiteral("/dav");
    QMap<QString, QString> trash;
    QMap<QString, QByteArray> requestBodies;
    QMap<QString, QMap<QByteArray, QByteArray>> requestHeaders;
    const QByteArray fileContent = QByteArray(512 * 1024, 'U');

    DriveServer(bool includeShares = false)
    {
        listen(QHostAddress::LocalHost, 0);
        connect(this, &QTcpServer::newConnection, this, [this, includeShares] {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                auto data = std::make_shared<QByteArray>();
                auto handled = std::make_shared<bool>(false);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, data, handled, includeShares] {
                    if (*handled) return;
                    *data += socket->readAll();
                    const int headerEnd = data->indexOf("\r\n\r\n");
                    if (headerEnd < 0) return;
                    qint64 length = 0;
                    for (const auto &line : data->left(headerEnd).split('\n')) {
                        if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toLongLong();
                    }
                    if (data->size() < headerEnd + 4 + length) return;
                    *handled = true;
                    const auto parts = data->left(data->indexOf('\r')).split(' ');
                    const QString path = QUrl::fromEncoded(parts.at(1)).path();
                    QMap<QByteArray, QByteArray> headers;
                    for (const auto &line : data->left(headerEnd).split('\n')) {
                        const int colon = line.indexOf(':');
                        if (colon >= 0) headers.insert(line.left(colon).toLower().trimmed(), line.mid(colon + 1).trimmed());
                    }
                    const QString key = QString::fromLatin1(parts.at(0)) + QLatin1Char(' ') + path;
                    requestHeaders.insert(key, headers);
                    requestBodies.insert(key, data->mid(headerEnd + 4, length));
                    requests.append({parts.at(0), path});
                    authenticated &= data->toLower().contains("authorization: bearer browser-test-token\r\n");
                    if (parts.at(0) == rejectVerb && rejectionsLeft > 0) {
                        --rejectionsLeft;
                        respond(socket, rejectStatus, "text/plain", "Rejected before mutation");
                    } else if (path == QStringLiteral("/graph/v1.0/me")) {
                        respond(socket, 200, "application/json", "{\"id\":\"self\"}");
                    } else if (path == QStringLiteral("/api/v0/settings/permissions-list")) {
                        respond(socket, 200, "application/json", QJsonDocument(QJsonObject{{"permissions", globalPermissions}}).toJson());
                    } else if (parts.at(0) == "GET" && path.startsWith(QStringLiteral("/graph/v1beta1/drives/")) && path.endsWith(QStringLiteral("/root/permissions"))) {
                        const auto body = QJsonDocument(workspacePermissions).toJson();
                        const int status = workspaceStatus;
                        const QPointer<QTcpSocket> guard(socket);
                        QTimer::singleShot(workspaceDelay, this, [this, guard, body, status] {
                            if (guard && guard->state() == QAbstractSocket::ConnectedState)
                                respond(guard, status, "application/json", body);
                        });
                    } else if (parts.at(0) == "POST" && path == QStringLiteral("/graph/v1beta1/drives/team/root/invite")) {
                        const auto invite = QJsonDocument::fromJson(requestBodies.value(key)).object();
                        workspaceInvitations.append(invite);
                        if (workspaceInviteStatus == 201) {
                            const auto recipient = invite.value("recipients").toArray().first().toObject();
                            const bool group = recipient.value("@libre.graph.recipient.type") == QStringLiteral("group");
                            const QString id = recipient.value("objectId").toString();
                            const auto identity = QJsonObject{{"id", id}, {"displayName", "Added member"}};
                            auto permissions = workspacePermissions.value("value").toArray();
                            permissions.append(QJsonObject{{"id", QString((group ? QStringLiteral("g:") : QStringLiteral("u:")) + id)}, {"roles", invite.value("roles")},
                                {"grantedToV2", QJsonObject{{group ? "group" : "user", identity}}}});
                            workspacePermissions.insert("value", permissions);
                        }
                        respond(socket, workspaceInviteStatus, "application/json", "{\"id\":\"member\"}");
                    } else if (parts.at(0) == "DELETE" && path.startsWith(QStringLiteral("/graph/v1beta1/drives/team/root/permissions/"))) {
                        const QString permission = path.section('/', -1);
                        workspaceRemovals.append(permission);
                        QJsonArray remaining;
                        for (const auto &p : workspacePermissions.value("value").toArray())
                            if (p.toObject().value("id").toString() != permission) remaining.append(p);
                        workspacePermissions.insert("value", remaining);
                        respond(socket, 204, "application/json", {});
                    } else if (parts.at(0) == "GET" && path == QStringLiteral("/ocs/v1.php/apps/files_sharing/api/v1/sharees")) {
                        recipientQueries.append(QUrlQuery(QUrl::fromEncoded(parts.at(1))));
                        const auto body = QJsonDocument(QJsonObject{{"ocs", QJsonObject{
                            {"meta", QJsonObject{{"statuscode", 100}}}, {"data", recipientData}}}}).toJson();
                        const QPointer<QTcpSocket> guard(socket);
                        const int status = recipientStatus;
                        QTimer::singleShot(recipientDelay, this, [this, guard, body, status] {
                            if (guard && guard->state() == QAbstractSocket::ConnectedState)
                                respond(guard, status, "application/json", body);
                        });
                    } else if (parts.at(0) == "POST" && path == QStringLiteral("/ocs/v1.php/apps/files_sharing/api/v1/shares")) {
                        ++sharePosts;
                        const QUrlQuery arguments(QString::fromUtf8(requestBodies.value(key)));
                        if ((!expectedShareReference.isEmpty() && arguments.queryItemValue("space_ref", QUrl::FullyDecoded) != expectedShareReference)
                            || (!expectedSharePath.isEmpty() && arguments.queryItemValue("path", QUrl::FullyDecoded) != expectedSharePath)) {
                            respond(socket, 400, "application/json", R"({"ocs":{"meta":{"statuscode":400,"message":"Can not share space root"}}})");
                            return;
                        }
                        const QPointer<QTcpSocket> guard(socket);
                        QTimer::singleShot(sharePostDelay, this, [this, guard, arguments] {
                            if (guard && guard->state() == QAbstractSocket::ConnectedState) {
                                if (arguments.queryItemValue("shareType") == "3") {
                                    const QString shareId = QStringLiteral("public-share-") + QString::number(existingShares.size() + 1);
                                    const QString publicUrl = url(QStringLiteral("/s/") + shareId).toString();
                                    const QJsonObject shareObj{
                                        {"id", shareId},
                                        {"share_type", 3},
                                        {"permissions", arguments.queryItemValue("permissions").toInt()},
                                        {"name", arguments.queryItemValue("name")},
                                        {"url", publicUrl},
                                        {"token", shareId}
                                    };
                                    existingShares.append(shareObj);
                                    const auto body = QJsonDocument(QJsonObject{{"ocs", QJsonObject{{"meta", QJsonObject{{"statuscode", 100}}}, {"data", shareObj}}}}).toJson();
                                    respond(guard, 200, "application/json", body);
                                } else {
                                    respond(guard, 200, "application/json", R"({"ocs":{"meta":{"statuscode":100},"data":{"id":"test-share"}}})");
                                }
                            }
                        });
                    } else if (parts.at(0) == "PUT" && path.startsWith(QStringLiteral("/ocs/v1.php/apps/files_sharing/api/v1/shares/"))) {
                        const QString shareId = path.section('/', -1);
                        const QUrlQuery arguments(QString::fromUtf8(requestBodies.value(key)));
                        sharePuts.append(arguments);
                        if (sharePutStatus != 200) {
                            const auto body = QJsonDocument(QJsonObject{{"ocs", QJsonObject{
                                {"meta", QJsonObject{{"statuscode", sharePutStatus}, {"message", sharePutMessage}}}, {"data", QJsonObject{}}}}}).toJson();
                            const QPointer<QTcpSocket> guard(socket);
                            const int status = sharePutStatus;
                            QTimer::singleShot(sharePutDelay, this, [this, guard, status, body] {
                                if (guard && guard->state() == QAbstractSocket::ConnectedState)
                                    respond(guard, status, "application/json", body);
                            });
                            return;
                        }
                        bool found = false;
                        for (int i = 0; i < existingShares.size(); ++i) {
                            auto obj = existingShares.at(i).toObject();
                            if (obj.value("id").toString() == shareId) {
                                found = true;
                                if (arguments.hasQueryItem("permissions")) obj.insert("permissions", arguments.queryItemValue("permissions").toInt());
                                if (arguments.hasQueryItem("name")) obj.insert("name", arguments.queryItemValue("name"));
                                if (arguments.hasQueryItem("expireDate")) obj.insert("expiration", arguments.queryItemValue("expireDate"));
                                if (arguments.hasQueryItem("password")) obj.insert("share_with", !arguments.queryItemValue("password").isEmpty());
                                existingShares.replace(i, obj);
                                break;
                            }
                        }
                        if (rejectUnknownShareId && !found) {
                            respond(socket, 200, "application/json", R"({"ocs":{"meta":{"statuscode":404,"message":"cannot find share"},"data":{}}})");
                            return;
                        }
                        respond(socket, 200, "application/json", R"({"ocs":{"meta":{"statuscode":100},"data":{}}})");
                    } else if (parts.at(0) == "DELETE" && path.startsWith(QStringLiteral("/ocs/v1.php/apps/files_sharing/api/v1/shares/"))) {
                        const QString shareId = path.section('/', -1);
                        shareDeletions.append(shareId);
                        QJsonArray remaining;
                        for (const auto &s : existingShares) {
                            if (s.toObject().value("id").toString() != shareId) remaining.append(s);
                        }
                        existingShares = remaining;
                        respond(socket, 200, "application/json", R"({"ocs":{"meta":{"statuscode":100},"data":[]}})");
                    } else if (parts.at(0) == "GET" && path == QStringLiteral("/ocs/v1.php/apps/files_sharing/api/v1/shares")) {
                        shareQueries.append(QUrlQuery(QUrl::fromEncoded(parts.at(1))));
                        const auto body = QJsonDocument(QJsonObject{{"ocs", QJsonObject{{"meta", QJsonObject{{"status", "ok"}, {"statuscode", 100}}}, {"data", existingShares}}}}).toJson();
                        respond(socket, shareGetStatus, "application/json", body);
                    } else if (parts.at(0) == "GET" && path == QStringLiteral("/graph/v1beta1/me/drive/sharedWithMe")) {
                        const auto body = QJsonDocument(QJsonObject{{"value", incomingShares}}).toJson();
                        const int status = incomingStatus;
                        const QPointer<QTcpSocket> guard(socket);
                        QTimer::singleShot(incomingDelay, this, [this, guard, body, status] {
                            if (guard && guard->state() == QAbstractSocket::ConnectedState)
                                respond(guard, status, "application/json", body);
                        });
                    } else if (parts.at(0) == "GET" && path == QStringLiteral("/graph/v1beta1/me/drive/sharedByMe")) {
                        const auto body = QJsonDocument(QJsonObject{{"value", outgoingShares}}).toJson();
                        const int status = outgoingStatus;
                        const QPointer<QTcpSocket> guard(socket);
                        QTimer::singleShot(outgoingDelay, this, [this, guard, body, status] {
                            if (guard && guard->state() == QAbstractSocket::ConnectedState)
                                respond(guard, status, "application/json", body);
                        });
                    } else if (path == QStringLiteral("/graph/v1.0/me/drives")) {
                        QJsonArray drives;
                        QStringList ids {QStringLiteral("personal"), QStringLiteral("team")};
                        if (includeShares) ids.append(QStringLiteral("a0ca6a90-a365-4782-871e-d44447bbc668$a0ca6a90-a365-4782-871e-d44447bbc668"));
                        for (const auto &id : ids) {
                            drives.append(QJsonObject{{"id", id}, {"name", id == "team" ? "Team files" : "Personal"},
                                {"driveType", id == "personal" ? "personal" : "project"}, {"eTag", "drive-tag"},
                                {"quota", id == "personal" ? personalQuota : QJsonObject{}},
                                {"root", QJsonObject{{"id", id == "team" ? teamRootId : id}, {"eTag", "root-tag"},
                                    {"webDavUrl", url(graphDavPrefix + "/" + id + "/").toString()}, {"webUrl", url("/web/" + id).toString()}}}});
                        }
                        for (const auto &drive : extraDrives) drives.append(drive);
                        respond(socket, 200, "application/json", QJsonDocument(QJsonObject{{"value", drives}}).toJson());
                    } else if (parts.at(0) == "PATCH" && path.startsWith(QStringLiteral("/graph/v1.0/drives/"))) {
                        const int status = workspaceMutationStatus;
                        if (status == 200) for (int i = 0; i < extraDrives.size(); ++i) {
                            auto drive = extraDrives.at(i).toObject();
                            if (drive.value("id").toString() != path.section('/', -1)) continue;
                            const auto patch = QJsonDocument::fromJson(requestBodies.value(key)).object();
                            for (auto it = patch.begin(); it != patch.end(); ++it) drive.insert(it.key(), it.value());
                            extraDrives[i] = drive;
                        }
                        const QPointer<QTcpSocket> guard(socket);
                        QTimer::singleShot(workspaceMutationDelay, this, [this, guard, status] {
                            if (guard && guard->state() == QAbstractSocket::ConnectedState) respond(guard, status, "application/json", "{}");
                        });
                    } else if (parts.at(0) == "GET" && QUrlQuery(QUrl::fromEncoded(parts.at(1))).queryItemValue("preview") == "1") {
                        thumbnailQueries.insert(path, QUrlQuery(QUrl::fromEncoded(parts.at(1))));
                        ++activeThumbnails;
                        maximumThumbnails = qMax(maximumThumbnails, activeThumbnails);
                        const QPointer<QTcpSocket> guard(socket);
                        const auto body = thumbnailResponses.value(path);
                        QTimer::singleShot(thumbnailDelay, this, [this, guard, body] {
                            --activeThumbnails;
                            if (guard && guard->state() == QAbstractSocket::ConnectedState)
                                respond(guard, body.isEmpty() ? 404 : 200, "image/png", body);
                        });
                    } else if (!mutationVerb.isEmpty() && parts.at(0) == mutationVerb) {
                        respond(socket, 207, "application/xml; charset=utf-8", mutationResponse);
                    } else if (parts.at(0) == "MKCOL") {
                        if (readOnly) respond(socket, 403, "text/plain", "Forbidden");
                        else if (exists(path)) respond(socket, 405, "text/plain", "Exists");
                        else { folders.insert(path); respond(socket, 201, "text/plain", {}); }
                    } else if (parts.at(0) == "PUT") {
                        if (readOnly) respond(socket, 403, "text/plain", "Forbidden");
                        else if (exists(path)) respond(socket, 412, "text/plain", "Exists");
                        else if (!holdUpload) { removed.remove(path); uploaded.insert(path, requestBodies.value(key)); respond(socket, 201, "text/plain", {}); }
                    } else if (parts.at(0) == "POST" && headers.contains("tus-resumable")) {
                        if (holdUpload) {
                            slowStarted = true;
                            return;
                        }
                        const QByteArray metaHeader = headers.value("upload-metadata");
                        QString targetPath;
                        const auto entries = metaHeader.split(',');
                        for (const auto &entry : entries) {
                            const auto pair = entry.trimmed().split(' ');
                            if (pair.size() == 2 && pair[0] == "filename") {
                                targetPath = QString::fromUtf8(QByteArray::fromBase64(pair[1]));
                            }
                        }
                        if (!targetPath.startsWith('/')) targetPath.prepend('/');
                        QString base = path;
                        while (base.endsWith('/')) base.chop(1);
                        if (!targetPath.startsWith(base)) targetPath = base + targetPath;
                        const QString sessionId = QStringLiteral("/tus-session-%1").arg(tusSessions.size() + 1);
                        const QByteArray chunk = data->mid(headerEnd + 4, length);
                        tusSessions.insert(sessionId, targetPath);
                        tusUploaded.insert(targetPath, chunk);
                        if (chunk.size() >= headers.value("upload-length").toLongLong()) {
                            uploaded.insert(targetPath, chunk);
                        }
                        QMap<QByteArray, QByteArray> extraHeaders;
                        extraHeaders.insert("Location", sessionId.toUtf8());
                        extraHeaders.insert("Tus-Resumable", "1.0.0");
                        extraHeaders.insert("Upload-Offset", QByteArray::number(chunk.size()));
                        respond(socket, 201, "text/plain", {}, extraHeaders);
                    } else if (parts.at(0) == "PATCH" && headers.contains("tus-resumable")) {
                        if (holdTusPatch) return;
                        const QString targetPath = tusSessions.value(path);
                        const QByteArray chunk = data->mid(headerEnd + 4, length);
                        tusUploaded[targetPath].append(chunk);
                        uploaded.insert(targetPath, tusUploaded[targetPath]);
                        QMap<QByteArray, QByteArray> extraHeaders;
                        extraHeaders.insert("Tus-Resumable", "1.0.0");
                        extraHeaders.insert("Upload-Offset", QByteArray::number(tusUploaded[targetPath].size()));
                        respond(socket, 204, "text/plain", {}, extraHeaders);
                    } else if (parts.at(0) == "PROPPATCH") {
                        if (!rejectFavorite) {
                            if (requestBodies.value(key).contains(">true<")) favorites.insert(path);
                            else favorites.remove(path);
                        }
                        respond(socket, 207, "application/xml; charset=utf-8",
                            QByteArray("<d:multistatus xmlns:d=\"DAV:\"><d:response><d:propstat><d:prop/><d:status>HTTP/1.1 ")
                            + (rejectFavorite ? "403 Forbidden" : "200 OK") + "</d:status></d:propstat></d:response></d:multistatus>");
                    } else if (parts.at(0) == "REPORT") {
                        if (!favoritesReport.isEmpty()) {
                            respond(socket, 207, "application/xml; charset=utf-8", favoritesReport);
                            return;
                        }
                        QByteArray xml = "<d:multistatus xmlns:d=\"DAV:\" xmlns:oc=\"http://owncloud.org/ns\">";
                        for (const auto &fav : favorites) xml += entry(fav, folders.contains(fav));
                        respond(socket, 207, "application/xml; charset=utf-8", xml + "</d:multistatus>");
                    } else if (parts.at(0) == "MOVE" || parts.at(0) == "COPY") {
                        const QString destination = QUrl::fromEncoded(headers.value("destination")).path();
                        if (exists(destination)) respond(socket, 412, "text/plain", "Exists");
                        else if (path.startsWith("/dav/spaces/trash-bin/")) {
                            const QString id = path.section('/', -1);
                            if (!trash.contains(id)) respond(socket, 404, "text/plain", "Missing");
                            else { removed.remove(trash.take(id)); uploaded.insert(destination, fileContent); respond(socket, 201, "text/plain", {}); }
                        } else if (!exists(path) || removed.contains(path)) respond(socket, 404, "text/plain", "Missing");
                        else {
                            if (folders.contains(path) || path.endsWith("/Docs")) folders.insert(destination);
                            else uploaded.insert(destination, uploaded.value(path, fileContent));
                            if (parts.at(0) == "MOVE") { removed.insert(path); uploaded.remove(path); folders.remove(path); }
                            respond(socket, 201, "text/plain", {});
                        }
                    } else if (parts.at(0) == "DELETE") {
                        if (path.startsWith("/dav/spaces/trash-bin/")) {
                            if (readOnly || deniedTrash.contains(path.section('/', -1))) respond(socket, 403, "text/plain", "Forbidden");
                            else if (!trash.remove(path.section('/', -1))) respond(socket, 404, "text/plain", "Missing");
                            else {
                                const QPointer<QTcpSocket> guard(socket);
                                QTimer::singleShot(trashDeleteDelay, this, [this, guard] {
                                    if (guard && guard->state() == QAbstractSocket::ConnectedState) respond(guard, 204, "text/plain", {});
                                });
                            }
                        } else if (!exists(path)) respond(socket, 404, "text/plain", "Missing");
                        else { removed.insert(path); trash.insert(QStringLiteral("deleted-%1").arg(trash.size() + 1), path); respond(socket, 204, "text/plain", {}); }
                    } else if (parts.at(0) == "PROPFIND") {
                        if (path.contains("/meta/")) {
                            const QByteArray versionsXml =
                                "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                                "<d:multistatus xmlns:d=\"DAV:\" xmlns:oc=\"http://owncloud.org/ns\">\n"
                                "  <d:response>\n"
                                "    <d:href>" + path.toUtf8() + "</d:href>\n"
                                "    <d:propstat>\n"
                                "      <d:prop><d:resourcetype><d:collection/></d:resourcetype></d:prop>\n"
                                "      <d:status>HTTP/1.1 200 OK</d:status>\n"
                                "    </d:propstat>\n"
                                "  </d:response>\n"
                                "  <d:response>\n"
                                "    <d:href>" + path.toUtf8() + "/1728390000</d:href>\n"
                                "    <d:propstat>\n"
                                "      <d:prop>\n"
                                "        <d:getcontentlength>72704</d:getcontentlength>\n"
                                "        <d:getlastmodified>Thu, 08 Oct 2026 12:20:00 GMT</d:getlastmodified>\n"
                                "        <d:getetag>&quot;etag-v1&quot;</d:getetag>\n"
                                "        <d:resourcetype/>\n"
                                "      </d:prop>\n"
                                "      <d:status>HTTP/1.1 200 OK</d:status>\n"
                                "    </d:propstat>\n"
                                "  </d:response>\n"
                                "</d:multistatus>";
                            respond(socket, 207, "application/xml; charset=utf-8", versionsXml);
                            return;
                        }
                        if (headers.value("depth") == "0") {
                            const auto permissions = itemPermissions.value(path, readOnly ? QStringLiteral("S") : QStringLiteral("RDNVCKW"));
                            const auto href = itemPermissionHref.isEmpty() ? path : itemPermissionHref;
                            const auto xml = QStringLiteral("<d:multistatus xmlns:d=\"DAV:\" xmlns:oc=\"http://owncloud.org/ns\"><d:response><d:href>%1</d:href><d:propstat><d:prop><oc:permissions>%2</oc:permissions><d:getetag>&quot;tag&quot;</d:getetag></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response></d:multistatus>")
                                .arg(href.toHtmlEscaped(), permissions.toHtmlEscaped()).toUtf8();
                            const QPointer<QTcpSocket> guard(socket);
                            const auto status = itemPermissionStatus;
                            QTimer::singleShot(itemPermissionDelay, this, [this, guard, status, xml] {
                                if (guard && guard->state() == QAbstractSocket::ConnectedState) respond(guard, status, "application/xml; charset=utf-8", xml);
                            });
                            return;
                        }
                        QByteArray xml = "<?xml version=\"1.0\"?><d:multistatus xmlns:d=\"DAV:\">";
                        xml += entry(path, true);
                        if (path == graphDavPrefix + "/personal") {
                            for (const auto &name : {"Docs", "slowfolder", "a&b.txt", "%2F.txt", "denied.txt", "slow.txt", "truncated.txt"})
                                if (!removed.contains(path + '/' + name)) xml += entry(path + '/' + name, QByteArray(name) == "Docs" || QByteArray(name) == "slowfolder");
                            xml += entry(path + "/../escape", false);
                            xml += entry(path + "/Docs/not-a-direct-child", false);
                        } else if (path == graphDavPrefix + "/personal/Docs") {
                            xml += entry(path + "/Budget 2026.pdf", false);
                        } else if (path == graphDavPrefix + "/personal/slowfolder") {
                            xml += entry(path + "/stale.txt", false);
                        } else if (path == graphDavPrefix + "/team") {
                            xml += entry(path + "/Roadmap.pdf", false);
                        } else if (path.startsWith("/dav/spaces/trash-bin/")) {
                            for (auto it = trash.cbegin(); it != trash.cend(); ++it) {
                                xml += entry(path + '/' + it.key(), false, it.value());
                            }
                        } else if (includeShares && path.contains("a0ca6a90-")) {
                            xml += entry(path + "/Shared document.pdf", false);
                        }
                        for (const auto &folder : folders) if (!removed.contains(folder) && folder.section('/', 0, -2) == path) xml += entry(folder, true);
                        for (auto it = uploaded.cbegin(); it != uploaded.cend(); ++it) if (!removed.contains(it.key()) && it.key().section('/', 0, -2) == path) xml += entry(it.key(), false);
                        xml += "</d:multistatus>";
                        if (path.endsWith("slowfolder")) {
                            QTimer::singleShot(150, socket, [this, socket, xml] { if (socket->state() == QAbstractSocket::ConnectedState) respond(socket, 207, "application/xml; charset=utf-8", xml); });
                        } else respond(socket, 207, "application/xml; charset=utf-8", xml);
                    } else if (path.endsWith(".pdf", Qt::CaseInsensitive) && holdPdf) {
                        socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(heldPdfSize) + "\r\nContent-Type: application/pdf\r\n\r\npartial");
                        slowStarted = true;
                    } else if (path.endsWith(".pdf", Qt::CaseInsensitive) && pdfResponse != 200) {
                        respond(socket, pdfResponse, "text/plain", "PDF unavailable");
                    } else if (path.endsWith("denied.txt")) {
                        respond(socket, 403, "text/plain", "Forbidden");
                    } else if (path.endsWith("slow.txt")) {
                        socket->write("HTTP/1.1 200 OK\r\nContent-Length: 1048576\r\nContent-Type: application/octet-stream\r\n\r\npartial");
                        slowStarted = true;
                    } else if (path.endsWith("truncated.txt")) {
                        socket->write("HTTP/1.1 200 OK\r\nContent-Length: 1048576\r\nContent-Type: application/octet-stream\r\nConnection: close\r\n\r\npartial");
                        socket->disconnectFromHost();
                    } else respond(socket, 200, "application/octet-stream", uploaded.value(path, fileContent));
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    QUrl url(const QString &path = {}) const
    {
        QUrl url(QStringLiteral("http://127.0.0.1:%1").arg(serverPort()));
        url.setPath(path);
        return url;
    }
    bool exists(const QString &path) const
    {
        return !removed.contains(path) && (folders.contains(path) || uploaded.contains(path)
            || QStringList{"Docs", "slowfolder", "a&b.txt", "%2F.txt", "denied.txt", "slow.txt", "truncated.txt"}.contains(path.mid(QStringLiteral("/dav/personal/").size())));
    }
    QByteArray entry(const QString &path, bool folder, const QString &original = {}) const
    {
        const QString href = url(path).path(QUrl::FullyEncoded).toHtmlEscaped();
        return QStringLiteral("<d:response xmlns:oc=\"http://owncloud.org/ns\"><d:href>%1</d:href><d:propstat><d:prop><d:resourcetype>%2</d:resourcetype>"
                              "<d:getetag>&quot;tag&quot;</d:getetag><oc:favorite>%4</oc:favorite><oc:permissions>%5</oc:permissions><oc:privatelink>%6</oc:privatelink>"
                              "<oc:trashbin-original-filename>%7</oc:trashbin-original-filename><oc:trashbin-original-location>%8</oc:trashbin-original-location>"
                              "<oc:trashbin-delete-datetime>2026-10-09T08:00:00Z</oc:trashbin-delete-datetime>"
                              "<d:getcontentlength>%9</d:getcontentlength><oc:size>%9</oc:size><d:getlastmodified>%3</d:getlastmodified>"
                              "<oc:fileid>file-123</oc:fileid><oc:id>file-123</oc:id>"
                              "</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>")
            .arg(href, folder ? QStringLiteral("<d:collection/>") : QString(),
                modifiedDates.value(path, path.endsWith("denied.txt") ? QStringLiteral("invalid date") : QStringLiteral("Wed, 01 Oct 2025 10:00:00 GMT")),
                favorites.contains(path) ? QStringLiteral("1") : QStringLiteral("0"),
                readOnly ? QStringLiteral("S") : QStringLiteral("RDNVCKW"), url("/f" + path).toString().toHtmlEscaped(),
                original.section('/', -1).toHtmlEscaped(), original.mid(QStringLiteral("/dav/personal/").size()).toHtmlEscaped(),
                QString::number(sizes.value(path, 524288))).toUtf8();
    }
    void respond(QTcpSocket *socket, int status, const QByteArray &type, const QByteArray &body, const QMap<QByteArray, QByteArray> &extraHeaders = {})
    {
        QByteArray resp = "HTTP/1.1 " + QByteArray::number(status) + " Response\r\nContent-Type: " + type
            + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n";
        for (auto it = extraHeaders.cbegin(); it != extraHeaders.cend(); ++it) {
            resp += it.key() + ": " + it.value() + "\r\n";
        }
        resp += "\r\n" + body;
        socket->write(resp);
        socket->disconnectFromHost();
    }
};

// Observe the visible drag-selection overlay without relying on private Qt state.
class SelectionPaintStyle : public QProxyStyle
{
public:
    SelectionPaintStyle() : QProxyStyle(QStringLiteral("Fusion")) { }
    mutable int rubberBands = 0;
    void drawControl(ControlElement element, const QStyleOption *option, QPainter *painter, const QWidget *widget = nullptr) const override
    {
        if (element == QStyle::CE_RubberBand) ++rubberBands;
        QProxyStyle::drawControl(element, option, painter, widget);
    }
};

QModelIndex select(QTreeView *tree, const QString &name)
{
    for (int row = 0; row < tree->model()->rowCount(); ++row) {
        const auto index = tree->model()->index(row, 0);
        if (index.data().toString() == name) {
            tree->setCurrentIndex(index);
            return index;
        }
    }
    return {};
}

QByteArray contents(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}
#ifdef Q_OS_MACOS
QByteArray pdfFixture(QSizeF dimensions = QSizeF(300, 400))
{
    QByteArray data;
    QBuffer buffer(&data);
    buffer.open(QIODevice::WriteOnly);
    {
        QPdfWriter writer(&buffer);
        writer.setResolution(72);
        writer.setPageSize(QPageSize(dimensions, QPageSize::Point));
        writer.setPageMargins(QMarginsF(0, 0, 0, 0));
        QPainter painter(&writer);
        painter.fillRect(QRect(20, 20, 60, 30), Qt::red);
        painter.drawText(QRect(20, 90, 250, 40), "UDrive PDF preview — page one");
        writer.newPage();
        painter.fillRect(QRect(20, 20, 60, 30), Qt::blue);
        painter.drawText(QRect(20, 90, 250, 40), "UDrive PDF preview — page two");
    }
    return data;
}

QByteArray rotatedPdfFixture()
{
    // A real PDF with a rotated crop box distinct from its media box.
    QList<QByteArray> objects {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 500] /CropBox [10 20 210 320] /Rotate 90 /Contents 4 0 R >>",
        "<< /Length 31 >>\nstream\n1 0 0 rg 10 20 60 30 re f\nendstream"
    };
    // Match stream length exactly so parsers need no repair.
    const QByteArray stream = "1 0 0 rg 10 20 60 30 re f\n";
    objects[3] = "<< /Length " + QByteArray::number(stream.size()) + " >>\nstream\n" + stream + "endstream";
    QByteArray data = "%PDF-1.4\n";
    QList<int> offsets {0};
    for (int i = 0; i < objects.size(); ++i) {
        offsets.append(data.size());
        data += QByteArray::number(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const int xref = data.size();
    data += "xref\n0 5\n0000000000 65535 f \n";
    for (int i = 1; i < offsets.size(); ++i) data += QByteArray::number(offsets[i]).rightJustified(10, '0') + " 00000 n \n";
    return data + "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
}

QByteArray expensivePdfFixture(bool nested)
{
    auto stream = [](QByteArray header, const QByteArray &content) {
        const QByteArray compressed = qCompress(content).mid(4);
        return QByteArray(header + " /Filter /FlateDecode /Length " + QByteArray::number(compressed.size())
            + " >>\nstream\n" + compressed + "\nendstream");
    };
    constexpr int depth = 25;
    QList<QByteArray> objects{"<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Kids [3 0 R] /Count 1 >>"};
    objects.append(nested ? QByteArray("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 32 32] /Resources << /XObject << /F "
        + QByteArray::number(5 + depth) + " 0 R >> >> /Contents 4 0 R >>")
        : QByteArray("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 32 32] /Resources << >> /Contents 4 0 R >>"));
    if (nested) {
        objects.append(stream("<<", "/F Do"));
        objects.append(stream("<< /Type /XObject /Subtype /Form /BBox [0 0 32 32] /Resources << >>", "0 0 10 10 re f"));
        for (int i = 1; i <= depth; ++i)
            objects.append(stream("<< /Type /XObject /Subtype /Form /BBox [0 0 32 32] /Resources << /XObject << /F "
                + QByteArray::number(4 + i) + " 0 R >> >>", "/F Do /F Do"));
    } else {
        QByteArray commands;
        commands.reserve(16 * 1000000);
        for (int i = 0; i < 1000000; ++i) commands += "0 0 10 10 re f\n";
        objects.append(stream("<<", commands));
    }
    QByteArray data = "%PDF-1.4\n"; QList<int> offsets{0};
    for (int i = 0; i < objects.size(); ++i) {
        offsets.append(data.size()); data += QByteArray::number(i + 1) + " 0 obj\n" + objects.at(i) + "\nendobj\n";
    }
    const int xref = data.size();
    data += "xref\n0 " + QByteArray::number(offsets.size()) + "\n0000000000 65535 f \n";
    for (int i = 1; i < offsets.size(); ++i) data += QByteArray::number(offsets.at(i)).rightJustified(10, '0') + " 00000 n \n";
    return data + "trailer\n<< /Root 1 0 R /Size " + QByteArray::number(offsets.size()) + " >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
}
#endif

}

class TestUDriveFileBrowser : public QObject
{
    Q_OBJECT
    QTemporaryDir _config;
    std::unique_ptr<FolderMan> _folders;
    QByteArray _previousLocalHttp;
    bool _localHttpWasSet = false;
private Q_SLOTS:
    void initTestCase()
    {
        if (Theme::instance()->appNameGUI() == QStringLiteral("UDrive"))
            QVERIFY(Resources::loadUDriveFonts());
        _localHttpWasSet = qEnvironmentVariableIsSet("UDRIVE_ALLOW_LOCAL_HTTP");
        _previousLocalHttp = qgetenv("UDRIVE_ALLOW_LOCAL_HTTP");
        qputenv("UDRIVE_ALLOW_LOCAL_HTTP", "1");
        QVERIFY(_config.isValid());
        ConfigFile::setConfDir(_config.path());
        Account::setCommonCacheDirectory(_config.path() + "/cache");
        _folders = FolderMan::createInstance();
    }

    void cleanupTestCase()
    {
        if (_localHttpWasSet) qputenv("UDRIVE_ALLOW_LOCAL_HTTP", _previousLocalHttp);
        else qunsetenv("UDRIVE_ALLOW_LOCAL_HTTP");
    }

    void loggingDialogLayout_data()
    {
        QTest::addColumn<QString>("locale");
        QTest::addColumn<bool>("dark");
        for (const QString &locale : {QStringLiteral("en_US"), QStringLiteral("km"), QStringLiteral("zh_CN")}) {
            QTest::newRow(qPrintable(locale + "-light")) << locale << false;
            QTest::newRow(qPrintable(locale + "-dark")) << locale << true;
        }
    }

    void loggingDialogLayout()
    {
        QFETCH(QString, locale);
        QFETCH(bool, dark);
        const auto previousMode = ConfigFile().themeMode();
        ConfigFile().setThemeMode(dark ? ConfigFile::ThemeMode::Dark : ConfigFile::ThemeMode::Light);
        const auto restore = qScopeGuard([&] { ConfigFile().setThemeMode(previousMode); });
        QTranslator translator;
        QVERIFY(translator.load(Translations::translationsFilePrefix() + locale, Translations::translationsDirectoryPath()));
        QVERIFY(qApp->installTranslator(&translator));
        const auto removeTranslator = qScopeGuard([&] { qApp->removeTranslator(&translator); });
        LogBrowser dialog(nullptr);
        dialog.show();
        QTest::qWait(50);
        auto *path = dialog.findChild<QLineEdit *>("locationField");
        auto *scroll = dialog.findChild<QScrollArea *>("contentScroll");
        auto *close = dialog.findChild<QDialogButtonBox *>("buttonBox")->button(QDialogButtonBox::Close);
        QVERIFY(path && path->isReadOnly());
        const QString actualPath = path->text();
        const QString longPath = QStringLiteral("/private/temporary/") + QString(400, QLatin1Char('a')) + QStringLiteral("/logs");
        path->setText(longPath);
        path->setCursorPosition(0);
        QTest::qWait(10);
        QVERIFY(dialog.width() <= 600);
        QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        path->selectAll();
        QCOMPARE(path->selectedText(), longPath);
        path->deselect();
        path->setText(actualPath);
        path->setCursorPosition(0);
        if (qEnvironmentVariableIsSet("UDRIVE_LOGGING_TEST_CAPTURE"))
            QVERIFY(dialog.grab().save(QStringLiteral("/tmp/udrive-logging-%1-%2.png").arg(locale, dark ? "dark" : "light")));
        dialog.resize(440, 360);
        QTest::qWait(30);
        QVERIFY(close->isVisible());
        QVERIFY(dialog.rect().contains(QRect(close->mapTo(&dialog, QPoint()), close->size())));
        QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
        QTest::qWait(10);
        QVERIFY(scroll->viewport()->rect().contains(QRect(path->mapTo(scroll->viewport(), QPoint()), path->size())));
        QTest::keyClick(&dialog, Qt::Key_Escape);
        QVERIFY(!dialog.isVisible());
    }

    void khmerFontRendering()
    {
        if (Theme::instance()->appNameGUI() != QStringLiteral("UDrive"))
            QSKIP("UDrive bundled font test.");
        const QFont original = qApp->font();
        QVERIFY(Resources::loadUDriveFonts());
        QVERIFY(Resources::loadUDriveFonts());
        QCOMPARE(qApp->font(), original);
        const auto families = QFontDatabase::applicationFallbackFontFamilies(QChar::Script_Khmer);
        QCOMPARE(families.count(QStringLiteral("Google Sans")), 1);
        for (const auto weight : {QFont::Normal, QFont::DemiBold, QFont::Bold}) {
            auto font = original;
            font.setWeight(weight);
            for (const auto &text : {QStringLiteral("ភាសាខ្មែរ"), QStringLiteral("ផ្ទាល់ខ្លួន"), QStringLiteral("ជ្រើសរើសទាំងអស់")}) {
                QTextLayout layout(text, font);
                layout.beginLayout();
                auto line = layout.createLine();
                QVERIFY(line.isValid());
                line.setLineWidth(1000);
                layout.endLayout();
                QVERIFY(line.height() > 0);
                const auto runs = layout.glyphRuns();
                QVERIFY(!runs.isEmpty());
                for (const auto &run : runs) {
                    QCOMPARE(run.rawFont().familyName(), QStringLiteral("Google Sans"));
                    for (const auto glyph : run.glyphIndexes())
                        QVERIFY(glyph != 0);
                }
            }
        }
        QVERIFY(!contents(QStringLiteral(":/client/resources/fonts/GoogleSans-OFL.txt")).isEmpty());
    }

    void networkSettingsLayout_data()
    {
        QTest::addColumn<QString>("locale");
        for (const auto &locale : {QStringLiteral("en_US"), QStringLiteral("km"), QStringLiteral("zh_CN")})
            QTest::newRow(qPrintable(locale)) << locale;
    }

    void networkSettingsLayout()
    {
        if (Theme::instance()->appNameGUI() != QStringLiteral("UDrive"))
            QSKIP("UDrive network settings layout test.");
        QFETCH(QString, locale);
        QTranslator translator;
        QVERIFY(translator.load(Translations::translationsFilePrefix() + locale, Translations::translationsDirectoryPath()));
        QVERIFY(qApp->installTranslator(&translator));
        const auto previous = qApp->palette();
        const auto restore = qScopeGuard([&] { qApp->removeTranslator(&translator); qApp->setPalette(previous); });
        const auto proxyType = ConfigFile().proxyType();
        SettingsDialog window(nullptr);
        window.resize(1150, 900);
        window.setCurrentPage(SettingsDialog::SettingsPage::Settings);
        window.show();
        QWidget *network = nullptr;
        for (auto *widget : window.findChildren<QWidget *>()) {
            if (widget->property("udriveNetwork").toBool()) {
                network = widget;
                break;
            }
        }
        QVERIFY(network);
        auto *manual = network->findChild<QRadioButton *>("manualProxyRadioButton");
        auto *noProxy = network->findChild<QRadioButton *>("noProxyRadioButton");
        auto *form = network->findChild<QWidget *>("manualSettings");
        auto *host = network->findChild<QLineEdit *>("hostLineEdit");
        auto *user = network->findChild<QLineEdit *>("userLineEdit");
        auto *password = network->findChild<QLineEdit *>("passwordLineEdit");
        auto *port = network->findChild<QAbstractSpinBox *>("portSpinBox");
        auto *type = network->findChild<QComboBox *>("typeComboBox");
        QVERIFY(manual && noProxy && form && host && user && password && port && type);
        QVERIFY(!form->isVisible());
        // Programmatic radio selection exercises layout signals without saving
        // proxy settings or writing any credentials to the system keychain.
        manual->setChecked(true);
        QVERIFY(form->isVisible() && form->isEnabled());
        QVERIFY(host->property("proxyHostMissing").toBool());
        QVERIFY(network->findChild<QLabel *>("proxyHostHint")->isVisible());
        host->setText(QStringLiteral("proxy.example.com"));
        QVERIFY(!host->property("proxyHostMissing").toBool());
        QVERIFY(!network->findChild<QLabel *>("proxyHostHint")->isVisible());
        QCOMPARE(password->echoMode(), QLineEdit::Password);
        QCOMPARE(type->count(), 2);
        if (locale == QStringLiteral("km")) {
            QVERIFY(network->findChild<QGroupBox *>("proxyGroupBox")->title().contains(QStringLiteral("Proxy")));
            QVERIFY(manual->text().contains(QStringLiteral("Proxy")));
            QCOMPARE(type->itemText(0), QStringLiteral("HTTP(S) Proxy"));
            QCOMPARE(type->itemText(1), QStringLiteral("SOCKS5 Proxy"));
        }
        if (qEnvironmentVariableIsSet("UDRIVE_UI_TEST_CAPTURE")) {
            for (const auto &theme : {QStringLiteral("dark"), QStringLiteral("light")}) {
                auto palette = previous;
                const bool dark = theme == QStringLiteral("dark");
                palette.setColor(QPalette::Window, QColor(dark ? "#292929" : "#FFFFFF"));
                palette.setColor(QPalette::WindowText, QColor(dark ? "#EDEEF0" : "#2B3035"));
                palette.setColor(QPalette::Base, palette.color(QPalette::Window));
                palette.setColor(QPalette::Button, palette.color(QPalette::Window));
                palette.setColor(QPalette::Text, palette.color(QPalette::WindowText));
                qApp->setPalette(palette);
                QTest::qWait(50);
                QVERIFY(network->grab().save(QStringLiteral("/tmp/udrive-network-%1-%2.png").arg(locale, theme)));
                type->showPopup();
                QTest::qWait(50);
                QVERIFY(type->view()->window()->grab().save(QStringLiteral("/tmp/udrive-proxy-type-%1-%2.png").arg(locale, theme)));
                type->hidePopup();
            }
        }
        noProxy->setChecked(true);
        QVERIFY(!form->isVisible());
        QCOMPARE(ConfigFile().proxyType(), proxyType);
    }

    void activityIssueTabTranslations_data()
    {
        QTest::addColumn<QString>("locale");
        for (const auto &locale : {QStringLiteral("en_US"), QStringLiteral("km"), QStringLiteral("zh_CN")})
            QTest::newRow(qPrintable(locale)) << locale;
    }

    void activityIssueTabTranslations()
    {
        QFETCH(QString, locale);
        QTranslator translator;
        QVERIFY(translator.load(Translations::translationsFilePrefix() + locale, Translations::translationsDirectoryPath()));
        QVERIFY(qApp->installTranslator(&translator));
        const auto restore = qScopeGuard([&] { qApp->removeTranslator(&translator); });
        const QString label = QCoreApplication::translate("OCC::ActivitySettings", "Not Synced");
        SettingsDialog window(nullptr);
        window.setCurrentPage(SettingsDialog::SettingsPage::Activity);
        window.show();
        auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("activityTabWidget"));
        QVERIFY(tabs);
        auto *bar = tabs->tabBar();
        const int issuesTab = 1;
        for (int count : {0, 3, 100, 1000, 0}) {
            QVERIFY(QMetaObject::invokeMethod(tabs->widget(issuesTab), "issueCountUpdated", Q_ARG(int, count)));
            const QString fullLabel = count > 0
                ? QCoreApplication::translate("OCC::ActivitySettings", "Not Synced (%1)").arg(count)
                : label;
            QCOMPARE(bar->tabText(issuesTab), fullLabel);
            if (count == 3 && qEnvironmentVariableIsSet("UDRIVE_UI_TEST_CAPTURE"))
                QVERIFY(bar->grab().save(QStringLiteral("/tmp/udrive-activity-tab-%1.png").arg(locale)));
        }
        // The badge is capped at 99+, so larger counts must not widen the tab.
        QVERIFY(QMetaObject::invokeMethod(tabs->widget(issuesTab), "issueCountUpdated", Q_ARG(int, 100)));
        const int cappedWidth = bar->sizeHint().width();
        QVERIFY(QMetaObject::invokeMethod(tabs->widget(issuesTab), "issueCountUpdated", Q_ARG(int, 1000)));
        QCOMPARE(bar->sizeHint().width(), cappedWidth);
    }

    void imageThumbnails()
    {
        DriveServer server;
        QImage red(80, 50, QImage::Format_RGB32); red.fill(Qt::red);
        QByteArray png;
        QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly); QVERIFY(red.save(&buffer, "PNG"));
        for (const auto &name : {"photo.jpg", "photo.png", "photo.gif", "photo.webp", "missing.jpg", "broken.png", "oversized.jpg"}) {
            const QString path = QStringLiteral("/dav/personal/") + QLatin1String(name);
            server.uploaded.insert(path, "original is never downloaded");
            if (!QString::fromLatin1(name).startsWith("missing")) server.thumbnailResponses.insert(path, png);
        }
        server.thumbnailResponses["/dav/personal/broken.png"] = "invalid image";
        server.thumbnailResponses["/dav/personal/oversized.jpg"] = QByteArray(512 * 1024 + 1, 'X');
        server.thumbnailDelay = 30;
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveFileBrowser browser(&state); browser.resize(1000, 1000); browser.show();
        auto *tree = browser.findChild<QTreeView *>("driveFiles");
        QTRY_COMPARE(tree->model()->rowCount(), 14);
        const auto fallback = select(tree, "missing.jpg").data(Qt::DecorationRole).value<QIcon>().cacheKey();
        QTRY_COMPARE(server.thumbnailQueries.size(), 7);
        auto hasRedThumbnail = [tree](const QString &name) {
            const auto icon = select(tree, name).data(Qt::DecorationRole).value<QIcon>();
            return icon.pixmap(32, 32).toImage().pixelColor(16, 16).red() > 230
                && icon.pixmap(32, 32).toImage().pixelColor(16, 16).green() < 30;
        };
        QTRY_VERIFY(hasRedThumbnail("photo.jpg"));
        QTRY_VERIFY(hasRedThumbnail("photo.png"));
        QTRY_VERIFY(hasRedThumbnail("photo.gif"));
        QTRY_VERIFY(hasRedThumbnail("photo.webp"));
        QCOMPARE(select(tree, "missing.jpg").data(Qt::DecorationRole).value<QIcon>().cacheKey(), fallback);
        QVERIFY(!hasRedThumbnail("broken.png")); QVERIFY(!hasRedThumbnail("oversized.jpg"));
        QVERIFY(server.maximumThumbnails <= 2);
        QVERIFY(server.authenticated);
        const auto query = server.thumbnailQueries.value("/dav/personal/photo.jpg");
        QCOMPARE(query.queryItemValue("x"), QStringLiteral("64"));
        QCOMPARE(query.queryItemValue("y"), QStringLiteral("64"));
        QCOMPARE(query.queryItemValue("c"), QStringLiteral("\"tag\""));
        const int before = server.requests.size();
        browser.showWorkspaces(); QTRY_COMPARE(tree->model()->rowCount(), 1);
        browser.showPersonal(); QTRY_COMPARE(tree->model()->rowCount(), 14);
        QTRY_VERIFY(hasRedThumbnail("photo.jpg"));
        int photoRequests = 0;
        for (int i = before; i < server.requests.size(); ++i) if (server.requests.at(i).second.contains("photo.")) ++photoRequests;
        QCOMPARE(photoRequests, 0); // etag-keyed cache survives navigation, never writes images to disk.
        if (qEnvironmentVariableIsSet("UDRIVE_THUMBNAIL_TEST_CAPTURE")) browser.grab().save("/tmp/udrive-thumbnails-preview.png");
    }

    void thumbnailAccountIsolationAndCancellation()
    {
        DriveServer firstServer; DriveServer secondServer;
        auto makePng = [](QColor color) {
            QImage image(32, 32, QImage::Format_RGB32); image.fill(color);
            QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly); image.save(&buffer, "PNG"); return bytes;
        };
        const QString path = QStringLiteral("/dav/personal/photo.jpg");
        firstServer.thumbnailResponses.insert(path, makePng(Qt::red));
        secondServer.thumbnailResponses.insert(path, makePng(Qt::blue));
        firstServer.thumbnailDelay = 150;
        auto makeAccount = [](const QUrl &url) {
            auto *account = new Account(QUuid::createUuid(), "test", url);
            account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
            account->credentials()->fetchFromKeychain(); return account;
        };
        AccountState first(makeAccount(firstServer.url())); AccountState second(makeAccount(secondServer.url()));
        QObject owner;
        UDriveThumbnails firstLoader(&first, &owner); UDriveThumbnails secondLoader(&second, &owner);
        QStandardItemModel firstModel; firstModel.appendRow(new QStandardItem("photo.jpg"));
        QStandardItemModel secondModel; secondModel.appendRow(new QStandardItem("photo.jpg"));
        QSignalSpy firstReady(&firstLoader, &UDriveThumbnails::ready);
        QSignalSpy secondReady(&secondLoader, &UDriveThumbnails::ready);
        firstLoader.request(firstModel.index(0, 0), firstServer.url("/dav/personal"), "photo.jpg", "tag");
        QTRY_COMPARE(firstServer.thumbnailQueries.size(), 1);
        firstLoader.cancel();
        secondLoader.request(secondModel.index(0, 0), secondServer.url("/dav/personal"), "photo.jpg", "tag");
        QTRY_COMPARE(secondReady.count(), 1);
        QCOMPARE(secondReady.first().at(1).value<QPixmap>().toImage().pixelColor(16, 16), QColor(Qt::blue));
        QTest::qWait(180); QCOMPARE(firstReady.count(), 0);
        firstLoader.request(firstModel.index(0, 0), firstServer.url("/dav/personal"), "photo.jpg", "tag");
        first.signOutByUi();
        QTest::qWait(180); QCOMPARE(firstReady.count(), 0);
        secondLoader.request(secondModel.index(0, 0), secondServer.url("/dav/personal"), "../escape.jpg", "tag");
        secondLoader.request(secondModel.index(0, 0), QUrl("http://example.com/dav"), "photo.jpg", "tag");
        QCOMPARE(secondReady.count(), 1);
    }

    void thumbnailCacheVersionsAndRemovedRows()
    {
        DriveServer server;
        auto makePng = [](QColor color, QSize size = QSize(32, 32)) {
            QImage image(size, QImage::Format_RGB32); image.fill(color);
            QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly); image.save(&buffer, "PNG"); return bytes;
        };
        auto makeAccount = [&server] {
            auto *account = new Account(QUuid::createUuid(), "test", server.url());
            account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
            account->credentials()->fetchFromKeychain(); return account;
        };
        AccountState first(makeAccount()); AccountState second(makeAccount());
        QObject owner;
        UDriveThumbnails firstLoader(&first, &owner); UDriveThumbnails secondLoader(&second, &owner);
        QStandardItemModel model; model.appendRow(new QStandardItem("photo.jpg"));
        QSignalSpy firstReady(&firstLoader, &UDriveThumbnails::ready);
        QSignalSpy secondReady(&secondLoader, &UDriveThumbnails::ready);
        const auto root = server.url("/dav/personal");
        const QString path = QStringLiteral("/dav/personal/photo.jpg");
        server.thumbnailResponses.insert(path, makePng(Qt::red));
        firstLoader.request(model.index(0, 0), root, "photo.jpg", "version1");
        QTRY_COMPARE(firstReady.count(), 1);
        const int firstRequests = server.requests.size();
        firstLoader.request(model.index(0, 0), root, "photo.jpg", "version1");
        QCOMPARE(firstReady.count(), 2); QCOMPARE(server.requests.size(), firstRequests);
        server.thumbnailResponses[path] = makePng(Qt::blue);
        // Same URL and ETag, but another login must fetch its own image.
        secondLoader.request(model.index(0, 0), root, "photo.jpg", "version1");
        QTRY_COMPARE(secondReady.count(), 1);
        QCOMPARE(secondReady.first().at(1).value<QPixmap>().toImage().pixelColor(16, 16), QColor(Qt::blue));
        firstLoader.request(model.index(0, 0), root, "photo.jpg", "version2");
        QTRY_COMPARE(firstReady.count(), 3);
        QCOMPARE(firstReady.last().at(1).value<QPixmap>().toImage().pixelColor(16, 16), QColor(Qt::blue));
        server.thumbnailResponses[path] = makePng(Qt::red, QSize(2049, 10));
        firstLoader.request(model.index(0, 0), root, "photo.jpg", "too-wide");
        QTest::qWait(100); QCOMPARE(firstReady.count(), 3);
        server.thumbnailResponses[path] = makePng(Qt::red);
        server.thumbnailDelay = 100;
        firstLoader.request(model.index(0, 0), root, "photo.jpg", "removed");
        model.removeRows(0, model.rowCount());
        QTest::qWait(180); QCOMPARE(firstReady.count(), 3);
    }

    void thumbnailsLoadVisibleRowsOnly()
    {
        DriveServer server;
        QImage image(32, 32, QImage::Format_RGB32); image.fill(Qt::red);
        QByteArray png; QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly); QVERIFY(image.save(&buffer, "PNG"));
        for (int i = 0; i < 60; ++i) {
            const QString path = QStringLiteral("/dav/personal/photo-%1.png").arg(i, 3, 10, QLatin1Char('0'));
            server.uploaded.insert(path, {}); server.thumbnailResponses.insert(path, png);
        }
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveFileBrowser browser(&state); browser.resize(840, 500); browser.show();
        auto *tree = browser.findChild<QTreeView *>("driveFiles");
        QTRY_COMPARE(tree->model()->rowCount(), 67);
        browser.findChild<QLineEdit *>("driveSearch")->setText("photo-");
        QTRY_COMPARE(tree->model()->rowCount(), 60);
        auto red = [tree](int row) {
            const auto image = tree->model()->index(row, 0).data(Qt::DecorationRole).value<QIcon>().pixmap(32, 32).toImage();
            return image.pixelColor(16, 16).red() > 230 && image.pixelColor(16, 16).green() < 30;
        };
        QTRY_VERIFY(red(0));
        QVERIFY(!server.thumbnailQueries.contains("/dav/personal/photo-059.png"));
        QVERIFY(server.thumbnailQueries.size() < 20);
        tree->verticalScrollBar()->setValue(tree->verticalScrollBar()->maximum());
        QTRY_VERIFY(red(59));
        QTRY_VERIFY(!red(0)); // offscreen row pixmaps must not escape the memory limit.
        int firstRequests = 0;
        for (const auto &request : server.requests) if (request.second == "/dav/personal/photo-000.png") ++firstRequests;
        tree->verticalScrollBar()->setValue(0);
        QTRY_VERIFY(red(0));
        int after = 0;
        for (const auto &request : server.requests) if (request.second == "/dav/personal/photo-000.png") ++after;
        QCOMPARE(after, firstRequests);
    }

    void authenticatedWritesRetryOnlyDefinitiveRejection_data()
    {
        QTest::addColumn<QByteArray>("verb");
        QTest::addColumn<int>("status");
        QTest::addColumn<int>("rejections");
        QTest::addColumn<int>("attempts");
        QTest::addColumn<bool>("success");
        QTest::addColumn<bool>("queued");
        for (const auto &verb : {QByteArray("PUT"), QByteArray("MOVE"), QByteArray("DELETE"), QByteArray("PROPPATCH")})
            for (bool queued : {false, true})
                QTest::newRow(QByteArray(verb + (queued ? "-queued" : "")).constData()) << verb << 401 << 1 << 2 << true << queued;
        for (bool queued : {false, true}) {
            QTest::newRow(queued ? "repeated401-queued" : "repeated401") << QByteArray("PUT") << 401 << 2 << 2 << false << queued;
            QTest::newRow(queued ? "ambiguous500-queued" : "ambiguous500") << QByteArray("PUT") << 500 << 1 << 1 << false << queued;
            QTest::newRow(queued ? "forbidden403-queued" : "forbidden403") << QByteArray("PUT") << 403 << 1 << 1 << false << queued;
        }
    }
    void authenticatedWritesRetryOnlyDefinitiveRejection()
    {
        QFETCH(QByteArray, verb); QFETCH(int, status); QFETCH(int, rejections); QFETCH(int, attempts); QFETCH(bool, success);
        QFETCH(bool, queued);
        DriveServer server; server.rejectVerb = verb; server.rejectStatus = status; server.rejectionsLeft = rejections;
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        auto *credentials = new RefreshingBrowserCredentials("browser-test-token", "test-refresh-token", account);
        account->setCredentials(credentials); credentials->fetchFromKeychain();
        AccountState state(account); UDriveFileBrowser browser(&state); browser.show();
        auto *tree = browser.findChild<QTreeView *>("driveFiles"); QTRY_COMPARE(tree->model()->rowCount(), 7);
        select(tree, "a&b.txt");
        QSignalSpy finished(&browser, &UDriveFileBrowser::operationFinished);
        QTemporaryDir inputs; QFile file(inputs.filePath("retry.txt")); QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray payload(50000, 'R'); file.write(payload); file.close();
        JobQueueGuard queueGuard(account->jobQueue());
        if (queued) queueGuard.block();
        QVERIFY(verb == "PUT" ? browser.uploadFile(file.fileName()) : verb == "MOVE" ? browser.renameSelected("renamed.txt")
            : verb == "DELETE" ? browser.deleteSelected() : browser.setSelectedFavorite(true));
        if (queued) {
            QCOMPARE(std::count_if(server.requests.cbegin(), server.requests.cend(), [&](const auto &request) { return request.first == verb; }), 0);
            queueGuard.unblock();
        }
        QTRY_COMPARE(finished.count(), 1); QCOMPARE(finished.first().at(0).toBool(), success);
        QVERIFY(browser.findChild<QLabel *>("driveUploadDetails")->isHidden());
        QVERIFY(!browser.findChild<QTimer *>("driveUploadProgressTimer")->isActive());
        QCOMPARE(std::count_if(server.requests.cbegin(), server.requests.cend(), [&](const auto &request) { return request.first == verb; }), attempts);
        if (success && verb == "PUT") QCOMPARE(server.uploaded.value("/dav/personal/retry.txt"), payload);
        if (success && verb == "PROPPATCH") QVERIFY(server.favorites.contains("/dav/personal/a&b.txt"));
        if (status == 401 && attempts == 2) {
            const QString path = verb == "PUT" ? QStringLiteral("/dav/personal/retry.txt") : QStringLiteral("/dav/personal/a&b.txt");
            QCOMPARE(server.requestHeaders.value(QString::fromLatin1(verb) + QLatin1Char(' ') + path).value("authorization"), QByteArray("Bearer refreshed-test-token"));
        }
        QCOMPARE(credentials->refreshes, status == 401 ? rejections : 0);
    }

    void thumbnailDecodeSlotsSurviveCancellation_data()
    {
        QTest::addColumn<bool>("recreate");
        QTest::newRow("cancel") << false;
        QTest::newRow("destroy-and-recreate") << true;
    }
    void thumbnailDecodeSlotsSurviveCancellation()
    {
        QFETCH(bool, recreate);
        DriveServer server;
        for (int i = 0; i < 4; ++i) server.thumbnailResponses.insert(QStringLiteral("/dav/personal/%1.jpg").arg(i), "fixture");
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account)); account->credentials()->fetchFromKeychain();
        AccountState state(account);
        QSemaphore gate; std::atomic<int> started{0};
        QStandardItemModel model; for (int i = 0; i < 4; ++i) model.appendRow(new QStandardItem(QString::number(i)));
        auto decoder = [&](QByteArray, int) {
            ++started; gate.acquire(); QImage image(32, 32, QImage::Format_RGB32); image.fill(Qt::red); return image;
        };
        auto thumbnails = std::make_unique<UDriveThumbnails>(&state, nullptr, decoder);
        struct ReleaseWorkers { QSemaphore *gate; ~ReleaseWorkers() { gate->release(8); QThreadPool::globalInstance()->waitForDone(5000); } } release{&gate};
        auto ready = std::make_unique<QSignalSpy>(thumbnails.get(), &UDriveThumbnails::ready);
        auto request = [&](int i) { thumbnails->request(model.index(i, 0), server.url("/dav/personal"), QStringLiteral("%1.jpg").arg(i), "tag"); };
        request(0); request(1); QTRY_COMPARE(started.load(), 2);
        if (recreate) {
            thumbnails.reset();
            thumbnails = std::make_unique<UDriveThumbnails>(&state, nullptr, decoder);
            ready = std::make_unique<QSignalSpy>(thumbnails.get(), &UDriveThumbnails::ready);
        } else thumbnails->cancel();
        request(2); request(3);
        QTest::qWait(100); QCOMPARE(started.load(), 2);
        QVERIFY(!server.thumbnailQueries.contains("/dav/personal/2.jpg"));
        gate.release(2); QTRY_COMPARE(started.load(), 4);
        QCOMPARE(ready->count(), 0); // Cancelled generation cannot emit the old images.
        gate.release(2); QTRY_COMPARE(ready->count(), 2);
    }

    void sortingMenuAndRecentIsolation()
    {
        auto settings = ConfigFile::settingsWithGroup(QStringLiteral("UDriveFileBrowser"));
        const auto previousSort = settings->value(QStringLiteral("sortMode"));
        const auto previousView = settings->value(QStringLiteral("viewMode"));
        settings->setValue(QStringLiteral("sortMode"), QStringLiteral("nameAscending"));
        settings->setValue(QStringLiteral("viewMode"), QStringLiteral("default"));
        struct RestoreSettings {
            QVariant sort, view;
            ~RestoreSettings() {
                auto settings = ConfigFile::settingsWithGroup(QStringLiteral("UDriveFileBrowser"));
                for (const auto &pair : {qMakePair(QStringLiteral("sortMode"), sort), qMakePair(QStringLiteral("viewMode"), view)}) {
                    if (pair.second.isValid()) settings->setValue(pair.first, pair.second);
                    else settings->remove(pair.first);
                }
            }
        } restore{previousSort, previousView};
        DriveServer server;
        const QStringList files{QStringLiteral("Alpha_sort.txt"), QStringLiteral("Beta_sort.txt"), QStringLiteral("Gamma_sort.txt")};
        server.sizes.insert(QStringLiteral("/dav/personal/denied.txt"), -1);
        const QList<qint64> sizes{1000, 40, 200};
        const QStringList dates{QStringLiteral("Mon, 01 Jun 2026 10:00:00 GMT"), QStringLiteral("Wed, 03 Jun 2026 10:00:00 GMT"), QStringLiteral("Tue, 02 Jun 2026 10:00:00 GMT")};
        QTemporaryDir synced; QVERIFY(synced.isValid());
        for (int i = 0; i < files.size(); ++i) {
            const QString path = QStringLiteral("/dav/personal/") + files.at(i);
            server.uploaded.insert(path, "test"); server.sizes.insert(path, sizes.at(i)); server.modifiedDates.insert(path, dates.at(i));
            QFile file(synced.filePath(files.at(i))); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("local file");
        }
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account)); account->credentials()->fetchFromKeychain();
        AccountState state(account);
        struct FolderCleanup {
            FolderMan *folders;
            ~FolderCleanup() { folders->unloadAndDeleteAllFolders(); }
        } cleanup{_folders.get()};
        FolderDefinition definition(server.url("/dav/personal"), "personal", "Personal");
        definition.setLocalPath(synced.path()); definition.setTargetPath(""); definition.setJournalPath(".test-sync.db"); definition.setPaused(true);
        QVERIFY(_folders->addFolder(&state, definition));
        {
            UDriveFileBrowser browser(&state); browser.resize(1100, 700); browser.show();
            auto *tree = browser.findChild<QTreeView *>("driveFiles");
            auto *tiles = browser.findChild<QListView *>("driveTiles");
            auto *search = browser.findChild<QLineEdit *>("driveSearch");
            auto *sort = browser.findChild<QPushButton *>("driveSort");
            QTRY_COMPARE(tree->model()->rowCount(), 10);
            QCOMPARE(sort->menu()->actions().size(), 7);
            auto names = [tree] {
                QStringList result;
                for (int i = 0; i < tree->model()->rowCount(); ++i) result.append(tree->model()->index(i, 0).data().toString());
                return result;
            };
            search->setText("_sort"); QCOMPARE(tree->model()->rowCount(), 3);
            select(tree, files.at(0));
            auto check = [&](const QString &key, const QStringList &expected) {
                auto *action = browser.findChild<QAction *>(QStringLiteral("driveSort_") + key);
                QVERIFY(action); action->trigger(); QVERIFY(action->isChecked()); QCOMPARE(names(), expected);
                QCOMPARE(tree->currentIndex().data().toString(), files.at(0));
            };
            check("nameAscending", files);
            check("nameDescending", {files.at(2), files.at(1), files.at(0)});
            check("newest", {files.at(1), files.at(2), files.at(0)});
            check("oldest", {files.at(0), files.at(2), files.at(1)});
            check("largest", {files.at(0), files.at(2), files.at(1)});
            check("smallest", {files.at(1), files.at(2), files.at(0)});
            check("recent", {files.at(1), files.at(2), files.at(0)});
            FileOpenReceiver receiver; QDesktopServices::setUrlHandler("file", &receiver, "record");
            struct ResetHandler { ~ResetHandler() { QDesktopServices::unsetUrlHandler("file"); } } reset;
            for (const auto &file : {files.at(2), files.at(0)}) {
                select(tree, file); browser.findChild<QPushButton *>("driveOpen")->click();
                QCOMPARE(browser.findChild<QPlainTextEdit *>("previewText")->toPlainText(), QStringLiteral("local file"));
                QVERIFY(receiver.opened.isEmpty());
                browser.findChild<QPushButton *>("previewBack")->click();
            }
            QCOMPARE(names(), (QStringList{files.at(0), files.at(2), files.at(1)}));
            browser.findChild<QAction *>("driveViewTiles")->trigger(); QVERIFY(tiles->isVisible());
            QCOMPARE(tiles->model(), tree->model()); QCOMPARE(names(), (QStringList{files.at(0), files.at(2), files.at(1)}));
            browser.findChild<QAction *>("driveViewDefault")->trigger();
            tree->sortByColumn(1, Qt::DescendingOrder);
            QVERIFY(browser.findChild<QAction *>("driveSort_largest")->isChecked());
            tree->sortByColumn(0, Qt::AscendingOrder);
            QVERIFY(browser.findChild<QAction *>("driveSort_nameAscending")->isChecked());
            browser.findChild<QAction *>("driveSort_recent")->trigger();
            search->clear();
            for (const auto &mode : {"newest", "oldest", "largest", "smallest"}) {
                browser.findChild<QAction *>(QStringLiteral("driveSort_") + QLatin1String(mode))->trigger();
                QCOMPARE(names().last(), QStringLiteral("denied.txt"));
                QVERIFY(names().mid(0, 2).contains(QStringLiteral("Docs")));
                QVERIFY(names().mid(0, 2).contains(QStringLiteral("slowfolder")));
            }
            browser.findChild<QAction *>("driveSort_recent")->trigger();
            QCOMPARE(names().mid(0, 2), (QStringList{"slowfolder", "Docs"}));
            QCOMPARE(names().at(2), files.at(0));
            browser.findChild<QPushButton *>("driveRefresh")->click();
            QTRY_COMPARE(tree->model()->rowCount(), 10); QCOMPARE(names().at(2), files.at(0));
        }
        UDriveFileBrowser reopened(&state); reopened.show();
        auto *tree = reopened.findChild<QTreeView *>("driveFiles");
        QTRY_COMPARE(tree->model()->rowCount(), 10);
        QVERIFY(reopened.findChild<QAction *>("driveSort_recent")->isChecked());
        reopened.findChild<QLineEdit *>("driveSearch")->setText("_sort");
        QCOMPARE(tree->model()->index(0, 0).data().toString(), files.at(0));
        // The same server paths on another account must not inherit opened-file history.
        auto *otherAccount = new Account(QUuid::createUuid(), "other", server.url());
        otherAccount->setCredentials(new Credentials("browser-test-token", "test-refresh-token", otherAccount)); otherAccount->credentials()->fetchFromKeychain();
        AccountState otherState(otherAccount); UDriveFileBrowser other(&otherState); other.show();
        auto *otherTree = other.findChild<QTreeView *>("driveFiles"); QTRY_COMPARE(otherTree->model()->rowCount(), 10);
        other.findChild<QLineEdit *>("driveSearch")->setText("_sort");
        QCOMPARE(otherTree->model()->index(0, 0).data().toString(), files.at(1));
    }

    void selectAllFilteredResultsAndActions_data()
    {
        QTest::addColumn<QString>("viewAction");
        QTest::newRow("table") << QStringLiteral("driveViewDefault");
        QTest::newRow("condensed") << QStringLiteral("driveViewCondensed");
        QTest::newRow("tiles") << QStringLiteral("driveViewTiles");
    }

    void selectAllFilteredResultsAndActions()
    {
        QFETCH(QString, viewAction);
        auto settings = ConfigFile::settingsWithGroup(QStringLiteral("UDriveFileBrowser"));
        const QVariant previousSort = settings->value(QStringLiteral("sortMode"));
        const QVariant previousView = settings->value(QStringLiteral("viewMode"));
        const auto restoreSettings = qScopeGuard([previousSort, previousView] {
            auto settings = ConfigFile::settingsWithGroup(QStringLiteral("UDriveFileBrowser"));
            if (previousSort.isValid()) settings->setValue(QStringLiteral("sortMode"), previousSort);
            else settings->remove(QStringLiteral("sortMode"));
            if (previousView.isValid()) settings->setValue(QStringLiteral("viewMode"), previousView);
            else settings->remove(QStringLiteral("viewMode"));
        });
        DriveServer server(true);
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveFileBrowser browser(&state); browser.resize(1050, 760); browser.show();
        auto *tree = browser.findChild<QTreeView *>("driveFiles");
        auto *tiles = browser.findChild<QListView *>("driveTiles");
        auto *all = browser.findChild<QCheckBox *>("driveSelectAll");
        auto *search = browser.findChild<QLineEdit *>("driveSearch");
        QVERIFY(all);
        QTRY_COMPARE(tree->model()->rowCount(), 7);
        QTRY_VERIFY(all->isEnabled());
        browser.findChild<QAction *>(viewAction)->trigger();
        auto *view = viewAction == QStringLiteral("driveViewTiles") ? static_cast<QAbstractItemView *>(tiles) : static_cast<QAbstractItemView *>(tree);
        QCOMPARE(tiles->selectionModel(), tree->selectionModel());
        QCOMPARE(all->checkState(), Qt::Unchecked);
        all->click();
        QCOMPARE(tree->selectionModel()->selectedRows().size(), 7);
        QCOMPARE(all->checkState(), Qt::Checked);
        QVERIFY(browser.findChild<QLabel *>("driveStatus")->text().contains(QStringLiteral("7")));
        DriveServer otherServer;
        auto *otherAccount = new Account(QUuid::createUuid(), "test", otherServer.url());
        otherAccount->setCredentials(new Credentials("browser-test-token", "test-refresh-token", otherAccount));
        otherAccount->credentials()->fetchFromKeychain();
        AccountState otherState(otherAccount);
        UDriveFileBrowser otherBrowser(&otherState);
        auto *otherAll = otherBrowser.findChild<QCheckBox *>("driveSelectAll");
        QTRY_VERIFY(otherAll->isEnabled());
        QCOMPARE(otherAll->checkState(), Qt::Unchecked);
        QCOMPARE(otherBrowser.findChild<QTreeView *>("driveFiles")->selectionModel()->selectedRows().size(), 0);
        QCOMPARE(all->checkState(), Qt::Checked);
        for (const auto *name : {"driveFavorite", "driveWebAction", "drivePermanentDelete"})
            QVERIFY(!browser.findChild<QAction *>(name)->isEnabled());
        QVERIFY(!browser.deleteSelected());
        QVERIFY(!browser.renameSelected("unexpected.txt"));
        QVERIFY(!browser.copySelected("unexpected.txt"));
        QVERIFY(!browser.moveSelected("Docs"));
        QVERIFY(!browser.restoreSelected());
        QVERIFY(!browser.setSelectedFavorite(true));
        QTemporaryDir destination;
        QVERIFY(!browser.saveSelectedFile(destination.filePath("unexpected.txt")));
        QVERIFY(!browser.previewSelectedPdf());

        const auto first = view->model()->index(0, 0);
        view->scrollTo(first);
        QTRY_VERIFY(view->visualRect(first).isValid());
        const QRect row = view->visualRect(first);
        const QPoint check = view == tree ? QPoint(row.left() + 17, row.center().y()) : row.topLeft() + QPoint(24, 24);
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, check);
        QCOMPARE(tree->selectionModel()->selectedRows().size(), 6);
        QCOMPARE(all->checkState(), Qt::PartiallyChecked);
        all->click();
        QCOMPARE(tree->selectionModel()->selectedRows().size(), 7);
        QCOMPARE(all->checkState(), Qt::Checked);
        all->click();
        QCOMPARE(tree->selectionModel()->selectedRows().size(), 0);
        QCOMPARE(all->checkState(), Qt::Unchecked);

        view->setFocus();
        QTest::keySequence(view, QKeySequence(QKeySequence::SelectAll));
        QCOMPARE(tree->selectionModel()->selectedRows().size(), 7);
        search->setText(QStringLiteral(".txt"));
        QCOMPARE(tree->model()->rowCount(), 5);
        QCOMPARE(tree->selectionModel()->selectedRows().size(), 5);
        QCOMPARE(all->checkState(), Qt::Checked);
        all->click(); all->click();
        QCOMPARE(tree->selectionModel()->selectedRows().size(), 5);
        search->setText(QStringLiteral("a&b"));
        QCOMPARE(tree->model()->rowCount(), 1);
        QCOMPARE(tree->selectionModel()->selectedRows().size(), 1);
        QCOMPARE(all->checkState(), Qt::Checked);
        QVERIFY(browser.findChild<QAction *>("driveFavorite")->isEnabled());
        search->setText(QStringLiteral("no matching file"));
        QCOMPARE(tree->model()->rowCount(), 0);
        QCOMPARE(all->checkState(), Qt::Unchecked);
        QVERIFY(!all->isEnabled());

        search->clear();
        QTRY_VERIFY(all->isEnabled()); all->click();
        browser.findChild<QAction *>("driveSort_largest")->trigger();
        QCOMPARE(tree->selectionModel()->selectedRows().size(), 7);
        QCOMPARE(all->checkState(), Qt::Checked);
        browser.findChild<QAction *>(view == tree ? "driveViewTiles" : "driveViewDefault")->trigger();
        QCOMPARE(tree->selectionModel()->selectedRows().size(), 7);
        QCOMPARE(all->checkState(), Qt::Checked);
        view = view == tree ? static_cast<QAbstractItemView *>(tiles) : static_cast<QAbstractItemView *>(tree);
        const auto clicked = view->model()->index(0, view == tree ? 4 : 0);
        view->scrollTo(clicked);
        QTRY_VERIFY(view->visualRect(clicked).isValid());
        const QRect clickedRect = view->visualRect(clicked);
        const QPoint more = view == tree ? QPoint(clickedRect.right() - 27, clickedRect.center().y())
                                         : QPoint(clickedRect.right() - 26, clickedRect.top() + 181);
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, more);
        auto *menu = browser.findChild<QMenu *>("driveActionsMenu");
        QVERIFY(menu && menu->isVisible());
        QCOMPARE(tree->selectionModel()->selectedRows().size(), 1);
        QCOMPARE(all->checkState(), Qt::PartiallyChecked);
        menu->close();

        all->click();
        browser.findChild<QPushButton *>("driveRefresh")->click();
        QTRY_COMPARE(tree->model()->rowCount(), 7);
        QTRY_VERIFY(all->isEnabled());
        QCOMPARE(tree->selectionModel()->selectedRows().size(), 0);
        QCOMPARE(all->checkState(), Qt::Unchecked);
        all->click();
        browser.showWorkspaces();
        QTRY_COMPARE(tree->model()->rowCount(), 1);
        QTRY_VERIFY(all->isEnabled());
        QCOMPARE(tree->selectionModel()->selectedRows().size(), 0);
        QCOMPARE(all->checkState(), Qt::Unchecked);
        all->click();
        state.signOutByUi();
        QTRY_VERIFY(!all->isEnabled());
        QCOMPARE(tree->selectionModel()->selectedRows().size(), 0);
        QCOMPARE(all->checkState(), Qt::Unchecked);
        for (const auto &request : server.requests)
            QVERIFY(request.first != "DELETE" && request.first != "MOVE" && request.first != "COPY" && request.first != "PROPPATCH");
    }

    void checkboxMouseGestures_data()
    {
        QTest::addColumn<QString>("viewAction");
        QTest::newRow("table") << QStringLiteral("driveViewDefault");
        QTest::newRow("condensed") << QStringLiteral("driveViewCondensed");
        QTest::newRow("tiles") << QStringLiteral("driveViewTiles");
    }

    void checkboxMouseGestures()
    {
        QFETCH(QString, viewAction);
        auto settings = ConfigFile::settingsWithGroup(QStringLiteral("UDriveFileBrowser"));
        const QVariant previousView = settings->value(QStringLiteral("viewMode"));
        const auto restoreSettings = qScopeGuard([previousView] {
            auto settings = ConfigFile::settingsWithGroup(QStringLiteral("UDriveFileBrowser"));
            if (previousView.isValid()) settings->setValue(QStringLiteral("viewMode"), previousView);
            else settings->remove(QStringLiteral("viewMode"));
        });
        DriveServer server;
        auto *account = new Account(QUuid::createUuid(), QStringLiteral("test"), server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveFileBrowser browser(&state);
        browser.resize(1150, 850);
        browser.show();
        auto *tree = browser.findChild<QTreeView *>(QStringLiteral("driveFiles"));
        QTRY_COMPARE(tree->model()->rowCount(), 7);
        browser.findChild<QAction *>(viewAction)->trigger();
        auto *view = viewAction == QStringLiteral("driveViewTiles")
            ? static_cast<QAbstractItemView *>(browser.findChild<QListView *>(QStringLiteral("driveTiles")))
            : static_cast<QAbstractItemView *>(tree);
        auto *all = browser.findChild<QCheckBox *>(QStringLiteral("driveSelectAll"));
        QTRY_VERIFY(all->isEnabled());
        auto *paintStyle = new SelectionPaintStyle;
        paintStyle->setParent(view);
        view->setStyle(paintStyle);
        view->viewport()->setStyle(paintStyle);
        const auto indexAt = [view](int row) { return view->model()->index(row, 0); };
        const auto checkAt = [view, tree, &indexAt](int row) {
            const auto rect = view->visualRect(indexAt(row));
            return view == tree ? QPoint(rect.left() + 17, rect.center().y()) : rect.topLeft() + QPoint(24, 24);
        };
        const auto bodyAt = [view, &indexAt](int row) { return view->visualRect(indexAt(row)).center(); };
        const auto move = [view](const QPoint &position, Qt::MouseButtons buttons) {
            QMouseEvent event(QEvent::MouseMove, position, view->viewport()->mapToGlobal(position), Qt::NoButton, buttons, Qt::NoModifier);
            QCoreApplication::sendEvent(view->viewport(), &event);
        };
        const auto overlayCount = [view, paintStyle] {
            paintStyle->rubberBands = 0;
            const auto image = view->viewport()->grab();
            Q_UNUSED(image)
            return paintStyle->rubberBands;
        };
        QTRY_VERIFY(view->visualRect(indexAt(6)).isValid());
        QVERIFY(view->viewport()->rect().contains(bodyAt(6)));
        // A normal click replaces selection; checkboxes add/remove individual items.
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, bodyAt(0));
        QCOMPARE(view->selectionModel()->selectedRows().size(), 1);
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, bodyAt(2));
        QCOMPARE(view->selectionModel()->selectedRows(), QModelIndexList{indexAt(2)});
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, bodyAt(0));
        QTest::mousePress(view->viewport(), Qt::LeftButton, Qt::NoModifier, checkAt(1));
        QCOMPARE(view->selectionModel()->selectedRows().size(), 2);
        const auto checked = view->selectionModel()->selection();
        // Moving while pressing a checkbox must not start a second, native drag gesture.
        move(bodyAt(6), Qt::LeftButton);
        QCOMPARE(view->selectionModel()->selection(), checked);
        QCOMPARE(overlayCount(), 0);
        // Release outside the original checkbox, over a different checkbox.
        QTest::mouseRelease(view->viewport(), Qt::LeftButton, Qt::NoModifier, checkAt(6));
        move(bodyAt(4), Qt::NoButton);
        QCOMPARE(view->selectionModel()->selection(), checked);
        QCOMPARE(overlayCount(), 0);
        QCOMPARE(all->checkState(), Qt::PartiallyChecked);
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, checkAt(1));
        QCOMPARE(view->selectionModel()->selectedRows(), QModelIndexList{indexAt(0)});

        // A genuine drag starting outside a checkbox must receive its release,
        // even when the pointer ends on a checkbox.
        const QPoint empty(view->viewport()->width() - 10, view->viewport()->height() - 10);
        QVERIFY(!view->indexAt(empty).isValid());
        QTest::mousePress(view->viewport(), Qt::LeftButton, Qt::NoModifier, empty);
        move(checkAt(0), Qt::LeftButton);
        if (view != tree) QVERIFY(overlayCount() > 0);
        QTest::mouseRelease(view->viewport(), Qt::LeftButton, Qt::NoModifier, checkAt(0));
        QCOMPARE(overlayCount(), 0);
        const auto dragged = view->selectionModel()->selection();
        move(bodyAt(5), Qt::NoButton);
        QCOMPARE(view->selectionModel()->selection(), dragged);
        QCOMPARE(overlayCount(), 0);
        // A later ordinary click must recover single selection and file actions.
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, bodyAt(3));
        QCOMPARE(view->selectionModel()->selectedRows(), QModelIndexList{indexAt(3)});
        QVERIFY(browser.findChild<QPushButton *>(QStringLiteral("driveShare"))->isEnabled());
    }

    void gridViewModesAndActions()
    {
        auto settings = ConfigFile::settingsWithGroup(QStringLiteral("UDriveFileBrowser"));
        const QVariant previous = settings->value(QStringLiteral("viewMode"));
        settings->setValue(QStringLiteral("viewMode"), QStringLiteral("default"));
        struct RestoreView {
            QVariant previous;
            ~RestoreView() {
                auto settings = ConfigFile::settingsWithGroup(QStringLiteral("UDriveFileBrowser"));
                if (previous.isValid()) settings->setValue(QStringLiteral("viewMode"), previous);
                else settings->remove(QStringLiteral("viewMode"));
            }
        } restore{previous};
        DriveServer server;
        QImage image(256, 160, QImage::Format_RGB32); image.fill(Qt::red);
        QByteArray png; QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly); QVERIFY(image.save(&buffer, "PNG"));
        server.uploaded.insert("/dav/personal/photo.jpg", "original");
        server.thumbnailResponses.insert("/dav/personal/photo.jpg", png);
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        {
            UDriveFileBrowser browser(&state); browser.resize(1050, 760); browser.show();
            auto *tree = browser.findChild<QTreeView *>("driveFiles");
            auto *tiles = browser.findChild<QListView *>("driveTiles");
            QTRY_COMPARE(tree->model()->rowCount(), 8);
            const auto selected = select(tree, "photo.jpg"); QVERIFY(selected.isValid());
            auto *tileAction = browser.findChild<QAction *>("driveViewTiles");
            auto *defaultAction = browser.findChild<QAction *>("driveViewDefault");
            auto *condensedAction = browser.findChild<QAction *>("driveViewCondensed");
            QVERIFY(tileAction && defaultAction && condensedAction);
            tileAction->trigger(); QTRY_VERIFY(tiles->isVisible()); QVERIFY(!tree->isVisible());
            QCOMPARE(tiles->selectionModel(), tree->selectionModel());
            QCOMPARE(tiles->currentIndex().data().toString(), QStringLiteral("photo.jpg"));
            QVERIFY(tileAction->isChecked()); QVERIFY(!defaultAction->isChecked());
            QTRY_COMPARE(server.thumbnailQueries.value("/dav/personal/photo.jpg").queryItemValue("x"), QStringLiteral("256"));
            QTRY_VERIFY(selected.data(Qt::DecorationRole).value<QIcon>().pixmap(128, 128).toImage().pixelColor(64, 64).red() > 230
                && selected.data(Qt::DecorationRole).value<QIcon>().pixmap(128, 128).toImage().pixelColor(64, 64).green() < 30);
            QVERIFY(tiles->visualRect(tiles->model()->index(0, 0)).width() > 100);
            if (qEnvironmentVariableIsSet("UDRIVE_GRID_TEST_CAPTURE")) {
                browser.grab().save("/tmp/udrive-grid-preview.png");
                // Capture the styled workspace using the same isolated dummy account.
                auto *manager = AccountManager::instance();
                auto *captureAccount = new Account(QUuid::createUuid(), "test", server.url());
                captureAccount->setCredentials(new Credentials("browser-test-token", "test-refresh-token", captureAccount));
                captureAccount->credentials()->fetchFromKeychain();
                auto *captureState = manager->addAccount(captureAccount);
                {
                    const QPalette originalPalette = qApp->palette();
                    QPalette dark = originalPalette;
                    dark.setColor(QPalette::Window, QColor("#292929")); dark.setColor(QPalette::WindowText, QColor("#EDEEF0"));
                    dark.setColor(QPalette::Base, QColor("#292929")); dark.setColor(QPalette::Text, QColor("#EDEEF0"));
                    dark.setColor(QPalette::Button, QColor("#323232")); dark.setColor(QPalette::ButtonText, QColor("#EDEEF0"));
                    qApp->setPalette(dark);
                    QTranslator translator;
                    translator.load(Translations::translationsFilePrefix() + QStringLiteral("km"), Translations::translationsDirectoryPath());
                    qApp->installTranslator(&translator);
                    SettingsDialog window(nullptr); window.resize(1250, 850); window.show();
                    QTest::qWait(250); window.grab().save("/tmp/udrive-grid-workspace-dark-khmer.png");
                    if (auto *sort = window.findChild<QPushButton *>("driveSort")) {
                        sort->menu()->popup(sort->mapToGlobal(QPoint(0, sort->height())));
                        QTest::qWait(50); sort->menu()->grab().save("/tmp/udrive-sort-menu-khmer.png"); sort->menu()->close();
                    }
                    if (auto *all = window.findChild<QCheckBox *>("driveSelectAll")) {
                        all->click(); QTest::qWait(50);
                        window.grab().save("/tmp/udrive-select-all-checked-khmer.png");
                        auto *files = window.findChild<QTreeView *>("driveFiles");
                        files->selectionModel()->select(files->model()->index(0, 0), QItemSelectionModel::Deselect | QItemSelectionModel::Rows);
                        QTest::qWait(50);
                        window.grab().save("/tmp/udrive-select-all-partial-khmer.png");
                    }
                    qApp->removeTranslator(&translator); qApp->setPalette(originalPalette);
                }
                manager->deleteAccount(captureState);
            }
            browser.findChild<QLineEdit *>("driveSearch")->setText("photo");
            QCOMPARE(tiles->model()->rowCount(), 1);
            QTRY_VERIFY(tiles->visualRect(tiles->model()->index(0, 0)).isValid());
            const QRect rect = tiles->visualRect(tiles->model()->index(0, 0));
            QTest::mouseClick(tiles->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(rect.right() - 26, rect.top() + 181));
            auto *menu = browser.findChild<QMenu *>("driveActionsMenu");
            QTRY_VERIFY(menu->isVisible()); menu->close();
            QVERIFY(browser.setSelectedFavorite(true));
            QTRY_VERIFY(server.favorites.contains("/dav/personal/photo.jpg"));
            QTRY_COMPARE(tiles->model()->rowCount(), 8);
            const auto docs = select(tree, "Docs"); QVERIFY(docs.isValid());
            Q_EMIT tiles->activated(docs);
            QTRY_COMPARE(tiles->model()->rowCount(), 1);
            QCOMPARE(tiles->model()->index(0, 0).data().toString(), QStringLiteral("Budget 2026.pdf"));
            browser.findChild<QPushButton *>("driveRoot")->click();
            QTRY_COMPARE(tiles->model()->rowCount(), 8);
            select(tree, "photo.jpg");
            condensedAction->trigger(); QTRY_VERIFY(tree->isVisible()); QVERIFY(!tiles->isVisible());
            QCOMPARE(tree->currentIndex().data().toString(), QStringLiteral("photo.jpg"));
            QTRY_VERIFY(tree->visualRect(tree->currentIndex()).height() < 42);
            defaultAction->trigger(); QTRY_VERIFY(tree->visualRect(tree->currentIndex()).height() >= 42);
            tileAction->trigger();
        }
        UDriveFileBrowser reopened(&state); reopened.resize(840, 640); reopened.show();
        QTRY_VERIFY(reopened.findChild<QListView *>("driveTiles")->isVisible());
        QVERIFY(reopened.findChild<QAction *>("driveViewTiles")->isChecked());
    }

    void languageSettingsRestoreAndRendering_data()
    {
        QTest::addColumn<QString>("savedLocale");
        QTest::addColumn<QString>("expectedLocale");
        QTest::newRow("english") << QStringLiteral("en_GB") << QStringLiteral("en_US");
        QTest::newRow("khmer") << QStringLiteral("km_KH") << QStringLiteral("km");
        QTest::newRow("chinese") << QStringLiteral("zh-Hans") << QStringLiteral("zh_CN");
        QTest::newRow("unsupported-old-language") << QStringLiteral("de") << QStringLiteral("en_US");
    }
    void languageSettingsRestoreAndRendering()
    {
        if (Theme::instance()->appNameGUI() != QStringLiteral("UDrive")) QSKIP("UDrive language selector test.");
        QFETCH(QString, savedLocale);
        QFETCH(QString, expectedLocale);
        const QString previous = ConfigFile().uiLanguage();
        ConfigFile().setUiLanguage(savedLocale);
        QTranslator translator;
        QVERIFY(translator.load(Translations::translationsFilePrefix() + expectedLocale, Translations::translationsDirectoryPath()));
        QVERIFY(qApp->installTranslator(&translator));
        struct Cleanup
        {
            QTranslator *translator;
            QString previous;
            ~Cleanup()
            {
                qApp->removeTranslator(translator);
                ConfigFile().setUiLanguage(previous);
            }
        } cleanup{&translator, previous};
        SettingsDialog window(nullptr);
        window.resize(1100, 800);
        window.setCurrentPage(SettingsDialog::SettingsPage::Settings);
        window.show();
        auto *languages = window.findChild<QComboBox *>("languageDropdown");
        QVERIFY(languages);
        QCOMPARE(languages->count(), 3);
        QCOMPARE(languages->currentData().toString(), expectedLocale);
        languages->showPopup();
        auto *languagePopup = languages->view()->window();
        const auto below = languages->mapToGlobal(QPoint(0, languages->height() + 4));
        QTRY_COMPARE(languagePopup->pos(), below);
        QCOMPARE(languagePopup->width(), languages->width());
        QVERIFY(languages->screen()->availableGeometry().contains(languagePopup->geometry()));
        languages->hidePopup();
        if (qEnvironmentVariableIsSet("UDRIVE_UI_TEST_CAPTURE")) {
            QTest::qWait(100);
            QVERIFY(window.grab().save(QStringLiteral("/tmp/udrive-language-%1.png").arg(expectedLocale)));
            const QPalette previous = qApp->palette();
            for (const auto &theme : {QStringLiteral("dark"), QStringLiteral("light")}) {
                auto palette = previous;
                const bool dark = theme == QStringLiteral("dark");
                palette.setColor(QPalette::Window, QColor(dark ? "#292929" : "#FFFFFF"));
                palette.setColor(QPalette::WindowText, QColor(dark ? "#EDEEF0" : "#2B3035"));
                palette.setColor(QPalette::Text, palette.color(QPalette::WindowText));
                palette.setColor(QPalette::Base, QColor(dark ? "#303330" : "#FAFCFB"));
                palette.setColor(QPalette::Button, palette.color(QPalette::Base));
                qApp->setPalette(palette);
                QTest::qWait(50);
                languages->showPopup();
                QTest::qWait(50);
                QVERIFY(languages->view()->window()->grab().save(QStringLiteral("/tmp/udrive-language-popup-%1-%2.png").arg(expectedLocale, theme)));
                languages->hidePopup();
            }
            qApp->setPalette(previous);
        }
        // Selection is saved canonically and restored when settings reopen.
        languages->setCurrentIndex(1);
        const QPalette previousPalette = qApp->palette();
        const auto restorePalette = qScopeGuard([previousPalette] { qApp->setPalette(previousPalette); });
        if (qEnvironmentVariableIsSet("UDRIVE_UI_TEST_CAPTURE")) {
            auto dark = previousPalette;
            dark.setColor(QPalette::Window, QColor("#292929"));
            dark.setColor(QPalette::WindowText, QColor("#EDEEF0"));
            qApp->setPalette(dark);
        }
        QVERIFY(QMetaObject::invokeMethod(languages, "activated", Qt::DirectConnection, Q_ARG(int, 1)));
        QCOMPARE(ConfigFile().uiLanguage(), QStringLiteral("km"));
        QPointer<QDialog> notice = window.findChild<QDialog *>("languageRestartNotice");
        QVERIFY(notice && notice->isVisible() && notice->isModal());
        auto *title = notice->findChild<QLabel *>("languageRestartTitle");
        auto *message = notice->findChild<QLabel *>("languageRestartMessage");
        auto *okay = notice->findChild<QPushButton *>("languageRestartOkay");
        QVERIFY(title && message && okay && okay->isDefault());
        QCOMPARE(title->textFormat(), Qt::PlainText);
        QCOMPARE(message->textFormat(), Qt::PlainText);
        QCOMPARE(title->text(), expectedLocale == QStringLiteral("en_US") ? QStringLiteral("Language saved")
            : translator.translate("OCC::GeneralSettings", "Language saved"));
        QCOMPARE(message->text(), expectedLocale == QStringLiteral("en_US") ? QStringLiteral("Restart UDrive to apply your new language.")
            : translator.translate("OCC::GeneralSettings", "Restart UDrive to apply your new language."));
        QVERIFY(QMetaObject::invokeMethod(languages, "activated", Qt::DirectConnection, Q_ARG(int, 1)));
        QCOMPARE(window.findChildren<QDialog *>("languageRestartNotice").size(), 1);
        if (qEnvironmentVariableIsSet("UDRIVE_UI_TEST_CAPTURE")) {
            QTest::qWait(50);
            QVERIFY(notice->grab().save(QStringLiteral("/tmp/udrive-language-restart-%1.png").arg(expectedLocale)));
        }
        QSignalSpy accepted(notice, &QDialog::accepted);
        okay->click();
        QCOMPARE(accepted.count(), 1);
        QTRY_VERIFY(notice.isNull());
        QVERIFY(QMetaObject::invokeMethod(languages, "activated", Qt::DirectConnection, Q_ARG(int, 1)));
        notice = window.findChild<QDialog *>("languageRestartNotice");
        QVERIFY(notice && notice->isVisible());
        QTest::keyClick(notice, Qt::Key_Escape);
        QTRY_VERIFY(notice.isNull());
        auto reopened = std::make_unique<SettingsDialog>(nullptr);
        auto *reopenedLanguages = reopened->findChild<QComboBox *>("languageDropdown");
        QCOMPARE(reopenedLanguages->currentData().toString(), QStringLiteral("km"));
        QVERIFY(QMetaObject::invokeMethod(reopenedLanguages, "activated", Qt::DirectConnection, Q_ARG(int, 1)));
        notice = reopened->findChild<QDialog *>("languageRestartNotice");
        QVERIFY(notice);
        reopened.reset();
        QVERIFY(notice.isNull());
    }

    void rowActionsTargetTheClickedFile_data()
    {
        QTest::addColumn<QString>("viewAction");
        QTest::newRow("table") << QStringLiteral("driveViewDefault");
        QTest::newRow("condensed") << QStringLiteral("driveViewCondensed");
    }

    void rowActionsTargetTheClickedFile()
    {
        QFETCH(QString, viewAction);
        auto settings = ConfigFile::settingsWithGroup(QStringLiteral("UDriveFileBrowser"));
        const QVariant previousView = settings->value(QStringLiteral("viewMode"));
        const auto restoreSettings = qScopeGuard([previousView] {
            auto settings = ConfigFile::settingsWithGroup(QStringLiteral("UDriveFileBrowser"));
            if (previousView.isValid()) settings->setValue(QStringLiteral("viewMode"), previousView);
            else settings->remove(QStringLiteral("viewMode"));
        });
        DriveServer server;
        server.favorites.insert("/dav/personal/a&b.txt");
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveFileBrowser browser(&state);
        browser.resize(1000, 640);
        browser.show();
        auto *tree = browser.findChild<QTreeView *>("driveFiles");
        QTRY_COMPARE(tree->model()->rowCount(), 7);
        browser.findChild<QAction *>(viewAction)->trigger();
        QCOMPARE(tree->model()->columnCount(), 5);
        select(tree, "denied.txt");
        QModelIndex target;
        for (int row = 0; row < tree->model()->rowCount(); ++row)
            if (tree->model()->index(row, 0).data().toString() == QStringLiteral("a&b.txt"))
                target = tree->model()->index(row, 4);
        QVERIFY(target.isValid());
        QRect row = tree->visualRect(target);
        if (qEnvironmentVariableIsSet("UDRIVE_ROW_TEST_CAPTURE"))
            browser.grab().save(QStringLiteral("/tmp/udrive-row-actions-%1.png").arg(viewAction));
        qApp->clipboard()->setText("before");
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(row.right() - 61, row.center().y()));
        QCOMPARE(qApp->clipboard()->text(), server.url("/f/dav/personal/a&b.txt").toString());
        QCOMPARE(tree->currentIndex().siblingAtColumn(0).data().toString(), QStringLiteral("a&b.txt"));
        auto *actions = browser.findChild<QMenu *>("driveActionsMenu");
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(row.right() - 27, row.center().y()));
        QVERIFY(actions && actions->isVisible());
        auto *webAction = browser.findChild<QAction *>("driveWebAction");
        QVERIFY(webAction != nullptr);
        QVERIFY(webAction->isEnabled());
        QVERIFY(!browser.findChild<QPushButton *>("driveOnline"));
        actions->hide();
        select(tree, "denied.txt");
        // Direct Share must target the clicked row, without sending an invitation.
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(row.right() - 95, row.center().y()));
        auto *sharePanel = browser.findChild<UDriveShareDialog *>();
        QVERIFY(sharePanel && sharePanel->isVisible());
        QVERIFY(!sharePanel->isWindow() && !sharePanel->isModal());
        QVERIFY(sharePanel->windowTitle().contains(QStringLiteral("a&b.txt")));
        browser.findChild<QToolButton *>("driveDetailsBack")->click();
        QCOMPARE(server.sharePosts, 0);
        QCOMPARE(tree->selectionModel()->selectedRows().size(), 1);
        QCOMPARE(tree->currentIndex().siblingAtColumn(0).data().toString(), QStringLiteral("a&b.txt"));
        QSignalSpy completed(&browser, &UDriveFileBrowser::operationFinished);
        QTest::qWait(50); // Opening the sidebar resizes the file list.
        row = tree->visualRect(target);
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(row.right() - 129, row.center().y()));
        QTRY_COMPARE(completed.count(), 1);
        QVERIFY(completed.first().first().toBool());
        QVERIFY(!server.favorites.contains("/dav/personal/a&b.txt"));
        server.readOnly = true;
        browser.findChild<QPushButton *>("driveRefresh")->click();
        QTRY_COMPARE(tree->model()->rowCount(), 7);
        select(tree, "a&b.txt");
        auto *share = browser.findChild<QAction *>("driveShareAction");
        QVERIFY(!share->isEnabled());
        QSignalSpy sharing(share, &QAction::triggered);
        row = tree->visualRect(tree->currentIndex().siblingAtColumn(4));
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(row.right() - 95, row.center().y()));
        QCOMPARE(sharing.count(), 0);
        QCOMPARE(server.sharePosts, 0);
        QVERIFY(server.authenticated);
    }


    void workspaceHeaderAccountIsolation()
    {
        if (Theme::instance()->appNameGUI() != QStringLiteral("UDrive"))
            QSKIP("UDrive workspace header test.");
        DriveServer firstServer;
        DriveServer secondServer;
        firstServer.personalQuota = {{"used", qint64(29 * 1024 * 1024)}, {"total", qint64(15) * 1024 * 1024 * 1024}};
        secondServer.personalQuota = {{"used", qint64(512)}, {"total", qint64(2048)}};
        auto *first = new Account(QUuid::createUuid(), "first", firstServer.url());
        auto *second = new Account(QUuid::createUuid(), "second", secondServer.url());
        first->setCredentials(new Credentials("browser-test-token", "test-refresh-token", first));
        second->setCredentials(new Credentials("browser-test-token", "test-refresh-token", second));
        first->credentials()->fetchFromKeychain();
        second->credentials()->fetchFromKeychain();
        auto *manager = AccountManager::instance();
        auto *firstState = manager->addAccount(first);
        auto *secondState = manager->addAccount(second);
        {
            SettingsDialog window(nullptr);
            window.resize(1280, 800);
            window.show();
            auto *headerSearch = window.findChild<QStackedWidget *>("driveHeaderSearch");
            QVERIFY(headerSearch);
            auto *home = window.findChild<QPushButton *>("driveHome");
            QVERIFY(home);
            auto *languages = window.findChild<QComboBox *>("languageDropdown");
            QVERIFY(languages);
            QCOMPARE(languages->count(), 3);
            QCOMPARE(languages->itemText(0), QStringLiteral("English"));
            QCOMPARE(languages->itemText(1), QStringLiteral("ភាសាខ្មែរ - Khmer"));
            QCOMPARE(languages->itemText(2), QStringLiteral("中文 - Chinese"));
            QCOMPARE(languages->itemData(0).toString(), QStringLiteral("en_US"));
            QCOMPARE(languages->itemData(1).toString(), QStringLiteral("km"));
            QCOMPARE(languages->itemData(2).toString(), QStringLiteral("zh_CN"));
            auto *quick = window.findChild<QQuickWidget *>();
            QVERIFY(quick);
            QVERIFY(quick->errors().isEmpty());
            window.setCurrentAccount(first);
            window.setCurrentPage(SettingsDialog::SettingsPage::Files);
            auto *firstSearch = qobject_cast<QLineEdit *>(headerSearch->currentWidget());
            QVERIFY(firstSearch);
            QCOMPARE(firstSearch->property("accountId").toUuid(), first->uuid());
            UDriveFileBrowser *firstBrowser = nullptr;
            // The test compiles the browser separately; locate the library-owned
            // widget by its public object name rather than comparing moc identities.
            for (auto *widget : window.findChildren<QWidget *>()) {
                if (widget->objectName() != QStringLiteral("udriveFileBrowser"))
                    continue;
                auto *browser = static_cast<UDriveFileBrowser *>(widget);
                if (browser->accountState()->account() == first)
                    firstBrowser = browser;
            }
            QVERIFY(firstBrowser);
            auto *tree = firstBrowser->findChild<QTreeView *>("driveFiles");
            QTRY_COMPARE(tree->model()->rowCount(), 7);
            QTRY_VERIFY(qAbs(window.storageFraction() - double(29 * 1024 * 1024) / double(qint64(15) * 1024 * 1024 * 1024)) < 0.000001);
            firstSearch->setText("Docs");
            QCOMPARE(tree->model()->rowCount(), 1);
            auto *clearSearch = firstSearch->findChild<QToolButton *>("driveClearSearch");
            auto *searchIcon = firstSearch->findChild<QToolButton *>("driveSearchIcon");
            QVERIFY(clearSearch && searchIcon);
            QVERIFY(clearSearch->isVisible());
            QTest::mouseClick(clearSearch, Qt::LeftButton);
            QVERIFY(firstSearch->text().isEmpty());
            QVERIFY(!clearSearch->isVisible());
            QCOMPARE(tree->model()->rowCount(), 7);
            QVERIFY(firstSearch->hasFocus());
            home->setFocus();
            QTest::mouseClick(searchIcon, Qt::LeftButton);
            QVERIFY(firstSearch->hasFocus());
            firstSearch->setText("Docs");
            window.setCurrentAccount(second);
            window.setCurrentPage(SettingsDialog::SettingsPage::Settings);
            QTest::mouseClick(home, Qt::LeftButton);
            QCOMPARE(window.currentPage(), SettingsDialog::SettingsPage::Files);
            QCOMPARE(window.currentAccount(), second);
            auto *secondSearch = qobject_cast<QLineEdit *>(headerSearch->currentWidget());
            QVERIFY(secondSearch && secondSearch != firstSearch);
            QCOMPARE(secondSearch->property("accountId").toUuid(), second->uuid());
            QCOMPARE(secondSearch->text(), QString());
            QTRY_COMPARE(window.storageFraction(), 0.25);
            window.setCurrentAccount(first);
            window.setCurrentPage(SettingsDialog::SettingsPage::Files);
            QCOMPARE(headerSearch->currentWidget(), firstSearch);
            QCOMPARE(firstSearch->text(), QStringLiteral("Docs"));
            firstSearch->clear();
            const auto docs = select(tree, "Docs");
            QVERIFY(docs.isValid());
            Q_EMIT tree->activated(docs);
            QTRY_COMPARE(tree->model()->rowCount(), 1);
            QCOMPARE(tree->model()->index(0, 0).data().toString(), QStringLiteral("Budget 2026.pdf"));
            firstSearch->setText(QStringLiteral("missing"));
            QTest::mouseClick(home, Qt::LeftButton);
            QCOMPARE(window.currentAccount(), first);
            QVERIFY(firstSearch->text().isEmpty());
            QTRY_COMPARE(tree->model()->rowCount(), 7);
            window.setCurrentPage(SettingsDialog::SettingsPage::Favorites);
            QTRY_COMPARE(tree->model()->rowCount(), 0);
            home->setFocus();
            QTest::keyClick(home, Qt::Key_Space);
            QCOMPARE(window.currentPage(), SettingsDialog::SettingsPage::Files);
            QCOMPARE(window.currentAccount(), first);
            QTRY_COMPARE(tree->model()->rowCount(), 7);
            QTRY_VERIFY(window.canCreateFiles());
            auto *profile = window.findChild<QPushButton *>("driveProfile");
            QVERIFY(profile);
            auto visiblePopup = [&window]() -> QWidget * {
                for (auto *widget : window.findChildren<QWidget *>())
                    if (widget->objectName() == QStringLiteral("driveAccountPopup") && widget->isVisible())
                        return widget;
                return nullptr;
            };
            profile->click();
            auto *popup = visiblePopup();
            QVERIFY(popup);
            QAbstractButton *firstCard = nullptr;
            QAbstractButton *secondCard = nullptr;
            for (auto *button : popup->findChildren<QAbstractButton *>()) {
                if (button->property("accountId").toUuid() == first->uuid())
                    firstCard = button;
                if (button->property("accountId").toUuid() == second->uuid())
                    secondCard = button;
            }
            QVERIFY(firstCard && secondCard);
            QVERIFY(firstCard->isChecked() && !secondCard->isChecked());
            QTRY_VERIFY(firstCard->hasFocus());
            QTest::keyClick(firstCard, Qt::Key_Down);
            QVERIFY(secondCard->hasFocus());
            QTest::keyClick(secondCard, Qt::Key_Up);
            QVERIFY(firstCard->hasFocus());
            QVERIFY(firstCard->toolTip().contains(firstServer.url().authority()));
            QVERIFY(popup->screen()->availableGeometry().contains(popup->geometry()));
            QVERIFY(QRect(window.mapToGlobal(QPoint()), window.size()).contains(popup->geometry()));
            secondCard->click();
            QVERIFY(!visiblePopup());
            QCOMPARE(window.currentAccount(), second);
            QCOMPARE(headerSearch->currentWidget(), secondSearch);
            profile->click();
            popup = visiblePopup();
            QVERIFY(popup);
            QTest::keyClick(popup, Qt::Key_Escape);
            QVERIFY(!visiblePopup());
            profile->click();
            popup = visiblePopup();
            QVERIFY(popup);
            auto *manage = popup->findChild<QAbstractButton *>("drivePopupManageAccount");
            QVERIFY(manage);
            manage->click();
            QCOMPARE(window.currentPage(), SettingsDialog::SettingsPage::Account);
            QCOMPARE(window.currentAccount(), second);
            QVERIFY(!visiblePopup());
            window.setCurrentAccount(first);
            window.setCurrentPage(SettingsDialog::SettingsPage::Files);
            if (qEnvironmentVariableIsSet("UDRIVE_UI_TEST_CAPTURE")) {
                firstServer.uploaded.insert("/dav/personal/Workspace timeline.pdf", QByteArray("preview fixture"));
                firstServer.uploaded.insert("/dav/personal/Project proposal.pptx", QByteArray("preview fixture"));
                firstServer.uploaded.insert("/dav/personal/Video.mp4", QByteArray("preview fixture"));
                firstServer.uploaded.insert("/dav/personal/Team budget.xlsx", QByteArray("preview fixture"));
                firstBrowser->findChild<QPushButton *>("driveRefresh")->click();
                QTRY_COMPARE(tree->model()->rowCount(), 11);
                const auto captureFileMenu = [&](const QString &theme) {
                    QVERIFY(select(tree, QStringLiteral("a&b.txt")).isValid());
                    auto *menu = firstBrowser->findChild<QMenu *>(QStringLiteral("driveActionsMenu"));
                    menu->popup(window.mapToGlobal(QPoint(window.width() - 8, 200)));
                    QTest::qWait(50);
                    QVERIFY(menu->grab().save(QStringLiteral("/tmp/udrive-file-actions-menu-%1.png").arg(theme)));
                    menu->close();
                };
                const QPalette previous = qApp->palette();
                auto dark = previous;
                dark.setColor(QPalette::Window, QColor("#202020"));
                dark.setColor(QPalette::WindowText, QColor("#EDEEF0"));
                dark.setColor(QPalette::Base, QColor("#292929"));
                dark.setColor(QPalette::Text, QColor("#EDEEF0"));
                dark.setColor(QPalette::Button, QColor("#292929"));
                dark.setColor(QPalette::ButtonText, QColor("#EDEEF0"));
                qApp->setPalette(dark);
                QTest::qWait(150);
                QVERIFY(window.grab().save("/tmp/udrive-workspace-preview.png"));
                captureFileMenu(QStringLiteral("dark"));
                firstSearch->setText(QStringLiteral("Project"));
                firstSearch->setFocus();
                QTest::qWait(50);
                QVERIFY(firstSearch->grab().save("/tmp/udrive-search-dark.png"));
                clearSearch->click();
                const auto captureNewMenu = [&](const QString &theme) {
                    window.openNewFileMenu();
                    auto *menu = firstBrowser->findChild<QMenu *>("driveNewMenu");
                    QVERIFY(menu);
                    QTest::qWait(50);
                    QVERIFY(menu->grab().save(QStringLiteral("/tmp/udrive-new-menu-%1.png").arg(theme)));
                    menu->setActiveAction(menu->actions().first());
                    QTest::qWait(30);
                    QVERIFY(menu->grab().save(QStringLiteral("/tmp/udrive-new-menu-%1-hover.png").arg(theme)));
                    menu->hide();
                };
                captureNewMenu(QStringLiteral("dark"));
                window.openNewFileMenu();
                if (auto *newMenu = firstBrowser->findChild<QMenu *>("driveNewMenu")) {
                    QTest::qWait(50);
                    newMenu->grab().save("/tmp/udrive-new-menu-full-width.png");
                    newMenu->hide();
                }
                profile->click();
                QVERIFY(visiblePopup());
                QTest::qWait(100);
                QVERIFY(visiblePopup()->grab().save("/tmp/udrive-accounts-popup.png"));
                visiblePopup()->close();
                QTranslator khmer;
                QVERIFY(khmer.load(QStringLiteral("client_km"), Translations::translationsDirectoryPath()));
                QVERIFY(qApp->installTranslator(&khmer));
                profile->click();
                QVERIFY(visiblePopup());
                QTest::qWait(100);
                QVERIFY(visiblePopup()->grab().save("/tmp/udrive-accounts-popup-khmer.png"));
                visiblePopup()->close();
                captureFileMenu(QStringLiteral("khmer"));
                qApp->removeTranslator(&khmer);
                window.resize(960, 700);
                QTest::qWait(100);
                QVERIFY(window.grab().save("/tmp/udrive-workspace-compact.png"));
                qApp->setPalette(previous);
                QTest::qWait(150);
                QVERIFY(window.grab().save("/tmp/udrive-workspace-light.png"));
                captureFileMenu(QStringLiteral("light"));
                captureNewMenu(QStringLiteral("light"));
                firstSearch->setText(QStringLiteral("Project"));
                firstSearch->setFocus();
                QTest::qWait(50);
                QVERIFY(firstSearch->grab().save("/tmp/udrive-search-light.png"));
                clearSearch->click();
                profile->click();
                QVERIFY(visiblePopup());
                QTest::qWait(50);
                QVERIFY(visiblePopup()->grab().save("/tmp/udrive-accounts-popup-light.png"));
                visiblePopup()->close();
                window.setCurrentPage(SettingsDialog::SettingsPage::Trash);
                auto *spacePicker = firstBrowser->findChild<QComboBox *>("driveSpaces");
                QTRY_VERIFY(spacePicker->isVisible() && spacePicker->count() > 1);
                for (const auto &theme : {QStringLiteral("dark"), QStringLiteral("light")}) {
                    qApp->setPalette(theme == QStringLiteral("dark") ? dark : previous);
                    QTest::qWait(50);
                    window.grab().save(QStringLiteral("/tmp/udrive-space-selector-%1.png").arg(theme));
                    spacePicker->showPopup(); QTest::qWait(50);
                    spacePicker->view()->window()->grab().save(QStringLiteral("/tmp/udrive-space-popup-%1.png").arg(theme));
                    spacePicker->hidePopup();
                    for (const auto &name : {QStringLiteral("driveSort"), QStringLiteral("driveViewMode")}) {
                        auto *button = firstBrowser->findChild<QPushButton *>(name);
                        button->menu()->popup(button->mapToGlobal(QPoint(0, button->height())));
                        QTest::qWait(50);
                        button->menu()->grab().save(QStringLiteral("/tmp/udrive-%1-popup-%2.png").arg(name, theme));
                        button->menu()->hide();
                    }
                }
                qApp->setPalette(previous);
                window.setCurrentPage(SettingsDialog::SettingsPage::Files);
            }
            // Closing a window with accounts and an open popup must not let
            // child cleanup re-enter an already destroyed SettingsDialog.
            auto transient = std::make_unique<SettingsDialog>(nullptr);
            transient->show();
            transient->findChild<QPushButton *>("driveProfile")->click();
            QVERIFY(transient->findChild<QWidget *>("driveAccountPopup")->isVisible());
            transient.reset();
            // Removing an account must remove its header search field too.
            profile->click();
            QVERIFY(visiblePopup());
            manager->deleteAccount(firstState);
            QVERIFY(!visiblePopup());
            QCOMPARE(headerSearch->count(), 2);
            QCOMPARE(window.currentAccount(), second);
            QCOMPARE(headerSearch->currentWidget(), secondSearch);
            if (qEnvironmentVariableIsSet("UDRIVE_UI_TEST_CAPTURE")) {
                const auto previous = qApp->palette();
                QTranslator khmer;
                QVERIFY(khmer.load(QStringLiteral("client_km"), Translations::translationsDirectoryPath()));
                QVERIFY(qApp->installTranslator(&khmer));
                const auto restore = qScopeGuard([&] { qApp->removeTranslator(&khmer); qApp->setPalette(previous); });
                for (const auto &theme : {QStringLiteral("dark"), QStringLiteral("light")}) {
                    auto palette = previous;
                    palette.setColor(QPalette::Window, QColor(theme == QStringLiteral("dark") ? "#202020" : "#FFFFFF"));
                    qApp->setPalette(palette);
                    QTest::qWait(50);
                    profile->click();
                    QVERIFY(visiblePopup());
                    QTest::qWait(50);
                    QVERIFY(visiblePopup()->grab().save(QStringLiteral("/tmp/udrive-profile-single-km-%1.png").arg(theme)));
                    visiblePopup()->close();
                }
            }
            manager->deleteAccount(secondState);
            QCOMPARE(headerSearch->count(), 1);
            QVERIFY(!window.canCreateFiles());
            home->click();
            QCOMPARE(window.currentPage(), SettingsDialog::SettingsPage::Settings);
            QVERIFY(!window.currentAccount());
        }
    }

#ifdef Q_OS_MACOS
    void pdfWorkerBudgetAndRecovery_data()
    {
        QTest::addColumn<bool>("nested");
        QTest::newRow("nested-forms-original-trigger") << true;
        QTest::newRow("compressed-command-stream") << false;
    }
    void pdfWorkerBudgetAndRecovery()
    {
        QFETCH(bool, nested);
        QTemporaryDir files; QFile expensive(files.filePath("expensive.pdf"));
        QVERIFY(expensive.open(QIODevice::WriteOnly)); const auto bytes = expensivePdfFixture(nested);
        QVERIFY(bytes.size() < UDrivePdfDocument::MaximumFileSize); expensive.write(bytes); expensive.close();
        QFile normal(files.filePath("normal.pdf")); QVERIFY(normal.open(QIODevice::WriteOnly)); normal.write(pdfFixture()); normal.close();
        UDrivePdfPreview preview; preview.resize(700, 600); preview.show();
        QSignalSpy failed(&preview, &UDrivePdfPreview::previewFailed), rendered(&preview, &UDrivePdfPreview::pageRendered);
        QElapsedTimer elapsed; elapsed.start(); preview.openFile(expensive.fileName(), "Expensive PDF");
        QPointer<QProcess> process = preview.findChild<QProcess *>("pdfRendererProcess"); QVERIFY(process);
        QTRY_COMPARE(process->state(), QProcess::Running); const auto pid = process->processId();
        QTRY_VERIFY_WITH_TIMEOUT(!failed.isEmpty() || !rendered.isEmpty(), UDrivePdfPreview::RenderTimeoutMs + 4000);
        QVERIFY(elapsed.elapsed() < UDrivePdfPreview::RenderTimeoutMs + 4000);
        if (nested) {
            QVERIFY(rendered.isEmpty()); QVERIFY(!failed.isEmpty());
            QVERIFY(preview.findChild<QLabel *>("pdfStatus")->text().contains("too long"));
            QTRY_VERIFY(process.isNull());
            QCOMPARE(::kill(pid, 0), -1); QCOMPARE(errno, ESRCH);
        }
        preview.clear(); QTRY_VERIFY(process.isNull());
        failed.clear(); rendered.clear(); preview.openFile(normal.fileName(), "Normal PDF");
        QTRY_COMPARE(rendered.count(), 1); QCOMPARE(failed.count(), 0);
        // Closing and replacement must terminate native work before opening another document.
        preview.openFile(expensive.fileName(), "Cancelled PDF");
        process = preview.findChild<QProcess *>("pdfRendererProcess");
        QTRY_COMPARE(process->state(), QProcess::Running); const auto cancelledPid = process->processId();
        QTest::qWait(30); preview.clear(); QTRY_VERIFY(process.isNull());
        QCOMPARE(::kill(cancelledPid, 0), -1); QCOMPARE(errno, ESRCH);
        rendered.clear(); preview.openFile(normal.fileName(), "After cancellation"); QTRY_COMPARE(rendered.count(), 1);
        process = preview.findChild<QProcess *>("pdfRendererProcess"); preview.clear(); QTRY_VERIFY(process.isNull());
        auto *closing = new UDrivePdfPreview;
        closing->openFile(expensive.fileName(), "Destroyed preview");
        process = closing->findChild<QProcess *>("pdfRendererProcess");
        QTRY_COMPARE(process->state(), QProcess::Running); const auto destroyedPid = process->processId();
        delete closing; QVERIFY(process.isNull());
        QCOMPARE(::kill(destroyedPid, 0), -1); QCOMPARE(errno, ESRCH);
    }

    void folderNavigationCancelsPendingPdf_data()
    {
        QTest::addColumn<bool>("tiles"); QTest::newRow("table") << false; QTest::newRow("tiles") << true;
    }
    void folderNavigationCancelsPendingPdf()
    {
        QFETCH(bool, tiles);
        auto settings = ConfigFile::settingsWithGroup(QStringLiteral("UDriveFileBrowser"));
        const auto previous = settings->value(QStringLiteral("viewMode"));
        settings->setValue(QStringLiteral("viewMode"), QStringLiteral("default"));
        auto restore = qScopeGuard([previous] {
            auto settings = ConfigFile::settingsWithGroup(QStringLiteral("UDriveFileBrowser"));
            if (previous.isValid()) settings->setValue(QStringLiteral("viewMode"), previous);
            else settings->remove(QStringLiteral("viewMode"));
        });
        DriveServer server; server.holdPdf = true; server.uploaded.insert("/dav/personal/Preview.pdf", pdfFixture());
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account)); account->credentials()->fetchFromKeychain();
        AccountState state(account); UDriveFileBrowser browser(&state); browser.show();
        auto *tree = browser.findChild<QTreeView *>("driveFiles"); auto *preview = browser.findChild<UDrivePdfPreview *>();
        QTRY_COMPARE(tree->model()->rowCount(), 8);
        if (tiles) browser.findChild<QAction *>("driveViewTiles")->trigger();
        QSignalSpy downloaded(&browser, &UDriveFileBrowser::downloadFinished), rendered(preview, &UDrivePdfPreview::pageRendered);
        select(tree, "Preview.pdf"); QVERIFY(browser.previewSelectedPdf()); QTRY_VERIFY(server.slowStarted);
        const auto folder = select(tree, "Docs"); QVERIFY(folder.isValid());
        if (tiles) Q_EMIT browser.findChild<QListView *>("driveTiles")->activated(folder);
        else Q_EMIT tree->activated(folder);
        QTRY_COMPARE(tree->model()->rowCount(), 1);
        QCOMPARE(downloaded.count(), 1); QVERIFY(!downloaded.first().at(0).toBool());
        QVERIFY(!QFileInfo::exists(downloaded.first().at(1).toString()));
        QTest::qWait(100); QVERIFY(!preview->isVisible()); QCOMPARE(rendered.count(), 0);
    }

    void pdfRendering()
    {
        QTemporaryDir files;
        const QString path = files.filePath("sample.pdf");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(pdfFixture()); file.close();
        QString error;
        auto document = UDrivePdfDocument::open(path, &error);
        QVERIFY2(document, qPrintable(error));
        QCOMPARE(document->pageCount(), 2);
        QCOMPARE(document->pageSize(1), QSizeF(300, 400));
        const auto image = document->render(1, QSize(300, 400));
        QCOMPARE(image.pixelColor(25, 25), QColor(Qt::red));
        QCOMPARE(image.pixelColor(25, 375), QColor(Qt::white));
        const auto enlarged = document->render(1, QSize(600, 800));
        // Enlarging the page must also enlarge its content, rather than leaving
        // the original-size artwork centered in a larger white bitmap.
        QCOMPARE(enlarged.pixelColor(100, 70), QColor(Qt::red));
        QCOMPARE(enlarged.pixelColor(170, 110), QColor(Qt::white));
        QCOMPARE(document->render(2, QSize(300, 400)).pixelColor(25, 25), QColor(Qt::blue));
        QVERIFY(document->render(0, QSize(300, 400)).isNull());
        QVERIFY(document->render(1, QSize(5000, 5000)).isNull());
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(rotatedPdfFixture()); file.close();
        auto rotated = UDrivePdfDocument::open(path, &error);
        QVERIFY2(rotated, qPrintable(error));
        QCOMPARE(rotated->pageSize(1), QSizeF(300, 200));
        const auto rotatedImage = rotated->render(1, QSize(300, 200));
        QCOMPARE(rotatedImage.pixelColor(25, 25), QColor(Qt::red));
        QCOMPARE(rotatedImage.pixelColor(275, 175), QColor(Qt::white));
        const auto enlargedRotated = rotated->render(1, QSize(600, 400));
        QCOMPARE(enlargedRotated.pixelColor(50, 50), QColor(Qt::red));
        QCOMPARE(enlargedRotated.pixelColor(550, 350), QColor(Qt::white));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("invalid PDF"); file.close();
        QVERIFY(!UDrivePdfDocument::open(path, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QVERIFY(file.resize(UDrivePdfDocument::MaximumFileSize + 1)); file.close();
        QVERIFY(!UDrivePdfDocument::open(path, &error));
        QVERIFY(error.contains("too large"));
    }

    void pdfPreviewCloud()
    {
        DriveServer server;
        server.uploaded.insert("/dav/personal/Preview.PDF", pdfFixture());
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveFileBrowser browser(&state);
        browser.resize(840, 640); browser.show();
        auto *tree = browser.findChild<QTreeView *>("driveFiles");
        auto *preview = browser.findChild<UDrivePdfPreview *>();
        QSignalSpy rendered(preview, &UDrivePdfPreview::pageRendered);
        QSignalSpy downloaded(&browser, &UDriveFileBrowser::downloadFinished);
        QSignalSpy failed(preview, &UDrivePdfPreview::previewFailed);
        FileOpenReceiver receiver;
        QDesktopServices::setUrlHandler("file", &receiver, "record");
        struct ResetHandler { ~ResetHandler() { QDesktopServices::unsetUrlHandler("file"); } } reset;
        QTRY_COMPARE(tree->model()->rowCount(), 8);
        QVERIFY(select(tree, "Preview.PDF").isValid());
        QVERIFY(browser.findChild<QPushButton *>("driveOpen")->isEnabled());
        // Double-click/Enter follows the same path as Open PDF.
        Q_EMIT tree->activated(tree->currentIndex());
        QTRY_VERIFY_WITH_TIMEOUT(!rendered.isEmpty(), 5000);
        QVERIFY(preview->isVisible()); QVERIFY(!tree->isVisible());
        QCOMPARE(preview->findChild<QComboBox *>("pdfZoom")->currentData().toInt(), -1); // Fit width
        auto *canvas = preview->findChild<QAbstractScrollArea *>("pdfCanvas");
        auto *pageImage = preview->findChild<QLabel *>("pdfPageImage");
        QVERIFY(canvas && pageImage);
        QTRY_VERIFY(pageImage->width() >= canvas->viewport()->width() * 0.9
            && pageImage->width() <= canvas->viewport()->width());
        QVERIFY(preview->findChild<QLabel *>("pdfPageImage")->pixmap().height() > 200);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(downloaded.count(), 1);
        QVERIFY(downloaded.first().at(0).toBool());
        QString cache = downloaded.first().at(1).toString();
        QVERIFY(QFileInfo::exists(cache));
        QCOMPARE(contents(cache), server.uploaded.value("/dav/personal/Preview.PDF"));
        QVERIFY(!(QFileInfo(cache).permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther)));
        QVERIFY(server.authenticated);
        QCOMPARE(server.requestHeaders.value("GET /dav/personal/Preview.PDF").value("if-match"), QByteArray("\"tag\""));
        auto *page = preview->findChild<QSpinBox *>("pdfPage");
        QCOMPARE(page->maximum(), 2);
        preview->findChild<QPushButton *>("pdfNext")->click();
        QTRY_COMPARE(rendered.last().at(0).toInt(), 2);
        QVERIFY(!preview->findChild<QPushButton *>("pdfNext")->isEnabled());
        // Compare explicit zoom levels; the default fit-width scale depends on
        // the viewport and can already exceed 200% in a wide window.
        preview->findChild<QComboBox *>("pdfZoom")->setCurrentIndex(4); // 100%
        QTRY_COMPARE(preview->findChild<QLabel *>("pdfPageImage")->size(), QSize(300, 400));
        const auto before = preview->findChild<QLabel *>("pdfPageImage")->pixmap().size();
        preview->findChild<QComboBox *>("pdfZoom")->setCurrentIndex(7); // 200%
        QTRY_VERIFY(preview->findChild<QLabel *>("pdfPageImage")->pixmap().height() > before.height());
        page->setValue(1);
        QTRY_COMPARE(rendered.last().at(0).toInt(), 1);
        if (qEnvironmentVariableIsSet("UDRIVE_PDF_TEST_CAPTURE")) browser.grab().save("/tmp/udrive-pdf-preview.png");
        preview->findChild<QPushButton *>("pdfBack")->click();
        QVERIFY(QFileInfo::exists(cache));
        QVERIFY(tree->isVisible());
        QCOMPARE(receiver.opened, QUrl());

        // Reopening the unchanged PDF uses the same temporary version.
        rendered.clear();
        QVERIFY(browser.previewSelectedPdf());
        QTRY_VERIFY(!rendered.isEmpty());
        QCOMPARE(downloaded.count(), 1);
        preview->findChild<QPushButton *>("pdfBack")->click();
        browser.hide();
        QTRY_VERIFY(!QFileInfo::exists(cache));
        browser.show();

        // Server failures must stay in the file browser and leave no preview copy.
        downloaded.clear(); rendered.clear(); server.pdfResponse = 403;
        QVERIFY(browser.previewSelectedPdf());
        QTRY_COMPARE(downloaded.count(), 1);
        QVERIFY(!downloaded.first().at(0).toBool());
        QVERIFY(!QFileInfo::exists(downloaded.first().at(1).toString()));
        QVERIFY(!preview->isVisible());
        server.pdfResponse = 200;
        server.holdPdf = true; server.heldPdfSize = UDrivePdfDocument::MaximumFileSize + 1;
        downloaded.clear();
        QVERIFY(browser.previewSelectedPdf());
        QTRY_COMPARE(downloaded.count(), 1);
        QVERIFY(!downloaded.first().at(0).toBool());
        QVERIFY(!QFileInfo::exists(downloaded.first().at(1).toString()));
        QVERIFY(browser.findChild<QLabel *>("driveStatus")->text().contains("too large"));
        server.holdPdf = false; server.heldPdfSize = 1048576; server.slowStarted = false;
        server.uploaded["/dav/personal/Preview.PDF"] = "invalid PDF";
        downloaded.clear();
        QVERIFY(browser.previewSelectedPdf());
        QTRY_COMPARE(failed.count(), 1);
        QVERIFY(preview->findChild<QLabel *>("pdfStatus")->text().contains("readable PDF"));
        cache = downloaded.last().at(1).toString();
        browser.showWorkspaces();
        QTRY_VERIFY(!QFileInfo::exists(cache));
        QTRY_COMPARE(tree->model()->rowCount(), 1);
        QVERIFY(!preview->isVisible());
        browser.showPersonal();
        QTRY_COMPARE(tree->model()->rowCount(), 8);
        select(tree, "Preview.PDF");
        server.holdPdf = true; downloaded.clear();
        QVERIFY(browser.previewSelectedPdf());
        QTRY_VERIFY(server.slowStarted);
        browser.showWorkspaces();
        QTRY_COMPARE(downloaded.count(), 1);
        QVERIFY(!downloaded.first().at(0).toBool());
        QVERIFY(!QFileInfo::exists(downloaded.first().at(1).toString()));
        QVERIFY(!preview->isVisible());
        server.holdPdf = false;
        server.uploaded["/dav/personal/Preview.PDF"] = pdfFixture();
        browser.showPersonal();
        QTRY_COMPARE(tree->model()->rowCount(), 8);
        select(tree, "Preview.PDF"); downloaded.clear(); rendered.clear();
        QVERIFY(browser.previewSelectedPdf());
        QTRY_VERIFY(!rendered.isEmpty());
        cache = downloaded.last().at(1).toString();
        // Hiding an account's browser must discard its private preview.
        browser.hide();
        QTRY_VERIFY(!QFileInfo::exists(cache));
        QVERIFY(preview->findChild<QLabel *>("pdfPageImage")->pixmap().isNull());
        browser.show(); rendered.clear(); downloaded.clear();
        QVERIFY(browser.previewSelectedPdf());
        QTRY_VERIFY(!rendered.isEmpty());
        cache = downloaded.last().at(1).toString();
        state.signOutByUi();
        QTRY_VERIFY(!QFileInfo::exists(cache));
        QVERIFY(!preview->isVisible());
        QVERIFY(!browser.previewSelectedPdf());
        QCOMPARE(tree->model()->rowCount(), 0);
    }

    void pdfPreviewSyncedFile()
    {
        DriveServer server;
        server.uploaded.insert("/dav/personal/Local.pdf", "not the local PDF");
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        struct FolderCleanup {
            FolderMan *folders;
            ~FolderCleanup() { folders->unloadAndDeleteAllFolders(); }
        } cleanup {_folders.get()};
        QTemporaryDir synced;
        QFile file(synced.filePath("Local.pdf"));
        QVERIFY(file.open(QIODevice::WriteOnly)); file.write(pdfFixture()); file.close();
        FolderDefinition definition(server.url("/dav/personal"), "personal", "Personal");
        definition.setLocalPath(synced.path()); definition.setTargetPath("");
        definition.setJournalPath(".test-sync.db"); definition.setPaused(true);
        auto *folder = _folders->addFolder(&state, definition);
        QVERIFY(folder); QVERIFY(!folder->hasSetupError());
        UDriveFileBrowser browser(&state);
        browser.resize(840, 640); browser.show();
        auto *tree = browser.findChild<QTreeView *>("driveFiles");
        auto *preview = browser.findChild<UDrivePdfPreview *>();
        QSignalSpy rendered(preview, &UDrivePdfPreview::pageRendered);
        QTRY_COMPARE(tree->model()->rowCount(), 8);
        select(tree, "Local.pdf");
        QVERIFY(browser.previewSelectedPdf());
        QTRY_VERIFY(!rendered.isEmpty());
        QVERIFY(!server.requests.contains({"GET", "/dav/personal/Local.pdf"}));
        QCOMPARE(preview->findChild<QSpinBox *>("pdfPage")->maximum(), 2);
        preview->findChild<QPushButton *>("pdfBack")->click();
        QVERIFY(QFileInfo::exists(file.fileName()));
    }

    void pdfContinuousScrolling()
    {
        QTemporaryDir files;
        const QString path = files.filePath("continuous.pdf");
        {
            QPdfWriter writer(path);
            writer.setResolution(72);
            writer.setPageSize(QPageSize(QSizeF(300, 400), QPageSize::Point));
            writer.setPageMargins(QMarginsF());
            QPainter painter(&writer);
            painter.fillRect(QRect(0, 0, 300, 400), Qt::red);
            for (int page = 2; page <= 24; ++page) {
                const QSize size = page == 2 ? QSize(500, 250) : QSize(300, 400);
                writer.setPageSize(QPageSize(size, QPageSize::Point));
                QVERIFY(writer.newPage());
                painter.fillRect(QRect(QPoint(), size), page == 2 ? Qt::blue : Qt::green);
            }
        }
        UDrivePdfPreview preview;
        preview.resize(700, 650); preview.show();
        preview.findChild<QComboBox *>("pdfZoom")->setCurrentIndex(4); // 100%
        QSignalSpy rendered(&preview, &UDrivePdfPreview::pageRendered);
        QSignalSpy failed(&preview, &UDrivePdfPreview::previewFailed);
        preview.openFile(path, "Continuous document");
        auto *worker = preview.findChild<QProcess *>("pdfRendererProcess");
        QVERIFY(worker);
        QSignalSpy requests(worker, &QIODevice::bytesWritten);
        auto *canvas = preview.findChild<QAbstractScrollArea *>("pdfCanvas");
        auto *page = preview.findChild<QSpinBox *>("pdfPage");
        auto *image = preview.findChild<QLabel *>("pdfPageImage");
        QVERIFY(canvas && page && image);
        QTRY_VERIFY(!rendered.isEmpty());
        QCOMPARE(page->maximum(), 24);
        QCOMPARE(page->value(), 1);
        // Both pages occupy the same scrollable viewport, including the second
        // page's different dimensions; it is not a single-page replacement.
        const auto lowerPageColor = [&] {
            const auto view = canvas->viewport()->grab().toImage();
            return view.pixelColor(view.width() / 2, qRound(500 * view.devicePixelRatio()));
        };
        QTRY_VERIFY(lowerPageColor().blue() > 200 && lowerPageColor().red() < 30);
        QCOMPARE(page->value(), 1);
        QVERIFY(!requests.isEmpty() && requests.size() <= 4); // Only visible pages, not all 24.
        QVERIFY(canvas->verticalScrollBar()->maximum() > 8000);
        canvas->verticalScrollBar()->setValue(300);
        QTRY_COMPARE(page->value(), 2);
        QTRY_COMPARE(image->size(), QSize(500, 250));
        QTRY_COMPARE(rendered.last().at(0).toInt(), 2);
        // Scrolling back reuses the first page and synchronizes the toolbar.
        canvas->verticalScrollBar()->setValue(0);
        QTRY_COMPARE(page->value(), 1);
        QTRY_COMPARE(rendered.last().at(0).toInt(), 1);
        QVERIFY(image->pixmap().toImage().pixelColor(50, 50).red() > 200);
        // Page controls jump within the document; zoom retains the selected page.
        page->setValue(24);
        QTRY_COMPARE(rendered.last().at(0).toInt(), 24);
        preview.findChild<QPushButton *>("pdfPrevious")->click();
        QTRY_COMPARE(rendered.last().at(0).toInt(), 23);
        preview.findChild<QComboBox *>("pdfZoom")->setCurrentIndex(7); // 200%
        QTRY_COMPARE(image->size(), QSize(600, 800));
        QCOMPARE(page->value(), 23);
        canvas->verticalScrollBar()->setValue(canvas->verticalScrollBar()->maximum());
        QTRY_COMPARE(page->value(), 24);
        QTRY_COMPARE(rendered.last().at(0).toInt(), 24);
        QCOMPARE(failed.count(), 0);
        preview.clear();
        QCOMPARE(canvas->verticalScrollBar()->maximum(), 0);
        QVERIFY(image->pixmap().isNull());
    }

    void pdfPreviewLocalAndStaleWork()
    {
        QTemporaryDir files;
        QFile file(files.filePath("preview.pdf"));
        QVERIFY(file.open(QIODevice::WriteOnly)); file.write(pdfFixture()); file.close();
        UDrivePdfPreview preview;
        preview.resize(700, 600); preview.show();
        QSignalSpy rendered(&preview, &UDrivePdfPreview::pageRendered);
        QSignalSpy failed(&preview, &UDrivePdfPreview::previewFailed);
        preview.openFile(file.fileName(), "Local document");
        // Closing/replacing while a worker runs must not show stale pages.
        preview.clear();
        QTest::qWait(200);
        QCOMPARE(rendered.count(), 0);
        preview.openFile(file.fileName(), "Local document");
        QTRY_VERIFY(!rendered.isEmpty());
        QCOMPARE(failed.count(), 0);
        // Page/zoom changes must continue using the worker's immutable snapshot.
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); file.write("replaced by sync"); file.close();
        rendered.clear();
        auto *page = preview.findChild<QSpinBox *>("pdfPage");
        page->setValue(2); page->setValue(1); page->setValue(2);
        QTRY_VERIFY(!rendered.isEmpty());
        QCOMPARE(rendered.last().at(0).toInt(), 2);
        preview.clear();
        QTest::qWait(100);
        QVERIFY(preview.findChild<QLabel *>("pdfPageImage")->pixmap().isNull());
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(pdfFixture(QSizeF(2000, 3000))); file.close();
        rendered.clear();
        preview.openFile(file.fileName(), "Large page");
        QTRY_VERIFY(!rendered.isEmpty());
        rendered.clear();
        preview.findChild<QComboBox *>("pdfZoom")->setCurrentIndex(8); // 300%
        QTRY_VERIFY_WITH_TIMEOUT(!rendered.isEmpty(), 15000);
        auto *image = preview.findChild<QLabel *>("pdfPageImage");
        QCOMPARE(image->size(), QSize(6000, 9000));
        QCOMPARE(image->pixmap().height(), 4096);
        preview.clear();
        // Destruction with a render in progress must also be safe.
        auto *closing = new UDrivePdfPreview;
        closing->openFile(file.fileName(), "Closing document");
        delete closing;
        QTest::qWait(100);
    }
#endif

    void breadcrumbNavigation()
    {
        DriveServer server;
        const QString design = QStringLiteral("/dav/personal/Docs/Design & research");
        const QString year = design + QStringLiteral("/<2026>");
        const QString drafts = year + QStringLiteral("/Drafts %2F");
        server.folders = {design, year, drafts};
        server.uploaded.insert(drafts + QStringLiteral("/Report.txt"), "report");
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveFileBrowser browser(&state);
        browser.resize(840, 640); browser.show();
        auto *tree = browser.findChild<QTreeView *>("driveFiles");
        auto *root = browser.findChild<QPushButton *>("driveRoot");
        QVERIFY(!browser.findChild<QPushButton *>("driveUp"));
        QTRY_COMPARE(tree->model()->rowCount(), 7);
        QVERIFY(!root->isEnabled());
        auto open = [&](const QString &name) {
            const auto index = select(tree, name);
            if (!index.isValid()) return false;
            Q_EMIT tree->activated(index);
            return true;
        };
        QVERIFY(open("Docs"));
        QTRY_COMPARE(tree->model()->rowCount(), 2);
        QVERIFY(open("Design & research"));
        QTRY_VERIFY(select(tree, "<2026>").isValid());
        QVERIFY(open("<2026>"));
        QTRY_VERIFY(select(tree, "Drafts %2F").isValid());
        QVERIFY(open("Drafts %2F"));
        QTRY_VERIFY(select(tree, "Report.txt").isValid());
        auto *current = browser.findChild<QLabel *>("driveCurrentFolder");
        QCOMPARE(current->accessibleName(), QStringLiteral("Drafts %2F"));
        QCOMPARE(current->textFormat(), Qt::PlainText);
        QCOMPARE(current->accessibleDescription(), QStringLiteral("Docs/Design & research/<2026>/Drafts %2F"));
        QVERIFY(server.requests.contains({"PROPFIND", drafts}));
        auto visibleButton = [&](const QString &name) -> QPushButton * {
            for (auto *button : browser.findChildren<QPushButton *>(name)) if (button->isVisible()) return button;
            return nullptr;
        };
        auto *overflow = visibleButton("driveBreadcrumbOverflow");
        QVERIFY(overflow); QVERIFY(overflow->menu());
        QCOMPARE(overflow->menu()->actions().size(), 2);
        QCOMPARE(overflow->menu()->actions().at(1)->text(), QStringLiteral("Design && research"));
        // Every hidden ancestor remains reachable through the menu.
        overflow->menu()->actions().at(1)->trigger();
        QTRY_VERIFY(select(tree, "<2026>").isValid());
        QCOMPARE(browser.findChild<QLabel *>("driveCurrentFolder")->accessibleName(), QStringLiteral("Design & research"));
        auto *parent = visibleButton("driveBreadcrumb");
        QVERIFY(parent);
        QCOMPARE(parent->property("breadcrumbPath").toString(), QStringLiteral("Docs"));
        parent->click();
        QTRY_COMPARE(tree->model()->rowCount(), 2);
        root->click();
        QTRY_COMPARE(tree->model()->rowCount(), 7);
        // An ancestor click cancels a slow listing rather than accepting its stale result.
        QVERIFY(open("slowfolder"));
        QTRY_VERIFY(server.requests.contains({"PROPFIND", "/dav/personal/slowfolder"}));
        root->click();
        QTRY_COMPARE(tree->model()->rowCount(), 7);
        QTest::qWait(200);
        QCOMPARE(tree->model()->rowCount(), 7);
        browser.showWorkspaces();
        QTRY_COMPARE(tree->model()->rowCount(), 1);
        QCOMPARE(root->toolTip(), QStringLiteral("Team files"));
        QVERIFY(!root->isEnabled());
        state.signOutByUi();
        QVERIFY(!browser.findChild<QWidget *>("driveBreadcrumbs")->isEnabled());
        QVERIFY(server.authenticated);
    }


    void uploadedImagePreviewsWithoutSaveDialogAndReusesCache()
    {
        DriveServer server;
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveFileBrowser browser(&state);
        auto *tree = browser.findChild<QTreeView *>("driveFiles");
        QTRY_COMPARE_WITH_TIMEOUT(tree->model()->rowCount(), 7, 5000);

        QTemporaryDir files;
        QVERIFY(files.isValid());
        const QString source = files.filePath(QString::fromUtf8("Photo ខ្មែរ & 100%.png"));
        QImage image(32, 32, QImage::Format_RGB32);
        image.fill(Qt::green);
        QVERIFY(image.save(source));
        const QByteArray payload = contents(source);
        QSignalSpy uploaded(&browser, &UDriveFileBrowser::operationFinished);
        QVERIFY(browser.uploadFile(source));
        QTRY_COMPARE(uploaded.count(), 1);
        QVERIFY(uploaded.first().at(0).toBool());
        QTRY_VERIFY(select(tree, QFileInfo(source).fileName()).isValid());
        auto *open = browser.findChild<QPushButton *>("driveOpen");
        QVERIFY(open->isEnabled());
        QCOMPARE(open->text(), QStringLiteral("Preview"));

        FileOpenReceiver receiver;
        QDesktopServices::setUrlHandler("file", &receiver, "record");
        const auto resetHandler = qScopeGuard([] { QDesktopServices::unsetUrlHandler("file"); });
        auto *preview = browser.findChild<UDriveFilePreview *>();
        QSignalSpy ready(preview, &UDriveFilePreview::ready);
        QSignalSpy downloaded(&browser, &UDriveFileBrowser::downloadFinished);
        Q_EMIT tree->activated(tree->currentIndex());
        QVERIFY(!browser.findChild<QFileDialog *>());
        QVERIFY(receiver.opened.isEmpty());
        QTRY_COMPARE(downloaded.count(), 1);
        QVERIFY(downloaded.first().at(0).toBool());
        QTRY_COMPARE(ready.count(), 1);
        const QString destination = downloaded.first().at(1).toString();
        QCOMPARE(contents(destination), payload);
        QVERIFY(!preview->findChild<QLabel *>("previewImage")->pixmap().isNull());
        QCOMPARE(receiver.opened, QUrl());
        preview->findChild<QPushButton *>("previewBack")->click();
        QVERIFY(QFileInfo::exists(destination));
        open->click();
        QTRY_COMPARE(ready.count(), 2);
        QCOMPARE(downloaded.count(), 1); // Same server version: no second transfer.
        preview->findChild<QPushButton *>("previewBack")->click();
        QVERIFY(tree->model()->setData(tree->currentIndex(), QStringLiteral("\"version2\""), Qt::UserRole + 9));
        open->click();
        QTRY_COMPARE(downloaded.count(), 2);
        QTRY_COMPARE(ready.count(), 3);
        state.signOutByUi();
        QTRY_VERIFY(!QFileInfo::exists(destination));
        QVERIFY(preview->findChild<QLabel *>("previewImage")->pixmap().isNull());
        QVERIFY(server.authenticated);
    }


    void officeFilesOpenAutomaticallyAsTemporaryReadOnlyCopies()
    {
        DriveServer server;
        server.uploaded.insert("/dav/personal/Report.docx", "office document fixture");
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveFileBrowser browser(&state);
        auto *tree = browser.findChild<QTreeView *>("driveFiles");
        QTRY_VERIFY(select(tree, "Report.docx").isValid());
        FileOpenReceiver receiver;
        QDesktopServices::setUrlHandler("file", &receiver, "record");
        const auto resetHandler = qScopeGuard([] { QDesktopServices::unsetUrlHandler("file"); });
        QSignalSpy downloaded(&browser, &UDriveFileBrowser::downloadFinished);
        auto *open = browser.findChild<QPushButton *>("driveOpen");
        QCOMPARE(open->text(), QStringLiteral("Open in app"));
        open->click();
        QTRY_VERIFY(!receiver.opened.isEmpty());
        QCOMPARE(receiver.openedContents, QByteArray("office document fixture"));
        const QString copy = receiver.opened.toLocalFile();
        QVERIFY(copy.endsWith(".docx"));
        QVERIFY(!(QFileInfo(copy).permissions() & QFileDevice::WriteOwner));
        QCOMPARE(downloaded.count(), 1);
        QVERIFY(!browser.findChild<QFileDialog *>());
        receiver.opened.clear();
        open->click();
        QTRY_VERIFY(!receiver.opened.isEmpty());
        QCOMPARE(downloaded.count(), 1);
        state.signOutByUi();
        QTRY_VERIFY(!QFileInfo::exists(copy));
    }

    void mediaPreviewRejectsInvalidImagesAndDoesNotRunUnknownFiles()
    {
        DriveServer server;
        server.uploaded.insert("/dav/personal/broken.png", "<html>This is not an image</html>");
        server.uploaded.insert("/dav/personal/program.exe", "not an executable");
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveFileBrowser browser(&state);
        auto *tree = browser.findChild<QTreeView *>("driveFiles");
        QTRY_VERIFY(select(tree, "broken.png").isValid());
        auto *preview = browser.findChild<UDriveFilePreview *>();
        QSignalSpy ready(preview, &UDriveFilePreview::ready);
        QSignalSpy fetched(&browser, &UDriveFileBrowser::downloadFinished);
        QVERIFY(browser.previewSelectedFile());
        QTRY_COMPARE(fetched.count(), 1);
        QTRY_VERIFY(preview->findChild<QLabel *>("previewStatus")->text().contains("could not be previewed"));
        QCOMPARE(ready.count(), 0);
        preview->findChild<QPushButton *>("previewBack")->click();
        QVERIFY(select(tree, "program.exe").isValid());
        const int requests = server.requests.count();
        QVERIFY(browser.previewSelectedFile());
        QVERIFY(preview->findChild<QLabel *>("previewStatus")->text().contains("not available"));
        QCOMPARE(server.requests.count(), requests);
        QVERIFY(!browser.findChild<QFileDialog *>());
    }

    void suspiciousDoubleExtensionsRejectedFromPreview()
    {
        for (const auto &name : {"invoice.exe.docx", "document.bat.pdf", "report.vbs.txt", "picture.scr.png", "malware.cmd.xlsx", "script.ps1.txt", "setup.msi.rtf"}) {
            QVERIFY(UDriveFilePreview::hasSuspiciousDoubleExtension(name));
            QCOMPARE(UDriveFilePreview::kindForName(name), UDriveFilePreview::Kind::Unsupported);
        }
        for (const auto &name : {"component.test.tsx", "data.sample.json", "config.backup.yaml", "main.spec.js", "Report.final.docx"}) {
            QVERIFY(!UDriveFilePreview::hasSuspiciousDoubleExtension(name));
            QVERIFY(UDriveFilePreview::kindForName(name) != UDriveFilePreview::Kind::Unsupported);
        }
        QVERIFY(!UDriveFilePreview::hasSuspiciousDoubleExtension(QStringLiteral("archive.tar.gz")));
    }

    void developerCodeAndConfigPreview()
    {
        for (const auto &name : {"main.py", "script.js", "types.ts", "component.tsx", "native.c", "engine.cpp", "header.h", "run.sh", "config.ini", "server.conf"}) {
            QCOMPARE(UDriveFilePreview::kindForName(name), UDriveFilePreview::Kind::Text);
        }
        DriveServer server;
        server.uploaded.insert("/dav/personal/main.py", "def hello():\n    return 'UDrive'\n");
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveFileBrowser browser(&state);
        auto *tree = browser.findChild<QTreeView *>("driveFiles");
        QTRY_VERIFY(select(tree, "main.py").isValid());
        auto *preview = browser.findChild<UDriveFilePreview *>();
        QSignalSpy ready(preview, &UDriveFilePreview::ready);
        QVERIFY(browser.previewSelectedFile());
        QTRY_COMPARE(ready.count(), 1);
        QCOMPARE(browser.findChild<QPlainTextEdit *>("previewText")->toPlainText(), QStringLiteral("def hello():\n    return 'UDrive'\n"));
        QVERIFY(preview->findChild<QLabel *>("previewStatus")->text().contains("text preview"));
    }

    void audioExtensionsPreview()
    {
        for (const auto &name : {"track.mp3", "audio.wav", "voice.m4a", "lossless.flac", "sound.ogg"}) {
            QCOMPARE(UDriveFilePreview::kindForName(name), UDriveFilePreview::Kind::Audio);
            QCOMPARE(UDriveFilePreview::maximumBytes(UDriveFilePreview::Kind::Audio), 100LL * 1024 * 1024);
        }
        UDriveFilePreview preview;
        preview.showLoading("track.mp3");
        QCOMPARE(preview.findChild<QLabel *>("previewStatus")->text(), QStringLiteral("Loading preview…"));
    }

    void orphanPreviewSweepRemovesStaleAndReadOnlyDirectories()
    {
        UDriveFileBrowser::cleanupOrphanedPreviews();
        const QString root = Account::commonCacheDirectory() + QStringLiteral("/preview-sessions");
        QTemporaryDir stale(root + QStringLiteral("/session-test-XXXXXX"));
        QVERIFY(stale.isValid());
        const QString previewPath = stale.filePath(QStringLiteral("preview"));
        const QString pdfPath = stale.filePath(QStringLiteral("pdf"));
        QVERIFY(QDir().mkdir(previewPath));
        QVERIFY(QDir().mkdir(pdfPath));
        QFile previewFile(previewPath + "/locked-doc.docx");
        QVERIFY(previewFile.open(QIODevice::WriteOnly));
        previewFile.write("locked content");
        previewFile.close();
        QVERIFY(previewFile.setPermissions(QFileDevice::ReadOwner));
        QFile pdfFile(pdfPath + "/test.pdf");
        QVERIFY(pdfFile.open(QIODevice::WriteOnly));
        pdfFile.write("pdf content");
        pdfFile.close();
        QVERIFY(pdfFile.setPermissions(QFileDevice::ReadOwner));
        QVERIFY(QFileInfo::exists(previewPath));
        QVERIFY(QFileInfo::exists(pdfPath));
        UDriveFileBrowser::cleanupOrphanedPreviews();
        QVERIFY(!QFileInfo::exists(previewPath));
        QVERIFY(!QFileInfo::exists(pdfPath));
        QVERIFY(!QFileInfo::exists(stale.path()));
    }

    void orphanPreviewSweepPreservesForeignLinksAndActiveSessions()
    {
        UDriveFileBrowser::cleanupOrphanedPreviews();
        const QString root = Account::commonCacheDirectory() + QStringLiteral("/preview-sessions");
        QTemporaryDir outside;
        QTemporaryDir legacy(QDir::tempPath() + QStringLiteral("/udrive-preview-test-XXXXXX"));
        QTemporaryDir stale(root + QStringLiteral("/session-stale-XXXXXX"));
        QTemporaryDir active(root + QStringLiteral("/session-active-XXXXXX"));
        QVERIFY(outside.isValid() && legacy.isValid() && stale.isValid() && active.isValid());
        QFile sentinel(outside.filePath(QStringLiteral("keep.txt")));
        QVERIFY(sentinel.open(QIODevice::WriteOnly));
        QCOMPARE(sentinel.write("keep"), 4);
        sentinel.close();
        QVERIFY(sentinel.setPermissions(QFileDevice::ReadOwner));
        const auto permissions = sentinel.permissions();
        QLockFile lock(active.filePath(QStringLiteral("active.lock")));
        lock.setStaleLockTime(0);
        QVERIFY(lock.tryLock());
#ifndef Q_OS_WIN
        const QString rootLink = root + QStringLiteral("/session-link");
        QVERIFY(QFile::link(outside.path(), rootLink));
        QVERIFY(QFile::link(outside.path(), stale.filePath(QStringLiteral("directory-link"))));
        QVERIFY(QFile::link(sentinel.fileName(), stale.filePath(QStringLiteral("file-link"))));
        QVERIFY(QFile::link(outside.path(), legacy.filePath(QStringLiteral("directory-link"))));
#else
        const QString rootLink = root + QStringLiteral("/session-junction");
        auto junction = [&](const QString &path) {
            return QProcess::execute(QStringLiteral("cmd.exe"), {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
                QDir::toNativeSeparators(path), QDir::toNativeSeparators(outside.path())}) == 0;
        };
        QVERIFY(junction(rootLink));
        QVERIFY(junction(stale.filePath(QStringLiteral("directory-junction"))));
        QVERIFY(QFileInfo(rootLink).isJunction());
#endif
        UDriveFileBrowser::cleanupOrphanedPreviews();
        QVERIFY(QFileInfo::exists(sentinel.fileName()));
        QCOMPARE(sentinel.permissions(), permissions);
        QVERIFY(sentinel.open(QIODevice::ReadOnly));
        QCOMPARE(sentinel.readAll(), QByteArray("keep"));
        QVERIFY(QFileInfo::exists(legacy.path()));
        QVERIFY(QFileInfo::exists(active.path()));
        QVERIFY(!QFileInfo::exists(stale.path()));
#ifndef Q_OS_WIN
        QVERIFY(QFileInfo(rootLink).isSymLink());
        QVERIFY(QFile::remove(rootLink));
#else
        QVERIFY(QFileInfo(rootLink).isJunction());
        QVERIFY(QDir().rmdir(rootLink));
#endif
        lock.unlock();
        UDriveFileBrowser::cleanupOrphanedPreviews();
        QVERIFY(!QFileInfo::exists(active.path()));
    }

    void previewCachesArePrivateToEachAccount()
    {
        DriveServer server;
        QImage image(16, 16, QImage::Format_RGB32); image.fill(Qt::green);
        QByteArray png; QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly); QVERIFY(image.save(&buffer, "PNG"));
        server.uploaded.insert("/dav/personal/private.png", png);
        auto accountFor = [&] {
            auto *account = new Account(QUuid::createUuid(), "test", server.url());
            account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
            account->credentials()->fetchFromKeychain(); return account;
        };
        AccountState first(accountFor()), second(accountFor());
        UDriveFileBrowser firstBrowser(&first), secondBrowser(&second);
        QSignalSpy firstFetched(&firstBrowser, &UDriveFileBrowser::downloadFinished), secondFetched(&secondBrowser, &UDriveFileBrowser::downloadFinished);
        for (auto *browser : {&firstBrowser, &secondBrowser}) {
            auto *tree = browser->findChild<QTreeView *>("driveFiles");
            QTRY_VERIFY(select(tree, "private.png").isValid());
            QVERIFY(browser->previewSelectedFile());
        }
        QTRY_COMPARE(firstFetched.count(), 1); QTRY_COMPARE(secondFetched.count(), 1);
        const QString firstPath = firstFetched.first().at(1).toString();
        const QString secondPath = secondFetched.first().at(1).toString();
        QVERIFY(firstPath != secondPath);
        {
            UDriveFileBrowser anotherBrowser(&second);
            UDriveFileBrowser::cleanupOrphanedPreviews();
            QVERIFY(QFileInfo::exists(firstPath));
            QVERIFY(QFileInfo::exists(secondPath));
        }
        first.signOutByUi();
        QTRY_VERIFY(!QFileInfo::exists(firstPath));
        QVERIFY(QFileInfo::exists(secondPath));
        second.signOutByUi();
        QTRY_VERIFY(!QFileInfo::exists(secondPath));
    }

#ifdef UDRIVE_HAS_MULTIMEDIA
    void videoControlsStayAtBottom()
    {
        QStackedWidget host;
        host.setObjectName(QStringLiteral("stack"));
        host.setStyleSheet(QStringLiteral("QStackedWidget#stack QPushButton { min-height: 20px; padding: 6px 10px; border: 1px solid #444; }"));
        auto *preview = new UDriveFilePreview(&host);
        QSignalSpy ready(preview, &UDriveFilePreview::ready);
        host.addWidget(preview);
        host.resize(1250, 850);
        host.show();
        preview->openFile(QFINDTESTDATA("testfiles/udrive-preview.mp4"), QStringLiteral("Clip.mov"));
        auto *screen = preview->findChild<QVideoWidget *>("previewVideo");
        auto *controls = preview->findChild<QWidget *>("previewControlBar");
        auto *container = preview->findChild<QWidget *>("previewVideoContainer");
        auto *seek = preview->findChild<QSlider *>("previewSeek");
        auto *button = preview->findChild<QPushButton *>("previewFullScreen");
        auto *player = preview->findChild<QMediaPlayer *>("previewPlayer");
        QVERIFY(screen && controls && container && seek && button && player);
        QTRY_VERIFY_WITH_TIMEOUT(screen->videoSink()->videoFrame().isValid(), 10000);
        player->pause();
        QTRY_COMPARE(preview->findChild<QLabel *>("previewStatus")->text(), QStringLiteral("Video preview"));
        QCOMPARE(ready.count(), 1);
        QTRY_VERIFY(screen->height() > 100);
        QVERIFY(controls->height() <= 80);
        QCOMPARE(controls->geometry().bottom(), container->contentsRect().bottom());
        QVERIFY(screen->height() > controls->height());
        QVERIFY(button->mapTo(controls, QPoint()).y() - seek->mapTo(controls, QPoint()).y() < 40);
        host.resize(840, 640);
        QTRY_VERIFY(controls->height() <= 80);
        QCOMPARE(controls->geometry().bottom(), container->contentsRect().bottom());
        button->click();
        auto *window = preview->findChild<QWidget *>("previewFullScreenWindow");
        QTRY_VERIFY(window->isVisible());
        QVERIFY(controls->height() <= 80);
        button->click();
        QTRY_VERIFY(!window->isVisible());
        QCOMPARE(screen->parentWidget(), container);
        QCOMPARE(controls->parentWidget(), container);
        QVERIFY(screen->height() > controls->height());
        QCOMPARE(controls->geometry().bottom(), container->contentsRect().bottom());
        QCOMPARE(ready.count(), 1);
    }

    void videoFullScreenControls_data()
    {
        QTest::addColumn<QString>("exitMethod");
        for (const auto &method : {"button", "escape-video", "escape-controls", "f", "double-click", "close", "clear", "destroy"})
            QTest::newRow(method) << QString::fromLatin1(method);
    }

    void videoFullScreenControls()
    {
        QFETCH(QString, exitMethod);
        auto preview = std::make_unique<UDriveFilePreview>();
        preview->resize(840, 640);
        preview->show();
        preview->openFile(QFINDTESTDATA("testfiles/udrive-preview.mp4"), QStringLiteral("Clip.mp4"));
        auto *screen = preview->findChild<QVideoWidget *>("previewVideo");
        auto *player = preview->findChild<QMediaPlayer *>("previewPlayer");
        auto *controls = preview->findChild<QWidget *>("previewControlBar");
        auto *button = preview->findChild<QPushButton *>("previewFullScreen");
        auto *window = preview->findChild<QWidget *>("previewFullScreenWindow");
        QVERIFY(screen && player && controls && button && window);
        const auto source = player->source();
        QVERIFY(!source.isEmpty());
        button->click();
        QTRY_VERIFY(window->isVisible() && window->isFullScreen());
        QVERIFY(controls->isVisible());
        QVERIFY(button->isVisible());
        QCOMPARE(screen->window(), window);
        QCOMPARE(button->window(), window);
        QCOMPARE(button->accessibleName(), QStringLiteral("Exit full screen"));
        if (exitMethod.startsWith(QStringLiteral("escape")) || exitMethod == QStringLiteral("f")) {
            // Programmatic clicks don't activate the macOS application.
            // Establish Qt's active window for the synthetic shortcut events.
            QApplication::setActiveWindow(window);
        }

        if (exitMethod == QStringLiteral("button")) button->click();
        else if (exitMethod == QStringLiteral("escape-video")) {
            screen->setFocus();
            QTest::keyClick(screen, Qt::Key_Escape);
        } else if (exitMethod == QStringLiteral("escape-controls")) {
            button->setFocus();
            QTest::keyClick(button, Qt::Key_Escape);
        } else if (exitMethod == QStringLiteral("f")) {
            button->setFocus();
            QTest::keyClick(button, Qt::Key_F);
        } else if (exitMethod == QStringLiteral("double-click")) QTest::mouseDClick(screen, Qt::LeftButton);
        else if (exitMethod == QStringLiteral("close")) window->close();
        else if (exitMethod == QStringLiteral("clear")) preview->clear();
        else if (exitMethod == QStringLiteral("destroy")) {
            const QPointer<QWidget> guard(window);
            preview.reset();
            QVERIFY(!guard);
            return;
        }

        QTRY_VERIFY(!window->isVisible());
        QCOMPARE(screen->window(), preview.get());
        QCOMPARE(button->window(), preview.get());
        QCOMPARE(button->accessibleName(), QStringLiteral("Full screen"));
        if (exitMethod == QStringLiteral("clear")) QVERIFY(player->source().isEmpty());
        else {
            QVERIFY(controls->isVisible());
            QCOMPARE(player->source(), source);
        }
    }

    void uploadedVideoPlaysInsideUDrive()
    {
        // Original 32x32 solid-color H.264 fixture generated with AVAssetWriter.
        DriveServer server;
        const QByteArray video = contents(QFINDTESTDATA("testfiles/udrive-preview.mp4"));
        QVERIFY(!video.isEmpty());
        server.uploaded.insert("/dav/personal/Clip.mp4", video);
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveFileBrowser browser(&state); browser.resize(840, 640); browser.show();
        auto *tree = browser.findChild<QTreeView *>("driveFiles");
        QTRY_VERIFY(select(tree, "Clip.mp4").isValid());
        auto *preview = browser.findChild<UDriveFilePreview *>();
        auto *player = preview->findChild<QMediaPlayer *>("previewPlayer");
        auto *screen = preview->findChild<QVideoWidget *>("previewVideo");
        QVERIFY(player && screen);
        QSignalSpy frames(screen->videoSink(), &QVideoSink::videoFrameChanged);
        QSignalSpy fetched(&browser, &UDriveFileBrowser::downloadFinished);
        QVERIFY(browser.previewSelectedFile());
        QTRY_COMPARE(fetched.count(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(player->duration() > 0, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty(), 10000);
        QVERIFY(player->source().isLocalFile());
        auto *timeLabel = preview->findChild<QLabel *>("previewTime");
        auto *forward10 = preview->findChild<QPushButton *>("previewForward10");
        auto *rewind10 = preview->findChild<QPushButton *>("previewRewind10");
        auto *fullScreenBtn = preview->findChild<QPushButton *>("previewFullScreen");
        QVERIFY(timeLabel && forward10 && rewind10 && fullScreenBtn);
        QVERIFY(timeLabel->text().contains('/'));
        forward10->click();
        rewind10->click();
        QVERIFY(!browser.findChild<QFileDialog *>());
        preview->findChild<QPushButton *>("previewBack")->click();
        QVERIFY(player->source().isEmpty());
        QVERIFY(browser.previewSelectedFile());
        QTRY_VERIFY(player->duration() > 0);
        QCOMPARE(fetched.count(), 1);
        fullScreenBtn->click();
        auto *fullScreenWindow = preview->findChild<QWidget *>("previewFullScreenWindow");
        QTRY_VERIFY(fullScreenWindow->isVisible());
        fullScreenBtn->click();
        QTRY_VERIFY(!fullScreenWindow->isVisible());
        frames.clear();
        player->setPosition(0);
        player->play();
        QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty(), 10000);
        fullScreenBtn->click();
        QTRY_VERIFY(fullScreenWindow->isVisible());
        browser.hide();
        QVERIFY(!fullScreenWindow->isVisible());
        QVERIFY(player->source().isEmpty());
    }
#endif

    void failedOrCancelledDownloadsDoNotOpenFiles()
    {
        DriveServer server;
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveFileBrowser browser(&state);
        auto *tree = browser.findChild<QTreeView *>("driveFiles");
        QTRY_COMPARE_WITH_TIMEOUT(tree->model()->rowCount(), 7, 5000);
        QTemporaryDir files;
        QVERIFY(files.isValid());
        const QString destination = files.filePath("saved.txt");
        FileOpenReceiver receiver;
        QDesktopServices::setUrlHandler("file", &receiver, "record");
        const auto resetHandler = qScopeGuard([] { QDesktopServices::unsetUrlHandler("file"); });
        QSignalSpy downloaded(&browser, &UDriveFileBrowser::downloadFinished);
        for (const auto &name : {"denied.txt", "truncated.txt"}) {
            QVERIFY(select(tree, name).isValid());
            QVERIFY(browser.saveSelectedFile(destination, true));
            QTRY_COMPARE(downloaded.count(), 1);
            QVERIFY(!downloaded.takeFirst().at(0).toBool());
            QVERIFY(receiver.opened.isEmpty());
            QVERIFY(!QFileInfo::exists(destination));
        }
        QVERIFY(select(tree, "slow.txt").isValid());
        QVERIFY(browser.saveSelectedFile(destination, true));
        QTRY_VERIFY(server.slowStarted);
        browser.findChild<QPushButton *>("driveCancelDownload")->click();
        QCOMPARE(downloaded.count(), 1);
        QVERIFY(!downloaded.takeFirst().at(0).toBool());
        QVERIFY(receiver.opened.isEmpty());
        QVERIFY(!QFileInfo::exists(destination));
        QVERIFY(select(tree, "a&b.txt").isValid());
        QVERIFY(browser.saveSelectedFile(destination)); // Download alone must not launch an application.
        QTRY_COMPARE(downloaded.count(), 1);
        QVERIFY(downloaded.first().at(0).toBool());
        QVERIFY(receiver.opened.isEmpty());
    }

    void browseAndDownload()
    {
        DriveServer server;
        QVERIFY(server.isListening());
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        // Load spaces without starting the account's connectivity validator.
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        struct FolderCleanup {
            FolderMan *folders;
            ~FolderCleanup() { folders->unloadAndDeleteAllFolders(); }
        } cleanup {_folders.get()};
        UDriveFileBrowser browser(&state);
        browser.resize(840, 640);
        browser.show();
        auto *tree = browser.findChild<QTreeView *>("driveFiles");
        auto *search = browser.findChild<QLineEdit *>("driveSearch");
        auto *rootButton = browser.findChild<QPushButton *>("driveRoot");
        QTRY_COMPARE_WITH_TIMEOUT(tree->model()->rowCount(), 7, 5000);
        QVERIFY(server.authenticated);
        QVERIFY(select(tree, "%2F.txt").isValid());
        QVERIFY(!select(tree, "escape").isValid());
        QCOMPARE(tree->model()->index(0, 0).data().toString(), QStringLiteral("Docs"));
        QVERIFY(tree->model()->index(2, 2).data().toString() != QStringLiteral("—"));
        QCOMPARE(select(tree, "denied.txt").siblingAtColumn(2).data().toString(), QStringLiteral("—"));
        search->setText("a&");
        QCOMPARE(tree->model()->rowCount(), 1);
        QCOMPARE(tree->model()->index(0, 0).data().toString(), QStringLiteral("a&b.txt"));
        search->clear();
        auto docs = select(tree, "Docs");
        QVERIFY(docs.isValid());
        Q_EMIT tree->activated(docs);
        QTRY_COMPARE(tree->model()->rowCount(), 1);
        QCOMPARE(tree->model()->index(0, 0).data().toString(), QStringLiteral("Budget 2026.pdf"));
        rootButton->click();
        QTRY_COMPARE(tree->model()->rowCount(), 7);

        auto slowFolder = select(tree, "slowfolder");
        Q_EMIT tree->activated(slowFolder);
        QTRY_VERIFY(server.requests.contains({"PROPFIND", "/dav/personal/slowfolder"}));
        rootButton->click();
        QTRY_COMPARE(tree->model()->rowCount(), 7);
        QTest::qWait(200);
        QCOMPARE(tree->model()->rowCount(), 7);

        QTemporaryDir downloads;
        QVERIFY(downloads.isValid());
        QSignalSpy finished(&browser, &UDriveFileBrowser::downloadFinished);
        QVERIFY(select(tree, "a&b.txt").isValid());
        const QString destination = downloads.path() + "/saved.txt";
        QVERIFY(browser.saveSelectedFile(destination));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QVERIFY(finished.takeFirst().at(0).toBool());
        QCOMPARE(contents(destination), server.fileContent);
        QVERIFY(server.requests.contains({"GET", "/dav/personal/a&b.txt"}));
        QVERIFY(select(tree, "%2F.txt").isValid());
        QVERIFY(browser.saveSelectedFile(destination));
        QTRY_COMPARE(finished.count(), 1);
        QVERIFY(finished.takeFirst().at(0).toBool());
        QVERIFY(server.requests.contains({"GET", "/dav/personal/%2F.txt"}));
        for (const auto &name : {"denied.txt", "truncated.txt"}) {
            QVERIFY(select(tree, name).isValid());
            QVERIFY(browser.saveSelectedFile(destination));
            QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
            QVERIFY(!finished.takeFirst().at(0).toBool());
            QCOMPARE(contents(destination), server.fileContent);
        }
        QVERIFY(select(tree, "slow.txt").isValid());
        QVERIFY(browser.saveSelectedFile(destination));
        QTRY_VERIFY(server.slowStarted);
        QTest::qWait(50);
        browser.findChild<QPushButton *>("driveCancelDownload")->click();
        QCOMPARE(finished.count(), 1);
        QVERIFY(!finished.takeFirst().at(0).toBool());
        QCOMPARE(contents(destination), server.fileContent);
        QVERIFY(select(tree, "a&b.txt").isValid());
        QVERIFY(browser.saveSelectedFile(downloads.path() + "/missing/destination.txt"));
        QTRY_COMPARE(finished.count(), 1);
        QVERIFY(!finished.takeFirst().at(0).toBool());
        QVERIFY(!QFileInfo::exists(downloads.path() + "/missing/destination.txt"));
        browser.showWorkspaces();
        QTRY_COMPARE(tree->model()->rowCount(), 1);
        QCOMPARE(tree->model()->index(0, 0).data().toString(), QStringLiteral("Roadmap.pdf"));
        browser.showPersonal();
        QTRY_COMPARE(tree->model()->rowCount(), 7);
        QTemporaryDir synced;
        const QString syncedFile = synced.path() + "/a&b.txt";
        {
            QFile file(syncedFile);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("local synced file");
        }
        FolderDefinition definition(server.url("/dav/personal"), "personal", "Personal");
        definition.setLocalPath(synced.path());
        definition.setTargetPath("");
        definition.setJournalPath(".test-sync.db");
        definition.setPaused(true);
        auto *folder = _folders->addFolder(&state, definition);
        QVERIFY(folder);
        QVERIFY(!folder->hasSetupError());
        QVERIFY(select(tree, "a&b.txt").isValid());
        QVERIFY(browser.findChild<QPushButton *>("driveOpen")->isEnabled());
        FileOpenReceiver receiver;
        QDesktopServices::setUrlHandler("file", &receiver, "record");
        browser.findChild<QPushButton *>("driveOpen")->click();
        QDesktopServices::unsetUrlHandler("file");
        QTRY_COMPARE(browser.findChild<QPlainTextEdit *>("previewText")->toPlainText(), QStringLiteral("local synced file"));
        QVERIFY(receiver.opened.isEmpty());
        browser.findChild<QPushButton *>("previewBack")->click();
        // A symlink to a file outside the sync root must not be opened as synced.
        QVERIFY(QFile::link(destination, synced.path() + "/denied.txt"));
        QVERIFY(select(tree, "denied.txt").isValid());
        QVERIFY(browser.findChild<QPushButton *>("driveOpen")->isEnabled());
        QCOMPARE(browser.findChild<QPushButton *>("driveOpen")->text(), QStringLiteral("Preview"));
        receiver.opened.clear();
        QDesktopServices::setUrlHandler("file", &receiver, "record");
        const auto resetHandler = qScopeGuard([] { QDesktopServices::unsetUrlHandler("file"); });
        finished.clear();
        QVERIFY(browser.saveSelectedFile(destination, true));
        QTRY_COMPARE(finished.count(), 1);
        QVERIFY(!finished.first().at(0).toBool());
        QVERIFY(receiver.opened.isEmpty()); // Never use the symlink's external target as a synced file.
        QVERIFY(server.authenticated);
    }


    void favoritesHrefCompatibility_data()
    {
        QTest::addColumn<QString>("href");
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("shown");
        QTest::addColumn<bool>("spaceEndpoints");
        QTest::newRow("canonical") << QStringLiteral("/dav/personal/a&b.txt") << QStringLiteral("a&b.txt") << true << false;
        QTest::newRow("remote-php-files") << QStringLiteral("/remote.php/dav/files/test/a&b.txt") << QStringLiteral("a&b.txt") << true << false;
        QTest::newRow("dav-files-alias") << QStringLiteral("/dav/files/test/a&b.txt") << QStringLiteral("a&b.txt") << true << false;
        QTest::newRow("absolute-dav-files") << QStringLiteral("absolute:/dav/files/test/a&b.txt") << QStringLiteral("a&b.txt") << true << false;
        QTest::newRow("remote-php-root") << QStringLiteral("/remote.php/dav/personal/a&b.txt") << QStringLiteral("a&b.txt") << true << false;
        QTest::newRow("literal-characters") << QStringLiteral("/dav/files/test/percent%#?.txt") << QStringLiteral("percent%#?.txt") << true << false;
        QTest::newRow("other-user") << QStringLiteral("/dav/files/other/a&b.txt") << QStringLiteral("a&b.txt") << false << false;
        QTest::newRow("outside-account") << QStringLiteral("foreign:/dav/files/test/a&b.txt") << QStringLiteral("a&b.txt") << false << false;
        QTest::newRow("traversal") << QStringLiteral("/dav/files/test/../a&b.txt") << QStringLiteral("a&b.txt") << false << false;
        QTest::newRow("unknown-space") << QStringLiteral("/dav/unknown/a&b.txt") << QStringLiteral("a&b.txt") << false << false;
        QTest::newRow("spaces-remote-php") << QStringLiteral("/remote.php/dav/spaces/personal/a&b.txt") << QStringLiteral("a&b.txt") << true << true;
        QTest::newRow("cs3-reference") << QStringLiteral("/remote.php/dav/personal/a&b.txt") << QStringLiteral("a&b.txt") << true << true;
    }

    void favoritesHrefCompatibility()
    {
        QFETCH(QString, href); QFETCH(QString, name); QFETCH(bool, shown); QFETCH(bool, spaceEndpoints);
        DriveServer server;
        if (spaceEndpoints) server.graphDavPrefix = QStringLiteral("/dav/spaces");
        const QString path = server.graphDavPrefix + QStringLiteral("/personal/") + name;
        if (name != QStringLiteral("a&b.txt")) server.uploaded.insert(path, "fixture");
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account); UDriveFileBrowser browser(&state); browser.show();
        auto *tree = browser.findChild<QTreeView *>("driveFiles");
        QTRY_VERIFY(select(tree, name).isValid());
        QSignalSpy completed(&browser, &UDriveFileBrowser::operationFinished);
        QVERIFY(browser.setSelectedFavorite(true)); QTRY_COMPARE(completed.count(), 1);
        QVERIFY(completed.first().at(0).toBool()); QVERIFY(server.favorites.contains(path));
        QTRY_VERIFY(select(tree, name).isValid());
        QCOMPARE(browser.findChild<QAction *>("driveFavorite")->text(), QStringLiteral("Remove from Favorites"));
        QUrl reported;
        if (href.startsWith("absolute:")) { reported = server.url(); href.remove(0, 9); }
        else if (href.startsWith("foreign:")) { reported = QUrl("https://unrelated.example"); href.remove(0, 8); }
        reported.setPath(href);
        QByteArray entry = server.entry(path, false);
        if (spaceEndpoints) entry.replace("</d:prop>", "<oc:spaceid>personal</oc:spaceid></d:prop>");
        const auto start = entry.indexOf("<d:href>") + 8;
        const auto end = entry.indexOf("</d:href>", start);
        entry.replace(start, end - start, reported.toString(QUrl::FullyEncoded).toHtmlEscaped().toUtf8());
        server.favoritesReport = "<d:multistatus xmlns:d=\"DAV:\">" + entry + "</d:multistatus>";
        browser.showFavorites();
        QTRY_VERIFY(!browser.findChild<QLabel *>("driveStatus")->text().startsWith(QStringLiteral("Loading")));
        QCOMPARE(tree->model()->rowCount(), shown ? 1 : 0);
        if (shown) {
            QVERIFY(select(tree, name).isValid());
            QTemporaryDir downloads;
            QSignalSpy saved(&browser, &UDriveFileBrowser::downloadFinished);
            QVERIFY(browser.saveSelectedFile(downloads.filePath("favorite.txt")));
            QTRY_COMPARE(saved.count(), 1); QVERIFY(saved.first().at(0).toBool());
            QVERIFY(server.requests.contains(qMakePair(QByteArray("GET"), path)));
        }
    }

    void trashSelectorExcludesShares()
    {
        DriveServer server(true);
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveFileBrowser browser(&state);
        auto *tree = browser.findChild<QTreeView *>("driveFiles");
        auto *spaces = browser.findChild<QComboBox *>("driveSpaces");
        QTRY_COMPARE(tree->model()->rowCount(), 7);

        browser.showShares();
        QCOMPARE(spaces->currentText(), QStringLiteral("Shares"));
        browser.showTrash();
        QCOMPARE(spaces->count(), 2);
        QCOMPARE(spaces->findText("Shares"), -1);
        QVERIFY(spaces->findText("Personal") >= 0);
        QVERIFY(spaces->findText("Team files") >= 0);
        QTRY_VERIFY(server.requests.contains({"PROPFIND", "/dav/spaces/trash-bin/personal"}));
        spaces->setCurrentIndex(spaces->findText("Team files"));
        QTRY_VERIFY(server.requests.contains({"PROPFIND", "/dav/spaces/trash-bin/team"}));
        for (const auto &request : server.requests) {
            if (request.first == "PROPFIND" && request.second.startsWith("/dav/spaces/trash-bin/")) {
                QVERIFY(request.second == "/dav/spaces/trash-bin/personal" || request.second == "/dav/spaces/trash-bin/team");
            }
        }
        browser.showShares();
        QCOMPARE(spaces->currentText(), QStringLiteral("Shares"));
    }

    void manageFilesFavoritesAndTrash()
    {
        DriveServer server(true);
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveFileBrowser browser(&state);
        auto *tree = browser.findChild<QTreeView *>("driveFiles");
        QTRY_COMPARE_WITH_TIMEOUT(tree->model()->rowCount(), 7, 5000);
        QSignalSpy operations(&browser, &UDriveFileBrowser::operationFinished);
        auto finish = [&](bool success) {
            QElapsedTimer timer; timer.start();
            while (operations.isEmpty() && timer.elapsed() < 5000) QTest::qWait(10);
            return operations.count() == 1 && operations.takeFirst().at(0).toBool() == success;
        };
        const int requestCount = server.requests.size();
        QVERIFY(!browser.createFolder("../escape"));
        QVERIFY(!browser.createFolder("evil\r\nheader"));
        QCOMPARE(server.requests.size(), requestCount);
        QVERIFY(browser.createFolder("Uploads & docs"));
        QVERIFY(finish(true));
        QTRY_VERIFY(select(tree, "Uploads & docs").isValid());
        QVERIFY(server.folders.contains("/dav/personal/Uploads & docs"));

        QTemporaryDir local;
        const QString localFile = local.path() + "/draft %.txt";
        const QByteArray payload(1024 * 128, 'P');
        { QFile file(localFile); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(payload), payload.size()); }
        QVERIFY(browser.uploadFile(localFile));
        QVERIFY(finish(true));
        QTRY_VERIFY(select(tree, "draft %.txt").isValid());
        QCOMPARE(server.uploaded.value("/dav/personal/draft %.txt"), payload);
        QCOMPARE(server.requestHeaders.value("PUT /dav/personal/draft %.txt").value("if-none-match"), QByteArray("*"));
        QVERIFY(browser.uploadFile(localFile));
        QVERIFY(finish(false));
        QCOMPARE(server.uploaded.value("/dav/personal/draft %.txt"), payload);
        QVERIFY(select(tree, "draft %.txt").isValid());
        QVERIFY(!browser.renameSelected("../bad"));
        QVERIFY(browser.renameSelected("Final & draft.txt"));
        QVERIFY(finish(true));
        QTRY_VERIFY(select(tree, "Final & draft.txt").isValid());
        const auto renameHeaders = server.requestHeaders.value("MOVE /dav/personal/draft %.txt");
        QCOMPARE(renameHeaders.value("overwrite"), QByteArray("F"));
        QCOMPARE(renameHeaders.value("if-match"), QByteArray("\"tag\""));
        QCOMPARE(QUrl::fromEncoded(renameHeaders.value("destination")).path(), QStringLiteral("/dav/personal/Final & draft.txt"));
        QVERIFY(browser.copySelected("Copy.txt"));
        QVERIFY(finish(true));
        QTRY_VERIFY(select(tree, "Copy.txt").isValid());
        QVERIFY(browser.moveSelected("Uploads & docs"));
        QVERIFY(finish(true));
        QTRY_VERIFY(select(tree, "Final & draft.txt").isValid());
        QVERIFY(!select(tree, "Copy.txt").isValid());
        QVERIFY(server.uploaded.contains("/dav/personal/Uploads & docs/Copy.txt"));
        QVERIFY(select(tree, "Final & draft.txt").isValid());
        QVERIFY(browser.renameSelected("a&b.txt"));
        QVERIFY(finish(false));
        QVERIFY(server.uploaded.contains("/dav/personal/Final & draft.txt"));
        QVERIFY(select(tree, "Final & draft.txt").isValid());
        QVERIFY(browser.setSelectedFavorite(true));
        QVERIFY(finish(true));
        QTRY_VERIFY(select(tree, "Final & draft.txt").isValid());
        QVERIFY(server.favorites.contains("/dav/personal/Final & draft.txt"));
        server.favorites.insert("/dav/team/Roadmap.pdf");
        browser.showFavorites();
        QTRY_COMPARE(tree->model()->rowCount(), 2);
        QVERIFY(select(tree, "Roadmap.pdf").isValid());
        QCOMPARE(tree->currentIndex().siblingAtColumn(3).data().toString(), QStringLiteral("Team files"));
        QSignalSpy favoriteDownload(&browser, &UDriveFileBrowser::downloadFinished);
        QVERIFY(browser.saveSelectedFile(local.path() + "/roadmap.pdf"));
        QVERIFY(favoriteDownload.wait(5000));
        QVERIFY(favoriteDownload.takeFirst().at(0).toBool());
        QVERIFY(server.requests.contains({"GET", "/dav/team/Roadmap.pdf"}));
        QVERIFY(select(tree, "Final & draft.txt").isValid());
        server.rejectFavorite = true;
        QVERIFY(browser.setSelectedFavorite(false));
        QVERIFY(finish(false));
        QVERIFY(server.favorites.contains("/dav/personal/Final & draft.txt"));
        server.rejectFavorite = false;
        QVERIFY(browser.setSelectedFavorite(false));
        QVERIFY(finish(true));
        QTRY_COMPARE(tree->model()->rowCount(), 1);

        browser.showPersonal();
        QTRY_VERIFY(select(tree, "Uploads & docs").isValid());
        QVERIFY(browser.setSelectedFavorite(true));
        QVERIFY(finish(true));
        QTRY_VERIFY(select(tree, "Uploads & docs").isValid());
        browser.showFavorites();
        QTRY_VERIFY(select(tree, "Uploads & docs").isValid());
        Q_EMIT tree->activated(tree->currentIndex());
        QTRY_VERIFY(select(tree, "Copy.txt").isValid());
        browser.findChild<QPushButton *>("driveRoot")->click();
        QTRY_VERIFY(select(tree, "Final & draft.txt").isValid());
        QVERIFY(browser.deleteSelected());
        QVERIFY(finish(true));
        QTRY_VERIFY(!select(tree, "Final & draft.txt").isValid());
        browser.showTrash();
        auto *spaces = browser.findChild<QComboBox *>("driveSpaces");
        spaces->setCurrentIndex(spaces->findText("Personal"));
        QTRY_COMPARE(tree->model()->rowCount(), 1);
        QVERIFY(select(tree, "Final & draft.txt").isValid());
        auto *restoreBtn = browser.findChild<QPushButton *>("driveRestore");
        QVERIFY(restoreBtn && !restoreBtn->isHidden() && restoreBtn->isEnabled());
        auto *openBtn = browser.findChild<QPushButton *>("driveOpen");
        QVERIFY(openBtn && openBtn->isHidden());
        auto *shareBtn = browser.findChild<QPushButton *>("driveShare");
        QVERIFY(shareBtn && shareBtn->isHidden());
        QVERIFY(!browser.saveSelectedFile(local.path() + "/trash.txt"));
        server.uploaded.insert("/dav/personal/Final & draft.txt", "new existing file");
        server.removed.remove("/dav/personal/Final & draft.txt");
        QVERIFY(browser.restoreSelected());
        QVERIFY(finish(false));
        QCOMPARE(server.uploaded.value("/dav/personal/Final & draft.txt"), QByteArray("new existing file"));
        QVERIFY(server.trash.contains("deleted-1"));
        server.uploaded.remove("/dav/personal/Final & draft.txt");
        server.removed.insert("/dav/personal/Final & draft.txt");
        QVERIFY(browser.restoreSelected());
        QVERIFY(finish(true));
        QTRY_COMPARE(tree->model()->rowCount(), 0);
        QCOMPARE(server.requestHeaders.value("MOVE /dav/spaces/trash-bin/personal/deleted-1").value("overwrite"), QByteArray("F"));
        browser.showPersonal();
        QVERIFY(restoreBtn && restoreBtn->isHidden());
        QTRY_VERIFY(select(tree, "Final & draft.txt").isValid());
        QVERIFY(shareBtn && !shareBtn->isHidden() && shareBtn->isEnabled());
        auto *shareAct = browser.findChild<QAction *>("driveShareAction");
        QVERIFY(shareAct && shareAct->isEnabled());
        const QString pendingUpload = local.path() + "/pending upload.txt";
        { QFile file(pendingUpload); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("pending upload"); }
        server.holdUpload = true;
        QVERIFY(browser.uploadFile(pendingUpload));
        QTRY_VERIFY(server.requests.contains({"PUT", "/dav/personal/pending upload.txt"}));
        browser.findChild<QPushButton *>("driveCancelDownload")->click();
        QVERIFY(finish(false));
        QVERIFY(!server.uploaded.contains("/dav/personal/pending upload.txt"));
        server.holdUpload = false;
        browser.showShares();
        browser.findChild<QAction *>(QStringLiteral("driveSharedWithMe"))->trigger();
        QTRY_COMPARE(tree->model()->rowCount(), 1);
        QCOMPARE(tree->model()->index(0, 0).data().toString(), QStringLiteral("Shared document.pdf"));
        QVERIFY(server.authenticated);
    }

    void uploadDurationFormatting()
    {
        QCOMPARE(UDriveFileBrowser::uploadDuration(-1000), QStringLiteral("00:00"));
        QCOMPARE(UDriveFileBrowser::uploadDuration(0), QStringLiteral("00:00"));
        QCOMPARE(UDriveFileBrowser::uploadDuration(500), QStringLiteral("00:00"));
        QCOMPARE(UDriveFileBrowser::uploadDuration(1000), QStringLiteral("00:01"));
        QCOMPARE(UDriveFileBrowser::uploadDuration(65000), QStringLiteral("01:05"));
        QCOMPARE(UDriveFileBrowser::uploadDuration(3665000), QStringLiteral("1:01:05"));
    }

    void uploadProgressDetails_data()
    {
        QTest::addColumn<QString>("mode");
        for (const auto *mode : {"put", "empty", "tus-data", "tus-no-data", "tus-patch"})
            QTest::newRow(mode) << QString::fromLatin1(mode);
    }

    void uploadProgressDetails()
    {
        QFETCH(QString, mode);
        const bool tus = mode.startsWith("tus");
        DriveServer server;
        server.holdUpload = mode != "tus-patch";
        server.holdTusPatch = mode == "tus-patch";
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        if (tus) {
            QVariantMap support{{QStringLiteral("version"), QStringLiteral("1.0.0")},
                {QStringLiteral("resumable"), QStringLiteral("1.0.0")},
                {QStringLiteral("extension"), mode == "tus-no-data" ? QStringLiteral("creation") : QStringLiteral("creation,creation-with-upload")},
                {QStringLiteral("max_chunk_size"), 5ULL * 1024 * 1024}};
            account->setCapabilities(Capabilities(server.url(), {
                {QStringLiteral("spaces"), QVariantMap{{QStringLiteral("enabled"), true}, {QStringLiteral("version"), QStringLiteral("1.0.0")}}},
                {QStringLiteral("files"), QVariantMap{{QStringLiteral("tus_support"), support}}}}));
        }
        AccountState state(account);
        UDriveFileBrowser browser(&state); browser.resize(1000, 650); browser.show();
        auto *tree = browser.findChild<QTreeView *>("driveFiles");
        QTRY_COMPARE(tree->model()->rowCount(), 7);
        auto *details = browser.findChild<QLabel *>("driveUploadDetails");
        auto *progress = browser.findChild<QProgressBar *>("driveTransferProgress");
        auto *timer = browser.findChild<QTimer *>("driveUploadProgressTimer");
        QVERIFY(details && progress && timer);
        QVERIFY(details->isHidden());
        QTemporaryDir files;
        QFile file(files.filePath("presentation.pptx"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray payload(mode == "empty" ? 0 : tus ? 12 * 1024 * 1024 : 1024 * 1024, 'U');
        QCOMPARE(file.write(payload), payload.size()); file.close();
        QSignalSpy finished(&browser, &UDriveFileBrowser::operationFinished);
        QVERIFY(browser.uploadFile(file.fileName()));
        QVERIFY(details->isVisible() && timer->isActive());
        QVERIFY(details->text().contains("presentation.pptx"));
        QVERIFY(details->text().contains("00:00 elapsed"));
        QCOMPARE(progress->value(), mode == "empty" ? 100 : 0);
        QTRY_VERIFY(server.requests.contains({tus ? QByteArray("POST") : QByteArray("PUT"),
            tus ? QStringLiteral("/dav/personal") : QStringLiteral("/dav/personal/presentation.pptx")}));
        if (mode == "tus-patch") QTRY_VERIFY(server.requests.contains({"PATCH", "/tus-session-1"}));
        const int expected = mode == "tus-no-data" ? 0 : mode == "tus-data" ? 41 : mode == "tus-patch" ? 83 : 100;
        QTRY_COMPARE(progress->value(), expected);
        QVERIFY(details->text().contains(QStringLiteral("%1%").arg(expected)));
        QVERIFY(details->text().contains(" of "));
        QTest::qWait(1100);
        QVERIFY(details->text().contains("elapsed"));
        if (expected == 100) QVERIFY(details->text().contains("Finalizing"));
        else if (expected == 0) QVERIFY(details->text().contains("Estimating time remaining"));
        else {
            QVERIFY(details->text().contains("About "));
            QVERIFY(details->text().contains("remaining"));
            QVERIFY(details->text().contains("/s"));
        }
        if (mode == "tus-data" && qEnvironmentVariableIsSet("UDRIVE_UI_TEST_CAPTURE"))
            QVERIFY(browser.grab().save(QStringLiteral("/tmp/udrive-upload-progress.png")));
        const QString before = details->text();
        QTRY_VERIFY_WITH_TIMEOUT(details->text() != before, 2500); // Elapsed time updates without a network event.
        browser.findChild<QPushButton *>("driveCancelDownload")->click();
        QTRY_COMPARE(finished.count(), 1);
        QVERIFY(!finished.first().at(0).toBool());
        QVERIFY(details->isHidden() && details->text().isEmpty() && !timer->isActive());
        // A new upload starts at zero, and success removes its timing display.
        server.holdUpload = false; server.holdTusPatch = false;
        QTRY_COMPARE(tree->model()->rowCount(), 7);
        QFile next(files.filePath("next.txt")); QVERIFY(next.open(QIODevice::WriteOnly)); next.write("next upload"); next.close();
        QVERIFY(browser.uploadFile(next.fileName()));
        QVERIFY(details->text().contains("next.txt"));
        QVERIFY(details->text().contains("0%"));
        QVERIFY(details->text().contains("00:00 elapsed"));
        QVERIFY(details->text().contains("Estimating time remaining"));
        QTRY_COMPARE(finished.count(), 2);
        QVERIFY(finished.last().at(0).toBool());
        QVERIFY(details->isHidden() && details->text().isEmpty() && !timer->isActive());
    }

    void uploadLargeFileViaTus()
    {
        DriveServer server(true);
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();

        QVariantMap capMap;
        QVariantMap spacesMap;
        spacesMap[QStringLiteral("enabled")] = true;
        spacesMap[QStringLiteral("version")] = QStringLiteral("1.0.0");
        capMap[QStringLiteral("spaces")] = spacesMap;

        QVariantMap filesMap;
        QVariantMap tusMap;
        tusMap[QStringLiteral("version")] = QStringLiteral("1.0.0");
        tusMap[QStringLiteral("resumable")] = QStringLiteral("1.0.0");
        tusMap[QStringLiteral("extension")] = QStringLiteral("creation,creation-with-upload");
        tusMap[QStringLiteral("max_chunk_size")] = 5ULL * 1024 * 1024;
        filesMap[QStringLiteral("tus_support")] = tusMap;
        capMap[QStringLiteral("files")] = filesMap;
        account->setCapabilities(Capabilities(server.url(), capMap));

        AccountState state(account);
        UDriveFileBrowser browser(&state);
        auto *tree = browser.findChild<QTreeView *>("driveFiles");
        QTRY_COMPARE_WITH_TIMEOUT(tree->model()->rowCount(), 7, 5000);
        QSignalSpy operations(&browser, &UDriveFileBrowser::operationFinished);
        auto finish = [&](bool success) {
            QElapsedTimer timer; timer.start();
            while (operations.isEmpty() && timer.elapsed() < 10000) QTest::qWait(10);
            return operations.count() == 1 && operations.takeFirst().at(0).toBool() == success;
        };

        QTemporaryDir local;
        const QString localFile = local.filePath("presentation.pptx");
        const QByteArray payload(12 * 1024 * 1024, 'U');
        {
            QFile file(localFile);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(payload), payload.size());
        }

        QVERIFY(browser.uploadFile(localFile));
        QVERIFY(finish(true));
        QCOMPARE(server.uploaded.value("/dav/personal/presentation.pptx").size(), payload.size());
        QCOMPARE(server.uploaded.value("/dav/personal/presentation.pptx"), payload);
    }

    void cancelTusUploadStopsTransferAndReenablesActions()
    {
        DriveServer server(true);
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();

        QVariantMap capMap;
        QVariantMap spacesMap;
        spacesMap[QStringLiteral("enabled")] = true;
        spacesMap[QStringLiteral("version")] = QStringLiteral("1.0.0");
        capMap[QStringLiteral("spaces")] = spacesMap;

        QVariantMap filesMap;
        QVariantMap tusMap;
        tusMap[QStringLiteral("version")] = QStringLiteral("1.0.0");
        tusMap[QStringLiteral("resumable")] = QStringLiteral("1.0.0");
        tusMap[QStringLiteral("extension")] = QStringLiteral("creation,creation-with-upload");
        tusMap[QStringLiteral("max_chunk_size")] = 5ULL * 1024 * 1024;
        filesMap[QStringLiteral("tus_support")] = tusMap;
        capMap[QStringLiteral("files")] = filesMap;
        account->setCapabilities(Capabilities(server.url(), capMap));

        AccountState state(account);
        UDriveFileBrowser browser(&state);
        auto *tree = browser.findChild<QTreeView *>("driveFiles");
        QTRY_COMPARE_WITH_TIMEOUT(tree->model()->rowCount(), 7, 5000);
        QSignalSpy operations(&browser, &UDriveFileBrowser::operationFinished);
        auto finish = [&](bool success) {
            QElapsedTimer timer; timer.start();
            while (operations.isEmpty() && timer.elapsed() < 5000) QTest::qWait(10);
            return operations.count() == 1 && operations.takeFirst().at(0).toBool() == success;
        };

        QTemporaryDir local;
        const QString localFile = local.filePath("cancelled.pptx");
        const QByteArray payload(12 * 1024 * 1024, 'X');
        {
            QFile file(localFile);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(payload), payload.size());
        }

        server.holdUpload = true;
        QVERIFY(browser.uploadFile(localFile));
        QTRY_VERIFY(server.requests.contains({"POST", "/dav/personal"}));
        auto *cancelBtn = browser.findChild<QPushButton *>("driveCancelDownload");
        QVERIFY(cancelBtn);
        cancelBtn->click();
        QVERIFY(finish(false));
        QVERIFY(!server.uploaded.contains("/dav/personal/cancelled.pptx"));
        server.holdUpload = false;
        browser.showShares();
        browser.findChild<QAction *>(QStringLiteral("driveSharedWithMe"))->trigger();
        QTRY_COMPARE(tree->model()->rowCount(), 1);
        QCOMPARE(tree->model()->index(0, 0).data().toString(), QStringLiteral("Shared document.pdf"));
    }

    void mutationMultistatus_data()
    {
        QTest::addColumn<QByteArray>("verb");
        QTest::addColumn<QByteArray>("response");
        QTest::addColumn<bool>("success");
        const QByteArray start = "<d:multistatus xmlns:d=\"DAV:\"><d:response><d:href>/dav/personal/Docs/child</d:href>";
        const QByteArray end = "</d:response></d:multistatus>";
        const QByteArray failed = start + "<d:status>HTTP/1.1 403 Forbidden</d:status>" + end;
        for (const auto &verb : {QByteArray("DELETE"), QByteArray("COPY"), QByteArray("MOVE")}) {
            QTest::newRow(verb.constData()) << verb << failed << false;
        }
        QTest::newRow("mixed-child-statuses") << QByteArray("DELETE")
            << start + "<d:status>HTTP/1.1 204 No Content</d:status></d:response><d:response>"
                "<d:href>/dav/personal/Docs/blocked</d:href><d:status>HTTP/1.1 423 Locked</d:status>" + end << false;
        QTest::newRow("successful-multistatus") << QByteArray("COPY")
            << start + "<d:status>HTTP/1.1 201 Created</d:status>" + end << true;
        QTest::newRow("malformed-xml") << QByteArray("DELETE") << start + "<d:status>HTTP/1.1 200 OK</d:status>" << false;
        QTest::newRow("missing-status") << QByteArray("DELETE") << start + end << false;
        QTest::newRow("wrong-root") << QByteArray("DELETE")
            << QByteArray("<d:error xmlns:d=\"DAV:\"><d:status>HTTP/1.1 200 OK</d:status></d:error>") << false;
        QTest::newRow("invalid-status") << QByteArray("DELETE")
            << start + "<d:status>not-http 200 OK</d:status>" + end << false;
        QTest::newRow("favorite-property-failure") << QByteArray("PROPPATCH")
            << start + "<d:propstat><d:prop/><d:status>HTTP/1.1 403 Forbidden</d:status></d:propstat>" + end << false;
    }

    void mutationMultistatus()
    {
        QFETCH(QByteArray, verb);
        QFETCH(QByteArray, response);
        QFETCH(bool, success);
        DriveServer server;
        server.mutationVerb = verb;
        server.mutationResponse = response;
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveFileBrowser browser(&state);
        auto *tree = browser.findChild<QTreeView *>("driveFiles");
        QTRY_COMPARE_WITH_TIMEOUT(tree->model()->rowCount(), 7, 5000);
        QVERIFY(select(tree, "Docs").isValid());
        QSignalSpy operations(&browser, &UDriveFileBrowser::operationFinished);
        if (verb == "DELETE") QVERIFY(browser.deleteSelected());
        else if (verb == "COPY") QVERIFY(browser.copySelected("Docs copy"));
        else if (verb == "MOVE") QVERIFY(browser.renameSelected("Renamed docs"));
        else QVERIFY(browser.setSelectedFavorite(true));
        QTRY_COMPARE(operations.count(), 1);
        const auto result = operations.takeFirst();
        QCOMPARE(result.at(0).toBool(), success);
        if (!success) {
            QVERIFY(result.at(1).toString().contains(QStringLiteral("Refresh")));
            QVERIFY(select(tree, "Docs").isValid());
        }
        const auto requests = server.requests;
        QCOMPARE(std::count_if(requests.begin(), requests.end(), [&verb](const auto &request) { return request.first == verb; }), 1);
        QVERIFY(server.authenticated);
    }


    void readOnlyPermissions()
    {
        DriveServer server;
        server.readOnly = true;
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveFileBrowser browser(&state);
        auto *tree = browser.findChild<QTreeView *>("driveFiles");
        QTRY_COMPARE_WITH_TIMEOUT(tree->model()->rowCount(), 7, 5000);
        QVERIFY(select(tree, "a&b.txt").isValid());
        const int requests = server.requests.size();
        QVERIFY(!browser.createFolder("No permission"));
        QVERIFY(!browser.renameSelected("No permission.txt"));
        QVERIFY(!browser.moveSelected("Docs"));
        QVERIFY(!browser.deleteSelected());
        QCOMPARE(server.requests.size(), requests);
    }

    void deleteConfirmationDialog()
    {
        // Test file confirmation dialog
        DeleteConfirmDialog fileDialog(QStringLiteral("readme.md"), false);
        auto *title = fileDialog.findChild<QLabel *>(QStringLiteral("confirmTitle"));
        auto *message = fileDialog.findChild<QLabel *>(QStringLiteral("confirmMessage"));
        auto *cancelBtn = fileDialog.findChild<QPushButton *>(QStringLiteral("cancelDeleteButton"));
        auto *deleteBtn = fileDialog.findChild<QPushButton *>(QStringLiteral("confirmDeleteButton"));

        QVERIFY(title != nullptr);
        QVERIFY(message != nullptr);
        QVERIFY(cancelBtn != nullptr);
        QVERIFY(deleteBtn != nullptr);

        QVERIFY(title->text().contains(QStringLiteral("readme.md")));
        QVERIFY(message->text().contains(QStringLiteral("This item will be moved to Deleted files")));
        QVERIFY(!message->text().contains(QStringLiteral("A folder includes all of its contents")));

        // Test folder confirmation dialog
        DeleteConfirmDialog folderDialog(QStringLiteral("Documents"), true);
        auto *folderMessage = folderDialog.findChild<QLabel *>(QStringLiteral("confirmMessage"));
        QVERIFY(folderMessage != nullptr);
        QVERIFY(folderMessage->text().contains(QStringLiteral("All contents inside this folder will also be moved to Deleted files")));

        DeleteConfirmDialog permanentDialog(QStringLiteral("Documents"), true, nullptr, true);
        QVERIFY(permanentDialog.findChild<QLabel *>("confirmMessage")->text().contains(QStringLiteral("cannot be undone")));
        QCOMPARE(permanentDialog.findChild<QPushButton *>("confirmDeleteButton")->text(), QStringLiteral("Delete permanently"));
        QVERIFY(permanentDialog.findChild<QPushButton *>("cancelDeleteButton")->isDefault());
    }

    void inputDialogStylingAndValidation()
    {
        UDriveInputDialog dialog(QStringLiteral("New folder"), QStringLiteral("Folder name"), QString(), QStringLiteral("OK"), nullptr, false);
        auto *title = dialog.findChild<QLabel *>(QStringLiteral("inputDialogTitle"));
        auto *label = dialog.findChild<QLabel *>(QStringLiteral("inputDialogLabel"));
        auto *field = dialog.findChild<QLineEdit *>(QStringLiteral("inputDialogField"));
        auto *cancelBtn = dialog.findChild<QPushButton *>(QStringLiteral("inputCancelButton"));
        auto *confirmBtn = dialog.findChild<QPushButton *>(QStringLiteral("inputConfirmButton"));

        QVERIFY(title && label && field && cancelBtn && confirmBtn);
        QCOMPARE(title->text(), QStringLiteral("New folder"));
        QCOMPARE(label->text(), QStringLiteral("Folder name"));
        QVERIFY(!confirmBtn->isEnabled());

        field->setText(QStringLiteral("   "));
        QVERIFY(!confirmBtn->isEnabled());

        field->setText(QStringLiteral("My Folder"));
        QVERIFY(confirmBtn->isEnabled());
        QCOMPARE(dialog.text(), QStringLiteral("My Folder"));

        UDriveInputDialog allowEmptyDialog(QStringLiteral("Move to folder"), QStringLiteral("Destination"), QString(), QStringLiteral("OK"), nullptr, true);
        auto *allowEmptyConfirm = allowEmptyDialog.findChild<QPushButton *>(QStringLiteral("inputConfirmButton"));
        QVERIFY(allowEmptyConfirm && allowEmptyConfirm->isEnabled());
    }

    void confirmRemoveAccountDialogStylingAndBehavior()
    {
        ConfirmRemoveAccountDialog dialog(QStringLiteral("test user@example.com"));
        auto *message = dialog.findChild<QLabel *>(QStringLiteral("confirmRemoveAccountMessage"));
        auto *cancelBtn = dialog.findChild<QPushButton *>(QStringLiteral("cancelRemoveAccountButton"));
        auto *removeBtn = dialog.findChild<QPushButton *>(QStringLiteral("removeAccountButton"));

        QVERIFY(message && cancelBtn && removeBtn);
        QCOMPARE(dialog.windowTitle(), QStringLiteral("Confirm Account Removal"));
        QVERIFY(message->text().contains(QStringLiteral("test user@example.com")));
        QVERIFY(message->text().contains(QStringLiteral("This will <b>not</b> delete any files.")));

        // Ensure no icon labels exist in the dialog (user requirement: don't use icon)
        const auto labels = dialog.findChildren<QLabel *>();
        for (auto *lbl : labels) {
            QVERIFY(lbl->pixmap().isNull());
        }

        // Verify button properties, cursor, and safe default behavior
        QVERIFY(cancelBtn->isDefault());
        QCOMPARE(cancelBtn->cursor().shape(), Qt::PointingHandCursor);
        QCOMPARE(removeBtn->cursor().shape(), Qt::PointingHandCursor);
        QCOMPARE(cancelBtn->text(), QStringLiteral("Cancel"));
        QCOMPARE(removeBtn->text(), QStringLiteral("Remove connection"));
    }

    void moveDialogStylingAndHierarchy()
    {
        const QUrl spaceRoot(QStringLiteral("https://example.com/remote.php/dav/spaces/space-123/"));
        QList<QPair<QString, QString>> initialFolders = {
            {QStringLiteral("working on testing"), QStringLiteral("working on testing")},
            {QStringLiteral("ថតថ្មី"), QStringLiteral("ថតថ្មី")}
        };

        // Case 1: moving a root file
        UDriveMoveDialog rootDialog(nullptr, spaceRoot, QStringLiteral("Personal"),
                                    QStringLiteral("2026-10-05 16.02.12.jpg"), QStringLiteral("2026-10-05 16.02.12.jpg"),
                                    false, initialFolders);
        auto *title = rootDialog.findChild<QLabel *>(QStringLiteral("moveDialogTitle"));
        auto *subtitle = rootDialog.findChild<QLabel *>(QStringLiteral("moveDialogSubtitle"));
        auto *destLabel = rootDialog.findChild<QLabel *>(QStringLiteral("moveDialogDestination"));
        auto *tree = rootDialog.findChild<QTreeWidget *>(QStringLiteral("moveDialogTree"));
        auto *newFolderBtn = rootDialog.findChild<QPushButton *>(QStringLiteral("moveNewFolderButton"));
        auto *cancelBtn = rootDialog.findChild<QPushButton *>(QStringLiteral("moveCancelButton"));
        auto *moveBtn = rootDialog.findChild<QPushButton *>(QStringLiteral("moveConfirmButton"));

        QVERIFY(title && subtitle && destLabel && tree && newFolderBtn && cancelBtn && moveBtn);
        QCOMPARE(rootDialog.windowTitle(), QStringLiteral("Move item"));
        QVERIFY(title->text().contains(QStringLiteral("2026-10-05 16.02.12.jpg")));
        QVERIFY(subtitle->text().contains(QStringLiteral("Personal")));

        // Root item is selected by default; since file is currently at root, move is disabled
        QVERIFY(!moveBtn->isEnabled());
        QVERIFY(destLabel->text().contains(QStringLiteral("Current location")));

        // Selecting a child folder enables Move
        auto *rootItem = tree->topLevelItem(0);
        QVERIFY(rootItem);
        QCOMPARE(rootItem->childCount(), 2);
        auto *firstFolder = rootItem->child(0);
        tree->setCurrentItem(firstFolder);
        QVERIFY(moveBtn->isEnabled());
        QCOMPARE(rootDialog.selectedDestination(), QStringLiteral("working on testing"));
        QVERIFY(destLabel->text().contains(QStringLiteral("Destination: working on testing")));

        // Case 2: moving a file that is in a subfolder
        UDriveMoveDialog subDialog(nullptr, spaceRoot, QStringLiteral("Personal"),
                                   QStringLiteral("working on testing/doc.pdf"), QStringLiteral("doc.pdf"),
                                   false, initialFolders);
        auto *subMoveBtn = subDialog.findChild<QPushButton *>(QStringLiteral("moveConfirmButton"));
        QVERIFY(subMoveBtn && subMoveBtn->isEnabled());
        QCOMPARE(subDialog.selectedDestination(), QString());

        // Case 3: moving a folder (must exclude itself to prevent invalid circular moves)
        UDriveMoveDialog folderDialog(nullptr, spaceRoot, QStringLiteral("Personal"),
                                      QStringLiteral("working on testing"), QStringLiteral("working on testing"),
                                      true, initialFolders);
        auto *folderTree = folderDialog.findChild<QTreeWidget *>(QStringLiteral("moveDialogTree"));
        auto *folderRootItem = folderTree->topLevelItem(0);
        QVERIFY(folderRootItem);
        QCOMPARE(folderRootItem->childCount(), 1);
        QCOMPARE(folderRootItem->child(0)->data(0, Qt::UserRole).toString(), QStringLiteral("ថតថ្មី"));
    }

    void testExpirationDateDialogStyling()
    {
        auto *account = new Account(QUuid::createUuid(), QStringLiteral("test"), QUrl(QStringLiteral("http://localhost")));
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveShareDialog panel(&state, QStringLiteral("Report.pdf"), false, QStringLiteral("Report.pdf"), {}, QStringLiteral("space-1"));

        QJsonObject share{
            {QStringLiteral("id"), QStringLiteral("link-1")},
            {QStringLiteral("expiration"), QStringLiteral("2026-10-22")}
        };

        panel.editPublicLink(QStringLiteral("link-1"), share, QStringLiteral("expireDate"));
        auto *dialog = panel.findChild<QDialog *>(QStringLiteral("publicLinkEditDialog"));
        QVERIFY(dialog);
        QCOMPARE(dialog->windowTitle(), QStringLiteral("Set expiration date"));

        auto *label = dialog->findChild<QLabel *>();
        QVERIFY(label);
        QCOMPARE(label->text(), QStringLiteral("Expiration date"));

        auto *date = dialog->findChild<QDateEdit *>(QStringLiteral("publicLinkExpirationDate"));
        QVERIFY(date);
        QCOMPARE(date->displayFormat(), QStringLiteral("dd/MM/yyyy"));
        QCOMPARE(date->buttonSymbols(), QAbstractSpinBox::NoButtons);

        auto *calendar = date->calendarWidget();
        QVERIFY(calendar);
        QCOMPARE(calendar->horizontalHeaderFormat(), QCalendarWidget::SingleLetterDayNames);
        QCOMPARE(calendar->verticalHeaderFormat(), QCalendarWidget::NoVerticalHeader);

        auto *clearBtn = calendar->findChild<QPushButton *>(QStringLiteral("calendarClearButton"));
        auto *todayBtn = calendar->findChild<QPushButton *>(QStringLiteral("calendarTodayButton"));
        QVERIFY(clearBtn && todayBtn);
        QCOMPARE(clearBtn->text(), QStringLiteral("Clear"));
        QCOMPARE(todayBtn->text(), QStringLiteral("Today"));

        auto *remove = dialog->findChild<QCheckBox *>(QStringLiteral("publicLinkRemoveSetting"));
        QVERIFY(remove);
        todayBtn->click();
        QCOMPARE(date->date(), QDate::currentDate());
        QCOMPARE(remove->isChecked(), false);

        clearBtn->click();
        QCOMPARE(remove->isChecked(), true);

        auto *box = dialog->findChild<QDialogButtonBox *>();
        QVERIFY(box);
        auto *confirmBtn = box->button(QDialogButtonBox::Save);
        QVERIFY(confirmBtn);
        QCOMPARE(confirmBtn->text(), QStringLiteral("Confirm"));

        dialog->show();
        dialog->resize(440, 200);
        QTest::qWait(50);
        dialog->grab().save(QStringLiteral("/tmp/expiration-dialog.png"));

        calendar->show();
        calendar->resize(320, 320);
        QTest::qWait(50);
        calendar->grab().save(QStringLiteral("/tmp/calendar-popup.png"));

        dialog->reject();
    }

    void testWorkspaceControlsAlignment()
    {
        auto *account = new Account(QUuid::createUuid(), QStringLiteral("test"), QUrl(QStringLiteral("http://localhost")));
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveFileBrowser browser(&state);
        browser.resize(1050, 760);
        browser.show();
        browser.showWorkspacesOverview();

        auto *controls = browser.findChild<QWidget *>(QStringLiteral("driveWorkspaceControls"));
        QVERIFY(controls);
        QVERIFY(controls->isVisible());

        auto *includeDisabled = browser.findChild<QPushButton *>(QStringLiteral("driveIncludeDisabled"));
        auto *search = browser.findChild<QLineEdit *>(QStringLiteral("driveWorkspaceSearch"));
        auto *sort = browser.findChild<QComboBox *>(QStringLiteral("driveWorkspaceSort"));
        auto *selectAll = browser.findChild<QCheckBox *>(QStringLiteral("driveWorkspaceSelectAll"));
        QVERIFY(includeDisabled && search && sort && selectAll);

        QTest::qWait(50);
        controls->grab().save(QStringLiteral("/tmp/workspace-controls.png"));
    }

    void testFolderSizeDisplay()
    {
        auto *account = new Account(QUuid::createUuid(), QStringLiteral("test"), QUrl(QStringLiteral("http://localhost")));
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveFileBrowser browser(&state);
        browser.show();
        auto *tree = browser.findChild<QTreeView *>(QStringLiteral("driveFiles"));
        QVERIFY(tree);

        // Append a folder entry with oc:size = 1048576 (1 MB)
        QMap<QString, QString> folderProps{
            {QStringLiteral("resourcetype"), QStringLiteral("collection")},
            {QStringLiteral("size"), QStringLiteral("1048576")},
            {QStringLiteral("permissions"), QStringLiteral("RDNVCKW")},
            {QStringLiteral("getetag"), QStringLiteral("etag-folder")}
        };
        browser.appendEntry(QStringLiteral("/dav/personal/Projects"), folderProps, QUrl(QStringLiteral("http://localhost/dav/personal")));

        auto *model = tree->model();
        QCOMPARE(model->rowCount(), 1);
        QCOMPARE(model->index(0, 0).data().toString(), QStringLiteral("Projects"));
        // Folder must display formatted size, NOT "—"
        const QString folderSizeStr = model->index(0, 1).data().toString();
        QVERIFY(folderSizeStr != QStringLiteral("—"));
        QCOMPARE(folderSizeStr, QLocale().formattedDataSize(1048576));

        // Append a folder with missing size: displays "—"
        QMap<QString, QString> unknownFolderProps{
            {QStringLiteral("resourcetype"), QStringLiteral("collection")},
            {QStringLiteral("permissions"), QStringLiteral("RDNVCKW")}
        };
        browser.appendEntry(QStringLiteral("/dav/personal/EmptyFolder"), unknownFolderProps, QUrl(QStringLiteral("http://localhost/dav/personal")));
        QCOMPARE(model->rowCount(), 2);

        int emptyFolderRow = -1;
        int projectsRow = -1;
        for (int r = 0; r < model->rowCount(); ++r) {
            const QString name = model->index(r, 0).data().toString();
            if (name == QStringLiteral("EmptyFolder")) {
                emptyFolderRow = r;
            } else if (name == QStringLiteral("Projects")) {
                projectsRow = r;
            }
        }
        QVERIFY(emptyFolderRow != -1);
        QVERIFY(projectsRow != -1);
        QCOMPARE(model->index(emptyFolderRow, 1).data().toString(), QStringLiteral("—"));
        QCOMPARE(model->index(projectsRow, 1).data().toString(), QLocale().formattedDataSize(1048576));
    }

    void testFileVersionsDisplayAndActions()
    {
        auto *account = new Account(QUuid::createUuid(), QStringLiteral("test"), QUrl(QStringLiteral("http://localhost")));
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveFileBrowser browser(&state);
        browser.resize(1000, 650);
        browser.show();
        auto *tree = browser.findChild<QTreeView *>(QStringLiteral("driveFiles"));
        QVERIFY(tree);

        // 1. Verify XML parsing of WebDAV versions multistatus
        const QByteArray xml =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<d:multistatus xmlns:d=\"DAV:\" xmlns:oc=\"http://owncloud.org/ns\">\n"
            "  <d:response>\n"
            "    <d:href>/dav/meta/file-123/v/</d:href>\n"
            "    <d:propstat>\n"
            "      <d:prop><d:resourcetype><d:collection/></d:resourcetype></d:prop>\n"
            "      <d:status>HTTP/1.1 200 OK</d:status>\n"
            "    </d:propstat>\n"
            "  </d:response>\n"
            "  <d:response>\n"
            "    <d:href>/dav/meta/file-123/v/1728390000</d:href>\n"
            "    <d:propstat>\n"
            "      <d:prop>\n"
            "        <d:getcontentlength>72704</d:getcontentlength>\n"
            "        <d:getlastmodified>Thu, 08 Oct 2026 12:20:00 GMT</d:getlastmodified>\n"
            "        <d:getetag>&quot;etag-v1&quot;</d:getetag>\n"
            "        <d:resourcetype/>\n"
            "      </d:prop>\n"
            "      <d:status>HTTP/1.1 200 OK</d:status>\n"
            "    </d:propstat>\n"
            "  </d:response>\n"
            "  <d:response>\n"
            "    <d:href>/dav/meta/file-123/v/1726000000</d:href>\n"
            "    <d:propstat>\n"
            "      <d:prop>\n"
            "        <d:getcontentlength>69632</d:getcontentlength>\n"
            "        <d:getlastmodified>Wed, 10 Sep 2026 09:15:00 GMT</d:getlastmodified>\n"
            "        <d:getetag>&quot;etag-v2&quot;</d:getetag>\n"
            "        <d:resourcetype/>\n"
            "      </d:prop>\n"
            "      <d:status>HTTP/1.1 200 OK</d:status>\n"
            "    </d:propstat>\n"
            "  </d:response>\n"
            "</d:multistatus>";

        const auto versions = browser.parseVersionsMultistatus(xml, QUrl(QStringLiteral("http://localhost/dav/meta/file-123/v/")));
        QCOMPARE(versions.size(), 2);
        // Sorted newest first
        QCOMPARE(versions.at(0).size, 72704);
        QCOMPARE(versions.at(0).href, QStringLiteral("/dav/meta/file-123/v/1728390000"));
        QCOMPARE(versions.at(1).size, 69632);
        QCOMPARE(versions.at(1).href, QStringLiteral("/dav/meta/file-123/v/1726000000"));

        // SEC-01 test: Malicious external origin must be rejected
        const QByteArray evilXml =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<d:multistatus xmlns:d=\"DAV:\">\n"
            "  <d:response>\n"
            "    <d:href>https://evil.com/dav/meta/file-123/v/999</d:href>\n"
            "    <d:propstat>\n"
            "      <d:prop><d:getcontentlength>123</d:getcontentlength><d:getlastmodified>Thu, 08 Oct 2026 12:20:00 GMT</d:getlastmodified></d:prop>\n"
            "      <d:status>HTTP/1.1 200 OK</d:status>\n"
            "    </d:propstat>\n"
            "  </d:response>\n"
            "</d:multistatus>";
        const auto evilVersions = browser.parseVersionsMultistatus(evilXml, QUrl(QStringLiteral("http://localhost/dav/meta/file-123/v/")));
        QCOMPARE(evilVersions.size(), 0);

        // 2. Append file entry with fileid
        QMap<QString, QString> fileProps{
            {QStringLiteral("resourcetype"), QString()},
            {QStringLiteral("getcontentlength"), QStringLiteral("72704")},
            {QStringLiteral("getlastmodified"), QStringLiteral("Thu, 08 Oct 2026 12:20:00 GMT")},
            {QStringLiteral("permissions"), QStringLiteral("RDNVW")},
            {QStringLiteral("fileid"), QStringLiteral("file-123")},
            {QStringLiteral("id"), QStringLiteral("file-123")}
        };
        browser.appendEntry(QStringLiteral("/dav/personal/Project_Flowchart.xlsx"), fileProps, QUrl(QStringLiteral("http://localhost/dav/personal")));

        // Select the file and open details
        auto *model = tree->model();
        QCOMPARE(model->rowCount(), 1);
        tree->selectionModel()->select(model->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);

        auto *detailsAct = browser.findChild<QAction *>(QStringLiteral("driveDetailsAction"));
        QVERIFY(detailsAct);
        detailsAct->trigger();

        auto *versionsBtn = browser.findChild<QPushButton *>(QStringLiteral("driveDetailsVersions"));
        QVERIFY(versionsBtn);
        QVERIFY(!versionsBtn->isHidden());

        // 3. Switch to Versions page
        browser.showFileDetailsVersions(true, true);

        auto *pages = browser.findChild<QStackedWidget *>(QStringLiteral("driveDetailsPages"));
        auto *title = browser.findChild<QLabel *>(QStringLiteral("driveDetailsTitle"));
        auto *back = browser.findChild<QToolButton *>(QStringLiteral("driveDetailsBack"));
        QVERIFY(pages && title && back);
        QCOMPARE(title->text(), QStringLiteral("Versions"));
        QVERIFY(back->isVisible());

        // 4. Test clicking back returns to Overview
        QTest::mouseClick(back, Qt::LeftButton);
        QCOMPARE(pages->currentIndex(), 0);
        QVERIFY(!back->isVisible());

        // 5. Append a folder and verify Versions button is hidden for folders
        QMap<QString, QString> folderProps{
            {QStringLiteral("resourcetype"), QStringLiteral("collection")},
            {QStringLiteral("permissions"), QStringLiteral("RDNVCKW")}
        };
        browser.appendEntry(QStringLiteral("/dav/personal/FolderA"), folderProps, QUrl(QStringLiteral("http://localhost/dav/personal")));
        int folderRow = (model->index(0, 0).data().toString() == QStringLiteral("FolderA")) ? 0 : 1;
        tree->selectionModel()->select(model->index(folderRow, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        QVERIFY(versionsBtn->isHidden());

        // 6. Populate versions into the panel and test widgets
        int fileRow = (folderRow == 0) ? 1 : 0;
        tree->selectionModel()->select(model->index(fileRow, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        browser.showFileDetailsVersions(true, true);
        browser.setVersions(versions, QStringLiteral("Project_Flowchart.xlsx"), QStringLiteral("Project_Flowchart.xlsx"), QUrl(), true);

        auto *restoreBtn = browser.findChild<QPushButton *>(QStringLiteral("versionRestoreBtn"));
        auto *downloadBtn = browser.findChild<QPushButton *>(QStringLiteral("versionDownloadBtn"));
        QVERIFY(restoreBtn);
        QVERIFY(downloadBtn);
        QVERIFY(restoreBtn->isVisible());
        QVERIFY(restoreBtn->isEnabled());
        QVERIFY(downloadBtn->isVisible());

        // SEC-04 test: Read-only files must disable the Restore button
        browser.setVersions(versions, QStringLiteral("Project_Flowchart.xlsx"), QStringLiteral("Project_Flowchart.xlsx"), QUrl(), false);
        restoreBtn = browser.findChild<QPushButton *>(QStringLiteral("versionRestoreBtn"));
        QVERIFY(restoreBtn);
        QVERIFY(!restoreBtn->isEnabled());

        // Reset to canRestore = true for visual rendering
        browser.setVersions(versions, QStringLiteral("Project_Flowchart.xlsx"), QStringLiteral("Project_Flowchart.xlsx"), QUrl(), true);

        // Save screenshot of the versions view for user verification
        auto *fileDetails = browser.findChild<QWidget *>(QStringLiteral("driveDetailsPages"))->parentWidget();
        if (fileDetails) {
            const bool dark = QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
            QPixmap pix(fileDetails->size());
            pix.fill(dark ? QColor(0x18, 0x1A, 0x1F) : Qt::white);
            QPainter p(&pix);
            fileDetails->render(&p);
            p.end();
            pix.save(QStringLiteral("/Users/tochratana/.gemini/antigravity/brain/00705d74-3dbe-4bb0-962c-114a4aadd1a1/versions-panel.png"));
        }
    }

    void testAboutDialogStyling()
    {
        AboutDialog dialog;
        dialog.resize(900, 650);
        dialog.show();
        QTest::qWait(50);

        auto *card = dialog.findChild<QFrame *>(QStringLiteral("aboutCard"));
        QVERIFY(card);
        auto *name = dialog.findChild<QLabel *>(QStringLiteral("aboutAppName"));
        QVERIFY(name);
        QCOMPARE(name->text(), QStringLiteral("UDrive"));

        auto *badge = dialog.findChild<QLabel *>(QStringLiteral("aboutVersionBadge"));
        QVERIFY(badge);

        auto *pages = dialog.findChild<QStackedWidget *>(QStringLiteral("aboutPages"));
        QVERIFY(pages);
        QCOMPARE(pages->currentIndex(), 0);

        auto *tabDiag = dialog.findChild<QPushButton *>(QStringLiteral("tabBtnDiag"));
        QVERIFY(tabDiag);
        QTest::mouseClick(tabDiag, Qt::LeftButton);
        QCOMPARE(pages->currentIndex(), 1);

        auto *copyBtn = dialog.findChild<QPushButton *>(QStringLiteral("aboutCopyDiagBtn"));
        QVERIFY(copyBtn);

        const bool dark = QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
        // 1. Capture Diagnostics tab
        QPixmap diagPix(dialog.size());
        diagPix.fill(dark ? QColor(0x18, 0x1A, 0x1F) : Qt::white);
        QPainter pDiag(&diagPix);
        dialog.render(&pDiag);
        pDiag.end();
        diagPix.save(QStringLiteral("/Users/tochratana/.gemini/antigravity/brain/00705d74-3dbe-4bb0-962c-114a4aadd1a1/about-dialog-diag.png"));

        // 2. Switch back to Overview for the Overview screenshot
        auto *tabAbout = dialog.findChild<QPushButton *>(QStringLiteral("tabBtnAbout"));
        QVERIFY(tabAbout);
        QTest::mouseClick(tabAbout, Qt::LeftButton);
        QCOMPARE(pages->currentIndex(), 0);

        QPixmap pix(dialog.size());
        pix.fill(dark ? QColor(0x18, 0x1A, 0x1F) : Qt::white);
        QPainter p(&pix);
        dialog.render(&p);
        p.end();
        pix.save(QStringLiteral("/Users/tochratana/.gemini/antigravity/brain/00705d74-3dbe-4bb0-962c-114a4aadd1a1/about-dialog.png"));

        // Verify GeneralSettings footer labels
        GeneralSettings genSettings;
        genSettings.resize(800, 600);
        genSettings.show();
        QTest::qWait(50);
        auto *copyLbl = genSettings.findChild<QLabel *>(QStringLiteral("copyrightLabel"));
        QVERIFY(copyLbl);
        QCOMPARE(copyLbl->text(), QStringLiteral("Unity Workspace"));
        auto *aboutBtn = genSettings.findChild<QPushButton *>(QStringLiteral("about_pushButton"));
        QVERIFY(aboutBtn);
        QCOMPARE(aboutBtn->text(), QStringLiteral("About UDrive"));

        QPixmap genPix(genSettings.size());
        genPix.fill(dark ? QColor(0x18, 0x1A, 0x1F) : Qt::white);
        QPainter pGen(&genPix);
        genSettings.render(&pGen);
        pGen.end();
        genPix.save(QStringLiteral("/Users/tochratana/.gemini/antigravity/brain/00705d74-3dbe-4bb0-962c-114a4aadd1a1/general-settings.png"));
    }

    void emptyTrash_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const auto *scenario : {"accept", "filter", "cancel", "space", "refresh", "etag", "signout", "hide", "error", "partial", "multistatus"})
            QTest::newRow(scenario) << QString::fromLatin1(scenario);
    }

    void emptyTrash()
    {
        QFETCH(QString, scenario);
        DriveServer server;
        server.trash.insert("deleted-1", "/dav/personal/a&b.txt");
        server.trash.insert("deleted-2", "/dav/personal/denied.txt");
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveFileBrowser browser(&state); browser.resize(1000, 650); browser.show();
        auto *tree = browser.findChild<QTreeView *>("driveFiles");
        auto *empty = browser.findChild<QPushButton *>("driveEmptyTrash");
        QVERIFY(empty);
        QTRY_COMPARE(tree->model()->rowCount(), 7);
        QVERIFY(!empty->isVisible());
        browser.showTrash();
        auto *spaces = browser.findChild<QComboBox *>("driveSpaces");
        spaces->setCurrentIndex(spaces->findText("Personal"));
        QTRY_COMPARE(tree->model()->rowCount(), 2);
        QTRY_VERIFY(empty->isVisible() && empty->isEnabled());
        if (scenario == "filter") {
            browser.findChild<QLineEdit *>("driveSearch")->setText("a&b");
            QTRY_COMPARE(tree->model()->rowCount(), 1);
        } else tree->selectAll(); // Works with multiple selection as well.
        QSignalSpy finished(&browser, &UDriveFileBrowser::operationFinished);
        empty->click();
        QPointer<DeleteConfirmDialog> dialog = browser.findChild<DeleteConfirmDialog *>();
        QVERIFY(dialog && dialog->isVisible());
        QVERIFY(dialog->findChild<QLabel *>("confirmTitle")->text().contains("Personal"));
        QVERIFY(dialog->findChild<QLabel *>("confirmMessage")->text().contains("2"));
        QVERIFY(dialog->findChild<QLabel *>("confirmMessage")->text().contains("hidden by search"));
        QVERIFY(dialog->findChild<QPushButton *>("cancelDeleteButton")->isDefault());
        QCOMPARE(server.trash.size(), 2); // Opening the dialog never deletes.
        if (scenario == "space") {
            spaces->setCurrentIndex(spaces->findText("Team files"));
            QTRY_COMPARE(tree->model()->rowCount(), 2);
        } else if (scenario == "refresh") {
            browser.findChild<QPushButton *>("driveRefresh")->click();
            QTRY_COMPARE(tree->model()->rowCount(), 2);
        } else if (scenario == "etag") {
            QVERIFY(tree->model()->setData(tree->model()->index(0, 0), QStringLiteral("\"new-revision\""), Qt::UserRole + 9));
        } else if (scenario == "signout") state.signOutByUi();
        else if (scenario == "hide") browser.hide();
        else if (scenario == "error") {
            server.rejectVerb = "DELETE"; server.rejectStatus = 403; server.rejectionsLeft = 1;
        } else if (scenario == "partial") server.deniedTrash.insert("deleted-2");
        else if (scenario == "multistatus") {
            server.mutationVerb = "DELETE";
            server.mutationResponse = "<d:multistatus xmlns:d=\"DAV:\"><d:response><d:href>/failed</d:href><d:status>HTTP/1.1 403 Forbidden</d:status></d:response></d:multistatus>";
        }
        if (dialog) {
            if (scenario == "cancel" || scenario == "hide") dialog->reject();
            else dialog->accept();
        }
        const bool success = scenario == "accept" || scenario == "filter";
        const bool attempted = success || scenario == "error" || scenario == "partial" || scenario == "multistatus";
        if (attempted) {
            QTRY_COMPARE(finished.count(), 1);
            QCOMPARE(finished.first().at(0).toBool(), success);
            if (success) {
                QTRY_VERIFY(server.trash.isEmpty());
                QTRY_VERIFY(!empty->isEnabled());
                QCOMPARE(server.requestHeaders.value("DELETE /dav/spaces/trash-bin/personal/deleted-1").value("if-match"), QByteArray("\"tag\""));
                QCOMPARE(server.requestHeaders.value("DELETE /dav/spaces/trash-bin/personal/deleted-2").value("if-match"), QByteArray("\"tag\""));
            } else if (scenario == "partial") {
                QCOMPARE(server.trash.size(), 1);
                QVERIFY(server.trash.contains("deleted-2"));
                QVERIFY(finished.first().at(1).toString().contains("1 of 2"));
            } else QCOMPARE(server.trash.size(), 2);
        } else {
            QTest::qWait(100);
            QCOMPARE(finished.count(), 0);
            QCOMPARE(server.trash.size(), 2);
        }
        QCOMPARE(std::count_if(server.requests.cbegin(), server.requests.cend(), [](const auto &request) { return request.first == "DELETE"; }), (success || scenario == "partial") ? 2 : attempted ? 1 : 0);
        QTRY_VERIFY(dialog.isNull());
    }

    void emptyTrashStopsBetweenItems_data()
    {
        QTest::addColumn<QString>("stop");
        for (const auto *stop : {"button", "navigate", "hide", "destroy"}) QTest::newRow(stop) << QString::fromLatin1(stop);
    }

    void emptyTrashStopsBetweenItems()
    {
        QFETCH(QString, stop);
        DriveServer server;
        server.trashDeleteDelay = 300;
        server.trash.insert("deleted-1", "/dav/personal/a&b.txt");
        server.trash.insert("deleted-2", "/dav/personal/denied.txt");
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        auto browser = std::make_unique<UDriveFileBrowser>(&state); browser->show();
        auto *tree = browser->findChild<QTreeView *>("driveFiles");
        QTRY_COMPARE(tree->model()->rowCount(), 7);
        browser->showTrash();
        auto *spaces = browser->findChild<QComboBox *>("driveSpaces");
        spaces->setCurrentIndex(spaces->findText("Personal"));
        QTRY_COMPARE(tree->model()->rowCount(), 2);
        auto *empty = browser->findChild<QPushButton *>("driveEmptyTrash");
        QTRY_VERIFY(empty->isEnabled());
        empty->click();
        browser->findChild<DeleteConfirmDialog *>()->accept();
        QTRY_VERIFY(server.requests.contains({"DELETE", "/dav/spaces/trash-bin/personal/deleted-1"}));
        QVERIFY(!empty->isEnabled());
        if (stop == "button") browser->findChild<QPushButton *>("driveCancelDownload")->click();
        else if (stop == "navigate") browser->showPersonal();
        else if (stop == "hide") browser->hide();
        else browser.reset();
        QTest::qWait(400);
        QVERIFY(server.trash.contains("deleted-2"));
        QCOMPARE(std::count_if(server.requests.cbegin(), server.requests.cend(), [](const auto &request) { return request.first == "DELETE"; }), 1);
    }

    void deleteConfirmationGuards_data()
    {
        QTest::addColumn<bool>("permanent");
        QTest::addColumn<QString>("change");
        for (bool permanent : {false, true})
            for (const auto &change : {"accept", "cancel", "space", "selection", "refresh", "etag", "signout", "hide"})
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(permanent ? "permanent" : "trash", change)))
                    << permanent << QString::fromLatin1(change);
    }

    void deleteConfirmationGuards()
    {
        QFETCH(bool, permanent); QFETCH(QString, change);
        DriveServer server;
        server.trash.insert("deleted-1", "/dav/personal/a&b.txt");
        server.trash.insert("deleted-2", "/dav/personal/denied.txt");
        server.uploaded.insert("/dav/team/a&b.txt", "other-space fixture");
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account); UDriveFileBrowser browser(&state); browser.show();
        auto *tree = browser.findChild<QTreeView *>("driveFiles");
        QTRY_COMPARE(tree->model()->rowCount(), 7);
        if (permanent) {
            browser.showTrash();
            auto *spaces = browser.findChild<QComboBox *>("driveSpaces");
            spaces->setCurrentIndex(spaces->findText("Personal"));
            QTRY_COMPARE(tree->model()->rowCount(), 2);
        }
        QVERIFY(select(tree, "a&b.txt").isValid());
        QAction *action = browser.findChild<QAction *>("drivePermanentDelete");
        if (!permanent)
            for (auto *candidate : browser.findChild<QMenu *>("driveActionsMenu")->actions())
                if (candidate->text() == QStringLiteral("Move to Deleted files")) action = candidate;
        QVERIFY(action && action->isEnabled());
        action->trigger(); // Returns immediately; no nested event loop.
        QPointer<DeleteConfirmDialog> dialog = browser.findChild<DeleteConfirmDialog *>();
        QVERIFY(dialog && dialog->isVisible());
        QSignalSpy completed(&browser, &UDriveFileBrowser::operationFinished);
        if (change == "space") {
            if (permanent) {
                auto *spaces = browser.findChild<QComboBox *>("driveSpaces");
                spaces->setCurrentIndex(spaces->findText("Team files"));
            } else browser.showWorkspaces();
            QTRY_COMPARE(tree->model()->rowCount(), 2);
            QVERIFY(select(tree, "a&b.txt").isValid());
        } else if (change == "selection") {
            QVERIFY(select(tree, "denied.txt").isValid());
        } else if (change == "refresh") {
            browser.findChild<QPushButton *>("driveRefresh")->click();
            QTRY_COMPARE(tree->model()->rowCount(), permanent ? 2 : 7);
            QVERIFY(select(tree, "a&b.txt").isValid());
        } else if (change == "etag") {
            // Change the listed revision without replacing the item/index.
            QVERIFY(tree->model()->setData(tree->currentIndex(), QStringLiteral("\"new-revision\""), Qt::UserRole + 9));
        } else if (change == "signout") {
            state.signOutByUi();
            QTRY_VERIFY(!dialog || !dialog->isVisible());
        } else if (change == "hide") {
            browser.hide();
            QTRY_VERIFY(!dialog || !dialog->isVisible());
        }
        if (dialog) {
            if (change == "cancel" || change == "hide") dialog->reject();
            else dialog->accept();
        }
        if (change == "accept") {
            QTRY_COMPARE(completed.count(), 1);
            QVERIFY(completed.first().at(0).toBool());
            const QString target = permanent ? QStringLiteral("/dav/spaces/trash-bin/personal/deleted-1") : QStringLiteral("/dav/personal/a&b.txt");
            QVERIFY(server.requests.contains(qMakePair(QByteArray("DELETE"), target)));
            QCOMPARE(server.requestHeaders.value("DELETE " + target).value("if-match"), QByteArray("\"tag\""));
            if (permanent) QVERIFY(!server.trash.contains("deleted-1"));
        } else {
            QTest::qWait(100);
            QCOMPARE(completed.count(), 0);
        }
        QCOMPARE(std::count_if(server.requests.cbegin(), server.requests.cend(), [](const auto &r) { return r.first == "DELETE"; }), change == "accept" ? 1 : 0);
        QTRY_VERIFY(dialog.isNull());
    }

    void deleteConfirmationParentDestruction_data()
    {
        QTest::addColumn<bool>("permanent");
        QTest::newRow("trash") << false;
        QTest::newRow("permanent") << true;
    }

    void deleteConfirmationParentDestruction()
    {
        QFETCH(bool, permanent);
        DriveServer server; server.trash.insert("deleted-1", "/dav/personal/a&b.txt");
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        auto browser = std::make_unique<UDriveFileBrowser>(&state); browser->show();
        auto *tree = browser->findChild<QTreeView *>("driveFiles");
        QTRY_COMPARE(tree->model()->rowCount(), 7);
        if (permanent) {
            browser->showTrash();
            auto *spaces = browser->findChild<QComboBox *>("driveSpaces");
            spaces->setCurrentIndex(spaces->findText("Personal"));
            QTRY_COMPARE(tree->model()->rowCount(), 1);
        }
        QVERIFY(select(tree, "a&b.txt").isValid());
        QAction *action = browser->findChild<QAction *>("drivePermanentDelete");
        if (!permanent)
            for (auto *candidate : browser->findChild<QMenu *>("driveActionsMenu")->actions())
                if (candidate->text() == QStringLiteral("Move to Deleted files")) action = candidate;
        QVERIFY(action && action->isEnabled()); action->trigger();
        QPointer<DeleteConfirmDialog> dialog = browser->findChild<DeleteConfirmDialog *>();
        QVERIFY(dialog && dialog->isVisible());
        browser.reset(); // Same destruction boundary used when removing an account.
        QVERIFY(dialog.isNull());
        QTest::qWait(50);
        QVERIFY(std::none_of(server.requests.cbegin(), server.requests.cend(), [](const auto &r) { return r.first == "DELETE"; }));
    }

    void deleteConfirmationTranslations_data()
    {
        QTest::addColumn<QString>("locale");
        QTest::addColumn<QString>("permanentLabel");
        QTest::newRow("khmer") << QStringLiteral("km") << QStringLiteral("លុបជាអចិន្ត្រៃយ៍");
        QTest::newRow("chinese") << QStringLiteral("zh_CN") << QStringLiteral("永久删除");
    }

    void deleteConfirmationTranslations()
    {
        QFETCH(QString, locale); QFETCH(QString, permanentLabel);
        QTranslator translator;
        QVERIFY(translator.load(Translations::translationsFilePrefix() + locale, Translations::translationsDirectoryPath()));
        QVERIFY(qApp->installTranslator(&translator));
        const auto cleanup = qScopeGuard([&] { qApp->removeTranslator(&translator); });
        const QString name = QStringLiteral("<b>ឯកសារ 文件.pdf</b>");
        for (bool permanent : {false, true}) {
            DeleteConfirmDialog dialog(name, true, nullptr, permanent);
            auto *title = dialog.findChild<QLabel *>("confirmTitle");
            auto *message = dialog.findChild<QLabel *>("confirmMessage");
            QVERIFY(title && message);
            QCOMPARE(title->textFormat(), Qt::PlainText);
            QVERIFY(title->text().contains(name));
            const auto source = permanent ? "This folder and all of its contents will be permanently deleted. This action cannot be undone."
                                          : "All contents inside this folder will also be moved to Deleted files. You can restore them anytime.";
            QCOMPARE(message->text(), translator.translate("OCC::DeleteConfirmDialog", source));
            QVERIFY(message->text() != QString::fromUtf8(source));
            if (permanent) {
                QCOMPARE(dialog.windowTitle(), permanentLabel);
                QCOMPARE(dialog.findChild<QPushButton *>("confirmDeleteButton")->text(), permanentLabel);
            }
        }
    }

    void monochromeStatusIcons()
    {
        if (Theme::instance()->appNameGUI() != QStringLiteral("UDrive") || Resources::isVanillaTheme())
            QSKIP("UDrive branding is not enabled in this build.");
        const QStringList states {QStringLiteral("ok"), QStringLiteral("error"), QStringLiteral("offline"),
            QStringLiteral("pause"), QStringLiteral("sync"), QStringLiteral("information")};
        for (int size : {22, 32}) {
            for (const auto &flavor : {QStringLiteral("black"), QStringLiteral("white")}) {
                QSet<QByteArray> silhouettes;
                for (const auto &state : states) {
                    const auto icon = Resources::loadIcon(flavor, QStringLiteral("state-") + state, Resources::IconType::BrandedIcon);
                    const auto image = icon.pixmap(size, size).toImage();
                    QVERIFY2(!image.isNull(), qPrintable(flavor + QLatin1Char('/') + state));
                    QByteArray alpha;
                    for (int y = 0; y < image.height(); ++y) {
                        for (int x = 0; x < image.width(); ++x) alpha.append(static_cast<char>(image.pixelColor(x, y).alpha()));
                    }
                    // macOS template icons use alpha, so color differences do
                    // not establish distinguishable sync-state indicators.
                    QVERIFY(alpha.contains(char(0)));
                    QVERIFY(alpha.contains(char(255)));
                    QVERIFY2(!silhouettes.contains(alpha), qPrintable(flavor + QLatin1Char('/') + state + QStringLiteral(" repeats another status mask")));
                    silhouettes.insert(alpha);
                }
            }
        }
    }

    void sidebarToggleAndVisibility()
    {
        if (Theme::instance()->appNameGUI() != QStringLiteral("UDrive") || Resources::isVanillaTheme())
            QSKIP("UDrive branding is not enabled in this build.");

        auto settings = ConfigFile::makeQSettings();
        settings.remove(QStringLiteral("UDrive/sidebarVisible"));

        SettingsDialog window(nullptr);
        window.resize(1200, 800);

        auto *quickWidget = window.findChild<QQuickWidget *>();
        auto *brandWidget = window.findChild<QWidget *>(QStringLiteral("driveBrand"));
        QVERIFY(quickWidget);
        QVERIFY(brandWidget);

        // Initially visible: sidebar open at 236px, brand widget at 224px
        QVERIFY(window.isSidebarVisible());
        QCOMPARE(quickWidget->width(), 236);
        QVERIFY(!quickWidget->isHidden());
        QCOMPARE(brandWidget->width(), 224);

        // Collapse sidebar via method / QML slot into 56px icon rail
        window.setSidebarVisible(false);
        QVERIFY(!window.isSidebarVisible());
        QCOMPARE(quickWidget->width(), 56);
        QVERIFY(!quickWidget->isHidden());
        QCOMPARE(brandWidget->width(), 140);
        QCOMPARE(settings.value(QStringLiteral("UDrive/sidebarVisible")).toBool(), false);

        // Expand back via toggleSidebar()
        window.toggleSidebar();
        QVERIFY(window.isSidebarVisible());
        QCOMPARE(quickWidget->width(), 236);
        QVERIFY(!quickWidget->isHidden());
        QCOMPARE(brandWidget->width(), 224);
        QCOMPARE(settings.value(QStringLiteral("UDrive/sidebarVisible")).toBool(), true);

        // Collapse again
        window.setSidebarVisible(false);
        QVERIFY(!window.isSidebarVisible());
        QCOMPARE(quickWidget->width(), 56);
        QVERIFY(!quickWidget->isHidden());

        // Test persistence when opening a new dialog
        SettingsDialog reopened(nullptr);
        reopened.resize(1200, 800);
        auto *reopenedQuickWidget = reopened.findChild<QQuickWidget *>();
        auto *reopenedBrandWidget = reopened.findChild<QWidget *>(QStringLiteral("driveBrand"));
        QVERIFY(reopenedQuickWidget && reopenedBrandWidget);
        QVERIFY(!reopened.isSidebarVisible());
        QCOMPARE(reopenedQuickWidget->width(), 56);
        QVERIFY(!reopenedQuickWidget->isHidden());
        QCOMPARE(reopenedBrandWidget->width(), 140);

        // Expanding reopened window
        reopened.toggleSidebar();
        QVERIFY(reopened.isSidebarVisible());
        QCOMPARE(reopenedQuickWidget->width(), 236);
        QVERIFY(!reopenedQuickWidget->isHidden());
        QCOMPARE(reopenedBrandWidget->width(), 224);

        // Cleanup
        settings.remove(QStringLiteral("UDrive/sidebarVisible"));
    }

    void detailsSidebarLayout_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::newRow("dark") << true;
        QTest::newRow("light") << false;
    }

    void detailsSidebarLayout()
    {
        QFETCH(bool, dark);
        DriveServer server;
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        auto *manager = AccountManager::instance();
        auto *state = manager->addAccount(account);
        const auto cleanupAccount = qScopeGuard([&] { manager->deleteAccount(state); });
        const QPalette previous = qApp->palette();
        const auto restorePalette = qScopeGuard([&] { qApp->setPalette(previous); });
        QPalette palette = previous;
        palette.setColor(QPalette::Window, QColor(dark ? "#292929" : "#FFFFFF"));
        palette.setColor(QPalette::WindowText, QColor(dark ? "#EDEEF0" : "#111827"));
        palette.setColor(QPalette::ButtonText, palette.color(QPalette::WindowText));
        qApp->setPalette(palette);
        SettingsDialog window(nullptr);
        window.resize(1250, 850);
        window.setCurrentAccount(account);
        window.setCurrentPage(SettingsDialog::SettingsPage::Files);
        window.show();
        // The executable and library each compile the browser's moc metadata.
        auto *browser = static_cast<UDriveFileBrowser *>(window.findChild<QWidget *>("udriveFileBrowser"));
        QVERIFY(browser);
        auto *tree = browser->findChild<QTreeView *>("driveFiles");
        QTRY_COMPARE(tree->model()->rowCount(), 7);
        QVERIFY(select(tree, "a&b.txt").isValid());
        auto *details = browser->findChild<QScrollArea *>("driveFileDetails");
        auto *splitter = browser->findChild<QSplitter *>("driveFileContent");
        auto *openDetails = browser->findChild<QAction *>("driveDetailsAction");
        auto *actions = browser->findChild<QPushButton *>("driveDetailsActions");
        QVERIFY(details && splitter && openDetails && actions);
        openDetails->trigger();
        QTRY_VERIFY(details->isVisible());
        QTest::qWait(50);
        const int expectedWidth = qBound(280, splitter->width() / 3, 360);
        QVERIFY(qAbs(details->width() - expectedWidth) <= 1);
        QVERIFY(qAbs(details->geometry().right() - splitter->contentsRect().right()) <= 1);
        const int detailsWidth = details->width();
        QCOMPARE(actions->height(), 44);
        if (qEnvironmentVariableIsSet("UDRIVE_SHARES_TEST_CAPTURE"))
            window.grab().save(QStringLiteral("/tmp/udrive-details-full-%1.png").arg(dark ? "dark" : "light"));
        actions->click();
        QTest::qWait(50);
        QCOMPARE(details->width(), detailsWidth);
        auto *page = browser->findChild<QWidget *>("driveDetailsActionsPage");
        int lastBottom = -1;
        int left = -1;
        int width = -1;
        for (auto *button : page->findChildren<QPushButton *>(QString(), Qt::FindDirectChildrenOnly)) {
            if (!button->isVisible()) continue;
            QCOMPARE(button->height(), 44);
            if (left < 0) { left = button->x(); width = button->width(); }
            QCOMPARE(button->x(), left);
            QCOMPARE(button->width(), width);
            if (lastBottom >= 0) QVERIFY(button->y() - lastBottom >= 6);
            lastBottom = button->geometry().bottom();
        }
        QVERIFY(lastBottom > 0);
        if (qEnvironmentVariableIsSet("UDRIVE_SHARES_TEST_CAPTURE"))
            window.grab().save(QStringLiteral("/tmp/udrive-actions-aligned-%1.png").arg(dark ? "dark" : "light"));
        browser->findChild<QToolButton *>("driveDetailsBack")->click();
        QCOMPARE(details->width(), detailsWidth);
        browser->findChild<QToolButton *>("driveDetailsClose")->click();
        openDetails->trigger();
        QTRY_VERIFY(details->isVisible());
        QCOMPARE(details->width(), detailsWidth);
        if (qEnvironmentVariableIsSet("UDRIVE_SHARES_TEST_CAPTURE")) {
            browser->findChild<QPushButton *>("driveDetailsShares")->click();
            QPointer<QWidget> emptyShare = browser->findChild<QWidget *>("udriveShareDialog");
            QVERIFY(emptyShare);
            QTRY_VERIFY(!server.shareQueries.isEmpty());
            QTest::qWait(50);
            window.grab().save(QStringLiteral("/tmp/udrive-shares-empty-%1.png").arg(dark ? "dark" : "light"));
            browser->findChild<QToolButton *>("driveDetailsBack")->click();
            QTRY_VERIFY(!emptyShare);
        }
        server.existingShares = QJsonArray{
            QJsonObject{{"id", "person-1"}, {"share_type", 0}, {"share_with_displayname", "Alice Example"}, {"permissions", 1}},
            QJsonObject{{"id", "public-1"}, {"share_type", 3}, {"name", "Review link"}, {"permissions", 1}, {"url", server.url("/s/review").toString()}}};
        browser->findChild<QPushButton *>("driveDetailsShares")->click();
        auto *share = static_cast<UDriveShareDialog *>(browser->findChild<QWidget *>("udriveShareDialog"));
        QVERIFY(share && share->isVisible() && !share->isWindow() && !share->isModal());
        QTRY_VERIFY(share->findChild<QComboBox *>("sharePersonRoleCombo"));
        QTRY_COMPARE(share->findChild<QScrollArea *>("sharesScrollArea")->verticalScrollBar()->maximum(), 0);
        QTRY_COMPARE(share->findChild<QScrollArea *>("publicLinksScrollArea")->verticalScrollBar()->maximum(), 0);
        QCOMPARE(details->width(), detailsWidth);
        QVERIFY(share->width() <= details->viewport()->width());
        QVERIFY(share->findChild<QLineEdit *>("shareUserInput")->width() >= 120);
        auto *shareRole = share->findChild<QComboBox *>("shareRoleCombo");
        QVERIFY(shareRole->fontMetrics().horizontalAdvance(shareRole->currentText()) + 48 <= shareRole->width());
        QCOMPARE(browser->findChild<QLabel *>("driveDetailsTitle")->text(), QStringLiteral("Shares"));
        if (qEnvironmentVariableIsSet("UDRIVE_SHARES_TEST_CAPTURE"))
            window.grab().save(QStringLiteral("/tmp/udrive-shares-sidebar-%1.png").arg(dark ? "dark" : "light"));
        // The access selector lives inside a transparent scroll area. Its menu
        // must paint an opaque surface just like the invite and public-link menus.
        for (const char *name : {"shareRoleCombo", "sharePersonRoleCombo", "publicLinkRoleCombo"}) {
            auto *combo = share->findChild<QComboBox *>(QLatin1String(name));
            QVERIFY(combo);
            window.activateWindow();
            QApplication::setActiveWindow(&window);
            combo->showPopup();
            auto *view = combo->view();
            auto *menu = view->window();
            QTRY_VERIFY(menu->isVisible());
            QTest::qWait(20);
            QVERIFY(!menu->testAttribute(Qt::WA_TranslucentBackground));
            QVERIFY(menu->width() >= combo->width());
            QVERIFY(menu->height() <= 120);
            QVERIFY(QRect(window.mapToGlobal(QPoint()), window.size()).contains(menu->geometry()));
            QCOMPARE(view->viewport()->grab().toImage().pixelColor(1, 1), QColor(dark ? "#1F2023" : "#FFFFFF"));
            QVERIFY(view->visualRect(combo->model()->index(0, 0)).height() >= 32);
            QVERIFY(view->visualRect(combo->model()->index(1, 0)).bottom() < view->viewport()->height());
            if (qEnvironmentVariableIsSet("UDRIVE_SHARES_TEST_CAPTURE")) {
                QPixmap capture = window.grab();
                QPainter painter(&capture);
                painter.drawPixmap(menu->mapToGlobal(QPoint()) - window.mapToGlobal(QPoint()), menu->grab());
                painter.end();
                capture.save(QStringLiteral("/tmp/udrive-permission-%1-%2.png").arg(QLatin1String(name), dark ? "dark" : "light"));
            }
            QTest::keyClick(view, Qt::Key_Escape);
            QTRY_VERIFY(!menu->isVisible());
            QCOMPARE(combo->currentData().toInt(), 1);
        }
        QCOMPARE(server.sharePuts.size(), 0);
        server.recipientData = QJsonObject{{"users", QJsonArray{
            QJsonObject{{"label", "Seng Porekeat"}, {"value", QJsonObject{{"shareType", 0}, {"shareWith", "porekeat"}}}},
            QJsonObject{{"label", "Por Keat"}, {"value", QJsonObject{{"shareType", 0}, {"shareWith", "porekeatv2"}}}}}}};
        auto *input = share->findChild<QLineEdit *>("shareUserInput");
        auto *completer = share->findChild<QCompleter *>();
        window.activateWindow();
        QApplication::setActiveWindow(&window);
        QTest::mouseClick(input, Qt::LeftButton);
        input->setFocus();
        QTRY_VERIFY(input->hasFocus());
        input->setText("por");
        auto *popup = completer->popup();
        QTRY_COMPARE(completer->completionModel()->rowCount(), 2);
        // Native activation can change while the fake server responds. Exercise
        // popup rendering explicitly; the recipient-search tests cover auto-open.
        QApplication::setActiveWindow(&window);
        input->setFocus();
        completer->complete(input->rect().translated(0, 4));
        QTRY_VERIFY(popup->isVisible());
        QCOMPARE(completer->completionModel()->rowCount(), 2);
        QCOMPARE(popup->width(), input->width());
        QVERIFY(!popup->testAttribute(Qt::WA_TranslucentBackground));
        QVERIFY(popup->y() >= input->mapToGlobal(QPoint(0, input->height())).y());
        QVERIFY(QRect(window.mapToGlobal(QPoint()), window.size()).contains(popup->geometry()));
        const auto surface = popup->viewport()->grab().toImage();
        QCOMPARE(surface.pixelColor(1, 1), QColor(dark ? "#1F2023" : "#FFFFFF"));
        if (qEnvironmentVariableIsSet("UDRIVE_SHARES_TEST_CAPTURE")) {
            QPixmap capture = window.grab();
            QPainter painter(&capture);
            painter.drawPixmap(popup->mapToGlobal(QPoint()) - window.mapToGlobal(QPoint()), popup->grab());
            painter.end();
            capture.save(QStringLiteral("/tmp/udrive-recipient-sidebar-%1.png").arg(dark ? "dark" : "light"));
        }
        QTest::keyClick(popup, Qt::Key_Down);
        QTest::keyClick(popup, Qt::Key_Return);
        QTRY_VERIFY(!popup->isVisible());
        QCOMPARE(input->text(), QStringLiteral("Seng Porekeat"));
        QVERIFY(share->findChild<QPushButton *>("shareConfirmButton")->isEnabled());
        QCOMPARE(server.sharePosts, 0);
        browser->findChild<QToolButton *>("driveDetailsBack")->click();
        QVERIFY(browser->findChild<QWidget *>("driveDetailsOverview")->isVisible());
    }

    void fileSharesSidebar()
    {
        DriveServer server;
        server.existingShares = {QJsonObject{{"id", "person-1"}, {"share_type", 0}, {"share_with_displayname", "Alice <Admin>"}, {"permissions", 1}}};
        server.recipientData = QJsonObject{{"users", QJsonArray{QJsonObject{{"label", "Carol"},
            {"value", QJsonObject{{"shareType", 0}, {"shareWith", "carol-id"}}}}}}};
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveFileBrowser browser(&state);
        browser.resize(1160, 850); browser.show();
        auto *tree = browser.findChild<QTreeView *>("driveFiles");
        QTRY_COMPARE(tree->model()->rowCount(), 7);
        QVERIFY(select(tree, "a&b.txt").isValid());
        browser.findChild<QAction *>("driveShareAction")->trigger();
        auto *panel = browser.findChild<UDriveShareDialog *>();
        QVERIFY(panel && panel->isVisible() && !panel->isWindow());
        QVERIFY(!QApplication::activeModalWidget());
        QTRY_COMPARE(server.shareQueries.size(), 1);
        QCOMPARE(server.shareQueries.first().queryItemValue("path", QUrl::FullyDecoded), QStringLiteral("/a&b.txt"));
        QCOMPARE(server.shareQueries.first().queryItemValue("space_ref", QUrl::FullyDecoded), QStringLiteral("personal/a&b.txt"));
        auto *input = panel->findChild<QLineEdit *>("shareUserInput");
        auto *confirm = panel->findChild<QPushButton *>("shareConfirmButton");
        auto *status = panel->findChild<QLabel *>("shareStatusLabel");
        auto *completer = panel->findChild<QCompleter *>();
        panel->findChild<QPushButton *>("shareCopyLinkButton")->click();
        QCOMPARE(qApp->clipboard()->text(), server.url("/f/dav/personal/a&b.txt").toString());
        QCOMPARE(server.sharePosts, 0);
        input->setText("carol");
        QVERIFY(!confirm->isEnabled());
        QTRY_COMPARE(completer->completionModel()->rowCount(), 1);
        Q_EMIT completer->activated(completer->completionModel()->index(0, 0));
        QVERIFY(confirm->isEnabled());
        panel->findChild<QComboBox *>("shareRoleCombo")->setCurrentIndex(1);
        confirm->click();
        QTRY_COMPARE(server.sharePosts, 1);
        QTRY_VERIFY(status->text().contains("Successfully shared"));
        const QUrlQuery invite(QString::fromUtf8(server.requestBodies.value("POST /ocs/v1.php/apps/files_sharing/api/v1/shares")));
        QCOMPARE(invite.queryItemValue("shareWith"), QStringLiteral("carol-id"));
        QCOMPARE(invite.queryItemValue("permissions"), QStringLiteral("15"));
        QCOMPARE(invite.queryItemValue("space_ref", QUrl::FullyDecoded), QStringLiteral("personal/a&b.txt"));
        QTRY_COMPARE(server.shareQueries.size(), 2);
        QTRY_VERIFY(panel->findChild<QComboBox *>("sharePersonRoleCombo"));
        auto *role = panel->findChild<QComboBox *>("sharePersonRoleCombo");
        role->setCurrentIndex(1); Q_EMIT role->activated(1);
        QTRY_COMPARE(server.sharePuts.size(), 1);
        QTRY_VERIFY(status->text() == QStringLiteral("Access updated."));
        QTRY_COMPARE(server.shareQueries.size(), 3);
        QTRY_COMPARE(panel->findChild<QComboBox *>("sharePersonRoleCombo")->currentData().toInt(), 15);
        panel->findChild<QPushButton *>("shareRemovePerson")->click();
        QTRY_COMPARE(server.shareDeletions.size(), 1);
        QTRY_COMPARE(server.shareQueries.size(), 4);
        panel->findChild<QPushButton *>("shareAddLinkButton")->click();
        QTRY_COMPARE(server.sharePosts, 2);
        QTRY_VERIFY(panel->findChild<QComboBox *>("publicLinkRoleCombo"));
        QTRY_VERIFY(browser.findChild<QLabel *>("driveDetailsPeople")->text().contains("public link"));
        QVERIFY(qApp->clipboard()->text().contains("/s/public-share-1"));
        auto buttons = panel->findChildren<QToolButton *>("publicLinkActionButton");
        QCOMPARE(buttons.size(), 1);
        buttons.at(0)->click();
        QCOMPARE(qApp->clipboard()->text(), server.url("/s/public-share-1").toString());
        panel->findChild<QAction *>("publicLinkDelete")->trigger();
        QTRY_COMPARE(server.shareDeletions.size(), 2);
        QTRY_VERIFY(server.existingShares.isEmpty());
        QTRY_VERIFY(browser.findChild<QLabel *>("driveDetailsPeople")->text().contains("Private"));
        browser.findChild<QToolButton *>("driveDetailsBack")->click();
        QVERIFY(browser.findChild<QWidget *>("driveDetailsOverview")->isVisible());
        QVERIFY(browser.findChild<QLabel *>("driveDetailsPeople")->text().contains("Private"));
    }

    void sharesSidebarCancelsOnExit_data()
    {
        QTest::addColumn<QString>("mode");
        for (const auto &mode : {"back", "close", "select", "navigate", "hide", "signout", "destroy"})
            QTest::newRow(mode) << QString::fromLatin1(mode);
    }

    void sharesSidebarCancelsOnExit()
    {
        QFETCH(QString, mode);
        DriveServer server;
        server.recipientDelay = 500;
        server.recipientData = QJsonObject{{"users", QJsonArray{QJsonObject{{"label", "Alice"},
            {"value", QJsonObject{{"shareType", 0}, {"shareWith", "alice-id"}}}}}}};
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        auto browser = std::make_unique<UDriveFileBrowser>(&state);
        browser->resize(1160, 850); browser->show();
        auto *tree = browser->findChild<QTreeView *>("driveFiles");
        QTRY_COMPARE(tree->model()->rowCount(), 7);
        QVERIFY(select(tree, "a&b.txt").isValid());
        browser->findChild<QAction *>("driveShareAction")->trigger();
        QPointer<UDriveShareDialog> panel = browser->findChild<UDriveShareDialog *>();
        QVERIFY(panel);
        panel->findChild<QLineEdit *>("shareUserInput")->setText("alice");
        QTRY_COMPARE(server.recipientQueries.size(), 1);
        if (mode == "back") browser->findChild<QToolButton *>("driveDetailsBack")->click();
        else if (mode == "close") browser->findChild<QToolButton *>("driveDetailsClose")->click();
        else if (mode == "select") QVERIFY(select(tree, "denied.txt").isValid());
        else if (mode == "navigate") browser->showFavorites();
        else if (mode == "hide") browser->hide();
        else if (mode == "signout") state.signOutByUi();
        else browser.reset();
        QTRY_VERIFY(!panel);
        QTest::qWait(550);
        QCOMPARE(server.sharePosts, 0);
        QCOMPARE(server.recipientQueries.size(), 1);
        QVERIFY(!QApplication::activeModalWidget());
        if (browser && mode != "signout" && mode != "hide" && mode != "navigate") {
            QVERIFY(select(tree, "denied.txt").isValid());
            browser->findChild<QAction *>("driveShareAction")->trigger();
            auto *next = browser->findChild<UDriveShareDialog *>();
            QVERIFY(next && next->isVisible());
            QVERIFY(next->findChild<QLineEdit *>("shareUserInput")->text().isEmpty());
            QTRY_VERIFY(server.shareQueries.last().queryItemValue("path", QUrl::FullyDecoded) == QStringLiteral("/denied.txt"));
        }
    }

    void actionResultToasts_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }

    void actionResultToasts()
    {
        QFETCH(bool, dark);
        const QPalette previous = qApp->palette();
        const auto restore = qScopeGuard([&] { qApp->setPalette(previous); });
        auto palette = previous;
        palette.setColor(QPalette::Window, QColor(dark ? "#292929" : "#FFFFFF"));
        palette.setColor(QPalette::WindowText, QColor(dark ? "#EDEEF0" : "#202923"));
        qApp->setPalette(palette);
        DriveServer server;
        server.existingShares = {QJsonObject{{"id", "person-1"}, {"share_type", 0}, {"share_with_displayname", "Alice"}, {"permissions", 1}}};
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        QWidget window;
        auto *layout = new QVBoxLayout(&window);
        auto *browser = new UDriveFileBrowser(&state, &window);
        layout->addWidget(browser);
        window.resize(1180, 820);
        window.show();
        auto *tree = browser->findChild<QTreeView *>("driveFiles");
        QTRY_COMPARE(tree->model()->rowCount(), 7);
        QVERIFY(!window.findChild<QFrame *>("udriveToast"));
        browser->findChild<QAction *>("driveViewCondensed")->trigger();
        QPointer<QFrame> notice = window.findChild<QFrame *>("udriveToast");
        QVERIFY(notice && notice->isVisible());
        QCOMPARE(notice->findChild<QLabel *>("udriveToastMessage")->text(), QStringLiteral("Preference saved."));
        QVERIFY(window.contentsRect().contains(notice->geometry()));
        QVERIFY(window.grab().save(dark ? "/tmp/udrive-toast-dark.png" : "/tmp/udrive-toast-light.png"));
        QVERIFY(notice->grab().save(dark ? "/tmp/udrive-toast-dark-detail.png" : "/tmp/udrive-toast-light-detail.png"));
        notice->findChild<QToolButton *>("udriveToastClose")->click();
        QTRY_VERIFY(!window.findChild<QFrame *>("udriveToast"));
        QVERIFY(select(tree, "a&b.txt").isValid());
        browser->findChild<QAction *>("driveShareAction")->trigger();
        auto *panel = browser->findChild<UDriveShareDialog *>();
        QVERIFY(panel);
        QTRY_VERIFY(panel->findChild<QComboBox *>("sharePersonRoleCombo"));
        auto *input = panel->findChild<QLineEdit *>("shareUserInput");
        input->setFocus();
        auto *role = panel->findChild<QComboBox *>("sharePersonRoleCombo");
        role->setCurrentIndex(1);
        Q_EMIT role->activated(1);
        QTRY_COMPARE(server.shareQueries.size(), 2);
        notice = window.findChild<QFrame *>("udriveToast");
        QVERIFY(notice && notice->isVisible());
        QCOMPARE(notice->property("noticeKind").toString(), QStringLiteral("success"));
        QCOMPARE(notice->findChild<QLabel *>("udriveToastMessage")->text(), QStringLiteral("Access updated."));
        QVERIFY(panel->findChild<QLabel *>("shareStatusLabel")->isHidden());
        QVERIFY(!notice->isAncestorOf(QApplication::focusWidget()));
        window.resize(800, 650);
        QTRY_VERIFY(window.contentsRect().contains(notice->geometry()));
        server.sharePutStatus = 403;
        role = panel->findChild<QComboBox *>("sharePersonRoleCombo");
        role->setCurrentIndex(0);
        Q_EMIT role->activated(0);
        QTRY_COMPARE(server.shareQueries.size(), 3);
        notice = window.findChild<QFrame *>("udriveToast");
        QVERIFY(notice && notice->isVisible());
        QCOMPARE(notice->property("noticeKind").toString(), QStringLiteral("error"));
        QVERIFY(notice->findChild<QLabel *>("udriveToastMessage")->text().contains("Could not update access"));
        browser->hide();
        QTRY_VERIFY(!notice || !notice->isVisible());
    }

    void toastTimeoutAndPlainText()
    {
        QWidget window;
        QVBoxLayout layout(&window);
        QLineEdit input;
        layout.addWidget(&input);
        window.resize(500, 300);
        window.show();
        input.setFocus();
        QTRY_COMPARE(QApplication::focusWidget(), &input);
        QTest::mouseMove(&window, QPoint(5, 5));
        const QString message = QStringLiteral("Saved <b>Report.txt</b> & notes.");
        UDriveToast::showMessage(&window, message);
        QPointer<QFrame> notice = window.findChild<QFrame *>("udriveToast");
        QVERIFY(notice);
        auto *text = notice->findChild<QLabel *>("udriveToastMessage");
        QCOMPARE(text->text(), message);
        QCOMPARE(text->textFormat(), Qt::PlainText);
        QCOMPARE(QApplication::focusWidget(), &input);

        // Capture Success Toast (matching user scenario: Public link copied to clipboard)
        UDriveToast::showMessage(&window, QStringLiteral("Public link copied to clipboard."), UDriveToast::Kind::Success);
        notice = window.findChild<QFrame *>("udriveToast");
        QVERIFY(notice);
        QTest::qWait(50);
        notice->grab().save(QStringLiteral("/Users/tochratana/.gemini/antigravity/brain/00705d74-3dbe-4bb0-962c-114a4aadd1a1/toast-centered.png"));

        UDriveToast::dismiss(&window);
        UDriveToast::showMessage(&window, message);
        notice = window.findChild<QFrame *>("udriveToast");
        QVERIFY(notice);
        QTRY_VERIFY_WITH_TIMEOUT(!notice, 7500);
        UDriveToast::showMessage(&window, QStringLiteral("Could not save."), UDriveToast::Kind::Error);
        notice = window.findChild<QFrame *>("udriveToast");
        QTest::qWait(6500);
        QVERIFY(notice && notice->isVisible());
        notice->findChild<QToolButton *>("udriveToastClose")->click();
        QTRY_VERIFY(!notice);
    }

    void shareIdsRemainIntact_data()
    {
        QTest::addColumn<QString>("shareId");
        QTest::addColumn<int>("shareType");
        for (int type : {0, 3}) {
            for (const auto &id : {QStringLiteral("person-1"), QStringLiteral("storage:space:share"),
                     QStringLiteral("literal%3Aid"), QStringLiteral("share?query#fragment")}) {
                QTest::newRow(qPrintable(QString::number(type) + QLatin1Char(':') + id)) << id << type;
            }
        }
    }

    void shareIdsRemainIntact()
    {
        QFETCH(QString, shareId);
        QFETCH(int, shareType);
        DriveServer server;
        server.rejectUnknownShareId = true;
        server.existingShares = {QJsonObject{{"id", shareId}, {"share_type", shareType},
            {"share_with_displayname", "Alice"}, {"permissions", 1}, {"url", server.url("/s/test-link").toString()}}};
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveShareDialog panel(&state, "Report.pdf", false, "/Report.pdf", {}, "personal", nullptr,
            UDriveShareDialog::Target::File, UDriveShareDialog::Presentation::Sidebar);
        panel.show();
        const auto roleName = shareType == 3 ? "publicLinkRoleCombo" : "sharePersonRoleCombo";
        QTRY_VERIFY(panel.findChild<QComboBox *>(roleName));
        auto *role = panel.findChild<QComboBox *>(roleName);
        role->setCurrentIndex(1);
        Q_EMIT role->activated(1);
        QTRY_COMPARE(server.sharePuts.size(), 1);
        QTRY_COMPARE(server.shareQueries.size(), 2);
        QCOMPARE(server.existingShares.first().toObject().value("permissions").toInt(), 15);
        QTRY_COMPARE(panel.findChild<QComboBox *>(roleName)->currentData().toInt(), 15);
        const QString endpoint = QStringLiteral("/ocs/v1.php/apps/files_sharing/api/v1/shares/") + shareId;
        QVERIFY(server.requests.contains({"PUT", endpoint}));
        if (shareType == 3) panel.findChild<QAction *>("publicLinkDelete")->trigger();
        else panel.findChild<QPushButton *>("shareRemovePerson")->click();
        QTRY_COMPARE(server.shareDeletions.size(), 1);
        QCOMPARE(server.shareDeletions.first(), shareId);
        QTRY_VERIFY(server.existingShares.isEmpty());
        QVERIFY(server.requests.contains({"DELETE", endpoint}));
    }

    void sharePermissionFailureRestoresRole()
    {
        DriveServer server;
        server.existingShares = {QJsonObject{{"id", "person-1"}, {"share_type", 0}, {"share_with_displayname", "Alice"}, {"permissions", 1}}};
        server.sharePutStatus = 403;
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveShareDialog panel(&state, "Report.pdf", false, "/Report.pdf", {}, "personal", nullptr,
            UDriveShareDialog::Target::File, UDriveShareDialog::Presentation::Sidebar);
        panel.show();
        QTRY_VERIFY(panel.findChild<QComboBox *>("sharePersonRoleCombo"));
        auto *role = panel.findChild<QComboBox *>("sharePersonRoleCombo");
        role->setCurrentIndex(1); Q_EMIT role->activated(1);
        QTRY_COMPARE(server.sharePuts.size(), 1);
        QTRY_COMPARE(server.shareQueries.size(), 2);
        QTRY_COMPARE(panel.findChild<QComboBox *>("sharePersonRoleCombo")->currentData().toInt(), 1);
        QVERIFY(panel.findChild<QLabel *>("shareStatusLabel")->text().contains("Could not update access"));
    }

    void sharesLayoutAndActions()
    {
        DriveServer server(true);
        const auto person = [](const QString &id, const QString &name) {
            return QJsonObject{{"user", QJsonObject{{"id", id}, {"displayName", name}}}};
        };
        const QJsonObject grant{{"grantedToV2", QJsonArray{person("sp", "Seng Porekeat"), person("ty", "Toch Vanna")}},
            {"roles", QJsonArray{"owner"}}};
        const auto item = [&grant](const QString &name, const QString &id) {
            const QString itemId = QStringLiteral("personal!") + id;
            return QJsonObject{{"id", itemId}, {"name", name}, {"file", QJsonObject{}}, {"size", 1234},
                {"lastModifiedDateTime", "2026-10-16T08:00:00Z"}, {"permissions", QJsonArray{grant}},
                {"parentReference", QJsonObject{{"driveId", "personal"}, {"path", "."}}}};
        };
        server.outgoingShares = {item("a&b.txt", "one"), item("denied.txt", "two"), item("truncated.txt", "three")};
        // Even an owner's grant to a recipient must not grant the signed-in user rename rights.
        server.itemPermissions.insert("/dav/personal/denied.txt", "S");
        server.itemPermissions.insert("/dav/personal/truncated.txt", "");
        server.itemPermissionDelay = 120;
        auto *account = new Account(QUuid::createUuid(), QStringLiteral("test"), server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveFileBrowser browser(&state);
        browser.resize(1160, 800); browser.show();
        auto *tree = browser.findChild<QTreeView *>("driveFiles");
        auto *menu = browser.findChild<QMenu *>("driveActionsMenu");
        auto *details = browser.findChild<QWidget *>("driveFileDetails");
        auto *detailsAction = browser.findChild<QAction *>("driveDetailsAction");
        auto *close = browser.findChild<QToolButton *>("driveDetailsClose");
        auto *name = browser.findChild<QLabel *>("driveDetailsName");
        auto *people = browser.findChild<QLabel *>("driveDetailsPeople");
        QVERIFY(tree && menu && details && detailsAction && close && name && people);
        QTRY_COMPARE(tree->model()->rowCount(), 7);
        browser.showShares(); browser.setShareTab(UDriveFileBrowser::ShareTab::WithOthers);
        QTRY_COMPARE(tree->model()->rowCount(), 3);
        QCOMPARE(tree->model()->headerData(3, Qt::Horizontal).toString(), QStringLiteral("Shared with"));
        QVERIFY(tree->isColumnHidden(1)); QVERIFY(tree->isColumnHidden(2)); QVERIFY(!tree->isColumnHidden(3));
        QVERIFY(select(tree, "a&b.txt").isValid());
        QTRY_VERIFY(details->isVisible());
        QCOMPARE(name->text(), QStringLiteral("a&b.txt"));
        QVERIFY(people->text().contains("Seng Porekeat")); QVERIFY(people->text().contains("Toch Vanna"));
        QVERIFY(browser.findChild<QLabel *>("driveDetailsLocation")->text().contains("Personal"));
        auto *detailsOpen = browser.findChild<QPushButton *>("driveDetailsOpen");
        auto *actionsHeader = browser.findChild<QPushButton *>("driveDetailsActions");
        auto *back = browser.findChild<QToolButton *>("driveDetailsBack");
        auto *actionsPage = browser.findChild<QWidget *>("driveDetailsActionsPage");
        auto *overviewPage = browser.findChild<QWidget *>("driveDetailsOverview");
        QVERIFY(detailsOpen && actionsHeader && back && actionsPage && overviewPage);
        QVERIFY(overviewPage->isVisible());
        QVERIFY(!actionsPage->isVisible());
        QVERIFY(!back->isVisible());
        actionsHeader->click();
        QVERIFY(actionsPage->isVisible());
        QVERIFY(!overviewPage->isVisible());
        QVERIFY(back->isVisible());
        QVERIFY(detailsOpen->isVisible());
        QCOMPARE(detailsOpen->text(), QStringLiteral("Preview"));
        QCOMPARE(details->accessibleName(), QStringLiteral("File actions"));
        QVERIFY(!menu->isVisible());
        QCOMPARE(name->text(), QStringLiteral("a&b.txt"));
        if (qEnvironmentVariableIsSet("UDRIVE_SHARES_TEST_CAPTURE")) {
            QTest::qWait(50);
            browser.grab().save("/tmp/udrive-actions-sidebar.png");
        }
        back->click();
        QVERIFY(overviewPage->isVisible());
        QVERIFY(!actionsPage->isVisible());
        QCOMPARE(details->accessibleName(), QStringLiteral("File details"));
        QVERIFY(browser.findChild<QFrame *>("driveDetailsCard"));
        const auto rename = [&menu] {
            for (auto *action : menu->actions()) if (action->text() == QStringLiteral("Rename")) return action;
            return static_cast<QAction *>(nullptr);
        }();
        QVERIFY(rename); QVERIFY(!rename->isEnabled());
        QTRY_VERIFY(rename->isEnabled());
        QCOMPARE(server.requestHeaders.value("PROPFIND /dav/personal/a&b.txt").value("depth"), QByteArray("0"));
        menu->popup(tree->viewport()->mapToGlobal(tree->visualRect(tree->currentIndex()).center()));
        QTRY_VERIFY(menu->isVisible());
        QStringList labels;
        for (auto *action : menu->actions()) if (action->isVisible() && !action->isSeparator()) {
            labels.append(action->text()); QVERIFY(!action->icon().isNull());
            QVERIFY(!action->isShortcutVisibleInContextMenu());
        }
        QCOMPARE(labels, QStringList({"Preview", "Share", "Copy permanent link", "Download", "Rename", "Details"}));
        menu->close();
        actionsHeader->click(); QVERIFY(actionsPage->isVisible());
        close->click(); QVERIFY(details->isHidden());
        detailsAction->trigger(); QVERIFY(details->isVisible());
        QVERIFY(overviewPage->isVisible());
        QSignalSpy operation(&browser, &UDriveFileBrowser::operationFinished);
        QVERIFY(browser.renameSelected("renamed.txt"));
        QTRY_VERIFY(server.requests.contains({"MOVE", "/dav/personal/a&b.txt"}));
        QTRY_COMPARE(operation.size(), 1);
        QTRY_COMPARE(tree->model()->rowCount(), 3);
        QCOMPARE(server.requestHeaders.value("MOVE /dav/personal/a&b.txt").value("overwrite"), QByteArray("F"));
        QVERIFY(select(tree, "denied.txt").isValid());
        QTest::qWait(160); QVERIFY(!rename->isEnabled());
        actionsHeader->click();
        auto *panelRename = browser.findChild<QPushButton *>("driveDetailsRename");
        QVERIFY(panelRename->isVisible());
        QVERIFY(!panelRename->isEnabled());
        QVERIFY(!browser.renameSelected("forbidden.txt"));
        QVERIFY(select(tree, "truncated.txt").isValid());
        QVERIFY(overviewPage->isVisible());
        QVERIFY(!actionsPage->isVisible());
        QTest::qWait(160); QVERIFY(!rename->isEnabled());
        QVERIFY(!browser.renameSelected("missing-permissions.txt"));

        // A stale response for the old row cannot enable actions on a new selection.
        browser.findChild<QPushButton *>("driveRefresh")->click();
        QTRY_COMPARE(tree->model()->rowCount(), 3);
        QVERIFY(select(tree, "a&b.txt").isValid());
        QVERIFY(select(tree, "denied.txt").isValid());
        QTest::qWait(180);
        QCOMPARE(name->text(), QStringLiteral("denied.txt")); QVERIFY(!rename->isEnabled());
        if (qEnvironmentVariableIsSet("UDRIVE_SHARES_TEST_CAPTURE")) {
            browser.grab().save("/tmp/udrive-shares-layout.png");
            menu->popup(tree->viewport()->mapToGlobal(tree->visualRect(tree->currentIndex()).center()));
            QTest::qWait(50); menu->grab().save("/tmp/udrive-shares-menu.png"); menu->close();
        }
        server.itemPermissionStatus = 403;
        browser.findChild<QPushButton *>("driveRefresh")->click();
        QTRY_COMPARE(tree->model()->rowCount(), 3);
        QVERIFY(select(tree, "a&b.txt").isValid());
        QTest::qWait(180); QVERIFY(!rename->isEnabled());
        server.itemPermissionStatus = 207;
        server.itemPermissionHref = "/dav/personal/somebody-else.txt";
        browser.findChild<QPushButton *>("driveRefresh")->click();
        QTRY_COMPARE(tree->model()->rowCount(), 3);
        QVERIFY(select(tree, "a&b.txt").isValid());
        QTest::qWait(180); QVERIFY(!rename->isEnabled());
        browser.showPersonal();
        QTRY_COMPARE(tree->model()->rowCount(), 7);
        QVERIFY(details->isHidden());
        QVERIFY(!tree->isColumnHidden(1)); QVERIFY(!tree->isColumnHidden(2));
        QCOMPARE(tree->model()->headerData(3, Qt::Horizontal).toString(), QStringLiteral("Location"));
        QVERIFY(select(tree, "Docs").isValid());
        detailsAction->trigger();
        QVERIFY(overviewPage->isVisible());
        actionsHeader->click();
        QVERIFY(actionsPage->isVisible());
        QCOMPARE(detailsOpen->text(), QStringLiteral("Open folder"));
        QVERIFY(!browser.findChild<QPushButton *>("driveDetailsDownload")->isEnabled());
        auto *favoriteButton = browser.findChild<QPushButton *>("driveDetailsFavorite");
        QVERIFY(favoriteButton->isVisible() && favoriteButton->isEnabled());
        QCOMPARE(favoriteButton->text(), QStringLiteral("Add to Favorites"));
        if (qEnvironmentVariableIsSet("UDRIVE_SHARES_TEST_CAPTURE")) {
            QTest::qWait(50);
            browser.grab().save("/tmp/udrive-actions-sidebar-personal.png");
        }
        operation.clear();
        favoriteButton->click();
        QTRY_COMPARE(operation.size(), 1);
        QVERIFY(operation.first().first().toBool());
        QVERIFY(server.favorites.contains(QStringLiteral("/dav/personal/Docs")));
        QTRY_COMPARE(tree->model()->rowCount(), 7);
        QVERIFY(select(tree, "Docs").isValid());
        QCOMPARE(favoriteButton->text(), QStringLiteral("Remove from Favorites"));
    }

    void outgoingAndIncomingShares()
    {
        DriveServer server(true);
        const auto sharedItem = [](const QString &drive, const QString &name, const QString &id, bool folder = false) {
            QJsonObject item{{"id", QStringLiteral("%1!%2").arg(drive, id)}, {"name", name}, {"size", 1234},
                {"eTag", "shared-tag"}, {"lastModifiedDateTime", "2026-10-16T08:00:00Z"},
                {"permissions", QJsonArray{QJsonObject{{"invitation", QJsonObject{}}}}},
                {"parentReference", QJsonObject{{"driveId", drive}, {"path", "."}}}};
            item.insert(folder ? QStringLiteral("folder") : QStringLiteral("file"), QJsonObject{});
            return item;
        };
        const auto personalFile = sharedItem(QStringLiteral("personal"), QStringLiteral("a&b.txt"), QStringLiteral("file-a"));
        const auto teamFile = sharedItem(QStringLiteral("team"), QStringLiteral("Roadmap.pdf"), QStringLiteral("file-b"));
        server.outgoingShares = {personalFile, personalFile, teamFile}; // One row per file, not per recipient.
        auto *account = new Account(QUuid::createUuid(), QStringLiteral("test"), server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveFileBrowser browser(&state);
        browser.resize(1150, 800);
        browser.show();
        auto *tree = browser.findChild<QTreeView *>(QStringLiteral("driveFiles"));
        auto *scope = browser.findChild<QPushButton *>(QStringLiteral("driveShareScope"));
        auto *outgoing = browser.findChild<QAction *>(QStringLiteral("driveSharedByMe"));
        auto *incoming = browser.findChild<QAction *>(QStringLiteral("driveSharedWithMe"));
        auto *refresh = browser.findChild<QPushButton *>(QStringLiteral("driveRefresh"));
        auto *search = browser.findChild<QLineEdit *>(QStringLiteral("driveSearch"));
        auto *status = browser.findChild<QLabel *>(QStringLiteral("driveStatus"));
        QTRY_COMPARE(tree->model()->rowCount(), 7);
        QVERIFY(!scope->isVisible());
        browser.showShares();
        auto *sharesNav = browser.findChild<QWidget *>(QStringLiteral("driveSharesNav"));
        auto *sharesControls = browser.findChild<QWidget *>(QStringLiteral("driveSharesControls"));
        auto *withMe = browser.findChild<QPushButton *>(QStringLiteral("driveSharedWithMeButton"));
        auto *withOthers = browser.findChild<QPushButton *>(QStringLiteral("driveSharedWithOthersButton"));
        auto *viaLink = browser.findChild<QPushButton *>(QStringLiteral("driveSharedViaLinkButton"));
        auto *typeFilter = browser.findChild<QComboBox *>(QStringLiteral("driveShareTypeFilter"));
        auto *shareSearch = browser.findChild<QLineEdit *>(QStringLiteral("driveShareSearch"));
        auto *emptyState = browser.findChild<QWidget *>(QStringLiteral("driveEmptyState"));
        QVERIFY(sharesNav && sharesControls && withMe && withOthers && viaLink && typeFilter && shareSearch && emptyState);
        QVERIFY(sharesNav->isVisible());
        QVERIFY(sharesControls->isVisible());
        QVERIFY(withMe->isChecked());
        QTRY_COMPARE(tree->model()->rowCount(), 1);
        QCOMPARE(tree->model()->index(0, 0).data().toString(), QStringLiteral("Shared document.pdf"));
        QCOMPARE(scope->text(), QStringLiteral("Shared with me"));
        outgoing->trigger();
        QTRY_COMPARE(tree->model()->rowCount(), 2);
        QVERIFY(scope->isVisible());
        QVERIFY(outgoing->isChecked());
        QVERIFY(withOthers->isChecked());
        QCOMPARE(scope->text(), QStringLiteral("Shared by me"));
        // Two account-scoped browsers must not reuse each other's outgoing list.
        DriveServer otherServer;
        otherServer.outgoingShares = {sharedItem(QStringLiteral("personal"), QStringLiteral("other-account.txt"), QStringLiteral("other"))};
        auto *otherAccount = new Account(QUuid::createUuid(), QStringLiteral("test"), otherServer.url());
        otherAccount->setCredentials(new Credentials("browser-test-token", "test-refresh-token", otherAccount));
        otherAccount->credentials()->fetchFromKeychain();
        AccountState otherState(otherAccount);
        UDriveFileBrowser otherBrowser(&otherState);
        auto *otherTree = otherBrowser.findChild<QTreeView *>(QStringLiteral("driveFiles"));
        QTRY_COMPARE(otherTree->model()->rowCount(), 7);
        otherBrowser.showShares();
        otherBrowser.findChild<QAction *>(QStringLiteral("driveSharedByMe"))->trigger();
        QTRY_COMPARE(otherTree->model()->rowCount(), 1);
        QCOMPARE(otherTree->model()->index(0, 0).data().toString(), QStringLiteral("other-account.txt"));
        QCOMPARE(tree->model()->rowCount(), 2);
        QVERIFY(!select(tree, QStringLiteral("other-account.txt")).isValid());
        QVERIFY(otherServer.authenticated);
        QVERIFY(select(tree, QStringLiteral("a&b.txt")).isValid());
        QVERIFY(!browser.canCreate());
        QVERIFY(!browser.renameSelected(QStringLiteral("changed.txt")));
        QVERIFY(!browser.copySelected(QStringLiteral("copy.txt")));
        QVERIFY(!browser.deleteSelected());
        QVERIFY(search->isEnabled());
        QVERIFY(refresh->isEnabled());
        QTemporaryDir destination;
        QSignalSpy downloaded(&browser, &UDriveFileBrowser::downloadFinished);
        QVERIFY(browser.saveSelectedFile(destination.filePath(QStringLiteral("shared.txt"))));
        QTRY_COMPARE(downloaded.size(), 1);
        QVERIFY(downloaded.first().at(0).toBool());
        QCOMPARE(contents(destination.filePath(QStringLiteral("shared.txt"))), server.fileContent);
        QVERIFY(server.requests.contains({"GET", "/dav/personal/a&b.txt"}));
        search->setText(QStringLiteral("Roadmap"));
        QCOMPARE(tree->model()->rowCount(), 1);
        search->clear();
        incoming->trigger();
        QTRY_COMPARE(tree->model()->rowCount(), 1);
        QCOMPARE(tree->model()->index(0, 0).data().toString(), QStringLiteral("Shared document.pdf"));
        QCOMPARE(scope->text(), QStringLiteral("Shared with me"));
        outgoing->trigger();
        QTRY_COMPARE(tree->model()->rowCount(), 2);
        shareSearch->setText(QStringLiteral("Nonexistent"));
        QCOMPARE(tree->model()->rowCount(), 0);
        shareSearch->clear();
        QCOMPARE(tree->model()->rowCount(), 2);
        typeFilter->setCurrentIndex(1);
        QCOMPARE(tree->model()->rowCount(), 2);
        typeFilter->setCurrentIndex(2);
        QCOMPARE(tree->model()->rowCount(), 0);
        browser.showPersonal();
        QTRY_COMPARE(tree->model()->rowCount(), 7);
        browser.showShares();
        outgoing->trigger();
        QTRY_COMPARE(tree->model()->rowCount(), 2);
        typeFilter->setCurrentIndex(0);
        QCOMPARE(tree->model()->rowCount(), 2);

        // A late outgoing response cannot replace the incoming view.
        server.outgoingDelay = 200;
        refresh->click();
        incoming->trigger();
        QTRY_COMPARE(tree->model()->rowCount(), 1);
        QTest::qWait(250);
        QCOMPARE(tree->model()->index(0, 0).data().toString(), QStringLiteral("Shared document.pdf"));
        server.outgoingDelay = 0;
        server.outgoingStatus = 500;
        outgoing->trigger();
        QTRY_VERIFY(status->text().startsWith(QStringLiteral("Could not load files shared by you.")));
        QCOMPARE(tree->model()->rowCount(), 0);
        server.outgoingStatus = 200;
        server.outgoingShares = {};
        refresh->click();
        QTRY_COMPARE(status->text(), QStringLiteral("No files shared by you yet."));
        QVERIFY(refresh->isEnabled());
        QCOMPARE(emptyState->findChild<QLabel *>(QStringLiteral("driveEmptyText"))->text(), QStringLiteral("No shares"));

        // Invalid paths and unknown spaces do not become actionable file rows.
        auto invalid = personalFile;
        invalid.insert(QStringLiteral("parentReference"), QJsonObject{{"driveId", "personal"}, {"path", "../outside"}});
        auto missingPath = sharedItem(QStringLiteral("personal"), QStringLiteral("do-not-guess.txt"), QStringLiteral("missing-path"));
        missingPath.insert(QStringLiteral("parentReference"), QJsonObject{{"driveId", "personal"}});
        server.outgoingShares = {personalFile, invalid, missingPath, sharedItem(QStringLiteral("unknown"), QStringLiteral("hidden.txt"), QStringLiteral("file-c"))};
        refresh->click();
        QTRY_COMPARE(tree->model()->rowCount(), 1);
        QVERIFY(status->text().contains(QStringLiteral("some items")));
        server.outgoingShares = {sharedItem(QStringLiteral("personal"), QStringLiteral("Docs"), QStringLiteral("docs"), true)};
        refresh->click();
        QTRY_VERIFY(select(tree, QStringLiteral("Docs")).isValid());
        Q_EMIT tree->activated(tree->currentIndex());
        QTRY_COMPARE(tree->model()->rowCount(), 1);
        QCOMPARE(tree->model()->index(0, 0).data().toString(), QStringLiteral("Budget 2026.pdf"));
        QVERIFY(!scope->isVisible());
        QVERIFY(server.authenticated);
    }


    void sharePermissionAndIdentityFilters()
    {
        DriveServer server(true);
        const auto permission = [](const QString &id) {
            return QJsonObject{{"invitation", QJsonObject{{"invitedBy", QJsonObject{{"user", QJsonObject{{"id", id}, {"displayName", "Alex"}}}}}}}};
        };
        const QJsonObject link{{"link", QJsonObject{{"type", "view"}}}};
        const auto outgoingItem = [](const QString &name, const QJsonArray &permissions) {
            return QJsonObject{{"id", QString(QStringLiteral("personal!") + name)}, {"name", name}, {"file", QJsonObject{}},
                {"permissions", permissions}, {"parentReference", QJsonObject{{"driveId", "personal"}, {"path", "."}}}};
        };
        server.outgoingShares = {outgoingItem("direct.txt", {permission("alice")}), outgoingItem("link.txt", {link}),
            outgoingItem("both.txt", {permission("bob"), link}), outgoingItem("unknown.txt", {})};
        auto *account = new Account(QUuid::createUuid(), QStringLiteral("test"), server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account); UDriveFileBrowser browser(&state);
        browser.resize(1150, 800); browser.show();
        auto *tree = browser.findChild<QTreeView *>(QStringLiteral("driveFiles"));
        QTRY_COMPARE(tree->model()->rowCount(), 7);
        browser.showShares();
        browser.findChild<QPushButton *>(QStringLiteral("driveSharedWithOthersButton"))->click();
        QTRY_COMPARE(tree->model()->rowCount(), 2);
        QVERIFY(select(tree, "direct.txt").isValid());
        QVERIFY(select(tree, "both.txt").isValid());
        QVERIFY(!select(tree, "link.txt").isValid());
        auto *sharer = browser.findChild<QComboBox *>(QStringLiteral("driveShareByFilter"));
        QTRY_COMPARE(sharer->count(), 3);
        // Identical display names must still identify different people.
        QCOMPARE(sharer->itemText(1), sharer->itemText(2));
        sharer->setCurrentIndex(sharer->findData(QStringLiteral("user:alice")));
        QCOMPARE(tree->model()->rowCount(), 1);
        QCOMPARE(tree->model()->index(0, 0).data().toString(), QStringLiteral("direct.txt"));
        auto *search = browser.findChild<QLineEdit *>(QStringLiteral("driveShareSearch"));
        auto *type = browser.findChild<QComboBox *>(QStringLiteral("driveShareTypeFilter"));
        search->setText("missing"); type->setCurrentIndex(2);
        QCOMPARE(tree->model()->rowCount(), 0);
        browser.findChild<QAction *>(QStringLiteral("driveResetShareFilters"))->trigger();
        QCOMPARE(tree->model()->rowCount(), 2);
        QCOMPARE(type->currentIndex(), 0); QVERIFY(search->text().isEmpty()); QCOMPARE(sharer->currentIndex(), 0);
        browser.findChild<QPushButton *>(QStringLiteral("driveSharedViaLinkButton"))->click();
        QTRY_COMPARE(tree->model()->rowCount(), 2);
        QVERIFY(select(tree, "link.txt").isValid()); QVERIFY(select(tree, "both.txt").isValid());
        QVERIFY(!select(tree, "direct.txt").isValid());
        for (const auto &request : server.requests) QVERIFY(request.first == "GET" || request.first == "PROPFIND");
        QVERIFY(server.authenticated);
    }


    void incomingShareMetadataAndCancellation()
    {
        DriveServer server(true); server.incomingStatus = 200;
        const QString jail = QStringLiteral("a0ca6a90-a365-4782-871e-d44447bbc668$a0ca6a90-a365-4782-871e-d44447bbc668");
        const auto incomingItem = [&jail](const QString &name, const QString &creator, bool hidden, bool mounted, bool folder = false) {
            QJsonObject remote{{"size", 123}, {"permissions", QJsonArray{QJsonObject{{"invitation", QJsonObject{
                {"invitedBy", QJsonObject{{"user", QJsonObject{{"id", creator}, {"displayName", creator}}}}}}}}}}};
            remote.insert(folder ? QStringLiteral("folder") : QStringLiteral("file"), QJsonObject{});
            return QJsonObject{{"id", QString(jail + QLatin1Char('!') + name)}, {"name", name}, {"parentReference", QJsonObject{{"driveId", jail}}},
                {"@UI.Hidden", hidden}, {"@client.synchronize", mounted}, {"remoteItem", remote}};
        };
        server.incomingShares = {incomingItem("visible.txt", "alice", false, true), incomingItem("hidden.txt", "bob", true, true),
            incomingItem("pending.txt", "bob", false, false), incomingItem("Folder", "alice", false, true, true),
            incomingItem("../escape", "alice", false, true)};
        auto *account = new Account(QUuid::createUuid(), QStringLiteral("test"), server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account); UDriveFileBrowser browser(&state);
        browser.resize(1150, 800); browser.show();
        auto *tree = browser.findChild<QTreeView *>(QStringLiteral("driveFiles"));
        QTRY_COMPARE(tree->model()->rowCount(), 7);
        browser.showShares();
        QTRY_COMPARE(tree->model()->rowCount(), 3);
        auto *hidden = browser.findChild<QPushButton *>(QStringLiteral("driveShareFilterHidden"));
        auto *visible = browser.findChild<QPushButton *>(QStringLiteral("driveShareFilterShares"));
        auto *sharer = browser.findChild<QComboBox *>(QStringLiteral("driveShareByFilter"));
        auto *type = browser.findChild<QComboBox *>(QStringLiteral("driveShareTypeFilter"));
        QVERIFY(hidden->isEnabled());
        QVERIFY(select(tree, "pending.txt").isValid());
        QTemporaryDir destination;
        QVERIFY(!browser.saveSelectedFile(destination.filePath("pending.txt")));
        Q_EMIT tree->activated(tree->currentIndex());
        QCOMPARE(tree->model()->rowCount(), 3);
        hidden->click(); QCOMPARE(tree->model()->rowCount(), 1);
        QCOMPARE(tree->model()->index(0, 0).data().toString(), QStringLiteral("hidden.txt"));
        sharer->setCurrentIndex(sharer->findData(QStringLiteral("user:alice")));
        QCOMPARE(tree->model()->rowCount(), 0);
        visible->click(); QCOMPARE(tree->model()->rowCount(), 2);
        type->setCurrentIndex(2); QCOMPARE(tree->model()->rowCount(), 1);
        QCOMPARE(tree->model()->index(0, 0).data().toString(), QStringLiteral("Folder"));
        browser.findChild<QAction *>(QStringLiteral("driveResetShareFilters"))->trigger();
        QCOMPARE(tree->model()->rowCount(), 3);
        // Reset is local, and a refresh preserves the selected identity.
        sharer->setCurrentIndex(sharer->findData(QStringLiteral("user:alice")));
        auto *refresh = browser.findChild<QPushButton *>(QStringLiteral("driveRefresh"));
        refresh->click(); QTRY_COMPARE(tree->model()->rowCount(), 2);
        QCOMPARE(sharer->currentData().toString(), QStringLiteral("user:alice"));
        QCOMPARE(tree->model()->rowCount(), 2);
        for (const auto &request : server.requests) {
            QVERIFY(request.first == "GET" || request.first == "PROPFIND");
            QVERIFY(!request.second.contains(QStringLiteral("pending.txt")));
            QVERIFY(!request.second.contains(QStringLiteral("escape")));
        }
        server.incomingStatus = 403; refresh->click();
        auto *status = browser.findChild<QLabel *>(QStringLiteral("driveStatus"));
        QTRY_VERIFY(status->text().startsWith(QStringLiteral("Could not load files shared with you.")));
        QCOMPARE(tree->model()->rowCount(), 0);
        QVERIFY(!hidden->isEnabled());
        // A response started on Shares cannot repopulate another section.
        server.incomingStatus = 200; server.incomingDelay = 200; refresh->click();
        browser.showPersonal(); QTRY_COMPARE(tree->model()->rowCount(), 7);
        QTest::qWait(250); QCOMPARE(tree->model()->rowCount(), 7);
        browser.showShares(); state.signOutByUi();
        QTest::qWait(250); QCOMPARE(tree->model()->rowCount(), 0);
        QVERIFY(!refresh->isEnabled());
        QVERIFY(server.authenticated);
    }


    void outgoingSharesWithoutIncomingSpace()
    {
        DriveServer server;
        server.outgoingShares = {QJsonObject{{"id", "personal!file"}, {"name", "a&b.txt"}, {"file", QJsonObject{}},
            {"permissions", QJsonArray{QJsonObject{{"invitation", QJsonObject{}}}}},
            {"parentReference", QJsonObject{{"driveId", "personal"}, {"path", "/"}}}}};
        auto *account = new Account(QUuid::createUuid(), QStringLiteral("test"), server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveFileBrowser browser(&state);
        auto *tree = browser.findChild<QTreeView *>(QStringLiteral("driveFiles"));
        QTRY_COMPARE(tree->model()->rowCount(), 7);
        browser.showShares();
        QTRY_COMPARE(tree->model()->rowCount(), 1);
        QVERIFY(browser.findChild<QPushButton *>(QStringLiteral("driveRefresh"))->isEnabled());
        server.outgoingDelay = 200;
        browser.findChild<QPushButton *>(QStringLiteral("driveRefresh"))->click();
        state.signOutByUi();
        QTest::qWait(250);
        QCOMPARE(tree->model()->rowCount(), 0);
        QVERIFY(!browser.findChild<QPushButton *>(QStringLiteral("driveRefresh"))->isEnabled());
    }

    void clickPopupsStayInsideWindow_data()
    {
        QTest::addColumn<bool>("rtl");
        QTest::newRow("left-to-right") << false;
        QTest::newRow("right-to-left") << true;
    }

    void clickPopupsStayInsideWindow()
    {
        QFETCH(bool, rtl);
        DriveServer server(true);
        auto *account = new Account(QUuid::createUuid(), QStringLiteral("test"), server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account); UDriveFileBrowser browser(&state);
        browser.setLayoutDirection(rtl ? Qt::RightToLeft : Qt::LeftToRight);
        browser.resize(1000, 700); browser.move(80, 60); browser.show();
        auto *tree = browser.findChild<QTreeView *>(QStringLiteral("driveFiles"));
        QTRY_COMPARE(tree->model()->rowCount(), 7);
        QVERIFY(select(tree, QStringLiteral("a&b.txt")).isValid());
        const auto bounds = [&browser] { return QRect(browser.mapToGlobal(QPoint()), browser.size()); };
        const auto menus = browser.findChildren<QMenu *>();
        QVERIFY(menus.size() >= 7);
        for (auto *menu : menus) {
            if (!menu->isEnabled()) continue;
            for (const auto &corner : {QPoint(browser.width() - 1, browser.height() - 1), QPoint(0, 0)}) {
                menu->popup(browser.mapToGlobal(corner));
                QTRY_VERIFY(menu->isVisible());
                QTRY_VERIFY2(bounds().contains(menu->geometry()), qPrintable(menu->objectName()));
                QTest::keyClick(menu, Qt::Key_Escape);
                QTRY_VERIFY2(!menu->isVisible(), qPrintable(menu->objectName()));
            }
        }
        for (int itemCount : {35, 80}) {
            UDriveMenu longMenu(&browser);
            for (int i = 0; i < itemCount; ++i) longMenu.addAction(QStringLiteral("Item %1").arg(i));
            longMenu.popup(browser.mapToGlobal(QPoint(browser.width() - 1, browser.height() - 1)));
            QTRY_VERIFY(longMenu.isVisible());
            QTRY_VERIFY(bounds().contains(longMenu.geometry()));
            QTest::keyClick(&longMenu, Qt::Key_End);
            QCOMPARE(longMenu.activeAction(), longMenu.actions().last());
            QVERIFY(longMenu.rect().contains(longMenu.actionGeometry(longMenu.activeAction())));
            QSignalSpy lastTriggered(longMenu.actions().last(), &QAction::triggered);
            QTest::keyClick(&longMenu, Qt::Key_Return);
            QCOMPARE(lastTriggered.size(), 1);
        }
        UDriveComboBox combo(&browser);
        combo.setProperty("udriveCombo", true);
        for (int i = 0; i < 40; ++i) combo.addItem(QString::number(i));
        combo.setGeometry(browser.width() - 140, browser.height() - 40, 130, 30);
        combo.show(); combo.showPopup();
        auto *popup = combo.view()->window();
        QTRY_VERIFY(popup->isVisible());
        QTRY_VERIFY(bounds().contains(popup->geometry()));
        QTest::keyClick(combo.view(), Qt::Key_End);
        QTest::keyClick(combo.view(), Qt::Key_Return);
        QCOMPARE(combo.currentIndex(), 39);
        QVERIFY(!popup->isVisible());
        for (const auto &request : server.requests) QVERIFY(request.first == "GET" || request.first == "PROPFIND");
    }

    void shareDialogBasic_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::newRow("dark") << true;
        QTest::newRow("light") << false;
    }

    void shareDialogBasic()
    {
        QFETCH(bool, dark);
        const auto previous = qApp->palette();
        const auto restorePalette = qScopeGuard([previous] { qApp->setPalette(previous); });
        auto palette = previous;
        palette.setColor(QPalette::Window, QColor(dark ? "#242528" : "#FFFFFF"));
        palette.setColor(QPalette::WindowText, QColor(dark ? "#EDEDED" : "#1F2328"));
        palette.setColor(QPalette::Base, palette.color(QPalette::Window));
        palette.setColor(QPalette::Button, palette.color(QPalette::Window));
        palette.setColor(QPalette::Text, palette.color(QPalette::WindowText));
        qApp->setPalette(palette);
        DriveServer server;
        auto *account = new Account(QUuid::createUuid(), QStringLiteral("test"), server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState accountState(account);
        UDriveShareDialog dialog(&accountState, QStringLiteral("Report.pdf"), false, QStringLiteral("/Docs/Report.pdf"), QUrl(QStringLiteral("https://example.com/f/123")), QStringLiteral("space-1"));
        dialog.show();
        auto *userField = dialog.findChild<QLineEdit *>(QStringLiteral("shareUserInput"));
        auto *roleField = dialog.findChild<QComboBox *>(QStringLiteral("shareRoleCombo"));
        auto *confirmBtn = dialog.findChild<QPushButton *>(QStringLiteral("shareConfirmButton"));
        QVERIFY(userField);
        QVERIFY(roleField);
        QVERIFY(confirmBtn);
        QVERIFY(!confirmBtn->isEnabled());
        userField->setText(QStringLiteral("alice@example.com"));
        QVERIFY(!confirmBtn->isEnabled());
        userField->clear();
        QVERIFY(!confirmBtn->isEnabled());
        QCOMPARE(roleField->currentData().toInt(), 1);
        roleField->showPopup();
        auto *popup = roleField->view()->window();
        QTRY_COMPARE(popup->pos(), roleField->mapToGlobal(QPoint(0, roleField->height() + 4)));
        QCOMPARE(popup->width(), roleField->width());
        QVERIFY(roleField->screen()->availableGeometry().contains(popup->geometry()));
        for (int row = 0; row < roleField->count(); ++row) {
            const auto index = roleField->model()->index(row, 0);
            QVERIFY(roleField->fontMetrics().horizontalAdvance(index.data().toString()) + 16 <= roleField->view()->visualRect(index).width());
        }
        if (qEnvironmentVariableIsSet("UDRIVE_UI_TEST_CAPTURE")) {
            QTest::qWait(50);
            QVERIFY(popup->grab().save(QStringLiteral("/tmp/udrive-share-role-%1.png").arg(dark ? "dark" : "light")));
        }
        QTest::keyClick(roleField->view(), Qt::Key_Down);
        QTest::keyClick(roleField->view(), Qt::Key_Return);
        QCOMPARE(roleField->currentData().toInt(), 15);
        QVERIFY(dialog.isVisible());
        if (qEnvironmentVariableIsSet("UDRIVE_UI_TEST_CAPTURE")) {
            QTest::qWait(50);
            QVERIFY(dialog.grab().save(QStringLiteral("/tmp/udrive-share-dialog-%1.png").arg(dark ? "dark" : "light")));
        }
        for (const auto &request : server.requests)
            QCOMPARE(request.first, QByteArray("GET"));
    }

    void publicLinkSettings_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::newRow("dark") << true;
        QTest::newRow("light") << false;
    }

    void publicLinkSettings()
    {
        QFETCH(bool, dark);
        const auto previousPalette = qApp->palette();
        const auto restore = qScopeGuard([&] { qApp->setPalette(previousPalette); });
        auto palette = previousPalette;
        palette.setColor(QPalette::Window, QColor(dark ? "#292929" : "#FFFFFF"));
        palette.setColor(QPalette::WindowText, QColor(dark ? "#EDEDED" : "#1F2328"));
        qApp->setPalette(palette);
        DriveServer server;
        server.existingShares = {
            QJsonObject{{"id", "link-1"}, {"share_type", 3}, {"name", "Review"}, {"permissions", 1}, {"url", server.url("/s/review").toString()}},
            QJsonObject{{"id", "link-2"}, {"share_type", 3}, {"name", "Other link"}, {"permissions", 15}, {"url", server.url("/s/other").toString()}}};
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveShareDialog panel(&state, "Report.pdf", false, "/Docs/Report.pdf", {}, "space-1");
        panel.resize(520, 700);
        panel.show();
        QTRY_VERIFY(panel.findChild<QAction *>("publicLinkRename"));
        auto *more = panel.findChild<QToolButton *>("publicLinkMoreButton");
        QVERIFY(more && more->menu());
        QTRY_VERIFY(more->isVisible());
        auto *menu = more->menu();
        menu->popup(more->mapToGlobal(QPoint(0, more->height())));
        QTRY_VERIFY(menu->isVisible());
        QCOMPARE(menu->grab().toImage().pixelColor(6, menu->height() / 2).alpha(), 255);
        if (qEnvironmentVariableIsSet("UDRIVE_SHARES_TEST_CAPTURE")) {
            QPixmap capture = panel.grab();
            QPainter painter(&capture);
            painter.drawPixmap(menu->mapToGlobal(QPoint()) - panel.mapToGlobal(QPoint()), menu->grab());
            painter.end();
            capture.save(QStringLiteral("/tmp/udrive-public-link-menu-%1.png").arg(dark ? "dark" : "light"));
        }
        menu->hide();

        panel.findChild<QAction *>("publicLinkRename")->trigger();
        auto *edit = panel.findChild<QDialog *>("publicLinkEditDialog");
        QVERIFY(edit);
        edit->findChild<QLineEdit *>("publicLinkEditValue")->setText("Discard");
        QPointer<QDialog> pending = edit;
        edit->reject();
        QTRY_VERIFY(!pending);
        QCOMPARE(server.sharePuts.size(), 0);

        panel.findChild<QAction *>("publicLinkRename")->trigger();
        edit = panel.findChild<QDialog *>("publicLinkEditDialog");
        auto *input = edit->findChild<QLineEdit *>("publicLinkEditValue");
        QCOMPARE(input->text(), QStringLiteral("Review"));
        QCOMPARE(input->maxLength(), 64);
        input->setText("Review & approval");
        pending = edit;
        edit->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        QTRY_VERIFY(!pending);
        QTRY_COMPARE(server.sharePuts.size(), 1);
        QCOMPARE(server.sharePuts.last().queryItems(), (QList<QPair<QString, QString>>{{"name", "Review & approval"}}));
        QTRY_COMPARE(server.existingShares.first().toObject().value("name").toString(), QStringLiteral("Review & approval"));
        QTRY_VERIFY(panel.findChild<QAction *>("publicLinkRename"));
        QTRY_VERIFY(panel.findChild<QLabel *>("shareStatusLabel")->text() == "Public link updated.");
        QTRY_COMPARE(panel.findChild<QLabel *>("publicLinkName")->text(), QStringLiteral("Review & approval"));

        panel.findChild<QAction *>("publicLinkExpiration")->trigger();
        edit = panel.findChild<QDialog *>("publicLinkEditDialog");
        auto *date = edit->findChild<QDateEdit *>("publicLinkExpirationDate");
        QCOMPARE(date->minimumDate(), QDate::currentDate());
        const auto expires = QDate::currentDate().addDays(10);
        date->setDate(expires);
        pending = edit;
        edit->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        QTRY_VERIFY(!pending);
        QTRY_COMPARE(server.sharePuts.size(), 2);
        QCOMPARE(server.sharePuts.last().queryItemValue("expireDate"), expires.toString(Qt::ISODate));
        QTRY_VERIFY(panel.findChild<QLabel *>("publicLinkStatus"));
        QVERIFY(panel.findChild<QLabel *>("publicLinkStatus")->text().contains("Expires"));

        panel.findChild<QAction *>("publicLinkPassword")->trigger();
        edit = panel.findChild<QDialog *>("publicLinkEditDialog");
        input = edit->findChild<QLineEdit *>("publicLinkEditValue");
        QCOMPARE(input->echoMode(), QLineEdit::Password);
        QVERIFY(input->text().isEmpty());
        QVERIFY(!edit->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->isEnabled());
        input->setText(" Test & password! ");
        pending = edit;
        edit->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        QTRY_VERIFY(!pending);
        QTRY_COMPARE(server.sharePuts.size(), 3);
        QCOMPARE(server.sharePuts.last().queryItemValue("password", QUrl::FullyDecoded), QStringLiteral(" Test & password! "));
        QTRY_VERIFY(panel.findChild<QLabel *>("publicLinkStatus")->text().contains("Password protected"));

        panel.findChild<QAction *>("publicLinkPassword")->trigger();
        edit = panel.findChild<QDialog *>("publicLinkEditDialog");
        QVERIFY(edit->findChild<QLineEdit *>("publicLinkEditValue")->text().isEmpty());
        auto *remove = edit->findChild<QCheckBox *>("publicLinkRemoveSetting");
        QVERIFY(remove->isVisible());
        remove->setChecked(true);
        pending = edit;
        edit->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        QTRY_VERIFY(!pending);
        QTRY_COMPARE(server.sharePuts.size(), 4);
        QVERIFY(server.sharePuts.last().hasQueryItem("password"));
        QVERIFY(server.sharePuts.last().queryItemValue("password").isEmpty());
        QTRY_VERIFY(!panel.findChild<QLabel *>("publicLinkStatus")->text().contains("Password protected"));

        panel.findChild<QAction *>("publicLinkExpiration")->trigger();
        edit = panel.findChild<QDialog *>("publicLinkEditDialog");
        edit->findChild<QCheckBox *>("publicLinkRemoveSetting")->setChecked(true);
        pending = edit;
        edit->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        QTRY_VERIFY(!pending);
        QTRY_COMPARE(server.sharePuts.size(), 5);
        QVERIFY(server.sharePuts.last().hasQueryItem("expireDate"));
        QVERIFY(server.sharePuts.last().queryItemValue("expireDate").isEmpty());
        QTRY_VERIFY(!panel.findChild<QLabel *>("publicLinkStatus"));
        QCOMPARE(server.existingShares.first().toObject().value("permissions").toInt(), 1);
        QCOMPARE(server.existingShares.last().toObject().value("name").toString(), QStringLiteral("Other link"));
        QCOMPARE(server.existingShares.last().toObject().value("permissions").toInt(), 15);

        server.sharePutStatus = 403;
        panel.findChild<QAction *>("publicLinkRename")->trigger();
        edit = panel.findChild<QDialog *>("publicLinkEditDialog");
        edit->findChild<QLineEdit *>("publicLinkEditValue")->setText("Rejected");
        pending = edit;
        edit->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        QTRY_VERIFY(edit->findChild<QWidget *>("publicLinkEditError")->isVisible());
        QVERIFY(pending && edit->isVisible());
        QCOMPARE(edit->findChild<QLabel *>("publicLinkEditErrorDetails")->text(), QStringLiteral("Access denied"));
        QCOMPARE(edit->findChild<QLineEdit *>("publicLinkEditValue")->text(), QStringLiteral("Rejected"));
        QVERIFY(!panel.findChild<QLabel *>("shareStatusLabel")->text().contains("Failed to update"));
        QCOMPARE(server.existingShares.first().toObject().value("name").toString(), QStringLiteral("Review & approval"));
        edit->reject();
        QTRY_VERIFY(!pending);
        QTRY_VERIFY(panel.findChild<QAction *>("publicLinkRename"));
        QTRY_COMPARE(panel.findChild<QLabel *>("publicLinkName")->text(), QStringLiteral("Review & approval"));
        panel.findChild<QAction *>("publicLinkRename")->trigger();
        edit = panel.findChild<QDialog *>("publicLinkEditDialog");
        QVERIFY(edit);
        pending = edit;
        panel.reject();
        QTRY_VERIFY(!pending);
        QCOMPARE(server.sharePuts.size(), 6);
    }

    void publicLinkPasswordError_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::newRow("dark") << true;
        QTest::newRow("light") << false;
    }

    void publicLinkPasswordError()
    {
        QFETCH(bool, dark);
        const auto previousPalette = qApp->palette();
        const auto restore = qScopeGuard([&] { qApp->setPalette(previousPalette); });
        auto palette = previousPalette;
        palette.setColor(QPalette::Window, QColor(dark ? "#292929" : "#FFFFFF"));
        palette.setColor(QPalette::WindowText, QColor(dark ? "#EDEDED" : "#1F2328"));
        qApp->setPalette(palette);
        DriveServer server;
        server.existingShares = {QJsonObject{{"id", "link-1"}, {"share_type", 3}, {"name", "Review"}, {"permissions", 1}, {"url", server.url("/s/review").toString()}}};
        server.sharePutStatus = 400;
        server.sharePutDelay = 150;
        server.sharePutMessage = QStringLiteral("At least 8 characters are required\nat least 1 uppercase letters are required\nat least 1 numbers are required\nat least 1 special characters are required !\"#$%&'()*+,-./:;<=>?@[]^_`{}|~");
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveShareDialog panel(&state, "Report.pdf", false, "/Report.pdf", {}, "space-1");
        panel.show();
        QTRY_VERIFY(panel.findChild<QAction *>("publicLinkPassword"));
        const QString oldStatus = panel.findChild<QLabel *>("shareStatusLabel")->text();
        panel.findChild<QAction *>("publicLinkPassword")->trigger();
        QPointer<QDialog> edit = panel.findChild<QDialog *>("publicLinkEditDialog");
        QVERIFY(edit);
        const int initialHeight = edit->height();
        auto *input = edit->findChild<QLineEdit *>("publicLinkEditValue");
        auto *save = edit->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save);
        input->setText("weak");
        save->click();
        QVERIFY(!save->isEnabled());
        QVERIFY(!input->isEnabled());
        save->click();
        QTRY_COMPARE(server.sharePuts.size(), 1);
        auto *error = edit->findChild<QWidget *>("publicLinkEditError");
        QTRY_VERIFY(error->isVisible());
        QVERIFY(edit->isVisible());
        QVERIFY(edit->height() > initialHeight + 40);
        QCOMPARE(input->text(), QStringLiteral("weak"));
        QCOMPARE(input->echoMode(), QLineEdit::Password);
        QVERIFY(input->isEnabled() && save->isEnabled());
        QCOMPARE(edit->findChild<QLabel *>("publicLinkEditErrorTitle")->text(), QStringLiteral("Choose a stronger password"));
        auto *details = edit->findChild<QLabel *>("publicLinkEditErrorDetails");
        QCOMPARE(details->text(), QStringLiteral("• At least 8 characters\n• At least 1 uppercase letter\n• At least 1 number\n• At least 1 special character"));
        QCOMPARE(details->textFormat(), Qt::PlainText);
        QCOMPARE(panel.findChild<QLabel *>("shareStatusLabel")->text(), oldStatus);
        QVERIFY(!panel.hasChanges());
        if (qEnvironmentVariableIsSet("UDRIVE_SHARES_TEST_CAPTURE"))
            edit->grab().save(QStringLiteral("/tmp/udrive-password-error-%1.png").arg(dark ? "dark" : "light"));

        // Retry in the same editor; preserve exact input, collapse error height on save, and close only on success.
        server.sharePutStatus = 200;
        server.sharePutDelay = 100;
        input->setText("New Strong Password 9!");
        save->click();
        QVERIFY(!error->isVisible());
        QCOMPARE(edit->height(), initialHeight);
        QTRY_VERIFY(!edit);
        QTRY_COMPARE(server.sharePuts.size(), 2);
        QCOMPARE(server.sharePuts.last().queryItemValue("password", QUrl::FullyDecoded), QStringLiteral("New Strong Password 9!"));
        QTRY_VERIFY(panel.findChild<QLabel *>("publicLinkStatus"));
        QVERIFY(panel.findChild<QLabel *>("publicLinkStatus")->text().contains("Password protected"));
        QVERIFY(panel.hasChanges());

        // Cancellation while saving invalidates the response and releases the UI.
        server.sharePutStatus = 400;
        server.sharePutDelay = 300;
        panel.findChild<QAction *>("publicLinkPassword")->trigger();
        edit = panel.findChild<QDialog *>("publicLinkEditDialog");
        input = edit->findChild<QLineEdit *>("publicLinkEditValue");
        input->setText("weak again");
        edit->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        QTRY_COMPARE(server.sharePuts.size(), 3);
        edit->reject();
        QTRY_VERIFY(!edit);
        QVERIFY(panel.findChild<QPushButton *>("shareAddLinkButton")->isEnabled());
        QTest::qWait(350);
        QVERIFY(!panel.findChild<QLabel *>("shareStatusLabel")->text().contains("required"));
        QCOMPARE(server.existingShares.first().toObject().value("permissions").toInt(), 1);
        QVERIFY(server.existingShares.first().toObject().value("share_with").toBool());
    }

    void publicLinkManagement()
    {
        DriveServer server;
        auto *account = new Account(QUuid::createUuid(), QStringLiteral("test"), server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState accountState(account);
        UDriveShareDialog dialog(&accountState, QStringLiteral("Report.pdf"), false, QStringLiteral("/Docs/Report.pdf"), QUrl(QStringLiteral("https://example.com/f/123")), QStringLiteral("space-1"));
        dialog.show();
        auto *addBtn = dialog.findChild<QPushButton *>(QStringLiteral("shareAddLinkButton"));
        auto *publicScroll = dialog.findChild<QScrollArea *>(QStringLiteral("publicLinksScrollArea"));
        auto *status = dialog.findChild<QLabel *>(QStringLiteral("shareStatusLabel"));
        QVERIFY(addBtn);
        QVERIFY(publicScroll);
        QVERIFY(status);

        // Initially no public links exist
        QTRY_VERIFY(publicScroll->widget()->findChild<QLabel *>());
        QCOMPARE(publicScroll->widget()->findChild<QLabel *>()->text(), QStringLiteral("This file has no public link."));

        // Click Add link to create a public link
        addBtn->click();
        QTRY_COMPARE(server.sharePosts, 1);
        QCOMPARE(server.existingShares.size(), 1);
        QTRY_VERIFY(!status->text().isEmpty());
        QVERIFY(status->text().contains(QStringLiteral("Public link created")));

        // Check clipboard got populated
        QVERIFY(QApplication::clipboard()->text().contains(QStringLiteral("/s/public-share-1")));

        // Verify public link row is rendered
        auto *roleCombo = dialog.findChild<QComboBox *>(QStringLiteral("publicLinkRoleCombo"));
        QTRY_VERIFY(roleCombo);
        QCOMPARE(roleCombo->currentData().toInt(), 1);

        // Update permissions to Can edit (15)
        roleCombo->setCurrentIndex(1);
        Q_EMIT roleCombo->activated(1);
        QTRY_COMPARE(server.sharePuts.size(), 1);
        QCOMPARE(server.sharePuts.first().queryItemValue("permissions"), QStringLiteral("15"));
        QTRY_VERIFY(status->text().contains(QStringLiteral("Public link permissions updated")));

        // Click copy action button
        auto actionButtons = dialog.findChildren<QToolButton *>(QStringLiteral("publicLinkActionButton"));
        QCOMPARE(actionButtons.size(), 1);
        auto *copyBtn = actionButtons.at(0);
        copyBtn->click();
        QCOMPARE(QApplication::clipboard()->text(), server.url(QStringLiteral("/s/public-share-1")).toString());
        QVERIFY(status->text().contains(QStringLiteral("copied to clipboard")));

        // Delete public link
        dialog.findChild<QAction *>("publicLinkDelete")->trigger();
        QTRY_COMPARE(server.shareDeletions.size(), 1);
        QCOMPARE(server.shareDeletions.first(), QStringLiteral("public-share-1"));
        QTRY_COMPARE(publicScroll->widget()->findChild<QLabel *>()->text(), QStringLiteral("This file has no public link."));

        // When target is Workspace, public links section is hidden
        UDriveShareDialog workspaceDialog(&accountState, QStringLiteral("Team files"), true, {}, {}, QStringLiteral("team"), nullptr,
            UDriveShareDialog::Target::Workspace);
        workspaceDialog.show();
        auto *wsPublicSection = workspaceDialog.findChild<QScrollArea *>(QStringLiteral("publicLinksScrollArea"));
        QVERIFY(wsPublicSection);
        QVERIFY(wsPublicSection->parentWidget()->isHidden());
    }

    void incomingShareNotifications()
    {
        DriveServer server(true);
        server.incomingStatus = 200;

        const QString jail = QStringLiteral("a0ca6a90-a365-4782-871e-d44447bbc668$a0ca6a90-a365-4782-871e-d44447bbc668");
        const auto makeShare = [&jail](const QString &name, const QString &creatorId, bool hidden = false, const QString &displayName = {}) {
            const QString display = displayName.isEmpty() ? creatorId : displayName;
            QJsonObject userObj;
            userObj.insert(QStringLiteral("id"), creatorId);
            userObj.insert(QStringLiteral("displayName"), display);

            QJsonObject invitedByObj;
            invitedByObj.insert(QStringLiteral("user"), userObj);

            QJsonObject invitationObj;
            invitationObj.insert(QStringLiteral("invitedBy"), invitedByObj);

            QJsonObject permissionObj;
            permissionObj.insert(QStringLiteral("invitation"), invitationObj);

            QJsonArray permissionsArray;
            permissionsArray.append(permissionObj);

            QJsonObject remoteItemObj;
            remoteItemObj.insert(QStringLiteral("file"), QJsonObject());
            remoteItemObj.insert(QStringLiteral("permissions"), permissionsArray);

            QJsonObject parentRef;
            parentRef.insert(QStringLiteral("driveId"), jail);

            QJsonObject share;
            share.insert(QStringLiteral("id"), QString(jail + QLatin1Char('!') + name));
            share.insert(QStringLiteral("name"), name);
            share.insert(QStringLiteral("parentReference"), parentRef);
            share.insert(QStringLiteral("@UI.Hidden"), hidden);
            share.insert(QStringLiteral("@client.synchronize"), true);
            share.insert(QStringLiteral("remoteItem"), remoteItemObj);
            return share;
        };

        server.incomingShares = { makeShare("existing1.pdf", "Toch Vanna"), makeShare("existing2.pdf", "Seng Porekeat") };

        auto *account = new Account(QUuid::createUuid(), QStringLiteral("test"), server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState accountState(account);

        UDriveIncomingSharesNotifier notifier(&accountState);
        QSignalSpy newShareSpy(&notifier, &UDriveIncomingSharesNotifier::newSharesDetected);
        QSignalSpy updateSpy(&notifier, &UDriveIncomingSharesNotifier::incomingSharesUpdated);

        // Initial check: baseline established, no notification sent!
        notifier.checkNow();
        QTRY_VERIFY(notifier.hasBaseline());
        QTRY_VERIFY(!notifier.isInFlight());
        QCOMPARE(newShareSpy.size(), 0);

        // Now add a new incoming share
        server.incomingShares.append(makeShare("Quarterly_Report.pdf", "Toch Vanna"));
        notifier.checkNow();
        QTRY_COMPARE(newShareSpy.size(), 1);
        const auto fileNames = newShareSpy.first().at(0).toStringList();
        const auto sharers = newShareSpy.first().at(1).toStringList();
        QCOMPARE(fileNames, QStringList{"Quarterly_Report.pdf"});
        QCOMPARE(sharers, QStringList{"Toch Vanna"});
        QCOMPARE(updateSpy.size(), 1);

        // Check again without changes: no new notifications
        newShareSpy.clear();
        notifier.checkNow();
        QTest::qWait(100);
        QCOMPARE(newShareSpy.size(), 0);

        // Add hidden share: should not notify
        server.incomingShares.append(makeShare("hidden.pdf", "Secret", true));
        notifier.checkNow();
        QTest::qWait(100);
        QCOMPARE(newShareSpy.size(), 0);

        // Security check: HTML / script tags in share name & sharer handled safely
        newShareSpy.clear();
        server.incomingShares.append(makeShare("<img src=x>report.pdf", "alice-id", false, "<b>Alice & Bob</b>"));
        notifier.checkNow();
        QTRY_COMPARE(newShareSpy.size(), 1);
        QCOMPARE(newShareSpy.first().at(0).toStringList(), QStringList{"<img src=x>report.pdf"});
        QCOMPARE(newShareSpy.first().at(1).toStringList(), QStringList{"<b>Alice & Bob</b>"});

        // Notifications model & unread state verification
        const auto notifications = notifier.notifications();
        QVERIFY(!notifications.isEmpty());
        QCOMPARE(notifier.unreadCount(), notifications.size());

        // Sharer initials check
        bool foundToch = false;
        for (const auto &n : notifications) {
            if (n.sharerName == QStringLiteral("Toch Vanna")) {
                QCOMPARE(n.sharerInitials, QStringLiteral("TV"));
                foundToch = true;
            }
        }
        QVERIFY(foundToch);

        // Mark single notification as read
        QSignalSpy unreadSpy(&notifier, &UDriveIncomingSharesNotifier::unreadCountChanged);
        const QString firstId = notifications.first().id;
        notifier.markAsRead(firstId);
        QCOMPARE(notifier.unreadCount(), notifications.size() - 1);
        QCOMPARE(unreadSpy.size(), 1);
        QCOMPARE(unreadSpy.last().at(0).toInt(), notifications.size() - 1);

        // Mark all as read
        notifier.markAllAsRead();
        QCOMPARE(notifier.unreadCount(), 0);
        QCOMPARE(unreadSpy.last().at(0).toInt(), 0);
        for (const auto &n : notifier.notifications()) {
            QVERIFY(n.isRead);
        }

        // Sign out: in-flight request ignored, no crash
        server.incomingDelay = 150;
        server.incomingShares.append(makeShare("after_logout.pdf", "Nobody"));
        notifier.checkNow();
        accountState.signOutByUi();
        QTest::qWait(200);
        QCOMPARE(newShareSpy.size(), 1);
    }
    void shareRecipientSearch_data()
    {
        QTest::addColumn<int>("type");
        QTest::addColumn<QString>("remotePath");
        QTest::addColumn<QString>("spaceId");
        QTest::addColumn<QString>("expectedPath");
        QTest::addColumn<bool>("folder");
        QTest::newRow("user-image") << 0 << QStringLiteral("2026-10-05 16.02.12.jpg")
            << QStringLiteral("provider$personal!personal") << QStringLiteral("/2026-10-05 16.02.12.jpg") << false;
        QTest::newRow("group-nested-folder") << 1 << QStringLiteral("/Docs/Team reports")
            << QStringLiteral("provider$team!team") << QStringLiteral("/Docs/Team reports") << true;
        QTest::newRow("unicode-and-reserved-characters") << 0 << QStringLiteral("/ឯកសារ/a+b & 100% # ?.pdf")
            << QStringLiteral("provider$personal!personal") << QStringLiteral("/ឯកសារ/a+b & 100% # ?.pdf") << false;
        QTest::newRow("literal-percent-escape") << 0 << QStringLiteral("Docs/%2F.txt")
            << QStringLiteral("provider$team!team") << QStringLiteral("/Docs/%2F.txt") << false;
        QTest::newRow("legacy-without-space") << 1 << QStringLiteral("/Docs/Report.pdf")
            << QString() << QStringLiteral("/Docs/Report.pdf") << false;
    }

    void shareRecipientSearch()
    {
        QFETCH(int, type);
        QFETCH(QString, remotePath);
        QFETCH(QString, spaceId);
        QFETCH(QString, expectedPath);
        QFETCH(bool, folder);
        DriveServer server;
        server.expectedShareReference = spaceId.isEmpty() ? QString() : spaceId + expectedPath;
        server.expectedSharePath = expectedPath;
        const QJsonObject recipient{{"label", "Alice <Admin>"}, {"value", QJsonObject{
            {"shareType", type}, {"shareWith", "server-id-42"}, {"shareWithAdditionalInfo", "alice@example.com"}}}};
        const QString kind = type == 0 ? QStringLiteral("users") : QStringLiteral("groups");
        // Exact and regular results overlap; unsupported recipients must not be offered.
        server.recipientData = QJsonObject{{"exact", QJsonObject{{kind, QJsonArray{recipient}}}},
            {kind, QJsonArray{recipient, QJsonObject{{"label", "Remote"}, {"value", QJsonObject{
                {"shareType", 6}, {"shareWith", "remote-id"}}}}}}};
        auto *account = new Account(QUuid::createUuid(), QStringLiteral("test"), server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveShareDialog dialog(&state, remotePath.section(QLatin1Char('/'), -1), folder, remotePath, {}, spaceId);
        dialog.show();
        auto *input = dialog.findChild<QLineEdit *>(QStringLiteral("shareUserInput"));
        auto *confirm = dialog.findChild<QPushButton *>(QStringLiteral("shareConfirmButton"));
        auto *role = dialog.findChild<QComboBox *>(QStringLiteral("shareRoleCombo"));
        auto *completer = dialog.findChild<QCompleter *>();
        auto *status = dialog.findChild<QLabel *>(QStringLiteral("shareStatusLabel"));
        QVERIFY(input && confirm && role && completer && status);
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        dialog.activateWindow();
        input->setFocus();
        QTRY_VERIFY(input->hasFocus());
        QTest::keyClicks(input, "a");
        QTest::qWait(350);
        QVERIFY(server.recipientQueries.isEmpty());
        QTest::keyClicks(input, "lice");
        QCOMPARE(input->text(), QStringLiteral("alice"));
        QTRY_COMPARE(server.recipientQueries.size(), 1);
        QCOMPARE(server.recipientQueries.first().queryItemValue("search"), QStringLiteral("alice"));
        QCOMPARE(server.recipientQueries.first().queryItemValue("itemType"), folder ? QStringLiteral("folder") : QStringLiteral("file"));
        QCOMPARE(server.recipientQueries.first().queryItemValue("perPage"), QStringLiteral("20"));
        QStringList shareTypes;
        for (const auto &item : server.recipientQueries.first().queryItems(QUrl::FullyDecoded)) {
            if (item.first == QStringLiteral("shareType[]")) shareTypes.append(item.second);
        }
        QCOMPARE(shareTypes, QStringList({"0", "1"}));
        QTRY_VERIFY(completer->popup()->isVisible());
        QCOMPARE(completer->completionModel()->rowCount(), 1);
        QTRY_COMPARE(completer->popup()->width(), input->width());
        QVERIFY(completer->popup()->y() >= input->mapToGlobal(QPoint(0, input->height())).y());
        if (qEnvironmentVariableIsSet("UDRIVE_UI_TEST_CAPTURE"))
            QVERIFY(completer->popup()->grab().save(QStringLiteral("/tmp/udrive-recipient-%1.png").arg(type)));
        QVERIFY(completer->completionModel()->index(0, 0).data().toString().contains("alice@example.com"));
        QCOMPARE(status->textFormat(), Qt::PlainText);
        QVERIFY(!confirm->isEnabled());
        QTest::keyClick(completer->popup(), Qt::Key_Down);
        QTest::keyClick(completer->popup(), Qt::Key_Return);
        QTRY_VERIFY(confirm->isEnabled());
        QCOMPARE(input->text(), QStringLiteral("Alice <Admin>"));
        QCOMPARE(server.sharePosts, 0); // Selecting a suggestion never sends an invitation.
        QVERIFY(dialog.isVisible());
        // Simulate the user clicking back into the field after accepting a suggestion.
        dialog.activateWindow();
        (void)QTest::qWaitForWindowActive(&dialog);
        QTest::mouseClick(input, Qt::LeftButton);
        input->setFocus();
        QTRY_VERIFY_WITH_TIMEOUT(input->hasFocus(), 10000);
        // Changing the selected label invalidates its identity.
        QTest::keyClicks(input, "x");
        QVERIFY(!confirm->isEnabled());
        input->setText(QStringLiteral("alice"));
        QTRY_COMPARE(server.recipientQueries.size(), 2);
        QTRY_COMPARE(completer->completionModel()->rowCount(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(input->hasFocus(), 10000);
        QTRY_VERIFY(completer->popup()->isVisible());
        QTest::keyClick(completer->popup(), Qt::Key_Down);
        QTest::keyClick(completer->popup(), Qt::Key_Return);
        QTRY_VERIFY(confirm->isEnabled());
        role->setCurrentIndex(1);
        server.sharePostDelay = 100;
        QTest::mouseClick(confirm, Qt::LeftButton);
        QVERIFY(!confirm->isEnabled());
        QVERIFY(!input->isEnabled());
        QTest::mouseClick(confirm, Qt::LeftButton);
        QTRY_COMPARE(server.sharePosts, 1);
        QTRY_VERIFY(input->text().isEmpty());
        QVERIFY(!confirm->isEnabled());
        const QUrlQuery body(QString::fromUtf8(server.requestBodies.value("POST /ocs/v1.php/apps/files_sharing/api/v1/shares")));
        QCOMPARE(body.queryItemValue("shareWith"), QStringLiteral("server-id-42"));
        QCOMPARE(body.queryItemValue("shareType"), QString::number(type));
        QCOMPARE(body.queryItemValue("permissions"), QStringLiteral("15"));
        QCOMPARE(body.queryItemValue("space_ref", QUrl::FullyDecoded), server.expectedShareReference);
        QCOMPARE(body.queryItemValue("path", QUrl::FullyDecoded), expectedPath);
        if (spaceId.isEmpty()) QVERIFY(!body.hasQueryItem("space_ref"));
        // Listing access must use the same item reference before and after sharing.
        QTRY_COMPARE(server.shareQueries.size(), 2);
        for (const auto &query : server.shareQueries) {
            QCOMPARE(query.queryItemValue("space_ref", QUrl::FullyDecoded), server.expectedShareReference);
            QCOMPARE(query.queryItemValue("path", QUrl::FullyDecoded), expectedPath);
            if (spaceId.isEmpty()) QVERIFY(!query.hasQueryItem("space_ref"));
        }
        QVERIFY(server.authenticated);
    }


    void shareRejectsInvalidTargets_data()
    {
        QTest::addColumn<QString>("remotePath");
        QTest::newRow("empty-root") << QString();
        QTest::newRow("slash-root") << QStringLiteral("/");
        QTest::newRow("traversal") << QStringLiteral("Docs/../other.txt");
        QTest::newRow("multiple-leading-slashes") << QStringLiteral("//Docs/report.pdf");
    }

    void shareRejectsInvalidTargets()
    {
        QFETCH(QString, remotePath);
        DriveServer server;
        auto *account = new Account(QUuid::createUuid(), QStringLiteral("test"), server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveShareDialog dialog(&state, QStringLiteral("Invalid target"), false, remotePath, {}, QStringLiteral("provider$personal!personal"));
        auto *input = dialog.findChild<QLineEdit *>(QStringLiteral("shareUserInput"));
        auto *confirm = dialog.findChild<QPushButton *>(QStringLiteral("shareConfirmButton"));
        input->setText(QStringLiteral("alice"));
        QTest::qWait(400);
        QVERIFY(server.recipientQueries.isEmpty());
        QVERIFY(server.shareQueries.isEmpty());
        QCOMPARE(server.sharePosts, 0);
        QVERIFY(!confirm->isEnabled());
    }

    void shareRecipientSearchPreservesFocus()
    {
        DriveServer server;
        server.recipientDelay = 150;
        server.recipientData = QJsonObject{{"users", QJsonArray{QJsonObject{{"label", "Alice"},
            {"value", QJsonObject{{"shareType", 0}, {"shareWith", "alice-id"}}}}}}};
        auto *account = new Account(QUuid::createUuid(), QStringLiteral("test"), server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveShareDialog dialog(&state, QStringLiteral("Docs"), true, QStringLiteral("/Docs"), {});
        dialog.show();
        auto *input = dialog.findChild<QLineEdit *>(QStringLiteral("shareUserInput"));
        auto *role = dialog.findChild<QComboBox *>(QStringLiteral("shareRoleCombo"));
        auto *confirm = dialog.findChild<QPushButton *>(QStringLiteral("shareConfirmButton"));
        auto *completer = dialog.findChild<QCompleter *>();
        QVERIFY(input && role && confirm && completer);
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        dialog.activateWindow();
        input->setFocus();
        QTRY_VERIFY(input->hasFocus());
        QTest::keyClicks(input, "alice");
        QTRY_COMPARE(server.recipientQueries.size(), 1);
        role->setFocus(Qt::TabFocusReason);
        QTRY_VERIFY(role->hasFocus());
        QTRY_COMPARE(completer->completionModel()->rowCount(), 1);
        QVERIFY(role->hasFocus());
        QVERIFY(!completer->popup()->isVisible());
        QVERIFY(!confirm->isEnabled());
        QCOMPARE(server.sharePosts, 0);
    }

    void shareRecipientSearchFailures()
    {
        DriveServer server;
        auto *account = new Account(QUuid::createUuid(), QStringLiteral("test"), server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveShareDialog dialog(&state, QStringLiteral("Docs"), true, QStringLiteral("/Docs"), {});
        dialog.show();
        auto *input = dialog.findChild<QLineEdit *>(QStringLiteral("shareUserInput"));
        auto *confirm = dialog.findChild<QPushButton *>(QStringLiteral("shareConfirmButton"));
        auto *status = dialog.findChild<QLabel *>(QStringLiteral("shareStatusLabel"));
        auto *completer = dialog.findChild<QCompleter *>();
        input->setFocus();
        input->setText(QStringLiteral("nobody"));
        QTRY_VERIFY(status->text().startsWith("No matching"));
        QCOMPARE(server.recipientQueries.first().queryItemValue("itemType"), QStringLiteral("folder"));
        QVERIFY(!confirm->isEnabled());
        server.recipientStatus = 500;
        input->setText(QStringLiteral("failure"));
        QTRY_VERIFY(status->text().startsWith("Could not search"));
        QVERIFY(!confirm->isEnabled());
        server.recipientStatus = 200;
        server.recipientDelay = 700;
        server.recipientData = QJsonObject{{"users", QJsonArray{QJsonObject{{"label", "Stale"},
            {"value", QJsonObject{{"shareType", 0}, {"shareWith", "stale-id"}}}}}}};
        input->setText(QStringLiteral("stale"));
        QTRY_COMPARE(server.recipientQueries.size(), 3);
        input->clear();
        QTest::qWait(800);
        QVERIFY(!completer->popup()->isVisible());
        QCOMPARE(completer->completionModel()->rowCount(), 0);
        QVERIFY(!confirm->isEnabled());
        input->setText(QStringLiteral("signout"));
        QTRY_COMPARE(server.recipientQueries.size(), 4);
        state.signOutByUi();
        QTRY_VERIFY(!dialog.isVisible());
        QTest::qWait(800);
        QCOMPARE(server.sharePosts, 0);
        QVERIFY(!completer->popup()->isVisible());
    }

    void workspacesOverviewCatalogAndNavigation()
    {
        DriveServer server;
        auto *account = new Account(QUuid::createUuid(), QStringLiteral("test"), server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveFileBrowser browser(&state);
        browser.resize(1050, 760);
        browser.show();

        auto *search = browser.takeSearchField();
        QVERIFY(search != nullptr);

        auto *tree = browser.findChild<QTreeView *>(QStringLiteral("driveFiles"));
        auto *workspacesView = browser.findChild<QListView *>(QStringLiteral("driveWorkspacesView"));
        auto *status = browser.findChild<QLabel *>(QStringLiteral("driveStatus"));
        QVERIFY(tree);
        QVERIFY(workspacesView);
        QVERIFY(status);

        // Initially in Personal section
        QTRY_VERIFY(tree->model()->rowCount() > 0);
        QVERIFY(!browser.isWorkspacesOverview());
        QVERIFY(tree->isVisible());
        QVERIFY(!workspacesView->isVisible());

        // Switch to Workspaces with workspaceLayout enabled -> switches to Overview
        browser.showWorkspaces();
        QVERIFY(browser.isWorkspacesOverview());
        QTRY_VERIFY(workspacesView->isVisible());
        QVERIFY(!tree->isVisible());

        // Workspaces model contains non-personal, non-share spaces ("Team files")
        QTRY_COMPARE(workspacesView->model()->rowCount(), 1);
        const auto firstIndex = workspacesView->model()->index(0, 0);
        QCOMPARE(firstIndex.data(Qt::DisplayRole).toString(), QStringLiteral("Team files"));
        QCOMPARE(firstIndex.data(UDriveFileBrowser::SpaceIdRole).toString(), QStringLiteral("team"));
        QVERIFY(status->text().contains(QStringLiteral("1")));

        // Search filtering
        search->setText(QStringLiteral("Nonexistent"));
        QCOMPARE(workspacesView->model()->rowCount(), 0);
        search->setText(QStringLiteral("Team"));
        QCOMPARE(workspacesView->model()->rowCount(), 1);
        search->clear();
        QCOMPARE(workspacesView->model()->rowCount(), 1);

        // Navigate inside the workspace
        browser.openWorkspace(QStringLiteral("team"));
        QVERIFY(!browser.isWorkspacesOverview());
        QTRY_VERIFY(tree->isVisible());
        QVERIFY(!workspacesView->isVisible());
        QTRY_COMPARE(tree->model()->rowCount(), 1);

        // Breadcrumb shows "Workspaces" button
        QTRY_VERIFY(browser.findChild<QAbstractButton *>(QStringLiteral("driveBreadcrumbWorkspaces")) != nullptr);
        auto *workspacesCrumb = browser.findChild<QAbstractButton *>(QStringLiteral("driveBreadcrumbWorkspaces"));
        QVERIFY(workspacesCrumb->isVisible());

        // Clicking "Workspaces" breadcrumb returns to overview catalog
        workspacesCrumb->click();
        QTRY_VERIFY(browser.isWorkspacesOverview());
        QTRY_VERIFY(workspacesView->isVisible());
        QVERIFY(!tree->isVisible());
        QCOMPARE(workspacesView->model()->rowCount(), 1);
    }

    void workspaceCatalogControls()
    {
        DriveServer server;
        for (const auto &name : {QStringLiteral("Alpha"), QStringLiteral("Archived workspace")}) {
            const bool disabled = name.startsWith(QStringLiteral("Archived"));
            const QString id = disabled ? QStringLiteral("archived") : QStringLiteral("alpha");
            QJsonObject root{{"id", QString(id + QStringLiteral("-root"))}, {"eTag", "root-tag"}, {"webDavUrl", server.url("/dav/" + id + "/").toString()},
                {"webUrl", server.url("/web/" + id).toString()}};
            if (disabled) root.insert("deleted", QJsonObject{{"state", "trashed"}});
            server.extraDrives.append(QJsonObject{{"id", id}, {"name", name}, {"driveType", "project"}, {"eTag", "drive-tag"}, {"root", root}});
        }
        auto *account = new Account(QUuid::createUuid(), QStringLiteral("test"), server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveFileBrowser browser(&state);
        browser.resize(1050, 760); browser.show();
        auto *globalSearch = browser.takeSearchField();
        const auto searchOwner = qScopeGuard([globalSearch] { delete globalSearch; });
        browser.showWorkspaces();
        auto *view = browser.findChild<QListView *>("driveWorkspacesView");
        auto *controls = browser.findChild<QWidget *>("driveWorkspaceControls");
        auto *includeDisabled = browser.findChild<QPushButton *>("driveIncludeDisabled");
        auto *search = browser.findChild<QLineEdit *>("driveWorkspaceSearch");
        auto *sort = browser.findChild<QComboBox *>("driveWorkspaceSort");
        auto *all = browser.findChild<QCheckBox *>("driveWorkspaceSelectAll");
        auto *count = browser.findChild<QLabel *>("driveWorkspaceCount");
        auto *open = browser.findChild<QPushButton *>("driveOpen");
        QVERIFY(view && controls && includeDisabled && search && sort && all && count && open);
        QTRY_COMPARE(view->model()->rowCount(), 2);
        QVERIFY(controls->isVisible());
        QCOMPARE(view->model()->index(0, 0).data().toString(), QStringLiteral("Alpha"));
        sort->setCurrentIndex(1);
        QCOMPARE(view->model()->index(0, 0).data().toString(), QStringLiteral("Team files"));
        sort->setCurrentIndex(0);
        includeDisabled->click();
        QCOMPARE(view->model()->rowCount(), 3);
        browser.resize(650, 760);
        QTRY_VERIFY(view->height() > 350); // Wrapped cards receive space for both rows.
        QTRY_COMPARE(view->verticalScrollBar()->maximum(), 0);
        browser.resize(1050, 760);
        QTRY_VERIFY(view->height() < 240);
        auto *manager = account->spacesManager();
        QCOMPARE(manager->spaces().size(), 3); // Personal and two active projects only.
        QCOMPARE(manager->disabledDrives().size(), 1);
        for (auto *space : manager->spaces()) QVERIFY(!space->disabled());
        search->setText(QStringLiteral("Archived"));
        QCOMPARE(globalSearch->text(), search->text());
        QCOMPARE(view->model()->rowCount(), 1);
        QVERIFY(view->model()->index(0, 0).data(UDriveFileBrowser::SpaceDisabledRole).toBool());
        all->click();
        QCOMPARE(view->selectionModel()->selectedRows(0).size(), 1);
        QCOMPARE(all->checkState(), Qt::Checked);
        QVERIFY(!open->isEnabled());
        Q_EMIT view->activated(view->model()->index(0, 0));
        QVERIFY(browser.isWorkspacesOverview());
        all->click();
        globalSearch->clear();
        QCOMPARE(search->text(), QString());
        QCOMPARE(view->model()->rowCount(), 3);
        QTRY_VERIFY(count->text().contains(QStringLiteral("3")));
        QTRY_VERIFY(view->height() < 240); // One row, followed immediately by its count.
        QVERIFY(count->mapTo(&browser, QPoint()).y() >= view->mapTo(&browser, QPoint(0, view->height())).y());
        if (qEnvironmentVariableIsSet("UDRIVE_WORKSPACE_TEST_CAPTURE")) {
            auto *accounts = AccountManager::instance();
            auto *captureAccount = new Account(QUuid::createUuid(), "test", server.url());
            captureAccount->setCredentials(new Credentials("browser-test-token", "test-refresh-token", captureAccount));
            captureAccount->credentials()->fetchFromKeychain();
            auto *captureState = accounts->addAccount(captureAccount);
            {
                const QPalette originalPalette = qApp->palette();
                const auto restorePalette = qScopeGuard([originalPalette] { qApp->setPalette(originalPalette); });
                QPalette dark = originalPalette;
                dark.setColor(QPalette::Window, QColor("#292929")); dark.setColor(QPalette::WindowText, QColor("#EDEEF0"));
                dark.setColor(QPalette::Base, QColor("#292929")); dark.setColor(QPalette::Text, QColor("#EDEEF0"));
                dark.setColor(QPalette::Button, QColor("#323232")); dark.setColor(QPalette::ButtonText, QColor("#EDEEF0"));
                qApp->setPalette(dark);
                SettingsDialog window(nullptr); window.resize(1250, 850); window.show();
                window.setCurrentPage(SettingsDialog::SettingsPage::Workspaces);
                auto *catalog = window.findChild<QListView *>("driveWorkspacesView");
                QTRY_VERIFY(catalog && catalog->isVisible());
                QTRY_COMPARE(catalog->model()->rowCount(), 2);
                QTRY_COMPARE(catalog->verticalScrollBar()->maximum(), 0);
                QTest::qWait(100);
                window.grab().save("/tmp/udrive-workspaces-layout.png");
                window.findChild<QPushButton *>("driveIncludeDisabled")->click();
                QTest::qWait(100);
                window.grab().save("/tmp/udrive-workspaces-disabled.png");
                auto *tableAction = window.findChild<QAction *>("driveWorkspaceViewTable");
                tableAction->trigger(); QTest::qWait(100);
                window.grab().save("/tmp/udrive-workspaces-table.png");
                auto *viewButton = window.findChild<QToolButton *>("driveWorkspaceViewMode");
                viewButton->menu()->popup(viewButton->mapToGlobal(QPoint(0, viewButton->height())));
                QTest::qWait(50); viewButton->menu()->grab().save("/tmp/udrive-workspaces-view-menu.png"); viewButton->menu()->close();
                auto *table = window.findChild<QTreeView *>("driveWorkspacesTable");
                table->selectionModel()->select(table->model()->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                QTest::qWait(50); window.grab().save("/tmp/udrive-workspaces-toolbar.png");
                QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, table->visualRect(table->model()->index(0, 5)).center());
                auto *actions = window.findChild<QMenu *>("driveWorkspaceActionsMenu");
                QTRY_VERIFY(actions && actions->isVisible()); QTest::qWait(50);
                actions->grab().save("/tmp/udrive-workspaces-actions-menu.png");
                auto *details = actions->findChild<QAction *>("driveWorkspace_details"); actions->close(); details->trigger();
                auto *detailsDialog = window.findChild<QDialog *>("driveWorkspaceDetailsDialog");
                QTRY_VERIFY(detailsDialog && detailsDialog->isVisible()); QTest::qWait(50);
                detailsDialog->grab().save("/tmp/udrive-workspaces-details.png"); detailsDialog->close();
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, table->visualRect(table->model()->index(0, 5)).center());
                actions = window.findChild<QMenu *>("driveWorkspaceActionsMenu"); QTRY_VERIFY(actions && actions->isVisible());
                auto *description = actions->findChild<QAction *>("driveWorkspace_description"); actions->close(); description->trigger();
                auto *descriptionDialog = window.findChild<QDialog *>("driveWorkspaceDescriptionDialog");
                QTRY_VERIFY(descriptionDialog && descriptionDialog->isVisible()); QTest::qWait(50);
                descriptionDialog->grab().save("/tmp/udrive-workspaces-description.png"); descriptionDialog->close();
                window.findChild<QAction *>("driveWorkspaceViewTiles")->trigger();
            }
            accounts->deleteAccount(captureState);
        }
        includeDisabled->click();
        QCOMPARE(view->model()->rowCount(), 2);
        all->click();
        QCOMPARE(view->selectionModel()->selectedRows(0).size(), 2);
        all->click();
        QCOMPARE(view->selectionModel()->selectedRows(0).size(), 0);
        search->setText(QStringLiteral("No match"));
        QCOMPARE(view->model()->rowCount(), 0);
        QVERIFY(!all->isEnabled());
        QCOMPARE(count->text(), QStringLiteral("No workspaces found."));
        search->clear();
        browser.showPersonal();
        QVERIFY(!controls->isVisible());
        QVERIFY(!count->isVisible());
    }

    void workspaceViewSwitcherAndDetails()
    {
        auto settings = ConfigFile::settingsWithGroup(QStringLiteral("UDriveFileBrowser"));
        const auto previous = settings->value(QStringLiteral("workspaceViewMode"));
        const auto restore = qScopeGuard([previous] {
            auto settings = ConfigFile::settingsWithGroup(QStringLiteral("UDriveFileBrowser"));
            if (previous.isValid()) settings->setValue(QStringLiteral("workspaceViewMode"), previous);
            else settings->remove(QStringLiteral("workspaceViewMode"));
        });
        settings->setValue(QStringLiteral("workspaceViewMode"), QStringLiteral("tiles"));
        DriveServer server;
        const QJsonObject alice{{"user", QJsonObject{{"id", "alice"}, {"displayName", "Alice Manager"}}}};
        const QJsonObject bob{{"user", QJsonObject{{"id", "bob"}, {"displayName", "Bob Member"}}}};
        const QJsonArray permissions{
            QJsonObject{{"roles", QJsonArray{"manager"}}, {"grantedToIdentities", QJsonArray{alice}}},
            QJsonObject{{"roles", QJsonArray{"editor"}}, {"grantedToIdentities", QJsonArray{alice, bob}}}};
        server.extraDrives.append(QJsonObject{{"id", "alpha"}, {"name", "Alpha"}, {"driveType", "project"}, {"eTag", "drive-tag"},
            {"lastModifiedDateTime", "2026-10-16T12:00:00Z"}, {"quota", QJsonObject{{"remaining", 999000000}}},
            {"root", QJsonObject{{"id", "alpha-root"}, {"eTag", "root-tag"}, {"permissions", permissions},
                {"webDavUrl", server.url("/dav/alpha/").toString()}}}});
        auto *account = new Account(QUuid::createUuid(), QStringLiteral("test"), server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveFileBrowser browser(&state); browser.resize(1100, 760); browser.show();
        auto *search = browser.takeSearchField();
        const auto searchOwner = qScopeGuard([search] { delete search; });
        browser.showWorkspaces();
        auto *tiles = browser.findChild<QListView *>("driveWorkspacesView");
        auto *table = browser.findChild<QTreeView *>("driveWorkspacesTable");
        auto *button = browser.findChild<QToolButton *>("driveWorkspaceViewMode");
        auto *tableAction = browser.findChild<QAction *>("driveWorkspaceViewTable");
        auto *tileAction = browser.findChild<QAction *>("driveWorkspaceViewTiles");
        auto *all = browser.findChild<QCheckBox *>("driveWorkspaceHeaderSelectAll");
        auto *count = browser.findChild<QLabel *>("driveWorkspaceCount");
        QVERIFY(tiles && table && button && tableAction && tileAction && all && count);
        QTRY_COMPARE(tiles->model()->rowCount(), 2);
        QVERIFY(tiles->isVisible()); QVERIFY(tileAction->isChecked());
        tableAction->trigger();
        QVERIFY(table->isVisible()); QVERIFY(!tiles->isVisible()); QVERIFY(tableAction->isChecked());
        QCOMPARE(table->selectionModel(), tiles->selectionModel());
        QCOMPARE(table->model()->columnCount(), 6);
        QCOMPARE(table->model()->headerData(3, Qt::Horizontal).toString(), QStringLiteral("Remaining quota"));
        const auto alpha = table->model()->index(0, 0);
        QCOMPARE(alpha.data().toString(), QStringLiteral("Alpha"));
        QCOMPARE(alpha.siblingAtColumn(1).data().toString(), QStringLiteral("Alice Manager"));
        QCOMPARE(alpha.siblingAtColumn(2).data().toString(), QStringLiteral("2")); // Unique members, including the manager.
        QVERIFY(alpha.siblingAtColumn(3).data().toString().contains(QStringLiteral("999")));
        QVERIFY(alpha.siblingAtColumn(3).data().toString().contains(QStringLiteral("MB")));
        QVERIFY(alpha.siblingAtColumn(4).data().toString() != QStringLiteral("—"));
        QCOMPARE(table->model()->index(1, 1).data().toString(), QStringLiteral("—")); // Missing metadata stays unknown.
        all->click();
        QCOMPARE(table->selectionModel()->selectedRows().size(), 2);
        QVERIFY(count->text().contains(QStringLiteral("2")));
        const QRect rect = table->visualRect(alpha);
        QTest::mousePress(table->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(rect.left() + 17, rect.center().y()));
        QTest::mouseMove(table->viewport(), rect.center());
        QTest::mouseRelease(table->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
        QCOMPARE(table->selectionModel()->selectedRows().size(), 1);
        QCOMPARE(all->checkState(), Qt::PartiallyChecked);
        QVERIFY(count->text().contains(QStringLiteral("1")));
        tileAction->trigger();
        QCOMPARE(tiles->selectionModel()->selectedRows().size(), 1);
        tableAction->trigger();
        search->setText(QStringLiteral("Alpha"));
        QCOMPARE(table->model()->rowCount(), 1);
        all->click();
        QCOMPARE(table->selectionModel()->selectedRows().size(), 1);
        const QRect actions = table->visualRect(table->model()->index(0, 5));
        QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, actions.center());
        auto *menu = browser.findChild<QMenu *>("driveWorkspaceActionsMenu");
        QTRY_VERIFY(menu && menu->isVisible());
        auto *members = menu->findChild<QAction *>("driveWorkspaceMembers");
        QTRY_VERIFY(members && members->isEnabled()); menu->close();
        members->trigger();
        auto *dialog = browser.findChild<UDriveShareDialog *>();
        QTRY_VERIFY(dialog && dialog->isVisible()); dialog->close();
        search->clear();
        table->sortByColumn(0, Qt::DescendingOrder);
        QCOMPARE(table->model()->index(0, 0).data().toString(), QStringLiteral("Team files"));
        browser.showPersonal();
        QVERIFY(!button->isVisible());
        browser.showWorkspaces();
        QVERIFY(table->isVisible());
        UDriveFileBrowser reopened(&state);
        auto *newSearch = reopened.takeSearchField();
        const auto newSearchOwner = qScopeGuard([newSearch] { delete newSearch; });
        reopened.showWorkspaces(); reopened.show();
        QVERIFY(reopened.findChild<QAction *>("driveWorkspaceViewTable")->isChecked());
        QVERIFY(reopened.findChild<QTreeView *>("driveWorkspacesTable")->isVisible());
        state.signOutByUi();
        QTRY_COMPARE(table->model()->rowCount(), 0);
        QVERIFY(!all->isEnabled()); QVERIFY(!table->isEnabled());
    }

    void workspaceRoleControls_data()
    {
        QTest::addColumn<QJsonArray>("global"); QTest::addColumn<QJsonArray>("effective");
        QTest::addColumn<bool>("create"); QTest::addColumn<bool>("manage"); QTest::addColumn<bool>("quota");
        const QJsonArray read{"libre.graph/driveItem/content/read", "libre.graph/driveItem/permissions/read"};
        auto editor = read; editor.append("libre.graph/driveItem/upload/create"); editor.append("libre.graph/driveItem/path/update");
        auto manager = editor; manager.append("libre.graph/driveItem/permissions/delete");
        QTest::newRow("viewer") << QJsonArray{} << read << false << false << false;
        QTest::newRow("editor") << QJsonArray{} << editor << false << false << false;
        QTest::newRow("workspace-manager") << QJsonArray{} << manager << false << true << false;
        QTest::newRow("user-with-create-rights") << QJsonArray{"Drives.Create.all"} << read << true << false << false;
        QTest::newRow("administrator") << QJsonArray{"Drives.Create.all", "Drives.ReadWriteEnabled.all", "Drives.ReadWriteProjectQuota.all"} << manager << true << true << true;
    }
    void workspaceRoleControls()
    {
        const auto previousView = ConfigFile::settingsWithGroup(QStringLiteral("UDriveFileBrowser"))->value(QStringLiteral("workspaceViewMode"));
        const auto restoreView = qScopeGuard([previousView] {
            auto settings = ConfigFile::settingsWithGroup(QStringLiteral("UDriveFileBrowser"));
            if (previousView.isValid()) settings->setValue(QStringLiteral("workspaceViewMode"), previousView);
            else settings->remove(QStringLiteral("workspaceViewMode"));
        });
        QFETCH(QJsonArray, global); QFETCH(QJsonArray, effective); QFETCH(bool, create); QFETCH(bool, manage); QFETCH(bool, quota);
        DriveServer server; server.globalPermissions = global; server.workspacePermissions.insert("@libre.graph.permissions.actions.allowedValues", effective);
        auto *account = new Account(QUuid::createUuid(), "test", server.url()); account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account)); account->credentials()->fetchFromKeychain();
        AccountState state(account); UDriveFileBrowser browser(&state); browser.resize(1100, 760); browser.show();
        browser.showWorkspacesOverview(); browser.findChild<QAction *>("driveWorkspaceViewTable")->trigger();
        auto *table = browser.findChild<QTreeView *>("driveWorkspacesTable");
        auto *operations = browser.findChild<UDriveWorkspaceActions *>(); QVERIFY(operations);
        QTRY_VERIFY(table->model()->rowCount() > 0); QTRY_VERIFY(operations->permissionsReady()); QCOMPARE(browser.canCreate(), create);
        table->selectionModel()->select(table->model()->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        QCOMPARE(browser.findChild<QAction *>("driveWorkspaceBulk_disable")->isEnabled(), manage);
        QCOMPARE(browser.findChild<QAction *>("driveWorkspaceBulk_quota")->isEnabled(), quota);
        QCOMPARE(browser.findChild<QAction *>("driveWorkspaceBulk_duplicate")->isEnabled(), create);
        QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, table->visualRect(table->model()->index(0, 5)).center());
        QPointer<QMenu> menu = browser.findChild<QMenu *>("driveWorkspaceActionsMenu"); QTRY_VERIFY(menu && menu->isVisible());
        QCOMPARE(menu->findChild<QAction *>("driveWorkspace_rename")->isEnabled(), manage);
        QCOMPARE(menu->findChild<QAction *>("driveWorkspace_quota")->isEnabled(), quota);
        menu->close(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (!create) { const int before = server.requests.size(); QVERIFY(!browser.createNewWorkspace()); QCOMPARE(server.requests.size(), before); }
        // Refresh immediately disables old grants, then installs the reduced access.
        server.globalPermissions = {}; server.workspacePermissions.insert("@libre.graph.permissions.actions.allowedValues", QJsonArray{});
        operations->refreshPermissions(); QVERIFY(!browser.canCreate()); QTRY_VERIFY(operations->permissionsReady());
        QVERIFY(!browser.findChild<QAction *>("driveWorkspaceBulk_disable")->isEnabled());
        QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, table->visualRect(table->model()->index(0, 5)).center());
        menu = browser.findChild<QMenu *>("driveWorkspaceActionsMenu"); QTRY_VERIFY(menu && menu->isVisible());
        QVERIFY(!menu->findChild<QAction *>("driveWorkspace_rename")->isEnabled());
        const int before = server.requests.size(); menu->findChild<QAction *>("driveWorkspace_rename")->trigger(); QCOMPARE(server.requests.size(), before);
        menu->close();
    }
    void workspaceManagementActions()
    {
        const auto previousView = ConfigFile::settingsWithGroup(QStringLiteral("UDriveFileBrowser"))->value(QStringLiteral("workspaceViewMode"));
        const auto restoreView = qScopeGuard([previousView] {
            auto settings = ConfigFile::settingsWithGroup(QStringLiteral("UDriveFileBrowser"));
            if (previousView.isValid()) settings->setValue(QStringLiteral("workspaceViewMode"), previousView);
            else settings->remove(QStringLiteral("workspaceViewMode"));
        });
        DriveServer server;
        server.extraDrives.append(QJsonObject{{"id", "alpha"}, {"name", "Alpha"}, {"driveType", "project"}, {"eTag", "drive-tag"},
            {"description", "Team notes"}, {"quota", QJsonObject{{"total", 15000000000LL}}},
            {"root", QJsonObject{{"id", "alpha-root"}, {"eTag", "root-tag"}, {"webDavUrl", server.url("/dav/alpha").toString()}}}});
        auto *account = new Account(QUuid::createUuid(), "test", server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account)); account->credentials()->fetchFromKeychain();
        AccountState state(account); UDriveFileBrowser browser(&state); browser.resize(1100, 760); browser.show();
        auto *search = browser.takeSearchField(); const auto cleanup = qScopeGuard([search] { delete search; });
        browser.showWorkspaces(); browser.findChild<QAction *>("driveWorkspaceViewTable")->trigger();
        auto *table = browser.findChild<QTreeView *>("driveWorkspacesTable");
        auto *toolbar = browser.findChild<QWidget *>("driveWorkspaceBulkActions");
        auto *count = browser.findChild<QLabel *>("driveWorkspaceCount");
        auto *duplicate = browser.findChild<QAction *>("driveWorkspaceBulk_duplicate");
        auto *quota = browser.findChild<QAction *>("driveWorkspaceBulk_quota");
        QVERIFY(table && toolbar && count && duplicate && quota);
        QTRY_COMPARE(table->model()->rowCount(), 2); QVERIFY(!toolbar->isVisible());
        table->selectionModel()->select(table->model()->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        QVERIFY(toolbar->isVisible()); QTRY_VERIFY(duplicate->isEnabled()); QTRY_VERIFY(quota->isEnabled());
        auto showMenu = [&]() -> QMenu * {
            const QRect rect = table->visualRect(table->model()->index(0, 5));
            QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
            return browser.findChild<QMenu *>("driveWorkspaceActionsMenu");
        };
        auto *menu = showMenu(); QTRY_VERIFY(menu && menu->isVisible());
        for (const auto *key : {"rename", "duplicate", "description", "subtitle", "image", "icon", "quota", "disable", "trash", "details"}) {
            auto *action = menu->findChild<QAction *>(QStringLiteral("driveWorkspace_") + QLatin1String(key));
            QVERIFY2(action && action->isEnabled(), key); QVERIFY(!action->text().endsWith(QStringLiteral("…")));
        }
        QVERIFY(!menu->findChild<QAction *>("driveWorkspace_download")->isEnabled()); // No advertised archive service.
        auto *details = menu->findChild<QAction *>("driveWorkspace_details"); menu->close(); details->trigger();
        auto *dialog = browser.findChild<QDialog *>("driveWorkspaceDetailsDialog"); QTRY_VERIFY(dialog && dialog->isVisible());
        bool nameFound = false;
        for (auto *label : dialog->findChildren<QLabel *>()) if (label->text() == QStringLiteral("Alpha")) { QCOMPARE(label->textFormat(), Qt::PlainText); nameFound = true; }
        QVERIFY(nameFound); dialog->close(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        menu = showMenu(); QTRY_VERIFY(menu && menu->isVisible()); auto *description = menu->findChild<QAction *>("driveWorkspace_description"); menu->close(); description->trigger();
        auto *editor = browser.findChild<QPlainTextEdit *>("driveWorkspaceDescriptionText"); QTRY_VERIFY(editor && editor->isVisible());
        QVERIFY(editor->toPlainText().isEmpty()); editor->setPlainText(QStringLiteral("# Notes"));
        browser.showPersonal(); QTRY_VERIFY(!editor->isVisible()); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        browser.showWorkspaces(); browser.findChild<QAction *>("driveWorkspaceViewTable")->trigger(); QTRY_COMPARE(table->model()->rowCount(), 2);
        table->selectionModel()->select(table->model()->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        const int before = server.requests.size();
        QTimer::singleShot(0, &browser, [&] { auto *input = browser.findChild<UDriveInputDialog *>(); QVERIFY(input); input->reject(); });
        browser.findChild<QAction *>("driveWorkspaceBulk_disable")->trigger(); QCOMPARE(server.requests.size(), before);
        server.workspaceMutationDelay = 150;
        QTimer::singleShot(0, &browser, [&] { auto *input = browser.findChild<UDriveInputDialog *>(); QVERIFY(input); input->findChild<QLineEdit *>()->setText(QStringLiteral("2.5")); input->accept(); });
        quota->trigger(); QVERIFY(!quota->isEnabled()); QVERIFY(!table->isEnabled()); QVERIFY(!browser.findChild<QLineEdit *>("driveWorkspaceSearch")->isEnabled());
        QTRY_VERIFY(browser.findChild<QLabel *>("udriveToastMessage")
            && browser.findChild<QLabel *>("udriveToastMessage")->text() == QStringLiteral("Workspace changes saved."));
        QCOMPARE(QJsonDocument::fromJson(server.requestBodies.value("PATCH /graph/v1.0/drives/alpha")).object().value("quota").toObject().value("total").toInteger(), 2500000000LL);
        QTRY_VERIFY(table->isEnabled());
        table->selectionModel()->select(table->model()->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        server.workspaceMutationStatus = 403;
        QTimer::singleShot(0, &browser, [&] { auto *input = browser.findChild<UDriveInputDialog *>(); QVERIFY(input); input->findChild<QLineEdit *>()->setText(QStringLiteral("3")); input->accept(); });
        quota->trigger();
        QTRY_VERIFY(browser.findChild<QLabel *>("udriveToastMessage")
            && browser.findChild<QLabel *>("udriveToastMessage")->text().contains(QStringLiteral("Permission denied")));
        QVERIFY(table->isEnabled());
        QCOMPARE(server.extraDrives.first().toObject().value("quota").toObject().value("total").toInteger(), 2500000000LL);
        QVERIFY(server.authenticated);
    }


    void workspaceInteractionRegressions_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const char *name : {"checkbox", "members", "signout", "refresh-shortcut"})
            QTest::newRow(name) << QString::fromUtf8(name);
    }

    void workspaceInteractionRegressions()
    {
        QFETCH(QString, scenario);
        DriveServer server;
        server.teamRootId = QStringLiteral("team-root"); // Graph drive ID differs from DAV root ID.
        auto *account = new Account(QUuid::createUuid(), QStringLiteral("test"), server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveFileBrowser browser(&state);
        browser.resize(1050, 760);
        browser.show();
        auto *search = browser.takeSearchField();
        const auto searchOwner = qScopeGuard([search] { delete search; });
        auto *tree = browser.findChild<QTreeView *>(QStringLiteral("driveFiles"));
        QTRY_VERIFY(tree->model()->rowCount() > 0);
        browser.showWorkspaces();
        auto *view = browser.findChild<QListView *>(QStringLiteral("driveWorkspacesView"));
        QTRY_VERIFY(view->isVisible());
        QTRY_COMPARE(view->model()->rowCount(), 1);
        QTRY_VERIFY(browser.findChild<UDriveWorkspaceActions *>()->permissionsReady());
        const auto index = view->model()->index(0, 0);
        const QRect rect = view->visualRect(index);
        if (scenario == QStringLiteral("checkbox")) {
            const QPoint check = rect.topLeft() + QPoint(27, 27);
            QTest::mousePress(view->viewport(), Qt::LeftButton, Qt::NoModifier, check);
            QTest::mouseMove(view->viewport(), rect.center());
            QTest::mouseRelease(view->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
            QCOMPARE(view->selectionModel()->selectedRows(0).size(), 1);
            QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, check);
            QCOMPARE(view->selectionModel()->selectedRows(0).size(), 0);
        } else if (scenario == QStringLiteral("members")) {
            QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(rect.right() - 50, rect.bottom() - 27));
            auto *dialog = browser.findChild<UDriveShareDialog *>();
            QVERIFY(dialog);
            auto *input = dialog->findChild<QLineEdit *>(QStringLiteral("shareUserInput"));
            QVERIFY(input);
            QTRY_VERIFY(input->isEnabled());
            QVERIFY(server.requests.contains({"GET", QStringLiteral("/graph/v1beta1/drives/team/root/permissions")}));
            input->setText(QStringLiteral("Alice"));
            QTRY_VERIFY(!server.recipientQueries.isEmpty());
            QCOMPARE(server.sharePosts, 0); // Workspace roots never use the file-sharing OCS endpoint.
        } else if (scenario == QStringLiteral("signout")) {
            view->selectionModel()->select(index, QItemSelectionModel::Select);
            state.signOutByUi();
            QCOMPARE(view->model()->rowCount(), 0);
            QVERIFY(!view->isEnabled());
            QVERIFY(!browser.findChild<QPushButton *>(QStringLiteral("driveOpen"))->isEnabled());
            // A late spaces update must not repopulate a signed-out account.
            account->spacesManager()->updated(account);
            QCOMPARE(view->model()->rowCount(), 0);
            const int requests = server.requests.size();
            browser.openWorkspace(QStringLiteral("team-root"));
            QTest::qWait(50);
            QCOMPARE(server.requests.size(), requests);
        } else {
            const int before = server.requests.count({"GET", QStringLiteral("/graph/v1.0/me/drives")});
            for (auto *action : browser.actions())
                if (action->shortcut() == QKeySequence(QKeySequence::Refresh)) action->trigger();
            QTRY_VERIFY(server.requests.count({"GET", QStringLiteral("/graph/v1.0/me/drives")}) > before);
            QTRY_COMPARE(view->model()->rowCount(), 1);
            QVERIFY(view->isVisible());
            QVERIFY(!tree->isVisible());
        }
    }

    void rejectedFavoriteRestoresState_data()
    {
        QTest::addColumn<bool>("multistatus");
        QTest::newRow("http-error") << false;
        QTest::newRow("property-error") << true;
    }

    void rejectedFavoriteRestoresState()
    {
        QFETCH(bool, multistatus);
        DriveServer server;
        server.rejectFavorite = !multistatus;
        if (multistatus) {
            server.mutationVerb = "PROPPATCH";
            server.mutationResponse = "<d:multistatus xmlns:d=\"DAV:\"><d:response><d:propstat><d:prop/>"
                "<d:status>HTTP/1.1 403 Forbidden</d:status></d:propstat></d:response></d:multistatus>";
        }
        auto *account = new Account(QUuid::createUuid(), QStringLiteral("test"), server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveFileBrowser browser(&state);
        auto *tree = browser.findChild<QTreeView *>(QStringLiteral("driveFiles"));
        QTRY_COMPARE(tree->model()->rowCount(), 7);
        QVERIFY(select(tree, QStringLiteral("Docs")).isValid());
        auto *favorite = browser.findChild<QAction *>(QStringLiteral("driveFavorite"));
        QCOMPARE(favorite->text(), QStringLiteral("Add to Favorites"));
        QSignalSpy finished(&browser, &UDriveFileBrowser::operationFinished);
        QVERIFY(browser.setSelectedFavorite(true));
        QTRY_COMPARE(finished.count(), 1);
        QVERIFY(!finished.first().first().toBool());
        QVERIFY(!server.favorites.contains(QStringLiteral("/dav/personal/Docs")));
        QCOMPARE(favorite->text(), QStringLiteral("Add to Favorites"));
    }

    void workspaceMembersAddAndRemove_data()
    {
        QTest::addColumn<int>("type");
        QTest::addColumn<bool>("failInvite");
        QTest::newRow("user") << 0 << false;
        QTest::newRow("group") << 1 << false;
        QTest::newRow("invite-error") << 0 << true;
    }

    void workspaceMembersAddAndRemove()
    {
        QFETCH(int, type);
        QFETCH(bool, failInvite);
        DriveServer server;
        server.workspaceInviteStatus = failInvite ? 403 : 201;
        const QJsonObject recipient{{"label", "Alice <Admin>"},
            {"value", QJsonObject{{"shareType", type}, {"shareWith", "directory-id"}}}};
        server.recipientData = {{type == 1 ? "groups" : "users", QJsonArray{recipient}}};
        auto *account = new Account(QUuid::createUuid(), QStringLiteral("test"), server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveShareDialog dialog(&state, QStringLiteral("Team files"), true, {}, {}, QStringLiteral("team"), nullptr,
            UDriveShareDialog::Target::Workspace);
        dialog.show();
        auto *input = dialog.findChild<QLineEdit *>(QStringLiteral("shareUserInput"));
        auto *role = dialog.findChild<QComboBox *>(QStringLiteral("shareRoleCombo"));
        auto *confirm = dialog.findChild<QPushButton *>(QStringLiteral("shareConfirmButton"));
        auto *completer = dialog.findChild<QCompleter *>();
        QTRY_VERIFY(input->isEnabled());
        QCOMPARE(role->count(), 2);
        role->setCurrentIndex(role->findData(QStringLiteral("editor-role")));
        input->setText(QStringLiteral("alice"));
        QTRY_COMPARE(completer->completionModel()->rowCount(), 1);
        QVERIFY(QMetaObject::invokeMethod(completer, "activated", Qt::DirectConnection,
            Q_ARG(QModelIndex, completer->completionModel()->index(0, 0))));
        QVERIFY(confirm->isEnabled());
        QVERIFY(server.workspaceInvitations.isEmpty());
        confirm->click();
        QTRY_COMPARE(server.workspaceInvitations.size(), 1);
        const auto invitation = server.workspaceInvitations.first();
        QCOMPARE(invitation.value("roles").toArray(), QJsonArray{"editor-role"});
        const auto target = invitation.value("recipients").toArray().first().toObject();
        QCOMPARE(target.value("objectId").toString(), QStringLiteral("directory-id"));
        QCOMPARE(target.value("@libre.graph.recipient.type").toString(), type == 1 ? QStringLiteral("group") : QStringLiteral("user"));
        QCOMPARE(server.sharePosts, 0);
        if (failInvite) {
            QTRY_VERIFY(confirm->isEnabled());
            QVERIFY(dialog.findChild<QLabel *>(QStringLiteral("shareStatusLabel"))->text().contains(QStringLiteral("Could not add")));
            QVERIFY(!dialog.findChild<QPushButton *>(QStringLiteral("workspaceRemoveMember")));
        } else {
            auto removeButton = [&] { return dialog.findChild<QPushButton *>(QStringLiteral("workspaceRemoveMember")); };
            QTRY_VERIFY(removeButton());
            QTRY_VERIFY(input->isEnabled());
            removeButton()->click();
            QTRY_COMPARE(server.workspaceRemovals.size(), 1);
            QCOMPARE(server.workspaceRemovals.first(), (type == 1 ? QStringLiteral("g:") : QStringLiteral("u:")) + QStringLiteral("directory-id"));
            QTRY_VERIFY(!removeButton());
            QVERIFY(server.workspacePermissions.value("value").toArray().isEmpty());
        }
        QVERIFY(server.authenticated);
    }


    void workspaceMemberAccessAndCancellation_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const char *scenario : {"read-only", "denied", "signout", "close"})
            QTest::newRow(scenario) << QString::fromUtf8(scenario);
    }

    void workspaceMemberAccessAndCancellation()
    {
        QFETCH(QString, scenario);
        DriveServer server;
        if (scenario == QStringLiteral("read-only"))
            server.workspacePermissions.insert("@libre.graph.permissions.actions.allowedValues", QJsonArray{});
        if (scenario == QStringLiteral("denied")) server.workspaceStatus = 403;
        const bool delayed = scenario == QStringLiteral("signout") || scenario == QStringLiteral("close");
        if (delayed) server.workspaceDelay = 300;
        auto *account = new Account(QUuid::createUuid(), QStringLiteral("test"), server.url());
        account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
        account->credentials()->fetchFromKeychain();
        AccountState state(account);
        UDriveShareDialog dialog(&state, QStringLiteral("Team files"), true, {}, {}, QStringLiteral("team"), nullptr,
            UDriveShareDialog::Target::Workspace);
        dialog.show();
        auto *input = dialog.findChild<QLineEdit *>(QStringLiteral("shareUserInput"));
        auto *confirm = dialog.findChild<QPushButton *>(QStringLiteral("shareConfirmButton"));
        auto *status = dialog.findChild<QLabel *>(QStringLiteral("shareStatusLabel"));
        QTRY_VERIFY(server.requests.contains({"GET", QStringLiteral("/graph/v1beta1/drives/team/root/permissions")}));
        if (delayed) {
            if (scenario == QStringLiteral("signout")) state.signOutByUi();
            else dialog.reject();
            QTRY_VERIFY(!dialog.isVisible());
            QTest::qWait(400);
        } else {
            QTRY_VERIFY(!status->text().startsWith(QStringLiteral("Loading")));
            QVERIFY(!input->isEnabled());
            QVERIFY(!dialog.findChild<QPushButton *>(QStringLiteral("workspaceRemoveMember")));
            input->setText(QStringLiteral("alice"));
            QTest::qWait(350);
        }
        QVERIFY(!confirm->isEnabled());
        QVERIFY(server.recipientQueries.isEmpty());
        QVERIFY(server.workspaceInvitations.isEmpty());
        QVERIFY(server.workspaceRemovals.isEmpty());
    }

    void workspaceMemberAccountIsolation()
    {
        DriveServer firstServer;
        DriveServer secondServer;
        firstServer.workspaceDelay = 200;
        auto member = [](const QString &name) {
            return QJsonObject{{"id", "u:member"}, {"roles", QJsonArray{"viewer-role"}},
                {"grantedToV2", QJsonObject{{"user", QJsonObject{{"id", "member"}, {"displayName", name}}}}}};
        };
        firstServer.workspacePermissions.insert("value", QJsonArray{member(QStringLiteral("First account member"))});
        secondServer.workspacePermissions.insert("value", QJsonArray{member(QStringLiteral("Second account member"))});
        auto accountFor = [](DriveServer &server) {
            auto *account = new Account(QUuid::createUuid(), QStringLiteral("test"), server.url());
            account->setCredentials(new Credentials("browser-test-token", "test-refresh-token", account));
            account->credentials()->fetchFromKeychain();
            return account;
        };
        AccountState firstState(accountFor(firstServer));
        AccountState secondState(accountFor(secondServer));
        UDriveShareDialog first(&firstState, QStringLiteral("Team"), true, {}, {}, QStringLiteral("team"), nullptr,
            UDriveShareDialog::Target::Workspace);
        UDriveShareDialog second(&secondState, QStringLiteral("Team"), true, {}, {}, QStringLiteral("team"), nullptr,
            UDriveShareDialog::Target::Workspace);
        first.show();
        second.show();
        auto contains = [](QDialog &dialog, const QString &text) {
            for (auto *label : dialog.findChildren<QLabel *>()) if (label->text() == text) return true;
            return false;
        };
        QTRY_VERIFY(contains(first, QStringLiteral("First account member")));
        QTRY_VERIFY(contains(second, QStringLiteral("Second account member")));
        QVERIFY(!contains(first, QStringLiteral("Second account member")));
        QVERIFY(!contains(second, QStringLiteral("First account member")));
        firstState.signOutByUi();
        QTRY_VERIFY(!first.isVisible());
        QVERIFY(second.isVisible());
        QVERIFY(second.findChild<QLineEdit *>(QStringLiteral("shareUserInput"))->isEnabled());
        QVERIFY(firstServer.workspaceInvitations.isEmpty() && secondServer.workspaceInvitations.isEmpty());
    }

};

QTEST_MAIN(TestUDriveFileBrowser)
#include "testudrivefilebrowser.moc"
