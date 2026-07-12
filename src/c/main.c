#include <pebble.h>
#include "audio.h"

// ---- AppMessage keys (raw integer keys, shared with src/pkjs/index.js) ----
#define KEY_PLAY_ID     0  // watch -> js : int, index of sound to play
#define KEY_SOUND_NAMES 1  // js -> watch : cstring, "\n"-joined "emoji name"
#define KEY_SOUND_COUNT 2  // js -> watch : int, number of sounds
#define KEY_PCM_META    3  // js -> watch : int, total bytes of incoming clip
#define KEY_PCM_CHUNK   4  // js -> watch : data, a chunk of PCM bytes
#define KEY_PCM_DONE    5  // js -> watch : int, transfer complete
#define KEY_STATUS      6  // js -> watch : cstring, status / error text
#define KEY_VOLUME      7  // js -> watch : int, volume percentage (1-100)

#define PERSIST_KEY_VOLUME 100

#define INBOX_SIZE  4096
#define OUTBOX_SIZE 256

#define STATUS_HIDE_MS 2500
#define MAX_STATUS_LEN 64

static Window *s_main_window;
static MenuLayer *s_menu_layer;
static TextLayer *s_status_layer;

// Sound list received from the phone (display strings "emoji name").
static char **s_sounds = NULL;
static int s_sound_count = 0;
static bool s_list_loaded = false;

static bool s_busy = false;  // a play request is in flight
static uint8_t s_volume_pct = 70;

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
// Sound list
// ---------------------------------------------------------------------------

static void free_sounds(void) {
  if (s_sounds) {
    for (int i = 0; i < s_sound_count; i++) {
      free(s_sounds[i]);
    }
    free(s_sounds);
    s_sounds = NULL;
  }
  s_sound_count = 0;
}

// Parse a "\n"-separated list of display strings into s_sounds.
static void update_sound_list(const char *joined) {
  free_sounds();
  s_list_loaded = true;

  if (!joined || joined[0] == '\0') {
    if (s_menu_layer) {
      menu_layer_reload_data(s_menu_layer);
    }
    return;
  }

  // Count rows.
  int count = 1;
  for (const char *p = joined; *p; p++) {
    if (*p == '\n') {
      count++;
    }
  }

  s_sounds = malloc(sizeof(char *) * count);
  if (!s_sounds) {
    return;
  }

  int idx = 0;
  const char *start = joined;
  const char *p = joined;
  for (;;) {
    if (*p == '\n' || *p == '\0') {
      int len = (int)(p - start);
      char *label = malloc((size_t)len + 1);
      if (label) {
        memcpy(label, start, (size_t)len);
        label[len] = '\0';
        s_sounds[idx++] = label;
      }
      if (*p == '\0') {
        break;
      }
      start = p + 1;
    }
    p++;
  }
  s_sound_count = idx;

  if (s_menu_layer) {
    menu_layer_reload_data(s_menu_layer);
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
// Menu callbacks
// ---------------------------------------------------------------------------

static uint16_t menu_get_num_rows(MenuLayer *menu_layer, uint16_t section_index, void *context) {
  if (!s_list_loaded) {
    return 1;  // "Loading" placeholder
  }
  if (s_sound_count == 0) {
    return 1;  // empty state
  }
  return (uint16_t)s_sound_count;
}

static void menu_draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *index, void *context) {
  if (!s_list_loaded) {
    menu_cell_basic_draw(ctx, cell_layer, "Loading\u2026", "Connecting to phone", NULL);
    return;
  }
  if (s_sound_count == 0) {
    menu_cell_basic_draw(ctx, cell_layer, "No sounds yet", "Add them in the Pebble app", NULL);
    return;
  }
  menu_cell_basic_draw(ctx, cell_layer, s_sounds[index->row], NULL, NULL);
}

static void menu_select_click(MenuLayer *menu_layer, MenuIndex *index, void *context) {
  if (!s_list_loaded || s_sound_count == 0) {
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
  if (names_t) {
    update_sound_list(names_t->value->cstring);
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
  menu_layer_set_click_config_onto_window(s_menu_layer, window);
  layer_add_child(window_layer, menu_layer_get_layer(s_menu_layer));

  // Status overlay along the bottom of the screen.
  GRect status_bounds = GRect(0, bounds.size.h - 28, bounds.size.w, 28);
  s_status_layer = text_layer_create(status_bounds);
  text_layer_set_background_color(s_status_layer, GColorBlack);
  text_layer_set_text_color(s_status_layer, GColorWhite);
  text_layer_set_text_alignment(s_status_layer, GTextAlignmentCenter);
  text_layer_set_font(s_status_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  layer_set_hidden(text_layer_get_layer(s_status_layer), true);
  layer_add_child(window_layer, text_layer_get_layer(s_status_layer));
}

static void main_window_unload(Window *window) {
  if (s_status_layer) {
    text_layer_destroy(s_status_layer);
    s_status_layer = NULL;
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
  free_sounds();
  if (s_main_window) {
    window_destroy(s_main_window);
  }
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
