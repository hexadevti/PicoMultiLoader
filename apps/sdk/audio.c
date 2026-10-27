// audio.c - PWM audio (audio.h). Both pins are the two channels of one PWM slice, so a 32-bit
// write to its compare register sets left and right at once: a DMA channel paced by a DMA timer
// writes one buffer word per sample, and its interrupt starts the next queued buffer (or a short
// silent one when the queue is empty or paused).
#include "audio.h"

#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/sync.h"

#define PWM_TOP     1023 // 10-bit levels: ~122 kHz carrier at 125 MHz
#define MID         ((PWM_TOP + 1) / 2)
#define MID_FRAME   ((uint32_t)MID << 16 | MID)
#define SILENCE_LEN 64

static uint32_t s_buf[AUDIO_BUFFERS][AUDIO_FRAMES];
static int s_len[AUDIO_BUFFERS];
static uint32_t s_silence[SILENCE_LEN];
static volatile int s_tail;    // the buffer playing (or next to play)
static volatile int s_queued;  // buffers queued, the playing one included
static volatile bool s_real;   // a queued buffer is playing (not silence)
static volatile bool s_paused, s_underrun;
static int s_dma = -1, s_timer = -1;
static uint s_slice;
static bool s_running;
static int s_volume = 10;

static void start_next(void)
{
    if (!s_paused && s_queued > 0) {
        s_real = true;
        dma_channel_transfer_from_buffer_now(s_dma, s_buf[s_tail], s_len[s_tail]);
    } else {
        s_real = false;
        dma_channel_transfer_from_buffer_now(s_dma, s_silence, SILENCE_LEN);
    }
}

static void __isr dma_done(void)
{
    if (!(dma_hw->ints1 & (1u << s_dma))) {
        return;
    }
    dma_hw->ints1 = 1u << s_dma;
    if (s_real) {
        s_tail = (s_tail + 1) % AUDIO_BUFFERS;
        s_queued--;
        if (s_queued == 0 && !s_paused) {
            s_underrun = true;
        }
    }
    start_next();
}

// numerator / denominator of the DMA timer: sys_clk * num / den is closest to `rate`.
static void set_rate(uint32_t rate)
{
    uint32_t sys = clock_get_hz(clk_sys);
    uint32_t best_num = 1, best_den = sys / rate;
    uint64_t best_err = UINT64_MAX;
    for (uint32_t den = 1; den <= 0xFFFF; den++) {
        uint64_t num = ((uint64_t)rate * den + sys / 2) / sys;
        if (num == 0 || num > 0xFFFF) {
            continue;
        }
        uint64_t got = (uint64_t)sys * num / den;
        uint64_t err = got > rate ? got - rate : rate - got;
        if (err < best_err) {
            best_err = err;
            best_num = (uint32_t)num;
            best_den = den;
            if (err == 0) {
                break;
            }
        }
    }
    dma_timer_set_fraction(s_timer, (uint16_t)best_num, (uint16_t)best_den);
}

bool audio_start(uint32_t rate)
{
    if (rate < 8000 || rate > 48000) {
        return false;
    }
    if (s_dma < 0) {
        gpio_set_function(AUDIO_LEFT_PIN, GPIO_FUNC_PWM);
        gpio_set_function(AUDIO_RIGHT_PIN, GPIO_FUNC_PWM);
        s_slice = pwm_gpio_to_slice_num(AUDIO_LEFT_PIN);
        pwm_config pc = pwm_get_default_config();
        pwm_config_set_wrap(&pc, PWM_TOP);
        pwm_config_set_clkdiv(&pc, 1.0f);
        pwm_init(s_slice, &pc, true);
        pwm_hw->slice[s_slice].cc = MID_FRAME;
        for (int i = 0; i < SILENCE_LEN; i++) {
            s_silence[i] = MID_FRAME;
        }
        s_dma = dma_claim_unused_channel(true);
        s_timer = dma_claim_unused_timer(true);
        irq_set_exclusive_handler(DMA_IRQ_1, dma_done);
    }
    audio_stop();
    set_rate(rate);
    dma_channel_config c = dma_channel_get_default_config(s_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, dma_get_timer_dreq(s_timer));
    dma_channel_configure(s_dma, &c, &pwm_hw->slice[s_slice].cc, s_silence, SILENCE_LEN, false);
    s_tail = 0;
    s_queued = 0;
    s_paused = false;
    s_underrun = false;
    dma_channel_set_irq1_enabled(s_dma, true);
    irq_set_enabled(DMA_IRQ_1, true);
    s_running = true;
    start_next();
    return true;
}

void audio_stop(void)
{
    if (!s_running) {
        return;
    }
    s_running = false;
    dma_channel_set_irq1_enabled(s_dma, false);
    dma_channel_abort(s_dma);
    dma_hw->ints1 = 1u << s_dma;
    pwm_hw->slice[s_slice].cc = MID_FRAME;
    s_queued = 0;
    s_real = false;
}

uint32_t *audio_buffer(void)
{
    if (!s_running) {
        return NULL;
    }
    uint32_t irq = save_and_disable_interrupts();
    int queued = s_queued, head = (s_tail + s_queued) % AUDIO_BUFFERS;
    restore_interrupts(irq);
    return queued < AUDIO_BUFFERS ? s_buf[head] : NULL;
}

void audio_queue(int frames)
{
    if (!s_running || frames <= 0) {
        return;
    }
    uint32_t irq = save_and_disable_interrupts();
    if (s_queued < AUDIO_BUFFERS) {
        s_len[(s_tail + s_queued) % AUDIO_BUFFERS] = frames > AUDIO_FRAMES ? AUDIO_FRAMES : frames;
        s_queued++;
    }
    restore_interrupts(irq);
}

void audio_flush(void)
{
    uint32_t irq = save_and_disable_interrupts();
    s_queued = s_real ? 1 : 0; // the playing buffer finishes
    restore_interrupts(irq);
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
    int32_t l = ((int32_t)left * s_volume / AUDIO_VOLUME_MAX + 32768) >> 6;
    int32_t r = ((int32_t)right * s_volume / AUDIO_VOLUME_MAX + 32768) >> 6;
    return (uint32_t)r << 16 | (uint32_t)l;
}
