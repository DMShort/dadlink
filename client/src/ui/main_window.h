#pragma once

#include <QMainWindow>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSet>

#include "config/config.h"
#include "crypto/key_exchange.h"
#include "crypto/srtp_session.h"
#include "voice/voice_session.h"
#include "input/ptt_manager.h"
#include "network/admin_api.h"

class WebSocketClient;
class ChannelTree;
class UserList;
class ActivityLog;
class OpsManager;
class QFrame;
class AdminPanel;
class VuMeter;

/// Main application window shown after successful authentication.
/// Contains channel tree, user list, activity log, voice controls, and status bar.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(AppConfig& config, WebSocketClient* wsClient,
               int userId, const QString& token, int voicePort,
               const QJsonArray& channels, const QJsonArray& users,
               bool isAdmin = false,
               QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    // WebSocket events
    void onChannelJoined(int channelId, const QJsonArray& roster);
    void onUserJoined(int channelId, const QJsonObject& user);
    void onUserLeft(int channelId, int userId);
    void onUserStateChanged(int channelId, int userId, bool muted, bool deafened, bool talking);
    void onChannelListReceived(const QJsonArray& channels);
    void onChannelUpdated(const QJsonObject& channel);
    void onChannelDeleted(int channelId);
    void onKeyExchangeInit(const QString& publicKeyBase64);
    void onKeyExchangeComplete();
    void onErrorReceived(const QString& code, const QString& message);
    void onDisconnected();
    void onReconnecting(int attempt);
    void onReconnectFailed();
    void onStateChanged();

    // UI actions
    void onChannelDoubleClicked(int channelId);
    void onChannelLeaveRequested(int channelId);
    void onMuteToggled();
    void onDeafenToggled();
    void onDisconnectAction();
    void onPttStateChanged(bool active, int channelId);

    // Ops
    void onOpStarted(const QJsonObject& op);
    void onOpEnded(int opId, const QString& name);

private:
    void joinOp(int opId);
    void showOpBanner(const QJsonObject& op);
    void setupUi();
    void setupMenuBar();
    void setupStatusBar();
    void connectSignals();
    void populateChannels(const QJsonArray& channels);
    void populateUsers(const QJsonArray& users);
    void updateStatusBar();

    AppConfig& m_config;
    WebSocketClient* m_wsClient;
    int m_userId;
    QString m_token;
    int m_voicePort;

    // Widgets
    ChannelTree* m_channelTree;
    UserList* m_userList;
    ActivityLog* m_activityLog;

    // Voice control buttons
    QPushButton* m_muteBtn;
    QPushButton* m_deafenBtn;

    // Status bar widgets
    QLabel* m_connectionStatus;
    QLabel* m_tlsIndicator;
    QLabel* m_latencyLabel;
    QLabel* m_qualityIndicator;

    // State
    bool m_muted = false;
    bool m_deafened = false;
    int m_viewedChannelId = -1;      // channel whose roster is shown in user list
    QSet<int> m_joinedChannels;      // all channels we're currently joined to
    QMap<int, QJsonArray> m_channelRosters; // channelId -> roster for each joined channel

    // Crypto
    KeyExchange m_keyExchange;
    SrtpSession m_srtpSession;

    // Voice
    VoiceSession m_voiceSession;
    PttManager m_pttManager;
    QLabel* m_pttIndicator = nullptr;

    // VU Meters
    VuMeter* m_inputMeter = nullptr;
    VuMeter* m_outputMeter = nullptr;

    // Admin
    bool m_isAdmin = false;
    AdminApi m_adminApi;
    AdminPanel* m_adminPanel = nullptr;

    // Ops
    OpsManager* m_opsManager = nullptr;
    QMap<int, QJsonObject> m_ops;          // op_id -> op (personalised view from server)
    QMap<int, QString> m_opNetNames;       // channel_id -> net name, for live op nets
    QSet<int> m_listenOnly;                // joined channels where we may not transmit
    QFrame* m_opBanner = nullptr;
    QLabel* m_opBannerText = nullptr;
    QPushButton* m_opJoinBtn = nullptr;
    int m_bannerOpId = -1;
};
