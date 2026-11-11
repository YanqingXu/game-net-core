// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "support/TestAssert.h"
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

namespace gamenet::test {
// Ephemeral test-only CA/keys. No repository private key or expiring fixture.
struct TlsCertificates {
    std::filesystem::path directory, ca, certificate, key, expiredCertificate, unrelatedCa, unrelatedKey;
    static void extension(X509* cert, X509* issuer, int nid, const char* value) {
        X509V3_CTX context; X509V3_set_ctx(&context, issuer, cert, nullptr, nullptr, 0);
        auto* ext = X509V3_EXT_conf_nid(nullptr, &context, nid, const_cast<char*>(value));
        GAMENET_TEST_ASSERT(ext && X509_add_ext(cert, ext, -1) == 1); X509_EXTENSION_free(ext);
    }
    using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using Cert = std::unique_ptr<X509, decltype(&X509_free)>;
    static Key makeKey() {
        Key result(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "prime256v1"), EVP_PKEY_free);
        GAMENET_TEST_ASSERT(result); return result;
    }
    static Cert makeCert(EVP_PKEY* key, X509* issuer, EVP_PKEY* signer, long serial, bool authority, bool expired) {
        Cert cert(X509_new(), X509_free); GAMENET_TEST_ASSERT(cert);
        GAMENET_TEST_ASSERT(X509_set_version(cert.get(), 2) == 1);
        ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), serial);
        X509_gmtime_adj(X509_getm_notBefore(cert.get()), expired ? -7200 : -60);
        X509_gmtime_adj(X509_getm_notAfter(cert.get()), expired ? -3600 : 3600);
        GAMENET_TEST_ASSERT(X509_set_pubkey(cert.get(), key) == 1);
        auto* subject = X509_get_subject_name(cert.get());
        GAMENET_TEST_ASSERT(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>(authority ? "GameNet ephemeral CA" : "localhost"), -1, -1, 0) == 1);
        GAMENET_TEST_ASSERT(X509_set_issuer_name(cert.get(), issuer ? X509_get_subject_name(issuer) : subject) == 1);
        extension(cert.get(), issuer ? issuer : cert.get(), NID_basic_constraints,
            authority ? "critical,CA:TRUE" : "critical,CA:FALSE");
        extension(cert.get(), issuer ? issuer : cert.get(), NID_key_usage,
            authority ? "critical,keyCertSign,cRLSign" : "critical,digitalSignature");
        if (!authority) {
            extension(cert.get(), issuer, NID_ext_key_usage, "serverAuth");
            extension(cert.get(), issuer, NID_subject_alt_name, "DNS:localhost,IP:127.0.0.1");
        }
        GAMENET_TEST_ASSERT(X509_sign(cert.get(), signer ? signer : key, EVP_sha256()) > 0);
        return cert;
    }
    static void writeCert(const std::filesystem::path& path, X509* cert) {
        auto* file = BIO_new_file(path.string().c_str(), "w"); GAMENET_TEST_ASSERT(file);
        GAMENET_TEST_ASSERT(PEM_write_bio_X509(file, cert) == 1); BIO_free(file);
    }
    TlsCertificates() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        directory = std::filesystem::temp_directory_path() / ("gamenet-tls-" + std::to_string(nonce));
        GAMENET_TEST_ASSERT(std::filesystem::create_directory(directory));
        ca = directory / "ca.pem"; certificate = directory / "server.pem"; key = directory / "key.pem";
        expiredCertificate = directory / "expired.pem"; unrelatedCa = directory / "other-ca.pem";
        unrelatedKey = directory / "other-key.pem";
        auto caKey = makeKey(); auto serverKey = makeKey(); auto otherKey = makeKey();
        auto root = makeCert(caKey.get(), nullptr, nullptr, 1, true, false);
        auto leaf = makeCert(serverKey.get(), root.get(), caKey.get(), 2, false, false);
        auto expired = makeCert(serverKey.get(), root.get(), caKey.get(), 3, false, true);
        auto other = makeCert(otherKey.get(), nullptr, nullptr, 4, true, false);
        writeCert(ca, root.get()); writeCert(certificate, leaf.get()); writeCert(expiredCertificate, expired.get());
        writeCert(unrelatedCa, other.get());
        auto* file = BIO_new_file(key.string().c_str(), "w"); GAMENET_TEST_ASSERT(file);
        GAMENET_TEST_ASSERT(PEM_write_bio_PrivateKey(file, serverKey.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1);
        BIO_free(file);
        file = BIO_new_file(unrelatedKey.string().c_str(), "w"); GAMENET_TEST_ASSERT(file);
        GAMENET_TEST_ASSERT(PEM_write_bio_PrivateKey(file, otherKey.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1);
        BIO_free(file);
    }
    ~TlsCertificates() {
        // Confine recursive removal to the one freshly created test directory.
        if (directory.parent_path() == std::filesystem::temp_directory_path() &&
            directory.filename().string().starts_with("gamenet-tls-")) {
            std::error_code ignored; std::filesystem::remove_all(directory, ignored);
        }
    }
};
}  // namespace gamenet::test
