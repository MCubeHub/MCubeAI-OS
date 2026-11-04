#pragma once
/* USB host controllers (UHCI / EHCI / xHCI) and HID pointing devices.
 *
 * The kernel only needs input: a HID tablet / absolute pointer or a single
 * touch digitizer feeds absolute pixel coordinates into the same event queue
 * the PS/2 mouse uses (input_push_abs). usb_init() only probes the controllers;
 * the port-by-port enumeration then runs in its own kernel thread
 * (usb_enum_task), because control transfers wait on the PIT and a single
 * unresponsive port on a 20-port xHCI used to stall the whole boot. The runtime
 * soft timer only polls interrupt-IN endpoints, so the callback stays in
 * "read a register, test a bit" territory.
 *
 * Controller drivers live in kernel/bus/usb_uhci.c, usb_ehci.c and usb_xhci.c
 * and register themselves through usb_register_controller(). Everything else
 * (device enumeration, HID report descriptor parsing, report decoding) is in
 * kernel/bus/usb.c.
 */

#include <mcube/types.h>

#define USB_MAX_CONTROLLERS 4
#define USB_MAX_DEVICES     12
#define USB_MAX_HID_FIELDS  48
#define USB_MAX_TOUCH_CONTACTS 8

/* Device speed. The values match the xHCI PORTSC speed field. */
#define USB_SPEED_LOW       1
#define USB_SPEED_FULL      2
#define USB_SPEED_HIGH      3
#define USB_SPEED_SUPER     4

/* How a HID device is decoded. */
#define USB_HID_NONE        0
#define USB_HID_MOUSE       1   /* relative pointer */
#define USB_HID_POINTER     2   /* absolute pointer (tablet) */
#define USB_HID_TOUCH       3   /* single touch / digitizer */
#define USB_HID_KEYBOARD    4   /* boot-compatible keyboard */

/* Descriptor / request constants. */
#define USB_REQ_GET_STATUS        0x00
#define USB_REQ_SET_ADDRESS       0x05
#define USB_REQ_GET_DESCRIPTOR    0x06
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_REQ_SET_IDLE          0x0A

#define USB_DESC_DEVICE     1
#define USB_DESC_CONFIG     2
#define USB_DESC_STRING     3
#define USB_DESC_INTERFACE  4
#define USB_DESC_ENDPOINT   5
#define USB_DESC_HID        0x21
#define USB_DESC_REPORT     0x22

typedef struct PACKED usb_setup {
    u8  bmRequestType;
    u8  bRequest;
    u16 wValue;
    u16 wIndex;
    u16 wLength;
} usb_setup_t;

typedef struct PACKED usb_device_desc {
    u8  bLength;
    u8  bDescriptorType;
    u16 bcdUSB;
    u8  bDeviceClass;
    u8  bDeviceSubClass;
    u8  bDeviceProtocol;
    u8  bMaxPacketSize0;
    u16 idVendor;
    u16 idProduct;
    u16 bcdDevice;
    u8  iManufacturer;
    u8  iProduct;
    u8  iSerialNumber;
    u8  bNumConfigurations;
} usb_device_desc_t;

typedef struct PACKED usb_config_desc {
    u8  bLength;
    u8  bDescriptorType;
    u16 wTotalLength;
    u8  bNumInterfaces;
    u8  bConfigurationValue;
    u8  iConfiguration;
    u8  bmAttributes;
    u8  bMaxPower;
} usb_config_desc_t;

typedef struct PACKED usb_interface_desc {
    u8 bLength;
    u8 bDescriptorType;
    u8 bInterfaceNumber;
    u8 bAlternateSetting;
    u8 bNumEndpoints;
    u8 bInterfaceClass;
    u8 bInterfaceSubClass;
    u8 bInterfaceProtocol;
    u8 iInterface;
} usb_interface_desc_t;

typedef struct PACKED usb_endpoint_desc {
    u8  bLength;
    u8  bDescriptorType;
    u8  bEndpointAddress;
    u8  bmAttributes;
    u16 wMaxPacketSize;
    u8  bInterval;
} usb_endpoint_desc_t;

typedef struct PACKED usb_hid_desc {
    u8  bLength;
    u8  bDescriptorType;
    u16 bcdHID;
    u8  bCountryCode;
    u8  bNumDescriptors;
    u8  bReportType;
    u16 bReportLength;
} usb_hid_desc_t;

/* One decoded HID report field (an Input item element). */
typedef struct usb_hid_field {
    u8  report_id;
    u8  bit_size;
    u8  flags;          /* Input item bits: bit0 constant, bit1 variable,
                         * bit2 relative */
    u16 usage_page;
    u16 usage;
    u32 bit_offset;     /* bits from the start of the report body */
    i32 logical_min;
    i32 logical_max;
} usb_hid_field_t;

typedef struct usb_controller usb_controller_t;
typedef struct usb_device usb_device_t;

/* Driver vector registered by each controller front end. All functions run in
 * thread context during boot except intr_poll(), which is also called from
 * the soft timer (interrupts off: never sleep in it). */
typedef struct usb_hcd_ops {
    const char *name;
    /* Bring the controller out of the firmware's hands (legacy SMI handoff,
     * reset, run, port power). Optional: called from usb_enum_task() once the
     * desktop is up, never during boot. A controller whose firmware refuses
     * the handoff must return non-zero and is then left alone. */
    int  (*start)(usb_controller_t *c);
    int  (*port_count)(usb_controller_t *c);
    int  (*port_connected)(usb_controller_t *c, int port);
    int  (*dev_open)(usb_controller_t *c, int port, usb_device_t *dev);
    int  (*set_address)(usb_controller_t *c, usb_device_t *dev, u8 address);
    int  (*control)(usb_controller_t *c, usb_device_t *dev,
                    const usb_setup_t *setup, void *buf, u32 len);
    int  (*intr_open)(usb_controller_t *c, usb_device_t *dev,
                      u8 ep_addr, u16 maxp, u8 interval);
    /* Non-blocking: returns the byte count of a completed report, 0 when
     * nothing arrived yet, negative on a transfer error. */
    int  (*intr_poll)(usb_controller_t *c, usb_device_t *dev,
                      void *buf, u32 len);
} usb_hcd_ops_t;

struct usb_controller {
    const usb_hcd_ops_t *ops;
    void *priv;
    u8 bus, slot, func;
};

struct usb_device {
    usb_controller_t *ctrl;
    void *hcd_priv;          /* per-device state owned by the controller */
    u8  address;
    u8  port;
    u8  speed;
    u8  ep0_maxp;            /* bMaxPacketSize0, 0 until the descriptor is read */
    u16 vendor;
    u16 product;
    u8  class_code;
    u8  subclass;
    u8  protocol;

    /* Chosen HID interrupt IN endpoint. */
    u8  ep_in;
    u16 ep_maxp;
    u8  ep_interval;

    /* HID decoding state. */
    u8  kind;                /* USB_HID_* */
    u8  report_id;           /* 0 = no report IDs */
    u16 report_len;          /* bytes of the selected report, ID included */
    u8  field_count;
    usb_hid_field_t fields[USB_MAX_HID_FIELDS];
    i16 x_idx, y_idx, btn_idx, tip_idx, cid_idx;

    /* Touch contacts. A Windows-precision panel packs several contacts into
     * one report, repeating Tip Switch / Contact ID / X / Y once per contact;
     * the k-th occurrence of each usage belongs to contact k. Single-contact
     * panels simply leave touch_contacts == 1. */
    u8  touch_contacts;
    i16 t_x[USB_MAX_TOUCH_CONTACTS];
    i16 t_y[USB_MAX_TOUCH_CONTACTS];
    i16 t_tip[USB_MAX_TOUCH_CONTACTS];
    i16 t_cid[USB_MAX_TOUCH_CONTACTS];

    /* Keyboard state: HID usages currently reported as held, plus the
     * modifier byte and Caps Lock latch. Used to synthesise press/release
     * edges because the report only carries the current key set. */
    u8  key_slots[8];
    u8  key_slot_count;
    u8  key_mods;
    u8  key_caps;

    /* Throttled logging. */
    i32 last_x, last_y;
    u32 last_buttons;
    u32 report_count;
};

/* kernel/bus/usb.c */
int  usb_init(void);
/* Port-by-port enumeration, run from a kernel thread after the scheduler is
 * up: a wedged port then only delays input, never the desktop. */
void usb_enum_task(void *arg);
void usb_poll(void);
int  usb_device_count(void);

/* Snapshot for SYS_USBINFO: controller/port/device counters plus the last
 * controller-level note. Safe to call from a syscall; it only reads
 * registers and kernel bookkeeping. */
struct mcube_usb_info;
void usb_get_info(struct mcube_usb_info *out);
/* Record a short controller-level note (max ~95 chars). It ends up in
 * mcube_usb_info_t.text, which the desktop shows, so keep it factual. */
void usb_diag_note(const char *fmt, ...);

/* Shared helpers for the controller drivers. */
int  usb_register_controller(const usb_hcd_ops_t *ops, void *priv,
                             u8 bus, u8 slot, u8 func);
u64  usb_dma_alloc(u32 pages);      /* zeroed, page aligned, physical address */

/* Map a controller BAR into the kernel direct map. The loader only builds the
 * identity window below 64 GiB, and UEFI is free to park a 64-bit BAR higher
 * than that (QEMU's OVMF puts the xHCI MMIO at 0xE000000000), so those pages
 * are mapped on demand. Returns a usable pointer, or NULL. */
void *usb_map_mmio(u64 phys, u64 size);

/* Controller probes (called by usb_init). */
int  usb_uhci_probe(void);
int  usb_ehci_probe(void);
int  usb_xhci_probe(void);
