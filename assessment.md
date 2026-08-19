# game-net-core 当前项目审计报告

审计日期：2026-08-18；执行策略更新：2026-08-19
受审实现检查点：`669ebb0a7c5c475dea74b12275c66a2ce1876804`
被替代 REL-C1 候选：`v0.3.0-rel-c1-refreeze-4@c061f9967b9481b70b2faf9a8fee24f5a3e72ffc`
报告性质：PERF-R1 probe-lifecycle remediation 审计及后续 continuous-execution 策略；历史证据与当前开发状态分区记录

2026-08-19 的执行决策废止候选冻结作为开发门。REL-C1、REL-V1、REL-V2、PERF-R1
和 END-R1 继续作为历史/持续证据名称，但不再串行阻塞 ARCH-G1、IOE 或 Runtime
Profile 工作。当前执行前沿以 `plan.md` 为准：ARCH-G1 完成后直接进入 IOE-R1，
RTM-R1 合同并行准备。

## 1. 执行结论

`game-net-core` 当前应定义为：

> 已具备较强契约与测试基础的 production-hardening preview，尚不是可宣布稳定或可对外采用的 production release。

本轮没有发现 P0。原审计的 4 项 P1 和 2 项 P2 中，P1-01、P1-04、P2-01、
P2-02 已关闭；2026-08-11 复审新增 P1-05/P1-06，均已在 `9d2a5be`
完成本地关闭。仍开放的是 P1-02 发布证据与 P1-03 许可证：

| ID | 级别 | 结论 | 性质 |
| --- | --- | --- | --- |
| P1-01 | P1（已关闭） | M3-R1 检查点 `95a6ab5` 已修复构造失败 fd 双重所有权，并通过 clean 独立全矩阵审查与同 SHA 六 producer CI | 核心线程/生命周期正确性 |
| P1-02 | P1 | `refreeze-4` 的历史 REL-V1/REL-V2/Core 证据和失败 capacity 样本继续保留；`669ebb0` 已修复 probe lifecycle。当前 main 尚无可推广的完整同提交 CI/performance/capacity/endurance 证据，但该缺口只阻塞 promotion，不阻塞 ARCH-G1/IOE/RTM 开发 | 推广证据 |
| P1-03 | P1 | 顶层许可证没有授予使用、复制、修改或分发许可 | 对外采用阻塞 |
| P1-04 | P1（已关闭） | API-R1 首次拒绝后的八组 blocker 已关闭，独立终审 `APPROVE`，0.3 reviewed surface 有严格 zero-diff gate | API 发布阻塞已解除 |
| P1-05 | P1（本地关闭） | TcpServer owner establishment 抛出时先在 owner 关闭，再经 base lifecycle 回滚 map/load/admission，最终引用回到 owner 释放 | 核心线程/生命周期正确性 |
| P1-06 | P1（本地关闭） | TcpClient 名称/构造/配置/注册失败纳入同一事务，fd 先关闭、请求先释放，终止回调可同步重连 | 客户端请求/所有权正确性 |
| P2-01 | P2（本地关闭） | `12adb00` 的 `EventLoopThreadPool` 非法配置和状态转换已在状态改变前显式拒绝 | 公共契约 |
| P2-02 | P2（已关闭） | roadmap、assessment、plan、README 与 migration status 已统一当前检查点、历史证据和下一任务 | 治理一致性 |

因此：

- 可以继续在任意明确 promotion commit 上执行同提交 CI、性能、容量和 endurance
  验证，但无需提前冻结开发。
- REL-C1 历史候选由 annotated tag `v0.3.0-rel-c1-refreeze-5` 标识；它不是当前
  开发基线或 REL-D1 发布决定。
- 不应宣称 Linux/Windows 双平台已经在当前 SHA 通过发布门。
- 不应宣称已有当前 SHA 的容量上限、性能无回归或 24/72 小时稳定性结论。
- P1-01 已在 `95a6ab5` 关闭；`446f86d` 的拒绝结论保留为历史证据。
- `9d2a5be` 仍是最初 API-R1 不可变 reviewed-surface 检查点；PERF-R1 的单一
  additive stable API 由 `api-r1-perf-r1-reviewed-surface@6b292156e3e94d3389e9f3b8513445e7eb4ab541`
  记录为 source-compatible。历史 `CANDIDATE_SHA` 由 freeze tag object 与远端
  ref 记录；未来 evidence 直接绑定被验证的 commit。
- API-R1 只批准 stable surface；它本身不替代
  同 SHA 远端、性能、容量或 endurance 证据。

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

直接关闭合同见
[`test_tcp_server_establishment_saturation.cpp`](tests/contract/tcp_server/test_tcp_server_establishment_saturation.cpp)
与其中的 deterministic construction-failure hook 覆盖。

P1-01 已关闭。批准范围只覆盖 M3-R1 生命周期/所有权问题，不扩展到
0.3 stable API、retained artifact、性能、容量、endurance 或最终发布决定。

Linux 命令、逐项审阅矩阵和签字字段已冻结在
`docs/reviews/m3-r1-closure-review.md`；该文件保留首次拒绝历史、remediation
证据、新候选 clean review 和最终签核。

### 1.2 M3-R2 本地关闭检查点（2026-08-05）

提交 `12adb00` 已把 `EventLoopThreadPool` 配置生命周期明确为
`Idle -> Started -> Idle`，并在任何状态改变前拒绝负线程数、非 base-loop
线程配置、started 状态配置以及重复 `start()`。`stop()` 在 Idle 保持幂等，
合法 stop 后仍可重新配置并重启；zero-thread 模式仍执行一次 base-loop init
callback，partial-start rollback 合同保持不变。`TcpServer::setThreadNum()` 同步
转发上述失败结果。

新增负向合同在修复前确定性失败；修复后 Windows/IOCP Release 全量
120/120、36/36 repository/API/CI guards 通过，EventLoopThreadPool 主合同、
restart soak 与 TcpServer 合同各重复 50 次，共 150/150 通过。该结论关闭
P2-01 的本地实现与合同范围；该检查点尚未冻结为新候选 SHA，最终候选仍须
执行 clean checkout 与同 SHA 远端复验。

### 1.3 API-R1 stable Core 关闭（2026-08-05）

独立 reviewer `/root/api_r1_independent_review` 以只读方式审查完整 stable
surface。首次结论为 `REJECT`，覆盖 typed shutdown/admission、stable 与
platform-internal 分类、配置阶段、callback/destruction/ownership、raw range、
options、source compatibility 和 install consumer 八组 blocker。全部修复后，
reviewer 多轮复核并最终给出 `APPROVE`，允许进入 REL-C1。

确定性证据：

- 历史 diff 相对 `v0.2.0-phase4-preview` 为 10 个 stable additions、4 个
  provisional additions、19 个 stable fingerprint changes，无删除、移动或
  target 变化；
- 0.3 reviewed-baseline diff 的 header/target/fingerprint changes 全为空，
  `same_compatibility_line=true`、`has_changes=false`；
- stable header 与 stable target additive drift 的负向注入均以 exit 3 被
  blocking gate 拒绝；
- Windows/IOCP Release 120/120、focused 8/8、fresh stable/provisional
  install consumers 2/2 通过；
- ABI 仍不承诺；direct TimerQueue application surface 等 0.2→0.3 有意 break
  已在兼容策略和审查档案中列明。

完整矩阵、首次拒绝、逐项 resolution、历史及同线 diff 见
`docs/reviews/api-r1-stable-core-review.md`。REL-C1 已把 snapshot 绑定到
annotated tag `api-r1-approved-surface` 与 peeled commit `9d2a5be...`，并以
`v0.3.0-rel-c1-refreeze-1` 的 peeled commit 作为非自引用的最终候选身份。
旧 tag `v0.3.0-rel-c1-freeze` 及其 `d3137f9` commit 只保留为被替代候选谱系。

### 1.4 Post-review TCP establishment remediation（2026-08-11）

复审在 API-R1 之后识别出两个 late failure 缺口：TcpServer 的 normal queue
已经接纳 establishment 后，`TcpConnection::connectEstablished()` 仍可能因
lifecycle-node 容量/分配失败抛出，旧实现会遗留 base map、selector load 与
active admission；TcpClient 在 `TcpConnection` 构造或 callback setup 抛出时，
旧实现不会释放 active connect request，后续 `tryConnect()` 只会被错误合并。

`9d2a5be` 的关闭结果：

- TcpServer rollback record 在 queue admission 后进入
  `AwaitingOwnerEstablishment`；失败时 owner 先执行 `connectDestroyed()`，base
  lifecycle 只回滚内部 map/load/admission，随后由 owner 回收最终引用；
- TcpClient connected-fd receiver 的名称分配、构造、配置、publication、IOCP
  association 与 Channel 建立共享异常边界；终止通知前关闭 sole fd owner 并
  释放 request，回调内同步重连形成新 Connector generation；
- `test_tcp_server_establishment_saturation.cpp` 以可释放的 lifecycle 容量占位
  证明失败账务归零、无 connection callback、同一 server 恢复健康建连；
- `test_tcp_client_contract.cpp` 注入 socket-claim 前构造失败，证明 terminal
  callback 内重连被接纳且旧 Connector settlement 不覆盖新尝试；
- 当前提交通过 Windows/IOCP Release 120/120、36/36 repository/API/CI guards、
  install consumers 2/2，API-R1 reviewed-surface diff 严格为空。

这些是本地检查点证据；`9d2a5be` 尚未获得同 SHA Linux/Windows remote、
paired benchmark/capacity 或 24/72 小时 endurance，因此不自动成为候选。

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
- 已版本化的 GitHub Actions CI、benchmark、capacity 与 endurance 证据记录。

GOV-R2 只同步治理文档和对应静态守卫，没有修改运行时代码、C++ 合同测试或
构建配置。

## 3. 冻结证据快照

### 3.1 Git 与版本

- 受审实现分支：`perf-r1-deterministic-capacity`
- 受审实现检查点：`669ebb0a7c5c475dea74b12275c66a2ce1876804`
- 被替代 REL-C1 候选：`v0.3.0-rel-c1-refreeze-4@c061f9967b9481b70b2faf9a8fee24f5a3e72ffc`
- 实现检查点提交时间：`2026-08-18T00:19:18+08:00`
- 实现检查点标题：`Make healthy probe lifecycle accounting reachable`
- 最新已发布标签：`v0.2.0-phase4-preview`
- 实现检查点位于该标签之后 78 个提交
- CMake package version：`0.3.0`
- 语言标准：C++23，关闭 compiler extensions
- 历史 v0.3 工程候选：annotated tag `v0.3.0-rel-c1-refreeze-5` peeled commit；
  完整 SHA 由 tag object 与远端 ref 记录，但不再冻结当前开发

本报告原始审计基线 `7a56132d6ea60346ec06c108cd627b7b4cd5a04f` 到
受审实现检查点：

- 20 个提交；
- 129 个文件变化；
- 11,405 行新增，1,980 行删除。

这 20 个提交完成：

- M3-R1 建连投递失败的 owner-loop rollback 合同与实现；
- TcpConnection partial-construction fd exact-once remediation；
- M3-R1 独立审查和同 SHA 六 producer 记录；
- M3-R2 EventLoopThreadPool 状态机与 TcpServer 转发负向合同；
- API-R1 stable surface remediation、独立批准和同线 zero-diff gate；
- post-review TcpServer/TcpClient establishment failure rollback；
- 长期架构目标与候选冻结治理同步；
- aggregate verifier 的 two-consumer 合同修复与回归守卫；
- PERF-R1 comparator/high-fd/profile remediation、直接 socket-option 合同与
  跨平台 candidate-10k 三重复本地 preflight；
- remote-evidence remediation：annotated-tag checkout、warm paired/interleaved
  sampling、owner-batched retention snapshot、完整 JSON flush 与 stderr byte
  preservation；随后补充 checkout 后恢复远端 annotated tag object 的步骤和静态合同；
- probe-lifecycle remediation：先保持一批已连接 socket 存活并等待 server accept
  收敛，再执行 exact echo、abortive close 和 server close 收敛。

`95a6ab5` 是 M3-R1 已独立审查的历史检查点；`12adb00` 在其上增加 M3-R2，
`7fa6922` 提交 API-R1 remediation，`9d2a5be` 再增加 late failure 修复，
`68b444d` 修复 aggregate verifier，`6b29215` 修复 PERF-R1 evidence/profile
determinism。因此任一早期检查点的 CI、性能、容量或 endurance 结果不能自动提升
为当前证据。

### 3.2 项目清单

| 项目 | 当前事实 |
| --- | ---: |
| 正式 intent | 61 |
| active intent | 30 |
| deferred intent | 20 |
| legacy intent | 11 |
| intent 显式 verification paths | 140 |
| public headers | 55 |
| `.cc` sources | 40 |
| CTest tests | 121 |

测试分区：

- unit：8；
- contract：100；
- integration：13。

交叉标签中包括：

- threading：94；
- lifecycle：99；
- game pipeline：7；
- broadcast：5。

### 3.3 本地验证

PERF-R1 probe-lifecycle 提交前在同一实现树的 Windows/IOCP 与 WSL Linux/epoll
Release 构建中执行了直接合同、守卫和完整 candidate-10k profile。关键结果：

```text
cmake --build build-perf-r1-fix --config Release --parallel
ctest --test-dir build-perf-r1-fix -C Release --output-on-failure
```

结果：

- Windows/IOCP 与 Linux/epoll Release 容量目标增量构建成功；
- Windows Release 全量 CTest 为 121/121，0 failed；
- probe lifecycle 顺序静态合同与其余 pre-migration repository guards 通过；
- candidate-10k 在 Windows 连续九次、Linux 三次通过，全部 healthy probes、reader recovery、
  pending-output drain 和 typed overload 归因均满足 v3 validator；
- Windows 每次 8,252 accepted / 1,748 `EndpointOverloaded`，Linux 每次
  8,000 / 2,000。

证据边界：

- 这是提交前完整构建树上的跨平台 preflight，不是新候选 detached clean gate；
- capacity 目录使用 synthetic candidate identity，不是 release evidence；
- 新候选仍须依次完成 REL-V1、REL-V2 与 PERF-R1。

### 3.4 本机 Windows 工具链

当前 `vswhere` 报告：

- Visual Studio Professional 2026 `18.8.2`；
- 路径：`D:\VS2026`；
- `isComplete=true`；
- `isLaunchable=true`；
- `isRebootRequired=false`。

这说明早先 self-hosted Windows run 中的 C++ tool detection 阻塞在本机现状上已消失，但尚未通过新的 Actions run 形成远端证据。

### 3.5 当前远端证据状态

当前证据边界按不可变提交区分：

- `95a6ab5` 的 M3-R1 独立 clean review 已批准；GitHub Actions run
  `30813037693` attempt 2 的 Linux Debug/Release/TSan/ASan-UBSan 与 Windows
  Debug/Release 六个 producer 均成功；
- 同一 run 的 aggregate job 虽为 success，但 retained artifact API 返回 0，
  聚合上传/校验步骤未形成可下载证据，因此 P1-02 仍开放；
- 被替代候选 `v0.3.0-rel-c1-freeze@d3137f9298b47474ea96dc694d44c5c026710039`
  的 run `31992899968` attempt 1 已形成六个成功 producer artifact、消费者
  2/2/2、TSan 93 与 libFuzzer 1000，但 aggregate verifier 错误期望一个
  consumer，故 run conclusion 为 failure，不能关闭 REL-V2；
- `refreeze-1@944f7222d7aa7a36e12ffda4ad038ec3ae7d30d7` 的 REL-V1 与 REL-V2
  run `32007753147` attempt 4 成功，但 PERF-R1 runs `32027919772` /
  `32027919807` 失败，因此只能保留为被替代候选的历史证据；
- `refreeze-2@f528898a2d688be329cf0dce4b167ffe0fad5647` 的本地 REL-V1 成功，但
  REL-V2 run `32034140286`、Core run `32034143490` 和 capacity runs
  `32034147244` / `32035475245` 暴露 evidence-tool 缺陷，不能晋级；
- `refreeze-3@0a500826844cb4f9345572909a733cc2e52ce14c` 的本地 REL-V1 完成
  Windows Debug/Release 121/121、install consumers 2/2/2、focused repeats
  800/800、36/36 guards、同线 API zero diff 和 Linux Release 121/121；REL-V2
  run `32039657783` 在 repository guards 前把本地 annotated tag ref 扁平化，
  该 run 取消且不能晋级；
- `refreeze-4@c061f9967b9481b70b2faf9a8fee24f5a3e72ffc` 已完成 detached clean
  REL-V1、REL-V2 run `32043448820`（六 producer + aggregate 全部 success）和
  paired Core run `32043874669`（Linux/Windows producer + aggregate 全部 success）；
  capacity run `32043877128` attempts 1/2 的 Linux producer 均成功，Windows
  分别在第 370/420 个探针出现一次 receive failure 后等待 accept 超时。失败 JSON
  均保留，不能以重跑覆盖；
- 当前 `refreeze-5` 尚无 fresh same-SHA REL-V1、REL-V2、paired benchmark/
  capacity 或 24/72 小时 endurance；
- `be749ad`、`5f926f3` 与 `b344318` 的性能/容量/endurance 结果仅是历史
  基础设施证据，不是 REL-C1 冻结候选的当前证据；
- REL-C1 当前候选身份只由 `v0.3.0-rel-c1-refreeze-5^{commit}` 解析；旧 tag
  `v0.3.0-rel-c1-refreeze-4`、`v0.3.0-rel-c1-refreeze-3`、`v0.3.0-rel-c1-refreeze-2`、`v0.3.0-rel-c1-refreeze-1` 与
  `v0.3.0-rel-c1-freeze` 只记录被替代候选。

远端基础设施状态可能独立变化；本报告不把未重新查询的 runner 在线状态或
旧 queued/failed run 描述为当前事实。发布门只接受最终候选 SHA 上可下载、
可校验且 identity/hash 完整的 evidence set。

## 4. 成熟度判断

| 维度 | 当前判断 |
| --- | --- |
| Intent/规则治理 | 强；清单、语义、依赖与 provenance 已有自动守卫 |
| Reactor/TCP 核心 | 强 preview；状态机、owner-loop 和 shutdown 合同密度高 |
| Windows IOCP | 已完成关键正确性和性能底座切片，本地 Release 合同通过 |
| Linux epoll | PERF-R1 remediation 已完成 Release 构建、直接合同、owner-batched/probe-barrier capacity 与 candidate-10k 三重复 preflight；后续按每个里程碑生成 exact-commit CI/性能证据 |
| 公平性/容量 | 已有显式批次、队列、deadline、内存预算和 retention 边界 |
| 上层 foundation | Protocol/Transport/Session/Logic/Broadcast 可测试，但仍是 foundation |
| Metrics | 有结构化接口和测试，明确为 provisional、非生产热路径实现 |
| API/安装 | 0.3 manifest 和 diff 完整；API-R1 已批准，`9d2a5be` 同线 diff 严格为空；PERF-R1 additive surface 已记录 source-compatible |
| 发布可采用性 | `refreeze-5` 只保留为历史工程候选；promotion 仍被完整同提交证据和许可证阻塞，开发不受阻塞 |

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

关闭状态（2026-08-03）：下述内容保留为原始问题证据。首次候选
`446f86d` 修复了 queue-rejection 的 off-owner release，但独立审查发现
construction-failure fd 双重所有权并拒绝关闭。remediation `95a6ab5` 把 fd
claim 移到全部可抛构造步骤之后，新增确定性故障注入，并通过 clean 独立
全矩阵复审；P1-01 已关闭。后续 `12adb00`、`7fa6922` 与 `9d2a5be`
均保留该 partial-construction exact-once 所有权边界。

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

已批准的 M3-R1 检查点 `95a6ab5` 已取得 Linux/Windows Debug/Release、
Linux ASan/UBSan、Linux TSan 和 install consumer 的六个成功 producer，
但 aggregate retained artifact 缺失。后续 `12adb00`、`7fa6922` 与 `9d2a5be`
又改变运行时代码，因此
最终 v0.3 候选仍缺少以下同 SHA 发布证据：

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

- `95a6ab5` 的 run `30813037693` attempt 2 六个 producer 均成功，但 retained
  artifact 下载为 0，聚合上传/校验没有形成可复核资产；
- `9d2a5be` 尚无 remote CI、paired benchmark/capacity 或 24/72 小时 endurance；
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

### P1-04：0.3 stable Core 表面独立审查（已关闭）

当前 public API diff 相对 `v0.2.0-phase4-preview`：

- 新增 10 个 `stable_core` headers；
- 新增 4 个 provisional headers；
- 19 个既有 stable headers 指纹变化；
- 无 header 删除；
- 无 target 增删；
- compatibility line 从 0.2 变为 0.3，因此不要求同线兼容决定；
- `stable_surface_review_required=true`。

跨线 diff 本身不是自动判定的同线兼容失败，但变化面积足够大，因此已由独立
reviewer 审查，而不是由 manifest guard 自行批准。

独立审查至少要覆盖：

- ownership、thread-affinity、re-entry 和 failure result；
- options 的默认值及非法值；
- raw pointer/length API 的有效区间前置条件；
- stable/provisional 分类；
- 0.3 源兼容承诺与 ABI 非承诺；
- install consumer 和公开示例。

关闭结果（2026-08-11 当前性复核）：

- 独立首次审查 `REJECT`，八组 blocker 均有对应 intent/合同/测试/实现修复；
- 最终独立结论 `APPROVE`，无剩余实现/合同 blocker；
- core-only consumer 覆盖全部 stable headers，provisional consumer 独立；
- CMake `SameMinorVersion` 与 0.3 正向、0.2/0.4 负向版本 probe 可执行；
- reviewed snapshot 与当前 manifest 严格 zero diff；stable additions 和
  fingerprint drift 都会被 blocking gate 拒绝；
- P1-04 关闭，但最终 SHA 绑定、远端证据和发布决定仍分别属于
  REL-C1、REL-V/PERF/END 与 REL-D1。

### P2-01：EventLoopThreadPool 非法状态转换没有完整拒绝

原审计实现：

- `setThreadNum(int)` 仅赋值；
- 不拒绝负数；
- 不断言 base-loop thread；
- 不拒绝 started 状态下修改；
- `start()` 直接设置 `started_=true`，不拒绝重复 start。

负线程数并不等价于合法的 zero-thread 模式：worker 循环不启动，同时 `numThreads_ == 0` 分支不会执行 base-loop init callback。用于 TcpServer 时，这会跳过 base-loop output budget/stop participant 初始化，并可在首个连接上触发“selected EventLoop without an output-memory budget”的 fatal path。

重复 `EventLoopThreadPool::start()` 又会追加 workers、重置 load accounting，并破坏“policy/configuration immutable after start”的 intent。

关闭条件：

- [x] 明确 `numThreads >= 0`；
- [x] 配置操作断言 base owner；
- [x] started 状态下的 thread count/policy 修改显式失败；
- [x] start-after-start 显式失败，stop 后 restart 仍按现有 contract 允许；
- [x] 增加 negative、wrong-thread、late-config、repeated-start contract。

关闭结果（2026-08-05）：

- `setThreadNum()` 以 `std::invalid_argument` 拒绝负数，以现有 owner-loop
  断言拒绝跨线程调用，并以 `std::logic_error` 拒绝 started 状态修改；
- `start()` 在 already-started 时以 `std::logic_error` 失败，且不会追加
  worker、重置 load accounting 或重复发布 init callback；
- `getAllLoops()` 与选择/load-accounting 操作一致要求 base-loop thread；
- [`test_event_loop_thread_pool.cpp`](tests/contract/event_loop_thread_pool/test_event_loop_thread_pool.cpp) 覆盖
  negative、wrong-thread、late-config、repeated-start、zero-thread 和合法 restart；
- [`test_tcp_server_contract.cpp`](tests/contract/tcp_server/test_tcp_server_contract.cpp) 覆盖 TcpServer 的
  negative 与 late thread-count 转发；
- 修复前新增合同失败，修复后 Release 全量 120/120、守卫 36/36，聚焦
  重复 150/150。

### P2-02：路线图文档存在当前性漂移

原始问题是 `docs/migration_status.md` 的 current audit 日期、assessment 基线、
plan 完成项和 roadmap 候选描述分属不同时间点，且 `be749ad`、`b344318` 等
历史证据一度与当前检查点混写。

关闭结果（2026-08-05）：

- roadmap、assessment、plan、README 和 migration status 统一使用已提交实现
  检查点 `9d2a5be`；
- 当时的上游参考 `7fa6922`、M3-R1 已审查检查点 `95a6ab5` 与“尚未冻结
  v0.3 最终候选”被明确区分；
- 当时测试清单统一为 120（8 unit、99 contract、13 integration；93
  threading、98 lifecycle）；当前 PERF-R1 清单已增至 121/8/100/13，历史
  GOV-R2 数字不冒充当前库存；
- `be749ad`、`5f926f3`、`b344318` 的 benchmark/capacity/endurance 结果只在
  historical evidence 范围出现；
- P1-01/P2-01 的关闭证据链接到具体合同；GOV-R2 当时将 API-R1 设为下一
  任务；当时 API-R1 关闭后曾把 REL-C1 作为唯一下一任务，该顺序已被 2026-08-19
  continuous-execution 决策替代；
- migration-status guard 同时检查四份治理文档与 README 的检查点和候选边界。

关闭门：

- [x] migration status 更新到已提交实现检查点，并明确当前无最终候选；
- [x] 当前结论与历史证据分区；
- [x] finding ID、plan task 和关闭证据一一对应；
- [x] 守卫继续从仓库事实推导 intent/test 数字并交叉检查治理文档。

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
- Windows/IOCP Release 120/120 没有暴露 `9d2a5be` 实现回归；
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

当前最重要的事情不是继续扩展协议或 Gateway，也不是再次冻结候选，而是向长期架构
目标形成连续可运行切片：

1. 在两个工作日内完成 ARCH-G1 的 active architecture intents、ADR、耦合清单、
   基线和具体测试地图；
2. 直接实现 IOE-R1 source-private Engine seam 与最小 Poller adapter；
3. 并行准备 RTM-R1 三个 TCP-only provisional Profile 的合同；
4. 把 CI、benchmark、capacity 作为伴随证据；只有准备 promotion 时才执行 endurance、
   许可证和 REL-D1。

具体执行拆分、依赖和关闭门记录在新的 `plan.md`。
