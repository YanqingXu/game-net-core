# Phase 4 最新审计报告（intent 方法整改后）

- 审计日期：2026-07-12（Asia/Shanghai）
- 审计分支：`agent/phase-4-foundation`
- 已验证功能候选：`5ebad2c1a4a9487437340935e21f7468140c7e8d`
- PR 验证 merge-ref：`e461b597f2642e000717f536f3b430b804ba26ad`
- 审计输入：仓库根目录 `assessment.md`、当前 intent/rules/contracts/tests/implementation、工作流与下载后重新校验的远端制品
- 证据台账：[`phase4_evidence_ledger.md`](phase4_evidence_ledger.md)

审计严格采用本项目的 intent-driven 顺序：intent → invariants → threading →
ownership → contracts → tests → implementation，不引入外部方法的阶段、目录或结论。

## 执行结论

`assessment.md` 指出的 Phase 4 代码级 P0 阻断已经全部关闭，并形成不可变功能候选
`5ebad2c1...`。该候选及其 PR merge-ref 已取得主 CI、真实 fuzz、repeat-50 long-soak
和 Linux/Windows benchmark pair 的远端结构化证据；下载后的 manifest、文件大小与
SHA-256 又使用仓库 verifier 独立复验通过。

当前结论是 **Phase 4 功能候选及候选级远端证据通过，正式发布闭环尚未完成**。
剩余事项是文档证据补录、PR review/Ready/merge、合并后 main 身份验证、annotated tag
和正式 GitHub Preview Release，不是新的功能实现缺口。

| 审计对象 | 最新判定 | 合并/发布判断 |
| --- | --- | --- |
| Intent 与依赖治理 | 通过 | 动态验证 25 个 active target、74 条 verification path；16 个冻结 Core intent 仅保留明确 exemption |
| Phase 4 实现与契约 | 通过 | 六项 P0、生命周期、并发、状态机与真实 transport 证据均已关闭 |
| 主 CI / sanitizer / fuzz | 通过 | run `29160903594`，六 producer + aggregate 7/7；五个全量 job 85/85、TSan 61/61、fuzz 1000、三个 consumer 各 1/1 |
| repeat-50 | 通过 | run `29161167423`：threading 3,050/3,050；Pipeline/Broadcast 400/400 |
| Phase 4 benchmark | 通过 | run `29161168417`：Linux epoll、Windows IOCP producer 与 pair gate 3/3 |
| draft PR #4 | 功能候选技术门禁通过，流程未完成 | `5ebad2c1...` 候选曾有 11/11 check-runs success；evidence-only descendant 以其自己的新 CI 为准；PR 尚未 review/merge |
| `v0.2.0-phase4-preview` | 未发布 | tag 与 GitHub Release 均不存在，不得宣称 Preview 已发布 |

因此，当前可宣称：

> **Phase 4 candidate 及其候选级远端证据通过；在 review、merge、main 验证、tag 与 GitHub Release 完成前，仍不能称为已发布 Preview。**

## 证据边界

### 1. 不可变身份

- candidate / PR head：`5ebad2c1a4a9487437340935e21f7468140c7e8d`；候选形成后本地、origin 与 PR head 一致且工作树干净。
- PR merge-ref：`e461b597f2642e000717f536f3b430b804ba26ad`。
- 主 CI 是 `pull_request` 事件，实际 checkout/测试 merge-ref `e461b597...`，同时在 aggregate manifest 中绑定 candidate 与 PR head `5ebad2c1...`；不能把两种 SHA 混写为同一个对象。
- 手动 long-soak 与 benchmark 直接 checkout `5ebad2c1...`，其 checkout/candidate/GitHub/current SHA 全部一致。
- `main` 当前仍为 `83d0e5405efe83357059b7e341f1fbb23db67582`，尚不包含 Phase 4 candidate。

### 2. 权威远端证据

#### 主 CI

- run [`29160903594`](https://github.com/YanqingXu/game-net-core/actions/runs/29160903594)，attempt 1，六个 producer 与 aggregate 共 7/7 success。
- aggregate artifact：`ci-evidence-set-e461b597f2642e000717f536f3b430b804ba26ad-29160903594-1`。
- artifact ZIP/GitHub digest：`408398ea5fdcf05675c819136a1ab5665d4dbe1fbcc5a537f45e5832d10cbf3a`。
- `gamenet.ci_evidence_set.v1`：status success；五个全量 producer 各配置/执行 85 项，TSan 从同一 85 项 inventory 精确执行 61 项；Linux Debug、Windows Debug、Windows Release consumer 各执行 1 项；ASan/UBSan producer 记录真实 libFuzzer 1000 units。
- 六个 producer artifact 的 bytes/hash 与 identity 已由远端 aggregate gate 校验；下载后使用 `tools/verify_ci_evidence_set.py` 再次重算，得到同一内容（仅生成时间不同）。

#### Long-soak

- run [`29161167423`](https://github.com/YanqingXu/game-net-core/actions/runs/29161167423)，attempt 1，job/全部步骤 success。
- artifact：`long-soak-linux-long-soak-5ebad2c1a4a9487437340935e21f7468140c7e8d-29161167423-1`；ZIP digest `1e54b26681e39ff72dfb3dec3b9fddd2f0caa180871b4aa7f21051672c57b8c4`。
- 85-test inventory SHA-256：`98c741723ea8f44b2604ab73ab69bf1d6c9649472e47a0ff33e0892e4f661c3c`。
- threading：61×50 = 3,050/3,050，1,670.46 秒；raw log SHA-256 `9fc822d93d952e93cdaa259726f15bee88defd2f74580f66ae3de6e4cf055d66`。
- Pipeline/Broadcast：8×50 = 400/400，34.18 秒；raw log SHA-256 `b722f4446d7bdb7a51d2fff61b3ed0c1d711f9aa08a8346f88f31df0cd3032dd`。
- 两份 `gamenet.ctest_repeat_evidence.v1` 均 success，原始日志又经仓库 verifier 独立复验。

#### Phase 4 benchmark

- run [`29161168417`](https://github.com/YanqingXu/game-net-core/actions/runs/29161168417)，attempt 1，Linux producer、Windows producer 与 pair gate 3/3 success。
- pair artifact：`phase4-benchmark-pair-5ebad2c1a4a9487437340935e21f7468140c7e8d-29161168417-1`；ZIP digest `a4ce4eda074928c1ca97fdb6171752fcf33028f684bd75e3b733a5e7734ce9a8`。
- `gamenet.phase4_benchmark_pair_evidence.v1` 同时绑定 Linux/epoll 与 Windows/IOCP、相同 candidate/run/attempt/参数和各 producer manifest hash；本地重算除生成时间外完全一致。

### 3. 本地补充证据

候选形成前的 final-v4 本地预检仍保留作诊断补充：Windows Debug/Release/MSVC ASan、
Linux Release/ASan/UBSan 均 85/85，Linux TSan 61/61，Linux/Windows exact-version
install consumer 各 1/1，27 个 Python guard、依赖方向与 `git diff --check` 全部通过。
2026-07-12 又从当前源树重新构建 Windows Debug 并通过 85/85（36.30 秒）。发布判断
优先使用上述远端不可变证据，而不是用本地日志替代远端 manifest。

### 4. 发布边界

- PR #4 仍为 open/draft，尚无 review 或 requested changes；技术状态 mergeable/CLEAN。
- `v0.2.0-phase4-preview` tag 与 GitHub Release 均不存在。
- 本次证据补录会形成候选之后的 documentation-only 变更。它不得被默认为与候选同 SHA；合并/发布前必须证明该 descendant 相对 `5ebad2c1...` 只改变证据文档并在 main/PR 门禁中重新验证，或让 tag 明确指向已验证候选。
- 历史 `v0.1.0-core-preview` 仍只是 annotated source tag，不是 Phase 4 Release。

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

这些门禁先由本地正负 fixture 证明可执行，随后已在上述三个远端 run 中产生并通过
候选证据。它们证明 candidate/merge-ref 的候选质量，但不等同于 review、merge、tag
或正式 Release 已完成。

## Intent 方法的过程审计限制

- 最终文件布局、active intent、五项 owner/thread/lifetime/re-entry/test 映射和依赖方向
  均符合 intent 方法；但 PR #4 仍由 `0d62054` 与 `5ebad2c1` 两个大范围 umbrella
  commit 构成，提交正文没有逐项回答五项 change gate。现有历史不能证明 intent、
  failing contract、implementation 的先后顺序，也不能替代 stacked review 过程。
- 16 个冻结 Core active intent 仍享有明确的 Phase 4 metadata exemption；其中八份没有
  在 intent body 中写出精确 `tests/...` / `tools/...` 路径，虽然对应测试真实存在并已
  注册。它是可追溯性债务，不是 Phase 4 测试缺失；后续应补齐后逐步移除 exemption。
- 仓库和工作树中的外部规格方法命名路径与内容引用均为零；本轮仅使用项目自身
  `intents/`、`rules/`、contracts/tests 与证据工作流。

## 当前仍未关闭的发布门禁

1. **证据文档 descendant 审计**：本次补录必须保持 documentation-only；提交后逐路径
   证明相对 `5ebad2c1...` 不改变 intent、rules、production code、tests 或 workflows，
   并让新 PR head 的常规检查重新通过。
2. **Review 与合并**：PR #4 仍是 Draft 且没有 review。转为 Ready、请求审查、选择合并
   策略并合并都需要明确授权；不得重写或丢失已验证功能候选。
3. **main/release identity**：记录 merge commit，验证其与 candidate 的功能等价关系，
   并在 main 上通过所需门禁。若出现任何功能、测试或 workflow 变化，重跑相应 soak/
   benchmark，而不是沿用旧证据。
4. **Tag 与 Release**：创建并验证 annotated `v0.2.0-phase4-preview` tag，生成源码/归档
   checksum，随后发布含 known limitations、unstable APIs、CI/fuzz/soak/benchmark/
   consumer 链接的正式 GitHub Preview Release。

以上任一发布项缺失时，可以称“Phase 4 candidate 及候选证据通过”，但不能称
“Preview 已发布”。

## 建议完成规划

1. 将本轮审计、台账、roadmap、migration status、CI 与 Release 草案作为一个
   evidence-only descendant 提交；用 `git diff 5ebad2c1...`、27 个 guard、85/85 Debug
   与 `git diff --check` 证明没有功能漂移。
2. 推送后等待新 PR head 的必需检查通过，并记录 documentation descendant 与已验证
   candidate 的父子/路径差异；候选 long-soak 与 benchmark 保持绑定 `5ebad2c1...`。
3. 获得明确授权后，把 PR 转为 Ready 并完成 review；选择不会重写已验证功能内容的
   合并策略。
4. 合并后记录 main/release commit 并执行所需 main 门禁；如等价性审计不成立，重新
   生成受影响的候选证据。
5. 获得发布授权后创建 annotated tag、生成源码/归档 checksum，并发布正式
   `v0.2.0-phase4-preview` GitHub Release；继续保持 Preview 定位，不声明
   production-ready。

## 最终判断

- intent/contract/implementation 整改：**通过**；
- 不可变功能 candidate `5ebad2c1...`：**已形成并推送**；
- PR merge-ref 主 CI、真实 fuzz 与 aggregate evidence：**通过**；
- direct-candidate remote repeat-50 与 benchmark pair：**通过**；
- intent-first 的历史提交顺序与 stacked-review 过程：**仍不可追溯，作为流程限制保留**；
- PR review/merge 与 main/release identity：**未完成**；
- `v0.2.0-phase4-preview` GitHub Release：**未完成**；
- production readiness：**不作此声明**。

## 远端链接

- [draft PR #4](https://github.com/YanqingXu/game-net-core/pull/4)
- [主 CI 29160903594](https://github.com/YanqingXu/game-net-core/actions/runs/29160903594)
- [Long-soak 29161167423](https://github.com/YanqingXu/game-net-core/actions/runs/29161167423)
- [Benchmark pair 29161168417](https://github.com/YanqingXu/game-net-core/actions/runs/29161168417)
- [旧 Phase 4 CI run 29147391402](https://github.com/YanqingXu/game-net-core/actions/runs/29147391402)
- [Core Preview source tag](https://github.com/YanqingXu/game-net-core/tree/v0.1.0-core-preview)
- [GitHub Releases](https://github.com/YanqingXu/game-net-core/releases)
