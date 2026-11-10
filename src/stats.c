#include <raw_hid/stats.h>

#include "stats_internal.h"

#include <zephyr/sys/atomic.h>

static atomic_t counts[RAW_HID_STAT_COUNT];
static atomic_t usb_last_err;
static atomic_t ble_last_err;

void raw_hid_stat_inc(enum raw_hid_stat s) {
    atomic_val_t v;
    do {
        v = atomic_get(&counts[s]);
        if (v >= 0x7fffffff) {
            return; // saturates
        }
    } while (!atomic_cas(&counts[s], v, v + 1));
}

void raw_hid_stat_failed(enum raw_hid_stat s, int err) {
    atomic_set(s == RAW_HID_STAT_BLE_FAILED ? &ble_last_err : &usb_last_err, err);
    raw_hid_stat_inc(s);
}

void raw_hid_stat_add(enum raw_hid_stat s, uint32_t n) {
    while (n--) {
        raw_hid_stat_inc(s);
    }
}

static uint32_t get(enum raw_hid_stat s) { return (uint32_t)atomic_get(&counts[s]); }

void raw_hid_stats_get(struct raw_hid_stats *out) {
    out->usb_sent = get(RAW_HID_STAT_USB_SENT);
    out->usb_failed = get(RAW_HID_STAT_USB_FAILED);
    out->ble_sent = get(RAW_HID_STAT_BLE_SENT);
    out->ble_failed = get(RAW_HID_STAT_BLE_FAILED);
    out->ble_skipped = get(RAW_HID_STAT_BLE_SKIPPED);
    out->ble_early = get(RAW_HID_STAT_BLE_EARLY);
    // Each part saturates at 0x7fffffff, so the sums fit.
    out->sent = out->usb_sent + out->ble_sent;
    out->queued = get(RAW_HID_STAT_QUEUED);
    out->dropped = get(RAW_HID_STAT_DROPPED);
    out->failed = out->usb_failed + out->ble_failed;
    out->stuck = get(RAW_HID_STAT_STUCK);
    out->usb_last_err = (int32_t)atomic_get(&usb_last_err);
    out->ble_last_err = (int32_t)atomic_get(&ble_last_err);
}
