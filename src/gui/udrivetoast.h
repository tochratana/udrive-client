// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "udriveicons.h"
#include <QAccessible>
#include <QCoreApplication>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QTimer>
#include <QToolButton>

namespace OCC {
// Action results belong to the window, outside page layouts and scrolling panels.
class UDriveToast final : public QFrame
{
public:
    enum class Kind { Information, Success, Error };

    static void preferenceSaved(QWidget *source)
    {
        showMessage(source, QCoreApplication::translate("UDriveToast", "Preference saved."));
    }

    static void dismiss(QWidget *source)
    {
        if (!source) return;
        if (auto *notice = source->window()->findChild<QFrame *>(QStringLiteral("udriveToast"), Qt::FindDirectChildrenOnly)) {
            notice->hide();
            notice->setObjectName({});
            notice->deleteLater();
        }
    }

    static void showMessage(QWidget *source, const QString &message, Kind kind = Kind::Information)
    {
        if (!source || message.trimmed().isEmpty() || !source->isVisible()) return;
        auto *host = source->window();
        dismiss(source);
        auto *toast = new UDriveToast(source, host, message, kind);
        toast->show();
        toast->raise();
        QAccessibleEvent alert(toast, QAccessible::Alert);
        QAccessible::updateAccessibility(&alert);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == _close && event->type() == QEvent::FocusIn) {
            _timer.stop();
        } else if (watched == _close && event->type() == QEvent::FocusOut) {
            if (_kind != Kind::Error) _timer.start();
        } else if (event->type() == QEvent::Hide || event->type() == QEvent::Close) {
            hide();
            deleteLater();
        } else if (watched == parentWidget() && event->type() == QEvent::Resize) {
            position();
        } else if (watched == parentWidget() && event->type() == QEvent::PaletteChange) {
            applyTheme();
        }
        return QFrame::eventFilter(watched, event);
    }

    void enterEvent(QEnterEvent *event) override
    {
        _timer.stop();
        QFrame::enterEvent(event);
    }
    void leaveEvent(QEvent *event) override
    {
        if (_kind != Kind::Error && !_close->hasFocus()) _timer.start();
        QFrame::leaveEvent(event);
    }

private:
    UDriveToast(QWidget *source, QWidget *host, const QString &message, Kind kind)
        : QFrame(host), _kind(kind)
    {
        setObjectName(QStringLiteral("udriveToast"));
        setProperty("noticeKind", kind == Kind::Error ? QStringLiteral("error")
            : kind == Kind::Success ? QStringLiteral("success") : QStringLiteral("information"));
        setAccessibleName(message);
        setAccessibleDescription(QCoreApplication::translate("UDriveToast", "Notification"));
        setAttribute(Qt::WA_StyledBackground);
        setFocusPolicy(Qt::NoFocus);
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(16, 10, 12, 10);
        layout->setSpacing(10);
        _icon = new QLabel(this);
        _icon->setFixedSize(20, 20);
        _icon->setAlignment(Qt::AlignCenter);
        layout->addWidget(_icon, 0, Qt::AlignVCenter);
        _text = new QLabel(message, this);
        _text->setObjectName(QStringLiteral("udriveToastMessage"));
        _text->setTextFormat(Qt::PlainText);
        _text->setWordWrap(true);
        _text->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        _text->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        layout->addWidget(_text, 1, Qt::AlignVCenter);
        _close = new QToolButton(this);
        _close->setObjectName(QStringLiteral("udriveToastClose"));
        _close->setAccessibleName(QCoreApplication::translate("UDriveToast", "Dismiss notification"));
        _close->setToolTip(_close->accessibleName());
        _close->setCursor(Qt::PointingHandCursor);
        _close->setFixedSize(24, 24);
        _close->setFocusPolicy(Qt::StrongFocus);
        _close->installEventFilter(this);
        layout->addWidget(_close, 0, Qt::AlignVCenter);
        connect(_close, &QToolButton::clicked, this, [this] { hide(); deleteLater(); });
        auto *shadow = new QGraphicsDropShadowEffect(this);
        shadow->setBlurRadius(16);
        shadow->setOffset(0, 3);
        shadow->setColor(QColor(0, 0, 0, 35));
        setGraphicsEffect(shadow);
        _timer.setSingleShot(true);
        _timer.setInterval(6000);
        connect(&_timer, &QTimer::timeout, this, [this] { hide(); deleteLater(); });
        if (_kind != Kind::Error) _timer.start();
        host->installEventFilter(this);
        if (source != host) source->installEventFilter(this);
        // A sidebar may be destroyed when switching accounts without hiding first.
        if (source != host) connect(source, &QObject::destroyed, this, [this] { hide(); deleteLater(); });
        applyTheme();
        position();
    }

    void applyTheme()
    {
        const bool dark = QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
        const QString text = dark ? QStringLiteral("#EDF2F0") : QStringLiteral("#102A43");
        const QString muted = dark ? QStringLiteral("#B3C1BA") : QStringLiteral("#526780");
        setStyleSheet(QStringLiteral(
            "QFrame#udriveToast { background: %1; border: 1px solid %2; border-radius: 12px; }"
            "QLabel#udriveToastMessage { color: %3; background: transparent; border: none; font-size: 13px; font-weight: 500; }"
            "QToolButton#udriveToastClose { background: transparent; border: none; border-radius: 5px; padding: 0; }"
            "QToolButton#udriveToastClose:hover { background: %4; }"
            "QToolButton#udriveToastClose:focus { border: 1px solid %5; }")
            .arg(dark ? QStringLiteral("#303632") : QStringLiteral("#FFFFFF"),
                dark ? QStringLiteral("#4B554E") : QStringLiteral("#E5E9EC"), text,
                dark ? QStringLiteral("#424B45") : QStringLiteral("#F0F3F5"), muted));
        const QColor iconColor = _kind == Kind::Error ? QColor(dark ? "#FF9B93" : "#C03D35")
            : _kind == Kind::Success ? QColor(dark ? "#69D99A" : "#16804A") : QColor(muted);
        _icon->setPixmap(UDriveIcons::icon(_kind == Kind::Success ? UDriveIcons::Symbol::Check : UDriveIcons::Symbol::Info, false, iconColor).pixmap(20, 20));
        _close->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Close, false, QColor(muted)));
        _close->setIconSize(QSize(18, 18));
    }

    void position()
    {
        const QRect bounds = parentWidget()->contentsRect().adjusted(16, 16, -16, -16);
        setFixedWidth(qMax(1, qMin(400, bounds.width())));
        const int height = qMax(48, layout()->totalHeightForWidth(width()));
        resize(width(), qMin(height, qMax(1, bounds.height())));
        move(bounds.right() - width() + 1, bounds.bottom() - this->height() + 1);
    }

    Kind _kind;
    QTimer _timer;
    QLabel *_icon;
    QLabel *_text;
    QToolButton *_close;
};
}
