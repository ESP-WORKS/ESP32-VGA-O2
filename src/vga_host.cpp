#include "vga_host.h"
#include "fw.h"
#include "font8x8.h"

#include "dispdrivers/vgadirectcontroller.h"
#include "fabgl.h"

#include "vdc.h"

#include <string.h>
#include <Arduino.h>

using namespace fabgl;

static VGADirectController s_vga;
static volatile UiMode s_mode = UiMode::Menu;
static volatile bool s_loading = false;
static volatile bool s_bw = false;
static volatile uint32_t s_frame = 0;

static char s_title[40];
static char s_status[40];
static char s_rowShow[MENU_ROWS][40];
static uint8_t s_rowSelMask[MENU_ROWS];
static char s_footer[2][40];
static char s_help[30][40];
static char s_msg[48];
static char s_hud[12];
static volatile int s_hudLen = 0;
static char s_loadingTxt[] = "LOADING"; /* DRAM — ISR não lê literal na flash */
static uint32_t s_fpsAcc = 0;
static uint32_t s_fpsT0 = 0;

static uint8_t s_black, s_white, s_yellow, s_dim, s_red;
static uint8_t s_o2pal[16];
static uint8_t s_o2palBw[16];

/* Hand-tuned RGB222 so greys 7/8/15 stay distinct (2600 Pitfall/Frostbite lesson). */
static const uint8_t kRgb222[16][3] = {
    {0, 0, 0}, {0, 1, 3}, {0, 2, 0}, {0, 2, 3},
    {2, 0, 0}, {2, 0, 2}, {2, 2, 0}, {2, 2, 2},
    {1, 1, 1}, {1, 2, 3}, {1, 3, 1}, {1, 3, 3},
    {3, 1, 1}, {3, 2, 3}, {3, 2, 1}, {3, 3, 3},
};

uint8_t vgaRaw(uint8_t r2, uint8_t g2, uint8_t b2) {
  return (uint8_t)s_vga.createRawPixel(RGB222(r2, g2, b2));
}

void vgaRebuildPalette() {
  s_black = vgaRaw(0, 0, 0);
  s_white = vgaRaw(3, 3, 3);
  s_yellow = vgaRaw(3, 3, 0);
  s_dim = vgaRaw(2, 2, 2);
  s_red = vgaRaw(3, 0, 0);
  for (int i = 0; i < 16; i++) {
    uint8_t r = kRgb222[i][0], g = kRgb222[i][1], b = kRgb222[i][2];
    s_o2pal[i] = vgaRaw(r, g, b);
    uint8_t y = (uint8_t)((r * 3 + g * 6 + b * 1 + 5) / 10);
    if (y > 3)
      y = 3;
    /* Keep 7 vs 15 vs 8 apart in B&W too. */
    if (i == 7 && y > 1)
      y = 2;
    if (i == 8 && y > 0)
      y = 1;
    if (i == 15)
      y = 3;
    s_o2palBw[i] = vgaRaw(y, y, y);
  }
}

static inline void IRAM_ATTR putGlyphSlice(uint8_t *dest, int x, unsigned char ch, int gy,
                                           uint8_t fg, uint8_t bg) {
  if (ch < 32 || ch > 127)
    ch = '?';
  uint8_t bits = FONT8X8[ch - 32][gy];
  /* FabGL I2S: pixel X lives at dest[X ^ 2], not dest[X]. dest[x]=p embaralha o glifo. */
  for (int b = 0; b < 8; b++)
    VGA_PIXELINROW(dest, x + b) = (bits & (1u << b)) ? fg : bg;
}

static inline void IRAM_ATTR putStrSlice(uint8_t *dest, int x, const char *s, int maxc, int gy,
                                         uint8_t fg, uint8_t bg) {
  for (int i = 0; i < maxc; i++) {
    char c = s[i];
    if (!c)
      break;
    putGlyphSlice(dest, x + i * 8, (unsigned char)c, gy, fg, bg);
  }
}

/* Packed 4-bit line → 2× horizontal (160→320), I2S dest[x^2]: p1,p1,p0,p0. */
static inline void IRAM_ATTR blitO2Line4(uint8_t *dest, const uint8_t *src, const uint8_t *pal) {
  uint32_t *d = (uint32_t *)dest;
  for (int i = 0; i < BMPPACKW; i++) {
    uint8_t b = src[i];
    uint32_t p0 = pal[b & 15];
    uint32_t p1 = pal[b >> 4];
    *d++ = p1 | (p1 << 8) | (p0 << 16) | (p0 << 24);
  }
}

static void IRAM_ATTR drawScanline(void *arg, uint8_t *dest, int scanLine) {
  (void)arg;
  UiMode mode = s_mode;
  if (mode == UiMode::Game && o2_vscreen) {
    const uint8_t *pal = s_bw ? s_o2palBw : s_o2pal;
    const uint8_t *front = o2_vscreen;
    /* VIDEO REF FW 15: lines 0..239, 2× wide. Do not crop YOFF. */
    int srcY = scanLine + GAME_YOFF;
    if (srcY >= 0 && srcY < BMPH && front)
      blitO2Line4(dest, front + srcY * BMPPACKW, pal);
    else
      memset(dest, s_black, VGA_W);
    /* J1/J2 overlay. FPS is serial-only. */
    if (scanLine < 8 && s_hudLen > 0)
      putStrSlice(dest, 8, s_hud, 8, scanLine, s_yellow, s_black);
  } else if (mode == UiMode::Help) {
    memset(dest, s_black, VGA_W);
    int row = scanLine / 8;
    int gy = scanLine & 7;
    if (row >= 0 && row < 30 && s_help[row][0])
      putStrSlice(dest, 8, s_help[row], MENU_COLS, gy, s_white, s_black);
  } else if (mode == UiMode::Message) {
    memset(dest, s_black, VGA_W);
    int gy;
    if (scanLine >= 112 && scanLine < 120) {
      gy = scanLine - 112;
      putStrSlice(dest, 8, s_msg, MENU_COLS, gy, s_yellow, s_black);
    } else if (scanLine >= 8 && scanLine < 16) {
      gy = scanLine - 8;
      putStrSlice(dest, 8, s_title, MENU_COLS, gy, s_yellow, s_black);
    }
  } else {
    /* Menu */
    memset(dest, s_black, VGA_W);
    int gy = scanLine & 7;
    if (scanLine < 8) {
      putStrSlice(dest, 8, s_title, MENU_COLS, gy, s_yellow, s_black);
    } else if (scanLine < 16) {
      putStrSlice(dest, 8, s_status, MENU_COLS, gy, s_white, s_black);
    } else if (scanLine >= MENU_LIST_Y && scanLine < MENU_LIST_Y + MENU_ROWS * 8) {
      int row = (scanLine - MENU_LIST_Y) / 8;
      bool sel = s_rowSelMask[row] != 0;
      uint8_t fg = sel ? s_black : s_white;
      uint8_t bg = sel ? s_yellow : s_black;
      if (sel)
        memset(dest, bg, VGA_W);
      putStrSlice(dest, 8, s_rowShow[row], MENU_COLS, gy, fg, bg);
    } else if (scanLine >= 216 && scanLine < 224) {
      putStrSlice(dest, 8, s_footer[0], MENU_COLS, gy, s_dim, s_black);
    } else if (scanLine >= 224 && scanLine < 232) {
      putStrSlice(dest, 8, s_footer[1], MENU_COLS, gy, s_dim, s_black);
    }

    if (s_loading) {
      /* Centered black box, 8 px pads, yellow LOADING. List stays around it. */
      const int tw = 7 * 8;
      const int boxW = tw + 16;
      const int boxH = 8 + 16;
      const int boxX = (VGA_W - boxW) / 2;
      const int boxY = (VGA_H - boxH) / 2;
      if (scanLine >= boxY && scanLine < boxY + boxH) {
        for (int x = boxX; x < boxX + boxW; x++)
          VGA_PIXELINROW(dest, x) = s_black;
        if (scanLine >= boxY + 8 && scanLine < boxY + 16)
          putStrSlice(dest, boxX + 8, s_loadingTxt, 7, scanLine - (boxY + 8), s_yellow, s_black);
      }
    }
  }

  if (scanLine == VGA_H - 1)
    s_frame++;
}

void vgaInit() {
  memset(s_title, 0, sizeof(s_title));
  memset(s_status, 0, sizeof(s_status));
  memset(s_rowShow, 0, sizeof(s_rowShow));
  memset(s_rowSelMask, 0, sizeof(s_rowSelMask));
  memset(s_footer, 0, sizeof(s_footer));
  memset(s_help, 0, sizeof(s_help));
  memset(s_msg, 0, sizeof(s_msg));
  memset(s_hud, 0, sizeof(s_hud));
  strncpy(s_title, "ODYSSEY 2  O2EM", sizeof(s_title) - 1);

  s_vga.begin();
  s_vga.setScanlinesPerCallBack(1);
  s_vga.setDrawScanlineCallback(drawScanline);
  s_vga.setResolution(QVGA_320x240_60Hz);
  vgaRebuildPalette();
}

void vgaWaitVsync() {
  uint32_t f = s_frame;
  uint32_t t0 = millis();
  while (s_frame == f && (millis() - t0) < 40)
    delay(1);
}

void vgaSetMode(UiMode m) { s_mode = m; }
UiMode vgaMode() { return s_mode; }
void vgaSetLoading(bool on) { s_loading = on; }
void vgaSetBw(bool on) { s_bw = on; }
bool vgaBw() { return s_bw; }
uint32_t vgaFrame() { return s_frame; }

void vgaCountEmuFrame() {
  s_fpsAcc++;
  uint32_t now = millis();
  if (s_fpsT0 == 0) {
    s_fpsT0 = now;
    return;
  }
  uint32_t dt = now - s_fpsT0;
  if (dt < 1000)
    return;
  unsigned n = (unsigned)((s_fpsAcc * 1000ul) / dt);
  s_fpsAcc = 0;
  s_fpsT0 = now;
  Serial.printf("FPS %u\n", n);
}

static void copyLine(char *dst, size_t n, const char *s) {
  memset(dst, 0, n);
  if (!s)
    return;
  strncpy(dst, s, n - 1);
}

void vgaSetStatus(const char *s) { copyLine(s_status, sizeof(s_status), s); }
void vgaSetMessage(const char *s) { copyLine(s_msg, sizeof(s_msg), s); }
void vgaSetHud(const char *s) {
  copyLine(s_hud, sizeof(s_hud), s);
  s_hudLen = (int)strlen(s_hud);
}
void vgaSetTitle(const char *s) { copyLine(s_title, sizeof(s_title), s); }
void vgaSetHelpLine(int row, const char *s) {
  if (row < 0 || row >= 30)
    return;
  copyLine(s_help[row], sizeof(s_help[row]), s);
}
void vgaClearHelp() { memset(s_help, 0, sizeof(s_help)); }
void vgaSetRowShow(int row, const char *s, bool selected) {
  if (row < 0 || row >= MENU_ROWS)
    return;
  copyLine(s_rowShow[row], sizeof(s_rowShow[row]), s);
  s_rowSelMask[row] = selected ? 1 : 0;
}
void vgaSetSelected(int visRow) {
  for (int i = 0; i < MENU_ROWS; i++)
    s_rowSelMask[i] = (i == visRow) ? 1 : 0;
}
void vgaSetFooter(int line, const char *s) {
  if (line < 0 || line > 1)
    return;
  copyLine(s_footer[line], sizeof(s_footer[line]), s);
}
