#pragma once
/* Legacy PCI configuration-space access (ports 0xCF8/0xCFC). */

#include <mcube/types.h>

#define PCI_MAX_BARS 6

typedef struct pci_device {
    u8  bus;
    u8  slot;
    u8  func;
    u16 vendor;
    u16 device;
    u8  class_code;
    u8  subclass;
    u8  prog_if;
    u8  irq;
    u64 bar[PCI_MAX_BARS];
} pci_device_t;

u32  pci_config_read32(u8 bus, u8 slot, u8 func, u16 off);
void pci_config_write32(u8 bus, u8 slot, u8 func, u16 off, u32 value);

int  pci_find_device(u16 vendor, u16 device, pci_device_t *out);
int  pci_find_class(u8 class_code, u8 subclass, pci_device_t *out);
int  pci_enable_bus_master(pci_device_t *dev);
void pci_dump(void);
