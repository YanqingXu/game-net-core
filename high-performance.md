# 核心判断

对于 `game-net-core`，我认为“高性能”不应定义成某个 Echo Benchmark 的峰值最高，而应定义成：

> **在统一、可靠、可验证的生命周期与背压语义下，用尽可能少的 CPU 周期、内存搬运、线程切换和系统调用，完成一条有效游戏消息的接收、解析、调度和发送，并且在接近饱和时仍保持可预测的 P99/P999 延迟。**

这里有一个非常重要的架构原则：

> **通用性放在语义和契约层，极致性能放在具体 backend、Runtime Profile 和数据路径层。**

也就是说：

* 统一 owner-loop、生命周期、背压、关闭、错误与观测语义；
* 但不要强迫 epoll、IOCP、io_uring 使用完全相同的数据面；
* 不要让所有游戏负载都走同一套线程拓扑；
* 公共 API 可以稳定，内部 fast path 必须允许平台化、批量化、特化。

你的长期目标文档已经基本采用了这个方向：EventLoop 是 owner scheduler，而不是狭义 epoll Reactor；Readiness 与 Completion 保留各自真实语义；运行模型由负载特征选择，而不是由“MMO、FPS、回合制”标签决定。这个判断是正确的。

---

# 一、高性能到底应该衡量什么

一个网络库真正的性能，应至少包含六个维度。

| 维度   | 应关注的指标                                       |
| ---- | -------------------------------------------- |
| 单核效率 | messages/s/core、Gbps/core、cycles/message     |
| 延迟   | P50、P99、P999、最大抖动                            |
| 内存效率 | bytes/connection、allocations/message、峰值积压    |
| 调度效率 | handoff/message、wakeup/message、queue lag     |
| 内核交互 | syscalls/message、batch size、context switches |
| 饱和行为 | 拒绝是否明确、内存是否有界、恢复时间、关闭时间                      |

最值得采用的主指标不是“最大吞吐”，而是：

> **在满足指定 P99/P999 延迟 SLO 的前提下，能够持续承受的最大负载。**

因为系统利用率逼近 100% 时，队列延迟会迅速放大。一个能跑到 200 万消息/秒、但在 85% 负载下 P999 已经失控的网络库，不能称为真正的高性能底座。

可以把每条消息的成本拆成：

```text
cycles/message
    = I/O 等待与分发
    + 数据复制
    + 内存分配
    + 协议解析
    + 原子操作与锁竞争
    + 跨线程 handoff
    + wakeup
    + 发送与背压记账
```

极致性能的第一原则不是“换一个更快的系统调用”，而是：

> **消除工作 > 合并工作 > 保持局部性 > 特化数据路径 > 指令级微优化。**

---

# 二、当前 `game-net-core` 已经具备的优势

当前仓库已经不是简单的 Reactor 雏形，而是具备了比较成熟的高性能基础：

1. 单 owner EventLoop，连接可变状态由所属线程独占；
2. Readiness 与 Completion 的语义不再被错误地抹平；
3. EventLoop 对 I/O、Timer、control、lifecycle、functor 分阶段有界调度；
4. 队列、发送缓存、连接、loop、server、global 都有容量和背压约束；
5. IOCP completion 能确定性 drain，关闭过程有明确状态机；
6. 已有核心、容量、Phase 4、广播、运行时拓扑、io_uring 对比和耐久测试；
7. Linux/epoll 仍是生产路径，io_uring 维持 source-private 实验，没有为了“新技术”污染公共接口。

尤其值得保留的是：

> **正确性、背压、终局收敛本身就是高性能的一部分，不能为了跑分删除。**

无界队列在短时测试中可能吞吐很高，但一旦进入持续过载，最终会变成内存膨胀、延迟失控和进程崩溃。你现在这种 `Accepted` 必须最终执行或形成可观察终止结果的契约，比很多只追求 QPS 的网络库更适合游戏服务器。

---

# 三、当前真正限制性能上限的地方

我认为当前的主要性能上限，暂时不在 `epoll_wait` 或 `GetQueuedCompletionStatusEx`，而在用户态数据路径。

## 1. 通用 EventLoop functor 路径过重

当前 EventLoop 的通用跨线程任务路径包含：

* `std::function<void()>`；
* `std::deque<PendingFunctor>`；
* executor 状态锁；
* pending queue 锁；
* 每任务 `now()` 时间戳；
* 可能的动态分配；
* wakeup。

控制通知和 lifecycle signal 已经做到了无 queue node、通知合并和仅首次 pending 时唤醒，这条路径很好；但普通 `executor.post()` 仍然是通用控制路径，不适合承载每一个高频游戏包。

**结论：**

> `std::function + mutex + deque` 应继续保留为安全、通用、低频控制路径，但不能成为百万级消息数据面的默认队列。

---

## 2. 当前协议解帧存在“每帧分配、每帧复制”

当前 `PacketFramer::push()` 会：

1. 将输入复制到自己的环形 storage；
2. 每解析出一个完整帧，创建一个新的 `std::string`；
3. 将其放入 `std::vector<std::string>`；
4. 返回给上层。

编码时也会创建新字符串，再写入长度头和 payload。

在 32～256 字节高频游戏包下，allocator、字符串对象和 memcpy 很可能比 epoll 本身更贵。

理想路径应接近：

```text
socket read
    -> Connection InputBuffer
    -> 原地检查长度头
    -> FrameView 指向 InputBuffer
    -> handler 立即消费
    -> retrieve
```

也就是：

* owner-local 处理：零堆分配、零额外 payload 复制；
* 需要跨线程：只创建一个可转移所有权的 `OwnedPacket`；
* 广播：创建一个不可变 `SharedPayload`。

现有 `push()` API可以保留，但应新增高性能接口，例如：

```cpp
FrameStatus visitFrames(
    Buffer& input,
    FrameVisitor visitor,
    FrameBudget budget);
```

其中 `FrameView` 只在 callback 期间有效。需要跨线程时，由上层显式调用 `retain()` 或转成 pooled owned block。

---

## 3. 网络到逻辑的跨域路径固定税较高

当前 `GameCommandQueue` 是：

```text
mutex
+ deque<GameCommand>
+ 每命令 std::string payload
+ 每命令时间戳
```

这套实现语义清楚、有界、容易验证，但在 Profile B/C/D 中，一条消息可能经历：

```text
socket Buffer
-> PacketFramer 内部 storage
-> frame std::string
-> GameCommand std::string
-> mutex queue
-> logic drain
-> 回包再跨线程
```

也就是“多次复制 + 两次 handoff + 两次 wakeup 风险”。

更高效的结构不是一个全局 MPMC 队列，而是：

```text
每个 NetworkLoop -> 每个 LogicShard
    一条有界 SPSC mailbox
```

对于固定的网络线程和逻辑线程拓扑，多个独立 SPSC 队列通常比一个中央 MPSC/MPMC 队列更好：

* 无多生产者争抢同一 tail cache line；
* producer/consumer index 可分 cache line；
* 队列容量独立；
* 热点和倾斜可直接观测；
* 可以一次批量 drain；
* 只在 empty → non-empty 时 wakeup。

只有在拓扑动态、生产者不可预知时，才使用 bounded MPSC。

---

## 4. 跨线程发送路径成本较高

当前跨线程 `TcpConnection::trySend()` 会：

1. 预留输出字节；
2. `shared_from_this()`；
3. 将 payload 复制到新的 `std::string`；
4. 构造闭包；
5. 投递到通用 EventLoop executor；
6. owner 线程再次检查连接状态；
7. 执行发送；
8. 经过 connection、loop、server、global 多级原子内存预算。

同 owner Linux 路径先直接 `write`，未写完才进入 Buffer，这个快路径是正确的；问题主要在跨线程发送。

应将发送分成三种明确路径：

```cpp
// owner-thread，当前调用期间借用
trySendBorrowed(std::span<const std::byte>);

// 转移所有权，无 payload copy
trySendOwned(OwnedBuffer&&);

// 广播/缓存结果，共享不可变存储
trySendShared(SharedPayload);
```

现有通用 `trySend(string_view)` 可以保留，但高频运行模型应明确保证：

> 正常业务回包由连接 owner 线程执行，不把跨线程 `trySend()` 当作常规路径。

---

## 5. epoll 就绪分发仍有明显用户态成本

当前 Linux source-private production adapter 在处理一个 readiness notice 时，至少存在：

1. registration map 查找；
2. notice 批内线性扫描以合并相同 identity；
3. `isCurrent()` 再做一次 registration 查找；
4. 然后才写入 active channel。

批次活跃连接很多时，线性去重可能形成 O(n²) 行为；哈希表还会带来额外 cache miss。

建议改成：

```text
RegistrationSlotArena
    slot index
    generation
    fd
    Channel*
    interests
    lastBatchEpoch
```

向 `epoll_event.data.u64` 写入：

```text
generation + slot index
```

wait 返回时：

```text
token
-> 直接数组索引 slot
-> generation 比较
-> lastBatchEpoch O(1) 合并
-> active list
```

这样：

* wait 热路径不再访问 `unordered_map`；
* stale event 检测仍然保留；
* descriptor reuse 仍然安全；
* 批内合并由线性扫描变为 O(1)；
* fd 到 slot 的 map 只存在于注册、更新、删除控制面。

这是我认为当前 epoll 路径最值得优先做的底层优化。

---

# 四、理想的数据面应该是什么样

## Owner-local 低延迟路径

```text
epoll / IOCP / io_uring
        ↓ batch
EventLoop owner
        ↓
read/readv 或 completion buffer
        ↓
原地解帧 FrameView
        ↓
轻量 owner-local handler
        ↓
OutputSegmentChain
        ↓
direct write / writev / WSASend
```

目标性能契约：

```text
0 次跨线程 handoff
0 次跨线程 wakeup
0 次每帧堆分配
0 次每帧 shared_ptr 增减
O(1) readiness dispatch
0～1 次额外 payload copy
1 次直接发送或一次批量 gather-send
```

## 跨逻辑域路径

```text
NetworkLoop
   ↓ move OwnedPacket
bounded typed mailbox
   ↓ ≤1 wakeup per burst
LogicShard batch drain
   ↓ move response
OwnerOutbox
   ↓ batch drain
Connection owner send
```

目标性能契约：

```text
1 次所有权转移，而不是 payload copy
1 次 bounded mailbox admission
每一批最多 1 次 wakeup
批量 drain
回包只在 connection owner 上操作
```

这比“所有地方都做无锁”更重要。

---

# 五、将性能发挥到极致的具体方向

## 第一优先级：零分配解帧和所有权化 Packet

建议建立内部统一的数据对象：

```cpp
class PacketView;       // 借用 Buffer，callback 内有效
class OwnedPacket;      // 独占 pooled storage
class SharedPayload;    // 不可变广播/缓存数据
class OutputSegments;   // header + payload scatter/gather
```

不同场景选择不同所有权：

| 场景               | 数据类型             |
| ---------------- | ---------------- |
| owner-local 同步处理 | `PacketView`     |
| network → logic  | `OwnedPacket`    |
| 单连接异步发送          | `OwnedBuffer`    |
| 广播、重复响应          | `SharedPayload`  |
| 发送长度头 + payload  | `OutputSegments` |

对于常见小包，可以让 `OwnedPacket` 带可配置的 inline storage，超过阈值才进入 per-loop block pool。

不要一开始就追求 Linux `MSG_ZEROCOPY`。对于几十到几百字节的小包，描述符管理、completion 和页生命周期成本通常可能超过一次 memcpy。游戏小包的重点是：

> **少分配、少对象、少 handoff、批量系统调用，而不是字面意义上的零复制。**

---

## 第二优先级：建立 typed mailbox 数据面

当前 control source 的设计已经证明了正确方向：预注册、固定 slot、通知合并、无 queue node。可以沿这个思想增加 source-private typed mailbox：

```cpp
template<class T>
class BoundedMailbox {
public:
    SubmitResult tryPush(T&& value);
    std::span<T> drainBatch(std::size_t maxCount);
};
```

关键要求：

* 数据容量固定、有界；
* wakeup 只发生在 empty → non-empty；
* consumer 批量 drain；
* producer 与 consumer index 分离 cache line；
* 拒绝结果必须明确；
* shutdown 后不能再产生幽灵 completion；
* 不使用每消息 `std::function`。

通用 executor 保留给：

* 生命周期控制；
* 配置变更；
* 非高频任务；
* 用户自定义管理操作。

typed mailbox 用于：

* packet；
* logic command；
* output command；
* broadcast task；
* actor/runtime dispatch。

---

## 第三优先级：scatter/gather 与小包合并

当前 Linux 输出 Buffer 最终使用单段 `write`，编码通常会把 header 和 payload 拼成一个字符串。可以新增 segment queue：

```text
[4-byte frame header]
[payload segment]
[next header]
[next payload]
```

发送时：

* Linux：`writev` / `sendmsg`；
* Windows：`WSASend`；
* IOCP：一个 operation 携带多个 WSABUF；
* io_uring：`sendmsg` 或受控的 send bundle。

对小包可以做有限 micro-batching：

```text
触发条件：
- segment 数量达到上限
- byte 数量达到上限
- 本轮 EventLoop 即将结束
- 延迟预算即将到期
```

不要设置无限等待来凑批次。延迟敏感消息可以立即 flush，普通消息允许极短的 owner-turn 合并。

---

## 第四优先级：广播 owner-direct fast path

你当前广播层已经做对了两件关键事情：

* payload 只生成一份，以不可变共享对象持有；
* endpoint 按网络 owner 分组后再投递。

但 owner task 内每个 endpoint 仍会：

* `weak_ptr.lock()`；
* 检查 owner executor；
* executor 内部加锁验证线程；
* 检查 open；
* 执行多级发送预算原子记账；
* 更新带 mutex 的 progress。

而 Dispatcher 已经把任务投递到了正确 owner，逐 endpoint 再验证一次 owner 是重复成本。

可以增加 source-private 接口：

```cpp
OwnerLocalEndpointHandle
TcpConnection::trySendOwnerLocal(SharedPayload);
```

流程改成：

```text
任务进入 owner
-> 一次验证 owner/generation
-> 批量遍历轻量 EndpointHandle
-> owner-local send
-> 批量更新统计
```

公开 `TransportEndpoint::send()` 继续保留完整安全检查；内部已经确认 owner 的批量路径走 fast path。

---

## 第五优先级：将多级预算从“每消息原子”改成“分片信用”

connection、loop、server、global 四层预算是正确的，但每条小消息都操作多组原子变量，cache coherence 成本会逐渐成为瓶颈。

可以采用分片 credit：

```text
GlobalBudget
    ↓ 一次领取一批 credit
ServerBudget
    ↓
LoopLocalCredit
    ↓ 普通整数加减
Connection owner-local accounting
```

原则：

* connection owner-local counter 使用普通整数；
* 每个 loop 从上级预算租赁一批 credit；
* credit 不足时才访问跨核 atomic；
* 回收可批量进行；
* 全局 hard limit 采用保守预留，绝不能超发。

跨线程请求先在 mailbox 层做容量 admission，进入 owner 后再做 owner-local发送记账。

---

## 第六优先级：自适应但有界的 EventLoop 调度

当前固定阶段和固定数量预算是很好的安全基础，但固定 `maxActiveChannelsPerIteration = 64` 等参数不一定适合所有负载。

可以升级为：

```text
硬数量上限
+ 本轮时间预算
+ backlog
+ oldest queue age
+ weighted deficit
```

例如：

* I/O backlog 高时适当扩大 I/O batch；
* Timer lag 上升时提高 Timer 权重；
* lifecycle/close 始终保留最低预算；
* functor queue age 上升时扩大 drain；
* budget exhausted 后下一轮 `poll(0)`，不重新睡眠；
* 不允许任何阶段无限 drain。

时间戳采样也可以从“每任务 `now()`”变成：

* 每批次统一 enqueue timestamp；
* 1/N 抽样；
* Debug/Benchmark 模式全量；
* Release fast mode 低频采样。

这样可以减少读时钟本身的成本。

---

# 六、io_uring 应该如何定位

当前仓库的实测非常有价值。在固定的 WSL2、256 路、64 字节场景中：

* epoll 中位 round trips/s：约 44.46 万；
* io_uring：约 22.19 万；
* epoll P50/P99 更好；
* io_uring P999 更好；
* io_uring 关闭收敛明显更快；
* io_uring 吞吐约为 epoll 的一半。

这不能证明 io_uring 不适合项目，但说明：

> **io_uring 当前不是吞吐捷径，而是需要重新塑造整个 Completion 数据面的独立能力。**

M6/IOE-X11 可以继续，但应保持以下边界：

1. source-private；
2. 不增加公共 backend selector；
3. 不强迫 TcpConnection 把 completion 伪装成 readiness；
4. 不以“使用了 io_uring”作为推广理由；
5. 只有在某个明确 Profile 上赢得吞吐、尾延迟、CPU 或关闭指标，才推广。

后续值得实验的 io_uring 能力包括：

* single issuer；
* CQ 批量 drain；
* multishot accept；
* provided buffer ring；
* multishot recv；
* registered files；
* 大块 payload 才尝试 send zerocopy；
* SQPOLL 只用于允许独占 CPU 核的部署模式。

对于普通小包 TCP 游戏服，最终完全可能出现：

```text
高频小包、普通部署：epoll 更优
大量异步 operation、复杂 completion 生命周期：io_uring 更优
Windows：IOCP 更优
```

这并不损害“通用性”，反而说明架构正确。

---

# 七、内存和 CPU Cache 层面的极致优化

## 1. 拆分 TcpConnection 热字段与冷字段

热字段：

* state；
* fd/operation identity；
* input/output index；
* writing/reading flags；
* pending bytes；
* generation；
* backpressure state。

冷字段：

* name；
* local/peer address；
* callback 配置；
  -错误描述；
  -统计快照；
  -关闭详细信息。

将热字段集中在少量 cache line 内，可以降低每次收发触碰的数据量。

## 2. Per-loop slab/pool

适合池化：

* Connection slot；
* Channel；
* Completion operation；
* Packet block；
* mailbox node；
* Output segment descriptor。

每个 EventLoop 使用自己的 pool，避免跨核 allocator 竞争和远端 NUMA 内存。

## 3. 避免 false sharing

重点隔离：

* MPSC/SPSC producer index 与 consumer index；
* 每 loop 统计；
* global budget 与 loop-local credit；
  -高频 atomic 状态与冷统计字段。

## 4. 稳定句柄代替热路径 shared_ptr

公共 API 可以继续使用 `shared_ptr` 保证易用性和生命周期安全，但内部批量路径可以使用：

```text
slot index + generation + owner id
```

进入 owner 后解析成连接指针。

这特别适合：

* 广播 recipient；
* session endpoint；
* completion observer；
* owner outbox。

---

# 八、编译器、操作系统和硬件层

这些优化应放在数据路径稳定之后。

## 编译层

建议提供明确的构建 Profile：

```text
PortableRelease
NativeTunedRelease
PGORelease
Sanitizer
BenchmarkInstrumented
```

生产性能构建可以使用：

* ThinLTO/LTO；
* PGO；
* 部署机器对应的 `-march`；
* Linux 可进一步实验 BOLT；
* 热路径 `noexcept`；
* cold error path 单独布局；
* 日志和全量观测从热路径移出。

不要让 `-march=native` 成为发布包默认值，应区分 portable package 和 deployment-tuned build。

## 操作系统层

提供部署指南而不是强制默认：

* EventLoop thread CPU affinity；
* NUMA-local allocation；
* NIC IRQ affinity；
* RSS/RPS/XPS；
* `SO_REUSEPORT`；
* listen backlog；
* fd limits；
* socket buffer；
* TCP_NODELAY；
* busy polling 仅作为独占核低延迟 Profile；
* 每 listener/worker 的连接倾斜观测。

`SO_REUSEPORT` 可以让各 I/O owner 自己 accept，减少 acceptor 到 worker 的 fd handoff，但必须测量负载均衡、连接倾斜和关闭语义，不能直接作为默认方案。

---

# 九、必须升级性能验证体系

当前 regression budget 中，不少吞吐指标允许 30% 回退，P999 允许 75%，连接 churn 尾延迟甚至允许 90% 回退。它适合阻止灾难性退化，但无法指导细粒度性能工程。

建议分成两层。

## 普通 PR Gate

用于发现明显退化：

* 编译和正确性；
* sanitizer；
* 基本容量；
* 宽松性能阈值；
* 短时运行。

## 固定裸机 Performance Lab

用于决定优化是否推广：

* 固定 CPU、内核、编译器；
* 固定 CPU affinity；
* 固定频率策略；
* 原生 Linux，不使用 WSL2 作为最终生产结论；
* 多次交错采样；
* 置信区间或 bootstrap；
* 3%～5% 级别变化检测；
* 保留 `perf stat`、flamegraph、机器信息和 commit identity。

基准矩阵至少应覆盖：

```text
包大小：
32 / 64 / 128 / 256 / 1024 / 4096 / 16384

连接数：
1K / 10K / 100K / 更大 idle capacity

活跃比例：
0.1% / 1% / 10% / 100%

负载：
50% / 70% / 85% / 95% / overload

模式：
ping-pong
单向吞吐
burst
连接 churn
广播
慢客户端
固定 Tick
跨线程 queued logic
shutdown/recovery
```

每个场景记录：

```text
cycles/message
instructions/message
LLC misses/message
branch misses/message
allocations/message
copied bytes/message
syscalls/message
wakeup/message
handoff/message
context switches
P50/P99/P999
RSS/connection
queue age
overload recovery
shutdown convergence
```

---

# 十、我建议的实施优先级

结合当前代码，我建议按以下顺序推进：

| 优先级 | 工作项                                              | 原因                                      |
| --- | ------------------------------------------------ | --------------------------------------- |
| P0  | 建立 hot-path cost ledger 和固定裸机基线                  | 没有成本模型，所有优化都容易变成猜测                      |
| P1  | PacketFramer 零分配 view API + OwnedPacket          | 当前存在明确的每帧 allocation/copy               |
| P1  | NetworkLoop ↔ LogicShard typed batch mailbox     | 消除 `std::function + mutex + deque` 的每包税 |
| P2  | epoll slot arena + generation + O(1) batch merge | 降低每个 readiness notice 的 hash/cache 成本   |
| P2  | OutputSegments + writev/WSASend                  | 避免 header/payload 拼接和多次 syscall         |
| P2  | 广播 owner-direct batch send                       | 复用现有共享 payload，去掉逐 endpoint 重复验证        |
| P3  | 分层 budget credit cache                           | 降低每包多级 atomic cache-line ping-pong      |
| P3  | 自适应有界 EventLoop budget                           | 提高不同活跃度下的吞吐与尾延迟稳定性                      |
| P4  | per-loop pool、hot/cold split、NUMA                | 进一步降低 allocator 和 cache miss            |
| P5  | 完整 io_uring server composition                   | 在用户态热路径已经足够轻之后再比较 backend               |

其中最值得立即执行的三项是：

1. **零分配解帧与 OwnedPacket；**
2. **typed mailbox/outbox，禁止普通高频包进入通用 functor 队列；**
3. **epoll registration slot 化，去掉 wait 热路径哈希与线性扫描。**

---

# 最终目标形态

`game-net-core` 最理想的定位不是“又一个 epoll/IOCP 封装库”，而是：

> **一个以 owner-loop 与可验证生命周期为公理，能够针对不同负载组合出零 handoff、批量 queued、固定 Tick、分片混合等运行模型，并在 Linux/Windows 上使用各自真实 I/O 语义的高性能网络运行时内核。**

公共层保持：

* 小而稳定；
* 生命周期清楚；
* typed result；
* 背压有界；
* backend 不泄漏。

内部层追求：

* owner-local；
* 零分配 fast path；
* 最少复制；
* O(1) dispatch；
* 批量 mailbox；
* 批量系统调用；
* cache-local；
* 平台专用实现；
* 基于证据推广。

当前 `GameNet::core` 已经进入稳定 source compatibility 约束，而 protocol、transport、logic、broadcast 仍是 provisional，平台 backend 仍是 internal，这恰好提供了正确的优化边界：**稳定核心契约，不冻结内部数据路径；先在 provisional/source-private 层验证，再决定是否提升。**

一句话概括下一阶段：

> **先把每条消息在用户态的分配、复制、锁、原子操作和 handoff 压到最低，再让 epoll、IOCP、io_uring 发挥各自的上限。**
