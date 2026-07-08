## 总体判断

`game-net-core` 又推进了一层：上一轮的重点是 **Windows IOCP 数据路径落地 + 37 个 lifecycle/race tests**；这一轮的重点已经变成 **TSan/race-oriented gate、54 个 CTest、跨线程生命周期压力覆盖、Linux pending-write 测试修复**。

我现在会把当前状态评估为：

**Phase 2 / Phase 3 已基本完成；项目进入 Phase 3.5 的 core hardening / race hardening / sanitizer hardening 阶段。Phase 4 仍未开始，也不应该现在开始。**

仓库自己的 migration status 仍然明确说 active target 是 Reactor / TCP foundation，高层 protocol、transport、game foundation、experimental 模块继续 deferred，直到 core 有稳定 targets、tests、examples。 Phase 表也仍然显示 Phase 1/2/3 present，Phase 4 deferred。

## 这次的新变化

最新提交已经从上次分析的 `dcff9e8` 前进到 `16969d8`，最近三笔提交分别是：

`204d20c`：`test: harden tcp lifecycle threading gates`
`99e303b`：`fix: stabilize tsan threading contracts`
`16969d8`：`fix: repair linux pending write socket options`。

这组提交的含义很清楚：项目不是继续加高层功能，而是在把 Reactor/TCP 的 **线程亲和、跨线程 API、pending connect/read/write、stop/destroy/forceClose、worker-loop teardown** 这些高风险路径压到 TSan 和 soak/race contract 下。

当前 worktree 已经配置 **54 个 CTest tests**，其中 6 个 unit、47 个 contract、1 个 integration；相比上一轮的 37 个测试，又新增了 17 个 contract，主要围绕 threading/race/lifecycle。

## 最大推进点：加入 Linux TSan race-oriented gate

这是这轮最重要的工程升级。

CI workflow 现在新增了 `linux-tsan` job，名字是 **Linux TSan race-oriented build and tests**；它使用 `GAMENET_ENABLE_TSAN=ON` 配置 Debug build，并且只跑 `threading` label 的 CTest，timeout 为 30 秒。

这意味着项目开始把“线程契约”从普通 contract test 推到 sanitizer 维度。对于一个 Reactor/one-loop-per-thread 网络库，这一步比增加 HTTP/RPC 功能更有价值。因为真正容易出事故的不是 echo 能不能跑，而是：

跨线程 `connect()` / `disconnect()` / `stop()` 是否只 marshal 到 owner loop；
pending `ConnectEx` 被 stop/destroy 后是否还有 stale completion；
pending read/write 被 forceClose 后对象是否仍活到 completion 收敛；
worker-owned connection stop 时，thread pool 是否过早 join；
TimerQueue ready callback 与 cancel race 是否会 double-fire。

migration status 也明确记录了这个 race-oriented CI：新 job 会以 `threading` label 过滤测试，覆盖 cross-thread API、worker-loop scheduling、pending read/write forceClose cancel-close、mixed-timing pending ConnectEx stop、active retry-enabled TcpClient stop-after-peer-close、cross-thread TcpClient connect/disconnect、worker-owned active-write stop、worker-callback TcpServer stop 等路径；但远端 green 证据要等下一次 workflow run。

需要特别说明：当前最新 head `16969d8` 没查到 workflow run。 仓库文档记录的最近远端 green 仍是 **ci #17 / commit `4cdf589`**，包含 Linux CMake、Linux ASan/UBSan、Linux Release、Windows MSVC IOCP 四个 job 成功。 所以最新状态应表述为：**54-test + TSan gate 已进入 worktree；最新 head 的远端 green 还需要下一次 CI run 证明。**

## 测试覆盖已经从 lifecycle 扩到 threading/race 矩阵

`tests/CMakeLists.txt` 现在显示测试矩阵明显加厚：

`EventLoopThreadPool` 增加了 restart soak。
`TcpClient` 增加了 retry stop soak、pending connect stop soak、cross-thread stop pending connect、mixed-timing pending connect stop soak、destroy pending connect、destroy active connection、active connection mixed-timing stop soak、cross-thread active disconnect、cross-thread connect。
`TcpServer` 增加了 multi-worker stop、worker active write stop soak、worker callback stop soak。
`TcpConnection` 增加了 cross-thread pending-read/pending-write forceClose、mixed-timing pending-read/pending-write forceClose soak 等。

这说明项目现在已经不是“主路径 + 几个关闭测试”，而是开始系统化覆盖：

1. owner-loop API 调度；
2. non-owner thread 调用；
3. pending operation cancellation；
4. delayed / immediate / mixed timing；
5. worker-owned connection teardown；
6. stop/destroy 与 callback reentrancy；
7. Linux + Windows 的差异路径。

`test_threading_gate_contracts.py` 还把这些测试强制要求带 `threading` 和 `lifecycle` label，并要求 workflow 中存在 `linux-tsan`、`GAMENET_ENABLE_TSAN=ON`、`ctest ... -L threading --timeout 30`。 这很关键：不是“写了测试但 TSan 不跑”，而是把高风险测试挂进专门的 race-oriented gate。

## TcpClient / Connector 方向：pending connect 与跨线程 API 继续硬化

`TcpClient` 现在对析构 active connection 的处理更严谨：析构时重置 lifetime token，取出 connection，清空 close callback 并改成只做 `connectDestroyed()`，然后在 connection loop 上 forceClose 或 connectDestroyed；同时 stop connector。

`connect()`、`disconnect()`、`stop()` 都是捕获 lifetime token 后通过 `loop_->runInLoop()` marshal 到 owner loop。 这和当前新增的 cross-thread tests 是一致的：例如 `test_tcp_client_cross_thread_connect.cpp` 明确从非 owner thread 调用 `client.connect()`，并断言连接回调仍然运行在 owner loop，连接断开后 client connection 为空、server connectionCount 为 0。

这种测试不是简单补覆盖率，而是在确认 API surface 的线程承诺：用户可以从非 owner thread 请求 connect/disconnect/stop，但真正状态机只能在 owner loop 上前进。

## TcpConnection 方向：pending write / pending read / forceClose 混合时序

这轮还明显加强了 pending-write 测试质量。最新提交 `16969d8` 专门修复 Linux pending write 测试中的 socket option：给 pending-write socketpair 测试补上 `SO_SNDBUF`，保证小发送缓冲真正生效，从而更稳定地制造 pending write。

`test_tcp_connection_force_close_pending_write_mixed_timing_soak.cpp` 展示了现在测试的复杂度：每轮建立 socketpair，设置非阻塞和小 send buffer，发送 8MB payload，然后用 4 种模式交错触发 owner/non-owner、immediate/delayed `forceClose()`，最终要求 connected/disconnected/close callback 都只发生一次且 connection 被释放。

这正是网络库最难的部分：不是“能写完”，而是“写不完、正在写、另一个线程关闭、completion 晚到、对象准备销毁”的情况下是否仍然单次收敛。

## EventLoopThreadPool 方向：stop/join 语义修正

上一轮 EventLoopThreadPool 已经有 queued-work soak；这轮源码修了 `stop()`：现在 `stop()` 明确要求在 base loop 线程调用，并逐个调用 `EventLoopThread::stop()`，而不是只对 worker loop 调 `quit()` 后直接清空。

这个改动很有价值，因为 thread pool stop 的核心不是发 quit 信号，而是 **join / 生命周期收敛**。如果只 quit 不 join，TSan 和长期压测都很容易暴露 worker thread 与 pool teardown 的竞态。`test_event_loop_thread_pool_contracts.py` 也把这一点写进 guard：要求源码里出现 `baseLoop_->assertInLoopThread();` 和 `thread->stop();`。

## Windows IOCP 当前状态

Windows IOCP 方向没有退回；相反，这轮文档把 54/54 本地 Windows CTest 证据同步进 milestone。Windows milestone 记录：本地 VS2026 Debug build 成功，CTest 10 秒 timeout 下 54/54 passed，0 failing tests。

Windows 覆盖也比上一轮更完整：

EventLoop wakeup 通过 `PostQueuedCompletionStatus`，不再依赖默认 poll timeout；
EventLoopThreadPool worker loops 有 queued-work soak 和 restart-stop soak；
TcpServer stop 覆盖 active write、multi-worker、worker-callback stop reentry；
TcpClient 覆盖 pending ConnectEx stop、cross-thread stop、destroy pending/active、retry stop、cross-thread connect/disconnect；
TcpConnection 覆盖 pending read/write、cross-thread forceClose、cross-thread shutdown、pending-output shutdown 等。

但这仍是 **local evidence + workflow gate 配置**，不是最新 head 的远端 green。Windows workflow 仍然是必须保持的 gate。

## CI 与 scope 边界

`src/core/CMakeLists.txt` 仍然只有 `gamenet_core` 目标，源文件也仍然是 core/net/base 与平台后端；Windows 分支是 `SocketsOps_win`、`Wakeup_win`、`IocpSocketOps_win`、`IocpTcpTransport_win`、`IocpPoller`，非 Windows 是 Linux socket/wakeup/EPollPoller。没有 protocol、transport、game、HTTP、RPC、KCP 等模块进入 active target。

CI workflow 也继续保持 deferred modules disabled：各 job 都显式设置 `GAMENET_ENABLE_TLS=OFF` 和 `GAMENET_ENABLE_EXPERIMENTAL=OFF`；TSan job 也是只针对当前 core target 的 threading label。

这说明项目没有因为测试变多而 scope 失控。它仍然是在打磨 core。

## 与 mini_trantor 的关系

`mini_trantor` 是大而全的实验框架：包含 Reactor/TCP、coroutine bridge、HTTP/WebSocket/RPC、DNS/TLS/IPv6、TCP/UDP/KCP transport、PacketFramer/CodecAdapter、SessionManager/LogicLoop/GameServerPipeline、broadcast/metrics 等。 它的成熟度矩阵也承认不少模块仍需要 close race、half-close、soak、backpressure、DNS race、fuzz corpus、重连窗口压力等证据。

`game-net-core` 现在不是在“追平 mini_trantor 的功能列表”，而是在把 mini_trantor 的最底层网络核心抽出来重新做成更可靠的 foundation。尤其是 Windows 后端和 TSan/threading gate，已经明显走向比原仓库更硬的工程化路线。

## 当前成熟度评估

**Reactor / EventLoop / TimerQueue：Stable-leaning。** EventLoop wakeup、TimerQueue ready-cancel race、owner-loop timer execution、cross-thread scheduling 都已有明确 contract；现在又进入 TSan threading label。
**EventLoopThreadPool：Stable-leaning，但还需远端 TSan green。** stop 已改为 owner-loop assert + thread->stop，queued-work soak 与 restart-stop soak 都已进入测试。
**TcpConnection：Beta+，接近 Stable 的路上。** read/write/close 主路径已强，pending read/write forceClose、cross-thread shutdown、pending output、mixed timing soak 都在补齐；但这个模块是最高风险区，仍需 TSan/长期 soak 反复跑。
**TcpClient / Connector：Beta+。** pending ConnectEx stop/destroy、cross-thread connect/disconnect、retry stop 都在硬化；真正升级到 Stable 需要最新 TSan job 和 Windows workflow 持续 green。
**TcpServer：Beta+。** multi-worker stop、worker active write、worker callback reentry 都开始覆盖，这是服务端生命周期的重要跃迁。
**Windows IOCP backend：Beta，方向非常正确。** 数据路径和 cancel/close 模型已经形成，但仍需要更多远端 CI 与长跑证据。
**Phase 4：未开始。** 当前不应急着迁移 PacketFramer/transport/session，更不应直接上 HTTP/RPC/game pipeline。

## 当前风险

第一，**最新 head 还没有远端 workflow run**。`16969d8` 未查到 workflow run，最新完整远端 green 仍停在 ci #17 / `4cdf589`。

第二，**TSan job 是正确方向，但还缺 green 证据**。migration status 明确说 remote green evidence for this new job is pending until the next workflow run。

第三，**测试规模已经快速膨胀到 54 个，后续要防止测试碎片化**。现在 contract 数量很多，建议下一步把常用 socketpair、nonblocking、small send buffer、loop runner、callback counter 抽成 tests/support 工具，避免每个测试复制一份辅助逻辑。

第四，**mixed-timing soak 还偏短**。目前很多 soak 是 8 次或轻量循环，适合作为 CI smoke/race gate，但不能替代长跑 soak。后续最好有 nightly 或手动 workflow 跑更大 iteration。

## 下一步建议

当前最优先的下一步不是 Phase 4，而是：

1. 触发最新 head 的完整 CI，尤其确认新增 `linux-tsan` job 是否 green。
2. 如果 TSan green，再把 migration status 从 “pending remote green evidence” 更新为最新 run 结果。
3. 抽象 tests/support，降低 lifecycle/race 测试重复代码。
4. 增加一个非默认的 long-soak workflow，扩大 mixed timing iteration，不阻塞普通 PR，但用于阶段性稳定性证据。
5. 保持 Phase 4 延后。等 Linux Debug/ASan/TSan/Release + Windows IOCP Debug 全部稳定后，Phase 4 首选仍应是 **PacketFramer / protocol framing**，而不是 HTTP/RPC/game pipeline。

## 最终评价

`game-net-core` 当前已经从“拆出 Reactor/TCP core”推进到 **可验证、可安装、双平台、race-oriented 的 core foundation**。这次最大的进步不是代码量，而是验证质量：54 个 CTest、47 个 contract、TSan threading gate、Windows IOCP cancel/close 文档与测试矩阵，说明项目正在朝“可靠底座”而不是“功能堆叠”前进。

我的判断是：

**当前进度：Phase 2/3 完成度很高，Phase 3.5 hardening 正在快速推进。**
**当前方向：继续压实 Reactor/TCP lifecycle、跨线程 API、TSan threading gate、IOCP cancel/close，而不是提前迁移上层模块。**
**当前关键门槛：最新 head 的 Linux TSan + Windows IOCP + Linux ASan/Release 全部远端 green。**
