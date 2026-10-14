// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "accountstate.h"
#include <QUrl>
#include <QPersistentModelIndex>
#include <QSet>
#include <QWidget>
#include <QDialog>
#include <QVariant>
#include <memory>

class QComboBox;
class QCheckBox;
class QHBoxLayout;
class QAction;
class QIODevice;
class QNetworkRequest;
class QMenu;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSaveFile;
class QSortFilterProxyModel;
class QStandardItemModel;
class QTreeView;
class QStackedWidget;
class QTemporaryDir;
class QTimer;
class QListView;
class QAbstractItemView;

namespace OCC {
class PropfindJob;
class SimpleNetworkJob;
class UDrivePdfPreview;
class UDriveThumbnails;
class DeleteConfirmDialog;

class UDriveFileBrowser : public QWidget
{
    Q_OBJECT
public:
    enum class Section { Personal, Workspaces, Shares, Favorites, Trash };
    Q_ENUM(Section)
    explicit UDriveFileBrowser(AccountState *state, QWidget *parent = nullptr);
    ~UDriveFileBrowser() override;
    AccountState *accountState() const { return _state; }
    void showPersonal();
    void showWorkspaces();
    void showShares();
    void showFavorites();
    void showTrash();
    bool createFolder(const QString &name);
    bool uploadFile(const QString &localFile);
    bool renameSelected(const QString &name);
    bool copySelected(const QString &name);
    bool moveSelected(const QString &destinationFolder);
    bool deleteSelected();
    bool restoreSelected();
    bool setSelectedFavorite(bool favorite);
    // Save the selected remote file to a destination already chosen by the user.
    bool saveSelectedFile(const QString &destination);
    bool previewSelectedPdf();
    QLineEdit *takeSearchField();
    bool canCreate() const;
    void showNewMenu(const QPoint &globalPosition);

Q_SIGNALS:
    void creationAvailabilityChanged();

Q_SIGNALS:
    void downloadFinished(bool success, const QString &destination);
    void operationFinished(bool success, const QString &message);
    void sectionChanged(OCC::UDriveFileBrowser::Section section);

protected:
    void hideEvent(QHideEvent *event) override;
    void showEvent(QShowEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    enum class ViewMode { Default, Condensed, Tiles };
    enum class SortMode { Recent, NameAscending, NameDescending, Newest, Oldest, Largest, Smallest };
    void setSortMode(SortMode mode, bool save = true);
    void recordOpened(const QUrl &root, const QString &path);
    QString recentSettingsGroup() const;
    void setViewMode(ViewMode mode, bool save = true);
    QAbstractItemView *activeView() const;
    bool startDownload(const QString &destination, bool preview);
    void closePdf();
    void setSection(Section section);
    void refreshFavorites();
    QUrl trashRoot() const;
    QUrl selectedRoot() const;
    QModelIndex selectedItem() const;
    int selectedCount() const;
    void selectAllFiles(bool select);
    void selectOnly(const QModelIndex &index);
    void updateSelectionState();
    bool runOperation(
        const QByteArray &verb, const QUrl &root, const QString &path, const QNetworkRequest &request, const QString &message, QIODevice *body = nullptr);
    void cancelOperation();
    void confirmDelete(bool permanent);
    void addFileActions(QMenu *menu);
    void updateBreadcrumbs();
    void navigateToBreadcrumb(const QString &path, const QUrl &root);
    void updateSpaces();
    void selectSpace();
    void refresh();
    void activateSelection();
    void downloadSelection();
    void updateActions();
    void cancelListing();
    void scheduleThumbnails();
    void loadVisibleThumbnails();
    void cancelDownload();
    void appendEntry(const QString &href, const QMap<QString, QString> &properties, const QUrl &root = {}, const QString &spaceId = {}, bool nested = false);
    QString localPath(const QString &remotePath, const QUrl &root = {}) const;
    bool canBrowse() const;
    void setMessage(const QString &message);

    bool _workspaceLayout = false;
    QPointer<AccountState> _state;
    Section _section = Section::Personal;
    QUrl _root;
    QUrl _webUrl;
    QString _spaceId;
    QString _path;
    QPointer<PropfindJob> _listing;
    QPointer<SimpleNetworkJob> _report;
    QPointer<SimpleNetworkJob> _operation;
    QPointer<DeleteConfirmDialog> _deleteConfirmation;
    bool _uploading = false;
    QString _driveId;
    QString _folderPermissions;
    bool _folderPermissionsKnown = false;
    QPointer<SimpleNetworkJob> _download;
    std::unique_ptr<QSaveFile> _downloadFile;
    QString _destination;
    QString _writeError;
    bool _previewDownload = false;
#ifdef Q_OS_MACOS
    QStackedWidget *_pages;
    UDrivePdfPreview *_pdfPreview;
    std::shared_ptr<QTemporaryDir> _previewStorage;
#endif
    QComboBox *_spaces;
    QPointer<QLineEdit> _search;
    QLabel *_heading;
    QWidget *_breadcrumbs;
    QHBoxLayout *_breadcrumbLayout;
    QString _spaceName;
    QLabel *_message;
    QString _statusMessage;
    QCheckBox *_selectAll;
    QPushButton *_rootButton;
    QPushButton *_refreshButton;
    QPushButton *_openButton;
    QPushButton *_downloadButton;
    QPushButton *_restoreButton;
    QPushButton *_cancelButton;
    QPushButton *_newButton;
    QMenu *_fileMenu;
    QAction *_newFolderAction;
    QAction *_uploadAction;
    QAction *_renameAction;
    QAction *_copyAction;
    QAction *_moveAction;
    QAction *_deleteAction;
    QAction *_restoreAction;
    QAction *_permanentDeleteAction;
    QAction *_favoriteAction;
    QAction *_linkAction;
    QAction *_webAction;
    QProgressBar *_progress;
    QTreeView *_tree;
    QListView *_tiles;
    QStackedWidget *_fileViews;
    QPushButton *_viewButton;
    QPushButton *_sortButton;
    SortMode _sortMode = SortMode::NameAscending;
    QVariantMap _recentFiles;
    ViewMode _viewMode = ViewMode::Default;
    QStandardItemModel *_model;
    QSortFilterProxyModel *_proxy;
    UDriveThumbnails *_thumbnails;
    QTimer *_thumbnailTimer;
    QSet<QPersistentModelIndex> _thumbnailRows;
};

class DeleteConfirmDialog : public QDialog
{
    Q_OBJECT
public:
    explicit DeleteConfirmDialog(const QString &fileName, bool isFolder, QWidget *parent = nullptr, bool permanent = false);
};
}
