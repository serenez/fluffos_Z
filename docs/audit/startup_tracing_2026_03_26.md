# 启动链路 Tracing 使用说明（2026-03-26）

## 背景

这轮改动的目标不是修改 mudlib 语义，也不是新增 efun/API，而是把驱动现有的启动与编译链路拆得更细，方便直接回答两个问题：

1. 启动慢，到底慢在 `load_object()` 的哪一段。
2. `jz_skill` 这类预加载 daemon，是慢在“文件编译”，还是慢在 `create()` 里继续触发的大量对象装载与 LPC 逻辑。

## 本次新增的 trace 点

### 启动总开关

- `driver.exe` 本来就支持从进程启动开始抓 trace，入口在 [mainlib.cc:343](D:\xiangmu\fluffos_Z\src\mainlib.cc:343) 到 [mainlib.cc:365](D:\xiangmu\fluffos_Z\src\mainlib.cc:365)。
- 用法：

```powershell
D:\xiangmu\fluffos_Z\build_codex_review_fix\src\driver.exe `
  --tracing D:\trace_startup.json `
  D:\xiangmu\fluffos_Z\24xinduobao\config.ini
```

### 编译阶段

在 [compiler.cc:2424](D:\xiangmu\fluffos_Z\src\compiler\internal\compiler.cc:2424) 到 [compiler.cc:2465](D:\xiangmu\fluffos_Z\src\compiler\internal\compiler.cc:2465) 增加了这些事件：

- `LPC Compile File`
- `compile.symbol_start`
- `compile.prolog`
- `compile.yyparse`
- `compile.symbol_end`
- `compile.epilog`

这些事件统一带 `args.object`，值是当前编译对象名。

### include 路径与文件打开

在 [lex.cc:1117](D:\xiangmu\fluffos_Z\src\compiler\internal\lex.cc:1117) 到 [lex.cc:1121](D:\xiangmu\fluffos_Z\src\compiler\internal\lex.cc:1121) 增加：

- `compile.init_include_path`

在 [lex.cc:1199](D:\xiangmu\fluffos_Z\src\compiler\internal\lex.cc:1199) 到 [lex.cc:1208](D:\xiangmu\fluffos_Z\src\compiler\internal\lex.cc:1208) 增加：

- `compile.include_open`

`compile.include_open` 会带：

- `args.include`
- `args.check_local`

### 对象装载阶段

在 [simulate.cc:484](D:\xiangmu\fluffos_Z\src\vm\internal\simulate.cc:484) 到 [simulate.cc:490](D:\xiangmu\fluffos_Z\src\vm\internal\simulate.cc:490) 增加：

- `load_object.compile_file`

在 [simulate.cc:579](D:\xiangmu\fluffos_Z\src\vm\internal\simulate.cc:579) 到 [simulate.cc:583](D:\xiangmu\fluffos_Z\src\vm\internal\simulate.cc:579) 增加：

- `load_object.valid_object`

在 [simulate.cc:590](D:\xiangmu\fluffos_Z\src\vm\internal\simulate.cc:590) 到 [simulate.cc:597](D:\xiangmu\fluffos_Z\src\vm\internal\simulate.cc:590) 增加：

- `load_object.init_object`
- `load_object.call_create`

## 最小验证方式

如果只想验证 tracing 是否生效，可以直接用 `lpcc.exe`：

```powershell
D:\xiangmu\fluffos_Z\build_codex_review_fix\src\lpcc.exe `
  --tracing D:\xiangmu\fluffos_Z\build_codex_review_fix\trace_lpcc_skill.json `
  D:\xiangmu\fluffos_Z\24xinduobao\config.ini `
  /kungfu/skill/1moliu/baduanjin
```

注意：

- `config.ini` 里如果 `mudlib directory` 写的是相对路径，`lpcc.exe` 的工作目录要切到 mudlib 根目录。
- 这次验证就是在 `D:\xiangmu\fluffos_Z\24xinduobao` 下执行的。

## 本次样本的直接结论

基于 `24xinduobao` 的一次真实 `lpcc --tracing` 样本，已经能看到下面这些关键事件：

- `adm/daemons/jz_skill.c`
  - `compile.yyparse`: `773 us`
  - `LPC Compile File`: `853 us`
  - `load_object.compile_file`: `854 us`
- `adm/daemons/jz_skill`
  - `load_object.call_create`: `507173 us`
  - `LPC Load Object`: `508087 us`

这说明一个很关键的事实：

- `jz_skill` 慢，**不是它这个 daemon 文件本身编译慢**
- 真正的大头在 `create()` 阶段
- 而 `create()` 里面又继续触发了 `des_skills()`、`save_allskill()`、目录扫描、技能对象加载和后续 LPC 逻辑

也就是说，驱动层现在已经能把“编译慢”与“create 链路慢”拆开看了。

## 当前 trace 结果里的另一个现实问题

在同一次样本里，trace 输出显示：

- 总时长大约 `6087141 us`
- 事件数达到 `1000001`
- trace 文件大小约 `120 MB`

这说明：

- `24xinduobao` 的 preload 链路本身就非常重
- 当前默认 `MAX_EVENTS = 1000000` 很容易被启动期打满

结果影响：

- trace 文件依然可用
- 但如果想抓“完整启动后半段”，有可能会被事件上限截断

## 建议的实际抓法

### 方案一：抓真实启动

适合看整条 preload 链。

```powershell
cd D:\xiangmu\fluffos_Z\24xinduobao
D:\xiangmu\fluffos_Z\build_codex_review_fix\src\driver.exe `
  --tracing D:\trace_startup.json `
  D:\xiangmu\fluffos_Z\24xinduobao\config.ini
```

重点搜索这些事件名：

- `LPC Load Object`
- `load_object.compile_file`
- `LPC Compile File`
- `compile.yyparse`
- `load_object.call_create`
- `compile.include_open`

### 方案二：抓单对象/单链路

适合缩小问题范围，避免 trace 被事件总量冲掉。

```powershell
cd D:\xiangmu\fluffos_Z\24xinduobao
D:\xiangmu\fluffos_Z\build_codex_review_fix\src\lpcc.exe `
  --tracing D:\trace_one_object.json `
  D:\xiangmu\fluffos_Z\24xinduobao\config.ini `
  /adm/daemons/jz_skill
```

或者：

```powershell
cd D:\xiangmu\fluffos_Z\24xinduobao
D:\xiangmu\fluffos_Z\build_codex_review_fix\src\lpcc.exe `
  --tracing D:\trace_one_skill.json `
  D:\xiangmu\fluffos_Z\24xinduobao\config.ini `
  /kungfu/skill/1moliu/baduanjin
```

## 这份 tracing 能回答什么

现在已经可以直接回答：

- 某个对象是慢在编译，还是慢在 `create()`
- 编译内部是 `yyparse` 慢，还是 include 查找/打开慢
- 某个 daemon 在 preload 阶段到底拖了多少时间

但它还不能自动回答：

- 哪段 LPC 业务逻辑“语义上最值得改”
- 哪个 mudlib 调用点从业务角度最该裁掉

这个仍然要结合 trace 结果再做二次分析。
