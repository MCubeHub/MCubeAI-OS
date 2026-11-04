/* Settings: a Windows 11 style two-pane settings window.
 *
 * The left rail is a fixed 190 px navigation list and the right pane shows one
 * page at a time. All geometry lives in the small rect helpers below; drawing
 * and mouse hit testing call exactly the same helpers, so they cannot drift
 * apart when the layout changes.
 *
 * Only the primitives from <mcube/gui.h> are used (clear, fill, rect, text).
 * The shared font is an 8x16 ASCII bitmap, so every drawn string is ASCII and
 * every position is chosen on an 8 px grid.
 */

#include <mcube/libc.h>
#include <mcube/gui.h>
#include <mcube/fb.h>

#define PAGE_COUNT 17

/* ---- geometry -------------------------------------------------------- */

#define TITLE_H    26
#define NAV_W      190
#define NAV_TOP    52
#define NAV_ROW_H  26
#define NAV_PITCH  30

#define CONTENT_X  214
#define KEY_W      150
#define ROW_H      24
#define CONTENT_TOP 96

/* Every page that carries switches puts them in the same block, so the draw
 * and hit-test code share one anchor instead of guessing per page. */
#define FEAT_TOP   300
#define FEAT_ROW   46

#define BTN_W      140
#define BTN_H      32
#define BIG_W      200
#define BIG_H      40

#define MAX_BTNS   10

/* ---- colours --------------------------------------------------------- */

#define C_BODY 0xE6ECF5
#define C_DIM  0x93A1B5
#define C_RULE 0x2A3646
#define C_WARN 0xF59E0B
#define C_OK   0x22C55E

/* Wallpaper choices mirror the shell's palettes; the swatch shows the top
 * colour so the preview matches what the desktop actually paints. */
static const u32 g_wall_swatch[4] = {
    0x05080F, 0x03100C, 0x120A06, 0x0A0C10,
};
static const char *g_wall_names[4] = {
    "Nebula", "Aurora", "Ember", "Slate",
};

static const u32 g_accents[5] = {
    0x3B82F6, 0x14B8A6, 0x8B5CF6, 0xF59E0B, 0xF43F5E,
};
static const char *g_accent_names[5] = {
    "Blue", "Teal", "Violet", "Amber", "Rose",
};

static int g_theme;           /* wallpaper index */
static int g_accent;
static u32 g_bg = 0x10151E;
static u32 g_panel = 0x1A2230;
static u32 g_sel = 0x243040;
static u32 g_accent_rgb = 0x3B82F6;
static char g_theme_status[64] = "Applies to the desktop immediately.";

/* ---- pages ----------------------------------------------------------- */

/* The categories follow the Windows 11 Settings rail, with the MCubeAIOS
 * specific pages (Display, Power, About) kept at the end where they do not
 * interrupt the familiar ordering. */
enum {
    PG_SYSTEM = 0,
    PG_BLUETOOTH,
    PG_NETWORK,
    PG_DISPLAY,
    PG_PERSONAL,
    PG_APPS,
    PG_ACCOUNTS,
    PG_TIME,
    PG_GAMING,
    PG_ACCESSIBILITY,
    PG_PRIVACY,
    PG_UPDATE,
    PG_POWER,
    PG_ABOUT,
    PG_STORAGE,
    PG_SHORTCUTS,
    PG_STATE,
};

static const char *g_page_names[PAGE_COUNT] = {
    "System", "Bluetooth & devices", "Network & internet", "Display",
    "Personalisation", "Apps", "Accounts", "Time & language",
    "Gaming", "Accessibility", "Privacy & security", "Windows Update",
    "Power", "About", "Storage", "Keyboard shortcuts", "Files & state",
};

static const char *g_page_sub[PAGE_COUNT] = {
    "Device information and quick tools",
    "Radios and paired hardware",
    "Adapter status and the DNS resolver test",
    "Framebuffer and desktop geometry",
    "Theme background and accent colour",
    "Installed programs and defaults",
    "Your local account and sign-in options",
    "Clock, date and regional format",
    "Game mode and the overlay",
    "Vision, hearing and keyboard aids",
    "Permissions and diagnostics",
    "Build channel and update checks",
    "Restart or shut down the machine",
    "Build, AI layer and licence",
    "Memory, program footprint and state files",
    "Every shortcut this build understands",
    "Root directory and the files the shell keeps",
};

static int g_page = PG_SYSTEM;
static int g_need_redraw = 1;

/* ---- live state ------------------------------------------------------ */

static mcube_hw_info_t g_hw;
static int g_have_hw;

static mcube_fb_info_t g_fb;
static int g_have_fb;

static mcube_time_t g_time;
static int g_have_time;

static mcube_net_info_t g_net;
static int g_have_net;

static int g_procs;

static int g_power_confirm;   /* 0 = idle, 1 = restart armed, 2 = shutdown */
static char g_power_status[64] = "Select an action.";

static int g_dns_state;       /* 0 idle, 1 resolved, 2 failed, 3 pending */
static char g_dns_status[160] = "Resolver: not tested";

static char g_update_status[80] = "No update check has run yet.";
static char g_update_checked[40];

/* Real device telemetry: the USB stack and the power supply are the two
 * subsystems whose state users actually check, so both pages read them live
 * instead of printing constants. */
static mcube_usb_info_t g_usb;
static int g_have_usb;
static mcube_battery_info_t g_bat;
static int g_have_bat;

/* Inventory scanned from the VFS every few ticks, not on every repaint. */
static int g_scan_age;
static int g_bin_count;
static u64 g_bin_bytes;
static char g_bin_names[160];
static int g_root_n;
static char g_root_name[12][32];
static u64 g_root_size[12];
static u32 g_root_type[12];

/* ---- rect helpers (single source of truth for draw + hit tests) ------ */

typedef struct {
    int x, y, w, h;
} rect_t;

static rect_t rect_make(int x, int y, int w, int h) {
    rect_t r;
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    return r;
}

static int rect_has(rect_t r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static rect_t nav_row_rect(int i) {
    return rect_make(0, NAV_TOP + i * NAV_PITCH, NAV_W, NAV_ROW_H);
}

static rect_t button_rect(int y, int col) {
    return rect_make(CONTENT_X + col * (BTN_W + 8), y, BTN_W, BTN_H);
}

static rect_t big_button_rect(int i) {
    return rect_make(CONTENT_X, 160 + i * 48, BIG_W, BIG_H);
}

static rect_t theme_rect(int i) {
    return rect_make(CONTENT_X + i * 64, 120, 56, 36);
}

static rect_t accent_rect(int i) {
    return rect_make(CONTENT_X + i * 56, 208, 40, 28);
}

static rect_t change_button_rect(void) {
    return rect_make(CONTENT_X, 312, BTN_W, BTN_H);
}

/* ---- small helpers --------------------------------------------------- */

static void text_clip(int x, int y, u32 rgb, const char *s, int max_chars) {
    int n;
    if (!s) return;
    n = (int)strlen(s);
    if (max_chars < 1) max_chars = 1;
    if (n > max_chars) n = max_chars;
    gui_textn(x, y, rgb, s, n);
}

static u32 shade(u32 c) {
    u32 r = (c >> 16) & 0xFFu;
    u32 g = (c >> 8) & 0xFFu;
    u32 b = c & 0xFFu;
    r = r * 3 / 4;
    g = g * 3 / 4;
    b = b * 3 / 4;
    return (r << 16) | (g << 8) | b;
}

static void kv(int y, const char *key, const char *value) {
    int max = (gui_width() - (CONTENT_X + KEY_W) - 24) / 8;
    gui_text(CONTENT_X, y, C_DIM, key);
    text_clip(CONTENT_X + KEY_W, y, C_BODY, value, max);
}

static void section(int y, const char *title) {
    gui_fill(CONTENT_X, y + 6, 3, 12, g_accent_rgb);
    gui_text(CONTENT_X + 12, y, g_accent_rgb, title);
}

static void gib_text(u64 bytes, char *out, size_t size) {
    u32 tenths;
    if (!bytes) {
        snprintf(out, size, "unknown");
        return;
    }
    tenths = (u32)((bytes * 10ULL) / GiB);
    snprintf(out, size, "%u.%u GiB", tenths / 10, tenths % 10);
}

static void size_text(u64 bytes, char *out, size_t size) {
    if (bytes < 1024ULL) {
        snprintf(out, size, "%u B", (u32)bytes);
    } else if (bytes < 1024ULL * 1024ULL) {
        snprintf(out, size, "%u KiB", (u32)(bytes / 1024ULL));
    } else {
        snprintf(out, size, "%u.%u MiB",
                 (u32)(bytes / (1024ULL * 1024ULL)),
                 (u32)((bytes % (1024ULL * 1024ULL)) * 10ULL /
                       (1024ULL * 1024ULL)));
    }
}

/* VFS names come back upper case from the FAT images, so the suffix test is
 * case-insensitive. */
static int ends_with(const char *s, const char *suffix) {
    int n = (int)strlen(s);
    int m = (int)strlen(suffix);
    if (m > n) return 0;
    for (int i = 0; i < m; i++) {
        char a = s[n - m + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

/* Walk /bin and total the .lgy images. The count is shown on Apps and
 * Storage instead of a constant that drifts as programs are added. */
static void scan_bin(void) {
    mcube_dirent_t e;
    int count = 0;
    u64 bytes = 0;
    int n = 0;

    g_bin_names[0] = 0;

    for (int i = 0; i < 80; i++) {
        char path[96];
        mcube_stat_t st;

        if (mc_readdir("/bin", i, &e) != 0) break;
        if (!ends_with(e.name, ".lgy")) continue;

        count++;
        snprintf(path, sizeof(path), "/bin/%s", e.name);
        st.size = 0;
        st.type = 0;
        st.mode = 0;
        if (stat(path, &st) == 0) bytes += st.size;

        if (count <= 4) {
            int len = (int)strlen(e.name) - 4;
            if (len > 14) len = 14;
            if (n + len + 3 < (int)sizeof(g_bin_names)) {
                if (n > 0) {
                    g_bin_names[n++] = ',';
                    g_bin_names[n++] = ' ';
                }
                memcpy(g_bin_names + n, e.name, (size_t)len);
                n += len;
                g_bin_names[n] = 0;
            }
        }
    }

    g_bin_count = count;
    g_bin_bytes = bytes;
}

/* A shallow listing of "/" with each entry's size, so the state files the
 * shell keeps are visible without a terminal. */
static void scan_root(void) {
    mcube_dirent_t e;
    int shown = 0;

    for (int i = 0; i < 24 && shown < 12; i++) {
        char path[64];
        mcube_stat_t st;

        if (mc_readdir("/", i, &e) != 0) break;
        if (strcmp(e.name, ".") == 0 || strcmp(e.name, "..") == 0) continue;

        snprintf(path, sizeof(path), "/%s", e.name);
        st.size = 0;
        st.type = 0;
        st.mode = 0;
        stat(path, &st);

        snprintf(g_root_name[shown], sizeof(g_root_name[0]), "%s", e.name);
        g_root_size[shown] = st.size;
        g_root_type[shown] = st.type;
        shown++;
    }

    g_root_n = shown;
}

static void uptime_text(char *out, size_t size) {
    u32 s, d, rem;
    if (!g_have_time) {
        snprintf(out, size, "unavailable");
        return;
    }
    s = (u32)(g_time.uptime_ms / 1000ULL);
    d = s / 86400u;
    rem = s % 86400u;
    snprintf(out, size, "%u d %02u:%02u:%02u", d, rem / 3600u,
             (rem / 60u) % 60u, rem % 60u);
}

static u32 uptime_seconds(void) {
    if (!g_have_time) return 0;
    return (u32)(g_time.uptime_ms / 1000ULL);
}

static void ip_text(u32 ip, char *out, size_t size) {
    snprintf(out, size, "%u.%u.%u.%u",
             (ip >> 24) & 0xFFu, (ip >> 16) & 0xFFu,
             (ip >> 8) & 0xFFu, ip & 0xFFu);
}

static void mac_text(const u8 *m, char *out, size_t size) {
    snprintf(out, size, "%02X:%02X:%02X:%02X:%02X:%02X",
             (unsigned)m[0], (unsigned)m[1], (unsigned)m[2],
             (unsigned)m[3], (unsigned)m[4], (unsigned)m[5]);
}

static const char *format_name(u32 f) {
    switch (f) {
    case 0: return "RGBA8888";
    case 1: return "BGRA8888";
    case 2: return "BITMASK";
    case 3: return "BLT only";
    default: return "unknown";
    }
}

/* ---- buttons --------------------------------------------------------- */

enum {
    KIND_BUTTON = 0,
    KIND_THEME,
    KIND_ACCENT,
};

enum {
    ACT_NONE = 0,
    ACT_TASKMAN,
    ACT_SYSINFO,
    ACT_TERMINAL,
    ACT_BROWSER,
    ACT_THEME,
    ACT_ACCENT,
    ACT_DNS,
    ACT_RESTART,
    ACT_SHUTDOWN,
    ACT_CANCEL,
    ACT_FILES,
    ACT_EDITOR,
    ACT_SLIDES,
    ACT_UPDATE,
};

typedef struct {
    rect_t r;
    const char *label;
    int action;
    int arg;
    int kind;
} btn_t;

/* Fills out[] for the current page; the same list is used for drawing and for
 * hit testing. Returns the number of entries. */
static int page_buttons(int page, btn_t *out) {
    int n = 0;

    switch (page) {
    case PG_SYSTEM:
        out[n++] = (btn_t){ button_rect(360, 0), "Task Manager", ACT_TASKMAN, 0, KIND_BUTTON };
        out[n++] = (btn_t){ button_rect(360, 1), "System Info", ACT_SYSINFO, 0, KIND_BUTTON };
        out[n++] = (btn_t){ button_rect(360, 2), "Terminal", ACT_TERMINAL, 0, KIND_BUTTON };
        break;

    case PG_PERSONAL:
        for (int i = 0; i < 4; i++) {
            out[n++] = (btn_t){ theme_rect(i), NULL, ACT_THEME, i, KIND_THEME };
        }
        for (int i = 0; i < 5; i++) {
            out[n++] = (btn_t){ accent_rect(i), NULL, ACT_ACCENT, i, KIND_ACCENT };
        }
        out[n++] = (btn_t){ change_button_rect(), "Change", ACT_NONE, 0, KIND_BUTTON };
        break;

    case PG_APPS:
        out[n++] = (btn_t){ button_rect(244, 0), "Terminal", ACT_TERMINAL, 0, KIND_BUTTON };
        out[n++] = (btn_t){ button_rect(244, 1), "Browser", ACT_BROWSER, 0, KIND_BUTTON };
        out[n++] = (btn_t){ button_rect(244, 2), "Files", ACT_FILES, 0, KIND_BUTTON };
        out[n++] = (btn_t){ button_rect(284, 0), "Editor", ACT_EDITOR, 0, KIND_BUTTON };
        out[n++] = (btn_t){ button_rect(284, 1), "Slides", ACT_SLIDES, 0, KIND_BUTTON };
        out[n++] = (btn_t){ button_rect(284, 2), "System Info", ACT_SYSINFO, 0, KIND_BUTTON };
        break;

    case PG_UPDATE:
        out[n++] = (btn_t){ button_rect(236, 0), "Check now", ACT_UPDATE, 0, KIND_BUTTON };
        break;

    case PG_NETWORK:
        out[n++] = (btn_t){ button_rect(272, 0), "Test gateway", ACT_DNS, 0, KIND_BUTTON };
        out[n++] = (btn_t){ button_rect(272, 1), "Browser", ACT_BROWSER, 0, KIND_BUTTON };
        break;

    case PG_POWER:
        out[n++] = (btn_t){ big_button_rect(0),
                            g_power_confirm == 1 ? "Click again to confirm" : "Restart",
                            ACT_RESTART, 0, KIND_BUTTON };
        out[n++] = (btn_t){ big_button_rect(1),
                            g_power_confirm == 2 ? "Click again to confirm" : "Shut down",
                            ACT_SHUTDOWN, 0, KIND_BUTTON };
        out[n++] = (btn_t){ big_button_rect(2), "Cancel", ACT_CANCEL, 0, KIND_BUTTON };
        break;

    default:
        break;
    }
    return n;
}

static void draw_buttons(void) {
    btn_t list[MAX_BTNS];
    int n = page_buttons(g_page, list);

    for (int i = 0; i < n; i++) {
        const btn_t *b = &list[i];

        if (b->kind == KIND_THEME) {
            rect_t r = b->r;
            gui_fill(r.x, r.y, r.w, r.h, g_wall_swatch[b->arg]);
            gui_rect(r.x, r.y, r.w, r.h,
                     b->arg == g_theme ? 0xFFFFFF : C_RULE);
        } else if (b->kind == KIND_ACCENT) {
            rect_t r = b->r;
            gui_fill(r.x, r.y, r.w, r.h, g_accents[b->arg]);
            gui_rect(r.x, r.y, r.w, r.h,
                     b->arg == g_accent ? 0xFFFFFF : C_RULE);
        } else {
            rect_t r = b->r;
            int disabled = b->action == ACT_NONE;
            gui_fill(r.x, r.y, r.w, r.h, disabled ? g_sel : g_accent_rgb);
            gui_rect(r.x, r.y, r.w, r.h, disabled ? C_RULE : shade(g_accent_rgb));
            gui_text(r.x + 12, r.y + (r.h - 16) / 2,
                     disabled ? C_DIM : 0xFFFFFF, b->label);
        }
    }
}

/* ---- theme persistence ----------------------------------------------- */

#define THEME_PATH "/THEME.CFG"

/* The desktop owns the painting; this file is the handoff between the two
 * processes. Written on every click so the choice survives a reboot. */
static void save_theme(void) {
    char buf[48];
    int n;
    int fd = open(THEME_PATH, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (fd < 0) {
        snprintf(g_theme_status, sizeof(g_theme_status), "Cannot write THEME.CFG");
        return;
    }
    n = snprintf(buf, sizeof(buf), "accent=%d\nwall=%d\n", g_accent, g_theme);
    if (n > 0) write(fd, buf, (size_t)n);
    close(fd);
    snprintf(g_theme_status, sizeof(g_theme_status), "Saved: %s / %s",
             g_wall_names[g_theme], g_accent_names[g_accent]);
}

static void load_theme(void) {
    char buf[64];
    int fd = open(THEME_PATH, VFS_O_RDONLY);
    if (fd < 0) return;
    int n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    char *p = strstr(buf, "accent=");
    if (p) {
        int a = atoi(p + 7);
        if (a >= 0 && a < 5) {
            g_accent = a;
            g_accent_rgb = g_accents[a];
        }
    }
    p = strstr(buf, "wall=");
    if (p) {
        int w = atoi(p + 5);
        if (w >= 0 && w < 4) g_theme = w;
    }
}

/* ---- feature switches ------------------------------------------------ */

/* Every switch lives in the same OPTIONS block on its page, so one anchor
 * keeps drawing and hit testing in agreement. The set mirrors the categories
 * a Windows 11 user expects to find in Settings, trimmed to what this build
 * can actually act on. */
typedef struct {
    int page;
    const char *label;
    const char *desc;
    int def;
} feat_t;

enum {
    FEAT_BT = 0,
    FEAT_BT_PAIR,
    FEAT_WIFI,
    FEAT_METERED,
    FEAT_SIDELOAD,
    FEAT_SIGNIN,
    FEAT_TIME_AUTO,
    FEAT_24H,
    FEAT_GAMEMODE,
    FEAT_GAMEBAR,
    FEAT_CONTRAST,
    FEAT_LARGETEXT,
    FEAT_STICKY,
    FEAT_LOCATION,
    FEAT_DIAG,
    FEAT_CAMERA,
};

static const feat_t g_feats[] = {
    { PG_BLUETOOTH,     "Bluetooth",            "Discoverable as MCUBE-PC",   1 },
    { PG_BLUETOOTH,     "Pair with a phone",    "Share files and notifications", 0 },
    { PG_NETWORK,       "Wi-Fi",                "Connect to available networks", 1 },
    { PG_NETWORK,       "Metered connection",   "Limit background data",      0 },
    { PG_APPS,          "Install from anywhere","Allow self-built .lgy images", 1 },
    { PG_ACCOUNTS,      "Require sign-in",      "Ask for a password at boot", 0 },
    { PG_TIME,          "Set time automatically","Follow the kernel clock",   1 },
    { PG_TIME,          "24-hour clock",        "Show 13:45 instead of 1:45 PM", 1 },
    { PG_GAMING,        "Game mode",            "Boost the focused window",   1 },
    { PG_GAMING,        "Game bar",             "Control centre overlay",     0 },
    { PG_ACCESSIBILITY, "High contrast",        "Stronger borders and text",  0 },
    { PG_ACCESSIBILITY, "Large text",           "Double the interface text size", 0 },
    { PG_ACCESSIBILITY, "Sticky keys",          "Latch Alt and Win modifiers", 0 },
    { PG_PRIVACY,       "Location",             "Allow apps to request a position", 0 },
    { PG_PRIVACY,       "Diagnostics",          "Send crash reports to the shell", 1 },
    { PG_PRIVACY,       "Camera",               "Allow apps to use a camera", 0 },
};

#define FEAT_COUNT ((int)(sizeof(g_feats) / sizeof(g_feats[0])))

static int g_feat_on[FEAT_COUNT];

/* The high-contrast switch changes this window itself: flat black panels and
 * a brighter selection block. The desktop applies the same switch to its
 * chrome, both reading the same CONFIG.CFG. */
static void apply_palette(void) {
    if (g_feat_on[FEAT_CONTRAST]) {
        g_bg = 0x000000;
        g_panel = 0x0B0B0B;
        g_sel = 0x2C2C2C;
    } else {
        g_bg = 0x10151E;
        g_panel = 0x1A2230;
        g_sel = 0x243040;
    }
}

#define CONFIG_PATH "/CONFIG.CFG"

/* One "f<index>=<0|1>" line per switch keeps the file diffable and lets a new
 * switch be added without breaking older profiles. */
static void save_config(void) {
    char buf[FEAT_COUNT * 8 + 8];
    int n = 0;
    int fd = open(CONFIG_PATH, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (fd < 0) return;

    for (int i = 0; i < FEAT_COUNT; i++) {
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "f%d=%d\n",
                      i, g_feat_on[i] ? 1 : 0);
    }
    write(fd, buf, (size_t)n);
    close(fd);
}

static void load_config(void) {
    char buf[256];
    int fd, n;

    for (int i = 0; i < FEAT_COUNT; i++) g_feat_on[i] = g_feats[i].def;

    fd = open(CONFIG_PATH, VFS_O_RDONLY);
    if (fd < 0) return;
    n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;

    for (int i = 0; i < FEAT_COUNT; i++) {
        char key[12];
        const char *p;
        snprintf(key, sizeof(key), "f%d=", i);
        p = strstr(buf, key);
        if (p) g_feat_on[i] = atoi(p + strlen(key)) ? 1 : 0;
    }

    apply_palette();
}

static rect_t feat_row_rect(int row) {
    return rect_make(CONTENT_X, FEAT_TOP + row * FEAT_ROW,
                     gui_width() - CONTENT_X - 24, 40);
}

static rect_t feat_switch_rect(rect_t r) {
    return rect_make(r.x + r.w - 44 - 12, r.y + 9, 44, 22);
}

static void draw_switch(rect_t r, int on) {
    u32 track = on ? g_accent_rgb : 0x334155;
    int kx = on ? r.x + r.w - 18 : r.x + 4;

    gui_fill(r.x, r.y, r.w, r.h, track);
    gui_rect(r.x, r.y, r.w, r.h, shade(track));
    gui_fill(kx, r.y + 4, 14, r.h - 8, on ? 0xFFFFFF : 0xB4C1D2);
}

static void draw_feats(int page) {
    int row = 0;
    int titled = 0;

    for (int i = 0; i < FEAT_COUNT; i++) {
        rect_t r;
        if (g_feats[i].page != page) continue;

        if (!titled) {
            section(FEAT_TOP - 26, "OPTIONS");
            titled = 1;
        }
        r = feat_row_rect(row);
        gui_fill(r.x, r.y, r.w, r.h, g_sel);
        gui_rect(r.x, r.y, r.w, r.h, C_RULE);
        gui_text(r.x + 12, r.y + 5, C_BODY, g_feats[i].label);
        text_clip(r.x + 12, r.y + 22, C_DIM, g_feats[i].desc, (r.w - 80) / 8);
        draw_switch(feat_switch_rect(r), g_feat_on[i]);
        row++;
    }
}

static int feat_hit(int page, int x, int y) {
    int row = 0;

    for (int i = 0; i < FEAT_COUNT; i++) {
        if (g_feats[i].page != page) continue;
        if (rect_has(feat_row_rect(row), x, y)) return i;
        row++;
    }
    return -1;
}

/* ---- page bodies ----------------------------------------------------- */

static void draw_system(void) {
    char line[160];
    int y = CONTENT_TOP;

    if (g_have_hw) {
        u32 cores = g_hw.cpu.cores ? g_hw.cpu.cores : g_hw.cpu.logical_cpus;

        kv(y, "Processor", g_hw.cpu.brand[0] ? g_hw.cpu.brand : "unknown");
        y += ROW_H;
        kv(y, "Vendor", g_hw.cpu.vendor[0] ? g_hw.cpu.vendor : "unknown");
        y += ROW_H;
        snprintf(line, sizeof(line), "%u cores / %u threads",
                 cores, g_hw.cpu.logical_cpus);
        kv(y, "Cores", line);
        y += ROW_H;
        gib_text(g_hw.total_ram_bytes, line, sizeof(line));
        kv(y, "Memory", line);
        y += ROW_H;
        gib_text(g_hw.usable_ram_bytes, line, sizeof(line));
        kv(y, "Usable memory", line);
        y += ROW_H;
    } else {
        kv(y, "Hardware", "inventory unavailable");
        y += ROW_H;
    }

    snprintf(line, sizeof(line), "%d", g_procs);
    kv(y, "Processes", line);
    y += ROW_H;

    uptime_text(line, sizeof(line));
    kv(y, "Uptime", line);
    y += ROW_H;

    kv(y, "Kernel", "0.1.0-gensis");
    y += ROW_H;

    /* USB health belongs on the System page: on this hardware it is the
     * difference between a usable desktop and a dead one. */
    if (g_have_usb) {
        snprintf(line, sizeof(line), "%u UHCI / %u EHCI / %u xHCI",
                 (u32)g_usb.ctrl_uhci, (u32)g_usb.ctrl_ehci,
                 (u32)g_usb.ctrl_xhci);
        kv(y, "USB controllers", line);
        y += ROW_H;
        snprintf(line, sizeof(line), "%u HID on %u ports (%u connected)",
                 (u32)g_usb.devs, (u32)g_usb.ports, (u32)g_usb.ccs);
        kv(y, "USB input", line);
        y += ROW_H;
    } else {
        kv(y, "USB input", "diagnostics unavailable");
        y += ROW_H;
    }

    section(336, "QUICK ACTIONS");
}

static void draw_display(void) {
    char line[96];
    int y = CONTENT_TOP;

    if (g_have_fb) {
        snprintf(line, sizeof(line), "%u x %u px",
                 (u32)g_fb.width, (u32)g_fb.height);
        kv(y, "Resolution", line);
        y += ROW_H;
        snprintf(line, sizeof(line), "%u bytes per line", (u32)g_fb.pitch);
        kv(y, "Pitch", line);
        y += ROW_H;
        snprintf(line, sizeof(line), "%s, %u bpp",
                 format_name(g_fb.format), (u32)g_fb.bpp);
        kv(y, "Pixel format", line);
        y += ROW_H;
    } else {
        kv(y, "Framebuffer", "unavailable");
        y += ROW_H;
    }

    snprintf(line, sizeof(line), "%d x %d px", gui_width(), gui_height());
    kv(y, "Window", line);
    y += ROW_H;

    kv(y, "Desktop layout", "taskbar 48 px, centred app icons");
    y += ROW_H;

    snprintf(line, sizeof(line), "t = %u s", uptime_seconds());
    kv(y, "Uptime tick", line);
    y += ROW_H;

    gui_text(CONTENT_X, y + 16, C_DIM, "One tick per second, delivered by the desktop.");
}

static void draw_personal(void) {
    section(96, "WALLPAPER");

    for (int i = 0; i < 4; i++) {
        gui_text(theme_rect(i).x + 2, 160,
                 i == g_theme ? C_BODY : C_DIM, g_wall_names[i]);
    }

    section(184, "ACCENT COLOUR");

    for (int i = 0; i < 5; i++) {
        gui_text(accent_rect(i).x, 240,
                 i == g_accent ? C_BODY : C_DIM, g_accent_names[i]);
    }

    section(264, "STATUS");
    gui_text(CONTENT_X, 288, C_BODY, g_theme_status);
    gui_text(CONTENT_X, 312, C_DIM, "Saved to /THEME.CFG and picked up by the");
    gui_text(CONTENT_X, 328, C_DIM, "desktop within one tick, no restart needed.");
}

static void draw_network(void) {
    char line[96];
    int y = CONTENT_TOP;

    kv(y, "Link", g_have_net ? (g_net.link_up ? "up" : "no adapter")
                             : "unavailable");
    y += ROW_H;

    if (g_have_net && g_net.ip) {
        ip_text(g_net.ip, line, sizeof(line));
    } else {
        snprintf(line, sizeof(line), "not configured");
    }
    kv(y, "IPv4", line);
    y += ROW_H;

    if (g_have_net && g_net.gateway) {
        ip_text(g_net.gateway, line, sizeof(line));
    } else {
        snprintf(line, sizeof(line), "not configured");
    }
    kv(y, "Gateway", line);
    y += ROW_H;

    if (g_have_net && g_net.netmask) {
        ip_text(g_net.netmask, line, sizeof(line));
    } else {
        snprintf(line, sizeof(line), "not configured");
    }
    kv(y, "Netmask", line);
    y += ROW_H;

    if (g_have_net) {
        mac_text(g_net.mac, line, sizeof(line));
    } else {
        snprintf(line, sizeof(line), "unknown");
    }
    kv(y, "MAC", line);
    y += ROW_H;

    kv(y, "DNS", "kernel resolver via SYS_DNS");

    if (g_dns_state == 1) {
        gui_text(CONTENT_X, 248, C_OK, g_dns_status);
    } else if (g_dns_state == 2) {
        gui_text(CONTENT_X, 248, C_WARN, g_dns_status);
    } else {
        gui_text(CONTENT_X, 248, C_DIM, g_dns_status);
    }

    gui_text(CONTENT_X, 272, C_DIM, "The lookup runs in the kernel and may take up to 3 s.");
}

/* The rail keeps the Windows 11 name, but the page is honest about what this
 * build has: USB HID, no Bluetooth HCI. Every value below comes from
 * SYS_USBINFO, i.e. from the driver as it is running right now. */
static void draw_bluetooth(void) {
    char line[160];
    int y = CONTENT_TOP;

    if (g_have_usb) {
        snprintf(line, sizeof(line), "%u UHCI / %u EHCI / %u xHCI",
                 (u32)g_usb.ctrl_uhci, (u32)g_usb.ctrl_ehci,
                 (u32)g_usb.ctrl_xhci);
        kv(y, "Controllers", line); y += ROW_H;

        snprintf(line, sizeof(line), "%u total, %u connected",
                 (u32)g_usb.ports, (u32)g_usb.ccs);
        kv(y, "Root ports", line); y += ROW_H;

        snprintf(line, sizeof(line), "%u bound: %u keyboard, %u pointer, %u touch",
                 (u32)g_usb.devs, (u32)g_usb.kbd, (u32)g_usb.ptr,
                 (u32)g_usb.touch);
        kv(y, "HID devices", line); y += ROW_H;

        snprintf(line, sizeof(line), "%u completed", (u32)g_usb.rescans);
        kv(y, "Hot-plug passes", line); y += ROW_H;

        kv(y, "Kernel note", g_usb.text[0] ? g_usb.text : "(no notes)");
        y += ROW_H;
    } else {
        kv(y, "USB bridge", "diagnostics unavailable"); y += ROW_H;
    }

    kv(y, "Bluetooth radio", "no HCI controller in this build"); y += ROW_H;

    gui_text(CONTENT_X, y + 8, C_DIM, "Keyboards, mice and touch panels arrive over");
    gui_text(CONTENT_X, y + 24, C_DIM, "USB HID. The USB stack re-scans every 750 ms,");
    gui_text(CONTENT_X, y + 40, C_DIM, "so a device plugged in later is picked up.");
}

static void draw_apps(void) {
    char line[96];
    int y = CONTENT_TOP;

    snprintf(line, sizeof(line), "%d programs in /bin", g_bin_count);
    kv(y, "Installed", line); y += ROW_H;

    size_text(g_bin_bytes, line, sizeof(line));
    kv(y, "Footprint", line); y += ROW_H;

    kv(y, "Default shell", "desktop.lgy"); y += ROW_H;
    kv(y, "Image format", ".lgy, flat, syscall ABI"); y += ROW_H;

    text_clip(CONTENT_X + KEY_W, y, C_BODY, g_bin_names,
              (gui_width() - (CONTENT_X + KEY_W) - 24) / 8);
    gui_text(CONTENT_X, y, C_DIM, "Built-in apps");

    section(220, "LAUNCH");
}

static void draw_accounts(void) {
    int y = CONTENT_TOP;

    kv(y, "User", "root"); y += ROW_H;
    kv(y, "Home", "/"); y += ROW_H;
    kv(y, "Session", "single seat, no login manager"); y += ROW_H;
    kv(y, "Password", "unset, no PAM in this build"); y += ROW_H;

    gui_text(CONTENT_X, y + 8, C_DIM, "The first user owns every device node the");
    gui_text(CONTENT_X, y + 24, C_DIM, "kernel publishes at boot.");
}

/* Days since 1970-01-01 to a calendar date (Howard Hinnant's civil_from_days).
 * Integer only, so it needs no floating point in user space. */
static void civil_from_epoch(u64 days, int *y, int *m, int *d) {
    int z = (int)days + 719468;
    int era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int yy = (int)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned dd = doy - (153 * mp + 2) / 5 + 1;
    unsigned mm = mp < 10 ? mp + 3 : mp - 9;

    *y = yy + (mm <= 2);
    *m = (int)mm;
    *d = (int)dd;
}

static void draw_time(void) {
    char line[96];
    int y = CONTENT_TOP;

    if (g_have_time && g_time.epoch) {
        u64 secs = g_time.epoch;
        u32 sod = (u32)(secs % 86400ULL);
        int yy, mm, dd;

        civil_from_epoch(secs / 86400ULL, &yy, &mm, &dd);
        snprintf(line, sizeof(line), "%04d-%02d-%02d (UTC)", yy, mm, dd);
        kv(y, "Date", line); y += ROW_H;

        if (g_feat_on[FEAT_24H]) {
            snprintf(line, sizeof(line), "%02u:%02u:%02u",
                     sod / 3600u, (sod / 60u) % 60u, sod % 60u);
        } else {
            u32 h12 = (sod / 3600u) % 12u;
            if (!h12) h12 = 12;
            snprintf(line, sizeof(line), "%u:%02u:%02u %s", h12,
                     (sod / 60u) % 60u, sod % 60u,
                     (sod / 3600u) < 12u ? "AM" : "PM");
        }
        kv(y, "Time", line); y += ROW_H;

        snprintf(line, sizeof(line), "%lu s since the Unix epoch",
                 (unsigned long)secs);
        kv(y, "Epoch", line); y += ROW_H;
    } else {
        kv(y, "Date", "not available"); y += ROW_H;
        kv(y, "Time", "not available"); y += ROW_H;
        kv(y, "Epoch", "not available"); y += ROW_H;
    }

    uptime_text(line, sizeof(line));
    kv(y, "Uptime", line); y += ROW_H;

    kv(y, "Region", "en-US, metric units");
}

static void draw_gaming(void) {
    char line[64];
    int y = CONTENT_TOP;

    snprintf(line, sizeof(line), "%d", g_procs);
    kv(y, "Running tasks", line); y += ROW_H;
    kv(y, "Compositor", "software, 60 Hz tick"); y += ROW_H;
    kv(y, "Frame pacing", "one full flush per tick"); y += ROW_H;

    gui_text(CONTENT_X, y + 8, C_DIM, "Game mode raises the tick budget for the");
    gui_text(CONTENT_X, y + 24, C_DIM, "focused window and quiets the tray.");
}

static void draw_accessibility(void) {
    int y = CONTENT_TOP;

    kv(y, "Text scale", g_feat_on[FEAT_LARGETEXT] ? "200%" : "100%"); y += ROW_H;
    kv(y, "Contrast", g_feat_on[FEAT_CONTRAST] ? "high" : "standard"); y += ROW_H;
    kv(y, "Modifiers", g_feat_on[FEAT_STICKY] ? "latched" : "held"); y += ROW_H;
    kv(y, "Cursor", "software pointer, 12 x 18"); y += ROW_H;

    gui_text(CONTENT_X, y + 8, C_DIM, "These switches only change the shell chrome;");
    gui_text(CONTENT_X, y + 24, C_DIM, "applications keep their own layout.");
}

static void draw_privacy(void) {
    int y = CONTENT_TOP;

    kv(y, "Telemetry", "off, local only"); y += ROW_H;
    kv(y, "Diagnostics sink", "serial log on COM1"); y += ROW_H;
    kv(y, "State files", "/CONFIG.CFG, /THEME.CFG"); y += ROW_H;
    kv(y, "Isolation", "one address space per app"); y += ROW_H;
}

static void draw_update(void) {
    int y = CONTENT_TOP;

    kv(y, "Channel", "Beta"); y += ROW_H;
    kv(y, "Build", "0.1.0-gensis"); y += ROW_H;
    kv(y, "Last check", g_update_checked[0] ? g_update_checked : "never"); y += ROW_H;
    kv(y, "Source", "local image, no network updater"); y += ROW_H;

    gui_text(CONTENT_X, y + 8, C_BODY, g_update_status);
    gui_text(CONTENT_X, 280, C_DIM, "Updates are applied by replacing /bin images.");
}

static void draw_power(void) {
    char line[96];
    int bx = CONTENT_X + BIG_W + 40;

    gui_text(CONTENT_X, 96, C_WARN, "Unsaved work will be lost.");
    kv(124, "Power mode", "Balanced");

    /* The action buttons occupy the left column, so the live power supply
     * telemetry goes into its own block beside them. */
    gui_text(bx, 160, g_accent_rgb, "POWER SUPPLY");
    gui_fill(bx, 176, gui_width() - bx - 24, 1, C_RULE);

    if (g_have_bat) {
        if (g_bat.present) {
            if (g_bat.has_percent)
                snprintf(line, sizeof(line), "Battery: %u%%", (u32)g_bat.percent);
            else
                snprintf(line, sizeof(line), "Battery: present (no gauge)");
        } else {
            snprintf(line, sizeof(line), "Battery: not installed");
        }
        gui_text(bx, 188, C_BODY, line);

        snprintf(line, sizeof(line), "Source: %s",
                 g_bat.ac_online ? "mains power" : "battery");
        gui_text(bx, 208, C_BODY, line);

        snprintf(line, sizeof(line), "Charging: %s",
                 g_bat.charging ? "yes" : "no");
        gui_text(bx, 228, C_BODY, line);

        if (g_bat.minutes_left) {
            snprintf(line, sizeof(line), "Runtime left: %u h %02u min",
                     (u32)(g_bat.minutes_left / 60u),
                     (u32)(g_bat.minutes_left % 60u));
        } else {
            snprintf(line, sizeof(line), "Runtime left: unknown");
        }
        gui_text(bx, 248, C_BODY, line);
    } else {
        gui_text(bx, 188, C_DIM, "Battery: telemetry unavailable");
    }

    gui_text(CONTENT_X, 316, C_BODY, g_power_status);
    gui_text(CONTENT_X, 340, C_DIM, "The power action is issued once confirmed.");
}

static void draw_about(void) {
    int y = CONTENT_TOP;

    kv(y, "OS name", "MCubeAIOS 0.1.0-gensis");
    y += ROW_H;
    kv(y, "Architecture", "x86_64");
    y += ROW_H;
    kv(y, "Boot mode", "UEFI only (no BIOS/CSM)");
    y += ROW_H;
    kv(y, "AI layer", "aiosd: AI-native syscalls 0x400-0x417");
    y += ROW_H;
    kv(y, "Model file", "/MODELS/TINY.GGU");
    y += ROW_H;

    gui_fill(CONTENT_X, 232, gui_width() - CONTENT_X - 24, 1, C_RULE);
    gui_text(CONTENT_X, 248, C_BODY,
             "Licence: MIT. Copyright 2026 MCubeAIOS contributors.");
    gui_text(CONTENT_X, 272, C_DIM,
             "Settings is part of the MCubeAIOS user space.");
}

/* ---- new practical pages --------------------------------------------- */

static void draw_storage(void) {
    char line[128];
    char human[32];
    int y = CONTENT_TOP;

    if (g_have_hw) {
        gib_text(g_hw.total_ram_bytes, line, sizeof(line));
        kv(y, "RAM installed", line); y += ROW_H;
        gib_text(g_hw.usable_ram_bytes, line, sizeof(line));
        kv(y, "RAM usable", line); y += ROW_H;
        snprintf(line, sizeof(line), "%u populated slot(s)",
                 (u32)g_hw.mem_module_count);
        kv(y, "Memory modules", line); y += ROW_H;
    } else {
        kv(y, "Memory", "inventory unavailable"); y += ROW_H;
    }

    {
        mcube_stat_t st;
        st.size = 0;
        st.type = 0;
        st.mode = 0;
        stat("/kernel.elf", &st);
        size_text(st.size, human, sizeof(human));
        kv(y, "Boot kernel", human); y += ROW_H;
    }

    size_text(g_bin_bytes, human, sizeof(human));
    snprintf(line, sizeof(line), "%d image(s), %s", g_bin_count, human);
    kv(y, "Programs in /bin", line); y += ROW_H;

    kv(y, "State files", "THEME.CFG, CONFIG.CFG, BOOKMARKS.TXT");
    y += ROW_H;

    kv(y, "Disk usage", "no filesystem counters exposed");

    gui_text(CONTENT_X, y + 24, C_DIM,
             "Sizes are read from the VFS every five ticks.");
}

static void draw_shortcuts(void) {
    int y = 118;

    section(96, "DESKTOP");
    gui_text(CONTENT_X, y, C_BODY, "Alt+Tab     switch between windows"); y += 18;
    gui_text(CONTENT_X, y, C_BODY, "Alt+F4      close the focused window"); y += 18;
    gui_text(CONTENT_X, y, C_BODY, "Win         open or close the Start menu"); y += 18;
    gui_text(CONTENT_X, y, C_BODY, "Esc         close the focused window"); y += 18;
    gui_text(CONTENT_X, y, C_DIM, "Drag the title bar to move, the edges to resize."); y += 18;

    y += 10;
    section(y, "BROWSER"); y += 22;
    gui_text(CONTENT_X, y, C_BODY, "Enter       load the address / follow a number"); y += 18;
    gui_text(CONTENT_X, y, C_BODY, "Ctrl+T      new tab          Ctrl+W  close tab"); y += 18;
    gui_text(CONTENT_X, y, C_BODY, "Ctrl+B      bookmark page   Ctrl+M  lists panel"); y += 18;
    gui_text(CONTENT_X, y, C_BODY, "Up/Down     scroll          PgUp/PgDn  page"); y += 18;
    gui_text(CONTENT_X, y, C_BODY, "Home        top of page     F5      reload"); y += 18;
    gui_text(CONTENT_X, y, C_DIM, "Click a [number] marker to follow that link."); y += 18;

    y += 10;
    section(y, "SETTINGS"); y += 22;
    gui_text(CONTENT_X, y, C_BODY, "Up/Down     previous / next page"); y += 18;
    gui_text(CONTENT_X, y, C_BODY, "1-9, 0      jump to a page      F5  refresh"); y += 18;
}

static void draw_state(void) {
    int y = 110;
    int xr = CONTENT_X + 330;

    section(96, "ROOT DIRECTORY");

    if (g_root_n == 0) {
        gui_text(CONTENT_X, y, C_DIM, "(empty or unreadable)");
    }

    for (int i = 0; i < g_root_n; i++) {
        int isdir = g_root_type[i] == 2;   /* VFS_TYPE_DIR */
        gui_text(CONTENT_X, y, C_DIM, isdir ? "DIR " : "FILE");
        text_clip(CONTENT_X + 48, y, C_BODY, g_root_name[i], 26);
        if (!isdir) {
            char human[32];
            int n;
            size_text(g_root_size[i], human, sizeof(human));
            n = (int)strlen(human);
            gui_text(xr - n * 8, y, C_DIM, human);
        }
        y += 18;
    }

    gui_text(CONTENT_X, y + 12, C_DIM,
             "State files are plain text: THEME.CFG holds the wallpaper");
    gui_text(CONTENT_X, y + 28, C_DIM,
             "and accent, CONFIG.CFG the options, BOOKMARKS.TXT the browser.");
}

/* ---- frame ----------------------------------------------------------- */

static void draw(void) {
    int w = gui_width();
    int h = gui_height();

    gui_clear(g_bg);

    /* Navigation rail. */
    gui_fill(0, TITLE_H, NAV_W, h - TITLE_H, g_panel);

    /* Title strip. */
    gui_fill(0, 0, w, TITLE_H, g_panel);
    gui_text(12, 5, g_accent_rgb, "Settings");
    {
        const char *name = g_page_names[g_page];
        int pw = (int)strlen(name) * 8;
        if (w - pw - 12 > CONTENT_X) {
            gui_text(w - pw - 12, 5, C_DIM, name);
        }
    }
    gui_fill(0, TITLE_H, w, 1, C_RULE);

    /* Nav rows: accent bar plus a filled block for the selected page. */
    gui_text(20, 32, C_DIM, "PAGES");
    for (int i = 0; i < PAGE_COUNT; i++) {
        rect_t r = nav_row_rect(i);
        int sel = i == g_page;

        if (sel) {
            gui_fill(r.x + 4, r.y, r.w - 12, r.h, g_sel);
            gui_fill(r.x, r.y, 3, r.h, g_accent_rgb);
        }
        gui_text(r.x + 20, r.y + (r.h - 16) / 2, sel ? 0xFFFFFF : C_DIM, g_page_names[i]);
    }
    gui_text(12, h - 24, C_DIM, "MCubeAIOS 0.1.0");

    /* Content header. */
    gui_text(CONTENT_X, 40, g_accent_rgb, g_page_names[g_page]);
    gui_fill(CONTENT_X, 62, 32, 3, g_accent_rgb);
    text_clip(CONTENT_X, 72, C_DIM, g_page_sub[g_page],
              (w - CONTENT_X - 24) / 8);

    switch (g_page) {
    case PG_SYSTEM:   draw_system();   break;
    case PG_BLUETOOTH: draw_bluetooth(); break;
    case PG_NETWORK:  draw_network();  break;
    case PG_DISPLAY:  draw_display();  break;
    case PG_PERSONAL: draw_personal(); break;
    case PG_APPS:     draw_apps();     break;
    case PG_ACCOUNTS: draw_accounts(); break;
    case PG_TIME:     draw_time();     break;
    case PG_GAMING:   draw_gaming();   break;
    case PG_ACCESSIBILITY: draw_accessibility(); break;
    case PG_PRIVACY:  draw_privacy();  break;
    case PG_UPDATE:   draw_update();   break;
    case PG_POWER:    draw_power();    break;
    case PG_ABOUT:    draw_about();    break;
    case PG_STORAGE:  draw_storage();  break;
    case PG_SHORTCUTS: draw_shortcuts(); break;
    case PG_STATE:    draw_state();    break;
    default: break;
    }
    draw_feats(g_page);
    draw_buttons();

    /* Footer. */
    gui_fill(CONTENT_X, h - 30, w - CONTENT_X - 24, 1, C_RULE);
    text_clip(CONTENT_X, h - 24, C_DIM,
              "Up/Down page   1-9 0 jump   F5 redraw   Esc close",
              (w - CONTENT_X - 24) / 8);

    gui_flush();
    g_need_redraw = 0;
}

/* ---- state changes --------------------------------------------------- */

static void refresh(void) {
    int rc;
    mcube_task_info_t tasks[64];

    g_have_hw = hwinfo(&g_hw) == 0;
    g_have_fb = fb_info(&g_fb) == 0;
    g_have_time = mc_time(&g_time) == 0;

    rc = net_info(&g_net);
    g_have_net = rc == 0;

    rc = taskinfo(tasks, (int)(sizeof(tasks) / sizeof(tasks[0])));
    g_procs = rc > 0 ? rc : 0;

    g_have_usb = usb_info(&g_usb) == 0;
    g_have_bat = battery_info(&g_bat) == 0;

    /* Directory walks open the path once per entry, so they run at a fifth
     * of the tick rate instead of on every repaint. */
    if (g_scan_age == 0) {
        scan_bin();
        scan_root();
    }
    g_scan_age = (g_scan_age + 1) % 5;

    g_need_redraw = 1;
}

static void set_page(int page) {
    if (page < 0 || page >= PAGE_COUNT) return;
    if (page != g_page && g_power_confirm) {
        g_power_confirm = 0;
        snprintf(g_power_status, sizeof(g_power_status), "Select an action.");
    }
    g_page = page;
    g_need_redraw = 1;
}

static void run_dns(void) {
    u32 ip = 0;
    int rc;

    g_dns_state = 3;
    snprintf(g_dns_status, sizeof(g_dns_status),
             "Resolver: resolving example.com ...");
    draw();                      /* show the pending line before blocking */

    rc = mc_dns_lookup("example.com", &ip);
    if (rc == 0) {
        char addr[32];
        ip_text(ip, addr, sizeof(addr));
        snprintf(g_dns_status, sizeof(g_dns_status),
                 "Resolver: example.com -> %s", addr);
        g_dns_state = 1;
    } else {
        snprintf(g_dns_status, sizeof(g_dns_status),
                 "Resolver: lookup failed (rc %d)", rc);
        g_dns_state = 2;
    }
    draw();
}

static void do_action(int action, int arg) {
    switch (action) {
    case ACT_TASKMAN:
        gui_launch("/bin/taskman.lgy");
        break;
    case ACT_SYSINFO:
        gui_launch("/bin/sysinfo.lgy");
        break;
    case ACT_TERMINAL:
        gui_launch("/bin/terminal.lgy");
        break;
    case ACT_BROWSER:
        gui_launch("/bin/browser.lgy");
        break;
    case ACT_FILES:
        gui_launch("/bin/files.lgy");
        break;
    case ACT_EDITOR:
        gui_launch("/bin/editor.lgy");
        break;
    case ACT_SLIDES:
        gui_launch("/bin/slides.lgy");
        break;

    case ACT_UPDATE: {
        char up[64];
        snprintf(g_update_status, sizeof(g_update_status),
                 "Checking for updates ...");
        g_need_redraw = 1;
        draw();
        uptime_text(up, sizeof(up));
        snprintf(g_update_checked, sizeof(g_update_checked), "uptime %s", up);
        snprintf(g_update_status, sizeof(g_update_status),
                 "Up to date. Build 0.1.0-gensis is the latest.");
        g_need_redraw = 1;
        break;
    }

    case ACT_THEME:
        if (arg >= 0 && arg < 4) {
            g_theme = arg;
            save_theme();
            g_need_redraw = 1;
        }
        break;

    case ACT_ACCENT:
        if (arg >= 0 && arg < 5) {
            g_accent = arg;
            g_accent_rgb = g_accents[arg];
            save_theme();
            g_need_redraw = 1;
        }
        break;

    case ACT_DNS:
        run_dns();
        break;

    case ACT_RESTART:
        if (g_power_confirm == 1) {
            g_power_confirm = 0;
            snprintf(g_power_status, sizeof(g_power_status), "Rebooting now.");
            g_need_redraw = 1;
            draw();
            mc_power(MCUBE_POWER_REBOOT);
        } else {
            g_power_confirm = 1;
            snprintf(g_power_status, sizeof(g_power_status),
                     "Restart armed: click again to confirm.");
            g_need_redraw = 1;
        }
        break;

    case ACT_SHUTDOWN:
        if (g_power_confirm == 2) {
            g_power_confirm = 0;
            snprintf(g_power_status, sizeof(g_power_status),
                     "Shutting down now.");
            g_need_redraw = 1;
            draw();
            mc_power(MCUBE_POWER_OFF);
        } else {
            g_power_confirm = 2;
            snprintf(g_power_status, sizeof(g_power_status),
                     "Shut down armed: click again to confirm.");
            g_need_redraw = 1;
        }
        break;

    case ACT_CANCEL:
        if (g_power_confirm) {
            g_power_confirm = 0;
            snprintf(g_power_status, sizeof(g_power_status),
                     "Power action cancelled.");
        } else {
            snprintf(g_power_status, sizeof(g_power_status),
                     "No power action pending.");
        }
        g_need_redraw = 1;
        break;

    default:
        break;
    }
}

/* ---- events ---------------------------------------------------------- */

static void handle_event(const gui_event_t *ev) {
    switch (ev->type) {
    case GUI_EVT_KEY:
        if (ev->code == INPUT_KEY_UP) {
            set_page((g_page + PAGE_COUNT - 1) % PAGE_COUNT);
        } else if (ev->code == INPUT_KEY_DOWN) {
            set_page((g_page + 1) % PAGE_COUNT);
        } else if (ev->code >= '1' && ev->code <= '9') {
            set_page((int)(ev->code - '1'));
        } else if (ev->code == '0') {
            set_page(9);
        } else if (ev->code == INPUT_KEY_F5) {
            refresh();
        } else if (ev->code == 27) {
            exit(0);
        }
        break;

    case GUI_EVT_MOUSE: {
        btn_t list[MAX_BTNS];
        int n;

        if (!(ev->buttons & 1)) break;

        for (int i = 0; i < PAGE_COUNT; i++) {
            if (rect_has(nav_row_rect(i), ev->x, ev->y)) {
                set_page(i);
                return;
            }
        }

        {
            int fi = feat_hit(g_page, ev->x, ev->y);
            if (fi >= 0) {
                g_feat_on[fi] = !g_feat_on[fi];
                save_config();
                apply_palette();
                g_need_redraw = 1;
                return;
            }
        }

        n = page_buttons(g_page, list);
        for (int i = 0; i < n; i++) {
            if (!rect_has(list[i].r, ev->x, ev->y)) continue;
            if (list[i].action != ACT_NONE) {
                do_action(list[i].action, list[i].arg);
            }
            break;
        }
        break;
    }

    case GUI_EVT_TICK:
        refresh();
        break;

    case GUI_EVT_RESIZE:
        g_need_redraw = 1;
        break;

    case GUI_EVT_CLOSE:
        exit(0);

    default:
        break;
    }
}

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;

    if (gui_init("Settings", 820, 600) != 0) {
        printf("USER_SETTINGS_FAIL: gui_init\n");
        return 1;
    }
    printf("USER_SETTINGS_OK: %d pages\n", PAGE_COUNT);

    load_theme();
    load_config();
    refresh();
    draw();

    for (;;) {
        gui_event_t ev;
        int rc = gui_wait(&ev, 500);
        if (rc < 0) exit(0);
        if (rc > 0) handle_event(&ev);
        if (g_need_redraw) draw();
    }
}
