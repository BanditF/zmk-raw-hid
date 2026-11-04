#pragma once

#include <stdint.h>

// The senders' side of raw_hid/stats.h.
// Each transport counts its own; raw_hid_stats_get adds them up for the totals.
enum raw_hid_stat {
    RAW_HID_STAT_USB_SENT,
    RAW_HID_STAT_QUEUED,
    RAW_HID_STAT_DROPPED,
    RAW_HID_STAT_USB_FAILED,
    RAW_HID_STAT_STUCK,
    RAW_HID_STAT_BLE_SENT,
    RAW_HID_STAT_BLE_FAILED,
    RAW_HID_STAT_BLE_SKIPPED,
    RAW_HID_STAT_COUNT,
};

void raw_hid_stat_inc(enum raw_hid_stat s);
// n at once (a ring emptied by a USB reset); n is at most the ring's size.
void raw_hid_stat_add(enum raw_hid_stat s, uint32_t n);
