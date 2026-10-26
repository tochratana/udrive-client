// SPDX-License-Identifier: GPL-2.0-or-later
#include "udriveadminwidget.h"
#include "udrivetoast.h"
#include "accountstate.h"
#include "libsync/account.h"
#include "common/utility.h"
#include "guiutility.h"
#include "udrivecombobox.h"
#include "udriveicons.h"
#include "udrivepopup.h"
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDesktopServices>
#include <QCompleter>
#include <QFormLayout>
#include <QFrame>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QScrollBar>
#include <QShowEvent>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QTabBar>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QHideEvent>
#include <QLocale>
#include <QApplication>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QPersistentModelIndex>
#include <QHelpEvent>
#include <QResizeEvent>
#include <QToolTip>
#include <cmath>

using namespace OCC;
using Resource = UDriveAdminApi::Resource;
namespace {
constexpr int recordRole = Qt::UserRole + 20;
constexpr int editableRole = Qt::UserRole + 21;
class AdminIcon : public QWidget
{
public:
    AdminIcon(UDriveIcons::Symbol symbol, QWidget *parent) : QWidget(parent), _symbol(symbol) { setFixedSize(20, 20); }
protected:
    void paintEvent(QPaintEvent *) override { QPainter painter(this); UDriveIcons::icon(_symbol).paint(&painter, rect()); }
private:
    UDriveIcons::Symbol _symbol;
};
class AdminDetailsEditButton : public QPushButton
{
public:
    using QPushButton::QPushButton;
protected:
    void paintEvent(QPaintEvent *event) override
    {
        QPushButton::paintEvent(event); QPainter painter(this);
        UDriveIcons::icon(UDriveIcons::Symbol::Next).paint(&painter, QRect(width() - 26, (height() - 18) / 2, 18, 18), Qt::AlignCenter, isEnabled() ? QIcon::Normal : QIcon::Disabled);
    }
};
class AdminHeader : public QHeaderView
{
public:
    explicit AdminHeader(QTreeView *tree) : QHeaderView(Qt::Horizontal, tree), _tree(tree) {}
protected:
    void paintSection(QPainter *painter, const QRect &rect, int section) const override
    {
        if (section != 0) { QHeaderView::paintSection(painter, rect, section); return; }
        QStyleOptionHeader option;
        initStyleOption(&option); option.rect = rect; option.section = section;
        style()->drawControl(QStyle::CE_HeaderSection, &option, painter, this);
        // Use the table's item indicator geometry and style for select-all as
        // well, including stylesheet padding and the platform's border metrics.
        QStyleOptionViewItem check; check.initFrom(_tree); check.widget = _tree;
        check.rect = rect; check.index = model()->index(0, 0);
        check.features = QStyleOptionViewItem::HasCheckIndicator | QStyleOptionViewItem::HasDecoration | QStyleOptionViewItem::HasDisplay;
        check.decorationSize = _tree->iconSize(); check.displayAlignment = Qt::AlignLeft | Qt::AlignVCenter;
        const int count = _tree->selectionModel()->selectedRows().size();
        check.checkState = count == 0 ? Qt::Unchecked : count == model()->rowCount() ? Qt::Checked : Qt::PartiallyChecked;
        check.state &= ~(QStyle::State_On | QStyle::State_Off | QStyle::State_NoChange);
        check.state |= check.checkState == Qt::Checked ? QStyle::State_On : check.checkState == Qt::PartiallyChecked ? QStyle::State_NoChange : QStyle::State_Off;
        check.rect = _tree->style()->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &check, _tree);
        _tree->style()->drawPrimitive(QStyle::PE_IndicatorItemViewItemCheck, &check, painter, _tree);
        option.rect.adjust(76, 0, -8, 0); option.text = model()->headerData(0, Qt::Horizontal).toString();
        option.textAlignment = Qt::AlignLeft | Qt::AlignVCenter;
        option.sortIndicator = sortIndicatorSection() == 0 ? (sortIndicatorOrder() == Qt::AscendingOrder ? QStyleOptionHeader::SortDown : QStyleOptionHeader::SortUp) : QStyleOptionHeader::None;
        style()->drawControl(QStyle::CE_HeaderLabel, &option, painter, this);
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && logicalIndexAt(event->position().toPoint()) == 0
            && event->position().x() < sectionViewportPosition(0) + 34) {
            if (_tree->selectionModel()->selectedRows().size() == model()->rowCount()) _tree->clearSelection();
            else _tree->selectAll();
            viewport()->update(); return;
        }
        QHeaderView::mousePressEvent(event);
    }
private:
    QTreeView *_tree;
};
class AdminActionsDelegate : public QStyledItemDelegate
{
public:
    AdminActionsDelegate(QTreeView *tree, std::function<void(const QModelIndex &, int)> open)
        : QStyledItemDelegate(tree), _tree(tree), _open(std::move(open))
    {
        tree->installEventFilter(this); tree->viewport()->installEventFilter(this);
    }
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyledItemDelegate::paint(painter, option, index);
        if (!isAction(index)) return;
        const bool users = index.model()->columnCount() == 6;
        const int first = users ? 0 : 2;
        for (int action = first; action < 3; ++action) {
            const auto symbol = action == 0 ? UDriveIcons::Symbol::Info : action == 1 ? UDriveIcons::Symbol::Rename : UDriveIcons::Symbol::MoreVertical;
            const bool editable = index.siblingAtColumn(0).data(editableRole).toBool();
            UDriveIcons::icon(symbol).paint(painter, actionRect(option.rect, action, users).adjusted(7, 7, -7, -7), Qt::AlignCenter,
                action == 1 && !editable ? QIcon::Disabled : QIcon::Normal);
        }
    }
    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option, const QModelIndex &index) override
    {
        if (event->type() == QEvent::MouseButtonRelease) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton && option.rect.contains(mouse->position().toPoint())) {
                if (isAction(index)) {
                    const bool users = index.model()->columnCount() == 6;
                    for (int action = users ? 0 : 2; action < 3; ++action)
                        if (actionRect(option.rect, action, users).contains(mouse->position().toPoint())) { _open(index, action); return true; }
                }
            }
        }
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }
    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override
    {
        QStyledItemDelegate::initStyleOption(option, index);
        option->decorationSize = index.column() == 0 ? QSize(32, 32) : QSize(20, 20);
    }
    bool helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option, const QModelIndex &index) override
    {
        if (isAction(index)) {
            const bool users = index.model()->columnCount() == 6;
            for (int action = users ? 0 : 2; action < 3; ++action) if (actionRect(option.rect, action, users).contains(event->pos())) {
                QToolTip::showText(event->globalPos(), action == 0 ? QObject::tr("User details") : action == 1 ? QObject::tr("Edit user") : QObject::tr("More actions"), view); return true;
            }
        }
        return QStyledItemDelegate::helpEvent(event, view, option, index);
    }
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == _tree->viewport() && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::MouseButtonDblClick)) {
            const auto *mouse = static_cast<QMouseEvent *>(event); const auto index = _tree->indexAt(mouse->position().toPoint());
            if (mouse->button() == Qt::LeftButton && index.isValid() && index.column() == 0
                && mouse->position().x() < _tree->visualRect(index).left() + 32) {
                if (event->type() == QEvent::MouseButtonRelease) {
                    _tree->selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
                    _tree->selectionModel()->select(index, QItemSelectionModel::Toggle | QItemSelectionModel::Rows);
                }
                return true;
            }
        }
        if (watched == _tree && event->type() == QEvent::KeyPress) {
            const auto *key = static_cast<QKeyEvent *>(event);
            if (key->key() == Qt::Key_Space && _tree->currentIndex().column() == 0) {
                _tree->selectionModel()->select(_tree->currentIndex(), QItemSelectionModel::Toggle | QItemSelectionModel::Rows); return true;
            }
            const bool activate = isAction(_tree->currentIndex()) && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter || key->key() == Qt::Key_Space);
            const bool context = key->key() == Qt::Key_Menu || (key->key() == Qt::Key_F10 && key->modifiers() == Qt::ShiftModifier);
            if ((activate || context) && _tree->currentIndex().isValid()) { _open(_tree->currentIndex(), 2); return true; }
        }
        return QObject::eventFilter(watched, event);
    }
private:
    static QRect actionRect(const QRect &rect, int action, bool users) { return QRect(rect.right() - (users ? (3 - action) * 32 : 32) + 1, rect.center().y() - 16, 32, 32); }
    static bool isAction(const QModelIndex &index) { return index.isValid() && index.column() == index.model()->columnCount() - 1; }
    QTreeView *_tree;
    std::function<void(const QModelIndex &, int)> _open;
};
QIcon userAvatar(const QJsonObject &record, int size = 64)
{
    const QString name = record.value(QStringLiteral("displayName")).toString();
    QString initials;
    for (const auto &word : name.split(QLatin1Char(' '), Qt::SkipEmptyParts)) { initials += word.left(1).toUpper(); if (initials.size() >= 2) break; }
    if (initials.isEmpty()) initials = record.value(QStringLiteral("onPremisesSamAccountName")).toString().left(2).toUpper();
    const QString id = record.value(QStringLiteral("id")).toString();
    uint hash = 0; for (const auto character : id) hash = hash * 31 + character.unicode();
    const QColor colors[] = {QColor("#167594"), QColor("#208579"), QColor("#267C2C"), QColor("#865B49"), QColor("#727A12"), QColor("#AC2858")};
    QPixmap pixmap(size, size); pixmap.fill(Qt::transparent); QPainter painter(&pixmap); painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen); painter.setBrush(colors[hash % 6]); painter.drawEllipse(pixmap.rect());
    auto font = qApp->font(); font.setPixelSize(qRound(size * 0.36)); font.setWeight(QFont::Medium); painter.setFont(font); painter.setPen(Qt::white);
    painter.drawText(pixmap.rect(), Qt::AlignCenter, initials.isEmpty() ? QStringLiteral("?") : initials);
    return QIcon(pixmap);
}
QString text(const QJsonObject &object, const char *key) { return object.value(QString::fromLatin1(key)).toString(); }
bool disabledWorkspace(const QJsonObject &record)
{
    return record.value(QStringLiteral("root")).toObject().value(QStringLiteral("deleted")).toObject()
        .value(QStringLiteral("state")).toString() == QStringLiteral("trashed");
}
QLabel *label(const QString &value, QWidget *parent)
{
    auto *result = new QLabel(value, parent); result->setTextFormat(Qt::PlainText); result->setWordWrap(true); return result;
}
QDialog *dialogFor(QWidget *owner, const QString &title, const QString &description, UDriveIcons::Symbol icon = UDriveIcons::Symbol::Settings)
{
    auto *dialog = new QDialog(owner);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setWindowTitle(title);
    dialog->setMinimumWidth(qMin(480, udrivePopupBounds(owner).width() - 24));
    const bool dark = qApp->palette().color(QPalette::Window).lightness() < 128;
    dialog->setStyleSheet(QStringLiteral(
        "QDialog { background: %1; color: %2; }"
        "QWidget#adminDialogHeader, QWidget#adminFormFields, QScrollArea, QScrollArea > QWidget, QScrollArea > QWidget > QWidget { background: transparent; }"
        "QLabel, QCheckBox { color: %2; background: transparent; }"
        "QLabel#adminDialogDescription { color: %4; } QLabel#adminFormError { color: %7; }"
        "QLineEdit, QComboBox, QListWidget { color: %2; background: %1; border: 1px solid %3; border-radius: 7px; padding: 9px 11px; }"
        "QLineEdit:focus, QComboBox:focus { border-color: %5; }"
        "QComboBox { padding-right: 30px; combobox-popup: 0; }"
        "QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: center right; width: 28px; border: none; }"
        "QComboBox::down-arrow { image: url(:/udrive/icons/%9.svg); width: 12px; height: 12px; }"
        "QComboBox QLineEdit { border: none; background: transparent; padding: 0; }"
        "QComboBox QAbstractItemView { background: %1; color: %2; selection-background-color: %8; selection-color: %2; border: 1px solid %3; padding: 4px; outline: none; }"
        "QComboBox QAbstractItemView::item { min-height: 28px; padding: 4px 8px; }"
        "QLineEdit:disabled, QComboBox:disabled { color: %4; border-color: %3; }"
        "QListWidget::item { padding: 8px 4px; border-radius: 4px; }"
        "QListWidget::item:selected { background: %8; color: %2; }"
        "QPushButton { color: %2; background: transparent; border: 1px solid %3; border-radius: 7px; padding: 9px 16px; }"
        "QPushButton:hover { background: %8; } QPushButton:focus { border-color: %5; }"
        "QPushButton[dialogAction=primary] { color: white; background: %5; border-color: %5; font-weight: 600; }"
        "QPushButton[dialogAction=primary]:hover { background: %6; border-color: %6; }"
        "QPushButton[dialogAction=danger] { color: %7; border-color: %7; }"
        "QPushButton:disabled { color: %4; background: transparent; border-color: %3; }"
        "QPushButton#adminDialogClose { border: none; padding: 6px; }"
        "QFrame#adminDialogRule { background: %3; border: none; max-height: 1px; }"
        "QScrollArea { border: none; }")
        .arg(dark ? QStringLiteral("#292929") : QStringLiteral("#FFFFFF"), dark ? QStringLiteral("#EDEEF0") : QStringLiteral("#2B3035"),
             dark ? QStringLiteral("#535A56") : QStringLiteral("#CCD4CE"), dark ? QStringLiteral("#ABB4AE") : QStringLiteral("#5D6861"),
             QStringLiteral("#00856B"), QStringLiteral("#006C57"), dark ? QStringLiteral("#FF9B9B") : QStringLiteral("#B42332"),
             dark ? QStringLiteral("#353C38") : QStringLiteral("#EDF4EF"),
             dark ? QStringLiteral("chevron-dark") : QStringLiteral("chevron-light")));
    auto *layout = new QVBoxLayout(dialog); layout->setContentsMargins(24, 24, 24, 20); layout->setSpacing(20);
    auto *header = new QWidget(dialog); header->setObjectName(QStringLiteral("adminDialogHeader"));
    auto *headerLayout = new QHBoxLayout(header); headerLayout->setContentsMargins(0, 0, 0, 0); headerLayout->setSpacing(12);
    auto *symbol = label({}, header); symbol->setPixmap(UDriveIcons::icon(icon).pixmap(26, 26)); headerLayout->addWidget(symbol, 0, Qt::AlignTop);
    auto *copy = new QVBoxLayout; copy->setSpacing(6);
    auto *heading = label(title, header); heading->setObjectName(QStringLiteral("adminDialogTitle"));
    auto font = heading->font(); font.setPointSize(17); font.setBold(true); heading->setFont(font); copy->addWidget(heading);
    auto *subtitle = label(description, header); subtitle->setObjectName(QStringLiteral("adminDialogDescription")); copy->addWidget(subtitle);
    headerLayout->addLayout(copy, 1);
    auto *close = new QPushButton(UDriveIcons::icon(UDriveIcons::Symbol::Close), {}, header);
    close->setObjectName(QStringLiteral("adminDialogClose")); close->setAccessibleName(QObject::tr("Close dialog")); close->setToolTip(QObject::tr("Close"));
    close->setAutoDefault(false); close->setFixedSize(30, 30); headerLayout->addWidget(close, 0, Qt::AlignTop);
    QObject::connect(close, &QPushButton::clicked, dialog, &QDialog::reject);
    layout->addWidget(header);
    return dialog;
}
QDialogButtonBox *dialogButtons(QDialog *dialog, QDialogButtonBox::StandardButtons actions)
{
    auto *rule = new QFrame(dialog); rule->setObjectName(QStringLiteral("adminDialogRule")); rule->setFixedHeight(1); dialog->layout()->addWidget(rule);
    auto *buttons = new QDialogButtonBox(actions, dialog); dialog->layout()->addWidget(buttons);
    if (auto *save = buttons->button(QDialogButtonBox::Save)) { save->setProperty("dialogAction", QStringLiteral("primary")); save->setDefault(true); }
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    return buttons;
}
void openDialog(QDialog *dialog)
{
    // Action properties are assigned after button creation. Refresh their
    // styles before measuring so the primary/destructive treatments apply.
    for (auto *button : dialog->findChildren<QPushButton *>()) {
        button->style()->unpolish(button); button->style()->polish(button);
    }
    const auto bounds = udrivePopupBounds(dialog->parentWidget());
    dialog->setMaximumSize(bounds.size() - QSize(16, 16));
    dialog->adjustSize();
    dialog->resize(dialog->size().boundedTo(dialog->maximumSize()));
    dialog->move(bounds.center() - dialog->rect().center());
    dialog->open();
    containUDrivePopup(dialog, dialog->parentWidget());
}
QString roleNames(const QJsonObject &record, const QJsonArray &roles)
{
    QStringList names;
    for (const auto &assignment : record.value(QStringLiteral("appRoleAssignments")).toArray()) {
        const auto grant = assignment.toObject();
        QString name;
        for (const auto &role : roles) {
            const auto entry = role.toObject();
            if (entry.value(QStringLiteral("id")) == grant.value(QStringLiteral("appRoleId"))
                && entry.value(QStringLiteral("resourceId")) == grant.value(QStringLiteral("resourceId"))) name = text(entry, "displayName");
        }
        names.append(name.isEmpty() ? QObject::tr("Assigned role") : name);
    }
    return names.isEmpty() ? QObject::tr("No role") : names.join(QStringLiteral(", "));
}
}

UDriveAdminWidget::UDriveAdminWidget(AccountState *state, QWidget *parent)
    : QWidget(parent), _api(new UDriveAdminApi(state, this)), _state(state)
{
    setObjectName(QStringLiteral("driveAdministration"));
    auto *pageLayout = new QHBoxLayout(this); pageLayout->setContentsMargins(0, 0, 0, 0); pageLayout->setSpacing(0);
    auto *content = new QWidget(this); content->setObjectName(QStringLiteral("driveAdminContent"));
    content->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding); pageLayout->addWidget(content);
    auto *layout = new QVBoxLayout(content); layout->setContentsMargins(24, 24, 24, 20); layout->setSpacing(16);
    layout->setSizeConstraint(QLayout::SetNoConstraint);
    auto *headingRow = new QHBoxLayout; headingRow->setSpacing(8);
    auto *heading = label(tr("Administration Settings"), this); heading->setObjectName(QStringLiteral("driveAdminTitle"));
    headingRow->addWidget(heading);
    auto *separator = new AdminIcon(UDriveIcons::Symbol::Next, this); headingRow->addWidget(separator);
    _section = new QPushButton(tr("Users"), this); _section->setObjectName(QStringLiteral("driveAdminSection"));
    _section->setProperty("driveToolbarButton", true); _section->setAccessibleName(tr("Administration section")); headingRow->addWidget(_section);
    headingRow->addStretch();
    _refresh = new QPushButton(UDriveIcons::icon(UDriveIcons::Symbol::Refresh), {}, this);
    _refresh->setObjectName(QStringLiteral("driveAdminRefresh")); _refresh->setProperty("driveToolbarButton", true);
    _refresh->setToolTip(tr("Refresh")); _refresh->setAccessibleName(tr("Refresh administration entries")); headingRow->addWidget(_refresh);
    auto *settings = new QPushButton(UDriveIcons::icon(UDriveIcons::Symbol::Settings), {}, this);
    settings->setProperty("driveToolbarButton", true); settings->setAccessibleName(tr("Administration settings")); settings->setToolTip(tr("Administration settings")); headingRow->addWidget(settings);
    layout->addLayout(headingRow);
    _tabs = new QTabBar(this); _tabs->setObjectName(QStringLiteral("driveAdminTabs")); _tabs->setExpanding(false); _tabs->setDrawBase(false);
    _tabs->addTab(UDriveIcons::icon(UDriveIcons::Symbol::Members), tr("Users"));
    _tabs->addTab(UDriveIcons::icon(UDriveIcons::Symbol::Share), tr("Groups"));
    _tabs->addTab(UDriveIcons::icon(UDriveIcons::Symbol::Grid), tr("Workspaces"));
    _tabs->hide();
    auto openSections = [this](QWidget *anchor) {
        UDriveMenu menu(this); menu.setObjectName(QStringLiteral("driveAdminSectionsMenu"));
        for (int i = 0; i < _tabs->count(); ++i) if (_tabs->isTabVisible(i)) {
            auto *action = menu.addAction(_tabs->tabIcon(i), _tabs->tabText(i), this, [this, i] { _tabs->setCurrentIndex(i); });
            action->setCheckable(true); action->setChecked(i == _tabs->currentIndex()); action->setEnabled(!_api->busy() && !_userEditorOpen);
        }
        menu.addSeparator();
        menu.addAction(UDriveIcons::icon(UDriveIcons::Symbol::Online), tr("Web administration"), this, [this] {
            if (_state && _state->account() && _api->available()) Utility::openBrowser(Utility::concatUrlPath(_state->account()->url(), QStringLiteral("admin-settings")), this);
        });
        menu.exec(anchor->mapToGlobal(QPoint(0, anchor->height())));
    };
    connect(settings, &QPushButton::clicked, this, [settings, openSections] { openSections(settings); });
    connect(_section, &QPushButton::clicked, this, [this, openSections] { openSections(_section); });
    layout->addSpacing(4);
    auto *filters = new QHBoxLayout; filters->setSpacing(10);
    _search = new QLineEdit(this); _search->setObjectName(QStringLiteral("driveAdminSearch")); _search->setClearButtonEnabled(true);
    _search->addAction(UDriveIcons::icon(UDriveIcons::Symbol::Search), QLineEdit::LeadingPosition);
    _search->setPlaceholderText(tr("Search users")); _search->setAccessibleName(tr("Search administration entries"));
    _search->setMinimumWidth(150); _search->setMaximumWidth(240);
    auto combo = [this](const QString &name) {
        auto *box = new UDriveComboBox(this); box->setObjectName(name); box->setProperty("udriveCombo", true);
        box->setMinimumContentsLength(6); box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        box->setMinimumWidth(96); box->setMaximumWidth(190); return box;
    };
    _filterLabel = new QWidget(this);
    auto *filterLabelLayout = new QHBoxLayout(_filterLabel); filterLabelLayout->setContentsMargins(0, 0, 0, 0); filterLabelLayout->setSpacing(5);
    auto *filterIcon = new AdminIcon(UDriveIcons::Symbol::Filter, _filterLabel);
    filterLabelLayout->addWidget(filterIcon); filterLabelLayout->addWidget(label(tr("Filter:"), _filterLabel)); filters->addWidget(_filterLabel);
    _groupFilter = combo(QStringLiteral("driveAdminGroupFilter")); _groupFilter->setAccessibleName(tr("Filter by group")); filters->addWidget(_groupFilter);
    _roleFilter = combo(QStringLiteral("driveAdminRoleFilter")); _roleFilter->setAccessibleName(tr("Filter by role")); filters->addWidget(_roleFilter);
    filters->addStretch(); filters->addWidget(_search);
    layout->addLayout(filters);
    _selectionActions = new QWidget(this);
    auto selectionPolicy = _selectionActions->sizePolicy(); selectionPolicy.setRetainSizeWhenHidden(true); _selectionActions->setSizePolicy(selectionPolicy);
    _selectionActions->setMinimumHeight(36);
    auto *actions = new QHBoxLayout(_selectionActions); actions->setContentsMargins(0, 0, 0, 0); actions->setSpacing(6);
    auto button = [this, actions](const QString &name, const QString &caption, UDriveIcons::Symbol icon) {
        auto *result = new QPushButton(UDriveIcons::icon(icon), caption, this); result->setObjectName(name);
        result->setProperty("driveToolbarButton", true); result->setIconSize(QSize(18, 18)); actions->addWidget(result); return result;
    };
    _edit = button(QStringLiteral("driveAdminEdit"), tr("Edit"), UDriveIcons::Symbol::Rename);
    _role = button(QStringLiteral("driveAdminRole"), tr("Role"), UDriveIcons::Symbol::Settings);
    _members = button(QStringLiteral("driveAdminMembers"), tr("Members"), UDriveIcons::Symbol::Members);
    _login = button(QStringLiteral("driveAdminLogin"), tr("Login access"), UDriveIcons::Symbol::Disable);
    _remove = button(QStringLiteral("driveAdminRemove"), tr("Delete"), UDriveIcons::Symbol::Trash);
    actions->addStretch();
    layout->insertWidget(1, _selectionActions);
    _status = label({}, this); _status->setObjectName(QStringLiteral("driveAdminStatus")); _status->hide(); layout->addWidget(_status);
    _model = new QStandardItemModel(this); _proxy = new QSortFilterProxyModel(this); _proxy->setSourceModel(_model);
    _proxy->setFilterCaseSensitivity(Qt::CaseInsensitive); _proxy->setFilterKeyColumn(-1); _proxy->setSortCaseSensitivity(Qt::CaseInsensitive);
    _tree = new QTreeView(this); _tree->setObjectName(QStringLiteral("driveAdminTable")); _tree->setAccessibleName(tr("Administration entries"));
    _tree->setModel(_proxy); _tree->setRootIsDecorated(false); _tree->setSortingEnabled(true); _tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    _tree->setSelectionMode(QAbstractItemView::ExtendedSelection); _tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _tree->setHeader(new AdminHeader(_tree)); _tree->setIconSize(QSize(32, 32));
    _tree->setUniformRowHeights(true); _tree->header()->setStretchLastSection(false); _tree->header()->setSectionsMovable(false); _tree->header()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    _tree->setTabKeyNavigation(true);
    _tree->setItemDelegate(new AdminActionsDelegate(_tree, [this](const QModelIndex &index, int actionIndex) {
        _tree->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows); updateActions();
        if (actionIndex == 0) { showDetails(); return; }
        if (actionIndex == 1) { if (_edit->isEnabled()) edit(false); return; }
        const auto action = index.siblingAtColumn(_proxy->columnCount() - 1);
        showRowActions(index, _tree->viewport()->mapToGlobal(_tree->visualRect(action).bottomRight()));
    }));
    _tree->setContextMenuPolicy(Qt::CustomContextMenu); layout->addWidget(_tree, 1);
    _empty = new QWidget(this); auto *emptyLayout = new QVBoxLayout(_empty); emptyLayout->addStretch();
    auto *emptyIcon = label({}, _empty); emptyIcon->setPixmap(UDriveIcons::icon(UDriveIcons::Symbol::Members).pixmap(48, 48)); emptyIcon->setAlignment(Qt::AlignCenter);
    emptyLayout->addWidget(emptyIcon); auto *emptyText = label(tr("No entries to display"), _empty); emptyText->setAlignment(Qt::AlignCenter); emptyLayout->addWidget(emptyText); emptyLayout->addStretch();
    layout->addWidget(_empty, 1); _empty->hide();
    _count = label({}, this); _count->setObjectName(QStringLiteral("driveAdminCount")); _count->setAlignment(Qt::AlignCenter); layout->addWidget(_count);
    buildDetails();
    connect(_search, &QLineEdit::textChanged, _proxy, &QSortFilterProxyModel::setFilterFixedString);
    connect(_proxy, &QAbstractItemModel::rowsInserted, this, &UDriveAdminWidget::updateActions);
    connect(_proxy, &QAbstractItemModel::rowsRemoved, this, &UDriveAdminWidget::updateActions);
    connect(_proxy, &QAbstractItemModel::modelReset, this, &UDriveAdminWidget::updateActions);
    connect(_tree->selectionModel(), &QItemSelectionModel::selectionChanged, this, &UDriveAdminWidget::updateActions);
    connect(_tabs, &QTabBar::currentChanged, this, [this] { _section->setText(_tabs->tabText(_tabs->currentIndex())); if (!_api->busy()) refresh(); });
    connect(_groupFilter, &QComboBox::currentIndexChanged, this, &UDriveAdminWidget::render);
    connect(_roleFilter, &QComboBox::currentIndexChanged, this, &UDriveAdminWidget::render);
    connect(_refresh, &QPushButton::clicked, this, &UDriveAdminWidget::activate);
    connect(_edit, &QPushButton::clicked, this, [this] { edit(false); });
    connect(_role, &QPushButton::clicked, this, &UDriveAdminWidget::editRole);
    connect(_members, &QPushButton::clicked, this, &UDriveAdminWidget::editMembers);
    connect(_login, &QPushButton::clicked, this, &UDriveAdminWidget::toggleLogin);
    connect(_remove, &QPushButton::clicked, this, &UDriveAdminWidget::removeSelected);
    connect(_tree, &QTreeView::doubleClicked, this, [this](const QModelIndex &index) { if (index.column() != _proxy->columnCount() - 1 && _edit->isEnabled()) edit(false); });
    connect(_tree, &QWidget::customContextMenuRequested, this, &UDriveAdminWidget::showActions);
    connect(_api, &UDriveAdminApi::cleared, this, [this] {
        closeDetails();
        _refreshAfterAccess = false; _reloadAfterWrite = false;
        for (auto *dialog : findChildren<QDialog *>()) dialog->reject();
        _records = {}; _model->clear(); _search->clear(); _groupFilter->clear(); _roleFilter->clear(); _status->clear(); _count->clear();
    });
    connect(_api, &UDriveAdminApi::accessChanged, this, [this] {
        updateAccess();
        if (isVisible() && _api->available()) _refreshAfterAccess = true;
        if (_hadAccess != _api->available()) { _hadAccess = _api->available(); Q_EMIT availabilityChanged(); }
    });
    connect(_api, &UDriveAdminApi::busyChanged, this, &UDriveAdminWidget::updateActions);
    connect(_api, &UDriveAdminApi::recordsLoaded, this, [this](Resource loaded, const QJsonArray &records) {
        if (loaded != resource()) return;
        _records = records;
        const QSignalBlocker groupSignals(_groupFilter), roleSignals(_roleFilter);
        const auto groupId = _groupFilter->currentData(); const auto roleId = _roleFilter->currentData();
        _groupFilter->clear(); _groupFilter->addItem(tr("Groups"), QString());
        _roleFilter->clear(); _roleFilter->addItem(tr("Roles"), QString()); _roleFilter->addItem(tr("No role"), QStringLiteral("none"));
        for (const auto &role : _api->roles()) {
            const auto data = role.toObject(); _roleFilter->addItem(text(data, "displayName"), text(data, "id"));
        }
        QSet<QString> groups;
        for (const auto &record : records) for (const auto &group : record.toObject().value(QStringLiteral("memberOf")).toArray()) {
            const auto data = group.toObject(); const auto id = text(data, "id");
            if (!id.isEmpty() && !groups.contains(id)) { groups.insert(id); _groupFilter->addItem(text(data, "displayName"), id); }
        }
        _groupFilter->setCurrentIndex(qMax(0, _groupFilter->findData(groupId))); _roleFilter->setCurrentIndex(qMax(0, _roleFilter->findData(roleId)));
        render();
        if (!_reopenDetailsId.isEmpty()) {
            const auto id = _reopenDetailsId; _reopenDetailsId.clear();
            for (int row = 0; row < _proxy->rowCount(); ++row) if (text(_proxy->index(row, 0).data(recordRole).toJsonObject(), "id") == id) {
                _tree->setCurrentIndex(_proxy->index(row, 0)); showDetails(); break;
            }
        }
    });
    connect(_api, &UDriveAdminApi::operationFinished, this, [this](bool success, const QString &message) {
        if (_userSaving) { userEditFinished(success, message); return; }
        if (!success) _reopenDetailsId.clear();
        const bool write = _reloadAfterWrite;
        _status->setText(message); _status->setVisible(!write && !message.isEmpty());
        if (write) UDriveToast::showMessage(this, message, success ? UDriveToast::Kind::Success : UDriveToast::Kind::Error);
        if (_refreshAfterAccess) { _refreshAfterAccess = false; if (success && isVisible() && _api->available()) refresh(); }
        else if (_reloadAfterWrite) { _reloadAfterWrite = false; if (success && isVisible() && _api->available()) refresh(); }
        updateActions();
    });
    auto *timer = new QTimer(this); timer->setInterval(60000);
    connect(timer, &QTimer::timeout, this, [this] {
        if (_api->busy() || _userEditorOpen) return;
        for (auto *dialog : findChildren<QDialog *>()) if (dialog->isVisible()) return;
        if (isVisible()) activate(); else _api->refreshAccess();
    }); timer->start();
    updateAccess();
}
Resource UDriveAdminWidget::resource() const { return static_cast<Resource>(qMax(0, _tabs->currentIndex())); }
bool UDriveAdminWidget::canCreate() const { return !_userEditorOpen && !_api->busy() && _api->canCreate(resource()); }
QString UDriveAdminWidget::creationLabel() const
{
    return resource() == Resource::Users ? tr("New user") : resource() == Resource::Groups ? tr("New group") : tr("New workspace");
}
void UDriveAdminWidget::createNew() { if (canCreate()) edit(true); }
QJsonObject UDriveAdminWidget::selected() const
{
    const auto rows = _tree->selectionModel()->selectedRows();
    return rows.size() == 1 ? rows.first().data(recordRole).toJsonObject() : QJsonObject();
}
void UDriveAdminWidget::showEvent(QShowEvent *event) { QWidget::showEvent(event); activate(); }
void UDriveAdminWidget::hideEvent(QHideEvent *event)
{
    for (auto *dialog : findChildren<QDialog *>()) dialog->reject();
    closeDetails();
    QWidget::hideEvent(event);
}
void UDriveAdminWidget::activate()
{
    _refreshAfterAccess = true;
    if (_api->busy()) return;
    _status->setText(tr("Checking administration permissions…")); _status->show(); _api->refreshAccess();
}
void UDriveAdminWidget::refresh()
{
    if (_api->busy() || !_api->canRead(resource())) return;
    _records = {}; _model->clear(); _search->clear(); _status->setText(tr("Loading…")); _status->show();
    _api->load(resource()); updateActions();
}
void UDriveAdminWidget::updateAccess()
{
    const QSignalBlocker blocker(_tabs);
    const int previous = _tabs->currentIndex();
    const bool wasVisible = previous >= 0 && _tabs->isTabVisible(previous);
    for (int i = 0; i < 3; ++i) _tabs->setTabVisible(i, _api->canRead(static_cast<Resource>(i)));
    if (wasVisible && _api->canRead(static_cast<Resource>(previous))) _tabs->setCurrentIndex(previous);
    else for (int i = 0; i < 3; ++i) if (_api->canRead(static_cast<Resource>(i))) { _tabs->setCurrentIndex(i); break; }
    if (_tabs->currentIndex() != previous || !_api->canRead(resource())) { _records = {}; _model->clear(); }
    updateActions();
}
void UDriveAdminWidget::updateActions()
{
    const auto type = resource(); const auto record = selected(); const bool one = _tree->selectionModel()->selectedRows().size() == 1 && !text(record, "id").isEmpty();
    const int selectedCount = _tree->selectionModel()->selectedRows().size();
    const bool idle = !_api->busy() && _api->canRead(type); const bool users = type == Resource::Users;
    const bool self = users && text(record, "id") == _api->selfId();
    _section->setText(_tabs->tabText(_tabs->currentIndex())); _section->setEnabled(!_api->busy());
    _selectionActions->setVisible(selectedCount > 0); _filterLabel->setVisible(users);
    { const QSignalBlocker blocker(_model);
      for (int row = 0; row < _proxy->rowCount(); ++row) {
          const auto index = _proxy->index(row, 0);
          _model->itemFromIndex(_proxy->mapToSource(index))->setCheckState(_tree->selectionModel()->isRowSelected(row) ? Qt::Checked : Qt::Unchecked);
      }
    }
    _tree->viewport()->update(); _tree->header()->viewport()->update();
    _tabs->setEnabled(!_api->busy()); _refresh->setEnabled(!_api->busy());
    _search->setEnabled(idle); _groupFilter->setVisible(users); _roleFilter->setVisible(users); _groupFilter->setEnabled(idle); _roleFilter->setEnabled(idle);
    _edit->setEnabled(idle && one && (_api->canUpdate(type) || (type == Resource::Workspaces && _api->canSetQuota())));
    _role->setVisible(users); _role->setEnabled(idle && one && !self && _api->canAssignRoles() && !_api->roles().isEmpty());
    _members->setVisible(type == Resource::Groups); _members->setEnabled(idle && one && _api->canRead(type));
    _login->setVisible(users); _login->setEnabled(idle && one && !self && _api->userFieldWritable(QStringLiteral("accountEnabled")));
    _remove->setText(type == Resource::Workspaces ? (disabledWorkspace(record) ? tr("Enable") : tr("Disable")) : tr("Delete"));
    _remove->setEnabled(idle && one && !self && _api->canDelete(type));
    _count->setText(tr("%1 entries").arg(_proxy->rowCount()));
    _empty->setVisible(idle && _proxy->rowCount() == 0); _tree->setVisible(!idle || _proxy->rowCount() > 0);
    updateDetails();
    _selectionActions->setEnabled(!_userEditorOpen); _tabs->setEnabled(!_api->busy() && !_userEditorOpen);
    _tree->setEnabled(!_userEditorOpen); _refresh->setEnabled(!_api->busy() && !_userEditorOpen); _section->setEnabled(!_api->busy() && !_userEditorOpen);
    _search->setEnabled(idle && !_userEditorOpen); _groupFilter->setEnabled(idle && !_userEditorOpen); _roleFilter->setEnabled(idle && !_userEditorOpen);
    Q_EMIT controlsChanged();
}
void UDriveAdminWidget::render()
{
    const auto type = resource(); _model->clear();
    _search->setPlaceholderText(type == Resource::Users ? tr("Search users") : type == Resource::Groups ? tr("Search groups") : tr("Search workspaces"));
    _model->setHorizontalHeaderLabels(type == Resource::Users ? QStringList{tr("User name"), tr("First and last name"), tr("Email"), tr("Role"), tr("Login"), tr("Actions")}
        : type == Resource::Groups ? QStringList{tr("Group"), tr("Description"), tr("ID"), tr("Actions")}
        : QStringList{tr("Workspace"), tr("Description"), tr("Quota"), tr("Status"), tr("Actions")});
    for (const auto &entry : _records) {
        const auto record = entry.toObject(); QStringList columns;
        if (type == Resource::Users) {
            const QString group = _groupFilter->currentData().toString(), role = _roleFilter->currentData().toString();
            bool groupMatches = group.isEmpty(), roleMatches = role.isEmpty();
            for (const auto &value : record.value(QStringLiteral("memberOf")).toArray()) if (text(value.toObject(), "id") == group) groupMatches = true;
            const auto assignments = record.value(QStringLiteral("appRoleAssignments")).toArray();
            if (role == QStringLiteral("none")) roleMatches = assignments.isEmpty();
            for (const auto &value : assignments) if (text(value.toObject(), "appRoleId") == role) roleMatches = true;
            if (!groupMatches || !roleMatches) continue;
            columns = {text(record, "onPremisesSamAccountName"), text(record, "displayName"), text(record, "mail"), roleNames(record, _api->roles()),
                record.value(QStringLiteral("accountEnabled")).toBool(true) ? tr("Allowed") : tr("Forbidden")};
        } else if (type == Resource::Groups) columns = {text(record, "displayName"), text(record, "description"), text(record, "id")};
        else {
            if (text(record, "driveType") != QStringLiteral("project")) continue;
            const auto quota = record.value(QStringLiteral("quota")).toObject().value(QStringLiteral("total"));
            columns = {text(record, "name"), text(record, "description"), quota.isDouble() ? QLocale().formattedDataSize(quota.toInteger()) : tr("Unknown"),
                disabledWorkspace(record) ? tr("Disabled") : tr("Active")};
        }
        QList<QStandardItem *> items;
        for (const auto &column : columns) { auto *item = new QStandardItem(column); item->setToolTip(column); items.append(item); }
        items.first()->setData(record, recordRole);
        items.first()->setCheckable(true);
        items.first()->setData(_api->canUpdate(type), editableRole);
        items.first()->setIcon(type == Resource::Users ? userAvatar(record) : UDriveIcons::icon(type == Resource::Groups ? UDriveIcons::Symbol::Share : UDriveIcons::Symbol::Grid));
        if (type == Resource::Users) items.at(4)->setIcon(UDriveIcons::icon(record.value(QStringLiteral("accountEnabled")).toBool(true) ? UDriveIcons::Symbol::Enable : UDriveIcons::Symbol::Disable));
        auto *actions = new QStandardItem;
        const auto caption = tr("Actions for %1").arg(columns.first());
        actions->setToolTip(caption); actions->setData(caption, Qt::AccessibleTextRole); items.append(actions);
        _model->appendRow(items);
    }
    for (int i = 0; i < _model->columnCount(); ++i) _tree->header()->setSectionResizeMode(i, i == 1 ? QHeaderView::Stretch : QHeaderView::ResizeToContents);
    // Let names and email share the remaining width so long addresses do not
    // push the row actions out of view in a compact window.
    if (type == Resource::Users) _tree->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    const int actionsColumn = _model->columnCount() - 1;
    _tree->header()->setSectionResizeMode(actionsColumn, QHeaderView::Fixed);
    _tree->header()->resizeSection(actionsColumn, type == Resource::Users ? 108 : 76);
    _tree->header()->setMinimumSectionSize(60);
    _tree->header()->setSectionResizeMode(0, QHeaderView::Interactive); _tree->header()->resizeSection(0, type == Resource::Users ? 210 : 180);
    _tree->sortByColumn(0, Qt::AscendingOrder); updateActions();
}
void UDriveAdminWidget::showActions(const QPoint &point)
{
    showRowActions(_tree->indexAt(point), _tree->viewport()->mapToGlobal(point));
}
void UDriveAdminWidget::showRowActions(const QModelIndex &index, const QPoint &globalPosition)
{
    if (!index.isValid() || _api->busy() || !_api->canRead(resource())) return;
    _tree->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows); updateActions();
    const QPersistentModelIndex row(index.siblingAtColumn(0));
    UDriveMenu menu(this); menu.setObjectName(QStringLiteral("driveAdminRowMenu"));
    connect(_model, &QAbstractItemModel::modelAboutToBeReset, &menu, &QMenu::close);
    connect(_proxy, &QAbstractItemModel::layoutAboutToBeChanged, &menu, &QMenu::close);
    connect(_proxy, &QAbstractItemModel::rowsAboutToBeRemoved, &menu, &QMenu::close);
    connect(_api, &UDriveAdminApi::busyChanged, &menu, &QMenu::close);
    connect(_api, &UDriveAdminApi::accessChanged, &menu, &QMenu::close);
    for (auto *button : {_edit, _role, _members, _login, _remove}) if (!button->isHidden()) {
        auto *action = menu.addAction(button->icon(), button->text(), this, [this, row, button] {
            if (!row.isValid() || _api->busy() || !_api->canRead(resource())) return;
            _tree->setCurrentIndex(row); updateActions();
            if (button->isEnabled()) button->click();
        });
        action->setEnabled(button->isEnabled());
    }
    if (resource() == Resource::Users) {
        menu.addSeparator();
        menu.addAction(UDriveIcons::icon(UDriveIcons::Symbol::Info), tr("User details"), this, [this, row] {
            if (!row.isValid()) return;
            _tree->selectionModel()->setCurrentIndex(row, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows); showDetails();
        });
    }
    menu.exec(globalPosition);
}
void UDriveAdminWidget::buildDetails()
{
    _detailsPane = new QScrollArea(this); _detailsPane->setObjectName(QStringLiteral("driveAdminDetails"));
    _detailsPane->setAccessibleName(tr("User details")); _detailsPane->setWidgetResizable(true);
    _detailsPane->setFrameShape(QFrame::NoFrame); _detailsPane->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *body = new QWidget(_detailsPane); body->setObjectName(QStringLiteral("driveAdminDetailsBody"));
    auto *layout = new QVBoxLayout(body); layout->setContentsMargins(18, 20, 18, 20); layout->setSpacing(16);
    auto *header = new QHBoxLayout;
    _detailsTitle = label(tr("Details"), body); _detailsTitle->setAlignment(Qt::AlignCenter);
    auto font = _detailsTitle->font(); font.setBold(true); font.setPointSize(14); _detailsTitle->setFont(font);
    _detailsBack = new QPushButton(UDriveIcons::icon(UDriveIcons::Symbol::Back), {}, body);
    _detailsBack->setObjectName(QStringLiteral("driveAdminDetailsBack")); _detailsBack->setAccessibleName(tr("Back to Details panel")); _detailsBack->setToolTip(tr("Back to Details panel"));
    _detailsBack->setProperty("driveToolbarButton", true); _detailsBack->setFixedSize(30, 30);
    auto backPolicy = _detailsBack->sizePolicy(); backPolicy.setRetainSizeWhenHidden(true); _detailsBack->setSizePolicy(backPolicy); _detailsBack->hide();
    connect(_detailsBack, &QPushButton::clicked, this, [this] { if (!_userSaving) leaveUserEditor(); });
    header->addWidget(_detailsBack); header->addWidget(_detailsTitle, 1);
    auto *close = new QPushButton(UDriveIcons::icon(UDriveIcons::Symbol::Close), {}, body);
    close->setObjectName(QStringLiteral("driveAdminDetailsClose")); close->setAccessibleName(tr("Close user details")); close->setToolTip(tr("Close"));
    close->setProperty("driveToolbarButton", true); close->setFixedSize(30, 30); header->addWidget(close);
    connect(close, &QPushButton::clicked, this, [this] { const bool partial = _userSavedSteps > 0; closeDetails(); updateActions(); if (partial) refresh(); _tree->setFocus(); });
    layout->addLayout(header); layout->addSpacing(24);
    _detailsAvatar = label({}, body); _detailsAvatar->setAlignment(Qt::AlignCenter); layout->addWidget(_detailsAvatar);
    auto *names = new QVBoxLayout; names->setSpacing(3);
    _detailsUsername = label({}, body); _detailsUsername->setObjectName(QStringLiteral("driveAdminDetailsUsername")); _detailsUsername->setAlignment(Qt::AlignCenter);
    _detailsName = label({}, body); _detailsName->setObjectName(QStringLiteral("driveAdminDetailsName")); _detailsName->setAlignment(Qt::AlignCenter);
    auto nameFont = _detailsName->font(); nameFont.setPointSize(16); nameFont.setWeight(QFont::Medium); _detailsName->setFont(nameFont);
    names->addWidget(_detailsUsername); names->addWidget(_detailsName); layout->addLayout(names); layout->addSpacing(12);
    _detailsStack = new QStackedWidget(body); _detailsStack->setObjectName(QStringLiteral("driveAdminDetailsStack"));
    auto *overview = new QWidget(_detailsStack); auto *overviewLayout = new QVBoxLayout(overview); overviewLayout->setContentsMargins(0, 0, 0, 0); overviewLayout->setSpacing(16);
    auto *form = new QFormLayout; form->setSpacing(8); form->setHorizontalSpacing(12);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow); form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    for (const auto &caption : QStringList{tr("User name"), tr("First and last name"), tr("Email"), tr("Role"), tr("Login"), tr("Quota"), tr("Groups")}) {
        auto *key = label(caption, body); auto keyFont = key->font(); keyFont.setWeight(QFont::DemiBold); key->setFont(keyFont);
        auto *value = label({}, body); value->setMinimumWidth(0); value->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        value->setAccessibleName(caption); _detailsValues.append(value); form->addRow(key, value);
    }
    overviewLayout->addLayout(form);
    auto *rule = new QFrame(body); rule->setObjectName(QStringLiteral("driveAdminDetailsRule")); rule->setFixedHeight(1); overviewLayout->addWidget(rule);
    _detailsEdit = new AdminDetailsEditButton(UDriveIcons::icon(UDriveIcons::Symbol::Rename), tr("Edit user"), body);
    _detailsEdit->setObjectName(QStringLiteral("driveAdminDetailsEdit")); _detailsEdit->setProperty("driveToolbarButton", true);
    _detailsEdit->setIconSize(QSize(20, 20)); overviewLayout->addWidget(_detailsEdit);
    connect(_detailsEdit, &QPushButton::clicked, this, [this] { if (_detailsOpen && _edit->isEnabled()) edit(false); });
    overviewLayout->addStretch(); _detailsStack->addWidget(overview); buildUserEditor(_detailsStack);
    _detailsStack->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored); _detailsStack->setMinimumHeight(overview->sizeHint().height());
    layout->addWidget(_detailsStack, 1); _detailsPane->setWidget(body); _detailsPane->hide();
}
void UDriveAdminWidget::showDetails()
{
    if (_api->busy() || !_api->canRead(Resource::Users) || resource() != Resource::Users || text(selected(), "id").isEmpty()) return;
    resetUserEditor(); _detailsOpen = true; updateDetails();
}
void UDriveAdminWidget::updateDetails()
{
    if (!_detailsOpen) return;
    const auto record = selected();
    if (resource() != Resource::Users || !_api->canRead(Resource::Users) || text(record, "id").isEmpty()) { closeDetails(); return; }
    if (_userEditorOpen && text(record, "id") != text(_userEditRecord, "id")) resetUserEditor();
    _detailsAvatar->setPixmap(userAvatar(record, 160).pixmap(80, 80));
    _detailsUsername->setText(text(record, "onPremisesSamAccountName")); _detailsName->setText(text(record, "displayName"));
    const auto total = record.value(QStringLiteral("drive")).toObject().value(QStringLiteral("quota")).toObject().value(QStringLiteral("total"));
    const QString quota = total.isDouble() && total.toInteger(-1) >= 0
        ? QLocale().formattedDataSize(total.toInteger(), total.toInteger() % 1000000000 == 0 ? 0 : 2, QLocale::DataSizeSIFormat) : tr("Not available");
    QStringList groups;
    for (const auto &group : record.value(QStringLiteral("memberOf")).toArray()) {
        const auto name = text(group.toObject(), "displayName"); if (!name.isEmpty()) groups.append(name);
    }
    groups.removeDuplicates(); groups.sort(Qt::CaseInsensitive);
    const QStringList values{text(record, "onPremisesSamAccountName"), text(record, "displayName"), text(record, "mail"), roleNames(record, _api->roles()),
        record.value(QStringLiteral("accountEnabled")).toBool(true) ? tr("Allowed") : tr("Forbidden"), quota, groups.isEmpty() ? tr("No groups") : groups.join(QStringLiteral(", "))};
    for (int i = 0; i < values.size(); ++i) { _detailsValues.at(i)->setText(values.at(i)); _detailsValues.at(i)->setToolTip(values.at(i)); }
    _detailsEdit->setEnabled(!_api->busy() && _api->canUpdate(Resource::Users)); updateUserEditorAccess(); _detailsPane->show(); positionDetails();
}
void UDriveAdminWidget::closeDetails()
{
    resetUserEditor(); _reopenDetailsId.clear(); _detailsOpen = false; _detailsPane->hide(); _detailsAvatar->clear(); _detailsUsername->clear(); _detailsName->clear();
    for (auto *value : _detailsValues) { value->clear(); value->setToolTip({}); }
    _detailsEdit->setEnabled(false); positionDetails();
}
void UDriveAdminWidget::positionDetails()
{
    if (!_detailsPane) return;
    const int panelWidth = qMin(width(), 340);
    // Narrow windows use an overlaid inspector, preserving room for the table
    // and filters without forcing the application window wider.
    layout()->setContentsMargins(0, 0, _detailsOpen && width() >= 900 ? panelWidth : 0, 0);
    if (_detailsOpen) { _detailsPane->setGeometry(width() - panelWidth, 0, panelWidth, height()); _detailsPane->raise(); }
}
void UDriveAdminWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event); positionDetails();
}
void UDriveAdminWidget::buildUserEditor(QWidget *parent)
{
    _userEditor = new QWidget(parent); _userEditor->setObjectName(QStringLiteral("driveAdminUserEditor"));
    auto *layout = new QVBoxLayout(_userEditor); layout->setContentsMargins(14, 16, 14, 16); layout->setSpacing(16);
    auto *form = new QFormLayout; form->setRowWrapPolicy(QFormLayout::WrapAllRows); form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow); form->setSpacing(12);
    for (const auto &field : QList<QPair<QString, QString>>{{QStringLiteral("onPremisesSamAccountName"), tr("User name")}, {QStringLiteral("displayName"), tr("First and last name")}, {QStringLiteral("mail"), tr("Email")}, {QStringLiteral("passwordProfile"), tr("New password (optional)")}}) {
        auto *input = new QLineEdit(_userEditor); input->setObjectName(QStringLiteral("adminEdit_") + field.first); input->setAccessibleName(field.second); input->setMaxLength(1024); input->setMinimumWidth(0);
        if (field.first == QStringLiteral("passwordProfile")) { input->setEchoMode(QLineEdit::Password); input->setPlaceholderText(tr("Leave blank to keep the current password")); input->setAttribute(Qt::WA_InputMethodEnabled, false); }
        form->addRow(field.second, input); _userInputs.insert(field.first, input);
    }
    _userRole = new UDriveComboBox(_userEditor); _userRole->setObjectName(QStringLiteral("adminEditRole")); _userRole->setProperty("udriveCombo", true); _userRole->setAccessibleName(tr("Role")); form->addRow(tr("Role"), _userRole);
    _userLogin = new UDriveComboBox(_userEditor); _userLogin->setObjectName(QStringLiteral("adminEditLogin")); _userLogin->setProperty("udriveCombo", true); _userLogin->setAccessibleName(tr("Login"));
    _userLogin->addItem(tr("Allowed"), true); _userLogin->addItem(tr("Forbidden"), false); form->addRow(tr("Login"), _userLogin);
    _userQuota = new QLineEdit(_userEditor); _userQuota->setObjectName(QStringLiteral("adminEditQuota")); _userQuota->setAccessibleName(tr("Personal quota in GB")); _userQuota->setMaxLength(32); form->addRow(tr("Personal quota (GB)"), _userQuota);
    layout->addLayout(form);
    _userEditError = label({}, _userEditor); _userEditError->setObjectName(QStringLiteral("driveAdminUserEditError")); _userEditError->hide(); layout->addWidget(_userEditError);
    auto *buttons = new QHBoxLayout; buttons->setSpacing(10);
    _userCancel = new QPushButton(tr("Cancel"), _userEditor); _userCancel->setObjectName(QStringLiteral("driveAdminUserCancel")); buttons->addWidget(_userCancel);
    _userSave = new QPushButton(tr("Save changes"), _userEditor); _userSave->setObjectName(QStringLiteral("driveAdminUserSave")); buttons->addWidget(_userSave);
    layout->addLayout(buttons); layout->addStretch(); _detailsStack->addWidget(_userEditor);
    connect(_userCancel, &QPushButton::clicked, this, [this] { if (!_userSaving) leaveUserEditor(); });
    connect(_userSave, &QPushButton::clicked, this, &UDriveAdminWidget::saveUserEditor);
}
void UDriveAdminWidget::openUserEditor()
{
    if (_api->busy() || !_edit->isEnabled() || resource() != Resource::Users) return;
    const auto record = selected(); if (text(record, "id").isEmpty()) return;
    resetUserEditor(); _userEditRecord = record; _userEditorOpen = true; _detailsOpen = true;
    for (auto it = _userInputs.begin(); it != _userInputs.end(); ++it) it.value()->setText(it.key() == QStringLiteral("passwordProfile") ? QString() : record.value(it.key()).toString());
    _userRole->clear(); int assigned = -1;
    for (const auto &value : _api->roles()) {
        const auto role = value.toObject(); if (!role.value(QStringLiteral("isEnabled")).toBool(true)) continue;
        _userRole->addItem(text(role, "displayName"), role);
        for (const auto &grant : record.value(QStringLiteral("appRoleAssignments")).toArray()) if (text(grant.toObject(), "appRoleId") == text(role, "id") && text(grant.toObject(), "resourceId") == text(role, "resourceId")) assigned = _userRole->count() - 1;
    }
    _userRole->setCurrentIndex(assigned); _userRole->setPlaceholderText(tr("No role"));
    _userLogin->setCurrentIndex(record.value(QStringLiteral("accountEnabled")).toBool(true) ? 0 : 1);
    const auto total = record.value(QStringLiteral("drive")).toObject().value(QStringLiteral("quota")).toObject().value(QStringLiteral("total"));
    _userQuota->setText(total.isDouble() && total.toInteger(-1) >= 0 ? QString::number(total.toDouble() / 1000000000.0, 'g', 12) : QString());
    _userQuota->setPlaceholderText(tr("Not available until the user has signed in"));
    _detailsStack->setCurrentWidget(_userEditor); _detailsStack->setMinimumHeight(_userEditor->sizeHint().height()); _detailsTitle->setText(tr("Edit user")); _detailsBack->show(); updateActions();
    _detailsPane->verticalScrollBar()->setValue(0);
}
void UDriveAdminWidget::resetUserEditor()
{
    _userEditorOpen = false; _userSaving = false; _userSavedSteps = 0; _userEdits = {}; _userEditPending = {}; _userEditRecord = {};
    for (auto *input : _userInputs) input->clear();
    _userQuota->clear(); _userRole->clear(); _userEditError->clear(); _userEditError->hide();
    _detailsStack->setCurrentIndex(0); _detailsStack->setMinimumHeight(_detailsStack->currentWidget()->sizeHint().height()); _detailsTitle->setText(tr("Details")); _detailsBack->setEnabled(true); _detailsBack->hide();
    findChild<QPushButton *>(QStringLiteral("driveAdminDetailsClose"))->setEnabled(true);
}
void UDriveAdminWidget::leaveUserEditor()
{
    const auto id = text(_userEditRecord, "id"); const bool partial = _userSavedSteps > 0;
    resetUserEditor();
    if (partial) { closeDetails(); _reopenDetailsId = id; refresh(); }
    else updateActions();
}
void UDriveAdminWidget::updateUserEditorAccess()
{
    if (!_userEditorOpen) return;
    const bool idle = !_api->busy() && !_userSaving; const bool self = text(_userEditRecord, "id") == _api->selfId();
    for (auto it = _userInputs.begin(); it != _userInputs.end(); ++it) it.value()->setEnabled(idle && _api->userFieldWritable(it.key()));
    _userRole->setEnabled(idle && !self && _api->canAssignRoles() && _userRole->count() > 0);
    _userLogin->setEnabled(idle && !self && _api->userFieldWritable(QStringLiteral("accountEnabled")));
    const auto drive = _userEditRecord.value(QStringLiteral("drive")).toObject();
    _userQuota->setEnabled(idle && _api->canSetPersonalQuota() && !text(drive, "id").isEmpty() && drive.value(QStringLiteral("quota")).toObject().value(QStringLiteral("total")).isDouble());
    _userSave->setEnabled(idle && _api->canUpdate(Resource::Users)); _userCancel->setEnabled(!_userSaving); _detailsBack->setEnabled(!_userSaving);
    findChild<QPushButton *>(QStringLiteral("driveAdminDetailsClose"))->setEnabled(!_userSaving);
}
void UDriveAdminWidget::saveUserEditor()
{
    if (!_userEditorOpen || _api->busy() || _userSaving || !_api->canUpdate(Resource::Users) || text(selected(), "id") != text(_userEditRecord, "id")) return;
    _userEditError->hide(); QJsonArray changes; QJsonObject fields;
    auto report = [this](const QString &message) { _userEditError->setText(message); _userEditError->show(); };
    for (auto it = _userInputs.begin(); it != _userInputs.end(); ++it) {
        if (!_api->userFieldWritable(it.key())) continue;
        const auto value = it.key() == QStringLiteral("passwordProfile") ? it.value()->text() : it.value()->text().trimmed();
        if ((it.key() == QStringLiteral("onPremisesSamAccountName") || it.key() == QStringLiteral("displayName")) && value.isEmpty()) { report(tr("Enter a name before saving.")); it.value()->setFocus(); return; }
        if (it.key() == QStringLiteral("passwordProfile")) { if (!value.isEmpty()) fields.insert(it.key(), QJsonObject{{QStringLiteral("password"), value}}); }
        else if (value != _userEditRecord.value(it.key()).toString()) fields.insert(it.key(), value);
    }
    if (!fields.isEmpty()) changes.append(QJsonObject{{QStringLiteral("kind"), QStringLiteral("profile")}, {QStringLiteral("fields"), fields}});
    if (_userLogin->isEnabled() && _userLogin->currentData().toBool() != _userEditRecord.value(QStringLiteral("accountEnabled")).toBool(true)) changes.append(QJsonObject{{QStringLiteral("kind"), QStringLiteral("login")}, {QStringLiteral("enabled"), _userLogin->currentData().toBool()}});
    if (_userQuota->isEnabled()) {
        bool valid = false; const double gb = _userQuota->text().toDouble(&valid);
        if (!valid || !std::isfinite(gb) || gb < 0 || gb > 9000000) { report(tr("Enter a valid personal quota in GB.")); _userQuota->setFocus(); return; }
        const qint64 total = qRound64(gb * 1000000000.0);
        if (total != _userEditRecord.value(QStringLiteral("drive")).toObject().value(QStringLiteral("quota")).toObject().value(QStringLiteral("total")).toInteger()) changes.append(QJsonObject{{QStringLiteral("kind"), QStringLiteral("quota")}, {QStringLiteral("total"), total}});
    }
    if (_userRole->isEnabled() && _userRole->currentIndex() >= 0) {
        const auto role = _userRole->currentData().toJsonObject(); bool assigned = false;
        for (const auto &entry : _userEditRecord.value(QStringLiteral("appRoleAssignments")).toArray()) if (text(entry.toObject(), "appRoleId") == text(role, "id") && text(entry.toObject(), "resourceId") == text(role, "resourceId")) assigned = true;
        if (!assigned) changes.append(QJsonObject{{QStringLiteral("kind"), QStringLiteral("role")}, {QStringLiteral("role"), role}});
    }
    if (changes.isEmpty()) { resetUserEditor(); updateActions(); return; }
    _userEdits = changes; _userSavedSteps = 0; _userSaving = true; sendNextUserEdit();
}
void UDriveAdminWidget::sendNextUserEdit()
{
    if (!_userSaving || !_userEditorOpen || !_detailsOpen || !isVisible()) return;
    if (_userEdits.isEmpty()) {
        const auto id = text(_userEditRecord, "id"); closeDetails(); _reopenDetailsId = id;
        UDriveToast::showMessage(this, tr("Changes saved."), UDriveToast::Kind::Success); refresh(); return;
    }
    _userEditPending = _userEdits.takeAt(0).toObject(); const auto kind = text(_userEditPending, "kind"); const auto id = text(_userEditRecord, "id"); bool sent = false;
    if (kind == QStringLiteral("profile")) sent = _api->save(Resource::Users, id, _userEditPending.value(QStringLiteral("fields")).toObject());
    else if (kind == QStringLiteral("login")) sent = _api->setLogin(id, _userEditPending.value(QStringLiteral("enabled")).toBool());
    else if (kind == QStringLiteral("quota")) sent = _api->setPersonalQuota(text(_userEditRecord.value(QStringLiteral("drive")).toObject(), "id"), _userEditPending.value(QStringLiteral("total")).toInteger());
    else if (kind == QStringLiteral("role")) { const auto role = _userEditPending.value(QStringLiteral("role")).toObject(); sent = _api->assignRole(id, text(role, "id"), text(role, "resourceId")); }
    if (!sent && _userSaving) userEditFinished(false, tr("This change is no longer permitted. Refresh and try again."));
}
void UDriveAdminWidget::userEditFinished(bool success, const QString &message)
{
    if (!success) {
        _userSaving = false; _userEdits = {}; _userEditPending = {};
        _userEditError->setText(_userSavedSteps > 0 ? tr("Some changes were saved. %1").arg(message) : message); _userEditError->show(); updateActions(); return;
    }
    const auto kind = text(_userEditPending, "kind");
    if (kind == QStringLiteral("profile")) {
        const auto fields = _userEditPending.value(QStringLiteral("fields")).toObject();
        for (auto it = fields.begin(); it != fields.end(); ++it) if (it.key() != QStringLiteral("passwordProfile")) _userEditRecord.insert(it.key(), it.value());
        _userInputs.value(QStringLiteral("passwordProfile"))->clear();
    } else if (kind == QStringLiteral("login")) _userEditRecord.insert(QStringLiteral("accountEnabled"), _userEditPending.value(QStringLiteral("enabled")));
    else if (kind == QStringLiteral("quota")) { auto drive = _userEditRecord.value(QStringLiteral("drive")).toObject(); auto quota = drive.value(QStringLiteral("quota")).toObject(); quota.insert(QStringLiteral("total"), _userEditPending.value(QStringLiteral("total"))); drive.insert(QStringLiteral("quota"), quota); _userEditRecord.insert(QStringLiteral("drive"), drive); }
    else if (kind == QStringLiteral("role")) { const auto role = _userEditPending.value(QStringLiteral("role")).toObject(); _userEditRecord.insert(QStringLiteral("appRoleAssignments"), QJsonArray{QJsonObject{{QStringLiteral("appRoleId"), text(role, "id")}, {QStringLiteral("resourceId"), text(role, "resourceId")}}}); }
    _userEditPending = {}; ++_userSavedSteps; sendNextUserEdit();
}
void UDriveAdminWidget::edit(bool create)
{
    if (_api->busy() || !(create ? canCreate() : _edit->isEnabled())) return;
    if (!create && resource() == Resource::Users) { openUserEditor(); return; }
    const auto type = resource(); const auto record = create ? QJsonObject() : selected(); const QString id = text(record, "id");
    const QString title = create ? creationLabel() : type == Resource::Users ? tr("Edit user") : type == Resource::Groups ? tr("Edit group") : tr("Edit workspace");
    auto *dialog = dialogFor(this, title, tr("Manage details on %1.").arg(_state->account()->url().host()),
        type == Resource::Workspaces ? UDriveIcons::Symbol::Grid : UDriveIcons::Symbol::Members);
    dialog->setObjectName(QStringLiteral("driveAdminEditDialog"));
    auto *fieldsWidget = new QWidget(dialog); fieldsWidget->setObjectName(QStringLiteral("adminFormFields"));
    auto *form = new QFormLayout(fieldsWidget); form->setContentsMargins(0, 0, 4, 0); form->setSpacing(8);
    form->setRowWrapPolicy(QFormLayout::WrapAllRows); form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignLeft); form->setFormAlignment(Qt::AlignTop);
    QMap<QString, QLineEdit *> inputs;
    auto field = [&](const QString &key, const QString &caption, const QString &value, bool editable) {
        auto *input = new QLineEdit(value, fieldsWidget); input->setObjectName(QStringLiteral("adminField_") + key); input->setMaxLength(1024); input->setEnabled(editable);
        input->setAccessibleName(caption); input->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        form->addRow(caption, input); inputs.insert(key, input); return input;
    };
    if (type == Resource::Users) {
        field(QStringLiteral("onPremisesSamAccountName"), tr("Username"), text(record, "onPremisesSamAccountName"), create || _api->userFieldWritable(QStringLiteral("onPremisesSamAccountName")));
        field(QStringLiteral("displayName"), tr("Full name"), text(record, "displayName"), create || _api->userFieldWritable(QStringLiteral("displayName")));
        field(QStringLiteral("mail"), tr("Email"), text(record, "mail"), create || _api->userFieldWritable(QStringLiteral("mail")));
        auto *password = field(QStringLiteral("passwordProfile"), create ? tr("Password") : tr("New password (optional)"), {}, create || _api->userFieldWritable(QStringLiteral("passwordProfile")));
        password->setEchoMode(QLineEdit::Password); password->setAttribute(Qt::WA_InputMethodEnabled, false);
    } else if (type == Resource::Groups) {
        field(QStringLiteral("displayName"), tr("Group name"), text(record, "displayName"), true);
        field(QStringLiteral("description"), tr("Description"), text(record, "description"), true);
    } else {
        field(QStringLiteral("name"), tr("Workspace name"), text(record, "name"), create || _api->canUpdate(type));
        field(QStringLiteral("description"), tr("Description"), text(record, "description"), create || _api->canUpdate(type));
        const auto total = record.value(QStringLiteral("quota")).toObject().value(QStringLiteral("total"));
        auto *quota = field(QStringLiteral("quota"), tr("Quota in GB (optional)"), total.isDouble() ? QString::number(total.toDouble() / 1000000000.0, 'g', 12) : QString(), _api->canSetQuota());
        quota->setPlaceholderText(tr("Leave empty to keep the server default"));
    }
    auto *scroll = new QScrollArea(dialog); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); scroll->setWidget(fieldsWidget);
    scroll->setMinimumHeight(qMin(fieldsWidget->sizeHint().height() + 4, qMax(80, udrivePopupBounds(this).height() - 240)));
    dialog->layout()->addWidget(scroll);
    auto *error = label({}, dialog); error->setObjectName(QStringLiteral("adminFormError")); error->hide(); dialog->layout()->addWidget(error);
    auto reportError = [error](const QString &message) { error->setText(message); error->show(); };
    auto *buttons = dialogButtons(dialog, QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    if (create) buttons->button(QDialogButtonBox::Save)->setText(type == Resource::Users ? tr("Create user") : type == Resource::Groups ? tr("Create group") : tr("Create workspace"));
    else buttons->button(QDialogButtonBox::Save)->setText(tr("Save changes"));
    connect(_api, &UDriveAdminApi::cleared, dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, dialog, [this, dialog, inputs, record, id, type, create, reportError] {
        QJsonObject fields;
        for (auto it = inputs.begin(); it != inputs.end(); ++it) {
            if (!it.value()->isEnabled()) continue;
            const QString value = it.key() == QStringLiteral("passwordProfile") ? it.value()->text() : it.value()->text().trimmed();
            if ((it.key() == QStringLiteral("displayName") || it.key() == QStringLiteral("name") || it.key() == QStringLiteral("onPremisesSamAccountName")) && value.isEmpty()) {
                reportError(tr("Enter a name before saving.")); it.value()->setFocus(); return;
            }
            if (it.key() == QStringLiteral("passwordProfile")) {
                if (create && value.trimmed().isEmpty()) { reportError(tr("Enter a password for the new user.")); it.value()->setFocus(); return; }
                if (!value.isEmpty()) fields.insert(it.key(), QJsonObject{{QStringLiteral("password"), value}});
            } else if (it.key() == QStringLiteral("quota")) {
                if (value.isEmpty()) continue;
                bool ok = false; const double gb = value.toDouble(&ok);
                if (!ok || !std::isfinite(gb) || gb < 0 || gb > 9000000) { reportError(tr("Enter a valid quota in GB.")); it.value()->setFocus(); return; }
                if (create || gb * 1000000000.0 != record.value(QStringLiteral("quota")).toObject().value(QStringLiteral("total")).toDouble())
                    fields.insert(it.key(), QJsonObject{{QStringLiteral("total"), gb * 1000000000.0}});
            } else if (create || value != record.value(it.key()).toString()) fields.insert(it.key(), value);
        }
        if (fields.isEmpty()) { dialog->accept(); return; }
        _reloadAfterWrite = true;
        if (!_api->save(type, id, fields)) { _reloadAfterWrite = false; reportError(tr("This change is no longer permitted. Refresh and try again.")); return; }
        for (auto *input : inputs) if (input->echoMode() == QLineEdit::Password) input->clear();
        dialog->accept();
    });
    openDialog(dialog);
}
void UDriveAdminWidget::removeSelected()
{
    if (!_remove->isEnabled()) return;
    const auto type = resource(); const auto record = selected(); const auto id = text(record, "id");
    const bool restore = type == Resource::Workspaces && disabledWorkspace(record);
    const QString name = type == Resource::Users ? text(record, "onPremisesSamAccountName") : type == Resource::Groups ? text(record, "displayName") : text(record, "name");
    const QString title = type == Resource::Workspaces ? (restore ? tr("Enable workspace") : tr("Disable workspace"))
        : type == Resource::Users ? tr("Delete user") : tr("Delete group");
    const QString message = type == Resource::Workspaces ? (restore ? tr("Enable workspace “%1”?") : tr("Disable workspace “%1”? Members will lose access until it is enabled again."))
        : tr("Delete “%1” from this server? This operation cannot be undone.");
    auto *dialog = dialogFor(this, title, message.arg(name), restore ? UDriveIcons::Symbol::Restore : UDriveIcons::Symbol::Trash);
    dialog->setObjectName(QStringLiteral("driveAdminConfirmDialog"));
    auto *buttons = dialogButtons(dialog, QDialogButtonBox::Cancel);
    auto *confirm = buttons->addButton(_remove->text(), QDialogButtonBox::DestructiveRole);
    confirm->setProperty("dialogAction", restore ? QStringLiteral("primary") : QStringLiteral("danger"));
    buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
    connect(confirm, &QPushButton::clicked, dialog, [this, dialog, type, record, id, restore] {
        _reloadAfterWrite = true;
        const bool sent = restore ? _api->restoreWorkspace(id, text(record, "name")) : _api->remove(type, id);
        if (!sent) { _reloadAfterWrite = false; return; } dialog->accept();
    });
    openDialog(dialog);
}
void UDriveAdminWidget::toggleLogin()
{
    if (!_login->isEnabled()) return;
    const auto record = selected(); const QString id = text(record, "id"); const bool enable = !record.value(QStringLiteral("accountEnabled")).toBool(true);
    auto *dialog = dialogFor(this, enable ? tr("Allow sign-in") : tr("Block sign-in"),
        (enable ? tr("Allow “%1” to sign in?") : tr("Prevent “%1” from signing in?")).arg(text(record, "onPremisesSamAccountName")), UDriveIcons::Symbol::Disable);
    dialog->setObjectName(QStringLiteral("driveAdminLoginDialog"));
    auto *buttons = dialogButtons(dialog, QDialogButtonBox::Cancel | QDialogButtonBox::Save);
    auto *save = buttons->button(QDialogButtonBox::Save); save->setText(enable ? tr("Allow sign-in") : tr("Block sign-in"));
    if (!enable) { save->setProperty("dialogAction", QStringLiteral("danger")); save->setDefault(false); buttons->button(QDialogButtonBox::Cancel)->setDefault(true); }
    connect(buttons, &QDialogButtonBox::accepted, dialog, [this, dialog, id, enable] {
        _reloadAfterWrite = true; if (_api->setLogin(id, enable)) dialog->accept(); else _reloadAfterWrite = false;
    }); openDialog(dialog);
}
void UDriveAdminWidget::editRole()
{
    if (!_role->isEnabled()) return;
    const auto record = selected(); const auto id = text(record, "id");
    auto *dialog = dialogFor(this, tr("Change user role"), tr("Change the server role for “%1”. Administrator roles grant access to manage other users and server resources.").arg(text(record, "onPremisesSamAccountName")), UDriveIcons::Symbol::Members);
    dialog->setObjectName(QStringLiteral("driveAdminRoleDialog"));
    auto *roles = new UDriveComboBox(dialog); roles->setAccessibleName(tr("Server role"));
    roles->setProperty("udriveCombo", true);
    for (const auto &value : _api->roles()) {
        const auto role = value.toObject(); if (role.value(QStringLiteral("isEnabled")).toBool(true)) roles->addItem(text(role, "displayName"), role);
    }
    int assigned = -1;
    for (const auto &value : record.value(QStringLiteral("appRoleAssignments")).toArray()) {
        const auto assignment = value.toObject();
        for (int i = 0; i < roles->count(); ++i) {
            const auto role = roles->itemData(i).toJsonObject();
            if (text(role, "id") == text(assignment, "appRoleId") && text(role, "resourceId") == text(assignment, "resourceId")) assigned = i;
        }
    }
    roles->setCurrentIndex(assigned); roles->setPlaceholderText(tr("Choose a server role"));
    auto *caption = label(tr("Server role"), dialog); caption->setBuddy(roles); dialog->layout()->addWidget(caption);
    dialog->layout()->addWidget(roles); auto *buttons = dialogButtons(dialog, QDialogButtonBox::Cancel | QDialogButtonBox::Save);
    buttons->button(QDialogButtonBox::Save)->setText(tr("Update role"));
    buttons->button(QDialogButtonBox::Save)->setEnabled(assigned >= 0);
    connect(roles, &QComboBox::currentIndexChanged, dialog, [buttons](int index) { buttons->button(QDialogButtonBox::Save)->setEnabled(index >= 0); });
    connect(buttons, &QDialogButtonBox::accepted, dialog, [this, dialog, roles, id] {
        const auto role = roles->currentData().toJsonObject(); _reloadAfterWrite = true;
        if (_api->assignRole(id, text(role, "id"), text(role, "resourceId"))) dialog->accept(); else _reloadAfterWrite = false;
    }); openDialog(dialog);
}
void UDriveAdminWidget::editMembers()
{
    if (!_members->isEnabled()) return;
    const auto record = selected(); const QString group = text(record, "id");
    auto *dialog = dialogFor(this, tr("Group members"), tr("Manage who belongs to “%1”.").arg(text(record, "displayName")), UDriveIcons::Symbol::Members);
    dialog->setObjectName(QStringLiteral("driveAdminMembersDialog"));
    auto *members = new QListWidget(dialog); members->setObjectName(QStringLiteral("adminMembersList")); members->setMinimumHeight(160); dialog->layout()->addWidget(members);
    auto *input = new UDriveComboBox(dialog); input->setObjectName(QStringLiteral("adminMemberUser")); input->setEditable(true); input->setInsertPolicy(QComboBox::NoInsert);
    input->setProperty("udriveCombo", true); input->setMinimumContentsLength(16); input->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    input->lineEdit()->setPlaceholderText(tr("Choose a user to add"));
    input->setPlaceholderText(tr("Choose a user to add")); input->setAccessibleName(tr("User to add to this group")); input->completer()->setCaseSensitivity(Qt::CaseInsensitive);
    input->setEnabled(_api->canRead(Resource::Users));
    auto *pickerLabel = label(tr("Add a user"), dialog); pickerLabel->setBuddy(input); dialog->layout()->addWidget(pickerLabel);
    auto *addRow = new QHBoxLayout; addRow->setSpacing(10); addRow->addWidget(input, 1);
    auto *add = new QPushButton(UDriveIcons::icon(UDriveIcons::Symbol::Add), tr("Add member"), dialog); add->setAutoDefault(false); addRow->addWidget(add);
    qobject_cast<QVBoxLayout *>(dialog->layout())->addLayout(addRow);
    auto *notice = label(tr("Loading members…"), dialog); dialog->layout()->addWidget(notice);
    auto *buttons = dialogButtons(dialog, QDialogButtonBox::Close);
    auto *remove = buttons->addButton(tr("Remove member"), QDialogButtonBox::ActionRole); remove->setProperty("dialogAction", QStringLiteral("danger"));
    buttons->button(QDialogButtonBox::Close)->setText(tr("Done")); buttons->button(QDialogButtonBox::Close)->setDefault(true);
    add->setEnabled(false); remove->setEnabled(false);
    auto update = [this, add, remove, members, input] {
        const bool allowed = !_api->busy() && _api->canUpdate(Resource::Groups);
        add->setEnabled(allowed && input->isEnabled() && !input->currentData().toString().isEmpty() && input->currentText() == input->itemText(input->currentIndex()));
        remove->setEnabled(allowed && members->currentItem());
    };
    connect(_api, &UDriveAdminApi::busyChanged, dialog, update); connect(input, &QComboBox::currentTextChanged, dialog, update); connect(members, &QListWidget::itemSelectionChanged, dialog, update);
    connect(_api, &UDriveAdminApi::recordsLoaded, dialog, [this, input, group](Resource resource, const QJsonArray &records) {
        if (resource != Resource::Users) return;
        input->clear();
        for (const auto &entry : records) { const auto user = entry.toObject(); input->addItem(text(user, "displayName") + QStringLiteral(" — ") + text(user, "onPremisesSamAccountName"), text(user, "id")); }
        input->setCurrentIndex(-1); _api->loadMembers(group);
    });
    connect(_api, &UDriveAdminApi::membersLoaded, dialog, [members, notice](const QJsonArray &records) {
        members->clear(); for (const auto &entry : records) { const auto user = entry.toObject();
            auto *item = new QListWidgetItem(text(user, "displayName") + QStringLiteral(" — ") + text(user, "onPremisesSamAccountName"), members);
            item->setData(Qt::UserRole, text(user, "id")); item->setToolTip(text(user, "id")); }
        notice->setText(QObject::tr("%1 members").arg(records.size()));
    });
    connect(_api, &UDriveAdminApi::operationFinished, dialog, [dialog](bool success, const QString &message) {
        if (!message.isEmpty()) UDriveToast::showMessage(dialog, message, success ? UDriveToast::Kind::Success : UDriveToast::Kind::Error);
    });
    connect(add, &QPushButton::clicked, dialog, [this, group, input] { _api->setGroupMember(group, input->currentData().toString(), true); });
    connect(remove, &QPushButton::clicked, dialog, [this, group, members] {
        if (members->currentItem()) _api->setGroupMember(group, members->currentItem()->data(Qt::UserRole).toString(), false);
    });
    // Membership writes report their outcome, then reload the visible member
    // list. Closing the dialog disconnects these callbacks automatically.
    connect(_api, &UDriveAdminApi::operationFinished, dialog, [this, dialog, group](bool success, const QString &message) {
        if (success && !message.isEmpty() && dialog->isVisible()) _api->loadMembers(group);
    });
    openDialog(dialog);
    if (_api->canRead(Resource::Users)) _api->load(Resource::Users); else _api->loadMembers(group);
}
