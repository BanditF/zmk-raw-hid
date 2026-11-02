#pragma once

/*
 * What the Raw HID senders did since boot (BanditF/foxglove#705, #680): cheap counters a consumer
 * can read for a self-report. Each is a saturating count kept by the senders as reports pass;
 * reading takes a copy and changes nothing. A consumer that builds against an older copy of this
 * module can test for this header with __has_include(<raw_hid/stats.h>).
 */

#include <stdint.h>

#define RAW_HID_STATS_VERSION 1

struct raw_hid_stats {
    // Reports a transport accepted: USB transfers the driver took, BLE notifications queued.
    uint32_t sent;
    // USB reports that waited in the ring because a transfer was in flight.
    uint32_t queued;
    // USB reports that waited and were dropped: the ring was full (its oldest goes), or a USB
    // reset or disconnect emptied it.
    uint32_t dropped;
    // Reports a transport refused (the USB driver's write, a BLE notify): that report is lost.
    uint32_t failed;
    // USB transfers taken as lost after STUCK_MS with no done callback (the next went past them).
    uint32_t stuck;
};

// Copies the counters into out. Any thread.
void raw_hid_stats_get(struct raw_hid_stats *out);
