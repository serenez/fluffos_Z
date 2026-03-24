# FluffOS

这个分支在保留 FluffOS 现有 LPC 驱动兼容性的基础上，重点加入了
`gateway` 网关链路、多个新的 LPC 扩展包，以及更完整的编译诊断能力。

如果你想快速了解这个分支新增了什么，先看这份 README，而不是沿用上游那版
偏通用的介绍。

英文版说明见 [README_EN.md](README_EN.md)。

## 本分支新增功能

### 1. Gateway 网关包

新增 `gateway` 包，用来把驱动接到外部网关进程。

当前代码里已经接通的能力包括：

- 独立的 gateway 监听端口，不复用 telnet / websocket 端口
- 4 字节大端长度头 + JSON 的网关协议
- gateway 主连接管理和玩家会话管理
- 心跳、超时、状态查询
- gateway 用户输出自动回写到网关链路
- 面向 LPC 的监听、配置、广播、定向发送、会话查询、会话销毁等 efun

相关文档与示例：

- 协议与 LPC 对接说明：
  [docs/driver/gateway.md](docs/driver/gateway.md)
- 最小 LPC 示例：
  [testsuite/clone/gateway_login_example.c](testsuite/clone/gateway_login_example.c)

### 2. JSON 扩展包

新增 `json_extension` 包，直接给 LPC 提供 JSON 能力：

- `json_encode(mixed)`
- `json_decode(string)`
- `write_json(string, mixed)`
- `read_json(string)`
- `sort_mapping(mapping, int|void)`
- `sort_mapping_int(mapping, int|void)`

### 3. 实用工具包

这次还新增了两个 LPC 侧工具包：

- `myutil`
  - `uuid()`
  - `strbuf_new()`
  - `strbuf_add()`
  - `strbuf_addf()`
  - `strbuf_dump()`
  - `strbuf_clear()`
- `seed_random`
  - `seed_random()`
  - `seed_random_batch()`
  - `seed_next()`

### 4. 编译诊断增强

编译器和 mudlib 错误处理现在会暴露更完整的结构化编译诊断信息，包括：

- 文件
- 行号
- 列号
- 源码片段
- caret 定位

本次提交也补了多组回归测试，覆盖：

- 直接语法错误
- include / header 语法错误
- 参数类型报错定位
- locals 扩容与 scratchpad 行为
- heartbeat 生命周期
- inherit_list 上限
- UTF-8 截断与 eval deadline 路径

### 5. 运行时与平台改进

这次提交也顺带补强了一些底层能力，细节不在 README 展开，只列核心方向：

- interactive 增加 gateway 会话字段
- `exec()` / `remove_interactive()` 增加 gateway 映射联动
- 工具模式支持不依赖 backend 的离线初始化
- Windows 控制台 UTF-8 / UTF-16 输出改进
- heartbeat、call_out、eval deadline、inherit_list 的稳定性修正

## 快速构建

主要构建平台仍然是 Linux、macOS，以及 Windows 的 MSYS2 / MINGW64。

如果本地不需要 DB 包，可以直接这样构建：

```bash
cmake -S . -B build -G Ninja -DPACKAGE_DB=OFF
cmake --build build --target driver
```

本分支新增功能最相关的开关：

- `-DPACKAGE_GATEWAY=ON`
- `-DPACKAGE_JSON_EXTENSION=ON`
- `-DPACKAGE_MYUTIL=ON`

本次新增的 gateway 运行时配置项：

- `gateway port`
- `gateway external`
- `gateway debug`
- `gateway packet size`

## 代码入口

- gateway 核心实现：
  [`src/packages/gateway/`](src/packages/gateway)
- gateway efun 声明：
  [`src/packages/gateway/gateway.spec`](src/packages/gateway/gateway.spec)
- gateway 初始化入口：
  [`src/mainlib.cc`](src/mainlib.cc)
- JSON 扩展：
  [`src/packages/json_extension/`](src/packages/json_extension)
- 实用工具包：
  [`src/packages/myutil/`](src/packages/myutil),
  [`src/packages/seed_random/`](src/packages/seed_random)

## 相关说明

- 这次大提交的代码评审记录：
  [docs/driver/uncommitted_review_2026_03_24.md](docs/driver/uncommitted_review_2026_03_24.md)

## 支持

- 官网 / 文档：<https://www.fluffos.info>
- 论坛：<https://forum.fluffos.info>
- Discord：<https://discord.gg/E5ycwE8NCc>
- QQ 群：`451819151`
