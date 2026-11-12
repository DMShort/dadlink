// Cross-checks the real client crypto (client/src/crypto) against the simulator.
//   interop derive <python_pub_b64>                      -> "<our_pub_b64> <key_hex> <salt_hex> <ct_hex>"
//   interop decrypt <key_hex> <salt_hex> <ct_hex> <seq> <src> -> plaintext or "FAIL"
#include "crypto/key_exchange.h"
#include "crypto/srtp_session.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static std::string hex(const uint8_t* d, size_t n) {
    static const char* h = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s += h[d[i] >> 4]; s += h[d[i] & 15]; }
    return s;
}
static std::vector<uint8_t> unhex(const std::string& s) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < s.size(); i += 2) out.push_back(uint8_t(std::stoi(s.substr(i, 2), nullptr, 16)));
    return out;
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "derive") {
        KeyExchange ke;
        if (!ke.generateKeypair() || !ke.computeSharedSecret(std::string(argv[2]))) { puts("FAIL"); return 1; }
        const DerivedKeys& k = ke.derivedKeys();
        SrtpSession s;
        s.init(k);
        const std::string msg = "dadlink-interop";
        auto ct = s.encrypt(reinterpret_cast<const uint8_t*>(msg.data()), msg.size(), 777, 42);
        printf("%s %s %s %s\n", ke.publicKeyBase64().c_str(), hex(k.masterKey.data(), 16).c_str(),
               hex(k.masterSalt.data(), 14).c_str(), hex(ct.data(), ct.size()).c_str());
        return 0;
    }
    if (argc >= 7 && std::string(argv[1]) == "decrypt") {
        DerivedKeys k{};
        auto key = unhex(argv[2]), salt = unhex(argv[3]), ct = unhex(argv[4]);
        std::copy(key.begin(), key.end(), k.masterKey.begin());
        std::copy(salt.begin(), salt.end(), k.masterSalt.begin());
        SrtpSession s;
        s.init(k);
        auto pt = s.decrypt(ct.data(), ct.size(), std::strtoull(argv[5], nullptr, 10), uint32_t(std::strtoul(argv[6], nullptr, 10)));
        if (pt.empty()) { puts("FAIL"); return 1; }
        printf("%.*s\n", int(pt.size()), reinterpret_cast<const char*>(pt.data()));
        return 0;
    }
    return 2;
}
