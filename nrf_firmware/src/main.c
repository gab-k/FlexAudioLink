/*
 * FlexLink Radio Test - nRF54LM20-DK
 *
 * Unified TX/RX firmware - role selected at runtime based on device unique ID.
 * Flash both boards with the same binary. The board whose UID matches
 * TX_DEVICE_ID transmits; all others receive.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/hwinfo.h>
#include <string.h>

#include "radio.h"

/***************************************************************************
 * Read device UID and decide role
 ***************************************************************************/

static uint64_t read_device_uid(void)
{
    uint8_t id[16];
    ssize_t len;

    len = hwinfo_get_device_id(id, sizeof(id));
    if (len < 0) {
        printk("hwinfo_get_device_id failed: %zd\n", len);
        return 0;
    }

    printk("Device UID (%zd bytes):", len);
    for (int i = 0; i < len; i++) {
        printk(" %02x", id[i]);
    }
    printk("\n");

    /* Take first 8 bytes as uint64 (little-endian) */
    uint64_t uid = 0;
    int copy_len = (len < 8) ? len : 8;
    memcpy(&uid, id, copy_len);

    printk("Device UID (u64): 0x%016llx\n", uid);
    return uid;
}

/***************************************************************************
 * Main - read UID and branch
 ***************************************************************************/

int main(void)
{
    printk("\n=== FlexLink Radio Test ===\n");

    uint64_t uid = read_device_uid();

    /* Set role before radio_init so the ISR knows which path to take */
    is_tx = (TX_DEVICE_ID != 0 && uid == TX_DEVICE_ID);

    radio_init();

    if (is_tx) {
        printk("Role: TX (UID matched)\n");
        run_tx();
    } else {
        if (TX_DEVICE_ID == 0) {
            printk("Role: RX (TX_DEVICE_ID not configured)\n");
        } else {
            printk("Role: RX (UID did not match TX board)\n");
        }
        run_rx();
    }

    return 0;
}
