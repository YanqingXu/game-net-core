## 结论

`game-net-core` 这次确实前进了一大步，但“大步”不是 Phase 4 功能扩张，而是 **Reactor/TCP core 的工程化、契约化、跨平台化大幅推进**。当前项目仍然明确停留在 **Reactor / TCP foundation**，protocol / transport / game foundation / experimental 仍未被提升为 active scope。README 仍然写明当前 active target 是 Reactor/TCP foundation，高层模块要等 core proven 后再 promotion。

最重要的变化是：Windows 不再只是“以后迁移 IOCP”的方向声明，而是已经出现 **IOCP 数据路径实现 + Windows MSVC CI gate + 29 个测试的跨平台验证入口**。最新提交链里能看到 Windows MSVC IOCP gate、仅安装 core public headers、VS2026 generator 等变化。 最新 workflow run 已经 completed/success，并且 Linux Debug、Linux ASan/UBSan、Linux Release、Windows MSVC IOCP 四个 job 全部 success。

## 当前进度判断

我现在会把当前状态从之前的“Phase 2/3 收尾”上调为：

**Phase 2 / Phase 3 已经基本站稳，正在进入 core hardening + platform hardening 阶段；Phase 4 仍然未开始。**

依据如下：

`docs/migration_status.md` 已更新到 2026-07-07，记录当前 worktree 配置了 **29 个 CTest tests**，包括 6 个 unit、22 个 contract、1 个 integration；相比之前 21/21 的基线，新增的不是泛泛的 happy path，而是 lifecycle、intent/documentation guards 和 Windows IOCP 数据路径相关契约。

测试清单也对应扩展了：新增 `tcp_client_retry_stop_race`、`tcp_server_stop_active_connections`、多个 `tcp_connection` close/reset/high-water/shutdown/write-complete/repeated-force-close 契约测试。 这说明项目开始补之前最缺的边界：关闭路径、重入、跨线程/重试竞态、背压/高水位、输出未完成时 shutdown 等。

CI 也从 Linux-only gate 扩展为 Linux + Windows：workflow 现在有 `linux-cmake`、`linux-asan-ubsan`、`linux-release`、`windows-msvc` 四个 job；Windows job 会配置 Visual Studio generator、build Debug、跑 CTest，并安装后构建 external consumer。

## 最大推进点：Windows IOCP 从 skeleton 进入数据路径

这是这轮进度的核心。

之前我会把 Windows 判断为“IOCP skeleton，不能算完整 backend”。现在已经不同：`src/core/CMakeLists.txt` 在 Windows 分支中新增了 `IocpSocketOps_win.cc`、`IocpTcpTransport_win.cc` 和 `IocpPoller.cc`，而不是只挂一个空的 `IocpPoller`。

新的 IOCP 抽象至少包括四层：

**1. Completion metadata 层。** `IocpOperation` 定义了 `Accept / Connect / Read / Write` 四种 operation kind，并把 `OVERLAPPED`、`Channel*`、bytesTransferred、error 放在同一个 completion 元数据结构里。

**2. Windows socket extension 层。** `IocpSocketOps` 提供 `createOverlappedTcpOrDie`、`loadAcceptEx`、`loadConnectEx`、`updateAcceptContextOrDie`、`updateConnectContextOrDie`、`bindUnspecifiedOrDie` 等接口。 对应实现里使用 `WSASocketW(... WSA_FLAG_OVERLAPPED)` 创建 overlapped TCP socket，并通过 `SIO_GET_EXTENSION_FUNCTION_POINTER` 加载 AcceptEx/ConnectEx。

**3. Poller completion 翻译层。** `IocpPoller::poll()` 调用 `GetQueuedCompletionStatus`，把 completion 中的 `IocpOperation` 转换成 Channel read/write/error/close event，并且通过 `PostQueuedCompletionStatus` 实现 IOCP wakeup。  `Poller` base class 也新增了 `preserveSocketAssociation()` 和 `wakeup()` 虚接口，Linux 默认返回 false/空操作，Windows override。

**4. TCP read/write transport 层。** `IocpTcpTransport` 现在负责 loop-owned `WSARecv` / `WSASend` overlapped read/write 状态，包含 `startRead()`、`completeRead()`、`startWrite()`、`completeWrite()`。 实现中 `startRead()` 提交 `WSARecv`，`startWrite()` 提交 `WSASend`，完成时把 bytes/error 反馈给 `TcpConnection`。

这意味着 Windows backend 已经从“编译占位”升级为 **真正参与 EventLoop / Acceptor / Connector / TcpConnection 主路径的 backend**。

## TCP 主链路也变硬了

`Acceptor` 在 Windows 下现在用 `AcceptEx`：listen 后创建 `IocpAcceptState`，加载 `AcceptEx` / `GetAcceptExSockaddrs`，post accept；完成后调用 `SO_UPDATE_ACCEPT_CONTEXT` 并把 accepted fd 交给上层。

`Connector` 在 Windows 下现在用 `ConnectEx`：创建 overlapped TCP socket、bind unspecified address、post `ConnectEx`，connect 完成后调用 `SO_UPDATE_CONNECT_CONTEXT`；fd 从 Connector 迁移到 TcpConnection 前还会调用 `preserveSocketAssociation()`，避免 IOCP 关联在 ownership transfer 时丢失。

`TcpConnection` 的 Windows 路径也改为通过 `IocpTcpTransport` 完成 read/write：connectEstablished 后 startRead，read completion 后重新 post read；write completion 后更新 output buffer，必要时继续 startWrite。

另一个细节也很关键：之前我指出 `TcpConnection::context` 是潜在线程契约缺口；现在它已经被收紧。header 明确写明 connection context 是 loop-owned mutable state，只能在 owner EventLoop thread 调用；实现中 `setContext()` / `getContext()` 都调用 `loop_->assertInLoopThread()`。

## 工程边界变得更干净

这轮还有一个方向很正确：不是只堆实现，而是在防止 scope 漂移。

CI 现在新增了 active intent consistency guard，确保 active intents 不再使用 legacy `mini/net` 路径，也要求 platform runtime intent 指向迁移后的 `gamenet/core/net/...` 路径。

安装边界也更收敛了：顶层 CMake 现在只安装 `include/gamenet/core` 到 `${CMAKE_INSTALL_INCLUDEDIR}/gamenet`，不是安装整个 `include/gamenet`。 这和 scope boundary 文档一致：deferred modules 不应保留空 public-header/source placeholder；目录只有在模块被正式 promotion、并同步 intent/tests/CMake/migration-status 时出现。

CI 文档也明确说 deferred modules 仍然关闭，包括 TLS、experimental transport、HTTP、WebSocket、RPC、UDP、KCP、PMTU/FEC、metrics、game pipeline。 这说明当前方向不是“把 mini_trantor 全搬过来”，而是继续保持 core target 的窄边界。

## 和 mini_trantor 的关系：不再只是子集，而是底层再工程化

`mini_trantor` 当前仍是大而全的实验框架：包含 Reactor/TCP、coroutine bridge、HTTP/WebSocket/RPC、HttpClient/RpcConnectionPool、DNS/TLS/IPv6、UDP/KCP、PacketFramer/CodecAdapter、SessionManager/LogicLoop/GameServerPipeline、broadcast/metrics 等。

但 `game-net-core` 现在已经不是简单“复制 mini_trantor 的 core 子目录”。它在一个更窄的范围里做了更硬的工程化：

`mini_trantor` README 仍把 Windows 后端描述为 `SelectPoller` preview；而 `game-net-core` 当前已经把 Windows active backend 明确推进到 IOCP，并通过 Windows MSVC workflow gate 验证。

所以这次拆解的意义变成：**mini_trantor 是功能验证场；game-net-core 是把其中最底层网络核心抽出来，重新做 target 边界、contract tests、install package、Linux/Windows backend gate 的基础库。**

## 当前风险与不足

第一，`docs/migration_status.md` 已经有一点时序滞后：文件里还写着新 lifecycle contracts 的 fresh Linux CI execution pending，以及旧 baseline 是 21/21。 但实际最新 commit 的 workflow run 已经成功，且四个 job 全 green。  建议下一次提交同步 migration status，把“pending fresh Linux CI”改成“latest CI run passed”。

第二，IOCP 路径虽然已落地，但仍应视为 **core backend milestone**，不是生产完成态。Windows milestone 文档自己也列出还要完成/保持的 ownership model：completion port ownership、EventLoop wakeup、loop-owned overlapped read/write state、Channel observer 角色、socket close / Channel remove / pending overlapped cancellation / final callbacks 的明确顺序。 特别是 pending overlapped cancellation/close ordering 仍是高风险区域，虽然已有 active connection 与 pending operation 的 gate 要求。

第三，`IocpPoller.h` 的注释仍说 read/write completion state 由后续连接层迁移接入，但现在 `IocpTcpTransport` 已经存在并接入 `TcpConnection`。 这是小问题，但在 intent-driven 项目里，注释/intent/实现漂移应该尽快修掉。

第四，Phase 4 仍未开始。Scope boundary 仍明确把 protocol framing、transport abstraction、HTTP、WebSocket、RPC、UDP、KCP、TLS、GameServerPipeline、SessionManager、LogicLoop、Broadcast、MetricsExporter 等列为 deferred。 这不是缺点，而是当前路线的核心约束。

## 当前方向建议

接下来最合理的方向不是马上迁移 HTTP/RPC/game，而是按这个顺序继续推进：

**第一，更新 migration status 与 IOCP 注释。** 把最新 CI green、29/29、Windows job success 写回 `docs/migration_status.md`，并修正 `IocpPoller.h` 中“read/write 以后接入”的过时注释。

**第二，继续压 TCP lifecycle 的深水区。** 重点补 pending overlapped cancel/close ordering、half-close、large output/backpressure、server stop during active write、client stop during ConnectEx pending、timer cancellation race。这比迁移 PacketFramer 更有价值。

**第三，补 TSan/soak/benchmark 证据。** 现在 Linux ASan/UBSan、Release、Windows Debug gate 都已建立；下一步如果要称 core 稳定，应增加长跑和 race-oriented 验证，尤其是 EventLoopThreadPool、TcpServer stop、TcpClient retry、TcpConnection close path。

**第四，Phase 4 promotion 仍应一次只升一个模块。** 最适合第一个提升的不是 HTTP/RPC，而是 **protocol framing / PacketFramer**：它足够靠近 TCP core，但仍能保持无业务逻辑。提升时要同步 active scope、CMake target、install headers、contract tests、migration status 和 scope guard 白名单。

## 最终判断

`game-net-core` 当前已经从“拆出 Reactor/TCP core 的新仓库”升级为 **具备双平台 CI gate、安装消费契约、生命周期负向测试、IOCP 数据路径雏形的独立 core networking foundation**。它仍然比 `mini_trantor` 功能少很多，但底层边界、测试纪律和 Windows backend 方向已经比原仓库更硬。

我会把当前方向概括为：

**先把 Reactor/TCP core 做成可被下游稳定消费的、Linux epoll + Windows IOCP 双后端 foundation；Phase 4 的 protocol/transport/game 迁移继续后置，直到 core 的 lifecycle、platform、package、soak/race 证据足够硬。**
