#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QProgressBar>
#include <QSslError>
#include <QJsonArray>

#include "config/config.h"
#include "ui/register_dialog.h"

class WebSocketClient;

/// Login dialog for DadLink server authentication.
/// Shows server address, port, org tag, username, password,
/// TLS checkbox, Remember Me, progress indicator, and input validation.
class LoginDialog : public QDialog {
    Q_OBJECT

public:
    explicit LoginDialog(AppConfig& config, WebSocketClient* wsClient, QWidget* parent = nullptr);

    /// The authenticated user ID (valid after successful login).
    int userId() const { return m_userId; }

    /// The JWT token received on successful auth.
    QString token() const { return m_token; }

    /// Voice port from server.
    int voicePort() const { return m_voicePort; }

    /// Channel list from auth_success.
    QJsonArray channels() const { return m_channels; }

    /// Online users from auth_success.
    QJsonArray users() const { return m_users; }

private slots:
    void onConnectClicked();
    void onDiscordClicked();
    void pollDiscord();
    void onRegisterClicked();
    void onConnected();
    void onDisconnected();
    void onChallengeReceived(const QStringList& methods);
    void onAuthSuccess(int userId, const QString& token, int voicePort,
                       const QJsonArray& channels, const QJsonArray& users);
    void onAuthFailed(const QString& reason);
    void onSslErrors(const QList<QSslError>& errors);
    void validateInputs();

private:
    void setupUi();
    void setFormEnabled(bool enabled);
    void showError(const QString& message);
    void showStatus(const QString& message);
    void saveConfigFromForm();
    void loadFormFromConfig();

    AppConfig& m_config;
    WebSocketClient* m_wsClient;

    // Form fields
    QLineEdit* m_serverAddress;
    QSpinBox* m_serverPort;
    QLineEdit* m_orgTag;
    QLineEdit* m_username;
    QLineEdit* m_password;
    QCheckBox* m_useTls;
    QCheckBox* m_rememberMe;
    QPushButton* m_connectBtn;
    QPushButton* m_registerBtn;
    QPushButton* m_discordBtn = nullptr;

    // Discord browser login
    class QNetworkAccessManager* m_http = nullptr;
    class QTimer* m_discordPoll = nullptr;
    QString m_discordVerifier;   // secret kept in the app; only its SHA-256 goes to the browser
    QString m_discordTicket;     // single-use ticket to redeem on the WebSocket
    qint64 m_discordDeadline = 0;
    bool m_discordPollInFlight = false;
    void stopDiscord(const QString& error = QString());
    QString httpBase() const;
    QProgressBar* m_progress;
    QLabel* m_statusLabel;
    QLabel* m_errorLabel;

    // Auth result
    int m_userId = 0;
    QString m_token;
    int m_voicePort = 0;
    QJsonArray m_channels;
    QJsonArray m_users;
};
