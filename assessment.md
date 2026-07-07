## 总体判断

`game-net-core` 当前处在 **Phase 2/3 收尾阶段**：Reactor / TCP foundation 已经从 `mini_trantor` 中拆出来，并形成了独立 CMake target、安装导出、基础示例、单元/契约/集成测试和 CI 护栏。当前方向不是继续堆功能，而是先把 **可独立消费的网络核心包** 稳住，再逐步引入 protocol / transport / game foundation / experimental 模块。

我没有在本地重新跑编译和 CTest；下面的验证状态基于仓库文件、提交记录和源码审阅。仓库自身记录的当前验证结果是 **21/21 tests passed**，包括 6 个 unit、14 个 contract、1 个 integration，并且有 ASan/UBSan、Release、install/package consumer gate。

## 当前进度

**1. 拆仓定位已经清楚。** README 明确写明：`game-net-core` 是面向 game servers 的 C++23 networking foundation，来源于 `mini_trantor` 的 component-by-component split，目标是先抽取 networking foundation，稳定 ownership/threading contracts，再提升高层模块。 当前活跃目标仍然是 Reactor / TCP foundation，protocol framing、transport abstraction、session、logic loop、broadcast、UDP/KCP 都被列为 planned/deferred，而不是当前实现范围。

**2. Phase 1、Phase 2、Phase 3 基本已落地。** `docs/migration_status.md` 记录 Phase 1 skeleton 已有 top-level CMake、README、AGENTS、docs、intents、rules、include/src/tests/examples layout；Phase 2 已有 base utilities、socket helpers、Channel/Poller/EventLoop/TimerQueue、Acceptor/Connector、TcpConnection/TcpServer/TcpClient；Phase 3 已有 `gamenet_core`、`GameNet::core`、install/export package config、echo examples、unit/contract/integration test structure、scope boundary guard、install consumer fixture 等。

**3. 构建边界已经从 monolith 变成 core package。** 顶层 CMake 使用 C++23，提供 `GAMENET_BUILD_TESTING`、ASan/UBSan、TSan、TLS、experimental 等选项，并只把 `src/core`、`examples`、`tests` 纳入当前 build。 核心库 target 是 `gamenet_core`，导出 alias 是 `GameNet::core`，并安装到 `GameNetCoreTargets`。 这说明拆仓已经不只是目录复制，而是在形成 downstream 可消费的包边界。

**4. 测试结构已经偏“契约化”。** `tests/CMakeLists.txt` 按 unit / contract / integration 分层，覆盖 Buffer、Channel、Poller、TimerQueue、EventLoop、EventLoopThread、EventLoopThreadPool、Acceptor、Connector、TcpConnection、TcpServer、TcpClient 等核心模块。 测试规则也明确要求 unit、contract、failure-path、threading-related test，并把 contract 定义为 public API、lifecycle、thread-affinity、callback ordering 的可执行约束。

**5. CI 护栏方向正确。** CI 当前在 Ubuntu 24.04 上跑 Debug build/test、ASan/UBSan build/test、Release build/test，并在 Debug job 中做 install + external consumer `find_package(GameNetCore)` 验证。 ASan/UBSan 和 Release job 都会运行 CTest。 另外还有 Python guard 检查 scope、sanitizer flags、install package contract、platform backend contract、release-safe tests 和 workflow job 结构。

## 与 mini_trantor 的关系

`mini_trantor` 已经是更大的实验性框架：README 里列出的当前主线包含 Reactor core、TCP、线程模型、coroutine bridge、HTTP/WebSocket/RPC、HttpClient/RpcConnectionPool、DNS、TLS、IPv6、UDP/KCP、PacketFramer、CodecAdapter、SessionManager、LogicLoop、GameServerPipeline、broadcast、metrics hook 等。 它的 CMake 也显示同一个 target 里混合了 core、TLS、UDP/KCP、broadcast、framing、codec、game、DNS、SignalWatcher、HTTP、WebSocket、RPC 等大量模块。

所以 `game-net-core` 当前不是 `mini_trantor` 的“功能迁移进度已追平版”，而是一次 **反向收敛**：先把 `mini_trantor` 中被高层协议和游戏管线污染风险最高的底层 Reactor/TCP 核心抽成独立、可验证、可安装、可复用的 foundation。这个方向是合理的，因为 `mini_trantor` README 自己也强调它不是生产替代品，并且还有 soak、真实压测、跨平台兼容、完整 fuzz corpus 等生产级缺口。

## 当前技术方向

**第一方向：稳定 owner-loop 语义。** `EventLoop` 明确是单线程 Reactor 调度核心，所有 loop-owned mutable state 只能在 owner thread 访问和销毁。 实现里维护 thread-local loop、owner thread id、wakeup fd、pending functor queue、TimerQueue 和 Poller；`runInLoop` 同线程立即执行，跨线程走 `queueInLoop`，`queueInLoop` 会在必要时 wakeup。  这和仓库规则一致：跨线程交互只能通过 `runInLoop`、`queueInLoop`、wakeup mechanism。

**第二方向：把 TCP lifecycle 作为核心交付面。** `TcpConnection` 绑定一个 `EventLoop`，拥有 socket/channel/buffer 状态，并提供 send/shutdown/forceClose/high-water/write-complete/context 等基础接口。  `TcpServer` 负责 accept、loop 分配、连接表和 stop/force-close，不解析协议。 `TcpClient` 通过 `Connector` 主动连接，最多拥有一个 plain `TcpConnection`。

**第三方向：强制 scope hardening。** `check_scope_boundaries.py` 明确只允许 active component 为 `core`，把 `protocol`、`transport`、`game`、`experimental` 作为 deferred component，并扫描 active implementation/test/example/cmake/CI 面中的 legacy mini include、mini namespace、deferred namespace、deferred target、HTTP/WS/RPC/KCP/TLS/UDP/GameServerPipeline/SessionManager/LogicLoop/BroadcastRouter/DnsResolver 等高层名词。  这说明当前方向是“先防止功能回流”，再谈 Phase 4 promotion。

**第四方向：Windows 不走 select，目标是 IOCP。** 当前 CMake 在 Windows 选择 `SocketsOps_win.cc`、`Wakeup_win.cc`、`IocpPoller.cc`，非 Windows 选择 Linux socket/wakeup/EPollPoller。 Poller intent 明确说 v1 backend targets 是 Linux epoll 和 Windows IOCP，WinSock `select()` 不是当前迁移可接受的性能目标。 但 Windows CI 仍然 deferred，直到 IOCP network backend 迁移完成。

## 主要缺口与风险

**1. Windows IOCP 还是 skeleton，不应被视为已完成跨平台支持。** `IocpPoller.h` 注释写得很直接：它拥有 completion port，并把 socket 句柄关联到端口；实际 TCP overlapped read/write completion state 需要后续连接层迁移接入。 也就是说，目前 Windows 方向是正确的，但还没有完成真正的 IOCP TCP 数据路径。当前最稳的运行平台仍是 Linux epoll。

**2. 当前测试数量够做 migration gate，但还不够证明生产稳定。** 21 个测试覆盖 unit/contract/integration 的基本骨架，echo integration 也已经存在。  但它还不是 soak/stress/fuzz/TSan/close-race/half-close/backpressure 的充分证据。仓库规则本身也要求 lifecycle-sensitive modules 覆盖 remove-before-destroy、callback-after-destroy prevention、repeated close/error guard、registration state consistency。

**3. 文档迁移仍有残留。** 例如 `platform_runtime.intent.md` 仍然写着 `mini/net/platform/SocketTypes.h`、`mini/net/SocketsOps.h`、`mini/net/poller/` 等旧路径。 但实际 public header 已经是 `gamenet/core/net/SocketsOps.h`。 这类残留不一定破坏代码，但会降低 intent 文档作为“模块宪法”的可信度。尤其当前 scope guard 扫描的是 include/src/tests/examples/cmake/.github/CMakeLists，而不是 intents/docs/rules。

**4. 一些 public API 的线程契约还可以更硬。** 规则要求 TcpConnection 状态转移、socket read/write 处理和跨线程 send 都回到 owner EventLoop。 `send()`、`shutdown()`、`forceClose()` 已经做了 loop marshal 或 owner-thread 执行。 但 `setContext()` / `getContext()` 目前直接操作 `std::any`，没有 thread assertion 或明确“只能在连接 owner loop 调用”的 API 约束。 这在未来接入 protocol adapter / session context 时可能成为 data race 或语义歧义点。

## 建议的下一步

我建议当前不要急着进入 Phase 4。更优顺序是：

1. **先把 active intent 文档迁移干净**：把 active module intents 中残留的 `mini/net/...` 路径改成 `gamenet/core/net/...`，并增加一个轻量 doc/intent consistency check，至少扫描 active intents 中的 legacy public path。这样 intent-driven workflow 才不会出现“代码已拆、模块宪法还是旧仓库”的漂移。

2. **补强 TCP lifecycle 的负向契约测试**：优先补 `TcpConnection` repeated close/error、peer reset、shutdown while output buffer pending、high-water mark、write-complete callback ordering、server stop with active connections、client retry stop race。现在主链路 echo 已通，但 foundation 的价值在生命周期边界。

3. **把 Windows IOCP 作为独立 milestone**：不要仅因为 CMake 选择了 `IocpPoller` 就宣称 Windows supported。下一步应先定义 IOCP completion ownership、overlapped read/write state 所属对象、cancel/close ordering，然后补 Windows-specific contract tests，再开 Windows CI。

4. **保持 Phase 4 的 promotion gate**：当要迁移 PacketFramer、transport abstraction 或 session foundation 时，应一次只提升一个 component，同时更新 `ACTIVE_COMPONENTS`、migration status、CMake target、tests 和 docs。当前 scope guard 已经明确要求 intentional Phase 4 promotion 必须同步更新 active scope 和 migration status。

## 结论

`game-net-core` 当前方向是健康的：它正在把 `mini_trantor` 的庞大实验框架拆成一个小而硬的 Reactor/TCP core，而不是复制整个 mini_trantor。当前可认为 **core migration 已基本成形，工程护栏已建立，CI/package/test skeleton 已可用**；但还不应称为生产级网络库，也不应马上扩协议层。下一阶段最有价值的是：清理 active intent 残留、补生命周期负向测试、完成真正 IOCP 数据路径，然后再按 Phase 4 分层迁移 protocol / transport / game foundation。
