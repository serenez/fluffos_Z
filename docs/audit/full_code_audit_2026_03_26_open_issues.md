# 全量代码问题清单（2026-03-26）

- 仓库: `fluffos_Z`
- 日期: `2026-03-26`
- 目标: 把当前代码里仍然存在、且能静态确认的问题集中记录到一个新文档里，便于后续逐条修复
- 结论口径: 只记录“当前仍存在”的问题；上一轮已经修复的历史问题不重复记入本表

## 审查范围

- 已遍历 `git ls-files` 中的源码/工具/测试/脚本配置文件，仓库文本型代码文件总量约 `3242` 个
- 自研 `src/`（排除 `src/thirdparty`）共 `303` 个文件，其中代码类文件约 `276` 个
- `testsuite/` 共 `421` 个文件
- `compat/`、`cmake/`、`.github/workflows/`、`docs/.vitepress/` 已做同步筛查
- `src/thirdparty/` 体量很大，本轮只记录“本项目自己要承担的集成面问题”，不上升为逐条上游缺陷台账

## 审查方法

- 危险模式筛查: `strcpy` / `strcat` / `sprintf` / 固定缓冲区 / `FILE *` 打开失败路径 / 变参格式函数
- 关键模块人工审读: 编译器、VM、文件系统、日志、工具链、ed、testsuite 标准库
- 运行验证:
  - 直接运行 `ctest` 会因为 `C:\msys64\mingw64\bin` 不在 `PATH` 中而全部报 `0xc0000135`
  - 补上 `PATH` 后，`build_codex_review_fix` 下 `46/46` 测试全部通过

## 发现摘要

- 高: 8
- 中: 7
- 低: 2

## 问题清单

### AUD-20260326-001 [高] `core/ed` 复用当前文件名时有 1 字节越界写

- 位置: `src/packages/core/ed.cc:863-866`
- 问题: `getfn()` 在无参数分支先写 `file[0] = '/'`，随后直接 `strcpy(file + 1, P_FNAME)`。当 `P_FNAME` 长度正好等于 `MAXFNAME - 1` 时，结尾 `\0` 会写到数组边界外。
- 触发: 当前正在编辑的文件名长度达到 `255`，然后执行复用当前文件名的 `e/r/w/f` 类命令。
- 影响: 固定缓冲区 1 字节越界写，容易污染相邻静态状态，表现为随机文件名异常或编辑器状态错乱。
- 建议: 改成有界复制，并为前导 `/` 和结尾 `\0` 预留空间。

### AUD-20260326-002 [高] `core/ed` 读取命令行文件名时完全没有长度上限

- 位置: `src/packages/core/ed.cc:871-875`
- 问题: `getfn()` 逐字符把 `inptr` 写入 `static char file[MAXFNAME]`，没有任何剩余空间检查。
- 触发: 在 `ed` 命令里输入超长文件名。
- 影响: 栈/静态缓冲区溢出，可直接打坏编辑器状态。
- 建议: 在写入循环里加长度判断，超长立即报错返回。

### AUD-20260326-003 [中] `core/ed` 会静默截断合法绝对路径，后续可能读写错文件

- 位置: `src/packages/core/ed.cc:888-897`
- 问题: `make_path_absolute()` 和 `check_valid_path()` 的返回值都被 `strncpy` 到 `MAXFNAME=256` 的固定缓冲里。超过上限时不会报错，只会静默截断。
- 触发: 合法绝对路径长度超过 `255`。
- 影响: 用户看到的路径和实际读写目标不一致，可能把内容写进错误文件。
- 建议: 用动态字符串承接返回值，或者在超长时直接报错。

### AUD-20260326-004 [高] 预处理器 `#include` 失败消息仍然能被超长文件名打穿

- 位置: `src/tools/preprocessor.hpp:974-975,992-993`
- 问题: `sprintf(buf, "Cannot %cinclude %s", ppchar, name)` 把 `name` 写进固定 `static char buf[1024]`；`name` 来自源码里的 include 字符串，没有长度约束。
- 触发: 写一个超长 `#include "..."` 且目标不存在，或者 `fopen()` 失败。
- 影响: 构建阶段栈/静态缓冲区溢出，预处理工具崩溃。
- 建议: 改成 `snprintf` 或 `std::string` 组装错误消息。

### AUD-20260326-005 [高] 预处理器未识别指令报错仍会被整行源码撑爆

- 位置: `src/tools/preprocessor.hpp:1106-1108`
- 问题: `sprintf(buff, "Unrecognised %c directive : %s\n", ppchar, yyp)` 把整条非法指令写进 `char buff[200]`。
- 触发: 构造超长非法预处理指令，比如很长的 `#foobar...`。
- 影响: 构建工具栈缓冲区溢出。
- 建议: 限制输出长度，或者直接使用动态字符串。

### AUD-20260326-006 [高] `build_applies` 对 `fopen()` 结果不做判空，失败时直接空指针解引用

- 位置: `src/tools/build_applies.cc:25-27,33,50-52,75-77`
- 问题: 输入文件、头文件、表文件三个 `FILE *` 都没有判空；后面立即对 `out/table` 调 `fprintf()`，对 `f` 调 `fgets()`。
- 触发: `src_dir` 错误、目标目录不可写、磁盘满、权限不足。
- 影响: 构建工具直接崩溃，CI/本地生成流程拿不到可读错误。
- 建议: 对三个 `fopen()` 都先判空并返回非零退出码。

### AUD-20260326-007 [高] 编译器的 `#include` 路径拼装链路仍有多处固定缓冲区溢出点

- 位置: `src/compiler/internal/lex.cc:993-1043,1220,1313-1314`
- 问题:
  - `merge()` 先 `strcpy(dest, current_file)`，后续再 `strcat/strncat` 追加路径片段，没有容量检查
  - `inc_open()` 用 `sprintf(buf, "%s/%s", inc_path[i], name)` 拼路径
  - `handle_include()` 在失败分支再用 `sprintf(buf, "Cannot #include %s", name)` 复写同一个 `MAXLINE` 缓冲
- 触发: 当前文件路径、include 搜索目录或 include 名字过长。
- 影响: 编译器在处理 include 时可能直接写穿 `MAXLINE=4096` 缓冲，属于源码可触发的构建期崩溃。
- 建议: 统一改成有界/动态路径拼装，失败时显式报“路径过长”。

### AUD-20260326-008 [中] 编译器把 build/config 字符串加引号时仍使用无界 `strcat`

- 位置: `src/compiler/internal/lex.cc:3428-3434`
- 问题: `add_quoted_predefine()` 用 `char save_buf[MAXLINE]`，然后 `strcpy("\"") + strcat(val) + strcat("\"")`。`val` 来自 `CXXFLAGS`、`MUD_NAME` 等构建或配置字符串，没有长度检查。
- 触发: 超长 `__CXXFLAGS__`、超长 `MUD_NAME`，或其他长字符串预定义。
- 影响: 编译器初始化阶段发生固定缓冲区溢出。
- 建议: 改成 `std::string`，或者在拼接前严格校验剩余容量。

### AUD-20260326-009 [中] 调试日志打开失败时，`pending_messages` 会无上限增长

- 位置: `src/base/internal/log.cc:81-85,99-109,123-128`
- 问题: `reset_debug_message_fp()` 打开日志失败后会调用 `debug_message()` 输出错误；而 `debug_message_fp` 仍为空，`debug_message()` 会把消息继续塞进 `pending_messages`。之后只要日志一直不可用，队列就会一直增长。
- 触发: `__LOG_DIR__` 不可写、`__DEBUG_LOG_FILE__` 非法、磁盘满。
- 影响: 长时间运行时可能持续吃内存，最终拖垮进程。
- 建议: 日志不可用时不要无限缓存，至少要有容量上限或直接降级成只写 stdout/stderr。

### AUD-20260326-010 [高] `dump_trace_line()` 把动态字符串当格式串传给 `debug_message()`

- 位置: `src/vm/internal/trace.cc:26-46`
- 问题: 函数先把对象名、程序名、函数名、位置信息拼进 `line`，随后直接 `debug_message(line)`。`debug_message()` 是 `printf` 风格变参函数，不是“纯字符串输出”接口。
- 触发: 只要对象名、程序名或 trace 位置信息中出现 `%`，例如路径名/虚拟对象名包含 `%`。
- 影响: 轻则 trace 输出错乱，重则触发未定义行为甚至崩溃；格式串中的 `%n` 还会进一步放大风险。
- 建议: 改成 `debug_message("%s", line)`。

### AUD-20260326-011 [高] `get_dir()` 仍然会在长目录路径场景下截断或写穿路径缓冲

- 位置: `src/packages/core/file.cc:168-169,187-188,206-210,253,269`
- 问题:
  - `path` 先被截断复制到 `temppath`
  - 随后 `strcat(endtemp++, "/")` 和 `strcpy(endtemp, de->d_name)` 会继续在尾部追加
  - `check_valid_path()` 没有保证路径长度落在 `MAX_FNAME_SIZE + MAX_PATH_LEN + 1` 以内
- 触发: 超长目录路径，或接近上限的目录路径下再遇到较长子文件名。
- 影响: `get_dir()` 既可能静默列错目录，也可能直接写穿栈缓冲。
- 建议: 用动态字符串拼装 `temppath/regexppath`，不要再依赖固定路径数组。

### AUD-20260326-012 [中] `get_dir()` 对 `stat()` 失败完全不处理，会把旧的 `struct stat` 继续编码出去

- 位置: `src/packages/core/file.cc:269-272`
- 问题: `stat(temppath, &st); /* We assume it works. */` 失败时没有检查返回值，`encode_stat()` 仍然使用 `st`。
- 触发: 目录遍历期间文件被删除/替换，或目标路径因为截断拼错。
- 影响: 返回给 LPC 的文件元数据可能是上一个条目的旧值，行为不稳定且难排查。
- 建议: 检查 `stat()` 返回值，失败时跳过该项或显式回填错误信息。

### AUD-20260326-013 [中] 多个 CLI/生成工具对输出文件创建和写入失败没有任何检查，却仍然返回成功

- 位置:
  - `src/main_o2json.cc:68-70`
  - `src/main_json2o.cc:61-63`
  - `src/main_generate_keywords.cc:54-55`
  - `src/tools/make_options_defs.cc:37-55`
- 问题: 这些工具都直接构造 `std::ofstream` 并写文件，没有判断 `is_open()/good()/fail()`；`make_options_defs.cc` 甚至无论写没写成功都打印 `Generate Success!`。
- 触发: 输出目录不存在、文件不可写、磁盘满、只读挂载。
- 影响: 自动化流程会把“输出文件缺失/内容不完整”误判成成功，产生静默坏结果。
- 建议: 在打开后和最终写入后都检查流状态，失败时返回非零退出码。

### AUD-20260326-014 [中] `make_func` 的长度校验放在 `sprintf()` 之后，发生溢出时已经来不及了

- 位置: `src/tools/make_func.y:144-145,189-193`
- 问题: `func` 规则里先把多段文本 `sprintf()` 到 `char buff[500]`，然后才用 `strlen(buff) > sizeof buff` 检查“是否溢出”。如果真的过长，缓冲已经先被写坏。
- 触发: 规格文件里出现足够长的 efun 名、alias 名或组合类型描述。
- 影响: 代码生成工具可能在解析 spec 时直接破坏栈内存。
- 建议: 改成 `snprintf()` 并检查返回值；或先计算所需长度再动态分配。

### AUD-20260326-015 [中] `checkmemory` 会把超长 `default fail message` 直接拷进固定 8 KiB 缓冲

- 位置: `src/packages/develop/checkmemory.cc:680-684`
- 问题: `CONFIG_STR(__DEFAULT_FAIL_MESSAGE__)` 直接 `strcpy(buf, dfm)`，随后再 `strcat(buf, "\n")`，没有任何上限判断。
- 触发: 配置文件把 `default fail message` 设成超长字符串。
- 影响: 调试/检查内存路径下触发缓冲区溢出，排障工具本身变得不可靠。
- 建议: 改成动态字符串或至少使用有界拷贝。

### AUD-20260326-016 [中] `number_string()` 的 `add_commas` 判定逻辑写反了

- 位置: `testsuite/std/number_string.c:23`
- 问题: 现在的条件是 `if( !nullp(add_commas) || add_commas == 1 )`。只要第二个参数“存在”，即便传的是 `0`，也会进入加逗号分支。
- 触发: 调用 `number_string(1234, 0)`。
- 影响: API 语义和声明不一致，调用方无法显式关闭逗号格式化。
- 建议: 改成只在 `add_commas == 1` 时启用，或显式把缺省值和布尔语义分开处理。

### AUD-20260326-017 [低] LPC 标准库样例仍带未使用局部变量，持续污染编译告警

- 位置:
  - `testsuite/std/base64.c:12,16`
  - `testsuite/std/number_string.c:13`
- 问题:
  - `base64.c` 里的 `p`、`rlen`
  - `number_string.c` 里的 `parts`、`part`
  都没有被使用
- 触发: 相关对象被编译时就会出现 warning。
- 影响: 编译日志噪声增加，真正需要关注的 warning 更难被发现。
- 建议: 删除无用变量，保持 testsuite/std 示例代码干净。

## 修复建议顺序

1. 先收口所有可直接触发固定缓冲区溢出的运行时路径: `ed.cc`、`trace.cc`、`file.cc`
2. 再修编译器与工具链: `lex.cc`、`preprocessor.hpp`、`build_applies.cc`、`make_func.y`
3. 然后处理稳定性/可维护性问题: `log.cc`、`checkmemory.cc`、CLI 输出错误处理
4. 最后清理测试与标准库低优先级问题: `testsuite/std/*`

## 本轮验证备注

- 直接执行 `ctest` 时，测试二进制因缺少 `libgtest.dll` / `libgtest_main.dll` 而统一返回 `0xc0000135`
- 手动把 `C:\msys64\mingw64\bin` 加入 `PATH` 后，`build_codex_review_fix` 下 `46/46` 测试全部通过
- 因此本表记录的是“静态确认的当前问题”，不是“当前已有回归失败的功能清单”
