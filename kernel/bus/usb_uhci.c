/* UHCI (USB 1.1) host controller driver.
 *
 * UHCI is the only real option for the USB 1.1 ports of 2000s-era chipsets
 * (PIIX3/PIIX4, ICH0-ICH7): PCI class 0x0C/0x03/0x00, one I/O BAR, a 1024-entry
 * frame list and 16-byte, 16-byte-aligned queue heads / transfer descriptors.
 *
 * Layout of the data structures created here:
 *   - one frame list page; every entry points at the control QH so that both
 *     control traffic and all interrupt QHs are visited each millisecond,
 *   - one control QH whose element is a chain of up to four TDs (setup, data,
 *     status) built per transfer,
 *   - one page per HID interrupt endpoint holding its QH, TD and report
 *     buffer.
 * The QH's last two dwords are software scratch and are left alone; all of the
 * addressing information lives in the TD token, which is why a single QH can
 * serve every device on the bus.
 */

#include <mcube/usb.h>
#include <mcube/kernel.h>
#include <mcube/io.h>
#include <mcube/mm.h>
#include <mcube/pci.h>
#include <mcube/timer.h>

#define UHCI_CMD        0x00
#define UHCI_STS        0x02
#define UHCI_INTR       0x04
#define UHCI_FRNUM      0x06
#define UHCI_FLBASE     0x08
#define UHCI_SOF        0x0C
#define UHCI_PORTSC(n)  (0x10 + (u16)((n) * 2))

#define UHCI_CMD_RS     0x0001
#define UHCI_CMD_HCRESET 0x0002
#define UHCI_CMD_CF     0x0040
#define UHCI_CMD_MAXP   0x0080

#define UHCI_STS_USBINT 0x0001
#define UHCI_STS_ERROR  0x0002
#define UHCI_STS_HCH    0x0020

#define UHCI_PORT_CCS   0x0001
#define UHCI_PORT_CSC   0x0002
#define UHCI_PORT_PE    0x0004
#define UHCI_PORT_PEC   0x0008
#define UHCI_PORT_LSDA  0x0100
#define UHCI_PORT_PR    0x0200

#define UHCI_PTR_TERM   0x0001
#define UHCI_PTR_QH     0x0002
#define UHCI_PTR_DEPTH  0x0004

#define UHCI_TD_ACTIVE  0x00800000
#define UHCI_TD_LS      0x04000000
#define UHCI_TD_CERR    (3u << 27)
#define UHCI_TD_ERR     0x007E0000

#define UHCI_PID_OUT    0xE1
#define UHCI_PID_IN     0x69
#define UHCI_PID_SETUP  0x2D

#define UHCI_FRAMES     1024
#define UHCI_MAX_TDS    4

typedef struct uhci_ep {
    volatile u32 *qh;
    u64 qh_phys;
    volatile u32 *td;
    u64 td_phys;
    u8 *buf;
    u64 buf_phys;
    u16 maxp;
    u8 toggle;
} uhci_ep_t;

typedef struct uhci_dev {
    uhci_ep_t ep;
} uhci_dev_t;

typedef struct uhci_ctrl {
    u16 io;
    volatile u32 *frame;
    u64 frame_phys;
    volatile u32 *cqh;
    u64 cqh_phys;
    volatile u32 *ctd;
    u64 ctd_phys;
    u8 *setup;
    u64 setup_phys;
    u8 *data;
    u64 data_phys;
    int ports;
} uhci_ctrl_t;

#define barrier() __asm__ __volatile__("" ::: "memory")

static const usb_hcd_ops_t g_uhci_ops;

/* ---- data structure helpers ------------------------------------------ */

static void *uhci_page(u64 *phys) {
    *phys = usb_dma_alloc(1);
    return *phys ? phys_to_virt(*phys) : NULL;
}

static void uhci_td_build(volatile u32 *td, u32 token, u32 buffer, u32 ls) {
    td[0] = UHCI_PTR_TERM;
    td[2] = token;
    td[3] = buffer;
    barrier();
    td[1] = UHCI_TD_ACTIVE | UHCI_TD_CERR | ls;
}

/* ---- control transfers ----------------------------------------------- */

static int uhci_control(usb_controller_t *c, usb_device_t *dev,
                        const usb_setup_t *setup, void *buf, u32 len) {
    uhci_ctrl_t *hc = (uhci_ctrl_t *)c->priv;
    u32 devaddr = dev->address & 0x7F;
    u32 ls = (dev->speed == USB_SPEED_LOW) ? UHCI_TD_LS : 0;
    int in = (setup->bmRequestType & 0x80) != 0;
    u32 base = (u32)hc->ctd_phys;
    volatile u32 *td = hc->ctd;
    int idx = 0;

    memcpy(hc->setup, setup, sizeof(usb_setup_t));
    if (len > 4096) len = 4096;
    if (buf && len && !in) memcpy(hc->data, buf, len);

    /* Setup stage: always an 8-byte OUT packet with data toggle 0. */
    uhci_td_build(&td[idx * 4],
                  UHCI_PID_SETUP | (devaddr << 8) | (7u << 21),
                  (u32)hc->setup_phys, ls);
    idx++;

    if (len > 0) {
        u32 done = 0;
        u32 toggle = 1;
        while (done < len && idx < UHCI_MAX_TDS - 1) {
            u32 chunk = len - done;
            if (chunk > 2048) chunk = 2048;
            uhci_td_build(&td[idx * 4],
                          (in ? UHCI_PID_IN : UHCI_PID_OUT) | (devaddr << 8) |
                          ((chunk - 1) << 21) | (toggle << 19),
                          (u32)(hc->data_phys + done), ls);
            toggle ^= 1;
            done += chunk;
            idx++;
        }
    }

    /* Status stage: opposite direction, zero length. */
    uhci_td_build(&td[idx * 4],
                  (in ? UHCI_PID_OUT : UHCI_PID_IN) | (devaddr << 8) |
                  (0x7FFu << 21), 0, ls);
    idx++;

    for (int i = 0; i < idx - 1; i++)
        td[i * 4] = (u32)(base + (u32)(i + 1) * 16) | UHCI_PTR_DEPTH;
    barrier();

    /* Publish the chain and let the frame list pick it up. */
    hc->cqh[1] = base;
    barrier();

    volatile u32 *last = &td[(idx - 1) * 4];
    u32 st = UHCI_TD_ACTIVE;
    for (int ms = 0; ms < 500; ms++) {
        st = last[1];
        if (!(st & UHCI_TD_ACTIVE)) break;
        timer_sleep_ms(1);
    }

    /* Detach the queue so a stale TD is never retried. */
    hc->cqh[1] = UHCI_PTR_TERM;
    barrier();

    if (st & UHCI_TD_ACTIVE) {
        kprintf("[usb] uhci control timeout (dev %u, req %u)\n",
                (u32)dev->address, (u32)setup->bRequest);
        return -1;
    }
    if (st & UHCI_TD_ERR) {
        kprintf("[usb] uhci control error 0x%08x (dev %u, req %u)\n", st,
                (u32)dev->address, (u32)setup->bRequest);
        return -1;
    }
    if (in && buf && len) memcpy(buf, hc->data, len);
    return 0;
}

/* ---- controller / port operations ------------------------------------ */

static int uhci_port_count(usb_controller_t *c) {
    return ((uhci_ctrl_t *)c->priv)->ports;
}

static int uhci_port_connected(usb_controller_t *c, int port) {
    uhci_ctrl_t *hc = (uhci_ctrl_t *)c->priv;
    return (inw(hc->io + UHCI_PORTSC(port - 1)) & UHCI_PORT_CCS) != 0;
}

static int uhci_set_address(usb_controller_t *c, usb_device_t *dev,
                            u8 address) {
    usb_setup_t setup;
    memset(&setup, 0, sizeof(setup));
    setup.bmRequestType = 0x00;
    setup.bRequest = USB_REQ_SET_ADDRESS;
    setup.wValue = address;

    if (uhci_control(c, dev, &setup, NULL, 0) != 0) return -1;
    timer_sleep_ms(2);
    dev->address = address;
    return 0;
}

static int uhci_dev_open(usb_controller_t *c, int port, usb_device_t *dev) {
    uhci_ctrl_t *hc = (uhci_ctrl_t *)c->priv;
    u16 reg = hc->io + UHCI_PORTSC(port - 1);
    u16 v = inw(reg);

    if (!(v & UHCI_PORT_CCS)) return -1;

    outw(reg, UHCI_PORT_PR);
    timer_sleep_ms(60);
    outw(reg, 0);
    timer_sleep_ms(20);

    v = inw(reg);
    if (!(v & UHCI_PORT_PE)) {
        outw(reg, UHCI_PORT_PE);
        timer_sleep_ms(10);
        v = inw(reg);
    }
    if (!(v & UHCI_PORT_PE)) return -1;

    dev->speed = (v & UHCI_PORT_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL;
    dev->address = 0;
    dev->ep0_maxp = 8;      /* low speed default until the descriptor is read */
    return 0;
}

static int uhci_intr_open(usb_controller_t *c, usb_device_t *dev,
                          u8 ep_addr, u16 maxp, u8 interval) {
    UNUSED(interval);
    uhci_ctrl_t *hc = (uhci_ctrl_t *)c->priv;
    u64 phys = 0;
    u8 *page = uhci_page(&phys);
    uhci_dev_t *ud = kzalloc(sizeof(*ud));
    if (!page || !ud) {
        kfree(ud);
        return -1;
    }

    uhci_ep_t *ep = &ud->ep;
    ep->qh = (volatile u32 *)page;
    ep->qh_phys = phys;
    ep->td = (volatile u32 *)(page + 0x10);
    ep->td_phys = phys + 0x10;
    ep->buf = page + 0x40;
    ep->buf_phys = phys + 0x40;
    ep->maxp = maxp ? maxp : 8;
    ep->toggle = 0;

    u32 epnum = ep_addr & 0x0F;
    u32 devaddr = dev->address & 0x7F;
    u32 ls = (dev->speed == USB_SPEED_LOW) ? UHCI_TD_LS : 0;

    ep->qh[0] = UHCI_PTR_TERM;
    ep->qh[1] = (u32)ep->td_phys;

    uhci_td_build(ep->td,
                  UHCI_PID_IN | (devaddr << 8) | (epnum << 15) |
                  ((ep->maxp - 1) << 21),
                  (u32)ep->buf_phys, ls);

    /* Splice the new QH in at the head of the chain behind the control QH. */
    ep->qh[0] = hc->cqh[0];
    barrier();
    hc->cqh[0] = (u32)ep->qh_phys | UHCI_PTR_QH;

    dev->hcd_priv = ud;
    return 0;
}

static int uhci_intr_poll(usb_controller_t *c, usb_device_t *dev, void *buf,
                          u32 len) {
    UNUSED(c);
    uhci_dev_t *ud = (uhci_dev_t *)dev->hcd_priv;
    if (!ud) return 0;

    uhci_ep_t *ep = &ud->ep;
    u32 st = ep->td[1];
    if (st & UHCI_TD_ACTIVE) return 0;

    int ret = 0;
    if (!(st & UHCI_TD_ERR)) {
        /* The status field holds the number of bytes transferred; a full
         * packet is encoded as zero on some implementations. */
        u32 actual = st & 0x7FF;
        if (actual == 0) actual = ep->maxp;
        if (actual > ep->maxp) actual = ep->maxp;
        if (actual > len) actual = len;
        if (actual > 0) {
            memcpy(buf, ep->buf, actual);
            ret = (int)actual;
        }
    } else {
        ret = -1;
    }

    /* Re-arm the same TD; the toggle flips on every completed packet. */
    ep->toggle ^= 1;
    u32 epnum = dev->ep_in & 0x0F;
    u32 devaddr = dev->address & 0x7F;
    u32 ls = (dev->speed == USB_SPEED_LOW) ? UHCI_TD_LS : 0;
    uhci_td_build(ep->td,
                  UHCI_PID_IN | (devaddr << 8) | (epnum << 15) |
                  (ep->toggle << 19) | ((ep->maxp - 1) << 21),
                  (u32)ep->buf_phys, ls);
    /* The controller advanced the QH element pointer past the finished TD,
     * so it has to be pointed at the re-armed one again. */
    barrier();
    ep->qh[1] = (u32)ep->td_phys;
    barrier();
    return ret;
}

static const usb_hcd_ops_t g_uhci_ops = {
    .name = "uhci",
    .port_count = uhci_port_count,
    .port_connected = uhci_port_connected,
    .dev_open = uhci_dev_open,
    .set_address = uhci_set_address,
    .control = uhci_control,
    .intr_open = uhci_intr_open,
    .intr_poll = uhci_intr_poll,
};

/* ---- probe ------------------------------------------------------------ */

static int uhci_init(u8 bus, u8 slot, u8 func, u16 io) {
    uhci_ctrl_t *hc = kzalloc(sizeof(*hc));
    if (!hc) return -1;
    hc->io = io;

    /* Controller reset. */
    outw(io + UHCI_CMD, UHCI_CMD_HCRESET);
    for (int i = 0; i < 100; i++) {
        if (!(inw(io + UHCI_CMD) & UHCI_CMD_HCRESET)) break;
        timer_sleep_ms(1);
    }
    outw(io + UHCI_CMD, 0);

    hc->frame = uhci_page(&hc->frame_phys);
    u64 qphys = 0;
    u8 *qpage = uhci_page(&qphys);
    u64 dphys = 0;
    u8 *dpage = uhci_page(&dphys);
    if (!hc->frame || !qpage || !dpage) {
        kfree(hc);
        return -1;
    }

    hc->cqh = (volatile u32 *)qpage;
    hc->cqh_phys = qphys;
    hc->ctd = (volatile u32 *)(qpage + 0x10);
    hc->ctd_phys = qphys + 0x10;
    hc->setup = qpage + 0x100;
    hc->setup_phys = qphys + 0x100;
    hc->data = dpage;
    hc->data_phys = dphys;

    hc->cqh[0] = UHCI_PTR_TERM;
    hc->cqh[1] = UHCI_PTR_TERM;

    for (int i = 0; i < UHCI_FRAMES; i++)
        hc->frame[i] = (u32)hc->cqh_phys | UHCI_PTR_QH;

    for (int i = 0; i < 8; i++) {
        u16 v = inw(io + UHCI_PORTSC(i));
        if (v == 0xFFFF || !(v & 0x0080)) break;
        hc->ports++;
    }
    if (hc->ports == 0) hc->ports = 2;

    outw(io + UHCI_FRNUM, 0);
    outw(io + UHCI_FLBASE, (u16)(hc->frame_phys & ~0xFFFu));
    outw(io + UHCI_FLBASE + 2, (u16)(hc->frame_phys >> 16));
    outw(io + UHCI_SOF, 0x40);
    outw(io + UHCI_INTR, 0);
    outw(io + UHCI_STS, 0x003F);
    barrier();
    outw(io + UHCI_CMD, UHCI_CMD_RS | UHCI_CMD_CF | UHCI_CMD_MAXP);

    if (usb_register_controller(&g_uhci_ops, hc, bus, slot, func) != 0) {
        kfree(hc);
        return -1;
    }
    return 0;
}

int usb_uhci_probe(void) {
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
                    ((cls >> 8) & 0xFF) != 0x00) {
                    continue;
                }

                /* UHCI puts its I/O BAR in BAR4 on PIIX3/PIIX4 and the ICH
                 * family; some implementations use BAR0. */
                u32 bar = pci_config_read32(bus, slot, func, 0x20);
                if (!(bar & 1))
                    bar = pci_config_read32(bus, slot, func, 0x10);
                if (!(bar & 1)) continue;

                u32 cmd = pci_config_read32(bus, slot, func, 0x04);
                pci_config_write32(bus, slot, func, 0x04, cmd | 0x05);

                /* Hand the ports to the OS and drop legacy SMI ownership. */
                u32 leg = pci_config_read32(bus, slot, func, 0xC0);
                pci_config_write32(bus, slot, func, 0xC0,
                                   (leg & ~0x8F00u) | 0x2000u);

                if (uhci_init(bus, slot, func, (u16)(bar & ~3u)) == 0)
                    found++;
            }
        }
    }
    return found;
}
