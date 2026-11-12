#include "login_dialog.h"
#include "network/websocket_client.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QMessageBox>
#include <QSslCertificate>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QSslConfiguration>
#include <QTimer>
#include <QUrlQuery>

LoginDialog::LoginDialog(AppConfig& config, WebSocketClient* wsClient, QWidget* parent)
    : QDialog(parent)
    , m_config(config)
    , m_wsClient(wsClient)
{
    setupUi();
    loadFormFromConfig();

    // WebSocket signals
    connect(m_wsClient, &WebSocketClient::connected, this, &LoginDialog::onConnected);
    connect(m_wsClient, &WebSocketClient::disconnected, this, &LoginDialog::onDisconnected);
    connect(m_wsClient, &WebSocketClient::challengeReceived, this, &LoginDialog::onChallengeReceived);
    connect(m_wsClient, &WebSocketClient::authSuccess, this, &LoginDialog::onAuthSuccess);
    connect(m_wsClient, &WebSocketClient::authFailed, this, &LoginDialog::onAuthFailed);
    connect(m_wsClient, &WebSocketClient::sslErrorOccurred, this, &LoginDialog::onSslErrors);
}

void LoginDialog::setupUi() {
    setWindowTitle("DadLink — Connect");
    setFixedWidth(400);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    auto* mainLayout = new QVBoxLayout(this);

    // Server group
    auto* serverGroup = new QGroupBox("Server", this);
    auto* serverForm = new QFormLayout(serverGroup);

    m_serverAddress = new QLineEdit(this);
    m_serverAddress->setPlaceholderText("e.g. 192.168.1.100 or myserver.com");
    serverForm->addRow("Address:", m_serverAddress);

    m_serverPort = new QSpinBox(this);
    m_serverPort->setRange(1, 65535);
    m_serverPort->setValue(9000);
    serverForm->addRow("Port:", m_serverPort);

    m_orgTag = new QLineEdit(this);
    m_orgTag->setPlaceholderText("Organization tag");
    serverForm->addRow("Org Tag:", m_orgTag);

    m_useTls = new QCheckBox("Use TLS (encrypted connection)", this);
    m_useTls->setChecked(true);
    serverForm->addRow("", m_useTls);

    mainLayout->addWidget(serverGroup);

    // Credentials group
    auto* credGroup = new QGroupBox("Credentials", this);
    auto* credForm = new QFormLayout(credGroup);

    m_username = new QLineEdit(this);
    m_username->setPlaceholderText("Username");
    credForm->addRow("Username:", m_username);

    m_password = new QLineEdit(this);
    m_password->setPlaceholderText("Password");
    m_password->setEchoMode(QLineEdit::Password);
    credForm->addRow("Password:", m_password);

    m_rememberMe = new QCheckBox("Remember me (save token for auto-login)", this);
    credForm->addRow("", m_rememberMe);

    mainLayout->addWidget(credGroup);

    // Discord login (server must have it enabled; see server config [discord])
    m_discordBtn = new QPushButton("Log in with Discord", this);
    m_discordBtn->setToolTip("Opens your browser to sign in with Discord. No DadLink password needed.");
    m_discordBtn->setStyleSheet("QPushButton { background: #5865F2; color: white; font-weight: bold; padding: 6px; border-radius: 4px; }"
                                "QPushButton:disabled { background: #3a3f6b; color: #aaa; }");
    m_discordBtn->setEnabled(false);
    mainLayout->addWidget(m_discordBtn);

    // Status and progress
    m_statusLabel = new QLabel(this);
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->setStyleSheet("color: gray;");
    mainLayout->addWidget(m_statusLabel);

    m_errorLabel = new QLabel(this);
    m_errorLabel->setAlignment(Qt::AlignCenter);
    m_errorLabel->setStyleSheet("color: red; font-weight: bold;");
    m_errorLabel->setWordWrap(true);
    m_errorLabel->hide();
    mainLayout->addWidget(m_errorLabel);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 0); // indeterminate
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(6);
    m_progress->hide();
    mainLayout->addWidget(m_progress);

    // Buttons
    auto* btnLayout = new QHBoxLayout();

    m_registerBtn = new QPushButton("Register", this);
    m_registerBtn->setMinimumWidth(100);
    m_registerBtn->setToolTip("Create a new account using an invite code");
    btnLayout->addWidget(m_registerBtn);

    btnLayout->addStretch();

    m_connectBtn = new QPushButton("Connect", this);
    m_connectBtn->setDefault(true);
    m_connectBtn->setMinimumWidth(120);
    m_connectBtn->setEnabled(false);
    btnLayout->addWidget(m_connectBtn);

    mainLayout->addLayout(btnLayout);

    // Input validation
    connect(m_serverAddress, &QLineEdit::textChanged, this, &LoginDialog::validateInputs);
    connect(m_orgTag, &QLineEdit::textChanged, this, &LoginDialog::validateInputs);
    connect(m_username, &QLineEdit::textChanged, this, &LoginDialog::validateInputs);
    connect(m_password, &QLineEdit::textChanged, this, &LoginDialog::validateInputs);

    connect(m_connectBtn,  &QPushButton::clicked, this, &LoginDialog::onConnectClicked);
    connect(m_registerBtn, &QPushButton::clicked, this, &LoginDialog::onRegisterClicked);
    connect(m_discordBtn, &QPushButton::clicked, this, &LoginDialog::onDiscordClicked);

    m_http = new QNetworkAccessManager(this);
    m_discordPoll = new QTimer(this);
    m_discordPoll->setInterval(1500);
    connect(m_discordPoll, &QTimer::timeout, this, &LoginDialog::pollDiscord);

    // Enter key triggers connect
    connect(m_password, &QLineEdit::returnPressed, this, [this]() {
        if (m_connectBtn->isEnabled()) {
            onConnectClicked();
        }
    });
}

void LoginDialog::loadFormFromConfig() {
    m_serverAddress->setText(m_config.server.address);
    m_serverPort->setValue(m_config.server.port);
    m_orgTag->setText(m_config.server.orgTag);
    m_useTls->setChecked(m_config.server.useTls);
    m_username->setText(m_config.user.username);
    m_rememberMe->setChecked(m_config.user.rememberMe);

    validateInputs();
}

void LoginDialog::saveConfigFromForm() {
    m_config.server.address = m_serverAddress->text().trimmed();
    m_config.server.port = m_serverPort->value();
    m_config.server.orgTag = m_orgTag->text().trimmed();
    m_config.server.useTls = m_useTls->isChecked();
    m_config.user.username = m_username->text().trimmed();
    m_config.user.rememberMe = m_rememberMe->isChecked();
}

void LoginDialog::validateInputs() {
    bool valid = !m_serverAddress->text().trimmed().isEmpty()
              && !m_orgTag->text().trimmed().isEmpty()
              && !m_username->text().trimmed().isEmpty()
              && !m_password->text().isEmpty();
    m_connectBtn->setEnabled(valid);
    // Discord login needs only the server and org
    if (m_discordBtn && !(m_discordPoll && m_discordPoll->isActive())) {
        m_discordBtn->setEnabled(!m_serverAddress->text().trimmed().isEmpty()
                                 && !m_orgTag->text().trimmed().isEmpty());
    }
}

void LoginDialog::onConnectClicked() {
    m_errorLabel->hide();
    showStatus("Connecting...");
    m_progress->show();
    setFormEnabled(false);

    saveConfigFromForm();

    // Load pinned cert fingerprint if available
    QString pinned = m_config.server.tlsPinnedCerts.value(m_config.server.address);
    m_wsClient->setPinnedCertFingerprint(pinned);

    m_wsClient->connectToServer(
        m_config.server.address,
        m_config.server.port,
        m_config.server.useTls
    );
}

void LoginDialog::onConnected() {
    showStatus("Connected. Waiting for challenge...");
}

void LoginDialog::onDisconnected() {
    m_progress->hide();
    setFormEnabled(true);
    showStatus("");
    showError("Connection lost. Please try again.");
}

void LoginDialog::onChallengeReceived(const QStringList& methods) {
    if (!m_discordTicket.isEmpty()) {
        const QString ticket = m_discordTicket;
        m_discordTicket.clear(); // single use
        showStatus("Signing in with Discord...");
        m_wsClient->authenticateDiscord(ticket, m_orgTag->text().trimmed());
        return;
    }
    if (!methods.contains("password")) {
        showError("Server does not support password authentication.");
        m_progress->hide();
        setFormEnabled(true);
        return;
    }

    showStatus("Authenticating...");
    m_wsClient->authenticatePassword(
        m_username->text().trimmed(),
        m_password->text(),
        m_orgTag->text().trimmed()
    );
}

void LoginDialog::onAuthSuccess(int userId, const QString& token, int voicePort,
                                 const QJsonArray& channels, const QJsonArray& users) {
    m_userId = userId;
    m_token = token;
    m_voicePort = voicePort;
    m_channels = channels;
    m_users = users;

    // Save token if Remember Me is checked
    if (m_rememberMe->isChecked()) {
        m_config.user.savedToken = token;
    } else {
        m_config.user.savedToken.clear();
    }

    m_config.save();

    m_progress->hide();
    showStatus("Authenticated!");

    // Enable auto-reconnect now that we have credentials
    m_wsClient->setAutoReconnect(true);

    accept(); // close dialog with QDialog::Accepted
}

void LoginDialog::onAuthFailed(const QString& reason) {
    m_progress->hide();
    setFormEnabled(true);
    showStatus("");

    // Map terse server reasons to friendlier messages
    QString friendly = reason;
    if (reason.contains("Invalid username or password", Qt::CaseInsensitive)) {
        friendly = "Incorrect username or password. Please try again.";
    } else if (reason.contains("User not found", Qt::CaseInsensitive)) {
        friendly = "Account not found. Check your username and org tag.";
    } else if (reason.contains("Organization not found", Qt::CaseInsensitive)) {
        friendly = "Organization not found. Check your org tag.";
    } else if (reason.contains("deactivated", Qt::CaseInsensitive)) {
        friendly = "Your account has been deactivated. Contact your administrator.";
    } else if (reason.contains("expired", Qt::CaseInsensitive)) {
        friendly = "Your session has expired. Please log in again.";
    }

    showError(friendly);

    // Focus the password field for retry
    m_password->selectAll();
    m_password->setFocus();
}

void LoginDialog::onSslErrors(const QList<QSslError>& errors) {
    // Build a human-readable error description
    QStringList errorStrings;
    QString fingerprint;

    for (const QSslError& error : errors) {
        errorStrings << error.errorString();

        // Extract certificate fingerprint for pinning
        if (!error.certificate().isNull() && fingerprint.isEmpty()) {
            fingerprint = QString(error.certificate().digest(QCryptographicHash::Sha256).toHex());
        }
    }

    QString msg = QString(
        "TLS certificate error:\n\n%1\n\n"
        "This may indicate a self-signed certificate.\n"
        "Do you want to trust this certificate and continue?"
    ).arg(errorStrings.join("\n"));

    if (!fingerprint.isEmpty()) {
        msg += QString("\n\nCertificate fingerprint (SHA-256):\n%1").arg(fingerprint);
    }

    auto result = QMessageBox::warning(
        this,
        "TLS Certificate Warning",
        msg,
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No
    );

    if (result == QMessageBox::Yes) {
        // Pin the certificate fingerprint in config
        if (!fingerprint.isEmpty()) {
            m_config.server.tlsPinnedCerts[m_config.server.address] = fingerprint;
            m_config.save();
        }
        // Ignore the errors and continue
        m_wsClient->disconnect();
        // Reconnect — QWebSocket doesn't support ignoreSslErrors mid-handshake cleanly,
        // so we set the pinned cert and reconnect with it accepted.
        // For now, ignore and let the user retry.
        showError("Certificate pinned. Please click Connect again.");
        m_progress->hide();
        setFormEnabled(true);
    } else {
        m_wsClient->disconnect();
        m_progress->hide();
        setFormEnabled(true);
        showError("Connection cancelled — certificate not trusted.");
    }
}

void LoginDialog::onRegisterClicked() {
    // Pre-fill server info from whatever is currently in the form
    RegisterDialog dlg(
        m_serverAddress->text().trimmed(),
        m_serverPort->value(),
        m_useTls->isChecked(),
        m_orgTag->text().trimmed(),
        this
    );

    if (dlg.exec() == QDialog::Accepted) {
        // Pre-fill login with the new username
        m_username->setText(dlg.registeredUsername());
        m_password->clear();
        m_password->setFocus();
        showStatus("Account created. Enter your password and connect.");
    }
}

void LoginDialog::setFormEnabled(bool enabled) {
    m_serverAddress->setEnabled(enabled);
    m_serverPort->setEnabled(enabled);
    m_orgTag->setEnabled(enabled);
    m_username->setEnabled(enabled);
    m_password->setEnabled(enabled);
    m_useTls->setEnabled(enabled);
    m_rememberMe->setEnabled(enabled);
    m_connectBtn->setEnabled(enabled);
    m_registerBtn->setEnabled(enabled);
    if (m_discordBtn) m_discordBtn->setEnabled(enabled);
}

void LoginDialog::showError(const QString& message) {
    m_errorLabel->setText(message);
    m_errorLabel->show();
}

void LoginDialog::showStatus(const QString& message) {
    m_statusLabel->setText(message);
}

// --- Discord login ---

QString LoginDialog::httpBase() const {
    return QString("%1://%2:%3").arg(m_useTls->isChecked() ? "https" : "http",
                                     m_serverAddress->text().trimmed())
        .arg(m_serverPort->value());
}

void LoginDialog::onDiscordClicked() {
    if (m_discordPoll->isActive()) { // second click = cancel
        stopDiscord("Discord login cancelled.");
        return;
    }
    m_errorLabel->hide();
    saveConfigFromForm();

    // PKCE-style: the browser only sees sha256(verifier); polling needs the verifier itself
    QByteArray raw(32, Qt::Uninitialized);
    for (int i = 0; i < raw.size(); ++i) raw[i] = char(QRandomGenerator::system()->bounded(256));
    m_discordVerifier = QString::fromLatin1(raw.toHex());
    const QString challenge = QString::fromLatin1(
        QCryptographicHash::hash(m_discordVerifier.toLatin1(), QCryptographicHash::Sha256).toHex());

    QUrl url(httpBase() + "/auth/discord/start");
    QUrlQuery q;
    q.addQueryItem("org", m_orgTag->text().trimmed());
    q.addQueryItem("challenge", challenge);
    url.setQuery(q);

    if (!QDesktopServices::openUrl(url)) {
        stopDiscord("Couldn't open your browser.");
        return;
    }
    m_discordDeadline = QDateTime::currentMSecsSinceEpoch() + 5 * 60 * 1000;
    m_discordPoll->start();
    m_discordBtn->setText("Waiting for Discord… (click to cancel)");
    m_connectBtn->setEnabled(false);
    m_progress->show();
    showStatus("Finish signing in in your browser.");
}

void LoginDialog::pollDiscord() {
    if (m_discordPollInFlight) return;
    if (QDateTime::currentMSecsSinceEpoch() > m_discordDeadline) {
        stopDiscord("Discord login timed out. Please try again.");
        return;
    }

    QUrl url(httpBase() + "/auth/discord/poll");
    QUrlQuery q;
    q.addQueryItem("verifier", m_discordVerifier);
    url.setQuery(q);
    QNetworkRequest req(url);
    if (m_useTls->isChecked()) {
        // Same trust model as the WebSocket: self-signed allowed, checked against the pinned fingerprint
        QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
        ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
        req.setSslConfiguration(ssl);
    }

    m_discordPollInFlight = true;
    QNetworkReply* reply = m_http->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        m_discordPollInFlight = false;
        if (!m_discordPoll->isActive()) return; // cancelled meanwhile

        const QString pinned = m_config.server.tlsPinnedCerts.value(m_serverAddress->text().trimmed());
        if (m_useTls->isChecked() && !pinned.isEmpty()) {
            QString fp = QString(reply->sslConfiguration().peerCertificate().digest(QCryptographicHash::Sha256).toHex());
            if (!fp.isEmpty() && fp.compare(pinned, Qt::CaseInsensitive) != 0) {
                stopDiscord("Server certificate changed. Refusing Discord login.");
                return;
            }
        }

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError && status == 0) return; // network blip: keep polling
        if (status == 404) {
            stopDiscord("This server doesn't have Discord login enabled.");
            return;
        }
        QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
        const QString st = o["status"].toString();
        if (st == "pending") return;
        if (st == "ok") {
            m_discordTicket = o["ticket"].toString();
            m_discordPoll->stop();
            m_discordBtn->setText("Log in with Discord");
            showStatus("Signed in with Discord. Connecting...");
            setFormEnabled(false);
            QString pinnedFp = m_config.server.tlsPinnedCerts.value(m_config.server.address);
            m_wsClient->setPinnedCertFingerprint(pinnedFp);
            m_wsClient->connectToServer(m_config.server.address, m_config.server.port, m_config.server.useTls);
            return;
        }
        stopDiscord(o["reason"].toString("Discord login failed."));
    });
}

void LoginDialog::stopDiscord(const QString& error) {
    m_discordPoll->stop();
    m_discordVerifier.clear();
    m_discordTicket.clear();
    m_discordBtn->setText("Log in with Discord");
    m_progress->hide();
    showStatus("");
    validateInputs();
    if (!error.isEmpty()) showError(error);
}
