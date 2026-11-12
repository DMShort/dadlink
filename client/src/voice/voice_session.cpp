#include "voice_session.h"

#include <QHostAddress>
#include <QHostInfo>
#include <QNetworkDatagram>
#include <QDebug>
#include <cmath>
#include <cstring>
#include <algorithm>

VoiceSession::VoiceSession(QObject* parent)
    : QObject(parent)
{
    m_transmitTimer.setInterval(20); // 20ms = one Opus frame
    m_transmitTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_transmitTimer, &QTimer::timeout, this, &VoiceSession::onTransmitTimer);

    m_playbackTimer.setInterval(20);
    m_playbackTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_playbackTimer, &QTimer::timeout, this, &VoiceSession::onPlaybackTimer);

    connect(&m_udpSocket, &QUdpSocket::readyRead, this, &VoiceSession::onReadyRead);
}

VoiceSession::~VoiceSession() {
    stop();
}

bool VoiceSession::initialize(int inputDeviceId, int outputDeviceId,
                               int bitrate, int complexity, bool fec, bool dtx) {
    m_inputDeviceId = inputDeviceId;
    m_outputDeviceId = outputDeviceId;

    if (!m_audioEngine.initialize()) {
        emit error("Failed to initialize audio engine");
        return false;
    }

    if (!m_opusCodec.initialize(bitrate, complexity, fec, dtx)) {
        emit error("Failed to initialize Opus codec");
        return false;
    }

    return true;
}

void VoiceSession::setSrtpSession(SrtpSession* session) {
    m_srtpSession = session;
}

void VoiceSession::setServer(const QString& host, int voicePort) {
    m_serverHost = host;
    m_voicePort = voicePort;

    // Pre-resolve hostname to IPv4 address
    m_resolvedAddr = QHostAddress(host);
    if (m_resolvedAddr.isNull()) {
        auto hostInfo = QHostInfo::fromName(host);
        for (const auto& addr : hostInfo.addresses()) {
            if (addr.protocol() == QAbstractSocket::IPv4Protocol) {
                m_resolvedAddr = addr;
                break;
            }
        }
    }
    qDebug() << "[VOICE] Server resolved:" << host << "->" << m_resolvedAddr.toString();
}

void VoiceSession::setIdentity(uint32_t userId, uint32_t channelId) {
    bool channelChanged = (m_channelId != channelId);
    m_userId = userId;
    m_channelId = channelId;

    // Clear jitter buffers on channel change so stale data doesn't block
    if (channelChanged) {
        std::lock_guard<std::mutex> lock(m_sourcesMutex);
        m_sources.clear();
        qDebug() << "[VOICE] Channel changed to" << channelId << "- cleared jitter buffers";
    }
}

void VoiceSession::setTransmitChannel(uint32_t channelId) {
    m_channelId = channelId;
    qDebug() << "[VOICE] Transmit channel set to" << channelId;
}

bool VoiceSession::start() {
    qDebug() << "[VOICE] start() called, running=" << m_running.load();
    if (m_running.load()) return true;

    // Bind UDP socket
    if (!m_udpSocket.bind(QHostAddress::AnyIPv4, 0)) {
        qDebug() << "[VOICE] Failed to bind UDP:" << m_udpSocket.errorString();
        emit error("Failed to bind UDP socket");
        return false;
    }
    qDebug() << "[VOICE] UDP bound to port" << m_udpSocket.localPort();

    // Start audio streams
    if (!m_audioEngine.startCapture(m_inputDeviceId)) {
        emit error(QString("Failed to start audio capture: %1")
                   .arg(QString::fromStdString(m_audioEngine.lastError())));
        return false;
    }

    if (!m_audioEngine.startPlayback(m_outputDeviceId)) {
        emit error(QString("Failed to start audio playback: %1")
                   .arg(QString::fromStdString(m_audioEngine.lastError())));
        m_audioEngine.stopCapture();
        return false;
    }

    m_txSequence = 0;
    m_txTimestamp = 0;

    m_transmitTimer.start();
    m_playbackTimer.start();
    m_running.store(true, std::memory_order_relaxed);

    emit started();
    return true;
}

void VoiceSession::stop() {
    if (!m_running.load()) return;

    m_transmitTimer.stop();
    m_playbackTimer.stop();
    m_audioEngine.stopCapture();
    m_audioEngine.stopPlayback();
    m_udpSocket.close();

    {
        std::lock_guard<std::mutex> lock(m_sourcesMutex);
        m_sources.clear();
    }

    m_running.store(false, std::memory_order_relaxed);
    emit stopped();
}

void VoiceSession::setPttActive(bool active) {
    qDebug() << "[VOICE] setPttActive(" << active << ") ch=" << m_channelId << "user=" << m_userId;
    m_pttActive.store(active, std::memory_order_relaxed);
}

void VoiceSession::setOpenMic(uint32_t channelId, bool enabled) {
    if (enabled) {
        m_openMicChannels.insert(channelId);
    } else {
        m_openMicChannels.remove(channelId);
    }
    qDebug() << "[VOICE] open mic" << (enabled ? "ON" : "OFF") << "for ch=" << channelId;
}

void VoiceSession::setInputVolume(float v) {
    m_audioEngine.setInputVolume(v);
}

void VoiceSession::setOutputVolume(float v) {
    m_audioEngine.setOutputVolume(v);
}

void VoiceSession::setDevices(int inputDeviceId, int outputDeviceId) {
    m_inputDeviceId = inputDeviceId;
    m_outputDeviceId = outputDeviceId;

    if (!m_running.load()) return;

    m_audioEngine.stopCapture();
    m_audioEngine.stopPlayback();

    if (!m_audioEngine.startCapture(m_inputDeviceId))
        emit error(QString("Failed to restart audio capture: %1")
                   .arg(QString::fromStdString(m_audioEngine.lastError())));

    if (!m_audioEngine.startPlayback(m_outputDeviceId))
        emit error(QString("Failed to restart audio playback: %1")
                   .arg(QString::fromStdString(m_audioEngine.lastError())));
}

float VoiceSession::inputLevel() const {
    return m_audioEngine.inputLevel();
}

float VoiceSession::outputLevel() const {
    return m_audioEngine.outputLevel();
}

void VoiceSession::setChannelVolume(uint32_t channelId, float volume) {
    std::lock_guard<std::mutex> lock(m_channelSettingsMutex);
    m_channelSettings[channelId].volume = std::clamp(volume, 0.0f, 1.0f);
}

void VoiceSession::setChannelPriority(uint32_t channelId, int priority) {
    std::lock_guard<std::mutex> lock(m_channelSettingsMutex);
    m_channelSettings[channelId].priority = std::clamp(priority, 1, 10);
}

void VoiceSession::setChannelMuted(uint32_t channelId, bool muted) {
    std::lock_guard<std::mutex> lock(m_channelSettingsMutex);
    m_channelSettings[channelId].muted = muted;
}

void VoiceSession::setChannelPan(uint32_t channelId, float pan) {
    std::lock_guard<std::mutex> lock(m_channelSettingsMutex);
    m_channelSettings[channelId].pan = std::clamp(pan, -1.0f, 1.0f);
}

void VoiceSession::setChannelRadioFilter(uint32_t channelId, bool enabled) {
    std::lock_guard<std::mutex> lock(m_channelSettingsMutex);
    m_channelSettings[channelId].radioFilter = enabled;
}

void VoiceSession::removeChannelSettings(uint32_t channelId) {
    std::lock_guard<std::mutex> lock(m_channelSettingsMutex);
    m_channelSettings.erase(channelId);
}

// --- Transmit path (20ms timer) ---

void VoiceSession::onTransmitTimer() {
    bool pttActive = m_pttActive.load(std::memory_order_relaxed);
    bool hasOpenMic = !m_openMicChannels.isEmpty();

    if (!pttActive && !hasOpenMic) {
        // Not talking: send an encrypted empty keepalive every ~5s so the server
        // knows our UDP address (listen-only users would otherwise hear nothing)
        // and the NAT mapping stays open.
        if (++m_keepaliveTicks >= KeepaliveIntervalTicks) {
            m_keepaliveTicks = 0;
            sendKeepalive();
        }
        return;
    }
    m_keepaliveTicks = 0;
    if (!m_srtpSession || !m_srtpSession->isReady()) {
        if (m_txSequence == 0) qDebug() << "[TX] SRTP not ready";
        return;
    }

    // Read and encode audio once regardless of how many channels we transmit to
    float pcm[AudioEngine::FramesPerBuffer];
    size_t read = m_audioEngine.readCapture(pcm, AudioEngine::FramesPerBuffer);
    if (read < AudioEngine::FramesPerBuffer) {
        if (m_txSequence == 0) qDebug() << "[TX] Not enough capture data:" << read;
        return;
    }

    auto encoded = m_opusCodec.encode(pcm, AudioEngine::FramesPerBuffer);
    if (encoded.empty()) { qDebug() << "[TX] Opus encode failed"; return; }

    auto encrypted = m_srtpSession->encrypt(encoded.data(), encoded.size(), m_txSequence);
    if (encrypted.empty()) { qDebug() << "[TX] SRTP encrypt failed"; return; }

    // Build the set of channels to transmit to this tick:
    //   - always: all open-mic channels
    //   - if PTT held: also the current PTT target channel
    QSet<uint32_t> targets = m_openMicChannels;
    if (pttActive) {
        targets.insert(m_channelId);
    }

    for (uint32_t chId : targets) {
        VoicePacket pkt;
        pkt.sequence  = m_txSequence;
        pkt.timestamp = m_txTimestamp;
        pkt.channelId = chId;
        pkt.userId    = m_userId;

        auto bytes = pkt.toBytes(encrypted.data(), encrypted.size());

        if (m_txSequence % 50 == 0) {
            qDebug() << "[TX] seq=" << m_txSequence << "ch=" << chId
                     << "user=" << m_userId << "size=" << bytes.size()
                     << (m_openMicChannels.contains(chId) ? "[open-mic]" : "[ptt]");
        }

        m_udpSocket.writeDatagram(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<qint64>(bytes.size()),
            m_resolvedAddr,
            static_cast<quint16>(m_voicePort)
        );
    }

    m_txSequence++;
    m_txTimestamp += AudioEngine::FramesPerBuffer;
}

void VoiceSession::sendKeepalive() {
    if (m_channelId == 0 || !m_srtpSession || !m_srtpSession->isReady()) return;

    const uint8_t dummy = 0;
    auto encrypted = m_srtpSession->encrypt(&dummy, 0, m_txSequence); // empty payload, tag only
    if (encrypted.empty()) return;

    VoicePacket pkt;
    pkt.sequence  = m_txSequence;
    pkt.timestamp = m_txTimestamp;
    pkt.channelId = m_channelId;
    pkt.userId    = m_userId;

    auto bytes = pkt.toBytes(encrypted.data(), encrypted.size());
    m_udpSocket.writeDatagram(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<qint64>(bytes.size()),
        m_resolvedAddr,
        static_cast<quint16>(m_voicePort)
    );
    m_txSequence++; // never reuse a GCM nonce
}

// --- Receive path ---

void VoiceSession::onReadyRead() {
    while (m_udpSocket.hasPendingDatagrams()) {
        QByteArray data;
        data.resize(static_cast<int>(m_udpSocket.pendingDatagramSize()));
        m_udpSocket.readDatagram(data.data(), data.size());
        processReceivedPacket(data);
    }
}

void VoiceSession::processReceivedPacket(const QByteArray& data) {
    if (!m_srtpSession || !m_srtpSession->isReady()) {
        qDebug() << "[RX] SRTP not ready, dropping packet";
        return;
    }

    auto pkt = VoicePacket::parse(
        reinterpret_cast<const uint8_t*>(data.constData()),
        static_cast<size_t>(data.size()));

    if (!pkt) {
        qDebug() << "[RX] Failed to parse voice packet, size=" << data.size();
        return;
    }

    // Don't play back our own audio
    if (pkt->userId == m_userId) return;

    qDebug() << "[RX] Got pkt from user=" << pkt->userId << "seq=" << pkt->sequence
             << "payload=" << pkt->payload.size();

    // SRTP decrypt
    auto decrypted = m_srtpSession->decrypt(
        pkt->payload.data(), pkt->payload.size(), pkt->sequence);

    if (decrypted.empty()) {
        qDebug() << "[RX] SRTP decrypt failed for seq=" << pkt->sequence;
        return;
    }

    // Feed into per-source jitter buffer
    std::lock_guard<std::mutex> lock(m_sourcesMutex);
    auto& source = m_sources[pkt->userId];
    if (!source) {
        source = std::make_unique<SourceState>();
        source->decoder.initialize();
    }
    source->channelId = pkt->channelId; // track which channel this source is on

    source->jitterBuffer.push(pkt->sequence, pkt->timestamp, std::move(decrypted));
}

// --- Playback mixing (20ms timer) ---

void VoiceSession::onPlaybackTimer() {
    constexpr int N = AudioEngine::FramesPerBuffer;
    float mixed[N * AudioEngine::PlaybackChannels]; // interleaved L/R
    std::memset(mixed, 0, sizeof(mixed));
    bool hasAudio = false;

    std::lock_guard<std::mutex> lock(m_sourcesMutex);

    // First pass: decode all sources and find the highest active priority
    struct DecodedSource {
        float pcm[AudioEngine::FramesPerBuffer];
        int decoded = 0;
        uint32_t channelId = 0;
    };
    std::vector<DecodedSource> decoded_sources;
    int highestActivePriority = 999; // lower number = higher priority

    {
        std::lock_guard<std::mutex> csLock(m_channelSettingsMutex);

        for (auto& [userId, source] : m_sources) {
            auto frame = source->jitterBuffer.pop();
            if (!frame) continue;

            // Check if this channel is muted
            uint32_t chId = source->channelId;
            auto settingsIt = m_channelSettings.find(chId);
            if (settingsIt != m_channelSettings.end() && settingsIt->second.muted) {
                continue; // skip muted channels entirely
            }

            DecodedSource ds;
            ds.channelId = chId;

            if (frame->payload.empty()) {
                ds.decoded = source->decoder.decodePlc(ds.pcm, AudioEngine::FramesPerBuffer);
            } else {
                ds.decoded = source->decoder.decode(
                    frame->payload.data(), static_cast<int>(frame->payload.size()),
                    ds.pcm, AudioEngine::FramesPerBuffer);
            }

            if (ds.decoded <= 0) continue;

            // Radio effect is per speaker so its filter state stays continuous
            if (settingsIt != m_channelSettings.end() && settingsIt->second.radioFilter) {
                source->radio.process(ds.pcm, std::min(ds.decoded, N));
            }

            // Track highest priority among active sources
            int priority = 5; // default
            if (settingsIt != m_channelSettings.end()) {
                priority = settingsIt->second.priority;
            }
            if (priority < highestActivePriority) {
                highestActivePriority = priority;
            }

            decoded_sources.push_back(std::move(ds));
        }

        // Second pass: mix with per-channel volume and ducking
        bool duckingOn = m_duckingEnabled.load(std::memory_order_relaxed);
        float duckLvl = m_duckLevel.load(std::memory_order_relaxed);

        for (auto& ds : decoded_sources) {
            float volume = 1.0f;
            int priority = 5;
            float pan = 0.0f;

            auto settingsIt = m_channelSettings.find(ds.channelId);
            if (settingsIt != m_channelSettings.end()) {
                volume = settingsIt->second.volume;
                priority = settingsIt->second.priority;
                pan = settingsIt->second.pan;
            }

            // Apply ducking: if a higher-priority channel is active, duck this one
            if (duckingOn && priority > highestActivePriority) {
                volume *= duckLvl;
            }

            // Balance-law pan: centre keeps full level in both ears (same loudness as
            // the old mono mix); moving one way fades the opposite ear to silence.
            const float gainL = volume * std::min(1.0f, 1.0f - pan);
            const float gainR = volume * std::min(1.0f, 1.0f + pan);

            for (int i = 0; i < ds.decoded && i < N; ++i) {
                mixed[i * 2]     += ds.pcm[i] * gainL;
                mixed[i * 2 + 1] += ds.pcm[i] * gainR;
            }
            hasAudio = true;
        }
    } // release channelSettingsMutex

    if (hasAudio) {
        // Hard clip to [-1, 1]
        for (int i = 0; i < N * AudioEngine::PlaybackChannels; ++i) {
            mixed[i] = std::clamp(mixed[i], -1.0f, 1.0f);
        }
        m_audioEngine.writePlayback(mixed, N);
    }
}
