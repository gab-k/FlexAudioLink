#include "cli.h"

#include "app_control.h"
#include "audio_io/i2s.h"
#include "audio_io/i2s_tone.h"
#include "audio_io/audio_path_common.h"
#include "audio_io/audio_path_wired.h"
#include "audio_io/audio_path_wireless.h"
#include "prop_gfsk/link.h"
#include "prop_gfsk/test_mode.h"

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
static bool g_status_link_push_enabled;
static bool g_status_audio_push_enabled;
static uint32_t g_status_link_push_period_ms = 500;
static uint32_t g_status_audio_push_period_ms = 500;
static int64_t g_next_status_link_deadline_ms;
static int64_t g_next_status_audio_deadline_ms;
/* Complete command lines are queued here and executed by the low-priority CLI thread. */
K_MSGQ_DEFINE(g_cli_msgq, CLI_MAX_CMD_LEN, CLI_QUEUE_DEPTH, 4);
/* Async user-visible messages from other threads are serialized here. */
K_MSGQ_DEFINE(g_cli_output_msgq, CLI_MAX_OUTPUT_LEN, CLI_QUEUE_DEPTH, 4);

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
	struct pgfsk_link_report stats;
	uint32_t loss_permille = 0U;

	pgfsk_link_get_report(&stats);

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
	enum app_profile profile = app_control_get_current_profile();

	switch (profile) {
	case APP_PROFILE_USB: {
		struct audio_path_wired_status s;

		audio_path_wired_get_status(&s);
		cli_print("#A active=%s state=%s spk_level_bytes=%u "
			  "spk_fifo_bytes=%u spk_pending_bytes=%u "
			  "spk_filtered_level_bytes=%u spk_error_bytes=%d spk_p_adjust_hz=%d "
			  "mic_level_bytes=%u mic_overruns=%u\n",
			  "wired",
			  audio_path_get_state_name(s.stream_state),
			  s.spk_level_bytes,
			  s.spk_fifo_bytes,
			  s.spk_pending_bytes,
			  s.spk_filtered_level_bytes,
			  s.spk_error_bytes,
			  s.spk_p_adjust_hz,
			  s.mic_level_bytes,
			  s.mic_overflow_bytes);
		return;
	}
	case APP_PROFILE_PGFSK_DONGLE: {
		struct audio_path_wireless_status s;

		audio_path_wireless_get_status(&s);
		cli_print("#A active=%s state=%s spk_level_bytes=%u "
			  "peer_adjust_hz=%d spk_underruns=%u overruns=%u spk_silence_bytes=%u "
			  "spk_dropped_oldest_bytes=%u spk_usb_level_bytes=%u mic_usb_level_bytes=%u "
			  "rx_malformed_frames=%u\n",
			  "wireless",
			  audio_path_get_state_name(s.stream_state),
			  s.spk_level_bytes,
			  s.peer_adjust_hz,
			  s.spk_underrun_bytes,
			  s.overflow_bytes,
			  s.spk_silence_inserted_bytes,
			  s.spk_dropped_oldest_bytes,
			  s.spk_usb_level_bytes,
			  s.mic_usb_level_bytes,
			  s.rx_malformed_frames);
		return;
	}
	case APP_PROFILE_PGFSK_HEADSET: {
		struct audio_path_wireless_status s;

		audio_path_wireless_get_status(&s);
		cli_print("#A active=%s state=%s spk_level_bytes=%u "
			  "spk_p_adjust_hz=%d spk_underruns=%u overruns=%u spk_silence_bytes=%u "
			  "spk_dropped_oldest_bytes=%u rx_malformed_frames=%u\n",
			  "wireless",
			  audio_path_get_state_name(s.stream_state),
			  s.spk_level_bytes,
			  s.spk_p_adjust_hz,
			  s.spk_underrun_bytes,
			  s.overflow_bytes,
			  s.spk_silence_inserted_bytes,
			  s.spk_dropped_oldest_bytes,
			  s.rx_malformed_frames);
		return;
	}
	default:
		cli_print("#A active=none\n");
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

static void cli_print_profile_group(void)
{
	cli_print("[profile]\n");
	cli_print("profile=%s\n", app_control_get_profile_name(app_control_get_current_profile()));
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

	switch (app_control_get_current_profile()) {
	case APP_PROFILE_USB:
		audio_io = "wired";
		break;
	case APP_PROFILE_PGFSK_DONGLE:
		audio_io = "usb";
		break;
	case APP_PROFILE_PGFSK_HEADSET:
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
	cli_print("  set profile <usb|pgfsk_dongle|pgfsk_headset>\n");
	cli_print("  i2s tone on|off|status\n");
	cli_print("  linktest on|off|status\n");
	cli_print("  status_link on [ms]|off\n");
	cli_print("  status_audio on [ms]|off\n");
	cli_print("  reset\n");
	cli_print("  scan\n");
}

static void cli_cmd_get(const char *arg)
{
	if (arg == NULL || *arg == '\0' || strcasecmp(arg, "all") == 0) {
		cli_print_audio_group();
		cli_print_radio_group();
		cli_print_profile_group();
		cli_print_device_group();
		cli_print_eq_group();
		return;
	}

	if (strcasecmp(arg, "profile") == 0) {
		cli_print_profile_group();
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

	if (strcasecmp(param, "profile") == 0) {
		bool ok;
		enum app_profile profile;

		if (strcasecmp(value, "usb") == 0) {
			profile = APP_PROFILE_USB;
		} else if (strcasecmp(value, "pgfsk_dongle") == 0) {
			profile = APP_PROFILE_PGFSK_DONGLE;
		} else if (strcasecmp(value, "pgfsk_headset") == 0) {
			profile = APP_PROFILE_PGFSK_HEADSET;
		} else {
			cli_print("ERR profile invalid_value\n");
			return;
		}

		ok = app_control_set_profile(profile);

		if (!ok) {
			cli_print("ERR profile rejected\n");
			return;
		}

		cli_print("OK profile=%s\n", app_control_get_profile_name(profile));
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
		cli_print("i2s ready=%s tone=%s tone_blocks=%u\n",
			  audio_i2s_is_ready() ? "yes" : "no",
			  audio_i2s_tone_is_enabled() ? "on" : "off",
			  audio_i2s_tone_get_enqueued_blocks());
		return;
	}

	if (strcasecmp(value, "on") == 0) {
		if (!audio_i2s_is_ready()) {
			cli_print("ERR i2s not_ready\n");
			return;
		}

		audio_i2s_tone_set_enabled(true);
		cli_print("OK i2s tone=on\n");
		return;
	}

	if (strcasecmp(value, "off") == 0) {
		audio_i2s_tone_set_enabled(false);
		cli_print("OK i2s tone=off\n");
		return;
	}

	cli_print("ERR i2s invalid_value\n");
}

static void cli_cmd_linktest(char *args)
{
	if (args == NULL || *args == '\0' || strcasecmp(args, "status") == 0) {
		cli_print("linktest=%s\n", pgfsk_test_mode_is_running() ? "on" : "off");
		return;
	}

	if (strcasecmp(args, "on") == 0) {
		if (!pgfsk_test_mode_start()) {
			cli_print("ERR linktest start_failed\n");
			return;
		}

		cli_print("OK linktest=on\n");
		return;
	}

	if (strcasecmp(args, "off") == 0) {
		if (!pgfsk_test_mode_stop()) {
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
		g_status_link_push_period_ms = cli_parse_status_period_ms(args + 2);
		g_status_link_push_enabled = true;
		g_next_status_link_deadline_ms = k_uptime_get() + g_status_link_push_period_ms;
		cli_print("OK status_link=%u\n", g_status_link_push_period_ms);
		return;
	}

	if (strcasecmp(args, "off") == 0) {
		g_status_link_push_enabled = false;
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
		g_status_audio_push_period_ms = cli_parse_status_period_ms(args + 2);
		g_status_audio_push_enabled = true;
		g_next_status_audio_deadline_ms = k_uptime_get() + g_status_audio_push_period_ms;
		cli_print("OK status_audio=%u\n", g_status_audio_push_period_ms);
		return;
	}

	if (strcasecmp(args, "off") == 0) {
		g_status_audio_push_enabled = false;
		cli_print("OK status_audio=off\n");
		return;
	}

	cli_print("ERR status_audio invalid_value\n");
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

	if (strcasecmp(cmd, "i2s") == 0) {
		cli_cmd_i2s(args);
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

	cli_print("ERR %s unknown_command\n", cmd);
}

static void cli_init(void)
{
	g_cli_echo_enabled = true;
	g_status_link_push_enabled = false;
	g_status_audio_push_enabled = false;
	g_status_link_push_period_ms = 500;
	g_status_audio_push_period_ms = 500;
	g_next_status_link_deadline_ms = 0;
	g_next_status_audio_deadline_ms = 0;
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

	len = cli_strnlen(line, CLI_MAX_CMD_LEN - 1U);
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

	len = cli_strnlen(message, CLI_MAX_OUTPUT_LEN - 1U);
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

		if (g_status_link_push_enabled && g_cli_connected && k_uptime_get() >= g_next_status_link_deadline_ms) {
			cli_emit_status_link_push();
			g_next_status_link_deadline_ms = k_uptime_get() + g_status_link_push_period_ms;
		}

		if (g_status_audio_push_enabled && g_cli_connected && k_uptime_get() >= g_next_status_audio_deadline_ms) {
			cli_emit_status_audio_push();
			g_next_status_audio_deadline_ms = k_uptime_get() + g_status_audio_push_period_ms;
		}
	}
}

K_THREAD_DEFINE(cli_thread_id, CLI_THREAD_STACK_SIZE, cli_thread, NULL, NULL, NULL, CLI_THREAD_PRIORITY, 0, 0);
