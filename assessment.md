# 总体判断

本次检查以 2026-08-24 IOE-X14 跨后端 TCP 语义套件关闭为当前前沿；
stable Apache-2.0 `v0.3.0@8e4a6ed` 仍是发布基线。`game-net-core` 已经不再只是从
`mini_trantor` 拆出来的
Reactor/TCP 练习项目，而是进入了：

> **核心网络语义、生命周期与过载治理、双平台运行证据和外部分发闭环已形成首个 stable v0.3 基线；0.x ABI 与上层 Runtime API 仍保持审慎边界。**

仓库当前最突出的价值不是某个单独的 epoll、IOCP 或 io_uring 实现，而是逐步建立了统一的：

```text
owner-thread ownership
+ bounded admission
+ generation-safe identity
+ typed result
+ callback containment
+ monotonic shutdown
+ exact-commit evidence
```

这套语义已经开始贯穿 Linux/epoll、Windows/IOCP、实验性 io_uring，以及上层 Runtime Profile。

2026-08-23，M2 在精确 promotion commit `0c30124` 上关闭：同提交双平台 CI、
sanitizer/TSan、10k/100k 容量、paired benchmark、repeat-50、1h/3h endurance、
Linux/Windows 安装包消费者、SPDX 2.3 SBOM、third-party notices 和完整 evidence
bundle 均通过。内部输出命名为 `v0.3.0-internal-candidate.1`，但仓库仍为
all-rights-reserved，不提供外部使用授权。该句描述内部候选形成时的历史边界；仓库已在
后续 M4 中经所有者授权切换为 Apache-2.0。此前 `a89e2b0` 的旧门槛取消运行继续作为
历史 `NO-PROMOTION` 记录保留，不参与本次通过结论。2026-08-23，私有
`gamenet-game-gateway` 又以关闭提交 `0a8fe1e` 完成 M3；精确网关 `4e2457e` / Core
`736a090` 的单一 Linux/epoll 进程通过 1h、3,743 个完整故障回放周期和 32 KiB RSS
增长门。M4 随后在精确 promotion commit `8e4a6ed` 上完成全部同提交证据、确定性
资产、annotated tag、stable Release 和回下载验证。M5 随后用真实网关重新审查
Runtime 共同能力，确认没有缺失的通用 Core 能力，发布 Profile 负载选择指南并记录
第二次 `NO-PROMOTION`；IOE-X11 已在 `013fecf`、IOE-X12 已在 `5be30e7`、
IOE-X13 已在 `5484d7a`、IOE-X14 已在 `351b3c0`、IOE-X15/M6 已在
`43795e8` 关闭。M7 Gateway 实现 `e43393c`、关闭 `588acd0` 与独立
`YanGameServer@b525416` 8/8 逐字段比较已以 `NO-PROMOTION` 关闭 M7；
M8 又以 Core `e5ea9ef` 3/3 与 `YanGameServer@b525416` 9/9 聚焦证据确认
callback-only Gateway 与 ActorScheduler Task 不构成可替代的双 consumer 合同，
因此以 `NO-PROMOTION` 关闭；当前唯一治理前沿是 M9
TLS/WebSocket/DNS 证据审查。

---

# 一、目前已经取得的主要成果

## 1. Reactor/TCP Core 已经形成完整基础网络库

当前项目版本线为 `GameNetCore 0.3.0`，采用 C++23，支持 Linux 和 Windows，1.0 前只构建静态库。安装目标已经按模块拆分为：

```text
GameNet::core
GameNet::protocol
GameNet::transport
GameNet::game_session
GameNet::game_logic
GameNet::broadcast
```

Linux 默认使用 epoll，Windows 使用 IOCP；Linux 还可以显式开启 default-off、独立版本化并可安装的实验性 io_uring 组件，默认包仍不包含任何实验产物。TLS、其他平台和动态库目前会在配置阶段明确拒绝，而不是静默降级。

Core 已经覆盖了比较完整的网络基础设施：

* `EventLoop`、`Channel`、`Poller`、`TimerQueue`
* `Buffer`、`Socket`、`InetAddress`
* `Acceptor`、`Connector`
* `TcpConnection`、`TcpServer`、`TcpClient`
* `EventLoopThread`、`EventLoopThreadPool`
* 日志、指标、内存保留统计、输出内存预算

这意味着当前项目已经具备独立作为 TCP 网络基础库使用的形态，而不是只有一个 echo demo。

## 2. EventLoop 已从“能跑”提升到“可推理”

当前 `EventLoop` 已经具备：

* 单 owner 线程和严格线程亲和语义；
* 有界普通任务队列和保留队列；
* I/O、Timer、control、lifecycle、functor 分阶段预算；
* 可合并的跨线程 control/lifecycle signal；
* typed `PostResult`；
* 异步回调异常 containment；
* `Running → Quiescing → FinalDraining → Shutdown` 单调生命周期；
* pending 数量、拒绝次数、wakeup、callback exception 等指标。

这部分是项目最重要的基础，因为它让“接纳了什么工作、谁拥有状态、退出时还欠什么义务”变得可以通过合同推导，而不是依赖实现习惯。

## 3. TcpConnection/TcpServer 已完成较深的生产加固

`TcpConnection` 已经不仅是简单的 send/read 封装，而是具备：

* owner-thread 状态所有权；
* 跨线程有界 `trySend`；
* connection/loop/server/global 多级输出内存预算；
* 高低水位背压和读暂停；
* 输入缓冲区上限；
* typed close reason 和 close phase；
* graceful shutdown 与 force close 分离；
* callback exception 隔离；
* 内存保留、输出高水位和拒绝次数观测。

`TcpServer` 则已经加入：

* RoundRobin、LeastConnections、QueueLag、ConsistentHash 等连接放置策略；
* 全局连接上限和单 IP 连接上限；
* 单 IP 固定窗口连接速率限制；
* 未认证连接超时；
* admission metrics；
* graceful stop future；
* worker 清理、base bookkeeping、连接释放和线程 join 的聚合关闭；
* accept error 的 Retry-or-Stop 策略；
* loop/server/global 输出内存统计。

这说明 Core 的重点已经从“建立连接和收发数据”进入“过载、异常、退出、资源上限和可观测性”阶段。

## 4. Readiness 与 Completion 已经开始真正解耦

这是当前架构层面最重要的成果。

项目已经建立 source-private I/O Engine seam，将 `EventLoop` 定义为 owner scheduler/event pump，而不是简单等同于 epoll Reactor：

```text
EventLoop
    ├── Readiness Engine：epoll
    ├── Completion Engine：IOCP
    └── Experimental Completion Engine：io_uring
```

具体成果包括：

* IOE-R1：建立内部 I/O Engine 边界；
* IOE-R2：epoll 使用 registration identity + generation 的 Readiness Engine；
* IOE-C1：IOCP 直接产生带 operation identity、generation、bytes、native error 和 lease 的 Completion notice；
* IOCP 不再把完成结果伪装成 `kReadEvent/kWriteEvent`；
* `Channel` 被重新限定为 Readiness registration/callback binding；
* cancellation request 与 terminal completion 被明确分离；
* kernel obligation、observer lifetime 和 storage lease 被明确区分。

这使项目不再试图用一个假的统一事件模型抹平 epoll 和 IOCP 的根本差异，同时仍共享同一套 owner-loop、admission 和 shutdown 公理。

## 5. 游戏网络上层组件已经形成一条可运行管线

Phase 4 已经实现：

* `PacketFramer`：长度前缀拆包与组包；
* `TransportEndpoint`：缩窄上层对具体 `TcpConnection` 的依赖；
* `PlayerSession`、`SessionManager`；
* `GameCommand`、有界 `GameCommandQueue`、`LogicLoop`；
* `BroadcastRouter`、`BroadcastDispatcher` 和 typed backpressure reason；
* 从 framed TCP、认证、Session、逻辑处理到响应发送的 pipeline demo。

这些组件的依赖方向也是正确的：

```text
broadcast / game_logic / game_session / protocol
                         ↓
                    transport
                         ↓
                       core
```

Core 没有反向依赖 Session、Logic、Broadcast 或游戏业务。

## 6. 四种 Runtime Profile 已完成垂直验证

当前已经实现四种非安装的运行模型：

| Profile | 模型                          | 适用方向                          |
| ------- | --------------------------- | ----------------------------- |
| A       | `SingleLoopInlineEvent`     | 轻逻辑、低 handoff、同线程事件处理         |
| B       | `MultiIoQueuedEvent`        | 多 I/O owner + 独立逻辑线程          |
| C       | `MultiIoDedicatedFixedTick` | 固定 Tick、skip/bounded catch-up |
| D       | `MultiIoShardedHybrid`      | 多逻辑 cell、事件与固定 Tick 混合        |

四种模型都有独立实现、示例和真实 TCP 集成合同。

其中比较关键的架构成果是：

* 连接放置与逻辑分片已经分离；
* TCP 连接建立后不迁移网络 owner；
* 逻辑输出通过 endpoint owner executor 返回网络 owner；
* cell 内有序，跨 cell 不承诺全局顺序；
* event-driven 与 fixed-tick 可以并存；
* 队列、每 tick drain、跨域 handoff 和 shutdown 都是有界的。

跨 Profile 共同能力审查最终给出 `NO-PROMOTION`，即暂时不急于发明一个万能公共 Runtime Profile API。这一决定是合理的：目前已经证明四种组合可行，但还没有证明它们确实需要一套稳定公共抽象。

## 7. 实验性 io_uring 已完成 X10 定策

io_uring 目前已经完成的纵向切片为：

```text
X1  one-shot Completion Engine
X2  EventLoop completion pump
X3  单连接 TCP driver
X4  shared-Pump 多连接 Hub
X5  固定容量与 churn 验证
X6  TcpConnection 语义 adapter
X7  graceful drain 与 half-close
X8  有界跨线程 send/shutdown/force admission
X9  source-private listener / Accept ownership
X10 固定协议 listener capacity / performance decision
```

X9 已经证明：

* 有限 `maxPendingAccepts` one-shot Accept 窗口；
* listener、Accept identity、accepted fd 的唯一所有权；
* connection capacity 满时拒绝并恢复；
* generation-safe route reuse；
* factory callback 重入；
* listener-first stop；
* listener future 不晚于 Hub stop future；
* owner quit 后 listener、route、operation、fd 和 byte 零残留。

X10 在精确提交 `f5d39b8` 上按 256 active routes、32 pending Accept、4×64
churn、每 route/wave 100 次 64-byte RTT、每后端一次 warm-up 和五次交错正式样本
完成比较。10 个样本的 correctness、容量恢复、owner/lifecycle 与最终零残留全部
通过，证据决定为限定范围的 `PROMOTE`：只授权将来的 source-private 塑形。

这不是“io_uring 全面更快”的结论。中位数 RTT/吞吐约为 epoll 的 0.499，P50/P99
分别约为 2.233/1.396 倍，P999 约为 0.765 倍，RSS 约为 0.954 倍；吞吐、P50 和
P99 是必须保留的性能债务。ARCH-G1 独立复核结论为 `APPROVE`，同时明确禁止据此
开放公共 backend selector、安装 io_uring target 或替换 production epoll。

M1 关闭验证保留默认测试基线为 129、Linux experimental 基线为 136；固定证据位于
`docs/development/benchmark_results/2026-08-22-ioe-x10-f5d39b8/`，独立审查位于
`docs/reviews/arch-g1-independent-review.md`。

## 8. 工程治理已经相当体系化

仓库已经建立：

* active/deferred/legacy intents；

* ownership、thread affinity、testing 等规则；

* API manifest 和 API baseline；

* exact-commit evidence ledger；

* scope、intent consistency、API compatibility guards；

* Linux/Windows CI；

* capacity gate；

* core benchmark；

* long soak；

* Windows self-hosted CI；

* install/package consumer 验证。

这使项目的开发方式已经从“实现后补测试”，转为：

```text
intent
→ invariants
→ ownership/threading
→ contracts
→ tests
→ implementation
→ exact-commit evidence
```

---

# 二、当前成熟度与主要缺口

## 已经成熟的部分

* owner-loop 并发公理；
* EventLoop 生命周期与有界调度；
* TCP 连接机械语义；
* epoll Readiness 模型；
* IOCP direct Completion 模型；
* 跨线程 admission；
* backpressure、内存预算和 graceful stop；
* 四种 TCP Runtime Profile 的垂直验证；
* API、测试和证据治理基础。

## 尚未完成的部分

### 1. stable v0.3 已发布，但 1.0 承诺尚未形成

`v0.3.0@8e4a6ed` 已完成同提交 CI、benchmark、capacity、fault、1h/3h、package、
SBOM 和 Release 回下载验证。剩余边界不是“尚未发布”，而是 0.x 期间 ABI 仍可按审查
演进，Runtime Profile 和 experimental io_uring 仍不属于 stable API。

### 2. 外部采用许可证阻塞已关闭

所有者授权后，仓库和 v0.3.0 分发资产已切换为 Apache-2.0，并完成 NOTICE、第三方
声明、源码 SPDX、package metadata 与 SBOM 校验。历史内部候选继续保留其形成时的
all-rights-reserved 快照，不被追溯改写。

### 3. Runtime Profile 仍是 provisional/non-installed；io_uring 保持独立实验面

Runtime Profile 已得到验证，但尚未进入稳定安装接口。io_uring 则从 IOE-X15 起仅在
Linux 显式 opt-in 时作为独立版本化的实验组件安装；默认包仍不导出它，也没有公共
backend selector。两者都不属于 stable API，这个状态是刻意保持的，并不代表遗漏。

### 4. 已有真实网关闭环，但仍缺少长期生产运营反馈

私有 `gamenet-game-gateway` 已覆盖以下完整参考路径：

```text
Gateway
→ 鉴权
→ Session
→ 协议分发
→ Logic shard
→ Lua/业务逻辑
→ Broadcast
→ Graceful shutdown
```

它已完成双平台、sanitizer、故障回放和一小时连续运行，并直接触发过一个 IOCP
correctness 修复。尚缺的是更长周期、更多独立 consumer、真实流量和运维升级反馈；
这也是 M5 不把单一网关需求提升为通用 Runtime API 的原因。

### 5. M1–M8 与 IOE-X11–IOE-X15 已关闭，M9 成为治理前沿

README、roadmap、migration status、plan、assessment 和 evidence ledger 已统一为
“M1–M8 与 IOE-X11–IOE-X15 关闭、M9 当前”。`0c30124` 的同提交门禁、1h/3h endurance、package、
SPDX 和 evidence bundle 形成 `v0.3.0-internal-candidate.1`；随后 M3 网关发现的 IOCP
生命周期 blocker 由 `736a090` 修复，并通过双平台真实网关与 1h 故障回放。`a89e2b0`
的取消快照继续保留为历史 `NO-PROMOTION`，未被提升为当前结论。最终
`8e4a6ed` 重新完成非豁免矩阵并发布 stable Apache-2.0 `v0.3.0`；12 个 Release
资产全部通过回下载复核。M5 对四个候选边界逐项复核后记录第二次
`NO-PROMOTION`，未增加公共 API、未发布空 v0.4，并交付负载选择指南。随后 IOE-X11
在 `013fecfe81277845eb3e60ccf5fe0205b753858d` 完成单 owner Server composition，
IOE-X12 又在 `5be30e701c61f8d6700bcc4be6bc0ef152120fb8` 完成多 owner topology，
IOE-X13 再在 `5484d7a89b01597824bc860e4d2d3cf3cfd45a82` 完成 one-shot Connect
与 source-private TcpClient composition。IOE-X14 最后在
`351b3c0e476a462265016d53361a02b5f2c51611` 用同一 portable contract 比较 epoll、
IOCP 与 io_uring 的 send/backpressure、read pause、close、half-close、cross-thread
admission 和 final drain；它同时关闭 shutdown 请求后的发送准入窗口和 Windows IOCP
disconnecting 状态不续投读取两个 production correctness 缺口，稳定 API 继续零漂移。
IOE-X15 随后在 `43795e841ba2a279ed6a3d5d831d60a9f2a25570` 导出显式
`GameNet::experimental_io_uring`、六个受独立 manifest 指纹保护的实验头文件和独立
package consumer；默认 Linux/Windows 包继续不含实验产物，没有 stable selector、tag
或 GitHub Release。
M7-G0 随后在 `44493b1d37c16567990e1660153d6b0843a8eecc` 对 Gateway 与独立
`YanGameServer` 做就绪审计：前者尚无 RPC consumer 合同，后者的 wire-v2/native
transport 生命周期并非同一 GameNet 协议需求，因此外部实现记录 `DEFER`、共享 RPC
记录 `NO-PROMOTION`。随后 Gateway M4 在
`d03cacd5aead885fc61a419c71d5c32a060cb700` 关闭，M7 外部 adapter 的
intent/rules/合同名在 `92a26072c3300275edc9d069a59fc17913c7614c` 获得授权；外部实现
随后在 `e43393c85fa37604d340fe866610756c99f4fe4e` 完成，并在
`588acd079be93de3e230ba4f07dd111f7bec6a3c` 绑定 Windows/Linux 11/11、sanitizer、
focused repeat、100,000 次 fuzz 与独立 `YanGameServer@b525416` RPC 8/8 比较。
两者的 wire、correlation、owner 和 completion 合同不可替代，所以 M7 最终
以 `NO-PROMOTION` 关闭，`rpc.intent.md` 继续 deferred，且没有 RPC/Lua
安装面或空 v0.6 发布。
M8-G0 继续在 Core `e5ea9efa71dbe52e841423ec3cac3e9529158b22`、Gateway
`588acd079be93de3e230ba4f07dd111f7bec6a3c` 与独立
`YanGameServer@b5254165389d762c3f3c63568c24ffab448fc501` 之间比较 async/coroutine
合同。Core EventLoop/TimerQueue 3/3 与 YanGame coroutine/timer/Actor-RPC/persistence
9/9 通过，但 Gateway 仍无 coroutine，YanGame Task 的 ActorScheduler 准入、帧共享
控制状态、continuation token、ActorRef 世代和 owner 销毁也不是 Core 旧 deferred
`start/detach/co_await` 包装合同。因此 M8 以 `NO-PROMOTION` 关闭，六个
async intents 保持 deferred，没有 coroutine/awaiter 安装面或空 v0.7 发布。
当前默认测试基线为 130（8 unit、108 contract、14 integration），Linux experimental
基线为 140（8 unit、118 contract、14 integration）。

---

# 三、下一步方向：建议的优先级

## P0（已关闭）：IOE-X10 与 ARCH-G1

IOE-X10 listener capacity/performance decision 已输出限定范围的 `PROMOTE`，ARCH-G1
已在两项 non-waivable blocker 和后续 destruction/rollback 缺口修复后给出
`APPROVE`。M6 的 IOE-X11–X15 因此获得条件授权，但仍须排在 v0.3 推广和真实网关
验证之后；当前不得继续横向增加实验功能。

## P1（已关闭）：选择 promotion commit，完成 v0.3 内部证据闭环

Promotion commit `0c30124` 已完成：

```text
Linux Debug/Release
Linux ASan/UBSan
Linux TSan
Windows Debug/Release/IOCP
install consumer
API manifest
paired benchmark
capacity
fault injection
1h endurance
3h endurance
package/SBOM/license
```

内部输出已形成：

```text
v0.3.0-internal-candidate.1
```

内部候选保留其形成时的专有许可证快照。后续 M4 已完成 Apache-2.0 切换、第三方来源
审计、最终外部包、SBOM、evidence、annotated tag、stable Release 和回下载验证。

## P1（已关闭）：建立真正的游戏网关参考实现

私有 `gamenet-game-gateway` 已只通过安装目标完成 Queued Event 与 Sharded Hybrid
垂直切片，覆盖 Auth/Session、Lua、广播、持久化、饱和恢复、callback re-entry、分片
隔离、确定性停机、流量回放和故障注入。反馈账本没有遗留通用 Core blocker，也没有
把 Lua、Actor、房间、数据库或部署拓扑反向引入 Core。

## P1（已关闭）：完成 M4 外部发布治理

所有者授权 Apache-2.0 与通过全部门后的公开发布；LICENSE/NOTICE/header/package
metadata 已同步。因 `736a090` 运行时修复选择的最终 promotion commit `8e4a6ed`
完整重跑并通过双平台、sanitizer、容量、性能、fault、repeat、1h/3h、最终包消费者和
evidence bundle 门，无 waiver。

M4 preflight 已完成：仓库本身已经是 Public，564 文件的来源/资产清单未发现 vendored
library、submodule、LFS 或外部测试数据。随后所有者授权并完成 Apache-2.0、源码 SPDX、
NOTICE、third-party notice 与安装包 metadata 切换。受跟踪的发布组装器两次生成
12/12 字节一致的最终 bundle，官方 SPDX 2.3 Schema 与全资产回下载验证通过；
`v0.3.0` 已作为 stable GitHub Release 发布。

## P2（已关闭）：根据真实集成结果决定公共 Runtime API

M5 已逐项审查现有 `TransportEndpoint`、typed/bounded `LogicExecutor` admission、
可等待单调 `RuntimeStopFuture` 和 shard/cadence 类型。真实网关的 Queued Event 与
Sharded Hybrid 只使用已安装 v0.3 能力，没有提出 missing broadly reusable
capability；对应概念的 owner、Accepted obligation、失败作用域和退休语义也仍不同。
因此第二次结论是 `NO-PROMOTION`，四个 Profile 继续保持 non-installed recipe/example。

- `TransportEndpoint`：已安装并由网关直接复用，不增加 Runtime alias；
- `LogicExecutor`：Queued、fixed-tick、Hybrid 的 admission/terminal obligation 不同；
- `RuntimeStopFuture`：network、logic、cadence 与多 cell 的完成义务不能无损归一；
- shard/cadence 类型：key vocabulary、ordering、catch-up 与 retirement 仍不一致。

不建议现在创建：

```cpp
UniversalGameServer
RuntimeProfileFactory
AnyTransportAnyLogicRuntime
```

这些形状继续禁止，避免形成配置复杂、热路径动态多态严重、生命周期难以推导的
“万能框架”。负载选择改由连接数、包频率、逻辑成本、tick、handoff、广播和背压
测量驱动；M5 不发布空 v0.4。

## P2（已关闭）：M6/IOE-X11 source-private io_uring Server composition

`X10=PROMOTE` 与 `ARCH-G1=APPROVE` 条件已满足。下一任务只组合现有 listener、Hub
和 semantic adapter，证明单 owner bind/listen、callback、admission 与 graceful/
forced stop；不修改 production `TcpServer`，不开放公共 backend selector。该切片已在
`013fecfe81277845eb3e60ccf5fe0205b753858d` 通过双平台、sanitizer、repeat、package 与
API/scope 门并关闭。

## P2（已关闭）：M6/IOE-X12 source-private 多 owner topology

下一任务由 accept owner 接收 fd，再通过有界 EventLoop admission 将 sole ownership
精确转移给选定 worker Hub；所有 post、admission 与 worker-shutdown 失败路径都必须由
当前 sole owner 回收 fd，已建立连接不得迁移 owner。

该切片已在 `5be30e701c61f8d6700bcc4be6bc0ef152120fb8` 关闭：四种 placement、
bounded handoff、post/admission/shutdown rollback、graceful/forced shutdown 和
accept-owner quit 均有真实 TCP 合同与 sanitizer/双平台边界证据。

## P2（已关闭）：M6/IOE-X13 source-private Connect/TcpClient

该切片已在 `5484d7a89b01597824bc860e4d2d3cf3cfd45a82` 关闭：one-shot Connect
覆盖地址复制与 lease 生命周期，source-private Client 覆盖 timeout、retry、cancel、
stale attempt、callback re-entry、foreign-thread rejection 与 owner quit，并与 production
`TcpClient` 比较 connected/message/disconnected 观察序列。normal、ASan/UBSan、TSan、
Linux/Windows 默认基线、安装隔离与稳定 API 零漂移门均通过。

## P2（已关闭）：M6/IOE-X14 跨后端语义套件

该切片已在 `351b3c0e476a462265016d53361a02b5f2c51611` 关闭：同一 server/client
合同驱动 epoll、IOCP 与 io_uring，比较 send/backpressure、read pause、close reason、
half-close、cross-thread admission 与 final drain；normal、ASan/UBSan、TSan、双平台、
安装隔离、治理守卫和稳定 API 零漂移门均通过。

## P2（已关闭）：M6/IOE-X15 实验安装面

该切片已在 `43795e841ba2a279ed6a3d5d831d60a9f2a25570` 将 io_uring target 作为显式
Linux-only experimental surface 安装，提供
`IoUringTcpServer` 与 `IoUringTcpClient` façade、独立 manifest/package consumer 和版本
说明；保持 opt-in 默认关闭，不给稳定 `TcpServer` 增加 backend selector，也不自动选择
io_uring。Linux 默认/实验安装消费者、Windows 默认消费者、140/130 完整门、ASan/UBSan、
TSan、仓库守卫和稳定 API 零漂移均通过；版本轨道已准备但未发布 tag/Release。

## P2（当前）：RPC、Lua 和协程优先放在上层适配仓库

对于你的总体游戏服务器目标，TCP Core 稳定后，实际收益更高的顺序应是：

1. Lua execution cell / Lua actor runtime；
2. typed RPC 和 request-response correlation；
3. C++ coroutine 与 Lua coroutine 的异步桥接；
4. WebSocket/TLS gateway adapter；
5. UDP/KCP 或双通道 Session。

其中前四项都可以先建立在 `TransportEndpoint` 和 Runtime Profile 之上，不需要修改 Core。

UDP/KCP 应继续等到：

* TCP Core promotion 完成；
* 至少两个 Runtime Profile 经真实项目验证；
* datagram/reliable-datagram intent 被提升；
* MTU、重传、拥塞、Session ownership 和双通道关闭语义明确。

---

# 四、建议采用的六个里程碑

```text
Milestone 1
已关闭：IOE-X10 + ARCH-G1 independent review + 文档状态统一

Milestone 2
已关闭：`v0.3.0-internal-candidate.1@0c30124`；同提交 CI / capacity /
benchmark / repeat / 1h/3h / package / SPDX / evidence bundle 全部通过。

Milestone 3
已关闭：真实 game gateway / Lua runtime reference integration；并在其反馈修复后
完成 M4 `v0.3.0@8e4a6ed` 外部发布闭环。

Milestone 4
已关闭：M5 第二次 Runtime 共同能力审查为 `NO-PROMOTION`；发布负载选择指南，未发布
空 v0.4。

Milestone 5
已关闭：M6/IOE-X15 在 `43795e8` 交付显式 Linux-only io_uring experimental 安装面，
未发布 tag/Release。

Milestone 6
已关闭：M7 外部 Gateway 与独立 consumer 比较结论为 `NO-PROMOTION`；
未加入 RPC/Lua Core 公共 API，未发布空 v0.6。

Milestone 7
已关闭：M8 async/coroutine 双 consumer 证据不足，结论为 `NO-PROMOTION`；
六个 intent 保持 deferred，未创建 coroutine 占位 target 或空 v0.7。

Milestone 8
当前：M9 审查 TLS/WebSocket/DNS 的外部双 consumer 证据；未提升 intent 之前
不创建公共 transport 或协议表面。
```

# 最终评价

`game-net-core` 当前已经完成了最难的一部分：**把高性能网络库最容易失控的 ownership、cross-thread admission、generation、backpressure、callback re-entry 和 shutdown 变成了可验证合同。**

下一阶段最大的风险已经不是“底层能力不够”，而是：

> **继续沉迷内部抽象和实验性功能，却迟迟没有形成一个可以在真实游戏服务器中长期运行、可以推广、可以被别人正确使用的稳定版本。**

因此，当前最合理的路线是：

```text
M1 已关闭（X10 PROMOTE + ARCH-G1 APPROVE）
→ M2 已关闭（v0.3.0-internal-candidate.1@0c30124）
→ M3 已关闭（private gateway 0a8fe1e；1h on 4e2457e/Core 736a090）
→ M4 已关闭（stable Apache-2.0 v0.3.0@8e4a6ed）
→ M5 已关闭（第二次 NO-PROMOTION；无空 v0.4）
→ M6/IOE-X11 已关闭（013fecf）
→ M6/IOE-X12 已关闭（5be30e7）
→ M6/IOE-X13 已关闭（5484d7a）
→ M6/IOE-X14 已关闭（351b3c0）
→ M6/IOE-X15 已关闭（43795e8；未发布 preview）
→ M7 已关闭（Gateway e43393c/588acd0；NO-PROMOTION）
→ M8 已关闭（Core e5ea9ef / YanGame b525416；NO-PROMOTION）
→ M9 TLS/WebSocket/DNS 证据审查（当前）
```

而不是立即启动 UDP、KCP、TLS、HTTP、WebSocket、RPC、协程以及更多 io_uring 高级特性。
