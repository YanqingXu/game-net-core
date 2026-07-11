# Phase 4 最新审计报告（intent 方法整改后）

- 审计日期：2026-07-12（Asia/Shanghai）
- 发布基线：`main@7668d6b82a0d815ccd79f83c572bc0a36bcceea0`
- 已验证功能候选：`5ebad2c1a4a9487437340935e21f7468140c7e8d`
- 最终 PR head / merge-ref：`4abd5960ec9d2d0bdc5cead9c3f4769949288c51` / `7132faa1334f311cd30aa08e6f41b31726b82a21`
- annotated tag object：`b76077f839230fb99f5e570ef623174747f04249`
- 审计输入：仓库根目录 `assessment.md`、当前 intent/rules/contracts/tests/implementation、工作流与下载后重新校验的远端制品
- 证据台账：[`phase4_evidence_ledger.md`](phase4_evidence_ledger.md)

审计严格采用本项目的 intent-driven 顺序：intent → invariants → threading →
ownership → contracts → tests → implementation，不引入外部方法的阶段、目录或结论。

## 执行结论

`assessment.md` 指出的 Phase 4 代码级 P0 阻断已经全部关闭，并形成不可变功能候选
`5ebad2c1...`。该候选及其 PR merge-ref 已取得主 CI、真实 fuzz、repeat-50 long-soak
和 Linux/Windows benchmark pair 的远端结构化证据；下载后的 manifest、文件大小与
SHA-256 又使用仓库 verifier 独立复验通过。

当前结论是 **Phase 4 功能候选、合并后 main 门禁与正式 Preview 发布闭环均已完成**。
PR #4 已按仓库所有者明确授权转为 Ready 并以 merge commit 合并；merge tree 与最终
PR-head tree 完全相同。精确 release commit 的 main CI、annotated tag、canonical
archives、`SHA256SUMS` 和正式 GitHub prerelease 均已复验。PR 没有独立 GitHub review，
该流程限制保留且不被写成“已审查”。

| 审计对象 | 最新判定 | 合并/发布判断 |
| --- | --- | --- |
| Intent 与依赖治理 | 通过 | 动态验证 25 个 active target、74 条 verification path；16 个冻结 Core intent 仅保留明确 exemption |
| Phase 4 实现与契约 | 通过 | 六项 P0、生命周期、并发、状态机与真实 transport 证据均已关闭 |
| candidate CI / sanitizer / fuzz | 通过 | run `29160903594`，六 producer + aggregate 7/7；五个全量 job 85/85、TSan 61/61、fuzz 1000、三个 consumer 各 1/1 |
| repeat-50 | 通过 | run `29161167423`：threading 3,050/3,050；Pipeline/Broadcast 400/400 |
| Phase 4 benchmark | 通过 | run `29161168417`：Linux epoll、Windows IOCP producer 与 pair gate 3/3 |
| PR #4 / main | 已合并并验证 | final PR CI `29162961320` 7/7；merge commit `7668d6b8...`；main CI `29168786199` 7/7；submitted reviews 0 |
| `v0.2.0-phase4-preview` | 已发布 Preview | annotated tag + formal prerelease + 3 个已重下载复验的 canonical assets |

因此，当前可宣称：

> **Phase 4 candidate、merge/main identity、annotated tag、checksums 与正式 GitHub Preview Release 已闭环；这仍不是 production-ready 或 ABI-stable 声明。**

## 证据边界

### 1. 不可变身份

- candidate / 当时 PR head：`5ebad2c1a4a9487437340935e21f7468140c7e8d`；候选形成后本地、origin 与 PR head 一致且工作树干净。
- candidate PR merge-ref：`e461b597f2642e000717f536f3b430b804ba26ad`。
- 最终 evidence-only PR head / merge-ref：`4abd5960ec9d2d0bdc5cead9c3f4769949288c51` / `7132faa1334f311cd30aa08e6f41b31726b82a21`；run `29162961320` 7/7 success。
- merge commit `7668d6b82a0d815ccd79f83c572bc0a36bcceea0` 以 `4abd5960...` 为第二父提交，二者 tree 均为 `f88cdda7e9d93f10de4d58ca8040292afe991c73`，diff 为空。
- candidate CI 是 `pull_request` 事件，实际 checkout/测试 merge-ref `e461b597...`，同时在 aggregate manifest 中绑定 candidate 与当时 PR head `5ebad2c1...`；不能把两种 SHA 混写为同一个对象。
- 手动 long-soak 与 benchmark 直接 checkout `5ebad2c1...`，其 checkout/candidate/GitHub/current SHA 全部一致。
- 发布时 `main` 为 `7668d6b8...`，tag peel 与 release-commit main CI checkout 均精确匹配。

### 2. 权威远端证据

#### Candidate CI

- run [`29160903594`](https://github.com/YanqingXu/game-net-core/actions/runs/29160903594)，attempt 1，六个 producer 与 aggregate 共 7/7 success。
- aggregate artifact：`ci-evidence-set-e461b597f2642e000717f536f3b430b804ba26ad-29160903594-1`。
- artifact ZIP/GitHub digest：`408398ea5fdcf05675c819136a1ab5665d4dbe1fbcc5a537f45e5832d10cbf3a`。
- `gamenet.ci_evidence_set.v1`：status success；五个全量 producer 各配置/执行 85 项，TSan 从同一 85 项 inventory 精确执行 61 项；Linux Debug、Windows Debug、Windows Release consumer 各执行 1 项；ASan/UBSan producer 记录真实 libFuzzer 1000 units。
- 六个 producer artifact 的 bytes/hash 与 identity 已由远端 aggregate gate 校验；下载后使用 `tools/verify_ci_evidence_set.py` 再次重算，得到同一内容（仅生成时间不同）。

#### Final PR 与 release-commit main CI

- final PR-head run [`29162961320`](https://github.com/YanqingXu/game-net-core/actions/runs/29162961320)，attempt 1，merge-ref `7132faa1...`，六 producer + aggregate 7/7 success；aggregate ZIP SHA-256 `6ab25ed3432de1778ec1d3a0ea17f2e2124fc9ac7c5f2a5b7662186a3d82580c`。
- main push run [`29168786199`](https://github.com/YanqingXu/game-net-core/actions/runs/29168786199)，attempt 1，checkout/candidate/GitHub/current SHA 均为 `7668d6b8...`，六 producer + aggregate 7/7 success。
- main aggregate `ci-evidence-set-7668d6b82a0d815ccd79f83c572bc0a36bcceea0-29168786199-1` 的 ZIP SHA-256 为 `2bd08a1bcb1c502ef5b6a79cd3a6cd79f0e687e6ce3bc2faeddfdcecda0aa9e3`；六个 producer 下载后由仓库 verifier 再次重算通过。

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

- PR #4 已 Ready/merged；仓库所有者明确授权 merge commit，但 GitHub submitted reviews 为 0。
- `v0.2.0-phase4-preview` 是 annotated but unsigned tag；tag object `b76077f...` 在远端 peel 到 `7668d6b8...`。
- [正式 GitHub prerelease](https://github.com/YanqingXu/game-net-core/releases/tag/v0.2.0-phase4-preview) 已发布，`isDraft=false`、`isPrerelease=true`、`isLatest=false`。
- canonical tar.gz、zip 与 `SHA256SUMS` 已从正式 Release 重下载；GitHub asset digest、本地重算和 checksum manifest 一致。GitHub 自动生成源码包不属于 canonical checksum 范围。
- tag 后的发布记录提交只同步事实，不属于 `v0.2.0-phase4-preview` 源码归档，也不得移动该 tag。

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

这些门禁先由本地正负 fixture 证明可执行，随后在候选远端 run 中产生并通过。
它们本身不等同于发布授权；Ready、merge、main 验证、tag 和正式 Release 已在独立的
所有者授权与发布步骤中完成并记录。

## Intent 方法的过程审计限制

- 最终文件布局、active intent、五项 owner/thread/lifetime/re-entry/test 映射和依赖方向
  均符合 intent 方法；但 PR #4 历史仍由 `0d62054` 与 `5ebad2c1` 两个大范围 umbrella
  commit 构成，提交正文没有逐项回答五项 change gate。现有历史不能证明 intent、
  failing contract、implementation 的先后顺序，也不能替代 stacked review 过程。
- 16 个冻结 Core active intent 仍享有明确的 Phase 4 metadata exemption；其中八份没有
  在 intent body 中写出精确 `tests/...` / `tools/...` 路径，虽然对应测试真实存在并已
  注册。它是可追溯性债务，不是 Phase 4 测试缺失；后续应补齐后逐步移除 exemption。
- 仓库和工作树中的外部规格方法命名路径与内容引用均为零；本轮仅使用项目自身
  `intents/`、`rules/`、contracts/tests 与证据工作流。

## 发布闭环与保留项

1. **发布闭环**：Ready、merge commit、final-PR/main CI、annotated tag、canonical
   archives/checksums、正式 prerelease 与发布后重下载复验均已完成。
2. **Review 限制**：PR #4 没有 submitted review。所有者授权满足本次操作授权，但不
   等价于独立代码审查；后续重要发布应把 required approvals 与 required CI contexts
   配置为平台强制门禁。
3. **Intent 可追溯性债务**：16 个冻结 Core active intent 的 metadata exemption 和
   八份缺少精确 verification path 的正文仍应在后续独立文档/治理 PR 中逐步消除。
4. **Preview 边界**：公开 API 不承诺 ABI/source stability；HTTP/RPC/UDP/KCP/TLS/
   coroutine 与正式 all-in-one pipeline library 继续 deferred，必须另行 intent promotion。

## 后续建议规划

1. 将分支保护升级为至少一项 required approval、conversation resolution 和六 producer
   aggregate required status，避免发布授权只存在于会话记录。
2. 以小型、单 intent merge unit 补齐冻结 Core intent 的 verification/provenance metadata，
   每项明确 owner thread、ownership/release、re-entry、cross-thread marshal 和对应测试。
3. 下一功能阶段只从 deferred intent 中逐项 promotion；不得把 Preview 发布解读为同时
   授权 HTTP、RPC、UDP/KCP、TLS、coroutine 或生产容量承诺。

## 最终判断

- intent/contract/implementation 整改：**通过**；
- 不可变功能 candidate `5ebad2c1...`：**已形成并推送**；
- PR merge-ref 主 CI、真实 fuzz 与 aggregate evidence：**通过**；
- direct-candidate remote repeat-50 与 benchmark pair：**通过**；
- intent-first 的历史提交顺序与 stacked-review 过程：**仍不可追溯，作为流程限制保留**；
- PR Ready/merge 与 main/release identity：**完成**；独立 GitHub review：**0，作为流程限制保留**；
- `v0.2.0-phase4-preview` GitHub Release：**已发布并重下载复验**；
- production readiness：**不作此声明**。

## 远端链接

- [merged PR #4](https://github.com/YanqingXu/game-net-core/pull/4)
- [candidate CI 29160903594](https://github.com/YanqingXu/game-net-core/actions/runs/29160903594)
- [final PR-head CI 29162961320](https://github.com/YanqingXu/game-net-core/actions/runs/29162961320)
- [release-commit main CI 29168786199](https://github.com/YanqingXu/game-net-core/actions/runs/29168786199)
- [Long-soak 29161167423](https://github.com/YanqingXu/game-net-core/actions/runs/29161167423)
- [Benchmark pair 29161168417](https://github.com/YanqingXu/game-net-core/actions/runs/29161168417)
- [旧 Phase 4 CI run 29147391402](https://github.com/YanqingXu/game-net-core/actions/runs/29147391402)
- [Core Preview source tag](https://github.com/YanqingXu/game-net-core/tree/v0.1.0-core-preview)
- [v0.2.0 Phase 4 Preview Release](https://github.com/YanqingXu/game-net-core/releases/tag/v0.2.0-phase4-preview)
