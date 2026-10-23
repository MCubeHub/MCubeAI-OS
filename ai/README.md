# AI 原生层（M9）

`aiosd` 与推理运行时以**用户态**实现，通过本地 socket 与 `ai_*` 系统调用
接入内核。计划目录：

```
ai/
├── aiosd/       守护进程：事件循环、会话、模型注册表、配置
├── runtime/     GGUF 解析、量化反量化、算子、KV cache、采样、BPE
├── context/     上下文窗口、截断/摘要、KV 快照恢复
├── memory/      短期记忆 LRU-K、长期存储、向量索引
├── tools/       工具注册表、JSON Schema 校验、内置工具
├── access/      权限策略、审计、沙箱调用
└── cli/         ai 命令（chat/exec/model/tool/memory/audit）
```

设计细节见 [`docs/ai-native.md`](../docs/ai-native.md)。
