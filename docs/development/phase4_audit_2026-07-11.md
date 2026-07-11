# Phase 4 最新审计报告（intent 方法整改后）

- 审计日期：2026-07-12（Asia/Shanghai）
- 审计分支：`agent/phase-4-foundation`
- 工作树基线：`0d62054e148a1c95793799eb88856363ac6843d3` 加未提交整改内容
- 审计输入：仓库根目录 `assessment.md`、当前 intent/rules/contracts/tests/implementation 与工作流定义
- 证据台账：[`phase4_evidence_ledger.md`](phase4_evidence_ledger.md)

审计严格采用本项目的 intent-driven 顺序：intent → invariants → threading →
ownership → contracts → tests → implementation，不引入外部方法的阶段、目录或结论。

## 执行结论

`assessment.md` 指出的 Phase 4 代码级 P0 阻断已经在当前 dirty worktree 中完成
整改，且最新一轮 Windows/Linux 全量 CTest、sanitizer 和静态治理守卫均通过。
本轮 final-v4 冻结树继续补齐 SessionManager 独立 `alive` 执行许可、LogicQueue
精确持锁 drain/submit 交错、Pipeline distinct logic-loop stop 支配，以及 Broadcast
真实慢 TCP peer 隔离证据。

当前结论仍是 **本地实现整改通过，候选发布证据未完成**。原因不是又出现了
新的 Phase 4 功能缺口，而是当前整改尚未形成不可变候选 SHA；主 CI、long-soak、
双平台 benchmark 和 GitHub Release 也都没有在该候选 SHA 上实际产生远端 artifact。

| 审计对象 | 最新判定 | 合并/发布判断 |
| --- | --- | --- |
| Intent 与依赖治理 | 本地通过 | 动态验证 25 个 active intent target、74 条显式 verification path；16 个冻结 Core intent 仅保留明确的 metadata exemption |
| Phase 4 实现与契约 | 本地通过 | `assessment.md` 的代码级 P0 已关闭；新增生命周期交错也有确定性测试 |
| 全量测试与 sanitizer | 当前 dirty worktree 本地通过 | Debug/Release/ASan/UBSan/TSan 结果不能替代候选 SHA 的远端运行 |
| repeat-50 | 最新 Pipeline 修复后的 dirty worktree 本地通过 | threading 61×50=3,050、Pipeline/Broadcast 8×50=400；仍非候选 SHA 的远端 long-soak |
| CI evidence contract | 本地定义与负向 fixture 通过 | 六个 producer 加一个 aggregate gate；尚无当前候选的远端 evidence set |
| Phase 4 benchmark | 本地 harness、快照 validator 与 pair contract 通过 | 尚无同一候选 SHA、同一 workflow run 的 Linux/Windows pair artifact |
| draft PR #4 | 仍指向旧 head/run | 不代表当前未提交整改；不能用旧 run 外推当前结论 |
| `v0.2.0-phase4-preview` | 尚未发布 | 当前 GitHub Releases 仍为空；不得宣称 Preview 已发布 |

因此，当前推荐结论是：

> **代码与本地契约可以进入候选提交阶段；在不可变候选 SHA 的同 SHA 远端证据集完成前，不得宣称远端验证完成，也不得发布 `v0.2.0-phase4-preview`。**

## 证据边界

### 1. 当前 dirty-worktree 本地证据

以下结果证明当前实现具备形成候选提交的质量，但它们没有共同的不可变 Git SHA，
不能充当发布证明。

| 本地门禁 | 最新结果 | 边界 |
| --- | --- | --- |
| Windows MSVC Debug | 85/85，36.47 秒 | `build-phase4/final-v4-win-debug-20260712T0027-ctest.*` |
| Windows MSVC Release | 85/85，36.97 秒 | `build-phase4-release/final-v4-win-release-20260712T0029-ctest.*`；Release 断言门禁有效 |
| Windows MSVC ASan | 85/85，43.19 秒 | `build-phase4-asan/final-v4-win-msvc-asan-20260712T0034-ctest.*`；使用匹配 MSVC 14.44 Hostx64 runtime，未见 sanitizer/runtime error |
| Linux Clang 19 Release | 85/85，34.76 秒 | `build-final-v4-linux-release/final-v4-ctest.*` |
| Linux Clang 19 ASan/UBSan | 85/85，36.18 秒 | `build-final-v4-linux-asan/final-v4-ctest.*`；未见 sanitizer error |
| Linux Clang 19 TSan | threading 61/61，35.63 秒 | `build-final-v4-linux-tsan/final-v4-ctest.*`；未见 data-race 报告 |
| Python scope/CMake/CI 守卫 | 27/27 | 4 个 scope、19 个 CMake、4 个 CI 守卫 |
| intent semantics | 通过 | 25 active targets、74 verification paths、16 frozen Core metadata exemptions、配置后生产依赖方向 |
| migration provenance | 通过 | 七份 Phase 4 intent、20 条 source path 固定到 `mini_trantor` source SHA |
| PacketFramer libFuzzer | 精确 1000 runs，最终 corpus 90，峰值 RSS 49 MiB | `build-final-v4-linux-fuzz/final-v4-evidence/`；仍需候选 SHA 远端 artifact |
| Release install consumer | Linux 1/1（0.02 秒）；Windows VS17 2022 Release 1/1（0.03 秒） | Windows 使用全新安装前缀并通过 `find_package(GameNetCore 0.2.0 EXACT REQUIRED)`；仍须与远端候选 artifact 身份绑定 |
| Phase 4 benchmark | Linux epoll 与 Windows IOCP final-v4 三场景快照均通过共享语义 validator | 本地双平台快照仍不是不可变候选的远端 pair artifact |

首次 Windows Debug 全量运行以 84/85 暴露 Broadcast `EndpointClosed` 旧总数断言仍为
2，而当前三个独立分支（transport 4/8/9）真实总数为 3；修正为总计 3 且逐 id 各 1
后重新构建，final-v4 85/85 通过。MSVC ASan 首轮 `0xc0000135` 是测试 shell 未包含
匹配 Hostx64 ASan runtime；注入 MSVC 14.44 runtime 路径后的全量运行 85/85 通过。
两次首轮日志都作为诊断历史保留，不充当 green 证据。

以下 final-v4 repeat-50 是在这些修正后的最新整改树上重新生成的本地预检；其身份仍是
`0d62054e...` 加未提交内容，而不是不可变 candidate SHA。

<!-- LATEST_TREE_REPEAT50_UPDATE_BEGIN -->
最新树 repeat-50：**本地通过**。

- threading 从同一 85-test inventory 精确选择 61 项，每项执行 50 次，实际/预期
  均为 3,050 次，零失败，`--timeout 60`，总测试时间 1,777.76 秒。结构化证据为
  `build-phase4/final-v4-threading-repeat50-evidence.json`；inventory SHA-256 为
  `37ee7fb3572c911fa771ba42ce1fcb91a252bc2c78c56b98b280f5305c77a09a`，
  619,797-byte 原始日志 SHA-256 为
  `ea4798bcfee98ada5fef01660a9f72b5c0ce0384f0f7234f35414b33311bc176`。
- Pipeline/Broadcast 以 `game_pipeline|broadcast` 精确选择 8 项，每项执行 50 次，
  实际/预期均为 400 次，零失败，`--timeout 60`，总测试时间 54.16 秒。结构化证据为
  `build-phase4/final-v4-phase4-repeat50-evidence.json`；使用同一 inventory，
  69,327-byte 原始日志 SHA-256 为
  `e38de84531c2c1d20dd5ebf70bf569eaaed7772478fb9e16b68bd511bd78839d`。

两份输出均为 `gamenet.ctest_repeat_evidence.v1`，证明精确 per-test execution count，
但仍只是 dirty-worktree 本地文件，不是远端 long-soak artifact。
<!-- LATEST_TREE_REPEAT50_UPDATE_END -->

### 2. 当前远端不可变事实

- draft PR #4 仍为 open/draft，head 为
  `0d62054e148a1c95793799eb88856363ac6843d3`，merge-ref 为
  `31107e8964a0f206087fe2f029a39a15107f6bda`。
- 与 PR #4 旧 head 关联的 `ci` run `29147391402` 实际 checkout/测试的是
  merge-ref `31107e8964a0f206087fe2f029a39a15107f6bda`，不是 head 本身。它只有
  当时的五个 producer/74-test 形态，未覆盖当前六 producer、85-test、61-test
  TSan 或 aggregate evidence set 契约。
- 当前 dirty worktree 没有对应的远端 workflow run，也没有可供核验的候选
  artifact。
- GitHub Releases 当前仍没有 Release；历史 `v0.1.0-core-preview` 只是源码 tag，
  不能冒充 `v0.2.0-phase4-preview` Release。

以上远端事实只描述旧快照和当前缺口。旧 run 的成功不能外推到当前整改，
本地通过也不能反向写成远端成功。

## Intent correctness 与治理复核

### 动态 active-intent 语义覆盖

`tests/scope/test_intent_semantics.py` 不再使用固定七份文件列表来冒充全部 active
intent 治理。它从 catalog 与 front matter 动态派生当前 25 个 active intent，
并验证 74 条显式 verification path：

- 69 条普通 C++ CTest source；
- 1 个真实 libFuzzer target；
- 4 个由 workflow 执行的 Python governance test。

`echo_server` 与 Core benchmark intent 分别指向真实的
`gamenet_echo_server` example target 和 `gamenet_core_benchmark` benchmark target，
不再借用安装库 alias 表达非库 artifact。

七份 Phase 4 enriched intent 仍严格要求 artifact kind、install 属性、provenance
和非空 verification path。16 份冻结 Core active intent 是有边界的渐进式 exemption：
其 target 必须解析为真实、已安装的 `GameNet::core`，但暂不强制补齐 enriched
artifact kind、迁移 provenance 与非空 verification metadata。这里的通过结论是
“25 个 active target 均解析且 exemption 可审计”，不是“16 份 Core intent 已完成
Phase 4 等级的 provenance enrichment”。

### 配置后生产依赖图

语义守卫从真实临时 CMake configure/trace 结果构造 target link graph，检查以下
六个生产库的直接和传递依赖方向：

- `gamenet_core`；
- `gamenet_protocol`、`gamenet_transport`；
- `gamenet_game_session`；
- `gamenet_game_logic`、`gamenet_broadcast`。

负向 fixture 同时证明顶层 `core → protocol` 直接反向依赖和
`core → bridge → protocol` 传递反向依赖都会失败，门禁不再只是源码字符串搜索。

`game_backpressure_policy.intent.md` 保持 deferred，但 Partial Supersession Record
已显式链接四个已落地的有界策略：LogicLoop、Broadcast、PacketFramer 和 Pipeline
pending-auth。metadata guard 同时证明源 intent 仍为 deferred，避免把局部实现误写成
全局 backpressure policy 已完成。

结论：旧审计中的不存在 target、表面 metadata grep、迁移 provenance 缺失和依赖
方向只靠约定等问题，已经在本地治理层关闭；冻结 Core enrichment 仍是明确保留项。

## 关键 P0 与新增交错复核

### PacketFramer：预算与真实 fuzz target

- 最大 buffered bytes、每次 push 的 frame/byte budget、
  `BudgetExhausted`/`BufferLimitExceeded` 和 continuation 均有显式契约。
- 环形缓冲避免 continuation 对 backlog 反复整体搬移；budget、wrap-around、
  reset/recovery 和差分 chunking 有直接断言。
- `LLVMFuzzerTestOneInput` 接收任意字节，以 6 个二进制 seed 和字典检查 bounded
  continuation、reset recovery 与 one-shot/chunked 一致性。

本地代码/测试阻断已关闭；发布仍要求候选 SHA 下的 sanitizer-backed fuzz artifact。

### TransportEndpoint：owner-loop 生命周期

- endpoint 不再依赖裸 `EventLoop*`，跨线程调度使用弱状态的
  `EventLoopExecutor`。
- EventLoop 在 final drain 前关闭 admission；已接受任务被排空，新任务返回明确
  `OwnerUnavailable`。
- endpoint outlive connection/loop、wrong-thread send/close、stop 后提交和跨线程
  `isOpen()` snapshot 均进入契约。

### SessionManager：索引、线程归属与真实提交/排空

- transport id 只有在 player 与 endpoint 均一致时才返回 Existing；同 id/new endpoint
  明确拒绝并关闭，不覆盖 player/transport 双索引。
- `postAuthenticate`、`postOffline`、`postHeartbeat` 和 shutdown 使用弱 lifetime
  admission；共享 `LifetimeState` 的原子 `alive` 是独立执行许可，强引用不能延长
  shutdown 后的 facade 访问权限，每个 queued task 在解引用 manager 前再次检查许可。
- `PlayerSession` 明确规定全部 accessor/mutation 只能在 management owner loop 使用；
  `shared_ptr<const PlayerSession>` 不是跨线程 snapshot，这一措辞有静态 contract guard。
- 生命周期测试现在让 heartbeat 与 offline 两个 producer 真正在同一 live session 上
  重叠提交。owner loop 在两个 producer 仍活跃时观察到 Offline，随后停止 producer，
  再用每个 producer 各自的 queue sentinel 证明其已接收 post 全部排空，最终 player/
  transport 双索引为空且 session 不复活。

这比“两个线程启动后立即 join，再运行 owner loop”的伪交错更强，直接覆盖 submit
与 owner drain 的时间重叠。

### LogicLoop：显式状态与 re-entry

- 生命周期是 `Created → Running → Stopping → Stopped` 的 one-shot 状态机，公开
  状态为原子 snapshot。
- producer、handler、output、metric callback re-entry 和 handler 内 stop 有确定语义：
  当前 batch 完成、backlog 精确丢弃并计量、后续 admission 关闭。
- 测试构建专用 hook 位于 `drain()` 持有 queue mutex 之后与 `submit()` 加锁之前；
  双闩锁精确证明 producer 已进入 submit 时 drain 临界区仍持锁。关闭测试构建时 hook
  无符号、无公开安装 API、无运行时分支或存储。
- 并发契约还证明多 producer 在 handler 活跃时提交，无重复和无法解释的丢失；
  timer/queued work 通过 lifetime token 撤销。

### Pipeline：CallbackState 撤销、同 loop stop 与 exact batch

- Pipeline 的异步边界共享 `CallbackState`；原子 `alive` 是独立的执行许可。
  即使 callback 仍持有强引用，stop/revoke 后也不能继续产生 I/O、management、auth
  timer、logic output、endpoint send/close 等副作用。
- I/O、management、认证 timer、logic output 与 endpoint observer 在副作用前重新检查
  `alive`。endpoint observer 被同步屏障固定跨越 stop/revoke 后，其 queued send 被取消，
  对端 `receivedBytes == 0`。
- 默认 management loop 与 LogicLoop owner 相同的路径中，Logic-stage observer 可以在
  handler 栈上同步调用 `pipeline.stop()`。当前 tick 尚未退栈时，被停止的 LogicLoop
  通过 owner-loop queued `shared_ptr` 延后释放，避免 handler/tick use-after-free；独立
  borrowed logic-loop 路径要求 caller 保持 owner loop 存活，并用确定性闩锁证明 active
  Logic callback 与 management-side stop 真正重叠；stop 在 logic owner 上排队并等待
  callback 退出后才完成销毁支配。
- AUTH 与 early command 的关键契约不再依赖 TCP 分包时序：fake `BatchEndpoint` 直接
  在一次精确 `onFrames({"AUTH exact-batch-player", "early-command"})` 调用中注入两帧，
  并断言 `AUTH_OK` 与 `RESP early-command`。真实 TCP 三 loop 集成仍保留。
- active worker callback 下立即销毁、queued auth 前 stop、disconnect-before-auth、
  Logic-stage 同步 stop 和 endpoint observer 跨 revoke 均是确定性交错测试。

因此，强引用延长“陈旧执行许可”、同 loop handler 栈内销毁 LogicLoop、observer
返回后继续 send，以及所谓 same-batch 实际依赖 socket 拆包等缺口均已关闭。
Preview 仍只承诺撤销/取消式 shutdown，不承诺公开的全 transport close-completion future。

### Broadcast：计划不可伪造与真实 TCP 多 loop

- `BroadcastPlan` 只能由 router 构造；dispatcher 防御校验 owner expiry、moved-from、
  close-between-route-dispatch 和 send rejection。
- `Scheduled` 与相应 `Sent`/`Dropped` 的原因和顺序可验证。
- 真实 `TcpTransportEndpoint` 多 worker-loop、1024 endpoint × 12 message、slow endpoint、
  ordering 及多轮 disconnect/reconnect 均进入测试。final-v4 还使用真实不读 socket 的
  慢 TCP peer 触发 server high-water，同时证明另一 worker loop 上的正常 peer 先完成且
  慢 endpoint 当时仍保持 open。

### 审计过程中额外修复

- Windows IOCP：Acceptor/Connector overlapped completion storage 由 Poller 保留到 completion
  出队或 Poller 关闭，处理 cancel/timeout 与延迟 completion 竞争；相关路径包含在 MSVC
  ASan 85/85 中。
- TimerQueue TSan fixture：repeating `TimerId` 的创建、读取和取消回到 owner loop，消除
  测试夹具自身的栈变量竞态；Linux TSan threading 61/61 无警报。

## 可执行证据契约复核

### 主 CI：六个 producer 加一个 aggregate gate

六个 producer 分别是 Linux Debug、Linux ASan/UBSan、Linux TSan、Linux Release、
Windows Debug IOCP 与 Windows Release。第七个 job `ci-evidence-set` 只聚合证据，
不是第七个平台 producer。

每个 producer 的 `gamenet.ci_evidence.v1` writer 现在强制：

- checkout HEAD、`GITHUB_SHA` 与 merge/current SHA 一致；
- PR 事件的 candidate 等于 PR head，非 PR 事件的 candidate 等于 `GITHUB_SHA`；
- artifact 名按顺序以 `-${job}-${sha}-${run_id}-${run_attempt}` 结尾；
- manifest 记录身份、命令、pre-upload status 以及每个文件的 bytes/SHA-256。

aggregate verifier 要求恰好六个 producer/job，统一 workflow/run/attempt/ref/event/repo
和 SHA，重新计算全部文件 hash，并检查五个全量 job 的 85 项 inventory、TSan 从
85 项中精确选择 61 项，以及 Linux Debug、Windows Debug、Windows Release 三条
install-consumer 路径各 1 项。成功时生成
`gamenet.ci_evidence_set.v1`。正向与身份、清单、文件篡改等负向 fixture 均通过。

### Long-soak：结构化 repeat evidence

`verify_ctest_repeat_evidence.py` 不把 CTest 最后一行“100% passed”当作足够证明。
它从 85 项 inventory 精确派生 `threading=61` 和
`game_pipeline=4 + broadcast=4`，解析每一条 CTest result line，要求每个选中测试
恰好执行指定 repeat 次数、没有 missing/unknown/nonpassing，并核对最终 summary。
输出 `gamenet.ctest_repeat_evidence.v1`，包含 per-test/total executions、real time、
inventory/log SHA-256、selection、repeat、timeout 与命令。long-soak artifact 也使用
canonical job/SHA/run/attempt 身份并保留 90 天。

### Phase 4 benchmark：双平台 pair gate

Linux/epoll 与 Windows/iocp producer 各保存 toolchain、三个 raw JSON、语义 manifest
与共享 CI job manifest。`verify_phase4_benchmark_evidence_set.py` 要求恰好这两个平台，
核对同 checkout/candidate/GitHub/merge SHA、run/attempt/workflow/ref/event/repo，重新计算
文件 hash，验证场景集合/顺序、相同参数、runner OS/backend，并生成
`gamenet.phase4_benchmark_pair_evidence.v1`。第三个 job 只负责 pair aggregation，
不是性能测量 producer。

这些门禁已经由本地正负 fixture 证明可执行，但当前仍只是“证据生产合同”，不是
已经产生的远端发布证据。

## 当前仍未关闭的发布门禁

1. **冻结唯一候选 SHA**：把 intent、规则/契约、实现、测试、工作流和文档形成干净
   候选提交；当前 dirty worktree 没有发布身份。
2. **同 SHA 主 CI evidence set**：候选 SHA 的六个 producer 全绿，aggregate gate 生成
   并上传 `gamenet.ci_evidence_set.v1`。
3. **候选 SHA 的真实 fuzz 与 long-soak artifact**：保存 corpus/dictionary/log/crash
   artifact 和两份 `gamenet.ctest_repeat_evidence.v1`，不能用本地构建目录替代。
4. **候选 SHA 的 benchmark pair artifact**：同一 run 的 Linux epoll 与 Windows IOCP
   producer 都成功，并由 pair gate 生成配对 manifest；不把不同主机吞吐值作横向阈值。
5. **PR 与 Release 闭环**：把 candidate SHA、run id、artifact 名/hash 写入账本，完成
   review 后合并；确认 release commit 与验证 commit 一致或可证明等价，再创建 annotated
   tag 和 GitHub Release `v0.2.0-phase4-preview`。

任何一项缺失时，只能称“Phase 4 本地整改候选”，不能称“同 SHA 远端验证完成”或
“Preview 已发布”。

## 建议完成规划

1. 锁定已完成的 final-v4 repeat-50、双平台 Release install-consumer、全量/sanitizer、
   fuzz 和 benchmark
   本地记录；后续若再改生产代码或关键契约，必须重跑受影响证据。
2. 做最终 scope boundary、intent semantics、dependency graph、Python guards、
   full CTest/sanitizer 与 `git diff --check` 复核，冻结唯一候选 SHA。
3. 在候选 SHA 运行六 producer 主 CI，只有 aggregate evidence set 成功才把主 CI 判为
   完成。
4. 在同一候选 SHA 运行 long-soak 与 benchmark workflow；分别以结构化 repeat manifest
   和双平台 pair manifest 为完成条件。
5. 把 PR head、candidate/merge SHA、run/attempt、producer/aggregate artifact 名和 hash
   写入证据台账，再更新审查结论并合并。
6. 创建 `v0.2.0-phase4-preview` GitHub Release，附验证 SHA、CI/soak/fuzz/benchmark
   evidence 链接、release notes 与已知限制；继续保持 Preview 定位，不声明
   production-ready。

## 最终判断

- 本地 intent/contract/implementation 整改：**通过**；
- 当前 dirty-worktree 全量单次测试与 sanitizer：**通过**；
- 最新 Pipeline 修复后的本地 repeat-50：**通过**；
- 不可变候选 SHA：**尚未形成**；
- 同 SHA 远端 aggregate/pair evidence：**未完成**；
- `v0.2.0-phase4-preview` GitHub Release：**未完成**；
- production readiness：**不作此声明**。

## 历史远端链接

- [draft PR #4](https://github.com/YanqingXu/game-net-core/pull/4)
- [旧 Phase 4 CI run 29147391402](https://github.com/YanqingXu/game-net-core/actions/runs/29147391402)
- [Core Preview source tag](https://github.com/YanqingXu/game-net-core/tree/v0.1.0-core-preview)
- [GitHub Releases](https://github.com/YanqingXu/game-net-core/releases)
