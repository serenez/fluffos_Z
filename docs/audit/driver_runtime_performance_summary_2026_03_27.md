# FluffOS 驱动运行时性能改动总览（2026-03-27）

## 1. 本次总计改动

- 总文件数：`43`
- 核心运行时代码：`25`
- 新增/更新测试与基准：`15`
- 文档：`3`
- 全量验证结果：`ctest` 活动用例 `90/90` 通过

## 2. 变动文件

### 核心运行时代码

- `src/backend.cc`
- `src/backend.h`
- `src/comm.cc`
- `src/comm.h`
- `src/compiler/internal/compiler.cc`
- `src/interactive.h`
- `src/packages/core/add_action.cc`
- `src/packages/core/add_action.h`
- `src/packages/core/call_out.cc`
- `src/packages/core/call_out.h`
- `src/packages/core/heartbeat.cc`
- `src/packages/core/heartbeat.h`
- `src/packages/gateway/gateway_session.cc`
- `src/packages/ops/parse.cc`
- `src/packages/ops/parse.h`
- `src/packages/parser/parser.cc`
- `src/packages/parser/parser.h`
- `src/user.cc`
- `src/user.h`
- `src/vm/internal/base/object.h`
- `src/vm/internal/base/program.h`
- `src/vm/internal/simulate.cc`
- `src/tests/CMakeLists.txt`
- `src/tests/test_lpc.cc`
- `testsuite/single/master.c`

### 新增/更新测试与基准

- `src/tests/call_out_bench.cc`
- `src/tests/gametick_bench.cc`
- `src/tests/heartbeat_bench.cc`
- `src/tests/input_buffer_bench.cc`
- `src/tests/mud_input_bench.cc`
- `src/tests/user_parser_bench.cc`
- `testsuite/clone/call_out_probe.c`
- `testsuite/clone/heartbeat_probe.c`
- `testsuite/clone/move_chain_init.c`
- `testsuite/clone/move_chain_noinit.c`
- `testsuite/clone/mud_input_probe.c`
- `testsuite/clone/parse_command_probe.c`
- `testsuite/clone/parser_perf_handler.c`
- `testsuite/clone/parser_perf_probe.c`
- `testsuite/clone/user_parser_probe.c`

### 文档

- `docs/audit/driver_runtime_performance_audit_2026_03_26.md`
- `docs/audit/driver_runtime_performance_execution_2026_03_26.md`
- `docs/audit/driver_runtime_performance_summary_2026_03_27.md`

## 3. 逐项结果

### 1. heartbeat 混合调度

- 改动函数/路径：
  - `set_heart_beat()`
  - `call_heart_beat()`
  - `query_heart_beat()`
- 影响到的游戏操作：
  - NPC 心跳
  - 玩家心跳
  - 战斗轮询
  - 持续回血/扣血
  - 周期性脚本
- 提升/降低：
  - `uniform_interval_1`：慢约 `5%~15%`
  - `uniform_interval_10`：快约 `82%`
  - `uniform_interval_100`：快约 `97.5%`
  - `mixed_interval_1_10`：快约 `31%~37%`
  - `mixed_interval_1_100`：快约 `39%`
- 结论：
  - 稀疏 heartbeat 和混合 heartbeat 明显变快
  - 纯 `interval=1` 高密度场景仍有回退

### 2. user_parser verb 索引

- 改动函数/路径：
  - `add_action()`
  - `remove_action()`
  - `remove_sent()`
  - `user_parser()`
- 影响到的游戏操作：
  - 所有通过 `add_action()` 注册的普通命令
  - 典型包括：
    - `look`
    - `get`
    - `drop`
    - `kill`
    - `talk`
    - 房间/物品/装备自定义指令
- 提升/降低：
  - `exact_miss_unique_4096`：约快 `43x`
  - `exact_tail_hit_unique_4096`：约快 `18.8x`
  - `short_fallback_plus_exact_tail_hit_4096`：约快 `19.1x`
- 结论：
  - 指令多、房间复杂、物品动作多时，普通命令分发显著变快

### 3. setup_new_commands 的 init miss 快路径

- 改动函数/路径：
  - `move_object()`
  - `setup_new_commands()`
  - `APPLY_INIT` 查找链
- 影响到的游戏操作：
  - 进房间
  - 离开房间
  - 跟随移动
  - 捡起物品
  - 放下物品
  - 容器进出
- 提升/降低：
  - `enabled_item_room_noinit_256`：`2561 ns -> 2286 ns`，快约 `10.7%`
  - `enabled_item_room_mixed_256_init_every_8`：`7869 ns -> 7538 ns`，快约 `4.2%`
  - `disabled_noinit_item_room_enabled_64`：`632 ns -> 435 ns`，快约 `31.2%`
- 结论：
  - 没有 `init()` 的目标对象越多，收益越明显

### 4. remove_sent owner 索引

- 改动函数/路径：
  - `remove_sent()`
  - `remove_action()`
  - `move_object()` 离开旧环境清理链
- 影响到的游戏操作：
  - 离开房间
  - 离开容器
  - 物品脱离玩家/环境
  - 房间切换时旧 action 清理
- 提升/降低：
  - `remove_sent_owners_256_actions_8`：约快 `8.5x`
  - `remove_sent_owners_1024_actions_1`：约快 `1.8x`
  - 传导到 `move_object()` 后的总结果：
    - `enabled_item_room_noinit_256`：`2561 ns -> 1221 ns`，快约 `52.3%`
    - `enabled_item_room_mixed_256_init_every_8`：`7869 ns -> 5374 ns`，快约 `31.7%`
    - `disabled_noinit_item_room_enabled_64`：`632 ns -> 233 ns`，快约 `63.1%`
- 结论：
  - 这条是 `move_object()` 主链上最扎实的一块收益

### 5. parse_command 命中项 memo

- 改动函数/路径：
  - `parse_command()`
  - `match_object()`
  - `find_string()`
- 影响到的游戏操作：
  - 使用旧 `parse_command` efun 的复杂文字命令
  - 尤其是带：
    - 复数名词
    - 形容词
    - 共享 id
    的物品解析
- 提升/降低：
  - `parse_command_room_shared_256_ids_8_adjs_4`：约快 `9.6%`
  - `parse_command_room_shared_512_ids_4_adjs_8`：约快 `11.4%`
- 结论：
  - 老解析器热路径有中等幅度收益

### 6. packages/parser remote users 惰性加载

- 改动函数/路径：
  - `parse_add_rule()`
  - `load_objects()`
  - `parse_command_users()`
- 影响到的游戏操作：
  - 使用 parser package 的自然语言指令
  - 尤其是可能涉及远端玩家匹配的 verb
- 提升/降低：
  - `parser_no_remote_users_override_2048`：`2762194 ns -> 3036 ns`
  - `parser_remote_users_override_2048`：`2994003 ns -> 2750310 ns`，快约 `8.1%`
- 结论：
  - 当前 verb 根本不需要 remote users 时，收益极大
  - remote 打开时没有回退

### 7. gametick 按 due tick 分桶

- 改动函数/路径：
  - `add_gametick_event()`
  - `call_tick_events()`
- 影响到的游戏操作：
  - 所有 gametick 驱动的延迟任务
  - 短延迟 `call_out`
  - 同 tick 批量事件
- 提升/降低：
  - `same_tick_50000`
    - `schedule_avg_ns`：`86 -> 22`，快约 `74.4%`
    - `dispatch_avg_ns`：`28 -> 10`，快约 `64.3%`
  - `shared_16_ticks_50000`
    - `schedule_avg_ns`：`53 -> 9`，快约 `83.0%`
    - `dispatch_avg_ns`：`27 -> 10`，快约 `63.0%`
  - `shared_256_ticks_50000`
    - `schedule_avg_ns`：`41 -> 10`，快约 `75.6%`
    - `dispatch_avg_ns`：`28 -> 9`，快约 `67.9%`
- 结论：
  - 同 tick 或少量共享 tick 的调度结构明显变轻

### 8. call_out owner 索引去垃圾化

- 改动函数/路径：
  - `find_call_out()`
  - `remove_call_out()`
  - `remove_all_call_out()`
  - owner 索引维护链
- 影响到的游戏操作：
  - 技能冷却查询
  - 定时任务取消
  - 对象批量清除 callout
  - 热点对象上的 `call_out` 名称查找
- 提升/降低：
  - `find_after_handle_removed_same_object_10000`：约快 `25.1%`
  - `find_after_handle_removed_same_object_50000`：约快 `31.7%`
- 结论：
  - 旧 owner 索引垃圾条目问题被收掉了

### 9. 文本输入缓冲命令终止符缓存

- 改动函数/路径：
  - `cmd_in_buf()`
  - `first_cmd_in_buf()`
  - `on_user_input()`
  - `gateway_session` 文本注入
- 影响到的游戏操作：
  - telnet 普通命令输入
  - websocket 普通命令输入
  - gateway 文本命令输入
- 提升/降低：
  - `single_line_256`：`103 ns -> 38 ns`，快约 `63.1%`
  - `single_line_4096`：`1267 ns -> 60 ns`，快约 `95.3%`
  - `two_lines_4096_4096`：`1425 ns -> 91 ns`，快约 `93.6%`
- 结论：
  - 普通文本命令的 driver 侧重复扫描基本被拿掉

### 10. MUD 多包批提取

- 改动函数/路径：
  - `get_user_data()`
  - `get_user_data_mud()`
  - `process_mud_chunk_for_test()`
- 影响到的游戏操作：
  - gateway/MUD 二进制包输入
  - 一次网络读入多个完整包的 burst 输入
- 提升/降低：
  - `mud_packets_32_payload_16`
    - `current_avg_ns=12371`
    - `legacy_avg_ns=12273`
    - `current_rounds_avg=1`
    - `legacy_rounds_avg=32`
  - `mud_packets_128_payload_16`
    - `current_avg_ns=49935`
    - `legacy_avg_ns=49629`
    - `current_rounds_avg=1`
    - `legacy_rounds_avg=128`
  - `mud_packets_128_payload_128`
    - `current_avg_ns=71279`
    - `legacy_avg_ns=70540`
    - `current_rounds_avg=1`
    - `legacy_rounds_avg=128`
- 结论：
  - 这条主要是**回合数下降**
  - 纯 CPU microbench 基本持平，个别 case 略慢

### 11. call_out 的 pending_call_t 复用池

- 改动函数/路径：
  - `new_call_out()`
  - `free_call()`
  - `free_called_call()`
  - `print_call_out_usage()`
  - `total_callout_size()`
- 影响到的游戏操作：
  - 高频创建/取消 callout
  - 技能冷却队列
  - 延迟技能撤销
  - 短期任务批量挂起/移除
- 提升/降低：
  - `schedule_remove_same_object_10000`
    - `2041520 ns -> 1512645 ns`
    - 快约 `25.9%`
  - `schedule_remove_same_object_50000`
    - `11722820 ns -> 11775890 ns`
    - 慢约 `0.5%`
- 结论：
  - 常见 churn 有明确收益
  - 超大峰值 churn 基本持平
  - 当前池上限是 `16384`，这是为了限制常驻内存

## 4. 已确认的下降项

- `heartbeat`
  - 纯 `uniform_interval_1` 场景慢约 `5%~15%`
- `MUD` 多包批提取
  - 纯 CPU microbench 没有明显提升，个别 case 略慢
- `call_out` 复用池
  - `50000` 级峰值 churn 慢约 `0.5%`

## 5. 对游戏的直接影响

- 玩家/NPC heartbeat：混合和稀疏分布更快，纯 `1 tick` 心跳仍要谨慎看实际服配置
- 普通命令输入：`add_action` 相关命令、房间/物品命令分发更快
- 移动链：进房间、离房间、跟随、拿取/放下物品更快
- 老 `parse_command`：复杂名词/形容词解析更快
- parser package：不需要 remote users 的自然语言指令更快
- 定时器/延迟任务：`gametick`、`call_out` 调度和查找更轻
- 文本输入：telnet/websocket/gateway 普通命令扫描更轻
- MUD 输入：burst 多包时回合数显著下降

## 6. 当前最终口径

- 收益最扎实的几块：
  - `user_parser`
  - `move_object/remove_sent`
  - `gametick`
  - 文本输入缓存
  - `call_out` 索引与复用池
- 需要带着负收益一起看的几块：
  - `heartbeat` 的纯 `interval=1`
  - `MUD` 多包路径的纯 CPU 数字
  - `call_out` 复用池在超大峰值 churn 下的上限取舍
