#include "admin_api.h"

#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QUrl>

AdminApi::AdminApi(QObject* parent)
    : QObject(parent)
{
    // Ignore cert errors (hostname mismatch, self-signed) — same policy as WebSocket.
    connect(&m_manager, &QNetworkAccessManager::sslErrors,
            [](QNetworkReply* reply, const QList<QSslError>&) {
                reply->ignoreSslErrors();
            });
}

void AdminApi::configure(const QString& host, int port, bool useTls, const QString& token) {
    QString scheme = useTls ? "https" : "http";
    m_baseUrl = QString("%1://%2:%3").arg(scheme).arg(host).arg(port);
    m_token = token;
}

// --- Users ---

void AdminApi::listUsers(SuccessCallback onSuccess, ErrorCallback onError) {
    get("/admin/users", onSuccess, onError);
}

void AdminApi::getUser(int userId, SuccessCallback onSuccess, ErrorCallback onError) {
    get(QString("/admin/users/%1").arg(userId), onSuccess, onError);
}

void AdminApi::createUser(const QString& username, const QString& password,
                           const QString& displayName, bool isAdmin,
                           SuccessCallback onSuccess, ErrorCallback onError) {
    QJsonObject body;
    body["username"] = username;
    body["password"] = password;
    if (!displayName.isEmpty()) body["display_name"] = displayName;
    body["is_admin"] = isAdmin;
    post("/admin/users", body, onSuccess, onError);
}

void AdminApi::updateUser(int userId, const QJsonObject& fields,
                           SuccessCallback onSuccess, ErrorCallback onError) {
    put(QString("/admin/users/%1").arg(userId), fields, onSuccess, onError);
}

void AdminApi::deleteUser(int userId, SuccessCallback onSuccess, ErrorCallback onError) {
    del(QString("/admin/users/%1").arg(userId), onSuccess, onError);
}

void AdminApi::revokeUserSessions(int userId, SuccessCallback onSuccess, ErrorCallback onError) {
    del(QString("/admin/users/%1/sessions").arg(userId), onSuccess, onError);
}

// --- Channels ---

void AdminApi::listChannels(SuccessCallback onSuccess, ErrorCallback onError) {
    get("/admin/channels", onSuccess, onError);
}

void AdminApi::createChannel(const QString& name, const QString& description,
                              int parentId, int maxUsers, int position,
                              SuccessCallback onSuccess, ErrorCallback onError) {
    QJsonObject body;
    body["name"] = name;
    if (!description.isEmpty()) body["description"] = description;
    if (parentId > 0) body["parent_id"] = parentId;
    if (maxUsers > 0) body["max_users"] = maxUsers;
    body["position"] = position;
    post("/admin/channels", body, onSuccess, onError);
}

void AdminApi::updateChannel(int channelId, const QJsonObject& fields,
                              SuccessCallback onSuccess, ErrorCallback onError) {
    put(QString("/admin/channels/%1").arg(channelId), fields, onSuccess, onError);
}

void AdminApi::deleteChannel(int channelId, SuccessCallback onSuccess, ErrorCallback onError) {
    del(QString("/admin/channels/%1").arg(channelId), onSuccess, onError);
}

// --- Roles ---

void AdminApi::listRoles(SuccessCallback onSuccess, ErrorCallback onError) {
    get("/admin/roles", onSuccess, onError);
}

void AdminApi::createRole(const QString& name, int permissions, int priority,
                           const QString& color,
                           SuccessCallback onSuccess, ErrorCallback onError,
                           const QString& discordRoleId) {
    QJsonObject body;
    body["name"] = name;
    body["permissions"] = permissions;
    body["priority"] = priority;
    if (!color.isEmpty()) body["color"] = color;
    if (!discordRoleId.trimmed().isEmpty()) body["discord_role_id"] = discordRoleId.trimmed();
    post("/admin/roles", body, onSuccess, onError);
}

void AdminApi::updateRole(int roleId, const QJsonObject& fields,
                           SuccessCallback onSuccess, ErrorCallback onError) {
    put(QString("/admin/roles/%1").arg(roleId), fields, onSuccess, onError);
}

void AdminApi::deleteRole(int roleId, SuccessCallback onSuccess, ErrorCallback onError) {
    del(QString("/admin/roles/%1").arg(roleId), onSuccess, onError);
}

void AdminApi::assignRole(int roleId, int userId, SuccessCallback onSuccess, ErrorCallback onError) {
    post(QString("/admin/roles/%1/assign/%2").arg(roleId).arg(userId), {}, onSuccess, onError);
}

void AdminApi::removeRole(int roleId, int userId, SuccessCallback onSuccess, ErrorCallback onError) {
    del(QString("/admin/roles/%1/assign/%2").arg(roleId).arg(userId), onSuccess, onError);
}

// --- Invites ---

void AdminApi::listInvites(SuccessCallback onSuccess, ErrorCallback onError) {
    get("/admin/invites", onSuccess, onError);
}

void AdminApi::createInvite(int maxUses, int expiresInHours,
                             SuccessCallback onSuccess, ErrorCallback onError) {
    QJsonObject body;
    if (maxUses > 0)        body["max_uses"]         = maxUses;
    if (expiresInHours > 0) body["expires_in_hours"] = expiresInHours;
    post("/admin/invites", body, onSuccess, onError);
}

void AdminApi::revokeInvite(const QString& code, SuccessCallback onSuccess, ErrorCallback onError) {
    del(QString("/admin/invites/%1").arg(code), onSuccess, onError);
}

// --- Audit ---

void AdminApi::getAuditLog(int limit, int offset, SuccessCallback onSuccess, ErrorCallback onError) {
    get(QString("/admin/audit?limit=%1&offset=%2").arg(limit).arg(offset), onSuccess, onError);
}

// --- HTTP helpers ---

// --- Ops ---

void AdminApi::listOpRoles(SuccessCallback onSuccess, ErrorCallback onError) {
    get("/ops/roles", onSuccess, onError);
}

void AdminApi::listOpTemplates(SuccessCallback onSuccess, ErrorCallback onError) {
    get("/ops/templates", onSuccess, onError);
}

void AdminApi::createOpTemplate(const QJsonObject& tmpl, SuccessCallback onSuccess, ErrorCallback onError) {
    post("/ops/templates", tmpl, onSuccess, onError);
}

void AdminApi::updateOpTemplate(int templateId, const QJsonObject& tmpl,
                                SuccessCallback onSuccess, ErrorCallback onError) {
    put(QString("/ops/templates/%1").arg(templateId), tmpl, onSuccess, onError);
}

void AdminApi::deleteOpTemplate(int templateId, SuccessCallback onSuccess, ErrorCallback onError) {
    del(QString("/ops/templates/%1").arg(templateId), onSuccess, onError);
}

void AdminApi::listOps(SuccessCallback onSuccess, ErrorCallback onError) {
    get("/ops", onSuccess, onError);
}

void AdminApi::startOp(int templateId, const QString& name, SuccessCallback onSuccess, ErrorCallback onError) {
    QJsonObject body;
    body["template_id"] = templateId;
    if (!name.isEmpty()) body["name"] = name;
    post("/ops", body, onSuccess, onError);
}

void AdminApi::endOp(int opId, SuccessCallback onSuccess, ErrorCallback onError) {
    del(QString("/ops/%1").arg(opId), onSuccess, onError);
}

void AdminApi::get(const QString& path, SuccessCallback onSuccess, ErrorCallback onError) {
    QNetworkRequest request(QUrl(m_baseUrl + path));
    request.setRawHeader("Authorization", ("Bearer " + m_token).toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QNetworkReply* reply = m_manager.get(request);
    connect(reply, &QNetworkReply::finished, this, [reply, onSuccess, onError]() {
        reply->deleteLater();
        int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());

        if (status >= 200 && status < 300) {
            if (onSuccess) onSuccess(doc.object());
        } else {
            QString error = doc.object()["error"].toString(reply->errorString());
            if (onError) onError(status, error);
        }
    });
}

void AdminApi::post(const QString& path, const QJsonObject& body,
                     SuccessCallback onSuccess, ErrorCallback onError) {
    QNetworkRequest request(QUrl(m_baseUrl + path));
    request.setRawHeader("Authorization", ("Bearer " + m_token).toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonDocument doc(body);
    QNetworkReply* reply = m_manager.post(request, doc.toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [reply, onSuccess, onError]() {
        reply->deleteLater();
        int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());

        if (status >= 200 && status < 300) {
            if (onSuccess) onSuccess(doc.object());
        } else {
            QString error = doc.object()["error"].toString(reply->errorString());
            if (onError) onError(status, error);
        }
    });
}

void AdminApi::put(const QString& path, const QJsonObject& body,
                    SuccessCallback onSuccess, ErrorCallback onError) {
    QNetworkRequest request(QUrl(m_baseUrl + path));
    request.setRawHeader("Authorization", ("Bearer " + m_token).toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonDocument doc(body);
    QNetworkReply* reply = m_manager.put(request, doc.toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [reply, onSuccess, onError]() {
        reply->deleteLater();
        int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());

        if (status >= 200 && status < 300) {
            if (onSuccess) onSuccess(doc.object());
        } else {
            QString error = doc.object()["error"].toString(reply->errorString());
            if (onError) onError(status, error);
        }
    });
}

void AdminApi::del(const QString& path, SuccessCallback onSuccess, ErrorCallback onError) {
    QNetworkRequest request(QUrl(m_baseUrl + path));
    request.setRawHeader("Authorization", ("Bearer " + m_token).toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QNetworkReply* reply = m_manager.deleteResource(request);
    connect(reply, &QNetworkReply::finished, this, [reply, onSuccess, onError]() {
        reply->deleteLater();
        int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());

        if (status >= 200 && status < 300) {
            if (onSuccess) onSuccess(doc.object());
        } else {
            QString error = doc.object()["error"].toString(reply->errorString());
            if (onError) onError(status, error);
        }
    });
}
