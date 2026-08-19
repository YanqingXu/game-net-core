# game-net-core 加速执行计划

计划重制日期：2026-08-19

长期方向：`goal.md`

历史实现检查点：`669ebb0a7c5c475dea74b12275c66a2ce1876804`

历史发布证据引用：`v0.3.0-rel-c1-refreeze-5`。它替代
`v0.3.0-rel-c1-refreeze-4@c061f9967b9481b70b2faf9a8fee24f5a3e72ffc`，但从本计划开始不再作为
开发基线、分支门或后续任务的冻结点。

当前测试基线：122（8/101/13；threading 95、lifecycle 100）。API-R1 已完成，
M3-R3（本地关闭）的生命周期修复和 PERF-R1 的 additive API 审查继续作为历史事实。

## 1. 执行决策

本计划停止“冻结候选 → 全量重验 → 工具修复 → 再冻结”的串行工作方式，切换为：

> **main 持续前进 + 小步纵向切片 + 每提交精确留证 + 里程碑按需推广。**

具体规则：

1. 不再把 REL-C1、candidate tag 或 refreeze 作为开发前置条件；
2. 已存在的 tag、run 和 evidence 保持不可变，只作为历史检查点；
3. 运行时、合同或工具发现问题时直接 fix-forward，不返回旧候选重新冻结；
4. 每份验证证据仍绑定精确 commit，不允许把旧证据提升为新提交的结论；
5. 24/72 小时 endurance、许可证和发布包装只阻塞对外推广，不阻塞架构演进；
6. endurance waiver 只能表达缺失证据，不能表达 endurance 通过；
7. ARCH-G1 合同资产与 IOE-R1 已落地；当前连续进入 IOE-R2，不等待候选冻结或独立
   review 排期；
8. IOE 与 Runtime Model 是两条并行实现线，持续证据是第三条伴随线。

## 2. 不可放宽的工程约束

加速不改变 intent-driven 约束。每个重要 Core 切片仍必须在同一变更中完成：

1. 确认或提升对应 active intent；
2. 写明 owner、ownership、re-entry、cross-thread marshal 和 shutdown；
3. 更新 thread-affinity、ownership、testing 等相关 rules；
4. 先新增能约束目标行为的 contract，再实现；
5. 运行 focused repeat、全量测试和 scope/intent/API guards；
6. 检查 Linux/epoll 与 Windows/IOCP 语义；
7. 对稳定 public surface 做显式兼容性决定；
8. 更新架构理解和 evidence ledger。

继续禁止：

- Core 反向依赖协议、Session、Logic、Broadcast 或业务状态；
- 用无界队列、无界 operation 或无界 final drain 换取表面吞吐；
- 把 IOCP completion 继续扩展成更多 fake readiness 语义；
- 在没有 active intent 的情况下启动 UDP、KCP、TLS、HTTP、RPC 或 coroutine；
- 把 callback、ownership 或 cancellation 行为留给“实现约定”而不写合同。

## 3. 总体推进图

```text
当前 main
   |
   +--> ARCH-G1：架构 intent / ADR / 基线（立即开始，限时）
           |
           +--> IOE-R1：source-private I/O Engine seam
           |       |
           |       +--> IOE-R2：epoll Readiness Engine
           |               |
           |               +--> IOE-C1：IOCP direct Completion
           |                       |
           |                       +--> IOE-X1：实验性 io_uring
           |
           +--> RTM-R1：三个 TCP-only provisional Profile
                   |
                   +--> RTM-R2：Logic sharding / Hybrid

持续伴随：快速合同门 -> 双平台 CI -> benchmark/capacity 趋势
按需推广：promotion commit -> 24/72h endurance -> license -> release decision
```

ARCH-G1 是共同起点；IOE-R1 与 RTM-R1 在架构边界明确后并行。IOE-C1 不等待所有
Runtime Profile 完成，RTM-R1 也不等待 IOCP direct completion 完成。

## 4. M0：执行模式切换

时间盒：1 个工作日。

任务：

- [x] 停止把 `v0.3.0-rel-c1-refreeze-5` 当作当前开发冻结点；
- [x] 将 REL-C1/REL-V1/REL-V2/PERF-R1/END-R1 从串行关键路径移除；
- [x] 把历史 tag、review、run 和失败样本保留为不可变 evidence；
- [x] 将长期目标中的冻结前置条件改为 continuous-evidence 原则；
- [x] 更新 roadmap、assessment、README、migration status 和治理合同的当前任务；
- [x] 建立每个新架构切片的 commit-bound evidence ledger 条目模板：
  `docs/development/commit_bound_evidence_ledger.md`。

关闭门：仓库不再有“REL-V1 是唯一下一任务”或“运行时变化必须 refreeze”的当前约束；
历史记录可以继续包含这些词，但必须标明 historical/superseded。

## 5. M1 / ARCH-G1：架构契约与基线

时间盒：2 个工作日。超过时间盒仍未形成可审查合同，立即缩小范围，不继续补写宏大设计。

本阶段不改运行时行为，但必须为下一提交直接提供实现入口。

### 5.1 Intent 与 ADR

- [x] 新建并激活 owner-loop + I/O Engine architecture intent；
- [x] 新建并激活 Runtime Model architecture intent；
- [x] 用 ADR 明确 EventLoop 是 owner scheduler/event pump，不等同于 epoll Reactor；
- [x] 明确 `Channel` 只属于 Readiness registration/callback binding；
- [x] 明确 Completion operation identity、generation、lease 和 terminal retirement；
- [x] 明确 ConnectionPlacementPolicy 与 LogicShardPolicy 分离；
- [x] 写出 source-private seam 到 stable public surface 的兼容策略。

### 5.2 基线与测试地图

- [x] 盘点 `EventLoop`、`Poller`、`Channel`、IOCP transport、options 和 metrics 的
  readiness/completion 耦合点；
- [x] 固化 Linux/epoll 与 Windows/IOCP 当前吞吐、P99/P999、wakeup、内存、shutdown、
  operation-retention 基线；
- [x] 为 IOE-R1/IOE-R2/IOE-C1 指定具体 contract 文件和失败 fixture；
- [x] 为三个 Runtime Profile 指定生命周期、饱和、handoff、Tick、内存和 shutdown 测试；
- [ ] 独立 review 只审 intent、边界、合同和基线，不等待发布证据。

关闭门：所有下一阶段的 owner、生命周期、失败结果、测试文件和性能预算可直接执行；
不存在还需再次召开“抽象边界设计轮”的开放问题。

## 6. M2 / IOE-R1：source-private I/O Engine seam

时间盒：3–5 个工作日。

目标：以不改变外部行为的方式建立可替换 Engine 边界，立即解除 EventLoop 对具体
Poller/IOCP 形状的长期耦合。

### 6.1 合同先行

- [x] 为 `IoEngine` 定义 owner-thread-only register/submit/cancel/dispatch；
- [x] 定义 `IoNoticeBatch`、capabilities、wakeup、`beginQuiesce()` 与 `quiescent()`；
- [x] 固定 Accepted/Rejected、admission seal、final drain 和 callback containment；
- [x] 新合同覆盖跨线程 wakeup、callback 内关闭、generation stale notice 和预算耗尽续跑；
- [x] 现有 EventLoop/Poller/Channel/TCP lifecycle contracts 保持通过；Windows/IOCP
  与 Linux/epoll Release CTest 均为 122/122。

### 6.2 最小实现

- [x] 新增 source-private `PollerIoEngineAdapter`；
- [x] EventLoop 只依赖 Engine seam，不直接判断具体 IocpPoller 类型；
- [x] 维持当前 Poller/Channel 数据路径，不在本阶段顺手重写 epoll 或 IOCP；
- [x] 将公共调度预算与 backend capacity 配置分层，但暂不改变 stable options；
- [x] 不新增公共 header；同线 public API compatibility gate 保持通过。

关闭门：Windows/IOCP 与 Linux/epoll 全量合同通过；相对 ARCH-G1 基线没有未接受的
吞吐、延迟或内存回归；EventLoop 已可在不改变 owner/lifecycle 公理的情况下驱动不同
capability Engine。IOE-R1 在精确提交
`8bb14e72d8935879396d12a7a51c891311aa2a78` 关闭；完整双平台、治理和配对基准证据见
`docs/development/commit_bound_evidence_ledger.md`。

## 7. M3 / IOE-R2：epoll Readiness Engine

时间盒：4–7 个工作日。

- [ ] 定义 `ReadinessPort`、registration identity/generation 和 `ReadinessNotice`；
- [ ] 将 epoll register/wait/wakeup/dispatch 收入 Readiness Engine；
- [ ] 保持 level/edge、EAGAIN、fd replacement、remove-before-destroy 和 active-batch
  retirement 合同；
- [ ] 明确同 source mask 可合并，但 stale generation 不可回调；
- [ ] 用批处理和预算保护 accept/read 热路径；
- [ ] epoll 保持 Linux 默认后端和后续 io_uring 的 fallback；
- [ ] benchmark/capacity 与 ARCH-G1 基线比较并形成数字决策。

关闭门：Linux Readiness 路径不再依赖 Completion 抽象；核心 TCP 行为和 public surface
无意外变化。

## 8. M4 / IOE-C1：IOCP direct Completion

时间盒：7–12 个工作日，可拆成 operation model、read、write、accept/connect、shutdown
五个连续可合并切片；不等待一个大 PR。

### 8.1 Operation 公理

- [ ] operation identity + generation 唯一；
- [ ] Accepted submission 导向唯一 terminal completion；
- [ ] 同步非 pending 失败不建立 future obligation；
- [ ] observer revoke 与 kernel obligation/storage lease 分离；
- [ ] terminal dequeue 后只在 owner 完成 retirement；
- [ ] cancel request 不等于 completion 已发生。

### 8.2 数据路径

- [ ] GQCSEx 结果直接形成 `CompletionNotice`；
- [ ] read/write/accept/connect/cancel completion 保留独立 operation 身份；
- [ ] 移除 fake `kReadEvent/kWriteEvent` 的结果转译；
- [ ] 移除 Channel 对 IOCP operation 和 storage 的携带职责；
- [ ] TcpConnection 保持统一连接、发送、背压、关闭和 callback contract；
- [ ] source-private IOCP TCP driver 管理 repost、pause-read、partial write 和 buffer lease。

### 8.3 关闭

- [ ] `Running -> Quiescing -> FinalDraining -> Shutdown` 可观察；
- [ ] cancel、late completion、peer close、owner quit 和 callback destruction 组合覆盖；
- [ ] 无 callback-after-destroy、kernel-reference-after-free、phantom completion 或
  stranded Accepted operation；
- [ ] Windows/IOCP 生命周期、饱和、容量和性能证据通过；
- [ ] Linux/epoll 全量合同无回归。

## 9. M5 / RTM-R1：TCP-only Runtime Profiles

启动条件：ARCH-G1 关闭。它与 IOE-R1/IOE-R2/IOE-C1 并行，不修改 stable Core 来等待
某个后端重构。

时间盒：每个 Profile 3–5 个工作日，逐个形成可运行垂直切片。

### 9.1 Profile A：SingleLoopInlineEvent

- [ ] 限定 handler 执行预算和禁止阻塞规则；
- [ ] 明确 transport、I/O topology、callback context 和 backpressure；
- [ ] 以 echo/轻量请求建立最低 handoff 基线。

### 9.2 Profile B：MultiIoQueuedEvent

- [ ] 使用已有 TransportEndpoint 和有界 queue；
- [ ] empty-to-non-empty 合并 wakeup；
- [ ] typed overload、generation-safe endpoint 和 owner executor 回送；
- [ ] 测量 P99/P999 handoff、队列 oldest age 和慢消费者恢复。

### 9.3 Profile C：MultiIoDedicatedFixedTick

- [ ] 明确 FixedDelay、FixedRateSkipMissed 或 FixedRateBoundedCatchUp；
- [ ] 不再把普通周期性 drain 描述为权威 fixed tick；
- [ ] 测量 Tick jitter、overrun、skip、catch-up 和 shutdown drain；
- [ ] 逻辑输出只通过 endpoint owner executor 返回连接 owner。

三个 Profile 全部保持 provisional。只有至少两个垂直切片证明共同需要时，才提升新的
安装接口；不得为每个组合创建一个 Server 类。

## 10. M6 / RTM-R2：Sharding 与 Hybrid

启动条件：RTM-R1 至少两个 Profile 有完整合同和数字证据。

- [ ] 分离 `ConnectionPlacementPolicy` 与 `LogicShardPolicy`；
- [ ] 已建立 TCP 连接不迁移 owner loop；
- [ ] 命令按 player/room/scene key 进入有界逻辑 cell；
- [ ] cell 内有序，跨 cell 不承诺全局顺序；
- [ ] event-driven 事务路径与 FixedTick 模拟路径可以并存；
- [ ] Actor、AOI、Room、World、脚本和业务状态继续位于 Core 外。

## 11. IOE-X1 与后续 Transport

IOE-X1 只有在 IOE-C1 的通用 Completion contract 稳定后启动：

- one-shot accept/recv/send；
- typed SQ-full rejection；
- cancel、terminal completion、lease 和 final drain；
- epoll 继续作为默认/fallback；
- multishot、provided buffers、fixed files、zero-copy、SQPOLL 全部后置。

UDP/可靠数据报/KCP 只有在 Core、Engine 和至少两个 TCP Profile 稳定后才可提升对应
deferred intent。HTTP、WebSocket、RPC、TLS 和 coroutine 继续不在当前路线内。

## 12. 持续证据策略

### 12.1 每个实现提交

- scope、intent consistency/metadata/semantics；
- focused contract + repeat；
- Windows/IOCP Release 全量；
- Linux/epoll CI 或在 PR 内完成的等价远端门；
- API manifest diff；
- 失败日志和结构化结果保留。

### 12.2 每个里程碑

- Linux Debug/Release/ASan/UBSan/TSan；
- Windows Debug/Release/IOCP；
- install consumers；
- paired benchmark/capacity；
- 独立 review；
- migration status 与 architecture docs 更新。

### 12.3 仅在准备推广时

- 选择当前 main 上一个明确 promotion commit；
- 在该 commit 上生成完整 same-commit CI、benchmark、capacity；
- 需要 production claim 时再运行 24/72 小时 endurance；
- 完成许可证、SBOM 和 third-party notices；
- 输出 GO、NO-GO 或 INTERNAL-CANDIDATE-ONLY。

不提前冻结 promotion commit。证据运行期间 main 可以继续开发；推广分支只接受证据修复，
若运行时发生变化则选择更新的 promotion commit，而不是 refreeze 旧 tag。

## 13. 旧发布任务的处理

| 旧任务 | 新状态 | 新用途 |
| --- | --- | --- |
| REL-C1 | retired-as-development-gate | 历史 tag 与候选记录保持不可变 |
| REL-V1 | continuous-local-gate | 每个 Core 切片执行 clean/focused/full local gate |
| REL-V2 | continuous-remote-gate | 每个合并候选执行 Linux/Windows CI |
| PERF-R1 | continuous-evidence-lane | 每个里程碑执行 paired benchmark/capacity |
| END-R1 | promotion-only | 只在需要 production claim 的 promotion commit 执行 |
| LIC-R1 | external-release-only | 不阻塞架构开发，继续阻塞外部采用 |
| REL-D1 | on-demand-promotion-decision | 证据齐备时才创建发布决策任务 |

历史 P1-02 继续表示“当前没有可推广的完整同提交发布证据”，但不再表示架构开发被阻塞。
P1-03 继续阻塞 externally adoptable release。P1-04 | API-R1（已关闭）和
P2-02 | GOV-R2（已关闭）的历史结论不变。

## 14. 并行度与 WIP

允许同时存在：

1. 一个 IOE Core 实现切片；
2. 一个 Runtime Profile 垂直切片；
3. 一个持续 benchmark/CI/evidence 任务。

同一时刻只允许一个切片改变稳定 Core 生命周期语义。Profile 线不得通过修改 Core
绕过 IOE 线的未完成合同。

失败处理：

- 单平台失败：暂停受影响切片并 fix-forward，另一正交线可继续；
- 基准回归：先定位 allocation/copy/wakeup/virtual dispatch，再决定接受或回退；
- stable API drift：没有兼容性决定不得合并；
- 生命周期不变量失败：立即停止该 Core 切片，不以 waiver 或测试降级继续；
- 外部 runner 不可用：记录基础设施阻塞，继续不依赖该 runner 的设计/合同工作。

## 15. 任务完成定义

任务状态统一为：

```text
planned -> contract-ready -> implemented -> verified -> integrated
```

不再使用 `frozen`、`refrozen` 或“候选 SHA 未变化”作为普通任务状态。

`integrated` 必须满足：

- active intent 和 rules 与实现一致；
- 具体 contract 文件存在并能证明旧行为不满足或新不变量成立；
- owner、ownership、re-entry、marshal、failure result 有答案；
- focused、全量、跨平台和 scope/API guards 达到本阶段门；
- benchmark/capacity 对热路径变化给出数字；
- 文档描述当前事实，不把历史 evidence 当成当前提交证据。

## 16. 当前执行前沿

现在立即执行：

> **IOE-R2：IOE-R1 已在精确提交 `8bb14e72d8935879396d12a7a51c891311aa2a78`
> 关闭。现在直接以失败合同定义 `ReadinessPort`、registration identity/generation 和
> `ReadinessNotice`，随后把 epoll register/wait/wakeup 收入显式 Readiness Engine。**

ARCH-G1 独立 review 与 RTM-R1 的三个 Profile contract 继续并行，不形成冻结点。下一个
可运行目标不是新发布 tag，而是：Linux 默认 epoll 路径具有显式、generation-safe 的
Readiness registration/notice 边界，Windows/IOCP 兼容路径与 stable public surface 保持
不变，合同和配对基准持续全绿。
