#pragma once

#include "key_exchange.h"
#include <vector>
#include <cstdint>
#include <chrono>
#include <optional>

/// Per-user SRTP encryption/decryption session using AES-128-GCM.
/// Mirrors the server's SrtpSession — same nonce derivation, same cipher.
/// Supports dual-key window during key rotation: when new keys are installed,
/// the previous keys are kept for a short window so in-flight packets can
/// still be decrypted.
class SrtpSession {
public:
    SrtpSession();
    ~SrtpSession();

    SrtpSession(const SrtpSession&) = delete;
    SrtpSession& operator=(const SrtpSession&) = delete;

    /// Initialize the session from derived keys.
    /// If already ready, moves current keys to previous (dual-key window).
    /// Returns true on success.
    bool init(const DerivedKeys& keys);

    /// Whether the session is initialized and ready for encryption.
    bool isReady() const { return m_ready; }

    /// Encrypt a plaintext Opus payload (always uses current key).
    /// Returns ciphertext with appended 16-byte auth tag, or empty on failure.
    /// sourceId = the speaker's user id (our own id when sending). It is part of the nonce, so
    /// two speakers using the same sequence number never reuse a key/nonce pair.
    std::vector<uint8_t> encrypt(const uint8_t* plaintext, size_t len, uint64_t sequence, uint32_t sourceId);

    /// Decrypt a ciphertext (with appended auth tag) to recover the Opus payload.
    /// Tries current key first, then previous key if within dual-key window.
    /// Returns plaintext, or empty on authentication failure.
    std::vector<uint8_t> decrypt(const uint8_t* ciphertext, size_t len, uint64_t sequence, uint32_t sourceId);

private:
    struct KeyMaterial {
        std::array<uint8_t, 16> key{};
        std::array<uint8_t, 14> salt{};
    };

    std::array<uint8_t, 12> deriveNonce(uint64_t sequence, uint32_t sourceId, const KeyMaterial& km) const;
    std::vector<uint8_t> encryptWith(const uint8_t* plaintext, size_t len, uint64_t sequence, uint32_t sourceId, const KeyMaterial& km);
    std::vector<uint8_t> decryptWith(const uint8_t* ciphertext, size_t len, uint64_t sequence, uint32_t sourceId, const KeyMaterial& km);

    KeyMaterial m_current;
    std::optional<KeyMaterial> m_previous;
    std::chrono::steady_clock::time_point m_rotationTime;
    bool m_ready = false;

    static constexpr auto DualKeyWindow = std::chrono::seconds(2);
};
