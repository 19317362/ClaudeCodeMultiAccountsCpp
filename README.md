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
- `usage` 通过 Claude API 拉取 5h / 7d **已用额度**(百分比),带 rate-limit 缓存
- 列账号时会为**每个**账号刷新过期/将过期的 token,让所有账号都能显示最新的额度与重置时间(不只当前账号)
- 自带简易 `install` / `uninstall`,配置 Claude 的 hooks / statusline / slash 命令
- 单文件、按调用名分发(`ccs`/`cc-switch` 列表切换,`ccso`/`cc-sync-oauth` 同步)

## 前置条件

- Linux(glibc / libstdc++)或 macOS(Xcode Command Line Tools)
- [xmake](https://xmake.io)(会自动用 xrepo 拉取并从源码编译 openssl + libcurl)
- 已安装并至少登录过一次 Claude Code

## 构建

```bash
xmake            # 首次会用 xrepo 下载并编译 openssl + libcurl(几分钟)
```

产物是单个可执行文件,路径随平台:

| 平台 | 产物 | 大小 |
| --- | --- | --- |
| Linux x86_64 | `build/linux/x86_64/release/ccs` | 约 4.7MB |
| macOS arm64 | `build/macosx/arm64/release/ccs` | 约 3.5MB |

验证第三方依赖确实静态链接进去了:

```bash
ldd    build/linux/x86_64/release/ccs   # Linux:只应看到 libc/libstdc++/libm/libgcc,无 curl/ssl
otool -L build/macosx/arm64/release/ccs # macOS:只应看到系统库与 Security/CoreFoundation,无 curl/ssl
```

### macOS 上的差异:凭证存在 Keychain

Claude Code 在 macOS 上**不写 `~/.claude/.credentials.json`**,而是把同一份
JSON(`{"claudeAiOauth": {...}}`)存进 login Keychain 的一条 generic password,
service 名为 `Claude Code-credentials`。因此 `ccs` 在 macOS 上通过
Security framework 读写 Keychain(见 `src/creds.cpp`),其余逻辑与 Linux 完全一致。

- 首次读写 Keychain 时,macOS 可能弹窗要求授权;选 **总是允许** 即可免除后续提示。
- 切换前会把 Keychain 里的旧值快照到
  `~/.claude/backups/multi-account-switch/.credentials.keychain.json.<时间戳>.bak`(0600),
  与 Linux 备份 `.credentials.json` 的行为对等。
- 传 `--credentials <路径>` 会强制改用文件后端(便于检查或测试),不再碰 Keychain。

只想在终端里手动用、不需要 Claude 集成时,跳过下面的 `install`,直接跑二进制或做个软链:

```bash
ln -sf "$PWD/build/macosx/arm64/release/ccs" ~/.local/bin/ccs   # 确保 ~/.local/bin 在 PATH 中
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

## 快速上手:从零开始添加 Alice / Bob 两个账号

`ccs` 只能捕获**当前已登录**的账号,所以「添加账号」= 依次登录每个账号,并在登录后
把它同步进 store。安装时注册的 `auth_success` 钩子会在每次登录成功后自动执行
`ccs sync`;若没安装钩子,手动运行 `ccso`(等价于 `ccs sync`)即可。

```text
# 前置:已构建并 `ccs install`,~/.local/bin 在 PATH 中
```

1. **登录第一个账号(Alice)**
   在 Claude Code 里执行 `/login`,用 Alice 的账号完成 OAuth 登录。
   - 已装钩子:登录成功后自动同步。
   - 未装钩子:手动执行一次 `ccso`。

   此时 `ccs` 应能看到:
   ```text
   Available Claude accounts:
   * [0] Alice | Pro | ... | used:just now
   ```

2. **登录第二个账号(Bob)**
   再次执行 `/login`,这次用 Bob 的账号登录(会覆盖 live 文件,指向 Bob)。
   同样自动同步,或手动 `ccso`。

   现在两个账号都在 store 里:
   ```text
   $ ccs
   Available Claude accounts:
     [0] Alice | Pro | ... | used:5m ago
   * [1] Bob   | Pro | ... | used:just now
   ```

3. **来回切换**
   ```bash
   ccs 0     # 切回 Alice
   ccs 1     # 切到 Bob
   ```
   切换后按提示 **重启 Claude Code** 使账号变更生效。

4. **(可选)起别名**
   列表里显示的是账号自身的 display name。若两个账号重名或想自定义,用别名区分:
   ```bash
   ccs --rename 0 Personal
   ccs --rename 1 Work
   ```

> 提示:第一次装好后,若你当前已登录了某个账号,直接 `ccso` 就能把它作为
> 「第一个账号」收进来,再 `/login` 换第二个账号即可,无需重复登录第一个。

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
5h used/reset: 75.0% / 2026-07-18 21:00:00
7d used/reset: 62.0% / 2026-07-20 22:00:00

Available Claude accounts:
  [0] Alice | Pro | 5H:91% (2026-07-18 21:00:00, ~1h) | 7D:52% (2026-07-21 13:00:00, 3D 1h) | used:13h ago
* [1] Bob | Pro | 5H:75% (2026-07-18 21:00:00, ~1h 46min) | 7D:62% (2026-07-20 22:00:00, 2D 2h) | used:1h ago

Run ccs <index> to make one of these stored entries the active Claude account.
Run ccs --remove <index> to remove a stored account.
```

其中 `5H` / `7D` 列显示的是 5 小时 / 7 天窗口的**已用**百分比(用得越多颜色越靠红),
括号里是该窗口的重置**绝对时间**(`yyyy-mm-dd hh:mm:ss`,后附到期倒计时);窗口无用量、
无重置时显示 `?`。行尾 `used:` 是该账号上次被选用的相对时间。

在 Claude chat shell 里也可用 `!ccs` / `!cc-switch` / `!cc-sync-oauth`。

## 文件与数据

| 路径 | 用途 |
| --- | --- |
| `~/.claude.json` | Claude live 配置(读写 `oauthAccount`) |
| `~/.claude/.credentials.json` | Claude live 凭证(读写 `claudeAiOauth`)—— **仅 Linux** |
| Keychain `Claude Code-credentials` | Claude live 凭证 —— **仅 macOS**,内容同上 |
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
| `src/creds.cpp` | live 凭证后端:Linux 走文件,macOS 走 Keychain |
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
- 凭证文件与 store(`~/.ClaudeCodeMultiAccounts.json`)都以 `0600` 写入 —— 两者都存着明文
  access / refresh token,不能对同组或其他用户可读;旧版本留下的 `0644` store 会在下次写入时收紧。
  反之,对本工具不拥有的文件(`~/.claude.json`)只沿用其原有权限,绝不放宽。
- 列账号(`ccs` / `ccs usage`)只使用现有 access token 查询用量,不会刷新或轮换 OAuth token。
  access token 已失效的账号会保留上次的用量快照;选择切换到该账号时,工具才会刷新 token,
  先把轮换后的 token 写入 store,再替换 Claude live 凭证,避免破坏 Claude Code 当前登录状态。

## License

MIT,见 [LICENSE](LICENSE)。
