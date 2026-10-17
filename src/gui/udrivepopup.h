// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QMenu>
#include <QActionEvent>
#include <QMoveEvent>
#include <QResizeEvent>
#include <QScreen>
#include <QScopedValueRollback>
#include <QShowEvent>
#include <QStyleOptionMenuItem>
#include <QTimer>

namespace OCC {
inline QRect udrivePopupBounds(const QWidget *owner)
{
    const auto *window = owner->window();
    const QRect client(window->mapToGlobal(QPoint()), window->size());
    const QRect visible = client.intersected(owner->screen()->availableGeometry());
    return visible.width() > 8 && visible.height() > 8 ? visible.adjusted(4, 4, -4, -4) : client;
}

inline void containUDrivePopup(QWidget *popup, const QWidget *owner)
{
    const QRect bounds = udrivePopupBounds(owner);
    const QSize size = popup->size().boundedTo(bounds.size());
    if (popup->size() != size) popup->resize(size);
    const QPoint position(qBound(bounds.left(), popup->x(), bounds.right() - popup->width() + 1),
        qBound(bounds.top(), popup->y(), bounds.bottom() - popup->height() + 1));
    if (popup->pos() != position) popup->move(position);
}

// Keep Qt's action handling, keyboard navigation and dismissal while limiting
// the menu to its owning app window instead of the entire desktop.
class UDriveMenu : public QMenu
{
public:
    explicit UDriveMenu(QWidget *parent) : QMenu(parent)
    {
        setWindowFlag(Qt::FramelessWindowHint);
        setAttribute(Qt::WA_TranslucentBackground);
        setStyleSheet(QStringLiteral("QMenu { menu-scrollable: 1; }"));
        connect(this, &QMenu::aboutToShow, this, [this] {
            setMaximumSize(udrivePopupBounds(parentWidget()).size());
        });
    }

protected:
    void actionEvent(QActionEvent *event) override
    {
        auto *action = event->action();
        if (event->type() != QEvent::ActionRemoved && !action->isSeparator()) {
            if (!action->isIconVisibleInMenu()) action->setIconVisibleInMenu(true);
            if (action->isShortcutVisibleInContextMenu()) action->setShortcutVisibleInContextMenu(false);
        }
        QMenu::actionEvent(event);
    }
    void initStyleOption(QStyleOptionMenuItem *option, const QAction *action) const override
    {
        QMenu::initStyleOption(option, action);
        option->text = option->text.section(QLatin1Char('\t'), 0, 0);
        option->reservedShortcutWidth = 0;
    }
    void showEvent(QShowEvent *event) override
    {
        QMenu::showEvent(event);
        contain();
        // Cocoa may finish positioning its popup after showEvent returns.
        QTimer::singleShot(0, this, [this] { contain(); });
    }
    void moveEvent(QMoveEvent *event) override
    {
        QMenu::moveEvent(event);
        contain();
    }
    void resizeEvent(QResizeEvent *event) override
    {
        QMenu::resizeEvent(event);
        contain();
    }

private:
    void contain()
    {
        if (_positioning || !isVisible()) return;
        const QScopedValueRollback<bool> guard(_positioning, true);
        containUDrivePopup(this, parentWidget());
    }
    bool _positioning = false;
};
}
