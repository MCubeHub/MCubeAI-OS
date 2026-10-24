/* GGUF loader + Llama-style forward pass + sampler (M9).
 *
 * Freestanding: no libm, no SSE, no hosted runtime. The transcendental
 * helpers below are implemented with x87 and small polynomial kernels.
 */

#include <mcube/libc.h>
#include <mcube/ai.h>
#include "ai_runtime.h"

/* ---------------------------------------------------------------------
 * float helpers
 * ------------------------------------------------------------------- */

static float f_sqrt(float x) {
    float r;
    if (x <= 0.0f) return 0.0f;
    __asm__ __volatile__("fsqrt" : "=t"(r) : "0"(x));
    return r;
}

#define F_LN2 0.6931471805599453f
#define F_INV_LN2 1.4426950408889634f

/* ln(x) for x > 0: range reduction to [1,2) then 2*atanh series. */
static float f_log(float x) {
    if (x <= 0.0f) return -1e30f;
    int e = 0;
    while (x >= 2.0f) { x *= 0.5f; e++; }
    while (x < 1.0f) { x *= 2.0f; e--; }

    float z = (x - 1.0f) / (x + 1.0f);
    float z2 = z * z;
    float term = z;
    float sum = 0.0f;
    for (int i = 1; i <= 21; i += 2) {
        sum += term / (float)i;
        term *= z2;
    }
    return 2.0f * sum + (float)e * F_LN2;
}

/* exp(x) = 2^k * exp(r), r in [-ln2/2, ln2/2]. */
static float f_exp(float x) {
    if (x < -60.0f) return 0.0f;
    if (x > 60.0f) x = 60.0f;

    float kf = x * F_INV_LN2;
    int k = (int)(kf >= 0.0f ? kf + 0.5f : kf - 0.5f);
    float r = x - (float)k * F_LN2;

    float term = 1.0f;
    float sum = 1.0f;
    for (int i = 1; i <= 12; i++) {
        term *= r / (float)i;
        sum += term;
    }

    while (k > 0) { sum *= 2.0f; k--; }
    while (k < 0) { sum *= 0.5f; k++; }
    return sum;
}

static float f_pow(float base, float e) {
    if (base <= 0.0f) return 0.0f;
    return f_exp(e * f_log(base));
}

/* ---------------------------------------------------------------------
 * little-endian reader over an in-memory file
 * ------------------------------------------------------------------- */

typedef struct {
    const u8 *p;
    u64 size;
    u64 off;
    int bad;
} rd_t;

static int rd_need(rd_t *r, u64 n) {
    if (r->bad) return 0;
    if (r->off > r->size || n > r->size - r->off) { r->bad = 1; return 0; }
    return 1;
}

static u32 rd_u32(rd_t *r) {
    if (!rd_need(r, 4)) return 0;
    const u8 *p = r->p + r->off;
    r->off += 4;
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u64 rd_u64(rd_t *r) {
    if (!rd_need(r, 8)) return 0;
    const u8 *p = r->p + r->off;
    r->off += 8;
    u64 v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | (u64)p[i];
    return v;
}

static float rd_f32(rd_t *r) {
    u32 bits = rd_u32(r);
    float f = 0.0f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

static int rd_bytes(rd_t *r, const u8 **data, u64 *len) {
    u64 n = rd_u64(r);
    if (!rd_need(r, n)) return -1;
    *data = r->p + r->off;
    *len = n;
    r->off += n;
    return 0;
}

static int rd_skip(rd_t *r, u32 type) {
    u64 n;
    switch (type) {
    case 0: case 1: case 7: n = 1; break;
    case 2: case 3: n = 2; break;
    case 4: case 5: case 6: n = 4; break;
    case 10: case 11: case 12: n = 8; break;
    case 8: {
        u64 len;
        const u8 *ignored;
        return rd_bytes(r, &ignored, &len);
    }
    case 9: {
        u32 elem = rd_u32(r);
        u64 count = rd_u64(r);
        for (u64 i = 0; i < count; i++) {
            if (rd_skip(r, elem) != 0) return -1;
        }
        return r->bad ? -1 : 0;
    }
    default:
        return -1;
    }
    if (!rd_need(r, n)) return -1;
    r->off += n;
    return 0;
}

static int key_eq(const u8 *k, u64 len, const char *s) {
    u64 n = strlen(s);
    return n == len && memcmp(k, s, (size_t)n) == 0;
}

/* ---------------------------------------------------------------------
 * model loading
 * ------------------------------------------------------------------- */

/* Negative stage codes are surfaced by aiosd so a broken image is easy to
 * diagnose from the serial console. */
static int load_file(const char *path, u8 **out, u64 *out_size) {
    mcube_stat_t st;
    if (stat(path, &st) != 0) return -1;
    if (st.size < 32) return -2;

    u8 *buf = malloc((size_t)st.size);
    if (!buf) return -3;

    int fd = open(path, VFS_O_RDONLY);
    if (fd < 0) return -4;

    u64 got = 0;
    while (got < st.size) {
        long n = read(fd, buf + got, (size_t)(st.size - got));
        if (n <= 0) break;
        got += (u64)n;
    }
    close(fd);

    if (got != st.size) return -5;
    *out = buf;
    *out_size = st.size;
    return 0;
}

const float *ai_tensor(const ai_model_t *m, const char *name) {
    if (!m || !m->ok) return NULL;
    for (u32 i = 0; i < m->n_tensors; i++) {
        const ai_tensor_t *t = &m->tensors[i];
        if (strcmp(t->name, name) != 0) continue;
        if (t->type != 0) return NULL;   /* only F32 is supported */

        u64 count = 1;
        for (u32 d = 0; d < t->n_dims && d < 4; d++) count *= t->dims[d];

        u64 off = m->data_off + t->offset;
        if (off > m->size) return NULL;
        if (count > (m->size - off) / sizeof(float)) return NULL;
        return (const float *)(m->buf + off);
    }
    return NULL;
}

void ai_model_close(ai_model_t *m) {
    if (!m) return;
    free(m->buf);
    memset(m, 0, sizeof(*m));
}

int ai_model_open(ai_model_t *m, const char *path) {
    if (!m || !path) return -1;
    memset(m, 0, sizeof(*m));

    u8 *buf = NULL;
    u64 size = 0;
    int rc = load_file(path, &buf, &size);
    if (rc != 0) return rc;

    rd_t r = { buf, size, 0, 0 };

    u32 magic = rd_u32(&r);
    u32 version = rd_u32(&r);
    u64 n_tensors = rd_u64(&r);
    u64 n_kv = rd_u64(&r);

    if (r.bad || magic != 0x46554747u /* "GGUF" */ ||
        version != 3 || n_tensors == 0 || n_tensors > AI_MAX_TENSORS) {
        free(buf);
        return -10;
    }

    char name[AI_NAME_MAX];
    memset(name, 0, sizeof(name));

    for (u64 i = 0; i < n_kv; i++) {
        const u8 *k = NULL;
        u64 klen = 0;
        if (rd_bytes(&r, &k, &klen) != 0) { free(buf); return -11; }
        u32 vt = rd_u32(&r);

        if (key_eq(k, klen, "general.name") && vt == 8) {
            const u8 *v = NULL;
            u64 vlen = 0;
            if (rd_bytes(&r, &v, &vlen) != 0) { free(buf); return -11; }
            u64 n = vlen < sizeof(name) - 1 ? vlen : sizeof(name) - 1;
            memcpy(name, v, (size_t)n);
        } else if (key_eq(k, klen, "llama.embedding_length") && vt == 4) {
            m->n_embd = rd_u32(&r);
        } else if (key_eq(k, klen, "llama.block_count") && vt == 4) {
            m->n_layer = rd_u32(&r);
        } else if (key_eq(k, klen, "llama.attention.head_count") && vt == 4) {
            m->n_head = rd_u32(&r);
        } else if (key_eq(k, klen, "llama.attention.head_count_kv") && vt == 4) {
            m->n_head_kv = rd_u32(&r);
        } else if (key_eq(k, klen, "llama.feed_forward_length") && vt == 4) {
            m->n_ff = rd_u32(&r);
        } else if (key_eq(k, klen, "llama.context_length") && vt == 4) {
            m->n_ctx = rd_u32(&r);
        } else if (key_eq(k, klen, "llama.vocab_size") && vt == 4) {
            m->n_vocab = rd_u32(&r);
        } else if (key_eq(k, klen, "llama.rope.freq_base") && vt == 6) {
            m->rope_freq_base = rd_f32(&r);
        } else if (key_eq(k, klen,
                          "llama.attention.layer_norm_rms_epsilon") &&
                   vt == 6) {
            m->rms_eps = rd_f32(&r);
        } else if (rd_skip(&r, vt) != 0) {
            free(buf);
            return -12;
        }
        if (r.bad) { free(buf); return -12; }
    }

    for (u64 i = 0; i < n_tensors; i++) {
        const u8 *tn = NULL;
        u64 tnlen = 0;
        if (rd_bytes(&r, &tn, &tnlen) != 0) { free(buf); return -13; }

        ai_tensor_t *t = &m->tensors[i];
        u64 n = tnlen < AI_NAME_MAX - 1 ? tnlen : AI_NAME_MAX - 1;
        memcpy(t->name, tn, (size_t)n);
        t->name[n] = 0;

        t->n_dims = rd_u32(&r);
        if (t->n_dims > 4) { free(buf); return -13; }
        for (u32 d = 0; d < t->n_dims; d++) t->dims[d] = rd_u64(&r);
        for (u32 d = t->n_dims; d < 4; d++) t->dims[d] = 1;
        t->type = rd_u32(&r);
        t->offset = rd_u64(&r);
        if (r.bad) { free(buf); return -13; }
    }
    m->n_tensors = (u32)n_tensors;

    m->data_off = (r.off + 31u) & ~31ULL;
    if (m->data_off > size) { free(buf); return -14; }

    /* hparams fall back to llama defaults if the file omitted them */
    if (m->n_embd == 0) m->n_embd = 256;
    if (m->n_layer == 0) m->n_layer = 1;
    if (m->n_head == 0) m->n_head = 4;
    if (m->n_head_kv == 0) m->n_head_kv = m->n_head;
    if (m->n_ff == 0) m->n_ff = m->n_embd;
    if (m->n_ctx == 0) m->n_ctx = 512;
    if (m->n_vocab == 0) m->n_vocab = 256;
    if (m->rope_freq_base == 0.0f) m->rope_freq_base = 10000.0f;
    if (m->rms_eps == 0.0f) m->rms_eps = 1e-5f;

    if (m->n_vocab != 256) {   /* byte-level tokenizer only */
        free(buf);
        return -15;
    }

    m->buf = buf;
    m->size = size;
    m->ok = 1;

    m->has_attn = ai_tensor(m, "blk.0.attn_q.weight") &&
                  ai_tensor(m, "blk.0.attn_k.weight") &&
                  ai_tensor(m, "blk.0.attn_v.weight") &&
                  ai_tensor(m, "blk.0.attn_output.weight");
    m->has_ffn = ai_tensor(m, "blk.0.ffn_gate.weight") &&
                 ai_tensor(m, "blk.0.ffn_up.weight") &&
                 ai_tensor(m, "blk.0.ffn_down.weight");

    snprintf(m->desc, sizeof(m->desc),
             "%s vocab=%u embd=%u layer=%u ctx=%u",
             name[0] ? name : "tiny", m->n_vocab, m->n_embd, m->n_layer,
             m->n_ctx);
    return 0;
}

/* ---------------------------------------------------------------------
 * tokenizer (byte level)
 * ------------------------------------------------------------------- */

int ai_encode(const ai_model_t *m, const char *text, int *out, int max) {
    (void)m;
    if (!text || !out) return -1;
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (n >= max) break;
        out[n++] = (int)*p;
    }
    return n;
}

char ai_decode_byte(const ai_model_t *m, int token) {
    (void)m;
    if (token <= 0 || token > 255) return 0;
    if (token == '\n' || token == '\t') return (char)token;
    if (token < 32 || token > 126) return '.';
    return (char)token;
}

/* ---------------------------------------------------------------------
 * forward pass
 * ------------------------------------------------------------------- */

typedef struct {
    ai_model_t *m;
    const float *tok_emb;
    const float *out_norm;
    const float *out_w;
    const float *attn_norm[4];
    const float *ffn_norm[4];
    const float *wq[4], *wk[4], *wv[4], *wo[4];
    const float *wgate[4], *wup[4], *wdown[4];
    float *kcache;
    float *vcache;
    float *x, *h, *q, *k, *v, *att, *gate, *up;
    float *logits;
} fwd_t;

static void matmul(const float *w, const float *x, float *y, int rows, int cols) {
    for (int r = 0; r < rows; r++) {
        const float *row = w + (u64)r * (u64)cols;
        float acc = 0.0f;
        for (int c = 0; c < cols; c++) acc += row[c] * x[c];
        y[r] = acc;
    }
}

static void rmsnorm(const float *x, const float *w, float *y, int n, float eps) {
    float ss = 0.0f;
    for (int i = 0; i < n; i++) ss += x[i] * x[i];
    float scale = 1.0f / f_sqrt(ss / (float)n + eps);
    for (int i = 0; i < n; i++) y[i] = x[i] * scale * (w ? w[i] : 1.0f);
}

static void rope(float *vec, int n_embd, int n_head, int pos, float base) {
    int hd = n_embd / n_head;
    if (hd < 2) return;

    /* theta_i = base^(-i/hd); step between pairs is base^(-2/hd). */
    float step = f_pow(base, -2.0f / (float)hd);
    for (int h = 0; h < n_head; h++) {
        float *p = vec + h * hd;
        float theta = 1.0f;
        for (int i = 0; i < hd; i += 2) {
            float angle = (float)pos * theta;
            /* Range-reduce to [-pi, pi] before the Taylor series. */
            while (angle > 3.14159265f) angle -= 6.28318531f;
            while (angle < -3.14159265f) angle += 6.28318531f;
            float a2 = angle * angle;
            float cs = 1.0f - a2 / 2.0f + a2 * a2 / 24.0f -
                       a2 * a2 * a2 / 720.0f + a2 * a2 * a2 * a2 / 40320.0f;
            float sn = angle - angle * a2 / 6.0f + angle * a2 * a2 / 120.0f -
                       angle * a2 * a2 * a2 / 5040.0f;
            float x0 = p[i], x1 = p[i + 1];
            p[i]     = x0 * cs - x1 * sn;
            p[i + 1] = x0 * sn + x1 * cs;
            theta *= step;
        }
    }
}

static void attention(fwd_t *f, int layer, int pos, int n_embd, int n_head,
                      int n_kv) {
    int hd = n_embd / n_head;
    int kv_dim = hd * n_kv;
    float *kc = f->kcache + (u64)layer * (u64)f->m->n_ctx * (u64)kv_dim;
    float *vc = f->vcache + (u64)layer * (u64)f->m->n_ctx * (u64)kv_dim;

    for (int i = 0; i < kv_dim; i++) {
        kc[(u64)pos * kv_dim + i] = f->k[i];
        vc[(u64)pos * kv_dim + i] = f->v[i];
    }

    float scale = 1.0f / f_sqrt((float)hd);
    for (int h = 0; h < n_head; h++) {
        int kvh = h * n_kv / n_head;
        const float *qh = f->q + h * hd;
        float maxs = -1e30f;
        for (int t = 0; t <= pos; t++) {
            const float *kh = kc + (u64)t * kv_dim + kvh * hd;
            float s = 0.0f;
            for (int i = 0; i < hd; i++) s += qh[i] * kh[i];
            s *= scale;
            f->att[t] = s;
            if (s > maxs) maxs = s;
        }
        float sum = 0.0f;
        for (int t = 0; t <= pos; t++) {
            f->att[t] = f_exp(f->att[t] - maxs);
            sum += f->att[t];
        }
        if (sum <= 0.0f) sum = 1.0f;
        for (int i = 0; i < hd; i++) {
            float acc = 0.0f;
            for (int t = 0; t <= pos; t++) {
                const float *vh = vc + (u64)t * kv_dim + kvh * hd;
                acc += f->att[t] * vh[i];
            }
            f->h[h * hd + i] = acc / sum;
        }
    }
}

static void forward_one(fwd_t *f, int token, int pos) {
    ai_model_t *m = f->m;
    int n_embd = (int)m->n_embd;
    int n_ff = (int)m->n_ff;
    int n_head = (int)m->n_head;
    int n_kv = (int)m->n_head_kv;

    for (int i = 0; i < n_embd; i++) {
        f->x[i] = f->tok_emb[(u64)token * (u64)n_embd + (u64)i];
    }

    for (int l = 0; l < (int)m->n_layer; l++) {
        if (m->has_attn) {
            rmsnorm(f->x, f->attn_norm[l], f->h, n_embd, m->rms_eps);
            matmul(f->wq[l], f->h, f->q, n_embd, n_embd);
            matmul(f->wk[l], f->h, f->k, (int)n_kv * (n_embd / n_head), n_embd);
            matmul(f->wv[l], f->h, f->v, (int)n_kv * (n_embd / n_head), n_embd);
            rope(f->q, n_embd, n_head, pos, m->rope_freq_base);
            rope(f->k, (int)n_kv * (n_embd / n_head), n_kv, pos,
                 m->rope_freq_base);
            attention(f, l, pos, n_embd, n_head, n_kv);
            matmul(f->wo[l], f->h, f->q, n_embd, n_embd);
            for (int i = 0; i < n_embd; i++) f->x[i] += f->q[i];
        }

        if (m->has_ffn) {
            rmsnorm(f->x, f->ffn_norm[l], f->h, n_embd, m->rms_eps);
            matmul(f->wgate[l], f->h, f->gate, n_ff, n_embd);
            matmul(f->wup[l], f->h, f->up, n_ff, n_embd);
            for (int i = 0; i < n_ff; i++) {
                float g = f->gate[i];
                float silu = g / (1.0f + f_exp(-g));
                f->gate[i] = silu * f->up[i];
            }
            matmul(f->wdown[l], f->gate, f->h, n_embd, n_ff);
            for (int i = 0; i < n_embd; i++) f->x[i] += f->h[i];
        }
    }

    rmsnorm(f->x, f->out_norm, f->h, n_embd, m->rms_eps);
    matmul(f->out_w, f->h, f->logits, (int)m->n_vocab, n_embd);
}

/* ---------------------------------------------------------------------
 * sampling
 * ------------------------------------------------------------------- */

static u32 xorshift(u32 *s) {
    u32 x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x ? x : 0x1234567u;
    return *s;
}

static int sample_token(float *logits, int n, const ai_params_t *p,
                        const int *hist, int nhist, u32 *rng) {
    /* repetition penalty over the recent window */
    int start = nhist > 64 ? nhist - 64 : 0;
    for (int i = start; i < nhist; i++) {
        int t = hist[i];
        if (t < 0 || t >= n) continue;
        if (logits[t] > 0.0f) logits[t] /= p->repeat_penalty;
        else logits[t] *= p->repeat_penalty;
    }

    float temp = p->temperature > 0.01f ? p->temperature : 0.01f;
    for (int i = 0; i < n; i++) logits[i] /= temp;

    int k = p->top_k;
    if (k <= 0 || k > n) k = n;

    int   idx[256];
    float val[256];
    u8    used[256];
    memset(used, 0, sizeof(used));

    for (int j = 0; j < k; j++) {
        int best = -1;
        float bv = -1e30f;
        for (int i = 0; i < n; i++) {
            if (used[i]) continue;
            if (logits[i] > bv) { bv = logits[i]; best = i; }
        }
        if (best < 0) { k = j; break; }
        used[best] = 1;
        idx[j] = best;
        val[j] = bv;
    }
    if (k == 0) return 0;

    float maxv = val[0];
    float sum = 0.0f;
    for (int j = 0; j < k; j++) {
        val[j] = f_exp(val[j] - maxv);
        sum += val[j];
    }
    if (sum <= 0.0f) return idx[0];
    for (int j = 0; j < k; j++) val[j] /= sum;

    /* nucleus: drop the tail once top-p mass is reached */
    float mass = 0.0f;
    int keep = k;
    for (int j = 0; j < k; j++) {
        mass += val[j];
        if (mass >= p->top_p) { keep = j + 1; break; }
    }

    float r = (float)(xorshift(rng) % 100000u) / 100000.0f;
    float acc = 0.0f;
    for (int j = 0; j < keep; j++) {
        acc += val[j];
        if (r <= acc) return idx[j];
    }
    return idx[keep - 1];
}

/* ---------------------------------------------------------------------
 * generation
 * ------------------------------------------------------------------- */

void ai_params_default(ai_params_t *p) {
    if (!p) return;
    p->temperature = AI_DEF_TEMPERATURE;
    p->top_k = AI_DEF_TOP_K;
    p->top_p = AI_DEF_TOP_P;
    p->repeat_penalty = AI_DEF_REPEAT_PENALTY;
    p->seed = 0x9E3779B9u;
    p->max_tokens = AI_DEF_MAX_TOKENS;
}

int ai_generate(ai_model_t *m, const int *prompt, int nprompt,
                const ai_params_t *params, ai_stream_fn fn, void *ud,
                int *n_out, u64 *ms_out) {
    if (!m || !m->ok || nprompt <= 0) return -1;
    if (nprompt >= (int)m->n_ctx) return -1;

    ai_params_t p;
    if (params) p = *params;
    else ai_params_default(&p);
    if (p.max_tokens <= 0) p.max_tokens = AI_DEF_MAX_TOKENS;
    if (p.max_tokens > (int)m->n_ctx - nprompt) {
        p.max_tokens = (int)m->n_ctx - nprompt;
    }

    const float *tok_emb = ai_tensor(m, "token_embd.weight");
    const float *out_norm = ai_tensor(m, "output_norm.weight");
    const float *out_w = ai_tensor(m, "output.weight");
    if (!tok_emb || !out_w) return -1;

    int n_embd = (int)m->n_embd;
    int n_ff = (int)m->n_ff;
    int n_vocab = (int)m->n_vocab;
    int kv_dim = (int)(m->n_embd * m->n_head_kv / m->n_head);

    fwd_t f;
    memset(&f, 0, sizeof(f));
    f.m = m;
    f.tok_emb = tok_emb;
    f.out_norm = out_norm;
    f.out_w = out_w;

    for (u32 l = 0; l < m->n_layer && l < 4; l++) {
        char key[AI_NAME_MAX];

        snprintf(key, sizeof(key), "blk.%u.attn_norm.weight", l);
        f.attn_norm[l] = ai_tensor(m, key);
        snprintf(key, sizeof(key), "blk.%u.ffn_norm.weight", l);
        f.ffn_norm[l] = ai_tensor(m, key);
        snprintf(key, sizeof(key), "blk.%u.attn_q.weight", l);
        f.wq[l] = ai_tensor(m, key);
        snprintf(key, sizeof(key), "blk.%u.attn_k.weight", l);
        f.wk[l] = ai_tensor(m, key);
        snprintf(key, sizeof(key), "blk.%u.attn_v.weight", l);
        f.wv[l] = ai_tensor(m, key);
        snprintf(key, sizeof(key), "blk.%u.attn_output.weight", l);
        f.wo[l] = ai_tensor(m, key);
        snprintf(key, sizeof(key), "blk.%u.ffn_gate.weight", l);
        f.wgate[l] = ai_tensor(m, key);
        snprintf(key, sizeof(key), "blk.%u.ffn_up.weight", l);
        f.wup[l] = ai_tensor(m, key);
        snprintf(key, sizeof(key), "blk.%u.ffn_down.weight", l);
        f.wdown[l] = ai_tensor(m, key);
    }

    usize sz_x = (usize)n_embd * sizeof(float);
    usize sz_ff = (usize)n_ff * sizeof(float);
    usize sz_kv = (usize)m->n_layer * m->n_ctx * (usize)kv_dim * sizeof(float);

    f.x = malloc((size_t)(sz_x * 3));
    f.h = malloc((size_t)sz_x);
    f.q = malloc((size_t)sz_x);
    f.k = malloc((size_t)sz_x);
    f.v = malloc((size_t)sz_x);
    f.att = malloc((size_t)((usize)m->n_ctx * sizeof(float)));
    f.gate = malloc((size_t)sz_ff);
    f.up = malloc((size_t)sz_ff);
    f.logits = malloc((size_t)((usize)n_vocab * sizeof(float)));
    if (m->has_attn) {
        f.kcache = malloc((size_t)sz_kv);
        f.vcache = malloc((size_t)sz_kv);
    }

    int rc = 0;
    if (!f.x || !f.h || !f.q || !f.k || !f.v || !f.att ||
        !f.gate || !f.up || !f.logits ||
        (m->has_attn && (!f.kcache || !f.vcache))) {
        rc = -1;
        goto out;
    }

    u64 t0 = 0;
    mcube_time_t ts;
    if (mc_time(&ts) == 0) t0 = ts.uptime_ms;

    int pos = 0;
    for (int i = 0; i < nprompt; i++) {
        forward_one(&f, prompt[i], pos++);
    }

    int hist[512];
    int nhist = 0;
    for (int i = 0; i < nprompt && nhist < 512; i++) hist[nhist++] = prompt[i];

    u32 rng = p.seed ? p.seed : 0x1234567u;
    int produced = 0;

    for (int i = 0; i < p.max_tokens; i++) {
        int token = sample_token(f.logits, n_vocab, &p, hist, nhist, &rng);
        if (token <= 0) break;

        char piece = ai_decode_byte(m, token);
        if (piece && fn) {
            if (fn(ud, &piece, 1) != 0) break;
        }
        produced++;

        hist[nhist < 512 ? nhist : 511] = token;
        if (nhist < 512) nhist++;

        forward_one(&f, token, pos++);
        if (pos + 1 >= (int)m->n_ctx) break;
    }

    if (ms_out) {
        mcube_time_t te;
        if (mc_time(&te) == 0) *ms_out = te.uptime_ms - t0;
        else *ms_out = 0;
    }
    if (n_out) *n_out = produced;

out:
    free(f.x);
    free(f.h);
    free(f.q);
    free(f.k);
    free(f.v);
    free(f.att);
    free(f.gate);
    free(f.up);
    free(f.logits);
    free(f.kcache);
    free(f.vcache);
    return rc;
}
