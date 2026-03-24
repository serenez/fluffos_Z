---
layout: doc
title: driver / gateway
---

# Gateway Package

这份说明只描述当前仓库代码里已经落地的 `gateway` 行为，依据的实现主要在：

- `src/packages/gateway/gateway.cc`
- `src/packages/gateway/gateway_session.cc`
- `src/comm.cc`
- `src/packages/core/interactive.cc`
- `src/mainlib.cc`

如果你要接 Go 网关或者其他前端进程，先看这里。

## 1. 它现在能做什么

`gateway` 包提供了一条独立于 telnet / websocket 的接入链路：

- 驱动监听一个单独的 gateway 端口
- 外部 gateway 进程连上来后，按固定 JSON 协议发送 `hello/login/data/discon/sys`
- 驱动为每个 `cid` 创建一个虚拟 interactive 会话
- LPC 侧通过 `gateway_logon()`、`gateway_receive()`、`gateway_disconnected()`
  处理业务
- 对 gateway 用户执行 `write()` / `tell_object()` / 普通输出时，驱动会自动把
  文本封成 gateway 输出包发回外部 gateway

## 2. 启动方式

### 编译开关

和网关相关的构建开关在 `src/CMakeLists.txt`：

- `PACKAGE_GATEWAY`
- `PACKAGE_JSON_EXTENSION`
- `PACKAGE_MYUTIL`

其中 `gateway` 代码会直接链接 `package_json_extension`。

### 运行时配置

驱动启动时，`mainlib.cc` 会在 `PACKAGE_GATEWAY` 打开时调用 `init_gateway()`。

当前新增的配置项在 `src/base/internal/rc.cc` 和
`src/include/runtime_config.h`：

- `gateway port`
- `gateway external`
- `gateway debug`
- `gateway packet size`

行为如下：

- `gateway port > 0` 时，驱动启动后自动监听 gateway 端口
- `gateway external = 0` 时监听 `127.0.0.1`
- `gateway external != 0` 时监听 `0.0.0.0`
- `gateway packet size` 用作接收包大小上限

也可以在 LPC 里直接调用：

```c
gateway_listen(port, external);
```

## 3. 线协议

### 封包格式

gateway 链路不是裸 JSON。

每个包都必须是：

```text
[4-byte big-endian length][JSON payload]
```

其中：

- 长度头是 4 字节大端整数
- 长度值只计算后面的 JSON payload
- 驱动收到后会先读长度，再按这个长度取完整 JSON

如果长度为 `0`，或者大于当前 `max_packet_size`，驱动会拒绝该包。

### 入站消息类型

驱动当前只识别下面 5 种 `type`：

- `hello`
- `login`
- `data`
- `discon`
- `sys`

其它类型会被忽略。

## 4. 外部 Gateway -> 驱动

### `hello`

示例：

```json
{"type":"hello","data":"gateway-main"}
```

当前行为很简单：

- `data` 如果是字符串，会被当成服务名记录日志
- 更新该主连接的心跳时间
- 不触发 LPC 回调

### `login`

示例：

```json
{"type":"login","cid":"u-1001","data":{"ip":"127.0.0.1","port":50000}}
```

要求与行为：

- `cid` 必须是字符串
- `data` 可以是对象，驱动会把它转换成 LPC `mixed`
- 如果 `data.ip` / `data.port` 存在，会写入 interactive 的
  `gateway_real_ip` / `gateway_real_port`
- 驱动会创建 gateway session，然后进入 LPC 登录链路

当前真实 LPC 调用链是：

```text
master->connect()
  -> return login object
  -> login_object->gateway_logon(data)
```

注意：

- 当前代码路径复用的是 `connect()`，不是 `gateway_connect()`
- `applies` 里虽然新增了 `GATEWAY_CONNECT`，但当前会话创建实现没有调用它

### `data`

示例：

```json
{"type":"data","cid":"u-1001","data":"look"}
```

或者：

```json
{"type":"data","cid":"u-1001","data":{"type":"cmd","data":"look"}}
```

或者：

```json
{"type":"data","cid":"u-1001","data":{"type":"cmd","data":{"text":"look"}}}
```

当前行为：

- `cid` 必须是字符串
- 驱动按 `cid` 找到对应 LPC 对象
- 然后调用：

```c
user->gateway_receive(mixed data)
```

这里有个必须说明的边界：

- 驱动当前不会自动把 `data` 注入旧的命令输入链路
- 驱动真实做的是调用 LPC 的 `gateway_receive(mixed data)`
- 你要不要把字符串当成命令执行，要由 LPC 自己决定

入参规则：

- 如果 `data` 是字符串，LPC 收到的就是字符串
- 如果 `data` 是 `{ "type": "cmd", "data": "xxx" }`，LPC 收到的也是字符串
- 如果 `data` 是 `{ "type": "cmd", "data": { "text": "xxx" } }`，LPC 收到的也是字符串
- 其它 JSON 对象 / 数组 / 数字 / 布尔，会按 JSON 转成 LPC `mixed`

### `discon`

示例：

```json
{"type":"discon","cid":"u-1001","reason_code":"kick","reason_text":"admin kick"}
```

当前行为：

- 根据 `cid` 销毁会话
- 如果 LPC 对象仍然存在，先调用：

```c
user->gateway_disconnected(reason_code, reason_text)
```

- 然后继续走 `remove_interactive()`，因此普通的 `net_dead()` 仍然会被触发

### `sys`

示例一，ping/pong：

```json
{"type":"sys","action":"ping"}
```

当前行为：

- `action == "ping"` 时，驱动立即回复：

```json
{"type":"sys","action":"pong"}
```

- `action == "pong"` 时，只更新心跳

示例二，带 `cid` 的系统消息：

```json
{"type":"sys","action":"notify","cid":"u-1001","data":{"kind":"mail"}}
```

当前行为：

- 如果带 `cid`，而且会话存在，驱动和 `data` 包一样，转给
  `gateway_receive(mixed data)`

示例三，不带 `cid` 的系统消息：

```json
{"type":"sys","action":"broadcast","data":{"kind":"reload"}}
```

当前行为：

- 驱动会尝试查找 `/adm/daemons/gateway_d`
- 如果这个对象存在，就调用：

```c
/adm/daemons/gateway_d->receive_system_message(mixed msg)
```

- 如果对象不存在，这类消息会被忽略

## 5. 驱动 -> 外部 Gateway

### 普通文本输出

只要对象是 gateway 用户，普通输出会走 gateway 链路。

也就是说：

- `write()`
- `tell_object()`
- 普通命令输出

最终都会被封成：

```json
{"type":"output","cid":"SESSION_ID","data":"...text..."}
```

然后再加 4 字节长度头发回 gateway。

### `gateway_session_send(object user, mixed data)`

这是按单个会话发送。

当前代码里：

- 如果 `data` 是 `mapping`
  - 驱动直接把这个 mapping 转成 JSON
  - 再补一个 `"cid"`
  - 其它字段保持 LPC 原样
- 如果 `data` 不是 `mapping`
  - 驱动会包装成：

```json
{"type":"output","cid":"SESSION_ID","data":<serialized-value>}
```

注意：

- `gateway.spec` 里还保留了一个可选第三参数位
- 当前实现实际上只使用前两个参数
- `gateway.spec` 注释里提到的 RPC 回调并没有在当前代码里实现

### `gateway_send(mapping data, mixed users = 0)`

这是按 gateway 主连接发送。

当前行为：

- 第一个参数必须是 `mapping`
- 不传 `users` 时，会把 mapping 原样广播给所有 gateway 主连接
- 传单个对象或对象数组时，会先按主连接分组
- 然后为每个主连接补一个 `"cids": ({ ... })` 对应的 JSON 数组
- 最后把这份 mapping 发给对应主连接

这意味着：

- `gateway_send()` 的目标是 gateway 主连接
- 它不是简单的“对单个 session 发包”
- 单会话发包优先用 `gateway_session_send()`

## 6. LPC 侧到底要改哪里

### 必需文件一：master 文件

master 需要继续提供 `connect()`。

当前 testsuite 的例子是：

- `testsuite/single/master.c`

它的做法是：

- 在 `connect()` 里 `new(LOGIN_OB)`
- 返回登录对象

这条链在 gateway 会话里仍然有效。

### 必需文件二：`connect()` 返回的登录对象

当前 testsuite 的 `LOGIN_OB` 定义在：

- `testsuite/include/globals.h`

默认是：

```c
#define LOGIN_OB "/clone/login"
```

而当前仓库里的 `testsuite/clone/login.c` 只实现了普通连接使用的 `logon()`，
没有实现任何 `gateway_*` 函数，所以它不能直接作为 gateway 登录对象使用。

如果你要让 gateway 登录链路工作，`connect()` 返回的那个对象至少要补这些函数：

```c
void gateway_logon(mixed data);
void gateway_receive(mixed data);
```

推荐同时补上：

```c
void gateway_disconnected(string reason_code, string reason_text);
void net_dead();
```

实际含义：

- `gateway_logon(mixed data)`
  - gateway 登录建立后立即调用
  - `data` 就是外部 gateway 发来的 `login.data`
  - 当前实现要求这个函数存在并正常返回，否则会话创建会失败
- `gateway_receive(mixed data)`
  - 接收 `data` 消息
  - 接收带 `cid` 的部分 `sys` 消息
  - 如果你想把字符串当命令执行，需要在这里自己处理
- `gateway_disconnected(string, string)`
  - 外部 gateway 主动断开 / 踢线 / 超时时的显式回调
- `net_dead()`
  - `remove_interactive()` 后仍然会走到
  - 适合做兼容旧逻辑的收尾

### 可选文件：系统消息守护进程

如果你要处理不带 `cid` 的 `sys` 消息，可以增加：

- `/adm/daemons/gateway_d.c`

并实现：

```c
void receive_system_message(mixed msg);
```

## 7. 最小 LPC 示例

仓库里新增了一个最小示例：

- `testsuite/clone/gateway_login_example.c`

这个示例演示了：

- `logon()` 和 `gateway_logon()` 共存
- `gateway_receive()` 如何区分字符串和结构化 payload
- 如何用 `gateway_session_send()` 回发结构化消息
- 如何在 `gateway_disconnected()` / `net_dead()` 做会话收尾

如果你想在 testsuite 这套 mudlib 里试它，可以把：

- `testsuite/include/globals.h` 里的 `LOGIN_OB`

改成：

```c
#define LOGIN_OB "/clone/gateway_login_example"
```

或者让 `master.c::connect()` 直接返回这个对象。

## 8. 常用 efun

当前 `gateway.spec` 里可用的主要 efun：

- `gateway_listen(int port, int external | void)`
- `gateway_status()`
- `gateway_config(string key, mixed value | void)`
- `gateway_send(mapping data, mixed users | void)`
- `gateway_set_heartbeat(int interval, int timeout | void)`
- `gateway_check_timeout()`
- `gateway_ping_master(int fd | void)`
- `gateway_sessions()`
- `gateway_session_info(object user)`
- `gateway_session_send(object user, mixed data, mixed unused | void)`
- `gateway_destroy_session(string cid)`
- `is_gateway_user(object user)`

## 9. 一个最小接入顺序

建议按这个顺序接：

1. 打开 `PACKAGE_GATEWAY` 和 `PACKAGE_JSON_EXTENSION`
2. 配置 `gateway port`
3. 保留 master 的 `connect()`
4. 让 `connect()` 返回的对象实现 `gateway_logon()` 和 `gateway_receive()`
5. 先用 `write()` 验证文本输出是否能回到 gateway
6. 再根据业务决定是否在 `gateway_receive()` 里处理字符串命令或结构化 JSON

## 10. 当前文档刻意不写的内容

下面这些名字虽然能在注释或 spec 里看到，但这份文档没有把它们当成已完成能力：

- RPC 回调
- `gateway_connect()` 作为当前实际登录入口

原因很简单：

- 当前仓库实现里，这两项都不是实际生效的主路径
- 这份文档只记录代码现在真的会发生什么
