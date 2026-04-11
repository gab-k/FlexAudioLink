#include "usb/usb_cdc.h"

#include "cli.h"

#include <stddef.h>
#include <stdint.h>

#include "tusb.h"

static const char s_cli_no_newline_message[] =
	"Command doesn't end with a newline (\\n) character.\r\n";

#define CLI_CDC_ITF 0U

/* Accumulates a single pending command line from the CDC byte stream. */
static char s_cli_line[CLI_MAX_CMD_LEN];
static size_t s_cli_line_len;

void usb_cdc_init(void)
{
	s_cli_line_len = 0U;
	tud_cdc_n_set_wanted_char(CLI_CDC_ITF, '\n');
}

void usb_cdc_on_unmount(void)
{
	s_cli_line_len = 0U;
	cli_set_connected(false);
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
	(void)itf;
	(void)rts;
	cli_set_connected(dtr);
}

void tud_cdc_rx_cb(uint8_t itf)
{
	uint32_t available_bytes;

	(void)itf;

	/* USB CDC input can arrive split across multiple packets/callbacks, so do
	 * not discard partial data just because '\n' has not arrived yet. TinyUSB
	 * will invoke tud_cdc_rx_wanted_cb() once the configured delimiter is seen.
	 *
	 * Only treat the stream as malformed if it grows to a full command-length
	 * chunk without a newline, since that leaves no room for a terminator.
	 */
	available_bytes = tud_cdc_n_available(itf);
	if (available_bytes < (CLI_MAX_CMD_LEN - 1U)) {
		return;
	}

	tud_cdc_n_read_flush(itf);
	s_cli_line_len = 0U;
	cli_enqueue_print_msg(s_cli_no_newline_message);
}

void tud_cdc_rx_wanted_cb(uint8_t itf, char wanted_char)
{
	bool overflowed = false;
	(void)wanted_char;

	while (tud_cdc_n_available(itf) > 0U) {
		int32_t ch = tud_cdc_n_read_char(itf);

		if (ch < 0) {
			break;
		}

		if (ch == '\r' || ch == '\n') {
			if (!overflowed && s_cli_line_len > 0U) {
				s_cli_line[s_cli_line_len] = '\0';
				cli_enqueue_command_line(s_cli_line);
			}

			s_cli_line_len = 0U;
			overflowed = false;
			continue;
		}

		if (overflowed) {
			continue;
		}

		if (s_cli_line_len < (CLI_MAX_CMD_LEN - 1U)) {
			s_cli_line[s_cli_line_len++] = (char)ch;
			continue;
		}

		s_cli_line_len = 0U;
		overflowed = true;
		cli_enqueue_print_msg(s_cli_no_newline_message);
	}
}
