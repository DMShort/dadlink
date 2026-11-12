#pragma once

#include <QObject>
#include <QSet>
#include <QUdpSocket>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <memory>
#include <map>
#include <mutex>
#include <unordered_map>

#include "audio/audio_engine.h"
#include "audio/opus_codec.h"
#include "crypto/srtp_session.h"
#include "voice/voice_packet.h"
#include "voice/jitter_buffer.h"

/// Voice session orchestrating the full pipeline:
///   Capture → Opus encode → SRTP encrypt → UDP send (transmit)
///   UDP recv → SRTP decrypt → Jitter buffer → Opus decode → Playback (receive)
class VoiceSession : public QObject {
    Q_OBJECT

public:
    explicit VoiceSession(QObject* parent = nullptr);
    ~VoiceSession() override;

    /// Initialize audio engine and Opus codec.
    bool initialize(int inputDeviceId = -1, int outputDeviceId = -1,
                    int bitrate = 32000, int complexity = 10,
                    bool fec = true, bool dtx = true);

    /// Set the SRTP session for encryption/decryption.
    void setSrtpSession(SrtpSession* session);

    /// Set the server address for UDP voice traffic.
    void setServer(const QString& host, int voicePort);

    /// Set our user ID and current channel (clears jitter buffers on channel change).
    void setIdentity(uint32_t userId, uint32_t channelId);

    /// Switch the transmit-target channel without clearing receive state.
    /// Use this for multi-channel PTT where we stay joined to all channels.
    void setTransmitChannel(uint32_t channelId);

    /// Start the voice session (audio + UDP).
    bool start();

    /// Stop the voice session.
    void stop();

    /// Set PTT state — when true, captured audio is transmitted.
    void setPttActive(bool active);

    /// Enable/disable open mic for a channel.
    /// Open-mic channels always transmit regardless of PTT state.
    void setOpenMic(uint32_t channelId, bool enabled);

    /// Set input/output volume.
    void setInputVolume(float v);
    void setOutputVolume(float v);

    /// Switch audio devices. If the session is running, restarts the audio streams.
    void setDevices(int inputDeviceId, int outputDeviceId);

    /// Per-channel audio controls.
    void setChannelVolume(uint32_t channelId, float volume);  // 0.0 – 1.0
    void setChannelPriority(uint32_t channelId, int priority); // 1 = highest
    void setChannelMuted(uint32_t channelId, bool muted);
    void removeChannelSettings(uint32_t channelId);

    /// Audio ducking: when a high-priority channel is active, lower-priority
    /// channels are reduced to duckLevel (0.0–1.0, default 0.3 = 30%).
    void setDuckingEnabled(bool enabled) { m_duckingEnabled.store(enabled, std::memory_order_relaxed); }
    void setDuckLevel(float level) { m_duckLevel.store(level, std::memory_order_relaxed); }

    /// Audio levels for VU meters.
    float inputLevel() const;
    float outputLevel() const;

    bool isRunning() const { return m_running.load(std::memory_order_relaxed); }

    /// Access the audio engine (for settings dialog device enumeration).
    AudioEngine& audioEngine() { return m_audioEngine; }

signals:
    void started();
    void stopped();
    void error(const QString& message);

private slots:
    void onTransmitTimer();
    void onReadyRead();

private:
    void processReceivedPacket(const QByteArray& data);

    AudioEngine m_audioEngine;
    OpusCodec m_opusCodec;
    SrtpSession* m_srtpSession = nullptr;

    QUdpSocket m_udpSocket;
    QString m_serverHost;
    QHostAddress m_resolvedAddr; // cached resolved IPv4 address
    int m_voicePort = 9001;

    int m_inputDeviceId = -1;
    int m_outputDeviceId = -1;

    uint32_t m_userId = 0;
    uint32_t m_channelId = 0;
    uint64_t m_txSequence = 0;
    uint64_t m_txTimestamp = 0;

    // Transmit timer (20ms intervals)
    QTimer m_transmitTimer;

    // Per-source jitter buffers and decoders
    struct SourceState {
        JitterBuffer jitterBuffer;
        OpusCodec decoder;
        uint32_t channelId = 0; // which channel this source is transmitting on
        SourceState() : jitterBuffer(20) {}
    };
    std::map<uint32_t, std::unique_ptr<SourceState>> m_sources;
    std::mutex m_sourcesMutex;

    // Per-channel audio settings
    struct ChannelAudioSettings {
        float volume = 1.0f;   // 0.0 – 1.0
        int priority = 5;     // 1 = highest, 10 = lowest
        bool muted = false;
    };
    std::unordered_map<uint32_t, ChannelAudioSettings> m_channelSettings;
    std::mutex m_channelSettingsMutex;

    // Ducking
    std::atomic<bool> m_duckingEnabled{true};
    std::atomic<float> m_duckLevel{0.3f}; // reduce ducked channels to 30%

    // Playback mixing timer
    QTimer m_playbackTimer;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_pttActive{false};

    // Channels that always transmit (open mic) — Qt main thread only, no mutex needed
    QSet<uint32_t> m_openMicChannels;

    void onPlaybackTimer();

    // UDP keepalive while not transmitting (registers address with server, keeps NAT open)
    void sendKeepalive();
    static constexpr int KeepaliveIntervalTicks = 250; // 250 x 20ms = 5s
    int m_keepaliveTicks = KeepaliveIntervalTicks;     // send one immediately on start
};
