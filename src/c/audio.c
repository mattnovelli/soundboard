#include "audio.h"

static uint8_t *s_buffer = NULL;
static uint32_t s_capacity = 0;  // allocated size of s_buffer
static uint32_t s_length = 0;    // expected total bytes for the current clip
static uint32_t s_received = 0;  // bytes received so far
static uint32_t s_play_offset = 0;
static bool s_receiving = false;
static bool s_playing = false;
static AppTimer *s_drain_timer = NULL;
static AudioDoneHandler s_done_handler = NULL;

static void free_buffer(void) {
  if (s_buffer) {
    free(s_buffer);
    s_buffer = NULL;
  }
  s_capacity = 0;
}

static void notify_done(void) {
  if (s_done_handler) {
    s_done_handler();
  }
}

void audio_init(void) {
  s_buffer = NULL;
  s_capacity = 0;
  s_length = 0;
  s_received = 0;
  s_play_offset = 0;
  s_receiving = false;
  s_playing = false;
  s_drain_timer = NULL;
}

void audio_deinit(void) {
  audio_stop();
  free_buffer();
}

void audio_set_done_handler(AudioDoneHandler handler) {
  s_done_handler = handler;
}

bool audio_is_receiving(void) {
  return s_receiving;
}

bool audio_is_playing(void) {
  return s_playing;
}

bool audio_begin_receive(uint32_t total_bytes) {
  audio_stop();

  if (total_bytes == 0) {
    return false;
  }
  if (total_bytes > AUDIO_MAX_PCM_BYTES) {
    total_bytes = AUDIO_MAX_PCM_BYTES;
  }

  if (s_capacity < total_bytes) {
    free_buffer();
    s_buffer = malloc(total_bytes);
    if (!s_buffer) {
      APP_LOG(APP_LOG_LEVEL_ERROR, "audio: malloc %u failed", (unsigned int)total_bytes);
      return false;
    }
    s_capacity = total_bytes;
  }

  s_length = total_bytes;
  s_received = 0;
  s_play_offset = 0;
  s_receiving = true;
  return true;
}

void audio_receive_chunk(const uint8_t *data, uint32_t len) {
  if (!s_receiving || !s_buffer || len == 0) {
    return;
  }
  if (s_received >= s_length) {
    return;
  }
  uint32_t space = s_length - s_received;
  if (len > space) {
    len = space;
  }
  memcpy(s_buffer + s_received, data, len);
  s_received += len;
}

#if defined(PBL_SPEAKER)
static void drain_timer_cb(void *ctx);

static void schedule_drain(void) {
  if (!s_drain_timer) {
    s_drain_timer = app_timer_register(AUDIO_TIMER_MS, drain_timer_cb, NULL);
  }
}

static void finish_playback(void) {
  if (s_drain_timer) {
    app_timer_cancel(s_drain_timer);
    s_drain_timer = NULL;
  }
  bool was_playing = s_playing;
  s_playing = false;
  if (was_playing) {
    speaker_stream_close();
  }
  notify_done();
}

static void drain_timer_cb(void *ctx) {
  s_drain_timer = NULL;
  if (!s_playing) {
    return;
  }

  while (s_play_offset < s_received) {
    uint32_t remaining = s_received - s_play_offset;
    uint32_t to_write = remaining < AUDIO_SAMPLES_PER_TICK ? remaining : AUDIO_SAMPLES_PER_TICK;
    uint32_t written = speaker_stream_write(s_buffer + s_play_offset, to_write);
    s_play_offset += written;
    if (written < to_write) {
      // Speaker buffer is full; yield and finish writing next tick.
      schedule_drain();
      return;
    }
  }

  if (s_play_offset >= s_length) {
    finish_playback();
    return;
  }
  // All received bytes written but more are still expected; keep ticking.
  schedule_drain();
}
#endif  // PBL_SPEAKER

void audio_play_received(uint8_t speaker_volume) {
  s_receiving = false;

  if (!s_buffer || s_received == 0) {
    notify_done();
    return;
  }

#if defined(PBL_SPEAKER)
  if (!speaker_stream_open(SpeakerPcmFormat_8kHz_8bit, speaker_volume)) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "audio: speaker_stream_open failed");
    notify_done();
    return;
  }
  speaker_set_volume(speaker_volume);
  s_play_offset = 0;
  s_playing = true;
  drain_timer_cb(NULL); // prime the drain loop immediately
#else
  (void)speaker_volume;
  vibes_short_pulse();
  notify_done();
#endif
}

void audio_stop(void) {
  if (s_drain_timer) {
    app_timer_cancel(s_drain_timer);
    s_drain_timer = NULL;
  }
#if defined(PBL_SPEAKER)
  if (s_playing) {
    speaker_stop();
    speaker_stream_close();
  }
#endif
  s_playing = false;
  s_receiving = false;
  s_play_offset = 0;
  s_received = 0;
}
