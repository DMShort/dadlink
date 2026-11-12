#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <vector>

class QLineEdit;
class QTableWidget;
class QLabel;

/// Create/edit an op template: name, description and a grid of
/// nets (rows) × roles (columns), each cell "—" / "Listen" / "Talk",
/// plus per-net audio defaults (priority, pan, radio) pushed to members.
class TemplateEditor : public QDialog {
    Q_OBJECT

public:
    /// roles: [{id, name, color}], tmpl: existing template (empty object = new).
    TemplateEditor(const QJsonArray& roles, const QJsonObject& tmpl, QWidget* parent = nullptr);

    /// The template as the server expects it: {name, description, nets:[…]}.
    QJsonObject result() const;

private:
    struct Net {
        QString name;
        int priority = 5;
        int pan = 0;          // -100 … 100
        bool radio = false;
        QMap<int, QString> access; // role_id -> "listen" | "talk"
    };

    void rebuild();            // model -> table
    void collect();            // table -> model
    void addNet(const QString& name);
    void moveSelected(int delta);
    bool validate();
    void accept() override;

    QJsonArray m_roles;
    std::vector<Net> m_nets;

    QLineEdit* m_name = nullptr;
    QLineEdit* m_description = nullptr;
    QTableWidget* m_table = nullptr;
    QLabel* m_error = nullptr;

    static constexpr int ColName = 0, ColPriority = 1, ColPan = 2, ColRadio = 3, ColFirstRole = 4;
};
