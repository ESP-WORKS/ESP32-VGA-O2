#pragma once
#include <stdint.h>

struct KeyEvent {
  uint16_t code; /* RETROK_* or 0 */
  uint8_t ascii; /* 0 if none */
  uint8_t down;
  uint8_t fkey;  /* 1..12 for F1-F12, 0 otherwise */
  uint8_t special; /* 1=esc 2=enter 3=space 4=up 5=down 6=left 7=right */
};

void ps2Init(int pinData, int pinClk);
void ps2PollHost(); /* send F4 after BAT, from task (not ISR) */
bool ps2Pop(KeyEvent *ev);
bool ps2Down(uint16_t retroKey);
void ps2Clear();
int ps2PinData();
int ps2PinClk();
