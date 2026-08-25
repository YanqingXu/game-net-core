能，而且还有相当大的空间。但需要先明确两个判断：

> **协程不是天然更快，无锁也不是天然更快。**

它们真正的价值是：

* 协程：把异步状态机变成编译器生成的状态机，并有机会消除 `std::function`、回调对象、层层 `shared_ptr` 和重复调度；
* 无锁队列：在明确的线程拓扑下，减少互斥锁竞争、动态分配和线程唤醒；
* 两者结合：形成“owner-local 直接执行 + 跨线程 typed mailbox + owner-local coroutine ready queue”的数据面。

对于 `game-net-core`，我认为最理想的执行模型是：

```text
                      EventLoop owner
                            │
                ┌───────────┴───────────┐
                │                       │
        owner-local ready queue   I/O readiness/completion
          无锁都不需要                   │
                │                       │
                └────── coroutine resume┘
                            │
                    decode / handler
                            │
               bounded typed mailbox
                       SPSC / MPSC
                            │
                       LogicShard
                            │
                    bounded outbox
                            │
                       网络 owner
                            │
                     batch send
```

而不是：

```text
每个消息
  -> 创建协程
  -> 创建 std::function
  -> shared_ptr 引用计数
  -> 投递全局无锁队列
  -> CAS 竞争
  -> wakeup
```

后者很可能比现在更慢。

---

# 一、协程适合放在哪里

仓库已经保留了 `Task<T>`、`ConnectionAwaiterRegistry`、`asyncSleep`、`WhenAll` 等 deferred intent。原始方向是正确的：`Task` 只是桥梁，不应成为另一套调度器；连接 Awaiter 的恢复也必须回到所属 EventLoop。

## 1. 最适合：一条连接或一个 Session 一个长生命周期协程

建议是：

```cpp
Task<void> runSession(Session& session)
{
    while (true)
    {
        auto packet = co_await session.nextPacket();

        if (!packet)
        {
            co_return;
        }

        auto result = handlePacket(*packet);

        auto sendResult = session.trySend(std::move(result));
        if (sendResult == SendResult::Backpressured)
        {
            co_await session.waitWritable();
        }
    }
}
```

但不要采用：

```text
收到一个包
-> 创建一个 Task
-> detach
-> 包处理结束后销毁协程
```

因为“一包一协程”可能引入：

* 一次 coroutine frame 分配；
* promise 构造与销毁；
* continuation 管理；
* 异常状态；
* suspend/resume；
* ready queue 投递。

正确粒度通常是：

| 场景            | 推荐协程粒度                          |
| ------------- | ------------------------------- |
| TCP 连接读取循环    | 一个连接一个 reader/session coroutine |
| 登录鉴权流程        | 一个登录流程一个 coroutine              |
| RPC 请求        | 一个有实际异步等待的请求一个 coroutine        |
| DB/Redis 异步调用 | 一个业务流程 coroutine                |
| 每个游戏包的简单分发    | 不创建新 coroutine                  |
| 广播内层遍历        | 不使用 coroutine                   |
| 固定 Tick 的实体遍历 | 不使用 coroutine                   |

协程最适合解决“流程有多个真正异步等待点”的情况，而不是替代普通函数。

---

## 2. 只在真正等待时 suspend

协程接口必须提供强 fast path：

```cpp
bool await_ready() noexcept;
```

例如：

* 输入缓冲区中已经有完整包：立即返回，不挂起；
* 输出队列仍有空间：直接发送，不挂起；
* 操作已完成：直接读取结果；
* deadline 已经过期：直接返回 timeout；
* 已经位于目标 owner：不进行线程切换。

也就是说，正常情况应当是：

```text
co_await nextPacket()
    -> Buffer 已有完整包
    -> await_ready() == true
    -> 不创建调度任务
    -> 不触发 wakeup
    -> 不进入通用 EventLoop 队列
```

协程只有在以下情况才真正 suspend：

* 当前没有完整数据；
* 输出背压达到阈值；
* 等待 Timer；
* 等待逻辑线程、DB 或 RPC；
* 等待连接关闭。

---

# 二、需要为协程增加一条专用 Ready Queue

当前 EventLoop 的通用任务路径是：

```text
std::function<void()>
+ std::deque<PendingFunctor>
+ mutex
+ enqueue timestamp
+ wakeup
```

它适合普通控制任务，但不适合高频 coroutine resume。

建议在 EventLoop 内部增加独立的：

```cpp
CoroutineReadyQueue
```

分成两条路径。

## 1. Owner-local resume

当完成事件已经发生在所属 EventLoop 上时：

```text
completion/readiness callback
    -> readyQueue.push(coroutine_handle)
    -> 当前阶段结束
    -> 批量 resume
```

这里完全不需要原子操作和锁，因为生产者和消费者都是 owner 线程。

可以使用：

* 固定容量环形数组；
* `small_vector<coroutine_handle<>>`；
* promise 内嵌 intrusive node；
* owner-local chunked queue。

不要立即递归调用：

```cpp
handle.resume();
```

否则容易产生：

* callback 深层递归；
* 重入 TcpConnection；
* 一条连接持续读取导致其他连接饥饿；
* coroutine A 恢复 B，B 恢复 C 的不可控调用栈。

更好的策略是：

```text
当前 I/O callback 只标记 runnable
EventLoop 在 CoroutineResumePhase 批量 resume
```

并增加：

```cpp
maxCoroutineResumesPerIteration
maxCoroutineResumeNanosecondsPerIteration
```

这与当前 EventLoop 已经存在的 I/O、Timer、control、lifecycle、functor 分阶段预算保持一致。

## 2. Cross-thread resume

跨线程 completion 进入：

```text
BoundedMpscResumeMailbox
```

消息内容只需：

```cpp
struct ResumeCommand
{
    std::coroutine_handle<> handle;
    std::uint64_t generation;
};
```

不再创建 `std::function`。

然后使用一个合并 doorbell：

```text
mailbox 从 empty 变成 non-empty
    -> 唤醒 EventLoop

后续继续 push
    -> 不重复 wakeup
```

你现在的 `EventLoopControlSource` 已经采用了相似思想：固定 slot、pending bit 合并、无 queue node，并且只有第一次进入 pending 状态才唤醒。这套机制完全可以作为 coroutine mailbox 的 doorbell。

---

# 三、协程 frame 本身也必须优化

C++ 协程通常需要 coroutine frame。若直接使用普通 `operator new`，十万连接可能产生大量小对象分配和内存碎片。

建议让 `promise_type` 使用 per-EventLoop slab：

```cpp
struct promise_type
{
    static void* operator new(std::size_t size);
    static void operator delete(void* ptr, std::size_t size) noexcept;
};
```

## 推荐的 frame pool

```text
EventLoopCoroutineFramePool
    ├── 128-byte class
    ├── 256-byte class
    ├── 512-byte class
    ├── 1024-byte class
    └── large fallback
```

要求：

* 分配和释放都发生在 owner；
* free list 不需要原子；
* frame 按 cache line 对齐；
* 记录 frame size 分布；
* 大 frame 必须可观测；
* 不要把大 payload 直接 capture 到 coroutine frame。

例如不要：

```cpp
co_await send([payload = 1MB_string] {});
```

而应当：

```cpp
OwnedPacket packet = packetPool.acquire(...);
co_await send(std::move(packet));
```

理想指标是：

```text
每连接最多一次 coroutine frame 分配
每包零 coroutine frame 分配
每次 co_await 零额外 heap allocation
```

仓库原有 `Task` intent 提到 lazy start、move-only frame ownership 和 FinalAwaiter symmetric transfer，这些都应保留；Task-to-Task 同线程组合可以利用 symmetric transfer，避免额外调度。

---

# 四、协程恢复语义需要修正的三个地方

现有 deferred intent 不能原样恢复，有几个地方与当前项目成熟后的生命周期契约不完全一致。

## 1. `asyncSleep` 不能在 EventLoop 退出时永久不恢复

旧 intent 中写到：如果 EventLoop 在 Timer 触发前退出，coroutine handle 可能不会恢复。

这与现在的原则不一致：

```text
Accepted operation
    -> 正常完成
    或
    -> 明确 Cancelled/Shutdown terminal result
```

正确语义应是：

```text
EventLoop 进入 Quiescing
    -> Timer Awaiter 收到 shutdown
    -> coroutine 在 owner 上恢复一次
    -> await_resume 返回 Cancelled/OwnerShutdown
    -> frame 正常收敛
```

不能让 coroutine frame、连接引用或业务对象永久悬挂。

## 2. `WhenAll` 必须返回原 owner

旧 `WhenAll` intent 允许父 coroutine 在“最后完成的子任务所在的线程”恢复。

这对通用 coroutine combinator 可以接受，但对网络连接 coroutine 不安全。

应当设计成：

```cpp
auto result = co_await whenAllOn(
    originExecutor,
    task1,
    task2,
    task3);
```

或者让 `OwnerTask` promise 记录：

```cpp
EventLoopExecutor continuationExecutor;
```

最后一个子任务完成后，只是向 origin owner 提交 resume，而不是直接在任意线程恢复父 coroutine。

否则可能出现：

```text
连接属于 NetworkLoop-1
DB task 在 DBThread-3 最后完成
父 coroutine 在 DBThread-3 恢复
直接访问 TcpConnection
-> 线程归属被破坏
```

## 3. Connection Awaiter 不应进入通用 Functor Queue

原始 intent 要求连接 Awaiter 通过 EventLoop queue 恢复，这是正确的语义边界；但实现上应升级成 source-private coroutine ready queue，而不是通用 `queueInLoop(std::function)`。

也就是：

```text
语义仍然是“回 owner EventLoop 恢复”
实现不再是“构造 std::function 并加锁入 deque”
```

---

# 五、无锁队列应该怎么用

最重要的原则是：

> **先根据生产者和消费者数量选队列，不要先决定“我要一个 MPMC 无锁队列”。**

## 推荐拓扑

| 数据方向                           | 推荐结构                               |
| ------------------------------ | ---------------------------------- |
| EventLoop owner 内部             | 普通 ring/vector，不需要锁和原子             |
| 一个 NetworkLoop → 一个 LogicShard | SPSC bounded ring                  |
| 一个 LogicShard → 一个 NetworkLoop | SPSC bounded outbox                |
| 多个 NetworkLoop → 一个 LogicShard | 每 producer 一条 SPSC，或 bounded MPSC  |
| 多个外部线程 → EventLoop             | bounded MPSC                       |
| 多个任意线程 → 多个任意线程                | 尽量避免 MPMC                          |
| 广播                             | 按 owner 分组后的 batch task            |
| 控制通知                           | 当前 bitset/coalesced control source |

## 最快的通常是 SPSC，而不是 MPMC

一条 SPSC ring 可以做到：

```text
producer:
    读取自己的 tail
    acquire 读取 head
    原地构造对象
    release 发布 tail

consumer:
    读取自己的 head
    acquire 读取 tail
    move 出对象
    release 发布 head
```

特点：

* 没有 CAS retry；
* 没有 mutex；
* 没有动态分配；
* 只有两个共享索引；
* producer 和 consumer index 可以放在不同 cache line；
* 可以批量 push/drain。

因此，对于固定线程拓扑：

```text
NetworkLoop[0] -> LogicShard[0]
NetworkLoop[0] -> LogicShard[1]
NetworkLoop[1] -> LogicShard[0]
NetworkLoop[1] -> LogicShard[1]
```

最激进的设计是每对线程一条 SPSC。

代价是队列数量为：

```text
NetworkLoopCount × LogicShardCount
```

如果规模是 4×4 或 8×8，一般可以接受；如果是 64×64，则需要 sparse mailbox 或 per-shard MPSC。

---

# 六、不要直接把所有队列替换成 MPMC

当前 `GameCommandQueue` 是 `mutex + deque<GameCommand>`，语义简单、容量明确，但每条命令携带 `std::string`，并且多个线程会竞争同一个 mutex。

可以增加高性能实现：

```cpp
SpscGameCommandMailbox
MpscGameCommandMailbox
```

但不建议直接做一个“万能 MPMC 队列”替换所有场景，因为 MPMC 常见成本包括：

* 多个 producer 争抢同一 tail cache line；
* CAS 失败和 retry；
* 每个 slot 的 sequence 原子；
* consumer 之间的 head 竞争；
* NUMA 跨节点 cache line 迁移；
* 复杂的 shutdown 和内存回收。

在低竞争时，一个短临界区 mutex 甚至可能比 MPMC 更快。

因此应当采用：

```text
静态明确拓扑
    -> SPSC

多个生产者、一个 owner
    -> MPSC

真正任意多生产者多消费者
    -> 重新审视架构，MPMC 作为最后选择
```

---

# 七、无锁队列必须有这六项约束

## 1. 固定容量

只能提供：

```cpp
SubmitResult tryPush(T&& value);
```

队列满时返回：

```text
QueueFull
Stopped
PayloadTooLarge
OwnerUnavailable
```

绝不能：

* 自动扩容；
* 在生产者线程无限 spin；
* 静默丢弃；
* fallback 到无界链表。

## 2. 原地构造

ring slot 应直接存储：

```cpp
OwnedPacket
GameCommand
OutputCommand
ResumeCommand
```

不要存：

```cpp
std::function<void()>
shared_ptr<Command>
unique_ptr<HeapNode>
```

否则队列无锁了，allocator 仍然是瓶颈。

## 3. 批量 admission 和 drain

接口应支持：

```cpp
tryPushBatch(...)
consumeBatch(maxCount, callback)
```

这样可以把：

* atomic acquire/release；
* wakeup；
* queue age 采样；
* owner 调度；

摊销到一批消息上。

## 4. empty → non-empty 才通知

典型 doorbell 语义：

```text
producer enqueue
    -> 如果此前没有 pending 通知
       才 eventfd/PostQueuedCompletionStatus/notify

consumer drain
    -> 清除 pending
    -> 再检查一次队列
       防止 clear 与 producer push 之间丢失唤醒
```

这比“每 push 一次 wakeup”重要得多。

## 5. 避免动态内存回收问题

优先使用固定 ring slot。

不要优先采用 lock-free linked list，因为它会引出：

* ABA；
* hazard pointer；
* epoch reclamation；
* 节点延迟回收；
* shutdown 时悬挂节点；
* allocator 竞争。

固定 ring 通过 slot sequence/generation 可以避开大部分回收难题。

## 6. Accepted 必须形成终局

必须保持项目已有的契约：

```text
Accepted
    -> 被 consumer 处理
    或
    -> shutdown 时形成显式 discarded/cancelled result
```

不能因为是无锁队列，就在停止时直接重置 head/tail 丢掉已接受任务。

---

# 八、协程与无锁队列结合的最佳方式

建议提供显式的跨域 Awaiter：

```cpp
Task<LogicResult> LogicShard::submit(OwnedPacket packet);
```

内部流程：

```text
1. 当前 NetworkLoop 将 OwnedPacket move 到 SPSC/MPSC slot
2. mailbox admission 成功
3. coroutine suspend
4. LogicShard 批量 drain
5. 处理完成
6. result move 到 NetworkLoop outbox
7. empty -> non-empty 时唤醒网络 owner
8. 网络 owner 恢复 coroutine
9. coroutine 继续 send
```

这里每条请求应尽量满足：

```text
payload copy：0 或 1 次
heap allocation：0
shared_ptr 增减：0
跨线程 ownership transfer：2 次
网络→逻辑 wakeup：每 burst 最多 1 次
逻辑→网络 wakeup：每 burst 最多 1 次
连接状态访问：只在网络 owner
```

示意代码：

```cpp
Task<void> processSession(Session& session, LogicShard& logic)
{
    while (auto packet = co_await session.nextOwnedPacket())
    {
        LogicResult result =
            co_await logic.submit(std::move(*packet));

        // submit 的 continuation 必须返回 session owner。
        const auto sendResult =
            session.trySend(std::move(result.payload));

        if (sendResult == SendResult::Backpressured)
        {
            co_await session.waitOutputBelowLowWatermark();
        }
    }
}
```

---

# 九、零拷贝与协程之间有一个生命周期陷阱

前面建议过 `PacketView` 零拷贝解帧，但它不能随意跨 `co_await`。

例如：

```cpp
PacketView packet = co_await session.nextPacketView();

co_await db.query(...);

// packet 此时可能已经指向失效的 Buffer 区域
use(packet);
```

这是危险的。

因此应该明确区分两种 API。

## 同步 owner-local fast path

```cpp
session.visitFrames([](PacketView packet) {
    handleImmediately(packet);
});
```

要求 callback 返回前消费完毕，不允许跨 suspend。

## 可挂起 coroutine path

```cpp
OwnedPacket packet = co_await session.nextOwnedPacket();
```

使用：

* per-loop packet pool；
* fixed-size block；
* move-only ownership；
* 必要时 segmented shared chunk。

也可以设计 `FrameLease` 固定底层 receive chunk，但这会增加：

* chunk 生命周期；
  -引用计数或 lease 计数；
* buffer 无法及时复用；
  -慢 coroutine 导致 receive memory retention。

所以对于高频小包：

> owner-local 处理用 `PacketView`，会跨 suspend 的流程用 pooled `OwnedPacket`。

---

# 十、Completion I/O 与协程天然适配，但要处理内核引用

IOCP 和未来 io_uring 很适合：

```cpp
co_await asyncRead();
co_await asyncWrite();
```

因为一个 operation 本身就有：

* operation identity；
* completion result；
* bytes；
* native error；
* cancellation；
* terminal retirement。

但有一条非常严格的规则：

> 内核还可能返回 completion 时，coroutine frame 不能被提前销毁。

如果 `OVERLAPPED` 或 io_uring operation storage 直接嵌在 coroutine frame 中，那么取消 coroutine 后也不能立刻销毁 frame，必须等待 terminal completion。

可以选择两种方案：

### 方案 A：operation 独立于 coroutine frame

```text
CoroutineFrame
    -> OperationState
        -> kernel
```

取消后 coroutine observer 可以失效，但 `OperationState` 保留到内核 terminal completion。

更安全，适合当前项目已有的 completion retirement 契约。

### 方案 B：operation 嵌在 frame 中

内存更紧凑，但 frame 必须进入：

```text
CancelRequested
-> CompletionDraining
-> TerminalCompletion
-> FrameDestroy
```

实现难度更高。

对于 `game-net-core`，我更建议先采用方案 A。

---

# 十一、Timer 也是一个明显的进一步优化点

当前 TimerQueue 使用：

```text
std::function
+ shared_ptr<Timer>
+ std::map<expiration, Timer>
+ unordered_map<id, Timer>
```

每次创建 Timer 都可能有对象分配、控制块分配和红黑树节点分配。

少量 Timer 没问题，但如果以后出现：

* 每连接 idle timeout；
* 心跳超时；
* RPC deadline；
* coroutine sleep；
* 重连 timer；
* actor timer；

十万连接可能意味着几十万个 Timer。

可以增加：

```text
Hierarchical Timing Wheel
```

例如：

```text
L0：1 ms × 1024 slots
L1：1 s × 1024 slots
L2：约 17 min × 1024 slots
overflow：min-heap
```

配合：

* Timer node 内嵌 intrusive link；
* per-loop timer slab；
* generation TimerId；
* owner-local insert/cancel 无锁；
* 一次 tick 批量 expire；
  -高精度少量 timer 留在 heap。

不建议立即删掉现有 TimerQueue，而是：

```text
普通控制 Timer
    -> 当前精确定时队列

海量连接 timeout/deadline
    -> HighVolumeTimerWheel
```

协程 `asyncSleep` 和 RPC deadline 可以逐步迁移到后者。

---

# 十二、还可以继续压的其他方面

## 1. Owner-local dense handle table

将热路径中的：

```text
unordered_map<SessionId, shared_ptr<Session>>
```

替换为：

```text
slot index + generation
```

例如：

```cpp
struct SessionHandle
{
    uint32_t slot;
    uint32_t generation;
};
```

优点：

* O(1) 数组访问；
* cache locality 更好；
* stale handle 检测；
* 避免热路径 hash；
* 避免每次 `shared_ptr` 原子引用计数。

## 2. RCU/Immutable Snapshot

适合：

* 路由表；
  -配置表；
  -只读 session directory；
* handler registry；
  -协议 message-id dispatch table。

更新线程创建新 snapshot，读路径只读指针，不加锁。

但不要在每个包上做 `atomic<shared_ptr>::load()`；可以由 EventLoop 在轮次边界刷新 snapshot 指针。

## 3. Stateless Compute Pool

网络 connection owner 不适合 work stealing，但以下任务可以：

* 压缩；
  -加密前后处理；
  -纯函数序列化；
  -路径计算；
  -不访问玩家可变状态的 CPU 工作。

可使用 work-stealing compute pool。

完成后必须：

```text
result -> 原 connection owner outbox
```

连接 coroutine 本身不迁移 owner。

## 4. 固定 Tick 不要使用大量 coroutine

固定 Tick 的核心模拟：

```text
for each entity
    update
```

通常直接批量遍历最快。

协程更适合 Tick 外部流程：

* 等待数据库；
* 匹配；
  -跨服 RPC；
  -登录；
  -延时任务。

不要把每个实体做成一个独立 coroutine，除非有明确 benchmark 证明。

## 5. 可选的 spin-then-poll

在专用 CPU 核、极低延迟 Profile 下：

```text
短时间 busy spin
-> 检查 mailbox / completion
-> 无工作后再 epoll/IOCP wait
```

可以降低唤醒延迟，但会持续占用 CPU。

必须是显式部署 Profile：

```text
Throughput
Balanced
DedicatedLowLatency
```

不能作为默认行为。

## 6. 批量恢复 coroutine

不要：

```text
取一个 handle
resume
取一个 handle
resume
```

而是：

```text
一次取 64/128 个 handles
记录统一 observedAt
按预算 resume
剩余留到下一轮
```

对 remote mailbox 也使用 batch pop。

---

# 十三、不同 Runtime Profile 应如何使用协程和无锁队列

| Profile                  | 协程使用                            | 队列建议                              |
| ------------------------ | ------------------------------- | --------------------------------- |
| `SingleLoopInlineEvent`  | 仅长流程；简单包处理不使用 coroutine         | owner-local ring，无原子              |
| `MultiIoQueuedEvent`     | 非常适合 network→logic await        | 每 NetworkLoop→Logic 一条 SPSC       |
| `DedicatedFixedTick`     | Tick 主体不用；外围异步流程使用              | Network→Tick SPSC，Tick 周期批量 drain |
| `ShardedHybrid`          | coroutine 必须绑定 LogicCell        | 每 NetworkLoop→Cell mailbox，禁止任意迁移 |
| IOCP/io_uring completion | operation awaiter 非常自然          | completion queue 由 backend 管理     |
| epoll readiness          | await 数据/空间，不是 await 单次 syscall | owner 读取至 EAGAIN 后批量分帧            |

当前项目的运行模型指南本身也强调，应按连接活跃度、包频率、逻辑成本、handoff、广播和背压选择 Profile，而不是只根据游戏类型选择。协程和无锁队列也应遵守同一原则。

---

# 十四、建议的落地顺序

## C1：协程基础层

先建立独立的 provisional target：

```text
GameNet::coroutine
    depends on GameNet::core
```

不要让 stable core 反向依赖它。

实现：

```text
Task<T>
OwnerTask<T>
CoroutineReadyQueue
LoopResumeAwaiter
CancellationToken
asyncSleep
```

修正：

* shutdown 必须 terminal resume；
* continuation 必须回 origin owner；
* detached Task 必须绑定生命周期域；
* owner-local resume 不走通用 Functor queue。

## Q1：SPSC typed mailbox

实现：

```text
SpscMailbox<OwnedPacket>
SpscMailbox<LogicResult>
MailboxDoorbell
BatchDrain
QueueSnapshot
```

先接入 `MultiIoQueuedEvent` benchmark，与当前 `GameCommandQueue` 做严格对比。

## C2：连接 Awaiter

增加：

```text
asyncNextOwnedPacket()
waitWritable()
waitClosed()
asyncDrain()
```

要求：

```text
一个连接最多一个 active read waiter
一个连接最多一个 active drain waiter
关闭时所有 waiter 恢复一次
```

## Q2：MPSC 和多生产者拓扑

只有 SPSC matrix 队列数量、内存或拓扑不合适时，再实现：

```text
BoundedMpscRing<T>
```

采用 per-slot sequence，不使用 lock-free linked list。

## P1：高容量 Timer

在协程 sleep、RPC deadline、连接 timeout 的数量确实上升后，再增加：

```text
PerLoopTimerWheel
```

---

# 十五、应建立的硬性能契约

协程和无锁队列进入正式 Runtime Profile 前，建议满足：

```text
Owner-local 包处理：
    heap allocations/message = 0
    generic EventLoop posts/message = 0
    cross-thread wakeups/message = 0

跨线程逻辑处理：
    payload ownership transfers = 2
    payload copies <= 1
    wakeups/burst/direction <= 1
    queue capacity = bounded
    silent drops = 0

Coroutine：
    coroutine frame allocations <= 1/connection
    per-await allocations = 0
    arbitrary-thread resume = 0
    unresumed terminal awaiters = 0

Queue：
    CAS retry 可观测
    queue age P99/P999 可观测
    full rejection 可观测
    shutdown residue = 0
    lost wakeup = 0
```

Benchmark 必须对照：

```text
callback + mutex queue
coroutine + mutex queue
callback + SPSC
coroutine + SPSC
```

否则很难知道提升来自协程、队列，还是测试场景变化。

---

# 最终判断

可以使用协程和无锁队列，而且它们很可能成为 `game-net-core` 下一阶段的重要性能来源。但最佳组合不是“协程化全部 API、无锁化全部容器”，而是：

```text
回调/直接调用
    用于 owner-local 极热路径

长生命周期 coroutine
    用于连接、Session、RPC、DB 等异步流程

owner-local ready queue
    用于同线程 coroutine resume

SPSC typed mailbox
    用于固定 NetworkLoop ↔ LogicShard 拓扑

bounded MPSC
    仅用于真正的多生产者入口

通用 std::function queue
    保留给低频控制和兼容 API
```

其中最有希望带来明显收益的组合是：

> **一个 Session 一个长生命周期 coroutine + per-loop coroutine frame pool + owner-local ready queue + NetworkLoop/LogicShard SPSC mailbox + empty→non-empty 合并唤醒。**

这套结构既能保持你现有的 owner-loop、背压和生命周期公理，又能把每包 `std::function`、锁竞争、动态分配、重复 wakeup 和多余 payload copy 从热路径中逐步移除。
