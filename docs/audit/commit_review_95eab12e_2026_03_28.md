# 提交审查记录：`95eab12e`

## 审查对象

- 提交：`95eab12efa6d820ecd5d3586755d350f92fb7695`
- 标题：`fix(driver): 收口预登录竞态并补齐交互失败路径防护`

## 结论

本次源码级复核确认有 `1` 个高风险问题。

## 发现

- `高` 这次提交虽然把裸 `event_base_once(..., user)` 改成了 `ev_logon` 可取消事件，但在 `schedule_user_logon_internal()` 之后并没有把连接标记成“预登录禁止读”，`on_user_read()` 也没有阻断读事件。结果是客户端如果在 `APPLY_CONNECT` 真正完成前就把输入打进来，驱动仍可能先走 `get_user_data()` / `process_input()`，把命令提早喂给临时挂在 `master_ob` 上的 interactive。问题点位于 `src/comm.cc:schedule_user_logon_internal` 和 `src/comm.cc:on_user_read`。这个问题后续由 `5495a02c` 通过 `PENDING_LOGON` + `activate_user_input()` 收口。

## 备注

- 这次提交修掉了预登录 UAF、TLS 失败回退和多条失败清理链，属于明显进步；但“预登录期间仍可读命令”的时序问题在这个快照里还存在。
