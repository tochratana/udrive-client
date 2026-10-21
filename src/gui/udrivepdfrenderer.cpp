// SPDX-License-Identifier: GPL-2.0-or-later
// Private, credential-free PDF worker. The GUI supervises and can kill this
// process; all CoreGraphics parsing, metadata and rendering stay here.
#include "udrivepdfdocument.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cmath>
#include <cstdio>
#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 2) return 2;
#ifdef Q_OS_WIN
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    QFile input, output;
    if (!input.open(stdin, QIODevice::ReadOnly) || !output.open(stdout, QIODevice::WriteOnly)) return 2;
    QString openError;
    // Keep one immutable snapshot for the entire preview, including page/zoom changes.
    const auto document = OCC::UDrivePdfDocument::open(app.arguments().at(1), &openError);
    for (;;) {
        const auto line = input.readLine(512);
        if (line.isEmpty()) return 0;
        if (!line.endsWith('\n')) return 2;
        const auto request = QJsonDocument::fromJson(line).object();
        const int page = request.value(QStringLiteral("page")).toInt();
        const int zoom = request.value(QStringLiteral("zoom")).toInt();
        const int width = request.value(QStringLiteral("width")).toInt();
        const int height = request.value(QStringLiteral("height")).toInt();
        const double dpr = request.value(QStringLiteral("dpr")).toDouble();
        if (page < 1 || width < 1 || height < 1 || width > 32768 || height > 32768
            || !std::isfinite(dpr) || dpr <= 0 || dpr > 16
            || (zoom != -1 && zoom != -2 && zoom != 50 && zoom != 75 && zoom != 100 && zoom != 125 && zoom != 150 && zoom != 200 && zoom != 300)) return 2;
        QString error = openError;
        QImage image;
        QSize display;
        if (document) {
            const auto size = document->pageSize(page);
            if (size.isEmpty()) error = QStringLiteral("This PDF page cannot be rendered.");
            else {
                qreal scale = zoom > 0 ? zoom / 100.0 : width / size.width();
                if (zoom == -2) scale = std::min(scale, height / size.height());
                const auto scaled = size * scale;
                if (scaled.width() > 32768 || scaled.height() > 32768)
                    error = QStringLiteral("This page is too large at this zoom. Choose Fit page or a smaller zoom.");
                else {
                    display = QSize(std::max(1, int(std::ceil(scaled.width()))), std::max(1, int(std::ceil(scaled.height()))));
                    scale = std::min(scale * dpr, 4096.0 / std::max(size.width(), size.height()));
                    const QSize pixels(std::clamp(int(std::ceil(size.width() * scale)), 1, 4096), std::clamp(int(std::ceil(size.height() * scale)), 1, 4096));
                    image = document->render(page, pixels);
                    if (image.isNull()) error = QStringLiteral("This PDF page cannot be rendered.");
                }
            }
        }
        const QJsonObject header{{QStringLiteral("pages"), document ? document->pageCount() : 0},
            {QStringLiteral("width"), image.width()}, {QStringLiteral("height"), image.height()},
            {QStringLiteral("displayWidth"), display.width()}, {QStringLiteral("displayHeight"), display.height()},
            {QStringLiteral("bytes"), double(image.sizeInBytes())}, {QStringLiteral("error"), error.left(1024)},
            {QStringLiteral("documentError"), !document}};
        const QByteArray metadata = QJsonDocument(header).toJson(QJsonDocument::Compact) + '\n';
        if (metadata.size() > 4096 || output.write(metadata) != metadata.size()) return 2;
        // Raw bounded RGBA avoids another image/document decoder in the GUI process.
        if (!image.isNull() && output.write(reinterpret_cast<const char *>(image.constBits()), image.sizeInBytes()) != image.sizeInBytes()) return 2;
        if (!output.flush()) return 2;
    }
}
