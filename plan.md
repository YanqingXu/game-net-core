# game-net-core 执行计划

计划日期：2026-07-30
更新日期：2026-08-17
起始基线：`main@7a56132d6ea60346ec06c108cd627b7b4cd5a04f`
依据：同基线的 `assessment.md`

当前治理事实（2026-08-17）绑定不可变实现检查点
`68b444dd109562d3d69c2b377c3bec90dcd15779`。REL-C1 替代候选由 annotated
tag `v0.3.0-rel-c1-refreeze-1` 唯一标识，其 peeled commit 是权威
`CANDIDATE_SHA`。它替代 `v0.3.0-rel-c1-freeze` peeled commit
`d3137f9298b47474ea96dc694d44c5c026710039`；旧候选 run `31992899968`
attempt 1 的六个 producer 成功，但 aggregate verifier 仍错误地期望一个
install consumer，因此不能作为 REL-V2 成功证据。

替代候选的 peeled commit 是
`944f7222d7aa7a36e12ffda4ad038ec3ae7d30d7`。REL-V1 已从该 SHA 的 detached
clean worktree 完成本地门；REL-V2 已由 GitHub Actions run `32007753147`
attempt 4 完成六个 producer、required artifacts 和 aggregate verifier，全部
结论为 `success`。当前发布关键路径已推进到 PERF-R1。

PERF-R1 首次远端执行已在同一候选 SHA 上完成，但门禁结论为 `failure`：
Core benchmark run `32027919772` 与 capacity run `32027919807` 都未产生可接受的
paired success evidence。当前候选因此仍被冻结但不可晋级；END-R1 未获准启动。
本地已准备 evidence-tooling/harness remediation，任何采纳后的新提交都会按本计划
规则使候选任务退回 REL-C1，并需要新的不可变候选 tag 以及 REL-V1/REL-V2/PERF-R1
全链重验。

长期方向见 `goal.md`。`goal.md` 只定义目标和边界，不授权实现；模块实现仍须由
`active` intent、rules、contracts、tests 和当前阶段 gate 共同授权。

## 1. 本轮目标

本轮只做 production-hardening remediation、候选冻结和发布证据收口：

1. 关闭建连失败回滚的 owner-loop 生命周期缺口；
2. 补齐 EventLoopThreadPool 非法配置/状态转换合同；
3. 完成 0.3 stable Core 独立审查；
4. 冻结新的唯一候选 SHA；
5. 生成同 SHA 的 Linux/Windows CI、性能、容量和 endurance 证据；
6. 给出 `v0.3.0-production-candidate` 是否可发布的明确结论。

在这些门关闭前，不启动正式 Gateway、HTTP、WebSocket、RPC、UDP、KCP、TLS 或新 game pipeline 模块。

本轮也不重构已通过 API-R1 的稳定 `EventLoop/Poller/Channel` surface。长期的
owner-loop + I/O Engine 与多运行模型目标只进入 REL-D1 之后的计划队列，不能抢占
REL-C1 或使当前候选证据失效。

## 2. 当前检查点

### 2.1 已完成

- [x] 重新审计 `7a56132d`；
- [x] 清空并重写 `assessment.md`；
- [x] 以 open findings 重建本计划；
- [x] 起始基线 Windows Release 增量构建成功；
- [x] 起始基线 119/119 CTests 通过；
- [x] 36/36 repository/API/CI guards 通过；
- [x] 当前 API diff 已确认需要 stable-surface review。
- [x] M3-R1 已实现并完成 Windows Debug/Release 120/120 与 focused 50/50 本地验证；
- [x] M3-R1 已完成 WSL2 Linux/epoll Debug/Release 120/120 与 focused 50/50 本地验证；
- [x] M3-R1 inventory 已同步为 120 tests、99 contract、93 threading、98 lifecycle、138 intent verification paths。
- [x] 首次独立 review 拒绝 `446f86d` 并定位 construction-failure fd 双重所有权；
- [x] remediation 工作树新增确定性构造失败合同，并重新通过 Windows/IOCP 与 Linux/epoll Debug/Release focused 50/50、全量 120/120 和 36/36 guards。
- [x] 独立 remediation pre-review 结论为 `approve-for-candidate-freeze`，无实现 blocker；
- [x] M3-R1 remediation 已冻结为 clean candidate `95a6ab5`；
- [x] 独立 Codex reviewer 从 clean checkout 完成 11 行矩阵并给出 `approve`；
- [x] 同 SHA CI run `30813037693` attempt 2 的六个 producer 全部成功；aggregate artifact 仍为 0，继续由 P1-02 跟踪。
- [x] M3-R2 已完成 EventLoopThreadPool 状态机合同、负向测试和 Windows/IOCP Release 本地验证：全量 120/120、守卫 36/36、聚焦重复 150/150。
- [x] GOV-R2 已统一 roadmap、assessment、plan、README 与 migration status 的实现检查点、历史证据边界、测试清单和下一执行任务。
- [x] API-R1 已完成独立 stable Core 审查；首次拒绝的八组 blocker 全部关闭，终审 `APPROVE`，历史 diff 与同线 zero-diff 均已归档。
- [x] M3-R3 已在 `9d2a5be` 关闭 TcpServer owner-establishment 失败的 base
  账务泄漏与 TcpClient construction failure 的 active-request 卡死；Windows/
  IOCP Release 120/120、36/36 guards、install consumers 2/2、API 同线 zero
  diff 均通过。
- [x] REL-C1 已以 annotated reviewed-surface tag 和独立 candidate freeze tag
  消除候选 commit 自引用，冻结唯一 v0.3 候选，并把完整 SHA 的权威记录放在
  tag object、远端 ref 与 Actions push identity 中。
- [x] REL-V1 已在 `944f7222d7aa7a36e12ffda4ad038ec3ae7d30d7` 的 detached
  clean worktree 完成 Windows Debug/Release clean build、各 120/120 CTest、
  install consumers 各 2/2、focused repeats 共 700/700、36/36 guards、API
  zero diff 和 inventory evidence；`Not Run` 为 0。
- [x] REL-V2 已由 run `32007753147` attempt 4 完成 Linux Debug/Release/
  ASan/UBSan 各 120/120、TSan threading 93/93、Windows IOCP Debug/Release
  各 120/120、install consumers 2/2/2 和 PacketFramer libFuzzer 1000 units；
  六个 producer artifacts 与 aggregate artifact 均为 required、非空且可下载，
  aggregate verifier 通过。

### 2.2 未完成

- [ ] 同 SHA paired benchmark/capacity（首次 runs `32027919772` / `32027919807`
  已执行并失败，详见 PERF-R1 执行记录）；
- [ ] 同 SHA 24/72 小时 endurance；
- [ ] 许可证决定；
- [ ] 最终 release decision。

## 3. Finding 与任务映射

| Assessment finding | Plan task | 关闭证据 |
| --- | --- | --- |
| P1-01 | M3-R1 | 新 saturation contract、owner-thread 证明、exact rollback |
| P1-02 | INF-R1、REL-V2、PERF-R1、END-R1 | 同候选 SHA 的成功 manifests/artifacts |
| P1-03 | LIC-R1 | 显式许可证及 package/SBOM metadata |
| P1-04 | API-R1（已关闭） | 独立审查记录、批准 manifest、同线 zero-diff gate |
| P1-05 | M3-R3（本地关闭） | owner teardown → base lifecycle rollback → owner final release 合同 |
| P1-06 | M3-R3（本地关闭） | connected-fd transaction、terminal-before-reconnect 请求释放合同 |
| P2-01 | M3-R2 | negative/wrong-thread/late-config/repeated-start contracts |
| P2-02 | GOV-R2（已关闭） | roadmap、migration status、assessment、plan、README 与候选事实一致，静态守卫交叉验证 |

规则：finding 只有在对应关闭证据存在时才能标记完成；代码合并本身不等于关闭。

## 4. 执行顺序

主依赖链：

```text
M3-R1 → M3-R2 → GOV-R2 → API-R1 → M3-R3 → GOV-R3 → REL-C1
                                                        ↓
                         REL-V1 → REL-V2
                                                        ↓
                         PERF-R1 → END-R1 → REL-D1
```

`INF-R1` 和 `LIC-R1` 可与代码 remediation 并行，但不得改变候选证据必须绑定唯一 SHA 的规则。

REL-D1 之后的架构依赖链：

```text
REL-D1
  -> ARCH-G1（目标转 active intent/ADR + 基线证据）
       -> IOE-R1（source-private Engine contract + Poller adapter）
            -> IOE-R2（epoll Readiness Engine）
                 -> IOE-C1（IOCP direct Completion notice）
                      -> IOE-X1（实验性 io_uring）

       -> RTM-R1（TCP-only Runtime Profiles）
            -> RTM-R2（Queued / Tick / Sharding）
                 -> TRN-X1（经 intent 提升后的 deferred transport）
```

`IOE-*` 与 `RTM-*` 是两个正交演进轨道。它们可以在 ARCH-G1 关闭后分别评估，
但任何稳定 Core 修改都必须满足 API 兼容决策、跨平台合同和独立 review；任何
Runtime Profile 都不得反向改变 Core 的 owner/lifecycle 语义。

## 5. M3-R1：建连投递失败生命周期闭环

优先级：最高
对应：P1-01

### 5.1 先更新 intent 和规则

- [x] 在 `intents/modules/tcp_server.intent.md` 定义 accepted fd 的所有权状态；
- [x] 定义 base admission、worker handoff、owner establishment、base map/load commit 的线性化点；
- [x] 定义 queue full、owner shutdown、owner unavailable、allocation failure 的 exact rollback；
- [x] 在 `rules/thread_affinity_rules.md` 保持 `TcpConnection` establishment/destruction owner-loop-only；
- [x] 明确不得通过 normal/reserve functor queue 执行该队列饱和后的唯一清理动作；
- [x] 指定验证文件：
  `tests/contract/tcp_server/test_tcp_server_establishment_saturation.cpp`。

### 5.2 设计门

实现前必须回答：

1. 在 `TcpConnection` 构造前，accepted fd 由谁关闭？
2. worker 接受 handoff 后，哪个事件表示 connection ownership 已建立？
3. base map、selector load、peer/admission deadline 分别在哪一步 commit？
4. 任一步失败时，每个 scope 由谁、在哪个线程回滚？
5. owner queue 已满或已 shutdown 时，如何避免 off-owner `TcpConnection` 析构？
6. close callback 是否可能在 base map commit 前重入？
7. stop 与 establishment handoff 并发时，哪个 generation/状态获胜？

没有书面状态机和失败表，不进入实现。

### 5.3 先写失败合同

- [x] 使用很小的 `EventLoopOptions` 填满 worker normal + reserve queue；
- [x] 保持 worker callback 阻塞，确保新 accepted connection 的 establishment 投递失败；
- [x] 证明 `connectEstablished()` 不运行；
- [x] 证明 `TcpConnection` 不在 base thread 析构；
- [x] 证明 accepted fd exact-once release；
- [x] 证明 base map、selector load、active/per-peer admission 和 authentication deadline 全部回滚；
- [x] 释放饱和后再建立健康连接，证明 server 继续服务；
- [x] 最终 stop/join 收敛且无挂起 Channel/completion；
- [x] Linux/epoll 与 Windows/IOCP 都执行该合同。

推荐新增独立 CTest。若采用该方案：

- 当前总数从 119 变为 120；
- 所有 `verify_ctest_inventory`、workflow evidence command、migration status 和测试分区数字同步更新；
- 守卫不得通过保留旧的 119 字面量来规避新增测试。

### 5.4 实现约束

- [x] 不削弱 owner-loop destruction rule；
- [x] 不把 queue 变为无界；
- [x] 不给业务工作滥用 control/lifecycle lane 的入口；
- [x] 不用 sleep 修复竞态；
- [x] 不吞掉 queue failure；
- [x] fd、connection、map/load/admission 各自 exact-once commit/release；
- [x] Linux 和 Windows 使用同一生命周期语义。

### 5.5 关闭门

Linux 执行与独立签字使用
`docs/reviews/m3-r1-closure-review.md`；该记录必须绑定一个 clean、已提交的
candidate SHA。

- [x] 新合同在修复前可稳定暴露旧路径；
- [x] 修复后 focused repeat 50/50；
- [x] Windows 全量 Debug/Release 120/120 CTests 通过；
- [x] Linux/epoll 全量 Debug/Release 120/120 CTests 通过；
- [x] lifecycle/threading labels 无回归；
- [x] intent、规则、测试和实现的状态名一致；
- [x] 独立 reviewer 未发现 owner 规则被放宽，但拒绝了首次候选的 construction-failure fd exact-once 状态；
- [x] remediation 通过故障注入证明 constructor 抛出时 connection Socket 尚未 claim fd；
- [x] remediation 绑定新 clean SHA 后，独立 reviewer 全矩阵批准。

## 6. M3-R2：EventLoopThreadPool 配置状态机

优先级：高
对应：P2-01
依赖：M3-R1 的 owner/stop 状态决定完成

### 6.1 合同

- [x] `numThreads >= 0`；
- [x] `setThreadNum()` 只允许 base-loop thread；
- [x] thread count 和 selection policy 在 start 后不可修改；
- [x] `start()` 在 already-started 时显式失败；
- [x] `stop()` 后 restart 继续受现有 restart-soak 支持；
- [x] zero-thread 模式必须执行 base-loop init callback；
- [x] partial start failure 仍停止并 join 已发布 workers。

### 6.2 测试

在 EventLoopThreadPool contract 中增加：

- [x] negative thread count；
- [x] wrong-thread `setThreadNum()`；
- [x] late thread-count mutation；
- [x] late policy mutation；
- [x] repeated start without stop；
- [x] stop 后合法 restart；
- [x] zero-thread callback 和 base fallback；
- [x] TcpServer 对非法 thread count 的一致转发/拒绝。

### 6.3 关闭门

- [x] 非法调用在状态改变前失败；
- [x] 不留下半启动 worker；
- [x] connection load accounting 不被重置或泄漏；
- [x] 原有 round-robin/least/queue-lag/hash 与 restart tests 全部通过。

本地关闭证据（2026-08-05）：新增合同先在旧实现上确定性失败；实现修复后，
Windows/IOCP Release 全量 120/120、36/36 repository/API/CI guards 通过，
EventLoopThreadPool 主合同、restart soak 与 TcpServer 合同各重复 50 次，
共 150/150 通过。`12adb00` 尚未冻结为新候选 SHA；该结果不能替代后续
REL-C1/REL-V2 的 clean checkout 与同 SHA 远端发布证据。

## 7. GOV-R2：当前事实与文档同步

优先级：高
对应：P2-02
依赖：M3-R1、M3-R2

- [x] 更新 `docs/migration_status.md` 的 current audit 日期和实现检查点；明确当前没有冻结的最终候选；
- [x] 把历史 benchmark/endurance 证据放入 historical evidence，不描述为当前候选；
- [x] 更新测试总数和 label 数；
- [x] 把 P1-01/P2-01 的关闭证据链接到具体 test；
- [x] 保持 intent inventory 由仓库事实推导；
- [x] 更新 roadmap 与 README 的 candidate 状态，但不提前声明 stable；
- [x] 检查 `assessment.md`、`plan.md`、README、roadmap、migration status 无互相矛盾的“current”结论；
- [x] 运行 `git diff --check` 和全部文档/治理守卫。

关闭证据（2026-08-05）：五份当前入口统一以 `12adb00` 为已提交实现检查点，
以 `95a6ab5` 为 M3-R1 已审查历史检查点，并明确当前没有冻结的 v0.3 最终
候选。测试清单统一为 120（8/99/13；threading 93、lifecycle 98），intent
清单仍由仓库事实推导。migration-status contract 已补充跨文档一致性断言；
`git diff --check` 和完整 36 项 repository/API/CI guards 通过。

代码关闭证据：
[`test_tcp_server_establishment_saturation.cpp`](tests/contract/tcp_server/test_tcp_server_establishment_saturation.cpp)、
[`test_event_loop_thread_pool.cpp`](tests/contract/event_loop_thread_pool/test_event_loop_thread_pool.cpp)
和 [`test_tcp_server_contract.cpp`](tests/contract/tcp_server/test_tcp_server_contract.cpp)。

维护原则：

- 计划只保留一个“当前执行队列”；
- 完成项写关闭证据，不保留相互矛盾的旧 checklist；
- 数字能生成就不手工复制；
- 历史 SHA 不冒充当前候选。

## 8. API-R1：0.3 stable Core 独立审查

优先级：发布阻塞
对应：P1-04
依赖：所有拟进入 0.3 的 runtime/public header 修改完成

### 8.1 审查输入

- [x] `api/public_api_manifest.json`；
- [x] 相对 `v0.2.0-phase4-preview` 的结构化 diff；
- [x] 10 个新增 stable headers；
- [x] 19 个变化的 stable header fingerprints；
- [x] 所有 provisional headers；
- [x] install consumer；
- [x] intent、thread/ownership rules 和 examples。

### 8.2 必查问题

- [x] stable/provisional 分类是否正确；
- [x] 每个跨线程操作是否返回可区分结果；
- [x] callbacks 的线程、re-entry 和异常语义是否公开；
- [x] options 的默认值、非法值、配置时机是否明确；
- [x] pointer/length API 是否写明有效内存区间前置条件；
- [x] shutdown/destruction 是否有唯一 owner；
- [x] 0.3 source compatibility 承诺是否可执行；
- [x] ABI 非承诺是否明确；
- [x] 没有把 deferred Gateway/transport API 误列为 stable。

### 8.3 关闭门

- [x] 至少一名非原实现 reviewer 完成审查；
- [x] 所有 blocking comments 关闭；
- [x] 审查导致的代码/API 变化已重新运行完整本地验证；
- [x] 历史 diff、同线 zero-diff 和 reviewed surface snapshot 已归档；
- [x] snapshot 的临时候选标记已由 REL-C1 替换为 annotated tag
  `api-r1-approved-surface` 与 peeled implementation commit `9d2a5be...`；
  最终候选 SHA 通过独立 freeze tag 非自引用解析。

关闭结果（2026-08-05）：独立 reviewer `/root/api_r1_independent_review`
首次 `REJECT` 后逐项复审八组 blocker，最终给出 `APPROVE`。Windows/IOCP
Release 120/120、focused 8/8、fresh install consumers 2/2 通过；0.3 同线
diff 为严格零变化，并以 stable header/target additive drift 负向注入证明门禁
会失败。审查记录见 `docs/reviews/api-r1-stable-core-review.md`。

## M3-R3：post-review TCP establishment failure remediation

优先级：运行时 P1，本地关闭
对应：P1-05、P1-06
提交：`9d2a5be0eb5439399f27c2f53ec1bf985c7de1d0`

- [x] TcpServer queue admission 后不提前丢弃 rollback record；owner
  establishment 失败先在 owner teardown，再由 base lifecycle 回滚
  map/load/admission，最终引用回到 owner 释放；
- [x] TcpClient 名称分配、构造、callback setup、publication、IOCP association
  与 Channel 建立纳入一个异常事务；fd 在 terminal callback 前关闭，active
  request 在 callback 前释放；
- [x] 服务端容量故障合同证明同实例账务归零并在释放容量后健康恢复；
- [x] 客户端构造故障合同证明 terminal callback 内同步重连被接纳，旧
  Connector generation settlement 不覆盖新尝试；
- [x] Windows/IOCP Release 全量 120/120、全部 36 项 repository/API/CI
  guards、stable/provisional install consumers 2/2；
- [x] API-R1 reviewed-surface diff 严格为空，公开 stable headers/targets 无漂移。

本任务只达到 `locally-verified`。`9d2a5be` 没有同 SHA remote Linux/Windows、
paired benchmark/capacity 或 24/72 小时 endurance，不能标为最终候选证据。

## GOV-R3：post-review checkpoint current-state sync

优先级：治理一致性，本地关闭
依赖：M3-R3

- [x] 五份当前入口统一到 `9d2a5be`，上游参考统一到 `7fa6922`；
- [x] API-R1 已完成、当前无最终候选、当前 SHA 无同 SHA 发布证据三类事实
  不再互相矛盾；
- [x] intent 显式 verification paths 更新为派生值 139；
- [x] migration-status guard 校验不可变实现检查点，并拒绝该点之后的
  runtime/API/build/test 漂移。

## 9. INF-R1：恢复证据基础设施

优先级：与 remediation 并行
对应：P1-02
需要项目/账户所有者处理外部状态

- [x] GitHub Actions 可正常调度，不存在 billing lock；
- [x] artifact policy 为 `required`，六个 producer artifact 在 run
  `31992899968` 成功 retained/download；
- [x] `gamenet-endurance` Linux runner online、idle；
- [x] `gamenet-windows` runner online、idle，并确认 VS C++ workload、CMake、
  Python 和 Git；
- [x] Windows runner 进程使用交互账户 `DESKTOP-8RFB597\PC`，不存在独立
  service 账户的 VS 可见性偏差；Linux systemd runner 使用已核对的 `xyq`；
- [x] checkout `game-net-core` 和 provenance `mini_trantor@3eba368...` 成功；
- [x] 候选 producer 已验证 artifact upload/download 与 canonical naming；
- [x] 已记录 runner version、OS、arch、toolchain version。

基础设施 smoke 不能替代最终候选执行。

## 10. REL-C1：冻结唯一候选 SHA

优先级：发布关键路径
依赖：M3-R1、M3-R2、GOV-R2、API-R1、M3-R3、GOV-R3

- [x] 工作树只含审查通过的变更；
- [x] finding table 无未处理的 runtime P1；
- [x] 本地快速门通过；
- [x] commit 并 push；
- [x] 记录完整 `CANDIDATE_SHA`；
- [x] `main`、远端 branch、Actions checkout identity 一致；
- [x] 从该点起禁止未重新冻结的 runtime/test/build 修改；
- [x] 文档修正如果改变 evidence manifest 输入，也必须重新确认 SHA/证据范围。

REL-C1 的机器可读记录是 `api/candidate_freeze.json`。候选 commit 不能在其
自身 tree 中嵌入自己的 SHA，因此该文件记录 candidate ref 与解析规则；annotated
tag `v0.3.0-rel-c1-refreeze-1` 的 object target 和远端 ref 记录完整 40 位 SHA。
`api-r1-approved-surface` 独立指向 `9d2a5be...`，证明 reviewed snapshot 的
header/target/fingerprint 与真实 Git tree 一致。freeze tag 不是 release tag，
也不替代 REL-V1/REL-V2、PERF-R1、END-R1 或 REL-D1。

首次 freeze tag `v0.3.0-rel-c1-freeze` 指向
`d3137f9298b47474ea96dc694d44c5c026710039`。run `31992899968` attempt 1 的
六个 producer 全部成功，但 aggregate 因 verifier 的 one-vs-two consumer
合同漂移失败；修复提交 `68b444d` 因此触发本次重新冻结。

任何候选后的代码变化都使后续证据失效，并回到 REL-C1。

## 11. REL-V1：候选本地 clean gate

优先级：候选冻结后立即执行

### 11.1 Windows

- [x] 全新 Debug configure/build；
- [x] 全新 Release configure/build；
- [x] Debug 全量 CTest；
- [x] Release 全量 CTest；
- [x] install/package consumer Debug；
- [x] install/package consumer Release；
- [x] 新 establishment saturation test focused repeat 50；
- [x] IOCP final drain、partial write、read storage、AcceptEx pool focused repeats；
- [x] 36 个或更新后的全部 repository/API/CI guards；
- [x] public API diff 和 CTest inventory evidence。

### 11.2 结果要求

- [x] 没有 Not Run；
- [x] 没有依赖旧 build tree 的缺失 executable；
- [x] 命令、配置、test count、SHA 和日志完整；
- [x] 本地结果只作为 preflight，不替代 remote gate。

关闭证据（2026-08-17）：历史执行声明
**REL-V1：在唯一 v0.3 候选 SHA 上执行本地 clean gate。** 已完成。证据目录为
`G:\gnc-relv1-944f722-run2\rel-v1-evidence`；`summary.json` SHA-256 是
`21061efc2ef82cf16c3fd76af44c46dfef4b4e1299b9fd370a9a2e96321cff89`，
证据索引 `SHA256SUMS.txt` SHA-256 是
`74c5aef414beaf7cd0afa3861fdc3cefa919ed5a1efd9050f858ec435e212c84`。
本地门只作为 preflight；其后独立的 REL-V2 远端门已经完成。

## 12. REL-V2：候选同 SHA 远端 CI

优先级：发布阻塞
依赖：INF-R1、REL-C1、REL-V1

必须成功：

- [x] Linux CMake Debug；
- [x] Linux Release；
- [x] Linux ASan/UBSan；
- [x] Linux TSan threading set；
- [x] Windows MSVC IOCP Debug；
- [x] Windows MSVC IOCP Release；
- [x] install/package consumers；
- [x] PacketFramer libFuzzer smoke；
- [x] aggregate evidence-set verifier。

每个 producer 必须：

- [x] checkout 最终 `CANDIDATE_SHA`；
- [x] 写入 success manifest；
- [x] 记录真实命令；
- [x] 保存 CTest/API/fuzz/install evidence；
- [x] 使用 canonical artifact name；
- [x] artifact 可下载且 hash 可验证。

queued、cancelled、billing failure、checkout failure、artifact quota failure 和 best-effort warning 都不是成功证据。

关闭证据（2026-08-17）：GitHub Actions run `32007753147` attempt 4 的 head SHA
严格等于 `944f7222d7aa7a36e12ffda4ad038ec3ae7d30d7`。六个 producer jobs 和
`Aggregate six-job CI evidence` 全部为 `success`；artifact policy 是 `required`，
六个 producer artifacts 与一个 aggregate artifact 均非空、未过期且已重新下载。
候选源码中的 `tools/verify_ci_evidence_set.py` 对下载副本重验成功，本地结果与
远端 aggregate manifest 除 `generated_at_utc` 外完全一致。远端 aggregate manifest
SHA-256 是
`f6b8c96b4e8afac629bf0e2a1879931f7ebfb81dc966f3cb6bae032ba8bcfbdc`。

## 13. PERF-R1：候选性能与容量收口

优先级：remote CI 之后
对应：P1-02

### 13.1 Core benchmark

- [x] Linux/epoll 和 Windows/IOCP 使用相同候选 SHA；
- [x] baseline 和 candidate 在同 runner、同 toolchain class、同矩阵命令执行；
- [x] canonical performance matrix 三次重复；
- [x] core-capacity matrix 三次重复；
- [x] slow-client overload/recovery；
- [x] idle 1k/10k、small echo 1/2/4 workers、sustained churn；
- [ ] paired evidence verifier 通过；
- [x] accept topology 结论为保留 single-listener/base-loop accept，不进入
  `SO_REUSEPORT` 独立设计轮。

### 13.2 Capacity gate

- [ ] Linux/Windows mixed-pressure-recovery 参数完全一致；
- [ ] slow-reader、healthy probes、schedule lag、TCP/Broadcast memory 和 RSS 同时记录；
- [ ] recovery stable window 达标；
- [ ] 所有 terminal rejection 可归因；
- [ ] 普通 gate 通过后再决定是否运行 dedicated 100k profile；
- [x] 未降低阈值、未删除失败样本、未以重跑覆盖确定性失败。

### 13.3 关闭门

- [ ] workflow producer 全部 success；
- [ ] paired identity/parameter/hash verifier success；
- [ ] regression decision 有数字和 reviewer；
- [ ] 结果写入 migration status，旧 seed 明确标为 historical。

### 13.4 首次执行记录（2026-08-17）

候选身份：annotated tag `v0.3.0-rel-c1-refreeze-1` peeled commit
`944f7222d7aa7a36e12ffda4ad038ec3ae7d30d7`。

Core benchmark run `32027919772` attempt 1：

- Linux/epoll 与 Windows/IOCP producer 都完整生成 canonical performance
  12 scenarios × 3 repetitions 和 core-capacity 12 scenarios × 3 repetitions；
- 两个平台都在 `Enforce same-runner performance budgets` 被拒绝，直接原因不是
  数值超预算，而是比较器把 frozen `gamenet.core_benchmark.v1` baseline 的参数对象
  与 candidate v2 的新增参数做全量相等比较，首个错误均为
  `core.connections-256: baseline/candidate parameters differ`；
- 下载原始 artifacts 后，以 fail-closed 的 v1 → v2 参数兼容规则重算：旧参数必须
  继续存在且值相等，只允许 candidate 增加 v2 字段。四份数值结果全部为 `pass`：
  Linux performance 21/21 metrics、Windows performance 21/21、Linux
  core-capacity 30/30、Windows core-capacity 30/30；
- 最接近预算的 performance 指标分别为 Linux
  `phase4.broadcast-fanout.operations_per_second`：baseline median
  `4,678,121.459`、candidate `3,914,200.084333`、threshold
  `3,274,685.0213`，以及 Windows
  `phase4.framing.throughput_mib_per_second`：baseline `2,078.244`、candidate
  `1,750.409601`、threshold `1,558.683`；
- core-capacity 两个平台最接近预算的指标均为
  `core.connection-churn-1000.churn_attempts_per_second`：Linux baseline
  `999.077654706`、candidate `998.962572773`、threshold `899.1698892354`；
  Windows baseline `992.862786006`、candidate `992.8523566`、threshold
  `893.5765074054`；
- 对同一 Linux candidate 原始矩阵运行 accept-topology evaluator，实际 rate ratio
  为 `0.998962572773`，`rate_missed=false`、`accept_saturated=false`，结论
  `retain_single_listener`；
- 以上是失败 producer 原始样本的 remediation review，不替代远端 producer 与
  paired verifier success。原 verifier 仍正确拒绝：
  `benchmark job did not succeed: linux-release-benchmark`。

Capacity run `32027919807` attempt 1：

- Linux producer 在第一个 `candidate-10k` sample 创建约 1,000 个真实 TCP client
  后，用 `fd_set/select()` 等待新连接；数值 fd 超过 `FD_SETSIZE`，glibc 以
  `bit out of range 0 - FD_SETSIZE` 终止。这是 benchmark client harness 缺陷，
  不是 runner interruption；
- 本地 remediation 把 Linux 单 socket connect wait 改为 `poll()`，Windows 继续
  使用 WinSock `select()`。Windows Release、WSL Linux Release 构建和
  `tests/cmake/test_capacity_profile_contract.py` 均通过；
- 修复后 WSL/Linux 能跑完整的原 `candidate-10k` 参数，但 10 × 32 KiB/connection
  可全部进入 Linux kernel send buffer：10,000 endpoints 全部 accepted、
  `pending_peak_bytes=32,768,000`、`overloaded_connections=0`，最终因
  `overload_observed=false` 按合同失败。这证明当前 32/64/256 KiB profile 只由
  历史 Windows seed 支撑，尚未形成 Linux/epoll 可重复的 typed-overload profile；
- 一个不作为 gate evidence 的 10-client feasibility probe 保持 32/64/256 KiB
  watermarks、把 payload 提高到 256 KiB 后，两平台都出现 typed rejection：Linux
  70 accepted/30 dropped，Windows 30/70；这只是后续 profile 设计输入。它会把
  candidate 的 logical payload 从约 312.5 MiB 提高到约 2.44 GiB，必须先审查
  hosted-runner 资源和 dedicated-100k 放大效应，不能直接写入固定 profile；
- Windows producer sample 1 通过：8,252 accepted、1,748
  `EndpointOverloaded`、pending peak `262,144,000` bytes、500/500 healthy probes、
  16 workers 回收 1,000/1,000 sockets；sample 2 在远端约 120 秒后失败，旧 runner
  未保留该失败 JSON。本地 Windows 同参数三次均通过，三次均为 8,252/1,748、
  pending peak `262,144,000`、500/500 probes，说明远端失败可重试但不能弥补
  Linux profile blocker；
- 两个平台都没有成功 manifest，paired verifier 正确拒绝：
  `expected 2 capacity manifests, got 0`；普通 gate 未通过，因此没有触发
  dedicated 100k profile。

当前 decision：PERF-R1 保持 open，END-R1 保持 blocked-by-dependency。已准备的
本地 remediation 包含 Linux high-fd connect wait 和 v1 → v2 performance
parameter compatibility 及其负向合同；在 profile 重新获得 Linux/Windows 同参数
typed-overload 证据前，不提交“通过”结论。若采纳任何代码/profile 修复，必须返回
REL-C1 冻结新 SHA，而不能移动现有候选 tag 或复用本次失败 attempt。

### 13.5 remediation 本地验证记录（2026-08-17）

- performance comparator 已限定为唯一受支持的 Core v1 → v2 bridge：所有 legacy
  parameter 必须继续存在且值相等，只允许 candidate 增加 v2 字段；同 schema 仍要求
  参数对象完全相等。正向与 shared-parameter drift 负向 fixture 已在 Windows/WSL
  通过；首次失败 artifacts 的四份数值重算仍全部通过预算；
- Linux client connect wait 已由 `fd_set/select()` 改为单 fd `poll()`，避免 1,000+
  descriptor 越过 `FD_SETSIZE`；Windows 保留 WinSock `select()`；
- 为保持 32 KiB payload、32/64/256 KiB application watermarks 和 dedicated-100k
  逻辑流量不放大，新增 owner-loop-only
  `TcpConnection::setSendBufferSize(std::size_t)`。它只请求有限 kernel send buffer，
  不改变应用层 output admission、ownership、callback 顺序或 lifecycle；稳定 API
  fingerprint、intent、rules、直接 socket-option 合同和兼容性审查已同步；
- candidate/dedicated profile 冻结相同 `server_send_buffer_bytes=4096`。三次 10-client
  微型重复在两平台都稳定产生完全可归因的 `EndpointOverloaded`；
- 完整本地 `candidate-10k` 三次重复均通过严格 v3 validator：Windows 每次
  8,252 accepted / 1,748 typed overload、pending peak 262,144,000 B；WSL Linux
  每次 8,000 / 2,000、pending peak 256,000,000 B。六个样本都完成 500/500
  healthy probes、16 workers 回收 1,000/1,000 sockets、pending 归零和 teardown
  release；
- 以上目录 `G:\gnc-perf-r1-remediation-4k` 是带 synthetic identity 的 precommit
  feasibility evidence，不是 candidate/release evidence。

当前 decision：profile remediation 已获得跨平台、同参数、三重复本地可行性证据，
但 PERF-R1 仍为 open。稳定 surface 必须先绑定新的不可变 reviewed checkpoint，随后
REL-C1 冻结新 candidate SHA，并从该 SHA 重新完成 REL-V1、REL-V2 与 PERF-R1；旧
tag 和失败 run 保持不可变，END-R1 继续 blocked-by-dependency。

## 14. END-R1：候选 endurance

优先级：性能/容量通过后
对应：P1-02

- [ ] 24 小时 Linux candidate run；
- [ ] 72 小时 Linux release run；
- [ ] 两次都绑定同一 `CANDIDATE_SHA`；
- [ ] 无 sanitizer/fatal/timeout/runner interruption；
- [ ] 连接、队列、pending output、retained memory 和 shutdown 收敛；
- [ ] observation acknowledgement、selected/executed tests、duration 和 evidence hash 完整；
- [ ] aggregate gate 验证证据身份；
- [ ] runner 中断或 artifact 丢失必须重跑，不做人工豁免。

## 15. LIC-R1：许可证与发布元数据

优先级：对外发布阻塞
对应：P1-03
需要项目所有者决策

- [ ] 选择明确许可证；
- [ ] 更新顶层 `LICENSE`；
- [ ] 更新 README licensing status；
- [ ] 更新 package metadata；
- [ ] 生成/审查 SBOM 与 third-party notices；
- [ ] 确认 `mini_trantor` provenance 和引入代码的许可兼容性；
- [ ] reviewer 确认源码与二进制分发条件。

若许可证仍为 no-grant，可以保留内部技术候选，但不得宣布 externally adoptable release。

## 16. REL-D1：最终发布决定

只有以下全部满足才能进入 release decision：

- [x] P1-01、P2-01 有代码和测试关闭证据；
- [x] stable API review 通过；
- [x] 唯一候选 SHA 未发生漂移；
- [x] local clean gate 通过；
- [x] six-job remote CI 与 aggregate 通过；
- [ ] paired benchmark/capacity 通过；
- [ ] 24/72 小时 endurance 通过；
- [ ] 文档与 manifest 同 SHA；
- [ ] 许可证允许目标发布方式；
- [ ] 没有未接受的 P0/P1。

输出只能是以下之一：

1. `GO`：发布 `v0.3.0-production-candidate`；
2. `NO-GO`：列出 finding、owner、下一验证；
3. `INTERNAL-CANDIDATE-ONLY`：工程门通过但许可证/外部授权未完成。

禁止把 preview、internal candidate 和 externally adoptable release 混为一类。

## 17. REL-D1 后的目标架构计划

本节状态：`queued-not-authorized`。

本节把 `goal.md` 转换为可执行的后续阶段，但不改变当前唯一任务。只有 REL-D1
完成、项目确认下一版本范围后，才能逐项创建或提升 active intent。任何 deferred
或 legacy intent 都不能因为出现在本节而自动获得实现授权。

### 17.1 ARCH-G1：架构契约与证据基线

目标：先把长期方向变成可审查的 intent/ADR 和可比较证据，不改运行时代码。

- [ ] 新建 owner-loop + I/O Engine architecture intent，明确 EventLoop 是 owner
  scheduler/event pump，而不只等于 Readiness Reactor；
- [ ] 新建 Runtime Model architecture intent，明确 Profile、Connection Placement、
  Logic Placement 和业务边界；
- [ ] 盘点 `Poller`、`Channel`、`EventLoopOptions`、Metrics 与 IOCP transport 中的
  readiness/completion 语义耦合；
- [ ] 固化 epoll/IOCP 当前吞吐、延迟、内存、wakeup、shutdown 和 operation-retention
  基线；
- [ ] 给出稳定 API 兼容策略：source-private 迁移、adapter、alias、弃用期或明确的
  pre-1.0 breaking-change gate；
- [ ] 定义所有 Engine、Readiness 专属和 Completion 专属测试矩阵；
- [ ] 独立 reviewer 按 intent → contract → ownership → lifecycle → tests 顺序批准。

关闭门：术语、边界、依赖方向、测试文件和性能回归阈值全部明确；没有代码/API
变更；后续每个实现任务都有对应 active intent 和 owner。

### 17.2 IOE-R1：source-private I/O Engine 契约

目标：不改变行为地建立 Engine seam，先证明抽象边界可行。

- [ ] 定义 source-private `IoEngine`、`IoNoticeBatch`、capabilities、wakeup、
  `beginQuiesce()` 和 `quiescent()` 契约；
- [ ] 用 `PollerIoEngineAdapter` 接入现有 Poller，保持 EventLoop 阶段顺序、active
  batch invalidation 和 shutdown fixed point 不变；
- [ ] 公共调度预算与 backend capacity 开始分层，但不改变 stable options；
- [ ] 新合同覆盖 owner-thread、跨线程 wakeup、admission seal、accepted-work drain、
  generation 和 callback 内关闭；
- [ ] Linux/epoll 与 Windows/IOCP 全量合同通过；
- [ ] 与 ARCH-G1 基线比较，热路径无未接受的回归。

禁止：本阶段不删除 Poller/Channel，不改变 TCP public surface，不引入 io_uring，也
不以“改名”为完成标准。

### 17.3 IOE-R2：epoll Readiness Engine

目标：建立纯 Readiness 参考实现并保持 Linux 行为和性能基线。

- [ ] 引入 `ReadinessPort`、registration identity/generation 和
  `ReadinessNotice`；
- [ ] 将 epoll 注册、等待、wakeup 和 ready dispatch 移入 Engine 边界；
- [ ] 明确 `Channel` 是 Readiness registration/callback binding；
- [ ] 保持 stale readiness、同 fd replacement、remove-before-destroy 和
  active-batch retirement 合同；
- [ ] 覆盖 level/edge 策略、EAGAIN、accept/read budget 和事件批处理；
- [ ] Linux benchmark/capacity 不超过 ARCH-G1 约定的回归预算。

### 17.4 IOE-C1：IOCP 直接 Completion 分发

目标：让 Completion 保留 operation 真实语义，不再伪装成唯一 active Channel。

- [ ] 定义 operation identity/generation、result、bytes、native error、flags、lease
  和 terminal retirement；
- [ ] `Accepted` submission 必须导向唯一 terminal completion；同步非 pending
  失败不建立 future completion obligation；
- [ ] GQCSEx 结果直接形成 `CompletionNotice`，由 owner-loop Completion sink/driver
  消费；
- [ ] 同一连接同批 read/write/cancel completion 保留为独立事实，不通过“每
  Channel 一次”人为合并；
- [ ] 移除 Channel 中的 IOCP operation 携带职责、fake read/write 映射和
  EventLoop 对具体 IocpPoller 的类型判断；
- [ ] shutdown 覆盖 cancel request、observer revoke、terminal dequeue、lease
  release 和 owner retirement；
- [ ] Windows/IOCP 全量生命周期、饱和、性能、容量和 endurance 证据通过。

关闭门：没有 callback-after-destroy、kernel-reference-after-free、phantom
completion 或 stranded Accepted operation；Linux Readiness 合同无回归。

### 17.5 IOE-X1：实验性 io_uring Completion Engine

依赖：IOE-C1 的通用 Completion contract 已稳定。

- [ ] 首版只完成 one-shot accept/recv/send、typed SQ-full rejection、cancel、
  terminal completion、buffer lease 和 final drain；
- [ ] 必须是真实 completion TCP data path，不以 `POLL_ADD` 包装另一个 epoll
  作为阶段完成；
- [ ] epoll 保持默认和 fallback；
- [ ] multishot、provided buffers、fixed files、zero-copy、SQPOLL 和 linked
  operations 全部继续 deferred；
- [ ] 只有最小闭环通过合同、性能和 endurance 后，才单独提升 multishot
  capability intent。

本阶段始终为 experimental，不能自动成为受支持平台承诺。

### 17.6 RTM-R1：TCP-only Runtime Profiles

目标：在不修改稳定 Core 的前提下验证多运行模型，而不是创建任意组合框架。

- [ ] 定义三个 provisional Profile：`SingleLoopInlineEvent`、
  `MultiIoQueuedEvent`、`MultiIoDedicatedFixedTick`；
- [ ] Profile 明确 transport、I/O topology、logic placement、dispatch cadence、
  broadcast 和 backpressure；
- [ ] Inline handler 必须有严格执行预算，不允许阻塞 I/O owner；
- [ ] Queued 模型使用有界队列、合并 wakeup 和 typed overload result；
- [ ] FixedTick 明确 FixedDelay、FixedRateSkipMissed 或
  FixedRateBoundedCatchUp，不再把周期性 drain 默认称为权威固定帧；
- [ ] 每个 Profile 有独立的生命周期、饱和、P99/P999 handoff、Tick jitter、
  内存、Linux/Windows 和 shutdown 证据；
- [ ] 只有至少两个垂直切片证明共同需要时，才提升 `ExecutionCell`、
  `CommandSink` 或 `RuntimeProfile` 为安装接口。

### 17.7 RTM-R2：逻辑分片与 Hybrid

依赖：RTM-R1 至少两个 Profile 通过证据门。

- [ ] 分离 `ConnectionPlacementPolicy` 与 `LogicShardPolicy`；
- [ ] I/O connection owner 建立后不因 player/room/scene affinity 迁移；
- [ ] 命令按 player/room/scene key 进入有界逻辑 cell；
- [ ] 输出通过 endpoint owner executor 回到原连接 owner；
- [ ] 支持 event-driven 事务路径与 FixedTick 模拟路径并存；
- [ ] 明确 cell 内顺序、跨 cell 非全局顺序和 stop/drain 顺序；
- [ ] Actor、AOI、Room、World 和业务状态继续留在 Core 之外。

### 17.8 TRN-X1：后续传输实验入口

依赖：Core、I/O Engine 和 TCP Runtime Profile 已有稳定证据；对应 deferred intent
已经根据当时仓库事实重写并提升。

- [ ] 先做 owner-loop UDP datagram foundation、bounded receive、typed send、MTU
  capability 和 shutdown contract；
- [ ] UDP 稳定后再评估可靠数据报、KCP 或 TCP + datagram 双通道 Session；
- [ ] 不把 KCP、FEC、PMTU、拥塞控制和多传输一次性合并为一个里程碑；
- [ ] TransportEndpoint 公共能力保持窄接口，传输专属 metadata 使用 capability
  或扩展接口；
- [ ] 每个实验传输独立标记 build/test/evidence status，不提前声明生产级。

### 17.9 后续能力的统一提升门

任何 IOE、RTM 或 TRN 能力从 experimental/provisional 提升为 supported 前，必须：

- [ ] 有 active intent、rules、具体 test files 和独立 review；
- [ ] 回答 owner、ownership、re-entry、cross-thread marshal 和 test 五个问题；
- [ ] Linux/Windows 支持声明与实际 backend 一致；
- [ ] 饱和、取消、关闭、fd/handle reuse 和 callback destruction 合同通过；
- [ ] benchmark/capacity/endurance 绑定同一候选 SHA；
- [ ] 与基线相比的吞吐、延迟、内存和复杂度回归有明确决策；
- [ ] 官方 Profile 数量有界，未验证组合继续保持 experimental；
- [ ] Core 没有吸收业务状态、协议研究或部署拓扑。

## 18. 每个任务的固定工作流

每个核心变更必须按以下顺序：

1. 更新/确认 intent；
2. 更新线程、所有权、failure rules；
3. 指定具体 test file；
4. 写能暴露旧行为的 contract；
5. 实现最小修复；
6. focused repeat；
7. 全量 test/guard；
8. 独立 review；
9. 更新 assessment finding 状态；
10. 更新 plan 关闭证据。

提交说明必须回答：

- owner loop 是谁；
- 谁创建/释放；
- 哪些 callback 可重入；
- 哪些操作可跨线程及如何 marshal；
- 哪个测试证明；
- 哪个失败结果可被调用方观察。

## 19. 计划维护规则

- `goal.md` 记录长期目标、统一语义与非目标，不作为实现授权；
- `assessment.md` 记录事实、风险与证据；
- `plan.md` 记录动作、依赖和关闭门；
- `intents/` 中的 `active` 文档才授权当前模块实现；
- finding ID 不复用；
- 一个任务只允许 `open → implemented → locally-verified → remotely-verified → closed`；
- 没有证据时不得从 locally-verified 跳到 closed；
- 外部基础设施阻塞要写明，不伪装成代码失败；
- 当前 SHA 变化时，候选相关任务自动退回 REL-C1；
- 每个检查点只保留一组 current 状态，历史内容转入明确的 historical evidence。

## 20. 下一项唯一执行任务

下一项任务是：

> **PERF-R1 remediation：审查并落地 evidence-tooling/harness 修复，重新设计且验证
> Linux/Windows 同参数 typed-overload capacity profile，然后返回 REL-C1 冻结新的
> 唯一候选 SHA。**

REL-V1 本地 clean gate 与 REL-V2 同 SHA 远端六 producer/aggregate gate 已完成。
当前 candidate 的 PERF-R1 runs `32027919772` / `32027919807` 已失败，不能进入
END-R1。现有失败 artifacts 继续绑定
`v0.3.0-rel-c1-refreeze-1^{commit}`；一旦 remediation 形成新提交，候选自动退回
REL-C1，并需以新 tag 重新完成 REL-V1、REL-V2 和 PERF-R1。新的 PERF-R1 仍必须在
Linux/epoll 与 Windows/IOCP 上使用同参数、同 runner/toolchain class 的
baseline/candidate 运行 canonical performance matrix、core-capacity matrix 和
mixed-pressure-recovery，并由 paired identity/parameter/hash verifier 给出可审查的
regression decision；完成前不能进入 END-R1。
