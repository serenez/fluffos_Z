# 提交审查记录：`aaf3a495`

## 审查对象

- 提交：`aaf3a495c72233e65fd0fa9471ba63e67aa2c90f`
- 标题：`fix(driver): 修复动态交互缓冲回归并补强网关链路`

## 结论

本次源码级复核确认有 `2` 个高风险问题。

## 发现

- `高` ASCII 直读路径在处理换行时使用 `*(nl - 1)` 判断前一个字符是否为 `\r`，但没有先验证 `nl > line_start`。如果输入块恰好以单独的 `\n` 开头，会直接越界读取缓冲区前一字节。问题点位于 `src/comm.cc:process_ascii_chunk_internal`。这个问题后续由 `ed4a8a9d` 修复。

- `高` 预登录延迟登录仍然沿用裸 `interactive_t *` 的 `event_base_once(..., arg=user)` 调度方式，断线或失败清理后仍可能回调到已释放用户。问题点仍在 `src/comm.cc:new_conn_handler`。这个问题后续由 `95eab12e` 修复。

## 备注

- 这次提交已经把输入缓冲和 `MAX_TEXT` 回到更合理状态，但预登录生命周期还没真正收口。
