#pragma once
#include <stdint.h>

enum class UiMode : uint8_t {
  Menu = 0,
  Help,
  Game,
  Message
};

void vgaInit();
void vgaWaitVsync();
void vgaSetMode(UiMode m);
UiMode vgaMode();
void vgaSetLoading(bool on);
void vgaSetBw(bool on);
bool vgaBw();
void vgaRebuildPalette();
void vgaSetStatus(const char *s);
void vgaSetMessage(const char *s);
void vgaSetHud(const char *s);
void vgaSetHelpLine(int row, const char *s);
void vgaClearHelp();
void vgaSetRowShow(int row, const char *s, bool selected);
void vgaSetSelected(int visRow); /* highlight only — no SD */
void vgaSetFooter(int line, const char *s);
void vgaSetTitle(const char *s);
uint8_t vgaRaw(uint8_t r2, uint8_t g2, uint8_t b2);
uint32_t vgaFrame();
void vgaCountEmuFrame();
