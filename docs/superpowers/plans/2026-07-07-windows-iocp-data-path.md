# Windows IOCP Data Path Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the Windows Reactor / TCP data path from readiness-style socket calls to real IOCP overlapped accept, connect, read, and write operations.

**Architecture:** Keep `IocpPoller` responsible for completion port ownership and completion-to-Channel translation only. Keep Acceptor, Connector, and TcpConnection as lifecycle owners, with Windows-only operation records/helper objects holding `OVERLAPPED` storage and completion metadata on the owner EventLoop thread.

**Tech Stack:** C++23, CMake, WinSock2 IOCP, `AcceptEx`, `ConnectEx`, `WSARecv`, `WSASend`, existing GameNet `EventLoop` / `Poller` / `Channel` / `TcpConnection` contracts.

---

## File Structure

- Create `tests/cmake/test_windows_iocp_data_path_contract.py`: static guard for the new IOCP operation layer and documentation links.
- Modify `.github/workflows/ci.yml`: run the new static guard with the existing repository guards.
- Modify `docs/development/ci.md`: document the new guard.
- Modify `docs/development/windows_iocp_milestone.md`: link this plan and the design spec.
- Modify `docs/migration_status.md`: record that the IOCP data-path design exists and remains deferred until runtime contracts pass.
- Create `include/gamenet/core/net/platform/IocpOperation.h`: Windows-only internal operation record definitions.
- Modify `include/gamenet/core/net/poller/IocpPoller.h`: keep wakeup API and add completion translation support without owning operations.
- Modify `src/core/net/poller/IocpPoller.cc`: recover operation records from `OVERLAPPED`, store completion metadata, and set Channel revents by operation kind.
- Create `include/gamenet/core/net/platform/IocpSocketOps.h`: Windows-only extension function and overlapped socket helpers.
- Create `src/core/net/platform/IocpSocketOps_win.cc`: load `AcceptEx` / `ConnectEx`, create overlapped TCP sockets, update connect/accept contexts.
- Modify `src/core/CMakeLists.txt`: compile `IocpSocketOps_win.cc` only on Windows.
- Modify `src/core/net/platform/SocketsOps_win.cc`: create stream sockets with `WSA_FLAG_OVERLAPPED`.
- Modify `include/gamenet/core/net/Acceptor.h` and `src/core/net/Acceptor.cc`: add Windows accept operation helper state and completion consumption.
- Modify `include/gamenet/core/net/Connector.h` and `src/core/net/Connector.cc`: add Windows connect operation helper state and completion consumption.
- Create `src/core/net/platform/IocpTcpTransport.h`: loop-owned Windows TcpConnection read/write helper.
- Create `src/core/net/platform/IocpTcpTransport_win.cc`: implement `WSARecv` / `WSASend` operation posting and completion consumption.
- Modify `include/gamenet/core/net/TcpConnection.h` and `src/core/net/TcpConnection.cc`: delegate Windows read/write operation details to the helper while preserving public lifecycle callbacks.
- Modify `tests/CMakeLists.txt`: no new runtime tests are required at first; existing contracts become the runtime gate.

## Runtime Contract Targets

The Windows IOCP data path is promoted only after these existing CTest targets
are green on Windows:

- `contract.acceptor.test_acceptor_contract`
- `contract.connector.test_connector_contract`
- `contract.poller.test_poller_contract`
- `contract.tcp_client.test_tcp_client_contract`
- `contract.tcp_server.test_tcp_server_contract`
- `contract.tcp_server.test_tcp_server_stop_active_connections`
- `contract.tcp_connection.test_tcp_connection_lifecycle`
- `contract.tcp_connection.test_tcp_connection_high_water_mark`
- `contract.tcp_connection.test_tcp_connection_peer_close`
- `contract.tcp_connection.test_tcp_connection_peer_reset`
- `integration.tcp.test_tcp_server_client_echo`

---

### Task 1: Add Static IOCP Data-Path Guard

**Files:**
- Create: `tests/cmake/test_windows_iocp_data_path_contract.py`
- Modify: `.github/workflows/ci.yml`
- Modify: `docs/development/ci.md`
- Modify: `docs/development/windows_iocp_milestone.md`
- Modify: `docs/migration_status.md`
- Test: `tests/ci/test_workflow_jobs.py`

Execution note: first create and run this guard as RED. Do not wire it into CI
until Tasks 2 and 3 provide the operation record, socket helpers, and Poller
completion translation required for the guard to pass.

- [ ] **Step 1: Write the failing guard**

Create `tests/cmake/test_windows_iocp_data_path_contract.py`:

```python
from __future__ import annotations

from pathlib import Path


def require(text: str, needle: str, source: Path) -> None:
    assert needle in text, f"missing IOCP data-path fragment in {source}: {needle}"


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    spec = repo_root / "docs" / "superpowers" / "specs" / "2026-07-07-windows-iocp-data-path-design.md"
    plan = repo_root / "docs" / "superpowers" / "plans" / "2026-07-07-windows-iocp-data-path.md"
    milestone = repo_root / "docs" / "development" / "windows_iocp_milestone.md"
    operation = repo_root / "include" / "gamenet" / "core" / "net" / "platform" / "IocpOperation.h"
    socket_ops = repo_root / "include" / "gamenet" / "core" / "net" / "platform" / "IocpSocketOps.h"
    poller_source = repo_root / "src" / "core" / "net" / "poller" / "IocpPoller.cc"
    core_cmake = repo_root / "src" / "core" / "CMakeLists.txt"
    workflow = repo_root / ".github" / "workflows" / "ci.yml"

    require(spec.read_text(encoding="utf-8"), "loop-owned Windows IOCP operation layer", spec)
    require(plan.read_text(encoding="utf-8"), "WSARecv", plan)
    require(milestone.read_text(encoding="utf-8"), "windows-iocp-data-path-design.md", milestone)

    operation_text = operation.read_text(encoding="utf-8")
    require(operation_text, "enum class IocpOperationKind", operation)
    require(operation_text, "OVERLAPPED overlapped", operation)
    require(operation_text, "bytesTransferred", operation)
    require(operation_text, "Channel* channel", operation)

    socket_ops_text = socket_ops.read_text(encoding="utf-8")
    require(socket_ops_text, "loadAcceptEx", socket_ops)
    require(socket_ops_text, "loadConnectEx", socket_ops)
    require(socket_ops_text, "createOverlappedTcpOrDie", socket_ops)

    poller_text = poller_source.read_text(encoding="utf-8")
    require(poller_text, "reinterpret_cast<IocpOperation*>", poller_source)
    require(poller_text, "operation->bytesTransferred", poller_source)
    require(poller_text, "IocpOperationKind::Read", poller_source)
    require(poller_text, "IocpOperationKind::Write", poller_source)

    require(core_cmake.read_text(encoding="utf-8"), "net/platform/IocpSocketOps_win.cc", core_cmake)
    require(workflow.read_text(encoding="utf-8"), "python3 tests/cmake/test_windows_iocp_data_path_contract.py", workflow)


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Run the guard and verify RED**

Run:

```powershell
py -3 tests\cmake\test_windows_iocp_data_path_contract.py
```

Expected: FAIL because `IocpOperation.h` and `IocpSocketOps.h` do not exist.

- [ ] **Step 3: Keep the guard local until it can pass**

Do not add the guard command to `.github/workflows/ci.yml`,
`docs/development/ci.md`, or `tests/ci/test_workflow_jobs.py` yet. Continue to
Tasks 2 and 3 first so the guard can become GREEN before it is part of the CI
gate.

---

### Task 1B: Wire The Passing Guard Into CI

**Files:**
- Modify: `.github/workflows/ci.yml`
- Modify: `docs/development/ci.md`
- Modify: `docs/migration_status.md`
- Modify: `tests/ci/test_workflow_jobs.py`
- Test: `tests/cmake/test_windows_iocp_data_path_contract.py`
- Test: `tests/ci/test_workflow_jobs.py`

- [ ] **Step 1: Verify the guard is already GREEN**

Run:

```powershell
py -3 tests\cmake\test_windows_iocp_data_path_contract.py
```

Expected: PASS.

- [ ] **Step 2: Wire the guard into CI docs and workflow**

Add this command to every `Check repository guards` block in `.github/workflows/ci.yml`:

```yaml
python3 tests/cmake/test_windows_iocp_data_path_contract.py
```

Add the same command to the guard list in `docs/development/ci.md`.

Add these links to `docs/development/windows_iocp_milestone.md`:

```markdown
The detailed data-path design is recorded in
`docs/superpowers/specs/2026-07-07-windows-iocp-data-path-design.md`, with the
TDD implementation plan in
`docs/superpowers/plans/2026-07-07-windows-iocp-data-path.md`.
```

Add this sentence to the Windows bullet in `docs/migration_status.md`:

```markdown
The next IOCP work item is the data-path plan in
`docs/superpowers/plans/2026-07-07-windows-iocp-data-path.md`.
```

- [ ] **Step 3: Verify CI workflow contract fails until the guard is listed**

Run:

```powershell
py -3 tests\ci\test_workflow_jobs.py
```

Expected: FAIL until `tests/ci/test_workflow_jobs.py` is updated to require the new guard command.

- [ ] **Step 4: Update workflow contract**

Add this required command to `tests/ci/test_workflow_jobs.py`:

```python
"python3 tests/cmake/test_windows_iocp_data_path_contract.py",
```

- [ ] **Step 5: Commit the static guard change**

Run:

```powershell
git add tests/cmake/test_windows_iocp_data_path_contract.py .github/workflows/ci.yml docs/development/ci.md docs/development/windows_iocp_milestone.md docs/migration_status.md tests/ci/test_workflow_jobs.py
git commit -m "test: guard windows iocp data path plan"
```

---

### Task 2: Add Operation Records And Completion Translation

**Files:**
- Create: `include/gamenet/core/net/platform/IocpOperation.h`
- Modify: `include/gamenet/core/net/poller/IocpPoller.h`
- Modify: `src/core/net/poller/IocpPoller.cc`
- Test: `tests/cmake/test_windows_iocp_data_path_contract.py`

- [ ] **Step 1: Write the operation record header**

Create `include/gamenet/core/net/platform/IocpOperation.h`:

```cpp
#pragma once

#include "gamenet/core/net/SocketTypes.h"

#ifdef _WIN32

#include <WinSock2.h>
#include <Windows.h>

namespace gamenet::net {

class Channel;

enum class IocpOperationKind {
    Accept,
    Connect,
    Read,
    Write,
};

struct IocpOperation {
    OVERLAPPED overlapped{};
    IocpOperationKind kind;
    Channel* channel{nullptr};
    DWORD bytesTransferred{0};
    DWORD error{0};
};

}  // namespace gamenet::net

#endif
```

- [ ] **Step 2: Run the guard and verify partial RED**

Run:

```powershell
py -3 tests\cmake\test_windows_iocp_data_path_contract.py
```

Expected: FAIL because `IocpSocketOps.h` does not exist and `IocpPoller.cc` does not yet recover `IocpOperation`.

- [ ] **Step 3: Translate IOCP completions through operation records**

Modify `src/core/net/poller/IocpPoller.cc` to include the operation header:

```cpp
#include "gamenet/core/net/platform/IocpOperation.h"
```

Replace the fd-only completion handling with:

```cpp
    auto* operation = reinterpret_cast<IocpOperation*>(overlapped);
    if (operation == nullptr || operation->channel == nullptr) {
        return now;
    }

    operation->bytesTransferred = bytesTransferred;
    operation->error = ok ? 0 : ::GetLastError();

    Channel* channel = operation->channel;
    const auto it = channels_.find(channel->fd());
    if (it == channels_.end() || it->second != channel || channel->index() != kAdded) {
        return now;
    }

    channel->setRevents(completionEvents(*operation));
    activeChannels->push_back(channel);
```

Add the operation-kind event mapper near the existing helpers:

```cpp
uint32_t completionEvents(const IocpOperation& operation) noexcept {
    if (operation.error != 0) {
        return Channel::kErrorEvent | Channel::kCloseEvent;
    }
    switch (operation.kind) {
    case IocpOperationKind::Accept:
    case IocpOperationKind::Read:
        return Channel::kReadEvent;
    case IocpOperationKind::Connect:
    case IocpOperationKind::Write:
        return Channel::kWriteEvent;
    }
    return Channel::kErrorEvent;
}
```

- [ ] **Step 4: Verify operation guard remains RED for socket helpers only**

Run:

```powershell
py -3 tests\cmake\test_windows_iocp_data_path_contract.py
```

Expected: FAIL because `IocpSocketOps.h` is still absent.

- [ ] **Step 5: Commit operation metadata translation**

Run:

```powershell
git add include/gamenet/core/net/platform/IocpOperation.h src/core/net/poller/IocpPoller.cc
git commit -m "feat: translate iocp completions through operations"
```

---

### Task 3: Add Windows IOCP Socket Helpers

**Files:**
- Create: `include/gamenet/core/net/platform/IocpSocketOps.h`
- Create: `src/core/net/platform/IocpSocketOps_win.cc`
- Modify: `src/core/net/platform/SocketsOps_win.cc`
- Modify: `src/core/CMakeLists.txt`
- Test: `tests/cmake/test_windows_iocp_data_path_contract.py`

- [ ] **Step 1: Create the helper API**

Create `include/gamenet/core/net/platform/IocpSocketOps.h`:

```cpp
#pragma once

#include "gamenet/core/net/SocketTypes.h"

#ifdef _WIN32

#include <MSWSock.h>
#include <WinSock2.h>

namespace gamenet::net::platform {

SocketFd createOverlappedTcpOrDie(sa_family_t family);
LPFN_ACCEPTEX loadAcceptEx(SocketFd sockfd);
LPFN_GETACCEPTEXSOCKADDRS loadGetAcceptExSockaddrs(SocketFd sockfd);
LPFN_CONNECTEX loadConnectEx(SocketFd sockfd);
void updateAcceptContextOrDie(SocketFd accepted, SocketFd listener);
void updateConnectContextOrDie(SocketFd connected);
void bindUnspecifiedOrDie(SocketFd sockfd, sa_family_t family);

}  // namespace gamenet::net::platform

#endif
```

- [ ] **Step 2: Create the Windows implementation**

Create `src/core/net/platform/IocpSocketOps_win.cc` with these functions:

```cpp
#include "gamenet/core/net/platform/IocpSocketOps.h"

#ifdef _WIN32

#include "gamenet/core/base/Logger.h"
#include "gamenet/core/net/SocketsOps.h"

#include <cstdlib>

namespace gamenet::net::platform {

namespace {

[[noreturn]] void die(const char* what) {
    LOG_SYSFATAL << what << ": " << sockets::errorMessage(sockets::lastError());
    std::abort();
}

template <typename Fn>
Fn loadExtension(SocketFd sockfd, GUID guid, const char* name) {
    Fn fn = nullptr;
    DWORD bytes = 0;
    const int rc = ::WSAIoctl(
        sockfd,
        SIO_GET_EXTENSION_FUNCTION_POINTER,
        &guid,
        sizeof(guid),
        &fn,
        sizeof(fn),
        &bytes,
        nullptr,
        nullptr);
    if (rc == SOCKET_ERROR || fn == nullptr) {
        die(name);
    }
    return fn;
}

}  // namespace

SocketFd createOverlappedTcpOrDie(sa_family_t family) {
    sockets::ensureInitialized();
    const SocketFd sockfd = ::WSASocketW(family, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (!sockets::isValid(sockfd)) {
        die("WSASocketW");
    }
    return sockfd;
}

LPFN_ACCEPTEX loadAcceptEx(SocketFd sockfd) {
    GUID guid = WSAID_ACCEPTEX;
    return loadExtension<LPFN_ACCEPTEX>(sockfd, guid, "AcceptEx");
}

LPFN_GETACCEPTEXSOCKADDRS loadGetAcceptExSockaddrs(SocketFd sockfd) {
    GUID guid = WSAID_GETACCEPTEXSOCKADDRS;
    return loadExtension<LPFN_GETACCEPTEXSOCKADDRS>(sockfd, guid, "GetAcceptExSockaddrs");
}

LPFN_CONNECTEX loadConnectEx(SocketFd sockfd) {
    GUID guid = WSAID_CONNECTEX;
    return loadExtension<LPFN_CONNECTEX>(sockfd, guid, "ConnectEx");
}

void updateAcceptContextOrDie(SocketFd accepted, SocketFd listener) {
    if (::setsockopt(accepted, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, reinterpret_cast<const char*>(&listener), sizeof(listener)) == SOCKET_ERROR) {
        die("SO_UPDATE_ACCEPT_CONTEXT");
    }
}

void updateConnectContextOrDie(SocketFd connected) {
    if (::setsockopt(connected, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0) == SOCKET_ERROR) {
        die("SO_UPDATE_CONNECT_CONTEXT");
    }
}

void bindUnspecifiedOrDie(SocketFd sockfd, sa_family_t family) {
    sockaddr_storage storage{};
    int len = 0;
    if (family == AF_INET6) {
        auto& addr = *reinterpret_cast<sockaddr_in6*>(&storage);
        addr.sin6_family = AF_INET6;
        len = sizeof(sockaddr_in6);
    } else {
        auto& addr = *reinterpret_cast<sockaddr_in*>(&storage);
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        len = sizeof(sockaddr_in);
    }
    if (::bind(sockfd, reinterpret_cast<sockaddr*>(&storage), len) == SOCKET_ERROR) {
        die("bind unspecified");
    }
}

}  // namespace gamenet::net::platform

#endif
```

- [ ] **Step 3: Compile the helper on Windows**

Modify the Windows section of `src/core/CMakeLists.txt`:

```cmake
list(APPEND GAMENET_CORE_SOURCES
    net/platform/SocketsOps_win.cc
    net/platform/Wakeup_win.cc
    net/platform/IocpSocketOps_win.cc
    net/poller/IocpPoller.cc
)
```

- [ ] **Step 4: Create overlapped stream sockets**

Modify `src/core/net/platform/SocketsOps_win.cc` so `createNonblockingOrDie()` calls `platform::createOverlappedTcpOrDie(family)` and then applies `setNonBlockingOrDie(sockfd)`. Include:

```cpp
#include "gamenet/core/net/platform/IocpSocketOps.h"
```

- [ ] **Step 5: Verify GREEN for static data-path guard**

Run:

```powershell
py -3 tests\cmake\test_windows_iocp_data_path_contract.py
```

Expected: PASS.

- [ ] **Step 6: Verify Windows Debug build**

Run:

```powershell
& 'D:\VS2026\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' -S . -B build-local-vs2026 -G 'Visual Studio 18 2026' -A x64 -DGAMENET_BUILD_TESTING=ON -DGAMENET_ENABLE_TLS=OFF -DGAMENET_ENABLE_EXPERIMENTAL=OFF
& 'D:\VS2026\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build build-local-vs2026 --config Debug --parallel
```

Expected: configure and build exit 0.

- [ ] **Step 7: Commit socket helper**

Run:

```powershell
git add include/gamenet/core/net/platform/IocpSocketOps.h src/core/net/platform/IocpSocketOps_win.cc src/core/net/platform/SocketsOps_win.cc src/core/CMakeLists.txt tests/cmake/test_windows_iocp_data_path_contract.py
git commit -m "feat: add windows iocp socket helpers"
```

---

### Task 4: Green The Windows Acceptor Contract

**Files:**
- Modify: `include/gamenet/core/net/Acceptor.h`
- Modify: `src/core/net/Acceptor.cc`
- Test: `tests/contract/acceptor/test_acceptor_contract.cpp`

- [ ] **Step 1: Verify RED**

Run:

```powershell
& 'D:\VS2026\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir build-local-vs2026 -C Debug -R '^contract\.acceptor\.test_acceptor_contract$' --output-on-failure --timeout 10
```

Expected: FAIL with `timed out waiting for accepted connections`.

- [ ] **Step 2: Add Acceptor Windows state**

Add this private Windows-only state to `include/gamenet/core/net/Acceptor.h`:

```cpp
#ifdef _WIN32
    struct IocpAcceptState;
    std::unique_ptr<IocpAcceptState> iocpAccept_;
#endif
```

Ensure the header includes `<memory>` if it does not already.

- [ ] **Step 3: Define the accept state**

Add this Windows-only state in `src/core/net/Acceptor.cc`:

```cpp
#ifdef _WIN32
#include "gamenet/core/net/platform/IocpOperation.h"
#include "gamenet/core/net/platform/IocpSocketOps.h"

#include <array>
#endif

#ifdef _WIN32
struct Acceptor::IocpAcceptState {
    IocpOperation operation{.kind = IocpOperationKind::Accept};
    SocketFd accepted{kInvalidSocket};
    std::array<char, (sizeof(sockaddr_storage) + 16) * 2> addresses{};
    LPFN_ACCEPTEX acceptEx{nullptr};
    LPFN_GETACCEPTEXSOCKADDRS getAcceptExSockaddrs{nullptr};
    bool pending{false};
};
#endif
```

- [ ] **Step 4: Post and complete AcceptEx**

In `Acceptor::listen()` on Windows, initialize `iocpAccept_`, assign `operation.channel = &acceptChannel_`, and post one `AcceptEx` after `acceptChannel_.enableReading()`.

Use this helper shape in `src/core/net/Acceptor.cc`:

```cpp
#ifdef _WIN32
void Acceptor::postAccept() {
    iocpAccept_->accepted = platform::createOverlappedTcpOrDie(listenAddr_.family());
    DWORD bytes = 0;
    iocpAccept_->pending = true;
    const BOOL ok = iocpAccept_->acceptEx(
        acceptSocket_.fd(),
        iocpAccept_->accepted,
        iocpAccept_->addresses.data(),
        0,
        sizeof(sockaddr_storage) + 16,
        sizeof(sockaddr_storage) + 16,
        &bytes,
        &iocpAccept_->operation.overlapped);
    if (!ok && sockets::lastError() != ERROR_IO_PENDING) {
        iocpAccept_->pending = false;
        LOG_SYSFATAL << "AcceptEx: " << sockets::errorMessage(sockets::lastError());
    }
}
#endif
```

Add a matching private declaration under `_WIN32`.

In `handleRead()` on Windows, consume the completed accept:

```cpp
#ifdef _WIN32
    if (!iocpAccept_ || !iocpAccept_->pending) {
        return;
    }
    iocpAccept_->pending = false;
    if (iocpAccept_->operation.error != 0) {
        sockets::close(iocpAccept_->accepted);
        iocpAccept_->accepted = kInvalidSocket;
        if (listening_) {
            postAccept();
        }
        return;
    }
    platform::updateAcceptContextOrDie(iocpAccept_->accepted, acceptSocket_.fd());
    InetAddress peerAddr(sockets::getPeerAddr(iocpAccept_->accepted));
    const SocketFd connfd = iocpAccept_->accepted;
    iocpAccept_->accepted = kInvalidSocket;
    if (newConnectionCallback_) {
        newConnectionCallback_(connfd, peerAddr);
    } else {
        sockets::close(connfd);
    }
    if (listening_) {
        postAccept();
    }
    return;
#endif
```

- [ ] **Step 5: Verify GREEN for Acceptor**

Run:

```powershell
& 'D:\VS2026\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build build-local-vs2026 --config Debug --target contract_acceptor_test_acceptor_contract --parallel
& 'D:\VS2026\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir build-local-vs2026 -C Debug -R '^contract\.acceptor\.test_acceptor_contract$' --output-on-failure --timeout 10
```

Expected: PASS.

- [ ] **Step 6: Commit Acceptor IOCP path**

Run:

```powershell
git add include/gamenet/core/net/Acceptor.h src/core/net/Acceptor.cc
git commit -m "feat: use acceptex for windows acceptor"
```

---

### Task 5: Green The Windows Connector Contract

**Files:**
- Modify: `include/gamenet/core/net/Connector.h`
- Modify: `src/core/net/Connector.cc`
- Test: `tests/contract/connector/test_connector_contract.cpp`

- [ ] **Step 1: Verify RED**

Run:

```powershell
& 'D:\VS2026\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir build-local-vs2026 -C Debug -R '^contract\.connector\.test_connector_contract$' --output-on-failure --timeout 10
```

Expected: FAIL with `timed out waiting for connector success`.

- [ ] **Step 2: Add Connector Windows connect state**

Add Windows-only state to `include/gamenet/core/net/Connector.h`:

```cpp
#ifdef _WIN32
    struct IocpConnectState;
    std::unique_ptr<IocpConnectState> iocpConnect_;
#endif
```

- [ ] **Step 3: Post ConnectEx instead of readiness connect**

On Windows in `Connector::connect()`, create an overlapped socket, bind it to
an unspecified local address, create the Channel, and post `ConnectEx`.

Use this shape:

```cpp
#ifdef _WIN32
    const SocketFd sockfd = platform::createOverlappedTcpOrDie(serverAddr_.family());
    platform::bindUnspecifiedOrDie(sockfd, serverAddr_.family());
    connecting(sockfd);
    iocpConnect_->operation.channel = channel_.get();
    iocpConnect_->operation.kind = IocpOperationKind::Connect;
    iocpConnect_->connectEx = platform::loadConnectEx(sockfd);
    DWORD bytes = 0;
    iocpConnect_->pending = true;
    const BOOL ok = iocpConnect_->connectEx(
        sockfd,
        serverAddr_.getSockAddr(),
        serverAddr_.getSockAddrLen(),
        nullptr,
        0,
        &bytes,
        &iocpConnect_->operation.overlapped);
    if (!ok && sockets::lastError() != ERROR_IO_PENDING) {
        iocpConnect_->pending = false;
        handleError();
    }
    return;
#endif
```

- [ ] **Step 4: Complete ConnectEx in handleWrite**

At the start of `Connector::handleWrite()` on Windows:

```cpp
#ifdef _WIN32
    if (iocpConnect_ && iocpConnect_->operation.error != 0) {
        handleError();
        return;
    }
    platform::updateConnectContextOrDie(channel_->fd());
#endif
```

Keep the existing `SO_ERROR`, self-connect detection, callback ordering, and ownership transfer.

- [ ] **Step 5: Verify GREEN for Connector**

Run:

```powershell
& 'D:\VS2026\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build build-local-vs2026 --config Debug --target contract_connector_test_connector_contract --parallel
& 'D:\VS2026\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir build-local-vs2026 -C Debug -R '^contract\.connector\.test_connector_contract$' --output-on-failure --timeout 10
```

Expected: PASS.

- [ ] **Step 6: Commit Connector IOCP path**

Run:

```powershell
git add include/gamenet/core/net/Connector.h src/core/net/Connector.cc
git commit -m "feat: use connectex for windows connector"
```

---

### Task 6: Green TcpConnection Read And Peer Close

**Files:**
- Create: `src/core/net/platform/IocpTcpTransport.h`
- Create: `src/core/net/platform/IocpTcpTransport_win.cc`
- Modify: `src/core/CMakeLists.txt`
- Modify: `include/gamenet/core/net/TcpConnection.h`
- Modify: `src/core/net/TcpConnection.cc`
- Test: `tests/contract/tcp_connection/test_tcp_connection_peer_close.cpp`

- [ ] **Step 1: Verify RED**

Run:

```powershell
& 'D:\VS2026\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir build-local-vs2026 -C Debug -R '^contract\.tcp_connection\.test_tcp_connection_peer_close$' --output-on-failure --timeout 10
```

Expected: FAIL with `timed out waiting for peer close teardown`.

- [ ] **Step 2: Create Windows TcpConnection helper API**

Create `src/core/net/platform/IocpTcpTransport.h`:

```cpp
#pragma once

#ifdef _WIN32

#include "gamenet/core/net/Buffer.h"
#include "gamenet/core/net/SocketTypes.h"
#include "gamenet/core/net/platform/IocpOperation.h"

#include <array>
#include <cstddef>

namespace gamenet::net {

class Channel;

class IocpTcpTransport {
public:
    explicit IocpTcpTransport(Channel* channel);

    void startRead();
    ssize_t completeRead(Buffer* input, int* savedErrno);
    bool readPending() const noexcept;

private:
    Channel* channel_;
    IocpOperation readOperation_;
    std::array<char, 65536> readStorage_{};
    WSABUF readBuffer_{};
    bool readPending_{false};
};

}  // namespace gamenet::net

#endif
```

- [ ] **Step 3: Implement WSARecv posting and completion**

Create `src/core/net/platform/IocpTcpTransport_win.cc`:

```cpp
#include "gamenet/core/net/platform/IocpTcpTransport.h"

#ifdef _WIN32

#include "gamenet/core/net/Channel.h"
#include "gamenet/core/net/SocketsOps.h"

namespace gamenet::net {

IocpTcpTransport::IocpTcpTransport(Channel* channel)
    : channel_(channel),
      readOperation_{.kind = IocpOperationKind::Read, .channel = channel} {}

void IocpTcpTransport::startRead() {
    if (readPending_) {
        return;
    }
    readOperation_.overlapped = OVERLAPPED{};
    readOperation_.bytesTransferred = 0;
    readOperation_.error = 0;
    readBuffer_.buf = readStorage_.data();
    readBuffer_.len = static_cast<ULONG>(readStorage_.size());
    DWORD flags = 0;
    DWORD bytes = 0;
    readPending_ = true;
    const int rc = ::WSARecv(channel_->fd(), &readBuffer_, 1, &bytes, &flags, &readOperation_.overlapped, nullptr);
    if (rc == SOCKET_ERROR && sockets::lastError() != WSA_IO_PENDING) {
        readPending_ = false;
        readOperation_.error = sockets::lastError();
        channel_->setRevents(Channel::kErrorEvent | Channel::kCloseEvent);
    }
}

ssize_t IocpTcpTransport::completeRead(Buffer* input, int* savedErrno) {
    readPending_ = false;
    if (readOperation_.error != 0) {
        *savedErrno = static_cast<int>(readOperation_.error);
        return -1;
    }
    const std::size_t n = static_cast<std::size_t>(readOperation_.bytesTransferred);
    if (n > 0) {
        input->append(readStorage_.data(), n);
    }
    return static_cast<ssize_t>(n);
}

bool IocpTcpTransport::readPending() const noexcept {
    return readPending_;
}

}  // namespace gamenet::net

#endif
```

- [ ] **Step 4: Wire helper into TcpConnection read path**

Add this private member to `TcpConnection.h`:

```cpp
#ifdef _WIN32
    std::unique_ptr<IocpTcpTransport> iocpTransport_;
#endif
```

Forward declare `class IocpTcpTransport;` under `_WIN32`.

In `TcpConnection` constructor on Windows:

```cpp
#ifdef _WIN32
    iocpTransport_ = std::make_unique<IocpTcpTransport>(channel_.get());
#endif
```

After `channel_->enableReading()` in `connectEstablished()`:

```cpp
#ifdef _WIN32
    iocpTransport_->startRead();
#endif
```

In `handleRead()`, use this Windows branch:

```cpp
#ifdef _WIN32
    const ssize_t n = iocpTransport_->completeRead(&inputBuffer_, &savedErrno);
#else
    const ssize_t n = inputBuffer_.readFd(channel_->fd(), &savedErrno);
#endif
```

After a successful message callback while still connected on Windows:

```cpp
#ifdef _WIN32
        if (state_ == kConnected) {
            iocpTransport_->startRead();
        }
#endif
```

- [ ] **Step 5: Compile helper on Windows**

Add this source in the Windows section of `src/core/CMakeLists.txt`:

```cmake
net/platform/IocpTcpTransport_win.cc
```

- [ ] **Step 6: Verify GREEN for peer close**

Run:

```powershell
& 'D:\VS2026\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build build-local-vs2026 --config Debug --target contract_tcp_connection_test_tcp_connection_peer_close --parallel
& 'D:\VS2026\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir build-local-vs2026 -C Debug -R '^contract\.tcp_connection\.test_tcp_connection_peer_close$' --output-on-failure --timeout 10
```

Expected: PASS.

- [ ] **Step 7: Commit read path**

Run:

```powershell
git add src/core/net/platform/IocpTcpTransport.h src/core/net/platform/IocpTcpTransport_win.cc src/core/CMakeLists.txt include/gamenet/core/net/TcpConnection.h src/core/net/TcpConnection.cc
git commit -m "feat: read tcp data through iocp"
```

---

### Task 7: Green TcpConnection Write, Shutdown, And Echo

**Files:**
- Modify: `src/core/net/platform/IocpTcpTransport.h`
- Modify: `src/core/net/platform/IocpTcpTransport_win.cc`
- Modify: `src/core/net/TcpConnection.cc`
- Test: `tests/contract/tcp_connection/test_tcp_connection_write_complete_ordering.cpp`
- Test: `tests/contract/tcp_connection/test_tcp_connection_shutdown_pending_output.cpp`
- Test: `tests/contract/tcp_connection/test_tcp_connection_high_water_mark.cpp`
- Test: `tests/integration/tcp/test_tcp_server_client_echo.cpp`

- [ ] **Step 1: Verify RED for write path**

Run:

```powershell
& 'D:\VS2026\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir build-local-vs2026 -C Debug -R '^contract\.tcp_connection\.test_tcp_connection_high_water_mark$|^integration\.tcp\.test_tcp_server_client_echo$' --output-on-failure --timeout 10
```

Expected: at least one test fails until Windows writes are backed by `WSASend`.

- [ ] **Step 2: Extend helper API for writes**

Add to `IocpTcpTransport.h`:

```cpp
    bool writePending() const noexcept;
    void startWrite(const char* data, std::size_t len);
    ssize_t completeWrite(int* savedErrno);

    IocpOperation writeOperation_;
    WSABUF writeBuffer_{};
    bool writePending_{false};
```

- [ ] **Step 3: Implement WSASend posting and completion**

Add to `IocpTcpTransport_win.cc`:

```cpp
void IocpTcpTransport::startWrite(const char* data, std::size_t len) {
    if (writePending_ || len == 0) {
        return;
    }
    writeOperation_.overlapped = OVERLAPPED{};
    writeOperation_.kind = IocpOperationKind::Write;
    writeOperation_.channel = channel_;
    writeOperation_.bytesTransferred = 0;
    writeOperation_.error = 0;
    writeBuffer_.buf = const_cast<char*>(data);
    writeBuffer_.len = static_cast<ULONG>(len);
    DWORD bytes = 0;
    writePending_ = true;
    const int rc = ::WSASend(channel_->fd(), &writeBuffer_, 1, &bytes, 0, &writeOperation_.overlapped, nullptr);
    if (rc == SOCKET_ERROR && sockets::lastError() != WSA_IO_PENDING) {
        writePending_ = false;
        writeOperation_.error = sockets::lastError();
        channel_->setRevents(Channel::kErrorEvent | Channel::kCloseEvent);
    }
}

ssize_t IocpTcpTransport::completeWrite(int* savedErrno) {
    writePending_ = false;
    if (writeOperation_.error != 0) {
        *savedErrno = static_cast<int>(writeOperation_.error);
        return -1;
    }
    return static_cast<ssize_t>(writeOperation_.bytesTransferred);
}

bool IocpTcpTransport::writePending() const noexcept {
    return writePending_;
}
```

- [ ] **Step 4: Delegate TcpConnection writes on Windows**

In `TcpConnection::sendInLoop()`, on Windows append bytes to `outputBuffer_`,
fire high-water crossing using the existing helper, and call:

```cpp
#ifdef _WIN32
    if (!iocpTransport_->writePending() && outputBuffer_.readableBytes() > 0) {
        iocpTransport_->startWrite(outputBuffer_.peek(), outputBuffer_.readableBytes());
        if (!channel_->isWriting()) {
            channel_->enableWriting();
        }
    }
    return;
#endif
```

In `TcpConnection::handleWrite()`, on Windows consume the completed write:

```cpp
#ifdef _WIN32
    int savedErrno = 0;
    const ssize_t n = iocpTransport_->completeWrite(&savedErrno);
    if (n > 0) {
        outputBuffer_.retrieve(static_cast<std::size_t>(n));
        if (outputBuffer_.readableBytes() > 0) {
            iocpTransport_->startWrite(outputBuffer_.peek(), outputBuffer_.readableBytes());
            return;
        }
        channel_->disableWriting();
        queueWriteComplete();
        if (state_ == kDisconnecting) {
            shutdownInLoop();
        }
        return;
    }
    if (n < 0 && !sockets::isWouldBlock(savedErrno) && !sockets::isInterrupted(savedErrno)) {
        handleError(savedErrno);
    }
    return;
#endif
```

- [ ] **Step 5: Verify write and echo contracts**

Run:

```powershell
& 'D:\VS2026\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build build-local-vs2026 --config Debug --parallel
& 'D:\VS2026\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir build-local-vs2026 -C Debug -R '^contract\.tcp_connection\.test_tcp_connection_write_complete_ordering$|^contract\.tcp_connection\.test_tcp_connection_shutdown_pending_output$|^contract\.tcp_connection\.test_tcp_connection_high_water_mark$|^integration\.tcp\.test_tcp_server_client_echo$' --output-on-failure --timeout 10
```

Expected: targeted tests PASS.

- [ ] **Step 6: Commit write path**

Run:

```powershell
git add src/core/net/platform/IocpTcpTransport.h src/core/net/platform/IocpTcpTransport_win.cc src/core/net/TcpConnection.cc
git commit -m "feat: write tcp data through iocp"
```

---

### Task 8: Full Windows Gate And Status Update

**Files:**
- Modify: `docs/development/windows_iocp_milestone.md`
- Modify: `docs/migration_status.md`
- Test: full Windows CTest and repository guards

- [ ] **Step 1: Run full Windows CTest**

Run:

```powershell
& 'D:\VS2026\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build build-local-vs2026 --config Debug --parallel
& 'D:\VS2026\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir build-local-vs2026 -C Debug --output-on-failure --timeout 10
```

Expected: 29/29 tests pass. If fewer than 29 pass, keep Windows support deferred and update the milestone with the exact failing tests.

- [ ] **Step 2: Run repository guards**

Run:

```powershell
py -3 tests\scope\test_scope_guard.py
py -3 tests\scope\test_intent_consistency.py
py -3 tools\check_scope_boundaries.py
py -3 tests\cmake\test_sanitizer_flags.py
py -3 tests\cmake\test_install_package_contract.py
py -3 tests\cmake\test_migration_status_contract.py
py -3 tests\cmake\test_msvc_utf8_contract.py
py -3 tests\cmake\test_platform_backend_contract.py
py -3 tests\cmake\test_tcp_lifecycle_contracts.py
py -3 tests\cmake\test_tcp_connection_context_contract.py
py -3 tests\cmake\test_windows_iocp_milestone_contract.py
py -3 tests\cmake\test_windows_iocp_data_path_contract.py
py -3 tests\cmake\test_release_safe_tests.py
py -3 tests\ci\test_workflow_jobs.py
git diff --check
```

Expected: every command exits 0. `git diff --check` may print line-ending warnings on Windows, but must not report whitespace errors.

- [ ] **Step 3: Update migration status with exact evidence**

If full Windows CTest passes, replace the current Windows status paragraph with:

```markdown
- Windows: Windows support remains intentionally staged, but the local VS2026
  Debug IOCP gate now configures, builds, and passes 29/29 configured CTest
  tests. Windows CI can be proposed after the install/package consumer gate is
  verified on Windows and the milestone document is updated with that evidence.
```

If full Windows CTest still has failures, keep the current "Windows support is
not promoted" statement and list the exact failing tests from CTest output.

- [ ] **Step 4: Clean temporary build directory**

Before deletion, confirm no process still references the build directory:

```powershell
Get-CimInstance Win32_Process | Where-Object { ($_.CommandLine -like '*build-local-vs2026*' -or $_.ExecutablePath -like '*build-local-vs2026*') -and $_.ProcessId -ne $PID } | Select-Object ProcessId,Name,CommandLine
```

If no processes are returned, remove only the verified in-repository build path:

```powershell
$repo = (Resolve-Path '.').Path
$target = (Resolve-Path 'build-local-vs2026').Path
if (-not $target.StartsWith($repo, [System.StringComparison]::OrdinalIgnoreCase)) { throw "Refusing to delete outside repo: $target" }
Remove-Item -LiteralPath $target -Recurse -Force
```

- [ ] **Step 5: Commit final status**

Run:

```powershell
git add docs/development/windows_iocp_milestone.md docs/migration_status.md
git commit -m "docs: record windows iocp data path evidence"
```

---

## Self-Review

- Spec coverage: The plan covers operation ownership, Poller completion translation, socket helper setup, accept, connect, read, write, close/error convergence, verification, and documentation status updates.
- Placeholder scan: The plan contains no placeholder markers and every task has concrete file paths, commands, and expected results.
- Type consistency: The operation type names are consistent across tasks: `IocpOperation`, `IocpOperationKind`, `IocpTcpTransport`, `loadAcceptEx`, `loadConnectEx`, and `createOverlappedTcpOrDie`.
- Scope: The plan is limited to the current Reactor / TCP foundation and does not promote deferred protocol, transport, game, or experimental modules.
