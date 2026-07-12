#pragma once
#include <pebble.h>

// Audio format: raw signed 8-bit mono PCM at 8 kHz (SpeakerPcmFormat_8kHz_8bit).
// This matches the format the phone resamples user sounds into.
#define AUDIO_SAMPLE_RATE 8000
#define AUDIO_TIMER_MS 40
// Bytes written to the speaker per drain tick (1 byte/sample at 8-bit).
#define AUDIO_SAMPLES_PER_TICK (AUDIO_SAMPLE_RATE * AUDIO_TIMER_MS / 1000) // 320
// Hard cap on a single clip: 4 seconds @ 8 kHz 8-bit = 32000 bytes.
#define AUDIO_MAX_PCM_BYTES (AUDIO_SAMPLE_RATE * 4)

// Speaker volume range. Emery's speaker is loud, so keep the usable range low.
#define AUDIO_VOLUME_MIN 5
#define AUDIO_VOLUME_MAX 40

typedef void (*AudioDoneHandler)(void);

void audio_init(void);
void audio_deinit(void);

// Called when a new clip transfer starts. total_bytes is clamped to
// AUDIO_MAX_PCM_BYTES. Returns false if the receive buffer could not be
// allocated.
bool audio_begin_receive(uint32_t total_bytes);

// Append received PCM bytes to the current clip buffer.
void audio_receive_chunk(const uint8_t *data, uint32_t len);

// Transfer complete: play the buffered clip through the speaker at the given
// speaker volume (already mapped to the AUDIO_VOLUME_MIN..MAX range).
void audio_play_received(uint8_t speaker_volume);

// Stop any playback / receive in progress and reset state.
void audio_stop(void);

bool audio_is_receiving(void);
bool audio_is_playing(void);

// Invoked (on the app task) when playback finishes, is stopped, or cannot start.
void audio_set_done_handler(AudioDoneHandler handler);
