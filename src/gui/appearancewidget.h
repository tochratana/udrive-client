// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "libsync/configfile.h"
#include <QFrame>
#include <QLabel>
#include <QMap>
#include <QWidget>

namespace OCC {

class ThemePreviewWidget;
class ThemeBadgeIconWidget;

class ThemeCardWidget : public QFrame
{
    Q_OBJECT
public:
    explicit ThemeCardWidget(ConfigFile::ThemeMode mode, const QString &title, const QString &subtitle, QWidget *parent = nullptr);

    ConfigFile::ThemeMode mode() const { return _mode; }
    bool isSelected() const { return _selected; }
    void setSelected(bool selected);
    void updateSystemState(bool isDark);
    void updateCardTheme();
    void retranslate(const QString &title, const QString &subtitle);

Q_SIGNALS:
    void clicked();

protected:
    void mousePressEvent(QMouseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void enterEvent(QEnterEvent *e) override;
    void leaveEvent(QEvent *e) override;
    void paintEvent(QPaintEvent *e) override;

private:
    void updateLabels(bool isDark);

    ConfigFile::ThemeMode _mode;
    QString _titleText;
    bool _selected = false;
    bool _hovered = false;
    bool _systemIsDark = false;
    ThemePreviewWidget *_preview = nullptr;
    QLabel *_titleLabel = nullptr;
    QLabel *_subtitleLabel = nullptr;
};

class AppearanceWidget : public QWidget
{
    Q_OBJECT
public:
    explicit AppearanceWidget(QWidget *parent = nullptr);

    ConfigFile::ThemeMode currentMode() const { return _currentMode; }
    void setThemeMode(ConfigFile::ThemeMode mode, bool emitSignal = false);
    void updateSystemAppearance();
    void updateThemeStyles();
    void retranslate();

Q_SIGNALS:
    void themeChanged(ConfigFile::ThemeMode mode);

protected:
    void changeEvent(QEvent *event) override;

private:
    ConfigFile::ThemeMode _currentMode = ConfigFile::ThemeMode::System;
    QMap<ConfigFile::ThemeMode, ThemeCardWidget *> _cards;
    QLabel *_header = nullptr;
    QLabel *_subtitle = nullptr;
};

} // namespace OCC
