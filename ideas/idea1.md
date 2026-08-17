请从架构角度详细分析以下问题：
> game-net-core 的目标是成为高性能游戏服务端的通用网络底座。不同游戏在连接规模、实时性、可靠性、固定 Tick、广播模式、线程亲和性和背压策略等方面差异明显，因此项目是否应采用“稳定的网络语义内核 + 可组合的运行模型”，根据实际负载选择 TCP/UDP、单/多 Reactor、I/O 与逻辑同线程或分离、事件驱动或固定 Tick 等模型，而不是用唯一底层模型覆盖所有游戏？
>
> 请分析这种多模型架构的必要性、公共内核与可替换策略的边界、模型选择依据、可能的抽象方式、性能与复杂度风险，以及适合 game-net-core 当前 Reactor/TCP 迁移阶段的渐进式演进路线。


# 结论

我赞成，而且建议把它确立为 `game-net-core` 的长期架构原则：

> **单一、稳定、可验证的网络语义内核；多个受约束、可组合的运行模型。**

更准确地说，不是建设多套互不兼容的底层网络内核，而是：

> **统一所有权语义、生命周期语义、线程亲和语义、背压结果语义；允许传输、线程拓扑、逻辑调度、连接分片、广播和过载处置策略变化。**

也就是：

```text
一个并发安全公理
+ 多种运行拓扑
+ 少量经过验证的标准组合
≠ 任意策略自由拼装
```

这是 `game-net-core` 成为“通用游戏网络底座”而不是“某一类游戏服务器框架”的必要条件。

当前仓库实际上已经走在这条路上，只是这些扩展点还分散在 `EventLoopThreadPool`、`TransportEndpoint`、`LogicLoop`、Pipeline 示例和 Broadcast 模块中，尚未被正式归纳成“运行模型层”。当前 `main` 最新治理提交为 `1daa649`，其父提交是实现检查点 `9d2a5be`；仓库尚未冻结最终 v0.3 候选，下一步仍是 REL-C1。

---

# 一、为什么多模型架构是必要的

游戏服务器的差异并不只在“连接数”，而在多个相互独立的负载维度：

```text
连接活跃度
× 单连接包频率
× 单包逻辑成本
× 延迟与抖动目标
× 可靠性和有序性要求
× 状态是否可分片
× 广播扇出规模
× Tick 一致性要求
× 可接受的丢弃和降级方式
```

连接数本身甚至不是选择 Reactor 数量的最佳指标。十万个几乎空闲的长连接，可能比一万个每秒发送几十个包、同时执行战斗和广播的连接更容易处理。

## 1. 唯一模型必然在部分游戏中产生结构性浪费

强制所有游戏采用“网络和逻辑分线程”：

* 简单大厅、聊天、回合制服务会多出队列、复制、唤醒和调度延迟。
* 原本可以在连接 owner loop 内直接完成的微小业务，被迫经过一次跨线程 handoff。
* 生命周期和停服顺序复杂度显著上升。

强制所有游戏采用“网络和逻辑同线程”：

* 复杂战斗、Lua 脚本、路径计算、批量结算会阻塞 I/O。
* 一个慢逻辑回调可能延迟同一 EventLoop 上全部连接的读写。
* 固定 Tick 超时会直接转化为网络延迟抖动。

强制所有游戏采用固定 Tick：

* 登录、聊天、商城、邮件等事务型业务会平白增加 5～50ms 的排队延迟。
* 低负载时仍然周期性唤醒。
* 瞬时积压可能诱发 Tick catch-up 螺旋。

强制所有游戏采用纯事件驱动：

* 世界模拟难以形成明确的状态提交边界。
* 同一逻辑帧内的事件顺序更难控制。
* 批量广播、物理模拟和战斗结算不容易获得稳定节拍。

强制所有游戏使用 TCP：

* 对实时位置同步而言，旧包阻塞新包的队头阻塞可能比丢一个旧包更糟。

强制所有游戏使用 UDP/KCP：

* 登录、支付、背包、邮件等可靠事务会承担不必要的重传、安全、MTU 和会话管理复杂度。

因此，**通用性不应来自一个无所不包的万能模型，而应来自一组保持相同安全语义的可选模型。**

---

# 二、当前仓库其实已经具备多模型的雏形

## 1. 单 Reactor 与多 Reactor 已经统一在一套语义下

`EventLoopThreadPool` 支持：

* 零 worker：全部连接归 base loop，即单 Reactor。
* N 个 worker：base acceptor + worker EventLoop，即主从多 Reactor。
* RoundRobin。
* LeastConnections。
* QueueLag。
* ConsistentHash。

这已经证明“一个 EventLoop 一个 owner 线程”并不等于“项目只能有一种线程拓扑”。

当前线程意图真正固定的是：

* Poller、Channel、Timer、连接状态由 owner EventLoop 串行拥有。
* 非 owner 线程只能通过明确的调度入口提交工作。
* 回调上下文和退出 drain 行为可推理。
* 不允许跨 loop 直接修改共享连接状态。

这是应当保留的**并发公理**，而不是需要替换的部署模型。

## 2. TransportEndpoint 已形成传输解耦边界

`TransportEndpoint` 当前只暴露：

* 稳定 transport session ID。
* owner executor。
* owner-thread `send/close`。
* 跨线程终止控制。
* `isOpen()` 快照。

它不解析协议，也不拥有玩家或业务状态。TCP 只是当前适配器。

这正是未来 TCP、UDP、KCP 或双通道会话共用上层 Session/Broadcast 的合理边界。

## 3. LogicLoop 已经是一种独立运行模型

当前 `LogicLoop` 是：

```text
跨线程有界提交
→ GameCommandQueue
→ 单一逻辑 EventLoop
→ 周期性有界 drain
→ 输出回送 endpoint owner
```

它明确用于把 I/O 回调与逻辑执行解耦，并且把自适应调度、业务模拟等留在外部。

Pipeline 示例还允许：

* 配置 I/O worker 数量。
* 逻辑与管理共用 loop。
* 注入独立的 logic loop。

因此，仓库现在已经能够表达“多 I/O Reactor + 独立逻辑线程”的基本模型，只是仍处于非安装示例层。

## 4. Broadcast 和背压也已经从 TCP 机械层中分离

Broadcast 当前已经具备：

* 按 endpoint owner loop 分组。
* immutable shared payload。
* 每任务 endpoint/byte 限制。
* 每 owner outstanding task/byte 限制。
* 全局 outstanding byte 限制。
* 优先级和明确丢弃原因。

TCP 层则负责：

* 单连接输入/输出上限。
* 高低水位读节流。
* loop/server/global 输出内存预算。
* 明确区分 connection、loop、server、global overload。

这已经基本符合“内核提供机械能力和事实，上层决定业务策略”的方向。

---

# 三、正确的总体分层

建议将长期架构明确为四层：

```text
┌─────────────────────────────────────────────┐
│                 游戏业务层                  │
│  Room / AOI / Actor / World / Lua / DB ... │
└─────────────────────────────────────────────┘
                       │
┌─────────────────────────────────────────────┐
│               运行模型与策略层              │
│ Logic Placement / Tick / Affinity /         │
│ Broadcast / Backpressure / Runtime Profile  │
└─────────────────────────────────────────────┘
                       │
┌─────────────────────────────────────────────┐
│            网络基础组件与传输适配层         │
│ Protocol / TransportEndpoint / Session /    │
│ TCP / UDP / KCP / PacketFramer              │
└─────────────────────────────────────────────┘
                       │
┌─────────────────────────────────────────────┐
│               稳定网络语义内核              │
│ Owner Executor / Lifecycle / Admission /    │
│ Timer / Buffer / Bounded Work / Metrics     │
│ EventLoop / Poller / epoll / IOCP           │
└─────────────────────────────────────────────┘
```

当前仓库的依赖方向已经要求 `GameNet::core` 不反向依赖 Phase 4，protocol/transport 位于 core 之上，Pipeline 只是组合示例。这个依赖方向应继续保持。

---

# 四、哪些必须进入稳定语义内核

下面这些不应成为随游戏变化的策略。

| 语义                     | 为什么必须统一                                             |
| ---------------------- | --------------------------------------------------- |
| owner-thread ownership | 决定哪些状态可以无锁访问，是整个安全模型的根                              |
| 跨线程提交结果                | 必须明确区分 Accepted、QueueFull、Shutdown、OwnerUnavailable |
| fd/socket 所有权转移        | 必须保证 exactly-once claim/release，不能由运行模型自行解释         |
| Channel/Poller 注册生命周期  | 与平台后端、对象释放和回调安全直接相关                                 |
| 连接关闭与 final drain      | 防止已接纳任务被静默遗失或访问已销毁对象                                |
| 回调线程与重入规则              | 上层必须能推导回调在哪个线程执行                                    |
| 单调时钟和 Timer 基本语义       | 固定 Tick、超时和重试都依赖同一时间基础                              |
| 有界 admission 和资源记账     | 防止任何运行模型通过无界队列“解决”压力                                |
| typed failure/result   | 上层策略必须根据事实作出决定，而不是解析日志                              |
| 基础观测指标                 | 各运行模型必须使用同一套可比较指标                                   |

其中尤其要坚持：

> **内核保证“不会失控”，策略决定“过载时牺牲什么”。**

例如：

* TCP 内核负责发现输出超过 hard limit。
* 上层决定丢弃低优先级消息、暂停读取、断开慢客户端还是降级广播。
* EventLoop 负责拒绝已经饱和的跨线程提交。
* 运行模型决定将请求退回客户端、重试、合并还是丢弃。

---

# 五、哪些应该成为可替换策略或运行模型

## 1. 传输模型

```text
TCP
UDP
KCP / reliable UDP
TCP + UDP 双通道
```

但不要为了“统一”而抹平其语义差异。

建议未来给 endpoint 增加一个只读能力描述，例如：

```cpp
struct TransportCapabilities {
    bool reliable;
    bool ordered;
    bool messageOriented;
    bool supportsPartialReliability;
    std::size_t recommendedPayloadBytes;
};
```

基础 `TransportEndpoint` 仍然只承担身份、生命周期和通用发送边界。UDP/KCP 的 MTU、可靠级别、通道和 datagram metadata 应使用专用扩展接口或发送描述对象，不应把基础接口膨胀成万能参数集合。

当前 UDP/KCP intents 已经将它们标记为 deferred experimental，并继续要求 socket、session、重传和 flush 状态属于单一 owner EventLoop。这说明未来传输切换不需要推翻现有并发语义。

## 2. I/O 拓扑

至少应允许：

```text
SingleReactor
MainReactor + N SubReactors
未来可验证后：per-loop listener / reuse-port topology
```

但不应允许每个业务模块自己创建无规则的网络线程。

## 3. I/O 与逻辑的放置方式

建议正式定义四种模型：

### InlineEventDriven

```text
I/O callback → protocol → business handler → send
```

适合：

* 大厅。
* 聊天。
* 登录。
* 回合制。
* 每次处理严格有界的轻量服务。

优点是零 handoff、低延迟、顺序清晰。

### QueuedEventDriven

```text
I/O callback → bounded command queue
             → coalesced logic drain
             → output
```

适合：

* 不需要固定 Tick。
* 但逻辑不能在 I/O loop 上执行。
* 希望消息尽快处理而不是等待下一帧。

这是当前 `LogicLoop` 尚未覆盖但很重要的中间模型。

### FixedTick

```text
I/O callback → bounded queue
             → fixed tick bounded drain
             → simulation commit
             → batched output
```

适合：

* 房间战斗。
* 世界模拟。
* 物理同步。
* 需要明确帧边界的逻辑。

### Hybrid

```text
认证/聊天/事务 → event-driven
战斗输入       → fixed tick
位置快照       → 可覆盖、可丢弃、最新值优先
```

这往往是 MMO 或动作游戏最合理的最终形态，而不是纯固定 Tick 或纯事件驱动。

## 4. 连接归属与逻辑归属必须分开

当前 `TcpServer` 在 accept 时把 `peerAddr.toIp()` 作为 affinity key 传给 `selectLoop()`。因此现有 ConsistentHash 实际上是按客户端 IP 选择连接 I/O loop。

这不能等同于玩家、房间或场景亲和性：

* 同一 NAT 后大量玩家可能落到同一个 I/O loop。
* accept 时通常还不知道 playerId、roomId。
* TCP 连接建立后再迁移 owner loop，会带来极高生命周期和 fd 注册复杂度。

正确做法是分成两个策略：

```text
ConnectionPlacementPolicy
    决定 socket 属于哪个 I/O EventLoop

LogicShardPolicy
    根据 playerId / roomId / sceneId
    决定命令属于哪个逻辑 ExecutionCell
```

不要为了业务亲和性迁移已建立的 TCP 连接。让输出通过 endpoint 的 `ownerExecutor()` 返回原连接 owner 即可。

---

# 六、固定 Tick 需要特别修正的地方

当前 `LogicLoop` 虽然称为 fixed-tick bridge，但实现调用的是双参数 `runEvery(interval, callback)`。这个兼容重载默认对应 **FixedDelay、无 catch-up**：下一次执行相对本次回调完成时间计算。

因此当前实际语义更接近：

```text
周期性有界 drain
```

而不是严格意义上的：

```text
固定频率权威模拟 Tick
```

例如 Tick 周期 20ms，业务处理用了 8ms，FixedDelay 下下一次回调可能相对完成时间再延后 20ms，长期会产生节拍漂移。

建议下一阶段引入明确的 `TickPolicy`：

```cpp
enum class TickCadence {
    FixedDelay,
    FixedRateSkipMissed,
    FixedRateBoundedCatchUp,
};

struct TickPolicy {
    std::chrono::steady_clock::duration interval;
    TickCadence cadence;
    std::size_t maxCatchUpTicks;
    std::size_t maxCommandsPerTick;
    std::chrono::steady_clock::duration maxWorkTimePerTick;
};
```

并输出：

```cpp
struct TickContext {
    std::uint64_t tickIndex;
    TimePoint scheduledAt;
    TimePoint startedAt;
    Duration lag;
    std::size_t skippedTicks;
};
```

需要注意：即使 EventLoop 对每轮 timer 数量有限制，一个 LogicLoop Tick 回调内部仍可能连续处理数百条命令。EventLoop 的公平预算约束的是“回调数量”，无法约束单个业务回调的执行时长。当前 EventLoop 已经为 active channels、timers、control、lifecycle、functors 设置分阶段预算，但重业务 Tick 仍可能独占 owner 线程。

所以：

* 同线程 FixedTick 只适合严格有界的微逻辑。
* 权威世界模拟默认应使用独立 logic EventLoop。
* catch-up 必须有界。
* 超过预算时应跳帧、降级或记录 overrun，不能无限追帧。

---

# 七、推荐的核心抽象方式

## 1. 不要创建“每种组合一个类”

不要形成：

```text
TcpSingleReactorFixedTickServer
TcpMultiReactorFixedTickServer
UdpMultiReactorFixedTickServer
TcpMultiReactorEventDrivenServer
...
```

这会迅速产生组合爆炸。

应当按正交维度组合：

```text
RuntimeModel =
    Transport
  + IoTopology
  + ConnectionPlacement
  + LogicPlacement
  + DispatchCadence
  + BroadcastStrategy
  + BackpressurePolicy
```

但对外只提供少量经过验证的 Profile，而不是允许完全自由组合。

## 2. 引入 ExecutionCell，而不是泛化 EventLoop

可以在未来 provisional runtime 层引入：

```cpp
class ExecutionCell {
public:
    SerialExecutor executor() const;
    CommandSubmitResult submit(GameCommand command);
    CellId id() const noexcept;
};
```

具体实现可以是：

```text
CoLocatedExecutionCell
    使用连接 owner EventLoop

DedicatedExecutionCell
    独立 EventLoop + Queue

FixedTickExecutionCell
    独立 EventLoop + Queue + TickDriver

ShardedExecutionCellPool
    多个 Cell，按 room/player/scene 分片
```

`ExecutionCell` 不应暴露 Poller、Channel 或 fd。它只是“串行执行域”，这样未来 Lua VM、Actor mailbox、房间状态都可以绑定到 Cell，而不会反向污染网络 core。

## 3. 把提交接口与调度方式分离

上层只依赖：

```cpp
class CommandSink {
public:
    virtual SubmitResult submit(GameCommand command) = 0;
};
```

不同实现决定：

* inline 执行。
* 事件驱动 drain。
* 固定 Tick drain。
* 分片转发。

这比让所有上层直接依赖 `LogicLoop` 更合理。当前 `LogicLoop` 应成为 `FixedTickCommandSink` 的一种实现，而不是唯一逻辑模型。

## 4. 背压策略采用“事实输入—动作输出”

建议不要让 policy 自己持有连接或调用网络 API：

```cpp
struct PressureSnapshot {
    std::size_t connectionPendingBytes;
    std::size_t loopPendingBytes;
    std::size_t serverPendingBytes;
    std::size_t logicQueueDepth;
    std::chrono::nanoseconds oldestCommandAge;
    MessagePriority priority;
    MessageClass messageClass;
};

enum class PressureAction {
    Accept,
    Reject,
    Drop,
    Coalesce,
    PauseRead,
    CloseEndpoint,
    DegradeDelivery,
};
```

策略只返回 `PressureAction`，具体动作仍在 owner loop 上执行。

这种设计可以避免：

* policy 跨线程直接操作连接。
* 回调重入。
* 策略对象持有业务对象生命周期。
* 同一压力被多个模块重复处置。

---

# 八、模型选择依据

建议把选择过程变成负载决策矩阵，而不是按“游戏类型名称”硬编码。

| 负载特征                | 推荐方向                                                     |
| ------------------- | -------------------------------------------------------- |
| 逻辑回调极轻、无阻塞、低广播      | I/O 与逻辑同线程、事件驱动                                          |
| 逻辑耗时波动明显或包含脚本       | I/O 与逻辑分离                                                |
| 需要世界帧一致性            | 独立 FixedTick ExecutionCell                               |
| 事务型业务、要求立即响应        | 事件驱动，不等待 Tick                                            |
| 大量低活跃长连接            | Reactor 数量按活跃事件率和 CPU 决定，不按连接数直接决定                       |
| 高频小包                | 多 Reactor、批量 drain、减少跨线程唤醒                               |
| 玩家/房间状态可分片          | 按 player/room/scene 分配 logic cell                        |
| 大规模全服广播             | owner-loop 分组、共享 payload、分批调度                            |
| 位置状态允许覆盖旧值          | coalesce/latest-value，而不是无条件排队                           |
| 关键事务不可丢失            | reliable transport + reject/slowdown，不做静默 drop           |
| 网络丢包下仍要求低实时延迟       | UDP/KCP 或 TCP+UDP 双通道                                    |
| Windows/IOCP 作为目标平台 | 单独验证 completion batching、buffer ownership、shutdown drain |

一些典型组合如下。

### 大厅、聊天、回合制

```text
TCP
+ Single/Small Multi Reactor
+ InlineEventDriven 或 QueuedEventDriven
+ 简单 RoundRobin
+ 连接级硬背压
```

### MMO 网关

```text
TCP
+ Main Reactor + N I/O Reactors
+ 网络和逻辑分离
+ player/scene logic sharding
+ owner-loop grouped broadcast
+ connection/loop/server/global 分层预算
```

### 房间制动作游戏

```text
TCP 控制通道 + UDP/KCP 实时通道
+ Multi Reactor
+ room-affinity ExecutionCell
+ 30/60Hz FixedRate Tick
+ 有界 catch-up
+ 状态消息 coalesce
```

### 大世界服务器

```text
多 I/O Reactor
+ scene/shard logic cells
+ FixedTick simulation
+ event-driven transaction side path
+ AOI 在业务层产生 BroadcastPlan
+ 网络层只执行 owner-grouped dispatch
```

---

# 九、性能和复杂度风险

## 1. 组合爆炸

如果有：

* 3 种 transport。
* 3 种 I/O topology。
* 4 种 logic model。
* 4 种 placement。
* 4 种 backpressure。

理论上就有 576 种组合，不可能全部验证。

解决办法不是减少架构能力，而是只维护少量“官方支持组合”：

```text
Profile A: SingleLoopInlineEvent
Profile B: MultiIoQueuedEvent
Profile C: MultiIoDedicatedFixedTick
Profile D: MultiIoShardedHybrid
```

其他组合允许实验，但不承诺生产等级。

## 2. 抽象热路径成本

风险包括：

* 多层 virtual call。
* `std::function`。
* `shared_ptr` 原子引用计数。
* payload 反复复制。
* 每包一次 heap allocation。
* 每命令一次跨线程 wakeup。

建议：

* 配置和组装阶段可以使用动态多态。
* 高频包路径优先 concrete type、批处理或静态策略。
* Broadcast 使用 immutable shared payload。
* Command 使用 move-only/value payload。
* 队列从 empty→non-empty 时才唤醒，后续提交合并。
* UDP/KCP 高频路径不必强制每个 datagram 都经过通用虚接口。

## 3. 跨线程 handoff 可能比网络本身更贵

分离 I/O 与逻辑意味着：

```text
解析
→ 构造命令
→ 加锁/原子
→ 入队
→ wakeup
→ cache miss
→ 逻辑处理
→ 再次调度回 endpoint owner
```

因此同线程和分线程都应存在，不能预设“分线程一定高性能”。

选择依据应是实际数据：

* 每消息跨线程次数。
* 每秒 wakeup 数。
* command queue oldest age。
* P99/P999 handoff latency。
* payload copy bytes。
* owner loop ready latency。
* CPU cache miss。
* logic tick jitter。

当前 EventLoop 指标已经包含 oldest pending latency、oldest ready latency、remaining work 和 budget exhausted，可直接作为运行模型比较的底层依据。

## 4. 多层背压可能形成反馈振荡

现在已有：

```text
connection hard limit
loop memory budget
server memory budget
global memory budget
logic queue limit
broadcast owner/global outstanding limit
```

如果各层独立触发 pause、drop、close，可能出现：

* 读暂停后逻辑队列已恢复，但无人恢复读取。
* 广播重试持续填满 EventLoop queue。
* 高低水位设置过近导致频繁 pause/resume。
* 关键控制包被普通广播挤压。
* 全局预算的原子热点。

需要明确压力优先级：

```text
进程安全硬限制
> server 安全限制
> loop 公平性限制
> connection 限制
> 业务优先级和降级策略
```

每层都必须有 hysteresis 和唯一 owner，不能多个模块同时恢复同一资源。

## 5. 亲和策略可能产生热点

当前 ConsistentHash 按 peer IP 选 I/O loop，适合作为稳定网络键，但不适合作为业务分片键。

典型热点：

* 大量玩家位于同一个 NAT。
* 一个超大房间拥有远高于其他房间的流量。
* 少量“鲸鱼玩家”产生大量请求。
* hash 均匀但业务成本不均匀。

因此需要分别观测：

* connection count。
* packet rate。
* pending queue latency。
* bytes/s。
* logic execution time。

而不是只看连接数量。

## 6. 停服生命周期复杂度上升

多模型下必须有统一停服顺序：

```text
1. 关闭新连接/新命令 admission
2. 停止认证和 session 新建
3. 停止或 drain logic execution
4. 将已产生输出提交给 endpoint owner
5. graceful drain transport output
6. force timeout fallback
7. 停止 worker loops
8. 销毁管理对象
```

这正是稳定内核中 final drain、typed shutdown、lifecycle source 等语义不能被策略化的原因。

---

# 十、适合当前阶段的渐进式路线

当前只有 `GameNet::core` 属于已审查的 stable surface；protocol、transport、session、logic、broadcast 和 metrics 仍是 provisional。API-R1 已要求 stable Core 在当前 0.3 线上保持 zero diff。

因此现在不适合为了“多模型”重构 `EventLoop`、`TcpServer` 或稳定头文件。

## 阶段 0：先完成 v0.3 候选冻结

当前优先级应保持：

```text
REL-C1 candidate freeze
→ same-SHA Linux/Windows
→ Sanitizer
→ performance
→ capacity
→ endurance
→ release
```

仓库路线图也明确当前没有最终 v0.3 candidate，REL-C1 是下一项工作。

这一阶段只建议在 0.4 分支准备架构 intent/ADR，不要再次改变 0.3 stable Core。

## 阶段 1：TCP-only Runtime Profiles

在不改变 `GameNet::core` 的情况下，新增 provisional/experimental runtime composition：

```text
runtime/
  RuntimeProfile
  ExecutionCell
  CommandSink
  TickPolicy
  LogicShardPolicy
  BackpressurePolicy
```

先实现三个受支持模型：

```text
R1 SingleLoopInlineEvent
R2 MultiIoQueuedEvent
R3 MultiIoDedicatedFixedTick
```

复用现有 Pipeline 示例作为试验场，而不是立刻发布全功能 `GameServerPipeline` 库。

## 阶段 2：修正 LogicLoop 的 Tick 语义

为 provisional `LogicLoop` 增加：

* FixedDelay。
* FixedRateSkipMissed。
* FixedRateBoundedCatchUp。
* max work duration。
* tick lag。
* overrun count。
* skipped tick count。
* oldest command age。

同时新增 QueuedEventDriven executor，避免所有分线程逻辑都必须等待 Tick。

## 阶段 3：引入逻辑分片

实现：

```text
ExecutionCellPool
LogicShardPolicy
player/room/scene affinity
per-cell queue and tick metrics
```

明确：

* I/O loop 不因玩家登录而迁移。
* 命令按业务 key 进入逻辑 cell。
* 输出回送 endpoint owner。
* 每个 key 只保证 cell 内顺序，不承诺跨 cell 全局顺序。

## 阶段 4：只迁移 UDP datagram foundation

先完成最小 UDP：

* owner-loop socket。
* datagram read budget。
* typed send result。
* bounded receive work。
* `UdpTransportEndpoint`。
* MTU capability 基础信息。
* lifecycle/shutdown contract。

此阶段不要同步引入完整 KCP、FEC、PMTU 研究栈。

## 阶段 5：KCP 与双通道 Session

当 UDP 基础稳定后，再实现：

```text
SessionTransportSet
  controlEndpoint: TCP
  realtimeEndpoint: UDP/KCP
```

并定义消息类别到 transport 的选择策略：

```text
登录/背包/支付      → TCP reliable ordered
技能输入/关键战斗   → KCP reliable
位置快照            → UDP best effort / latest value
聊天与系统通知      → TCP
```

## 阶段 6：基于证据提升官方模型

每个 Runtime Profile 至少需要独立证明：

* 生命周期和停服收敛。
* owner-thread 正确性。
* 队列饱和行为。
* P99/P999 延迟。
* Tick jitter 和 overrun。
* 每连接内存。
* 跨线程 wakeup 和 handoff 成本。
* 广播压力与恢复。
* Linux/epoll。
* Windows/IOCP。
* ASan/UBSan/TSan。
* capacity 和 endurance。

只有通过这些门的组合才进入“支持模型”，其余保持 experimental。

---

# 最终架构判断

`game-net-core` 不应使用唯一运行模型覆盖所有游戏，但也不应发展成多个互不一致的网络内核。

最合理的定位是：

```text
稳定部分：
    EventLoop 所有权
    线程亲和
    生命周期
    admission
    typed result
    bounded work
    内存预算
    timer 基础语义
    平台后端一致性

可组合部分：
    TCP / UDP / KCP
    单 / 多 Reactor
    连接分配
    I/O 与逻辑同线程 / 分离
    event-driven / fixed-tick / hybrid
    logic sharding
    broadcast
    backpressure action
```

对当前项目最重要的约束可以概括为三句话：

> **不要把“一条 EventLoop 一个 owner 线程”误解为“只能有一种运行模型”。**

> **不要把当前 FixedDelay LogicLoop 误解为所有游戏都适用的固定帧执行器。**

> **不要在 v0.3 候选冻结前修改已经通过 API-R1 的 stable Core；多模型应先在 provisional runtime 层演进。**

这条路线既能保留当前 Reactor/TCP 内核已经积累的大量生命周期、IOCP、背压和发布治理成果，也能让 `game-net-core` 真正覆盖大厅服、网关、房间服、MMO 世界服和实时动作服务器，而不被任何一种游戏形态绑死。
