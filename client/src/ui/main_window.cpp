#include "main_window.h"
#include "channel_tree.h"
#include "user_list.h"
#include "activity_log.h"
#include "vu_meter.h"
#include "settings_dialog.h"
#include "network/websocket_client.h"
#include "ui/admin/admin_panel.h"
#include "ui/ops/ops_manager.h"

#include <QSplitter>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QMenuBar>
#include <QFrame>
#include <QStatusBar>
#include <QCloseEvent>
#include <QGroupBox>
#include <QFormLayout>
#include <QMessageBox>
#include <QDebug>

MainWindow::MainWindow(AppConfig& config, WebSocketClient* wsClient,
                       int userId, const QString& token, int voicePort,
                       const QJsonArray& channels, const QJsonArray& users,
                       bool isAdmin,
                       QWidget* parent)
    : QMainWindow(parent)
    , m_config(config)
    , m_wsClient(wsClient)
    , m_userId(userId)
    , m_token(token)
    , m_voicePort(voicePort)
    , m_isAdmin(isAdmin)
{
    setupUi();
    setupMenuBar();
    setupStatusBar();
    connectSignals();

    // Populate initial data
    populateChannels(channels);
    populateUsers(users);

    // Ops announced right after login (before this window existed) are cached by the client
    const auto ops = m_wsClient->activeOps();
    for (auto it = ops.constBegin(); it != ops.constEnd(); ++it) onOpStarted(it.value());

    // Probe admin API to check if user has admin access
    m_adminApi.configure(config.server.address, config.server.port, config.server.useTls, token);
    m_adminApi.listUsers(
        [this](const QJsonObject&) {
            // Admin API responded — user has admin access
            m_isAdmin = true;
            m_adminPanel = new AdminPanel(&m_adminApi, this);
            m_adminPanel->setWindowTitle("Admin Panel");
            m_adminPanel->setWindowFlags(Qt::Window);
            m_adminPanel->resize(700, 500);

            // Add Admin menu dynamically
            auto* adminMenu = menuBar()->addMenu("&Admin");
            adminMenu->addAction("&Admin Panel", this, [this]() {
                if (m_adminPanel) {
                    m_adminPanel->refresh();
                    m_adminPanel->show();
                    m_adminPanel->raise();
                }
            });

            m_activityLog->addEntry("Admin access detected.", QColor(100, 200, 100));
        },
        [](int, const QString&) {
            // Not admin — silently ignore
        }
    );

    // Probe the ops API: Admins and Op Leaders get an Ops menu
    m_adminApi.listOpTemplates(
        [this](const QJsonObject&) {
            m_opsManager = new OpsManager(&m_adminApi, this);
            auto* opsMenu = menuBar()->addMenu("&Ops");
            opsMenu->addAction("&Op Manager…", this, [this]() {
                m_opsManager->refresh();
                m_opsManager->show();
                m_opsManager->raise();
            });
        },
        [](int, const QString&) { /* not an op leader */ });

    // Initialize voice session
    m_voiceSession.initialize(
        config.audio.inputDeviceId,
        config.audio.outputDeviceId,
        config.opus.bitrate,
        config.opus.complexity,
        config.opus.enableFec,
        config.opus.enableDtx
    );
    m_voiceSession.setServer(config.server.address, voicePort);
    m_voiceSession.setIdentity(static_cast<uint32_t>(userId), 0);
    m_voiceSession.setInputVolume(config.audio.inputVolume);
    m_voiceSession.setOutputVolume(config.audio.outputVolume);

    // Load saved per-channel PTT bindings (keyboard, mouse buttons, joystick/HOTAS)
    for (auto it = config.hotkeys.constBegin(); it != config.hotkeys.constEnd(); ++it) {
        if (it.value() > 0) {
            m_pttManager.setBinding(it.key(), it.value());
        }
    }

    // Global keyboard + mouse hooks, and DirectInput joysticks (needs our window handle)
    m_pttManager.install(reinterpret_cast<void*>(winId()));

    // Load saved per-channel audio settings into voice session
    m_voiceSession.setDuckingEnabled(config.voice.duckingEnabled);
    m_voiceSession.setDuckLevel(config.voice.duckLevel);
    for (auto it = config.channelAudio.constBegin(); it != config.channelAudio.constEnd(); ++it) {
        auto chId = static_cast<uint32_t>(it.key());
        m_voiceSession.setChannelVolume(chId, it.value().volume);
        m_voiceSession.setChannelPriority(chId, it.value().priority);
        m_voiceSession.setChannelMuted(chId, it.value().muted);
        m_voiceSession.setChannelPan(chId, it.value().pan);
        m_voiceSession.setChannelRadioFilter(chId, it.value().radioFilter);
    }

    // Restore window geometry
    resize(config.ui.windowWidth, config.ui.windowHeight);
    move(config.ui.windowX, config.ui.windowY);

    setWindowTitle(QString("DadLink v%1").arg(QStringLiteral(DADLINK_VERSION)));
    m_activityLog->addEntry("Connected to server.", QColor(0, 200, 0));
}

void MainWindow::setupUi() {
    auto* centralWidget = new QWidget(this);
    auto* mainLayout = new QHBoxLayout(centralWidget);
    mainLayout->setContentsMargins(4, 4, 4, 4);

    // Left panel: channel tree + voice controls
    auto* leftPanel = new QWidget(this);
    auto* leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(0, 0, 0, 0);

    // Op banner: appears when an op starts, offers one-click join to your assigned nets
    m_opBanner = new QFrame(this);
    m_opBanner->setStyleSheet("QFrame { background: #1d3557; border: 1px solid #457b9d; border-radius: 4px; }"
                              "QLabel { color: #f1faee; border: none; }");
    auto* bannerLayout = new QVBoxLayout(m_opBanner);
    bannerLayout->setContentsMargins(8, 6, 8, 6);
    m_opBannerText = new QLabel(m_opBanner);
    m_opBannerText->setWordWrap(true);
    bannerLayout->addWidget(m_opBannerText);
    auto* bannerButtons = new QHBoxLayout();
    m_opJoinBtn = new QPushButton("Join op", m_opBanner);
    m_opJoinBtn->setStyleSheet("font-weight: bold;");
    auto* dismissBtn = new QPushButton("Dismiss", m_opBanner);
    bannerButtons->addWidget(m_opJoinBtn);
    bannerButtons->addWidget(dismissBtn);
    bannerLayout->addLayout(bannerButtons);
    m_opBanner->hide();
    leftLayout->addWidget(m_opBanner);
    connect(m_opJoinBtn, &QPushButton::clicked, this, [this]() { joinOp(m_bannerOpId); });
    connect(dismissBtn, &QPushButton::clicked, this, [this]() { m_opBanner->hide(); });

    m_channelTree = new ChannelTree(this);
    leftLayout->addWidget(m_channelTree, 1);

    // Voice controls
    auto* voiceGroup = new QGroupBox("Voice Controls", this);
    auto* voiceLayout = new QVBoxLayout(voiceGroup);

    // PTT indicator
    m_pttIndicator = new QLabel("PTT: OFF", this);
    m_pttIndicator->setAlignment(Qt::AlignCenter);
    m_pttIndicator->setMinimumHeight(28);
    m_pttIndicator->setStyleSheet("background: #333; color: #888; border-radius: 4px; padding: 4px;");
    voiceLayout->addWidget(m_pttIndicator);

    auto* btnLayout = new QHBoxLayout();
    m_muteBtn = new QPushButton("Mute", this);
    m_muteBtn->setCheckable(true);
    m_muteBtn->setMinimumHeight(32);
    btnLayout->addWidget(m_muteBtn);

    m_deafenBtn = new QPushButton("Deafen", this);
    m_deafenBtn->setCheckable(true);
    m_deafenBtn->setMinimumHeight(32);
    btnLayout->addWidget(m_deafenBtn);

    voiceLayout->addLayout(btnLayout);

    // VU meters
    auto* meterLayout = new QFormLayout();
    m_inputMeter = new VuMeter(this);
    m_outputMeter = new VuMeter(this);
    meterLayout->addRow("In:", m_inputMeter);
    meterLayout->addRow("Out:", m_outputMeter);
    voiceLayout->addLayout(meterLayout);

    leftLayout->addWidget(voiceGroup);

    // Right panel: user list + activity log
    auto* rightSplitter = new QSplitter(Qt::Vertical, this);

    // User list with header
    auto* userPanel = new QWidget(this);
    auto* userLayout = new QVBoxLayout(userPanel);
    userLayout->setContentsMargins(0, 0, 0, 0);
    auto* userHeader = new QLabel("Users", this);
    userHeader->setStyleSheet("font-weight: bold; padding: 4px;");
    userLayout->addWidget(userHeader);
    m_userList = new UserList(this);
    userLayout->addWidget(m_userList);
    rightSplitter->addWidget(userPanel);

    // Activity log with header
    auto* logPanel = new QWidget(this);
    auto* logLayout = new QVBoxLayout(logPanel);
    logLayout->setContentsMargins(0, 0, 0, 0);
    auto* logHeader = new QLabel("Activity", this);
    logHeader->setStyleSheet("font-weight: bold; padding: 4px;");
    logLayout->addWidget(logHeader);
    m_activityLog = new ActivityLog(this);
    logLayout->addWidget(m_activityLog);
    rightSplitter->addWidget(logPanel);

    rightSplitter->setStretchFactor(0, 2);
    rightSplitter->setStretchFactor(1, 1);

    // Main splitter: left | right
    auto* mainSplitter = new QSplitter(Qt::Horizontal, this);
    mainSplitter->addWidget(leftPanel);
    mainSplitter->addWidget(rightSplitter);
    mainSplitter->setStretchFactor(0, 1);
    mainSplitter->setStretchFactor(1, 2);

    mainLayout->addWidget(mainSplitter);
    setCentralWidget(centralWidget);
}

void MainWindow::setupMenuBar() {
    auto* fileMenu = menuBar()->addMenu("&File");
    fileMenu->addAction("&Disconnect", this, &MainWindow::onDisconnectAction);
    fileMenu->addSeparator();
    fileMenu->addAction("E&xit", this, &QMainWindow::close);

    auto* settingsMenu = menuBar()->addMenu("&Settings");
    settingsMenu->addAction("&Preferences...", this, [this]() {
        SettingsDialog dialog(m_config, &m_voiceSession.audioEngine(),
                              &m_pttManager, m_channelTree, &m_voiceSession, this);
        dialog.setOpNetNames(m_opNetNames);
        connect(&dialog, &SettingsDialog::audioSettingsChanged, this, [this]() {
            m_voiceSession.setInputVolume(m_config.audio.inputVolume);
            m_voiceSession.setOutputVolume(m_config.audio.outputVolume);
            m_voiceSession.setDevices(m_config.audio.inputDeviceId, m_config.audio.outputDeviceId);
        });
        dialog.exec();
    });

    menuBar()->addMenu("&Help");
}

void MainWindow::setupStatusBar() {
    m_connectionStatus = new QLabel("Connected", this);
    m_connectionStatus->setStyleSheet("color: green;");

    m_tlsIndicator = new QLabel(this);
    if (m_config.server.useTls) {
        m_tlsIndicator->setText("TLS");
        m_tlsIndicator->setStyleSheet("color: #00cc00; font-weight: bold; padding: 0 4px;");
        m_tlsIndicator->setToolTip("Connection is encrypted with TLS");
    } else {
        m_tlsIndicator->setText("NO TLS");
        m_tlsIndicator->setStyleSheet("color: #cc0000; font-weight: bold; padding: 0 4px;");
        m_tlsIndicator->setToolTip("Connection is NOT encrypted");
    }

    m_latencyLabel = new QLabel("Ping: --", this);
    m_latencyLabel->setStyleSheet("padding: 0 4px;");

    m_qualityIndicator = new QLabel("--", this);
    m_qualityIndicator->setStyleSheet("padding: 0 4px;");
    m_qualityIndicator->setToolTip("Connection quality based on ping latency");

    statusBar()->addWidget(m_connectionStatus);
    statusBar()->addPermanentWidget(m_tlsIndicator);
    statusBar()->addPermanentWidget(m_latencyLabel);
    statusBar()->addPermanentWidget(m_qualityIndicator);
}

void MainWindow::connectSignals() {
    // WebSocket events
    connect(m_wsClient, &WebSocketClient::channelSpeakAccess, this, [this](int channelId, bool canSpeak) {
        if (canSpeak) m_listenOnly.remove(channelId);
        else          m_listenOnly.insert(channelId);
    });
    connect(m_wsClient, &WebSocketClient::opStarted, this, &MainWindow::onOpStarted);
    connect(m_wsClient, &WebSocketClient::opEnded, this, &MainWindow::onOpEnded);
    connect(m_wsClient, &WebSocketClient::channelJoined, this, &MainWindow::onChannelJoined);
    connect(m_wsClient, &WebSocketClient::userJoined, this, &MainWindow::onUserJoined);
    connect(m_wsClient, &WebSocketClient::userLeft, this, &MainWindow::onUserLeft);
    connect(m_wsClient, &WebSocketClient::userStateChanged, this, &MainWindow::onUserStateChanged);
    connect(m_wsClient, &WebSocketClient::channelListReceived, this, &MainWindow::onChannelListReceived);
    connect(m_wsClient, &WebSocketClient::channelUpdated, this, &MainWindow::onChannelUpdated);
    connect(m_wsClient, &WebSocketClient::channelDeleted, this, &MainWindow::onChannelDeleted);
    connect(m_wsClient, &WebSocketClient::userCountChanged, this, [this](int channelId, int count) {
        m_channelTree->setUserCount(channelId, count);
    });
    connect(m_wsClient, &WebSocketClient::pongReceived, this, [this](int latencyMs) {
        if (latencyMs >= 0) {
            m_latencyLabel->setText(QString("Ping: %1ms").arg(latencyMs));

            // Quality indicator based on RTT
            if (latencyMs < 50) {
                m_qualityIndicator->setText("Excellent");
                m_qualityIndicator->setStyleSheet("color: #00cc00; padding: 0 4px;");
            } else if (latencyMs < 100) {
                m_qualityIndicator->setText("Good");
                m_qualityIndicator->setStyleSheet("color: #88cc00; padding: 0 4px;");
            } else if (latencyMs < 150) {
                m_qualityIndicator->setText("Fair");
                m_qualityIndicator->setStyleSheet("color: #cccc00; padding: 0 4px;");
            } else {
                m_qualityIndicator->setText("Poor");
                m_qualityIndicator->setStyleSheet("color: #cc0000; padding: 0 4px;");
            }
        }
    });
    connect(m_wsClient, &WebSocketClient::keyExchangeInit, this, &MainWindow::onKeyExchangeInit);
    connect(m_wsClient, &WebSocketClient::keyExchangeComplete, this, &MainWindow::onKeyExchangeComplete);
    connect(m_wsClient, &WebSocketClient::errorReceived, this, &MainWindow::onErrorReceived);
    connect(m_wsClient, &WebSocketClient::disconnected, this, &MainWindow::onDisconnected);
    connect(m_wsClient, &WebSocketClient::reconnecting, this, &MainWindow::onReconnecting);
    connect(m_wsClient, &WebSocketClient::reconnectFailed, this, &MainWindow::onReconnectFailed);
    connect(m_wsClient, &WebSocketClient::stateChanged, this, &MainWindow::onStateChanged);
    connect(m_wsClient, &WebSocketClient::updateAvailable, this, [this](const QString& latest, const QString& current, const QString& url) {
        QString msg = QString("Update available! You have v%1, latest is v%2.").arg(current, latest);
        if (!url.isEmpty()) {
            msg += QString(" Download: %1").arg(url);
        }
        m_activityLog->addEntry(msg, QColor(255, 165, 0));
        setWindowTitle(QString("DadLink v%1 (update available: v%2)").arg(current, latest));
    });

    // Channel tree
    connect(m_channelTree, &ChannelTree::channelActivated, this, &MainWindow::onChannelDoubleClicked);
    connect(m_channelTree, &ChannelTree::channelLeaveRequested, this, &MainWindow::onChannelLeaveRequested);
    connect(m_channelTree, &ChannelTree::channelMuteToggled, this, [this](int channelId, bool muted) {
        m_voiceSession.setChannelMuted(static_cast<uint32_t>(channelId), muted);
        m_config.channelAudio[channelId].muted = muted;
        m_config.save();
        m_activityLog->addEntry(QString("Channel #%1 %2").arg(channelId).arg(muted ? "muted" : "unmuted"),
                                muted ? QColor(128, 128, 128) : QColor(0, 200, 0));
    });

    connect(m_channelTree, &ChannelTree::channelOpenMicToggled, this, [this](int channelId, bool enabled) {
        m_voiceSession.setOpenMic(static_cast<uint32_t>(channelId), enabled);
        m_config.channelAudio[channelId].openMic = enabled;
        m_config.save();

        // Update PTT indicator to reflect open-mic state
        if (enabled) {
            m_pttIndicator->setText(QString("OPEN MIC: CH #%1").arg(channelId));
            m_pttIndicator->setStyleSheet("background: #4a3000; color: #ffa500; border-radius: 4px; padding: 4px; font-weight: bold;");
        } else {
            // Check if any other channel still has open mic active
            bool anyOpenMic = false;
            for (int chId : m_joinedChannels) {
                if (m_config.channelAudio.contains(chId) && m_config.channelAudio[chId].openMic) {
                    anyOpenMic = true; break;
                }
            }
            if (!anyOpenMic) {
                m_pttIndicator->setText("PTT: OFF");
                m_pttIndicator->setStyleSheet("background: #333; color: #888; border-radius: 4px; padding: 4px;");
            }
        }

        m_activityLog->addEntry(
            QString("Channel #%1 open mic %2").arg(channelId).arg(enabled ? "enabled" : "disabled"),
            enabled ? QColor(255, 165, 0) : QColor(128, 128, 128));
    });

    // Voice controls
    connect(m_muteBtn, &QPushButton::clicked, this, &MainWindow::onMuteToggled);
    connect(m_deafenBtn, &QPushButton::clicked, this, &MainWindow::onDeafenToggled);

    // PTT
    connect(&m_pttManager, &PttManager::pttStateChanged, this, &MainWindow::onPttStateChanged);
}

void MainWindow::populateChannels(const QJsonArray& channels) {
    m_channelTree->setChannels(channels);
}

void MainWindow::populateUsers(const QJsonArray& users) {
    // Initial online user list — org-wide, not channel-specific
    // Channel-specific roster comes via channel_joined
}

// --- WebSocket event handlers ---

void MainWindow::onChannelJoined(int channelId, const QJsonArray& roster) {
    m_joinedChannels.insert(channelId);
    m_channelRosters[channelId] = roster;
    m_channelTree->addJoinedChannel(channelId);
    m_channelTree->setUserCount(channelId, roster.size());

    // Show this channel's roster in user list (switch view to newly joined)
    m_viewedChannelId = channelId;
    m_channelTree->setCurrentChannel(channelId);
    m_userList->setRoster(roster);

    // Set initial transmit target to first joined channel
    if (m_joinedChannels.size() == 1) {
        m_voiceSession.setIdentity(static_cast<uint32_t>(m_userId),
                                    static_cast<uint32_t>(channelId));
    }

    // Restore open-mic preference from config
    if (m_config.channelAudio.contains(channelId) && m_config.channelAudio[channelId].openMic
        && !m_listenOnly.contains(channelId)) {
        m_voiceSession.setOpenMic(static_cast<uint32_t>(channelId), true);
        m_pttIndicator->setText(QString("OPEN MIC: CH #%1").arg(channelId));
        m_pttIndicator->setStyleSheet("background: #4a3000; color: #ffa500; border-radius: 4px; padding: 4px; font-weight: bold;");
    }

    const bool listenOnly = m_listenOnly.contains(channelId);
    const QString label = m_opNetNames.contains(channelId)
        ? m_opNetNames.value(channelId) : QString("#%1").arg(channelId);
    m_activityLog->addEntry(QString("Joined %1%2 (%3 channels active)")
                                .arg(label, listenOnly ? " — listen only" : "")
                                .arg(m_joinedChannels.size()),
                            listenOnly ? QColor(255, 165, 0) : QColor(0, 200, 0));
}

void MainWindow::onUserJoined(int channelId, const QJsonObject& user) {
    QString name = user["display_name"].toString();
    if (name.isEmpty()) name = user["username"].toString();

    qDebug() << "[MAIN] onUserJoined: channelId=" << channelId
             << "user=" << name
             << "m_viewedChannelId=" << m_viewedChannelId
             << "m_joinedChannels=" << m_joinedChannels;

    // Update cached roster
    if (m_channelRosters.contains(channelId)) {
        m_channelRosters[channelId].append(user);
    }

    if (channelId == m_viewedChannelId) {
        qDebug() << "[MAIN] Adding user to visible user list";
        m_userList->addUser(user);
    }
    if (m_joinedChannels.contains(channelId)) {
        m_activityLog->addEntry(QString("%1 joined channel #%2").arg(name).arg(channelId));
    }
    // Update user count in channel tree
    m_channelTree->setUserCount(channelId, m_channelRosters.value(channelId).size());
}

void MainWindow::onUserLeft(int channelId, int userId) {
    if (channelId == m_viewedChannelId) {
        m_userList->removeUser(userId);
    }
    if (m_joinedChannels.contains(channelId)) {
        m_activityLog->addEntry(QString("User #%1 left channel #%2").arg(userId).arg(channelId));
    }
}

void MainWindow::onUserStateChanged(int channelId, int userId, bool muted, bool deafened, bool talking) {
    if (channelId == m_viewedChannelId) {
        m_userList->updateUserState(userId, muted, deafened, talking);
    }
}

void MainWindow::onChannelListReceived(const QJsonArray& channels) {
    m_channelTree->setChannels(channels);
}

void MainWindow::onChannelUpdated(const QJsonObject& channel) {
    m_channelTree->updateChannel(channel);
}

void MainWindow::onChannelDeleted(int channelId) {
    m_listenOnly.remove(channelId);
    if (m_opNetNames.remove(channelId) > 0) {
        // Op net gone: drop its per-channel config so stale ids don't pile up.
        // (The PTT binding lives on in opNetHotkeys, keyed by net name.)
        m_config.channelAudio.remove(channelId);
        for (auto it = m_config.hotkeys.begin(); it != m_config.hotkeys.end(); ) {
            if (it.value() == channelId) it = m_config.hotkeys.erase(it); else ++it;
        }
        m_voiceSession.removeChannelSettings(static_cast<uint32_t>(channelId));
        m_config.save();
    }
    m_channelTree->removeChannel(channelId);
    m_joinedChannels.remove(channelId);
    m_channelRosters.remove(channelId);
    m_pttManager.removeHotkey(channelId);

    if (channelId == m_viewedChannelId) {
        m_viewedChannelId = -1;
        m_userList->clearUsers();
        m_activityLog->addEntry("Your channel was deleted.", QColor(200, 0, 0));
    }
}

void MainWindow::onKeyExchangeInit(const QString& publicKeyBase64) {
    bool isRotation = m_srtpSession.isReady();
    qDebug() << "[MW] onKeyExchangeInit called, rotation=" << isRotation;

    if (isRotation) {
        m_activityLog->addEntry("Key rotation initiated by server.");
    } else {
        m_activityLog->addEntry("Key exchange initiated by server.");
    }

    if (!m_keyExchange.generateKeypair()) {
        m_activityLog->addEntry("Failed to generate keypair!", QColor(200, 0, 0));
        return;
    }

    QJsonObject payload;
    payload["public_key"] = QString::fromStdString(m_keyExchange.publicKeyBase64());
    m_wsClient->sendMessage("key_exchange_response", payload);

    if (!m_keyExchange.computeSharedSecret(publicKeyBase64.toStdString())) {
        m_activityLog->addEntry("Failed to compute shared secret!", QColor(200, 0, 0));
        return;
    }

    // init() handles dual-key: if already ready, saves previous keys for 2s window
    if (!m_srtpSession.init(m_keyExchange.derivedKeys())) {
        m_activityLog->addEntry("Failed to initialize SRTP session!", QColor(200, 0, 0));
        return;
    }

    if (isRotation) {
        m_activityLog->addEntry("SRTP keys rotated — dual-key window active.");
    } else {
        m_activityLog->addEntry("SRTP keys derived — awaiting server confirmation.");
    }
}

void MainWindow::onKeyExchangeComplete() {
    bool isRotation = m_voiceSession.isRunning();
    qDebug() << "[MW] onKeyExchangeComplete called, srtp ready=" << m_srtpSession.isReady()
             << "rotation=" << isRotation;

    if (isRotation) {
        m_activityLog->addEntry("Key rotation complete — new encryption keys active.", QColor(0, 200, 0));
    } else {
        m_activityLog->addEntry("Key exchange complete — voice encryption ready.", QColor(0, 200, 0));

        // Wire up SRTP session and start voice
        m_voiceSession.setSrtpSession(&m_srtpSession);
        qDebug() << "[MW] Starting voice session...";
        if (m_voiceSession.start()) {
            auto& ae = m_voiceSession.audioEngine();
            m_activityLog->addEntry(
                QString("Voice session started. In: %1 | Out: %2")
                    .arg(QString::fromStdString(ae.captureDeviceName()))
                    .arg(QString::fromStdString(ae.playbackDeviceName())),
                QColor(0, 200, 0));

            // Start VU meters
            m_inputMeter->setLevelSource([this]() { return m_voiceSession.inputLevel(); });
            m_outputMeter->setLevelSource([this]() { return m_voiceSession.outputLevel(); });
            m_inputMeter->start();
            m_outputMeter->start();
        } else {
            m_activityLog->addEntry("Failed to start voice session.", QColor(200, 0, 0));
        }
    }
}

void MainWindow::onErrorReceived(const QString& code, const QString& message) {
    // Map server error codes to human-readable messages
    QString friendly;
    bool showPopup = false;

    if (code == "not_authenticated") {
        friendly = "Session expired. Please reconnect.";
        showPopup = true;
    } else if (code == "session_revoked") {
        friendly = message.isEmpty() ? "Your session was revoked by an administrator." : message;
        showPopup = true;
    } else if (code == "not_found") {
        friendly = "The channel no longer exists.";
    } else if (code == "channel_full") {
        friendly = "Cannot join — channel is full.";
    } else if (code == "wrong_password") {
        friendly = "Incorrect channel password.";
    } else if (code == "rate_limited") {
        friendly = "Slow down — too many requests.";
    } else if (code == "permission_denied") {
        friendly = "You don't have permission to do that.";
    } else if (code == "timeout") {
        friendly = "Connection timed out.";
    } else if (code == "key_exchange_error") {
        friendly = "Voice encryption setup failed. Try reconnecting.";
        showPopup = true;
    } else if (code == "internal_error") {
        friendly = "Server error. Please try again.";
    } else {
        // Fallback: use server message if available, otherwise show code
        friendly = message.isEmpty() ? QString("Unexpected error: %1").arg(code) : message;
    }

    m_activityLog->addEntry(friendly, QColor(200, 0, 0));

    if (showPopup) {
        QMessageBox::warning(this, "Error", friendly);
    }
}

void MainWindow::onDisconnected() {
    m_connectionStatus->setText("Disconnected");
    m_connectionStatus->setStyleSheet("color: red;");
    m_latencyLabel->setText("Ping: --");
    m_qualityIndicator->setText("--");
    m_qualityIndicator->setStyleSheet("padding: 0 4px;");
    m_voiceSession.stop();
    m_inputMeter->stop();
    m_outputMeter->stop();
    m_activityLog->addEntry("Disconnected from server.", QColor(200, 0, 0));
}

void MainWindow::onReconnecting(int attempt) {
    m_connectionStatus->setText(QString("Reconnecting... (%1)").arg(attempt));
    m_connectionStatus->setStyleSheet("color: orange;");
    m_activityLog->addEntry(QString("Reconnecting (attempt %1)...").arg(attempt), QColor(200, 200, 0));
}

void MainWindow::onReconnectFailed() {
    m_connectionStatus->setText("Connection lost");
    m_connectionStatus->setStyleSheet("color: red;");
    m_activityLog->addEntry("Reconnection failed after max attempts.", QColor(200, 0, 0));
    QMessageBox::warning(this, "Connection Lost",
                         "Unable to reconnect to the server.\nPlease restart the application.");
}

void MainWindow::onStateChanged() {
    updateStatusBar();
}

// --- UI actions ---

void MainWindow::onChannelDoubleClicked(int channelId) {
    if (m_joinedChannels.contains(channelId)) {
        // Already joined — switch the viewed roster
        m_viewedChannelId = channelId;
        m_channelTree->setCurrentChannel(channelId);
        if (m_channelRosters.contains(channelId)) {
            m_userList->setRoster(m_channelRosters[channelId]);
        }
        return;
    }

    // Join the channel (additive — don't leave other channels)
    QJsonObject joinPayload;
    joinPayload["channel_id"] = channelId;
    m_wsClient->sendMessage("join_channel", joinPayload);
}

void MainWindow::onChannelLeaveRequested(int channelId) {
    if (!m_joinedChannels.contains(channelId)) return;

    QJsonObject leavePayload;
    leavePayload["channel_id"] = channelId;
    m_wsClient->sendMessage("leave_channel", leavePayload);

    m_joinedChannels.remove(channelId);
    m_channelRosters.remove(channelId);
    m_listenOnly.remove(channelId);
    m_channelTree->removeJoinedChannel(channelId);
    // PTT binding is kept so it works again on rejoin; onPttStateChanged ignores unjoined channels.
    m_voiceSession.setOpenMic(static_cast<uint32_t>(channelId), false);

    if (channelId == m_viewedChannelId) {
        m_viewedChannelId = -1;
        m_userList->clearUsers();
        // Switch view to another joined channel if available
        if (!m_joinedChannels.isEmpty()) {
            int nextId = *m_joinedChannels.constBegin();
            m_viewedChannelId = nextId;
            m_channelTree->setCurrentChannel(nextId);
            if (m_channelRosters.contains(nextId)) {
                m_userList->setRoster(m_channelRosters[nextId]);
            }
        }
    }

    m_activityLog->addEntry(QString("Left channel #%1").arg(channelId));
}

void MainWindow::onMuteToggled() {
    m_muted = m_muteBtn->isChecked();
    QJsonObject payload;
    payload["muted"] = m_muted;
    m_wsClient->sendMessage("set_muted", payload);
    m_muteBtn->setText(m_muted ? "Unmute" : "Mute");
}

void MainWindow::onDeafenToggled() {
    m_deafened = m_deafenBtn->isChecked();
    QJsonObject payload;
    payload["deafened"] = m_deafened;
    m_wsClient->sendMessage("set_deafened", payload);
    m_deafenBtn->setText(m_deafened ? "Undeafen" : "Deafen");

    if (m_deafened && !m_muted) {
        m_muted = true;
        m_muteBtn->setChecked(true);
        m_muteBtn->setText("Unmute");
        QJsonObject mutePayload;
        mutePayload["muted"] = true;
        m_wsClient->sendMessage("set_muted", mutePayload);
    }
}

void MainWindow::onPttStateChanged(bool active, int channelId) {
    if (m_muted) return; // don't transmit when muted

    if (active) {
        // Resolve target channel: -1 means default key → use viewed channel
        int targetChannel = channelId;
        if (targetChannel < 0) {
            targetChannel = m_viewedChannelId;
        }

        // Only transmit if we're actually joined to this channel
        if (targetChannel < 0 || !m_joinedChannels.contains(targetChannel)) {
            return;
        }

        // Listen-only net: don't open the mic, and say why
        if (m_listenOnly.contains(targetChannel)) {
            QString net = m_opNetNames.value(targetChannel, QString("#%1").arg(targetChannel));
            m_pttIndicator->setText(QString("LISTEN ONLY: %1").arg(net));
            m_pttIndicator->setStyleSheet("background: #4a3000; color: #ffa500; border-radius: 4px; padding: 4px; font-weight: bold;");
            return;
        }

        // Switch voice session to target channel (no jitter buffer clear)
        m_voiceSession.setTransmitChannel(static_cast<uint32_t>(targetChannel));
        m_voiceSession.setPttActive(true);

        m_pttIndicator->setText(QString("PTT: CH #%1").arg(targetChannel));
        m_pttIndicator->setStyleSheet("background: #004400; color: #00ff00; border-radius: 4px; padding: 4px; font-weight: bold;");
    } else {
        m_voiceSession.setPttActive(false);
        m_pttIndicator->setText("PTT: OFF");
        m_pttIndicator->setStyleSheet("background: #333; color: #888; border-radius: 4px; padding: 4px;");
    }
}

// --- Ops ---

void MainWindow::onOpStarted(const QJsonObject& op) {
    const int opId = op["id"].toInt();
    const bool isNew = !m_ops.contains(opId);
    m_ops[opId] = op;
    for (const auto& v : op["nets"].toArray()) {
        QJsonObject n = v.toObject();
        m_opNetNames[n["channel_id"].toInt()] = n["name"].toString();
    }
    if (isNew) {
        m_activityLog->addEntry(QString("Op \"%1\" is live.").arg(op["name"].toString()), QColor(100, 170, 255));
    }
    showOpBanner(op);
}

void MainWindow::showOpBanner(const QJsonObject& op) {
    QStringList mine;
    for (const auto& v : op["nets"].toArray()) {
        QJsonObject n = v.toObject();
        if (!n["assigned"].toBool()) continue;
        mine << QString("%1 (%2)").arg(n["name"].toString(),
                                       n["access"].toString() == "talk" ? "talk" : "listen");
    }
    m_bannerOpId = op["id"].toInt();
    if (mine.isEmpty()) {
        m_opBannerText->setText(QString("<b>Op %1 is live.</b><br>Your roles aren't assigned to any of its nets. "
                                        "You can still join its open channels from the list.")
                                    .arg(op["name"].toString().toHtmlEscaped()));
        m_opJoinBtn->hide();
    } else {
        m_opBannerText->setText(QString("<b>Op %1 is live.</b><br>Your nets: %2")
                                    .arg(op["name"].toString().toHtmlEscaped(), mine.join(", ").toHtmlEscaped()));
        m_opJoinBtn->show();
    }
    m_opBanner->show();
}

void MainWindow::joinOp(int opId) {
    if (!m_ops.contains(opId)) return;
    const QJsonObject op = m_ops[opId];
    int joined = 0;

    for (const auto& v : op["nets"].toArray()) {
        QJsonObject n = v.toObject();
        if (!n["assigned"].toBool()) continue;
        const int channelId = n["channel_id"].toInt();
        const auto chId = static_cast<uint32_t>(channelId);
        const QString netName = n["name"].toString();

        // Audio layout from the op leader's template (the member can still change it in Settings)
        ChannelAudioConfig& cfg = m_config.channelAudio[channelId];
        cfg.priority = n["priority"].toInt(5);
        cfg.pan = static_cast<float>(n["pan"].toDouble());
        cfg.radioFilter = n["radio"].toBool();
        m_voiceSession.setChannelVolume(chId, cfg.volume);
        m_voiceSession.setChannelPriority(chId, cfg.priority);
        m_voiceSession.setChannelPan(chId, cfg.pan);
        m_voiceSession.setChannelRadioFilter(chId, cfg.radioFilter);
        m_voiceSession.setChannelMuted(chId, false);
        cfg.muted = false;

        // PTT binding remembered for this net name from earlier ops
        const QString binding = m_config.opNetHotkeys.value(netName.toLower());
        if (!binding.isEmpty()) {
            m_pttManager.setBinding(binding, channelId);
            for (auto it = m_config.hotkeys.begin(); it != m_config.hotkeys.end(); ) {
                if (it.value() == channelId || it.key() == binding) it = m_config.hotkeys.erase(it); else ++it;
            }
            m_config.hotkeys[binding] = channelId;
        }

        if (!m_joinedChannels.contains(channelId)) {
            QJsonObject joinPayload;
            joinPayload["channel_id"] = channelId;
            m_wsClient->sendMessage("join_channel", joinPayload);
        }
        ++joined;
    }
    m_config.save();
    m_opBanner->hide();
    m_activityLog->addEntry(QString("Joining op \"%1\": %2 net(s).").arg(op["name"].toString()).arg(joined),
                            QColor(100, 170, 255));
}

void MainWindow::onOpEnded(int opId, const QString& name) {
    m_ops.remove(opId);
    if (m_bannerOpId == opId) {
        m_opBanner->hide();
        m_bannerOpId = -1;
    }
    m_activityLog->addEntry(QString("Op \"%1\" ended.").arg(name), QColor(160, 160, 160));
    // Its channels are removed via channel_deleted (see onChannelDeleted)
}

void MainWindow::onDisconnectAction() {
    m_voiceSession.stop();
    m_pttManager.uninstall();
    m_wsClient->disconnect();
    close();
}

void MainWindow::updateStatusBar() {
    switch (m_wsClient->state()) {
    case WebSocketClient::State::Connected:
        m_connectionStatus->setText("Connected");
        m_connectionStatus->setStyleSheet("color: green;");
        break;
    case WebSocketClient::State::Reconnecting:
        m_connectionStatus->setText("Reconnecting...");
        m_connectionStatus->setStyleSheet("color: orange;");
        break;
    case WebSocketClient::State::Disconnected:
        m_connectionStatus->setText("Disconnected");
        m_connectionStatus->setStyleSheet("color: red;");
        break;
    default:
        m_connectionStatus->setText("Connecting...");
        m_connectionStatus->setStyleSheet("color: gray;");
        break;
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    m_config.ui.windowX = x();
    m_config.ui.windowY = y();
    m_config.ui.windowWidth = width();
    m_config.ui.windowHeight = height();
    m_config.save();

    m_voiceSession.stop();
    m_pttManager.uninstall();
    m_wsClient->disconnect();
    event->accept();
}
