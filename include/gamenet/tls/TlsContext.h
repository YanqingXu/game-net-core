// Copyright 2026 Yanqing Xu
// SPDX-License-Identifier: Apache-2.0
#pragma once

// TLS-C1: immutable OpenSSL configuration, shared by owner-local streams.
#include <memory>
#include <string>

namespace gamenet::tls {
class TlsStream;
class TlsContext {
public:
    enum class Role { Client, Server };
    // Empty trustFile selects OpenSSL's platform-configured default trust paths.
    static std::shared_ptr<TlsContext> client(std::string trustFile = {});
    static std::shared_ptr<TlsContext> server(std::string certificateChainFile, std::string privateKeyFile);
    ~TlsContext();
    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;
    Role role() const noexcept;
private:
    struct Impl;
    explicit TlsContext(std::unique_ptr<Impl> impl);
    void* nativeContext() const noexcept;
    std::unique_ptr<Impl> impl_;
    friend class TlsStream;
};
}  // namespace gamenet::tls
