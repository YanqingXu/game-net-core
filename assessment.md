以下分析最初基于通过 GitHub 读取的仓库元数据、最近提交、PR/Issue、README、CMake、CI workflow、migration status 和关键文档；本轮后续已在本地补跑 Debug / Release CMake、build 和 CTest。

## 总体判断

`game-net-core` 当前不是在推进上层功能，而是在把 **Reactor / TCP foundation** 做成可验证、可安装、双平台、race-oriented 的 C++23 网络核心。项目定位仍然很明确：它是从 `mini_trantor` 拆出的游戏服务器网络基础库，目标是先稳定 ownership/threading/contracts，再迁移更高层模块。README 也明确说当前 active target 仍是 Reactor / TCP foundation，protocol / transport / game foundation / experimental 都还在 planned/deferred 状态。

我的结论是：

**Phase 1/2/3 已经基本完成；仓库现在处在 Phase 3.5：core hardening / race hardening / sanitizer hardening。Phase 4 还不应正式开始。**

## 最新进度

最近提交的方向非常集中：最新提交是 `test: harden lifecycle verification gates`，前面几笔包括 `feat: add coost-compatible logger`、`fix: repair linux pending write socket options`、`fix: stabilize tsan threading contracts`、`test: harden tcp lifecycle threading gates`、`test: harden reactor tcp lifecycle`。这些提交标题显示，近期主线是在强化 lifecycle、TSan/threading、pending write、logger 基础设施，而不是扩展 HTTP/RPC/game pipeline。

当前 migration status 记录的阶段表也说明：Phase 1 skeleton、Phase 2 Reactor/TCP core、Phase 3 CMake/test/package split 都已 present；Phase 4 仍是 deferred。当前已配置 **64 个 CTest tests**，包括 **7 个 unit、56 个 contract、1 个 integration**。

核心 target 仍然很干净：顶层 CMake 只进入 `src/core`、`examples` 和 `tests`；安装边界也只安装 `include/gamenet/core`。`src/core/CMakeLists.txt` 只构建 `gamenet_core` / `GameNet::core`，Linux 侧走 EPoll，Windows 侧走 `IocpPoller`、`IocpSocketOps_win`、`IocpTcpTransport_win`。

Windows IOCP 已经不是占位实现。当前 Windows milestone 记录了本地 VS2026 Debug build 成功、Windows CTest 64/64 passed，并且明确说 Windows support 使用 `IocpPoller`，不接受 WinSock `select()` 作为 active target fallback。

TcpConnection / TcpClient / TcpServer 的高风险路径已经被大量 contract 覆盖：direct Connector retry-stop cancellation、repeated connectDestroyed stale-registration cleanup、post-close send ignore、pending `ConnectEx` stop/destroy、cross-thread connect/disconnect、repeated active disconnect idempotence、repeated active stop idempotence、repeated active connect idempotence、pending read/write forceClose、cross-thread shutdown、server active-write stop、worker-callback stop reentry、server repeated stop idempotence 等都进入测试矩阵。`tests/CMakeLists.txt` 里可以看到这些测试都被打上 `threading` / `lifecycle` 标签，用于后续 TSan 和 soak gate。

CI 也已经明显升级。`ci.yml` 当前包含 Linux Debug CMake、Linux ASan/UBSan、Linux TSan、Linux Release、Windows MSVC IOCP 五类 job；其中 TSan job 使用 `GAMENET_ENABLE_TSAN=ON`，只跑 `threading` label。

同时新增了一个非默认 `long-soak` workflow，只能手动触发，默认 repeat 20、timeout 60 秒，专门重复运行 `threading` CTest slice。这是正确的设计：不拖慢普通 PR gate，但给 race/lifecycle 问题留出长跑证据通道。

## 当前主要风险

第一，**最新 HEAD 的远端完整 green 证据已经补齐**。GitHub Actions 当前显示 `ci #23` / commit `9b27a0a` 成功，覆盖 Linux CMake、Linux ASan/UBSan、Linux TSan、Linux Release、Windows MSVC IOCP；远端 manual `long-soak` run `28986707243` / job `86017363504` 也已在 commit `9b27a0a3c3993cb1f90ef4357fa80027205ca221` 上通过，repeat 20、timeout 60 秒、36/36 threading-labeled tests passed。本地当前 worktree 的 Windows Debug long-soak 已覆盖当前 43-test threading slice：43/43 tests across 20 repeats，CTest reported total test time 637.56 seconds。

第二，**race-oriented gate 已经建好，但它的价值取决于持续跑绿**。当前 TSan/long-soak 的方向是正确的，不过这些 gate 一旦开始发现 flaky/race，就应优先修 core，而不是继续迁移上层模块。

第三，**文档漂移已在本轮收敛**。`migration_status.md` 已从旧的 `ci #17` / `4cdf589` 和 TSan pending 证据刷新到 `ci #23` / `9b27a0a`；README Current Scope 也已显式列出 Logger，避免 core scope 与实际 CMake/tests 不一致。

第四，**远端手动 long-soak 证据已经补齐**。普通 CI、TSan gate、本地 Windows Debug 当前 43-test threading repeat 20，以及 GitHub manual `long-soak` repeat 20 都已有 green evidence；下一步不再是补基础证据，而是保持这些 gate 持续跑绿，并继续做测试支撑收敛和 race hardening。

## 下一步任务方向

### P0：记录最新 HEAD 的完整远端 green

这是当前最重要的任务。最新 HEAD 上这些 job 已经跑绿：

`linux-cmake`、`linux-asan-ubsan`、`linux-tsan`、`linux-release`、`windows-msvc`。

把 run id、commit sha、时间、结果同步到 `docs/migration_status.md`。现在文档已经要求在 Phase 4 之前保持 Linux/Windows CI gate green，并为缺失的本地、soak、race-oriented、platform-specific verification 单独补验证步骤。

### P1：保持 long-soak 作为阶段性 race 证据

`long-soak.yml` 已经存在，默认 repeat 是 20，timeout 是 60 秒，并且 GitHub run `28986707243` 已用默认参数通过。后续可以在高风险 lifecycle/race 改动后再次手动触发；如果默认 repeat 稳定，再视结果提高到 50 或 100。它应该作为 Phase 3.5 hardening 的里程碑证据，而不是普通 PR 必跑项。

### P1：继续压 TcpConnection / TcpClient / TcpServer lifecycle

优先级顺序建议是：

`TcpConnection` 最高。继续盯 pending read/write、forceClose、shutdown pending output、writeComplete ordering、peer close/reset、cross-thread cancellation。这个模块是对象生命周期和 IO completion 最容易出问题的地方。

`TcpClient / Connector` 第二。继续盯 pending ConnectEx stop/destroy、retry timer、cross-thread connect/disconnect/stop，确保 stale completion 不会复活 stopped client。

`TcpServer / EventLoopThreadPool` 第三。继续盯 worker-owned connection teardown、worker callback reentry、active write stop、多 worker stop，以及 thread pool stop/join 收敛。

这些路径已经被大量 contract 覆盖，下一步的重点不是“新增很多相似测试”，而是让这些测试在 TSan 和 long-soak 下持续稳定；本轮已补上 owner/non-owner `TcpConnection::send()` after close ignore 契约，前一轮也补上了 repeated owner/non-owner `TcpClient::stop()` / `connect()` active-connection idempotence 契约和 repeated owner/non-owner `TcpConnection::shutdown()` 在 pending output drain 后只 half-close 一次的契约。

### P1：整理测试支撑代码，防止 lifecycle/race 测试碎片化

migration status 已经记录测试支撑在抽象：`SocketPair.h`、`ClientSocket.h`、`TcpConnectionHarness.h`、`TcpConnectionCallbacks.h`、`LoopTest.h`、`ThreadHandoff.h`、`FutureTest.h`、`TcpClientStopHarness.h`、`TcpServerHarness.h` 分别集中 socketpair/nonblocking/small-send-buffer、非阻塞测试客户端 connect/cleanup、loop-bound TcpConnection construction、callback counting、EventLoop watchdog、one-shot/delayed 非 owner 线程 handoff、EventLoop/EventLoopThread/TimerQueue/EventLoopThreadPool 有界 future wait、retry-stop stale-reconnect harness、多客户端 TcpServer setup 与 worker-loop 分布断言。下一步不是机械新增更多相似 helper，而是在后续 race/lifecycle 改动中继续发现重复就小步收敛，防止 56 个 contract 继续增长后维护成本失控。

### P2：保持文档证据同步

后续文档更新应继续保持小而聚焦：

已将 `assessment.md` 和 `docs/migration_status.md` 的 CI evidence 从 `ci #17` / `4cdf589` 和 TSan pending 改成当前 `ci #23` / `9b27a0a`。

已更新 README Current Scope，明确 Logger 属于 stable/core。

远端 `long-soak` run `28986707243` 的 run id、commit sha、时间、结果已写入 `docs/migration_status.md` 和 `docs/development/ci.md`。

这类 PR 不改核心代码，但能降低后续 agent/维护者误判项目状态的概率。

### P2：给“进入 Phase 4”定义硬门槛

我建议把 Phase 4 的准入条件写成明确 checklist：

最新 HEAD 的 Linux Debug、ASan/UBSan、TSan、Release、Windows MSVC IOCP 全部 green。

手动 long-soak 至少有一次可接受 repeat 数的 green 记录；当前已有远端 run `28986707243`，repeat 20，36/36 threading-labeled tests passed；本地当前 worktree 也已有 Windows Debug repeat 20，43/43 threading-labeled tests passed。

`migration_status.md`、`ci.md`、`windows_iocp_milestone.md` 没有 pending evidence。

TcpConnection / TcpClient / TcpServer 的 lifecycle/race tests 没有已知 flaky。

安装包消费者在 Linux 和 Windows 都通过 `find_package(GameNetCore)` / `GameNet::core` 验证。当前 install/package gate 已经是 CI 范围的一部分。

### P3：Phase 4 的首选入口应是 protocol framing，不是 HTTP/RPC/game pipeline

等 P0/P1/P2 完成后，Phase 4 第一项建议选 **PacketFramer / protocol framing**。理由是它是最靠近 TCP core 的上层抽象，能自然验证 Buffer、TcpConnection message callback、backpressure、frame boundary、malformed packet handling，而不会一下子把 session、logic loop、broadcast、HTTP/RPC 都拉进来。

README 的 planned modules 也是从 protocol framing、transport abstraction、session foundation、logic loop、broadcast foundation、UDP/KCP experimental transport 逐步展开；同时 non-goals 明确说当前项目不是完整 game server framework、不是生产 KCP、不是 AOI/world-state、不是数据库/cache、不是完整 gateway platform。

## 建议的下一批 PR 拆分

我会这样排：

1. **PR A：long-soak evidence PR**
   已触发远端 `long-soak` workflow，并已更新 `migration_status.md` / `docs/development/ci.md` 的远端 soak 证据。

2. **PR B：test support consolidation PR**
   继续收敛 lifecycle/race 测试 helper，降低重复 harness。

3. **PR C：TcpConnection race hardening PR**
   专注 pending read/write forceClose、shutdown pending output、writeComplete ordering 的 TSan/soak 稳定性。

4. **PR D：TcpClient/Connector race hardening PR**
   专注 pending ConnectEx、retry timer、cross-thread stop/connect/disconnect。

5. **PR E：Phase 4 design-only PR**
   只新增 protocol framing intent / invariants / contract plan，不直接大规模实现 HTTP/RPC/game pipeline。

最终判断：**下一步不要急着迁移上层功能；最新 HEAD 的远端 CI、TSan 和 long-soak 证据已经补齐，接下来继续做测试支撑收敛和 TcpConnection / TcpClient / TcpServer race hardening。只有当 Reactor/TCP foundation 的 race/lifecycle evidence 持续稳定后，才进入 protocol framing。**
