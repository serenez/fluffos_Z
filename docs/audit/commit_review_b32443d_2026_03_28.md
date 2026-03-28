# 提交审查记录：`b32443d9540d25f579cf19d2372dc7910a6c951d`

## 审查范围

- 提交：`b32443d9540d25f579cf19d2372dc7910a6c951d`
- 标题：`fix(driver): 收口剩余审计问题并清零 2026-03-26 台账`

审查方式：

- 查看 `git show --stat --summary --name-only`
- 逐文件复核高风险源码 diff
- 重点检查新增测试、工具调用、运行时修复是否引入回归

---

## 结论总览

本提交的核心运行时修复整体看起来成立，当前确认到的主要问题集中在**新增测试辅助逻辑**，不是本次修复的核心 driver 代码本身。

当前确认 2 条有价值问题：

1. 新增工具回归测试把二进制路径和扩展名写死，存在明显的跨平台和多配置构建误报风险。
2. 新增 `run_command_capture()` 通过字符串拼接调用 shell，没有做参数引用和转义，路径中带空格时测试会误失败。

---

## 发现

### 🟠 [中] 新增工具回归测试把可执行文件名和目录布局写死成当前 Windows 单配置构建

涉及文件：

- [test_lpc.cc](/D:/xiangmu/fluffos_Z/src/tests/test_lpc.cc#L803)
- [test_lpc.cc](/D:/xiangmu/fluffos_Z/src/tests/test_lpc.cc#L807)
- [test_lpc.cc](/D:/xiangmu/fluffos_Z/src/tests/test_lpc.cc#L1187)
- [test_lpc.cc](/D:/xiangmu/fluffos_Z/src/tests/test_lpc.cc#L1202)
- [test_lpc.cc](/D:/xiangmu/fluffos_Z/src/tests/test_lpc.cc#L1233)
- [test_lpc.cc](/D:/xiangmu/fluffos_Z/src/tests/test_lpc.cc#L1248)
- [test_lpc.cc](/D:/xiangmu/fluffos_Z/src/tests/test_lpc.cc#L1260)
- [test_lpc.cc](/D:/xiangmu/fluffos_Z/src/tests/test_lpc.cc#L1287)
- [CMakeLists.txt](/D:/xiangmu/fluffos_Z/src/tests/CMakeLists.txt#L40)

问题本质：

- `src_binary_path()` 直接用 `g_test_binary_dir.parent_path()` 推导 `src` 目录
- `tool_binary_path()` 再拼 `tools`
- 用例里又把目标名全部写死成 `build_applies.exe`、`make_options_defs.exe`、`o2json.exe`、`json2o.exe`、`generate_keywords.exe`、`make_func.exe`

风险点：

- Linux/macOS 下这些工具通常没有 `.exe` 扩展名
- VS/Xcode 这类多配置构建下，`lpc_tests` 所在目录未必和工具目录只差一个 `parent_path()`
- 结果会变成“工具本身没问题，但测试辅助找错二进制”，从而误报失败

影响：

- 新增回归测试的可移植性明显不足
- 不同平台或不同 CMake generator 下，CI/本地测试可能无故变红

结论：

- 这是测试层的真实回归风险
- 需要改成由 CMake 显式传入目标路径，或至少按平台处理扩展名和多配置输出目录

---

### 🟠 [中] `run_command_capture()` 通过字符串拼接执行 shell 命令，没有对参数做引用和转义

涉及文件：

- [test_lpc.cc](/D:/xiangmu/fluffos_Z/src/tests/test_lpc.cc#L844)
- [test_lpc.cc](/D:/xiangmu/fluffos_Z/src/tests/test_lpc.cc#L848)
- [test_lpc.cc](/D:/xiangmu/fluffos_Z/src/tests/test_lpc.cc#L850)
- [test_lpc.cc](/D:/xiangmu/fluffos_Z/src/tests/test_lpc.cc#L858)
- [test_lpc.cc](/D:/xiangmu/fluffos_Z/src/tests/test_lpc.cc#L866)
- [test_lpc.cc](/D:/xiangmu/fluffos_Z/src/tests/test_lpc.cc#L1232)

问题本质：

- `run_command_capture()` 只给可执行文件本身加了引号
- 参数通过 `command += " " + arg` 直接拼接
- Windows 侧再交给 `cmd /c`
- 非 Windows 侧交给 `popen()`

风险点：

- 参数中只要含空格、引号、括号、`&` 等 shell 敏感字符，测试就可能拆参错误
- 当前用例已经在传绝对路径参数，例如 `TestO2JsonReportsOutputOpenFailure` 的 `test.o`
- 一旦工作目录或构建目录包含空格，这些测试就会失败，但失败原因是测试辅助，而不是被测工具

影响：

- 工具回归测试对环境路径过于敏感
- 本地目录名、构建目录名或 CI runner 路径变化，都可能触发伪失败

结论：

- 这是测试辅助实现不稳，不是工具程序本身的问题
- 需要改成真正的参数化进程启动，而不是拼 shell 字符串

---

## 最终判断

`b32443d` 这次提交里，当前最明确的问题不在 `log.cc`、`get_dir()`、`trace.cc`、`lex.cc` 这些核心修复点，而在新增的工具回归测试：

1. 路径和扩展名假设过强
2. shell 参数拼接不安全

这两条都会导致“回归测试不稳定、误报失败”，尤其是在非当前这套 Windows 单配置构建环境下。
