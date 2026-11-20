#include "audio_host.h"
#include "fw.h"

#include <Arduino.h>
#include "driver/timer.h"
#include "driver/dac.h"
#include "driver/gpio.h"
#include <string.h>
#include <stdlib.h>

static volatile uint8_t s_ring[AUDIO_RING];
static volatile uint16_t s_r, s_w;
static volatile uint16_t s_queued;
static volatile int s_lpf;
static volatile bool s_on = false;
static bool s_inited = false;

static void IRAM_ATTR onTimer(void *arg) {
  (void)arg;
  uint8_t s = 0x80;
  if (s_on && s_r != s_w) {
    s = s_ring[s_r];
    s_r = (uint16_t)((s_r + 1) & (AUDIO_RING - 1));
    if (s_queued)
      s_queued--;
  }
  /* 1-pole LPF in the ISR (kills DAC hiss). */
  s_lpf += ((int)s - s_lpf) >> 2;
  int o = s_lpf;
  if (o < 0)
    o = 0;
  if (o > 255)
    o = 255;
  dac_output_voltage(DAC_CHANNEL_1, (uint8_t)o);
  timer_group_clr_intr_status_in_isr(TIMER_GROUP_0, TIMER_1);
  timer_group_enable_alarm_in_isr(TIMER_GROUP_0, TIMER_1);
}

void audioInit() {
  if (s_inited)
    return;
  s_r = s_w = 0;
  s_queued = 0;
  s_lpf = 0x80;
  s_on = true;
  dac_output_enable(DAC_CHANNEL_1);
  dac_output_voltage(DAC_CHANNEL_1, 0x80);

  timer_config_t cfg = {
      .alarm_en = TIMER_ALARM_EN,
      .counter_en = TIMER_PAUSE,
      .intr_type = TIMER_INTR_LEVEL,
      .counter_dir = TIMER_COUNT_UP,
      .auto_reload = TIMER_AUTORELOAD_EN,
      .divider = 80, /* 1 MHz */
  };
  timer_init(TIMER_GROUP_0, TIMER_1, &cfg);
  timer_set_counter_value(TIMER_GROUP_0, TIMER_1, 0);
  timer_set_alarm_value(TIMER_GROUP_0, TIMER_1, 45); /* ~22222 Hz ~ 22050 */
  timer_enable_intr(TIMER_GROUP_0, TIMER_1);
  timer_isr_register(TIMER_GROUP_0, TIMER_1, onTimer, nullptr, ESP_INTR_FLAG_IRAM, nullptr);
  timer_start(TIMER_GROUP_0, TIMER_1);
  s_inited = true;
}

void audioShutdown() {
  if (!s_inited)
    return;
  s_on = false;
  timer_pause(TIMER_GROUP_0, TIMER_1);
  timer_disable_intr(TIMER_GROUP_0, TIMER_1);
  dac_output_voltage(DAC_CHANNEL_1, 0x80);
  dac_output_disable(DAC_CHANNEL_1);
  gpio_reset_pin((gpio_num_t)PIN_AUDIO_DAC);
  s_inited = false;
}

void audioEnable(bool on) { s_on = on; }
bool audioEnabled() { return s_on; }

void audioCloseAndShrink() {
  s_r = s_w = 0;
  s_queued = 0;
  s_lpf = 0x80;
}

void audioPush(const uint8_t *pcm, int n) {
  if (!s_on || !pcm || n <= 0)
    return;
  for (int i = 0; i < n; i++) {
    if (s_queued >= AUDIO_QUEUE_CAP)
      return;
    uint16_t nw = (uint16_t)((s_w + 1) & (AUDIO_RING - 1));
    if (nw == s_r)
      return;
    s_ring[s_w] = pcm[i];
    s_w = nw;
    s_queued++;
  }
}

void audioIdle() {
  /* nothing — ISR holds DC 0x80 when empty */
}
