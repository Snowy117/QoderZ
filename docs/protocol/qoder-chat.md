# Qoder 聊天协议

本文记录 Qoder 上游 `agent_chat_generation` 的线上协议：请求体、COSY 签名、会话与任务、
队列重试、SSE 外壳、事件语义与错误映射。它是 QoderZ 上游客户端（`src/qoder/chat/`）
的规范来源；HTTP 服务如何把这些事件翻译成 OpenAI 响应，见
[openai-compat](openai-compat.md)。

> 协议是非官方逆向结果，可能随 Qoder 客户端升级而变。所有实现都应以固定夹具与
> 端到端 loopback 测试锁定行为，不依赖真实账号。

## 1. 端点与请求头

聊天只有一个端点，`POST`：

```text
{api_base}/algo/api/v2/service/pro/sse/agent_chat_generation
  ?FetchKeys=llm_model_result&AgentId=agent_common&Encode=1
```

`api_base` 由站点解析得到（见 [qoder-auth](qoder-auth.md)）。`Encode=1` 表示请求体经过
私有 BodyCodec 编码；客户端始终编码，不发送明文 JSON。

| 头 | 值 | 说明 |
| --- | --- | --- |
| `Accept` | `text/event-stream` | 覆盖 COSY 默认的 `application/json` |
| `Cache-Control` | `no-cache` | |
| `X-Model-Key` | 目录里的 `ModelDescriptor.Key` | 不是客户端请求中的 model 字符串 |
| `X-Model-Source` | 目录里的 `Source` | 为空时省略 |
| `Accept-Encoding` | `identity` | 压缩会破坏 SSE 逐帧解析 |
| `Content-Type` | `application/json` | 虽然 body 已编码 |
| 其余 19 个 COSY 头 | 见认证文档 | `Authorization: Bearer COSY.<payload>.<md5>` |

客户端使用 HTTP/1.1、流式读响应、连接超时 15 秒、读超时无限；不自动解压。
`Content-Length` 是编码后的字节数。

## 2. BodyCodec

编码不是标准 base64：它使用固定 64 字符表、`$` 补齐，并对整段做一次三等分换位。

- 字母表（顺序敏感）：

  ```text
  _doRTgHZBKcGVjlvpC,@aFSx#DPuNJme&i*MzLOEn)sUrthbf%Y^w.(kIQyXqWA!
  ```

- 每 3 字节映射为 4 个字符；余 1 字节补 `$ $`，余 2 字节补 `$`。
- `max_encoded_length(n) = ((n + 2) / 3) * 4`，编码结果长度恒等于它。
- 换位：`third = n / 3`（整除），把前缀 `[0, third)` 与后缀 `[n - third, n)` 整体交换，
  中段不动；`n < 4` 不交换。
- 解码只处理完整的 4 字符组，尾部不足 4 字符直接忽略；非法字符是错误。

固定向量：

| 明文 | 编码 |
| --- | --- |
| `""` | `""` |
| `a` | `$p$#` |
| `ab` | `$SB#` |
| `abc` | `MSK#` |
| `hello` | `q$FruHPH` |
| `Qoder` | `B$WzDxaS` |
| `工具调用` | `RQQ,n#SkyGoQumLQ` |

## 3. 请求体

写入顺序固定；无缩进，UTF-8 原样输出：

```text
request_id, request_set_id, chat_record_id, session_id,
business{product, version, type, id, name, begin_at, stage},
agent_id, task_id, session_type, model_config, system[], messages[], [tools[]], parameters{}
```

| 字段 | 值 / 来源 | 可省略 |
| --- | --- | --- |
| `request_id` | 每次翻译新生成 UUID | 否 |
| `chat_record_id` | 与 `request_id` 相同 | 否 |
| `request_set_id` | 当前任务 id（同一轮对话稳定） | 否 |
| `session_id` | 会话派生 id（见 §4） | 否 |
| `business.product/version/type` | `app` / `1.1.69` / `agent` | 否 |
| `business.id` | 任务 id | 否 |
| `business.name` | 最后一条 user 文本的前 10 个字符 | 否 |
| `business.begin_at` | 任务创建时刻 Unix 毫秒 | 否 |
| `business.stage` | 该任务已回答过则为 `processing`，否则 `start` | 否 |
| `agent_id` | `agent_common` | 否 |
| `task_id` | `common` | 否 |
| `session_type` | `app` | 否 |
| `model_config` | 模型目录里的原始对象，逐字保留 | 否 |
| `system` | `[{"type":"text","text":<system_text>}]` | 否 |
| `messages` | 首条恒为 system，见下 | 否 |
| `tools` | 仅当有工具且 `tool_choice != "none"` | 是 |
| `parameters.enable_thinking` | 由模型能力与请求推导 | 否 |
| `parameters.max_tokens` | `max_completion_tokens` 优先，其次 `max_tokens`，正整数，缺省 **32000** | 否 |
| `parameters.reasoning_effort` | 空则省略 | 是 |
| `parameters.context_length` | 见窗口选择；`<= 0` 省略 | 是 |

请求体**没有** `stream` 字段；流式由 URL 的 `Encode=1`、`Accept: text/event-stream` 决定。

### messages

- system：`{"role":"system","content":[{"type":"text","text":...}]}`，恒为第一条，文本可为空。
- user：`{"role":"user","content":[blocks...]}`；若原 OpenAI content 是数组，再写一份
  私有字段 `"contents":[blocks...]`。
- assistant：`{"role":"assistant","content":[blocks...],"tool_calls":[...]}`，无工具调用时省略后者。
- tool：`{"role":"tool","tool_call_id":"...","content":"<纯字符串>"}`。
- block：`{"type":"text","text":...}` 或 `{"type":"image_url","image_url":{"url":...}}`，
  可附加 `cache_control":{"type":"ephemeral"}`。
- `cache_control` 只加在最后一条 user 的最后一个“可缓存”块上；`thinking`、
  `redacted_thinking`、`tool_use`、`tool_result` 不缓存。
- 工具结果里的图片移到一条新的 user 消息，文本为 `[From the result of <call_id>:]`。
- tools 元素：`{"type":"function","function":{"name","description","parameters"}}`；
  缺 `parameters` 时补 `{"type":"object","properties":{}}`。

### system_text

固定前缀：

```text
You are a Qoder agent. Use the instructions below and the tools available to you to assist the user.
```

后接全部 system/developer 消息（`\n\n` 连接）；`tool_choice == "required"` 时再追加：

```text
You must call an available function in this response.
```

`tool_choice` 字符串按小写比较；对象视为 `"function"`；`"none"` 时不发送 tools，也不追加 required。

### 窗口选择

`len = messages_bytes + tools_bytes + 11`（无 tools 时按 `"[]"` 计 2 字节）；
`need = (len + 2) / 3 + max_tokens`。取：

1. `default_window > 0` 时先用它，否则用 `context`；
2. `need <= base` → `base`；
3. 否则取大于等于 `need` 的最小窗口；
4. 再否则取最大窗口；
5. 再否则取 `context`。

因为窗口依赖已经写出的 messages/tools 字节数，`parameters` 必须最后写（或分两遍测量）。

## 4. 会话 id 与任务生命周期

### session_id

依次取第一个非空来源：

1. HTTP 头 `X-Qoder2Api-Session` 或 `X-Session-Id`，标签 `session`；
2. 请求体 `prompt_cache_key`，标签 `key`；
3. 请求体 `user`，标签 `user`；
4. 都没有：标签 `chat`，内容为 system_text 与首条 user 文本。

对“标签 + 值”做 SHA-256，每段先写 **int32 小端字节数**，再写 UTF-8 字节。得到 64 位小写
hex，取前 32 位格式化成 `8-4-4-4-12`。它形似 UUID 但没有版本位，不编码账户、不持久化。

### 任务（request_set_id）

`turn_key = SHA256(len(session_id) + session_id + int32(last_user_index) + len(last_user_text) + last_user_text)`。
命中任务表则复用并刷新 `last_used`；否则新建：

- `id`：UUID；
- `begin_at`：当前 Unix 毫秒；
- `answered`：false；
- `name`：最后一条 user 文本前 10 个字符。

任务表容量 512、空闲 2 小时淘汰（LRU）。`mark_answered(request_set_id)` 只把 `answered`
置 true，不刷新 `last_used`。只有下游把这次响应完整写出后，下一次同一轮请求才会看到
`stage: processing`；当前这次永远是 `start`。

多账户不改变 session/task；隔离靠账户池把 session 绑到一个账户。进程重启后 session_id 可复现，
但任务 id、`begin_at`、`answered` 都会丢，`stage` 回到 `start`。

## 5. 队列与重试

Qoder 没有队列状态轮询端点，也不读上游 `Retry-After` 响应头。队列只从错误体识别：

- `code == "10605"`（字符串精确比较）；
- 或 `queue.isQueued == true`；
- 或 `queue.serviceAvailable == false`。

命中则为 busy，`retry_after = queue.retryAfterSeconds > 0 ? 它 : 30` 秒。

重试只针对 busy：

- 总预算 4 分钟，从首次发送前起算；
- 最多 11 次请求（`attempt > 10` 截止）；
- 单次等待 `clamp(retry_after, 500ms, 30s)`，且不超过剩余预算；
- **重发完全相同的已签名 body 与请求头**，不刷新 `Cosy-Date`；
- 预算耗尽或等待为 0 → 返回 busy。

上游对同一请求返回 103 `Duplicate request` 时仍归为 busy，但它是终止信号：同一请求
已经入队或已经完成，再发只会继续被拒，因此客户端立即结束重试，也不把账号判成认证失败。

认证、额度、请求错误、上游错误、传输错误都不在客户端内部重试；前两者由账户池换账户。

## 6. SSE 外壳

响应是逐行的 SSE。每行以 `data:` 开头（允许前后空白），内容是**一个** JSON 对象：

```json
{"statusCodeValue":200,"body":"<inner JSON string>"}
{"statusCodeValue":200,"body":{"choices":[...]}}
```

- `statusCodeValue` 可以是数字或数字字符串。
- 缺失 `statusCodeValue`：视为 0，但**不算失败**。
- 存在但非法（如 `"abc"`）：视为 0，且**算失败**。
- `IsFailure = status 存在 && status != 200`，所以 201/0 都算失败。
- `body` 为字符串时按文本处理；为对象时保留原始字节；其它形态忽略。
- 失败时错误文本取 body：字符串且非空 → 该字符串；对象 → 严格 UTF-8；否则 → 整个外壳 JSON。
- 字符串 body 先 trim；空/空白忽略；`"[DONE]"` 表示结束；否则按 UTF-8 解析为内层 JSON。
- 裸 `data: [DONE]`（不是外壳）不是合法对象，会被丢弃；但 EOF 仍会补一个 Stop。
- 内层 JSON 必须是对象，`choices` 必须是数组且非空，否则静默跳过这一帧。
- `event:`、`id:`、`retry:`、空 `data:` 都忽略；不做多行 data 拼接。
- 单行上限 16 MiB；EOF 时没有换行的最后一行也会投递。

解析状态机：`reading → done | failed`。置为 done 后忽略后续行。收到 `[DONE]` 或 EOF 时：

- 先 flush 文本工具调用缓冲；
- 若存在“有 arguments、没有 name”的原生工具调用 → 发 Error(502)，**不发 Stop**；
- 否则发 Stop（带 finish_reason 与 usage）。

## 7. 内层 chunk 与事件

只读 `choices[0]`。每帧处理顺序：

1. 记录 `finish_reason`（最后一个非空值生效）；
2. `delta.reasoning_content` 非空 → reasoning 事件；
3. `delta.content` 非空 → 文本工具调用拆分器；
4. `delta.tool_calls[]` → 原生工具事件；
5. 顶层 `usage` 出现即覆盖（包括 `choices: []` 的尾帧）。

### 原生工具调用

上游每个工具分片带 `index`（可能缺省 0）。QoderZ 需要把“上游索引”和“输出索引”分开：

- 按上游 index 维护状态；
- `id` 只在当前为空时写入；id 迟到不回填已发出的事件；
- `arguments` 累加；
- `name` 迟到时：先 flush 文本拆分器，再分配输出索引、补 id（若上游没有）、发
  ToolStart（携带累计 arguments）；此后该 index 的每个 arguments 分片发 ToolArguments 增量；
- 同名/重复 name 忽略；
- 同时接受 `function.*` 与顶层的 `name` / `arguments`；
- 到 EOF 仍没有 name → 错误，不能作为工具发出。

### 文本工具调用

模型可能把调用写成文本：

```text
<tool_call><function=NAME><parameter=KEY>VALUE</parameter>...</function></tool_call>
<tool_call>{"name":"NAME","arguments":{...}}</tool_call>
```

拆分器必须增量处理跨 chunk 的标签，忽略代码围栏与行内反引号中的内容；完整调用发出
ToolStart（参数为紧凑 JSON），普通文本原样发出。形态像调用但解析失败、且不在代码上下文里
→ Error(502)，不能猜一个调用。

### finish_reason

- 本次出现任何工具调用（原生或文本）→ `tool_calls`；
- 否则上游 `finish_reason` 忽略大小写等于 `length` → `length`；
- 否则 `stop`。

### usage

`total == 0` 时用 `prompt + completion`；`cached` 取 `cached_tokens`，没有则取
`prompt_tokens_details.cached_tokens`；`reasoning` 取 `completion_tokens_details.reasoning_tokens`。

## 8. 错误映射

HTTP 非 2xx 时读 body（上限 1 MiB）后映射；流内错误则把外壳状态与错误 body 走同一套规则。
先做“状态不在 400..599 → 502”，再按以下优先级：

| 条件 | 分类 | 下游状态 / type / code |
| --- | --- | --- |
| code 10605 / queue.isQueued / serviceAvailable=false | busy | 503 / `server_busy` / 503 |
| code 103 / message 含 `duplicate request` | busy（终止，不再重发） | 503 / `server_busy` / 503 |
| code 105 | auth | 401 / `authentication_error` / 401 |
| code ∈ {110} ∪ [112,119] ∪ {122} | quota | 429 / `insufficient_quota` / 429 |
| code ∈ {406,416,430,80411,80412,100400–100403} | request | 400 / `invalid_request_error` / 400 |
| message 含 `error in upstream response` | upstream | 502 / `upstream_error` / 502 |
| HTTP 401/403 | auth | 401 / `authentication_error` / 401 |
| HTTP 429 或 message 含 `quota` | quota | 429 / `insufficient_quota` / 429 |
| 其它 | upstream | 502 / `upstream_error` / 502 |

数字 code 用严格整数解析；`" 10605"`、`"010605"` 不命中字符串比较。错误 body 可能带 BOM，
嵌套 JSON 的外层 code 401/403、内层 code 10605 时采用内层。传输/IO 失败 → 502；客户端断开
→ 取消（499，不写错误体）。

## 9. 固定夹具

```text
data: {"statusCodeValue":200,"body":"{\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Hello\"}}]}"}
data: {"statusCodeValue":200,"body":"{\"choices\":[{\"index\":0,\"delta\":{\"content\":\" world\"},\"finish_reason\":\"stop\"}]}"}
data: {"statusCodeValue":200,"body":"{\"choices\":[],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5,\"total_tokens\":15,\"prompt_tokens_details\":{\"cached_tokens\":2},\"completion_tokens_details\":{\"reasoning_tokens\":3}}}"}
data: {"statusCodeValue":200,"body":"[DONE]"}
```

工具分片：

```text
data: {"statusCodeValue":200,"body":"{\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_abc\",\"function\":{\"arguments\":\"{\\\"path\\\"\"}}]}}]}"}
data: {"statusCodeValue":200,"body":"{\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"name\":\"read\",\"arguments\":\":\\\"a.go\\\"}\"}}]}}]}"}
data: {"statusCodeValue":200,"body":"{\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"}\"}}]}}]}"}
```

期望：ToolStart(id=`call_abc`, name=`read`, arguments=`{"path":"a.go"}`)、ToolArguments(`}`)、Stop(`tool_calls`)。

## 10. 常见陷阱

1. 签名覆盖的是**编码后**的 body 和去掉 `/algo` 的 path；query 不参与。
2. 队列重试复用同一份签名，`Cosy-Date` 不刷新。
3. 请求体没有 `stream`；`Encode=1` 与 BodyCodec 必须配套。
4. `context_length` 依赖 messages/tools 的已写出字节数，parameters 必须最后写。
5. `statusCodeValue` 缺失不等于失败；存在但非法反而算失败。
6. `[DONE]` 只在 body 字符串里生效。
7. 只读 `choices[0]`；非流式的 `message` 不解析。
8. 工具 name 可能迟到；id 迟到不回填；上游 index 与输出 index 不能混。
9. 任何工具调用都会把 finish_reason 强制成 `tool_calls`。
10. 队列错误可能藏在 `message` 字符串里，10605 是字符串精确比较。
11. 截断的 SSE 流会被当作正常结束；只有缺 name / 残缺文本工具调用会报错。
12. `MarkAnswered` 必须等下游写完；同轮下一次请求才会看到 `processing`。
13. user 数组内容会同时写 `content` 与 `contents`；tool 消息 content 是纯字符串。
14. `cache_control` 只落在最后一条 user 的最后一个有效块。
15. 错误 body 超过 1 MiB 会被截断，可能丢失 code。
16. `X-Model-Key` 用目录 key，不是客户端请求的 model 字符串。

## 11. QoderZ 模块

| 模块 | 职责 |
| --- | --- |
| `protocol/body_codec` | BodyCodec 编解码、长度公式、固定向量 |
| `protocol/cosy_signer` | AES/RSA/MD5 签名与 19 个头 |
| `protocol/sse/line_reader` | 增量切行、16 MiB 上限、EOF 半行 |
| `qoder/chat/stream`（`stream_parser`） | 外壳 → 内层 chunk → chat_event |
| `protocol/sse/text_tool_splitter` | 文本工具调用增量拆分 |
| `qoder/chat/request` | 请求 JSON 组装、窗口选择、会话/任务 |
| `qoder/chat/client` | 发送、队列重试、首事件 peek |
| `qoder/chat/stream` | 上游字节流 → chat_event |
| `qoder/errors` | 错误分类与下游映射 |
