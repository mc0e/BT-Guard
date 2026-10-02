#include <pebble.h>

// ---------------------------------------------------------------------
// Foreground app.
//
// Four screens, chosen at launch (and switched live while open) based
// on why we were started and the current connection state:
//
//   CONNECTED_LOCATE     - Shown when the user opens the app (launcher or
//                          quick launch) while the phone is connected.
//                          Same "Press Select to sound phone" screen as
//                          RECONNECTED_LOCATE, but no vibration. Auto-
//                          dismisses after 60s of no interaction.
//
//   SPLASH_OK           - "Connected, running" full-screen message.
//                          Auto-dismisses after ~1.5s. Shown when we're
//                          launched by anything other than the user or
//                          quick launch (system, phone, etc.) and the
//                          phone link is already up.
//
//   DISCONNECTED_MENU    - "Phone disconnected" banner over the alert
//                          interval menu (which also carries a "Sound
//                          phone" row). Shown whenever we're launched
//                          and the link is down, however we got here.
//                          Buzzes on the selected repeat interval for
//                          as long as this screen stays open.
//
//   RECONNECTED_LOCATE   - Shown when the link comes back: either live,
//                          while DISCONNECTED_MENU is open, or because
//                          the worker just launched us specifically to
//                          report a reconnect (see worker.c). Buzzes
//                          once with a distinct pattern, offers Select
//                          to make the phone sound (real AppMessage to
//                          a phone-side script - see src/pkjs), and
//                          auto-dismisses after 15s of no interaction, or
//                          60s if the user opened the app or has pressed
//                          anything since (each Select press resets it).
//
// The system Back button always leaves the screen immediately,
// regardless of state or timers.
// ---------------------------------------------------------------------

#define PERSIST_KEY_INTERVAL_INDEX 100   // must match worker.c
#define MSG_TYPE_SET_INTERVAL 1          // must match worker.c
#define NUM_INTERVAL_OPTIONS 5           // must match worker.c
#define NONE_INDEX (NUM_INTERVAL_OPTIONS - 1)

#define SPLASH_DISMISS_MS 1500
#define LOCATE_DISMISS_MS 15000          // untouched, not opened by the user
#define LOCATE_DISMISS_ENGAGED_MS 60000  // user opened the app, or has interacted
#define LOCATE_FEEDBACK_MS 1000

// Shortest first, "None" last.
static const char * const s_interval_labels[NUM_INTERVAL_OPTIONS] = {
  "30 sec", "5 min", "20 min", "1 hour", "None"
};
// Milliseconds for each option; 0 for "None" means "don't repeat".
static const uint32_t s_interval_ms[NUM_INTERVAL_OPTIONS] = {
  30 * 1000, 5 * 60 * 1000, 20 * 60 * 1000, 60 * 60 * 1000, 0
};

typedef enum {
  STATE_SPLASH_OK,
  STATE_CONNECTED_LOCATE,
  STATE_DISCONNECTED_MENU,
  STATE_RECONNECTED_LOCATE,
} AppScreenState;

static Window *s_window;
static TextLayer *s_splash_layer;
static TextLayer *s_status_layer;   // banner above the border, D/R states
static Layer *s_border_layer;       // rounded-rect frame, D/R states
static MenuLayer *s_menu_layer;     // D state content
static TextLayer *s_locate_layer;   // R state content

static AppScreenState s_state;
static AppLaunchReason s_launch_reason;
// True once the user has deliberately opened the app (launcher / quick
// launch) or pressed anything in it since it opened. Lengthens the
// locate-screen auto-dismiss from 15s to 60s.
static bool s_engaged;
static bool s_menu_ready;  // ignore menu selection callbacks during setup

static int s_active_index;      // persisted "current" choice, marked in the menu
static AppTimer *s_alert_timer; // repeat buzz while DISCONNECTED_MENU is open
static AppTimer *s_splash_timer;
static AppTimer *s_locate_timer;
static AppTimer *s_feedback_timer;

// Forward decl: defined in the "Border drawing" section below, but
// referenced from window_load() above it.
static void border_layer_update_proc(Layer *layer, GContext *ctx);

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

// Deliberately distinct from vibrate_alert()'s urgent pattern - a
// short double-buzz, since this means good news (link is back).
static void vibrate_reconnect(void) {
  static const uint32_t segments[] = { 100, 100, 100 };
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

// ---- "Sound phone" - real AppMessage to the phone-side script --------

static void outbox_failed_handler(DictionaryIterator *iter, AppMessageResult reason, void *context) {
  // Couldn't get the request to the phone at all (JS not ready, etc.) -
  // fall back to a vibrate so the button never feels dead.
  vibrate_alert();
}

static void request_phone_sound(void) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
    vibrate_alert();
    return;
  }
  dict_write_uint8(iter, MESSAGE_KEY_SOUND_PHONE, 1);
  if (app_message_outbox_send() != APP_MSG_OK) {
    vibrate_alert();
  }
}

// ---- Repeat-buzz timer, active only in DISCONNECTED_MENU --------------

static void alert_tick(void *data) {
  if (!connection_service_peek_pebble_app_connection() && s_interval_ms[s_active_index] > 0) {
    vibrate_alert();
    s_alert_timer = app_timer_register(s_interval_ms[s_active_index], alert_tick, NULL);
  } else {
    s_alert_timer = NULL;
  }
}

static void cancel_alert_timer(void) {
  if (s_alert_timer) {
    app_timer_cancel(s_alert_timer);
    s_alert_timer = NULL;
  }
}

static void restart_alert_timer(void) {
  cancel_alert_timer();
  if (!connection_service_peek_pebble_app_connection() && s_interval_ms[s_active_index] > 0) {
    s_alert_timer = app_timer_register(s_interval_ms[s_active_index], alert_tick, NULL);
  }
}

// ---- Splash auto-dismiss ------------------------------------------------

static void dismiss_splash(void *data) {
  s_splash_timer = NULL;
  window_stack_pop(true);
}

static void cancel_splash_timer(void) {
  if (s_splash_timer) {
    app_timer_cancel(s_splash_timer);
    s_splash_timer = NULL;
  }
}

static void start_splash_timer(void) {
  cancel_splash_timer();
  s_splash_timer = app_timer_register(SPLASH_DISMISS_MS, dismiss_splash, NULL);
}

// ---- Locate-screen idle auto-dismiss and Select feedback ---------------

static void dismiss_locate(void *data) {
  s_locate_timer = NULL;
  window_stack_pop(true);
}

static void cancel_locate_timer(void) {
  if (s_locate_timer) {
    app_timer_cancel(s_locate_timer);
    s_locate_timer = NULL;
  }
}

static void restart_locate_timer(void) {
  cancel_locate_timer();
  s_locate_timer = app_timer_register(
      s_engaged ? LOCATE_DISMISS_ENGAGED_MS : LOCATE_DISMISS_MS, dismiss_locate, NULL);
}

static void revert_locate_text(void *data) {
  s_feedback_timer = NULL;
  if (s_state == STATE_RECONNECTED_LOCATE || s_state == STATE_CONNECTED_LOCATE) {
    text_layer_set_text(s_locate_layer, "Press Select\nto sound phone");
  }
}

static void locate_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  s_engaged = true;
  request_phone_sound();
  text_layer_set_text(s_locate_layer, "Beeping...");

  if (s_feedback_timer) {
    app_timer_cancel(s_feedback_timer);
  }
  s_feedback_timer = app_timer_register(LOCATE_FEEDBACK_MS, revert_locate_text, NULL);

  // Any interaction with the locate screen pushes the auto-dismiss back.
  restart_locate_timer();
}

static void locate_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, locate_select_click_handler);
}

// ---- Menu callbacks (2 sections: "Sound phone", "Alert every:") -------
//
// Section 0, row 0: the "Sound phone" action.
// Section 1, rows 0..NUM_INTERVAL_OPTIONS-1: the interval choices,
// with the active one marked, same as before.

static uint16_t get_num_sections(MenuLayer *menu_layer, void *context) {
  return 2;
}

static uint16_t get_num_rows(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  return (section_index == 0) ? 1 : NUM_INTERVAL_OPTIONS;
}

static int16_t get_header_height(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  return (section_index == 1) ? 20 : 0;
}

static void draw_header(GContext *ctx, const Layer *cell_layer, uint16_t section_index, void *context) {
  if (section_index != 1) {
    return;
  }
  GRect bounds = layer_get_bounds(cell_layer);
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, "Alert every:", fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                      GRect(4, 1, bounds.size.w - 8, 18),
                      GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static int16_t get_cell_height(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  return 36;
}

static void draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *context) {
  GRect bounds = layer_get_bounds(cell_layer);
  graphics_context_set_text_color(ctx, GColorBlack);

  if (cell_index->section == 0) {
    graphics_draw_text(ctx, "Sound phone", fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
                        GRect(4, 4, bounds.size.w - 8, bounds.size.h - 4),
                        GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    return;
  }

  uint16_t row = cell_index->row;
  char buf[24];
  snprintf(buf, sizeof(buf), "%s%s", (row == (uint16_t) s_active_index) ? "> " : "  ",
           s_interval_labels[row]);
  graphics_draw_text(ctx, buf, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
                      GRect(4, 4, bounds.size.w - 8, bounds.size.h - 4),
                      GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void selection_changed(MenuLayer *menu_layer, MenuIndex new_index, MenuIndex old_index,
                              void *context) {
  if (s_menu_ready) {
    s_engaged = true;  // Up/Down scrolling counts as interaction
  }
}

static void select_click(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  s_engaged = true;
  if (cell_index->section == 0) {
    request_phone_sound();
    return;
  }

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

// ---- State machine -------------------------------------------------------

static void enter_state(AppScreenState new_state) {
  s_state = new_state;

  layer_set_hidden(text_layer_get_layer(s_splash_layer), true);
  layer_set_hidden(text_layer_get_layer(s_status_layer), true);
  layer_set_hidden(s_border_layer, true);
  layer_set_hidden(menu_layer_get_layer(s_menu_layer), true);
  layer_set_hidden(text_layer_get_layer(s_locate_layer), true);

  cancel_splash_timer();
  cancel_alert_timer();
  cancel_locate_timer();
  if (s_feedback_timer) {
    app_timer_cancel(s_feedback_timer);
    s_feedback_timer = NULL;
  }

  switch (new_state) {
    case STATE_SPLASH_OK:
      layer_set_hidden(text_layer_get_layer(s_splash_layer), false);
      window_set_click_config_provider(s_window, NULL);
      start_splash_timer();
      break;

    case STATE_DISCONNECTED_MENU:
      layer_set_hidden(text_layer_get_layer(s_status_layer), false);
      text_layer_set_text(s_status_layer, "Phone disconnected");
      layer_set_hidden(s_border_layer, false);
      layer_set_hidden(menu_layer_get_layer(s_menu_layer), false);
      menu_layer_set_click_config_onto_window(s_menu_layer, s_window);
      restart_alert_timer();
      break;

    case STATE_CONNECTED_LOCATE:
    case STATE_RECONNECTED_LOCATE:
      layer_set_hidden(text_layer_get_layer(s_status_layer), false);
      text_layer_set_text(s_status_layer,
                          new_state == STATE_RECONNECTED_LOCATE ? "Phone reconnected"
                                                                : "Phone connected");
      layer_set_hidden(s_border_layer, false);
      layer_set_hidden(text_layer_get_layer(s_locate_layer), false);
      text_layer_set_text(s_locate_layer, "Press Select\nto sound phone");
      window_set_click_config_provider(s_window, locate_click_config_provider);
      if (new_state == STATE_RECONNECTED_LOCATE) {
        vibrate_reconnect();  // only for a genuine reconnect, not a manual open
      }
      restart_locate_timer();
      break;
  }
}

static void connection_handler(bool connected) {
  switch (s_state) {
    case STATE_SPLASH_OK:
      if (!connected) {
        // Dropped while we were still showing "all good" - switch
        // straight into the real disconnected flow.
        enter_state(STATE_DISCONNECTED_MENU);
      }
      break;

    case STATE_DISCONNECTED_MENU:
      if (connected) {
        enter_state(STATE_RECONNECTED_LOCATE);
      } else {
        restart_alert_timer(); // defensive; normally just a no-op reschedule
      }
      break;

    case STATE_CONNECTED_LOCATE:
    case STATE_RECONNECTED_LOCATE:
      if (!connected) {
        // Dropped again before being dismissed - resume the normal
        // disconnected flow rather than keep offering to "locate" a
        // phone that isn't there right now.
        enter_state(STATE_DISCONNECTED_MENU);
      }
      break;
  }
}

// ---- Window lifecycle ---------------------------------------------------

static void window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  int16_t top_h = bounds.size.h / 4;
  int16_t bottom_h = bounds.size.h - top_h;
  int16_t margin = 4;

  GRect status_frame = GRect(0, 0, bounds.size.w, top_h);
  GRect border_frame = GRect(0, top_h, bounds.size.w, bottom_h);
  GRect inner_frame = GRect(margin, top_h + margin,
                             bounds.size.w - 2 * margin, bottom_h - 2 * margin);

  s_splash_layer = text_layer_create(bounds);
  text_layer_set_text(s_splash_layer, "Connected\n\nBT Guard running");
  text_layer_set_font(s_splash_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_splash_layer, GTextAlignmentCenter);
  text_layer_set_overflow_mode(s_splash_layer, GTextOverflowModeWordWrap);
  layer_add_child(window_layer, text_layer_get_layer(s_splash_layer));

  s_status_layer = text_layer_create(status_frame);
  text_layer_set_font(s_status_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(s_status_layer, GTextAlignmentCenter);
  text_layer_set_overflow_mode(s_status_layer, GTextOverflowModeWordWrap);
  layer_add_child(window_layer, text_layer_get_layer(s_status_layer));

  s_border_layer = layer_create(border_frame);
  layer_set_update_proc(s_border_layer, border_layer_update_proc);
  layer_add_child(window_layer, s_border_layer);

  s_menu_layer = menu_layer_create(inner_frame);
  menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks) {
    .get_num_sections = get_num_sections,
    .get_num_rows = get_num_rows,
    .get_header_height = get_header_height,
    .draw_header = draw_header,
    .get_cell_height = get_cell_height,
    .draw_row = draw_row,
    .select_click = select_click,
    .selection_changed = selection_changed,
  });
  menu_layer_set_selected_index(s_menu_layer, MenuIndex(1, s_active_index), MenuRowAlignCenter, false);
  menu_layer_set_highlight_colors(s_menu_layer, GColorCyan, GColorBlack);
  layer_add_child(window_layer, menu_layer_get_layer(s_menu_layer));

  s_locate_layer = text_layer_create(inner_frame);
  text_layer_set_font(s_locate_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_locate_layer, GTextAlignmentCenter);
  text_layer_set_overflow_mode(s_locate_layer, GTextOverflowModeWordWrap);
  layer_add_child(window_layer, text_layer_get_layer(s_locate_layer));

  s_menu_ready = true;
  enter_state(s_state);

  if (s_launch_reason == APP_LAUNCH_WORKER && s_state == STATE_DISCONNECTED_MENU) {
    // Fresh alert from the worker while still disconnected - buzz right
    // away rather than waiting out the first interval.
    vibrate_alert();
  }
}

static void window_unload(Window *window) {
  text_layer_destroy(s_splash_layer);
  text_layer_destroy(s_status_layer);
  layer_destroy(s_border_layer);
  menu_layer_destroy(s_menu_layer);
  text_layer_destroy(s_locate_layer);
}

// ---- Border drawing -------------------------------------------------------

static void border_layer_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_draw_round_rect(ctx, GRect(0, 0, bounds.size.w, bounds.size.h), 8);
}

// ---- App lifecycle ---------------------------------------------------------

static void init(void) {
  s_active_index = clamp_index(persist_read_int(PERSIST_KEY_INTERVAL_INDEX));

  ensure_worker_running();
  connection_service_subscribe((ConnectionHandlers) {
    .pebble_app_connection_handler = connection_handler,
  });

  app_message_register_outbox_failed(outbox_failed_handler);
  app_message_open(64, 64);

  s_launch_reason = launch_reason();
  s_engaged = (s_launch_reason == APP_LAUNCH_USER ||
               s_launch_reason == APP_LAUNCH_QUICK_LAUNCH);
  bool connected = connection_service_peek_pebble_app_connection();

  if (s_launch_reason == APP_LAUNCH_WORKER) {
    // The worker only ever launches us for one of two reasons: we're
    // still disconnected (standard alert), or we just reconnected
    // (see worker.c) - live status tells us which.
    s_state = connected ? STATE_RECONNECTED_LOCATE : STATE_DISCONNECTED_MENU;
  } else if (connected && (s_launch_reason == APP_LAUNCH_USER ||
                           s_launch_reason == APP_LAUNCH_QUICK_LAUNCH)) {
    // Opened by hand with the phone connected: offer the locate option
    // straight away instead of the brief "all good" splash.
    s_state = STATE_CONNECTED_LOCATE;
  } else {
    s_state = connected ? STATE_SPLASH_OK : STATE_DISCONNECTED_MENU;
  }

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);
}

static void deinit(void) {
  cancel_alert_timer();
  cancel_splash_timer();
  cancel_locate_timer();
  if (s_feedback_timer) {
    app_timer_cancel(s_feedback_timer);
  }
  connection_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
