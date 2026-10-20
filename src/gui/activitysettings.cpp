/*
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
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

#include "activitysettings.h"
#include "localactivitywidget.h"
#include "resources.h"
#include "syncerrorwidget.h"
#include "udriveicons.h"
#include "libsync/configfile.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QTabBar>
#include <QTabWidget>

namespace OCC {

class ActivityTabBar : public QTabBar
{
    Q_OBJECT
public:
    explicit ActivityTabBar(QWidget *parent = nullptr)
        : QTabBar(parent)
    {
        setDrawBase(false);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setMouseTracking(true);
        setAttribute(Qt::WA_Hover);
    }

    void setIssueCount(int count)
    {
        if (_issueCount != count) {
            _issueCount = count;
            updateGeometry();
            update();
        }
    }

    int issueCount() const { return _issueCount; }

    QSize tabSizeHint(int index) const override
    {
        const QString text = tabLabel(index);
        QFont f = font();
        f.setPixelSize(13);
        f.setWeight(currentIndex() == index ? QFont::DemiBold : QFont::Medium);
        const QFontMetrics fm(f);
        int textWidth = fm.horizontalAdvance(text);

        int iconWidth = tabIcon(index).isNull() ? 0 : 26;
        int badgeWidth = 0;
        if (index == 1 && _issueCount > 0) {
            const QString badgeStr = _issueCount > 99 ? QStringLiteral("99+") : QString::number(_issueCount);
            QFont badgeFont = font();
            badgeFont.setPixelSize(10);
            badgeFont.setBold(true);
            const QFontMetrics bfm(badgeFont);
            badgeWidth = qMax(18, bfm.horizontalAdvance(badgeStr) + 12) + 8;
        }
        return QSize(textWidth + iconWidth + badgeWidth + 28, 40);
    }

protected:
    void mouseMoveEvent(QMouseEvent *event) override
    {
        QTabBar::mouseMoveEvent(event);
        update();
    }

    void leaveEvent(QEvent *event) override
    {
        QTabBar::leaveEvent(event);
        update();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);

        const auto themeMode = ConfigFile().themeMode();
        const bool dark = themeMode == ConfigFile::ThemeMode::Dark
            || (themeMode == ConfigFile::ThemeMode::System && qApp->palette().color(QPalette::Window).lightness() < 128);

        const QColor textColor(dark ? "#EDEEF0" : "#2B3035");
        const QColor mutedTextColor(dark ? "#939D97" : "#7D8781");
        const QColor hoverColor(dark ? "#323E35" : "#EAF8EF");
        const QColor accentGreen("#40D979");

        const QPoint mousePos = mapFromGlobal(QCursor::pos());
        const int hoveredTab = underMouse() ? tabAt(mousePos) : -1;

        for (int i = 0; i < count(); ++i) {
            const QRect rect = tabRect(i);
            const bool isSelected = (i == currentIndex());
            const bool isHovered = (i == hoveredTab);

            // Hover background
            if (isHovered && !isSelected) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(hoverColor);
                painter.drawRoundedRect(rect.adjusted(2, 4, -2, -6), 6, 6);
            }

            // Calculate content placement
            QFont tabFont = font();
            tabFont.setPixelSize(13);
            tabFont.setWeight(isSelected ? QFont::DemiBold : QFont::Medium);
            painter.setFont(tabFont);
            const QFontMetrics fm(tabFont);

            const QString text = tabLabel(i);
            const int textWidth = fm.horizontalAdvance(text);
            const QIcon icon = tabIcon(i);
            const int iconSize = 18;
            const int iconGap = icon.isNull() ? 0 : 8;

            int badgeWidth = 0;
            QString badgeStr;
            if (i == 1 && _issueCount > 0) {
                badgeStr = _issueCount > 99 ? QStringLiteral("99+") : QString::number(_issueCount);
                QFont badgeFont = font();
                badgeFont.setPixelSize(10);
                badgeFont.setBold(true);
                const QFontMetrics bfm(badgeFont);
                badgeWidth = qMax(18, bfm.horizontalAdvance(badgeStr) + 12);
            }
            const int badgeGap = badgeWidth > 0 ? 8 : 0;

            const int totalContentWidth = (icon.isNull() ? 0 : iconSize + iconGap) + textWidth + badgeGap + badgeWidth;
            int startX = rect.left() + (rect.width() - totalContentWidth) / 2;
            const int centerY = rect.center().y() - 1;

            // Draw Icon
            if (!icon.isNull()) {
                const QRect iconRect(startX, centerY - iconSize / 2, iconSize, iconSize);
                icon.paint(&painter, iconRect);
                startX += iconSize + iconGap;
            }

            // Draw Text
            painter.setPen(isSelected ? textColor : mutedTextColor);
            const QRect textRect(startX, rect.top(), textWidth, rect.height() - 4);
            painter.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, text);
            startX += textWidth;

            // Draw Badge
            if (badgeWidth > 0) {
                startX += badgeGap;
                const int badgeHeight = 18;
                const QRect badgeRect(startX, centerY - badgeHeight / 2, badgeWidth, badgeHeight);

                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor("#FF4B55"));
                painter.drawRoundedRect(QRectF(badgeRect), badgeHeight / 2.0, badgeHeight / 2.0);

                QFont badgeFont = font();
                badgeFont.setPixelSize(10);
                badgeFont.setBold(true);
                painter.setFont(badgeFont);
                painter.setPen(Qt::white);
                painter.drawText(badgeRect, Qt::AlignCenter, badgeStr);
            }

            // Draw Emerald Underline Indicator for selected tab
            if (isSelected) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(accentGreen);
                const int indicatorWidth = qMax(44, totalContentWidth + 12);
                const int indicatorLeft = rect.left() + (rect.width() - indicatorWidth) / 2;
                painter.drawRoundedRect(QRectF(indicatorLeft, rect.bottom() - 3, indicatorWidth, 3), 1.5, 1.5);
            }
        }
    }

private:
    QString tabLabel(int index) const
    {
        const auto label = tabData(index);
        return label.isValid() ? label.toString() : tabText(index);
    }

    int _issueCount = 0;
};

class ActivityTabWidget : public QTabWidget
{
    Q_OBJECT
public:
    explicit ActivityTabWidget(QWidget *parent = nullptr)
        : QTabWidget(parent)
        , _tabBar(new ActivityTabBar(this))
    {
        setTabBar(_tabBar);
        setObjectName(QStringLiteral("activityTabWidget"));
        _tabBar->setObjectName(QStringLiteral("activityTabBar"));
        setDocumentMode(true);
    }

    ActivityTabBar *activityTabBar() const { return _tabBar; }

private:
    ActivityTabBar *_tabBar;
};

ActivitySettings::ActivitySettings(QWidget *parent)
    : QWidget(parent)
{
    auto *vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    // create modern tab widget for the activity views
    _tab = new ActivityTabWidget(this);
    vbox->addWidget(_tab);
    setLayout(vbox);

    auto _localActivityWidget = new LocalActivityWidget(this);
    auto localActivityTabId = _tab->addTab(_localActivityWidget, UDriveIcons::icon(UDriveIcons::Symbol::Activity), tr("Local Activity"));

    auto _syncErrorWidget = new SyncErrorWidget(this);
    _syncErrorTabId = _tab->addTab(_syncErrorWidget, UDriveIcons::icon(UDriveIcons::Symbol::Online), tr("Not Synced"));
    // Paint the translated base label; retain the full tab text for accessibility.
    _tab->activityTabBar()->setTabData(_syncErrorTabId, tr("Not Synced"));
    slotShowIssueItemCount(0); // to display the label.
    connect(_syncErrorWidget, &SyncErrorWidget::issueCountUpdated, this, &ActivitySettings::slotShowIssueItemCount);

    // We want the local activity tab to be the default
    _tab->setCurrentIndex(localActivityTabId);
}

ActivitySettings::~ActivitySettings()
{
}

void ActivitySettings::slotShowIssueItemCount(const int cnt)
{
    _tab->activityTabBar()->setIssueCount(cnt);
    if (cnt > 0) {
        _tab->setTabIcon(_syncErrorTabId, Resources::getCoreIcon(QStringLiteral("states/warning")));
    } else {
        _tab->setTabIcon(_syncErrorTabId, UDriveIcons::icon(UDriveIcons::Symbol::Online));
    }
    QString cntText = tr("Not Synced");
    if (cnt) {
        //: %1 is the number of not synced files.
        cntText = tr("Not Synced (%1)").arg(cnt);
    }
    _tab->setTabText(_syncErrorTabId, cntText);
}

} // OCC namespace

#include "activitysettings.moc"
