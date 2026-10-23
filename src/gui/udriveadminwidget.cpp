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
#include <cmath>

using namespace OCC;
using Resource = UDriveAdminApi::Resource;
namespace {
constexpr int recordRole = Qt::UserRole + 20;
class AdminActionsDelegate : public QStyledItemDelegate
{
public:
    AdminActionsDelegate(QTreeView *tree, std::function<void(const QModelIndex &)> open)
        : QStyledItemDelegate(tree), _tree(tree), _open(std::move(open))
    {
        tree->installEventFilter(this);
    }
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyledItemDelegate::paint(painter, option, index);
        if (isAction(index))
            UDriveIcons::icon(UDriveIcons::Symbol::MoreVertical).paint(painter, QRect(option.rect.center() - QPoint(9, 9), QSize(18, 18)));
    }
    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option, const QModelIndex &index) override
    {
        if (isAction(index) && event->type() == QEvent::MouseButtonRelease) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton && option.rect.contains(mouse->position().toPoint())) {
                _open(index); return true;
            }
        }
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == _tree && event->type() == QEvent::KeyPress) {
            const auto *key = static_cast<QKeyEvent *>(event);
            const bool activate = isAction(_tree->currentIndex()) && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter || key->key() == Qt::Key_Space);
            const bool context = key->key() == Qt::Key_Menu || (key->key() == Qt::Key_F10 && key->modifiers() == Qt::ShiftModifier);
            if ((activate || context) && _tree->currentIndex().isValid()) { _open(_tree->currentIndex()); return true; }
        }
        return QStyledItemDelegate::eventFilter(watched, event);
    }
private:
    static bool isAction(const QModelIndex &index) { return index.isValid() && index.column() == index.model()->columnCount() - 1; }
    QTreeView *_tree;
    std::function<void(const QModelIndex &)> _open;
};
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
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(24, 24, 24, 20); layout->setSpacing(16);
    auto *heading = label(tr("Administration"), this); heading->setObjectName(QStringLiteral("driveAdminTitle"));
    auto titleFont = heading->font(); titleFont.setPointSize(18); titleFont.setBold(true); heading->setFont(titleFont);
    auto *headingRow = new QHBoxLayout; headingRow->addWidget(heading, 1);
    auto *web = new QPushButton(UDriveIcons::icon(UDriveIcons::Symbol::Online), tr("Web administration"), this);
    web->setProperty("driveToolbarButton", true); web->setToolTip(tr("Open server administration, including additional web-only settings."));
    headingRow->addWidget(web); layout->addLayout(headingRow);
    connect(web, &QPushButton::clicked, this, [this] {
        if (_state && _state->account() && _api->available())
            Utility::openBrowser(Utility::concatUrlPath(_state->account()->url(), QStringLiteral("admin-settings")), this);
    });
    layout->addWidget(label(tr("Manage your server’s users, groups, and workspaces."), this));
    _tabs = new QTabBar(this); _tabs->setObjectName(QStringLiteral("driveAdminTabs")); _tabs->setExpanding(false); _tabs->setDrawBase(false);
    _tabs->addTab(UDriveIcons::icon(UDriveIcons::Symbol::Members), tr("Users"));
    _tabs->addTab(UDriveIcons::icon(UDriveIcons::Symbol::Share), tr("Groups"));
    _tabs->addTab(UDriveIcons::icon(UDriveIcons::Symbol::Grid), tr("Workspaces"));
    layout->addWidget(_tabs, 0, Qt::AlignLeft);
    auto *filters = new QHBoxLayout; filters->setSpacing(10);
    _search = new QLineEdit(this); _search->setObjectName(QStringLiteral("driveAdminSearch")); _search->setClearButtonEnabled(true);
    _search->addAction(UDriveIcons::icon(UDriveIcons::Symbol::Search), QLineEdit::LeadingPosition);
    _search->setPlaceholderText(tr("Search users")); _search->setAccessibleName(tr("Search administration entries"));
    filters->addWidget(_search, 1);
    auto combo = [this](const QString &name) {
        auto *box = new UDriveComboBox(this); box->setObjectName(name); box->setProperty("udriveCombo", true);
        box->setMinimumContentsLength(12); box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        box->setMinimumWidth(160); return box;
    };
    _groupFilter = combo(QStringLiteral("driveAdminGroupFilter")); _groupFilter->setAccessibleName(tr("Filter by group")); filters->addWidget(_groupFilter);
    _roleFilter = combo(QStringLiteral("driveAdminRoleFilter")); _roleFilter->setAccessibleName(tr("Filter by role")); filters->addWidget(_roleFilter);
    filters->addStretch();
    layout->addLayout(filters);
    auto *actions = new QHBoxLayout; actions->setSpacing(6);
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
    _refresh = button(QStringLiteral("driveAdminRefresh"), tr("Refresh"), UDriveIcons::Symbol::Refresh);
    layout->addLayout(actions);
    _status = label({}, this); _status->setObjectName(QStringLiteral("driveAdminStatus")); _status->hide(); layout->addWidget(_status);
    _model = new QStandardItemModel(this); _proxy = new QSortFilterProxyModel(this); _proxy->setSourceModel(_model);
    _proxy->setFilterCaseSensitivity(Qt::CaseInsensitive); _proxy->setFilterKeyColumn(-1); _proxy->setSortCaseSensitivity(Qt::CaseInsensitive);
    _tree = new QTreeView(this); _tree->setObjectName(QStringLiteral("driveAdminTable")); _tree->setAccessibleName(tr("Administration entries"));
    _tree->setModel(_proxy); _tree->setRootIsDecorated(false); _tree->setSortingEnabled(true); _tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    _tree->setSelectionMode(QAbstractItemView::SingleSelection); _tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _tree->setUniformRowHeights(true); _tree->header()->setStretchLastSection(false); _tree->header()->setSectionsMovable(false);
    _tree->setTabKeyNavigation(true);
    _tree->setItemDelegate(new AdminActionsDelegate(_tree, [this](const QModelIndex &index) {
        const auto action = index.siblingAtColumn(_proxy->columnCount() - 1);
        showRowActions(index, _tree->viewport()->mapToGlobal(_tree->visualRect(action).bottomRight()));
    }));
    _tree->setContextMenuPolicy(Qt::CustomContextMenu); layout->addWidget(_tree, 1);
    _empty = new QWidget(this); auto *emptyLayout = new QVBoxLayout(_empty); emptyLayout->addStretch();
    auto *emptyIcon = label({}, _empty); emptyIcon->setPixmap(UDriveIcons::icon(UDriveIcons::Symbol::Members).pixmap(48, 48)); emptyIcon->setAlignment(Qt::AlignCenter);
    emptyLayout->addWidget(emptyIcon); auto *emptyText = label(tr("No entries to display"), _empty); emptyText->setAlignment(Qt::AlignCenter); emptyLayout->addWidget(emptyText); emptyLayout->addStretch();
    layout->addWidget(_empty, 1); _empty->hide();
    _count = label({}, this); _count->setObjectName(QStringLiteral("driveAdminCount")); layout->addWidget(_count);
    connect(_search, &QLineEdit::textChanged, _proxy, &QSortFilterProxyModel::setFilterFixedString);
    connect(_proxy, &QAbstractItemModel::rowsInserted, this, &UDriveAdminWidget::updateActions);
    connect(_proxy, &QAbstractItemModel::rowsRemoved, this, &UDriveAdminWidget::updateActions);
    connect(_proxy, &QAbstractItemModel::modelReset, this, &UDriveAdminWidget::updateActions);
    connect(_tree->selectionModel(), &QItemSelectionModel::selectionChanged, this, &UDriveAdminWidget::updateActions);
    connect(_tabs, &QTabBar::currentChanged, this, [this] { if (!_api->busy()) refresh(); });
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
        _groupFilter->clear(); _groupFilter->addItem(tr("All groups"), QString());
        _roleFilter->clear(); _roleFilter->addItem(tr("All roles"), QString()); _roleFilter->addItem(tr("No role"), QStringLiteral("none"));
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
    });
    connect(_api, &UDriveAdminApi::operationFinished, this, [this](bool success, const QString &message) {
        const bool write = _reloadAfterWrite;
        _status->setText(message); _status->setVisible(!write && !message.isEmpty());
        if (write) UDriveToast::showMessage(this, message, success ? UDriveToast::Kind::Success : UDriveToast::Kind::Error);
        if (_refreshAfterAccess) { _refreshAfterAccess = false; if (success && isVisible() && _api->available()) refresh(); }
        else if (_reloadAfterWrite) { _reloadAfterWrite = false; if (success && isVisible() && _api->available()) refresh(); }
        updateActions();
    });
    auto *timer = new QTimer(this); timer->setInterval(60000);
    connect(timer, &QTimer::timeout, this, [this] {
        if (_api->busy()) return;
        for (auto *dialog : findChildren<QDialog *>()) if (dialog->isVisible()) return;
        if (isVisible()) activate(); else _api->refreshAccess();
    }); timer->start();
    updateAccess();
}
Resource UDriveAdminWidget::resource() const { return static_cast<Resource>(qMax(0, _tabs->currentIndex())); }
bool UDriveAdminWidget::canCreate() const { return !_api->busy() && _api->canCreate(resource()); }
QString UDriveAdminWidget::creationLabel() const
{
    return resource() == Resource::Users ? tr("New user") : resource() == Resource::Groups ? tr("New group") : tr("New workspace");
}
void UDriveAdminWidget::createNew() { if (canCreate()) edit(true); }
QJsonObject UDriveAdminWidget::selected() const
{
    return _tree->currentIndex().siblingAtColumn(0).data(recordRole).toJsonObject();
}
void UDriveAdminWidget::showEvent(QShowEvent *event) { QWidget::showEvent(event); activate(); }
void UDriveAdminWidget::hideEvent(QHideEvent *event)
{
    for (auto *dialog : findChildren<QDialog *>()) dialog->reject();
    QWidget::hideEvent(event);
}
void UDriveAdminWidget::activate()
{
    _refreshAfterAccess = true;
    if (_api->busy()) return;
    _status->setText(tr("Checking administration permissions…")); _status->show(); _api->refreshAccess();
}
QLineEdit *UDriveAdminWidget::takeSearchField()
{
    _search->setParent(nullptr); _search->setObjectName(QStringLiteral("driveSearch")); return _search;
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
    const auto type = resource(); const auto record = selected(); const bool one = !text(record, "id").isEmpty();
    const bool idle = !_api->busy() && _api->canRead(type); const bool users = type == Resource::Users;
    const bool self = users && text(record, "id") == _api->selfId();
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
    Q_EMIT controlsChanged();
}
void UDriveAdminWidget::render()
{
    const auto type = resource(); _model->clear();
    _search->setPlaceholderText(type == Resource::Users ? tr("Search users") : type == Resource::Groups ? tr("Search groups") : tr("Search workspaces"));
    _model->setHorizontalHeaderLabels(type == Resource::Users ? QStringList{tr("Username"), tr("Name"), tr("Email"), tr("Role"), tr("Login"), tr("Actions")}
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
        items.first()->setIcon(UDriveIcons::icon(type == Resource::Users ? UDriveIcons::Symbol::Members : type == Resource::Groups ? UDriveIcons::Symbol::Share : UDriveIcons::Symbol::Grid));
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
    _tree->header()->resizeSection(actionsColumn, qMax(76, _tree->fontMetrics().horizontalAdvance(tr("Actions")) + 24));
    _tree->sortByColumn(0, Qt::AscendingOrder); updateActions();
}
void UDriveAdminWidget::showActions(const QPoint &point)
{
    showRowActions(_tree->indexAt(point), _tree->viewport()->mapToGlobal(point));
}
void UDriveAdminWidget::showRowActions(const QModelIndex &index, const QPoint &globalPosition)
{
    if (!index.isValid() || _api->busy() || !_api->canRead(resource())) return;
    _tree->setCurrentIndex(index); updateActions();
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
    menu.exec(globalPosition);
}
void UDriveAdminWidget::edit(bool create)
{
    if (_api->busy() || !(create ? canCreate() : _edit->isEnabled())) return;
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
