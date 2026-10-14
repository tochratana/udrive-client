// SPDX-License-Identifier: GPL-2.0-or-later
#include "gui/udrivefilebrowser.h"
#include "gui/settingsdialog.h"
#include "gui/accountmanager.h"
#include "gui/translations.h"
#include <QTranslator>
#include <QUrlQuery>
#include <QScrollBar>
#include <QHeaderView>
#include <QProcess>
#include <QElapsedTimer>
#include <QSemaphore>
#include <QThreadPool>
#include <QScopeGuard>
#include <atomic>
#include <QListView>
#include <QBuffer>
#include <QStandardItemModel>
#include "gui/udrivethumbnails.h"
#include <QAbstractButton>
#include <QScreen>
#include <QStackedWidget>
#include <QClipboard>
#include <QQuickWidget>
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
#include "libsync/jobqueue.h"
#include "libsync/theme.h"
#include "resources/resources.h"
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QDesktopServices>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTreeView>
#include <QTimer>
#include <QAction>
#include <QComboBox>
#include <QSet>
#include <QtTest>
#include <algorithm>

using namespace OCC;

class FileOpenReceiver : public QObject
{
    Q_OBJECT
public:
    QUrl opened;
public Q_SLOTS:
    void record(const QUrl &url) { opened = url; }
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
    bool slowStarted = false;
    bool rejectFavorite = false;
    bool readOnly = false;
    bool holdUpload = false;
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
                    } else if (path == QStringLiteral("/graph/v1.0/me/drives")) {
                        QJsonArray drives;
                        QStringList ids {QStringLiteral("personal"), QStringLiteral("team")};
                        if (includeShares) ids.append(QStringLiteral("a0ca6a90-a365-4782-871e-d44447bbc668$a0ca6a90-a365-4782-871e-d44447bbc668"));
                        for (const auto &id : ids) {
                            drives.append(QJsonObject{{"id", id}, {"name", id == "team" ? "Team files" : "Personal"},
                                {"driveType", id == "personal" ? "personal" : "project"}, {"eTag", "drive-tag"},
                                {"quota", id == "personal" ? personalQuota : QJsonObject{}},
                                {"root", QJsonObject{{"id", id}, {"eTag", "root-tag"},
                                    {"webDavUrl", url("/dav/" + id + "/").toString()}, {"webUrl", url("/web/" + id).toString()}}}});
                        }
                        respond(socket, 200, "application/json", QJsonDocument(QJsonObject{{"value", drives}}).toJson());
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
                    } else if (parts.at(0) == "PROPPATCH") {
                        if (!rejectFavorite) {
                            if (requestBodies.value(key).contains(">true<")) favorites.insert(path);
                            else favorites.remove(path);
                        }
                        respond(socket, 207, "application/xml; charset=utf-8",
                            QByteArray("<d:multistatus xmlns:d=\"DAV:\"><d:response><d:propstat><d:prop/><d:status>HTTP/1.1 ")
                            + (rejectFavorite ? "403 Forbidden" : "200 OK") + "</d:status></d:propstat></d:response></d:multistatus>");
                    } else if (parts.at(0) == "REPORT") {
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
                        if (!exists(path)) respond(socket, 404, "text/plain", "Missing");
                        else { removed.insert(path); trash.insert(QStringLiteral("deleted-%1").arg(trash.size() + 1), path); respond(socket, 204, "text/plain", {}); }
                    } else if (parts.at(0) == "PROPFIND") {
                        QByteArray xml = "<?xml version=\"1.0\"?><d:multistatus xmlns:d=\"DAV:\">";
                        xml += entry(path, true);
                        if (path == "/dav/personal") {
                            for (const auto &name : {"Docs", "slowfolder", "a&b.txt", "%2F.txt", "denied.txt", "slow.txt", "truncated.txt"})
                                if (!removed.contains(path + '/' + name)) xml += entry(path + '/' + name, QByteArray(name) == "Docs" || QByteArray(name) == "slowfolder");
                            xml += entry(path + "/../escape", false);
                            xml += entry(path + "/Docs/not-a-direct-child", false);
                        } else if (path == "/dav/personal/Docs") {
                            xml += entry(path + "/Budget 2026.pdf", false);
                        } else if (path == "/dav/personal/slowfolder") {
                            xml += entry(path + "/stale.txt", false);
                        } else if (path == "/dav/team") {
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
                              "<d:getcontentlength>%9</d:getcontentlength><d:getlastmodified>%3</d:getlastmodified>"
                              "</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>")
            .arg(href, folder ? QStringLiteral("<d:collection/>") : QString(),
                modifiedDates.value(path, path.endsWith("denied.txt") ? QStringLiteral("invalid date") : QStringLiteral("Wed, 01 Oct 2025 10:00:00 GMT")),
                favorites.contains(path) ? QStringLiteral("1") : QStringLiteral("0"),
                readOnly ? QStringLiteral("S") : QStringLiteral("RDNVCKW"), url("/f" + path).toString().toHtmlEscaped(),
                original.section('/', -1).toHtmlEscaped(), original.mid(QStringLiteral("/dav/personal/").size()).toHtmlEscaped(),
                QString::number(sizes.value(path, 524288))).toUtf8();
    }
    void respond(QTcpSocket *socket, int status, const QByteArray &type, const QByteArray &body)
    {
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " Response\r\nContent-Type: " + type
            + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
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
                QCOMPARE(receiver.opened.toLocalFile(), QFileInfo(synced.filePath(file)).canonicalFilePath());
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
        if (qEnvironmentVariableIsSet("UDRIVE_UI_TEST_CAPTURE")) {
            QTest::qWait(100);
            QVERIFY(window.grab().save(QStringLiteral("/tmp/udrive-language-%1.png").arg(expectedLocale)));
        }
        // Selection is saved canonically and restored when settings reopen.
        languages->setCurrentIndex(1);
        auto *general = languages->parentWidget();
        while (general && general->metaObject()->indexOfMethod("saveMiscSettings()") < 0)
            general = general->parentWidget();
        QVERIFY(general);
        QVERIFY(QMetaObject::invokeMethod(general, "saveMiscSettings", Qt::DirectConnection));
        QCOMPARE(ConfigFile().uiLanguage(), QStringLiteral("km"));
        SettingsDialog reopened(nullptr);
        QCOMPARE(reopened.findChild<QComboBox *>("languageDropdown")->currentData().toString(), QStringLiteral("km"));
    }

    void rowActionsTargetTheClickedFile()
    {
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
        QCOMPARE(tree->model()->columnCount(), 5);
        select(tree, "denied.txt");
        QModelIndex target;
        for (int row = 0; row < tree->model()->rowCount(); ++row)
            if (tree->model()->index(row, 0).data().toString() == QStringLiteral("a&b.txt"))
                target = tree->model()->index(row, 4);
        QVERIFY(target.isValid());
        QRect row = tree->visualRect(target);
        qApp->clipboard()->setText("before");
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(row.right() - 61, row.center().y()));
        QCOMPARE(qApp->clipboard()->text(), server.url("/f/dav/personal/a&b.txt").toString());
        QCOMPARE(tree->currentIndex().siblingAtColumn(0).data().toString(), QStringLiteral("a&b.txt"));
        auto *actions = browser.findChild<QMenu *>("driveActionsMenu");
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(row.right() - 27, row.center().y()));
        QVERIFY(actions && actions->isVisible());
        actions->hide();
        QSignalSpy completed(&browser, &UDriveFileBrowser::operationFinished);
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(row.right() - 95, row.center().y()));
        QTRY_COMPARE(completed.count(), 1);
        QVERIFY(completed.first().first().toBool());
        QVERIFY(!server.favorites.contains("/dav/personal/a&b.txt"));
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
            window.setCurrentAccount(second);
            window.setCurrentPage(SettingsDialog::SettingsPage::Files);
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
                qApp->removeTranslator(&khmer);
                window.resize(960, 700);
                QTest::qWait(100);
                QVERIFY(window.grab().save("/tmp/udrive-workspace-compact.png"));
                qApp->setPalette(previous);
                QTest::qWait(150);
                QVERIFY(window.grab().save("/tmp/udrive-workspace-light.png"));
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
            manager->deleteAccount(secondState);
            QCOMPARE(headerSearch->count(), 1);
            QVERIFY(!window.canCreateFiles());
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
        const auto before = preview->findChild<QLabel *>("pdfPageImage")->pixmap().size();
        preview->findChild<QComboBox *>("pdfZoom")->setCurrentIndex(7); // 200%
        QTRY_VERIFY(preview->findChild<QLabel *>("pdfPageImage")->pixmap().height() > before.height());
        page->setValue(1);
        QTRY_COMPARE(rendered.last().at(0).toInt(), 1);
        if (qEnvironmentVariableIsSet("UDRIVE_PDF_TEST_CAPTURE")) browser.grab().save("/tmp/udrive-pdf-preview.png");
        preview->findChild<QPushButton *>("pdfBack")->click();
        QTRY_VERIFY(!QFileInfo::exists(cache));
        QVERIFY(tree->isVisible());
        QCOMPARE(receiver.opened, QUrl());

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
        QTRY_VERIFY(!rendered.isEmpty());
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
        QCOMPARE(receiver.opened.toLocalFile(), QFileInfo(syncedFile).canonicalFilePath());
        // A symlink to a file outside the sync root must not be opened as synced.
        QVERIFY(QFile::link(destination, synced.path() + "/denied.txt"));
        QVERIFY(select(tree, "denied.txt").isValid());
        QVERIFY(!browser.findChild<QPushButton *>("driveOpen")->isEnabled());
        QVERIFY(server.authenticated);
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
        QTRY_VERIFY(select(tree, "Final & draft.txt").isValid());
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
        QTRY_COMPARE(tree->model()->rowCount(), 1);
        QCOMPARE(tree->model()->index(0, 0).data().toString(), QStringLiteral("Shared document.pdf"));
        QVERIFY(server.authenticated);
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
};

QTEST_MAIN(TestUDriveFileBrowser)
#include "testudrivefilebrowser.moc"
