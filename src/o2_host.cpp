#include "o2_host.h"
#include "fw.h"
#include "sd_host.h"
#include "audio_host.h"
#include "vga_host.h"
#include "crc32.h"
#include "libretro.h"
#include "wrapalleg.h"

#include <Arduino.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include "esp_heap_caps.h"

extern "C" {
#include "vmachine.h"
#include "cpu.h"
#include "audio.h"
#include "vdc.h"
#include "keyboard.h"
#include "config.h"
}

int RLOOP = 0;
uint8_t soundBuffer[SOUND_BUFFER_LEN];
int joystick_data[2][5];

extern "C" void update_joy(void) {}

static bool s_loaded = false;
static bool s_biosOk = false;
static bool s_euro = false;
static int s_frameskip = 0;
static int s_joy = 0;
static char s_romName[64];
static char s_biosErr[48];
static int s_skipAcc = 0;
static int s_palAcc = 0;

static uint32_t crc32_file_mem(const uint8_t *p, size_t n) { return crc32_buf(p, n); }

bool o2BiosPresent() { return s_biosOk; }
const char *o2BiosError() { return s_biosErr; }
const char *o2RomName() { return s_romName; }
bool o2Loaded() { return s_loaded; }
void o2SetEuro(bool euro) { s_euro = euro; }
bool o2Euro() { return s_euro; }
void o2SetFrameskip(int n) { s_frameskip = n < 0 ? 0 : (n > 2 ? 2 : n); }
int o2Frameskip() { return s_frameskip; }
void o2SetJoyPlayer(int n) { s_joy = n & 1; }
int o2JoyPlayer() { return s_joy; }

static void holeMsg(char *err, int errMax, size_t want) {
  size_t hole = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  snprintf(err, errMax, "Out of RAM (%uK, hole %u)", (unsigned)((want + 1023) / 1024),
           (unsigned)hole);
}

bool o2LoadBios() {
  s_biosOk = false;
  strncpy(s_biosErr, "BIOS missing (o2rom.bin)", sizeof(s_biosErr) - 1);
  char path[48];
  if (!sdBiosPath(path, sizeof(path)))
    return false;
  size_t n = 0;
  if (!sdReadFile(path, rom_table[0], 1024, &n) || n != 1024) {
    strncpy(s_biosErr, "BIOS missing (o2rom.bin)", sizeof(s_biosErr) - 1);
    return false;
  }
  for (int i = 1; i < 8; i++)
    memcpy(rom_table[i], rom_table[0], 1024);
  uint32_t crc = crc32_file_mem(rom_table[0], 1024);
  app_data.vpp = 0;
  switch (crc) {
  case 0x8016A315:
    app_data.bios = ROM_O2;
    s_euro = false;
    break;
  case 0xE20A9F41:
    app_data.bios = ROM_G7400;
    app_data.vpp = 1;
    s_euro = true;
    break;
  case 0xA318E8D6:
    app_data.bios = ROM_C52;
    s_euro = true;
    break;
  case 0x11647CA5:
    app_data.bios = ROM_JOPAC;
    app_data.vpp = 1;
    s_euro = true;
    break;
  default:
    app_data.bios = ROM_UNKNOWN;
    break;
  }
  s_biosOk = true;
  s_biosErr[0] = 0;
  return true;
}

static bool looksPickAxe(const char *n) {
  char u[80];
  int i = 0;
  for (; n[i] && i < 79; i++)
    u[i] = (char)toupper((unsigned char)n[i]);
  u[i] = 0;
  if (strstr(u, "DIDI") || strstr(u, "ENCANTADA"))
    return true;
  if (strstr(u, "PICKAXE") || strstr(u, "PICK AXE") || strstr(u, "PICK-AXE"))
    return true;
  return false;
}

static size_t trimCartImage(uint8_t *buf, size_t n) {
  /* Some dumps carry a 16-byte header; O2EM wants a multiple of 1024. */
  if (n > 16 && (n % 1024) == 16) {
    memmove(buf, buf + 16, n - 16);
    n -= 16;
  }
  /* 8 KB copies of a 4 KB cart (common "Didi" dump) must not use 4-bank mode. */
  if (n == 8192) {
    int blank = 1;
    for (size_t i = 4096; i < 8192; i++) {
      if (buf[i]) {
        blank = 0;
        break;
      }
    }
    if (blank || memcmp(buf, buf + 4096, 4096) == 0)
      n = 4096;
  }
  return n;
}

static bool nameLooksPal(const char *n) {
  char u[80];
  int i = 0;
  for (; n[i] && i < 79; i++)
    u[i] = (char)toupper((unsigned char)n[i]);
  u[i] = 0;
  if (strstr(u, "(PAL)") || strstr(u, "(EU)") || strstr(u, "(G7000)") || strstr(u, "(EUROPE)"))
    return true;
  if (strstr(u, "(U)") || strstr(u, "(NTSC)") || strstr(u, "(USA)"))
    return false;
  return s_euro;
}

static bool loadCart(const uint8_t *data, size_t size, char *err, int errMax) {
  int bios = app_data.bios;
  int vpp = app_data.vpp;
  memset(&app_data, 0, sizeof(app_data));
  app_data.bios = bios;
  app_data.vpp = vpp;
  app_data.stick[0] = app_data.stick[1] = 1;
  app_data.limit = 1;
  app_data.sound_en = 1;
  app_data.speed = 100;
  app_data.filter = 0;
  app_data.euro = s_euro ? 1 : 0;
  app_data.crc = crc32_file_mem(data, size);

  if ((size % 1024) != 0) {
    strncpy(err, "ROM not found", errMax - 1);
    return false;
  }
  if (size > O2_ROM_MAX) {
    strncpy(err, "ROM too big (16K max)", errMax - 1);
    return false;
  }

  int nb;
  if ((size % 3072) == 0) {
    app_data.three_k = 1;
    nb = (int)(size / 3072);
    if (nb > 8)
      nb = 8;
    const uint8_t *p = data;
    for (int i = nb - 1; i >= 0; i--) {
      memcpy(&rom_table[i][1024], p, 3072);
      p += 3072;
    }
  } else {
    nb = (int)(size / 2048);
    if (nb < 1)
      nb = 1;
    if (nb > 8)
      nb = 8;
    const uint8_t *p = data;
    for (int i = nb - 1; i >= 0; i--) {
      memcpy(&rom_table[i][1024], p, 2048);
      p += 2048;
      memcpy(&rom_table[i][3072], &rom_table[i][2048], 1024);
    }
  }
  rom = rom_table[0];
  if (nb == 1)
    app_data.bank = 1;
  else if (nb == 2)
    app_data.bank = 2;
  else if (nb == 4)
    app_data.bank = 3;
  else
    app_data.bank = 4;
  return true;
}

bool o2LoadRom(const char *filename, char *err, int errMax) {
  o2Unload();
  if (!s_biosOk && !o2LoadBios()) {
    strncpy(err, "BIOS missing (o2rom.bin)", errMax - 1);
    return false;
  }
  char path[96];
  snprintf(path, sizeof(path), ROM_DIR "/%s", filename);
  size_t want = O2_ROM_MAX;
  uint8_t *buf = (uint8_t *)malloc(want);
  if (!buf) {
    holeMsg(err, errMax, want);
    return false;
  }
  size_t n = 0;
  if (!sdReadFile(path, buf, want, &n) || n == 0) {
    free(buf);
    strncpy(err, "ROM not found", errMax - 1);
    return false;
  }
  s_euro = nameLooksPal(filename);
  n = trimCartImage(buf, n);
  if (!loadCart(buf, n, err, errMax)) {
    free(buf);
    return false;
  }
  /* Steal: cart already copied into rom_table. Free the SD buffer. */
  free(buf);

  if (!init_display()) {
    holeMsg(err, errMax, (size_t)BMPPACKW * BMPH * 2 + (size_t)BMPW * BMPH);
    return false;
  }
  app_data.euro = s_euro ? 1 : 0;
  if (!init_system()) {
    close_display();
    strncpy(err, "init_system failed", errMax - 1);
    return false;
  }
  /* Pick Axe Pete / Didi: O2EM needs ~2× vblank work. CRC 0xD62814A3 is
   * the US/EU dump; Brazilian filenames or padded 8K often miss that CRC. */
  if (looksPickAxe(filename) || app_data.crc == 0xD62814A3 ||
      app_data.crc == 0xB2FFB353 || app_data.crc == 0x81C20196)
    evblclk = 12000;
  init_cpu();
  init_audio();
  strncpy(s_romName, filename, sizeof(s_romName) - 1);
  s_loaded = true;
  s_skipAcc = 0;
  s_palAcc = 0;
  Serial.printf("ROM crc=%08lX size=%u bank=%d evbl=%d\n", (unsigned long)app_data.crc,
                (unsigned)n, app_data.bank, evblclk);
  return true;
}

void o2Unload() {
  if (s_loaded) {
    close_audio();
    close_display();
    audioCloseAndShrink();
  }
  s_loaded = false;
  s_romName[0] = 0;
  memset(joystick_data, 0, sizeof(joystick_data));
}

void o2Reset() {
  if (!s_loaded)
    return;
  init_cpu();
  init_system();
}

void o2ClearKeys() {
  memset(key, 0, sizeof(key));
  memset(joystick_data, 0, sizeof(joystick_data));
}

void o2SetKey(int retroKey, bool down) {
  auto setk = [down](int k) {
    if (k >= 0 && k < 512)
      key[k] = down ? 1 : 0;
  };
  setk(retroKey);
  /* PC keys that the 8244 matrix expects under a different RetroKey. */
  switch (retroKey) {
  case RETROK_BACKSPACE:
    setk(RETROK_DELETE); /* O2 CLEAR */
    break;
  case RETROK_KP_PLUS:
    setk(RETROK_PLUS);
    break;
  case RETROK_KP_MULTIPLY:
    setk(RETROK_ASTERISK);
    break;
  case RETROK_KP_MINUS:
    setk(RETROK_MINUS);
    break;
  case RETROK_KP_DIVIDE:
    setk(RETROK_SLASH);
    break;
  case RETROK_KP_ENTER:
    setk(RETROK_RETURN);
    break;
  case RETROK_KP0:
  case RETROK_KP1:
  case RETROK_KP2:
  case RETROK_KP3:
  case RETROK_KP4:
  case RETROK_KP5:
  case RETROK_KP6:
  case RETROK_KP7:
  case RETROK_KP8:
  case RETROK_KP9:
    setk(RETROK_0 + (retroKey - RETROK_KP0));
    break;
  default:
    break;
  }
}

void o2ApplyInput() {
  /* Joystick for selected player: QAOP+Space and arrows+Space.
   * Letters still feed the O2 matrix (unlike 2600). */
  int j = s_joy;
  memset(joystick_data, 0, sizeof(joystick_data));
  joystick_data[j][0] = key[RETROK_q] || key[RETROK_UP];
  joystick_data[j][1] = key[RETROK_a] || key[RETROK_DOWN];
  joystick_data[j][2] = key[RETROK_o] || key[RETROK_LEFT];
  joystick_data[j][3] = key[RETROK_p] || key[RETROK_RIGHT];
  joystick_data[j][4] = key[RETROK_SPACE];
}

static void downmixAndPush() {
  if (!audioEnabled())
    return;
  /* 1056 samples/frame @ ~60 → ~63 kHz. Bresenham to 22050. */
  int outN = (fps == FPS_PAL) ? 441 : 368;
  uint8_t tmp[512];
  if (outN > (int)sizeof(tmp))
    outN = sizeof(tmp);
  int acc = 0;
  int src = 0;
  for (int i = 0; i < outN; i++) {
    tmp[i] = soundBuffer[src];
    acc += SOUND_BUFFER_LEN;
    while (acc >= outN) {
      acc -= outN;
      src++;
      if (src >= SOUND_BUFFER_LEN)
        src = SOUND_BUFFER_LEN - 1;
    }
  }
  audioPush(tmp, outN);
}

void o2RunFrame() {
  if (!s_loaded)
    return;
  if (s_frameskip) {
    s_skipAcc++;
    if ((s_skipAcc % (s_frameskip + 1)) != 0) {
      vgaWaitVsync();
      return;
    }
  }
  if (s_euro) {
    /* Internal 50 Hz, VGA 60 Hz: extra vsync every 6th tick. */
    s_palAcc++;
    if ((s_palAcc % 6) == 0) {
      vgaWaitVsync();
      return;
    }
  }
  o2ApplyInput();
  RLOOP = 1;
  cpu_exec();
  downmixAndPush();
  vgaCountEmuFrame();
}

void o2Present() { vdc_present(); }
