#include "websocket_client.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QSslConfiguration>
#include <QDebug>
#include <algorithm>

WebSocketClient::WebSocketClient(QObject* parent)
    : QObject(parent)
{
    connect(&m_socket, &QWebSocket::connected, this, &WebSocketClient::onConnected);
    connect(&m_socket, &QWebSocket::disconnected, this, &WebSocketClient::onDisconnected);
    connect(&m_socket, &QWebSocket::textMessageReceived, this, &WebSocketClient::onTextMessageReceived);
    connect(&m_socket, &QWebSocket::sslErrors, this, &WebSocketClient::onSslErrors);

    m_pingTimer.setInterval(PingIntervalMs);
    connect(&m_pingTimer, &QTimer::timeout, this, &WebSocketClient::onPingTimer);

    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &WebSocketClient::onReconnectTimer);
}

WebSocketClient::~WebSocketClient() {
    m_intentionalDisconnect = true;
    m_pingTimer.stop();
    m_reconnectTimer.stop();
    m_socket.close();
}

void WebSocketClient::connectToServer(const QString& host, int port, bool useTls) {
    m_host = host;
    m_port = port;
    m_useTls = useTls;
    m_intentionalDisconnect = false;

    setState(State::Connecting);

    QString scheme = useTls ? "wss" : "ws";
    QUrl url(QString("%1://%2:%3/ws").arg(scheme).arg(host).arg(port));

    if (useTls) {
        QSslConfiguration sslConfig = QSslConfiguration::defaultConfiguration();
        // Always allow self-signed certs — we handle trust via certificate pinning.
        // QWebSocket silently drops connections on SSL errors without emitting
        // sslErrors on some platforms, so VerifyPeer is not usable with self-signed certs.
        sslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);
        m_socket.setSslConfiguration(sslConfig);
    }

    m_socket.open(url);
}

void WebSocketClient::disconnect() {
    m_intentionalDisconnect = true;
    m_autoReconnect = false;
    stopPingTimer();
    m_reconnectTimer.stop();
    m_socket.close();
}

void WebSocketClient::authenticatePassword(const QString& username, const QString& password, const QString& orgTag) {
    m_pendingMethod = "password";
    m_pendingUsername = username;
    m_pendingPassword = password;
    m_pendingOrgTag = orgTag;
    m_pendingToken.clear();

    QJsonObject payload;
    payload["method"] = "password";
    payload["username"] = username;
    payload["password"] = password;
    payload["org_tag"] = orgTag;

    setState(State::Authenticating);
    sendMessage("authenticate", payload);
}

void WebSocketClient::authenticateToken(const QString& token) {
    m_pendingMethod = "token";
    m_pendingToken = token;
    m_pendingUsername.clear();
    m_pendingPassword.clear();

    QJsonObject payload;
    payload["method"] = "token";
    payload["token"] = token;

    setState(State::Authenticating);
    sendMessage("authenticate", payload);
}

void WebSocketClient::sendMessage(const QString& type, const QJsonObject& payload) {
    QJsonObject envelope;
    envelope["type"] = type;
    envelope["payload"] = payload;

    QJsonDocument doc(envelope);
    m_socket.sendTextMessage(doc.toJson(QJsonDocument::Compact));
}

// --- Private slots ---

void WebSocketClient::onConnected() {
    resetReconnect();
    setState(State::WaitingForChallenge);
    startPingTimer();
    emit connected();
}

void WebSocketClient::onDisconnected() {
    stopPingTimer();
    State prevState = m_state;
    setState(State::Disconnected);
    emit disconnected();

    if (!m_intentionalDisconnect && m_autoReconnect) {
        scheduleReconnect();
    }
}

void WebSocketClient::onTextMessageReceived(const QString& message) {
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        qWarning() << "WebSocket: malformed JSON received:" << parseError.errorString();
        return;
    }

    QJsonObject root = doc.object();
    QString type = root["type"].toString();
    QJsonObject payload = root["payload"].toObject();

    dispatchMessage(type, payload);
}

void WebSocketClient::onSslErrors(const QList<QSslError>& errors) {
    // Cert errors (hostname mismatch, self-signed, unknown CA) are expected when
    // connecting to a server whose cert hostname differs from the address typed
    // (e.g. localhost vs scda-link.duckdns.org). VerifyNone is already set so
    // we just tell Qt to proceed. Only escalate genuine protocol-level failures.
    bool onlyCertErrors = std::all_of(errors.begin(), errors.end(), [](const QSslError& e) {
        switch (e.error()) {
            case QSslError::HostNameMismatch:
            case QSslError::SelfSignedCertificate:
            case QSslError::SelfSignedCertificateInChain:
            case QSslError::UnableToGetLocalIssuerCertificate:
            case QSslError::UnableToVerifyFirstCertificate:
            case QSslError::CertificateUntrusted:
            case QSslError::InvalidCaCertificate:
                return true;
            default:
                return false;
        }
    });

    m_socket.ignoreSslErrors();

    if (!onlyCertErrors) {
        emit sslErrorOccurred(errors);
    }
}

void WebSocketClient::onPingTimer() {
    if (m_state == State::Connected || m_state == State::WaitingForChallenge) {
        m_pingStopwatch.start();
        sendMessage("ping");
    }
}

void WebSocketClient::onReconnectTimer() {
    if (m_intentionalDisconnect) return;

    m_reconnectAttempt++;
    if (m_reconnectAttempt > MaxReconnectAttempts) {
        emit reconnectFailed();
        return;
    }

    emit reconnecting(m_reconnectAttempt);
    setState(State::Reconnecting);
    connectToServer(m_host, m_port, m_useTls);
}

// --- Private methods ---

void WebSocketClient::setState(State newState) {
    if (m_state != newState) {
        m_state = newState;
        emit stateChanged(newState);
    }
}

void WebSocketClient::dispatchMessage(const QString& type, const QJsonObject& payload) {
    if (type == "challenge") {
        QStringList methods;
        for (const auto& v : payload["methods"].toArray()) {
            methods.append(v.toString());
        }
        setState(State::WaitingForChallenge);
        emit challengeReceived(methods);

        // If reconnecting with saved credentials, auto-authenticate
        if (m_reconnectAttempt > 0 && !m_pendingMethod.isEmpty()) {
            if (m_pendingMethod == "token" && !m_pendingToken.isEmpty()) {
                authenticateToken(m_pendingToken);
            } else if (m_pendingMethod == "password" && !m_pendingUsername.isEmpty()) {
                authenticatePassword(m_pendingUsername, m_pendingPassword, m_pendingOrgTag);
            }
        }
    }
    else if (type == "auth_success") {
        int userId = payload["user_id"].toInt();
        QString token = payload["token"].toString();
        int voicePort = payload["voice_port"].toInt();
        QJsonArray channels = payload["channels"].toArray();
        QJsonArray users = payload["users"].toArray();

        // Fresh session: the server re-sends running ops right after auth_success
        m_activeOps.clear();

        // Save token for reconnection
        m_pendingToken = token;
        m_pendingMethod = "token";

        setState(State::Connected);
        emit authSuccess(userId, token, voicePort, channels, users);

        // Check for client update
        QString latestVersion = payload["client_version"].toString();
        if (!latestVersion.isEmpty()) {
            QString currentVersion = QStringLiteral(DADLINK_VERSION);
            if (latestVersion != currentVersion) {
                QString downloadUrl = payload["client_download_url"].toString();
                emit updateAvailable(latestVersion, currentVersion, downloadUrl);
            }
        }
    }
    else if (type == "auth_failed") {
        QString reason = payload["reason"].toString();
        setState(State::WaitingForChallenge);
        emit authFailed(reason);
    }
    else if (type == "key_exchange_init") {
        emit keyExchangeInit(payload["public_key"].toString());
    }
    else if (type == "key_exchange_complete") {
        emit keyExchangeComplete();
    }
    else if (type == "channel_joined") {
        // can_speak absent = older server = full access
        emit channelSpeakAccess(payload["channel_id"].toInt(), payload["can_speak"].toBool(true));
        emit channelJoined(payload["channel_id"].toInt(), payload["roster"].toArray());
    }
    else if (type == "op_started") {
        QJsonObject op = payload["op"].toObject();
        m_activeOps[op["id"].toInt()] = op;
        emit opStarted(op);
    }
    else if (type == "op_ended") {
        int opId = payload["op_id"].toInt();
        m_activeOps.remove(opId);
        emit opEnded(opId, payload["name"].toString());
    }
    else if (type == "user_joined") {
        qDebug() << "[WS] Received user_joined: channel_id=" << payload["channel_id"].toInt()
                 << "user=" << payload["user"].toObject()["username"].toString();
        emit userJoined(payload["channel_id"].toInt(), payload["user"].toObject());
    }
    else if (type == "user_left") {
        emit userLeft(payload["channel_id"].toInt(), payload["user_id"].toInt());
    }
    else if (type == "user_state_changed") {
        emit userStateChanged(
            payload["channel_id"].toInt(),
            payload["user_id"].toInt(),
            payload["muted"].toBool(),
            payload["deafened"].toBool(),
            payload["talking"].toBool()
        );
    }
    else if (type == "channel_list") {
        emit channelListReceived(payload["channels"].toArray());
    }
    else if (type == "channel_updated") {
        emit channelUpdated(payload["channel"].toObject());
    }
    else if (type == "channel_deleted") {
        emit channelDeleted(payload["channel_id"].toInt());
    }
    else if (type == "user_count_changed") {
        emit userCountChanged(payload["channel_id"].toInt(), payload["user_count"].toInt());
    }
    else if (type == "pong") {
        if (m_pingStopwatch.isValid()) {
            m_latencyMs = static_cast<int>(m_pingStopwatch.elapsed());
        }
        emit pongReceived(m_latencyMs);
    }
    else if (type == "error") {
        emit errorReceived(payload["code"].toString(), payload["message"].toString());
    }
    else {
        qWarning() << "WebSocket: unknown message type:" << type;
    }
}

void WebSocketClient::startPingTimer() {
    m_pingTimer.start();
}

void WebSocketClient::stopPingTimer() {
    m_pingTimer.stop();
}

void WebSocketClient::scheduleReconnect() {
    int delay = std::min(BaseReconnectMs * (1 << m_reconnectAttempt), MaxReconnectMs);
    m_reconnectTimer.start(delay);
}

void WebSocketClient::resetReconnect() {
    m_reconnectAttempt = 0;
    m_reconnectTimer.stop();
}
