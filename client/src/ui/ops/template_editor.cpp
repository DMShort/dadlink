#include "template_editor.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSet>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include <cmath>

namespace {
constexpr const char* kNone = "—";

QWidget* centred(QWidget* w, QWidget* parent) {
    auto* box = new QWidget(parent);
    auto* l = new QHBoxLayout(box);
    l->setContentsMargins(0, 0, 0, 0);
    l->setAlignment(Qt::AlignCenter);
    l->addWidget(w);
    return box;
}
}

TemplateEditor::TemplateEditor(const QJsonArray& roles, const QJsonObject& tmpl, QWidget* parent)
    : QDialog(parent)
    , m_roles(roles)
{
    setWindowTitle(tmpl.isEmpty() ? "New Op Template" : "Edit Op Template");
    setMinimumSize(820, 460);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    auto* layout = new QVBoxLayout(this);

    auto* form = new QFormLayout();
    m_name = new QLineEdit(tmpl["name"].toString(), this);
    m_name->setPlaceholderText("e.g. Standard Fleet Op");
    m_name->setMaxLength(64);
    m_description = new QLineEdit(tmpl["description"].toString(), this);
    m_description->setPlaceholderText("Optional");
    form->addRow("Name:", m_name);
    form->addRow("Description:", m_description);
    layout->addLayout(form);

    auto* help = new QLabel(
        "Each row is a net. For each role choose <b>Listen</b> or <b>Talk</b>; members are "
        "auto-joined to the nets their roles are on. A net with no roles set is open to everyone "
        "(but nobody is auto-joined). Priority, pan and radio are pushed to members' clients.", this);
    help->setWordWrap(true);
    help->setStyleSheet("color: #bbb; padding: 2px;");
    layout->addWidget(help);

    m_table = new QTableWidget(this);
    m_table->verticalHeader()->hide();
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(m_table, 1);

    auto* rowButtons = new QHBoxLayout();
    auto* addBtn = new QPushButton("Add net", this);
    auto* removeBtn = new QPushButton("Remove net", this);
    auto* upBtn = new QPushButton("Move up", this);
    auto* downBtn = new QPushButton("Move down", this);
    rowButtons->addWidget(addBtn);
    rowButtons->addWidget(removeBtn);
    rowButtons->addStretch();
    rowButtons->addWidget(upBtn);
    rowButtons->addWidget(downBtn);
    layout->addLayout(rowButtons);

    m_error = new QLabel(this);
    m_error->setStyleSheet("color: #ff6b6b;");
    m_error->hide();
    layout->addWidget(m_error);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &TemplateEditor::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(addBtn, &QPushButton::clicked, this, [this]() {
        collect();
        addNet(QString("Net %1").arg(m_nets.size() + 1));
        rebuild();
        m_table->selectRow(static_cast<int>(m_nets.size()) - 1);
    });
    connect(removeBtn, &QPushButton::clicked, this, [this]() {
        int row = m_table->currentRow();
        if (row < 0 || row >= static_cast<int>(m_nets.size())) return;
        collect();
        m_nets.erase(m_nets.begin() + row);
        rebuild();
    });
    connect(upBtn, &QPushButton::clicked, this, [this]() { moveSelected(-1); });
    connect(downBtn, &QPushButton::clicked, this, [this]() { moveSelected(+1); });

    // Load existing nets, or a sensible starting layout
    const QJsonArray nets = tmpl["nets"].toArray();
    for (const auto& v : nets) {
        QJsonObject n = v.toObject();
        Net net;
        net.name = n["name"].toString();
        net.priority = n["priority"].toInt(5);
        net.pan = static_cast<int>(std::lround(n["pan"].toDouble() * 100));
        net.radio = n["radio"].toBool();
        for (const auto& r : n["roles"].toArray()) {
            QJsonObject ra = r.toObject();
            net.access[ra["role_id"].toInt()] = ra["access"].toString();
        }
        m_nets.push_back(net);
    }
    if (m_nets.empty()) {
        addNet("Command");
        m_nets.back().priority = 1;
        addNet("Fleet");
        m_nets.back().pan = -60;
        m_nets.back().radio = true;
        addNet("Ground");
        m_nets.back().pan = 60;
    }
    rebuild();
}

void TemplateEditor::addNet(const QString& name) {
    Net n;
    n.name = name;
    m_nets.push_back(n);
}

void TemplateEditor::rebuild() {
    const int roleCount = m_roles.size();
    m_table->clear();
    m_table->setColumnCount(ColFirstRole + roleCount);
    QStringList headers{"Net", "Priority", "Pan", "Radio"};
    for (const auto& r : m_roles) headers << r.toObject()["name"].toString();
    m_table->setHorizontalHeaderLabels(headers);
    m_table->horizontalHeader()->setSectionResizeMode(ColName, QHeaderView::Stretch);
    m_table->setColumnWidth(ColPriority, 70);
    m_table->setColumnWidth(ColPan, 80);
    m_table->setColumnWidth(ColRadio, 55);
    for (int c = 0; c < roleCount; ++c) m_table->setColumnWidth(ColFirstRole + c, 95);

    m_table->setRowCount(static_cast<int>(m_nets.size()));
    for (int row = 0; row < static_cast<int>(m_nets.size()); ++row) {
        const Net& n = m_nets[row];
        m_table->setItem(row, ColName, new QTableWidgetItem(n.name));

        auto* prio = new QSpinBox(this);
        prio->setRange(1, 10);
        prio->setValue(n.priority);
        prio->setToolTip("1 = highest. Higher-priority nets duck lower ones when active.");
        m_table->setCellWidget(row, ColPriority, prio);

        auto* pan = new QSpinBox(this);
        pan->setRange(-100, 100);
        pan->setSingleStep(10);
        pan->setValue(n.pan);
        pan->setToolTip("-100 = left ear, 0 = centre, 100 = right ear");
        m_table->setCellWidget(row, ColPan, pan);

        auto* radio = new QCheckBox(this);
        radio->setChecked(n.radio);
        radio->setToolTip("Comms-radio voice effect");
        m_table->setCellWidget(row, ColRadio, centred(radio, this));

        for (int c = 0; c < roleCount; ++c) {
            int roleId = m_roles[c].toObject()["id"].toInt();
            auto* combo = new QComboBox(this);
            combo->addItem(kNone, QString());
            combo->addItem("Listen", "listen");
            combo->addItem("Talk", "talk");
            int idx = combo->findData(n.access.value(roleId));
            combo->setCurrentIndex(idx < 0 ? 0 : idx);
            m_table->setCellWidget(row, ColFirstRole + c, combo);
        }
    }
}

void TemplateEditor::collect() {
    for (int row = 0; row < m_table->rowCount() && row < static_cast<int>(m_nets.size()); ++row) {
        Net& n = m_nets[row];
        if (auto* item = m_table->item(row, ColName)) n.name = item->text().trimmed();
        if (auto* w = qobject_cast<QSpinBox*>(m_table->cellWidget(row, ColPriority))) n.priority = w->value();
        if (auto* w = qobject_cast<QSpinBox*>(m_table->cellWidget(row, ColPan))) n.pan = w->value();
        if (auto* box = m_table->cellWidget(row, ColRadio)) {
            if (auto* cb = box->findChild<QCheckBox*>()) n.radio = cb->isChecked();
        }
        n.access.clear();
        for (int c = 0; c < m_roles.size(); ++c) {
            auto* combo = qobject_cast<QComboBox*>(m_table->cellWidget(row, ColFirstRole + c));
            if (!combo) continue;
            QString access = combo->currentData().toString();
            if (!access.isEmpty()) n.access[m_roles[c].toObject()["id"].toInt()] = access;
        }
    }
}

void TemplateEditor::moveSelected(int delta) {
    int row = m_table->currentRow();
    int target = row + delta;
    if (row < 0 || target < 0 || target >= static_cast<int>(m_nets.size())) return;
    collect();
    std::swap(m_nets[row], m_nets[target]);
    rebuild();
    m_table->selectRow(target);
}

bool TemplateEditor::validate() {
    collect();
    QString err;
    QSet<QString> seen;
    if (m_name->text().trimmed().isEmpty()) err = "Give the template a name.";
    else if (m_nets.empty()) err = "Add at least one net.";
    else {
        for (const auto& n : m_nets) {
            if (n.name.isEmpty()) { err = "Every net needs a name."; break; }
            if (seen.contains(n.name.toLower())) { err = QString("Two nets are called \"%1\".").arg(n.name); break; }
            seen.insert(n.name.toLower());
        }
    }
    m_error->setText(err);
    m_error->setVisible(!err.isEmpty());
    return err.isEmpty();
}

void TemplateEditor::accept() {
    if (validate()) QDialog::accept();
}

QJsonObject TemplateEditor::result() const {
    QJsonObject out;
    out["name"] = m_name->text().trimmed();
    QString desc = m_description->text().trimmed();
    if (!desc.isEmpty()) out["description"] = desc;
    QJsonArray nets;
    for (int i = 0; i < static_cast<int>(m_nets.size()); ++i) {
        const Net& n = m_nets[i];
        QJsonObject o;
        o["name"] = n.name;
        o["position"] = i;
        o["priority"] = n.priority;
        o["pan"] = n.pan / 100.0;
        o["radio"] = n.radio;
        QJsonArray roles;
        for (auto it = n.access.constBegin(); it != n.access.constEnd(); ++it) {
            QJsonObject ra;
            ra["role_id"] = it.key();
            ra["access"] = it.value();
            roles.append(ra);
        }
        o["roles"] = roles;
        nets.append(o);
    }
    out["nets"] = nets;
    return out;
}
