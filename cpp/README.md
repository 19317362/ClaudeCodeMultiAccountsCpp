# ccs — Claude Code Multi-Account Switcher (C++ 单文件版)

这是 Node 版 `ccs` / `cc-switch` 的 C++ 移植:**编译后只有一个可执行文件**,
不再需要往 `~/.claude` 里铺一整套 `lib/*.cjs`。用 [xmake](https://xmake.io) 管理
编译,第三方依赖(`nlohmann_json`、`libcurl`、`openssl`)通过 xrepo 拉取并**静态链接**,
所以产物只依赖系统的 glibc / libstdc++。

## 功能对齐

与 Node 版核心行为一致:

- 账号快照存于 `~/.ClaudeCodeMultiAccounts.json`
- 从 `~/.claude.json`(`oauthAccount`)+ `~/.claude/.credentials.json`(`claudeAiOauth`)读取当前 live 账号
- 列表 / 按 index 切换 / `sync` / `usage` / `--remove` / `--rename`
- 切换前检测运行中的 `claude` 进程、按需刷新 OAuth token(联网)、**先写 store 再写 live**、原子写 + 每文件保留最近 3 个备份
- 切换时只改写 `oauthAccount` 与 `claudeAiOauth`,保留两个文件里其它所有字段
- `usage` 通过 `api.anthropic.com/api/oauth/usage` 拉取,带 rate-limit 缓存
- 工具自身设置存于 `~/.claude/multi-account-switch/settings.json`(`showUsage`、`rateLimitResetAt`)

## 构建

```bash
cd cpp
xmake            # 首次会用 xrepo 下载并从源码编译 openssl + libcurl(几分钟)
```

产物:`build/linux/x86_64/release/ccs`(约 4.7MB,单文件)。

```bash
ldd build/linux/x86_64/release/ccs   # 只应看到 libc / libstdc++ / libm / libgcc,无 curl/ssl
```

## 安装(简易)

```bash
./build/linux/x86_64/release/ccs install
```

`install` 会:

- 把二进制复制到 `~/.claude/multi-account-switch/bin/ccs`
- 在 `~/.local/bin` 放置 `cc-switch` / `ccs` / `cc-sync-oauth` / `ccso` 四个同一二进制的副本
  (按调用名分发:`cc-sync-oauth`、`ccso` 默认执行 sync,其余执行 list/switch)
- 在 `~/.claude/settings.json` 写入 `auth_success`(sync)、`SessionStart startup`(session-start)
  钩子与 `statusLine`;若已有 statusline 命令,会保存为下游透传目标
- 在 `~/.claude/commands` 写入 `/cc-switch`、`/cc-sync-oauth` slash 命令
- 写入前对 `settings.json` 做备份

确保 `~/.local/bin` 在 `PATH` 中。卸载:`ccs uninstall`(保留已存账号快照)。

## 用法

```bash
ccs                    # 列出账号(带 usage)
ccs 1                  # 切换到 index 1
ccs sync               # 把当前 live 账号写入 store
ccs usage              # 拉取当前账号用量
ccs --remove 2         # 删除某个已存账号
ccs --rename 1 Work    # 设置别名;省略名字则清除
ccs --hide-usage       # 关闭列表里的 usage 显示(--show-usage 打开)
```

## 源码结构

单一二进制,按职责拆分为多个 `.cpp`(全部声明集中在 `src/ccs.hpp`):

| 文件 | 职责 |
| --- | --- |
| `paths.cpp` | 默认路径、`jx::` JSON 取值助手、文件系统 |
| `timefmt.cpp` | ISO 时间解析/格式化(对齐 JS `Date`) |
| `store.cpp` | JSON 读写、原子写、备份、live/store 落盘、工具设置 |
| `accounts.cpp` | 账号 key、store 同步、选择、凭证守卫 |
| `http.cpp` | libcurl 封装(POST/GET) |
| `auth.cpp` | OAuth token 刷新 |
| `usage.cpp` | 用量 API、快照、列格式化 |
| `output.cpp` | 显示名、套餐推断、汇总、消息、颜色 |
| `actions.cpp` | list/switch/sync/usage/remove/rename 主流程、进程检测 |
| `install.cpp` | install / uninstall / session-start / statusline |
| `main.cpp` | 参数解析与分发 |
