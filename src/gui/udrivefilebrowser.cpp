// SPDX-License-Identifier: GPL-2.0-or-later
#include "udrivefilebrowser.h"
#include "udriveicons.h"
#include "udrivethumbnails.h"
#include "creds/localdevelopment.h"

#include "folderman.h"
#include "libsync/configfile.h"
#include <QListView>
#include <QActionGroup>
#include <QStackedWidget>
#ifdef Q_OS_MACOS
#include "udrivepdfpreview.h"
#include <QStackedWidget>
#include <QTemporaryDir>
#endif
#include "libsync/account.h"
#include "libsync/creds/abstractcredentials.h"
#include "libsync/graphapi/spacesmanager.h"
#include "libsync/networkjobs.h"
#include "common/utility.h"

#include <QComboBox>
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QCryptographicHash>
#include <QFile>
#include <QInputDialog>
#include <QMessageBox>
#include <QXmlStreamReader>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QNetworkReply>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QStyle>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QTreeView>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QMouseEvent>
#include <QTimeZone>
#include <QTimer>
#include <QScrollBar>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>

namespace OCC {
namespace {
enum Roles { PathRole = Qt::UserRole + 1, DirectoryRole, SortRole, RootRole, SpaceRole,
    PermissionsRole, FavoriteRole, FavoriteKnownRole, EtagRole, LinkRole, RestorePathRole, ThumbnailRole, OpenedRole };

QString recentKey(const QUrl &root, const QString &path)
{
    const QByteArray identity = root.toEncoded() + '\0' + path.toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
}

void trimRecentFiles(QVariantMap &files)
{
    while (files.size() > 256) {
        auto oldest = std::min_element(files.begin(), files.end(), [](const QVariant &a, const QVariant &b) { return a.toLongLong() < b.toLongLong(); });
        files.erase(oldest);
    }
}

bool validName(const QString &name)
{
    return !name.isEmpty() && name != QStringLiteral(".") && name != QStringLiteral("..")
        && !name.contains(QLatin1Char('/')) && !name.contains(QLatin1Char('\\'))
        && std::none_of(name.begin(), name.end(), [](QChar c) { return c.unicode() < 32 || c.unicode() == 127; });
}

bool validPath(const QString &path)
{
    if (path.isEmpty()) return false;
    for (const auto &part : path.split(QLatin1Char('/'))) if (!validName(part)) return false;
    return true;
}

class FileActionJob : public SimpleNetworkJob
{
public:
    using SimpleNetworkJob::SimpleNetworkJob;
    // Only a definitive authentication rejection is safe to replay. Do not
    // resend ambiguous write failures, redirects, or repeatedly rejected tokens.
    bool needsRetry() const override
    {
        return reply() && httpStatusCode() == 401 && reply()->error() == QNetworkReply::AuthenticationRequiredError
            && reply()->attribute(QNetworkRequest::RedirectionTargetAttribute).isNull()
            && _attempts == 1 && SimpleNetworkJob::needsRetry();
    }

protected:
    void newReplyHook(QNetworkReply *reply) override
    {
        // Queue unblocking also calls retry() before the first request. Count
        // actual network attempts rather than queue releases.
        ++_attempts;
        SimpleNetworkJob::newReplyHook(reply);
    }

private:
    int _attempts = 0;
};

bool multistatusSucceeded(const QByteArray &xml)
{
    QXmlStreamReader reader(xml);
    if (!reader.readNextStartElement() || reader.namespaceUri() != QLatin1String("DAV:")
        || reader.name() != QLatin1String("multistatus")) return false;
    bool statusFound = false;
    bool valid = true;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement() && reader.namespaceUri() == QLatin1String("DAV:") && reader.name() == QLatin1String("status")) {
            const auto status = reader.readElementText().simplified().split(QLatin1Char(' '));
            bool codeValid = false;
            const int code = status.value(1).toInt(&codeValid);
            statusFound = true;
            valid &= status.value(0).startsWith(QLatin1String("HTTP/")) && codeValid && code >= 200 && code < 300;
        }
    }
    return statusFound && valid && !reader.hasError();
}

QString withoutTrailingSlash(QString path)
{
    while (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    return path;
}

class BreadcrumbLabel : public QLabel
{
public:
    BreadcrumbLabel(const QString &name, QWidget *parent) : QLabel(parent), _name(name)
    {
        setTextFormat(Qt::PlainText);
        setAccessibleName(name); setToolTip(name);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        auto bold = font(); bold.setBold(true); setFont(bold);
        setText(name);
    }
protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QLabel::resizeEvent(event);
        setText(fontMetrics().elidedText(_name, Qt::ElideMiddle, qMax(0, width() - 8)));
    }
private:
    QString _name;
};

class FileRowDelegate : public QStyledItemDelegate
{
public:
    FileRowDelegate(QTreeView *tree, std::function<void(const QModelIndex &, int)> action)
        : QStyledItemDelegate(tree)
        , _tree(tree)
        , _action(std::move(action))
    {
    }
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        auto adjusted = option;
        if (index.column() == 0) {
            adjusted.rect.adjust(34, 0, 0, 0);
            QStyledItemDelegate::paint(painter, adjusted, index);
            const QRect check(option.rect.left() + 8, option.rect.center().y() - 9, 18, 18);
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            const bool selected = option.state & QStyle::State_Selected;
            painter->setPen(QPen(selected ? QColor("#40D979") : QColor("#858585"), 1.5));
            painter->setBrush(selected ? QColor("#40D979") : Qt::transparent);
            painter->drawRoundedRect(check, 4, 4);
            if (selected) {
                painter->setPen(QPen(QColor("#143B20"), 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter->drawLine(check.topLeft() + QPoint(4, 9), check.topLeft() + QPoint(8, 13));
                painter->drawLine(check.topLeft() + QPoint(8, 13), check.topLeft() + QPoint(14, 5));
            }
            painter->restore();
            return;
        }
        if (index.column() == 4) {
            QStyledItemDelegate::paint(painter, option, index);
            const auto mode = _tree->isEnabled() ? QIcon::Normal : QIcon::Disabled;
            const auto name = index.siblingAtColumn(0);
            if (name.data(FavoriteRole).toBool())
                UDriveIcons::icon(UDriveIcons::Symbol::Star).paint(painter, actionRect(option.rect, 0), Qt::AlignCenter, mode);
            if (name.data(LinkRole).toUrl().isValid() && !name.data(LinkRole).toUrl().isEmpty())
                UDriveIcons::icon(UDriveIcons::Symbol::Link).paint(painter, actionRect(option.rect, 1), Qt::AlignCenter, mode);
            UDriveIcons::icon(UDriveIcons::Symbol::MoreVertical).paint(painter, actionRect(option.rect, 2), Qt::AlignCenter, mode);
            return;
        }
        QStyledItemDelegate::paint(painter, option, index);
    }
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(qMax(_tree->property("driveCondensed").toBool() ? 30 : 42, size.height()));
        return size;
    }
    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option, const QModelIndex &index) override
    {
        if (event->type() == QEvent::MouseButtonRelease) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton && index.column() == 4) {
                for (int action = 0; action < 3; ++action)
                    if (actionRect(option.rect, action).adjusted(-6, -10, 6, 10).contains(mouse->position().toPoint())) {
                        _action(index.siblingAtColumn(0), action);
                        return true;
                    }
            }
        }
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }

private:
    static QRect actionRect(const QRect &row, int action) { return QRect(row.right() - 104 + action * 34, row.center().y() - 9, 18, 18); }
    QTreeView *_tree;
    std::function<void(const QModelIndex &, int)> _action;
};

class FileTileDelegate : public QStyledItemDelegate
{
public:
    FileTileDelegate(QListView *view, std::function<void(const QModelIndex &)> more)
        : QStyledItemDelegate(view), _more(std::move(more)) { }
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return QSize(184, 204); }
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const bool dark = option.palette.color(QPalette::Window).lightness() < 128;
        const bool selected = option.state & QStyle::State_Selected;
        const bool hover = option.state & QStyle::State_MouseOver;
        const QRect card = option.rect.adjusted(5, 5, -5, -5);
        painter->save();
        painter->setClipRect(card);
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(QPen(selected ? QColor("#40D979") : QColor(dark ? "#464646" : "#E1E5E8"), selected ? 2 : 1));
        painter->setBrush(QColor(selected ? (dark ? "#2D4233" : "#E3F6E9") : hover ? (dark ? "#3A3A3A" : "#F0F5F2") : (dark ? "#323232" : "#FAFBFA")));
        painter->drawRoundedRect(card.adjusted(1, 1, -1, -1), 12, 12);
        const QRect preview(card.left() + 14, card.top() + 16, card.width() - 28, 112);
        auto icon = index.data(Qt::DecorationRole).value<QIcon>();
        if (index.data(ThumbnailRole).toBool()) {
            const auto pixmap = icon.pixmap(QSize(256, 256));
            const auto size = pixmap.size().scaled(preview.size(), Qt::KeepAspectRatio);
            const QRect target(preview.center() - QPoint(size.width() / 2, size.height() / 2), size);
            QPainterPath clip; clip.addRoundedRect(target, 6, 6);
            painter->save(); painter->setClipPath(clip, Qt::IntersectClip); painter->drawPixmap(target, pixmap); painter->restore();
        } else icon.paint(painter, QRect(preview.center() - QPoint(26, 26), QSize(52, 52)));
        painter->setPen(QColor(dark ? "#EDEEF0" : "#2B3035"));
        auto font = option.font; font.setWeight(QFont::DemiBold); painter->setFont(font);
        const QRect name(card.left() + 12, card.top() + 138, card.width() - 24, 22);
        painter->drawText(name, Qt::AlignVCenter | Qt::AlignLeft, painter->fontMetrics().elidedText(index.data().toString(), Qt::ElideMiddle, name.width()));
        font.setWeight(QFont::Normal); font.setPointSizeF(qMax(9.0, font.pointSizeF() - 1)); painter->setFont(font);
        painter->setPen(QColor(dark ? "#AFB8B2" : "#64776B"));
        const QRect detail(card.left() + 12, card.top() + 167, card.width() - 52, 20);
        const QString metadata = index.siblingAtColumn(1).data().toString();
        painter->drawText(detail, Qt::AlignVCenter | Qt::AlignLeft, painter->fontMetrics().elidedText(metadata, Qt::ElideRight, detail.width()));
        UDriveIcons::icon(UDriveIcons::Symbol::MoreVertical).paint(painter, moreRect(option.rect));
        if (selected || (option.state & QStyle::State_HasFocus)) {
            const QRect check(card.left() + 10, card.top() + 10, 18, 18);
            painter->setPen(QPen(QColor("#40D979"), 1.5)); painter->setBrush(selected ? QColor("#40D979") : Qt::transparent);
            painter->drawRoundedRect(check, 4, 4);
            if (selected) {
                painter->setPen(QPen(QColor("#143B20"), 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter->drawLine(check.topLeft() + QPoint(4, 9), check.topLeft() + QPoint(8, 13));
                painter->drawLine(check.topLeft() + QPoint(8, 13), check.topLeft() + QPoint(14, 5));
            }
        }
        painter->restore();
    }
    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option, const QModelIndex &index) override
    {
        if (event->type() == QEvent::MouseButtonRelease) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton && moreRect(option.rect).adjusted(-8, -6, 8, 6).contains(mouse->position().toPoint())) {
                _more(index); return true;
            }
        }
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }
private:
    static QRect moreRect(const QRect &rect) { return QRect(rect.right() - 35, rect.top() + 172, 18, 18); }
    std::function<void(const QModelIndex &)> _more;
};

class FilesProxy : public QSortFilterProxyModel
{
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    void setRecent(bool recent) { _recent = recent; invalidate(); }
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override
    {
        const bool leftDirectory = left.siblingAtColumn(0).data(DirectoryRole).toBool();
        const bool rightDirectory = right.siblingAtColumn(0).data(DirectoryRole).toBool();
        if (leftDirectory != rightDirectory)
            return sortOrder() == Qt::AscendingOrder ? leftDirectory : rightDirectory;
        if (_recent) {
            const auto a = left.siblingAtColumn(0).data(OpenedRole).toLongLong();
            const auto b = right.siblingAtColumn(0).data(OpenedRole).toLongLong();
            if (a != b) return a < b;
        }
        if (left.column() == 1 || left.column() == 2) {
            const bool aKnown = left.column() == 1 ? left.data(SortRole).toLongLong() >= 0 : left.data(SortRole).toDateTime().isValid();
            const bool bKnown = right.column() == 1 ? right.data(SortRole).toLongLong() >= 0 : right.data(SortRole).toDateTime().isValid();
            if (aKnown != bKnown) return sortOrder() == Qt::AscendingOrder ? aKnown : bKnown;
        }
        if (left.data(SortRole) == right.data(SortRole))
            return QString::compare(left.siblingAtColumn(0).data().toString(), right.siblingAtColumn(0).data().toString(), Qt::CaseInsensitive) < 0;
        return QSortFilterProxyModel::lessThan(left, right);
    }
private:
    bool _recent = false;
};
}

UDriveFileBrowser::UDriveFileBrowser(AccountState *state, QWidget *parent)
    : QWidget(parent)
    , _state(state)
{
    setObjectName(QStringLiteral("udriveFileBrowser"));

#ifdef Q_OS_MACOS
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    _pages = new QStackedWidget(this);
    outer->addWidget(_pages);
    auto *files = new QWidget(_pages);
    _pages->addWidget(files);
    _pdfPreview = new UDrivePdfPreview(_pages);
    _pages->addWidget(_pdfPreview);
    connect(_pdfPreview, &UDrivePdfPreview::backRequested, this, [this] {
        closePdf();
        activeView()->setFocus();
    });
    auto *layout = new QVBoxLayout(files);
#else
    auto *layout = new QVBoxLayout(this);
#endif
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(10);

    auto *top = new QHBoxLayout;
    _search = new QLineEdit(this);
    _search->setObjectName(QStringLiteral("driveSearch"));
    _search->setPlaceholderText(tr("Search this folder"));
    _search->setAccessibleName(tr("Search this folder"));
    _search->setClearButtonEnabled(true);
    top->addWidget(_search, 1);
    _newButton = new QPushButton(tr("New"), this);
    _newButton->setObjectName(QStringLiteral("driveNew"));
    _newButton->setProperty("udrivePrimary", true);
    _newButton->setProperty("udriveMenuButton", true);
    _newButton->setMinimumWidth(116);
    _newButton->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Add, true));
    auto *newMenu = new QMenu(_newButton);
    _newFolderAction = newMenu->addAction(tr("New folder"));
    _uploadAction = newMenu->addAction(tr("Upload file…"));
    _newButton->setMenu(newMenu);
    top->addWidget(_newButton);
    _onlineButton = new QPushButton(tr("View online"), this);
    _onlineButton->setObjectName(QStringLiteral("driveOnline"));
    _onlineButton->setProperty("driveToolbarButton", true);
    _onlineButton->setFixedHeight(40);
    _onlineButton->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Online));
    // Keep View online next to the section tools.
    _onlineButton->setToolTip(tr("Open this space in your web browser"));
    layout->addLayout(top);

    _heading = new QLabel(tr("Personal"), this);
    _heading->setObjectName(QStringLiteral("driveHeading"));
    _heading->setTextFormat(Qt::PlainText);
    auto font = _heading->font();
    font.setPointSize(13);
    font.setBold(true);
    _heading->setFont(font);

    _spaces = new QComboBox(this);
    _spaces->setObjectName(QStringLiteral("driveSpaces"));
    _spaces->setProperty("udriveCombo", true);
    _spaces->setAccessibleName(tr("Choose a space"));
    _spaces->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    _refreshButton = new QPushButton(tr("Refresh"), this);
    _refreshButton->setObjectName(QStringLiteral("driveRefresh"));
    _refreshButton->setProperty("driveToolbarButton", true);
    _refreshButton->setAccessibleName(tr("Refresh"));
    _refreshButton->setToolTip(tr("Refresh"));
    _refreshButton->setText(QString());
    _refreshButton->setFixedSize(40, 40);
    _refreshButton->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Refresh));
    auto *headingRow = new QHBoxLayout;
    headingRow->setSpacing(8);
    headingRow->setAlignment(Qt::AlignVCenter);
    headingRow->addWidget(_heading, 1);
    auto *sortLabel = new QLabel(tr("Sort by"), this);
    _sortButton = new QPushButton(this);
    _sortButton->setObjectName(QStringLiteral("driveSort"));
    _sortButton->setProperty("driveToolbarButton", true);
    _sortButton->setFixedHeight(40);
    sortLabel->setBuddy(_sortButton);
    auto *sortMenu = new QMenu(_sortButton);
    auto *sortGroup = new QActionGroup(sortMenu);
    sortGroup->setExclusive(true);
    const QStringList sortLabels{tr("Recent"), tr("A-Z"), tr("Z-A"), tr("Newest"), tr("Oldest"), tr("Largest"), tr("Smallest")};
    const QStringList sortIds{QStringLiteral("recent"), QStringLiteral("nameAscending"), QStringLiteral("nameDescending"),
        QStringLiteral("newest"), QStringLiteral("oldest"), QStringLiteral("largest"), QStringLiteral("smallest")};
    for (int i = 0; i < sortLabels.size(); ++i) {
        auto *action = sortMenu->addAction(sortLabels.at(i));
        action->setObjectName(QStringLiteral("driveSort_") + sortIds.at(i));
        action->setData(sortIds.at(i));
        action->setCheckable(true);
        sortGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, i] { setSortMode(SortMode(i)); });
    }
    _sortButton->setMenu(sortMenu);
    headingRow->addWidget(sortLabel);
    headingRow->addWidget(_sortButton);
    headingRow->addWidget(_onlineButton);
    headingRow->addWidget(_refreshButton);
    _viewButton = new QPushButton(this);
    _viewButton->setObjectName(QStringLiteral("driveViewMode"));
    _viewButton->setProperty("driveToolbarButton", true);
    _viewButton->setAccessibleName(tr("Change file view"));
    _viewButton->setToolTip(tr("Change file view"));
    _viewButton->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::List));
    _viewButton->setIconSize(QSize(18, 18));
    _viewButton->setFixedSize(56, 40);
    auto *viewMenu = new QMenu(_viewButton);
    auto *viewGroup = new QActionGroup(viewMenu); viewGroup->setExclusive(true);
    const QList<QPair<QString, ViewMode>> modes{{tr("Condensed table view"), ViewMode::Condensed}, {tr("Default table view"), ViewMode::Default}, {tr("Tiles view"), ViewMode::Tiles}};
    for (const auto &mode : modes) {
        auto *action = viewMenu->addAction(UDriveIcons::icon(mode.second == ViewMode::Tiles ? UDriveIcons::Symbol::Grid : UDriveIcons::Symbol::List), mode.first);
        action->setObjectName(mode.second == ViewMode::Tiles ? QStringLiteral("driveViewTiles") : mode.second == ViewMode::Condensed ? QStringLiteral("driveViewCondensed") : QStringLiteral("driveViewDefault"));
        action->setData(int(mode.second)); action->setCheckable(true); viewGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, mode] { setViewMode(mode.second); });
    }
    _viewButton->setMenu(viewMenu);
    headingRow->addWidget(_viewButton);
    layout->addLayout(headingRow);
    layout->addWidget(_spaces);

    _breadcrumbs = new QWidget(this);
    _breadcrumbs->setObjectName(QStringLiteral("driveBreadcrumbs"));
    _breadcrumbs->setAccessibleName(tr("Folder path"));
    _breadcrumbLayout = new QHBoxLayout(_breadcrumbs);
    _breadcrumbLayout->setContentsMargins(0, 0, 0, 0);
    _breadcrumbLayout->setSpacing(4);
    _rootButton = new QPushButton(tr("Personal"), _breadcrumbs);
    _rootButton->setObjectName(QStringLiteral("driveRoot"));
    _rootButton->setAccessibleName(tr("Open space root"));
    _rootButton->setProperty("udriveBreadcrumb", true);
    _rootButton->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Folder));
    _rootButton->setIconSize(QSize(18, 18));
    layout->addWidget(_breadcrumbs);

    _model = new QStandardItemModel(this);
    _model->setHorizontalHeaderLabels({tr("Name"), tr("Size"), tr("Modified"), tr("Location"), tr("Actions")});
    _proxy = new FilesProxy(this);
    _proxy->setSourceModel(_model);
    _proxy->setFilterKeyColumn(0);
    _proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    _proxy->setSortRole(SortRole);
    _proxy->setSortCaseSensitivity(Qt::CaseInsensitive);
    _tree = new QTreeView(this);
    _tree->setObjectName(QStringLiteral("driveFiles"));
    _tree->setAccessibleName(tr("Files in this folder"));
    _tree->setModel(_proxy);
    _tree->setRootIsDecorated(false);
    _tree->setUniformRowHeights(true);
    _tree->setAlternatingRowColors(false);
    _tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    _tree->setSelectionMode(QAbstractItemView::SingleSelection);
    _tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _tree->setSortingEnabled(true);
    _tree->sortByColumn(0, Qt::AscendingOrder);
    connect(_tree->header(), &QHeaderView::sortIndicatorChanged, this, [this](int column, Qt::SortOrder order) {
        if (column == 0) setSortMode(order == Qt::AscendingOrder ? SortMode::NameAscending : SortMode::NameDescending);
        else if (column == 1) setSortMode(order == Qt::AscendingOrder ? SortMode::Smallest : SortMode::Largest);
        else if (column == 2) setSortMode(order == Qt::AscendingOrder ? SortMode::Oldest : SortMode::Newest);
        else setSortMode(_sortMode);
    });
    _tree->header()->setStretchLastSection(false);
    _tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    _tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    _tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    _tree->header()->setSectionResizeMode(3, QHeaderView::Interactive);
    _tree->setColumnHidden(3, true);
    _tree->header()->setSectionResizeMode(4, QHeaderView::Fixed);
    _tree->header()->resizeSection(4, 120);
    _tree->header()->setMinimumSectionSize(70);
    _tree->setMouseTracking(true);
    _tree->setItemDelegate(new FileRowDelegate(_tree, [this](const QModelIndex &index, int action) {
        _tree->setCurrentIndex(index);
        updateActions();
        if (action == 0 && index.data(FavoriteRole).toBool() && _favoriteAction->isEnabled())
            _favoriteAction->trigger();
        else if (action == 1 && _linkAction->isEnabled())
            _linkAction->trigger();
        else if (action == 2 && _fileMenu->isEnabled())
            _fileMenu->popup(QCursor::pos());
    }));
    _tiles = new QListView(this);
    _tiles->setObjectName(QStringLiteral("driveTiles"));
    _tiles->setAccessibleName(tr("Files in tiles view"));
    _tiles->setModel(_proxy);
    _tiles->setSelectionModel(_tree->selectionModel());
    _tiles->setModelColumn(0);
    _tiles->setViewMode(QListView::IconMode);
    _tiles->setResizeMode(QListView::Adjust);
    _tiles->setMovement(QListView::Static);
    _tiles->setFlow(QListView::LeftToRight);
    _tiles->setWrapping(true);
    _tiles->setGridSize(QSize(196, 216));
    _tiles->setIconSize(QSize(128, 128));
    _tiles->setUniformItemSizes(true);
    _tiles->setSelectionMode(QAbstractItemView::SingleSelection);
    _tiles->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _tiles->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    _tiles->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _tiles->setMouseTracking(true);
    _tiles->setItemDelegate(new FileTileDelegate(_tiles, [this](const QModelIndex &index) {
        _tiles->setCurrentIndex(index); updateActions();
        if (_fileMenu->isEnabled()) _fileMenu->popup(QCursor::pos());
    }));
    _fileViews = new QStackedWidget(this);
    _fileViews->setObjectName(QStringLiteral("driveFileViews"));
    _fileViews->addWidget(_tree); _fileViews->addWidget(_tiles);
    layout->addWidget(_fileViews, 1);

    auto *actions = new QHBoxLayout;
    _message = new QLabel(this);
    _message->setObjectName(QStringLiteral("driveStatus"));
    _message->setTextFormat(Qt::PlainText);
    _message->setWordWrap(true);
    actions->addWidget(_message, 1);
    _openButton = new QPushButton(tr("Open"), this);
    _openButton->setObjectName(QStringLiteral("driveOpen"));
    _openButton->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Open));
    _downloadButton = new QPushButton(tr("Download"), this);
    _downloadButton->setObjectName(QStringLiteral("driveDownload"));
    _downloadButton->setProperty("udrivePrimary", true);
    _downloadButton->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Download, true));
    actions->addWidget(_openButton);
    actions->addWidget(_downloadButton);
    _fileMenu = new QMenu(this);
    _fileMenu->setObjectName(QStringLiteral("driveActionsMenu"));
    _renameAction = new QAction(tr("Rename…"), this);
    _copyAction = new QAction(tr("Make a copy…"), this);
    _moveAction = new QAction(tr("Move to folder…"), this);
    _deleteAction = new QAction(tr("Move to Deleted files…"), this);
    _restoreAction = new QAction(tr("Restore"), this);
    _favoriteAction = new QAction(tr("Add to Favorites"), this);
    _favoriteAction->setObjectName(QStringLiteral("driveFavorite"));
    _linkAction = new QAction(tr("Copy sign-in link"), this);
    _linkAction->setToolTip(tr("People opening this link must sign in and have access to this file."));
    _newFolderAction->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Folder));
    _uploadAction->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Upload));
    _renameAction->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Rename));
    _copyAction->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Copy));
    _moveAction->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Move));
    _deleteAction->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Trash));
    _restoreAction->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Restore));
    _favoriteAction->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Star));
    _linkAction->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Link));
    addFileActions(_fileMenu);
    layout->addLayout(actions);

    auto *transfer = new QHBoxLayout;
    _progress = new QProgressBar(this);
    _progress->setAccessibleName(tr("Download progress"));
    _cancelButton = new QPushButton(tr("Cancel download"), this);
    _cancelButton->setObjectName(QStringLiteral("driveCancelDownload"));
    _cancelButton->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Close));
    transfer->addWidget(_progress, 1);
    transfer->addWidget(_cancelButton);
    layout->addLayout(transfer);
    _progress->hide();
    _cancelButton->hide();

    for (auto *button : {_newButton, _onlineButton, _refreshButton, _rootButton, _openButton, _downloadButton, _cancelButton})
        button->setIconSize(QSize(20, 20));
    _onlineButton->setIconSize(QSize(18, 18));
    _refreshButton->setIconSize(QSize(18, 18));
    _rootButton->setIconSize(QSize(18, 18));
    _tree->setIconSize(QSize(32, 32));
    _thumbnails = new UDriveThumbnails(_state, this);
    _thumbnailTimer = new QTimer(this);
    _thumbnailTimer->setSingleShot(true);
    _thumbnailTimer->setInterval(40);
    connect(_thumbnailTimer, &QTimer::timeout, this, &UDriveFileBrowser::loadVisibleThumbnails);
    connect(_thumbnails, &UDriveThumbnails::ready, this, [this](const QPersistentModelIndex &index, const QPixmap &pixmap) {
        if (!index.isValid() || index.model() != _model || !canBrowse()) return;
        const auto visible = _proxy->mapFromSource(index);
        auto *view = activeView();
        if (!view->isVisible() || !visible.isValid() || !view->visualRect(visible).intersects(view->viewport()->rect())) return;
        _thumbnailRows.insert(index);
        _model->setData(index, true, ThumbnailRole);
        _model->setData(index, QIcon(pixmap), Qt::DecorationRole);
    });
    _tree->viewport()->installEventFilter(this);
    connect(_tree->verticalScrollBar(), &QScrollBar::valueChanged, this, &UDriveFileBrowser::scheduleThumbnails);
    _tiles->viewport()->installEventFilter(this);
    connect(_tiles->verticalScrollBar(), &QScrollBar::valueChanged, this, &UDriveFileBrowser::scheduleThumbnails);
    connect(_proxy, &QAbstractItemModel::rowsInserted, this, &UDriveFileBrowser::scheduleThumbnails);
    connect(_proxy, &QAbstractItemModel::layoutChanged, this, &UDriveFileBrowser::scheduleThumbnails);
    connect(_proxy, &QAbstractItemModel::modelReset, this, &UDriveFileBrowser::scheduleThumbnails);
    auto *searchAction = _search->addAction(UDriveIcons::icon(UDriveIcons::Symbol::Search), QLineEdit::LeadingPosition);
    searchAction->setText(tr("Search this folder"));
    connect(searchAction, &QAction::triggered, _search.data(), [this] { _search->setFocus(); });

    connect(_search.data(), &QLineEdit::textChanged, _proxy, &QSortFilterProxyModel::setFilterFixedString);
    connect(_search.data(), &QLineEdit::textChanged, this, &UDriveFileBrowser::scheduleThumbnails);
    connect(_spaces, &QComboBox::currentIndexChanged, this, &UDriveFileBrowser::selectSpace);
    connect(_refreshButton, &QPushButton::clicked, this, &UDriveFileBrowser::refresh);
    addAction(tr("Refresh"), QKeySequence::Refresh, this, &UDriveFileBrowser::refresh)->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    addAction(tr("Search"), QKeySequence::Find, this, [this] { _search->setFocus(); })->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(_tree, &QTreeView::activated, this, &UDriveFileBrowser::activateSelection);
    connect(_tiles, &QListView::activated, this, &UDriveFileBrowser::activateSelection);
    connect(_tree->selectionModel(), &QItemSelectionModel::selectionChanged, this, &UDriveFileBrowser::updateActions);
    connect(_openButton, &QPushButton::clicked, this, &UDriveFileBrowser::activateSelection);
    connect(_downloadButton, &QPushButton::clicked, this, &UDriveFileBrowser::downloadSelection);
    connect(_cancelButton, &QPushButton::clicked, this, [this] {
        if (_uploading) {
            cancelOperation();
            refresh();
            setMessage(tr("Upload canceled. Refresh to check the server."));
        } else {
            cancelDownload();
            setMessage(tr("Download canceled."));
        }
    });
    connect(_newFolderAction, &QAction::triggered, this, [this] {
        const QUrl root = _root;
        const QString path = _path;
        const QPointer<UDriveFileBrowser> guard(this);
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("New folder"), tr("Folder name"), QLineEdit::Normal, {}, &ok);
        if (guard && ok && _root == root && _path == path && !createFolder(name))
            setMessage(tr("Enter a valid new folder name."));
    });
    connect(_uploadAction, &QAction::triggered, this, [this] {
        const QUrl root = _root;
        const QString path = _path;
        const QPointer<UDriveFileBrowser> guard(this);
        const QString file = QFileDialog::getOpenFileName(this, tr("Upload file"));
        if (guard && !file.isEmpty() && root == _root && path == _path)
            uploadFile(file);
    });
    auto renameOrCopy = [this](bool copy) {
        const auto item = selectedItem();
        const QString oldPath = item.data(PathRole).toString();
        const QUrl root = selectedRoot();
        const QPointer<UDriveFileBrowser> guard(this);
        bool ok = false;
        const QString name =
            QInputDialog::getText(this, copy ? tr("Make a copy") : tr("Rename"), tr("New name"), QLineEdit::Normal, item.data().toString(), &ok);
        if (guard && ok && root == selectedRoot() && selectedItem().data(PathRole).toString() == oldPath && !(copy ? copySelected(name) : renameSelected(name)))
            setMessage(tr("Choose a different, valid name."));
    };
    connect(_renameAction, &QAction::triggered, this, [renameOrCopy] { renameOrCopy(false); });
    connect(_copyAction, &QAction::triggered, this, [renameOrCopy] { renameOrCopy(true); });
    connect(_moveAction, &QAction::triggered, this, [this] {
        const QString oldPath = selectedItem().data(PathRole).toString();
        const QUrl root = selectedRoot();
        const QPointer<UDriveFileBrowser> guard(this);
        bool ok = false;
        const QString path =
            QInputDialog::getText(this, tr("Move to folder"), tr("Destination folder in this space (leave empty for the root)"), QLineEdit::Normal, {}, &ok);
        if (guard && ok && root == selectedRoot() && selectedItem().data(PathRole).toString() == oldPath && !moveSelected(path))
            setMessage(tr("Choose a valid destination folder. Existing files will not be overwritten."));
    });
    connect(_deleteAction, &QAction::triggered, this, [this] {
        const QString path = selectedItem().data(PathRole).toString();
        const QUrl root = selectedRoot();
        const QPointer<UDriveFileBrowser> guard(this);
        const auto answer = QMessageBox::question(this, tr("Move to Deleted files"),
            tr("Move “%1” to Deleted files? A folder includes all of its contents.").arg(selectedItem().data().toString()),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (guard && answer == QMessageBox::Yes && root == selectedRoot() && path == selectedItem().data(PathRole).toString())
            deleteSelected();
    });
    connect(_restoreAction, &QAction::triggered, this, &UDriveFileBrowser::restoreSelected);
    connect(_favoriteAction, &QAction::triggered, this, [this] { setSelectedFavorite(!selectedItem().data(FavoriteRole).toBool()); });
    connect(_linkAction, &QAction::triggered, this, [this] {
        if (_linkAction->isEnabled()) {
            QApplication::clipboard()->setText(selectedItem().data(LinkRole).toUrl().toString());
            setMessage(tr("Sign-in link copied. Access permissions still apply."));
        }
    });
    connect(_rootButton, &QPushButton::clicked, this, [this] { navigateToBreadcrumb({}, _root); });
    connect(_onlineButton, &QPushButton::clicked, this, [this] {
        if (_webUrl.scheme() == QStringLiteral("https") || _webUrl.scheme() == QStringLiteral("http"))
            QDesktopServices::openUrl(_webUrl);
    });
    connect(FolderMan::instance(), &FolderMan::folderListChanged, this, &UDriveFileBrowser::updateActions);
    connect(FolderMan::instance(), &FolderMan::folderSyncStateChange, this, &UDriveFileBrowser::updateActions);
    for (auto *view : {static_cast<QAbstractItemView *>(_tree), static_cast<QAbstractItemView *>(_tiles)}) {
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view, &QAbstractItemView::customContextMenuRequested, this, [this, view](const QPoint &position) {
        const auto index = view->indexAt(position);
        if (!index.isValid())
            return;
        view->setCurrentIndex(index);
        QMenu menu(this);
        auto *open = menu.addAction(_openButton->icon(), _openButton->text(), this, &UDriveFileBrowser::activateSelection);
        open->setEnabled(_openButton->isEnabled());
        auto *download = menu.addAction(UDriveIcons::icon(UDriveIcons::Symbol::Download), tr("Download"), this, &UDriveFileBrowser::downloadSelection);
        download->setEnabled(_downloadButton->isEnabled());
        menu.addSeparator();
        addFileActions(&menu);
        menu.exec(view->viewport()->mapToGlobal(position));
    });
    }

    if (_state && _state->account()) {
        auto *manager = _state->account()->spacesManager();
        connect(manager, &GraphApi::SpacesManager::updated, this, &UDriveFileBrowser::updateSpaces);
        connect(_state, &AccountState::stateChanged, this, [this] {
            if (!canBrowse()) {
                cancelListing();
                cancelDownload();
                closePdf();
                cancelOperation();
                _model->removeRows(0, _model->rowCount());
                setMessage(tr("Sign in to browse your files. You can open synced folders from Sync."));
                updateActions();
            } else {
                updateSpaces();
                if (!_root.isEmpty() && !_listing)
                    refresh();
            }
        });
        connect(_state, &QObject::destroyed, this, [this] {
            cancelListing();
            cancelDownload();
            closePdf();
            cancelOperation();
            _model->removeRows(0, _model->rowCount());
            _root.clear();
            updateActions();
        });
    }
    const QString savedView = ConfigFile::settingsWithGroup(QStringLiteral("UDriveFileBrowser"))->value(QStringLiteral("viewMode"), QStringLiteral("default")).toString();
    setViewMode(savedView == QStringLiteral("tiles") ? ViewMode::Tiles : savedView == QStringLiteral("condensed") ? ViewMode::Condensed : ViewMode::Default, false);
    _recentFiles = ConfigFile::settingsWithGroup(recentSettingsGroup())->value(QStringLiteral("opened")).toMap();
    const qint64 latestAllowed = QDateTime::currentMSecsSinceEpoch() + 60000;
    for (auto it = _recentFiles.begin(); it != _recentFiles.end();) {
        if (it.key().size() != 64 || it.value().toLongLong() <= 0 || it.value().toLongLong() > latestAllowed) it = _recentFiles.erase(it);
        else ++it;
    }
    trimRecentFiles(_recentFiles);
    const auto savedSort = ConfigFile::settingsWithGroup(QStringLiteral("UDriveFileBrowser"))->value(QStringLiteral("sortMode"), QStringLiteral("nameAscending")).toString();
    const auto sortActions = _sortButton->menu()->actions();
    int sortIndex = 1;
    for (int i = 0; i < sortActions.size(); ++i) if (sortActions.at(i)->data().toString() == savedSort) sortIndex = i;
    setSortMode(SortMode(sortIndex), false);
    updateSpaces();
}

UDriveFileBrowser::~UDriveFileBrowser()
{
    cancelListing();
    cancelDownload();
    closePdf();
    cancelOperation();
}

QAbstractItemView *UDriveFileBrowser::activeView() const
{
    return _viewMode == ViewMode::Tiles ? static_cast<QAbstractItemView *>(_tiles) : _tree;
}

void UDriveFileBrowser::setViewMode(ViewMode mode, bool save)
{
    const auto selected = selectedItem();
    closePdf();
    _thumbnails->cancel();
    for (const auto &index : _thumbnailRows) if (index.isValid()) {
        _model->setData(index, false, ThumbnailRole);
        _model->setData(index, UDriveIcons::fileIcon(index.data().toString(), false), Qt::DecorationRole);
    }
    _thumbnailRows.clear();
    _viewMode = mode;
    _thumbnails->setPixels(mode == ViewMode::Tiles ? 256 : 64);
    _tree->setProperty("driveCondensed", mode == ViewMode::Condensed);
    _tree->setIconSize(mode == ViewMode::Condensed ? QSize(20, 20) : QSize(32, 32));
    _tree->doItemsLayout();
    _fileViews->setCurrentWidget(mode == ViewMode::Tiles ? static_cast<QWidget *>(_tiles) : _tree);
    _viewButton->setIcon(UDriveIcons::icon(mode == ViewMode::Tiles ? UDriveIcons::Symbol::Grid : UDriveIcons::Symbol::List));
    for (auto *action : _viewButton->menu()->actions()) action->setChecked(action->data().toInt() == int(mode));
    if (save) {
        auto settings = ConfigFile::settingsWithGroup(QStringLiteral("UDriveFileBrowser"));
        settings->setValue(QStringLiteral("viewMode"), mode == ViewMode::Tiles ? QStringLiteral("tiles") : mode == ViewMode::Condensed ? QStringLiteral("condensed") : QStringLiteral("default"));
    }
    if (selected.isValid()) activeView()->scrollTo(selected);
    activeView()->setFocus();
    scheduleThumbnails();
}

void UDriveFileBrowser::addFileActions(QMenu *menu)
{
    menu->addActions({_favoriteAction, _linkAction, _renameAction, _copyAction, _moveAction, _restoreAction, _deleteAction});
}

void UDriveFileBrowser::setSortMode(SortMode mode, bool save)
{
    _sortMode = mode;
    const bool date = mode == SortMode::Recent || mode == SortMode::Newest || mode == SortMode::Oldest;
    const int column = date ? 2 : mode == SortMode::Largest || mode == SortMode::Smallest ? 1 : 0;
    const auto order = mode == SortMode::NameAscending || mode == SortMode::Oldest || mode == SortMode::Smallest
        ? Qt::AscendingOrder : Qt::DescendingOrder;
    static_cast<FilesProxy *>(_proxy)->setRecent(mode == SortMode::Recent);
    const QSignalBlocker blocker(_tree->header());
    _tree->sortByColumn(column, order);
    _proxy->sort(column, order);
    const auto actions = _sortButton->menu()->actions();
    for (int i = 0; i < actions.size(); ++i) actions.at(i)->setChecked(i == int(mode));
    const auto *action = actions.at(int(mode));
    _sortButton->setText(action->text());
    _sortButton->setAccessibleName(tr("Sort by: %1").arg(action->text()));
    _sortButton->setToolTip(mode == SortMode::Recent ? tr("Files opened most recently in UDrive; unopened files follow by modified date.")
                                                  : tr("Sort by: %1").arg(action->text()));
    if (save) ConfigFile::settingsWithGroup(QStringLiteral("UDriveFileBrowser"))->setValue(QStringLiteral("sortMode"), action->data());
    const auto selected = selectedItem();
    if (selected.isValid()) activeView()->scrollTo(selected);
    scheduleThumbnails();
}

QString UDriveFileBrowser::recentSettingsGroup() const
{
    return QStringLiteral("UDriveFileBrowserRecent/") + (_state && _state->account() ? _state->account()->uuid().toString(QUuid::WithoutBraces) : QStringLiteral("none"));
}

void UDriveFileBrowser::recordOpened(const QUrl &root, const QString &path)
{
    if (!canBrowse()) return;
    const QString key = recentKey(root, path);
    qint64 timestamp = QDateTime::currentMSecsSinceEpoch();
    for (auto it = _recentFiles.cbegin(); it != _recentFiles.cend(); ++it) timestamp = std::max(timestamp, it.value().toLongLong() + 1);
    _recentFiles.insert(key, timestamp);
    // Store only bounded, account-scoped hashes, never file names or token values.
    trimRecentFiles(_recentFiles);
    ConfigFile::settingsWithGroup(recentSettingsGroup())->setValue(QStringLiteral("opened"), _recentFiles);
    for (int row = 0; row < _model->rowCount(); ++row) {
        const auto index = _model->index(row, 0);
        _model->setData(index, _recentFiles.value(recentKey(index.data(RootRole).toUrl(), index.data(PathRole).toString()), qint64(0)), OpenedRole);
    }
    // Recent sorts the date column but its primary value lives in the name column.
    if (_sortMode == SortMode::Recent) static_cast<FilesProxy *>(_proxy)->setRecent(true);
}

bool UDriveFileBrowser::canBrowse() const
{
    return _state && _state->account() && !_state->isSignedOut()
        && _state->account()->credentials() && _state->account()->credentials()->ready();
}

void UDriveFileBrowser::showPersonal()
{
    setSection(Section::Personal);
}

void UDriveFileBrowser::showWorkspaces()
{
    setSection(Section::Workspaces);
}

void UDriveFileBrowser::showShares() { setSection(Section::Shares); }
void UDriveFileBrowser::showFavorites() { setSection(Section::Favorites); }
void UDriveFileBrowser::showTrash() { setSection(Section::Trash); }

void UDriveFileBrowser::setSection(Section section)
{
    closePdf();
    const bool changed = _section != section;
    _section = section;
    if (changed) {
        cancelListing(); cancelDownload();
        if (_uploading) cancelOperation();
        _spaceId.clear();
        _path.clear();
        updateSpaces();
        Q_EMIT sectionChanged(section);
    }
}

void UDriveFileBrowser::navigateToBreadcrumb(const QString &path, const QUrl &root)
{
    if (!canBrowse() || _operation || _root != root || _section == Section::Favorites || _section == Section::Trash
        || path == _path || (!path.isEmpty() && (!validPath(path) || !_path.startsWith(path + QLatin1Char('/'))))) return;
    cancelDownload(); closePdf();
    _path = path;
    refresh();
    activeView()->setFocus();
}

void UDriveFileBrowser::updateBreadcrumbs()
{
    while (auto *item = _breadcrumbLayout->takeAt(0)) {
        if (auto *widget = item->widget(); widget && widget != _rootButton) {
            widget->hide(); widget->deleteLater();
        }
        delete item;
    }
    _breadcrumbs->setVisible(_section != Section::Favorites && _section != Section::Trash && (!_workspaceLayout || !_path.isEmpty()));
    const QString rootName = _spaceName.isEmpty() ? _heading->text() : _spaceName;
    _rootButton->setText(_rootButton->fontMetrics().elidedText(rootName, Qt::ElideMiddle, 140).replace(QLatin1Char('&'), QStringLiteral("&&")));
    _rootButton->setToolTip(rootName);
    _rootButton->setProperty("breadcrumbCurrent", _path.isEmpty());
    _rootButton->style()->unpolish(_rootButton); _rootButton->style()->polish(_rootButton);
    _breadcrumbLayout->addWidget(_rootButton);
    const auto parts = _path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    auto separator = [this] {
        auto *label = new QLabel(QStringLiteral("›"), _breadcrumbs);
        label->setObjectName(QStringLiteral("driveBreadcrumbSeparator"));
        label->setTextFormat(Qt::PlainText);
        label->setAlignment(Qt::AlignCenter);
        label->setFixedWidth(16);
        _breadcrumbLayout->addWidget(label);
    };
    const QUrl root = _root;
    if (parts.size() > 2) {
        separator();
        auto *more = new QPushButton(QStringLiteral("…"), _breadcrumbs);
        more->setObjectName(QStringLiteral("driveBreadcrumbOverflow"));
        more->setProperty("udriveBreadcrumb", true);
        more->setAccessibleName(tr("Earlier parent folders"));
        more->setToolTip(tr("Earlier parent folders"));
        auto *menu = new QMenu(more);
        for (int i = 0; i < parts.size() - 2; ++i) {
            const QString path = parts.mid(0, i + 1).join(QLatin1Char('/'));
            auto *action = menu->addAction(UDriveIcons::icon(UDriveIcons::Symbol::Folder),
                QString(parts.at(i)).replace(QLatin1Char('&'), QStringLiteral("&&")));
            action->setToolTip(path);
            connect(action, &QAction::triggered, this, [this, path, root] { navigateToBreadcrumb(path, root); });
        }
        more->setMenu(menu);
        _breadcrumbLayout->addWidget(more);
    }
    for (int i = qMax(0, int(parts.size()) - 2); i < parts.size(); ++i) {
        separator();
        const QString path = parts.mid(0, i + 1).join(QLatin1Char('/'));
        if (i == parts.size() - 1) {
            auto *current = new BreadcrumbLabel(parts.at(i), _breadcrumbs);
            current->setObjectName(QStringLiteral("driveCurrentFolder"));
            current->setAccessibleDescription(path);
            _breadcrumbLayout->addWidget(current, 1);
        } else {
            auto *button = new QPushButton(_breadcrumbs);
            button->setText(button->fontMetrics().elidedText(parts.at(i), Qt::ElideMiddle, 160).replace(QLatin1Char('&'), QStringLiteral("&&")));
            button->setObjectName(QStringLiteral("driveBreadcrumb"));
            button->setProperty("udriveBreadcrumb", true);
            button->setAccessibleName(parts.at(i)); button->setToolTip(path);
            button->setProperty("breadcrumbPath", path);
            connect(button, &QPushButton::clicked, this, [this, path, root] { navigateToBreadcrumb(path, root); });
            _breadcrumbLayout->addWidget(button);
        }
    }
    if (parts.isEmpty()) _breadcrumbLayout->addStretch();
}

void UDriveFileBrowser::updateSpaces()
{
    switch (_section) {
    case Section::Personal: _heading->setText(tr("Personal")); break;
    case Section::Workspaces: _heading->setText(tr("Workspaces")); break;
    case Section::Shares: _heading->setText(tr("Shares")); break;
    case Section::Favorites: _heading->setText(tr("Favorites")); break;
    case Section::Trash: _heading->setText(tr("Deleted files")); break;
    }
    const bool folderView = _section != Section::Favorites && _section != Section::Trash;
    _newButton->setVisible(folderView && !_workspaceLayout);
    _breadcrumbs->setVisible(folderView && (!_workspaceLayout || !_path.isEmpty()));
    const QString searchHint = _section == Section::Favorites ? tr("Search Favorites")
        : _section == Section::Trash ? tr("Search Deleted files") : tr("Search this folder");
    _search->setPlaceholderText(searchHint);
    _search->setAccessibleName(searchHint);
    _tree->setColumnHidden(3, _section != Section::Favorites && _section != Section::Trash);
    _model->setHeaderData(2, Qt::Horizontal, _section == Section::Trash ? tr("Deleted") : tr("Modified"));
    const QSignalBlocker blocker(_spaces);
    const QString previous = _spaceId;
    _spaces->clear();
    if (_state && _state->account()) {
        auto spaces = _state->account()->spacesManager()->spaces();
        std::sort(spaces.begin(), spaces.end(), [](const auto *a, const auto *b) { return a->displayName().localeAwareCompare(b->displayName()) < 0; });
        for (auto *space : spaces) {
            const bool personal = space->sortPriority() == 100;
            const bool shares = space->sortPriority() == 50;
            if (space->disabled() || (_section == Section::Personal && !personal)
                || (_section == Section::Workspaces && (personal || shares)) || (_section == Section::Shares && !shares)) continue;
            _spaces->addItem(space->displayName(), space->id());
            _spaces->setItemData(_spaces->count() - 1, space->webDavUrl(), Qt::UserRole + 1);
            _spaces->setItemData(_spaces->count() - 1, space->webUrl(), Qt::UserRole + 2);
            _spaces->setItemData(_spaces->count() - 1, space->driveId(), Qt::UserRole + 3);
        }
    }
    const int previousIndex = _spaces->findData(previous);
    _spaces->setCurrentIndex(previousIndex < 0 ? (_spaces->count() ? 0 : -1) : previousIndex);
    // A single space is selected automatically; only show a selector when
    // there is a real choice. Favorites already spans all spaces.
    _spaces->setVisible(_section != Section::Favorites && _spaces->count() > 1);
    _spaces->setEnabled(canBrowse() && _spaces->count());
    selectSpace();
}

void UDriveFileBrowser::selectSpace()
{
    const QString id = _spaces->currentData().toString();
    QUrl root = _spaces->currentData(Qt::UserRole + 1).toUrl();
    // DAV collection responses may omit the trailing slash in their self href.
    root.setPath(withoutTrailingSlash(root.path()));
    _webUrl = _spaces->currentData(Qt::UserRole + 2).toUrl();
    _driveId = _spaces->currentData(Qt::UserRole + 3).toString();
    const QString name = _spaces->currentText().isEmpty() ? _heading->text() : _spaces->currentText();
    _spaceName = name;
    if (id == _spaceId && root == _root && !id.isEmpty()) {
        updateBreadcrumbs();
        updateActions();
        return;
    }
    cancelListing();
    closePdf();
    // An in-progress save belongs to the space in which it was started.
    cancelDownload();
    if (_uploading) cancelOperation();
    _spaceId = id;
    _root = root;
    _path.clear();
    _search->clear();
    refresh();
}

void UDriveFileBrowser::cancelListing()
{
    _thumbnailTimer->stop();
    _thumbnails->cancel();
    _thumbnailRows.clear();
    auto job = _listing;
    _listing.clear();
    if (job) job->abort();
    auto report = _report;
    _report.clear();
    if (report) report->abort();
}

void UDriveFileBrowser::scheduleThumbnails()
{
    if (isVisible() && canBrowse() && _section != Section::Trash) _thumbnailTimer->start();
}

bool UDriveFileBrowser::eventFilter(QObject *watched, QEvent *event)
{
    if ((watched == _tree->viewport() || watched == _tiles->viewport()) && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) scheduleThumbnails();
    return QWidget::eventFilter(watched, event);
}

void UDriveFileBrowser::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    scheduleThumbnails();
}

void UDriveFileBrowser::loadVisibleThumbnails()
{
    auto *view = activeView();
    if (!isVisible() || !view->isVisible() || !canBrowse() || _section == Section::Trash) return;
    // Row icons also own pixmaps. Drop offscreen copies so scrolling a huge
    // folder cannot bypass the bounded thumbnail cache.
    for (auto it = _thumbnailRows.begin(); it != _thumbnailRows.end();) {
        const auto visible = _proxy->mapFromSource(*it);
        if (!it->isValid() || !visible.isValid() || !view->visualRect(visible).intersects(view->viewport()->rect())) {
            if (it->isValid()) {
                _model->setData(*it, false, ThumbnailRole);
                _model->setData(*it, UDriveIcons::fileIcon(it->data().toString(), false), Qt::DecorationRole);
            }
            it = _thumbnailRows.erase(it);
        } else ++it;
    }
    QModelIndex first;
    // Grid cells have gaps; find the first visible cell rather than scanning
    // every earlier file when a large folder is scrolled down.
    for (int y = 0; !first.isValid() && y < qMin(216, view->viewport()->height()); y += 8)
        for (int x = 8; !first.isValid() && x < view->viewport()->width(); x += 24) first = view->indexAt(QPoint(x, y));
    const int start = first.isValid() ? first.row() : 0;
    for (int row = start; row < _proxy->rowCount(); ++row) {
        const auto index = _proxy->index(row, 0);
        const QRect rect = view->visualRect(index);
        if (rect.top() >= view->viewport()->height()) break;
        if (rect.bottom() < 0 || index.data(DirectoryRole).toBool() || index.data(ThumbnailRole).toBool()
            || !UDriveThumbnails::supports(index.data().toString())) continue;
        _thumbnails->request(_proxy->mapToSource(index), index.data(RootRole).toUrl(), index.data(PathRole).toString(), index.data(EtagRole).toString());
    }
}

void UDriveFileBrowser::refresh()
{
    cancelListing();
    _model->removeRows(0, _model->rowCount());
    updateBreadcrumbs();
    _search->clear();
    _folderPermissions.clear();
    _folderPermissionsKnown = false;
    updateActions();
    if (!canBrowse()) {
        setMessage(tr("Sign in to browse your files. You can open synced folders from Sync."));
        return;
    }
    if (_section == Section::Favorites) { refreshFavorites(); return; }
    if (_root.isEmpty()) {
        setMessage(_state->account()->spacesManager()->isReady() ? tr("No spaces are available in this section.")
                                                                 : tr("Waiting for your spaces. Use Sync to check your connection."));
        return;
    }
    if (!LocalDevelopment::allowsAuthenticatedUrl(_root, _state->account()->url()) || !_root.query().isEmpty() || !_root.fragment().isEmpty()) {
        setMessage(tr("The server returned an invalid space address."));
        return;
    }
    setMessage(tr("Loading files…"));
    const QUrl requestRoot = _section == Section::Trash ? trashRoot() : _root;
    if (requestRoot.isEmpty()) {
        setMessage(tr("Deleted files are not available for this space."));
        return;
    }
    auto *job = new PropfindJob(_state->account(), requestRoot, _section == Section::Trash ? QString() : _path, PropfindJob::Depth::One, this);
    _listing = job;
    job->setTimeout(std::chrono::seconds(30));
    job->setProperties({"resourcetype", "getcontentlength", "getlastmodified", "getetag",
        "http://owncloud.org/ns:permissions", "http://owncloud.org/ns:favorite", "http://owncloud.org/ns:privatelink",
        "http://owncloud.org/ns:trashbin-original-filename", "http://owncloud.org/ns:trashbin-original-location", "http://owncloud.org/ns:trashbin-delete-datetime"});
    connect(job, &PropfindJob::directoryListingIterated, this, [this, job](const QString &href, const QMap<QString, QString> &properties) {
        if (_listing == job) appendEntry(href, properties);
    });
    connect(job, &PropfindJob::finishedWithoutError, this, [this, job] {
        if (_listing != job) return;
        _listing.clear();
        setMessage(_model->rowCount() ? tr("%n item(s)", nullptr, _model->rowCount()) : tr("This folder is empty."));
        updateActions();
        scheduleThumbnails();
    });
    connect(job, &PropfindJob::finishedWithError, this, [this, job] {
        if (_listing != job) return;
        _listing.clear();
        _model->removeRows(0, _model->rowCount());
        const QString error = job->errorString();
        setMessage(error.isEmpty() ? tr("The server did not return a valid folder listing.") : tr("Could not load this folder: %1").arg(error));
        updateActions();
    });
    job->start();
    updateActions();
}

void UDriveFileBrowser::appendEntry(const QString &href, const QMap<QString, QString> &properties,
    const QUrl &entryRoot, const QString &entrySpace, bool nested)
{
    const QUrl root = entryRoot.isEmpty() ? _root : entryRoot;
    const bool trash = _section == Section::Trash;
    const QUrl responseRoot = trash ? trashRoot() : root;
    const QString current = withoutTrailingSlash(Utility::concatUrlPath(responseRoot, trash || nested ? QString() : _path).path());
    if (withoutTrailingSlash(href) == current) {
        _folderPermissions = properties.value(QStringLiteral("permissions"));
        _folderPermissionsKnown = properties.contains(QStringLiteral("permissions"));
        return;
    }
    const QString prefix = withoutTrailingSlash(responseRoot.path()) + QLatin1Char('/');
    if (!href.startsWith(prefix)) return;
    const QString relative = withoutTrailingSlash(href.mid(prefix.size()));
    if (!validPath(relative)) return;
    const QString directoryPrefix = nested || trash || _path.isEmpty() ? QString() : _path + QLatin1Char('/');
    if (!relative.startsWith(directoryPrefix)) return;
    QString name = nested ? relative.section(QLatin1Char('/'), -1) : relative.mid(directoryPrefix.size());
    if (trash) name = properties.value(QStringLiteral("trashbin-original-filename")).section(QLatin1Char('/'), -1);
    // Only direct children are accepted; never treat a server href as a local path.
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral("..")
        || name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')) || name.contains(QChar::Null)) return;
    const bool directory = properties.value(QStringLiteral("resourcetype")).contains(QStringLiteral("collection"));
    auto *item = new QStandardItem(UDriveIcons::fileIcon(name, directory), name);
    item->setData(relative, PathRole);
    item->setData(directory, DirectoryRole);
    item->setData(root, RootRole);
    item->setData(entrySpace.isEmpty() ? _spaceId : entrySpace, SpaceRole);
    item->setData(properties.value(QStringLiteral("permissions")), PermissionsRole);
    item->setData(properties.value(QStringLiteral("favorite")) == QStringLiteral("1") || properties.value(QStringLiteral("favorite")) == QStringLiteral("true"), FavoriteRole);
    item->setData(properties.contains(QStringLiteral("favorite")), FavoriteKnownRole);
    item->setData(properties.value(QStringLiteral("getetag")), EtagRole);
    item->setData(QUrl(properties.value(QStringLiteral("privatelink"))), LinkRole);
    QString restorePath = properties.value(QStringLiteral("trashbin-original-location"));
    while (restorePath.startsWith(QLatin1Char('/'))) restorePath.remove(0, 1);
    item->setData(validPath(restorePath) ? restorePath : QString(), RestorePathRole);
    item->setData(name, SortRole);
    item->setData(_recentFiles.value(recentKey(root, relative), qint64(0)), OpenedRole);
    item->setToolTip(name);
    bool validSize = false;
    const qint64 bytes = properties.value(QStringLiteral("getcontentlength")).toLongLong(&validSize);
    validSize = validSize && bytes >= 0;
    auto *size = new QStandardItem(directory || !validSize ? QStringLiteral("—") : QLocale().formattedDataSize(bytes));
    size->setData(validSize ? bytes : qint64(-1), SortRole);
    auto modified = QDateTime::fromString(properties.value(QStringLiteral("getlastmodified")), QStringLiteral("ddd, dd MMM yyyy HH:mm:ss 'GMT'"));
    if (modified.isValid()) modified.setTimeZone(QTimeZone::utc());
    if (trash) {
        const QString deleted = properties.value(QStringLiteral("trashbin-delete-datetime"));
        modified = QDateTime::fromString(deleted, Qt::ISODate);
        if (!modified.isValid()) {
            modified = QDateTime::fromString(deleted, QStringLiteral("ddd, dd MMM yyyy HH:mm:ss 'GMT'"));
            if (modified.isValid()) modified.setTimeZone(QTimeZone::utc());
        }
    }
    auto *date = new QStandardItem(modified.isValid() ? QLocale().toString(modified.toLocalTime(), QLocale::ShortFormat) : QStringLiteral("—"));
    date->setData(modified, SortRole);
    date->setToolTip(modified.isValid() ? modified.toLocalTime().toString(Qt::ISODate) : QString());
    QString locationText = trash ? restorePath : relative.section(QLatin1Char('/'), 0, -2);
    if (nested && _state && _state->account()) {
        for (const auto *space : _state->account()->spacesManager()->spaces()) {
            if (space->id() == entrySpace) {
                locationText = space->displayName() + (locationText.isEmpty() ? QString() : QStringLiteral(" / ") + locationText);
                break;
            }
        }
    }
    auto *location = new QStandardItem(locationText);
    location->setData(location->text(), SortRole);
    size->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto *actions = new QStandardItem;
    actions->setToolTip(tr("Favorite, copy sign-in link, or more actions"));
    _model->appendRow({item, size, date, location, actions});
}

QString UDriveFileBrowser::localPath(const QString &remotePath, const QUrl &root) const
{
    if (!_state || !_state->account()) return {};
    const QUrl remote = Utility::concatUrlPath(root.isEmpty() ? _root : root, remotePath);
    for (auto *folder : FolderMan::instance()->foldersForAccount(_state->account()->uuid())) {
        const QUrl folderUrl = folder->remoteUrl();
        const QString prefix = withoutTrailingSlash(folderUrl.path()) + QLatin1Char('/');
        if (folderUrl.scheme() != remote.scheme() || folderUrl.host() != remote.host() || folderUrl.port() != remote.port()
            || !remote.path().startsWith(prefix)) continue;
        const QString root = QFileInfo(folder->path()).canonicalFilePath();
        const QString candidate = QFileInfo(QDir(root).filePath(remote.path().mid(prefix.size()))).canonicalFilePath();
        if (!root.isEmpty() && candidate.startsWith(root + QLatin1Char('/')) && QFileInfo(candidate).isFile())
            return candidate;
    }
    return {};
}

void UDriveFileBrowser::updateActions()
{
    const auto index = _tree->currentIndex().siblingAtColumn(0);
    const bool valid = index.isValid() && canBrowse() && !_listing && !_report && !_operation;
    const bool trash = _section == Section::Trash;
    const bool directory = index.data(DirectoryRole).toBool();
    const bool local = valid && !trash && !directory && !localPath(index.data(PathRole).toString(), selectedRoot()).isEmpty();
    bool pdf = false;
#ifdef Q_OS_MACOS
    pdf = !directory && index.data().toString().endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive);
#endif
    _openButton->setText(directory ? tr("Open folder") : pdf ? tr("Open PDF") : tr("Open file"));
    _openButton->setEnabled(valid && !trash && !_download && (directory || local || pdf));
    _openButton->setToolTip(pdf  ? tr("Preview this PDF inside UDrive.")
            : local || directory ? QString()
                                 : tr("Download this file to open it on your computer."));
    _downloadButton->setEnabled(valid && !trash && !directory && !_download);
    const bool navigate = canBrowse() && !_root.isEmpty() && _section != Section::Favorites && !trash && !_operation;
    _breadcrumbs->setEnabled(navigate);
    _rootButton->setEnabled(navigate && !_path.isEmpty());
    _refreshButton->setEnabled(canBrowse() && !_root.isEmpty());
    _onlineButton->setEnabled(!_webUrl.isEmpty());
    if (_search)
        _search->setEnabled(canBrowse() && !_root.isEmpty());
    _spaces->setEnabled(canBrowse() && _spaces->count());
    const bool writableFolder =
        canBrowse() && !_root.isEmpty() && !_listing && !_report && !_operation && !_download && !trash && _section != Section::Favorites;
    _newFolderAction->setEnabled(writableFolder && (!_folderPermissionsKnown || _folderPermissions.contains(QLatin1Char('K'))));
    _uploadAction->setEnabled(writableFolder && (!_folderPermissionsKnown || _folderPermissions.contains(QLatin1Char('C'))));
    _newButton->setEnabled(_newFolderAction->isEnabled() || _uploadAction->isEnabled());
    const QString permissions = index.data(PermissionsRole).toString();
    _renameAction->setEnabled(valid && !trash && !_download && (permissions.isEmpty() || permissions.contains(QLatin1Char('N'))));
    _copyAction->setEnabled(
        valid && !trash && !_download && (!_folderPermissionsKnown || _folderPermissions.contains(directory ? QLatin1Char('K') : QLatin1Char('C'))));
    _moveAction->setEnabled(valid && !trash && !_download && (permissions.isEmpty() || permissions.contains(QLatin1Char('V'))));
    _deleteAction->setEnabled(valid && !trash && !_download && (permissions.isEmpty() || permissions.contains(QLatin1Char('D'))));
    _restoreAction->setVisible(trash);
    _restoreAction->setEnabled(valid && trash && !index.data(RestorePathRole).toString().isEmpty());
    _favoriteAction->setVisible(!trash);
    _favoriteAction->setEnabled(valid && !trash && index.data(FavoriteKnownRole).toBool());
    _favoriteAction->setText(index.data(FavoriteRole).toBool() ? tr("Remove from Favorites") : tr("Add to Favorites"));
    const QUrl link = index.data(LinkRole).toUrl();
    _linkAction->setEnabled(valid && !trash && (link.scheme() == QStringLiteral("https") || link.scheme() == QStringLiteral("http")));
    _linkAction->setVisible(!trash);
    _deleteAction->setVisible(!trash);
    _renameAction->setVisible(!trash);
    _copyAction->setVisible(!trash);
    _moveAction->setVisible(!trash);
    _fileMenu->setEnabled(valid);
    _openButton->setVisible(index.isValid());
    _downloadButton->setVisible(index.isValid());
    Q_EMIT creationAvailabilityChanged();
}

void UDriveFileBrowser::activateSelection()
{
    const auto index = _tree->currentIndex().siblingAtColumn(0);
    if (!index.isValid() || _listing || _report || _operation || !canBrowse() || _section == Section::Trash) return;
    const QString path = index.data(PathRole).toString();
    if (index.data(DirectoryRole).toBool()) {
        cancelDownload();
        closePdf();
        if (_section == Section::Favorites) {
            const QString spaceId = index.data(SpaceRole).toString();
            for (auto *space : _state->account()->spacesManager()->spaces()) {
                if (space->id() != spaceId) continue;
                _section = space->sortPriority() == 100 ? Section::Personal : space->sortPriority() == 50 ? Section::Shares : Section::Workspaces;
                _spaceId = spaceId;
                updateSpaces();
                Q_EMIT sectionChanged(_section);
                break;
            }
        }
        _path = path;
        refresh();
    } else if (previewSelectedPdf()) {
        return;
    } else if (const QString local = localPath(path, selectedRoot()); !local.isEmpty()) {
        const auto root = selectedRoot();
        if (QDesktopServices::openUrl(QUrl::fromLocalFile(local))) recordOpened(root, path);
        else
            setMessage(tr("Could not open this file on your computer."));
    } else {
        downloadSelection();
    }
}

QLineEdit *UDriveFileBrowser::takeSearchField()
{
    _workspaceLayout = true;
    _search->setParent(nullptr);
    _newButton->hide();
    updateBreadcrumbs();
    return _search;
}

bool UDriveFileBrowser::canCreate() const
{
    return _newButton->isEnabled();
}

void UDriveFileBrowser::showNewMenu(const QPoint &position)
{
    if (canCreate())
        _newButton->menu()->popup(position);
}

void UDriveFileBrowser::setMessage(const QString &message)
{
    _message->setText(message);
}

QModelIndex UDriveFileBrowser::selectedItem() const { return _tree->currentIndex().siblingAtColumn(0); }

QUrl UDriveFileBrowser::selectedRoot() const
{
    const QUrl root = selectedItem().data(RootRole).toUrl();
    return root.isEmpty() ? _root : root;
}

QUrl UDriveFileBrowser::trashRoot() const
{
    if (!_state || !_state->account() || !validName(_driveId)) return {};
    return Utility::concatUrlPath(_state->account()->url(), QStringLiteral("dav/spaces/trash-bin/") + _driveId);
}

void UDriveFileBrowser::refreshFavorites()
{
    setMessage(tr("Loading Favorites…"));
    QNetworkRequest request;
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/xml; charset=utf-8"));
    QByteArray body = "<?xml version=\"1.0\"?><oc:filter-files xmlns:d=\"DAV:\" xmlns:oc=\"http://owncloud.org/ns\">"
        "<d:prop><d:resourcetype/><d:getcontentlength/><d:getlastmodified/><d:getetag/><oc:permissions/><oc:favorite/><oc:privatelink/></d:prop>"
        "<oc:filter-rules><oc:favorite>1</oc:favorite></oc:filter-rules></oc:filter-files>";
    auto *job = new SimpleNetworkJob(_state->account(), _state->account()->davUrl(), {}, "REPORT", std::move(body), request, this);
    _report = job;
    job->setTimeout(std::chrono::seconds(30));
    connect(job, &AbstractNetworkJob::finishedSignal, this, [this, job] {
        if (_report != job) return;
        _report.clear();
        bool success = job->reply() && job->reply()->error() == QNetworkReply::NoError && job->httpStatusCode() == 207;
        int unmapped = 0;
        if (success) {
            LsColXMLParser parser;
            connect(&parser, &LsColXMLParser::directoryListingIterated, this, [this, &unmapped](const QString &href, const QMap<QString, QString> &props) {
                auto normalized = [](const QString &path) {
                    const QString legacy(QStringLiteral("/remote.php/dav/spaces/"));
                    return path.startsWith(legacy) ? QStringLiteral("/dav/spaces/") + path.mid(legacy.size()) : path;
                };
                // The DAV parser already decoded the href. Preserve literal #,
                // percent and question-mark characters in the resource name.
                QString path = href;
                const int scheme = path.indexOf(QStringLiteral("://"));
                if (scheme >= 0) {
                    const int slash = path.indexOf(QLatin1Char('/'), scheme + 3);
                    path = slash >= 0 ? path.mid(slash) : QString();
                }
                for (auto *space : _state->account()->spacesManager()->spaces()) {
                    QUrl root = space->webDavUrl(); root.setPath(withoutTrailingSlash(root.path()));
                    const QString prefix = normalized(root.path()) + QLatin1Char('/');
                    if (normalized(path).startsWith(prefix)) {
                        appendEntry(root.path() + QLatin1Char('/') + normalized(path).mid(prefix.size()), props, root, space->id(), true);
                        return;
                    }
                    const QString legacy = withoutTrailingSlash(_state->account()->davUrl().path()) + QLatin1Char('/');
                    const auto spaces = _state->account()->spacesManager()->spaces();
                    const int personalCount = static_cast<int>(std::count_if(spaces.begin(), spaces.end(), [](const auto *s) { return s->sortPriority() == 100; }));
                    if (space->sortPriority() == 100 && personalCount == 1 && path.startsWith(legacy)) {
                        appendEntry(root.path() + QLatin1Char('/') + path.mid(legacy.size()), props, root, space->id(), true);
                        return;
                    }
                }
                if (!props.isEmpty()) ++unmapped;
            });
            QHash<QString, qint64> sizes;
            success = parser.parse(job->reply()->readAll(), &sizes, {});
        }
        if (!success) {
            _model->removeRows(0, _model->rowCount());
            setMessage(tr("Could not load Favorites. %1").arg(job->errorString()));
        } else {
            setMessage(unmapped ? tr("%1 Favorites shown; some items are no longer available.").arg(_model->rowCount())
                : _model->rowCount() ? tr("%n favorite(s)", nullptr, _model->rowCount()) : tr("No Favorites yet. Use Actions to add a file or folder."));
        }
        updateActions();
    });
    job->start();
    updateActions();
}

bool UDriveFileBrowser::runOperation(
    const QByteArray &verb, const QUrl &root, const QString &path, const QNetworkRequest &request, const QString &message, QIODevice *body)
{
    if (!canBrowse() || _operation || _download || !validPath(path) || root.host().isEmpty()
        || !LocalDevelopment::allowsAuthenticatedUrl(root, _state->account()->url()) || !root.query().isEmpty() || !root.fragment().isEmpty()) {
        delete body;
        return false;
    }
    const QUrl viewRoot = _root;
    const QString viewPath = _path;
    const Section section = _section;
    auto *job = new FileActionJob(_state->account(), root, path, verb, body, request, this);
    if (body)
        body->setParent(job);
    _operation = job;
    _uploading = verb == "PUT";
    job->setTimeout(std::chrono::seconds(60));
    if (_uploading) {
        _progress->setAccessibleName(tr("Upload progress"));
        _progress->setRange(0, 100); _progress->setValue(0); _progress->show();
        _cancelButton->setText(tr("Cancel upload")); _cancelButton->show();
        job->addNewReplyHook([this, job](QNetworkReply *reply) {
            connect(reply, &QNetworkReply::uploadProgress, this, [this, job](qint64 sent, qint64 total) {
                if (_operation == job && total > 0) _progress->setValue(static_cast<int>(100.0 * sent / total));
            });
        });
    }
    connect(job, &AbstractNetworkJob::finishedSignal, this, [this, job, verb, message, viewRoot, viewPath, section] {
        if (_operation != job) return;
        bool success = job->reply() && job->reply()->error() == QNetworkReply::NoError && job->httpStatusCode() >= 200 && job->httpStatusCode() < 300;
        // Collection operations can return 207 while individual items failed.
        // A successful HTTP envelope does not confirm a successful mutation.
        if (success && job->httpStatusCode() == 207) success = multistatusSucceeded(job->reply()->readAll());
        if (verb == "PROPPATCH") success = success && job->httpStatusCode() == 207;
        QString result = message;
        if (!success) {
            result = job->httpStatusCode() == 412 ? tr("A file already exists or this item changed. Refresh and try again. Nothing was overwritten.")
                : tr("Operation failed: %1 (HTTP %2)").arg(job->errorString()).arg(job->httpStatusCode());
            if (job->httpStatusCode() == 207) result = verb == "PROPPATCH"
                ? tr("The server did not confirm the Favorite change. Refresh to check the result.")
                : tr("The server did not confirm that all items succeeded. Some changes may have completed. Refresh to check the result.");
            if (verb == "MOVE" && section == Section::Trash && job->httpStatusCode() == 409)
                result = tr("The original parent folder is missing. Restore or recreate that folder first.");
        }
        _operation.clear();
        if (_uploading) { _progress->hide(); _cancelButton->hide(); }
        _uploading = false;
        if (success && _root == viewRoot && _path == viewPath && _section == section) refresh();
        setMessage(result);
        updateActions();
        Q_EMIT operationFinished(success, result);
    });
    setMessage(_uploading ? tr("Uploading…") : tr("Working…"));
    job->start(); updateActions();
    return true;
}

void UDriveFileBrowser::cancelOperation()
{
    auto job = _operation; _operation.clear();
    if (job) job->abort();
    if (_uploading) { _progress->hide(); _cancelButton->hide(); }
    _uploading = false;
    if (job) Q_EMIT operationFinished(false, tr("Operation canceled. Refresh to check the server."));
}

bool UDriveFileBrowser::createFolder(const QString &name)
{
    if (!_newFolderAction->isEnabled() || !validName(name)) return false;
    return runOperation("MKCOL", _root, _path.isEmpty() ? name : _path + QLatin1Char('/') + name, {}, tr("Folder created."));
}

bool UDriveFileBrowser::uploadFile(const QString &localFile)
{
    const QFileInfo info(localFile);
    if (!_uploadAction->isEnabled() || !info.isFile() || !validName(info.fileName())) return false;
    auto *file = new QFile(localFile);
    if (!file->open(QIODevice::ReadOnly)) { setMessage(file->errorString()); delete file; return false; }
    QNetworkRequest request;
    request.setRawHeader("If-None-Match", "*");
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));
    request.setHeader(QNetworkRequest::ContentLengthHeader, file->size());
    return runOperation("PUT", _root, _path.isEmpty() ? info.fileName() : _path + QLatin1Char('/') + info.fileName(), request, tr("File uploaded."), file);
}

bool UDriveFileBrowser::renameSelected(const QString &name)
{
    if (!_renameAction->isEnabled() || !validName(name)) return false;
    const auto item = selectedItem(); const QString source = item.data(PathRole).toString();
    const int slash = source.lastIndexOf(QLatin1Char('/'));
    const QString destination = slash < 0 ? name : source.left(slash + 1) + name;
    if (destination == source) return false;
    QNetworkRequest request;
    request.setRawHeader("Destination", Utility::concatUrlPath(selectedRoot(), destination).toEncoded());
    request.setRawHeader("Overwrite", "F");
    if (!item.data(EtagRole).toString().isEmpty()) request.setRawHeader("If-Match", item.data(EtagRole).toString().toUtf8());
    return runOperation("MOVE", selectedRoot(), source, request, tr("Item renamed."));
}

bool UDriveFileBrowser::copySelected(const QString &name)
{
    if (!_copyAction->isEnabled() || !validName(name)) return false;
    const auto item = selectedItem(); const QString source = item.data(PathRole).toString();
    const int slash = source.lastIndexOf(QLatin1Char('/'));
    const QString destination = slash < 0 ? name : source.left(slash + 1) + name;
    if (destination == source) return false;
    QNetworkRequest request;
    request.setRawHeader("Destination", Utility::concatUrlPath(selectedRoot(), destination).toEncoded());
    request.setRawHeader("Overwrite", "F");
    request.setRawHeader("Depth", "infinity");
    if (!item.data(EtagRole).toString().isEmpty()) request.setRawHeader("If-Match", item.data(EtagRole).toString().toUtf8());
    return runOperation("COPY", selectedRoot(), source, request, tr("Copy created."));
}

bool UDriveFileBrowser::moveSelected(const QString &destinationFolder)
{
    if (!_moveAction->isEnabled()) return false;
    QString folder = destinationFolder;
    if (folder.startsWith(QLatin1Char('/'))) folder.remove(0, 1);
    folder = withoutTrailingSlash(folder);
    if (!folder.isEmpty() && !validPath(folder)) return false;
    const auto item = selectedItem(); const QString source = item.data(PathRole).toString();
    const QString destination = folder.isEmpty() ? item.data().toString() : folder + QLatin1Char('/') + item.data().toString();
    if (destination == source || (item.data(DirectoryRole).toBool() && destination.startsWith(source + QLatin1Char('/')))) return false;
    QNetworkRequest request;
    request.setRawHeader("Destination", Utility::concatUrlPath(selectedRoot(), destination).toEncoded());
    request.setRawHeader("Overwrite", "F");
    if (!item.data(EtagRole).toString().isEmpty()) request.setRawHeader("If-Match", item.data(EtagRole).toString().toUtf8());
    return runOperation("MOVE", selectedRoot(), source, request, tr("Item moved."));
}

bool UDriveFileBrowser::deleteSelected()
{
    if (!_deleteAction->isEnabled()) return false;
    QNetworkRequest request;
    if (!selectedItem().data(EtagRole).toString().isEmpty()) request.setRawHeader("If-Match", selectedItem().data(EtagRole).toString().toUtf8());
    return runOperation("DELETE", selectedRoot(), selectedItem().data(PathRole).toString(), request, tr("Item moved to Deleted files."));
}

bool UDriveFileBrowser::restoreSelected()
{
    if (!_restoreAction->isEnabled()) return false;
    const QString path = selectedItem().data(RestorePathRole).toString();
    if (!validPath(path)) return false;
    QNetworkRequest request;
    request.setRawHeader("Destination", Utility::concatUrlPath(_root, path).toEncoded());
    request.setRawHeader("Overwrite", "F");
    return runOperation("MOVE", trashRoot(), selectedItem().data(PathRole).toString(), request, tr("Item restored to its original location."));
}

bool UDriveFileBrowser::setSelectedFavorite(bool favorite)
{
    if (!_favoriteAction->isEnabled()) return false;
    auto *body = new QBuffer;
    body->setData(QByteArray("<?xml version=\"1.0\"?><d:propertyupdate xmlns:d=\"DAV:\" xmlns:oc=\"http://owncloud.org/ns\"><d:set><d:prop><oc:favorite>")
        + (favorite ? "true" : "false") + "</oc:favorite></d:prop></d:set></d:propertyupdate>");
    body->open(QIODevice::ReadOnly);
    QNetworkRequest request;
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/xml; charset=utf-8"));
    return runOperation("PROPPATCH", selectedRoot(), selectedItem().data(PathRole).toString(), request,
        favorite ? tr("Added to Favorites.") : tr("Removed from Favorites."), body);
}

bool UDriveFileBrowser::previewSelectedPdf()
{
#ifdef Q_OS_MACOS
    const auto index = selectedItem();
    if (!index.isValid() || index.data(DirectoryRole).toBool() || _listing || _report || _operation || _download
        || !canBrowse() || _section == Section::Trash || !index.data().toString().endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive)) return false;
    closePdf();
    const QString local = localPath(index.data(PathRole).toString(), selectedRoot());
    if (!local.isEmpty()) {
        _pages->setCurrentWidget(_pdfPreview);
        _pdfPreview->openFile(local, index.data().toString());
        _pdfPreview->findChild<QPushButton *>(QStringLiteral("pdfBack"))->setFocus();
        recordOpened(selectedRoot(), index.data(PathRole).toString());
        return true;
    }
    _previewStorage = std::make_shared<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/udrive-pdf-XXXXXX"));
    if (!_previewStorage->isValid()) { _previewStorage.reset(); setMessage(tr("Could not create a temporary PDF preview.")); return true; }
    const QString destination = _previewStorage->filePath(QStringLiteral("preview.pdf"));
    if (!startDownload(destination, true)) _previewStorage.reset();
    return true;
#else
    return false;
#endif
}

void UDriveFileBrowser::closePdf()
{
#ifdef Q_OS_MACOS
    if (_previewDownload) cancelDownload();
    _pdfPreview->clear();
    _previewStorage.reset();
    _pages->setCurrentIndex(0);
#endif
}

void UDriveFileBrowser::hideEvent(QHideEvent *event)
{
    _thumbnailTimer->stop();
    _thumbnails->cancel();
    closePdf();
    QWidget::hideEvent(event);
}

void UDriveFileBrowser::cancelDownload()
{
    auto job = _download;
    _download.clear();
    if (job) job->abort();
    if (_downloadFile) _downloadFile->cancelWriting();
    _downloadFile.reset();
#ifdef Q_OS_MACOS
    if (_previewDownload) _previewStorage.reset();
#endif
    _previewDownload = false;
    _progress->hide();
    _cancelButton->hide();
    updateActions();
    if (job) Q_EMIT downloadFinished(false, _destination);
}

void UDriveFileBrowser::downloadSelection()
{
    const auto index = _tree->currentIndex().siblingAtColumn(0);
    if (!index.isValid() || index.data(DirectoryRole).toBool() || _listing || _download || !canBrowse()) return;
    const QString path = index.data(PathRole).toString();
    const QUrl root = _root;
    const QString currentPath = _path;
    const QUrl entryRoot = selectedRoot();
    const QPointer<UDriveFileBrowser> guard(this);
    const QString destination = QFileDialog::getSaveFileName(this, tr("Save file"),
        QDir(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)).filePath(index.data().toString()));
    if (!guard || destination.isEmpty() || !canBrowse() || _root != root || _path != currentPath || _download
        || selectedRoot() != entryRoot || _tree->currentIndex().siblingAtColumn(0).data(PathRole).toString() != path) return;
    saveSelectedFile(destination);
}

bool UDriveFileBrowser::saveSelectedFile(const QString &destination)
{
    return startDownload(destination, false);
}

bool UDriveFileBrowser::startDownload(const QString &destination, bool preview)
{
    const auto index = _tree->currentIndex().siblingAtColumn(0);
    if (destination.isEmpty() || !index.isValid() || index.data(DirectoryRole).toBool() || _listing || _report || _operation || _download
        || !canBrowse() || _section == Section::Trash) return false;
    const QString path = index.data(PathRole).toString();
    _destination = destination;
    _previewDownload = preview;
    _writeError.clear();
    QNetworkRequest request;
    if (preview && !index.data(EtagRole).toString().isEmpty()) request.setRawHeader("If-Match", index.data(EtagRole).toString().toUtf8());
    const QString name = index.data().toString();
    const QUrl root = selectedRoot();
    auto *job = new SimpleNetworkJob(_state->account(), selectedRoot(), path, "GET", nullptr, request, this);
    _download = job;
    job->setTimeout(std::chrono::seconds(60));
    job->addNewReplyHook([this, job](QNetworkReply *reply) {
        if (_download != job) return;
        // Start a fresh atomic save on every authentication retry/redirect.
        _downloadFile = std::make_unique<QSaveFile>(_destination);
        if (!_downloadFile->open(QIODevice::WriteOnly)) {
            _writeError = _downloadFile->errorString();
            QTimer::singleShot(0, job, &AbstractNetworkJob::abort);
            return;
        }
        if (_previewDownload && !_downloadFile->setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
            _writeError = tr("Could not secure the temporary PDF file.");
            QTimer::singleShot(0, job, &AbstractNetworkJob::abort);
            return;
        }
#ifdef Q_OS_MACOS
        connect(reply, &QNetworkReply::metaDataChanged, this, [this, job, reply] {
            if (_download != job || job->reply() != reply || !_previewDownload) return;
            if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200
                && reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() > UDrivePdfDocument::MaximumFileSize) {
                _writeError = tr("This PDF is too large to preview (maximum 100 MiB).");
                job->abort();
            }
        });
#endif
        connect(reply, &QNetworkReply::readyRead, this, [this, job, reply] {
            if (_download != job || job->reply() != reply || !_downloadFile || !_writeError.isEmpty()) return;
            if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) return;
            const QByteArray chunk = reply->readAll();
#ifdef Q_OS_MACOS
            if (_previewDownload && _downloadFile->pos() + chunk.size() > UDrivePdfDocument::MaximumFileSize) {
                _writeError = tr("This PDF is too large to preview (maximum 100 MiB).");
                job->abort(); return;
            }
#endif
            if (_downloadFile->write(chunk) != chunk.size()) {
                _writeError = _downloadFile->errorString();
                job->abort();
            }
        });
        connect(reply, &QNetworkReply::downloadProgress, this, [this, job](qint64 received, qint64 total) {
            if (_download != job) return;
            _progress->setRange(0, total > 0 ? 100 : 0);
            if (total > 0) _progress->setValue(static_cast<int>(100.0 * received / total));
        });
    });
    connect(job, &AbstractNetworkJob::finishedSignal, this, [this, job, preview, name, root, path] {
        if (_download != job) return;
        bool success = job->reply() && job->reply()->error() == QNetworkReply::NoError
            && job->httpStatusCode() == 200 && _writeError.isEmpty() && _downloadFile;
        if (success) {
            const QByteArray remaining = job->reply()->readAll();
#ifdef Q_OS_MACOS
            if (preview && _downloadFile->pos() + remaining.size() > UDrivePdfDocument::MaximumFileSize) {
                success = false;
                _writeError = tr("This PDF is too large to preview (maximum 100 MiB).");
            }
#endif
            success = success && _downloadFile->write(remaining) == remaining.size() && _downloadFile->commit();
            if (!success && _writeError.isEmpty()) _writeError = _downloadFile->errorString();
        }
        QString error = !_writeError.isEmpty() ? _writeError : job->errorString();
        if (error.isEmpty()) error = tr("The server returned HTTP %1.").arg(job->httpStatusCode());
        _download.clear();
        if (!success && _downloadFile) _downloadFile->cancelWriting();
        _downloadFile.reset();
        _progress->hide();
        _cancelButton->hide();
        _previewDownload = false;
#ifdef Q_OS_MACOS
        if (preview && success) {
            recordOpened(root, path);
            _pages->setCurrentWidget(_pdfPreview);
            _pdfPreview->openFile(_destination, name, _previewStorage);
            _pdfPreview->findChild<QPushButton *>(QStringLiteral("pdfBack"))->setFocus();
        } else if (preview) _previewStorage.reset();
#endif
        setMessage(success ? preview ? tr("PDF opened in UDrive.") : tr("Saved to %1").arg(QDir::toNativeSeparators(_destination)) : tr("Download failed: %1").arg(error));
        updateActions();
        Q_EMIT downloadFinished(success, _destination);
    });
    _progress->setRange(0, 0);
    _progress->setAccessibleName(tr("Download progress"));
    _cancelButton->setText(tr("Cancel download"));
    _progress->show();
    _cancelButton->show();
    setMessage(tr("Downloading %1…").arg(index.data().toString()));
    job->start();
    updateActions();
    return true;
}
}
