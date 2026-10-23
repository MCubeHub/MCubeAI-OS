/* Intel 8254x (e1000) driver, polled.
 *
 * QEMU's default e1000 is an 82540EM. We keep interrupts disabled and drain
 * the RX ring from net_poll(), which keeps the network stack independent of
 * PCI interrupt routing (ACPI _PRT parsing arrives with the APIC work).
 */

#include <mcube/net.h>
#include <mcube/pci.h>
#include <mcube/io.h>
#include <mcube/mm.h>
#include <mcube/kernel.h>
#include <mcube/timer.h>
#include <mcube/atomic.h>

#define E1000_CTRL   0x0000
#define E1000_STATUS 0x0008
#define E1000_EERD   0x0014
#define E1000_ICR    0x00C0
#define E1000_IMS    0x00D0
#define E1000_RCTL   0x0100
#define E1000_TCTL   0x0400
#define E1000_TIPG   0x0410
#define E1000_RDBAL  0x2800
#define E1000_RDBAH  0x2804
#define E1000_RDLEN  0x2808
#define E1000_RDH    0x2810
#define E1000_RDT    0x2818
#define E1000_TDBAL  0x3800
#define E1000_TDBAH  0x3804
#define E1000_TDLEN  0x3808
#define E1000_TDH    0x3810
#define E1000_TDT    0x3818
#define E1000_MTA    0x5200
#define E1000_RAL    0x5400
#define E1000_RAH    0x5404

#define CTRL_RST      (1u << 26)
#define CTRL_SLU      (1u << 6)

#define RCTL_EN       (1u << 1)
#define RCTL_UPE      (1u << 3)
#define RCTL_MPE      (1u << 4)
#define RCTL_BAM      (1u << 15)
#define RCTL_SECRC    (1u << 26)

#define TCTL_EN       (1u << 1)
#define TCTL_PSP      (1u << 3)
#define TCTL_CT       (0x10u << 4)
#define TCTL_COLD     (0x40u << 12)

#define TX_CMD_EOP    (1u << 0)
#define TX_CMD_IFCS   (1u << 1)
#define TX_CMD_RS     (1u << 3)
#define TX_STATUS_DD  (1u << 0)
#define RX_STATUS_DD  (1u << 0)

#define RING_SIZE 32

typedef struct PACKED e1000_rx_desc {
    u64 addr;
    u16 length;
    u16 checksum;
    u8  status;
    u8  errors;
    u16 special;
} e1000_rx_desc_t;

typedef struct PACKED e1000_tx_desc {
    u64 addr;
    u16 length;
    u8  cso;
    u8  cmd;
    u8  status;
    u8  css;
    u16 special;
} e1000_tx_desc_t;

static volatile u32 *g_mmio;
static e1000_rx_desc_t *g_rx_ring;
static e1000_tx_desc_t *g_tx_ring;
static u8 *g_rx_buf[RING_SIZE];
static u8 *g_tx_buf[RING_SIZE];
static u32 g_rx_tail;
static u32 g_tx_tail;
static net_device_t g_dev;

static u32 reg_read(u32 off) {
    return g_mmio[off / 4];
}

static void reg_write(u32 off, u32 value) {
    g_mmio[off / 4] = value;
}

static int eeprom_read(u8 addr, u16 *out) {
    reg_write(E1000_EERD, ((u32)addr << 8) | 1u);
    for (int i = 0; i < 100000; i++) {
        u32 v = reg_read(E1000_EERD);
        if (v & (1u << 4)) {
            *out = (u16)(v >> 16);
            return 0;
        }
    }
    return -1;
}

static int e1000_send(const void *frame, u32 len) {
    if (!g_tx_ring || len == 0 || len > 1514) return -1;

    u32 idx = g_tx_tail;
    memcpy(g_tx_buf[idx], frame, len);

    e1000_tx_desc_t *d = &g_tx_ring[idx];
    d->addr = virt_to_phys(g_tx_buf[idx]);
    d->length = (u16)len;
    d->cso = 0;
    d->cmd = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
    d->status = 0;
    d->css = 0;
    d->special = 0;

    g_tx_tail = (idx + 1) % RING_SIZE;
    reg_write(E1000_TDT, g_tx_tail);

    volatile e1000_tx_desc_t *vd = d;
    for (int i = 0; i < 2000000; i++) {
        if (vd->status & TX_STATUS_DD) {
            d->status = 0;
            return 0;
        }
        __asm__ __volatile__("pause" ::: "memory");
    }
    kprintf("[e1000] tx timeout status=%x tdh=%u tdt=%u\n",
            vd->status, reg_read(E1000_TDH), reg_read(E1000_TDT));
    return -1;
}

static void e1000_poll(void) {
    if (!g_rx_ring) return;

    for (int budget = 0; budget < RING_SIZE; budget++) {
        e1000_rx_desc_t *d = &g_rx_ring[g_rx_tail];
        if (!(d->status & RX_STATUS_DD)) break;

        u32 len = d->length;
        if (len >= 14 && len <= 2048) net_receive(g_rx_buf[g_rx_tail], len);

        d->status = 0;
        reg_write(E1000_RDT, g_rx_tail);
        g_rx_tail = (g_rx_tail + 1) % RING_SIZE;
    }
}

static int e1000_setup_rings(void) {
    u64 rx_ring_phys = pmm_alloc_page();
    u64 tx_ring_phys = pmm_alloc_page();
    if (!rx_ring_phys || !tx_ring_phys) return -1;

    g_rx_ring = (e1000_rx_desc_t *)phys_to_virt(rx_ring_phys);
    g_tx_ring = (e1000_tx_desc_t *)phys_to_virt(tx_ring_phys);
    memset(g_rx_ring, 0, PAGE_SIZE);
    memset(g_tx_ring, 0, PAGE_SIZE);

    for (u32 i = 0; i < RING_SIZE; i++) {
        u64 rx = pmm_alloc_page();
        u64 tx = pmm_alloc_page();
        if (!rx || !tx) return -1;
        g_rx_buf[i] = (u8 *)phys_to_virt(rx);
        g_tx_buf[i] = (u8 *)phys_to_virt(tx);
        g_rx_ring[i].addr = rx;
        g_tx_ring[i].addr = tx;
        g_tx_ring[i].status = TX_STATUS_DD;
    }

    reg_write(E1000_RDBAL, (u32)rx_ring_phys);
    reg_write(E1000_RDBAH, (u32)(rx_ring_phys >> 32));
    reg_write(E1000_RDLEN, RING_SIZE * sizeof(e1000_rx_desc_t));
    reg_write(E1000_RDH, 0);
    reg_write(E1000_RDT, RING_SIZE - 1);

    reg_write(E1000_TDBAL, (u32)tx_ring_phys);
    reg_write(E1000_TDBAH, (u32)(tx_ring_phys >> 32));
    reg_write(E1000_TDLEN, RING_SIZE * sizeof(e1000_tx_desc_t));
    reg_write(E1000_TDH, 0);
    reg_write(E1000_TDT, 0);

    g_rx_tail = 0;
    g_tx_tail = 0;
    return 0;
}

net_device_t *e1000_init(void) {
    static const u16 ids[] = { 0x100E, 0x100F, 0x10D3, 0x153A };
    pci_device_t pci;
    int found = 0;

    for (usize i = 0; i < ARRAY_SIZE(ids); i++) {
        if (pci_find_device(0x8086, ids[i], &pci) == 0) {
            found = 1;
            break;
        }
    }
    if (!found) {
        kprintf("[e1000] no Intel 8254x device found\n");
        return NULL;
    }

    pci_enable_bus_master(&pci);
    u64 bar = pci.bar[0] & ~0xFULL;
    if (!bar) {
        kprintf("[e1000] BAR0 is not a memory BAR\n");
        return NULL;
    }

    g_mmio = (volatile u32 *)phys_to_virt(bar);
    kprintf("[e1000] %04x:%04x at %02x:%02x.%u bar %p irq %u\n",
            pci.vendor, pci.device, pci.bus, pci.slot, pci.func,
            (void *)bar, pci.irq);

    /* Reset and wait for the self-clearing bit. */
    reg_write(E1000_CTRL, CTRL_RST);
    for (int i = 0; i < 1000; i++) {
        if (!(reg_read(E1000_CTRL) & CTRL_RST)) break;
        timer_sleep_ms(1);
    }

    reg_write(E1000_IMS, 0);
    reg_write(E1000_ICR, 0xFFFFFFFFu);
    for (u32 i = 0; i < 128; i++) reg_write(E1000_MTA + i * 4, 0);

    if (e1000_setup_rings() != 0) {
        kprintf("[e1000] cannot allocate descriptor rings\n");
        return NULL;
    }

    /* MAC address: RAL/RAH are loaded from the EEPROM by the hardware. */
    u32 ral = reg_read(E1000_RAL);
    u32 rah = reg_read(E1000_RAH);
    u8 mac[ETH_ALEN];
    mac[0] = (u8)(ral);
    mac[1] = (u8)(ral >> 8);
    mac[2] = (u8)(ral >> 16);
    mac[3] = (u8)(ral >> 24);
    mac[4] = (u8)(rah);
    mac[5] = (u8)(rah >> 8);

    if (!(ral | (rah & 0xFFFF))) {
        u16 w0 = 0, w1 = 0, w2 = 0;
        if (eeprom_read(0, &w0) == 0 && eeprom_read(1, &w1) == 0 &&
            eeprom_read(2, &w2) == 0) {
            mac[0] = (u8)(w0);
            mac[1] = (u8)(w0 >> 8);
            mac[2] = (u8)(w1);
            mac[3] = (u8)(w1 >> 8);
            mac[4] = (u8)(w2);
            mac[5] = (u8)(w2 >> 8);
        }
    }

    reg_write(E1000_RAL, (u32)mac[0] | ((u32)mac[1] << 8) |
                         ((u32)mac[2] << 16) | ((u32)mac[3] << 24));
    reg_write(E1000_RAH, (u32)mac[4] | ((u32)mac[5] << 8) | (1u << 31));

    reg_write(E1000_RCTL, RCTL_EN | RCTL_UPE | RCTL_MPE | RCTL_BAM | RCTL_SECRC);
    reg_write(E1000_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);
    reg_write(E1000_TIPG, 0x0060200Au);
    reg_write(E1000_CTRL, reg_read(E1000_CTRL) | CTRL_SLU);

    g_dev.name[0] = 0;
    strncpy(g_dev.name, "eth0", sizeof(g_dev.name) - 1);
    memcpy(g_dev.mac, mac, ETH_ALEN);
    g_dev.send = e1000_send;
    g_dev.poll = e1000_poll;
    g_dev.priv = &pci;
    g_dev.next = NULL;

    kprintf("[e1000] MAC %02x:%02x:%02x:%02x:%02x:%02x link %s\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
            (reg_read(E1000_STATUS) & 2) ? "up" : "down");
    return &g_dev;
}
