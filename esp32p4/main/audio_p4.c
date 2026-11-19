// audio_p4.c - the apps' audio.h on the ESP32-P4 board, for the built-in Music Player.
//
// The PicoCalc's sound is two pins (the Pico's GP26 and GP27, P4 GPIO 23 and 22) into an RC filter
// and the amplifier. I2S0 drives them as a PDM DAC, one line per ear: a 1-bit stream the same filter
// turns back into sound (as picocalc-doom-esp32p4 does). A task writes the queued buffers to the I2S
// driver, which blocks while its DMA ring is full, and silence when the queue is empty or paused.
// A frame is two signed 16-bit samples, the left one in the low half.
#include "audio.h"

#include <stdio.h>
#include <string.h>
#include "driver/i2s_pdm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PIN_LEFT  23 // GP26
#define PIN_RIGHT 22 // GP27

#define SILENCE_FRAMES 128
#define TASK_PRIO      5
#define TASK_STACK     3072

static uint32_t s_buf[AUDIO_BUFFERS][AUDIO_FRAMES];
static int s_len[AUDIO_BUFFERS];
static uint32_t s_silence[SILENCE_FRAMES];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile int s_tail;   // the buffer playing (or next to play)
static volatile int s_queued; // buffers queued, the playing one included
static volatile bool s_real;  // a queued buffer is being written (not silence)
static volatile bool s_paused, s_underrun, s_running;
static volatile TaskHandle_t s_task;
static i2s_chan_handle_t s_tx;
static int s_volume = 10;

static void audio_task(void *arg)
{
    (void)arg;
    while (s_running) {
        const void *src = s_silence;
        size_t bytes = sizeof(s_silence);
        taskENTER_CRITICAL(&s_lock);
        s_real = !s_paused && s_queued > 0;
        if (s_real) {
            src = s_buf[s_tail];
            bytes = (size_t)s_len[s_tail] * sizeof(uint32_t);
        }
        taskEXIT_CRITICAL(&s_lock);

        size_t written;
        i2s_channel_write(s_tx, src, bytes, &written, pdMS_TO_TICKS(200));

        if (s_real) {
            taskENTER_CRITICAL(&s_lock);
            if (s_queued > 0) { // audio_flush() may have emptied the queue meanwhile
                s_tail = (s_tail + 1) % AUDIO_BUFFERS;
                s_queued--;
                if (s_queued == 0 && !s_paused) {
                    s_underrun = true;
                }
            }
            s_real = false;
            taskEXIT_CRITICAL(&s_lock);
        }
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

bool audio_start(uint32_t rate)
{
    if (rate < 8000 || rate > 48000) {
        return false;
    }
    audio_stop();

    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.dma_desc_num = 4;
    chan.dma_frame_num = 256;
    chan.auto_clear = true; // silence, not a repeat, if the task is late
    if (i2s_new_channel(&chan, &s_tx, NULL) != ESP_OK) {
        printf("audio: no I2S channel\n");
        return false;
    }
    i2s_pdm_tx_config_t pdm = {
        .clk_cfg = I2S_PDM_TX_CLK_DAC_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_PDM_TX_SLOT_PCM_FMT_DAC_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .clk = I2S_GPIO_UNUSED,
            .dout = PIN_LEFT,
            .dout2 = PIN_RIGHT,
        },
    };
    if (pdm.clk_cfg.up_sample_fs > 480) {
        pdm.clk_cfg.up_sample_fs = 480; // the driver's limit
    }
    pdm.clk_cfg.bclk_div = 8; // the smallest: keeps MCLK under half of the 160 MHz source
    if (i2s_channel_init_pdm_tx_mode(s_tx, &pdm) != ESP_OK || i2s_channel_enable(s_tx) != ESP_OK) {
        printf("audio: PDM output refused at %lu Hz\n", (unsigned long)rate);
        i2s_del_channel(s_tx);
        s_tx = NULL;
        return false;
    }
    memset(s_silence, 0, sizeof(s_silence));
    s_tail = 0;
    s_queued = 0;
    s_real = false;
    s_paused = false;
    s_underrun = false;
    s_running = true;
    TaskHandle_t t;
    if (xTaskCreate(audio_task, "audio", TASK_STACK, NULL, TASK_PRIO, &t) != pdPASS) {
        s_running = false;
        i2s_channel_disable(s_tx);
        i2s_del_channel(s_tx);
        s_tx = NULL;
        return false;
    }
    s_task = t;
    return true;
}

void audio_stop(void)
{
    if (!s_tx) {
        return;
    }
    s_running = false;
    while (s_task) { // it finishes the write it is in (at most one DMA ring)
        vTaskDelay(1);
    }
    i2s_channel_disable(s_tx);
    i2s_del_channel(s_tx);
    s_tx = NULL;
    s_queued = 0;
    s_real = false;
}

uint32_t *audio_buffer(void)
{
    if (!s_running) {
        return NULL;
    }
    taskENTER_CRITICAL(&s_lock);
    int queued = s_queued, head = (s_tail + s_queued) % AUDIO_BUFFERS;
    taskEXIT_CRITICAL(&s_lock);
    return queued < AUDIO_BUFFERS ? s_buf[head] : NULL;
}

void audio_queue(int frames)
{
    if (!s_running || frames <= 0) {
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    if (s_queued < AUDIO_BUFFERS) {
        s_len[(s_tail + s_queued) % AUDIO_BUFFERS] = frames > AUDIO_FRAMES ? AUDIO_FRAMES : frames;
        s_queued++;
    }
    taskEXIT_CRITICAL(&s_lock);
}

void audio_flush(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_queued = s_real ? 1 : 0; // the playing buffer finishes
    taskEXIT_CRITICAL(&s_lock);
}

void audio_pause(bool paused)
{
    s_paused = paused; // takes effect at the end of the playing buffer
}

void audio_set_volume(int volume)
{
    s_volume = volume < 0 ? 0 : volume > AUDIO_VOLUME_MAX ? AUDIO_VOLUME_MAX : volume;
}

int audio_volume(void)
{
    return s_volume;
}

bool audio_underrun(void)
{
    bool u = s_underrun;
    s_underrun = false;
    return u;
}

uint32_t audio_frame(int16_t left, int16_t right)
{
    int32_t l = (int32_t)left * s_volume / AUDIO_VOLUME_MAX;
    int32_t r = (int32_t)right * s_volume / AUDIO_VOLUME_MAX;
    return (uint32_t)(uint16_t)r << 16 | (uint16_t)l;
}
