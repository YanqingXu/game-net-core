## 总体判断

`game-net-core` 的进展方向很明确：**不是在做上层 game framework，而是在把 Reactor/TCP core 打磨成可验证、可安装、双平台、race-oriented 的 C++23 网络基础库**。README 仍然把它定义为从 `mini_trantor` 拆出的 networking foundation，迁移策略是先稳定 ownership / threading / contracts，再提升更高层模块。

我会把当前状态判断为：

**Phase 1 / 2 / 3 基本完成；当前处在 Phase 3.5：Reactor/TCP core hardening、race hardening、CI hardening、Windows IOCP validation。Phase 4 仍然不应正式开始。**

仓库自己的 phase table 也支持这个判断：skeleton、Reactor/TCP core、CMake/test/package split 都是 present；protocol / transport / game foundation / experimental 仍然 deferred。

---

## 最新进展：主线已经从“功能迁移”转为“生命周期与并发收敛”

最新 commit 是 `d3fc124...`，提交信息为 `test: harden reactor lifecycle contracts`；它前面的提交也集中在 lifecycle、threading、TSan、pending write、logger 等 hardening 方向。

这说明项目现在的核心工作不是继续往上迁 HTTP/RPC/game pipeline，而是把已有的 Reactor/TCP foundation 压到更强的验证矩阵下。当前 migration status 记录了 **65 个 CTest tests**：7 个 unit、57 个 contract、1 个 integration。

从 scope 看，Stable/Core 目前包括 Logger、EventLoop、Channel、Poller、TimerQueue、Buffer、InetAddress、Socket、Acceptor、Connector、TcpConnection、TcpServer、TcpClient、EventLoopThreadPool；planned modules 仍是 protocol framing、transport abstraction、session foundation、logic loop、broadcast、UDP/KCP experimental。

---

## 架构与构建边界：目前保持得比较干净

顶层 CMake 仍然只打开核心路径：`src/core`、`examples`、`tests`；TLS 和 experimental 都是 option，默认不进入 active scope。安装边界也只安装 `include/gamenet/core`，这对 component split 很重要。

`src/core/CMakeLists.txt` 只构建 `gamenet_core`，并导出 `GameNet::core`；平台选择也清晰：Linux 侧是 `SocketsOps_linux`、`Wakeup_linux`、`EPollPoller`，Windows 侧是 `SocketsOps_win`、`Wakeup_win`、`IocpSocketOps_win`、`IocpTcpTransport_win`、`IocpPoller`。

这个边界是健康的：现在还没有把 protocol、transport、game、experimental 的实现面混进 active target。Phase 4 入口继续压在 migration gate 后面是正确策略。Roadmap 也明确说 Phase 4 必须等 `migration_status.md` 的 readiness gate 满足后才能开始。

---

## CI 进展：已经进入“多维验证”阶段

当前 `ci.yml` 有五个主 job：

Linux Debug CMake build and tests；Linux ASan/UBSan；Linux TSan race-oriented；Linux Release；Windows MSVC IOCP build and tests。

这已经不是“能编译就行”的 CI，而是把 core 的几个关键风险面拆开验证：

Debug 全量 CTest；ASan/UBSan 覆盖内存与 UB；TSan 只跑 `threading` label 的 race-oriented subset；Release 跑测试，防止 `NDEBUG` 把 contract 检查消掉；Windows MSVC IOCP 既跑 CTest，又验证 install/package consumer。

另外，`long-soak` workflow 是手动触发，不挂 push/PR，默认 repeat 20、timeout 60，只重复跑 `threading` slice。这个设计很好：普通 CI 不被拖慢，但高风险 lifecycle/race 改动可以用它补长跑证据。

---

## Windows IOCP：已经从 skeleton 推到真实数据路径

Windows 方向的进展很实质。`windows_iocp_milestone.md` 明确说当前 Windows source selection 使用 `IocpPoller`，WinSock `select()` 不是 accepted fallback。

IOCP path 现在覆盖了 EventLoop wakeup、TimerQueue、EventLoopThreadPool、Acceptor/TcpServer、Connector/TcpClient、TcpConnection read/write/lifecycle、integration echo 等路径。文档记录 Windows CTest 本地 65/65 passed，并且列出了 `PostQueuedCompletionStatus`、`AcceptEx`、`ConnectEx`、`WSARecv`、`WSASend` 等实际路径。

实现上，`IocpPoller` 通过 `GetQueuedCompletionStatus` 取 completion，把 `IocpOperation` 映射回 Channel，并用 `PostQueuedCompletionStatus` 实现 wakeup；这说明 Windows 不是简单模拟 epoll，而是走 completion model。

`IocpTcpTransport` 也已经封装了 overlapped `WSARecv` / `WSASend`、pending read/write 状态、completion bytes、cancel pending operations。

---

## 测试矩阵：重点已经覆盖到高风险生命周期路径

`tests/CMakeLists.txt` 显示测试已经系统覆盖 Acceptor、Connector、EventLoop、EventLoopThread、EventLoopThreadPool、Poller、TcpClient、TcpServer、TimerQueue、TcpConnection、integration echo，并大量打上 `threading` / `lifecycle` 标签。

最有价值的是 TcpClient / Connector / TcpConnection / TcpServer 的 race/lifecycle 组合测试已经很密：

TcpClient 覆盖 retry-stop、pending connect stop、cross-thread stop pending connect、destroy pending connect、destroy active connection、active connection stop mixed timing、cross-thread disconnect、repeated disconnect/stop/connect、cross-thread connect。

TcpServer 覆盖 active connections stop、active write stop、multi-worker stop、worker active-write soak、worker callback stop soak、repeated stop、stop soak。

TcpConnection 覆盖 peer close/reset、repeated forceClose、repeated connectDestroyed、cross-thread send、send after close、cross-thread forceClose、pending read/write forceClose、cross-thread shutdown、repeated shutdown、shutdown pending output、write-complete ordering。

这组测试方向是对的，因为 Reactor/TCP core 真正容易出事故的地方不是 echo demo，而是 stale completion、stop/destroy reentry、owner-loop handoff、pending IO cancellation、worker loop teardown、callback ordering。

---

## 代码层面看，核心生命周期模型已经比较完整

`TcpConnection::send()` 已经区分 owner-loop 与 cross-thread 调用；非 owner 线程调用会复制 payload 并通过 `runInLoop` 回到 owner loop，且只在 connected 状态下发送。`shutdown()` 和 `forceClose()` 也都通过 loop 收敛。

`connectEstablished()` 会 tie channel、enable reading，并在 Windows 侧启动 IOCP read；`connectDestroyed()` 会处理 connected/disconnecting/disconnected 状态，disable/remove Channel，并防止重复 remove。

`handleClose()` 在 Windows 侧如果还有 pending overlapped operations，会设置 `forceClosePending_`、保留 `forceCloseGuard_`、cancel pending operations，等 completion 收敛后再 `finishClose()`。这正是 IOCP lifecycle 正确性里最重要的一步：不能释放 operation storage 后还让 completion 回来打到悬空对象。

`TcpClient` 的 connect/disconnect/stop 都 marshal 到 owner loop；new connection 创建 TcpConnection 后设置 callback 并调用 `connectEstablished()`；removeConnection 也回到 owner loop 后再 queue `connectDestroyed()`，总体符合 owner-loop 模型。

`TcpServer::stopInLoop()` 会先停 Acceptor，再 forceClose all connections；只有在没有 active connections 时才 stop thread pool。对于 worker-owned connections，它通过 remaining counter 等所有 close path 收敛后再 stop pool，这个设计方向是合理的。

---

## 我看到的主要风险与缺口

### 1. 最新 HEAD 的远端 green 证据存在“语义漂移”

仓库文档记录的 remote green 是 `ci #23` / commit `9b27a0a...`，并说明该 run 覆盖 Linux CMake、ASan/UBSan、TSan、Release、Windows MSVC IOCP。

但当前最新 commit 已经是 `d3fc124...`。这意味着严格说，**文档里的 green evidence 不是最新 HEAD 的 evidence，而是 latest HEAD 前一笔 commit 的 evidence**。这不是严重问题，因为 `d3fc124` 看起来主要是 tests/docs/contract gate hardening；但在 Phase 4 readiness gate 语境下，应该把“latest HEAD green”说严谨：要么为 `d3fc124` 记录新的 CI run，要么把文档措辞改成“latest recorded remote green is 9b27”。

本轮推进已选择保守路径：`docs/migration_status.md` 和 `docs/development/ci.md` 明确把 `ci #23` / `9b27a0a...` 记录为 latest recorded remote green evidence，同时保留 Phase 4 gate 对最新 HEAD fresh remote CI evidence 的要求。这样不会把 `d3fc124...` 误写成已有远端 green。

### 2. `long-soak.yml` 的 guard 列表和 `ci.yml` 有轻微不一致

`ci.yml` 的 repository guards 已经包含 `tests/cmake/test_event_loop_contracts.py`。

但 `long-soak.yml` 的 guard list 里没有同一个 EventLoop contract guard；它有 event_loop_thread_pool、migration、MSVC、platform backend、TCP lifecycle、TimerQueue、threading、Windows IOCP、release-safe、workflow jobs、long-soak workflow guard。

这不是功能 bug，但属于 CI parity drift。建议下一次小 PR 把 `test_event_loop_contracts.py` 加进 long-soak guard list，保持普通 CI、文档、本地 equivalent、long-soak 的 guard surface 一致。

本轮推进已补齐 parity：`.github/workflows/long-soak.yml` 的 repository guards 现在包含 `tests/cmake/test_event_loop_contracts.py`，`tests/ci/test_long_soak_workflow.py` 也会要求 long-soak workflow、CI 文档和 migration status 同步记录这个 EventLoop guard。

### 3. `EventLoop::queueInLoop()` 有一个值得优先审计的潜在 TSan 风险

`EventLoop` 的 `callingPendingFunctors_` 是普通 `bool`，而 `queueInLoop()` 可能被跨线程调用，并读取 `callingPendingFunctors_` 来决定是否 wakeup。

同时，`callingPendingFunctors_` 在 `doPendingFunctors()` 里被 owner loop 写入 true/false。

这类模式在 Reactor 库里很常见，但从严格 C++ data-race 角度看，普通 bool 被一个线程写、另一个线程无锁读，是需要审计的。建议把它改成 `std::atomic<bool>`，或者把这个状态纳入 mutex 保护，或者通过 contract 明确证明只有 owner thread 读写。考虑到项目已经把 TSan 作为 gate，这个点值得尽早处理，避免以后在高并发测试里随机爆 race report。

本轮推进已按最小 atomic 路径处理：`callingPendingFunctors_` 改为 `std::atomic<bool>`，`queueInLoop()` 使用 relaxed load 做 wakeup 判定，`doPendingFunctors()` 使用 relaxed store 标记 pending functor 执行窗口；EventLoop intent 和 contract guard 也要求 cross-thread-observed pending functor execution state 必须 atomic 或 synchronized。

### 4. `TcpClient::enableRetry()/disableRetry()` 的线程域还不够硬

TcpClient intent 明确说 connect/disconnect 可 cross-thread marshal，新连接/移除连接在 owner loop，Connector state transition 在 owner loop。

但 `enableRetry()` / `disableRetry()` 现在直接写 `retry_` 并调用 connector 的 retry flag setter，没有 owner-loop assert，也没有 marshal。

如果设计意图是“只能在 start/connect 前配置”，建议在 header/intent 里写清楚，并在测试里锁死；如果希望它也是 runtime API，则应改为 owner-loop mutation，避免和 active connect/remove/retry path 并发。这个点不是当前 blocker，但应该在 Phase 4 前收口，因为 protocol framing 之后用户 API 面会变大，模糊线程语义会被放大。

本轮推进已选择 runtime API 语义：`enableRetry()` / `disableRetry()` 可跨线程调用，并通过 `runInLoop()` marshal 到 owner loop 后再更新 `Connector` retry state；`retry_` 改为 atomic，供非 owner 线程安全观察。新增 `contract.tcp_client.test_tcp_client_cross_thread_retry_config` 覆盖非 owner `disableRetry()`，并验证 peer close 之后不会通过 retry 复活已禁用 retry 的 client。

### 5. Logger 的“线程安全”语义需要再定义精确一点

Logger header 写的是“同步、线程安全的基础日志能力”，并且支持设置 OutputFunc / TopicOutputFunc / FlushFunc。

实现上，callback 配置通过 mutex 取 snapshot，但真正调用 output/topicOutput 是在 mutex 外部进行。

这有利于避免 callback reentry 死锁，但“线程安全”到底是指 logger 内部配置安全，还是每条日志输出也全局串行，就需要明确。如果用户自定义 output function 不是线程安全的，当前实现不会替它串行化。建议把 contract 写成二选一：要么保证 output invocation 串行；要么文档明确 OutputFunc 必须自行保证线程安全。

### 6. IOCP 当前 correctness 优先，后续性能上可能需要 batching

`IocpPoller::poll()` 现在每次 `GetQueuedCompletionStatus` 取一个 completion，然后返回 active channel list。

这对 correctness 和 contract 测试足够；但如果目标是高吞吐 game server core，后续可以考虑 `GetQueuedCompletionStatusEx` 或小批量 drain，以减少高并发 completion 下的 loop iteration overhead。这个属于 Phase 3.5 后半段或 Phase 4 前性能预研，不应抢在 lifecycle correctness 前面。

---

## 下一步建议的 PR 拆分

**PR A：HEAD evidence / docs correction**

已按保守分支推进：`docs/migration_status.md`、`docs/development/ci.md`、`assessment.md` 已把 `ci #23` / `9b27a0a...` 写成 latest recorded green evidence，而不是 latest HEAD green evidence。若后续 `d3fc124` 或更新 HEAD 有新的 green run，再记录 run id、commit sha、时间、job 结果。

**PR B：EventLoop TSan cleanup**

已按最小改法推进：`callingPendingFunctors_` 改成 `std::atomic<bool>`，并用 relaxed load/store 仅承载 wakeup 判定的跨线程观察语义；`tests/cmake/test_event_loop_contracts.py` 锁定 intent/header/source 约束。这个 PR 没有混入 TcpConnection/TcpClient 行为变更。

**PR C：long-soak guard parity**

已推进：`tests/cmake/test_event_loop_contracts.py` 已加入 `.github/workflows/long-soak.yml` 的 guard list，并扩展 `tests/ci/test_long_soak_workflow.py`，要求 long-soak workflow、CI 文档和 migration status 都包含这个 EventLoop guard parity 记录。

**PR D：TcpClient retry config threading contract**

已推进：`enableRetry()/disableRetry()` 明确为 runtime API，跨线程调用经 owner loop marshal 后更新 `Connector` retry state；新增 cross-thread retry config contract，并把 intent、CMake、threading gate、migration/CI 文档同步到这个语义。

**PR E：TcpConnection race hardening continuation**

继续围绕 pending read/write forceClose、shutdown pending output、writeComplete ordering、post-close send ignore、peer close/reset reentry 做 TSan 和 long-soak 证据。这个 PR 应尽量只碰 TcpConnection / IocpTcpTransport / 对应 tests。

**PR F：Phase 4 design-only PacketFramer**

等 A–E 稳定后，再开 Phase 4 的第一步。第一步建议仍然是 protocol framing / PacketFramer 的 design-only PR：只加 intent、invariants、contract plan，不直接实现 HTTP/RPC/session/game pipeline。Roadmap 里 Phase 4 的顺序也是 protocol framing 先于 transport abstraction 和 game foundation。

---

## 最终判断

`game-net-core` 当前进展是健康的：core scope 没膨胀，Linux/Windows 双平台路径已经建立，IOCP 不再是 skeleton，CTest 数量和 contract 密度显著提高，TSan 与 long-soak 也已经进入工程流程。

但现在还不是 Phase 4 的最佳时点。下一阶段最有价值的工作不是“迁更多模块”，而是继续补齐 **expanded 44-test threading slice 的 repeat-soak evidence**，并把 **TcpConnection pending IO lifecycle** 继续压到 TSan / long-soak 证据下。等这些都稳定后，再从 **PacketFramer / protocol framing** 作为 Phase 4 的第一个设计入口推进。
