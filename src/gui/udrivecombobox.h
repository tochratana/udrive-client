// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "udrivepopup.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QFrame>
#include <QLayout>
#include <QScreen>
#include <QTimer>

namespace OCC {
class UDriveComboBox : public QComboBox
{
public:
    using QComboBox::QComboBox;

    void showPopup() override
    {
        const bool customPopup = property("udriveCombo").toBool();
        if (customPopup) {
            auto *popup = view()->window();
            popup->setWindowFlag(Qt::FramelessWindowHint);
            const bool opaque = property("udriveOpaquePopup").toBool();
            if (opaque) {
                popup->setPalette(view()->palette());
                popup->setAutoFillBackground(true);
                popup->setObjectName(QStringLiteral("udriveOpaqueComboPopup"));
                popup->setStyleSheet(QStringLiteral("#udriveOpaqueComboPopup { background: %1; border: none; }")
                    .arg(view()->palette().color(QPalette::Base).name()));
            } else if (popup->objectName() == QLatin1String("udriveOpaqueComboPopup")) {
                popup->setObjectName(QString());
                popup->setStyleSheet(QString());
            }
            popup->setAttribute(Qt::WA_TranslucentBackground, !opaque);
        }
        QComboBox::showPopup();
        if (!customPopup)
            return;
        if (auto *frame = qobject_cast<QFrame *>(view()->window()))
            frame->setFrameShape(QFrame::NoFrame);
        positionPopup();
        // Cocoa may finish its popup animation after showPopup returns.
        QTimer::singleShot(0, this, [this] { positionPopup(); });
    }

private:
    void positionPopup()
    {
        auto *popup = view()->window();
        if (!popup->isVisible())
            return;
        const QRect available = udrivePopupBounds(this);
        int contentHeight = 0;
        if (view()->model()) {
            const int count = qMin(maxVisibleItems(), view()->model()->rowCount());
            for (int i = 0; i < count; ++i) {
                const int h = view()->sizeHintForRow(i);
                contentHeight += (h > 0 ? h : 46);
            }
            if (contentHeight > 0) {
                const QMargins pm = popup->layout() ? popup->layout()->contentsMargins() : QMargins();
                const QMargins vm = view()->contentsMargins();
                contentHeight += pm.top() + pm.bottom() + vm.top() + vm.bottom() + 10;
            }
        }
        if (contentHeight <= 0)
            contentHeight = view()->sizeHint().height();

        const int desiredWidth = qMax(width(), qMax(view()->minimumWidth(), 236));
        const int targetWidth = qMin(desiredWidth, available.width());
        const int targetHeight = qMin(contentHeight, available.height());
        popup->setFixedSize(targetWidth, targetHeight);
        const QPoint below = mapToGlobal(QPoint(0, height() + 4));
        const int left = layoutDirection() == Qt::RightToLeft
            ? mapToGlobal(QPoint(width(), 0)).x() - popup->width() : below.x();
        int top = below.y();
        if (top + popup->height() > available.bottom() + 1)
            top = mapToGlobal(QPoint(0, -4)).y() - popup->height();
        popup->move(qBound(available.left(), left, available.right() - popup->width() + 1),
            qBound(available.top(), top, available.bottom() - popup->height() + 1));
    }
};
}
