#pragma once

#include <QDialog>
#include <QTabWidget>
#include <QComboBox>
#include <QSlider>
#include <QSpinBox>
#include <QCheckBox>
#include <QLabel>
#include <QTableWidget>
#include <QPushButton>
#include <QMap>

#include "config/config.h"

class AudioEngine;
class PttManager;
class ChannelTree;

class VoiceSession;

/// Settings dialog with tabs for Audio, Voice, Hotkeys, Channel Audio, and Network.
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    SettingsDialog(AppConfig& config, AudioEngine* audioEngine,
                   PttManager* pttManager = nullptr,
                   ChannelTree* channelTree = nullptr,
                   VoiceSession* voiceSession = nullptr,
                   QWidget* parent = nullptr);

signals:
    /// Emitted when audio settings change (device, volume).
    void audioSettingsChanged();

    /// Emitted when voice settings change (bitrate, FEC, DTX).
    void voiceSettingsChanged();

private slots:
    void onApply();
    void onOk();

private:
    void setupUi();
    QWidget* createAudioTab();
    QWidget* createVoiceTab();
    QWidget* createHotkeyTab();
    QWidget* createChannelAudioTab();
    QWidget* createNetworkTab();
    void loadFromConfig();
    void saveToConfig();
    void loadHotkeyTable();
    void loadChannelAudioTable();


    AppConfig& m_config;
    AudioEngine* m_audioEngine;
    PttManager* m_pttManager;
    ChannelTree* m_channelTree;
    VoiceSession* m_voiceSession;
    QTabWidget* m_tabs;

    // Audio tab
    QComboBox* m_inputDevice;
    QComboBox* m_outputDevice;
    QSlider* m_inputVolume;
    QSlider* m_outputVolume;
    QLabel* m_inputVolLabel;
    QLabel* m_outputVolLabel;
    QSlider* m_noiseGate;
    QLabel* m_noiseGateLabel;

    // Voice tab
    QSpinBox* m_bitrate;
    QSpinBox* m_complexity;
    QCheckBox* m_enableFec;
    QCheckBox* m_enableDtx;

    // Hotkey tab
    QTableWidget* m_hotkeyTable = nullptr;
    QComboBox* m_hotkeyChannelCombo = nullptr;
    QPushButton* m_bindButton = nullptr;   // "Press to bind…" capture button
    QLabel* m_joystickStatus = nullptr;    // "2 joysticks detected"
    void updateJoystickStatus();
    void finishCapture();

    // Channel Audio tab
    QTableWidget* m_channelAudioTable = nullptr;
    QCheckBox* m_duckingEnabled = nullptr;
    QSlider* m_duckLevel = nullptr;
    QLabel* m_duckLevelLabel = nullptr;

    // Network tab
    QCheckBox* m_useTls;
};
