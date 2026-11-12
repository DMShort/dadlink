#include "config.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

AppConfig::AppConfig() {}

QString AppConfig::configFilePath() const {
    if (!m_configPath.isEmpty()) {
        return m_configPath;
    }
    return QCoreApplication::applicationDirPath() + "/config.json";
}

bool AppConfig::load(const QString& path) {
    m_configPath = path.isEmpty() ? configFilePath() : path;

    QFile file(m_configPath);
    if (!file.open(QIODevice::ReadOnly)) {
        // No config file yet — use defaults
        return true;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return false;
    }

    QJsonObject root = doc.object();

    if (root.contains("server"))  serverFromJson(root["server"].toObject());
    if (root.contains("user"))    userFromJson(root["user"].toObject());
    if (root.contains("voice"))   voiceFromJson(root["voice"].toObject());
    if (root.contains("audio"))   audioFromJson(root["audio"].toObject());
    if (root.contains("opus"))    opusFromJson(root["opus"].toObject());
    if (root.contains("hotkeys"))       hotkeysFromJson(root["hotkeys"].toObject());
    if (root.contains("channel_audio")) channelAudioFromJson(root["channel_audio"].toObject());
    if (root.contains("ui"))            uiFromJson(root["ui"].toObject());
    if (root.contains("overlay")) {
        QJsonObject o = root["overlay"].toObject();
        overlay.enabled  = o["enabled"].toBool(true);
        overlay.corner   = o["corner"].toString("top-right");
        overlay.offsetX  = o["offset_x"].toInt(24);
        overlay.offsetY  = o["offset_y"].toInt(140);
        overlay.screen   = o["screen"].toInt(0);
        overlay.scale    = o["scale"].toDouble(1.0);
        overlay.opacity  = o["opacity"].toDouble(0.85);
        overlay.showIdle = o["show_idle"].toBool(false);
    }
    opNetHotkeys.clear();
    if (root.contains("op_net_hotkeys")) {
        QJsonObject o = root["op_net_hotkeys"].toObject();
        for (auto it = o.constBegin(); it != o.constEnd(); ++it) opNetHotkeys[it.key()] = it.value().toString();
    }

    return true;
}

bool AppConfig::save(const QString& path) const {
    QString savePath = path.isEmpty() ? configFilePath() : path;

    QJsonObject root;
    root["server"]  = serverToJson();
    root["user"]    = userToJson();
    root["voice"]   = voiceToJson();
    root["audio"]   = audioToJson();
    root["opus"]    = opusToJson();
    root["hotkeys"]       = hotkeysToJson();
    root["channel_audio"] = channelAudioToJson();
    root["ui"]            = uiToJson();
    QJsonObject opKeys;
    for (auto it = opNetHotkeys.constBegin(); it != opNetHotkeys.constEnd(); ++it) opKeys[it.key()] = it.value();
    root["op_net_hotkeys"] = opKeys;
    QJsonObject ov;
    ov["enabled"]   = overlay.enabled;
    ov["corner"]    = overlay.corner;
    ov["offset_x"]  = overlay.offsetX;
    ov["offset_y"]  = overlay.offsetY;
    ov["screen"]    = overlay.screen;
    ov["scale"]     = overlay.scale;
    ov["opacity"]   = overlay.opacity;
    ov["show_idle"] = overlay.showIdle;
    root["overlay"] = ov;

    QJsonDocument doc(root);

    QFile file(savePath);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }

    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();
    return true;
}

// --- Serialization ---

QJsonObject AppConfig::serverToJson() const {
    QJsonObject obj;
    obj["address"]    = server.address;
    obj["port"]       = server.port;
    obj["voice_port"] = server.voicePort;
    obj["org_tag"]    = server.orgTag;
    obj["use_tls"]    = server.useTls;
    QJsonObject certs;
    for (auto it = server.tlsPinnedCerts.constBegin(); it != server.tlsPinnedCerts.constEnd(); ++it) {
        certs[it.key()] = it.value();
    }
    obj["tls_pinned_certs"] = certs;
    return obj;
}

QJsonObject AppConfig::userToJson() const {
    QJsonObject obj;
    obj["username"]        = user.username;
    obj["remember_me"]     = user.rememberMe;
    obj["saved_token"]     = user.savedToken.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(user.savedToken);
    obj["last_channel_id"] = user.lastChannelId < 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(user.lastChannelId);
    return obj;
}

QJsonObject AppConfig::voiceToJson() const {
    QJsonObject obj;
    obj["hot_mic_enabled"]    = voice.hotMicEnabled;
    obj["hot_mic_channel_id"] = voice.hotMicChannelId < 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(voice.hotMicChannelId);
    obj["ducking_enabled"]    = voice.duckingEnabled;
    obj["duck_level"]         = static_cast<double>(voice.duckLevel);
    return obj;
}

QJsonObject AppConfig::audioToJson() const {
    QJsonObject obj;
    obj["input_device_id"]       = audio.inputDeviceId;
    obj["output_device_id"]      = audio.outputDeviceId;
    obj["input_volume"]          = static_cast<double>(audio.inputVolume);
    obj["output_volume"]         = static_cast<double>(audio.outputVolume);
    obj["noise_gate_threshold"]  = static_cast<double>(audio.noiseGateThreshold);
    return obj;
}

QJsonObject AppConfig::opusToJson() const {
    QJsonObject obj;
    obj["bitrate"]    = opus.bitrate;
    obj["complexity"] = opus.complexity;
    obj["enable_fec"] = opus.enableFec;
    obj["enable_dtx"] = opus.enableDtx;
    return obj;
}

QJsonObject AppConfig::hotkeysToJson() const {
    QJsonObject obj;
    for (auto it = hotkeys.constBegin(); it != hotkeys.constEnd(); ++it) {
        obj[it.key()] = it.value();
    }
    return obj;
}

QJsonObject AppConfig::channelAudioToJson() const {
    QJsonObject obj;
    for (auto it = channelAudio.constBegin(); it != channelAudio.constEnd(); ++it) {
        QJsonObject ch;
        ch["volume"]    = static_cast<double>(it.value().volume);
        ch["priority"]  = it.value().priority;
        ch["muted"]     = it.value().muted;
        ch["open_mic"]  = it.value().openMic;
        ch["pan"]       = static_cast<double>(it.value().pan);
        ch["radio"]     = it.value().radioFilter;
        obj[QString::number(it.key())] = ch;
    }
    return obj;
}

QJsonObject AppConfig::uiToJson() const {
    QJsonObject obj;
    obj["theme"]            = ui.theme;
    obj["window_x"]         = ui.windowX;
    obj["window_y"]         = ui.windowY;
    obj["window_width"]     = ui.windowWidth;
    obj["window_height"]    = ui.windowHeight;
    obj["show_activity_log"] = ui.showActivityLog;
    return obj;
}

// --- Deserialization ---

void AppConfig::serverFromJson(const QJsonObject& obj) {
    if (obj.contains("address"))    server.address   = obj["address"].toString();
    if (obj.contains("port"))       server.port      = obj["port"].toInt(9000);
    if (obj.contains("voice_port")) server.voicePort = obj["voice_port"].toInt(9001);
    if (obj.contains("org_tag"))    server.orgTag    = obj["org_tag"].toString();
    if (obj.contains("use_tls"))    server.useTls    = obj["use_tls"].toBool(true);
    if (obj.contains("tls_pinned_certs")) {
        QJsonObject certs = obj["tls_pinned_certs"].toObject();
        for (auto it = certs.constBegin(); it != certs.constEnd(); ++it) {
            server.tlsPinnedCerts[it.key()] = it.value().toString();
        }
    }
}

void AppConfig::userFromJson(const QJsonObject& obj) {
    if (obj.contains("username"))        user.username      = obj["username"].toString();
    if (obj.contains("remember_me"))     user.rememberMe    = obj["remember_me"].toBool(false);
    if (obj.contains("saved_token") && !obj["saved_token"].isNull())
        user.savedToken = obj["saved_token"].toString();
    if (obj.contains("last_channel_id") && !obj["last_channel_id"].isNull())
        user.lastChannelId = obj["last_channel_id"].toInt(-1);
}

void AppConfig::voiceFromJson(const QJsonObject& obj) {
    if (obj.contains("hot_mic_enabled"))    voice.hotMicEnabled   = obj["hot_mic_enabled"].toBool(false);
    if (obj.contains("hot_mic_channel_id") && !obj["hot_mic_channel_id"].isNull())
        voice.hotMicChannelId = obj["hot_mic_channel_id"].toInt(-1);
    if (obj.contains("ducking_enabled"))    voice.duckingEnabled  = obj["ducking_enabled"].toBool(true);
    if (obj.contains("duck_level"))         voice.duckLevel       = static_cast<float>(obj["duck_level"].toDouble(0.3));
}

void AppConfig::audioFromJson(const QJsonObject& obj) {
    if (obj.contains("input_device_id"))      audio.inputDeviceId      = obj["input_device_id"].toInt(-1);
    if (obj.contains("output_device_id"))     audio.outputDeviceId     = obj["output_device_id"].toInt(-1);
    if (obj.contains("input_volume"))         audio.inputVolume        = static_cast<float>(obj["input_volume"].toDouble(1.0));
    if (obj.contains("output_volume"))        audio.outputVolume       = static_cast<float>(obj["output_volume"].toDouble(1.0));
    if (obj.contains("noise_gate_threshold")) audio.noiseGateThreshold = static_cast<float>(obj["noise_gate_threshold"].toDouble(0.01));
}

void AppConfig::opusFromJson(const QJsonObject& obj) {
    if (obj.contains("bitrate"))    opus.bitrate    = obj["bitrate"].toInt(32000);
    if (obj.contains("complexity")) opus.complexity = obj["complexity"].toInt(10);
    if (obj.contains("enable_fec")) opus.enableFec  = obj["enable_fec"].toBool(true);
    if (obj.contains("enable_dtx")) opus.enableDtx  = obj["enable_dtx"].toBool(true);
}

void AppConfig::hotkeysFromJson(const QJsonObject& obj) {
    hotkeys.clear();
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
        hotkeys[it.key()] = it.value().toInt();
    }
}

void AppConfig::channelAudioFromJson(const QJsonObject& obj) {
    channelAudio.clear();
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
        int channelId = it.key().toInt();
        QJsonObject ch = it.value().toObject();
        ChannelAudioConfig cfg;
        cfg.volume   = static_cast<float>(ch["volume"].toDouble(1.0));
        cfg.priority = ch["priority"].toInt(5);
        cfg.muted    = ch["muted"].toBool(false);
        cfg.openMic  = ch["open_mic"].toBool(false);
        cfg.pan      = static_cast<float>(ch["pan"].toDouble(0.0));
        cfg.radioFilter = ch["radio"].toBool(false);
        channelAudio[channelId] = cfg;
    }
}

void AppConfig::uiFromJson(const QJsonObject& obj) {
    if (obj.contains("theme"))            ui.theme          = obj["theme"].toString("dark");
    if (obj.contains("window_x"))         ui.windowX        = obj["window_x"].toInt(100);
    if (obj.contains("window_y"))         ui.windowY        = obj["window_y"].toInt(100);
    if (obj.contains("window_width"))     ui.windowWidth    = obj["window_width"].toInt(900);
    if (obj.contains("window_height"))    ui.windowHeight   = obj["window_height"].toInt(600);
    if (obj.contains("show_activity_log")) ui.showActivityLog = obj["show_activity_log"].toBool(true);
}
