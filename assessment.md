## 总体结论

截至发布提交 [`c4818d4`](https://github.com/YanqingXu/game-net-core/commit/c4818d4b3956c85830e04d4a1f32df4ad701d453)，`game-net-core` 已经完成 Phase 3.5，并发布 [`v0.1.0-core-preview`](https://github.com/YanqingXu/game-net-core/tree/v0.1.0-core-preview)。

我的阶段判断是：

> Phase 1～3.5 已完成并发布；Phase 4 的 PacketFramer、Transport、Session、
> LogicLoop、Pipeline 示例和 Broadcast 基础已在当前 worktree 完成本地实现与
> Windows Debug/Release 验证。Phase 4 尚未发布，远端 Linux/sanitizer/TSan
> 证据仍需由后续 CI 产生。

按不同口径估算：

| 评估口径             |      当前进度 | 判断                         |
| ---------------- | --------: | -------------------------- |
| Reactor/TCP 功能实现 |   90%～95% | 主链路完整                      |
| Core 工程化成熟度      |   90% 左右 | Core Preview 已发布；后续按模块继续提高性能与生产级证据 |
| 完整游戏网络底座愿景       | 约 70%～75% | Phase 4 基础闭环已实现；丰富协议、实验传输和生产化仍未迁移 |
| 生产可用程度           | 仍不建议宣称生产级 | 正确性证据较强，但性能、长稳、优雅停服仍不足     |

## 当前实现进度

| 模块                           | 状态       | 评价                                                |
| ---------------------------- | -------- | ------------------------------------------------- |
| CMake、安装和包导出                 | 已完成      | 已导出 `GameNet::core`，支持 `find_package`             |
| EventLoop / Channel / Poller | 已完成并持续硬化 | Linux epoll 与 Windows IOCP 均有实现                   |
| TimerQueue / Wakeup          | 已完成      | 已覆盖线程与取消竞态                                        |
| Acceptor / Connector         | 已完成      | ConnectEx、retry-stop 等高风险路径已有测试                   |
| TcpConnection                | 主体完成     | read/write/close、pending IO cancellation 已覆盖      |
| TcpServer / TcpClient        | 主体完成     | 多线程 stop、retry、重复调用、跨线程调用已有契约                     |
| EventLoopThreadPool          | 已完成      | 有 restart/stop/queued-work 测试                     |
| Windows IOCP                 | 功能可用     | `AcceptEx/ConnectEx/WSARecv/WSASend` 已进入真实数据路径    |
| 测试体系                         | 较完整      | 当前为 74 个 CTest：7 unit、65 contract、2 integration |
| CI                           | 已建立      | Linux Debug、ASan/UBSan、TSan、Release、Windows MSVC  |
| 长稳测试                         | 已达当前门禁   | `a7fd77c` 已完成远端 46-test × 50，2300 次执行全部通过 |
| PacketFramer / Transport          | 已完成本地基础实现 | 独立 target、active intent 和契约测试已落地 |
| Session / Logic / Broadcast       | 已完成本地基础实现 | 明确 management/logic/endpoint loop 边界与背压结果 |
| Pipeline                          | 示例闭环已完成 | 保持非安装组合层；真实 TCP 集成测试覆盖认证、请求、响应和断线清理 |
| Phase 4 远端验证                  | 待完成      | 当前只有本地 Windows Debug/Release 与 install-consumer 证据 |

核心目标和边界在 [README](https://github.com/YanqingXu/game-net-core/blob/main/README.md)、[migration_status.md](https://github.com/YanqingXu/game-net-core/blob/main/docs/migration_status.md) 和 [scope_boundary.md](https://github.com/YanqingXu/game-net-core/blob/main/docs/scope_boundary.md) 中保持得比较清楚。

## Phase 3.5 收口结果与下一步

### P0：验证、合并与 Core Preview 发布已闭环

PR #2 的核心候选提交是 `a7fd77cbd2140041cebb3f900d5c609fafc2adad`，发布提交是 `c4818d4b3956c85830e04d4a1f32df4ad701d453`。验证文档继续使用不可自指的证据模型：

* `ci` run [`29076601085`](https://github.com/YanqingXu/game-net-core/actions/runs/29076601085)（#27）在同一 SHA 上通过 Linux Debug、ASan/UBSan、TSan、Release 和 Windows MSVC IOCP 五个 job
* `long-soak` run [`29077148022`](https://github.com/YanqingXu/game-net-core/actions/runs/29077148022) 使用 `--repeat until-fail:50 --timeout 60`，46/46 threading tests 全部通过，CTest 实际耗时 1632.47 秒
* `core-benchmark` run [`29077151229`](https://github.com/YanqingXu/game-net-core/actions/runs/29077151229) 在同一 SHA 上产出 Linux epoll 与 Windows IOCP Release artifacts，8 份 JSON 均为 `gamenet.core_benchmark.v1` 且 `status=ok`
* #26 暴露的 repeated-connect 失败已由 generation-tagged 请求准入修复；本地 Debug/Release 全量、46-test threading repeat-5 和单契约 repeat-50 也全部通过
* PR #2 以 merge commit `c4818d4` 合入 `main`，没有改写上述候选提交；主分支 `ci` run [`29079836593`](https://github.com/YanqingXu/game-net-core/actions/runs/29079836593)（#29）再次通过全部五个 job
* annotated tag `v0.1.0-core-preview` 已发布并指向 `c4818d4`

因此，Phase 3.5 已完成。该入口约束已经落实：Phase 4 从 PacketFramer 的
design/intent/contracts 开始，再按 Transport、Session、Logic、Pipeline 示例和
Broadcast 顺序实施；没有迁移 HTTP、RPC、KCP 或正式的全能 Pipeline library。

## Phase 4 执行基线（2026-07-11）

Phase 4 以 `v0.1.0-core-preview`（`c4818d4`）为冻结的 Core 基线，按依赖方向逐层推进。实施过程中允许新增独立的 `GameNet::protocol`、`GameNet::transport`、`GameNet::game_session`、`GameNet::game_logic` 和 `GameNet::broadcast` target，但不得把上层实现重新并入 `GameNet::core`。

执行顺序和状态以本表为准；每一项只有在 intent、线程/所有权契约、实现和对应测试同时落地后才能标记完成：

| 顺序 | 交付物 | 启动状态 | 完成门禁 |
| --- | --- | --- | --- |
| PR-1 | `PacketFramer` | 已完成（本地） | 独立 protocol target；半包、粘包、空帧、超大/恶意长度、分段输入、fuzz smoke 和跨平台字节序契约通过 |
| PR-2 | `TransportEndpoint` + TCP adapter | 已完成（本地） | endpoint 只暴露身份、owner loop、发送和关闭；不改变 `TcpConnection` 生命周期语义 |
| PR-3 | `PlayerSession` + `SessionManager` | 已完成（本地） | manager-loop 单一所有权；认证、上线、离线、rebind、重复登录、断线清理和 heartbeat/idle 契约通过 |
| PR-4 | `LogicLoop` + bounded command queue | 已完成（本地） | 值类型命令、有界提交结果、固定 tick drain；I/O 线程无同步等待且过载可观测 |
| PR-5 | Pipeline demo + integration tests | 已完成（本地） | TCP bytes 到响应和断线清理的完整闭环通过；Pipeline 仍为组合层，不成为 core 依赖 |
| PR-6 | Broadcast + backpressure metrics | 已完成（本地） | 按 owner loop 分批 fanout；数量/字节预算、低优先级丢弃和原因指标可验证；无跨线程直接 send |

共同实施约束：

* 每个新模块先写 active intent，再写 contracts，最后写实现。
* 每个模块明确 owner thread、释放路径、可重入 callback 和跨线程 marshal 方法。
* 新 target 只能依赖同层或更低层 target；`GameNet::core` 的链接接口和源文件不得出现 Phase 4 上层依赖。
* UDP、KCP、HTTP、WebSocket、RPC、TLS 和 coroutine 不属于本轮 PR-1～PR-6 的完成范围，继续保持 deferred/experimental。
* 完成 Phase 4 后以本地全量 Debug/Release CTest 和适用的 sanitizer/Windows CI 作为新验证证据；在远端 run 完成前不得把本地结果写成远端发布证据。

当前本地完成证据：

* Windows MSVC Debug：74/74，34.67 秒。
* Windows MSVC Release：74/74，34.41 秒。
* PacketFramer 的 contract 与 fuzz smoke 另以 GNU g++ C++23、
  `-Wall -Wextra -Wpedantic -Werror` 编译运行通过。
* install consumer 已通过 `find_package(GameNetCore)` 同时链接并运行
  `GameNet::core`、`protocol`、`transport`、`game_session`、`game_logic` 和
  `broadcast` 六个导出 target。
* Phase 4 新增 7 个 CTest：6 个 contract、1 个 integration；threading slice
  从 46 增至 51。51 个 threading tests 已完成 Windows Debug repeat-5，
  255 次执行全部通过，CTest 实际耗时 170.00 秒。上述结果是本地证据，
  不替代远端 CI/TSan。

这里的文档模型已经从“Current HEAD”改为只记录：

```text
Last validated commit
CI run id
long-soak run id
验证日期
测试数量与结果
```

后续继续避免使用自指式的 `Current HEAD` 字段。

### P0：TcpConnection 公共状态查询线程契约已通过远端 TSan

当前 worktree 已将 `TcpConnection::state_` 改为 `std::atomic<StateE>`，`connected()` / `disconnected()` 成为可跨线程调用的 snapshot observer，并新增 `contract.tcp_connection.test_tcp_connection_cross_thread_state` 验证非 owner 线程观察 connect/close 状态转换。

同时，`setTcpNoDelay()`、callback setters、context access、`connectEstablished()`、`connectDestroyed()` 继续被明确为 owner-loop-only；`TcpServer` 也已调整为在连接 owner loop 上安装 callback 后再 establishment。

`ci` #27 的 Linux TSan race-oriented job 已在候选提交 `a7fd77c` 上完整通过，普通 Linux、ASan/UBSan、Release 与 Windows IOCP job 也验证了同一 SHA。

### P1：Linux/Windows 同 SHA 性能基线已建立

当前 worktree 已新增默认关闭的 `gamenet_core_benchmark`，覆盖：

* 每连接内存占用
* 单线程吞吐和延迟
* 多 EventLoop 扩展效率
* 慢客户端和输出堆积下的内存上限

manual `core-benchmark` workflow 已在 `a7fd77c` 上产出 Linux epoll 与 Windows IOCP 的 Release artifacts，覆盖 `echo` / `connections` / `slow-client` 三个场景。当前证据是可审计快照，不是跨平台性能阈值；IOCP 单次 completion 与未来批量 drain 的对比仍是后续性能议题。

### P1：历史 intent 存在迁移语义漂移

当前 worktree 已给全部 formal intent 加上 front matter，并由 `tests/scope/test_intent_metadata.py` 强制 active / deferred / legacy 分类。这个问题的剩余部分从“缺少元数据”变成“持续维护分类准确性”。

其中 active intent 必须指向 `GameNet::core`；deferred 和 legacy intent 不能授权实现，只能作为设计资产。统一元数据形态为：

```yaml
status: active | deferred | legacy
target: GameNet::core
migration_source: mini_trantor
promote_gate: phase-4-protocol
```

## 推荐依赖关系

```mermaid
flowchart TD
    C["GameNet::core<br/>Reactor / TCP"] --> P["GameNet::protocol<br/>PacketFramer"]
    C --> T["GameNet::transport<br/>Endpoint abstraction"]
    P --> S["GameNet::game_session<br/>PlayerSession / SessionManager"]
    T --> S
    S --> L["GameNet::game_logic<br/>LogicLoop / CommandQueue"]
    S --> B["GameNet::broadcast<br/>Router / Dispatcher"]
    P --> G["GameServerPipeline<br/>integration target"]
    T --> G
    L --> G
    B --> G
    T --> X["experimental_udp / kcp"]
```

依赖原则：

* `core` 永远不能反向依赖协议、会话或游戏层。
* `protocol` 只负责字节流到帧，不负责玩家和业务语义。
* `transport` 只抽象发送、关闭和 endpoint 身份，不解析协议。
* `SessionManager` 管理网络会话，不拥有玩家业务数据。
* `LogicLoop` 不依赖 `TcpConnection`，只处理值类型命令。
* `GameServerPipeline` 先作为组合层和集成示例，不要立即变成“大一统核心类”。
* UDP/KCP 实现 transport 接口，但必须继续保持 experimental。

## 按优先级排序的架构路线图

### P0：完成 Phase 3.5，发布 Core Preview

目标：把当前 Reactor/TCP 核心冻结成稳定基线。

完成条件：

* [x] 同一个候选提交上五个 CI job 全绿。
* [x] 当前 46 个 threading tests 完成远端 `repeat 50`。
* [x] `TcpConnection` 状态查询线程契约通过远端 CI / TSan。
* [x] 明确 Logger、自定义 callback、Socket option 的线程语义。
* [x] 清理验证文档的自指 HEAD 问题。
* [x] 合并 PR #2 并打标签 `v0.1.0-core-preview`。

### P1：PacketFramer——Phase 4 唯一正确入口

新增 `GameNet::protocol`，但第一版只实现长度帧：

```text
uint32 length | payload
```

不要一开始就把 `messageId/requestId/playerId` 塞进 PacketFramer。建议分成：

* `PacketFramer`：处理半包、粘包、长度校验。
* `GamePacketHeader`：处理 messageId、requestId、flags。
* `Codec`：负责对象序列化，后续可接 Protobuf、自定义二进制或 Lua。

验收测试：

* 半包
* 多帧粘包
* 空帧
* 超大帧
* 恶意长度
* 分段输入
* fuzz smoke
* Linux/Windows 一致结果

### P2：最小 Transport Endpoint

新增 `GameNet::transport`：

* `TransportSessionId`
* `TransportEndpoint`
* `send(bytes)`
* `close(reason)`
* `ownerLoop()`
* TCP adapter

第一版不要创建庞大的 `TransportManager`，因为目前只有 TCP。等 UDP/KCP 真正进入时再增加 transport registry。

### P2：Session Foundation

新增：

* `PlayerSession`
* `SessionManager`
* `SessionState`
* transport/session 双向索引
* authenticate、online、offline、rebind
* idle timeout 与 heartbeat 状态

关键约束：

* SessionManager 由明确的 base/management loop 拥有。
* I/O loop 只能异步提交，不允许通过 `future.get()` 等待 LogicLoop。
* Session 只保存网络身份和生命周期，不保存背包、战斗、地图等业务状态。

### P2：LogicLoop 和有界命令队列

新增：

* `GameCommand`
* `GameCommandQueue`
* `LogicLoop`
* 固定 tick drain
* 有界队列与显式 submit result

必须保证：

```text
I/O EventLoop -> enqueue command -> LogicLoop
```

禁止退化为：

```text
I/O EventLoop -> 同步执行游戏逻辑
```

### P3：跑通第一个 GameServerPipeline

最小闭环：

```text
TCP bytes
→ PacketFramer
→ auth/session
→ GameCommandQueue
→ LogicLoop handler
→ response
→ TransportEndpoint
```

先把它放在 `examples/game_server_pipeline_demo` 和 integration tests 中。只有当边界稳定后，才考虑提升成正式 library target。

### P3：Broadcast 与全链路背压

在 SessionManager 稳定后实现：

* `BroadcastRouter`
* `BroadcastDispatcher`
* 按 owner loop 批量派发
* fanout 数量和字节预算
* 低优先级丢弃策略
* 显式 metrics reason

AOI、房间和世界状态必须由上层提供目标 session 列表，不能进入 broadcast 核心。

### P4：扩展模块

推荐顺序：

1. coroutine bridge，严格复用 EventLoop 调度。
2. TLS transport adapter。
3. UDP transport。
4. KCP preview。
5. HTTP/WebSocket/RPC adapter。
6. PMTU/FEC 研究模块。

其中 UDP/KCP/PMTU 必须保持独立 experimental target，不能重新侵入 `GameNet::core`。

## 可执行开发计划

| 周期    | PR    | 核心交付物                             | 验收标准                                 |
| ----- | ----- | --------------------------------- | ------------------------------------ |
| 已完成   | PR-0A | CI evidence、文档证据模型、当前 long-soak   | 五个 CI job 同 SHA 全绿；46 threading × 50 |
| 已完成   | PR-0B | TcpConnection 公共 API 线程契约         | 跨线程状态查询契约与远端 TSan 均通过                 |
| 已完成   | PR-0C | core benchmark 基线                 | Linux/Windows 均输出吞吐、P99、内存数据         |
| 已完成（本地） | PR-1  | PacketFramer intent、target、tests  | 半包/粘包/恶意长度/fuzz 全绿                   |
| 已完成（本地） | PR-2  | TransportEndpoint + TCP adapter   | 不改变 TcpConnection 生命周期语义             |
| 已完成（本地） | PR-3  | PlayerSession + SessionManager    | reconnect、重复登录、断线清理契约通过              |
| 已完成（本地） | PR-4  | LogicLoop + bounded command queue | I/O 线程无同步等待；过载可观测                    |
| 已完成（本地） | PR-5  | Pipeline demo + integration tests | 登录、请求、响应、断线完整闭环                      |
| 已完成（本地） | PR-6  | Broadcast + backpressure metrics  | 大 fanout 分批；无跨线程直接 send              |
| 后续    | PR-7+ | coroutine/TLS/UDP/KCP             | 每个模块独立 target 和独立成熟度标签               |

最关键的路线判断没有改变：Phase 4 基础已经按 PacketFramer → Transport →
Session → LogicLoop → Pipeline 示例 → Broadcast 的顺序落地，但现在仍不要继续
迁移 HTTP、RPC、KCP 或把示例提升成完整 GameServerPipeline library。下一步应先
取得新 worktree 的远端 Linux、sanitizer/TSan 与 Windows CI 证据，再决定 Phase 4
preview 发布；UDP/KCP 等继续保持独立 experimental target。
