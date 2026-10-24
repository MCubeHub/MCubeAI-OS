#!/usr/bin/env python3
"""Generate the built-in tiny GGUF model loaded by `aiosd`.

The file is a real GGUF v3 container with llama-shaped tensors. The weights
encode a byte-level bigram language model trained (at generation time) on the
small corpus below:

    token_embd.weight   [n_embd, n_vocab]   identity: token i -> one-hot e_i
    output_norm.weight  [n_embd]            ones
    output.weight       [n_vocab, n_embd]   log P(next byte | current byte)
    blk.0.attn_norm.weight / blk.0.ffn_norm.weight   ones

Attention and FFN weights are intentionally omitted; the engine treats an
absent weight as zero, so the deterministic path is
    embedding -> RMSNorm -> lm_head -> sampler
while still exercising the real GGUF loader, tokenizer, KV cache setup,
sampling and streaming code paths.

The lm_head rows are divided by sqrt(n_embd) to compensate for the RMSNorm
gain the engine applies before the projection, so the logits match the log
probabilities the corpus produced.
"""

import math
import os
import struct
import sys

GGUF_MAGIC = b'GGUF'
GGUF_VERSION = 3

# gguf metadata value types
T_UINT32 = 4
T_FLOAT32 = 6
T_STRING = 8
T_ARRAY = 9
GGML_F32 = 0

CORPUS = (
    "MCubeAIOS is a small x86_64 operating system written from scratch. "
    "It boots through UEFI, brings up a framebuffer desktop, and runs local "
    "language models as a first class system service.\n"
    "The ai layer is not an app: aiosd is a daemon, the kernel provides a "
    "message channel, and every session keeps its own history and memory.\n"
    "Type ai hello to talk to the model, or ai status to inspect the daemon. "
    "Tools can list files, read a file, report the time, or compute a sum. "
    "Every tool call is audited and sensitive actions ask for confirmation.\n"
    "The model here is tiny by design. It predicts one byte at a time, so the "
    "text it streams back looks like the corpus it was trained on.\n"
)


def write_str(buf, data):
    """GGUF strings are u64 length + raw bytes (no NUL terminator)."""
    buf += struct.pack('<Q', len(data))
    buf += data


def write_kv_str(buf, key, value):
    write_str(buf, key)
    buf += struct.pack('<I', T_STRING)
    write_str(buf, value.encode('utf-8'))


def write_kv_u32(buf, key, value):
    write_str(buf, key)
    buf += struct.pack('<I', T_UINT32)
    buf += struct.pack('<I', value)


def write_kv_f32(buf, key, value):
    write_str(buf, key)
    buf += struct.pack('<I', T_FLOAT32)
    buf += struct.pack('<f', value)


def build_bigram(vocab, corpus):
    """log P(next | prev) with add-one smoothing."""
    counts = [[0] * vocab for _ in range(vocab)]
    data = corpus.encode('utf-8')
    for i in range(len(data) - 1):
        counts[data[i]][data[i + 1]] += 1
    total = [sum(row) for row in counts]

    table = []
    for prev in range(vocab):
        denom = total[prev] + vocab
        row = [math.log((counts[prev][nxt] + 1) / denom) for nxt in range(vocab)]
        table.append(row)
    return table


def main():
    if len(sys.argv) != 2:
        sys.exit('usage: gen_tiny_model.py <output.gguf>')
    out = sys.argv[1]
    parent = os.path.dirname(out)
    if parent:
        os.makedirs(parent, exist_ok=True)

    vocab = 256
    n_embd = 256
    n_layer = 1
    n_head = 4
    n_kv_head = 4
    n_ff = 256
    n_ctx = 512
    align = 32

    # --- metadata -----------------------------------------------------
    meta = bytearray()
    write_kv_str(meta, b'general.architecture', 'llama')
    write_kv_str(meta, b'general.name', 'mcube-tiny-bigram')
    write_kv_u32(meta, b'llama.vocab_size', vocab)
    write_kv_u32(meta, b'llama.embedding_length', n_embd)
    write_kv_u32(meta, b'llama.block_count', n_layer)
    write_kv_u32(meta, b'llama.attention.head_count', n_head)
    write_kv_u32(meta, b'llama.attention.head_count_kv', n_kv_head)
    write_kv_u32(meta, b'llama.feed_forward_length', n_ff)
    write_kv_u32(meta, b'llama.context_length', n_ctx)
    write_kv_u32(meta, b'llama.rope.dimension_count', n_embd // n_head)
    write_kv_f32(meta, b'llama.rope.freq_base', 10000.0)
    write_kv_f32(meta, b'llama.attention.layer_norm_rms_epsilon', 1e-5)

    # tokenizer: 256 raw byte tokens
    write_str(meta, b'tokenizer.ggml.model')
    meta += struct.pack('<I', T_STRING)
    write_str(meta, b'llama')

    write_str(meta, b'tokenizer.ggml.tokens')
    meta += struct.pack('<I', T_ARRAY)
    meta += struct.pack('<I', T_STRING)
    meta += struct.pack('<Q', vocab)
    for i in range(vocab):
        write_str(meta, bytes([i]))

    kv_count = 14  # 12 llama/general entries + tokenizer model + token list

    # --- tensors ------------------------------------------------------
    tensors = []

    emb = [0.0] * (n_embd * vocab)
    for i in range(vocab):
        emb[i * n_embd + i] = 1.0
    tensors.append((b'token_embd.weight', [n_embd, vocab], emb))

    tensors.append((b'output_norm.weight', [n_embd], [1.0] * n_embd))

    bigram = build_bigram(vocab, CORPUS)
    gain = math.sqrt(n_embd)
    lm = [0.0] * (vocab * n_embd)
    for nxt in range(vocab):
        row = bigram[nxt]
        for prev in range(vocab):
            lm[nxt * n_embd + prev] = row[prev] / gain
    tensors.append((b'output.weight', [vocab, n_embd], lm))

    tensors.append((b'blk.0.attn_norm.weight', [n_embd], [1.0] * n_embd))
    tensors.append((b'blk.0.ffn_norm.weight', [n_embd], [1.0] * n_embd))

    info = bytearray()
    payload = bytearray()
    for name, dims, data in tensors:
        write_str(info, name)
        info += struct.pack('<I', len(dims))
        for d in dims:
            info += struct.pack('<Q', d)
        info += struct.pack('<I', GGML_F32)
        info += struct.pack('<Q', len(payload))
        payload += struct.pack('<%df' % len(data), *data)
        pad = (-len(payload)) % align
        payload += b'\x00' * pad

    file = bytearray()
    file += GGUF_MAGIC
    file += struct.pack('<I', GGUF_VERSION)
    file += struct.pack('<Q', len(tensors))
    file += struct.pack('<Q', kv_count)
    file += meta
    file += info
    file += b'\x00' * ((-len(file)) % align)
    file += payload

    with open(out, 'wb') as f:
        f.write(file)

    print('wrote %s (%d bytes, %d tensors, %d kv)'
          % (out, len(file), len(tensors), kv_count))


if __name__ == '__main__':
    main()
