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
#pragma once

#include "owncloudguilib.h"

#include <QDialog>

class QLabel;
class QPushButton;

namespace OCC {

namespace Ui {
    class AboutDialog;
}

class OWNCLOUDGUI_EXPORT AboutDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AboutDialog(QWidget *parent = nullptr);
    ~AboutDialog() override;

private:
    void openBrowser(const QString &s);
    void openBrowserFromUrl(const QUrl &s);
    void setupUpdaterWidget();

private:
    Ui::AboutDialog *ui = nullptr;
    QWidget *_updaterWidget = nullptr;
    QLabel *_updateStateLabel = nullptr;
    QPushButton *_restartButton = nullptr;
};

} // namespace OCC
