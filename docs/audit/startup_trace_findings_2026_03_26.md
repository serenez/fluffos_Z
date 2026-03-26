# 启动链路 Trace 结果分析（2026-03-26）

## 样本说明

本次分析使用的样本不是纯静态推断，而是实际运行：

```powershell
cd D:\xiangmu\fluffos_Z\24xinduobao
D:\xiangmu\fluffos_Z\build_codex_review_fix\src\lpcc.exe `
  --tracing D:\xiangmu\fluffos_Z\build_codex_review_fix\trace_lpcc_skill.json `
  D:\xiangmu\fluffos_Z\24xinduobao\config.ini `
  /kungfu/skill/1moliu/baduanjin
```

这次样本里 `lpcc.exe` 依然会完整执行：

- `init_main()`
- `vm_start()`
- `master / simul_efun` 初始化
- `preload` 文件加载

所以它已经覆盖了启动期最关键的 `preload` 链路，能够用于分析：

- `jz_all`
- `jz_skill`
- `jz_npc`
- 以及它们在启动期继续触发的大量对象装载

样本 trace 文件：

- [trace_lpcc_skill.json](D:\xiangmu\fluffos_Z\build_codex_review_fix\trace_lpcc_skill.json)

## 结果边界

这份 trace 有两个必须先说明的边界：

1. `ScopedTracer` 默认只记录 `>= 100 us` 的事件。  
   所以非常短的小事件不会出现，尤其是大量很快的 `include open` 不会全部入样本。

2. trace 事件总数上限是 `1,000,000`。  
   这次样本最终打到了 `1000001` 条事件并停止继续收集，所以后半段长链路有“外层事件没来得及落盘”的情况。

结论含义：

- 用来找“真正的大头热点”是足够的
- 但不适合用来精确统计所有微小事件

## 结论一：启动最重的 preload，不是编译慢，而是 create 链慢

按 preload daemon 的 `LPC Load Object` 排序，当前样本最重的是：

1. `adm/daemons/jz_all`
   - `LPC Load Object`: `661.436 ms`
   - `load_object.call_create`: `660.639 ms`
   - `LPC Compile File`: `0.748 ms`
   - `compile.yyparse`: `0.681 ms`

2. `adm/daemons/jz_skill`
   - `LPC Load Object`: `508.087 ms`
   - `load_object.call_create`: `507.173 ms`
   - `LPC Compile File`: `0.853 ms`
   - `compile.yyparse`: `0.773 ms`

3. 其他 preload daemon 基本都已经掉到一个数量级以下
   - `securityd`: `12.213 ms`
   - `logind`: `9.573 ms`
   - `named`: `3.191 ms`
   - `familyd`: `2.685 ms`

这说明：

- 启动慢的主要矛盾，不在编译器前端
- 至少在这份 mudlib 里，`jz_all` 和 `jz_skill` 的源码编译成本都不到 `1 ms`
- 真正重的是它们 `create()` 里继续触发的扫描、字符串 `call_other`、对象装载和 LPC 逻辑

## 结论二：`jz_skill` 已经可以确定是“create 触发大规模技能树装载”

源码对应位置：

- [jz_skill.c:34](D:\xiangmu\fluffos_Z\24xinduobao\adm\daemons\jz_skill.c:34)
- [jz_skill.c:100](D:\xiangmu\fluffos_Z\24xinduobao\adm\daemons\jz_skill.c:100)

关键链路：

1. `create()`
2. `des_skills()`
3. `save_allskill()`
4. `deep_path_list("/kungfu/skill/")`
5. 对每个文件执行：
   - `filename->is_perform()`
   - `filename->query_id()`

trace 样本中，`jz_skill` 本身的关键数据是：

- `compile.yyparse`: `773 us`
- `LPC Compile File`: `853 us`
- `load_object.call_create`: `507173 us`

也就是说：

- `jz_skill.c` 编译本身几乎可以忽略
- 真正的启动成本几乎全部发生在 `create()` 里面

## 结论三：`jz_all` 比 `jz_skill` 还重，说明装备树扫描更狠

源码对应位置：

- [jz_all.c:76](D:\xiangmu\fluffos_Z\24xinduobao\adm\daemons\jz_all.c:76)
- [jz_all.c:147](D:\xiangmu\fluffos_Z\24xinduobao\adm\daemons\jz_all.c:147)

它在 `create()` 里会：

1. 扫多个装备目录
2. 对每个装备对象跑 `d->is_zb()`
3. 后面还会反复跑：
   - `zb->query("yanse")`
   - `zbs->query_type()`

样本结果里，`jz_all` 是全 preload 第一名：

- `LPC Load Object`: `661.436 ms`
- `load_object.call_create`: `660.639 ms`
- `LPC Compile File`: `0.748 ms`

这说明从驱动视角看：

- 装备树这条启动链比技能树更重
- 依旧不是编译慢，而是 `create()` 里的对象级查询慢

## 结论四：`jz_npc` 的“真正大头”已经转移到它触发的房间/NPC对象上

源码对应位置：

- [jz_npc.c:68](D:\xiangmu\fluffos_Z\24xinduobao\adm\daemons\jz_npc.c:68)
- [jz_npc.c:139](D:\xiangmu\fluffos_Z\24xinduobao\adm\daemons\jz_npc.c:139)

`jz_npc` 的核心模式比 `jz_skill` 更重：

1. 扫 `/d/` 全区域
2. `deep_path_list(dir + zone + "/")`
3. 每个房间文件先判断：
   - `room_name->is_room()`
   - `room_name->query("exits")`
4. 然后执行：
   - `find_object(room_name) || load_object(room_name)`
5. 对 `query("objects")` 里的对象继续跑：
   - `obj->is_npc()`
   - `obj->query("chat_msg")`
   - `obj->is_random_move()`

这条链的特点是：

- 会真实装载大量房间对象
- 再继续把房间里挂的 NPC/对象信息拉出来
- 比 `jz_skill` 那种“技能文件元信息扫描”更容易把启动期拖进深层对象树

这次 trace 里，`jz_npc` 自身只明确看到：

- `LPC Compile File`: `1.075 ms`
- `compile.yyparse`: `0.967 ms`

但没有拿到完整的 `load_object.call_create` 外层闭合事件。原因不是它不耗时，而是：

- 在 `jz_npc->create()` 往下继续装房间/NPC 的过程中
- 事件数已经被大量 LPC function trace 打满
- 外层 `ScopedTracer` 还没来得及结束，tracing 就停了

所以这次样本对 `jz_npc` 的结论应该是：

- **不是没问题**
- **而是太重，重到把 trace 上限先打满了**

## 结论五：从“对象树体量”看，地图房间/NPC 树是当前最重的一类

下面这三组是对样本中相关对象的累计统计。

注意：这是“累计成本桶”，不是严格 wall-clock，总和里包含嵌套调用，不能直接拿来当总启动时间。

### 装备树

前缀：

- `clone/armor/`
- `clone/weapon/`

累计结果：

- 对象数：`360`
- `LPC Load Object`: `648.974 ms`
- `load_object.call_create`: `53.873 ms`
- `LPC Compile File`: `561.047 ms`
- `compile.yyparse`: `532.671 ms`

### 技能树

前缀：

- `kungfu/skill/`

累计结果：

- 对象数：`349`
- `LPC Load Object`: `489.369 ms`
- `load_object.call_create`: `0 ms`
- `LPC Compile File`: `463.653 ms`
- `compile.yyparse`: `438.677 ms`

### 房间/NPC 树

前缀：

- `d/`
- `room/`

累计结果：

- 对象数：`357`
- `LPC Load Object`: `1494.254 ms`
- `load_object.call_create`: `1088.862 ms`
- `LPC Compile File`: `189.580 ms`
- `compile.yyparse`: `172.447 ms`

这组数据的含义很清楚：

- 技能树和装备树更像“冷编 + 查询型负载”
- 房间/NPC 树更像“运行初始化型负载”
- 真正把启动拖重的，往往不是 parser，而是对象 `create()` 里的房间/NPC 初始化链

## 结论六：当前 trace 已经足够支持一个很明确的判断

从驱动层看，当前 mudlib 启动慢的主因是：

1. preload daemon 在 `create()` 中主动触发大规模对象树扫描
2. 路径字符串调用落到 `call_other(string)`，未加载对象会继续走 `load_object()`
3. 冷对象装载后又进入 `create()`，形成深层主线程串行链

所以当前的启动慢，主要不是：

- `for/while` 指令本身慢
- 编译器 parser 本身特别慢
- 单个 `.c` 文件文本过大

而是：

- **对象装载与 create 链条太深**
- **大量 LPC 元信息查询是通过“先把对象真正装起来”实现的**

## 对后续优化方向的意义

基于这份 trace，驱动层后续判断可以收敛成三点：

1. 编译器并行化不是当前最现实的提速点  
   因为 `jz_all` / `jz_skill` 的 compile 本身只占不到 `1 ms`。

2. 真正值得继续盯的，是 `load_object()` 的冷路径和 `create()` 触发的对象树扩张  
   也就是：
   - 对象冷加载
   - inherit 链
   - `valid_object`
   - `call_create`

3. 如果后面还要继续做驱动级提速，应该优先做“让启动期问题看得更清楚”的能力  
   比如：
   - 更聚焦的 trace 抓法
   - 更大的 trace 事件上限
   - 或针对 `create()` 链的专门统计

而不是一开始就去碰编译器并行化。
