#include <pebble.h>
#include "audio.h"

// ---- AppMessage keys (raw integer keys, shared with src/pkjs/index.js) ----
#define KEY_PLAY_ID      0  // watch -> js : int, index of sound to play
#define KEY_SOUND_NAMES  1  // js -> watch : cstring, "\n"-joined names (no emoji)
#define KEY_SOUND_COUNT  2  // js -> watch : int, number of sounds
#define KEY_PCM_META     3  // js -> watch : int, total bytes of incoming clip
#define KEY_PCM_CHUNK    4  // js -> watch : data, a chunk of PCM bytes
#define KEY_PCM_DONE     5  // js -> watch : int, transfer complete
#define KEY_STATUS       6  // js -> watch : cstring, status / error text
#define KEY_VOLUME       7  // js -> watch : int, volume percentage (1-100)
#define KEY_SOUND_EMOJIS 8  // js -> watch : cstring, "\n"-joined emoji per sound
#define KEY_GRID_MODE    9  // js -> watch : int, 1 = touch grid mode

#define PERSIST_KEY_VOLUME    100
#define PERSIST_KEY_GRID_MODE 101

#define INBOX_SIZE  4096
#define OUTBOX_SIZE 256

#define STATUS_HIDE_MS 2500
#define MAX_STATUS_LEN 64

// Touch grid geometry.
#define GRID_COLS 2
#define GRID_ROWS 2
#define GRID_PER_PAGE (GRID_COLS * GRID_ROWS)
#define GRID_INDICATOR_H 22
#define SWIPE_THRESHOLD 30
#define TAP_SLOP 22

static Window *s_main_window;
static MenuLayer *s_menu_layer;
static Layer *s_grid_layer;
static TextLayer *s_status_layer;

// Sound list received from the phone: parallel name / emoji arrays.
static char **s_names = NULL;
static int s_name_count = 0;
static char **s_emojis = NULL;
static int s_emoji_count = 0;
static bool s_list_loaded = false;

static bool s_busy = false;  // a play request is in flight
static uint8_t s_volume_pct = 100;
static bool s_grid_mode = false;
static int s_page = 0;

static AppTimer *s_status_hide_timer = NULL;
static char s_status_text[MAX_STATUS_LEN];

// ---------------------------------------------------------------------------
// Volume helpers
// ---------------------------------------------------------------------------

#if defined(PBL_SPEAKER)
static uint8_t speaker_volume_from_pct(uint8_t pct) {
  if (pct > 100) {
    pct = 100;
  }
  return (uint8_t)(AUDIO_VOLUME_MIN +
                   ((uint32_t)pct * (AUDIO_VOLUME_MAX - AUDIO_VOLUME_MIN)) / 100);
}
#endif

static bool use_grid(void) {
#if defined(PBL_TOUCH)
  return s_grid_mode;
#else
  return false;
#endif
}

// ---------------------------------------------------------------------------
// Status overlay
// ---------------------------------------------------------------------------

static void hide_status(void) {
  if (s_status_hide_timer) {
    app_timer_cancel(s_status_hide_timer);
    s_status_hide_timer = NULL;
  }
  if (s_status_layer) {
    layer_set_hidden(text_layer_get_layer(s_status_layer), true);
  }
}

static void status_hide_timer_cb(void *ctx) {
  s_status_hide_timer = NULL;
  hide_status();
}

static void show_status(const char *msg, bool auto_hide) {
  if (!s_status_layer) {
    return;
  }
  strncpy(s_status_text, msg, sizeof(s_status_text) - 1);
  s_status_text[sizeof(s_status_text) - 1] = '\0';
  text_layer_set_text(s_status_layer, s_status_text);
  layer_set_hidden(text_layer_get_layer(s_status_layer), false);

  if (s_status_hide_timer) {
    app_timer_cancel(s_status_hide_timer);
    s_status_hide_timer = NULL;
  }
  if (auto_hide) {
    s_status_hide_timer = app_timer_register(STATUS_HIDE_MS, status_hide_timer_cb, NULL);
  }
}

// ---------------------------------------------------------------------------
// Sound list parsing
// ---------------------------------------------------------------------------

static char **parse_lines(const char *joined, int *out_count) {
  *out_count = 0;
  if (!joined || joined[0] == '\0') {
    return NULL;
  }

  int count = 1;
  for (const char *p = joined; *p; p++) {
    if (*p == '\n') {
      count++;
    }
  }

  char **arr = malloc(sizeof(char *) * count);
  if (!arr) {
    return NULL;
  }

  int idx = 0;
  const char *start = joined;
  const char *p = joined;
  for (;;) {
    if (*p == '\n' || *p == '\0') {
      int len = (int)(p - start);
      char *s = malloc((size_t)len + 1);
      if (s) {
        memcpy(s, start, (size_t)len);
        s[len] = '\0';
        arr[idx++] = s;
      }
      if (*p == '\0') {
        break;
      }
      start = p + 1;
    }
    p++;
  }
  *out_count = idx;
  return arr;
}

static void free_lines(char **arr, int count) {
  if (!arr) {
    return;
  }
  for (int i = 0; i < count; i++) {
    free(arr[i]);
  }
  free(arr);
}

static void set_names(const char *joined) {
  free_lines(s_names, s_name_count);
  s_names = parse_lines(joined, &s_name_count);
  s_list_loaded = true;
}

static void set_emojis(const char *joined) {
  free_lines(s_emojis, s_emoji_count);
  s_emojis = parse_lines(joined, &s_emoji_count);
}

static const char *emoji_at(int idx) {
  if (s_emojis && idx >= 0 && idx < s_emoji_count) {
    return s_emojis[idx];
  }
  return "";
}

static void refresh_ui(void) {
  if (s_menu_layer) {
    menu_layer_reload_data(s_menu_layer);
  }
  if (s_grid_layer) {
    layer_mark_dirty(s_grid_layer);
  }
}

// ---------------------------------------------------------------------------
// Play requests
// ---------------------------------------------------------------------------

static void request_play(int index) {
  if (s_busy) {
    return;
  }
  if (connection_service_peek_pebble_app_connection() == false) {
    show_status("Open Pebble app", true);
    vibes_short_pulse();
    return;
  }

  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
    show_status("Busy, try again", true);
    return;
  }
  dict_write_int(iter, KEY_PLAY_ID, &index, sizeof(int), true);
  if (app_message_outbox_send() != APP_MSG_OK) {
    show_status("Send failed", true);
    return;
  }

  s_busy = true;
  show_status("Loading\u2026", false);
}

static void on_audio_done(void) {
  s_busy = false;
  hide_status();
}

// ---------------------------------------------------------------------------
// Grid mode
// ---------------------------------------------------------------------------

static int grid_page_count(void) {
  if (s_name_count <= 0) {
    return 1;
  }
  return (s_name_count + GRID_PER_PAGE - 1) / GRID_PER_PAGE;
}

static void grid_set_page(int p) {
  int pages = grid_page_count();
  if (p < 0) {
    p = 0;
  }
  if (p >= pages) {
    p = pages - 1;
  }
  if (p != s_page) {
    s_page = p;
    if (s_grid_layer) {
      layer_mark_dirty(s_grid_layer);
    }
  }
}

static void grid_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);

  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorBlack);

  if (!s_list_loaded) {
    graphics_draw_text(ctx, "Loading\u2026",
                       fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD), b,
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    return;
  }
  if (s_name_count == 0) {
    graphics_draw_text(ctx, "No sounds yet\nAdd them in the Pebble app",
                       fonts_get_system_font(FONT_KEY_GOTHIC_18), b,
                       GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    return;
  }

  int grid_h = b.size.h - GRID_INDICATOR_H;
  int tw = b.size.w / GRID_COLS;
  int th = grid_h / GRID_ROWS;

  int pages = grid_page_count();
  if (s_page >= pages) {
    s_page = pages - 1;
  }
  if (s_page < 0) {
    s_page = 0;
  }

  for (int i = 0; i < GRID_PER_PAGE; i++) {
    int idx = s_page * GRID_PER_PAGE + i;
    int col = i % GRID_COLS;
    int row = i / GRID_COLS;
    GRect cell = GRect(col * tw, row * th, tw, th);

    graphics_context_set_stroke_color(ctx, GColorLightGray);
    graphics_draw_rect(ctx, cell);

    if (idx >= s_name_count) {
      continue;
    }

    graphics_context_set_text_color(ctx, GColorBlack);
    graphics_draw_text(ctx, emoji_at(idx),
                       fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD),
                       GRect(cell.origin.x, cell.origin.y + 8, cell.size.w, 36),
                       GTextOverflowModeFill, GTextAlignmentCenter, NULL);
    graphics_draw_text(ctx, s_names[idx],
                       fonts_get_system_font(FONT_KEY_GOTHIC_14),
                       GRect(cell.origin.x + 2, cell.origin.y + th - 24,
                             cell.size.w - 4, 22),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  }

  char pg[32];
  snprintf(pg, sizeof(pg), "%d / %d", s_page + 1, pages);
  graphics_draw_text(ctx, pg, fonts_get_system_font(FONT_KEY_GOTHIC_18),
                     GRect(0, b.size.h - GRID_INDICATOR_H, b.size.w, GRID_INDICATOR_H),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

static void grid_up_click(ClickRecognizerRef recognizer, void *context) {
  grid_set_page(s_page - 1);
}

static void grid_down_click(ClickRecognizerRef recognizer, void *context) {
  grid_set_page(s_page + 1);
}

static void grid_click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, grid_up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, grid_down_click);
}

#if defined(PBL_TOUCH)
static int16_t s_touch_x0;
static int16_t s_touch_y0;
static bool s_touch_tracking;

static void grid_tap(int16_t x, int16_t y) {
  if (!s_grid_layer || s_name_count == 0) {
    return;
  }
  GRect b = layer_get_bounds(s_grid_layer);
  int grid_h = b.size.h - GRID_INDICATOR_H;
  if (y >= grid_h) {
    return;  // tapped the page indicator strip
  }
  int tw = b.size.w / GRID_COLS;
  int th = grid_h / GRID_ROWS;
  int col = x / tw;
  int row = y / th;
  if (col < 0) col = 0;
  if (col >= GRID_COLS) col = GRID_COLS - 1;
  if (row < 0) row = 0;
  if (row >= GRID_ROWS) row = GRID_ROWS - 1;

  int idx = s_page * GRID_PER_PAGE + row * GRID_COLS + col;
  if (idx < s_name_count) {
    request_play(idx);
  }
}

static void touch_handler(const TouchEvent *event, void *context) {
  switch (event->type) {
    case TouchEvent_Touchdown:
      s_touch_x0 = event->x;
      s_touch_y0 = event->y;
      s_touch_tracking = true;
      break;
    case TouchEvent_Liftoff: {
      if (!s_touch_tracking) {
        break;
      }
      s_touch_tracking = false;
      int16_t dx = event->x - s_touch_x0;
      int16_t dy = event->y - s_touch_y0;
      int adx = dx < 0 ? -dx : dx;
      int ady = dy < 0 ? -dy : dy;
      if (ady > SWIPE_THRESHOLD && ady > adx) {
        // Swipe up -> next page, swipe down -> previous page.
        grid_set_page(dy < 0 ? s_page + 1 : s_page - 1);
      } else if (adx < TAP_SLOP && ady < TAP_SLOP) {
        grid_tap(event->x, event->y);
      }
      break;
    }
    default:
      break;
  }
}
#endif  // PBL_TOUCH

// Switch between the list menu and the touch grid.
static void apply_ui_mode(void) {
  bool grid = use_grid();

  if (s_menu_layer) {
    layer_set_hidden(menu_layer_get_layer(s_menu_layer), grid);
  }
  if (s_grid_layer) {
    layer_set_hidden(s_grid_layer, !grid);
  }

  if (grid) {
    window_set_click_config_provider(s_main_window, grid_click_config);
#if defined(PBL_TOUCH)
    if (touch_service_is_enabled()) {
      touch_service_subscribe(touch_handler, NULL);
    }
#endif
    if (s_grid_layer) {
      layer_mark_dirty(s_grid_layer);
    }
  } else {
#if defined(PBL_TOUCH)
    touch_service_unsubscribe();
#endif
    if (s_menu_layer) {
      menu_layer_set_click_config_onto_window(s_menu_layer, s_main_window);
      menu_layer_reload_data(s_menu_layer);
    }
  }
}

// ---------------------------------------------------------------------------
// Menu (list mode) callbacks
// ---------------------------------------------------------------------------

static uint16_t menu_get_num_rows(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  if (!s_list_loaded) {
    return 1;  // "Loading" placeholder
  }
  if (s_name_count == 0) {
    return 1;  // empty state
  }
  return (uint16_t)s_name_count;
}

static void menu_draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *index, void *context) {
  if (!s_list_loaded) {
    menu_cell_basic_draw(ctx, cell_layer, "Loading\u2026", "Connecting to phone", NULL);
    return;
  }
  if (s_name_count == 0) {
    menu_cell_basic_draw(ctx, cell_layer, "No sounds yet", "Add them in the Pebble app", NULL);
    return;
  }

  const char *emoji = emoji_at(index->row);
  static char buf[64];
  if (emoji[0]) {
    snprintf(buf, sizeof(buf), "%s %s", emoji, s_names[index->row]);
  } else {
    snprintf(buf, sizeof(buf), "%s", s_names[index->row]);
  }
  menu_cell_basic_draw(ctx, cell_layer, buf, NULL, NULL);
}

static void menu_select_click(MenuLayer *menu_layer, MenuIndex *index, void *context) {
  if (!s_list_loaded || s_name_count == 0) {
    vibes_short_pulse();
    return;
  }
  request_play(index->row);
}

// ---------------------------------------------------------------------------
// AppMessage
// ---------------------------------------------------------------------------

static void inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *names_t = dict_find(iter, KEY_SOUND_NAMES);
  Tuple *emojis_t = dict_find(iter, KEY_SOUND_EMOJIS);
  if (names_t) {
    set_names(names_t->value->cstring);
  }
  if (emojis_t) {
    set_emojis(emojis_t->value->cstring);
  }

  Tuple *grid_t = dict_find(iter, KEY_GRID_MODE);
  if (grid_t) {
    bool g = grid_t->value->int32 != 0;
    persist_write_bool(PERSIST_KEY_GRID_MODE, g);
    if (g != s_grid_mode) {
      s_grid_mode = g;
      apply_ui_mode();
    }
  }

  if (names_t || emojis_t) {
    s_page = 0;
    refresh_ui();
  }

  Tuple *volume_t = dict_find(iter, KEY_VOLUME);
  if (volume_t) {
    int32_t pct = volume_t->value->int32;
    if (pct < 1) {
      pct = 1;
    }
    if (pct > 100) {
      pct = 100;
    }
    s_volume_pct = (uint8_t)pct;
    persist_write_int(PERSIST_KEY_VOLUME, s_volume_pct);
  }

  Tuple *meta_t = dict_find(iter, KEY_PCM_META);
  if (meta_t) {
    uint32_t total = (uint32_t)meta_t->value->int32;
    if (!audio_begin_receive(total)) {
      show_status("Sound too big", true);
      s_busy = false;
    } else {
      show_status("Loading\u2026", false);
    }
  }

  Tuple *chunk_t = dict_find(iter, KEY_PCM_CHUNK);
  if (chunk_t) {
    audio_receive_chunk(chunk_t->value->data, chunk_t->length);
  }

  Tuple *done_t = dict_find(iter, KEY_PCM_DONE);
  if (done_t) {
    s_busy = false;
#if defined(PBL_SPEAKER)
    show_status("Playing\u2026", false);
    audio_play_received(speaker_volume_from_pct(s_volume_pct));
#else
    show_status("No speaker", true);
    audio_play_received(0);  // falls back to a vibration
#endif
  }

  Tuple *status_t = dict_find(iter, KEY_STATUS);
  if (status_t) {
    show_status(status_t->value->cstring, true);
    s_busy = false;
  }
}

static void inbox_dropped(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Inbox dropped: %d", (int)reason);
  show_status("Transfer error", true);
  s_busy = false;
  audio_stop();
}

static void outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Outbox failed: %d", (int)reason);
  show_status("Send failed", true);
  s_busy = false;
}

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------

static void main_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  s_menu_layer = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks) {
    .get_num_rows = menu_get_num_rows,
    .draw_row = menu_draw_row,
    .select_click = menu_select_click,
  });
  menu_layer_set_highlight_colors(s_menu_layer,
                                  PBL_IF_COLOR_ELSE(GColorJaegerGreen, GColorBlack),
                                  GColorWhite);
  layer_add_child(window_layer, menu_layer_get_layer(s_menu_layer));

  s_grid_layer = layer_create(bounds);
  layer_set_update_proc(s_grid_layer, grid_update_proc);
  layer_set_hidden(s_grid_layer, true);
  layer_add_child(window_layer, s_grid_layer);

  // Status overlay along the bottom of the screen.
  GRect status_bounds = GRect(0, bounds.size.h - 28, bounds.size.w, 28);
  s_status_layer = text_layer_create(status_bounds);
  text_layer_set_background_color(s_status_layer, GColorBlack);
  text_layer_set_text_color(s_status_layer, GColorWhite);
  text_layer_set_text_alignment(s_status_layer, GTextAlignmentCenter);
  text_layer_set_font(s_status_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  layer_set_hidden(text_layer_get_layer(s_status_layer), true);
  layer_add_child(window_layer, text_layer_get_layer(s_status_layer));

  apply_ui_mode();
}

static void main_window_unload(Window *window) {
#if defined(PBL_TOUCH)
  touch_service_unsubscribe();
#endif
  if (s_status_layer) {
    text_layer_destroy(s_status_layer);
    s_status_layer = NULL;
  }
  if (s_grid_layer) {
    layer_destroy(s_grid_layer);
    s_grid_layer = NULL;
  }
  if (s_menu_layer) {
    menu_layer_destroy(s_menu_layer);
    s_menu_layer = NULL;
  }
}

// ---------------------------------------------------------------------------
// Init / deinit
// ---------------------------------------------------------------------------

static void init(void) {
  if (persist_exists(PERSIST_KEY_VOLUME)) {
    int32_t v = persist_read_int(PERSIST_KEY_VOLUME);
    if (v >= 1 && v <= 100) {
      s_volume_pct = (uint8_t)v;
    }
  }
  if (persist_exists(PERSIST_KEY_GRID_MODE)) {
    s_grid_mode = persist_read_bool(PERSIST_KEY_GRID_MODE);
  }

  audio_init();
  audio_set_done_handler(on_audio_done);

  app_message_register_inbox_received(inbox_received);
  app_message_register_inbox_dropped(inbox_dropped);
  app_message_register_outbox_failed(outbox_failed);
  app_message_open(INBOX_SIZE, OUTBOX_SIZE);

  s_main_window = window_create();
  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load = main_window_load,
    .unload = main_window_unload,
  });
  window_stack_push(s_main_window, true);
}

static void deinit(void) {
  audio_deinit();
  free_lines(s_names, s_name_count);
  free_lines(s_emojis, s_emoji_count);
  if (s_main_window) {
    window_destroy(s_main_window);
  }
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
