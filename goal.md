# game-net-core 长期目标

更新日期：2026-08-20

形成本文时参考的治理提交：`1daa649`

该治理提交绑定的实现检查点：`9d2a5be0eb5439399f27c2f53ec1bf985c7de1d0`

历史发布治理绑定的实现检查点：`669ebb0a7c5c475dea74b12275c66a2ce1876804`。
历史候选 `v0.3.0-rel-c1-refreeze-5` 替代
`v0.3.0-rel-c1-refreeze-4@c061f9967b9481b70b2faf9a8fee24f5a3e72ffc`；这些引用只记录
不可变历史证据。从 2026-08-19 起，候选冻结不再是架构开发前置条件，当前执行改为
main 持续前进、每提交精确留证、需要推广时再选择 promotion commit。

## 1. 文档定位

本文定义 `game-net-core` 的长期产品目标、架构方向和演进边界，用于回答：

> 一个面向高性能游戏服务端的通用网络底座，应该统一什么，又应该允许什么变化？

本文不是当前实现授权，也不是发布证据。具体模块只有在对应 intent 被提升为
`active`，并同步完成 rules、contracts、tests 和 review 后，才进入实现范围。
当前发布事实、任务顺序和关闭证据仍分别以 `assessment.md`、`plan.md` 和
`docs/migration_status.md` 为准。

## 2. 一句话目标

`game-net-core` 的长期目标是：

> 建立一个统一、可验证的 owner-loop 并发与生命周期内核，在其下真实表达
> Readiness 与 Completion I/O 语义，在其上提供少量经过验证、可组合的游戏网络
> 运行模型，而不是用唯一 Reactor 形态覆盖所有平台和所有游戏负载。

可以概括为：

```text
一个并发与生命周期公理
+ 多种真实 I/O capability
+ 少量经过验证的运行模型 Profile
!= 多套互不兼容的网络内核
!= 任意策略自由拼装
```

## 3. 核心架构判断

### 3.1 EventLoop 继续存在，但重新明确定位

`EventLoop` 应继续作为单线程 owner scheduler / event pump，统一负责：

- owner-thread 与线程亲和；
- 跨线程 admission 和 wakeup；
- Timer、control、lifecycle 和普通任务调度；
- 各阶段公平预算；
- callback 上下文与异常 containment；
- `Running -> Quiescing -> FinalDraining -> Shutdown`；
- 已接受工作的 final drain。

它不应再被狭义地等同于“epoll Reactor”。Readiness 和 Completion 后端都可以由
一个 owner EventLoop 串行驱动，而不改变连接状态无锁、回调线程确定和显式跨线程
投递这些核心优势。

### 3.2 Reactor 不是唯一底层 I/O 语义

如果 Reactor 指“owner EventLoop 串行分发事件”，它仍是核心骨架；如果 Reactor
专指“内核通知 fd ready，应用随后执行 read/write”，它不应是唯一 I/O 模型。

目标架构必须能够分别表达：

- Readiness：epoll，未来可能的 kqueue；
- Completion：IOCP，未来实验性的 io_uring completion data path；
- 同一 backend 暴露多个 capability 的可能性，而不是按操作系统建立僵硬继承树。

Completion 不应长期伪装为 fake read/write readiness。已经完成的 operation 必须保留
自己的 identity、result、bytes、native error、generation、storage lease 和 terminal
retirement 语义。

### 3.3 游戏类型不直接决定运行模型

`FPS`、`MMO`、`回合制` 等名称只能提供推荐 Profile，不能硬编码底层模型。实际选择
应由以下负载特征驱动：

- 连接数量、活跃度、包频率和单包大小；
- 延迟、抖动、可靠性和有序性目标；
- 单次逻辑成本及其波动；
- 事件驱动、固定 Tick 或混合调度要求；
- 玩家、房间、场景和世界状态的分片能力；
- 广播扇出、慢客户端比例和允许的降级方式；
- 跨线程切换、内存、CPU 与 cache-locality 预算。

## 4. 目标分层

```text
┌──────────────────────────────────────────────┐
│                  游戏业务层                  │
│ Room / AOI / Actor / World / Lua / DB ...   │
└──────────────────────┬───────────────────────┘
                       │
┌──────────────────────▼───────────────────────┐
│                运行模型与策略层              │
│ Placement / Queue / Tick / Shard /           │
│ Broadcast / Backpressure / Runtime Profile   │
└──────────────────────┬───────────────────────┘
                       │
┌──────────────────────▼───────────────────────┐
│             传输与网络基础组件层             │
│ TransportEndpoint / Session / Framer /       │
│ TCP / future UDP or reliable datagram        │
└──────────────────────┬───────────────────────┘
                       │
┌──────────────────────▼───────────────────────┐
│             稳定 owner-loop 语义内核         │
│ EventLoop / Executor / Timer / Lifecycle /   │
│ Admission / Bounded Work / Typed Result      │
└──────────────────────┬───────────────────────┘
                       │
┌──────────────────────▼───────────────────────┐
│                    I/O Engine                │
│ Readiness capabilities | Completion          │
│ capabilities | wait / wakeup / quiescence    │
└──────────────────────────────────────────────┘
```

依赖只能向下。网络核心不得反向依赖 Room、AOI、Actor、World、脚本或部署拓扑。

## 5. 稳定语义内核必须统一的契约

以下内容不随游戏类型、I/O backend 或运行 Profile 改变。

### 5.1 Owner 与线程亲和

- 一个 EventLoop 绑定一个 owner 线程；
- 一个 I/O Engine 归一个 EventLoop 所有；
- 连接可变状态仅由其 owner 执行域修改；
- 注册、提交、取消和结果分发在 owner 线程完成；
- 跨线程调用只能通过明确、可拒绝的 executor/control/lifecycle 路径；
- 用户 callback 不在任意内核线程上执行。

### 5.2 Admission 与终局

- `Accepted` 表示内核或运行时已经取得明确义务；
- 被接受的工作必须执行，或产生可观察的取消/终止结果；
- `Rejected` 表示没有取得义务，之后不得凭空产生完成事件；
- admission 关闭后返回 `Shutdown` 或 `OwnerUnavailable`；
- 队列、operation slot、内存预算和每轮工作量必须有界。

对于 Completion，进一步要求：

```text
Accepted submission
    -> 0..N progress completions
    -> exactly 1 terminal completion
    -> owner-loop retirement
```

普通 one-shot operation 可以没有 progress completion；未来 multishot capability
也必须遵守唯一 terminal retirement。

### 5.3 身份、generation 与生命周期

- Readiness 使用 registration identity + generation；
- Completion 使用 operation identity + generation；
- fd/SOCKET 数值复用不能让旧事件命中新对象；
- stale readiness 不得执行 callback；
- completion observer 即使已撤销，kernel obligation 和 storage lease 仍须终结；
- source/operation 只能完成一次最终释放；
- 不允许 callback-after-destroy、kernel-reference-after-free 或 accepted work
  stranded at shutdown。

### 5.4 调度、公平性与 wakeup

- I/O、Timer、control、lifecycle 和 functor 分阶段且有界；
- budget exhausted 时保留剩余工作，并在下一轮继续而不是重新阻塞；
- wakeup 是可合并的调度中断，不是业务消息；
- 从 idle 到 runnable 的状态变化不能丢失；
- 公共调度预算与 backend capacity 配置必须分开。

### 5.5 错误与观测

- 高层使用稳定、可分类的 typed result；
- 调试信息保留 errno、WinSock error 或 CQE result 等 native error；
- 所有 Engine 提供可比较的 wait、dispatch、lag、budget 和 shutdown 指标；
- Readiness 与 Completion 可以额外提供各自专属指标，不制造虚假的统一字段。

## 6. Readiness 与 Completion 不应抹平的差异

| 维度 | Readiness | Completion |
| --- | --- | --- |
| 基本身份 | source/fd registration | 一次 operation submission |
| 通知含义 | 当前可能可以读写 | 某个操作已经完成 |
| I/O 发起 | ready 后执行 syscall | wait 前已经提交 |
| 结果 | ready mask/condition | operation、bytes、result、flags |
| 合并方式 | 同 source mask 可以合并 | 每个 completion 保留 operation identity |
| 缓冲区生命期 | 通常覆盖当前 syscall | 覆盖整个 in-flight operation |
| 取消 | 移除 interest/关闭 source | 请求取消后仍需 terminal retire |
| 读暂停 | 移除 read interest | 不 repost，必要时取消已提交 read |
| 关闭重点 | remove-before-destroy | cancel/drain/retire-before-release |

由此得到长期边界：

- `Channel` 是 Readiness registration 与 callback binding，不是万能 I/O 事件对象；
- Completion 通过 operation/sink 分发，不再转换成 fake `kReadEvent/kWriteEvent`；
- `TcpConnection` 保持统一的连接、发送、背压和关闭语义；
- source-private TCP I/O driver 分别实现 readiness 和 completion data path；
- public 兼容性通过 adapter、alias 或分阶段迁移维护，不在当前发布线上直接破坏。

## 7. 可组合运行模型

I/O 模型之外，项目应允许少量受约束的运行模型。

### 7.1 基础模型

- `InlineEventDriven`：I/O owner 上完成严格有界的轻量处理；
- `QueuedEventDriven`：I/O 向独立有界队列提交，逻辑尽快批量 drain；
- `DedicatedFixedTick`：独立逻辑执行域按明确 cadence 和预算处理；
- `ShardedHybrid`：事务路径事件驱动，实时模拟固定 Tick，并按玩家/房间/场景分片。

运行模型按正交维度组合：

```text
Runtime Model
    = Transport
    + I/O Topology
    + Connection Placement
    + Logic Placement
    + Dispatch Cadence
    + Broadcast Strategy
    + Backpressure Policy
```

但项目只承诺少量官方 Profile。任意组合只能保持 experimental，直到其生命周期、
饱和、性能、跨平台和 endurance 证据完整。

### 7.2 两种归属必须分开

- `ConnectionPlacementPolicy` 决定 socket 属于哪个 I/O owner；
- `LogicShardPolicy` 根据 player/room/scene key 决定命令属于哪个逻辑执行域；
- 已建立 TCP 连接不因业务亲和性迁移 owner loop；
- 逻辑输出通过 `TransportEndpoint::ownerExecutor()` 回到连接 owner。

## 8. 核心边界与非目标

### 8.1 核心负责

- owner-loop、I/O Engine 和平台后端集成；
- socket/operation 生命周期；
- 有界 admission、资源记账、Timer 和 wakeup；
- TCP 连接机械语义；
- typed result、基础观测和可验证的 shutdown；
- 向上提供窄的 transport/executor 能力边界。

### 8.2 核心不负责

- Room、AOI、Actor、World、ECS 和玩家业务状态；
- 账号、支付、背包、邮件、匹配和数据库事务；
- Lua VM 或业务脚本调度器；
- 多进程、多节点网关编排和部署控制面；
- 用游戏类型枚举自动选择全部策略；
- 把业务消息语义、最新值覆盖或降级规则硬编码进网络层；
- 为追求统一接口而隐藏 Readiness/Completion 的关键差异；
- 同时维护多套互不兼容的 EventLoop、生命周期和关闭系统。

## 9. 性能与复杂度约束

架构扩展必须避免“抽象正确但热路径退化”。重点约束包括：

- 配置/组装阶段可以动态多态，包级热路径优先具体类型、批处理和窄接口；
- 不默认引入每包 allocation、`shared_ptr` 计数、`std::function` 或跨线程 wakeup；
- payload 优先 move/value 或 immutable shared storage；
- 只在 empty -> non-empty 等必要状态变化时唤醒；
- 不通过无界队列掩盖模型不匹配；
- 不为组合能力创建“每种组合一个 Server 类”；
- 不在没有 benchmark、capacity 和 lifecycle 证据时提升官方 Profile。

必须持续测量：

- P50/P99/P999 I/O 与 handoff latency；
- wakeup、线程跳转和每消息分配次数；
- pending/ready oldest age；
- Tick jitter、overrun、skip 和 bounded catch-up；
- bytes copied、in-flight operation、队列与保留内存；
- 慢客户端、广播和 shutdown 压力下的恢复时间；
- Linux/epoll 与 Windows/IOCP 的同场景差异。

## 10. 当前仓库与目标的关系

当前仓库已经具备目标架构的基础：

- `EventLoop` 已拥有 owner、admission、公平预算和 final-drain 状态机；
- epoll 已由 generation-safe Readiness Engine 驱动，Channel 保留在真实 readiness
  路径；
- IOCP 已直接分发带 identity/result/bytes/generation 的 Completion notice，fake
  Channel readiness 兼容路径已退役；
- Linux 已有 default-off、non-installed 的真实 raw-syscall io_uring one-shot
  Completion Engine，Accept/Recv/Send、SQ-full、cancel、lease 和 final drain 合同及
  opt-in 数字基准均已闭环；其 source-private EventLoop completion pump 已通过真实
  ring-fd Channel 驱动、独立 dispatch budget、quit 自动取消和 terminal lease 退休
  合同；其单连接 Completion TCP driver 又在真实 loopback TCP 上证明 one-recv-in-flight、
  有限 FIFO send、pause/no-repost、callback re-entry 和 socket-after-terminal 退休；其
  shared-Pump Hub 进一步在一个 owner Engine 上 generation-safe 路由多个真实 TCP route，
  证明 per-route/aggregate 预算、隔离关闭、slot 换代复用和 owner-quit aggregate drain；
  固定 256-route/64-churn 容量与结构化 Release 数字又给出受限 adapter `PROMOTE`；
  source-private semantic adapter 已把稳定 `TcpSendResult`/close info、low/high/hard
  backpressure、回调重入和可等待终局映射到单个 Hub route；下一合同继续补齐 graceful
  pending-output drain 与 TCP half-close。这些能力仍不安装，生产默认和 fallback 仍是 epoll；
- `TransportEndpoint` 已缩窄上层对 `TcpConnection` 的依赖；
- `SingleLoopInlineEvent` 已证明单 owner、零跨域 handoff；`MultiIoQueuedEvent` 已证明
  多 I/O owner、独立逻辑 owner、有界合并唤醒和 generation-safe 回送；
  `MultiIoDedicatedFixedTick` 已证明权威 fixed-rate tick、skip/bounded catch-up、有界
  tick drain 和双阶段停止；`MultiIoShardedHybrid` 已证明连接放置与逻辑分片独立、
  多 cell 有界隔离、cell 内顺序以及 event/tick 共存。四个 Runtime Profile 都保持
  非安装。`LogicLoop`、Pipeline 示例和 Broadcast 则展示其他跨执行域组合与有界背压。

当前张力也已经明确：

- 四个 TCP Runtime Profile 已完成垂直合同；跨 Profile 共同能力审查以
  `NO-PROMOTION` 关闭，Profile D 的 logic sharding 与 Hybrid 合同不自动授权公共抽象；
- Profile A/B/C/D 的组合接口仍故意留在非安装 example/support 层；四个 Profile 只触发
  后续共同能力审查，不自动授权提升公共抽象；
- I/O Engine 的部分兼容 ABI/layout 仍保留在 0.3 stable surface，物理清理必须等
  明确审查的 breaking line；
- IOE-X1/X2/X3/X4/X5/X6 只证明隔离 one-shot completion、EventLoop 驱动、单连接、
  固定容量 shared-Pump 以及 source-private forced-close/backpressure adapter 语义。
  capacity/soak 与方向性数字已经允许继续做 adapter contract shaping，但不自动授权
  production TcpConnection 集成、公共 backend selector、graceful half-close 等价或
  multishot/provided-buffer 等高级能力；
- 当前 `LogicLoop` 更接近周期性有界 drain，不应被描述为所有游戏适用的权威
  FixedRate Tick。

这些张力是后续设计输入，不是立即重写 stable Core 的理由。

## 11. 演进原则

长期演进必须遵循：

1. 不把候选冻结或发布决定作为架构演进前置条件；main 持续前进，证据绑定精确
   commit，需要推广时再选择 promotion commit；
2. 先写 active intent、rules 和 contract，再改核心实现；
3. 先建立不改变行为的 source-private adapter；
4. 先用 epoll 证明 Readiness 热路径无明显回归；
5. 再让 IOCP 直接发布 Completion notice，移除 fake readiness；
6. io_uring 只作为实验性真实 completion data path，从 one-shot 闭环开始；
7. TCP-only Runtime Profiles 可在 provisional 层验证，不反向污染 stable Core；
8. UDP、可靠数据报、双通道 Session 等必须在各自 intent 被正式提升后推进；
9. 每个阶段只提升经过 Linux/Windows、饱和、关闭和性能证据验证的组合；开发集成与
   对外推广分离，缺少 endurance 或许可证只阻塞推广，不阻塞下一能力切片。

## 12. 目标验收标准

当以下条件成立时，长期目标才算达到可推广状态：

- EventLoop 文档与接口不再把统一并发内核等同于单一 Readiness Reactor；
- epoll 与 IOCP 在同一 owner-loop 生命周期下保留各自真实 I/O 语义；
- Completion 不再依赖 fake Channel event 传递 operation result；
- Readiness registration 与 Completion operation 都有 generation-safe 生命周期；
- accepted submission、取消、terminal completion 和 final drain 可被合同完整证明；
- 至少三个 TCP Runtime Profile 有明确适用条件和跨平台证据；
- Profile 选择基于负载指标而非游戏类型硬编码；
- 不存在未授权的业务状态、部署拓扑或协议研究能力进入 Core；
- 所有官方能力都有 intent、rules、tests、benchmark 和 release evidence；
- 新抽象没有造成不可接受的吞吐、延迟、内存或维护复杂度回归。

## 13. 最终原则

`game-net-core` 不追求一个“适合所有游戏的万能 Reactor”，也不追求多个彼此独立的
网络内核。它追求的是：

> 一个稳定的 owner-loop 并发与生命周期内核，连接多种真实 I/O capability，并向上
> 支撑少量经过证据验证的游戏网络运行模型。

所有未来设计都应接受三个问题的检验：

1. 它是否保持统一、可推理的 owner 与生命周期语义？
2. 它是否真实表达平台和负载差异，而不是制造虚假统一？
3. 它是否仍然是网络底座能力，而不是把游戏业务框架搬进 Core？
