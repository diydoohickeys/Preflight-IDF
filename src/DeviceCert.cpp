#include "DeviceCert.h"
#include "Logger.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"
#include "mbedtls/build_info.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/x509_csr.h"
#if MBEDTLS_VERSION_MAJOR >= 4
#include "psa/crypto.h"
#else
#include "mbedtls/ecp.h"
#endif
#include <algorithm>
#include <initializer_list>
#include <cstdio>
#include <cstring>
#include <vector>

namespace wifisetup {

namespace {

constexpr const char* NVS_NAMESPACE = "pf_tls";
constexpr const char* KEY_HOST = "host";
constexpr const char* KEY_CERT = "cert";
constexpr const char* KEY_KEY = "key";
constexpr const char* KEY_CA = "ca";
constexpr const char* KEY_SIGNED = "signed";   // u8: 1 = the certificate came from the user's CA
constexpr const char* KEY_HTTPS = "https";     // u8: 1 = the user turned HTTPS on
// A P-256 certificate with two SANs is ~650 bytes of PEM, its key ~230.
constexpr size_t CERT_PEM_MAX = 1536;
constexpr size_t KEY_PEM_MAX = 512;
constexpr size_t CSR_PEM_MAX = 1024;
constexpr size_t HOST_MAX = 72;

bool readBlob(nvs_handle_t h, const char* key, std::string& out) {
    size_t len = 0;
    if (nvs_get_blob(h, key, nullptr, &len) != ESP_OK || len < 2) {
        return false;
    }
    out.resize(len);
    if (nvs_get_blob(h, key, &out[0], &len) != ESP_OK) {
        return false;
    }
    out.resize(len - 1);  // stored with its NUL; the string keeps it implicitly
    return true;
}

bool readHost(nvs_handle_t h, std::string& out) {
    char host[HOST_MAX] = {};
    size_t len = sizeof(host);
    if (nvs_get_str(h, KEY_HOST, host, &len) != ESP_OK) {
        return false;
    }
    out = host;
    return true;
}

bool isSigned(nvs_handle_t h) {
    uint8_t v = 0;
    return nvs_get_u8(h, KEY_SIGNED, &v) == ESP_OK && v == 1;
}

// A signed certificate is kept even after a hostname change (the user re-signs; a self-signed
// replacement would silently throw away their trusted one), with a warning saying so.
bool loadStored(const std::string& hostname, std::string& certPem, std::string& keyPem) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    std::string host;
    const bool haveHost = readHost(h, host);
    const bool signedByCa = isSigned(h);
    const bool ok = haveHost && (signedByCa || hostname == host) &&
                    readBlob(h, KEY_CERT, certPem) && readBlob(h, KEY_KEY, keyPem);
    nvs_close(h);
    if (ok && signedByCa && hostname != host) {
        Logger.warning("HTTPS: the signed certificate is for %s, the device is now %s - re-run "
                       "preflight-certs.ps1 -Device", host.c_str(), hostname.c_str());
    }
    return ok;
}

bool store(const std::string& hostname, const std::string& certPem, const std::string& keyPem) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    const bool ok = nvs_set_str(h, KEY_HOST, hostname.c_str()) == ESP_OK &&
                    nvs_set_blob(h, KEY_CERT, certPem.c_str(), certPem.size() + 1) == ESP_OK &&
                    nvs_set_blob(h, KEY_KEY, keyPem.c_str(), keyPem.size() + 1) == ESP_OK &&
                    nvs_set_u8(h, KEY_SIGNED, 0) == ESP_OK &&
                    nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

#if MBEDTLS_VERSION_MAJOR < 4
int fillRandom(void*, unsigned char* out, size_t len) {
    esp_fill_random(out, len);
    return 0;
}
#endif

bool parseKey(mbedtls_pk_context* pk, const std::string& keyPem) {
    const auto* data = reinterpret_cast<const unsigned char*>(keyPem.c_str());
#if MBEDTLS_VERSION_MAJOR >= 4
    psa_crypto_init();
    return mbedtls_pk_parse_key(pk, data, keyPem.size() + 1, nullptr, 0) == 0;
#else
    return mbedtls_pk_parse_key(pk, data, keyPem.size() + 1, nullptr, 0, fillRandom, nullptr) == 0;
#endif
}

bool keysMatch(const mbedtls_pk_context* pub, const mbedtls_pk_context* prv) {
#if MBEDTLS_VERSION_MAJOR >= 4
    return mbedtls_pk_check_pair(pub, prv) == 0;
#else
    return mbedtls_pk_check_pair(pub, prv, fillRandom, nullptr) == 0;
#endif
}

#if defined(MBEDTLS_X509_CRT_WRITE_C) && defined(MBEDTLS_PEM_WRITE_C) && defined(MBEDTLS_PK_WRITE_C)

bool generateKey(mbedtls_pk_context* pk) {
#if MBEDTLS_VERSION_MAJOR >= 4
    // mbedTLS 4 generates keys through PSA only; the pk context takes a copy.
    psa_crypto_init();
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 256);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    mbedtls_svc_key_id_t id;
    if (psa_generate_key(&attributes, &id) != PSA_SUCCESS) {
        return false;
    }
    const bool ok = mbedtls_pk_copy_from_psa(id, pk) == 0;
    psa_destroy_key(id);
    return ok;
#else
    return mbedtls_pk_setup(pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0 &&
           mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(*pk), fillRandom, nullptr) == 0;
#endif
}

bool generate(const std::string& hostname, std::string& certPem, std::string& keyPem) {
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    mbedtls_x509write_cert crt;
    mbedtls_x509write_crt_init(&crt);

    std::string local = hostname + ".local";
    const std::string name = "CN=" + local;

    mbedtls_x509_san_list sanLocal = {};
    sanLocal.node.type = MBEDTLS_X509_SAN_DNS_NAME;
    sanLocal.node.san.unstructured_name.p = reinterpret_cast<unsigned char*>(&local[0]);
    sanLocal.node.san.unstructured_name.len = local.size();
    std::string bare = hostname;
    mbedtls_x509_san_list sanBare = {};
    sanBare.node.type = MBEDTLS_X509_SAN_DNS_NAME;
    sanBare.node.san.unstructured_name.p = reinterpret_cast<unsigned char*>(&bare[0]);
    sanBare.node.san.unstructured_name.len = bare.size();
    sanLocal.next = &sanBare;

    // Positive and non-zero: the top bit of the first byte is cleared, the last is forced on.
    unsigned char serial[16];
    esp_fill_random(serial, sizeof(serial));
    serial[0] &= 0x7F;
    serial[sizeof(serial) - 1] |= 0x01;

    std::vector<unsigned char> certBuf(CERT_PEM_MAX);
    std::vector<unsigned char> keyBuf(KEY_PEM_MAX);

    bool ok = generateKey(&pk);
    if (ok) {
        mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key(&crt, &pk);
        mbedtls_x509write_crt_set_issuer_key(&crt, &pk);
        ok = mbedtls_x509write_crt_set_subject_name(&crt, name.c_str()) == 0 &&
             mbedtls_x509write_crt_set_issuer_name(&crt, name.c_str()) == 0 &&
             mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial)) == 0 &&
             // A device has no reliable clock at first boot, so the window brackets any plausible date.
             mbedtls_x509write_crt_set_validity(&crt, "20240101000000", "20491231235959") == 0 &&
             mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1) == 0 &&
             mbedtls_x509write_crt_set_subject_alternative_name(&crt, &sanLocal) == 0;
    }
#if MBEDTLS_VERSION_MAJOR >= 4
    ok = ok && mbedtls_x509write_crt_pem(&crt, certBuf.data(), certBuf.size()) == 0;
#else
    ok = ok && mbedtls_x509write_crt_pem(&crt, certBuf.data(), certBuf.size(), fillRandom, nullptr) == 0;
#endif
    ok = ok && mbedtls_pk_write_key_pem(&pk, keyBuf.data(), keyBuf.size()) == 0;

    if (ok) {
        certPem.assign(reinterpret_cast<const char*>(certBuf.data()));
        keyPem.assign(reinterpret_cast<const char*>(keyBuf.data()));
    }
    std::fill(keyBuf.begin(), keyBuf.end(), 0);
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&pk);
    return ok;
}

#else

bool generate(const std::string&, std::string&, std::string&) {
    Logger.error("HTTPS: certificate writing is compiled out of mbedTLS - enable "
                 "CONFIG_MBEDTLS_X509_CREATE_C (and X509_CRT_WRITE_C, PEM_WRITE_C, PK_WRITE_C)");
    return false;
}

#endif

}  // namespace

bool loadOrCreateDeviceCert(const std::string& hostname, std::string& certPem, std::string& keyPem) {
    if (loadStored(hostname, certPem, keyPem)) {
        return true;
    }
    const int64_t startUs = esp_timer_get_time();
    if (!generate(hostname, certPem, keyPem)) {
        Logger.error("HTTPS: could not create the device certificate");
        return false;
    }
    if (!store(hostname, certPem, keyPem)) {
        Logger.warning("HTTPS: certificate made but not stored - a new one is made next boot");
    }
    Logger.info("HTTPS: new self-signed certificate for %s.local (%lld ms)", hostname.c_str(),
                (long long)((esp_timer_get_time() - startUs) / 1000));
    return true;
}

void clearDeviceCert() {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

void revertToSelfSigned() {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        for (const char* key : {KEY_HOST, KEY_CERT, KEY_KEY, KEY_CA, KEY_SIGNED}) {
            nvs_erase_key(h, key);
        }
        nvs_commit(h);
        nvs_close(h);
    }
}

bool httpsChosen() {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t v = 0;
    const bool on = nvs_get_u8(h, KEY_HTTPS, &v) == ESP_OK && v == 1;
    nvs_close(h);
    return on;
}

bool setHttpsChosen(bool on) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    const bool ok = nvs_set_u8(h, KEY_HTTPS, on ? 1 : 0) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool deviceCertInfo(DeviceCertInfo& out) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    std::string certPem;
    const bool have = readBlob(h, KEY_CERT, certPem);
    out.signedByCa = isSigned(h);
    nvs_close(h);
    if (!have) {
        return false;
    }

    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    const bool ok = mbedtls_x509_crt_parse(&crt, reinterpret_cast<const unsigned char*>(certPem.c_str()),
                                           certPem.size() + 1) == 0;
    if (ok) {
        char buf[160];
        if (mbedtls_x509_dn_gets(buf, sizeof(buf), &crt.subject) > 0) out.subject = buf;
        if (mbedtls_x509_dn_gets(buf, sizeof(buf), &crt.issuer) > 0) out.issuer = buf;
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d", crt.valid_to.year, crt.valid_to.mon, crt.valid_to.day);
        out.validTo = buf;
    }
    mbedtls_x509_crt_free(&crt);
    return ok;
}

bool makeDeviceCsr(std::string& csrPem, std::string& hostname, std::string& error) {
#if defined(MBEDTLS_X509_CSR_WRITE_C) && defined(MBEDTLS_PEM_WRITE_C)
    std::string keyPem;
    {
        nvs_handle_t h;
        if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
            error = "No device key yet - HTTPS has not started";
            return false;
        }
        const bool ok = readBlob(h, KEY_KEY, keyPem) && readHost(h, hostname);
        nvs_close(h);
        if (!ok) {
            error = "No device key yet - HTTPS has not started";
            return false;
        }
    }

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    mbedtls_x509write_csr csr;
    mbedtls_x509write_csr_init(&csr);
    std::vector<unsigned char> buf(CSR_PEM_MAX);
    const std::string name = "CN=" + hostname + ".local";

    bool ok = parseKey(&pk, keyPem);
    std::fill(keyPem.begin(), keyPem.end(), '\0');
    if (ok) {
        mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
        mbedtls_x509write_csr_set_key(&csr, &pk);
        ok = mbedtls_x509write_csr_set_subject_name(&csr, name.c_str()) == 0;
    }
#if MBEDTLS_VERSION_MAJOR >= 4
    ok = ok && mbedtls_x509write_csr_pem(&csr, buf.data(), buf.size()) == 0;
#else
    ok = ok && mbedtls_x509write_csr_pem(&csr, buf.data(), buf.size(), fillRandom, nullptr) == 0;
#endif
    if (ok) {
        csrPem.assign(reinterpret_cast<const char*>(buf.data()));
    } else {
        error = "Could not build the signing request";
    }
    mbedtls_x509write_csr_free(&csr);
    mbedtls_pk_free(&pk);
    return ok;
#else
    (void)csrPem;
    (void)hostname;
    error = "CSR writing is compiled out of mbedTLS (CONFIG_MBEDTLS_X509_CSR_WRITE_C)";
    return false;
#endif
}

bool installSignedDeviceCert(const std::string& certPem, const std::string& caPem, std::string& error) {
    std::string keyPem;
    std::string host;
    {
        nvs_handle_t h;
        if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
            error = "No device key yet - HTTPS has not started";
            return false;
        }
        const bool ok = readBlob(h, KEY_KEY, keyPem) && readHost(h, host);
        nvs_close(h);
        if (!ok) {
            error = "No device key yet - HTTPS has not started";
            return false;
        }
    }

    mbedtls_x509_crt crt;
    mbedtls_x509_crt ca;
    mbedtls_pk_context pk;
    mbedtls_x509_crt_init(&crt);
    mbedtls_x509_crt_init(&ca);
    mbedtls_pk_init(&pk);

    bool ok = true;
    if (mbedtls_x509_crt_parse(&crt, reinterpret_cast<const unsigned char*>(certPem.c_str()),
                               certPem.size() + 1) != 0) {
        error = "The certificate is not valid PEM";
        ok = false;
    } else if (mbedtls_x509_crt_parse(&ca, reinterpret_cast<const unsigned char*>(caPem.c_str()),
                                      caPem.size() + 1) != 0) {
        error = "The CA certificate is not valid PEM";
        ok = false;
    } else if (!parseKey(&pk, keyPem)) {
        error = "The device key could not be read";
        ok = false;
    } else if (!keysMatch(&crt.pk, &pk)) {
        error = "The certificate is not for this device's key - sign a fresh CSR";
        ok = false;
    } else {
        uint32_t flags = 0;
        mbedtls_x509_crt_verify(&crt, &ca, nullptr, nullptr, &flags, nullptr, nullptr);
        // A device may have no clock yet; the browser checks the dates.
        flags &= ~(uint32_t)(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE);
        if (flags != 0) {
            error = "The certificate is not signed by the given CA";
            ok = false;
        }
    }
    std::fill(keyPem.begin(), keyPem.end(), '\0');
    mbedtls_pk_free(&pk);
    mbedtls_x509_crt_free(&ca);
    mbedtls_x509_crt_free(&crt);
    if (!ok) {
        return false;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        error = "Could not open storage";
        return false;
    }
    ok = nvs_set_blob(h, KEY_CERT, certPem.c_str(), certPem.size() + 1) == ESP_OK &&
         nvs_set_blob(h, KEY_CA, caPem.c_str(), caPem.size() + 1) == ESP_OK &&
         nvs_set_u8(h, KEY_SIGNED, 1) == ESP_OK &&
         nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (!ok) {
        error = "Could not store the certificate";
        return false;
    }
    Logger.info("HTTPS: CA-signed certificate installed for %s.local", host.c_str());
    return true;
}

bool loadTrustedCa(std::string& caPem) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    const bool ok = isSigned(h) && readBlob(h, KEY_CA, caPem);
    nvs_close(h);
    return ok;
}

}  // namespace wifisetup
