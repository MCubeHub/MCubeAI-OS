# AI 原生层设计

目标：AI 不是"打开一个聊天窗口"，而是系统的一等公民——Shell、文件管理器、
编辑器、任务管理器都能通过同一套会话/工具/权限接口调用本地模型，并且离线可用。

## 1. 进程与线程模型

| 组件 | 位置 | 说明 |
| --- | --- | --- |
| `aiosd` | 用户态守护进程 | 常驻，监听 `/run/aiosd.sock`；管理模型、会话、记忆、工具、审计 |
| `ai-worker` 线程池 | aiosd 内 | 默认 `min(4, ncpu-1)` 个推理线程，绑定 CPU 配额 |
| `ai-sandbox` | 子进程 | 工具执行沙箱：独立 UID、路径白名单、系统调用白名单 |
| `ai` | CLI 客户端 | 单轮/多轮/exec/工具/记忆/模型管理 |
| `ai-chat` | GUI 客户端 | 流式渲染、Markdown、附件、任务面板 |

`aiosd` 启动流程：读取 `/etc/aios/config.json` → 打开审计日志 → 扫描模型目录 →
按需加载默认模型（懒加载，首次请求时预热）→ 注册内置工具 → 监听 socket →
注册 `ai_*` 事件通道 → 进入事件循环。

## 2. 推理核心（Inference Core）

### 2.1 模型加载

- 支持 GGUF v3：解析 header、metadata KV、tensor info、对齐与数据类型。
- 量化：Q4_0/Q4_K/Q5_K/Q8_0（首版），反量化到 f32 的 block 级内核。
- 权重使用 `mmap` 映射模型文件，按层惰性触碰；内存紧张时按层换出
  （`madvise` 语义由存储管理器实现，首版常驻）。
- 模型目录：`/usr/share/aios/models/*.gguf` + 用户目录；维护
  `model_id → {path, size, quant, context_len, tokenizer}` 注册表。
- 校验：文件大小、magic、tensor 偏移边界；损坏模型拒绝加载并写审计日志。

### 2.2 计算图与算子

首版实现 Llama 架构前向：

```
embedding → for each layer:
    RMSNorm → QKV 投影 → RoPE → KV cache 追加 →
    grouped-query attention (causal) → 输出投影 → 残差 →
    RMSNorm → FFN(SwiGLU: gate/up/down) → 残差
→ final RMSNorm → lm_head → logits
```

- 算子：matmul（含量化权重）、softmax、RMSNorm、RoPE、SiLU、残差、argmax。
- CPU 优化：标量参考实现 + AVX2/FMA 路径（运行时 CPUID 选择），
  多线程按行切分；后续 NEON/AArch64。
- GPU 后端接口：`struct ai_backend_ops { init, upload, matmul, attention, sync }`，
  首版只有 CPU 后端，Vulkan/CUDA 作为可选插件。
- KV cache：按层 `[n_kv_heads, seq_len, head_dim]`，f16 存储；
  预分配 `context_len` 上限，超限触发上下文截断/摘要。

### 2.3 Tokenizer 与采样

- BPE：从 GGUF metadata 读取 vocab/merges，支持 byte-level 回退与特殊 token。
- 流式 detoken：处理跨 token 的 UTF-8 多字节边界。
- 采样：greedy、temperature、top-k、top-p、repetition penalty、max_tokens、
  stop strings；随机源为每会话独立 PRNG（可复现实验）。
- 取消：每个生成 step 检查取消标志（原子变量），保证 < 1 token 延迟退出。

### 2.4 推理任务与调度

```
queued → preparing(context) → running → tool_wait → running → done
                                     ↘ cancelled / failed
```

- 优先级：交互式对话 > 前台工具任务 > 后台索引/摘要。
- 前台请求可抢占后台任务：后台在 layer 边界检查抢占，保存 KV 索引。
- 并发上限：`max_concurrent`（默认 1 个大模型 + 1 个小模型），
  超出排队；CPU 配额通过线程数与 nice 值控制。
- 每任务统计：prompt tokens、generated tokens、tok/s、首 token 延迟、峰值 RSS。

## 3. 上下文管理器

- 会话 = `{id, model, messages[], kv_state, token_usage, created_at}`。
- 消息角色：system/user/assistant/tool；tool 消息带 `tool_call_id`。
- 超长策略：
  1. 保留 system + 最近 N 轮 + 与本轮相关的检索片段；
  2. 早期消息生成滚动摘要（调用小模型或规则摘要）；
  3. 摘要替换原文并在消息上标记 `compressed=true`。
- KV 快照：保存每层已计算 token 数、采样状态（RNG、penalty 历史）、
  消息偏移；恢复时从快照点继续，避免重跑 prompt。
- 上下文切换与调度联动：任务被抢占 → `context_snapshot(task)`；
  恢复 → `context_restore(task)`；快照内存计入任务 RSS 配额。
- 多模型隔离：每个模型独立 KV 池与会话表，句柄带 model_id 校验。

## 4. 记忆管理器（RAM 短记忆）

- 分级：
  - `session`：当前会话变量、最近 8 轮原始消息、工具调用中间结果；
  - `project`：当前项目目录的索引摘要（落盘）；
  - `user`：长期偏好（落盘 + 向量索引）。
- 块结构：`{id, kind, text, tokens, ts, hits, embedding?, ttl}`。
- 淘汰：LRU-K（K=2），超容量时先换出 `session` 中低命中块到存储管理器，
  再淘汰无引用块；写入/读取/删除均更新访问计数。
- 接口：`mem_put/mem_get/mem_search/mem_forget/mem_stats`。
- 过期：TTL + 会话结束归档；用户可 `ai memory clear --session`。
- 隐私：记忆块带 `owner_uid`，跨用户不可见；敏感路径内容默认不写入记忆。

## 5. 存储管理器（长期记忆 + 向量库）

目录：

```
/var/lib/aios/
├── sessions/<session-id>.jsonl      # 消息流（追加写，崩溃可恢复）
├── profiles/<uid>.json              # 用户偏好
├── index/<collection>/              # 向量索引
│   ├── meta.json                    # 维度、模型、条目数
│   ├── vectors.bin                  # f32 连续存储
│   └── ids.bin                      # 条目 id 表
└── audit/audit-YYYYMMDD.jsonl       # AI 操作审计
```

- 向量检索：首版暴力余弦相似度（向量数 < 10 万），后续 HNSW。
- 文档入库：分块（按段落/固定 token）、embedding（本地小模型）、
  记录来源路径与 mtime，文件变更时增量重建。
- 会话恢复：启动时扫描最近会话，`ai sessions` 可列出并 `--resume`。
- 原子性：索引写入采用临时文件 + rename；`meta.json` 记录版本与校验和。

## 6. 工具管理器（Function Calling）

工具描述（JSON Schema）：

```json
{
  "name": "fs.read",
  "description": "Read a UTF-8 text file",
  "parameters": {
    "type": "object",
    "properties": {
      "path": {"type": "string", "maxLength": 4096},
      "max_bytes": {"type": "integer", "maximum": 1048576}
    },
    "required": ["path"]
  },
  "permission": "read",
  "impl": "builtin:fs_read"
}
```

内置工具：

| 类别 | 工具 | 默认权限 |
| --- | --- | --- |
| 文件 | `fs.read` `fs.write` `fs.list` `fs.search` | read / confirm |
| 进程 | `proc.list` `proc.kill` `proc.exec` | read / confirm / sandbox |
| 系统 | `sys.info` `sys.time` `sys.disk` | allow |
| 网络 | `net.http` `net.search` | confirm（离线模式禁用） |
| 计算 | `calc.eval` `text.transform` `code.run` | allow / sandbox |

循环：模型输出 `tool_call` → 校验 JSON Schema 与权限 → 审计 →
执行（内置函数 / 沙箱子进程）→ 结果截断与注入隔离标记 → 回填上下文 →
继续生成。限制：每次请求最多 8 次调用、单次结果 64 KiB、总耗时 120 s，
任何异常转为 `tool_result{error}` 而不是让推理线程崩溃。

## 7. 访问管理器与审计

- 身份：每个 Agent 绑定 `uid` + capability 集（`FS_READ`、`FS_WRITE`、
  `PROC_READ`、`NET_ACCESS`、`EXEC`）。默认只能访问 `~` 与项目目录。
- 路径检查：规范化后做前缀匹配，拒绝 `..` 逃逸、符号链接逃逸、设备文件。
- 敏感目录：`~/.ssh`、`/etc/aios/keys`、浏览器配置默认屏蔽（policy 文件可改）。
- 确认流程：`confirm` 级工具在执行前向 UI 发 `tool_confirm` 事件，
  用户同意后带一次性 token 执行；超时 60 s 自动拒绝。
- 审计事件：`{ts, uid, session, tool, args_digest, decision, result, duration}`，
  只读接口 `ai audit --tail`，支持按会话导出。
- 提示注入：工具结果包裹 `<untrusted>...</untrusted>` 并在 UI 标记；
  检测到"忽略之前指令"等模式时提高风险等级并要求确认后续写操作。

## 8. 系统调用与内核钩子

| 号 | 名称 | 说明 |
| --- | --- | --- |
| 0x400 | `ai_session_create` | 创建会话，返回 fd |
| 0x401 | `ai_session_send` | 提交 prompt（异步） |
| 0x402 | `ai_stream_read` | 读取 token/tool 事件 |
| 0x403 | `ai_task_cancel` | 取消任务 |
| 0x404 | `ai_tool_register` | 注册工具（仅特权） |
| 0x405 | `ai_status` | 查询模型/任务/资源状态 |
| 0x406 | `ai_memory_*` | 记忆读写/检索/清理 |
| 0x407 | `ai_authorize` | 提交用户授权结果 |
| 0x408 | `ai_audit_read` | 读取审计事件 |

内核侧只做：fd/会话句柄管理、共享内存环、事件通知（`poll`/`epoll` 兼容）、
权限与审计挂钩。真正的推理在 `aiosd`，内核不解析模型文件。

## 9. 守护进程协议与 CLI

Socket：`AF_UNIX, SOCK_SEQPACKET`，帧 = `[u32 len][u8 type][payload]`，
payload 为 JSON（首版）或 CBOR（带宽优化）。

请求：`hello / model.load / model.list / session.new / session.send /
session.cancel / tool.list / memory.query / status / config.get|set`。
响应事件：`token / tool_call / tool_confirm / tool_result / done / error / stats`。

CLI：

```
ai "解释一下这个错误"                  # 单轮
ai chat                                # 多轮交互
ai exec "找出占用内存最多的进程"        # 非交互，允许工具
ai model list|load|unload
ai tool list
ai memory show|clear|search
ai sessions list|resume|rm
ai audit --tail 20
ai status                              # tok/s、RSS、KV cache、队列
```

Shell 集成：`? 命令` 将整行交给 AI；AI 返回命令时高亮并等待确认；
`history` 内容进入会话记忆（可在配置中关闭）。

## 10. 桌面客户端

- 会话侧栏、消息气泡、流式 Markdown（标题/列表/代码块/表格/粗体）、
  复制、重新生成、停止生成、附件（图片预留视觉接口）。
- 参数面板：模型、温度、top-p、最大长度、系统提示。
- 任务面板：后台 Agent 列表、状态、进度、取消、工具调用时间线（可展开）。
- 全局快捷键 `Super+Space` 唤起侧边助手；托盘图标菜单。
- 系统集成：文件管理器"用 AI 摘要"、编辑器补全/解释/重构、通知摘要、
  崩溃界面生成中文故障说明（读取 `dmesg` 与审计日志片段）。

## 11. 配置与资源

```json
{
  "model": {"default": "qwen2.5-1.5b-q4_k_m", "dir": "/usr/share/aios/models"},
  "inference": {"threads": 4, "max_concurrent": 2, "context_len": 4096},
  "memory": {"session_blocks": 512, "lru_k": 2, "max_mb": 256},
  "tools": {"allow": ["fs.read", "proc.list", "sys.*"], "confirm": ["fs.write", "proc.exec"]},
  "network": {"offline": true, "api_base": "", "api_key": ""},
  "ui": {"language": "zh-CN", "theme": "dark"}
}
```

资源监控：`aios` 在任务管理器中单独分组，显示推理 CPU、RSS、KV cache、
tok/s、队列长度；超过配额时降级为小模型或排队，而不是 OOM。

## 12. 里程碑验收

- 模型：Qwen2.5-1.5B-Instruct Q4_K_M 在 QEMU（4 vCPU）上 > 3 tok/s。
- 对话：离线单轮问答语义正确；多轮保持 8 轮上下文；停止按钮 < 200 ms 生效。
- 工具：`ai exec "列出 /home 下最大的文件"` 正确调用 `fs.list/stat` 并给出结果。
- 安全：访问 `~/.ssh` 被拒绝；写文件需确认；审计日志包含完整调用链。
- 恢复：关闭再启动 `aiosd` 后 `ai sessions resume <id>` 能继续上下文。
