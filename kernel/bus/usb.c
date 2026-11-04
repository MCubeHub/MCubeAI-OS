/* USB core: controller registration, device enumeration and HID decoding.
 *
 * Three controller front ends (UHCI, EHCI, xHCI) register a usb_hcd_ops_t and
 * this file drives the portable part: reset, GET_DESCRIPTOR, SET_ADDRESS,
 * SET_CONFIGURATION, the HID report descriptor parser and the runtime report
 * decoder. Absolute pointers and single-touch digitizers are translated into
 * screen pixels and pushed through input_push_abs(), so user space keeps
 * consuming the same INPUT_MOUSE events as with the PS/2 mouse.
 *
 * Everything except usb_poll() runs in the boot context before sched_start(),
 * where timer_sleep_ms() is available for the control-transfer waits.
 */

#include <mcube/usb.h>
#include <mcube/kernel.h>
#include <mcube/input.h>
#include <mcube/tty.h>
#include <mcube/mm.h>
#include <mcube/timer.h>
#include <mcube/syscall.h>
#include <mcube/io.h>

#define USB_CONTROL_TIMEOUT_MS 500
#define USB_TOUCH_LOG_DELTA    8
/* Wall-clock budget for the boot-time enumeration pass. Real firmware can
 * leave ports flagged "connected" that never answer, and a single 20-port
 * xHCI then spends tens of seconds inside port resets. */
#define USB_ENUM_BUDGET_MS     6000

static usb_controller_t g_ctrls[USB_MAX_CONTROLLERS];
static int g_ctrl_count;

static usb_device_t g_devices[USB_MAX_DEVICES];
static int g_dev_count;

static u8 *g_desc;              /* descriptor scratch page (DMA) */
static u8 *g_rbuf;              /* interrupt report buffer (DMA) */
static u8 *g_erbuf;             /* enumeration scratch, never touched by poll */
static i32 g_screen_w = 1024;
static i32 g_screen_h = 768;

/* Per-controller-type probe results and the hot-plug pass counter; they feed
 * SYS_USBINFO so a machine with a dead input stack can be read off the screen
 * instead of a serial console. */
static u32 g_n_uhci, g_n_ehci, g_n_xhci;
static u32 g_rescans;
static int g_poll_armed;
static char g_note[sizeof(((mcube_usb_info_t *)0)->text)];

/* Set while usb_enum_task() owns the controllers: the poll timer must not run
 * a transfer on a device that is being enumerated right now. */
static volatile int g_enum_busy;
static u8 g_ctrl_started[USB_MAX_CONTROLLERS];

/* Record the last thing the controller layer did. The buffer is shown verbatim
 * in the shell's status card, so it must stay short and self-contained. */
void usb_diag_note(const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    kvsnprintf(g_note, sizeof(g_note), fmt, ap);
    __builtin_va_end(ap);
}

/* ---- controller registration ---------------------------------------- */

int usb_register_controller(const usb_hcd_ops_t *ops, void *priv,
                            u8 bus, u8 slot, u8 func) {
    if (!ops || g_ctrl_count >= USB_MAX_CONTROLLERS) return -1;
    usb_controller_t *c = &g_ctrls[g_ctrl_count++];
    c->ops = ops;
    c->priv = priv;
    c->bus = bus;
    c->slot = slot;
    c->func = func;
    return 0;
}

u64 usb_dma_alloc(u32 pages) {
    if (pages == 0) pages = 1;
    u64 phys = pmm_alloc_pages(pages);
    if (!phys) return 0;
    memset(phys_to_virt(phys), 0, (usize)pages * PAGE_SIZE);
    return phys;
}

/* Identity window the loader built at MCUBE_DIRECT_MAP_BASE. */
#define USB_IDENTITY_WINDOW (64ULL << 30)

void *usb_map_mmio(u64 phys, u64 size) {
    if (!phys) return NULL;
    if (size == 0) size = 0x10000;

    u64 start = phys & ~(PAGE_SIZE - 1);
    u64 end = ALIGN_UP(phys + size, PAGE_SIZE);
    if (start < USB_IDENTITY_WINDOW) start = USB_IDENTITY_WINDOW;

    for (u64 pa = start; pa < end; pa += PAGE_SIZE) {
        vmm_map_page(MCUBE_DIRECT_MAP_BASE + pa, pa,
                     PTE_PRESENT | PTE_WRITE | PTE_PCD);
    }
    return (void *)(uptr)(MCUBE_DIRECT_MAP_BASE + phys);
}

int usb_device_count(void) {
    return g_dev_count;
}

/* ---- control transfer helpers ---------------------------------------- */

static int usb_control(usb_device_t *dev, u8 type, u8 req, u16 value,
                       u16 index, void *buf, u32 len) {
    usb_setup_t setup;
    setup.bmRequestType = type;
    setup.bRequest = req;
    setup.wValue = value;
    setup.wIndex = index;
    setup.wLength = (u16)len;
    return dev->ctrl->ops->control(dev->ctrl, dev, &setup, buf, len);
}

static int usb_get_desc(usb_device_t *dev, u8 type, u8 index, u16 windex,
                        void *buf, u32 len) {
    return usb_control(dev, 0x80, USB_REQ_GET_DESCRIPTOR,
                       (u16)(((u16)type << 8) | index), windex, buf, len);
}

/* ---- HID report descriptor parser ------------------------------------ */

#define HID_MAX_USAGES 32

typedef struct hid_parse {
    u32 usage_page;
    u32 lmin_raw, lmax_raw;
    u8  lmin_size, lmax_size;
    u32 report_size, report_count;
    u8  report_id;
    u32 bit_pos;
    u32 usages[HID_MAX_USAGES];
    u16 usage_pages[HID_MAX_USAGES];
    int usage_count;
    u32 usage_min, usage_max;
    int has_range;
    struct {
        u32 usage_page;
        u32 lmin_raw, lmax_raw;
        u32 report_size, report_count;
        u8  lmin_size, lmax_size, report_id;
    } stack[4];
    int depth;
} hid_parse_t;

static u32 hid_item_u32(const u8 *p, u32 size) {
    switch (size) {
    case 1: return p[0];
    case 2: return (u32)p[0] | ((u32)p[1] << 8);
    case 4:
        return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
               ((u32)p[3] << 24);
    default: return 0;
    }
}

static i32 hid_sign_extend(u32 raw, u8 size) {
    if (size == 0) return 0;
    u32 bits = (u32)size * 8;
    if (bits >= 32) return (i32)raw;
    if (raw & (1u << (bits - 1))) return (i32)(raw | (~0u << bits));
    return (i32)raw;
}

static void hid_add_input(usb_device_t *dev, hid_parse_t *ps, u32 flags) {
    i32 lo, hi;
    int is_signed = 0;
    if (ps->lmin_size && (ps->lmin_raw & (1u << (ps->lmin_size * 8 - 1))))
        is_signed = 1;
    if (is_signed) {
        lo = hid_sign_extend(ps->lmin_raw, ps->lmin_size);
        hi = hid_sign_extend(ps->lmax_raw, ps->lmax_size);
    } else {
        lo = (i32)ps->lmin_raw;
        hi = (i32)ps->lmax_raw;
    }

    u32 rsize = ps->report_size;
    u32 count = ps->report_count ? ps->report_count : 1;
    if (count > 128) count = 128;

    for (u32 k = 0; k < count; k++) {
        u32 page = 0, usage = 0;
        int usable = 1;
        if (ps->usage_count > 0) {
            int idx = (k < (u32)ps->usage_count) ? (int)k : ps->usage_count - 1;
            page = ps->usage_pages[idx];
            usage = ps->usages[idx];
        } else if (ps->has_range) {
            page = ps->usage_page;
            usage = ps->usage_min + k;
            if (usage > ps->usage_max) usage = ps->usage_max;
        } else {
            usable = 0;
        }

        /* Filter out constant padding (bit0); it only advances the offset. */
        if (!(flags & 0x01) && usable && rsize > 0 && rsize <= 32 &&
            dev->field_count < USB_MAX_HID_FIELDS) {
            usb_hid_field_t *f = &dev->fields[dev->field_count++];
            f->report_id = ps->report_id;
            f->bit_size = (u8)rsize;
            f->flags = (u8)(flags & 0x07);
            f->usage_page = (u16)page;
            f->usage = (u16)usage;
            f->bit_offset = ps->bit_pos;
            f->logical_min = lo;
            f->logical_max = hi;
        }
        ps->bit_pos += rsize;
    }
}

static void hid_select(usb_device_t *dev) {
    int i, j;

    /* 0) Keyboard: any modifier usage (0xE0..0xE7) on the keyboard page. Every
     * boot-compatible keyboard descriptor starts with that range, and decoding
     * does not need the rest of the layout to be known. */
    for (i = 0; i < dev->field_count; i++) {
        usb_hid_field_t *f = &dev->fields[i];
        if (f->usage_page != 0x07 || f->usage < 0xE0 || f->usage > 0xE7)
            continue;
        dev->kind = USB_HID_KEYBOARD;
        dev->report_id = f->report_id;
        break;
    }
    if (dev->kind == USB_HID_KEYBOARD) {
        u32 bits = 0;
        for (i = 0; i < dev->field_count; i++) {
            usb_hid_field_t *f = &dev->fields[i];
            if (f->report_id != dev->report_id) continue;
            u32 end = f->bit_offset + f->bit_size;
            if (end > bits) bits = end;
        }
        dev->report_len = (u16)((bits + 7) / 8 + (dev->report_id ? 1 : 0));
        return;
    }

    /* 1) Touch / digitizer: a report may carry several contacts, so the k-th
     * occurrence of each usage is assigned to contact k. This keeps a plain
     * single-contact digitizer working (contacts == 1) and makes a Windows
     * precision panel usable, where the whole report used to be decoded as if
     * only one of its contacts existed. */
    for (i = 0; i < dev->field_count; i++) {
        usb_hid_field_t *f = &dev->fields[i];
        if (f->usage_page != 0x0D || f->usage != 0x42) continue;

        u8 rid = f->report_id;
        u8 nx = 0, ny = 0, nt = 0, nc = 0;
        for (j = 0; j < dev->field_count; j++) {
            usb_hid_field_t *g = &dev->fields[j];
            if (g->report_id != rid) continue;
            int is_xy = (g->usage_page == 0x0D || g->usage_page == 0x01);
            if (g->usage == 0x30 && is_xy) {
                if (nx < USB_MAX_TOUCH_CONTACTS) dev->t_x[nx] = (i16)j;
                nx++;
            } else if (g->usage == 0x31 && is_xy) {
                if (ny < USB_MAX_TOUCH_CONTACTS) dev->t_y[ny] = (i16)j;
                ny++;
            } else if (g->usage == 0x42 && g->usage_page == 0x0D) {
                if (nt < USB_MAX_TOUCH_CONTACTS) dev->t_tip[nt] = (i16)j;
                nt++;
            } else if (g->usage == 0x51) {
                if (nc < USB_MAX_TOUCH_CONTACTS) dev->t_cid[nc] = (i16)j;
                nc++;
            }
        }
        if (nx == 0 || ny == 0) continue;

        u8 contacts = nx < ny ? nx : ny;
        if (contacts > USB_MAX_TOUCH_CONTACTS) contacts = USB_MAX_TOUCH_CONTACTS;
        dev->touch_contacts = contacts;

        dev->kind = USB_HID_TOUCH;
        dev->report_id = rid;
        dev->x_idx = dev->t_x[0];
        dev->y_idx = dev->t_y[0];
        dev->tip_idx = nt ? dev->t_tip[0] : -1;
        dev->cid_idx = nc ? dev->t_cid[0] : -1;
        break;
    }

    /* 2) Generic desktop X/Y: absolute tablet or relative mouse. */
    if (dev->kind == USB_HID_NONE) {
        int xi = -1, yi = -1, bi = -1;
        u8 rid = 0;
        for (i = 0; i < dev->field_count; i++) {
            usb_hid_field_t *f = &dev->fields[i];
            if (f->usage_page == 0x01 && f->usage == 0x30) {
                xi = i;
                rid = f->report_id;
                break;
            }
        }
        if (xi >= 0) {
            for (j = 0; j < dev->field_count; j++) {
                usb_hid_field_t *g = &dev->fields[j];
                if (g->report_id != rid) continue;
                if (g->usage_page == 0x01 && g->usage == 0x31) yi = j;
                else if (g->usage_page == 0x09 && g->usage == 0x01) bi = j;
            }
        }
        if (xi >= 0 && yi >= 0) {
            usb_hid_field_t *xf = &dev->fields[xi];
            /* Input bit2 means relative; a signed logical minimum does too. */
            int relative = (xf->flags & 0x04) != 0 || xf->logical_min < 0;
            dev->kind = relative ? USB_HID_MOUSE : USB_HID_POINTER;
            dev->report_id = rid;
            dev->x_idx = (i16)xi;
            dev->y_idx = (i16)yi;
            dev->btn_idx = (i16)bi;
        }
    }

    if (dev->kind == USB_HID_NONE) return;

    /* Byte length of the selected report, report ID included. */
    u32 bits = 0;
    for (i = 0; i < dev->field_count; i++) {
        usb_hid_field_t *f = &dev->fields[i];
        if (f->report_id != dev->report_id) continue;
        u32 end = f->bit_offset + f->bit_size;
        if (end > bits) bits = end;
    }
    dev->report_len = (u16)((bits + 7) / 8 + (dev->report_id ? 1 : 0));
}

static void usb_hid_parse(usb_device_t *dev, const u8 *desc, u32 len) {
    hid_parse_t ps;
    memset(&ps, 0, sizeof(ps));

    for (u32 i = 0; i < len;) {
        u8 prefix = desc[i];
        if (prefix == 0xFE) {                   /* long item */
            if (i + 2 >= len) break;
            i += 3 + desc[i + 1];
            continue;
        }

        u32 size = prefix & 0x03;
        if (size == 3) size = 4;
        if (i + 1 + size > len) break;
        const u8 *p = &desc[i + 1];
        u32 raw = hid_item_u32(p, size);
        u32 type = (prefix >> 2) & 0x03;
        u32 tag = prefix >> 4;
        i += 1 + size;

        if (type == 0) {                        /* main item */
            if (tag == 0x8) hid_add_input(dev, &ps, raw);
            ps.usage_count = 0;
            ps.has_range = 0;
            ps.usage_min = 0;
            ps.usage_max = 0;
        } else if (type == 1) {                 /* global item */
            switch (tag) {
            case 0x0: ps.usage_page = raw; break;
            case 0x1: ps.lmin_raw = raw; ps.lmin_size = (u8)size; break;
            case 0x2: ps.lmax_raw = raw; ps.lmax_size = (u8)size; break;
            case 0x7: ps.report_size = raw; break;
            case 0x8:
                if ((u8)raw != ps.report_id) {
                    ps.report_id = (u8)raw;
                    ps.bit_pos = 0;
                }
                break;
            case 0x9: ps.report_count = raw; break;
            case 0xA:                           /* push */
                if (ps.depth < 4) {
                    ps.stack[ps.depth].usage_page = ps.usage_page;
                    ps.stack[ps.depth].lmin_raw = ps.lmin_raw;
                    ps.stack[ps.depth].lmax_raw = ps.lmax_raw;
                    ps.stack[ps.depth].lmin_size = ps.lmin_size;
                    ps.stack[ps.depth].lmax_size = ps.lmax_size;
                    ps.stack[ps.depth].report_size = ps.report_size;
                    ps.stack[ps.depth].report_count = ps.report_count;
                    ps.stack[ps.depth].report_id = ps.report_id;
                    ps.depth++;
                }
                break;
            case 0xB:                           /* pop */
                if (ps.depth > 0) {
                    ps.depth--;
                    ps.usage_page = ps.stack[ps.depth].usage_page;
                    ps.lmin_raw = ps.stack[ps.depth].lmin_raw;
                    ps.lmax_raw = ps.stack[ps.depth].lmax_raw;
                    ps.lmin_size = ps.stack[ps.depth].lmin_size;
                    ps.lmax_size = ps.stack[ps.depth].lmax_size;
                    ps.report_size = ps.stack[ps.depth].report_size;
                    ps.report_count = ps.stack[ps.depth].report_count;
                    ps.report_id = ps.stack[ps.depth].report_id;
                }
                break;
            default: break;
            }
        } else if (type == 2) {                 /* local item */
            switch (tag) {
            case 0x0: {
                u32 page = ps.usage_page;
                u32 usage = raw;
                if (size == 4) {
                    page = raw >> 16;
                    usage = raw & 0xFFFF;
                }
                if (ps.usage_count < HID_MAX_USAGES) {
                    ps.usage_pages[ps.usage_count] = (u16)page;
                    ps.usages[ps.usage_count] = usage;
                    ps.usage_count++;
                }
                break;
            }
            case 0x1:
                ps.usage_min = (size == 4) ? (raw & 0xFFFF) : raw;
                ps.has_range = 1;
                break;
            case 0x2:
                ps.usage_max = (size == 4) ? (raw & 0xFFFF) : raw;
                break;
            default: break;
            }
        }
    }

    hid_select(dev);
}

/* ---- report decoding ------------------------------------------------- */

static int hid_get_bits(const u8 *data, u32 nbytes, u32 bit_off, u32 bit_size,
                        u32 *out) {
    if (bit_size == 0 || bit_size > 32) return -1;
    if ((bit_off + bit_size + 7) / 8 > nbytes) return -1;
    u32 v = 0;
    for (u32 b = 0; b < bit_size; b++) {
        u32 bit = bit_off + b;
        if (data[bit >> 3] & (1u << (bit & 7))) v |= 1u << b;
    }
    *out = v;
    return 0;
}

static i32 hid_field_value(const usb_device_t *dev, const u8 *data, u32 nbytes,
                           int idx) {
    if (idx < 0 || idx >= dev->field_count) return 0;
    const usb_hid_field_t *f = &dev->fields[idx];
    u32 raw;
    if (hid_get_bits(data, nbytes, f->bit_offset, f->bit_size, &raw) != 0)
        return 0;
    if (f->logical_min < 0 && f->bit_size < 32 &&
        (raw & (1u << (f->bit_size - 1)))) {
        raw |= ~0u << f->bit_size;
    }
    return (i32)raw;
}

static i32 hid_scale(i32 v, i32 lo, i32 hi, i32 span) {
    if (span < 0) span = 0;
    if (hi <= lo) return 0;
    i64 num = (i64)(v - lo) * (i64)span;
    i32 out = (i32)(num / (i64)(hi - lo));
    if (out < 0) out = 0;
    if (out > span) out = span;
    return out;
}

/* ---- keyboard decoding ------------------------------------------------ */

/* HID usage 0x04..0x37 -> unshifted ASCII (Boot Keyboard, US layout). */
static const char g_hid_ascii[] =
    "abcdefghijklmnopqrstuvwxyz" "1234567890"
    "\n\033\b\t "                 /* Enter, Esc, Backspace, Tab, Space */
    "-=[]\\;'`,./";               /* 0x2D .. 0x37 */

static const char g_hid_shift_letters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
static const char g_hid_shift_digits[]  = "!@#$%^&*()";
static const char g_hid_shift_punct[]   = { '_', '+', '{', '}', '|', ':',
                                            '"', '~', '<', '>', '?' };

static u32 hid_key_code(u32 usage, int shifted, int caps) {
    if (usage >= 0x04 && usage <= 0x1D) {          /* A..Z */
        u32 k = usage - 0x04;
        int upper = shifted ^ caps;
        return (u32)(u8)(upper ? g_hid_shift_letters[k] : g_hid_ascii[k]);
    }
    if (usage >= 0x1E && usage <= 0x27) {          /* 1..0 */
        u32 k = usage - 0x1E;
        return (u32)(u8)(shifted ? g_hid_shift_digits[k]
                                 : g_hid_ascii[26 + k]);
    }
    if (usage >= 0x28 && usage <= 0x2C)            /* Enter/Esc/BS/Tab/Space */
        return (u32)(u8)g_hid_ascii[36 + (usage - 0x28)];
    if (usage >= 0x2D && usage <= 0x37)
        return (u32)(u8)(shifted ? g_hid_shift_punct[usage - 0x2D]
                                 : g_hid_ascii[41 + (usage - 0x2D)]);

    if (usage >= 0x3A && usage <= 0x45)            /* F1..F12 */
        return INPUT_KEY_F1 + (usage - 0x3A);
    switch (usage) {
    case 0x49: return INPUT_KEY_HOME;   /* Insert has no console equivalent */
    case 0x4A: return INPUT_KEY_HOME;
    case 0x4B: return INPUT_KEY_PGUP;
    case 0x4C: return INPUT_KEY_DELETE;
    case 0x4D: return INPUT_KEY_END;
    case 0x4E: return INPUT_KEY_PGDN;
    case 0x4F: return INPUT_KEY_RIGHT;
    case 0x50: return INPUT_KEY_LEFT;
    case 0x51: return INPUT_KEY_DOWN;
    case 0x52: return INPUT_KEY_UP;
    default: return 0;
    }
}

static void usb_kbd_report(usb_device_t *dev, const u8 *data, u32 len) {
    /* Modifier bits 0..7 = LCtrl, LShift, LAlt, LWin, RCtrl, RShift, RAlt,
     * RWin -- exactly the order of HID usages 0xE0..0xE7. */
    static const u16 mod_codes[8] = {
        INPUT_KEY_LCTRL, INPUT_KEY_LSHIFT, INPUT_KEY_LALT, INPUT_KEY_LWIN,
        INPUT_KEY_RCTRL, INPUT_KEY_RSHIFT, INPUT_KEY_RALT, INPUT_KEY_RWIN,
    };
    u32 mods = 0;
    u8 keys[8];
    int key_cnt = 0;
    int i, j;

    if (dev->field_count == 0) {
        /* Boot protocol fallback: modifier byte, reserved byte, six usages. */
        if (len < 3) return;
        mods = data[0];
        for (int k = 2; k < 8 && k < (int)len; k++) {
            if (data[k] >= 0x04 && data[k] <= 0xA4 && key_cnt < 8)
                keys[key_cnt++] = data[k];
        }
    } else {
        for (i = 0; i < dev->field_count; i++) {
            usb_hid_field_t *f = &dev->fields[i];
            if (f->report_id != dev->report_id) continue;
            if (f->bit_size == 1) {
                if (f->usage >= 0xE0 && f->usage <= 0xE7 &&
                    hid_field_value(dev, data, len, i) != 0)
                    mods |= 1u << (f->usage - 0xE0);
            } else if (f->bit_size == 8) {
                /* Keyboard reports are arrays: the byte is the usage itself. */
                i32 v = hid_field_value(dev, data, len, i);
                if (v >= 0x04 && v <= 0xA4 && key_cnt < 8)
                    keys[key_cnt++] = (u8)v;
            }
        }
    }

    u32 changed = mods ^ dev->key_mods;
    for (int b = 0; b < 8; b++) {
        if (!(changed & (1u << b))) continue;
        input_push_key((mods & (1u << b)) ? mod_codes[b]
                                          : (mod_codes[b] | INPUT_KEY_RELEASE));
    }
    int shifted = ((mods >> 1) & 1) | ((mods >> 5) & 1);
    int alt = ((mods >> 2) & 1) | ((mods >> 6) & 1);
    dev->key_mods = (u8)mods;

    /* Releases first, so a key that rolls into another re-presses cleanly. */
    for (i = 0; i < dev->key_slot_count; i++) {
        int held = 0;
        for (j = 0; j < key_cnt; j++)
            if (keys[j] == dev->key_slots[i]) held = 1;
        if (held) continue;
        u32 code = hid_key_code(dev->key_slots[i], shifted, dev->key_caps);
        if (code) input_push_key(code | INPUT_KEY_RELEASE);
    }

    for (j = 0; j < key_cnt; j++) {
        int existed = 0;
        for (i = 0; i < dev->key_slot_count; i++)
            if (dev->key_slots[i] == keys[j]) existed = 1;
        if (existed) continue;

        if (keys[j] == 0x39) {                     /* Caps Lock latch */
            dev->key_caps ^= 1;
            continue;
        }
        u32 code = hid_key_code(keys[j], shifted, dev->key_caps);
        if (!code) continue;
        if (code < 0x100 && !alt) tty_input((char)code);
        input_push_key(code);
    }

    dev->key_slot_count = (u8)key_cnt;
    for (i = 0; i < key_cnt; i++) dev->key_slots[i] = keys[i];

    if (dev->report_count == 1) {
        kprintf("[usb] keyboard report accepted (mods %02x, %d key(s))\n",
                mods, key_cnt);
    }
}

/* Every one-bit Button-page field is a real button, so the mask covers left,
 * middle and right instead of just the primary button. */
static u32 usb_button_mask(const usb_device_t *dev, const u8 *data, u32 len) {
    u32 mask = 0;
    for (int i = 0; i < dev->field_count; i++) {
        const usb_hid_field_t *f = &dev->fields[i];
        if (f->report_id != dev->report_id) continue;
        if (f->usage_page != 0x09 || f->bit_size != 1) continue;
        if (f->usage < 1 || f->usage > 3) continue;
        if (hid_field_value(dev, data, len, i) != 0)
            mask |= 1u << (f->usage - 1);
    }
    return mask;
}

static void usb_log_touch(usb_device_t *dev, i32 x, i32 y, u32 buttons) {
    i32 dx = x - dev->last_x;
    i32 dy = y - dev->last_y;
    if (dev->report_count > 1 && buttons == dev->last_buttons &&
        dx < USB_TOUCH_LOG_DELTA && dx > -USB_TOUCH_LOG_DELTA &&
        dy < USB_TOUCH_LOG_DELTA && dy > -USB_TOUCH_LOG_DELTA) {
        return;
    }
    kprintf("[usb] touch x=%d y=%d btn=%u\n", x, y, buttons);
    dev->last_x = x;
    dev->last_y = y;
    dev->last_buttons = buttons;
}

static void usb_hid_report(usb_device_t *dev, const u8 *data, u32 len) {
    if (dev->kind == USB_HID_NONE) return;
    if (dev->report_id) {
        if (len < 1 || data[0] != dev->report_id) return;
        data++;
        len--;
    }
    dev->report_count++;

    if (dev->kind == USB_HID_KEYBOARD) {
        usb_kbd_report(dev, data, len);
        return;
    }

    if (dev->kind == USB_HID_MOUSE) {
        if (dev->x_idx < 0) {                    /* boot protocol fallback */
            if (len < 3) return;
            input_push_mouse((i32)(i8)data[1], (i32)(i8)data[2],
                             (u32)(data[0] & 0x07));
            return;
        }
        i32 dx = hid_field_value(dev, data, len, dev->x_idx);
        i32 dy = hid_field_value(dev, data, len, dev->y_idx);
        if (dev->report_count == 1)
            kprintf("[usb] mouse report accepted (dx %d dy %d)\n", dx, dy);
        input_push_mouse(dx, dy, usb_button_mask(dev, data, len));
        return;
    }

    /* Touch: pick the first contact whose tip switch is down. Falling back to
     * contact 0 with no button reports the hover position, which keeps a
     * finger-drag from jumping to a stale contact. */
    if (dev->kind == USB_HID_TOUCH) {
        u8 sel = 0;
        u32 buttons = 0;
        for (u8 t = 0; t < dev->touch_contacts; t++) {
            if (dev->t_tip[t] < 0) continue;
            if (hid_field_value(dev, data, len, dev->t_tip[t]) != 0) {
                sel = t;
                buttons = 1;
                break;
            }
        }
        if (dev->t_x[sel] < 0 || dev->t_y[sel] < 0) return;

        const usb_hid_field_t *xf = &dev->fields[dev->t_x[sel]];
        const usb_hid_field_t *yf = &dev->fields[dev->t_y[sel]];
        i32 rawx = hid_field_value(dev, data, len, dev->t_x[sel]);
        i32 rawy = hid_field_value(dev, data, len, dev->t_y[sel]);
        i32 px = hid_scale(rawx, xf->logical_min, xf->logical_max,
                           g_screen_w - 1);
        i32 py = hid_scale(rawy, yf->logical_min, yf->logical_max,
                           g_screen_h - 1);
        input_push_abs(px, py, buttons);
        usb_log_touch(dev, px, py, buttons);
        return;
    }

    const usb_hid_field_t *xf = &dev->fields[dev->x_idx];
    const usb_hid_field_t *yf = &dev->fields[dev->y_idx];
    i32 rawx = hid_field_value(dev, data, len, dev->x_idx);
    i32 rawy = hid_field_value(dev, data, len, dev->y_idx);
    i32 px = hid_scale(rawx, xf->logical_min, xf->logical_max, g_screen_w - 1);
    i32 py = hid_scale(rawy, yf->logical_min, yf->logical_max, g_screen_h - 1);

    u32 buttons = usb_button_mask(dev, data, len);
    input_push_abs(px, py, buttons);
    usb_log_touch(dev, px, py, buttons);
}

/* ---- configuration descriptor parsing -------------------------------- */

typedef struct usb_hid_info {
    u8  iface;
    u8  protocol;
    u16 report_len;
    int found;
} usb_hid_info_t;

static void usb_parse_config(usb_device_t *dev, const u8 *buf, u32 len,
                             usb_hid_info_t *hi, u8 *cfg_value) {
    u32 off = 0;
    int in_hid = 0;

    memset(hi, 0, sizeof(*hi));
    *cfg_value = 1;

    while (off + 2 <= len) {
        u8 blen = buf[off];
        u8 btype = buf[off + 1];
        if (blen < 2 || off + blen > len) break;

        if (btype == USB_DESC_CONFIG && blen >= 9) {
            *cfg_value = buf[off + 5];
        } else if (btype == USB_DESC_INTERFACE && blen >= 9) {
            u8 cls = buf[off + 5];
            in_hid = (cls == 3) && !hi->found;
            if (in_hid) {
                hi->found = 1;
                hi->iface = buf[off + 2];
                hi->protocol = buf[off + 7];
                /* The interface class identifies HID; report it instead of the
                 * device class, which is 0 for most composite HID devices. */
                dev->class_code = cls;
                dev->subclass = buf[off + 6];
                dev->protocol = buf[off + 7];
            }
        } else if (btype == USB_DESC_HID && blen >= 9 && in_hid) {
            hi->report_len = (u16)(buf[off + 7] | (buf[off + 8] << 8));
        } else if (btype == USB_DESC_ENDPOINT && blen >= 7 && in_hid) {
            u8 attr = buf[off + 3] & 0x03;
            u8 addr = buf[off + 2];
            if (attr == 3 && (addr & 0x80) && dev->ep_in == 0) {
                dev->ep_in = addr;
                dev->ep_maxp = (u16)((buf[off + 4] |
                                      (buf[off + 5] << 8)) & 0x7FF);
                dev->ep_interval = buf[off + 6];
            }
        }
        off += blen;
    }
}

/* ---- device logging -------------------------------------------------- */

static void usb_log_bind(const usb_device_t *dev) {
    if (dev->kind == USB_HID_KEYBOARD) {
        kprintf("[usb] keyboard %04x:%04x class %02x:%02x:%02x -> boot "
                "keyboard, report %u bytes\n", dev->vendor, dev->product,
                dev->class_code, dev->subclass, dev->protocol,
                (u32)dev->report_len);
        return;
    }
    if (dev->kind == USB_HID_MOUSE && dev->x_idx < 0) {
        kprintf("[usb] mouse %04x:%04x class %02x:%02x:%02x -> boot protocol, "
                "3-byte relative\n", dev->vendor, dev->product,
                dev->class_code, dev->subclass, dev->protocol);
        return;
    }

    const char *name, *what;
    switch (dev->kind) {
    case USB_HID_TOUCH:   name = "touch";  what = "touch digitizer";  break;
    case USB_HID_POINTER: name = "tablet"; what = "absolute pointer"; break;
    default:              name = "mouse";  what = "relative pointer"; break;
    }

    const usb_hid_field_t *xf = &dev->fields[dev->x_idx];
    const usb_hid_field_t *yf = &dev->fields[dev->y_idx];
    kprintf("[usb] %s %04x:%04x class %02x:%02x:%02x -> %s, "
            "x %d..%d, y %d..%d, report id %u\n", name,
            dev->vendor, dev->product, dev->class_code, dev->subclass,
            dev->protocol, what, xf->logical_min, xf->logical_max,
            yf->logical_min, yf->logical_max, (u32)dev->report_id);

    if (dev->kind == USB_HID_TOUCH) {
        kprintf("[usb] touch contacts=%u, report %u bytes, tip %s\n",
                (u32)dev->touch_contacts, (u32)dev->report_len,
                dev->t_tip[0] >= 0 ? "switch" : "none");
    }
}

/* ---- enumeration ----------------------------------------------------- */

static void usb_enumerate(usb_controller_t *c, int port) {
    if (g_dev_count >= USB_MAX_DEVICES) return;

    usb_device_t *dev = &g_devices[g_dev_count];
    memset(dev, 0, sizeof(*dev));
    dev->ctrl = c;
    dev->port = (u8)port;
    dev->x_idx = -1;
    dev->y_idx = -1;
    dev->btn_idx = -1;
    dev->tip_idx = -1;
    dev->cid_idx = -1;
    dev->touch_contacts = 0;
    for (int t = 0; t < USB_MAX_TOUCH_CONTACTS; t++) {
        dev->t_x[t] = -1;
        dev->t_y[t] = -1;
        dev->t_tip[t] = -1;
        dev->t_cid[t] = -1;
    }

    if (c->ops->dev_open(c, port, dev) != 0) {
        kprintf("[usb] %s port %d: reset failed\n", c->ops->name, port);
        usb_diag_note("%s p%d: port reset failed", c->ops->name, port);
        return;
    }

    usb_device_desc_t dd;
    memset(&dd, 0, sizeof(dd));
    if (usb_get_desc(dev, USB_DESC_DEVICE, 0, 0, &dd, sizeof(dd)) != 0 ||
        dd.bLength < 18) {
        kprintf("[usb] %s port %d: GET_DESCRIPTOR(device) failed\n",
                c->ops->name, port);
        usb_diag_note("%s p%d: no device descriptor", c->ops->name, port);
        return;
    }
    dev->vendor = dd.idVendor;
    dev->product = dd.idProduct;
    dev->class_code = dd.bDeviceClass;
    dev->subclass = dd.bDeviceSubClass;
    dev->protocol = dd.bDeviceProtocol;
    dev->ep0_maxp = dd.bMaxPacketSize0 ? dd.bMaxPacketSize0 : 8;

    u8 addr = (u8)(g_dev_count + 1);
    if (c->ops->set_address(c, dev, addr) != 0) {
        kprintf("[usb] %s port %d: SET_ADDRESS failed\n", c->ops->name, port);
        usb_diag_note("%s p%d: SET_ADDRESS failed", c->ops->name, port);
        return;
    }

    kprintf("[usb] device %04x:%04x on %s port %d, addr %u\n",
            dev->vendor, dev->product, c->ops->name, port,
            (u32)dev->address);

    usb_config_desc_t cdesc;
    memset(&cdesc, 0, sizeof(cdesc));
    if (usb_get_desc(dev, USB_DESC_CONFIG, 0, 0, &cdesc, 9) != 0 ||
        cdesc.bLength < 9) {
        kprintf("[usb] %s port %d: GET_DESCRIPTOR(config) failed\n",
                c->ops->name, port);
        return;
    }

    u32 total = cdesc.wTotalLength;
    if (total < 9) total = 9;
    if (total > 2048) total = 2048;
    memset(g_desc, 0, total);
    int got = usb_get_desc(dev, USB_DESC_CONFIG, 0, 0, g_desc, total);
    if (got < 0) {
        kprintf("[usb] %s port %d: config descriptor read failed\n",
                c->ops->name, port);
        return;
    }

    usb_hid_info_t hi;
    u8 cfg_value = 1;
    u32 clen = cdesc.wTotalLength;
    if (clen > total) clen = total;
    usb_parse_config(dev, g_desc, clen, &hi, &cfg_value);

    if (usb_control(dev, 0x00, USB_REQ_SET_CONFIGURATION, cfg_value, 0,
                    NULL, 0) != 0) {
        kprintf("[usb] %s port %d: SET_CONFIGURATION(%u) failed\n",
                c->ops->name, port, (u32)cfg_value);
        return;
    }

    if (!hi.found) {
        kprintf("[usb] %04x:%04x is not a HID interface, ignored\n",
                dev->vendor, dev->product);
        return;
    }

    u32 rlen = hi.report_len;
    if (rlen > 1024) rlen = 1024;
    /* Report descriptors are interface recipients (bmRequestType 0x81). */
    if (rlen && usb_control(dev, 0x81, USB_REQ_GET_DESCRIPTOR,
                            (u16)(USB_DESC_REPORT << 8), hi.iface, g_erbuf,
                            rlen) == 0) {
        usb_hid_parse(dev, g_erbuf, rlen);
    }

    if (dev->kind == USB_HID_NONE) {
        if (hi.protocol == 2) {
            /* No usable report descriptor: HID boot protocol, 3-byte mouse. */
            dev->kind = USB_HID_MOUSE;
            dev->report_len = 3;
            kprintf("[usb] %04x:%04x report descriptor unusable, "
                    "falling back to HID boot protocol (mouse)\n",
                    dev->vendor, dev->product);
        } else if (hi.protocol == 1) {
            /* Boot keyboard: 8-byte report, decoded positionally. */
            dev->kind = USB_HID_KEYBOARD;
            dev->report_len = 8;
            kprintf("[usb] %04x:%04x report descriptor unusable, "
                    "falling back to HID boot protocol (keyboard)\n",
                    dev->vendor, dev->product);
        } else {
            kprintf("[usb] hid %04x:%04x has no pointer/touch/keyboard usage, "
                    "ignored\n", dev->vendor, dev->product);
            return;
        }
    }

    /* Periodic reports instead of the 10 ms HID default. */
    usb_control(dev, 0x21, USB_REQ_SET_IDLE, 0, hi.iface, NULL, 0);

    if (!dev->ep_in || c->ops->intr_open(c, dev, dev->ep_in, dev->ep_maxp,
                                         dev->ep_interval) != 0) {
        kprintf("[usb] %04x:%04x: no usable interrupt IN endpoint\n",
                dev->vendor, dev->product);
        usb_diag_note("%04x:%04x: no interrupt endpoint", dev->vendor,
                 dev->product);
        return;
    }

    usb_log_bind(dev);
    g_dev_count++;

    if (dev->kind == USB_HID_TOUCH || dev->kind == USB_HID_POINTER) {
        kprintf("USER_USB_TOUCH_OK\n");
    } else if (dev->kind == USB_HID_KEYBOARD) {
        kprintf("USER_USB_KEYBOARD_OK\n");
    }
}

/* ---- init / runtime poll ---------------------------------------------- */

static void usb_poll_cb(void *arg) {
    UNUSED(arg);
    usb_poll();
}

int usb_init(void) {
    mcube_fb_info_t fb;

    g_ctrl_count = 0;
    g_dev_count = 0;
    g_n_uhci = g_n_ehci = g_n_xhci = 0;
    g_rescans = 0;
    g_poll_armed = 0;
    g_enum_busy = 0;
    g_note[0] = 0;
    for (int i = 0; i < USB_MAX_CONTROLLERS; i++) g_ctrl_started[i] = 0;

    if (fb_get_info(&fb) == 0 && fb.width > 1 && fb.height > 1) {
        g_screen_w = (i32)fb.width;
        g_screen_h = (i32)fb.height;
    }

    u64 dphys = usb_dma_alloc(1);
    u64 rphys = usb_dma_alloc(1);
    u64 ephys = usb_dma_alloc(1);
    if (!dphys || !rphys || !ephys) {
        kprintf("[usb] out of memory for DMA buffers\n");
        if (dphys) pmm_free_page(dphys);
        if (rphys) pmm_free_page(rphys);
        if (ephys) pmm_free_page(ephys);
        usb_diag_note("out of DMA memory");
        return -1;
    }
    g_desc = (u8 *)phys_to_virt(dphys);
    g_rbuf = (u8 *)phys_to_virt(rphys);
    g_erbuf = (u8 *)phys_to_virt(ephys);

    g_n_uhci = (u32)usb_uhci_probe();
    g_n_ehci = (u32)usb_ehci_probe();
    g_n_xhci = (u32)usb_xhci_probe();
    kprintf("[usb] controller probes: uhci %u, ehci %u, xhci %u\n",
            g_n_uhci, g_n_ehci, g_n_xhci);

    if (g_ctrl_count == 0) {
        kprintf("[usb] no USB host controller found\n");
        usb_diag_note("no USB host controller");
        return -1;
    }

    for (int ci = 0; ci < g_ctrl_count; ci++) {
        usb_controller_t *c = &g_ctrls[ci];
        kprintf("[usb] %s at PCI %02x:%02x.%u, %d port(s)\n", c->ops->name,
                c->bus, c->slot, c->func, c->ops->port_count(c));
    }
    usb_diag_note("probed %u uhci, %u ehci, %u xhci", g_n_uhci, g_n_ehci,
             g_n_xhci);
    return 0;
}

/* Port-by-port enumeration in thread context. Nothing here may block the boot:
 * the thread is spawned after sched_start(), so even a port that never answers
 * only delays input, never the desktop.
 *
 * The same thread keeps rescanning after the first pass. A board whose firmware
 * needs a moment before it answers on the root ports, or a user who plugs the
 * keyboard in after the desktop came up, then still gets input instead of a
 * machine that stays dead until reboot. */

#define USB_RESCAN_PERIOD_MS 750
#define USB_RESCAN_BUDGET_MS 3000

static int usb_port_taken(usb_controller_t *c, int port) {
    for (int i = 0; i < g_dev_count; i++)
        if (g_devices[i].ctrl == c && g_devices[i].port == port) return 1;
    return 0;
}

/* One pass over a controller's root ports. Returns the number of devices bound
 * during the pass; `budget_ms` caps the wall-clock time a wedged port may eat. */
static int usb_scan_controller(usb_controller_t *c, u32 budget_ms) {
    int ports = c->ops->port_count(c);
    if (ports <= 0) return 0;

    int before = g_dev_count;
    u64 t0 = timer_uptime_ms();

    for (int p = 1; p <= ports; p++) {
        if (timer_uptime_ms() - t0 > budget_ms) {
            kprintf("[usb] %s: scan budget of %u ms used up\n", c->ops->name,
                    budget_ms);
            break;
        }
        if (g_dev_count >= USB_MAX_DEVICES) break;
        if (usb_port_taken(c, p)) continue;
        if (!c->ops->port_connected(c, p)) continue;

        u64 pt = timer_uptime_ms();
        kprintf("[usb] %s port %d: device attached\n", c->ops->name, p);
        usb_enumerate(c, p);
        pt = timer_uptime_ms() - pt;
        if (pt > 400)
            kprintf("[usb] %s port %d: took %u ms\n", c->ops->name, p,
                    (u32)pt);
    }
    return g_dev_count - before;
}

void usb_enum_task(void *arg) {
    UNUSED(arg);

    if (g_ctrl_count == 0 || !g_desc || !g_rbuf) return;

    /* Let the desktop paint itself before any controller is woken up: the
     * start hook resets the xHCI and powers its ports, which is the point
     * where a firmware with live legacy SMI handlers can freeze the machine.
     * Happening after the first frame means the user still sees a desktop. */
    timer_sleep_ms(1500);

    int first = 1;
    for (;;) {
        u64 flags = irq_save();
        g_enum_busy = 1;
        irq_restore(flags);

        for (int ci = 0; ci < g_ctrl_count; ci++) {
            usb_controller_t *c = &g_ctrls[ci];
            if (c->ops->port_count(c) <= 0) continue;

            if (!g_ctrl_started[ci]) {
                /* Try the handoff exactly once: a firmware that refuses it
                 * keeps the controller, and resetting it every pass would only
                 * add risk without changing the outcome. */
                g_ctrl_started[ci] = 1;
                if (!c->ops->start || c->ops->start(c) != 0) {
                    kprintf("[usb] %s: controller did not start, skipped\n",
                            c->ops->name);
                    usb_diag_note("%s: start failed", c->ops->name);
                    continue;
                }
                kprintf("[usb] scanning %s, %d port(s)\n", c->ops->name,
                        c->ops->port_count(c));
            }

            usb_scan_controller(c, first ? USB_ENUM_BUDGET_MS
                                         : USB_RESCAN_BUDGET_MS);
        }

        flags = irq_save();
        g_enum_busy = 0;
        irq_restore(flags);

        if (first) {
            kprintf("[usb] first pass done: %d HID device(s) on %d "
                    "controller(s)\n", g_dev_count, g_ctrl_count);
            first = 0;
        } else {
            g_rescans++;
        }

        /* Arm the poll timer as soon as anything usable exists, and never
         * give up on it afterwards: the pass above keeps watching for
         * hot-plugged devices. */
        if (g_dev_count > 0 && !g_poll_armed) {
            timer_add(8, 8, usb_poll_cb, NULL);
            g_poll_armed = 1;
            kprintf("[usb] polling %d HID device(s) every 8 ms\n", g_dev_count);
        }

        timer_sleep_ms(USB_RESCAN_PERIOD_MS);
    }
}

/* Called from the soft timer (PIT context, interrupts off): only touch
 * registers and copy already-completed DMA buffers, never sleep. */
void usb_poll(void) {
    if (g_enum_busy) return;

    for (int i = 0; i < g_dev_count; i++) {
        usb_device_t *dev = &g_devices[i];
        usb_controller_t *c = dev->ctrl;
        if (!dev->ep_in || !c || !c->ops->intr_poll) continue;

        u32 want = dev->report_len ? dev->report_len : dev->ep_maxp;
        if (want > 512) want = 512;
        if (want == 0) continue;

        int n = c->ops->intr_poll(c, dev, g_rbuf, want);
        if (n <= 0) continue;
        if ((u32)n > want) n = (int)want;
        usb_hid_report(dev, g_rbuf, (u32)n);
    }
}

/* ---- diagnostics ------------------------------------------------------ */

void usb_get_info(struct mcube_usb_info *out) {
    if (!out) return;

    mcube_usb_info_t *i = (mcube_usb_info_t *)out;
    memset(i, 0, sizeof(*i));

    i->ctrl_uhci = g_n_uhci;
    i->ctrl_ehci = g_n_ehci;
    i->ctrl_xhci = g_n_xhci;
    i->devs = (u32)g_dev_count;
    i->rescans = g_rescans;

    for (int ci = 0; ci < g_ctrl_count; ci++) {
        usb_controller_t *c = &g_ctrls[ci];
        int ports = c->ops->port_count(c);
        if (ports <= 0) continue;
        i->ports += (u32)ports;
        for (int p = 1; p <= ports; p++)
            if (c->ops->port_connected(c, p)) i->ccs++;
    }

    for (int d = 0; d < g_dev_count; d++) {
        switch (g_devices[d].kind) {
        case USB_HID_KEYBOARD: i->kbd++; break;
        case USB_HID_MOUSE:
        case USB_HID_POINTER:  i->ptr++; break;
        case USB_HID_TOUCH:    i->touch++; break;
        default: break;
        }
    }

    usize n = strlen(g_note);
    if (n >= sizeof(i->text)) n = sizeof(i->text) - 1;
    memcpy(i->text, g_note, n);
    i->text[n] = 0;
}
