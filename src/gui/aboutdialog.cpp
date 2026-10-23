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
#include "aboutdialog.h"
#include "ui_aboutdialog.h"

#include "gui/guiutility.h"
#include "libsync/theme.h"
#include "common/version.h"
#include "common/vfs.h"

#include <QClipboard>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QSslSocket>
#include <QSysInfo>
#include <QTextBrowser>
#include <QTimer>
#include <QVBoxLayout>

#ifdef WITH_AUTO_UPDATER
#include "config/appconfig.h"
#include "libsync/configfile.h"
#include "updater/ocupdater.h"
#ifdef Q_OS_MAC
// FIXME We should unify those, but Sparkle does everything behind the scene transparently
#include "updater/sparkleupdater.h"
#endif
#endif

namespace {
#ifdef WITH_AUTO_UPDATER
bool isTestPilotCloudTheme()
{
    return OCC::Theme::instance()->appName() == QLatin1String("testpilotcloud");
}
#endif
}

namespace OCC {

AboutDialog::AboutDialog(QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("OCC__AboutDialog"));
    setWindowTitle(tr("About"));

    const bool dark = palette().color(QPalette::Window).lightness() < 128;
    const QString cardBg = dark ? QStringLiteral("#212429") : QStringLiteral("#FFFFFF");
    const QString subCardBg = dark ? QStringLiteral("rgba(255, 255, 255, 0.04)") : QStringLiteral("#F8FAFC");
    const QString borderCol = dark ? QStringLiteral("rgba(255, 255, 255, 0.08)") : QStringLiteral("#E2E8F0");
    const QString textPri = dark ? QStringLiteral("#FFFFFF") : QStringLiteral("#0F172A");
    const QString textSec = dark ? QStringLiteral("#94A3B8") : QStringLiteral("#64748B");
    const QString textMuted = dark ? QStringLiteral("#64748B") : QStringLiteral("#94A3B8");

    // Clean integrated layout directly on the dialog - no artificial stretches or floating island box
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(36, 28, 36, 24);
    mainLayout->setSpacing(16);

    auto *card = new QFrame(this);
    card->setObjectName(QStringLiteral("aboutCard"));
    card->setFrameShape(QFrame::NoFrame);
    card->setStyleSheet(QStringLiteral("QFrame#aboutCard { background: transparent; border: none; }"));
    mainLayout->addWidget(card, 1);

    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(16);

    // 1. Header (Name + Version Badge + SHA Badge + Tagline + Online Button) - NO ICON
    auto *headerCol = new QVBoxLayout;
    headerCol->setSpacing(8);

    auto *titleRow = new QHBoxLayout;
    titleRow->setSpacing(10);

    auto *appNameLabel = new QLabel(Theme::instance()->appNameGUI(), card);
    appNameLabel->setObjectName(QStringLiteral("aboutAppName"));
    appNameLabel->setStyleSheet(QStringLiteral(
        "font-size: 26px; font-weight: 700; color: %1;"
    ).arg(textPri));
    titleRow->addWidget(appNameLabel);

    const QString versionStr = QStringLiteral("%1").arg(Version::displayString());
    auto *versionBadge = new QLabel(versionStr, card);
    versionBadge->setObjectName(QStringLiteral("aboutVersionBadge"));
    versionBadge->setStyleSheet(QStringLiteral(
        "color: %1;"
        "font-size: 15px;"
        "font-weight: 500;"
        "background: transparent;"
        "border: none;"
        "padding: 6px 0 0 2px;"
    ).arg(textSec));
    titleRow->addWidget(versionBadge);
    titleRow->addStretch(1);

    auto *onlineBtn = new QPushButton(tr("Open UDrive online"), card);
    onlineBtn->setObjectName(QStringLiteral("aboutOnlineBtn"));
    onlineBtn->setCursor(Qt::PointingHandCursor);
    onlineBtn->setStyleSheet(QStringLiteral(
        "QPushButton#aboutOnlineBtn {"
        "    background: rgba(34, 197, 94, 0.10);"
        "    color: #22C55E;"
        "    border: 1px solid rgba(34, 197, 94, 0.25);"
        "    border-radius: 6px;"
        "    padding: 5px 14px;"
        "    font-size: 12px;"
        "    font-weight: 600;"
        "}"
        "QPushButton#aboutOnlineBtn:hover {"
        "    background: rgba(34, 197, 94, 0.20);"
        "    border-color: rgba(34, 197, 94, 0.40);"
        "}"
    ));
    connect(onlineBtn, &QPushButton::clicked, this, [this] {
        openBrowser(QStringLiteral("https://drive.unity-workspace.com"));
    });
    titleRow->addWidget(onlineBtn);

    headerCol->addLayout(titleRow);

    auto *taglineLabel = new QLabel(tr("UDrive brings your Unity Workspace files to your desktop."), card);
    taglineLabel->setObjectName(QStringLiteral("aboutTagline"));
    taglineLabel->setStyleSheet(QStringLiteral(
        "font-size: 13px; color: %1;"
    ).arg(textSec));
    headerCol->addWidget(taglineLabel);

    cardLayout->addLayout(headerCol);

    // 2. Segmented Pill Tab Bar (About vs Versions)
    auto *tabContainer = new QFrame(card);
    tabContainer->setObjectName(QStringLiteral("aboutTabContainer"));
    tabContainer->setFixedWidth(260);
    tabContainer->setStyleSheet(QStringLiteral(
        "QFrame#aboutTabContainer {"
        "    background: %1;"
        "    border: 1px solid %2;"
        "    border-radius: 8px;"
        "    padding: 2px;"
        "}"
    ).arg(dark ? QStringLiteral("#16181B") : QStringLiteral("#F1F5F9"), borderCol));

    auto *tabBarLayout = new QHBoxLayout(tabContainer);
    tabBarLayout->setContentsMargins(2, 2, 2, 2);
    tabBarLayout->setSpacing(4);

    auto *tabAboutBtn = new QPushButton(tr("About"), tabContainer);
    tabAboutBtn->setObjectName(QStringLiteral("tabBtnAbout"));
    tabAboutBtn->setCursor(Qt::PointingHandCursor);

    auto *tabDiagBtn = new QPushButton(tr("Versions"), tabContainer);
    tabDiagBtn->setObjectName(QStringLiteral("tabBtnDiag"));
    tabDiagBtn->setCursor(Qt::PointingHandCursor);

    tabBarLayout->addWidget(tabAboutBtn, 1);
    tabBarLayout->addWidget(tabDiagBtn, 1);

    auto *tabRow = new QHBoxLayout;
    tabRow->addWidget(tabContainer);
    tabRow->addStretch(1);
    cardLayout->addLayout(tabRow);

    // 3. Stacked Pages
    auto *pages = new QStackedWidget(card);
    pages->setObjectName(QStringLiteral("aboutPages"));

    // --- Page 0: About ---
    auto *aboutPage = new QWidget(pages);
    auto *aboutPageLayout = new QVBoxLayout(aboutPage);
    aboutPageLayout->setContentsMargins(0, 0, 0, 0);
    aboutPageLayout->setSpacing(16);

    // Normal text with dots (NO BOXES)
    auto *aboutText = new QLabel(aboutPage);
    aboutText->setObjectName(QStringLiteral("aboutText"));
    aboutText->setStyleSheet(QStringLiteral("font-size: 13px;"));
    aboutText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    aboutText->setText(QStringLiteral(
        "<p style='margin: 6px 0;'><span style='color: %1; font-weight: bold;'>•</span> <b style='color: %2;'>Core Engine:</b> <span style='color: %3;'>Built on the ownCloud Desktop Client.</span></p>"
        "<p style='margin: 6px 0;'><span style='color: %1; font-weight: bold;'>•</span> <b style='color: %2;'>Copyright:</b> <span style='color: %3;'>Unity Workspace and contributors.</span></p>"
        "<p style='margin: 6px 0;'><span style='color: %1; font-weight: bold;'>•</span> <b style='color: %2;'>License:</b> <span style='color: %3;'>Distributed under the GNU General Public License (GPL), version 2 or later.</span></p>"
    ).arg(dark ? QStringLiteral("#00856B") : QStringLiteral("#00856B"), textPri, textSec));
    aboutPageLayout->addWidget(aboutText);

    // Quick links row with dots (NO BOXES)
    auto *linksRow = new QHBoxLayout;
    linksRow->setSpacing(8);

    auto makeTextLink = [=](const QString &text, const QString &url) {
        auto *btn = new QPushButton(text, aboutPage);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(QStringLiteral(
            "QPushButton {"
            "    background: transparent;"
            "    color: #22C55E;"
            "    border: none;"
            "    padding: 4px 2px;"
            "    font-size: 13px;"
            "    font-weight: 500;"
            "}"
            "QPushButton:hover {"
            "    text-decoration: underline;"
            "}"
        ));
        connect(btn, &QPushButton::clicked, this, [this, url] { openBrowser(url); });
        return btn;
    };

    auto makeDot = [=]() {
        auto *dot = new QLabel(QStringLiteral("•"), aboutPage);
        dot->setStyleSheet(QStringLiteral("color: %1; font-size: 13px; font-weight: bold;").arg(textMuted));
        return dot;
    };

    linksRow->addWidget(makeTextLink(tr("Unity Workspace"), QStringLiteral("https://unity-workspace.com")));
    linksRow->addWidget(makeDot());
    linksRow->addWidget(makeTextLink(tr("Community & Help"), QStringLiteral("https://central.owncloud.com/c/desktop-client")));
    linksRow->addWidget(makeDot());
    linksRow->addWidget(makeTextLink(tr("License Terms"), QStringLiteral("https://www.gnu.org/licenses/old-licenses/gpl-2.0.html")));
    linksRow->addStretch(1);
    aboutPageLayout->addLayout(linksRow);

    // Updater widget
    _updaterWidget = new QWidget(aboutPage);
    auto *updaterLayout = new QHBoxLayout(_updaterWidget);
    updaterLayout->setContentsMargins(0, 4, 0, 0);
    updaterLayout->setSpacing(10);

    _updateStateLabel = new QLabel(_updaterWidget);
    _updateStateLabel->setStyleSheet(QStringLiteral("font-size: 12px; color: %1;").arg(textSec));
    updaterLayout->addWidget(_updateStateLabel, 1);

    _restartButton = new QPushButton(tr("Restart & Update"), _updaterWidget);
    _restartButton->setCursor(Qt::PointingHandCursor);
    _restartButton->setStyleSheet(QStringLiteral(
        "QPushButton { background: #00856B; color: white; border: none; border-radius: 6px; padding: 4px 12px; font-size: 12px; font-weight: 600; }"
        "QPushButton:hover { background: #00705A; }"
    ));
    _restartButton->hide();
    updaterLayout->addWidget(_restartButton);

    aboutPageLayout->addWidget(_updaterWidget);
    aboutPageLayout->addStretch(1);
    pages->addWidget(aboutPage);

    // --- Page 1: Versions ---
    auto *diagPage = new QWidget(pages);
    auto *diagPageLayout = new QVBoxLayout(diagPage);
    diagPageLayout->setContentsMargins(0, 0, 0, 0);
    diagPageLayout->setSpacing(12);

    auto *diagTopRow = new QHBoxLayout;
    diagTopRow->setSpacing(10);

    auto *diagHeaderLbl = new QLabel(tr("ENVIRONMENT & RUNTIME"), diagPage);
    diagHeaderLbl->setStyleSheet(QStringLiteral(
        "font-size: 11px; font-weight: 700; color: %1;"
    ).arg(textMuted));
    diagTopRow->addWidget(diagHeaderLbl);
    diagTopRow->addStretch(1);

    auto *copyBtn = new QPushButton(tr("Copy System Info"), diagPage);
    copyBtn->setObjectName(QStringLiteral("aboutCopyDiagBtn"));
    copyBtn->setCursor(Qt::PointingHandCursor);
    copyBtn->setStyleSheet(QStringLiteral(
        "QPushButton#aboutCopyDiagBtn {"
        "    background: %1;"
        "    color: %2;"
        "    border: 1px solid %3;"
        "    border-radius: 6px;"
        "    padding: 5px 14px;"
        "    font-size: 12px;"
        "    font-weight: 600;"
        "}"
        "QPushButton#aboutCopyDiagBtn:hover {"
        "    background: rgba(34, 197, 94, 0.15);"
        "    color: #22C55E;"
        "    border-color: rgba(34, 197, 94, 0.35);"
        "}"
    ).arg(subCardBg, textPri, borderCol));

    connect(copyBtn, &QPushButton::clicked, this, [copyBtn] {
        const QString plainInfo = Theme::instance()->aboutVersions(Theme::VersionFormat::Plain);
        QGuiApplication::clipboard()->setText(plainInfo);
        copyBtn->setText(tr("Copied!"));
        QTimer::singleShot(2500, copyBtn, [copyBtn] {
            copyBtn->setText(tr("Copy System Info"));
        });
    });
    diagTopRow->addWidget(copyBtn);
    diagPageLayout->addLayout(diagTopRow);

    // Environment & Runtime specs displayed as clean text with dots (NO BOXES!)
    auto *specsLayout = new QHBoxLayout;
    specsLayout->setSpacing(32);

    auto *leftCol = new QVBoxLayout;
    leftCol->setSpacing(10);

    auto *rightCol = new QVBoxLayout;
    rightCol->setSpacing(10);

    auto makeSpecItem = [=](const QString &label, const QString &val) {
        auto *item = new QLabel(diagPage);
        item->setTextInteractionFlags(Qt::TextSelectableByMouse);
        item->setText(QStringLiteral(
            "<span style='color: %1; font-weight: bold;'>•</span> "
            "<b style='color: %2;'>%3:</b> "
            "<span style='color: %4;'>%5</span>"
        ).arg(dark ? QStringLiteral("#00856B") : QStringLiteral("#00856B"), textPri, label, textSec, val));
        item->setStyleSheet(QStringLiteral("font-size: 13px;"));
        return item;
    };

    const QString versionDisplay = Version::displayString();
    const QString qtVer = QString::fromUtf8(qVersion());
    const QString sslVer = QSslSocket::sslLibraryVersionString();
    const QString vfsVer = Utility::enumToString(VfsPluginManager::instance().bestAvailableVfsMode());
    const QString osVer = QStringLiteral("%1 (%2)").arg(QSysInfo::prettyProductName(), Utility::currentCpuArch());
    const QString qpaVer = qGuiApp ? qGuiApp->platformName() : QStringLiteral("cocoa");

    leftCol->addWidget(makeSpecItem(tr("UDrive Version"), versionDisplay));
    leftCol->addWidget(makeSpecItem(tr("Qt Framework"), qtVer));
    leftCol->addWidget(makeSpecItem(tr("SSL Library"), sslVer));

    rightCol->addWidget(makeSpecItem(tr("Virtual Files"), vfsVer));
    rightCol->addWidget(makeSpecItem(tr("Operating System"), osVer));
    rightCol->addWidget(makeSpecItem(tr("Display Platform"), qpaVer));

    specsLayout->addLayout(leftCol, 1);
    specsLayout->addLayout(rightCol, 1);
    diagPageLayout->addLayout(specsLayout);

    auto *detailHeader = new QLabel(tr("BUILD & RUNTIME LOG"), diagPage);
    detailHeader->setStyleSheet(QStringLiteral(
        "font-size: 11px; font-weight: 700; color: %1; margin-top: 10px;"
    ).arg(textMuted));
    diagPageLayout->addWidget(detailHeader);

    // Detailed technical log - NO SCROLLBAR, FULL VISIBILITY, SINGLE UNIFIED FONT
    auto *versionInfo = new QTextBrowser(diagPage);
    versionInfo->setObjectName(QStringLiteral("versionInfo"));
    versionInfo->setOpenExternalLinks(false);
    versionInfo->setText(Theme::instance()->aboutVersions(Theme::VersionFormat::RichText));
    connect(versionInfo, &QTextBrowser::anchorClicked, this, &AboutDialog::openBrowserFromUrl);
    versionInfo->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    versionInfo->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    versionInfo->setFixedHeight(125);
    versionInfo->setStyleSheet(QStringLiteral(
        "QTextBrowser {"
        "    background-color: %1;"
        "    border: 1px solid %2;"
        "    border-radius: 8px;"
        "    color: %3;"
        "    font-size: 12px;"
        "    padding: 10px 12px;"
        "}"
    ).arg(subCardBg, borderCol, textSec));
    diagPageLayout->addWidget(versionInfo);

    diagPageLayout->addStretch(1);
    pages->addWidget(diagPage);
    cardLayout->addWidget(pages);

    // Wire Segmented Tabs
    auto updateTabStyles = [=](int index) {
        const QString activeStyle = QStringLiteral(
            "QPushButton { background: %1; color: %2; font-weight: 600; font-size: 13px; border: none; border-radius: 6px; padding: 6px 16px; }"
        ).arg(dark ? QStringLiteral("#2A2D33") : QStringLiteral("#FFFFFF"), textPri);

        const QString inactiveStyle = QStringLiteral(
            "QPushButton { background: transparent; color: %1; font-weight: 500; font-size: 13px; border: none; border-radius: 6px; padding: 6px 16px; }"
            "QPushButton:hover { color: %2; background: rgba(255, 255, 255, 0.04); }"
        ).arg(textSec, textPri);

        tabAboutBtn->setStyleSheet(index == 0 ? activeStyle : inactiveStyle);
        tabDiagBtn->setStyleSheet(index == 1 ? activeStyle : inactiveStyle);
        pages->setCurrentIndex(index);
    };

    connect(tabAboutBtn, &QPushButton::clicked, this, [=] { updateTabStyles(0); });
    connect(tabDiagBtn, &QPushButton::clicked, this, [=] { updateTabStyles(1); });
    updateTabStyles(0);

    // 4. Footer (Divider + Powered by + OK Button)
    auto *footerDivider = new QFrame(card);
    footerDivider->setFixedHeight(1);
    footerDivider->setStyleSheet(QStringLiteral("background: %1; border: none;").arg(borderCol));
    cardLayout->addWidget(footerDivider);

    auto *footerRow = new QHBoxLayout;
    footerRow->setContentsMargins(0, 0, 0, 0);

    auto *poweredBy = new QLabel(tr("Unity Workspace Desktop"), card);
    poweredBy->setStyleSheet(QStringLiteral("font-size: 12px; color: %1;").arg(textMuted));
    footerRow->addWidget(poweredBy);
    footerRow->addStretch(1);

    auto *okButton = new QPushButton(tr("OK"), card);
    okButton->setObjectName(QStringLiteral("aboutOkButton"));
    okButton->setCursor(Qt::PointingHandCursor);
    okButton->setDefault(true);
    okButton->setStyleSheet(QStringLiteral(
        "QPushButton#aboutOkButton {"
        "    background-color: #00856B;"
        "    color: #FFFFFF;"
        "    border: none;"
        "    border-radius: 8px;"
        "    padding: 7px 24px;"
        "    font-size: 13px;"
        "    font-weight: 600;"
        "    min-width: 80px;"
        "}"
        "QPushButton#aboutOkButton:hover { background-color: #00705A; }"
        "QPushButton#aboutOkButton:pressed { background-color: #005C49; }"
        "QPushButton#aboutOkButton:focus { border: 2px solid #22C55E; }"
    ));
    connect(okButton, &QPushButton::clicked, this, &QDialog::accept);
    footerRow->addWidget(okButton);

    cardLayout->addLayout(footerRow);

    setupUpdaterWidget();
}

AboutDialog::~AboutDialog()
{
    delete ui;
}

void AboutDialog::openBrowser(const QString &s)
{
    Utility::openBrowser(QUrl(s), this);
}

void AboutDialog::openBrowserFromUrl(const QUrl &s)
{
    Utility::openBrowser(s, this);
}

void AboutDialog::setupUpdaterWidget()
{
    if (!_updaterWidget) return;
#ifdef WITH_AUTO_UPDATER
    // non-standard update channels are only supported by the vanilla theme and the testpilotcloud theme
    if (!Resources::isVanillaTheme() && !isTestPilotCloudTheme()) {
        if (Utility::isMac()) {
            _updaterWidget->hide();
            return;
        }
    }

    ConfigFile().makeQSettings().remove("skipUpdateCheck"); // remove old config key

    if (!AppConfig().skipUpdateCheck() && Updater::instance()) {
        if (auto *ocupdater = qobject_cast<OCUpdater *>(Updater::instance())) {
            auto updateInfo = [ocupdater, this] {
                QString statusString = ocupdater->statusString();
                switch (ocupdater->downloadState()) {
                case OCUpdater::Unknown:
                    [[fallthrough]];
                case OCUpdater::CheckingServer:
                    [[fallthrough]];
                case OCUpdater::UpToDate:
                    break;
                case OCUpdater::Downloading:
                    [[fallthrough]];
                case OCUpdater::DownloadComplete:
                    [[fallthrough]];
                case OCUpdater::DownloadFailed:
                    [[fallthrough]];
                case OCUpdater::DownloadTimedOut:
                    [[fallthrough]];
                case OCUpdater::UpdateOnlyAvailableThroughSystem:
                    statusString = QStringLiteral("Version %1 is available. %2").arg(ocupdater->availableVersionString(), statusString);
                    break;
                }

                if (_updateStateLabel) _updateStateLabel->setText(statusString);
                if (_restartButton) _restartButton->setVisible(ocupdater->downloadState() == OCUpdater::DownloadComplete);
            };
            connect(ocupdater, &OCUpdater::downloadStateChanged, this, updateInfo);
            if (_restartButton) connect(_restartButton, &QAbstractButton::clicked, ocupdater, &Updater::applyUpdateAndRestart);
            updateInfo();
        }
#ifdef HAVE_SPARKLE
        if (SparkleUpdater *sparkleUpdater = qobject_cast<SparkleUpdater *>(Updater::instance())) {
            if (_updateStateLabel) _updateStateLabel->setText(sparkleUpdater->statusString());
            if (_restartButton) _restartButton->setVisible(false);
        }
#endif
    } else {
        _updaterWidget->hide();
    }
#else
    _updaterWidget->hide();
#endif
}

} // namespace OCC
