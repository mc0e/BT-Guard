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
// resource named BANNER_DISCONNECTED / BANNER_CONNECTED. If the resource
// doesn't exist on the platform being built (declare it in package.json
// with "targetPlatforms"), that state falls back to text drawn in
// font_banner. When any banner image exists, the banner strip is as
// tall as the tallest one.
//
// Snooze icon: the disconnected screen shows an icon to the left of the
// options panel if a bitmap resource named SNOOZE_ICON exists on the
// platform being built; otherwise a "Zz" text placeholder is drawn in a
// box of snooze_placeholder_w x snooze_placeholder_h. An icon is drawn
// at the bitmap's own size.
//
// Locate icon: the connected screens ("Press Select to sound phone") draw
// a bitmap resource named LOCATE_ICON at its own size if it exists on the
// platform being built; otherwise they draw the text in font_locate. Either
// way the content is centred on the whole screen (both axes, never above
// the banner), shifting left only as far as needed to clear the action bar
// if one is attached. There is no border on these screens.
//
// Interval labels: short_labels selects the compact set ("30 s" rather
// than "30 sec") for platforms whose options panel is too narrow.
//
// Disconnected-screen geometry: the icon and the options panel are both
// vertically centred on the middle of the screen. The panel fits all the
// interval rows if there is room below the banner, and otherwise shrinks
// (symmetrically about the screen centre) and scrolls. The banner stays
// centred on the screen, shifting left only as far as needed to keep
// clear of the action bar.
// ---------------------------------------------------------------------
typedef struct {
  const char *font_banner;       // text fallback for the connection banner
  const char *font_splash;       // "Connected / BT Guard running"
  const char *font_locate;       // "Press Select to sound phone"
  const char *font_menu_row;     // interval rows (also the "Zz" placeholder)
  int16_t banner_height;         // text banner height; 0 = quarter of screen
  int16_t margin;                // gap between border and content, and between elements
  int16_t border_radius;
  int16_t menu_row_height;
  int16_t panel_width;           // options panel width; 0 = fill space right of the icon
  int16_t panel_height;          // options panel height; 0 = fit all rows
  bool short_labels;             // use compact interval labels ("30 s")
  int16_t snooze_placeholder_w;  // "Zz" placeholder box, used when there's no icon resource
  int16_t snooze_placeholder_h;
} PlatformStyle;

#if defined(PBL_PLATFORM_EMERY)         // Pebble Time 2 (200x228)
static const PlatformStyle s_style = {
  .short_labels = false,
  .snooze_placeholder_w = 40,
  .snooze_placeholder_h = 40,
  .font_banner = FONT_KEY_ROBOTO_CONDENSED_21,
  .font_splash = FONT_KEY_GOTHIC_24_BOLD,
  .font_locate = FONT_KEY_GOTHIC_24_BOLD,
  .font_menu_row = FONT_KEY_GOTHIC_24_BOLD,
  .banner_height = 0,
  .margin = 4,
  .border_radius = 8,
  .menu_row_height = 36,
  .panel_width = 0,
  .panel_height = 0,
};
#elif defined(PBL_PLATFORM_FLINT)       // Pebble 2 Duo (144x168, B&W)
static const PlatformStyle s_style = {
  .short_labels = true,
  .snooze_placeholder_w = 30,
  .snooze_placeholder_h = 30,
  .font_banner = FONT_KEY_GOTHIC_18_BOLD,
  .font_splash = FONT_KEY_GOTHIC_24_BOLD,
  .font_locate = FONT_KEY_GOTHIC_24_BOLD,
  .font_menu_row = FONT_KEY_GOTHIC_18_BOLD,
  .banner_height = 0,
  .margin = 4,
  .border_radius = 8,
  .menu_row_height = 30,
  .panel_width = 70,
  .panel_height = 0,
};
#elif defined(PBL_PLATFORM_GABBRO)       // Pebble 2 Duo (144x168, B&W)
static const PlatformStyle s_style = {
  .short_labels = false,
  .snooze_placeholder_w = 40,
  .snooze_placeholder_h = 40,
  .font_banner = FONT_KEY_GOTHIC_18_BOLD,
  .font_splash = FONT_KEY_GOTHIC_24_BOLD,
  .font_locate = FONT_KEY_GOTHIC_24_BOLD,
  .font_menu_row = FONT_KEY_GOTHIC_24_BOLD,
  .banner_height = 0,
  .margin = 4,
  .border_radius = 8,
  .menu_row_height = 36,
  .panel_width = 0,
  .panel_height = 0,
};
#else                                   // any other platform
static const PlatformStyle s_style = {
  .short_labels = false,
  .snooze_placeholder_w = 40,
  .snooze_placeholder_h = 40,
  .font_banner = FONT_KEY_GOTHIC_18_BOLD,
  .font_splash = FONT_KEY_GOTHIC_24_BOLD,
  .font_locate = FONT_KEY_GOTHIC_24_BOLD,
  .font_menu_row = FONT_KEY_GOTHIC_24_BOLD,
  .banner_height = 0,
  .margin = 4,
  .border_radius = 8,
  .menu_row_height = 36,
  .panel_width = 0,
  .panel_height = 0,
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
//   DISCONNECTED_MENU    - "Phone disconnected" banner over a snooze icon
//                          and a panel of alert-interval options, with an
//                          action bar for Up / Select / Down. Shown
//                          whenever we're launched and the link is down,
//                          however we got here. Buzzes on the selected
//                          repeat interval for as long as this screen
//                          stays open.
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
// Compact versions, selected by PlatformStyle.short_labels.
static const char * const s_interval_labels_short[NUM_INTERVAL_OPTIONS] = {
  "30 s", "5 m", "20 m", "1 h", "None"
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
  BANNER_COUNT
} BannerKind;

static const char *const s_banner_text[BANNER_COUNT] = {
  "Phone disconnected", "Phone connected"
};
static GBitmap *s_banner_bitmap[BANNER_COUNT];  // NULL = no image, use text
static BannerKind s_banner_kind;
static Layer *s_border_layer;       // rounded-rect frame round the options panel (D state only)
static MenuLayer *s_menu_layer;     // D state content
static Layer *s_snooze_layer;       // D state: snooze icon left of the options
static GBitmap *s_snooze_bitmap;    // NULL = no image, draw "Zz" placeholder
static Layer *s_locate_layer;       // C/R states: full-screen layer drawing icon or text
static GBitmap *s_locate_bitmap;    // NULL = no image, draw text
static bool s_locate_beeping;       // brief "Beeping..." feedback after Select

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

// Action bar: system ActionBarLayers, attached to the window with
// action_bar_layer_add_to_window() so the SDK handles placement and icon
// positions for each platform's screen shape and button layout. Two exist,
// and at most one is attached at a time (see set_action_bar()):
//   s_action_bar         - DISCONNECTED_MENU: Up / Select / Down
//   s_locate_action_bar  - CONNECTED_LOCATE / RECONNECTED_LOCATE: Select
//                          only, shown as a right caret, which sounds the phone
// SPLASH_OK has none. The icons are PNG bitmap resources ACTION_UP /
// ACTION_SELECT / ACTION_DOWN / ACTION_RIGHT; add per-platform variants in
// package.json via "targetPlatforms" when other platforms are supported.
#define ACTION_BAR_BG GColorLightGray   // the supplied icons are black

static ActionBarLayer *s_action_bar;          // D state: Up / Select / Down
static ActionBarLayer *s_locate_action_bar;   // C/R states: Select only
static ActionBarLayer *s_action_bar_current;  // whichever is attached, or NULL
static bool s_action_bar_attached;            // s_action_bar_current != NULL; read by layout code
static GBitmap *s_icon_up;
static GBitmap *s_icon_select;
static GBitmap *s_icon_down;
static GBitmap *s_icon_right;

// Window geometry remembered from window_load() so enter_state() can
// re-lay-out the screen elements for each state.
static int16_t s_win_w, s_win_h, s_top_h, s_bottom_h;

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
    s_locate_beeping = false;
    layer_mark_dirty(s_locate_layer);
  }
}

static void locate_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  s_engaged = true;
  request_phone_sound();
  s_locate_beeping = true;
  layer_mark_dirty(s_locate_layer);

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

// ---- Menu callbacks (1 section, no header) -----------------------------
//
// Rows 0..NUM_INTERVAL_OPTIONS-1: the interval choices, with the
// active one marked.

static uint16_t get_num_sections(MenuLayer *menu_layer, void *context) {
  return 1;
}

static uint16_t get_num_rows(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  return NUM_INTERVAL_OPTIONS;
}

static int16_t get_cell_height(MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
  return s_style.menu_row_height;
}

static void draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *context) {
  GRect bounds = layer_get_bounds(cell_layer);
  graphics_context_set_text_color(ctx, GColorBlack);

  uint16_t row = cell_index->row;
  const char *label = s_style.short_labels ? s_interval_labels_short[row] : s_interval_labels[row];
  char buf[24];
  snprintf(buf, sizeof(buf), "%s%s", (row == (uint16_t) s_active_index) ? "> " : "  ", label);
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
    default: return 0;
  }
}

static void banner_layer_update_proc(Layer *layer, GContext *ctx) {
  // The banner layer spans the full screen width so content is centred on
  // the screen, not on the area left of the action bar. If the content
  // would run under the action bar, it's shifted left just far enough to
  // clear it (and no further left than the screen edge).
  GRect bounds = layer_get_bounds(layer);
  int16_t clear_right = bounds.size.w - (s_action_bar_attached ? ACTION_BAR_WIDTH : 0);
  GBitmap *bmp = s_banner_bitmap[s_banner_kind];

  if (bmp) {
    GRect b = gbitmap_get_bounds(bmp);
    int16_t x = (bounds.size.w - b.size.w) / 2;
    if (x + b.size.w > clear_right) {
      x = clear_right - b.size.w;
    }
    if (x < 0) {
      x = 0;
    }
    GRect r = GRect(x, (bounds.size.h - b.size.h) / 2, b.size.w, b.size.h);
    graphics_context_set_compositing_mode(ctx, GCompOpSet);  // honour transparency
    graphics_draw_bitmap_in_rect(ctx, bmp, r);
    return;
  }

  // Text fallback, centred both ways.
  const char *text = s_banner_text[s_banner_kind];
  GFont font = fonts_get_system_font(s_style.font_banner);
  GRect avail = GRect(2, 0, clear_right - 4, bounds.size.h);
  GSize size = graphics_text_layout_get_content_size(text, font, avail,
                                                     GTextOverflowModeWordWrap,
                                                     GTextAlignmentCenter);
  int16_t text_w = size.w + 4;  // slack so rounding can't force a re-wrap
  if (text_w > avail.size.w) {
    text_w = avail.size.w;
  }
  int16_t x = (bounds.size.w - text_w) / 2;
  if (x + text_w > clear_right - 2) {
    x = clear_right - 2 - text_w;
  }
  if (x < 2) {
    x = 2;
  }
  int16_t y = (bounds.size.h - size.h) / 2;
  if (y < 0) {
    y = 0;
  }
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, text, font, GRect(x, y, text_w, size.h + 6),
                     GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

static void banner_show(BannerKind kind) {
  s_banner_kind = kind;
  layer_set_hidden(s_banner_layer, false);
  layer_mark_dirty(s_banner_layer);
}

// ---- Snooze icon (disconnected screen) --------------------------------

static void snooze_layer_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  if (s_snooze_bitmap) {
    graphics_context_set_compositing_mode(ctx, GCompOpSet);  // honour transparency
    graphics_draw_bitmap_in_rect(ctx, s_snooze_bitmap, bounds);
    return;
  }

  // Placeholder until a SNOOZE_ICON resource is added.
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, "Zz", fonts_get_system_font(s_style.font_menu_row),
                     GRect(0, (bounds.size.h - 28) / 2, bounds.size.w, 30),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

// ---- Locate content (connected screens) -------------------------------

#define LOCATE_TEXT "Press Select\nto sound phone"
#define LOCATE_BEEPING_TEXT "Beeping..."

// Top-left origin for content of `size`, centred on the whole screen. If it
// would run under the action bar (when attached) it's shifted left just far
// enough to clear it, but never past the screen edge. It's also kept below
// the banner if it's too tall to centre without touching it.
static GPoint locate_origin(GSize size, GRect bounds) {
  int16_t clear_right = bounds.size.w - (s_action_bar_attached ? ACTION_BAR_WIDTH : 0);
  int16_t x = (bounds.size.w - size.w) / 2;
  if (x + size.w > clear_right) {
    x = clear_right - size.w;
  }
  if (x < 0) {
    x = 0;
  }
  int16_t y = (bounds.size.h - size.h) / 2;
  int16_t min_y = s_top_h + s_style.margin;
  if (y < min_y) {
    y = min_y;
  }
  return GPoint(x, y);
}

static void locate_layer_update_proc(Layer *layer, GContext *ctx) {
  // The layer spans the full window so "centre" means the centre of the screen.
  GRect bounds = layer_get_bounds(layer);

  if (s_locate_bitmap && !s_locate_beeping) {
    GSize size = gbitmap_get_bounds(s_locate_bitmap).size;
    GPoint o = locate_origin(size, bounds);
    graphics_context_set_compositing_mode(ctx, GCompOpSet);  // honour transparency
    graphics_draw_bitmap_in_rect(ctx, s_locate_bitmap, GRect(o.x, o.y, size.w, size.h));
    return;
  }

  // Text (no icon resource, or the "Beeping..." feedback).
  const char *text = s_locate_beeping ? LOCATE_BEEPING_TEXT : LOCATE_TEXT;
  GFont font = fonts_get_system_font(s_style.font_locate);
  int16_t clear_right = bounds.size.w - (s_action_bar_attached ? ACTION_BAR_WIDTH : 0);
  GRect avail = GRect(2, 0, clear_right - 4, bounds.size.h);
  GSize size = graphics_text_layout_get_content_size(text, font, avail,
                                                     GTextOverflowModeWordWrap,
                                                     GTextAlignmentCenter);
  size.w += 4;  // slack so rounding can't force a re-wrap
  if (size.w > avail.size.w) {
    size.w = avail.size.w;
  }
  GPoint o = locate_origin(size, bounds);
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, text, font, GRect(o.x, o.y, size.w, size.h + 6),
                     GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

// ---- Layout per screen ---------------------------------------------------

// Disconnected: the action bar spans the full window height down the right
// edge (as the SDK places it), so the icon and options panel work within
// the width left of it. They sit side by side in the middle, both centred
// vertically on the middle of the screen, below the banner (which keeps
// its full-width, screen-centred frame; see banner_layer_update_proc()).
//
// Other screens: banner across the top, and the locate icon/text centred on
// the screen (no border; see locate_layer_update_proc()).
static void apply_layout(bool disconnected) {
  int16_t margin = s_style.margin;

  if (!disconnected) {
    layer_mark_dirty(s_banner_layer);
    layer_mark_dirty(s_locate_layer);
    return;
  }

  int16_t main_w = s_win_w - ACTION_BAR_WIDTH;
  int16_t centre_y = s_win_h / 2;

  GSize icon = s_snooze_bitmap ? gbitmap_get_bounds(s_snooze_bitmap).size
                               : GSize(s_style.snooze_placeholder_w, s_style.snooze_placeholder_h);
  int16_t icon_x = margin;

  int16_t panel_x = icon_x + icon.w + margin;
  int16_t panel_w = s_style.panel_width ? s_style.panel_width : main_w - panel_x - margin;
  if (panel_w < 2 * margin + 1) {
    panel_w = 2 * margin + 1;
  }

  // Never taller than the space below the banner, measured symmetrically
  // about the screen centre so the panel stays centred on it.
  int16_t max_h = 2 * (centre_y - s_top_h - margin);
  int16_t panel_h = s_style.panel_height ? s_style.panel_height
                                         : NUM_INTERVAL_OPTIONS * s_style.menu_row_height + 2 * margin;
  if (panel_h > max_h) {
    panel_h = max_h;
  }
  if (panel_h < 2 * margin + 1) {
    panel_h = 2 * margin + 1;
  }
  int16_t panel_y = centre_y - panel_h / 2;

  layer_set_frame(s_border_layer, GRect(panel_x, panel_y, panel_w, panel_h));
  layer_set_frame(menu_layer_get_layer(s_menu_layer),
                  GRect(panel_x + margin, panel_y + margin, panel_w - 2 * margin, panel_h - 2 * margin));
  layer_set_frame(s_snooze_layer, GRect(icon_x, centre_y - icon.h / 2, icon.w, icon.h));

  layer_mark_dirty(s_banner_layer);
  layer_mark_dirty(s_border_layer);
  layer_mark_dirty(s_snooze_layer);
}

// Attach `bar` to the window (NULL = no bar), detaching whichever was
// attached before. Attaching installs the bar's own click config on the
// window, so callers set up button handling afterwards where needed.
static void set_action_bar(ActionBarLayer *bar) {
  if (bar == s_action_bar_current) {
    return;
  }
  if (s_action_bar_current) {
    action_bar_layer_remove_from_window(s_action_bar_current);
  }
  if (bar) {
    action_bar_layer_add_to_window(bar, s_window);
  }
  s_action_bar_current = bar;
  s_action_bar_attached = (bar != NULL);
}

// ---- State machine -------------------------------------------------------

static void enter_state(AppScreenState new_state) {
  s_state = new_state;

  layer_set_hidden(text_layer_get_layer(s_splash_layer), true);
  layer_set_hidden(s_banner_layer, true);
  layer_set_hidden(s_border_layer, true);
  layer_set_hidden(menu_layer_get_layer(s_menu_layer), true);
  layer_set_hidden(s_snooze_layer, true);
  layer_set_hidden(s_locate_layer, true);
  apply_layout(new_state == STATE_DISCONNECTED_MENU);
  // Each case below chooses its action bar (or none) and then sets up the
  // window's click handling.

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
      set_action_bar(NULL);
      window_set_click_config_provider(s_window, NULL);
      start_splash_timer();
      break;

    case STATE_DISCONNECTED_MENU:
      banner_show(BANNER_DISCONNECTED);
      layer_set_hidden(s_border_layer, false);
      layer_set_hidden(menu_layer_get_layer(s_menu_layer), false);
      layer_set_hidden(s_snooze_layer, false);
      // The menu was resized by apply_layout(), so re-centre on the active row.
      menu_layer_set_selected_index(s_menu_layer, MenuIndex(0, s_active_index),
                                    MenuRowAlignCenter, false);
      // Attaching installs the action bar's own (empty) click config on the
      // window, so the menu layer must claim the buttons afterwards.
      set_action_bar(s_action_bar);
      menu_layer_set_click_config_onto_window(s_menu_layer, s_window);
      restart_alert_timer();
      break;

    case STATE_CONNECTED_LOCATE:
    case STATE_RECONNECTED_LOCATE:
      banner_show(BANNER_CONNECTED);
      s_locate_beeping = false;
      layer_set_hidden(s_locate_layer, false);
      layer_mark_dirty(s_locate_layer);
      set_action_bar(s_locate_action_bar);  // its click config provides the Select handler
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

  // Snooze icon, if this platform build has one.
  s_snooze_bitmap = NULL;
#ifdef RESOURCE_ID_SNOOZE_ICON
  s_snooze_bitmap = gbitmap_create_with_resource(RESOURCE_ID_SNOOZE_ICON);
#endif

  // Locate icon, if this platform build has one.
  s_locate_bitmap = NULL;
#ifdef RESOURCE_ID_LOCATE_ICON
  s_locate_bitmap = gbitmap_create_with_resource(RESOURCE_ID_LOCATE_ICON);
#endif

  int16_t top_h = image_h ? image_h
                : s_style.banner_height ? s_style.banner_height
                : bounds.size.h / 4;
  if (top_h > bounds.size.h / 2) {
    top_h = bounds.size.h / 2;  // never let the banner swallow the screen
  }
  int16_t bottom_h = bounds.size.h - top_h;
  int16_t margin = s_style.margin;

  s_win_w = bounds.size.w;
  s_win_h = bounds.size.h;
  s_top_h = top_h;
  s_bottom_h = bottom_h;

  // Initial frames only; apply_layout() sets the real ones for each state.
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
    .get_cell_height = get_cell_height,
    .draw_row = draw_row,
    .select_click = select_click,
    .selection_changed = selection_changed,
  });
  menu_layer_set_selected_index(s_menu_layer, MenuIndex(0, s_active_index), MenuRowAlignCenter, false);
  menu_layer_set_highlight_colors(s_menu_layer, GColorCyan, GColorBlack);
  layer_add_child(window_layer, menu_layer_get_layer(s_menu_layer));

  s_snooze_layer = layer_create(GRect(margin, top_h, s_style.snooze_placeholder_w,
                                      s_style.snooze_placeholder_h));
  layer_set_update_proc(s_snooze_layer, snooze_layer_update_proc);
  layer_add_child(window_layer, s_snooze_layer);

  // Full window, so the content is centred on the screen rather than on
  // the area left of any action bar.
  s_locate_layer = layer_create(bounds);
  layer_set_update_proc(s_locate_layer, locate_layer_update_proc);
  layer_add_child(window_layer, s_locate_layer);

  s_icon_up = gbitmap_create_with_resource(RESOURCE_ID_ACTION_UP);
  s_icon_select = gbitmap_create_with_resource(RESOURCE_ID_ACTION_SELECT);
  s_icon_down = gbitmap_create_with_resource(RESOURCE_ID_ACTION_DOWN);

  // Created here, and attached to the window one at a time as the screen
  // changes (see set_action_bar()).
  s_action_bar = action_bar_layer_create();
  action_bar_layer_set_background_color(s_action_bar, ACTION_BAR_BG);
  action_bar_layer_set_icon(s_action_bar, BUTTON_ID_UP, s_icon_up);
  action_bar_layer_set_icon(s_action_bar, BUTTON_ID_SELECT, s_icon_select);
  action_bar_layer_set_icon(s_action_bar, BUTTON_ID_DOWN, s_icon_down);
  s_icon_right = gbitmap_create_with_resource(RESOURCE_ID_ACTION_RIGHT);
  s_locate_action_bar = action_bar_layer_create();
  action_bar_layer_set_background_color(s_locate_action_bar, ACTION_BAR_BG);
  action_bar_layer_set_icon(s_locate_action_bar, BUTTON_ID_SELECT, s_icon_right);
  // Attaching this bar installs this provider on the window, so Select works.
  action_bar_layer_set_click_config_provider(s_locate_action_bar, locate_click_config_provider);

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
  layer_destroy(s_snooze_layer);
  if (s_snooze_bitmap) {
    gbitmap_destroy(s_snooze_bitmap);
    s_snooze_bitmap = NULL;
  }
  layer_destroy(s_locate_layer);
  if (s_locate_bitmap) {
    gbitmap_destroy(s_locate_bitmap);
    s_locate_bitmap = NULL;
  }
  set_action_bar(NULL);
  action_bar_layer_destroy(s_action_bar);
  action_bar_layer_destroy(s_locate_action_bar);
  gbitmap_destroy(s_icon_up);
  gbitmap_destroy(s_icon_select);
  gbitmap_destroy(s_icon_down);
  gbitmap_destroy(s_icon_right);
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
