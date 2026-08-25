# game-net-core 高性能执行计划：HP0–HP8

计划重制日期：2026-08-25

长期方向：`goal.md`

性能设计输入：`high-performance.md`、`high-performance2.md`

当前治理基线：`202bf9f993575d144c4c440a3972dd80733da0a7`

## 1. 总结与当前前沿

- M1–M11、IOE-X1–X15 均已关闭；历史细节继续由 `assessment.md`、
  `docs/migration_status.md` 和证据账本保存，不再占用本计划主体。
- 新的唯一 Core 主线为 HP0–HP8：先建立成本账本，再依次优化 framing、跨域
  mailbox、epoll 分发、发送/广播、预算与调度、内存局部性，最后条件性验证协程和
  高级 backend。
- stable v0.3 Core 保持源兼容；协议、传输、逻辑等 provisional 表面允许兼容新增。
  所有快路径先在 provisional/source-private 层验证。
- 同时只允许一个 Core 实现切片；benchmark/lab 证据任务可以并行。每个切片遵循
  `intent -> rules -> contracts -> implementation -> exact-commit evidence`。

## 2. 接口与架构边界

- 向 provisional `GameNet::protocol` 兼容新增：

  - `PacketView`：只借用调用方输入，不能保存、跨线程或跨 `co_await`；
  - `OwnedPacket`：move-only 所有权载体；由 `PacketView::retain()` 至多复制一次；
  - `FrameVisitResult`：包含状态、已消费字节、帧数和 continuation 标志；
  - `PacketFramer::visitFrames(span, visitor)`：使用既有帧数/字节预算，直接解析
    `TcpConnection::inputBuffer` 可读区；
  - 现有 `push()`、`encode()` 和错误语义全部保留，并与新接口做 differential
    contract。

- mailbox/outbox、EventLoop mailbox source、OutputSegmentChain、credit cache 和
  owner-local handle table 首先保持非安装、source-private。
- `GameCommandQueue` 继续作为兼容实现和对照基线，不直接替换成万能 MPMC 队列。
- HP4 通过性能门后，才对 stable `TcpConnection` 做独立兼容审查并兼容新增：

  ```cpp
  TcpSendResult trySendOwned(std::string&&);
  TcpSendResult trySendShared(std::shared_ptr<const std::string>);
  ```

  现有 `trySend(std::string_view)` 保持 borrowed/兼容路径；正常高频回包通过 owner
  outbox 回到连接 owner 后发送。
- 不增加公共 Runtime factory、公共 mailbox、公共 backend selector 或默认
  io_uring；不扩展到 HTTP、TLS、UDP/KCP、RPC、Lua、AOI 等模块。

## 3. 执行里程碑

### HP0：成本账本与固定性能实验室——当前立即执行

状态：**基础设施已实现，exact-commit fixed-lab 证据 `DEFER`**。已建立 active intent、
预登记矩阵、suite target、`gamenet.hot_path_cost.v1` runner/validator、普通 CI 静态守卫
和双平台 self-hosted 手动工作流；当前工作树只能形成 development smoke，不能关闭 HP0。

- 新建默认关闭、非安装的 hot-path benchmark，冻结
  `gamenet.hot_path_cost.v1` 数据格式。
- 覆盖 framing、NetworkLoop↔LogicShard、epoll readiness、发送、广播、连接容量和
  关闭恢复。
- 记录 cycles/instructions/LLC misses、allocations、copied bytes、syscalls、wakeup、
  generic post、handoff、P50/P99/P999、queue age、RSS、过载恢复和 shutdown
  convergence。
- 固定原生 Linux/epoll 和 Windows/IOCP runner、CPU affinity、频率策略、编译器及
  构建类型；WSL 只作开发证据。
- 为每个后续切片预登记主指标、护栏指标和场景，形成优化前 exact-commit baseline。

关闭门：benchmark schema/validator/CI guard 完整，当前 callback+mutex 路径完成基线，
所有样本可追溯到精确提交。

### HP1：零额外复制的 owner-local 解帧

- 更新 active PacketFramer、Buffer、Profile A intent/rules，明确 `PacketView`
  生命周期和 callback re-entry 限制。
- `visitFrames` 直接读取调用方连续可读区；只在完整帧处理后返回消费长度，partial
  frame 留在输入 Buffer。
- owner-local handler 直接消费 view；跨域处理显式转换为 `OwnedPacket`。
- Profile A 和 framing benchmark 增加 legacy/candidate 双路径；只有通过性能门才切换
  默认 recipe。

关闭门：

- owner-local `allocations/frame = 0`、额外 payload copy 为 0、generic
  post/wakeup/handoff 为 0；
- partial/sticky/empty/oversized/budget/fault/reset 行为与旧 API 等价；
- `tests/contract/protocol/test_packet_framer_view.cpp`、现有 PacketFramer contracts 和
  fuzz 全部通过。

### HP2：SPSC typed mailbox 与 owner outbox

- 建立固定容量、原地构造、cache-line 隔离的 `SpscMailbox<T>`；支持 typed
  rejection、batch push/drain 和无静默丢弃的关闭清算。
- 每个 NetworkLoop→LogicShard 和 LogicShard→NetworkLoop 使用独立 SPSC；构造时按
  `producer × consumer × capacity` 核算总内存，超出 `maxMailboxBytes` 直接拒绝，不
  自动回退 MPSC/MPMC。
- 增加 source-private EventLoop mailbox source：预注册、generation-safe、无每消息
  queue node，仅 empty→non-empty 通知；它拥有独立有界阶段，不借用 control/lifecycle
  lane。
- Profile B 使用内部 `DataPlaneCommand<OwnedPacket>`，批量 drain，结果经 owner
  outbox 返回并重验 route generation；现有 `GameCommandQueue` 保留为基线。
- Profile B 通过后，再分别验证 C/D；不因实现复用而提升公共 Runtime API。

关闭门：

- payload copy ≤1、无每消息 queue allocation、每 burst 每方向最多一次 wakeup；
- QueueFull/Stopped/OwnerUnavailable 可区分，Accepted 工作全部处理或形成显式
  cancelled/discarded 终局；
- lost wakeup、wraparound、shutdown residue、route generation 和回调重入合同通过；
- `tests/contract/event_loop/test_event_loop_mailbox_source.cpp`、
  `tests/contract/runtime_model/test_spsc_mailbox.cpp`、现有 Network/Logic split contract
  通过；
- callback+SPSC 相对 callback+mutex 达到推广门。

### HP3：epoll O(1) readiness dispatch

- 将 wait 热路径改为 slot arena：`slot index + generation` 写入
  `epoll_event.data.u64`。
- fd→slot 哈希只用于 register/update/cancel；wait 通过数组直接定位、generation 校验
  和 `lastBatchEpoch` O(1) 合并。
- slot 复用必须先换代；wakeup token、stale fd、兴趣变更和 active-batch
  invalidation 语义保持不变。
- 保持 `ReadinessRegistrationIdentity` 观察语义和 stable Core API 不变。

关闭门：

- wait 热路径无 `unordered_map` 查询和线性 notice 去重；
- slot reuse、fd reuse、重复 mask、remove/re-register、epoch wrap、wakeup 与 stale
  notice 合同通过；
- `tests/contract/io_engine/test_readiness_engine.cpp` 和新增 slot-arena contract 通过；
- 1K/10K/100K 连接、不同活跃比例下达到推广门；Windows 回归零语义变化。

### HP4：分段发送、所有权发送与广播 owner-direct

- 内部建立有界 `OutputSegmentChain`，segment 持有 owned 或 immutable shared storage
  及 offset。
- Linux 使用 `writev/sendmsg`，Windows 使用多 `WSABUF` 的 `WSASend`；单次最多 16 段
  或 64 KiB。
- 队列为空时仍立即尝试 direct send；micro-batching 只合并当前 owner turn，不为凑
  批次等待下一轮。
- owner outbox 使用 owned send；广播 owner task 一次验证 owner/generation 后批量
  shared send，统计批量发布。
- 先以 source-private 路径验证；通过门后才按第 2 节签名增加 stable Core API，并更新
  API manifest/review。

关闭门：

- header/payload 不再为发送强制拼接；部分写保持顺序、offset 和精确 pending-byte
  核算；
- connection/loop/server/global hard limit 不超发，关闭后 segment/byte residue 为 0；
- 新增 owned/shared/segmented send contracts，并保持现有 IOCP segmented-write、
  output-memory 和真实广播集成测试通过。

### HP5：分片 credit 与多级背压降原子成本

- 为 fast path 增加 loop-local credit lease；连接 owner 使用本地记账，上级预算只在
  批量领取/归还时访问跨核 atomic。
- credit 采用保守预留，任何时刻 global、server、loop、connection hard limit 均不得
  超发。
- 公共跨线程 `trySend` 兼容路径继续使用原有精确 admission；mailbox/outbox 快路径
  进入 owner 后消费 credit。
- 停机、连接迁移失败、owner unavailable 和异常必须归还全部 credit。

关闭门：新增并发领取/回收、层级拒绝、关闭清算合同；现有 TCP/Broadcast
memory-budget contracts 全部通过；atomic operations/message 明确下降并达到推广门。

### HP6：自适应有界调度与内存局部性

- HP6-A：EventLoop 在既有数量上限外增加每阶段时间预算、backlog、oldest age 和
  weighted deficit；control/lifecycle 保留最低服务，任何阶段不得无限 drain，budget
  exhausted 后继续 `poll(0)`。
- HP6-B：只池化 HP0 证明为热点的 Packet block、Output segment 和 mailbox storage；
  per-loop pool 的跨线程归还通过有界 reclaim mailbox 回原 owner，并参与 final drain。
- HP6-C：根据 cache-miss/bytes-per-connection 证据拆分 TcpConnection 热冷字段；
  Connection/Channel pool 和 NUMA 策略仅在前述证据表明收益时实施。
- generic functor 队列仍服务低频控制/兼容 API，不为高频 packet 数据面使用。

关闭门：公平性、队列年龄、Timer/lifecycle 最低进度、pool exhaustion、remote reclaim、
shutdown zero-residue 合同通过；每项子优化独立测量，不把多个变化混入一个性能结论。

### HP7：条件性协程与高容量 Timer 实验

启动条件：HP2 已集成，且存在含两个以上真实异步等待点的 Session/RPC-like 流程。
否则记录 `SKIPPED-BY-EVIDENCE`。

- 先重写 deferred async/coroutine intents，修正 shutdown 不恢复、任意线程
  continuation 和通用 functor resume 等旧语义，再激活非安装实验 target。
- 实现 `OwnerTask<T>`、owner-local `CoroutineReadyQueue`、bounded remote resume
  mailbox、CancellationToken、asyncSleep 和 connection awaiters。
- 一个连接/Session 一个长生命周期 coroutine；禁止一包一 coroutine、广播内层
  coroutine 和固定 Tick 实体 coroutine。
- owner-local completion 只入 ready queue，在独立有界阶段批量 resume；跨线程
  resume 经 typed mailbox 返回 origin owner。
- Completion operation 独立于 coroutine frame；内核 terminal completion 前不得释放
  operation storage。
- `PacketView` 禁止跨 suspend；可挂起路径只能持有 `OwnedPacket`。
- 只有 ready queue、取消和 owner-return 语义通过后才考虑 `whenAll/whenAny`；父
  continuation 必须回 origin owner。
- 仅当并发 Timer 规模达到 10 万且现有 TimerQueue 成为已测热点时，增加独立
  `HighVolumeTimerWheel`；现有精确定时队列保留。

关闭门：

- frame allocation ≤1/connection、per-await allocation=0、arbitrary-thread resume=0、
  terminal waiter residue=0；
- 完成 callback+mutex、coroutine+mutex、callback+SPSC、coroutine+SPSC 四组同场景
  对照；
- coroutine+SPSC 满足推广门，否则保持非安装实验且相关公共 intents 继续 deferred。

### HP8：backend、构建 Profile 与最终推广审查

- 在用户态数据路径完成后，重新比较 epoll、IOCP 和 io_uring 的相同
  Server/Client/Profile 场景。
- io_uring 的 multishot accept/recv、provided buffer、registered files、send bundle
  各自独立实验；SQPOLL 仅用于 DedicatedLowLatency 部署 Profile。
- 不因采用 io_uring 而推广；只在明确 Runtime Profile 上满足吞吐、尾延迟、CPU、内存
  和关闭门才扩大实验范围。
- 提供 `PortableRelease`、`NativeTunedRelease`、`PGORelease`、`Sanitizer`、
  `BenchmarkInstrumented` 构建 Profile；portable package 不默认使用 `-march=native`。
- 输出 CPU affinity、NUMA、IRQ/RSS/RPS/XPS、socket buffer、TCP_NODELAY 和可选
  SO_REUSEPORT 部署指南；SO_REUSEPORT 仍受现有 accept-topology 证据门控制。
- 完成 HP0 全矩阵复测和独立架构/API 审查后，再决定下一版本号；不创建空版本或提前
  承诺 v1。

## 4. 性能、测试与推广规则

- 普通 PR：Linux/Windows correctness、focused repeat、ASan/UBSan、TSan、fuzz、
  capacity、API/scope guards，并保留现有宽松灾难性回退门。
- 固定性能实验室：每个 revision/scenario 先 1 次不计入 warmup，再执行至少 10 个
  交错正式样本；保存原始样本、机器信息、命令、commit、二进制 hash 和
  `perf stat`/等价数据。
- 默认路径推广必须同时满足：

  - 预登记主指标中位数改善至少 5%，且 95% bootstrap 置信区间不跨越无改善；
  - 非目标吞吐、P99/P999、CPU、RSS、恢复和关闭指标回退不超过 3%；
  - 结构成本合同、生命周期、背压、owner、generation 和零残留全部通过。

- 未达到 5% 但 correctness 完整的实现标记 `KEEP-EXPERIMENTAL`；出现生命周期或核算
  失败则 `REJECT`；缺少硬件/场景证据则 `DEFER`，不得降低负载后宣称通过。
- 最终矩阵覆盖 32–16384 字节、1K/10K/100K 连接、0.1%–100% 活跃度、
  50%–overload，以及 ping-pong、单向吞吐、burst、churn、广播、慢客户端、fixed
  Tick、queued logic 和 shutdown/recovery。

## 5. 固定假设

- 高性能优先于 v1 发布准备；HP8 之前不恢复 v1 发布线。
- callback API 永久保留；协程是后置、条件性、非安装实验。
- 固定拓扑优先 SPSC；MPSC 只在 SPSC matrix 的实测内存或拓扑证据不合格后另立
  切片，MPMC 不作为默认方案。
- Linux 默认仍为 epoll，Windows 默认仍为 IOCP，io_uring 保持显式 opt-in。
- “高性能”以满足 P99/P999 SLO 时的持续负载、每消息成本和饱和恢复为准，不以单一
  Echo 峰值定义。

## 6. 状态与完成定义

任务状态统一为：

```text
planned -> contract-ready -> implemented -> verified -> integrated
```

每个 HP 切片必须明确输出 `INTEGRATE`、`KEEP-EXPERIMENTAL`、`REJECT`、`DEFER` 或
`SKIPPED-BY-EVIDENCE`，不得以“后续再决定”结束。

`integrated` 必须满足：

- 对应 intent 为 active，且 rules、实现和文档一致；
- owner、ownership、callback re-entry、cross-thread marshal 和 shutdown 均有明确答案；
- 具体 contract 在目标行为回归时会失败；
- focused、全量、双平台、sanitizer、API/scope guard 达到阶段门；
- 性能结论绑定同一精确提交并满足本计划的 5%/3% 推广规则；
- Accepted 工作、队列、operation、segment、credit、pool 和 retained storage 全部形成
  可观察终局并收敛至零残留；
- README、roadmap、migration status、assessment、plan 和 evidence ledger 描述同一
  当前事实。

## 7. 当前立即执行

> **当前前沿为 HP0：成本账本与固定性能实验室。** schema、默认关闭且非安装的
> benchmark suite、validator/CI guard 已实现；下一步只允许选择干净精确提交，在固定
> 原生 Linux/epoll 与 Windows/IOCP runner 上补齐 observer 成本，并各执行 1 次不计入
> warmup + 至少 10 次正式样本。HP0 关闭前不得开始 HP1 Core 实现，也不得提前增加
> PacketView、mailbox、稳定发送 API、coroutine target 或高级 io_uring capability。
