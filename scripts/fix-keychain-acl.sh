#!/bin/bash
# 修正 macOS login Keychain 中 "Claude Code-credentials" 条目的 partition list。
#
# 症状
#   Claude Code 每次读取凭证都弹出「Claude Code-credentials」授权窗,输入密码并点
#   「始终允许」之后仍然不停重复,短时间内可弹出上百次。ccs 自己用起来却从不弹窗。
#
# 成因
#   该条目由 ccs 通过 SecItemAdd 创建时(见 src/creds.cpp 的 keychainWrite),系统只把
#   调用方——也就是 ccs 自己的 cdhash——写进 partition list,并且把 change_acl 授权的
#   应用列表置空:
#
#     entry: authorizations (1): partition_id
#            description: cdhash:<ccs>, apple:, apple-tool:
#     entry: authorizations (1): change_acl
#            applications (0):        <-- 没有任何程序有权修改这个 ACL
#
#   Claude Code 是另一个签名身份(Developer ID, TeamIdentifier Q6L2SF6YDW),不在
#   partition list 内,于是每次读取都被拦下。点「始终允许」时系统会尝试把它追加进
#   partition list,但 change_acl 为空列表,写回失败,下次照旧弹窗——形成死循环。
#
# 修法
#   用登录密码直接改写 partition list(SecKeychainItemSetAccessWithPassword 凭密码授权,
#   不受 change_acl 限制),放行 Claude Code 的 TeamIdentifier 与本机 ccs 的 cdhash。
#
# 用法
#   ./scripts/fix-keychain-acl.sh          修复(会提示输入 macOS 登录密码)
#   ./scripts/fix-keychain-acl.sh --show   只打印当前 ACL,不做任何修改、不需要密码
#
# 注意
#   ccs 是 ad-hoc 签名,没有 TeamIdentifier,只能按 cdhash 授权;重新编译 ccs 会改变
#   cdhash,届时轮到 ccs 被挡在外面,重跑本脚本即可。Claude Code 用的是 teamid,升级
#   版本不受影响。
set -euo pipefail

SERVICE="Claude Code-credentials"
KEYCHAIN="$HOME/Library/Keychains/login.keychain-db"
# Anthropic PBC。仅在本机找不到 claude 可执行文件时作为兜底。
CLAUDE_TEAM_ID_FALLBACK="Q6L2SF6YDW"

if [ "$(uname -s)" != "Darwin" ]; then
  echo "本脚本只适用于 macOS(Linux 上凭证是普通文件,不涉及 Keychain ACL)。" >&2
  exit 1
fi

# 打印条目的 access 段(属性与 ACL,不含密码数据,因此不需要授权)。
show_acl() {
  security dump-keychain -a "$KEYCHAIN" 2>/dev/null \
    | grep -A 45 "\"svce\"<blob>=\"$SERVICE\"" \
    | awk '/^access:/ { p = 1 } p && /^keychain:/ { exit } p'
}

if ! security find-generic-password -s "$SERVICE" "$KEYCHAIN" >/dev/null 2>&1; then
  echo "login Keychain 中没有 \"$SERVICE\" 条目;先在 Claude Code 里 /login。" >&2
  exit 1
fi

if [ "${1:-}" = "--show" ]; then
  show_acl
  exit 0
fi

# 条目通常记在登录用户名下,但也可能是别的 account 名。带 -a 更精确,匹配不上就退回
# 只按 service 定位——与 src/creds.cpp 中 keychainReadAny() 的两级查找保持一致。
account_args=()
if security find-generic-password -s "$SERVICE" -a "$USER" "$KEYCHAIN" >/dev/null 2>&1; then
  account_args=(-a "$USER")
fi

parts=(apple: apple-tool:)

# Claude Code 用 teamid 授权:版本升级会换 cdhash,但 TeamIdentifier 不变。
claude_bin="$(command -v claude || true)"
team=""
if [ -n "$claude_bin" ]; then
  team="$(codesign -dvvv "$claude_bin" 2>&1 | sed -n 's/^TeamIdentifier=//p' || true)"
fi
if [ -z "$team" ] || [ "$team" = "not set" ]; then
  team="$CLAUDE_TEAM_ID_FALLBACK"
  echo "提示:未能从本机 claude 读到 TeamIdentifier,使用已知值 $team。"
fi
parts+=("teamid:$team")

# ccs 可能同时存在多个副本(install 会在 ~/.local/bin 放 cc-switch / ccs / cc-sync-oauth
# / ccso,以及 ~/.claude/multi-account-switch/bin/ccs)。内容相同则 cdhash 相同,去重即可。
seen_cdhash=""
for candidate in \
  "$HOME/.claude/multi-account-switch/bin/ccs" \
  "$HOME/.local/bin/ccs" \
  "$HOME/.local/bin/cc-switch" \
  "$HOME/.local/bin/cc-sync-oauth" \
  "$HOME/.local/bin/ccso" \
  "$(command -v ccs || true)"
do
  [ -n "$candidate" ] && [ -x "$candidate" ] || continue
  cd_hash="$(codesign -dvvv "$candidate" 2>&1 | sed -n 's/^CDHash=//p' || true)"
  [ -n "$cd_hash" ] || continue
  case " $seen_cdhash " in *" $cd_hash "*) continue ;; esac
  seen_cdhash="$seen_cdhash $cd_hash"
  parts+=("cdhash:$cd_hash")
done

S="$(IFS=,; echo "${parts[*]}")"
echo "将 partition list 设为:"
echo "  $S"
echo
echo "接下来 security 会要求输入你的 macOS 登录密码(输入时不回显):"

security set-generic-password-partition-list \
  -S "$S" -s "$SERVICE" "${account_args[@]}" "$KEYCHAIN" >/dev/null

echo
echo "✅ 已更新。当前 ACL:"
show_acl
