#pragma once

#include <cmath>

/// "Radio" voice effect: telephone/comms band-pass (~300–3400 Hz) plus gentle
/// saturation. Makes a net instantly recognisable by ear (e.g. Command sounds
/// clean, Fleet sounds like a radio). One instance per speaker, so filter state
/// stays continuous across that speaker's 20ms frames.
///
/// Real-time safe: no allocation, no locks.
class RadioFilter {
public:
    RadioFilter() { design(48000.0f); }

    void reset() {
        for (auto& b : m_stages) b.z1 = b.z2 = 0.0f;
    }

    /// Process mono samples in place.
    void process(float* samples, int count) {
        for (int i = 0; i < count; ++i) {
            float x = samples[i];
            for (auto& b : m_stages) x = b.tick(x);
            // Soft clip for a bit of "radio" grit, normalised so small signals keep unity gain
            x = std::tanh(x * kDrive) / kDrive;
            samples[i] = x * kMakeup;
        }
    }

private:
    struct Biquad {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        float z1 = 0, z2 = 0;
        float tick(float x) { // transposed direct form II
            float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    static constexpr float kPi = 3.14159265358979f;
    static constexpr float kDrive = 2.0f;
    static constexpr float kMakeup = 1.4f;   // band-limiting removes energy; bring level back up

    // 2x high-pass @300 Hz + 2x low-pass @3400 Hz → 24 dB/oct skirts each side
    Biquad m_stages[4];

    void design(float fs) {
        setHighPass(m_stages[0], 300.0f, fs);
        setHighPass(m_stages[1], 300.0f, fs);
        setLowPass(m_stages[2], 3400.0f, fs);
        setLowPass(m_stages[3], 3400.0f, fs);
    }

    // RBJ cookbook, Q = 1/sqrt(2) (Butterworth)
    static void setLowPass(Biquad& b, float f0, float fs) {
        float w0 = 2.0f * kPi * f0 / fs, c = std::cos(w0), al = std::sin(w0) / (2.0f * 0.70710678f);
        float a0 = 1.0f + al;
        b.b0 = (1.0f - c) / 2.0f / a0; b.b1 = (1.0f - c) / a0; b.b2 = b.b0;
        b.a1 = -2.0f * c / a0;         b.a2 = (1.0f - al) / a0;
    }
    static void setHighPass(Biquad& b, float f0, float fs) {
        float w0 = 2.0f * kPi * f0 / fs, c = std::cos(w0), al = std::sin(w0) / (2.0f * 0.70710678f);
        float a0 = 1.0f + al;
        b.b0 = (1.0f + c) / 2.0f / a0; b.b1 = -(1.0f + c) / a0; b.b2 = b.b0;
        b.a1 = -2.0f * c / a0;         b.a2 = (1.0f - al) / a0;
    }
};
