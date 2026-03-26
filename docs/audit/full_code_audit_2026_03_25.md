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
| AUD-20260326-011 | FIXED | 中 | `compress` | `src/packages/compress/compress.cc:48` | `f_compress_file()` 在判断输入是否已经带 `.gz` 时，直接计算 `input_file + len - strlen(GZ_EXTENSION)`，没有先确认 `len >= 3`。 | `compress_file("")`、`compress_file("a")`、`compress_file("ab")` 这类短文件名。 | 越界读，错误路径判断不可靠；在特定构建和布局下可直接触发异常。 | 已抽成安全的后缀判断函数，先做最小长度检查，再比较 `.gz`。 |
| AUD-20260326-012 | FIXED | 中 | `compress` | `src/packages/compress/compress.cc:143` | `f_uncompress_file()` 在判断输入是否以 `.gz` 结尾时，同样直接使用 `input_file + len - strlen(GZ_EXTENSION)`，没有检查最小长度。 | `uncompress_file("")`、`uncompress_file("a")`、`uncompress_file("ab")` 等短文件名。 | 越界读，输入校验逻辑可被短字符串直接打穿。 | 已改成复用安全后缀判断逻辑，并补短文件名回归。 |
| AUD-20260326-013 | FIXED | 高 | `compress` | `src/packages/compress/compress.cc:68,166` | `f_compress_file()` / `f_uncompress_file()` 在 `check_valid_path()` 返回后，又把结果 `strcpy` 到固定 `char outname[1024]`。路径校验只判断权限和合法性，没有保证长度不超过 1023。 | 传入超长输出路径，或合法路径在归一化后仍超过 1023 字节。 | 栈缓冲区溢出，触发点在压缩/解压输出文件路径。 | 已改成 `std::string` 保存归一化后的输出路径，并补长输出路径定向回归。 |
| AUD-20260326-014 | FIXED | 高 | `vm/simulate` | `src/vm/internal/simulate.cc:353,508` | `filename_to_obname()` 之前会在缓冲打满后静默截断，`load_object()` 处理 `inherit_file` 时又把失败分支回退到裸复制，超长继承路径既可能被截断，也会把错误处理带回危险路径。 | 源码里写入超长 `inherit "<very long path>"`。 | 编译/加载对象时可能走错对象名，错误分支还会重新引入固定缓冲复制风险。 | 已让 `filename_to_obname()` 在截断时明确返回失败，`load_object()` 对超长 `inherit_file` 直接报错退出，并补回归。 |
| AUD-20260326-015 | FIXED | 高 | `tools/preprocessor` | `src/tools/preprocessor.hpp:940,974,992` | 预处理器在处理 `#include` 失败时，把待包含文件名用 `sprintf(buf, "Cannot %cinclude %s", ...)` 写进固定 `static char buf[1024]`。`name` 直接来自源码里的 include 字符串，长度没有上限。 | 构造超长 `#include "...."` 路径，且文件不存在或 `fopen()` 失败。 | 工具链进程栈缓冲区溢出，触发点在构建阶段。 | 已改成动态字符串组装错误消息，并补长 include 失败回归。 |
| AUD-20260326-016 | FIXED | 高 | `tools/preprocessor` | `src/tools/preprocessor.hpp:1106-1107` | 预处理器在报 `Unrecognised %c directive` 时，把整行源代码 `yyp` 通过 `sprintf` 写入固定 `char buff[200]`。非法指令行长度完全由源码控制。 | 写入超长非法预处理指令，例如极长的 `#foobar...`。 | 构建阶段栈缓冲区溢出。 | 已改成动态字符串报错，并补超长未知指令回归。 |
| AUD-20260326-017 | FIXED | 中 | `tools/build_applies` | `src/tools/build_applies.cc:25-27,33,50,75` | `build_applies` 打开输入/输出文件后没有判空，随后立刻对 `out`/`table` 执行 `fprintf()`，并对 `f` 执行 `fgets()`。只要 `src_dir` 错误、当前目录不可写或目标文件无法创建，工具就会直接解引用空 `FILE *`。 | 构建目录不可写、传入错误源码目录、磁盘/权限异常。 | 构建工具崩溃，错误信息不可读，也会干扰自动化生成流程。 | 已对所有 `fopen()` 结果判空，失败时打印可读错误并返回非零退出码，同时补工具失败路径回归。 |
| AUD-20260326-018 | FIXED | 高 | `core/ed` | `src/packages/core/ed.cc:871-875` | `getfn()` 读取 `e`/`r`/`w`/`f` 等命令后的文件名时，按字符连续写入 `static char file[MAXFNAME]`，没有任何长度检查。 | 在 ed 命令里输入超长文件名。 | 栈缓冲区溢出，触发点在运行时编辑器命令解析。 | 已改成 `std::string` 承接命令文件名，并补长绝对路径回归。 |
| AUD-20260326-019 | FIXED | 中 | `core/ed` | `src/packages/core/ed.cc:863-866` | `getfn()` 在“沿用当前文件名”分支里先写入 `'/'`，再执行 `strcpy(file + 1, P_FNAME)`。当 `P_FNAME` 长度正好达到 `MAXFNAME - 1` 时，会把结尾 `\\0` 写到数组末尾之外。 | 当前 ed 文件名长度达到 255 字节，并执行无参数的相关命令。 | 固定缓冲区 1 字节越界写，可能污染相邻静态状态。 | 已改成动态字符串复用当前文件名，不再依赖固定缓冲。 |
| AUD-20260326-020 | FIXED | 中 | `core/ed` | `src/packages/core/ed.cc:888-897` | `getfn()` 对 `make_path_absolute()` 和 `check_valid_path()` 返回的路径都直接 `strncpy` 到 `MAXFNAME=256` 的固定缓冲，超过长度时静默截断，后续读写会落到错误文件。 | 合法绝对路径长度超过 255 字节。 | 编辑器读写错误文件，表现为数据错写、状态错乱或“文件名显示正常但实际目标不一致”。 | 已改成动态路径承接，并同步修正 `ed_start()` / `object_ed_start()` 的文件名状态保存。 |

## 建议修复顺序

1. `file.cc`、`log.cc`、`make_func.y`、`checkmemory.cc`、CLI 输出失败路径和 testsuite/std 这一批已全部收口。
2. 下一轮可把重点转到 `src/packages/contrib/` 的错误处理和格式化字符串路径。
3. 继续扫 `src/vm/internal/` 里其余启动、trace 和错误输出链路。
4. 继续补工具链失败路径回归，尤其是还没覆盖到的生成工具和边界输入组合。

## 下一轮审计建议

- 继续扫 `src/packages/core/` 里 `regexp.cc`、`file.cc`、`log.cc` 等剩余固定缓冲热点。
- 继续扫 `src/vm/internal/` 里启动配置、错误输出、trace 相关路径。
- 对 `src/packages/contrib/` 和 `src/tools/` 做一次格式化字符串与失败路径专项。
- 继续扫 `src/packages/core/trace.cc` 周边未覆盖的 trace/debug 输出链路。
- 继续补 `src/tools/` 里 `make_func`、`make_options_defs` 之外工具的失败路径验证。

## 本轮验证

- `build_codex_review_fix` 重新编译 `lpc_tests`、`build_applies`、`make_options_defs` 通过。
- 新增定向回归通过:
  - `DriverTest.TestEdTracksLongFilenamesWithoutTruncation`
  - `DriverTest.TestIncludeResolutionHandlesLongCurrentFilenameSafely`
  - `DriverTest.TestBuildAppliesReportsMissingInputFileGracefully`
  - `DriverTest.TestMakeOptionsDefsHandlesLongMissingIncludeGracefully`
  - `DriverTest.TestMakeOptionsDefsHandlesLongUnknownDirectiveGracefully`
  - `DriverTest.TestO2JsonReportsOutputOpenFailure`
  - `DriverTest.TestJson2OReportsOutputOpenFailure`
  - `DriverTest.TestGenerateKeywordsReportsOutputOpenFailure`
  - `DriverTest.TestMakeOptionsDefsReportsOutputOpenFailure`
  - `DriverTest.TestMakeFuncHandlesLongAliasWithoutOverflow`
- 整套回归通过:
  - `ctest --test-dir build_codex_review_fix --output-on-failure -j 1` 结果 `56/56`

## 本轮追加审计记录

- 本轮已完成 `apply.cc`、`dwlib.cc`、`file.cc`、`simul_efun.cc`、`interpret.cc` 的修复。
- `call_other` 长函数名和 `get_line_number` 长文件名已经补上回归。
- `dwlib` 的定向辅助对象已补入 testsuite，但当前测试配置未启用 `PACKAGE_DWLIB`，对应回归未参与执行。
- 已修复 `TestAsciiChunkStopsAfterProcessInputDestructsObject` 的批量执行波动，问题根因是测试在对象自毁后继续访问已析构对象。
- `compress.cc` 和 `simulate.cc` 这轮新增的 4 条问题已全部修复并补回归。
- 已补修 `preprocessor.hpp`、`build_applies.cc`、`ed.cc` 的 6 条已确认问题，并补工具/长路径定向回归。
- 本轮额外收口了 `trace.cc` 的格式串风险和 `lex.cc` 的 include 路径固定缓冲链路。
- `ed_start()` / `object_ed_start()` 读取长路径后再截断保存状态的问题，已随 `ed.cc` 动态文件名改造一并修正。
- 已继续收口 `file.cc/get_dir()` 的长路径拼装，并确认 `stat()` 失败分支保持安全清零行为。
- `log.cc` 在日志文件不可用时改成有限队列缓存，避免 `pending_messages` 无上限增长。
- `main_o2json.cc`、`main_json2o.cc`、`main_generate_keywords.cc`、`make_options_defs.cc` 已补输出文件打开/写入/关闭失败处理。
- `make_func.y` 已用动态字符串重写生成文本，顺手去掉相邻 `operator`/`Invalid type` 固定缓冲热点。
- `checkmemory.cc` 的默认失败消息改成动态字符串；`number_string.c`、`base64.c` 的逻辑和告警问题也已一并收口并补回归。
