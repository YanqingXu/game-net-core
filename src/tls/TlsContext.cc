// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#include "gamenet/tls/TlsContext.h"
#include <openssl/ssl.h>
#include <stdexcept>
#include <utility>

namespace gamenet::tls {
struct TlsContext::Impl {
    SSL_CTX* context{};
    Role role;
    explicit Impl(Role value) : role(value) {
        context = SSL_CTX_new(TLS_method());
        if (!context) throw std::runtime_error("SSL_CTX_new failed");
        if (SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION) != 1) {
            SSL_CTX_free(context); throw std::runtime_error("TLS minimum version failed");
        }
        SSL_CTX_set_options(context, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION | SSL_OP_NO_TICKET);
        SSL_CTX_set_session_cache_mode(context, SSL_SESS_CACHE_OFF);
        SSL_CTX_set_num_tickets(context, 0);
        SSL_CTX_set_max_cert_list(context, 64 * 1024);
        SSL_CTX_set_verify_depth(context, 8);
        SSL_CTX_set_default_passwd_cb(context, [](char*, int, int, void*) { return 0; });
    }
    ~Impl() { SSL_CTX_free(context); }
};
TlsContext::TlsContext(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
TlsContext::~TlsContext() = default;
TlsContext::Role TlsContext::role() const noexcept { return impl_->role; }
void* TlsContext::nativeContext() const noexcept { return impl_->context; }
std::shared_ptr<TlsContext> TlsContext::client(std::string trustFile) {
    auto impl = std::make_unique<Impl>(Role::Client);
    SSL_CTX_set_verify(impl->context, SSL_VERIFY_PEER, nullptr);
    if (trustFile.find('\0') != std::string::npos ||
        (trustFile.empty() ? SSL_CTX_set_default_verify_paths(impl->context) :
            SSL_CTX_load_verify_locations(impl->context, trustFile.c_str(), nullptr)) != 1)
        throw std::runtime_error("TLS trust store load failed");
    return std::shared_ptr<TlsContext>(new TlsContext(std::move(impl)));
}
std::shared_ptr<TlsContext> TlsContext::server(std::string chain, std::string key) {
    auto impl = std::make_unique<Impl>(Role::Server);
    if (chain.empty() || key.empty() || chain.find('\0') != std::string::npos || key.find('\0') != std::string::npos ||
        SSL_CTX_use_certificate_chain_file(impl->context, chain.c_str()) != 1 ||
        SSL_CTX_use_PrivateKey_file(impl->context, key.c_str(), SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(impl->context) != 1)
        throw std::runtime_error("TLS certificate/private key load or match failed");
    return std::shared_ptr<TlsContext>(new TlsContext(std::move(impl)));
}
}  // namespace gamenet::tls
