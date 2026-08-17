现有的高性能模型除了Reactor还有哪些？

有，而且要先区分一个概念：**Reactor 本质上是 I/O 事件分发模型**，并不是完整的“高性能服务器运行模型”。在高性能服务器领域，除了 Reactor，常见的还有 **Proactor、Completion Queue、Actor、SEDA、Shared-Nothing、Poll Mode、协程 Runtime** 等模型，它们解决的问题并不完全在同一层。

如果你是在设计 `game-net-core`，我建议把这些模型按层次理解。

### 1. Reactor：就绪通知

典型：

```text
socket
  ↓
epoll / kqueue
  ↓
Reactor
  ↓
read/write
  ↓
callback / coroutine / logic
```

核心语义是：

> 系统告诉你“现在可以读/写了”，真正的 `read/write` 由应用程序执行。

Linux `epoll`、BSD/macOS `kqueue` 最典型。

你当前 `game-net-core` 的：

```text
EventLoop
Channel
Poller
TcpConnection
```

实际上就是经典 Reactor。

---

### 2. Proactor：完成通知

Proactor 和 Reactor 最大的区别是：

```text
Reactor:
    可以读了
        ↓
    application read()

Proactor:
    application submit read
        ↓
    kernel 执行 read
        ↓
    read 完成通知 application
```

即：

> Reactor 通知“ready”，Proactor 通知“completed”。

经典架构：

```text
Application
    │
    │ async_read()
    ▼
OS / IO Runtime
    │
    │ operation completed
    ▼
Completion Queue
    │
    ▼
Completion Handler
```

Windows IOCP 就是最典型的 Completion / Proactor 风格。

这也是为什么你如果做 Windows IOCP 后端，强行把它完全包装成：

```cpp
Poller::poll()
Channel::handleEvent()
```

实际上会有一定语义扭曲。

更合理的抽象可能是：

```text
IO Backend

├── ReadinessBackend
│     ├── epoll
│     └── kqueue
│
└── CompletionBackend
      ├── IOCP
      └── io_uring
```

这对 `game-net-core` 很重要。

---

### 3. io_uring：Completion / Submission 模型

Linux 的 `io_uring` 更接近：

```text
Application
    │
    ▼
Submission Queue
    │
    ▼
Kernel
    │
    ▼
Completion Queue
    │
    ▼
Application
```

也就是：

```text
SQ → Kernel → CQ
```

例如：

```cpp
submit_recv(fd, buffer);

...

completion = cq.pop();
handle(completion);
```

它不像 `epoll`：

```cpp
epoll_wait();
recv(fd);
```

而是：

```cpp
submit recv
    ↓
recv completed
```

因此对于未来的 `game-net-core`，最好不要把：

```text
epoll
IOCP
io_uring
```

全部理解成“Poller 实现”。

更抽象地应该是：

```text
Execution Runtime
        │
        ▼
IO Engine
   ┌────┴─────┐
Ready       Completion
 │               │
epoll         io_uring
kqueue        IOCP
```

---

### 4. Thread-per-Connection / Thread-per-Request

最直接：

```text
Connection 1 → Thread 1
Connection 2 → Thread 2
Connection 3 → Thread 3
...
```

或者：

```text
request → thread pool task
```

传统 blocking server：

```cpp
while (true) {
    auto fd = accept();

    std::thread([fd] {
        while (...) {
            recv(fd);
            process();
            send(fd);
        }
    }).detach();
}
```

传统 OS thread 下连接数高了以后问题明显：

* thread stack 内存
* context switch
* scheduler 开销
* cache locality 差

所以现代高并发网络服务器很少真正：

```text
1 connection = 1 OS thread
```

但这里出现了一个非常有意思的变化。

---

## 5. Coroutine-per-Connection

逻辑上仍然：

```text
Connection 1 → Coroutine 1
Connection 2 → Coroutine 2
Connection 3 → Coroutine 3
```

但不是 OS thread，而是轻量协程。

例如：

```cpp
Task<void> session(Socket socket)
{
    while (true)
    {
        auto packet = co_await socket.recv();
        auto response = co_await process(packet);
        co_await socket.send(response);
    }
}
```

底层可能是：

```text
Coroutine
    ↓
Async Runtime
    ↓
epoll / io_uring / IOCP
```

所以：

> coroutine 并不是 Reactor 的竞争者。

反而经常是：

```text
Reactor + Coroutine
```

或者：

```text
io_uring + Coroutine
```

或者：

```text
IOCP + Coroutine
```

这是现代 C++ Runtime 很值得走的方向。

对于游戏服务器尤其如此。

---

# 6. Actor Model

Actor 模型关注的已经不是 I/O，而是**并发状态管理**：

```text
        ┌─────────┐
        │ PlayerA │
        └────┬────┘
             │ message
             ▼
        ┌─────────┐
        │ MapActor│
        └────┬────┘
             │
             ▼
        ┌─────────┐
        │ Guild   │
        └─────────┘
```

Actor 原则：

```text
Actor owns state
Actor receives messages
Actor processes messages serially
```

因此：

```cpp
playerActor.send(Damage{100});
```

而不是：

```cpp
mutex.lock();
player.hp -= 100;
mutex.unlock();
```

游戏服务器非常适合。

例如：

```text
Connection
    ↓
Session Actor
    ↓
Player Actor
    ↓
Map Actor
    ↓
World Actor
```

底层依然可能：

```text
Reactor
```

所以完整结构可能是：

```text
epoll Reactor
     ↓
Coroutine Runtime
     ↓
Actor Scheduler
     ↓
Game Logic
```

这是三个不同层级。

---

# 7. Shared-Nothing

Shared-Nothing 是很多高性能服务器真正重要的设计。

核心原则：

> 每个线程拥有自己的数据，尽量不共享。

例如：

```text
Core 0
┌───────────────────┐
│ EventLoop          │
│ Connections        │
│ Timers             │
│ Local Queue        │
└───────────────────┘

Core 1
┌───────────────────┐
│ EventLoop          │
│ Connections        │
│ Timers             │
│ Local Queue        │
└───────────────────┘
```

线程之间：

```text
message passing
```

而不是：

```text
mutex + shared container
```

典型思想：

```text
one loop per thread
one shard per core
```

你之前的 `mini-trantor / game-net-core` 实际上已经部分采用这种思想。

例如：

```text
Acceptor
   │
   ├── Loop0 → Conn A C E
   ├── Loop1 → Conn B D F
   └── Loop2 → Conn G H I
```

一个连接一旦分配：

```text
Connection affinity
```

以后基本固定在该 EventLoop。

这对游戏服务器极其重要，因为可以获得：

* cache locality
* 减少锁
* 减少 migration
* 降低 cache line bouncing

---

# 8. SEDA：Stage Event-Driven Architecture

SEDA 是非常值得游戏服务器研究的模型。

全称：

> Staged Event-Driven Architecture

把系统拆成多个阶段：

```text
Network IO
    │
    ▼
Decode
    │
    ▼
Route
    │
    ▼
Game Logic
    │
    ▼
Serialize
    │
    ▼
Network IO
```

每个 Stage：

```text
Queue + Workers
```

例如：

```text
          queue
IO ───────────────→ Decode
                       │
                     queue
                       ↓
                     Logic
                       │
                     queue
                       ↓
                     DB
```

优势：

* 隔离慢模块
* 可以独立扩容
* 天然背压
* 容易监控 queue depth

但是问题也非常明显：

```text
queue
 ↓
cache miss
 ↓
thread hop
 ↓
latency
```

所以游戏服务器不能无脑：

```text
每一层一个线程池
```

否则一次 packet：

```text
IO thread
 → codec thread
 → dispatch thread
 → logic thread
 → serialize thread
 → IO thread
```

线程跳六次，性能可能反而很差。

因此现代做法往往是 **融合 SEDA**：

```text
fast path:
IO → Decode → Logic
全部同线程

slow path:
       ↓
   DB / HTTP / File
       ↓
worker pool
```

这其实非常适合你的 `game-net-core`。

---

# 9. Poll Mode / Busy Polling

还有一种极致性能模型：

```cpp
while (running)
{
    poll_nic();
    process_packets();
}
```

不：

```text
epoll_wait
sleep
wake up
```

而是 CPU 永远转：

```text
poll
poll
poll
poll
```

典型：

* DPDK
* SPDK
* 高频交易
* 高性能 packet processing

架构：

```text
CPU Core 0
    │
    └── NIC RX Queue 0

CPU Core 1
    │
    └── NIC RX Queue 1
```

甚至：

```text
kernel bypass
```

完全绕过传统：

```text
socket
TCP stack
epoll
syscall
```

优点：

```text
极低 latency
极高 throughput
```

缺点：

```text
CPU 100%
工程复杂
网络协议栈复杂
```

普通 MMO 基本没必要，但百万级 packet/s Gateway 或实时服务器值得了解。

---

# 10. Disruptor / RingBuffer 模型

还有一类：

```text
Producer
    │
    ▼
RingBuffer
    │
    ├── Consumer A
    ├── Consumer B
    └── Consumer C
```

目标：

```text
lock-free / low-lock
cache friendly
preallocated
```

典型是 LMAX Disruptor。

游戏服务器可以用于：

```text
IO thread
    ↓
RingBuffer
    ↓
Logic thread
```

或者：

```text
Logic
 ↓
Logging RingBuffer
 ↓
Log Thread
```

非常适合：

* 日志
* telemetry
* 跨线程消息
* packet queue

但它仍然不是 I/O 模型。

---

# 11. CSP / Channel 模型

Go 非常典型：

```go
go playerLoop()

packetCh <- packet
```

思想：

> Don't communicate by sharing memory; share memory by communicating.

结构：

```text
Coroutine
   │
 Channel
   │
Coroutine
```

C++ Runtime 也可以：

```cpp
co_await channel.send(msg);

auto msg = co_await channel.receive();
```

从某种意义上：

```text
Actor
CSP
Coroutine
```

三者可以组合。

---

# 如果把这些模型放到一张图里

真正应该这样理解：

```text
                    Game Server Runtime

                          Game Logic
                             │
                  ┌──────────┴──────────┐
                  │                     │
                Actor                  ECS
                  │
                  ▼
             Execution Model
        ┌─────────┼──────────┐
        │         │          │
    Coroutine   Fixed Tick   Event
        │
        ▼
            Threading Model
      ┌─────────┼───────────┐
      │         │           │
Single Thread  Shared     Shared-Nothing
              Pool
      │
      ▼
              IO Model
       ┌────────┴────────┐
       │                 │
   Readiness         Completion
       │                 │
    Reactor            Proactor
       │                 │
  epoll/kqueue      IOCP/io_uring
       │
       ▼
               Network Stack
       ┌────────┼──────────┐
       │        │          │
      TCP      UDP       QUIC
       │
       ▼
              Hardware
```

因此有一个关键结论：

**Reactor、Actor、Coroutine、Fixed Tick 并不是互斥选项。**

一个高性能游戏 Runtime 完全可能是：

```text
TCP
 ↓
epoll Reactor
 ↓
one-loop-per-core
 ↓
coroutine
 ↓
Actor
 ↓
fixed tick
```

而 Windows：

```text
TCP
 ↓
IOCP Completion Engine
 ↓
one-runtime-per-core
 ↓
coroutine
 ↓
Actor
 ↓
fixed tick
```

---

## 对 `game-net-core`，我反而建议最终形成这种抽象

不要把顶层架构定义成：

```text
Reactor Framework
```

而定义成：

```text
Game Network Runtime
```

底下允许不同模型：

```text
                 Runtime
                    │
        ┌───────────┴───────────┐
        │                       │
     IO Engine              Execution
        │                       │
 ┌──────┴──────┐        ┌───────┼────────┐
 │             │        │       │        │
Ready      Completion  Event    Tick   Coroutine
 │             │
epoll         IOCP
kqueue        io_uring
```

再往上：

```text
Transport
├── TCP
├── UDP
└── QUIC

Dispatch
├── Callback
├── Coroutine
├── Actor
└── Message Queue

Threading
├── Single Reactor
├── Multi Reactor
├── IO + Logic same thread
├── IO / Logic separated
└── Shared-Nothing
```

这样你之前提出的：

> **“稳定网络语义内核 + 可组合运行模型”**

其实就非常有意义。

最值得你重点研究的不是“找一个 Reactor 的替代品”，而是这 **5 个方向**：

| 模型                        | 解决什么          | 对游戏服务器价值 |
| ------------------------- | ------------- | -------- |
| Reactor                   | readiness I/O | ★★★★★    |
| Proactor / Completion     | async I/O     | ★★★★★    |
| Coroutine Runtime         | 异步控制流         | ★★★★★    |
| Actor / Shared-Nothing    | 并发状态          | ★★★★★    |
| Fixed Tick / Event Hybrid | 游戏逻辑调度        | ★★★★★    |
| SEDA                      | pipeline / 背压 | ★★★★     |
| Poll Mode                 | 极致网络性能        | ★★～★★★★★ |

如果你的目标真的是**通用高性能游戏 Runtime**，最终很可能不是选择其中一个，而是形成：

```text
           game-net-core
                 │
        Stable Semantic Core
                 │
       ┌─────────┴──────────┐
       │                    │
 Readiness Engine     Completion Engine
       │                    │
epoll/kqueue         IOCP/io_uring
       │                    │
       └─────────┬──────────┘
                 │
         Execution Policy
    ┌────────────┼────────────┐
 SameThread    Coroutine    Worker
    │
    └────────────┬────────────┘
                 │
         Scheduling Policy
        ┌────────┴────────┐
      Event            Fixed Tick
        │
        └────────┬───────┘
                 │
           Actor / World
```

这比单纯做一个“更好的 Reactor”上限高很多。
