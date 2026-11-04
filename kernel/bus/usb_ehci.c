/* EHCI (USB 2.0) host controller driver.
 *
 * Used by chipsets from roughly 2002-2012 (PCI class 0x0C/0x03/0x20, memory
 * mapped BAR0). Only high-speed devices are handled here; a full/low-speed
 * device stays owned by the companion UHCI/OHCI controller, which is exactly
 * what the hardware does when the reset does not enable the port.
 *
 * Data structures: one asynchronous QH ring (the control QH points at itself),
 * a 1024-entry periodic frame list where every entry leads to the interrupt
 * QHs, and per-transfer qTD chains of at most three entries. qTD buffers must
 * not cross a 4 KiB boundary, so control payloads are bounced through a page
 * of DMA memory.
 */

#include <mcube/usb.h>
#include <mcube/kernel.h>
#include <mcube/mm.h>
#include <mcube/pci.h>
#include <mcube/timer.h>

#define EHCI_USBCMD     0x00
#define EHCI_USBSTS     0x04
#define EHCI_USBINTR    0x08
#define EHCI_FRINDEX    0x0C
#define EHCI_CTRLDSSEG  0x10
#define EHCI_PERIODIC   0x14
#define EHCI_ASYNC      0x18
#define EHCI_CONFIGFLAG 0x40
#define EHCI_PORTSC(n)  (0x44 + 4 * (n))

#define EHCI_CMD_RS     0x00000001
#define EHCI_CMD_HCRESET 0x00000002
#define EHCI_CMD_PSE    0x00000010
#define EHCI_CMD_ASE    0x00000020
#define EHCI_CMD_ITC(x) ((u32)(x) << 16)

#define EHCI_STS_USBINT 0x00000001
#define EHCI_STS_PCD    0x00000004
#define EHCI_STS_HCH    0x00001000
#define EHCI_STS_HSE    0x00004000

#define EHCI_PORT_CCS       0x00000001
#define EHCI_PORT_CSC       0x00000002
#define EHCI_PORT_PED       0x00000004
#define EHCI_PORT_PEC       0x00000008
#define EHCI_PORT_PR        0x00000100
#define EHCI_PORT_PP        0x00001000
#define EHCI_PORT_OWNER     0x00002000

#define QTD_TOGGLE      0x80000000
#define QTD_IOC         0x00008000
#define QTD_ACTIVE      0x00000080
#define QTD_HALTED      0x00000040
#define QTD_ERR_MASK    0x00000078
#define QTD_PID_OUT     0x00000000
#define QTD_PID_IN      0x00000100
#define QTD_PID_SETUP   0x00000200
#define QTD_CERR3       0x00000C00

#define QH_DTC          0x00004000
#define QH_H            0x00008000
#define QH_CONTROL      0x08000000
#define QH_MULT1        0x40000000

#define EHCI_FRAMES     1024
#define EHCI_MAX_QTD    3

typedef struct ehci_ep {
    volatile u32 *qh;
    u64 qh_phys;
    volatile u32 *td;
    u64 td_phys;
    u8 *buf;
    u64 buf_phys;
    u16 maxp;
} ehci_ep_t;

typedef struct ehci_dev {
    ehci_ep_t ep;
} ehci_dev_t;

typedef struct ehci_ctrl {
    volatile u8 *cap;
    volatile u32 *op;
    u64 base_phys;
    int ports;
    int ppc;                /* port power control supported */
    int started;
    volatile u32 *aqh;
    u64 aqh_phys;
    volatile u32 *ctd;
    u64 ctd_phys;
    u8 *setup;
    u64 setup_phys;
    u8 *data;
    u64 data_phys;
    volatile u32 *periodic;
    u64 periodic_phys;
    u64 per_head;           /* first QH of the periodic chain, 0 when empty */
} ehci_ctrl_t;

#define barrier() __asm__ __volatile__("" ::: "memory")

static const usb_hcd_ops_t g_ehci_ops;
static int ehci_start(usb_controller_t *c);

static inline u32 ehci_read(ehci_ctrl_t *hc, u32 off) {
    return hc->op[off / 4];
}

static inline void ehci_write(ehci_ctrl_t *hc, u32 off, u32 val) {
    hc->op[off / 4] = val;
}

/* ---- transfer descriptors -------------------------------------------- */

static void ehci_td_build(volatile u32 *td, u32 token, u64 buf_phys) {
    td[0] = 1;
    td[1] = 1;
    td[2] = token;
    td[3] = (u32)buf_phys;
    td[4] = 0;
    td[5] = 0;
    td[6] = 0;
    td[7] = 0;
}

/* ---- control transfers ------------------------------------------------ */

static int ehci_control(usb_controller_t *c, usb_device_t *dev,
                        const usb_setup_t *setup, void *buf, u32 len) {
    ehci_ctrl_t *hc = (ehci_ctrl_t *)c->priv;
    u32 devaddr = dev->address & 0x7F;
    u32 maxp = dev->ep0_maxp ? dev->ep0_maxp : 64;
    int in = (setup->bmRequestType & 0x80) != 0;
    u32 base = (u32)hc->ctd_phys;
    volatile u32 *td = hc->ctd;
    int idx = 0;

    if (len > 2048) len = 2048;
    memcpy(hc->setup, setup, sizeof(usb_setup_t));
    if (buf && len) memcpy(hc->data, buf, len);

    ehci_td_build(&td[idx * 8],
                  QTD_ACTIVE | QTD_CERR3 | QTD_PID_SETUP | (8u << 16),
                  hc->setup_phys);
    idx++;

    if (len > 0) {
        ehci_td_build(&td[idx * 8],
                      QTD_ACTIVE | QTD_CERR3 | QTD_TOGGLE |
                      (in ? QTD_PID_IN : QTD_PID_OUT) | (len << 16),
                      hc->data_phys);
        idx++;
    }

    ehci_td_build(&td[idx * 8],
                  QTD_ACTIVE | QTD_CERR3 | QTD_TOGGLE |
                  (in ? QTD_PID_OUT : QTD_PID_IN),
                  0);
    idx++;

    for (int i = 0; i < idx - 1; i++)
        td[i * 8] = (u32)(base + (u32)(i + 1) * 32);
    barrier();

    /* Control QH: DTC=1 so every transfer starts with DATA0 at setup. */
    hc->aqh[1] = QH_CONTROL | QH_DTC | QH_H | (maxp << 16) | (2u << 12) |
                 devaddr;
    hc->aqh[2] = QH_MULT1 | 0x01;
    hc->aqh[5] = 1;
    hc->aqh[6] = 0;
    barrier();
    hc->aqh[4] = base;
    barrier();

    volatile u32 *last = &td[(idx - 1) * 8];
    u32 tok = QTD_ACTIVE;
    for (int ms = 0; ms < 500; ms++) {
        tok = last[2];
        if (!(tok & QTD_ACTIVE)) break;
        timer_sleep_ms(1);
    }

    /* Terminate the queue so a stale chain cannot be retried. */
    hc->aqh[4] = 1;
    barrier();

    if (tok & QTD_ACTIVE) {
        kprintf("[usb] ehci control timeout (dev %u, req %u)\n",
                (u32)dev->address, (u32)setup->bRequest);
        return -1;
    }
    if (tok & QTD_ERR_MASK) {
        kprintf("[usb] ehci control error 0x%08x (dev %u, req %u)\n", tok,
                (u32)dev->address, (u32)setup->bRequest);
        return -1;
    }

    if (in && buf && len) memcpy(buf, hc->data, len);
    return 0;
}

/* ---- controller / port operations ------------------------------------ */

static int ehci_port_count(usb_controller_t *c) {
    return ((ehci_ctrl_t *)c->priv)->ports;
}

static int ehci_port_connected(usb_controller_t *c, int port) {
    ehci_ctrl_t *hc = (ehci_ctrl_t *)c->priv;
    u32 v = ehci_read(hc, EHCI_PORTSC(port - 1));
    return (v & (EHCI_PORT_CCS | EHCI_PORT_OWNER)) == EHCI_PORT_CCS;
}

static int ehci_dev_open(usb_controller_t *c, int port, usb_device_t *dev) {
    ehci_ctrl_t *hc = (ehci_ctrl_t *)c->priv;
    u32 reg = EHCI_PORTSC(port - 1);
    u32 v = ehci_read(hc, reg);

    if (!(v & EHCI_PORT_CCS) || (v & EHCI_PORT_OWNER)) return -1;

    ehci_write(hc, reg, (v & EHCI_PORT_PP) | EHCI_PORT_PR);
    timer_sleep_ms(60);

    v = ehci_read(hc, reg);
    if (v & EHCI_PORT_OWNER) return -1;       /* handed to the companion */

    /* Clear the reset and the change bits, keep power on. */
    ehci_write(hc, reg, (v & EHCI_PORT_PP) | EHCI_PORT_CSC | EHCI_PORT_PEC);
    timer_sleep_ms(20);

    v = ehci_read(hc, reg);
    if (!(v & EHCI_PORT_PED) || (v & EHCI_PORT_OWNER)) {
        ehci_write(hc, reg, (v & EHCI_PORT_PP) | EHCI_PORT_CSC |
                            EHCI_PORT_PEC);
        return -1;
    }

    dev->speed = USB_SPEED_HIGH;
    dev->address = 0;
    dev->ep0_maxp = 64;
    return 0;
}

static int ehci_set_address(usb_controller_t *c, usb_device_t *dev,
                            u8 address) {
    usb_setup_t setup;
    memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = 0x00;
    setup.bRequest = USB_REQ_SET_ADDRESS;
    setup.wValue = address;

    if (ehci_control(c, dev, &setup, NULL, 0) != 0) return -1;
    timer_sleep_ms(2);
    dev->address = address;
    return 0;
}

static int ehci_intr_open(usb_controller_t *c, usb_device_t *dev,
                          u8 ep_addr, u16 maxp, u8 interval) {
    ehci_ctrl_t *hc = (ehci_ctrl_t *)c->priv;
    u64 phys = 0;
    u8 *page = NULL;
    phys = usb_dma_alloc(1);
    if (!phys) return -1;
    page = phys_to_virt(phys);
    memset(page, 0, 4096);

    ehci_dev_t *ud = kzalloc(sizeof(*ud));
    if (!ud) return -1;

    ehci_ep_t *ep = &ud->ep;
    ep->qh = (volatile u32 *)page;
    ep->qh_phys = phys;
    ep->td = (volatile u32 *)(page + 0x40);
    ep->td_phys = phys + 0x40;
    ep->buf = page + 0x80;
    ep->buf_phys = phys + 0x80;
    ep->maxp = maxp ? maxp : 8;
    UNUSED(interval);

    u32 epnum = ep_addr & 0x0F;
    u32 devaddr = dev->address & 0x7F;

    ep->qh[0] = 1;
    ep->qh[1] = (ep->maxp << 16) | (epnum << 8) | (2u << 12) | devaddr;
    ep->qh[2] = QH_MULT1 | 0x01;
    ep->qh[3] = 0;
    ep->qh[5] = 1;
    ep->qh[6] = 0;
    ehci_td_build(ep->td, QTD_ACTIVE | QTD_CERR3 | QTD_PID_IN |
                  (ep->maxp << 16), ep->buf_phys);
    barrier();
    ep->qh[4] = (u32)ep->td_phys;

    /* Insert at the head of the periodic chain seen by every micro-frame. */
    ep->qh[0] = hc->per_head ? (u32)hc->per_head | 2 : 1;
    barrier();
    hc->per_head = ep->qh_phys;
    for (int i = 0; i < EHCI_FRAMES; i++)
        hc->periodic[i] = (u32)ep->qh_phys | 2;

    dev->hcd_priv = ud;
    return 0;
}

static int ehci_intr_poll(usb_controller_t *c, usb_device_t *dev, void *buf,
                          u32 len) {
    UNUSED(c);
    ehci_dev_t *ud = (ehci_dev_t *)dev->hcd_priv;
    if (!ud) return 0;

    ehci_ep_t *ep = &ud->ep;
    u32 tok = ep->td[2];
    if (tok & QTD_ACTIVE) return 0;

    int ret = 0;
    if (tok & QTD_ERR_MASK) {
        ret = -1;
    } else {
        u32 actual = len;
        if (actual > ep->maxp) actual = ep->maxp;
        memcpy(buf, ep->buf, actual);
        ret = (int)actual;
    }

    ehci_td_build(ep->td, QTD_ACTIVE | QTD_CERR3 | QTD_PID_IN |
                  (ep->maxp << 16), ep->buf_phys);
    barrier();
    ep->qh[4] = (u32)ep->td_phys;
    barrier();
    return ret;
}

static const usb_hcd_ops_t g_ehci_ops = {
    .name = "ehci",
    .start = ehci_start,
    .port_count = ehci_port_count,
    .port_connected = ehci_port_connected,
    .dev_open = ehci_dev_open,
    .set_address = ehci_set_address,
    .control = ehci_control,
    .intr_open = ehci_intr_open,
    .intr_poll = ehci_intr_poll,
};

/* ---- probe ------------------------------------------------------------ */

/* Probe only reads the capability registers. Everything that touches the
 * controller's own state (reset, CONFIGFLAG, port power, the async/periodic
 * rings) happens in ehci_start(), which usb_enum_task() calls once the desktop
 * is up. A firmware that keeps the controller busy then costs input, not the
 * whole boot. */
static int ehci_init(u8 bus, u8 slot, u8 func, u64 bar_phys) {
    ehci_ctrl_t *hc = kzalloc(sizeof(*hc));
    if (!hc) return -1;

    hc->base_phys = bar_phys;
    /* The loader only builds the identity window below 64 GiB; any BAR parked
     * above it has to be mapped on demand. */
    hc->cap = (volatile u8 *)usb_map_mmio(bar_phys, 0x10000);
    if (!hc->cap) {
        kfree(hc);
        return -1;
    }
    u32 caplen = hc->cap[0];
    if (caplen < 0x20 || caplen > 0x100) {
        kprintf("[usb] ehci %02x:%02x.%u: bad CAPLENGTH %u at %p\n",
                bus, slot, func, caplen, (void *)(uptr)bar_phys);
        kfree(hc);
        return -1;
    }
    hc->op = (volatile u32 *)((volatile u8 *)hc->cap + caplen);

    u32 hcs = hc->cap[4] | ((u32)hc->cap[5] << 8) |
              ((u32)hc->cap[6] << 16) | ((u32)hc->cap[7] << 24);
    hc->ports = hcs & 0x0F;
    hc->ppc = (hcs >> 4) & 0x01;
    if (hc->ports < 1) hc->ports = 1;
    if (hc->ports > 8) hc->ports = 8;
    hc->started = 0;

    if (usb_register_controller(&g_ehci_ops, hc, bus, slot, func) != 0) {
        kfree(hc);
        return -1;
    }
    return 0;
}

/* Reset and run the controller. Called once, in usb_enum_task() context. */
static int ehci_start(usb_controller_t *c) {
    ehci_ctrl_t *hc = (ehci_ctrl_t *)c->priv;
    if (!hc) return -1;
    if (hc->started) return 0;

    /* Reset; the controller clears HCRESET when it is ready. */
    ehci_write(hc, EHCI_USBCMD, EHCI_CMD_HCRESET);
    int ok = 0;
    for (int i = 0; i < 200; i++) {
        if (!(ehci_read(hc, EHCI_USBCMD) & EHCI_CMD_HCRESET)) {
            ok = 1;
            break;
        }
        timer_sleep_ms(1);
    }
    if (!ok) {
        kprintf("[usb] ehci: reset never completed\n");
        usb_diag_note("ehci: reset timed out");
        return -1;
    }
    ehci_write(hc, EHCI_USBCMD, EHCI_CMD_ITC(8));
    ehci_write(hc, EHCI_CTRLDSSEG, 0);

    /* Route all ports to this controller. */
    ehci_write(hc, EHCI_CONFIGFLAG, 1);

    for (int p = 0; p < hc->ports; p++) {
        if (hc->ppc) ehci_write(hc, EHCI_PORTSC(p),
                                ehci_read(hc, EHCI_PORTSC(p)) | EHCI_PORT_PP);
    }
    if (hc->ppc) timer_sleep_ms(20);

    u64 qphys = usb_dma_alloc(1);
    u64 tphys = usb_dma_alloc(1);
    u64 lphys = usb_dma_alloc(1);
    u64 dphys = usb_dma_alloc(1);
    if (!qphys || !tphys || !lphys || !dphys) {
        if (qphys) pmm_free_page(qphys);
        if (tphys) pmm_free_page(tphys);
        if (lphys) pmm_free_page(lphys);
        if (dphys) pmm_free_page(dphys);
        usb_diag_note("ehci: out of DMA memory");
        return -1;
    }

    hc->aqh = (volatile u32 *)phys_to_virt(qphys);
    hc->aqh_phys = qphys;
    hc->ctd = (volatile u32 *)phys_to_virt(tphys);
    hc->ctd_phys = tphys;
    hc->setup = (u8 *)hc->ctd + 0x100;
    hc->setup_phys = tphys + 0x100;
    hc->data = (u8 *)phys_to_virt(dphys);
    hc->data_phys = dphys;
    hc->periodic = (volatile u32 *)phys_to_virt(lphys);
    hc->periodic_phys = lphys;

    /* Async ring: the single control QH links to itself, head-of-list set. */
    hc->aqh[0] = (u32)qphys | 2;
    hc->aqh[1] = QH_CONTROL | QH_DTC | QH_H | (64u << 16) | (2u << 12);
    hc->aqh[2] = QH_MULT1 | 0x01;
    hc->aqh[3] = 0;
    hc->aqh[4] = 1;
    hc->aqh[5] = 1;
    hc->aqh[6] = 0;

    for (int i = 0; i < EHCI_FRAMES; i++)
        hc->periodic[i] = 1;

    barrier();
    ehci_write(hc, EHCI_PERIODIC, (u32)lphys);
    ehci_write(hc, EHCI_ASYNC, (u32)qphys);
    ehci_write(hc, EHCI_USBINTR, 0);
    ehci_write(hc, EHCI_USBSTS, 0x3F);
    barrier();
    ehci_write(hc, EHCI_USBCMD, EHCI_CMD_RS | EHCI_CMD_PSE | EHCI_CMD_ASE |
                                EHCI_CMD_ITC(8));

    int running = 0;
    for (int i = 0; i < 100; i++) {
        if (!(ehci_read(hc, EHCI_USBSTS) & EHCI_STS_HCH)) {
            running = 1;
            break;
        }
        timer_sleep_ms(1);
    }
    if (!running) {
        kprintf("[usb] ehci: halted after start\n");
        usb_diag_note("ehci: halted after run");
        return -1;
    }

    hc->started = 1;
    return 0;
}

int usb_ehci_probe(void) {
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
                    ((cls >> 8) & 0xFF) != 0x20) {
                    continue;
                }

                u32 bar_lo = pci_config_read32(bus, slot, func, 0x10);
                if (bar_lo & 1) continue;           /* I/O BAR, not MMIO */
                u64 bar = (u64)(bar_lo & ~0xFu);
                /* Type bits 2:1 == 0b10 means a 64-bit BAR: its upper half
                 * lives in the next config dword. Firmware is free to park it
                 * above 4 GiB, where the low half alone reads as zero. */
                if (((bar_lo >> 1) & 3) == 2)
                    bar |= (u64)pci_config_read32(bus, slot, func, 0x14) << 32;
                if (!bar) continue;

                u32 cmd = pci_config_read32(bus, slot, func, 0x04);
                pci_config_write32(bus, slot, func, 0x04, cmd | 0x06);

                if (ehci_init(bus, slot, func, bar) == 0)
                    found++;
            }
        }
    }
    return found;
}
