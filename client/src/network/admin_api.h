#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QJsonObject>
#include <QJsonArray>
#include <functional>

/// HTTP client for the DadLink admin REST API.
/// All methods are asynchronous — results delivered via callbacks.
class AdminApi : public QObject {
    Q_OBJECT

public:
    using SuccessCallback = std::function<void(const QJsonObject& response)>;
    using ErrorCallback = std::function<void(int statusCode, const QString& error)>;

    explicit AdminApi(QObject* parent = nullptr);

    /// Set the server base URL and auth token.
    void configure(const QString& host, int port, bool useTls, const QString& token);

    // --- Users ---
    void listUsers(SuccessCallback onSuccess, ErrorCallback onError);
    void getUser(int userId, SuccessCallback onSuccess, ErrorCallback onError);
    void createUser(const QString& username, const QString& password,
                    const QString& displayName, bool isAdmin,
                    SuccessCallback onSuccess, ErrorCallback onError);
    void updateUser(int userId, const QJsonObject& fields,
                    SuccessCallback onSuccess, ErrorCallback onError);
    void deleteUser(int userId, SuccessCallback onSuccess, ErrorCallback onError);
    void revokeUserSessions(int userId, SuccessCallback onSuccess, ErrorCallback onError);

    // --- Channels ---
    void listChannels(SuccessCallback onSuccess, ErrorCallback onError);
    void createChannel(const QString& name, const QString& description,
                       int parentId, int maxUsers, int position,
                       SuccessCallback onSuccess, ErrorCallback onError);
    void updateChannel(int channelId, const QJsonObject& fields,
                       SuccessCallback onSuccess, ErrorCallback onError);
    void deleteChannel(int channelId, SuccessCallback onSuccess, ErrorCallback onError);

    // --- Roles ---
    void listRoles(SuccessCallback onSuccess, ErrorCallback onError);
    void createRole(const QString& name, int permissions, int priority,
                    const QString& color,
                    SuccessCallback onSuccess, ErrorCallback onError);
    void updateRole(int roleId, const QJsonObject& fields,
                    SuccessCallback onSuccess, ErrorCallback onError);
    void deleteRole(int roleId, SuccessCallback onSuccess, ErrorCallback onError);
    void assignRole(int roleId, int userId, SuccessCallback onSuccess, ErrorCallback onError);
    void removeRole(int roleId, int userId, SuccessCallback onSuccess, ErrorCallback onError);

    // --- Invites ---
    void listInvites(SuccessCallback onSuccess, ErrorCallback onError);
    void createInvite(int maxUses, int expiresInHours, SuccessCallback onSuccess, ErrorCallback onError);
    void revokeInvite(const QString& code, SuccessCallback onSuccess, ErrorCallback onError);

    // --- Ops (Admin or Op Leader) ---
    void listOpRoles(SuccessCallback onSuccess, ErrorCallback onError);
    void listOpTemplates(SuccessCallback onSuccess, ErrorCallback onError);
    void createOpTemplate(const QJsonObject& tmpl, SuccessCallback onSuccess, ErrorCallback onError);
    void updateOpTemplate(int templateId, const QJsonObject& tmpl, SuccessCallback onSuccess, ErrorCallback onError);
    void deleteOpTemplate(int templateId, SuccessCallback onSuccess, ErrorCallback onError);
    void listOps(SuccessCallback onSuccess, ErrorCallback onError);
    void startOp(int templateId, const QString& name, SuccessCallback onSuccess, ErrorCallback onError);
    void endOp(int opId, SuccessCallback onSuccess, ErrorCallback onError);

    // --- Audit ---
    void getAuditLog(int limit, int offset, SuccessCallback onSuccess, ErrorCallback onError);

private:
    void get(const QString& path, SuccessCallback onSuccess, ErrorCallback onError);
    void post(const QString& path, const QJsonObject& body,
              SuccessCallback onSuccess, ErrorCallback onError);
    void put(const QString& path, const QJsonObject& body,
             SuccessCallback onSuccess, ErrorCallback onError);
    void del(const QString& path, SuccessCallback onSuccess, ErrorCallback onError);

    QNetworkAccessManager m_manager;
    QString m_baseUrl;
    QString m_token;
};
