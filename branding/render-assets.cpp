// Build-time asset conversion: preserve the supplied logo's pixels and alpha.
// Run with the project's Qt environment; see README.md.
#include <QDir>
#include <QFont>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QString>

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    if (argc != 2)
        return 1;
    const QDir theme(QString::fromLocal8Bit(argv[1]));
    const QImage logo(theme.filePath(QStringLiteral("universal/udrive-icon.png")));
    if (logo.isNull())
        return 2;

    for (int size : {16, 24, 32, 48, 64, 128, 256, 512, 1024}) {
        const QImage icon = logo.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        if (!icon.save(theme.filePath(QStringLiteral("colored/%1-udrive-icon.png").arg(size)))
            || !icon.save(theme.filePath(QStringLiteral("colored/%1-udrive-sidebar.png").arg(size))))
            return 3;
    }

    QImage wordmark(1000, 320, QImage::Format_ARGB32_Premultiplied);
    wordmark.fill(Qt::transparent);
    QPainter painter(&wordmark);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.drawImage(QRect(0, 0, 320, 320), logo);
    QFont font(QStringLiteral("Helvetica"));
    font.setPixelSize(160);
    font.setBold(true);
    painter.setFont(font);
    painter.setPen(Qt::white);
    painter.drawText(QRect(335, 0, 665, 320), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("UDrive"));
    painter.end();
    for (const char *name : {"wizard_logo", "wizard_logo_dark", "wizard_footer_logo"}) {
        if (!wordmark.save(theme.filePath(QStringLiteral("universal/%1.png").arg(QString::fromLatin1(name)))))
            return 4;
    }
    return 0;
}
