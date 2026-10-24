#pragma once
/* Local inference runtime for MCubeAIOS (M9).
 *
 * This lives in user space and is linked into `aiosd` only. It parses a GGUF
 * v3 container, builds the Llama-style forward pass, samples tokens and
 * exposes the agent layer (tool registry + sessions) that aiosd serves over
 * the kernel AI channel.
 *
 * The whole runtime is freestanding C: no libm, no SSE. Float helpers used by
 * softmax/silu are implemented with x87 + polynomial approximations.
 */

#include <mcube/types.h>

#define AI_MAX_TENSORS 32
#define AI_NAME_MAX    48

/* Default generation parameters (see docs/ai-native.md). */
#define AI_DEF_TEMPERATURE     0.80f
#define AI_DEF_TOP_K           40
#define AI_DEF_TOP_P           0.95f
#define AI_DEF_REPEAT_PENALTY  1.10f
#define AI_DEF_MAX_TOKENS      32

typedef struct ai_params {
    float temperature;
    int   top_k;
    float top_p;
    float repeat_penalty;
    u32   seed;
    int   max_tokens;
} ai_params_t;

typedef struct ai_tensor {
    char name[AI_NAME_MAX];
    u32  type;      /* ggml type, 0 = F32 */
    u32  n_dims;
    u64  dims[4];   /* dims[0] is the fastest-varying axis */
    u64  offset;    /* relative to the tensor data section */
} ai_tensor_t;

typedef struct ai_model {
    u8  *buf;
    u64  size;
    u64  data_off;      /* absolute offset of the tensor data section */
    u32  n_tensors;
    ai_tensor_t tensors[AI_MAX_TENSORS];

    u32  n_vocab;
    u32  n_embd;
    u32  n_layer;
    u32  n_head;
    u32  n_head_kv;
    u32  n_ff;
    u32  n_ctx;
    float rope_freq_base;
    float rms_eps;

    int  has_attn;
    int  has_ffn;
    int  ok;
    char desc[96];
} ai_model_t;

void ai_params_default(ai_params_t *p);

int  ai_model_open(ai_model_t *m, const char *path);
void ai_model_close(ai_model_t *m);
const float *ai_tensor(const ai_model_t *m, const char *name);

/* Byte-level tokenizer: token id == byte value. */
int  ai_encode(const ai_model_t *m, const char *text, int *out, int max);
char ai_decode_byte(const ai_model_t *m, int token);

typedef int (*ai_stream_fn)(void *ud, const char *piece, int npiece);

/* Generate `p->max_tokens` tokens after `prompt`. Returns 0 on success. */
int  ai_generate(ai_model_t *m, const int *prompt, int nprompt,
                 const ai_params_t *p, ai_stream_fn fn, void *ud,
                 int *n_out, u64 *ms_out);

/* ---- agent layer ---------------------------------------------------- */

#define AI_SESSIONS     8
#define AI_SESSION_TURNS 8
#define AI_TOOL_MAX     12

typedef struct ai_tool {
    const char *name;
    const char *schema;     /* one-line JSON schema summary */
    const char *help;
    int         needs_confirm;
    int       (*run)(const char *args, char *out, int cap);
} ai_tool_t;

void ai_agent_init(void);
int  ai_tool_count(void);
const ai_tool_t *ai_tool_at(int i);
const ai_tool_t *ai_tool_find(const char *name);
int  ai_tool_invoke(const ai_tool_t *t, const char *args, char *out, int cap);

/* Rule-based intent routing. Returns the tool to call and fills `args`
 * (may be empty). Returns NULL when the request is plain chat. */
const ai_tool_t *ai_route(const char *prompt, char *args, int argcap);

void ai_session_reset(u32 key);
int  ai_session_turns(u32 key);
int  ai_session_append(u32 key, const char *role, const char *text);
/* Recent dialogue, oldest first, truncated to fit `cap`. */
void ai_session_brief(u32 key, char *out, int cap);
/* Number of live sessions (for status reporting). */
int  ai_session_count(void);
