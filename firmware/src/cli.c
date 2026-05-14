#include "cli.h"

#include "app_control.h"
#include "audio/i2s.h"
#include "audio/path_common.h"
#include "audio/path_wired.h"
#include "audio/path_dongle.h"
#include "audio/path_headset.h"
#include "prop/session.h"
#include "prop/test_mode.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>


#define APP_FW_VERSION "0.1.0-nrf54" // TODO: auto-generate from git tag/commit
#define CLI_QUEUE_DEPTH 4
#define CLI_THREAD_STACK_SIZE 2048
#define CLI_THREAD_PRIORITY 10

static bool cli_connected;
static bool cli_echo_enabled = true;
static bool status_link_push_enabled;
static bool status_audio_push_enabled;
static uint32_t status_link_push_period_ms = 500;
static uint32_t status_audio_push_period_ms = 500;
static int64_t next_status_link_deadline_ms;
static int64_t next_status_audio_deadline_ms;
/* Complete command lines are queued here and executed by the low-priority CLI thread. */
K_MSGQ_DEFINE(cli_msgq, CLI_MAX_CMD_LEN, CLI_QUEUE_DEPTH, 4);
/* Async user-visible messages from other threads are serialized here. */
K_MSGQ_DEFINE(cli_output_msgq, CLI_MAX_OUTPUT_LEN, CLI_QUEUE_DEPTH, 4);

static void cli_process_line(char *line);
static void cli_thread(void *arg1, void *arg2, void *arg3);
static void cli_init(void);
static void cli_print(const char *fmt, ...);

static size_t cli_strnlen(const char *s, size_t max_len)
{
	size_t len = 0U;

	if (s == NULL) {
		return 0U;
	}

	while (len < max_len && s[len] != '\0') {
		len++;
	}

	return len;
}

static void cli_write_raw(const char *data, size_t len)
{
	uint32_t written = 0U;

	if (!cli_connected || !tud_cdc_connected() || data == NULL || len == 0U) {
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
	char buf[CLI_MAX_OUTPUT_LEN];
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

static void cli_emit_status_link_push(void)
{
	struct prop_session_report stats;
	uint32_t loss_permille = 0U;

	prop_session_get_report(&stats);

	if ((stats.rx_ok_count + stats.packets_lost_in_service) > 0U) {
		loss_permille = (stats.packets_lost_in_service * 1000U) /
			(stats.rx_ok_count + stats.packets_lost_in_service);
	}

	cli_print("#L rssi=%d loss=%u.%u tx=%u rx_ok=%u "
		  "lost=%u crc_err=%u dlate=%u rx_incmplt=%u txf=%u "
		  "lb1=%u lb2=%u lb34=%u lb5p=%u lbmax=%u "
		  "outages=%u in_service_ms=%llu\n",
		  stats.last_rssi_dbm,
		  loss_permille / 10U,
		  loss_permille % 10U,
		  stats.packets_tx,
		  stats.rx_ok_count,
		  stats.packets_lost_in_service,
		  stats.crc_error_count,
		  stats.deadline_late_count,
		  stats.rx_incomplete_count,
		  stats.tx_trigger_fail_count,
		  stats.loss_burst_1_count,
		  stats.loss_burst_2_count,
		  stats.loss_burst_3_4_count,
		  stats.loss_burst_5_plus_count,
		  stats.max_loss_burst_len,
		  stats.outage_count,
		  stats.time_in_service_us / 1000U);
}

static void cli_emit_status_audio_push(void)
{
	enum app_mode mode = app_control_get_current_mode();

	switch (mode) {
	case APP_MODE_USB: {
		struct codec_path_status s;

		path_wired_get_status(&s);
		cli_print("#A state=%s spk_fifo_bytes=%u spk_pending_bytes=%u "
			  "spk_filtered_level_bytes=%u spk_error_bytes=%d spk_p_adjust_hz=%d "
			  "spk_fll_target_rate_hz=%d spk_underruns=%u\n",
			  path_get_state_name(s.stream_state),
			  s.spk_fifo_bytes,
			  s.spk_pending_bytes,
			  s.spk_filtered_level_bytes,
			  s.spk_error_bytes,
			  s.spk_p_adjust_hz,
			  s.spk_fll_target_rate_hz,
			  s.spk_underrun_events);
		return;
	}
	case APP_MODE_PROP_DONGLE: {
		struct path_dongle_status s;

		path_dongle_get_status(&s);
		cli_print("#A overflow_bytes=%u\n",
			  s.overflow_bytes);
		return;
	}
	case APP_MODE_PROP_HEADSET: {
		struct codec_path_status s;

		path_headset_get_status(&s);
		cli_print("#A state=%s spk_fifo_bytes=%u spk_pending_bytes=%u "
			  "spk_filtered_level_bytes=%u spk_error_bytes=%d spk_p_adjust_hz=%d "
			  "spk_fll_target_rate_hz=%d spk_underruns=%u\n",
			  path_get_state_name(s.stream_state),
			  s.spk_fifo_bytes,
			  s.spk_pending_bytes,
			  s.spk_filtered_level_bytes,
			  s.spk_error_bytes,
			  s.spk_p_adjust_hz,
			  s.spk_fll_target_rate_hz,
			  s.spk_underrun_events);
		return;
	}
	default:
		cli_print("#A state=none\n");
		return;
	}
}

static uint32_t cli_parse_status_period_ms(char *args_after_on)
{
	unsigned long value = 500UL;

	while (args_after_on != NULL && (*args_after_on == ' ' || *args_after_on == '\t')) {
		++args_after_on;
	}

	if (args_after_on != NULL && *args_after_on != '\0') {
		value = strtoul(args_after_on, NULL, 10);
		if (value == 0UL) {
			value = 500UL;
		}
	}

	return (uint32_t)value;
}

static void cli_print_mode_group(void)
{
	cli_print("[mode]\n");
	cli_print("mode=%s\n", app_control_get_mode_name(app_control_get_current_mode()));
}

static void cli_print_radio_group(void)
{
	cli_print("[radio]\n");
	cli_print("phy_rate=4\n");
	cli_print("tx_power=8\n");
	cli_print("fhss_exclusion=none\n");
	cli_print("payload_ms_dl=1\n");
	cli_print("payload_ms_ul=1\n");
	cli_print("jitter_buffer_ms=10\n");
}

static void cli_print_device_group(void)
{
	const char *audio_io;

	switch (app_control_get_current_mode()) {
	case APP_MODE_USB:
		audio_io = "wired";
		break;
	case APP_MODE_PROP_DONGLE:
		audio_io = "usb";
		break;
	case APP_MODE_PROP_HEADSET:
		audio_io = "codec";
		break;
	default:
		audio_io = "unknown";
		break;
	}

	cli_print("[device]\n");
	cli_print("audio_io=%s\n", audio_io);
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
	cli_print("  set mode <usb|prop_dongle|prop_headset>\n");
	cli_print("  i2s tone on|off|status\n");
	cli_print("  linktest on|off|status\n");
	cli_print("  status_link on [ms]|off\n");
	cli_print("  status_audio on [ms]|off\n");
	cli_print("  fll auto|status|<rate_hz>\n");
	cli_print("  reset\n");
	cli_print("  scan\n");
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

	if (strcasecmp(param, "mode") == 0) {
		enum app_mode mode;

		if (strcasecmp(value, "usb") == 0) {
			mode = APP_MODE_USB;
		} else if (strcasecmp(value, "prop_dongle") == 0) {
			mode = APP_MODE_PROP_DONGLE;
		} else if (strcasecmp(value, "prop_headset") == 0) {
			mode = APP_MODE_PROP_HEADSET;
		} else {
			cli_print("ERR mode invalid_value\n");
			return;
		}

		if (mode == app_control_get_current_mode()) {
			cli_print("OK mode=%s\n", app_control_get_mode_name(mode));
			return;
		}

		if (!app_control_save_boot_mode(mode)) {
			cli_print("ERR mode persist_failed\n");
			return;
		}

		cli_print("OK mode=%s\nrebooting...\n", app_control_get_mode_name(mode));
		tud_cdc_write_flush();
		k_sleep(K_MSEC(50));
		sys_reboot(SYS_REBOOT_COLD);
		/* sys_reboot does not return; surface error if it does. */
		cli_print("ERR mode reboot_failed\n");
		return;
	}

	cli_print("ERR %s unsupported\n", param);
}

static void cli_cmd_linktest(char *args)
{
	if (args == NULL || *args == '\0' || strcasecmp(args, "status") == 0) {
		cli_print("linktest=%s\n", prop_test_mode_is_running() ? "on" : "off");
		return;
	}

	if (strcasecmp(args, "on") == 0) {
		if (!prop_test_mode_start()) {
			cli_print("ERR linktest start_failed\n");
			return;
		}

		cli_print("OK linktest=on\n");
		return;
	}

	if (strcasecmp(args, "off") == 0) {
		if (!prop_test_mode_stop()) {
			cli_print("ERR linktest stop_failed\n");
			return;
		}

		cli_print("OK linktest=off\n");
		return;
	}

	cli_print("ERR linktest invalid_value\n");
}

static void cli_cmd_status_link(char *args)
{
	if (args == NULL || *args == '\0') {
		cli_print("ERR status_link invalid_value\n");
		return;
	}

	if (strncasecmp(args, "on", 2) == 0) {
		status_link_push_period_ms = cli_parse_status_period_ms(args + 2);
		status_link_push_enabled = true;
		next_status_link_deadline_ms = k_uptime_get() + status_link_push_period_ms;
		cli_print("OK status_link=%u\n", status_link_push_period_ms);
		return;
	}

	if (strcasecmp(args, "off") == 0) {
		status_link_push_enabled = false;
		cli_print("OK status_link=off\n");
		return;
	}

	cli_print("ERR status_link invalid_value\n");
}

static void cli_cmd_status_audio(char *args)
{
	if (args == NULL || *args == '\0') {
		cli_print("ERR status_audio invalid_value\n");
		return;
	}

	if (strncasecmp(args, "on", 2) == 0) {
		status_audio_push_period_ms = cli_parse_status_period_ms(args + 2);
		status_audio_push_enabled = true;
		next_status_audio_deadline_ms = k_uptime_get() + status_audio_push_period_ms;
		cli_print("OK status_audio=%u\n", status_audio_push_period_ms);
		return;
	}

	if (strcasecmp(args, "off") == 0) {
		status_audio_push_enabled = false;
		cli_print("OK status_audio=off\n");
		return;
	}

	cli_print("ERR status_audio invalid_value\n");
}

static void cli_cmd_fll(char *args)
{
	long rate;
	int32_t lo, hi;
	enum app_mode mode = app_control_get_current_mode();

	if (mode == APP_MODE_PROP_DONGLE) {
		cli_print("ERR fll not available for dongle mode\n");
		return;
	}

	if (args == NULL || *args == '\0' || strcasecmp(args, "auto") == 0) {
		fll_set_auto();
		cli_print("OK fll=auto (P-controller running)\n");
		return;
	}

	if (strcasecmp(args, "status") == 0) {
		int32_t fixed = fll_get_fixed_rate();

		if (fixed != 0) {
			cli_print("OK fll=fixed target_rate=%d\n", fixed);
		} else {
			cli_print("OK fll=auto\n");
		}
		return;
	}

	lo = (int32_t)(AUDIO_I2S_SAMPLE_RATE_HZ - FLL_ADJUST_MAX_HZ);
	hi = (int32_t)(AUDIO_I2S_SAMPLE_RATE_HZ + FLL_ADJUST_MAX_HZ);

	rate = strtol(args, NULL, 10);
	if (rate < lo || rate > hi) {
		cli_print("ERR fll rate %ld out of range (%d-%d)\n", rate, lo, hi);
		return;
	}

	if (fll_set_fixed((int32_t)rate)) {
		cli_print("OK fll=fixed target_rate=%ld (P-controller paused)\n", rate);
	} else {
		cli_print("ERR fll driver rejected rate %ld\n", rate);
	}
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

	if (cli_echo_enabled) {
		cli_print("%s%s%s\n", cmd, args ? " " : "", args ? args : "");
	}

	if (strcasecmp(cmd, "help") == 0) {
		cli_print_help();
		return;
	}

	if (strcasecmp(cmd, "echo") == 0) {
		if (args == NULL || strcasecmp(args, "on") == 0) {
			cli_echo_enabled = true;
			cli_print("OK echo=on\n");
		} else if (strcasecmp(args, "off") == 0) {
			cli_echo_enabled = false;
			cli_print("OK echo=off\n");
		} else {
			cli_print("ERR echo invalid_value\n");
		}
		return;
	}

	if (strcasecmp(cmd, "status_link") == 0) {
		cli_cmd_status_link(args);
		return;
	}

	if (strcasecmp(cmd, "status_audio") == 0) {
		cli_cmd_status_audio(args);
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

	if (strcasecmp(cmd, "linktest") == 0) {
		cli_cmd_linktest(args);
		return;
	}

	if (strcasecmp(cmd, "scan") == 0) {
		cli_print("ERR %s unsupported\n", cmd);
		return;
	}

	if (strcasecmp(cmd, "reset") == 0) {
		cli_print("OK reset\n");
		sys_reboot(SYS_REBOOT_COLD);
		return;
	}

	if (strcasecmp(cmd, "fll") == 0) {
		cli_cmd_fll(args);
		return;
	}

	cli_print("ERR %s unknown_command\n", cmd);
}

static void cli_init(void)
{
	cli_echo_enabled = true;
	status_link_push_enabled = false;
	status_audio_push_enabled = false;
	status_link_push_period_ms = 500;
	status_audio_push_period_ms = 500;
	next_status_link_deadline_ms = 0;
	next_status_audio_deadline_ms = 0;
	k_msgq_purge(&cli_msgq);
	k_msgq_purge(&cli_output_msgq);
}

void cli_set_connected(bool connected)
{
	bool announce = connected && !cli_connected;

	cli_connected = connected;

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

	len = cli_strnlen(line, CLI_MAX_CMD_LEN - 1U);
	memcpy(queued_line, line, len);
	queued_line[len] = '\0';
	/* Drop on overflow rather than blocking the USB path. */
	(void)k_msgq_put(&cli_msgq, queued_line, K_NO_WAIT);
}

void cli_enqueue_print_msg(const char *message)
{
	char queued_message[CLI_MAX_OUTPUT_LEN];
	size_t len;

	if (message == NULL) {
		return;
	}

	len = cli_strnlen(message, CLI_MAX_OUTPUT_LEN - 1U);
	memcpy(queued_message, message, len);
	queued_message[len] = '\0';
	(void)k_msgq_put(&cli_output_msgq, queued_message, K_NO_WAIT);
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
		if (k_msgq_get(&cli_msgq, line, K_MSEC(10)) == 0) {
			cli_process_line(line);
		}

		if (k_msgq_get(&cli_output_msgq, message, K_NO_WAIT) == 0) {
			cli_write_raw(message, strlen(message));
		}

		if (status_link_push_enabled && cli_connected && k_uptime_get() >= next_status_link_deadline_ms) {
			cli_emit_status_link_push();
			next_status_link_deadline_ms = k_uptime_get() + status_link_push_period_ms;
		}

		if (status_audio_push_enabled && cli_connected && k_uptime_get() >= next_status_audio_deadline_ms) {
			cli_emit_status_audio_push();
			next_status_audio_deadline_ms = k_uptime_get() + status_audio_push_period_ms;
		}
	}
}

K_THREAD_DEFINE(cli_thread_id, CLI_THREAD_STACK_SIZE, cli_thread, NULL, NULL, NULL, CLI_THREAD_PRIORITY, 0, 0);
