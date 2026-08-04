# C++ 版频繁要求重新登录修复说明

## 问题

C++ 版从提交 `7dc8849` 开始,在执行 `ccs` 或 `ccs usage` 时会刷新所有 access token
已过期或即将过期的账号。OAuth refresh token 是轮换且单次使用的,刷新当前账号后,
新 token 只写入 `~/.ClaudeCodeMultiAccounts.json`,没有同步写入 Claude Code 正在使用的
`~/.claude/.credentials.json`。

因此 live credentials 仍保留已经失效的旧 refresh token。当前 access token 到期后,
Claude Code 使用旧 token 刷新会收到 `invalid_grant`,随后要求用户重新登录。

原版 Node.js 实现的列表和 usage 流程只读取现有 access token,不会刷新 OAuth token。

## 修复

1. `src/usage.cpp` 不再在列表或 usage 流程调用 `refreshTokens()`。
2. OAuth token 只允许在账号切换流程刷新。
3. 切换流程继续保持安全顺序:刷新目标账号、先持久化 store、再写 Claude live credentials。
4. `writeJsonAtomic()` 的最终直接写入失败现在会抛出异常,不再静默留下配置与凭证不一致的状态。
5. README 已同步更新为新的 token 生命周期语义。

## 行为变化

- `ccs` 和 `ccs usage` 不会改变任何账号的 OAuth token。
- access token 已失效的非当前账号可能显示缓存的用量信息。
- 用户真正切换到该账号时才刷新 token;若 refresh token 已失效,切换会中止且不会覆盖当前
  Claude Code live credentials。

## 验证要点

- 源码中 `refreshTokens()` 只应由账号切换流程调用。
- `xmake build` 必须成功。
- 执行 `ccs` 或 `ccs usage` 前后,当前账号 live credentials 中的 access/refresh token
  不应被工具改写。
