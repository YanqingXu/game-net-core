game-net-core 是否应该继续以 owner-loop/EventLoop 作为统一并发内核，同时将底层 I/O 从单一 Reactor/Poller 抽象演进为可分别容纳 Readiness（epoll/kqueue）和 Completion（IOCP/io_uring）语义的 I/O Engine？两类后端应共享哪些生命周期与调度契约，哪些差异不应被强行抹平？

# 架构结论

**应该继续以 owner-loop / `EventLoop` 作为统一并发内核，同时将底层 I/O 从“单一 Poller 抽象”演进为同时容纳 Readiness 与 Completion 语义的 `I/O Engine`。**

但关键不是简单地把 `Poller` 改名为 `IoEngine`，而是重新划定职责：

```text
EventLoop
    = 串行执行域
    + 调度公平性
    + 跨线程 admission
    + Timer / Control / Lifecycle
    + Quiescing / Final Drain

I/O Engine
    = 内核 I/O 集成
    + wait / wakeup
    + readiness registration
    + completion submission
    + kernel obligation tracking
```

最终应形成：

```text
统一的是：
    owner-loop、生命周期、线程亲和、调度和关闭语义

不统一的是：
    Readiness 与 Completion 的 I/O 操作模型
```

也就是说，未来不应继续描述为：

> “Linux 和 Windows 都使用 Reactor/Poller，只是 Windows 的 Poller 内部叫 IOCP。”

而应描述为：

> “Linux epoll/kqueue 与 Windows IOCP、Linux io_uring 都挂在同一个 owner-loop 调度内核下，但使用不同的 I/O Engine capability。”

---

# 一、为什么 owner-loop / EventLoop 应继续保留

Completion I/O 并不意味着必须放弃 EventLoop，也不意味着必须采用多个线程同时消费一个 completion port。

当前 `EventLoop` 已经沉淀了比“Reactor”更重要的语义：

- 每个 loop 只有一个 owner 线程。
- 连接状态只在 owner 线程修改。
- 跨线程工作通过有界队列提交。
- callback 上下文确定。
- Timer、control、lifecycle 和普通 functor 有明确调度阶段。
- `quit()` 会封闭新的 admission。
- 已接受的工作必须在退出前 drain。
- completion obligation 未归零时不能结束 loop。

这些才是 `game-net-core` 真正值得稳定下来的并发内核。

当前 `EventLoop` 的退出状态机已经是：

```text
Running
    ↓ quit / seal external admission
Quiescing
    ↓ drain I/O + functors + control + lifecycle
FinalDraining
    ↓ reach fixed point
Shutdown
```

在 Quiescing 和 FinalDraining 阶段，代码会持续以零超时消费后端事件，并将 `Poller::hasPendingCompletionOperations()` 作为退出条件之一。这已经是一个适用于 completion backend 的正确关闭骨架。

因此，未来应当做的是：

> **把 `EventLoop` 从“Reactor 调度器”重新定义为“单线程 owner scheduler / event pump”。**

类名可以继续叫 `EventLoop`，不必为了理论纯洁性改名，但文档中不应再把它和 Readiness Reactor 完全等同。

---

# 二、当前 `Poller` 抽象已经出现明显张力

当前 `Poller` 的基本接口是典型 Readiness 形状：

```cpp
poll(timeout, activeChannels)
updateChannel(channel)
removeChannel(channel)
```

它维护 fd 到 `Channel*` 的注册关系，并返回 active Channel 列表。

对于 epoll，这个抽象非常自然：

```text
注册 fd + interest mask
    ↓
epoll_wait()
    ↓
返回可读/可写/错误/关闭状态
    ↓
用户态再执行 recv/send/accept
```

当前 `EPollPoller` 正是把 `EPOLLIN/EPOLLOUT/EPOLLERR/EPOLLHUP` 映射为 `Channel` 事件，然后由 `Channel` 调用 read/write/error/close callback。

但 IOCP 并不返回“哪个 socket 已经可以读写”，而是返回：

```text
哪个 OVERLAPPED 操作完成了
+ 完成了多少字节
+ 完成状态或错误
+ 对应的 completion key
```

`GetQueuedCompletionStatusEx` 返回的条目包含已完成操作使用的 `OVERLAPPED` 地址、传输字节数和 completion key；它本质上是 operation completion，而不是 fd readiness。

当前实现为了兼容 `Poller → activeChannels` 的接口，采用了如下转换：

```text
IOCP completion packet
    ↓
IocpOperation { kind, bytesTransferred, error }
    ↓
kind 转换为 Channel::kReadEvent / kWriteEvent
    ↓
把 IocpOperation* 临时塞入 Channel
    ↓
EventLoop dispatch Channel callback
    ↓
TcpConnection 再取出 operation 并 completeRead/completeWrite
```

`IocpPoller` 明确把 Accept/Read completion 映射为 read event，把 Connect/Write completion 映射为 write event；同一 Channel 在一个 batch 中出现多个 completion 时，还必须把后续 operation 放进固定 deferred batch，延迟到下一轮发布。

`Channel` 因此出现了 Windows 专用字段：

```cpp
IocpOperation* iocpCompletionOperation_;
IocpOperation* iocpAcceptCompletionHead_;
IocpOperation* iocpAcceptCompletionTail_;
```

以及对应的 `take/append` 接口。

这不是实现错误。它是当前迁移阶段为保持上层契约所作的合理兼容。但从长期架构看，它说明：

> **Completion 被迫伪装成 Readiness，已经开始污染 `Poller`、`Channel`、`EventLoopOptions` 和 Metrics。**

例如当前稳定 `EventLoopOptions` 已经直接出现：

```cpp
maxActiveChannelsPerIteration
maxIocpCompletionsPerPoll
```

指标中也同时存在：

```cpp
ActiveChannelsDrained
IocpCompletionPacketsDrained
```

这些都是底层抽象边界需要演进的信号。

仓库早期的 epoll/IOCP 设计文档其实已经预判了这个问题：如果 readiness `Poller` 兼容面变得过于扭曲，应在 `EventLoop` 下引入窄的 completion adapter，而不是继续强行维持假统一。

---

# 三、推荐的目标架构

建议演进为：

```text
┌─────────────────────────────────────────────┐
│                  EventLoop                  │
│                                             │
│  Owner Thread                               │
│  Executor Admission                         │
│  TimerQueue                                 │
│  Control / Lifecycle                        │
│  Dispatch Budgets                           │
│  Quiescing / Final Drain                    │
└──────────────────────┬──────────────────────┘
                       │ owns
┌──────────────────────▼──────────────────────┐
│                   IoEngine                  │
│                                             │
│  waitUntil(deadline)                        │
│  wakeup()                                   │
│  beginQuiesce()                             │
│  quiescent()                                │
│  metrics()                                  │
└───────────────┬─────────────────┬───────────┘
                │                 │
      Readiness capability   Completion capability
                │                 │
      register/modify/remove submit/cancel/retire
                │                 │
          epoll / kqueue       IOCP / io_uring
                │                 │
           ReadyNotice         CompletionNotice
                └────────┬────────┘
                         ▼
               EventLoop owner dispatch
```

这里有一个重要设计点：

## 不建议只做刚性的类继承树

不要简单设计成：

```cpp
class IoEngine;
class ReadinessEngine : public IoEngine;
class CompletionEngine : public IoEngine;
```

更适合的是：

```cpp
class IoEngine {
public:
    virtual WaitSummary waitUntil(
        std::chrono::steady_clock::time_point deadline,
        IoEventBatch& batch) = 0;

    virtual WakeupResult wakeup() noexcept = 0;
    virtual void beginQuiesce() noexcept = 0;
    virtual bool quiescent() const noexcept = 0;

    virtual IoEngineCapabilities capabilities() const noexcept = 0;

    virtual ReadinessPort* readinessPort() noexcept = 0;
    virtual CompletionPort* completionPort() noexcept = 0;
};
```

原因是 `io_uring` 不一定只能提供单一语义。它既可以直接提交 recv/send/accept 等异步操作，也可以提交 poll operation；一个 poll submission 甚至可以通过 multishot 产生多次 CQE。因此，长期最好把 Readiness 和 Completion 定义为 **capability**，而不是把每个操作系统后端锁死成一个简单类别。

不过对于 `game-net-core` 的首个 io_uring 版本，应当明确选择：

> **真实 completion TCP data path，而不是仅用 `IORING_OP_POLL_ADD` 包装出另一个 epoll。**

否则只是更换 syscall，没有验证 Completion 架构。

---

# 四、两类后端应该共享的契约

## 1. Owner 与线程亲和契约

无论使用 epoll、kqueue、IOCP 还是 io_uring，都应保持：

```text
一个 IoEngine 属于一个 EventLoop
所有注册、提交、取消和完成分发都由 owner 线程执行
跨线程调用只通过 EventLoopExecutor / control / lifecycle lane
用户 callback 永远不在任意内核工作线程上执行
```

IOCP 本身允许多个线程等待同一个 completion port，但这只是操作系统能力，不意味着项目必须采用该并发模型。`game-net-core` 应继续选择每个 owner loop 串行消费自己的 I/O 结果，从而维持现有连接状态无锁、回调上下文确定的优势。Microsoft 文档确认同一 completion port 可以被多个线程等待，但完成包只满足其中一个等待线程；项目完全可以有意限制为单 consumer owner loop。

## 2. Admission 线性化契约

统一定义：

```text
在 Running 阶段被接受的 I/O 工作：
    必须产生一个可观察的终局

进入 Quiescing 后的新工作：
    必须返回 Shutdown / OwnerUnavailable

不能出现：
    API 返回 Accepted，但工作既未完成、也未取消、也未被 drain
```

对于 Completion，应该更严格：

```text
SubmitResult::Accepted
    => engine 已取得操作义务
    => operation 最终必须 terminal-retire

SubmitResult::Rejected
    => engine 未取得操作义务
    => 以后绝不能凭空出现 completion
```

当前 Windows 实现已经采用这一原则：同步且非 `WSA_IO_PENDING` 的提交失败不创建 completion obligation；真正进入 pending 或同步成功的 overlapped operation 才拥有未来 completion。

## 3. 身份与 generation 契约

所有 I/O source 或 operation 都必须有稳定身份：

```text
Readiness:
    RegistrationId + Generation

Completion:
    OperationId + Generation
```

共同保证：

- fd/SOCKET 数值复用不能让旧事件命中新对象。
- 已注销 source 的 stale readiness 不得执行 callback。
- 已取消或 observer 已撤销的 completion 仍然要释放 storage lease。
- 同一个 operation 只能 terminal-retire 一次。
- 旧 generation 的事件不能修改新 generation 的连接状态。

当前 `Channel` active-batch generation、同 fd replacement 检查以及 IOCP association rollback，已经证明这是跨后端都需要保留的基础契约。

## 4. 生命周期与销毁契约

统一的上层目标是：

```text
没有 callback-after-destroy
没有 kernel-reference-after-storage-free
没有 stale registration
没有 Accepted work stranded at shutdown
```

但实现上必须分成两套规则：

### Readiness

```text
disable interests
→ unregister
→ invalidate generation
→ destroy source
```

### Completion

```text
seal new submissions
→ request cancel / close observer
→ retain operation + referenced storage
→ dequeue terminal completion
→ release kernel obligation
→ retire operation
→ destroy owner
```

当前 IOCP 生命周期规则已经明确：成功提交的 overlapped operation 拥有一个未来 completion packet；取消前必须先建立 shutdown obligation；`ERROR_NOT_FOUND` 也不能证明 completion 已经消费。

## 5. Owner-loop callback 契约

两种后端都应保证：

- connection、message、write-complete、close callback 在 owner loop。
- callback 可以按照文档约定重入 owner-safe API。
- 异常由 EventLoop 统一 containment。
- callback 可以关闭当前连接。
- callback 返回后不能再次访问已撤销对象。

但不要承诺底层事件的微观顺序完全相同。

## 6. 公平性契约

两类 backend 都必须有界，但“预算单位”可以不同：

```text
共同预算：
    每轮最多 dispatch 多少 I/O notice
    每轮最多执行多少 Timer
    每轮最多执行多少 functor/control/lifecycle
    budget exhausted 时下一轮不得再次阻塞

Readiness 专属预算：
    每个 source 最多 syscall 次数
    每个 source 最多读取字节
    accept-loop 最大 accept 数
    读到 EAGAIN 前是否允许提前让出

Completion 专属预算：
    每轮最多 dequeue CQE/completion packet 数
    每连接最多处理多少 completion
    最大 in-flight operation 数
    SQ/CQ 或 operation-slot 容量
```

当前 `EventLoop` 已经对 active Channel、Timer、control、lifecycle 和 functor 分别设置预算；这一调度框架应继续保留。

## 7. Wakeup 契约

共享语义：

```text
wakeup 是调度中断
不是业务消息
允许合并
不能丢失从 idle 到 runnable 的状态变化
```

但实现不应强制为一个“可读 fd”：

- epoll 可以使用 eventfd/pipe。
- kqueue 可以使用 `EVFILT_USER` 或其他平台机制。
- IOCP 使用 `PostQueuedCompletionStatus`。
- io_uring 可以使用适合其事件泵的机制。

当前 IOCP 已经通过原子 `wakeupPending_` 合并 physical completion packet，这比把所有后端伪装成 wakeup Channel 更自然。

未来可让 wakeup primitive 完全归 `IoEngine` 所有，`EventLoop` 只调用 `engine.wakeup()`。

## 8. 统一错误分类，但保留 native error

建议统一高层类别：

```cpp
enum class IoTerminalStatus {
    Success,
    PeerClosed,
    Cancelled,
    TimedOut,
    ResourceExhausted,
    OwnerShutdown,
    BackendFailure,
};
```

同时保留：

```cpp
NativeError {
    platform;
    code;
}
```

这样上层可以跨平台决策，调试时又不会丢失 `errno`、WinSock error、NTSTATUS 或 CQE `res`。

---

# 五、绝对不应强行抹平的差异

| 维度 | Readiness：epoll/kqueue | Completion：IOCP/io_uring |
|---|---|---|
| 基本对象 | fd/source registration | 一次具体 operation submission |
| 内核通知含义 | “当前可能可以读/写” | “这个操作已经完成” |
| 实际 I/O 发起时间 | 收到 readiness 后调用 recv/send | wait 前已提交 recv/send/accept |
| 返回信息 | ready mask，可能附带 EOF/data hints | operation identity、result、bytes、flags |
| 通知合并 | 同一个 source 的 read/write mask 可以合并 | 每个 completion 必须保留 operation identity |
| 用户态动作 | 循环 syscall，直到 EAGAIN 或预算耗尽 | 消费结果，然后决定是否 repost |
| 缓冲区生命周期 | syscall 执行期间有效通常足够 | 按具体操作要求保持到提交或 completion |
| 取消 | 通常是删除 interest、关闭 fd | cancel 是异步请求，目标 operation 仍需 terminal retire |
| 读暂停 | 移除 read interest | 不 repost 下一次 read，必要时取消当前 read |
| 写背压 | 用户态 buffer + EPOLLOUT 开关 | in-flight write + segment queue + submission capacity |
| accept | readiness 后循环 accept | 预提交 AcceptEx / accept SQE |
| 错误发现 | readiness 后 syscall 或 `SO_ERROR` | completion result 直接携带错误 |
| 触发风格 | level/edge/oneshot/filter condition | one-shot/multishot operation completion |
| 关闭义务 | remove-before-destroy | cancel/observe-terminal-completion-before-release |

epoll 和 kqueue 都属于 readiness/condition notification 家族，但也不应被视为完全等价。kqueue 通过 `(ident, filter)` 标识事件，`EVFILT_READ/WRITE` 还可以携带可读字节、剩余写空间和 EOF 等 filter-specific 数据；这些信息不应为了迁就 epoll mask 而全部丢掉。

IOCP 的 completion entry 返回原始 `OVERLAPPED` identity 和已传输字节数；io_uring 的 CQE 通过 `user_data` 回传 submission identity，并在 `res/flags` 中携带结果。它们可以共享“operation completion”大语义，但不应强行共享同一块 native operation struct。

---

# 六、Completion 不应继续伪装成唯一 active Channel

这是此次演进最重要的具体边界。

当前 `Poller` intent 规定：

> 一次 poll result 中每个 Channel 最多出现一次；同一 Channel 的额外 IOCP operation 要延迟到下一轮。

这对 Readiness 很合理，因为 read/write/error mask 可以合并；但对 Completion 并不是天然规律。

例如同一个 TCP 连接可能在一批中同时出现：

```text
Read operation completion
Write operation completion
Cancel operation completion
```

它们不是同一个“可读写状态”，而是三个独立、已经发生的事实。

建议新 Engine 返回内部事件：

```cpp
struct ReadinessNotice {
    ReadinessRegistrationId registration;
    ReadyMask ready;
    std::uint64_t generation;
};

struct CompletionNotice {
    OperationId operation;
    IoOperationKind kind;
    std::ptrdiff_t result;
    NativeError error;
    CompletionFlags flags;
    std::uint64_t generation;

    // source-private，确保完成分发前相关 operation/storage 仍存在。
    LifetimeLease lease;
};

using IoNotice =
    std::variant<ReadinessNotice, CompletionNotice>;
```

然后：

```text
ReadinessNotice
    → ReadinessChannel / source callback

CompletionNotice
    → CompletionOperation / CompletionSink
```

不要再经过：

```text
CompletionNotice
    → fake kReadEvent/kWriteEvent
    → Channel
```

这样可以移除：

- `Channel` 中的 IOCP operation 指针。
- 同 Channel completion deferred queue。
- completion 被读写 event mask 覆盖的问题。
- `EventLoop` 对具体 `IocpPoller` 的 friend/dynamic_cast。
- `Poller` 中与 readiness 无关的 retention/obligation virtual hook。

---

# 七、Channel 应正式降格为 Readiness 专用抽象

建议长期语义是：

```text
Channel
    = ReadinessRegistration + callback binding

不是：
    = 所有 I/O 后端的万能事件对象
```

可以有两种处理方式。

## 兼容方式

继续保留类名 `Channel`，但在文档中明确：

> `Channel` 只用于 readiness source；completion backend 不通过 Channel 传递 operation result。

这种方式对现有 API 破坏最小。

## 更干净的后续方式

在 1.0 前的后续兼容线上重命名内部概念：

```text
Channel → ReadinessChannel
```

公共兼容 alias 可以保留一段时间：

```cpp
using Channel = ReadinessChannel;
```

Completion 侧则引入：

```cpp
class CompletionOperation;
class CompletionSink;
class CompletionPort;
```

---

# 八、TcpConnection 应通过 I/O Driver 适配两种模型

`TcpConnection` 的公共网络语义可以统一：

- connected / disconnected。
- send admission。
- output pending。
- read pause/resume。
- shutdown / force close。
- high/low-water。
- callback owner affinity。

但底层读写驱动不能完全统一。

建议引入 source-private：

```cpp
class TcpIoDriver {
public:
    virtual ~TcpIoDriver() = default;

    virtual IoModel model() const noexcept = 0;

    virtual ReadStartResult ensureReadActive(
        std::size_t maxBytes) = 0;

    virtual WriteStartResult ensureWriteActive() = 0;

    virtual void pauseRead() = 0;
    virtual void resumeRead() = 0;

    virtual CancelResult cancelPending() noexcept = 0;
    virtual bool quiescent() const noexcept = 0;
};
```

具体实现：

```text
ReadinessTcpDriver
    Channel + recv/send-until-EAGAIN

IocpTcpDriver
    WSARecv / WSASend + OVERLAPPED

IoUringTcpDriver
    recv/send SQE + CQE
```

当前 `IocpTcpTransport` 已经是这一方向的原型：它独立拥有 read/write `IocpOperation`、read storage、write segments、pending 状态和 CancelIoEx 行为；`IocpPoller` 只观察这些 operation。

下一步不是删除这种分离，而是进一步让它脱离 `Channel`：

```cpp
// 当前
IocpOperation::channel

// 未来
CompletionOperation::sink
CompletionOperation::ownerGeneration
CompletionOperation::lifetimeLease
```

---

# 九、io_uring 还要求 completion contract 支持 multishot

不能把 Completion 简化为：

```text
一次 submit
→ 恰好一次 completion callback
```

对于普通 one-shot operation，可以这样；但 io_uring 的 multishot accept、recv、poll 可以由一个 submission 产生多次 CQE，直到最终 CQE 不再包含 `IORING_CQE_F_MORE`。取消 multishot operation 时，取消请求本身和目标 operation 的终止也可能分别产生 CQE。

因此建议定义：

```text
一次 Accepted submission
    可以产生：
        0..N 个 Progress Completion
        恰好 1 个 Terminal Completion

只有 Terminal Completion：
    才释放 operation obligation 和 operation storage
```

状态可表示为：

```text
Created
  ↓ submit accepted
Active
  ↓ progress completion, MORE=true
Active
  ↓ final success/error/cancel, MORE=false
Terminal
  ↓ owner-loop retire
Retired
```

IOCP 当前主要是 one operation 对应一个 completion packet，但它同样可以遵循这一更一般的 contract。

---

# 十、配置和指标也应拆分

当前配置同时混合：

```cpp
maxActiveChannelsPerIteration
maxIocpCompletionsPerPoll
```

建议拆成两层：

```cpp
struct EventLoopSchedulingOptions {
    std::size_t maxIoDispatchesPerIteration;
    std::size_t maxTimersPerIteration;
    std::size_t maxFunctorsPerIteration;
    std::size_t maxControlCallbacksPerIteration;
    std::size_t maxLifecycleCallbacksPerIteration;
};

struct EpollEngineOptions {
    std::size_t initialEventCapacity;
    TriggerMode triggerMode;
};

struct IocpEngineOptions {
    std::size_t dequeueBatchSize;
    std::size_t maxInflightOperations;
};

struct IoUringEngineOptions {
    std::size_t submissionQueueEntries;
    std::size_t completionQueueEntries;
    std::size_t maxInflightOperations;
    bool enableMultishotAccept;
};
```

其中：

- `maxIoDispatchesPerIteration` 是 EventLoop 公平性契约。
- `dequeueBatchSize`、SQ/CQ size 是 backend capacity。
- 两者不能混为同一个参数。

指标同理：

```text
公共：
    wait duration
    collected notices
    dispatched notices
    remaining notices
    oldest notice latency
    budget exhausted
    wakeup / merged wakeup
    owner shutdown rejects

Readiness：
    registered sources
    ready masks
    source read/write budget exhaustion
    EAGAIN exits

Completion：
    submitted operations
    rejected submissions
    in-flight operations
    progress completions
    terminal completions
    cancellation requests
    cancellation terminals
    CQ/SQ pressure
    retained operation bytes
```

---

# 十一、适合当前仓库的渐进式迁移路线

## 阶段 0：不要打断当前 v0.3 候选线

API-R1 已经批准当前 0.3 stable Core surface，并启用了严格 zero-diff gate；后续 stable header、target 或 fingerprint 漂移都会失败。当前还没有最终 v0.3 production-candidate，REL-C1 仍是下一项任务。

因此：

> **不要在 v0.3 candidate freeze 之前重构稳定 `EventLoop/Poller/Channel` 表面。**

现在可以先写 intent、ADR、实验分支和 contract test，但不要重新打开已审查的 stable surface。

## 阶段 1：先定义 Engine contract，不改行为

新增 source-private：

```text
src/core/io/IoEngine
src/core/io/IoNotice
src/core/io/ReadinessPort
src/core/io/CompletionPort
```

第一步只让现有 `Poller` 通过 adapter 接入：

```text
EventLoop
  → PollerIoEngineAdapter
  → existing Poller
```

功能完全不变，先验证：

- EventLoop 调度顺序不变。
- active-batch invalidation 不变。
- shutdown fixed point 不变。
- benchmark 无明显回归。

## 阶段 2：迁移 epoll，建立纯 Readiness 基准实现

实现：

```text
EpollEngine
ReadinessRegistration
ReadinessNotice
```

让 `Channel` 只依赖 `ReadinessPort`。

此阶段应保持 Linux 行为完全一致，用它证明 Engine API 没有损害 readiness 热路径。

## 阶段 3：让 IOCP 直接发布 CompletionNotice

逐步移除：

```text
completionEvents(IocpOperation) → Channel event mask
Channel::iocpCompletionOperation_
IocpPoller same-Channel deferred-as-readiness
EventLoop dynamic_cast<IocpPoller*>
```

替换为：

```text
GQCSEx
→ CompletionNotice batch
→ owner-loop CompletionSink
→ IocpTcpDriver::onCompletion()
```

同一连接的 read/write completion 可以在同一批中保留为两个独立 operation；连接 state machine 负责确定处理顺序和关闭收敛，而不是由 Poller 通过“每 Channel 一次”规则人为延迟。

## 阶段 4：将 wakeup 收入 Engine

让：

```cpp
EventLoop::wakeup()
```

只调用：

```cpp
ioEngine_->wakeup();
```

- EpollEngine 内部拥有 eventfd。
- IocpEngine 内部使用 PostQueuedCompletionStatus。
- 后续 KqueueEngine/IoUringEngine 各自选择适当实现。

这样 EventLoop 不再构造跨平台 fake wakeup Channel。

## 阶段 5：实验性 io_uring Completion Engine

首版只做最小闭环：

- one-shot accept。
- one-shot recv。
- one-shot send。
- cancellation。
- SQ full typed rejection。
- completion result。
- quit/final-drain。
- operation storage lifetime。
- Linux fallback 到 epoll。

暂不同时引入：

- multishot。
- provided buffer ring。
- fixed files。
- zero-copy。
- SQPOLL。
- linked operations。
- advanced batching。

先证明最基础的 completion lifecycle，再逐项通过 intent promotion 增加。

## 阶段 6：增加 multishot 和 buffer capability

在基础 one-shot 路径稳定后，再增加：

```text
MultishotAccept
MultishotRecv
ProvidedBufferPool
CompletionFlags::More
BufferLease
```

并验证：

- progress completion 不提前释放 operation。
- final completion 唯一。
- buffer exhaustion。
- cancel 与 final CQE。
- CQ pressure 和公平性。

## 阶段 7：kqueue Readiness Engine

当前仓库只支持 Linux 和 Windows，其他系统在 configure 阶段失败；kqueue 仍是未来可能性。

等 macOS/BSD 平台被正式提升时，kqueue 应作为另一个 `ReadinessPort` 实现，而不是模仿 epoll 的所有 event bit。

---

# 十二、需要建立的测试矩阵

## 所有 Engine 都必须通过

```text
owner-thread dispatch
cross-thread wakeup
admission seal
accepted-work final drain
callback exception containment
source/operation generation
fd/handle reuse
callback 内关闭和销毁
quit 与 I/O 并发
bounded dispatch fairness
no callback after detach
```

## Readiness 专属

```text
read/write interest enable/disable
level-trigger drain
edge-trigger drain
EAGAIN convergence
stale readiness after unregister
同 fd replacement
accept/read byte budget
```

## Completion 专属

```text
synchronous submit failure => no completion
accepted submit => terminal retirement
cancel request ≠ target completion
cancel + immediate quit
operation and buffer lease
multiple completions for one connection
out-of-order read/write completion
submission queue saturation
completion queue pressure
multishot progress + terminal
```

当前 Windows 代码中，`CancelIoEx(ERROR_NOT_FOUND)` 后仍然建立 completion obligation 的处理非常关键；这类契约应提升为通用 Completion Engine contract，而不是继续被认为是 IOCP 特例。

---

# 最终判断

`game-net-core` 最合适的长期架构不是：

```text
一个 Reactor/Poller 抽象适配所有系统
```

也不是：

```text
epoll 一套运行时
IOCP 一套运行时
io_uring 再来一套运行时
```

而是：

```text
一个 owner-loop 并发与生命周期内核
+
一个可表达多种 I/O capability 的 Engine 层
+
Readiness 与 Completion 各自保留真实语义
```

最应坚持的四条边界是：

1. **`EventLoop` 继续统一线程、调度、admission 和 final drain。**
2. **`Channel` 只代表 Readiness registration，不再承载 Completion operation。**
3. **Completion 必须保留 operation identity、result、storage lease 和 terminal retirement。**
4. **上层 `TcpConnection/TransportEndpoint/Session` 共享网络语义，但底层 read/write/cancel 机制不强求同构。**

因此答案是明确的：

> **应当演进到 `owner-loop/EventLoop + I/O Engine`。这不是推翻现有 Reactor/TCP 方向，而是把当前已经成熟的 owner-loop 语义从 Readiness Reactor 中提炼出来，使它真正成为同时支撑 epoll、kqueue、IOCP 和 io_uring 的通用并发内核。**
