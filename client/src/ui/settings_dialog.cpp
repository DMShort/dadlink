#include "settings_dialog.h"
#include "audio/audio_engine.h"
#include "input/ptt_manager.h"
#include "ui/channel_tree.h"
#include "voice/voice_session.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QHeaderView>

SettingsDialog::SettingsDialog(AppConfig& config, AudioEngine* audioEngine,
                               PttManager* pttManager, ChannelTree* channelTree,
                               VoiceSession* voiceSession,
                               QWidget* parent)
    : QDialog(parent)
    , m_config(config)
    , m_audioEngine(audioEngine)
    , m_pttManager(pttManager)
    , m_channelTree(channelTree)
    , m_voiceSession(voiceSession)
{
    setupUi();
    loadFromConfig();
}

void SettingsDialog::setupUi() {
    setWindowTitle("Settings");
    setMinimumWidth(450);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    auto* mainLayout = new QVBoxLayout(this);

    m_tabs = new QTabWidget(this);
    m_tabs->addTab(createAudioTab(), "Audio");
    m_tabs->addTab(createVoiceTab(), "Voice");
    m_tabs->addTab(createHotkeyTab(), "Hotkeys");
    m_tabs->addTab(createChannelAudioTab(), "Channel Audio");
    m_tabs->addTab(createNetworkTab(), "Network");
    mainLayout->addWidget(m_tabs);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
    mainLayout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::onOk);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, &SettingsDialog::onApply);
}

QWidget* SettingsDialog::createAudioTab() {
    auto* widget = new QWidget(this);
    auto* layout = new QVBoxLayout(widget);

    // Device selection
    auto* deviceGroup = new QGroupBox("Devices", this);
    auto* deviceForm = new QFormLayout(deviceGroup);

    m_inputDevice = new QComboBox(this);
    m_inputDevice->addItem("Default", -1);
    if (m_audioEngine && m_audioEngine->isInitialized()) {
        for (const auto& dev : m_audioEngine->inputDevices()) {
            m_inputDevice->addItem(QString::fromStdString(dev.name), dev.id);
        }
    }
    deviceForm->addRow("Input:", m_inputDevice);

    m_outputDevice = new QComboBox(this);
    m_outputDevice->addItem("Default", -1);
    if (m_audioEngine && m_audioEngine->isInitialized()) {
        for (const auto& dev : m_audioEngine->outputDevices()) {
            m_outputDevice->addItem(QString::fromStdString(dev.name), dev.id);
        }
    }
    deviceForm->addRow("Output:", m_outputDevice);
    layout->addWidget(deviceGroup);

    // Volume controls
    auto* volumeGroup = new QGroupBox("Volume", this);
    auto* volumeForm = new QFormLayout(volumeGroup);

    auto* inputVolLayout = new QHBoxLayout();
    m_inputVolume = new QSlider(Qt::Horizontal, this);
    m_inputVolume->setRange(0, 200); // 0-200%
    m_inputVolLabel = new QLabel("100%", this);
    m_inputVolLabel->setMinimumWidth(45);
    inputVolLayout->addWidget(m_inputVolume);
    inputVolLayout->addWidget(m_inputVolLabel);
    volumeForm->addRow("Input:", inputVolLayout);

    connect(m_inputVolume, &QSlider::valueChanged, this, [this](int val) {
        m_inputVolLabel->setText(QString("%1%").arg(val));
    });

    auto* outputVolLayout = new QHBoxLayout();
    m_outputVolume = new QSlider(Qt::Horizontal, this);
    m_outputVolume->setRange(0, 200);
    m_outputVolLabel = new QLabel("100%", this);
    m_outputVolLabel->setMinimumWidth(45);
    outputVolLayout->addWidget(m_outputVolume);
    outputVolLayout->addWidget(m_outputVolLabel);
    volumeForm->addRow("Output:", outputVolLayout);

    connect(m_outputVolume, &QSlider::valueChanged, this, [this](int val) {
        m_outputVolLabel->setText(QString("%1%").arg(val));
    });

    // Noise gate
    auto* ngLayout = new QHBoxLayout();
    m_noiseGate = new QSlider(Qt::Horizontal, this);
    m_noiseGate->setRange(0, 100); // 0.00 - 1.00
    m_noiseGateLabel = new QLabel("0.01", this);
    m_noiseGateLabel->setMinimumWidth(45);
    ngLayout->addWidget(m_noiseGate);
    ngLayout->addWidget(m_noiseGateLabel);
    volumeForm->addRow("Noise Gate:", ngLayout);

    connect(m_noiseGate, &QSlider::valueChanged, this, [this](int val) {
        m_noiseGateLabel->setText(QString::number(val / 100.0, 'f', 2));
    });

    layout->addWidget(volumeGroup);
    layout->addStretch();
    return widget;
}

QWidget* SettingsDialog::createVoiceTab() {
    auto* widget = new QWidget(this);
    auto* layout = new QVBoxLayout(widget);

    auto* codecGroup = new QGroupBox("Opus Codec", this);
    auto* form = new QFormLayout(codecGroup);

    m_bitrate = new QSpinBox(this);
    m_bitrate->setRange(8000, 128000);
    m_bitrate->setSingleStep(1000);
    m_bitrate->setSuffix(" bps");
    form->addRow("Bitrate:", m_bitrate);

    m_complexity = new QSpinBox(this);
    m_complexity->setRange(0, 10);
    form->addRow("Complexity:", m_complexity);

    m_enableFec = new QCheckBox("Enable Forward Error Correction", this);
    form->addRow("", m_enableFec);

    m_enableDtx = new QCheckBox("Enable Discontinuous Transmission", this);
    form->addRow("", m_enableDtx);

    layout->addWidget(codecGroup);
    layout->addStretch();
    return widget;
}

QWidget* SettingsDialog::createHotkeyTab() {
    auto* widget = new QWidget(this);
    auto* layout = new QVBoxLayout(widget);

    // Info label
    auto* info = new QLabel(
        "Assign a push-to-talk input to each channel: a keyboard key, a mouse side/middle "
        "button, or any joystick / HOTAS button. Pick a channel, click \"Press to bind\", "
        "then press the key or button you want. Inputs work while Star Citizen has focus.", this);
    info->setWordWrap(true);
    info->setStyleSheet("padding: 4px; color: #ccc;");
    layout->addWidget(info);

    // Current bindings table
    auto* group = new QGroupBox("Channel PTT Bindings", this);
    auto* groupLayout = new QVBoxLayout(group);

    m_hotkeyTable = new QTableWidget(0, 3, this);
    m_hotkeyTable->setHorizontalHeaderLabels({"Channel", "Input", ""});
    m_hotkeyTable->horizontalHeader()->setStretchLastSection(false);
    m_hotkeyTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_hotkeyTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_hotkeyTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_hotkeyTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_hotkeyTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_hotkeyTable->verticalHeader()->hide();
    groupLayout->addWidget(m_hotkeyTable);

    // Add binding controls
    auto* addLayout = new QHBoxLayout();

    m_hotkeyChannelCombo = new QComboBox(this);
    m_hotkeyChannelCombo->setMinimumWidth(150);
    if (m_channelTree) {
        auto names = m_channelTree->channelNames();
        for (auto it = names.constBegin(); it != names.constEnd(); ++it) {
            m_hotkeyChannelCombo->addItem(it.value(), it.key());
        }
    }
    addLayout->addWidget(new QLabel("Channel:", this));
    addLayout->addWidget(m_hotkeyChannelCombo);

    m_bindButton = new QPushButton("Press to bind…", this);
    m_bindButton->setMinimumWidth(260);
    addLayout->addWidget(m_bindButton, 1);
    groupLayout->addLayout(addLayout);

    // Joystick status + rescan
    auto* joyLayout = new QHBoxLayout();
    m_joystickStatus = new QLabel(this);
    m_joystickStatus->setStyleSheet("color: #aaa;");
    auto* rescanBtn = new QPushButton("Rescan joysticks", this);
    joyLayout->addWidget(m_joystickStatus, 1);
    joyLayout->addWidget(rescanBtn);
    groupLayout->addLayout(joyLayout);
    updateJoystickStatus();

    connect(rescanBtn, &QPushButton::clicked, this, [this]() {
        if (m_pttManager) m_pttManager->rescanJoysticks();
        updateJoystickStatus();
        loadHotkeyTable(); // device names may now resolve
    });

    connect(m_bindButton, &QPushButton::clicked, this, [this]() {
        if (!m_pttManager) return;
        if (m_pttManager->isCapturing()) { m_pttManager->cancelCapture(); return; }
        if (m_hotkeyChannelCombo->currentData().toInt() <= 0) return;
        m_bindButton->setText("Press a key, mouse or stick button…  (Esc to cancel)");
        m_bindButton->setStyleSheet("background: #4a3000; color: #ffa500; font-weight: bold;");
        m_pttManager->startCapture();
    });

    if (m_pttManager) {
        connect(m_pttManager, &PttManager::bindingCaptured, this, [this](const QString& binding) {
            finishCapture();
            int channelId = m_hotkeyChannelCombo->currentData().toInt();
            if (channelId <= 0 || binding.isEmpty()) return;

            m_pttManager->setBinding(binding, channelId);

            // Save to config: one input per channel, one channel per input
            const QString key = PttManager::normalizeBinding(binding);
            for (auto it = m_config.hotkeys.begin(); it != m_config.hotkeys.end(); ) {
                if (it.value() == channelId || PttManager::normalizeBinding(it.key()) == key)
                    it = m_config.hotkeys.erase(it);
                else
                    ++it;
            }
            m_config.hotkeys[key] = channelId;
            loadHotkeyTable();
        });
        connect(m_pttManager, &PttManager::captureCancelled, this, [this]() { finishCapture(); });
        // Don't leave the manager in capture mode if the dialog closes mid-capture
        connect(this, &QDialog::finished, m_pttManager, &PttManager::cancelCapture);
    }

    layout->addWidget(group);
    layout->addStretch();
    return widget;
}

QWidget* SettingsDialog::createChannelAudioTab() {
    auto* widget = new QWidget(this);
    auto* layout = new QVBoxLayout(widget);

    // Info label
    auto* info = new QLabel(
        "Set per-channel volume and priority. Higher priority channels (lower number) "
        "will duck lower-priority channels when active.", this);
    info->setWordWrap(true);
    info->setStyleSheet("padding: 4px; color: #ccc;");
    layout->addWidget(info);

    // Channel audio table: Channel | Volume | Priority | Muted
    auto* group = new QGroupBox("Channel Audio Settings", this);
    auto* groupLayout = new QVBoxLayout(group);

    m_channelAudioTable = new QTableWidget(0, 4, this);
    m_channelAudioTable->setHorizontalHeaderLabels({"Channel", "Volume", "Priority", "Muted"});
    m_channelAudioTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_channelAudioTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_channelAudioTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    m_channelAudioTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
    m_channelAudioTable->setColumnWidth(1, 160);
    m_channelAudioTable->setColumnWidth(2, 80);
    m_channelAudioTable->setColumnWidth(3, 60);
    m_channelAudioTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_channelAudioTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_channelAudioTable->verticalHeader()->hide();
    groupLayout->addWidget(m_channelAudioTable);

    layout->addWidget(group);

    // Ducking controls
    auto* duckGroup = new QGroupBox("Audio Ducking", this);
    auto* duckLayout = new QFormLayout(duckGroup);

    m_duckingEnabled = new QCheckBox("Enable priority-based audio ducking", this);
    duckLayout->addRow("", m_duckingEnabled);

    auto* duckSliderLayout = new QHBoxLayout();
    m_duckLevel = new QSlider(Qt::Horizontal, this);
    m_duckLevel->setRange(0, 100); // 0% - 100%
    m_duckLevelLabel = new QLabel("30%", this);
    m_duckLevelLabel->setMinimumWidth(45);
    duckSliderLayout->addWidget(m_duckLevel);
    duckSliderLayout->addWidget(m_duckLevelLabel);
    duckLayout->addRow("Duck Level:", duckSliderLayout);

    auto* duckInfo = new QLabel(
        "Duck level controls how much lower-priority channels are reduced "
        "when a higher-priority channel is active. 0% = silent, 100% = no ducking.", this);
    duckInfo->setWordWrap(true);
    duckInfo->setStyleSheet("color: gray; font-size: 10px;");
    duckLayout->addRow(duckInfo);

    connect(m_duckLevel, &QSlider::valueChanged, this, [this](int val) {
        m_duckLevelLabel->setText(QString("%1%").arg(val));
    });

    layout->addWidget(duckGroup);
    layout->addStretch();
    return widget;
}

void SettingsDialog::loadChannelAudioTable() {
    if (!m_channelAudioTable) return;
    m_channelAudioTable->setRowCount(0);

    QMap<int, QString> channelNameMap;
    if (m_channelTree) {
        channelNameMap = m_channelTree->channelNames();
    }

    // Show all known channels (not just ones with custom settings)
    for (auto it = channelNameMap.constBegin(); it != channelNameMap.constEnd(); ++it) {
        int channelId = it.key();
        QString name = it.value();

        // Get current settings (or defaults)
        ChannelAudioConfig cfg;
        if (m_config.channelAudio.contains(channelId)) {
            cfg = m_config.channelAudio[channelId];
        }

        int row = m_channelAudioTable->rowCount();
        m_channelAudioTable->insertRow(row);

        // Channel name
        m_channelAudioTable->setItem(row, 0, new QTableWidgetItem(name));

        // Volume slider
        auto* volSlider = new QSlider(Qt::Horizontal, this);
        volSlider->setRange(0, 100);
        volSlider->setValue(static_cast<int>(cfg.volume * 100));
        m_channelAudioTable->setCellWidget(row, 1, volSlider);

        connect(volSlider, &QSlider::valueChanged, this, [this, channelId](int val) {
            float volume = val / 100.0f;
            m_config.channelAudio[channelId].volume = volume;
            if (m_voiceSession) {
                m_voiceSession->setChannelVolume(static_cast<uint32_t>(channelId), volume);
            }
        });

        // Priority spinbox
        auto* prioritySpin = new QSpinBox(this);
        prioritySpin->setRange(1, 10);
        prioritySpin->setValue(cfg.priority);
        m_channelAudioTable->setCellWidget(row, 2, prioritySpin);

        connect(prioritySpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this, channelId](int val) {
            m_config.channelAudio[channelId].priority = val;
            if (m_voiceSession) {
                m_voiceSession->setChannelPriority(static_cast<uint32_t>(channelId), val);
            }
        });

        // Muted checkbox
        auto* muteCheck = new QCheckBox(this);
        muteCheck->setChecked(cfg.muted);
        auto* muteContainer = new QWidget(this);
        auto* muteLayout = new QHBoxLayout(muteContainer);
        muteLayout->addWidget(muteCheck);
        muteLayout->setAlignment(Qt::AlignCenter);
        muteLayout->setContentsMargins(0, 0, 0, 0);
        m_channelAudioTable->setCellWidget(row, 3, muteContainer);

        connect(muteCheck, &QCheckBox::toggled, this, [this, channelId](bool muted) {
            m_config.channelAudio[channelId].muted = muted;
            if (m_voiceSession) {
                m_voiceSession->setChannelMuted(static_cast<uint32_t>(channelId), muted);
            }
        });
    }
}

QWidget* SettingsDialog::createNetworkTab() {
    auto* widget = new QWidget(this);
    auto* layout = new QVBoxLayout(widget);

    auto* group = new QGroupBox("Connection", this);
    auto* form = new QFormLayout(group);

    m_useTls = new QCheckBox("Use TLS (requires server restart to take effect)", this);
    form->addRow("", m_useTls);

    auto* info = new QLabel("Server address and port are configured in the login dialog.", this);
    info->setWordWrap(true);
    info->setStyleSheet("color: gray;");
    form->addRow(info);

    layout->addWidget(group);
    layout->addStretch();
    return widget;
}

void SettingsDialog::loadFromConfig() {
    // Audio
    int inputIdx = m_inputDevice->findData(m_config.audio.inputDeviceId);
    if (inputIdx >= 0) m_inputDevice->setCurrentIndex(inputIdx);

    int outputIdx = m_outputDevice->findData(m_config.audio.outputDeviceId);
    if (outputIdx >= 0) m_outputDevice->setCurrentIndex(outputIdx);

    m_inputVolume->setValue(static_cast<int>(m_config.audio.inputVolume * 100));
    m_outputVolume->setValue(static_cast<int>(m_config.audio.outputVolume * 100));
    m_noiseGate->setValue(static_cast<int>(m_config.audio.noiseGateThreshold * 100));

    // Voice
    m_bitrate->setValue(m_config.opus.bitrate);
    m_complexity->setValue(m_config.opus.complexity);
    m_enableFec->setChecked(m_config.opus.enableFec);
    m_enableDtx->setChecked(m_config.opus.enableDtx);

    // Network
    m_useTls->setChecked(m_config.server.useTls);

    // Hotkeys
    loadHotkeyTable();

    // Channel Audio
    loadChannelAudioTable();
    if (m_duckingEnabled) m_duckingEnabled->setChecked(m_config.voice.duckingEnabled);
    if (m_duckLevel) m_duckLevel->setValue(static_cast<int>(m_config.voice.duckLevel * 100));
}

void SettingsDialog::loadHotkeyTable() {
    if (!m_hotkeyTable) return;

    m_hotkeyTable->setRowCount(0);

    // Get channel names for display
    QMap<int, QString> channelNameMap;
    if (m_channelTree) {
        channelNameMap = m_channelTree->channelNames();
    }

    for (auto it = m_config.hotkeys.constBegin(); it != m_config.hotkeys.constEnd(); ++it) {
        const QString binding = it.key();
        int channelId = it.value();
        if (binding.isEmpty() || channelId <= 0) continue;

        int row = m_hotkeyTable->rowCount();
        m_hotkeyTable->insertRow(row);

        QString chName = channelNameMap.value(channelId, QString("#%1").arg(channelId));
        QString inputName = m_pttManager ? m_pttManager->displayName(binding) : binding;
        m_hotkeyTable->setItem(row, 0, new QTableWidgetItem(chName));
        m_hotkeyTable->setItem(row, 1, new QTableWidgetItem(inputName));

        auto* removeBtn = new QPushButton("Remove", this);
        m_hotkeyTable->setCellWidget(row, 2, removeBtn);

        connect(removeBtn, &QPushButton::clicked, this, [this, binding, channelId]() {
            m_config.hotkeys.remove(binding);
            if (m_pttManager) {
                m_pttManager->removeHotkey(channelId);
            }
            loadHotkeyTable();
        });
    }
}

void SettingsDialog::finishCapture() {
    if (!m_bindButton) return;
    m_bindButton->setText("Press to bind…");
    m_bindButton->setStyleSheet("");
}

void SettingsDialog::updateJoystickStatus() {
    if (!m_joystickStatus) return;
    int n = m_pttManager ? m_pttManager->joystickCount() : 0;
    m_joystickStatus->setText(n == 0 ? QStringLiteral("No joysticks detected")
                                     : QStringLiteral("%1 joystick/HOTAS device%2 detected").arg(n).arg(n == 1 ? "" : "s"));
}

void SettingsDialog::saveToConfig() {
    // Audio
    m_config.audio.inputDeviceId = m_inputDevice->currentData().toInt();
    m_config.audio.outputDeviceId = m_outputDevice->currentData().toInt();
    m_config.audio.inputVolume = m_inputVolume->value() / 100.0f;
    m_config.audio.outputVolume = m_outputVolume->value() / 100.0f;
    m_config.audio.noiseGateThreshold = m_noiseGate->value() / 100.0f;

    // Voice
    m_config.opus.bitrate = m_bitrate->value();
    m_config.opus.complexity = m_complexity->value();
    m_config.opus.enableFec = m_enableFec->isChecked();
    m_config.opus.enableDtx = m_enableDtx->isChecked();

    // Network
    m_config.server.useTls = m_useTls->isChecked();

    // Hotkeys are saved immediately when added/removed

    // Ducking
    if (m_duckingEnabled) m_config.voice.duckingEnabled = m_duckingEnabled->isChecked();
    if (m_duckLevel) m_config.voice.duckLevel = m_duckLevel->value() / 100.0f;

    // Apply ducking to voice session
    if (m_voiceSession) {
        m_voiceSession->setDuckingEnabled(m_config.voice.duckingEnabled);
        m_voiceSession->setDuckLevel(m_config.voice.duckLevel);
    }

    // Channel audio is saved immediately when sliders/spinboxes change

    m_config.save();
}

void SettingsDialog::onApply() {
    saveToConfig();
    emit audioSettingsChanged();
    emit voiceSettingsChanged();
}

void SettingsDialog::onOk() {
    saveToConfig();
    emit audioSettingsChanged();
    emit voiceSettingsChanged();
    accept();
}

