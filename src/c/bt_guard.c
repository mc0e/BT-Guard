#include <pebble.h>

// ---------------------------------------------------------------------
// Platform presentation.
//
// Everything that should look different on different watches lives in
// this one table, so adapting to a new platform means adding one block
// here (plus, optionally, banner images - see below), not hunting
// through the code.
//
// Fonts are system font keys. Add a block per platform; anything not
// listed falls through to the #else defaults.
//
// Banner images: for each connection state the app looks for a bitmap
// resource named BANNER_DISCONNECTED / BANNER_CONNECTED /
// BANNER_RECONNECTED. If the resource doesn't exist on the platform
// being built (declare it in package.json with "targetPlatforms"), that
// state falls back to text drawn in font_banner. When any banner image
// exists, the banner strip is as tall as the tallest one.
// ---------------------------------------------------------------------
typedef struct {
  const char *font_banner;       // text fallback for the connection banner
  const char *font_splash;       // "Connected / BT Guard running"
  const char *font_locate;       // "Press Select to sound phone"
  const char *font_menu_row;     // interval rows
  const char *font_menu_header;  // "Alert every:"
  int16_t banner_height;         // text banner height; 0 = quarter of screen
  int16_t margin;                // gap between border and content
  int16_t border_radius;
  int16_t menu_row_height;
  int16_t menu_header_height;
} PlatformStyle;

#if defined(PBL_PLATFORM_EMERY)         // Pebble Time 2 (200x228)
static const PlatformStyle s_style = {
  .font_banner = FONT_KEY_ROBOTO_CONDENSED_21,
  .font_splash = FONT_KEY_GOTHIC_24_BOLD,
  .font_locate = FONT_KEY_GOTHIC_24_BOLD,
  .font_menu_row = FONT_KEY_GOTHIC_24_BOLD,
  .font_menu_header = FONT_KEY_GOTHIC_14_BOLD,
  .banner_height = 0,
  .margin = 4,
  .border_radius = 8,
  .menu_row_height = 36,
  .menu_header_height = 20,
};
#elif defined(PBL_PLATFORM_FLINT)       // Pebble 2 Duo (144x168, B&W)
static const PlatformStyle s_style = {
  .font_banner = FONT_KEY_GOTHIC_18_BOLD,
  .font_splash = FONT_KEY_GOTHIC_24_BOLD,
  .font_locate = FONT_KEY_GOTHIC_24_BOLD,
  .font_menu_row = FONT_KEY_GOTHIC_24_BOLD,
  .font_menu_header = FONT_KEY_GOTHIC_14_BOLD,
  .banner_height = 0,
  .margin = 4,
  .border_radius = 8,
  .menu_row_height = 36,
  .menu_header_height = 20,
};
#else                                   // any other platform
static const PlatformStyle s_style = {
  .font_banner = FONT_KEY_GOTHIC_18_BOLD,
  .font_splash = FONT_KEY_GOTHIC_24_BOLD,
  .font_locate = FONT_KEY_GOTHIC_24_BOLD,
  .font_menu_row = FONT_KEY_GOTHIC_24_BOLD,
  .font_menu_header = FONT_KEY_GOTHIC_14_BOLD,
  .banner_height = 0,
  .margin = 4,
  .border_radius = 8,
  .menu_row_height = 36,
  .menu_header_height = 20,
};
#endif

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
//                          interval menu. Shown whenever we're launched
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
static Layer *s_banner_layer;       // connection banner above the border

typedef enum {
  BANNER_DISCONNECTED,
  BANNER_CONNECTED,
  BANNER_RECONNECTED,
  BANNER_COUNT
} BannerKind;

static const char *const s_banner_text[BANNER_COUNT] = {
  "Phone disconnected", "Phone connected", "Phone reconnected"
};
static GBitmap *s_banner_bitmap[BANNER_COUNT];  // NULL = no image, use text
static BannerKind s_banner_kind;
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

// ---- Menu callbacks (1 section: "Alert every:") ------------------------
//
// Rows 0..NUM_INTERVAL_OPTIONS-1: the interval choices, with the
// active one marked.

static uint16_t get_num_sections(MenuLayer *menu_layer, void *context) {
  return 1;
}

static uint16_t get_num_rows(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  return NUM_INTERVAL_OPTIONS;
}

static int16_t get_header_height(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  return s_style.menu_header_height;
}

static void draw_header(GContext *ctx, const Layer *cell_layer, uint16_t section_index, void *context) {
  GRect bounds = layer_get_bounds(cell_layer);
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, "Alert every:", fonts_get_system_font(s_style.font_menu_header),
                      GRect(4, 1, bounds.size.w - 8, 18),
                      GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static int16_t get_cell_height(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  return s_style.menu_row_height;
}

static void draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *context) {
  GRect bounds = layer_get_bounds(cell_layer);
  graphics_context_set_text_color(ctx, GColorBlack);

  uint16_t row = cell_index->row;
  char buf[24];
  snprintf(buf, sizeof(buf), "%s%s", (row == (uint16_t) s_active_index) ? "> " : "  ",
           s_interval_labels[row]);
  graphics_draw_text(ctx, buf, fonts_get_system_font(s_style.font_menu_row),
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

// ---- Connection banner (image if available, otherwise text) -----------

// Resource id for a banner image, or 0 if this platform build has none.
// The #ifdefs make a missing resource a quiet fallback to text rather
// than a build error.
static uint32_t banner_resource_id(BannerKind kind) {
  switch (kind) {
#ifdef RESOURCE_ID_BANNER_DISCONNECTED
    case BANNER_DISCONNECTED: return RESOURCE_ID_BANNER_DISCONNECTED;
#endif
#ifdef RESOURCE_ID_BANNER_CONNECTED
    case BANNER_CONNECTED: return RESOURCE_ID_BANNER_CONNECTED;
#endif
#ifdef RESOURCE_ID_BANNER_RECONNECTED
    case BANNER_RECONNECTED: return RESOURCE_ID_BANNER_RECONNECTED;
#endif
    default: return 0;
  }
}

static void banner_layer_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  GBitmap *bmp = s_banner_bitmap[s_banner_kind];

  if (bmp) {
    GRect b = gbitmap_get_bounds(bmp);
    GRect r = GRect((bounds.size.w - b.size.w) / 2, (bounds.size.h - b.size.h) / 2,
                    b.size.w, b.size.h);
    graphics_context_set_compositing_mode(ctx, GCompOpSet);  // honour transparency
    graphics_draw_bitmap_in_rect(ctx, bmp, r);
    return;
  }

  // Text fallback, centred both ways.
  const char *text = s_banner_text[s_banner_kind];
  GFont font = fonts_get_system_font(s_style.font_banner);
  GRect box = GRect(2, 0, bounds.size.w - 4, bounds.size.h);
  GSize size = graphics_text_layout_get_content_size(text, font, box,
                                                     GTextOverflowModeWordWrap,
                                                     GTextAlignmentCenter);
  int16_t y = (bounds.size.h - size.h) / 2;
  if (y < 0) {
    y = 0;
  }
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, text, font, GRect(2, y, box.size.w, size.h + 6),
                     GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

static void banner_show(BannerKind kind) {
  s_banner_kind = kind;
  layer_set_hidden(s_banner_layer, false);
  layer_mark_dirty(s_banner_layer);
}

// ---- State machine -------------------------------------------------------

static void enter_state(AppScreenState new_state) {
  s_state = new_state;

  layer_set_hidden(text_layer_get_layer(s_splash_layer), true);
  layer_set_hidden(s_banner_layer, true);
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
      banner_show(BANNER_DISCONNECTED);
      layer_set_hidden(s_border_layer, false);
      layer_set_hidden(menu_layer_get_layer(s_menu_layer), false);
      menu_layer_set_click_config_onto_window(s_menu_layer, s_window);
      restart_alert_timer();
      break;

    case STATE_CONNECTED_LOCATE:
    case STATE_RECONNECTED_LOCATE:
      banner_show(new_state == STATE_RECONNECTED_LOCATE ? BANNER_RECONNECTED
                                                        : BANNER_CONNECTED);
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

  // Load whichever banner images exist for this platform build.
  int16_t image_h = 0;
  for (int i = 0; i < BANNER_COUNT; i++) {
    uint32_t id = banner_resource_id((BannerKind) i);
    s_banner_bitmap[i] = id ? gbitmap_create_with_resource(id) : NULL;
    if (s_banner_bitmap[i]) {
      int16_t h = gbitmap_get_bounds(s_banner_bitmap[i]).size.h;
      if (h > image_h) {
        image_h = h;
      }
    }
  }

  int16_t top_h = image_h ? image_h
                : s_style.banner_height ? s_style.banner_height
                : bounds.size.h / 4;
  if (top_h > bounds.size.h / 2) {
    top_h = bounds.size.h / 2;  // never let the banner swallow the screen
  }
  int16_t bottom_h = bounds.size.h - top_h;
  int16_t margin = s_style.margin;

  GRect status_frame = GRect(0, 0, bounds.size.w, top_h);
  GRect border_frame = GRect(0, top_h, bounds.size.w, bottom_h);
  GRect inner_frame = GRect(margin, top_h + margin,
                             bounds.size.w - 2 * margin, bottom_h - 2 * margin);

  s_splash_layer = text_layer_create(bounds);
  text_layer_set_text(s_splash_layer, "Connected\n\nBT Guard running");
  text_layer_set_font(s_splash_layer, fonts_get_system_font(s_style.font_splash));
  text_layer_set_text_alignment(s_splash_layer, GTextAlignmentCenter);
  text_layer_set_overflow_mode(s_splash_layer, GTextOverflowModeWordWrap);
  layer_add_child(window_layer, text_layer_get_layer(s_splash_layer));

  s_banner_layer = layer_create(status_frame);
  layer_set_update_proc(s_banner_layer, banner_layer_update_proc);
  layer_add_child(window_layer, s_banner_layer);

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
  menu_layer_set_selected_index(s_menu_layer, MenuIndex(0, s_active_index), MenuRowAlignCenter, false);
  menu_layer_set_highlight_colors(s_menu_layer, GColorCyan, GColorBlack);
  layer_add_child(window_layer, menu_layer_get_layer(s_menu_layer));

  s_locate_layer = text_layer_create(inner_frame);
  text_layer_set_font(s_locate_layer, fonts_get_system_font(s_style.font_locate));
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
  layer_destroy(s_banner_layer);
  for (int i = 0; i < BANNER_COUNT; i++) {
    if (s_banner_bitmap[i]) {
      gbitmap_destroy(s_banner_bitmap[i]);
      s_banner_bitmap[i] = NULL;
    }
  }
  layer_destroy(s_border_layer);
  menu_layer_destroy(s_menu_layer);
  text_layer_destroy(s_locate_layer);
}

// ---- Border drawing -------------------------------------------------------

static void border_layer_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_draw_round_rect(ctx, GRect(0, 0, bounds.size.w, bounds.size.h), s_style.border_radius);
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