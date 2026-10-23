/*
 * Copyright (C) by Daniel Molkentin <danimo@owncloud.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include "settingsdialog.h"
#include "ui_settingsdialog.h"

#include "accountmanager.h"
#include "accountview.h"
#include "udrivefilebrowser.h"
#include "udriveadminwidget.h"
#include "udriveicons.h"
#include "udrivepopup.h"
#include "common/utility.h"
#include "resources/resources.h"
#include "libsync/graphapi/spacesmanager.h"
#include <QLineEdit>
#include <QPushButton>
#include <QMenu>
#include <QLocale>
#include "activitysettings.h"
#include "application.h"
#include "configfile.h"
#include "generalsettings.h"
#include "gui/qmlutils.h"
#include "owncloudgui.h"
#include "resources/qmlresources.h"
#include "theme.h"

#include <QMessageBox>
#include <QEvent>
#include <QFocusEvent>
#include <QLabel>
#include <QVBoxLayout>
#include <QScreen>
#include <QWindow>
#include <QQuickItem>
#include <QAbstractButton>
#include <QFrame>
#include <QScrollArea>
#include <QPainter>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTextOption>
#include <algorithm>
#include <functional>

#ifdef Q_OS_MAC
#include "settingsdialog_mac.h"

void setActivationPolicy(ActivationPolicy policy);
#endif

Q_LOGGING_CATEGORY(lcSettingsDialog, "gui.settingsdialog", QtInfoMsg);

namespace {
// Plain-text painting keeps server-provided names readable without treating them as markup.
class AccountPopupButton : public QAbstractButton
{
public:
    AccountPopupButton(const QString &name, const QString &detail, const QIcon &icon, QWidget *parent)
        : QAbstractButton(parent)
        , _detail(detail)
        , _icon(icon)
    {
        setText(name);
        setAccessibleName(detail.isEmpty() ? name : name + QStringLiteral(", ") + detail);
        setToolTip(detail.isEmpty() ? name : name + QLatin1Char('\n') + detail);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFixedHeight(detail.isEmpty() ? 36 : 52);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const bool dark = palette().color(QPalette::Window).lightness() < 128;
        const QColor textColor(dark ? "#EDEEF0" : "#24332C");
        const QRectF background = QRectF(rect()).adjusted(1, 1, -1, -1);
        painter.setPen(hasFocus() && _keyboardFocus ? QPen(QColor("#40D979"), 1.5) : QPen(Qt::NoPen));
        painter.setBrush(isDown() ? QColor(dark ? "#404244" : "#E4E6E5")
                : underMouse() ? QColor(dark ? "#383A3C" : "#F0F1F0") : Qt::transparent);
        painter.drawRoundedRect(background, 6, 6);
        if (!_icon.isNull()) {
            const QRect iconBox(8, (height() - 20) / 2, 20, 20);
            _icon.paint(&painter, iconBox);
        }
        const int rightInset = isCheckable() ? 30 : 8;
        const int textLeft = _icon.isNull() ? 8 : 36;
        const QRect textRect(textLeft, 4, qMax(0, width() - textLeft - rightInset), height() - 8);
        auto nameFont = font();
        nameFont.setPixelSize(_detail.isEmpty() ? 13 : 14);
        nameFont.setWeight(QFont::DemiBold);
        painter.setFont(nameFont);
        painter.setPen(textColor);
        if (_detail.isEmpty()) {
            QTextOption options;
            options.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
            options.setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
            painter.drawText(textRect, text(), options);
        } else {
            auto detailFont = font();
            detailFont.setPixelSize(12);
            const int nameHeight = QFontMetrics(nameFont).height();
            const int detailHeight = QFontMetrics(detailFont).height();
            const int top = (height() - nameHeight - detailHeight - 2) / 2;
            painter.drawText(QRect(textLeft, top, textRect.width(), nameHeight), Qt::AlignVCenter | Qt::AlignLeft,
                painter.fontMetrics().elidedText(text(), Qt::ElideRight, textRect.width()));
            painter.setFont(detailFont);
            painter.setPen(QColor(dark ? "#AFB8B2" : "#64776B"));
            painter.drawText(
                QRect(textLeft, top + nameHeight + 2, textRect.width(), detailHeight), Qt::AlignVCenter | Qt::AlignLeft,
                painter.fontMetrics().elidedText(_detail, Qt::ElideMiddle, textRect.width()));
        }
        if (isChecked()) {
            const QPoint center(width() - 18, height() / 2);
            painter.setPen(QPen(QColor(dark ? "#63E493" : "#008051"), 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.drawLine(center + QPoint(-4, 0), center + QPoint(-1, 3));
            painter.drawLine(center + QPoint(-1, 3), center + QPoint(4, -3));
        }
    }
    void enterEvent(QEnterEvent *event) override
    {
        QAbstractButton::enterEvent(event);
        update();
    }
    void leaveEvent(QEvent *event) override
    {
        QAbstractButton::leaveEvent(event);
        update();
    }
    void focusInEvent(QFocusEvent *event) override
    {
        _keyboardFocus = event->reason() == Qt::TabFocusReason || event->reason() == Qt::BacktabFocusReason || event->reason() == Qt::ShortcutFocusReason;
        QAbstractButton::focusInEvent(event);
        update();
    }

private:
    QString _detail;
    QIcon _icon;
    bool _keyboardFocus = false;
};

class AccountPopup : public QFrame
{
public:
    explicit AccountPopup(QWidget *parent)
        : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint)
    {
        setObjectName(QStringLiteral("driveAccountPopup"));
        setFrameShape(QFrame::NoFrame);
        setAttribute(Qt::WA_DeleteOnClose);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoMouseReplay);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::KeyPress) {
            auto *key = static_cast<QKeyEvent *>(event);
            if (key->key() == Qt::Key_Escape) {
                close();
                return true;
            }
            if (key->key() == Qt::Key_Up || key->key() == Qt::Key_Down) {
                moveButtonFocus(qobject_cast<QAbstractButton *>(watched), key->key() == Qt::Key_Down);
                return true;
            }
        }
        return QFrame::eventFilter(watched, event);
    }
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const bool dark = palette().color(QPalette::Window).lightness() < 128;
        painter.setPen(QPen(QColor(dark ? "#464B48" : "#DFE6E1"), 1));
        painter.setBrush(QColor(dark ? "#2B2D2F" : "#FFFFFF"));
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 14, 14);
    }
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape) {
            close();
            event->accept();
        } else if (event->key() == Qt::Key_Down || event->key() == Qt::Key_Up) {
            moveButtonFocus(qobject_cast<QAbstractButton *>(focusWidget()), event->key() == Qt::Key_Down);
            event->accept();
        } else
            QFrame::keyPressEvent(event);
    }

private:
    void moveButtonFocus(QAbstractButton *current, bool forward)
    {
        auto buttons = findChildren<QAbstractButton *>();
        buttons.removeIf([](const QAbstractButton *button) { return !button->isEnabled() || !button->isVisible(); });
        if (buttons.isEmpty())
            return;
        const int index = buttons.indexOf(current);
        const int target = index < 0 ? 0 : (index + (forward ? 1 : buttons.size() - 1)) % buttons.size();
        buttons.at(target)->setFocus(forward ? Qt::TabFocusReason : Qt::BacktabFocusReason);
        if (auto *scroll = findChild<QScrollArea *>(); scroll && scroll->widget()->isAncestorOf(buttons.at(target)))
            scroll->ensureWidgetVisible(buttons.at(target));
    }
};

QWidget *workspacePage(QWidget *content, const QString &title, const QString &description)
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(12);
    auto *heading = new QLabel(title, page);
    QFont font = heading->font();
    font.setPointSize(20);
    font.setBold(true);
    heading->setFont(font);
    heading->setWordWrap(true);
    layout->addWidget(heading);
    auto *subtitle = new QLabel(description, page);
    subtitle->setObjectName(QStringLiteral("workspaceDescription"));
    subtitle->setWordWrap(true);
    layout->addWidget(subtitle);
    if (content->layout())
        content->layout()->setContentsMargins(0, 8, 0, 0);
    layout->addWidget(content, 1);
    return page;
}

auto minimumSizeHint(const QWidget *w, bool sidebar)
{
    const QSize min = sidebar ? QSize(960, 700) : QSize(800, 700);
    const auto screen = w->windowHandle() ? w->windowHandle()->screen() : QApplication::screenAt(QCursor::pos());
    if (screen) {
        const auto availableSize = screen->availableSize();
        if (availableSize.isValid()) {
            // Assume we can use at least 90% of the screen, if the screen is smaller than 800x700 pixels.
            //
            // Note: this means that the wizards have even less space: with the style we use, the
            // wizard tries to fit inside the window. So, if this is a common case that users have
            // such small screens, and the contents of the wizard screen are squashed together (or
            // not shown due to lack of space), we should consider putting that content in a
            // scroll-view.
            return min.boundedTo(availableSize * 0.9);
        }
    }
    return min;
}


class AvatarImageProvider : public QQuickImageProvider
{
    Q_OBJECT
public:
    AvatarImageProvider()
        : QQuickImageProvider(QQuickImageProvider::Pixmap, QQuickImageProvider::ForceAsynchronousImageLoading)
    {
    }

    QPixmap requestPixmap(const QString &id, QSize *size, const QSize &requestedSize)
    {
        const auto qmlIcon = OCC::Resources::QMLResources::parseIcon(id);
        const auto accountState = OCC::AccountManager::instance()->accountState(QUuid::fromString(qmlIcon.iconName));
        if (!accountState || !accountState->account())
            return {};
        return OCC::Resources::pixmap(requestedSize, accountState->account()->avatar(), qmlIcon.enabled ? QIcon::Normal : QIcon::Disabled, size);
    }
};
}


namespace OCC {

class NotificationBadgeButton : public QPushButton
{
public:
    explicit NotificationBadgeButton(QWidget *parent = nullptr)
        : QPushButton(parent)
    {
        setObjectName(QStringLiteral("driveHeaderTool"));
        setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Bell));
        setIconSize(QSize(20, 20));
        setFixedSize(36, 36);
        setCursor(Qt::PointingHandCursor);
        setAccessibleName(tr("Notifications"));
        setToolTip(tr("Notifications"));
    }

    void setUnreadCount(int count)
    {
        if (_unreadCount == count)
            return;
        _unreadCount = count;
        setToolTip(_unreadCount > 0
            ? tr("Notifications (%1 unread)").arg(_unreadCount)
            : tr("Notifications"));
        update();
    }

    int unreadCount() const { return _unreadCount; }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QPushButton::paintEvent(event);
        if (_unreadCount <= 0)
            return;

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const QString badgeText = _unreadCount > 99 ? QStringLiteral("99+") : QString::number(_unreadCount);
        QFont badgeFont = font();
        badgeFont.setPixelSize(10);
        badgeFont.setBold(true);
        painter.setFont(badgeFont);

        const QFontMetrics fm(badgeFont);
        const int textWidth = fm.horizontalAdvance(badgeText);
        const int badgeHeight = 16;
        const int badgeWidth = qMax(badgeHeight, textWidth + 8);
        const QRect badgeRect(width() - badgeWidth - 1, 1, badgeWidth, badgeHeight);

        const bool dark = palette().color(QPalette::Window).lightness() < 128;
        painter.setPen(QPen(QColor(dark ? "#2B2D2F" : "#FFFFFF"), 1.5));
        painter.setBrush(QColor(QStringLiteral("#FF4B55")));
        painter.drawRoundedRect(QRectF(badgeRect).adjusted(-0.5, -0.5, 0.5, 0.5), badgeHeight / 2.0 + 0.5, badgeHeight / 2.0 + 0.5);

        painter.setPen(Qt::white);
        painter.drawText(badgeRect, Qt::AlignCenter, badgeText);
    }

private:
    int _unreadCount = 0;
};

class NotificationAvatar : public QWidget
{
public:
    explicit NotificationAvatar(const QString &initials, QWidget *parent = nullptr)
        : QWidget(parent)
        , _initials(initials.isEmpty() ? QStringLiteral("?") : initials.toUpper())
    {
        setFixedSize(36, 36);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(QStringLiteral("#007C58")));
        painter.drawEllipse(rect());

        painter.setPen(Qt::white);
        QFont f = font();
        f.setPixelSize(13);
        f.setBold(true);
        painter.setFont(f);
        painter.drawText(rect(), Qt::AlignCenter, _initials);
    }

private:
    QString _initials;
};

class UnreadDotWidget : public QWidget
{
public:
    explicit UnreadDotWidget(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setFixedSize(8, 8);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(QStringLiteral("#00856B")));
        painter.drawEllipse(rect());
    }
};

static QString sanitizeNotificationRichText(const QString &raw, int maxLen = 128)
{
    QString clean = raw.trimmed().simplified();
    clean.remove(QRegularExpression(QStringLiteral("[\\p{Cc}\\x{202a}-\\x{202e}\\x{2066}-\\x{2069}]")));
    if (clean.size() > maxLen)
        clean = clean.left(maxLen - 1) + QStringLiteral("…");
    return clean.toHtmlEscaped();
}

class NotificationRowWidget : public QWidget
{
public:
    explicit NotificationRowWidget(const UDriveShareNotification &item,
                                  std::function<void(const QString &)> onClicked,
                                  QWidget *parent = nullptr)
        : QWidget(parent)
        , _notificationId(item.id)
        , _isRead(item.isRead)
        , _onClicked(std::move(onClicked))
    {
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_Hover);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

        auto *rowLayout = new QHBoxLayout(this);
        rowLayout->setContentsMargins(12, 10, 12, 10);
        rowLayout->setSpacing(12);

        auto *avatar = new NotificationAvatar(item.sharerInitials, this);
        rowLayout->addWidget(avatar, 0, Qt::AlignTop);

        auto *textLayout = new QVBoxLayout;
        textLayout->setContentsMargins(0, 0, 0, 0);
        textLayout->setSpacing(4);

        const bool dark = palette().color(QPalette::Window).lightness() < 128;
        const QString safeSharer = sanitizeNotificationRichText(item.sharerName, 64);
        const QString safeFile = sanitizeNotificationRichText(item.fileName, 128);
        const QString textColor = dark ? QStringLiteral("#EDEEF0") : QStringLiteral("#24332C");
        const QString mutedColor = dark ? QStringLiteral("#AFB8B2") : QStringLiteral("#64776B");

        auto *descLabel = new QLabel(this);
        descLabel->setTextFormat(Qt::RichText);
        descLabel->setWordWrap(true);
        descLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        descLabel->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 13px; }").arg(textColor));

        const QString richText = safeSharer.isEmpty()
            ? tr("Someone shared <b>%1</b> with you").arg(safeFile)
            : tr("<b>%1</b> shared <b>%2</b> with you").arg(safeSharer, safeFile);
        descLabel->setText(richText);
        textLayout->addWidget(descLabel);

        const QString timeAgo = Utility::timeAgoInWords(item.timestamp);
        auto *timeLabel = new QLabel(timeAgo, this);
        timeLabel->setTextFormat(Qt::PlainText);
        timeLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        timeLabel->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 12px; }").arg(mutedColor));
        textLayout->addWidget(timeLabel);

        rowLayout->addLayout(textLayout, 1);

        _unreadDot = new UnreadDotWidget(this);
        _unreadDot->setVisible(!_isRead);
        rowLayout->addWidget(_unreadDot, 0, Qt::AlignCenter);
    }

    void markRead()
    {
        if (!_isRead) {
            _isRead = true;
            if (_unreadDot) {
                _unreadDot->hide();
            }
            update();
        }
    }

    void activate()
    {
        markRead();
        if (_onClicked) {
            _onClicked(_notificationId);
        }
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            activate();
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter || event->key() == Qt::Key_Space) {
            activate();
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
    }

    void focusInEvent(QFocusEvent *event) override
    {
        QWidget::focusInEvent(event);
        update();
    }

    void focusOutEvent(QFocusEvent *event) override
    {
        QWidget::focusOutEvent(event);
        update();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        if (underMouse() || hasFocus()) {
            const bool dark = palette().color(QPalette::Window).lightness() < 128;
            painter.setBrush(QColor(dark ? "#383A3C" : "#F0F4F2"));
            painter.setPen(hasFocus() ? QPen(QColor(QStringLiteral("#00856B")), 1.5) : Qt::NoPen);
            painter.drawRoundedRect(QRectF(rect()).adjusted(2, 1, -2, -1), 8, 8);
        }
    }

    void enterEvent(QEnterEvent *event) override
    {
        QWidget::enterEvent(event);
        update();
    }

    void leaveEvent(QEvent *event) override
    {
        QWidget::leaveEvent(event);
        update();
    }

private:
    QString _notificationId;
    bool _isRead = false;
    std::function<void(const QString &)> _onClicked;
    UnreadDotWidget *_unreadDot = nullptr;
};

class UDriveNotificationPopup : public QFrame
{
public:
    explicit UDriveNotificationPopup(UDriveIncomingSharesNotifier *notifier,
                                    std::function<void(const QString &)> onItemClicked,
                                    QWidget *parent = nullptr)
        : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint)
        , _notifier(notifier)
        , _onItemClicked(std::move(onItemClicked))
    {
        setObjectName(QStringLiteral("driveNotificationPopup"));
        setFrameShape(QFrame::NoFrame);
        setAttribute(Qt::WA_DeleteOnClose);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoMouseReplay);

        setupUI();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const bool dark = palette().color(QPalette::Window).lightness() < 128;
        painter.setPen(QPen(QColor(dark ? "#464B48" : "#DFE6E1"), 1));
        painter.setBrush(QColor(dark ? "#2B2D2F" : "#FFFFFF"));
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 14, 14);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape) {
            close();
            event->accept();
            return;
        } else if (event->key() == Qt::Key_Down || event->key() == Qt::Key_Up) {
            moveRowFocus(event->key() == Qt::Key_Down);
            event->accept();
            return;
        }
        QFrame::keyPressEvent(event);
    }

private:
    void setupUI()
    {
        const bool dark = palette().color(QPalette::Window).lightness() < 128;
        const QString titleColor = dark ? QStringLiteral("#EDEEF0") : QStringLiteral("#24332C");
        const QString linkColor = dark ? QStringLiteral("#40D979") : QStringLiteral("#00856B");
        const QString linkHoverColor = dark ? QStringLiteral("#63E493") : QStringLiteral("#006E4E");
        const QString dividerColor = dark ? QStringLiteral("#404642") : QStringLiteral("#E3EAE6");

        auto *mainLayout = new QVBoxLayout(this);
        mainLayout->setContentsMargins(8, 12, 8, 8);
        mainLayout->setSpacing(4);

        // Header
        auto *headerWidget = new QWidget(this);
        auto *headerLayout = new QHBoxLayout(headerWidget);
        headerLayout->setContentsMargins(8, 0, 8, 4);
        headerLayout->setSpacing(8);

        auto *title = new QLabel(tr("Notifications"), headerWidget);
        title->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 16px; font-weight: 700; }").arg(titleColor));
        headerLayout->addWidget(title);
        headerLayout->addStretch(1);

        auto *markAll = new QPushButton(tr("Mark all as read"), headerWidget);
        markAll->setFlat(true);
        markAll->setCursor(Qt::PointingHandCursor);
        markAll->setStyleSheet(QStringLiteral(
            "QPushButton { background: transparent; border: none; color: %1; font-size: 13px; font-weight: 500; padding: 2px 4px; }"
            "QPushButton:hover { color: %2; }"
            "QPushButton:disabled { color: %3; }")
            .arg(linkColor, linkHoverColor, dark ? QStringLiteral("#6B7280") : QStringLiteral("#9CA3AF")));

        const int unreadCount = _notifier ? _notifier->unreadCount() : 0;
        markAll->setEnabled(unreadCount > 0);
        connect(markAll, &QPushButton::clicked, this, [this, markAll] {
            if (_notifier) {
                _notifier->markAllAsRead();
            }
            markAll->setEnabled(false);
            for (auto *row : _rows) {
                if (row) {
                    row->markRead();
                }
            }
        });
        headerLayout->addWidget(markAll);

        mainLayout->addWidget(headerWidget);

        // Divider
        auto *divider = new QFrame(this);
        divider->setFixedHeight(1);
        divider->setStyleSheet(QStringLiteral("background: %1; border: none;").arg(dividerColor));
        mainLayout->addWidget(divider);

        // Content (display up to 50 most recent notifications for optimal performance)
        const auto allNotifications = _notifier ? _notifier->notifications() : QList<UDriveShareNotification>();
        const auto notifications = allNotifications.mid(0, 50);

        auto *scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; border: none; }"));

        auto *content = new QWidget(scroll);
        content->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
        auto *contentLayout = new QVBoxLayout(content);
        contentLayout->setContentsMargins(0, 4, 0, 4);
        contentLayout->setSpacing(2);
        contentLayout->setAlignment(Qt::AlignTop);

        if (notifications.isEmpty()) {
            auto *emptyLabel = new QLabel(tr("No notifications"), content);
            emptyLabel->setAlignment(Qt::AlignCenter);
            emptyLabel->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 13px; padding: 36px 16px; }")
                .arg(dark ? QStringLiteral("#AFB8B2") : QStringLiteral("#64776B")));
            contentLayout->addWidget(emptyLabel);
            scroll->setFixedHeight(120);
        } else {
            for (const auto &item : notifications) {
                auto *row = new NotificationRowWidget(item, [this](const QString &id) {
                    close();
                    if (_onItemClicked) {
                        _onItemClicked(id);
                    }
                }, content);
                _rows.append(row);
                contentLayout->addWidget(row);
            }
            const int totalHeight = qMin(380, qMax(80, int(notifications.size()) * 78 + 8));
            scroll->setFixedHeight(totalHeight);
        }

        scroll->setWidget(content);
        mainLayout->addWidget(scroll);
    }

    void moveRowFocus(bool forward)
    {
        if (_rows.isEmpty())
            return;
        QWidget *current = focusWidget();
        int index = -1;
        for (int i = 0; i < _rows.size(); ++i) {
            if (_rows.at(i) == current) {
                index = i;
                break;
            }
        }
        const int target = index < 0 ? 0 : (index + (forward ? 1 : _rows.size() - 1)) % _rows.size();
        _rows.at(target)->setFocus(forward ? Qt::TabFocusReason : Qt::BacktabFocusReason);
        if (auto *scroll = findChild<QScrollArea *>())
            scroll->ensureWidgetVisible(_rows.at(target));
    }

    QPointer<UDriveIncomingSharesNotifier> _notifier;
    std::function<void(const QString &)> _onItemClicked;
    QList<NotificationRowWidget *> _rows;
};

SettingsDialog::SettingsDialog(ownCloudGui *gui, QWidget *parent)
    : QMainWindow(parent)
    , _ui(new Ui::SettingsDialog)
    , _gui(gui)
{
    setObjectName(QStringLiteral("Settings")); // required as group for saveGeometry call
    setWindowTitle(Theme::instance()->appNameGUI());
    _ui->setupUi(this);

    const bool udrive = Theme::instance()->appNameGUI() == QStringLiteral("UDrive");
    setMinimumSize(::minimumSizeHint(this, udrive));
    if (udrive) {
        _ui->verticalLayout_2->setDirection(QBoxLayout::LeftToRight);
        _ui->verticalLayout_2->setSpacing(12);
        _ui->verticalLayout_2->setContentsMargins(10, 0, 10, 10);
        createWorkspaceHeader();
        _ui->quickWidget->setFixedWidth(_sidebarVisible ? 236 : 56);
        const auto *screen = QApplication::screenAt(QCursor::pos());
        const QSize initialSize(1280, 820);
        resize(screen ? initialSize.boundedTo(screen->availableSize() * 0.9) : initialSize);
        updateUDriveAppearance();
        qApp->installEventFilter(this);
    }

    // People perceive this as a Window, so also make Ctrl+W work
    addAction(tr("Hide"), Qt::CTRL | Qt::Key_W, this, &SettingsDialog::hide);
    if (udrive) {
        addAction(tr("Toggle Sidebar"), Qt::CTRL | Qt::Key_B, this, &SettingsDialog::toggleSidebar);
        addAction(tr("Toggle Sidebar"), Qt::CTRL | Qt::Key_Backslash, this, &SettingsDialog::toggleSidebar);
    }

    if (!udrive) {
        _ui->quickWidget->setFixedHeight(minimumHeight() * 0.13);
    }
    _ui->quickWidget->engine()->addImageProvider(QStringLiteral("avatar"), new AvatarImageProvider);
    _ui->quickWidget->setOCContext(QUrl(udrive
        ? QStringLiteral("qrc:/qt/qml/org/ownCloud/gui/qml/UDriveSidebar.qml")
        : QStringLiteral("qrc:/qt/qml/org/ownCloud/gui/qml/AccountBar.qml")), this);
    connect(
        _ui->quickWidget->engine(), &QQmlEngine::quit, QApplication::instance(),
        [this] {
            auto box = new QMessageBox(QMessageBox::Question, tr("Quit %1").arg(Theme::instance()->appNameGUI()),
                tr("Are you sure you want to quit %1?").arg(Theme::instance()->appNameGUI()), QMessageBox::Yes | QMessageBox::No, this);
            box->setAttribute(Qt::WA_DeleteOnClose);
            connect(box, &QMessageBox::accepted, this, [] {
                // delay quit to prevent a Qt 6.6 crash in the destructor of the dialog
                QTimer::singleShot(0, qApp, &QCoreApplication::quit);
            });
            box->open();
        },
        Qt::QueuedConnection);

    _activitySettings = new ActivitySettings;
    _activityPage = udrive ? workspacePage(_activitySettings, tr("Activity"), tr("Review file transfers and sync issues.")) : _activitySettings;
    _ui->stack->addWidget(_activityPage);

    _generalSettings = new GeneralSettings;
    _settingsPage = udrive ? workspacePage(_generalSettings, tr("Settings"), tr("Choose how UDrive works on this computer.")) : _generalSettings;
    _ui->stack->addWidget(_settingsPage);
    connect(_generalSettings, &GeneralSettings::showAbout, gui, &ownCloudGui::slotAbout);
    connect(_generalSettings, &GeneralSettings::syncOptionsChanged, FolderMan::instance(), &FolderMan::slotReloadSyncOptions);

    ConfigFile().restoreGeometry(this);
#ifdef Q_OS_MAC
    // UDrive is a desktop app from startup. Temporarily switching to Accessory
    // removes its Dock entry, then showing the window adds it back as a new launch.
    setActivationPolicy(udrive ? ActivationPolicy::Regular : ActivationPolicy::Accessory);
#endif

    connect(_ui->dialogStack, &QStackedWidget::currentChanged, this, [this] {
        auto *w = _ui->dialogStack->currentWidget();
        if (!w->windowTitle().isEmpty()) {
            setWindowTitle(tr("%1 - %2").arg(Theme::instance()->appNameGUI(), w->windowTitle()));
        } else {
            setWindowTitle(Theme::instance()->appNameGUI());
        }
    });

    setCurrentPage(SettingsPage::Settings);

    // load any existing accounts from the manager.
    const auto accounts = AccountManager::instance()->accounts();
    for (const auto &accountState : accounts) {
        onAccountAdded(accountState);
    }
    if (!_viewForAccount.isEmpty()) {
        setCurrentAccount(accounts.first()->account());
        if (udrive) setCurrentPage(SettingsPage::Files);
    }

    connect(AccountManager::instance(), &AccountManager::accountAdded, this, &SettingsDialog::onAccountAdded);
    connect(AccountManager::instance(), &AccountManager::accountRemoved, this, &SettingsDialog::onAccountRemoved);
}

SettingsDialog::~SettingsDialog()
{
    qApp->removeEventFilter(this);
    // Stop bindings and child cleanup signals before this controller's derived
    // destructor ends; file-browser cleanup can otherwise re-enter its slots.
    _ui->quickWidget->setSource(QUrl());
    for (auto *child : findChildren<QObject *>())
        QObject::disconnect(child, nullptr, this, nullptr);
    QObject::disconnect(AccountManager::instance(), nullptr, this, nullptr);
    QObject::disconnect(this, nullptr, this, nullptr);
    if (_accountPopup)
        _accountPopup->close();
    if (_notificationPopup) {
        _notificationPopup->disconnect();
        _notificationPopup->close();
        delete _notificationPopup.data();
    }
    delete _ui;
}

bool SettingsDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == qApp && (event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::ThemeChange)) {
        if (ConfigFile().themeMode() != ConfigFile::ThemeMode::System) {
            Application::applyApplicationTheme(ConfigFile().themeMode());
        }
        // Allow QApplication to finish applying the new system palette first.
        QTimer::singleShot(0, this, &SettingsDialog::updateUDriveAppearance);
    }
    return QMainWindow::eventFilter(watched, event);
}

void SettingsDialog::createWorkspaceHeader()
{
    auto *header = new QWidget(_ui->page);
    header->setObjectName(QStringLiteral("driveWorkspaceHeader"));
    auto *row = new QHBoxLayout(header);
    row->setContentsMargins(18, 6, 18, 8);
    row->setSpacing(12);
    _brandWidget = new QWidget(header);
    _brandWidget->setObjectName(QStringLiteral("driveBrand"));
    _brandWidget->setFixedWidth(224);
    auto *brandRow = new QHBoxLayout(_brandWidget);
    brandRow->setContentsMargins(0, 0, 0, 0);
    brandRow->setSpacing(8);

    auto *home = new QPushButton(_brandWidget);
    home->setObjectName(QStringLiteral("driveHome"));
    home->setFixedHeight(40);
    home->setCursor(Qt::PointingHandCursor);
    home->setAccessibleName(tr("Go to Personal"));
    home->setToolTip(tr("Go to Personal"));
    auto *homeRow = new QHBoxLayout(home);
    homeRow->setContentsMargins(4, 4, 8, 4);
    homeRow->setSpacing(8);
    auto *logo = new QLabel(home);
    logo->setPixmap(Resources::themeUniversalIcon(QStringLiteral("udrive-icon")).pixmap(30, 30));
    logo->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto *name = new QLabel(QStringLiteral("<span style='color:#00B889'>U</span>Drive"), home);
    name->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto font = name->font();
    font.setPointSize(17);
    font.setBold(true);
    name->setFont(font);
    homeRow->addWidget(logo);
    homeRow->addWidget(name);
    home->setFixedWidth(homeRow->sizeHint().width());
    brandRow->addWidget(home);
    brandRow->addStretch();
    connect(home, &QPushButton::clicked, this, [this] {
        setCurrentPage(SettingsPage::Files);
        if (auto *files = qobject_cast<UDriveFileBrowser *>(_ui->stack->currentWidget()))
            files->showHome();
    });
    row->addWidget(_brandWidget);
    _searchStack = new QStackedWidget(header);
    _searchStack->setObjectName(QStringLiteral("driveHeaderSearch"));
    _searchStack->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    _searchStack->setFixedHeight(44);
    auto *emptySearch = new QLineEdit(header);
    emptySearch->setObjectName(QStringLiteral("driveSearch"));
    emptySearch->setPlaceholderText(tr("Select an account to search files"));
    emptySearch->setEnabled(false);
    _searchStack->addWidget(emptySearch);
    row->addWidget(_searchStack, 1);
    _notificationButton = new NotificationBadgeButton(header);
    connect(_notificationButton, &QPushButton::clicked, this, &SettingsDialog::showNotificationPopup);
    row->addWidget(_notificationButton);
    auto addTool = [this, header, row](UDriveIcons::Symbol symbol, const QString &label, SettingsPage page) {
        auto *button = new QPushButton(header);
        button->setObjectName(QStringLiteral("driveHeaderTool"));
        button->setIcon(UDriveIcons::icon(symbol));
        button->setIconSize(QSize(20, 20));
        button->setFixedSize(36, 36);
        button->setToolTip(label);
        button->setAccessibleName(label);
        connect(button, &QPushButton::clicked, this, [this, page] { setCurrentPage(page); });
        row->addWidget(button);
    };
    addTool(UDriveIcons::Symbol::Activity, tr("Activity"), SettingsPage::Activity);
    addTool(UDriveIcons::Symbol::Settings, tr("Settings"), SettingsPage::Settings);
    _profileButton = new QPushButton(header);
    _profileButton->setObjectName(QStringLiteral("driveProfile"));
    _profileButton->setFixedSize(34, 34);
    _profileButton->setAccessibleName(tr("Switch or manage accounts"));
    _profileButton->setToolTip(tr("Switch or manage accounts"));
    row->addWidget(_profileButton);
    connect(_profileButton, &QPushButton::clicked, this, &SettingsDialog::showAccountPopup);
    // Keep modal sign-in pages separate from the workspace header.
    _ui->verticalLayout_2->removeWidget(_ui->quickWidget);
    _ui->verticalLayout_2->removeWidget(_ui->stack);
    _ui->verticalLayout_2->setDirection(QBoxLayout::TopToBottom);
    _ui->verticalLayout_2->setContentsMargins(0, 0, 0, 0);
    _ui->verticalLayout_2->setSpacing(0);
    _ui->verticalLayout_2->addWidget(header);
    auto *body = new QHBoxLayout;
    body->setContentsMargins(10, 0, 10, 10);
    body->setSpacing(12);
    body->addWidget(_ui->quickWidget);
    body->addWidget(_ui->stack, 1);
    _ui->verticalLayout_2->addLayout(body, 1);
    connect(this, &SettingsDialog::currentPageChanged, this, &SettingsDialog::updateWorkspaceHeader);
    connect(this, &SettingsDialog::currentAccountChanged, this, &SettingsDialog::updateWorkspaceHeader);

    auto settings = ConfigFile::makeQSettings();
    if (!settings.value(QStringLiteral("UDrive/sidebarVisible"), true).toBool()) {
        setSidebarVisible(false);
    }
}

void SettingsDialog::showAccountPopup()
{
    if (_accountPopup && _accountPopup->isVisible()) {
        _accountPopup->close();
        return;
    }
    auto *popup = new AccountPopup(this);
    _accountPopup = popup;
    const QRect available = udrivePopupBounds(this);
    popup->setFixedWidth(qMin(344, available.width() - 20));
    const bool dark = palette().color(QPalette::Window).lightness() < 128;
    popup->setStyleSheet(QStringLiteral("QFrame#driveAccountPopup { background: %4; border: none; border-radius: 14px; }"
                                        "QLabel#driveAccountTitle { color: %1; font-size: 15px; font-weight: 700; padding: 0 4px; }"
                                        "QLabel#driveAccountHint { color: %2; font-size: 12px; padding: 0 4px; }"
                                        "QScrollArea#driveAccountList, QWidget#driveAccountRows { background: transparent; border: none; }"
                                        "QFrame#driveAccountDivider { background: %3; border: none; }")
            .arg(dark ? QStringLiteral("#EDEEF0") : QStringLiteral("#24332C"), dark ? QStringLiteral("#AFB8B2") : QStringLiteral("#64776B"),
                dark ? QStringLiteral("#404642") : QStringLiteral("#E3EAE6"), dark ? QStringLiteral("#2B2D2F") : QStringLiteral("#FFFFFF")));
    auto *layout = new QVBoxLayout(popup);
    layout->setContentsMargins(8, 10, 8, 6);
    layout->setSpacing(2);
    auto *title = new QLabel(tr("Accounts"), popup);
    title->setObjectName(QStringLiteral("driveAccountTitle"));
    title->setTextFormat(Qt::PlainText);
    layout->addWidget(title);
    auto accounts = AccountManager::instance()->accounts();
    if (accounts.size() > 1) {
        auto *hint = new QLabel(tr("Switch between your accounts."), popup);
        hint->setObjectName(QStringLiteral("driveAccountHint"));
        hint->setWordWrap(true);
        hint->setTextFormat(Qt::PlainText);
        layout->addWidget(hint);
    }
    layout->addSpacing(4);
    // Keep the active account first, then use a stable, readable name order.
    std::stable_sort(accounts.begin(), accounts.end(), [this](const auto *left, const auto *right) {
        const bool leftActive = left->account() == _lastAccount;
        const bool rightActive = right->account() == _lastAccount;
        if (leftActive != rightActive)
            return leftActive;
        return left->account()->displayNameWithHost().compare(right->account()->displayNameWithHost(), Qt::CaseInsensitive) < 0;
    });
    AccountPopupButton *firstButton = nullptr;
    if (!accounts.isEmpty()) {
        auto *scroll = new QScrollArea(popup);
        scroll->setObjectName(QStringLiteral("driveAccountList"));
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setFixedHeight(qMin(int(accounts.size()) * 54 - 2, qMax(52, qMin(268, available.height() - 200))));
        auto *rows = new QWidget(scroll);
        rows->setObjectName(QStringLiteral("driveAccountRows"));
        auto *rowLayout = new QVBoxLayout(rows);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(2);
        rowLayout->setAlignment(Qt::AlignTop);
        for (auto *state : accounts) {
            QPointer<Account> account = state->account();
            const QString name = account->davDisplayName().isEmpty() ? account->hostName() : account->davDisplayName();
            const QString server = account->url().adjusted(QUrl::RemoveUserInfo).authority();
            auto *card =
                new AccountPopupButton(name, server, {}, rows);
            card->setObjectName(QStringLiteral("driveAccountCard"));
            card->setProperty("accountId", account->uuid());
            card->setCheckable(true);
            card->setChecked(account == _lastAccount);
            rowLayout->addWidget(card);
            if (!firstButton || card->isChecked())
                firstButton = card;
            connect(card, &QAbstractButton::clicked, this, [this, popup, account] {
                popup->close();
                if (account && _viewForAccount.contains(account->uuid())) {
                    setCurrentAccount(account);
                    setCurrentPage(SettingsPage::Files);
                }
            });
        }
        scroll->setWidget(rows);
        layout->addWidget(scroll);
        if (firstButton)
            scroll->ensureWidgetVisible(firstButton);
    }
    auto *divider = new QFrame(popup);
    divider->setObjectName(QStringLiteral("driveAccountDivider"));
    divider->setFixedHeight(1);
    layout->addSpacing(4);
    layout->addWidget(divider);
    layout->addSpacing(4);
    auto *add = new AccountPopupButton(tr("Add account"), {}, UDriveIcons::icon(UDriveIcons::Symbol::Add), popup);
    add->setObjectName(QStringLiteral("drivePopupAddAccount"));
    layout->addWidget(add);
    connect(add, &QAbstractButton::clicked, this, [this, popup] {
        popup->close();
        createNewAccount();
    });
    if (_lastAccount) {
        auto *manage = new AccountPopupButton(tr("Manage account and sync"), {}, UDriveIcons::icon(UDriveIcons::Symbol::Settings), popup);
        manage->setObjectName(QStringLiteral("drivePopupManageAccount"));
        layout->addWidget(manage);
        connect(manage, &QAbstractButton::clicked, this, [this, popup] {
            popup->close();
            if (_lastAccount)
                setCurrentPage(SettingsPage::Account);
        });
        if (canAdminister()) {
            auto *admin = new AccountPopupButton(tr("Admin settings"), {}, UDriveIcons::icon(UDriveIcons::Symbol::Members), popup);
            admin->setObjectName(QStringLiteral("drivePopupAdminSettings"));
            layout->addWidget(admin);
            connect(admin, &QAbstractButton::clicked, this, [this, popup] {
                popup->close(); setCurrentPage(SettingsPage::Administration);
            });
        }
    }
    for (auto *button : popup->findChildren<QAbstractButton *>())
        button->installEventFilter(popup);
    connect(AccountManager::instance(), &AccountManager::accountsChanged, popup, &QWidget::close);
    connect(this, &SettingsDialog::currentAccountChanged, popup, &QWidget::close);
    popup->adjustSize();
    const QPoint anchor = _profileButton->mapToGlobal(QPoint(_profileButton->width(), _profileButton->height() + 8));
    int x = qBound(available.left() + 8, anchor.x() - popup->width(), available.right() - popup->width() - 8);
    int y = anchor.y();
    if (y + popup->height() > available.bottom() - 8)
        y = _profileButton->mapToGlobal(QPoint(0, -8)).y() - popup->height();
    y = qBound(available.top() + 8, y, available.bottom() - popup->height() - 8);
    popup->move(x, y);
    popup->show();
    (firstButton ? firstButton : add)->setFocus(Qt::PopupFocusReason);
}

void SettingsDialog::showNotificationPopup()
{
    if (_notificationPopup) {
        const bool wasVisible = _notificationPopup->isVisible();
        _notificationPopup->close();
        delete _notificationPopup.data();
        if (wasVisible)
            return;
    }
    if (!_notificationButton)
        return;

    auto *files = qobject_cast<UDriveFileBrowser *>(_ui->stack->currentWidget());
    if (!files && _lastAccount) {
        files = _filesForAccount.value(_lastAccount->uuid(), nullptr);
    }
    auto *notifier = files ? files->sharesNotifier() : nullptr;

    QPointer<UDriveFileBrowser> safeFiles = files;
    auto *popup = new UDriveNotificationPopup(notifier, [this, safeFiles](const QString &id) {
        if (safeFiles) {
            if (safeFiles->sharesNotifier()) {
                safeFiles->sharesNotifier()->markAsRead(id);
            }
            setCurrentPage(SettingsPage::Files);
            safeFiles->navigateToSharesTab();
        }
    }, this);
    _notificationPopup = popup;

    connect(AccountManager::instance(), &AccountManager::accountsChanged, popup, &QWidget::close);
    connect(this, &SettingsDialog::currentAccountChanged, popup, &QWidget::close);

    const QRect available = udrivePopupBounds(this);
    popup->setFixedWidth(qMin(380, available.width() - 20));
    popup->adjustSize();

    const QPoint anchor = _notificationButton->mapToGlobal(QPoint(_notificationButton->width() / 2, _notificationButton->height() + 8));
    int x = anchor.x() - popup->width() / 2;
    x = qBound(available.left() + 8, x, available.right() - popup->width() - 8);
    int y = anchor.y();
    if (y + popup->height() > available.bottom() - 8)
        y = _notificationButton->mapToGlobal(QPoint(0, -8)).y() - popup->height();
    y = qBound(available.top() + 8, y, available.bottom() - popup->height() - 8);

    popup->move(x, y);
    popup->show();
}

void SettingsDialog::updateNotificationBadge()
{
    if (!_notificationButton)
        return;

    auto *files = qobject_cast<UDriveFileBrowser *>(_ui->stack->currentWidget());
    if (!files && _lastAccount) {
        files = _filesForAccount.value(_lastAccount->uuid(), nullptr);
    }
    const int count = (files && files->sharesNotifier()) ? files->sharesNotifier()->unreadCount() : 0;
    _notificationButton->setUnreadCount(count);
}

void SettingsDialog::updateWorkspaceHeader()
{
    if (!_searchStack)
        return;
    auto *files = qobject_cast<UDriveFileBrowser *>(_ui->stack->currentWidget());
    auto *search = files ? files->findChild<QLineEdit *>(QStringLiteral("driveSearch")) : nullptr;
    // Search fields live in the header after integration; retain account identity on each page.
    for (int i = 1; files && i < _searchStack->count(); ++i) {
        auto *field = _searchStack->widget(i);
        if (!field->property("administrationSearch").toBool() && field->property("accountId").toUuid() == files->accountState()->account()->uuid()) {
            search = qobject_cast<QLineEdit *>(field);
            break;
        }
    }
    if (_currentPage == SettingsPage::Administration && _lastAccount) {
        if (auto *admin = _adminForAccount.value(_lastAccount->uuid())) {
            search = admin->searchField();
            if (_searchStack->indexOf(search) < 0) {
                admin->takeSearchField(); search->setProperty("accountId", _lastAccount->uuid()); search->setProperty("administrationSearch", true);
                _searchStack->addWidget(search);
            }
        }
    }
    _searchStack->setCurrentIndex(search ? _searchStack->indexOf(search) : 0);
    _profileButton->setText(_lastAccount ? _lastAccount->initials().toUpper().left(3) : QStringLiteral("+"));
    updateNotificationBadge();
    Q_EMIT workspaceChanged();
}

bool SettingsDialog::canCreateFiles() const
{
    if (_currentPage == SettingsPage::Administration && _lastAccount) {
        auto *admin = _adminForAccount.value(_lastAccount->uuid()); return admin && admin->canCreate();
    }
    auto *files = qobject_cast<UDriveFileBrowser *>(_ui->stack->currentWidget());
    return files && files->canCreate();
}

bool SettingsDialog::canAdminister() const
{
    auto *admin = _lastAccount ? _adminForAccount.value(_lastAccount->uuid()) : nullptr;
    return admin && admin->api()->available();
}
QString SettingsDialog::adminCreationLabel() const
{
    auto *admin = _lastAccount ? _adminForAccount.value(_lastAccount->uuid()) : nullptr;
    return admin ? admin->creationLabel() : QString();
}

void SettingsDialog::openNewFileMenu()
{
    if (_currentPage == SettingsPage::Administration && _lastAccount) {
        if (auto *admin = _adminForAccount.value(_lastAccount->uuid())) admin->createNew();
        return;
    }
    auto *files = qobject_cast<UDriveFileBrowser *>(_ui->stack->currentWidget());
    auto *root = _ui->quickWidget->rootObject();
    auto *button = root ? root->findChild<QQuickItem *>(QStringLiteral("udriveNew")) : nullptr;
    if (!files || !button || !button->isVisible()) return;
    if (files->isWorkspacesOverview()) {
        files->createNewWorkspace();
        return;
    }
    const QRectF bounds = button->mapRectToScene(button->boundingRect());
    const QPoint position(qRound(bounds.left()), qRound(bounds.bottom()) + 8);
    files->showNewMenu(_ui->quickWidget->mapToGlobal(position), qRound(bounds.width()));
}

bool SettingsDialog::isWorkspacesOverview() const
{
    auto *files = qobject_cast<UDriveFileBrowser *>(_ui->stack->currentWidget());
    return files && files->isWorkspacesOverview();
}

QString SettingsDialog::storageLabel() const
{
    if (!_lastAccount)
        return tr("No account connected");
    // Match the web's decimal units and omit redundant decimals for whole units.
    const auto formatSize = [](qint64 bytes) {
        qint64 unit = 1;
        while (bytes / unit >= 1000) unit *= 1000;
        const int precision = bytes < 1024 * 1024 || bytes % unit == 0 ? 0 : 1;
        return QLocale().formattedDataSize(bytes, precision, QLocale::DataSizeSIFormat);
    };
    for (const auto *space : _lastAccount->spacesManager()->spaces()) {
        if (space->sortPriority() != 100 || space->disabled())
            continue;
        const auto quota = space->quota();
        if (quota.is_used_Valid() && quota.getUsed() >= 0) {
            const QString used = formatSize(quota.getUsed());
            if (quota.is_total_Valid() && quota.getTotal() > 0)
                return tr("%1 of %2 used").arg(used, formatSize(quota.getTotal()));
            return tr("%1 used").arg(used);
        }
    }
    return tr("Storage information unavailable");
}

double SettingsDialog::storageFraction() const
{
    if (_lastAccount)
        for (const auto *space : _lastAccount->spacesManager()->spaces()) {
            const auto quota = space->quota();
            if (space->sortPriority() == 100 && !space->disabled() && quota.is_used_Valid() && quota.is_total_Valid() && quota.getTotal() > 0)
                return qBound(0.0, double(quota.getUsed()) / double(quota.getTotal()), 1.0);
        }
    return 0;
}

void SettingsDialog::updateUDriveAppearance()
{
    if (Theme::instance()->appNameGUI() != QStringLiteral("UDrive"))
        return;

    // Scope these styles to the content area; OAuth wizards and native dialogs
    // continue to use their own palettes. Follow the system's light/dark mode or user preference.
    const auto themeMode = ConfigFile().themeMode();
    const bool dark = themeMode == ConfigFile::ThemeMode::Dark
        || (themeMode == ConfigFile::ThemeMode::System && qApp->palette().color(QPalette::Window).lightness() < 128);
    const QString surface = dark ? QStringLiteral("#292929") : QStringLiteral("#FFFFFF");
    const QString text = dark ? QStringLiteral("#EDEEF0") : QStringLiteral("#2B3035");
    const QString border = dark ? QStringLiteral("#414141") : QStringLiteral("#E1E5E8");
    const QString hover = dark ? QStringLiteral("#323E35") : QStringLiteral("#EAF8EF");
    const QString disabledText = dark ? QStringLiteral("#939D97") : QStringLiteral("#7D8781");
    const QString disabledSurface = dark ? QStringLiteral("#303431") : QStringLiteral("#F3F5F4");
    const QString arrow = dark ? QStringLiteral("chevron-dark") : QStringLiteral("chevron-light");
    const QString toolbarSurface = dark ? QStringLiteral("#303330") : QStringLiteral("#FAFCFB");
    const QString toolbarBorder = dark ? QStringLiteral("#535A56") : QStringLiteral("#CCD4CE");
    const QString toolbarAccent = dark ? QStringLiteral("#40D979") : QStringLiteral("#007C58");
    _ui->quickWidget->setClearColor(dark ? QColor("#202020") : QColor("#F6F7F8"));
    _ui->page->setStyleSheet(QStringLiteral(
        "QWidget#page { background: %8; color: %2; }"
        "QStackedWidget#stack { border: 1px solid %3; border-radius: 16px; }"
        "QWidget#driveWorkspaceHeader { background: %8; color: %2; }"
        "QPushButton#driveHome { border: 1px solid transparent; border-radius: 8px; background: transparent; }"
        "QPushButton#driveHome:hover { background: %4; }"
        "QPushButton#driveHome:focus { border-color: #40D979; }"
        "QWidget#driveWorkspaceHeader QPushButton#driveHeaderTool { border: none; border-radius: 8px; background: transparent; }"
        "QWidget#driveWorkspaceHeader QPushButton#driveHeaderTool:hover { background: %4; }"
        "QWidget#driveWorkspaceHeader QPushButton#driveHeaderTool:focus { border: 1px solid #40D979; }"
        "QPushButton#driveProfile { border: none; border-radius: 17px; color: white; background: #92600D; font-weight: 600; }"
        "QPushButton#driveProfile:hover { background: #AE7516; }"
        "QPushButton#driveProfile:focus { border: 2px solid #40D979; }"
        "QStackedWidget#stack { background: %1; }"
        "QStackedWidget#stack QPushButton { min-height: 20px; padding: 6px 10px; border: 1px solid %3; border-radius: 8px; background: %1; color: %2; font-weight: 500; }"
        "QStackedWidget#stack QPushButton:hover { background: %4; border-color: #00856B; }"
        "QStackedWidget#stack QPushButton:pressed { background: %4; border-color: #00856B; }"
        "QStackedWidget#stack QPushButton:focus { border-color: #00856B; }"
        "QStackedWidget#stack QPushButton:disabled { background: %6; color: %5; border-color: %3; }"
        "QStackedWidget#stack QPushButton[udriveCompact=\"true\"] { min-height: 18px; padding: 4px 8px; }"
        "QStackedWidget#stack QPushButton[udriveBreadcrumb=\"true\"] { min-height: 18px; padding: 4px 6px; border-radius: 6px; border-color: transparent; background: transparent; }"
        "QStackedWidget#stack QPushButton[udriveBreadcrumb=\"true\"]:hover { background: %4; border-color: transparent; color: %2; }"
        "QStackedWidget#stack QPushButton[udriveBreadcrumb=\"true\"]:focus { border-color: #00856B; }"
        "QStackedWidget#stack QPushButton[udriveBreadcrumb=\"true\"]:disabled { background: transparent; border-color: transparent; color: %5; }"
        "QStackedWidget#stack QPushButton[breadcrumbCurrent=\"true\"]:disabled { color: %2; font-weight: 600; }"
        "QStackedWidget#stack QPushButton[udriveBreadcrumb=\"true\"]::menu-indicator { image: none; width: 0; height: 0; }"
        "QLabel#driveCurrentFolder { color: %2; padding: 4px; }"
        "QLabel#driveBreadcrumbSeparator { color: %5; font-size: 20px; }"
        "QStackedWidget#stack QPushButton[udriveMenuButton=\"true\"] { padding-right: 26px; }"
        "QStackedWidget#stack QPushButton::menu-indicator { image: url(:/udrive/icons/%7.svg); width: 10px; height: 10px; subcontrol-origin: padding; subcontrol-position: center right; right: 8px; }"
        "QStackedWidget#stack QPushButton[udrivePrimary=\"true\"] { background: #40D979; border-color: #40D979; color: #12351D; font-weight: 600; }"
        "QStackedWidget#stack QPushButton[udrivePrimary=\"true\"]:hover { background: #63E493; border-color: #63E493; }"
        "QStackedWidget#stack QPushButton[udrivePrimary=\"true\"]:pressed { background: #30C86A; border-color: #30C86A; }"
        "QStackedWidget#stack QPushButton[udrivePrimary=\"true\"]:focus { border-color: #007C58; }"
        "QStackedWidget#stack QPushButton[udrivePrimary=\"true\"]::menu-indicator { image: url(:/udrive/icons/chevron-primary.svg); }"
        "QStackedWidget#stack QPushButton[udrivePrimary=\"true\"]:disabled { background: %6; border-color: %3; color: %5; }"
        "QStackedWidget#stack QPushButton::menu-indicator:disabled { image: url(:/udrive/icons/chevron-disabled.svg); }"
        "QStackedWidget#stack QMenu { padding: 6px; border: 1px solid %3; border-radius: 10px; background: %1; color: %2; }"
        "QStackedWidget#stack QMenu::item { padding: 6px 14px 6px 8px; border-radius: 6px; }"
        "QStackedWidget#stack QMenu::item:selected { background: %4; color: %2; }"
        "QStackedWidget#stack QMenu::item:disabled { color: %5; }"
        "QStackedWidget#stack QMenu::separator { height: 1px; background: %3; margin: 5px 6px; }"
        "QStackedWidget#stack QMenu#driveNewMenu { padding: 8px; border-radius: 12px; }"
        "QStackedWidget#stack QMenu#driveNewMenu::item { min-height: 22px; padding: 7px 18px 7px 12px; margin: 1px 0; border-radius: 8px; font-size: 14px; font-weight: 500; }"
        "QStackedWidget#stack QMenu#driveNewMenu::icon { left: 10px; }"
        "QStackedWidget#stack QMenu#driveNewMenu::item:selected { background: %4; color: %11; }"
        "QStackedWidget#stack QMenu#driveNewMenu::item:disabled { color: %5; }"
        "QCheckBox#driveSelectAll, QCheckBox#driveWorkspaceSelectAll, QCheckBox#driveWorkspaceHeaderSelectAll { color: %2; spacing: 8px; padding-right: 4px; }"
        "QCheckBox#driveSelectAll:disabled, QCheckBox#driveWorkspaceSelectAll:disabled, QCheckBox#driveWorkspaceHeaderSelectAll:disabled { color: %5; }"
        "QCheckBox#driveSelectAll::indicator, QCheckBox#driveWorkspaceSelectAll::indicator, QCheckBox#driveWorkspaceHeaderSelectAll::indicator { width: 18px; height: 18px; border: 1px solid #858585; border-radius: 4px; background: transparent; }"
        "QCheckBox#driveSelectAll::indicator:hover, QCheckBox#driveWorkspaceSelectAll::indicator:hover, QCheckBox#driveWorkspaceHeaderSelectAll::indicator:hover { border-color: %11; }"
        "QCheckBox#driveSelectAll::indicator:checked, QCheckBox#driveWorkspaceSelectAll::indicator:checked, QCheckBox#driveWorkspaceHeaderSelectAll::indicator:checked { border-color: #40D979; background: #40D979; image: url(:/client/resources/core/checkbox-check.svg); }"
        "QCheckBox#driveSelectAll::indicator:indeterminate, QCheckBox#driveWorkspaceSelectAll::indicator:indeterminate, QCheckBox#driveWorkspaceHeaderSelectAll::indicator:indeterminate { border-color: #40D979; background: #40D979; image: url(:/client/resources/core/checkbox-partial.svg); }"
        "QCheckBox#driveSelectAll::indicator:disabled, QCheckBox#driveWorkspaceSelectAll::indicator:disabled, QCheckBox#driveWorkspaceHeaderSelectAll::indicator:disabled { border-color: %3; background: %6; }"
        "QStackedWidget#stack QGroupBox { border: 1px solid %3; border-radius: 12px; margin-top: 22px; padding: 12px; }"
        "QStackedWidget#stack QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; padding: 0 4px; color: %2; }"
        "QWidget[udriveNetwork=\"true\"] QGroupBox#proxyGroupBox { border: none; margin-top: 20px; padding: 0; }"
        "QWidget[udriveNetwork=\"true\"] QGroupBox#proxyGroupBox::title { padding: 0; font-weight: 600; }"
        "QWidget[udriveNetwork=\"true\"] QRadioButton, QWidget[udriveNetwork=\"true\"] QCheckBox { spacing: 8px; min-height: 24px; }"
        "QWidget[udriveNetwork=\"true\"] QRadioButton::indicator { width: 16px; height: 16px; border: 1px solid %10; border-radius: 9px; background: %8; }"
        "QWidget[udriveNetwork=\"true\"] QRadioButton::indicator:checked { border-color: %11; background: qradialgradient(cx: 0.5, cy: 0.5, radius: 0.5, fx: 0.5, fy: 0.5, stop: 0 #40D979, stop: 0.4 #40D979, stop: 0.41 %8, stop: 1 %8); }"
        "QWidget[udriveNetwork=\"true\"] QRadioButton::indicator:hover, QWidget[udriveNetwork=\"true\"] QRadioButton::indicator:focus { border-color: %11; }"
        "QWidget[udriveNetwork=\"true\"] QCheckBox::indicator { width: 16px; height: 16px; border: 1px solid %10; border-radius: 4px; background: %8; }"
        "QWidget[udriveNetwork=\"true\"] QCheckBox::indicator:checked { border-color: #40D979; background: #40D979; image: url(:/client/resources/core/checkbox-check.svg); }"
        "QWidget[udriveNetwork=\"true\"] QCheckBox::indicator:hover { border-color: %11; }"
        "QWidget[udriveNetwork=\"true\"] QRadioButton::indicator:disabled, QWidget[udriveNetwork=\"true\"] QCheckBox::indicator:disabled { background: %6; border-color: %3; }"
        "QWidget[udriveNetwork=\"true\"] QLineEdit[proxyInput=\"true\"], QWidget[udriveNetwork=\"true\"] QSpinBox#portSpinBox { min-height: 20px; padding: 7px 10px; border: 1px solid %3; border-radius: 8px; background: %8; color: %2; font-size: 13px; selection-background-color: %4; }"
        "QWidget[udriveNetwork=\"true\"] QLineEdit[proxyInput=\"true\"]:hover, QWidget[udriveNetwork=\"true\"] QSpinBox#portSpinBox:hover { border-color: %10; }"
        "QWidget[udriveNetwork=\"true\"] QLineEdit[proxyInput=\"true\"]:focus, QWidget[udriveNetwork=\"true\"] QSpinBox#portSpinBox:focus { border-color: #00856B; }"
        "QWidget[udriveNetwork=\"true\"] QLineEdit[proxyInput=\"true\"]:disabled, QWidget[udriveNetwork=\"true\"] QSpinBox#portSpinBox:disabled { background: %6; border-color: %3; color: %5; }"
        "QWidget[udriveNetwork=\"true\"] QLineEdit[proxyHostMissing=\"true\"] { border-color: #D66A73; }"
        "QWidget[udriveNetwork=\"true\"] QLabel#proxyHostHint { color: #D66A73; font-size: 12px; }"
        "QWidget[udriveNetwork=\"true\"] QLabel#labelLocalhost { color: %5; font-size: 12px; }"
        "QScrollArea#scrollArea { border: none; background: %1; }"
        "QWidget#scrollAreaWidgetContents_2 { background: %1; }"
        "QLineEdit#driveSearch { min-height: 20px; padding: 10px 12px; border: 1px solid %3; border-radius: 21px; background: %8; color: %2; }"
        "QLineEdit#driveSearch:hover { border-color: %10; }"
        "QLineEdit#driveSearch:focus { border-color: #00856B; }"
        "QLineEdit#driveSearch QToolButton { border: none; border-radius: 14px; padding: 0; background: transparent; }"
        "QLineEdit#driveSearch QToolButton:hover { background: %4; }"
        "QLineEdit#driveSearch QToolButton:pressed { background: %6; }"
        "QTreeView#driveWorkspacesTable { border: none; background: %1; color: %2; padding: 0; outline: none; }"
        "QTreeView#driveFiles { border: none; background: %1; color: %2; padding: 0; outline: none; }"
        "QListView#driveTiles { border: none; background: %1; color: %2; padding: 0; outline: none; }"
        "QListView#driveWorkspacesView { border: none; background: %1; color: %2; padding: 4px; outline: none; }"
        "QStackedWidget#stack QPushButton#driveLearnWorkspaces { background: %12; border: none; border-radius: 16px; min-height: 18px; padding: 0 12px; color: %2; font-weight: 600; }"
        "QStackedWidget#stack QPushButton#driveLearnWorkspaces:hover { background: %6; color: %2; }"
        "QWidget#driveWorkspaceControls QPushButton#driveIncludeDisabled { border: 1px solid %5; border-radius: 14px; padding: 4px 14px; min-height: 20px; background: transparent; color: %2; font-weight: 400; font-size: 13px; }"
        "QWidget#driveWorkspaceControls QPushButton#driveIncludeDisabled:hover { background: %4; }"
        "QWidget#driveWorkspaceControls QPushButton#driveIncludeDisabled:checked { background: %4; border-color: %11; color: %11; }"
        "QWidget#driveWorkspaceControls QLineEdit#driveWorkspaceSearch { border: 1px solid %5; border-radius: 5px; padding: 4px 10px; background: %12; color: %2; }"
        "QWidget#driveWorkspaceControls QLineEdit#driveWorkspaceSearch:focus { border-color: %11; }"
        "QWidget#driveWorkspaceControls QComboBox#driveWorkspaceSort { border: 1px solid %5; border-radius: 5px; padding: 6px 30px 6px 10px; background: %12; color: %2; }"
        "QWidget#driveWorkspaceControls QComboBox#driveWorkspaceSort::drop-down { border: none; width: 24px; }"
        "QWidget#driveWorkspaceControls QComboBox#driveWorkspaceSort::down-arrow { image: url(:/udrive/icons/%7.svg); width: 12px; height: 12px; }"
        "QFrame#driveWorkspaceRule, QFrame#driveWorkspaceTitleDivider { color: %3; background: %3; border: none; }"
        "QFrame#driveWorkspaceRule { max-height: 1px; }"
        "QFrame#driveWorkspaceTitleDivider { max-width: 1px; }"
        "QLabel#driveWorkspaceCount { color: %5; font-size: 12px; padding: 4px 0; }"
        "QWidget#driveWorkspaceBulkActions QToolButton { border: none; border-radius: 8px; background: transparent; color: %2; padding: 6px 8px; font-size: 13px; }"
        "QWidget#driveWorkspaceBulkActions QToolButton:hover { background: %4; }"
        "QWidget#driveWorkspaceBulkActions QToolButton:disabled { color: %5; }"
        "QWidget#driveWorkspaceHeaderActions QToolButton { border: 1px solid transparent; border-radius: 8px; background: transparent; padding: 4px; }"
        "QWidget#driveWorkspaceHeaderActions QToolButton:hover { background: %12; }"
        "QWidget#driveWorkspaceHeaderActions QToolButton:focus { border-color: %11; }"
        "QToolButton#driveWorkspaceViewMode::menu-indicator { image: none; width: 0; }"
        "QWidget#driveSharesNav QPushButton[shareTab=\"true\"] { border: none; border-bottom: 2px solid transparent; border-radius: 0; padding: 6px 14px; background: transparent; color: %5; font-size: 13px; font-weight: 500; }"
        "QWidget#driveSharesNav QPushButton[shareTab=\"true\"]:hover { color: %2; }"
        "QWidget#driveSharesNav QPushButton[shareTab=\"true\"]:checked { border-bottom: 2px solid #40D979; color: %2; font-weight: 600; }"
        "QWidget#driveSharesNav QToolButton#driveShareSettings { border: none; border-radius: 8px; background: transparent; padding: 4px; }"
        "QWidget#driveSharesNav QToolButton#driveShareSettings::menu-indicator { image: none; width: 0; height: 0; }"
        "QWidget#driveSharesNav QToolButton#driveShareSettings:hover { background: %4; }"
        "QWidget#driveSharesControls QPushButton#driveShareFilterShares, QWidget#driveSharesControls QPushButton#driveShareFilterHidden { border: 1px solid %5; border-radius: 14px; padding: 4px 14px; min-height: 20px; background: transparent; color: %2; font-weight: 400; font-size: 13px; }"
        "QWidget#driveSharesControls QPushButton#driveShareFilterShares:hover, QWidget#driveSharesControls QPushButton#driveShareFilterHidden:hover { background: %4; }"
        "QWidget#driveSharesControls QPushButton#driveShareFilterShares:checked { background: #40D979; border-color: #40D979; color: #12351D; font-weight: 600; }"
        "QWidget#driveSharesControls QPushButton#driveShareFilterHidden:checked { background: %4; border-color: %11; color: %11; font-weight: 500; }"
        "QWidget#driveSharesControls QLineEdit#driveShareSearch { border: 1px solid %5; border-radius: 5px; padding: 4px 10px; background: %12; color: %2; }"
        "QWidget#driveSharesControls QLineEdit#driveShareSearch:focus { border-color: %11; }"
        "QWidget#driveSharesControls QComboBox#driveShareTypeFilter, QWidget#driveSharesControls QComboBox#driveShareByFilter { border: 1px solid %5; border-radius: 14px; padding: 4px 26px 4px 12px; min-height: 20px; background: transparent; color: %2; font-size: 13px; }"
        "QWidget#driveSharesControls QComboBox#driveShareTypeFilter::drop-down, QWidget#driveSharesControls QComboBox#driveShareByFilter::drop-down { border: none; width: 20px; }"
        "QWidget#driveSharesControls QComboBox#driveShareTypeFilter::down-arrow, QWidget#driveSharesControls QComboBox#driveShareByFilter::down-arrow { image: url(:/udrive/icons/%7.svg); width: 10px; height: 10px; }"
        "QFrame#driveSharesRule { color: %3; background: %3; border: none; max-height: 1px; }"
        "QWidget#driveEmptyState { background: %1; }"
        "QLabel#driveEmptyText { color: %2; font-size: 18px; font-weight: 600; }"
        "QStackedWidget#stack QPushButton[driveToolbarButton=\"true\"] { min-height: 32px; padding: 0 10px; border: 1px solid transparent; border-radius: 8px; background: transparent; font-size: 13px; font-weight: 500; }"
        "QStackedWidget#stack QPushButton[driveToolbarButton=\"true\"]:hover { background: %4; border-color: transparent; }"
        "QStackedWidget#stack QPushButton[driveToolbarButton=\"true\"]:pressed { background: %4; border-color: transparent; }"
        "QStackedWidget#stack QPushButton[driveToolbarButton=\"true\"]:focus { border-color: %11; }"
        "QStackedWidget#stack QPushButton[driveToolbarButton=\"true\"]:disabled { background: transparent; color: %5; border-color: transparent; }"
        "QStackedWidget#stack QPushButton#driveRefresh { padding: 0; }"
        "QStackedWidget#stack QPushButton#driveViewMode { padding: 0 20px 0 8px; }"
        "QStackedWidget#stack QPushButton#driveViewMode::menu-indicator { image: url(:/udrive/icons/%7.svg); width: 10px; height: 10px; subcontrol-origin: padding; subcontrol-position: center right; right: 6px; }"
        "QStackedWidget#stack QPushButton#driveSort { padding: 0 24px 0 8px; }"
        "QStackedWidget#stack QPushButton#driveSort::menu-indicator { image: url(:/udrive/icons/%7.svg); width: 10px; height: 10px; subcontrol-origin: padding; subcontrol-position: center right; right: 8px; }"
        "QTreeView#driveWorkspacesTable::item, QTreeView#driveFiles::item { padding: 6px 8px; border-bottom: 1px solid %3; }"
        "QTreeView#driveWorkspacesTable::item:hover, QTreeView#driveFiles::item:hover { background: %6; }"
        "QTreeView#driveWorkspacesTable::item:selected, QTreeView#driveFiles::item:selected { background: %4; color: %2; }"
        "QTreeView#driveWorkspacesTable QHeaderView::section, QTreeView#driveFiles QHeaderView::section { background: %1; color: %2; border: none; border-bottom: 1px solid %3; padding: 12px 8px; }"
        "QStackedWidget#stack QComboBox#driveSpaces, QStackedWidget#stack QComboBox#languageDropdown, QStackedWidget#stack QComboBox#pdfZoom { min-height: 22px; border: 1px solid %3; border-radius: 9px; padding: 8px 38px 8px 12px; background: %1; color: %2; }"
        "QWidget[udriveNetwork=\"true\"] QComboBox#typeComboBox { min-height: 20px; padding: 7px 30px 7px 10px; border: 1px solid %3; border-radius: 8px; background: %8; color: %2; font-size: 13px; }"
        "QWidget[udriveNetwork=\"true\"] QComboBox#typeComboBox:hover, QWidget[udriveNetwork=\"true\"] QComboBox#typeComboBox:focus { border-color: #00856B; }"
        "QStackedWidget#stack QComboBox#driveSpaces:hover, QStackedWidget#stack QComboBox#languageDropdown:hover, QStackedWidget#stack QComboBox#pdfZoom:hover { border-color: #00856B; }"
        "QStackedWidget#stack QComboBox#driveSpaces:focus, QStackedWidget#stack QComboBox#languageDropdown:focus, QStackedWidget#stack QComboBox#pdfZoom:focus { border-color: #00856B; }"
        "QStackedWidget#stack QComboBox[udriveCombo=\"true\"]::drop-down { subcontrol-origin: padding; subcontrol-position: center right; width: 30px; border: none; }"
        "QStackedWidget#stack QComboBox[udriveCombo=\"true\"]::down-arrow { image: url(:/udrive/icons/%7.svg); width: 14px; height: 14px; }"
        "QStackedWidget#stack QComboBox[udriveCombo=\"true\"]:disabled { color: %5; background: %6; }"
        "QStackedWidget#stack QComboBox[udriveCombo=\"true\"]::down-arrow:disabled { image: url(:/udrive/icons/chevron-disabled.svg); }"
        "QStackedWidget#stack QComboBox[udriveCombo=\"true\"] QAbstractItemView { background: %1; color: %2; selection-background-color: %4; selection-color: %11; border: 1px solid %3; border-radius: 8px; padding: 4px; outline: none; }"
        "QStackedWidget#stack QComboBox[udriveCombo=\"true\"] QAbstractItemView::item { min-height: 24px; padding: 4px 8px; border: none; border-radius: 5px; }"
        "QStackedWidget#stack QComboBox[udriveCombo=\"true\"] QAbstractItemView::item:hover { background: %6; }"
        "QStackedWidget#stack QComboBox[udriveCombo=\"true\"] QAbstractItemView::item:selected { background: %4; color: %11; }"
        "QStackedWidget#stack QComboBox[udriveCombo=\"true\"] { combobox-popup: 0; }"
        "QComboBox#languageDropdown QListView#driveLanguagePopup { background: %9; color: %2; border: 1px solid %10; border-radius: 9px; padding: 5px; outline: none; }"
        "QComboBox#languageDropdown QListView#driveLanguagePopup::item { min-height: 24px; padding: 6px 10px; border: none; border-radius: 6px; }"
        "QComboBox#languageDropdown QListView#driveLanguagePopup::item:hover { background: %6; }"
        "QComboBox#languageDropdown QListView#driveLanguagePopup::item:selected { background: %4; color: %11; }"
        "QStackedWidget#stack QComboBox#driveSpaces { min-height: 20px; padding: 6px 28px 6px 10px; border-radius: 8px; border-color: %10; background: %9; font-size: 13px; font-weight: 500; }"
        "QStackedWidget#stack QComboBox#driveSpaces:hover, QStackedWidget#stack QComboBox#driveSpaces:focus { border-color: %11; }"
        "QStackedWidget#stack QComboBox#driveSpaces::drop-down { width: 24px; }"
        "QStackedWidget#stack QComboBox#driveSpaces::down-arrow { width: 10px; height: 10px; }"
        "QListView#driveSpacesPopup { background: %9; color: %2; border: 1px solid %10; border-radius: 8px; padding: 4px; outline: none; }"
        "QListView#driveSpacesPopup::item { padding: 4px 8px; border: none; border-radius: 5px; }"
        "QListView#driveSpacesPopup::item:hover { background: %6; }"
        "QListView#driveSpacesPopup::item:selected { background: %4; color: %11; }"
        "QWidget[udriveSync=\"true\"], QWidget[udriveSync=\"true\"] QFrame#workspaceHeader, QWidget[udriveSync=\"true\"] QWidget#accountStatus { background: transparent; border: none; }"
        "QWidget[udriveSync=\"true\"] QLabel { background: transparent; color: %2; }"
        "QWidget[udriveSync=\"true\"] QLabel#workspaceDescription, QLabel#syncSectionDescription, QLabel#syncedFolderCount { color: %5; }"
        "QStackedWidget#stack QWidget[udriveSync=\"true\"] QPushButton[syncAction=\"true\"] { background: transparent; border: 1px solid %3; border-radius: 8px; min-height: 20px; padding: 7px 12px; }"
        "QStackedWidget#stack QWidget[udriveSync=\"true\"] QPushButton[syncAction=\"true\"]:hover { background: %4; border-color: %11; }"
        "QStackedWidget#stack QWidget[udriveSync=\"true\"] QPushButton[syncAction=\"true\"]:pressed { background: %6; }"
        "QStackedWidget#stack QWidget[udriveSync=\"true\"] QPushButton[syncAction=\"true\"]:focus { border-color: %11; }"
        "QStackedWidget#stack QWidget[udriveSync=\"true\"] QPushButton[syncAction=\"true\"]:disabled { background: transparent; color: %5; border-color: %3; }"
        "QWidget#driveAdministration, QWidget#driveAdministration QLabel { background: transparent; color: %2; }"
        "QTreeView#driveAdminTable { border: none; background: %1; color: %2; outline: none; }"
        "QTreeView#driveAdminTable::item { padding: 10px 8px; border-bottom: 1px solid %3; }"
        "QTreeView#driveAdminTable::item:selected { background: %4; color: %2; }"
        "QTreeView#driveAdminTable QHeaderView::section { background: %1; color: %2; border: none; border-bottom: 1px solid %3; padding: 10px 8px; }"
        "QTabBar#driveAdminTabs::tab { background: transparent; color: %5; border-bottom: 2px solid transparent; padding: 8px 14px; }"
        "QTabBar#driveAdminTabs::tab:selected { color: %2; border-bottom-color: %11; }"
        "QComboBox#driveAdminGroupFilter, QComboBox#driveAdminRoleFilter { background: transparent; color: %2; border: 1px solid %3; border-radius: 8px; padding: 7px 28px 7px 10px; }"
        "QLabel#driveAdminCount { color: %5; font-size: 12px; }"
        "QTreeView#accountFoldersTreeView { border: none; border-radius: 0; background: %1; color: %2; padding: 0; }"
        "QLabel#syncedFolderCount { padding-top: 8px; font-size: 12px; }"
        "QTabWidget#activityTabWidget { background: transparent; border: none; }"
        "QTabWidget#activityTabWidget::pane { border: none; background: transparent; top: 0; }"
        "QTabBar#activityTabBar { background: transparent; border: none; border-bottom: 1px solid %3; }"
        "QTabBar#activityTabBar::tab { background: transparent; color: %5; border: none; border-bottom: 2px solid transparent; padding: 8px 16px; font-size: 13px; font-weight: 500; margin-right: 8px; }"
        "QTabBar#activityTabBar::tab:hover { color: %2; background: %4; border-radius: 6px 6px 0 0; }"
        "QTabBar#activityTabBar::tab:selected { color: %2; border-bottom: 2px solid #40D979; font-weight: 600; }"
        "QTableView#activityTableView { border: 1px solid %3; border-radius: 12px; background: %1; color: %2; alternate-background-color: %12; gridline-color: transparent; outline: none; selection-background-color: %4; selection-color: %2; }"
        "QTableView#activityTableView::item { padding: 8px 10px; border: none; border-bottom: 1px solid %3; }"
        "QTableView#activityTableView::item:hover { background: %4; }"
        "QTableView#activityTableView::item:selected { background: %4; color: %2; }"
        "QTableView#activityTableView QHeaderView { background: %1; border: none; border-bottom: 1px solid %3; border-top-left-radius: 11px; border-top-right-radius: 11px; }"
        "QTableView#activityTableView QHeaderView::section { background: %1; color: %5; border: none; border-bottom: 1px solid %3; padding: 10px 12px; font-weight: 600; font-size: 12px; }"
        "QTableView#activityTableView QHeaderView::section:hover { color: %2; }"
        "QTableView#activityTableView QCornerButton::section { background: %1; border: none; }"
        "QPushButton#activityFilterButton { border: 1px solid %3; border-radius: 14px; min-height: 20px; padding: 4px 14px; background: %1; color: %2; font-size: 13px; font-weight: 500; }"
        "QPushButton#activityFilterButton:hover { background: %4; border-color: %11; }"
        "QPushButton#activityFilterButton:focus { border-color: %11; }"
        "QPushButton#activityFilterButton[filterActive=\"true\"] { background: %4; border-color: %11; color: %11; font-weight: 600; }"
        "QLabel#activityItemCount { color: %5; font-size: 13px; font-weight: 500; padding-left: 2px; }"
        "QWidget#activityEmptyState { background: %1; border: 1px solid %3; border-radius: 12px; }"
        "QLabel#activityEmptyTitle { color: %2; font-size: 17px; font-weight: 600; }"
        "QLabel#activityEmptySubtitle { color: %5; font-size: 13px; }"
        "QLabel#_tooManyIssuesWarning, QLabel#_conflictHelp { color: #E53E3E; font-size: 12px; padding: 4px 0; }")
        .arg(surface, text, border, hover, disabledText, disabledSurface, arrow, dark ? QStringLiteral("#202020") : QStringLiteral("#F6F7F8"))
        .arg(toolbarSurface, toolbarBorder, toolbarAccent, dark ? QStringLiteral("#383838") : QStringLiteral("#F4F5F4")));
}

void SettingsDialog::onAccountAdded(AccountState *state)
{
    if (!state || !state->account())
        return;
    // asap we need to create some kind of accountView builder that will instantiate a controller and view + whatever else
    // as currently everything is in the view which is absolutely not ok, especially given the multitude of "heavy lifting"
    // that goes on in there

    auto accountView = new AccountView(state, this);
    _ui->stack->addWidget(accountView);
    _viewForAccount.insert(state->account()->uuid(), accountView);
    if (Theme::instance()->appNameGUI() == QStringLiteral("UDrive")) {
        auto *files = new UDriveFileBrowser(state, this);
        _ui->stack->addWidget(files);
        _filesForAccount.insert(state->account()->uuid(), files);
        if (files->sharesNotifier()) {
            connect(files->sharesNotifier(), &UDriveIncomingSharesNotifier::unreadCountChanged,
                    this, &SettingsDialog::updateNotificationBadge);
        }
        auto *admin = new UDriveAdminWidget(state, this);
        _adminForAccount.insert(state->account()->uuid(), admin);
        _ui->stack->addWidget(admin);
        connect(admin, &UDriveAdminWidget::controlsChanged, this, &SettingsDialog::workspaceChanged);
        connect(admin, &UDriveAdminWidget::availabilityChanged, this, [this, admin, id = state->account()->uuid()] {
            if (!_lastAccount || _lastAccount->uuid() != id) return;
            if (_accountPopup) _accountPopup->close();
            Q_EMIT workspaceChanged();
            if (_currentPage == SettingsPage::Administration && !admin->api()->available()) setCurrentPage(SettingsPage::Files);
        });
        QTimer::singleShot(0, admin, [admin] { admin->api()->refreshAccess(); });
        auto *search = files->takeSearchField();
        search->setProperty("accountId", state->account()->uuid());
        _searchStack->addWidget(search);
        connect(files, &UDriveFileBrowser::creationAvailabilityChanged, this, &SettingsDialog::workspaceChanged);
        connect(state->account()->spacesManager(), &GraphApi::SpacesManager::updated, this, &SettingsDialog::workspaceChanged);
        connect(files, &UDriveFileBrowser::sectionChanged, this, [this, files](UDriveFileBrowser::Section section) {
            if (_ui->stack->currentWidget() != files)
                return;
            switch (section) {
            case UDriveFileBrowser::Section::Personal:
                _currentPage = SettingsPage::Files;
                break;
            case UDriveFileBrowser::Section::Workspaces:
                _currentPage = SettingsPage::Workspaces;
                break;
            case UDriveFileBrowser::Section::Shares:
                _currentPage = SettingsPage::Shares;
                break;
            case UDriveFileBrowser::Section::Favorites:
                _currentPage = SettingsPage::Favorites;
                break;
            case UDriveFileBrowser::Section::Trash:
                _currentPage = SettingsPage::Trash;
                break;
            }
            Q_EMIT currentPageChanged();
            Q_EMIT workspaceChanged();
        });
    }

    setCurrentAccount(state->account());
    if (!_filesForAccount.isEmpty())
        setCurrentPage(SettingsPage::Files);
}

void SettingsDialog::onAccountRemoved(AccountState *state)
{
    if (!state || !state->account())
        return;
    // todo: #37. using the account after we know it's been removed is not ok.
    Account *acc = state->account();
    if (auto *admin = _adminForAccount.take(acc->uuid())) {
        admin->api()->clear();
        auto *search = admin->searchField();
        if (_searchStack && _searchStack->indexOf(search) >= 0) { _searchStack->removeWidget(search); search->deleteLater(); }
        _ui->stack->removeWidget(admin); admin->deleteLater();
    }
    ConfigFile::settingsWithGroup(QStringLiteral("UDriveFileBrowserRecent/") + acc->uuid().toString(QUuid::WithoutBraces))->remove(QString());
    if (auto *files = _filesForAccount.take(acc->uuid())) {
        for (int i = 1; _searchStack && i < _searchStack->count(); ++i) {
            if (_searchStack->widget(i)->property("accountId").toUuid() == acc->uuid()) {
                auto *search = _searchStack->widget(i);
                _searchStack->removeWidget(search);
                delete search;
                break;
            }
        }
        _ui->stack->removeWidget(files);
        delete files;
    }
    if (_notificationPopup) {
        _notificationPopup->close();
        delete _notificationPopup.data();
    }

    if (_lastAccount == acc)
        _lastAccount.clear();

    if (AccountView *asw = _viewForAccount.value(acc->uuid(), nullptr)) {
        _viewForAccount.remove(acc->uuid());
        _ui->stack->removeWidget(asw);
        asw->deleteLater();
        //  go to the settings page if the last account was removed
        if (_viewForAccount.isEmpty()) {
            setCurrentPage(SettingsPage::Settings);
        } else if (_currentAccount == acc) {
            const auto accounts = AccountManager::instance()->accounts();
            for (auto *remaining : accounts) {
                if (remaining->account() != acc) {
                    _currentAccount = nullptr;
                    setCurrentAccount(remaining->account());
                    if (!_filesForAccount.isEmpty())
                        setCurrentPage(SettingsPage::Files);
                    break;
                }
            }
        }
    }
}

void SettingsDialog::addModalWidget(QWidget *w)
{
    ownCloudGui::raise();
    if (_ui->dialogStack->indexOf(w) == -1) {
        _ui->dialogStack->addWidget(w);
        _ui->dialogStack->setCurrentWidget(w);
    }
}

void SettingsDialog::requestModality(Account *account)
{
    if (!account)
        return;

    _ui->quickWidget->setEnabled(false);
    if (_modalStack.isEmpty()) {
        setCurrentAccount(account);
    }
    _modalStack.append(account);
    ownCloudGui::raise();
}

void SettingsDialog::ceaseModality(Account *account)
{
    if (account && _modalStack.contains(account)) {
        _modalStack.removeOne(account);
        if (!_modalStack.isEmpty()) {
            setCurrentAccount(_modalStack.first());
        }
    }
    _ui->quickWidget->setEnabled(_modalStack.isEmpty());
}

AccountView *SettingsDialog::accountView(Account *account) const
{
    return _viewForAccount.value(account->uuid(), nullptr);
}

void SettingsDialog::setVisible(bool visible)
{
    if (!visible) {
        ConfigFile cfg;
        cfg.saveGeometry(this);
    }

#ifdef Q_OS_MAC
    // Keep the UDrive workspace's Dock entry when its window is closed.
    // Switching back to Accessory makes every reopen look like a new launch.
    if (visible || Theme::instance()->appNameGUI() == QStringLiteral("UDrive")) {
        setActivationPolicy(ActivationPolicy::Regular);
    } else {
        setActivationPolicy(ActivationPolicy::Accessory);
    }
#endif
    QMainWindow::setVisible(visible);
}

void SettingsDialog::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange) {
        _ui->retranslateUi(this);
        setWindowTitle(tr("%1").arg(Theme::instance()->appNameGUI()));
        if (_ui->quickWidget && _ui->quickWidget->engine()) {
            _ui->quickWidget->engine()->retranslate();
        }
        updateWorkspaceHeader();
        updateNotificationBadge();
        for (auto *browser : _filesForAccount) {
            if (browser) browser->retranslate();
        }
        Q_EMIT workspaceChanged();
        Q_EMIT currentPageChanged();
    }
    QMainWindow::changeEvent(event);
}

void SettingsDialog::setCurrentPage(SettingsPage currentPage)
{
    if (currentPage == _currentPage && _currentPage == SettingsPage::Workspaces) {
        if (_lastAccount) {
            if (auto *files = _filesForAccount.value(_lastAccount->uuid())) {
                files->showWorkspaces();
            }
        }
        return;
    }
    _currentPage = currentPage;
    _currentAccount = nullptr;
    switch (_currentPage) {
    case SettingsPage::Administration: {
        auto *admin = _lastAccount ? _adminForAccount.value(_lastAccount->uuid()) : nullptr;
        if (!admin || !admin->api()->available()) {
            _currentPage = SettingsPage::Settings; _ui->stack->setCurrentWidget(_settingsPage); break;
        }
        _currentAccount = _lastAccount;
        _ui->stack->setCurrentWidget(admin);
        break;
    }
    case SettingsPage::Activity:
        _ui->stack->setCurrentWidget(_activityPage);
        break;
    case SettingsPage::Settings:
        _ui->stack->setCurrentWidget(_settingsPage);
        break;
    case SettingsPage::Account:
    case SettingsPage::Files:
    case SettingsPage::Workspaces:
    case SettingsPage::Shares:
    case SettingsPage::Favorites:
    case SettingsPage::Trash: {
        if (!_lastAccount || !_viewForAccount.contains(_lastAccount->uuid())) {
            _currentPage = SettingsPage::Settings;
            _ui->stack->setCurrentWidget(_settingsPage);
            break;
        }
        _currentAccount = _lastAccount;
        auto *files = _filesForAccount.value(_currentAccount->uuid());
        if (_currentPage == SettingsPage::Account || !files) {
            _currentPage = SettingsPage::Account;
            _ui->stack->setCurrentWidget(accountView(_currentAccount));
        } else {
            if (_currentPage == SettingsPage::Files) files->showPersonal();
            else if (_currentPage == SettingsPage::Workspaces) files->showWorkspaces();
            else if (_currentPage == SettingsPage::Shares) files->showShares();
            else if (_currentPage == SettingsPage::Favorites) files->showFavorites();
            else files->showTrash();
            _ui->stack->setCurrentWidget(files);
        }
        break;
    }
    case SettingsPage::None:
        Q_UNREACHABLE();
    }
    Q_EMIT currentAccountChanged();
    Q_EMIT currentPageChanged();
    Q_EMIT workspaceChanged();
}

SettingsDialog::SettingsPage SettingsDialog::currentPage() const
{
    return _currentPage;
}


void SettingsDialog::setCurrentAccount(Account *account)
{
    if (!account || (account == _currentAccount && _currentPage == SettingsPage::Account))
        return;

    _currentAccount = account;
    _lastAccount = account;

    _ui->stack->setCurrentWidget(accountView(account));
    _currentPage = SettingsPage::Account;

    Q_EMIT currentAccountChanged();
    Q_EMIT currentPageChanged();
}

Account *SettingsDialog::currentAccount() const
{
    return _currentAccount;
}

void SettingsDialog::createNewAccount()
{
    ocApp()->gui()->runAccountWizard();
}

void SettingsDialog::runFolderWizard(Account *account)
{
    if (!account)
        return;
    setCurrentAccount(account);
    accountView(_currentAccount)->slotAddFolder();
}

void SettingsDialog::toggleSidebar()
{
    setSidebarVisible(!isSidebarVisible());
}

bool SettingsDialog::isSidebarVisible() const
{
    return _sidebarVisible;
}

void SettingsDialog::setSidebarVisible(bool visible)
{
    if (_sidebarVisible == visible) {
        return;
    }
    _sidebarVisible = visible;
    if (_ui->quickWidget) {
        _ui->quickWidget->setFixedWidth(visible ? 236 : 56);
        _ui->quickWidget->setVisible(true);
    }
    if (_brandWidget) {
        _brandWidget->setFixedWidth(visible ? 224 : 140);
    }
    auto settings = ConfigFile::makeQSettings();
    settings.setValue(QStringLiteral("UDrive/sidebarVisible"), visible);
    Q_EMIT sidebarVisibilityChanged(visible);
}

} // namespace OCC

#include "settingsdialog.moc"
