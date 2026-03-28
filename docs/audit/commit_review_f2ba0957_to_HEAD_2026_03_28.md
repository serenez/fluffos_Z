# 提交审查记录：`f2ba0957b94ac3245fde0d3f085d7216313e9ab9..HEAD`

## 审查范围

- `f2ba0957b94ac3245fde0d3f085d7216313e9ab9`
- `10c699bea7d99275edbb4a6a2119b7e04fd6b63b`

审查方式：

- 逐提交看 `git show --stat --summary --name-only`
- 对高风险模块做源码级 diff 复核
- 重点检查运行时行为回归、对象生命周期、协议链、命令链和调度链

---

## 结论总览

本范围内确认到 3 条有价值记录：

1. `f2ba0957` 引入的 `comm` 命令终止符缓存，已经被 `10c699be` 回退；这是一个**已修复的历史问题**。
2. `f2ba0957` 引入的 `user_parser` 预收集 sentence 快照，存在**当前 HEAD 仍然保留的悬空指针/错误分发风险**。
3. `f2ba0957` 引入的 `PORT_TYPE_MUD` 批量包提取，存在**当前 HEAD 仍然保留的输入缓冲停滞风险**。

---

## 发现

### 🔴 [高] `f2ba0957` 的 `user_parser` 先快照 `sentence_t *`，会把后续已删除 sentence 继续拿来执行

涉及文件：

- `f2ba0957`
- `src/packages/core/add_action.cc`

证据点：

- 当前实现先在 `build_parse_command_cache()` / `collect_matching_sentences()` 中把命中的 `sentence_t *` 放进缓存和临时 `vector`
- 随后 `user_parser()` 再逐个消费这些快照指针
- 对应当前源码位置：
  - [add_action.cc](/D:/xiangmu/fluffos_Z/src/packages/core/add_action.cc#L124)
  - [add_action.cc](/D:/xiangmu/fluffos_Z/src/packages/core/add_action.cc#L137)
  - [add_action.cc](/D:/xiangmu/fluffos_Z/src/packages/core/add_action.cc#L637)

问题本质：

- 旧逻辑是沿着实时的 `command_giver->sent` 链表走
- 新逻辑改成“先收集一批 `sentence_t *`，再执行”
- 但命令处理过程中，前一个 action 可以触发：
  - `remove_action()`
  - `remove_sent()`
  - 对象移动
  - 对象析构
- 一旦后续 sentence 在前一个 action 里被释放，`matching_sentences` 里的指针就会变成悬空指针
- 下一轮循环仍会解引用 `s->flags`、`s->verb`、`s->function`，轻则错误分发，重则 UAF

对游戏的影响：

- 某个命令执行过程中，如果会删动作、移动对象、析构携带动作的物体
- 后续同一轮命令匹配就可能继续执行已经无效的 verb/action
- 体感上会表现为：
  - 命令误触发
  - 动作链错乱
  - 偶发崩溃或未定义行为

结论：

- 这是一个**当前 HEAD 仍未修复**的阻塞级问题

---

### 🔴 [高] `f2ba0957` 的 `get_user_data_mud()` 会把已进入 libevent 输入缓冲的数据卡住，等待下一次网络事件才继续

涉及文件：

- `f2ba0957`
- `src/comm.cc`

证据点：

- `get_user_data_mud()` 每轮先按“补齐头部”或“补齐当前包体”计算 `text_space`
- 读完后如果 `evbuffer` 里还有数据，但 `ip->text_end > 0`，就直接 `break`
- 对应当前源码位置：
  - [comm.cc](/D:/xiangmu/fluffos_Z/src/comm.cc#L134)
  - [comm.cc](/D:/xiangmu/fluffos_Z/src/comm.cc#L248)
  - [comm.cc](/D:/xiangmu/fluffos_Z/src/comm.cc#L265)
  - [comm.cc](/D:/xiangmu/fluffos_Z/src/comm.cc#L300)

问题本质：

- 当 `ip->text_end < 4` 时，代码只会读取“补齐 4 字节头”的那一点数据
- 如果同一个 libevent 输入缓冲里已经同时有后续包体数据，当前实现也会因为 `ip->text_end > 0` 直接退出循环
- 这样会把**已经在 `bufferevent` 输入缓冲里的剩余数据**留到以后再处理
- 但这批数据不一定会触发新的读回调；如果对端这次已经发完，连接就可能卡在“本地缓冲里明明有数据，但 driver 不继续消费”

对游戏的影响：

- `PORT_TYPE_MUD` 协议连接可能出现：
  - 包处理延迟
  - 协议停顿
  - 一次 burst 输入后需要等待下一次网络活动才继续
- 这类问题最难查，因为不是每次都表现成崩溃，更像“协议偶发卡死/半包停住”

结论：

- 这是一个**当前 HEAD 仍未修复**的阻塞级问题

---

### 🟡 [重要，已由后续提交回退] `f2ba0957` 的文本命令终止符缓存改变了提示周期/命令抽取时序，导致部分 mudlib 客户端协议头前混入默认 `> `

涉及文件：

- `f2ba0957`
- `10c699be`
- `src/comm.cc`
- `src/interactive.h`
- `src/user.cc`
- `src/user.h`
- `src/packages/gateway/gateway_session.cc`

记录说明：

- `f2ba0957` 引入 `text_command_end` 缓存，试图减少对同一条文本命令的重复扫描
- 这条优化后来被 `10c699be` 窄范围回退
- 回退原因已经在实际联调里坐实：它会改变部分 mudlib/client 的提示输出时序，导致协议头前混入默认 `> `

对游戏的影响：

- 客户端把协议头当普通文本显示
- 典型现象就是协议行前出现裸 `> `
- 进而导致客户端识别不到 `100/007` 这类协议头

当前状态：

- 该问题**已由 `10c699be` 回退**
- 本次范围的最后一个提交没有再引入新的同类逻辑问题

---

## 逐提交记录

### `f2ba0957` `perf(driver): 收口运行时性能第一批优化并补全基准记录`

审查结论：

- 这是一个大体量聚合提交，收益点很多，但也把多条运行时主链一次性改动打包进来了
- 当前确认的问题主要集中在：
  - `comm` 输入链
  - `user_parser` 命令链

已确认问题：

- 高：`user_parser` sentence 快照导致后续删除后的悬空指针风险
- 高：`PORT_TYPE_MUD` 批量提取会把已在 libevent 输入缓冲里的数据卡住
- 重要：文本命令终止符缓存改变提示周期/协议显示，后续已由 `10c699be` 回退

其他模块本轮结论：

- `heartbeat`、`gametick/call_out`、`parse_command`、`packages/parser` 这几块，这轮没有找到同等级的硬 bug 证据
- 但 `f2ba0957` 属于大聚合提交，后续若继续深查，优先级仍应放在“命令链”和“网络链”

### `10c699be` `revert(comm): 回退命令终止符缓存优化并恢复旧版输入抽取行为`

审查结论：

- 该提交是一次窄范围回退
- 目标明确，只回退 `comm` 文本命令缓存，不带入 `mapping` 和其他脏改动
- 这次复核中**未发现新的逻辑问题**

注意点：

- 它解决的是上一提交里的文本命令缓存回归
- 它**没有处理** `f2ba0957` 一并引入的 `PORT_TYPE_MUD` 批量读取停滞问题

---

## 最终判断

这段提交范围里，当前最值得优先处理的是两条仍然留在 HEAD 上的高风险问题：

1. `user_parser` 预收集 sentence 快照导致的悬空指针/错误分发
2. `PORT_TYPE_MUD` 批量读取在 partial packet 场景下可能把已缓冲数据卡住

文本命令终止符缓存导致的协议头前缀 `> ` 问题，已经由 `10c699be` 回退，不再算当前 HEAD 未解决项。
