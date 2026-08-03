# game-net-core 当前项目审计报告

审计日期：2026-07-30
审计基线：`main@7a56132d6ea60346ec06c108cd627b7b4cd5a04f`
报告性质：重新审计；本文件已完全替换旧审计内容

## 1. 执行结论

`game-net-core` 当前应定义为：

> 已具备较强契约与测试基础的 production-hardening preview，尚不是可宣布稳定或可对外采用的 production release。

本轮没有发现 P0。发现 4 项 P1 和 2 项 P2；截至 2026-08-03，P1-01 已关闭，
其余 finding 仍保持开放：

| ID | 级别 | 结论 | 性质 |
| --- | --- | --- | --- |
| P1-01 | P1（已关闭） | 新候选 `95a6ab5` 已修复构造失败 fd 双重所有权，并通过 clean 独立全矩阵审查与同 SHA 六 producer CI | 核心线程/生命周期正确性 |
| P1-02 | P1 | `95a6ab5` 的六个同 SHA CI producer 已通过，但 retained aggregate artifact、性能、容量与 endurance 证据仍不完整 | 发布证据 |
| P1-03 | P1 | 顶层许可证没有授予使用、复制、修改或分发许可 | 对外采用阻塞 |
| P1-04 | P1 | 0.3 stable Core 表面变化尚未完成独立审查 | API 发布阻塞 |
| P2-01 | P2 | `EventLoopThreadPool` 非法配置和状态转换没有完整拒绝 | 公共契约 |
| P2-02 | P2 | 路线图文档与实际 HEAD 存在时间和完成状态漂移 | 治理一致性 |

因此：

- 可以继续 M3 production-hardening 修复与本地验证。
- 不应把当前 SHA 标记为 `v0.3.0-production-candidate` 的最终候选。
- 不应宣称 Linux/Windows 双平台已经在当前 SHA 通过发布门。
- 不应宣称已有当前 SHA 的容量上限、性能无回归或 24/72 小时稳定性结论。
- P1-01 已在 `95a6ab5` 关闭；`446f86d` 的拒绝结论保留为历史证据。

### 1.1 M3-R1 收口检查点（2026-08-03）

候选 `446f86d10c8c78725bf59bbabdebd7f3d1968af3` 已完成首次独立审查，但结论为 `request changes`：

- intent、thread-affinity、ownership 和 testing rules 已补充 accepted-fd、establishment handoff、exact rollback 与有界 owner-loop cleanup 合同；
- 新增 `test_tcp_server_establishment_saturation`，以 8 normal + 4 reserve 的小容量和两个 least-connections worker 验证 map/load/per-peer admission/deadline、Accepted counter/metric、owner 析构与后续恢复；测试总数为 120，contract/threading/lifecycle 分别为 99/93/98，intent 显式 verification paths 为 138；
- 修复前该合同路径稳定触发 off-owner 析构不变量；最终加强版修复后 Windows/IOCP 与 Linux/epoll 的 Debug/Release focused repeat 均为 50/50；
- Windows Debug 120/120、Release 120/120 CTests 通过，36/36 repository/API/CI guards 通过；
- WSL2 Ubuntu 24.04.4 LTS（Linux 6.18.33.2、G++ 13.3.0、CMake 3.28.3）完成真实 epoll 构建；Debug 与 Release 库存均为 120（threading 93、lifecycle 98），全量均为 120/120；
- Linux 首次编译发现并修复两个测试基础设施可移植性缺口：IOCP-only harness 方法签名完整置于 `_WIN32` 下，EventLoop fair-budget 测试在 Linux 使用 native nonblocking `socketpair`；对应静态契约守卫已补齐；
- public API manifest 未变化，回滚登记上限绑定 worker normal functor queue capacity，没有引入无界队列；
- 独立 reviewer 在 `TcpConnection` partial construction 中发现 exact-once fd 缺口：`socket_` 已取得 fd 后，base `pendingSocket` 尚未 release，后续构造异常会产生 double-close/误关复用 fd 风险；
- 当前 remediation 已把 connection-side fd claim 移到所有可抛构造步骤之后，并在 participant mutex 内连续执行构造、rollback owner 保存和 base guard release；新增确定性 construction-failure hook 验证抛出点 connection 侧尚未持有 fd；
- remediation 工作树的 Windows/IOCP 与 WSL2 Linux/epoll Debug/Release focused 均为 50/50、全量均为 120/120，36/36 guards 通过；
- 独立 reviewer 对 remediation 的只读 pre-review 结论为 `approve-for-candidate-freeze`、无实现 blocker；
- remediation 已提交为 clean candidate `95a6ab5afbe33c4f84ab11c926e4867da94e8282`；
- 独立 Codex reviewer 从 `/home/xyq/m3-r1-review-95a6ab5-kubcup/source` 完成 11 行矩阵，Linux/epoll Debug/Release focused 50/50、全量 120/120，并给出 `approve`；
- 同 SHA GitHub Actions run `30813037693` attempt 2 的 Linux Debug/Release/TSan/ASan-UBSan 与 Windows Debug/Release 六个 producer 全部成功；
- aggregate job 为 success，但 artifact API 仍是 0，校验与聚合上传步骤被跳过，因此该缺口继续归入 P1-02。

P1-01 已关闭。批准范围只覆盖 M3-R1 生命周期/所有权问题，不扩展到
0.3 stable API、retained artifact、性能、容量、endurance 或最终发布决定。

Linux 命令、逐项审阅矩阵和签字字段已冻结在
`docs/reviews/m3-r1-closure-review.md`；该文件保留首次拒绝历史、remediation
证据、新候选 clean review 和最终签核。

## 2. 审计范围与方法

审计顺序遵循仓库规则：

1. intent 正确性；
2. 公共契约；
3. 不变量；
4. 线程归属；
5. 所有权；
6. 生命周期；
7. 实现；
8. 测试和发布证据。

本轮覆盖：

- `intents/`、`rules/`、README、迁移状态和开发文档；
- EventLoop、Poller、IOCP/epoll、Channel、Wakeup、TimerQueue、DeadlineQueue；
- EventLoopThread、EventLoopThreadPool、Acceptor、Connector；
- TcpConnection、TcpServer、TcpClient；
- PacketFramer、Transport、Session、Logic、Broadcast、Metrics；
- CMake 目标、安装/API manifest、测试清单；
- GitHub Actions 当前 SHA 的 CI、benchmark、capacity 与 runner 状态。

本轮只重写 `assessment.md` 和 `plan.md`，没有修改运行时代码、测试或构建配置。

## 3. 冻结证据快照

### 3.1 Git 与版本

- 当前分支：`main`
- `HEAD`：`7a56132d6ea60346ec06c108cd627b7b4cd5a04f`
- 本地 `origin/main`：同一 SHA
- 提交时间：`2026-07-30T14:03:20+08:00`
- 提交标题：`ci: bind capacity and endurance promotion evidence`
- 最新已发布标签：`v0.2.0-phase4-preview`
- 当前 HEAD 位于该标签之后 62 个提交
- CMake package version：`0.3.0`
- 语言标准：C++23，关闭 compiler extensions

旧审计基线 `e24c8476a62838cea3ca18185964d68d792885fa` 到当前 HEAD：

- 26 个提交；
- 146 个文件变化；
- 20,455 行新增，780 行删除。

这 26 个提交已经完成或显著推进：

- IOCP Accept/Connect cancellation final drain；
- IOCP wakeup coalescing；
- stable segmented writes、partial write 与按需 read storage；
- bounded AcceptEx pre-post pool；
- EventLoop/IOCP 公平预算；
- repeating timer cadence、bucketed deadlines 和 loop selectors；
- Buffer/PacketFramer retention 回收；
- connection/loop/server/global TCP output memory hierarchy；
- Broadcast owner/global outstanding 原子记账；
- core/capacity/endurance 证据工作流。

旧审计中关于上述项目“尚未实现”的结论已经失效，不再沿用。

### 3.2 项目清单

| 项目 | 当前事实 |
| --- | ---: |
| 正式 intent | 61 |
| active intent | 30 |
| deferred intent | 20 |
| legacy intent | 11 |
| intent 显式 verification paths | 137 |
| public headers | 55 |
| `.cc` sources | 40 |
| CTest tests | 119 |

测试分区：

- unit：8；
- contract：98；
- integration：13。

交叉标签中包括：

- threading：92；
- lifecycle：97；
- game pipeline：7；
- broadcast：5。

### 3.3 本地验证

在当前 HEAD 上执行：

```text
cmake --build build-q1e-tests --config Release --parallel 8
ctest --test-dir build-q1e-tests -C Release --output-on-failure --timeout 60
```

结果：

- Release 增量构建成功；
- 119/119 CTests 通过；
- 0 failed；
- 总耗时约 56.72 秒。

随后执行 `.github/workflows/ci.yml` 当前主 gate 对应的仓库/API/CI Python 守卫：

- 36/36 通过；
- public API diff 成功生成；
- scope、intent metadata/semantics、migration provenance、CMake、平台、生命周期、workflow、benchmark/capacity/endurance 合同均通过。

证据边界：

- 这是现有完整构建树上的当前 HEAD 增量 Release 验证，不是全新目录的 clean configure；
- 本轮没有本地重跑 Debug、安装 consumer、ASan/UBSan、TSan 或 libFuzzer；
- 这些不能被 119/119 Release 结果替代。

### 3.4 本机 Windows 工具链

当前 `vswhere` 报告：

- Visual Studio Professional 2026 `18.8.2`；
- 路径：`D:\VS2026`；
- `isComplete=true`；
- `isLaunchable=true`；
- `isRebootRequired=false`。

这说明早先 self-hosted Windows run 中的 C++ tool detection 阻塞在本机现状上已消失，但尚未通过新的 Actions run 形成远端证据。

### 3.5 当前远端证据状态

当前 SHA 的主要 Actions 状态：

- [`ci` run 30518726195](https://github.com/YanqingXu/game-net-core/actions/runs/30518726195) 仍处于 queued：
  - Windows Release job 在旧工具链状态下止于 bootstrap；
  - Windows Debug job 止于 GitHub checkout 网络失败；
  - Linux CMake、Release、ASan/UBSan、TSan jobs 仍未形成成功结果。
- [`windows-self-hosted-ci` run 30521379378](https://github.com/YanqingXu/game-net-core/actions/runs/30521379378) attempt 3 失败：
  - 当次执行无法发现 VS C++ tools；
  - artifact upload 又命中 storage quota。
- [`core-benchmark` run 30518766777](https://github.com/YanqingXu/game-net-core/actions/runs/30518766777) 在 job 启动前因账户 billing lock 失败。
- [`capacity-gate` run 30518769101](https://github.com/YanqingXu/game-net-core/actions/runs/30518769101) 同样在 job 启动前因 billing lock 失败。

当前 runner：

- `gamenet-windows`：online、idle；
- `gamenet-endurance`：offline。

这些失败不能被描述为代码测试失败，但它们同样不能产生发布证据。

## 4. 成熟度判断

| 维度 | 当前判断 |
| --- | --- |
| Intent/规则治理 | 强；清单、语义、依赖与 provenance 已有自动守卫 |
| Reactor/TCP 核心 | 强 preview；状态机、owner-loop 和 shutdown 合同密度高 |
| Windows IOCP | 已完成关键正确性和性能底座切片，本地 Release 合同通过 |
| Linux epoll | 当前工作树已完成 WSL2 本地 Debug/Release 120/120；已提交候选的远端结果仍未完成 |
| 公平性/容量 | 已有显式批次、队列、deadline、内存预算和 retention 边界 |
| 上层 foundation | Protocol/Transport/Session/Logic/Broadcast 可测试，但仍是 foundation |
| Metrics | 有结构化接口和测试，明确为 provisional、非生产热路径实现 |
| API/安装 | 0.3 manifest 和 diff 完整；stable surface review 未完成 |
| 发布可采用性 | 被当前 SHA 证据、许可证和 API 审查共同阻塞 |

## 5. 已确认的强项

### 5.1 Intent-first 已形成机器可验证闭环

当前不是仅靠人工维护模块列表。守卫会验证：

- intent front matter；
- active/deferred/legacy 状态；
- verification path；
- 迁移 provenance；
- 目标依赖方向；
- public API manifest；
- 平台与 lifecycle 合同。

这是项目当前最有价值的工程资产之一。

### 5.2 EventLoop 生命周期和公平性边界清晰

EventLoop 已区分：

- active Channel；
- timer；
- control source；
- lifecycle hub；
- normal functor；
- quiescing/final drain。

IOCP final drain 会继续零超时轮询，直到已接受 functor、控制/lifecycle 信号和 completion obligations 收敛。各 ready-source 又有明确的 per-turn budget，避免一个来源无限占用 loop。

### 5.3 IOCP 数据路径已不再停留在功能可用层面

当前实现包含：

- `GetQueuedCompletionStatusEx` bounded batches；
- wakeup pending coalescing；
- bounded AcceptEx pool；
- stable segmented write ownership；
- partial write continuation；
- 按需固定 read storage；
- exact completion obligation；
- accept/connect cancellation drain。

对应 contract tests 在当前 Windows Release 构建中通过。

### 5.4 内存治理已形成层级模型

已确认：

- TCP output 采用 connection → loop → server → optional global 的原子 reservation hierarchy；
- 后级拒绝会同步回滚前级；
- close、queue rejection、write completion 会释放 reservation；
- Buffer、PacketFramer 和 IOCP read storage 有 retention/recovery 机制；
- Broadcast 有 owner task/bytes 与 global bytes 的 exact-once 记账；
- 慢读与恢复场景已有版本化 capacity profile。

这已经显著优于只有单连接 high-water mark 的实现。

### 5.5 Deadline 和 selector 设计保持了 owner-loop 原则

`DeadlineQueue`：

- 不持有用户回调或目标对象；
- owner-loop-only；
- generation-safe；
- bucketed、budgeted；
- 不为每个连接/session 创建一个 TimerQueue callback。

EventLoopThreadPool 已有 round-robin、least-connections、queue-lag 和 stable rendezvous hashing，且 selector 输入保持为 owner-thread snapshot。

## 6. 主要发现

### P1-01：worker 建连投递失败会破坏 owner-loop 析构规则

状态更新（2026-08-03）：下述内容保留为原始问题证据。首次候选 `446f86d` 修复了 queue-rejection 的 off-owner release，但独立审查又发现 construction-failure fd 双重所有权，因此拒绝关闭。后续 remediation 已在工作树通过确定性故障注入与双平台验证；新的 clean candidate SHA 绑定和独立复审仍是关闭条件。

证据：

1. `TcpServer::newConnection()` 在 base loop 选择 `ioLoop`。
2. 它在 base loop 构造一个归属于 `ioLoop` 的 `TcpConnection`，插入 base map 并提交 load/admission。
3. `connectEstablished()` 通过 `ioLoop->runInLoop()` 投递。
4. worker 的 normal + reserve functor queue 满时，`runInLoop()` 抛出 `overflow_error`。
5. catch 分支只有在 `ioLoop == loop_` 时调用 `connectDestroyed()`。
6. worker 情况下，base map 被删除，最后的 `shared_ptr`/functor 在 base thread 解引用，`TcpConnection` 因而可在非 owner 线程析构。

这与以下明确合同冲突：

- `rules/thread_affinity_rules.md`：connection establishment 和 destruction 都是 owning-EventLoop-thread-only；
- `intents/modules/tcp_server.intent.md`：`connectEstablished/connectDestroyed` 必须运行在 owning connection loop。

当前对象尚未注册 Channel，因此本轮没有证明该路径必然导致崩溃或 UAF；但它已经是明确的线程/生命周期合同违反，并会使未来构造期资源变化变得危险。

现有 `test_tcp_server_saturation_shutdown` 验证 stop saturation，不覆盖“accepted connection establishment queue saturation”。

关闭条件：

- intent、规则和 failure semantics 先定义投递失败时 fd、map、load、admission 的唯一所有者；
- 建连投递失败不能依赖已经饱和的 normal/reserve queue 做清理；
- `TcpConnection` 不得在 owner loop 之外析构；
- 新增确定性 saturation contract，验证 exact close/rollback、无连接回调、计数归零、server 后续仍可服务并可正常 stop。

### P1-02：当前 SHA 没有发布级同 SHA 证据

被拒候选 `446f86d` 的同 SHA remote CI 已提供 Linux/Windows
Debug/Release、Linux ASan/UBSan、Linux TSan 和 install consumer 的成功
producer 结果。但这些结果不能替代新 remediation 候选的以下发布证据：

- Linux CMake Debug/Release；
- Linux ASan/UBSan；
- Linux TSan threading set；
- Windows Debug/Release clean build；
- install/package consumer；
- paired Linux epoll / Windows IOCP benchmark；
- capacity gate；
- 当前 SHA 的 24/72 小时 endurance；
- retained evidence manifests/artifacts。

当前证据缺口包括：

- `446f86d` 已被独立审查拒绝，所有 candidate-bound 结果都必须在新 SHA 重建；
- attempt 3 的六个 producer 均成功，但 artifact 下载为 0/6，聚合验证明确跳过；
- 新 remediation 尚无 remote CI、paired benchmark/capacity 或 24/72 小时 endurance；
- retained manifest、run identity、参数和 artifact hash 尚未形成完整证据链。

关闭条件是新候选 SHA 的必需 jobs 全绿，并且 manifest、candidate SHA、run identity、参数和 artifact hash 完整一致。

### P1-03：许可证阻塞外部采用

当前 `LICENSE` 明确写明：

> No license is granted unless a license notice is added by the project owner.

因此，构建、测试和内部候选工作可以继续，但不能把项目描述为可供外部用户合法使用、修改或分发的开源/可采用发行版。

关闭条件：

- 项目所有者选择并发布明确许可证；
- README、package metadata、SBOM/third-party notices 与许可证保持一致；
- 发布审查确认源码、二进制包和依赖许可链。

### P1-04：0.3 stable Core 表面需要独立审查

当前 public API diff 相对 `v0.2.0-phase4-preview`：

- 新增 10 个 `stable_core` headers；
- 新增 4 个 provisional headers；
- 17 个既有 stable headers 指纹变化；
- 无 header 删除；
- 无 target 增删；
- compatibility line 从 0.2 变为 0.3，因此不要求同线兼容决定；
- `stable_surface_review_required=true`。

这不是自动判定的兼容性失败，但变化面积足够大，不能只依赖 manifest guard 自行批准。

独立审查至少要覆盖：

- ownership、thread-affinity、re-entry 和 failure result；
- options 的默认值及非法值；
- raw pointer/length API 的有效区间前置条件；
- stable/provisional 分类；
- 0.3 源兼容承诺与 ABI 非承诺；
- install consumer 和公开示例。

### P2-01：EventLoopThreadPool 非法状态转换没有完整拒绝

当前实现：

- `setThreadNum(int)` 仅赋值；
- 不拒绝负数；
- 不断言 base-loop thread；
- 不拒绝 started 状态下修改；
- `start()` 直接设置 `started_=true`，不拒绝重复 start。

负线程数并不等价于合法的 zero-thread 模式：worker 循环不启动，同时 `numThreads_ == 0` 分支不会执行 base-loop init callback。用于 TcpServer 时，这会跳过 base-loop output budget/stop participant 初始化，并可在首个连接上触发“selected EventLoop without an output-memory budget”的 fatal path。

重复 `EventLoopThreadPool::start()` 又会追加 workers、重置 load accounting，并破坏“policy/configuration immutable after start”的 intent。

关闭条件：

- 明确 `numThreads >= 0`；
- 配置操作断言 base owner；
- started 状态下的 thread count/policy 修改显式失败；
- start-after-start 显式失败，stop 后 restart 仍按现有 contract 允许；
- 增加 negative、wrong-thread、late-config、repeated-start contract。

### P2-02：路线图文档存在当前性漂移

`docs/migration_status.md` 顶部仍写着 `Current production-roadmap audit: 2026-07-27`，而当前 HEAD 为 2026-07-30。旧 `assessment.md` 也仍以 `e24c8476` 为基线，旧 `plan.md` 同时存在：

- 顶部把内存治理列为未完成；
- 详细 M3-Q1 又已全部完成；
- 总 milestone checklist 仍未同步关闭。

本轮已通过完全重写 `assessment.md` 和 `plan.md` 消除后两项漂移；`docs/migration_status.md` 仍需在下一轮 remediation 后同步，而不是继续叠加历史“current”段落。

关闭条件：

- migration status 更新到新候选 SHA；
- 当前结论与历史证据分区；
- finding ID、plan task 和关闭证据一一对应；
- 守卫继续从仓库事实推导数字，不复制易漂移的手工清单。

## 7. 已知边界，不作为本轮缺陷

以下是有意的 scope/promotion 边界：

- `MetricsExporter` 的 reference implementation 会分配、哈希并竞争 mutex；intent 已明确其为 provisional、opt-in、非生产热路径。
- TcpServer 当前只有 unauthenticated deadline，没有一般 connection read-idle policy；intent 已明确后者尚未 promoted。
- Packet/Gateway、HTTP、WebSocket、RPC、UDP、KCP、TLS 和完整 game-server pipeline 不属于当前稳定 Core 范围。
- 上层 Protocol/Transport/Session/Logic/Broadcast 是可测试 foundation，不等于正式 Gateway 产品层。

在核心 remediation 与候选发布门关闭前，不应靠扩展范围掩盖当前 P1。

## 8. 性能、容量与安全判断

### 8.1 可以确认

- admission、per-turn drain、timer/deadline、TCP output、Broadcast outstanding 和 retention 都有显式有限边界；
- Windows Release 119 tests 没有暴露当前实现回归；
- benchmark/capacity 工具及其 schema/negative fixtures 通过仓库守卫；
- 旧 SHA 已有历史 benchmark/endurance 资产，可用于方法校验。

### 8.2 不能确认

- 当前 SHA 相对冻结 baseline 无性能回归；
- 当前 SHA 在 Linux/epoll 与 Windows/IOCP 上达到同一容量目标；
- 10k/100k/更高连接或 session 规模已经通过 promotion gate；
- 当前 SHA 已通过 24/72 小时 endurance；
- metrics enabled 的 owner-loop 开销已达到生产要求。

### 8.3 安全边界

当前核心具有：

- 有界输入、输出、队列、deadline 和 peer tracking；
- callback exception containment；
- explicit close reasons；
- graceful/forced shutdown convergence；
- scope guard 阻止 deferred 协议模块意外进入当前构建。

但它不是应用安全系统：

- 不提供认证协议、授权、TLS、业务限流策略或完整 DoS 防护；
- unauthenticated deadline 只是基础 admission primitive；
- 最终安全结论必须由使用该核心的 Gateway/业务层共同完成。

## 9. 审计限制

- 原始审计时 P1-01 仅来自静态路径与合同交叉审计；2026-08-03 remediation 已用 deterministic saturation test 动态复现旧路径并验证修复。
- 被拒候选 `446f86d` 已完成 Windows/IOCP 与 WSL2 Linux/epoll 的 Debug/Release、sanitizer、install consumer 和六个同 SHA remote producer；artifact 聚合为 0/6，不构成 retained aggregate evidence。
- construction-failure remediation 已绑定 `95a6ab5`，完成独立 clean review、双平台 Debug/Release、sanitizer、install consumer 和同 SHA 六 producer CI；retained aggregate artifact 仍缺失。
- 本轮不进行 ABI 检查，也不把 0.x 版本自动视为没有源兼容责任。

## 10. 最终判断

项目已跨过“功能样例库”阶段，核心设计、规则、测试与容量治理具有真实工程深度。过去一轮 M3 工作有效关闭了多个旧风险，尤其是 IOCP final drain、公平性、稳定写所有权、按需读内存和全局内存治理。

当前最重要的事情不是继续扩展协议或 Gateway，而是按以下顺序收口：

1. 补齐 P2-01 的公共状态机合同；
2. 完成 0.3 stable surface 独立审查；
3. 修复 retained artifact 并跑通同 SHA 性能、容量和 endurance；
4. 由项目所有者完成许可证决定。

具体执行拆分、依赖和关闭门记录在新的 `plan.md`。
