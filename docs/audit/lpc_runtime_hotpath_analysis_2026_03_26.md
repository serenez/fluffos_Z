# FluffOS LPC 运行期热点路径详细分析（2026-03-26）

## 1. 目标与范围

- 目标：
  - 聚焦“LPC 侧最容易感知到”的运行期热点。
  - 不再泛泛看所有 `for/while`，而是只记录那些会在高频调用下明显放大 CPU、分配、递归深度、对象扫描、文件扫描或文本匹配成本的代码。
  - 对每个问题给出更细的剖析，并明确“下一步到底该怎么改”。
- 范围：
  - 驱动内核与直接暴露给 LPC 的 efun/基础设施。
  - 重点覆盖：对象递归扫描、命令解析、路径匹配、正则、文件读取、全对象扫描、文本渲染。
- 说明：
  - 本文是驱动侧静态分析结果，不是 profiling 采样结果。
  - “谁绝对最热”仍取决于 mudlib 的真实调用方式，所以本文会把“是否需要 mudlib 对照”单独标出来。

---

## 2. 总结结论

当前最值得优先盯住的热点，不是所有循环，而是下面 9 条：

1. `deep_inventory()` 双遍递归扫描
2. `present()` / `object_present2()` 对每个候选重复分配匹配字符串并调用 `id()`
3. `parse_command` 主链路仍有多层嵌套匹配和重复字符串拆分
4. `objects()` / `livings()` 依赖全对象扫描
5. `match_path()` 逐级构造前缀并逐次查 mapping
6. 内置 `regexp()` / `reg_assoc()` 仍是旧 regex 引擎路径
7. `pcre_*` 虽有缓存，但仍是 PCRE1，且没有 `study/JIT`
8. `read_file()` 统一走整块读取再按行裁切
9. `terminal_colour()` 是明显的多遍文本重写热点

其中最适合直接在驱动里先动手的，是：

- `HOT-01 deep_inventory()`
- `HOT-02 present()/object_present2()`
- `HOT-03 parse_command` 的局部热点
- `HOT-08 read_file()`

最适合“先拿 mudlib 使用情况验证，再决定是否值得重构”的，是：

- `HOT-04 objects()/livings()`
- `HOT-05 match_path()`
- `HOT-09 terminal_colour()`

最像“库或底层引擎升级项”的，是：

- `HOT-06` + `HOT-07` 正则链路

---

## 3. 逐项详细分析

### HOT-01 `deep_inventory()` 仍是双遍递归，属于高风险高收益热点

- 位置：
  - `src/vm/internal/base/array.cc:1221`
  - `src/vm/internal/base/array.cc:1248`
  - `src/vm/internal/base/array.cc:1298`
  - `src/vm/internal/base/array.cc:1367`
- 当前实现：
  - 先递归执行 `deep_inventory_count()`，把整棵对象树走一遍，统计结果数量。
  - 再递归执行 `deep_inventory_collect()`，把整棵树再走一遍，真正收集对象。
  - 带过滤函数时，还会在收集阶段继续执行 LPC function pointer 回调。
- 为什么会贵：
  - 固定双遍树扫描，本质就是 `2 x 遍历成本`。
  - 结构越深、房间内嵌套容器越多，递归成本越明显。
  - 如果 mudlib 依赖 `deep_inventory()` 做命令范围对象查找、清理、事件广播或战斗扫描，这会直接变成热点。
- 复杂度特征：
  - 时间复杂度仍是 `O(N)`，但常数项明显偏大。
  - 递归深度与对象嵌套深度绑定，存在深树栈风险。
- 影响到 LPC 的典型路径：
  - `parse_command` 侧会使用递归 inventory 结果。
  - 各类“房间内寻找所有对象/玩家/容器内容”的 mudlib 逻辑。
- 推荐改法：
  - 改成单遍的迭代 DFS/BFS。
  - 先用 `std::vector<object_t *>` 做临时收集，最后一次性转成 LPC array。
  - 过滤函数仍保持“主线程回调 LPC”的现有语义，不要移到后台线程。
- 风险评估：
  - 中等。
  - 主要风险不在算法，而在保持返回顺序、隐藏对象语义、过滤 function pointer 语义一致。
- 最终决策：
  - `驱动直接改`
- 是否需要 mudlib 对照：
  - 不是必须。
  - 但如果有 mudlib，可以进一步确认 `deep_inventory()` 具体被哪些系统高频调用。

---

### HOT-02 `present()` / `object_present2()` 在 inventory 扫描中对每个候选重复分配字符串

- 位置：
  - `src/vm/internal/simulate.cc:706`
  - `src/vm/internal/simulate.cc:756`
- 当前实现：
  - `object_present2()` 线性遍历 `ob->next_inv`。
  - 对每个候选对象，都重新 `new_string(namelen, "object_present2")`。
  - 然后把这个新字符串压栈，再调用一次 `apply(APPLY_ID, ob, 1, ORIGIN_DRIVER)`。
- 为什么会贵：
  - 这里真正贵的不是线性遍历本身，而是“每个候选一次分配 + 一次 LPC 调用”。
  - 如果房间里对象多，`present("sword")`、`present("guard 2")` 这类操作会把成本放大得很明显。
- 复杂度特征：
  - 遍历是 `O(K)`，其中 `K` 是当前 inventory 大小。
  - 但每步都带有分配和动态调用，常数项远大于普通遍历。
- 影响到 LPC 的典型路径：
  - 命令解析。
  - 房间物品操作。
  - 交互对象定位。
- 推荐改法：
  - 先做最低风险优化：
    - 把待匹配的 `name` 只构造一次，不要每个候选对象都重新分配。
    - 可以预先构造一个共享/临时 svalue，在循环中复用。
  - 不建议第一步就做 inventory `id` 索引，因为 mudlib 的 `id()` 是动态语义，驱动很难安全缓存。
- 风险评估：
  - 低到中等。
  - 只要不改 `id()` 的调用语义和匹配顺序，这类优化比较稳。
- 最终决策：
  - `驱动直接改`
- 是否需要 mudlib 对照：
  - 不必须。
  - 但如果 mudlib 大量依赖 `present()` 做命令解析前置检查，优先级会更高。

---

### HOT-03 `parse_command` 主链路仍有多层嵌套扫描和重复拆词

- 位置：
  - `src/packages/ops/parse.cc:998`
  - `src/packages/ops/parse.cc:1082`
  - `src/packages/ops/parse.cc:1145`
  - `src/packages/ops/parse.cc:1250`
  - `src/packages/ops/parse.cc:1323`
  - `src/packages/ops/parse.cc:1393`
  - `src/packages/ops/parse.cc:1474`
- 当前实现：
  - `item_parse()` 遍历候选对象。
  - `match_object()` 对每个对象遍历 id/plural id 列表。
  - `find_string()` 对多词 id 反复 `explode_string()`。
  - `check_adjectiv()` 还会构造 `adj1 adj2 ...` 的拼接字符串，组合回退匹配。
  - `member_string()` 是线性查数组。
- 为什么会贵：
  - 这是典型的“对象数 x 单对象可匹配词数 x 输入词数”的嵌套组合路径。
  - 还叠加了重复分配、重复 `explode_string()`、重复 `strcmp()`。
  - 命令系统越复杂、对象别名越多、形容词越多，这块越容易成为 CPU 热点。
- 复杂度特征：
  - 很难给出单一 Big-O，因为它叠加了多个维度。
  - 实际上更像：
    - `候选对象数`
    - `每个对象的 id/adj 列表大小`
    - `输入命令词数量`
    - `多词别名和多形容词的组合数`
- 推荐改法：
  - 第一阶段先做局部去重，不改整体语义：
    - `find_string()` 对多词 id 不要每次都 `explode_string()`，可在 `load_lpc_info()` 后预拆缓存。
    - `member_string()` 能哈希化的默认列表先哈希化。
    - `check_adjectiv()` 避免反复 `strcat()` 拼整句。
  - 第二阶段再考虑：
    - 给对象解析元信息建立更稳定的缓存结构。
- 风险评估：
  - 中等偏高。
  - 这里语义复杂，容易碰到历史 mudlib 对 `parse_command` 的兼容依赖。
- 最终决策：
  - `驱动直接改，但分阶段做`
- 是否需要 mudlib 对照：
  - 建议需要。
  - 因为不同 mudlib 对 `parse_command` 的依赖深度差异很大。

---

### HOT-04 `objects()` / `livings()` 仍依赖全对象链表扫描

- 位置：
  - `src/vm/internal/base/object.cc:2061`
  - `src/vm/internal/base/array.cc:1963`
  - `src/vm/internal/base/array.cc:1988`
- 当前实现：
  - `get_objects()` 直接遍历 `obj_list` 全链表，先构造临时对象指针数组。
  - `livings()` 在其上再过滤。
  - `objects()` 还可能继续执行 LPC 过滤函数。
- 为什么会贵：
  - 这是典型的全局 `O(N对象总数)` 扫描。
  - 如果 mudlib 在心跳、定时任务、广播、在线检查、垃圾扫描里频繁调 `objects()` 或 `livings()`，会非常重。
- 当前状态判断：
  - 从驱动实现上看，它没有明显算法 bug。
  - 问题在于这个 efun 的语义本来就决定了它贵。
- 推荐改法：
  - 优先不是改驱动，而是限制 mudlib 热路径用法。
  - 高频场景改成 mudlib 自维护 registry/list。
  - 驱动侧如果后续要增强，更合理的是增加更细的索引型 efun，而不是强行优化 `objects()` 现有语义。
- 风险评估：
  - 高。
  - 因为这是语义层面的“大扫全局”，不是普通局部热循环。
- 最终决策：
  - `先拿 mudlib 验证再改`
- 是否需要 mudlib 对照：
  - `需要`
  - 要先看 mudlib 是否真的在高频路径里滥用 `objects()/livings()`。

---

### HOT-05 `match_path()` 逐级构造路径前缀并逐次查 mapping

- 位置：
  - `src/packages/core/efuns_main.cc:948`
- 当前实现：
  - 先分配一块与输入路径等长的缓冲。
  - 从头扫描路径，逐步生成 `a/`、`a/b/`、`a/b/c` 这类前缀。
  - 每生成一个前缀就去 mapping 里查一次。
- 为什么会贵：
  - 路径越深，字符串写入与 mapping 查询次数越多。
  - 如果 mudlib 用它做权限控制、路径路由、虚拟目录、对象装载策略，这会在热路径重复发生。
- 推荐改法：
  - 两种方向：
    - 小改：对规范化后的路径结果做短期缓存。
    - 大改：把路径规则换成 trie/radix tree 一类结构。
  - 第一阶段更适合做缓存，不建议立刻重写数据结构。
- 风险评估：
  - 中等。
  - 主要风险在于缓存失效策略和 mapping 动态更新时的语义一致性。
- 最终决策：
  - `先拿 mudlib 验证再改`
- 是否需要 mudlib 对照：
  - `需要`
  - 要先看 mudlib 是否高频依赖 `match_path()`。

---

### HOT-06 内置 `regexp()/reg_assoc()` 仍是旧 regex 引擎，热路径不适合作为首选

- 位置：
  - `src/packages/core/regexp.cc:1402`
  - `src/packages/core/regexp.cc:1428`
  - `src/packages/core/regexp.cc:1589`
- 当前实现：
  - `match_single_regexp()` 每次都 `regcomp()`，随后 `regexec()`，最后释放。
  - `match_regexp()` 对整个数组复用一次编译后的 regexp，但仍走旧内置 regex。
  - `reg_assoc()` 会编译模式数组中的每一项，再在文本上反复推进匹配。
- 为什么会贵：
  - 旧内置实现本身就不是现代高性能正则路线。
  - 单次调用下“现编现跑”的成本高。
  - 文本列表过滤、日志匹配、命令模式、频道过滤一旦频繁使用，就会持续消耗 CPU。
- 推荐改法：
  - 驱动层面不建议继续给这套旧实现做大优化。
  - 更合理的路线是：
    - 把热路径 mudlib 调用迁移到 `pcre_*`。
    - 内置 `regexp` 保留兼容接口，不再作为高性能主路径。
- 风险评估：
  - 中等。
  - 主要风险是 mudlib 对旧 regex 语义的兼容依赖。
- 最终决策：
  - `不优先优化旧实现，优先推动热路径改用 pcre_*`
- 是否需要 mudlib 对照：
  - `建议需要`
  - 要确认 mudlib 当前到底主要用 `regexp` 还是 `pcre_*`。

---

### HOT-07 `pcre_*` 虽有缓存，但仍是 PCRE1，且没有 `study/JIT`

- 位置：
  - `src/packages/pcre/pcre.cc:406`

---

## 4. 2026-03-26 本轮修复落地结果

下面这一节记录的是已经真正落地到驱动代码里的修改，不再是“建议”
状态。

### FIX-01 `deep_inventory()` 改为单遍迭代扫描

- 修改位置：
  - `src/vm/internal/base/array.cc`
- 实际改法：
  - 删除原先的 `deep_inventory_count()` + `deep_inventory_collect()` 双遍递归。
  - 改成显式 `std::vector<object_t *>` 栈做单遍迭代遍历。
  - 在收集到对象时立即加引用，避免 function pointer 回调期间对象被析构后
    结果数组悬空。
- 语义保持：
  - 保留隐藏对象规则。
  - 保留 function pointer 的 `1 / 2 / 3` 返回值语义。
  - 保留 `take_top` 行为。
- 结果：
  - 去掉整棵树固定双遍扫描。
  - 去掉递归深度带来的栈风险。

### FIX-02 `present()` / `object_present2()` 去掉“每候选重复构造字符串”

- 修改位置：
  - `src/vm/internal/simulate.cc`
- 实际改法：
  - 把待匹配名字预先构造成一个可复用的 `svalue_t`。
  - 在 inventory 遍历过程中复用这一个参数对象，不再对每个候选重复
    `new_string()`。
- 语义保持：
  - 仍然按原顺序逐个调用 `id()`。
  - 仍然支持 `"name 2"` 这种编号匹配。
- 结果：
  - 去掉最明显的热路径分配放大点。
  - 不碰 mudlib `id()` 动态语义。

### FIX-03 `livings()` 改为专用命令对象链表；`objects()` 增加无过滤快路径

- 修改位置：
  - `src/packages/core/add_action.cc`
  - `src/packages/core/add_action.h`
  - `src/vm/internal/base/object.h`
  - `src/vm/internal/base/array.cc`
  - `src/vm/internal/simulate.cc`
- 实际改法：
  - 给 `enable_commands()` 对象维护一条专用双向链表。
  - `livings()` 直接遍历这条链表，不再每次全扫 `obj_list`。
  - `objects()` 在“无回调、无过滤”这一最常见路径上，直接单遍填充结果，
    不再先做临时快照数组。
- 语义保持：
  - 不改变 `enable_commands()` / `disable_commands()` 行为。
  - 对象析构时同步把节点从链表移除。
  - 回调版 `objects()` 仍保留原快照路径，避免回调期间对象链变化带来语义
    偏差。
- 结果：
  - `livings()` 从“全对象扫描”降为“仅命令对象扫描”。
  - `objects()` 常见无过滤场景减少一次额外内存分配与复制。

### FIX-04 `match_path()` 去掉逐级堆缓冲构造

- 修改位置：
  - `src/packages/core/efuns_main.cc`
- 实际改法：
  - 改成先规范化路径一次，再从长前缀向短前缀回退查找。
  - 使用 `std::string` 原地临时截断，而不是手工逐步拼前缀缓冲区。
- 语义保持：
  - 仍然优先精确匹配。
  - 仍然按最长匹配前缀返回。
- 结果：
  - 减少路径层级越深时的重复构造成本。

### FIX-05 `read_file()` 为普通文件增加正向流式快路径

- 修改位置：
  - `src/packages/core/file.cc`
- 实际改法：
  - 新增普通文件快速路径：当 `start > 0` 且文件不是 gzip 时，按块流式读入，
    只从目标起始行开始拼接结果。
  - gzip 文件、负数起始行等仍保留旧的大缓冲兼容路径。
  - 统一补了 CRLF 归一化，避免 Windows 文本行尾在返回值里反复产生额外
    处理。
- 语义保持：
  - 保留 `read_file()` 对 gzip 文件的兼容行为。
  - 保留 `start/lines` 的原有意义。
- 结果：
  - 对“只取后半段几行”的普通文本文件，避免先整段读完再线性跳过前缀。

### FIX-06 内置 `regexp()` / `reg_assoc()` 增加编译结果缓存

- 修改位置：
  - `src/packages/core/regexp.cc`
- 实际改法：
  - 增加小型正则缓存表，按 `pattern + excompat` 复用编译后的 regexp。
  - `regexp()`、`match_regexp()`、`reg_assoc()` 改为先查缓存，再决定是否
    `regcomp()`。
- 语义保持：
  - 不改旧正则引擎的匹配语义。
  - 不强推 mudlib 改用别的接口。
- 结果：
  - 把热路径上的“现编现跑”改成“多数情况下直接复用已编译结果”。

### FIX-07 `pcre_*` 缓存补上 `study/JIT` 预处理信息

- 修改位置：
  - `src/packages/pcre/pcre.cc`
  - `src/packages/pcre/pcre.h`
- 实际改法：
  - 扩展缓存桶，除了保存 `compiled_pattern`，还保存 `study_data` 和
    `jit_enabled`。
  - 命中缓存时直接复用 `pcre_extra`。
  - 首次编译后执行 `pcre_study()`，为后续高频执行预热。
- 语义保持：
  - 不更换 regex 引擎，不升级到 PCRE2。
  - 仍然维持现有 `pcre_*` efun 接口和返回值。
- 结果：
  - 高频相同模式执行时，减少重复准备成本。

### FIX-08 `parse_command` 去掉 `find_string()` 的重复拆词

- 修改位置：
  - `src/packages/ops/parse.cc`
- 实际改法：
  - `find_string()` 不再对每个多词短语反复 `explode_string()`。
  - 改成直接逐词和命令词数组比对。
  - `member_string()` 增加首字符和长度的快速过滤，减少无效 `strcmp()`。
- 语义保持：
  - 多词 id 的匹配顺序与返回行为保持不变。
  - 不改 `parse_command` 的历史兼容语义。
- 结果：
  - 去掉解析热路径里最显眼的重复拆分与无差别字符串比较。

### FIX-09 `terminal_colour()` 无钩子时不再每段都 `apply()`

- 修改位置：
  - `src/packages/contrib/contrib.cc`
- 实际改法：
  - 先检测当前对象是否实现 `terminal_colour_replace` 钩子。
  - 如果没有，就完全跳过逐段 `copy_and_push_string()` + `apply()`。
- 语义保持：
  - 实现了钩子的对象仍按原逻辑处理。
  - 未实现钩子的对象不再为空转进入 LPC。
- 结果：
  - 大段彩色文本渲染时，去掉了一整条“本来什么都不会发生”的 LPC 调用链。

### FIX-10 本轮明确不做的事

- 不修改 mudlib。
- 不把 mudlib 调用量当作驱动改造优先级的唯一依据。
- 不把单线程驱动强行改成多 worker 并发模型。
- 不做 PCRE2 升级和正则语义替换。

---

## 5. 验证结果

- 构建：
  - `C:\\msys64\\mingw64\\bin\\cmake.exe --build build_codex_review_fix --parallel 4`
- 测试：
  - `C:\\msys64\\mingw64\\bin\\ctest.exe --test-dir build_codex_review_fix --output-on-failure -j 1`
- 结果：
  - 编译通过
  - `46/46` 测试全部通过

---

## 6. 最终结论

本轮不是只补了一个点，而是把此前这批 LPC 运行期热点里，已经识别出的
驱动侧问题全部落到了代码上，并且保持了“单线程驱动、LPC 回调语义不变”
这个边界。
  - `src/packages/pcre/pcre.cc:431`
  - `src/packages/pcre/pcre.cc:436`
  - `src/packages/pcre/pcre.cc:474`
  - `src/packages/pcre/pcre.cc:682`
  - `src/packages/pcre/pcre.cc:977`
  - `src/packages/pcre/pcre.cc:1049`
- 当前实现：
  - 会缓存编译结果。
  - bucket 内也做了简单的 move-to-front。
  - 但缓存的是 PCRE1 编译结果。
  - 代码里明确留了 “Add support for studied patterns here” 的注释，当前并没有 `study/JIT`。
- 为什么还值得关注：
  - 这已经比旧 `regexp` 好很多，但仍不是这条链路的上限。
  - 如果 mudlib 大量依赖复杂正则，PCRE2 + JIT 仍然有现实收益空间。
- 推荐改法：
  - 中长期路线：
    - 评估升级到 PCRE2。
    - 在兼容前提下增加 JIT。
  - 不建议改成 RE2 这类语义差异较大的库，因为兼容风险高。
- 风险评估：
  - 高。
  - 这是库升级，不只是本地代码重排。
  - 会碰到构建系统、API 适配、pattern 兼容、平台支持等问题。
- 最终决策：
  - `作为二阶段库升级项，不作为第一批立即修改`
- 是否需要 mudlib 对照：
  - `建议需要`
  - 只有在 mudlib 的 regex 负载确实重时，这个升级才有高 ROI。

---

### HOT-08 `read_file()` 统一整块读取，再按行裁切；普通文件也走 gzip 路线

- 位置：
  - `src/packages/core/file.cc:295`
  - `src/packages/core/file.cc:430`
- 当前实现：
  - `read_file()` 会先 `gzopen()` / `gzread()` 把内容整段读入固定大缓冲。
  - 然后再根据 `start` / `lines` 从缓冲里线性定位起止位置。
  - 即便调用方只想要几行，也要先把前面的内容读出来并扫描过去。
- 为什么会贵：
  - 这对“小范围按行读取”非常不友好。
  - 如果帮助系统、日志尾部读取、公告分页、源码浏览都频繁使用 `read_file()`，LPC 会明显感到慢。
  - 普通文本文件统一走 `gz*` 路线，也会带来额外常数成本。
- 推荐改法：
  - 第一阶段：
    - 对普通文件增加 `fopen/fread` 快路径。
    - 只有压缩文件或确有 gzip 需求时才走 zlib。
  - 第二阶段：
    - 为按行读取增加流式定位能力，而不是总把整个区间先读到内存。
  - 不建议第一步就上 `mmap`，跨平台和边界处理成本更高。
- 风险评估：
  - 中等。
  - 风险点主要在兼容压缩文件行为和现有边界语义。
- 最终决策：
  - `驱动直接改`
- 是否需要 mudlib 对照：
  - 不是必须。
  - 但如果 mudlib 里大量做帮助/日志分页，优先级会更高。

---

### HOT-09 `terminal_colour()` 仍是明显的多遍文本重写热点

- 位置：
  - `src/packages/contrib/contrib.cc:531`
- 当前实现：
  - 先拆 `%^...%^` 片段。
  - 再按 mapping 做替换和长度计算。
  - 再做换行、缩进、颜色延续。
  - 期间夹杂大量 `strlen/strcpy/strcat/memcpy` 与多轮遍历。
- 为什么会贵：
  - 这段代码不是单次复杂，而是“多遍文本管线 + 多份中间状态”。
  - 如果 mudlib 对大段输出、频道输出、战斗刷屏、帮助文本渲染频繁使用它，CPU 成本会很可观。
- 但为什么不是第一批：
  - 它通常更偏“展示层热点”，不像 `parse_command/present/deep_inventory` 那样直接压在核心交互链路上。
  - 很多收益其实更适合在 mudlib 侧做缓存，而不是一上来重写驱动实现。
- 推荐改法：
  - 先看 mudlib 是否重复渲染相同模板。
  - 如果是，优先在 mudlib 做模板缓存。
  - 驱动侧如果后续再动，才考虑减少多遍扫描和重复 `strlen/strcat`。
- 风险评估：
  - 中等。
  - 功能边界多，容易出兼容问题。
- 最终决策：
  - `先拿 mudlib 验证再改`
- 是否需要 mudlib 对照：
  - `需要`

---

## 4. 关于“是否换更快的库”的结论

### 4.1 值得优先考虑的库/底层升级

- 正则：
  - 最值得考虑的是 `PCRE1 -> PCRE2 + JIT`。
  - 这是目前最像“换库就真能明显提速”的一项。

### 4.2 暂时不建议折腾的库升级

- JSON：
  - 仓库已经使用 `yyjson`，这条线已经是高性能路线。
  - 没必要再花精力换 JSON 库。
- 文件读取：
  - 这里主要是算法和 I/O 路径问题，不是“换个库就解决”。
- 全对象扫描：
  - 这里核心问题是接口语义本来就要求全扫，不是标准库容器慢。

### 4.3 比“换库”更值得先做的事

- 改算法：
  - `deep_inventory()` 单遍化
  - `read_file()` 普通文件快路径
  - `parse_command` 去掉重复拆词和重复拼串
- 减少分配：
  - `present()` 复用待匹配字符串
- 减少全局扫描：
  - 限制 mudlib 热路径使用 `objects()/livings()`

---

## 5. 推荐推进顺序

### 第一批：直接改驱动，收益高，风险可控

1. `HOT-01 deep_inventory()`
2. `HOT-02 present()/object_present2()`
3. `HOT-08 read_file()`
4. `HOT-03 parse_command` 的局部去重优化

### 第二批：看 mudlib 使用情况后再决定

1. `HOT-04 objects()/livings()`
2. `HOT-05 match_path()`
3. `HOT-09 terminal_colour()`

### 第三批：库升级专项

1. `HOT-06` 热路径逐步从 `regexp` 迁移到 `pcre_*`
2. `HOT-07` 评估 `PCRE2 + JIT`

---

## 6. 需要 mudlib 时，我最希望你提供什么

如果你后面愿意放一个 mudlib 进来，最有价值的不是“随便一份 mudlib”，而是能让我验证下面几类调用的真实使用频率：

- `deep_inventory`
- `present`
- `parse_command`
- `objects` / `livings`
- `regexp` / `reg_assoc` / `pcre_*`
- `read_file`
- `terminal_colour`
- `save_variable` / `restore_variable`

只要能把 mudlib 放进工作区，我就可以继续做两件事：

1. 统计真实调用面，重新排优先级
2. 判断哪些优化应该做在驱动，哪些应该做在 mudlib

---

## 7. 当前最终判断

仅从驱动静态实现看，LPC 运行期里最值得继续提升的，不是“再去找更多 while/for”，而是：

- 把递归扫描改成单遍
- 把高频查找里的重复分配去掉
- 把命令解析里最明显的重复拆词/拼串去掉
- 把文件读取改成真正适合按行访问的路径
- 把正则热路径尽量往 `pcre_*`，再往 `PCRE2 + JIT` 迁移

这些点比继续零散地抠小循环，更容易给 LPC 侧带来真实可感知的性能提升。
