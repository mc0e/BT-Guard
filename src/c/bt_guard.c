#include <pebble.h>

// ---------------------------------------------------------------------
// Foreground app.
//
// One screen: a menu of alert repeat intervals (30 sec / 5 min /
// 20 min / 1 hour / None), with the active one marked. This is the
// same screen whether the worker just launched us because the phone
// link dropped, or you opened the app yourself.
//
// While disconnected, this screen buzzes on the selected schedule for
// as long as it stays open - the app owns its own repeat timer rather
// than depending on the worker to relaunch it for every buzz, so
// alerting keeps going while you're looking at the menu. Picking a
// different row takes effect immediately, both here and for the
// worker's background schedule (once you leave).
//
// If the phone reconnects while this screen is showing, we swap the
// menu out for a brief "connection re-established" message and then
// pop the window ourselves.
//
// The system Back button is the standard way to leave the screen;
// there's no custom "dismiss" handler otherwise - Select just picks a
// row.
// ---------------------------------------------------------------------

#define PERSIST_KEY_INTERVAL_INDEX 100   // must match worker.c
#define MSG_TYPE_SET_INTERVAL 1          // must match worker.c
#define NUM_INTERVAL_OPTIONS 5           // must match worker.c
#define NONE_INDEX (NUM_INTERVAL_OPTIONS - 1)

// How long the "connection re-established" message stays up before we
// pop the window ourselves.
#define RECONNECT_DISMISS_MS 1500

// Shortest first, "None" last.
static const char * const s_interval_labels[NUM_INTERVAL_OPTIONS] = {
  "30 sec", "5 min", "20 min", "1 hour", "None"
};
// Milliseconds for each option; 0 for "None" means "don't repeat".
static const uint32_t s_interval_ms[NUM_INTERVAL_OPTIONS] = {
  30 * 1000, 5 * 60 * 1000, 20 * 60 * 1000, 60 * 60 * 1000, 0
};

static Window *s_window;
static MenuLayer *s_menu_layer;
static TextLayer *s_reconnect_layer;

static int s_active_index;      // persisted "current" choice, marked in the menu
static AppTimer *s_alert_timer; // drives repeat buzzing while this screen is open

static bool s_was_connected;    // previous connection state, to detect the edge
static AppTimer *s_dismiss_timer; // pops the window after the reconnect message

static int clamp_index(int idx) {
  if (idx < 0) return 0;
  if (idx >= NUM_INTERVAL_OPTIONS) return NUM_INTERVAL_OPTIONS - 1;
  return idx;
}

static void vibrate_alert(void) {
  static const uint32_t segments[] = { 100, 200, 100, 200, 100, 200, 100, 200, 300 };
  VibePattern pattern = {
    .durations = segments,
    .num_segments = ARRAY_LENGTH(segments),
  };
  vibes_enqueue_custom_pattern(pattern);
}

static void ensure_worker_running(void) {
  if (!app_worker_is_running()) {
    app_worker_launch();
  }
}

static void notify_worker_of_interval(int idx) {
  AppWorkerMessage msg = { .data0 = (uint16_t) idx };
  app_worker_send_message(MSG_TYPE_SET_INTERVAL, &msg);
}

// ---- Repeat-buzz timer, active only while this screen is open --------

static void alert_tick(void *data) {
  if (!connection_service_peek_pebble_app_connection() && s_interval_ms[s_active_index] > 0) {
    vibrate_alert();
    s_alert_timer = app_timer_register(s_interval_ms[s_active_index], alert_tick, NULL);
  } else {
    s_alert_timer = NULL;
  }
}

static void restart_alert_timer(void) {
  if (s_alert_timer) {
    app_timer_cancel(s_alert_timer);
    s_alert_timer = NULL;
  }
  if (!connection_service_peek_pebble_app_connection() && s_interval_ms[s_active_index] > 0) {
    s_alert_timer = app_timer_register(s_interval_ms[s_active_index], alert_tick, NULL);
  }
}

// ---- Reconnect message / auto-dismiss ---------------------------------

static void dismiss_after_reconnect(void *data) {
  s_dismiss_timer = NULL;
  window_stack_pop(true);
}

static void show_reconnect_message(void) {
  if (s_dismiss_timer) {
    return; // already showing the message and counting down
  }

  layer_set_hidden(menu_layer_get_layer(s_menu_layer), true);

  if (!s_reconnect_layer) {
    GRect bounds = layer_get_bounds(window_get_root_layer(s_window));
    s_reconnect_layer = text_layer_create(bounds);
    text_layer_set_background_color(s_reconnect_layer, GColorWhite);
    text_layer_set_text_color(s_reconnect_layer, GColorBlack);
    text_layer_set_font(s_reconnect_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
    text_layer_set_text_alignment(s_reconnect_layer, GTextAlignmentCenter);
    text_layer_set_overflow_mode(s_reconnect_layer, GTextOverflowModeWordWrap);
    text_layer_set_text(s_reconnect_layer, "Connection re-established");
    layer_add_child(window_get_root_layer(s_window), text_layer_get_layer(s_reconnect_layer));
  } else {
    layer_set_hidden(text_layer_get_layer(s_reconnect_layer), false);
  }

  s_dismiss_timer = app_timer_register(RECONNECT_DISMISS_MS, dismiss_after_reconnect, NULL);
}

static void connection_handler(bool connected) {
  // Stop nagging the moment we reconnect; if it drops again while this
  // screen is still open, resume on whatever interval is selected.
  restart_alert_timer();

  if (connected && !s_was_connected) {
    // We just came back from an outage while this screen was open -
    // say so, then dismiss ourselves shortly after.
    show_reconnect_message();
  }
  s_was_connected = connected;
}

// ---- Menu callbacks ----------------------------------------------------

static uint16_t get_num_rows(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  return NUM_INTERVAL_OPTIONS;
}

static int16_t get_header_height(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  return 22;
}

static void draw_header(GContext *ctx, const Layer *cell_layer, uint16_t section_index, void *context) {
  GRect bounds = layer_get_bounds(cell_layer);
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, "Alert every:", fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                      GRect(4, 2, bounds.size.w - 8, 20),
                      GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static int16_t get_cell_height(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  return 46;
}

static void draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *context) {
  uint16_t row = cell_index->row;
  GRect bounds = layer_get_bounds(cell_layer);

  char buf[24];
  snprintf(buf, sizeof(buf), "%s%s", (row == (uint16_t) s_active_index) ? "> " : "  ",
           s_interval_labels[row]);

  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, buf, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD),
                      GRect(4, 6, bounds.size.w - 8, bounds.size.h - 6),
                      GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void select_click(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  uint16_t row = cell_index->row;
  if (row == (uint16_t) s_active_index) {
    return; // already active
  }
  s_active_index = row;
  persist_write_int(PERSIST_KEY_INTERVAL_INDEX, s_active_index);
  notify_worker_of_interval(s_active_index);
  restart_alert_timer();
  layer_mark_dirty(menu_layer_get_layer(s_menu_layer));
}

// ---- Window lifecycle ---------------------------------------------------

static void window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  s_menu_layer = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks) {
    .get_num_rows = get_num_rows,
    .get_header_height = get_header_height,
    .draw_header = draw_header,
    .get_cell_height = get_cell_height,
    .draw_row = draw_row,
    .select_click = select_click,
  });
  menu_layer_set_click_config_onto_window(s_menu_layer, window);
  menu_layer_set_selected_index(s_menu_layer, MenuIndex(s_active_index, 0), MenuRowAlignCenter, false);
  menu_layer_set_highlight_colors 	( s_menu_layer, GColorCyan, GColorBlack );
  layer_add_child(window_layer, menu_layer_get_layer(s_menu_layer));
}

static void window_unload(Window *window) {
  menu_layer_destroy(s_menu_layer);
  if (s_reconnect_layer) {
    text_layer_destroy(s_reconnect_layer);
    s_reconnect_layer = NULL;
  }
}

static void init(void) {
  s_active_index = clamp_index(persist_read_int(PERSIST_KEY_INTERVAL_INDEX));

  ensure_worker_running();
  s_was_connected = connection_service_peek_pebble_app_connection();
  connection_service_subscribe((ConnectionHandlers) {
    .pebble_app_connection_handler = connection_handler,
  });

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);

  if (launch_reason() == APP_LAUNCH_WORKER) {
    // Fresh alert - buzz right away.
    vibrate_alert();
  }
  // Begin (or resume) repeat buzzing while this screen stays open, if
  // we're actually disconnected and a repeat interval is selected.
  restart_alert_timer();
}

static void deinit(void) {
  if (s_alert_timer) {
    app_timer_cancel(s_alert_timer);
  }
  if (s_dismiss_timer) {
    app_timer_cancel(s_dismiss_timer);
  }
  connection_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}