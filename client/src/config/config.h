#pragma once

#include <QString>
#include <QJsonObject>
#include <QMap>

struct ServerConfig {
    QString address;
    int port = 9000;
    int voicePort = 9001;
    QString orgTag;
    bool useTls = true;
    QMap<QString, QString> tlsPinnedCerts; // hostname -> fingerprint
};

struct UserConfig {
    QString username;
    bool rememberMe = false;
    QString savedToken;
    int lastChannelId = -1;
};

struct ChannelAudioConfig {
    float volume = 1.0f;   // 0.0 – 1.0
    int priority = 5;     // 1 = highest, 10 = lowest
    bool muted = false;
    bool openMic = false;  // always transmit in this channel (no PTT required)
    float pan = 0.0f;      // -1 left, 0 centre, +1 right
    bool radioFilter = false; // comms-radio voice effect
};

struct VoiceSettings {
    bool hotMicEnabled = false;
    int hotMicChannelId = -1;
    bool duckingEnabled = true;
    float duckLevel = 0.3f; // 30% volume when ducked
};

struct AudioConfig {
    int inputDeviceId = -1;
    int outputDeviceId = -1;
    float inputVolume = 1.0f;
    float outputVolume = 1.0f;
    float noiseGateThreshold = 0.01f;
};

struct OpusConfig {
    int bitrate = 32000;
    int complexity = 10;
    bool enableFec = true;
    bool enableDtx = true;
};

struct OverlayConfig {
    bool enabled = true;
    QString corner = "top-right"; // top-left | top-right | bottom-left | bottom-right
    int offsetX = 24;             // px from the chosen corner
    int offsetY = 140;
    int screen = 0;               // monitor index
    double scale = 1.0;           // 0.75 – 2.0
    double opacity = 0.85;        // 0.3 – 1.0
    bool showIdle = false;        // show a small "DadLink" tag when nobody is talking
};

struct UiConfig {
    QString theme = "dark";
    int windowX = 100;
    int windowY = 100;
    int windowWidth = 900;
    int windowHeight = 600;
    bool showActivityLog = true;
};

class AppConfig {
public:
    AppConfig();

    bool load(const QString& path = QString());
    bool save(const QString& path = QString()) const;

    QString configFilePath() const;

    ServerConfig server;
    UserConfig user;
    VoiceSettings voice;
    AudioConfig audio;
    OpusConfig opus;
    QMap<QString, int> hotkeys; // key name -> channel_id
    /// PTT bindings for op nets, keyed by lower-case net name ("fleet" -> "joy:{…}:b5").
    /// Op channels get new ids every op, so these follow the net name into each new op.
    QMap<QString, QString> opNetHotkeys;
    OverlayConfig overlay;
    QMap<int, ChannelAudioConfig> channelAudio; // channelId -> audio settings
    UiConfig ui;

private:
    QString m_configPath;

    QJsonObject serverToJson() const;
    QJsonObject userToJson() const;
    QJsonObject voiceToJson() const;
    QJsonObject audioToJson() const;
    QJsonObject opusToJson() const;
    QJsonObject hotkeysToJson() const;
    QJsonObject channelAudioToJson() const;
    QJsonObject uiToJson() const;

    void serverFromJson(const QJsonObject& obj);
    void userFromJson(const QJsonObject& obj);
    void voiceFromJson(const QJsonObject& obj);
    void audioFromJson(const QJsonObject& obj);
    void opusFromJson(const QJsonObject& obj);
    void hotkeysFromJson(const QJsonObject& obj);
    void channelAudioFromJson(const QJsonObject& obj);
    void uiFromJson(const QJsonObject& obj);
};
