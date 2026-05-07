/*
 * FlexAudioLink nRF54 firmware
 *
 * Subsystems self-initialize via K_THREAD_DEFINE; main runs the boot
 * mode selection that starts the audio path for the persisted mode.
 */

#include "app_control.h"

int main(void)
{
	app_control_boot();
	return 0;
}
