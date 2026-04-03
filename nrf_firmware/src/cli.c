#include "cli.h"

#include "audio_io/i2s.h"
#include "mode.h"
#include "proprietary/link.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>

#include "tusb.h"

#define APP_FW_VERSION "0.1.0-nrf54" // TODO: auto-generate from git tag/commit
#define CLI_QUEUE_DEPTH 4
#define CLI_THREAD_STACK_SIZE 2048
#define CLI_THREAD_PRIORITY 10

static bool g_cli_connected;
static bool g_cli_echo_enabled = true;
static bool g_status_push_enabled;
static uint32_t g_status_push_period_ms = 500;
static int64_t g_next_status_deadline_ms;
/* Complete command lines are queued here and executed by the low-priority CLI thread. */
K_MSGQ_DEFINE(g_cli_msgq, CLI_MAX_CMD_LEN, CLI_QUEUE_DEPTH, 4);
/* Async user-visible messages from other threads are serialized here. */
K_MSGQ_DEFINE(g_cli_output_msgq, CLI_MAX_OUTPUT_LEN, CLI_QUEUE_DEPTH, 4);

static void cli_process_line(char *line);
static void cli_thread(void *arg1, void *arg2, void *arg3);
static void cli_init(void);

static void cli_write_raw(const char *data, size_t len)
{
	uint32_t written = 0U;

	if (!g_cli_connected || !tud_cdc_connected() || data == NULL || len == 0U) {
		return;
	}

	while (written < len) {
		uint32_t pushed = tud_cdc_write(data + written, len - written);

		if (pushed == 0U) {
			break;
		}

		written += pushed;
	}

	tud_cdc_write_flush();
}

static void cli_print(const char *fmt, ...)
{
	char buf[192];
	va_list args;
	int len;

	va_start(args, fmt);
	len = vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	if (len <= 0) {
		return;
	}

	if (len > (int)(sizeof(buf) - 1)) {
		len = sizeof(buf) - 1;
	}

	cli_write_raw(buf, (size_t)len);
}

static void cli_emit_status(void)
{
	struct radio_stats stats;
	uint32_t loss_permille = 0U;

	proprietary_link_get_stats(&stats);

	if ((stats.packets_rx + stats.packets_lost) > 0U) {
		loss_permille = (stats.packets_lost * 1000U) /
			(stats.packets_rx + stats.packets_lost);
	}

	cli_print("#S rssi=%d bat=100 loss=%u.%u conn=%s tx=%u rx=%u lost=%u urun=0 orun=0 cerr=0 fw=%s\n",
		  stats.last_rssi_dbm,
		  loss_permille / 10U,
		  loss_permille % 10U,
		  stats.peer_connected ? "yes" : "no",
		  stats.packets_tx,
		  stats.packets_rx,
		  stats.packets_lost,
		  APP_FW_VERSION);
}

static void cli_print_mode_group(void)
{
	cli_print("[mode]\n");
	cli_print("role=%s\n", mode_get_role_name(mode_get_current_role()));
	cli_print("mode=%s\n", mode_get_operating_mode_name(mode_get_current_operating_mode()));
}

static void cli_print_radio_group(void)
{
	cli_print("[radio]\n");
	cli_print("phy_rate=4\n");
	cli_print("tx_power=8\n");
	cli_print("fhss_exclusion=none\n");
	cli_print("ble_phy=2M\n");
	cli_print("payload_ms_dl=1\n");
	cli_print("payload_ms_ul=1\n");
	cli_print("jitter_buffer_ms=10\n");
}

static void cli_print_device_group(void)
{
	cli_print("[device]\n");
	cli_print("audio_io=%s\n", mode_get_current_role() == DEVICE_ROLE_DONGLE ? "usb" : "codec");
	cli_print("device_addr=0xD0D0D0D0\n");
	cli_print("peer_addr=0xA1A1A1A1\n");
	cli_print("auto_sleep=0\n");
	cli_print("low_battery_threshold=10\n");
}

static void cli_print_audio_group(void)
{
	cli_print("[audio]\n");
	cli_print("sample_rate_spk=48000\n");
	cli_print("bit_width_spk=16\n");
	cli_print("channels_spk=stereo\n");
	cli_print("codec_spk=pcm\n");
	cli_print("volume=80\n");
	cli_print("sidetone=0\n");
	cli_print("sample_rate_mic=48000\n");
	cli_print("bit_width_mic=16\n");
	cli_print("channels_mic=mono\n");
	cli_print("codec_mic=pcm\n");
	cli_print("mic_gain=12\n");
	cli_print("mic_mute=off\n");
}

static void cli_print_eq_group(void)
{
	cli_print("[eq]\n");
	cli_print("eq0=100,0\n");
	cli_print("eq1=400,0\n");
	cli_print("eq2=1000,0\n");
	cli_print("eq3=4000,0\n");
	cli_print("eq4=10000,0\n");
}

static void cli_print_help(void)
{
	cli_print("Available commands:\n");
	cli_print("  help\n");
	cli_print("  echo on|off\n");
	cli_print("  get <group|param>\n");
	cli_print("  set role <dongle|headset>\n");
	cli_print("  set mode <proprietary|ble|usb>\n");
	cli_print("  i2s tone on|off|status\n");
	cli_print("  status\n");
	cli_print("  status on [ms]\n");
	cli_print("  status off\n");
	cli_print("  reset\n");
	cli_print("  scan\n");
	cli_print("  linktest\n");
}

static void cli_cmd_get(const char *arg)
{
	if (arg == NULL || *arg == '\0' || strcasecmp(arg, "all") == 0) {
		cli_print_audio_group();
		cli_print_radio_group();
		cli_print_mode_group();
		cli_print_device_group();
		cli_print_eq_group();
		return;
	}

	if (strcasecmp(arg, "mode") == 0) {
		cli_print_mode_group();
		return;
	}

	if (strcasecmp(arg, "radio") == 0) {
		cli_print_radio_group();
		return;
	}

	if (strcasecmp(arg, "device") == 0) {
		cli_print_device_group();
		return;
	}

	if (strcasecmp(arg, "audio") == 0) {
		cli_print_audio_group();
		return;
	}

	if (strcasecmp(arg, "eq") == 0) {
		cli_print_eq_group();
		return;
	}

	if (strcasecmp(arg, "role") == 0) {
		cli_print("role=%s\n", mode_get_role_name(mode_get_current_role()));
		return;
	}

	if (strcasecmp(arg, "mode_param") == 0 || strcasecmp(arg, "operating_mode") == 0) {
		cli_print("mode=%s\n", mode_get_operating_mode_name(mode_get_current_operating_mode()));
		return;
	}

	cli_print("ERR %s unknown_param\n", arg);
}

static void cli_cmd_set(char *args)
{
	char *param = strtok(args, " \t");
	char *value = strtok(NULL, "");

	if (param == NULL || value == NULL) {
		cli_print("ERR set invalid_args\n");
		return;
	}

	while (*value == ' ' || *value == '\t') {
		++value;
	}

	if (strcasecmp(param, "role") == 0) {
		bool ok;

		if (strcasecmp(value, "dongle") == 0) {
			ok = mode_request_role(DEVICE_ROLE_DONGLE);
		} else if (strcasecmp(value, "headset") == 0) {
			ok = mode_request_role(DEVICE_ROLE_HEADSET);
		} else {
			cli_print("ERR role invalid_value\n");
			return;
		}

		if (!ok) {
			cli_print("ERR role rejected\n");
			return;
		}

		cli_print("OK requested role=%s\n", value);
		return;
	}

	if (strcasecmp(param, "mode") == 0) {
		bool ok;

		if (strcasecmp(value, "proprietary") == 0) {
			ok = mode_request_operating_mode(OPERATING_MODE_PROPRIETARY);
		} else if (strcasecmp(value, "ble") == 0) {
			ok = mode_request_operating_mode(OPERATING_MODE_BLE);
		} else if (strcasecmp(value, "usb") == 0) {
			ok = mode_request_operating_mode(OPERATING_MODE_USB);
		} else {
			cli_print("ERR mode invalid_value\n");
			return;
		}

		if (!ok) {
			cli_print("ERR mode rejected\n");
			return;
		}

		cli_print("OK requested mode=%s\n", value);
		return;
	}

	cli_print("ERR %s unsupported\n", param);
}

static void cli_cmd_i2s(char *args)
{
	char *subcmd;
	char *value;

	if (args == NULL) {
		cli_print("ERR i2s invalid_args\n");
		return;
	}

	subcmd = strtok(args, " \t");
	value = strtok(NULL, " \t");

	if (subcmd == NULL) {
		cli_print("ERR i2s invalid_args\n");
		return;
	}

	if (strcasecmp(subcmd, "tone") != 0) {
		cli_print("ERR i2s unsupported\n");
		return;
	}

	if (value == NULL || strcasecmp(value, "status") == 0) {
		cli_print("i2s ready=%s tone=%s\n",
			  audio_i2s_is_ready() ? "yes" : "no",
			  audio_i2s_is_tone_enabled() ? "on" : "off");
		return;
	}

	if (strcasecmp(value, "on") == 0) {
		if (!audio_i2s_is_ready()) {
			cli_print("ERR i2s not_ready\n");
			return;
		}

		audio_i2s_set_tone_enabled(true);
		cli_print("OK i2s tone=on\n");
		return;
	}

	if (strcasecmp(value, "off") == 0) {
		audio_i2s_set_tone_enabled(false);
		cli_print("OK i2s tone=off\n");
		return;
	}

	cli_print("ERR i2s invalid_value\n");
}

static void cli_process_line(char *line)
{
	char *cmd;
	char *args;

	if (line == NULL) {
		return;
	}

	while (isspace((unsigned char)*line)) {
		++line;
	}

	if (*line == '\0') {
		return;
	}

	cmd = strtok(line, " \t");
	args = strtok(NULL, "");

	if (g_cli_echo_enabled) {
		cli_print("%s%s%s\n", cmd, args ? " " : "", args ? args : "");
	}

	if (strcasecmp(cmd, "help") == 0) {
		cli_print_help();
		return;
	}

	if (strcasecmp(cmd, "echo") == 0) {
		if (args == NULL || strcasecmp(args, "on") == 0) {
			g_cli_echo_enabled = true;
			cli_print("OK echo=on\n");
		} else if (strcasecmp(args, "off") == 0) {
			g_cli_echo_enabled = false;
			cli_print("OK echo=off\n");
		} else {
			cli_print("ERR echo invalid_value\n");
		}
		return;
	}

	if (strcasecmp(cmd, "status") == 0) {
		if (args == NULL || *args == '\0') {
			cli_emit_status();
			return;
		}

		if (strncasecmp(args, "on", 2) == 0) {
			char *period = args + 2;
			unsigned long value = 500UL;

			while (*period == ' ' || *period == '\t') {
				++period;
			}

			if (*period != '\0') {
				value = strtoul(period, NULL, 10);
				if (value == 0UL) {
					value = 500UL;
				}
			}

			g_status_push_period_ms = (uint32_t)value;
			g_status_push_enabled = true;
			g_next_status_deadline_ms = k_uptime_get() + g_status_push_period_ms;
			cli_print("OK status=%u\n", g_status_push_period_ms);
			return;
		}

		if (strcasecmp(args, "off") == 0) {
			g_status_push_enabled = false;
			cli_print("OK status=off\n");
			return;
		}

		cli_print("ERR status invalid_value\n");
		return;
	}

	if (strcasecmp(cmd, "get") == 0) {
		cli_cmd_get(args);
		return;
	}

	if (strcasecmp(cmd, "set") == 0) {
		cli_cmd_set(args);
		return;
	}

	if (strcasecmp(cmd, "i2s") == 0) {
		cli_cmd_i2s(args);
		return;
	}

	if (strcasecmp(cmd, "scan") == 0 || strcasecmp(cmd, "linktest") == 0) {
		cli_print("ERR %s unsupported\n", cmd);
		return;
	}

	if (strcasecmp(cmd, "reset") == 0) {
		cli_print("OK reset\n");
		sys_reboot(SYS_REBOOT_COLD);
		return;
	}

	cli_print("ERR %s unknown_command\n", cmd);
}

static void cli_init(void)
{
	g_cli_echo_enabled = true;
	g_status_push_enabled = false;
	g_status_push_period_ms = 500;
	g_next_status_deadline_ms = 0;
	k_msgq_purge(&g_cli_msgq);
	k_msgq_purge(&g_cli_output_msgq);
}

void cli_set_connected(bool connected)
{
	bool announce = connected && !g_cli_connected;

	g_cli_connected = connected;

	if (announce) {
		static const char welcome[] = "FlexAudioLink CLI\nType 'help' for commands\n";

		cli_write_raw(welcome, sizeof(welcome) - 1U);
	}
}

void cli_enqueue_command_line(char *line)
{
	char queued_line[CLI_MAX_CMD_LEN];
	size_t len;

	if (line == NULL) {
		return;
	}

	len = strnlen(line, CLI_MAX_CMD_LEN - 1U);
	memcpy(queued_line, line, len);
	queued_line[len] = '\0';
	/* Drop on overflow rather than blocking the USB path. */
	(void)k_msgq_put(&g_cli_msgq, queued_line, K_NO_WAIT);
}

void cli_enqueue_print_msg(const char *message)
{
	char queued_message[CLI_MAX_OUTPUT_LEN];
	size_t len;

	if (message == NULL) {
		return;
	}

	len = strnlen(message, CLI_MAX_OUTPUT_LEN - 1U);
	memcpy(queued_message, message, len);
	queued_message[len] = '\0';
	(void)k_msgq_put(&g_cli_output_msgq, queued_message, K_NO_WAIT);
}

static void cli_thread(void *arg1, void *arg2, void *arg3)
{
	char line[CLI_MAX_CMD_LEN];
	char message[CLI_MAX_OUTPUT_LEN];

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	/* The CLI thread owns CLI-core initialization. Transport hookup stays
	 * outside so USB can attach/detach independently.
	 */
	cli_init();

	/* Mirrors the old FreeRTOS design: USB input is decoupled from command execution. */
	while (1) {
		if (k_msgq_get(&g_cli_msgq, line, K_MSEC(10)) == 0) {
			cli_process_line(line);
		}

		if (k_msgq_get(&g_cli_output_msgq, message, K_NO_WAIT) == 0) {
			cli_write_raw(message, strlen(message));
		}

		if (g_status_push_enabled && g_cli_connected &&
		    k_uptime_get() >= g_next_status_deadline_ms) {
			cli_emit_status();
			g_next_status_deadline_ms = k_uptime_get() +
				g_status_push_period_ms;
		}
	}
}

K_THREAD_DEFINE(cli_thread_id, CLI_THREAD_STACK_SIZE, cli_thread,
		NULL, NULL, NULL, CLI_THREAD_PRIORITY, 0, 0);
