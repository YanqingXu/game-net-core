# game-net-core 新一轮执行计划

计划日期：2026-07-30
起始基线：`main@7a56132d6ea60346ec06c108cd627b7b4cd5a04f`
依据：同基线的 `assessment.md`

## 1. 本轮目标

本轮只做 production-hardening remediation、候选冻结和发布证据收口：

1. 关闭建连失败回滚的 owner-loop 生命周期缺口；
2. 补齐 EventLoopThreadPool 非法配置/状态转换合同；
3. 完成 0.3 stable Core 独立审查；
4. 冻结新的唯一候选 SHA；
5. 生成同 SHA 的 Linux/Windows CI、性能、容量和 endurance 证据；
6. 给出 `v0.3.0-production-candidate` 是否可发布的明确结论。

在这些门关闭前，不启动正式 Gateway、HTTP、WebSocket、RPC、UDP、KCP、TLS 或新 game pipeline 模块。

## 2. 当前检查点

### 2.1 已完成

- [x] 重新审计 `7a56132d`；
- [x] 清空并重写 `assessment.md`；
- [x] 以 open findings 重建本计划；
- [x] 当前 Windows Release 增量构建成功；
- [x] 119/119 CTests 通过；
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

### 2.2 未完成

- [ ] P2-01 EventLoopThreadPool 非法状态合同；
- [ ] 0.3 stable Core 独立审查；
- [ ] 同 SHA paired benchmark/capacity；
- [ ] 同 SHA 24/72 小时 endurance；
- [ ] 许可证决定；
- [ ] 最终 release decision。

## 3. Finding 与任务映射

| Assessment finding | Plan task | 关闭证据 |
| --- | --- | --- |
| P1-01 | M3-R1 | 新 saturation contract、owner-thread 证明、exact rollback |
| P1-02 | INF-R1、REL-V2、PERF-R1、END-R1 | 同候选 SHA 的成功 manifests/artifacts |
| P1-03 | LIC-R1 | 显式许可证及 package/SBOM metadata |
| P1-04 | API-R1 | 独立审查记录与批准后的 manifest |
| P2-01 | M3-R2 | negative/wrong-thread/late-config/repeated-start contracts |
| P2-02 | GOV-R2 | migration status、assessment、plan 与候选事实一致 |

规则：finding 只有在对应关闭证据存在时才能标记完成；代码合并本身不等于关闭。

## 4. 执行顺序

主依赖链：

```text
M3-R1 → M3-R2 → GOV-R2 → API-R1 → REL-C1
                                      ↓
                         REL-V1 → REL-V2
                                      ↓
                         PERF-R1 → END-R1 → REL-D1
```

`INF-R1` 和 `LIC-R1` 可与代码 remediation 并行，但不得改变候选证据必须绑定唯一 SHA 的规则。

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

- [ ] `numThreads >= 0`；
- [ ] `setThreadNum()` 只允许 base-loop thread；
- [ ] thread count 和 selection policy 在 start 后不可修改；
- [ ] `start()` 在 already-started 时显式失败；
- [ ] `stop()` 后 restart 继续受现有 restart-soak 支持；
- [ ] zero-thread 模式必须执行 base-loop init callback；
- [ ] partial start failure 仍停止并 join 已发布 workers。

### 6.2 测试

在 EventLoopThreadPool contract 中增加：

- [ ] negative thread count；
- [ ] wrong-thread `setThreadNum()`；
- [ ] late thread-count mutation；
- [ ] late policy mutation；
- [ ] repeated start without stop；
- [ ] stop 后合法 restart；
- [ ] zero-thread callback 和 base fallback；
- [ ] TcpServer 对非法 thread count 的一致转发/拒绝。

### 6.3 关闭门

- [ ] 非法调用在状态改变前失败；
- [ ] 不留下半启动 worker；
- [ ] connection load accounting 不被重置或泄漏；
- [ ] 原有 round-robin/least/queue-lag/hash 与 restart tests 全部通过。

## 7. GOV-R2：当前事实与文档同步

优先级：高
对应：P2-02
依赖：M3-R1、M3-R2

- [ ] 更新 `docs/migration_status.md` 的 current audit 日期和候选 SHA；
- [ ] 把历史 benchmark/endurance 证据放入 historical evidence，不描述为当前候选；
- [ ] 更新测试总数和 label 数；
- [ ] 把 P1-01/P2-01 的关闭证据链接到具体 test；
- [ ] 保持 intent inventory 由仓库事实推导；
- [ ] 更新 README 的 candidate 状态，但不提前声明 stable；
- [ ] 检查 `assessment.md`、`plan.md`、README、migration status 无互相矛盾的“current”结论；
- [ ] 运行 `git diff --check` 和全部文档/治理守卫。

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

- [ ] `api/public_api_manifest.json`；
- [ ] 相对 `v0.2.0-phase4-preview` 的结构化 diff；
- [ ] 10 个新增 stable headers；
- [ ] 17 个变化的 stable header fingerprints；
- [ ] 所有 provisional headers；
- [ ] install consumer；
- [ ] intent、thread/ownership rules 和 examples。

### 8.2 必查问题

- [ ] stable/provisional 分类是否正确；
- [ ] 每个跨线程操作是否返回可区分结果；
- [ ] callbacks 的线程、re-entry 和异常语义是否公开；
- [ ] options 的默认值、非法值、配置时机是否明确；
- [ ] pointer/length API 是否写明有效内存区间前置条件；
- [ ] shutdown/destruction 是否有唯一 owner；
- [ ] 0.3 source compatibility 承诺是否可执行；
- [ ] ABI 非承诺是否明确；
- [ ] 没有把 deferred Gateway/transport API 误列为 stable。

### 8.3 关闭门

- [ ] 至少一名非原实现 reviewer 完成审查；
- [ ] 所有 blocking comments 关闭；
- [ ] 审查导致任何代码/API 变化时，重新运行 GOV-R2 和全部验证；
- [ ] 最终 API diff 归档并绑定最终候选 SHA。

## 9. INF-R1：恢复证据基础设施

优先级：与 remediation 并行
对应：P1-02
需要项目/账户所有者处理外部状态

- [ ] 解除 GitHub Actions billing lock；
- [ ] 清理或扩容 artifact storage quota；
- [ ] 使 `gamenet-endurance` Linux runner online、idle；
- [ ] 在 `gamenet-windows` runner 上再次确认 VS C++ workload、CMake、Python 和 Git；
- [ ] 验证 runner service 账户看到的 VS 安装与交互账户一致；
- [ ] 验证 checkout 到 `game-net-core` 和 provenance `mini_trantor` 稳定；
- [ ] 用非候选 smoke run 验证 artifact upload；
- [ ] 记录 runner version、OS、arch、toolchain version。

基础设施 smoke 不能替代最终候选执行。

## 10. REL-C1：冻结唯一候选 SHA

优先级：发布关键路径
依赖：M3-R1、M3-R2、GOV-R2、API-R1

- [ ] 工作树只含审查通过的变更；
- [ ] finding table 无未处理的 runtime P1；
- [ ] 本地快速门通过；
- [ ] commit 并 push；
- [ ] 记录完整 `CANDIDATE_SHA`；
- [ ] `main`、远端 branch、Actions checkout identity 一致；
- [ ] 从该点起禁止未重新冻结的 runtime/test/build 修改；
- [ ] 文档修正如果改变 evidence manifest 输入，也必须重新确认 SHA/证据范围。

任何候选后的代码变化都使后续证据失效，并回到 REL-C1。

## 11. REL-V1：候选本地 clean gate

优先级：候选冻结后立即执行

### 11.1 Windows

- [ ] 全新 Debug configure/build；
- [ ] 全新 Release configure/build；
- [ ] Debug 全量 CTest；
- [ ] Release 全量 CTest；
- [ ] install/package consumer Debug；
- [ ] install/package consumer Release；
- [ ] 新 establishment saturation test focused repeat 50；
- [ ] IOCP final drain、partial write、read storage、AcceptEx pool focused repeats；
- [ ] 36 个或更新后的全部 repository/API/CI guards；
- [ ] public API diff 和 CTest inventory evidence。

### 11.2 结果要求

- [ ] 没有 Not Run；
- [ ] 没有依赖旧 build tree 的缺失 executable；
- [ ] 命令、配置、test count、SHA 和日志完整；
- [ ] 本地结果只作为 preflight，不替代 remote gate。

## 12. REL-V2：候选同 SHA 远端 CI

优先级：发布阻塞
依赖：INF-R1、REL-C1、REL-V1

必须成功：

- [ ] Linux CMake Debug；
- [ ] Linux Release；
- [ ] Linux ASan/UBSan；
- [ ] Linux TSan threading set；
- [ ] Windows MSVC IOCP Debug；
- [ ] Windows MSVC IOCP Release；
- [ ] install/package consumers；
- [ ] PacketFramer libFuzzer smoke；
- [ ] aggregate evidence-set verifier。

每个 producer 必须：

- checkout 最终 `CANDIDATE_SHA`；
- 写入 success manifest；
- 记录真实命令；
- 保存 CTest/API/fuzz/install evidence；
- 使用 canonical artifact name；
- artifact 可下载且 hash 可验证。

queued、cancelled、billing failure、checkout failure、artifact quota failure 和 best-effort warning 都不是成功证据。

## 13. PERF-R1：候选性能与容量收口

优先级：remote CI 之后
对应：P1-02

### 13.1 Core benchmark

- [ ] Linux/epoll 和 Windows/IOCP 使用相同候选 SHA；
- [ ] baseline 和 candidate 在同 runner、同 toolchain class、同参数执行；
- [ ] canonical performance matrix 三次重复；
- [ ] core-capacity matrix 三次重复；
- [ ] slow-client overload/recovery；
- [ ] idle 1k/10k、small echo 1/2/4 workers、sustained churn；
- [ ] paired evidence verifier 通过；
- [ ] 对 accept topology 给出保留 base-loop accept 或进入独立设计轮的明确结论。

### 13.2 Capacity gate

- [ ] Linux/Windows mixed-pressure-recovery 参数完全一致；
- [ ] slow-reader、healthy probes、schedule lag、TCP/Broadcast memory 和 RSS 同时记录；
- [ ] recovery stable window 达标；
- [ ] 所有 terminal rejection 可归因；
- [ ] 普通 gate 通过后再决定是否运行 dedicated 100k profile；
- [ ] 不用降低阈值或删除失败样本制造通过。

### 13.3 关闭门

- [ ] workflow producer 全部 success；
- [ ] paired identity/parameter/hash verifier success；
- [ ] regression decision 有数字和 reviewer；
- [ ] 结果写入 migration status，旧 seed 明确标为 historical。

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

- [ ] P1-01、P2-01 有代码和测试关闭证据；
- [ ] stable API review 通过；
- [ ] 唯一候选 SHA 未发生漂移；
- [ ] local clean gate 通过；
- [ ] six-job remote CI 与 aggregate 通过；
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

## 17. M4 入口

M4 仅在 REL-D1 完成后评估。

候选主题可以包括：

- 正式 Packet contract；
- Gateway/Pipeline；
- production metrics exporter；
- general connection idle policy；
- deferred transports。

M4 必须重新执行 intent → rules → contracts → tests → implementation，不得因保留的 deferred intent 自动扩大当前 scope。

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

- `assessment.md` 记录事实、风险与证据；
- `plan.md` 记录动作、依赖和关闭门；
- finding ID 不复用；
- 一个任务只允许 `open → implemented → locally-verified → remotely-verified → closed`；
- 没有证据时不得从 locally-verified 跳到 closed；
- 外部基础设施阻塞要写明，不伪装成代码失败；
- 当前 SHA 变化时，候选相关任务自动退回 REL-C1；
- 每个检查点只保留一组 current 状态，历史内容转入明确的 historical evidence。

## 20. 下一项唯一执行任务

下一项任务是：

> **M3-R2：补齐 EventLoopThreadPool 非法配置和状态转换合同。**

M3-R1 已由新候选 `95a6ab5` 的 clean 独立审查关闭；首次候选
`446f86d` 的 `request changes` 保留为历史记录。M3-R2 仍须遵循
intent → rules → contracts → tests → implementation 的顺序。
