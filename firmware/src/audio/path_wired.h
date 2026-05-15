#pragma once

#include "audio/path_common.h"

void path_wired_init(void);
void path_wired_get_status(struct codec_path_status *out);
void path_wired_wake_thread(void);