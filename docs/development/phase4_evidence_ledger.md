# Phase 4 证据台账

更新日期：2026-07-12（Asia/Shanghai）

本台账只记录可追溯事实。工作流定义或 verifier 测试通过表示“证据合同可执行”，
不表示远端 artifact 已经产生；dirty-worktree 本地结果也不得填写到远端候选栏。

## 身份账本

| 字段 | 当前值 | 状态 |
| --- | --- | --- |
| 发布记录基线 | `main@7668d6b82a0d815ccd79f83c572bc0a36bcceea0` | `v0.2.0-phase4-preview` 的 peeled commit |
| 已验证功能 candidate SHA | `5ebad2c1a4a9487437340935e21f7468140c7e8d` | 不可变提交；候选形成时本地/origin/PR head 一致且工作树干净 |
| candidate tree status | clean | 代码、intent、rules、tests、workflow 与候选内文档均已提交 |
| PR | [#4](https://github.com/YanqingXu/game-net-core/pull/4)，merged | explicit owner authorization；merge commit `7668d6b8...`；GitHub submitted reviews 为 0 |
| PR #4 head（候选运行时） | `5ebad2c1a4a9487437340935e21f7468140c7e8d` | candidate identity |
| PR #4 merge-ref（候选 CI） | `e461b597f2642e000717f536f3b430b804ba26ad` | candidate `pull_request` run 的实际 checkout/test identity |
| PR #4 最终 evidence-only head / merge-ref | `4abd5960ec9d2d0bdc5cead9c3f4769949288c51` / `7132faa1334f311cd30aa08e6f41b31726b82a21` | run `29162961320`，六 producer + aggregate 7/7 success |
| 当前发布 `main` | `7668d6b82a0d815ccd79f83c572bc0a36bcceea0` | merge tree 与最终 PR-head tree `f88cdda7e9d93f10de4d58ca8040292afe991c73` 相同 |
| candidate CI run | [`29160903594`](https://github.com/YanqingXu/game-net-core/actions/runs/29160903594)，attempt 1 | 六 producer + aggregate，7/7 success |
| release-commit main CI | [`29168786199`](https://github.com/YanqingXu/game-net-core/actions/runs/29168786199)，attempt 1 | exact checkout `7668d6b8...`；六 producer + aggregate，7/7 success |
| long-soak run | [`29161167423`](https://github.com/YanqingXu/game-net-core/actions/runs/29161167423)，attempt 1 | direct candidate checkout，success |
| benchmark run | [`29161168417`](https://github.com/YanqingXu/game-net-core/actions/runs/29161168417)，attempt 1 | direct candidate checkout，3/3 success |
| `v0.2.0-phase4-preview` tag | annotated object `b76077f839230fb99f5e570ef623174747f04249` | remote peel 精确指向 `7668d6b8...`；annotated but unsigned |
| GitHub Release | [v0.2.0 Phase 4 Preview](https://github.com/YanqingXu/game-net-core/releases/tag/v0.2.0-phase4-preview) | published prerelease；`isDraft=false`、`isPrerelease=true` |

候选之后的 evidence-only descendant 已逐路径证明只改变 8 个 `docs/` 文件，并在
最终 PR head 和合并后 main 上重新通过必要 CI。发布 tag 保持指向已验证 merge commit；
本次 tag 后发布记录提交不属于已发布源码归档，也不会移动或重写该 tag。

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

主 CI 是 PR 事件：candidate/PR head 为 `5ebad2c1...`，实际 checkout 为 merge-ref
`e461b597...`，aggregate manifest 同时绑定两者。手动 long-soak 与 benchmark 直接
checkout candidate，因此其 checkout/candidate/GitHub/current SHA 均为 `5ebad2c1...`。

| 门禁 | 要求的权威证据 | Candidate SHA | Run/attempt | Artifact | 结论 |
| --- | --- | --- | --- | --- | --- |
| 主 CI | 六 producer manifest + `gamenet.ci_evidence_set.v1` | candidate `5ebad2c1...`；checkout `e461b597...` | `29160903594` / 1 | `ci-evidence-set-e461b597f2642e000717f536f3b430b804ba26ad-29160903594-1`；ZIP SHA-256 `408398ea5fdcf05675c819136a1ab5665d4dbe1fbcc5a537f45e5832d10cbf3a` | **通过**；下载六 producer 后由仓库 verifier 独立重算通过 |
| Linux Debug + consumer | 85/85 + consumer 1/1 | 同上 | `29160903594` / 1 | `ci-evidence-linux-cmake-e461b597f2642e000717f536f3b430b804ba26ad-29160903594-1`；ZIP `df14c0617c2eb7753a9d57ba45f05f3b260fc4c838277cd40e2f3c77374b8510` | **通过** |
| Linux ASan/UBSan + fuzz | 85/85；真实 libFuzzer 1000 units；corpus/dictionary/log | 同上 | `29160903594` / 1 | `ci-evidence-linux-asan-ubsan-e461b597f2642e000717f536f3b430b804ba26ad-29160903594-1`；ZIP `dc4a8177b33df5de707ccfa4927d372925058bc8d4c402f9b44c9e552b9048e6` | **通过** |
| Linux TSan | 85-test inventory 中精确选择 threading 61/61 | 同上 | `29160903594` / 1 | `ci-evidence-linux-tsan-e461b597f2642e000717f536f3b430b804ba26ad-29160903594-1`；ZIP `545ff700b819ff8daf80a20930d27c6e6616874282cfb4b9d70f88330a9677c5` | **通过** |
| Linux Release | 85/85 | 同上 | `29160903594` / 1 | `ci-evidence-linux-release-e461b597f2642e000717f536f3b430b804ba26ad-29160903594-1`；ZIP `2850e10fd2768e1213ea240532cdaf651d810070ac1ff5b892176d5d122e34e0` | **通过** |
| Windows Debug IOCP + consumer | 85/85 + consumer 1/1 | 同上 | `29160903594` / 1 | `ci-evidence-windows-msvc-e461b597f2642e000717f536f3b430b804ba26ad-29160903594-1`；ZIP `af853a7a6d98ec5581bc2e7924beff34560abdbac286c87361a38b1b5c46d69f` | **通过** |
| Windows Release IOCP + consumer | 85/85 + consumer 1/1 | 同上 | `29160903594` / 1 | `ci-evidence-windows-msvc-release-e461b597f2642e000717f536f3b430b804ba26ad-29160903594-1`；ZIP `f0aa6858e30cd52f47fc624ec6b2ab77eee1c4aa018f1a3bc3e46fbb4f8b8073` | **通过** |
| Long-soak | 两份 `gamenet.ctest_repeat_evidence.v1` + raw logs/inventory | `5ebad2c1...` direct checkout | `29161167423` / 1 | `long-soak-linux-long-soak-5ebad2c1a4a9487437340935e21f7468140c7e8d-29161167423-1`；ZIP `1e54b26681e39ff72dfb3dec3b9fddd2f0caa180871b4aa7f21051672c57b8c4` | **通过**；3,050/3,050 与 400/400，raw logs 独立复验 |
| Phase 4 benchmark pair | 两个 producer manifest + `gamenet.phase4_benchmark_pair_evidence.v1` | `5ebad2c1...` direct checkout | `29161168417` / 1 | Linux ZIP `0a7ebecff8889a19688da57e3f44f6726e4e9e9731f69e5d8a545bcc75344978`；Windows ZIP `cc77f51a94649afdd3e616f816cf7014effed7a8d7dceb28185dd90e43809e8b`；pair ZIP `a4ce4eda074928c1ca97fdb6171752fcf33028f684bd75e3b733a5e7734ce9a8` | **通过**；pair manifest 本地重算一致 |
| 最终 PR-head CI | 六 producer manifest + aggregate | head `4abd5960...`；checkout `7132faa1...` | `29162961320` / 1 | aggregate ZIP SHA-256 `6ab25ed3432de1778ec1d3a0ea17f2e2124fc9ac7c5f2a5b7662186a3d82580c` | **通过** |
| release-commit main CI | 六 producer manifest + `gamenet.ci_evidence_set.v1` | exact `7668d6b8...` | `29168786199` / 1 | `ci-evidence-set-7668d6b82a0d815ccd79f83c572bc0a36bcceea0-29168786199-1`；ZIP SHA-256 `2bd08a1bcb1c502ef5b6a79cd3a6cd79f0e687e6ce3bc2faeddfdcecda0aa9e3` | **通过**；六 producer 下载后由仓库 verifier 独立重算通过 |
| GitHub Release | annotated tag、Release URL、notes、evidence links | release commit `7668d6b8...` | published `2026-07-11T21:38:05Z` | tar.gz `c2f5f2ec...`；zip `4a75373a...`；`SHA256SUMS` asset `4df5d440...` | **通过**；三资产从正式 Release 重下载并复验 |

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
