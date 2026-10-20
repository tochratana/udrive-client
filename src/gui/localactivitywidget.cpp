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

#include "accountmanager.h"
#include "accountstate.h"
#include "commonstrings.h"
#include "folder.h"
#include "folderman.h"
#include "guiutility.h"
#include "openfilemanager.h"
#include "localactivitywidget.h"
#include "syncfileitem.h"

#include "models/expandingheaderview.h"

#include <QActionGroup>
#include <QClipboard>
#include "ui_localactivitywidget.h"
#include "activityitemdelegate.h"
#include "udriveicons.h"
#include "libsync/configfile.h"

#include <QLabel>
#include <QStackedWidget>

namespace OCC {

LocalActivityWidget::LocalActivityWidget(QWidget *parent)
    : QWidget(parent)
    , _ui(new Ui::LocalActivityWidget)
{
    _ui->setupUi(this);

    _ui->verticalLayout->setContentsMargins(0, 10, 0, 0);
    _ui->verticalLayout->setSpacing(10);
    _ui->horizontalLayout->setContentsMargins(0, 0, 0, 0);
    _ui->horizontalLayout->setSpacing(10);

    _countLabel = new QLabel(this);
    _countLabel->setObjectName(QStringLiteral("activityItemCount"));
    _ui->horizontalLayout->insertWidget(0, _countLabel);

    _ui->_filterButton->setObjectName(QStringLiteral("activityFilterButton"));
    _ui->_filterButton->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Filter));
    _ui->_filterButton->setCursor(Qt::PointingHandCursor);

    connect(ProgressDispatcher::instance(), &ProgressDispatcher::itemCompleted,
        this, &LocalActivityWidget::slotItemCompleted);

    connect(_ui->_tableView, &QTableView::customContextMenuRequested, this, &LocalActivityWidget::slotItemContextMenu);

    // Build the model-view "stack":
    //  _model <- _sortModel <- _statusSortModel <- _tableView
    _model = new ProtocolItemModel(2000, false, this);
    _sortModel = new Models::SignalledQSortFilterProxyModel(this);
    connect(_sortModel, &Models::SignalledQSortFilterProxyModel::filterChanged, this, &LocalActivityWidget::filterDidChange);
    _sortModel->setSourceModel(_model);
    _sortModel->setSortRole(Models::UnderlyingDataRole);
    _ui->_tableView->setModel(_sortModel);

    _ui->_tableView->setObjectName(QStringLiteral("activityTableView"));
    _ui->_tableView->setItemDelegate(new ActivityItemDelegate(_ui->_tableView));
    _ui->_tableView->setAlternatingRowColors(false);
    _ui->_tableView->setShowGrid(false);
    _ui->_tableView->setFrameShape(QFrame::NoFrame);
    _ui->_tableView->verticalHeader()->setDefaultSectionSize(40);

    // Stacked widget for table and modern empty state
    _stack = new QStackedWidget(this);
    _ui->verticalLayout->replaceWidget(_ui->_tableView, _stack);
    _stack->addWidget(_ui->_tableView);

    _emptyWidget = new QWidget(this);
    _emptyWidget->setObjectName(QStringLiteral("activityEmptyState"));
    auto *emptyLayout = new QVBoxLayout(_emptyWidget);
    emptyLayout->setAlignment(Qt::AlignCenter);
    emptyLayout->setSpacing(10);
    emptyLayout->setContentsMargins(32, 48, 32, 48);

    _emptyIcon = new QLabel(_emptyWidget);
    _emptyIcon->setObjectName(QStringLiteral("activityEmptyIcon"));
    _emptyIcon->setAlignment(Qt::AlignCenter);
    _emptyIcon->setFixedSize(64, 64);
    emptyLayout->addWidget(_emptyIcon, 0, Qt::AlignHCenter);
    emptyLayout->addSpacing(4);

    _emptyTitle = new QLabel(_emptyWidget);
    _emptyTitle->setObjectName(QStringLiteral("activityEmptyTitle"));
    _emptyTitle->setAlignment(Qt::AlignCenter);
    QFont titleFont = _emptyTitle->font();
    titleFont.setPixelSize(17);
    titleFont.setBold(true);
    _emptyTitle->setFont(titleFont);
    emptyLayout->addWidget(_emptyTitle, 0, Qt::AlignHCenter);

    _emptySubtitle = new QLabel(_emptyWidget);
    _emptySubtitle->setObjectName(QStringLiteral("activityEmptySubtitle"));
    _emptySubtitle->setAlignment(Qt::AlignCenter);
    _emptySubtitle->setWordWrap(true);
    _emptySubtitle->setMaximumWidth(420);
    QFont subFont = _emptySubtitle->font();
    subFont.setPixelSize(13);
    _emptySubtitle->setFont(subFont);
    emptyLayout->addWidget(_emptySubtitle, 0, Qt::AlignHCenter);

    _stack->addWidget(_emptyWidget);

    connect(_sortModel, &QAbstractItemModel::rowsInserted, this, &LocalActivityWidget::updateEmptyState);
    connect(_sortModel, &QAbstractItemModel::rowsRemoved, this, &LocalActivityWidget::updateEmptyState);
    connect(_sortModel, &QAbstractItemModel::modelReset, this, &LocalActivityWidget::updateEmptyState);
    connect(_sortModel, &QAbstractItemModel::layoutChanged, this, &LocalActivityWidget::updateEmptyState);

    auto header = new ExpandingHeaderView(QStringLiteral("ActivityListHeaderV2"), _ui->_tableView);
    _ui->_tableView->setHorizontalHeader(header);
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setExpandingColumn(static_cast<int>(ProtocolItemModel::ProtocolItemRole::File));
    header->setSortIndicator(static_cast<int>(ProtocolItemModel::ProtocolItemRole::Time), Qt::DescendingOrder);
    header->hideSection(static_cast<int>(ProtocolItemModel::ProtocolItemRole::Status));
    header->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(header, &QHeaderView::customContextMenuRequested, header, [header, this] {
        auto menu = showFilterMenu(header, _sortModel, static_cast<int>(ProtocolItemModel::ProtocolItemRole::Account), tr("Account"));
        menu->addSeparator();
        header->addResetActionToMenu(menu);
    });

    connect(_ui->_filterButton, &QAbstractButton::clicked, this, [this] {
        showFilterMenu(_ui->_filterButton, _sortModel, static_cast<int>(ProtocolItemModel::ProtocolItemRole::Account), tr("Account"));
    });

    connect(FolderMan::instance(), &FolderMan::folderRemoved, this, &LocalActivityWidget::onFolderRemoved);
    connect(FolderMan::instance(), &FolderMan::folderListChanged, this, &LocalActivityWidget::onFolderListChanged);

    updateEmptyState();
}

LocalActivityWidget::~LocalActivityWidget()
{
    delete _ui;
}

void LocalActivityWidget::onFolderListChanged(const QUuid &accountId, const QList<Folder *> folders)
{
    _model->remove_if([accountId, folders](const ProtocolItem &item) {
        return (item.folder()->accountState()->account()->uuid() == accountId && !folders.contains(item.folder()));
    });
}

void LocalActivityWidget::onFolderRemoved(const QUuid &accountId, Folder *f)
{
    Q_UNUSED(accountId);
    _model->remove_if([f](const ProtocolItem &item) { return item.folder() == f; });
}
/**
 * @brief Show a filter menu for the given model.
 *
 * @param parent Parent widget
 * @param model The model that will do the filtering
 * @param role the role (column number) to filter on
 * @param columnName the name column on which the filter is done
 * @return
 */
QMenu *LocalActivityWidget::showFilterMenu(QWidget *parent, Models::SignalledQSortFilterProxyModel *model, int role, const QString &columnName)
{
    auto menu = new QMenu(parent);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setAccessibleName(tr("Filter menu"));
    Models::addFilterMenuItems(menu, AccountManager::instance()->accountNames(), model, role, columnName, Qt::DisplayRole);
    QTimer::singleShot(0, menu, [menu] {
        menu->popup(QCursor::pos());
        // accassability
        menu->setFocus();
    });
    return menu;
}

void LocalActivityWidget::showContextMenu(QWidget *parent, QTableView *table, Models::SignalledQSortFilterProxyModel *sortModel, ProtocolItemModel *itemModel,
    const QModelIndexList &items, const QPoint &pos)
{
    auto menu = new QMenu(parent);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setAccessibleName(tr("Actions menu"));

    // keep in sync with ActivityWidget::slotItemContextMenu
    menu->addAction(CommonStrings::copyToClipBoard(), parent, [text = Models::formatSelection(items)] {
        QApplication::clipboard()->setText(text);
    });

    if (items.size() == 1) {
        const auto &data = itemModel->protocolItem(items.first());

        // Show in file browser action
        const QString localPath = data.folder()->path() + data.path();
        // keep in sync with ActivityWidget::slotItemContextMenu
        auto showInFileBrowserAction = menu->addAction(CommonStrings::showInFileBrowser(), parent, [localPath] {
            // Double-check if the file still exists
            if (QFileInfo::exists(localPath)) {
                showInFileManager(localPath);
            }
        });
        if (!QFileInfo::exists(localPath)) {
            showInFileBrowserAction->setEnabled(false);
        }

        // "Open in Browser" action
        auto showInWebBrowserAction = menu->addAction(CommonStrings::showInWebBrowser());
        showInWebBrowserAction->setEnabled(false);
        fetchPrivateLinkUrl(data.folder()->accountState()->account(), data.folder()->webDavUrl(), data.folder()->remotePathTrailingSlash() + data.path(),
            parent, [showInWebBrowserAction, parent, pos = menu->actions().size(), menu = QPointer<QMenu>(menu)](const QUrl &url) {
                // as fetchPrivateLinkUrl is async we need to check the menu still exists
                if (menu) {
                    connect(showInWebBrowserAction, &QAction::triggered, [url, parent] { Utility::openBrowser(url, parent); });
                    showInWebBrowserAction->setEnabled(true);
                }
            });

        menu->addSeparator();

        // Sort actions
        auto sortActions = new QActionGroup(menu);
        int columnNr = table->columnAt(pos.x());
        QString columnName = itemModel->headerData(columnNr, Qt::Horizontal, Qt::DisplayRole).toString();
        auto ascendingAction =
            menu->addAction(tr("Sort ascending by %1").arg(columnName), [table, columnNr]() { table->sortByColumn(columnNr, Qt::AscendingOrder); });
        ascendingAction->setCheckable(true);
        sortActions->addAction(ascendingAction);
        auto descendingAction =
            menu->addAction(tr("Sort descending by %1").arg(columnName), [table, columnNr]() { table->sortByColumn(columnNr, Qt::DescendingOrder); });
        descendingAction->setCheckable(true);
        sortActions->addAction(descendingAction);
        if (columnNr == sortModel->sortColumn()) {
            switch (sortModel->sortOrder()) {
            case Qt::AscendingOrder:
                ascendingAction->setChecked(true);
                break;
            case Qt::DescendingOrder:
                descendingAction->setChecked(true);
                break;
            }
        }

        // Retry action
        switch (data.status()) {
        case SyncFileItem::DetailError:
            Q_FALLTHROUGH();
        case SyncFileItem::SoftError:
            Q_FALLTHROUGH();
        case SyncFileItem::BlacklistedError:
            menu->addSeparator();
            menu->addAction(tr("Retry sync"), parent, [data, folder = QPointer<Folder>(data.folder())] {
                if (folder) {
                    folder->journalDb()->wipeErrorBlacklistEntry(data.path());
                    FolderMan::instance()->scheduler()->enqueueFolder(folder, SyncScheduler::Priority::Medium);
                }
            });
            break;
        default:
            break;
        }
    }

    menu->popup(table->mapToGlobal(pos));
    menu->setFocus(); // For accessibility
}

void LocalActivityWidget::slotItemContextMenu(const QPoint &pos)
{
    auto rows = _ui->_tableView->selectionModel()->selectedRows();
    for (int i = 0; i < rows.size(); ++i) {
        rows[i] = _sortModel->mapToSource(rows[i]);
    }
    showContextMenu(this, _ui->_tableView, _sortModel, _model, rows, pos);
}

void LocalActivityWidget::slotItemCompleted(Folder *folder, const SyncFileItemPtr &item)
{
    if (!item->showInProtocolTab())
        return;
    _model->addProtocolItem(ProtocolItem { folder, item });
    updateEmptyState();
}

void LocalActivityWidget::filterDidChange()
{
    const bool active = !_sortModel->filterRegularExpression().pattern().isEmpty();
    _ui->_filterButton->setText(CommonStrings::filterButtonText(active ? 1 : 0));
    _ui->_filterButton->setProperty("filterActive", active);
    _ui->_filterButton->style()->unpolish(_ui->_filterButton);
    _ui->_filterButton->style()->polish(_ui->_filterButton);
    updateEmptyState();
}

void LocalActivityWidget::updateEmptyState()
{
    const int count = _sortModel ? _sortModel->rowCount() : 0;
    if (_countLabel) {
        _countLabel->setText(count > 0 ? tr("%n item(s)", nullptr, count) : QString());
    }

    if (count > 0) {
        _stack->setCurrentWidget(_ui->_tableView);
    } else {
        const auto themeMode = ConfigFile().themeMode();
        const bool dark = themeMode == ConfigFile::ThemeMode::Dark
            || (themeMode == ConfigFile::ThemeMode::System && qApp->palette().color(QPalette::Window).lightness() < 128);

        const bool filtered = (_model && _model->rowCount() > 0);
        if (filtered) {
            _emptyIcon->setPixmap(UDriveIcons::icon(UDriveIcons::Symbol::Search, false, QColor(dark ? "#939D97" : "#7D8781")).pixmap(64, 64));
            _emptyTitle->setText(tr("No matching activity"));
            _emptySubtitle->setText(tr("Try selecting a different filter or account."));
        } else {
            _emptyIcon->setPixmap(UDriveIcons::icon(UDriveIcons::Symbol::Activity, false, QColor(dark ? "#63E493" : "#00856B")).pixmap(64, 64));
            _emptyTitle->setText(tr("No activity yet"));
            _emptySubtitle->setText(tr("Synced files and updates will appear here as activity occurs."));
        }
        _stack->setCurrentWidget(_emptyWidget);
    }
}

} // OCC namespace
