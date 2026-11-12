#include "admin_panel.h"
#include "network/admin_api.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QMessageBox>
#include <QJsonArray>
#include <QJsonObject>
#include <QDialog>
#include <QFormLayout>
#include <QLineEdit>
#include <QCheckBox>
#include <QSpinBox>
#include <QDialogButtonBox>
#include <QClipboard>
#include <QApplication>
#include <QDateTime>
#include <QGridLayout>
#include <functional>
#include <memory>
#include <vector>

namespace {
// Must match server/src/auth/permissions.rs
struct PermDef { int bit; const char* label; const char* tip; };
const PermDef kPerms[] = {
    {1,   "Join channels",   "Reserved: per-channel access is set by channel ACLs / op templates"},
    {2,   "Speak",           "Reserved: per-channel talk rights are set by channel ACLs / op templates"},
    {4,   "Move users",      ""},
    {8,   "Manage channels", ""},
    {16,  "Manage users",    ""},
    {32,  "Manage roles",    ""},
    {64,  "View audit log",  ""},
    {128, "Admin",           "Full access to everything, including the Admin Panel"},
    {256, "Op Leader",       "Create op templates and start/end ops (Ops menu) without full admin"},
};

QString permissionNames(int mask) {
    QStringList names;
    for (const auto& p : kPerms) if (mask & p.bit) names << p.label;
    return names.isEmpty() ? QStringLiteral("—") : names.join(", ");
}

/// Checkbox list for a role's permission bits. Returns a getter for the resulting mask.
std::function<int()> addPermissionPicker(QFormLayout* form, QWidget* parent, int initial) {
    auto* box = new QWidget(parent);
    auto* grid = new QGridLayout(box);
    grid->setContentsMargins(0, 0, 0, 0);
    auto checks = std::make_shared<std::vector<std::pair<QCheckBox*, int>>>();
    int i = 0;
    for (const auto& p : kPerms) {
        auto* cb = new QCheckBox(p.label, box);
        cb->setChecked(initial & p.bit);
        if (*p.tip) cb->setToolTip(p.tip);
        grid->addWidget(cb, i / 2, i % 2);
        checks->push_back({cb, p.bit});
        ++i;
    }
    form->addRow("Permissions:", box);
    return [checks]() {
        int mask = 0;
        for (const auto& [cb, bit] : *checks) if (cb->isChecked()) mask |= bit;
        return mask;
    };
}
}


AdminPanel::AdminPanel(AdminApi* api, QWidget* parent)
    : QWidget(parent)
    , m_api(api)
{
    setupUi();
    connect(m_tabs, &QTabWidget::currentChanged, this, &AdminPanel::onTabChanged);
}

void AdminPanel::setupUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_tabs = new QTabWidget(this);
    m_tabs->addTab(createUsersTab(), "Users");
    m_tabs->addTab(createChannelsTab(), "Channels");
    m_tabs->addTab(createRolesTab(), "Roles");
    m_tabs->addTab(createInvitesTab(), "Invites");
    layout->addWidget(m_tabs);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setStyleSheet("padding: 4px;");
    layout->addWidget(m_statusLabel);
}

QWidget* AdminPanel::createUsersTab() {
    auto* widget = new QWidget(this);
    auto* layout = new QVBoxLayout(widget);

    m_usersTable = new QTableWidget(this);
    m_usersTable->setColumnCount(5);
    m_usersTable->setHorizontalHeaderLabels({"ID", "Username", "Display Name", "Admin", "Active"});
    m_usersTable->horizontalHeader()->setStretchLastSection(true);
    m_usersTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_usersTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_usersTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(m_usersTable);

    auto* btnLayout = new QHBoxLayout();
    auto* addBtn = new QPushButton("Add User", this);
    auto* editBtn = new QPushButton("Edit User", this);
    auto* deleteBtn = new QPushButton("Delete User", this);
    auto* revokeBtn = new QPushButton("Revoke Sessions", this);
    auto* refreshBtn = new QPushButton("Refresh", this);

    btnLayout->addWidget(addBtn);
    btnLayout->addWidget(editBtn);
    btnLayout->addWidget(deleteBtn);
    btnLayout->addWidget(revokeBtn);
    btnLayout->addStretch();
    btnLayout->addWidget(refreshBtn);
    layout->addLayout(btnLayout);

    connect(addBtn, &QPushButton::clicked, this, &AdminPanel::onAddUser);
    connect(editBtn, &QPushButton::clicked, this, &AdminPanel::onEditUser);
    connect(deleteBtn, &QPushButton::clicked, this, &AdminPanel::onDeleteUser);
    connect(revokeBtn, &QPushButton::clicked, this, &AdminPanel::onRevokeSessions);
    connect(refreshBtn, &QPushButton::clicked, this, &AdminPanel::refreshUsers);

    return widget;
}

QWidget* AdminPanel::createChannelsTab() {
    auto* widget = new QWidget(this);
    auto* layout = new QVBoxLayout(widget);

    m_channelsTable = new QTableWidget(this);
    m_channelsTable->setColumnCount(5);
    m_channelsTable->setHorizontalHeaderLabels({"ID", "Name", "Description", "Max Users", "Position"});
    m_channelsTable->horizontalHeader()->setStretchLastSection(true);
    m_channelsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_channelsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_channelsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(m_channelsTable);

    auto* btnLayout = new QHBoxLayout();
    auto* addBtn = new QPushButton("Add Channel", this);
    auto* editBtn = new QPushButton("Edit Channel", this);
    auto* deleteBtn = new QPushButton("Delete Channel", this);
    auto* refreshBtn = new QPushButton("Refresh", this);

    btnLayout->addWidget(addBtn);
    btnLayout->addWidget(editBtn);
    btnLayout->addWidget(deleteBtn);
    btnLayout->addStretch();
    btnLayout->addWidget(refreshBtn);
    layout->addLayout(btnLayout);

    connect(addBtn, &QPushButton::clicked, this, &AdminPanel::onAddChannel);
    connect(editBtn, &QPushButton::clicked, this, &AdminPanel::onEditChannel);
    connect(deleteBtn, &QPushButton::clicked, this, &AdminPanel::onDeleteChannel);
    connect(refreshBtn, &QPushButton::clicked, this, &AdminPanel::refreshChannels);

    return widget;
}

QWidget* AdminPanel::createRolesTab() {
    auto* widget = new QWidget(this);
    auto* layout = new QVBoxLayout(widget);

    m_rolesTable = new QTableWidget(this);
    m_rolesTable->setColumnCount(5);
    m_rolesTable->setHorizontalHeaderLabels({"ID", "Name", "Permissions", "Priority", "Color"});
    m_rolesTable->horizontalHeader()->setStretchLastSection(true);
    m_rolesTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_rolesTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_rolesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(m_rolesTable);

    auto* btnLayout = new QHBoxLayout();
    auto* addBtn = new QPushButton("Add Role", this);
    auto* editBtn = new QPushButton("Edit Role", this);
    auto* deleteBtn = new QPushButton("Delete Role", this);
    auto* assignBtn = new QPushButton("Assign to User", this);
    auto* refreshBtn = new QPushButton("Refresh", this);

    btnLayout->addWidget(addBtn);
    btnLayout->addWidget(editBtn);
    btnLayout->addWidget(deleteBtn);
    btnLayout->addWidget(assignBtn);
    btnLayout->addStretch();
    btnLayout->addWidget(refreshBtn);
    layout->addLayout(btnLayout);

    connect(addBtn, &QPushButton::clicked, this, &AdminPanel::onAddRole);
    connect(editBtn, &QPushButton::clicked, this, &AdminPanel::onEditRole);
    connect(deleteBtn, &QPushButton::clicked, this, &AdminPanel::onDeleteRole);
    connect(assignBtn, &QPushButton::clicked, this, &AdminPanel::onAssignRole);
    connect(refreshBtn, &QPushButton::clicked, this, &AdminPanel::refreshRoles);

    return widget;
}

void AdminPanel::refresh() {
    refreshUsers();
    refreshChannels();
    refreshRoles();
    refreshInvites();
}

void AdminPanel::onTabChanged(int index) {
    switch (index) {
    case 0: refreshUsers(); break;
    case 1: refreshChannels(); break;
    case 2: refreshRoles(); break;
    case 3: refreshInvites(); break;
    }
}

// --- Users ---

void AdminPanel::refreshUsers() {
    m_api->listUsers(
        [this](const QJsonObject& response) {
            auto users = response["users"].toArray();
            m_usersTable->setRowCount(users.size());
            for (int i = 0; i < users.size(); ++i) {
                auto u = users[i].toObject();
                m_usersTable->setItem(i, 0, new QTableWidgetItem(QString::number(u["id"].toInt())));
                m_usersTable->setItem(i, 1, new QTableWidgetItem(u["username"].toString()));
                m_usersTable->setItem(i, 2, new QTableWidgetItem(u["display_name"].toString()));
                m_usersTable->setItem(i, 3, new QTableWidgetItem(u["is_admin"].toBool() ? "Yes" : "No"));
                m_usersTable->setItem(i, 4, new QTableWidgetItem(u["is_active"].toBool() ? "Yes" : "No"));
            }
            showSuccess(QString("Loaded %1 users").arg(users.size()));
        },
        [this](int, const QString& err) { showError("Failed to load users: " + err); }
    );
}

void AdminPanel::onAddUser() {
    QDialog dialog(this);
    dialog.setWindowTitle("Add User");
    auto* form = new QFormLayout(&dialog);

    auto* username = new QLineEdit(&dialog);
    auto* password = new QLineEdit(&dialog);
    password->setEchoMode(QLineEdit::Password);
    auto* displayName = new QLineEdit(&dialog);
    auto* isAdmin = new QCheckBox(&dialog);

    form->addRow("Username:", username);
    form->addRow("Password:", password);
    form->addRow("Display Name:", displayName);
    form->addRow("Admin:", isAdmin);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) return;

    m_api->createUser(username->text(), password->text(), displayName->text(), isAdmin->isChecked(),
        [this](const QJsonObject&) { showSuccess("User created"); refreshUsers(); },
        [this](int, const QString& err) { showError("Failed to create user: " + err); }
    );
}

void AdminPanel::onEditUser() {
    int userId = selectedUserId();
    if (userId <= 0) { showError("No user selected"); return; }

    QDialog dialog(this);
    dialog.setWindowTitle("Edit User");
    auto* form = new QFormLayout(&dialog);

    auto* displayName = new QLineEdit(&dialog);
    auto* password = new QLineEdit(&dialog);
    password->setEchoMode(QLineEdit::Password);
    password->setPlaceholderText("Leave blank to keep current");
    auto* isAdmin = new QCheckBox(&dialog);
    auto* isActive = new QCheckBox(&dialog);
    isActive->setChecked(true);

    form->addRow("Display Name:", displayName);
    form->addRow("New Password:", password);
    form->addRow("Admin:", isAdmin);
    form->addRow("Active:", isActive);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) return;

    QJsonObject fields;
    if (!displayName->text().isEmpty()) fields["display_name"] = displayName->text();
    if (!password->text().isEmpty()) fields["password"] = password->text();
    fields["is_admin"] = isAdmin->isChecked();
    fields["is_active"] = isActive->isChecked();

    m_api->updateUser(userId, fields,
        [this](const QJsonObject&) { showSuccess("User updated"); refreshUsers(); },
        [this](int, const QString& err) { showError("Failed to update user: " + err); }
    );
}

void AdminPanel::onDeleteUser() {
    int userId = selectedUserId();
    if (userId <= 0) { showError("No user selected"); return; }

    if (QMessageBox::question(this, "Delete User",
            QString("Are you sure you want to delete user #%1?").arg(userId))
        != QMessageBox::Yes) return;

    m_api->deleteUser(userId,
        [this](const QJsonObject&) { showSuccess("User deleted"); refreshUsers(); },
        [this](int, const QString& err) { showError("Failed to delete user: " + err); }
    );
}

void AdminPanel::onRevokeSessions() {
    int userId = selectedUserId();
    if (userId <= 0) { showError("No user selected"); return; }

    m_api->revokeUserSessions(userId,
        [this](const QJsonObject&) { showSuccess("Sessions revoked"); },
        [this](int, const QString& err) { showError("Failed to revoke sessions: " + err); }
    );
}

// --- Channels ---

void AdminPanel::refreshChannels() {
    m_api->listChannels(
        [this](const QJsonObject& response) {
            auto channels = response["channels"].toArray();
            m_channelsTable->setRowCount(channels.size());
            for (int i = 0; i < channels.size(); ++i) {
                auto ch = channels[i].toObject();
                m_channelsTable->setItem(i, 0, new QTableWidgetItem(QString::number(ch["id"].toInt())));
                m_channelsTable->setItem(i, 1, new QTableWidgetItem(ch["name"].toString()));
                m_channelsTable->setItem(i, 2, new QTableWidgetItem(ch["description"].toString()));
                m_channelsTable->setItem(i, 3, new QTableWidgetItem(QString::number(ch["max_users"].toInt())));
                m_channelsTable->setItem(i, 4, new QTableWidgetItem(QString::number(ch["position"].toInt())));
            }
            showSuccess(QString("Loaded %1 channels").arg(channels.size()));
        },
        [this](int, const QString& err) { showError("Failed to load channels: " + err); }
    );
}

void AdminPanel::onAddChannel() {
    QDialog dialog(this);
    dialog.setWindowTitle("Add Channel");
    auto* form = new QFormLayout(&dialog);

    auto* name = new QLineEdit(&dialog);
    auto* description = new QLineEdit(&dialog);
    auto* maxUsers = new QSpinBox(&dialog);
    maxUsers->setRange(0, 1000);
    maxUsers->setSpecialValueText("Unlimited");
    auto* position = new QSpinBox(&dialog);
    position->setRange(0, 999);

    form->addRow("Name:", name);
    form->addRow("Description:", description);
    form->addRow("Max Users:", maxUsers);
    form->addRow("Position:", position);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) return;

    m_api->createChannel(name->text(), description->text(), 0, maxUsers->value(), position->value(),
        [this](const QJsonObject&) { showSuccess("Channel created"); refreshChannels(); },
        [this](int, const QString& err) { showError("Failed to create channel: " + err); }
    );
}

void AdminPanel::onEditChannel() {
    int channelId = selectedChannelId();
    if (channelId <= 0) { showError("No channel selected"); return; }

    QDialog dialog(this);
    dialog.setWindowTitle("Edit Channel");
    auto* form = new QFormLayout(&dialog);

    auto* name = new QLineEdit(&dialog);
    auto* description = new QLineEdit(&dialog);
    auto* maxUsers = new QSpinBox(&dialog);
    maxUsers->setRange(0, 1000);
    auto* position = new QSpinBox(&dialog);
    position->setRange(0, 999);

    form->addRow("Name:", name);
    form->addRow("Description:", description);
    form->addRow("Max Users:", maxUsers);
    form->addRow("Position:", position);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) return;

    QJsonObject fields;
    if (!name->text().isEmpty()) fields["name"] = name->text();
    if (!description->text().isEmpty()) fields["description"] = description->text();
    fields["max_users"] = maxUsers->value();
    fields["position"] = position->value();

    m_api->updateChannel(channelId, fields,
        [this](const QJsonObject&) { showSuccess("Channel updated"); refreshChannels(); },
        [this](int, const QString& err) { showError("Failed to update channel: " + err); }
    );
}

void AdminPanel::onDeleteChannel() {
    int channelId = selectedChannelId();
    if (channelId <= 0) { showError("No channel selected"); return; }

    if (QMessageBox::question(this, "Delete Channel",
            QString("Are you sure you want to delete channel #%1?\nAll users will be kicked.").arg(channelId))
        != QMessageBox::Yes) return;

    m_api->deleteChannel(channelId,
        [this](const QJsonObject&) { showSuccess("Channel deleted"); refreshChannels(); },
        [this](int, const QString& err) { showError("Failed to delete channel: " + err); }
    );
}

// --- Roles ---

void AdminPanel::refreshRoles() {
    m_api->listRoles(
        [this](const QJsonObject& response) {
            auto roles = response["roles"].toArray();
            m_rolesTable->setRowCount(roles.size());
            for (int i = 0; i < roles.size(); ++i) {
                auto r = roles[i].toObject();
                m_rolesTable->setItem(i, 0, new QTableWidgetItem(QString::number(r["id"].toInt())));
                m_rolesTable->setItem(i, 1, new QTableWidgetItem(r["name"].toString()));
                auto* permItem = new QTableWidgetItem(permissionNames(r["permissions"].toInt()));
                permItem->setData(Qt::UserRole, r["permissions"].toInt());
                m_rolesTable->setItem(i, 2, permItem);
                m_rolesTable->setItem(i, 3, new QTableWidgetItem(QString::number(r["priority"].toInt())));
                m_rolesTable->setItem(i, 4, new QTableWidgetItem(r["color"].toString()));
            }
            showSuccess(QString("Loaded %1 roles").arg(roles.size()));
        },
        [this](int, const QString& err) { showError("Failed to load roles: " + err); }
    );
}

void AdminPanel::onAddRole() {
    QDialog dialog(this);
    dialog.setWindowTitle("Add Role");
    auto* form = new QFormLayout(&dialog);

    auto* name = new QLineEdit(&dialog);
    auto* priority = new QSpinBox(&dialog);
    priority->setRange(0, 999);
    auto* color = new QLineEdit(&dialog);
    color->setPlaceholderText("#FF0000");

    form->addRow("Name:", name);
    auto permissions = addPermissionPicker(form, &dialog, 1 | 2);
    form->addRow("Priority:", priority);
    form->addRow("Color:", color);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) return;

    m_api->createRole(name->text(), permissions(), priority->value(), color->text(),
        [this](const QJsonObject&) { showSuccess("Role created"); refreshRoles(); },
        [this](int, const QString& err) { showError("Failed to create role: " + err); }
    );
}

void AdminPanel::onEditRole() {
    int roleId = selectedRoleId();
    if (roleId <= 0) { showError("No role selected"); return; }

    QDialog dialog(this);
    dialog.setWindowTitle("Edit Role");
    auto* form = new QFormLayout(&dialog);

    // Prefill with the role's current values (previously every edit reset permissions to 0)
    const auto selected = m_rolesTable->selectedItems();
    const int row = selected.isEmpty() ? -1 : selected.first()->row();
    auto cell = [this, row](int col) { auto* it = m_rolesTable->item(row, col); return it ? it->text() : QString(); };
    const int currentPerms = (row >= 0 && m_rolesTable->item(row, 2))
        ? m_rolesTable->item(row, 2)->data(Qt::UserRole).toInt() : 0;

    auto* name = new QLineEdit(cell(1), &dialog);
    auto* priority = new QSpinBox(&dialog);
    priority->setRange(0, 999);
    priority->setValue(cell(3).toInt());
    auto* color = new QLineEdit(cell(4), &dialog);

    form->addRow("Name:", name);
    auto permissions = addPermissionPicker(form, &dialog, currentPerms);
    form->addRow("Priority:", priority);
    form->addRow("Color:", color);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) return;

    QJsonObject fields;
    if (!name->text().isEmpty()) fields["name"] = name->text();
    fields["permissions"] = permissions();
    fields["priority"] = priority->value();
    if (!color->text().isEmpty()) fields["color"] = color->text();

    m_api->updateRole(roleId, fields,
        [this](const QJsonObject&) { showSuccess("Role updated"); refreshRoles(); },
        [this](int, const QString& err) { showError("Failed to update role: " + err); }
    );
}

void AdminPanel::onDeleteRole() {
    int roleId = selectedRoleId();
    if (roleId <= 0) { showError("No role selected"); return; }

    if (QMessageBox::question(this, "Delete Role",
            QString("Are you sure you want to delete role #%1?").arg(roleId))
        != QMessageBox::Yes) return;

    m_api->deleteRole(roleId,
        [this](const QJsonObject&) { showSuccess("Role deleted"); refreshRoles(); },
        [this](int, const QString& err) { showError("Failed to delete role: " + err); }
    );
}

void AdminPanel::onAssignRole() {
    int roleId = selectedRoleId();
    if (roleId <= 0) { showError("No role selected"); return; }

    bool ok;
    int userId = QInputDialog::getInt(this, "Assign Role",
        "Enter User ID to assign this role to:", 0, 1, 999999, 1, &ok);
    if (!ok) return;

    m_api->assignRole(roleId, userId,
        [this](const QJsonObject&) { showSuccess("Role assigned"); },
        [this](int, const QString& err) { showError("Failed to assign role: " + err); }
    );
}

// --- Invites ---

QWidget* AdminPanel::createInvitesTab() {
    auto* widget = new QWidget(this);
    auto* layout = new QVBoxLayout(widget);

    m_invitesTable = new QTableWidget(this);
    m_invitesTable->setColumnCount(4);
    m_invitesTable->setHorizontalHeaderLabels({"Code", "Uses", "Expires", "Created"});
    m_invitesTable->horizontalHeader()->setStretchLastSection(true);
    m_invitesTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_invitesTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_invitesTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_invitesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(m_invitesTable);

    auto* btnLayout = new QHBoxLayout();
    auto* createBtn = new QPushButton("Create Invite", this);
    auto* copyBtn   = new QPushButton("Copy Code", this);
    auto* revokeBtn = new QPushButton("Revoke", this);
    auto* refreshBtn = new QPushButton("Refresh", this);

    btnLayout->addWidget(createBtn);
    btnLayout->addWidget(copyBtn);
    btnLayout->addWidget(revokeBtn);
    btnLayout->addStretch();
    btnLayout->addWidget(refreshBtn);
    layout->addLayout(btnLayout);

    connect(createBtn,  &QPushButton::clicked, this, &AdminPanel::onCreateInvite);
    connect(copyBtn,    &QPushButton::clicked, this, &AdminPanel::onCopyInviteCode);
    connect(revokeBtn,  &QPushButton::clicked, this, &AdminPanel::onRevokeInvite);
    connect(refreshBtn, &QPushButton::clicked, this, &AdminPanel::refreshInvites);

    return widget;
}

void AdminPanel::refreshInvites() {
    m_api->listInvites(
        [this](const QJsonObject& response) {
            auto invites = response["invites"].toArray();
            m_invitesTable->setRowCount(invites.size());
            for (int i = 0; i < invites.size(); ++i) {
                auto inv = invites[i].toObject();

                QString uses = inv["max_uses"].isNull()
                    ? QString("%1 / unlimited").arg(inv["use_count"].toInt())
                    : QString("%1 / %2").arg(inv["use_count"].toInt()).arg(inv["max_uses"].toInt());

                QString expires = inv["expires_at"].isNull()
                    ? "Never"
                    : QDateTime::fromString(inv["expires_at"].toString(), Qt::ISODateWithMs)
                          .toLocalTime().toString("dd MMM yyyy hh:mm");

                QString created = QDateTime::fromString(inv["created_at"].toString(), Qt::ISODateWithMs)
                                      .toLocalTime().toString("dd MMM yyyy hh:mm");

                m_invitesTable->setItem(i, 0, new QTableWidgetItem(inv["code"].toString()));
                m_invitesTable->setItem(i, 1, new QTableWidgetItem(uses));
                m_invitesTable->setItem(i, 2, new QTableWidgetItem(expires));
                m_invitesTable->setItem(i, 3, new QTableWidgetItem(created));
            }
            showSuccess(QString("%1 active invite code(s)").arg(invites.size()));
        },
        [this](int, const QString& err) { showError("Failed to load invites: " + err); }
    );
}

void AdminPanel::onCreateInvite() {
    QDialog dialog(this);
    dialog.setWindowTitle("Create Invite Code");
    auto* form = new QFormLayout(&dialog);

    auto* maxUses = new QSpinBox(&dialog);
    maxUses->setRange(0, 10000);
    maxUses->setValue(0);
    maxUses->setSpecialValueText("Unlimited");

    auto* expiryHours = new QSpinBox(&dialog);
    expiryHours->setRange(0, 8760);
    expiryHours->setValue(168);
    expiryHours->setSuffix(" hours");
    expiryHours->setSpecialValueText("Never expires");

    form->addRow("Max Uses:", maxUses);
    form->addRow("Expires In:", expiryHours);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) return;

    m_api->createInvite(maxUses->value(), expiryHours->value(),
        [this](const QJsonObject& response) {
            QString code = response["invite"].toObject()["code"].toString();

            // Show code prominently with a Copy button
            QDialog result(this);
            result.setWindowTitle("Invite Code Created");
            result.setFixedWidth(320);
            auto* layout = new QVBoxLayout(&result);

            auto* label = new QLabel("Share this code with your members:", &result);
            label->setAlignment(Qt::AlignCenter);
            layout->addWidget(label);

            auto* codeLabel = new QLabel(code, &result);
            codeLabel->setAlignment(Qt::AlignCenter);
            codeLabel->setStyleSheet("font-size: 28px; font-weight: bold; font-family: monospace; "
                                     "padding: 12px; background: #f0f0f0; border-radius: 4px;");
            layout->addWidget(codeLabel);

            auto* copyBtn = new QPushButton("Copy to Clipboard", &result);
            connect(copyBtn, &QPushButton::clicked, [code, copyBtn]() {
                QApplication::clipboard()->setText(code);
                copyBtn->setText("Copied!");
            });
            layout->addWidget(copyBtn);

            auto* closeBtn = new QPushButton("Close", &result);
            connect(closeBtn, &QPushButton::clicked, &result, &QDialog::accept);
            layout->addWidget(closeBtn);

            result.exec();
            refreshInvites();
        },
        [this](int, const QString& err) { showError("Failed to create invite: " + err); }
    );
}

void AdminPanel::onRevokeInvite() {
    QString code = selectedInviteCode();
    if (code.isEmpty()) { showError("No invite selected"); return; }

    if (QMessageBox::question(this, "Revoke Invite",
            QString("Revoke invite code %1?\nAnyone with this code will no longer be able to register.").arg(code))
        != QMessageBox::Yes) return;

    m_api->revokeInvite(code,
        [this](const QJsonObject&) { showSuccess("Invite revoked"); refreshInvites(); },
        [this](int, const QString& err) { showError("Failed to revoke invite: " + err); }
    );
}

void AdminPanel::onCopyInviteCode() {
    QString code = selectedInviteCode();
    if (code.isEmpty()) { showError("No invite selected"); return; }
    QApplication::clipboard()->setText(code);
    showSuccess(QString("Copied: %1").arg(code));
}

// --- Helpers ---

int AdminPanel::selectedUserId() const {
    auto items = m_usersTable->selectedItems();
    if (items.isEmpty()) return -1;
    int row = items.first()->row();
    return m_usersTable->item(row, 0)->text().toInt();
}

int AdminPanel::selectedChannelId() const {
    auto items = m_channelsTable->selectedItems();
    if (items.isEmpty()) return -1;
    int row = items.first()->row();
    return m_channelsTable->item(row, 0)->text().toInt();
}

int AdminPanel::selectedRoleId() const {
    auto items = m_rolesTable->selectedItems();
    if (items.isEmpty()) return -1;
    int row = items.first()->row();
    return m_rolesTable->item(row, 0)->text().toInt();
}

QString AdminPanel::selectedInviteCode() const {
    auto items = m_invitesTable->selectedItems();
    if (items.isEmpty()) return {};
    return m_invitesTable->item(items.first()->row(), 0)->text();
}

void AdminPanel::showError(const QString& message) {
    m_statusLabel->setText(message);
    m_statusLabel->setStyleSheet("color: red; padding: 4px;");
}

void AdminPanel::showSuccess(const QString& message) {
    m_statusLabel->setText(message);
    m_statusLabel->setStyleSheet("color: green; padding: 4px;");
}
