/*
 * Copyright (C) by Hannah von Reth <hannah.vonreth@owncloud.com>
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

#include "activityitemdelegate.h"
#include "models/protocolitemmodel.h"
#include "models/models.h"
#include "udriveicons.h"
#include "libsync/configfile.h"
#include "syncfileitem.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QMouseEvent>
#include <QPainter>

namespace OCC {

ActivityItemDelegate::ActivityItemDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
    if (auto *view = qobject_cast<QAbstractItemView *>(parent)) {
        if (view->viewport()) {
            view->viewport()->installEventFilter(this);
            view->setMouseTracking(true);
        }
    }
}

bool ActivityItemDelegate::eventFilter(QObject *watched, QEvent *event)
{
    auto *view = qobject_cast<QAbstractItemView *>(parent());
    if (view && watched == view->viewport()) {
        if (event->type() == QEvent::MouseMove) {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            const QModelIndex idx = view->indexAt(mouseEvent->position().toPoint());
            const QPersistentModelIndex rowIdx = idx.isValid() ? QPersistentModelIndex(idx.siblingAtColumn(0)) : QPersistentModelIndex();
            if (rowIdx != _hoveredIndex) {
                _hoveredIndex = rowIdx;
                view->viewport()->update();
            }
        } else if (event->type() == QEvent::Leave) {
            if (_hoveredIndex.isValid()) {
                _hoveredIndex = QPersistentModelIndex();
                view->viewport()->update();
            }
        }
    }
    return QStyledItemDelegate::eventFilter(watched, event);
}

QSize ActivityItemDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    auto size = QStyledItemDelegate::sizeHint(option, index);
    size.setHeight(qMax(40, size.height() + 14));
    return size;
}

void ActivityItemDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setRenderHint(QPainter::SmoothPixmapTransform);

    const auto themeMode = ConfigFile().themeMode();
    const bool dark = themeMode == ConfigFile::ThemeMode::Dark
        || (themeMode == ConfigFile::ThemeMode::System && qApp->palette().color(QPalette::Window).lightness() < 128);

    const bool isSelected = (option.state & QStyle::State_Selected);
    const bool isHovered = (_hoveredIndex.isValid() && _hoveredIndex.row() == index.row());
    const bool isEven = (index.row() % 2 == 0);

    // 1. Draw theme-aware row background
    QColor rowBg;
    if (isSelected) {
        rowBg = dark ? QColor("#1D3E27") : QColor("#E0F5E8");
    } else if (isHovered) {
        rowBg = dark ? QColor("#323E35") : QColor("#EAF8EF");
    } else {
        if (dark) {
            rowBg = isEven ? QColor("#292929") : QColor("#242424");
        } else {
            rowBg = isEven ? QColor("#FFFFFF") : QColor("#F9FAFB");
        }
    }

    painter->fillRect(option.rect, rowBg);

    // Subtle bottom row divider
    painter->setPen(QColor(dark ? "#383838" : "#E5E7EB"));
    painter->drawLine(option.rect.bottomLeft(), option.rect.bottomRight());

    // 2. Content per column
    const auto column = static_cast<ProtocolItemModel::ProtocolItemRole>(index.column());
    const QRect rect = option.rect.adjusted(12, 0, -12, 0);

    const QColor textPrimary(dark ? "#EDEEF0" : "#2B3035");
    const QColor textSecondary(dark ? "#939D97" : "#7D8781");

    // Retrieve underlying status for the row
    const auto statusIdx = index.siblingAtColumn(static_cast<int>(ProtocolItemModel::ProtocolItemRole::Status));
    const auto statusVal = statusIdx.data(Models::UnderlyingDataRole).toInt();
    const auto status = static_cast<SyncFileItem::Status>(statusVal);

    if (column == ProtocolItemModel::ProtocolItemRole::Status) {
        // Draw Status Pill Badge
        const QString statusText = index.data(Qt::DisplayRole).toString();
        if (!statusText.isEmpty()) {
            QColor badgeBg, badgeBorder, badgeText;
            if (status == SyncFileItem::Success || status == SyncFileItem::NoStatus) {
                // Emerald
                badgeBg = dark ? QColor("#143B20") : QColor("#DCFCE7");
                badgeBorder = dark ? QColor("#2D6A42") : QColor("#86EFAC");
                badgeText = dark ? QColor("#63E493") : QColor("#166534");
            } else if (status == SyncFileItem::NormalError || status == SyncFileItem::FatalError
                || status == SyncFileItem::DetailError || status == SyncFileItem::BlacklistedError
                || status == SyncFileItem::Conflict) {
                // Red
                badgeBg = dark ? QColor("#451212") : QColor("#FEE2E2");
                badgeBorder = dark ? QColor("#7F1D1D") : QColor("#FCA5A5");
                badgeText = dark ? QColor("#FCA5A5") : QColor("#991B1B");
            } else {
                // Amber / Blue (Warning / Excluded / Ignored)
                badgeBg = dark ? QColor("#452A05") : QColor("#FEF3C7");
                badgeBorder = dark ? QColor("#784D12") : QColor("#FCD34D");
                badgeText = dark ? QColor("#FCD34D") : QColor("#92400E");
            }

            QFont badgeFont = option.font;
            badgeFont.setPixelSize(11);
            badgeFont.setBold(true);
            painter->setFont(badgeFont);

            const QFontMetrics bfm(badgeFont);
            const int textW = bfm.horizontalAdvance(statusText);
            const int badgeH = 22;
            const int badgeW = textW + 16;
            const int badgeY = option.rect.center().y() - badgeH / 2;
            const QRect badgeRect(rect.left(), badgeY, badgeW, badgeH);

            painter->setBrush(badgeBg);
            painter->setPen(QPen(badgeBorder, 1));
            painter->drawRoundedRect(QRectF(badgeRect).adjusted(0.5, 0.5, -0.5, -0.5), badgeH / 2.0, badgeH / 2.0);

            painter->setPen(badgeText);
            painter->drawText(badgeRect, Qt::AlignCenter, statusText);
        }
    } else if (column == ProtocolItemModel::ProtocolItemRole::File) {
        // Draw File Icon + File Name
        const QString filePath = index.data(Qt::DisplayRole).toString();
        const QIcon fileIcon = UDriveIcons::fileIcon(filePath, false);
        const int iconSize = 20;
        const int iconY = option.rect.center().y() - iconSize / 2;
        const QRect iconRect(rect.left(), iconY, iconSize, iconSize);
        fileIcon.paint(painter, iconRect);

        QFont fileFont = option.font;
        fileFont.setPixelSize(13);
        fileFont.setWeight(QFont::Medium);
        painter->setFont(fileFont);
        painter->setPen(textPrimary);

        const QRect textRect(rect.left() + iconSize + 10, rect.top(), rect.width() - iconSize - 10, rect.height());
        const QString elided = painter->fontMetrics().elidedText(filePath, Qt::ElideMiddle, textRect.width());
        painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, elided);
    } else if (column == ProtocolItemModel::ProtocolItemRole::Action) {
        // Draw Action message with action icon
        const QString message = index.data(Qt::DisplayRole).toString();
        const QIcon actionIcon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));

        int leftX = rect.left();
        if (!actionIcon.isNull()) {
            const int iconSize = 16;
            const int iconY = option.rect.center().y() - iconSize / 2;
            actionIcon.paint(painter, QRect(leftX, iconY, iconSize, iconSize));
            leftX += iconSize + 8;
        }

        QFont actionFont = option.font;
        actionFont.setPixelSize(13);
        painter->setFont(actionFont);
        painter->setPen(textPrimary);

        const QRect textRect(leftX, rect.top(), qMax(0, rect.right() - leftX), rect.height());
        const QString elided = painter->fontMetrics().elidedText(message, Qt::ElideRight, textRect.width());
        painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, elided);
    } else {
        // Time, Size, Folder, Account
        const QString str = index.data(Qt::DisplayRole).toString();
        QFont cellFont = option.font;
        cellFont.setPixelSize(13);
        painter->setFont(cellFont);
        painter->setPen(column == ProtocolItemModel::ProtocolItemRole::Account ? textPrimary : textSecondary);

        const Qt::Alignment align = (column == ProtocolItemModel::ProtocolItemRole::Size)
            ? (Qt::AlignVCenter | Qt::AlignRight)
            : (Qt::AlignVCenter | Qt::AlignLeft);
        const QString elided = painter->fontMetrics().elidedText(str, Qt::ElideRight, rect.width());
        painter->drawText(rect, align, elided);
    }

    painter->restore();
}

} // namespace OCC
