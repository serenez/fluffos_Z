# 修复记录：2026-03-28 当前 HEAD 审查问题收口

## 范围

本次只处理前面审查中已经确认、且当前 HEAD 仍然存在的真实问题：

1. `user_parser` 预收集 `sentence_t *` 后继续执行，存在悬空指针/错误分发风险
2. `PORT_TYPE_MUD` 输入缓冲在单次 read 内可能停滞，已进 `evbuffer` 的后续包要等下一次网络事件
3. 工具回归测试把目标路径、扩展名和输出目录结构写死，跨平台/多配置构建容易误报
4. `run_command_capture()` 通过 shell 字符串拼参数，路径含空格和特殊字符时容易拆参错误

---

## 已修复项

### 1. `user_parser` sentence 快照悬空指针风险

涉及文件：

- `src/packages/core/add_action.cc`
- `src/packages/core/add_action.h`
- `src/vm/internal/base/object.h`
- `src/vm/internal/simulate.cc`
- `src/tests/test_lpc.cc`

处理方式：

- 给 `sentence_t` 增加实例级 `serial`
- 分配 sentence 时生成新 serial，释放时清零
- `user_parser()` 不再直接信任预收集的 `sentence_t *`
- 在真正执行前，重新沿 `command_giver->sent` 按“指针 + serial”确认 sentence 仍然活着
- 若 sentence 已被前一个 action 删除，则跳过，不再继续解引用
- 补了定向测试：
  - 在 `collect_matching_sentences()` 之后、执行前，故意通过 hook 删除目标 owner 的 sentence
  - 期望只保留默认动作，不再误执行已删除的 exact 动作

---

### 2. `PORT_TYPE_MUD` 已缓冲数据停滞

涉及文件：

- `src/comm.cc`
- `src/tests/test_lpc.cc`

处理方式：

- 移除 `get_user_data_mud()` 里“`ip->text_end > 0` 就直接退出本轮循环”的早退
- 现在只要 `bufferevent` 输入缓冲里还有数据，就继续消费，直到：
  - 当前输入缓冲为空
  - 或者真的只剩一个不完整包，需要等待更多数据
- 补了定向测试：
  - 单次往 `evbuffer` 塞两个完整 MUD 包
  - 只调用一次 `get_user_data(ip)`
  - 期望两条输入都被处理完，`ip->text_end == 0`，输入缓冲长度为 `0`

---

### 3. 工具测试路径/扩展名假设过强

涉及文件：

- `src/tests/test_lpc.cc`

处理方式：

- `src_binary_path()` 和 `tool_binary_path()` 不再硬编码当前 Windows 单配置目录结构
- 按平台补扩展名：
  - Windows 自动补 `.exe`
  - 其他平台保持无扩展名
- 当 `lpc_tests` 位于 `Debug/Release/RelWithDebInfo/MinSizeRel` 这类多配置目录时，自动回退到更高层目录再搜索
- 在 `build/src...` 附近做受控递归搜索，优先返回更合理的候选目标
- 测试调用统一改成不写死 `.exe`

---

### 4. `run_command_capture()` shell 拼参不安全

涉及文件：

- `src/tests/test_lpc.cc`

处理方式：

- Windows：
  - 不再走 `cmd /c "<string>"` 拼命令
  - 改成 `CreateProcessW` + 明确参数引用
  - 用匿名管道直接抓 stdout/stderr
- POSIX：
  - 改成 `fork + execv`
  - 不再依赖 `popen("<string> 2>&1")`
- 这样参数中的空格、引号、括号、`&` 等字符不再被 shell 误拆

---

## 影响说明

本次修复只影响两类行为：

1. **运行时安全与协议处理**
   - `user_parser`
   - `PORT_TYPE_MUD`

2. **测试辅助稳定性**
   - 工具路径定位
   - 工具命令执行

没有改动：

- `mapping`/`shared-key`
- `heartbeat`
- `call_out`
- `parse_command`
- `packages/parser`

---

## 验证状态

本次已完成：

- 源码级 diff 复核
- `git diff --check`
- 为两条运行时问题补了定向 gtest
- `C:\msys64\mingw64\bin\cmake.exe --build build_codex_review_fix --target lpc_tests --parallel 4`
- `ctest --test-dir build_codex_review_fix --output-on-failure -j 1`

当前结果：

- 活跃测试 `90/90` 全通过
- 性能基准测试 `9` 条保持 disabled

补充说明：

- 之前出现的“构建/测试跑不起来”，根因是当前会话未继承 `C:\msys64\mingw64\bin`
- 在工具链 `PATH` 正常后，`lpc_tests` 可正常重链和运行
- `TestGetUserDataMudConsumesBufferedPacketsWithoutSecondReadEvent` 最后收口的是测试夹具本身：
  - `bufferevent_pair` 需要先 `enable`
  - win32 backend 下不能假设单次 `EVLOOP_NONBLOCK` 后输入缓冲必然已有数据
