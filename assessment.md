# game-net-core 推荐开发路线

> 基线日期：2026-07-26
> 当前执行检查点：2026-07-27
> 主线基线：`main@2b1be4343f7c478eb40542451f30aad8ca474003`
> 候选分支：`codex/phase6-production-candidate@b3443182d0606792df44a12bcb08927e767bc060`
> 执行分支：`codex/assessment-roadmap-execution`

## 0. 执行进度

本路线已经进入实施，而不是仅保留为建议。当前本地执行检查点的
CTest 清单为 **102 项（8 unit、85 contract、9 integration）**：

- M0 Phase 6 草案已快进整合到执行分支并完成代码审计，M0 的本地实现
  与门禁已经闭环；stable tag 仍受独立 maintainer 评审、许可决策和最终
  SHA 重跑约束；
- MetricsExporter/Core、Logic、Broadcast recorder 已明确降为
  provisional，当前字符串、哈希、全局 mutex 模型不再被描述为稳定热路径；
- Core benchmark 已升级为 `gamenet.core_benchmark.v2`，echo/slow-client
  均使用 `trySend()`，并由独立 validator 核验 requested/accepted/rejected、
  low/high/hard/input limits、pending peak、pause/resume 与 recovery；
- public API manifest 已升级为 v2，保留
  `v0.2.0-phase4-preview@7668d6b...` 历史 snapshot，并在 CI 生成确定性
  target/header/category/fingerprint diff；
- CMake 已收紧为 Linux/Windows、static-only；unsupported OS、
  `BUILD_SHARED_LIBS=ON`、未实现 TLS/experimental 开关均 fail closed；
- 当前 all-rights-reserved/no-license-grant 已记录为外部采用阻断项；
- `b344318` 的 24 小时（73,617 cycles）与 72 小时（220,851 cycles）
  Linux/epoll 结果已登记为基础设施历史证据；任何后续 runtime 修改仍需在
  最终冻结 SHA 重跑；
- M1 已完成第一组底层原语：typed `PostResult`、固定 control-source slot、
  pending bit 合并、quit admission 线性化和 final drain；
- 跨线程普通任务饱和时，高水位与写完成通知会按合同丢弃并计数，连接状态
  转换先于可选通知；Windows IOCP 同步提交失败会保留原始错误并立即收敛；
- Connector/TcpClient 已增加请求代际、owner-thread 合同与 typed 控制结果，
  Connector 的当前回调 Channel 使用 source-private 退役槽，不再依赖普通
  pending queue 延长生命周期；
- Connector 成功观察者同步 `restart()` 或 `stop() → start()` 时，旧代仅
  通过 compare-exchange 收口自身状态，不会覆盖新代 `kConnecting`；
  TcpClient 在旧连接已经 disconnecting 时接受的显式 connect 会独立保留，
  即使 automatic retry 关闭也会在旧连接移除后启动；
- EventLoop final-drain 完成与 control Shutdown 会一起发布，重复 `quit()`
  不会复活已关闭 executor 的 owner 身份；Windows Connector 回调拒绝 fd
  时也不会留下可污染 SOCKET 数值复用的 IOCP association 记录；
- TcpClient 现在把“已经发出 disconnected 回调、尚待内部 remove”的旧连接
  识别为已结束生命周期；即使 retry=false，回调内返回 Accepted 的显式
  `tryConnect()` 也会生成并保留新请求，不再被旧 request id 静默合并；
- Windows Connector → TcpConnection 接管已改为可回滚事务：
  provisional `connection_`、request bookkeeping、IOCP association 与替代
  Channel 注册任一步失败都会清理并发布 terminal failure；测试可分别注入
  association preserve 和 replacement registration 的分配失败，并证明随后
  的重入显式连接可以成功；
- Channel/EventLoop/Poller 已完成 active-batch 代际与精确身份校验：
  批次中被移除/销毁的 Channel 不会继续分发旧 readiness，同一 fd 的陈旧
  remove 不会误删新注册；
- 上一提交检查点的工作树已重新配置、全量构建并通过 Debug/Release CTest
  **94/94**；当时的测试标签为 unit 8、contract 78、integration 8、
  threading 67、lifecycle 73。此前 active-batch Debug/Release 各 50 次
  重复亦通过；27 个 Python guard 文件所覆盖的仓库逻辑门禁、public API
  v2 manifest/diff 和 CTest 标签/数量核验均通过；
- `GAMENET_BUILD_TESTING=OFF` 的纯运行时 Release 已重新构建；本轮条件
  编译的 IOCP/Logic internal test hooks 在工程、产物、安装库和安装头中
  均为零，隔离安装包的外部 consumer 已通过 Release 运行与 CTest 1/1；
- `Logger::resetForTesting()` 仍是既有公开 Logger 合同，并非本轮条件编译
  hook；后续独立 API 人工评审必须明确决定是否继续保留该公开测试接口；
- benchmark v2 已在最终两项 P1 修复的直接前一 runtime revision 上通过
  echo、2×8 MiB accepted slow-client、2×32 MiB overload 三类独立
  validator；由于 P1 修复再次改变 runtime，严格的最终 SHA 性能证据仍需
  下次冻结候选时重跑，不能把该结果冒充为最终 artifact 的性能证明；
- M1 的四个剩余切片已按依赖顺序在当前工作树完成：EventLoop 动态
  lifecycle hub、`Running → Quiescing → FinalDraining → Shutdown` 与
  IOCP completion 静默、TcpConnection 显式 close/drain、TcpServer
  worker aggregate/BaseReleased/ack/join，以及结构化 close reason 和
  独立 `TcpClientControl`；
- 当前工作树的 Windows MSVC Debug 与 Release 已分别全量通过
  **102/102** CTest；21 个本轮相关 Python 静态合同通过，public API v2
  manifest/diff 通过，隔离安装包的 Release consumer 通过 CTest 1/1。

M1 的本地 runtime 实现与直接合同现已闭环。尚未完成的是冻结候选 SHA
之后的跨平台、sanitizer/TSan、性能与 24/72 小时耐久证据重跑，以及独立
maintainer 的 API、性能和发布工具人工评审。因此当前仍不得创建 stable
tag，也不得把历史 `b344318` 耐久记录当作本轮 runtime 的最终证明。

独立 maintainer 的 API、性能和发布工具人工评审仍是稳定候选门禁；当前
执行不得据此创建 stable tag。

### 0.1 上一收敛检查点与本轮执行结果

上一轮按用户要求在 `codex/assessment-roadmap-execution` 分支收敛、提交
并推送了当时的全部本地修改。该远端分支尖端是本轮工作的代码基线；没有
从未包含这些 M0/M1 原语的旧 `main` 或 Phase 6 候选重新开始。

已冻结到本检查点的范围：

1. M0 的平台、构建、许可、API manifest v2、CI/证据语义和 benchmark v2；
2. M1 的 typed admission、固定 control source、final drain、active-batch
   失效、精确 Channel 身份、IOCP 同步错误传播和可选通知降级；
3. Connector/TcpClient 请求代际、回调重入、显式 reconnect、事务式 IOCP
   fd 接管，以及对应的 deterministic contract tests。

该检查点留下的实施顺序不可倒置。本轮已严格按原顺序完成：

1. 先更新 `event_loop`、`tcp_connection`、`tcp_server` intents 与三份 rules；
2. 先写 lifecycle committed-notify、dirty-set/detach-generation、queue
   saturation、IOCP cancel/quit completion-drain 的失败契约；
3. 实现 loop 级动态节点 hub：跨线程 signal 零分配、generation 防 ABA、
   owner-thread attach/detach、预算 drain 与自重排；
4. 再把 EventLoop 状态推进为
   `Running → Quiescing → FinalDraining → Shutdown`，在 Windows 持续 poll
   直到 IOCP completion 与 lifecycle hub 同时静默；
5. 最后接入 TcpConnection、TcpServer 三方/聚合停服握手、结构化 close
   reason 和析构后安全的 `TcpClientControl`，并补齐 public API manifest、
   安装消费和文档证据。

仍然明确未完成、不得误报为已验证的范围：

1. 当前 runtime 修改后的 24h/72h endurance 尚未重跑；
2. 当前 runtime 的 Linux、ASan/UBSan、TSan、repeat-50 和跨平台性能证据
   尚未绑定到冻结候选 SHA；
3. 独立 maintainer 的 API、性能和发布工具人工评审尚未完成；
4. stable tag 仍禁止。

上一提交检查点的最终复验记录（历史 94 项清单）：

- Debug：重新配置和全目标构建成功，CTest 94/94，0 失败；
- Release：重新配置和全目标构建成功，CTest 94/94，0 失败；
- Connector 定向合同覆盖 callback restart/stop-start、显式 reconnect、
  association preserve fault 和 replacement registration fault，单次通过；
- public API v2 manifest、immutable git baseline 和 compatibility diff
  验证通过，无待裁决的 compatibility-line 阻断；
- testing-off Windows/MSVC Release 全量构建成功，构建树 0 tests；新增及
  既有 IOCP internal hooks 在工程命令、48 个二进制产物、安装库和 52 个
  安装头/CMake 文件中均为零；
- 构建与安装后的 `gamenet_core.lib` SHA-256 均为
  `A2EE360888AE856E05DA8F3ECA5DD0A077E5E214C691D055A3553E0F6FA6C0CC`；
- 隔离安装包的外部 consumer 配置、Release 构建、直接运行和 CTest 1/1
  均通过，其 SHA-256 为
  `3449C593E790E1243DB38738B06811B68F787F34176FC2C31C1E7365C92F1BFF`。

当前未提交工作树的本地复验记录：

- Windows MSVC Debug：全目标构建成功，CTest 102/102，0 失败；
- Windows MSVC Release：全目标构建成功，CTest 102/102，0 失败；
- 21 个本轮相关 Python 静态合同、public API manifest 和确定性
  compatibility diff 均通过；
- `cmake --install` 后的隔离 Release consumer 可配置、构建并通过
  CTest 1/1；`TcpClientControl.h` 与 `TcpConnectionClose.h` 均出现在
  安装树；
- 以上仅是本地实现闭环证据，不替代冻结 SHA 所需的远端、sanitizer、
  性能和 24/72 小时耐久证据。

## 1. 目标与当前判断

game-net-core 的目标是成为高性能、通用、可验证的游戏服务器网络底座。这里的“通用”不等于在 Core 中堆叠所有协议，而是要求：

- Reactor/TCP 核心在高负载、队列饱和、慢客户端、断连风暴和停服期间仍能确定性收敛；
- 线程归属、所有权、回调重入和跨线程投递是可审计的公开合同；
- 上层 Packet、Session、Logic、Broadcast 能组合成正式 Gateway，而不是只能运行的示例；
- 性能由可重复的容量模型、延迟分位数、内存和过载恢复证据证明；
- TLS、UDP、KCP 等扩展建立在统一的 ingress、结果模型和背压策略之上；
- 发布版本具有精确 SHA、兼容性边界、测试证据和可追溯资产。

当前项目已经是一套较成熟的 Reactor/TCP Preview，而不是原型。Core 的正常路径、跨平台功能、线程/生命周期测试和 CI 证据较强；主要差距集中在饱和终态、Windows IOCP 数据路径、大规模公平性、Phase 4/5 上下层语义闭环，以及正式 Gateway 和安全传输。

下一阶段的核心原则是：

> 先证明系统在最坏状态下必然收敛，再优化吞吐；先完成 TCP 全链路，再扩展 TLS、UDP 和 KCP。

## 2. 执行原则

### 2.1 严格遵循 intent-driven 流程

每个核心改动必须按以下顺序推进：

1. 更新或新增 `intents/`；
2. 写明不变量；
3. 写明 owner loop/thread；
4. 写明所有权和释放者；
5. 写明允许重入的 callback；
6. 写明跨线程操作及 marshal 方式；
7. 写明过载、关闭和失败终态；
8. 先添加 contract/failure/threading tests；
9. 最后实现；
10. 更新证据账本和发布文档。

每个 Core PR 的描述必须回答：

- 哪个 loop/thread 拥有状态？
- 谁持有对象，谁负责释放？
- 哪些 callback 可以重入？
- 哪些 API 可以跨线程调用？
- 队列满、owner 停止、对象销毁时返回什么？
- 哪个具体测试验证每条合同？
- 对 public/source contract 有什么兼容性影响？
- 失败时如何回滚，哪些证据 SHA 会失效？

### 2.2 保持范围克制

在饱和终态、IOCP 内存模型和正式 Gateway 未闭环前：

- 不新增 HTTP、WebSocket、RPC；
- 不将 coroutine 引入 Core 所有权模型；
- 不同时启动 TLS、UDP、KCP；
- 不把账号、房间、AOI、对象序列化等业务逻辑放入网络底座；
- 不把当前 Pipeline 示例直接升级为大而全框架。

### 2.3 小 PR、单一风险、精确证据

- 每个 PR 只解决一组紧密相关的不变量；
- 禁止把核心生命周期修复、性能重构和新协议混在一个 PR；
- 运行时代码发生变化后，旧 SHA 的耐久与性能证据只能作为历史参考；
- 发布证据必须属于最终候选 SHA；
- API manifest 是变更提醒器，不是兼容性证明，仍需真实 consumer 和 API diff。

## 3. 总体里程碑与依赖

```text
M0  Phase 6 发布基础设施审计
 │
 ├── M1  饱和终态与核心生命周期闭环
 │    │
 │    └── M2  Phase 4 / Phase 5 上层语义对齐
 │          │
 │          ├── M3  IOCP v2、内存与公平性
 │          │
 │          └── M4  正式 Packet/Gateway/Pipeline
 │                  │
 │                  └── M5  端到端背压、指标与生产发布门禁
 │                         │
 │                         └── M6  TLS 与 Gateway Security
 │                                │
 │                                └── M7  UDP experimental
 │                                       │
 │                                       └── M8  KCP preview
```

M3 和 M4 可以在 M1、M2 完成后并行。M3 完成后可以执行 M5-A，收口 Core/Phase 4 的 v0.3 候选；M4 完成后再执行 M5-B，汇总正式 Gateway 的全链路证据。M6 依赖正式 Gateway 生命周期，M7 依赖 transport-neutral ingress，M8 严格依赖 UDP。

## 4. M0：Phase 6 发布基础设施审计

### 4.1 目标

保留 Phase 6 已完成的兼容性、指标、性能回归、故障注入和耐久基础设施，但不把当前 Draft 直接当作稳定版本发布。

### 4.2 任务

1. 审计 `codex/phase6-production-candidate` 的 8 个提交。
2. 更新 PR #7 中过期的候选 SHA、CI、24 小时和 72 小时证据。
3. 将 MetricsExporter 和上层 recorder 标记为 provisional，避免在热路径设计稳定前冻结 API。
4. 修正 Core benchmark 的语义漂移：
   - 使用 `trySend()`；
   - 记录 requested、accepted、rejected bytes；
   - 记录 hard limit、pause/resume、peak pending output；
   - 不再输出 `high_water_notification_only`；
   - benchmark schema 升级为 v2。
5. 检查 public API manifest：
   - stable Core、provisional upper layer、platform internal 必须分开；
   - internal poller/platform header 不应作为推荐消费 API；
   - manifest 更新必须输出与上一版本的结构化差异。
6. 明确平台支持等级：
   - 推荐 Linux/epoll 作为首要生产性能平台；
   - Windows/IOCP 在 M3 前保持功能与正确性支持；
   - 如果 Windows 必须同为 Tier 1，M3 必须成为 0.3 候选的发布阻断项。
7. 明确当前只支持 Linux 和 Windows；macOS/BSD 配置必须显式报 unsupported，不能落入 Linux epoll 分支。
8. 对尚未实现的 `GAMENET_ENABLE_TLS`、`GAMENET_ENABLE_EXPERIMENTAL` 给出显式 configure error 或清楚的占位说明，不能成为无效果开关。
9. 明确 static/shared 策略；在没有导出宏和 shared-build CI 前，发布文档应声明 static-first。
10. 明确许可证策略。当前 `LICENSE` 未授予外部使用许可；若计划供外部项目采用，必须在发布前解决。

### 4.3 验收门禁

- PR 描述、migration status、roadmap 与真实 SHA/运行状态一致；
- Metrics API 未被错误声明为稳定热路径接口；
- benchmark v2 能检测发送拒绝，不能把被拒绝字节计为成功；
- API manifest 能区分 public、provisional、internal；
- 至少一次独立人工 API、性能和发布工具审查；
- M0 可以合并为发布基础设施，但不得因此直接创建 stable tag。

## 5. M1：饱和终态与核心生命周期闭环

M1 是所有后续开发的最高优先级门禁。

### 5.1 EventLoop 控制面投递模型

#### 问题

普通任务和 teardown/control work 共用有限 pending queue。队列满时 `queueInLoop()` 抛异常，Server shutdown、remove、force-close、认证超时和 Connector 清理可能无法完成。

#### 设计要求

新增或修订 EventLoop overload intent，定义两类任务：

- user/data work：允许拒绝，返回明确结果；
- internal control work：数量必须可证明有界，并保证最终被执行或被同步合并。

不建议简单增加一个无界“紧急队列”。推荐设计：

- internal control lane 不向普通调用者开放；
- shutdown/remove/cancel 使用幂等状态位或 generation 合并重复请求；
- TcpServer 按 worker 投递一个聚合 shutdown task，而不是每连接一个 task；
- control task 的最大数量由 subsystem/worker 数决定，而不是连接数决定；
- owner loop 已停止时返回 `OwnerUnavailable`，调用方必须执行定义好的 fallback；
- 同线程 teardown 优先内联执行；
- 可选通知、指标和用户 callback 排队失败不得阻断状态转换。

#### API 方向

统一结果枚举，避免 `void`、异常和 `bool` 混用：

```cpp
enum class PostResult {
    Accepted,
    QueueFull,
    OwnerUnavailable,
    Shutdown,
};
```

公开 data-plane API 使用 non-throwing admission；内部 control API 必须说明为什么不会丢失。现有兼容 API 可以保留一段迁移期，但生产代码不得继续忽略结果。

#### 必测场景

- normal queue 已满时 close 仍收敛；
- normal+reserve 已满时 graceful stop future 仍完成；
- 多 worker、每 worker 超过普通队列容量的连接关闭；
- authentication timeout 在队列满时确实关闭连接；
- remove callback 无法回流 base loop 时连接表与 admission 计数不残留；
- Connector retry/cancel/Channel release 在队列满时不泄漏；
- loop quit 与跨线程 control post 竞争；
- callback 抛异常与队列饱和同时发生；
- ASan/UBSan、TSan、repeat-50。

### 5.2 TcpConnection 状态转换与可选通知解耦

#### 任务

- 先完成 output buffer、backpressure 和 write-interest 状态转换；
- 再异步投递 high-water/write-complete 通知；
- 通知投递失败只增加 dropped-notification 指标，不得使连接停写；
- Windows read resume 不得依赖可能被拒绝的普通任务；
- `send()` 逐步标记为兼容接口，生产路径统一使用 `trySend()`；
- 增加结构化 close/error reason：
  - PeerEof；
  - Reset；
  - ConnectTimeout；
  - InputLimit；
  - OutputOverload；
  - AdmissionPolicy；
  - GracefulShutdown；
  - ForcedShutdown；
  - CallbackFailure；
  - InternalError。

#### 验收门禁

- 所有 connection 状态转换都有单次终态保证；
- high-water callback 投递失败不会阻止 EPOLLOUT/WSASend；
- pending output reservation 在所有异常和关闭路径归零；
- close reason 能传递到 TcpServer/TcpClient 和上层指标。

### 5.3 Connector 线程合同

#### 推荐决策

Connector 保持 owner-loop-only，由 TcpClient 提供线程安全 facade。这与当前 intent 的窄职责最一致。

#### 任务

- `start/stop/restart/setRetry...` 增加 owner-loop assert；
- 或将跨线程入口全部移入 lambda 后再修改 loop-owned 状态；
- `state()` 若需跨线程读取，改为明确 atomic snapshot；
- `restart()` 恢复配置的 initial retry delay，而不是固定 500 ms；
- TcpClient 暴露 ConnectorOptions、connect timeout、自定义 backoff；
- TcpClient 增加显式 terminal failure callback/result；
- 使用 request generation 丢弃过期 connect/stop/retry。

#### 必测场景

- 直接跨线程 start/stop/restart；
- stop 与 retry timer 同时发生；
- stop 与 Windows ConnectEx completion 同时发生；
- 自定义初始退避在 restart 后保持；
- owner loop 销毁后 facade 调用；
- TSan 合同。

### 5.4 active Channel 批次生命周期

#### 问题

Poller 返回裸 `Channel*` 集合。一个 Channel callback 可能移除并销毁同一 active batch 中尚未 dispatch 的另一个 Channel。

#### 推荐方案

二选一并写入 intent：

1. dispatch epoch + deferred reclamation；或
2. EventLoop 跟踪 active batch membership，禁止在本轮结束前销毁 batch 中的 Channel。

仅依靠 Channel 自身的 `eventHandling_` 不足以保护“尚未开始回调”的 Channel。

#### 必测场景

- 两个 Channel 同时 ready，第一个 callback 移除并请求销毁第二个；
- callback 移除自身；
- callback 销毁 tied owner；
- close/error/read/write 多事件组合；
- epoll 与 IOCP 都验证延迟 completion。

### 5.5 IOCP 同步失败正确性

- WSARecv/WSASend 同步失败必须立即进入统一 error/close 路径；
- 不得只设置 revents 后等待一个不会到来的 completion；
- error、cancel completion 和正常 completion 必须只收敛一次；
- 增加 WSAENOBUFS、WSAECONNRESET、ERROR_OPERATION_ABORTED 故障注入。

### 5.6 TcpServer idle-timeout 合同漂移

当前 active `tcp_server.intent.md` 声明了可选 idle timeout，但公开 API 和实现只有 unauthenticated timeout，没有通用连接 idle policy。

分两步处理：

1. M1 立即修正文档和 active intent，不得继续把未实现能力写成已完成合同；
2. M3 在 deadline bucket/时间轮具备后实现可扩展 idle timeout。

正式设计必须明确：

- read-idle、write-idle、all-idle 的语义，首版推荐只提供 read-idle；
- activity timestamp 由 connection owner loop 更新；
- timeout close 回到正常 close/remove 路径；
- callback、timer 和业务读取竞争时如何判断 generation；
- idle policy 与 heartbeat、unauthenticated timeout 的优先级；
- 是否允许应用显式刷新 activity；
- 大规模连接不能默认创建一个高分配成本 timer 对象。

计划测试：

- quiet connection 到期关闭；
- 持续入站数据刷新 deadline；
- idle timeout 与 peer close/force close/graceful stop 竞争；
- 旧 generation timer 不关闭新连接；
- 10k/100k deadline storm 的 loop lag 和内存。

### 5.7 M1 完成定义

- 没有生命周期控制路径依赖“可能抛出且无 fallback”的投递；
- 队列饱和测试覆盖 Core 与多 worker server，不只覆盖 EventLoop 本身；
- Connector 没有普通字段的跨线程读写；
- active batch destruction 有明确实现保护和回归测试；
- 所有关闭路径产生结构化 reason；
- Linux/Windows Debug、Release、sanitizer、TSan 和 repeat 证据全绿。

## 6. M2：Phase 4 / Phase 5 上层语义对齐

### 6.1 统一异步结果

将以下 `void` 或被忽略的 `bool` 改为显式结果：

- SessionManager postAuthenticate/postOffline/postHeartbeat；
- Pipeline I/O → management handoff；
- management → logic handoff；
- logic output → management/owner loop；
- endpoint close/send；
- Broadcast task dispatch。

结果至少区分：

- Accepted；
- QueueFull；
- OwnerUnavailable；
- Shutdown；
- EndpointClosed；
- EndpointOverloaded；
- PolicyRejected。

每个调用点必须决定：

- 重试；
- 丢弃；
- 关闭连接；
- 降级；
- 或记录指标。

不得只记录日志后继续保持模糊状态。

### 6.2 修复 Pipeline 黑洞连接

- framer continuation admission 失败必须关闭 endpoint；
- `Authenticating` 状态的 pending frame 必须有 count/bytes 双上限；
- 认证投递失败必须回退到 Closing，而不是永久停留；
- output send 被拒绝时必须执行明确策略；
- 连接进入 closing 后停止接收新业务命令。

### 6.3 Session binding generation

为每次认证/rebind 分配 generation：

```text
(sessionId, transportId, generation)
```

- Logic command 携带 generation；
- 在执行业务 handler 前验证当前 binding；
- superseded connection 的命令不得产生业务副作用；
- stale disconnect 只能影响对应 generation；
- duplicate replace 和 reject-new 都必须有真实 Pipeline 集成测试。

### 6.4 接入 Phase 5 Core 能力

正式 Pipeline 必须使用：

- `TcpServer::setAdmissionOptions()`；
- `TcpServer::tryMarkConnectionAuthenticated()`；
- connection backpressure options；
- callback exception policy；
- `TcpServer::stopGracefully()` future。

认证成功后必须同步更新 Core admission 状态。停服顺序建议：

1. 停止新连接和新认证；
2. 停止接收新业务命令；
3. 等待已接受 management/logic work；
4. 停止 Broadcast 新计划；
5. 排空允许完成的输出；
6. 超时后 force close；
7. 等待所有 owner-loop cleanup；
8. 最后销毁 Session、Logic、Server 和 loops。

### 6.5 Broadcast 结果与全局预算

- 区分 dispatch queue full、owner unavailable、endpoint overloaded、closed；
- DispatchSummary 返回 scheduled、accepted、dropped 和 reason counts；
- 增加 per-owner outstanding task/bytes；
- 增加全局 broadcast outstanding bytes；
- 连续多个合法 plan 也不能绕过总预算；
- 高/低优先级采用明确 shedding 策略；
- shared payload 继续保持跨 loop value ownership。

### 6.6 Session 类型安全

- PlayerSession 构造和 mutation 收到 SessionManager private/friend；
- Broadcast 消费 immutable `BroadcastTarget` 或 `SessionSnapshot`；
- 外部不得通过 mutable alias 跨线程修改 session；
- heartbeat/idle expiry 接入 Pipeline；
- 当前 O(N) expiry 先建立规模基准，再决定 min-heap、bucket 或时间轮。

### 6.7 M2 测试矩阵

- EventLoop queue 饱和下 auth/offline/heartbeat；
- AUTH 与 command 同批、认证延迟、认证失败、断连早于认证完成；
- duplicate replace 旧连接继续发 command；
- stale output、stale disconnect、session generation rollover；
- management/logic/endpoint 三个物理 loop；
- shutdown 时存在 pending auth、pending command、pending output、pending broadcast；
- Broadcast 每种 dropped reason；
- heartbeat/idle timeout 的完整 TCP 路径；
- callback re-entry 和 callback 中销毁；
- ASan/UBSan、TSan、repeat-50、真实 TCP integration。

## 7. M3：IOCP v2、内存治理与调度公平性

### 7.1 先定义容量模型

性能优化前先固定测试硬件、编译器、CPU governor、网络拓扑和业务 profile。每个结果必须记录：

- commit SHA；
- OS、kernel、compiler、build type；
- CPU、内存、NUMA 和网卡；
- connection/message/payload 数量；
- throughput；
- P50/P95/P99/P999；
- CPU/core；
- RSS 与 bytes/connection；
- allocation rate；
- pending queue/bytes high-water；
- accepted/rejected/dropped；
- overload 后恢复时间；
- graceful shutdown 完成时间。

建议建立以下 promotion profiles：

| Profile | 首阶段规模 | 目标规模 | 关键指标 |
|---|---:|---:|---|
| Idle connections | 10k | 100k | RSS/conn、CPU idle、stop time |
| Small echo | 64/256/1024 B | 多连接多 worker | msg/s、MiB/s、P99/P999 |
| Connection churn | 1k/s | 按硬件提升 | accept/s、close/s、worker skew |
| Slow reader | 1k connections | 混合正常客户端 | RSS、rejected bytes、恢复时间 |
| Timer storm | 100k timers | 同刻/分桶 | loop lag、P99 timer delay |
| Queue saturation | 所有队列上限 | 多层同时饱和 | 终态、drop reason、恢复 |
| Broadcast TCP | 1k recipients | 10k+ recipients | fanout P99、RSS、slow-client |
| Full Pipeline | auth+command+response | churn+slow+广播混合 | end-to-end P99、queue lag |

阈值必须先作为趋势基线，再在专用 runner 稳定后变成回归预算。不得把跨机器绝对值当作可靠比较。

### 7.2 IOCP v2 数据路径

#### 任务

1. `GetQueuedCompletionStatusEx` 批量 drain；
2. 每轮 completion count/time budget；
3. wakeup coalescing，避免每个跨线程任务一个 completion；
4. 可配置 AcceptEx pre-post pool；
5. read buffer 改为 slab/pool 或按需小块；
6. idle connection 不再常驻 64 KiB；
7. output 改为稳定 segment/chunk ownership；
8. WSASend 直接引用稳定 pending segment，避免完整镜像；
9. partial completion 只移动 offset，不复制剩余全部数据；
10. 每次提交显式限制在 `ULONG`；
11. cancel/completion lifetime 继续由明确 owner 保持。

#### 验收门禁

- 10k idle connection RSS 显著低于当前约 71 KiB/连接基线；
- slow-client 不再出现 output buffer 与 writeStorage 双份峰值；
- batch completion 在吞吐与 CPU 上有可重复收益；
- cancel、partial write、同步失败和延迟 completion 全部通过；
- Windows Debug/Release 与 sanitizer 证据齐全。

### 7.3 Linux/通用公平性

- accept loop 增加 count/time budget；
- active Channel dispatch 增加 count/time budget；
- expired timer drain 增加 count/time budget；
- 未完成 ready work 使用 zero-timeout 或 wakeup 延续；
- 定义 repeating timer 的 fixed-delay/fixed-rate 和最大 catch-up；
- 大规模连接 deadline 使用分层时间轮或 bucket；
- EventLoopThreadPool 增加可插拔 selector：
  - round-robin；
  - least-connections；
  - queue-lag；
  - consistent-hash；
- 在 connect/disconnect storm 证明 base-loop 瓶颈后，再引入 `SO_REUSEPORT` per-worker accept。

### 7.4 全局内存治理

在现有 per-connection hard limit 之上增加：

- per-loop pending output bytes；
- server/global pending output bytes；
- broadcast outstanding bytes；
- PacketFramer retained capacity；
- Buffer hysteretic trim；
- pool/slab 最大保留量；
- 超预算策略和结构化指标。

全局 quota 不应通过每次发送争用一个全局 mutex；建议使用 per-loop 计数和低频聚合。

### 7.5 M3 完成定义

- Linux/Windows 都有同 runner 性能矩阵；
- IOCP 不再单 completion drain；
- idle connection 内存不由固定 64 KiB 缓冲主导；
- 慢客户端输出不重复持有完整 payload；
- accept、timer、active channels、functors 均有公平预算；
- overload 后延迟和内存能恢复到稳定区间；
- 10k profile 成为每个性能候选的强制门禁，100k 作为专用容量门禁。

## 8. M4：正式 Packet 与 Gateway/Pipeline

### 8.1 Rich Game Packet

在 PacketFramer 之上增加窄的网络 envelope：

```text
version
header length
message id
request id
flags
priority
payload length
payload
```

不在底座中定义 protobuf、JSON、FlatBuffers 或业务对象。序列化由外部 adapter 负责。

#### 合同

- 固定字节序；
- version negotiation/rejection；
- unknown flags；
- header/payload 独立上限；
- request/response correlation；
- priority 只表达调度意图，不绕过硬限制；
- 畸形 header fail-closed；
- parser 有 frame/byte/time budget；
- 真 libFuzzer、最小 corpus、跨分块 differential test。

### 8.2 Transport-neutral ingress

当前 TransportEndpoint 主要抽象 output。UDP 前需增加窄 ingress/event sink：

- Connected；
- Bytes/Datagram received；
- Writable/Overloaded；
- Closed with reason；
- transport/session identity；
- owner executor。

不要创建包含 TCP、TLS、UDP、KCP 所有细节的 mega-interface。transport-specific 信息通过受控扩展或 metadata value 暴露。

### 8.3 正式 Gateway 组件

从示例中提取可安装但保持窄职责的组件：

- IngressAdapter；
- PacketCodec；
- Admission/AuthCoordinator；
- SessionBindingManager；
- CommandDispatcher；
- OutputDispatcher；
- ShutdownCoordinator。

必须是可注入接口：

- async auth validator；
- command handler/sink；
- output encoder；
- clock/timer；
- metrics sink；
- overload policy。

Gateway 不拥有账号数据库、世界状态、房间或 AOI。

### 8.4 Auth 与 cancellation

- auth request 带 connection generation；
- validator completion 带 cancellation/lifetime token；
- auth timeout、disconnect、replacement 后的旧 completion 必须失效；
- pending auth count/bytes/rate 有界；
- 成功后调用 Core authenticated marker；
- 失败返回结构化 reason；
- 不在 owner loop 执行阻塞认证。

### 8.5 Logic 调度与公平性

- 增加 oldest-command-lag soft/hard limit；
- per-session pending count/bytes；
- 单 session 每 tick 最大 drain；
- priority 参与 admission/shedding，但不得饿死正常队列；
- handler 执行时间可观测；
- 慢 handler 与异常 handler 隔离；
- Windows timer cadence 经过专门验证后，再决定是否替换 mutex+deque 为分片 MPSC。

先修策略与公平性，再以 allocation/lock profiling 驱动无锁结构；不得仅为了“高性能”盲目引入 lock-free。

### 8.6 M4 完成定义

- Pipeline 从 example 变成窄的 provisional library；
- example 只负责演示配置和业务 handler；
- auth、session generation、logic、broadcast、output、graceful stop 全链路闭环；
- ingress 对未来 TLS/UDP 可复用；
- packet codec 有 fuzz 与版本合同；
- 真实 TCP full-pipeline benchmark 和 soak 进入 CI/专用 runner。

## 9. M5：端到端背压、可观测性与生产发布门禁

M5 分成两个 promotion point：

- **M5-A Core/Phase 4 candidate gate**：在 M3 完成后即可执行，用于收口 `v0.3.0-production-candidate`，不依赖正式 Gateway；
- **M5-B Gateway end-to-end gate**：在 M4 完成后执行，增加完整 auth/session/logic/broadcast/TCP 链路证据。

M5-A 可以先发布候选，但不能把尚未完成的 Gateway、安全或多传输能力写入版本承诺。M5-B 通过后再发布 Gateway Preview。

### 9.1 统一背压状态

构建从 socket 到业务的完整决策链：

```text
socket input
→ framing budget
→ pending-auth budget
→ session quota
→ logic queue/lag
→ output quota
→ broadcast quota
→ connection/server memory quota
```

每层必须提供：

- 当前容量；
- hard/soft threshold；
- admission result；
- action；
- reason；
- metric；
- 恢复条件。

禁止某层接受工作后，在下一层静默丢弃且不反馈。

### 9.2 Metrics 热路径重构

Phase 6 的 InMemoryMetricsExporter 适合作为功能原型，不适合作为最终热路径。

#### 任务

- 预注册 `MetricId`；
- 固定 label schema，禁止每事件动态拼接字符串；
- per-loop/thread-local counter；
- histogram 使用明确 bucket 或可替换 recorder；
- snapshot 时聚合；
- counter 与 histogram 名称/type 冲突检测；
- cumulative counter 不作为 histogram sample；
- endpoint 级高频事件支持采样或聚合；
- exporter failure 不影响网络行为。

#### 性能门禁

- metrics disabled；
- metrics enabled but no scrape；
- metrics enabled + periodic snapshot；
- Broadcast 高 fanout metrics；
- EventLoop 高频 wakeup metrics；
- 记录 CPU、allocation、lock contention 和 P99 差异。

### 9.3 生产配置

提供分层配置并在 start 前验证：

- EventLoop budgets；
- thread count/selector/affinity；
- connection input/output limits；
- per-loop/global memory quota；
- server admission；
- auth timeout/rate；
- heartbeat/idle；
- Logic queue/lag/fairness；
- Broadcast budgets；
- socket options；
- shutdown deadline；
- metrics。

非法配置必须在启动前失败，不能运行中静默修正。

### 9.4 CI 与发布强化

正式候选至少包含：

- Linux GCC Debug/Release；
- Linux Clang Debug/Release；
- ASan/UBSan；
- TSan threading +关键 Pipeline；
- Windows MSVC Debug/Release IOCP；
- Windows ASan 常态 job；
- IPv4/IPv6 TCP integration；
- EMFILE/ENOBUFS/WSAENOBUFS 故障注入；
- Packet fuzz；
- repeat-50；
- 10k capacity/performance gate；
- 24 小时候选 endurance；
- 72 小时 release endurance；
- install consumer 编译并执行；
- static analysis；
- warning policy；
- public API diff。

发布资产：

- annotated tag；
- release notes；
- known limitations；
- checksums；
- SBOM；
- provenance；
- CI/performance/endurance manifests；
- raw benchmark samples；
- API manifest；
- upgrade notes。

GitHub Actions 的短期 artifact 不能作为唯一长期证据，关键 manifests 和原始样本必须附到 Release。

### 9.5 M5 完成定义

- 所有层的 overload reason 可以从 ingress 追踪到最终 action；
- metrics-on 开销在专用 runner 上有预算；
- runtime 配置可验证；
- 最终候选 SHA 拥有完整 CI、性能、24/72 小时证据；
- 发布仍可声明 API provisional，但不得模糊 ABI/安全/容量边界。

## 10. M6：TLS 与 Gateway Security

### 10.1 前置条件

- M1/M2 生命周期和 shutdown 已闭环；
- M4 transport-neutral ingress 已稳定；
- M5 overload/metrics 能观察 handshake 和 auth；
- plain TCP 行为保持不变。

### 10.2 ConnectionTransport 内部抽象

先在 TcpConnection 内部建立窄 transport helper：

- plain socket read/write；
- TLS read/write；
- WANT_READ/WANT_WRITE；
- pending encrypted/plain bytes；
- shutdown/cancel；
- error mapping。

该抽象服务 TcpConnection，不与上层 TransportEndpoint 混为一谈。

### 10.3 TLS 合同

- 非阻塞 handshake；
- handshake timeout；
- certificate chain/hostname verification；
- SNI；
- session resumption；
- WANT_READ/WANT_WRITE 公平性；
- output/input hard limit；
- `close_notify` 与 forced close；
- pending I/O teardown；
- Linux/Windows；
- invalid certificate、truncated record、slow handshake、renegotiation policy；
- TLS 库版本和 CVE 更新策略。

### 10.4 Gateway Security

- 注入式 token validator；
- nonce/replay protection hook；
- pre-auth frame count/bytes/rate limit；
- per-peer auth attempt rate；
- auth failure reason 不泄露敏感细节；
- security metrics 限制 label cardinality；
- 日志不记录 token/secret；
- fuzz auth envelope；
- TLS 和认证分别可测试，但公网发布 gate 要求二者同时成立。

## 11. M7：UDP experimental

### 11.1 范围

首版只实现非阻塞 datagram 基础：

- IPv4/IPv6；
- zero-length datagram；
- truncation detection；
- peer identity；
- owner-loop read budget；
- receive/send result；
- stop/detach；
- basic session mapping；
- Linux epoll 与 Windows IOCP。

首版不同时实现 PMTU、raw ICMP、FEC、可靠传输或复杂拥塞控制。

### 11.2 平台路径

- Linux 先以 recvfrom/sendto 建立合同，再按证据考虑 recvmmsg/sendmmsg；
- Windows 使用 overlapped WSARecvFrom/WSASendTo，并保持 completion lifetime；
- burst drain 必须有 count/time budget；
- peer map 和 receive buffer 必须有内存上限。

### 11.3 测试与门禁

- truncation、zero-length、oversized datagram；
- IPv4/IPv6；
- burst fairness；
- stop 与 pending receive completion；
- peer address reuse；
- malformed datagram fuzz；
- packet loss 不应破坏资源回收；
- UDP target 保持 experimental、默认关闭。

## 12. M8：KCP preview

### 12.1 前置条件

- UDP owner/lifetime/backpressure 合同稳定；
- fake clock 和 fake network 可用；
- packet/session memory 上限可观测；
- UDP decoder fuzz 已建立。

### 12.2 首版范围

- conversation/session identity；
- ACK/RTO/retry；
- loss/reorder/duplicate/jitter；
- sequence wrap；
- fragmentation/reassembly；
- max fragment/message/session memory；
- idle/timeout/stop；
- decoder fuzz；
- metrics。

### 12.3 明确限制

首版 KCP 只能是 preview：

- 不宣称 production-grade congestion control；
- 不在同一版本加入 PMTU、cwnd、冗余和 XOR parity；
- 每个扩展必须有独立 intent、仿真模型和 promotion gate；
- 与 TCP/TLS Core 保持单向依赖和独立 target。

## 13. 建议的近期 PR 拆分

### PR-A：Phase 6 infrastructure audit

- 更新候选证据；
- Metrics 标记 provisional；
- benchmark schema v2；
- API manifest 分类；
- 不创建稳定 tag。

### PR-B：EventLoop control-plane saturation

- control lane/coalescing intent；
- TcpServer 聚合 shutdown；
- queue-full 生命周期故障注入；
- graceful future 必然完成。

### PR-C：Connector、Channel batch 与 IOCP 同步错误

- Connector owner-loop contract；
- request generation；
- active batch reclamation；
- WSARecv/WSASend 同步失败；
- TSan/ASan contracts。

### PR-D：Phase 4 typed dispatch results

- PostResult/DispatchResult；
- SessionManager 不再忽略 admission；
- Pipeline continuation 失败关闭；
- Broadcast reason 完整映射。

### PR-E：Session generation 与 Pipeline/Core 对齐

- duplicate replace generation；
- Core authenticated marker；
- graceful shutdown coordinator；
- heartbeat/idle full path。

### PR-F：Performance harness v2

- workload profiles；
- requested/accepted/rejected；
- RSS、CPU、P99/P999、recovery；
- metrics on/off；
- 10k profile。

### PR-G：IOCP batching 与 buffer ownership

- GQCSEx；
- wakeup coalescing；
- read pool；
- segmented write；
- AcceptEx pool。

### PR-H：公平预算与时间轮

- accept/channel/timer budget；
- timer semantics；
- deadline bucket/wheel；
- loop selector。

### PR-I：Rich Packet 与 ingress

- envelope；
- codec；
- fuzz；
- transport-neutral ingress。

### PR-J：正式 Gateway/Pipeline

- injectable auth；
- session/logic/output coordinator；
- end-to-end backpressure；
- real TCP benchmark。

### PR-K：Production metrics/config

- MetricId；
- sharded recorder；
- validated runtime config；
- release observability。

### PR-L：0.3 production-candidate closure

- 不新增功能；
- 最终 SHA 全量 CI；
- Linux/Windows performance；
- 24/72 小时 endurance；
- API diff；
- SBOM/provenance/checksums；
- Release 和 known limitations。

### PR 依赖与合并顺序

| PR | 前置依赖 | 可以并行的工作 | 合并后才能开始 |
|---|---|---|---|
| A | 无 | B 的 intent/contract 设计 | 最终 API/发布分类 |
| B | EventLoop overload intent 评审 | C 的 Connector/Channel 测试设计 | D、Server saturation 修复 |
| C | 相关 lifetime/thread intent | B 的非重叠实现 | IOCP v2 数据路径 |
| D | B 的 PostResult/control contract | Broadcast reason 设计 | E |
| E | D | F 的 workload 设计 | 正式 Pipeline promotion |
| F | B/E 的可观测字段确定 | G 的 buffer intent | G、H 的性能比较 |
| G | C、F baseline | H 的 timer/fairness 设计 | Windows capacity gate |
| H | F baseline | G | M3 完成 |
| I | D/E 的结果与 generation 模型 | G/H | J |
| J | I、E | K 的 recorder 设计 | M5-B、TLS |
| K | F、D 的 reason schema | J | L |
| L | A～H、K；若纳入 Gateway 则还需 I/J | 不与运行时代码修改并行 | v0.3 candidate 发布 |

推荐执行节奏：

1. A 先收口治理边界，同时完成 B 的 intent 和失败测试；
2. B 合并后立即推进 C、D；
3. D 后推进 E，形成 Phase 4/5 语义闭环；
4. F 先建立可信 baseline，再实施 G、H；
5. Core/Phase 4 候选按 A～H、K、L 收口；
6. I/J 在依赖稳定后建立 Gateway Preview；
7. TLS、UDP、KCP 不与 P0/P1 修复争抢主线评审资源。

## 14. 计划测试文件与证据映射

下列路径是计划中的明确验证落点。实现 PR 可以调整命名，但不能删除对应验证层次；新文件必须注册到 CTest，并回写到相关 active intent。

| 工作流 | 必须更新的 intent/rules | 计划新增或扩展的测试 |
|---|---|---|
| EventLoop control saturation | `event_loop.intent.md`、`graceful_shutdown.intent.md`、thread/ownership/testing rules | `tests/contract/event_loop/test_event_loop_control_saturation.cpp` |
| TcpServer 饱和停服 | `tcp_server.intent.md`、`graceful_shutdown.intent.md` | `tests/contract/tcp_server/test_tcp_server_saturation_shutdown.cpp` |
| TcpConnection 通知/写状态 | `tcp_connection.intent.md`、`backpressure_policy.intent.md` | `tests/contract/tcp_connection/test_tcp_connection_queue_saturation.cpp` |
| Connector 线程合同 | `connector.intent.md`、`tcp_client.intent.md`、thread rules | `tests/contract/connector/test_connector_thread_contract.cpp` |
| active Channel lifetime | `channel.intent.md`、`event_loop.intent.md`、lifetime intent | `tests/contract/channel/test_channel_active_batch_lifetime.cpp` |
| IOCP 同步错误与批处理 | `poller.intent.md`、`acceptor.intent.md`、`tcp_connection.intent.md` | `tests/contract/tcp_connection/test_tcp_connection_iocp_sync_error.cpp`、`tests/integration/tcp/test_iocp_batch_completion.cpp` |
| TcpServer idle timeout | `tcp_server.intent.md`、`timer_queue.intent.md` | `tests/contract/tcp_server/test_tcp_server_idle_timeout.cpp`、`tests/integration/tcp/test_tcp_server_idle_timeout.cpp` |
| 上层投递饱和 | `player_session.intent.md`、Pipeline use-case intent、`broadcast.intent.md` | `tests/integration/game_pipeline/test_game_server_pipeline_saturation.cpp` |
| Session generation | `player_session.intent.md`、Pipeline use-case intent | `tests/integration/game_pipeline/test_game_server_pipeline_binding_generation.cpp` |
| Pipeline graceful stop | Pipeline use-case intent、`graceful_shutdown.intent.md` | 扩展 `tests/integration/game_pipeline/test_game_server_pipeline_shutdown.cpp` |
| Broadcast 真实过载 | `broadcast.intent.md`、`game_backpressure_policy.intent.md` | `tests/integration/broadcast/test_broadcast_tcp_overload.cpp` |
| Logic fairness/lag | `logic_loop.intent.md`、`game_backpressure_policy.intent.md` | `tests/contract/game_logic/test_logic_loop_fairness.cpp` |
| Rich Packet | 新增 active game-packet intent、更新 scope | `tests/contract/protocol/test_game_packet_contract.cpp`、新增 fuzz harness/corpus |
| Gateway | 新增 active gateway intent、更新 ownership/thread rules | `tests/integration/game_gateway/` 下 auth、handoff、shutdown、overload 矩阵 |
| Metrics hot path | `metrics_exporter.intent.md`、performance baseline intents | metrics contract + metrics-on/off benchmark |
| TLS | `connection_transport.intent.md`、`tls.intent.md` | `tests/contract/tls/` 与真实 loop integration |
| UDP | `udp.intent.md`、platform/runtime rules | `tests/contract/udp/`、`tests/integration/udp/`、decoder fuzz |
| KCP | `kcp_transport.intent.md` | fake-clock/network contract、loss/reorder/fragment fuzz |

每个候选 SHA 的证据清单必须包含：

- 普通 Debug/Release CTest；
- Linux ASan/UBSan；
- Linux TSan；
- Windows IOCP Debug/Release；
- 可用时的 Windows ASan；
- 真正的多 loop TCP integration；
- saturation、slow reader/writer、RST/half-close、connection storm；
- auth success/timeout/disconnect/replacement 竞争；
- real TCP broadcast slow endpoint；
- metrics on/off；
- repeat/nightly fuzz；
- 24/72 小时 RSS、fd/handle、queue-depth plateau。

性能回归预算与绝对容量 SLO 必须分开记录：

- 回归预算回答“同 runner 是否比上一基线退化”；
- 容量 SLO 回答“指定硬件和 workload 能承载多少连接/吞吐/延迟”；
- synthetic endpoint benchmark 只能证明调度算法成本，不能作为真实网络容量证明。

## 15. 建议发布序列

版本号可在实施时调整，但发布含义应保持清晰：

| 建议版本 | 主要内容 | 稳定性声明 |
|---|---|---|
| `v0.3.0-production-candidate` | M0、M1、M2、M3 与 M5-A | Core/Phase 4 source contract 候选；无 ABI 承诺 |
| `v0.4.0-performance-preview` | M3 的 100k 容量提升、趋势阈值固化和平台调优 | 容量证据预览 |
| `v0.5.0-gateway-preview` | M4 正式 Packet/Gateway | 上层 API provisional |
| `v0.6.0-tls-preview` | M6 TLS/Security | 安全部署预览 |
| `v0.7.0-udp-experimental` | M7 UDP | experimental |
| `v0.8.0-kcp-preview` | M8 KCP | experimental/preview |

只要 M1～M3、Metrics 或其他工作改变运行时代码，现有 Phase 6 的 24/72 小时结果就不能直接作为最终 0.3 候选证据，必须在最终 SHA 重跑。

## 16. 全局完成标准

项目只有同时满足以下条件，才能从“高质量 Preview”提升为“生产候选网络底座”：

- 所有控制和生命周期操作在队列饱和时仍能收敛；
- 没有未定义的跨线程普通字段访问；
- 没有依赖裸指针存活概率的 active batch/queued callback；
- input、logic、output、broadcast 和 global memory 都有可观测硬边界；
- duplicate login、stale completion、shutdown race 不产生业务副作用；
- Linux/Windows 的错误、取消、关闭语义一致；
- 10k/100k、慢客户端、连接风暴、timer storm、真实 TCP broadcast 有证据；
- metrics 开启后的性能开销受控；
- 正式 Gateway 不包含业务逻辑，但具备可注入 auth/handler/output；
- TLS/UDP/KCP 只在其依赖合同稳定后进入 active scope；
- 每个发布版本都有精确 SHA、兼容性说明、原始证据和已知限制。

## 17. 明确暂缓项

以下内容不进入近期主线：

- HTTP/WebSocket/RPC；
- 游戏账号系统；
- AOI、房间、世界状态；
- 对象序列化框架；
- coroutine 全面改写；
- 未经基准证明的 lock-free 重构；
- UDP 首版中的 PMTU/FEC；
- KCP 首版中的复杂拥塞控制和冗余算法；
- 没有 TLS、安全认证和限流合同的公网 production 声明。

近期最重要的交付物不是更多协议，而是：

> 一套在队列饱和、连接风暴、慢客户端、回调异常和优雅停服过程中，仍能通过合同测试证明必然收敛的 TCP 游戏服务器网络底座。
