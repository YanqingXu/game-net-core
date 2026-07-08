## 总体判断

`game-net-core` 这轮进展的重点已经从“Windows IOCP 数据路径落地”继续推进到了 **Reactor/TCP 生命周期硬化**。也就是说，它不是进入 Phase 4 去迁移 protocol / transport / game 模块，而是在把 Phase 2/3 的 core foundation 做得更接近“可长期承载上层模块”的状态。

我现在会把它评估为：

**Phase 2 / Phase 3 基本完成；当前处于 Phase 3.5：core hardening、race hardening、Windows IOCP cancel/close hardening；Phase 4 仍然明确未开始。**

迁移状态文档仍然明确说当前 active execution target 是 Reactor / TCP foundation，protocol、transport、game foundation、experimental 仍 deferred，直到 core 有稳定 targets、tests、examples。 Phase 表也显示 Phase 1/2/3 已经 present，而 Phase 4 仍是 deferred。

## 最新进展概览

最新提交记录显示，当前 head 是 `dcff9e886ceaad3ae983fd5fdf88d034c7a5057e`，提交信息为 **`test: harden reactor tcp lifecycle`**。它位于上一轮看到的 `5e3c84b...` 之后。 从 `5e3c84b` 到当前 head 只 ahead 2 个提交，但改动密度很高：新增/修改了 TCP lifecycle tests、TimerQueue contract、EventLoopThreadPool contract、Connector/TcpConnection/TcpServer 的 cancel/close 逻辑、Windows IOCP milestone 文档和 CI guard。

当前 worktree 已经配置 **37 个 CTest tests**，结构是 6 个 unit、30 个 contract、1 个 integration。相比上一轮 29 个测试，新增的 8 个基本都集中在 race、soak、cross-thread、pending operation cancellation 这些真正危险的生命周期区域。

需要注意一个细节：当前 `dcff9e8` 这个最新提交本身没有查到 workflow run；仓库文档记录的最新 remote green 是 **ci #17 for commit `4cdf589` on main**，包含 Linux CMake、Linux ASan/UBSan、Linux Release、Windows MSVC IOCP 四个 job 全成功。  所以结论应表述为：**当前代码已经把 37-test hardening 写入 worktree 和 guards；最新完整远端 CI green 证据停在 ci #17 / `4cdf589`，而不是最新 `dcff9e8`。**

## 最大变化：从“IOCP 主路径可跑”进入“IOCP cancel/close 语义可控”

上一轮最大变化是 IOCP 数据路径：`AcceptEx`、`ConnectEx`、`WSARecv`、`WSASend` 进入 core TCP 路径。现在的关键变化是：**pending ConnectEx / pending WSARecv / pending WSASend 的取消和销毁顺序开始被显式建模与测试。**

`Connector` 现在有 Windows 专用的 `cancelPendingConnectInLoop()`、`finishCancelInLoop()`，并引入 `connectStopGuard_` 来保证 pending ConnectEx 取消完成前对象仍存活。 在 `stopInLoop()` 中，如果处于 `kConnecting` 并且能取消 pending connect，就先返回等待完成包；完成路径进入 `handleWrite()` / `handleError()` 时，如果处于 canceling 或 `!connect_`，会走 `finishCancelInLoop()`，而不是误把 stale completion 当作成功连接。   Windows 下实际调用 `CancelIoEx`，并处理 `ERROR_NOT_FOUND` / `ERROR_INVALID_HANDLE` 这类竞态结果。

`TcpConnection` 也新增了 `forceClosePending_` 和 `forceCloseGuard_`，并把关闭过程拆成 `handleClose()` 与 `finishClose()`。  Windows 下如果 IOCP transport 仍有 pending operations，`handleClose()` 不直接销毁，而是进入 pending close 状态、设置 guard、调用 `cancelPendingOperations()`，等待 completion 回来后再 `finishClose()`。 `handleRead()` / `handleWrite()` 在 completion 后如果发现 `forceClosePending_`，会再次进入 `handleClose()` 完成关闭收敛。

`IocpTcpTransport` 也从“能 WSARecv/WSASend”升级到“能报告 pending 状态并取消 pending operations”。它新增了 `hasPendingOperations()` 和 `cancelPendingOperations()`，后者对 read/write overlapped 调用 `CancelIoEx`。

这是一类非常关键的改动，因为 IOCP 的核心风险从来不是“能否收到 completion”，而是 **关闭、取消、对象生命周期、completion 晚到** 是否能统一收敛。当前方向明显抓住了这个核心风险。

## TCP lifecycle 测试覆盖明显变硬

`tests/CMakeLists.txt` 当前已经把 lifecycle/race 测试扩展到了 37 个配置测试，其中新增的重点包括：

`TcpClient`：`retry_stop_race`、`retry_stop_soak`、`stop_pending_connect`。
`TcpServer`：`stop_active_connections`、`stop_active_write`、`stop_soak`。
`TcpConnection`：cross-thread send、cross-thread shutdown、cross-thread forceClose soak、forceClose pending read、shutdown pending output、write-complete ordering、peer close/reset、high-water、repeated forceClose。

这些不是装饰性测试。比如 `test_tcp_client_stop_pending_connect.cpp` 明确验证：`client.stop()` 必须取消 in-flight `ConnectEx`，即使稍后 server start，也不能接受 stale completion 形成连接。

`test_tcp_server_stop_active_write.cpp` 验证 server 在连接建立后发送 8MB payload，然后立即 `server.stop()`，最终必须只收到一次 disconnect callback，连接计数归零。 这对 backpressure / pending output / stop reentrancy 都是重要边界。

`test_tcp_connection_cross_thread_send.cpp` 验证从非 owner thread 调用 `send()` 时，payload 必须复制并经 EventLoop marshal，写入仍发生在 owner loop，且用户 callback 不得跑到错误线程。 最终断言包括 write-complete 一次、peer 读到完整 payload、close callback 一次、connection disconnected。

`test_tcp_connection_force_close_pending_read.cpp` 验证在 pending read 存在时 `forceClose()` 仍能收敛到单次 connected/disconnected/close callback，并且 connection 被释放。 这正好对应 IOCP pending read cancellation 的高风险点。

## EventLoopThreadPool 和 TimerQueue 也从“基本正确”进入 race/soak 覆盖

这轮不只是 TCP 测试增加了。CI guard 新增了 `test_event_loop_thread_pool_contracts.py` 和 `test_timer_queue_contracts.py`。workflow 四个 job 都会在 CMake configure 前运行这些 guard。

`EventLoopThreadPool` 的 contract guard 要求覆盖“cross-thread queued work reaches each published worker loop”。 实际 C++ 测试里，4 个 submitter 线程向 3 个 worker loop 各提交 16 次任务，断言所有任务都在 worker loops 执行，base loop 执行次数为 0。 这比“能启动两个线程，round-robin 返回 loop”强很多。

`TimerQueue` 的新增契约是 ready-timer cancellation race：一个 ready callback 取消同一批 ready timer 中后一个 timer，要求后一个 callback 不触发。 实际测试里两个 timer 使用同一 `when`，第一个 callback cancel 第二个，然后最终断言 `firstFired` 为 true、`secondFired` 为 false。 这对 timer queue 的 reset/cancel/inQueue 状态机非常关键。

## CI 和工程护栏继续加厚

CI 文档现在明确把 EventLoopThreadPool queued-work race/soak、TimerQueue ready-timer cancellation race、Windows IOCP milestone/data-path、intent consistency、scope boundary、install package、ASan/UBSan、Release、Windows MSVC Debug + install consumer 都放进 gate 范围。

同时 deferred modules 仍然关闭：TLS、experimental transport、HTTP、WebSocket、RPC、UDP、KCP、PMTU/FEC、metrics、game pipeline 都没有进入 active scope。 文档还强调如果 active implementation/test surface 出现 deferred high-level module name，workflow 会失败；Phase 4 promotion 必须同步更新 active scope 和 migration status。

这说明项目方向很清楚：**继续把 core 打硬，而不是被 mini_trantor 的上层模块诱惑提前膨胀。**

## 与 mini_trantor 的关系再评估

`mini_trantor` 仍然是更完整的实验框架：它包含 coroutine bridge、HTTP/WebSocket/RPC、DNS/TLS/IPv6、TCP/UDP/KCP transport、PacketFramer、CodecAdapter、SessionManager、LogicLoop、GameServerPipeline、broadcast/metrics 等。 它的模块成熟度矩阵也承认很多区域仍需 race、soak、fuzz、backpressure、长跑压测。

`game-net-core` 现在不是追求“功能上赶上 mini_trantor”，而是在做更底层的工程反向收敛：把 `mini_trantor` 中最核心、最容易被上层复杂度污染的 Reactor/TCP/loop/thread/timer/socket 基础抽出来，以更小 target、更强 contract、更清晰 package 和更严格 CI 重新证明。

尤其 Windows 方向上，`mini_trantor` README 仍描述 Windows 使用 `SelectPoller` preview。 而 `game-net-core` 已把 Windows active source selection 指向 IOCP backend，并记录 IOCP path 覆盖 `PostQueuedCompletionStatus`、`AcceptEx`、`ConnectEx`、`WSARecv`、`WSASend`。 这意味着拆出来的 core 不只是“子集”，而是在部分底层平台能力上已经开始反超原仓库的工程质量。

## 当前成熟度评估

我会按模块给一个实际判断：

**EventLoop / Channel / Poller / TimerQueue：接近 Stable。** EventLoop 主路径、cross-thread wakeup、TimerQueue ready-cancel race、Poller backend abstraction 都有明确契约。TimerQueue 的同批 ready timer cancellation 测试是一个质量跃迁。

**EventLoopThread / EventLoopThreadPool：接近 Stable，但还需要更长 soak。** 当前已有 worker-loop queued-work soak，能证明 published worker loops 的 thread-affinity 和 cross-thread queueing，不再只是启动/停止测试。

**TcpServer / TcpClient / TcpConnection / Connector：Stable-leaning Beta。** 主链路、关闭路径、cross-thread send/shutdown/forceClose、pending output、pending read、retry stop、pending connect 都有 contract；但 TCP lifecycle 本身是高风险区，我会等更多 long soak、TSan/race-oriented evidence 后再称 full Stable。

**Windows IOCP backend：Beta，方向正确。** 它已经不是 skeleton，已经覆盖 accept/connect/read/write/wakeup/install consumer；但 IOCP cancel/close 的长期 race 仍然需要更多重复/压力/延迟注入测试。当前已开始处理 `CancelIoEx`、pending completion 和 lifetime guard，这是正确方向。

**Phase 4 模块：仍未开始，不应提前。** Protocol framing、transport abstraction、session、logic loop、broadcast、UDP/KCP 等仍应等待 core 的 Linux/Windows gate 持续稳定。

## 当前风险

第一，**最新 head `dcff9e8` 尚无对应 workflow run**。文档记录了 ci #17 / `4cdf589` green，但当前 37-test hardening 的最新提交未查到 workflow run。 所以当前应避免说“最新 head 已远端 green”；更准确是“最新 worktree 已配置 37 tests，最近完整远端 baseline 是 ci #17 green”。

第二，**IOCP cancel/close 语义虽然开始成型，但仍是最危险区域**。`CancelIoEx` 后 completion 仍可能回来，对象必须靠 guard 活到 completion 收敛；当前实现已经做了 guard，但还需要更多随机化、重复、压力测试来证明没有偶发 late completion/use-after-free。

第三，**测试数量增长快，下一步需要避免 contract guard 变成字符串检查自嗨**。现在 Python guard 会检查测试/文档/intent/CMake 是否包含指定 fragment；这适合防止结构漂移，但真正的质量仍来自 C++ contract 在多平台、sanitizer、soak 下长期跑。也就是说 Python guard 是元护栏，不应替代运行时 race 证据。

第四，**还没有看到 TSan 作为稳定 gate**。Linux ASan/UBSan 和 Release 已有，但这个项目的风险核心是线程亲和、跨线程投递、生命周期和 callback reentrancy；未来 TSan 或替代的 race-oriented CI 会非常有价值。

## 当前方向判断

当前方向非常健康，而且比上一轮更清晰：

**它正在把 game-net-core 做成一个“Linux epoll + Windows IOCP 双后端、以 lifecycle contract 为中心、可安装消费的 C++23 Reactor/TCP foundation”。**

最合理的下一步不是 Phase 4，而是继续补这些证据：

1. 让 `dcff9e8` 这批 37-test hardening 通过完整远端 CI，并更新 migration status。
2. 增加 IOCP pending read/write/connect cancel 的重复压力测试，最好带随机时序、短 timeout 和多轮 iteration。
3. 给 `TcpServer::stop()`、`TcpClient::stop()`、`TcpConnection::forceClose()` 做更长 soak，覆盖 worker-owned connections 和 base-loop shutdown ordering。
4. 逐步引入 TSan 或可替代的 race CI。
5. 当 core 连续稳定后，Phase 4 首选迁移 **PacketFramer / protocol framing**，而不是 HTTP/RPC/game pipeline，因为它最贴近 TCP core、最容易保持边界纯净。

## 最终评价

`game-net-core` 当前已经不是“刚拆出来的 core 仓库”，而是一个正在快速成型的 **网络核心基础库**。它的最大价值不是功能多，而是边界硬：scope guard、intent guard、install package、Release-safe tests、ASan/UBSan、Windows IOCP gate、lifecycle/race contract 都在围绕同一个目标服务。

我的评估是：

**当前进度：Phase 2/3 基本完成，Phase 3.5 core hardening 正在进行。**
**当前方向：继续压实 Reactor/TCP lifecycle、IOCP cancel/close、TimerQueue/EventLoopThreadPool race，而不是提前迁移上层模块。**
**当前短板：最新 head 还缺远端 CI 结果；IOCP cancel/close 需要更长时间和更随机化的 race/soak 证据；TSan/race gate 仍值得补。**
