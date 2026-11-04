/* Browser: a minimal text web browser for MCubeAIOS.
 *
 * The window talks to the desktop over <mcube/gui.h> like every other GUI
 * program. Two kinds of page exist:
 *
 *   mcube://home      a built-in start page that needs no network at all
 *   http://host/path  a plain HTTP/1.0 GET over a BSD socket
 *
 * The response body is turned into a flat array of wrapped text lines, which
 * is all the 8x16 bitmap font can draw. Every failure leaves the previous page
 * on screen and reports the reason in the status bar.
 */

#include <mcube/libc.h>
#include <mcube/gui.h>
#include <mcube/input.h>
#include <mcube/imgdec.h>

#define WIN_W       780
#define WIN_H       540

#define TABBAR_H    22
#define TOOLBAR_H   28
#define BOOKBAR_H   20
#define CONTENT_Y   (TABBAR_H + TOOLBAR_H + BOOKBAR_H)
#define STATUS_H    20
#define ROW_H       16
#define CHAR_W      8

#define MAX_LINES   4000
#define MAX_COLS    200
#define MAX_BODY    (48 * 1024)
#define URL_MAX     200
#define STATUS_MAX  120
#define HIST_MAX    16
#define LOAD_TIMEOUT_MS 8000

/* Inline images. A page may contain up to MAX_IMAGES of them; each is fetched,
 * decoded and scaled once at load time, then kept in a heap buffer so scrolling
 * only costs a few small draw commands per frame. */
#define MAX_IMAGES      2
#define IMG_MAX_BYTES   (384 * 1024)
#define IMG_MAX_W       764
#define IMG_MAX_H       440
#define IMG_DEC_MAX_PX  IMGDEC_MAX_PX

#define MAX_TABS        8
#define TAB_TITLE_MAX   60
#define MAX_BOOKMARKS   12
#define BM_TITLE_MAX    40
#define BM_LABEL_CHARS  16
#define BOOKMARK_PATH   "/BOOKMARKS.TXT"

/* Every <a href> becomes a numbered link: the page shows "[n]" after its text,
 * a mouse click anywhere in that span opens it, and typing just the number in
 * the address bar does the same. A stale table is cleared on every render. */
#define MAX_LINKS       96
#define PANEL_ROW_H     18
#define PANEL_MAX_ROWS  14

#define C_BG     0x0E141C
#define C_PANEL  0x161F2B
#define C_ACCENT 0x3B82F6
#define C_TEXT   0xE6ECF5
#define C_DIM    0x8FA0B6
#define C_FIELD  0x0A1018

typedef struct rect {
    int x, y, w, h;
} rect_t;

/* One tab keeps only what can be restored cheaply: the address, a title for
 * the strip and the scroll offset. The page text itself lives in the shared
 * render buffer, so switching tabs reloads the address. */
typedef struct tab {
    char url[URL_MAX + 1];
    char title[TAB_TITLE_MAX + 1];
    int top;
} tab_t;

/* One anchor on the current page. l0/c0 is the first character of the anchor
 * text and l1/c1 the last, so a click hit-test is a simple range check. ml/mc
 * is where the "[n]" marker starts, which is also clickable. */
typedef struct link {
    char url[URL_MAX + 1];
    int l0, c0;
    int l1, c1;
    int ml, mc;
} link_t;

/* Wrapped page text: MAX_LINES rows of MAX_COLS bytes, heap allocated because
 * the 800 KiB would otherwise sit in .bss. */
static char (*g_lines)[MAX_COLS];
static char *g_body;            /* page source, re-rendered on resize */
static char *g_rx;              /* raw socket receive buffer */

static int g_nlines;
static int g_top;
static int g_wrap;
static int g_cur;               /* line currently being filled */
static int g_col;
static int g_body_len;
static int g_body_html;
static int g_need_redraw = 1;

static char g_url[URL_MAX + 1] = "mcube://home";
static char g_status[STATUS_MAX + 1] = "ready";

static char g_hist[HIST_MAX][URL_MAX + 1];
static int g_hist_count;
static int g_hist_pos = -1;

static tab_t g_tabs[MAX_TABS] = { { "mcube://home", "New Tab", 0 } };
static int g_tab_count = 1;
static int g_tab_cur;
static int g_tab_vis;           /* tabs that fit on the strip */

static char g_bm_title[MAX_BOOKMARKS][BM_TITLE_MAX + 1];
static char g_bm_url[MAX_BOOKMARKS][URL_MAX + 1];
static int g_bm_count;
static int g_bm_vis;            /* bookmark chips that fit in the bar */

static int g_ctrl;              /* Ctrl is down */

/* Links found while rendering, and the base address relative hrefs resolve
 * against. g_active_link is the anchor currently open, -1 when none. */
static link_t g_links[MAX_LINKS];
static int g_link_count;
static int g_active_link = -1;
static char g_base_host[128];
static int g_base_port = 80;
static char g_base_path[MAX_COLS] = "/";

/* Decoded images for the current page. The pixel buffers are allocated once at
 * startup: the user heap has no free(), so every page reuses the same slots. */
typedef struct {
    char url[URL_MAX + 1];
    int used;
    int w, h;               /* size after scaling to the window width */
    int slot;               /* desktop cache slot */
} img_t;

typedef struct {
    int img;                /* index into g_images */
    int line;               /* first page line covered by the image */
    int rows;               /* text rows the image occupies */
} img_place_t;

static img_t g_images[MAX_IMAGES];
static int g_img_count;
static img_place_t g_places[MAX_IMAGES];
static int g_place_count;
static char *g_img_raw;     /* raw HTTP response of one image */
static u8   *g_img_aux;     /* concatenated PNG IDAT stream */
static u8   *g_img_rows;    /* inflated PNG scanlines */
static u32  *g_img_dec;     /* decoded natural-size pixels */
static u32  *g_img_px[MAX_IMAGES];

/* Overlay panel listing bookmarks and recent history; also the address field
 * selection state, where the first keystroke replaces the whole address. */
static int g_panel_open;
static int g_url_sel;
static int g_pre;               /* inside <pre>: whitespace is significant */
static int g_head;              /* inside <h1>/<h2>: render in upper case */

static rect_t g_btn_back, g_btn_home, g_btn_reload, g_btn_go, g_btn_star;
static rect_t g_btn_menu, g_panel;
static rect_t g_url_box;
static rect_t g_tab_rect[MAX_TABS], g_tab_close[MAX_TABS], g_btn_newtab;
static rect_t g_bm_rect[MAX_BOOKMARKS];

static void draw(void);
static void rerender(void);
static void load_url_inner(const char *url, int push);
static void load_url(const char *url);
static int tag_is(const char *name, int len, const char *lit);
static int img_find(const char *url);

/* ---- status ------------------------------------------------------- */

static void set_status(const char *s) {
    snprintf(g_status, sizeof(g_status), "%s", s ? s : "");
    g_need_redraw = 1;
}

/* ---- bookmark helpers --------------------------------------------- */

/* Characters of a bookmark title that fit on its chip in the bookmark bar. */
static int bm_label_len(int i) {
    int n = (int)strlen(g_bm_title[i]);
    return n > BM_LABEL_CHARS ? BM_LABEL_CHARS : n;
}

static int bookmark_has(const char *url) {
    for (int i = 0; i < g_bm_count; i++) {
        if (strcmp(g_bm_url[i], url) == 0) return 1;
    }
    return 0;
}

/* ---- text extraction ---------------------------------------------- */

static void put_char(char c) {
    if (g_cur >= MAX_LINES) return;
    if (g_col >= g_wrap) {
        if (g_cur + 1 >= MAX_LINES) return;
        g_cur++;
        g_col = 0;
        g_lines[g_cur][0] = 0;
    }
    g_lines[g_cur][g_col++] = c;
    g_lines[g_cur][g_col] = 0;
}

static void put_break(void) {
    if (g_cur + 1 >= MAX_LINES) return;
    g_cur++;
    g_col = 0;
    g_lines[g_cur][0] = 0;
}

static void render_start(void) {
    g_cur = 0;
    g_col = 0;
    g_lines[0][0] = 0;
    g_wrap = (gui_width() - 16) / CHAR_W;
    if (g_wrap < 16) g_wrap = 16;
    if (g_wrap > MAX_COLS - 1) g_wrap = MAX_COLS - 1;
    g_link_count = 0;
    g_active_link = -1;
    g_place_count = 0;
    g_pre = 0;
    g_head = 0;
}

/* ---- links -------------------------------------------------------- */

/* Close the anchor that is being rendered: remember where its text ended, then
 * append " [n]" so the reader can see the number the address bar accepts. */
static void link_finish(void) {
    if (g_active_link < 0) return;

    link_t *lk = &g_links[g_active_link];
    lk->l1 = g_cur;
    lk->c1 = g_col;

    put_char(' ');
    char mk[6];
    snprintf(mk, sizeof(mk), "[%d]", g_active_link + 1);
    put_char(mk[0]);
    lk->ml = g_cur;
    lk->mc = g_col - 1;
    for (int i = 1; mk[i]; i++) put_char(mk[i]);

    g_active_link = -1;
}

/* Turn an href into an address this browser can fetch. Fragments and schemes
 * without a handler are dropped rather than shown as dead links. */
static int link_resolve(const char *href, char *out, int cap) {
    out[0] = 0;
    if (!href || !href[0] || href[0] == '#') return -1;
    if (strncmp(href, "http://", 7) == 0) {
        snprintf(out, (size_t)cap, "%s", href);
        return 0;
    }
    if (strncmp(href, "https://", 8) == 0 || strncmp(href, "mailto:", 7) == 0 ||
        strncmp(href, "javascript:", 11) == 0) {
        return -1;
    }
    if (g_base_host[0] == 0) return -1;

    if (href[0] == '/') {
        snprintf(out, (size_t)cap, "http://%s:%d%s", g_base_host, g_base_port, href);
        return 0;
    }

    /* Relative to the directory of the page currently loaded. */
    char dir[MAX_COLS];
    snprintf(dir, sizeof(dir), "%s", g_base_path);
    int n = (int)strlen(dir);
    while (n > 0 && dir[n - 1] != '/') n--;
    if (n == 0) snprintf(dir, sizeof(dir), "/");
    else dir[n] = 0;
    snprintf(out, (size_t)cap, "http://%s:%d%s%s",
             g_base_host, g_base_port, dir, href);
    return 0;
}

static void link_open(const char *href) {
    link_finish();              /* nested <a> ends the previous anchor */
    if (g_link_count >= MAX_LINKS) return;

    char resolved[URL_MAX + 1];
    if (link_resolve(href, resolved, (int)sizeof(resolved)) != 0) return;

    link_t *lk = &g_links[g_link_count];
    memset(lk, 0, sizeof(*lk));
    snprintf(lk->url, URL_MAX + 1, "%s", resolved);
    lk->l0 = g_cur;
    lk->c0 = g_col;
    g_active_link = g_link_count;
    g_link_count++;
}

/* Pull one attribute value out of the tag body s[start..end). Attribute names
 * are matched case-insensitively, values may be quoted or bare. */
static void attr_value(const char *s, int start, int end, const char *name,
                       char *out, int cap) {
    out[0] = 0;

    int i = start;
    while (i < end) {
        while (i < end && (s[i] == ' ' || s[i] == '\t' ||
                           s[i] == '\r' || s[i] == '\n')) i++;
        int ns = i;
        while (i < end && s[i] != '=' && s[i] != ' ' && s[i] != '\t' &&
               s[i] != '\r' && s[i] != '\n') i++;
        int nl = i - ns;
        int match = nl > 0 && tag_is(s + ns, nl, name);

        while (i < end && (s[i] == ' ' || s[i] == '\t')) i++;
        if (i < end && s[i] == '=') {
            i++;
            while (i < end && (s[i] == ' ' || s[i] == '\t')) i++;
            char quote = 0;
            if (i < end && (s[i] == '"' || s[i] == '\'')) quote = s[i++];
            int vs = i;
            if (quote) {
                while (i < end && s[i] != quote) i++;
            } else {
                while (i < end && s[i] != ' ' && s[i] != '\t' &&
                       s[i] != '\r' && s[i] != '\n') i++;
            }
            int vl = i - vs;
            if (match) {
                int cn = vl < cap - 1 ? vl : cap - 1;
                if (cn < 0) cn = 0;
                memcpy(out, s + vs, (size_t)cn);
                out[cn] = 0;
                return;
            }
            if (quote && i < end) i++;
        } else if (match) {
            return;             /* attribute with no value */
        }
        if (i == ns) i++;       /* never stall on a stray character */
    }
}

/* Emit a short replacement string for a multi-character entity. */
static void put_str(const char *t) {
    while (*t) put_char(*t++);
}


/* Plain text: keep it verbatim apart from wrapping and dropping CR. */
static void render_plain(const char *s, int len) {
    render_start();
    for (int i = 0; i < len; i++) {
        u8 uc = (u8)s[i];
        if (uc == '\r') continue;
        if (uc == '\n') put_break();
        else if (uc == '\t') { for (int k = 0; k < 4; k++) put_char(' '); }
        else if (uc >= 0x20 && uc < 0x7F) put_char((char)uc);
    }
}

static int tag_is(const char *name, int len, const char *lit) {
    int i = 0;
    while (i < len && lit[i]) {
        char a = name[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != lit[i]) return 0;
        i++;
    }
    return i == len && lit[i] == 0;
}

static int block_tag(const char *name, int len) {
    static const char *names[] = {
        "p", "div", "tr", "table", "ul", "ol", "li",
        "h1", "h2", "h3", "h4", "h5", "h6", "title", "center"
    };
    for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
        if (tag_is(name, len, names[i])) return 1;
    }
    return 0;
}

static int ent_at(const char *s, int len, int i, const char *lit) {
    int n = (int)strlen(lit);
    if (i + n > len) return 0;
    return memcmp(s + i, lit, (size_t)n) == 0;
}

static int is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* Strip tags, decode entities and collapse whitespace. Anchors are collected
 * for the numbered-link table, and a few structural tags get more shape:
 * headings in upper case, <pre> keeps its layout, <td> cells are separated,
 * <img> shows its alt text and <hr> becomes a rule. */
static void render_html(const char *s, int len) {
    int i = 0;
    int pending_space = 0;

    render_start();

    while (i < len) {
        char c = s[i];

        if (c == '<') {
            int j = i + 1;
            if (j >= len) { put_char('<'); i++; continue; }

            char nc = s[j];
            if (nc == '!') {
                if (j + 2 < len && s[j + 1] == '-' && s[j + 2] == '-') {
                    j += 3;
                    while (j + 2 < len &&
                           !(s[j] == '-' && s[j + 1] == '-' && s[j + 2] == '>')) j++;
                    i = (j + 3 <= len) ? j + 3 : len;
                } else {
                    while (j < len && s[j] != '>') j++;
                    i = (j < len) ? j + 1 : len;
                }
                pending_space = 0;
                continue;
            }
            if (nc == '?') {
                while (j < len && s[j] != '>') j++;
                i = (j < len) ? j + 1 : len;
                pending_space = 0;
                continue;
            }

            if (!(nc == '/' || is_alpha(nc))) {   /* literal "<" */
                if (pending_space) {
                    if (g_col > 0) put_char(' ');
                    pending_space = 0;
                }
                put_char('<');
                i++;
                continue;
            }

            int closing = 0;
            if (nc == '/') { closing = 1; j++; }
            int ns = j;
            while (j < len && s[j] != '>' && s[j] != ' ' && s[j] != '\t' &&
                   s[j] != '\r' && s[j] != '\n' && s[j] != '/') j++;
            int nl = j - ns;
            int k = j;
            while (k < len && s[k] != '>') k++;

            if (nl > 0) {
                char name[16];
                int cn = nl < 15 ? nl : 15;
                memcpy(name, s + ns, (size_t)cn);
                name[cn] = 0;

                if (tag_is(name, cn, "a")) {
                    if (closing) {
                        link_finish();
                    } else {
                        char href[URL_MAX + 1];
                        attr_value(s, j, k, "href", href, (int)sizeof(href));
                        link_open(href);
                    }
                } else if (!closing && tag_is(name, cn, "br")) {
                    put_break();
                } else if (tag_is(name, cn, "hr")) {
                    if (!closing) {
                        if (g_col > 0) put_break();
                        for (int t = 0; t < 48; t++) put_char('-');
                        put_break();
                    }
                } else if (!closing && tag_is(name, cn, "img")) {
                    char src[URL_MAX + 1];
                    char alt[64];
                    attr_value(s, j, k, "src", src, (int)sizeof(src));
                    attr_value(s, j, k, "alt", alt, (int)sizeof(alt));

                    int idx = -1;
                    if (src[0]) {
                        char url[URL_MAX + 1];
                        if (link_resolve(src, url, (int)sizeof(url)) == 0) {
                            idx = img_find(url);
                        }
                    }

                    if (idx >= 0 && g_place_count < MAX_IMAGES) {
                        /* Reserve whole text rows for the bitmap; draw() paints
                         * it over those lines and clips to the visible part. */
                        if (g_col > 0) put_break();
                        int irows = (g_images[idx].h + ROW_H - 1) / ROW_H;
                        int top = g_cur;
                        for (int r = 0; r < irows; r++) put_break();
                        g_places[g_place_count].img = idx;
                        g_places[g_place_count].line = top;
                        g_places[g_place_count].rows = irows;
                        g_place_count++;
                    } else {
                        if (g_col > 0) put_char(' ');
                        put_str("[IMG");
                        if (alt[0]) {
                            put_str(": ");
                            for (int t = 0; alt[t] && alt[t] < 0x7F; t++) put_char(alt[t]);
                        }
                        put_str("]");
                    }
                } else if (tag_is(name, cn, "pre")) {
                    if (g_col > 0) put_break();
                    g_pre = closing ? 0 : 1;
                } else if (!closing && tag_is(name, cn, "td")) {
                    if (g_col > 0) put_str(" | ");
                } else if (tag_is(name, cn, "h1") || tag_is(name, cn, "h2")) {
                    if (g_col > 0) put_break();
                    if (!closing) put_break();      /* one rule of air above */
                    g_head = closing ? 0 : 1;
                } else if (!closing && tag_is(name, cn, "li")) {
                    if (g_col > 0) put_break();
                    put_str("* ");
                } else if (block_tag(name, cn)) {
                    if (g_col > 0) put_break();
                }
            }

            pending_space = 0;
            i = (k < len) ? k + 1 : len;
            continue;
        }

        if (c == '&') {
            if (ent_at(s, len, i, "&amp;"))       { c = '&';  i += 5; }
            else if (ent_at(s, len, i, "&lt;"))   { c = '<';  i += 4; }
            else if (ent_at(s, len, i, "&gt;"))   { c = '>';  i += 4; }
            else if (ent_at(s, len, i, "&quot;")) { c = '"';  i += 6; }
            else if (ent_at(s, len, i, "&apos;")) { c = '\''; i += 6; }
            else if (ent_at(s, len, i, "&#39;"))  { c = '\''; i += 5; }
            else if (ent_at(s, len, i, "&nbsp;")) { c = ' ';  i += 6; }
            else if (s[i + 1] == '#') {
                /* &#NN; / &#xNN; decimal form only; anything unprintable
                 * becomes a space so the page stays coherent. */
                int q = i + 2;
                int val = 0, digits = 0;
                while (q < len && s[q] >= '0' && s[q] <= '9' && digits < 5) {
                    val = val * 10 + (s[q] - '0');
                    q++;
                    digits++;
                }
                if (digits > 0 && q < len && s[q] == ';') {
                    c = (val >= 0x20 && val < 0x7F) ? (char)val : ' ';
                    i = q + 1;
                } else {
                    c = '&';
                    i += 1;
                }
            }
            else if (ent_at(s, len, i, "&copy;") || ent_at(s, len, i, "&reg;")) {
                if (pending_space) { if (g_col > 0) put_char(' '); pending_space = 0; }
                put_str(ent_at(s, len, i, "&copy;") ? "(c)" : "(r)");
                i += 7;
                continue;
            } else if (ent_at(s, len, i, "&trade;")) {
                if (pending_space) { if (g_col > 0) put_char(' '); pending_space = 0; }
                put_str("(tm)");
                i += 7;
                continue;
            } else if (ent_at(s, len, i, "&mdash;") || ent_at(s, len, i, "&ndash;")) {
                if (pending_space) { if (g_col > 0) put_char(' '); pending_space = 0; }
                put_str(ent_at(s, len, i, "&mdash;") ? "--" : "-");
                i += ent_at(s, len, i, "&mdash;") ? 7 : 6;
                continue;
            } else if (ent_at(s, len, i, "&hellip;")) {
                if (pending_space) { if (g_col > 0) put_char(' '); pending_space = 0; }
                put_str("...");
                i += 8;
                continue;
            } else if (ent_at(s, len, i, "&bull;")) {
                if (pending_space) { if (g_col > 0) put_char(' '); pending_space = 0; }
                put_str("*");
                i += 6;
                continue;
            } else if (ent_at(s, len, i, "&laquo;") || ent_at(s, len, i, "&raquo;") ||
                       ent_at(s, len, i, "&ldquo;") || ent_at(s, len, i, "&rdquo;")) {
                if (pending_space) { if (g_col > 0) put_char(' '); pending_space = 0; }
                put_str("\"");
                i += 7;
                continue;
            } else if (ent_at(s, len, i, "&lsquo;") || ent_at(s, len, i, "&rsquo;")) {
                if (pending_space) { if (g_col > 0) put_char(' '); pending_space = 0; }
                put_str("'");
                i += 6;
                continue;
            } else if (ent_at(s, len, i, "&middot;")) {
                if (pending_space) { if (g_col > 0) put_char(' '); pending_space = 0; }
                put_str(".");
                i += 7;
                continue;
            } else if (ent_at(s, len, i, "&times;")) {
                if (pending_space) { if (g_col > 0) put_char(' '); pending_space = 0; }
                put_str("x");
                i += 7;
                continue;
            } else if (ent_at(s, len, i, "&euro;")) {
                if (pending_space) { if (g_col > 0) put_char(' '); pending_space = 0; }
                put_str("EUR");
                i += 6;
                continue;
            } else {
                c = '&';
                i += 1;
            }
        } else {
            i++;
        }

        u8 uc = (u8)c;

        if (g_pre) {
            if (uc == '\r') { pending_space = 0; continue; }
            if (uc == '\n') { put_break(); pending_space = 0; continue; }
            if (uc == '\t') {
                for (int t = 0; t < 4; t++) put_char(' ');
                pending_space = 0;
                continue;
            }
            if (uc < 0x20 || uc >= 0x7F) { pending_space = 0; continue; }
            put_char((char)uc);
            pending_space = 0;
            continue;
        }

        if (uc == ' ' || uc == '\t' || uc == '\r' || uc == '\n') {
            pending_space = 1;
            continue;
        }
        if (uc < 0x20 || uc >= 0x7F) continue;   /* ASCII output only */
        if (pending_space) {
            if (g_col > 0) put_char(' ');
            pending_space = 0;
        }
        if (g_head && uc >= 'a' && uc <= 'z') uc = (u8)(uc - 'a' + 'A');
        put_char((char)uc);
    }

    link_finish();
}

/* ---- page layout -------------------------------------------------- */

static int page_rows(void) {
    int rows = (gui_height() - CONTENT_Y - STATUS_H) / ROW_H;
    return rows < 1 ? 1 : rows;
}

static void clamp_top(void) {
    int max = g_nlines - page_rows();
    if (max < 0) max = 0;
    if (g_top > max) g_top = max;
    if (g_top < 0) g_top = 0;
}

static void rerender(void) {
    if (!g_body) return;
    if (g_body_html) render_html(g_body, g_body_len);
    else render_plain(g_body, g_body_len);
    g_nlines = g_cur + 1;
    if (g_nlines > MAX_LINES) g_nlines = MAX_LINES;
    if (g_nlines < 1) g_nlines = 1;
    clamp_top();
    g_need_redraw = 1;
}

/* ---- drawing ------------------------------------------------------ */

static void layout(void) {
    int w = gui_width();
    int x = 6;
    int ty = TABBAR_H;

    g_btn_back.x = x;   g_btn_back.y = ty + 4;  g_btn_back.w = 48;
    g_btn_back.h = 20;
    x += 54;
    g_btn_home.x = x;   g_btn_home.y = ty + 4;  g_btn_home.w = 48;
    g_btn_home.h = 20;
    x += 54;
    g_btn_reload.x = x; g_btn_reload.y = ty + 4; g_btn_reload.w = 60;
    g_btn_reload.h = 20;
    x += 66;

    g_btn_go.x = w - 46;   g_btn_go.y = ty + 4;
    g_btn_go.w = 40;       g_btn_go.h = 20;
    g_btn_star.x = w - 78; g_btn_star.y = ty + 4;
    g_btn_star.w = 24;     g_btn_star.h = 20;
    g_btn_menu.x = w - 110; g_btn_menu.y = ty + 4;
    g_btn_menu.w = 24;      g_btn_menu.h = 20;

    g_url_box.x = x;
    g_url_box.y = ty + 3;
    g_url_box.w = g_btn_menu.x - 8 - x;
    g_url_box.h = 22;
    if (g_url_box.w < 40) g_url_box.w = 40;

    /* Overlay panel with bookmarks on the left and history on the right, sized
     * to the page area so the toolbar above it stays clickable. */
    g_panel.x = 6;
    g_panel.y = CONTENT_Y;
    g_panel.w = w - 12;
    if (g_panel.w > 620) g_panel.w = 620;
    g_panel.h = gui_height() - STATUS_H - 4 - g_panel.y;
    if (g_panel.h < 60) g_panel.h = 60;

    /* Tab strip: every tab shrinks towards a minimum width so the whole strip
     * plus the "+" button fits even in a narrowed window. */
    int tx = 4;
    g_tab_vis = 0;
    for (int i = 0; i < g_tab_count; i++) {
        int avail = w - 32 - tx;
        int left = g_tab_count - i;
        if (avail < 40) break;

        int tw = (int)strlen(g_tabs[i].title) * CHAR_W + 28;
        if (tw < 92) tw = 92;
        if (tw > 170) tw = 170;
        int maxw = avail - (left - 1) * 40 - 2;
        if (maxw < 40) maxw = 40;
        if (tw > maxw) tw = maxw;

        g_tab_rect[i].x = tx;
        g_tab_rect[i].y = 2;
        g_tab_rect[i].w = tw;
        g_tab_rect[i].h = TABBAR_H - 4;
        g_tab_close[i].x = tx + tw - 14;
        g_tab_close[i].y = 4;
        g_tab_close[i].w = 10;
        g_tab_close[i].h = 12;

        tx += tw + 2;
        g_tab_vis++;
    }
    for (int i = g_tab_vis; i < MAX_TABS; i++) {
        g_tab_rect[i].w = 0;
        g_tab_rect[i].h = 0;
        g_tab_close[i].w = 0;
        g_tab_close[i].h = 0;
    }
    g_btn_newtab.x = tx;
    g_btn_newtab.y = 4;
    g_btn_newtab.w = 22;
    g_btn_newtab.h = 14;
    if (g_btn_newtab.x + g_btn_newtab.w > w - 4) g_btn_newtab.x = w - 26;

    /* Bookmark bar. */
    int by = TABBAR_H + TOOLBAR_H;
    int bx = 6;
    g_bm_vis = 0;
    for (int i = 0; i < g_bm_count; i++) {
        int bw = bm_label_len(i) * CHAR_W + 12;
        if (bx + bw > w - 6) break;
        g_bm_rect[i].x = bx;
        g_bm_rect[i].y = by + 3;
        g_bm_rect[i].w = bw;
        g_bm_rect[i].h = BOOKBAR_H - 6;
        bx += bw + 4;
        g_bm_vis++;
    }
}

static void draw_button(const rect_t *r, const char *label, int accent) {
    gui_fill(r->x, r->y, r->w, r->h, accent ? C_ACCENT : 0x223044);
    int tw = (int)strlen(label) * CHAR_W;
    int tx = r->x + (r->w - tw) / 2;
    if (tx < r->x + 4) tx = r->x + 4;
    gui_text(tx, r->y + 2, 0xFFFFFF, label);
}

static void text_chunks(int x, int y, u32 color, const char *s, int n) {
    while (n > 0) {
        int k = n > 100 ? 100 : n;
        gui_textn(x, y, color, s, k);
        x += k * CHAR_W;
        s += k;
        n -= k;
    }
}

static void draw_url_field(void) {
    int cap = (g_url_box.w - 8) / CHAR_W;
    int len = (int)strlen(g_url);
    int start = 0;

    gui_fill(g_url_box.x, g_url_box.y, g_url_box.w, g_url_box.h, C_FIELD);
    gui_rect(g_url_box.x, g_url_box.y, g_url_box.w, g_url_box.h, C_ACCENT);
    if (cap < 1) return;

    if (len > cap) start = len - cap;
    /* The whole address is selected the moment the field is clicked, the way
     * a browser behaves: the next keystroke replaces it. */
    if (g_url_sel && len - start > 0) {
        gui_fill(g_url_box.x + 3, g_url_box.y + 3,
                 (len - start) * CHAR_W + 2, 16, 0x1D4ED8);
    }
    text_chunks(g_url_box.x + 4, g_url_box.y + 3, C_TEXT, g_url + start,
                len - start);

    int cx = g_url_box.x + 4 + (len - start) * CHAR_W;
    if (cx + 2 > g_url_box.x + g_url_box.w - 3) {
        cx = g_url_box.x + g_url_box.w - 4;
    }
    gui_fill(cx, g_url_box.y + 4, 2, 14, C_ACCENT);
}

static void draw_tabbar(void) {
    int w = gui_width();

    gui_fill(0, 0, w, TABBAR_H, C_PANEL);
    gui_fill(0, TABBAR_H - 1, w, 1, 0x0A1018);

    for (int i = 0; i < g_tab_vis; i++) {
        const rect_t *r = &g_tab_rect[i];
        int active = (i == g_tab_cur);
        gui_fill(r->x, r->y, r->w, r->h, active ? 0x223044 : 0x121A24);
        if (active) gui_fill(r->x, r->y + r->h - 2, r->w, 2, C_ACCENT);

        int cap = (r->w - 24) / CHAR_W;
        int n = (int)strlen(g_tabs[i].title);
        if (n > cap) n = cap;
        if (cap > 0 && n > 0) {
            text_chunks(r->x + 4, r->y + 3, active ? C_TEXT : C_DIM,
                        g_tabs[i].title, n);
        }

        const rect_t *c = &g_tab_close[i];
        gui_fill(c->x, c->y, c->w, c->h, active ? 0x2A3646 : 0x1A2430);
        if (c->w >= CHAR_W) gui_text(c->x + 1, c->y, active ? C_TEXT : C_DIM, "x");
    }

    gui_fill(g_btn_newtab.x, g_btn_newtab.y, g_btn_newtab.w, g_btn_newtab.h,
             0x223044);
    if (g_btn_newtab.w >= CHAR_W) {
        gui_text(g_btn_newtab.x + (g_btn_newtab.w - CHAR_W) / 2,
                 g_btn_newtab.y + 1, C_TEXT, "+");
    }
}

static void draw_bookbar(void) {
    int w = gui_width();
    int y = TABBAR_H + TOOLBAR_H;

    gui_fill(0, y, w, BOOKBAR_H, 0x111A24);
    gui_fill(0, y + BOOKBAR_H - 1, w, 1, 0x0A1018);

    if (g_bm_vis == 0) {
        gui_text(6, y + 3, C_DIM,
                 "No bookmarks - Ctrl+B (or the star) saves this page");
        return;
    }

    for (int i = 0; i < g_bm_vis; i++) {
        const rect_t *r = &g_bm_rect[i];
        gui_fill(r->x, r->y, r->w, r->h, 0x1C2733);
        text_chunks(r->x + 6, r->y + 2, C_TEXT, g_bm_title[i], bm_label_len(i));
    }
}

/* ---- bookmarks / history panel ------------------------------------ */

static int panel_colw(void) {
    int cw = (g_panel.w - 28) / 2;
    return cw < 40 ? 40 : cw;
}

static int panel_left_x(void) { return g_panel.x + 10; }
static int panel_right_x(void) { return g_panel.x + 10 + panel_colw() + 8; }
static int panel_first_y(void) { return g_panel.y + 34; }

static int panel_rows(void) {
    int r = (g_panel.h - 44) / PANEL_ROW_H;
    if (r < 0) r = 0;
    if (r > PANEL_MAX_ROWS) r = PANEL_MAX_ROWS;
    return r;
}

static void panel_row(int x, int y, int cw, const char *text, int dim) {
    gui_fill(x, y, cw, PANEL_ROW_H - 3, dim ? 0x161F2B : 0x1A2533);
    int cap = (cw - 8) / CHAR_W;
    int n = (int)strlen(text);
    if (n > cap) n = cap < 0 ? 0 : cap;
    if (n > 0) text_chunks(x + 4, y + 2, C_TEXT, text, n);
}

static void draw_panel(void) {
    int cw = panel_colw();
    int lx = panel_left_x();
    int rx = panel_right_x();
    int y0 = panel_first_y();
    int rows = panel_rows();
    char cnt[24];

    gui_fill(g_panel.x, g_panel.y, g_panel.w, g_panel.h, 0x101822);
    gui_rect(g_panel.x, g_panel.y, g_panel.w, g_panel.h, C_ACCENT);

    gui_text(lx, g_panel.y + 8, C_ACCENT, "BOOKMARKS");
    snprintf(cnt, sizeof(cnt), "%d", g_bm_count);
    gui_text(lx + 9 * CHAR_W, g_panel.y + 8, C_DIM, cnt);

    gui_text(rx, g_panel.y + 8, C_ACCENT, "HISTORY");
    snprintf(cnt, sizeof(cnt), "%d", g_hist_count);
    gui_text(rx + 7 * CHAR_W, g_panel.y + 8, C_DIM, cnt);

    gui_text(g_panel.x + g_panel.w - 22, g_panel.y + 8, C_TEXT, "x");

    if (rows <= 0) return;

    if (g_bm_count == 0) {
        gui_text(lx + 4, y0 + 2, C_DIM, "(none yet - use the star)");
    }
    for (int i = 0; i < rows && i < g_bm_count; i++) {
        int ry = y0 + i * PANEL_ROW_H;
        panel_row(lx, ry, cw, g_bm_title[i], 0);
        /* The chip on the right of the row removes the bookmark. */
        gui_fill(lx + cw - 14, ry + 2, 11, 12, 0x2A3646);
        gui_text(lx + cw - 13, ry + 1, C_DIM, "x");
    }

    if (g_hist_count == 0) {
        gui_text(rx + 4, y0 + 2, C_DIM, "(empty)");
    }
    for (int i = 0; i < rows && i < g_hist_count; i++) {
        int ry = y0 + i * PANEL_ROW_H;
        int j = g_hist_count - 1 - i;      /* newest first */
        const char *u = g_hist[j];
        if (strncmp(u, "http://", 7) == 0) u += 7;
        panel_row(rx, ry, cw, u, 1);
    }
}

static void draw(void) {
    int w = gui_width();
    int h = gui_height();
    int rows = page_rows();
    int cols = (w - 16) / CHAR_W;

    if (cols < 1) cols = 1;
    clamp_top();
    layout();

    gui_clear(C_BG);

    /* Tab strip. */
    draw_tabbar();

    /* Toolbar. */
    gui_fill(0, TABBAR_H, w, TOOLBAR_H, C_PANEL);
    gui_fill(0, TABBAR_H + TOOLBAR_H - 1, w, 1, 0x0A1018);
    draw_button(&g_btn_back, "Back", 0);
    draw_button(&g_btn_home, "Home", 0);
    draw_button(&g_btn_reload, "Reload", 0);
    draw_url_field();
    draw_button(&g_btn_star, "*", bookmark_has(g_url));
    draw_button(&g_btn_go, "GO", 1);

    /* Overflow menu: three bars, mirrored by the shortcut Ctrl+M. */
    gui_fill(g_btn_menu.x, g_btn_menu.y, g_btn_menu.w, g_btn_menu.h,
             g_panel_open ? C_ACCENT : 0x223044);
    for (int r = 0; r < 3; r++) {
        gui_fill(g_btn_menu.x + 6, g_btn_menu.y + 6 + r * 5,
                 g_btn_menu.w - 12, 2, 0xFFFFFF);
    }

    /* Bookmark bar. */
    draw_bookbar();

    /* Page text. */
    for (int r = 0; r < rows; r++) {
        int idx = g_top + r;
        if (idx >= g_nlines) break;
        int n = (int)strlen(g_lines[idx]);
        if (n > cols) n = cols;
        if (n > 0) text_chunks(8, CONTENT_Y + 2 + r * ROW_H, C_TEXT,
                               g_lines[idx], n);
    }

    /* Inline bitmaps: painted over the text rows reserved for them, clipped to
     * the visible part of the page (and to the page area, never the bars). */
    for (int i = 0; i < g_place_count; i++) {
        const img_place_t *p = &g_places[i];
        const img_t *im = &g_images[p->img];
        int yline = p->line - g_top;
        if (yline + p->rows <= 0 || yline >= rows) continue;

        int src_y = 0;
        if (yline < 0) {
            src_y = -yline * ROW_H;
            yline = 0;
        }
        int avail = (rows - yline) * ROW_H;
        int draw_h = im->h - src_y;
        if (draw_h > avail) draw_h = avail;
        if (draw_h <= 0) continue;
        gui_image_draw(im->slot, 8, CONTENT_Y + 2 + yline * ROW_H, src_y, draw_h);
    }

    /* Number markers sit on top of the page text in the accent colour, which
     * is what makes them look like something you can click. */
    for (int i = 0; i < g_link_count; i++) {
        const link_t *lk = &g_links[i];
        if (lk->ml < g_top || lk->ml >= g_top + rows) continue;
        char mk[6];
        snprintf(mk, sizeof(mk), "[%d]", i + 1);
        int mlen = (int)strlen(mk);
        if (lk->mc + mlen > cols) continue;
        int mx = 8 + lk->mc * CHAR_W;
        int my = CONTENT_Y + 2 + (lk->ml - g_top) * ROW_H;
        gui_fill(mx, my, mlen * CHAR_W, ROW_H, C_BG);
        gui_text(mx, my, C_ACCENT, mk);
    }

    if (g_panel_open) draw_panel();

    /* Status bar. */
    gui_fill(0, h - STATUS_H, w, STATUS_H, C_PANEL);
    gui_fill(0, h - STATUS_H, w, 1, 0x0A1018);
    int sn = (int)strlen(g_status);
    if (sn > (w - 16) / CHAR_W) sn = (w - 16) / CHAR_W;
    if (sn < 0) sn = 0;
    if (sn > 0) text_chunks(8, h - STATUS_H + 2, C_DIM, g_status, sn);

    gui_flush();
    g_need_redraw = 0;
}

/* ---- networking --------------------------------------------------- */

static u64 now_ms(void) {
    mcube_time_t ts;
    if (mc_time(&ts) != 0) return 0;
    return ts.uptime_ms;
}

/* Parse "a.b.c.d" into a host byte order IPv4 address. */
static int parse_ipv4(const char *s, u32 *ip_out) {
    u32 ip = 0;
    int parts = 0;

    while (parts < 4) {
        int v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s - '0');
            if (v > 255) return -1;
            s++;
            digits++;
        }
        if (!digits) return -1;
        ip = (ip << 8) | (u32)v;
        parts++;
        if (parts < 4) {
            if (*s != '.') return -1;
            s++;
        }
    }
    if (*s != 0) return -1;
    *ip_out = ip;
    return 0;
}

/* Split a URL into host/port/path. Returns -1 on anything we cannot fetch. */
static int parse_url(const char *url, char *host, int hostcap, int *port,
                     char *path, int pathcap, int *local) {
    *local = 0;
    if (strcmp(url, "mcube://home") == 0 || strcmp(url, "mcube://home/") == 0) {
        *local = 1;
        return 0;
    }
    if (strncmp(url, "http://", 7) != 0) return -1;

    const char *p = url + 7;
    const char *hs = p;
    while (*p && *p != '/' && *p != ':') p++;
    int hl = (int)(p - hs);
    if (hl <= 0 || hl >= hostcap) return -1;
    memcpy(host, hs, (size_t)hl);
    host[hl] = 0;

    int prt = 80;
    if (*p == ':') {
        p++;
        if (*p < '0' || *p > '9') return -1;
        prt = 0;
        while (*p >= '0' && *p <= '9') {
            prt = prt * 10 + (*p - '0');
            if (prt > 65535) return -1;
            p++;
        }
    }
    if (*p == '/') {
        snprintf(path, (size_t)pathcap, "%s", p);
    } else if (*p == 0) {
        snprintf(path, (size_t)pathcap, "/");
    } else {
        return -1;
    }
    *port = prt;
    return 0;
}

static void pump_events(void) {
    gui_event_t ev;
    while (gui_poll(&ev) > 0) {
        if (ev.type == GUI_EVT_CLOSE) exit(0);
        if (ev.type == GUI_EVT_RESIZE) {
            rerender();
            draw();
        }
    }
}

/* One HTTP/1.0 request, staged into `buf` (capacity `cap`). Returns the number
 * of bytes received, or -1 with the status bar describing the failure unless
 * `quiet` is set (image fetches fall back to their alt text instead). */
static int http_fetch_into(const char *host, int port, const char *path,
                           char *buf, int cap, int quiet) {
    char msg[STATUS_MAX + 1];
    char req[512];
    u32 ip = 0;

    if (parse_ipv4(host, &ip) != 0) {
        if (mc_dns_lookup(host, &ip) != 0) {
            if (!quiet) {
                snprintf(msg, sizeof(msg), "DNS failed for %s", host);
                set_status(msg);
            }
            return -1;
        }
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        if (!quiet) set_status("socket() failed");
        return -1;
    }

    mcube_sockaddr_in_t sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = __builtin_bswap16((u16)port);
    sa.sin_addr = __builtin_bswap32(ip);

    if (connect(fd, &sa, sizeof(sa)) != 0) {
        if (!quiet) {
            snprintf(msg, sizeof(msg), "connect failed for %s:%d", host, port);
            set_status(msg);
        }
        close(fd);
        return -1;
    }

    int rn = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.0\r\n"
                      "Host: %s\r\n"
                      "User-Agent: MCubeBrowser/1.0\r\n"
                      "Connection: close\r\n"
                      "\r\n",
                      path, host);
    if (rn <= 0 || (size_t)rn >= sizeof(req)) {
        if (!quiet) set_status("URL is too long");
        close(fd);
        return -1;
    }

    int sent = 0;
    while (sent < rn) {
        ssize_t n = sendto(fd, req + sent, (size_t)(rn - sent), 0, NULL, 0);
        if (n <= 0) {
            if (!quiet) set_status("send failed");
            close(fd);
            return -1;
        }
        sent += (int)n;
    }

    u64 start = now_ms();
    int idle = 0;
    int total = 0;
    int timeout = 0;

    for (;;) {
        if (total >= cap) break;

        mcube_pollfd_t pfd;
        pfd.fd = fd;
        pfd.events = MCUBE_POLLIN;
        pfd.revents = 0;

        int pr = poll(&pfd, 1, 200);
        if (pr < 0) {
            if (!quiet) set_status("poll failed");
            close(fd);
            return -1;
        }
        if (pr > 0) {
            ssize_t n = recvfrom(fd, buf + total, (size_t)(cap - total),
                                 0, NULL, NULL);
            if (n <= 0) break;          /* end of stream */
            total += (int)n;
            idle = 0;
        } else {
            idle += 200;
        }

        u64 now = now_ms();
        if ((start != 0 && now != 0 && now - start >= LOAD_TIMEOUT_MS) ||
            idle >= LOAD_TIMEOUT_MS) {
            timeout = 1;
            break;
        }
        pump_events();
    }

    close(fd);
    if (timeout && total == 0) {
        if (!quiet) set_status("load timed out");
        return -1;
    }
    return total;
}

static int http_fetch(const char *host, int port, const char *path) {
    return http_fetch_into(host, port, path, g_rx, MAX_BODY, 0);
}

static int contains_ci(const char *s, int n, const char *lit) {
    int ln = (int)strlen(lit);
    for (int i = 0; i + ln <= n; i++) {
        int k = 0;
        while (k < ln) {
            char a = s[i + k];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (a != lit[k]) break;
            k++;
        }
        if (k == ln) return 1;
    }
    return 0;
}

/* Status line and headers are cut away; the body is copied to g_body. */
static int process_response(int total) {
    if (total <= 0) {
        set_status("empty response");
        return -1;
    }
    g_rx[total] = 0;

    if (total < 5 || memcmp(g_rx, "HTTP/", 5) != 0) {
        set_status("not an HTTP response");
        return -1;
    }

    int le = 0;
    while (le < total && g_rx[le] != '\r' && g_rx[le] != '\n') le++;

    char status[STATUS_MAX + 1];
    int sl = le < STATUS_MAX - 8 ? le : STATUS_MAX - 8;
    memcpy(status, g_rx, (size_t)sl);
    status[sl] = 0;

    int hend = -1;
    int skip = 0;
    for (int i = 0; i + 3 < total; i++) {
        if (g_rx[i] == '\r' && g_rx[i + 1] == '\n' &&
            g_rx[i + 2] == '\r' && g_rx[i + 3] == '\n') {
            hend = i;
            skip = 4;
            break;
        }
    }
    if (hend < 0) {
        for (int i = 0; i + 1 < total; i++) {
            if (g_rx[i] == '\n' && g_rx[i + 1] == '\n') {
                hend = i;
                skip = 2;
                break;
            }
        }
    }

    int body_off = (hend >= 0) ? hend + skip : total;
    int hdr_end = (hend >= 0) ? hend : total;
    int chunked = contains_ci(g_rx + le, hdr_end - le, "chunked");

    int blen = total - body_off;
    if (blen < 0) blen = 0;
    if (blen > MAX_BODY) blen = MAX_BODY;
    memcpy(g_body, g_rx + body_off, (size_t)blen);
    g_body[blen] = 0;
    g_body_len = blen;
    g_body_html = 1;

    if (chunked) snprintf(g_status, sizeof(g_status), "%s [chunked]", status);
    else snprintf(g_status, sizeof(g_status), "%s", status);

    g_need_redraw = 1;
    return 0;
}

/* ---- image loading ---------------------------------------------------- */

static int img_find(const char *url) {
    for (int i = 0; i < g_img_count; i++) {
        if (g_images[i].used && strcmp(g_images[i].url, url) == 0) return i;
    }
    return -1;
}

static void images_reset(void) {
    g_img_count = 0;
    g_place_count = 0;
    for (int i = 0; i < MAX_IMAGES; i++) g_images[i].used = 0;
}

/* Fetch, decode and scale one image into cache slot `slot`. Returns 0 on
 * success; every failure leaves the alt text in charge. */
static int load_one_image(const char *url, int slot) {
    if (slot < 0 || slot >= MAX_IMAGES || !g_img_px[slot]) return -1;

    char host[128];
    char path[MAX_COLS];
    int port = 80;
    int local = 0;

    if (parse_url(url, host, (int)sizeof(host), &port, path,
                  (int)sizeof(path), &local) != 0 || local) return -1;

    int total = http_fetch_into(host, port, path, g_img_raw, IMG_MAX_BYTES, 1);
    if (total <= 0) return -1;
    if (total < 5 || memcmp(g_img_raw, "HTTP/", 5) != 0) return -1;

    /* Cut the header off exactly like process_response does for pages. */
    int hend = -1, skip = 0;
    for (int i = 0; i + 3 < total; i++) {
        if (g_img_raw[i] == '\r' && g_img_raw[i + 1] == '\n' &&
            g_img_raw[i + 2] == '\r' && g_img_raw[i + 3] == '\n') {
            hend = i;
            skip = 4;
            break;
        }
    }
    if (hend < 0) {
        for (int i = 0; i + 1 < total; i++) {
            if (g_img_raw[i] == '\n' && g_img_raw[i + 1] == '\n') {
                hend = i;
                skip = 2;
                break;
            }
        }
    }
    if (hend < 0) return -1;

    const u8 *body = (const u8 *)g_img_raw + hend + skip;
    int blen = total - hend - skip;
    if (blen <= 0) return -1;

    int nw = 0, nh = 0;
    if (img_decode(body, blen, g_img_dec, &nw, &nh, g_img_aux, IMG_MAX_BYTES,
                   g_img_rows, IMG_DEC_MAX_PX * 4 + 4096) != 0) return -1;

    /* Fit the window width, never upscale, and cap the height so the bitmap
     * cannot push the page layout around. */
    int cw = gui_width() - 16;
    if (cw > IMG_MAX_W) cw = IMG_MAX_W;
    if (cw < 32) cw = 32;

    int dw = nw, dh = nh;
    if (dw > cw) {
        dh = (int)((long long)nh * cw / dw);
        dw = cw;
    }
    if (dh > IMG_MAX_H) dh = IMG_MAX_H;
    if (dw <= 0 || dh <= 0) return -1;

    /* Nearest-neighbour downscale into the page's slot buffer. */
    u32 *dst = g_img_px[slot];
    for (int y = 0; y < dh; y++) {
        int sy = (int)((long long)y * nh / dh);
        if (sy >= nh) sy = nh - 1;
        const u32 *srow = g_img_dec + (size_t)sy * (size_t)nw;
        u32 *drow = dst + (size_t)y * (size_t)dw;
        for (int x = 0; x < dw; x++) {
            int sx = (int)((long long)x * nw / dw);
            if (sx >= nw) sx = nw - 1;
            drow[x] = srow[sx];
        }
    }

    if (gui_image_load(slot, dw, dh, dst) != 0) return -1;

    img_t *im = &g_images[slot];
    snprintf(im->url, sizeof(im->url), "%s", url);
    im->used = 1;
    im->w = dw;
    im->h = dh;
    im->slot = slot;
    return 0;
}

/* Walk the page source once after a load, fetching every <img src>. Rendering
 * only looks the results up by URL, so a resize never re-fetches anything. */
static void images_load_page(void) {
    images_reset();
    if (!g_body || !g_body_html || g_body_len <= 0) return;
    if (!g_img_raw || !g_img_dec || !g_img_aux || !g_img_rows) return;

    /* The per-image progress line is temporary; put the HTTP status back so
     * the page ends up reporting what the server actually said. */
    char saved[STATUS_MAX + 1];
    snprintf(saved, sizeof(saved), "%s", g_status);

    int n = g_body_len;
    for (int i = 0; i + 4 < n && g_img_count < MAX_IMAGES; i++) {
        if (g_body[i] != '<') continue;

        int j = i + 1;
        if (j < n && g_body[j] == '/') continue;
        int ns = j;
        while (j < n && g_body[j] != '>' && g_body[j] != ' ' &&
               g_body[j] != '\t' && g_body[j] != '\r' &&
               g_body[j] != '\n' && g_body[j] != '/') j++;
        int nl = j - ns;
        if (nl <= 0 || !tag_is(g_body + ns, nl, "img")) continue;

        int k = j;
        while (k < n && g_body[k] != '>') k++;

        char src[URL_MAX + 1];
        attr_value(g_body, j, k, "src", src, (int)sizeof(src));
        i = k;                              /* resume after the tag */

        if (!src[0]) continue;
        char url[URL_MAX + 1];
        if (link_resolve(src, url, (int)sizeof(url)) != 0) continue;
        if (img_find(url) >= 0) continue;   /* same image referenced twice */

        set_status("loading images...");
        draw();
        if (load_one_image(url, g_img_count) == 0) g_img_count++;
    }
    set_status(saved);
}

/* ---- navigation --------------------------------------------------- */

static void hist_push(const char *url) {
    if (g_hist_count > 0 && g_hist_pos >= 0 &&
        strcmp(g_hist[g_hist_pos], url) == 0) return;

    if (g_hist_pos >= 0 && g_hist_pos + 1 < g_hist_count) {
        g_hist_count = g_hist_pos + 1;
    }
    if (g_hist_count >= HIST_MAX) {
        memmove(g_hist[0], g_hist[1],
                sizeof(g_hist[0]) * (size_t)(HIST_MAX - 1));
        g_hist_count = HIST_MAX - 1;
    }
    snprintf(g_hist[g_hist_count], URL_MAX + 1, "%s", url);
    g_hist_count++;
    g_hist_pos = g_hist_count - 1;
}

static const char HOME_PAGE[] =
    "MCubeAIOS Browser\n"
    "=================\n"
    "\n"
    "Welcome. This is the browser that ships with MCubeAIOS: a\n"
    "small text client that speaks plain HTTP/1.0 and draws HTML\n"
    "with the kernel's 8x16 bitmap font.\n"
    "\n"
    "ADDRESS BAR\n"
    "-----------\n"
    "Type into the field at the top of the window.\n"
    "\n"
    "  printable ASCII   append to the address\n"
    "  Backspace         delete the last character\n"
    "  Enter             load the address\n"
    "\n"
    "The Back, Home and Reload buttons sit on the left, GO on the\n"
    "right. All of them also respond to the mouse.\n"
    "\n"
    "WHAT THIS BROWSER CAN DO\n"
    "-----------------------\n"
    "  mcube://home         this built-in page\n"
    "  http://host/         plain HTTP, port 80\n"
    "  http://host:port/    plain HTTP, another port\n"
    "\n"
    "Hosts may be dotted-quad IPv4 addresses such as 10.0.2.2, or\n"
    "names resolved by the MCubeAIOS DNS client. Each load sends a\n"
    "single GET and reads until the server closes the stream.\n"
    "\n"
    "Pictures: an <img src> that points at a PNG, BMP or PPM file is\n"
    "fetched and shown inline, scaled down to the window width. At\n"
    "most two pictures per page are cached; the rest keep their alt\n"
    "text.\n"
    "\n"
    "Not supported: HTTPS, cookies, POST, JavaScript, CSS, frames\n"
    "and colours. The text inside the HTML is kept in one narrow\n"
    "column.\n"
    "\n"
    "TRY IT\n"
    "------\n"
    "  http://10.0.2.2/      QEMU user-mode host loopback\n"
    "  http://example.com/   a small public HTTP test page\n"
    "\n"
    "READING\n"
    "-------\n"
    "  Up / Down       scroll one line\n"
    "  PgUp / PgDn     scroll one page\n"
    "  Home            jump to the top of the page\n"
    "\n"
    "The status bar shows \"loading...\", the server's HTTP status\n"
    "line, or the error that stopped the load.\n"
    "\n"
    "MCubeAIOS 0.1.0 \"Genesis\" (x86_64)\n"
    "Browser 1.0  --  built-in start page\n";

/* Pull the <title> of the current HTML page into out. Returns 0 when the body
 * is plain text or has no title element. */
static int html_title(char *out, int cap) {
    int n = g_body_len;

    out[0] = 0;
    if (!g_body || !g_body_html || n <= 0) return 0;

    for (int i = 0; i + 6 < n; i++) {
        if (g_body[i] != '<') continue;

        int j = i + 1;
        int closing = 0;
        if (j < n && g_body[j] == '/') { closing = 1; j++; }
        int ns = j;
        while (j < n && g_body[j] != '>' && g_body[j] != ' ' &&
               g_body[j] != '\t' && g_body[j] != '\r' && g_body[j] != '\n') j++;
        int nl = j - ns;
        if (closing || nl <= 0) continue;
        if (!tag_is(g_body + ns, nl, "title")) continue;

        int k = j;
        while (k < n && g_body[k] != '>') k++;
        k++;

        int o = 0;
        while (k < n && o < cap && g_body[k] != '<') {
            char c = g_body[k++];
            if (c == '\r' || c == '\n' || c == '\t') c = ' ';
            if (c >= 0x20 && c < 0x7F) out[o++] = c;
        }
        while (o > 0 && out[o - 1] == ' ') o--;
        int lead = 0;
        while (lead < o && out[lead] == ' ') lead++;
        if (lead > 0) {
            memmove(out, out + lead, (size_t)(o - lead));
            o -= lead;
        }
        out[o] = 0;
        return o > 0;
    }
    return 0;
}

/* Fallback title: the address without its scheme. */
static void title_from_url(char *out, int cap) {
    const char *p = g_url;

    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    } else if (strncmp(p, "mcube://", 8) == 0) {
        p += 8;
        if (strcmp(p, "home") == 0 || strcmp(p, "home/") == 0) {
            snprintf(out, (size_t)cap, "%s", "Home");
            return;
        }
    }
    if (*p == 0) snprintf(out, (size_t)cap, "%s", "New Tab");
    else snprintf(out, (size_t)cap, "%s", p);
}

/* ---- tabs --------------------------------------------------------- */

/* Remember what the active tab should restore later. */
static void tab_save_state(void) {
    if (g_tab_cur < 0 || g_tab_cur >= g_tab_count) return;
    snprintf(g_tabs[g_tab_cur].url, sizeof(g_tabs[g_tab_cur].url),
             "%s", g_url);
    g_tabs[g_tab_cur].top = g_top;
}

/* Switch to tab idx. The page buffer is shared, so the tab's address is
 * loaded again; the scroll offset is restored afterwards. */
static void tab_activate(int idx, int restore) {
    char url[URL_MAX + 1];
    int top;

    if (idx < 0 || idx >= g_tab_count) return;

    top = g_tabs[idx].top;
    snprintf(url, sizeof(url), "%s", g_tabs[idx].url);
    if (url[0] == 0) snprintf(url, sizeof(url), "%s", "mcube://home");

    g_tab_cur = idx;
    load_url_inner(url, 0);

    if (restore) {
        g_top = top;
        clamp_top();
        g_tabs[g_tab_cur].top = g_top;
    }
    g_need_redraw = 1;
}

static void tab_new(void) {
    int idx;

    if (g_tab_count >= MAX_TABS) {
        set_status("tab limit reached (8)");
        return;
    }

    tab_save_state();
    idx = g_tab_count;
    memset(&g_tabs[idx], 0, sizeof(g_tabs[idx]));
    snprintf(g_tabs[idx].url, sizeof(g_tabs[idx].url), "%s", "mcube://home");
    snprintf(g_tabs[idx].title, sizeof(g_tabs[idx].title), "%s", "New Tab");
    g_tab_count++;
    g_tab_cur = idx;
    g_top = 0;
    load_url("mcube://home");
    g_need_redraw = 1;
}

static void tab_close(int idx) {
    if (idx < 0 || idx >= g_tab_count) return;

    if (g_tab_count <= 1) {
        set_status("last tab stays open");
        return;
    }

    for (int k = idx; k + 1 < g_tab_count; k++) g_tabs[k] = g_tabs[k + 1];
    g_tab_count--;

    if (g_tab_cur > idx) {
        g_tab_cur--;
    } else if (g_tab_cur == idx) {
        int next = idx < g_tab_count ? idx : g_tab_count - 1;
        tab_activate(next, 1);
        return;
    }
    g_need_redraw = 1;
}

/* ---- bookmarks ---------------------------------------------------- */

/* /BOOKMARKS.TXT is one "title|url" pair per line; a missing file is simply
 * an empty list. */
static void bookmarks_load(void) {
    static char buf[4096];
    ssize_t got;
    int n;

    g_bm_count = 0;

    int fd = open(BOOKMARK_PATH, VFS_O_RDONLY);
    if (fd < 0) return;
    got = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (got <= 0) return;
    n = (int)got;
    buf[n] = 0;

    for (int i = 0; i < n && g_bm_count < MAX_BOOKMARKS; ) {
        int e = i;
        while (e < n && buf[e] != '\n') e++;

        int ll = e - i;
        while (ll > 0 && (buf[i + ll - 1] == '\r' || buf[i + ll - 1] == ' ')) ll--;
        buf[i + ll] = 0;

        char *line = buf + i;
        char *bar = strchr(line, '|');
        if (bar) {
            *bar = 0;
            if (bar[1] != 0) {
                snprintf(g_bm_title[g_bm_count], BM_TITLE_MAX + 1, "%s",
                         line[0] ? line : bar + 1);
                snprintf(g_bm_url[g_bm_count], URL_MAX + 1, "%s", bar + 1);
                g_bm_count++;
            }
        }
        i = e + 1;
    }
}

static void bookmarks_save(void) {
    int fd = open(BOOKMARK_PATH, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    int ok = 1;

    if (fd < 0) {
        set_status("cannot write /BOOKMARKS.TXT");
        return;
    }

    for (int i = 0; i < g_bm_count && ok; i++) {
        char line[BM_TITLE_MAX + URL_MAX + 8];
        int n = snprintf(line, sizeof(line), "%s|%s\n", g_bm_title[i],
                         g_bm_url[i]);
        if (n <= 0) { ok = 0; break; }
        if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;

        int done = 0;
        while (done < n) {
            ssize_t w = write(fd, line + done, (size_t)(n - done));
            if (w <= 0) { ok = 0; break; }
            done += (int)w;
        }
    }
    close(fd);
    if (!ok) set_status("bookmark write failed");
}

static void bookmark_add(void) {
    char title[TAB_TITLE_MAX + 1];

    if (g_url[0] == 0) {
        set_status("nothing to bookmark");
        return;
    }
    if (bookmark_has(g_url)) {
        set_status("already bookmarked");
        return;
    }
    if (g_bm_count >= MAX_BOOKMARKS) {
        set_status("bookmark list full (12)");
        return;
    }

    /* The tab title only describes the loaded page, so it is used only while
     * the address bar still matches it. */
    if (g_tab_cur >= 0 && g_tab_cur < g_tab_count &&
        strcmp(g_tabs[g_tab_cur].url, g_url) == 0) {
        snprintf(title, sizeof(title), "%s", g_tabs[g_tab_cur].title);
    } else {
        title_from_url(title, TAB_TITLE_MAX);
    }
    if (title[0] == 0) snprintf(title, sizeof(title), "%s", g_url);

    snprintf(g_bm_title[g_bm_count], BM_TITLE_MAX + 1, "%s", title);
    snprintf(g_bm_url[g_bm_count], URL_MAX + 1, "%s", g_url);
    g_bm_count++;

    bookmarks_save();
    set_status("bookmark added to /BOOKMARKS.TXT");
}

/* Drop bookmark i, shift the rest down and rewrite /BOOKMARKS.TXT. */
static void bookmark_remove(int i) {
    if (i < 0 || i >= g_bm_count) return;

    for (int k = i; k < g_bm_count - 1; k++) {
        memcpy(g_bm_title[k], g_bm_title[k + 1], sizeof(g_bm_title[k]));
        memcpy(g_bm_url[k], g_bm_url[k + 1], sizeof(g_bm_url[k]));
    }
    g_bm_count--;
    memset(g_bm_title[g_bm_count], 0, sizeof(g_bm_title[0]));
    memset(g_bm_url[g_bm_count], 0, sizeof(g_bm_url[0]));

    bookmarks_save();
    set_status("bookmark removed");
}

static void after_load(void) {
    char title[TAB_TITLE_MAX + 1];

    if (!html_title(title, TAB_TITLE_MAX)) title_from_url(title, TAB_TITLE_MAX);
    if (g_tab_cur >= 0 && g_tab_cur < g_tab_count) {
        snprintf(g_tabs[g_tab_cur].title, sizeof(g_tabs[g_tab_cur].title),
                 "%s", title);
        snprintf(g_tabs[g_tab_cur].url, sizeof(g_tabs[g_tab_cur].url),
                 "%s", g_url);
        g_tabs[g_tab_cur].top = g_top;
    }

    draw();
    printf("USER_BROWSER_PAGE: %d lines, status %s\n", g_nlines, g_status);
}

static void load_url_inner(const char *url, int push) {
    char addr[URL_MAX + 1];
    char host[128];
    char path[MAX_COLS];
    int port = 80;
    int local = 0;

    snprintf(addr, sizeof(addr), "%s", url);
    snprintf(g_url, sizeof(g_url), "%s", addr);

    if (parse_url(addr, host, (int)sizeof(host), &port, path,
                  (int)sizeof(path), &local) != 0) {
        set_status("unsupported URL (mcube://home or http://host/path)");
        after_load();
        return;
    }

    g_top = 0;

    if (local) {
        g_base_host[0] = 0;
        g_base_port = 80;
        snprintf(g_base_path, sizeof(g_base_path), "%s", "/");
        int hl = (int)strlen(HOME_PAGE);
        if (hl > MAX_BODY) hl = MAX_BODY;
        memcpy(g_body, HOME_PAGE, (size_t)hl);
        g_body[hl] = 0;
        g_body_len = hl;
        g_body_html = 0;
        images_reset();
        rerender();
        set_status("mcube://home (built-in page)");
    } else {
        set_status("loading...");
        draw();

        int total = http_fetch(host, port, path);
        if (total < 0) {                    /* keep the old page */
            after_load();
            return;
        }
        if (process_response(total) != 0) {
            after_load();
            return;
        }
        /* Remember where this page came from so relative hrefs on it can be
         * turned into absolute addresses. */
        snprintf(g_base_host, sizeof(g_base_host), "%s", host);
        g_base_port = port;
        snprintf(g_base_path, sizeof(g_base_path), "%s", path);
        /* Fetch the inline pictures before laying the page out, so the first
         * render already reserves rows for the ones that decoded. */
        images_load_page();
        rerender();
    }

    if (push) hist_push(addr);
    after_load();
}

static void load_url(const char *url) {
    load_url_inner(url, 1);
}

static void go_back(void) {
    if (g_hist_pos <= 0) {
        set_status("no earlier page in history");
        g_need_redraw = 1;
        return;
    }
    g_hist_pos--;
    load_url_inner(g_hist[g_hist_pos], 0);
}

/* ---- input -------------------------------------------------------- */

static int in_rect(const rect_t *r, int x, int y) {
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

static void scroll_by(int lines) {
    g_top += lines;
    clamp_top();
    g_need_redraw = 1;
}

/* Link under a click in the page area. The number marker after the anchor
 * text counts as part of the link, so clicking "[3]" works as well. */
static int link_at(int x, int y) {
    if (y < CONTENT_Y + 2 || y >= gui_height() - STATUS_H) return -1;
    if (x < 8) return -1;

    int line = g_top + (y - CONTENT_Y - 2) / ROW_H;
    int col = (x - 8) / CHAR_W;

    for (int i = 0; i < g_link_count; i++) {
        const link_t *lk = &g_links[i];
        if (line < lk->l0 || line > lk->l1) continue;
        if (line == lk->l0 && col < lk->c0) continue;

        int last = MAX_COLS;
        if (line == lk->l1) last = lk->c1;
        if (line == lk->ml) {
            char mk[6];
            snprintf(mk, sizeof(mk), "[%d]", i + 1);
            last = lk->mc + (int)strlen(mk) - 1;
        }
        if (col > last) continue;
        return i;
    }
    return -1;
}

/* A bare number in the address bar follows that numbered link. */
static int follow_number(const char *text) {
    int v = 0, digits = 0;

    for (const char *p = text; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
        v = v * 10 + (*p - '0');
        if (v > MAX_LINKS) return 0;
        digits++;
    }
    if (digits == 0 || v < 1 || v > g_link_count) return 0;

    load_url(g_links[v - 1].url);
    return 1;
}

/* Clicks on the bookmarks/history panel. Returns 1 when the event is consumed,
 * including a click outside the panel, which simply closes it. */
static int panel_click(int x, int y) {
    if (x < g_panel.x || x >= g_panel.x + g_panel.w ||
        y < g_panel.y || y >= g_panel.y + g_panel.h) {
        g_panel_open = 0;
        g_need_redraw = 1;
        return 1;
    }

    if (x >= g_panel.x + g_panel.w - 24 && y < g_panel.y + 24) {
        g_panel_open = 0;
        g_need_redraw = 1;
        return 1;
    }

    int row = (y - panel_first_y()) / PANEL_ROW_H;
    if (row >= 0 && row < panel_rows()) {
        int cw = panel_colw();
        int lx = panel_left_x();
        if (x >= lx && x < lx + cw && row < g_bm_count) {
            if (x >= lx + cw - 16) {
                bookmark_remove(row);
            } else {
                g_panel_open = 0;
                load_url(g_bm_url[row]);
            }
            g_need_redraw = 1;
            return 1;
        }
        int rx = panel_right_x();
        if (x >= rx && x < rx + cw && row < g_hist_count) {
            int j = g_hist_count - 1 - row;
            g_panel_open = 0;
            load_url(g_hist[j]);
            return 1;
        }
    }

    g_need_redraw = 1;
    return 1;
}

static void handle_key(u32 code) {
    /* Ctrl+T / Ctrl+W / Ctrl+B. A driver that folds Ctrl into the key reports
     * 0x14 / 0x17 / 0x02 instead of the letter, so both forms are accepted. */
    if (g_ctrl) {
        if (code == 't' || code == 'T' || code == 0x14) { tab_new(); return; }
        if (code == 'w' || code == 'W' || code == 0x17) {
            tab_close(g_tab_cur);
            return;
        }
        if (code == 'b' || code == 'B' || code == 0x02) { bookmark_add(); return; }
        if (code == 'm' || code == 'M' || code == 0x0D) {
            g_panel_open = !g_panel_open;
            g_need_redraw = 1;
            return;
        }
        return;                     /* other Ctrl chords are not text */
    }
    if (code == 0x14) { tab_new(); return; }
    if (code == 0x17) { tab_close(g_tab_cur); return; }
    if (code == 0x02) { bookmark_add(); return; }

    if (code == 0x1B) {             /* Esc dismisses the overlay/selection */
        if (g_panel_open) g_panel_open = 0;
        else if (g_url_sel) g_url_sel = 0;
        else return;
        g_need_redraw = 1;
        return;
    }

    if (code == '\n') {
        g_url_sel = 0;
        if (follow_number(g_url)) return;
        if (g_url[0] == 0) set_status("type a URL first");
        else load_url(g_url);
    } else if (code == '\b') {
        if (g_url_sel) {
            g_url[0] = 0;
            g_url_sel = 0;
        } else {
            size_t len = strlen(g_url);
            if (len > 0) g_url[len - 1] = 0;
        }
        g_need_redraw = 1;
    } else if (code >= 0x20 && code <= 0x7E) {
        size_t len = strlen(g_url);
        if (g_url_sel) {            /* replace the selected address */
            g_url[0] = 0;
            len = 0;
            g_url_sel = 0;
        }
        if (len < URL_MAX) {
            g_url[len] = (char)code;
            g_url[len + 1] = 0;
        }
        g_need_redraw = 1;
    } else if (code == INPUT_KEY_UP) {
        scroll_by(-1);
    } else if (code == INPUT_KEY_DOWN) {
        scroll_by(1);
    } else if (code == INPUT_KEY_PGUP) {
        scroll_by(-page_rows());
    } else if (code == INPUT_KEY_PGDN) {
        scroll_by(page_rows());
    } else if (code == INPUT_KEY_HOME) {
        g_top = 0;
        g_need_redraw = 1;
    } else if (code == INPUT_KEY_F5) {
        load_url_inner(g_url, 0);
    }
}

static void handle_mouse(const gui_event_t *ev) {
    if (!(ev->buttons & 1)) return;
    layout();

    /* The overlay owns every click while it is open, including the one that
     * dismisses it. */
    if (g_panel_open && panel_click(ev->x, ev->y)) return;

    /* Tab strip: close boxes first, then the tab body, then "+". */
    if (ev->y < TABBAR_H) {
        for (int i = 0; i < g_tab_vis; i++) {
            if (in_rect(&g_tab_close[i], ev->x, ev->y)) {
                tab_close(i);
                return;
            }
        }
        for (int i = 0; i < g_tab_vis; i++) {
            if (in_rect(&g_tab_rect[i], ev->x, ev->y)) {
                if (i != g_tab_cur) {
                    tab_save_state();
                    tab_activate(i, 1);
                }
                return;
            }
        }
        if (in_rect(&g_btn_newtab, ev->x, ev->y)) tab_new();
        return;
    }

    /* Bookmark bar. */
    if (ev->y >= TABBAR_H + TOOLBAR_H && ev->y < CONTENT_Y) {
        for (int i = 0; i < g_bm_vis; i++) {
            if (in_rect(&g_bm_rect[i], ev->x, ev->y)) {
                load_url(g_bm_url[i]);
                return;
            }
        }
        return;
    }

    if (in_rect(&g_btn_back, ev->x, ev->y)) go_back();
    else if (in_rect(&g_btn_home, ev->x, ev->y)) load_url("mcube://home");
    else if (in_rect(&g_btn_reload, ev->x, ev->y)) load_url_inner(g_url, 0);
    else if (in_rect(&g_btn_star, ev->x, ev->y)) bookmark_add();
    else if (in_rect(&g_btn_menu, ev->x, ev->y)) {
        g_panel_open = !g_panel_open;
        g_need_redraw = 1;
    } else if (in_rect(&g_url_box, ev->x, ev->y)) {
        g_url_sel = 1;              /* next keystroke replaces the address */
        g_need_redraw = 1;
    } else if (in_rect(&g_btn_go, ev->x, ev->y)) {
        if (g_url[0] == 0) set_status("type a URL first");
        else load_url(g_url);
    } else {
        int li = link_at(ev->x, ev->y);
        if (li >= 0) load_url(g_links[li].url);
    }
}

static void handle_event(const gui_event_t *ev) {
    switch (ev->type) {
    case GUI_EVT_KEY: {
        u32 base = ev->code & ~INPUT_KEY_RELEASE;
        int released = (ev->code & INPUT_KEY_RELEASE) != 0;

        if (base == INPUT_KEY_LCTRL || base == INPUT_KEY_RCTRL) {
            g_ctrl = released ? 0 : 1;
            break;
        }
        if (released) break;        /* only presses become actions */
        handle_key(ev->code);
        break;
    }

    case GUI_EVT_MOUSE:
        handle_mouse(ev);
        break;

    case GUI_EVT_RESIZE:
        rerender();
        break;

    case GUI_EVT_CLOSE:
        exit(0);

    default:
        break;
    }
}

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)envp;

    g_lines = calloc(MAX_LINES, MAX_COLS);
    g_body = malloc(MAX_BODY + 1);
    g_rx = malloc(MAX_BODY + 1);
    if (!g_lines || !g_body || !g_rx) {
        printf("USER_BROWSER_FAIL: out of memory\n");
        return 1;
    }

    /* Picture buffers, allocated once and reused for every page. A short
     * allocation only disables images; the text browser still works. */
    g_img_raw = malloc(IMG_MAX_BYTES + 1);
    g_img_aux = malloc(IMG_MAX_BYTES);
    g_img_rows = malloc((size_t)IMG_DEC_MAX_PX * 4 + 4096);
    g_img_dec = malloc((size_t)IMG_DEC_MAX_PX * 4);
    for (int i = 0; i < MAX_IMAGES; i++) {
        g_img_px[i] = malloc((size_t)IMG_MAX_W * IMG_MAX_H * 4);
    }

    if (gui_init("Browser", WIN_W, WIN_H) != 0) {
        printf("USER_BROWSER_FAIL: gui_init\n");
        return 1;
    }
    printf("USER_BROWSER_OK: gui ready\n");

    bookmarks_load();

    /* An explicit address on the command line skips the start page, which is
     * how the desktop or the shell can hand the browser a page directly. */
    if (argc > 1 && argv && argv[1] && argv[1][0]) {
        snprintf(g_url, sizeof(g_url), "%s", argv[1]);
        load_url(g_url);
    } else {
        load_url("mcube://home");
    }

    for (;;) {
        gui_event_t ev;
        int rc = gui_wait(&ev, 200);
        if (rc < 0) exit(0);
        if (rc > 0) handle_event(&ev);
        if (g_need_redraw) draw();
    }
}
