#include "ps2kbd.h"
#include "fw.h"
#include "libretro.h"

#include <Arduino.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_intr_alloc.h"

static int s_dat = PIN_KBD_DATA_DEFAULT;
static int s_clk = PIN_KBD_CLK_DEFAULT;

static volatile uint16_t s_shift;
static volatile uint8_t s_bits;
static volatile uint8_t s_gotBat;
static volatile uint8_t s_scanning;
static volatile uint8_t s_needF4;
static volatile uint8_t s_e0;
static volatile uint8_t s_f0;
static volatile uint8_t s_lshift;
static volatile uint8_t s_rshift;

#define QMAX 32
static volatile KeyEvent s_q[QMAX];
static volatile uint8_t s_qh, s_qt;
static uint8_t s_down[512];

int ps2PinData() { return s_dat; }
int ps2PinClk() { return s_clk; }

static const uint16_t kSet2[128] = {
    /* 00 */ 0, RETROK_F9, 0, RETROK_F5, RETROK_F3, RETROK_F1, RETROK_F2, RETROK_F12,
    /* 08 */ 0, RETROK_F10, RETROK_F8, RETROK_F6, RETROK_F4, RETROK_TAB, RETROK_BACKQUOTE, 0,
    /* 10 */ 0, RETROK_LALT, RETROK_LSHIFT, 0, RETROK_LCTRL, RETROK_q, RETROK_1, 0,
    /* 18 */ 0, 0, RETROK_z, RETROK_s, RETROK_a, RETROK_w, RETROK_2, 0,
    /* 20 */ 0, RETROK_c, RETROK_x, RETROK_d, RETROK_e, RETROK_4, RETROK_3, 0,
    /* 28 */ 0, RETROK_SPACE, RETROK_v, RETROK_f, RETROK_t, RETROK_r, RETROK_5, 0,
    /* 30 */ 0, RETROK_n, RETROK_b, RETROK_h, RETROK_g, RETROK_y, RETROK_6, 0,
    /* 38 */ 0, 0, RETROK_m, RETROK_j, RETROK_u, RETROK_7, RETROK_8, 0,
    /* 40 */ 0, RETROK_COMMA, RETROK_k, RETROK_i, RETROK_o, RETROK_0, RETROK_9, 0,
    /* 48 */ 0, RETROK_PERIOD, RETROK_SLASH, RETROK_l, RETROK_SEMICOLON, RETROK_p, RETROK_MINUS, 0,
    /* 50 */ 0, 0, RETROK_QUOTE, 0, RETROK_LEFTBRACKET, RETROK_EQUALS, 0, 0,
    /* 58 */ RETROK_CAPSLOCK, RETROK_RSHIFT, RETROK_RETURN, RETROK_RIGHTBRACKET, 0, RETROK_BACKSLASH, 0, 0,
    /* 60 */ 0, 0, 0, 0, 0, 0, RETROK_BACKSPACE, 0,
    /* 68 */ 0, RETROK_KP1, 0, RETROK_KP4, RETROK_KP7, 0, 0, 0,
    /* 70 */ RETROK_KP0, RETROK_KP_PERIOD, RETROK_KP2, RETROK_KP5, RETROK_KP6, RETROK_KP8, RETROK_ESCAPE, RETROK_NUMLOCK,
    /* 78 */ RETROK_F11, RETROK_KP_PLUS, RETROK_KP3, RETROK_KP_MINUS, RETROK_KP_MULTIPLY, RETROK_KP9, RETROK_SCROLLOCK, 0,
};

static uint8_t fkeyOf(uint16_t k) {
  if (k >= RETROK_F1 && k <= RETROK_F12)
    return (uint8_t)(k - RETROK_F1 + 1);
  return 0;
}

static uint8_t specialOf(uint16_t k) {
  if (k == RETROK_ESCAPE)
    return 1;
  if (k == RETROK_RETURN || k == RETROK_KP_ENTER)
    return 2;
  if (k == RETROK_SPACE)
    return 3;
  if (k == RETROK_UP)
    return 4;
  if (k == RETROK_DOWN)
    return 5;
  if (k == RETROK_LEFT)
    return 6;
  if (k == RETROK_RIGHT)
    return 7;
  return 0;
}

static uint8_t asciiOf(uint16_t k) {
  if (k >= RETROK_a && k <= RETROK_z)
    return (uint8_t)('A' + (k - RETROK_a));
  if (k >= RETROK_0 && k <= RETROK_9)
    return (uint8_t)k;
  if (k == RETROK_SPACE)
    return ' ';
  if (k == RETROK_PERIOD)
    return '.';
  if (k == RETROK_MINUS)
    return '-';
  if (k == RETROK_EQUALS)
    return '=';
  if (k == RETROK_SLASH)
    return '/';
  if (k == RETROK_PLUS || k == RETROK_KP_PLUS)
    return '+';
  if (k == RETROK_ASTERISK || k == RETROK_KP_MULTIPLY)
    return '*';
  if (k == RETROK_QUESTION)
    return '?';
  return 0;
}

static void IRAM_ATTR pushEv(uint16_t code, uint8_t down) {
  uint8_t n = (uint8_t)((s_qh + 1) % QMAX);
  if (n == s_qt)
    return;
  KeyEvent e;
  e.code = code;
  e.ascii = asciiOf(code);
  e.down = down;
  e.fkey = fkeyOf(code);
  e.special = specialOf(code);
  memcpy((void *)&s_q[s_qh], &e, sizeof(e));
  s_qh = n;
}

static void IRAM_ATTR handleByte(uint8_t b) {
  if (b == 0xAA) {
    s_gotBat = 1;
    s_needF4 = 1;
    return;
  }
  if (b == 0xFA || b == 0xFE || b == 0xEE)
    return;
  if (b != 0xF0 && b != 0xE0 && b != 0xAA)
    s_scanning = 1;
  if (b == 0xE0) {
    s_e0 = 1;
    return;
  }
  if (b == 0xF0) {
    s_f0 = 1;
    return;
  }
  uint16_t code = 0;
  if (s_e0) {
    switch (b) {
    case 0x75: code = RETROK_UP; break;
    case 0x72: code = RETROK_DOWN; break;
    case 0x6B: code = RETROK_LEFT; break;
    case 0x74: code = RETROK_RIGHT; break;
    case 0x5A: code = RETROK_KP_ENTER; break;
    case 0x11: code = RETROK_RALT; break; /* O2 NO */
    case 0x14: code = RETROK_RCTRL; break;
    case 0x4A: code = RETROK_KP_DIVIDE; break;
    case 0x71: code = RETROK_DELETE; break; /* O2 CLEAR */
    case 0x70: code = RETROK_INSERT; break;
    case 0x6C: code = RETROK_HOME; break;
    case 0x69: code = RETROK_END; break;
    case 0x7D: code = RETROK_PAGEUP; break;
    case 0x7A: code = RETROK_PAGEDOWN; break;
    default: break;
    }
    s_e0 = 0;
  } else if (b == 0x83) {
    code = RETROK_F7;
  } else if (b < 128) {
    code = kSet2[b];
  }
  uint8_t down = s_f0 ? 0 : 1;
  s_f0 = 0;
  if (code == RETROK_LSHIFT)
    s_lshift = down;
  else if (code == RETROK_RSHIFT)
    s_rshift = down;
  /* Unshifted PC keys → O2 matrix keys that live on their own 8244 positions. */
  if (s_lshift || s_rshift) {
    if (code == RETROK_SLASH)
      code = RETROK_QUESTION;
    else if (code == RETROK_EQUALS)
      code = RETROK_PLUS;
    else if (code == RETROK_8)
      code = RETROK_ASTERISK;
  }
  if (!code)
    return;
  if (code < 512)
    s_down[code] = down;
  pushEv(code, down);
}

static void IRAM_ATTR onClk(void *arg) {
  (void)arg;
  int bit = gpio_get_level((gpio_num_t)s_dat);
  if (s_bits == 0) {
    if (bit != 0)
      return; /* wait start bit 0 */
    s_shift = 0;
    s_bits = 1;
    return;
  }
  if (s_bits >= 1 && s_bits <= 8) {
    if (bit)
      s_shift |= (uint16_t)(1u << (s_bits - 1));
    s_bits++;
    return;
  }
  if (s_bits == 9) {
    s_bits++; /* parity, ignore */
    return;
  }
  /* stop */
  s_bits = 0;
  handleByte((uint8_t)s_shift);
}

static void sendByte(uint8_t v) {
  gpio_set_intr_type((gpio_num_t)s_clk, GPIO_INTR_DISABLE);
  pinMode(s_clk, OUTPUT);
  pinMode(s_dat, OUTPUT);
  digitalWrite(s_clk, LOW);
  delayMicroseconds(60);
  digitalWrite(s_dat, LOW);
  delayMicroseconds(10);
  pinMode(s_clk, INPUT_PULLUP);
  /* wait device to take CLK */
  uint32_t t0 = micros();
  while (digitalRead(s_clk) && (micros() - t0) < 15000) {
  }
  uint8_t parity = 1;
  for (int i = 0; i < 8; i++) {
    int b = (v >> i) & 1;
    digitalWrite(s_dat, b);
    if (b)
      parity ^= 1;
    t0 = micros();
    while (!digitalRead(s_clk) && (micros() - t0) < 15000) {
    }
    t0 = micros();
    while (digitalRead(s_clk) && (micros() - t0) < 15000) {
    }
  }
  digitalWrite(s_dat, parity);
  t0 = micros();
  while (!digitalRead(s_clk) && (micros() - t0) < 15000) {
  }
  t0 = micros();
  while (digitalRead(s_clk) && (micros() - t0) < 15000) {
  }
  pinMode(s_dat, INPUT_PULLUP);
  t0 = micros();
  while (digitalRead(s_dat) && (micros() - t0) < 15000) {
  }
  t0 = micros();
  while (digitalRead(s_clk) && (micros() - t0) < 15000) {
  }
  t0 = micros();
  while ((!digitalRead(s_clk) || !digitalRead(s_dat)) && (micros() - t0) < 15000) {
  }
  gpio_set_intr_type((gpio_num_t)s_clk, GPIO_INTR_NEGEDGE);
}

void ps2Init(int pinData, int pinClk) {
  s_dat = pinData;
  s_clk = pinClk;
  s_bits = 0;
  s_e0 = s_f0 = 0;
  s_lshift = s_rshift = 0;
  s_qh = s_qt = 0;
  s_gotBat = 0;
  s_needF4 = 0;
  memset((void *)s_down, 0, sizeof(s_down));
  pinMode(s_dat, INPUT_PULLUP);
  pinMode(s_clk, INPUT_PULLUP);
  gpio_install_isr_service(0);
  gpio_set_intr_type((gpio_num_t)s_clk, GPIO_INTR_NEGEDGE);
  gpio_isr_handler_add((gpio_num_t)s_clk, onClk, nullptr);
}

void ps2PollHost() {
  /* Host commands only after BAT 0xAA, or if the keyboard is already scanning. */
  if (s_needF4 && s_gotBat) {
    s_needF4 = 0;
    sendByte(0xF4);
  }
}

bool ps2Pop(KeyEvent *ev) {
  if (s_qh == s_qt)
    return false;
  memcpy(ev, (const void *)&s_q[s_qt], sizeof(*ev));
  s_qt = (uint8_t)((s_qt + 1) % QMAX);
  return true;
}

bool ps2Down(uint16_t retroKey) {
  if (retroKey >= 512)
    return false;
  return s_down[retroKey] != 0;
}

void ps2Clear() {
  memset((void *)s_down, 0, sizeof(s_down));
  s_qh = s_qt = 0;
}
