// dhcpserver.h - hands out addresses to the devices that join our own access point
#pragma once

#include <stdbool.h>
#include "lwip/netif.h"

/** Serve 192.168.4.16-23 (for an AP at 192.168.4.1/24) on `netif`. `joined` runs on each new lease. */
bool dhcpserver_start(struct netif *netif, void (*joined)(void));

void dhcpserver_stop(void);
