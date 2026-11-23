# Qoder 认证与账户协议

本文记录 QoderZ 与 Qoder 服务器之间的站点、登录、令牌刷新、COSY 签名，以及模型目录与
用量查询的线上协议。它是 `src/qoder/auth/`、`src/qoder/catalog/`、`src/qoder/usage/`
与 `src/protocol/cosy_signer` 的规范来源。凭据文件如何落盘、账户如何选择，见
[账户与凭据](../design/accounts.md)；CLI 交互见 [CLI 设计](../design/cli.md)。

> 协议是非官方逆向结果。所有测试只使用 loopback stub，不接触真实站点，也不消耗额度。

## 1. 站点

| id | Web | OpenApi | Api | ClientId | DeviceChat | VpcDomain |
| --- | --- | --- | --- | --- | --- | --- |
| `qoder` | `https://qoder.com` | `https://openapi.qoder.sh` | `https://api3.qoder.sh` | `732aef47-9cf2-46a2-95fe-4cebb5d0d1fa` | false | 无 |
| `qoder-cn` | `https://qoder.cn` | `https://openapi.qoder.com.cn` | `https://gateway.qoder.com.cn` | `e883ade2-e6e3-4d6d-adf7-f92ceff5fdcb` | true | `vpc.qoder.com.cn` |

站点 id 大小写不敏感，前后空白会 trim。未知站点或空站点是配置错误（退出码 1），
不能回退到默认站点。

### VPC

只有 `qoder-cn` 支持企业 VPC。实例解析后生成：

```text
web      = https://{instance}.vpc.qoder.com.cn
openapi  = https://{instance}-openapi.vpc.qoder.com.cn
api      = https://{instance}-gateway.vpc.qoder.com.cn
```

实例校验规则：

1. `null` / 空白表示“没有 VPC”，不是错误；
2. trim 后转小写；
3. 拒绝空串、`http://`、IPv4 字面量；
4. `https://` 形式只接受绝对 https、缺省端口或 443、无 userinfo/query/fragment、
   path 恰为 `/`、host 与去掉前缀和单个尾 `/` 的原文一致；
5. 若带域名，依次剥掉 `-openapi.{domain}`、`-gateway.{domain}`、`.{domain}`；
6. 剥完不能为空、不能含 `.`；
7. 必须匹配 `^[a-z0-9]([a-z0-9-]*[a-z0-9])?$`：拒绝 `_`、首尾 `-`。

凭据里的 VPC 优先于站点自身的 VPC。站点不支持 VPC 却要求 VPC、或实例非法，都是配置错误，
**绝不回退公网主机**。

### 测试用上游覆盖

`QODERZ_UPSTREAM` 可以把三个 base 全部指向一个 loopback origin（如
`http://127.0.0.1:18899`）。只接受 loopback；非 loopback 直接配置错误，避免把凭据交给
任意主机。这个开关只用于测试与本地测量，默认关闭。

## 2. 设备流登录

### 2.1 参数

| 参数 | 生成方式 |
| --- | --- |
| `verifier` | 64 个随机字节，base64url 无 padding（86 字符） |
| `challenge` | `base64url(SHA256(ASCII(verifier)))` |
| `nonce` | 新 UUID，36 字符小写带连字符 |
| `machine_id` | 新 UUID；登录成功后持久化到凭据 |

设备页 URL：

```text
{web}/device/selectAccounts
  ?challenge={challenge}&challenge_method=S256
  &nonce={nonce}&machine_id={machine_id}&client_id={client_id}
```

没有 `redirect_uri`。终端打印该 URL；非 `--no-browser` 时尝试打开浏览器，失败只提示手动打开。
提示文本：`Sign in to {站点名} in the browser and authorize this device`。

### 2.2 轮询

```text
GET {openapi}/api/v1/deviceToken/poll
  ?nonce={nonce}&verifier={verifier}&challenge_method=S256
```

- 先请求再等待；间隔 2 秒；总预算 15 分钟；
- 成功条件：HTTP 200 且 `token` 或 `device_token` 非空；
- 其它状态码、空 token 都继续轮询，不报错；
- 超时 → 认证错误（未过期）：`the sign-in timed out; start again`；
- 成功体里同时兼容 `refresh_token` / `refreshToken`、`user_id` / `uid` / `id`、
  `user_name` / `name`。

### 2.3 换 job token

```text
POST {openapi}/api/v1/me/jobToken
Authorization: Bearer {poll_token}
Content-Type: application/json

{"clientId":"{client_id}"}
```

- 200：`access = token | access_token`，必须非空；`refresh = refresh_token | refreshToken`；
  `expires_in` 按**毫秒**解释；`deviceChat = false`；
- `qoder-cn` 且 HTTP 4xx（**429 除外**）：回退到轮询拿到的 device token：
  `access = poll.token`、`refresh = poll.refresh`、`deviceChat = true`、
  `expires_in` 按**秒**解释；
- `qoder-cn` 且 HTTP 429：不视为回退信号，按 [§8](#8-错误映射) 映射为额度错误。
  回退会把 `deviceChat = true` 永久写进账户记录，而 429 是临时限流；用一次节流换账户永久降级
  不值得，也不符合「429 可重试」的语义；
- 国际站同状态：一律认证错误（说明这次登录被上游拒绝），不看 body 文案；
- 其它非 200：上游错误。

### 2.4 userinfo

```text
GET {openapi}/api/v1/userinfo
Authorization: Bearer {poll_token}
```

始终用轮询得到的 device token（不是刚发的 job token）。200 时读 `email`、
`name | display_name | user_name`、`id | user_id | uid`；非 200 返回空，不算致命。
合并规则：

- `uid = userinfo.uid ?? poll.user_id`
- `name = userinfo.name ?? poll.user_name`
- `email` 只来自 userinfo，可为空

VPC 登录且 uid 为空 → 认证错误 `Qoder returned no account id for the VPC sign-in`。

### 2.5 落库字段

设备登录成功后保存：

| 字段 | 值 |
| --- | --- |
| `access` | job token（CN 回退时等于 device token） |
| `refresh` | job refresh（CN 回退时为 device refresh） |
| `expires_at` | 绝对时间，毫秒 |
| `uid` / `name` / `email` | 来自 userinfo 合并 |
| `account_id` | email（空则 uid），VPC 非空时再拼 ` (vpc)`；仅用于展示/身份匹配 |
| `machine_id` | 登录开始时的 machine_id |
| `device_token` / `device_refresh` | 轮询拿到的 device token 对 |
| `device_chat` | 是否走了 CN device fallback |
| `pat_auth` / `pat` | false / 空 |
| `vpc` | 实例 id 或空 |

## 3. PAT 登录

PAT 必须以 `pt-` 开头（区分大小写），trim 后非空；校验在任何网络请求之前完成。
命令行直接传 `--pat` 会进入 shell 历史与 `ps`，QoderZ 额外支持 `--pat-stdin` 与
`QODERZ_PAT` 环境变量（见 CLI 文档）。

```text
POST {openapi}/api/v1/jobToken/exchange
Content-Type: application/json

{"personal_token":"pt-..."}
```

无 `Authorization` 头。400/401/403 → 认证错误（未过期）；其它非 200 → 上游错误；
`token | device_token | access_token` 为空 → 上游错误。

随后用新 access 调用 `GET /api/v1/userinfo`；失败或 uid 为空 → 认证错误。落库时：

- `pat_auth = true`，`pat` = trim 后的明文 PAT；
- `machine_id` = 新 UUID；
- `device_token` / `device_refresh` = 空，`device_chat = false`；
- 其余字段同设备登录。

## 4. 令牌刷新

刷新完全惰性：每次使用凭据前检查，不跑后台定时器。

### 4.1 新鲜度

`expires_at - now > 5 分钟` 视为新鲜，直接返回，不读盘、不发请求。

### 4.2 单飞与“已花掉”映射

refresh token 是单次使用的，并发的刷新必须只发生一次：

- 每个账户一把异步门；并发调用者在门内等待，只让第一个真正刷新；
- 记住“旧 refresh token → 新凭据”的映射，保留 1 小时、最多 16 跳；
- 拿门后重新读盘：如果磁盘上的 access 已变，说明别人刷过，直接用磁盘版或映射结果；
- 账户被删导致保存失败：只告警，仍返回新凭据；随后逐出该账户的门。

### 4.3 端点

| 分支 | 端点 | 请求体 | 200 读取 | 失败语义 |
| --- | --- | --- | --- | --- |
| job | `POST {openapi}/api/v1/jobToken/refresh` | `{"refresh_token":"..."}` | `token|access_token` 与 `refresh_token|refreshToken` 都非空；`expires_in` 毫秒 | 401/403：PAT 账户用旧 PAT 重新 exchange（保留 machine_id），否则认证过期；其它非 200 上游错误 |
| device | `POST {openapi}/api/v1/deviceToken/refresh` | `{"refresh_token":"..."}` | `token|device_token` 非空；refresh 缺失沿用旧值；`expires_in` 秒；同时写 `access = device_token`、`refresh = device_refresh` | 401/403 一律认证过期；其它非 200 上游错误 |

只有 401/403 标认证失败（CLI 退出 3；服务端冷却 15 分钟），其它非 200 只是可重试的上游错误
（退出 4）。网络异常与取消原样向上抛。

### 4.4 时间字段

`expires_in` 的单位由端点决定：job 毫秒、device 秒。绝对时间优先，兼容
`expires_at | expire_time | expiresAt | expireTime`；字符串先按 ISO-8601 解析，纯数字
`< 1e12` 视为秒再乘 1000；全部缺失时默认 `now + 1 天`。

## 5. COSY 签名

COSY 只用于 `site.api` 下的 `/algo/*` 调用（模型目录、聊天）。认证与用量走普通
`Authorization: Bearer <token>`。

### 5.1 参数

- `CosyVersion = "1.1.69"`；RSA 公钥固定内嵌（1024-bit，只用于加密 AES key）；
- `key` = **8 个随机字节的小写 hex 字符串的 ASCII 字节**，即 16 字节 AES-128 key；
  不是 8 字节原始随机数，这是最容易错的一步；
- `info` 明文 JSON，字段顺序固定：

  ```json
  {"uid":"...","aid":"","name":"...","email":"...","security_oauth_token":"<access>"}
  ```

- `info` 密文 = AES-128-CBC / PKCS7，**IV = key**，再 base64；
- `wrapped_key` = RSA PKCS#1 v1.5 加密 `key` 的 base64，固定 128 字节；
- `request_id` = 16 随机字节的 32 位小写 hex；
- `payload` = base64(紧凑 JSON)：

  ```json
  {"version":"v1","requestId":"<32hex>","info":"<base64>","cosyVersion":"1.1.69","ideVersion":""}
  ```

- `path` = URL 的绝对路径；若以 `/algo` 开头则去掉这 5 个字符；query/fragment 不参与；
- `signature` = 小写 hex MD5，输入按序：

  ```text
  payload + "\n" + wrapped_key + "\n" + unix_seconds + "\n" + UTF8(wire_body) + "\n" + path
  ```

  `wire_body` 是编码后的请求体；空体就是空串。

- `Authorization: Bearer COSY.<payload>.<signature>`。

### 5.2 请求头

```text
Accept: application/json
Accept-Encoding: identity
Content-Type: application/json
Authorization: Bearer COSY.<payload>.<signature>
Cosy-Business-Product: app
Cosy-Business-Type: agent
Cosy-ClientIp: {machine_id}
Cosy-ClientType: 10
Cosy-Data-Policy: disagree
Cosy-Date: {unix_seconds}
Cosy-Key: {wrapped_key}
Cosy-MachineId: {machine_id}
Cosy-MachineToken: {machine_id}
Cosy-MachineType: 5
Cosy-MachineOS: {arch}_{platform}
Cosy-Scene: app
Cosy-User: {uid}
Cosy-Version: 1.1.69
Login-Version: v2
```

`arch` 为 `x86_64 | aarch64 | x86 | other`；`platform` 为 `win32 | darwin | linux | freebsd | unknown`，
例如 `x86_64_linux`。聊天调用会把 `Accept` 覆盖为 `text/event-stream`。

uid 或 machine_id 为空 → 配置错误：`The stored account has no uid or machine id; run 'qoderz login' again.`

### 5.3 固定向量

`key = ASCII("0123456789abcdef")`、identity 为
`uid-1 / Ada Lovelace / ada@example.com / jt-access-token / machine-42 / x86_64_linux` 时，
AES 部分（base64）为：

```text
cME0higR7RPuNL3LZJjqAexNgGJFc4QH+atZ/vo5QSKeChOvcZ6GoEgMij7vf0iv+0gnoLj14/LuhdGWd0IvhuirL6rOIxnN06ymuZtg9bRlhnISX5u9wGCOsP0ham1FZ8kCmHo+fhXOjCmExtOmjjNAYc5x3Yd6YAJ2iXZIdS8=
```

实现必须逐字节复现，不得为了让测试通过而重算期望值。RSA 加密是随机的，但输出 base64
解码后必须恰好 128 字节；同一时间戳的两次签名，`Cosy-Key` 与 `Authorization` 必须不同。

## 6. 模型目录

```text
GET {api}/algo/api/v2/model/list?Encode=1
Accept: application/json
```

使用 COSY 头，空体。非 200 → 统一错误映射。缓存 key 为 `site + vpc + uid`，TTL 5 分钟，
最多 64 条；空结果不缓存，直接返回站点内置 fallback。

合并 `app` 与 `chat` 两个数组：

1. 没有 `app`：有 `chat` 就返回 `chat`，否则空；
2. 有 `app` 无 `chat`：返回 `app`；
3. 遍历 `chat`：已在 `app` 的 key 只移动锚点，**不覆盖 app 版本**；新 key 插到锚点后；
   锚点初始 `-1` 表示追加到末尾。

过滤：`key` 非空、不是 `auto` / `default`、`enable` truthy（缺失即 false）、key 首次出现。

每个模型保留原始 JSON，作为聊天请求的 `model_config`。字段解析：

- `name = display_name`，空则 key；
- 窗口：`context_config` 各成员的 `token_count > 0` 去重；`default_window` 取第一个
  `is_default` 的；没有 `context_config` 时回退 `available_context_windows` +
  `default_context_window`；`context = max(windows)`，无窗口时取 `max_input_tokens`；
- `images = is_vl` truthy；
- 思考：有 `thinking_config` 时看 `enabled` 是否存在、`disabled` 是否存在；efforts 取
  `enabled.efforts` 的键，按 `minimal, low, medium, high, xhigh, max` 排序；无
  `thinking_config` 时用 `is_reasoning`；
- 价格字段只影响展示，不影响请求。

内置 fallback 模型列表在 `qoder/catalog/fallback.cpp`，以源码为准；空目录结果不能变成
“没有模型”可用。

## 7. 用量

```text
GET {openapi}/sash/api/v2/me/usage
Authorization: Bearer {token}
Cosy-ClientType: 10
User-Agent: Qoder
Accept: application/json
Accept-Encoding: identity
```

token 选择：PAT 账户且没有 device token 时用 `access`；否则优先 `device_token`，
再退 `access`。

解析：

- `displayMode / display_mode == "enterprise"`（大小写不敏感）→ 只有 plan，没有窗口；
- 否则必须等于 `qoder` 且存在对象 `qoderUsage / qoder_usage`，否则上游错误
  `Qoder: missing quota data`；
- `plan = userType | user_type`（先根后 usage 块）；
- `until = expiresAt | expires_at`（先根后 usage 块；字符串按 ISO，数字 `< 1e11` 当秒）；
- 窗口依次读 `userQuota`、`addOnQuota`、`orgResourcePackage`、
  `dedicatedResourcePackages[]`；
- 每个窗口 `total | cap` 必须是数字且 > 0；`used` / `remaining` 存在但不是数字则丢弃；
  两者都没有则丢弃；只有 remaining 时 `used = total - remaining`；`used < 0` 丢弃；
- 名称取块内 `name`，空则 fallback 名；单位取 `unit`，空则 `credits`；
- `percent = min(100, 100 * used / total)`；
- 显示文本 `"{used:0.##} / {total:0.##} {unit}"`。

数字含义是**已用**，不是剩余。`used=120, total=100` 显示 `120 / 100 credits`、百分比 100。

## 8. 错误分类

| 场景 | 分类 | CLI |
| --- | --- | --- |
| 站点/VPC/凭据文件/参数非法 | 配置错误 | 1 |
| 401/403、code 105、刷新被拒 | 认证错误 | 3 |
| 额度 code 110 / 112–119 / 122、HTTP 429 或 message 含 quota | 额度错误 | 4 |
| 10605 / queue.isQueued / serviceAvailable=false | 忙碌 | 4 |
| code 103 / message 含 `duplicate request` | 忙碌（终止，不再重发） | 4 |
| 406/416/430/80411/80412/100400–100403 | 请求错误 | 4 |
| 其它上游/传输失败 | 上游错误 | 4 |

日志只允许出现站点、uid、HTTP 状态。access/refresh/PAT、`Cosy-Key`、`Authorization`、
payload、signature、email、verifier、prompt 都不得进入日志。

## 9. QoderZ 模块

| 模块 | 职责 |
| --- | --- |
| `config/site` | 站点表、VPC 解析、loopback 覆盖 |
| `config/credentials` | 账户存储与身份匹配（见设计文档） |
| `qoder/auth/device` | 设备流参数、URL、轮询 |
| `qoder/auth/pat` | PAT 校验与交换 |
| `qoder/auth/token` | 刷新、单飞、已花掉映射 |
| `protocol/cosy_signer` | COSY 头与签名 |
| `qoder/catalog` | 模型目录合并、字段解析、fallback、缓存 |
| `qoder/usage` | 用量解析与快照 |
