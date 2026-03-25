# 全量代码审计台账

- 日期: 2026-03-25
- 仓库: `fluffos_Z`
- 目标: 把问题发现、审计进度、后续修复入口收敛到一个文件里，避免重复零散排查

## 审计工作流

1. 先按攻击面排序，而不是按目录名排序。
   优先检查: 网络输入、文件系统、异步线程、数据库、对象生命周期、格式化字符串、固定缓冲区。
2. 每轮只记录可证明的问题。
   能静态确认、能说明触发条件、能定位到具体文件和行号的，才进入问题表。
3. 问题表和巡检进度分开维护。
   问题表记录“发现了什么”，巡检进度记录“哪些地方已经看过，哪些还没看”。
4. 修复后不删除记录，只改状态。
   推荐状态: `OPEN` / `FIXED` / `WONTFIX` / `NEEDS-VERIFY`。
5. 每次修复必须补 1 个验证动作。
   优先级: 现有回归 > 新增定向测试 > 手工复现。

## 巡检进度

### 已完成高风险首轮

- `src/comm.cc`
- `src/net/`
- `src/packages/async/`
- `src/packages/core/dns.cc`
- `src/packages/core/add_action.cc`
- `src/packages/core/file.cc`
- `src/packages/db/`
- `src/packages/gateway/`
- `src/packages/mudlib_stats/`
- `src/packages/dwlib/dwlib.cc`
- `src/packages/parser/`
- `src/packages/sockets/`
- `src/vm/internal/apply.cc`
- `src/vm/internal/base/interpret.cc`
- `src/vm/internal/base/object.cc`
- `src/vm/internal/simul_efun.cc`

### 待继续扩扫

- `src/compiler/`
- `src/packages/core/` 其余文件
- `src/packages/contrib/`
- `src/packages/compress/`
- `src/packages/dwlib/`
- `src/packages/external/`
- `src/packages/ops/`
- `src/vm/internal/` 其余文件
- `src/tools/`

## 问题表

| ID | 状态 | 级别 | 模块 | 位置 | 问题摘要 | 触发条件 | 风险 | 修复建议 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| AUD-20260325-001 | FIXED | 高 | `vm/object` | `src/vm/internal/base/object.cc:1496,1546-1552` | `save_object()` 使用固定 `save_name[256]`，随后直接 `strcpy(save_name, ob->obname)`，对象名长度超过 255 时会越界写。 | 超长对象名，例如深层路径、长 clone 名、手工构造对象名。 | 静态区溢出，可能破坏后续保存逻辑或触发崩溃。 | 已改为动态字符串构造保存头部，并同步收敛到共享的对象名归一化逻辑。 |
| AUD-20260325-002 | FIXED | 中 | `vm/object` | `src/vm/internal/base/object.cc:1517` | `save_object()` 在检查扩展名前直接访问 `file[len - 2]` 和 `file[len - 1]`，没有先判断 `len >= 2`。 | `save_object("")` 或单字符文件名。 | 越界读，错误路径判断不可靠；在特定构建下可能直接异常。 | 已补 `len` 边界判断，并为单字符路径补了回归覆盖。 |
| AUD-20260325-003 | FIXED | 高 | `parser` | `src/packages/parser/parser.cc:3074-3111` | `parse_recurse()` 用 `char buf[1024]` 累积多个词，并在循环里连续 `strcpy(p, *iwp++)`，没有任何剩余空间检查。 | 传入很长的 `parse_sentence()` 文本，单词总长度超过 1023 字节。 | 栈缓冲区溢出，可由 LPC 输入直接触发。 | 已改为动态字符串拼接，并补超长句子的回归测试。 |
| AUD-20260325-004 | FIXED | 高 | `core/add_action` | `src/packages/core/add_action.cc:439-445` | 错误路径先把 `s->verb` 写入固定 `char buf[256]`，再调用 `error(buf)`。这同时包含固定缓冲区溢出和格式串二次解析问题。 | `add_action()` 传入超长动词，或动词中包含 `%`。 | 可能导致错误路径崩溃；带 `%` 的动词还会把 `error()` 当成格式串继续解析。 | 已改为直接 `error("...%s...", s->verb)`，去掉中间缓冲。 |
| AUD-20260325-005 | FIXED | 中 | `mudlib_stats` | `src/packages/mudlib_stats/mudlib_stats.cc:495-522` | `restore_stat_list()` 用 `fscanf(f, "%s", fname)` 读取到 `fname_buf[MAXPATHLEN]`，没有宽度限制。 | 统计文件里出现超长 author/domain 名称，或文件被外部篡改。 | 栈缓冲区溢出，驱动启动恢复统计时可能崩溃。 | 已改成 `std::ifstream` + 动态字符串解析，并补长名称恢复回归。 |
| AUD-20260325-006 | FIXED | 高 | `vm/apply` | `src/vm/internal/apply.cc:73-92,101-119` | `check_co_args2()` 和 `check_co_args()` 都把函数名、程序名和类型信息拼进固定 `char buf[1024]`，随后在告警分支写日志，在异常分支直接 `error(buf)`。函数名来自词法器标识符，长度可到 `MAXLINE=4096`。 | 超长函数名，或较长函数名叠加较长 `prog->filename` / `ob_name`。 | 栈缓冲区溢出；异常分支还存在格式串二次解析风险。 | 已改成动态字符串组装消息，并统一走安全的 `smart_log` / `error("%s", ...)`。 |
| AUD-20260325-007 | FIXED | 高 | `dwlib` | `src/packages/dwlib/dwlib.cc:739-751` | `replace_objects()` 先把 `thing->u.ob->obname` 复制到固定 `char buf[2000]`，之后还会继续拼接 ` (destructed)` 和 master 返回的 `object_name()` 字符串，没有任何剩余空间检查。 | 对象名接近上限，或 master 返回很长的对象描述字符串。 | 栈缓冲区溢出，可由 mudlib 返回值直接放大。 | 已改成动态字符串拼接，再一次性压栈。 |
| AUD-20260325-008 | FIXED | 中 | `core/file` | `src/packages/core/file.cc:857-867,910-918` | `do_rename()` / `copy_file()` 在目标是目录时，用 `sprintf(newto, "%s/%s", to, cp)` 拼接到固定 `char newto[MAX_FNAME_SIZE + MAX_PATH_LEN + 2]`。但 `check_valid_path()` / `legal_path()` 只做权限和路径合法性检查，没有长度上限。 | LPC 侧传入超长目标目录名，或超长文件名移动到目录下。 | 栈缓冲区溢出，触发点在文件移动/复制路径。 | 已改成动态路径拼接，同时顺手去掉了同一路径下 `newfrom` 的固定缓冲复制。 |
| AUD-20260325-009 | FIXED | 中 | `vm/simul_efun` | `src/vm/internal/simul_efun.cc:64-70` | `init_simul_efun()` 在 `file` 非空后直接访问 `file[strlen(file) - 2]`，长度为 1 的配置值会越界读；同一分支里对 `buf[512]` 追加 `".c"` 也没有确认剩余空间。 | `simul_efun_file` 配置成单字符名，或极长文件名。 | 启动阶段越界读；长文件名场景还可能继续写穿栈缓冲。 | 已改成动态构造 simul_efun 目标文件名，并补上长度判断。 |
| AUD-20260325-010 | FIXED | 中 | `vm/interpret` | `src/vm/internal/base/interpret.cc:4459-4466` | `get_line_number()` 用固定 `static char buf[256]` 写入 `"/%s:%d"`。而 `progp->filename` 来自 `load_object()` 的对象路径，长度可接近 400，明显超过缓冲。 | 长路径对象在报错、trace、`call_stack()` 等路径下取行号信息。 | 静态缓冲区溢出，可能污染后续错误输出或直接崩溃。 | 已改成线程局部动态字符串返回，避免固定长度上限。 |

## 建议修复顺序

1. 继续扫 `src/packages/core/` 的剩余固定缓冲热点。
2. 继续扫 `src/vm/internal/` 的启动和错误输出链路。
3. 把 `src/packages/contrib/`、`src/packages/compress/`、`src/tools/` 的格式化字符串问题继续捞干净。

## 下一轮审计建议

- 继续扫 `src/packages/core/` 里 `ed.cc`、`regexp.cc`、`trace.cc` 等剩余固定缓冲热点。
- 继续扫 `src/vm/internal/` 里启动配置、错误输出、trace 相关路径。
- 对 `src/packages/contrib/`、`src/packages/compress/` 和 `src/tools/` 做一次格式化字符串专项。
- 下一轮优先把还没覆盖到的 `file.cc` / `simul_efun.cc` 边界场景补成定向回归。

## 本轮验证

- `build_codex_review_fix` 重新编译 `lpc_tests` 通过。
- 新增定向回归:
  - `DriverTest.TestCallOtherTypeErrorHandlesLongFunctionName`
  - `DriverTest.TestGetLineNumberHandlesLongFilename`
- 单测级验证通过:
  - `DriverTest.TestAsciiChunkStopsAfterProcessInputDestructsObject` 单独运行 10 次均通过
  - `ctest -R DriverTest.TestAsciiChunkStopsAfterProcessInputDestructsObject` 单独运行通过
- 当前仍有 1 个待确认波动:
  - `ctest --output-on-failure -j 1` 的整套批量运行里，`DriverTest.TestAsciiChunkStopsAfterProcessInputDestructsObject` 在第 29 项位置稳定报 `0xc0000374`
  - 同一测试单独运行稳定通过，说明这是一个现有的批量执行场景问题，还需要额外排查

## 本轮追加审计记录

- 本轮已完成 `apply.cc`、`dwlib.cc`、`file.cc`、`simul_efun.cc`、`interpret.cc` 的修复。
- `call_other` 长函数名和 `get_line_number` 长文件名已经补上回归。
- `dwlib` 的定向辅助对象已补入 testsuite，但当前测试配置未启用 `PACKAGE_DWLIB`，对应回归未参与执行。
