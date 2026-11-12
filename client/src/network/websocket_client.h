#pragma once

#include <QMap>

#include <QObject>
#include <QWebSocket>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>
#include <QElapsedTimer>
#include <QSslError>

/// WebSocket client for DadLink server communication.
/// Handles connection, authentication, message dispatch, and auto-reconnection.
class WebSocketClient : public QObject {
    Q_OBJECT

public:
    enum class State {
        Disconnected,
        Connecting,
        WaitingForChallenge,
        Authenticating,
        Connected,
        Reconnecting,
    };

    explicit WebSocketClient(QObject* parent = nullptr);
    ~WebSocketClient() override;

    /// Connect to the server.
    void connectToServer(const QString& host, int port, bool useTls);

    /// Disconnect from the server.
    void disconnect();

    /// Authenticate with username/password.
    void authenticatePassword(const QString& username, const QString& password, const QString& orgTag);

    /// Authenticate with a saved token.
    void authenticateToken(const QString& token);

    /// Send a typed JSON message to the server.
    void sendMessage(const QString& type, const QJsonObject& payload = {});

    /// Current connection state.
    State state() const { return m_state; }

    /// Whether we are fully authenticated.
    bool isAuthenticated() const { return m_state == State::Connected; }

    /// Enable or disable auto-reconnection.
    void setAutoReconnect(bool enabled) { m_autoReconnect = enabled; }

    /// Last measured ping RTT in milliseconds (-1 if no measurement yet).
    int latencyMs() const { return m_latencyMs; }

    /// Whether the connection is using TLS.
    bool isTls() const { return m_useTls; }

    /// Set a pinned certificate fingerprint (hex SHA-256). When set, self-signed certs
    /// matching this fingerprint are accepted without verification.
    void setPinnedCertFingerprint(const QString& fingerprint) { m_pinnedFingerprint = fingerprint; }

signals:
    // Connection lifecycle
    void connected();
    void disconnected();
    void stateChanged(State newState);

    // Authentication
    void challengeReceived(const QStringList& methods);
    void authSuccess(int userId, const QString& token, int voicePort,
                     const QJsonArray& channels, const QJsonArray& users);
    void authFailed(const QString& reason);

    // Key exchange
    void keyExchangeInit(const QString& publicKeyBase64);
    void keyExchangeComplete();

    // Channel events
    void channelJoined(int channelId, const QJsonArray& roster);
    void userJoined(int channelId, const QJsonObject& user);
    void userLeft(int channelId, int userId);
    void userStateChanged(int channelId, int userId, bool muted, bool deafened, bool talking);
    void channelListReceived(const QJsonArray& channels);
    void channelUpdated(const QJsonObject& channel);
    void channelDeleted(int channelId);
    void userCountChanged(int channelId, int userCount);
    /// Sent just before channelJoined: false = listen-only in that channel.
    void channelSpeakAccess(int channelId, bool canSpeak);

    // Ops (personalised to this user's roles)
    void opStarted(const QJsonObject& op);
    void opEnded(int opId, const QString& name);

    // General
    void pongReceived(int latencyMs);
    void errorReceived(const QString& code, const QString& message);
    void sslErrorOccurred(const QList<QSslError>& errors);

    // Update notification
    void updateAvailable(const QString& latestVersion, const QString& currentVersion, const QString& downloadUrl);

    // Connection issues
    void reconnecting(int attempt);
    void reconnectFailed();

public:
    /// Ops currently running (op_id -> op), cached so a window created after login
    /// still sees ops announced right after auth_success.
    QMap<int, QJsonObject> activeOps() const { return m_activeOps; }

private slots:
    void onConnected();
    void onDisconnected();
    void onTextMessageReceived(const QString& message);
    void onSslErrors(const QList<QSslError>& errors);
    void onPingTimer();
    void onReconnectTimer();

private:
    void setState(State newState);
    void dispatchMessage(const QString& type, const QJsonObject& payload);
    void startPingTimer();
    void stopPingTimer();
    void scheduleReconnect();
    void resetReconnect();

    QWebSocket m_socket;
    QMap<int, QJsonObject> m_activeOps;
    State m_state = State::Disconnected;

    // Connection info (for reconnection)
    QString m_host;
    int m_port = 9000;
    bool m_useTls = true;
    QString m_pinnedFingerprint;

    // Pending auth credentials (for reconnection)
    QString m_pendingMethod;
    QString m_pendingUsername;
    QString m_pendingPassword;
    QString m_pendingOrgTag;
    QString m_pendingToken;

    // Ping/keepalive
    QTimer m_pingTimer;
    QElapsedTimer m_pingStopwatch;
    int m_latencyMs = -1;
    static constexpr int PingIntervalMs = 15000;

    // Auto-reconnection
    bool m_autoReconnect = false;
    bool m_intentionalDisconnect = false;
    QTimer m_reconnectTimer;
    int m_reconnectAttempt = 0;
    static constexpr int MaxReconnectAttempts = 20;
    static constexpr int BaseReconnectMs = 1000;
    static constexpr int MaxReconnectMs = 30000;
};
