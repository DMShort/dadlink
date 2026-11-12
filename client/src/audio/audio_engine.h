#pragma once

#include <portaudio.h>
#include <atomic>
#include <vector>
#include <string>
#include <functional>
#include <memory>

#include "ring_buffer.h"

/// Audio device info for UI display.
struct AudioDeviceInfo {
    int id;
    std::string name;
    int maxInputChannels;
    int maxOutputChannels;
    double defaultSampleRate;
};

/// Audio engine wrapping PortAudio.
/// Capture: 48kHz mono float32. Playback: 48kHz stereo float32 (interleaved L/R) so
/// channels can be panned; falls back to a mono device stream (downmixed) if stereo
/// can't be opened.
/// Uses lock-free ring buffers for inter-thread communication.
class AudioEngine {
public:
    static constexpr int SampleRate = 48000;
    static constexpr int FramesPerBuffer = 960; // 20ms at 48kHz
    static constexpr int Channels = 1;          // mono (capture + Opus)
    static constexpr int PlaybackChannels = 2;  // stereo interleaved playback buffer

    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    /// Initialize PortAudio. Must be called before anything else.
    bool initialize();

    /// Terminate PortAudio. Called by destructor.
    void terminate();

    /// List available input devices.
    std::vector<AudioDeviceInfo> inputDevices() const;

    /// List available output devices.
    std::vector<AudioDeviceInfo> outputDevices() const;

    /// Start capture stream. -1 = default device.
    bool startCapture(int deviceId = -1);

    /// Stop capture stream.
    void stopCapture();

    /// Start playback stream. -1 = default device.
    bool startPlayback(int deviceId = -1);

    /// Stop playback stream.
    void stopPlayback();

    /// Read captured audio from the ring buffer.
    /// Returns number of samples actually read.
    size_t readCapture(float* buffer, size_t frames);

    /// Write interleaved stereo audio (L,R,L,R…) to the playback ring buffer.
    /// frames = number of stereo frames. Returns frames actually written.
    size_t writePlayback(const float* stereo, size_t frames);

    /// Channels the output device was actually opened with (2, or 1 on fallback).
    int outputChannels() const { return m_outputChannels; }

    /// Input volume multiplier (0.0 - 1.0+). Atomic, safe from audio callback.
    void setInputVolume(float volume) { m_inputVolume.store(volume, std::memory_order_relaxed); }
    float inputVolume() const { return m_inputVolume.load(std::memory_order_relaxed); }

    /// Output volume multiplier (0.0 - 1.0+). Atomic, safe from audio callback.
    void setOutputVolume(float volume) { m_outputVolume.store(volume, std::memory_order_relaxed); }
    float outputVolume() const { return m_outputVolume.load(std::memory_order_relaxed); }

    /// Current RMS level of input (for VU meter). Updated by callback.
    float inputLevel() const { return m_inputLevel.load(std::memory_order_relaxed); }

    /// Current RMS level of output (for VU meter). Updated by callback.
    float outputLevel() const { return m_outputLevel.load(std::memory_order_relaxed); }

    bool isCapturing() const { return m_captureStream != nullptr; }
    bool isPlaying() const { return m_playbackStream != nullptr; }
    bool isInitialized() const { return m_initialized; }

    /// Human-readable description of the last PortAudio error.
    const std::string& lastError() const { return m_lastError; }

    /// Name of the device currently open for capture/playback (empty if not open).
    std::string captureDeviceName() const;
    std::string playbackDeviceName() const;

private:
    static int captureCallback(const void* input, void* output,
                               unsigned long frameCount,
                               const PaStreamCallbackTimeInfo* timeInfo,
                               PaStreamCallbackFlags statusFlags,
                               void* userData);

    static int playbackCallback(const void* input, void* output,
                                unsigned long frameCount,
                                const PaStreamCallbackTimeInfo* timeInfo,
                                PaStreamCallbackFlags statusFlags,
                                void* userData);

    bool m_initialized = false;
    PaStream* m_captureStream = nullptr;
    PaStream* m_playbackStream = nullptr;
    std::string m_lastError;
    PaDeviceIndex m_captureDeviceIndex = paNoDevice;
    PaDeviceIndex m_playbackDeviceIndex = paNoDevice;

    // Ring buffers: ~200ms capacity (about 10 frames)
    std::unique_ptr<RingBuffer<float>> m_captureBuffer;
    std::unique_ptr<RingBuffer<float>> m_playbackBuffer; // interleaved stereo
    int m_outputChannels = PlaybackChannels;

    std::atomic<float> m_inputVolume{1.0f};
    std::atomic<float> m_outputVolume{1.0f};
    std::atomic<float> m_inputLevel{0.0f};
    std::atomic<float> m_outputLevel{0.0f};
};
