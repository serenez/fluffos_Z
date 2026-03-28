# 提交审查记录：`115faae9`

## 审查对象

- 提交：`115faae9855e022fb8ea9ed8e2fd628af99e1510`
- 标题：`feat(driver): 接入 gateway 包并补强编译诊断与运行时链路`

## 结论

本次源码级复核确认有 `2` 个高风险问题。

## 发现

- `高` 预登录延迟登录回调把裸 `interactive_t *` 直接交给 `event_base_once(..., arg=user)`，断线或建连失败后没有可取消句柄，回调可能在对象释放后继续触发，存在 use-after-free 风险。问题点位于 `src/comm.cc:new_conn_handler`。这个问题后续由 `95eab12` 收口。

- `高` 本提交把 `MAX_TEXT` 从 `1 MiB` 直接降到了 `64 KiB`，同时 `decode_mud_packet_size()` 又把 MUD 包大小上限绑在 `MAX_TEXT - 5`。这会把原本可接受的大包、网关大文本或较大协议帧直接判成非法输入，形成兼容性回退。问题点位于 `src/interactive.h:MAX_TEXT` 和 `src/comm.cc:decode_mud_packet_size`。这个问题后续由 `aaf3a495` 回退/修正。

## 备注

- 这次提交引入 gateway 主链本身没有在这轮审查里发现更高优先级的新增逻辑错误，主要风险集中在预登录生命周期和输入上限这两处。
