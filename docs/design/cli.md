# CLI 设计

QoderZ 只有可执行文件 `qoderz`。CLI 的目标是让“多账户”变成自然操作，而不是一串必须记住的
全局旗标：默认账户、按名字/邮箱/uid 选择、TTY 下的编号选择、每个命令自己的 `--json`，
以及不会把 PAT 留在 shell 历史里的登录方式。

## 1. 命令总览

```text
qoderz <command> [options]
qoderz help [command]

commands:
  login     登录并保存账户（设备流或 PAT）
  logout    删除一个或多个账户
  accounts  列出账户
  account   管理单个账户（use / remove / show）
  models    打印模型目录
  usage     打印套餐与额度窗口
  serve     启动 OpenAI 兼容 HTTP 服务
  version   打印版本
  help      打印帮助
```

别名：`add` → `login`，`ls` → `accounts`，`rm` → `logout`。命令名大小写不敏感，但只接受一个。

全局选项可以放在命令前后：

```text
--site <id>            站点：qoder | qoder-cn
--credentials <path>   凭据文件路径
--log-level <level>    trace | debug | info | warn | error | critical
--no-color             禁用 ANSI 颜色（NO_COLOR 环境变量同效）
```

`--json` **不是**全局旗标：每个支持 JSON 的命令自己声明，避免出现“加了 `--json` 却静默无效”。
`--help` / `--version` 不触发任何网络或凭据读取。

## 2. login

```text
qoderz login [--site <id>] [--name <name>] [--vpc <instance>]
             [--pat <token> | --pat-stdin | --device]
             [--no-browser] [--replace] [--json]
```

- 默认设备流；给了 `--pat` 或 `--pat-stdin` 则走 PAT；`--device` 强制设备流，与两者互斥，此时 `QODERZ_PAT` 也被忽略。
- `--pat` 与 `--pat-stdin` 互斥。`--pat-stdin` 从标准输入读一行，避免 `ps` 与 shell 历史；
  `QODERZ_PAT` 也可以提供 PAT，优先级低于显式旗标。
- 站点默认取 `QODERZ_SITE`，再缺省 `qoder-cn`。输出会明确写出站点，避免“为什么登录到了 CN”。
- `--name` 先校验再联网；同站点忽略大小写唯一。
- `--replace`：允许“同名但不同身份”的登录覆盖旧账户；没有它则报错并提示。
- 设备流打印登录 URL 与说明；非 `--no-browser` 时尝试开浏览器，失败只提示手动打开。
  轮询期间每 15 秒向 stderr 打印一次已等待时长与剩余时间；非 TTY 下只打印开始与结束。
- 成功后在 stdout 打印：

  ```text
  Signed in to qoder-cn as ada@example.com (account 'work').
  work is now the default account for qoder-cn.
  ```

- `--json` 输出账户摘要，不含 token：

  ```json
  {"site":"qoder-cn","name":"work","vpc":"","email":"ada@example.com","uid":"u-123",
   "account_id":"ada@example.com","default":true}
  ```

## 3. accounts

```text
qoderz accounts [--site <id> | --all-sites] [--json] [--wide]
```

- 默认列出所有站点的账户；`--site` 过滤。
- 表格列：`NAME SITE EMAIL UID VPC EXPIRES KIND DEFAULT`。
  `KIND` 为 `device`、`pat` 或 `device+pat`；默认账户在名字后加 `*`；空值显示 `-`。
- `EXPIRES` 用本地时区并带偏移，例如 `2026-11-11 08:53 +08:00`，避免 UTC 误读。
- `--wide` 增加 `ACCOUNT_ID` 与 `CREATED` 列。
- `--json` 输出数组，字段：`name/site/vpc/email/uid/account_id/expires_at/kind/default`。
- 命令永不联网，也永不输出 token/PAT。

## 4. logout 与 account remove

```text
qoderz logout [--site <id>] [--account <selector>]... [--all] [--yes] [--json]
qoderz account remove <selector>... [--yes]
```

- `--account` 可重复；`--all` 删除该站点全部账户；两者互斥。
- 裸 `logout`：站点只有一个账户时删除它；多个账户时报错并列出名字，提示用 `--account` 或 `--all`。
- 删除前需要确认。TTY 下询问 `Remove account 'work' for qoder-cn? [y/N]`；非 TTY 或 `--yes` 时
  直接执行。`--all` 的确认文本会显示数量。
- 删除默认账户后，自动提升同站点剩余账户中按名字排序的第一个；最后一个账户被删时删除
  `defaults[site]`，全文件为空时删除凭据文件。
- 选择器不存在时返回非零并给可用列表；`--json` 输出 `{"removed":["qoder-cn/work"],"default":"..."}`。
- `account remove` 是同一行为的显式子命令，`logout` 保留为习惯入口。

## 5. account use / show

```text
qoderz account use <selector> [--site <id>]
qoderz account show <selector> [--site <id>] [--json]
```

- `use` 把账户设为所在站点的默认；如果 selector 同时匹配多个站点，必须用 `--site` 限定。
- `show` 打印站点、名字、VPC、email、uid、account_id、类型、过期时间、是否默认；
  永不打印 token。`--json` 字段与 `accounts --json` 的单元素一致。

## 6. models 与 usage

```text
qoderz models [--site <id>] [--account <selector>] [--json]
qoderz usage  [--site <id>] [--account <selector>] [--json]
```

- 选择账户：`--account` → 默认账户 → 站点唯一账户。
- 如果站点有多个账户且没有默认：TTY 下展示编号选择器（默认第一项，回车即选）；
  非 TTY 下报错并列出候选，提示 `qoderz account use <name>`。
- `models` 每次刷新目录；表格列 `ID NAME CONTEXT IMAGES REASONING`，CONTEXT 用 `1M`/`200K`
  形式；`--json` 使用 OpenAI 风格：

  ```json
  {"object":"list","data":[{"id":"ultimate","object":"model","owned_by":"qoder",
   "context_window":1000000,"supports_images":true,"supports_tools":true,
   "reasoning":["low","medium","high"]}]}
  ```

- `usage` 打印 `Plan`、`Expires`（本地时区带偏移）和每个窗口
  `Name: used / total unit used (percent%)`；`--json`：

  ```json
  {"plan":"pro_trial","until":"2026-01-01T00:00:00+08:00",
   "windows":[{"name":"Credits","used_percent":25.0,"display":"250 / 1000 credits"}]}
  ```

- 两个命令的日志都写 stderr；stdout 只留表格或 JSON。

## 7. serve

```text
qoderz serve [--listen <url>] [--site <id>]
             [--account <selector>] | [--accounts <selector>,...]
             [--api-key <key> | --api-key-stdin]
             [--threads <n>] [--max-body-bytes <n>] [--ignore-cooldown]
             [--allow-public] [--log-level <level>] [--json]
```

- `--listen` 默认 `QODERZ_LISTEN` 或 `http://127.0.0.1:8788`；只支持 http（本地网关）。
- 默认站点取 `--site`、`QODERZ_SITE`、`qoder-cn`。
- 默认使用该站点**全部账户**作为池；`--account` 固定一个账户、不再轮转（冷却仍然生效，
  `--ignore-cooldown` 才让冷却中的账户继续参与）；`--accounts a,b` 只使用指定账户（必须同站点）。
  `--account` 与 `--accounts` 互斥。
- `--threads` 默认 `min(hardware_concurrency, 4)`，最少 1。
- `--max-body-bytes` 默认 32 MiB；超过返回 413 OpenAI 错误体。
- `--api-key` 也可用 `QODERZ_API_KEY`；`--api-key-stdin` 从标准输入读取。
- 安全默认：监听非 loopback 且没有 API key 时**拒绝启动**，并提示设置 key；
  确实要公开时加 `--allow-public`。这比 NoQoder 的“只警告”更适合长期运行的服务。
- 启动时向 stderr 打印：版本、监听地址、站点、账户数量、模式（pool/pinned/subset）、
  线程数、body 上限。`--json` 打印机器可读的启动摘要，但服务仍把日志写 stderr。
- SIGINT/SIGTERM 优雅关闭：停止接受新连接，给在途请求一个短宽限，然后退出 0。

## 8. 无命令与 help

- `qoderz` 无参数：打印版本、凭据文件路径、各站点账户数/默认账户，以及常用命令提示；
  不联网，退出 0。这比直接甩一屏帮助更友好。
- `qoderz help <command>`：命令级帮助，包含 2–3 个示例和退出码说明。
- `qoderz version` / `--version`：`qoderz 0.1.2`。

## 9. 选择器与交互

- selector 先精确匹配 name（忽略大小写），再 uid，再 email；都没有时做唯一前缀匹配。
- 多义：TTY 下编号选择；非 TTY 报错并列出候选。
- 账户名、站点、过期时间都可以复制粘贴；不要求记住 key。
- 任何交互在非 TTY 下都必须退化为“明确失败 + 可执行提示”，不能等待输入。

## 10. 环境变量

| 变量 | 用途 | 默认 |
| --- | --- | --- |
| `QODERZ_SITE` | 默认站点 | `qoder-cn` |
| `QODERZ_CREDENTIALS` | 凭据文件 | XDG 路径 |
| `QODERZ_LISTEN` | serve 监听地址 | `http://127.0.0.1:8788` |
| `QODERZ_API_KEY` | serve 的 Bearer key | 无 |
| `QODERZ_ACCOUNT` | models/usage/serve 的默认选择器 | 无 |
| `QODERZ_LOG_LEVEL` | 日志级别 | `info` |
| `QODERZ_UPSTREAM` | 全部上游指向 loopback stub（仅测试/测量） | 无 |
| `QODERZ_PAT` | PAT 登录令牌 | 无 |
| `QODERZ_THREADS` | serve 线程数 | `min(hw,4)` |
| `NO_COLOR` | 禁用颜色 | 无 |

优先级：命令行 > 环境变量 > 内置默认。`QODERZ_ACCOUNT` 只在 models/usage/serve 生效；
在 login/logout/accounts 下不读取，也不静默改变行为。

## 11. 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 成功、`--help`、`--version`、无命令 |
| 1 | 配置错误（凭据文件、站点、VPC、参数校验）或未预期内部错误 |
| 2 | 命令行用法错误（未知命令/选项、互斥选项） |
| 3 | 认证失败或登录过期 |
| 4 | 上游失败（含额度、忙碌、请求被拒） |
| 130 | Ctrl-C |

错误统一写 stderr：`error: <message>`；命令结果写 stdout。`--json` 模式下 stdout 必须是纯 JSON。
日志永远写 stderr。

## 12. 相比 NoQoder 的改进

| NoQoder 的痛点 | QoderZ 的做法 |
| --- | --- |
| `--json` 是全局旗标，部分命令静默无效 | 每个命令显式声明 `--json`，不支持就报用法错误 |
| 无默认账户时 models/usage/serve 直接失败 | TTY 编号选择；`account use` 显式设默认；serve 默认用池 |
| 裸 `logout` 单账户无确认直接删 | 始终确认；`--yes` 供脚本使用 |
| PAT/API key 只能走命令行 | `--pat-stdin`、`--api-key-stdin`、环境变量 |
| 设备登录 15 分钟无进度 | 每 15 秒报告已等待/剩余时间 |
| 改名只能靠 `--name`，旧 key 无法引用 | selector 支持 name/uid/email/唯一前缀；`account show` 可查 |
| 池模式与固定账户语义只写在 README | `serve` 启动摘要明确模式与账户数；`--accounts` 支持子集 |
| 默认站点是 qoder-cn，国际用户被坑 | 保留默认但输出明确写站点；文档与 help 首屏提示 `--site qoder` |

## 13. 示例

```sh
# 设备流登录（默认 qoder-cn）
qoderz login --name work

# 国际站 PAT，从标准输入读，避免 shell 历史
printf '%s' "$PAT" | qoderz login --site qoder --pat-stdin --name personal

# 查看所有账户
qoderz accounts

# 把 personal 设为默认
qoderz account use personal

# 查询
qoderz models
qoderz usage --account work --json

# 只用两个账户开池
qoderz serve --accounts work,personal --listen http://127.0.0.1:8788

# 固定单账户
qoderz serve --account work
```
