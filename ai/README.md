# AI native layer (M9 baseline)

Everything that can fail lives in user space: the kernel only moves fixed-size
messages. `aiosd` is a resident daemon; `ai` is the CLI client.

| File | Role |
| --- | --- |
| `../kernel/ai/ai_chan.c` | session channels (two anonymous pipes), daemon registration, audit ring |
| `../user/lib/ai.c` | client wrappers, full-message `send`/`recv` |
| `../user/programs/aiosd.c` | daemon: model load, request loop, confirm/stream/session |
| `../user/programs/ai.c` | CLI: single turn, chat REPL, exec, status/tools/model/audit/memory |
| `ai_engine.c` | GGUF v3 parser, llama-style forward pass, sampler |
| `ai_agent.c` | rule-based intent router, tools, per-session memory |

## Channel and protocol

`SYS_AI_CONNECT` returns a session id and installs two pipes as fd 3 (requests)
and fd 4 (responses). `SYS_AI_ACCEPT` on the daemon side blocks until a client
connects; the returned pid is informational. Messages are 528-byte `ai_msg_t`
records (`include/mcube/ai.h`): `type`, `session`, `flags`, `len`, 512-byte
`data`. Large payloads are chunked, and the first/last chunk is marked in
`flags`. The kernel pipe path returns at most 512 bytes per `read()`, so the
client library gathers exactly one message before decoding it.

Requests: `HELLO CHAT STATUS TOOL_LIST MEMORY AUDIT MODEL CANCEL CONFIRM`.
Events: `TOKEN INFO TOOL CONFIRM STATS DONE ERROR`. `CONFIRM` is a client
response for tools that need approval; a denied call is audited and skipped.

## Model

`tools/gen_tiny_model.py` writes `build/tiny.gguf`: a real GGUF v3 file with
llama metadata, 256 byte tokens, F32 tensors and a bigram `output.weight`
trained on a small built-in corpus. The engine also runs a real single-layer
llama block when attention/FFN tensors are present; the generated model omits
them so the deterministic path is embedding -> RMSNorm -> lm_head -> sampler.
Only F32 tensors and a 256-entry byte vocabulary are accepted for now.

The ESP copy is `/models/tiny.ggu`, an 8.3 short name, because the FAT32
driver does not parse LFN entries yet. Regenerate with `make image` (the file
is built automatically) or `make build/tiny.gguf`.

## Tools

`sys.info`, `sys.time`, `sys.disk`, `proc.list`, `fs.list`, `fs.read`,
`fs.write` (confirmation required), `fs.search`, `calc.eval`. The router is
rule based (`ai_route`) because the tiny model cannot emit reliable function
calls. Tool output is capped at 2 KiB per call, appended to the session, and
fed back to the model for the final answer.

Every call and denial is written to the kernel audit ring via `SYS_AI_LOG`;
only the registered daemon may append. `ai audit` reads the ring back.

## Usage

```
ai "system info"          # route to sys.info, then continue inference
ai chat                   # multi-turn REPL
ai exec --yes "..."       # non-interactive; auto-approves fs.write
ai status | tools | model | audit | memory [clear]
```

## Not implemented yet

Long-file-name lookup, Q4/Q5/Q8 dequantization, BPE/SentencePiece, SIMD
matmul, context summarization and KV snapshots, LRU-K/vector memory, session
persistence, sandboxed tool processes, Shell `?` integration, desktop client.
