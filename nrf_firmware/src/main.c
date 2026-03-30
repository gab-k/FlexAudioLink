/*
 * FlexLink Radio Test - nRF54LM20-DK
 *
 * Unified TX/RX firmware - role selected at runtime based on device unique ID.
 * Flash both boards with the same binary. The board whose UID matches
 * TX_DEVICE_ID transmits; all others receive.
 *
 * All subsystems self-initialize via K_THREAD_DEFINE; main is idle.
 */

#include <zephyr/kernel.h>

int main(void)
{
	return 0;
}
