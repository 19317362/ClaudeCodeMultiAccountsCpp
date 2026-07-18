# Claude Code Multi-Account Switcher (C++)

`ccs` 是一个用于在多个 Claude Code OAuth 账号之间切换的命令行工具。它把每个账号的
快照保存在 `~/.ClaudeCodeMultiAccounts.json`,切换时只把选中的账号写回 Claude 的
live 文件,从而实现多账号并存与一键切换。

本仓库是该工具的 **C++ 实现**:用 [xmake](https://xmake.io) 构建,第三方依赖
(`nlohmann_json`、`libcurl`、`openssl`)通过 xrepo 拉取并**静态链接**,
**编译产物是单个可执行文件**,只依赖系统的 glibc / libstdc++——不需要 Node.js,
也不用往 `~/.claude` 里铺一整套脚本。

## 特性

- 列出、切换、同步、删除、重命名(别名)已保存的账号
- 切换前:检测运行中的 `claude` 进程、按需刷新即将过期的 OAuth token(联网)
- **先写 store 再写 live** 的顺序,配合原子写 + 每文件保留最近 3 个备份,避免写坏
- 切换只改写 `oauthAccount` 与 `claudeAiOauth`,保留两个文件里其它所有字段
- `usage` 通过 Claude API 拉取 5h / 7d 剩余额度,带 rate-limit 缓存
- 自带简易 `install` / `uninstall`,配置 Claude 的 hooks / statusline / slash 命令
- 单文件、按调用名分发(`ccs`/`cc-switch` 列表切换,`ccso`/`cc-sync-oauth` 同步)

## 前置条件

- Linux,glibc / libstdc++(编译期需要)
- [xmake](https://xmake.io)(会自动用 xrepo 拉取并从源码编译 openssl + libcurl)
- 已安装并至少登录过一次 Claude Code

## 构建

```bash
xmake            # 首次会用 xrepo 下载并编译 openssl + libcurl(几分钟)
```

产物:`build/linux/x86_64/release/ccs`(约 4.7MB,单文件)。

```bash
ldd build/linux/x86_64/release/ccs   # 只应看到 libc/libstdc++/libm/libgcc,无 curl/ssl
```

## 安装

```bash
./build/linux/x86_64/release/ccs install
```

`install` 会:

- 把二进制复制到 `~/.claude/multi-account-switch/bin/ccs`
- 在 `~/.local/bin` 放置 `cc-switch` / `ccs` / `cc-sync-oauth` / `ccso` 四个同一二进制的副本
  (按调用名分发:`cc-sync-oauth`、`ccso` 默认执行 sync,其余执行 list/switch)
- 在 `~/.claude/settings.json` 写入 `auth_success`(sync)、`SessionStart startup`
  (session-start)钩子与 `statusLine`;若已有 statusline 命令,会保存为下游透传目标
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
ccs --help             # 显示用法
ccs --version          # 显示版本
```

示例输出:

```text
$ ccs
--- Usage ---
5h remaining/reset: 25.0% / 2026-07-18 21:00:00
7d remaining/reset: 38.0% / 2026-07-20 22:00:00

Available Claude accounts:
  [0] Alice | Pro | 5H:9% (now) | 7D:48% (3D 1h) | used:13h ago
* [1] Bob | Pro | 5H:25% (~1h 46min) | 7D:38% (2D 2h) | used:1h ago

Run ccs <index> to make one of these stored entries the active Claude account.
Run ccs --remove <index> to remove a stored account.
```

在 Claude chat shell 里也可用 `!ccs` / `!cc-switch` / `!cc-sync-oauth`。

## 文件与数据

| 路径 | 用途 |
| --- | --- |
| `~/.claude.json` | Claude live 配置(读写 `oauthAccount`) |
| `~/.claude/.credentials.json` | Claude live 凭证(读写 `claudeAiOauth`) |
| `~/.ClaudeCodeMultiAccounts.json` | 本工具的账号快照存储 |
| `~/.claude/multi-account-switch/settings.json` | 工具设置(`showUsage`、`rateLimitResetAt`) |
| `~/.claude/backups/multi-account-switch/` | 写 live/store 前的备份 |

## 源码结构

单一二进制,按职责拆分为多个 `.cpp`(全部声明集中在 `src/ccs.hpp`):

| 文件 | 职责 |
| --- | --- |
| `src/paths.cpp` | 默认路径、`jx::` JSON 取值助手、文件系统 |
| `src/timefmt.cpp` | ISO 时间解析/格式化(对齐 JS `Date`) |
| `src/store.cpp` | JSON 读写、原子写、备份、live/store 落盘、工具设置 |
| `src/accounts.cpp` | 账号 key、store 同步、选择、凭证守卫 |
| `src/http.cpp` | libcurl 封装(POST/GET) |
| `src/auth.cpp` | OAuth token 刷新 |
| `src/usage.cpp` | 用量 API、快照、列格式化 |
| `src/output.cpp` | 显示名、套餐推断、汇总、消息、颜色 |
| `src/actions.cpp` | list/switch/sync/usage/remove/rename 主流程、进程检测 |
| `src/install.cpp` | install / uninstall / session-start / statusline |
| `src/main.cpp` | 参数解析与分发 |

## 说明

- 这是一个本地工具,不是官方 Claude 插件;切换时会改写 Claude 的内部 live 文件。
- 凭证文件写回时沿用「已存在文件的原有权限」;仅当文件不存在时才以 `0600` 新建。

## License

MIT,见 [LICENSE](LICENSE)。
