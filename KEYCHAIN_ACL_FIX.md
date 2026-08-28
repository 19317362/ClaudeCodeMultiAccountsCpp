# macOS 反复弹出 Keychain 授权窗的修复说明

## 问题

在 macOS 上使用 `ccs` 切换账号后,Claude Code 每次读取凭证都会弹出
「Claude Code-credentials」授权窗。输入密码并点击**始终允许**之后仍然不停重复,
短时间内可弹出上百次。`ccs` 自己读写 Keychain 却从不弹窗。

## 成因

`src/creds.cpp` 的 `keychainWrite()` 在条目不存在时用 `SecItemAdd` 新建。系统为这样
创建的条目生成的 ACL 有两处问题:

```
entry: authorizations (1): partition_id
       description: cdhash:<ccs>, apple:, apple-tool:
entry: authorizations (1): change_acl
       applications (0):
```

1. **partition list 只含创建者**。ccs 是 ad-hoc 签名,没有 TeamIdentifier,系统按
   cdhash 把它写进 partition list。Claude Code 是另一个签名身份(Developer ID,
   TeamIdentifier `Q6L2SF6YDW`),不在列表内,每次读取都被拦下。
2. **`change_acl` 的应用列表为空**。点击「始终允许」时,系统会尝试把请求方追加进
   partition list,但没有任何程序有权修改这个 ACL,写回失败。

两者叠加就是死循环:拦下 → 弹窗 → 授权写不回去 → 下次继续拦下。这解释了为什么
「始终允许」无效,以及为什么只有 Claude Code 弹窗而 ccs 不弹。

作为对照,由 `security` 命令行创建的条目 partition list 是 `apple:, apple-tool:`,
且 `change_acl` 授权给 `/usr/bin/security`,因此「始终允许」能正常生效。

## 修复

`scripts/fix-keychain-acl.sh` 用登录密码直接改写 partition list。
`SecKeychainItemSetAccessWithPassword` 凭密码授权,不受 `change_acl` 空列表限制:

```bash
./scripts/fix-keychain-acl.sh          # 修复,会提示输入 macOS 登录密码
./scripts/fix-keychain-acl.sh --show   # 只打印当前 ACL,不修改、不需要密码
```

脚本自动探测本机 Claude Code 的 TeamIdentifier 与 ccs 各副本的 cdhash,把
partition list 设为:

```
apple:, apple-tool:, teamid:<Claude Code>, cdhash:<ccs>
```

Claude Code 按 **teamid** 授权而非 cdhash,因此升级版本后依然匹配。

## 行为变化

- Claude Code 读取 Keychain 凭证不再弹窗。
- 条目内容(access / refresh token)不受影响,脚本只改 ACL 不碰密码数据。
- `ccs` 的读写行为不变。

## 已知限制

`ccs` 是 ad-hoc 签名,cdhash 由二进制内容决定,**重新编译后会变化**,届时轮到 ccs 被
挡在 partition list 之外。重跑一次脚本即可,它每次都重新读取当前 cdhash。

## 为什么不改 C++ 源码

`src/creds.cpp` 中的 `SecItemAdd` 只在条目不存在时才会走到——正常切换流程会先经过
`readLiveCredentials()`,条目缺失时它已经报错要求先 `/login`。而 `keychainWrite()`
的常规路径是 `SecItemUpdate`,**不会改动 ACL**,源码中也没有任何删除条目的逻辑
(无 `SecItemDelete`)。因此只要条目一直存在,修好的 partition list 就能长期保持。

彻底根治新建路径需要在 `SecItemAdd` 时通过 `SecAccessCreate` 传入空信任数组来构造
开放 ACL,涉及已废弃的 Keychain API,且必须在真机上验证 ACL 与 partition list 的实际
生成结果,收益与风险不成比例,故暂不改动。

## 验证要点

- `./scripts/fix-keychain-acl.sh --show` 中 `partition_id` 一项应包含
  `teamid:Q6L2SF6YDW` 与本机 ccs 的 cdhash。
- 修复前后 `security find-generic-password -s "Claude Code-credentials" -w` 读出的
  内容应逐字节一致。
- Claude Code 重启后正常读取凭证,不再弹窗。
