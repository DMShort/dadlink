#include "ops_manager.h"
#include "template_editor.h"
#include "network/admin_api.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLineEdit>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

OpsManager::OpsManager(AdminApi* api, QWidget* parent)
    : QDialog(parent)
    , m_api(api)
{
    setWindowTitle("Op Manager");
    setWindowFlags(Qt::Window);
    resize(760, 460);

    auto* layout = new QVBoxLayout(this);
    auto* tabs = new QTabWidget(this);
    tabs->addTab(createTemplatesTab(), "Templates");
    tabs->addTab(createActiveTab(), "Active Ops");
    layout->addWidget(tabs, 1);

    m_status = new QLabel(this);
    m_status->setStyleSheet("color: #aaa; padding: 2px;");
    layout->addWidget(m_status);
}

QWidget* OpsManager::createTemplatesTab() {
    auto* w = new QWidget(this);
    auto* l = new QVBoxLayout(w);

    m_templateTable = new QTableWidget(0, 3, w);
    m_templateTable->setHorizontalHeaderLabels({"Template", "Nets", "Description"});
    m_templateTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_templateTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_templateTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_templateTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_templateTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_templateTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_templateTable->verticalHeader()->hide();
    l->addWidget(m_templateTable, 1);

    auto* buttons = new QHBoxLayout();
    auto* newBtn = new QPushButton("New…", w);
    auto* editBtn = new QPushButton("Edit…", w);
    auto* delBtn = new QPushButton("Delete", w);
    auto* startBtn = new QPushButton("Start op…", w);
    startBtn->setStyleSheet("font-weight: bold;");
    buttons->addWidget(newBtn);
    buttons->addWidget(editBtn);
    buttons->addWidget(delBtn);
    buttons->addStretch();
    buttons->addWidget(startBtn);
    l->addLayout(buttons);

    connect(newBtn, &QPushButton::clicked, this, [this]() { editTemplate(-1); });
    connect(editBtn, &QPushButton::clicked, this, [this]() { editTemplate(selectedTemplateRow()); });
    connect(m_templateTable, &QTableWidget::cellDoubleClicked, this, [this](int row, int) { editTemplate(row); });
    connect(startBtn, &QPushButton::clicked, this, &OpsManager::startOpFromSelected);
    connect(delBtn, &QPushButton::clicked, this, [this]() {
        int row = selectedTemplateRow();
        if (row < 0) { setStatus("Select a template first.", true); return; }
        QJsonObject t = m_templates[row].toObject();
        if (QMessageBox::question(this, "Delete template",
                QString("Delete template \"%1\"? Running ops are not affected.").arg(t["name"].toString()))
            != QMessageBox::Yes) return;
        m_api->deleteOpTemplate(t["id"].toInt(),
            [this](const QJsonObject&) { setStatus("Template deleted."); loadTemplates(); },
            [this](int, const QString& e) { setStatus("Delete failed: " + e, true); });
    });
    return w;
}

QWidget* OpsManager::createActiveTab() {
    auto* w = new QWidget(this);
    auto* l = new QVBoxLayout(w);

    m_opsTable = new QTableWidget(0, 3, w);
    m_opsTable->setHorizontalHeaderLabels({"Op", "Nets", "Started"});
    m_opsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_opsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_opsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_opsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_opsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_opsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_opsTable->verticalHeader()->hide();
    l->addWidget(m_opsTable, 1);

    auto* buttons = new QHBoxLayout();
    auto* refreshBtn = new QPushButton("Refresh", w);
    auto* endBtn = new QPushButton("End op", w);
    endBtn->setStyleSheet("color: #ff8080;");
    buttons->addWidget(refreshBtn);
    buttons->addStretch();
    buttons->addWidget(endBtn);
    l->addLayout(buttons);

    connect(refreshBtn, &QPushButton::clicked, this, &OpsManager::loadOps);
    connect(endBtn, &QPushButton::clicked, this, &OpsManager::endSelectedOp);
    return w;
}

void OpsManager::refresh() {
    m_api->listOpRoles(
        [this](const QJsonObject& r) { m_roles = r["roles"].toArray(); loadTemplates(); },
        [this](int, const QString& e) { setStatus("Could not load roles: " + e, true); });
    loadOps();
}

void OpsManager::loadTemplates() {
    m_api->listOpTemplates(
        [this](const QJsonObject& r) {
            m_templates = r["templates"].toArray();
            m_templateTable->setRowCount(m_templates.size());
            for (int i = 0; i < m_templates.size(); ++i) {
                QJsonObject t = m_templates[i].toObject();
                QStringList nets;
                for (const auto& n : t["nets"].toArray()) nets << n.toObject()["name"].toString();
                m_templateTable->setItem(i, 0, new QTableWidgetItem(t["name"].toString()));
                m_templateTable->setItem(i, 1, new QTableWidgetItem(nets.join(", ")));
                m_templateTable->setItem(i, 2, new QTableWidgetItem(t["description"].toString()));
            }
            setStatus(QString("%1 template(s)").arg(m_templates.size()));
        },
        [this](int, const QString& e) { setStatus("Could not load templates: " + e, true); });
}

void OpsManager::loadOps() {
    m_api->listOps(
        [this](const QJsonObject& r) {
            m_ops = r["ops"].toArray();
            m_opsTable->setRowCount(m_ops.size());
            for (int i = 0; i < m_ops.size(); ++i) {
                QJsonObject o = m_ops[i].toObject();
                QStringList nets;
                for (const auto& n : o["nets"].toArray()) nets << n.toObject()["name"].toString();
                QDateTime started = QDateTime::fromString(o["started_at"].toString(), Qt::ISODateWithMs);
                m_opsTable->setItem(i, 0, new QTableWidgetItem(o["name"].toString()));
                m_opsTable->setItem(i, 1, new QTableWidgetItem(nets.join(", ")));
                m_opsTable->setItem(i, 2, new QTableWidgetItem(
                    started.isValid() ? started.toLocalTime().toString("ddd HH:mm") : QString()));
            }
        },
        [this](int, const QString& e) { setStatus("Could not load ops: " + e, true); });
}

int OpsManager::selectedTemplateRow() const {
    int row = m_templateTable->currentRow();
    return (row >= 0 && row < m_templates.size()) ? row : -1;
}

void OpsManager::editTemplate(int row) {
    QJsonObject existing = row >= 0 ? m_templates[row].toObject() : QJsonObject();
    if (m_roles.isEmpty()) {
        setStatus("No roles yet. Create roles in the Admin Panel first so you can assign them to nets.", true);
    }
    TemplateEditor editor(m_roles, existing, this);
    if (editor.exec() != QDialog::Accepted) return;

    auto ok = [this](const QJsonObject&) { setStatus("Template saved."); loadTemplates(); };
    auto fail = [this](int, const QString& e) { setStatus("Save failed: " + e, true); };
    if (row >= 0) m_api->updateOpTemplate(existing["id"].toInt(), editor.result(), ok, fail);
    else          m_api->createOpTemplate(editor.result(), ok, fail);
}

void OpsManager::startOpFromSelected() {
    int row = selectedTemplateRow();
    if (row < 0) { setStatus("Select a template to start an op from.", true); return; }
    QJsonObject t = m_templates[row].toObject();

    bool ok = false;
    QString name = QInputDialog::getText(this, "Start op",
        QString("Op name (template: %1):").arg(t["name"].toString()),
        QLineEdit::Normal, t["name"].toString(), &ok).trimmed();
    if (!ok) return;

    m_api->startOp(t["id"].toInt(), name,
        [this](const QJsonObject& r) {
            setStatus(QString("Op \"%1\" started. Members have been notified.")
                          .arg(r["op"].toObject()["name"].toString()));
            loadOps();
        },
        [this](int, const QString& e) { setStatus("Could not start op: " + e, true); });
}

void OpsManager::endSelectedOp() {
    int row = m_opsTable->currentRow();
    if (row < 0 || row >= m_ops.size()) { setStatus("Select an op to end.", true); return; }
    QJsonObject o = m_ops[row].toObject();
    if (QMessageBox::question(this, "End op",
            QString("End \"%1\"? Its channels are removed and everyone in them is disconnected from those nets.")
                .arg(o["name"].toString()))
        != QMessageBox::Yes) return;

    m_api->endOp(o["id"].toInt(),
        [this](const QJsonObject&) { setStatus("Op ended."); loadOps(); },
        [this](int, const QString& e) { setStatus("Could not end op: " + e, true); });
}

void OpsManager::setStatus(const QString& text, bool error) {
    m_status->setText(text);
    m_status->setStyleSheet(error ? "color: #ff6b6b; padding: 2px;" : "color: #aaa; padding: 2px;");
}
