#include <raw_hid/raw_hid.h>
#include <raw_hid/events.h>

#include <zmk/usb.h>
#include <zmk/event_manager.h>
#include <zmk/events/usb_conn_state_changed.h>

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static const struct device *raw_hid_dev;

/*
 * One IN transfer at a time; the reports that come meanwhile wait in a small ring and go out
 * one by one after the endpoint's done callback (BanditF/foxglove#680: a key press sends three
 * reports back to back, and dropping the one that found the endpoint busy sometimes lost the
 * layer report). When the ring is full the oldest waiting report is dropped: the newest wins.
 *
 * A transfer the host never completes (the bus reset or suspended under it: a PC restart,
 * sleep, a replug of the PC side) never calls in_ready_cb, and every later report used to be
 * lost for good. Two ways out:
 * - a USB reset, disconnect or (re)configuration ends whatever was in flight
 *   (usb_state_listener); reset and disconnect also drop the waiting reports, no host reads them;
 * - failing that, a report that finds the transfer pending for STUCK_MS takes it as lost and
 *   goes from the other buffer.
 *
 * Start/stop, no timers: a report starts a transfer when none is in flight, else it waits in the
 * ring; each done callback sends the next waiting one (on the system work queue) and the chain
 * ends when the ring is empty; a USB status change ends a transfer in flight.
 */
#define STUCK_MS 100
#define QUEUE_LEN CONFIG_RAW_HID_QUEUE_SIZE

struct queued {
    uint8_t len;
    uint8_t data[CONFIG_RAW_HID_REPORT_SIZE];
};
static struct queued queue[QUEUE_LEN];
static uint8_t q_head, q_count; // the oldest waiting report, and how many wait
static bool in_flight;          // a transfer the driver accepted hasn't completed
static int64_t in_flight_since;
static struct k_spinlock lock;

/*
 * Static, not on the stack: the nRF USB device DMAs straight out of this buffer, and the transfer
 * outlives the call. A stack buffer sends its first bytes correctly and then whatever the stack
 * holds by then -- which looks like a report with a good header and RAM pointers in the tail.
 * Two of them, used in turn: a report sent past a stuck transfer (above) may find that transfer
 * still reading its buffer, so it fills the other one. The driver takes one transfer at a time,
 * so two are enough.
 */
static uint8_t reports[2][CONFIG_RAW_HID_REPORT_SIZE];
static int last; // the buffer of the last transfer the driver accepted

static bool usb_ready(void) {
#if IS_ENABLED(CONFIG_ZMK_USB)
    switch (zmk_usb_get_status()) {
    case USB_DC_ERROR:
    case USB_DC_RESET:
    case USB_DC_DISCONNECTED:
    case USB_DC_UNKNOWN:
        return false; // nobody to read it: don't queue a transfer that can't complete
    default:
        break;
    }
#endif
    return true;
}

// Starts one transfer; the caller has set in_flight. A refusal clears it and loses this report
// (the next report or done callback goes on with the ring).
static void write_report(const uint8_t *data, uint8_t len) {
    uint8_t *report = reports[!last];
    memset(report, 0, CONFIG_RAW_HID_REPORT_SIZE);
    memcpy(report, data, MIN(len, CONFIG_RAW_HID_REPORT_SIZE));
    LOG_HEXDUMP_DBG(report, CONFIG_RAW_HID_REPORT_SIZE, "USB - Sending Raw HID report");

    int err = hid_int_ep_write(raw_hid_dev, report, CONFIG_RAW_HID_REPORT_SIZE, NULL);
    k_spinlock_key_t key = k_spin_lock(&lock);
    if (err) {
        in_flight = false;
    } else {
        last = !last;
    }
    k_spin_unlock(&lock, key);
    if (err) {
        LOG_ERR("Failed to send report: %i", err);
    }
}

// Takes the oldest waiting report, if any, and marks a transfer in flight for it. Under lock.
static bool pop_locked(struct queued *out) {
    if (q_count == 0) {
        return false;
    }
    *out = queue[q_head];
    q_head = (q_head + 1) % QUEUE_LEN;
    q_count--;
    in_flight = true;
    in_flight_since = k_uptime_get();
    return true;
}

// Sends the oldest waiting report when nothing is in flight: after the done callback and after a
// USB (re)configuration. System work queue.
static void drain_cb(struct k_work *work) {
    if (!usb_ready()) {
        return;
    }
    struct queued next;
    k_spinlock_key_t key = k_spin_lock(&lock);
    bool go = !in_flight && pop_locked(&next);
    k_spin_unlock(&lock, key);
    if (go) {
        write_report(next.data, next.len);
    }
}
static K_WORK_DEFINE(drain_work, drain_cb);

static void in_ready_cb(const struct device *dev) {
    k_spinlock_key_t key = k_spin_lock(&lock);
    in_flight = false;
    bool more = q_count > 0;
    k_spin_unlock(&lock, key);
    if (more) {
        k_work_submit(&drain_work); // not from inside the USB driver's callback
    }
}

#define HID_GET_REPORT_TYPE_MASK 0xff00
#define HID_GET_REPORT_ID_MASK 0x00ff

#define HID_REPORT_TYPE_INPUT 0x100
#define HID_REPORT_TYPE_OUTPUT 0x200
#define HID_REPORT_TYPE_FEATURE 0x300

static int get_report_cb(const struct device *dev, struct usb_setup_packet *setup, int32_t *len,
                         uint8_t **data) {
    return 0;
}

static int set_report_cb(const struct device *dev, struct usb_setup_packet *setup, int32_t *len,
                         uint8_t **data) {
    if ((setup->wValue & HID_GET_REPORT_TYPE_MASK) != HID_REPORT_TYPE_OUTPUT &&
        (setup->wValue & HID_GET_REPORT_TYPE_MASK) != HID_REPORT_TYPE_FEATURE) {
        LOG_ERR("[# raw-hid #] Set: Unsupported report type %d requested",
                (setup->wValue & HID_GET_REPORT_TYPE_MASK) >> 8);
        return -ENOTSUP;
    }

    LOG_INF("USB - Received Raw HID report of length %i", *len);
    LOG_HEXDUMP_DBG(*data, *len, "USB - Received Raw HID report");
    raise_raw_hid_received_event((struct raw_hid_received_event){.data = *data, .length = *len});

    return 0;
}

static const struct hid_ops ops = {
    .int_in_ready = in_ready_cb,
    .get_report = get_report_cb,
    .set_report = set_report_cb,
};

static void send_report(const uint8_t *data, uint8_t len) {
    if (!usb_ready()) {
        return;
    }
    LOG_INF("USB - Sending Raw HID report of length %i", len);
    struct queued next;
    k_spinlock_key_t key = k_spin_lock(&lock);
    if (in_flight && k_uptime_get() - in_flight_since >= STUCK_MS) {
        // The transfer looks lost (the host never read it): the next one goes past it.
        LOG_WRN("USB - Raw HID transfer pending for %d ms, sending anyway", STUCK_MS);
        in_flight = false;
    }
    if (q_count == QUEUE_LEN) {
        LOG_WRN("USB - Raw HID queue full, dropping the oldest report");
        q_head = (q_head + 1) % QUEUE_LEN;
        q_count--;
    }
    struct queued *slot = &queue[(q_head + q_count) % QUEUE_LEN];
    slot->len = MIN(len, CONFIG_RAW_HID_REPORT_SIZE);
    memcpy(slot->data, data, slot->len);
    q_count++;
    // Oldest first: this report goes now only when nothing waits ahead of it.
    bool go = !in_flight && pop_locked(&next);
    k_spin_unlock(&lock, key);
    if (go) {
        write_report(next.data, next.len);
    }
}

static int raw_hid_sent_event_listener(const zmk_event_t *eh) {
    struct raw_hid_sent_event *event = as_raw_hid_sent_event(eh);
    if (event) {
        send_report(event->data, event->length);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(usb_process_raw_hid_sent_event, raw_hid_sent_event_listener);
ZMK_SUBSCRIPTION(usb_process_raw_hid_sent_event, raw_hid_sent_event);

#if IS_ENABLED(CONFIG_ZMK_USB)
static int usb_state_listener(const zmk_event_t *eh) {
    switch (zmk_usb_get_status()) {
    case USB_DC_RESET:
    case USB_DC_DISCONNECTED: {
        // Nothing in flight survives these, and no host will read what waits.
        k_spinlock_key_t key = k_spin_lock(&lock);
        in_flight = false;
        q_count = 0;
        k_spin_unlock(&lock, key);
        break;
    }
    case USB_DC_CONFIGURED: {
        // A (re)configuration ends a transfer in flight too; anything waiting goes now.
        k_spinlock_key_t key = k_spin_lock(&lock);
        in_flight = false;
        k_spin_unlock(&lock, key);
        k_work_submit(&drain_work);
        break;
    }
    default:
        break;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(usb_raw_hid_state, usb_state_listener);
ZMK_SUBSCRIPTION(usb_raw_hid_state, zmk_usb_conn_state_changed);
#endif

static int raw_hid_init(void) {
    raw_hid_dev = device_get_binding(CONFIG_RAW_HID_DEVICE);
    if (raw_hid_dev == NULL) {
        LOG_ERR("Unable to locate HID device");
        return -EINVAL;
    }

    usb_hid_register_device(raw_hid_dev, raw_hid_report_desc, sizeof(raw_hid_report_desc), &ops);

    usb_hid_init(raw_hid_dev);

    return 0;
}

SYS_INIT(raw_hid_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
