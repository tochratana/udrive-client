// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QAbstractItemView>
#include <QComboBox>
#include <QScreen>
#include <QTimer>

namespace OCC {
class UDriveComboBox : public QComboBox
{
public:
    using QComboBox::QComboBox;

    void showPopup() override
    {
        QComboBox::showPopup();
        if (!property("udriveCombo").toBool())
            return;
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
        const QRect available = screen()->availableGeometry().adjusted(4, 4, -4, -4);
        popup->resize(qMin(width(), available.width()), qMin(popup->height(), available.height()));
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
