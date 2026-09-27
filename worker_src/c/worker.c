#include <pebble_worker.h>

// ---------------------------------------------------------------------
// Background worker.
//
// Runs independently of any foreground app or watchface. Subscribes to
// the connection service; whenever the phone link drops, it launches
// the foreground app (bt_guard.c) so it can vibrate and show the
// interval-picker screen - workers have no UI/Vibes access of their
// own, only Foundation-level services like ConnectionService, Timer,
// and Storage.
//
// While the app stays open it takes over the actual repeat-buzzing
// itself (see bt_guard.c), so this worker's own timer here is really
// a fallback: it keeps relaunching the app on the selected interval in
// case the app got closed but the outage hasn't been resolved yet.
// Relaunching an app that's already in the foreground is a harmless
// no-op, so there's no need to track whether the app is currently
// open from here.
// ---------------------------------------------------------------------

#define PERSIST_KEY_INTERVAL_INDEX 100   // must match bt_guard.c
#define MSG_TYPE_SET_INTERVAL 1          // must match bt_guard.c
#define NUM_INTERVAL_OPTIONS 5           // must match bt_guard.c
#define NONE_INDEX (NUM_INTERVAL_OPTIONS - 1)

// 30 sec / 5 min / 20 min / 1 hour / None, in milliseconds.
// 0 for "None" means "don't repeat".
static const uint32_t s_interval_ms[NUM_INTERVAL_OPTIONS] = {
  30 * 1000, 5 * 60 * 1000, 20 * 60 * 1000, 60 * 60 * 1000, 0
};

static AppTimer *s_retry_timer;
static bool s_connected = true;
static uint32_t s_current_interval_ms = 30 * 1000;

static void alert_and_reschedule(void *data); // forward decl, defined below

static void set_interval_index(int idx) {
  if (idx < 0 || idx >= NUM_INTERVAL_OPTIONS) {
    return;
  }
  s_current_interval_ms = s_interval_ms[idx];
  persist_write_int(PERSIST_KEY_INTERVAL_INDEX, idx);

  // If an alert is already pending, apply the new interval right away
  // rather than waiting out whatever was previously scheduled.
  if (s_retry_timer) {
    app_timer_cancel(s_retry_timer);
    s_retry_timer = NULL;
    if (s_current_interval_ms > 0) {
      s_retry_timer = app_timer_register(s_current_interval_ms, alert_and_reschedule, NULL);
    }
  }
}

static void alert_and_reschedule(void *data) {
  // Re-check current state in case we reconnected between the timer
  // firing and this callback running.
  if (!s_connected) {
    worker_launch_app();
    if (s_current_interval_ms > 0) {
      s_retry_timer = app_timer_register(s_current_interval_ms, alert_and_reschedule, NULL);
    } else {
      s_retry_timer = NULL; // "None" selected - don't reschedule
    }
  } else {
    s_retry_timer = NULL;
  }
}

static void start_alerting(void) {
  if (s_retry_timer) {
    return; // already alerting on schedule
  }
  // Every fresh outage starts by grabbing your attention quickly; from
  // the alert screen you can then pick a slower pace (or None).
  set_interval_index(0);
  worker_launch_app();
  s_retry_timer = app_timer_register(s_current_interval_ms, alert_and_reschedule, NULL);
}

static void stop_alerting(void) {
  if (s_retry_timer) {
    app_timer_cancel(s_retry_timer);
    s_retry_timer = NULL;
  }
}

static void bt_handler(bool connected) {
  s_connected = connected;
  if (connected) {
    stop_alerting();
  } else {
    start_alerting();
  }
}

static void worker_message_handler(uint16_t type, AppWorkerMessage *message) {
  if (type == MSG_TYPE_SET_INTERVAL) {
    set_interval_index(message->data0);
  }
}

static void worker_init(void) {
  s_connected = connection_service_peek_pebble_app_connection();
  connection_service_subscribe((ConnectionHandlers) {
    .pebble_app_connection_handler = bt_handler,
  });
  app_worker_message_subscribe(worker_message_handler);
  if (!s_connected) {
    start_alerting();
  }
}

static void worker_deinit(void) {
  stop_alerting();
  connection_service_unsubscribe();
}

int main(void) {
  worker_init();
  worker_event_loop();
  worker_deinit();
}