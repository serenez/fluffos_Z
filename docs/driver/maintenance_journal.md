---
layout: doc
title: driver 提交记录
---
# Driver 提交记录

这份文档从 `115faae9` 开始，按时间顺序记录 driver 相关提交。

记录规则：

- 文档统一使用中文。
- 每条记录包含提交时间、提交号、标题、提交正文。
- 提交正文默认直接保留 git 提交信息，尽量不改写。
- 未提交内容不写进这份文档，等正式提交后再补记录。

## 提交记录

### 2026-03-26 00:04

**提交号**

`本次提交`

**标题**

`fix(driver): 修复长字符串边界并整理 driver 提交记录`

**提交信息**

```text
修复内容：
- 修复 call_other 类型检查报错路径对固定 1024 字节缓冲和 error(buf) 的依赖，超长函数名、对象名和程序名不再把错误路径打穿
- 修复 dwlib 的 replace_objects() 在对象名和 object_name() 返回值较长时的固定缓冲溢出，改为动态字符串拼接
- 修复 rename/copy_file 在目录目标路径下使用固定数组拼接新路径的问题，改为动态构造目标路径
- 修复 simul_efun 启动文件名处理对短字符串的越界读，并去掉追加 .c 时对固定缓冲的无界写入
- 修复 get_line_number() 使用固定 256 字节缓冲拼接长文件名的问题，改为线程局部动态字符串

文档整理：
- 将 docs/driver/maintenance_journal.md 重写为按时间顺序整理的提交记录文档
- 从 2026-03-24 23:36 的 115faae9 开始，按时间、标题、正文保留历史提交信息，便于直接查阅
- 将 driver 文档入口调整为中文标题，统一文档展示风格
- 同步更新代码审查记录，标记本轮已修复的问题与当前验证情况

测试与验证：
- 使用 MSYS2 MINGW64 重新编译 build_codex_review_fix 的 lpc_tests，编译通过
- 新增定向回归：
  - DriverTest.TestCallOtherTypeErrorHandlesLongFunctionName
  - DriverTest.TestGetLineNumberHandlesLongFilename
- DriverTest.TestAsciiChunkStopsAfterProcessInputDestructsObject 单独运行 10 次均通过
- ctest -R DriverTest.TestAsciiChunkStopsAfterProcessInputDestructsObject 单独执行通过

已知情况：
- ctest --output-on-failure -j 1 整套批量执行时，现有测试 DriverTest.TestAsciiChunkStopsAfterProcessInputDestructsObject 仍会在第 29 项位置出现 0xc0000374
- 同一测试单独执行稳定通过，说明这是批量执行场景下的遗留波动，本次提交先把已确认的边界修复和记录文档落下，后续继续追
```

### 2026-03-24 23:36

**提交号**

`115faae9855e022fb8ea9ed8e2fd628af99e1510`

**标题**

`feat(driver): 接入 gateway 包并补强编译诊断与运行时链路`

**提交信息**

```text
本次提交基于当前工作区全部未提交内容统一整理提交，覆盖构建配置、驱动运行时、编译器诊断、网络网关、新增工具包、测试补强与评审文档落盘。

一、构建与包系统
- 新增 PACKAGE_GATEWAY、PACKAGE_JSON_EXTENSION、PACKAGE_MYUTIL 开关，并补充 seed_random 包接入。
- 调整 build_packages_genfiles.sh 调用方式，显式通过 BASH_EXECUTABLE 执行脚本，修正 PACKAGES_SPEC_FULL 清理变量名错误。
- 在 config.h 中写入实际编译器标志，修正 CXXFLAGS 与版本信息输出。
- 新增 gateway 运行时配置项：gateway port、gateway external、gateway debug、gateway packet size。
- 更新 applies 表，加入 GATEWAY_LOGON 与 GATEWAY_CONNECT apply，重新生成 applies_table.autogen.*。

二、Gateway、JSON 与实用包扩展
- 新增 gateway 包，接入监听、主连接管理、会话创建/销毁、心跳检测、发送接口、interactive 映射联动等能力。
- comm.cc 增加 PORT_TYPE_GATEWAY 发送分支，并在 remove_interactive / exec 过程中补充 gateway 会话清理与映射更新。
- interactive_t 新增 gateway_session_id、gateway_real_ip、gateway_real_port、gateway_master_fd 等字段，并增加 GATEWAY_SESSION / GATEWAY_MSGPACK 标志位。
- 新增 json_extension 包，提供 json_encode/json_decode/read_json/write_json 以及 mapping 排序辅助能力，并为 gateway 输出封包提供 JSON 构建接口。
- 新增 myutil 包，提供 uuid 与字符串缓冲区工具；新增 seed_random 包，提供基于 xoroshiro128++ 的种子随机数接口。
- mainlib 初始化流程补充 gateway/myutil 初始化入口。

三、驱动运行时与平台兼容改进
- init_main 支持 require_backend=false 的离线初始化模式，LPCC / symbol 工具启动时不再强依赖事件后端。
- backend 初始化增加失败诊断日志，DNS 初始化在无 event base 时安全降级。
- Windows 控制台切换为 UTF-8 输入输出，debug_message 在控制台优先走 UTF-16 写出，减少乱码。
- websocket 关闭路径改为更稳妥的 interactive / evbuffer 清理顺序。
- telnet_ext、external、socket_efuns 等处修正 Windows/格式化/分支穿透细节问题。
- add_action 在 f_command 中补充 update_load_av 统计调用。

四、编译器诊断与语义分析补强
- 编译器新增 compile_diagnostic_snapshot 缓存，支持记录 object/file/line/column/message/source/caret 等结构化诊断信息。
- 语法与语义错误改为尽量对齐具体符号位置，新增 diagnostic anchor / override 机制，用于局部变量声明、函数声明、参数类型校验等错误定位。
- mudlib_error_handler 现在会把 compile_error_* 字段注入错误 mapping，便于 mudlib 获取更完整的编译失败上下文。
- 改进 include/header 语法错误、直接语法错误、efun/函数参数类型错误的定位质量。
- 修复 scratchpad 大块分配/重分配实现细节，移除依赖 block[2] 头布局的旧写法。
- 调整 locals 扩容逻辑，同时扩展 locals 与 type_of_locals 两组存储，避免大小不同步。
- 引入 error placeholder 节点判定，避免错误节点继续参与 pop_value 链路。

五、核心运行时逻辑修正
- call_out handle 分配改为显式寻找可用句柄，移除依赖递增 unique 与旧句柄比较的逻辑，避免长生命周期 call_out 句柄失效。
- heartbeat 改为 heartbeats + heartbeats_pending + interval map 组合管理，补齐当前执行对象、动态增删与去重检查逻辑。
- eval_limit 在非 Linux 平台增加基于 steady_clock 的 deadline 回退实现，并补充 check_eval 接口。
- inherit_list / deep_inherit_list 由固定 256 项缓存改为 vector，修复大量继承链场景下的截断问题。
- ed 缩进栈改为静态存储区，避免函数内局部大数组反复构造。
- maximum local variables 运行时配置下限从 64 放宽到 1。

六、协议与输入链路调整
- 为 MUD 包长度解析增加 decode_mud_packet_size 校验函数，统一处理网络字节序与非法长度情况。
- interactive 输入缓冲上限由 1 MiB 调整为 64 KiB，并在读包阶段提前拒绝非法或越界长度。
- Gateway 用户输出改为通过 gateway_send_to_session 回写到网关链路。

七、测试与回归补强
- src/tests/test_lpc.cc 新增编译诊断、include/header 定位、参数类型高亮、heartbeat 生命周期、locals 扩容、scratchpad 拼接、继承列表上限、UTF-8 截断、eval deadline 等多组单元测试。
- 新增 testsuite/include/line_error_bad_header.h、testsuite/single/tests/compiler/fail/line_error_include.c、line_error_direct.c 等编译失败夹具。
- testsuite/command/more.c 调整背景色宏命名，避免缩写冲突。

八、文档与仓库整理
- 新增 docs/driver/uncommitted_review_2026_03_24.md，记录本轮未提交代码评审发现与后续关注项。
- 更新 docs/.vitepress/sidebar.ts，为专项评审记录补充入口。
- 删除 QWEN.md 与 TODO.md。

九、核验记录
- 使用 MSYS2 / MINGW64 重新全新配置构建目录（PACKAGE_DB=OFF）。
- 成功构建目标：lpc_tests。
- 成功执行测试：src/tests/lpc_tests.exe，17/17 通过。
```

### 2026-03-25 00:09

**提交号**

`a07b64468fb3118f2f473ceb513107eb5b79dca8`

**标题**

`docs(gateway): 重写中文 README 并补充网关接入文档`

**提交信息**

```text
本次提交集中整理当前工作区中的文档与示例文件，目标是把本分支新增功能，尤其是 gateway 包，对外说明补齐，并将仓库主入口调整为中文优先。

一、README 入口调整
- README.md 改为中文主入口，突出当前分支新增能力，不再沿用偏上游通用介绍的结构。
- 原 README_CN.md 调整为英文版并改名为 README_EN.md，形成“中文主入口 + 英文副本”的仓库说明结构。
- 主 README 重点展示 gateway、json_extension、myutil、seed_random、编译诊断增强等新增功能。
- 保留简化后的构建说明、关键编译开关与主要代码入口，方便中文项目直接落地使用。

二、Gateway 协议与对接文档
- 新增 docs/driver/gateway.md，基于 gateway.cc、gateway_session.cc、comm.cc、interactive.cc、mainlib.cc 等真实实现整理协议与接入说明。
- 明确网关链路采用“4 字节大端长度头 + JSON payload”的封包格式，而不是裸 JSON。
- 明确驱动当前只识别 hello、login、data、discon、sys 五类入站消息。
- 说明 login 的实际 LPC 调用链是 master->connect() 返回登录对象，再调用 gateway_logon(data)，并明确当前实现没有走 gateway_connect() 主路径。
- 说明 data 与带 cid 的部分 sys 消息最终都会调用 LPC 侧 gateway_receive(mixed data)，不会自动注入旧命令缓冲。
- 说明普通 write/tell_object 输出会被驱动封装成 output 消息回写到 gateway 链路。
- 说明 gateway_send() 与 gateway_session_send() 的语义区别，以及 gateway_d 守护进程对 receive_system_message(mixed msg) 的可选支持。
- 补齐 LPC 侧接入要求，明确 master 文件、LOGIN_OB 返回对象、gateway_logon/gateway_receive/gateway_disconnected/net_dead 等函数职责。
- 特别记录 testsuite 现有 /clone/login 只有 logon()，不能直接作为 gateway 登录对象使用，避免 mudlib 接错入口。

三、文档导航补齐
- docs/driver/index.md 增加 gateway 页面入口。
- docs/.vitepress/sidebar.ts 在 Driver Internal 分组下增加 Gateway 链接，便于从文档站点直接访问。

四、最小 LPC 示例
- 新增 testsuite/clone/gateway_login_example.c，提供一个不改动现有 testsuite 登录流的最小 gateway 接入示例。
- 示例覆盖 gateway_logon()、gateway_receive()、gateway_disconnected()、net_dead() 等核心入口。
- 示例展示字符串 payload、结构化 payload、gateway_session_send() 回包，以及命令式处理应由 LPC 显式决定的边界。

五、核验记录
- 已执行 git diff --check，当前暂存内容没有格式错误。
- 已检查 git diff --cached --stat，确认提交范围覆盖 README、driver 文档、侧边栏入口与 LPC 示例。
- 尝试执行 docs 目录下的 npm run build，但当前环境缺少 vitepress，可见错误为 "vitepress is not recognized as an internal or external command"；因此本次未完成文档站点构建验证。
```

### 2026-03-25 02:12

**提交号**

`aaf3a495c72233e65fd0fa9471ba63e67aa2c90f`

**标题**

`fix(driver): 修复动态交互缓冲回归并补强网关链路`

**提交信息**

```text
- 将 interactive 输入缓冲从固定数组重构为动态分配，初始容量改为 4 KiB，逻辑上限恢复为 1 MiB，避免每个 interactive 固定预留整块大缓冲
- 重写 comm.cc 的读入与缓冲管理逻辑，统一按 text_capacity 扩容与压缩，覆盖 telnet、ascii、websocket 和 MUD 四条输入链路
- 修复 MUD 长包在 64 KiB 分块读取下的整包判定问题，只有累计收到 4 字节长度头和完整 payload 后才执行 restore_svalue 与 process_input
- 修复普通 TCP/Telnet 最后一条换行命令在 clean_buf 中被提前清空的问题，保持 first_cmd_in_buf 返回的缓冲内容在 process_input 前仍然有效
- 修复 gateway_session_send 错误路径 double free，并补齐 gateway 会话创建/销毁失败路径对动态 text 缓冲的释放
- 保持 gateway max_packet_size 独立于 MAX_TEXT，全局共享缓冲恢复为 1 MiB 后，gateway 仍按自身配置做协议包上限控制
- 为 websocket 建连流程补充 new_user 失败保护，避免 interactive 分配失败时继续解引用空指针
- 修正内存统计与 checkmemory 逻辑，将动态 text 缓冲纳入 Interactives 统计与 TAG_INTERACTIVE 标记，避免 memory_summary 和检查工具失真
- 补充 LPC/驱动回归测试，覆盖 compile_error_* mudlib 契约、gateway 会话生命周期、动态缓冲增长、MUD 包完成判定，以及最后一条换行命令不被提前清空的回归场景
- 新增 testsuite/clone/input_capture_user.c 作为最小交互对象夹具，配合 testsuite/single/master.c 支撑驱动侧输入回归验证
- 更新 docs/driver/uncommitted_review_2026_03_24.md，回写本轮复审后的真实结论与已修复项
- 同步纳入当前工作区里的 grammar autogen 产物调整与 .gitignore 变更，保持提交内容与现状一致

验证：
- cmake --build build_codex_gateway_fix --target lpc_tests -j 4
- .\\build_codex_gateway_fix\\src\\tests\\lpc_tests.exe（28/28 passed）
```

### 2026-03-25 02:37

**提交号**

`ed4a8a9db0e291bc0eec69ffb09d80584c4961bd`

**标题**

`fix(driver): 修复 ASCII 端口边界解析并补齐分配失败防护`

**提交信息**

```text
修复普通 ASCII 交互口在动态缓冲改造后暴露出的两类边界问题。
一是空行输入时对 `\r` 的回看存在越界风险；二是同一次 read 中出现
`cmd1\r\ncmd2\n` 时，第二条命令可能因为游标推进错误而滞留在缓冲中。
本次将 ASCII 分支的行切分逻辑收口为统一 helper，按累计缓冲解析并保留
现有 `process_input` 调用语义。

同时补齐低内存和分配失败路径的回滚保护。
`new_user()` 在 `evtimer_new()` 失败时现在会完整释放 interactive 与动态输入缓冲；
`ws_ascii` 与 `ws_telnet` 在 `evbuffer_new()` 失败时会立即撤销 interactive 并中止建连，
避免后续继续解引用空缓冲。

测试方面，扩展了 `testsuite/clone/input_capture_user.c` 夹具，增加输入历史与清理接口，
并在 `src/tests/test_lpc.cc` 中补上两条 ASCII 回归测试：
- 空行 `"\n"` 能安全进入 `process_input("")`
- 同批 `"look\r\nsay\n"` 会连续产出两条命令，不再卡住第二条

验证：
- `cmake --build build_codex_gateway_fix --target lpc_tests -j 4`
- `.\\build_codex_gateway_fix\\src\\tests\\lpc_tests.exe`
- 结果：`30/30 passed`
```

### 2026-03-25 03:13

**提交号**

`95eab12efa6d820ecd5d3586755d350f92fb7695`

**标题**

`fix(driver): 收口预登录竞态并补齐交互失败路径防护`

**提交信息**

```text
这次继续修复最近两轮输入缓冲与连接链路改造后剩余的高风险遗留问题，
重点收口普通 TCP / websocket / TLS / 预登录阶段的资源生命周期与竞态边界。

核心修复包括：
- 修复 ASCII 直读直处理分支在 `process_input()` 里自毁对象后仍继续访问
  原 `interactive_t` 的 use-after-free 风险；现在调用 LPC 后会先验证对象和
  interactive 绑定关系，再决定是否继续处理后续命令。
- 重构普通连接与 websocket 建连后的延迟登录调度，不再把裸 `interactive_t *`
  直接交给 `event_base_once(..., EV_TIMEOUT, ...)`；新增 `ev_logon` 事件句柄并
  挂到 `interactive_t` 上，断线或失败清理时可显式取消，避免登录回调在对象已
  释放后再次触发。
- 新增 `cleanup_pending_user()` 统一清理预登录阶段的临时 interactive，覆盖
  `ev_logon`、`ev_buffer`、`ev_command`、动态文本缓冲、telnet、TLS、编码器、
  gateway 字段等资源，解决此前 `remove_interactive(master_ob, 0)` 无法回收
  预登录用户的问题。
- 修复 `new_user()` 在 TLS 端口上 `SSL_new()` 失败时误退化为非 TLS 会话的风险；
  现在拿不到 `SSL *` 会直接失败返回。
- 修复 `new_user()` 在 `evtimer_new()` 失败时遗漏释放 `SSL` 的泄漏问题。
- 修复 `new_user_event_listener()` 未检查 `bufferevent_*_new()` / 初始化失败的
  空指针使用问题；失败时会完整回滚而不是继续进入监听链路。
- websocket 的 `ws_ascii` / `ws_telnet` 建连失败路径统一接入新的预登录清理逻辑，
  同时保留现有 `evbuffer_new()` 失败保护。

测试方面：
- 扩展 `testsuite/clone/input_capture_user.c`，增加“收到输入后立即 destruct”开关，
  用于构造最容易触发 use-after-free 的 LPC 场景。
- 在 `src/tests/test_lpc.cc` 新增回归测试：
  - `TestAsciiChunkStopsAfterProcessInputDestructsObject`
    验证 `process_input()` 自毁对象后，ASCII 分支不会继续访问旧 interactive。
  - `TestScheduledUserLogonCanBeCancelledBeforeDispatch`
    验证预登录 logon 事件在清理后可以取消，不会在下一轮事件循环里再次触发。
- 现有 ASCII 边界、动态缓冲、gateway 生命周期和编译诊断测试继续保留。

验证：
- `git diff --check`
- `cmake --build build_codex_gateway_fix --target lpc_tests -j 4`
- `.\\build_codex_gateway_fix\\src\\tests\\lpc_tests.exe`
- 结果：`32/32 passed`
```

### 2026-03-25 19:44

**提交号**

`5495a02c3ea7a0281492fc40f803d1ec38d9d647`

**标题**

`fix(driver): 修复 gateway 会话生命周期与输入竞态`

**提交信息**

```text
修复内容：
- 为 gateway 会话创建和销毁补充 active owner 解析，避免 gateway_logon() 与
  gateway_disconnected() 中 exec 或 destruct 之后继续使用旧对象
- 在 exec 映射更新中同步 user_ob_load_time，修复 session 查询在对象迁移后失效
- 在 master 读回调和广播路径中先快照 fd 再重查 master，避免 remove_master()
  导致的 UAF 与 unordered_map 迭代器失效
- 为 pending logon 连接阻断读事件并恢复缓冲输入，修复登录前命令提前执行、
  websocket 关闭路径绕过清理，以及 connect() 失败时的清理缺口
- 为 process_input() 增加对象保活，修复 process_input 中 exec 后命令继续调度
  触发的引用计数 fatal
- 为 telnet/tls 初始化失败补充判空和资源释放，避免空指针继续执行

测试：
- 新增 gateway exec 登录、裸 CR、exec 后连续命令与命令调度测试用例
- 使用 MSYS2 MINGW64 重新编译 lpc_tests，并通过 ctest 36/36
```

### 2026-03-25 20:00

**提交号**

`a2bccf2f6f8d37f0b3b94a7b37b838e6080f5758`

**标题**

`fix(lpcc): 调整 tracing 开关为显式启用`

**提交信息**

```text
变更内容：
- 移除 lpcc 启动时默认执行的 Tracer::start("trace_lpcc.json")
- 新增 --tracing <trace.json> 参数，只有显式传参时才开启 tracing 并写出文件
- 调整 lpcc 参数解析，避免 --tracing 的输出文件名被误识别为 config_file 或 lpc_file
- 更新 Usage 文案为 lpcc [--tracing trace.json] config_file lpc_file
- 对未知开关和 --tracing 缺参场景增加直接报错返回

行为变化：
- 默认执行 lpcc 不再在当前目录生成 trace_lpcc.json
- 需要抓取 tracing 时，必须显式指定输出文件路径
- tracing 输出文件名不再固定，调用方可按需指定位置

验证记录：
- 使用 MSYS2 MINGW64 重新编译 lpcc 目标
- 在 testsuite 目录执行 ../build_codex_review_fix/src/lpcc.exe etc/config.test /single/master
  确认退出码为 0，且默认不生成 trace_lpcc.json
- 在 testsuite 目录执行 ../build_codex_review_fix/src/lpcc.exe --tracing D:/xiangmu/fluffos_Z/build_codex_review_fix/lpcc_trace_test.json etc/config.test /single/master
  确认退出码为 0，且显式指定的 tracing 文件成功生成
```

### 2026-03-25 22:17

**提交号**

`3b53156792a7f612280b9a2fe5d5617cf9839f30`

**标题**

`fix(driver): 修复 lpcc tracing 落盘与语法生成漂移`

**提交信息**

```text
变更内容：
- 为 lpcc 的 tracing 增加统一退场回收，避免仅成功路径调用 Tracer::collect()
- 修复 lpcc 在 --tracing 显式开启后，编译失败或参数错误等早退场景不生成 trace 文件的问题
- 调整 Bison 生成参数，改用相对输出名并启用 --no-lines，避免生成结果嵌入本机绝对路径
- 刷新仓库内预生成的 grammar.autogen.cc / grammar.autogen.h，移除 #line 路径噪音与路径相关 include guard

行为变化：
- lpcc 默认仍然不生成 trace 文件
- lpcc 显式传入 --tracing 时，成功和失败路径都会写出 trace
- 语法预生成文件在不同机器上重新生成时，diff 将明显收敛，不再携带本机路径信息

验证记录：
- 使用 MSYS2 MINGW64 重新配置并编译 lpcc 目标
- 验证默认运行 EXIT:0，且 testsuite/trace_lpcc.json 不生成
- 验证显式 tracing 且成功时 EXIT:0，trace 文件成功生成
- 验证显式 tracing 且失败时 EXIT:1，trace 文件仍然成功生成
- 运行 ctest --output-on-failure -j 1，结果 36/36 全部通过
```

### 2026-03-25 23:05

**提交号**

`dbe3f9ae94a93263725fd0da371a71895b126d41`

**标题**

`fix(driver): 修复异步 IO、DNS 与 DB 并发安全问题`

**提交信息**

```text
修复内容：
- 修正 async getdir 使用错误元素大小扩容的问题，改为保存目录项字符串，消除目录扫描时的堆越界写
- 为 async 普通读写和 gzip 读写补齐 open、read、gzopen、gzdopen 失败处理，避免空指针调用和 resize(-1) 这类崩溃路径
- 为 db_close、db_commit、db_fetch、db_rollback、db_status、db_cleanup 补上与 async_db_exec 一致的互斥保护，消除关闭连接与后台执行并发时的 UAF 风险
- 为 socket 地址解析增加 host/service 长度检查，阻断超长 "host port" 输入导致的栈溢出
- 将 mudlib_stats 的 author/domain 缓冲改为可扩展存储，避免 master 返回长字符串时写坏固定静态缓冲区
- 在 DNS 解析基础设施为空或请求提交失败时走统一失败回调和清理流程，避免空指针调用与回调资源泄漏
- 增加测试辅助接口和 DNS 回调 helper，补充超长地址、DNS 失败回调、长 author/domain 字符串等回归覆盖
- 调整 exec 相关测试清理逻辑，避免测试自身对迁移对象重复销毁导致的假失败

验证记录：
- 使用 MSYS2 MINGW64 编译 build_codex_review_fix 的 lpc_tests，编译通过
- 在 build_codex_review_fix 下执行 ctest --output-on-failure -j 1，结果 39/39 全通过
- 额外配置启用 SQLite 的 build_codex_db_check，确认 db 包与 lpc_tests 编译通过
```

### 2026-03-25 23:27

**提交号**

`67331e794ce8308bdb991edc1dbc2f47554127d1`

**标题**

`fix(driver): 修复审计台账中的输入边界与保存路径问题`

**提交信息**

```text
修复内容：
- 修复 save_object 对对象名使用固定 256 字节缓冲的问题，改为动态构造保存头部，避免超长对象名覆盖静态区
- 修复 save_object 对短文件名直接访问 file[len-2] 的越界读，补齐长度判断并兼容单字符保存路径
- 重写 parser 的 parse_recurse 拼接逻辑，去掉固定 1024 字节栈缓冲，避免长句输入触发栈溢出
- 修复 add_action 错误路径中间 buf 的固定缓冲与 error(buf) 二次格式化风险，改为直接格式化报错
- 将 mudlib_stats 的 restore_stat_list 改为基于 ifstream 和动态字符串解析，消除 fscanf("%s") 对固定缓冲的溢出风险
- 更新全量代码审计台账，记录本轮问题状态、修复结果与验证情况

测试：
- 新增 parser 超长句、add_action 长动词与格式符、save_object 单字符路径回归
- 新增 mudlib_stats 长名称保存/恢复回归
- 使用 MSYS2 MINGW64 重新编译 build_codex_review_fix 的 lpc_tests
- 在 build_codex_review_fix 下执行 ctest --output-on-failure -j 1，结果 40/40 全通过
```

### 2026-03-25 23:29

**提交号**

`a350d16c1648ddcca6dd2a3d4bb28563988a5310`

**标题**

`docs(driver): 新增维护日志并接入文档入口`

**提交信息**

```text
变更内容：
- 新增 docs/driver/maintenance_journal.md，作为驱动侧统一维护日志
- 记录最近多次提交从 aaf3a495 到 67331e79 的变更范围、核心改动与验证结果
- 在文档顶部明确约定：后续所有非琐碎代码更新都要同步追加到该文件
- 更新 docs/driver/index.md，增加 maintenance_journal 入口，便于从 driver 文档页直接查看最近维护历史

目的：
- 让近期修改历史不再依赖 git log 才能理解
- 让代码修复、验证结果、审计台账之间形成稳定可追溯链路
- 为后续继续按审计台账推进提供统一记录位置
```
