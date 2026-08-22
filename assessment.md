# 总体判断

本次检查以 IOE-X10 实现与证据提交 `f5d39b8` 及 2026-08-22 的 M1
治理关闭为当前前沿。`game-net-core` 已经不再只是从 `mini_trantor` 拆出来的
Reactor/TCP 练习项目，而是进入了：

> **核心网络语义基本成形、生命周期与过载治理较完善、运行模型得到验证，但尚未完成正式生产推广闭环的 production-hardening preview。**

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

2026-08-22 的 M2 执行在精确候选 `a89e2b0` 上完成了 CI、容量、benchmark、
repeat-50 和安装包预检，但按收敛指令取消了只运行 21,073.016 秒的 candidate-24h，
且未启动 release-72h。当前结论因此是 `STOPPED / NO-PROMOTION`；这不是
`v0.3.0-internal-candidate.1` 发布或 M2 关闭。

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

Linux 默认使用 epoll，Windows 使用 IOCP；Linux 还可以显式开启 non-installed、default-off 的实验性 io_uring 模块。TLS、其他平台和动态库目前会在配置阶段明确拒绝，而不是静默降级。

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

### 1. 还不能称为正式 production release

仓库自身的审计结论仍是 production-hardening preview，而不是 production-ready。当前 `main` 尚缺少一个明确 promotion commit 上完整的同提交：

```text
CI
+ benchmark
+ capacity
+ fault injection
+ 24/72 小时 endurance
+ package/release evidence
```

### 2. 外部采用仍被许可证阻塞

当前许可证明确写明不授予使用、复制、修改或分发许可。因此即使代码公开，外部项目也不能把它当作普通开源库合法采用。

### 3. Runtime Profile 和 io_uring 仍是 provisional/non-installed

它们已经得到验证，但尚未进入稳定安装接口，也没有公共 backend selector。这个状态是刻意保持的，并不代表遗漏。

### 4. 还缺少真实游戏服务器的长期使用反馈

目前已有 echo、pipeline 和 runtime profile demo，但仍缺少一个真正覆盖以下路径的参考服务器：

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

这会导致很多 API 目前主要经过合同验证，而不是经过真实业务迭代验证。

### 5. M1 治理漂移已关闭，M2 已形成未推广停止检查点

README、roadmap、migration status、plan、assessment 和 evidence ledger 已统一为
“M1 关闭、M2 `STOPPED / NO-PROMOTION`”。`a89e2b0` 的 CI、性能、容量、故障、
repeat 和 package preflight 可以保留为精确提交证据，但被取消的 24h 快照不能替代
完整 24h/72h endurance，也不能据此形成 internal candidate。

---

# 三、下一步方向：建议的优先级

## P0（已关闭）：IOE-X10 与 ARCH-G1

IOE-X10 listener capacity/performance decision 已输出限定范围的 `PROMOTE`，ARCH-G1
已在两项 non-waivable blocker 和后续 destruction/rollback 缺口修复后给出
`APPROVE`。M6 的 IOE-X11–X15 因此获得条件授权，但仍须排在 v0.3 推广和真实网关
验证之后；当前不得继续横向增加实验功能。

## P1：选择一个 promotion commit，完成真正的 v0.3 证据闭环

M1 已结束。现在应停止继续扩展 IOE-X11/X12，选择一个明确 commit 作为 promotion commit，在该提交上完成：

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
24h endurance
72h endurance
package/SBOM/license
```

如果只是内部使用，可以形成：

```text
v0.3.0-internal-candidate
```

如果准备让外部项目采用，则必须先确定 MIT、Apache-2.0、BSL 或其他明确许可证，并补齐第三方声明和 SBOM。

## P1：建立一个真正的游戏网关参考实现

结合你后续要把 `game-net-core` 与 Lua、YanGameServer 等项目结合起来，下一项最能提升项目价值的工作，不是再增加一个抽象层，而是建立独立的参考项目，例如：

```text
game-net-runtime-demo
或
gamenet-game-gateway
```

建议采用：

```text
TcpServer
→ PacketFramer
→ Auth/SessionManager
→ MultiIoQueuedEvent 或 MultiIoShardedHybrid
→ Lua Runtime
→ BroadcastDispatcher
→ TcpTransportEndpoint
```

这个参考实现应放在 Core 之外，避免把 Lua、Actor、RPC、房间、AOI、数据库和部署拓扑反向塞入 `GameNet::core`。

它主要用来回答当前合同测试回答不了的问题：

* API 是否过于繁琐；
* Session generation 是否容易正确使用；
* Lua 回调阻塞如何隔离；
* 逻辑队列饱和后真实业务如何降级；
* 玩家断线、重连、踢下线如何映射；
* Broadcast 与场景分片怎样组合；
* 停服时网络、逻辑、Lua 和持久化的顺序是否合理。

## P2：根据真实集成结果决定公共 Runtime API

当前四个 Profile 全部保持 non-installed、共同能力审查为 `NO-PROMOTION`，这一状态暂时不应改变。

只有参考网关或实际游戏服证明至少两个 Profile 重复需要某些概念时，才考虑提升极窄的公共接口，例如：

```cpp
RuntimeEndpoint
LogicExecutor
RuntimeStopFuture
ShardKey
TickCadence
```

不建议现在创建：

```cpp
UniversalGameServer
RuntimeProfileFactory
AnyTransportAnyLogicRuntime
```

否则很容易形成配置复杂、热路径动态多态严重、生命周期难以推导的“万能框架”。

## P2：RPC、Lua 和协程优先放在上层适配仓库

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

# 四、建议采用的四个里程碑

```text
Milestone 1
已关闭：IOE-X10 + ARCH-G1 independent review + 文档状态统一

Milestone 2
停止检查点：
`a89e2b0` 的同提交 CI / capacity / benchmark / repeat / package preflight 已通过；
24h 在 21,073.016 秒取消，72h 未启动，故未形成 v0.3.0 internal candidate。

Milestone 3
建立真实 game gateway / Lua runtime reference integration
用实际业务验证 API 和 Runtime Profile

Milestone 4
根据真实重复需求，决定 v0.4：
- 极窄 Runtime 公共能力
- io_uring source-private integration
- RPC/Lua/coroutine 上层适配
三者中选择一条主线，而不是同时展开
```

# 最终评价

`game-net-core` 当前已经完成了最难的一部分：**把高性能网络库最容易失控的 ownership、cross-thread admission、generation、backpressure、callback re-entry 和 shutdown 变成了可验证合同。**

下一阶段最大的风险已经不是“底层能力不够”，而是：

> **继续沉迷内部抽象和实验性功能，却迟迟没有形成一个可以在真实游戏服务器中长期运行、可以推广、可以被别人正确使用的稳定版本。**

因此，当前最合理的路线是：

```text
M1 已关闭（X10 PROMOTE + ARCH-G1 APPROVE）
→ M2 v0.3 停止检查点（NO-PROMOTION；等待重新授权完整重跑）
→ 真实游戏网关/Lua 集成
→ 再决定 v0.4 扩展方向
```

而不是立即启动 UDP、KCP、TLS、HTTP、WebSocket、RPC、协程以及更多 io_uring 高级特性。
