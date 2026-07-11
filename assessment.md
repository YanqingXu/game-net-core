# 审计结论

> **历史输入快照。** 本文记录 2026-07-11 的初始审计，因此正文中的 Draft PR、
> Request changes 与未发布判断不应被当作当前状态。整改后的最新审计见
> [`docs/development/phase4_audit_2026-07-11.md`](docs/development/phase4_audit_2026-07-11.md)，
> 已发布 Preview 的身份、证据与校验和见
> [`docs/development/releases/v0.2.0-phase4-preview.md`](docs/development/releases/v0.2.0-phase4-preview.md)。

截至 **2026 年 7 月 11 日**，应把项目状态严格拆成两个基线：

1. **`main` / 已发布基线**：Phase 1～3.5 的 Reactor/TCP Core 已收口，代码树仍以 `GameNet::core` 为主；仓库 README 也仍把 Reactor/TCP 定义为当前 active target。([github.com][1])
2. **Phase 4 开发基线**：PacketFramer、Transport、Session、LogicLoop、Pipeline 示例和 Broadcast 都存在于当前 **draft PR #4**，但尚未合入 `main`、尚未发布。PR #4 目前是 1 个提交、57 个文件、2829 行新增，状态为 open、draft、mergeable。

我的最终判定是：

| 审计对象                       | 判定                               | 说明                                                  |
| -------------------------- | -------------------------------- | --------------------------------------------------- |
| Phase 2/3 Reactor/TCP Core | **通过，Preview-grade 稳定**          | 跨平台 CI、Sanitizers、长稳、真实 IOCP 数据路径均有证据               |
| `v0.1.0-core-preview`      | **Tag 已发布；GitHub Release 未发布**   | 是可追溯 annotated tag，不是带发布说明和资产的 GitHub Release       |
| Phase 4 代码基础               | **有条件通过**                        | 模块边界和基本契约正确，但存在多项生命周期、状态机和测试真实性缺口                   |
| 当前 PR #4                   | **建议 Request changes，不应按现状直接合并** | 主要阻断项是生命周期、真实 fuzz、并发测试、Pipeline 认证状态机和 Intent 语义校验 |
| 完整游戏网络底座                   | **尚未到 70%～75% 的可发布成熟度**          | “模块文件已存在”接近该口径；按可合并、可验证、可发布口径约为 55%～65%             |

---

# 1. Phase 2/3 Core 审计

## 1.1 `v0.1.0-core-preview` 的真实发布状态

Core 的证据链是成立的：

* 最后验证提交为 `c4818d4b...`；
* `ci` run `29079836593` 在该提交上通过 Linux Debug、ASan/UBSan、TSan、Linux Release 和 Windows MSVC IOCP 五个 job；
* annotated tag `v0.1.0-core-preview` 指向该验证提交。
* 我独立核对了该 run，五个 job 的结论均为 `success`。

但“发布”需要更精确地表述：

> **当前是 tagged core preview，不是 GitHub Release。**

GitHub 仓库页面仍明确显示 **No releases published**。([github.com][1])

这不影响 tag 作为源码基线的有效性，但缺少：

* Release notes；
* 已知限制；
* 构建产物或源码归档校验和；
* benchmark / soak artifact 索引；
* 下游升级说明。

因此，文档里建议统一使用“已打 annotated preview tag”，不要笼统写成“GitHub Release 已发布”。

## 1.2 跨平台 CI 与 Windows IOCP

当前 CI 矩阵覆盖：

| Job               | 实际范围                               | 审计判断     |
| ----------------- | ---------------------------------- | -------- |
| Linux CMake Debug | 全量 CTest、守卫、安装消费方编译                | 通过       |
| Linux ASan/UBSan  | Debug + 全量 CTest                   | 通过       |
| Linux TSan        | Debug + `threading` 标签测试           | 通过，但不是全量 |
| Linux Release     | Release + 全量 CTest                 | 通过       |
| Windows MSVC IOCP | Windows Debug + 全量 CTest + 安装消费方编译 | 通过       |

工作流确实分别启用了 `-fsanitize=address,undefined` 和 `-fsanitize=thread`；TSan job 只执行 `ctest -L threading`。Windows job 使用真实 MSVC/IOCP 路径，但远端只跑 Debug 配置。

Windows 支持不是“能编译的兼容层”，而是已经覆盖真实 IOCP 数据路径：

* `AcceptEx`；
* `ConnectEx`；
* `WSARecv`；
* `WSASend`；
* `PostQueuedCompletionStatus` wakeup；
* pending overlapped I/O 的取消与 teardown 顺序。

这部分可以认定为 **功能可用且具有较强契约证据**。

## 1.3 Sanitizers 与长稳证据

Core 当前有三类独立验证：

* ASan/UBSan：全量 Debug CTest；
* TSan：线程相关 slice；
* long-soak：46 个 threading tests 重复 50 次，共 2300 次执行，远端成功。
* Linux epoll 与 Windows IOCP 的 Release benchmark workflow 均成功，并上传原始 JSON。

因此 Core 的正确定位是：

> **可以作为后续组件迁移的冻结 Preview 基线；不应宣传为 production-ready。**

剩余工程缺口主要是：

1. TSan 只覆盖 `threading` 标签，不覆盖全量测试。
2. Windows Release 目前是本地结果和手动 benchmark，不是主 CI 必需 job。
3. 性能数据只是快照，没有回归阈值。
4. long-soak 是手动 workflow，不是每个候选版本的自动门禁。
5. 远端 install consumer 只配置和编译，没有执行其 `main()`；工作流的“verify”命名略高于实际证据。

---

# 2. Phase 4 当前落地状态

## 2.1 先纠正文档与远端证据的不一致

PR #4 中的 `migration_status.md` 和 `assessment.md` 仍写着“Phase 4 远端 Linux/sanitizer/TSan 证据待产生”。

但实际情况已经变化：

* PR head `0d62054e...` 对应 `ci` run #32；
* run #32 已完成且结论为 success；
* Linux Debug、ASan/UBSan、TSan、Release、Windows MSVC IOCP 五个 job 全部成功。

因此，在 PR 合并前必须更新证据账本，至少记录：

* PR head SHA；
* GitHub 生成的 merge-ref SHA；
* run id `29147391402`；
* 五个 job 的结果；
* 74-test 数量；
* TSan 实际执行的是哪 51 个线程标签测试。

否则当前文档已经不是最新事实。

## 2.2 是否遵循 Intent → Invariants → Contracts → Tests

`AGENTS.md` 要求顺序是：

`Intent → Invariants → Threading → Ownership → Contracts → Tests → Implementation`，并明确要求回答 owner thread、释放者、可重入 callback、跨线程 marshal 和具体测试文件。

对 PR #4 的判断应分成两层：

### 产物结构：基本符合

六个 Phase 4 交付物均具有：

* active intent；
* 线程和所有权说明；
* 独立 CMake target 或 example target；
* contract / integration test；
* 实现代码。

### 开发历史：不可审计

当前 PR 把原路线图 PR-1～PR-6 的全部内容压在 **一个提交** 中。无法从提交历史证明：

* intent 是否先于代码确定；
* contract 是否先失败再实现；
* 每一层是否独立评审和验证；
* 后一层是否只依赖已冻结的前一层。

因此更准确的结论是：

> **最终文件布局遵循了意图驱动的形式，但提交历史没有提供意图驱动过程的证据。**

---

# 3. Phase 4 模块逐项审计

## 3.1 PacketFramer

### 已完成的部分

PacketFramer active intent 明确了：

* `uint32` 大端长度前缀；
* 半包、粘包、空帧；
* 最大 payload；
* faulted 状态与显式 reset；
* caller-owned、非线程安全；
* 不承载业务字段。

实现和基本 contract 覆盖了上述主要行为。

### 阻断问题

**1. “fuzz smoke”并不是真正的 fuzz。**

当前测试只用固定随机种子生成合法 payload，先 `encode()`，再随机分块喂给 parser，验证往返结果。它没有把任意畸形字节作为输入，也不是 `LLVMFuzzerTestOneInput` harness。

而原 `mini_trantor` 已有真正的 libFuzzer 入口，会把任意输入同时送入单帧和批量解码。

因此 PR 文案中的“fuzz passed”应改为“deterministic chunked round-trip smoke passed”，并补回真正的 fuzz target。

**2. 缺少每次解析的 frame/byte budget。**

当前 `push()` 会先把整个输入追加到内部 `std::string`，再循环解析所有完整帧。恶意客户端可以用大量小帧让单次 I/O callback 长时间占用 owner loop，也可以使一次追加产生很高的瞬时内存压力。原项目 PacketFramer 已经有 `maxFramesPerBatch`。

进入 Pipeline 前应增加：

* `maxBufferedBytes`；
* `maxFramesPerPush`；
* 剩余输入的 owner-loop continuation；
* 超限时的明确状态和测试。

**判定：有条件通过；真实 fuzz 和解析预算是合并前门禁。**

## 3.2 TransportEndpoint

### 已完成的部分

接口保持较窄：

* immutable session id；
* owner loop；
* send；
* close；
* isOpen；
* TCP adapter 通过 `weak_ptr<TcpConnection>` 观察底层连接。

这符合“不立即引入庞大 TransportManager”的路线。

### 阻断问题

`TcpTransportEndpoint` 同时保存：

* `EventLoop* ownerLoop_`；
* `weak_ptr<TcpConnection>`。

`send()` / `close()` 在确认弱连接仍存活之前，先解引用 `ownerLoop_` 检查线程。若 endpoint 比 connection 和 loop 活得更久，会出现悬空 loop 指针风险。现有测试只覆盖“connection 已过期、EventLoop 仍存活”，没有覆盖 endpoint outlives loop。

这与项目生命周期规则存在直接张力：仓库明确禁止没有生命周期保证的 raw-pointer 回调和“probably still alive”假设。

此外，BroadcastRouter 会在 management loop 调用 `endpoint->isOpen()`，但 TransportEndpoint intent 没有明确声明 `isOpen()` 是跨线程安全 snapshot observer。

需要补齐：

* endpoint / connection / loop 的严格 lifetime dominance；
* 或 loop lifetime token；
* `isOpen()` 的跨线程契约；
* wrong-thread close；
* endpoint outlives connection/loop；
* loop stop 后 queued send/close 的行为。

**判定：接口方向正确，生命周期契约未闭环。**

## 3.3 PlayerSession / SessionManager

### 已完成的部分

设计具备合理的管理面：

* 单一 management loop 所有权；
* player 和 transport 双索引；
* duplicate login replace/reject；
* rebind；
* stale disconnect 防护；
* heartbeat / idle expiration；
* endpoint close 回到 endpoint owner loop。

### 阻断问题

**1. TransportSessionId 冲突会破坏双索引不变量。**

当两个不同玩家使用相同 transport id 调用 `authenticate()` 时，代码会直接：

```cpp
byTransport_[session->transportId().value] = session;
```

第二个 session 覆盖 transport 索引，但第一个仍留在 `byPlayer_`。此时两个 player session 认为自己绑定了同一个 transport，只有一个能从 transport 索引找到。

这是可确定构造的逻辑错误，必须显式 reject、rebind 或先清理旧绑定。

**2. 异步方法捕获裸 `this`。**

`postAuthenticate`、`postOffline`、`postHeartbeat` 都把裸 `this` 放入 EventLoop 队列。若 SessionManager 在任务执行前销毁，会形成 use-after-free 风险。

原 `mini_trantor` SessionManager 已经保留了 lifetime token；当前迁移反而丢失了这项保护。

**3. 测试声明高于实际覆盖。**

intent 声称测试覆盖 callback re-entry、endpoint-loop closure 和 cross-thread async submit；当前测试只实际跨线程调用了 `postAuthenticate`，没有覆盖：

* `postOffline` / `postHeartbeat` 的竞争；
* callback 内重新进入 manager；
* manager destruction with queued work；
* transport id collision。

**判定：基本状态机可用，但索引唯一性和异步生命周期是 P0。**

## 3.4 LogicLoop / GameCommandQueue

### 已完成的部分

当前实现具备：

* 值类型 GameCommand；
* command count / queued bytes / payload bytes 三种上限；
* Accepted / QueueFull / PayloadTooLarge / Stopped；
* 固定 tick、每 tick 最大 drain；
* queue snapshot 和 high-water mark；
* handler/output/metric callback。

### 阻断问题

**1. 所谓“concurrent submit”测试没有真实并发。**

测试先启动一个线程完成全部 submit，然后立即 `join()`，之后才调用 `eventLoop.loop()`。因此 submit 与 drain 从未重叠。

它也没有验证 intent 声称的“handler/output callback 可重入 submit”。

TSan 能证明当前执行路径没有报告竞态，但这条测试没有制造最重要的并发交错。

**2. stop / restart 语义不完整。**

`stop()` 永久 `queue_.close()`；之后再次 `start()` 会重新安装 timer，但 queue 仍拒绝所有命令，形成“运行中但永远不能接收”的状态。

必须二选一：

* 明确 LogicLoop 是 one-shot，stop 后禁止 restart；
* 或提供 queue reopen/reset 并规定旧命令处理方式。

**3. 生命周期与 observer 契约不足。**

Timer callback 捕获裸 `this`；`running_` 是普通 bool，而公开的 `running()` 没有 owner-thread assert，也不是 atomic。若调用方跨线程读取，会产生数据竞争风险。

原 `mini_trantor` LogicLoop 已使用 atomic running、独立 lifetime token 和更完整的 backpressure 结果。

**4. stop 时的 pending command 策略缺失。**

当前 stop 只关闭 admission 和取消 timer，队列内命令保留但永不消费，也没有 dropped/drained 数量。

**判定：API 骨架正确，但当前测试不足以证明线程和生命周期契约；它应是后续最高优先级。**

## 3.5 Pipeline 示例

### 已完成的部分

Pipeline 保持在非安装 example 中，没有把组合层反向塞入 core。它通过真实 TCP 测试了：

* 分段 AUTH frame；
* AUTH_OK；
* 两个粘连 command frame；
* LogicLoop 响应；
* 断线后 session 清理。

这是正确的第一条 vertical slice。

### 阻断问题

**1. `AUTH + command` 同批到达会被错误关闭。**

收到第一条 AUTH frame 后，Pipeline 调用 `postAuthenticate()`，该操作总是排队；在 callback 执行前，同一 `onMessage()` 中的下一条 frame 仍看到 `sessionId == 0`，于是被当作另一条 AUTH。普通 command 会触发 ProtocolError。

当前集成测试是在收到 `AUTH_OK` 后才发送两个 command，所以没有覆盖这个窗口。

需要显式状态机：

```text
Unauthenticated
  -> Authenticating
  -> Online
  -> Closing
```

`Authenticating` 期间必须规定：

* bounded pending frames；
* 暂停读取；
* 或明确 reject/close。

**2. shutdown 仍不安全。**

Pipeline、SessionManager、LogicLoop、server callbacks 多处捕获裸 `this`。`stop()` 先关闭 LogicLoop，再停止 server，但没有等待：

* queued authentication；
* session cleanup；
* logic output；
* endpoint send；
* server close callback

全部收敛。

**3. 只使用一个 EventLoop。**

当前示例证明了模块组合，但没有证明生产形态中的：

* I/O loop；
* management loop；
* logic loop

三者分离后的 handoff 和销毁顺序。

**判定：vertical slice 成立，但状态机与 shutdown 尚未达到可提升为正式 library 的程度。**

## 3.6 Broadcast

### 已完成的部分

Broadcast 设计总体清晰：

* 上层提供 session 列表，不计算 AOI/房间；
* management loop 路由；
* 按 endpoint owner loop 分组；
* fanout / byte hard limit；
* low-priority soft limit；
* payload 共享；
* chunked dispatch；
* 细分 metric reason。

### 主要缺口

1. `TransportEndpoint::isOpen()` 的跨线程安全没有成为接口级不变量。
2. `BroadcastPlan` 是公开可构造结构，允许外部产生 `ownerLoop == nullptr` 的 batch；dispatcher 会直接解引用。
3. Plan 和 task 持有 endpoint/payload，但不拥有或观察 owner loop 生命周期。
4. 测试只要求看到某一种 hard limit reason，没有分别证明 fanout hard limit 和 byte hard limit。
5. `EndpointClosed`、`SendRejected` 没有被完整验证。
6. 只有 fake endpoint，没有真实 TCP endpoint 的多 loop 集成。
7. 没有高 fanout、reconnect、ordering、slow endpoint 的 soak/performance 证据。当前源项目已有多类 broadcast stress/latency 测试，迁移时不应完全丢失这些证据。

**判定：算法骨架较好，适合作为 PR-6，但还不是全链路背压闭环。**

---

# 4. Intent 元数据审计

## 4.1 整体分类

PR #4 中共有：

* 24 active；
* 23 deferred；
* 11 legacy。

Intent index 对三种状态的语义定义是正确的：

* active 才是当前实现授权；
* deferred 只是未来设计资产；
* legacy 不能作为当前范围、路线或验证证据。

大部分分类合理：

* `connection_transport` 继续 deferred 是正确的，因为它描述的是 TcpConnection 内部 plain TCP/TLS 读写 helper，与当前上层 `TransportEndpoint` 不是同一抽象。
* TLS 继续 deferred 正确。
* UDP/KCP 继续 `GameNet::experimental` + `experimental-only` 正确。
* v1/v2/v3/v5/v6 等历史阶段文档归入 legacy 合理。

## 4.2 需要修正的分类与守卫

### 问题一：Pipeline active intent 指向不存在的 target

Pipeline intent 写的是：

```yaml
status: active
target: GameNet::game
```

但当前 CMake 只有：

* `gamenet_pipeline_demo_support`；
* `gamenet_game_server_pipeline_demo`；

没有 `GameNet::game` alias，也没有安装该 target。

这违反了 Intent README 对 active 的定义：active 必须指向已实现组件。

建议不要为了满足元数据而创建正式 `GameNet::game` library；更合适的是扩展 metadata：

```yaml
artifact_kind: example
target: gamenet_game_server_pipeline_demo
```

### 问题二：metadata guard 只校验字符串合法，不校验目标真实存在

当前 guard 会检查：

* status / target / source / gate 是否属于允许集合；
* active/deferred/legacy 与目录索引是否一致；
* stale source-project 字符串是否出现在 active body。

它不会检查：

* `GameNet::*` alias 是否在 CMake 中真实存在；
* intent 声明的测试文件是否存在；
* 测试是否注册到 CTest；
* active target 是否实际安装；
* example intent 是否真的有对应 executable。

因此它能防止格式漂移，不能防止语义漂移。

### 问题三：`game_backpressure_policy` 已部分实现，却仍整体 deferred

该 deferred intent 描述了：

* LogicLoop admission limits；
* command backlog；
* broadcast hard/soft budgets；
* priority shedding；
* metric reasons。

其中若干部分已经出现在 active LogicLoop 和 Broadcast 实现中。继续把整份文档标为 deferred 并非“实现越权”，因为本次确实另写了 active intents；但它会让读者无法判断：

* 哪些章节已经被 active intent 取代；
* 哪些仍是未来设计；
* 实现是否偷偷依赖 deferred 设计资产。

建议拆分为：

* active `logic_admission.intent.md`；
* active `broadcast_backpressure.intent.md`；
* deferred `end_to_end_game_backpressure.intent.md`；

或者增加：

```yaml
superseded_sections:
derived_active_intents:
```

### 问题四：`migration_source: native` 的溯源能力不足

PacketFramer、SessionManager、LogicLoop、Pipeline、Broadcast 在 `mini_trantor` 中都有明确前身，甚至已有更丰富的 fuzz、lifetime token、backpressure 和 stress tests。

把这些全部标成 `native` 会隐藏“重设计自旧模块”的事实。更好的元数据应记录：

```yaml
migration_source: mini_trantor
source_commit: 3eba368...
source_paths:
migration_mode: redesign
```

这对识别迁移过程中丢失的安全机制尤其重要。

---

# 5. 对当前进度百分比的修正

PR 分支的 `assessment.md` 把“完整游戏网络底座愿景”估算为约 70%～75%。

这个数字只有在“计划中的六类组件是否都有文件”这一口径下成立。按更严格的审计口径：

| 口径                               |          建议估算 |
| -------------------------------- | ------------: |
| 已合入、已发布的 `main`                  | **约 40%～45%** |
| 加上 draft PR #4 的功能骨架             |     **约 60%** |
| Phase 4 计划交付物的“文件存在率”            |   **接近 100%** |
| Phase 4 生命周期/并发/失败契约成熟度          | **约 50%～60%** |
| 相对 `mini_trantor` 对应模块的行为与测试迁移覆盖 | **约 25%～40%** |
| Production readiness             |     **仍明显不足** |

造成差距的不是模块数量，而是：

* 真正的 fuzz 缺失；
* shutdown/lifetime token 丢失；
* 并发测试没有真实交错；
* Pipeline 认证状态窗口；
* Broadcast 缺少真实 transport soak；
* Phase 4 没有远端 long-soak / benchmark；
* 尚未合入和发布。

---

# 6. 后续路线图：PR-4 到 PR-7+

当前 GitHub PR #4 不应被视为原路线图中的“PR-4 LogicLoop”；它实际是 PR-1～PR-6 的 umbrella draft。建议把它保留为 tracking PR，并提取为可独立评审的 stacked merge units。

前置条件是先把 PacketFramer、Transport、Session 三层从当前大 PR 中独立出来，并解决前述 P0。

## 逻辑路线编号

| 优先级 | PR                         | 范围                                        | 合并门禁                                                                                                                        |
| --: | -------------------------- | ----------------------------------------- | --------------------------------------------------------------------------------------------------------------------------- |
|   1 | **PR-4 LogicLoop**         | Queue + fixed tick + metrics              | 真正多 producer 并发；submit/drain 重叠；callback re-entry；明确 one-shot/restart；stop 的 drain/drop 语义；lifetime token；TSan              |
|   2 | **PR-5 Pipeline 示例**       | TCP → Framer → Session → Logic → Response | 显式认证状态机；`AUTH+command` 同批测试；disconnect-before-auth-complete；多 loop handoff；有界 pending input；安全 shutdown                     |
|   3 | **PR-6 Broadcast**         | Router + Dispatcher + backpressure        | `isOpen()` snapshot 契约；opaque/validated plan；全 reason 矩阵；真实 TcpTransportEndpoint 双 loop 测试；large-fanout soak；send rejection |
|   4 | **PR-7 Phase 4 closure**   | 不新增功能，只补证据并发布                             | 五 job 全绿；51 threading × 50；真实 fuzz；Windows Release CI；安装 consumer 实际运行；Phase 4 benchmark；文档更新；版本提升                          |
|   5 | **PR-8 Coroutine bridge**  | 仅调度适配，不改变 core ownership                  | owner-loop resume；取消；awaiter destruction；无隐式阻塞；只有出现明确使用者时才启动                                                                |
|   6 | **PR-9 TLS adapter**       | 可选独立 transport target                     | 非阻塞 handshake；WANT_READ/WANT_WRITE；证书验证；timeout；close_notify；pending I/O shutdown；Linux/Windows CI                          |
|   7 | **PR-10 UDP experimental** | 独立实验 target                               | datagram read budget；truncation；IPv4/IPv6；peer identity；zero-length；burst fairness；Linux epoll + Windows IOCP               |
|   8 | **PR-11 KCP preview**      | 构建于 UDP experimental                      | fake clock/network；loss/reorder/duplicate/jitter；重传；seq wrap；fragment/reassembly；内存与 session 上限；decoder fuzz                |

## PR-7 Phase 4 closure 应发布为新版本

当前 CMake 项目版本仍是 `0.1.0`，但 PR #4 新增了五个安装级公共 target。

不建议把带有大量新公共 API 的 Phase 4 继续作为同一个 `0.1.0` 身份发布。更清晰的版本应是例如：

```text
v0.2.0-phase4-preview
```

并创建正式 GitHub Release，附：

* 精确 tag / commit；
* known limitations；
* Linux / Windows CI run；
* sanitizer 结果；
* soak 结果；
* benchmark artifacts；
* 安装消费方验证；
* 不稳定 API 清单。

---

# 7. 进入下一阶段前必须补齐的工程护栏

| 护栏             | 最低要求                                                                                   |
| -------------- | -------------------------------------------------------------------------------------- |
| 生命周期           | 所有 queued/timer callback 禁止无保证的裸 `this`；引入 lifetime token、weak owner 或显式 shutdown/join |
| 活跃 Intent 语义   | active target 必须真实存在；example 与 installed target 分开表示                                   |
| Intent-to-test | intent 中列出的每个 test path 必须存在且注册到 CTest                                                 |
| 源迁移追踪          | 每个迁移模块记录 source commit/path，以及 kept/deferred/dropped invariants                        |
| 并发测试           | 不接受“线程提交后 join，再启动 loop”作为并发证明；必须制造 submit/drain、close/callback、destroy/queued-work 交错 |
| Fuzz           | PacketFramer 和未来 UDP/KCP decoder 必须有真正的 sanitizer-backed fuzz harness 和最小 corpus       |
| 长稳             | Phase 4 的 51 个 threading tests 至少远端 repeat-50；Pipeline/Broadcast 再增加专项 soak            |
| 包验证            | 安装 consumer 不仅编译，还要在 Linux/Windows 执行                                                  |
| 性能             | 增加 framing throughput、queue lag/P99、broadcast fanout latency 和内存高水位基线                  |
| 证据账本           | 同时记录 head SHA、merge-ref SHA、run id、job 范围、测试数；禁止用本地结果替代远端证据                            |
| 发布             | 公共 target 增加后提升版本，并用正式 Release 固化 known limitations                                    |
| 依赖方向           | 保持 `core` 永不依赖 protocol/session/game/experimental；experimental target 默认关闭             |

---

# 最终合并建议

**Core Phase 2/3：审计通过。**

**当前 Phase 4 PR #4：建议 Request changes。** 在合并前至少必须解决以下六个阻断项：

1. SessionManager transport id 冲突。
2. SessionManager / LogicLoop / Pipeline 的 queued callback 生命周期。
3. PacketFramer 的真实 fuzz 和 frame/byte processing budget。
4. LogicLoop 的真实并发、re-entry 和 stop/restart 契约。
5. Pipeline 的 `Authenticating` 状态及 `AUTH+command` 同批输入。
6. Active intent target 与实际 CMake/example target 的语义校验。

最优的下一步不是继续添加 TLS/UDP/KCP，而是把当前 PR #4 转为 tracking PR，按 Packet/Transport/Session → LogicLoop → Pipeline → Broadcast 的依赖顺序拆分，并用 PR-7 专门完成 Phase 4 的远端证据和 Preview Release 收口。

[1]: https://github.com/YanqingXu/game-net-core "GitHub - YanqingXu/game-net-core: A modern C++23 networking foundation for game servers. · GitHub"
