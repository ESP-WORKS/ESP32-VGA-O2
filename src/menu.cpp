#include "menu.h"
#include "fw.h"
#include "vga_host.h"
#include "sd_host.h"
#include "ps2kbd.h"
#include "audio_host.h"
#include "o2_host.h"
#include "libretro.h"

#include <Arduino.h>
#include <string.h>
#include <ctype.h>

static int s_sel = 0;
static int s_page = 0;
static bool s_help = false;
static uint32_t s_f5t = 0;
static char s_lastRom[64];

static int pageCount() {
  int n = g_sd.romCount;
  if (n <= 0)
    return 1;
  return (n + MENU_ROWS - 1) / MENU_ROWS;
}

static void clipName(char *dst, const char *src) {
  int n = (int)strlen(src);
  if (n <= MENU_COLS) {
    strncpy(dst, src, MENU_COLS);
    dst[MENU_COLS] = 0;
    return;
  }
  memcpy(dst, src, MENU_COLS - 1);
  dst[MENU_COLS - 1] = '>';
  dst[MENU_COLS] = 0;
}

static void showLoading() {
  vgaSetLoading(true);
  vgaWaitVsync();
}

static void hideLoading() { vgaSetLoading(false); }

static void refreshPage() {
  int pages = pageCount();
  if (s_page >= pages)
    s_page = pages - 1;
  if (s_page < 0)
    s_page = 0;
  int base = s_page * MENU_ROWS;
  for (int i = 0; i < MENU_ROWS; i++)
    vgaSetRowShow(i, "", false);

  char name[96];
  char show[40];
  int vis = g_sd.romCount - base;
  if (vis > MENU_ROWS)
    vis = MENU_ROWS;
  if (vis < 0)
    vis = 0;
  for (int i = 0; i < vis; i++) {
    sdReadName(base + i, name, sizeof(name));
    clipName(show, name);
    vgaSetRowShow(i, show, (base + i) == s_sel);
  }
  if (g_sd.romCount == 0) {
    if (!g_sd.mounted)
      vgaSetRowShow(0, "SD not found", false);
    else if (!g_sd.folderOk)
      vgaSetRowShow(0, "/ODYSSEY2 folder not found", false);
    else if (!o2BiosPresent())
      vgaSetRowShow(0, "BIOS missing (o2rom.bin)", false);
    else
      vgaSetRowShow(0, "No ROMs in /ODYSSEY2", false);
  }

  char st[40];
  sdFormatStatus(st, sizeof(st));
  if (g_sd.mounted && g_sd.folderOk && !o2BiosPresent())
    snprintf(st, sizeof(st), "BIOS missing (o2rom.bin)");
  vgaSetStatus(st);

  char f0[40], f1[40];
  snprintf(f0, sizeof(f0), "Page %d/%d  <> page  A-Z jump  Enter", s_page + 1, pages);
  snprintf(f1, sizeof(f1), "F1 help  A-Z jump  F12 ESP reset");
  vgaSetFooter(0, f0);
  vgaSetFooter(1, f1);
  vgaSetTitle("ODYSSEY 2  O2EM");
}

static void jumpLetter(char L) {
  L = (char)toupper((unsigned char)L);
  int idx = sdFindLetter(L);
  if (idx < 0)
    return;
  s_sel = idx;
  int np = s_sel / MENU_ROWS;
  if (np != s_page) {
    showLoading();
    s_page = np;
    refreshPage();
    hideLoading();
  } else {
    refreshPage();
  }
}

static void changePage(int dir) {
  int np = s_page + dir;
  int pc = pageCount();
  if (np < 0)
    np = 0;
  if (np >= pc)
    np = pc - 1;
  if (np == s_page)
    return;
  showLoading();
  s_page = np;
  s_sel = s_page * MENU_ROWS;
  if (s_sel >= g_sd.romCount && g_sd.romCount)
    s_sel = g_sd.romCount - 1;
  refreshPage();
  hideLoading();
}

static void loadSelected() {
  if (g_sd.romCount <= 0)
    return;
  if (!o2BiosPresent()) {
    vgaSetMode(UiMode::Message);
    vgaSetMessage("BIOS missing (o2rom.bin)");
    delay(1200);
    vgaSetMode(UiMode::Menu);
    return;
  }
  char name[96];
  sdReadName(s_sel, name, sizeof(name));
  char err[64];
  showLoading();
  bool ok = o2LoadRom(name, err, sizeof(err));
  hideLoading();
  if (!ok) {
    vgaSetMode(UiMode::Message);
    vgaSetMessage(err);
    delay(1500);
    vgaSetMode(UiMode::Menu);
    refreshPage();
    return;
  }
  strncpy(s_lastRom, name, sizeof(s_lastRom) - 1);
  vgaSetHud(o2JoyPlayer() ? "J2" : "J1");
  vgaSetMode(UiMode::Game);
}

static const char *kHelp[] = {
    "ODYSSEY 2 EMULATOR FOR ESP32 by FG1998",
    "",
    "Based on O2EM by Daniel Boris",
    "",
    "F1   Help",
    "F2   Color / Black-and-white",
    "F3   Frameskip  0=off  1=leve  2=agressivo",
    "     (off if the game is already fast)",
    "F4   1st player fire hold",
    "F5   RESET do jogo (segure)",
    "F6   Joystick J1 / J2",
    "     QAOP+Space and Arrows+Space",
    "F7   Rebuild ROM cache",
    "F9   ROM menu  (/ODYSSEY2 on SD)",
    "F10  Reload game",
    "F11  Game RESET",
    "F12  ESP32 reset (exit to Bootloader)",
    "",
    "In game: J1/J2 top-left",
    "O2: A-Z 0-9 Space . - = /  ?=Shift+/",
    "    +=Shift+=  *=Shift+8  CLEAR=Bksp",
    "    YES=Alt  NO=AltGr  ENTER=Enter",
    "",
    "In the menu: up/down selects",
    "             left/right page (24)",
    "             A-Z jumps to letter",
    "             Enter/Space loads",
    "ESC returns to the game",
};

void menuShowHelp() {
  s_help = true;
  vgaClearHelp();
  int n = (int)(sizeof(kHelp) / sizeof(kHelp[0]));
  for (int i = 0; i < n && i < 30; i++)
    vgaSetHelpLine(i, kHelp[i]);
  vgaSetMode(UiMode::Help);
}

void menuLeaveHelp() {
  s_help = false;
  if (o2Loaded())
    vgaSetMode(UiMode::Game);
  else
    vgaSetMode(UiMode::Menu);
}

static void rebuildCache() {
  showLoading();
  vgaSetStatus("Building file cache");
  vgaWaitVsync();
  sdRebuildCache();
  s_sel = s_page = 0;
  refreshPage();
  hideLoading();
}

static void handleFkey(uint8_t fk, bool down) {
  if (!down)
    return;
  switch (fk) {
  case 1:
    if (s_help)
      menuLeaveHelp();
    else
      menuShowHelp();
    break;
  case 2:
    vgaSetBw(!vgaBw());
    break;
  case 3:
    o2SetFrameskip((o2Frameskip() + 1) % 3);
    break;
  case 6:
    o2SetJoyPlayer(o2JoyPlayer() ^ 1);
    vgaSetHud(o2JoyPlayer() ? "J2" : "J1");
    break;
  case 7:
    rebuildCache();
    if (s_help)
      menuLeaveHelp();
    vgaSetMode(UiMode::Menu);
    break;
  case 9:
    s_help = false;
    vgaSetMode(UiMode::Menu);
    refreshPage();
    break;
  case 10:
    if (s_lastRom[0]) {
      char err[64];
      o2LoadRom(s_lastRom, err, sizeof(err));
      vgaSetMode(o2Loaded() ? UiMode::Game : UiMode::Menu);
    }
    break;
  case 11:
    o2Reset();
    break;
  case 12:
    audioShutdown();
    ESP.restart();
    break;
  default:
    break;
  }
}

static void handleMenuKey(const KeyEvent &e) {
  if (!e.down)
    return;
  if (e.fkey) {
    handleFkey(e.fkey, true);
    return;
  }
  if (s_help) {
    if (e.special == 1 || e.fkey == 1)
      menuLeaveHelp();
    return;
  }
  if (e.special == 4) { /* up — instant, no SD */
    if (s_sel > 0)
      s_sel--;
    if (s_sel / MENU_ROWS != s_page) {
      showLoading();
      s_page = s_sel / MENU_ROWS;
      refreshPage();
      hideLoading();
    } else {
      vgaSetSelected(s_sel - s_page * MENU_ROWS);
    }
  } else if (e.special == 5) {
    if (s_sel + 1 < g_sd.romCount)
      s_sel++;
    if (s_sel / MENU_ROWS != s_page) {
      showLoading();
      s_page = s_sel / MENU_ROWS;
      refreshPage();
      hideLoading();
    } else {
      vgaSetSelected(s_sel - s_page * MENU_ROWS);
    }
  } else if (e.special == 6)
    changePage(-1);
  else if (e.special == 7)
    changePage(1);
  else if (e.special == 2 || e.special == 3)
    loadSelected();
  else if (e.special == 1) {
    if (o2Loaded())
      vgaSetMode(UiMode::Game);
  } else if (e.ascii >= 'A' && e.ascii <= 'Z')
    jumpLetter((char)e.ascii);
  else if (e.ascii >= '0' && e.ascii <= '9')
    jumpLetter((char)e.ascii);
}

static void handleGameKey(const KeyEvent &e) {
  if (e.fkey) {
    if (e.fkey == 4) {
      o2SetKey(RETROK_SPACE, e.down);
      return;
    }
    if (e.fkey == 5) {
      if (e.down)
        s_f5t = millis();
      else
        s_f5t = 0;
      return;
    }
    handleFkey(e.fkey, e.down);
    return;
  }
  if (e.special == 1 && e.down) {
    vgaSetMode(UiMode::Menu);
    refreshPage();
    return;
  }
  if (e.code)
    o2SetKey(e.code, e.down);
  /* F4 = extra P1 fire hold */
  if (e.fkey == 4)
    o2SetKey(RETROK_SPACE, e.down);
}

void emuTask(void *arg) {
  (void)arg;
  disableCore0WDT();
  vgaSetMode(UiMode::Menu);
  vgaSetTitle("ODYSSEY 2  O2EM");
  vgaSetStatus("Mounting SD...");

  sdMount();
  ps2Init(g_kbdData, g_kbdClk);

  if (g_sd.miso == PIN_AUDIO_DAC)
    audioEnable(false);
  else
    audioInit();

  if (g_magicb >= 0) {
    pinMode(g_magicb, INPUT);
  }

  if (g_sd.folderOk) {
    if (!sdCacheExists()) {
      vgaSetStatus("Building file cache");
      vgaWaitVsync();
    }
    sdLoadCache(false);
  }
  o2LoadBios();
  refreshPage();

  uint32_t lastMagic = 0;
  for (;;) {
    ps2PollHost();
    KeyEvent e;
    while (ps2Pop(&e)) {
      if (vgaMode() == UiMode::Game)
        handleGameKey(e);
      else
        handleMenuKey(e);
    }

    if (g_magicb >= 0) {
      int v = digitalRead(g_magicb);
      if (v == LOW && (millis() - lastMagic) > 400) {
        lastMagic = millis();
        s_help = false;
        vgaSetMode(UiMode::Menu);
        refreshPage();
      }
    }

    if (vgaMode() == UiMode::Game) {
      if (s_f5t && (millis() - s_f5t) > 800) {
        s_f5t = 0;
        o2Reset();
      }
      o2RunFrame();
      vgaWaitVsync();
      o2Present();
    } else {
      vgaWaitVsync();
    }
  }
}
