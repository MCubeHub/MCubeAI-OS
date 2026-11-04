/* xHCI (USB 3.x) host controller driver.
 *
 * Every machine built after ~2013 (including current Seewo all-in-ones) only
 * exposes xHCI: PCI class 0x0C/0x03/0x30, memory mapped BAR0, a command ring,
 * one event ring per interrupter, device context arrays and transfer rings.
 * USB 2 ports sit behind the same controller, so a full-speed touch panel is
 * handled here instead of being handed to a companion controller.
 *
 * Only what the kernel needs is implemented: slot allocation, Address Device,
 * control transfers on EP0 and periodic transfers on one interrupt IN
 * endpoint. The event ring is polled by the soft timer; interrupts are left
 * masked, which is why the driver never needs an IRQ handler.
 */

#include <mcube/usb.h>
#include <mcube/kernel.h>
#include <mcube/mm.h>
#include <mcube/pci.h>
#include <mcube/timer.h>

#define XHCI_MAX_SLOTS      32
#define XHCI_CMD_TRBS       32
#define XHCI_EVENT_TRBS     32
#define XHCI_RING_TRBS      32

/* Operational registers, relative to CAPLENGTH. */
#define OP_USBCMD       0x00
#define OP_USBSTS       0x04
#define OP_PAGESIZE     0x08
#define OP_DNCTRL       0x14
#define OP_CRCR         0x18
#define OP_DCBAAP       0x30
#define OP_CONFIG       0x38
#define OP_PORTSC(n)    (0x400 + 0x10 * (n))

#define CMD_RUN         (1u << 0)
#define CMD_HCRST       (1u << 1)
#define CMD_INTE        (1u << 2)

#define STS_HCH         (1u << 0)
#define STS_HSE         (1u << 2)
#define STS_EINT        (1u << 3)
#define STS_PCD         (1u << 4)
#define STS_CNR         (1u << 11)
#define STS_HCE         (1u << 12)

/* Runtime registers, relative to RTSOFF. */
#define RT_IMAN         0x20
#define RT_ERSTSZ       0x28
#define RT_ERSTBA       0x30
#define RT_ERDP         0x38

/* Port status/control. */
#define PS_CCS          (1u << 0)
#define PS_PED          (1u << 1)
#define PS_PR           (1u << 4)
#define PS_PLS_MASK     (0xFu << 5)
#define PS_PP           (1u << 9)
#define PS_SPEED(p)     (((p) >> 10) & 0x0F)
#define PS_CSC          (1u << 17)
#define PS_PEC          (1u << 18)
#define PS_WRC          (1u << 19)
#define PS_OCC          (1u << 20)
#define PS_PRC          (1u << 21)
#define PS_PLC          (1u << 22)
#define PS_CEC          (1u << 23)
#define PS_CHANGE_MASK  (PS_CSC | PS_PEC | PS_WRC | PS_OCC | PS_PRC | PS_PLC | \
                         PS_CEC)

/* TRB types. */
#define TRB_NORMAL      1
#define TRB_SETUP       2
#define TRB_DATA        3
#define TRB_STATUS      4
#define TRB_LINK        6
#define TRB_ENABLE_SLOT 9
#define TRB_DISABLE_SLOT 10
#define TRB_ADDRESS_DEV 11
#define TRB_CONFIG_EP   12
#define TRB_EVAL_CTX    13
#define TRB_TRANSFER_EV 32
#define TRB_CMD_COMPLETE 33

#define TRB_CYCLE       (1u << 0)
#define TRB_TC          (1u << 1)
#define TRB_CHAIN       (1u << 4)
#define TRB_IOC         (1u << 5)
#define TRB_IDT         (1u << 6)
#define TRB_TYPE(t)     ((u32)(t) << 10)
#define TRB_DIR_IN      (1u << 16)
#define TRB_TRT_OUT     (2u << 16)
#define TRB_TRT_IN      (3u << 16)
#define TRB_SLOT(s)     ((u32)(s) << 24)

/* Completion codes. */
#define CC_SUCCESS      1
#define CC_SHORT_PACKET 13

/* Context field helpers. */
#define SLOT0_SPEED(s)  ((u32)(s) << 20)
#define SLOT0_ENTRIES(n) ((u32)(n) << 27)
#define SLOT1_PORT(p)   ((u32)(p) << 16)
#define EP1_CERR(n)     ((u32)(n) << 1)
#define EP1_TYPE(t)     ((u32)(t) << 3)
#define EP1_MAXPACKET(p) ((u32)(p) << 16)
#define EP0_INTERVAL(i) ((u32)(i) << 16)

#define EP_TYPE_CONTROL 4
#define EP_TYPE_INT_IN  7

#define ctx_size(hc)    ((u32)(hc)->csz * 32u)

#define barrier() __asm__ __volatile__("" ::: "memory")

typedef struct PACKED xhci_trb {
    u64 param;
    u32 status;
    u32 control;
} xhci_trb_t;

typedef struct xhci_ring {
    xhci_trb_t *trbs;
    u64 phys;
    u32 size;
    u32 enq;
    u8 ccs;
} xhci_ring_t;

typedef struct xhci_dev {
    u8 slot;
    u8 ep_id;
    u8 armed;
    u8 pending;
    u8 ep0_done;
    u32 ep0_code;
    u32 irq_code;
    xhci_ring_t ep0;
    xhci_ring_t irq;
    u8 *irq_buf;
    u64 irq_buf_phys;
    u32 irq_len;
    u8 *dev_ctx;
    u64 dev_ctx_phys;
    u8 *in_ctx;
    u64 in_ctx_phys;
    u8 ep0_maxp;
} xhci_dev_t;

typedef struct xhci_ctrl {
    volatile u8 *cap;
    volatile u8 *op;
    volatile u8 *rt;
    volatile u8 *db;
    u64 base_phys;
    int ports;
    u32 max_slots;
    u8  csz;                /* context size in 32-byte units (1 or 2) */
    u8  ppc;
    u8  started;            /* reset + run already done by xhci_start() */
    u32 sp_count;           /* scratchpad buffers the controller asks for */

    u64 *dcbaa;
    u64 dcbaa_phys;

    xhci_ring_t cmd;
    xhci_trb_t *ev;
    u64 ev_phys;
    u32 ev_deq;
    u8 ev_ccs;
    u32 *erst;
    u64 erst_phys;

    u8 *data;
    u64 data_phys;

    /* Last command completion, filled in by the event drain. */
    u8 cmd_done;
    u32 cmd_code;
    u64 cmd_trb;
    u32 cmd_slot;

    xhci_dev_t *slots[XHCI_MAX_SLOTS + 1];
} xhci_ctrl_t;

static const usb_hcd_ops_t g_xhci_ops;
static int xhci_start(usb_controller_t *c);

static inline u32 xr(volatile u8 *base, u32 off) {
    return *(volatile u32 *)(base + off);
}

static inline void xw(volatile u8 *base, u32 off, u32 val) {
    *(volatile u32 *)(base + off) = val;
}

/* ---- rings ------------------------------------------------------------ */

static void ring_init(xhci_ctrl_t *hc, xhci_ring_t *r, u64 phys, u32 trbs) {
    UNUSED(hc);
    r->trbs = (xhci_trb_t *)phys_to_virt(phys);
    r->phys = phys;
    r->size = trbs;
    r->enq = 0;
    r->ccs = 1;
}

static void ring_enqueue(xhci_ring_t *r, u64 param, u32 status, u32 control) {
    if (r->enq >= r->size - 1) {
        xhci_trb_t *link = &r->trbs[r->size - 1];
        link->param = r->phys;
        link->status = 0;
        barrier();
        link->control = TRB_TYPE(TRB_LINK) | TRB_TC | r->ccs;
        barrier();
        r->ccs ^= 1;
        r->enq = 0;
    }

    xhci_trb_t *t = &r->trbs[r->enq];
    t->param = param;
    t->status = status;
    barrier();
    t->control = control | r->ccs;
    r->enq++;
}

static u64 ring_next_phys(const xhci_ring_t *r) {
    if (r->enq >= r->size - 1) return r->phys;
    return r->phys + (u64)r->enq * sizeof(xhci_trb_t);
}

/* ---- event ring ------------------------------------------------------- */

static void xhci_submit_irq(xhci_ctrl_t *hc, xhci_dev_t *d) {
    ring_enqueue(&d->irq, d->irq_buf_phys, d->irq_len,
                 TRB_TYPE(TRB_NORMAL) | TRB_IOC);
    barrier();
    xw(hc->db, (u32)d->slot * 4, d->ep_id);
}

/* Consume every event the controller has produced. Command completions are
 * recorded for the synchronous wait; transfer events mark the owning device. */
static void xhci_drain(xhci_ctrl_t *hc) {
    /* Bounded: a controller that keeps re-posting events (a flapping port on a
     * 20-port xHCI, or a desynchronised ring) would otherwise spin here with
     * interrupts enabled but no way out. Two ring passes are plenty. */
    for (u32 guard = 0; guard < XHCI_EVENT_TRBS * 2; guard++) {
        xhci_trb_t *ev = &hc->ev[hc->ev_deq];
        if ((ev->control & TRB_CYCLE) != hc->ev_ccs) break;

        u32 type = (ev->control >> 10) & 0x3F;
        if (type == TRB_CMD_COMPLETE) {
            hc->cmd_code = (ev->status >> 24) & 0xFF;
            hc->cmd_trb = ev->param;
            hc->cmd_slot = (ev->control >> 24) & 0xFF;
            hc->cmd_done = 1;
        } else if (type == TRB_TRANSFER_EV) {
            u32 slot = (ev->control >> 24) & 0xFF;
            u32 ep = (ev->control >> 16) & 0x1F;
            u32 code = (ev->status >> 24) & 0xFF;
            xhci_dev_t *d = (slot <= XHCI_MAX_SLOTS) ? hc->slots[slot] : NULL;
            if (d) {
                if (ep == 1) {
                    d->ep0_code = code;
                    d->ep0_done = 1;
                } else if (ep == d->ep_id) {
                    d->irq_code = code;
                    d->pending = 1;
                    d->armed = 0;
                }
            }
        }

        hc->ev_deq++;
        if (hc->ev_deq >= XHCI_EVENT_TRBS) {
            hc->ev_deq = 0;
            hc->ev_ccs ^= 1;
        }

        /* Advance the dequeue pointer, clearing EHB. */
        u64 erdp = hc->ev_phys + (u64)hc->ev_deq * sizeof(xhci_trb_t);
        xw(hc->rt, RT_ERDP + 4, (u32)(erdp >> 32));
        xw(hc->rt, RT_ERDP, (u32)erdp | (1u << 3));
    }
}

static int xhci_wait_cmd(xhci_ctrl_t *hc, u64 trb_phys, u32 *slot_out) {
    hc->cmd_done = 0;
    for (int ms = 0; ms < 250; ms++) {
        xhci_drain(hc);
        if (hc->cmd_done && hc->cmd_trb == trb_phys) {
            if (slot_out) *slot_out = hc->cmd_slot;
            if (hc->cmd_code != CC_SUCCESS) {
                kprintf("[usb] xhci command failed, completion code %u\n",
                        (u32)hc->cmd_code);
                return -1;
            }
            return 0;
        }
        timer_sleep_ms(1);
    }
    kprintf("[usb] xhci command timeout\n");
    return -1;
}

static void xhci_ring_cmd(xhci_ctrl_t *hc, u64 param, u32 status,
                          u32 control, u64 *trb_phys) {
    *trb_phys = ring_next_phys(&hc->cmd);
    ring_enqueue(&hc->cmd, param, status, control);
    barrier();
    xw(hc->db, 0, 0);
}

/* ---- control transfers ------------------------------------------------ */

static int xhci_control(usb_controller_t *c, usb_device_t *dev,
                        const usb_setup_t *setup, void *buf, u32 len) {
    xhci_ctrl_t *hc = (xhci_ctrl_t *)c->priv;
    xhci_dev_t *d = (xhci_dev_t *)dev->hcd_priv;
    if (!d) return -1;

    if (len > 2048) len = 2048;
    int in = (setup->bmRequestType & 0x80) != 0;

    u64 setup_val = 0;
    memcpy(&setup_val, setup, sizeof(usb_setup_t));
    if (buf && len && !in) memcpy(hc->data, buf, len);

    /* Setup, optional data and status form one transfer: all but the last
     * TRB carry CHAIN so the controller drives them as a unit. */
    u32 trt = (len == 0) ? 0 : (in ? TRB_TRT_IN : TRB_TRT_OUT);
    ring_enqueue(&d->ep0, setup_val, sizeof(usb_setup_t),
                 TRB_TYPE(TRB_SETUP) | TRB_IDT | TRB_CHAIN | trt);
    if (len)
        ring_enqueue(&d->ep0, hc->data_phys, len,
                     TRB_TYPE(TRB_DATA) | TRB_CHAIN | (in ? TRB_DIR_IN : 0));
    ring_enqueue(&d->ep0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC);

    d->ep0_done = 0;
    d->ep0_code = 0;
    barrier();
    xw(hc->db, (u32)d->slot * 4, 1);

    for (int ms = 0; ms < 500; ms++) {
        xhci_drain(hc);
        if (d->ep0_done) break;
        timer_sleep_ms(1);
    }

    if (!d->ep0_done ||
        (d->ep0_code != CC_SUCCESS && d->ep0_code != CC_SHORT_PACKET)) {
        kprintf("[usb] xhci control failed (slot %u, req %u, code %u)\n",
                (u32)d->slot, (u32)setup->bRequest, (u32)d->ep0_code);
        return -1;
    }

    if (in && buf && len) memcpy(buf, hc->data, len);
    return 0;
}

/* ---- controller / port operations ------------------------------------ */

static int xhci_port_count(usb_controller_t *c) {
    return ((xhci_ctrl_t *)c->priv)->ports;
}

static int xhci_port_connected(usb_controller_t *c, int port) {
    xhci_ctrl_t *hc = (xhci_ctrl_t *)c->priv;
    u32 reg = OP_PORTSC(port - 1);
    u32 v = xr(hc->op, reg);
    /* A port index past the register block (bad HCS1 count) reads back all
     * ones, and bit 0 would look like "connected". Treat it as empty. */
    if (v == 0xFFFFFFFFu) return 0;
    /* Acknowledge pending change bits while scanning. Leaving them set on a
     * port we end up skipping keeps the controller posting port-status-change
     * events (the 2018 Seewo xHCI has 20 ports, several of them unpopulated). */
    if (v & PS_CHANGE_MASK)
        xw(hc->op, reg, (v & ~PS_CHANGE_MASK) | PS_PP | PS_CHANGE_MASK);
    return (v & PS_CCS) != 0;
}

static int xhci_set_address(usb_controller_t *c, usb_device_t *dev,
                            u8 address) {
    xhci_ctrl_t *hc = (xhci_ctrl_t *)c->priv;
    xhci_dev_t *d = (xhci_dev_t *)dev->hcd_priv;
    UNUSED(address);

    /* The slot id already is the device address; update EP0's packet size if
     * the descriptor reported something different from the address default. */
    if (d && dev->ep0_maxp && dev->ep0_maxp != d->ep0_maxp) {
        u8 *in = d->in_ctx;
        u32 csz = ctx_size(hc);
        memset(in, 0, 4096);
        *(u32 *)(in + 4) = 0x2;                     /* add slot + EP0 */
        u32 *slot = (u32 *)(in + csz);
        slot[0] = SLOT0_SPEED(dev->speed) | SLOT0_ENTRIES(1);
        slot[1] = SLOT1_PORT(dev->port);
        u32 *ep0 = (u32 *)(in + csz * 2);
        ep0[1] = EP1_MAXPACKET(dev->ep0_maxp) | EP1_CERR(3) |
                 EP1_TYPE(EP_TYPE_CONTROL);
        ep0[2] = (u32)d->ep0.phys | 1;
        ep0[3] = (u32)(d->ep0.phys >> 32);
        ep0[4] = 8;

        u64 trb = 0;
        xhci_ring_cmd(hc, d->in_ctx_phys, 0, TRB_TYPE(TRB_EVAL_CTX) |
                      TRB_SLOT(d->slot), &trb);
        if (xhci_wait_cmd(hc, trb, NULL) == 0) d->ep0_maxp = (u8)dev->ep0_maxp;
    }

    if (d) dev->address = d->slot;
    return 0;
}

static int xhci_dev_open(usb_controller_t *c, int port, usb_device_t *dev);

/* Release one device: Disable Slot (so the controller stops DMA into these
 * rings) and return every page xhci_dev_open()/xhci_intr_open() allocated.
 * Every failure path funnels through here, which is what keeps a port that
 * fails enumeration from leaking memory on each 750 ms rescan. */
static void xhci_dev_release(xhci_ctrl_t *hc, xhci_dev_t *d) {
    if (!d) return;

    if (d->slot && d->slot <= XHCI_MAX_SLOTS) {
        if (hc->slots[d->slot] == d) hc->slots[d->slot] = NULL;
        if (hc->dcbaa) hc->dcbaa[d->slot] = 0;
        u64 trb = 0;
        xhci_ring_cmd(hc, 0, 0,
                      TRB_TYPE(TRB_DISABLE_SLOT) | TRB_SLOT(d->slot), &trb);
        xhci_wait_cmd(hc, trb, NULL);
        d->slot = 0;
    }

    usb_dma_free(d->ep0.phys, 1);
    usb_dma_free(d->dev_ctx_phys, 2);
    usb_dma_free(d->in_ctx_phys, 2);
    usb_dma_free(d->irq.phys, 1);
    usb_dma_free(d->irq_buf_phys, 1);
    kfree(d);
}

static void xhci_dev_close(usb_controller_t *c, usb_device_t *dev) {
    xhci_ctrl_t *hc = (xhci_ctrl_t *)c->priv;
    xhci_dev_t *d = dev ? (xhci_dev_t *)dev->hcd_priv : NULL;
    if (!hc || !d) return;

    dev->hcd_priv = NULL;
    xhci_dev_release(hc, d);

    /* Leave the port powered and clear the sticky change bits. A full port
     * reset here would immediately queue the same failing device again; the
     * normal rescan path still retries it after a replug. */
    if (dev->port > 0 && dev->port <= (u8)hc->ports) {
        u32 reg = OP_PORTSC(dev->port - 1);
        u32 v = xr(hc->op, reg);
        xw(hc->op, reg, (v & ~PS_PLS_MASK) | PS_CHANGE_MASK | (v & PS_PP));
    }
}

static int xhci_dev_open(usb_controller_t *c, int port, usb_device_t *dev) {
    xhci_ctrl_t *hc = (xhci_ctrl_t *)c->priv;
    u32 reg = OP_PORTSC(port - 1);
    u32 v = xr(hc->op, reg);

    if (!(v & PS_CCS)) return -1;
    if (xr(hc->op, OP_USBSTS) & (STS_HCH | STS_HSE)) {
        kprintf("[usb] xhci: controller halted or failed, port %d skipped\n",
                port);
        return -1;
    }

    if (hc->ppc && !(v & PS_PP)) {
        xw(hc->op, reg, (v & ~PS_CHANGE_MASK) | PS_PP);
        timer_sleep_ms(20);
        v = xr(hc->op, reg);
    }

    /* Reset, keeping port power on and not clearing change bits yet. */
    xw(hc->op, reg, (v & ~PS_CHANGE_MASK) | PS_PP | PS_PR);

    v = xr(hc->op, reg);
    for (int ms = 0; ms < 120; ms++) {
        v = xr(hc->op, reg);
        if (!(v & PS_PR) && (v & PS_PED)) break;
        timer_sleep_ms(1);
    }
    if (!(v & PS_PED)) {
        xw(hc->op, reg, (v & ~PS_CHANGE_MASK) | PS_CHANGE_MASK | PS_PP);
        return -1;
    }

    /* Clear all change bits now that the port is enabled. */
    xw(hc->op, reg, (v & ~PS_PLS_MASK) | PS_CHANGE_MASK | PS_PP);

    u32 speed = PS_SPEED(v);
    if (speed != USB_SPEED_LOW && speed != USB_SPEED_FULL &&
        speed != USB_SPEED_HIGH) {
        kprintf("[usb] xhci port %d: unsupported speed %u\n", port, speed);
        return -1;
    }

    xhci_dev_t *d = kzalloc(sizeof(*d));
    if (!d) return -1;

    dev->speed = (u8)speed;
    dev->address = 0;
    dev->ep0_maxp = (speed == USB_SPEED_HIGH) ? 64 : 8;
    d->ep0_maxp = dev->ep0_maxp;

    d->dev_ctx_phys = usb_dma_alloc(2);
    d->in_ctx_phys = usb_dma_alloc(2);
    u64 ep0_phys = usb_dma_alloc(1);
    if (!d->dev_ctx_phys || !d->in_ctx_phys || !ep0_phys) {
        xhci_dev_release(hc, d);
        return -1;
    }
    d->dev_ctx = phys_to_virt(d->dev_ctx_phys);
    d->in_ctx = phys_to_virt(d->in_ctx_phys);
    ring_init(hc, &d->ep0, ep0_phys, XHCI_RING_TRBS);

    /* Enable Slot. */
    d->slot = 0;
    u32 slot_id = 0;
    u64 trb = 0;
    xhci_ring_cmd(hc, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT), &trb);
    if (xhci_wait_cmd(hc, trb, &slot_id) != 0) {
        xhci_dev_release(hc, d);
        return -1;
    }
    d->slot = (u8)slot_id;
    if (d->slot == 0 || d->slot > hc->max_slots || d->slot > XHCI_MAX_SLOTS) {
        kprintf("[usb] xhci: bad slot %u (max %u)\n", (u32)d->slot,
                hc->max_slots);
        xhci_dev_release(hc, d);
        return -1;
    }

    hc->dcbaa[d->slot] = d->dev_ctx_phys;
    hc->slots[d->slot] = d;
    d->ep_id = 3;               /* endpoint 1 IN */

    /* Address Device: slot context plus EP0. */
    u32 csz = ctx_size(hc);
    memset(d->in_ctx, 0, 4096);
    *(u32 *)(d->in_ctx + 4) = 0x3;
    u32 *slot = (u32 *)(d->in_ctx + csz);
    slot[0] = SLOT0_SPEED(dev->speed) | SLOT0_ENTRIES(1);
    slot[1] = SLOT1_PORT(port);
    u32 *ep0 = (u32 *)(d->in_ctx + csz * 2);
    ep0[0] = EP0_INTERVAL(0);
    ep0[1] = EP1_MAXPACKET(dev->ep0_maxp) | EP1_CERR(3) |
             EP1_TYPE(EP_TYPE_CONTROL);
    ep0[2] = (u32)d->ep0.phys | 1;
    ep0[3] = (u32)(d->ep0.phys >> 32);
    ep0[4] = 8;

    trb = 0;
    xhci_ring_cmd(hc, d->in_ctx_phys, 0,
                  TRB_TYPE(TRB_ADDRESS_DEV) | TRB_SLOT(d->slot), &trb);
    if (xhci_wait_cmd(hc, trb, NULL) != 0) {
        xhci_dev_release(hc, d);
        return -1;
    }

    dev->address = d->slot;
    dev->port = (u8)port;
    dev->hcd_priv = d;
    return 0;
}

static int xhci_intr_open(usb_controller_t *c, usb_device_t *dev,
                          u8 ep_addr, u16 maxp, u8 interval) {
    xhci_ctrl_t *hc = (xhci_ctrl_t *)c->priv;
    xhci_dev_t *d = (xhci_dev_t *)dev->hcd_priv;
    if (!d) return -1;

    u32 epnum = ep_addr & 0x0F;
    u32 ep_id = epnum * 2 + 1;
    u32 csz = ctx_size(hc);

    u64 irq_phys = usb_dma_alloc(1);
    u64 buf_phys = usb_dma_alloc(1);
    if (!irq_phys || !buf_phys) {
        /* The two pages are allocated separately; free whichever one was
         * obtained instead of dropping it on the floor. */
        usb_dma_free(irq_phys, 1);
        usb_dma_free(buf_phys, 1);
        return -1;
    }

    ring_init(hc, &d->irq, irq_phys, XHCI_RING_TRBS);
    d->irq_buf = phys_to_virt(buf_phys);
    d->irq_buf_phys = buf_phys;
    d->ep_id = (u8)ep_id;

    u32 ival = interval ? interval : 8;
    if (dev->speed >= USB_SPEED_HIGH) {
        ival = 3;               /* 8 micro-frames */
    } else {
        ival = ival * 8;
        u32 exp = 1;
        while ((1u << exp) < ival && exp < 16) exp++;
        ival = exp;
    }

    memset(d->in_ctx, 0, 4096);
    *(u32 *)(d->in_ctx + 4) = (1u << 0) | (1u << ep_id);
    u32 *slot = (u32 *)(d->in_ctx + csz);
    slot[0] = SLOT0_SPEED(dev->speed) | SLOT0_ENTRIES(ep_id);
    slot[1] = SLOT1_PORT(dev->port);
    u32 *ep = (u32 *)(d->in_ctx + csz * (ep_id + 1));
    ep[0] = EP0_INTERVAL(ival);
    ep[1] = EP1_MAXPACKET(maxp ? maxp : 8) | EP1_CERR(3) |
            EP1_TYPE(EP_TYPE_INT_IN);
    ep[2] = (u32)d->irq.phys | 1;
    ep[3] = (u32)(d->irq.phys >> 32);
    ep[4] = 8;

    u64 trb = 0;
    xhci_ring_cmd(hc, d->in_ctx_phys, 0,
                  TRB_TYPE(TRB_CONFIG_EP) | TRB_SLOT(d->slot), &trb);
    if (xhci_wait_cmd(hc, trb, NULL) != 0) {
        /* CONFIG_EP failed: hand the pages back here and clear the fields so
         * the dev_close() that follows does not free them a second time. The
         * slot itself is disabled by xhci_dev_release(). */
        d->irq.phys = 0;
        d->irq_buf_phys = 0;
        d->irq_buf = NULL;
        usb_dma_free(irq_phys, 1);
        usb_dma_free(buf_phys, 1);
        return -1;
    }

    d->irq_len = maxp ? maxp : 8;
    d->armed = 1;
    xhci_submit_irq(hc, d);
    return 0;
}

static int xhci_intr_poll(usb_controller_t *c, usb_device_t *dev, void *buf,
                          u32 len) {
    xhci_ctrl_t *hc = (xhci_ctrl_t *)c->priv;
    xhci_dev_t *d = (xhci_dev_t *)dev->hcd_priv;
    if (!d) return 0;

    xhci_drain(hc);
    if (!d->pending) return 0;

    int ret = 0;
    if (d->irq_code == CC_SUCCESS || d->irq_code == CC_SHORT_PACKET) {
        u32 n = d->irq_len;
        if (n > len) n = len;
        memcpy(buf, d->irq_buf, n);
        ret = (int)n;
    } else {
        ret = -1;
    }
    d->pending = 0;

    /* Re-arm the periodic transfer now that the buffer was consumed. */
    xhci_submit_irq(hc, d);
    return ret;
}

static const usb_hcd_ops_t g_xhci_ops = {
    .name = "xhci",
    .start = xhci_start,
    .port_count = xhci_port_count,
    .port_connected = xhci_port_connected,
    .dev_open = xhci_dev_open,
    .dev_close = xhci_dev_close,
    .set_address = xhci_set_address,
    .control = xhci_control,
    .intr_open = xhci_intr_open,
    .intr_poll = xhci_intr_poll,
};

/* ---- probe ------------------------------------------------------------ */

/* Take the controller away from the firmware's legacy USB support.
 *
 * xHCI replaced the old SMI-driven USB emulation with an ownership semaphore:
 * the OS sets OS Owned (bit 24) and waits for BIOS Owned (bit 16) to clear.
 * Writing the semaphore alone is not enough, though: the legacy capability's
 * control/status register (USBLEGCTLSTS, +0x04) still has its SMI enables
 * armed, so the firmware's SMM handler keeps getting re-entered as soon as we
 * reset the controller or power its ports. On a 2018 Seewo panel that showed
 * up as a hard freeze right after the controller probe, with every core stuck
 * in SMM; masking those enables is what actually hands the hardware over.
 *
 * Returns 0 when the controller is ours, non-zero when the firmware refuses to
 * let go - the caller must then leave the controller completely untouched. */
static int xhci_legacy_handoff(volatile u8 *cap) {
    u32 hdr = xr(cap, 0x10);
    u32 off = (hdr >> 16) & 0xFFFF;

    /* The xECP chain is a list of capabilities, each pointing at the next one
     * in dwords. Only CAP ID 1 (USB Legacy Support) matters here. */
    for (int guard = 0; guard < 16 && off >= 0x40 && off < 0xFFF0; guard++) {
        volatile u8 *p = cap + off;
        u32 id = xr(p, 0);

        if ((id & 0xFF) == 1) {
            xw(p, 0, id | (1u << 24));
            for (int i = 0; i < 200; i++) {
                if (!(xr(p, 0) & (1u << 16))) break;
                timer_sleep_ms(1);
            }
            if (xr(p, 0) & (1u << 16)) return 1;
            xw(p, 4, 0);        /* mask every legacy SMI enable */
            barrier();
            return 0;
        }
        off = (u32)((id >> 8) & 0xFF) * 4;
    }
    return 0;                   /* no legacy support: nothing to claim */
}

/* Probe only: map the BAR, read the capabilities, take ownership from the
 * firmware and register. Reset, ring setup, RUN and port power all happen in
 * xhci_start(), long after the desktop is on screen. */
static int xhci_init(u8 bus, u8 slot_no, u8 func, u64 bar_phys) {
    xhci_ctrl_t *hc = kzalloc(sizeof(*hc));
    if (!hc) return -1;

    hc->base_phys = bar_phys;
    hc->cap = (volatile u8 *)usb_map_mmio(bar_phys, 0x10000);
    if (!hc->cap) {
        kfree(hc);
        return -1;
    }
    u32 caplen = hc->cap[0];
    if (caplen < 0x40 || caplen > 0x100) {
        kprintf("[usb] xhci %02x:%02x.%u: bad CAPLENGTH %u at %p\n",
                bus, slot_no, func, caplen, (void *)(uptr)bar_phys);
        kfree(hc);
        return -1;
    }

    u32 hcs1 = xr(hc->cap, 0x04);
    u32 hcs2 = xr(hc->cap, 0x08);
    u32 hcc1 = xr(hc->cap, 0x10);
    u32 dboff = xr(hc->cap, 0x14);
    u32 rtsoff = xr(hc->cap, 0x18);

    hc->max_slots = hcs1 & 0xFF;
    hc->ports = (hcs1 >> 24) & 0xFF;
    hc->csz = (hcc1 & (1u << 2)) ? 2 : 1;
    hc->ppc = (hcc1 & (1u << 3)) ? 1 : 0;
    if (hc->ports > 32) hc->ports = 32;

    hc->op = hc->cap + caplen;
    hc->rt = hc->cap + rtsoff;
    hc->db = hc->cap + dboff;

    /* The whole register set has to sit inside the mapped 64 KiB window. */
    if (caplen + 0x400 + 0x10 * hc->ports > 0xFFF0 ||
        rtsoff > 0xFFF0 || dboff > 0xFFF0) {
        kprintf("[usb] xhci: register layout out of range "
                "(caplen %u, rt %u, db %u, %d ports)\n",
                caplen, rtsoff, dboff, hc->ports);
        kfree(hc);
        return -1;
    }

    if (!(hcc1 & 1u)) {         /* 64-bit addressing is mandatory */
        kfree(hc);
        return -1;
    }

    /* Never touch a controller the firmware will not let go of. */
    if (xhci_legacy_handoff(hc->cap) != 0) {
        kprintf("[usb] xhci %02x:%02x.%u: firmware keeps the controller, "
                "leaving it alone\n", bus, slot_no, func);
        usb_diag_note("xhci: firmware keeps it (legacy SMI)");
        kfree(hc);
        return -1;
    }

    u32 slots = hc->max_slots;
    if (slots > 63) slots = 63;
    hc->max_slots = slots;
    hc->sp_count = ((hcs2 >> 21) & 0x1F) | (((hcs2 >> 27) & 0x1F) << 5);

    if (usb_register_controller(&g_xhci_ops, hc, bus, slot_no, func) != 0) {
        kfree(hc);
        return -1;
    }
    return 0;
}

/* Bring the controller to the running state. Called from usb_enum_task() once
 * the desktop is painted: the reset, the ring programming and the port power
 * writes are exactly the operations that wake the firmware's SMM handler, so
 * they must not happen while the boot still owns the screen. */
static int xhci_start(usb_controller_t *c) {
    xhci_ctrl_t *hc = (xhci_ctrl_t *)c->priv;
    if (!hc) return -1;
    if (hc->started) return 0;

    /* Reset and wait for the controller to become ready. */
    xw(hc->op, OP_USBCMD, CMD_HCRST);
    for (int i = 0; i < 300; i++) {
        u32 sts = xr(hc->op, OP_USBSTS);
        if (!(xr(hc->op, OP_USBCMD) & CMD_HCRST) && !(sts & STS_CNR)) break;
        timer_sleep_ms(1);
    }
    if (xr(hc->op, OP_USBSTS) & STS_CNR) {
        kprintf("[usb] xhci: controller not ready after reset\n");
        usb_diag_note("xhci: reset timed out");
        return -1;
    }

    /* Device context array: one entry per slot plus the scratchpad entry. */
    u64 dcbaa_phys = usb_dma_alloc(2);
    if (!dcbaa_phys) return -1;
    hc->dcbaa = (u64 *)phys_to_virt(dcbaa_phys);
    hc->dcbaa_phys = dcbaa_phys;

    /* Scratchpad buffers, if the controller asks for any. */
    u32 sp = hc->sp_count;
    if (sp > 0 && sp <= 64) {
        u64 arr_phys = usb_dma_alloc(1);
        if (arr_phys) {
            u64 *arr = (u64 *)phys_to_virt(arr_phys);
            for (u32 i = 0; i < sp; i++)
                arr[i] = usb_dma_alloc(1);
            hc->dcbaa[0] = arr_phys;
        }
    }

    hc->data_phys = usb_dma_alloc(1);
    u64 cmd_phys = usb_dma_alloc(1);
    u64 ev_phys = usb_dma_alloc(1);
    u64 erst_phys = usb_dma_alloc(1);
    if (!hc->data_phys || !cmd_phys || !ev_phys || !erst_phys) return -1;
    hc->data = phys_to_virt(hc->data_phys);

    ring_init(hc, &hc->cmd, cmd_phys, XHCI_CMD_TRBS);
    hc->ev = (xhci_trb_t *)phys_to_virt(ev_phys);
    hc->ev_phys = ev_phys;
    hc->ev_deq = 0;
    hc->ev_ccs = 1;
    hc->erst = (u32 *)phys_to_virt(erst_phys);
    hc->erst_phys = erst_phys;
    hc->erst[0] = (u32)ev_phys;
    hc->erst[1] = (u32)(ev_phys >> 32);
    hc->erst[2] = XHCI_EVENT_TRBS;
    hc->erst[3] = 0;

    /* Program the controller. */
    xw(hc->op, OP_CONFIG, hc->max_slots);
    xw(hc->op, OP_DCBAAP, (u32)dcbaa_phys);
    xw(hc->op, OP_DCBAAP + 4, (u32)(dcbaa_phys >> 32));

    xw(hc->rt, RT_ERSTSZ, 1);
    /* The segment table base is latched when its high dword is written, so
     * the low dword has to be programmed first. */
    xw(hc->rt, RT_ERSTBA, (u32)erst_phys);
    xw(hc->rt, RT_ERSTBA + 4, (u32)(erst_phys >> 32));
    xw(hc->rt, RT_ERDP, (u32)ev_phys);
    xw(hc->rt, RT_ERDP + 4, (u32)(ev_phys >> 32));

    xw(hc->op, OP_CRCR, ((u32)cmd_phys) | 1u);
    xw(hc->op, OP_CRCR + 4, (u32)(cmd_phys >> 32));

    barrier();
    xw(hc->op, OP_USBCMD, CMD_RUN);

    for (int i = 0; i < 100; i++) {
        if (!(xr(hc->op, OP_USBSTS) & STS_HCH)) break;
        timer_sleep_ms(1);
    }

    /* Power every port. */
    if (hc->ppc) {
        for (int p = 0; p < hc->ports; p++)
            xw(hc->op, OP_PORTSC(p),
               xr(hc->op, OP_PORTSC(p)) | PS_PP);
        timer_sleep_ms(50);
    }

    hc->started = 1;
    kprintf("[usb] xhci %02x:%02x.%u: running, %d port(s)\n", c->bus, c->slot,
            c->func, hc->ports);
    return 0;
}

int usb_xhci_probe(void) {
    int found = 0;

    for (u8 bus = 0; bus < 8; bus++) {
        for (u8 slot = 0; slot < 32; slot++) {
            for (u8 func = 0; func < 8; func++) {
                u32 id = pci_config_read32(bus, slot, func, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) {
                    if (func == 0) break;
                    continue;
                }

                u32 cls = pci_config_read32(bus, slot, func, 0x08);
                if (((cls >> 24) & 0xFF) != 0x0C ||
                    ((cls >> 16) & 0xFF) != 0x03 ||
                    ((cls >> 8) & 0xFF) != 0x30) {
                    continue;
                }

                u32 bar_lo = pci_config_read32(bus, slot, func, 0x10);
                if (bar_lo & 1) continue;           /* I/O BAR, not MMIO */
                u64 bar = (u64)(bar_lo & ~0xFu);
                /* Type bits 2:1 == 0b10 means a 64-bit BAR: the upper half
                 * lives in the next config dword. OVMF routinely parks it
                 * above 4 GiB, where the low half reads as zero. */
                if (((bar_lo >> 1) & 3) == 2)
                    bar |= (u64)pci_config_read32(bus, slot, func, 0x14) << 32;
                if (!bar) continue;

                u32 cmd = pci_config_read32(bus, slot, func, 0x04);
                pci_config_write32(bus, slot, func, 0x04, cmd | 0x06);

                /* xhci_init() maps the BAR, takes the legacy handoff and
                 * registers the controller; nothing is reset here. */
                if (xhci_init(bus, slot, func, bar) == 0)
                    found++;
            }
        }
    }
    return found;
}
