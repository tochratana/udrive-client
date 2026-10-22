// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "accountstate.h"
#include <QUrl>
#include <QPersistentModelIndex>
#include <QSet>
#include <QWidget>
#include <QDialog>
#include <QVariant>
#include <QMap>
#include <QUuid>
#include <QElapsedTimer>
#include <memory>
#include <functional>

class QToolButton;
class QComboBox;
class QCompleter;
class QCheckBox;
class QHBoxLayout;
class QVBoxLayout;
class QAction;
class QIODevice;
class QJsonObject;
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
class QScrollArea;
class QTreeWidget;
class QTreeWidgetItem;

namespace OpenAPI { class OAIDrive; }

namespace OCC {
class PropfindJob;
class SimpleNetworkJob;
class JsonApiJob;
class JsonJob;
class UDrivePdfPreview;
class UDriveFilePreview;
class UDriveThumbnails;
class UDriveWorkspaceActions;
class DeleteConfirmDialog;
class UDriveIncomingSharesNotifier;
class UDriveShareDialog;
class UDriveMoveDialog;

class UDriveFileBrowser : public QWidget
{
    Q_OBJECT
public:
    enum class Section { Personal, Workspaces, Shares, Favorites, Trash };
    Q_ENUM(Section)
    enum WorkspaceRole {
        SpaceIdRole = Qt::UserRole + 20,
        SpaceWebDavUrlRole = Qt::UserRole + 21,
        SpaceWebUrlRole = Qt::UserRole + 22,
        SpaceDriveIdRole = Qt::UserRole + 23,
        SpaceDisabledRole = Qt::UserRole + 24,
        SpaceDescriptionRole = Qt::UserRole + 25,
        SpaceDriveRole = Qt::UserRole + 26
    };
    explicit UDriveFileBrowser(AccountState *state, QWidget *parent = nullptr);
    ~UDriveFileBrowser() override;
    AccountState *accountState() const { return _state; }
    void showPersonal();
    void showHome();
    void showWorkspaces();
    void showWorkspacesOverview();
    bool isWorkspacesOverview() const { return _section == Section::Workspaces && _workspaceOverview; }
    void openWorkspace(const QString &spaceId);
    bool createNewWorkspace();
    void showShares();
    enum class ShareTab { WithMe, WithOthers, ViaLink };
    void setShareTab(ShareTab tab);
    ShareTab shareTab() const { return _shareTab; }
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
    // Opening is requested explicitly and happens only after a successful save.
    bool saveSelectedFile(const QString &destination, bool openAfterDownload = false);
    bool previewSelectedPdf();
    bool previewSelectedFile();
    static void cleanupOrphanedPreviews();
    static QString uploadDuration(qint64 milliseconds);
    QLineEdit *takeSearchField();
    bool canCreate() const;
    void showNewMenu(const QPoint &globalPosition, int minimumWidth = 0);
    UDriveIncomingSharesNotifier *sharesNotifier() const { return _sharesNotifier; }
    void navigateToSharesTab();
    void retranslate();
    void appendEntry(const QString &href, const QMap<QString, QString> &properties, const QUrl &root = {}, const QString &spaceId = {}, bool nested = false);

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
    void changeEvent(QEvent *event) override;

private:
    enum class ViewMode { Default, Condensed, Tiles };
    enum class SortMode { Recent, NameAscending, NameDescending, Newest, Oldest, Largest, Smallest };
    void setSortMode(SortMode mode, bool save = true);
    void recordOpened(const QUrl &root, const QString &path);
    QString recentSettingsGroup() const;
    void setViewMode(ViewMode mode, bool save = true);
    QAbstractItemView *activeView() const;
    bool startDownload(const QString &destination, bool preview, bool openAfterDownload = false);
    void closePdf();
    void showPreparedFile(const QString &path, const QString &name, const QUrl &root, const QString &remotePath, std::shared_ptr<QTemporaryDir> storage);
    QString previewCacheKey(const QModelIndex &index) const;
    void rememberPreview(const QString &key, const QString &path, const std::shared_ptr<QTemporaryDir> &storage);
    void setSection(Section section);
    void refreshFavorites();
    void refreshSharedByMe();
    void refreshIncomingShares();
    void updateShareFilters();
    void rebuildSharerFilter();
    void resetShareFilters();
    bool sharedByMeOverview() const { return _section == Section::Shares && _sharedByMe; }
    QUrl trashRoot() const;
    QUrl selectedRoot() const;
    QModelIndex selectedItem() const;
    int selectedCount() const;
    void selectAllFiles(bool select);
    void selectOnly(const QModelIndex &index);
    void updateSelectionState();
    void updateEmptyState();
    bool runOperation(
        const QByteArray &verb, const QUrl &root, const QString &path, const QNetworkRequest &request, const QString &message, QIODevice *body = nullptr);
    bool uploadFileTus(const QString &localFile, const QString &remotePath, qint64 totalSize);
    void startUploadProgress(const QString &name, qint64 total);
    void updateUploadProgress(qint64 sent);
    void refreshUploadDetails();
    void stopUploadProgress();
    void cancelOperation();
    void confirmDelete(bool permanent);
    void confirmEmptyTrash();
    void deleteNextTrashItem();
    void finishEmptyTrash(bool success, const QString &message);
    void addFileActions(QMenu *menu);
    void buildFileDetails();
    void updateFileDetails();
    void showFileDetailsActions(bool show, bool focus = false);
    void showFileShares();
    void clearFileShares();
    void updateFileColumns();
    void loadShareItemPermissions();
    void updateBreadcrumbs();
    void navigateToBreadcrumb(const QString &path, const QUrl &root);
    void updateSpaces();
    void selectSpace();
    void refresh();
    void activateSelection();
    void downloadSelection();
    void chooseDownloadDestination(bool openAfterDownload);
    void updateActions();
    bool rowActionEnabled(const QModelIndex &index, int action) const;
    void cancelListing();
    void scheduleThumbnails();
    void loadVisibleThumbnails();
    void cancelDownload();
    QString localPath(const QString &remotePath, const QUrl &root = {}) const;
    bool canBrowse() const;
    void setMessage(const QString &message);
    void showWorkspaceCardMenu(const QModelIndex &index);
    void updateWorkspaceCardHeight();
    void setWorkspaceTableView(bool table, bool save = true);
    void appendWorkspace(const OpenAPI::OAIDrive &drive, const QString &name);
    void showWorkspaceMembers(const QModelIndex &index);
    void workspaceAction(const QString &action, const QList<OpenAPI::OAIDrive> &drives);
    void editWorkspaceDescription(const OpenAPI::OAIDrive &drive, const QString &text);
    void updateWorkspaceImages();
    QModelIndexList selectedWorkspaces() const;

    bool _workspaceLayout = false;
    bool _workspaceOverview = false;
    bool _includeDisabledWorkspaces = false;
    bool _workspaceNotice = false;
    QWidget *_fileToolbar = nullptr;
    QWidget *_workspaceHeaderActions = nullptr;
    QWidget *_workspaceBulkActions = nullptr;
    UDriveWorkspaceActions *_workspaceActions = nullptr;
    QAction *_workspaceDownloadAction = nullptr;
    QAction *_workspaceDuplicateAction = nullptr;
    QAction *_workspaceQuotaAction = nullptr;
    QAction *_workspaceDisableAction = nullptr;
    QToolButton *_workspaceCancelButton = nullptr;
    QWidget *_workspaceTitleDivider = nullptr;
    QWidget *_workspaceControls = nullptr;
    QWidget *_workspacePage = nullptr;
    QWidget *_workspaceSelectionControls = nullptr;
    QStackedWidget *_workspaceViews = nullptr;
    QTreeView *_workspacesTable = nullptr;
    QToolButton *_workspaceViewButton = nullptr;
    QCheckBox *_workspaceHeaderSelectAll = nullptr;
    bool _workspaceTableMode = false;
    QLabel *_workspaceCount = nullptr;
    QLineEdit *_workspaceSearch = nullptr;
    QPushButton *_includeDisabledButton = nullptr;
    QCheckBox *_workspaceSelectAll = nullptr;
    QComboBox *_workspaceSort = nullptr;
    QPushButton *_learnWorkspacesButton = nullptr;
    QListView *_workspacesView = nullptr;
    QStandardItemModel *_workspacesModel = nullptr;
    QSortFilterProxyModel *_workspacesProxy = nullptr;
    QPointer<AccountState> _state;
    Section _section = Section::Personal;
    ShareTab _shareTab = ShareTab::WithMe;
    bool _sharedByMe = false;
    bool _includeHiddenShares = false;
    bool _incomingSharesApiAvailable = true;
    bool _shareMetadataAvailable = false;
    QMap<QString, QString> _shareSharers;
    QPushButton *_shareScope = nullptr;
    QWidget *_sharesNav = nullptr;
    QPushButton *_sharedWithMeButton = nullptr;
    QPushButton *_sharedWithOthersButton = nullptr;
    QPushButton *_sharedViaLinkButton = nullptr;
    QToolButton *_sharesSettingsButton = nullptr;
    QWidget *_sharesControls = nullptr;
    QPushButton *_sharesFilterShares = nullptr;
    QPushButton *_sharesFilterHidden = nullptr;
    QComboBox *_shareTypeFilter = nullptr;
    QComboBox *_sharedByFilter = nullptr;
    QLineEdit *_sharesSearch = nullptr;
    bool _emptyStateReady = false;
    QWidget *_emptyState = nullptr;
    QLabel *_emptyIcon = nullptr;
    QLabel *_emptyText = nullptr;
    QUrl _root;
    QUrl _webUrl;
    QString _spaceId;
    QString _path;
    QPointer<PropfindJob> _listing;
    QPointer<SimpleNetworkJob> _report;
    QPointer<SimpleNetworkJob> _operation;
    QPointer<DeleteConfirmDialog> _deleteConfirmation;
    bool _emptyingTrash = false;
    QList<QPair<QString, QString>> _trashItems;
    int _trashPosition = 0;
    QUrl _emptyTrashRoot;
    QUuid _emptyTrashAccount;
    bool _uploading = false;
    QString _uploadName;
    qint64 _uploadTotal = 0;
    qint64 _uploadSent = 0;
    QElapsedTimer _uploadElapsed;
    QTimer *_uploadProgressTimer = nullptr;
    QLabel *_uploadDetails = nullptr;
    QString _driveId;
    QString _folderPermissions;
    bool _folderPermissionsKnown = false;
    QPointer<SimpleNetworkJob> _download;
    std::unique_ptr<QSaveFile> _downloadFile;
    QString _destination;
    QString _writeError;
    bool _previewDownload = false;
    bool _filePreviewDownload = false;
    QString _filePreviewKey;
    QStackedWidget *_pages;
    UDriveFilePreview *_filePreview;
    struct CachedPreview { QString path; std::shared_ptr<QTemporaryDir> storage; qint64 bytes = 0; };
    QMap<QString, CachedPreview> _filePreviewCache;
    QStringList _previewCacheOrder;
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    UDrivePdfPreview *_pdfPreview;
#endif
    std::shared_ptr<QTemporaryDir> _previewStorage;
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
    QPushButton *_emptyTrashButton;
    QPushButton *_shareButton;
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
    QAction *_shareAction;
    QAction *_detailsAction = nullptr;
    QWidget *_fileDetails = nullptr;
    QStackedWidget *_detailsPages = nullptr;
    QToolButton *_detailsBack = nullptr;
    QLabel *_detailsTitle = nullptr;
    QLabel *_detailsFileIcon = nullptr;
    QPointer<UDriveShareDialog> _detailsShares;
    QLabel *_detailsName = nullptr;
    QLabel *_detailsIcon = nullptr;
    QLabel *_detailsLocation = nullptr;
    QLabel *_detailsModified = nullptr;
    QLabel *_detailsSize = nullptr;
    QLabel *_detailsPeople = nullptr;
    QLabel *_detailsOwner = nullptr;
    QPushButton *_detailsOpen = nullptr;
    QPushButton *_detailsShare = nullptr;
    QPushButton *_detailsDownload = nullptr;
    QPushButton *_detailsRename = nullptr;
    QPushButton *_detailsWeb = nullptr;
    bool _detailsRequested = false;
    QPersistentModelIndex _detailsIndex;
    QPointer<PropfindJob> _shareItemJob;
    QProgressBar *_progress;
    QTreeView *_tree;
    QListView *_tiles;
    QPointer<QWidget> _checkboxViewport;
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
    UDriveIncomingSharesNotifier *_sharesNotifier = nullptr;
};

class DeleteConfirmDialog : public QDialog
{
    Q_OBJECT
public:
    explicit DeleteConfirmDialog(const QString &fileName, bool isFolder, QWidget *parent = nullptr, bool permanent = false, int trashCount = 0);
};

class UDriveInputDialog : public QDialog
{
    Q_OBJECT
public:
    explicit UDriveInputDialog(const QString &title, const QString &label, const QString &initialText = {},
                               const QString &confirmButtonText = {}, QWidget *parent = nullptr,
                               bool allowEmpty = false);
    QString text() const;
    static QString getText(QWidget *parent, const QString &title, const QString &label,
                           const QString &initialText = {}, const QString &confirmButtonText = {},
                           bool *ok = nullptr, bool allowEmpty = false);

private:
    QLineEdit *_lineEdit = nullptr;
    QPushButton *_confirmButton = nullptr;
};

class UDriveMoveDialog : public QDialog
{
    Q_OBJECT
public:
    explicit UDriveMoveDialog(AccountState *accountState, const QUrl &rootUrl, const QString &spaceName,
                              const QString &sourcePath, const QString &sourceName, bool isFolder,
                              const QList<QPair<QString, QString>> &initialFolders = {},
                              QWidget *parent = nullptr);
    ~UDriveMoveDialog() override;

    QString selectedDestination() const;

private:
    void populateInitialFolders(const QList<QPair<QString, QString>> &initialFolders);
    void loadFolderChildren(QTreeWidgetItem *parentItem, const QString &folderPath);
    void createNewFolder();
    void updateMoveButtonState();

    QPointer<AccountState> _accountState;
    QUrl _rootUrl;
    QString _spaceName;
    QString _sourcePath;
    QString _sourceName;
    QString _currentParentFolder;
    bool _isFolder = false;

    QTreeWidget *_tree = nullptr;
    QTreeWidgetItem *_rootItem = nullptr;
    QPushButton *_newFolderButton = nullptr;
    QPushButton *_cancelButton = nullptr;
    QPushButton *_moveButton = nullptr;
    QLabel *_destinationLabel = nullptr;
};

class UDriveShareDialog : public QDialog
{
    Q_OBJECT
public:
    enum class Target { File, Workspace };
    enum class Presentation { Dialog, Sidebar };
    explicit UDriveShareDialog(AccountState *accountState, const QString &fileName, bool isFolder,
                               const QString &remotePath, const QUrl &linkUrl, const QString &spaceId = {},
                               QWidget *parent = nullptr, Target target = Target::File, Presentation presentation = Presentation::Dialog);
    ~UDriveShareDialog() override;
    bool hasChanges() const { return _modified; }
    void editPublicLink(const QString &shareId, const QJsonObject &share, const QString &field);

Q_SIGNALS:
    void accessListLoaded(const QStringList &people, int publicLinks);

private:
    bool canShare() const;
    void cancelRecipientSearch();
    void searchRecipients();
    void selectRecipient(const QModelIndex &index);
    void sendShare();
    void fetchShares();
    void revokeShare(const QString &shareId);
    void updatePersonPermissions(const QString &shareId, int permissions);
    void copyLink();
    void createPublicLink();
    void updatePublicLinkPermissions(const QString &shareId, int permissions);
    void updatePublicLink(const QString &shareId, const QString &field, const QString &value,
        std::function<void(bool, const QString &)> completion = {});
    void setPublicLinkBusy(bool busy);
    void copyPublicLink(const QString &url);
    void deletePublicLink(const QString &shareId);
    void fetchWorkspaceMembers();
    void inviteWorkspaceMember();
    void removeWorkspaceMember(const QString &permissionId);
    SimpleNetworkJob *workspaceRequest(const QByteArray &verb, const QString &suffix, const QJsonObject &body);
    void cancelWorkspaceRequest();
    void cancelPublicLinkRequest();

    QPointer<AccountState> _accountState;
    QTimer *_recipientTimer = nullptr;
    QCompleter *_recipientCompleter = nullptr;
    QStandardItemModel *_recipientModel = nullptr;
    QPointer<JsonApiJob> _recipientJob;
    QPointer<SimpleNetworkJob> _workspaceJob;
    QPointer<JsonApiJob> _publicLinkJob;
    Target _target = Target::File;
    bool _sidebar = false;
    bool _modified = false;
    QMap<QString, QString> _workspaceRoles;
    bool _canInviteMembers = false;
    bool _canRemoveMembers = false;
    QString _recipientId;
    QString _recipientLabel;
    int _recipientType = -1;
    bool _sharing = false;
    bool _closed = false;
    QString _fileName;
    bool _isFolder = false;
    QString _remotePath;
    QUrl _linkUrl;
    QString _spaceId;

    QLineEdit *_userInput = nullptr;
    QComboBox *_roleCombo = nullptr;
    QPushButton *_shareButton = nullptr;
    QLabel *_statusLabel = nullptr;
    QVBoxLayout *_sharesLayout = nullptr;
    QWidget *_sharesContainer = nullptr;
    QPushButton *_copyLinkButton = nullptr;
    QWidget *_publicLinksSection = nullptr;
    QPushButton *_addLinkButton = nullptr;
    QScrollArea *_publicLinksScrollArea = nullptr;
    QWidget *_publicLinksContainer = nullptr;
    QVBoxLayout *_publicLinksLayout = nullptr;
};

struct UDriveShareNotification
{
    QString id;
    QString fileName;
    QString sharerName;
    QString sharerInitials;
    QDateTime timestamp;
    bool isRead = false;
};

class UDriveIncomingSharesNotifier : public QObject
{
    Q_OBJECT
public:
    explicit UDriveIncomingSharesNotifier(AccountState *accountState, QObject *parent = nullptr);
    ~UDriveIncomingSharesNotifier() override;

    void checkNow();
    bool hasBaseline() const;
    bool isInFlight() const;
    void recordKnownShareIds(const QSet<QString> &ids);

    QList<UDriveShareNotification> notifications() const;
    int unreadCount() const;
    void markAllAsRead();
    void markAsRead(const QString &id);

Q_SIGNALS:
    void incomingSharesUpdated();
    void newSharesDetected(const QStringList &fileNames, const QStringList &sharers);
    void notificationsChanged();
    void unreadCountChanged(int count);

private Q_SLOTS:
    void poll();
    void onJobFinished();
    void onAccountStateChanged();

private:
    void loadReadState();
    void saveReadState();
    void dispatchNotifications(const QList<QPair<QString, QString>> &newShares);

    QPointer<AccountState> _accountState;
    QTimer *_pollTimer = nullptr;
    QPointer<JsonJob> _pollJob;
    QSet<QString> _knownShareIds;
    QSet<QString> _readShareIds;
    QList<UDriveShareNotification> _notifications;
    bool _initialFetchDone = false;
    bool _apiAvailable = true;
    bool _recheckRequested = false;
};
}
