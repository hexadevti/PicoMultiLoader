// timesync.h - set the clock (clock.h) from the web while Wi-Fi is up as a station
#pragma once

/** Sync when due (at once, then every few hours). Call from webserver_poll() while the link is up. */
void timesync_poll(void);

/** Drop any request in flight (Wi-Fi is going down); the next poll syncs again. */
void timesync_stop(void);
