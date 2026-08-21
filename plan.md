# game-net-core 完整后续执行计划：IOE-X10 至 v1.0

计划重制日期：2026-08-22

长期方向：`goal.md`

当前评估：`assessment.md`

前序治理检查点：`a5ff7e6d823984a86e89146889f29f6615702ec3`

M1 实现与证据检查点：`f5d39b800b4dd943531670aa09840c931c3dee4d`
（IOE-X10；固定协议原始证据绑定该提交）

## 1. 计划定位与总体顺序

本计划覆盖从当前 IOE-X10 前沿到 v1.0 的完整、证据门控路线。已经关闭的
M0–M6、IOE-R1/R2/C1、IOE-X1–X9 和 Runtime Profile A/B/C/D 不再逐项展开；
完整历史结论分别见：

- `docs/migration_status.md`；
- `docs/architecture/runtime_profile_common_capability_review.md`；
- `docs/development/commit_bound_evidence_ledger.md`。

总体顺序固定为：

```text
M1  IOE-X10 + ARCH-G1
M2  v0.3 内部候选
M3  真实网关验证
M4  v0.3 外部发布
M5  v0.4 Runtime 边界
M6  v0.5 io_uring 实验后端（条件执行）
M7  v0.6 Lua / RPC
M8  v0.7 Async / Coroutine
M9  v0.8 TLS / WebSocket / DNS
M10 v0.9 UDP / KCP 实验能力
M11 v1.0 稳定发布
```

当前唯一实现前沿是 **M2 v0.3.0 内部候选**。
同一时刻只允许一条 Core 实现主线；一个 Core 外部网关集成切片和一个持续证据任务
可以并行。每个条件分支必须明确记录执行、`NO-PROMOTION`、`DEFER` 或
`skipped-by-evidence`，不得以“后续再决定”结束。

## 2. 当前事实与全程边界

- `a5ff7e6d823984a86e89146889f29f6615702ec3` 是进入 M1 前的治理检查点；
- `f5d39b800b4dd943531670aa09840c931c3dee4d` 已实现 IOE-X10 固定协议、
  结构化比较、验证器和 ARCH-G1 fix-forward；
- 当前仓库留存的默认测试基线为 129（8 unit、107 contract、14 integration），
  Linux experimental 基线为 136（8 unit、114 contract、14 integration）。这些是
  已记录证据，不表示本次计划重制重新执行了测试；
- Profile A/B/C/D 的共同能力审查结论保持 `NO-PROMOTION`；
- main 持续前进，证据绑定精确 commit，需要推广时再选择 promotion commit；
- EventLoop 继续是单 owner scheduler / event pump，Readiness 与 Completion 保留
  各自真实语义；
- owner、ownership、re-entry、cross-thread marshal、typed failure 和 shutdown
  必须先在 active intent、rules 和 contract 中明确；
- epoll 和 IOCP 是 v1 稳定后端；io_uring、UDP/KCP 最多进入显式 opt-in 的实验面；
- 不通过无界队列、operation、buffer、mailbox 或 final drain 换取表面吞吐；
- Lua、Actor、Room、AOI、World、数据库和部署控制面不得进入 `GameNet::core`；
- 完整 HTTP server、raw ICMP、FEC、高级拥塞控制、zero-copy 和跨进程 Session
  migration 不进入 v1 范围。

历史 REL-C1、REL-V1、candidate tag 和 refreeze 记录保持不可变，只作为历史证据。
`v0.3.0-rel-c1-refreeze-5` 及其替代的
`v0.3.0-rel-c1-refreeze-4@c061f9967b9481b70b2faf9a8fee24f5a3e72ffc`
不再是开发冻结点。

## 3. M1：IOE-X10、ARCH-G1 与治理统一

优先级：P0。

M1 状态：已关闭（2026-08-22）。

### 3.1 IOE-X10 固定合同

在 benchmark 实现前，先在 active I/O Engine intent 和 testing rules 中加入 X10
固定测量合同。X10 不改变运行时语义；若测量暴露 correctness 或 lifecycle 缺陷，
必须另开合同先行的 fix-forward 切片，修复后重新执行全部样本。

| 参数 | 固定值 |
| --- | ---: |
| 并发 active routes | 256 |
| `maxPendingAccepts` | 32 |
| Hub route 上限 | 256 |
| churn | 4 波，每波替换 64 routes |
| 每连接 echo round trips | 100 |
| payload | 64 bytes |
| warm-up | 每个 backend 1 次完整但不计入结果的运行 |
| 正式样本 | 每个 backend 5 次 Release 样本，交错运行 |

同一个 listener benchmark 场景驱动 source-private io_uring listener 与 production
epoll `TcpServer`。固定同一机器、CPU affinity、构建配置、client count、payload 和
采样顺序，并记录 CPU、内核、编译器、构建类型和 affinity。

每个结构化样本必须记录并校验：

- connect、echo、close 完成数与完成率；
- throughput、P50/P99/P999 latency；
- fd、active route、Accept/Recv/Send operation 高水位；
- Engine-owned bytes、pending send bytes、process RSS/working set；
- capacity rejection、SQ rejection 及拒绝后的恢复时间；
- listener stop、Hub/server shutdown 延迟；
- listener、route、operation、notice、fd、pending byte 和 Engine-owned byte 最终残留。

correctness、资源核算、恢复、owner/lifecycle、零残留或五次正式样本验证任一失败即
`DEFER`。全部通过后才可根据中位数和 tail 分布给出 `PROMOTE`；`PROMOTE` 只授权后续
source-private shaping，不表示 io_uring 全面优于 epoll，也不开放公共 backend selector。
环境不能完成固定协议时直接记录 `DEFER`，不得静默降低负载后宣称通过。

### 3.2 ARCH-G1 独立审查

独立 reviewer 按 intent、public contract、invariants、thread affinity、ownership、
lifecycle、implementation、test completeness 的顺序审查，并输出
`docs/reviews/arch-g1-independent-review.md`。审查必须回答：

1. I/O Engine seam 是否降低具体 backend 耦合；
2. EventLoop 还残留哪些 readiness/completion 兼容职责；
3. stable 0.3 API/layout 中哪些兼容字节或 ABI slot 必须保留到 breaking line；
4. Runtime Profile 是否向 Core 泄漏 placement、tick、shard 或业务策略；
5. io_uring Hub/Adapter 是否复制了过多 production `TcpConnection` 逻辑；
6. 结合 X10，应继续 source-private integration 还是暂停实验。

结论只能是 `APPROVE` 或 `REQUEST-CHANGES`。生命周期、所有权、线程亲和和 stable API
blocker 不允许 waiver；必须 fix-forward、补合同并复审。

### 3.3 决策分支

```text
X10=DEFER 或 ARCH-G1 要求暂停
    -> 冻结 io_uring 功能扩展
    -> X1–X9 合同继续进入 CI 维护
    -> M6 标记 skipped-by-evidence
    -> 继续 M2–M5 和 M7

X10=PROMOTE 且 ARCH-G1=APPROVE
    -> 记录 IOE-X11–X15 授权
    -> 仍先完成 v0.3 推广和真实网关验证
    -> 到 M6 再实施生产等价塑形
```

实际分支为 `X10=PROMOTE 且 ARCH-G1=APPROVE`。`PROMOTE` 仅授权到 M6 时继续
source-private IOE-X11–X15 塑形；它不表示 io_uring 全面优于 epoll，也不改变默认
后端、安装面或 stable API。固定证据保存在
`docs/development/benchmark_results/2026-08-22-ioe-x10-f5d39b8/`，独立复核保存在
`docs/reviews/arch-g1-independent-review.md`。

中位数显示 io_uring 的 RTT/吞吐约为 epoll 的 0.499，P50/P99 分别约为 2.233/
1.396 倍，P999 约为 0.765 倍，RSS 约为 0.954 倍。因此后续塑形必须保留吞吐、
P50 和 P99 性能债务，不能把限定范围的通过改写为“更快”。

### 3.4 M1 关闭门

- X10 已输出限定范围的 `PROMOTE` 或 `DEFER`；
- ARCH-G1 为 `APPROVE`，或所有 blocker 已关闭并复审通过；
- README、roadmap、migration status、plan 和 evidence ledger 只有一个当前前沿；
- Linux experimental focused/repeat/sanitizer 与默认 Linux/Windows 回归门通过；
- stable API manifest 无变化；
- 没有引入 selector、production backend replacement 或高级 io_uring feature。

关闭结果：X10 为限定范围的 `PROMOTE`；ARCH-G1：`APPROVE`；固定协议 10 个正式
样本全部有效且 SHA-256 受治理守卫校验；Linux experimental 136/136、focused 重复、
ASan/UBSan 与经 `setarch x86_64 -R` 规避 WSL ASLR 映射冲突后的 TSan 均通过；
默认 Linux/Windows 回归与 stable API zero-diff 通过。Windows 完整门发现并修复了
Profile C cadence-stop 同一退休被重复计数的问题，新增合同证明取消投递可为 0/1，
但结果恰好发布一次。

## 4. M2：v0.3.0 内部候选

优先级：P1。启动条件：M1 关闭。

状态：执行中。M1 关闭提交推送后，从该 main 检查点选择 promotion commit；在证据
完成前不展开 IOE-X11 或新的 Runtime 功能。

- 选择 main 上一个精确 promotion commit，停止展开新的 IOE/Runtime 功能；
- 完成 Linux Debug/Release、ASan/UBSan、TSan、Windows Debug/Release/IOCP；
- 完成 focused repeat、repository/scope/intent guards、install consumers 和 API manifest；
- 完成 paired benchmark、capacity、fault injection、24h 和 72h endurance；
- 生成内部 package、SBOM、third-party notices 和完整 evidence bundle；
- waiver 只能记录证据缺失，不能计为通过；
- 任一生命周期、API、容量或 endurance 门失败即 `NO-GO`，修复后选择新 promotion
  commit 全量重验。

输出命名为 `v0.3.0-internal-candidate.1`，不声明外部可采用。证据期间 main 可以继续
开发；promotion evidence 只绑定被验证的精确提交，运行时或工具变化时不 refreeze
旧候选。

## 5. M3：`gamenet-game-gateway` 真实集成

优先级：P1。启动条件：M2 形成内部候选。

在独立仓库中使用安装后的 GameNet targets，禁止依赖 source-private 或 non-installed
helper。实现相同业务场景的两种运行模型：

1. Queued Event：多 I/O owner、独立有界逻辑执行域；
2. Sharded Hybrid：连接 owner 不迁移，命令按 player/room/scene key 进入逻辑 cell。

共同流水线：

```text
TcpServer
-> PacketFramer
-> Auth / SessionManager
-> bounded logic/shard executor
-> Lua execution cell
-> BroadcastDispatcher
-> TcpTransportEndpoint
```

必须验证：

- 鉴权成功、失败、超时和重复登录；
- Session generation、断线、重连和踢下线；
- Lua 阻塞和异常不阻塞网络 owner；
- logic/shard queue、输出和广播饱和后的 typed 降级与恢复；
- 慢客户端、分片广播和 callback re-entry；
- 网络、逻辑、Lua、广播和持久化边界的确定性关闭顺序；
- 真实流量回放、故障注入和至少一次 24h 集成运行。

集成反馈账本必须把问题分为：Core correctness blocker、缺少的通用能力、仅属于网关/
业务的策略、API 易用性问题、性能或内存问题。只有前两类可以触发 game-net-core
变更，并且仍须经过 active intent、rules、contract 和兼容性审查。

## 6. M4：Apache-2.0 下的 v0.3.0 外部发布

优先级：P1。启动条件：M3 关闭通用 Core blocker。

- 只修复 M3 暴露的通用 Core blocker，不把 Lua、Room、Actor、RPC 或部署拓扑引入 Core；
- 顶层许可证切换为 Apache-2.0，并同步源码 header、README、package metadata、NOTICE、
  third-party notices 和 SBOM；
- 审计第三方来源、生成代码、测试语料和 benchmark 资产的许可证兼容性；
- 若 M3 后存在运行时变化，重新选择 promotion commit 并完整执行 M2 同提交矩阵；
- 增加从干净安装包构建的 Linux/Windows 外部消费者和 0.2/0.3 升级消费者；
- 发布 `v0.3.0`、校验和、源码包、二进制包、SBOM、已知限制和证据索引。

v0.3 稳定承诺仅覆盖现有安装目标和 TCP epoll/IOCP 路径。Runtime Profiles 与
io_uring 不属于 v0.3 stable API。

## 7. M5：v0.4 Runtime 边界

优先级：P2。启动条件：M3 的 Queued Event 与 Sharded Hybrid 都有真实证据。

重新执行跨 Profile 共同能力审查。允许提升的候选仅限：

- 复用现有 `TransportEndpoint`；
- typed、有界的 `LogicExecutor` admission；
- 可等待且单调的 `RuntimeStopFuture`；
- 只有两个实现语义一致时才加入 shard/cadence 类型。

明确禁止 `UniversalGameServer`、`RuntimeProfileFactory`、
`AnyTransportAnyLogicRuntime` 和每种策略组合一个 Server 类。

```text
两个真实实现存在相同 owner/lifecycle/admission 需求
    -> 激活 Runtime public-surface intent
    -> 每个公共概念单独合同、实现和 API review
    -> 形成 v0.4.0

语义仍然不同或只有名称相似
    -> 记录第二次 NO-PROMOTION
    -> Profile 继续作为官方 recipe/example
    -> 不创建占位公共抽象
```

无论是否提升 API，都发布 Profile A/B/C 的负载选择指南，记录连接数、包频率、逻辑
成本、tick、handoff、广播和背压适用条件。Profile D 继续 provisional，直到真实
sharding 证据完整。若没有公共能力被提升，不发布空版本；后续可交付能力接管 v0.4
版本号。

## 8. M6：v0.5 io_uring 可安装实验后端

仅在 `X10=PROMOTE` 且 ARCH-G1 批准时执行；否则整个 M6 标记为
`skipped-by-evidence`，直接进入 M7。

### 8.1 IOE-X11：单 owner Server composition

- 将现有 listener、Hub 和 semantic adapter 组合成 source-private 单 owner server；
- 对齐 bind/listen、callback、admission、graceful stop 和 force escalation；
- 保持 epoll 生产默认，不修改 production `TcpServer`。

### 8.2 IOE-X12：多 owner topology

- accept owner 取得 fd 后，通过有界 EventLoop admission 将 sole ownership 转移给
  选定 worker Hub；
- post、admission 或 worker shutdown 失败时，由当前 sole owner 精确关闭 fd；
- 保持 RoundRobin、LeastConnections、QueueLag 和 ConsistentHash 的连接放置语义；
- 已建立连接不迁移 owner，每个 worker 独占自己的 Hub/Pump。

### 8.3 IOE-X13：Connect/TcpClient

- 增加 one-shot Connect operation；
- 覆盖 timeout、retry、cancel、stale attempt、callback re-entry 和 owner quit；
- 完成 source-private client adapter 与 production `TcpClient` 观察序列对比。

### 8.4 IOE-X14：跨后端语义套件

- 同一 server/client 合同驱动 epoll、IOCP 和 io_uring；
- 比较 send/backpressure、read pause、close reason、half-close、cross-thread admission
  和 final drain；
- 性能数字保持同场景方向性，不制造虚假统一。

### 8.5 IOE-X15：实验安装面

- 将 `gamenet_experimental_io_uring` 以 `GameNet::experimental_io_uring` 安装；
- 提供显式 Linux-only `IoUringTcpServer` 和 `IoUringTcpClient` façade；
- 保留 `GAMENET_ENABLE_EXPERIMENTAL=OFF` 默认值；
- 不向稳定 `TcpServer` 添加 backend selector，不自动选择 io_uring；
- experimental API 使用独立 manifest、package consumer 和版本说明，不进入 stable
  compatibility manifest。

完成后可发布 v0.5.0 experimental preview；epoll 仍是 Linux 默认和稳定后端。

## 9. M7：v0.6 Lua 与 typed RPC

坚持“外部先行、通用能力再提升”。

Lua execution cell 保持在 `gamenet-game-gateway`：

- 每 cell 单 owner；
- 有界 mailbox、执行预算和内存预算；
- Lua 异常、超时、reload 和 shutdown 可观察；
- 网络 callback 不直接进入 Lua VM；
- Lua 不进入 `GameNet::core`。

RPC 首先在外部适配层实现 callback/value 版本：

- request/response/error frame；
- generation-safe request id；
- pending request 上限；
- timeout、connection close 和 shutdown 清算；
- handler 异常转 typed error；
- 不依赖 coroutine。

只有网关和第二个独立 consumer 都证明相同 wire/lifecycle 需求后，才提升
`rpc.intent.md`，将 codec、typed result 和 per-connection channel 提升到
`GameNet::protocol`。业务 method registry、鉴权和服务发现继续外置。

关闭门包括 malformed/oversized/partial frame、重复/迟到响应、超时竞态、断线清算、
queue saturation、双平台真实 TCP 和 fuzz coverage。缺少第二个 consumer 时记录
`NO-PROMOTION`，RPC 保持外部 adapter。

## 10. M8：v0.7 Async 与 Coroutine

只有 RPC、timer 或第二个适配器证明需要统一异步语义时，按以下固定顺序提升 deferred
intents：

1. `async_semantics`：统一 value/error/cancel、executor 和 exactly-once continuation；
2. `coroutine_task`：frame ownership、detach、异常和 owner-loop resume；
3. `async_timer`：取消与 TimerQueue retirement；
4. `connection_awaiter_registry`：read/write/close awaiter 与 generation；
5. `when_all`：全部完成和异常聚合；
6. `when_any`：winner、loser cancellation 和 frame retirement；
7. RPC coroutine bridge；
8. C++ coroutine 与 Lua coroutine bridge，继续位于外部网关仓库。

每个切片必须证明：

- coroutine frame sole ownership；
- resume 只发生在目标 owner executor；
- callback/close/timeout/cancel 竞态只完成一次；
- EventLoop shutdown 不遗留 suspended frame；
- rejected scheduling 不产生幽灵 continuation；
- callback API 继续可用，不强迫稳定 Core 用户采用 coroutine。

若缺少两个真实 consumer，这些能力保持外部 adapter，不为满足版本号而提升，也不发布
空版本。

## 11. M9：v0.8 TLS、WebSocket 与 DNS

顺序固定为：

1. `ConnectionTransport` source-private seam，以 plain TCP adapter 证明零行为变化；
2. OpenSSL `TlsContext` 与 owner-loop TLS transport；
3. TcpServer/TcpClient TLS 配置、handshake timeout、证书错误和 graceful TLS shutdown；
4. `DnsResolver` 的有界 worker、admission、cancel 和 typed result；
5. 外部 WebSocket gateway adapter；
6. 只有两个 consumer 需要时，将 framing/control-frame 部分提升到
   `GameNet::protocol`。

TLS 必须覆盖 WANT_READ/WANT_WRITE、partial I/O、renegotiation 禁令、peer close、
callback exception、certificate reload boundary 和双平台 package consumer。

WebSocket 必须覆盖升级校验、mask、fragmentation、ping/pong、close handshake、payload
上限和慢客户端。只实现 WebSocket 所需的最小升级适配；完整 HTTP server 继续不在
v1 范围。

## 12. M10：v0.9 UDP/KCP 实验能力

启动条件：

- v0.3 外部发布完成；
- 至少两个 TCP Runtime Model 经过 M3 真实验证；
- UDP、KCP 和必要 PMTU intents 正式提升；
- owner、Session generation、MTU、retransmission、backpressure 和双通道关闭语义
  完成评审。

### 12.1 DGM-U1：UDP owner-loop 基础

- owner-owned socket/registration；
- 有界 datagram batch；
- typed send/receive/EMSGSIZE；
- generation-safe peer/session identity；
- Linux/Windows loopback 和 shutdown。

### 12.2 DGM-U2：Datagram TransportEndpoint

- bounded send admission；
- peer/session token；
- TCP 与 UDP endpoint 不共享可变 owner 状态；
- 双通道 Session 只在上层通过 generation 绑定。

### 12.3 DGM-K1：基础可靠数据报

- KCP session、timer、重传、ACK、窗口和有序交付；
- deterministic loss/reorder/duplicate/delay fixture；
- 所有队列、window、in-flight bytes 和 per-turn work 有界。

### 12.4 DGM-K2：生命周期与压力

- handshake/session timeout；
- MTU reduction；
- slow peer/backpressure；
- reconnect、stale packet、shutdown/final drain；
- capacity、soak 和方向性 benchmark。

### 12.5 DGM-X1：实验安装

- 提供显式 opt-in 的 `GameNet::experimental_datagram`；
- UDP/KCP headers 使用 experimental versioning；
- 不作为 v1 稳定传输，不自动与 TCP 组成双通道。

raw ICMP、authenticated PMTU signal、FEC、advanced congestion control、zero-copy
和跨进程 Session migration 全部留到 v1 后。

## 13. M11：v1.0 稳定化与发布

### 13.1 v1 稳定范围

- Linux/epoll 和 Windows/IOCP；
- EventLoop、TCP、Timer、owner/lifecycle/admission/typed result；
- 已安装的 protocol、transport、session、logic、broadcast 能力；
- Profile A/B/C 的官方 workload 指南和跨平台证据；
- 经 M5 证明的极窄 Runtime API；若 M5 为 `NO-PROMOTION` 则不强行加入；
- Apache-2.0、SBOM、NOTICE 和明确的兼容政策。

### 13.2 v1 实验范围

- io_uring 可安装后端，仅在 M6 完成时提供；
- UDP/KCP experimental target；
- Profile D 和尚未满足公共提升门的 adapter；
- experimental 能力不计入稳定 backend parity。

### 13.3 v1 发布门

- stable API/ABI policy 独立审查；
- 0.3→1.0 source migration consumer；
- Linux/Windows Debug/Release、sanitizer、TSan、fuzz；
- TCP、Runtime、RPC/TLS 适用场景的 paired benchmark/capacity；
- fault injection；
- game gateway 24h 与 Core 72h endurance；
- 安装包、源码包、校验和、SBOM、许可证和第三方声明；
- 所有 evidence 绑定同一 promotion commit；
- 无 callback-after-destroy、kernel-reference-after-free、stranded Accepted work
  或未说明的 public API drift。

通过后发布 `v1.0.0`。缺失 endurance、许可证、兼容性或生命周期证据时只能发布内部
候选，不能以 waiver 宣称 v1 完成。

## 14. Public Interfaces 与版本策略

- v0.3：不新增 Runtime/backend public API；
- v0.4：最多新增经双实现证明的 `LogicExecutor`、`RuntimeStopFuture`，复用
  `TransportEndpoint`；
- v0.5：条件性安装 `GameNet::experimental_io_uring`、`IoUringTcpServer`、
  `IoUringTcpClient`；
- v0.6：条件性提升通用 RPC codec/channel 到 `GameNet::protocol`，Lua 永久位于
  Core 外；
- v0.7：条件性提升 async/coroutine primitives，callback API 保持完整；
- v0.8：TLS/DNS 位于 transport，WebSocket framing 位于 protocol，完整 HTTP 不进入 v1；
- v0.9：安装 experimental datagram target，不纳入 stable API parity；
- 条件未满足时记录 `NO-PROMOTION` 或 `skipped-by-evidence`，不创建占位接口；
- 版本里程碑因证据分支被跳过时不发布空版本，后续可交付能力接管下一个可用版本号。

## 15. 持续验证、WIP 与完成定义

允许同时存在：

1. 一个 Core 实现切片；
2. 一个 Core 外部集成切片；
3. 一个 CI/benchmark/capacity/endurance 证据任务。

同一时刻只允许一个切片改变 stable Core 生命周期语义。每个 Core 切片必须遵守：

```text
intent -> invariants -> threading/ownership -> contracts
       -> implementation -> exact-commit evidence
```

任务状态统一为：

```text
planned -> contract-ready -> implemented -> verified -> integrated
```

`integrated` 必须满足：

- active intent 与 rules 和实现一致；
- owner、ownership、re-entry、cross-thread marshal、typed failure 和 shutdown 有答案；
- 具体 contract 在目标行为回归时会失败；
- focused repeat、全量、双平台、sanitizer、scope 和 API guards 达到阶段门；
- 热路径变化有 benchmark/capacity 数字；
- evidence 绑定精确 commit；
- migration status、roadmap、README、plan 和 evidence ledger 描述同一当前事实；
- 历史证据不被提升为当前提交结论。

## 16. 当前立即执行

> **IOE-X10 listener capacity / performance decision**：先固化固定协议和结构化
> 校验，再对 source-private io_uring listener 与 production epoll `TcpServer` 执行
> 同场景交错测量；同时完成 ARCH-G1 独立审查。X10 只输出继续 source-private shaping
> 的 `PROMOTE` / `DEFER`，不开放公共 backend selector，也不替换生产 TCP 路径。
