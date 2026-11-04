#pragma once
/* Minimal ACPI table access: RSDP -> RSDT/XSDT -> FADT, plus the ACPI mode
 * handshake and the FADT reset register. Everything the power paths need to
 * behave on real firmware instead of only on the handful of hard-coded chipset
 * ports QEMU happens to answer on. */

#include <mcube/types.h>
#include <mcube/bootinfo.h>

void acpi_init(const boot_info_t *bi);
void acpi_print_summary(void);

/* True when the firmware handed us usable FADT power-management blocks. */
int acpi_available(void);

/* True when the DSDT describes a battery pack (_BST/_BIF/_BIX). The kernel
 * does not interpret AML yet, so this is a presence probe, not telemetry. */
int acpi_battery_present(void);

/* Request ACPI S5 (soft off). Returns 0 only if the machine survived, which
 * means the request was ignored. */
int acpi_shutdown(void);

/* Request a platform reset through the FADT reset register. Returns 0 if the
 * request could not be issued. */
int acpi_reboot(void);

/* ---- embedded controller (kernel/acpi/ec.c) -------------------------- */

/* 1 when a controller answers on the legacy 0x66 status port. */
int ec_available(void);
/* One EC byte transaction. Both return 0 on success. */
int ec_read(u8 addr, u8 *out);
int ec_write(u8 addr, u8 val);

/* ---- power-source telemetry ------------------------------------------ */

typedef struct {
    u8  present;      /* firmware describes a battery pack */
    u8  ac_online;    /* running from mains */
    u8  charging;     /* battery is charging */
    u8  has_percent;  /* `percent` holds a measurement */
    u8  percent;      /* 0..100 */
    u8  pad[3];
    u32 minutes_left; /* 0 = unknown */
} acpi_battery_state_t;

/* Query the firmware through AML (_BST + _BIX/_BIF). Cached briefly because a
 * call may hit the EC several times. Returns 0 when `out` was filled, -1 when
 * the platform cannot be queried (no battery, no AML, EC failure is masked:
 * `present` may still be 1 with `has_percent` 0). */
int acpi_battery_read(acpi_battery_state_t *out);

/* ---- minimal AML interpreter (kernel/acpi/aml.c) --------------------- */

/* Build the namespace from the DSDT. Returns the object count, 0 on failure. */
int aml_init(const u8 *dsdt, u32 len);
/* Number of namespace objects recorded by the last aml_init(). */
int aml_objects(void);
/* Run the battery methods and translate them into a reading. Returns 0 when a
 * percentage was produced, -1 otherwise. Never faults: any unsupported AML
 * opcode simply aborts the evaluation. */
int aml_battery_query(acpi_battery_state_t *out);
