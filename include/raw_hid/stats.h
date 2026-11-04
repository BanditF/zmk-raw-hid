#pragma once

/*
 * What the Raw HID senders did since boot (BanditF/foxglove#705, #680): cheap counters a consumer
 * can read for a self-report. Each is a saturating count kept by the senders as reports pass;
 * reading takes a copy and changes nothing. A consumer that builds against an older copy of this
 * module can test for this header with __has_include(<raw_hid/stats.h>).
 */

#include <stdint.h>

// Version 2 counts each transport on its own (the fields after stuck) and no longer counts a BLE
// report nobody listens for as failed: that is skipped.
#define RAW_HID_STATS_VERSION 2

struct raw_hid_stats {
    // Reports a transport accepted: USB transfers the driver took, BLE notifications queued
    // (usb_sent + ble_sent).
    uint32_t sent;
    // USB reports that waited in the ring because a transfer was in flight.
    uint32_t queued;
    // USB reports that waited and were dropped: the ring was full (its oldest goes), or a USB
    // reset or disconnect emptied it.
    uint32_t dropped;
    // Reports a transport refused (the USB driver's write, a BLE notify on a link that listens):
    // that report is lost (usb_failed + ble_failed).
    uint32_t failed;
    // USB transfers taken as lost after STUCK_MS with no done callback (the next went past them).
    uint32_t stuck;
    // Version 2: per transport.
    uint32_t usb_sent;
    uint32_t usb_failed;
    uint32_t ble_sent;
    uint32_t ble_failed;
    // BLE reports not sent because no PC listens there: no link on the active profile, the PC
    // hasn't turned the report's notifications on, or the link isn't encrypted yet. Normal while
    // the PC uses USB; not a fault.
    uint32_t ble_skipped;
};

// Copies the counters into out. Any thread.
void raw_hid_stats_get(struct raw_hid_stats *out);
