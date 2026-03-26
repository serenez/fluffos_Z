# 24xinduobao 对照下的 FluffOS 运行期热点验证（2026-03-26）

## 0. 边界修正（2026-03-26 晚）

这份文档保留的意义，只是：

- 把 `24xinduobao` 当作真实 LPC 调用样本；
- 用来检查驱动优化后有没有把 LPC 既有调用语义改错。

这份文档**不再用于给驱动问题排优先级**，也**不代表需要修改 mudlib**。

本轮真正落地的修复，全部都只发生在 `fluffos_Z` 驱动代码里，没有改
`24xinduobao` 业务 mudlib。

## 1. 目标

本文件用于把前一轮在驱动侧识别出的 LPC 运行期热点，放到真实业务
mudlib `24xinduobao` 中做一次“调用面验证”。

这一步的目标不是继续泛泛地列问题，而是回答下面三个更关键的问题：

1. 这个热点在真实 mudlib 中到底有没有被大量使用。
2. 它是落在玩家主路径，还是只落在管理命令、低频工具链路。
3. 后续驱动应该优先改什么，以及这样改会不会改错 LPC 语义。

---

## 2. 本次验证结论总览

### 2.1 直接结论

经过对 `24xinduobao` 的静态调用面扫描，本轮结论有了明显收敛：

- **最高优先级热点已经明确是 `present()`**。
- **`read_file()` 仍然有优化价值，但优先级低于 `present()`。**
- **`deep_inventory()` 可以改，但不再是第一批最该动的点。**
- **`parse_command()`、`objects()`、`livings()`、`match_path()`、`terminal_colour()`、内置 `regexp()/reg_assoc()` 都不该排进第一批驱动改造。**

### 2.2 调用量级统计

基于 `24xinduobao` 目录静态扫描得到：

- `present(...)`：**572** 次
- `read_file(...)`：**78** 次
- `objects(...)`：**11** 次
- `livings(...)`：**2** 次
- `deep_inventory(...)`：**3** 次
- `parse_command(...)`：**2** 次
- `regexp(...)`：**2** 次
- `reg_assoc(...)`：**1** 次
- `terminal_colour(...)`：**9** 次
- `pcre_match(...)`：**5** 次
- `match_path(...)`：**0** 次

这组数字说明：

- `present()` 在这份 mudlib 里是绝对头部热点。
- `read_file()` 是次级热点，但调用面明显更偏工具、帮助、配置、展示。
- `objects/livings/parse_command/match_path` 的原始优先级需要下调。

---

## 3. 一个关键发现：这份 mudlib 实际上重载了 `present()`

### 3.1 活跃 simul_efun 配置

`24xinduobao/config.ini:9`

```text
simulated efun file : /adm/single/simul_efun
```

而 `/adm/single/simul_efun` 又实际包含了：

- `24xinduobao/adm/single/simul_efun.c`
- `24xinduobao/adm/simul_efun/object.c`

其中 `present()` 的实际实现位于：

- `24xinduobao/adm/simul_efun/object.c:160`

核心逻辑：

```c
object present(mixed str, object ob)
{
	object obj;

	if(stringp(str)&&(obj=find_object(str)))
	{
		if(ob) return efun::present(obj,ob);
		else return efun::present(obj);
	}
	if(ob) return efun::present(str,ob);
	else return efun::present(str);
}
```

### 3.2 这意味着什么

这意味着在 `24xinduobao` 里，绝大多数 `present("xxx", env)` 调用，
不是直接打到驱动 `efun::present()`，而是先经过 mudlib 的 simul_efun
包装。

这个包装做了一个额外动作：

- 对所有 `string` 参数先尝试 `find_object(str)`

这会带来两个直接后果：

1. 即使目标本来只是背包、房间、容器内的普通 id 查找，也会先做一次
   全局对象查找尝试。
2. 驱动侧即便把 `present()` 优化了，mudlib 这一层额外开销仍然会留下。

### 3.3 最终判断

这是本次对照验证里最重要的新结论：

- **`present()` 仍然是驱动侧第一优先级优化项。**
- **mudlib 里的 simul_efun 包装只作为语义参照，不纳入本轮改造范围。**
- **驱动优化时必须考虑这层包装的存在，避免把 LPC 现有调用语义改坏。**

---

## 4. 热点逐项验证

## HOT-M01 `present()` 已被真实 mudlib 验证为最高优先级热点

### 位置与证据

驱动相关：

- `src/vm/internal/simulate.cc:756`

mudlib 相关：

- `24xinduobao/adm/simul_efun/object.c:160`
- `24xinduobao/cmds/std/look.c:169`
- `24xinduobao/cmds/std/get.c:58`
- `24xinduobao/cmds/std/get.c:88`
- `24xinduobao/feature/finance.c:14`
- `24xinduobao/feature/finance.c:41`
- `24xinduobao/adm/daemons/moneyd.c:110`
- `24xinduobao/d/changan/hubiao/hubiao.h:54`
- `24xinduobao/adm/daemons/channeld.c:214`

### 为什么这次验证把它提到第一位

`present()` 在 mudlib 中一共出现 **572 次**，而且不只是“分布广”。
更关键的是，它落在很多真实玩家路径上：

- 物品查看：`cmds/std/look.c`
- 拾取：`cmds/std/get.c`
- 金钱结算：`feature/finance.c`
- 钱币处理：`adm/daemons/moneyd.c`
- 护镖/任务界面：`d/changan/hubiao/hubiao.h`
- 大量 NPC、场景脚本、任务脚本

这说明它不是单一管理员命令热点，而是**游戏运行主路径热点**。

### 真实 mudlib 中的典型低效模式

#### 模式 A：重复查找同一个对象

`24xinduobao/d/changan/hubiao/hubiao.h:312-319`

这里对同一种令牌执行了多次 `present(...)`：

- 先判断是否存在
- 然后再次 `present(...)` 取对象
- 再读取 `query_amount()`

这类模式在玩家交互菜单、任务菜单里会持续重复。

#### 模式 B：同一逻辑里多次查找同一背包货币对象

`24xinduobao/feature/finance.c:14-16`

```c
gold = present("gold_money");
silver = present("silver_money");
coin = present("coin_money");
```

`24xinduobao/feature/finance.c:41-43` 又重复一次。

`24xinduobao/adm/daemons/moneyd.c:110-125` 和 `:190-193`
也有同类逻辑。

这类路径虽然单次不大，但累计频率高，而且高度稳定。

#### 模式 C：高频命令里的多次 presence 判断

`24xinduobao/cmds/std/look.c:633-811`

`look` 在同一个对象展示过程中，多次判断：

- `present(obj, me)`
- `present(obj, environment(me))`

这类命令在实际游戏里会高频触发，属于最值得优化的主路径。

#### 模式 D：simul_efun 包装会影响驱动优化收益与语义边界

`24xinduobao/adm/simul_efun/object.c:164`

这一层不是本轮要改的对象，但它说明了两件事：

- 驱动优化 `present()` 时，不能假设所有 LPC 都是直接调用 efun。
- 驱动优化完成后，最终收益会受到 mudlib 包装层影响，因此后续评估要看
  真实 LPC 调用结果，而不是只看驱动局部 benchmark。

### 影响

- 提高玩家常用命令的总 CPU 消耗。
- 放大驱动 `object_present2()` 中“每候选构造字符串并调 `id()`”的成本。
- 驱动优化完成后，收益会受到 mudlib 包装层影响，因此需要用真实 LPC
  调用路径回归验证。

### 决策

**结论：立即进入第一批改造。**

驱动侧改造顺序建议如下：

1. **先改驱动**  
   在 `object_present2()` 里去掉“每个候选重复构造查找字符串”的额外分配。

2. **改完后立刻用 mudlib 回归**
   重点回归 `look/get/finance/moneyd/hubiao` 这些高频调用面，确认
   `string` / `object` 两类参数行为没有被驱动改坏。

### 风险

- 驱动侧局部性能优化本身风险低。
- 真正的风险点不在“改不改 mudlib”，而在于驱动优化后是否仍兼容
  `24xinduobao/adm/simul_efun/object.c:160` 这种包装调用方式。

### 推荐做法

- 不触碰 mudlib 包装本身。
- 驱动改完后，专门回归下面两类调用：
  - `present("id", env)` 这种普通 id 查找
  - `present(obj_or_path_like_value, env)` 这种经过 simul_efun 包装的调用

---

## HOT-M02 `read_file()` 仍有优化收益，但优先级低于 `present()`

### 位置与证据

驱动相关：

- `src/packages/core/file.cc:295`

mudlib 相关：

- `24xinduobao/cmds/usr/help.c:28`
- `24xinduobao/cmds/usr/help.c:71`
- `24xinduobao/feature/more.c:195`
- `24xinduobao/adm/simul_efun/file.c:155`
- `24xinduobao/adm/simul_efun/file.c:179`
- `24xinduobao/adm/daemons/mapd.c` 多处
- `24xinduobao/adm/daemons/configd.c:33`

### 真实使用形态

`read_file()` 共 **78** 次。

其中主要分为两类：

1. **帮助/分页/展示类**
   - `cmds/usr/help.c`
   - `feature/more.c`

2. **工具/管理/配置加载类**
   - `adm/daemons/configd.c`
   - `adm/daemons/mapd.c`
   - `adm/daemons/logind.c`
   - `adm/simul_efun/file.c`

### 为什么它不是第一位

虽然调用量不低，但和 `present()` 不同，`read_file()` 的很多调用并不在
玩家每个动作都会命中的路径上。

它更多是：

- 查帮助
- 看说明
- 分页浏览
- 管理命令
- 守护进程加载配置

因此它是**明确有价值，但不如 `present()` 紧急**的第二梯队问题。

### 这次对照后，最值得改的方向

`24xinduobao/feature/more.c:195`

```c
content = read_file(file, line, page);
```

这说明 mudlib 已经大量依赖“按行截取”的读取方式。

所以驱动里做：

- 普通文件非 gzip 快路径
- 避免整文件读入后再线性切行

是有现实收益的。

### 决策

**结论：放入第二批驱动改造。**

### 风险

低。只要保持返回值、边界行号、压缩文件行为兼容即可。

---

## HOT-M03 `deep_inventory()` 有使用，但不是第一批主战场

### 位置与证据

- `24xinduobao/feature/guarder.c:44`
- `24xinduobao/cmds/usr/suicide.c:26`
- `24xinduobao/cmds/adm/updatei.c:109`

### 真实情况

`deep_inventory()` 在这份 mudlib 中只出现 **3 次**。

而且从调用面看：

1. `guarder.c`
   用于阻止玩家背着别的玩家闯门。
2. `suicide.c`
   用于删档前检查是否背着活人。
3. `updatei.c`
   管理命令里用于编译前搬运房间对象。

### 结论

这证明驱动里的“双遍递归 deep_inventory”确实是个实现问题，
但在这份真实 mudlib 下，它不是第一批最值得优先抢救的热点。

### 决策

**结论：可以改，但优先级下调到第三批。**

### 风险

低。只要保持结果顺序和返回集合不变，单遍迭代实现是安全的。

---

## HOT-M04 `parse_command()` 在这份 mudlib 下不构成主热点

### 位置与证据

- `24xinduobao/cmds/imm/call.c:87`
- `24xinduobao/cmds/imm/call_old.c:86`

### 真实情况

显式 `parse_command(...)` 调用只有 **2 次**，而且都在管理命令里。

### 结论

前一轮从驱动源码形态上看，`parse.cc` 的确像潜在热点；
但在这份 mudlib 里，**它没有被真实业务频繁使用**。

### 决策

**结论：从第一批改造中移出，暂不优先。**

---

## HOT-M05 `objects()` / `livings()` 不属于当前业务热点

### 位置与证据

- `24xinduobao/cmds/wiz/status.c:50`
- `24xinduobao/cmds/wiz/status.c:71`
- `24xinduobao/cmds/adm/updatei.c:67`
- `24xinduobao/mudcore/cmds/player/mudinfo.c:45`
- `24xinduobao/mudcore/cmds/player/mudinfo.c:46`
- `24xinduobao/mudcore/cmds/wizard/objects.c:10`
- `24xinduobao/mudcore/cmds/wizard/livings.c:9`

### 真实情况

- `objects(...)`：11 次
- `livings(...)`：2 次

绝大多数落在：

- 巫师命令
- 信息展示命令
- 维护链路

### 结论

驱动全对象扫描的设计问题仍然存在，但和这份 mudlib 的真实业务负载
相比，它并不是优先堵口。

### 决策

**结论：暂缓。除非后续采样明确看到它占 CPU。**

---

## HOT-M06 `match_path()` 当前没有真实调用

### 位置与证据

对 `24xinduobao` 的静态扫描结果为 **0 次**。

### 结论

无需排进当前优化计划。

### 决策

**结论：直接暂缓。**

---

## HOT-M07 内置 `regexp()/reg_assoc()` 不值得优先动

### 位置与证据

- `24xinduobao/include/net/ftpdsupp.h:199`
- `24xinduobao/include/net/ftpdsupp.h:252`
- `24xinduobao/adm/daemons/channeld.c:439`

### 真实情况

- `regexp(...)`：2 次
- `reg_assoc(...)`：1 次

其中最接近实时路径的是：

- `adm/daemons/channeld.c:439`

```c
tmp = reg_assoc(msg, ({ pattern }), ({ 1 }));
```

它用于频道消息地址剥离。

### 结论

这条路径不是完全没有意义，但从量级看，不足以支撑“现在就升级整套
正则引擎”的优先级。

### 决策

**结论：从当前批次里移出。**

如果未来需要做 PCRE2/JIT，也应当把它视为独立基础设施升级，而不是
为了解当前主性能瓶颈。

---

## HOT-M08 `pcre_match()` 已在少数路径使用，但不是当前主问题

### 位置与证据

- `24xinduobao/adm/simul_efun/chinese.c:34`
- `24xinduobao/adm/simul_efun/charset.c:32`
- `24xinduobao/mudcore/system/daemons/http/qq_d.c:38`
- `24xinduobao/mudcore/system/daemons/http/qq_d.c:41`

### 真实情况

`pcre_match(...)` 共 **5 次**，分布非常有限。

### 结论

PCRE1 -> PCRE2 + JIT 仍然是“理论上最值得升级的库项”，
但它不是当前这份 mudlib 的第一阻塞点。

### 决策

**结论：保留为中长期升级方向，当前不插队。**

---

## HOT-M09 `terminal_colour()` 真实调用面偏窄

### 位置与证据

- `24xinduobao/adm/daemons/qq.c:41`
- `24xinduobao/adm/daemons/qq.c:57`
- `24xinduobao/adm/daemons/qq.c:82`
- `24xinduobao/adm/daemons/qq.c:116`
- `24xinduobao/adm/daemons/qq.c:138`
- `24xinduobao/mudcore/system/kernel/creator.c` 多处

### 真实情况

`terminal_colour(...)` 共 **9** 次。

主要落在：

- QQ 机器人消息体模板填充
- 代码生成/脚手架文本

### 结论

它不是玩家核心链路中的普遍热点。

### 决策

**结论：暂缓，不进前两批。**

---

## 5. 优先级重排后的改造顺序

结合驱动实现和 `24xinduobao` 真实 mudlib，对当前优化顺序做如下重排：

### 第一批

1. **驱动：优化 `present()` 内部查找路径**
   - 目标：降低 `object_present2()` 的重复分配与重复字符串构造成本。

2. **驱动：优化 `read_file()` 普通文件快路径**
   - 目标：提升 `help`、`more`、配置读取、文件查看链路。

3. **驱动：把 `deep_inventory()` 改成单遍实现**
   - 目标：修正实现形态问题，顺便消掉递归双扫。

### 暂缓项

4. `parse_command()`
5. `objects()/livings()`
6. `match_path()`
7. `regexp()/reg_assoc()`
8. `terminal_colour()`
9. `PCRE2 + JIT` 升级

---

## 6. 最终判断

如果只允许我根据这份真实 mudlib 先动一批，我的最终判断是：

- **先动 `present()`，但只改驱动。**
- **第二个再动 `read_file()`。**
- **`deep_inventory()` 可以改，但已经不再是第一优先级。**
- **其余热点暂时不要抢跑。**

和前一轮相比，这份 `24xinduobao` 的最大价值不在于“证明所有问题都存在”，
而在于把真正该先改的点从源码形态热点里筛了出来。

现在可以进入下一阶段：

1. 先按驱动优先级顺序做代码改造。
2. 每改一项，就用 `24xinduobao` 的真实调用面回头检查是否破坏语义。
3. 改完 `present()` 后，再决定是否立即推进 `read_file()`。
