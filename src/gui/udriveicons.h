// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QGuiApplication>
#include <QIcon>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QtMath>
#include <initializer_list>

namespace OCC::UDriveIcons {
enum class Symbol {
    Add,
    Online,
    Refresh,
    Up,
    Folder,
    Open,
    Download,
    More,
    Upload,
    Rename,
    Copy,
    Move,
    Trash,
    Restore,
    Star,
    Link,
    Close,
    Search,
    Back,
    Next,
    File,
    Image,
    Pdf,
    Video,
    Sheet,
    Slides,
    Grid,
    List,
    Settings,
    Activity,
    MoreVertical
};

// Draw the same rounded, 24-unit stroke icons at any device pixel ratio.
// Resolve colors when painting so existing controls follow palette changes.
class Engine final : public QIconEngine
{
public:
    Engine(Symbol symbol, bool primary, QColor tint = {})
        : _symbol(symbol)
        , _primary(primary)
        , _tint(tint)
    {
    }
    QIconEngine *clone() const override { return new Engine(_symbol, _primary, _tint); }
    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State) override
    {
        const bool dark = QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
        const QColor color = mode == QIcon::Disabled ? QColor(dark ? "#939D97" : "#7D8781")
            : _tint.isValid()                        ? _tint
            : _primary                               ? QColor("#12351D")
                                                     : QColor(dark ? "#EDEEF0" : "#2B3035");
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const qreal side = qMin(rect.width(), rect.height());
        painter->translate(rect.x() + (rect.width() - side) / 2, rect.y() + (rect.height() - side) / 2);
        painter->scale(side / 24, side / 24);
        painter->setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->setBrush(Qt::NoBrush);
        auto line = [painter](std::initializer_list<QPointF> points) {
            QPainterPath path;
            auto point = points.begin();
            path.moveTo(*point++);
            for (; point != points.end(); ++point)
                path.lineTo(*point);
            painter->drawPath(path);
        };
        switch (_symbol) {
        case Symbol::Add:
            line({{12, 5}, {12, 19}});
            line({{5, 12}, {19, 12}});
            break;
        case Symbol::Online:
            painter->drawEllipse(QRectF(3, 3, 18, 18));
            painter->drawEllipse(QRectF(8, 3, 8, 18));
            line({{3, 12}, {21, 12}});
            break;
        case Symbol::Refresh:
            painter->drawArc(QRectF(4, 4, 16, 16), 35 * 16, 285 * 16);
            line({{16, 3}, {21, 3}, {21, 8}});
            break;
        case Symbol::Up:
            line({{6, 10}, {12, 4}, {18, 10}});
            line({{12, 4}, {12, 20}});
            break;
        case Symbol::Folder:
            line({{3, 8}, {3, 5}, {9, 5}, {12, 8}, {21, 8}, {21, 19}, {3, 19}, {3, 8}});
            break;
        case Symbol::Open:
            line({{13, 4}, {20, 4}, {20, 11}});
            line({{20, 4}, {10, 14}});
            line({{9, 5}, {4, 5}, {4, 20}, {19, 20}, {19, 15}});
            break;
        case Symbol::Download:
            line({{12, 3}, {12, 15}});
            line({{7, 10}, {12, 15}, {17, 10}});
            line({{4, 16}, {4, 21}, {20, 21}, {20, 16}});
            break;
        case Symbol::Upload:
            line({{12, 16}, {12, 4}});
            line({{7, 9}, {12, 4}, {17, 9}});
            line({{4, 16}, {4, 21}, {20, 21}, {20, 16}});
            break;
        case Symbol::MoreVertical:
            painter->setBrush(color);
            painter->setPen(Qt::NoPen);
            for (int y : {5, 12, 19})
                painter->drawEllipse(QPointF(12, y), 1.4, 1.4);
            break;
        case Symbol::More:
            painter->setBrush(color);
            painter->setPen(Qt::NoPen);
            for (int x : {5, 12, 19})
                painter->drawEllipse(QPointF(x, 12), 1.6, 1.6);
            break;
        case Symbol::Rename:
            line({{4, 20}, {5, 14}, {16, 3}, {21, 8}, {10, 19}, {4, 20}});
            line({{14, 5}, {19, 10}});
            break;
        case Symbol::Copy:
            painter->drawRoundedRect(QRectF(8, 8, 13, 13), 2, 2);
            line({{15, 4}, {15, 3}, {3, 3}, {3, 15}, {4, 15}});
            break;
        case Symbol::Move:
            line({{3, 8}, {3, 5}, {9, 5}, {12, 8}, {21, 8}, {21, 19}, {3, 19}, {3, 8}});
            line({{7, 14}, {17, 14}});
            line({{14, 11}, {17, 14}, {14, 17}});
            break;
        case Symbol::Trash:
            line({{3, 6}, {21, 6}});
            line({{9, 6}, {9, 3}, {15, 3}, {15, 6}});
            line({{5, 6}, {6, 21}, {18, 21}, {19, 6}});
            line({{10, 10}, {10, 17}});
            line({{14, 10}, {14, 17}});
            break;
        case Symbol::Restore:
            painter->drawArc(QRectF(4, 4, 16, 16), -45 * 16, 285 * 16);
            line({{3, 3}, {3, 9}, {9, 9}});
            line({{12, 8}, {12, 12}, {15, 14}});
            break;
        case Symbol::Star:
            line({{12, 3}, {14.8, 8.7}, {21, 9.6}, {16.5, 14}, {17.6, 20.2}, {12, 17.3}, {6.4, 20.2}, {7.5, 14}, {3, 9.6}, {9.2, 8.7}, {12, 3}});
            break;
        case Symbol::Link: {
            QPainterPath path;
            path.moveTo(9, 15);
            path.cubicTo(6, 18, 4, 18, 3, 16);
            path.cubicTo(1, 13, 4, 10, 7, 8);
            path.moveTo(15, 9);
            path.cubicTo(18, 6, 20, 6, 21, 8);
            path.cubicTo(23, 11, 20, 14, 17, 16);
            painter->drawPath(path);
            line({{8, 16}, {16, 8}});
            break;
        }
        case Symbol::Close:
            line({{6, 6}, {18, 18}});
            line({{18, 6}, {6, 18}});
            break;
        case Symbol::Search:
            painter->drawEllipse(QRectF(3, 3, 13, 13));
            line({{15, 15}, {21, 21}});
            break;
        case Symbol::Back:
            line({{14, 5}, {7, 12}, {14, 19}});
            break;
        case Symbol::Next:
            line({{10, 5}, {17, 12}, {10, 19}});
            break;
        case Symbol::Grid:
            for (int x : {4, 14})
                for (int y : {4, 14})
                    painter->drawRoundedRect(QRectF(x, y, 6, 6), 1, 1);
            break;
        case Symbol::List:
            for (int y : {5, 12, 19}) {
                line({{8, qreal(y)}, {21, qreal(y)}});
                painter->drawRoundedRect(QRectF(3, y - 1, 2, 2), .5, .5);
            }
            break;
        case Symbol::Settings: {
            QPainterPath gear;
            for (int i = 0; i < 32; ++i) {
                const qreal angle = qDegreesToRadians(i * 11.25);
                const qreal radius = i % 4 == 1 || i % 4 == 2 ? 10.0 : 8.0;
                const QPointF point(12 + qCos(angle) * radius, 12 + qSin(angle) * radius);
                if (i == 0)
                    gear.moveTo(point);
                else
                    gear.lineTo(point);
            }
            gear.closeSubpath();
            painter->drawPath(gear);
            painter->drawEllipse(QRectF(8.5, 8.5, 7, 7));
            break;
        }
        case Symbol::Activity:
            painter->drawEllipse(QRectF(3, 3, 18, 18));
            line({{12, 6}, {12, 12}, {16, 14}});
            break;
        case Symbol::File:
        case Symbol::Image:
        case Symbol::Pdf:
        case Symbol::Video:
        case Symbol::Sheet:
        case Symbol::Slides:
            line({{5, 3}, {14, 3}, {20, 9}, {20, 21}, {5, 21}, {5, 3}});
            line({{14, 3}, {14, 9}, {20, 9}});
            if (_symbol == Symbol::Image) {
                painter->drawEllipse(QPointF(9, 13), 1.2, 1.2);
                line({{7, 18}, {10, 15}, {13, 17}, {16, 14}, {18, 18}});
            }
            if (_symbol == Symbol::Pdf) {
                line({{8, 13}, {17, 13}});
                line({{8, 17}, {14, 17}});
            }
            if (_symbol == Symbol::Video)
                line({{9, 11}, {9, 18}, {15, 14.5}, {9, 11}});
            if (_symbol == Symbol::Sheet) {
                painter->drawRect(QRectF(8, 12, 9, 6));
                line({{8, 15}, {17, 15}});
                line({{12, 12}, {12, 18}});
            }
            if (_symbol == Symbol::Slides) {
                painter->drawRect(QRectF(8, 12, 9, 5));
                line({{12, 17}, {12, 19}});
            }
            break;
        }
        painter->restore();
    }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override { return scaledPixmap(size, mode, state, 1); }
    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override
    {
        QPixmap pixmap(size * scale);
        pixmap.setDevicePixelRatio(scale);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        paint(&painter, QRect(QPoint(), size), mode, state);
        return pixmap;
    }

private:
    Symbol _symbol;
    bool _primary;
    QColor _tint;
};

inline QIcon icon(Symbol symbol, bool primary = false, QColor tint = {})
{
    return QIcon(new Engine(symbol, primary, tint));
}
inline QIcon fileIcon(const QString &name, bool directory)
{
    if (directory)
        return QIcon(new Engine(Symbol::Folder, false, QColor("#40D979")));
    const QString extension = name.section(QLatin1Char('.'), -1).toLower();
    if (extension == QStringLiteral("pdf"))
        return QIcon(new Engine(Symbol::Pdf, false, QColor("#FF4B55")));
    if (extension == QStringLiteral("ppt") || extension == QStringLiteral("pptx") || extension == QStringLiteral("pptm")
        || extension == QStringLiteral("pps") || extension == QStringLiteral("ppsx") || extension == QStringLiteral("odp")) {
        static const QIcon presentation(QStringLiteral(":/client/resources/filetypes/presentation.png"));
        return presentation;
    }
    if (extension == QStringLiteral("xls") || extension == QStringLiteral("xlsx") || extension == QStringLiteral("xlsm")
        || extension == QStringLiteral("xlsb") || extension == QStringLiteral("csv") || extension == QStringLiteral("tsv") || extension == QStringLiteral("ods")) {
        static const QIcon spreadsheet(QStringLiteral(":/client/resources/filetypes/spreadsheet.png"));
        return spreadsheet;
    }
    if (extension == QStringLiteral("svg") || extension == QStringLiteral("odg") || extension == QStringLiteral("otg")
        || extension == QStringLiteral("drawio") || extension == QStringLiteral("vsd") || extension == QStringLiteral("vsdx")
        || extension == QStringLiteral("dwg") || extension == QStringLiteral("dxf")) {
        static const QIcon drawing(QStringLiteral(":/client/resources/filetypes/drawing.png"));
        return drawing;
    }
    if (extension == QStringLiteral("mp4") || extension == QStringLiteral("mov") || extension == QStringLiteral("webm")
        || extension == QStringLiteral("avi") || extension == QStringLiteral("mkv"))
        return QIcon(new Engine(Symbol::Video, false, QColor("#00BCD4")));
    if (extension == QStringLiteral("jpg") || extension == QStringLiteral("jpeg") || extension == QStringLiteral("png")
        || extension == QStringLiteral("gif") || extension == QStringLiteral("webp")
        || extension == QStringLiteral("bmp") || extension == QStringLiteral("ico") || extension == QStringLiteral("heic"))
        return QIcon(new Engine(Symbol::Image, false, QColor("#8B5CF6")));
    if (extension == QStringLiteral("doc") || extension == QStringLiteral("docx") || extension == QStringLiteral("docm")
        || extension == QStringLiteral("dot") || extension == QStringLiteral("dotx") || extension == QStringLiteral("odt") || extension == QStringLiteral("rtf")) {
        static const QIcon document(QStringLiteral(":/client/resources/filetypes/document.png"));
        return document;
    }
    if (extension == QStringLiteral("txt") || extension == QStringLiteral("md") || extension == QStringLiteral("json")
        || extension == QStringLiteral("xml") || extension == QStringLiteral("html") || extension == QStringLiteral("js")
        || extension == QStringLiteral("py") || extension == QStringLiteral("cpp") || extension == QStringLiteral("h"))
        return QIcon(new Engine(Symbol::File, false, QColor("#10B981")));
    if (extension == QStringLiteral("zip") || extension == QStringLiteral("tar") || extension == QStringLiteral("gz")
        || extension == QStringLiteral("7z") || extension == QStringLiteral("rar"))
        return QIcon(new Engine(Symbol::File, false, QColor("#F59E0B")));
    if (extension == QStringLiteral("mp3") || extension == QStringLiteral("wav") || extension == QStringLiteral("flac")
        || extension == QStringLiteral("ogg") || extension == QStringLiteral("m4a"))
        return QIcon(new Engine(Symbol::File, false, QColor("#EC4899")));
    return icon(Symbol::File);
}
}
