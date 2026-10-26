#include "AccessGuard.h"
#include "Logger.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"
#include "mbedtls/build_info.h"
#include "mbedtls/md.h"
#if MBEDTLS_VERSION_MAJOR >= 4
#include "psa/crypto.h"
#endif
#include <cstring>

AccessGuardClass Access;

namespace {

constexpr const char* NVS_NAMESPACE = "pf_access";
constexpr const char* KEY_SALT = "salt";
constexpr const char* KEY_HASH = "hash";
constexpr const char* KEY_SESSION = "session";
constexpr const char* KEY_API = "apikey";

class Lock {
public:
    explicit Lock(SemaphoreHandle_t m) : _m(m) { if (_m) xSemaphoreTake(_m, portMAX_DELAY); }
    ~Lock() { if (_m) xSemaphoreGive(_m); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
private:
    SemaphoreHandle_t _m;
};

bool readBlob(nvs_handle_t h, const char* key, void* out, size_t len) {
    size_t got = len;
    return nvs_get_blob(h, key, out, &got) == ESP_OK && got == len;
}

bool readToken(nvs_handle_t h, const char* key, char* out, size_t len) {
    size_t got = len;
    return nvs_get_str(h, key, out, &got) == ESP_OK && got == len;
}

}  // namespace

void AccessGuardClass::begin() {
    if (_mutex) {
        return;
    }
    _mutex = xSemaphoreCreateMutex();

#if MBEDTLS_VERSION_MAJOR >= 4
    // mbedTLS 4 runs every digest through PSA; a second init is a no-op.
    psa_crypto_init();
#endif

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        _hasPassword = readBlob(h, KEY_SALT, _salt, SALT_BYTES) &&
                       readBlob(h, KEY_HASH, _hash, HASH_BYTES) &&
                       readToken(h, KEY_SESSION, _session, sizeof(_session)) &&
                       readToken(h, KEY_API, _apiKey, sizeof(_apiKey));
        nvs_close(h);
    }
    Logger.info("Access: %s", _hasPassword ? "device password set" : "no device password yet (set in /setup)");
}

bool AccessGuardClass::hasPassword() {
    Lock lock(_mutex);
    return _hasPassword;
}

bool AccessGuardClass::setPassword(const char* password) {
    const size_t len = password ? strlen(password) : 0;
    if (len < MIN_PASSWORD || len > MAX_PASSWORD) {
        return false;
    }

    uint8_t salt[SALT_BYTES];
    uint8_t hash[HASH_BYTES];
    esp_fill_random(salt, sizeof(salt));
    const int64_t startUs = esp_timer_get_time();
    if (!derive(password, salt, hash)) {
        Logger.error("Access: password hashing failed");
        return false;
    }
    const int64_t elapsedMs = (esp_timer_get_time() - startUs) / 1000;

    {
        Lock lock(_mutex);
        memcpy(_salt, salt, SALT_BYTES);
        memcpy(_hash, hash, HASH_BYTES);
        randomHex(_session);
        if (!_hasPassword || _apiKey[0] == '\0') {
            randomHex(_apiKey);
        }
        _hasPassword = true;
        _failures = 0;
        if (!persist()) {
            Logger.error("Access: could not store the device password");
            return false;
        }
    }
    Logger.info("Access: device password set (hashing took %lld ms)", (long long)elapsedMs);
    return true;
}

AccessGuardClass::LoginResult AccessGuardClass::checkPassword(const char* password, uint32_t* retryAfterS) {
    uint8_t salt[SALT_BYTES];
    uint8_t expected[HASH_BYTES];
    {
        Lock lock(_mutex);
        if (!_hasPassword) {
            return LoginResult::Wrong;
        }
        if (_failures >= FREE_ATTEMPTS) {
            const int64_t waitUs = _lastFailureUs + (int64_t)LOCKOUT_S * 1000000 - esp_timer_get_time();
            if (waitUs > 0) {
                if (retryAfterS) *retryAfterS = (uint32_t)(waitUs / 1000000) + 1;
                return LoginResult::LockedOut;
            }
        }
        memcpy(salt, _salt, SALT_BYTES);
        memcpy(expected, _hash, HASH_BYTES);
    }

    uint8_t got[HASH_BYTES];
    const size_t len = password ? strlen(password) : 0;
    const bool ok = len > 0 && len <= MAX_PASSWORD && derive(password, salt, got) &&
                    equalConstantTime(got, expected, HASH_BYTES);

    Lock lock(_mutex);
    if (ok) {
        _failures = 0;
        return LoginResult::Ok;
    }
    _failures++;
    _lastFailureUs = esp_timer_get_time();
    if (_failures == FREE_ATTEMPTS) {
        Logger.warning("Access: %u wrong passwords in a row - one attempt per %u s from now",
                       (unsigned)FREE_ATTEMPTS, (unsigned)LOCKOUT_S);
    }
    return LoginResult::Wrong;
}

void AccessGuardClass::clear() {
    Lock lock(_mutex);
    _hasPassword = false;
    memset(_salt, 0, sizeof(_salt));
    memset(_hash, 0, sizeof(_hash));
    memset(_session, 0, sizeof(_session));
    memset(_apiKey, 0, sizeof(_apiKey));
    _failures = 0;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    Logger.warning("Access: device password cleared");
}

bool AccessGuardClass::matchesSession(const char* token) {
    Lock lock(_mutex);
    return _hasPassword && token && strlen(token) == TOKEN_HEX &&
           equalConstantTime((const uint8_t*)token, (const uint8_t*)_session, TOKEN_HEX);
}

bool AccessGuardClass::matchesApiKey(const char* token) {
    Lock lock(_mutex);
    return _hasPassword && token && strlen(token) == TOKEN_HEX &&
           equalConstantTime((const uint8_t*)token, (const uint8_t*)_apiKey, TOKEN_HEX);
}

void AccessGuardClass::sessionToken(char out[TOKEN_HEX + 1]) {
    Lock lock(_mutex);
    memcpy(out, _session, TOKEN_HEX + 1);
}

void AccessGuardClass::apiKey(char out[TOKEN_HEX + 1]) {
    Lock lock(_mutex);
    memcpy(out, _apiKey, TOKEN_HEX + 1);
}

bool AccessGuardClass::rotateSession() {
    Lock lock(_mutex);
    randomHex(_session);
    return persist();
}

bool AccessGuardClass::regenerateApiKey() {
    Lock lock(_mutex);
    randomHex(_apiKey);
    return persist();
}

// Caller holds the lock.
bool AccessGuardClass::persist() {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    const bool ok = nvs_set_blob(h, KEY_SALT, _salt, SALT_BYTES) == ESP_OK &&
                    nvs_set_blob(h, KEY_HASH, _hash, HASH_BYTES) == ESP_OK &&
                    nvs_set_str(h, KEY_SESSION, _session) == ESP_OK &&
                    nvs_set_str(h, KEY_API, _apiKey) == ESP_OK &&
                    nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

// PBKDF2-HMAC-SHA256, one output block, with HMAC (RFC 2104) built on the plain SHA-256 API:
// mbedTLS 4 no longer offers HMAC through md.h (psa_mac_ only), mbedtls_pkcs5_pbkdf2_hmac_ext
// isn't public there, and the hash calls are the same in 3.6 and 4. The output is standard
// PBKDF2, so a hash stored by any earlier build still verifies.
bool AccessGuardClass::derive(const char* password, const uint8_t* salt, uint8_t out[HASH_BYTES]) {
    constexpr size_t BLOCK_BYTES = 64;
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) {
        return false;
    }

    // inner/outer hold the key-padded states; each HMAC clones them instead of rehashing the key.
    mbedtls_md_context_t inner, outer, work;
    mbedtls_md_init(&inner);
    mbedtls_md_init(&outer);
    mbedtls_md_init(&work);
    uint8_t key[BLOCK_BYTES] = {};
    uint8_t pad[BLOCK_BYTES];
    uint8_t innerHash[HASH_BYTES];
    uint8_t u[HASH_BYTES];

    const size_t passwordLen = strlen(password);
    bool ok = mbedtls_md_setup(&inner, info, 0) == 0 &&
              mbedtls_md_setup(&outer, info, 0) == 0 &&
              mbedtls_md_setup(&work, info, 0) == 0;
    if (ok && passwordLen > BLOCK_BYTES) {
        ok = mbedtls_md(info, (const unsigned char*)password, passwordLen, key) == 0;
    } else if (ok) {
        memcpy(key, password, passwordLen);
    }
    if (ok) {
        for (size_t i = 0; i < BLOCK_BYTES; i++) pad[i] = key[i] ^ 0x36;
        ok = mbedtls_md_starts(&inner) == 0 && mbedtls_md_update(&inner, pad, BLOCK_BYTES) == 0;
    }
    if (ok) {
        for (size_t i = 0; i < BLOCK_BYTES; i++) pad[i] = key[i] ^ 0x5c;
        ok = mbedtls_md_starts(&outer) == 0 && mbedtls_md_update(&outer, pad, BLOCK_BYTES) == 0;
    }

    // HMAC over a then b; result may alias a, which is fully read before result is written.
    auto hmac = [&](const uint8_t* a, size_t aLen, const uint8_t* b, size_t bLen, uint8_t* result) {
        return mbedtls_md_clone(&work, &inner) == 0 &&
               mbedtls_md_update(&work, a, aLen) == 0 &&
               (bLen == 0 || mbedtls_md_update(&work, b, bLen) == 0) &&
               mbedtls_md_finish(&work, innerHash) == 0 &&
               mbedtls_md_clone(&work, &outer) == 0 &&
               mbedtls_md_update(&work, innerHash, HASH_BYTES) == 0 &&
               mbedtls_md_finish(&work, result) == 0;
    };

    const uint8_t blockIndex[4] = {0, 0, 0, 1};
    ok = ok && hmac(salt, SALT_BYTES, blockIndex, sizeof(blockIndex), u);
    if (ok) {
        memcpy(out, u, HASH_BYTES);
        for (uint32_t i = 1; ok && i < PBKDF2_ITERATIONS; i++) {
            ok = hmac(u, HASH_BYTES, nullptr, 0, u);
            for (size_t j = 0; j < HASH_BYTES; j++) {
                out[j] ^= u[j];
            }
        }
    }

    mbedtls_md_free(&inner);
    mbedtls_md_free(&outer);
    mbedtls_md_free(&work);
    memset(key, 0, sizeof(key));
    memset(pad, 0, sizeof(pad));
    memset(innerHash, 0, sizeof(innerHash));
    memset(u, 0, sizeof(u));
    return ok;
}

void AccessGuardClass::randomHex(char out[TOKEN_HEX + 1]) {
    static const char* kHex = "0123456789abcdef";
    uint8_t bytes[TOKEN_BYTES];
    esp_fill_random(bytes, sizeof(bytes));
    for (size_t i = 0; i < TOKEN_BYTES; i++) {
        out[i * 2] = kHex[bytes[i] >> 4];
        out[i * 2 + 1] = kHex[bytes[i] & 0x0F];
    }
    out[TOKEN_HEX] = '\0';
}

bool AccessGuardClass::equalConstantTime(const uint8_t* a, const uint8_t* b, size_t len) {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= a[i] ^ b[i];
    }
    return diff == 0;
}
