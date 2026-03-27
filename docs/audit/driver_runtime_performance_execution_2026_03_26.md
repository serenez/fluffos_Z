# FluffOS 驱动运行时性能优化实施记录（2026-03-26）

## 1. 目标

基于 `docs/audit/driver_runtime_performance_audit_2026_03_26.md`
里的审计结论，按“先厘清语义边界，再落代码，再补回归和记录”的顺序，
逐项推进驱动运行时性能优化。

说明：

- 这里只记录实际实施过程，不重复展开完整问题分析。
- 每一项默认要求：
  - 先确认当前语义边界
  - 再动底层结构
  - 再补测试/基准
  - 最后回写记录

---

## 2. 当前阶段

### 阶段 A：`heartbeat` 调度重构

- 状态：`第二轮深挖完成，混合调度版已验证`
- 目标：
  - 去掉当前“每个 tick 扫全部 heartbeat 对象”的结构性成本
  - 保持 `set_heart_beat()`、`query_heart_beat()`、
    `heart_beats()`、运行中增删 heartbeat 的语义兼容
- 最终保留实现：
  - 新增测试/基准入口：
    - `src/packages/core/heartbeat.h`
    - `src/packages/core/heartbeat.cc`
    - `src/tests/heartbeat_bench.cc`
    - `src/tests/test_lpc.cc`
    - `testsuite/clone/heartbeat_probe.c`
  - 底层结构：
    - `object -> canonical heartbeat entry`
    - intrusive queue：`pending / current_round / every_round`
    - `due_round -> sparse bucket` 稀疏调度
  - 关键语义：
    - 新 heartbeat 仍在本轮结束后激活
    - heartbeat 执行中对自己 `set_heart_beat(0/1/N)` 仍可即时改 interval
    - 对“修改其他对象 heartbeat”的同轮行为，按 legacy 顺序补了 `seq` 保真
      规则：
      - 当前对象之前的目标，不会被错误拉回本轮
      - 当前对象之后的目标，仍可在本轮被提前或延后
    - `query_heart_beat()`、`get_heart_beats()`、
      `check_heartbeats()` 行为保持兼容
- 中途被否决的方案：
  - `std::set<due_tick>` 稀疏调度原型
    - 语义可跑通，但调度常数过大，已放弃
  - 第一版 `due_round -> bucket(list)` 调度
    - 稀疏场景收益明显，但 `interval=1` / mixed 负载退化严重
    - 已被当前混合调度版替换
- 已补回归：
  - `DriverTest.TestHeartbeatIntervalLifecycle`
  - `DriverTest.TestHeartbeatNewEntryActivatesAfterPendingDrain`
  - `DriverTest.TestHeartbeatSelfDisableEnableDoesNotDuplicateEntries`
  - `DriverTest.TestHeartbeatCanEnableOtherObjectForSameRoundExecution`
  - `DriverTest.TestHeartbeatCanDelayOtherObjectCurrentRoundExecution`
  - `DriverTest.TestHeartbeatDoesNotPullEarlierSparseObjectIntoCurrentRound`
- 基准方法：
  - 当前实现：
    - `C:\msys64\mingw64\bin\ninja.exe -C build_codex_review_fix lpc_tests heartbeat_bench`
    - `build_codex_review_fix/src/tests/heartbeat_bench.exe`
  - 基线实现：
    - 在 `b32443d9` 的干净临时 worktree 中，仅补最小 bench 钩子
    - 配置时显式 `-DPACKAGE_DB=OFF`
    - `C:\msys64\mingw64\bin\ninja.exe -C build_codex_review_fix heartbeat_bench`
  - 验证命令：
    - `build_codex_review_fix/src/tests/lpc_tests.exe --gtest_filter=DriverTest.TestHeartbeat*`
    - `ctest --test-dir build_codex_review_fix --output-on-failure -j 1`
- 本轮两轮复测对标：
  - `uniform_interval_1`
    - 基线 `186736~200752 ns/cycle`
    - 当前 `209760~214649 ns/cycle`
    - 结论：仍慢，但已经收敛到约 `5%~15%`
  - `uniform_interval_10`
    - 基线 `176218~178558 ns/cycle`
    - 当前 `31107~31714 ns/cycle`
    - 结论：稳定快约 `82%`
  - `uniform_interval_100`
    - 基线 `174676~176412 ns/cycle`
    - 当前 `4147~4407 ns/cycle`
    - 结论：稳定快约 `97.5%`
  - `mixed_interval_1_10`
    - 基线 `185434~187765 ns/cycle`
    - 当前 `118122~128361 ns/cycle`
    - 结论：稳定快约 `31%~37%`
  - `mixed_interval_1_100`
    - 基线 `178099~180259 ns/cycle`
    - 当前 `108569~108617 ns/cycle`
    - 结论：稳定快约 `39%`
- 当前判断：
  - 这版已经不再是“只对稀疏场景好看”的实验原型
  - 在 mixed 负载下已经整体转正
  - 剩下的主要短板是：
    - 全量 `interval=1` 仍慢约 `15%`
  - 也就是说：
    - 如果线上 heartbeat 大量是 `10/30/60` 或 mixed 分布，这版值得认真评估
    - 如果线上几乎清一色都是 `interval=1`，还要继续压 fast path
- 验证结果：
  - heartbeat 定向回归：`6/6` 通过
  - 全量 `ctest`：`61/61` 通过
- 下一步建议：
  - 若继续深挖 `heartbeat`，重点只剩：
    - 继续压 `uniform_interval_1` 常数项
    - 看是否要把 intrusive queue 再往“纯轮转快路径”推一步
  - 若切到下一个热点，`heartbeat` 这一项已经可以作为一个完整阶段收口

---

## 3. 后续待办

### B：命令分发链

- `move_object() + setup_new_commands() + remove_sent()`
- `user_parser()` sentence verb 索引

### 阶段 B1：`user_parser()` sentence verb 索引

- 状态：`首轮实现完成，基准和回归已验证`
- 目标：
  - 去掉当前 `user_parser()` 每次都线性扫完整 sentence 链的固定成本
  - 保持 `add_action` 命令优先级、`V_SHORT/V_NOSPACE`、空 verb fallback、
    `remove_sent()`、`query_verb()` 的现有语义
- 最终保留实现：
  - 新增测试/基准入口：
    - `src/packages/core/add_action.h`
    - `src/packages/core/add_action.cc`
    - `src/vm/internal/simulate.cc`
    - `src/tests/user_parser_bench.cc`
    - `src/tests/test_lpc.cc`
    - `testsuite/clone/user_parser_probe.c`
  - 底层结构：
    - `object -> lazy parse cache`
    - cache 分三类：
      - `exact verb`
      - `fallback`
      - `special(V_SHORT/V_NOSPACE)`
    - 执行前只收集可能命中的候选，再按原 sentence 链顺序执行
  - 关键语义：
    - `command_giver->sent` 仍是唯一真相源
    - 相同命令的实际执行顺序，仍以原 sentence 链顺序为准
    - `commands()`、`query_verb()`、`notify_fail()` 行为不变
    - 只优化候选查找，不改命令函数执行逻辑
  - 已接入的 cache 失效点：
    - `add_action()`
    - `remove_action()`
    - `remove_sent()`
    - `destruct2()`
- 已补回归：
  - `DriverTest.TestUserParserPreservesEmptyShortFallbackOrderAgainstExactVerb`
  - `DriverTest.TestUserParserPreservesExactBeforePrefixWhenChainRequiresIt`
  - `DriverTest.TestUserParserPreservesExplicitDefaultVerbOrderAgainstExactVerb`
  - `DriverTest.TestUserParserHonorsRemoveSentAfterPreviousParse`
- 基准方法：
  - 当前实现：
    - `build_codex_review_fix/src/tests/user_parser_bench.exe`
  - 基线实现：
    - 同一工作树先补 bench/回归夹具，再在未接索引前执行一次
    - 口径是 `avg_ns`
  - 验证命令：
    - `build_codex_review_fix/src/tests/lpc_tests.exe --gtest_filter=DriverTest.TestUserParser*`
    - `ctest --test-dir build_codex_review_fix --output-on-failure -j 1`
- 当前确认的 benchmark 结果：
  - `exact_miss_unique_4096`
    - 基线 `5825 ns`
    - 当前 `134 ns`
    - 结论：约 `43x`
  - `exact_tail_hit_unique_4096`
    - 基线 `4908 ns`
    - 当前 `261 ns`
    - 结论：约 `18.8x`
  - `short_fallback_plus_exact_tail_hit_4096`
    - 基线 `4997 ns`
    - 当前 `261 ns`
    - 结论：约 `19.1x`
- 验证结果：
  - `user_parser` 定向回归：`4/4` 通过
  - 全量 `ctest`：`65/65` 通过
- 当前判断：
  - 这条已经可以作为完整小闭环收口
  - 不建议继续把时间花在 `user_parser` 细抠上
  - 下一步应该切到：
    - `move_object() + setup_new_commands() + remove_sent()`
    - 或 `parse_command/packages/parser`

### 阶段 B2：`setup_new_commands()` 的 `init()` miss 快路径

- 状态：`第二轮实现完成，基准和回归已验证`
- 目标：
  - 去掉 `move_object() -> setup_new_commands()` 中大量“对象根本没有
    `init()` 仍然照常走完整 apply 路径”的固定成本
  - 保持 `init()` 调用顺序、`command_giver`、`remove_sent()`、
    `move_object()` 的现有兼容语义
- 最终保留实现：
  - 修改入口：
    - `src/packages/core/add_action.cc`
    - `src/vm/internal/base/program.h`
    - `src/compiler/internal/compiler.cc`
    - `src/tests/test_lpc.cc`
    - `testsuite/clone/move_chain_init.c`
    - `testsuite/clone/move_chain_noinit.c`
  - 底层结构：
    - `program_t` 新增 `init_lookup_state`
    - `setup_new_commands()` 改为：
      - 有 `init()` 的对象仍走原 `apply(APPLY_INIT, ...)`
      - 没有 `init()` 的对象只保留 driver 侧 miss 预处理：
        - `time_of_ref`
        - lazy reset 检查
        - `O_RESET_STATE`
    - shadow 链仍按原 `apply_low()` 的查找方向判断是否存在 `init()`
  - 关键语义：
    - `setup_new_commands()` 的遍历顺序完全不变
    - `dest -> item`、`ob -> item`、`item -> ob`、`dest -> item`
      四个方向的触发顺序不变
    - 只有“没有 `init()` 的目标对象”不再进入完整 apply 调用栈
- 中途被否决的方案：
  - 第一版：每次现查 `apply_cache_lookup(APPLY_INIT, prog)` 再决定是否 apply
    - 结论：mixed 负载略好，但
      `enabled item -> 全无 init 房间` 反而退化
    - 已放弃
  - 独立 `move_chain_bench.exe`
    - 结论：脱离 `lpc_tests` 生命周期后不够稳定
    - 已改成 `lpc_tests` 内 disabled perf case
- 已补回归：
  - `DriverTest.TestMoveObjectEnabledItemCallsInitObjectsInDestination`
  - `DriverTest.TestMoveObjectEnabledSiblingCallsMovedItemInit`
  - `DriverTest.TestMoveObjectLargeNoInitRoomSingleMoveCompletes`
- perf 入口：
  - `build_codex_review_fix/src/tests/lpc_tests.exe --gtest_filter=DriverTest.DISABLED_PerfMoveObjectEnabledItemRoomNoInit256 --gtest_also_run_disabled_tests`
  - `build_codex_review_fix/src/tests/lpc_tests.exe --gtest_filter=DriverTest.DISABLED_PerfMoveObjectEnabledItemRoomMixed256InitEvery8 --gtest_also_run_disabled_tests`
  - `build_codex_review_fix/src/tests/lpc_tests.exe --gtest_filter=DriverTest.DISABLED_PerfMoveObjectDisabledNoInitItemRoomEnabled64 --gtest_also_run_disabled_tests`
- benchmark 结果：
  - `enabled_item_room_noinit_256`
    - 基线 `2561 ns`
    - 第一版 `3153 ns`
    - 当前 `2286 ns`
    - 结论：相对基线约快 `10.7%`
  - `enabled_item_room_mixed_256_init_every_8`
    - 基线 `7869 ns`
    - 第一版 `7453 ns`
    - 当前 `7538 ns`
    - 结论：相对基线约快 `4.2%`
  - `disabled_noinit_item_room_enabled_64`
    - 基线 `632 ns`
    - 第一版 `605 ns`
    - 当前 `435 ns`
    - 结论：相对基线约快 `31.2%`
- 当前判断：
  - 这条优化已经可以作为 `move_object()/setup_new_commands()` 链的第一阶段保留实现
  - 它解决的是“无 `init()` 对象太多时的 driver 固定开销”
  - 它没有解决的仍然是：
    - 真正有 `init()` 的对象很多时的 apply 成本
    - `remove_sent()` 的按 owner 清理成本
  - 所以下一步应该切到：
    - `remove_sent()` / sentence owner 索引
    - 或 `parse_command/packages/parser`

### 阶段 B3：`remove_sent()` 的 owner 清理加速

- 状态：`首轮实现完成，基准和回归已验证`
- 目标：
  - 去掉 `move_object()` 离开旧环境时，
    `remove_sent(owner, user)` 对整条 sentence 主链线性扫描的固定成本
  - 保持 `commands()`、`user_parser()`、`remove_action()`、
    `move_object()` 的现有可见顺序和语义兼容
- 最终保留实现：
  - 修改入口：
    - `src/packages/core/add_action.cc`
    - `src/vm/internal/base/object.h`
    - `src/vm/internal/simulate.cc`
    - `src/tests/test_lpc.cc`
  - 底层结构：
    - `object_t` 新增 `sent_owners`
    - `sentence_t` 新增：
      - `prev`
      - `owner_next`
      - `owner_head_next`
    - `user->sent` 仍是可见主链
    - `sent_owners` 只是按 owner 分组的侧链头表
  - 关键语义：
    - `commands()` 和 `user_parser()` 仍按主链顺序工作
    - `remove_sent(owner, user)` 只删指定 owner 的 sentence
    - `remove_action()` 删除单个 sentence 时，也会同步维护 owner 侧链
    - `destruct2()` 批量释放 sentence 前先清 `sent_owners`
  - 为什么保留这版：
    - 不引入额外 bucket allocation
    - 不改主链模型
    - 只优化“更快找到 owner 对应的 sentence”
- 中途取舍：
  - 没采用独立 `remove_sent_bench.exe`
    - 原因：在当前 driver 生命周期下，不如 `lpc_tests` 内 disabled perf case 稳定
  - 也没采用 owner 全局额外分配表
    - 原因：实现更重，第一阶段没必要
- 已补回归：
  - `DriverTest.TestRemoveSentRemovesOnlyTargetOwnerCommands`
  - `DriverTest.TestRemoveSentHandlesInterleavedOwnerCommands`
  - `DriverTest.TestRemoveActionKeepsOwnerBucketsConsistentForLaterRemoveSent`
  - `DriverTest.TestUserParserHonorsRemoveSentAfterPreviousParse`
- perf 入口：
  - `build_codex_review_fix/src/tests/lpc_tests.exe --gtest_filter=DriverTest.DISABLED_PerfRemoveSentOwners256Actions8 --gtest_also_run_disabled_tests`
  - `build_codex_review_fix/src/tests/lpc_tests.exe --gtest_filter=DriverTest.DISABLED_PerfRemoveSentOwners1024Actions1 --gtest_also_run_disabled_tests`
  - 复测移动链：
    - `DriverTest.DISABLED_PerfMoveObjectEnabledItemRoomNoInit256`
    - `DriverTest.DISABLED_PerfMoveObjectEnabledItemRoomMixed256InitEvery8`
    - `DriverTest.DISABLED_PerfMoveObjectDisabledNoInitItemRoomEnabled64`
- benchmark 方法：
  - 新增 test-only 的 legacy 线性删除 helper
  - 在同一份 binary、同一套测试数据上，同时量：
    - 当前 driver `remove_sent()`
    - 旧线性扫描路径
  - 这样不用额外维护 baseline worktree，就能得到干净的算法对比
- benchmark 结果：
  - `remove_sent_owners_256_actions_8`
    - 当前 `456 ns`
    - legacy `3868 ns`
    - 结论：约快 `8.5x`
  - `remove_sent_owners_1024_actions_1`
    - 当前 `978 ns`
    - legacy `1777 ns`
    - 结论：约快 `1.8x`
  - `move_object()` 链复测：
    - `enabled_item_room_noinit_256`
      - B2 `2286 ns`
      - 当前 `1221 ns`
      - 结论：相对 B2 再快约 `46.6%`
    - `enabled_item_room_mixed_256_init_every_8`
      - B2 `7538 ns`
      - 当前 `5374 ns`
      - 结论：相对 B2 再快约 `28.7%`
    - `disabled_noinit_item_room_enabled_64`
      - B2 `435 ns`
      - 当前 `233 ns`
      - 结论：相对 B2 再快约 `46.4%`
- 验证结果：
  - `remove_sent` 定向语义回归：`4/4` 通过
  - 全量 `ctest`：`71/71` 通过
- 当前判断：
  - 这条优化已经不只是局部算法好看
  - 它已经把收益传导到了 `move_object()` 的真实路径上
  - 到这里为止，命令链这条主线已经有两个可保留阶段：
    - B2 `init()` miss 快路径
    - B3 `remove_sent()` owner 清理
  - 下一步应切到：
    - `parse_command`
    - `packages/parser`

### C：解析器链

- `parse_command`
- `packages/parser`

### 阶段 C1：`parse_command` 的命中项 memo

- 状态：`首轮实现完成，基准和回归已验证`
- 目标：
  - 在不改 `parse_command` 外部语义和刷新时机的前提下，
    压低 `match_object() -> find_string()` 里重复 phrase 扫描的成本
  - 严格避免把“激进缓存”带来的常数项回退引进来
- 最终保留实现：
  - 修改入口：
    - `src/packages/ops/parse.cc`
    - `src/packages/ops/parse.h`
    - `src/tests/test_lpc.cc`
    - `testsuite/clone/parse_command_probe.c`
  - 底层结构：
    - 每次 `parse()` 调用内维护轻量 memo
    - key 是 `(needle,start_index)`
    - 只缓存成功命中的结果
  - 关键语义：
    - 不改 `parse()` 的整体流程
    - 不改 `deep_inventory()` 候选范围
    - 不改 `%i` 返回值结构
    - 不改 plural / adjective 的匹配规则
    - 不改 master/object 词表拉取时机
- 中途否掉的方案：
  - `unordered_map` 版 `find_string` 全量缓存
    - 结论：哈希常数项太大，benchmark 退化
  - `member_string()` 对每个数组建 `unordered_set`
    - 结论：对象词表构建成本过高，benchmark 退化
  - `find_string` 连 miss 一起缓存
    - 结论：噪声 id 太多时，miss cache 会把一次性词条大量塞进缓存，
      明显拖慢路径
- 已补回归：
  - `DriverTest.TestParseCommandMatchesLegacyPluralWithAdjectives`
  - `DriverTest.TestParseCommandMatchesLegacyPluralSelection`
- perf 入口：
  - `build_codex_review_fix/src/tests/lpc_tests.exe --gtest_filter=DriverTest.DISABLED_PerfParseCommandRoomShared256Ids8Adjs4 --gtest_also_run_disabled_tests`
  - `build_codex_review_fix/src/tests/lpc_tests.exe --gtest_filter=DriverTest.DISABLED_PerfParseCommandRoomShared512Ids4Adjs8 --gtest_also_run_disabled_tests`
- benchmark 方法：
  - 继续使用 `lpc_tests.exe` 内 disabled perf case
  - 新增 test-only 的 legacy mode 开关
  - 在同一份 binary、同一套 room/object 夹具上直接对比：
    - 当前实现
    - legacy mode
  - command 固定为共享 plural + adjective，刻意放大 noun/adjective
    匹配的重复扫描
- benchmark 结果：
  - `parse_command_room_shared_256_ids_8_adjs_4`
    - 当前 `101036 ns`
    - legacy `111780 ns`
    - 结论：约快 `9.6%`
  - `parse_command_room_shared_512_ids_4_adjs_8`
    - 当前 `200201 ns`
    - legacy `226069 ns`
    - 结论：约快 `11.4%`
- 验证结果：
  - `parse_command` 定向语义回归：`2/2` 通过
  - 全量 `ctest`：`73/73` 通过
- 当前判断：
  - 这条优化收益中等，但兼容性边界很干净
  - 当前实现可以作为 `parse_command` 的第一阶段保留版本
  - 后续如果继续深挖，优先级建议是：
    - pattern token cache
    - master 默认词表 cache
  - 不建议回到：
    - miss cache
    - 每对象词表哈希化

### 阶段 C2：`packages/parser` 的 remote users 惰性加载

- 状态：`首轮实现完成，基准和回归已验证`
- 目标：
  - 去掉 `packages/parser` 在 no-remote verb 上的无意义 master/users 成本
  - 保持 remote livings 打开时的既有可见范围和匹配行为
- 最终保留实现：
  - 修改入口：
    - `src/packages/parser/parser.cc`
    - `src/packages/parser/parser.h`
    - `src/tests/test_lpc.cc`
    - `testsuite/single/master.c`
    - `testsuite/clone/parser_perf_handler.c`
    - `testsuite/clone/parser_perf_probe.c`
  - 底层结构：
    - `interrogate_master()` 改成按位加载：
      - literals
      - specials
      - users
    - `parse_add_rule()` 只拉 literals + specials
    - `load_objects()` 先扫描当前 verb 的 handler 集，
      判断这次解析是否真的需要 remote users
    - 只有需要时，才调用：
      - `parse_command_users`
      - `init_users()`
      - 远端 users 追加进 `loaded_objects`
  - test-only 支撑：
    - `set_parser_legacy_master_user_loading_for_test()`
      用同一份 binary 直接对比 legacy/current
    - master 增加 `parse_command_users()` 观测钩子，
      可以伪造远端 users 并统计调用次数
- 关键语义：
  - 不改 `parse_sentence()` 的外部接口
  - 不改 remote livings 打开时的对象候选范围
  - 不改 noun/plural/adjective 与 object relation 的匹配链
  - 只收紧“不需要 remote users 时的准备工作”
- 已补回归：
  - `DriverTest.TestParserAddRuleDoesNotFetchMasterUsers`
  - `DriverTest.TestParserSkipsRemoteUsersWhenVerbDoesNotNeedThem`
  - `DriverTest.TestParserRemoteUsersRemainReachableWhenEnabled`
- perf 入口：
  - `build_codex_review_fix/src/tests/lpc_tests.exe --gtest_filter=DriverTest.DISABLED_PerfParserNoRemoteUsersOverride2048 --gtest_also_run_disabled_tests`
  - `build_codex_review_fix/src/tests/lpc_tests.exe --gtest_filter=DriverTest.DISABLED_PerfParserRemoteUsersOverride2048 --gtest_also_run_disabled_tests`
- benchmark 方法：
  - 用 master 测试钩子注入 2048 个远端 users
  - `no-remote` case 只保留 1 个本地命中对象，
    专门量“当前 verb 不需要 remote users 时，
    driver 还要不要白扫一遍 master users”
  - `remote` case 保持 2048 个远端 users 可达，
    用来验证开启 remote 时没有语义/性能倒退
- benchmark 结果：
  - `parser_no_remote_users_override_2048`
    - 当前 `3036 ns`
    - legacy `2762194 ns`
  - `parser_remote_users_override_2048`
    - 当前 `2750310 ns`
    - legacy `2994003 ns`
- 验证结果：
  - parser/parse_command 定向回归：`5/5` 通过
  - 全量 `ctest`：`76/76` 通过
- 额外说明：
  - disabled perf case 对 bench 自建 clone 额外 pin 了一份 ref，
    只为了把 cleanup 阶段的 synthetic ref 抖动隔离出 benchmark
  - 这部分只存在于 perf 夹具，不影响运行时逻辑
- 当前判断：
  - 这轮可以作为 `packages/parser` 的第一阶段保留实现
  - 真正下一步如果继续深挖，优先级应转向：
    - 缩小 `load_objects()` 的全量重建范围
    - 把 object/hash rebuild 做成更细粒度的 dirty 刷新
  - 不建议马上去做跨 parse 的大缓存，
    因为刷新边界和可见性语义风险仍然高

### D：调度与输入链

- gametick / `call_out`
- 网络输入缓冲与 MUD 包提取

### 阶段 D1：`gametick` 队列按 due tick 分桶

- 状态：`首轮实现完成，基准和回归已验证`
- 目标：
  - 去掉当前 `gametick` 上“每个事件一个 `multimap` 节点”的结构性成本
  - 保持 `call_out(0)`、同 tick FIFO、事件失效和 `walltime` 边界兼容
- 最终保留实现：
  - 修改入口：
    - `src/backend.h`
    - `src/backend.cc`
    - `src/tests/gametick_bench.cc`
    - `src/tests/test_lpc.cc`
    - `src/tests/CMakeLists.txt`
  - 底层结构：
    - `map<due_tick, vector<TickEvent *>>`
    - `add_gametick_event()`：
      - 创建/定位 due tick bucket
      - 顺序 `push_back` 事件
    - `call_tick_events()`：
      - 每轮只取当前最早到期 bucket
      - `move` 出 bucket 后按原顺序执行
      - 回调中新加的 `delay=0` 事件，进入新 bucket，
        因此仍排在当前已抽出的批次之后
    - `TickEvent` 对 callback 改成 move 构造，减少一份 `std::function` 拷贝
  - 关键语义：
    - 同 tick 内执行顺序不变
    - 回调里追加的同 tick 事件，仍晚于当前批次执行
    - `TickEvent::valid=false` 仍能取消同 tick 尚未执行的事件
    - `add_walltime_event()` 路径完全不变
- 中途否掉的方案：
  - intrusive list bucket
    - 结论：插入路径可接受，但执行阶段 cache locality 很差，
      `dispatch` benchmark 退化
    - 已放弃
- 已补回归：
  - `DriverTest.TestGametickPreservesInsertionOrderWithinSameTick`
  - `DriverTest.TestGametickSchedulesNewSameTickEventsAfterCurrentBatch`
  - `DriverTest.TestGametickSkipsInvalidatedPendingSameTickEvent`
- benchmark 入口：
  - `build_codex_review_fix/src/tests/gametick_bench.exe`
- benchmark 方法：
  - 口径只量 scheduler 本体，不混入 LPC 业务逻辑
  - 输出：
    - `schedule_avg_ns`
    - `dispatch_avg_ns`
  - 基线是改 `backend` 前同一份 bench 跑出的数字
- benchmark 结果：
  - `same_tick_50000`
    - 基线：
      - `schedule_avg_ns=86`
      - `dispatch_avg_ns=28`
    - 当前：
      - `schedule_avg_ns=22`
      - `dispatch_avg_ns=10`
  - `shared_16_ticks_50000`
    - 基线：
      - `schedule_avg_ns=53`
      - `dispatch_avg_ns=27`
    - 当前：
      - `schedule_avg_ns=9`
      - `dispatch_avg_ns=10`
  - `shared_256_ticks_50000`
    - 基线：
      - `schedule_avg_ns=41`
      - `dispatch_avg_ns=28`
    - 当前：
      - `schedule_avg_ns=10`
      - `dispatch_avg_ns=9`
- 验证结果：
  - `DriverTest.TestGametick*`：`3/3` 通过
  - 全量 `ctest`：活动用例 `79/79` 通过
- 当前判断：
  - 这条已经可以作为 `gametick/call_out` 的第一阶段保留实现
  - 它主要解决的是：
    - 同 tick / 少量共享 due tick 的调度容器常数项
  - 它还没有碰：
    - `call_out` 对象索引垃圾累积
    - `pending_call_t` 分配模型
    - timing wheel / 分层时间轮
  - 所以下一步如果继续深挖这条线，应该转去：
    - `call_out` 索引与回收
    - 或者直接切下一个热点：网络输入链

### 阶段 D2：`call_out` owner 索引去垃圾化

- 状态：`首轮实现完成，基准和回归已验证`
- 目标：
  - 去掉旧 `object -> handle` 索引里会持续累积的陈旧条目
  - 让 `find_call_out(name)` / `remove_call_out(name)` / `remove_all_call_out(obj)`
    只扫当前 owner 的活跃 callout
  - 不改现有 efun 接口和时间语义
- 最终保留实现：
  - 修改入口：
    - `src/packages/core/call_out.cc`
    - `src/packages/core/call_out.h`
    - `src/tests/test_lpc.cc`
    - `src/tests/call_out_bench.cc`
    - `src/tests/CMakeLists.txt`
    - `testsuite/clone/call_out_probe.c`
  - 底层结构：
    - `handle -> pending_call_t *` 快查表保留
    - `owner object -> pending_call_t *head` 改成 intrusive 双向链
    - `pending_call_t` 新增 owner bucket 链字段
    - 所有 callout 死亡路径都会即时摘 owner bucket：
      - 执行完成
      - `remove_call_out(handle/name)`
      - `remove_all_call_out(obj)`
      - `clear_call_outs()`
      - `reclaim_call_outs()`
  - 关键语义：
    - 不改 `call_out/call_out_walltime` 对外行为
    - 不改 `find_call_out(handle)` 的 owner 校验
    - 不改 `remove_call_out(handle)` 当前的宽松语义
    - 只改 object-scoped 索引的内部维护方式
- 已补回归：
  - `DriverTest.TestCallOutByHandleRemovalDoesNotLeaveOwnerIndexGarbage`
  - `DriverTest.TestExecutedCallOutDoesNotLeaveOwnerIndexGarbage`
  - `DriverTest.TestRemoveAllCallOutClearsOwnerIndexBucket`
- benchmark 入口：
  - `build_codex_review_fix/src/tests/call_out_bench.exe`
- benchmark 方法：
  - 同一对象上先批量创建 future callout
  - 再用 `remove_call_out(handle)` 全部删掉
  - 最后只留下 1 个 live callout，再测一次 `find_call_out(name)`
  - 输出里的 `owner_index_*_delta=1` 是预期值：
    - 表示 bench 结束时只剩最后那个 live callout
- benchmark 结果：
  - `find_after_handle_removed_same_object_10000`
    - 基线：
      - `avg_ns=2865162`
      - `owner_index_entries=400040`
      - `owner_index_buckets=712697`
    - 当前：
      - `avg_ns=2146905`
      - `owner_index_entries_delta=1`
      - `owner_index_buckets_delta=1`
  - `find_after_handle_removed_same_object_50000`
    - 基线：
      - `avg_ns=15385510`
      - `owner_index_entries=900050`
      - `owner_index_buckets=1447153`
    - 当前：
      - `avg_ns=10504150`
      - `owner_index_entries_delta=1`
      - `owner_index_buckets_delta=1`
- 验证结果：
  - `DriverTest.*CallOut*`：`3/3` 通过
  - 全量 `ctest`：活动用例 `82/82` 通过
- 当前判断：
  - 这条已经可以作为 `call_out` 的第一阶段保留实现
  - 到这里为止，调度链已收口两步：
    - D1 `gametick` 分桶
    - D2 `call_out` owner 索引去垃圾化
  - 下一步如果继续留在这条线上，才值得看：
    - `pending_call_t` 分配与池化
    - timing wheel / 分层时间轮
  - 如果切热点，优先项应回到：
    - 网络输入缓冲 / MUD 包提取

### 阶段 D3：文本输入缓冲命令终止符缓存

- 状态：`首轮实现完成，基准和回归已验证`
- 目标：
  - 去掉 websocket/telnet/gateway 文本输入链里
    `cmd_in_buf()` 和 `first_cmd_in_buf()` 对同一段命令的重复扫描
  - 保持 `SINGLE_CHAR`、`CRLF`、`SKIP_COMMAND`、
    `process_user_command()` 的现有可见语义不变
- 最终保留实现：
  - 修改入口：
    - `src/interactive.h`
    - `src/user.h`
    - `src/user.cc`
    - `src/comm.h`
    - `src/comm.cc`
    - `src/packages/gateway/gateway_session.cc`
    - `src/tests/test_lpc.cc`
    - `src/tests/input_buffer_bench.cc`
  - 底层结构：
    - `interactive_t` 新增 `text_command_end`
    - `cmd_in_buf()` 首次扫到的第一个 `\r/\n` 会缓存到 `interactive`
    - `first_cmd_in_buf()` 直接复用缓存，不再二次扫描同一条命令
    - `interactive_compact_text()` 会同步平移缓存位置
    - 缓冲改写点统一失效缓存：
      - `on_user_input()`
      - `gateway_session` 输入注入
      - `clean_buf()` 的 skip/前移
      - `interactive_reset_text()`
  - 刻意不做的事：
    - 不把 `MUD` 包多包批提取一起塞进这一轮
    - 原因是那条会碰：
      - `process_input()` 重入
      - `exec/destruct`
      - interactive 所属对象转移后的早停语义
- 已补回归：
  - `DriverTest.TestCmdInBufCachesBufferedCommandEndAcrossExtraction`
  - `DriverTest.TestOnUserInputBackspaceInvalidatesCachedCommandEnd`
  - 联跑已有输入链回归：
    - `TestMudPacketCompletionRequiresFullPayloadBytes`
    - `TestExtractFirstCommandPreservesLastBufferedLineCommand`
    - `TestProcessUserCommandKeepsLastLineBufferedCommandIntact`
    - `TestAsciiChunkProcessesEmptyLineSafely`
    - `TestAsciiChunkProcessesCommandAfterCrLfInSameRead`
    - `TestAsciiChunkProcessesBareCarriageReturn`
    - `TestAsciiChunkContinuesAfterExecTransfersInteractive`
    - `TestAsciiChunkStopsAfterProcessInputDestructsObject`
    - `TestProcessUserCommandSchedulesNextCommandAfterExec`
- 基准方法：
  - 新增 `build_codex_review_fix/src/tests/input_buffer_bench.exe`
  - 在同一份 binary 内同时跑：
    - 当前实现：命令终止符缓存
    - legacy 对照：旧的双扫描 helper
  - 口径只量 driver 侧：
    - `cmd_in_buf()`
    - `first_cmd_in_buf()`
- benchmark 结果：
  - `single_line_256`
    - 当前 `38 ns`
    - legacy `103 ns`
    - 结论：约快 `63.1%`
  - `single_line_4096`
    - 当前 `60 ns`
    - legacy `1267 ns`
    - 结论：约快 `95.3%`
  - `two_lines_4096_4096`
    - 当前 `91 ns`
    - legacy `1425 ns`
    - 结论：约快 `93.6%`
- 验证命令：
  - `build_codex_review_fix/src/tests/lpc_tests.exe --gtest_filter=DriverTest.TestMudPacketCompletionRequiresFullPayloadBytes:DriverTest.TestExtractFirstCommandPreservesLastBufferedLineCommand:DriverTest.TestCmdInBufCachesBufferedCommandEndAcrossExtraction:DriverTest.TestOnUserInputBackspaceInvalidatesCachedCommandEnd:DriverTest.TestProcessUserCommandKeepsLastLineBufferedCommandIntact:DriverTest.TestAsciiChunkProcessesEmptyLineSafely:DriverTest.TestAsciiChunkProcessesCommandAfterCrLfInSameRead:DriverTest.TestAsciiChunkProcessesBareCarriageReturn:DriverTest.TestAsciiChunkContinuesAfterExecTransfersInteractive:DriverTest.TestAsciiChunkStopsAfterProcessInputDestructsObject:DriverTest.TestProcessUserCommandSchedulesNextCommandAfterExec`
  - `build_codex_review_fix/src/tests/input_buffer_bench.exe`
  - `ctest --test-dir build_codex_review_fix --output-on-failure -j 1`
- 验证结果：
  - 输入链定向回归：`11/11` 通过
  - 全量 `ctest`：活动用例 `84/84` 通过
- 当前判断：
  - 这条已经可以作为网络输入路径的第一阶段保留实现
  - 它解决的是：
    - 文本命令缓冲的重复扫描
  - 它还没解决的是：
    - `MUD` 包一次读到多包时的批量提取
    - `process_input()` 之间的跨包早停/重入语义
  - 所以下一步如果继续留在这条线上，唯一值得做的就是：
    - `MUD` 多包批提取
    - 并把 `exec/destruct/interactive 转移` 的语义边界单独补齐

### 阶段 D4：MUD 多包批提取

- 状态：`首轮实现完成，基准和回归已验证`
- 目标：
  - 去掉 `PORT_TYPE_MUD` 一次 read 只处理 1 个完整包的限制
  - 保持半包缓存、`exec/destruct` 早停和包格式兼容
- 最终保留实现：
  - 修改入口：
    - `src/comm.cc`
    - `src/comm.h`
    - `src/tests/test_lpc.cc`
    - `src/tests/mud_input_bench.cc`
    - `src/tests/CMakeLists.txt`
    - `testsuite/clone/mud_input_probe.c`
  - 底层结构：
    - `get_user_data()` 对 `PORT_TYPE_MUD` 走 `get_user_data_mud()`
    - chunk append 后，循环提取并处理缓冲里的所有完整包
    - 新增 `process_mud_chunk_for_test()` 作为无 socket 的测试入口
  - 关键语义：
    - 半包仍保留在 `interactive->text`
    - 多完整包按原顺序逐个进入 `process_input`
    - `exec` 转移 interactive 后，后续包继续交给新的 owner
    - 对象析构或 interactive 脱离后，后续包立即停止
    - MUD 包格式不变
- 已补回归：
  - `DriverTest.TestMudPacketCompletionRequiresFullPayloadBytes`
  - `DriverTest.TestGetUserDataMudProcessesMultiplePacketsFromSingleBufferedRead`
  - `DriverTest.TestGetUserDataMudPreservesPartialSecondPacketAcrossReads`
  - `DriverTest.TestGetUserDataMudContinuesAfterExecTransfersInteractive`
  - `DriverTest.TestGetUserDataMudStopsAfterProcessInputDestructsObject`
- benchmark 入口：
  - `build_codex_review_fix/src/tests/mud_input_bench.exe`
- benchmark 方法：
  - 同一份 binary 内对比：
    - `current`：单次 chunk 尽量把完整包都处理完
    - `legacy`：每轮只处理 1 个完整包
  - 输出包含：
    - `current_avg_ns / legacy_avg_ns`
    - `current_rounds_avg / legacy_rounds_avg`
- benchmark 结果：
  - `mud_packets_32_payload_16`
    - 当前 `12371 ns`
    - legacy `12273 ns`
    - `current_rounds_avg=1`
    - `legacy_rounds_avg=32`
  - `mud_packets_128_payload_16`
    - 当前 `49935 ns`
    - legacy `49629 ns`
    - `current_rounds_avg=1`
    - `legacy_rounds_avg=128`
  - `mud_packets_128_payload_128`
    - 当前 `71279 ns`
    - legacy `70540 ns`
    - `current_rounds_avg=1`
    - `legacy_rounds_avg=128`
- 验证命令：
  - `build_codex_review_fix/src/tests/lpc_tests.exe --gtest_filter=DriverTest.TestMudPacketCompletionRequiresFullPayloadBytes:DriverTest.TestGetUserDataMudProcessesMultiplePacketsFromSingleBufferedRead:DriverTest.TestGetUserDataMudPreservesPartialSecondPacketAcrossReads:DriverTest.TestGetUserDataMudContinuesAfterExecTransfersInteractive:DriverTest.TestGetUserDataMudStopsAfterProcessInputDestructsObject`
  - `build_codex_review_fix/src/tests/mud_input_bench.exe`
  - `ctest --test-dir build_codex_review_fix --output-on-failure -j 1`
- 验证结果：
  - MUD 输入定向回归：`5/5` 通过
  - 全量 `ctest`：活动用例 `88/88` 通过
- 当前判断：
  - 这条可以作为网络输入路径的第二阶段保留实现
  - 它的主要收益是：
    - burst 输入下回合数显著下降
    - 吞吐/延迟路径更顺
  - 它不是一个强 `CPU microbench` 收益项

### 阶段 D5：`call_out` 的 `pending_call_t` 复用池

- 状态：`首轮实现完成，基准和回归已验证`
- 目标：
  - 去掉 `new_call_out()/remove_call_out_by_handle()` 高频 churn 下的
    `DCALLOC/FREE` 抖动
  - 保持 `call_out` 时间语义、owner 索引和 `TickEvent` 失效语义不变
- 最终保留实现：
  - 修改入口：
    - `src/packages/core/call_out.cc`
    - `src/packages/core/call_out.h`
    - `src/tests/test_lpc.cc`
    - `src/tests/call_out_bench.cc`
  - 底层结构：
    - 新增有上限的 `pending_call_t` free list
    - 池子上限固定 `16384`
    - `new_call_out()` 优先复用池节点，取不到才分配
    - `free_call()/free_called_call()` 在释放外部资源后再回收 struct 本体
    - `clear_call_outs()` 额外清空池，避免退出/测试隔离时残留
    - `print_call_out_usage()` / `total_callout_size()` 已计入池子占用
  - 关键语义：
    - `tick_event->valid=false` 语义保持不变
    - owner 索引、handle 索引和 `find/remove/remove_all` 行为不变
    - 不做无界池，避免一次性峰值直接变长期常驻内存
- 已补回归：
  - `DriverTest.TestCallOutPoolReusesFreedEntriesWhenEnabled`
  - `DriverTest.TestCallOutPoolCanBeDisabledForLegacyBenchComparison`
  - 联跑已有 3 条 `call_out` 索引语义回归
- benchmark 入口：
  - `build_codex_review_fix/src/tests/call_out_bench.exe`
- benchmark 方法：
  - 同一份 binary 内对比：
    - `current`：启用复用池
    - `legacy`：关闭池，回到直接分配/释放
  - 口径只量：
    - `new_call_out()`
    - `remove_call_out_by_handle()`
    这条批量创建/撤销路径
- benchmark 结果：
  - `schedule_remove_same_object_10000`
    - 当前 `1512645 ns`
    - legacy `2041520 ns`
    - `pooled_entries_after_warmup=10000`
    - 结论：约快 `25.9%`
  - `schedule_remove_same_object_50000`
    - 当前 `11775890 ns`
    - legacy `11722820 ns`
    - `pooled_entries_after_warmup=16384`
    - 结论：基本持平，约慢 `0.5%`
    - 原因：池子有上限，超大峰值 churn 只做部分复用
- 验证命令：
  - `build_codex_review_fix/src/tests/lpc_tests.exe --gtest_filter=*CallOut*`
  - `build_codex_review_fix/src/tests/call_out_bench.exe`
  - `ctest --test-dir build_codex_review_fix --output-on-failure -j 1`
- 验证结果：
  - `call_out` 定向回归：`5/5` 通过
  - 全量 `ctest`：活动用例 `90/90` 通过
- 当前判断：
  - 这条可以作为 `call_out` 的第三阶段保留实现
  - 它适合解决：
    - 常见批量 `new/remove` callout 的 allocator 抖动
  - 它不追求：
    - 极端大峰值 churn 的满额吞吐
    - 更激进的时间轮设计

### E：长期项

- VM dispatch

---

## 4. 记录规则

- 每进入一个新项，目前文件都要先补“目标、边界、状态”。
- 每完成一个小闭环，回写：
  - 修改范围
  - 测试与基准
  - 兼容性结论
- 正式提交后，再把提交信息同步到
  `docs/driver/maintenance_journal.md`。
