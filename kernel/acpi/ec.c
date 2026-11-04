/* ACPI embedded controller (EC) byte transactions.
 *
 * Battery state lives behind the EC on every laptop: the DSDT declares
 * OperationRegion(..., EmbeddedControl, 0, 0x100) and carves it into bit
 * fields that the AML methods read. Those reads have to go through the two
 * legacy ports (command/status 0x66, data 0x62) with the standard IBF/OBF
 * handshake.
 *
 * Everything here is best-effort. A desktop has no EC at all, the status port
 * reads back 0xFF and every call fails with a plain -1 so callers can fall back
 * to "no telemetry" instead of hanging.
 */

#include <mcube/acpi.h>
#include <mcube/io.h>
#include <mcube/kernel.h>

#define EC_SC            0x66   /* command / status */
#define EC_DATA          0x62   /* data */

#define EC_SC_OBF        0x01   /* output buffer full: EC has data for us */
#define EC_SC_IBF        0x02   /* input buffer full: EC is busy with our byte */

#define EC_CMD_READ      0x80
#define EC_CMD_WRITE     0x81

/* Roughly 1 ms of port I/O at ~1us per io_wait, generous enough for a slow
 * EC but short enough that a missing controller never stalls the boot. */
#define EC_SPIN          20000

static int g_ec_state = -1;    /* -1 unknown, 0 absent, 1 present */

static int ec_wait_clear_ibf(void) {
    for (int i = 0; i < EC_SPIN; i++) {
        if (!(inb(EC_SC) & EC_SC_IBF)) return 0;
        io_wait();
    }
    return -1;
}

static int ec_wait_set_obf(void) {
    for (int i = 0; i < EC_SPIN; i++) {
        if (inb(EC_SC) & EC_SC_OBF) return 0;
        io_wait();
    }
    return -1;
}

int ec_available(void) {
    if (g_ec_state >= 0) return g_ec_state;
    /* An unpopulated port reads 0xFF. A real EC drives the status register and
     * never leaves all bits set. */
    g_ec_state = (inb(EC_SC) == 0xFF) ? 0 : 1;
    return g_ec_state;
}

/* Discard a byte the firmware left in the output buffer, otherwise the OBF
 * handshake below would immediately "succeed" with stale data. */
static void ec_drain(void) {
    if (inb(EC_SC) & EC_SC_OBF) (void)inb(EC_DATA);
}

int ec_read(u8 addr, u8 *out) {
    if (!out || !ec_available()) return -1;

    for (int attempt = 0; attempt < 3; attempt++) {
        ec_drain();
        if (ec_wait_clear_ibf()) continue;
        outb(EC_SC, EC_CMD_READ);
        if (ec_wait_clear_ibf()) continue;
        outb(EC_DATA, addr);
        if (ec_wait_set_obf()) continue;
        *out = inb(EC_DATA);
        return 0;
    }
    return -1;
}

int ec_write(u8 addr, u8 val) {
    if (!ec_available()) return -1;

    for (int attempt = 0; attempt < 3; attempt++) {
        ec_drain();
        if (ec_wait_clear_ibf()) continue;
        outb(EC_SC, EC_CMD_WRITE);
        if (ec_wait_clear_ibf()) continue;
        outb(EC_DATA, addr);
        if (ec_wait_clear_ibf()) continue;
        outb(EC_DATA, val);
        if (ec_wait_clear_ibf()) continue;
        return 0;
    }
    return -1;
}
