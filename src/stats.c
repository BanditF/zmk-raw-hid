#include <raw_hid/stats.h>

#include "stats_internal.h"

#include <zephyr/sys/atomic.h>

static atomic_t counts[RAW_HID_STAT_COUNT];

void raw_hid_stat_inc(enum raw_hid_stat s) {
    atomic_val_t v;
    do {
        v = atomic_get(&counts[s]);
        if (v >= 0x7fffffff) {
            return; // saturates
        }
    } while (!atomic_cas(&counts[s], v, v + 1));
}

void raw_hid_stat_add(enum raw_hid_stat s, uint32_t n) {
    while (n--) {
        raw_hid_stat_inc(s);
    }
}

void raw_hid_stats_get(struct raw_hid_stats *out) {
    out->sent = (uint32_t)atomic_get(&counts[RAW_HID_STAT_SENT]);
    out->queued = (uint32_t)atomic_get(&counts[RAW_HID_STAT_QUEUED]);
    out->dropped = (uint32_t)atomic_get(&counts[RAW_HID_STAT_DROPPED]);
    out->failed = (uint32_t)atomic_get(&counts[RAW_HID_STAT_FAILED]);
    out->stuck = (uint32_t)atomic_get(&counts[RAW_HID_STAT_STUCK]);
}
