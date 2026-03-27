# FluffOS 驱动运行时性能审计（2026-03-26）

## 1. 结论

这轮从驱动层看，当前最值得关注的性能点，不是 JSON，也不是单个
`unordered_map`/`vector` 选型问题，而是几条会被高频运行时路径反复放大的
“结构性成本”：

1. `heartbeat` 调度每个 tick 都扫描全部 heartbeat 对象
2. `move_object()` 进入/离开环境时会触发 `init/remove_sent` 风暴
3. `user_parser()` 仍按 sentence 链线性扫命令
4. `parse_command` 旧链路与 `packages/parser` 新链路都偏重
5. gametick / `call_out` 调度器每个事件都有额外容器和分配成本
6. 网络输入缓冲存在重复扫描，MUD 包输入不支持批量提取
7. VM 主解释器仍是传统大 `switch` 分发

这几项里，按“潜在热点强度”看，我建议的优先级顺序是：

1. `heartbeat`
2. `move_object() + setup_new_commands() + remove_sent()`
3. `user_parser()`
4. `parse_command` / `packages/parser`
5. gametick / `call_out`
6. 网络输入路径
7. VM 分发

---

## 2. 审计说明

- 目标：
  - 从网络、VM、解析器、命令系统、调度器等角度，整理当前驱动层的主要性能点。
  - 对每个点写清楚：`问题是什么`、`会造成什么影响`、`怎么优化`、
    `会不会影响当前逻辑`。
- 范围：
  - 本文只看驱动层源码，不讨论 mudlib 侧使用方式。
  - 重点覆盖 `src/comm.cc`、`src/backend.cc`、`src/packages/core`、
    `src/packages/ops`、`src/packages/parser`、`src/vm/internal/base`。
- 方法：
  - 基于当前仓库源码做静态分析。
  - 本文不是 flamegraph，也不是压力测试结果。
- 说明：
  - 这里记录的是“值得优化的点”，不是“已经确认一定是全服最大热点”。
  - 真正要排落地顺序，后续最好再补定向 benchmark / tracing。

---

## 3. 优先级总览

这里分两层看：

- `潜在收益排序`：更像真实热点、命中后收益可能更大的点
- `建议落地顺序`：兼顾兼容风险、实现复杂度、验证成本后的实际施工顺序

### P1：建议优先处理

- `heartbeat` 全量扫描
- `move_object()` / `setup_new_commands()` / `remove_sent()` 命令链
- `user_parser()` sentence 线性扫描

### P2：收益可能很高，但要更小心

- `parse_command` 旧解析器
- `packages/parser` 新解析器
- gametick / `call_out` 调度器

### P3：中长期优化项

- 网络输入缓冲与 MUD 包批量提取
- VM computed goto / threaded dispatch

### 暂不建议优先动

- JSON 包
- apply cache
- tracing 关闭状态下的常规路径

原因很简单：这些地方当前实现并不是本轮看到的主矛盾。

---

## 4. 逐项分析

## PERF-RT-01 `heartbeat` 调度每个 tick 都扫描全部 heartbeat 对象

- 位置：
  - `src/packages/core/heartbeat.cc:34`
  - `src/packages/core/heartbeat.cc:58`
  - `src/packages/core/heartbeat.cc:183`
- 当前问题：
  - `call_heart_beat()` 每次触发时，都会先取 `heartbeats.size()`，
    然后把当前队列里的每个对象都弹出、递减 `heart_beat_ticks`、
    再决定是否回塞。
  - 这意味着即使某个对象的 heartbeat 间隔是 `10`、`30`、`60`，
    它依然会在每一个全局 heartbeat tick 上被访问一次。
- 会造成什么影响：
  - steady-state 成本是 `O(全部 heartbeat 对象)`，而不是
    `O(本 tick 实际到期的 heartbeat 对象)`。
  - 对 NPC、玩家、战斗对象都大量挂 heartbeat 的服，这条会长期吃 CPU。
  - 间隔越大的 heartbeat，当前实现浪费越明显。
- 怎么优化：
  - 方向一：改成“按到期 tick 组织”的结构，比如：
    - timing wheel
    - bucket queue
    - 小根堆 / min-heap
  - 目标是让当前 tick 只处理“现在到期”的 heartbeat。
  - `set_heart_beat()` 仍然保留现有接口，只改底层调度结构。
- 对当前逻辑会不会有影响：
  - `中`
  - 只要保持以下语义不变，玩法逻辑理论上不该变：
    - `set_heart_beat(ob, 0)` 的即时停用语义
    - heartbeat 执行中修改自身 interval 的行为
    - `heartbeats_pending` 这类“本轮执行后再并入”的保护语义
    - `query_heart_beat()` 返回值语义
  - 真正要小心的是“同一个 tick 内多个 heartbeat 的执行顺序”。
    如果 mudlib 暗中依赖当前顺序，调度结构变化可能带来边缘差异。

### 实施反馈（2026-03-27）

- 已做过三轮底层实现验证：
  - 第一轮：`std::set<due_tick>` 稀疏调度
    - 结论：语义能跑通，但调度常数过大，已放弃
  - 第二轮：`due_round -> bucket(list)` 分桶调度
    - 结论：稀疏场景收益明显，但 `interval=1` / mixed 负载退化严重
  - 第三轮：`every_round + current_round + sparse bucket` 混合调度
    - 结论：mixed 负载已经整体转正，且顺序语义补齐到了可接受状态
- 第三轮当前实现要点：
  - `object -> canonical heartbeat entry`
  - intrusive queue 管理 `pending / current_round / every_round`
  - `due_round -> sparse bucket`
  - 通过 `seq` 保留 legacy 同轮顺序语义：
    - 当前对象之后的目标，仍可被提前/延后到本轮
    - 当前对象之前的睡眠目标，不会被错误拉回本轮
- 基线对比方式：
  - 基线取自 `b32443d9`
  - 在干净临时 worktree 中，仅补最小 bench 钩子进行对标
- 当前确认的两轮 benchmark 结果：
  - `uniform_interval_1`
    - 基线 `186736~200752 ns/cycle`
    - 当前 `209760~214649 ns/cycle`
    - 结论：仍慢，但只剩约 `5%~15%`
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
- 阶段性判断：
  - 这条优化方向已经从“实验原型”进入“可以认真评估”的状态
  - mixed 负载和稀疏 heartbeat 下，当前实现是成立的
  - 剩下的主要短板只在：
    - 几乎全量 `interval=1` 的纯高密度场景
- 当前建议更新为：
  - `heartbeat` 仍然值得继续优化
  - 但后续工作只需要聚焦：
    - `interval=1` 快路径常数项
    - 是否需要把当前 intrusive queue 再向纯轮转模型收一步
  - 不再建议回到：
    - `std::set`
    - 纯 bucket 且没有 `interval=1` 专用处理的方案

---

## PERF-RT-02 `move_object()` 进入/离开环境时会触发 `init/remove_sent` 风暴

- 位置：
  - `src/vm/internal/simulate.cc:1581`
  - `src/vm/internal/simulate.cc:1607`
  - `src/vm/internal/simulate.cc:1614`
  - `src/packages/core/add_action.cc:215`
  - `src/packages/core/add_action.cc:668`
- 当前问题：
  - `move_object()` 在对象离开旧环境时，会对旧环境、旧环境内对象、
    被移动对象之间反复调用 `remove_sent()`。
  - 进入新环境后，又会调用 `setup_new_commands()`。
  - `setup_new_commands()` 会在：
    - `item -> dest`
    - `ob -> item`
    - `item -> ob`
    - `dest -> item`
    这些方向上反复触发 `APPLY_INIT`。
  - 房间里对象越多、玩家越多、携带物越多，这条链的放大效应越明显。
- 会造成什么影响：
  - 玩家移动、NPC 跟随、捡取/放下物品、进出容器时，
    延迟会明显随环境复杂度上升。
  - 这条链会把“房间大小”“物品数量”“可交互对象数量”直接转成 CPU 开销。
  - 如果房间系统复杂，这比 JSON、日志之类的点更接近体感卡顿。
- 怎么优化：
  - 第一阶段不要碰 `init()` 语义，先压结构性重复工作：
    - sentence 结构建立 verb 索引或 owner 索引
    - `remove_sent()` 做更快的按 owner 清理
    - 对同一次 move 的批量改动做合并处理
  - 第二阶段再考虑：
    - 把 action 依附关系改成“环境世代号 + 增量刷新”
    - 避免每次 move 都把所有关系完全拆掉再重建
- 对当前逻辑会不会有影响：
  - `高`
  - `init()` 的调用顺序、谁先拿到谁的 action、房间和物品谁覆盖谁，
    都可能被 mudlib 依赖。
  - 这里能优化，但必须把“调用顺序”和“可见集合”当成兼容约束来做。
  - 不建议直接改行为，只建议先做“更快地得到同样结果”。

### 实施反馈（2026-03-27）

- 已完成第一轮低风险实现，范围只覆盖
  `setup_new_commands()` 中“目标对象没有 `init()`”时的 miss 快路径：
  - `setup_new_commands()` 的遍历顺序没变
  - `remove_sent()` 行为没变
  - 只是在确定目标对象没有 `init()` 时，不再进入完整 apply 调用栈
- 当前保留实现：
  - `program_t` 新增 `init_lookup_state`
  - 运行时首次判断一个程序是否定义 `init()` 后，直接把结果缓存到
    `program_t`
  - `setup_new_commands()` 对无 `init()` 目标只保留 driver 侧 miss 预处理：
    - `time_of_ref`
    - lazy reset 检查
    - `O_RESET_STATE`
  - 对有 `init()` 的对象，仍走原来的 `apply(APPLY_INIT, ...)`
  - shadow 链判断仍按 `apply_low()` 的查找方向保真
- 中途否掉的方案：
  - 第一版：每次现查 `apply_cache_lookup(APPLY_INIT, prog)`
    - mixed 负载略有收益
    - 但 `enabled item -> 全无 init 房间` 反而退化
    - 已放弃
  - 独立 bench 程序：
    - 在当前 driver 生命周期下不如 `lpc_tests` 内 disabled perf case 稳定
    - 已撤回
- 当前基准入口：
  - `build_codex_review_fix/src/tests/lpc_tests.exe --gtest_filter=DriverTest.DISABLED_PerfMoveObjectEnabledItemRoomNoInit256 --gtest_also_run_disabled_tests`
  - `build_codex_review_fix/src/tests/lpc_tests.exe --gtest_filter=DriverTest.DISABLED_PerfMoveObjectEnabledItemRoomMixed256InitEvery8 --gtest_also_run_disabled_tests`
  - `build_codex_review_fix/src/tests/lpc_tests.exe --gtest_filter=DriverTest.DISABLED_PerfMoveObjectDisabledNoInitItemRoomEnabled64 --gtest_also_run_disabled_tests`
- 当前确认的 benchmark 结果：
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
- 兼容性判断更新为：
  - 当前实现没有改：
    - `move_object()` 的链接/脱链顺序
    - `setup_new_commands()` 的 `init()` 调用顺序
    - `remove_sent()` 的可见行为
  - 当前实现只改了：
    - “根本没有 `init()` 的目标对象还要不要走完整 apply 路径”
  - 所以它属于：
    - 低风险、但收益有限的第一阶段优化
- 下一步建议：
  - 不要继续在 `setup_new_commands()` 的 miss 判断上细抠
  - 下一阶段应该切到：
    - `remove_sent()` / sentence owner 索引
    - 或 `parse_command/packages/parser`

### 实施反馈（2026-03-27 第二阶段：`remove_sent()` owner 清理）

- 已完成 `remove_sent()` 的首轮低风险实现，目标是：
  - 不改 `move_object()` / `commands()` / `user_parser()` 的外部可见顺序
  - 只把“按 owner 删除 sentence”的内部查找路径从
    `全 sentence 线性扫` 改成 `先找 owner 桶，再删该 owner 的 sentence`
- 当前保留实现：
  - `object_t` 新增 `sent_owners`
    - 这是“当前 user 上有哪些 owner 挂过 action”的侧链表头
  - `sentence_t` 新增：
    - `prev`
    - `owner_next`
    - `owner_head_next`
  - `command_giver->sent` 仍然保留为可见主链
  - `commands()`、`user_parser()`、`parse cache` 仍以主链顺序为准
  - `remove_sent(owner, user)` 现在会：
    - 先在 `user->sent_owners` 上找到 owner bucket
    - 再只遍历该 owner 的 sentence
    - 从主链摘除并释放
  - `remove_action()` 也同步改成：
    - 删除主链节点时，一并从 owner bucket 侧链里摘除
  - `destruct2()` 在批量释放 `ob->sent` 前会先清 `ob->sent_owners`
- 为什么选这版而不是更激进的结构：
  - 不额外引入 bucket heap allocation
  - 不改 `commands()` / `parse_command()` 现有依赖的主链模型
  - 不碰 `input_to/get_char` 的外部接口，只要求：
    - `alloc_sentence()` / `free_sentence()` 把新增字段清零
  - 这属于：
    - 结构有变化
    - 但语义边界相对可控
- 当前 benchmark 方法：
  - 继续使用 `lpc_tests.exe` 内 disabled perf case
  - 不再单独造 baseline worktree
  - 新增 test-only 的 `legacy linear remove_sent` 对照实现
    - 在同一份构建、同一套夹具数据上，直接对比：
      - 当前 driver 的 `remove_sent()`
      - 旧线性扫描路径
  - 同时复跑 `move_object()` 的 3 条 perf case，看局部收益是否传导到整条移动链
- 当前确认的 benchmark 结果：
  - `remove_sent_owners_256_actions_8`
    - 当前 `456 ns`
    - 旧线性实现 `3868 ns`
    - 结论：约快 `8.5x`
  - `remove_sent_owners_1024_actions_1`
    - 当前 `978 ns`
    - 旧线性实现 `1777 ns`
    - 结论：约快 `1.8x`
  - 传导到 `move_object()` 链后的复测结果：
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
- 已补回归：
  - `DriverTest.TestRemoveSentRemovesOnlyTargetOwnerCommands`
  - `DriverTest.TestRemoveSentHandlesInterleavedOwnerCommands`
  - `DriverTest.TestRemoveActionKeepsOwnerBucketsConsistentForLaterRemoveSent`
  - 以及此前已有的
    `DriverTest.TestUserParserHonorsRemoveSentAfterPreviousParse`
- 兼容性判断更新为：
  - 当前实现没有改：
    - `user->sent` 的可见顺序
    - `commands()` 输出顺序
    - `user_parser()` 的候选执行顺序
    - `move_object()` 的链接/脱链顺序
  - 当前实现改的是：
    - driver 内部如何更快地定位“某个 owner 挂在某个 user 身上的 sentence”
  - 主要风险点已经覆盖到：
    - interleaved owner sentence
    - `remove_action()` 先删一个，再 `remove_sent()` 删剩余
    - 先 parse 再 `remove_sent()` 的 parse cache 失效
  - 因此这条可以视为：
    - `move_object()/remove_sent()` 链上的第二阶段保留实现
- 下一步建议：
  - 这条线已经拿到比较扎实的收益
  - 下一阶段建议离开命令链，转去：
    - `parse_command`
    - `packages/parser`
  - 如果后面回头继续抠命令链，优先级也应该低于 parser 本身

---

## PERF-RT-03 普通命令分发仍按 sentence 链线性扫描

- 位置：
  - `src/packages/core/add_action.cc:359`
  - `src/packages/core/add_action.cc:549`
  - `src/comm.cc:1474`
- 当前问题：
  - 网络输入最终会进入 `process_input()`，再走 `safe_parse_command()`。
  - `user_parser()` 会遍历 `command_giver->sent` 整条链表，逐个比对 verb。
  - 当前 sentence 匹配没有按 verb 做索引，仍是典型线性扫描。
- 会造成什么影响：
  - 每条命令输入的固定成本会随 sentence 数量增长。
  - sentence 的来源包括：
    - 玩家自身 action
    - 房间 action
    - 身上物品 action
    - 周围对象 action
  - 环境越复杂，每条 `look/get/kill/talk` 之类的普通命令都会更慢。
- 怎么优化：
  - 第一阶段最合理：
    - 给 sentence 建 verb 索引
    - 把“精确 verb”“前缀 verb”“无空格 verb”分桶
  - 这样大多数输入可以先命中一个很小的候选集合，
    再保留当前执行顺序做细匹配。
  - 第二阶段再考虑把 `add_action/remove_action/remove_sent`
    都接到同一套索引结构上。
- 对当前逻辑会不会有影响：
  - `中高`
  - 风险点主要不是“能不能匹配”，而是“匹配优先级是否完全一致”。
  - 需要严格保留当前这些规则：
    - 新加 action 在链表头部
    - 精确匹配与 `V_NOSPACE/V_SHORT` 的现有优先级
    - 相同 verb 下的先后覆盖顺序
  - 只要桶内顺序仍按当前 sentence 链顺序保持，外部逻辑不应变化。

### 实施反馈（2026-03-27）

- 已完成第一轮低风险实现，范围只覆盖 `user_parser()` 候选集缩小：
  - `command_giver->sent` 仍是唯一真相源
  - 新增 lazy parse cache：
    - `exact verb -> ordered sentence list`
    - `fallback sentence list`
    - `V_SHORT/V_NOSPACE special sentence list`
  - 命中时先缩小候选集，再按原 sentence 链顺序执行
- 已接入的失效点：
  - `add_action()`
  - `remove_action()`
  - `remove_sent()`
  - `destruct2()`
- 已补回归：
  - `DriverTest.TestUserParserPreservesEmptyShortFallbackOrderAgainstExactVerb`
  - `DriverTest.TestUserParserPreservesExactBeforePrefixWhenChainRequiresIt`
  - `DriverTest.TestUserParserPreservesExplicitDefaultVerbOrderAgainstExactVerb`
  - `DriverTest.TestUserParserHonorsRemoveSentAfterPreviousParse`
- 专项 benchmark：
  - 入口：`src/tests/user_parser_bench.cc`
  - 夹具：`testsuite/clone/user_parser_probe.c`
  - 口径：只量 `parse_command()/user_parser()` 的 sentence 匹配与 apply 开销，
    不混入网络读缓冲和 `process_input()` 路径
- 当前对标结果：
  - `exact_miss_unique_4096`
    - 基线 `5825 ns`
    - 当前 `134 ns`
    - 结论：约快 `97.7%`，约 `43x`
  - `exact_tail_hit_unique_4096`
    - 基线 `4908 ns`
    - 当前 `261 ns`
    - 结论：约快 `94.7%`，约 `18.8x`
  - `short_fallback_plus_exact_tail_hit_4096`
    - 基线 `4997 ns`
    - 当前 `261 ns`
    - 结论：约快 `94.8%`，约 `19.1x`
- 当前判断：
  - 这条优化已经成立，且收益明显大于 `mapping` 那种常数项微调
  - 当前实现没有改 `init()`、`move_object()`、`commands()`、`query_verb()` 的外部语义
  - 下一步不建议继续深挖 `user_parser` 本身，而应转去：
    - `move_object() + setup_new_commands() + remove_sent()`
    - `parse_command` / `packages/parser`

---

## PERF-RT-04 `parse_command` 旧解析器仍然偏重

- 位置：
  - `src/packages/ops/parse.cc:520`
  - `src/packages/ops/parse.cc:562`
  - `src/packages/ops/parse.cc:575`
  - `src/packages/ops/parse.cc:581`
  - `src/packages/ops/parse.cc:587`
  - `src/packages/ops/parse.cc:593`
  - `src/packages/ops/parse.cc:1287`
  - `src/packages/ops/parse.cc:1360`
- 当前问题：
  - `parse()` 一开始就会：
    - `explode_string()` 命令
    - `explode_string()` pattern
    - 对对象参数做 `deep_inventory()`
  - 之后又会从 master/object 拉多份词表：
    - id
    - plural id
    - adjective
    - preposition
    - allword
  - 匹配阶段仍有多层嵌套：
    - `item_parse()`
    - `match_object()`
    - `find_string()`
    - `check_adjectiv()`
- 会造成什么影响：
  - 解析成本会同时受以下因素放大：
    - 命令词数
    - 候选对象数
    - 每个对象的 id / plural / adjective 数量
    - 解析 pattern 的复杂度
  - 如果 mudlib 热路径大量依赖 `parse_command` efun，
    这会成为明显 CPU 热点。
- 怎么优化：
  - 第一阶段只做低风险去重：
    - 预拆或缓存对象描述词
    - 减少重复 `explode_string()`
    - 减少多词匹配时的重复扫描
  - 第二阶段才考虑：
    - 对 parse 元数据做更稳定的缓存
    - 把“对象词表刷新”和“命令解析”分离
- 对当前逻辑会不会有影响：
  - `中`
  - 只做缓存和拆词去重，一般不该改变语义。
  - 但如果开始改变 parse 元数据刷新时机，风险会明显升高。

### 实施反馈（2026-03-27 第一阶段）

- 这轮保留的实现很克制，只动了 `find_string()` 这一层：
  - 新增每次 `parse()` 调用内的轻量 memo
  - 只缓存“成功命中的 `(needle,start_index)`”
  - 不缓存 miss
  - 不改 `load_lpc_info()`、`item_parse()`、`deep_inventory()`、
    master/object apply 的刷新时机
- 为什么只保留 hit-only memo：
  - 这条链的真实热路径是：
    - 大量对象共享同一个真正会命中的 noun / phrase
    - 同时每个对象还可能带一批只出现一次的噪声 id
  - 如果把 miss 也缓存，会把大量“一次性噪声词”塞进 cache，
    反而拖慢解析
  - 中途试过两版更激进的方案，都被 benchmark 否掉了：
    - `unordered_map` 版 `find_string` 全量缓存
      - 结论：哈希常数项太大，退化明显
    - `member_string()` 按数组建 `unordered_set`
      - 结论：每对象词表建 set 的构建成本大于省下的线性扫描
- 当前保留实现：
  - `src/packages/ops/parse.cc`
    - `find_string()` 命中项 memo
    - 新增 test-only 的 legacy mode 开关，用于同 binary 对标
  - `src/packages/ops/parse.h`
    - 暴露 test-only 开关声明
  - `src/tests/test_lpc.cc`
    - 新增 direct `parse()` 夹具
    - 新增 legacy/current 同 binary 对比 perf case
  - `testsuite/clone/parse_command_probe.c`
    - 可控 noun/plural/adjective 词表 probe
- benchmark 方法：
  - 不再维护独立 baseline worktree
  - 直接在同一份 `lpc_tests.exe` 里切：
    - 当前实现
    - legacy mode
  - 口径是：
    - `parse()` 本体
    - source 使用 room，对象通过 `deep_inventory()` 进入候选集
    - pattern 固定为 ` 'get' %i `
    - command 使用共享 plural + adjective，专门压 noun/adjective 匹配链
- 当前确认的 benchmark 结果：
  - `parse_command_room_shared_256_ids_8_adjs_4`
    - 当前 `101036 ns`
    - legacy `111780 ns`
    - 结论：约快 `9.6%`
  - `parse_command_room_shared_512_ids_4_adjs_8`
    - 当前 `200201 ns`
    - legacy `226069 ns`
    - 结论：约快 `11.4%`
- 已补回归：
  - `DriverTest.TestParseCommandMatchesLegacyPluralWithAdjectives`
  - `DriverTest.TestParseCommandMatchesLegacyPluralSelection`
- 兼容性判断更新为：
  - 当前实现没有改：
    - `%i` 的返回结构
    - plural / adjective 匹配语义
    - `deep_inventory()` 的候选对象范围
    - master/object 词表拉取时机
  - 当前实现只改：
    - 同一轮 `parse()` 内，已成功匹配过的 phrase 如何复用结果
- 当前判断：
  - 这条优化收益不如 `user_parser()` 或 `remove_sent()` 那么大，
    但已经是正收益，而且保持在低风险边界内
  - 下一步若继续深挖 `parse_command`，建议优先看：
    - pattern token cache
    - master 默认词表 cache
  - 不建议再回到：
    - 缓存 miss
    - 为每个对象词表建哈希集合

---

## PERF-RT-05 `packages/parser` 新解析器也不轻

- 位置：
  - `src/packages/parser/parser.cc:448`
  - `src/packages/parser/parser.cc:459`
  - `src/packages/parser/parser.cc:851`
  - `src/packages/parser/parser.cc:1229`
- 当前问题：
  - `interrogate_master()` 会从 master 拉用户表、literals、special words。
  - `interrogate_object()` 会对单个对象打一串 apply：
    - `noun`
    - `plural`
    - `adjective`
    - `is_living`
    - `inventory_accessible`
    - `inventory_visible`
  - `load_objects()` 还会先找需要 refresh 的对象，再批量 interrogate，
    再重建本轮解析对象集合与 hash 表。
- 会造成什么影响：
  - 如果 `packages/parser` 被用作主命令解析链，
    解析性能会高度依赖：
    - 当前可见对象数
    - 当前用户数
    - 对象 parse 元数据刷新频率
  - 这种成本通常不是单次很夸张，而是“每次解析都叠一层对象/词表准备成本”。
- 怎么优化：
  - 最合理的方向不是简单换容器，而是：
    - 增量刷新 parse 元数据
    - 对未变化对象尽量不重复 interrogate
    - 缩小每次 `load_objects()` 的重建范围
  - 也可以引入世代号 / dirty 标记，只刷新真正变化的对象。
- 对当前逻辑会不会有影响：
  - `高`
  - 这里最怕的是缓存过期不及时，导致：
    - 名词识别错误
    - 可见性错误
    - 解析结果和当前房间状态不一致
  - 所以这条能优化，但必须先把刷新边界定义清楚。

### 实施反馈（2026-03-27 第一阶段）

- 已完成第一轮低风险实现，范围只覆盖：
  - `master users` 惰性加载
  - 当前 verb 不需要 `remote livings` 时，`load_objects()` 不再扫描、
    interrogate、拼接远端 users
- 修改入口：
  - `src/packages/parser/parser.cc`
  - `src/packages/parser/parser.h`
  - `src/tests/test_lpc.cc`
  - `testsuite/single/master.c`
  - `testsuite/clone/parser_perf_handler.c`
  - `testsuite/clone/parser_perf_probe.c`
- 最终保留实现：
  - 把 `interrogate_master()` 拆成按需加载：
    - `MASTER_LOAD_LITERALS`
    - `MASTER_LOAD_SPECIALS`
    - `MASTER_LOAD_USERS`
  - `parse_add_rule()` 只拉 literals + specials，不再顺手调用
    `parse_command_users`
  - `load_objects()` 先根据当前 `parse_verb_entry` 的 handler 集，
    判断这次解析是否真的需要 remote users
  - 只有真的需要时，才：
    - 调 master `parse_command_users`
    - `init_users()`
    - 把远端 users 追加进 `loaded_objects`
- 已补语义回归：
  - `DriverTest.TestParserAddRuleDoesNotFetchMasterUsers`
  - `DriverTest.TestParserSkipsRemoteUsersWhenVerbDoesNotNeedThem`
  - `DriverTest.TestParserRemoteUsersRemainReachableWhenEnabled`
- benchmark 口径：
  - 入口：`src/tests/test_lpc.cc`
  - 通过 master 测试钩子伪造大规模 `parse_command_users()` 返回值
  - `no-remote` case 刻意只给一个本地命中对象，
    远端 users 只作为“本应被跳过的额外成本”
  - 这个数字代表“远端 users 跳过收益上限”，不是普通场景平均值
- benchmark 结果：
  - `parser_no_remote_users_override_2048`
    - 当前 `3036 ns`
    - legacy `2762194 ns`
    - 结论：极端 no-remote 场景收益非常大，
      因为旧实现会白白扫描 2048 个远端 users
  - `parser_remote_users_override_2048`
    - 当前 `2750310 ns`
    - legacy `2994003 ns`
    - 结论：remote 打开时没有行为回退，性能小幅变快
- 兼容性结论：
  - 这轮不改：
    - `parse_sentence()` 外部接口
    - noun/plural/adjective 匹配规则
    - remote livings 开启时的对象可见范围
  - 只改“什么时候去 master 拉 users，什么时候把远端 users 塞进本轮对象集”
  - 目前定向语义回归和全量 `ctest` 都已通过
- 备注：
  - disabled perf case 里对 bench 自建 clone 额外 pin 了一份 ref，
    只为避免 synthetic perf 夹具在 cleanup 阶段把 ref 抖动混进结果
  - 这部分只存在于测试代码，不影响 driver 运行时语义

---

## PERF-RT-06 gametick / `call_out` 调度器有明显结构成本

- 位置：
  - `src/backend.cc:116`
  - `src/backend.cc:120`
  - `src/backend.cc:168`
  - `src/backend.h:22`
  - `src/packages/core/call_out.cc:94`
  - `src/packages/core/call_out.cc:170`
- 当前问题：
  - gametick 事件队列当前是 `std::multimap<tick, TickEvent *>`。
  - 每个事件都要单独分配一个 `TickEvent`。
  - `TickEvent` 内部还包了一层 `std::function<void()>`。
  - `call_out` 这类高频调度会把插入、删除、分配、间接调用成本持续放大。
- 会造成什么影响：
  - 事件越多，调度器自身的容器和分配成本越明显。
  - 大量 `call_out(0)`、短周期 `call_out`、周期性后台任务，
    都会让这条路径变重。
  - 即使真正回调逻辑不重，调度器本身也会吃掉一部分 CPU。
- 怎么优化：
  - 更适合的方向：
    - timing wheel
    - bucket queue
    - 分层时间轮
  - 同时可以考虑：
    - `TickEvent` 池化
    - 避免热路径 `std::function`
    - 同 tick 事件用链表/数组批量管理
- 对当前逻辑会不会有影响：
  - `中`
  - 需要保留：
    - `call_out(0)` 的时序语义
    - 同一 tick 内事件执行顺序
    - gametick 与 walltime 的现有边界
  - 若保持这些不变，外部逻辑不该变化。

### 实施反馈（2026-03-27 第一阶段）

- 已完成第一轮低风险实现，范围只覆盖 `gametick` 队列本身：
  - `walltime event` 路径不变
  - `pending_call_t` / `remove_call_out()` / `find_call_out()` 语义不变
  - 只把 `backend.cc` 里的 `gametick` 容器从
    `multimap<tick, TickEvent *>` 改成 `map<tick, vector<TickEvent *>>`
- 最终保留实现：
  - 修改入口：
    - `src/backend.h`
    - `src/backend.cc`
    - `src/tests/gametick_bench.cc`
    - `src/tests/test_lpc.cc`
    - `src/tests/CMakeLists.txt`
  - 底层结构：
    - `due_tick -> ordered vector<TickEvent *>`
    - `add_gametick_event()` 先定位/创建 tick bucket，再顺序追加
    - `call_tick_events()` 每轮只取当前最早到期 bucket，
      `move` 出来后按原顺序执行
  - 关键语义：
    - 同 tick 内仍按注册顺序执行
    - 回调里新加的 `delay=0` 事件，仍排在“当前已抽出的批次”之后
    - `TickEvent::valid=false` 的失效语义仍保持
    - `add_walltime_event()` 仍走原 `libevent event_base_once` 路径
  - 顺手收掉了一处常数项：
    - `TickEvent` 对 `std::function` 改成 move 构造，避免多拷一份 callback
- 中途否掉的方案：
  - 第一版 intrusive list bucket
    - 结论：树节点确实少了，但执行阶段变成沿堆上链表追指针，
      `dispatch` 明显退化
    - 已放弃，最终改成 `vector<TickEvent *>` bucket
- 已补回归：
  - `DriverTest.TestGametickPreservesInsertionOrderWithinSameTick`
  - `DriverTest.TestGametickSchedulesNewSameTickEventsAfterCurrentBatch`
  - `DriverTest.TestGametickSkipsInvalidatedPendingSameTickEvent`
- benchmark 方法：
  - 新增 `src/tests/gametick_bench.cc`
  - 口径只量 scheduler 自身：
    - `schedule_ns`
    - `dispatch_ns`
  - 不混入 LPC 回调本体逻辑，专门观察
    `gametick/call_out` 底层队列的结构成本
  - 基线是同一工作树在改 `backend` 前跑一次 bench，
    当前值是改完后同一套 bench 的结果
- 当前确认的 benchmark 结果：
  - `same_tick_50000`
    - 基线：
      - `schedule_avg_ns=86`
      - `dispatch_avg_ns=28`
    - 当前：
      - `schedule_avg_ns=22`
      - `dispatch_avg_ns=10`
    - 结论：
      - schedule 约快 `74.4%`
      - dispatch 约快 `64.3%`
  - `shared_16_ticks_50000`
    - 基线：
      - `schedule_avg_ns=53`
      - `dispatch_avg_ns=27`
    - 当前：
      - `schedule_avg_ns=9`
      - `dispatch_avg_ns=10`
    - 结论：
      - schedule 约快 `83.0%`
      - dispatch 约快 `63.0%`
  - `shared_256_ticks_50000`
    - 基线：
      - `schedule_avg_ns=41`
      - `dispatch_avg_ns=28`
    - 当前：
      - `schedule_avg_ns=10`
      - `dispatch_avg_ns=9`
    - 结论：
      - schedule 约快 `75.6%`
      - dispatch 约快 `67.9%`
- 兼容性判断更新为：
  - 当前实现没有改：
    - `call_out(0)` 的可见时序
    - 同 tick 批次内的执行顺序
    - `valid=false` 取消待执行事件的语义
    - `walltime` 与 `gametick` 的边界
  - 当前实现只改：
    - `gametick` 事件在 driver 内部如何按 due tick 存放与抽取
- 当前判断：
  - 这条第一阶段实现已经成立
  - 它解决的是：
    - 同 tick / 少量共享 due tick 场景下，
      `multimap` 每事件一个树节点的固定成本
  - 它还没碰的仍然是：
    - `call_out` 对象索引结构
    - `pending_call_t` 分配与回收
    - 更激进的 timing wheel / 分层时间轮
  - 所以这可以作为：
    - `gametick/call_out` 的第一阶段保留实现
  - 如果后面继续深挖这条线，下一步优先级应转到：
    - `call_out` 的 object handle 索引垃圾积累
    - 或更激进的时间轮设计

### 实施反馈（2026-03-27 第二阶段：`call_out` owner 索引去垃圾化）

- 已完成第二轮低风险实现，范围只覆盖 `call_out` 的 owner 索引结构：
  - 不改 `call_out` / `find_call_out` / `remove_call_out` 的 efun 形态
  - 不改 `walltime` / `gametick` 的时间语义
  - 只把“object -> handle multimap 且允许垃圾累积”的结构，
    改成“object -> 活跃 callout intrusive bucket”
- 最终保留实现：
  - 修改入口：
    - `src/packages/core/call_out.cc`
    - `src/packages/core/call_out.h`
    - `src/tests/test_lpc.cc`
    - `src/tests/call_out_bench.cc`
    - `src/tests/CMakeLists.txt`
    - `testsuite/clone/call_out_probe.c`
  - 底层结构：
    - `handle -> pending_call_t *` 仍保留
    - `owner object -> pending_call_t *head` 改成 intrusive 双向链
    - `pending_call_t` 新增：
      - `owner_bucket_key`
      - `owner_prev`
      - `owner_next`
  - 关键行为：
    - 新建 callout 时，立即挂进 owner bucket
    - callout 被执行、`remove_call_out(handle)`、`remove_call_out(name)`、
      `remove_all_call_out()`、`clear_call_outs()`、`reclaim_call_outs()`
      时，都会立刻从 owner bucket 摘掉
    - `find/remove/remove_all` 现在只扫当前 owner 的活跃 callout，
      不再一边扫一边清陈旧 handle
- 兼容性明确保留：
  - `remove_call_out(handle)` 现有“不校验 owner 归属”的行为没有改
  - 这轮只改索引结构，不顺手改权限/安全语义
- 为什么这条值得做：
  - 旧实现的 `g_callout_object_handle_map` 会长期累积垃圾条目，
    直到 `reclaim_call_outs()` 才清
  - 这会导致：
    - `find_call_out(name)`
    - `remove_call_out(name)`
    - `remove_all_call_out(obj)`
    在热点对象上越来越慢
  - 也会让 `print_call_out_usage()` 里的 object map 垃圾计数持续变大
- 已补回归：
  - `DriverTest.TestCallOutByHandleRemovalDoesNotLeaveOwnerIndexGarbage`
  - `DriverTest.TestExecutedCallOutDoesNotLeaveOwnerIndexGarbage`
  - `DriverTest.TestRemoveAllCallOutClearsOwnerIndexBucket`
- benchmark 方法：
  - 新增 `src/tests/call_out_bench.cc`
  - 专门构造：
    - 同一对象上批量创建大量 future callout
    - 再用 `remove_call_out(handle)` 全部删掉
    - 最后只保留 1 个 live callout，测一次 `find_call_out(name)`
  - 这样可以直接放大“旧 owner 索引里的垃圾条目”对 object-scoped
    查找路径的拖累
  - 输出中的：
    - `owner_index_entries_delta=1`
    - `owner_index_buckets_delta=1`
    代表 bench 结束时只剩最后那个 live callout
- 当前确认的 benchmark 结果：
  - `find_after_handle_removed_same_object_10000`
    - 基线：
      - `avg_ns=2865162`
      - `owner_index_entries=400040`
      - `owner_index_buckets=712697`
    - 当前：
      - `avg_ns=2146905`
      - `owner_index_entries_delta=1`
      - `owner_index_buckets_delta=1`
    - 结论：
      - `find_call_out(name)` 约快 `25.1%`
      - owner 索引垃圾从“几十万级累积”收敛到只剩目标 live callout
  - `find_after_handle_removed_same_object_50000`
    - 基线：
      - `avg_ns=15385510`
      - `owner_index_entries=900050`
      - `owner_index_buckets=1447153`
    - 当前：
      - `avg_ns=10504150`
      - `owner_index_entries_delta=1`
      - `owner_index_buckets_delta=1`
    - 结论：
      - `find_call_out(name)` 约快 `31.7%`
      - owner 索引垃圾同样被收敛掉
- 兼容性判断更新为：
  - 当前实现没有改：
    - `call_out(0)` / `call_out_walltime()` 的时间语义
    - `find_call_out(handle)` 的 owner 校验语义
    - `remove_call_out(handle)` 现有的宽松行为
    - `this_player in call_out` 的上下文恢复
  - 当前实现只改：
    - driver 内部怎样维护 owner 侧的 callout 索引
- 当前判断：
  - 这条可以作为 `call_out` 的第二阶段保留实现
  - 到这里为止，`gametick/call_out` 已经有两个可保留阶段：
    - D1 `gametick` due tick 分桶
    - D2 `call_out` owner 索引去垃圾化
  - 如果后面继续深挖这条线，下一步优先级才该转去：
    - `pending_call_t` 分配模型
    - 更激进的 timing wheel / 分层时间轮

### 实施反馈（2026-03-27 第三阶段：`pending_call_t` 复用池）

- 已完成第三轮低风险实现，范围只覆盖 `call_out` 节点本体的分配/回收：
  - 不改 `call_out` / `call_out_walltime` 的时间语义
  - 不改 owner 索引、handle 索引和回调执行时机
  - 只把 `pending_call_t` 的 `DCALLOC/FREE` 抖动，改成“有上限的 struct 复用池”
- 最终保留实现：
  - 修改入口：
    - `src/packages/core/call_out.cc`
    - `src/packages/core/call_out.h`
    - `src/tests/test_lpc.cc`
    - `src/tests/call_out_bench.cc`
  - 底层结构：
    - 新增全局 `pending_call_t` free list
    - 池子上限固定为 `16384` 个 entry
    - `new_call_out()` 优先从池里取空节点，取不到才 `DCALLOC`
    - `free_called_call()` 在释放：
      - function/funp
      - object ref
      - `vs`
      - `command_giver`
      - `tick_event`
      这些外部资源之后，再回收 `pending_call_t` 本体
  - 兼容性边界：
    - `tick_event->valid=false` 语义保留
    - 旧 `TickEvent` 即使还在队列里，也只会因为 `valid=false` 被丢弃，
      不会回调到已经复用的 `pending_call_t`
    - `clear_call_outs()` 仍是“退出/测试隔离”清理点，这轮额外把池子一起清空
    - `print_call_out_usage()` / `total_callout_size()` 已把池子占用纳入统计
- 为什么只做“有上限的池”：
  - 不做无界缓存，避免把一次性峰值 churn 直接变成长期常驻内存
  - `16384` 上限可以覆盖常见批量创建/撤销场景，同时把额外常驻内存
    控制在 `16384 * sizeof(pending_call_t)` 这个量级
- 已补回归：
  - `DriverTest.TestCallOutPoolReusesFreedEntriesWhenEnabled`
  - `DriverTest.TestCallOutPoolCanBeDisabledForLegacyBenchComparison`
- benchmark 方法：
  - 继续使用 `src/tests/call_out_bench.cc`
  - 新增同一份 binary 内的 `pool on/off` 对照：
    - `current`：启用复用池
    - `legacy`：关闭池，回到直接 `DCALLOC/FREE`
  - 口径只量：
    - `new_call_out()`
    - `remove_call_out_by_handle()`
    这条批量创建/撤销路径
- 当前确认的 benchmark 结果：
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
    - 原因：池子上限是 `16384`，超大峰值 churn 只做部分复用，不追求用更多常驻内存换满额吞吐
- 兼容性判断更新为：
  - 当前实现没有改：
    - `call_out(0)` / `call_out_walltime()` 的时间语义
    - owner 索引和 handle 索引行为
    - `find/remove/remove_all` 的外部返回值和可见顺序
    - `this_player in call_out` 的上下文恢复
  - 当前实现只改：
    - `pending_call_t` 本体在 driver 内部怎样分配和复用
- 当前判断：
  - 这条可以作为 `call_out` 的第三阶段保留实现
  - 它适合解决：
    - 同一对象或同一批任务反复 `new/remove` callout 的 allocator 抖动
  - 它不试图解决：
    - 极端大峰值 churn 的满额吞吐
    - `gametick` / `call_out` 的更激进时间轮设计
  - 所以这条之后，`call_out` 主线的下一步优先级才该转去：
    - timing wheel / 分层时间轮
    - 或别的未触达 runtime 热点

---

## PERF-RT-07 网络输入缓冲存在重复扫描，MUD 包输入不支持批量提取

- 位置：
  - `src/comm.cc:97`
  - `src/comm.cc:111`
  - `src/comm.cc:1161`
  - `src/comm.cc:1167`
  - `src/comm.cc:1294`
  - `src/comm.cc:1326`
  - `src/comm.cc:1349`
  - `src/comm.cc:1474`
- 当前问题：
  - 普通文本输入路径里：
    - `on_user_input()` 逐字节写入缓冲
    - `cmd_in_buf()` 再扫一遍找换行
    - `first_cmd_in_buf()` 再扫一遍提取第一条命令
  - MUD 包路径里：
    - 只按“当前缓冲刚好组成一个完整包”处理
    - 如果一次 `recv` 里带了多个完整包，当前实现不会批量提取
- 会造成什么影响：
  - 文本输入路径会重复扫描相同数据。
  - MUD 包路径会浪费一次性收到多包时的吞吐潜力。
  - 高吞吐连接或网关输入场景下，会让驱动多吃无谓 CPU。
- 怎么优化：
  - 文本输入：
    - 改成 ring buffer 或维护换行位置索引
    - 避免每次都从头扫描
  - MUD 包：
    - 支持一次性循环提取多个完整包
    - 保留半包缓存语义
- 对当前逻辑会不会有影响：
  - `低到中`
  - 只要保留以下语义，逻辑影响很小：
    - `SINGLE_CHAR`
    - CRLF 兼容
    - `SKIP_COMMAND`
    - `HAS_PROCESS_INPUT`
    - 每条命令仍按当前顺序进入 `process_input()`

### 实施反馈（2026-03-27 第一阶段：文本输入缓冲命令终止符缓存）

- 已完成第一轮低风险实现，但范围只覆盖“文本命令缓冲的重复扫描”：
  - 不改 `MUD` 包一次读取多个完整包时的处理策略
  - 不改 `ASCII` 直读路径 `process_ascii_chunk_internal()` 的命令调度方式
  - 只把 websocket/telnet/gateway 这条
    `cmd_in_buf() -> first_cmd_in_buf()` 的双扫描链压成“首扫缓存 + 提取复用”
- 当前保留实现：
  - 修改入口：
    - `src/interactive.h`
    - `src/user.h`
    - `src/user.cc`
    - `src/comm.h`
    - `src/comm.cc`
    - `src/packages/gateway/gateway_session.cc`
    - `src/tests/test_lpc.cc`
    - `src/tests/input_buffer_bench.cc`
    - `src/tests/CMakeLists.txt`
  - 底层结构：
    - `interactive_t` 新增 `text_command_end`
    - `cmd_in_buf()` 首次扫描时缓存第一个 `\r/\n` 的位置
    - `first_cmd_in_buf()` 直接复用缓存，不再重复扫同一段命令
    - `interactive_compact_text()` 在缓冲搬移时同步平移缓存位置
    - 所有会直接改写文本缓冲的路径统一失效缓存：
      - `on_user_input()`
      - `gateway_session` 输入注入
      - `clean_buf()` 命令丢弃/前移
      - `interactive_reset_text()`
  - 兼容性边界：
    - 保留 `SINGLE_CHAR`
    - 保留 `CRLF / LFCR / CR / LF`
    - 保留 `SKIP_COMMAND`
    - 保留 `process_user_command()` 在 `exec/destruct` 后的现有停机/续调度语义
    - `MUD` 包多包批提取这轮刻意不动，避免和 `process_input()` 重入顺序绑死
- 已补回归：
  - `DriverTest.TestCmdInBufCachesBufferedCommandEndAcrossExtraction`
  - `DriverTest.TestOnUserInputBackspaceInvalidatesCachedCommandEnd`
  - 以及原有 9 条输入链回归联跑
- 基准方法：
  - 新增 `src/tests/input_buffer_bench.cc`
  - 在同一份 binary 内同时运行：
    - 当前实现：缓存终止符位置
    - legacy 对照：旧的双扫描实现
  - 口径只量：
    - `cmd_in_buf()`
    - `first_cmd_in_buf()`
    这一段的 driver 侧扫描成本，不混 LPC `process_input()`
- 当前确认的 benchmark 结果：
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
- 验证结果：
  - 输入链定向回归：`11/11` 通过
  - 全量 `ctest`：活动用例 `84/84` 通过
- 当前判断：
  - 这条优化可以作为网络输入路径的第一阶段保留实现
  - 它已经把“文本命令同一段数据被重复扫描”这部分成本拿掉
  - 但它还没有覆盖：
    - `MUD` 包一次读取多包时的批量提取
    - `process_input()` 之间的跨包早停/重入顺序
  - 所以下一阶段若继续深挖这条线，优先项只剩：
    - `MUD` 多包批提取
    - 并把 `exec/destruct/interactive 转移` 语义边界单独补齐

### 实施反馈（2026-03-27 第二阶段：MUD 多包批提取）

- 已完成第二轮低风险实现，但范围只覆盖 `PORT_TYPE_MUD`：
  - 不改普通文本输入路径
  - 不改 MUD 包序列化格式
  - 只把“一次 read 里最多处理 1 个完整包”的旧路径，改成
    “缓冲里有几个完整包就顺序处理几个”
- 最终保留实现：
  - 修改入口：
    - `src/comm.cc`
    - `src/comm.h`
    - `src/tests/test_lpc.cc`
    - `src/tests/mud_input_bench.cc`
    - `src/tests/CMakeLists.txt`
    - `testsuite/clone/mud_input_probe.c`
  - 底层结构：
    - `get_user_data()` 对 `PORT_TYPE_MUD` 走专用 `get_user_data_mud()`
    - 每次 read 先把数据 append 到 `interactive->text`
    - 然后循环：
      - 解包头
      - 判断缓冲里是否已有完整包
      - `restore_svalue()`
      - `safe_apply(APPLY_PROCESS_INPUT, ...)`
      - 消费当前包并继续看下一个
    - 新增 test helper：
      - `process_mud_chunk_for_test()`
  - 关键语义：
    - 半包仍然继续缓存在 `interactive->text`
    - 一次读到多个完整包时，按原顺序逐个进入 `process_input`
    - `process_input()` 里如果 `exec` 转移了 interactive，后续包会继续交给新的 owner
    - `process_input()` 里如果对象析构或 interactive 脱离，后续包立即停止
    - 包格式仍然是：
      - 4 字节长度前缀
      - 后续 `save_svalue` 负载
- 已补回归：
  - `DriverTest.TestMudPacketCompletionRequiresFullPayloadBytes`
  - `DriverTest.TestGetUserDataMudProcessesMultiplePacketsFromSingleBufferedRead`
  - `DriverTest.TestGetUserDataMudPreservesPartialSecondPacketAcrossReads`
  - `DriverTest.TestGetUserDataMudContinuesAfterExecTransfersInteractive`
  - `DriverTest.TestGetUserDataMudStopsAfterProcessInputDestructsObject`
- benchmark 方法：
  - 新增 `src/tests/mud_input_bench.cc`
  - 同一份 binary 内对比：
    - `current`：单次 chunk 内尽量把完整包都处理完
    - `legacy`：每次回调只处理 1 个完整包
  - 口径输出：
    - `current_avg_ns / legacy_avg_ns`
    - `current_rounds_avg / legacy_rounds_avg`
  - 这里的 `rounds` 指：
    - 在相同输入流上，需要多少轮“驱动读入并尝试处理”的回合
- 当前确认的 benchmark 结果：
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
- 兼容性判断更新为：
  - 当前实现没有改：
    - MUD 包格式
    - 半包缓存语义
    - `exec/destruct` 后的停机/续处理边界
  - 当前实现改的是：
    - 一次 read 已经拿到多个完整包时，是否还要分很多轮再喂进 `process_input`
- 当前判断：
  - 这条优化可以作为网络输入路径的第二阶段保留实现
  - 它的主要收益不在“单线程纯 CPU microbench 明显变快”
  - 它真正解决的是：
    - burst 输入下的驱动回合数
    - 多包输入的吞吐/延迟结构成本
  - 所以这轮应被归类为：
    - 吞吐/批处理路径清理
    - 而不是强 `CPU` 数字优化

---

## PERF-RT-08 VM 主解释器仍是传统大 `switch`

- 位置：
  - `src/vm/internal/base/interpret.cc:2049`
- 当前问题：
  - 主执行循环当前仍是典型 `switch (instruction)` 分发。
  - 这是最传统、最稳，但也是分发成本最高的实现之一。
- 会造成什么影响：
  - 每条 opcode 都要付一次分支分发成本。
  - 指令执行密度高时，这条成本会被持续累计。
  - 这类问题不会像 `move/init` 那样直接表现成“某个动作爆慢”，
    但会构成全局基线开销。
- 怎么优化：
  - 中长期才值得评估：
    - computed goto
    - direct threaded dispatch
    - opcode 布局优化
  - 这类优化通常需要配套 benchmark、编译器兼容性验证、
    调试工具链验证。
- 对当前逻辑会不会有影响：
  - `高`
  - 功能语义理论上不该变，但工程风险很高：
    - 不同编译器支持差异
    - 调试体验变差
    - 崩溃定位和可维护性变复杂
  - 这是“长期收益大，但不该现在先动”的典型项。

---

## 5. 暂不建议优先动的点

## PERF-RT-N01 JSON 不是当前主矛盾

- 位置：
  - `src/packages/json_extension/pkg_json.cc:307`
  - `src/packages/json_extension/pkg_json.cc:326`
- 现状：
  - 当前 runtime JSON 已经直接用 `yyjson`。
- 结论：
  - 除非后续 benchmark 明确表明 JSON 调用量极端高，
    否则这轮不建议优先在这里花时间。

## PERF-RT-N02 apply cache 已经不是低垂果子

- 位置：
  - `src/vm/internal/apply.cc:217`
  - `src/vm/internal/base/program.h:259`
- 现状：
  - `apply_low()` 已经走 apply cache。
  - `program_t` 里的 `apply_lookup_table` 已经是 `unordered_map`。
- 结论：
  - 这里不是当前最值得先打的点。

## PERF-RT-N03 tracing 关闭时不是主要负担

- 位置：
  - `src/base/internal/tracing.h:175`
- 现状：
  - `ScopedTracer` 只有在 `Tracer::enabled()` 为真时，
    才会真正构造内部对象。
- 结论：
  - tracing 开启时当然会有成本，但关闭状态下不属于当前主瓶颈。

---

## 6. 推荐落地顺序

### 第一批：高收益，且仍有机会保持逻辑完全不变

1. `heartbeat` 调度结构改造
2. `user_parser()` 的 sentence verb 索引
3. 网络输入重复扫描优化

这三项的共同点是：

- 有明显结构性浪费
- 优化目标清晰
- 在不改变外部语义的前提下，有机会拿到比较直接的收益

### 第二批：收益很可能更大，但兼容性风险更高

1. `move_object() + setup_new_commands() + remove_sent()`
2. `parse_command`
3. `packages/parser`
4. gametick / `call_out`

这几项的特点是：

- 一旦命中真实热点，收益可能非常明显
- 但都和历史行为、调用顺序、缓存时机强相关
- 必须先补专门 benchmark 和回归测试

### 第三批：长期项

1. VM computed goto / threaded dispatch

这项不适合作为当前第一波优化目标。

---

## 7. 如果后续要真正动手，建议怎么做

### 第一步：先补 benchmark / 埋点

建议先做 4 组定向 benchmark：

1. `heartbeat`
   - 1k / 10k / 50k heartbeat 对象
   - interval 混合分布
2. `move/init`
   - 房间内 10 / 100 / 500 个对象
   - 玩家携带物数量递增
3. `user_parser`
   - sentence 数量递增
   - verb 冲突、前缀 verb、无空格 verb 分开测
4. `call_out`
   - 大量短期 call_out
   - 大量同 tick 事件

### 第二步：按“先不改语义，只改数据结构”原则推进

- `heartbeat`：
  - 先改到底层调度结构
  - 不改 efun 语义
- `user_parser`：
  - 先加 verb 索引
  - 保留桶内原 sentence 顺序
- `network input`：
  - 先消除重复扫描
  - 不改 `process_input()` 时序

### 第三步：再碰高风险逻辑链

- `move/init`
- `parse_command`
- `packages/parser`

这里每动一项，都应该先把“当前行为基线”测出来，再做兼容回归。

---

## 8. 最终判断

当前驱动确实还有不少可提升空间，但真正值得打的，不是“再找几个旧库换掉”，
而是把几条最重的运行时主链重新做一遍数据结构和调度模型。

如果只按源码判断，不看 mudlib，不做 profiling，
当前最值得优先推进的就是：

1. `heartbeat`
2. `user_parser`
3. `move/init`

其中：

- `heartbeat` 最适合先动，收益预期高，兼容性风险相对可控
- `user_parser` 次之，结构问题也很明显
- `move/init` 风险最大，但一旦命中真实热点，收益也可能最大
