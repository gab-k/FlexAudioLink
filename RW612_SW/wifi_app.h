#ifndef _WIFI_APP_H_
#define _WIFI_APP_H_

#include "udp_tasks.h"
#include "wpl.h"
#include "wlan.h"
#include "pin_mux.h"

void wifi_task(void *pvParameters);

#endif /* _WIFI_APP_H_ */