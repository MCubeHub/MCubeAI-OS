/* PCI bus enumeration and BAR decoding.
 *
 * Only the legacy CAM mechanism is used for now. It is enough for QEMU's
 * e1000, PIIX IDE and future AHCI/NVMe devices, and works before any ACPI
 * PCI routing is parsed.
 */

#include <mcube/pci.h>
#include <mcube/io.h>
#include <mcube/kernel.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static u32 cfg_addr(u8 bus, u8 slot, u8 func, u16 off) {
    return 0x80000000u | ((u32)bus << 16) | ((u32)slot << 11) |
           ((u32)func << 8) | (off & 0xFC);
}

u32 pci_config_read32(u8 bus, u8 slot, u8 func, u16 off) {
    outl(PCI_CONFIG_ADDRESS, cfg_addr(bus, slot, func, off));
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write32(u8 bus, u8 slot, u8 func, u16 off, u32 value) {
    outl(PCI_CONFIG_ADDRESS, cfg_addr(bus, slot, func, off));
    outl(PCI_CONFIG_DATA, value);
}

static void read_bars(pci_device_t *dev) {
    for (int i = 0; i < PCI_MAX_BARS; i++) {
        u16 off = (u16)(0x10 + i * 4);
        u32 lo = pci_config_read32(dev->bus, dev->slot, dev->func, off);
        if (lo == 0 || (lo & 1)) {
            dev->bar[i] = 0;
            continue;
        }
        u64 addr = lo & 0xFFFFFFF0u;
        int width = 1;
        if (((lo >> 1) & 3) == 2 && i + 1 < PCI_MAX_BARS) {
            u32 hi = pci_config_read32(dev->bus, dev->slot, dev->func,
                                       (u16)(off + 4));
            addr |= (u64)hi << 32;
            dev->bar[i + 1] = 0;
            width = 2;
        }
        dev->bar[i] = addr;
        i += width - 1;
    }
}

static void fill_device(pci_device_t *dev) {
    u32 id = pci_config_read32(dev->bus, dev->slot, dev->func, 0x00);
    dev->vendor = (u16)(id & 0xFFFF);
    dev->device = (u16)(id >> 16);

    u32 classreg = pci_config_read32(dev->bus, dev->slot, dev->func, 0x08);
    dev->prog_if = (u8)(classreg >> 8);
    dev->subclass = (u8)(classreg >> 16);
    dev->class_code = (u8)(classreg >> 24);

    u32 irq = pci_config_read32(dev->bus, dev->slot, dev->func, 0x3C);
    dev->irq = (u8)(irq & 0xFF);

    read_bars(dev);
}

int pci_find_device(u16 vendor, u16 device, pci_device_t *out) {
    for (u8 bus = 0; bus < 8; bus++) {
        for (u8 slot = 0; slot < 32; slot++) {
            for (u8 func = 0; func < 8; func++) {
                u32 id = pci_config_read32(bus, slot, func, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) {
                    if (func == 0) break;
                    continue;
                }
                if ((u16)(id & 0xFFFF) != vendor ||
                    (u16)(id >> 16) != device) {
                    continue;
                }
                memset(out, 0, sizeof(*out));
                out->bus = bus;
                out->slot = slot;
                out->func = func;
                fill_device(out);
                return 0;
            }
        }
    }
    return -1;
}

int pci_find_class(u8 class_code, u8 subclass, pci_device_t *out) {
    for (u8 bus = 0; bus < 8; bus++) {
        for (u8 slot = 0; slot < 32; slot++) {
            for (u8 func = 0; func < 8; func++) {
                u32 id = pci_config_read32(bus, slot, func, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) {
                    if (func == 0) break;
                    continue;
                }
                u32 classreg = pci_config_read32(bus, slot, func, 0x08);
                if ((u8)(classreg >> 24) != class_code ||
                    (u8)(classreg >> 16) != subclass) {
                    continue;
                }
                memset(out, 0, sizeof(*out));
                out->bus = bus;
                out->slot = slot;
                out->func = func;
                fill_device(out);
                return 0;
            }
        }
    }
    return -1;
}

int pci_enable_bus_master(pci_device_t *dev) {
    u32 cmd = pci_config_read32(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= 0x0006u; /* memory space + bus master */
    pci_config_write32(dev->bus, dev->slot, dev->func, 0x04, cmd);
    return 0;
}

void pci_dump(void) {
    for (u8 bus = 0; bus < 8; bus++) {
        for (u8 slot = 0; slot < 32; slot++) {
            for (u8 func = 0; func < 8; func++) {
                u32 id = pci_config_read32(bus, slot, func, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) {
                    if (func == 0) break;
                    continue;
                }
                u32 classreg = pci_config_read32(bus, slot, func, 0x08);
                kprintf("[pci] %02x:%02x.%u %04x:%04x class %02x:%02x.%02x\n",
                        bus, slot, func, (u16)(id & 0xFFFF),
                        (u16)(id >> 16), (u8)(classreg >> 24),
                        (u8)(classreg >> 16), (u8)(classreg >> 8));
            }
        }
    }
}
