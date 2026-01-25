#ifndef _WIFI_APP_H_
#define _WIFI_APP_H_

#include "udp_tasks.h"
#include "wpl.h"
#include "wlan.h"
#include "pin_mux.h"
#include "mode.h"

// Event Bits
#define WIFI_EVENT_IP_ACQUIRED  (1 << 0) // IP Acquired (AP or STA)

void wifi_task(void *pvParameters);

#endif /* _WIFI_APP_H_ */