# FluffOS 性能专项审计（2026-03-26）

## 1. 审计范围与方法

- 审计目标：只记录“性能问题”，不重复展开本轮之前已记录的通用正确性/安全性问题。
- 扫描范围：`git ls-files` 下可执行/可编译代码与脚本文件共 `1719` 个，约 `579,769` 行。
- 覆盖方式：
  - 全量文本遍历，按文件逐个读取。
  - 在全量扫描基础上，重点复核运行时热点：日志输出、网络发送/flush、目录扫描、异步任务、数据库访问、解析器、配置读取。
  - 对发现点再回到源码上下文逐段确认，避免只凭关键词命中下结论。
- 说明：
  - 本轮是静态性能审计，没有做压力测试/benchmark。
  - 第三方 vendored 代码也在扫描范围内，但本文件只记录“本仓库本地逻辑或本地包装层”里可直接归因的问题；纯上游第三方实现未单独列项。

---

## 2. 发现汇总

当前确认的高置信度性能问题共 `11` 条，按影响从高到低排序。

---

## 2.1 修复状态（2026-03-26）

- `PERF-01` 已修复
  - 处理：去掉 `debug()` 宏的重复格式化，复用日志格式缓冲，调整为按行刷新，Windows 控制台输出改为复用宽字符缓冲。
  - 位置：`src/base/internal/log.h`、`src/base/internal/log.cc`
- `PERF-02` 已修复
  - 处理：telnet/OOB 协商输出改为“写缓冲后挂一次延迟 flush 事件”，避免小包即时刷出。
  - 位置：`src/comm.cc`、`src/comm.h`、`src/interactive.h`、`src/net/telnet.cc`、`src/packages/core/telnet_ext.cc`、`src/packages/core/mssp.cc`
- `PERF-03` 已修复
  - 处理：`get_dir()` 改为单遍扫描收集并直接排序，不再固定双遍 `readdir()`。
  - 位置：`src/packages/core/file.cc`
- `PERF-04` 已修复
  - 处理：`async_getdir()` 达到上限后立即停止扫描，并把排序下沉到 worker 线程。
  - 位置：`src/packages/async/async.cc`
- `PERF-05` 已修复
  - 处理：异步子系统改为单个常驻 worker + `condition_variable` 等待新任务，不再为每批任务重复建/销线程。
  - 位置：`src/packages/async/async.cc`
- `PERF-06` 已修复
  - 处理：异步完成回调增加事件合并标志，同一时刻只挂一个 `check_reqs()` 事件。
  - 位置：`src/packages/async/async.cc`
- `PERF-07` 已修复
  - 处理：关闭流程改为条件变量等待队列排空并 `join` worker，不再忙等抢锁。
  - 位置：`src/packages/async/async.cc`
- `PERF-08` 已修复
  - 处理：数据库句柄表改成稳定槽位，执行/拉取/提交/回滚/异步执行改为“每个 handle 一把锁”，不再所有连接共用一把全局执行锁。
  - 位置：`src/packages/db/db.h`、`src/packages/db/db.cc`、`src/packages/async/async.cc`
- `PERF-09` 已修复
  - 处理：配置解析改为按 key 建索引，避免配置项读取时反复线性扫描整份文本。
  - 位置：`src/base/internal/rc.cc`
- `PERF-10` 已修复
  - 处理：数字词解析改为预构建查找表，去掉双重循环和重复字符串拼装。
  - 位置：`src/packages/ops/parse.cc`、`compat/simuls/parse_command.c`
- `PERF-11` 已修复
  - 处理：`myutil` 的字符串 buffer 清理改为按过期时间推进的独立队列，不再从 `unordered_map.begin()` 随机扫。
  - 位置：`src/packages/myutil/myutil.cc`

- 设计说明：
  - `async` 仍然保持“后台 worker 做 I/O / 主线程执行 LPC 回调”的模型，没有扩成多 worker 并发执行。
  - 数据库锁粒度虽然缩小到了 handle 级别，但同一 handle 上的 `cleanup/execute/fetch/commit/rollback/close` 依旧串行，避免破坏单连接状态机。

---

## PERF-01 日志链路每条消息都做重复格式化、堆分配与强制 flush

- 位置：
  - `src/base/internal/log.h:53-63`
  - `src/base/internal/log.cc:99-125`
  - `src/base/internal/log.cc:32-59`
- 问题：
  - `debug()` 宏先 `snprintf()` 一次到 `_buf[1024]`，随后再调用 `debug_message()`。
  - `debug_message()` 又执行一轮 `vsnprintf(nullptr, 0, ...)` 计算长度，再分配 `std::vector<char>`，再第二次 `vsnprintf()` 真正写入。
  - 写出后无条件 `fflush(stdout)`，若日志文件已打开还会再 `fflush(debug_message_fp)`。
  - Windows 控制台路径还会额外做两次 `MultiByteToWideChar()` 再 `WriteConsoleW()`。
- 触发条件：
  - 任意启用 `DBG_*` 的运行期日志。
  - 启动期和错误路径上所有 `debug_message()`。
- 影响：
  - 每条日志都至少经历“格式化两次 + 一次堆分配 + 一到两次强制 flush”。
  - 在网络/解析/调试高频路径开启日志时，会明显放大系统调用成本、锁争用与 I/O 抖动。
  - 对 Windows 控制台输出尤其重，因为编码转换本身又是两遍。
- 建议：
  - 把日志格式化合并为单次缓冲写入，避免 `debug()` 和 `debug_message()` 双重格式化。
  - 将 `stdout`/日志文件改为批量刷新或按级别控制刷新，而不是每条日志都 `fflush()`。
  - Windows 下优先复用缓冲区，避免每条消息都重新做 UTF-8 -> UTF-16 长度探测与分配。

---

## PERF-02 Telnet/协商输出在大量调用点上立即 flush，导致小包写放大

- 位置：
  - `src/comm.cc:938-983`
  - `src/net/telnet.cc:105-116`
  - `src/net/telnet.cc:151`
  - `src/net/telnet.cc:262`
  - `src/net/telnet.cc:400`
  - `src/net/telnet.cc:571`
  - `src/net/telnet.cc:584`
  - `src/net/telnet.cc:593`
  - `src/packages/core/telnet_ext.cc:13`
  - `src/packages/core/telnet_ext.cc:26`
  - `src/packages/core/telnet_ext.cc:45`
  - `src/packages/core/telnet_ext.cc:61`
  - `src/packages/core/telnet_ext.cc:73`
  - `src/packages/core/telnet_ext.cc:130`
  - `src/packages/core/telnet_ext.cc:202`
  - `src/packages/core/telnet_ext.cc:239`
  - `src/packages/core/telnet_ext.cc:252`
  - `src/packages/core/mssp.cc:25-31`
- 问题：
  - 多个 telnet 协商/OOB 接口在写入一个很小的协商包后立刻 `flush_message(ip)`。
  - `flush_message()` 在 socket bufferevent 上会直接走 `evbuffer_write()`；SSL 路径还会直接 `SSL_write()` 当前输出缓冲。
  - 这相当于把 libevent 的聚合发送优势拆掉，退化成“构造一个小片段就立即写一次”。
- 触发条件：
  - 登录协商、NAWS、GMCP、MSDP、ZMP、MSSP、echo/linemode 切换等。
  - SSL 连接下影响更明显。
- 影响：
  - 产生大量小写调用、更多 syscall/SSL record 开销、更多包碎片。
  - 玩家登录、终端协商或脚本频繁发送协议扩展时，吞吐下降明显。
  - flush 频率越高，event loop 越难批量合并网络发送。
- 建议：
  - 将协议协商写入输出缓冲后延迟到一次批量 flush。
  - 只有在协议语义要求“立即可见”时才强制 flush。
  - 对同一 tick / 同一协商批次做合并发送。

---

## PERF-03 `get_dir()` 固定双遍目录扫描，且元信息模式下对每个条目额外 `stat()`

- 位置：
  - `src/packages/core/file.cc:221-277`
- 问题：
  - `get_dir()` 先完整 `readdir()` 一遍统计数量，再 `rewinddir()` 后第二遍重新读取并构造结果。
  - `flags == -1` 时还会为每个条目单独执行一次 `stat()`。
  - 构造完成后再对整批结果 `qsort()`。
- 触发条件：
  - 任意 `get_dir()` 调用。
  - 大目录、模糊匹配目录、`flags == -1` 的目录元信息查询最明显。
- 影响：
  - 固定产生“两次目录遍历 + N 次 stat + 一次全量排序”。
  - 大目录上会把 I/O、syscall 和 CPU 排序成本全部拉满。
  - 当 mudlib 把 `get_dir()` 用在在线列表、文件浏览、自动扫描时，容易形成热点。
- 建议：
  - 单遍扫描直接写入 `std::vector`，最终一次性转为 LPC array。
  - 若只需要前 N 项，避免先统计再回扫。
  - 可选地提供“不排序”或“仅文件名”快路径。
  - `flags == -1` 时优先复用目录项已有信息，必要时再惰性 `stat()`。

---

## PERF-04 `async_getdir()` 达到上限后仍继续扫完整个目录，而且排序仍在主线程完成

- 位置：
  - `src/packages/async/async.cc:282-307`
  - `src/packages/async/async.cc:336-347`
  - `src/packages/async/async.cc:400-417`
- 问题：
  - worker 线程在 `req->entries.size() >= req->limit` 后不再保存条目，但仍继续把整个目录扫到 EOF，只为了累加 `i`。
  - `handle_getdir()` 并没有把 `req->ret` 的总数暴露给调用方，说明这部分全量计数在当前实现里是纯额外成本。
  - 排序工作也没有在 worker 线程完成，而是在主线程 `qsort()`。
- 触发条件：
  - `async_getdir()` 读取大目录，且 `limit` 明显小于目录真实大小。
- 影响：
  - worker 线程做了大量“结果不会返回给调用方”的无效 `readdir()`。
  - 返回阶段主线程仍会因为排序而阻塞，削弱“异步目录读取”的意义。
  - 大目录下，这条路径会同时浪费后台 I/O 与前台 CPU。
- 建议：
  - 如果接口只返回前 `limit` 项，就在命中上限后直接停止扫描。
  - 如果必须返回总数，应在接口层显式暴露该语义，而不是做隐性全量计数。
  - 把排序下沉到 worker，主线程只做结果封装与回调。

---

## PERF-05 异步任务队列没有常驻 worker/条件变量，队列空了就销毁线程，下一次再重建

- 位置：
  - `src/packages/async/async.cc:73-118`
- 问题：
  - `do_stuff()` 发现 `reqs` 为空时直接 `std::thread(thread_func).detach()`。
  - `thread_func()` 一旦看到队列空就立刻 `return`，不会阻塞等待下一批任务。
  - 结果就是“队列每次从空转为非空”都要重新创建/销毁一个后台线程。
- 触发条件：
  - 高频、离散的小型异步调用，例如零散的 async read/write/getdir/db exec。
- 影响：
  - 线程创建/销毁成本被放大到每个任务批次。
  - 请求呈碎片化分布时，吞吐、调度稳定性和尾延迟都变差。
  - detach 模式也让状态协调更脆弱，难以做背压和并发度控制。
- 建议：
  - 改为常驻 worker 线程或固定线程池。
  - 使用 `condition_variable`/事件通知等待新任务，而不是空队列直接退出。
  - 明确限制并发度，避免线程生命周期抖动。

---

## PERF-06 异步完成回调没有做事件合并，每个完成请求都额外投递一个 0ms 事件

- 位置：
  - `src/packages/async/async.cc:97-109`
  - `src/packages/async/async.cc:450-486`
- 问题：
  - 每处理完一个请求，worker 都会执行一次 `add_walltime_event(std::chrono::milliseconds(0), [] { check_reqs(); })`。
  - 但 `check_reqs()` 本身是“单次调用就把 `finished_reqs` 全部 drain 干净”的实现。
  - 这意味着同一批 N 个完成请求会投递 N 个 0ms 事件，其中只有第一个真正有活干，后面的事件大概率只是空跑。
- 触发条件：
  - 一次批量完成多个异步请求。
- 影响：
  - backend tick 队列被大量瞬时小事件淹没。
  - 产生冗余锁、回调调度与事件节点分配成本。
  - 与 `PERF-05` 的线程抖动叠加后，异步子系统整体效率更差。
- 建议：
  - 为“异步结果待回收”增加一个原子/标志位，只允许同一时刻挂一个回收事件。
  - 或直接在主循环可控点统一回收 finished 队列，而不是每个请求都投递一次 0ms 事件。

---

## PERF-07 关闭流程里对异步队列采用忙等，自旋争抢互斥锁

- 位置：
  - `src/packages/async/async.cc:489-497`
  - `src/vm/internal/simulate.cc:69-77`
- 问题：
  - `complete_all_asyncio()` 通过 `while (true)` 持续加锁检查 `reqs.empty()`，直到队列清空。
  - 该循环没有 `sleep`、没有条件变量、也没有等待 worker 完成的同步原语。
- 触发条件：
  - 关服/退出阶段仍有异步 I/O 未完成。
- 影响：
  - 关闭阶段会空转占满一个 CPU 核心。
  - 主线程持续争抢 `reqs_lock`，反而会影响 worker 把剩余任务做完。
  - 任务越慢，忙等越明显。
- 建议：
  - 用条件变量、joinable worker 或计数栅栏等待异步任务完成。
  - 避免在退出路径上做无退避的锁轮询。

---

## PERF-08 数据库子系统使用单个全局互斥锁，导致所有连接都被串行化

- 位置：
  - `src/packages/db/db.cc:84-94`
  - `src/packages/db/db.cc:192-194`
  - `src/packages/db/db.cc:236-238`
  - `src/packages/db/db.cc:396-397`
  - `src/packages/db/db.cc:462-463`
  - `src/packages/db/db.cc:510-511`
  - `src/packages/db/db.cc:546-547`
  - `src/packages/db/db.cc:570-571`
  - `src/packages/db/db.cc:595-596`
  - `src/packages/async/async.cc:245-267`
- 问题：
  - `db_mut` 是单个全局 `pthread_mutex_t`。
  - close/commit/fetch/rollback/status，以及异步 `db_exec` 的 cleanup/execute 都会先拿这把全局锁。
  - 这意味着不同 DB handle、不同连接、甚至不同数据库类型之间也不能并发。
- 触发条件：
  - 开启 `PACKAGE_ASYNC` 且系统中存在多个数据库连接或多个并发查询。
- 影响：
  - “异步数据库”在驱动层被降级成“单线程串行数据库入口”。
  - 一个慢查询会直接拖住其他连接的提交、拉取和执行。
  - 吞吐上限和尾延迟都会被最慢那条查询主导。
- 建议：
  - 至少改为“每个 db handle 一把锁”。
  - 若底层客户端线程安全，可进一步细分到连接级别。
  - 把 `cleanup/execute/fetch` 的串行范围缩小到真正需要保护的对象。

---

## PERF-09 配置解析器把同一份配置文本反复线性扫描，启动成本是 O(配置项数 × 行数)

- 位置：
  - `src/base/internal/rc.cc:132-176`
  - `src/base/internal/rc.cc:230-404`
- 问题：
  - `read_config()` 先把全部配置行存进 `config_lines`。
  - 之后每读取一个配置项，就调用一次 `scan_config_line()` 从头到尾线性扫描整份 `config_lines`。
  - 端口项、obsolete 项、整型 flag 项都重复这一过程。
- 触发条件：
  - 驱动启动或重新读取配置。
- 影响：
  - 解析复杂配置时会重复执行大量 `sscanf()`。
  - 这是纯启动期成本，虽然不影响每帧运行，但在频繁重启/自动化测试/多实例拉起时会放大。
- 建议：
  - 读文件时直接拆成 `key -> value` 映射。
  - 只在首次解析时做一次 tokenize，后续按 key O(1)/O(log n) 读取。
  - obsolete 检查也复用同一份索引，不再反复扫整份文本。

---

## PERF-10 命令数字词解析仍保留双重循环 + 临时字符串拼装

- 位置：
  - `src/packages/ops/parse.cc:919-927`
  - `compat/simuls/parse_command.c:360-367`
- 问题：
  - 数字词（如 one/twenty three）解析时，使用 `10 x 10` 双重循环枚举所有组合。
  - 每次循环都拼接字符串，再与输入单词比较。
  - 源码注释已经直接写明 “This next double loop is incredibly stupid.”。
- 触发条件：
  - 命令解析里涉及数量词的路径。
- 影响：
  - 对单次调用来说是小开销，但它处在命令解析链路里，累计调用频率高。
  - 兼容层里还保留了一份同类实现，问题重复存在。
- 建议：
  - 预构建静态词典表，直接哈希/映射查找。
  - 兼容层与新实现统一复用同一份表，避免双份维护。

---

## PERF-11 `myutil` 的“惰性清理”并不按时间维度推进，长期运行后容易保留大量过期 buffer

- 位置：
  - `src/packages/myutil/myutil.cc:80-98`
  - `src/packages/myutil/myutil.cc:107-123`
  - `src/packages/myutil/myutil.cc:131-169`
- 问题：
  - `perform_lazy_cleanup()` 每次只从 `unordered_map.begin()` 开始检查前 `10` 个元素。
  - `unordered_map` 的迭代顺序与 `last_touch` 没有任何时间相关性，这不是 LRU，也不是时间轮。
  - 结果是很多早已过期的 buffer 可能长期躲在别的 bucket 中，持续占用容量。
- 触发条件：
  - 长时间运行，且 `strbuf_new()/strbuf_add()/strbuf_addf()` 被频繁调用。
- 影响：
  - `g_str_buffers` 容易长期维持在偏大的体量，拖慢哈希查找的常数项并恶化缓存局部性。
  - 到达 `MAX_STR_BUFFERS` 后，即使系统中实际多数对象已经过期，也可能因为没扫到而拒绝新建。
  - 这会把本应“平滑摊销”的清理策略变成“不稳定、不可预期”的容量抖动。
- 建议：
  - 改为按时间有序的数据结构维护过期队列，或记录游标持续推进，而不是每次都从 `begin()` 开始。
  - 若继续使用哈希表，至少要维护独立的过期索引。

---

## 3. 本轮未列入问题清单，但已复核的点

- `src/vm/internal/base/apply_cache.cc`
  - 当前是按 `program_t` 懒构建 `apply_lookup_table`，方向是对的；本轮未发现明显退化点。
- `src/compiler/internal/compiler.cc` 的若干 `qsort()`
  - 主要发生在编译阶段，属于一次性构建成本；目前更像设计/确定性问题，而不是明确的运行期性能缺陷。
- `src/backend.cc` tick 事件调度
  - 当前看到的额外开销更多来自异步子系统频繁投递 0ms 事件，而不是 tick 队列本身的单点设计。

---

## 4. 优先级建议

建议优先处理顺序：

1. `PERF-01` 日志链路重复格式化与强制 flush
2. `PERF-02` Telnet/协议协商路径立即 flush
3. `PERF-08` 数据库全局锁串行化
4. `PERF-04` / `PERF-05` / `PERF-06` / `PERF-07` 异步子系统整体验证与重构
5. `PERF-03` `get_dir()` 双遍扫描与逐项 `stat()`
6. `PERF-09` / `PERF-10` / `PERF-11` 启动期与长尾低频路径优化

---

## 5. 结论

这轮性能问题不是“零散小毛病”，而是比较集中地落在四条链路上：

- 日志输出链路：重复格式化、强制 flush、控制台转换成本高。
- 网络发送链路：协议协商过度立即 flush，破坏缓冲聚合。
- 异步基础设施：线程生命周期、事件回收、关闭等待策略都偏粗糙。
- 数据与文件链路：目录扫描、数据库互斥、配置读取仍有明显的线性重复工作。

如果只允许先修一批，最值得优先动手的是：`日志`、`telnet flush`、`数据库全局锁`、`async 队列模型`。
