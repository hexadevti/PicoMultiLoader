// audio.h - stereo sound out of the PicoCalc's speakers / headphone jack: PWM on GPIO 26 (left)
// and 27 (right), fed by DMA from a queue of buffers at the sample rate.
//
//   audio_start(44100);
//   uint32_t *buf;
//   while ((buf = audio_buffer()) != NULL) {    // in the main loop: keep the queue full
//       for (int i = 0; i < AUDIO_FRAMES; i++) buf[i] = audio_frame(left[i], right[i]);
//       audio_queue(AUDIO_FRAMES);
//   }
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define AUDIO_LEFT_PIN  26
#define AUDIO_RIGHT_PIN 27
#define AUDIO_FRAMES    1024 // per buffer
#define AUDIO_BUFFERS   6    // queued: ~140 ms at 44.1 kHz
#define AUDIO_VOLUME_MAX 16

/** Start the output at `rate` Hz (8000..48000), silent until buffers are queued. */
bool audio_start(uint32_t rate);

/** Stop and leave the outputs at the middle level. */
void audio_stop(void);

/** A free buffer of AUDIO_FRAMES frames to fill, or NULL if the queue is full. */
uint32_t *audio_buffer(void);

/** Queue the buffer audio_buffer() returned, holding `frames` frames. */
void audio_queue(int frames);

/** Drop every queued buffer (e.g. after a seek). */
void audio_flush(void);

/** Pause or resume the output (paused, it holds the middle level). */
void audio_pause(bool paused);

/** 0..AUDIO_VOLUME_MAX, used by audio_frame(). */
void audio_set_volume(int volume);
int audio_volume(void);

/** True if the output ran out of buffers since the last call. */
bool audio_underrun(void);

/** One frame for a buffer from two signed 16-bit samples, at the current volume. */
uint32_t audio_frame(int16_t left, int16_t right);
