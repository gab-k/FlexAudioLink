#include "usb/usb_cdc.h"

#include "cli.h"

#include <stddef.h>
#include <stdint.h>

#include "tusb.h"

static const char s_cli_no_newline_message[] =
	"Command doesn't end with a newline (\\n) character.\r\n";

/* Accumulates a single pending command line from the CDC byte stream. */
static char s_cli_line[CLI_MAX_CMD_LEN];
static size_t s_cli_line_len;

void usb_cdc_init(void)
{
	s_cli_line_len = 0U;
	tud_cdc_set_wanted_char('\n');
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
	(void)itf;

	/* Match the old firmware behavior: if data arrives without the wanted
	 * newline terminator, discard it and warn immediately.
	 */
	tud_cdc_read_flush();
	s_cli_line_len = 0U;
	cli_enqueue_print_msg(s_cli_no_newline_message);
}

void tud_cdc_rx_wanted_cb(uint8_t itf, char wanted_char)
{
	uint32_t available_bytes;
	uint32_t read_len;
	uint32_t count = 0U;

	(void)itf;
	(void)wanted_char;

	available_bytes = tud_cdc_available();
	read_len = (available_bytes < (CLI_MAX_CMD_LEN - 1U)) ?
		available_bytes : (CLI_MAX_CMD_LEN - 1U);

	if (read_len > 0U) {
		count = tud_cdc_read(s_cli_line, read_len);
	}

	while (count > 0U &&
	       (s_cli_line[count - 1U] == '\n' || s_cli_line[count - 1U] == '\r')) {
		count--;
	}

	s_cli_line_len = count;
	
	if (s_cli_line_len == 0U) {
		return;
	}

	s_cli_line[s_cli_line_len] = '\0';
	cli_enqueue_command_line(s_cli_line);
	s_cli_line_len = 0U;
}
