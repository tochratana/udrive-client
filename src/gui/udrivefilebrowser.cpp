// SPDX-License-Identifier: GPL-2.0-or-later
#include "udrivefilebrowser.h"
#include "udriveicons.h"
#include "udrivecombobox.h"
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
#include "libsync/networkjobs/jsonjob.h"
#include "common/utility.h"

#include <QComboBox>
#include <QCompleter>
#include <QCheckBox>
#include <QAction>
#include <QScrollArea>
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
    PermissionsRole, FavoriteRole, FavoriteKnownRole, EtagRole, LinkRole, RestorePathRole, ThumbnailRole, OpenedRole, TrashedRole };

enum RowAction { RowFavorite, RowShare, RowLink, RowMore };

QMenu *createFileMenu(QWidget *parent)
{
    auto *menu = new QMenu(parent);
    // Let the stylesheet draw the outline without a second native window frame.
    menu->setWindowFlag(Qt::FramelessWindowHint);
    menu->setAttribute(Qt::WA_TranslucentBackground);
    return menu;
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
        size.setHeight(qMax(_tree->property("driveCondensed").toBool() ? 30 : 42, size.height()));
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
                          std::function<void(const QModelIndex &)> moreAction,
                          std::function<void(const QModelIndex &)> toggleSelect)
        : QStyledItemDelegate(view)
        , _memberAction(std::move(memberAction))
        , _moreAction(std::move(moreAction))
        , _toggleSelect(std::move(toggleSelect))
    {
    }

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override
    {
        return QSize(224, 184);
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

        const QColor bg = selected ? (dark ? QColor("#223B2A") : QColor("#E3F6E9"))
            : hover                ? (dark ? QColor("#333842") : QColor("#F0F4F2"))
                                   : (dark ? QColor("#282C34") : QColor("#FFFFFF"));
        const QColor border = selected ? QColor("#40D979")
            : hover                    ? (dark ? QColor("#4E5562") : QColor("#CCD3D9"))
                                       : (dark ? QColor("#3B404A") : QColor("#E1E5E8"));

        painter->setPen(QPen(border, selected ? 2 : 1));
        painter->setBrush(bg);
        painter->drawRoundedRect(card.adjusted(1, 1, -1, -1), 12, 12);

        const QRect check(card.left() + 12, card.top() + 12, 18, 18);
        painter->setPen(QPen(selected ? QColor("#40D979") : QColor(dark ? "#5A6270" : "#A2ABB5"), 1.5));
        painter->setBrush(selected ? QColor("#40D979") : Qt::transparent);
        painter->drawRoundedRect(check, 4, 4);
        if (selected) {
            painter->setPen(QPen(QColor("#143B20"), 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter->drawLine(check.topLeft() + QPoint(4, 9), check.topLeft() + QPoint(8, 13));
            painter->drawLine(check.topLeft() + QPoint(8, 13), check.topLeft() + QPoint(14, 5));
        }

        const QRect iconRect(card.center().x() - 26, card.top() + 34, 52, 52);
        const QColor gridColor = dark ? QColor("#C8D0D8") : QColor("#4A525A");
        UDriveIcons::icon(UDriveIcons::Symbol::Grid, false, gridColor).paint(painter, iconRect);

        const QRect titleRect(card.left() + 14, card.bottom() - 32, card.width() - 76, 22);
        painter->setPen(QColor(dark ? "#FFFFFF" : "#1A201C"));
        auto font = option.font;
        font.setWeight(QFont::DemiBold);
        font.setPointSizeF(qMax(10.0, font.pointSizeF()));
        painter->setFont(font);
        painter->drawText(titleRect, Qt::AlignVCenter | Qt::AlignLeft, painter->fontMetrics().elidedText(index.data().toString(), Qt::ElideRight, titleRect.width()));

        const QColor actionColor = dark ? QColor("#8E98A4") : QColor("#6B7580");
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
                if (checkRect(option.rect).adjusted(-4, -4, 4, 4).contains(pt)) {
                    if (_toggleSelect) _toggleSelect(index);
                    return true;
                }
                if (memberRect(option.rect).adjusted(-4, -4, 4, 4).contains(pt)) {
                    if (_memberAction) _memberAction(index);
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
    static QRect checkRect(const QRect &rect)
    {
        const QRect card = rect.adjusted(6, 6, -6, -6);
        return QRect(card.left() + 12, card.top() + 12, 18, 18);
    }
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
    std::function<void(const QModelIndex &)> _toggleSelect;
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

DeleteConfirmDialog::DeleteConfirmDialog(const QString &fileName, bool isFolder, QWidget *parent, bool permanent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("deleteConfirmDialog"));
    setWindowTitle(permanent ? tr("Delete permanently") : tr("Move to Deleted files"));
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

    auto *titleLabel = new QLabel((permanent ? tr("Permanently delete “%1”?") : tr("Move “%1” to Deleted files?")).arg(fileName), this);
    titleLabel->setObjectName(QStringLiteral("confirmTitle"));
    titleLabel->setTextFormat(Qt::PlainText);
    titleLabel->setWordWrap(true);
    textLayout->addWidget(titleLabel);

    const QString descText = permanent
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

    auto *deleteButton = new QPushButton(permanent ? tr("Delete permanently") : tr("Delete"), this);
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

UDriveShareDialog::UDriveShareDialog(AccountState *accountState, const QString &fileName, bool isFolder,
                                     const QString &remotePath, const QUrl &linkUrl, const QString &spaceId,
                                     QWidget *parent)
    : QDialog(parent)
    , _accountState(accountState)
    , _fileName(fileName)
    , _isFolder(isFolder)
    , _remotePath(remotePath.startsWith(QLatin1Char('/')) ? remotePath.mid(1) : remotePath)
    , _linkUrl(linkUrl)
    , _spaceId(spaceId)
{
    setObjectName(QStringLiteral("udriveShareDialog"));
    setWindowTitle(tr("Share “%1”").arg(fileName));
    setModal(true);
    setWindowFlags((windowFlags() | Qt::Dialog) & ~Qt::WindowContextHelpButtonHint);
    setMinimumWidth(480);

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
        "QComboBox#shareRoleCombo { min-height: 22px; padding: 7px 30px 7px 10px; border: 1px solid %2; border-radius: 8px; background: %5; color: %3; font-size: 13px; combobox-popup: 0; }"
        "QComboBox#shareRoleCombo:hover, QComboBox#shareRoleCombo:focus { border-color: %12; }"
        "QComboBox#shareRoleCombo::drop-down { subcontrol-origin: padding; subcontrol-position: center right; width: 28px; border: none; }"
        "QComboBox#shareRoleCombo::down-arrow { image: url(:/udrive/icons/%10.svg); width: 12px; height: 12px; }"
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
        "QScrollArea#sharesScrollArea { border: 1px solid %2; border-radius: 8px; background: %5; }"
    ).arg(bg, border, text, subtext, inputBg, cancelBg, cancelHover, primaryDisabled, primaryDisabledText).arg(arrow, selectedBg, accent));

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(20, 20, 20, 16);
    mainLayout->setSpacing(14);

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

    auto *subtextLabel = new QLabel(isFolder ? tr("Share folder with other users") : tr("Share file with other users"), this);
    subtextLabel->setObjectName(QStringLiteral("shareDialogSubtext"));
    subtextLabel->setTextFormat(Qt::PlainText);
    titleLayout->addWidget(subtextLabel);

    headerLayout->addLayout(titleLayout, 1);
    mainLayout->addLayout(headerLayout);

    auto *inviteSectionLabel = new QLabel(tr("Invite user or group"), this);
    inviteSectionLabel->setObjectName(QStringLiteral("shareSectionTitle"));
    mainLayout->addWidget(inviteSectionLabel);

    auto *inputRow = new QHBoxLayout;
    inputRow->setSpacing(8);

    _userInput = new QLineEdit(this);
    _userInput->setObjectName(QStringLiteral("shareUserInput"));
    _userInput->setPlaceholderText(tr("Search users or groups…"));
    _userInput->setMaxLength(256);
    _userInput->setClearButtonEnabled(true);
    inputRow->addWidget(_userInput, 1);

    _roleCombo = new UDriveComboBox(this);
    _roleCombo->setObjectName(QStringLiteral("shareRoleCombo"));
    _roleCombo->setProperty("udriveCombo", true);
    _roleCombo->setCursor(Qt::PointingHandCursor);
    auto *roles = new QListView(_roleCombo);
    roles->setObjectName(QStringLiteral("shareRolePopup"));
    roles->setFrameShape(QFrame::NoFrame);
    roles->setUniformItemSizes(true);
    roles->setSpacing(2);
    roles->setMouseTracking(true);
    roles->setTextElideMode(Qt::ElideNone);
    roles->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _roleCombo->setView(roles);
    _roleCombo->setItemDelegate(new QStyledItemDelegate(_roleCombo));
    _roleCombo->setMaxVisibleItems(2);
    _roleCombo->addItem(tr("Viewer"), 1);
    _roleCombo->addItem(tr("Editor"), 15);
    _roleCombo->ensurePolished();
    const int roleWidth = qMax(_roleCombo->fontMetrics().horizontalAdvance(_roleCombo->itemText(0)),
        _roleCombo->fontMetrics().horizontalAdvance(_roleCombo->itemText(1)));
    _roleCombo->setFixedWidth(qMax(112, roleWidth + 44));
    _roleCombo->setFixedHeight(38);
    inputRow->addWidget(_roleCombo);

    _shareButton = new QPushButton(tr("Share"), this);
    _shareButton->setObjectName(QStringLiteral("shareConfirmButton"));
    _shareButton->setFixedHeight(38);
    _shareButton->setCursor(Qt::PointingHandCursor);
    _shareButton->setEnabled(false);
    inputRow->addWidget(_shareButton);

    mainLayout->addLayout(inputRow);

    _statusLabel = new QLabel(this);
    _statusLabel->setObjectName(QStringLiteral("shareStatusLabel"));
    _statusLabel->setTextFormat(Qt::PlainText);
    _statusLabel->setWordWrap(true);
    _statusLabel->setStyleSheet(QStringLiteral("font-size: 12px; min-height: 16px;"));
    mainLayout->addWidget(_statusLabel);

    auto *accessSectionLabel = new QLabel(tr("People with access"), this);
    accessSectionLabel->setObjectName(QStringLiteral("shareSectionTitle"));
    mainLayout->addWidget(accessSectionLabel);

    auto *scrollArea = new QScrollArea(this);
    scrollArea->setObjectName(QStringLiteral("sharesScrollArea"));
    scrollArea->setWidgetResizable(true);
    scrollArea->setMinimumHeight(110);
    scrollArea->setMaximumHeight(160);

    _sharesContainer = new QWidget(scrollArea);
    _sharesLayout = new QVBoxLayout(_sharesContainer);
    _sharesLayout->setContentsMargins(10, 8, 10, 8);
    _sharesLayout->setSpacing(6);
    _sharesLayout->addStretch(1);
    scrollArea->setWidget(_sharesContainer);
    mainLayout->addWidget(scrollArea);

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
    auto *recipients = new QListView;
    recipients->setObjectName(QStringLiteral("shareRecipientPopup"));
    recipients->setAccessibleName(tr("Matching users and groups"));
    recipients->setWindowFlag(Qt::FramelessWindowHint);
    recipients->setFrameShape(QFrame::NoFrame);
    recipients->setUniformItemSizes(true);
    recipients->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    recipients->setTextElideMode(Qt::ElideRight);
    recipients->setStyleSheet(QStringLiteral(
        "QListView { background: %1; color: %2; border: 1px solid %3; border-radius: 8px; padding: 0; outline: none; }"
        "QListView::item { padding: 6px 10px; border: none; border-radius: 5px; }"
        "QListView::item:selected { background: %4; color: %5; }")
        .arg(inputBg, text, border, selectedBg, accent));
    _recipientCompleter->setPopup(recipients);
    _recipientCompleter->setWidget(_userInput);
    connect(_recipientCompleter, qOverload<const QModelIndex &>(&QCompleter::activated),
        this, &UDriveShareDialog::selectRecipient);
    connect(_userInput, &QLineEdit::textChanged, this, [this] {
        cancelRecipientSearch();
        _recipientId.clear();
        _recipientLabel.clear();
        _recipientType = -1;
        _shareButton->setEnabled(false);
        _statusLabel->setStyleSheet(QStringLiteral("font-size: 12px; min-height: 16px;"));
        _statusLabel->setText(tr("Type at least 2 characters to find a user or group."));
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
        _shareButton->setEnabled(false);
    });
    if (_accountState) {
        connect(_accountState, &AccountState::stateChanged, this, [this] {
            if (!canShare()) reject();
        });
        connect(_accountState, &QObject::destroyed, this, [this] { reject(); });
    }
    _statusLabel->setText(tr("Type at least 2 characters to find a user or group."));

    fetchShares();
    _userInput->setFocus();
}

UDriveShareDialog::~UDriveShareDialog()
{
    cancelRecipientSearch();
}

bool UDriveShareDialog::canShare() const
{
    return !_closed && validPath(_remotePath) && (_spaceId.isEmpty() || validName(_spaceId))
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
    if (!canShare() || _sharing || query.size() < 2) return;
    _statusLabel->setText(tr("Searching…"));
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
                _statusLabel->setText(tr("Could not search users and groups. Try again."));
            }
        });
    });
    connect(job, &JsonApiJob::finishedSignal, this, [this, job, query] {
        if (_recipientJob != job || !canShare() || query != _userInput->text().trimmed()) return;
        _recipientJob.clear();
        const auto data = job->data().value(QStringLiteral("ocs")).toObject().value(QStringLiteral("data"));
        if (!job->ocsSuccess() || !data.isObject()) {
            _statusLabel->setText(tr("Could not search users and groups. Try again."));
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
                item->setSizeHint(QSize(0, qMax(48, _userInput->fontMetrics().height() * 2 + 12)));
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
            _statusLabel->setText(tr("No matching users or groups. Try another name or email."));
        } else {
            _statusLabel->setText(tr("Select a user or group, then click Share."));
            _recipientCompleter->setCompletionPrefix(QString());
            if (isVisible() && _userInput->hasFocus())
                _recipientCompleter->complete(_userInput->rect().translated(0, 4));
        }
    });
    job->start();
}

void UDriveShareDialog::selectRecipient(const QModelIndex &index)
{
    if (!canShare() || _sharing || !index.isValid()) return;
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
    _statusLabel->setText(tr("Selected: %1").arg(label));
}

void UDriveShareDialog::sendShare()
{
    if (!canShare() || _sharing || _recipientId.isEmpty() || (_recipientType != 0 && _recipientType != 1))
        return;
    const QString user = _recipientLabel;
    _sharing = true;
    _userInput->setEnabled(false);
    _roleCombo->setEnabled(false);
    _shareButton->setEnabled(false);
    _statusLabel->setText(tr("Sharing with %1…").arg(user));
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
            _statusLabel->setText(tr("Failed to share: %1").arg(err));
            _statusLabel->setStyleSheet(QStringLiteral("color: #EF4444; font-size: 12px;"));
        } else {
            _userInput->clear();
            _statusLabel->setText(tr("Successfully shared with %1").arg(user));
            _statusLabel->setStyleSheet(QStringLiteral("color: #22C55E; font-size: 12px;"));
            fetchShares();
        }
    });
    job->start();
}

void UDriveShareDialog::fetchShares()
{
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
        if (!canShare() || !job->ocsSuccess())
            return;

        while (auto *item = _sharesLayout->takeAt(0)) {
            if (auto *w = item->widget())
                w->deleteLater();
            delete item;
        }

        const auto &data = job->data().value(QStringLiteral("ocs")).toObject().value(QStringLiteral("data")).toArray();
        if (data.isEmpty()) {
            auto *noShares = new QLabel(tr("Not shared with anyone yet."), _sharesContainer);
            noShares->setStyleSheet(QStringLiteral("color: #9BA3AF; font-size: 12px; font-style: italic;"));
            _sharesLayout->addWidget(noShares);
        } else {
            for (const auto &element : data) {
                const auto map = element.toObject();
                const QString shareId = map[QStringLiteral("id")].toVariant().toString();
                const QString displayName = map[QStringLiteral("share_with_displayname")].toString();
                const QString shareWith = displayName.isEmpty() ? map[QStringLiteral("share_with")].toString() : displayName;
                const int perms = map[QStringLiteral("permissions")].toInt();
                const QString role = (perms & 2 || perms & 4 || perms & 8 || perms & 16) ? tr("Editor") : tr("Viewer");

                auto *rowWidget = new QWidget(_sharesContainer);
                auto *rowLayout = new QHBoxLayout(rowWidget);
                rowLayout->setContentsMargins(4, 2, 4, 2);

                auto *userIcon = new QLabel(rowWidget);
                userIcon->setPixmap(UDriveIcons::icon(UDriveIcons::Symbol::Folder).pixmap(16, 16));
                rowLayout->addWidget(userIcon);

                auto *userLabel = new QLabel(shareWith, rowWidget);
                userLabel->setTextFormat(Qt::PlainText);
                userLabel->setStyleSheet(QStringLiteral("font-weight: 500; font-size: 13px;"));
                rowLayout->addWidget(userLabel, 1);

                auto *roleBadge = new QLabel(role, rowWidget);
                roleBadge->setStyleSheet(QStringLiteral("background: rgba(34, 197, 94, 0.15); color: #22C55E; border-radius: 4px; padding: 2px 8px; font-size: 11px; font-weight: 600;"));
                rowLayout->addWidget(roleBadge);

                auto *removeBtn = new QPushButton(tr("Remove"), rowWidget);
                removeBtn->setCursor(Qt::PointingHandCursor);
                removeBtn->setStyleSheet(QStringLiteral("color: #EF4444; border: none; background: transparent; font-size: 12px; font-weight: 500; padding: 2px 6px;"));
                connect(removeBtn, &QPushButton::clicked, this, [this, shareId] {
                    revokeShare(shareId);
                });
                rowLayout->addWidget(removeBtn);

                _sharesLayout->addWidget(rowWidget);
            }
        }
        _sharesLayout->addStretch(1);
    });
    job->start();
}

void UDriveShareDialog::revokeShare(const QString &shareId)
{
    if (!_accountState || !_accountState->account() || shareId.isEmpty())
        return;

    _statusLabel->setText(tr("Revoking access…"));
    _statusLabel->setStyleSheet(QStringLiteral("color: #3B82F6; font-size: 12px;"));

    auto *job = new JsonApiJob(_accountState->account(), QStringLiteral("ocs/v1.php/apps/files_sharing/api/v1/shares/%1").arg(shareId), "DELETE", {}, {}, this);
    connect(job, &JsonApiJob::finishedSignal, this, [this, job] {
        if (job->ocsSuccess()) {
            _statusLabel->setText(tr("Access removed."));
            _statusLabel->setStyleSheet(QStringLiteral("color: #22C55E; font-size: 12px;"));
            fetchShares();
        } else {
            const QString err = job->ocsMessage().isEmpty() ? job->errorString() : job->ocsMessage();
            _statusLabel->setText(tr("Failed to revoke: %1").arg(err));
            _statusLabel->setStyleSheet(QStringLiteral("color: #EF4444; font-size: 12px;"));
        }
    });
    job->start();
}

void UDriveShareDialog::copyLink()
{
    if (_linkUrl.isValid() && !_linkUrl.isEmpty()) {
        QApplication::clipboard()->setText(_linkUrl.toString());
        _statusLabel->setText(tr("Sign-in link copied to clipboard."));
        _statusLabel->setStyleSheet(QStringLiteral("color: #22C55E; font-size: 12px;"));
    }
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

    _spaces = new QComboBox(this);
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
    auto *headingRow = new QHBoxLayout;
    headingRow->setSpacing(8);
    headingRow->setAlignment(Qt::AlignVCenter);
    headingRow->addWidget(_heading);
    _learnWorkspacesButton = new QPushButton(tr("Learn about workspaces"), this);
    _learnWorkspacesButton->setObjectName(QStringLiteral("driveLearnWorkspaces"));
    _learnWorkspacesButton->setProperty("driveToolbarButton", true);
    _learnWorkspacesButton->setCursor(Qt::PointingHandCursor);
    _learnWorkspacesButton->hide();
    connect(_learnWorkspacesButton, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://unity-workspace.com/docs")));
    });
    headingRow->addWidget(_learnWorkspacesButton);
    _shareScope = new QPushButton(tr("Shared by me"), this);
    _shareScope->setObjectName(QStringLiteral("driveShareScope"));
    _shareScope->setProperty("driveToolbarButton", true);
    _shareScope->setAccessibleName(tr("Share direction"));
    _shareScope->setFixedHeight(34);
    auto *scopeMenu = createFileMenu(_shareScope);
    auto *scopeGroup = new QActionGroup(scopeMenu);
    scopeGroup->setExclusive(true);
    for (int i = 0; i < 2; ++i) {
        auto *action = scopeMenu->addAction(i == 0 ? tr("Shared by me") : tr("Shared with me"));
        action->setObjectName(i == 0 ? QStringLiteral("driveSharedByMe") : QStringLiteral("driveSharedWithMe"));
        action->setCheckable(true);
        action->setChecked(i == 0);
        scopeGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, action, i] {
            if (_sharedByMe == (i == 0)) return;
            _sharedByMe = i == 0;
            _shareScope->setText(action->text());
            if (_section != Section::Shares) return;
            cancelListing();
            cancelDownload();
            _spaceId.clear();
            _root.clear();
            _path.clear();
            updateSpaces();
        });
    }
    _shareScope->setMenu(scopeMenu);
    QTimer::singleShot(0, this, [this, scopeMenu] {
        scopeMenu->ensurePolished();
        _shareScope->setMinimumWidth(scopeMenu->sizeHint().width());
    });
    connect(scopeMenu, &QMenu::aboutToShow, this, [this, scopeMenu] { scopeMenu->setFixedWidth(_shareScope->width()); });
    headingRow->addWidget(_shareScope);
    headingRow->addWidget(_spaces);
    headingRow->addStretch(1);
    _selectAll = new SelectAllCheckBox(tr("Select all"), this);
    _selectAll->setObjectName(QStringLiteral("driveSelectAll"));
    _selectAll->setTristate(true);
    _selectAll->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    _selectAll->setToolTip(tr("Select all files in the current results."));
    headingRow->addWidget(_selectAll);
    headingRow->addSpacing(12);
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
    headingRow->addWidget(sortLabel);
    headingRow->addWidget(_sortButton);
    headingRow->addWidget(_refreshButton);
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
    headingRow->addWidget(_viewButton);
    layout->addLayout(headingRow);

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
    _workspacesProxy = new QSortFilterProxyModel(this);
    _workspacesProxy->setSourceModel(_workspacesModel);
    _workspacesProxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    _workspacesProxy->setSortCaseSensitivity(Qt::CaseInsensitive);
    _workspacesProxy->setSortRole(Qt::DisplayRole);
    _workspacesView = new QListView(this);
    _workspacesView->setObjectName(QStringLiteral("driveWorkspacesView"));
    _workspacesView->setAccessibleName(tr("Workspaces catalog"));
    _workspacesView->setModel(_workspacesProxy);
    _workspacesView->setViewMode(QListView::IconMode);
    _workspacesView->setResizeMode(QListView::Adjust);
    _workspacesView->setMovement(QListView::Static);
    _workspacesView->setFlow(QListView::LeftToRight);
    _workspacesView->setWrapping(true);
    _workspacesView->setGridSize(QSize(224, 184));
    _workspacesView->setUniformItemSizes(true);
    _workspacesView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    _workspacesView->setSelectionBehavior(QAbstractItemView::SelectItems);
    _workspacesView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _workspacesView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    _workspacesView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _workspacesView->setMouseTracking(true);
    auto *cardDelegate = new WorkspaceCardDelegate(_workspacesView,
        [this](const QModelIndex &index) {
            const QString spaceId = index.data(SpaceIdRole).toString();
            const QString name = index.data(Qt::DisplayRole).toString();
            const QUrl webUrl = index.data(SpaceWebUrlRole).toUrl();
            if (_state) {
                auto *dialog = new UDriveShareDialog(_state, name, true, QString(), webUrl, spaceId, this);
                dialog->setAttribute(Qt::WA_DeleteOnClose);
                dialog->open();
            }
        },
        [this](const QModelIndex &index) {
            showWorkspaceCardMenu(index);
        },
        [this](const QModelIndex &index) {
            _workspacesView->selectionModel()->select(index, QItemSelectionModel::Toggle);
            updateSelectionState();
            updateActions();
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
    _fileViews = new QStackedWidget(this);
    _fileViews->setObjectName(QStringLiteral("driveFileViews"));
    _fileViews->addWidget(_tree); _fileViews->addWidget(_tiles); _fileViews->addWidget(_workspacesView);
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
    connect(_tiles->verticalScrollBar(), &QScrollBar::valueChanged, this, &UDriveFileBrowser::scheduleThumbnails);
    connect(_proxy, &QAbstractItemModel::rowsInserted, this, &UDriveFileBrowser::scheduleThumbnails);
    connect(_proxy, &QAbstractItemModel::layoutChanged, this, &UDriveFileBrowser::scheduleThumbnails);
    connect(_proxy, &QAbstractItemModel::modelReset, this, &UDriveFileBrowser::scheduleThumbnails);
    connect(_search.data(), &QLineEdit::textChanged, _proxy, &QSortFilterProxyModel::setFilterFixedString);
    connect(_search.data(), &QLineEdit::textChanged, _workspacesProxy, &QSortFilterProxyModel::setFilterFixedString);
    connect(_search.data(), &QLineEdit::textChanged, this, &UDriveFileBrowser::scheduleThumbnails);
    connect(_spaces, &QComboBox::currentIndexChanged, this, &UDriveFileBrowser::selectSpace);
    connect(_refreshButton, &QPushButton::clicked, this, [this] {
        if (isWorkspacesOverview()) {
            if (_state && _state->account() && _state->account()->spacesManager()) {
                _state->account()->spacesManager()->refresh();
            }
            updateSpaces();
            return;
        }
        refresh();
    });
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
        const QString name = UDriveInputDialog::getText(this, tr("New folder"), tr("Folder name"), {}, tr("OK"), &ok);
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
            UDriveInputDialog::getText(this, copy ? tr("Make a copy") : tr("Rename"), tr("New name"), item.data().toString(), tr("OK"), &ok);
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
            UDriveInputDialog::getText(this, tr("Move to folder"), tr("Destination folder in this space (leave empty for the root)"), {}, tr("OK"), &ok, true);
        if (guard && ok && root == selectedRoot() && selectedItem().data(PathRole).toString() == oldPath && !moveSelected(path))
            setMessage(tr("Choose a valid destination folder. Existing files will not be overwritten."));
    });
    connect(_deleteAction, &QAction::triggered, this, [this] { confirmDelete(false); });
    connect(_restoreAction, &QAction::triggered, this, &UDriveFileBrowser::restoreSelected);
    connect(_restoreButton, &QPushButton::clicked, this, &UDriveFileBrowser::restoreSelected);
    connect(_permanentDeleteAction, &QAction::triggered, this, [this] { confirmDelete(true); });
    connect(_favoriteAction, &QAction::triggered, this, [this] { setSelectedFavorite(!selectedItem().data(FavoriteRole).toBool()); });
    connect(_shareAction, &QAction::triggered, this, [this] {
        const auto item = selectedItem();
        if (!item.isValid() || !_state || !_state->account())
            return;
        const QString fileName = item.data().toString();
        const bool isFolder = item.data(DirectoryRole).toBool();
        const QString path = item.data(PathRole).toString();
        const QUrl link = item.data(LinkRole).toUrl();
        const QString spaceId = item.data(SpaceRole).toString();

        UDriveShareDialog dialog(_state, fileName, isFolder, path, link, spaceId, this);
        connect(&dialog, &QDialog::finished, this, [this] {
            if (sharedByMeOverview()) refresh();
        });
        dialog.exec();
    });
    connect(_linkAction, &QAction::triggered, this, [this] {
        if (_linkAction->isEnabled()) {
            QApplication::clipboard()->setText(selectedItem().data(LinkRole).toUrl().toString());
            setMessage(tr("Sign-in link copied. Access permissions still apply."));
        }
    });
    connect(_rootButton, &QPushButton::clicked, this, [this] { navigateToBreadcrumb({}, _root); });
    connect(_webAction, &QAction::triggered, this, [this] {
        const auto item = selectedItem();
        QUrl targetUrl = item.isValid() ? item.data(LinkRole).toUrl() : QUrl();
        if (targetUrl.scheme() != QStringLiteral("https") && targetUrl.scheme() != QStringLiteral("http"))
            targetUrl = _webUrl;
        if (targetUrl.scheme() == QStringLiteral("https") || targetUrl.scheme() == QStringLiteral("http"))
            QDesktopServices::openUrl(targetUrl);
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
                if (_deleteConfirmation) _deleteConfirmation->reject();
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
            if (_deleteConfirmation) _deleteConfirmation->reject();
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
    if (isWorkspacesOverview() && _workspacesView) return _workspacesView;
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
    menu->addActions({_shareAction, _favoriteAction, _linkAction, _webAction, _renameAction, _copyAction, _moveAction, _restoreAction, _permanentDeleteAction, _deleteAction});
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
    if (_workspacesProxy)
        _workspacesProxy->sort(0, mode == SortMode::NameDescending ? Qt::DescendingOrder : Qt::AscendingOrder);
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
    if (_uploading) cancelOperation();
    _spaceId.clear();
    _path.clear();
    updateSpaces();
    Q_EMIT sectionChanged(Section::Workspaces);
    Q_EMIT creationAvailabilityChanged();
}

void UDriveFileBrowser::openWorkspace(const QString &spaceId)
{
    if (!_state || !_state->account()) return;
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
    if (_uploading) cancelOperation();

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
    if (!canBrowse() || !_state || !_state->account()) return false;
    bool ok = false;
    const QString name = UDriveInputDialog::getText(this, tr("New Workspace"), tr("Workspace name:"), QString(), tr("Create Workspace"), &ok);
    if (!ok || name.trimmed().isEmpty()) return false;

    QJsonObject payload;
    payload.insert(QStringLiteral("name"), name.trimmed());

    auto *job = new SimpleNetworkJob(_state->account(), _state->account()->url(), QStringLiteral("graph/v1.0/drives"), "POST", payload, QNetworkRequest(), this);
    connect(job, &SimpleNetworkJob::finishedSignal, this, [this, job, name] {
        job->deleteLater();
        if (job->httpStatusCode() == 200 || job->httpStatusCode() == 201) {
            setMessage(tr("Workspace \"%1\" created.").arg(name));
            if (_state && _state->account() && _state->account()->spacesManager()) {
                _state->account()->spacesManager()->refresh();
            }
        } else {
            setMessage(tr("Could not create workspace \"%1\".").arg(name));
        }
    });
    job->start();
    return true;
}

void UDriveFileBrowser::showWorkspaceCardMenu(const QModelIndex &index)
{
    const QString spaceId = index.data(SpaceIdRole).toString();
    const QUrl webUrl = index.data(SpaceWebUrlRole).toUrl();
    if (spaceId.isEmpty()) return;

    auto *menu = createFileMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    auto *openAction = menu->addAction(UDriveIcons::icon(UDriveIcons::Symbol::Open), tr("Open"));
    connect(openAction, &QAction::triggered, this, [this, spaceId] {
        openWorkspace(spaceId);
    });

    if (webUrl.isValid() && (webUrl.scheme() == QStringLiteral("http") || webUrl.scheme() == QStringLiteral("https"))) {
        auto *webAction = menu->addAction(UDriveIcons::icon(UDriveIcons::Symbol::Online), tr("Open on Web"));
        connect(webAction, &QAction::triggered, this, [webUrl] {
            QDesktopServices::openUrl(webUrl);
        });

        auto *linkAction = menu->addAction(UDriveIcons::icon(UDriveIcons::Symbol::Link), tr("Copy web link"));
        connect(linkAction, &QAction::triggered, this, [this, webUrl] {
            QApplication::clipboard()->setText(webUrl.toString());
            setMessage(tr("Link copied to clipboard."));
        });
    }

    menu->popup(QCursor::pos());
}

void UDriveFileBrowser::showShares() { setSection(Section::Shares); }
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
        cancelListing(); cancelDownload();
        if (_uploading) cancelOperation();
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
    switch (_section) {
    case Section::Personal: _heading->setText(tr("Personal")); break;
    case Section::Workspaces: _heading->setText(tr("Workspaces")); break;
    case Section::Shares: _heading->setText(tr("Shares")); break;
    case Section::Favorites: _heading->setText(tr("Favorites")); break;
    case Section::Trash: _heading->setText(tr("Deleted files")); break;
    }
    _shareScope->setVisible(_section == Section::Shares);
    if (_learnWorkspacesButton)
        _learnWorkspacesButton->setVisible(isWorkspacesOverview());
    const bool insideWorkspace = (_section == Section::Workspaces && !_workspaceOverview);
    const bool folderView = _section != Section::Favorites && _section != Section::Trash && !sharedByMeOverview() && !isWorkspacesOverview();
    _newButton->setVisible(folderView && !_workspaceLayout);
    _breadcrumbs->setVisible(folderView && (!_workspaceLayout || !_path.isEmpty() || insideWorkspace));
    const QString searchHint = _section == Section::Favorites ? tr("Search Favorites")
        : _section == Section::Trash ? tr("Search Deleted files")
        : isWorkspacesOverview() ? tr("Search workspaces")
        : sharedByMeOverview() ? tr("Search files shared by me") : tr("Search this folder");
    _search->setPlaceholderText(searchHint);
    _search->setAccessibleName(searchHint);
    _tree->setColumnHidden(3, _section != Section::Favorites && _section != Section::Trash && !sharedByMeOverview());
    _model->setHeaderData(2, Qt::Horizontal, _section == Section::Trash ? tr("Deleted") : tr("Modified"));

    if (isWorkspacesOverview()) {
        _spaces->setVisible(false);
        _spaces->clear();
        _workspacesModel->clear();
        int count = 0;
        if (_state && _state->account()) {
            auto spaces = _state->account()->spacesManager()->spaces();
            std::sort(spaces.begin(), spaces.end(), [](const auto *a, const auto *b) {
                return a->displayName().localeAwareCompare(b->displayName()) < 0;
            });
            for (auto *space : spaces) {
                const bool personal = space->sortPriority() == 100;
                const bool shares = space->sortPriority() == 50;
                if (personal || shares) continue;
                if (space->disabled() && !_includeDisabledWorkspaces) continue;

                auto *item = new QStandardItem(space->displayName());
                item->setData(space->id(), SpaceIdRole);
                item->setData(space->webDavUrl(), SpaceWebDavUrlRole);
                item->setData(space->webUrl(), SpaceWebUrlRole);
                item->setData(space->driveId(), SpaceDriveIdRole);
                item->setData(space->disabled(), SpaceDisabledRole);
                item->setData(space->description(), SpaceDescriptionRole);
                _workspacesModel->appendRow(item);
                count++;
            }
        }
        _statusMessage = count > 0 ? tr("%n space(s) in total", nullptr, count) : tr("No workspaces found.");
        _message->setAlignment(Qt::AlignCenter);
        _message->setText(_statusMessage);
        _fileViews->setCurrentWidget(_workspacesView);
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
            if (space->disabled() || (_section == Section::Personal && !personal)
                || (_section == Section::Workspaces && (personal || shares)) || (_section == Section::Shares && !shares)) continue;
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
    _spaces->setVisible(_section != Section::Favorites && !sharedByMeOverview() && _spaces->count() > 1);
    _spaces->setEnabled(canBrowse() && _spaces->count());
    selectSpace();
}

void UDriveFileBrowser::selectSpace()
{
    _fileViews->setCurrentWidget(_viewMode == ViewMode::Tiles ? static_cast<QWidget *>(_tiles) : _tree);
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
    const bool fileViewport = watched == _tree->viewport() || watched == _tiles->viewport();
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
        auto *view = watched == _tree->viewport() ? static_cast<QAbstractItemView *>(_tree) : static_cast<QAbstractItemView *>(_tiles);
        const auto index = view->indexAt(mouse->position().toPoint());
        const QRect rect = view->visualRect(index);
        const QRect check = view == _tree ? QRect(rect.left() + 8, rect.center().y() - 9, 18, 18)
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
    if (sharedByMeOverview()) { refreshSharedByMe(); return; }
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
    item->setData(trash, TrashedRole);
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
    if (isWorkspacesOverview()) {
        const bool hasSel = _workspacesView && _workspacesView->selectionModel() && _workspacesView->selectionModel()->hasSelection();
        _openButton->setText(tr("Open workspace"));
        _openButton->setEnabled(hasSel);
        _openButton->setVisible(hasSel);
        _downloadButton->setVisible(false);
        _shareButton->setVisible(false);
        _restoreButton->setVisible(false);
        _refreshButton->setEnabled(canBrowse());
        if (_search) _search->setEnabled(canBrowse());
        _viewButton->setEnabled(false);
        updateSelectionState();
        Q_EMIT creationAvailabilityChanged();
        return;
    }
    _viewButton->setEnabled(true);
    const auto index = selectedItem();
    const bool valid = index.isValid() && canBrowse() && !_listing && !_report && !_operation;
    const bool trash = _section == Section::Trash;
    const bool overview = sharedByMeOverview();
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
    const bool navigate = canBrowse() && !_root.isEmpty() && _section != Section::Favorites && !overview && !trash && !_operation;
    _breadcrumbs->setEnabled(navigate);
    _rootButton->setEnabled(navigate && !_path.isEmpty());
    _refreshButton->setEnabled(canBrowse() && (!_root.isEmpty() || overview));
    _shareScope->setEnabled(canBrowse() && !_operation);
    if (_search)
        _search->setEnabled(canBrowse() && (!_root.isEmpty() || overview));
    _spaces->setEnabled(canBrowse() && _spaces->count());
    const bool writableFolder =
        canBrowse() && !_root.isEmpty() && !_listing && !_report && !_operation && !_download && !trash && _section != Section::Favorites && !overview;
    _newFolderAction->setEnabled(writableFolder && (!_folderPermissionsKnown || _folderPermissions.contains(QLatin1Char('K'))));
    _uploadAction->setEnabled(writableFolder && (!_folderPermissionsKnown || _folderPermissions.contains(QLatin1Char('C'))));
    _newButton->setEnabled(_newFolderAction->isEnabled() || _uploadAction->isEnabled());
    const QString permissions = index.data(PermissionsRole).toString();
    _renameAction->setEnabled(valid && !trash && !overview && !_download && (permissions.isEmpty() || permissions.contains(QLatin1Char('N'))));
    _copyAction->setEnabled(
        valid && !trash && !overview && !_download && (!_folderPermissionsKnown || _folderPermissions.contains(directory ? QLatin1Char('K') : QLatin1Char('C'))));
    _moveAction->setEnabled(valid && !trash && !overview && !_download && (permissions.isEmpty() || permissions.contains(QLatin1Char('V'))));
    _deleteAction->setEnabled(valid && !trash && !overview && !_download && (permissions.isEmpty() || permissions.contains(QLatin1Char('D'))));
    _restoreAction->setVisible(trash);
    _restoreAction->setEnabled(valid && trash && !index.data(RestorePathRole).toString().isEmpty());
    _permanentDeleteAction->setVisible(trash);
    _permanentDeleteAction->setEnabled(valid && trash);
    _favoriteAction->setVisible(!trash);
    _favoriteAction->setEnabled(rowActionEnabled(index, RowFavorite));
    _favoriteAction->setText(index.data(FavoriteRole).toBool() ? tr("Remove from Favorites") : tr("Add to Favorites"));
    _favoriteAction->setIcon(UDriveIcons::icon(index.data(FavoriteRole).toBool() ? UDriveIcons::Symbol::StarFilled : UDriveIcons::Symbol::Star));
    const QUrl link = index.data(LinkRole).toUrl();
    _linkAction->setEnabled(rowActionEnabled(index, RowLink));
    _linkAction->setVisible(!trash);
    _shareAction->setEnabled(rowActionEnabled(index, RowShare));
    _shareAction->setVisible(!trash);
    const bool hasWeb = (link.scheme() == QStringLiteral("https") || link.scheme() == QStringLiteral("http")) ||
                        (_webUrl.scheme() == QStringLiteral("https") || _webUrl.scheme() == QStringLiteral("http"));
    _webAction->setEnabled(valid && !trash && hasWeb);
    _webAction->setVisible(!trash);
    _deleteAction->setVisible(!trash);
    _renameAction->setVisible(!trash);
    _copyAction->setVisible(!trash);
    _moveAction->setVisible(!trash);
    _fileMenu->setEnabled(valid);
    _openButton->setVisible(index.isValid() && !trash);
    _downloadButton->setVisible(index.isValid() && !trash);
    _shareButton->setVisible(index.isValid() && !trash);
    _shareButton->setEnabled(_shareAction->isEnabled());
    _restoreButton->setVisible(index.isValid() && trash);
    _restoreButton->setEnabled(valid && trash && !index.data(RestorePathRole).toString().isEmpty());
    _tree->viewport()->update();
    updateSelectionState();
    Q_EMIT creationAvailabilityChanged();
}

void UDriveFileBrowser::activateSelection()
{
    if (isWorkspacesOverview()) {
        const auto indexes = _workspacesView && _workspacesView->selectionModel() ? _workspacesView->selectionModel()->selectedIndexes() : QModelIndexList();
        if (!indexes.isEmpty()) {
            openWorkspace(indexes.first().data(SpaceIdRole).toString());
        }
        return;
    }
    const auto index = selectedItem();
    if (!index.isValid() || _listing || _report || _operation || !canBrowse() || _section == Section::Trash) return;
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
    if (isWorkspacesOverview()) return canBrowse();
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
    _statusMessage = message;
    updateSelectionState();
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
            _workspacesView->selectAll();
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

void UDriveFileBrowser::updateSelectionState()
{
    if (isWorkspacesOverview()) {
        const int count = _workspacesView && _workspacesView->selectionModel()
            ? _workspacesView->selectionModel()->selectedIndexes().count() : 0;
        const int total = _workspacesProxy ? _workspacesProxy->rowCount() : 0;
        const QSignalBlocker blocker(_selectAll);
        _selectAll->setCheckState(count == 0 ? Qt::Unchecked : count == total ? Qt::Checked : Qt::PartiallyChecked);
        _selectAll->setEnabled(canBrowse() && total > 0);
        _message->setAlignment(Qt::AlignCenter);
        _message->setText(count > 0 ? tr("%n workspace(s) selected", nullptr, count) : _statusMessage);
        return;
    }
    const int count = selectedCount();
    const int total = _proxy->rowCount();
    const QSignalBlocker blocker(_selectAll);
    _selectAll->setCheckState(count == 0 ? Qt::Unchecked : count == total ? Qt::Checked : Qt::PartiallyChecked);
    _selectAll->setEnabled(canBrowse() && total > 0 && !_listing && !_report && !_operation && !_download);
    _message->setText(count > 1 ? tr("%1 selected. Select one item to use file actions.").arg(count) : _statusMessage);
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
            return;
        }
        QSet<QString> seen;
        int unavailable = 0;
        const auto entries = value.toArray();
        for (const auto &entry : entries) {
            if (_model->rowCount() >= 1000) { ++unavailable; break; }
            const auto item = entry.toObject();
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
                    || !root.query().isEmpty() || !root.fragment().isEmpty()) continue;
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
                appendEntry(root.path() + QLatin1Char('/') + path, properties, root, space->id(), true);
                seen.insert(id);
                mapped = true;
                break;
            }
            if (!mapped) ++unavailable;
        }
        setMessage(unavailable ? tr("%1 shared items shown; some items are no longer available.").arg(_model->rowCount())
            : _model->rowCount() ? tr("%n item(s)", nullptr, _model->rowCount()) : tr("No files shared by you yet."));
        updateActions();
        scheduleThumbnails();
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
        "<d:prop><d:resourcetype/><d:getcontentlength/><d:getlastmodified/><d:getetag/><oc:permissions/><oc:favorite/><oc:privatelink/><oc:spaceid/></d:prop>"
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
            setMessage(tr("The selected item changed. Review it and confirm again."));
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
    const auto item = selectedItem();
    if (item.isValid()) {
        auto *proxy = qobject_cast<QSortFilterProxyModel *>(_tree->model());
        if (proxy) {
            auto *model = qobject_cast<QStandardItemModel *>(proxy->sourceModel());
            if (model) {
                auto *sourceItem = model->itemFromIndex(proxy->mapToSource(item));
                if (sourceItem) {
                    sourceItem->setData(favorite, FavoriteRole);
                }
            }
        }
    }
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
    _checkboxViewport.clear();
    if (_deleteConfirmation) _deleteConfirmation->reject();
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
    const auto index = selectedItem();
    if (!index.isValid() || index.data(DirectoryRole).toBool() || _listing || _download || !canBrowse()) return;
    const QString path = index.data(PathRole).toString();
    const QUrl root = _root;
    const QString currentPath = _path;
    const QUrl entryRoot = selectedRoot();
    const QPointer<UDriveFileBrowser> guard(this);
    const QString destination = QFileDialog::getSaveFileName(this, tr("Save file"),
        QDir(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)).filePath(index.data().toString()));
    if (!guard || destination.isEmpty() || !canBrowse() || _root != root || _path != currentPath || _download
        || selectedRoot() != entryRoot || selectedItem().data(PathRole).toString() != path) return;
    saveSelectedFile(destination);
}

bool UDriveFileBrowser::saveSelectedFile(const QString &destination)
{
    return startDownload(destination, false);
}

bool UDriveFileBrowser::startDownload(const QString &destination, bool preview)
{
    const auto index = selectedItem();
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
