#include "srtp_session.h"

#include <openssl/evp.h>
#include <QDebug>
#include <cstring>

static constexpr int GCM_TAG_SIZE = 16;

SrtpSession::SrtpSession() = default;
SrtpSession::~SrtpSession() = default;

bool SrtpSession::init(const DerivedKeys& keys) {
    if (m_ready) {
        // Key rotation: move current to previous
        m_previous = m_current;
        m_rotationTime = std::chrono::steady_clock::now();
        qDebug() << "[SRTP] Key rotated — previous keys saved for dual-key window";
    }

    m_current.key = keys.masterKey;
    m_current.salt = keys.masterSalt;
    m_ready = true;
    return true;
}

std::vector<uint8_t> SrtpSession::encrypt(const uint8_t* plaintext, size_t len, uint64_t sequence, uint32_t sourceId) {
    if (!m_ready) return {};
    return encryptWith(plaintext, len, sequence, sourceId, m_current);
}

std::vector<uint8_t> SrtpSession::decrypt(const uint8_t* ciphertext, size_t len, uint64_t sequence, uint32_t sourceId) {
    if (!m_ready) return {};

    // Try current key first
    auto result = decryptWith(ciphertext, len, sequence, sourceId, m_current);
    if (!result.empty()) return result;

    // Try previous key if within dual-key window
    if (m_previous.has_value()) {
        auto elapsed = std::chrono::steady_clock::now() - m_rotationTime;
        if (elapsed < DualKeyWindow) {
            result = decryptWith(ciphertext, len, sequence, sourceId, *m_previous);
            if (!result.empty()) {
                qDebug() << "[SRTP] Decrypted with previous key (dual-key window)";
                return result;
            }
        } else {
            // Window expired, discard previous keys
            m_previous.reset();
        }
    }

    return {};
}

std::vector<uint8_t> SrtpSession::encryptWith(const uint8_t* plaintext, size_t len,
                                               uint64_t sequence, uint32_t sourceId, const KeyMaterial& km) {
    auto nonce = deriveNonce(sequence, sourceId, km);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return {};

    std::vector<uint8_t> ciphertext(len + GCM_TAG_SIZE);
    int outLen = 0;
    int totalLen = 0;

    bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr, nullptr, nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1
        && EVP_EncryptInit_ex(ctx, nullptr, nullptr, km.key.data(), nonce.data()) == 1
        && EVP_EncryptUpdate(ctx, ciphertext.data(), &outLen, plaintext, static_cast<int>(len)) == 1;

    if (ok) {
        totalLen = outLen;
        ok = EVP_EncryptFinal_ex(ctx, ciphertext.data() + totalLen, &outLen) == 1;
        totalLen += outLen;
    }

    if (ok) {
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, GCM_TAG_SIZE,
                                  ciphertext.data() + totalLen) == 1;
        totalLen += GCM_TAG_SIZE;
    }

    EVP_CIPHER_CTX_free(ctx);

    if (!ok) return {};
    ciphertext.resize(static_cast<size_t>(totalLen));
    return ciphertext;
}

std::vector<uint8_t> SrtpSession::decryptWith(const uint8_t* ciphertext, size_t len,
                                               uint64_t sequence, uint32_t sourceId, const KeyMaterial& km) {
    if (len < static_cast<size_t>(GCM_TAG_SIZE)) return {};

    auto nonce = deriveNonce(sequence, sourceId, km);

    size_t dataLen = len - GCM_TAG_SIZE;
    const uint8_t* tag = ciphertext + dataLen;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return {};

    std::vector<uint8_t> plaintext(dataLen);
    int outLen = 0;
    int totalLen = 0;

    bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr, nullptr, nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1
        && EVP_DecryptInit_ex(ctx, nullptr, nullptr, km.key.data(), nonce.data()) == 1
        && EVP_DecryptUpdate(ctx, plaintext.data(), &outLen, ciphertext, static_cast<int>(dataLen)) == 1;

    if (ok) {
        totalLen = outLen;
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, GCM_TAG_SIZE,
                                  const_cast<uint8_t*>(tag)) == 1;
    }

    if (ok) {
        ok = EVP_DecryptFinal_ex(ctx, plaintext.data() + totalLen, &outLen) == 1;
        totalLen += outLen;
    }

    EVP_CIPHER_CTX_free(ctx);

    if (!ok) return {};
    plaintext.resize(static_cast<size_t>(totalLen));
    return plaintext;
}

std::array<uint8_t, 12> SrtpSession::deriveNonce(uint64_t sequence, uint32_t sourceId, const KeyMaterial& km) const {
    std::array<uint8_t, 12> nonce{};

    // Speaker id in the first 4 bytes (big-endian) — must match server/src/crypto/srtp_session.rs
    nonce[0] = static_cast<uint8_t>((sourceId >> 24) & 0xFF);
    nonce[1] = static_cast<uint8_t>((sourceId >> 16) & 0xFF);
    nonce[2] = static_cast<uint8_t>((sourceId >> 8) & 0xFF);
    nonce[3] = static_cast<uint8_t>(sourceId & 0xFF);

    // Place sequence number in last 8 bytes (big-endian)
    nonce[4]  = static_cast<uint8_t>((sequence >> 56) & 0xFF);
    nonce[5]  = static_cast<uint8_t>((sequence >> 48) & 0xFF);
    nonce[6]  = static_cast<uint8_t>((sequence >> 40) & 0xFF);
    nonce[7]  = static_cast<uint8_t>((sequence >> 32) & 0xFF);
    nonce[8]  = static_cast<uint8_t>((sequence >> 24) & 0xFF);
    nonce[9]  = static_cast<uint8_t>((sequence >> 16) & 0xFF);
    nonce[10] = static_cast<uint8_t>((sequence >> 8) & 0xFF);
    nonce[11] = static_cast<uint8_t>(sequence & 0xFF);

    // XOR with salt (first 12 bytes of the 14-byte salt)
    for (int i = 0; i < 12; ++i) {
        nonce[i] ^= km.salt[i];
    }

    return nonce;
}
