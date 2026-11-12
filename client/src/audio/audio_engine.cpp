#include "audio_engine.h"

#include <algorithm>
#include <cmath>
#include <cstring>

AudioEngine::AudioEngine()
    : m_captureBuffer(std::make_unique<RingBuffer<float>>(SampleRate / 5))   // ~200ms
    , m_playbackBuffer(std::make_unique<RingBuffer<float>>(SampleRate / 5 * PlaybackChannels))
{}

AudioEngine::~AudioEngine() {
    terminate();
}

bool AudioEngine::initialize() {
    if (m_initialized) return true;

    PaError err = Pa_Initialize();
    if (err != paNoError) return false;

    m_initialized = true;
    return true;
}

void AudioEngine::terminate() {
    stopCapture();
    stopPlayback();
    if (m_initialized) {
        Pa_Terminate();
        m_initialized = false;
    }
}

// Returns the WASAPI host API index, or -1 if not available (non-Windows).
static PaHostApiIndex wasapiHostApi() {
    return Pa_HostApiTypeIdToHostApiIndex(paWASAPI);
}

// Resolves device id -1 to the WASAPI default input/output, falling back to
// the PortAudio global default if WASAPI isn't available.
static PaDeviceIndex resolveInputDevice(int deviceId) {
    if (deviceId >= 0) return deviceId;
    PaHostApiIndex wasapi = wasapiHostApi();
    if (wasapi >= 0) {
        const PaHostApiInfo* info = Pa_GetHostApiInfo(wasapi);
        if (info && info->defaultInputDevice != paNoDevice)
            return info->defaultInputDevice;
    }
    return Pa_GetDefaultInputDevice();
}

static PaDeviceIndex resolveOutputDevice(int deviceId) {
    if (deviceId >= 0) return deviceId;
    PaHostApiIndex wasapi = wasapiHostApi();
    if (wasapi >= 0) {
        const PaHostApiInfo* info = Pa_GetHostApiInfo(wasapi);
        if (info && info->defaultOutputDevice != paNoDevice)
            return info->defaultOutputDevice;
    }
    return Pa_GetDefaultOutputDevice();
}

std::vector<AudioDeviceInfo> AudioEngine::inputDevices() const {
    std::vector<AudioDeviceInfo> result;
    if (!m_initialized) return result;

    PaHostApiIndex wasapi = wasapiHostApi();

    int count = Pa_GetDeviceCount();
    for (int i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (!info || info->maxInputChannels <= 0) continue;
        // On Windows, show only WASAPI devices to avoid duplicate entries
        // (each physical device otherwise appears once per host API).
        // On other platforms wasapi == -1, so the filter is skipped.
        if (wasapi >= 0 && info->hostApi != wasapi) continue;

        AudioDeviceInfo dev;
        dev.id = i;
        dev.name = info->name;
        dev.maxInputChannels = info->maxInputChannels;
        dev.maxOutputChannels = info->maxOutputChannels;
        dev.defaultSampleRate = info->defaultSampleRate;
        result.push_back(dev);
    }
    return result;
}

std::vector<AudioDeviceInfo> AudioEngine::outputDevices() const {
    std::vector<AudioDeviceInfo> result;
    if (!m_initialized) return result;

    PaHostApiIndex wasapi = wasapiHostApi();

    int count = Pa_GetDeviceCount();
    for (int i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (!info || info->maxOutputChannels <= 0) continue;
        if (wasapi >= 0 && info->hostApi != wasapi) continue;

        AudioDeviceInfo dev;
        dev.id = i;
        dev.name = info->name;
        dev.maxInputChannels = info->maxInputChannels;
        dev.maxOutputChannels = info->maxOutputChannels;
        dev.defaultSampleRate = info->defaultSampleRate;
        result.push_back(dev);
    }
    return result;
}

bool AudioEngine::startCapture(int deviceId) {
    if (!m_initialized || m_captureStream) return false;

    PaStreamParameters inputParams;
    inputParams.device = resolveInputDevice(deviceId);
    if (inputParams.device == paNoDevice) return false;

    inputParams.channelCount = Channels;
    inputParams.sampleFormat = paFloat32;
    inputParams.suggestedLatency = Pa_GetDeviceInfo(inputParams.device)->defaultHighInputLatency;
    inputParams.hostApiSpecificStreamInfo = nullptr;

    m_captureBuffer->reset();

    PaError err = Pa_OpenStream(
        &m_captureStream,
        &inputParams,
        nullptr, // no output
        SampleRate,
        FramesPerBuffer,
        paClipOff,
        captureCallback,
        this
    );
    if (err != paNoError) {
        m_lastError = std::string("Capture open failed (device ") +
                      std::to_string(inputParams.device) + "): " +
                      Pa_GetErrorText(err);
        m_captureStream = nullptr;
        return false;
    }

    err = Pa_StartStream(m_captureStream);
    if (err != paNoError) {
        m_lastError = std::string("Capture start failed: ") + Pa_GetErrorText(err);
        Pa_CloseStream(m_captureStream);
        m_captureStream = nullptr;
        return false;
    }

    m_captureDeviceIndex = inputParams.device;
    return true;
}

void AudioEngine::stopCapture() {
    if (m_captureStream) {
        Pa_StopStream(m_captureStream);
        Pa_CloseStream(m_captureStream);
        m_captureStream = nullptr;
    }
}

bool AudioEngine::startPlayback(int deviceId) {
    if (!m_initialized || m_playbackStream) return false;

    PaStreamParameters outputParams;
    outputParams.device = resolveOutputDevice(deviceId);
    if (outputParams.device == paNoDevice) return false;

    outputParams.channelCount = PlaybackChannels;
    outputParams.sampleFormat = paFloat32;
    outputParams.suggestedLatency = Pa_GetDeviceInfo(outputParams.device)->defaultHighOutputLatency;
    outputParams.hostApiSpecificStreamInfo = nullptr;

    m_playbackBuffer->reset();

    PaError err = Pa_OpenStream(
        &m_playbackStream, nullptr, &outputParams, SampleRate, FramesPerBuffer,
        paClipOff, playbackCallback, this);
    m_outputChannels = PlaybackChannels;

    if (err != paNoError) {
        // Some devices only accept mono — fall back and downmix in the callback
        outputParams.channelCount = 1;
        m_playbackStream = nullptr;
        err = Pa_OpenStream(
            &m_playbackStream, nullptr, &outputParams, SampleRate, FramesPerBuffer,
            paClipOff, playbackCallback, this);
        m_outputChannels = 1;
    }
    if (err != paNoError) {
        m_lastError = std::string("Playback open failed (device ") +
                      std::to_string(outputParams.device) + "): " +
                      Pa_GetErrorText(err);
        m_playbackStream = nullptr;
        return false;
    }

    err = Pa_StartStream(m_playbackStream);
    if (err != paNoError) {
        m_lastError = std::string("Playback start failed: ") + Pa_GetErrorText(err);
        Pa_CloseStream(m_playbackStream);
        m_playbackStream = nullptr;
        return false;
    }

    m_playbackDeviceIndex = outputParams.device;
    return true;
}

void AudioEngine::stopPlayback() {
    if (m_playbackStream) {
        Pa_StopStream(m_playbackStream);
        Pa_CloseStream(m_playbackStream);
        m_playbackStream = nullptr;
    }
}

size_t AudioEngine::readCapture(float* buffer, size_t frames) {
    return m_captureBuffer->read(buffer, frames);
}

size_t AudioEngine::writePlayback(const float* stereo, size_t frames) {
    // Only ever write whole stereo frames so L/R can never swap
    size_t maxFrames = m_playbackBuffer->availableWrite() / PlaybackChannels;
    if (frames > maxFrames) frames = maxFrames;
    if (frames == 0) return 0;
    return m_playbackBuffer->write(stereo, frames * PlaybackChannels) / PlaybackChannels;
}

std::string AudioEngine::captureDeviceName() const {
    if (m_captureDeviceIndex == paNoDevice) return {};
    const PaDeviceInfo* info = Pa_GetDeviceInfo(m_captureDeviceIndex);
    return info ? info->name : "unknown";
}

std::string AudioEngine::playbackDeviceName() const {
    if (m_playbackDeviceIndex == paNoDevice) return {};
    const PaDeviceInfo* info = Pa_GetDeviceInfo(m_playbackDeviceIndex);
    return info ? info->name : "unknown";
}

// --- Audio callbacks (real-time safe — no alloc, no mutex, no IO) ---

int AudioEngine::captureCallback(const void* input, void* /*output*/,
                                  unsigned long frameCount,
                                  const PaStreamCallbackTimeInfo* /*timeInfo*/,
                                  PaStreamCallbackFlags /*statusFlags*/,
                                  void* userData) {
    auto* engine = static_cast<AudioEngine*>(userData);
    const auto* samples = static_cast<const float*>(input);

    if (!samples) return paContinue;

    // Apply input volume and compute RMS
    float volume = engine->m_inputVolume.load(std::memory_order_relaxed);
    float sumSquares = 0.0f;

    // Use stack buffer to avoid allocation
    float adjusted[960]; // FramesPerBuffer
    size_t count = (frameCount > 960) ? 960 : frameCount;

    for (size_t i = 0; i < count; ++i) {
        adjusted[i] = samples[i] * volume;
        sumSquares += adjusted[i] * adjusted[i];
    }

    float rms = std::sqrt(sumSquares / static_cast<float>(count));
    engine->m_inputLevel.store(rms, std::memory_order_relaxed);

    engine->m_captureBuffer->write(adjusted, count);
    return paContinue;
}

int AudioEngine::playbackCallback(const void* /*input*/, void* output,
                                   unsigned long frameCount,
                                   const PaStreamCallbackTimeInfo* /*timeInfo*/,
                                   PaStreamCallbackFlags /*statusFlags*/,
                                   void* userData) {
    auto* engine = static_cast<AudioEngine*>(userData);
    auto* out = static_cast<float*>(output);
    const int outCh = engine->m_outputChannels;
    const float volume = engine->m_outputVolume.load(std::memory_order_relaxed);
    float sumSquares = 0.0f;

    if (outCh == PlaybackChannels) {
        // Device is stereo: ring buffer layout matches output directly
        size_t want = frameCount * PlaybackChannels;
        size_t read = engine->m_playbackBuffer->read(out, want);
        if (read < want) std::memset(out + read, 0, (want - read) * sizeof(float));
        for (size_t i = 0; i < want; ++i) {
            out[i] *= volume;
            sumSquares += out[i] * out[i];
        }
        sumSquares /= PlaybackChannels;
    } else {
        // Mono fallback: downmix stereo pairs in fixed-size chunks (no allocation)
        float chunk[FramesPerBuffer * PlaybackChannels];
        unsigned long done = 0;
        while (done < frameCount) {
            unsigned long n = std::min<unsigned long>(frameCount - done, FramesPerBuffer);
            size_t read = engine->m_playbackBuffer->read(chunk, n * PlaybackChannels);
            if (read < n * PlaybackChannels)
                std::memset(chunk + read, 0, (n * PlaybackChannels - read) * sizeof(float));
            for (unsigned long f = 0; f < n; ++f) {
                float s = 0.5f * (chunk[f * 2] + chunk[f * 2 + 1]) * volume;
                out[done + f] = s;
                sumSquares += s * s;
            }
            done += n;
        }
    }

    float rms = std::sqrt(sumSquares / static_cast<float>(frameCount));
    engine->m_outputLevel.store(rms, std::memory_order_relaxed);

    return paContinue;
}
