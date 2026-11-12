#pragma once

#include <QDialog>
#include <QJsonArray>

class AdminApi;
class QTableWidget;
class QLabel;

/// Op Manager (Admin / Op Leader): build templates and start or end live ops.
class OpsManager : public QDialog {
    Q_OBJECT

public:
    explicit OpsManager(AdminApi* api, QWidget* parent = nullptr);

    /// Reload roles, templates and active ops from the server.
    void refresh();

private:
    QWidget* createTemplatesTab();
    QWidget* createActiveTab();
    void loadTemplates();
    void loadOps();
    int selectedTemplateRow() const;
    void editTemplate(int row); // -1 = new
    void startOpFromSelected();
    void endSelectedOp();
    void setStatus(const QString& text, bool error = false);

    AdminApi* m_api;
    QJsonArray m_roles;
    QJsonArray m_templates;
    QJsonArray m_ops;
    QTableWidget* m_templateTable = nullptr;
    QTableWidget* m_opsTable = nullptr;
    QLabel* m_status = nullptr;
};
