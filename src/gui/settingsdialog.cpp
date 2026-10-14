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
#include "udriveicons.h"
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
#include <QLabel>
#include <QVBoxLayout>
#include <QScreen>
#include <QWindow>
#include <QAbstractButton>
#include <QFrame>
#include <QScrollArea>
#include <QPainter>
#include <QKeyEvent>
#include <QTextOption>
#include <algorithm>

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
    AccountPopupButton(const QString &name, const QString &detail, const QString &initials, const QIcon &icon, QWidget *parent)
        : QAbstractButton(parent)
        , _detail(detail)
        , _initials(initials.toUpper())
        , _icon(icon)
    {
        setText(name);
        setAccessibleName(detail.isEmpty() ? name : name + QStringLiteral(", ") + detail);
        setToolTip(detail.isEmpty() ? name : name + QLatin1Char('\n') + detail);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFixedHeight(detail.isEmpty() ? 48 : 72);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const bool dark = palette().color(QPalette::Window).lightness() < 128;
        const QColor textColor(dark ? "#EDEEF0" : "#24332C");
        const QRectF background = QRectF(rect()).adjusted(1, 1, -1, -1);
        painter.setPen(hasFocus() ? QPen(QColor("#40D979"), 1.5) : isChecked() ? QPen(QColor(dark ? "#3C6C4D" : "#A8DDBA"), 1) : QPen(Qt::NoPen));
        painter.setBrush(isChecked()       ? QColor(dark ? "#2D4233" : "#E3F6E9")
                : underMouse() || isDown() ? QColor(dark ? "#353A37" : "#F0F5F2")
                                           : Qt::transparent);
        painter.drawRoundedRect(background, 10, 10);
        const QRect avatar(12, (height() - 36) / 2, 36, 36);
        if (!_initials.isEmpty()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor("#00866A"));
            painter.drawRoundedRect(avatar, 12, 12);
            if (!_icon.isNull()) {
                painter.save();
                QPainterPath clip;
                clip.addRoundedRect(avatar, 12, 12);
                painter.setClipPath(clip);
                _icon.paint(&painter, avatar);
                painter.restore();
            } else {
                painter.setPen(Qt::white);
                auto initialsFont = font();
                initialsFont.setPixelSize(13);
                initialsFont.setBold(true);
                painter.setFont(initialsFont);
                painter.drawText(avatar, Qt::AlignCenter, _initials.left(3));
            }
        } else if (!_icon.isNull())
            _icon.paint(&painter, avatar.adjusted(7, 7, -7, -7));
        const int rightInset = isCheckable() ? 38 : 12;
        const QRect textRect(60, 8, qMax(0, width() - 60 - rightInset), height() - 16);
        auto nameFont = font();
        nameFont.setPixelSize(13);
        nameFont.setWeight(QFont::DemiBold);
        painter.setFont(nameFont);
        painter.setPen(textColor);
        if (_detail.isEmpty()) {
            QTextOption options;
            options.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
            options.setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
            painter.drawText(textRect, text(), options);
        } else {
            painter.drawText(
                textRect.adjusted(0, 2, 0, -28), Qt::AlignVCenter | Qt::AlignLeft, painter.fontMetrics().elidedText(text(), Qt::ElideRight, textRect.width()));
            auto detailFont = font();
            detailFont.setPixelSize(11);
            painter.setFont(detailFont);
            painter.setPen(QColor(dark ? "#AFB8B2" : "#64776B"));
            painter.drawText(
                textRect.adjusted(0, 29, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, painter.fontMetrics().elidedText(_detail, Qt::ElideMiddle, textRect.width()));
        }
        if (isChecked()) {
            painter.setPen(QPen(QColor(dark ? "#40D979" : "#008051"), 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            const QPoint center(width() - 22, height() / 2);
            painter.drawLine(center + QPoint(-5, 0), center + QPoint(-1, 4));
            painter.drawLine(center + QPoint(-1, 4), center + QPoint(6, -4));
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

private:
    QString _detail;
    QString _initials;
    QIcon _icon;
};

class AccountPopup : public QFrame
{
public:
    explicit AccountPopup(QWidget *parent)
        : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint)
    {
        setObjectName(QStringLiteral("driveAccountPopup"));
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
        painter.setPen(QPen(QColor(dark ? "#464646" : "#DFE6E1"), 1));
        painter.setBrush(QColor(dark ? "#292929" : "#FFFFFF"));
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
        _ui->quickWidget->setFixedWidth(236);
        const auto *screen = QApplication::screenAt(QCursor::pos());
        const QSize initialSize(1280, 820);
        resize(screen ? initialSize.boundedTo(screen->availableSize() * 0.9) : initialSize);
        updateUDriveAppearance();
        qApp->installEventFilter(this);
    }

    // People perceive this as a Window, so also make Ctrl+W work
    addAction(tr("Hide"), Qt::CTRL | Qt::Key_W, this, &SettingsDialog::hide);

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
    delete _ui;
}

bool SettingsDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == qApp && (event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::ThemeChange)) {
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
    auto *brand = new QWidget(header);
    brand->setFixedWidth(224);
    auto *brandRow = new QHBoxLayout(brand);
    brandRow->setContentsMargins(0, 0, 0, 0);
    auto *logo = new QLabel(brand);
    logo->setPixmap(Resources::themeUniversalIcon(QStringLiteral("udrive-icon")).pixmap(30, 30));
    auto *name = new QLabel(QStringLiteral("<span style='color:#00B889'>U</span>Drive"), brand);
    auto font = name->font();
    font.setPointSize(17);
    font.setBold(true);
    name->setFont(font);
    brandRow->addWidget(logo);
    brandRow->addWidget(name);
    brandRow->addStretch();
    row->addWidget(brand);
    _searchStack = new QStackedWidget(header);
    _searchStack->setObjectName(QStringLiteral("driveHeaderSearch"));
    _searchStack->setMaximumWidth(720);
    _searchStack->setFixedHeight(44);
    auto *emptySearch = new QLineEdit(header);
    emptySearch->setObjectName(QStringLiteral("driveSearch"));
    emptySearch->setPlaceholderText(tr("Select an account to search files"));
    emptySearch->setEnabled(false);
    _searchStack->addWidget(emptySearch);
    row->addWidget(_searchStack, 1);
    row->addStretch(0);
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
}

void SettingsDialog::showAccountPopup()
{
    if (_accountPopup && _accountPopup->isVisible()) {
        _accountPopup->close();
        return;
    }
    auto *popup = new AccountPopup(this);
    _accountPopup = popup;
    const auto *screen = _profileButton->screen();
    const QRect available = screen->availableGeometry();
    popup->setFixedWidth(qMin(360, available.width() - 20));
    const bool dark = palette().color(QPalette::Window).lightness() < 128;
    popup->setStyleSheet(QStringLiteral("QFrame#driveAccountPopup { background: %1; border: 1px solid %2; border-radius: 14px; }"
                                        "QLabel#driveAccountTitle { color: %3; font-size: 14px; font-weight: 600; }"
                                        "QLabel#driveAccountHint { color: %4; font-size: 12px; }"
                                        "QScrollArea#driveAccountList, QWidget#driveAccountRows { background: transparent; border: none; }"
                                        "QFrame#driveAccountDivider { background: %2; border: none; }")
            .arg(dark ? QStringLiteral("#292929") : QStringLiteral("#FFFFFF"), dark ? QStringLiteral("#464646") : QStringLiteral("#DFE6E1"),
                dark ? QStringLiteral("#EDEEF0") : QStringLiteral("#24332C"), dark ? QStringLiteral("#AFB8B2") : QStringLiteral("#64776B")));
    auto *layout = new QVBoxLayout(popup);
    layout->setContentsMargins(12, 14, 12, 10);
    layout->setSpacing(6);
    auto *title = new QLabel(tr("Accounts"), popup);
    title->setObjectName(QStringLiteral("driveAccountTitle"));
    title->setTextFormat(Qt::PlainText);
    auto *hint = new QLabel(tr("Switch between your accounts."), popup);
    hint->setObjectName(QStringLiteral("driveAccountHint"));
    hint->setWordWrap(true);
    hint->setTextFormat(Qt::PlainText);
    layout->addWidget(title);
    layout->addWidget(hint);
    layout->addSpacing(8);
    auto accounts = AccountManager::instance()->accounts();
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
        scroll->setFixedHeight(qMin(int(accounts.size()) * 78, qMax(78, qMin(312, available.height() - 240))));
        auto *rows = new QWidget(scroll);
        rows->setObjectName(QStringLiteral("driveAccountRows"));
        auto *rowLayout = new QVBoxLayout(rows);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(6);
        for (auto *state : accounts) {
            QPointer<Account> account = state->account();
            const QString name = account->davDisplayName().isEmpty() ? account->hostName() : account->davDisplayName();
            const QString server = account->url().adjusted(QUrl::RemoveUserInfo).authority();
            auto *card =
                new AccountPopupButton(name, server, account->initials().isEmpty() ? QStringLiteral("?") : account->initials(), account->avatar(), rows);
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
    auto *add = new AccountPopupButton(tr("Add account"), {}, {}, UDriveIcons::icon(UDriveIcons::Symbol::Add), popup);
    add->setObjectName(QStringLiteral("drivePopupAddAccount"));
    layout->addWidget(add);
    connect(add, &QAbstractButton::clicked, this, [this, popup] {
        popup->close();
        createNewAccount();
    });
    if (_lastAccount) {
        auto *manage = new AccountPopupButton(tr("Manage account and sync"), {}, {}, UDriveIcons::icon(UDriveIcons::Symbol::Settings), popup);
        manage->setObjectName(QStringLiteral("drivePopupManageAccount"));
        layout->addWidget(manage);
        connect(manage, &QAbstractButton::clicked, this, [this, popup] {
            popup->close();
            if (_lastAccount)
                setCurrentPage(SettingsPage::Account);
        });
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

void SettingsDialog::updateWorkspaceHeader()
{
    if (!_searchStack)
        return;
    auto *files = qobject_cast<UDriveFileBrowser *>(_ui->stack->currentWidget());
    auto *search = files ? files->findChild<QLineEdit *>(QStringLiteral("driveSearch")) : nullptr;
    // Search fields live in the header after integration; retain account identity on each page.
    for (int i = 1; files && i < _searchStack->count(); ++i) {
        auto *field = _searchStack->widget(i);
        if (field->property("accountId").toUuid() == files->accountState()->account()->uuid()) {
            search = qobject_cast<QLineEdit *>(field);
            break;
        }
    }
    _searchStack->setCurrentIndex(search ? _searchStack->indexOf(search) : 0);
    _profileButton->setText(_lastAccount ? _lastAccount->initials().toUpper().left(3) : QStringLiteral("+"));
    Q_EMIT workspaceChanged();
}

bool SettingsDialog::canCreateFiles() const
{
    auto *files = qobject_cast<UDriveFileBrowser *>(_ui->stack->currentWidget());
    return files && files->canCreate();
}

void SettingsDialog::openNewFileMenu()
{
    if (auto *files = qobject_cast<UDriveFileBrowser *>(_ui->stack->currentWidget()))
        files->showNewMenu(_ui->quickWidget->mapToGlobal(QPoint(12, 82)));
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
    // continue to use their own palettes. Follow the system's light/dark mode.
    const bool dark = qApp->palette().color(QPalette::Window).lightness() < 128;
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
        "QStackedWidget#stack QMenu { padding: 4px; border: 1px solid %3; border-radius: 8px; background: %1; color: %2; }"
        "QStackedWidget#stack QMenu::item { padding: 4px 18px 4px 8px; border-radius: 4px; }"
        "QStackedWidget#stack QMenu::item:selected { background: %4; color: %2; }"
        "QStackedWidget#stack QMenu::item:disabled { color: %5; }"
        "QCheckBox#driveSelectAll { color: %2; spacing: 8px; padding-right: 4px; }"
        "QCheckBox#driveSelectAll:disabled { color: %5; }"
        "QCheckBox#driveSelectAll::indicator { width: 18px; height: 18px; border: 1px solid #858585; border-radius: 4px; background: transparent; }"
        "QCheckBox#driveSelectAll::indicator:hover { border-color: %11; }"
        "QCheckBox#driveSelectAll::indicator:checked { border-color: #40D979; background: #40D979; image: url(:/client/resources/core/checkbox-check.svg); }"
        "QCheckBox#driveSelectAll::indicator:indeterminate { border-color: #40D979; background: #40D979; image: url(:/client/resources/core/checkbox-partial.svg); }"
        "QCheckBox#driveSelectAll::indicator:disabled { border-color: %3; background: %6; }"
        "QStackedWidget#stack QGroupBox { border: 1px solid %3; border-radius: 12px; margin-top: 22px; padding: 12px; }"
        "QStackedWidget#stack QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; padding: 0 4px; color: %2; }"
        "QScrollArea#scrollArea { border: none; background: %1; }"
        "QWidget#scrollAreaWidgetContents_2 { background: %1; }"
        "QLineEdit#driveSearch { min-height: 20px; padding: 10px 12px; border: 1px solid %3; border-radius: 20px; background: %8; color: %2; }"
        "QLineEdit#driveSearch:focus { border-color: #00856B; }"
        "QTreeView#driveFiles { border: none; background: %1; color: %2; padding: 0; outline: none; }"
        "QListView#driveTiles { border: none; background: %1; color: %2; padding: 0; outline: none; }"
        "QStackedWidget#stack QPushButton[driveToolbarButton=\"true\"] { min-height: 32px; padding: 0 10px; border: 1px solid %10; border-radius: 8px; background: %9; font-size: 13px; font-weight: 500; }"
        "QStackedWidget#stack QPushButton[driveToolbarButton=\"true\"]:hover { background: %4; border-color: %11; }"
        "QStackedWidget#stack QPushButton[driveToolbarButton=\"true\"]:pressed { background: %4; border-color: %11; }"
        "QStackedWidget#stack QPushButton[driveToolbarButton=\"true\"]:focus { border-color: %11; }"
        "QStackedWidget#stack QPushButton[driveToolbarButton=\"true\"]:disabled { background: %1; color: %5; border-color: %3; }"
        "QStackedWidget#stack QPushButton#driveRefresh { padding: 0; }"
        "QStackedWidget#stack QPushButton#driveViewMode { padding: 0 20px 0 8px; }"
        "QStackedWidget#stack QPushButton#driveViewMode::menu-indicator { image: url(:/udrive/icons/%7.svg); width: 10px; height: 10px; subcontrol-origin: padding; subcontrol-position: center right; right: 6px; }"
        "QStackedWidget#stack QPushButton#driveSort { padding: 0 24px 0 8px; }"
        "QStackedWidget#stack QPushButton#driveSort::menu-indicator { image: url(:/udrive/icons/%7.svg); width: 10px; height: 10px; subcontrol-origin: padding; subcontrol-position: center right; right: 8px; }"
        "QTreeView#driveFiles::item { padding: 6px 8px; border-bottom: 1px solid %3; }"
        "QTreeView#driveFiles::item:hover { background: %6; }"
        "QTreeView#driveFiles::item:selected { background: %4; color: %2; }"
        "QTreeView#driveFiles QHeaderView::section { background: %1; color: %2; border: none; border-bottom: 1px solid %3; padding: 12px 8px; }"
        "QStackedWidget#stack QComboBox#driveSpaces, QStackedWidget#stack QComboBox#languageDropdown, QStackedWidget#stack QComboBox#pdfZoom { min-height: 22px; border: 1px solid %3; border-radius: 9px; padding: 8px 38px 8px 12px; background: %1; color: %2; }"
        "QStackedWidget#stack QComboBox#driveSpaces:hover, QStackedWidget#stack QComboBox#languageDropdown:hover, QStackedWidget#stack QComboBox#pdfZoom:hover { border-color: #00856B; }"
        "QStackedWidget#stack QComboBox#driveSpaces:focus, QStackedWidget#stack QComboBox#languageDropdown:focus, QStackedWidget#stack QComboBox#pdfZoom:focus { border-color: #00856B; }"
        "QStackedWidget#stack QComboBox[udriveCombo=\"true\"]::drop-down { subcontrol-origin: padding; subcontrol-position: center right; width: 30px; border: none; }"
        "QStackedWidget#stack QComboBox[udriveCombo=\"true\"]::down-arrow { image: url(:/udrive/icons/%7.svg); width: 14px; height: 14px; }"
        "QStackedWidget#stack QComboBox[udriveCombo=\"true\"]:disabled { color: %5; background: %6; }"
        "QStackedWidget#stack QComboBox[udriveCombo=\"true\"]::down-arrow:disabled { image: url(:/udrive/icons/chevron-disabled.svg); }"
        "QStackedWidget#stack QComboBox[udriveCombo=\"true\"] QAbstractItemView { background: %1; color: %2; selection-background-color: %4; selection-color: %2; border: 1px solid %3; padding: 4px; }"
        "QStackedWidget#stack QComboBox#driveSpaces { min-height: 20px; padding: 6px 28px 6px 10px; border-radius: 8px; border-color: %10; background: %9; font-size: 13px; font-weight: 500; }"
        "QStackedWidget#stack QComboBox#driveSpaces:hover, QStackedWidget#stack QComboBox#driveSpaces:focus { border-color: %11; }"
        "QStackedWidget#stack QComboBox#driveSpaces::drop-down { width: 24px; }"
        "QStackedWidget#stack QComboBox#driveSpaces::down-arrow { width: 10px; height: 10px; }"
        "QListView#driveSpacesPopup { background: %9; color: %2; border: 1px solid %10; border-radius: 8px; padding: 4px; outline: none; }"
        "QListView#driveSpacesPopup::item { padding: 4px 8px; border: none; border-radius: 5px; }"
        "QListView#driveSpacesPopup::item:hover { background: %6; }"
        "QListView#driveSpacesPopup::item:selected { background: %4; color: %11; }"
        "QTreeView#accountFoldersTreeView { border: 1px solid %3; border-radius: 12px; background: %1; padding: 8px; }"
        "QLabel#syncedFolderCount { color: %2; padding-top: 8px; }")
        .arg(surface, text, border, hover, disabledText, disabledSurface, arrow, dark ? QStringLiteral("#202020") : QStringLiteral("#F6F7F8"))
        .arg(toolbarSurface, toolbarBorder, toolbarAccent));
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

void SettingsDialog::setCurrentPage(SettingsPage currentPage)
{
    _currentPage = currentPage;
    _currentAccount = nullptr;
    switch (_currentPage) {
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


} // namespace OCC

#include "settingsdialog.moc"
