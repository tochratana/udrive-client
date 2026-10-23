// SPDX-License-Identifier: GPL-2.0-or-later
#include "udrivefilebrowser.h"
#include "udriveicons.h"
#include "udriveworkspaceactions.h"
#include "udrivecombobox.h"
#include "udrivepopup.h"
#include "udrivetoast.h"
#include "udrivethumbnails.h"
#include "udrivefilepreview.h"
#include "creds/localdevelopment.h"
#include "application.h"
#include "owncloudgui.h"
#include "libsync/theme.h"
#include <QRegularExpression>

#include "folderman.h"
#include "libsync/configfile.h"
#include <QListView>
#include <QActionGroup>
#include <QStackedWidget>
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
#include "udrivepdfpreview.h"
#endif
#include <QTemporaryDir>
#include "uploaddevice.h"
#include "libsync/account.h"
#include "libsync/creds/abstractcredentials.h"
#include "libsync/graphapi/spacesmanager.h"
#include "libsync/networkjobs.h"
#include "libsync/networkjobs/jsonjob.h"
#include "common/utility.h"
#include "guiutility.h"

#include <QComboBox>
#include <QCompleter>
#include <QCheckBox>
#include <QAction>
#include <QScrollArea>
#include <QSplitter>
#include <QApplication>
#include <cmath>
#include <QBuffer>
#include <QClipboard>
#include <QCryptographicHash>
#include <QFile>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QPlainTextEdit>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QImageReader>
#include <QTextBoundaryFinder>
#include <QMessageBox>
#include <QXmlStreamReader>
#include <QDateTime>
#include <QDateEdit>
#include <QCalendarWidget>
#include <QTableView>
#include <QHeaderView>
#include <QTextCharFormat>
#include <QSpinBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QFrame>
#include <QHelpEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QNetworkReply>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QToolButton>
#include <QToolTip>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QStyle>
#include <QStylePainter>
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
#include <limits>
#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace OCC {
namespace {
bool privatePreviewDirectory(const QString &path)
{
    const QFileInfo info(path);
    if (!info.isDir() || info.isSymLink() || info.isJunction()) return false;
#ifdef Q_OS_UNIX
    if (info.ownerId() != geteuid() || (info.permissions() & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
            | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther))) return false;
#endif
    return true;
}

QString previewStorageRoot()
{
    const QString cache = Account::commonCacheDirectory();
    if (cache.isEmpty()) return {};
    const QString root = QDir(cache).filePath(QStringLiteral("preview-sessions"));
    if (!QFileInfo::exists(root)) {
        if (!QDir().mkpath(cache) || !QDir().mkdir(root)) return {};
        if (!QFile::setPermissions(root, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) return {};
    }
    return privatePreviewDirectory(root) ? root : QString();
}

std::shared_ptr<QTemporaryDir> createPreviewStorage(const QString &prefix)
{
    // A session lock lives for the process lifetime, independently of account
    // caches. Maintenance serializes session creation with orphan cleanup.
    struct Session {
        std::unique_ptr<QTemporaryDir> directory;
        std::unique_ptr<QLockFile> lock;
    };
    static Session session;
    if (!session.directory) {
        const QString root = previewStorageRoot();
        if (root.isEmpty()) return {};
        QLockFile maintenance(root + QStringLiteral("/maintenance.lock"));
        if (!maintenance.tryLock(1000)) return {};
        auto directory = std::make_unique<QTemporaryDir>(root + QStringLiteral("/session-XXXXXX"));
        if (!directory->isValid() || !privatePreviewDirectory(directory->path())) return {};
        auto lock = std::make_unique<QLockFile>(directory->filePath(QStringLiteral("active.lock")));
        lock->setStaleLockTime(0);
        if (!lock->tryLock()) return {};
        session.directory = std::move(directory);
        session.lock = std::move(lock);
    }
    auto storage = std::make_shared<QTemporaryDir>(session.directory->filePath(prefix + QStringLiteral("-XXXXXX")));
    return storage->isValid() ? storage : std::shared_ptr<QTemporaryDir>();
}

bool removePreviewDirectory(const QString &path)
{
    // Only called beneath a checked private session. Unlink links themselves;
    // never traverse them or change their targets' permissions.
    bool removed = true;
    const QDir dir(path);
    for (const auto &entry : dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
        if (entry.isJunction()) {
            removed &= QDir().rmdir(entry.absoluteFilePath());
        } else if (entry.isSymLink()) {
            removed &= QFile::remove(entry.absoluteFilePath());
        } else if (entry.isDir()) {
            removed &= removePreviewDirectory(entry.absoluteFilePath());
        } else {
            QFile file(entry.absoluteFilePath());
            if (!file.remove()) {
                removed &= file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) && file.remove();
            }
        }
    }
    return removed && QDir().rmdir(path);
}

enum Roles { PathRole = Qt::UserRole + 1, DirectoryRole, SortRole, RootRole, SpaceRole,
    PermissionsRole, FavoriteRole, FavoriteKnownRole, EtagRole, LinkRole, RestorePathRole, ThumbnailRole, OpenedRole, TrashedRole,
    ShareHiddenRole, ShareSharersRole, ShareUnavailableRole, LocationRole, SharePeopleRole, SharePermissionsLoadedRole, SharePublicLinksRole, FileIdRole };

QString fileSharingSummary(const QStringList &people, int publicLinks)
{
    QStringList parts;
    if (!people.isEmpty()) parts.append(UDriveFileBrowser::tr("Shared with %1").arg(people.join(QStringLiteral(", "))));
    if (publicLinks > 0) parts.append(UDriveFileBrowser::tr("%n public link(s)", nullptr, publicLinks));
    return parts.isEmpty() ? UDriveFileBrowser::tr("Private (only you)") : parts.join(QStringLiteral(" · "));
}

enum RowAction { RowFavorite, RowShare, RowLink, RowMore };

class VersionItemWidget : public QWidget
{
public:
    VersionItemWidget(const UDriveFileBrowser::FileVersionItem &version, bool isLast, bool canRestore,
                      const std::function<void()> &onRestore,
                      const std::function<void()> &onDownload,
                      QWidget *parent = nullptr)
        : QWidget(parent)
        , _version(version)
        , _isLast(isLast)
    {
        const bool dark = QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
        const QString textPrimary = dark ? QStringLiteral("#F9FAFB") : QStringLiteral("#111827");
        const QString textSecondary = dark ? QStringLiteral("#9CA3AF") : QStringLiteral("#6B7280");
        const QString hoverBg = dark ? QStringLiteral("rgba(255, 255, 255, 0.08)") : QStringLiteral("rgba(0, 0, 0, 0.05)");

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(28, 4, 8, 16);
        layout->setSpacing(4);

        const QString timeAgo = Utility::timeAgoInWords(_version.modified);
        const QString sizeStr = _version.size >= 0 ? QLocale().formattedDataSize(_version.size) : QString();
        auto *header = new QLabel(this);
        header->setTextFormat(Qt::RichText);
        if (!sizeStr.isEmpty()) {
            header->setText(QStringLiteral("<span style='font-weight: 600; color: %1;'>%2</span> <span style='color: %3;'>- %4</span>")
                .arg(textPrimary, timeAgo.toHtmlEscaped(), textSecondary, sizeStr.toHtmlEscaped()));
        } else {
            header->setText(QStringLiteral("<span style='font-weight: 600; color: %1;'>%2</span>").arg(textPrimary, timeAgo.toHtmlEscaped()));
        }
        header->setToolTip(_version.modified.isValid() ? _version.modified.toLocalTime().toString(Qt::ISODate) : QString());
        layout->addWidget(header);

        const auto makeButton = [this, textPrimary, hoverBg](UDriveIcons::Symbol sym, const QString &text, const std::function<void()> &clicked) {
            auto *btn = new QPushButton(UDriveIcons::icon(sym), text, this);
            btn->setCursor(Qt::PointingHandCursor);
            btn->setIconSize(QSize(16, 16));
            btn->setStyleSheet(QStringLiteral(
                "QPushButton {"
                "    background: transparent;"
                "    border: none;"
                "    border-radius: 4px;"
                "    color: %1;"
                "    font-size: 13px;"
                "    font-weight: 500;"
                "    padding: 3px 6px;"
                "    text-align: left;"
                "}"
                "QPushButton:hover {"
                "    background: %2;"
                "}"
                "QPushButton:disabled {"
                "    opacity: 0.5;"
                "    color: #9CA3AF;"
                "}"
            ).arg(textPrimary, hoverBg));
            QObject::connect(btn, &QPushButton::clicked, btn, clicked);
            return btn;
        };

        auto *restoreBtn = makeButton(UDriveIcons::Symbol::Restore, UDriveFileBrowser::tr("Restore"), onRestore);
        restoreBtn->setObjectName(QStringLiteral("versionRestoreBtn"));
        if (!canRestore) {
            restoreBtn->setEnabled(false);
            restoreBtn->setToolTip(UDriveFileBrowser::tr("You need edit permission to restore this file"));
        } else {
            restoreBtn->setToolTip(UDriveFileBrowser::tr("Restore this version to replace the active file"));
        }
        layout->addWidget(restoreBtn, 0, Qt::AlignLeft);

        auto *downloadBtn = makeButton(UDriveIcons::Symbol::Download, UDriveFileBrowser::tr("Download"), onDownload);
        downloadBtn->setObjectName(QStringLiteral("versionDownloadBtn"));
        downloadBtn->setToolTip(UDriveFileBrowser::tr("Download and save this version locally"));
        layout->addWidget(downloadBtn, 0, Qt::AlignLeft);
    }

    const UDriveFileBrowser::FileVersionItem &version() const { return _version; }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QWidget::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const bool dark = palette().color(QPalette::Window).lightness() < 128;
        const QColor lineColor = dark ? QColor(0x37, 0x41, 0x51) : QColor(0xE2, 0xE8, 0xF0);
        const QColor nodeFill = dark ? QColor(0x4B, 0x55, 0x63) : QColor(0xD1, 0xD5, 0xDB);
        const QColor nodeBorder = dark ? QColor(0x6B, 0x72, 0x80) : QColor(0x9C, 0xA3, 0xAF);

        const int lineX = 14;
        const int nodeY = 14;
        const qreal radius = 4.0;

        // Draw vertical track line
        painter.setPen(QPen(lineColor, 2));
        painter.drawLine(lineX, 0, lineX, _isLast ? qMin(height(), nodeY + 54) : height());

        // Draw circular node dot
        painter.setPen(QPen(nodeBorder, 1.5));
        painter.setBrush(nodeFill);
        painter.drawEllipse(QPointF(lineX, nodeY), radius, radius);
    }

private:
    UDriveFileBrowser::FileVersionItem _version;
    bool _isLast = false;
};

class FileDetailsActionButton : public QPushButton
{
public:
    using QPushButton::QPushButton;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QStyleOptionButton option;
        initStyleOption(&option);
        const QString label = option.text;
        const QIcon image = option.icon;
        option.text.clear();
        option.icon = QIcon();
        QStylePainter painter(this);
        painter.drawControl(QStyle::CE_PushButton, option);
        // Use fixed icon and text columns on every platform.
        const QRect iconRect(12, (height() - 20) / 2, 20, 20);
        const QRect textRect(44, 0, qMax(0, width() - 56), height());
        const auto mode = isEnabled() ? QIcon::Normal : QIcon::Disabled;
        painter.drawItemPixmap(QStyle::visualRect(layoutDirection(), rect(), iconRect), Qt::AlignCenter,
            image.pixmap(QSize(20, 20), devicePixelRatioF(), mode));
        painter.drawItemText(QStyle::visualRect(layoutDirection(), rect(), textRect),
            Qt::AlignVCenter | Qt::AlignLeading | Qt::TextShowMnemonic, option.palette, isEnabled(),
            fontMetrics().elidedText(label, Qt::ElideRight, textRect.width()), QPalette::ButtonText);
    }
};

QMenu *createFileMenu(QWidget *parent)
{
    return new UDriveMenu(parent);
}

void styleWorkspaceDialog(QDialog *dialog)
{
    const bool dark = qApp->palette().color(QPalette::Window).lightness() < 128;
    dialog->setStyleSheet(QStringLiteral(
        "QDialog { background: %1; color: %2; }"
        "QLabel { color: %2; }"
        "QPlainTextEdit { background: %3; color: %2; border: 1px solid %4; border-radius: 8px; padding: 12px; }"
        "QPlainTextEdit:focus { border-color: #22C55E; }"
        "QPushButton { background: %3; color: %2; border: 1px solid %4; border-radius: 8px; padding: 8px 16px; }"
        "QPushButton:hover { border-color: #22C55E; }"
        "QPushButton:default { background: #22C55E; color: white; border-color: #22C55E; }"
        "QPushButton:disabled { color: #8A9390; }").arg(
            dark ? "#242528" : "#FFFFFF", dark ? "#EDEEF0" : "#1F2328",
            dark ? "#1B1C1E" : "#FAFCFB", dark ? "#3E4146" : "#D0D7DE"));
}

class SearchField : public QLineEdit
{
public:
    SearchField(const QString &searchLabel, const QString &clearLabel, QWidget *parent)
        : QLineEdit(parent)
    {
        setMinimumHeight(44);
        auto addButton = [this](const QString &name, const QString &label, UDriveIcons::Symbol symbol) {
            auto *button = new QToolButton(this);
            button->setObjectName(name);
            button->setIcon(UDriveIcons::icon(symbol));
            button->setIconSize(QSize(18, 18));
            button->setFixedSize(28, 28);
            button->setAutoRaise(true);
            button->setFocusPolicy(Qt::NoFocus);
            button->setCursor(Qt::PointingHandCursor);
            button->setAccessibleName(label);
            button->setToolTip(label);
            return button;
        };
        _searchButton = addButton(QStringLiteral("driveSearchIcon"), searchLabel, UDriveIcons::Symbol::Search);
        _clearButton = addButton(QStringLiteral("driveClearSearch"), clearLabel, UDriveIcons::Symbol::Close);
        _clearButton->hide();
        connect(_searchButton, &QToolButton::clicked, this, [this] { setFocus(Qt::MouseFocusReason); });
        connect(_clearButton, &QToolButton::clicked, this, [this] {
            clear();
            setFocus(Qt::MouseFocusReason);
        });
        connect(this, &QLineEdit::textChanged, this, [this] {
            _clearButton->setVisible(!text().isEmpty());
            positionButtons();
        });
        positionButtons();
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QLineEdit::resizeEvent(event);
        positionButtons();
    }
    void changeEvent(QEvent *event) override
    {
        QLineEdit::changeEvent(event);
        if (event->type() == QEvent::LayoutDirectionChange)
            positionButtons();
    }

private:
    void positionButtons()
    {
        if (!_searchButton || !_clearButton)
            return;
        const bool rtl = layoutDirection() == Qt::RightToLeft;
        const int leading = 8;
        const int trailing = width() - 36;
        const int top = (height() - 28) / 2;
        _searchButton->move(rtl ? trailing : leading, top);
        _clearButton->move(rtl ? leading : trailing, top);
        const int clearMargin = text().isEmpty() ? 0 : 32;
        setTextMargins(rtl ? clearMargin : 32, 0, rtl ? 32 : clearMargin, 0);
    }

    QToolButton *_searchButton = nullptr;
    QToolButton *_clearButton = nullptr;
};

class SelectAllCheckBox : public QCheckBox
{
public:
    using QCheckBox::QCheckBox;
protected:
    void nextCheckState() override
    {
        setCheckState(checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
    }
};

class SpaceItemDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(qMax(32, size.height()));
        return size;
    }
};

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

QMap<QString, QString> shareCreators(const QJsonObject &item, bool incoming)
{
    QMap<QString, QString> result;
    const auto add = [&result](const QJsonObject &identity) {
        for (const auto *kind : {"user", "group"}) {
            const auto person = identity.value(QLatin1String(kind)).toObject();
            const QString id = person.value(QStringLiteral("id")).toString();
            if (!validName(id) || id.size() > 1024) continue;
            const QString label = person.value(QStringLiteral("displayName")).toString().left(256);
            result.insert(QLatin1String(kind) + QLatin1Char(':') + id, label.isEmpty() ? id : label);
        }
    };
    const auto remote = item.value(QStringLiteral("remoteItem")).toObject();
    const auto permissions = incoming ? remote.value(QStringLiteral("permissions")) : item.value(QStringLiteral("permissions"));
    for (const auto &permission : permissions.toArray())
        add(permission.toObject().value(QStringLiteral("invitation")).toObject().value(QStringLiteral("invitedBy")).toObject());
    // sharedWithMe.createdBy identifies the creator of the oldest share. It
    // must not be confused with an outgoing resource's original file author.
    if (incoming && result.isEmpty()) add(item.value(QStringLiteral("createdBy")).toObject());
    return result;
}

QStringList shareRecipients(const QJsonObject &item)
{
    QMap<QString, QString> people;
    const auto add = [&people](const QJsonObject &identity) {
        for (const auto *kind : {"user", "group"}) {
            const auto person = identity.value(QLatin1String(kind)).toObject();
            const auto id = person.value(QStringLiteral("id")).toString();
            const auto label = person.value(QStringLiteral("displayName")).toString().simplified().left(256);
            if (!id.isEmpty() && people.size() < 256)
                people.insert(QLatin1String(kind) + QLatin1Char(':') + id, label.isEmpty() ? id.left(256) : label);
        }
    };
    for (const auto &entry : item.value(QStringLiteral("permissions")).toArray()) {
        const auto permission = entry.toObject();
        if (permission.value(QStringLiteral("link")).isObject()) {
            people.insert(QStringLiteral("link"), QCoreApplication::translate("OCC::UDriveFileBrowser", "People with the link"));
            continue;
        }
        const auto previousSize = people.size();
        for (const auto *key : {"grantedToV2", "grantedTo", "grantedToIdentitiesV2", "grantedToIdentities"}) {
            const auto identities = permission.value(QLatin1String(key));
            if (identities.isObject()) add(identities.toObject());
            else for (const auto &identity : identities.toArray()) add(identity.toObject());
        }
        const auto email = permission.value(QStringLiteral("invitation")).toObject().value(QStringLiteral("email")).toString().left(256);
        if (people.size() == previousSize && !email.isEmpty() && people.size() < 256) people.insert(QStringLiteral("email:") + email, email);
    }
    return people.values();
}

QString personInitials(const QString &name)
{
    QString clean = name.trimmed().simplified();
    clean.remove(QRegularExpression(QStringLiteral("[\\p{Cc}\\x{202a}-\\x{202e}\\x{2066}-\\x{2069}]")));
    const auto words = clean.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QString initials;
    for (const auto &word : words) {
        QTextBoundaryFinder boundary(QTextBoundaryFinder::Grapheme, word);
        const auto end = boundary.toNextBoundary();
        if (end > 0) initials += word.left(end).toUpper();
        if (initials.size() >= 2) break;
    }
    return initials.isEmpty() ? QStringLiteral("?") : initials;
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
    FileRowDelegate(QTreeView *tree, std::function<void(const QModelIndex &, int)> action,
        std::function<bool(const QModelIndex &, int)> enabled)
        : QStyledItemDelegate(tree)
        , _tree(tree)
        , _action(std::move(action))
        , _enabled(std::move(enabled))
    {
        if (_tree && _tree->viewport()) {
            _tree->viewport()->installEventFilter(this);
        }
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (_tree && watched == _tree->viewport()) {
            if (event->type() == QEvent::MouseMove) {
                const auto *mouse = static_cast<QMouseEvent *>(event);
                const QPoint pos = mouse->position().toPoint();
                const QModelIndex index = _tree->indexAt(pos);
                const QPersistentModelIndex rowName = index.isValid() ? QPersistentModelIndex(index.siblingAtColumn(0)) : QPersistentModelIndex();
                if (rowName != _hoveredRow) {
                    if (_hoveredRow.isValid()) {
                        _tree->viewport()->update(rowRect(_hoveredRow));
                    }
                    _hoveredRow = rowName;
                    if (_hoveredRow.isValid()) {
                        _tree->viewport()->update(rowRect(_hoveredRow));
                    }
                } else if (index.isValid() && index.column() == 4) {
                    _tree->viewport()->update(_tree->visualRect(index));
                }

                bool overAction = false;
                if (index.isValid() && index.column() == 4) {
                    const auto name = index.siblingAtColumn(0);
                    const QRect rect = _tree->visualRect(index);
                    for (int action = RowFavorite; action <= RowMore; ++action) {
                        if (visible(name, action) && hitRect(actionRect(rect, action)).contains(pos)) {
                            overAction = true;
                            break;
                        }
                    }
                }
                _tree->viewport()->setCursor(overAction ? Qt::PointingHandCursor : Qt::ArrowCursor);
            } else if (event->type() == QEvent::Leave) {
                if (_hoveredRow.isValid()) {
                    const auto old = _hoveredRow;
                    _hoveredRow = QPersistentModelIndex();
                    _tree->viewport()->update(rowRect(old));
                }
                _tree->viewport()->setCursor(Qt::ArrowCursor);
            }
        }
        return QStyledItemDelegate::eventFilter(watched, event);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        auto adjusted = option;
        const auto name = index.siblingAtColumn(0);
        if (isRowHovered(name)) {
            adjusted.state |= QStyle::State_MouseOver;
        } else {
            adjusted.state &= ~QStyle::State_MouseOver;
        }
        if (index.column() == 0) {
            adjusted.rect.adjust(34, 0, 0, 0);
            if (_tree->property("driveSharesOverview").toBool()) {
                auto background = adjusted;
                initStyleOption(&background, index);
                background.text.clear(); background.icon = QIcon();
                _tree->style()->drawControl(QStyle::CE_ItemViewItem, &background, painter, _tree);
                const auto textColor = option.palette.color(option.state & QStyle::State_Selected ? QPalette::HighlightedText : QPalette::Text);
                const auto icon = qvariant_cast<QIcon>(name.data(Qt::DecorationRole));
                icon.paint(painter, QRect(adjusted.rect.left() + 4, option.rect.center().y() - 12, 24, 24));
                QRect textRect = adjusted.rect.adjusted(38, 4, -8, -4);
                painter->save(); painter->setPen(textColor);
                painter->drawText(textRect.adjusted(0, 0, 0, -18), Qt::AlignLeft | Qt::AlignVCenter,
                    option.fontMetrics.elidedText(name.data().toString(), Qt::ElideRight, textRect.width()));
                auto subtitle = textRect; subtitle.setTop(subtitle.bottom() - 16);
                auto muted = textColor; muted.setAlpha(175); painter->setPen(muted);
                auto small = option.font; small.setPointSizeF(qMax(9.0, small.pointSizeF() - 1)); painter->setFont(small);
                UDriveIcons::icon(UDriveIcons::Symbol::Folder).paint(painter, QRect(subtitle.left(), subtitle.top() + 1, 14, 14));
                subtitle.adjust(19, 0, 0, 0);
                painter->drawText(subtitle, Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(small).elidedText(name.data(LocationRole).toString(), Qt::ElideMiddle, subtitle.width()));
                painter->restore();
            } else QStyledItemDelegate::paint(painter, adjusted, index);
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
        if (index.column() == 3 && _tree->property("driveSharesOverview").toBool()) {
            auto background = adjusted; initStyleOption(&background, index); background.text.clear();
            _tree->style()->drawControl(QStyle::CE_ItemViewItem, &background, painter, _tree);
            const auto people = name.data(SharePeopleRole).toStringList();
            painter->save(); painter->setRenderHint(QPainter::Antialiasing);
            const int count = qMin(people.size(), 3);
            const int left = option.rect.left() + 8;
            auto font = option.font; font.setPointSize(9); painter->setFont(font);
            for (int i = 0; i < count; ++i) {
                const QRect avatar(left + i * 25, option.rect.center().y() - 14, 28, 28);
                painter->setPen(option.palette.color(QPalette::Base));
                painter->setBrush(QColor::fromHsv(qHash(people.at(i)) % 360, 100, 115));
                painter->drawEllipse(avatar);
                painter->setPen(Qt::white);
                painter->drawText(avatar, Qt::AlignCenter, personInitials(people.at(i)));
            }
            painter->setPen(option.palette.color(QPalette::Text));
            if (people.size() > count) painter->drawText(option.rect.adjusted(88, 0, -4, 0), Qt::AlignVCenter, QStringLiteral("+%1").arg(people.size() - count));
            if (people.isEmpty()) painter->drawText(option.rect.adjusted(8, 0, -4, 0), Qt::AlignVCenter, QStringLiteral("—"));
            painter->restore();
            return;
        }
        if (index.column() == 4) {
            QStyledItemDelegate::paint(painter, adjusted, index);
            const bool isFavorite = name.data(FavoriteRole).toBool();
            const UDriveIcons::Symbol symbols[]{
                isFavorite ? UDriveIcons::Symbol::StarFilled : UDriveIcons::Symbol::Star,
                UDriveIcons::Symbol::Share,
                UDriveIcons::Symbol::Link,
                UDriveIcons::Symbol::MoreVertical
            };
            for (int action = RowFavorite; action <= RowMore; ++action) {
                if (!visible(name, action)) continue;
                const bool enabled = _enabled(name, action);
                const QRect icon = actionRect(option.rect, action);
                const bool iconHovered = enabled && _tree && _tree->viewport() && _tree->viewport()->underMouse()
                    && hitRect(icon).contains(_tree->viewport()->mapFromGlobal(QCursor::pos()));
                if (iconHovered) {
                    painter->save();
                    painter->setPen(Qt::NoPen);
                    auto color = option.palette.color(QPalette::Text); color.setAlpha(22);
                    painter->setBrush(color);
                    painter->drawRoundedRect(hitRect(icon), 6, 6);
                    painter->restore();
                }
                UDriveIcons::icon(symbols[action]).paint(painter, icon, Qt::AlignCenter, enabled ? QIcon::Normal : QIcon::Disabled);
            }
            return;
        }
        QStyledItemDelegate::paint(painter, adjusted, index);
    }
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(qMax(_tree->property("driveSharesOverview").toBool() ? 52 : _tree->property("driveCondensed").toBool() ? 30 : 42, size.height()));
        return size;
    }
    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option, const QModelIndex &index) override
    {
        if (event->type() == QEvent::MouseButtonRelease) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton && index.column() == 4) {
                const auto name = index.siblingAtColumn(0);
                for (int action = RowFavorite; action <= RowMore; ++action)
                    if (visible(name, action) && hitRect(actionRect(option.rect, action)).contains(mouse->position().toPoint())) {
                        if (_enabled(name, action)) _action(name, action);
                        return true;
                    }
            }
        }
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }

    bool helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option, const QModelIndex &index) override
    {
        if (event->type() == QEvent::ToolTip && index.column() == 4) {
            const auto name = index.siblingAtColumn(0);
            const char *labels[]{QT_TRANSLATE_NOOP("OCC::UDriveFileBrowser", "Favorite"),
                QT_TRANSLATE_NOOP("OCC::UDriveFileBrowser", "Share this item with other users"),
                QT_TRANSLATE_NOOP("OCC::UDriveFileBrowser", "Copy sign-in link"), QT_TRANSLATE_NOOP("OCC::UDriveFileBrowser", "Actions")};
            for (int action = RowFavorite; action <= RowMore; ++action) {
                if (visible(name, action) && hitRect(actionRect(option.rect, action)).contains(event->pos())) {
                    QString label;
                    if (action == RowFavorite) {
                        label = name.data(FavoriteRole).toBool()
                            ? QCoreApplication::translate("OCC::UDriveFileBrowser", "Remove from Favorites")
                            : QCoreApplication::translate("OCC::UDriveFileBrowser", "Add to Favorites");
                    } else {
                        label = QCoreApplication::translate("OCC::UDriveFileBrowser", labels[action]);
                    }
                    QToolTip::showText(event->globalPos(), label, view->viewport());
                    return true;
                }
            }
            QToolTip::hideText();
            return true;
        }
        return QStyledItemDelegate::helpEvent(event, view, option, index);
    }

private:
    static QRect actionRect(const QRect &row, int action) { return QRect(row.right() - 138 + action * 34, row.center().y() - 9, 18, 18); }
    static QRect hitRect(const QRect &icon) { return icon.adjusted(-5, -5, 5, 5); }

    QRect rowRect(const QPersistentModelIndex &index) const
    {
        if (!index.isValid() || !_tree || !_tree->viewport()) return {};
        const QRect rect = _tree->visualRect(index);
        return QRect(0, rect.top(), _tree->viewport()->width(), rect.height());
    }

    bool isRowHovered(const QModelIndex &name) const
    {
        if (_hoveredRow.isValid() && _hoveredRow == name)
            return true;
        if (!_tree || !_tree->viewport() || !_tree->viewport()->underMouse())
            return false;
        const QPoint pos = _tree->viewport()->mapFromGlobal(QCursor::pos());
        const QModelIndex atPos = _tree->indexAt(pos);
        return atPos.isValid() && atPos.siblingAtColumn(0) == name;
    }

    bool visible(const QModelIndex &name, int action) const
    {
        if (action == RowMore) return true;
        if (_tree->property("driveSharesOverview").toBool()) return false;
        if (name.data(TrashedRole).toBool()) return false;
        if (action == RowFavorite)
            return name.data(FavoriteRole).toBool() || isRowHovered(name);
        return true;
    }

    QTreeView *_tree = nullptr;
    std::function<void(const QModelIndex &, int)> _action;
    std::function<bool(const QModelIndex &, int)> _enabled;
    QPersistentModelIndex _hoveredRow;
};

class FileTileDelegate : public QStyledItemDelegate
{
public:
    FileTileDelegate(QListView *view, std::function<void(const QModelIndex &)> more)
        : QStyledItemDelegate(view), _more(std::move(more)) { }
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return QSize(196, 210); }
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
        const QString sizeStr = index.siblingAtColumn(1).data().toString();
        const QString dateStr = index.siblingAtColumn(2).data().toString();
        const QString locStr = index.siblingAtColumn(3).data().toString();
        QString metadata = sizeStr;
        if (sizeStr == QStringLiteral("0 bytes") || sizeStr == QStringLiteral("—")) {
            if (!dateStr.isEmpty() && dateStr != QStringLiteral("—"))
                metadata = dateStr;
            else if (!locStr.isEmpty() && locStr != QStringLiteral("—"))
                metadata = locStr;
        }
        painter->drawText(detail, Qt::AlignVCenter | Qt::AlignLeft, painter->fontMetrics().elidedText(metadata, Qt::ElideRight, detail.width()));
        UDriveIcons::icon(UDriveIcons::Symbol::MoreVertical).paint(painter, moreRect(option.rect));
        const QRect check(card.left() + 10, card.top() + 10, 18, 18);
        painter->setPen(QPen(selected ? QColor("#40D979") : QColor("#858585"), 1.5));
        painter->setBrush(selected ? QColor("#40D979") : Qt::transparent);
        painter->drawRoundedRect(check, 4, 4);
        if (selected) {
            painter->setPen(QPen(QColor("#143B20"), 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter->drawLine(check.topLeft() + QPoint(4, 9), check.topLeft() + QPoint(8, 13));
            painter->drawLine(check.topLeft() + QPoint(8, 13), check.topLeft() + QPoint(14, 5));
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

class WorkspaceCardDelegate : public QStyledItemDelegate
{
public:
    WorkspaceCardDelegate(QListView *view,
                          std::function<void(const QModelIndex &)> memberAction,
                          std::function<void(const QModelIndex &)> moreAction)
        : QStyledItemDelegate(view)
        , _memberAction(std::move(memberAction))
        , _moreAction(std::move(moreAction))
    {
    }

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override
    {
        return QSize(246, 190);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const bool dark = option.palette.color(QPalette::Window).lightness() < 128;
        const bool selected = option.state & QStyle::State_Selected;
        const bool hover = option.state & QStyle::State_MouseOver;
        const QRect card = option.rect.adjusted(6, 6, -6, -6);

        painter->save();
        painter->setClipRect(card);
        painter->setRenderHint(QPainter::Antialiasing);

        const bool disabled = index.data(UDriveFileBrowser::SpaceDisabledRole).toBool();
        const QColor bg = selected ? (dark ? QColor("#2D4233") : QColor("#E3F6E9"))
            : hover                ? (dark ? QColor("#404040") : QColor("#F0F4F2"))
                                   : (dark ? QColor("#383838") : QColor("#F4F5F4"));
        painter->setPen(selected ? QPen(QColor("#40D979"), 2) : QPen(Qt::NoPen));
        painter->setBrush(bg);
        painter->drawRoundedRect(card.adjusted(1, 1, -1, -1), 7, 7);

        const QRect check(card.left() + 12, card.top() + 12, 18, 18);
        painter->setPen(QPen(selected ? QColor("#40D979") : QColor(dark ? "#5A6270" : "#A2ABB5"), 1.5));
        painter->setBrush(selected ? QColor("#40D979") : Qt::transparent);
        painter->drawRoundedRect(check, 4, 4);
        if (selected) {
            painter->setPen(QPen(QColor("#143B20"), 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter->drawLine(check.topLeft() + QPoint(4, 9), check.topLeft() + QPoint(8, 13));
            painter->drawLine(check.topLeft() + QPoint(8, 13), check.topLeft() + QPoint(14, 5));
        }

        const QRect iconRect(card.center().x() - 22, card.top() + 48, 44, 44);
        const QColor gridColor = dark ? QColor("#C8D0D8") : QColor("#4A525A");
        const QIcon image = index.data(Qt::DecorationRole).value<QIcon>();
        if (image.isNull()) UDriveIcons::icon(UDriveIcons::Symbol::Grid, false, gridColor).paint(painter, iconRect);
        else image.paint(painter, QRect(card.left() + 12, card.top() + 34, card.width() - 24, 98));

        const QRect titleRect(card.left() + 14, card.bottom() - 32, card.width() - 76, 22);
        painter->setPen(QColor(dark ? "#FFFFFF" : "#1A201C"));
        auto font = option.font;
        font.setWeight(QFont::Normal);
        font.setPointSizeF(qMax(10.0, font.pointSizeF()));
        painter->setFont(font);
        painter->drawText(titleRect, Qt::AlignVCenter | Qt::AlignLeft, painter->fontMetrics().elidedText(index.data().toString(), Qt::ElideRight, titleRect.width()));

        if (disabled) {
            const QString label = QCoreApplication::translate("OCC::UDriveFileBrowser", "Disabled");
            painter->setPen(QColor(dark ? "#AAB1AD" : "#68736C"));
            painter->drawText(card.adjusted(42, 10, -12, 0), Qt::AlignTop | Qt::AlignRight, label);
        }
        const QColor actionColor = dark ? QColor("#C4C8C5") : QColor("#6B7580");
        UDriveIcons::icon(UDriveIcons::Symbol::Members, false, actionColor).paint(painter, memberRect(option.rect));
        UDriveIcons::icon(UDriveIcons::Symbol::MoreVertical, false, actionColor).paint(painter, moreRect(option.rect));

        painter->restore();
    }

    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option, const QModelIndex &index) override
    {
        if (event->type() == QEvent::MouseButtonRelease) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton) {
                const QPoint pt = mouse->position().toPoint();
                if (memberRect(option.rect).adjusted(-4, -4, 4, 4).contains(pt)) {
                    if (!index.data(UDriveFileBrowser::SpaceDisabledRole).toBool() && _memberAction) _memberAction(index);
                    return true;
                }
                if (moreRect(option.rect).adjusted(-4, -4, 4, 4).contains(pt)) {
                    if (_moreAction) _moreAction(index);
                    return true;
                }
            }
        }
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }

private:
    static QRect memberRect(const QRect &rect)
    {
        const QRect card = rect.adjusted(6, 6, -6, -6);
        return QRect(card.right() - 54, card.bottom() - 31, 20, 20);
    }
    static QRect moreRect(const QRect &rect)
    {
        const QRect card = rect.adjusted(6, 6, -6, -6);
        return QRect(card.right() - 28, card.bottom() - 31, 20, 20);
    }

    std::function<void(const QModelIndex &)> _memberAction;
    std::function<void(const QModelIndex &)> _moreAction;
};

// The first header cell leaves space for a real, keyboard-accessible checkbox.
class WorkspaceHeader : public QHeaderView
{
public:
    explicit WorkspaceHeader(QWidget *parent) : QHeaderView(Qt::Horizontal, parent)
    {
        selectAll = new SelectAllCheckBox(QString(), viewport());
        selectAll->setObjectName(QStringLiteral("driveWorkspaceHeaderSelectAll"));
        selectAll->setAccessibleName(QCoreApplication::translate("OCC::UDriveFileBrowser", "Select all"));
        selectAll->setTristate(true);
        selectAll->setFixedSize(22, 22);
    }
    QCheckBox *selectAll;
protected:
    void paintSection(QPainter *painter, const QRect &rect, int logicalIndex) const override
    {
        if (logicalIndex == 0) {
            QStyleOptionHeader section;
            initStyleOptionForIndex(&section, logicalIndex);
            section.rect = rect;
            style()->drawControl(QStyle::CE_HeaderSection, &section, painter, this);
        }
        QHeaderView::paintSection(painter, logicalIndex == 0 ? rect.adjusted(34, 0, 0, 0) : rect, logicalIndex);
    }
    void resizeEvent(QResizeEvent *event) override
    {
        QHeaderView::resizeEvent(event);
        selectAll->move(8, (height() - selectAll->height()) / 2);
    }
    void scrollContentsBy(int dx, int dy) override
    {
        QHeaderView::scrollContentsBy(dx, dy);
        selectAll->move(sectionViewportPosition(0) + 8, (height() - selectAll->height()) / 2);
    }
};

class WorkspaceTableDelegate : public QStyledItemDelegate
{
public:
    WorkspaceTableDelegate(QTreeView *view, std::function<void(const QModelIndex &)> more)
        : QStyledItemDelegate(view), _more(std::move(more)) { }
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(qMax(48, size.height()));
        return size;
    }
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        if (index.column() != 0 && index.column() != 5) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        auto background = option;
        initStyleOption(&background, index);
        background.text.clear(); background.icon = QIcon();
        const auto *widget = option.widget;
        (widget ? widget->style() : QApplication::style())->drawControl(QStyle::CE_ItemViewItem, &background, painter, widget);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setClipRect(option.rect);
        if (index.column() == 0) {
            const bool selected = option.state & QStyle::State_Selected;
            const QRect check(option.rect.left() + 8, option.rect.center().y() - 9, 18, 18);
            painter->setPen(QPen(selected ? QColor("#40D979") : QColor("#858585"), 1.5));
            painter->setBrush(selected ? QColor("#40D979") : Qt::transparent);
            painter->drawRoundedRect(check, 4, 4);
            if (selected) {
                painter->setPen(QPen(QColor("#143B20"), 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter->drawLine(check.topLeft() + QPoint(4, 9), check.topLeft() + QPoint(8, 13));
                painter->drawLine(check.topLeft() + QPoint(8, 13), check.topLeft() + QPoint(14, 5));
            }
            const auto image = index.data(Qt::DecorationRole).value<QIcon>();
            (image.isNull() ? UDriveIcons::icon(UDriveIcons::Symbol::Grid) : image).paint(painter, QRect(option.rect.left() + 44, option.rect.center().y() - 10, 20, 20));
            const QRect text = option.rect.adjusted(78, 0, -8, 0);
            painter->setPen(option.palette.color(QPalette::Text));
            QString label = index.data().toString();
            if (index.data(UDriveFileBrowser::SpaceDisabledRole).toBool()) {
                label += QStringLiteral(" · ") + QCoreApplication::translate("OCC::UDriveFileBrowser", "Disabled");
                painter->setPen(option.palette.color(QPalette::Disabled, QPalette::Text));
            }
            painter->drawText(text, Qt::AlignLeft | Qt::AlignVCenter, option.fontMetrics.elidedText(label, Qt::ElideRight, text.width()));
        } else {
            UDriveIcons::icon(UDriveIcons::Symbol::MoreVertical).paint(painter, QRect(option.rect.center() - QPoint(10, 10), QSize(20, 20)));
        }
        painter->restore();
    }
    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option, const QModelIndex &index) override
    {
        if (index.column() == 5 && event->type() == QEvent::MouseButtonRelease
            && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
            _more(index.siblingAtColumn(0));
            return true;
        }
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }
private:
    std::function<void(const QModelIndex &)> _more;
};

class FilesProxy : public QSortFilterProxyModel
{
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    void setRecent(bool recent) { _recent = recent; invalidate(); }
    void setTypeFilter(int filter) { _typeFilter = filter; invalidateFilter(); }
    void setSharesSection(bool shares) { _shares = shares; invalidateFilter(); }
    void setShareFilter(bool overview, bool hidden, const QString &sharer) {
        if (_shareOverview == overview && _hidden == hidden && _sharer == sharer) return;
        _shareOverview = overview; _hidden = hidden; _sharer = sharer; invalidateFilter();
    }
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override
    {
        if (_shares) {
            const auto item = sourceModel()->index(sourceRow, 0, sourceParent);
            if (_shareOverview && (item.data(ShareHiddenRole).toBool() != _hidden
                || (!_sharer.isEmpty() && !item.data(ShareSharersRole).toStringList().contains(_sharer)))) return false;
            if (_typeFilter == 1) {
                const auto idx = sourceModel()->index(sourceRow, 0, sourceParent);
                if (idx.data(DirectoryRole).toBool()) return false;
            } else if (_typeFilter == 2) {
                const auto idx = sourceModel()->index(sourceRow, 0, sourceParent);
                if (!idx.data(DirectoryRole).toBool()) return false;
            }
        }
        return QSortFilterProxyModel::filterAcceptsRow(sourceRow, sourceParent);
    }
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
    bool _shares = false;
    int _typeFilter = 0;
    bool _shareOverview = false;
    bool _hidden = false;
    QString _sharer;
};
}

DeleteConfirmDialog::DeleteConfirmDialog(const QString &fileName, bool isFolder, QWidget *parent, bool permanent, int trashCount)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("deleteConfirmDialog"));
    setWindowTitle(trashCount > 0 ? tr("Empty trash") : permanent ? tr("Delete permanently") : tr("Move to Deleted files"));
    setModal(true);
    setWindowFlags((windowFlags() | Qt::Dialog) & ~Qt::WindowContextHelpButtonHint);
    setMinimumWidth(440);

    const bool dark = QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
    const QString bg = dark ? QStringLiteral("#242528") : QStringLiteral("#FFFFFF");
    const QString text = dark ? QStringLiteral("#EDEDED") : QStringLiteral("#1F2328");
    const QString subtext = dark ? QStringLiteral("#9BA3AF") : QStringLiteral("#59636E");
    const QString border = dark ? QStringLiteral("#3E4146") : QStringLiteral("#D0D7DE");
    const QString cancelBg = dark ? QStringLiteral("#2E3135") : QStringLiteral("#F3F4F6");
    const QString cancelHover = dark ? QStringLiteral("#383B40") : QStringLiteral("#E5E7EB");

    setStyleSheet(QStringLiteral(
        "QDialog#deleteConfirmDialog { background-color: %1; }"
        "QLabel#confirmTitle { color: %3; font-size: 14px; font-weight: 600; }"
        "QLabel#confirmMessage { color: %4; font-size: 12px; }"
        "QPushButton#cancelDeleteButton { background-color: %5; color: %3; border: 1px solid %2; border-radius: 8px; padding: 7px 16px; font-size: 13px; font-weight: 500; min-width: 80px; }"
        "QPushButton#cancelDeleteButton:hover { background-color: %6; }"
        "QPushButton#cancelDeleteButton:focus { border: 1px solid #40D979; }"
        "QPushButton#confirmDeleteButton { background-color: #E03838; color: #FFFFFF; border: none; border-radius: 8px; padding: 7px 18px; font-size: 13px; font-weight: 600; min-width: 80px; }"
        "QPushButton#confirmDeleteButton:hover { background-color: #EF4444; }"
        "QPushButton#confirmDeleteButton:pressed { background-color: #B91C1C; }"
        "QPushButton#confirmDeleteButton:focus { border: 2px solid #FCA5A5; }"
    ).arg(bg, border, text, subtext, cancelBg, cancelHover));

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(20, 20, 20, 16);
    mainLayout->setSpacing(18);

    auto *textLayout = new QVBoxLayout;
    textLayout->setSpacing(5);
    textLayout->setAlignment(Qt::AlignVCenter);

    auto *titleLabel = new QLabel((trashCount > 0 ? tr("Empty Deleted files in “%1”?") : permanent ? tr("Permanently delete “%1”?") : tr("Move “%1” to Deleted files?")).arg(fileName), this);
    titleLabel->setObjectName(QStringLiteral("confirmTitle"));
    titleLabel->setTextFormat(Qt::PlainText);
    titleLabel->setWordWrap(true);
    textLayout->addWidget(titleLabel);

    const QString descText = trashCount > 0
        ? tr("All %n item(s) in this space’s Deleted files will be permanently deleted, including items hidden by search and the contents of deleted folders. This action cannot be undone.", nullptr, trashCount)
        : permanent
        ? (isFolder ? tr("This folder and all of its contents will be permanently deleted. This action cannot be undone.")
                    : tr("This item will be permanently deleted. This action cannot be undone."))
        : (isFolder ? tr("All contents inside this folder will also be moved to Deleted files. You can restore them anytime.")
                    : tr("This item will be moved to Deleted files. You can restore it anytime."));
    auto *messageLabel = new QLabel(descText, this);
    messageLabel->setObjectName(QStringLiteral("confirmMessage"));
    messageLabel->setTextFormat(Qt::PlainText);
    messageLabel->setWordWrap(true);
    textLayout->addWidget(messageLabel);

    mainLayout->addLayout(textLayout);

    auto *buttonsLayout = new QHBoxLayout;
    buttonsLayout->setSpacing(10);
    buttonsLayout->addStretch(1);

    auto *cancelButton = new QPushButton(tr("Cancel"), this);
    cancelButton->setObjectName(QStringLiteral("cancelDeleteButton"));
    cancelButton->setCursor(Qt::PointingHandCursor);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    buttonsLayout->addWidget(cancelButton);

    auto *deleteButton = new QPushButton(trashCount > 0 ? tr("Empty trash") : permanent ? tr("Delete permanently") : tr("Delete"), this);
    deleteButton->setObjectName(QStringLiteral("confirmDeleteButton"));
    deleteButton->setCursor(Qt::PointingHandCursor);
    connect(deleteButton, &QPushButton::clicked, this, &QDialog::accept);
    buttonsLayout->addWidget(deleteButton);

    mainLayout->addLayout(buttonsLayout);

    cancelButton->setDefault(true);
}

UDriveInputDialog::UDriveInputDialog(const QString &title, const QString &label, const QString &initialText,
                                     const QString &confirmButtonText, QWidget *parent, bool allowEmpty)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("udriveInputDialog"));
    setWindowTitle(title);
    setModal(true);
    setWindowFlags((windowFlags() | Qt::Dialog) & ~Qt::WindowContextHelpButtonHint);
    setMinimumWidth(440);

    const bool dark = QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
    const QString bg = dark ? QStringLiteral("#242528") : QStringLiteral("#FFFFFF");
    const QString text = dark ? QStringLiteral("#EDEDED") : QStringLiteral("#1F2328");
    const QString subtext = dark ? QStringLiteral("#9BA3AF") : QStringLiteral("#59636E");
    const QString border = dark ? QStringLiteral("#3E4146") : QStringLiteral("#D0D7DE");
    const QString inputBg = dark ? QStringLiteral("#1B1C1E") : QStringLiteral("#FAFCFB");
    const QString cancelBg = dark ? QStringLiteral("#2E3135") : QStringLiteral("#F3F4F6");
    const QString cancelHover = dark ? QStringLiteral("#383B40") : QStringLiteral("#E5E7EB");
    const QString primaryDisabled = dark ? QStringLiteral("#2A4733") : QStringLiteral("#A3E4B8");
    const QString primaryDisabledText = dark ? QStringLiteral("#587864") : QStringLiteral("#FFFFFF");

    setStyleSheet(QStringLiteral(
        "QDialog#udriveInputDialog { background-color: %1; }"
        "QLabel#inputDialogTitle { color: %3; font-size: 15px; font-weight: 600; }"
        "QLabel#inputDialogLabel { color: %4; font-size: 13px; }"
        "QLineEdit#inputDialogField { min-height: 20px; padding: 8px 12px; border: 1px solid %2; border-radius: 8px; background: %5; color: %3; font-size: 13px; selection-background-color: #2E4436; }"
        "QLineEdit#inputDialogField:focus { border: 1px solid #22C55E; }"
        "QLineEdit#inputDialogField QToolButton { border: none; border-radius: 12px; padding: 0; background: transparent; }"
        "QPushButton#inputCancelButton { background-color: %6; color: %3; border: 1px solid %2; border-radius: 8px; padding: 7px 16px; font-size: 13px; font-weight: 500; min-width: 80px; min-height: 20px; }"
        "QPushButton#inputCancelButton:hover { background-color: %7; }"
        "QPushButton#inputCancelButton:focus { border: 1px solid #22C55E; }"
        "QPushButton#inputConfirmButton { background-color: #22C55E; color: #FFFFFF; border: none; border-radius: 8px; padding: 7px 18px; font-size: 13px; font-weight: 600; min-width: 80px; min-height: 20px; }"
        "QPushButton#inputConfirmButton:hover { background-color: #2AD564; }"
        "QPushButton#inputConfirmButton:pressed { background-color: #16A34A; }"
        "QPushButton#inputConfirmButton:disabled { background-color: %8; color: %9; }"
        "QPushButton#inputConfirmButton:focus { border: 2px solid #86EFAC; }"
    ).arg(bg, border, text, subtext, inputBg, cancelBg, cancelHover, primaryDisabled, primaryDisabledText));

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(20, 20, 20, 16);
    mainLayout->setSpacing(16);

    auto *headerLayout = new QVBoxLayout;
    headerLayout->setSpacing(6);

    auto *titleLabel = new QLabel(title, this);
    titleLabel->setObjectName(QStringLiteral("inputDialogTitle"));
    titleLabel->setTextFormat(Qt::PlainText);
    headerLayout->addWidget(titleLabel);

    if (!label.isEmpty()) {
        auto *fieldLabel = new QLabel(label, this);
        fieldLabel->setObjectName(QStringLiteral("inputDialogLabel"));
        fieldLabel->setTextFormat(Qt::PlainText);
        fieldLabel->setWordWrap(true);
        headerLayout->addWidget(fieldLabel);
    }
    mainLayout->addLayout(headerLayout);

    _lineEdit = new QLineEdit(initialText, this);
    _lineEdit->setObjectName(QStringLiteral("inputDialogField"));
    _lineEdit->setClearButtonEnabled(true);
    if (!initialText.isEmpty()) {
        _lineEdit->selectAll();
    }
    mainLayout->addWidget(_lineEdit);

    auto *buttonsLayout = new QHBoxLayout;
    buttonsLayout->setSpacing(10);
    buttonsLayout->addStretch(1);

    auto *cancelButton = new QPushButton(tr("Cancel"), this);
    cancelButton->setObjectName(QStringLiteral("inputCancelButton"));
    cancelButton->setCursor(Qt::PointingHandCursor);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    buttonsLayout->addWidget(cancelButton);

    _confirmButton = new QPushButton(confirmButtonText.isEmpty() ? tr("OK") : confirmButtonText, this);
    _confirmButton->setObjectName(QStringLiteral("inputConfirmButton"));
    _confirmButton->setCursor(Qt::PointingHandCursor);
    _confirmButton->setDefault(true);
    connect(_confirmButton, &QPushButton::clicked, this, &QDialog::accept);
    buttonsLayout->addWidget(_confirmButton);

    mainLayout->addLayout(buttonsLayout);

    if (!allowEmpty) {
        auto updateConfirmState = [this] {
            _confirmButton->setEnabled(!_lineEdit->text().trimmed().isEmpty());
        };
        connect(_lineEdit, &QLineEdit::textChanged, this, updateConfirmState);
        updateConfirmState();
    }

    connect(_lineEdit, &QLineEdit::returnPressed, this, [this] {
        if (_confirmButton->isEnabled()) {
            accept();
        }
    });

    _lineEdit->setFocus();
}

QString UDriveInputDialog::text() const
{
    return _lineEdit ? _lineEdit->text().trimmed() : QString();
}

QString UDriveInputDialog::getText(QWidget *parent, const QString &title, const QString &label,
                                   const QString &initialText, const QString &confirmButtonText,
                                   bool *ok, bool allowEmpty)
{
    UDriveInputDialog dialog(title, label, initialText, confirmButtonText, parent, allowEmpty);
    const int result = dialog.exec();
    if (ok)
        *ok = (result == QDialog::Accepted);
    return (result == QDialog::Accepted) ? dialog.text() : QString();
}

UDriveMoveDialog::UDriveMoveDialog(AccountState *accountState, const QUrl &rootUrl, const QString &spaceName,
                                   const QString &sourcePath, const QString &sourceName, bool isFolder,
                                   const QList<QPair<QString, QString>> &initialFolders,
                                   QWidget *parent)
    : QDialog(parent)
    , _accountState(accountState)
    , _rootUrl(rootUrl)
    , _spaceName(spaceName)
    , _sourcePath(sourcePath)
    , _sourceName(sourceName.isEmpty() ? sourcePath.section(QLatin1Char('/'), -1) : sourceName)
    , _isFolder(isFolder)
{
    const int lastSlash = _sourcePath.lastIndexOf(QLatin1Char('/'));
    _currentParentFolder = (lastSlash >= 0) ? _sourcePath.left(lastSlash) : QString();

    setObjectName(QStringLiteral("udriveMoveDialog"));
    setWindowTitle(tr("Move item"));
    setModal(true);
    setWindowFlags((windowFlags() | Qt::Dialog) & ~Qt::WindowContextHelpButtonHint);
    resize(480, 420);
    setMinimumSize(420, 360);

    const bool dark = QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
    const QString bg = dark ? QStringLiteral("#242528") : QStringLiteral("#FFFFFF");
    const QString text = dark ? QStringLiteral("#EDEDED") : QStringLiteral("#1F2328");
    const QString subtext = dark ? QStringLiteral("#9BA3AF") : QStringLiteral("#59636E");
    const QString border = dark ? QStringLiteral("#3E4146") : QStringLiteral("#D0D7DE");
    const QString inputBg = dark ? QStringLiteral("#1B1C1E") : QStringLiteral("#FAFCFB");
    const QString cancelBg = dark ? QStringLiteral("#2E3135") : QStringLiteral("#F3F4F6");
    const QString cancelHover = dark ? QStringLiteral("#383B40") : QStringLiteral("#E5E7EB");
    const QString primaryDisabled = dark ? QStringLiteral("#2A4733") : QStringLiteral("#A3E4B8");
    const QString primaryDisabledText = dark ? QStringLiteral("#587864") : QStringLiteral("#FFFFFF");
    const QString treeItemSelected = dark ? QStringLiteral("#2A3E31") : QStringLiteral("#DCFCE7");
    const QString treeItemTextSelected = dark ? QStringLiteral("#4ADE80") : QStringLiteral("#15803D");

    setStyleSheet(QStringLiteral(
        "QDialog#udriveMoveDialog { background-color: %1; }"
        "QLabel#moveDialogTitle { color: %3; font-size: 15px; font-weight: 600; }"
        "QLabel#moveDialogSubtitle { color: %4; font-size: 13px; }"
        "QLabel#moveDialogDestination { color: %4; font-size: 12px; }"
        "QTreeWidget#moveDialogTree { background-color: %5; border: 1px solid %2; border-radius: 8px; color: %3; font-size: 13px; outline: none; padding: 4px; selection-background-color: transparent; show-decoration-selected: 1; }"
        "QTreeWidget#moveDialogTree:focus { outline: none; }"
        "QTreeWidget#moveDialogTree::item { padding: 5px 4px; border-radius: 6px; min-height: 22px; outline: none; border: none; }"
        "QTreeWidget#moveDialogTree::item:hover { background-color: %7; }"
        "QTreeWidget#moveDialogTree::item:selected { background-color: %10; color: %11; }"
        "QPushButton#moveNewFolderButton { background-color: transparent; color: #22C55E; border: 1px solid #22C55E; border-radius: 8px; padding: 7px 14px; font-size: 13px; font-weight: 500; min-height: 20px; }"
        "QPushButton#moveNewFolderButton:hover { background-color: rgba(34, 197, 94, 0.1); }"
        "QPushButton#moveNewFolderButton:pressed { background-color: rgba(34, 197, 94, 0.2); }"
        "QPushButton#moveCancelButton { background-color: %6; color: %3; border: 1px solid %2; border-radius: 8px; padding: 7px 16px; font-size: 13px; font-weight: 500; min-width: 80px; min-height: 20px; }"
        "QPushButton#moveCancelButton:hover { background-color: %7; }"
        "QPushButton#moveCancelButton:focus { border: 1px solid #22C55E; }"
        "QPushButton#moveConfirmButton { background-color: #22C55E; color: #FFFFFF; border: none; border-radius: 8px; padding: 7px 18px; font-size: 13px; font-weight: 600; min-width: 80px; min-height: 20px; }"
        "QPushButton#moveConfirmButton:hover { background-color: #2AD564; }"
        "QPushButton#moveConfirmButton:pressed { background-color: #16A34A; }"
        "QPushButton#moveConfirmButton:disabled { background-color: %8; color: %9; }"
        "QPushButton#moveConfirmButton:focus { border: 2px solid #86EFAC; }"
    ).arg(bg, border, text, subtext, inputBg, cancelBg, cancelHover, primaryDisabled, primaryDisabledText, treeItemSelected, treeItemTextSelected));

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(20, 20, 20, 16);
    mainLayout->setSpacing(12);

    auto *headerLayout = new QVBoxLayout;
    headerLayout->setSpacing(4);

    auto *titleLabel = new QLabel(tr("Move \"%1\"").arg(_sourceName), this);
    titleLabel->setObjectName(QStringLiteral("moveDialogTitle"));
    titleLabel->setTextFormat(Qt::PlainText);
    headerLayout->addWidget(titleLabel);

    auto *subLabel = new QLabel(tr("Select destination in %1").arg(_spaceName.isEmpty() ? tr("Personal") : _spaceName), this);
    subLabel->setObjectName(QStringLiteral("moveDialogSubtitle"));
    subLabel->setTextFormat(Qt::PlainText);
    headerLayout->addWidget(subLabel);

    mainLayout->addLayout(headerLayout);

    _tree = new QTreeWidget(this);
    _tree->setObjectName(QStringLiteral("moveDialogTree"));
    _tree->setAttribute(Qt::WA_MacShowFocusRect, false);
    _tree->setHeaderHidden(true);
    _tree->setAnimated(true);
    _tree->setIndentation(20);
    _tree->setSelectionMode(QAbstractItemView::SingleSelection);
    _tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    mainLayout->addWidget(_tree, 1);

    _destinationLabel = new QLabel(this);
    _destinationLabel->setObjectName(QStringLiteral("moveDialogDestination"));
    _destinationLabel->setTextFormat(Qt::PlainText);
    mainLayout->addWidget(_destinationLabel);

    auto *buttonsLayout = new QHBoxLayout;
    buttonsLayout->setSpacing(10);

    _newFolderButton = new QPushButton(tr("+ New folder"), this);
    _newFolderButton->setObjectName(QStringLiteral("moveNewFolderButton"));
    _newFolderButton->setCursor(Qt::PointingHandCursor);
    connect(_newFolderButton, &QPushButton::clicked, this, &UDriveMoveDialog::createNewFolder);
    buttonsLayout->addWidget(_newFolderButton);

    buttonsLayout->addStretch(1);

    _cancelButton = new QPushButton(tr("Cancel"), this);
    _cancelButton->setObjectName(QStringLiteral("moveCancelButton"));
    _cancelButton->setCursor(Qt::PointingHandCursor);
    connect(_cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    buttonsLayout->addWidget(_cancelButton);

    _moveButton = new QPushButton(tr("Move"), this);
    _moveButton->setObjectName(QStringLiteral("moveConfirmButton"));
    _moveButton->setCursor(Qt::PointingHandCursor);
    _moveButton->setDefault(true);
    connect(_moveButton, &QPushButton::clicked, this, &QDialog::accept);
    buttonsLayout->addWidget(_moveButton);

    mainLayout->addLayout(buttonsLayout);

    _rootItem = new QTreeWidgetItem(_tree);
    _rootItem->setIcon(0, UDriveIcons::fileIcon(QString(), true));
    QString rootText = _spaceName.isEmpty() ? tr("Personal") : _spaceName;
    if (_currentParentFolder.isEmpty()) {
        rootText += tr(" (current location)");
    }
    _rootItem->setText(0, rootText);
    _rootItem->setData(0, Qt::UserRole, QStringLiteral(""));
    _rootItem->setExpanded(true);

    populateInitialFolders(initialFolders);

    connect(_tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *, QTreeWidgetItem *) {
        updateMoveButtonState();
    });

    connect(_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *, int) {
        if (_moveButton->isEnabled()) {
            accept();
        }
    });

    connect(_tree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) {
        if (!item->data(0, Qt::UserRole + 1).toBool()) {
            for (int i = item->childCount() - 1; i >= 0; --i) {
                if (item->child(i)->data(0, Qt::UserRole + 2).toBool()) {
                    delete item->takeChild(i);
                }
            }
            auto *loadingItem = new QTreeWidgetItem(item);
            loadingItem->setText(0, tr("Loading…"));
            loadingItem->setData(0, Qt::UserRole + 2, true);
            loadFolderChildren(item, item->data(0, Qt::UserRole).toString());
        }
    });

    _tree->setCurrentItem(_rootItem);
    updateMoveButtonState();

    loadFolderChildren(_rootItem, QStringLiteral(""));
}

UDriveMoveDialog::~UDriveMoveDialog() = default;

void UDriveMoveDialog::populateInitialFolders(const QList<QPair<QString, QString>> &initialFolders)
{
    for (const auto &pair : initialFolders) {
        const QString path = pair.first;
        const QString name = pair.second;
        if (path.isEmpty() || name.isEmpty() || path.contains(QLatin1Char('/'))) continue;
        if (_isFolder && (path == _sourcePath || path.startsWith(_sourcePath + QLatin1Char('/')))) continue;

        QString itemText = name;
        if (path == _currentParentFolder) {
            itemText += tr(" (current location)");
        }
        auto *item = new QTreeWidgetItem(_rootItem);
        item->setText(0, itemText);
        item->setIcon(0, UDriveIcons::fileIcon(QString(), true));
        item->setData(0, Qt::UserRole, path);
        item->setData(0, Qt::UserRole + 1, false);
        auto *dummy = new QTreeWidgetItem(item);
        dummy->setText(0, tr("Loading…"));
        dummy->setData(0, Qt::UserRole + 2, true);
    }
}

void UDriveMoveDialog::loadFolderChildren(QTreeWidgetItem *parentItem, const QString &folderPath)
{
    if (!parentItem || parentItem->treeWidget() != _tree || !_accountState || !_accountState->account() || _rootUrl.isEmpty()) {
        if (parentItem && parentItem->treeWidget() == _tree) {
            for (int i = parentItem->childCount() - 1; i >= 0; --i) {
                if (parentItem->child(i)->data(0, Qt::UserRole + 2).toBool()) {
                    delete parentItem->takeChild(i);
                }
            }
            parentItem->setData(0, Qt::UserRole + 1, true);
        }
        return;
    }

    auto *job = new PropfindJob(_accountState->account(), _rootUrl, folderPath, PropfindJob::Depth::One, this);
    job->setTimeout(std::chrono::seconds(20));
    job->setProperties({"resourcetype"});

    connect(job, &PropfindJob::directoryListingIterated, this, [this, parentItem, folderPath](const QString &href, const QMap<QString, QString> &properties) {
        if (!parentItem || parentItem->treeWidget() != _tree) return;
        const QString prefix = withoutTrailingSlash(_rootUrl.path()) + QLatin1Char('/');
        if (!href.startsWith(prefix)) return;
        const QString relative = withoutTrailingSlash(href.mid(prefix.size()));
        if (!validPath(relative)) return;
        if (relative == folderPath || withoutTrailingSlash(href) == withoutTrailingSlash(_rootUrl.path())) return;

        const QString dirPrefix = folderPath.isEmpty() ? QString() : folderPath + QLatin1Char('/');
        if (!relative.startsWith(dirPrefix)) return;
        const QString childName = relative.mid(dirPrefix.size());
        if (childName.isEmpty() || childName.contains(QLatin1Char('/'))) return;

        const bool isDir = properties.value(QStringLiteral("resourcetype")).contains(QStringLiteral("collection"));
        if (!isDir) return;

        if (_isFolder && (relative == _sourcePath || relative.startsWith(_sourcePath + QLatin1Char('/')))) return;

        for (int i = 0; i < parentItem->childCount(); ++i) {
            auto *child = parentItem->child(i);
            if (!child->data(0, Qt::UserRole + 2).toBool() && child->data(0, Qt::UserRole).toString() == relative) {
                return;
            }
        }

        QString itemText = childName;
        if (relative == _currentParentFolder) {
            itemText += tr(" (current location)");
        }
        auto *childItem = new QTreeWidgetItem(parentItem);
        childItem->setText(0, itemText);
        childItem->setIcon(0, UDriveIcons::fileIcon(QString(), true));
        childItem->setData(0, Qt::UserRole, relative);
        childItem->setData(0, Qt::UserRole + 1, false);

        auto *dummy = new QTreeWidgetItem(childItem);
        dummy->setText(0, tr("Loading…"));
        dummy->setData(0, Qt::UserRole + 2, true);
    });

    auto cleanup = [this, parentItem] {
        if (!parentItem || parentItem->treeWidget() != _tree) return;
        for (int i = parentItem->childCount() - 1; i >= 0; --i) {
            if (parentItem->child(i)->data(0, Qt::UserRole + 2).toBool()) {
                delete parentItem->takeChild(i);
            }
        }
        parentItem->setData(0, Qt::UserRole + 1, true);
    };

    connect(job, &PropfindJob::finishedWithoutError, this, cleanup);
    connect(job, &PropfindJob::finishedWithError, this, cleanup);
    job->start();
}

void UDriveMoveDialog::createNewFolder()
{
    auto *parentItem = _tree ? _tree->currentItem() : nullptr;
    if (!parentItem) {
        parentItem = _rootItem;
    }
    const QString parentPath = parentItem ? parentItem->data(0, Qt::UserRole).toString() : QString();

    bool ok = false;
    const QString name = UDriveInputDialog::getText(
        this,
        tr("New folder"),
        tr("Folder name"),
        {},
        tr("Create"),
        &ok);
    if (!ok) return;

    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || !validPath(trimmed) || trimmed.contains(QLatin1Char('/')) || trimmed.contains(QLatin1Char('\\'))) {
        return;
    }

    const QString newPath = parentPath.isEmpty() ? trimmed : parentPath + QLatin1Char('/') + trimmed;

    auto addFolderItem = [this, parentItem, trimmed, newPath] {
        if (!parentItem || parentItem->treeWidget() != _tree) return;
        auto *newItem = new QTreeWidgetItem(parentItem);
        newItem->setText(0, trimmed);
        newItem->setIcon(0, UDriveIcons::fileIcon(QString(), true));
        newItem->setData(0, Qt::UserRole, newPath);
        newItem->setData(0, Qt::UserRole + 1, true);
        parentItem->setExpanded(true);
        _tree->setCurrentItem(newItem);
        updateMoveButtonState();
    };

    if (_accountState && _accountState->account() && !_rootUrl.isEmpty()) {
        auto *job = new FileActionJob(_accountState->account(), _rootUrl, newPath, "MKCOL", nullptr, {}, this);
        connect(job, &AbstractNetworkJob::finishedSignal, this, [job, addFolderItem] {
            const int status = job->httpStatusCode();
            if (status >= 200 && status < 300) {
                addFolderItem();
            }
        });
        job->start();
    } else {
        addFolderItem();
    }
}

void UDriveMoveDialog::updateMoveButtonState()
{
    const auto *item = _tree ? _tree->currentItem() : nullptr;
    if (!item) {
        _destinationLabel->setText(tr("Select a folder"));
        _moveButton->setEnabled(false);
        return;
    }

    const QString destPath = item->data(0, Qt::UserRole).toString();
    const bool isCurrentFolder = (destPath == _currentParentFolder);
    const bool isInvalidNesting = _isFolder && (destPath == _sourcePath || destPath.startsWith(_sourcePath + QLatin1Char('/')));

    if (isCurrentFolder) {
        const QString displayName = destPath.isEmpty() ? (_spaceName.isEmpty() ? tr("Personal") : _spaceName) : item->text(0);
        _destinationLabel->setText(tr("Current location: %1").arg(displayName));
        _moveButton->setEnabled(false);
    } else if (isInvalidNesting) {
        _destinationLabel->setText(tr("Cannot move a folder into itself"));
        _moveButton->setEnabled(false);
    } else {
        const QString displayName = destPath.isEmpty() ? (_spaceName.isEmpty() ? tr("Personal") : _spaceName) : item->text(0);
        _destinationLabel->setText(tr("Destination: %1").arg(displayName));
        _moveButton->setEnabled(true);
    }
}

QString UDriveMoveDialog::selectedDestination() const
{
    const auto *item = _tree ? _tree->currentItem() : nullptr;
    return item ? item->data(0, Qt::UserRole).toString() : QString();
}

QString initialsForName(const QString &name)
{
    const QStringList parts = name.trimmed().split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (parts.isEmpty()) return QStringLiteral("?");
    if (parts.size() == 1) return parts.first().left(2).toUpper();
    return (parts.first().left(1) + parts.last().left(1)).toUpper();
}

QColor avatarColorForName(const QString &name)
{
    static const QVector<QColor> colors{
        QColor(QStringLiteral("#475569")), // Slate (SP in mock)
        QColor(QStringLiteral("#B45309")), // Amber (TV in mock)
        QColor(QStringLiteral("#0D9488")), // Teal
        QColor(QStringLiteral("#1D4ED8")), // Blue
        QColor(QStringLiteral("#6D28D9")), // Purple
        QColor(QStringLiteral("#BE185D")), // Pink
        QColor(QStringLiteral("#047857")), // Emerald
        QColor(QStringLiteral("#C2410C")), // Orange
    };
    return colors.at(qHash(name) % colors.size());
}

QPixmap createAvatarPixmap(const QString &name, int size, qreal dpr)
{
    QPixmap pixmap(QSize(size, size) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(avatarColorForName(name));
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(QRectF(0, 0, size, size));
    painter.setPen(Qt::white);
    QFont font = painter.font();
    font.setPixelSize(qRound(size * 0.38));
    font.setBold(true);
    painter.setFont(font);
    painter.drawText(QRectF(0, 0, size, size), Qt::AlignCenter, initialsForName(name));
    return pixmap;
}

class ShareRoleItemDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        Q_UNUSED(option);
        Q_UNUSED(index);
        return QSize(236, 46);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);

        const bool selected = (option.state & QStyle::State_Selected) || (option.state & QStyle::State_HasFocus);
        const bool hovered = (option.state & QStyle::State_MouseOver);

        const QRect r = option.rect.adjusted(2, 2, -2, -2);
        if (selected) {
            painter->setBrush(QColor(QStringLiteral("#22C55E")));
            painter->setPen(Qt::NoPen);
            painter->drawRoundedRect(r, 8, 8);
        } else if (hovered) {
            const bool dark = option.palette.color(QPalette::Window).lightness() < 128;
            painter->setBrush(QColor(dark ? QStringLiteral("#32353A") : QStringLiteral("#F3F4F6")));
            painter->setPen(Qt::NoPen);
            painter->drawRoundedRect(r, 8, 8);
        }

        const bool isEdit = index.data(Qt::UserRole).toInt() == 15 || index.row() == 1;
        const UDriveIcons::Symbol iconSym = isEdit ? UDriveIcons::Symbol::Rename : UDriveIcons::Symbol::Eye;
        const bool dark = option.palette.color(QPalette::Window).lightness() < 128;
        const QColor defaultText = dark ? QColor(QStringLiteral("#EDEDED")) : QColor(QStringLiteral("#1F2328"));
        const QColor iconColor = selected ? Qt::white : defaultText;

        const QRect iconRect(r.left() + 10, r.top() + (r.height() - 20) / 2, 20, 20);
        painter->drawPixmap(iconRect, UDriveIcons::icon(iconSym, false, iconColor).pixmap(20, 20));

        const QString title = isEdit ? UDriveFileBrowser::tr("Can edit") : UDriveFileBrowser::tr("Can view");
        const QString subtitle = isEdit
            ? UDriveFileBrowser::tr("View, download, upload and edit.")
            : UDriveFileBrowser::tr("View and download.");

        const QColor titleColor = selected ? Qt::white : defaultText;
        const QColor subtitleColor = selected ? QColor(QStringLiteral("#E6F9EE")) : (dark ? QColor(QStringLiteral("#9BA3AF")) : QColor(QStringLiteral("#6B7280")));

        const int textLeft = iconRect.right() + 12;
        const int textWidth = r.right() - textLeft - (selected ? 28 : 8);

        QFont titleFont = option.font;
        titleFont.setBold(true);
        titleFont.setPixelSize(13);
        painter->setFont(titleFont);
        painter->setPen(titleColor);
        painter->drawText(QRect(textLeft, r.top() + 4, textWidth, 18), Qt::AlignLeft | Qt::AlignVCenter, title);

        QFont subFont = option.font;
        subFont.setBold(false);
        subFont.setPixelSize(11);
        painter->setFont(subFont);
        painter->setPen(subtitleColor);
        painter->drawText(QRect(textLeft, r.top() + 22, textWidth, 16), Qt::AlignLeft | Qt::AlignVCenter, subtitle);

        if (selected) {
            const QRect checkRect(r.right() - 24, r.top() + (r.height() - 16) / 2, 16, 16);
            painter->drawPixmap(checkRect, UDriveIcons::icon(UDriveIcons::Symbol::Check, false, Qt::white).pixmap(16, 16));
        }

        painter->restore();
    }
};

class SharePopupListView : public QListView
{
public:
    SharePopupListView(QWidget *parent, const QColor &background)
        : QListView(parent), _background(background)
    {
        setWindowFlag(Qt::FramelessWindowHint);
        setAttribute(Qt::WA_TranslucentBackground, false);
        auto colors = palette();
        colors.setColor(QPalette::Base, background);
        colors.setColor(QPalette::Window, background);
        setPalette(colors);
        viewport()->setPalette(colors);
        viewport()->setAutoFillBackground(true);
    }

    QSize sizeHint() const override
    {
        const int count = model() ? qMin(2, model()->rowCount()) : 2;
        return QSize(236, count * 46 + 10);
    }

    QSize minimumSizeHint() const override
    {
        return sizeHint();
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        // Embedded sharing forms use transparent styles. Popup surfaces must
        // still cover the form, including inside a scroll area's viewport.
        QPainter painter(viewport());
        painter.fillRect(viewport()->rect(), _background);
        painter.end();
        QListView::paintEvent(event);
    }

private:
    QColor _background;
};

void configureShareRolePopup(QComboBox *combo)
{
    const bool dark = QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
    const QString background = dark ? QStringLiteral("#1F2023") : QStringLiteral("#FFFFFF");
    auto *view = new SharePopupListView(combo, QColor(background));
    view->setObjectName(QStringLiteral("shareRolePopup"));
    view->setFrameShape(QFrame::NoFrame);
    view->setUniformItemSizes(true);
    view->setSpacing(0);
    view->setMouseTracking(true);
    view->setTextElideMode(Qt::ElideNone);
    view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    view->setMinimumWidth(236);
    view->setStyleSheet(QStringLiteral(
        "QListView#shareRolePopup { background: %1; border: 1px solid %2; border-radius: 10px; padding: 4px; outline: none; }")
        .arg(background, dark ? QStringLiteral("#3E4146") : QStringLiteral("#D0D7DE")));
    combo->setProperty("udriveOpaquePopup", true);
    combo->setCursor(Qt::PointingHandCursor);
    combo->setView(view);
    combo->setItemDelegate(new ShareRoleItemDelegate(combo));
    combo->setMaxVisibleItems(2);
}

QStringList publicLinkPasswordRequirements(const QString &message)
{
    // Summarize only requirements actually reported by the server. Policies
    // differ between installations; do not invent a local password policy.
    const QRegularExpression rule(QStringLiteral(
        "at least\\s+(\\d+)\\s+(uppercase letters?|lowercase letters?|numbers?|digits?|special characters?|characters?)\\s+(?:are|is)\\s+required"),
        QRegularExpression::CaseInsensitiveOption);
    QStringList requirements;
    auto matches = rule.globalMatch(message);
    while (matches.hasNext()) {
        const auto match = matches.next();
        const int count = match.captured(1).toInt();
        const QString kind = match.captured(2).toLower();
        QString text;
        if (kind.startsWith(QLatin1String("uppercase"))) text = (count == 1 ? UDriveShareDialog::tr("At least %1 uppercase letter") : UDriveShareDialog::tr("At least %1 uppercase letters")).arg(count);
        else if (kind.startsWith(QLatin1String("lowercase"))) text = (count == 1 ? UDriveShareDialog::tr("At least %1 lowercase letter") : UDriveShareDialog::tr("At least %1 lowercase letters")).arg(count);
        else if (kind.startsWith(QLatin1String("number")) || kind.startsWith(QLatin1String("digit"))) text = (count == 1 ? UDriveShareDialog::tr("At least %1 number") : UDriveShareDialog::tr("At least %1 numbers")).arg(count);
        else if (kind.startsWith(QLatin1String("special"))) text = (count == 1 ? UDriveShareDialog::tr("At least %1 special character") : UDriveShareDialog::tr("At least %1 special characters")).arg(count);
        else text = (count == 1 ? UDriveShareDialog::tr("At least %1 character") : UDriveShareDialog::tr("At least %1 characters")).arg(count);
        if (!requirements.contains(text)) requirements.append(text);
    }
    if (!requirements.isEmpty()) {
        const QRegularExpression words(QStringLiteral("[\\p{L}]{3,}"));
        for (const auto &line : message.split(QLatin1Char('\n'))) {
            if (!rule.match(line).hasMatch() && words.match(line).hasMatch())
                requirements.append(line.simplified().left(180));
        }
    }
    return requirements;
}

QString publicLinkUpdateError(JsonApiJob *job)
{
    if (!job->ocsMessage().isEmpty()) return job->ocsMessage();
    // JsonJob leaves HTTP error bodies unread. These can contain the same OCS
    // validation details as a successful HTTP response with a failed OCS status.
    if (job->reply()) {
        const auto data = QJsonDocument::fromJson(job->reply()->read(64 * 1024)).object();
        const auto message = data.value(QStringLiteral("ocs")).toObject().value(QStringLiteral("meta")).toObject().value(QStringLiteral("message")).toString();
        if (!message.isEmpty()) return message;
    }
    switch (job->httpStatusCode()) {
    case 401: return UDriveShareDialog::tr("Your session has expired. Sign in again and retry.");
    case 403: return UDriveShareDialog::tr("You don't have permission to change this link.");
    case 404: return UDriveShareDialog::tr("This link is no longer available. Close this dialog and refresh the shares.");
    case 429: return UDriveShareDialog::tr("Too many requests. Wait a moment and try again.");
    default: return job->httpStatusCode() >= 500
        ? UDriveShareDialog::tr("The server couldn't save this change. Try again in a moment.")
        : UDriveShareDialog::tr("Couldn't save this change. Check your connection and try again.");
    }
}

class ShareRecipientPopup : public SharePopupListView
{
public:
    ShareRecipientPopup(QLineEdit *input, const QColor &background)
        : SharePopupListView(nullptr, background), _input(input)
    {
    }

    QSize sizeHint() const override
    {
        const int count = model() ? qMin(6, model()->rowCount()) : 0;
        return QSize(_input ? _input->width() : 236, count > 0 ? (count * 52 + 10) : 0);
    }

    QSize minimumSizeHint() const override
    {
        return QSize(0, 0);
    }

protected:
    void showEvent(QShowEvent *event) override
    {
        QListView::showEvent(event);
        positionPopup();
        QTimer::singleShot(0, this, [this] { positionPopup(); });
    }
    void moveEvent(QMoveEvent *event) override
    {
        QListView::moveEvent(event);
        positionPopup();
    }
    void resizeEvent(QResizeEvent *event) override
    {
        QListView::resizeEvent(event);
        positionPopup();
    }

private:
    void positionPopup()
    {
        if (_positioning || !isVisible() || !_input) return;
        const QScopedValueRollback<bool> guard(_positioning, true);
        const QRect available = udrivePopupBounds(_input);
        resize(qMin(_input->width(), available.width()), qMin(height(), available.height()));
        const QPoint below = _input->mapToGlobal(QPoint(0, _input->height() + 4));
        const int left = layoutDirection() == Qt::RightToLeft
            ? _input->mapToGlobal(QPoint(_input->width(), 0)).x() - width() : below.x();
        const int top = below.y() + height() <= available.bottom() + 1
            ? below.y() : _input->mapToGlobal(QPoint(0, -height() - 4)).y();
        move(qBound(available.left(), left, available.right() - width() + 1),
            qBound(available.top(), top, available.bottom() - height() + 1));
    }
    QPointer<QLineEdit> _input;
    bool _positioning = false;
};

class ShareRecipientDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override
    {
        return QSize(0, 52);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const bool dark = option.palette.color(QPalette::Window).lightness() < 128;
        const bool selected = (option.state & QStyle::State_Selected);
        const bool hover = (option.state & QStyle::State_MouseOver);

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);

        const QRect itemRect = option.rect.adjusted(2, 2, -2, -2);

        // 1. Hover / Selected background
        if (selected) {
            const QColor bg = dark ? QColor(QStringLiteral("#23382B")) : QColor(QStringLiteral("#E7F7ED"));
            painter->setPen(Qt::NoPen);
            painter->setBrush(bg);
            painter->drawRoundedRect(itemRect, 8, 8);

            // Left emerald accent indicator
            painter->setBrush(QColor(QStringLiteral("#22C55E")));
            painter->drawRoundedRect(QRect(itemRect.left() + 3, itemRect.top() + 10, 3, itemRect.height() - 20), 1.5, 1.5);
        } else if (hover) {
            const QColor bg = dark ? QColor(QStringLiteral("#2A2D33")) : QColor(QStringLiteral("#F3F4F6"));
            painter->setPen(Qt::NoPen);
            painter->setBrush(bg);
            painter->drawRoundedRect(itemRect, 8, 8);
        }

        // 2. Avatar
        const int type = index.data(Qt::UserRole + 1).toInt();
        const QString label = index.data(Qt::UserRole + 2).toString();
        const QString detail = index.data(Qt::UserRole + 3).toString();
        QString initials = index.data(Qt::UserRole + 4).toString();
        if (initials.isEmpty())
            initials = personInitials(label.isEmpty() ? detail : label);

        const int avatarSize = 32;
        const int avatarX = itemRect.left() + 12;
        const int avatarY = itemRect.top() + (itemRect.height() - avatarSize) / 2;
        const QRect avatarRect(avatarX, avatarY, avatarSize, avatarSize);

        if (type == 1) {
            // Group Avatar: Indigo
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(QStringLiteral("#4F46E5")));
            painter->drawEllipse(avatarRect);
            UDriveIcons::icon(UDriveIcons::Symbol::Members, false, Qt::white).paint(painter, avatarRect.adjusted(7, 7, -7, -7));
        } else {
            // User Avatar: Signature Brand Teal
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(QStringLiteral("#007C58")));
            painter->drawEllipse(avatarRect);

            painter->setPen(Qt::white);
            QFont avFont = painter->font();
            avFont.setPixelSize(12);
            avFont.setBold(true);
            painter->setFont(avFont);
            painter->drawText(avatarRect, Qt::AlignCenter, initials);
        }

        // 3. Text content
        const int textX = avatarRect.right() + 10;
        const int textRight = itemRect.right() - 8;
        const int textWidth = qMax(0, textRight - textX);

        if (textWidth > 0) {
            // Line 1: Primary display name
            const QRect nameRect(textX, itemRect.top() + 7, textWidth, 18);
            QFont nameFont = painter->font();
            nameFont.setPixelSize(13);
            nameFont.setBold(true);
            painter->setFont(nameFont);
            painter->setPen(dark ? QColor(QStringLiteral("#EDEDED")) : QColor(QStringLiteral("#1F2328")));
            painter->drawText(nameRect, Qt::AlignVCenter | Qt::AlignLeft,
                painter->fontMetrics().elidedText(label, Qt::ElideRight, nameRect.width()));

            // Line 2: Badge + Secondary detail info
            const QString badgeText = (type == 1)
                ? QCoreApplication::translate("OCC::UDriveShareDialog", "Group · %1").section(QLatin1Char(' '), 0, 0)
                : QCoreApplication::translate("OCC::UDriveShareDialog", "User · %1").section(QLatin1Char(' '), 0, 0);
            const QString displayedBadgeText = badgeText.isEmpty()
                ? (type == 1 ? QStringLiteral("Group") : QStringLiteral("User"))
                : badgeText;
            QFont badgeFont = painter->font();
            badgeFont.setPixelSize(10);
            badgeFont.setBold(true);
            painter->setFont(badgeFont);

            const int badgeTextWidth = painter->fontMetrics().horizontalAdvance(displayedBadgeText);
            const int badgeHeight = 16;
            const int badgeWidth = badgeTextWidth + 12;
            const QRect badgeRect(textX, itemRect.top() + 27, badgeWidth, badgeHeight);

            const QColor badgeBg = (type == 1)
                ? (dark ? QColor(99, 102, 241, static_cast<int>(255 * 0.18)) : QColor(99, 102, 241, static_cast<int>(255 * 0.12)))
                : (dark ? QColor(34, 197, 94, static_cast<int>(255 * 0.15)) : QColor(34, 197, 94, static_cast<int>(255 * 0.12)));
            const QColor badgeColor = (type == 1)
                ? (dark ? QColor(QStringLiteral("#A5B4FC")) : QColor(QStringLiteral("#4F46E5")))
                : (dark ? QColor(QStringLiteral("#4ADE80")) : QColor(QStringLiteral("#16A34A")));

            painter->setPen(Qt::NoPen);
            painter->setBrush(badgeBg);
            painter->drawRoundedRect(badgeRect, badgeHeight / 2.0, badgeHeight / 2.0);
            painter->setPen(badgeColor);
            painter->drawText(badgeRect, Qt::AlignCenter, displayedBadgeText);

            // Detail (e.g. username, email)
            const int detailX = badgeRect.right() + 6;
            const int detailWidth = qMax(0, textRight - detailX);
            if (detailWidth > 0 && !detail.isEmpty()) {
                const QRect detailRect(detailX, itemRect.top() + 26, detailWidth, 18);
                QFont detFont = painter->font();
                detFont.setPixelSize(11);
                detFont.setBold(false);
                painter->setFont(detFont);
                painter->setPen(dark ? QColor(QStringLiteral("#9BA3AF")) : QColor(QStringLiteral("#59636E")));
                painter->drawText(detailRect, Qt::AlignVCenter | Qt::AlignLeft,
                    painter->fontMetrics().elidedText(detail, Qt::ElideRight, detailWidth));
            }
        }

        painter->restore();
    }
};

UDriveShareDialog::UDriveShareDialog(AccountState *accountState, const QString &fileName, bool isFolder,
                                     const QString &remotePath, const QUrl &linkUrl, const QString &spaceId,
                                     QWidget *parent, Target target, Presentation presentation)
    : QDialog(parent)
    , _accountState(accountState)
    , _target(target)
    , _sidebar(presentation == Presentation::Sidebar)
    , _fileName(fileName)
    , _isFolder(isFolder)
    , _remotePath(remotePath.startsWith(QLatin1Char('/')) ? remotePath.mid(1) : remotePath)
    , _linkUrl(linkUrl)
    , _spaceId(spaceId)
{
    setObjectName(QStringLiteral("udriveShareDialog"));
    setWindowTitle(_target == Target::Workspace ? tr("Workspace members — %1").arg(fileName) : tr("Share “%1”").arg(fileName));
    setModal(!_sidebar);
    setWindowFlags(_sidebar ? Qt::Widget : (windowFlags() | Qt::Dialog) & ~Qt::WindowContextHelpButtonHint);
    setMinimumWidth(_sidebar ? 0 : 480);

    const bool dark = QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
    const QString bg = dark ? QStringLiteral("#242528") : QStringLiteral("#FFFFFF");
    const QString text = dark ? QStringLiteral("#EDEDED") : QStringLiteral("#1F2328");
    const QString subtext = dark ? QStringLiteral("#9BA3AF") : QStringLiteral("#59636E");
    const QString border = dark ? QStringLiteral("#3E4146") : QStringLiteral("#D0D7DE");
    const QString inputBg = dark ? QStringLiteral("#1B1C1E") : QStringLiteral("#FAFCFB");
    const QString cancelBg = dark ? QStringLiteral("#2E3135") : QStringLiteral("#F3F4F6");
    const QString cancelHover = dark ? QStringLiteral("#383B40") : QStringLiteral("#E5E7EB");
    const QString primaryDisabled = dark ? QStringLiteral("#2A4733") : QStringLiteral("#A3E4B8");
    const QString primaryDisabledText = dark ? QStringLiteral("#587864") : QStringLiteral("#FFFFFF");

    const QString arrow = dark ? QStringLiteral("chevron-dark") : QStringLiteral("chevron-light");
    const QString selectedBg = dark ? QStringLiteral("#2E4436") : QStringLiteral("#E7F7ED");
    const QString accent = dark ? QStringLiteral("#63E493") : QStringLiteral("#007C58");

    setStyleSheet(QStringLiteral(
        "QDialog#udriveShareDialog { background-color: %1; }"
        "QLabel#shareDialogTitle { color: %3; font-size: 15px; font-weight: 600; }"
        "QLabel#shareDialogSubtext { color: %4; font-size: 12px; }"
        "QLabel#shareSectionTitle { color: %3; font-size: 13px; font-weight: 600; margin-top: 6px; }"
        "QLineEdit#shareUserInput { min-height: 22px; padding: 7px 12px; border: 1px solid %2; border-radius: 8px; background: %5; color: %3; font-size: 13px; selection-background-color: #2E4436; }"
        "QLineEdit#shareUserInput:focus { border: 1px solid #22C55E; }"
        "QComboBox#shareRoleCombo { min-height: 22px; padding: 7px 22px 7px 10px; border: 1px solid %2; border-radius: 8px; background: %5; color: %3; font-size: 13px; combobox-popup: 0; }"
        "QComboBox#shareRoleCombo:hover, QComboBox#shareRoleCombo:focus { border-color: %12; }"
        "QComboBox#shareRoleCombo::drop-down { subcontrol-origin: border; subcontrol-position: center right; width: 22px; border: none; }"
        "QComboBox#shareRoleCombo::down-arrow { image: url(:/udrive/icons/%10.svg); width: 10px; height: 10px; }"
        "QComboBox#shareRoleCombo QListView#shareRolePopup { background: %5; color: %3; border: 1px solid %2; border-radius: 8px; padding: 4px; outline: none; }"
        "QComboBox#shareRoleCombo QListView#shareRolePopup::item { min-height: 24px; padding: 4px 8px; border: none; border-radius: 5px; }"
        "QComboBox#shareRoleCombo QListView#shareRolePopup::item:hover { background: %7; }"
        "QComboBox#shareRoleCombo QListView#shareRolePopup::item:selected { background: %11; color: %12; }"
        "QPushButton#shareConfirmButton { background-color: #22C55E; color: #FFFFFF; border: none; border-radius: 8px; padding: 7px 18px; font-size: 13px; font-weight: 600; min-height: 22px; }"
        "QPushButton#shareConfirmButton:hover { background-color: #2AD564; }"
        "QPushButton#shareConfirmButton:pressed { background-color: #16A34A; }"
        "QPushButton#shareConfirmButton:disabled { background-color: %8; color: %9; }"
        "QPushButton#shareCopyLinkButton { background-color: %6; color: %3; border: 1px solid %2; border-radius: 8px; padding: 7px 14px; font-size: 12px; font-weight: 500; min-height: 20px; }"
        "QPushButton#shareCopyLinkButton:hover { background-color: %7; }"
        "QPushButton#shareDoneButton { background-color: %6; color: %3; border: 1px solid %2; border-radius: 8px; padding: 7px 20px; font-size: 13px; font-weight: 500; min-width: 70px; min-height: 20px; }"
        "QPushButton#shareDoneButton:hover { background-color: %7; }"
        "QPushButton#shareAddLinkButton { background-color: transparent; color: #22C55E; border: 1px solid #22C55E; border-radius: 6px; padding: 4px 10px; font-size: 12px; font-weight: 500; min-height: 20px; }"
        "QPushButton#shareAddLinkButton:hover { background-color: rgba(34, 197, 94, 0.1); }"
        "QPushButton#shareAddLinkButton:pressed { background-color: rgba(34, 197, 94, 0.2); }"
        "QPushButton#shareAddLinkButton:disabled { border-color: %8; color: %9; }"
        "QScrollArea#sharesScrollArea, QScrollArea#publicLinksScrollArea { border: 1px solid %2; border-radius: 8px; background: %5; }"
        "QComboBox#publicLinkRoleCombo { min-height: 20px; padding: 3px 24px 3px 8px; border: 1px solid %2; border-radius: 6px; background: %5; color: %3; font-size: 12px; combobox-popup: 0; }"
        "QComboBox#publicLinkRoleCombo:hover, QComboBox#publicLinkRoleCombo:focus { border-color: %12; }"
        "QComboBox#publicLinkRoleCombo::drop-down { subcontrol-origin: border; subcontrol-position: center right; width: 20px; border: none; }"
        "QComboBox#publicLinkRoleCombo::down-arrow { image: url(:/udrive/icons/%10.svg); width: 10px; height: 10px; }"
        "QToolButton#publicLinkActionButton { border: none; background: transparent; border-radius: 4px; padding: 4px; }"
        "QToolButton#publicLinkActionButton:hover, QToolButton#publicLinkActionButton:pressed { background: transparent; }"
        "QComboBox#sharePersonRoleCombo { min-width: 0; min-height: 20px; padding: 4px 26px 4px 8px; border: 1px solid %2; border-radius: 6px; background: %5; color: %3; font-size: 12px; combobox-popup: 0; }"
        "QComboBox#sharePersonRoleCombo::drop-down { subcontrol-origin: border; subcontrol-position: center right; width: 22px; border: none; }"
        "QComboBox#sharePersonRoleCombo::down-arrow { image: url(:/udrive/icons/%10.svg); width: 10px; height: 10px; }"
    ).arg(bg, border, text, subtext, inputBg, cancelBg, cancelHover, primaryDisabled, primaryDisabledText).arg(arrow, selectedBg, accent));

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(_sidebar ? 0 : 20, _sidebar ? 0 : 20, _sidebar ? 0 : 20, _sidebar ? 0 : 16);
    mainLayout->setSpacing(_sidebar ? 20 : 14);

    if (!_sidebar) {
        auto *headerLayout = new QHBoxLayout;
        headerLayout->setSpacing(12);

        auto *iconLabel = new QLabel(this);
        iconLabel->setPixmap(UDriveIcons::icon(isFolder ? UDriveIcons::Symbol::Folder : UDriveIcons::Symbol::File).pixmap(32, 32));
        headerLayout->addWidget(iconLabel);

        auto *titleLayout = new QVBoxLayout;
        titleLayout->setSpacing(2);
        auto *titleLabel = new QLabel(fileName, this);
        titleLabel->setObjectName(QStringLiteral("shareDialogTitle"));
        titleLabel->setTextFormat(Qt::PlainText);
        titleLayout->addWidget(titleLabel);

        auto *subtextLabel = new QLabel(_target == Target::Workspace ? tr("Manage workspace membership")
            : isFolder ? tr("Share folder with other users") : tr("Share file with other users"), this);
        subtextLabel->setObjectName(QStringLiteral("shareDialogSubtext"));
        subtextLabel->setTextFormat(Qt::PlainText);
        titleLayout->addWidget(subtextLabel);

        headerLayout->addLayout(titleLayout, 1);
        mainLayout->addLayout(headerLayout);

    }

    QVBoxLayout *peopleLayout = mainLayout;
    if (_sidebar) {
        auto *card = new QWidget(this);
        card->setObjectName(QStringLiteral("sharePeopleCard"));
        peopleLayout = new QVBoxLayout(card);
        peopleLayout->setContentsMargins(0, 8, 0, 0);
        peopleLayout->setSpacing(12);
        mainLayout->addWidget(card);
    }
    auto *inviteSectionLabel = new QLabel(_sidebar ? tr("Share with people") : tr("Invite user or group"), this);
    inviteSectionLabel->setObjectName(QStringLiteral("shareSectionTitle"));
    auto *inviteHeader = new QHBoxLayout;
    inviteHeader->setSpacing(6);
    inviteHeader->addWidget(inviteSectionLabel);
    if (_sidebar) {
        auto *helpLabel = new QLabel(this);
        helpLabel->setObjectName(QStringLiteral("shareHelpIcon"));
        helpLabel->setPixmap(UDriveIcons::icon(UDriveIcons::Symbol::Help, false, QColor(dark ? "#9BA3AF" : "#59636E")).pixmap(14, 14));
        helpLabel->setToolTip(tr("Share directly with people or groups in your organization."));
        inviteHeader->addWidget(helpLabel);
    }
    inviteHeader->addStretch(1);
    peopleLayout->addLayout(inviteHeader);
    if (_sidebar && _linkUrl.isValid() && !_linkUrl.isEmpty()) {
        _copyLinkButton = new QPushButton(UDriveIcons::icon(UDriveIcons::Symbol::Link), tr("Permanent link"), this);
        _copyLinkButton->setObjectName(QStringLiteral("shareCopyLinkButton"));
        _copyLinkButton->setToolTip(tr("Requires sign-in and existing access to this file. This does not grant access."));
        _copyLinkButton->setCursor(Qt::PointingHandCursor);
        connect(_copyLinkButton, &QPushButton::clicked, this, &UDriveShareDialog::copyLink);
        inviteHeader->addWidget(_copyLinkButton);
    }

    if (_sidebar) {
        auto *searchLabel = new QLabel(tr("Search"), this);
        searchLabel->setObjectName(QStringLiteral("shareSearchLabel"));
        searchLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; font-weight: 500;").arg(subtext));
        peopleLayout->addWidget(searchLabel);
    }

    auto *inputRow = new QHBoxLayout;
    inputRow->setSpacing(8);

    _userInput = new QLineEdit(this);
    _userInput->setObjectName(QStringLiteral("shareUserInput"));
    _userInput->setPlaceholderText(_sidebar ? QString() : tr("Search users or groups…"));
    _userInput->setMaxLength(256);
    _userInput->setClearButtonEnabled(true);
    _userInput->setMinimumWidth(0);
    if (_sidebar) peopleLayout->addWidget(_userInput);
    else inputRow->addWidget(_userInput, 1);

    _roleCombo = new UDriveComboBox(this);
    _roleCombo->setObjectName(QStringLiteral("shareRoleCombo"));
    _roleCombo->setProperty("udriveCombo", true);
    _roleCombo->setCursor(Qt::PointingHandCursor);
    configureShareRolePopup(_roleCombo);
    _roleCombo->addItem(_sidebar ? tr("Can view") : tr("Viewer"), 1);
    _roleCombo->addItem(_sidebar ? tr("Can edit") : tr("Editor"), 15);
    QFont roleFont = _roleCombo->font();
    roleFont.setPixelSize(13);
    _roleCombo->setFont(roleFont);
    _roleCombo->ensurePolished();
    auto updateSearchRoleWidth = [this] {
        const int roleWidth = qMax(_roleCombo->fontMetrics().horizontalAdvance(_roleCombo->itemText(0)),
            _roleCombo->fontMetrics().horizontalAdvance(_roleCombo->itemText(1)));
        _roleCombo->setFixedWidth(_sidebar ? qMax(124, roleWidth + 54) : qMax(120, roleWidth + 48));
    };
    updateSearchRoleWidth();
    connect(_roleCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [updateSearchRoleWidth](int) {
        updateSearchRoleWidth();
    });
    _roleCombo->setFixedHeight(38);
    inputRow->addWidget(_roleCombo);
    if (_sidebar) {
        inputRow->addStretch(1);
    }

    _shareButton = new QPushButton(_target == Target::Workspace ? tr("Add member") : tr("Share"), this);
    _shareButton->setObjectName(QStringLiteral("shareConfirmButton"));
    _shareButton->setFixedHeight(38);
    _shareButton->setCursor(Qt::PointingHandCursor);
    _shareButton->setEnabled(false);
    inputRow->addWidget(_shareButton);

    peopleLayout->addLayout(inputRow);

    _statusLabel = new QLabel(this);
    _statusLabel->setObjectName(QStringLiteral("shareStatusLabel"));
    _statusLabel->setTextFormat(Qt::PlainText);
    _statusLabel->setWordWrap(true);
    _statusLabel->setStyleSheet(_sidebar ? QStringLiteral("font-size: 12px;") : QStringLiteral("font-size: 12px; min-height: 16px;"));
    peopleLayout->addWidget(_statusLabel);

    auto *accessSectionLabel = new QLabel(_target == Target::Workspace ? tr("Workspace members") : _sidebar ? tr("Shared with") : tr("People with access"), this);
    accessSectionLabel->setObjectName(QStringLiteral("shareSectionTitle"));
    peopleLayout->addWidget(accessSectionLabel);

    auto *scrollArea = new QScrollArea(this);
    scrollArea->setObjectName(QStringLiteral("sharesScrollArea"));
    scrollArea->setWidgetResizable(true);
    scrollArea->setMinimumHeight(110);
    scrollArea->setMaximumHeight(160);
    if (_sidebar) {
        scrollArea->setFrameShape(QFrame::NoFrame);
        scrollArea->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
        scrollArea->viewport()->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
    }

    _sharesContainer = new QWidget(scrollArea);
    _sharesLayout = new QVBoxLayout(_sharesContainer);
    _sharesLayout->setContentsMargins(_sidebar ? 0 : 10, 8, _sidebar ? 0 : 10, 8);
    _sharesLayout->setSpacing(6);
    _sharesLayout->addStretch(1);
    scrollArea->setWidget(_sharesContainer);
    peopleLayout->addWidget(scrollArea);

    _publicLinksSection = new QWidget(this);
    _publicLinksSection->setObjectName(QStringLiteral("sharePublicLinksCard"));
    auto *publicLinksSectionLayout = new QVBoxLayout(_publicLinksSection);
    publicLinksSectionLayout->setContentsMargins(0, _sidebar ? 18 : 0, 0, 0);
    publicLinksSectionLayout->setSpacing(_sidebar ? 12 : 6);

    auto *publicLinksHeaderLayout = new QHBoxLayout;
    auto *publicLinksTitle = new QLabel(tr("Public links"), _publicLinksSection);
    publicLinksTitle->setObjectName(QStringLiteral("shareSectionTitle"));
    publicLinksHeaderLayout->addWidget(publicLinksTitle);
    publicLinksHeaderLayout->addStretch(1);

    _addLinkButton = new QPushButton(UDriveIcons::icon(UDriveIcons::Symbol::Add), tr("Add link"), _publicLinksSection);
    _addLinkButton->setObjectName(QStringLiteral("shareAddLinkButton"));
    _addLinkButton->setCursor(Qt::PointingHandCursor);
    _addLinkButton->setAutoDefault(false);
    connect(_addLinkButton, &QPushButton::clicked, this, &UDriveShareDialog::createPublicLink);
    publicLinksHeaderLayout->addWidget(_addLinkButton);
    publicLinksSectionLayout->addLayout(publicLinksHeaderLayout);

    _publicLinksScrollArea = new QScrollArea(_publicLinksSection);
    _publicLinksScrollArea->setObjectName(QStringLiteral("publicLinksScrollArea"));
    _publicLinksScrollArea->setWidgetResizable(true);
    _publicLinksScrollArea->setMinimumHeight(60);
    _publicLinksScrollArea->setMaximumHeight(120);
    if (_sidebar) {
        _publicLinksScrollArea->setFrameShape(QFrame::NoFrame);
        _publicLinksScrollArea->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
        _publicLinksScrollArea->viewport()->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
    }

    _publicLinksContainer = new QWidget(_publicLinksScrollArea);
    _publicLinksLayout = new QVBoxLayout(_publicLinksContainer);
    _publicLinksLayout->setContentsMargins(_sidebar ? 0 : 10, 8, _sidebar ? 0 : 10, 8);
    _publicLinksLayout->setSpacing(6);
    _publicLinksLayout->addStretch(1);
    _publicLinksScrollArea->setWidget(_publicLinksContainer);
    publicLinksSectionLayout->addWidget(_publicLinksScrollArea);

    mainLayout->addWidget(_publicLinksSection);

    if (_target == Target::Workspace) {
        _publicLinksSection->hide();
    }

    if (!_sidebar) {
        auto *bottomRow = new QHBoxLayout;
        bottomRow->setSpacing(10);

        if (_linkUrl.isValid() && !_linkUrl.isEmpty()) {
            _copyLinkButton = new QPushButton(UDriveIcons::icon(UDriveIcons::Symbol::Link), tr("Copy sign-in link"), this);
            _copyLinkButton->setObjectName(QStringLiteral("shareCopyLinkButton"));
            _copyLinkButton->setCursor(Qt::PointingHandCursor);
            connect(_copyLinkButton, &QPushButton::clicked, this, &UDriveShareDialog::copyLink);
            bottomRow->addWidget(_copyLinkButton);
        }

        bottomRow->addStretch(1);

        auto *doneButton = new QPushButton(tr("Done"), this);
        doneButton->setObjectName(QStringLiteral("shareDoneButton"));
        doneButton->setCursor(Qt::PointingHandCursor);
        doneButton->setAutoDefault(false);
        connect(doneButton, &QPushButton::clicked, this, &QDialog::accept);
        bottomRow->addWidget(doneButton);

        mainLayout->addLayout(bottomRow);

    } else {
        mainLayout->addStretch(1);
    }

    if (_sidebar) {
        const QString sidebarBorder = dark ? QStringLiteral("rgba(255, 255, 255, 0.12)") : QStringLiteral("rgba(0, 0, 0, 0.14)");
        const QString sidebarHover = dark ? QStringLiteral("rgba(255, 255, 255, 0.05)") : QStringLiteral("rgba(0, 0, 0, 0.04)");
        setStyleSheet(styleSheet() + QStringLiteral(
            "QDialog#udriveShareDialog { background: transparent; }"
            "QWidget#sharePeopleCard, QWidget#sharePublicLinksCard { background: transparent; border: none; border-radius: 0; }"
            "QWidget#sharePublicLinksCard { border-top: 1px solid %2; }"
            "QLabel { color: %3; background: transparent; }"
            "QLabel#shareSectionTitle { margin: 0; }"
            "QLabel#shareStatusLabel { color: %5; font-weight: 400; }"
            "QLineEdit#shareUserInput { min-width: 0; background: transparent; border-color: %2; }"
            "QLineEdit#shareUserInput:focus { border-color: %4; }"
            "QComboBox#shareRoleCombo, QComboBox#sharePersonRoleCombo, QComboBox#publicLinkRoleCombo { background: transparent; border-color: %2; }"
            "QScrollArea#sharesScrollArea, QScrollArea#publicLinksScrollArea { border: none; background: transparent; }"
            "QScrollArea#sharesScrollArea > QWidget > QWidget, QScrollArea#publicLinksScrollArea > QWidget > QWidget { background: transparent; }"
            "QScrollArea QScrollBar:vertical { width: 6px; background: transparent; margin: 0; border: none; }"
            "QScrollArea QScrollBar::handle:vertical { background: %2; border-radius: 3px; min-height: 20px; }"
            "QScrollArea QScrollBar::add-line:vertical, QScrollArea QScrollBar::sub-line:vertical { height: 0; background: transparent; border: none; }"
            "QPushButton#shareCopyLinkButton { min-width: 0; min-height: 20px; padding: 4px 6px; background: transparent; border: none; color: %5; font-size: 12px; font-weight: 400; }"
            "QPushButton#shareCopyLinkButton:hover { background: %1; color: %3; }"
            "QPushButton#shareAddLinkButton { min-height: 24px; padding: 2px 6px; background: transparent; border: 1px solid transparent; color: %4; }"
            "QPushButton#shareAddLinkButton:hover { background: %1; }"
            "QPushButton#shareAddLinkButton:disabled { color: %5; background: transparent; }"
            "QPushButton#shareConfirmButton { min-width: 0; padding: 6px 14px; background: #40D979; color: #12351D; }"
            "QPushButton#shareConfirmButton:hover { background: #63E493; }"
            "QPushButton#shareConfirmButton:disabled { background: %1; color: %5; }").arg(sidebarHover, sidebarBorder, text, accent, subtext));
    }

    // Search only this account's directory. No typed text is used as a recipient ID.
    _recipientTimer = new QTimer(this);
    _recipientTimer->setSingleShot(true);
    _recipientTimer->setInterval(300);
    connect(_recipientTimer, &QTimer::timeout, this, &UDriveShareDialog::searchRecipients);
    _recipientModel = new QStandardItemModel(this);
    _recipientCompleter = new QCompleter(_recipientModel, this);
    _recipientCompleter->setCompletionMode(QCompleter::UnfilteredPopupCompletion);
    _recipientCompleter->setCompletionRole(Qt::UserRole + 2);
    _recipientCompleter->setMaxVisibleItems(6);
    auto *recipients = new ShareRecipientPopup(_userInput, QColor(dark ? QStringLiteral("#1F2023") : QStringLiteral("#FFFFFF")));
    recipients->setObjectName(QStringLiteral("shareRecipientPopup"));
    recipients->setAccessibleName(tr("Matching users and groups"));
    recipients->setFrameShape(QFrame::NoFrame);
    recipients->setFocusPolicy(Qt::NoFocus);
    recipients->setUniformItemSizes(true);
    recipients->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    recipients->setTextElideMode(Qt::ElideRight);
    recipients->setSpacing(2);
    recipients->setMouseTracking(true);
    recipients->setStyleSheet(QStringLiteral(
        "QListView#shareRecipientPopup {"
        "  background: %1;"
        "  border: 1px solid %2;"
        "  border-radius: 10px;"
        "  padding: 4px;"
        "  outline: none;"
        "}"
        "QListView#shareRecipientPopup QScrollBar:vertical {"
        "  border: none;"
        "  background: transparent;"
        "  width: 6px;"
        "  margin: 4px 2px 4px 0;"
        "}"
        "QListView#shareRecipientPopup QScrollBar::handle:vertical {"
        "  background: %3;"
        "  border-radius: 3px;"
        "  min-height: 20px;"
        "}"
        "QListView#shareRecipientPopup QScrollBar::add-line:vertical,"
        "QListView#shareRecipientPopup QScrollBar::sub-line:vertical {"
        "  height: 0px;"
        "}")
        .arg(dark ? QStringLiteral("#1F2023") : QStringLiteral("#FFFFFF"),
             border,
             dark ? QStringLiteral("#4A4D54") : QStringLiteral("#C4C9D0")));
    _recipientCompleter->setPopup(recipients);
    _recipientCompleter->setWidget(_userInput);
    recipients->setItemDelegate(new ShareRecipientDelegate(recipients));
    connect(_recipientCompleter, qOverload<const QModelIndex &>(&QCompleter::activated),
        this, &UDriveShareDialog::selectRecipient);
    connect(_userInput, &QLineEdit::textChanged, this, [this] {
        cancelRecipientSearch();
        _recipientId.clear();
        _recipientLabel.clear();
        _recipientType = -1;
        _shareButton->setEnabled(false);
        _statusLabel->setStyleSheet(_sidebar ? QStringLiteral("font-size: 12px;") : QStringLiteral("font-size: 12px; min-height: 16px;"));
        if (!_sidebar) {
            setStatus(tr("Type at least 2 characters to find a user or group."));
        } else {
            setStatus({});
        }
        if (canShare() && _userInput->text().trimmed().size() >= 2)
            _recipientTimer->start();
    });
    // Return accepts a suggestion; only the Share button submits an invitation.
    for (auto *button : findChildren<QPushButton *>())
        button->setAutoDefault(false);
    connect(_shareButton, &QPushButton::clicked, this, &UDriveShareDialog::sendShare);
    connect(this, &QDialog::finished, this, [this] {
        _closed = true;
        cancelRecipientSearch();
        cancelWorkspaceRequest();
        cancelPublicLinkRequest();
        for (auto *job : findChildren<JsonApiJob *>()) job->abort();
        _shareButton->setEnabled(false);
    });
    if (_accountState) {
        connect(_accountState, &AccountState::stateChanged, this, [this] {
            if (!canShare()) reject();
        });
        connect(_accountState, &QObject::destroyed, this, [this] { reject(); });
    }
    if (!_sidebar) {
        setStatus(tr("Type at least 2 characters to find a user or group."));
    }

    if (_target == Target::Workspace) {
        _roleCombo->clear();
        _userInput->setEnabled(false);
        _roleCombo->setEnabled(false);
    }
    fetchShares();
    _userInput->setFocus();
}

UDriveShareDialog::~UDriveShareDialog()
{
    _closed = true;
    cancelRecipientSearch();
    cancelWorkspaceRequest();
    cancelPublicLinkRequest();
    for (auto *job : findChildren<JsonApiJob *>()) job->abort();
}

bool UDriveShareDialog::canShare() const
{
    const bool validTarget = _target == Target::Workspace ? validName(_spaceId) && _remotePath.isEmpty()
        : validPath(_remotePath) && (_spaceId.isEmpty() || validName(_spaceId));
    return !_closed && validTarget
        && _accountState && !_accountState->isSignedOut() && _accountState->account()
        && _accountState->account()->credentials() && _accountState->account()->credentials()->ready();
}

void UDriveShareDialog::cancelRecipientSearch()
{
    _recipientTimer->stop();
    const auto job = _recipientJob;
    _recipientJob.clear(); // Invalidate before abort(), which can finish synchronously.
    if (job) job->abort();
    _recipientCompleter->popup()->hide();
    _recipientModel->clear();
}

void UDriveShareDialog::searchRecipients()
{
    const QString query = _userInput->text().trimmed();
    if (!canShare() || _sharing || query.size() < 2 || (_target == Target::Workspace && !_canInviteMembers)) return;
    setStatus(tr("Searching…"));
    const JsonApiJob::UrlQuery args {
        { QStringLiteral("search"), query },
        { QStringLiteral("itemType"), _isFolder ? QStringLiteral("folder") : QStringLiteral("file") },
        { QStringLiteral("shareType[]"), QStringLiteral("0") },
        { QStringLiteral("shareType[]"), QStringLiteral("1") },
        { QStringLiteral("page"), QStringLiteral("1") },
        { QStringLiteral("perPage"), QStringLiteral("20") }
    };
    auto *job = new JsonApiJob(_accountState->account(),
        QStringLiteral("ocs/v1.php/apps/files_sharing/api/v1/sharees"), args, {}, this);
    _recipientJob = job;
    job->setTimeout(std::chrono::seconds(15));
    // Bound the directory response even if a server ignores perPage.
    job->addNewReplyHook([this, job](QNetworkReply *reply) {
        connect(reply, &QIODevice::readyRead, job, [this, job, reply] {
            if (reply->bytesAvailable() > 512 * 1024 && _recipientJob == job) {
                cancelRecipientSearch();
                setStatus(tr("Could not search users and groups. Try again."));
            }
        });
    });
    connect(job, &JsonApiJob::finishedSignal, this, [this, job, query] {
        if (_recipientJob != job || !canShare() || query != _userInput->text().trimmed()) return;
        _recipientJob.clear();
        const auto data = job->data().value(QStringLiteral("ocs")).toObject().value(QStringLiteral("data"));
        if (!job->ocsSuccess() || !data.isObject()) {
            setStatus(tr("Could not search users and groups. Try again."));
            return;
        }
        QSet<QString> seen;
        const auto addRecipients = [this, &seen](const QJsonObject &source, const QString &key, int type) {
            const auto values = source.value(key).toArray();
            for (const auto &element : values) {
                if (_recipientModel->rowCount() >= 20) break;
                const auto entry = element.toObject();
                const auto value = entry.value(QStringLiteral("value")).toObject();
                const QString id = value.value(QStringLiteral("shareWith")).toString();
                const QString identity = QString::number(type) + QLatin1Char(':') + id;
                if (value.value(QStringLiteral("shareType")).toInt(-1) != type
                    || id.trimmed().isEmpty() || id.size() > 1024 || seen.contains(identity)) continue;
                seen.insert(identity);
                QString label = entry.value(QStringLiteral("label")).toString().left(256).simplified();
                if (label.isEmpty()) label = id.left(256).simplified();
                QString detail = value.value(QStringLiteral("shareWithAdditionalInfo")).toString().left(256).simplified();
                if (detail.isEmpty()) detail = id.left(256).simplified();
                auto *item = new QStandardItem(label + QLatin1Char('\n')
                    + (type == 0 ? tr("User · %1") : tr("Group · %1")).arg(detail));
                item->setData(id, Qt::UserRole);
                item->setData(type, Qt::UserRole + 1);
                item->setData(label, Qt::UserRole + 2);
                item->setData(detail, Qt::UserRole + 3);
                item->setData(personInitials(label.isEmpty() ? detail : label), Qt::UserRole + 4);
                item->setSizeHint(QSize(0, 52));
                item->setEditable(false);
                _recipientModel->appendRow(item);
            }
        };
        const auto object = data.toObject();
        for (const auto &source : {object.value(QStringLiteral("exact")).toObject(), object}) {
            addRecipients(source, QStringLiteral("users"), 0);
            addRecipients(source, QStringLiteral("groups"), 1);
        }
        if (_recipientModel->rowCount() == 0) {
            setStatus(tr("No matching users or groups. Try another name or email."));
        } else {
            setStatus(tr("Select a user or group, then click Share."));
            _recipientCompleter->setCompletionPrefix(QString());
            if (isVisible() && _userInput->hasFocus())
                _recipientCompleter->complete(_userInput->rect().translated(0, 4));
        }
    });
    job->start();
}

void UDriveShareDialog::selectRecipient(const QModelIndex &index)
{
    if (!canShare() || _sharing || !index.isValid() || (_target == Target::Workspace && !_canInviteMembers)) return;
    // Copy values before hiding/clearing the completion model.
    const QString id = index.data(Qt::UserRole).toString();
    const int type = index.data(Qt::UserRole + 1).toInt();
    const QString label = index.data(Qt::UserRole + 2).toString();
    if (id.isEmpty() || (type != 0 && type != 1)) return;
    cancelRecipientSearch();
    _recipientId = id;
    _recipientType = type;
    _recipientLabel = label;
    const QSignalBlocker blocker(_userInput);
    _userInput->setText(label);
    _userInput->setFocus(Qt::PopupFocusReason);
    _shareButton->setEnabled(true);
    setStatus(tr("Selected: %1").arg(label));
}

void UDriveShareDialog::sendShare()
{
    if (_target == Target::Workspace) { inviteWorkspaceMember(); return; }
    if (!canShare() || _sharing || _recipientId.isEmpty() || (_recipientType != 0 && _recipientType != 1))
        return;
    const QString user = _recipientLabel;
    _sharing = true;
    _userInput->setEnabled(false);
    _roleCombo->setEnabled(false);
    _shareButton->setEnabled(false);
    setStatus(tr("Sharing with %1…").arg(user));
    _statusLabel->setStyleSheet(QStringLiteral("color: #3B82F6; font-size: 12px;"));

    const int roleIndex = _roleCombo->currentIndex();
    const int permissions = (roleIndex == 1) ? 15 : 1;
    const QString roleStr = (roleIndex == 1) ? QStringLiteral("editor") : QStringLiteral("viewer");

    QString path = _remotePath;
    if (!path.startsWith(QLatin1Char('/')))
        path.prepend(QLatin1Char('/'));

    JsonApiJob::UrlQuery args {
        { QStringLiteral("path"), path },
        { QStringLiteral("shareType"), QString::number(_recipientType) },
        { QStringLiteral("shareWith"), _recipientId },
        { QStringLiteral("permissions"), QString::number(permissions) },
        { QStringLiteral("role"), roleStr }
    };
    if (!_spaceId.isEmpty()) {
        // The server prioritizes space_ref over path. A bare space ID targets
        // the space root; append the selected item's decoded, relative path.
        args.append({ QStringLiteral("space_ref"), _spaceId + path });
    }

    auto *job = new JsonApiJob(_accountState->account(), QStringLiteral("ocs/v1.php/apps/files_sharing/api/v1/shares"), "POST", args, {}, this);
    connect(job, &JsonApiJob::finishedSignal, this, [this, job, user] {
        if (!canShare()) return;
        _sharing = false;
        _userInput->setEnabled(true);
        _roleCombo->setEnabled(true);
        _shareButton->setEnabled(!_recipientId.isEmpty());
        if (!job->ocsSuccess()) {
            const QString err = job->ocsMessage().isEmpty() ? job->errorString() : job->ocsMessage();
            showResult(tr("Failed to share: %1").arg(err), false);
            _statusLabel->setStyleSheet(QStringLiteral("color: #EF4444; font-size: 12px;"));
        } else {
            _modified = true;
            _userInput->clear();
            showResult(tr("Successfully shared with %1").arg(user));
            _statusLabel->setStyleSheet(QStringLiteral("color: #22C55E; font-size: 12px;"));
            fetchShares();
        }
    });
    job->start();
}

void UDriveShareDialog::fetchShares()
{
    if (_target == Target::Workspace) { fetchWorkspaceMembers(); return; }
    if (!canShare() || _remotePath.isEmpty())
        return;

    QString path = _remotePath;
    if (!path.startsWith(QLatin1Char('/')))
        path.prepend(QLatin1Char('/'));

    JsonApiJob::UrlQuery args {
        { QStringLiteral("path"), path },
        { QStringLiteral("reshares"), QStringLiteral("true") }
    };
    if (!_spaceId.isEmpty()) {
        // The server prioritizes space_ref over path. A bare space ID targets
        // the space root; append the selected item's decoded, relative path.
        args.append({ QStringLiteral("space_ref"), _spaceId + path });
    }

    auto *job = new JsonApiJob(_accountState->account(), QStringLiteral("ocs/v1.php/apps/files_sharing/api/v1/shares"), "GET", args, {}, this);
    connect(job, &JsonApiJob::finishedSignal, this, [this, job] {
        if (!canShare())
            return;
        if (!job->ocsSuccess()) {
            const QString err = job->ocsMessage().isEmpty() ? job->errorString() : job->ocsMessage();
            if (!err.isEmpty()) {
                setStatus(tr("Could not load shares: %1").arg(err));
                _statusLabel->setStyleSheet(QStringLiteral("color: #EF4444; font-size: 12px;"));
            }
            return;
        }

        while (auto *item = _sharesLayout->takeAt(0)) {
            if (auto *w = item->widget())
                w->deleteLater();
            delete item;
        }

        if (_publicLinksLayout) {
            while (auto *item = _publicLinksLayout->takeAt(0)) {
                if (auto *w = item->widget())
                    w->deleteLater();
                delete item;
            }
        }

        const auto &data = job->data().value(QStringLiteral("ocs")).toObject().value(QStringLiteral("data")).toArray();
        int userShareCount = 0;
        int publicLinkCount = 0;
        QStringList people;

        const bool dark = QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
        const QString bg = dark ? QStringLiteral("#242528") : QStringLiteral("#FFFFFF");
        const QString text = dark ? QStringLiteral("#EDEDED") : QStringLiteral("#1F2328");
        const QString subtext = dark ? QStringLiteral("#9BA3AF") : QStringLiteral("#59636E");
        const QString border = dark ? QStringLiteral("#3E4146") : QStringLiteral("#D0D7DE");
        const QString cancelHover = dark ? QStringLiteral("#383B40") : QStringLiteral("#E5E7EB");
        const QString arrow = dark ? QStringLiteral("chevron-dark") : QStringLiteral("chevron-light");

        for (const auto &element : data) {
            const auto map = element.toObject();
            const int shareType = map.contains(QStringLiteral("share_type"))
                ? map[QStringLiteral("share_type")].toInt()
                : map[QStringLiteral("shareType")].toInt(-1);
            const QString shareId = map[QStringLiteral("id")].toVariant().toString().trimmed();
            if (!validName(shareId))
                continue;
            const int perms = map[QStringLiteral("permissions")].toInt();

            if (shareType == 3 && _publicLinksLayout) {
                ++publicLinkCount;
                QString linkName = map[QStringLiteral("name")].toString().trimmed();
                if (linkName.isEmpty())
                    linkName = map[QStringLiteral("label")].toString().trimmed();
                if (linkName.isEmpty())
                    linkName = tr("Unnamed link");
                linkName = linkName.left(256).simplified();
                const QString linkUrl = map[QStringLiteral("url")].toString();

                auto *rowWidget = new QWidget(_publicLinksContainer);
                auto *rowLayout = new QHBoxLayout;
                const QDate expiration = QDate::fromString(map.value(QStringLiteral("expiration")).toString().left(10), Qt::ISODate);
                const auto password = map.value(QStringLiteral("share_with"));
                const bool passwordProtected = map.value(QStringLiteral("password_protected")).toBool()
                    || (password.isString() && !password.toString().isEmpty()) || password.toBool();
                const bool isEdit = (perms == 15 || (perms & 2) || (perms & 4) || (perms & 8) || (perms & 16));

                if (_sidebar) {
                    rowWidget->setLayout(rowLayout);
                    rowLayout->setContentsMargins(0, 6, 0, 6);
                    rowLayout->setSpacing(10);

                    auto *iconLabel = new QLabel(rowWidget);
                    iconLabel->setFixedSize(36, 36);
                    iconLabel->setStyleSheet(QStringLiteral("background: %1; border-radius: 18px;").arg(dark ? QStringLiteral("#2E3135") : QStringLiteral("#F3F4F6")));
                    iconLabel->setAlignment(Qt::AlignCenter);
                    iconLabel->setPixmap(UDriveIcons::icon(UDriveIcons::Symbol::Link, false, QColor(text)).pixmap(18, 18));
                    rowLayout->addWidget(iconLabel, 0, Qt::AlignVCenter);

                    auto *infoLayout = new QVBoxLayout;
                    infoLayout->setContentsMargins(0, 0, 0, 0);
                    infoLayout->setSpacing(1);

                    auto *nameLabel = new QLabel(linkName, rowWidget);
                    nameLabel->setObjectName(QStringLiteral("publicLinkName"));
                    nameLabel->setTextFormat(Qt::PlainText);
                    nameLabel->setWordWrap(false);
                    nameLabel->setStyleSheet(QStringLiteral("font-weight: 600; font-size: 13px; color: %1;").arg(text));
                    infoLayout->addWidget(nameLabel);

                    QStringList linkStatus;
                    if (passwordProtected) linkStatus.append(tr("Password protected"));
                    if (expiration.isValid()) linkStatus.append(tr("Expires %1").arg(QLocale().toString(expiration, QLocale::ShortFormat)));
                    if (!linkStatus.isEmpty()) {
                        auto *status = new QLabel(linkStatus.join(QStringLiteral(" · ")), rowWidget);
                        status->setObjectName(QStringLiteral("publicLinkStatus"));
                        status->setTextFormat(Qt::PlainText);
                        status->setStyleSheet(QStringLiteral("font-size: 12px; color: %1;").arg(subtext));
                        infoLayout->addWidget(status);
                    }

                    auto *permCombo = new UDriveComboBox(rowWidget);
                    permCombo->setObjectName(QStringLiteral("publicLinkRoleCombo"));
                    permCombo->setProperty("udriveCombo", true);
                    permCombo->setCursor(Qt::PointingHandCursor);
                    configureShareRolePopup(permCombo);
                    permCombo->addItem(tr("Can view"), 1);
                    permCombo->addItem(tr("Can edit"), 15);
                    permCombo->setCurrentIndex(isEdit ? 1 : 0);
                    connect(permCombo, qOverload<int>(&QComboBox::activated), this, [this, shareId, permCombo](int index) {
                        const int newPerms = permCombo->itemData(index).toInt();
                        updatePublicLinkPermissions(shareId, newPerms);
                    });
                    QFont permFont = permCombo->font();
                    permFont.setPixelSize(12);
                    permCombo->setFont(permFont);
                    permCombo->setStyleSheet(QStringLiteral(
                        "QComboBox#publicLinkRoleCombo {"
                        "    border: none; background: transparent; padding: 0 14px 0 0;"
                        "    color: %1; font-size: 12px; font-weight: 400; combobox-popup: 0;"
                        "}"
                        "QComboBox#publicLinkRoleCombo:hover { color: %2; }"
                        "QComboBox#publicLinkRoleCombo::drop-down { subcontrol-origin: border; subcontrol-position: center right; width: 12px; border: none; }"
                        "QComboBox#publicLinkRoleCombo::down-arrow { image: url(:/udrive/icons/%3.svg); width: 8px; height: 8px; }"
                    ).arg(subtext, text, arrow));
                    permCombo->setFixedHeight(18);
                    auto updatePermWidth = [permCombo] {
                        permCombo->setFixedWidth(permCombo->fontMetrics().horizontalAdvance(permCombo->currentText()) + 26);
                    };
                    updatePermWidth();
                    connect(permCombo, qOverload<int>(&QComboBox::currentIndexChanged), permCombo, [updatePermWidth](int) {
                        updatePermWidth();
                    });
                    infoLayout->addWidget(permCombo, 0, Qt::AlignLeft);

                    rowLayout->addLayout(infoLayout, 1);

                    auto *copyBtn = new QToolButton(rowWidget);
                    copyBtn->setObjectName(QStringLiteral("publicLinkActionButton"));
                    copyBtn->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Copy));
                    copyBtn->setIconSize(QSize(16, 16));
                    copyBtn->setToolTip(tr("Copy link"));
                    copyBtn->setCursor(Qt::PointingHandCursor);
                    connect(copyBtn, &QToolButton::clicked, this, [this, linkUrl] {
                        copyPublicLink(linkUrl);
                    });
                    rowLayout->addWidget(copyBtn, 0, Qt::AlignVCenter);

                    auto *more = new QToolButton(rowWidget);
                    more->setObjectName(QStringLiteral("publicLinkMoreButton"));
                    more->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::MoreVertical));
                    more->setIconSize(QSize(16, 16));
                    more->setToolTip(tr("Link settings"));
                    more->setAccessibleName(tr("Settings for %1").arg(linkName));
                    more->setCursor(Qt::PointingHandCursor);
                    more->setPopupMode(QToolButton::InstantPopup);
                    more->setStyleSheet(QStringLiteral("QToolButton { border: none; border-radius: 4px; background: transparent; padding: 4px; } QToolButton:hover, QToolButton:pressed, QToolButton:open { background: transparent; } QToolButton::menu-indicator { image: none; }"));
                    auto *menu = new UDriveMenu(more);
                    menu->setObjectName(QStringLiteral("publicLinkMenu"));
                    menu->setStyleSheet(QStringLiteral(
                        "QMenu { background: %1; color: %2; border: 1px solid %3; border-radius: 8px; padding: 4px; }"
                        "QMenu::item { padding: 6px 14px 6px 8px; border-radius: 4px; }"
                        "QMenu::item:selected { background: %4; }"
                        "QMenu::separator { height: 1px; background: %3; margin: 4px; }")
                        .arg(bg, text, border, cancelHover));
                    const auto addSetting = [this, menu, shareId, map](UDriveIcons::Symbol icon, const QString &text, const QString &field, const QString &name) {
                        auto *action = menu->addAction(UDriveIcons::icon(icon), text);
                        action->setObjectName(name);
                        connect(action, &QAction::triggered, this, [this, shareId, map, field] { editPublicLink(shareId, map, field); });
                    };
                    addSetting(UDriveIcons::Symbol::Rename, tr("Rename"), QStringLiteral("name"), QStringLiteral("publicLinkRename"));
                    addSetting(UDriveIcons::Symbol::Calendar, expiration.isValid() ? tr("Change expiration date") : tr("Set expiration date"), QStringLiteral("expireDate"), QStringLiteral("publicLinkExpiration"));
                    addSetting(UDriveIcons::Symbol::Lock, passwordProtected ? tr("Change password") : tr("Add password"), QStringLiteral("password"), QStringLiteral("publicLinkPassword"));
                    menu->addSeparator();
                    auto *remove = menu->addAction(UDriveIcons::icon(UDriveIcons::Symbol::Trash), tr("Delete link"));
                    remove->setObjectName(QStringLiteral("publicLinkDelete"));
                    connect(remove, &QAction::triggered, this, [this, shareId] { deletePublicLink(shareId); });
                    more->setMenu(menu);
                    rowLayout->addWidget(more, 0, Qt::AlignVCenter);
                } else {
                    rowWidget->setLayout(rowLayout);
                    rowLayout->setContentsMargins(4, 2, 4, 2);
                    rowLayout->setSpacing(8);

                    auto *iconLabel = new QLabel(rowWidget);
                    iconLabel->setPixmap(UDriveIcons::icon(UDriveIcons::Symbol::Link).pixmap(16, 16));
                    rowLayout->addWidget(iconLabel);

                    auto *nameLabel = new QLabel(linkName, rowWidget);
                    nameLabel->setObjectName(QStringLiteral("publicLinkName"));
                    nameLabel->setTextFormat(Qt::PlainText);
                    nameLabel->setWordWrap(true);
                    nameLabel->setMinimumWidth(0);
                    nameLabel->setStyleSheet(QStringLiteral("font-weight: 500; font-size: 13px;"));
                    rowLayout->addWidget(nameLabel, 1);

                    QStringList linkStatus;
                    if (passwordProtected) linkStatus.append(tr("Password protected"));
                    if (expiration.isValid()) linkStatus.append(tr("Expires %1").arg(QLocale().toString(expiration, QLocale::ShortFormat)));
                    if (!linkStatus.isEmpty()) {
                        auto *status = new QLabel(linkStatus.join(QStringLiteral(" · ")), rowWidget);
                        status->setObjectName(QStringLiteral("publicLinkStatus"));
                        status->setTextFormat(Qt::PlainText);
                        status->setWordWrap(true);
                        status->setStyleSheet(QStringLiteral("font-size: 12px; color: %1;").arg(subtext));
                        rowLayout->addWidget(status);
                    }

                    auto *permCombo = new UDriveComboBox(rowWidget);
                    permCombo->setObjectName(QStringLiteral("publicLinkRoleCombo"));
                    permCombo->setProperty("udriveCombo", true);
                    permCombo->setCursor(Qt::PointingHandCursor);
                    configureShareRolePopup(permCombo);
                    permCombo->addItem(tr("Can view"), 1);
                    permCombo->addItem(tr("Can edit"), 15);
                    permCombo->setCurrentIndex(isEdit ? 1 : 0);
                    connect(permCombo, qOverload<int>(&QComboBox::activated), this, [this, shareId, permCombo](int index) {
                        const int newPerms = permCombo->itemData(index).toInt();
                        updatePublicLinkPermissions(shareId, newPerms);
                    });
                    rowLayout->addWidget(permCombo);

                    auto *copyBtn = new QToolButton(rowWidget);
                    copyBtn->setObjectName(QStringLiteral("publicLinkActionButton"));
                    copyBtn->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Copy));
                    copyBtn->setIconSize(QSize(16, 16));
                    copyBtn->setToolTip(tr("Copy link"));
                    copyBtn->setCursor(Qt::PointingHandCursor);
                    connect(copyBtn, &QToolButton::clicked, this, [this, linkUrl] {
                        copyPublicLink(linkUrl);
                    });
                    rowLayout->addWidget(copyBtn);

                    auto *more = new QToolButton(rowWidget);
                    more->setObjectName(QStringLiteral("publicLinkMoreButton"));
                    more->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::MoreVertical));
                    more->setIconSize(QSize(16, 16));
                    more->setToolTip(tr("Link settings"));
                    more->setAccessibleName(tr("Settings for %1").arg(linkName));
                    more->setCursor(Qt::PointingHandCursor);
                    more->setPopupMode(QToolButton::InstantPopup);
                    more->setStyleSheet(QStringLiteral("QToolButton { border: none; border-radius: 6px; background: transparent; padding: 6px; } QToolButton:hover, QToolButton:pressed, QToolButton:open { background: transparent; } QToolButton::menu-indicator { image: none; }"));
                    auto *menu = new UDriveMenu(more);
                    menu->setObjectName(QStringLiteral("publicLinkMenu"));
                    menu->setStyleSheet(QStringLiteral(
                        "QMenu { background: %1; color: %2; border: 1px solid %3; border-radius: 8px; padding: 5px; menu-scrollable: 1; }"
                        "QMenu::item { padding: 9px 18px 9px 10px; border-radius: 5px; }"
                        "QMenu::item:selected { background: %4; }"
                        "QMenu::separator { height: 1px; background: %3; margin: 5px; }")
                        .arg(dark ? "#303030" : "#FFFFFF", dark ? "#EDEDED" : "#1F2328",
                            dark ? "#454545" : "#D0D7DE", dark ? "#414141" : "#F0F2F4"));
                    menu->setAttribute(Qt::WA_TranslucentBackground, false);
                    const auto addSetting = [this, menu, shareId, map](UDriveIcons::Symbol icon, const QString &text, const QString &field, const QString &name) {
                        auto *action = menu->addAction(UDriveIcons::icon(icon), text);
                        action->setObjectName(name);
                        connect(action, &QAction::triggered, this, [this, shareId, map, field] { editPublicLink(shareId, map, field); });
                    };
                    addSetting(UDriveIcons::Symbol::Rename, tr("Rename"), QStringLiteral("name"), QStringLiteral("publicLinkRename"));
                    addSetting(UDriveIcons::Symbol::Calendar, expiration.isValid() ? tr("Change expiration date") : tr("Set expiration date"), QStringLiteral("expireDate"), QStringLiteral("publicLinkExpiration"));
                    addSetting(UDriveIcons::Symbol::Lock, passwordProtected ? tr("Change password") : tr("Add password"), QStringLiteral("password"), QStringLiteral("publicLinkPassword"));
                    menu->addSeparator();
                    auto *remove = menu->addAction(UDriveIcons::icon(UDriveIcons::Symbol::Trash), tr("Delete link"));
                    remove->setObjectName(QStringLiteral("publicLinkDelete"));
                    connect(remove, &QAction::triggered, this, [this, shareId] { deletePublicLink(shareId); });
                    more->setMenu(menu);
                    rowLayout->addWidget(more);
                }

                _publicLinksLayout->addWidget(rowWidget);
                if (_sidebar) rowWidget->show();
            } else if (shareType == 0 || shareType == 1 || shareType == -1) {
                ++userShareCount;
                const QString displayName = map[QStringLiteral("share_with_displayname")].toString();
                const QString shareWith = displayName.isEmpty() ? map[QStringLiteral("share_with")].toString() : displayName;
                people.append(shareWith);

                auto *rowWidget = new QWidget(_sharesContainer);
                auto *rowLayout = new QHBoxLayout;
                if (_sidebar) {
                    rowWidget->setLayout(rowLayout);
                    rowLayout->setContentsMargins(0, 6, 0, 6);
                    rowLayout->setSpacing(10);

                    auto *avatarLabel = new QLabel(rowWidget);
                    avatarLabel->setFixedSize(36, 36);
                    avatarLabel->setPixmap(createAvatarPixmap(shareWith, 36, devicePixelRatioF()));
                    rowLayout->addWidget(avatarLabel, 0, Qt::AlignVCenter);

                    auto *infoLayout = new QVBoxLayout;
                    infoLayout->setContentsMargins(0, 0, 0, 0);
                    infoLayout->setSpacing(1);

                    auto *userLabel = new QLabel(shareWith, rowWidget);
                    userLabel->setTextFormat(Qt::PlainText);
                    userLabel->setWordWrap(false);
                    userLabel->setStyleSheet(QStringLiteral("font-weight: 600; font-size: 13px; color: %1;").arg(text));
                    infoLayout->addWidget(userLabel);

                    auto *role = new UDriveComboBox(rowWidget);
                    role->setObjectName(QStringLiteral("sharePersonRoleCombo"));
                    role->setProperty("udriveCombo", true);
                    configureShareRolePopup(role);
                    role->addItem(tr("Can view"), 1);
                    role->addItem(tr("Can edit"), 15);
                    role->setCurrentIndex((perms & 2 || perms & 4 || perms & 8 || perms & 16) ? 1 : 0);
                    role->setAccessibleName(tr("Access for %1").arg(shareWith));
                    connect(role, qOverload<int>(&QComboBox::activated), this, [this, shareId, role](int index) {
                        updatePersonPermissions(shareId, role->itemData(index).toInt());
                    });
                    QFont roleFont = role->font();
                    roleFont.setPixelSize(12);
                    role->setFont(roleFont);
                    role->setStyleSheet(QStringLiteral(
                        "QComboBox#sharePersonRoleCombo {"
                        "    border: none; background: transparent; padding: 0 14px 0 0;"
                        "    color: %1; font-size: 12px; font-weight: 400; combobox-popup: 0;"
                        "}"
                        "QComboBox#sharePersonRoleCombo:hover { color: %2; }"
                        "QComboBox#sharePersonRoleCombo::drop-down { subcontrol-origin: border; subcontrol-position: center right; width: 12px; border: none; }"
                        "QComboBox#sharePersonRoleCombo::down-arrow { image: url(:/udrive/icons/%3.svg); width: 8px; height: 8px; }"
                    ).arg(subtext, text, arrow));
                    role->setFixedHeight(18);
                    auto updateRoleWidth = [role] {
                        role->setFixedWidth(role->fontMetrics().horizontalAdvance(role->currentText()) + 26);
                    };
                    updateRoleWidth();
                    connect(role, qOverload<int>(&QComboBox::currentIndexChanged), role, [updateRoleWidth](int) {
                        updateRoleWidth();
                    });
                    infoLayout->addWidget(role, 0, Qt::AlignLeft);

                    rowLayout->addLayout(infoLayout, 1);

                    auto *removeBtn = new QPushButton(rowWidget);
                    removeBtn->setObjectName(QStringLiteral("shareRemovePerson"));
                    removeBtn->hide();
                    connect(removeBtn, &QPushButton::clicked, this, [this, shareId] {
                        revokeShare(shareId);
                    });

                    auto *moreBtn = new QToolButton(rowWidget);
                    moreBtn->setObjectName(QStringLiteral("sharePersonMoreButton"));
                    moreBtn->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::MoreVertical));
                    moreBtn->setIconSize(QSize(16, 16));
                    moreBtn->setToolTip(tr("More options for %1").arg(shareWith));
                    moreBtn->setCursor(Qt::PointingHandCursor);
                    moreBtn->setPopupMode(QToolButton::InstantPopup);
                    moreBtn->setStyleSheet(QStringLiteral("QToolButton { border: none; border-radius: 4px; background: transparent; padding: 4px; } QToolButton:hover, QToolButton:pressed, QToolButton:open { background: transparent; } QToolButton::menu-indicator { image: none; }"));

                    auto *menu = new UDriveMenu(moreBtn);
                    menu->setObjectName(QStringLiteral("sharePersonMenu"));
                    menu->setStyleSheet(QStringLiteral(
                        "QMenu { background: %1; color: %2; border: 1px solid %3; border-radius: 8px; padding: 4px; }"
                        "QMenu::item { padding: 6px 14px 6px 8px; border-radius: 4px; }"
                        "QMenu::item:selected { background: %4; }")
                        .arg(bg, text, border, cancelHover));
                    auto *removeAction = menu->addAction(UDriveIcons::icon(UDriveIcons::Symbol::Trash, false, QColor(QStringLiteral("#EF4444"))), tr("Remove access"));
                    removeAction->setObjectName(QStringLiteral("sharePersonRemoveAction"));
                    connect(removeAction, &QAction::triggered, this, [removeBtn] {
                        removeBtn->click();
                    });
                    moreBtn->setMenu(menu);
                    rowLayout->addWidget(moreBtn, 0, Qt::AlignVCenter);
                } else {
                    rowWidget->setLayout(rowLayout);
                    rowLayout->setContentsMargins(4, 2, 4, 2);

                    auto *avatarLabel = new QLabel(rowWidget);
                    avatarLabel->setFixedSize(28, 28);
                    avatarLabel->setPixmap(createAvatarPixmap(shareWith, 28, devicePixelRatioF()));
                    rowLayout->addWidget(avatarLabel, 0, Qt::AlignVCenter);

                    auto *userLabel = new QLabel(shareWith, rowWidget);
                    userLabel->setTextFormat(Qt::PlainText);
                    userLabel->setWordWrap(true);
                    userLabel->setMinimumWidth(0);
                    userLabel->setStyleSheet(QStringLiteral("font-weight: 500; font-size: 13px;"));
                    rowLayout->addWidget(userLabel, 1);

                    auto *role = new UDriveComboBox(rowWidget);
                    role->setObjectName(QStringLiteral("sharePersonRoleCombo"));
                    role->setProperty("udriveCombo", true);
                    configureShareRolePopup(role);
                    role->addItem(tr("Can view"), 1);
                    role->addItem(tr("Can edit"), 15);
                    role->setCurrentIndex((perms & 2 || perms & 4 || perms & 8 || perms & 16) ? 1 : 0);
                    role->setAccessibleName(tr("Access for %1").arg(shareWith));
                    connect(role, qOverload<int>(&QComboBox::activated), this, [this, shareId, role](int index) {
                        updatePersonPermissions(shareId, role->itemData(index).toInt());
                    });
                    QFont roleFont = role->font();
                    roleFont.setPixelSize(12);
                    role->setFont(roleFont);
                    const int roleTextWidth = qMax(role->fontMetrics().horizontalAdvance(tr("Can view")),
                        role->fontMetrics().horizontalAdvance(tr("Can edit")));
                    role->setFixedWidth(roleTextWidth + 40);
                    rowLayout->addWidget(role);

                    auto *removeBtn = new QPushButton(tr("Remove"), rowWidget);
                    removeBtn->setObjectName(QStringLiteral("shareRemovePerson"));
                    removeBtn->setAccessibleName(tr("Remove access for %1").arg(shareWith));
                    removeBtn->setCursor(Qt::PointingHandCursor);
                    removeBtn->setStyleSheet(QStringLiteral("color: #EF4444; border: none; background: transparent; font-size: 12px; font-weight: 500; padding: 2px 6px;"));
                    connect(removeBtn, &QPushButton::clicked, this, [this, shareId] {
                        revokeShare(shareId);
                    });
                    rowLayout->addWidget(removeBtn);
                }

                _sharesLayout->addWidget(rowWidget);
                if (_sidebar) rowWidget->show();
            }
        }

        if (userShareCount == 0) {
            auto *noShares = new QLabel(_sidebar ? tr("No people added yet.") : tr("Not shared with anyone yet."), _sharesContainer);
            noShares->setStyleSheet(_sidebar ? QStringLiteral("color: %1; font-size: 12px; font-weight: 400;").arg(QGuiApplication::palette().color(QPalette::Window).lightness() < 128 ? QStringLiteral("#9BA3AF") : QStringLiteral("#59636E"))
                : QStringLiteral("color: #9BA3AF; font-size: 12px; font-style: italic;"));
            _sharesLayout->addWidget(noShares);
        }
        _sharesLayout->addStretch(1);

        if (_publicLinksLayout) {
            if (publicLinkCount == 0) {
                auto *noLinks = new QLabel(_sidebar ? tr("No public links created.") : tr("This file has no public link."), _publicLinksContainer);
                noLinks->setStyleSheet(_sidebar ? QStringLiteral("color: %1; font-size: 12px; font-weight: 400;").arg(QGuiApplication::palette().color(QPalette::Window).lightness() < 128 ? QStringLiteral("#9BA3AF") : QStringLiteral("#59636E"))
                    : QStringLiteral("color: #9BA3AF; font-size: 12px; font-style: italic;"));
                _publicLinksLayout->addWidget(noLinks);
            }
            _publicLinksLayout->addStretch(1);
        }
        if (_sidebar) {
            auto *peopleScroll = findChild<QScrollArea *>(QStringLiteral("sharesScrollArea"));
            peopleScroll->setFixedHeight(qBound(40, _sharesLayout->sizeHint().height() + 2, 240));
            _publicLinksScrollArea->setFixedHeight(qBound(40, _publicLinksLayout->sizeHint().height() + 2, 240));
        }
        Q_EMIT accessListLoaded(people, publicLinkCount);
    });
    job->start();
}

void UDriveShareDialog::setStatus(const QString &message)
{
    _statusLabel->setText(message);
    _statusLabel->setVisible(!message.isEmpty());
}

void UDriveShareDialog::showResult(const QString &message, bool success)
{
    _statusLabel->setText(message);
    _statusLabel->hide();
    UDriveToast::showMessage(this, message, success ? UDriveToast::Kind::Success : UDriveToast::Kind::Error);
}

void UDriveShareDialog::updatePersonPermissions(const QString &shareId, int permissions)
{
    if (!canShare() || _sharing || _target != Target::File || !validName(shareId) || (permissions != 1 && permissions != 15)) return;
    _sharing = true;
    _sharesContainer->setEnabled(false);
    _userInput->setEnabled(false);
    _roleCombo->setEnabled(false);
    _shareButton->setEnabled(false);
    _addLinkButton->setEnabled(false);
    if (!_sidebar) {
        setStatus(tr("Updating access…"));
    }
    const JsonApiJob::UrlQuery args{{QStringLiteral("permissions"), QString::number(permissions)}};
    // JsonApiJob takes a decoded path; QUrl encodes it when building the request.
    auto *job = new JsonApiJob(_accountState->account(), QStringLiteral("ocs/v1.php/apps/files_sharing/api/v1/shares/%1").arg(shareId), "PUT", args, {}, this);
    connect(job, &JsonApiJob::finishedSignal, this, [this, job] {
        if (!canShare()) return;
        _sharing = false;
        _sharesContainer->setEnabled(true);
        _userInput->setEnabled(true);
        _roleCombo->setEnabled(true);
        _shareButton->setEnabled(!_recipientId.isEmpty());
        _addLinkButton->setEnabled(true);
        if (job->ocsSuccess()) _modified = true;
        showResult(job->ocsSuccess() ? tr("Access updated.") : tr("Could not update access: %1").arg(job->ocsMessage().isEmpty() ? job->errorString() : job->ocsMessage()), job->ocsSuccess());
        fetchShares();
    });
    job->start();
}

void UDriveShareDialog::revokeShare(const QString &shareId)
{
    if (!canShare() || !validName(shareId))
        return;

    if (!_sidebar) {
        setStatus(tr("Revoking access…"));
        _statusLabel->setStyleSheet(QStringLiteral("color: #3B82F6; font-size: 12px;"));
    }

    auto *job = new JsonApiJob(_accountState->account(), QStringLiteral("ocs/v1.php/apps/files_sharing/api/v1/shares/%1").arg(shareId), "DELETE", {}, {}, this);
    connect(job, &JsonApiJob::finishedSignal, this, [this, job] {
        if (!canShare())
            return;
        if (job->ocsSuccess()) {
            _modified = true;
            showResult(tr("Access removed."));
            _statusLabel->setStyleSheet(QStringLiteral("color: #22C55E; font-size: 12px;"));
            fetchShares();
        } else {
            const QString err = job->ocsMessage().isEmpty() ? job->errorString() : job->ocsMessage();
            showResult(tr("Failed to revoke: %1").arg(err), false);
            _statusLabel->setStyleSheet(QStringLiteral("color: #EF4444; font-size: 12px;"));
        }
    });
    job->start();
}

void UDriveShareDialog::cancelWorkspaceRequest()
{
    const auto job = _workspaceJob;
    _workspaceJob.clear();
    if (job) job->abort();
}

void UDriveShareDialog::cancelPublicLinkRequest()
{
    const auto job = _publicLinkJob;
    _publicLinkJob.clear();
    if (job) job->abort();
}

SimpleNetworkJob *UDriveShareDialog::workspaceRequest(const QByteArray &verb, const QString &suffix, const QJsonObject &body)
{
    const QString path = QStringLiteral("graph/v1beta1/drives/") + _spaceId + QStringLiteral("/root/") + suffix;
    SimpleNetworkJob *job = verb == "DELETE"
        ? new SimpleNetworkJob(_accountState->account(), _accountState->account()->url(), path, verb, SimpleNetworkJob::UrlQuery{}, QNetworkRequest{}, this)
        : verb == "POST" ? new JsonJob(_accountState->account(), _accountState->account()->url(), path, verb, body, QNetworkRequest{}, this)
        : new JsonJob(_accountState->account(), _accountState->account()->url(), path, verb, SimpleNetworkJob::UrlQuery{}, QNetworkRequest{}, this);
    _workspaceJob = job;
    job->setTimeout(std::chrono::seconds(15));
    job->addNewReplyHook([job](QNetworkReply *reply) {
        QObject::connect(reply, &QIODevice::readyRead, job, [job, reply] {
            if (reply->bytesAvailable() > 512 * 1024) job->abort();
        });
    });
    return job;
}

void UDriveShareDialog::fetchWorkspaceMembers()
{
    if (!canShare() || _sharing) return;
    cancelWorkspaceRequest();
    cancelRecipientSearch();
    _canInviteMembers = false;
    _canRemoveMembers = false;
    _userInput->setEnabled(false);
    _roleCombo->setEnabled(false);
    _shareButton->setEnabled(false);
    setStatus(tr("Loading workspace members…"));
    while (auto *item = _sharesLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    auto *job = static_cast<JsonJob *>(workspaceRequest("GET", QStringLiteral("permissions"), {}));
    connect(job, &SimpleNetworkJob::finishedSignal, this, [this, job] {
        if (_workspaceJob != job || !canShare()) return;
        _workspaceJob.clear();
        if (!job->reply() || job->reply()->error() != QNetworkReply::NoError || job->httpStatusCode() != 200
            || !job->data().value(QStringLiteral("value")).isArray()) {
            setStatus(tr("Could not load workspace members. Close and try again."));
            return;
        }
        const auto data = job->data();
        const auto actions = data.value(QStringLiteral("@libre.graph.permissions.actions.allowedValues")).toArray();
        _canInviteMembers = actions.contains(QStringLiteral("libre.graph/driveItem/permissions/create"));
        _canRemoveMembers = actions.contains(QStringLiteral("libre.graph/driveItem/permissions/delete"));
        // These stable built-in IDs let read-only members understand existing
        // grants too. Only the server's allowedValues populate the invite menu.
        _workspaceRoles = {{QStringLiteral("a8d5fe5e-96e3-418d-825b-534dbdf22b99"), tr("Viewer")},
            {QStringLiteral("58c63c02-1d89-4572-916a-870abc5a1b7d"), tr("Editor")},
            {QStringLiteral("312c0871-5ef7-4b3a-85b6-0e4074c64049"), tr("Manager")}};
        _roleCombo->clear();
        int roleWidth = 112;
        for (const auto &value : data.value(QStringLiteral("@libre.graph.permissions.roles.allowedValues")).toArray()) {
            const auto role = value.toObject();
            const QString id = role.value(QStringLiteral("id")).toString();
            QString label = _workspaceRoles.value(id, role.value(QStringLiteral("displayName")).toString());
            if (!validName(id) || label.isEmpty() || _roleCombo->findData(id) >= 0 || _roleCombo->count() >= 50) continue;
            label = label.left(256);
            _workspaceRoles.insert(id, label);
            _roleCombo->addItem(label, id);
            roleWidth = qMax(roleWidth, _roleCombo->fontMetrics().horizontalAdvance(label) + 44);
        }
        _roleCombo->setFixedWidth(qMin(240, roleWidth));
        _roleCombo->setMaxVisibleItems(8);
        _canInviteMembers = _canInviteMembers && _roleCombo->count() > 0;
        _userInput->setEnabled(_canInviteMembers);
        _roleCombo->setEnabled(_canInviteMembers);
        _shareButton->setEnabled(_canInviteMembers && !_recipientId.isEmpty());
        setStatus(_canInviteMembers ? tr("Type at least 2 characters to find a user or group.")
                                               : tr("You can view members but cannot add them."));
        int count = 0;
        for (const auto &value : data.value(QStringLiteral("value")).toArray()) {
            const auto permission = value.toObject();
            const auto identities = permission.value(QStringLiteral("grantedToV2")).toObject();
            const bool group = identities.contains(QStringLiteral("group"));
            const auto identity = identities.value(group ? QStringLiteral("group") : QStringLiteral("user")).toObject();
            const QString id = identity.value(QStringLiteral("id")).toString();
            const QString permissionId = permission.value(QStringLiteral("id")).toString();
            if (id.isEmpty() || !validName(permissionId) || count >= 1000) continue;
            ++count;
            auto *row = new QWidget(_sharesContainer);
            auto *layout = new QHBoxLayout(row);
            layout->setContentsMargins(4, 2, 4, 2);
            const QString displayName = identity.value(QStringLiteral("displayName")).toString();
            auto *name = new QLabel(displayName.isEmpty() ? id : displayName, row);
            name->setTextFormat(Qt::PlainText);
            layout->addWidget(name, 1);
            QStringList roles;
            for (const auto &role : permission.value(QStringLiteral("roles")).toArray())
                roles.append(_workspaceRoles.value(role.toString(), tr("Member")));
            auto *label = new QLabel(roles.isEmpty() ? tr("Member") : roles.join(QStringLiteral(", ")), row);
            label->setTextFormat(Qt::PlainText);
            layout->addWidget(label);
            if (_canRemoveMembers) {
                auto *remove = new QPushButton(tr("Remove"), row);
                remove->setObjectName(QStringLiteral("workspaceRemoveMember"));
                remove->setAutoDefault(false);
                connect(remove, &QPushButton::clicked, this, [this, permissionId] { removeWorkspaceMember(permissionId); });
                layout->addWidget(remove);
            }
            _sharesLayout->addWidget(row);
        }
        if (!count) {
            auto *empty = new QLabel(tr("No workspace members found."), _sharesContainer);
            _sharesLayout->addWidget(empty);
        }
        _sharesLayout->addStretch(1);
    });
    job->start();
}

void UDriveShareDialog::inviteWorkspaceMember()
{
    const QString role = _roleCombo->currentData().toString();
    if (!canShare() || _sharing || _workspaceJob || !_canInviteMembers || _recipientId.isEmpty()
        || (_recipientType != 0 && _recipientType != 1) || role.isEmpty() || _roleCombo->findData(role) < 0) return;
    _sharing = true;
    _userInput->setEnabled(false); _roleCombo->setEnabled(false); _shareButton->setEnabled(false);
    setStatus(tr("Adding workspace member…"));
    const QJsonObject recipient{{QStringLiteral("objectId"), _recipientId},
        {QStringLiteral("@libre.graph.recipient.type"), _recipientType == 1 ? QStringLiteral("group") : QStringLiteral("user")}};
    const QJsonObject body{{QStringLiteral("recipients"), QJsonArray{recipient}}, {QStringLiteral("roles"), QJsonArray{role}}};
    auto *job = workspaceRequest("POST", QStringLiteral("invite"), body);
    connect(job, &SimpleNetworkJob::finishedSignal, this, [this, job] {
        if (_workspaceJob != job || !canShare()) return;
        _workspaceJob.clear(); _sharing = false;
        if (job->reply() && job->reply()->error() == QNetworkReply::NoError
            && (job->httpStatusCode() == 200 || job->httpStatusCode() == 201)) {
            _userInput->clear();
            fetchWorkspaceMembers();
            showResult(tr("Workspace member added."));
        } else {
            _userInput->setEnabled(_canInviteMembers); _roleCombo->setEnabled(_canInviteMembers);
            _shareButton->setEnabled(_canInviteMembers && !_recipientId.isEmpty());
            showResult(tr("Could not add workspace member. Try again."), false);
        }
    });
    job->start();
}

void UDriveShareDialog::removeWorkspaceMember(const QString &permissionId)
{
    if (!canShare() || _sharing || _workspaceJob || !_canRemoveMembers || !validName(permissionId)) return;
    _sharing = true;
    cancelRecipientSearch();
    _userInput->setEnabled(false); _roleCombo->setEnabled(false); _shareButton->setEnabled(false);
    setStatus(tr("Removing workspace member…"));
    auto *job = workspaceRequest("DELETE", QStringLiteral("permissions/") + permissionId, {});
    connect(job, &SimpleNetworkJob::finishedSignal, this, [this, job] {
        if (_workspaceJob != job || !canShare()) return;
        _workspaceJob.clear(); _sharing = false;
        if (job->reply() && job->reply()->error() == QNetworkReply::NoError && job->httpStatusCode() == 204) {
            fetchWorkspaceMembers();
            showResult(tr("Workspace member removed."));
        } else {
            _userInput->setEnabled(_canInviteMembers); _roleCombo->setEnabled(_canInviteMembers);
            _shareButton->setEnabled(_canInviteMembers && !_recipientId.isEmpty());
            showResult(tr("Could not remove workspace member. Try again."), false);
        }
    });
    job->start();
}

void UDriveShareDialog::copyLink()
{
    if (_linkUrl.isValid() && !_linkUrl.isEmpty()) {
        QApplication::clipboard()->setText(_linkUrl.toString());
        showResult(tr("Sign-in link copied to clipboard."));
        _statusLabel->setStyleSheet(QStringLiteral("color: #22C55E; font-size: 12px;"));
    }
}

void UDriveShareDialog::createPublicLink()
{
    if (!canShare() || _sharing || _remotePath.isEmpty())
        return;

    _sharing = true;
    setStatus(tr("Creating public link…"));
    _statusLabel->setStyleSheet(QStringLiteral("color: #3B82F6; font-size: 12px;"));
    if (_addLinkButton)
        _addLinkButton->setEnabled(false);
    if (_shareButton)
        _shareButton->setEnabled(false);

    QString path = _remotePath;
    if (!path.startsWith(QLatin1Char('/')))
        path.prepend(QLatin1Char('/'));

    JsonApiJob::UrlQuery args {
        { QStringLiteral("path"), path },
        { QStringLiteral("shareType"), QStringLiteral("3") },
        { QStringLiteral("permissions"), QStringLiteral("1") },
        { QStringLiteral("name"), tr("Unnamed link") }
    };
    if (!_spaceId.isEmpty()) {
        args.append({ QStringLiteral("space_ref"), _spaceId + path });
    }

    auto *job = new JsonApiJob(_accountState->account(), QStringLiteral("ocs/v1.php/apps/files_sharing/api/v1/shares"), "POST", args, {}, this);
    _publicLinkJob = job;
    connect(job, &JsonApiJob::finishedSignal, this, [this, job] {
        if (_publicLinkJob == job)
            _publicLinkJob.clear();
        _sharing = false;
        if (!canShare()) return;
        if (_addLinkButton)
            _addLinkButton->setEnabled(true);
        if (_shareButton)
            _shareButton->setEnabled(!_recipientId.isEmpty());

        if (!job->ocsSuccess()) {
            const QString err = job->ocsMessage().isEmpty() ? job->errorString() : job->ocsMessage();
            showResult(tr("Failed to create public link: %1").arg(err), false);
            _statusLabel->setStyleSheet(QStringLiteral("color: #EF4444; font-size: 12px;"));
        } else {
            _modified = true;
            const auto data = job->data().value(QStringLiteral("ocs")).toObject().value(QStringLiteral("data")).toObject();
            const QString url = data.value(QStringLiteral("url")).toString();
            const QUrl parsedUrl(url);
            const bool isSafeUrl = parsedUrl.isValid() && (parsedUrl.scheme() == QLatin1String("https") || parsedUrl.scheme() == QLatin1String("http"));
            if (isSafeUrl) {
                QApplication::clipboard()->setText(url);
                showResult(tr("Public link created and copied to clipboard."));
            } else {
                showResult(tr("Public link created."));
            }
            _statusLabel->setStyleSheet(QStringLiteral("color: #22C55E; font-size: 12px;"));
            fetchShares();
        }
    });
    job->start();
}

void UDriveShareDialog::updatePublicLinkPermissions(const QString &shareId, int permissions)
{
    if (permissions == 1 || permissions == 15)
        updatePublicLink(shareId, QStringLiteral("permissions"), QString::number(permissions));
}

void UDriveShareDialog::editPublicLink(const QString &shareId, const QJsonObject &share, const QString &field)
{
    if (!canShare() || _sharing || !validName(shareId)) return;
    const bool rename = field == QLatin1String("name");
    const bool expiration = field == QLatin1String("expireDate");
    if (!rename && !expiration && field != QLatin1String("password")) return;

    auto *dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("publicLinkEditDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(rename ? tr("Rename link") : expiration ? tr("Set expiration date") : tr("Link password"));
    dialog->setMinimumWidth(440);
    styleWorkspaceDialog(dialog);
    const bool dark = QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
    const QString calIcon = dark ? QStringLiteral("calendar-dark") : QStringLiteral("calendar-light");
    dialog->setStyleSheet(dialog->styleSheet() + QStringLiteral(
        "QLineEdit { background: %1; color: %2; border: 1px solid %3; border-radius: 8px; padding: 8px 12px; font-size: 13px; }"
        "QLineEdit:focus { border-color: #22C55E; }"
        "QDateEdit { background: %1; color: %2; border: 1px solid %3; border-radius: 8px; padding: 8px 36px 8px 12px; font-size: 13px; }"
        "QDateEdit:focus { border-color: #22C55E; }"
        "QDateEdit::up-button, QDateEdit::down-button { width: 0px; height: 0px; border: none; }"
        "QDateEdit::drop-down { subcontrol-origin: padding; subcontrol-position: top right; width: 34px; border: none; background: transparent; }"
        "QDateEdit::down-arrow { image: url(:/udrive/icons/%4.svg); width: 18px; height: 18px; }"
        "QCheckBox { color: %2; spacing: 8px; font-size: 13px; }")
        .arg(dark ? "#1B1C1E" : "#FAFCFB", dark ? "#EDEDED" : "#1F2328", dark ? "#3E4146" : "#D0D7DE", calIcon));
    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(14);
    auto *label = new QLabel(rename ? tr("Link name") : expiration ? tr("Expiration date") : tr("New password"), dialog);
    layout->addWidget(label);
    auto *input = new QLineEdit(dialog);
    input->setObjectName(QStringLiteral("publicLinkEditValue"));
    auto *date = new QDateEdit(dialog);
    date->setObjectName(QStringLiteral("publicLinkExpirationDate"));
    date->setDisplayFormat(QStringLiteral("dd/MM/yyyy"));
    date->setButtonSymbols(QAbstractSpinBox::NoButtons);
    date->setCalendarPopup(true);
    date->setMinimumDate(QDate::currentDate());
    const QDate previous = QDate::fromString(share.value(QStringLiteral("expiration")).toString().left(10), Qt::ISODate);
    date->setDate(previous.isValid() && previous >= QDate::currentDate() ? previous : QDate::currentDate().addDays(7));
    auto *remove = new QCheckBox(expiration ? tr("Remove expiration date") : tr("Remove password"), dialog);
    remove->setObjectName(QStringLiteral("publicLinkRemoveSetting"));

    if (auto *calendar = date->calendarWidget()) {
        calendar->setVerticalHeaderFormat(QCalendarWidget::NoVerticalHeader);
        calendar->setHorizontalHeaderFormat(QCalendarWidget::SingleLetterDayNames);
        calendar->setGridVisible(false);
        calendar->setMinimumSize(320, 310);

        if (auto *view = calendar->findChild<QTableView *>(QStringLiteral("qt_calendar_calendarview"))) {
            view->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
            view->verticalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        }

        QTextCharFormat normalDayFormat;
        normalDayFormat.setForeground(dark ? QColor("#EDEDED") : QColor("#1F2328"));
        calendar->setWeekdayTextFormat(Qt::Saturday, normalDayFormat);
        calendar->setWeekdayTextFormat(Qt::Sunday, normalDayFormat);

        QTextCharFormat headerFormat;
        headerFormat.setForeground(dark ? QColor("#9BA3AF") : QColor("#6B7280"));
        headerFormat.setFontWeight(QFont::DemiBold);
        calendar->setHeaderTextFormat(headerFormat);

        auto *prevBtn = calendar->findChild<QToolButton *>(QStringLiteral("qt_calendar_prevmonth"));
        auto *nextBtn = calendar->findChild<QToolButton *>(QStringLiteral("qt_calendar_nextmonth"));
        auto *yearEdit = calendar->findChild<QSpinBox *>(QStringLiteral("qt_calendar_yearedit"));
        if (prevBtn) {
            prevBtn->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Back));
            prevBtn->setText(QString());
        }
        if (nextBtn) {
            nextBtn->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Next));
            nextBtn->setText(QString());
        }
        if (yearEdit) {
            yearEdit->setButtonSymbols(QAbstractSpinBox::NoButtons);
        }

        auto *bottomBar = new QWidget(calendar);
        bottomBar->setObjectName(QStringLiteral("calendarBottomBar"));
        auto *barLayout = new QHBoxLayout(bottomBar);
        barLayout->setContentsMargins(12, 4, 12, 8);
        auto *clearBtn = new QPushButton(tr("Clear"), bottomBar);
        clearBtn->setObjectName(QStringLiteral("calendarClearButton"));
        clearBtn->setCursor(Qt::PointingHandCursor);
        auto *todayBtn = new QPushButton(tr("Today"), bottomBar);
        todayBtn->setObjectName(QStringLiteral("calendarTodayButton"));
        todayBtn->setCursor(Qt::PointingHandCursor);
        barLayout->addWidget(clearBtn);
        barLayout->addStretch(1);
        barLayout->addWidget(todayBtn);
        if (auto *vbox = qobject_cast<QVBoxLayout *>(calendar->layout())) {
            vbox->addWidget(bottomBar);
        }

        connect(clearBtn, &QPushButton::clicked, dialog, [remove, calendar] {
            remove->setChecked(true);
            if (auto *p = calendar->parentWidget()) p->close();
        });
        connect(todayBtn, &QPushButton::clicked, dialog, [date, remove, calendar] {
            remove->setChecked(false);
            const QDate today = QDate::currentDate();
            date->setDate(today);
            calendar->setSelectedDate(today);
            if (auto *p = calendar->parentWidget()) p->close();
        });
        connect(calendar, &QCalendarWidget::clicked, dialog, [remove](const QDate &) {
            remove->setChecked(false);
        });

        const QString calBg = dark ? QStringLiteral("#222428") : QStringLiteral("#FFFFFF");
        const QString calText = dark ? QStringLiteral("#EDEDED") : QStringLiteral("#1F2328");
        const QString calBorder = dark ? QStringLiteral("#383A40") : QStringLiteral("#D0D7DE");
        const QString calHover = dark ? QStringLiteral("#2E3136") : QStringLiteral("#F3F4F6");
        const QString calMuted = dark ? QStringLiteral("#9BA3AF") : QStringLiteral("#6B7280");

        calendar->setStyleSheet(QStringLiteral(
            "QCalendarWidget { background-color: %1; border: 1px solid %3; border-radius: 8px; }"
            "QWidget#qt_calendar_navigationbar { background-color: %1; border-bottom: 1px solid %3; padding: 6px 8px; }"
            "QToolButton#qt_calendar_prevmonth, QToolButton#qt_calendar_nextmonth { background: transparent; border: none; border-radius: 4px; padding: 4px; color: %2; min-width: 24px; min-height: 24px; }"
            "QToolButton#qt_calendar_prevmonth:hover, QToolButton#qt_calendar_nextmonth:hover { background-color: %4; }"
            "QToolButton#qt_calendar_monthbutton, QToolButton#qt_calendar_yearbutton { background: transparent; color: %2; border: none; font-size: 13px; font-weight: 600; padding: 4px 6px; }"
            "QToolButton#qt_calendar_monthbutton:hover, QToolButton#qt_calendar_yearbutton:hover { background-color: %4; border-radius: 4px; }"
            "QToolButton#qt_calendar_monthbutton::menu-indicator { image: none; width: 0px; }"
            "QSpinBox { background: transparent; color: %2; border: none; font-size: 13px; font-weight: 600; }"
            "QSpinBox::up-button, QSpinBox::down-button { width: 0px; height: 0px; border: none; }"
            "QTableView#qt_calendar_calendarview { background-color: %1; color: %2; border: none; selection-background-color: #22C55E; selection-color: #FFFFFF; outline: none; font-size: 12px; }"
            "QTableView#qt_calendar_calendarview::item { border-radius: 4px; padding: 0px; margin: 1px; }"
            "QTableView#qt_calendar_calendarview::item:hover { background-color: %4; }"
            "QTableView#qt_calendar_calendarview::item:selected { background-color: #22C55E; color: #FFFFFF; }"
            "QTableView#qt_calendar_calendarview::item:disabled { color: %5; }"
            "QHeaderView::section { background-color: %1; color: %5; border: none; font-size: 11px; font-weight: 600; padding: 4px 0; }"
            "QWidget#calendarBottomBar { background-color: %1; border-top: 1px solid %3; }"
            "QPushButton#calendarClearButton, QPushButton#calendarTodayButton { background: transparent; color: #22C55E; border: none; font-size: 13px; font-weight: 500; padding: 4px 8px; }"
            "QPushButton#calendarClearButton:hover, QPushButton#calendarTodayButton:hover { text-decoration: underline; }"
        ).arg(calBg, calText, calBorder, calHover, calMuted));
    }

    if (expiration) {
        input->hide();
        layout->addWidget(date);
        if (previous.isValid()) layout->addWidget(remove);
        else remove->hide();
    } else {
        date->hide();
        input->setMaxLength(rename ? 64 : 256);
        input->setText(rename ? share.value(QStringLiteral("name")).toString() : QString());
        input->setEchoMode(rename ? QLineEdit::Normal : QLineEdit::Password);
        input->setAccessibleName(label->text());
        input->selectAll();
        layout->addWidget(input);
        const auto protection = share.value(QStringLiteral("share_with"));
        if (!rename && (share.value(QStringLiteral("password_protected")).toBool() || protection.toBool() || !protection.toString().isEmpty()))
            layout->addWidget(remove);
        else remove->hide();
    }
    auto *errorBox = new QWidget(dialog);
    errorBox->setObjectName(QStringLiteral("publicLinkEditError"));
    errorBox->setAccessibleName(tr("Could not save link settings"));
    errorBox->setStyleSheet(QStringLiteral(
        "QWidget#publicLinkEditError { background: transparent; border: none; }"
        "QLabel { background: transparent; border: none; }"));
    auto *errorLayout = new QVBoxLayout(errorBox);
    errorLayout->setContentsMargins(0, 4, 0, 4);
    errorLayout->setSpacing(6);
    auto *errorTitle = new QLabel(errorBox);
    errorTitle->setObjectName(QStringLiteral("publicLinkEditErrorTitle"));
    errorTitle->setTextFormat(Qt::PlainText);
    errorTitle->setWordWrap(true);
    errorTitle->setStyleSheet(QStringLiteral("font-weight: 600; font-size: 13px; color: %1;").arg(dark ? "#F87171" : "#DC2626"));
    errorLayout->addWidget(errorTitle);
    auto *errorDetails = new QLabel(errorBox);
    errorDetails->setObjectName(QStringLiteral("publicLinkEditErrorDetails"));
    errorDetails->setTextFormat(Qt::PlainText);
    errorDetails->setWordWrap(true);
    errorDetails->setMinimumWidth(0);
    errorDetails->setStyleSheet(QStringLiteral("font-size: 12px; color: %1;").arg(dark ? "#D1D5DB" : "#4B5563"));
    errorLayout->addWidget(errorDetails);
    layout->addWidget(errorBox);
    errorBox->hide();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, dialog);
    buttons->button(QDialogButtonBox::Save)->setText(expiration ? tr("Confirm") : tr("Save"));
    layout->addWidget(buttons);
    const auto updateSave = [dialog, input, date, remove, buttons, rename, expiration] {
        const bool busy = dialog->property("publicLinkSaving").toBool();
        buttons->button(QDialogButtonBox::Save)->setEnabled(!busy && (remove->isChecked() || expiration
            || (rename ? !input->text().trimmed().isEmpty() : !input->text().isEmpty())));
        buttons->button(QDialogButtonBox::Save)->setText(busy ? tr("Saving…") : expiration ? tr("Confirm") : tr("Save"));
        input->setEnabled(!busy && !remove->isChecked());
        date->setEnabled(!busy && !remove->isChecked());
        remove->setEnabled(!busy);
    };
    connect(input, &QLineEdit::textChanged, dialog, updateSave);
    connect(remove, &QCheckBox::toggled, dialog, updateSave);
    connect(remove, &QCheckBox::toggled, dialog, [dialog, errorBox](bool checked) {
        if (checked && errorBox->isVisible()) {
            errorBox->hide();
            dialog->adjustSize();
        }
    });
    updateSave();
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, dialog, [this, dialog, input, date, remove, shareId, field, rename, expiration,
        errorBox, errorTitle, errorDetails, updateSave] {
        if (dialog->property("publicLinkSaving").toBool()) return;
        const QString value = remove->isChecked() ? QString() : expiration ? date->date().toString(Qt::ISODate)
            : rename ? input->text().trimmed() : input->text();
        dialog->setProperty("publicLinkSaving", true);
        errorBox->hide();
        dialog->adjustSize();
        updateSave();
        const QPointer<QDialog> guard(dialog);
        updatePublicLink(shareId, field, value, [guard, input, errorBox, errorTitle, errorDetails, updateSave, rename, expiration](bool success, const QString &error) {
            if (!guard) return;
            guard->setProperty("publicLinkSaving", false);
            if (success) {
                input->clear();
                guard->accept();
                return;
            }
            const auto requirements = !rename && !expiration ? publicLinkPasswordRequirements(error) : QStringList();
            errorTitle->setText(!requirements.isEmpty() ? tr("Choose a stronger password")
                : rename ? tr("Couldn't rename this link") : expiration ? tr("Couldn't update the expiration date") : tr("Couldn't save the password"));
            errorDetails->setText(!requirements.isEmpty() ? QStringLiteral("• ") + requirements.join(QStringLiteral("\n• "))
                : error.isEmpty() ? tr("Check your connection and try again.") : error.simplified().left(400));
            errorBox->show();
            updateSave();
            if (!expiration) { input->setFocus(); input->selectAll(); }
            guard->adjustSize();
        });
    });
    connect(dialog, &QDialog::finished, this, [this, dialog, input] {
        input->clear();
        if (!dialog->property("publicLinkSaving").toBool()) return;
        dialog->setProperty("publicLinkSaving", false);
        cancelPublicLinkRequest();
        setPublicLinkBusy(false);
        setStatus(tr("Update cancelled."));
        _statusLabel->setStyleSheet(QStringLiteral("font-size: 12px;"));
    });
    connect(this, &QDialog::finished, dialog, &QDialog::reject);
    dialog->adjustSize();
    dialog->open();
    if (!expiration) input->setFocus();
}

void UDriveShareDialog::setPublicLinkBusy(bool busy)
{
    _sharing = busy;
    _publicLinksContainer->setEnabled(!busy);
    _addLinkButton->setEnabled(!busy);
    _shareButton->setEnabled(!busy && !_recipientId.isEmpty());
}

void UDriveShareDialog::updatePublicLink(const QString &shareId, const QString &field, const QString &value,
    std::function<void(bool, const QString &)> completion)
{
    if (!canShare() || _sharing || _target != Target::File || !validName(shareId)) {
        if (completion) completion(false, tr("This link can't be updated right now. Close this dialog and try again."));
        return;
    }
    if (field != QLatin1String("name") && field != QLatin1String("password")
        && field != QLatin1String("expireDate") && field != QLatin1String("permissions")) return;
    if (field == QLatin1String("name") && (value.trimmed().isEmpty() || value.size() > 64)) {
        if (completion) completion(false, tr("Enter a link name with 1 to 64 characters."));
        return;
    }
    if (field == QLatin1String("expireDate") && !value.isEmpty()
        && (!QDate::fromString(value, Qt::ISODate).isValid() || QDate::fromString(value, Qt::ISODate) < QDate::currentDate())) {
        if (completion) completion(false, tr("Choose today's date or a later date."));
        return;
    }
    setPublicLinkBusy(true);
    const QString previousStatus = _statusLabel->text();
    const QString previousStyle = _statusLabel->styleSheet();
    if (!_sidebar) {
        setStatus(tr("Updating public link…"));
        _statusLabel->setStyleSheet(QStringLiteral("font-size: 12px;"));
    }
    // The OCS endpoint accepts one changed setting per request.
    const JsonApiJob::UrlQuery args{{field, value}};
    auto *job = new JsonApiJob(_accountState->account(), QStringLiteral("ocs/v1.php/apps/files_sharing/api/v1/shares/%1").arg(shareId), "PUT", args, {}, this);
    _publicLinkJob = job;
    connect(job, &JsonApiJob::finishedSignal, this, [this, job, field, previousStatus, previousStyle, completion = std::move(completion)] {
        if (_publicLinkJob != job) return;
        _publicLinkJob.clear();
        if (!canShare()) return;
        setPublicLinkBusy(false);
        if (!job->ocsSuccess()) {
            const QString err = publicLinkUpdateError(job);
            if (completion) {
                setStatus(previousStatus);
                _statusLabel->setStyleSheet(previousStyle);
                completion(false, err);
                return;
            }
            showResult(tr("Could not update link permissions. Try again."), false);
            _statusLabel->setToolTip(err);
            _statusLabel->setStyleSheet(QStringLiteral("color: #EF4444; font-size: 12px;"));
        } else {
            _modified = true;
            showResult(field == QLatin1String("permissions") ? tr("Public link permissions updated.") : tr("Public link updated."));
            _statusLabel->setStyleSheet(QStringLiteral("color: #22C55E; font-size: 12px;"));
            if (completion) completion(true, {});
        }
        fetchShares();
    });
    job->start();
}

void UDriveShareDialog::copyPublicLink(const QString &url)
{
    const QUrl parsedUrl(url);
    if (!parsedUrl.isValid() || (parsedUrl.scheme() != QLatin1String("https") && parsedUrl.scheme() != QLatin1String("http")))
        return;

    QApplication::clipboard()->setText(url);
    showResult(tr("Public link copied to clipboard."));
    _statusLabel->setStyleSheet(QStringLiteral("color: #22C55E; font-size: 12px;"));
}

void UDriveShareDialog::deletePublicLink(const QString &shareId)
{
    revokeShare(shareId);
}

QString UDriveFileBrowser::uploadDuration(qint64 milliseconds)
{
    if (milliseconds <= 0) return QStringLiteral("00:00");
    const qint64 seconds = milliseconds / 1000;
    const auto twoDigits = [](qint64 value) { return QString::number(value).rightJustified(2, QLatin1Char('0')); };
    const QString minutesAndSeconds = twoDigits(seconds / 60 % 60) + QLatin1Char(':') + twoDigits(seconds % 60);
    return seconds >= 3600 ? QString::number(seconds / 3600) + QLatin1Char(':') + minutesAndSeconds : minutesAndSeconds;
}

UDriveFileBrowser::UDriveFileBrowser(AccountState *state, QWidget *parent)
    : QWidget(parent)
    , _state(state)
{
    setObjectName(QStringLiteral("udriveFileBrowser"));
    cleanupOrphanedPreviews();

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    _pages = new QStackedWidget(this);
    outer->addWidget(_pages);
    auto *files = new QWidget(_pages);
    _pages->addWidget(files);
    _filePreview = new UDriveFilePreview(_pages);
    _pages->addWidget(_filePreview);
    connect(_filePreview, &UDriveFilePreview::backRequested, this, [this] { closePdf(); activeView()->setFocus(); });
    connect(_filePreview, &UDriveFilePreview::downloadRequested, this, &UDriveFileBrowser::downloadSelection);
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    _pdfPreview = new UDrivePdfPreview(_pages);
    _pages->addWidget(_pdfPreview);
    connect(_pdfPreview, &UDrivePdfPreview::downloadRequested, this, &UDriveFileBrowser::downloadSelection);
    connect(_pdfPreview, &UDrivePdfPreview::backRequested, this, [this] {
        closePdf();
        activeView()->setFocus();
    });
#endif
    auto *layout = new QVBoxLayout(files);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(10);

    auto *top = new QHBoxLayout;
    _search = new SearchField(tr("Search"), tr("Clear search"), this);
    _search->setObjectName(QStringLiteral("driveSearch"));
    _search->setPlaceholderText(tr("Search this folder"));
    _search->setAccessibleName(tr("Search this folder"));
    top->addWidget(_search, 1);
    _newButton = new QPushButton(tr("New"), this);
    _newButton->setObjectName(QStringLiteral("driveNew"));
    _newButton->setProperty("udrivePrimary", true);
    _newButton->setProperty("udriveMenuButton", true);
    _newButton->setMinimumWidth(96);
    _newButton->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Add, true));
    auto *newMenu = createFileMenu(_newButton);
    newMenu->setObjectName(QStringLiteral("driveNewMenu"));
    connect(newMenu, &QMenu::aboutToShow, this, [this, newMenu] {
        if (_newButton->isVisible()) newMenu->setMinimumWidth(_newButton->width());
    });
    _newFolderAction = newMenu->addAction(tr("New folder"));
    _uploadAction = newMenu->addAction(tr("Upload file"));
    _newButton->setMenu(newMenu);
    top->addWidget(_newButton);
    layout->addLayout(top);

    _heading = new QLabel(tr("Personal"), this);
    _heading->setObjectName(QStringLiteral("driveHeading"));
    _heading->setTextFormat(Qt::PlainText);
    auto font = _heading->font();
    font.setPointSize(13);
    font.setBold(true);
    _heading->setFont(font);

    _spaces = new UDriveComboBox(this);
    _spaces->setObjectName(QStringLiteral("driveSpaces"));
    _spaces->setProperty("udriveCombo", true);
    _spaces->setAccessibleName(tr("Choose a space"));
    _spaces->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    _spaces->setMinimumWidth(144);
    _spaces->setMaximumWidth(240);
    _spaces->setFixedHeight(34);
    _spaces->setIconSize(QSize(16, 16));
    auto *spaceList = new QListView(_spaces);
    spaceList->setObjectName(QStringLiteral("driveSpacesPopup"));
    spaceList->setFrameShape(QFrame::NoFrame);
    spaceList->setUniformItemSizes(true);
    spaceList->setSpacing(2);
    spaceList->setMinimumWidth(180);
    spaceList->setTextElideMode(Qt::ElideRight);
    spaceList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _spaces->setView(spaceList);
    _spaces->setItemDelegate(new SpaceItemDelegate(_spaces));
    _spaces->setMaxVisibleItems(8);
    _refreshButton = new QPushButton(tr("Refresh"), this);
    _refreshButton->setObjectName(QStringLiteral("driveRefresh"));
    _refreshButton->setProperty("driveToolbarButton", true);
    _refreshButton->setAccessibleName(tr("Refresh"));
    _refreshButton->setToolTip(tr("Refresh"));
    _refreshButton->setText(QString());
    _refreshButton->setFixedSize(34, 34);
    _refreshButton->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Refresh));
    _refreshButton->setIconSize(QSize(20, 20));
    _workspaceActions = new UDriveWorkspaceActions(_state, this);
    connect(_workspaceActions, &UDriveWorkspaceActions::permissionsChanged, this, &UDriveFileBrowser::updateActions);
    connect(_workspaceActions, &UDriveWorkspaceActions::busyChanged, this, &UDriveFileBrowser::updateActions);
    connect(_workspaceActions, &UDriveWorkspaceActions::finished, this, [this](bool success, const QString &message) {
        if (canBrowse() && !message.isEmpty()) _state->account()->spacesManager()->refresh();
        if (!message.isEmpty()) showResult(message, success);
        updateActions();
    });
    _sharesNotifier = new UDriveIncomingSharesNotifier(_state, this);
    connect(_sharesNotifier, &UDriveIncomingSharesNotifier::incomingSharesUpdated, this, [this] {
        if (_section == Section::Shares && _shareTab == ShareTab::WithMe && _path.isEmpty() && !_listing && !_report) {
            refreshIncomingShares();
        }
    });
    if (_state) connect(_state, &AccountState::stateChanged, this, [this] {
        if (!canBrowse() && isVisible()) UDriveToast::dismiss(this);
        if (!canBrowse()) for (auto *dialog : findChildren<QDialog *>()) if (dialog->property("udriveWorkspaceDialog").toBool()) dialog->close();
    });
    connect(_workspaceActions, &UDriveWorkspaceActions::descriptionLoaded, this, &UDriveFileBrowser::editWorkspaceDescription);
    auto *headingRow = new QHBoxLayout;
    headingRow->setSpacing(8);
    headingRow->setAlignment(Qt::AlignVCenter);
    _workspaceBulkActions = new QWidget(this);
    _workspaceBulkActions->setObjectName(QStringLiteral("driveWorkspaceBulkActions"));
    auto *bulk = new QHBoxLayout(_workspaceBulkActions); bulk->setContentsMargins(0, 0, 0, 0); bulk->setSpacing(8);
    auto bulkAction = [this, bulk](const QString &label, const QString &key, UDriveIcons::Symbol icon) {
        auto *action = new QAction(UDriveIcons::icon(icon), label, this);
        action->setObjectName(QStringLiteral("driveWorkspaceBulk_") + key);
        auto *button = new QToolButton(_workspaceBulkActions); button->setDefaultAction(action);
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); button->setAutoRaise(true); button->setIconSize(QSize(18, 18));
        bulk->addWidget(button);
        connect(action, &QAction::triggered, this, [this, key] {
            QList<OpenAPI::OAIDrive> drives;
            for (const auto &index : selectedWorkspaces()) drives.append(index.data(SpaceDriveRole).value<OpenAPI::OAIDrive>());
            workspaceAction(key, drives);
        });
        return action;
    };
    _workspaceDownloadAction = bulkAction(tr("Download"), QStringLiteral("download"), UDriveIcons::Symbol::Download);
    _workspaceDuplicateAction = bulkAction(tr("Duplicate"), QStringLiteral("duplicate"), UDriveIcons::Symbol::Copy);
    _workspaceQuotaAction = bulkAction(tr("Edit quota"), QStringLiteral("quota"), UDriveIcons::Symbol::Cloud);
    _workspaceDisableAction = bulkAction(tr("Disable"), QStringLiteral("disable"), UDriveIcons::Symbol::Disable);
    _workspaceCancelButton = new QToolButton(_workspaceBulkActions); _workspaceCancelButton->setText(tr("Cancel"));
    connect(_workspaceCancelButton, &QToolButton::clicked, _workspaceActions, &UDriveWorkspaceActions::cancel);
    bulk->addWidget(_workspaceCancelButton);
    _workspaceBulkActions->hide(); headingRow->addWidget(_workspaceBulkActions);
    headingRow->addWidget(_heading);
    auto *titleDivider = new QFrame(this);
    titleDivider->setFrameShape(QFrame::VLine);
    titleDivider->setObjectName(QStringLiteral("driveWorkspaceTitleDivider"));
    titleDivider->setFixedHeight(20);
    _workspaceTitleDivider = titleDivider;
    _workspaceTitleDivider->hide();
    headingRow->addWidget(titleDivider);
    _learnWorkspacesButton = new QPushButton(tr("Learn about workspaces"), this);
    _learnWorkspacesButton->setObjectName(QStringLiteral("driveLearnWorkspaces"));
    _learnWorkspacesButton->setFixedHeight(32);
    _learnWorkspacesButton->setCursor(Qt::PointingHandCursor);
    _learnWorkspacesButton->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Help));
    _learnWorkspacesButton->setIconSize(QSize(16, 16));
    _learnWorkspacesButton->hide();
    connect(_learnWorkspacesButton, &QPushButton::clicked, this, [this] {
        Utility::openBrowser(QUrl(QStringLiteral("https://unity-workspace.com/docs")), this);
    });
    headingRow->addWidget(_learnWorkspacesButton);
    _sharesNav = new QWidget(this);
    _sharesNav->setObjectName(QStringLiteral("driveSharesNav"));
    auto *sharesNavLayout = new QHBoxLayout(_sharesNav);
    sharesNavLayout->setContentsMargins(0, 0, 0, 0);
    sharesNavLayout->setSpacing(8);

    _sharedWithMeButton = new QPushButton(UDriveIcons::icon(UDriveIcons::Symbol::Share), tr("Shared with me"), _sharesNav);
    _sharedWithMeButton->setObjectName(QStringLiteral("driveSharedWithMeButton"));
    _sharedWithMeButton->setProperty("shareTab", true);
    _sharedWithMeButton->setCheckable(true);
    _sharedWithMeButton->setChecked(true);
    _sharedWithMeButton->setCursor(Qt::PointingHandCursor);
    _sharedWithMeButton->setFixedHeight(36);
    sharesNavLayout->addWidget(_sharedWithMeButton);

    _sharedWithOthersButton = new QPushButton(UDriveIcons::icon(UDriveIcons::Symbol::ShareOut), tr("Shared with others"), _sharesNav);
    _sharedWithOthersButton->setObjectName(QStringLiteral("driveSharedWithOthersButton"));
    _sharedWithOthersButton->setProperty("shareTab", true);
    _sharedWithOthersButton->setCheckable(true);
    _sharedWithOthersButton->setChecked(false);
    _sharedWithOthersButton->setCursor(Qt::PointingHandCursor);
    _sharedWithOthersButton->setFixedHeight(36);
    sharesNavLayout->addWidget(_sharedWithOthersButton);

    _sharedViaLinkButton = new QPushButton(UDriveIcons::icon(UDriveIcons::Symbol::Link), tr("Shared via link"), _sharesNav);
    _sharedViaLinkButton->setObjectName(QStringLiteral("driveSharedViaLinkButton"));
    _sharedViaLinkButton->setProperty("shareTab", true);
    _sharedViaLinkButton->setCheckable(true);
    _sharedViaLinkButton->setChecked(false);
    _sharedViaLinkButton->setCursor(Qt::PointingHandCursor);
    _sharedViaLinkButton->setFixedHeight(36);
    sharesNavLayout->addWidget(_sharedViaLinkButton);

    _sharesSettingsButton = new QToolButton(_sharesNav);
    _sharesSettingsButton->setObjectName(QStringLiteral("driveShareSettings"));
    _sharesSettingsButton->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Settings));
    _sharesSettingsButton->setAutoRaise(true);
    _sharesSettingsButton->setFixedSize(36, 36);
    _sharesSettingsButton->setIconSize(QSize(20, 20));
    _sharesSettingsButton->setCursor(Qt::PointingHandCursor);
    _sharesSettingsButton->setToolTip(tr("Share settings"));
    sharesNavLayout->addWidget(_sharesSettingsButton);
    auto *shareSettings = createFileMenu(_sharesSettingsButton);
    shareSettings->setObjectName(QStringLiteral("driveShareSettingsMenu"));
    auto *showHiddenShares = shareSettings->addAction(tr("Hidden Shares"));
    showHiddenShares->setObjectName(QStringLiteral("driveShowHiddenShares"));
    showHiddenShares->setCheckable(true);
    connect(showHiddenShares, &QAction::triggered, this, [this](bool hidden) {
        _includeHiddenShares = hidden;
        updateShareFilters();
    });
    auto *resetShares = shareSettings->addAction(tr("Reset filters"));
    resetShares->setObjectName(QStringLiteral("driveResetShareFilters"));
    connect(resetShares, &QAction::triggered, this, &UDriveFileBrowser::resetShareFilters);
    connect(shareSettings, &QMenu::aboutToShow, this, [this, showHiddenShares] {
        showHiddenShares->setChecked(_includeHiddenShares);
        showHiddenShares->setEnabled(_shareTab == ShareTab::WithMe && _path.isEmpty() && _shareMetadataAvailable && !_report);
    });
    _sharesSettingsButton->setMenu(shareSettings);
    _sharesSettingsButton->setPopupMode(QToolButton::InstantPopup);

    _shareScope = new QPushButton(tr("Shared with me"), this);
    _shareScope->setObjectName(QStringLiteral("driveShareScope"));
    _shareScope->setProperty("driveToolbarButton", true);
    _shareScope->setAccessibleName(tr("Share direction"));
    _shareScope->setFixedSize(0, 0);
    _shareScope->hide();
    auto *scopeMenu = createFileMenu(_shareScope);
    auto *scopeGroup = new QActionGroup(scopeMenu);
    scopeGroup->setExclusive(true);
    auto *byMeAction = scopeMenu->addAction(tr("Shared by me"));
    byMeAction->setObjectName(QStringLiteral("driveSharedByMe"));
    byMeAction->setCheckable(true);
    scopeGroup->addAction(byMeAction);

    auto *withMeAction = scopeMenu->addAction(tr("Shared with me"));
    withMeAction->setObjectName(QStringLiteral("driveSharedWithMe"));
    withMeAction->setCheckable(true);
    withMeAction->setChecked(true);
    scopeGroup->addAction(withMeAction);

    connect(byMeAction, &QAction::triggered, this, [this] { setShareTab(ShareTab::WithOthers); });
    connect(withMeAction, &QAction::triggered, this, [this] { setShareTab(ShareTab::WithMe); });
    connect(_sharedWithMeButton, &QPushButton::clicked, this, [this] { setShareTab(ShareTab::WithMe); });
    connect(_sharedWithOthersButton, &QPushButton::clicked, this, [this] { setShareTab(ShareTab::WithOthers); });
    connect(_sharedViaLinkButton, &QPushButton::clicked, this, [this] { setShareTab(ShareTab::ViaLink); });

    _shareScope->setMenu(scopeMenu);
    headingRow->addWidget(_shareScope);
    headingRow->addWidget(_spaces);
    headingRow->addStretch(1);
    _sharesNav->hide();
    headingRow->addWidget(_sharesNav);
    _selectAll = new SelectAllCheckBox(tr("Select all"), this);
    _selectAll->setObjectName(QStringLiteral("driveSelectAll"));
    _selectAll->setTristate(true);
    _selectAll->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    _selectAll->setToolTip(tr("Select all files in the current results."));
    _fileToolbar = new QWidget(this);
    auto *fileToolbar = new QHBoxLayout(_fileToolbar);
    fileToolbar->setContentsMargins(0, 0, 0, 0);
    fileToolbar->setSpacing(8);
    _emptyTrashButton = new QPushButton(tr("Empty trash"), this);
    _emptyTrashButton->setObjectName(QStringLiteral("driveEmptyTrash"));
    _emptyTrashButton->setProperty("driveToolbarButton", true);
    _emptyTrashButton->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Trash, false, QColor(QStringLiteral("#E03838"))));
    _emptyTrashButton->setFixedHeight(34);
    _emptyTrashButton->setToolTip(tr("Permanently delete all items in this space’s Deleted files."));
    _emptyTrashButton->hide();
    fileToolbar->addWidget(_emptyTrashButton);
    connect(_emptyTrashButton, &QPushButton::clicked, this, &UDriveFileBrowser::confirmEmptyTrash);
    fileToolbar->addWidget(_selectAll);
    fileToolbar->addSpacing(12);
    auto *sortLabel = new QLabel(tr("Sort by"), this);
    _sortButton = new QPushButton(this);
    _sortButton->setObjectName(QStringLiteral("driveSort"));
    _sortButton->setProperty("driveToolbarButton", true);
    _sortButton->setFixedHeight(34);
    sortLabel->setBuddy(_sortButton);
    auto *sortMenu = createFileMenu(_sortButton);
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
    // Measure after the workspace stylesheet is applied so every translated
    // option fits while the button and popup keep the same width.
    QTimer::singleShot(0, this, [this, sortMenu] {
        sortMenu->ensurePolished();
        _sortButton->setMinimumWidth(sortMenu->sizeHint().width());
    });
    connect(sortMenu, &QMenu::aboutToShow, this, [this, sortMenu] {
        sortMenu->setFixedWidth(_sortButton->width());
    });
    fileToolbar->addWidget(sortLabel);
    fileToolbar->addWidget(_sortButton);
    fileToolbar->addWidget(_refreshButton);
    _viewButton = new QPushButton(this);
    _viewButton->setObjectName(QStringLiteral("driveViewMode"));
    _viewButton->setProperty("driveToolbarButton", true);
    _viewButton->setAccessibleName(tr("Change file view"));
    _viewButton->setToolTip(tr("Change file view"));
    _viewButton->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::List));
    _viewButton->setIconSize(QSize(18, 18));
    _viewButton->setFixedSize(48, 34);
    auto *viewMenu = createFileMenu(_viewButton);
    auto *viewGroup = new QActionGroup(viewMenu); viewGroup->setExclusive(true);
    const QList<QPair<QString, ViewMode>> modes{{tr("Condensed table view"), ViewMode::Condensed}, {tr("Default table view"), ViewMode::Default}, {tr("Tiles view"), ViewMode::Tiles}};
    for (const auto &mode : modes) {
        auto *action = viewMenu->addAction(UDriveIcons::icon(mode.second == ViewMode::Tiles ? UDriveIcons::Symbol::Grid : UDriveIcons::Symbol::List), mode.first);
        action->setObjectName(mode.second == ViewMode::Tiles ? QStringLiteral("driveViewTiles") : mode.second == ViewMode::Condensed ? QStringLiteral("driveViewCondensed") : QStringLiteral("driveViewDefault"));
        action->setData(int(mode.second)); action->setCheckable(true); viewGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, mode] { setViewMode(mode.second); });
    }
    _viewButton->setMenu(viewMenu);
    fileToolbar->addWidget(_viewButton);
    headingRow->addWidget(_fileToolbar);
    _workspaceHeaderActions = new QWidget(this);
    _workspaceHeaderActions->setObjectName(QStringLiteral("driveWorkspaceHeaderActions"));
    auto *workspaceHeader = new QHBoxLayout(_workspaceHeaderActions);
    workspaceHeader->setContentsMargins(0, 0, 0, 0);
    workspaceHeader->setSpacing(8);
    _workspaceViewButton = new QToolButton(_workspaceHeaderActions);
    _workspaceViewButton->setObjectName(QStringLiteral("driveWorkspaceViewMode"));
    _workspaceViewButton->setAccessibleName(tr("Workspace view"));
    _workspaceViewButton->setToolTip(tr("Workspace view"));
    _workspaceViewButton->setIconSize(QSize(20, 20));
    _workspaceViewButton->setFixedSize(32, 32);
    _workspaceViewButton->setPopupMode(QToolButton::InstantPopup);
    auto *workspaceViewMenu = createFileMenu(_workspaceViewButton);
    auto *workspaceViewGroup = new QActionGroup(workspaceViewMenu);
    workspaceViewGroup->setExclusive(true);
    for (const bool table : {true, false}) {
        auto *action = workspaceViewMenu->addAction(UDriveIcons::icon(table ? UDriveIcons::Symbol::List : UDriveIcons::Symbol::Grid),
            table ? tr("Default table view") : tr("Tiles view"));
        action->setObjectName(table ? QStringLiteral("driveWorkspaceViewTable") : QStringLiteral("driveWorkspaceViewTiles"));
        action->setIconVisibleInMenu(true);
        action->setData(table); action->setCheckable(true); workspaceViewGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, table] { setWorkspaceTableView(table); });
    }
    _workspaceViewButton->setMenu(workspaceViewMenu);
    workspaceHeader->addWidget(_workspaceViewButton);
    auto *workspaceRefresh = new QToolButton(_workspaceHeaderActions);
    workspaceRefresh->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Refresh));
    workspaceRefresh->setAutoRaise(true);
    workspaceRefresh->setFixedSize(32, 32);
    workspaceRefresh->setIconSize(QSize(20, 20));
    workspaceRefresh->setAccessibleName(tr("Refresh"));
    workspaceRefresh->setToolTip(tr("Refresh"));
    connect(workspaceRefresh, &QToolButton::clicked, _refreshButton, &QPushButton::click);
    workspaceHeader->addWidget(workspaceRefresh);
    _workspaceHeaderActions->hide();
    headingRow->addWidget(_workspaceHeaderActions);
    layout->addLayout(headingRow);

    _workspaceControls = new QWidget(this);
    _workspaceControls->setObjectName(QStringLiteral("driveWorkspaceControls"));
    auto *workspaceControls = new QVBoxLayout(_workspaceControls);
    workspaceControls->setContentsMargins(0, 8, 0, 0);
    workspaceControls->setSpacing(16);

    auto *workspaceFilterContent = new QVBoxLayout;
    workspaceFilterContent->setSpacing(4);

    auto *workspaceSearchLabelRow = new QHBoxLayout;
    workspaceSearchLabelRow->addStretch(1);
    auto *searchLabel = new QLabel(tr("Search"), _workspaceControls);
    searchLabel->setFixedWidth(224);
    workspaceSearchLabelRow->addWidget(searchLabel);
    workspaceFilterContent->addLayout(workspaceSearchLabelRow);

    auto *filters = new QHBoxLayout;
    filters->setSpacing(10);
    auto *filterIcon = new QLabel(_workspaceControls);
    filterIcon->setPixmap(UDriveIcons::icon(UDriveIcons::Symbol::Filter).pixmap(22, 22));
    filters->addWidget(filterIcon, 0, Qt::AlignVCenter);
    auto *filterLabel = new QLabel(tr("Filter:"), _workspaceControls);
    filters->addWidget(filterLabel, 0, Qt::AlignVCenter);
    _includeDisabledButton = new QPushButton(tr("Include disabled"), _workspaceControls);
    _includeDisabledButton->setObjectName(QStringLiteral("driveIncludeDisabled"));
    _includeDisabledButton->setCheckable(true);
    _includeDisabledButton->setCursor(Qt::PointingHandCursor);
    filters->addWidget(_includeDisabledButton, 0, Qt::AlignVCenter);
    connect(_includeDisabledButton, &QPushButton::toggled, this, [this](bool included) {
        _includeDisabledWorkspaces = included;
        _workspaceNotice = false;
        if (isWorkspacesOverview()) updateSpaces();
    });
    filters->addStretch(1);

    _workspaceSearch = new QLineEdit(_workspaceControls);
    _workspaceSearch->setObjectName(QStringLiteral("driveWorkspaceSearch"));
    _workspaceSearch->setAccessibleName(tr("Search workspaces"));
    _workspaceSearch->setClearButtonEnabled(true);
    _workspaceSearch->setMinimumHeight(30);
    _workspaceSearch->setFixedWidth(224);
    searchLabel->setBuddy(_workspaceSearch);
    filters->addWidget(_workspaceSearch, 0, Qt::AlignVCenter);

    workspaceFilterContent->addLayout(filters);
    workspaceControls->addLayout(workspaceFilterContent);

    _workspaceSelectionControls = new QWidget(_workspaceControls);
    auto *selection = new QHBoxLayout(_workspaceSelectionControls);
    selection->setContentsMargins(0, 0, 0, 0);
    selection->setSpacing(12);
    _workspaceSelectAll = new SelectAllCheckBox(tr("Select all"), _workspaceControls);
    _workspaceSelectAll->setObjectName(QStringLiteral("driveWorkspaceSelectAll"));
    _workspaceSelectAll->setTristate(true);
    selection->addWidget(_workspaceSelectAll, 0, Qt::AlignVCenter);
    selection->addSpacing(6);
    auto *workspaceSortLabel = new QLabel(tr("Sort by"), _workspaceControls);
    selection->addWidget(workspaceSortLabel, 0, Qt::AlignVCenter);
    _workspaceSort = new UDriveComboBox(_workspaceControls);
    _workspaceSort->setObjectName(QStringLiteral("driveWorkspaceSort"));
    _workspaceSort->setProperty("udriveCombo", true);
    _workspaceSort->addItems({tr("A-Z"), tr("Z-A")});
    _workspaceSort->setMinimumWidth(100);
    workspaceSortLabel->setBuddy(_workspaceSort);
    selection->addWidget(_workspaceSort, 0, Qt::AlignVCenter);
    selection->addStretch(1);
    workspaceControls->addWidget(_workspaceSelectionControls);
    auto *rule = new QFrame(_workspaceControls);
    rule->setObjectName(QStringLiteral("driveWorkspaceRule"));
    rule->setFrameShape(QFrame::HLine);
    workspaceControls->addWidget(rule);
    _workspaceControls->hide();
    layout->addWidget(_workspaceControls);

    _sharesControls = new QWidget(this);
    _sharesControls->setObjectName(QStringLiteral("driveSharesControls"));
    auto *sharesControlsLayout = new QVBoxLayout(_sharesControls);
    sharesControlsLayout->setContentsMargins(0, 8, 0, 0);
    sharesControlsLayout->setSpacing(16);

    auto *sharesFilterContent = new QVBoxLayout;
    sharesFilterContent->setSpacing(4);
    auto *sharesSearchLabelRow = new QHBoxLayout;
    sharesSearchLabelRow->addStretch(1);
    auto *shareSearchLabel = new QLabel(tr("Search"), _sharesControls);
    shareSearchLabel->setFixedWidth(224);
    sharesSearchLabelRow->addWidget(shareSearchLabel);
    sharesFilterContent->addLayout(sharesSearchLabelRow);

    auto *sharesFilterRow = new QHBoxLayout;
    sharesFilterRow->setSpacing(10);

    auto *sharesFilterIcon = new QLabel(_sharesControls);
    sharesFilterIcon->setPixmap(UDriveIcons::icon(UDriveIcons::Symbol::Filter).pixmap(22, 22));
    sharesFilterRow->addWidget(sharesFilterIcon, 0, Qt::AlignVCenter);
    sharesFilterRow->addWidget(new QLabel(tr("Filter:"), _sharesControls), 0, Qt::AlignVCenter);

    _sharesFilterShares = new QPushButton(tr("Shares"), _sharesControls);
    _sharesFilterShares->setObjectName(QStringLiteral("driveShareFilterShares"));
    _sharesFilterShares->setCheckable(true);
    _sharesFilterShares->setChecked(true);
    _sharesFilterShares->setCursor(Qt::PointingHandCursor);
    sharesFilterRow->addWidget(_sharesFilterShares, 0, Qt::AlignVCenter);

    _sharesFilterHidden = new QPushButton(tr("Hidden Shares"), _sharesControls);
    _sharesFilterHidden->setObjectName(QStringLiteral("driveShareFilterHidden"));
    _sharesFilterHidden->setCheckable(true);
    _sharesFilterHidden->setChecked(false);
    _sharesFilterHidden->setCursor(Qt::PointingHandCursor);
    sharesFilterRow->addWidget(_sharesFilterHidden, 0, Qt::AlignVCenter);

    connect(_sharesFilterShares, &QPushButton::clicked, this, [this] {
        _includeHiddenShares = false;
        updateShareFilters();
    });
    connect(_sharesFilterHidden, &QPushButton::clicked, this, [this] {
        _includeHiddenShares = true;
        updateShareFilters();
    });

    _shareTypeFilter = new UDriveComboBox(_sharesControls);
    _shareTypeFilter->setObjectName(QStringLiteral("driveShareTypeFilter"));
    _shareTypeFilter->setProperty("udriveCombo", true);
    _shareTypeFilter->addItems({tr("Share Type"), tr("Files"), tr("Folders")});
    _shareTypeFilter->setMinimumWidth(110);
    sharesFilterRow->addWidget(_shareTypeFilter, 0, Qt::AlignVCenter);

    connect(_shareTypeFilter, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        static_cast<FilesProxy *>(_proxy)->setTypeFilter(index);
        updateEmptyState();
    });

    _sharedByFilter = new UDriveComboBox(_sharesControls);
    _sharedByFilter->setObjectName(QStringLiteral("driveShareByFilter"));
    _sharedByFilter->setProperty("udriveCombo", true);
    _sharedByFilter->setAccessibleName(tr("Shared By"));
    _sharedByFilter->addItem(tr("Anyone"), QString());
    _sharedByFilter->setMinimumWidth(110);
    sharesFilterRow->addWidget(_sharedByFilter, 0, Qt::AlignVCenter);
    connect(_sharedByFilter, &QComboBox::currentIndexChanged, this, &UDriveFileBrowser::updateShareFilters);

    sharesFilterRow->addStretch(1);

    _sharesSearch = new QLineEdit(_sharesControls);
    _sharesSearch->setObjectName(QStringLiteral("driveShareSearch"));
    _sharesSearch->setAccessibleName(tr("Search shares"));
    _sharesSearch->setClearButtonEnabled(true);
    _sharesSearch->setMinimumHeight(30);
    _sharesSearch->setFixedWidth(224);
    shareSearchLabel->setBuddy(_sharesSearch);
    sharesFilterRow->addWidget(_sharesSearch, 0, Qt::AlignVCenter);

    sharesFilterContent->addLayout(sharesFilterRow);
    sharesControlsLayout->addLayout(sharesFilterContent);

    connect(_sharesSearch, &QLineEdit::textChanged, this, &UDriveFileBrowser::updateEmptyState);

    auto *sharesRule = new QFrame(_sharesControls);
    sharesRule->setObjectName(QStringLiteral("driveSharesRule"));
    sharesRule->setFrameShape(QFrame::HLine);
    sharesControlsLayout->addWidget(sharesRule);

    _sharesControls->hide();
    layout->addWidget(_sharesControls);

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
    _tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
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
    _tree->header()->resizeSection(4, 154);
    _tree->header()->setMinimumSectionSize(70);
    _tree->setMouseTracking(true);
    _tree->setItemDelegate(new FileRowDelegate(_tree, [this](const QModelIndex &index, int action) {
        selectOnly(index);
        updateActions();
        if (action == RowFavorite && _favoriteAction->isEnabled())
            _favoriteAction->trigger();
        else if (action == RowShare && _shareAction->isEnabled())
            _shareAction->trigger();
        else if (action == RowLink && _linkAction->isEnabled())
            _linkAction->trigger();
        else if (action == RowMore && _fileMenu->isEnabled())
            _fileMenu->popup(QCursor::pos());
    }, [this](const QModelIndex &index, int action) { return rowActionEnabled(index, action); }));
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
    _tiles->setGridSize(QSize(208, 222));
    _tiles->setIconSize(QSize(128, 128));
    _tiles->setUniformItemSizes(true);
    _tiles->setSelectionMode(QAbstractItemView::ExtendedSelection);
    _tiles->setSelectionBehavior(QAbstractItemView::SelectRows);
    _tiles->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _tiles->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    _tiles->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _tiles->setMouseTracking(true);
    _tiles->setItemDelegate(new FileTileDelegate(_tiles, [this](const QModelIndex &index) {
        selectOnly(index); updateActions();
        if (_fileMenu->isEnabled()) _fileMenu->popup(QCursor::pos());
    }));
    _workspacesModel = new QStandardItemModel(this);
    _workspacesModel->setHorizontalHeaderLabels({tr("Name"), tr("Manager"), tr("Members"), tr("Remaining quota"), tr("Modified"), tr("Actions")});
    _workspacesProxy = new QSortFilterProxyModel(this);
    _workspacesProxy->setSourceModel(_workspacesModel);
    _workspacesProxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    _workspacesProxy->setSortCaseSensitivity(Qt::CaseInsensitive);
    _workspacesProxy->setSortRole(SortRole);
    _workspacesProxy->setFilterKeyColumn(0);
    _workspacesView = new QListView(this);
    _workspacesView->setObjectName(QStringLiteral("driveWorkspacesView"));
    _workspacesView->setAccessibleName(tr("Workspaces catalog"));
    _workspacesView->setModel(_workspacesProxy);
    _workspacesView->setViewMode(QListView::IconMode);
    _workspacesView->setResizeMode(QListView::Adjust);
    _workspacesView->setMovement(QListView::Static);
    _workspacesView->setFlow(QListView::LeftToRight);
    _workspacesView->setWrapping(true);
    _workspacesView->setGridSize(QSize(246, 190));
    _workspacesView->setUniformItemSizes(true);
    _workspacesView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    _workspacesView->setSelectionBehavior(QAbstractItemView::SelectItems);
    _workspacesView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _workspacesView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    _workspacesView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _workspacesView->setMouseTracking(true);
    auto *cardDelegate = new WorkspaceCardDelegate(_workspacesView,
        [this](const QModelIndex &index) { showWorkspaceMembers(index); },
        [this](const QModelIndex &index) {
            showWorkspaceCardMenu(index);
        });
    _workspacesView->setItemDelegate(cardDelegate);
    connect(_workspacesView, &QListView::activated, this, [this](const QModelIndex &index) {
        const QString spaceId = index.data(SpaceIdRole).toString();
        if (!spaceId.isEmpty()) openWorkspace(spaceId);
    });
    connect(_workspacesView->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
        updateSelectionState();
        updateActions();
    });
    _workspacesTable = new QTreeView(this);
    _workspacesTable->setObjectName(QStringLiteral("driveWorkspacesTable"));
    _workspacesTable->setAccessibleName(tr("Workspaces catalog"));
    auto *workspaceHeaderView = new WorkspaceHeader(_workspacesTable);
    _workspaceHeaderSelectAll = workspaceHeaderView->selectAll;
    _workspacesTable->setHeader(workspaceHeaderView);
    _workspacesTable->setModel(_workspacesProxy);
    _workspacesTable->setSelectionModel(_workspacesView->selectionModel());
    _workspacesTable->setRootIsDecorated(false);
    _workspacesTable->setUniformRowHeights(true);
    _workspacesTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    _workspacesTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    _workspacesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _workspacesTable->setSortingEnabled(true);
    _workspacesTable->sortByColumn(0, Qt::AscendingOrder);
    _workspacesTable->setItemDelegate(new WorkspaceTableDelegate(_workspacesTable, [this](const QModelIndex &index) { showWorkspaceCardMenu(index); }));
    _workspacesTable->header()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    _workspacesTable->header()->setStretchLastSection(false);
    _workspacesTable->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    _workspacesTable->header()->setMinimumSectionSize(80);
    for (int col = 1; col <= 4; ++col) _workspacesTable->header()->setSectionResizeMode(col, QHeaderView::ResizeToContents);
    _workspacesTable->header()->setSectionResizeMode(5, QHeaderView::Fixed);
    _workspacesTable->header()->resizeSection(5, 60);
    connect(_workspaceHeaderSelectAll, &QCheckBox::clicked, this, [this] { selectAllFiles(_workspaceHeaderSelectAll->checkState() == Qt::Checked); });
    connect(_workspacesTable, &QTreeView::activated, this, [this](const QModelIndex &index) {
        if (index.column() != 5) openWorkspace(index.siblingAtColumn(0).data(SpaceIdRole).toString());
    });
    connect(_workspacesTable->header(), &QHeaderView::sortIndicatorChanged, this, [this](int column, Qt::SortOrder order) {
        if (column == 5) { _workspacesTable->sortByColumn(0, Qt::AscendingOrder); return; }
        if (column == 0) {
            const QSignalBlocker blocker(_workspaceSort);
            _workspaceSort->setCurrentIndex(order == Qt::AscendingOrder ? 0 : 1);
        }
    });
    _fileViews = new QStackedWidget(this);
    _fileViews->setObjectName(QStringLiteral("driveFileViews"));
    _workspacePage = new QWidget(_fileViews);
    _workspacePage->installEventFilter(this);
    auto *catalog = new QVBoxLayout(_workspacePage);
    catalog->setContentsMargins(0, 0, 0, 0);
    catalog->setSpacing(10);
    _workspaceViews = new QStackedWidget(_workspacePage);
    _workspaceViews->addWidget(_workspacesView);
    _workspaceViews->addWidget(_workspacesTable);
    catalog->addWidget(_workspaceViews, 1);
    _workspaceCount = new QLabel(_workspacePage);
    _workspaceCount->setObjectName(QStringLiteral("driveWorkspaceCount"));
    _workspaceCount->setTextFormat(Qt::PlainText);
    _workspaceCount->setAlignment(Qt::AlignCenter);
    _workspaceCount->setWordWrap(true);
    catalog->addWidget(_workspaceCount);
    _fileViews->addWidget(_tree); _fileViews->addWidget(_tiles); _fileViews->addWidget(_workspacePage);
    _emptyState = new QWidget(_fileViews);
    _emptyState->setObjectName(QStringLiteral("driveEmptyState"));
    auto *emptyLayout = new QVBoxLayout(_emptyState);
    emptyLayout->setAlignment(Qt::AlignCenter);
    emptyLayout->setSpacing(16);
    _emptyIcon = new QLabel(_emptyState);
    _emptyIcon->setObjectName(QStringLiteral("driveEmptyIcon"));
    _emptyIcon->setAlignment(Qt::AlignCenter);
    _emptyIcon->setFixedSize(72, 72);
    _emptyIcon->setPixmap(UDriveIcons::icon(UDriveIcons::Symbol::Share).pixmap(72, 72));
    emptyLayout->addWidget(_emptyIcon, 0, Qt::AlignHCenter);
    _emptyText = new QLabel(tr("No shares"), _emptyState);
    _emptyText->setObjectName(QStringLiteral("driveEmptyText"));
    _emptyText->setAlignment(Qt::AlignCenter);
    _emptyText->setWordWrap(true);
    _emptyText->setMaximumWidth(520);
    emptyLayout->addWidget(_emptyText, 0, Qt::AlignHCenter);
    _fileViews->addWidget(_emptyState);
    auto *content = new QSplitter(Qt::Horizontal, this);
    content->setObjectName(QStringLiteral("driveFileContent"));
    content->setChildrenCollapsible(false);
    content->addWidget(_fileViews);
    content->setStretchFactor(0, 1);
    layout->addWidget(content, 1);

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
    _restoreButton = new QPushButton(tr("Restore"), this);
    _restoreButton->setObjectName(QStringLiteral("driveRestore"));
    _restoreButton->setProperty("udrivePrimary", true);
    _restoreButton->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Restore, true));
    _restoreButton->hide();
    _shareButton = new QPushButton(tr("Share"), this);
    _shareButton->setObjectName(QStringLiteral("driveShare"));
    _shareButton->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Share));
    _shareButton->setCursor(Qt::PointingHandCursor);
    _shareButton->hide();
    actions->addWidget(_openButton);
    actions->addWidget(_downloadButton);
    actions->addWidget(_shareButton);
    actions->addWidget(_restoreButton);
    _fileMenu = createFileMenu(this);
    _fileMenu->setObjectName(QStringLiteral("driveActionsMenu"));
    _shareAction = new QAction(tr("Share"), this);
    _shareAction->setObjectName(QStringLiteral("driveShareAction"));
    _shareAction->setToolTip(tr("Share this item with other users"));
    _renameAction = new QAction(tr("Rename"), this);
    _copyAction = new QAction(tr("Make a copy"), this);
    _moveAction = new QAction(tr("Move to folder"), this);
    _deleteAction = new QAction(tr("Move to Deleted files"), this);
    _restoreAction = new QAction(tr("Restore"), this);
    _permanentDeleteAction = new QAction(tr("Delete permanently"), this);
    _permanentDeleteAction->setObjectName(QStringLiteral("drivePermanentDelete"));
    _favoriteAction = new QAction(tr("Add to Favorites"), this);
    _favoriteAction->setObjectName(QStringLiteral("driveFavorite"));
    _linkAction = new QAction(tr("Copy sign-in link"), this);
    _linkAction->setToolTip(tr("People opening this link must sign in and have access to this file."));
    _webAction = new QAction(tr("Open on Web"), this);
    _webAction->setObjectName(QStringLiteral("driveWebAction"));
    _webAction->setToolTip(tr("Open this item in your web browser"));
    _detailsAction = new QAction(UDriveIcons::icon(UDriveIcons::Symbol::Help), tr("Details"), this);
    _detailsAction->setObjectName(QStringLiteral("driveDetailsAction"));
    connect(_detailsAction, &QAction::triggered, this, [this] {
        _detailsRequested = true;
        updateFileDetails();
        showFileDetailsActions(false);
    });
    _versionsAction = new QAction(UDriveIcons::icon(UDriveIcons::Symbol::Restore), tr("Version history"), this);
    _versionsAction->setObjectName(QStringLiteral("driveMenuVersions"));
    _versionsAction->setToolTip(tr("View and restore previous versions of this file"));
    connect(_versionsAction, &QAction::triggered, this, [this] {
        showFileDetailsVersions(true, true);
    });
    _newFolderAction->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Folder));
    _uploadAction->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Upload));
    _shareAction->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Share));
    _renameAction->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Rename));
    _copyAction->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Copy));
    _moveAction->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Move));
    _deleteAction->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Trash));
    _restoreAction->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Restore));
    _permanentDeleteAction->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Trash, false, QColor(QStringLiteral("#E03838"))));
    _favoriteAction->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Star));
    _linkAction->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Link));
    _webAction->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Online));
    connect(_shareButton, &QPushButton::clicked, _shareAction, &QAction::trigger);
    addFileActions(_fileMenu);
    buildFileDetails();
    content->addWidget(_fileDetails);
    content->setStretchFactor(1, 0);
    layout->addLayout(actions);

    _uploadDetails = new QLabel(this);
    _uploadDetails->setObjectName(QStringLiteral("driveUploadDetails"));
    _uploadDetails->setAccessibleName(tr("Upload details"));
    _uploadDetails->setTextFormat(Qt::PlainText);
    _uploadDetails->setWordWrap(true);
    _uploadDetails->hide();
    layout->addWidget(_uploadDetails);
    _uploadProgressTimer = new QTimer(this);
    _uploadProgressTimer->setObjectName(QStringLiteral("driveUploadProgressTimer"));
    _uploadProgressTimer->setInterval(1000);
    connect(_uploadProgressTimer, &QTimer::timeout, this, &UDriveFileBrowser::refreshUploadDetails);
    auto *transfer = new QHBoxLayout;
    _progress = new QProgressBar(this);
    _progress->setObjectName(QStringLiteral("driveTransferProgress"));
    _progress->setAccessibleName(tr("Download progress"));
    _cancelButton = new QPushButton(tr("Cancel download"), this);
    _cancelButton->setObjectName(QStringLiteral("driveCancelDownload"));
    _cancelButton->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Close));
    transfer->addWidget(_progress, 1);
    transfer->addWidget(_cancelButton);
    layout->addLayout(transfer);
    _progress->hide();
    _cancelButton->hide();

    for (auto *button : {_newButton, _refreshButton, _rootButton, _openButton, _downloadButton, _shareButton, _restoreButton, _cancelButton})
        button->setIconSize(QSize(20, 20));
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
    _workspacesView->viewport()->installEventFilter(this);
    _workspacesTable->viewport()->installEventFilter(this);
    connect(_tiles->verticalScrollBar(), &QScrollBar::valueChanged, this, &UDriveFileBrowser::scheduleThumbnails);
    connect(_proxy, &QAbstractItemModel::rowsInserted, this, &UDriveFileBrowser::scheduleThumbnails);
    connect(_proxy, &QAbstractItemModel::layoutChanged, this, &UDriveFileBrowser::scheduleThumbnails);
    connect(_proxy, &QAbstractItemModel::modelReset, this, &UDriveFileBrowser::scheduleThumbnails);
    connect(_search.data(), &QLineEdit::textChanged, _proxy, &QSortFilterProxyModel::setFilterFixedString);
    connect(_search.data(), &QLineEdit::textChanged, _workspacesProxy, &QSortFilterProxyModel::setFilterFixedString);
    connect(_search.data(), &QLineEdit::textChanged, _workspaceSearch, &QLineEdit::setText);
    connect(_workspaceSearch, &QLineEdit::textChanged, _search.data(), &QLineEdit::setText);
    connect(_search.data(), &QLineEdit::textChanged, _sharesSearch, &QLineEdit::setText);
    connect(_sharesSearch, &QLineEdit::textChanged, _search.data(), &QLineEdit::setText);
    connect(_workspaceSearch, &QLineEdit::textChanged, this, [this] { _workspaceNotice = false; updateSelectionState(); });
    connect(_workspaceSelectAll, &QCheckBox::clicked, this, [this] { selectAllFiles(_workspaceSelectAll->checkState() == Qt::Checked); });
    connect(_workspaceSort, &QComboBox::currentIndexChanged, this, [this](int index) {
        _workspacesTable->sortByColumn(0, index == 1 ? Qt::DescendingOrder : Qt::AscendingOrder);
    });
    _workspacesProxy->sort(0, Qt::AscendingOrder);
    connect(_search.data(), &QLineEdit::textChanged, this, &UDriveFileBrowser::scheduleThumbnails);
    connect(_spaces, &QComboBox::currentIndexChanged, this, &UDriveFileBrowser::selectSpace);
    connect(_refreshButton, &QPushButton::clicked, this, &UDriveFileBrowser::refresh);
    connect(_workspacesProxy, &QAbstractItemModel::rowsInserted, this, &UDriveFileBrowser::updateSelectionState);
    connect(_workspacesProxy, &QAbstractItemModel::rowsRemoved, this, &UDriveFileBrowser::updateSelectionState);
    connect(_workspacesProxy, &QAbstractItemModel::modelReset, this, &UDriveFileBrowser::updateSelectionState);
    connect(_workspacesProxy, &QAbstractItemModel::layoutChanged, this, &UDriveFileBrowser::updateActions);
    addAction(tr("Refresh"), QKeySequence::Refresh, this, &UDriveFileBrowser::refresh)->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    addAction(tr("Search"), QKeySequence::Find, this, [this] { _search->setFocus(); })->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(_tree, &QTreeView::activated, this, &UDriveFileBrowser::activateSelection);
    connect(_tiles, &QListView::activated, this, &UDriveFileBrowser::activateSelection);
    connect(_tree->selectionModel(), &QItemSelectionModel::selectionChanged, this, &UDriveFileBrowser::updateActions);
    connect(_tree->selectionModel(), &QItemSelectionModel::currentChanged, this, &UDriveFileBrowser::updateActions);
    connect(_selectAll, &QCheckBox::clicked, this, [this] { selectAllFiles(_selectAll->checkState() == Qt::Checked); });
    connect(_proxy, &QAbstractItemModel::rowsInserted, this, &UDriveFileBrowser::updateSelectionState);
    connect(_proxy, &QAbstractItemModel::rowsRemoved, this, &UDriveFileBrowser::updateSelectionState);
    connect(_proxy, &QAbstractItemModel::modelReset, this, &UDriveFileBrowser::updateSelectionState);
    connect(_proxy, &QAbstractItemModel::layoutChanged, this, &UDriveFileBrowser::updateActions);
    addAction(tr("Select all"), QKeySequence::SelectAll, this, [this] { selectAllFiles(true); })
        ->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(_openButton, &QPushButton::clicked, this, &UDriveFileBrowser::activateSelection);
    connect(_downloadButton, &QPushButton::clicked, this, &UDriveFileBrowser::downloadSelection);
    connect(_cancelButton, &QPushButton::clicked, this, [this] {
        if (_emptyingTrash) {
            cancelOperation();
            refresh();
            showNotice(tr("Empty trash stopped. Some items may already have been permanently deleted."), Notice::Information);
        } else if (_uploading) {
            cancelOperation();
            refresh();
            showNotice(tr("Upload canceled. Refresh to check the server."), Notice::Information);
        } else {
            cancelDownload();
            showNotice(tr("Download canceled."), Notice::Information);
        }
    });
    connect(_newFolderAction, &QAction::triggered, this, [this] {
        const QUrl root = _root;
        const QString path = _path;
        const QPointer<UDriveFileBrowser> guard(this);
        bool ok = false;
        const QString name = UDriveInputDialog::getText(this, tr("New folder"), tr("Folder name"), {}, tr("OK"), &ok);
        if (guard && ok && _root == root && _path == path && !createFolder(name))
            showResult(tr("Enter a valid new folder name."), false);
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
            UDriveInputDialog::getText(this, copy ? tr("Make a copy") : tr("Rename"), tr("New name"), item.data().toString(), tr("OK"), &ok);
        if (guard && ok && root == selectedRoot() && selectedItem().data(PathRole).toString() == oldPath && !(copy ? copySelected(name) : renameSelected(name)))
            showResult(tr("Choose a different, valid name."), false);
    };
    connect(_renameAction, &QAction::triggered, this, [renameOrCopy] { renameOrCopy(false); });
    connect(_copyAction, &QAction::triggered, this, [renameOrCopy] { renameOrCopy(true); });
    connect(_moveAction, &QAction::triggered, this, [this] {
        const auto item = selectedItem();
        if (!item.isValid()) return;
        const QString oldPath = item.data(PathRole).toString();
        const QString itemName = item.data().toString();
        const bool isFolder = item.data(DirectoryRole).toBool();
        const QUrl root = selectedRoot();
        const QString spaceName = _spaceName;
        const QPointer<UDriveFileBrowser> guard(this);

        QList<QPair<QString, QString>> initialFolders;
        if (_model && _path.isEmpty()) {
            for (int r = 0; r < _model->rowCount(); ++r) {
                const auto idx = _model->index(r, 0);
                if (idx.data(DirectoryRole).toBool() && !idx.data(TrashedRole).toBool()) {
                    const QString p = idx.data(PathRole).toString();
                    const QString n = idx.data().toString();
                    if (!p.isEmpty() && !n.isEmpty()) {
                        initialFolders.append({p, n});
                    }
                }
            }
        }

        auto *dialog = new UDriveMoveDialog(_state, root, spaceName, oldPath, itemName, isFolder, initialFolders, this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(dialog, &QDialog::accepted, this, [this, guard, root, oldPath, dialog] {
            if (!guard || root != selectedRoot() || selectedItem().data(PathRole).toString() != oldPath) {
                return;
            }
            const QString destFolder = dialog->selectedDestination();
            if (!moveSelected(destFolder)) {
                showResult(tr("Choose a valid destination folder. Existing files will not be overwritten."), false);
            }
        });
        dialog->open();
    });
    connect(_deleteAction, &QAction::triggered, this, [this] { confirmDelete(false); });
    connect(_restoreAction, &QAction::triggered, this, &UDriveFileBrowser::restoreSelected);
    connect(_restoreButton, &QPushButton::clicked, this, &UDriveFileBrowser::restoreSelected);
    connect(_permanentDeleteAction, &QAction::triggered, this, [this] { confirmDelete(true); });
    connect(_favoriteAction, &QAction::triggered, this, [this] { setSelectedFavorite(!selectedItem().data(FavoriteRole).toBool()); });
    connect(_shareAction, &QAction::triggered, this, &UDriveFileBrowser::showFileShares);
    connect(_linkAction, &QAction::triggered, this, [this] {
        if (_linkAction->isEnabled()) {
            QApplication::clipboard()->setText(selectedItem().data(LinkRole).toUrl().toString());
            showResult(tr("Sign-in link copied. Access permissions still apply."));
        }
    });
    connect(_rootButton, &QPushButton::clicked, this, [this] { navigateToBreadcrumb({}, _root); });
    connect(_webAction, &QAction::triggered, this, [this] {
        const auto item = selectedItem();
        QUrl targetUrl = item.isValid() ? item.data(LinkRole).toUrl() : QUrl();
        if (targetUrl.scheme() != QStringLiteral("https") && targetUrl.scheme() != QStringLiteral("http"))
            targetUrl = _webUrl;
        Utility::openBrowser(targetUrl, this);
    });
    connect(FolderMan::instance(), &FolderMan::folderListChanged, this, &UDriveFileBrowser::updateActions);
    connect(FolderMan::instance(), &FolderMan::folderSyncStateChange, this, &UDriveFileBrowser::updateActions);
    for (auto *view : {static_cast<QAbstractItemView *>(_tree), static_cast<QAbstractItemView *>(_tiles)}) {
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view, &QAbstractItemView::customContextMenuRequested, this, [this, view](const QPoint &position) {
        const auto index = view->indexAt(position);
        if (!index.isValid())
            return;
        selectOnly(index);
        UDriveMenu menu(this);
        addFileActions(&menu);
        menu.exec(view->viewport()->mapToGlobal(position));
    });
    }

    if (_state && _state->account()) {
        auto *manager = _state->account()->spacesManager();
        connect(manager, &GraphApi::SpacesManager::updated, this, &UDriveFileBrowser::updateSpaces);
        connect(_state, &AccountState::stateChanged, this, [this] {
            if (!canBrowse()) {
                if (_deleteConfirmation) _deleteConfirmation->reject();
                cancelListing();
                cancelDownload();
                closePdf();
                _filePreviewCache.clear(); _previewCacheOrder.clear();
                cancelOperation();
                _model->removeRows(0, _model->rowCount());
                _workspacesModel->removeRows(0, _workspacesModel->rowCount());
                _root.clear(); _webUrl.clear(); _spaceId.clear(); _driveId.clear();
                setMessage(tr("Sign in to browse your files. You can open synced folders from Sync."));
                updateActions();
            } else {
                updateSpaces();
                if (!_root.isEmpty() && !_listing)
                    refresh();
            }
        });
        connect(_state, &QObject::destroyed, this, [this] {
            if (_deleteConfirmation) _deleteConfirmation->reject();
            cancelListing();
            cancelDownload();
            closePdf();
            _filePreviewCache.clear(); _previewCacheOrder.clear();
            cancelOperation();
            _model->removeRows(0, _model->rowCount());
            _workspacesModel->removeRows(0, _workspacesModel->rowCount());
            _root.clear(); _webUrl.clear(); _spaceId.clear(); _driveId.clear();
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
    setWorkspaceTableView(ConfigFile::settingsWithGroup(QStringLiteral("UDriveFileBrowser"))->value(QStringLiteral("workspaceViewMode"), QStringLiteral("tiles")).toString() == QStringLiteral("table"), false);
    updateSpaces();
}

UDriveFileBrowser::~UDriveFileBrowser()
{
    clearFileShares();
    _workspaceActions->cancel();
    cancelListing();
    cancelDownload();
    closePdf();
    cancelOperation();
}

QAbstractItemView *UDriveFileBrowser::activeView() const
{
    if (isWorkspacesOverview() && _workspacesView) return _workspaceTableMode ? static_cast<QAbstractItemView *>(_workspacesTable) : _workspacesView;
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
        UDriveToast::preferenceSaved(this);
    }
    updateEmptyState();
    if (selected.isValid()) activeView()->scrollTo(selected);
    activeView()->setFocus();
    scheduleThumbnails();
}

void UDriveFileBrowser::addFileActions(QMenu *menu)
{
    auto *open = menu->addAction(_openButton->icon(), _openButton->text(), this, &UDriveFileBrowser::activateSelection);
    open->setObjectName(QStringLiteral("driveMenuOpen"));
    menu->addSeparator();
    menu->addActions({_shareAction, _linkAction, _webAction});
    menu->addSeparator();
    auto *download = menu->addAction(UDriveIcons::icon(UDriveIcons::Symbol::Download), tr("Download"), this, &UDriveFileBrowser::downloadSelection);
    download->setObjectName(QStringLiteral("driveMenuDownload"));
    menu->addActions({_renameAction, _copyAction, _moveAction, _deleteAction, _restoreAction, _permanentDeleteAction});
    menu->addSeparator();
    menu->addAction(_favoriteAction);
    menu->addAction(_versionsAction);
    menu->addAction(_detailsAction);
    // Reuse the same availability rules as the buttons for every menu opening.
    connect(menu, &QMenu::aboutToShow, menu, [this, open, download] {
        const bool shares = _tree->property("driveSharesOverview").toBool();
        open->setText(shares && _openButton->text() == tr("Open PDF") ? tr("Open in PDF Viewer") : _openButton->text());
        open->setIcon(_openButton->icon());
        open->setEnabled(_openButton->isEnabled());
        open->setVisible(_section != Section::Trash);
        download->setEnabled(_downloadButton->isEnabled());
        download->setVisible(_section != Section::Trash);
        const auto item = selectedItem();
        const bool singleFile = item.isValid() && !item.data(DirectoryRole).toBool();
        _versionsAction->setVisible(_section != Section::Trash && !shares);
        _versionsAction->setEnabled(canBrowse() && singleFile && !item.data(ShareUnavailableRole).toBool());
    });
}

void UDriveFileBrowser::updateFileColumns()
{
    const bool sharesOverview = _section == Section::Shares && _path.isEmpty();
    _tree->setProperty("driveSharesOverview", sharesOverview);
    _tree->setColumnHidden(1, sharesOverview);
    _tree->setColumnHidden(2, sharesOverview);
    _tree->setColumnHidden(3, !sharesOverview && _section != Section::Favorites && _section != Section::Trash && !sharedByMeOverview());
    _model->setHeaderData(3, Qt::Horizontal, sharesOverview ? (_shareTab == ShareTab::WithMe ? tr("Shared by") : tr("Shared with")) : tr("Location"));
    _tree->header()->setSectionResizeMode(3, sharesOverview ? QHeaderView::Fixed : QHeaderView::Interactive);
    _tree->header()->resizeSection(3, sharesOverview ? 130 : 180);
    _tree->header()->resizeSection(4, sharesOverview ? 70 : 154);
}

void UDriveFileBrowser::buildFileDetails()
{
    const bool dark = QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
    const QString textPrimary = dark ? QStringLiteral("#FFFFFF") : QStringLiteral("#111827");
    const QString textSecondary = dark ? QStringLiteral("#9CA3AF") : QStringLiteral("#6B7280");
    const QString borderColor = dark ? QStringLiteral("rgba(255, 255, 255, 0.08)") : QStringLiteral("#E5E7EB");
    const QString cardBg = dark ? QStringLiteral("rgba(255, 255, 255, 0.04)") : QStringLiteral("#F3F4F6");
    const QString hoverBg = dark ? QStringLiteral("rgba(255, 255, 255, 0.06)") : QStringLiteral("rgba(0, 0, 0, 0.05)");

    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("driveFileDetails"));
    scroll->setAccessibleName(tr("File details"));
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setMinimumWidth(280);
    scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    scroll->viewport()->setAutoFillBackground(false);
    scroll->viewport()->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
    scroll->setStyleSheet(QStringLiteral(
        "QScrollArea#driveFileDetails {"
        "    background: transparent;"
        "    border: none;"
        "    border-left: 1px solid %1;"
        "}"
        "QScrollArea#driveFileDetails > QWidget,"
        "QScrollArea#driveFileDetails > QWidget > QWidget {"
        "    background: transparent;"
        "}"
    ).arg(borderColor));

    auto *panel = new QWidget(scroll);
    panel->setStyleSheet(QStringLiteral("background: transparent;"));
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(12, 12, 12, 16);
    layout->setSpacing(10);

    // Sidebar navigation
    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(4);

    _detailsBack = new QToolButton(panel);
    _detailsBack->setObjectName(QStringLiteral("driveDetailsBack"));
    _detailsBack->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Back));
    _detailsBack->setAccessibleName(tr("Back to Details panel"));
    _detailsBack->setToolTip(tr("Back to Details panel"));
    _detailsBack->setAutoRaise(true);
    _detailsBack->setCursor(Qt::PointingHandCursor);
    _detailsBack->setFixedSize(26, 26);
    connect(_detailsBack, &QToolButton::clicked, this, [this] { showFileDetailsOverview(true); });
    header->addWidget(_detailsBack);
    header->addStretch(1);
    _detailsTitle = new QLabel(tr("Actions"), panel);
    _detailsTitle->setObjectName(QStringLiteral("driveDetailsTitle"));
    _detailsTitle->setStyleSheet(QStringLiteral("font-size: 14px; font-weight: 600; color: %1;").arg(textPrimary));
    header->addWidget(_detailsTitle);
    header->addStretch(1);

    auto *close = new QToolButton(panel);
    close->setObjectName(QStringLiteral("driveDetailsClose"));
    close->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Close));
    close->setAccessibleName(tr("Close details"));
    close->setToolTip(tr("Close details"));
    close->setAutoRaise(true);
    close->setCursor(Qt::PointingHandCursor);
    close->setFixedSize(26, 26);
    close->setStyleSheet(QStringLiteral(
        "QToolButton#driveDetailsClose {"
        "    border: none;"
        "    border-radius: 4px;"
        "    background: transparent;"
        "    padding: 3px;"
        "}"
        "QToolButton#driveDetailsClose:hover {"
        "    background: %1;"
        "}"
    ).arg(hoverBg));
    connect(close, &QToolButton::clicked, this, [this] { _detailsRequested = false; showFileDetailsActions(false); _fileDetails->hide(); });
    header->addWidget(close, 0, Qt::AlignVCenter);
    layout->addLayout(header);

    // 2. File Name (bold text)
    _detailsName = new QLabel(panel);
    _detailsName->setObjectName(QStringLiteral("driveDetailsName"));
    _detailsName->setTextFormat(Qt::PlainText);
    _detailsName->setWordWrap(true);
    _detailsName->setMinimumWidth(0);
    _detailsName->setTextInteractionFlags(Qt::TextSelectableByMouse);
    _detailsName->setStyleSheet(QStringLiteral(
        "QLabel#driveDetailsName {"
        "    font-size: 14px;"
        "    font-weight: 600;"
        "    color: %1;"
        "    margin-top: 4px;"
        "    margin-bottom: 2px;"
        "}"
    ).arg(textPrimary));
    auto *fileHeader = new QHBoxLayout;
    fileHeader->setSpacing(10);
    _detailsFileIcon = new QLabel(panel);
    _detailsFileIcon->setObjectName(QStringLiteral("driveDetailsFileIcon"));
    _detailsFileIcon->setFixedSize(24, 24);
    fileHeader->addWidget(_detailsFileIcon);
    fileHeader->addWidget(_detailsName, 1);
    layout->addLayout(fileHeader);

    _detailsPages = new QStackedWidget(panel);
    _detailsPages->setObjectName(QStringLiteral("driveDetailsPages"));
    layout->addWidget(_detailsPages, 1);
    auto *detailsPage = new QWidget(_detailsPages);
    detailsPage->setObjectName(QStringLiteral("driveDetailsOverview"));
    auto *detailsLayout = new QVBoxLayout(detailsPage);
    detailsLayout->setContentsMargins(0, 0, 0, 0);
    detailsLayout->setSpacing(10);
    _detailsPages->addWidget(detailsPage);

    // 3. Preview Card
    auto *card = new QFrame(detailsPage);
    card->setObjectName(QStringLiteral("driveDetailsCard"));
    card->setFixedHeight(140);
    card->setStyleSheet(QStringLiteral(
        "QFrame#driveDetailsCard {"
        "    background: %1;"
        "    border: 1px solid %2;"
        "    border-radius: 8px;"
        "}"
    ).arg(cardBg, borderColor));
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(6, 6, 6, 6);
    cardLayout->setAlignment(Qt::AlignCenter);
    _detailsIcon = new QLabel(card);
    _detailsIcon->setObjectName(QStringLiteral("driveDetailsIcon"));
    _detailsIcon->setAlignment(Qt::AlignCenter);
    _detailsIcon->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
    cardLayout->addWidget(_detailsIcon);
    detailsLayout->addWidget(card);

    // 4. Sharing status
    _detailsPeople = new QLabel(detailsPage);
    _detailsPeople->setObjectName(QStringLiteral("driveDetailsPeople"));
    _detailsPeople->setTextFormat(Qt::PlainText);
    _detailsPeople->setWordWrap(true);
    _detailsPeople->setStyleSheet(QStringLiteral(
        "QLabel#driveDetailsPeople {"
        "    font-size: 13px;"
        "    color: %1;"
        "    padding: 2px 0 4px 0;"
        "}"
    ).arg(textSecondary));
    detailsLayout->addWidget(_detailsPeople);

    // 5. Metadata Grid (2 columns: Key | Value)
    auto *grid = new QGridLayout;
    grid->setContentsMargins(0, 4, 0, 8);
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(8);
    grid->setColumnStretch(0, 0);
    grid->setColumnStretch(1, 1);

    const auto makeKey = [detailsPage, textSecondary](const QString &title) {
        auto *key = new QLabel(title, detailsPage);
        key->setStyleSheet(QStringLiteral("QLabel { font-size: 12px; font-weight: 500; color: %1; background: transparent; }").arg(textSecondary));
        return key;
    };
    const auto makeValue = [detailsPage, textPrimary](const QString &objectName) {
        auto *val = new QLabel(detailsPage);
        val->setObjectName(objectName);
        val->setTextFormat(Qt::PlainText);
        val->setWordWrap(true);
        val->setTextInteractionFlags(Qt::TextSelectableByMouse);
        val->setMinimumWidth(0);
        val->setStyleSheet(QStringLiteral("QLabel { font-size: 12px; font-weight: 400; color: %1; background: transparent; }").arg(textPrimary));
        return val;
    };

    grid->addWidget(makeKey(tr("Last modified")), 0, 0, Qt::AlignLeft | Qt::AlignTop);
    _detailsModified = makeValue(QStringLiteral("driveDetailsModified"));
    grid->addWidget(_detailsModified, 0, 1, Qt::AlignLeft | Qt::AlignTop);

    grid->addWidget(makeKey(tr("Owner")), 1, 0, Qt::AlignLeft | Qt::AlignTop);
    _detailsOwner = makeValue(QStringLiteral("driveDetailsOwner"));
    grid->addWidget(_detailsOwner, 1, 1, Qt::AlignLeft | Qt::AlignTop);

    grid->addWidget(makeKey(tr("Size")), 2, 0, Qt::AlignLeft | Qt::AlignTop);
    _detailsSize = makeValue(QStringLiteral("driveDetailsSize"));
    grid->addWidget(_detailsSize, 2, 1, Qt::AlignLeft | Qt::AlignTop);

    grid->addWidget(makeKey(tr("Location")), 3, 0, Qt::AlignLeft | Qt::AlignTop);
    _detailsLocation = makeValue(QStringLiteral("driveDetailsLocation"));
    grid->addWidget(_detailsLocation, 3, 1, Qt::AlignLeft | Qt::AlignTop);

    detailsLayout->addLayout(grid);

    // 6. Sidebar sections
    const auto makeAccordionHeader = [detailsPage, detailsLayout, borderColor, textPrimary, textSecondary, hoverBg](
        UDriveIcons::Symbol sym, const QString &title, QLabel **chevronOut) {
        auto *row = new QWidget(detailsPage);
        auto *rowLayout = new QVBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(4);
        auto *separator = new QFrame(row);
        separator->setFixedHeight(1);
        separator->setStyleSheet(QStringLiteral("background: %1; border: none;").arg(borderColor));
        rowLayout->addWidget(separator);
        auto *btn = new QPushButton(row);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFixedHeight(44);
        btn->setStyleSheet(QStringLiteral(
            "QPushButton {"
            "    border: 1px solid transparent;"
            "    border-radius: 8px;"
            "    padding: 0;"
            "    min-height: 42px; max-height: 42px;"
            "    background: transparent;"
            "}"
            "QPushButton:hover {"
            "    background: %1;"
            "}"
            "QPushButton:focus { border-color: #00856B; }"
        ).arg(hoverBg));

        auto *h = new QHBoxLayout(btn);
        h->setContentsMargins(10, 0, 10, 0);
        h->setSpacing(8);

        auto *iconLbl = new QLabel(btn);
        iconLbl->setAttribute(Qt::WA_TransparentForMouseEvents);
        iconLbl->setPixmap(UDriveIcons::icon(sym).pixmap(16, 16));
        iconLbl->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
        h->addWidget(iconLbl);

        auto *titleLbl = new QLabel(title, btn);
        titleLbl->setAttribute(Qt::WA_TransparentForMouseEvents);
        titleLbl->setStyleSheet(QStringLiteral("QLabel { border: none; background: transparent; color: %1; font-size: 13px; font-weight: 500; }").arg(textPrimary));
        h->addWidget(titleLbl);

        h->addStretch(1);

        auto *chev = new QLabel(QStringLiteral("›"), btn);
        chev->setAttribute(Qt::WA_TransparentForMouseEvents);
        chev->setStyleSheet(QStringLiteral("QLabel { border: none; background: transparent; color: %1; font-size: 15px; font-weight: bold; }").arg(textSecondary));
        h->addWidget(chev);

        if (chevronOut) *chevronOut = chev;
        rowLayout->addWidget(btn);
        detailsLayout->addWidget(row);
        return btn;
    };

    auto *actionsHeader = makeAccordionHeader(UDriveIcons::Symbol::Open, tr("Actions"), nullptr);
    actionsHeader->setObjectName(QStringLiteral("driveDetailsActions"));

    auto *actionsContainer = new QWidget(_detailsPages);
    actionsContainer->setObjectName(QStringLiteral("driveDetailsActionsPage"));
    actionsContainer->setAccessibleName(tr("File actions"));
    auto *actionsLayout = new QVBoxLayout(actionsContainer);
    actionsLayout->setContentsMargins(0, 12, 0, 0);
    actionsLayout->setSpacing(6);
    _detailsPages->addWidget(actionsContainer);

    const auto actionStyle = QStringLiteral(
        "QPushButton { background: transparent; color: %1; border: 1px solid transparent;"
        " border-radius: 6px; padding: 0; min-width: 0; min-height: 42px; max-height: 42px; font-size: 13px; }"
        "QPushButton:hover { background: %2; }"
        "QPushButton:focus { border: 1px solid #00856B; }"
        "QPushButton:disabled { color: %3; }").arg(textPrimary, hoverBg, textSecondary);
    _detailsOpen = new FileDetailsActionButton(actionsContainer);
    _detailsOpen->setObjectName(QStringLiteral("driveDetailsOpen"));
    _detailsOpen->setStyleSheet(actionStyle);
    _detailsOpen->setCursor(Qt::PointingHandCursor);
    _detailsOpen->setFixedHeight(44);
    _detailsOpen->setIconSize(QSize(20, 20));
    connect(_detailsOpen, &QPushButton::clicked, this, &UDriveFileBrowser::activateSelection);
    actionsLayout->addWidget(_detailsOpen);

    const auto actionButton = [actionsContainer, actionsLayout, actionStyle](QAction *action, const QString &objectName) {
        auto *button = new FileDetailsActionButton(actionsContainer);
        button->setObjectName(objectName);
        button->setStyleSheet(actionStyle);
        button->setCursor(Qt::PointingHandCursor);
        button->setFixedHeight(44);
        button->setIconSize(QSize(20, 20));
        const auto sync = [button, action] {
            button->setText(action->text());
            button->setIcon(action->icon());
            button->setEnabled(action->isEnabled());
            button->setVisible(action->isVisible());
            button->setToolTip(action->toolTip());
        };
        sync();
        connect(action, &QAction::changed, button, sync);
        connect(button, &QPushButton::clicked, action, &QAction::trigger);
        actionsLayout->addWidget(button);
        return button;
    };
    _detailsDownload = actionButton(_fileMenu->findChild<QAction *>(QStringLiteral("driveMenuDownload")), QStringLiteral("driveDetailsDownload"));
    actionButton(_deleteAction, QStringLiteral("driveDetailsDelete"));
    actionButton(_moveAction, QStringLiteral("driveDetailsMove"));
    actionButton(_copyAction, QStringLiteral("driveDetailsCopy"));
    _detailsRename = actionButton(_renameAction, QStringLiteral("driveDetailsRename"));
    actionButton(_favoriteAction, QStringLiteral("driveDetailsFavorite"));
    _detailsShare = actionButton(_shareAction, QStringLiteral("driveDetailsShare"));
    _detailsWeb = actionButton(_webAction, QStringLiteral("driveDetailsWeb"));
    actionButton(_versionsAction, QStringLiteral("driveDetailsVersionsAction"));
    actionsLayout->addStretch(1);
    connect(actionsHeader, &QPushButton::clicked, this, [this] { showFileDetailsActions(true, true); });

    auto *sharesHeader = makeAccordionHeader(UDriveIcons::Symbol::Share, tr("Shares"), nullptr);
    sharesHeader->setObjectName(QStringLiteral("driveDetailsShares"));
    connect(sharesHeader, &QPushButton::clicked, this, [this] { _shareAction->trigger(); });

    _detailsVersionsHeader = makeAccordionHeader(UDriveIcons::Symbol::Restore, tr("Versions"), nullptr);
    _detailsVersionsHeader->setObjectName(QStringLiteral("driveDetailsVersions"));
    connect(_detailsVersionsHeader, &QPushButton::clicked, this, [this] { showFileDetailsVersions(true, true); });

    auto *versionsContainer = new QWidget(_detailsPages);
    versionsContainer->setObjectName(QStringLiteral("driveDetailsVersionsPage"));
    versionsContainer->setAccessibleName(tr("File versions"));
    auto *versionsPageLayout = new QVBoxLayout(versionsContainer);
    versionsPageLayout->setContentsMargins(0, 0, 0, 0);
    versionsPageLayout->setSpacing(0);

    auto *versionsTopDivider = new QFrame(versionsContainer);
    versionsTopDivider->setFrameShape(QFrame::HLine);
    versionsTopDivider->setFixedHeight(1);
    versionsTopDivider->setStyleSheet(QStringLiteral("background: %1; border: none;").arg(borderColor));
    versionsPageLayout->addWidget(versionsTopDivider);

    _versionsStatusLabel = new QLabel(versionsContainer);
    _versionsStatusLabel->setObjectName(QStringLiteral("driveVersionsStatus"));
    _versionsStatusLabel->setAlignment(Qt::AlignCenter);
    _versionsStatusLabel->setWordWrap(true);
    _versionsStatusLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 13px; padding: 32px 16px;").arg(textSecondary));
    versionsPageLayout->addWidget(_versionsStatusLabel);

    _versionsScroll = new QScrollArea(versionsContainer);
    _versionsScroll->setObjectName(QStringLiteral("driveVersionsScroll"));
    _versionsScroll->setWidgetResizable(true);
    _versionsScroll->setFrameShape(QFrame::NoFrame);
    _versionsScroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; border: none; }"));

    auto *versionsList = new QWidget(_versionsScroll);
    versionsList->setObjectName(QStringLiteral("driveVersionsList"));
    versionsList->setStyleSheet(QStringLiteral("background: transparent;"));
    _versionsListLayout = new QVBoxLayout(versionsList);
    _versionsListLayout->setContentsMargins(0, 8, 0, 8);
    _versionsListLayout->setSpacing(0);
    _versionsListLayout->addStretch(1);
    _versionsScroll->setWidget(versionsList);
    versionsPageLayout->addWidget(_versionsScroll, 1);

    _detailsPages->addWidget(versionsContainer);
    _detailsVersionsPage = versionsContainer;

    auto *bottomBorder = new QFrame(panel);
    bottomBorder->setFrameShape(QFrame::HLine);
    bottomBorder->setFixedHeight(1);
    bottomBorder->setStyleSheet(QStringLiteral("background: %1; border: none;").arg(borderColor));
    detailsLayout->addWidget(bottomBorder);

    detailsLayout->addStretch(1);
    scroll->setWidget(panel);
    _fileDetails = scroll;
    showFileDetailsActions(false);
    _fileDetails->hide();
}

void UDriveFileBrowser::showFileDetailsOverview(bool focus)
{
    if (!_detailsPages) return;
    clearFileShares();
    if (_versionsJob) {
        _versionsJob->abort();
        _versionsJob.clear();
    }
    _detailsPages->setCurrentIndex(0);
    _detailsBack->hide();
    _detailsTitle->hide();
    _detailsFileIcon->hide();
    _fileDetails->setAccessibleName(tr("File details"));
    static_cast<QScrollArea *>(_fileDetails)->verticalScrollBar()->setValue(0);
    if (focus && _fileDetails->isVisible()) {
        if (auto *actionsBtn = _detailsPages->widget(0)->findChild<QPushButton *>(QStringLiteral("driveDetailsActions")))
            actionsBtn->setFocus(Qt::OtherFocusReason);
    }
}

void UDriveFileBrowser::showFileDetailsActions(bool show, bool focus)
{
    if (!_detailsPages) return;
    if (!show) {
        showFileDetailsOverview(focus);
        return;
    }
    clearFileShares();
    if (_versionsJob) {
        _versionsJob->abort();
        _versionsJob.clear();
    }
    _detailsTitle->setText(tr("Actions"));
    _detailsPages->setCurrentIndex(1);
    _detailsBack->show();
    _detailsTitle->show();
    _detailsFileIcon->show();
    _fileDetails->setAccessibleName(tr("File actions"));
    static_cast<QScrollArea *>(_fileDetails)->verticalScrollBar()->setValue(0);
    if (focus && _fileDetails->isVisible()) {
        _detailsBack->setFocus(Qt::OtherFocusReason);
    }
}

void UDriveFileBrowser::showFileDetailsVersions(bool show, bool focus)
{
    if (!_detailsPages) return;
    if (!show) {
        showFileDetailsOverview(focus);
        return;
    }
    clearFileShares();
    _detailsRequested = true;
    updateFileDetails();
    _detailsTitle->setText(tr("Versions"));
    _detailsPages->setCurrentWidget(_detailsVersionsPage);
    _detailsBack->show();
    _detailsTitle->show();
    _detailsFileIcon->show();
    _fileDetails->setAccessibleName(tr("File versions"));
    static_cast<QScrollArea *>(_fileDetails)->verticalScrollBar()->setValue(0);
    if (focus && _fileDetails->isVisible()) {
        _detailsBack->setFocus(Qt::OtherFocusReason);
    }
    loadVersionsForItem(selectedItem());
}

QUrl UDriveFileBrowser::metaVersionsUrl(const QString &fileId) const
{
    if (!_state || !_state->account() || fileId.trimmed().isEmpty()) return {};
    // SEC-03: Reject fileId containing path traversal or URI delimiter characters
    if (fileId.contains(QLatin1Char('/')) || fileId.contains(QLatin1Char('\\')) || fileId.contains(QLatin1Char('?'))
        || fileId.contains(QLatin1Char('#')) || fileId.contains(QStringLiteral(".."))) {
        return {};
    }
    const QString encodedId = QString::fromUtf8(QUrl::toPercentEncoding(fileId.trimmed()));
    const QString davPath = _state->account()->davPath();
    if (davPath.contains(QStringLiteral("remote.php"))) {
        return Utility::concatUrlPath(_state->account()->url(), QStringLiteral("remote.php/dav/meta/") + encodedId + QStringLiteral("/v/"));
    }
    return Utility::concatUrlPath(_state->account()->url(), QStringLiteral("dav/meta/") + encodedId + QStringLiteral("/v/"));
}

void UDriveFileBrowser::loadVersionsForItem(const QModelIndex &index)
{
    if (!index.isValid() || index.data(DirectoryRole).toBool()) {
        if (_versionsStatusLabel) {
            _versionsStatusLabel->setText(tr("Versions are only available for files."));
            _versionsStatusLabel->show();
        }
        if (_versionsScroll) _versionsScroll->hide();
        return;
    }

    if (_versionsJob) {
        _versionsJob->abort();
        _versionsJob.clear();
    }

    if (_versionsListLayout) {
        while (_versionsListLayout->count() > 1) {
            auto *item = _versionsListLayout->takeAt(0);
            if (item->widget()) delete item->widget();
            delete item;
        }
    }

    if (_versionsStatusLabel) {
        _versionsStatusLabel->setText(tr("Loading versions…"));
        _versionsStatusLabel->show();
    }
    if (_versionsScroll) _versionsScroll->hide();

    const QString fileId = index.data(FileIdRole).toString();
    const QString filePath = index.data(PathRole).toString();
    const QString fileName = index.data().toString();
    const QUrl root = index.data(RootRole).toUrl();
    const QString permissions = index.data(PermissionsRole).toString();
    const bool canRestore = permissions.isEmpty() || permissions.contains(QLatin1Char('W')) || permissions.contains(QLatin1Char('V'));

    const QUrl versionsUrl = metaVersionsUrl(fileId);
    if (versionsUrl.isEmpty()) {
        if (_versionsStatusLabel) {
            _versionsStatusLabel->setText(tr("No previous versions available."));
            _versionsStatusLabel->show();
        }
        return;
    }

    QNetworkRequest request(versionsUrl);
    request.setRawHeader("Depth", "1");
    request.setRawHeader("Content-Type", "text/xml");
    auto *job = new SimpleNetworkJob(_state->account(), versionsUrl, QString(), "PROPFIND", nullptr, request, this);
    _versionsJob = job;

    connect(job, &AbstractNetworkJob::finishedSignal, this, [this, job, fileName, filePath, root, canRestore] {
        if (_versionsJob != job) return;
        _versionsJob.clear();

        if (!job->reply() || job->reply()->error() != QNetworkReply::NoError
            || job->httpStatusCode() < 200 || job->httpStatusCode() >= 300) {
            if (_versionsStatusLabel) {
                _versionsStatusLabel->setText(tr("No previous versions available."));
                _versionsStatusLabel->show();
            }
            if (_versionsScroll) _versionsScroll->hide();
            return;
        }

        const QByteArray xml = job->reply()->readAll();
        const auto versions = parseVersionsMultistatus(xml, job->reply()->url());
        setVersions(versions, fileName, filePath, root, canRestore);
    });

    job->start();
}

void UDriveFileBrowser::setVersions(const QList<FileVersionItem> &versions, const QString &fileName, const QString &filePath, const QUrl &root, bool canRestore)
{
    if (_versionsJob) {
        _versionsJob->abort();
        _versionsJob.clear();
    }

    if (!_versionsListLayout) return;
    while (_versionsListLayout->count() > 1) {
        auto *item = _versionsListLayout->takeAt(0);
        if (item->widget()) delete item->widget();
        delete item;
    }

    if (versions.isEmpty()) {
        if (_versionsStatusLabel) {
            _versionsStatusLabel->setText(tr("No previous versions available."));
            _versionsStatusLabel->show();
        }
        if (_versionsScroll) _versionsScroll->hide();
        return;
    }

    if (_versionsStatusLabel) _versionsStatusLabel->hide();
    if (_versionsScroll) _versionsScroll->show();

    for (int i = 0; i < versions.size(); ++i) {
        const auto &ver = versions.at(i);
        const bool isLast = (i == versions.size() - 1);
        auto *widget = new VersionItemWidget(ver, isLast, canRestore,
            [this, ver, fileName, filePath, root] {
                confirmAndRestoreVersion(ver, fileName, filePath, root);
            },
            [this, ver, fileName] {
                downloadVersion(ver, fileName);
            },
            _versionsScroll->widget());
        _versionsListLayout->insertWidget(_versionsListLayout->count() - 1, widget);
        widget->show();
    }
}

QList<UDriveFileBrowser::FileVersionItem> UDriveFileBrowser::parseVersionsMultistatus(const QByteArray &xml, const QUrl &requestUrl) const
{
    QList<FileVersionItem> result;
    QXmlStreamReader reader(xml);
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement() && reader.name() == QLatin1String("response")) {
            QString href;
            qint64 size = -1;
            QDateTime modified;
            QString etag;
            bool isCollection = false;

            while (!reader.atEnd()) {
                reader.readNext();
                if (reader.isEndElement() && reader.name() == QLatin1String("response")) break;
                if (reader.isStartElement()) {
                    const QString tag = reader.name().toString();
                    if (tag == QLatin1String("href")) {
                        href = reader.readElementText();
                    } else if (tag == QLatin1String("collection")) {
                        isCollection = true;
                    } else if (tag == QLatin1String("getcontentlength") || tag == QLatin1String("size")) {
                        size = reader.readElementText().toLongLong();
                    } else if (tag == QLatin1String("getlastmodified")) {
                        const QString dtStr = reader.readElementText();
                        modified = QDateTime::fromString(dtStr, QStringLiteral("ddd, dd MMM yyyy HH:mm:ss 'GMT'"));
                        if (!modified.isValid()) modified = QDateTime::fromString(dtStr, Qt::ISODate);
                        if (modified.isValid()) modified.setTimeZone(QTimeZone::utc());
                    } else if (tag == QLatin1String("getetag")) {
                        etag = reader.readElementText();
                    }
                }
            }

            if (!isCollection && !href.isEmpty() && !href.endsWith(QLatin1Char('/'))) {
                FileVersionItem item;
                item.href = href;
                const QUrl resolved = requestUrl.resolved(QUrl(href));
                const bool validScheme = (resolved.scheme() == QStringLiteral("http") || resolved.scheme() == QStringLiteral("https"));
                const bool sameOrigin = validScheme && (resolved.scheme() == requestUrl.scheme())
                    && (resolved.host().compare(requestUrl.host(), Qt::CaseInsensitive) == 0)
                    && (resolved.port(0) == requestUrl.port(0));
                if (!sameOrigin) {
                    continue;
                }
                item.url = resolved;
                item.size = size;
                item.modified = modified;
                item.etag = etag;
                result.append(item);
            }
        }
    }

    std::sort(result.begin(), result.end(), [](const FileVersionItem &a, const FileVersionItem &b) {
        return a.modified > b.modified;
    });

    return result;
}

void UDriveFileBrowser::confirmAndRestoreVersion(const FileVersionItem &version, const QString &fileName, const QString &filePath, const QUrl &root)
{
    const QString timeStr = Utility::timeAgoInWords(version.modified);
    auto *dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("restoreConfirmDialog"));
    dialog->setWindowTitle(tr("Restore version"));
    dialog->setModal(true);
    dialog->setMinimumWidth(400);

    const bool dark = palette().color(QPalette::Window).lightness() < 128;
    const QString cardBg = dark ? QStringLiteral("#23272B") : QStringLiteral("#FFFFFF");
    const QString textPrimary = dark ? QStringLiteral("#EDEEF0") : QStringLiteral("#1F2328");
    const QString textSecondary = dark ? QStringLiteral("#939D97") : QStringLiteral("#656D76");

    dialog->setStyleSheet(QStringLiteral(
        "QDialog#restoreConfirmDialog { background-color: %1; border-radius: 12px; }"
        "QLabel { background: transparent; }"
    ).arg(cardBg));

    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(24, 24, 24, 20);
    layout->setSpacing(16);

    auto *title = new QLabel(tr("Restore “%1”?").arg(fileName), dialog);
    title->setObjectName(QStringLiteral("restoreConfirmTitle"));
    title->setTextFormat(Qt::PlainText);
    title->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 600; color: %1;").arg(textPrimary));
    title->setWordWrap(true);
    layout->addWidget(title);

    auto *desc = new QLabel(tr("The current file will be replaced with the version from %1.")
        .arg(timeStr), dialog);
    desc->setObjectName(QStringLiteral("restoreConfirmDesc"));
    desc->setTextFormat(Qt::PlainText);
    desc->setStyleSheet(QStringLiteral("font-size: 13px; color: %1;").arg(textSecondary));
    desc->setWordWrap(true);
    layout->addWidget(desc);

    auto *btnBox = new QDialogButtonBox(QDialogButtonBox::Cancel, dialog);
    auto *restoreBtn = new QPushButton(UDriveIcons::icon(UDriveIcons::Symbol::Restore), tr("Restore"), dialog);
    restoreBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: #00856B; color: white; border: none; border-radius: 6px; padding: 6px 16px; font-weight: 600; font-size: 13px; }"
        "QPushButton:hover { background: #00705A; }"
    ));
    btnBox->addButton(restoreBtn, QDialogButtonBox::AcceptRole);
    connect(btnBox, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(btnBox, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    layout->addWidget(btnBox);

    connect(dialog, &QDialog::accepted, this, [this, version, fileName, filePath, root] {
        const QUrl targetUrl = Utility::concatUrlPath(root.isEmpty() ? selectedRoot() : root, filePath);
        if (version.url.scheme() != targetUrl.scheme() || version.url.host() != targetUrl.host() || version.url.port(0) != targetUrl.port(0)) {
            showResult(tr("Security error: Version source and target do not match origin."), false);
            return;
        }
        QNetworkRequest req(version.url);
        req.setRawHeader("Destination", targetUrl.toEncoded());
        req.setRawHeader("Overwrite", "T");

        auto *job = new SimpleNetworkJob(_state->account(), version.url, QString(), "MOVE", nullptr, req, this);
        connect(job, &AbstractNetworkJob::finishedSignal, this, [this, job, fileName] {
            if (job->reply() && job->reply()->error() == QNetworkReply::NoError
                && job->httpStatusCode() >= 200 && job->httpStatusCode() < 300) {
                showResult(tr("Restored “%1” to selected version.").arg(fileName));
                refresh();
                if (_detailsIndex.isValid()) {
                    loadVersionsForItem(_detailsIndex);
                }
            } else {
                showResult(tr("Could not restore version."), false);
            }
        });
        job->start();
    });

    dialog->open();
}

void UDriveFileBrowser::downloadVersion(const FileVersionItem &version, const QString &fileName)
{
    const QString extension = QFileInfo(fileName).suffix();
    const QString base = QFileInfo(fileName).completeBaseName();
    const QString dateStr = version.modified.toLocalTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm"));
    const QString defaultName = extension.isEmpty()
        ? QStringLiteral("%1 (version %2)").arg(base, dateStr)
        : QStringLiteral("%1 (version %2).%3").arg(base, dateStr, extension);

    const QString targetPath = QFileDialog::getSaveFileName(this, tr("Download Version"), defaultName);
    if (targetPath.isEmpty()) return;

    setMessage(tr("Downloading version of “%1”…").arg(fileName));

    auto saveFile = std::make_shared<QSaveFile>(targetPath);
    auto writeError = std::make_shared<QString>();
    auto *job = new SimpleNetworkJob(_state->account(), version.url, QString(), "GET", nullptr, QNetworkRequest(), this);
    job->setTimeout(std::chrono::seconds(120));

    job->addNewReplyHook([saveFile, writeError](QNetworkReply *reply) {
        if (saveFile->isOpen()) {
            saveFile->cancelWriting();
        }
        writeError->clear();

        connect(reply, &QNetworkReply::readyRead, reply, [reply, saveFile, writeError] {
            if (!writeError->isEmpty()) return;
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status != 0 && (status < 200 || status >= 300)) return;
            if (!saveFile->isOpen()) {
                if (!saveFile->open(QIODevice::WriteOnly)) {
                    *writeError = saveFile->errorString();
                    reply->abort();
                    return;
                }
            }
            const QByteArray chunk = reply->readAll();
            if (saveFile->write(chunk) != chunk.size()) {
                *writeError = saveFile->errorString();
                reply->abort();
            }
        });
    });

    connect(job, &AbstractNetworkJob::finishedSignal, this, [this, job, targetPath, saveFile, writeError] {
        if (job->reply() && job->reply()->error() == QNetworkReply::NoError
            && job->httpStatusCode() >= 200 && job->httpStatusCode() < 300
            && writeError->isEmpty()) {
            if (!saveFile->isOpen()) {
                // If the downloaded file is 0 bytes, readyRead was never emitted; open it now so commit succeeds.
                if (!saveFile->open(QIODevice::WriteOnly)) {
                    *writeError = saveFile->errorString();
                }
            }
            if (writeError->isEmpty() && saveFile->commit()) {
                showResult(tr("Downloaded “%1”.").arg(QFileInfo(targetPath).fileName()));
                return;
            }
        }
        if (saveFile->isOpen()) saveFile->cancelWriting();
        showResult(writeError->isEmpty() ? tr("Could not download version.") : tr("Could not write downloaded file: %1").arg(*writeError), false);
    });
    job->start();
}

void UDriveFileBrowser::clearFileShares()
{
    auto *share = _detailsShares.data();
    _detailsShares.clear();
    if (!share) return;
    if (share->hasChanges() && sharedByMeOverview() && _detailsIndex.isValid()) {
        const auto source = _detailsIndex;
        QTimer::singleShot(0, this, [this, source] {
            if (source == _detailsIndex && !_detailsShares && sharedByMeOverview() && canBrowse() && isVisible()) refresh();
        });
    }
    _detailsPages->removeWidget(share);
    share->reject();
    share->deleteLater();
}

void UDriveFileBrowser::showFileShares()
{
    if (!_shareAction->isEnabled() || !canBrowse()) return;
    const auto item = selectedItem();
    if (!item.isValid()) return;
    _detailsRequested = true;
    updateFileDetails();
    if (!_detailsShares) {
        auto *share = new UDriveShareDialog(_state, item.data().toString(), item.data(DirectoryRole).toBool(),
            item.data(PathRole).toString(), item.data(LinkRole).toUrl(), item.data(SpaceRole).toString(), _detailsPages,
            UDriveShareDialog::Target::File, UDriveShareDialog::Presentation::Sidebar);
        _detailsShares = share;
        _detailsPages->addWidget(share);
        const QPersistentModelIndex source = _detailsIndex;
        connect(share, &UDriveShareDialog::accessListLoaded, this, [this, share, source](const QStringList &people, int publicLinks) {
            if (_detailsShares != share || source != _detailsIndex || !source.isValid()) return;
            _model->setData(source, people, SharePeopleRole);
            _model->setData(source, publicLinks, SharePublicLinksRole);
            _detailsPeople->setText(fileSharingSummary(people, publicLinks));
        });
        connect(share, &QDialog::finished, this, [this, share] {
            if (_detailsShares == share) showFileDetailsActions(false);
        });
    }
    _detailsPages->setCurrentWidget(_detailsShares);
    _detailsTitle->setText(tr("Shares"));
    _detailsTitle->show();
    _detailsBack->show();
    _detailsFileIcon->show();
    _fileDetails->setAccessibleName(tr("File shares"));
    static_cast<QScrollArea *>(_fileDetails)->verticalScrollBar()->setValue(0);
    _detailsShares->findChild<QLineEdit *>(QStringLiteral("shareUserInput"))->setFocus(Qt::OtherFocusReason);
}

void UDriveFileBrowser::updateFileDetails()
{
    if (!_fileDetails || !_detailsAction) return;
    const auto index = selectedItem();
    const bool available = index.isValid() && canBrowse() && _section != Section::Trash && !isWorkspacesOverview();
    _detailsAction->setVisible(_section != Section::Trash);
    _detailsAction->setEnabled(available);
    const QPersistentModelIndex source = available ? _proxy->mapToSource(index) : QModelIndex();
    if (source != _detailsIndex) {
        auto job = _shareItemJob; _shareItemJob.clear(); if (job) job->abort();
        if (_versionsJob) { _versionsJob->abort(); _versionsJob.clear(); }
        _detailsIndex = source;
        showFileDetailsActions(false);
        _detailsRequested = available && _section == Section::Shares;
    }
    _fileDetails->setVisible(available && _detailsRequested);
    if (_fileDetails->isVisible() && !_fileDetails->property("driveDetailsSized").toBool()) {
        if (auto *splitter = qobject_cast<QSplitter *>(_fileDetails->parentWidget())) {
            const int sidebarWidth = qBound(280, splitter->width() / 3, 360);
            splitter->setSizes({qMax(0, splitter->width() - sidebarWidth - splitter->handleWidth()), sidebarWidth});
            _fileDetails->setProperty("driveDetailsSized", true);
        }
    }
    if (!available) return;

    _detailsName->setText(index.data().toString());

    const bool isDir = index.data(DirectoryRole).toBool();
    const auto icon = index.data(Qt::DecorationRole).value<QIcon>();
    _detailsFileIcon->setPixmap(UDriveIcons::fileIcon(index.data().toString(), isDir).pixmap(24, 24));
    if (!isDir && index.data(ThumbnailRole).toBool() && !icon.isNull()) {
        const auto pixmap = icon.pixmap(QSize(256, 256));
        if (!pixmap.isNull()) {
            _detailsIcon->setPixmap(pixmap.scaled(QSize(200, 120), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        } else {
            _detailsIcon->setPixmap(UDriveIcons::fileIcon(index.data().toString(), isDir).pixmap(64, 64));
        }
    } else {
        _detailsIcon->setPixmap(UDriveIcons::fileIcon(index.data().toString(), isDir).pixmap(64, 64));
    }

    _detailsModified->setText(index.siblingAtColumn(2).data().toString());
    const QString sizeStr = index.siblingAtColumn(1).data().toString();
    if (!sizeStr.isEmpty() && sizeStr != QStringLiteral("—")) {
        _detailsSize->setText(sizeStr);
    } else {
        _detailsSize->setText(isDir ? tr("Folder") : QStringLiteral("—"));
    }

    QString loc = index.data(LocationRole).toString();
    if (loc.isEmpty()) {
        loc = _path.isEmpty() ? (_spaceName.isEmpty() ? tr("Personal") : _spaceName)
                              : (_spaceName.isEmpty() ? _path : _spaceName + QStringLiteral(" / ") + _path);
    }
    _detailsLocation->setText(loc);
    if (_detailsVersionsHeader) {
        _detailsVersionsHeader->setVisible(!isDir);
    }
    if (_detailsPages && _detailsPages->currentWidget() == _detailsVersionsPage) {
        if (isDir) {
            showFileDetailsOverview();
        } else {
            loadVersionsForItem(index);
        }
    }

    QString owner;
    if (_section == Section::Shares && _shareTab == ShareTab::WithMe) {
        const auto sharers = index.data(ShareSharersRole).toStringList();
        if (!sharers.isEmpty()) {
            owner = sharers.join(QStringLiteral(", "));
        }
    }
    if (owner.isEmpty() && _state && _state->account()) {
        owner = _state->account()->davDisplayName();
        if (owner.isEmpty()) {
            owner = _state->account()->davUser();
        }
    }
    if (owner.isEmpty()) {
        owner = tr("Personal");
    }
    if (_detailsOwner) {
        _detailsOwner->setText(owner);
    }

    const auto people = index.data(SharePeopleRole).toStringList();
    _detailsPeople->setText(fileSharingSummary(people, index.data(SharePublicLinksRole).toInt()));

    bool pdf = false;
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    pdf = !isDir && index.data().toString().endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive);
#endif
    const bool external = UDriveFilePreview::kindForName(index.data().toString()) == UDriveFilePreview::Kind::External;
    _detailsOpen->setText(isDir ? tr("Open folder") : pdf ? tr("Open PDF") : external ? tr("Open in app") : tr("Preview"));
    _detailsOpen->setIcon(isDir ? UDriveIcons::icon(UDriveIcons::Symbol::Folder) : UDriveIcons::icon(UDriveIcons::Symbol::Open));
    _detailsOpen->setEnabled(_openButton->isEnabled());
    _detailsOpen->setVisible(true);

    _detailsShare->setEnabled(_shareAction->isEnabled());
    _detailsDownload->setEnabled(_downloadButton->isEnabled());
    _detailsRename->setEnabled(_renameAction->isEnabled());
    _detailsWeb->setEnabled(_webAction->isEnabled());
    if (isVisible() && _tree->property("driveSharesOverview").toBool() && !index.data(ShareUnavailableRole).toBool()
        && !index.data(SharePermissionsLoadedRole).toBool() && !_shareItemJob && !_listing && !_report && !_operation)
        loadShareItemPermissions();
}

void UDriveFileBrowser::loadShareItemPermissions()
{
    const QPersistentModelIndex item = _detailsIndex;
    const QUrl root = item.data(RootRole).toUrl();
    const QString path = item.data(PathRole).toString();
    if (!canBrowse() || !item.isValid() || !validPath(path) || !root.userInfo().isEmpty() || !root.query().isEmpty()
        || !root.fragment().isEmpty() || !LocalDevelopment::allowsAuthenticatedUrl(root, _state->account()->url())) return;
    auto *job = new PropfindJob(_state->account(), root, path, PropfindJob::Depth::Zero, this);
    _shareItemJob = job;
    job->setTimeout(std::chrono::seconds(15));
    job->setProperties({"http://owncloud.org/ns:permissions", "getetag"});
    const QString target = withoutTrailingSlash(Utility::concatUrlPath(root, path).path());
    // Recipient grants describe other users. Only the authenticated DAV response
    // for this exact item may enable a write action in the shared-items overview.
    auto result = std::make_shared<QMap<QString, QString>>();
    connect(job, &PropfindJob::directoryListingIterated, this, [this, job, item, target, result](const QString &href, const QMap<QString, QString> &properties) {
        if (_shareItemJob == job && item == _detailsIndex && withoutTrailingSlash(href) == target) *result = properties;
    });
    const auto complete = [this, job, item, result](bool success) {
        if (_shareItemJob != job) return;
        _shareItemJob.clear();
        if (!canBrowse() || !item.isValid() || item != _detailsIndex) return;
        _model->setData(item, true, SharePermissionsLoadedRole);
        if (success && result->contains(QStringLiteral("permissions")) && !result->value(QStringLiteral("permissions")).isEmpty()) {
            _model->setData(item, result->value(QStringLiteral("permissions")), PermissionsRole);
            if (!result->value(QStringLiteral("getetag")).isEmpty()) _model->setData(item, result->value(QStringLiteral("getetag")), EtagRole);
        }
        updateActions();
    };
    connect(job, &PropfindJob::finishedWithoutError, this, [complete] { complete(true); });
    connect(job, &PropfindJob::finishedWithError, this, [complete] { complete(false); });
    job->start();
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
    if (save) {
        ConfigFile::settingsWithGroup(QStringLiteral("UDriveFileBrowser"))->setValue(QStringLiteral("sortMode"), action->data());
        UDriveToast::preferenceSaved(this);
    }
    const auto selected = selectedItem();
    updateEmptyState();
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

void UDriveFileBrowser::showHome()
{
    showPersonal();
    _search->clear();
    navigateToBreadcrumb(QString(), _root);
}

void UDriveFileBrowser::showWorkspaces()
{
    if (_workspaceLayout) {
        showWorkspacesOverview();
    } else {
        setSection(Section::Workspaces);
    }
}

void UDriveFileBrowser::showWorkspacesOverview()
{
    closePdf();
    _section = Section::Workspaces;
    _workspaceOverview = true;
    cancelListing();
    cancelDownload();
    if (_uploading || _emptyingTrash) cancelOperation();
    _model->removeRows(0, _model->rowCount());
    _spaceId.clear(); _root.clear(); _webUrl.clear(); _driveId.clear();
    _path.clear();
    _search->clear();
    updateSpaces();
    Q_EMIT sectionChanged(Section::Workspaces);
    Q_EMIT creationAvailabilityChanged();
}

void UDriveFileBrowser::openWorkspace(const QString &spaceId)
{
    if (!canBrowse()) return;
    auto *manager = _state->account()->spacesManager();
    GraphApi::Space *space = manager ? manager->space(spaceId) : nullptr;
    if (!space && manager) {
        for (auto *s : manager->spaces()) {
            if (s->id() == spaceId || s->driveId() == spaceId) {
                space = s;
                break;
            }
        }
    }
    if (!space) return;

    closePdf();
    cancelListing();
    cancelDownload();
    if (_uploading || _emptyingTrash) cancelOperation();

    _section = Section::Workspaces;
    _workspaceOverview = false;
    _spaceId = space->id();
    _root = space->webDavUrl();
    _root.setPath(withoutTrailingSlash(_root.path()));
    _webUrl = space->webUrl();
    _driveId = space->driveId();
    _spaceName = space->displayName();
    _path.clear();
    _search->clear();

    _fileViews->setCurrentWidget(_viewMode == ViewMode::Tiles ? static_cast<QWidget *>(_tiles) : _tree);
    _message->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    updateBreadcrumbs();
    updateSpaces();
    refresh();
    Q_EMIT sectionChanged(Section::Workspaces);
    Q_EMIT creationAvailabilityChanged();
}

bool UDriveFileBrowser::createNewWorkspace()
{
    if (!canBrowse() || !_workspaceActions->canCreate() || _workspaceActions->busy()) return false;
    const QPointer<UDriveFileBrowser> guard(this);
    bool ok = false;
    const QString name = UDriveInputDialog::getText(this, tr("New Workspace"), tr("Workspace name:"), QString(), tr("Create Workspace"), &ok);
    if (!guard || !ok || !canBrowse()) return false;
    return _workspaceActions->create(name.trimmed());
}

QModelIndexList UDriveFileBrowser::selectedWorkspaces() const
{
    QModelIndexList rows;
    for (const auto &index : _workspacesView->selectionModel()->selectedIndexes())
        if (index.column() == 0) rows.append(index);
    return rows;
}

void UDriveFileBrowser::setWorkspaceTableView(bool table, bool save)
{
    QList<QPersistentModelIndex> selected;
    for (const auto &index : selectedWorkspaces()) selected.append(QPersistentModelIndex(index));
    _workspaceTableMode = table;
    _workspacesProxy->sort(table ? _workspacesTable->header()->sortIndicatorSection() : 0,
        table ? _workspacesTable->header()->sortIndicatorOrder() : _workspaceSort->currentIndex() == 1 ? Qt::DescendingOrder : Qt::AscendingOrder);
    _workspaceViews->setCurrentWidget(table ? static_cast<QWidget *>(_workspacesTable) : _workspacesView);
    _workspaceSelectionControls->setVisible(!table);
    _workspaceControls->findChild<QFrame *>(QStringLiteral("driveWorkspaceRule"))->setVisible(!table);
    _workspaceViewButton->setIcon(UDriveIcons::icon(table ? UDriveIcons::Symbol::List : UDriveIcons::Symbol::Grid));
    for (auto *action : _workspaceViewButton->menu()->actions()) action->setChecked(action->data().toBool() == table);
    auto *selection = _workspacesView->selectionModel();
    selection->clearSelection();
    for (const auto &index : selected) selection->select(index, QItemSelectionModel::Select | QItemSelectionModel::Rows);
    if (save) {
        ConfigFile::settingsWithGroup(QStringLiteral("UDriveFileBrowser"))->setValue(QStringLiteral("workspaceViewMode"), table ? QStringLiteral("table") : QStringLiteral("tiles"));
        UDriveToast::preferenceSaved(this);
    }
    updateWorkspaceCardHeight();
    if (isWorkspacesOverview()) activeView()->setFocus();
}

void UDriveFileBrowser::appendWorkspace(const OpenAPI::OAIDrive &drive, const QString &name)
{
    const auto root = drive.getRoot();
    const bool disabled = root.getDeleted().getState() == QStringLiteral("trashed");
    auto *item = new QStandardItem(name);
    item->setData(root.getId(), SpaceIdRole);
    item->setData(drive.getId(), SpaceDriveIdRole);
    item->setData(QUrl(root.getWebDavUrl()), SpaceWebDavUrlRole);
    item->setData(QUrl(root.getWebUrl().isEmpty() ? drive.getWebUrl() : root.getWebUrl()), SpaceWebUrlRole);
    item->setData(disabled, SpaceDisabledRole);
    item->setData(drive.getDescription(), SpaceDescriptionRole);
    item->setData(QVariant::fromValue(drive), SpaceDriveRole);
    if (auto *space = _state->account()->spacesManager()->space(drive.getId())) {
        item->setData(space->image()->image(), Qt::DecorationRole);
        connect(space, &GraphApi::Space::imageChanged, this, &UDriveFileBrowser::updateWorkspaceImages, Qt::UniqueConnection);
    }
    item->setData(name, SortRole);
    item->setToolTip(disabled ? tr("Disabled") : drive.getDescription());
    QStringList managers;
    QSet<QString> members;
    for (const auto &permission : root.getPermissions()) {
        const auto roles = permission.getRoles();
        const bool manager = roles.contains(QStringLiteral("manager")) || roles.contains(QStringLiteral("312c0871-5ef7-4b3a-85b6-0e4074c64049"));
        for (const auto &identity : permission.getGrantedToIdentities()) {
            const auto user = identity.is_user_Set() ? identity.getUser() : identity.getGroup();
            if (user.getId().isEmpty()) continue;
            members.insert((identity.is_user_Set() ? QStringLiteral("u:") : QStringLiteral("g:")) + user.getId());
            if (manager) managers.append(user.getDisplayName().isEmpty() ? user.getId() : user.getDisplayName());
        }
    }
    managers.removeDuplicates(); managers.sort(Qt::CaseInsensitive);
    auto *manager = new QStandardItem(managers.isEmpty() ? QStringLiteral("—") : managers.join(QStringLiteral(", ")));
    manager->setData(manager->text(), SortRole); manager->setToolTip(manager->text());
    const bool knownMembers = root.is_permissions_Set() && root.is_permissions_Valid();
    auto *memberCount = new QStandardItem(knownMembers ? QString::number(members.size()) : QStringLiteral("—"));
    if (knownMembers) memberCount->setData(members.size(), SortRole);
    const auto quota = drive.getQuota();
    const qint64 remaining = quota.getRemaining();
    const bool knownRemaining = quota.is_remaining_Valid() && remaining >= 0;
    auto *remainingQuota = new QStandardItem(knownRemaining ? QLocale().formattedDataSize(remaining, remaining >= 1000000 ? 1 : 0, QLocale::DataSizeSIFormat) : QStringLiteral("—"));
    if (knownRemaining) remainingQuota->setData(remaining, SortRole);
    const auto modified = drive.getLastModifiedDateTime();
    auto *modifiedDate = new QStandardItem(modified.isValid() ? QLocale().toString(modified.toLocalTime(), QLocale::ShortFormat) : QStringLiteral("—"));
    if (modified.isValid()) modifiedDate->setData(modified.toMSecsSinceEpoch(), SortRole);
    _workspacesModel->appendRow({item, manager, memberCount, remainingQuota, modifiedDate, new QStandardItem});
}

void UDriveFileBrowser::showWorkspaceMembers(const QModelIndex &index)
{
    const auto name = index.siblingAtColumn(0);
    const QString driveId = name.data(SpaceDriveIdRole).toString();
    if (!canBrowse() || !_workspaceActions->can(QStringLiteral("members"), name.data(SpaceDriveRole).value<OpenAPI::OAIDrive>()) || !validName(driveId)) return;
    auto *dialog = new UDriveShareDialog(_state, name.data().toString(), true, QString(), name.data(SpaceWebUrlRole).toUrl(), driveId, this, UDriveShareDialog::Target::Workspace);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
}

void UDriveFileBrowser::updateWorkspaceImages()
{
    if (!canBrowse()) return;
    for (int row = 0; row < _workspacesModel->rowCount(); ++row) {
        auto *item = _workspacesModel->item(row, 0);
        if (auto *space = _state->account()->spacesManager()->space(item->data(SpaceDriveIdRole).toString()))
            item->setData(space->image()->image(), Qt::DecorationRole);
    }
}

void UDriveFileBrowser::editWorkspaceDescription(const OpenAPI::OAIDrive &drive, const QString &text)
{
    if (!canBrowse() || !_workspaceActions->can(QStringLiteral("description"), drive)) return;
    auto *dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("driveWorkspaceDescriptionDialog"));
    dialog->setProperty("udriveWorkspaceDialog", true); dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Edit description")); dialog->resize(640, 440); styleWorkspaceDialog(dialog);
    auto *layout = new QVBoxLayout(dialog); layout->setContentsMargins(24, 24, 24, 20); layout->setSpacing(16);
    auto *title = new QLabel(drive.getName(), dialog); title->setWordWrap(true); title->setTextFormat(Qt::PlainText); layout->addWidget(title);
    auto *editor = new QPlainTextEdit(text, dialog); editor->setObjectName(QStringLiteral("driveWorkspaceDescriptionText"));
    editor->setPlaceholderText(tr("Describe this workspace using Markdown.")); layout->addWidget(editor);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, dialog);
    buttons->button(QDialogButtonBox::Save)->setDefault(true); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    auto updateSave = [this, editor, buttons, drive] {
        buttons->button(QDialogButtonBox::Save)->setEnabled(_workspaceActions->can(QStringLiteral("description"), drive)
            && editor->toPlainText().toUtf8().size() <= 1024 * 1024);
    };
    connect(editor, &QPlainTextEdit::textChanged, dialog, updateSave);
    connect(_workspaceActions, &UDriveWorkspaceActions::permissionsChanged, dialog, updateSave);
    updateSave();
    connect(buttons, &QDialogButtonBox::accepted, this, [this, dialog, editor, drive] {
        if (canBrowse() && _workspaceActions->writeSpecial(drive, QStringLiteral("readme"), editor->toPlainText().toUtf8())) {
            dialog->accept(); setMessage(tr("Working…"));
        }
    });
    dialog->open();
}

void UDriveFileBrowser::workspaceAction(const QString &action, const QList<OpenAPI::OAIDrive> &drives)
{
    if (!canBrowse() || _workspaceActions->busy() || drives.isEmpty()) return;
    for (const auto &drive : drives) if (!_workspaceActions->can(action, drive)) return;
    const auto drive = drives.first();
    const bool active = std::all_of(drives.cbegin(), drives.cend(), [](const auto &d) { return d.getRoot().getDeleted().getState() != QStringLiteral("trashed"); });
    if (!active && action != QStringLiteral("restore") && action != QStringLiteral("details") && action != QStringLiteral("web") && action != QStringLiteral("link")) return;
    const QPointer<UDriveFileBrowser> guard(this);
    if (action == QStringLiteral("members")) {
        for (int row = 0; row < _workspacesModel->rowCount(); ++row) {
            const auto index = _workspacesModel->index(row, 0);
            if (index.data(SpaceDriveIdRole).toString() == drive.getId()) { showWorkspaceMembers(_workspacesProxy->mapFromSource(index)); break; }
        }
    } else if (action == QStringLiteral("open")) {
        openWorkspace(drive.getRoot().getId());
    } else if (action == QStringLiteral("trash")) {
        showTrash();
        const int index = _spaces->findData(drive.getRoot().getId());
        if (index >= 0) _spaces->setCurrentIndex(index);
    } else if (action == QStringLiteral("web") || action == QStringLiteral("link")) {
        const QUrl url(drive.getWebUrl().isEmpty() ? drive.getRoot().getWebUrl() : drive.getWebUrl());
        if (url.scheme() != QStringLiteral("https") && url.scheme() != QStringLiteral("http")) return;
        if (action == QStringLiteral("web")) Utility::openBrowser(url, this);
        else { QApplication::clipboard()->setText(url.toString()); showResult(tr("Link copied to clipboard.")); }
    } else if (action == QStringLiteral("details")) {
        auto *dialog = new QDialog(this); dialog->setWindowTitle(tr("Details")); dialog->resize(500, 350); styleWorkspaceDialog(dialog);
        dialog->setObjectName(QStringLiteral("driveWorkspaceDetailsDialog"));
        dialog->setProperty("udriveWorkspaceDialog", true); dialog->setAttribute(Qt::WA_DeleteOnClose);
        auto *layout = new QFormLayout(dialog); layout->setContentsMargins(24, 24, 24, 20); layout->setVerticalSpacing(16);
        layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        auto field = [layout, dialog](const QString &name, const QString &value) {
            auto *label = new QLabel(value.isEmpty() ? QStringLiteral("—") : value, dialog); label->setTextFormat(Qt::PlainText); label->setWordWrap(true);
            label->setTextInteractionFlags(Qt::TextSelectableByMouse); layout->addRow(name, label);
        };
        field(tr("Name"), drive.getName()); field(tr("Subtitle"), drive.getDescription());
        field(tr("Status"), active ? tr("Active") : tr("Disabled"));
        for (int row = 0; row < _workspacesModel->rowCount(); ++row) if (_workspacesModel->item(row, 0)->data(SpaceDriveIdRole).toString() == drive.getId()) {
            for (int column : {1, 2, 3, 4}) field(_workspacesModel->headerData(column, Qt::Horizontal).toString(), _workspacesModel->item(row, column)->text());
        }
        const auto quota = drive.getQuota();
        if (quota.is_total_Valid() && quota.getTotal() >= 0) field(tr("Total quota"), QLocale().formattedDataSize(quota.getTotal(), 1, QLocale::DataSizeSIFormat));
        field(tr("Workspace ID"), drive.getId());
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog); layout->addRow(buttons);
        connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject); dialog->open();
    } else if (action == QStringLiteral("description")) {
        _workspaceActions->readDescription(drive);
    } else if (action == QStringLiteral("rename") || action == QStringLiteral("subtitle") || action == QStringLiteral("quota")) {
        bool ok = false;
        const bool quota = action == QStringLiteral("quota"), rename = action == QStringLiteral("rename");
        const QString initial = quota ? QLocale().toString(drive.getQuota().getTotal() / 1000000000.0, 'f', 3) : rename ? drive.getName() : drive.getDescription();
        const QString value = UDriveInputDialog::getText(this, quota ? tr("Edit quota") : rename ? tr("Rename") : tr("Edit subtitle"),
            quota ? tr("Quota in GB (minimum 0.001):") : rename ? tr("Workspace name:") : tr("Subtitle:"), initial, tr("Save"), &ok, !rename && !quota);
        if (!guard || !ok || !canBrowse()) return;
        QJsonObject payload;
        if (quota) {
            bool valid = false; const double gb = QLocale().toDouble(value, &valid);
            if (!valid || !std::isfinite(gb) || gb < 0.001 || gb > 9000000) { showResult(tr("Enter a quota between 0.001 and 9000000 GB."), false); return; }
            payload.insert(QStringLiteral("quota"), QJsonObject{{QStringLiteral("total"), qRound64(gb * 1000000000.0)}});
        } else if (rename) {
            if (!validName(value.trimmed())) { showResult(tr("Enter a valid workspace name."), false); return; }
            payload.insert(QStringLiteral("name"), value.trimmed());
        } else payload.insert(QStringLiteral("description"), value);
        if (_workspaceActions->patch(drives, payload)) setMessage(tr("Working…"));
    } else if (action == QStringLiteral("disable") || action == QStringLiteral("restore")) {
        const bool restore = action == QStringLiteral("restore");
        if (!restore) {
            UDriveInputDialog confirmation(tr("Disable workspaces"), tr("Disable %1 workspace(s)? Their files will become unavailable until restored.").arg(drives.size()), QString(), tr("Disable"), this, true);
            confirmation.findChild<QLineEdit *>()->hide();
            if (confirmation.exec() != QDialog::Accepted || !guard || !canBrowse()) return;
        }
        if (_workspaceActions->disable(drives, restore)) setMessage(tr("Working…"));
    } else if (action == QStringLiteral("duplicate")) {
        QSet<QString> existing;
        for (auto *space : _state->account()->spacesManager()->spaces()) existing.insert(space->displayName());
        QStringList names;
        for (const auto &source : drives) {
            const QString base = tr("%1 (copy)").arg(source.getName()); QString name = base; int suffix = 2;
            while (existing.contains(name)) name = base + QStringLiteral(" ") + QString::number(suffix++);
            existing.insert(name); names.append(name);
        }
        if (_workspaceActions->duplicate(drives, names)) setMessage(tr("Working…"));
    } else if (action == QStringLiteral("download")) {
        const QString destination = QFileDialog::getSaveFileName(this, tr("Download workspaces"), drives.size() == 1 ? drive.getName() + QStringLiteral(".zip") : QStringLiteral("workspaces.zip"), tr("ZIP archives (*.zip)"));
        if (guard && canBrowse() && _workspaceActions->download(drives, destination)) setMessage(tr("Working…"));
    } else if (action == QStringLiteral("image") || action == QStringLiteral("icon")) {
        QImage image;
        if (action == QStringLiteral("image")) {
            const QString file = QFileDialog::getOpenFileName(this, tr("Edit image"), {}, tr("Images (*.png *.jpg *.jpeg *.webp)"));
            if (!guard || file.isEmpty() || !canBrowse()) return;
            QImageReader reader(file); const QSize size = reader.size();
            if (!size.isValid() || qint64(size.width()) * size.height() > 16000000 || QFileInfo(file).size() > 8 * 1024 * 1024
                || !QList<QByteArray>{"png", "jpeg", "webp"}.contains(reader.format().toLower())) {
                showResult(tr("Choose a PNG, JPEG, or WebP image up to 8 MiB and 16 megapixels."), false); return;
            }
            reader.setAutoTransform(true); image = reader.read();
            if (!image.isNull()) image = image.scaled(1024, 1024, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        } else {
            bool ok = false;
            const QString emoji = UDriveInputDialog::getText(this, tr("Set icon"), tr("Enter one emoji:"), QString::fromUtf8("📁"), tr("Save"), &ok);
            if (!guard || !ok || !canBrowse()) return;
            QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, emoji);
            if (emoji.size() > 32 || finder.toNextBoundary() != emoji.size()) { showResult(tr("Enter one emoji."), false); return; }
            image = QImage(128, 128, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent);
            QPainter painter(&image); QFont font = qApp->font();
#ifdef Q_OS_MACOS
            font.setFamily(QStringLiteral("Apple Color Emoji"));
#endif
            font.setPixelSize(92); painter.setFont(font); painter.drawText(image.rect(), Qt::AlignCenter, emoji);
        }
        if (image.isNull()) { showResult(tr("Could not read this image."), false); return; }
        QByteArray png; QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly);
        if (image.save(&buffer, "PNG") && _workspaceActions->writeSpecial(drive, QStringLiteral("image"), png)) setMessage(tr("Working…"));
    }
}

void UDriveFileBrowser::showWorkspaceCardMenu(const QModelIndex &index)
{
    if (!canBrowse() || !index.isValid()) return;
    const auto drive = index.siblingAtColumn(0).data(SpaceDriveRole).value<OpenAPI::OAIDrive>();
    if (!validName(drive.getId())) return;
    auto *menu = createFileMenu(this);
    menu->setObjectName(QStringLiteral("driveWorkspaceActionsMenu")); menu->setAttribute(Qt::WA_DeleteOnClose);
    const bool active = !index.data(SpaceDisabledRole).toBool();
    const bool idle = !_workspaceActions->busy();
    auto add = [this, menu, drive, idle](const QString &label, const QString &key, UDriveIcons::Symbol icon, bool enabled = true) {
        auto *action = menu->addAction(UDriveIcons::icon(icon), label);
        action->setObjectName(key == QStringLiteral("members") ? QStringLiteral("driveWorkspaceMembers") : QStringLiteral("driveWorkspace_") + key);
        action->setIconVisibleInMenu(true); action->setEnabled(idle && enabled && _workspaceActions->can(key, drive));
        connect(_workspaceActions, &UDriveWorkspaceActions::permissionsChanged, action, [this, action, key, drive, enabled] {
            action->setEnabled(!_workspaceActions->busy() && enabled && _workspaceActions->can(key, drive));
        });
        connect(action, &QAction::triggered, this, [this, key, drive] { workspaceAction(key, {drive}); });
    };
    add(tr("Members"), QStringLiteral("members"), UDriveIcons::Symbol::Members, active);
    add(tr("Download"), QStringLiteral("download"), UDriveIcons::Symbol::Download, active && _workspaceActions->archiveAvailable());
    menu->addSeparator();
    add(tr("Rename"), QStringLiteral("rename"), UDriveIcons::Symbol::Rename, active);
    add(tr("Duplicate"), QStringLiteral("duplicate"), UDriveIcons::Symbol::Copy, active);
    add(tr("Edit description"), QStringLiteral("description"), UDriveIcons::Symbol::File, active);
    add(tr("Edit subtitle"), QStringLiteral("subtitle"), UDriveIcons::Symbol::Rename, active);
    add(tr("Edit image"), QStringLiteral("image"), UDriveIcons::Symbol::Image, active);
    add(tr("Set icon"), QStringLiteral("icon"), UDriveIcons::Symbol::Grid, active);
    menu->addSeparator();
    add(tr("Edit quota"), QStringLiteral("quota"), UDriveIcons::Symbol::Cloud, active);
    add(active ? tr("Disable") : tr("Restore workspace"), active ? QStringLiteral("disable") : QStringLiteral("restore"), active ? UDriveIcons::Symbol::Disable : UDriveIcons::Symbol::Restore);
    add(tr("Show deleted files"), QStringLiteral("trash"), UDriveIcons::Symbol::Trash, active);
    menu->addSeparator();
    add(tr("Details"), QStringLiteral("details"), UDriveIcons::Symbol::Help);
    add(tr("Open"), QStringLiteral("open"), UDriveIcons::Symbol::Open, active);
    add(tr("Open on Web"), QStringLiteral("web"), UDriveIcons::Symbol::Online);
    add(tr("Copy web link"), QStringLiteral("link"), UDriveIcons::Symbol::Link);
    menu->popup(QCursor::pos());
}

void UDriveFileBrowser::showShares()
{
    bool hasIncoming = false;
    if (_state && _state->account()) {
        for (auto *space : _state->account()->spacesManager()->spaces()) {
            if (space->sortPriority() == 50 && !space->disabled()) {
                hasIncoming = true;
                break;
            }
        }
    }
    _shareTab = hasIncoming ? ShareTab::WithMe : ShareTab::WithOthers;
    _sharedByMe = (_shareTab == ShareTab::WithOthers || _shareTab == ShareTab::ViaLink);
    if (_sharedWithMeButton) _sharedWithMeButton->setChecked(_shareTab == ShareTab::WithMe);
    if (_sharedWithOthersButton) _sharedWithOthersButton->setChecked(_shareTab == ShareTab::WithOthers);
    if (_sharedViaLinkButton) _sharedViaLinkButton->setChecked(_shareTab == ShareTab::ViaLink);
    if (_shareScope) {
        _shareScope->setText(_shareTab == ShareTab::WithMe ? tr("Shared with me")
            : _shareTab == ShareTab::WithOthers ? tr("Shared by me") : tr("Shared via link"));
        if (auto *m = _shareScope->menu()) {
            if (auto *byMe = m->findChild<QAction *>(QStringLiteral("driveSharedByMe"))) byMe->setChecked(_shareTab == ShareTab::WithOthers);
            if (auto *withMe = m->findChild<QAction *>(QStringLiteral("driveSharedWithMe"))) withMe->setChecked(_shareTab == ShareTab::WithMe);
        }
    }
    setSection(Section::Shares);
}

void UDriveFileBrowser::setShareTab(ShareTab tab)
{
    if (tab != _shareTab) {
        _includeHiddenShares = false;
        if (_sharedByFilter) _sharedByFilter->setCurrentIndex(0);
    }
    _shareTab = tab;
    _sharedByMe = (tab == ShareTab::WithOthers || tab == ShareTab::ViaLink);
    if (_sharedWithMeButton) _sharedWithMeButton->setChecked(tab == ShareTab::WithMe);
    if (_sharedWithOthersButton) _sharedWithOthersButton->setChecked(tab == ShareTab::WithOthers);
    if (_sharedViaLinkButton) _sharedViaLinkButton->setChecked(tab == ShareTab::ViaLink);
    if (_shareScope) {
        _shareScope->setText(tab == ShareTab::WithMe ? tr("Shared with me")
            : tab == ShareTab::WithOthers ? tr("Shared by me") : tr("Shared via link"));
        if (auto *m = _shareScope->menu()) {
            if (auto *byMe = m->findChild<QAction *>(QStringLiteral("driveSharedByMe"))) byMe->setChecked(tab == ShareTab::WithOthers);
            if (auto *withMe = m->findChild<QAction *>(QStringLiteral("driveSharedWithMe"))) withMe->setChecked(tab == ShareTab::WithMe);
        }
    }
    if (_section != Section::Shares) return;
    cancelListing();
    cancelDownload();
    _spaceId.clear();
    _root.clear();
    _path.clear();
    updateSpaces();
    refresh();
}

void UDriveFileBrowser::rebuildSharerFilter()
{
    const QString selected = _sharedByFilter->currentData().toString();
    const QSignalBlocker blocker(_sharedByFilter);
    _sharedByFilter->clear();
    _sharedByFilter->addItem(tr("Anyone"), QString());
    auto keys = _shareSharers.keys();
    std::sort(keys.begin(), keys.end(), [this](const QString &a, const QString &b) {
        const int order = _shareSharers.value(a).localeAwareCompare(_shareSharers.value(b));
        return order == 0 ? a < b : order < 0;
    });
    for (const auto &key : keys) _sharedByFilter->addItem(_shareSharers.value(key), key);
    _sharedByFilter->setCurrentIndex(qMax(0, _sharedByFilter->findData(selected)));
}

void UDriveFileBrowser::updateShareFilters()
{
    if (!_sharedByFilter) return;
    const bool overview = _section == Section::Shares && _path.isEmpty();
    const bool incoming = overview && _shareTab == ShareTab::WithMe;
    const bool idle = canBrowse() && !_listing && !_report && !_operation;
    const bool metadata = overview && _shareMetadataAvailable;
    if (!incoming) _includeHiddenShares = false;
    _sharesFilterShares->setVisible(incoming);
    _sharesFilterHidden->setVisible(incoming);
    _sharesFilterShares->setChecked(!_includeHiddenShares);
    _sharesFilterHidden->setChecked(_includeHiddenShares);
    _sharesFilterShares->setEnabled(idle && metadata);
    _sharesFilterHidden->setEnabled(idle && metadata);
    _sharesFilterHidden->setToolTip(metadata ? QString() : tr("Share metadata is not available from this server."));
    _sharedByFilter->setEnabled(idle && metadata && _sharedByFilter->count() > 1);
    _sharesSettingsButton->setEnabled(idle && overview);
    _shareTypeFilter->setEnabled(idle);
    static_cast<FilesProxy *>(_proxy)->setShareFilter(metadata, metadata && _includeHiddenShares,
        metadata ? _sharedByFilter->currentData().toString() : QString());
    updateSelectionState();
}

void UDriveFileBrowser::resetShareFilters()
{
    _includeHiddenShares = false;
    _sharedByFilter->setCurrentIndex(0);
    _shareTypeFilter->setCurrentIndex(0);
    _sharesSearch->clear();
    updateShareFilters();
}

void UDriveFileBrowser::updateEmptyState()
{
    if (!_emptyState) return;
    const bool workspaces = isWorkspacesOverview();
    const bool ready = workspaces ? canBrowse() && _state->account()->spacesManager()->isReady() : _emptyStateReady;
    const int count = workspaces ? _workspacesProxy->rowCount() : _proxy->rowCount();
    const bool empty = canBrowse() && ready && count == 0 && !_listing && !_report && !_operation
        && (!workspaces || !_workspaceActions->busy());
    if (!empty) {
        _fileViews->setCurrentWidget(workspaces ? _workspacePage
            : _viewMode == ViewMode::Tiles ? static_cast<QWidget *>(_tiles) : _tree);
        return;
    }

    UDriveIcons::Symbol symbol = UDriveIcons::Symbol::Folder;
    QString text;
    switch (_section) {
    case Section::Personal: text = tr("This folder is empty."); break;
    case Section::Workspaces:
        symbol = UDriveIcons::Symbol::Grid;
        text = tr("No workspaces found.");
        if (!workspaces) { symbol = UDriveIcons::Symbol::Folder; text = tr("This folder is empty."); }
        break;
    case Section::Shares:
        symbol = _shareTab == ShareTab::ViaLink ? UDriveIcons::Symbol::Link
            : _shareTab == ShareTab::WithOthers ? UDriveIcons::Symbol::ShareOut : UDriveIcons::Symbol::Share;
        text = tr("No shares");
        break;
    case Section::Favorites: symbol = UDriveIcons::Symbol::Star; text = tr("No Favorites"); break;
    case Section::Trash: symbol = UDriveIcons::Symbol::Trash; text = tr("No deleted files"); break;
    }
    if (workspaces ? _workspacesModel->rowCount() > 0 : _model->rowCount() > 0) {
        symbol = UDriveIcons::Symbol::Search;
        text = tr("No results found.");
    }
    _emptyIcon->setPixmap(UDriveIcons::icon(symbol).pixmap(72, 72));
    _emptyText->setText(text);
    _fileViews->setCurrentWidget(_emptyState);
}
void UDriveFileBrowser::showFavorites() { setSection(Section::Favorites); }
void UDriveFileBrowser::showTrash() { setSection(Section::Trash); }

void UDriveFileBrowser::setSection(Section section)
{
    closePdf();
    const bool changed = _section != section;
    _section = section;
    if (_section != Section::Workspaces) {
        _workspaceOverview = false;
    }
    if (changed) {
        if (_proxy) {
            const bool isShares = (_section == Section::Shares);
            static_cast<FilesProxy *>(_proxy)->setSharesSection(isShares);
            if (!isShares) {
                if (_shareTypeFilter) {
                    const QSignalBlocker blocker(_shareTypeFilter);
                    _shareTypeFilter->setCurrentIndex(0);
                }
                if (_sharesSearch) {
                    const QSignalBlocker blocker(_sharesSearch);
                    _sharesSearch->clear();
                }
                static_cast<FilesProxy *>(_proxy)->setTypeFilter(0);
                _proxy->setFilterFixedString(_search ? _search->text() : QString());
            } else {
                _proxy->setFilterFixedString(_sharesSearch ? _sharesSearch->text() : QString());
            }
        }
        _workspaceActions->cancel();
        for (auto *dialog : findChildren<QDialog *>()) if (dialog->property("udriveWorkspaceDialog").toBool()) dialog->close();
        cancelListing(); cancelDownload();
        if (_uploading || _emptyingTrash) cancelOperation();
        _spaceId.clear();
        _path.clear();
        updateSpaces();
        Q_EMIT sectionChanged(section);
        Q_EMIT creationAvailabilityChanged();
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
    const bool insideWorkspace = (_section == Section::Workspaces && !_workspaceOverview);
    _breadcrumbs->setVisible(_section != Section::Favorites && _section != Section::Trash && !sharedByMeOverview() && !isWorkspacesOverview() && (!_workspaceLayout || !_path.isEmpty() || insideWorkspace));

    auto separator = [this] {
        auto *label = new QLabel(QStringLiteral("›"), _breadcrumbs);
        label->setObjectName(QStringLiteral("driveBreadcrumbSeparator"));
        label->setTextFormat(Qt::PlainText);
        label->setAlignment(Qt::AlignCenter);
        label->setFixedWidth(16);
        label->show();
        _breadcrumbLayout->addWidget(label);
    };

    if (insideWorkspace) {
        auto *workspacesBtn = new QPushButton(_breadcrumbs);
        workspacesBtn->setObjectName(QStringLiteral("driveBreadcrumbWorkspaces"));
        workspacesBtn->setProperty("udriveBreadcrumb", true);
        workspacesBtn->setText(tr("Workspaces"));
        workspacesBtn->setToolTip(tr("All workspaces"));
        workspacesBtn->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Grid));
        workspacesBtn->setIconSize(QSize(16, 16));
        connect(workspacesBtn, &QPushButton::clicked, this, [this] { showWorkspacesOverview(); });
        workspacesBtn->show();
        _breadcrumbLayout->addWidget(workspacesBtn);
        separator();
    }

    const QString rootName = _spaceName.isEmpty() ? _heading->text() : _spaceName;
    _rootButton->setText(_rootButton->fontMetrics().elidedText(rootName, Qt::ElideMiddle, 140).replace(QLatin1Char('&'), QStringLiteral("&&")));
    _rootButton->setToolTip(rootName);
    _rootButton->setProperty("breadcrumbCurrent", _path.isEmpty());
    _rootButton->style()->unpolish(_rootButton); _rootButton->style()->polish(_rootButton);
    _rootButton->show();
    _breadcrumbLayout->addWidget(_rootButton);
    const auto parts = _path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    const QUrl root = _root;
    if (parts.size() > 2) {
        separator();
        auto *more = new QPushButton(QStringLiteral("…"), _breadcrumbs);
        more->setObjectName(QStringLiteral("driveBreadcrumbOverflow"));
        more->setProperty("udriveBreadcrumb", true);
        more->setAccessibleName(tr("Earlier parent folders"));
        more->setToolTip(tr("Earlier parent folders"));
        auto *menu = createFileMenu(more);
        for (int i = 0; i < parts.size() - 2; ++i) {
            const QString path = parts.mid(0, i + 1).join(QLatin1Char('/'));
            auto *action = menu->addAction(UDriveIcons::icon(UDriveIcons::Symbol::Folder),
                QString(parts.at(i)).replace(QLatin1Char('&'), QStringLiteral("&&")));
            action->setToolTip(path);
            connect(action, &QAction::triggered, this, [this, path, root] { navigateToBreadcrumb(path, root); });
        }
        more->setMenu(menu);
        more->show();
        _breadcrumbLayout->addWidget(more);
    }
    for (int i = qMax(0, int(parts.size()) - 2); i < parts.size(); ++i) {
        separator();
        const QString path = parts.mid(0, i + 1).join(QLatin1Char('/'));
        if (i == parts.size() - 1) {
            auto *current = new BreadcrumbLabel(parts.at(i), _breadcrumbs);
            current->setObjectName(QStringLiteral("driveCurrentFolder"));
            current->setAccessibleDescription(path);
            current->show();
            _breadcrumbLayout->addWidget(current, 1);
        } else {
            auto *button = new QPushButton(_breadcrumbs);
            button->setText(button->fontMetrics().elidedText(parts.at(i), Qt::ElideMiddle, 160).replace(QLatin1Char('&'), QStringLiteral("&&")));
            button->setObjectName(QStringLiteral("driveBreadcrumb"));
            button->setProperty("udriveBreadcrumb", true);
            button->setAccessibleName(parts.at(i)); button->setToolTip(path);
            button->setProperty("breadcrumbPath", path);
            connect(button, &QPushButton::clicked, this, [this, path, root] { navigateToBreadcrumb(path, root); });
            button->show();
            _breadcrumbLayout->addWidget(button);
        }
    }
    if (parts.isEmpty()) _breadcrumbLayout->addStretch();
}

void UDriveFileBrowser::updateSpaces()
{
    _heading->show(); _workspaceBulkActions->hide();
    switch (_section) {
    case Section::Personal: _heading->setText(tr("Personal")); break;
    case Section::Workspaces: _heading->setText(tr("Workspaces")); break;
    case Section::Shares: _heading->setText(tr("Shares")); break;
    case Section::Favorites: _heading->setText(tr("Favorites")); break;
    case Section::Trash: _heading->setText(tr("Deleted files")); break;
    }
    const bool isShares = (_section == Section::Shares);
    if (_proxy) {
        static_cast<FilesProxy *>(_proxy)->setSharesSection(isShares);
        if (!isShares) {
            if (_shareTypeFilter) {
                const QSignalBlocker blocker(_shareTypeFilter);
                _shareTypeFilter->setCurrentIndex(0);
            }
            if (_sharesSearch) {
                const QSignalBlocker blocker(_sharesSearch);
                _sharesSearch->clear();
            }
            static_cast<FilesProxy *>(_proxy)->setTypeFilter(0);
            _proxy->setFilterFixedString(_search ? _search->text() : QString());
        } else {
            _proxy->setFilterFixedString(_sharesSearch ? _sharesSearch->text() : QString());
        }
    }
    _sharesNav->setVisible(isShares);
    _sharesControls->setVisible(isShares);
    _shareScope->setVisible(isShares);
    _learnWorkspacesButton->setVisible(isWorkspacesOverview());
    _workspaceTitleDivider->setVisible(isWorkspacesOverview());
    _workspaceHeaderActions->setVisible(isWorkspacesOverview());
    _workspaceControls->setVisible(isWorkspacesOverview());
    _fileToolbar->setVisible(!isWorkspacesOverview() && !isShares);
    if (isShares) {
        _heading->hide();
        _sharedWithMeButton->setChecked(_shareTab == ShareTab::WithMe);
        _sharedWithOthersButton->setChecked(_shareTab == ShareTab::WithOthers);
        _sharedViaLinkButton->setChecked(_shareTab == ShareTab::ViaLink);
    }
    const bool insideWorkspace = (_section == Section::Workspaces && !_workspaceOverview);
    const bool folderView = _section != Section::Favorites && _section != Section::Trash && !sharedByMeOverview() && !isWorkspacesOverview() && !isShares;
    _newButton->setVisible(folderView && !_workspaceLayout);
    _breadcrumbs->setVisible((folderView || (isShares && !_path.isEmpty())) && (!_workspaceLayout || !_path.isEmpty() || insideWorkspace));
    const QString searchHint = _section == Section::Favorites ? tr("Search Favorites")
        : _section == Section::Trash ? tr("Search Deleted files")
        : isWorkspacesOverview() ? tr("Search workspaces")
        : sharedByMeOverview() ? tr("Search files shared by me")
        : isShares ? tr("Search files shared with you") : tr("Search this folder");
    _search->setPlaceholderText(searchHint);
    _search->setAccessibleName(searchHint);
    updateFileColumns();
    _model->setHeaderData(2, Qt::Horizontal, _section == Section::Trash ? tr("Deleted") : tr("Modified"));

    if (isWorkspacesOverview()) {
        const QSignalBlocker blocker(_spaces);
        _spaces->setVisible(false);
        _spaces->clear();
        _workspacesModel->removeRows(0, _workspacesModel->rowCount());
        _workspacesModel->setHorizontalHeaderLabels({tr("Name"), tr("Manager"), tr("Members"), tr("Remaining quota"), tr("Modified"), tr("Actions")});
        int count = 0;
        if (canBrowse()) {
            auto spaces = _state->account()->spacesManager()->spaces();
            std::sort(spaces.begin(), spaces.end(), [](const auto *a, const auto *b) {
                return a->displayName().localeAwareCompare(b->displayName()) < 0;
            });
            for (auto *space : spaces) {
                const bool personal = space->sortPriority() == 100;
                const bool shares = space->sortPriority() == 50;
                if (personal || shares) continue;
                if (space->disabled() && !_includeDisabledWorkspaces) continue;

                appendWorkspace(space->drive(), space->displayName());
                count++;
            }
        }
        if (_includeDisabledWorkspaces && canBrowse()) {
            for (const auto &drive : _state->account()->spacesManager()->disabledDrives()) {
                if (drive.getDriveType() != QStringLiteral("project") || !validName(drive.getId())
                    || !validName(drive.getRoot().getId())) continue;
                appendWorkspace(drive, drive.getName());
                ++count;
            }
        }
        _workspacesProxy->sort(_workspaceTableMode ? _workspacesTable->header()->sortIndicatorSection() : 0,
            _workspaceTableMode ? _workspacesTable->header()->sortIndicatorOrder() : _workspaceSort->currentIndex() == 1 ? Qt::DescendingOrder : Qt::AscendingOrder);
        QList<OpenAPI::OAIDrive> permissionDrives;
        for (int row = 0; row < _workspacesModel->rowCount(); ++row) permissionDrives.append(_workspacesModel->item(row, 0)->data(SpaceDriveRole).value<OpenAPI::OAIDrive>());
        _workspaceActions->setPermissionDrives(permissionDrives);
        if (!_workspaceNotice) _statusMessage = count > 0 ? tr("Workspaces: %1").arg(count) : tr("No workspaces found.");
        _message->setAlignment(Qt::AlignCenter);
        _message->setText(_statusMessage);
        _fileViews->setCurrentWidget(_workspacePage);
        updateBreadcrumbs();
        updateActions();
        return;
    }

    _message->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    const QSignalBlocker blocker(_spaces);
    const QString previous = _spaceId;
    _spaces->clear();
    if (_state && _state->account()) {
        auto spaces = _state->account()->spacesManager()->spaces();
        std::sort(spaces.begin(), spaces.end(), [](const auto *a, const auto *b) { return a->displayName().localeAwareCompare(b->displayName()) < 0; });
        for (auto *space : spaces) {
            const bool personal = space->sortPriority() == 100;
            const bool shares = space->sortPriority() == 50;
            // Shares aggregates received files; it has no storage-space trash bin.
            if (space->disabled() || (_section == Section::Personal && !personal)
                || (_section == Section::Workspaces && (personal || shares)) || (_section == Section::Shares && !shares)
                || (_section == Section::Trash && shares)) continue;
            const auto symbol = personal ? UDriveIcons::Symbol::Folder : shares ? UDriveIcons::Symbol::Link : UDriveIcons::Symbol::Grid;
            _spaces->addItem(UDriveIcons::icon(symbol), space->displayName(), space->id());
            _spaces->setItemData(_spaces->count() - 1, space->webDavUrl(), Qt::UserRole + 1);
            _spaces->setItemData(_spaces->count() - 1, space->webUrl(), Qt::UserRole + 2);
            _spaces->setItemData(_spaces->count() - 1, space->driveId(), Qt::UserRole + 3);
        }
    }
    const int previousIndex = _spaces->findData(previous);
    _spaces->setCurrentIndex(previousIndex < 0 ? (_spaces->count() ? 0 : -1) : previousIndex);
    // A single space is selected automatically; only show a selector when
    // there is a real choice. Favorites already spans all spaces.
    _spaces->setVisible(_section != Section::Favorites && !sharedByMeOverview() && !isShares && _spaces->count() > 1);
    _spaces->setEnabled(canBrowse() && _spaces->count());
    selectSpace();
}

void UDriveFileBrowser::selectSpace()
{
    updateEmptyState();
    _message->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    const QString id = _spaces->currentData().toString();
    QUrl root = _spaces->currentData(Qt::UserRole + 1).toUrl();
    // DAV collection responses may omit the trailing slash in their self href.
    root.setPath(withoutTrailingSlash(root.path()));
    _webUrl = _spaces->currentData(Qt::UserRole + 2).toUrl();
    _driveId = _spaces->currentData(Qt::UserRole + 3).toString();
    const QString name = _spaces->currentText().isEmpty() ? _heading->text() : _spaces->currentText();
    _spaces->setToolTip(_spaces->currentText());
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
    if (_uploading || _emptyingTrash) cancelOperation();
    _spaceId = id;
    _root = root;
    _path.clear();
    _search->clear();
    refresh();
}

void UDriveFileBrowser::cancelListing()
{
    _emptyStateReady = false;
    _thumbnailTimer->stop();
    _thumbnails->cancel();
    _thumbnailRows.clear();
    auto job = _listing;
    _listing.clear();
    if (job) job->abort();
    auto report = _report;
    _report.clear();
    if (report) report->abort();
    auto details = _shareItemJob;
    _shareItemJob.clear();
    if (details) details->abort();
    _detailsIndex = QPersistentModelIndex();
    _detailsRequested = false;
    clearFileShares();
    if (_fileDetails) _fileDetails->hide();
}

void UDriveFileBrowser::scheduleThumbnails()
{
    if (isVisible() && canBrowse() && _section != Section::Trash && !isWorkspacesOverview()) _thumbnailTimer->start();
}

bool UDriveFileBrowser::eventFilter(QObject *watched, QEvent *event)
{
    if ((watched == _workspacePage || watched == _workspacesView->viewport() || watched == _workspacesTable->viewport()) && event->type() == QEvent::Resize)
        QTimer::singleShot(0, this, &UDriveFileBrowser::updateWorkspaceCardHeight);
    const bool fileViewport = watched == _tree->viewport() || watched == _tiles->viewport() || watched == _workspacesView->viewport() || watched == _workspacesTable->viewport();
    if (fileViewport && _checkboxViewport == watched) {
        // A checkbox owns its entire mouse gesture. Passing only its movement
        // to Qt would start a rubber-band selection using an older press position.
        if (event->type() == QEvent::MouseMove) {
            if (static_cast<QMouseEvent *>(event)->buttons().testFlag(Qt::LeftButton)) return true;
            _checkboxViewport.clear(); // Recover if the release was lost.
        } else if (event->type() == QEvent::MouseButtonRelease
            && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
            _checkboxViewport.clear();
            return true;
        } else if (event->type() == QEvent::Hide || event->type() == QEvent::UngrabMouse
            || event->type() == QEvent::WindowDeactivate) {
            _checkboxViewport.clear();
        }
    }
    if (fileViewport && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick)) {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        auto *view = watched == _tree->viewport() ? static_cast<QAbstractItemView *>(_tree)
            : watched == _tiles->viewport() ? static_cast<QAbstractItemView *>(_tiles)
            : watched == _workspacesTable->viewport() ? static_cast<QAbstractItemView *>(_workspacesTable) : _workspacesView;
        const auto index = view->indexAt(mouse->position().toPoint());
        const QRect rect = view->visualRect(index);
        const QRect check = (view == _tree || view == _workspacesTable) ? QRect(rect.left() + 8, rect.center().y() - 9, 18, 18)
            : view == _workspacesView ? QRect(rect.left() + 18, rect.top() + 18, 18, 18).adjusted(-4, -4, 4, 4)
                                     : QRect(rect.left() + 15, rect.top() + 15, 18, 18);
        if (mouse->button() == Qt::LeftButton && index.isValid() && index.column() == 0 && check.contains(mouse->position().toPoint())) {
            _checkboxViewport = view->viewport();
            if (event->type() == QEvent::MouseButtonPress && _selectAll->isEnabled()) {
                view->setFocus();
                view->selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
                view->selectionModel()->select(index, QItemSelectionModel::Toggle | QItemSelectionModel::Rows);
            }
            return true;
        }
        _checkboxViewport.clear();
    }
    // A drag that began in the view must deliver its release to Qt, even if it
    // ends over a checkbox, so Qt can clear its drag state and selection overlay.
    if (fileViewport && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) scheduleThumbnails();
    return QWidget::eventFilter(watched, event);
}

void UDriveFileBrowser::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    updateFileDetails();
    scheduleThumbnails();
}

void UDriveFileBrowser::retranslate()
{
    updateSpaces();
    updateActions();
    updateSelectionState();
    updateEmptyState();
    if (_workspacesModel) {
        _workspacesModel->setHorizontalHeaderLabels({tr("Name"), tr("Manager"), tr("Members"), tr("Remaining quota"), tr("Modified"), tr("Actions")});
    }
    if (_model) {
        _model->setHeaderData(0, Qt::Horizontal, tr("Name"));
        _model->setHeaderData(1, Qt::Horizontal, tr("Size"));
        _model->setHeaderData(2, Qt::Horizontal, _section == Section::Trash ? tr("Deleted") : tr("Modified"));
        _model->setHeaderData(3, Qt::Horizontal, tr("Shared by"));
    }
    if (_sharedWithMeButton) _sharedWithMeButton->setText(tr("Shared with me"));
    if (_sharedWithOthersButton) _sharedWithOthersButton->setText(tr("Shared with others"));
    if (_sharedViaLinkButton) _sharedViaLinkButton->setText(tr("Shared via link"));
    if (_openButton) _openButton->setText(tr("Open"));
    if (_downloadButton) _downloadButton->setText(tr("Download"));
    if (_shareButton) _shareButton->setText(tr("Share"));
    if (_restoreButton) _restoreButton->setText(tr("Restore"));
    if (_emptyTrashButton) _emptyTrashButton->setText(tr("Empty trash"));
}

void UDriveFileBrowser::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange) {
        retranslate();
    }
    QWidget::changeEvent(event);
}

void UDriveFileBrowser::loadVisibleThumbnails()
{
    auto *view = activeView();
    if (!isVisible() || !view->isVisible() || !canBrowse() || _section == Section::Trash || isWorkspacesOverview()) return;
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
        if (rect.bottom() < 0 || index.data(ShareUnavailableRole).toBool() || index.data(DirectoryRole).toBool() || index.data(ThumbnailRole).toBool()
            || !UDriveThumbnails::supports(index.data().toString())) continue;
        _thumbnails->request(_proxy->mapToSource(index), index.data(RootRole).toUrl(), index.data(PathRole).toString(), index.data(EtagRole).toString());
    }
}

void UDriveFileBrowser::refresh()
{
    if (_emptyingTrash) return;
    if (isWorkspacesOverview()) {
        if (_workspaceActions->busy()) return;
        _workspaceActions->refreshPermissions();
        _workspaceNotice = false;
        cancelListing();
        updateSpaces();
        if (canBrowse()) _state->account()->spacesManager()->refresh();
        return;
    }
    cancelListing();
    _shareMetadataAvailable = false;
    _shareSharers.clear();
    _model->removeRows(0, _model->rowCount());
    updateBreadcrumbs();
    updateFileColumns();
    _search->clear();
    _folderPermissions.clear();
    _folderPermissionsKnown = false;
    updateActions();
    if (!canBrowse()) {
        setMessage(tr("Sign in to browse your files. You can open synced folders from Sync."));
        return;
    }
    if (_section == Section::Favorites) { refreshFavorites(); return; }
    if (sharedByMeOverview()) { refreshSharedByMe(); return; }
    if (_section == Section::Shares && _path.isEmpty() && _incomingSharesApiAvailable) { refreshIncomingShares(); return; }
    if (_root.isEmpty()) {
        _emptyStateReady = _section == Section::Shares && _state->account()->spacesManager()->isReady();
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
        "http://owncloud.org/ns:size", "http://owncloud.org/ns:permissions", "http://owncloud.org/ns:favorite", "http://owncloud.org/ns:privatelink",
        "http://owncloud.org/ns:trashbin-original-filename", "http://owncloud.org/ns:trashbin-original-location", "http://owncloud.org/ns:trashbin-delete-datetime",
        "http://owncloud.org/ns:fileid", "http://owncloud.org/ns:id"});
    connect(job, &PropfindJob::directoryListingIterated, this, [this, job](const QString &href, const QMap<QString, QString> &properties) {
        if (_listing == job) appendEntry(href, properties);
    });
    connect(job, &PropfindJob::finishedWithoutError, this, [this, job] {
        if (_listing != job) return;
        _listing.clear();
        _emptyStateReady = true;
        qint64 totalBytes = 0;
        bool hasKnownSize = false;
        for (int row = 0; row < _model->rowCount(); ++row) {
            const qint64 b = _model->index(row, 1).data(SortRole).toLongLong();
            if (b >= 0) {
                totalBytes += b;
                hasKnownSize = true;
            }
        }
        if (_model->rowCount() == 0) {
            setMessage(tr("This folder is empty."));
        } else if (hasKnownSize && totalBytes > 0) {
            setMessage(tr("%n item(s) (%1)", nullptr, _model->rowCount()).arg(QLocale().formattedDataSize(totalBytes)));
        } else {
            setMessage(tr("%n item(s)", nullptr, _model->rowCount()));
        }
        updateActions();
        scheduleThumbnails();
        updateEmptyState();
    });
    connect(job, &PropfindJob::finishedWithError, this, [this, job] {
        if (_listing != job) return;
        _listing.clear();
        _model->removeRows(0, _model->rowCount());
        const QString error = job->errorString();
        setMessage(error.isEmpty() ? tr("The server did not return a valid folder listing.") : tr("Could not load this folder: %1").arg(error));
        updateActions();
        updateEmptyState();
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
    item->setData(trash, TrashedRole);
    item->setData(root, RootRole);
    item->setData(entrySpace.isEmpty() ? _spaceId : entrySpace, SpaceRole);
    item->setData(properties.value(QStringLiteral("permissions")), PermissionsRole);
    item->setData(properties.value(QStringLiteral("favorite")) == QStringLiteral("1") || properties.value(QStringLiteral("favorite")) == QStringLiteral("true"), FavoriteRole);
    item->setData(properties.contains(QStringLiteral("favorite")), FavoriteKnownRole);
    item->setData(properties.value(QStringLiteral("getetag")), EtagRole);
    item->setData(QUrl(properties.value(QStringLiteral("privatelink"))), LinkRole);
    QString fileId = properties.value(QStringLiteral("fileid"));
    if (fileId.isEmpty()) fileId = properties.value(QStringLiteral("id"));
    item->setData(fileId, FileIdRole);
    QString restorePath = properties.value(QStringLiteral("trashbin-original-location"));
    while (restorePath.startsWith(QLatin1Char('/'))) restorePath.remove(0, 1);
    item->setData(validPath(restorePath) ? restorePath : QString(), RestorePathRole);
    item->setData(name, SortRole);
    item->setData(_recentFiles.value(recentKey(root, relative), qint64(0)), OpenedRole);
    item->setToolTip(name);
    bool validSize = false;
    qint64 bytes = -1;
    if (properties.contains(QStringLiteral("size"))) {
        bytes = properties.value(QStringLiteral("size")).toLongLong(&validSize);
    }
    if (!validSize && properties.contains(QStringLiteral("getcontentlength"))) {
        bytes = properties.value(QStringLiteral("getcontentlength")).toLongLong(&validSize);
    }
    validSize = validSize && bytes >= 0;
    auto *size = new QStandardItem(!validSize ? QStringLiteral("—") : QLocale().formattedDataSize(bytes));
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
    item->setData(locationText, LocationRole);
    location->setData(location->text(), SortRole);
    size->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto *actions = new QStandardItem;
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

bool UDriveFileBrowser::rowActionEnabled(const QModelIndex &index, int action) const
{
    if (!index.isValid() || !canBrowse() || _listing || _report || _operation) return false;
    if (action == RowMore) return true;
    if (index.data(ShareUnavailableRole).toBool() && action != RowLink) return false;
    if (index.data(TrashedRole).toBool()) return false;
    if (action == RowFavorite) return index.data(FavoriteKnownRole).toBool();
    if (action == RowShare) {
        const QString permissions = index.data(PermissionsRole).toString();
        return !_download && (permissions.isEmpty() || permissions.contains(QLatin1Char('R')));
    }
    const QUrl link = index.data(LinkRole).toUrl();
    return action == RowLink && (link.scheme() == QStringLiteral("https") || link.scheme() == QStringLiteral("http"));
}

void UDriveFileBrowser::updateActions()
{
    _emptyTrashButton->setVisible(_section == Section::Trash);
    _emptyTrashButton->setEnabled(_section == Section::Trash && canBrowse() && !_listing && !_report && !_operation && !_download
        && !_emptyingTrash && _emptyStateReady && _model->rowCount() > 0 && !trashRoot().isEmpty());
    if (isWorkspacesOverview()) {
        const bool idle = !_workspaceActions->busy();
        const auto rows = selectedWorkspaces();
        const bool allActive = !rows.isEmpty() && std::all_of(rows.cbegin(), rows.cend(), [](const auto &row) { return !row.data(SpaceDisabledRole).toBool(); });
        _workspaceBulkActions->setVisible(!rows.isEmpty() || !idle);
        _heading->setVisible(rows.isEmpty() && idle);
        _learnWorkspacesButton->setVisible(rows.isEmpty() && idle);
        _workspaceTitleDivider->setVisible(rows.isEmpty() && idle);
        auto allowed = [this, &rows, idle, allActive](const QString &action) {
            return canBrowse() && idle && allActive && std::all_of(rows.cbegin(), rows.cend(), [this, &action](const auto &row) {
                return _workspaceActions->can(action, row.data(SpaceDriveRole).template value<OpenAPI::OAIDrive>());
            });
        };
        _workspaceDownloadAction->setEnabled(allowed(QStringLiteral("download")) && _workspaceActions->archiveAvailable());
        _workspaceDuplicateAction->setEnabled(allowed(QStringLiteral("duplicate")));
        _workspaceQuotaAction->setEnabled(allowed(QStringLiteral("quota")));
        _workspaceDisableAction->setEnabled(allowed(QStringLiteral("disable")));
        _workspaceCancelButton->setVisible(!idle);
        const bool hasSel = _workspacesView && _workspacesView->selectionModel() && _workspacesView->selectionModel()->hasSelection();
        _openButton->setText(tr("Open workspace"));
        const auto selected = selectedWorkspaces();
        _openButton->setEnabled(canBrowse() && idle && hasSel && selected.size() == 1 && !selected.first().data(SpaceDisabledRole).toBool());
        _workspacesView->setEnabled(canBrowse() && idle);
        _workspacesTable->setEnabled(canBrowse() && idle);
        _openButton->setVisible(hasSel);
        _downloadButton->setVisible(false);
        _shareButton->setVisible(false);
        _restoreButton->setVisible(false);
        _refreshButton->setEnabled(canBrowse() && idle);
        if (_search) _search->setEnabled(canBrowse() && idle);
        _viewButton->setEnabled(false);
        _includeDisabledButton->setEnabled(canBrowse() && idle);
        _workspaceSelectAll->setEnabled(canBrowse() && idle);
        _workspaceSort->setEnabled(canBrowse() && idle);
        _workspaceSearch->setEnabled(canBrowse() && idle);
        _workspaceHeaderActions->setEnabled(canBrowse() && idle);
        _message->hide();
        updateSelectionState();
        updateFileDetails();
        Q_EMIT creationAvailabilityChanged();
        return;
    }
    _message->setVisible(!_uploading && !_resultMessage);
    _heading->show(); _workspaceBulkActions->hide();
    _workspacesView->setEnabled(canBrowse());
    _viewButton->setEnabled(true);
    const auto index = selectedItem();
    const bool valid = index.isValid() && !index.data(ShareUnavailableRole).toBool() && canBrowse() && !_listing && !_report && !_operation && !_emptyingTrash;
    const bool trash = _section == Section::Trash;
    const bool overview = sharedByMeOverview();
    const bool directory = index.data(DirectoryRole).toBool();
    bool pdf = false;
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    pdf = !directory && index.data().toString().endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive);
#endif
    const bool external = UDriveFilePreview::kindForName(index.data().toString()) == UDriveFilePreview::Kind::External;
    _openButton->setText(directory ? tr("Open folder") : pdf ? tr("Open PDF") : external ? tr("Open in app") : tr("Preview"));
    _openButton->setEnabled(valid && !trash && !_download);
    _openButton->setToolTip(pdf  ? tr("Preview this PDF inside UDrive.")
            : directory ? QString() : external ? tr("Open this file with its installed application. Files outside a sync folder open as read-only copies.")
                                 : tr("Preview this file inside UDrive."));
    _downloadButton->setEnabled(valid && !trash && !directory && !_download);
    const bool navigate = canBrowse() && !_root.isEmpty() && _section != Section::Favorites && !overview && !trash && !_operation;
    _breadcrumbs->setEnabled(navigate);
    _rootButton->setEnabled(navigate && !_path.isEmpty());
    const bool incomingOverview = _section == Section::Shares && _path.isEmpty();
    _refreshButton->setEnabled(canBrowse() && !_emptyingTrash && (!_root.isEmpty() || overview || incomingOverview));
    _shareScope->setEnabled(canBrowse() && !_operation);
    if (_search)
        _search->setEnabled(canBrowse() && (!_root.isEmpty() || overview || incomingOverview));
    _spaces->setEnabled(canBrowse() && _spaces->count());
    const bool writableFolder =
        canBrowse() && !_root.isEmpty() && !_listing && !_report && !_operation && !_download && !trash && _section != Section::Favorites && !overview
        && !(_section == Section::Shares && _path.isEmpty());
    _newFolderAction->setEnabled(writableFolder && (!_folderPermissionsKnown || _folderPermissions.contains(QLatin1Char('K'))));
    _uploadAction->setEnabled(writableFolder && (!_folderPermissionsKnown || _folderPermissions.contains(QLatin1Char('C'))));
    _newButton->setEnabled(_newFolderAction->isEnabled() || _uploadAction->isEnabled());
    const QString permissions = index.data(PermissionsRole).toString();
    _renameAction->setEnabled(valid && !trash && !_download && (permissions.contains(QLatin1Char('N')) || (!overview && permissions.isEmpty())));
    _copyAction->setEnabled(
        valid && !trash && !overview && !incomingOverview && !_download && (!_folderPermissionsKnown || _folderPermissions.contains(directory ? QLatin1Char('K') : QLatin1Char('C'))));
    _moveAction->setEnabled(valid && !trash && !overview && !_download && (permissions.isEmpty() || permissions.contains(QLatin1Char('V'))));
    _deleteAction->setEnabled(valid && !trash && !overview && !_download && (permissions.isEmpty() || permissions.contains(QLatin1Char('D'))));
    _restoreAction->setVisible(trash);
    _restoreAction->setEnabled(valid && trash && !index.data(RestorePathRole).toString().isEmpty());
    _permanentDeleteAction->setVisible(trash);
    _permanentDeleteAction->setEnabled(valid && trash);
    const bool sharesOverview = _tree->property("driveSharesOverview").toBool();
    _favoriteAction->setVisible(!trash && !sharesOverview);
    _favoriteAction->setEnabled(rowActionEnabled(index, RowFavorite));
    _favoriteAction->setText(index.data(FavoriteRole).toBool() ? tr("Remove from Favorites") : tr("Add to Favorites"));
    _favoriteAction->setIcon(UDriveIcons::icon(index.data(FavoriteRole).toBool() ? UDriveIcons::Symbol::StarFilled : UDriveIcons::Symbol::Star));
    const QUrl link = index.data(LinkRole).toUrl();
    _linkAction->setEnabled(rowActionEnabled(index, RowLink));
    _linkAction->setVisible(!trash);
    _linkAction->setText(sharesOverview ? tr("Copy permanent link") : tr("Copy sign-in link"));
    _shareAction->setEnabled(rowActionEnabled(index, RowShare));
    _shareAction->setVisible(!trash);
    const bool hasWeb = (link.scheme() == QStringLiteral("https") || link.scheme() == QStringLiteral("http")) ||
                        (_webUrl.scheme() == QStringLiteral("https") || _webUrl.scheme() == QStringLiteral("http"));
    _webAction->setEnabled(index.isValid() && canBrowse() && !_listing && !_report && !_operation && !trash && hasWeb);
    _webAction->setVisible(!trash && !sharesOverview);
    _versionsAction->setEnabled(valid && !trash && !directory && !sharesOverview);
    _versionsAction->setVisible(!trash && !sharesOverview);
    _deleteAction->setVisible(!trash && !sharesOverview);
    _renameAction->setVisible(!trash);
    _copyAction->setVisible(!trash && !sharesOverview);
    _moveAction->setVisible(!trash && !sharesOverview);
    _fileMenu->setEnabled(valid);
    _openButton->setVisible(index.isValid() && !trash && !sharesOverview);
    _downloadButton->setVisible(index.isValid() && !trash && !sharesOverview);
    _shareButton->setVisible(index.isValid() && !trash && !sharesOverview);
    _shareButton->setEnabled(_shareAction->isEnabled());
    _restoreButton->setVisible(index.isValid() && trash);
    _restoreButton->setEnabled(valid && trash && !index.data(RestorePathRole).toString().isEmpty());
    _tree->viewport()->update();
    updateFileDetails();
    updateShareFilters();
    updateSelectionState();
    Q_EMIT creationAvailabilityChanged();
}

void UDriveFileBrowser::activateSelection()
{
    if (isWorkspacesOverview()) {
        const auto indexes = _workspacesView && _workspacesView->selectionModel() ? selectedWorkspaces() : QModelIndexList();
        if (!indexes.isEmpty()) {
            openWorkspace(indexes.first().data(SpaceIdRole).toString());
        }
        return;
    }
    const auto index = selectedItem();
    if (!index.isValid() || _listing || _report || _operation || !canBrowse() || _section == Section::Trash) return;
    if (index.data(ShareUnavailableRole).toBool()) {
        setMessage(tr("Open this share on the web to enable syncing with this device."));
        return;
    }
    const QString path = index.data(PathRole).toString();
    if (index.data(DirectoryRole).toBool()) {
        cancelDownload();
        closePdf();
        if (_section == Section::Favorites || sharedByMeOverview()) {
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
    } else {
        previewSelectedFile();
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
    if (isWorkspacesOverview()) return canBrowse() && _workspaceActions->canCreate() && !_workspaceActions->busy();
    return _newButton->isEnabled();
}

void UDriveFileBrowser::showNewMenu(const QPoint &position, int minimumWidth)
{
    if (canCreate()) {
        auto *menu = _newButton->menu();
        menu->setMinimumWidth(qMax(0, minimumWidth));
        menu->popup(position);
    }
}

void UDriveFileBrowser::setMessage(const QString &message)
{
    _resultMessage = false;
    _statusMessage = message;
    _workspaceNotice = isWorkspacesOverview();
    updateSelectionState();
}

void UDriveFileBrowser::showResult(const QString &message, bool success)
{
    showNotice(message, success ? Notice::Success : Notice::Error);
}

void UDriveFileBrowser::showNotice(const QString &message, Notice kind)
{
    _statusMessage = message;
    _resultMessage = true;
    _workspaceNotice = false;
    updateSelectionState();
    _message->hide();
    UDriveToast::showMessage(this, message, kind == Notice::Error ? UDriveToast::Kind::Error
        : kind == Notice::Success ? UDriveToast::Kind::Success : UDriveToast::Kind::Information);
}

int UDriveFileBrowser::selectedCount() const
{
    int count = 0;
    for (const auto &range : _tree->selectionModel()->selection())
        if (range.left() == 0) count += range.height();
    return count;
}

QModelIndex UDriveFileBrowser::selectedItem() const
{
    QModelIndex selected;
    for (const auto &range : _tree->selectionModel()->selection()) {
        if (range.left() != 0) continue;
        if (range.height() != 1 || selected.isValid()) return {};
        selected = range.topLeft();
    }
    return selected;
}

void UDriveFileBrowser::selectOnly(const QModelIndex &index)
{
    _tree->selectionModel()->setCurrentIndex(index.siblingAtColumn(0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
}

void UDriveFileBrowser::selectAllFiles(bool select)
{
    if (!_selectAll->isEnabled()) return;
    if (isWorkspacesOverview()) {
        if (!_workspacesView) return;
        if (select) {
            _workspacesView->selectionModel()->select(QItemSelection(_workspacesProxy->index(0, 0),
                _workspacesProxy->index(_workspacesProxy->rowCount() - 1, _workspacesProxy->columnCount() - 1)), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        } else {
            _workspacesView->clearSelection();
        }
        updateSelectionState();
        updateActions();
        return;
    }
    auto *selection = _tree->selectionModel();
    if (select) {
        const QItemSelection results(_proxy->index(0, 0), _proxy->index(_proxy->rowCount() - 1, _proxy->columnCount() - 1));
        selection->select(results, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    } else {
        selection->clearSelection();
    }
    updateActions();
}

void UDriveFileBrowser::updateWorkspaceCardHeight()
{
    if (!isWorkspacesOverview() || !_workspacePage || !_workspaceCount) return;
    const QSize grid = _workspacesView->gridSize();
    const int columns = qMax(1, _workspacesView->viewport()->width() / grid.width());
    const int count = _workspacesProxy->rowCount();
    const int rows = (count + columns - 1) / columns;
    const int desired = _workspaceTableMode ? _workspacesTable->header()->height() + count * 48 + 16 : rows * grid.height() + 16;
    const int available = qMax(80, _workspacePage->height() - _workspaceCount->sizeHint().height() - 16);
    _workspaceViews->setMaximumHeight(qMin(desired, available));
}

void UDriveFileBrowser::updateSelectionState()
{
    if (isWorkspacesOverview()) {
        const int count = _workspacesView && _workspacesView->selectionModel()
            ? selectedWorkspaces().count() : 0;
        const int total = _workspacesProxy ? _workspacesProxy->rowCount() : 0;
        const QSignalBlocker blocker(_selectAll);
        _selectAll->setCheckState(count == 0 ? Qt::Unchecked : count == total ? Qt::Checked : Qt::PartiallyChecked);
        _selectAll->setEnabled(canBrowse() && !_workspaceActions->busy() && total > 0);
        _message->setAlignment(Qt::AlignCenter);
        for (auto *checkbox : {_workspaceSelectAll, _workspaceHeaderSelectAll}) {
            const QSignalBlocker workspaceBlocker(checkbox);
            checkbox->setCheckState(_selectAll->checkState());
            checkbox->setEnabled(_selectAll->isEnabled());
        }
        const QString summary = _workspaceNotice ? _statusMessage
            : count > 0 ? tr("Selected workspaces: %1").arg(count)
            : total ? tr("Workspaces: %1").arg(total) : tr("No workspaces found.");
        _message->setText(summary);
        _workspaceCount->setText(summary);
        updateWorkspaceCardHeight();
        updateEmptyState();
        return;
    }
    const int count = selectedCount();
    const int total = _proxy->rowCount();
    const QSignalBlocker blocker(_selectAll);
    _selectAll->setCheckState(count == 0 ? Qt::Unchecked : count == total ? Qt::Checked : Qt::PartiallyChecked);
    _selectAll->setEnabled(canBrowse() && total > 0 && !_listing && !_report && !_operation && !_download);
    if (count > 1) {
        qint64 selectedBytes = 0;
        bool hasSelectedBytes = false;
        auto *view = activeView();
        if (view && view->selectionModel()) {
            const auto selectedRows = view->selectionModel()->selectedRows(0);
            for (const auto &proxyIdx : selectedRows) {
                const auto sourceIdx = _proxy->mapToSource(proxyIdx);
                const qint64 b = _model->index(sourceIdx.row(), 1).data(SortRole).toLongLong();
                if (b >= 0) {
                    selectedBytes += b;
                    hasSelectedBytes = true;
                }
            }
        }
        if (hasSelectedBytes && selectedBytes > 0) {
            _message->setText(tr("%1 selected (%2). Select one item to use file actions.")
                .arg(count).arg(QLocale().formattedDataSize(selectedBytes)));
        } else {
            _message->setText(tr("%1 selected. Select one item to use file actions.").arg(count));
        }
    } else {
        _message->setText(_statusMessage);
    }
    updateEmptyState();
}

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

void UDriveFileBrowser::refreshIncomingShares()
{
    setMessage(tr("Loading files shared with you…"));
    auto *job = new JsonJob(_state->account(), _state->account()->url(),
        QStringLiteral("graph/v1beta1/me/drive/sharedWithMe"), "GET", SimpleNetworkJob::UrlQuery{}, QNetworkRequest{}, this);
    _report = job;
    job->setTimeout(std::chrono::seconds(30));
    job->addNewReplyHook([job](QNetworkReply *reply) {
        QObject::connect(reply, &QIODevice::readyRead, job, [job, reply] {
            if (reply->bytesAvailable() > 4 * 1024 * 1024) job->abort();
        });
    });
    connect(job, &JsonJob::finishedSignal, this, [this, job] {
        if (_report != job || _section != Section::Shares || _shareTab != ShareTab::WithMe || !_path.isEmpty() || !canBrowse()) return;
        _report.clear();
        if (job->httpStatusCode() == 404 || job->httpStatusCode() == 405 || job->httpStatusCode() == 501) {
            // Older servers can still browse the mounted share jail via DAV.
            // Do not pretend DAV supplies hidden state or sharer identities.
            _incomingSharesApiAvailable = false;
            _includeHiddenShares = false;
            refresh();
            return;
        }
        const auto value = job->data().value(QStringLiteral("value"));
        if (!job->reply() || job->reply()->error() != QNetworkReply::NoError || job->httpStatusCode() != 200 || !value.isArray()) {
            setMessage(tr("Could not load files shared with you. %1").arg(job->errorString()));
            updateActions();
            return;
        }
        _shareMetadataAvailable = true;
        int unavailable = 0;
        QSet<QString> seen;
        for (const auto &entry : value.toArray()) {
            if (_model->rowCount() >= 1000) { ++unavailable; break; }
            const auto item = entry.toObject();
            const auto remote = item.value(QStringLiteral("remoteItem")).toObject();
            const auto parent = item.value(QStringLiteral("parentReference")).toObject();
            const QString drive = parent.value(QStringLiteral("driveId")).toString();
            const QString id = item.value(QStringLiteral("id")).toString();
            const QString name = item.value(QStringLiteral("name")).toString();
            if (!validName(drive) || !validName(id) || !id.startsWith(drive + QLatin1Char('!'))
                || id.endsWith(QLatin1Char('!')) || !validName(name) || !item.value(QStringLiteral("remoteItem")).isObject()) {
                ++unavailable; continue;
            }
            if (seen.contains(id)) continue;
            seen.insert(id);
            GraphApi::Space *jail = nullptr;
            for (auto *space : _state->account()->spacesManager()->spaces())
                if (space->sortPriority() == 50 && !space->disabled() && space->driveId() == drive) { jail = space; break; }
            // Unmounted shares are still listed and filterable, but their names
            // must never become guessed DAV request targets.
            const bool mounted = jail && item.value(QStringLiteral("@client.synchronize")).toBool(false);
            QUrl root = jail ? jail->webDavUrl() : _state->account()->url();
            root.setPath(withoutTrailingSlash(root.path()));
            if (!LocalDevelopment::allowsAuthenticatedUrl(root, _state->account()->url()) || !root.userInfo().isEmpty()
                || !root.query().isEmpty() || !root.fragment().isEmpty()) { ++unavailable; continue; }
            QMap<QString, QString> props;
            const bool knownType = remote.contains(QStringLiteral("folder")) || remote.contains(QStringLiteral("file"));
            props.insert(QStringLiteral("resourcetype"), remote.contains(QStringLiteral("folder")) ? QStringLiteral("collection") : QString());
            props.insert(QStringLiteral("permissions"), QStringLiteral("R"));
            props.insert(QStringLiteral("getetag"), item.value(QStringLiteral("eTag")).toString());
            const auto size = remote.value(QStringLiteral("size"));
            if (size.isDouble() && size.toDouble() >= 0) props.insert(QStringLiteral("getcontentlength"), QString::number(size.toInteger()));
            const auto modified = QDateTime::fromString(remote.value(QStringLiteral("lastModifiedDateTime")).toString(), Qt::ISODate);
            if (modified.isValid()) props.insert(QStringLiteral("getlastmodified"), modified.toUTC().toString(QStringLiteral("ddd, dd MMM yyyy HH:mm:ss 'GMT'")));
            const QUrl link(remote.value(QStringLiteral("webUrl")).toString());
            if (link.isValid() && link.userInfo().isEmpty() && (link.scheme() == QStringLiteral("https") || LocalDevelopment::allowsHttp(link)))
                props.insert(QStringLiteral("privatelink"), link.toString());
            const int row = _model->rowCount();
            appendEntry(root.path() + QLatin1Char('/') + name, props, root, jail ? jail->id() : QString(), true);
            if (_model->rowCount() != row + 1) { ++unavailable; continue; }
            auto *file = _model->item(row, 0);
            const auto creators = shareCreators(item, true);
            for (auto i = creators.cbegin(); i != creators.cend(); ++i) _shareSharers.insert(i.key(), i.value());
            file->setData(creators.keys(), ShareSharersRole);
            file->setData(creators.values(), SharePeopleRole);
            _model->item(row, 3)->setText(creators.values().join(QStringLiteral(", ")));
            _model->item(row, 3)->setToolTip(creators.values().join(QLatin1Char('\n')));
            // Accept the older lowercase spelling documented by early servers.
            file->setData(item.value(QStringLiteral("@UI.Hidden")).toBool(item.value(QStringLiteral("@UI.hidden")).toBool(false)), ShareHiddenRole);
            file->setData(!mounted || !knownType, ShareUnavailableRole);
            if (!mounted || !knownType) file->setToolTip(tr("Open this share on the web to enable syncing with this device."));
        }
        if (_sharesNotifier)
            _sharesNotifier->recordKnownShareIds(seen);
        _emptyStateReady = true;
        rebuildSharerFilter();
        setMessage(unavailable ? tr("%1 shared items shown; some items are no longer available.").arg(_model->rowCount())
            : _model->rowCount() ? tr("%n item(s)", nullptr, _model->rowCount()) : tr("No shares"));
        updateActions();
        scheduleThumbnails();
    });
    job->start();
    updateActions();
}

void UDriveFileBrowser::refreshSharedByMe()
{
    setMessage(tr("Loading files shared by you…"));
    auto *job = new JsonJob(_state->account(), _state->account()->url(),
        QStringLiteral("graph/v1beta1/me/drive/sharedByMe"), "GET", SimpleNetworkJob::UrlQuery{}, QNetworkRequest{}, this);
    _report = job;
    job->setTimeout(std::chrono::seconds(30));
    job->addNewReplyHook([job](QNetworkReply *reply) {
        QObject::connect(reply, &QIODevice::readyRead, job, [job, reply] {
            if (reply->bytesAvailable() > 4 * 1024 * 1024) job->abort();
        });
    });
    connect(job, &JsonJob::finishedSignal, this, [this, job] {
        if (_report != job || !sharedByMeOverview() || !canBrowse()) return;
        _report.clear();
        const auto value = job->data().value(QStringLiteral("value"));
        if (!job->reply() || job->reply()->error() != QNetworkReply::NoError || job->httpStatusCode() != 200 || !value.isArray()) {
            setMessage(tr("Could not load files shared by you. %1").arg(job->errorString()));
            updateActions();
            updateEmptyState();
            return;
        }
        QSet<QString> seen;
        int unavailable = 0;
        _shareMetadataAvailable = true;
        const auto entries = value.toArray();
        for (const auto &entry : entries) {
            if (_model->rowCount() >= 1000) { ++unavailable; break; }
            const auto item = entry.toObject();
            const auto permissions = item.value(QStringLiteral("permissions"));
            if (!permissions.isArray()) { ++unavailable; continue; }
            QJsonArray matchedPermissions;
            for (const auto &value : permissions.toArray()) {
                const auto permission = value.toObject();
                const bool link = permission.value(QStringLiteral("link")).isObject();
                const bool direct = !link && (permission.value(QStringLiteral("invitation")).isObject()
                    || permission.value(QStringLiteral("grantedToV2")).isObject()
                    || !permission.value(QStringLiteral("grantedToV2")).toArray().isEmpty()
                    || !permission.value(QStringLiteral("grantedToIdentities")).toArray().isEmpty());
                if (_shareTab == ShareTab::ViaLink ? link : direct) matchedPermissions.append(permission);
            }
            if (matchedPermissions.isEmpty()) continue;
            const auto parent = item.value(QStringLiteral("parentReference")).toObject();
            const QString driveId = parent.value(QStringLiteral("driveId")).toString();
            const QString id = item.value(QStringLiteral("id")).toString();
            const QString name = item.value(QStringLiteral("name")).toString();
            const auto parentPath = parent.value(QStringLiteral("path"));
            QString path = parentPath.toString();
            if (path.startsWith(QLatin1Char('/'))) path.remove(0, 1);
            while (path.startsWith(QStringLiteral("./"))) path.remove(0, 2);
            if (path == QStringLiteral(".")) path.clear();
            if (!path.isEmpty()) path += QLatin1Char('/');
            path += name;
            if (!parentPath.isString() || !validName(name) || !validPath(path) || !validName(id)
                || driveId.isEmpty() || !id.startsWith(driveId + QLatin1Char('!')) || id.endsWith(QLatin1Char('!'))) {
                ++unavailable;
                continue;
            }
            if (seen.contains(id)) continue;
            bool mapped = false;
            for (const auto *space : _state->account()->spacesManager()->spaces()) {
                if (space->disabled() || space->driveId() != driveId || id == space->id()) continue;
                QUrl root = space->webDavUrl();
                root.setPath(withoutTrailingSlash(root.path()));
                if (!LocalDevelopment::allowsAuthenticatedUrl(root, _state->account()->url())
                    || !root.userInfo().isEmpty() || !root.query().isEmpty() || !root.fragment().isEmpty()) continue;
                QMap<QString, QString> properties;
                properties.insert(QStringLiteral("resourcetype"), item.contains(QStringLiteral("folder")) ? QStringLiteral("collection") : QString());
                properties.insert(QStringLiteral("getetag"), item.value(QStringLiteral("eTag")).toString());
                // This overview does not infer the owner's write permissions from
                // the recipients' grants. Folder navigation reloads DAV permissions.
                properties.insert(QStringLiteral("permissions"), QStringLiteral("R"));
                if (item.value(QStringLiteral("size")).isDouble() && item.value(QStringLiteral("size")).toDouble() >= 0)
                    properties.insert(QStringLiteral("getcontentlength"), QString::number(item.value(QStringLiteral("size")).toVariant().toLongLong()));
                const auto modified = QDateTime::fromString(item.value(QStringLiteral("lastModifiedDateTime")).toString(), Qt::ISODate);
                if (modified.isValid()) properties.insert(QStringLiteral("getlastmodified"), modified.toUTC().toString(QStringLiteral("ddd, dd MMM yyyy HH:mm:ss 'GMT'")));
                const QUrl link(item.value(QStringLiteral("webUrl")).toString());
                if (!link.isEmpty() && LocalDevelopment::allowsAuthenticatedUrl(link, _state->account()->url()))
                    properties.insert(QStringLiteral("privatelink"), link.toString());
                const int row = _model->rowCount();
                appendEntry(root.path() + QLatin1Char('/') + path, properties, root, space->id(), true);
                if (_model->rowCount() != row + 1) continue;
                auto creatorItem = item;
                creatorItem.insert(QStringLiteral("permissions"), matchedPermissions);
                auto creators = shareCreators(creatorItem, false);
                if (creators.isEmpty()) creators.insert(QStringLiteral("account:") + _state->account()->uuid().toString(), tr("Shared by me"));
                for (auto i = creators.cbegin(); i != creators.cend(); ++i) _shareSharers.insert(i.key(), i.value());
                _model->item(row, 0)->setData(creators.keys(), ShareSharersRole);
                const auto recipients = shareRecipients(creatorItem);
                _model->item(row, 0)->setData(recipients, SharePeopleRole);
                _model->item(row, 3)->setText(recipients.join(QStringLiteral(", ")));
                _model->item(row, 3)->setToolTip(recipients.join(QLatin1Char('\n')));
                seen.insert(id);
                mapped = true;
                break;
            }
            if (!mapped) ++unavailable;
        }
        _emptyStateReady = true;
        rebuildSharerFilter();
        setMessage(unavailable ? tr("%1 shared items shown; some items are no longer available.").arg(_model->rowCount())
            : _model->rowCount() ? tr("%n item(s)", nullptr, _model->rowCount()) : tr("No files shared by you yet."));
        updateActions();
        scheduleThumbnails();
        updateEmptyState();
    });
    job->start();
    updateActions();
}

void UDriveFileBrowser::refreshFavorites()
{
    setMessage(tr("Loading Favorites…"));
    QNetworkRequest request;
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/xml; charset=utf-8"));
    QByteArray body = "<?xml version=\"1.0\"?><oc:filter-files xmlns:d=\"DAV:\" xmlns:oc=\"http://owncloud.org/ns\">"
        "<d:prop><d:resourcetype/><d:getcontentlength/><d:getlastmodified/><d:getetag/><oc:permissions/><oc:favorite/><oc:privatelink/><oc:spaceid/><oc:size/><oc:fileid/><oc:id/></d:prop>"
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
                const auto spaces = _state->account()->spacesManager()->spaces();
                auto normalized = [](QString path) {
                    // Reverse proxies and DAV endpoints may include or omit
                    // remote.php. Keep the already-decoded resource suffix intact.
                    if (path.startsWith(QLatin1String("/remote.php/dav/"))) path.remove(0, 11);
                    return path;
                };
                auto sameOrigin = [](const QUrl &a, const QUrl &b) {
                    const int defaultPort = a.scheme() == QStringLiteral("https") ? 443 : 80;
                    return a.isValid() && a.userInfo().isEmpty() && a.scheme() == b.scheme()
                        && a.host() == b.host() && a.port(defaultPort) == b.port(defaultPort);
                };
                // The DAV parser has already decoded href. Parsing its whole
                // URL again would reinterpret literal %, # and ? in file names.
                QString path = href;
                const int scheme = path.indexOf(QStringLiteral("://"));
                if (scheme >= 0 && !path.startsWith(QLatin1Char('/'))) {
                    const int slash = path.indexOf(QLatin1Char('/'), scheme + 3);
                    const QUrl origin(slash >= 0 ? path.left(slash) : path);
                    bool knownOrigin = sameOrigin(origin, _state->account()->url());
                    for (const auto *space : spaces)
                        knownOrigin |= !space->disabled() && sameOrigin(origin, space->webDavUrl());
                    if (!knownOrigin) { if (!props.isEmpty()) ++unmapped; return; }
                    path = slash >= 0 ? path.mid(slash) : QString();
                }
                path = normalized(path);
                const QString spaceHint = props.value(QStringLiteral("spaceid"));
                const QString legacy = withoutTrailingSlash(normalized(_state->account()->davUrl().path())) + QLatin1Char('/');
                const int personalCount = static_cast<int>(std::count_if(spaces.begin(), spaces.end(), [](const auto *space) {
                    return !space->disabled() && space->sortPriority() == 100;
                }));
                for (const auto *space : spaces) {
                    if (space->disabled()) continue;
                    QUrl root = space->webDavUrl(); root.setPath(withoutTrailingSlash(root.path()));
                    const QString prefix = normalized(root.path()) + QLatin1Char('/');
                    if (path.startsWith(prefix)) {
                        appendEntry(root.path() + QLatin1Char('/') + path.mid(prefix.size()), props, root, space->id(), true);
                        return;
                    }
                    // Older REPORT handlers can expose the CS3 space reference
                    // directly below /dav. Correlate it with the advertised drive.
                    const QString reference = QStringLiteral("/dav/") + space->driveId() + QLatin1Char('/');
                    if (!spaceHint.isEmpty() && spaceHint == space->driveId() && validName(space->driveId()) && path.startsWith(reference)) {
                        appendEntry(root.path() + QLatin1Char('/') + path.mid(reference.size()), props, root, space->id(), true);
                        return;
                    }
                    if (space->sortPriority() == 100 && personalCount == 1 && path.startsWith(legacy)
                        && (spaceHint.isEmpty() || spaceHint == space->driveId() || spaceHint == space->id())) {
                        appendEntry(root.path() + QLatin1Char('/') + path.mid(legacy.size()), props, root, space->id(), true);
                        return;
                    }
                }
                if (!props.isEmpty()) ++unmapped;
            });
            QHash<QString, qint64> sizes;
            success = parser.parse(job->reply()->readAll(), &sizes, {});
        }
        _emptyStateReady = success;
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
    if (!canBrowse() || _operation || _emptyingTrash || _download || !validPath(path) || root.host().isEmpty()
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
    const int timeoutSec = _uploading
        ? std::max<int>(300, static_cast<int>(body ? body->size() / (50 * 1024) : 300))
        : 60;
    job->setTimeout(std::chrono::seconds(timeoutSec));
    if (_uploading) {
        startUploadProgress(path.section(QLatin1Char('/'), -1), body ? body->size() : 0);
        job->addNewReplyHook([this, job](QNetworkReply *reply) {
            connect(reply, &QNetworkReply::uploadProgress, this, [this, job](qint64 sent, qint64 total) {
                Q_UNUSED(total);
                if (_operation == job && sent >= 0) updateUploadProgress(sent);
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
        if (_uploading) { stopUploadProgress(); _progress->hide(); _cancelButton->hide(); }
        _uploading = false;
        if (success && _root == viewRoot && _path == viewPath && _section == section) refresh();
        setMessage(result);
        updateActions();
        showResult(result, success);
        Q_EMIT operationFinished(success, result);
    });
    setMessage(_uploading ? tr("Uploading…") : tr("Working…"));
    job->start(); updateActions();
    return true;
}

void UDriveFileBrowser::cancelOperation()
{
    auto job = _operation; _operation.clear();
    const bool wasEmptying = _emptyingTrash;
    _emptyingTrash = false; _trashItems.clear(); _emptyTrashRoot.clear(); _emptyTrashAccount = {};
    if (job) job->abort();
    if (_uploading || wasEmptying) { _progress->hide(); _cancelButton->hide(); }
    if (_uploading) stopUploadProgress();
    _uploading = false;
    updateActions();
    if (job || wasEmptying) Q_EMIT operationFinished(false, tr("Operation canceled. Refresh to check the server."));
}

bool UDriveFileBrowser::createFolder(const QString &name)
{
    if (!_newFolderAction->isEnabled() || !validName(name)) return false;
    return runOperation("MKCOL", _root, _path.isEmpty() ? name : _path + QLatin1Char('/') + name, {}, tr("Folder created."));
}

void UDriveFileBrowser::startUploadProgress(const QString &name, qint64 total)
{
    _uploadName = name;
    _uploadTotal = std::max<qint64>(0, total);
    _uploadSent = 0;
    _uploadElapsed.start();
    _progress->setAccessibleName(tr("Upload progress"));
    _progress->setRange(0, 100);
    _progress->setValue(0);
    _progress->show();
    _cancelButton->setText(tr("Cancel upload"));
    _cancelButton->show();
    _message->hide();
    _uploadDetails->show();
    refreshUploadDetails();
    _uploadProgressTimer->start();
}

void UDriveFileBrowser::updateUploadProgress(qint64 sent)
{
    if (!_uploading || !_uploadElapsed.isValid()) return;
    // TUS callers supply the file offset plus bytes sent in the current chunk.
    // Repeated progress events and retries must never be added together.
    _uploadSent = std::clamp<qint64>(sent, 0, _uploadTotal);
    refreshUploadDetails();
}

void UDriveFileBrowser::refreshUploadDetails()
{
    if (!_uploading || !_uploadElapsed.isValid()) return;
    const qint64 elapsed = _uploadElapsed.elapsed();
    const int percent = _uploadTotal == 0 || _uploadSent == _uploadTotal ? 100
        : std::clamp(int(100.0 * _uploadSent / _uploadTotal), 0, 99);
    _progress->setValue(percent);
    const QLocale locale;
    QStringList details{
        tr("%1%").arg(locale.toString(percent)),
        tr("%1 of %2").arg(locale.formattedDataSize(_uploadSent), locale.formattedDataSize(_uploadTotal)),
        tr("%1 elapsed").arg(uploadDuration(elapsed))};
    QString remaining = tr("Estimating time remaining…");
    if (elapsed >= 1000 && _uploadSent > 0) {
        const qint64 speed = qint64(std::min(1000.0 * _uploadSent / elapsed, double(std::numeric_limits<qint64>::max() / 2)));
        details.insert(2, tr("%1/s").arg(locale.formattedDataSize(speed)));
        const double milliseconds = std::ceil(double(_uploadTotal - _uploadSent) * elapsed / _uploadSent);
        if (std::isfinite(milliseconds) && milliseconds < double(std::numeric_limits<qint64>::max() / 2)) {
            const qint64 rounded = ((qint64(milliseconds) + 999) / 1000) * 1000;
            remaining = tr("About %1 remaining").arg(uploadDuration(rounded));
        }
    }
    // Sending the request body is not confirmation that the server saved it.
    details.append(_uploadSent == _uploadTotal ? tr("Finalizing…") : remaining);
    _uploadDetails->setText(tr("Uploading “%1”\n%2").arg(_uploadName, details.join(QStringLiteral(" · "))));
}

void UDriveFileBrowser::stopUploadProgress()
{
    _uploadProgressTimer->stop();
    _uploadElapsed.invalidate();
    _uploadName.clear();
    _uploadSent = 0; _uploadTotal = 0;
    _uploadDetails->clear();
    _uploadDetails->hide();
}

bool UDriveFileBrowser::uploadFile(const QString &localFile)
{
    const QFileInfo info(localFile);
    if (!_uploadAction->isEnabled() || !info.isFile() || !validName(info.fileName())) return false;
    if (!canBrowse() || _operation || _download) return false;

    const QString fileName = info.fileName();
    const QString remotePath = _path.isEmpty() ? fileName : _path + QLatin1Char('/') + fileName;
    const qint64 totalSize = info.size();

    const auto &tus = _state->account()->capabilities().tusSupport();
    if (tus.isValid() && totalSize > 5LL * 1024 * 1024) {
        return uploadFileTus(localFile, remotePath, totalSize);
    }

    auto *file = new QFile(localFile);
    if (!file->open(QIODevice::ReadOnly)) { showResult(file->errorString(), false); delete file; return false; }
    QNetworkRequest request;
    request.setRawHeader("If-None-Match", "*");
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));
    request.setHeader(QNetworkRequest::ContentLengthHeader, totalSize);
    return runOperation("PUT", _root, remotePath, request, tr("File uploaded."), file);
}

bool UDriveFileBrowser::uploadFileTus(const QString &localFile, const QString &remotePath, qint64 totalSize)
{
    const auto &tus = _state->account()->capabilities().tusSupport();
    const quint64 maxChunk = tus.max_chunk_size;
    const quint64 chunkSize = (maxChunk > 0 && maxChunk <= 10ULL * 1024 * 1024) ? maxChunk : 10ULL * 1024 * 1024;

    _uploading = true;
    startUploadProgress(QFileInfo(localFile).fileName(), totalSize);
    setMessage(tr("Uploading…"));
    updateActions();

    const QUrl viewRoot = _root;
    const QString viewPath = _path;
    const Section section = _section;

    QNetworkRequest postReq;
    postReq.setRawHeader("Tus-Resumable", "1.0.0");
    postReq.setRawHeader("Upload-Length", QByteArray::number(totalSize));

    QByteArrayList meta;
    auto addMeta = [&meta](const QByteArray &k, const QByteArray &v) {
        meta << k + ' ' + v.toBase64();
    };
    const QString tusPath = remotePath.startsWith(QLatin1Char('/')) ? remotePath : (QLatin1Char('/') + remotePath);
    addMeta("filename", tusPath.toUtf8());
    addMeta("mtime", QByteArray::number(static_cast<qint64>(QFileInfo(localFile).lastModified().toSecsSinceEpoch())));
    postReq.setRawHeader("Upload-Metadata", meta.join(','));

    const bool creationWithUpload = tus.extensions.contains(QStringLiteral("creation-with-upload"));
    const quint64 firstChunk = creationWithUpload ? std::min<quint64>(chunkSize, totalSize) : 0;
    UploadDevice *firstDevice = creationWithUpload ? new UploadDevice(localFile, 0, firstChunk) : nullptr;
    if (firstDevice) {
        if (!firstDevice->open(QIODevice::ReadOnly)) {
            showResult(firstDevice->errorString(), false);
            delete firstDevice;
            _uploading = false;
            stopUploadProgress();
            _progress->hide();
            _cancelButton->hide();
            updateActions();
            return false;
        }
        postReq.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/offset+octet-stream"));
        postReq.setHeader(QNetworkRequest::ContentLengthHeader, QByteArray::number(firstChunk));
        postReq.setRawHeader("Upload-Offset", "0");
    }

    auto *postJob = new FileActionJob(_state->account(), _root, QString(), "POST", firstDevice, postReq, this);
    if (firstDevice) firstDevice->setParent(postJob);
    _operation = postJob;
    postJob->setTimeout(std::chrono::seconds(300));

    if (firstDevice) {
        postJob->addNewReplyHook([this, postJob, firstChunk](QNetworkReply *reply) {
            connect(reply, &QNetworkReply::uploadProgress, this, [this, postJob, firstChunk](qint64 sent, qint64) {
                if (_operation == postJob && sent >= 0)
                    updateUploadProgress(std::min<qint64>(sent, firstChunk));
            });
        });
    }

    auto sendChunkPtr = std::make_shared<std::function<void(quint64, const QUrl &)>>();

    *sendChunkPtr = [this, localFile, totalSize, chunkSize, viewRoot, viewPath, section, sendChunkPtr](quint64 offset, const QUrl &location) {
        if (!_uploading || offset >= static_cast<quint64>(totalSize)) {
            _operation.clear();
            _uploading = false;
            stopUploadProgress();
            _progress->hide();
            _cancelButton->hide();
            if (_root == viewRoot && _path == viewPath && _section == section) refresh();
            setMessage(tr("File uploaded."));
            updateActions();
            showResult(tr("File uploaded."), true);
            Q_EMIT operationFinished(true, tr("File uploaded."));
            return;
        }

        const quint64 thisChunk = std::min<quint64>(chunkSize, totalSize - offset);
        auto *device = new UploadDevice(localFile, offset, thisChunk);
        if (!device->open(QIODevice::ReadOnly)) {
            const QString err = tr("Upload failed: %1").arg(device->errorString());
            _operation.clear();
            _uploading = false;
            stopUploadProgress();
            _progress->hide();
            _cancelButton->hide();
            setMessage(err);
            updateActions();
            delete device;
            showResult(err, false);
            Q_EMIT operationFinished(false, err);
            return;
        }

        QNetworkRequest patchReq;
        patchReq.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/offset+octet-stream"));
        patchReq.setHeader(QNetworkRequest::ContentLengthHeader, QByteArray::number(thisChunk));
        patchReq.setRawHeader("Upload-Offset", QByteArray::number(offset));
        patchReq.setRawHeader("Tus-Resumable", "1.0.0");

        auto *patchJob = new FileActionJob(_state->account(), location, QString(), "PATCH", device, patchReq, this);
        device->setParent(patchJob);
        _operation = patchJob;
        patchJob->setTimeout(std::chrono::seconds(300));

        patchJob->addNewReplyHook([this, patchJob, offset, thisChunk](QNetworkReply *reply) {
            connect(reply, &QNetworkReply::uploadProgress, this, [this, patchJob, offset, thisChunk](qint64 sent, qint64) {
                if (_operation == patchJob && sent >= 0)
                    updateUploadProgress(offset + std::min<quint64>(sent, thisChunk));
            });
        });

        connect(patchJob, &AbstractNetworkJob::finishedSignal, this, [this, patchJob, sendChunkPtr, offset, thisChunk, location, totalSize] {
            if (_operation != patchJob) return;
            if (!patchJob->reply() || patchJob->reply()->error() != QNetworkReply::NoError
                || patchJob->httpStatusCode() < 200 || patchJob->httpStatusCode() >= 300) {
                const QString err = tr("Upload failed: %1 (HTTP %2)").arg(patchJob->errorString()).arg(patchJob->httpStatusCode());
                _operation.clear();
                _uploading = false;
                stopUploadProgress();
                _progress->hide();
                _cancelButton->hide();
                setMessage(err);
                updateActions();
                showResult(err, false);
                Q_EMIT operationFinished(false, err);
                return;
            }

            quint64 nextOffset = offset + thisChunk;
            if (patchJob->reply()->hasRawHeader("Upload-Offset")) {
                nextOffset = patchJob->reply()->rawHeader("Upload-Offset").toULongLong();
            }
            updateUploadProgress(std::min<quint64>(nextOffset, totalSize));
            (*sendChunkPtr)(nextOffset, location);
        });

        patchJob->start();
    };

    connect(postJob, &AbstractNetworkJob::finishedSignal, this, [this, postJob, firstChunk, totalSize, sendChunkPtr] {
        if (_operation != postJob) return;
        if (!postJob->reply() || postJob->reply()->error() != QNetworkReply::NoError
            || postJob->httpStatusCode() < 200 || postJob->httpStatusCode() >= 300) {
            const QString err = tr("Upload failed: %1 (HTTP %2)").arg(postJob->errorString()).arg(postJob->httpStatusCode());
            _operation.clear();
            _uploading = false;
            stopUploadProgress();
            _progress->hide();
            _cancelButton->hide();
            setMessage(err);
            updateActions();
            showResult(err, false);
            Q_EMIT operationFinished(false, err);
            return;
        }

        QUrl location = postJob->reply()->header(QNetworkRequest::LocationHeader).toUrl();
        if (location.isEmpty()) {
            const QString locHeader = QString::fromUtf8(postJob->reply()->rawHeader("Location"));
            location = QUrl(locHeader);
        }
        if (location.isRelative()) {
            location = _root.resolved(location);
        }

        quint64 serverOffset = firstChunk;
        if (postJob->reply()->hasRawHeader("Upload-Offset")) {
            serverOffset = postJob->reply()->rawHeader("Upload-Offset").toULongLong();
        }
        updateUploadProgress(std::min<quint64>(serverOffset, totalSize));
        (*sendChunkPtr)(serverOffset, location);
    });

    postJob->start();
    return true;
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

void UDriveFileBrowser::confirmEmptyTrash()
{
    if (_deleteConfirmation || !_emptyTrashButton->isEnabled() || !canBrowse()) return;
    const QUrl root = trashRoot();
    if (root.host().isEmpty() || !LocalDevelopment::allowsAuthenticatedUrl(root, _state->account()->url())
        || !root.query().isEmpty() || !root.fragment().isEmpty()) return;
    const auto accountId = _state->account()->uuid();
    QList<QPersistentModelIndex> rows;
    QList<QPair<QString, QString>> items;
    QSet<QString> paths;
    // Use the complete source listing, not the search-filtered proxy or selection.
    for (int row = 0; row < _model->rowCount(); ++row) {
        const auto item = _model->index(row, 0);
        const QString path = item.data(PathRole).toString();
        if (!validPath(path) || paths.contains(path) || !item.data(TrashedRole).toBool()) {
            showResult(tr("Deleted files changed. Refresh and try again."), false);
            return;
        }
        paths.insert(path);
        rows.append(QPersistentModelIndex(item));
        items.append({path, item.data(EtagRole).toString()});
    }
    if (items.isEmpty()) return;
    auto *dialog = new DeleteConfirmDialog(_spaceName, true, this, true, items.size());
    _deleteConfirmation = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::finished, this, [this, dialog] {
        if (_deleteConfirmation == dialog) _deleteConfirmation.clear();
    });
    connect(dialog, &QDialog::accepted, this, [this, root, accountId, rows, items] {
        if (!canBrowse() || !isVisible() || _section != Section::Trash || trashRoot() != root
            || _state->account()->uuid() != accountId || _listing || _report || _operation || _download
            || _emptyingTrash || !_emptyStateReady || _model->rowCount() != rows.size()) {
            showResult(tr("Deleted files changed. Review them and confirm again."), false);
            return;
        }
        for (int i = 0; i < rows.size(); ++i) {
            if (!rows.at(i).isValid() || rows.at(i).data(PathRole).toString() != items.at(i).first
                || rows.at(i).data(EtagRole).toString() != items.at(i).second) {
                showResult(tr("Deleted files changed. Review them and confirm again."), false);
                return;
            }
        }
        _trashItems = items;
        _trashPosition = 0;
        _emptyTrashRoot = root;
        _emptyTrashAccount = accountId;
        _emptyingTrash = true;
        _progress->setAccessibleName(tr("Empty trash progress"));
        _progress->setRange(0, items.size());
        _progress->setValue(0);
        _progress->show();
        _cancelButton->setText(tr("Stop"));
        _cancelButton->show();
        updateActions();
        deleteNextTrashItem();
    });
    dialog->open();
}

void UDriveFileBrowser::deleteNextTrashItem()
{
    if (!_emptyingTrash || _operation) return;
    if (!canBrowse() || !isVisible() || _section != Section::Trash || trashRoot() != _emptyTrashRoot
        || _state->account()->uuid() != _emptyTrashAccount) {
        finishEmptyTrash(false, tr("Empty trash stopped. Some items may already have been permanently deleted."));
        return;
    }
    if (_trashPosition == _trashItems.size()) {
        finishEmptyTrash(true, tr("Permanently deleted %n item(s).", nullptr, _trashPosition));
        return;
    }
    const auto item = _trashItems.at(_trashPosition);
    QNetworkRequest request;
    if (!item.second.isEmpty()) request.setRawHeader("If-Match", item.second.toUtf8());
    auto *job = new FileActionJob(_state->account(), _emptyTrashRoot, item.first, "DELETE", nullptr, request, this);
    _operation = job;
    job->setTimeout(std::chrono::seconds(60));
    connect(job, &AbstractNetworkJob::finishedSignal, this, [this, job] {
        if (_operation != job || !_emptyingTrash) return;
        bool success = job->reply() && job->reply()->error() == QNetworkReply::NoError
            && job->httpStatusCode() >= 200 && job->httpStatusCode() < 300;
        if (success && job->httpStatusCode() == 207) success = multistatusSucceeded(job->reply()->readAll());
        _operation.clear();
        if (!success) {
            finishEmptyTrash(false, tr("Could not empty trash (HTTP %1). %2 of %3 items were confirmed deleted. Some changes may have completed; remaining items were not processed.")
                .arg(job->httpStatusCode()).arg(_trashPosition).arg(_trashItems.size()));
            return;
        }
        ++_trashPosition;
        _progress->setValue(_trashPosition);
        setMessage(tr("Emptying trash… %1 of %2").arg(_trashPosition).arg(_trashItems.size()));
        QTimer::singleShot(0, this, &UDriveFileBrowser::deleteNextTrashItem);
    });
    setMessage(tr("Emptying trash… %1 of %2").arg(_trashPosition + 1).arg(_trashItems.size()));
    job->start();
}

void UDriveFileBrowser::finishEmptyTrash(bool success, const QString &message)
{
    const QUrl root = _emptyTrashRoot;
    const auto accountId = _emptyTrashAccount;
    _emptyingTrash = false;
    _trashItems.clear();
    _emptyTrashRoot.clear();
    _emptyTrashAccount = {};
    _progress->hide();
    _cancelButton->hide();
    if (canBrowse() && _section == Section::Trash && trashRoot() == root && _state->account()->uuid() == accountId) {
        refresh();
        if (_listing) connect(_listing, &PropfindJob::finishedWithoutError, this, [this, root, accountId, message] {
            if (canBrowse() && _section == Section::Trash && trashRoot() == root && _state->account()->uuid() == accountId) {
                setMessage(message);
                _resultMessage = true;
                _message->hide();
            }
        });
    }
    setMessage(message);
    updateActions();
    showResult(message, success);
    Q_EMIT operationFinished(success, message);
}

void UDriveFileBrowser::confirmDelete(bool permanent)
{
    auto *action = permanent ? _permanentDeleteAction : _deleteAction;
    if (_deleteConfirmation || !action->isEnabled() || !canBrowse() || _download) return;
    const QPersistentModelIndex item(selectedItem());
    if (!item.isValid()) return;
    const auto accountId = _state->account()->uuid();
    const auto section = _section;
    const QUrl root = permanent ? trashRoot() : selectedRoot();
    const QString path = item.data(PathRole).toString();
    const QString name = item.data().toString();
    const QString etag = item.data(EtagRole).toString();
    const bool directory = item.data(DirectoryRole).toBool();
    auto *dialog = new DeleteConfirmDialog(name, directory, this, permanent);
    _deleteConfirmation = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::finished, this, [this, dialog] {
        if (_deleteConfirmation == dialog) _deleteConfirmation.clear();
    });
    // No nested event loop or stack-owned QObject: browser/account destruction
    // safely deletes the dialog and disconnects this completion callback.
    connect(dialog, &QDialog::accepted, this, [this, permanent, item, accountId, section, root, path, name, etag, directory] {
        const auto current = selectedItem();
        const auto *action = permanent ? _permanentDeleteAction : _deleteAction;
        if (!canBrowse() || !isVisible() || !action->isEnabled() || _download || _section != section
            || _state->account()->uuid() != accountId || !item.isValid() || current != item
            || (permanent ? trashRoot() : selectedRoot()) != root || current.data(PathRole).toString() != path
            || current.data().toString() != name || current.data(EtagRole).toString() != etag
            || current.data(DirectoryRole).toBool() != directory) {
            showResult(tr("The selected item changed. Review it and confirm again."), false);
            return;
        }
        QNetworkRequest request;
        if (!etag.isEmpty()) request.setRawHeader("If-Match", etag.toUtf8());
        runOperation("DELETE", root, path, request,
            permanent ? tr("Item permanently deleted.") : tr("Item moved to Deleted files."));
    });
    dialog->open();
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
    const QPersistentModelIndex item = _proxy->mapToSource(selectedItem());
    const bool previous = item.data(FavoriteRole).toBool();
    _model->setData(item, favorite, FavoriteRole);
    // Restore the server-confirmed state on errors and cancellations, including
    // synchronous failures to start the operation.
    const auto rollback = connect(this, &UDriveFileBrowser::operationFinished, this, [this, item, previous](bool success) {
        if (!success && item.isValid()) {
            _model->setData(item, previous, FavoriteRole);
            updateActions();
        }
    }, Qt::SingleShotConnection);
    auto *body = new QBuffer;
    body->setData(QByteArray("<?xml version=\"1.0\"?><d:propertyupdate xmlns:d=\"DAV:\" xmlns:oc=\"http://owncloud.org/ns\"><d:set><d:prop><oc:favorite>")
        + (favorite ? "true" : "false") + "</oc:favorite></d:prop></d:set></d:propertyupdate>");
    body->open(QIODevice::ReadOnly);
    QNetworkRequest request;
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/xml; charset=utf-8"));
    const bool started = runOperation("PROPPATCH", selectedRoot(), selectedItem().data(PathRole).toString(), request,
        favorite ? tr("Added to Favorites.") : tr("Removed from Favorites."), body);
    if (!started) {
        disconnect(rollback);
        if (item.isValid()) _model->setData(item, previous, FavoriteRole);
        updateActions();
    }
    return started;
}

bool UDriveFileBrowser::previewSelectedPdf()
{
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    const auto index = selectedItem();
    if (!index.isValid() || index.data(ShareUnavailableRole).toBool() || index.data(DirectoryRole).toBool() || _listing || _report || _operation || _download
        || !canBrowse() || _section == Section::Trash || !index.data().toString().endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive)
        || UDriveFilePreview::hasSuspiciousDoubleExtension(index.data().toString())) return false;
    closePdf();
    const QString local = localPath(index.data(PathRole).toString(), selectedRoot());
    if (!local.isEmpty()) {
        _pages->setCurrentWidget(_pdfPreview);
        _pdfPreview->openFile(local, index.data().toString());
        _pdfPreview->findChild<QPushButton *>(QStringLiteral("pdfBack"))->setFocus();
        recordOpened(selectedRoot(), index.data(PathRole).toString());
        return true;
    }
    const QString key = previewCacheKey(index);
    const auto cached = _filePreviewCache.constFind(key);
    if (!key.isEmpty() && cached != _filePreviewCache.cend() && QFileInfo::exists(cached->path)) {
        _pages->setCurrentWidget(_pdfPreview);
        _pdfPreview->openFile(cached->path, index.data().toString(), cached->storage);
        recordOpened(selectedRoot(), index.data(PathRole).toString());
        return true;
    }
    _filePreviewKey = key;
    _previewStorage = createPreviewStorage(QStringLiteral("pdf"));
    if (!_previewStorage) { showResult(tr("Could not create a temporary PDF preview."), false); return true; }
    const QString destination = _previewStorage->filePath(QStringLiteral("preview.pdf"));
    if (!startDownload(destination, true)) _previewStorage.reset();
    return true;
#else
    return false;
#endif
}

QString UDriveFileBrowser::previewCacheKey(const QModelIndex &index) const
{
    const QString etag = index.data(EtagRole).toString();
    if (etag.isEmpty()) return {}; // Without a server version, always fetch fresh content.
    QByteArray identity = selectedRoot().toEncoded();
    identity.append('\n').append(index.data(PathRole).toString().toUtf8()).append('\n').append(etag.toUtf8());
    return QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
}

void UDriveFileBrowser::cleanupOrphanedPreviews()
{
    const QString root = previewStorageRoot();
    if (root.isEmpty()) return;
    QLockFile maintenance(root + QStringLiteral("/maintenance.lock"));
    if (!maintenance.tryLock()) return;
    const QDir dir(root);
    for (const auto &entry : dir.entryInfoList({QStringLiteral("session-*")}, QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!privatePreviewDirectory(entry.absoluteFilePath())) continue;
        const QString lockPath = QDir(entry.absoluteFilePath()).filePath(QStringLiteral("active.lock"));
        const QFileInfo lockInfo(lockPath);
        if (lockInfo.isSymLink() || lockInfo.isJunction()) continue;
        QLockFile active(lockPath);
        active.setStaleLockTime(0);
        if (!active.tryLock()) continue;
        active.unlock();
        removePreviewDirectory(entry.absoluteFilePath());
    }
}

void UDriveFileBrowser::rememberPreview(const QString &key, const QString &path, const std::shared_ptr<QTemporaryDir> &storage)
{
    if (key.isEmpty()) return;
    _filePreviewCache.insert(key, {path, storage, QFileInfo(path).size()});
    _previewCacheOrder.removeAll(key); _previewCacheOrder.append(key);
    auto totalSize = [this] { qint64 size = 0; for (const auto &entry : _filePreviewCache) size += entry.bytes; return size; };
    while (_previewCacheOrder.size() > 4 || totalSize() > 512LL * 1024 * 1024)
        _filePreviewCache.remove(_previewCacheOrder.takeFirst());
}

bool UDriveFileBrowser::previewSelectedFile()
{
    const auto index = selectedItem();
    if (!index.isValid() || index.data(DirectoryRole).toBool() || index.data(ShareUnavailableRole).toBool()
        || _listing || _report || _operation || _download || !canBrowse() || _section == Section::Trash) return false;
    closePdf();
    const QString name = index.data().toString();
    const QString path = index.data(PathRole).toString();
    const QUrl root = selectedRoot();
    _filePreview->showLoading(name);
    _pages->setCurrentWidget(_filePreview);
    const auto kind = UDriveFilePreview::kindForName(name);
    if (kind == UDriveFilePreview::Kind::Unsupported) {
        _filePreview->showError(tr("Preview is not available for this file type. Use Download to save a copy."));
        return true;
    }
    const QString local = localPath(path, root);
    if (!local.isEmpty()) { showPreparedFile(local, name, root, path, {}); return true; }
    const QString key = previewCacheKey(index);
    const auto cached = _filePreviewCache.constFind(key);
    if (!key.isEmpty() && cached != _filePreviewCache.cend() && QFileInfo::exists(cached->path)) {
        const auto file = *cached;
        _previewCacheOrder.removeAll(key); _previewCacheOrder.append(key);
        showPreparedFile(file.path, name, root, path, file.storage);
        return true;
    }
    _previewStorage = createPreviewStorage(QStringLiteral("preview"));
    if (!_previewStorage) {
        _filePreview->showError(tr("Could not create a temporary preview.")); return true;
    }
    // Keep the real extension for the media decoder and associated Office app.
    const QString suffix = QFileInfo(name).suffix().toLower();
    const QString destination = _previewStorage->filePath(QStringLiteral("preview.") + suffix);
    _filePreviewDownload = true;
    _filePreviewKey = key;
    if (!startDownload(destination, false)) { _filePreviewDownload = false; _previewStorage.reset(); }
    return true;
}

void UDriveFileBrowser::showPreparedFile(const QString &path, const QString &name, const QUrl &root, const QString &remotePath, std::shared_ptr<QTemporaryDir> storage)
{
    if (UDriveFilePreview::kindForName(name) == UDriveFilePreview::Kind::External) {
        // Synced originals remain editable. Temporary Office copies are read-only.
        if (storage && !QFile::setPermissions(path, QFileDevice::ReadOwner)) {
            _filePreview->showError(tr("Could not prepare a read-only copy of this file.")); return;
        }
        _previewStorage = storage;
        const QPointer<UDriveFileBrowser> guard(this);
        const bool opened = QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        if (!guard) return;
        _pages->setCurrentIndex(0);
        if (opened) { recordOpened(root, remotePath); showNotice(storage ? tr("Opened a read-only copy. Sync this folder to edit files and upload changes automatically.") : tr("File opened."), Notice::Information); }
        else showResult(tr("No application could open this file. Install an application for this file type, or use Download."), false);
    } else {
        _pages->setCurrentWidget(_filePreview);
        _filePreview->openFile(path, name, std::move(storage));
        _filePreview->findChild<QPushButton *>(QStringLiteral("previewBack"))->setFocus();
        recordOpened(root, remotePath);
    }
}

void UDriveFileBrowser::closePdf()
{
    if (_previewDownload || _filePreviewDownload) cancelDownload();
    _filePreview->clear();
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    _pdfPreview->clear();
#endif
    _previewStorage.reset();
    _pages->setCurrentIndex(0);
}

void UDriveFileBrowser::hideEvent(QHideEvent *event)
{
    showFileDetailsActions(false);
    auto details = _shareItemJob;
    _shareItemJob.clear();
    if (details) details->abort();
    if (_versionsJob) {
        _versionsJob->abort();
        _versionsJob.clear();
    }
    _workspaceActions->cancel();
    for (auto *dialog : findChildren<QDialog *>()) if (dialog->property("udriveWorkspaceDialog").toBool()) dialog->close();
    _checkboxViewport.clear();
    if (_deleteConfirmation) _deleteConfirmation->reject();
    if (_emptyingTrash) cancelOperation();
    _thumbnailTimer->stop();
    _thumbnails->cancel();
    closePdf();
    _filePreviewCache.clear(); _previewCacheOrder.clear();
    QWidget::hideEvent(event);
}

void UDriveFileBrowser::cancelDownload()
{
    auto job = _download;
    _download.clear();
    if (job) job->abort();
    if (_downloadFile) _downloadFile->cancelWriting();
    _downloadFile.reset();
    if (_previewDownload || _filePreviewDownload) _previewStorage.reset();
    _previewDownload = false;
    _filePreviewDownload = false;
    _progress->hide();
    _cancelButton->hide();
    updateActions();
    if (job) Q_EMIT downloadFinished(false, _destination);
}

void UDriveFileBrowser::downloadSelection()
{
    chooseDownloadDestination(false);
}

void UDriveFileBrowser::chooseDownloadDestination(bool openAfterDownload)
{
    const auto index = selectedItem();
    if (!index.isValid() || index.data(ShareUnavailableRole).toBool() || index.data(DirectoryRole).toBool() || _listing || _report || _operation || _download || !canBrowse()) return;
    const QString path = index.data(PathRole).toString();
    const QUrl root = _root;
    const QString currentPath = _path;
    const QUrl entryRoot = selectedRoot();
    const QPointer<UDriveFileBrowser> guard(this);
    const QString destination = QFileDialog::getSaveFileName(this, openAfterDownload ? tr("Download and open file") : tr("Save file"),
        QDir(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)).filePath(index.data().toString()));
    if (!guard || destination.isEmpty() || !canBrowse() || _root != root || _path != currentPath || _download
        || selectedRoot() != entryRoot || selectedItem().data(PathRole).toString() != path) return;
    saveSelectedFile(destination, openAfterDownload);
}

bool UDriveFileBrowser::saveSelectedFile(const QString &destination, bool openAfterDownload)
{
    return startDownload(destination, false, openAfterDownload);
}

bool UDriveFileBrowser::startDownload(const QString &destination, bool preview, bool openAfterDownload)
{
    const auto index = selectedItem();
    if (destination.isEmpty() || !index.isValid() || index.data(ShareUnavailableRole).toBool() || index.data(DirectoryRole).toBool() || _listing || _report || _operation || _download
        || !canBrowse() || _section == Section::Trash) return false;
    const QString path = index.data(PathRole).toString();
    _destination = destination;
    _previewDownload = preview;
    const bool filePreview = _filePreviewDownload;
    const QString cacheKey = _filePreviewKey;
    const qint64 maximumSize = preview ? 100LL * 1024 * 1024
        : filePreview ? UDriveFilePreview::maximumBytes(UDriveFilePreview::kindForName(index.data().toString())) : std::numeric_limits<qint64>::max();
    const QString tooLarge = preview ? tr("This PDF is too large to preview (maximum 100 MiB).") : tr("This file is too large to preview. Use Download to save a copy.");
    _writeError.clear();
    QNetworkRequest request;
    if ((preview || filePreview) && !index.data(EtagRole).toString().isEmpty()) request.setRawHeader("If-Match", index.data(EtagRole).toString().toUtf8());
    const QString name = index.data().toString();
    const QUrl root = selectedRoot();
    auto *job = new SimpleNetworkJob(_state->account(), selectedRoot(), path, "GET", nullptr, request, this);
    _download = job;
    job->setTimeout(std::chrono::seconds(60));
    job->addNewReplyHook([this, job, maximumSize, tooLarge](QNetworkReply *reply) {
        if (_download != job) return;
        // Start a fresh atomic save on every authentication retry/redirect.
        _downloadFile = std::make_unique<QSaveFile>(_destination);
        if (!_downloadFile->open(QIODevice::WriteOnly)) {
            _writeError = _downloadFile->errorString();
            QTimer::singleShot(0, job, &AbstractNetworkJob::abort);
            return;
        }
        if ((_previewDownload || _filePreviewDownload) && !_downloadFile->setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
            _writeError = tr("Could not secure the temporary preview file.");
            QTimer::singleShot(0, job, &AbstractNetworkJob::abort);
            return;
        }
        connect(reply, &QNetworkReply::metaDataChanged, this, [this, job, reply, maximumSize, tooLarge] {
            if (_download != job || job->reply() != reply) return;
            if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200
                && reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() > maximumSize) {
                _writeError = tooLarge;
                job->abort();
            }
        });
        connect(reply, &QNetworkReply::readyRead, this, [this, job, reply, maximumSize, tooLarge] {
            if (_download != job || job->reply() != reply || !_downloadFile || !_writeError.isEmpty()) return;
            if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) return;
            const QByteArray chunk = reply->readAll();
            if (_downloadFile->pos() + chunk.size() > maximumSize) {
                _writeError = tooLarge;
                job->abort(); return;
            }
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
    connect(job, &AbstractNetworkJob::finishedSignal, this, [this, job, preview, name, root, path, destination, openAfterDownload, filePreview, cacheKey, maximumSize, tooLarge] {
        if (_download != job) return;
        bool success = job->reply() && job->reply()->error() == QNetworkReply::NoError
            && job->httpStatusCode() == 200 && _writeError.isEmpty() && _downloadFile;
        if (success) {
            const QByteArray remaining = job->reply()->readAll();
            if (_downloadFile->pos() + remaining.size() > maximumSize) {
                success = false;
                _writeError = tooLarge;
            }
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
        _filePreviewDownload = false;
        if (filePreview) {
            if (success) {
                const auto storage = _previewStorage;
                rememberPreview(cacheKey, destination, storage);
                const QPointer<UDriveFileBrowser> guard(this);
                showPreparedFile(destination, name, root, path, storage);
                if (!guard) return;
            } else {
                _previewStorage.reset();
                _filePreview->showError(tr("Preview failed: %1").arg(error));
            }
            updateActions();
            Q_EMIT downloadFinished(success, destination);
            return;
        }
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
        if (preview && success) {
            rememberPreview(cacheKey, destination, _previewStorage);
            recordOpened(root, path);
            _pages->setCurrentWidget(_pdfPreview);
            _pdfPreview->openFile(_destination, name, _previewStorage);
            _pdfPreview->findChild<QPushButton *>(QStringLiteral("pdfBack"))->setFocus();
        } else if (preview) _previewStorage.reset();
#endif
        QString message = success ? preview ? tr("PDF opened in UDrive.") : tr("Saved to %1").arg(QDir::toNativeSeparators(destination)) : tr("Download failed: %1").arg(error);
        if (success && openAfterDownload) {
            const QPointer<UDriveFileBrowser> guard(this);
            const bool opened = QDesktopServices::openUrl(QUrl::fromLocalFile(destination));
            if (!guard) return;
            if (opened) {
                recordOpened(root, path);
                message = tr("Opened %1. The downloaded copy is saved to %2.").arg(name, QDir::toNativeSeparators(destination));
            } else {
                message = tr("Saved to %1, but no application could open this file. Choose a default application for this file type.").arg(QDir::toNativeSeparators(destination));
            }
        }
        if (preview && success) setMessage(message);
        else showResult(message, success);
        updateActions();
        Q_EMIT downloadFinished(success, destination);
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

UDriveIncomingSharesNotifier::UDriveIncomingSharesNotifier(AccountState *accountState, QObject *parent)
    : QObject(parent)
    , _accountState(accountState)
{
    _pollTimer = new QTimer(this);
    _pollTimer->setSingleShot(true);
    _pollTimer->setInterval(std::chrono::seconds(30));
    connect(_pollTimer, &QTimer::timeout, this, &UDriveIncomingSharesNotifier::poll);

    loadReadState();

    if (_accountState) {
        connect(_accountState, &AccountState::stateChanged, this, &UDriveIncomingSharesNotifier::onAccountStateChanged);
        connect(_accountState, &QObject::destroyed, this, [this] {
            _pollTimer->stop();
            if (_pollJob) {
                auto *job = _pollJob.data();
                _pollJob.clear();
                disconnect(job, nullptr, this, nullptr);
                job->abort();
            }
        });
        if (_accountState->account() && _accountState->account()->spacesManager()) {
            connect(_accountState->account()->spacesManager(), &GraphApi::SpacesManager::ready, this, &UDriveIncomingSharesNotifier::checkNow);
        }
    }

    if (_accountState && !_accountState->isSignedOut() && _accountState->account()
        && _accountState->account()->credentials() && _accountState->account()->credentials()->ready()) {
        _pollTimer->start(100);
    }
}

UDriveIncomingSharesNotifier::~UDriveIncomingSharesNotifier()
{
    _pollTimer->stop();
    if (_pollJob) {
        auto *job = _pollJob.data();
        _pollJob.clear();
        disconnect(job, nullptr, this, nullptr);
        job->abort();
    }
}

bool UDriveIncomingSharesNotifier::hasBaseline() const
{
    return _initialFetchDone;
}

bool UDriveIncomingSharesNotifier::isInFlight() const
{
    return !_pollJob.isNull();
}

void UDriveIncomingSharesNotifier::recordKnownShareIds(const QSet<QString> &ids)
{
    _knownShareIds.unite(ids);
    _initialFetchDone = true;
}

void UDriveIncomingSharesNotifier::loadReadState()
{
    if (!_accountState || !_accountState->account())
        return;
    const QString key = QStringLiteral("UDrive/ReadShareIds/") + _accountState->account()->uuid().toString(QUuid::WithoutBraces);
    auto settings = ConfigFile::makeQSettings();
    const auto list = settings.value(key).toStringList();
    _readShareIds = QSet<QString>(list.begin(), list.end());
}

void UDriveIncomingSharesNotifier::saveReadState()
{
    if (!_accountState || !_accountState->account())
        return;
    const QString key = QStringLiteral("UDrive/ReadShareIds/") + _accountState->account()->uuid().toString(QUuid::WithoutBraces);
    auto settings = ConfigFile::makeQSettings();
    QStringList list;
    for (const auto &id : _readShareIds) {
        if (!_initialFetchDone || _knownShareIds.contains(id))
            list.append(id);
    }
    settings.setValue(key, list);
}

QList<UDriveShareNotification> UDriveIncomingSharesNotifier::notifications() const
{
    return _notifications;
}

int UDriveIncomingSharesNotifier::unreadCount() const
{
    int count = 0;
    for (const auto &n : _notifications) {
        if (!n.isRead)
            ++count;
    }
    return count;
}

void UDriveIncomingSharesNotifier::markAllAsRead()
{
    bool changed = false;
    for (auto &n : _notifications) {
        if (!n.isRead) {
            n.isRead = true;
            _readShareIds.insert(n.id);
            changed = true;
        }
    }
    if (changed) {
        saveReadState();
        Q_EMIT unreadCountChanged(0);
        Q_EMIT notificationsChanged();
    }
}

void UDriveIncomingSharesNotifier::markAsRead(const QString &id)
{
    bool changed = false;
    for (auto &n : _notifications) {
        if (n.id == id && !n.isRead) {
            n.isRead = true;
            _readShareIds.insert(id);
            changed = true;
            break;
        }
    }
    if (changed) {
        saveReadState();
        Q_EMIT unreadCountChanged(unreadCount());
        Q_EMIT notificationsChanged();
    }
}

void UDriveIncomingSharesNotifier::checkNow()
{
    if (_pollJob) {
        _recheckRequested = true;
        return;
    }
    if (_pollTimer)
        _pollTimer->stop();
    poll();
}

void UDriveIncomingSharesNotifier::onAccountStateChanged()
{
    if (!_accountState || _accountState->isSignedOut()
        || !_accountState->account() || !_accountState->account()->credentials()
        || !_accountState->account()->credentials()->ready()) {
        _recheckRequested = false;
        _pollTimer->stop();
        if (_pollJob) {
            auto *job = _pollJob.data();
            _pollJob.clear();
            disconnect(job, nullptr, this, nullptr);
            job->abort();
        }
        return;
    }

    if (_apiAvailable && !_pollJob && !_pollTimer->isActive()) {
        _pollTimer->start(100);
    }
}

void UDriveIncomingSharesNotifier::poll()
{
    if (!_accountState || _accountState->isSignedOut()
        || !_accountState->account() || !_accountState->account()->credentials()
        || !_accountState->account()->credentials()->ready())
        return;

    if (!_apiAvailable || _pollJob)
        return;

    auto *job = new JsonJob(_accountState->account(), _accountState->account()->url(),
        QStringLiteral("graph/v1beta1/me/drive/sharedWithMe"), "GET", SimpleNetworkJob::UrlQuery{}, QNetworkRequest{}, this);
    _pollJob = job;
    job->setTimeout(std::chrono::seconds(30));
    job->addNewReplyHook([job](QNetworkReply *reply) {
        QObject::connect(reply, &QIODevice::readyRead, job, [job, reply] {
            if (reply->bytesAvailable() > 4 * 1024 * 1024) job->abort();
        });
    });
    connect(job, &JsonJob::finishedSignal, this, &UDriveIncomingSharesNotifier::onJobFinished);
    job->start();
}

static QString sanitizeNotificationText(const QString &raw)
{
    QString clean = raw.trimmed().simplified();
    clean.remove(QRegularExpression(QStringLiteral("[\\p{Cc}\\x{202a}-\\x{202e}\\x{2066}-\\x{2069}]")));
    if (clean.size() > 128)
        clean = clean.left(127) + QStringLiteral("…");
    return clean;
}

void UDriveIncomingSharesNotifier::onJobFinished()
{
    auto *job = _pollJob.data();
    _pollJob.clear();
    const bool recheck = _recheckRequested;
    _recheckRequested = false;

    if (!job || !job->reply())
        return;

    if (!_accountState || _accountState->isSignedOut()
        || !_accountState->account() || !_accountState->account()->credentials()
        || !_accountState->account()->credentials()->ready()) {
        return;
    }

    if (job->httpStatusCode() == 404 || job->httpStatusCode() == 405 || job->httpStatusCode() == 501) {
        _apiAvailable = false;
        _pollTimer->stop();
        return;
    }

    if (_apiAvailable) {
        _pollTimer->start(std::chrono::seconds(30));
    }

    if (job->httpStatusCode() != 200 || job->reply()->error() != QNetworkReply::NoError) {
        return;
    }

    const auto value = job->data().value(QStringLiteral("value"));
    if (!value.isArray())
        return;

    loadReadState();
    QSet<QString> currentIds;
    QList<QPair<QString, QString>> newShares;
    QStringList newFileNames;
    QStringList newSharers;
    QList<UDriveShareNotification> parsedNotifications;

    for (const auto &entry : value.toArray()) {
        const auto item = entry.toObject();
        const QString id = item.value(QStringLiteral("id")).toString();
        const QString name = item.value(QStringLiteral("name")).toString();
        if (id.isEmpty() || name.isEmpty() || !validName(name) || !item.value(QStringLiteral("remoteItem")).isObject())
            continue;

        const bool hidden = item.value(QStringLiteral("@UI.Hidden")).toBool(item.value(QStringLiteral("@UI.hidden")).toBool(false));
        if (hidden)
            continue;

        currentIds.insert(id);

        const auto creators = shareCreators(item, true);
        const QString sharer = creators.isEmpty() ? QString() : creators.values().first();
        const auto remote = item.value(QStringLiteral("remoteItem")).toObject();
        const auto modified = QDateTime::fromString(remote.value(QStringLiteral("lastModifiedDateTime")).toString(), Qt::ISODate);
        const QDateTime timestamp = modified.isValid() ? modified.toUTC() : QDateTime::currentDateTimeUtc();

        UDriveShareNotification notif;
        notif.id = id;
        notif.fileName = name;
        notif.sharerName = sharer;
        notif.sharerInitials = personInitials(sharer);
        notif.timestamp = timestamp;
        notif.isRead = _readShareIds.contains(id);
        parsedNotifications.append(notif);

        if (_initialFetchDone && !_knownShareIds.contains(id)) {
            newShares.append(qMakePair(name, sharer));
            newFileNames.append(name);
            newSharers.append(sharer);
        }
    }

    std::sort(parsedNotifications.begin(), parsedNotifications.end(), [](const auto &a, const auto &b) {
        return a.timestamp > b.timestamp;
    });
    _notifications = parsedNotifications;

    if (!_initialFetchDone) {
        _knownShareIds = currentIds;
        _initialFetchDone = true;
        Q_EMIT notificationsChanged();
        Q_EMIT unreadCountChanged(unreadCount());
        if (recheck) {
            QTimer::singleShot(0, this, &UDriveIncomingSharesNotifier::poll);
        }
        return;
    }

    if (!currentIds.isEmpty() || _knownShareIds.isEmpty()) {
        _knownShareIds = currentIds;
    }

    if (!newShares.isEmpty()) {
        dispatchNotifications(newShares);
        Q_EMIT newSharesDetected(newFileNames, newSharers);
        Q_EMIT incomingSharesUpdated();
    }

    Q_EMIT notificationsChanged();
    Q_EMIT unreadCountChanged(unreadCount());

    if (recheck) {
        QTimer::singleShot(0, this, &UDriveIncomingSharesNotifier::poll);
    }
}

void UDriveFileBrowser::navigateToSharesTab()
{
    setSection(Section::Shares);
    setShareTab(ShareTab::WithMe);
}

void UDriveIncomingSharesNotifier::dispatchNotifications(const QList<QPair<QString, QString>> &newShares)
{
    if (newShares.isEmpty() || !Application::hasInstance() || !ocApp()->gui())
        return;

    const QString appName = Theme::instance()->appNameGUI();
    const QIcon shareIcon = UDriveIcons::icon(UDriveIcons::Symbol::Share);

    if (newShares.size() == 1) {
        const auto &share = newShares.first();
        const QString name = sanitizeNotificationText(share.first);
        const QString sharer = sanitizeNotificationText(share.second);
        const QString message = sharer.isEmpty()
            ? tr("Someone shared '%1' with you").arg(name)
            : tr("%1 shared '%2' with you").arg(sharer, name);
        ocApp()->gui()->slotShowOptionalTrayMessage(appName, message, shareIcon);
    } else if (newShares.size() <= 3) {
        for (const auto &share : newShares) {
            const QString name = sanitizeNotificationText(share.first);
            const QString sharer = sanitizeNotificationText(share.second);
            const QString message = sharer.isEmpty()
                ? tr("Someone shared '%1' with you").arg(name)
                : tr("%1 shared '%2' with you").arg(sharer, name);
            ocApp()->gui()->slotShowOptionalTrayMessage(appName, message, shareIcon);
        }
    } else {
        const auto &firstShare = newShares.first();
        const QString name = sanitizeNotificationText(firstShare.first);
        const QString sharer = sanitizeNotificationText(firstShare.second);
        const int others = newShares.size() - 1;
        const QString message = !sharer.isEmpty()
            ? tr("%1 shared '%2' and %n other item(s) with you", nullptr, others).arg(sharer, name)
            : tr("%n new item(s) were shared with you", nullptr, newShares.size());
        ocApp()->gui()->slotShowOptionalTrayMessage(appName, message, shareIcon);
    }
}
}
