# Phase 4 证据台账

更新日期：2026-07-12（Asia/Shanghai）

本台账只记录可追溯事实。工作流定义或 verifier 测试通过表示“证据合同可执行”，
不表示远端 artifact 已经产生；dirty-worktree 本地结果也不得填写到远端候选栏。

## 身份账本

| 字段 | 当前值 | 状态 |
| --- | --- | --- |
| 本地分支 | `agent/phase-4-foundation` | 信息 |
| 本地整改基线 | `0d62054e148a1c95793799eb88856363ac6843d3` 加未提交内容 | dirty worktree，非候选 |
| 不可变 candidate SHA | 待生成 | 未完成 |
| candidate tree status | 待生成 | 必须为干净提交 |
| draft PR | #4，open/draft | 仍指向旧快照 |
| PR #4 head | `0d62054e148a1c95793799eb88856363ac6843d3` | 历史远端身份 |
| PR #4 merge-ref | `31107e8964a0f206087fe2f029a39a15107f6bda` | 历史远端身份 |
| 旧 CI run | `29147391402` | 旧五 producer/74-test 形态，不可复用 |
| `v0.2.0-phase4-preview` tag | 待创建 | 未完成 |
| GitHub Release | 无 | 未完成 |

## 当前本地预检账本

| 检查 | 结果 | 可追溯材料/备注 |
| --- | --- | --- |
| Windows Debug full CTest | 85/85，36.47 秒 | `build-phase4/final-v4-win-debug-20260712T0027-ctest.*` |
| Windows Release full CTest | 85/85，36.97 秒 | `build-phase4-release/final-v4-win-release-20260712T0029-ctest.*` |
| Windows MSVC ASan full CTest | 85/85，43.19 秒 | `build-phase4-asan/final-v4-win-msvc-asan-20260712T0034-ctest.*`；匹配 MSVC 14.44 Hostx64 runtime |
| Linux Clang 19 Release full CTest | 85/85，34.76 秒 | `build-final-v4-linux-release/final-v4-ctest.*` |
| Linux Clang 19 ASan/UBSan full CTest | 85/85，36.18 秒 | `build-final-v4-linux-asan/final-v4-ctest.*` |
| Linux Clang 19 TSan threading | 61/61，35.63 秒 | `build-final-v4-linux-tsan/final-v4-ctest.*` |
| Python governance/build/workflow guards | 27/27 | scope 4、CMake 19、CI 4 |
| Intent semantic inventory | 通过 | 25 active targets、74 verification paths、16 frozen Core metadata exemptions |
| 配置后依赖图 | 通过 | 六生产库的直接/传递方向及两类反向负例 |
| Migration provenance | 通过 | 7 Phase 4 intent、20 paths、固定 source SHA |
| PacketFramer libFuzzer | 精确 1000 runs；最终 corpus 90；峰值 RSS 49 MiB | `build-final-v4-linux-fuzz/final-v4-evidence/`；dirty-worktree 本地预检，非远端 artifact |
| Linux Release install consumer | 1/1，0.02 秒 | `build-final-v4-linux-release-consumer/final-v4-ctest.*` |
| Windows Release install consumer | VS17 2022 Release 1/1，0.03 秒 | fresh prefix；`find_package(GameNetCore 0.2.0 EXACT REQUIRED)`；`build-phase4-consumer-v020-final-v4-20260712T0036b/final-v4-*.{raw.log,junit.xml}` |
| Phase 4 Linux/Windows benchmark snapshots | final-v4 三场景/平台均通过 validator | Linux epoll 与 Windows IOCP dirty-worktree 快照，不作跨机性能比较 |

<!-- LATEST_TREE_REPEAT50_LEDGER_BEGIN -->
| 最新树 threading repeat-50 | 61×50=3,050，零失败，1,777.76 秒，timeout 60 | `build-phase4/final-v4-threading-repeat50-evidence.json`（SHA-256 `12b5f863b679282189c0f5736b48d4e86a0513ed0593cf8325dd06e6926f4102`）；619,797-byte log SHA-256 `ea4798bcfee98ada5fef01660a9f72b5c0ce0384f0f7234f35414b33311bc176` |
| 最新树 Pipeline/Broadcast repeat-50 | 8×50=400，零失败，54.16 秒，timeout 60 | `build-phase4/final-v4-phase4-repeat50-evidence.json`（SHA-256 `9d147fca9c4031ae12848df23b872e7beaa442118bdc24f2a1fb2ac9ec464823`）；69,327-byte log SHA-256 `e38de84531c2c1d20dd5ebf70bf569eaaed7772478fb9e16b68bd511bd78839d` |
<!-- LATEST_TREE_REPEAT50_LEDGER_END -->

两份 final-v4 evidence 使用同一份 `build-phase4/final-v4-ctest-inventory.json`，
其 SHA-256 为
`37ee7fb3572c911fa771ba42ce1fcb91a252bc2c78c56b98b280f5305c77a09a`。
它们覆盖 Session alive permission、Logic exact drain hook、Pipeline distinct-loop stop
与 real slow TCP 修复；更早的 61×50
与 8×50 仅保留在本地构建历史中，不作为本表当前树通过项。

## 候选 SHA 远端证据账本

以下字段必须来自同一个不可变 candidate SHA。当前全部为待生成。

| 门禁 | 要求的权威证据 | Candidate SHA | Run/attempt | Artifact | 结论 |
| --- | --- | --- | --- | --- | --- |
| 主 CI | 六 producer manifest + `gamenet.ci_evidence_set.v1` | 待填 | 待填 | `ci-evidence-set-<sha>-<run>-<attempt>` | 未运行 |
| Linux Debug + consumer | `gamenet.ci_evidence.v1`、85-test inventory/JUnit/log、consumer 1/1 | 待填 | 待填 | canonical name 待填 | 未运行 |
| Linux ASan/UBSan + fuzz | 85-test evidence、fuzz log/dict/corpus/crash artifacts | 待填 | 待填 | canonical name 待填 | 未运行 |
| Linux TSan | 85-test inventory 中精确选择 threading 61 | 待填 | 待填 | canonical name 待填 | 未运行 |
| Linux Release | `gamenet.ci_evidence.v1`、85-test inventory/JUnit/log | 待填 | 待填 | canonical name 待填 | 未运行 |
| Windows Debug IOCP + consumer | 85-test evidence + install consumer 1/1 | 待填 | 待填 | canonical name 待填 | 未运行 |
| Windows Release IOCP + consumer | 85-test evidence + install consumer 1/1 | 待填 | 待填 | canonical name 待填 | 未运行 |
| Long-soak | 两份 `gamenet.ctest_repeat_evidence.v1` + raw logs/inventory | 待填 | 待填 | `long-soak-<job>-<sha>-<run>-<attempt>` | 未运行 |
| Phase 4 benchmark pair | 两个 producer manifest + `gamenet.phase4_benchmark_pair_evidence.v1` | 待填 | 待填 | `phase4-benchmark-pair-<sha>-<run>-<attempt>` | 未运行 |
| GitHub Release | annotated tag、Release URL、notes、evidence links | 待填 | 不适用 | Release URL 待填 | 未创建 |

## 完成规则

1. 六个主 CI producer 全部成功但 aggregate verifier 失败时，主 CI 证据仍判失败。
2. 两个平台 benchmark producer 全部成功但 pair verifier 失败时，benchmark 证据仍判失败。
3. repeat log 只有最终 pass summary、但无法证明每个精确测试执行 repeat 次数时，long-soak
   仍判失败。
4. 任一 artifact 的 candidate/checkout/GitHub/merge SHA、run/attempt 或文件 hash 不一致时，
   不得写入“通过”。
5. 合并 commit 与验证 candidate 不相同时，必须重新验证，或记录并审计可证明等价关系；
   不能默认外推。
6. GitHub Release 只有在上述候选证据闭环后才能创建，并继续标为 Preview。
