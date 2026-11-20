#include "sd_host.h"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

SdState g_sd;
RomEntry g_roms[MAX_ROMS];
int g_kbdData = PIN_KBD_DATA_DEFAULT;
int g_kbdClk = PIN_KBD_CLK_DEFAULT;
int g_magicb = -1;

static SPIClass s_spi(HSPI);
static bool s_spiBegun = false;

static bool isIgnored(const char *name) {
  if (!name || !name[0])
    return true;
  if (name[0] == '.')
    return true;
  if (name[0] == '_' && name[1] == '.')
    return true;
  if (!strcasecmp(name, ".DS_Store"))
    return true;
  if (!strncmp(name, "._", 2))
    return true;
  if (!strcasecmp(name, "cache.txt"))
    return true;
  return false;
}

static bool isBiosName(const char *name) {
  return !strcasecmp(name, "o2rom.bin") || !strcasecmp(name, "o2bios.bin") ||
         !strcasecmp(name, "odyssey2.bin");
}

static const char *extOf(const char *name) {
  const char *d = strrchr(name, '.');
  return d ? d + 1 : "";
}

static bool isRomName(const char *name, size_t sz) {
  if (isIgnored(name) || isBiosName(name))
    return false;
  const char *e = extOf(name);
  if (e[0]) {
    return !strcasecmp(e, "bin") || !strcasecmp(e, "rom") || !strcasecmp(e, "o2");
  }
  /* No extension: classic O2 2/4/8/12/16 KB, multiple of 1024. */
  return sz >= 2048 && sz <= O2_ROM_MAX && (sz % 1024) == 0;
}

static int cmpRom(const void *a, const void *b) {
  const RomEntry *ra = (const RomEntry *)a;
  const RomEntry *rb = (const RomEntry *)b;
  return strcasecmp(ra->key, rb->key);
}

static void makeKey(char *key, const char *name) {
  memset(key, 0, 11);
  int n = 0;
  for (const char *p = name; *p && n < 10; p++) {
    char c = *p;
    if (c == '.')
      break;
    key[n++] = (char)toupper((unsigned char)c);
  }
}

static void sdRelease() {
  SD.end();
  if (s_spiBegun) {
    s_spi.end();
    s_spiBegun = false;
  }
  delay(5);
  /* Never reset GPIO 15 (VGA VSYNC, default HSPI CS). */
  gpio_reset_pin((gpio_num_t)SD_CS);
  gpio_reset_pin((gpio_num_t)SD_MOSI);
  gpio_reset_pin((gpio_num_t)SD_SCK);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
}

static void sdPrepMiso(int miso) {
  gpio_reset_pin((gpio_num_t)miso);
  /* GPIO 34–39 are input-only and have no internal pull-up. */
  if (miso <= 33) {
    pinMode(miso, INPUT_PULLUP);
    gpio_pullup_en((gpio_num_t)miso);
  } else {
    pinMode(miso, INPUT);
  }
}

/* CMD0 only: 74+ clocks, then GO_IDLE. Bounded by a few SPI bytes — no 500 ms
 * sdWait. SD.begin is NOT called unless the card answers 0x01 (idle). */
static uint8_t sdCmd0Once() {
  digitalWrite(SD_CS, HIGH);
  for (int i = 0; i < 16; i++)
    s_spi.transfer(0xFF);
  digitalWrite(SD_CS, LOW);
  s_spi.transfer(0x40);
  s_spi.transfer(0x00);
  s_spi.transfer(0x00);
  s_spi.transfer(0x00);
  s_spi.transfer(0x00);
  s_spi.transfer(0x95);
  uint8_t r = 0xFF;
  for (int i = 0; i < 10; i++) {
    r = s_spi.transfer(0xFF);
    if (!(r & 0x80))
      break;
  }
  digitalWrite(SD_CS, HIGH);
  s_spi.transfer(0xFF);
  return r;
}

static bool sdProbe(int miso) {
  sdRelease();
  sdPrepMiso(miso);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  delay(10);
  s_spi.begin(SD_SCK, miso, SD_MOSI, SD_CS);
  s_spiBegun = true;
  s_spi.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
  uint8_t r = 0xFF;
  for (int n = 0; n < 8; n++) {
    r = sdCmd0Once();
    if (r == 0x01)
      break;
    delay(8);
  }
  s_spi.endTransaction();
  Serial.printf("SD probe MISO=%d  CMD0=0x%02x\n", miso, r);
  Serial.flush();
  return r == 0x01;
}

static bool tryBegin(int miso, uint32_t hz) {
  Serial.printf("SD try MISO=%d  %uHz\n", miso, (unsigned)hz);
  Serial.flush();
  /* maxFiles=2: menu/cache don't need many handles. */
  if (SD.begin(SD_CS, s_spi, hz, "/sd", 2)) {
    g_sd.miso = miso;
    g_sd.mounted = true;
    uint64_t mb = SD.cardSize() / (1024ull * 1024ull);
    Serial.printf("SD OK  MISO=%d  %uHz  type=%d  %lluMB\n", miso, (unsigned)hz,
                  (int)SD.cardType(), (unsigned long long)mb);
    Serial.flush();
    return true;
  }
  Serial.println("SD fail (mount)");
  Serial.flush();
  SD.end();
  return false;
}

bool sdMount() {
  memset(&g_sd, 0, sizeof(g_sd));
  g_kbdData = PIN_KBD_DATA_DEFAULT;
  g_kbdClk = PIN_KBD_CLK_DEFAULT;
  g_magicb = -1;
  strncpy(g_sd.lastError, "SD not found", sizeof(g_sd.lastError) - 1);

  /* Spec: MISO=2 then 35. Never GPIO 25 (DAC). Probe CMD0 first so a missing
   * card cannot sit inside SD.begin / sdWait(500) until the task WDT fires.
   * After a live idle token, mount at 2 MHz (VGA I2S + 4 MHz is flaky on some
   * v1.4 boards), then 400 kHz, then 4 MHz. */
  const int misos[] = {SD_MISO_PRIMARY, SD_MISO_FALLBACK};
  const uint32_t hz[] = {2000000u, 400000u, 4000000u};
  for (int m = 0; m < 2; m++) {
    vTaskDelay(pdMS_TO_TICKS(5));
    if (!sdProbe(misos[m])) {
      Serial.printf("SD fail (no card on MISO=%d)\n", misos[m]);
      Serial.flush();
      sdRelease();
      continue;
    }
    for (int h = 0; h < 3; h++) {
      vTaskDelay(pdMS_TO_TICKS(5));
      if (tryBegin(misos[m], hz[h])) {
        g_sd.lastError[0] = 0;
        sdParseBootRc();
        g_sd.folderOk = SD.exists(ROM_DIR);
        if (!g_sd.folderOk)
          strncpy(g_sd.lastError, "/ODYSSEY2 folder not found", sizeof(g_sd.lastError) - 1);
        char bp[48];
        g_sd.biosOk = sdBiosPath(bp, sizeof(bp));
        return true;
      }
    }
    sdRelease();
  }
  g_sd.mounted = false;
  Serial.println("SD not found");
  Serial.flush();
  return false;
}

static void parseRcFile(const char *path) {
  File f = SD.open(path, FILE_READ);
  if (!f)
    return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0 || line[0] == '#')
      continue;
    int eq = line.indexOf('=');
    if (eq <= 0)
      continue;
    String k = line.substring(0, eq);
    String v = line.substring(eq + 1);
    k.trim();
    v.trim();
    k.toLowerCase();
    int gi = v.toInt();
    if (k == "kbddat")
      g_kbdData = gi;
    else if (k == "kbdclk")
      g_kbdClk = gi;
    else if (k == "magicb")
      g_magicb = gi;
  }
  f.close();
}

void sdParseBootRc() {
  if (SD.exists(BOOTRC_NAME))
    parseRcFile(BOOTRC_NAME);
  else if (SD.exists(BOOTRC_NAME_UC))
    parseRcFile(BOOTRC_NAME_UC);
}

bool sdHasFolder() { return g_sd.folderOk; }

int sdMiso() { return g_sd.miso; }

bool sdBiosPath(char *out, int outMax) {
  const char *cands[] = {ROM_DIR "/o2rom.bin", ROM_DIR "/o2bios.bin", ROM_DIR "/odyssey2.bin"};
  for (auto p : cands) {
    if (SD.exists(p)) {
      strncpy(out, p, outMax - 1);
      out[outMax - 1] = 0;
      return true;
    }
  }
  return false;
}

static bool writeCacheFromScan() {
  File dir = SD.open(ROM_DIR);
  if (!dir)
    return false;
  File out = SD.open(CACHE_NAME, FILE_WRITE);
  if (!out) {
    dir.close();
    return false;
  }
  File e = dir.openNextFile();
  while (e) {
    const char *nm = e.name();
    const char *base = strrchr(nm, '/');
    base = base ? base + 1 : nm;
    size_t sz = e.size();
    e.close();
    if (isRomName(base, sz)) {
      out.println(base);
    }
    e = dir.openNextFile();
  }
  out.close();
  dir.close();
  return true;
}

static bool loadIndexFromCache() {
  File f = SD.open(CACHE_NAME, FILE_READ);
  if (!f)
    return false;
  g_sd.romCount = 0;
  uint32_t off = 0;
  while (f.available() && g_sd.romCount < MAX_ROMS) {
    uint32_t lineOff = off;
    String line = f.readStringUntil('\n');
    off = f.position();
    line.trim();
    if (line.length() == 0)
      continue;
    if (line.endsWith("\r"))
      line.remove(line.length() - 1);
    const char *nm = line.c_str();
    if (isIgnored(nm) || isBiosName(nm))
      continue;
    RomEntry &r = g_roms[g_sd.romCount];
    r.cacheOff = lineOff;
    makeKey(r.key, nm);
    r.letter = r.key[0];
    if (r.letter < 'A' || r.letter > 'Z') {
      if (r.letter >= '0' && r.letter <= '9')
        ;
      else
        r.letter = '#';
    }
    g_sd.romCount++;
  }
  f.close();
  qsort(g_roms, g_sd.romCount, sizeof(RomEntry), cmpRom);
  /* Recompute letters after sort (key already set). */
  for (int i = 0; i < g_sd.romCount; i++) {
    char L = g_roms[i].key[0];
    g_roms[i].letter = (L >= 'A' && L <= 'Z') ? L : ((L >= '0' && L <= '9') ? L : '#');
  }
  return true;
}

bool sdLoadCache(bool forceRebuild) {
  g_sd.romCount = 0;
  if (!g_sd.folderOk)
    return false;
  bool have = SD.exists(CACHE_NAME);
  if (!have || forceRebuild) {
    if (!writeCacheFromScan()) {
      strncpy(g_sd.lastError, "cache write failed", sizeof(g_sd.lastError) - 1);
      return false;
    }
  }
  if (!loadIndexFromCache()) {
    /* cache existed but unreadable — rebuild once */
    if (!forceRebuild) {
      writeCacheFromScan();
      if (loadIndexFromCache())
        return true;
    }
    strncpy(g_sd.lastError, "cache read failed", sizeof(g_sd.lastError) - 1);
    return false;
  }
  return true;
}

void sdRebuildCache() { sdLoadCache(true); }

void sdReadName(int index, char *out, int outMax) {
  out[0] = 0;
  if (index < 0 || index >= g_sd.romCount)
    return;
  File f = SD.open(CACHE_NAME, FILE_READ);
  if (!f)
    return;
  if (!f.seek(g_roms[index].cacheOff)) {
    f.close();
    return;
  }
  String line = f.readStringUntil('\n');
  f.close();
  line.trim();
  if (line.endsWith("\r"))
    line.remove(line.length() - 1);
  strncpy(out, line.c_str(), outMax - 1);
  out[outMax - 1] = 0;
}

int sdFindLetter(char letter) {
  letter = (char)toupper((unsigned char)letter);
  for (int i = 0; i < g_sd.romCount; i++) {
    if (g_roms[i].letter == letter)
      return i;
  }
  return -1;
}

bool sdReadFile(const char *path, uint8_t *dst, size_t maxLen, size_t *outLen) {
  if (outLen)
    *outLen = 0;
  File f = SD.open(path, FILE_READ);
  if (!f)
    return false;
  size_t n = f.size();
  if (n > maxLen)
    n = maxLen;
  size_t got = f.read(dst, n);
  f.close();
  if (outLen)
    *outLen = got;
  return got > 0;
}

bool sdCacheExists() { return g_sd.mounted && SD.exists(CACHE_NAME); }

void sdFormatStatus(char *out, int n) {
  if (!g_sd.mounted) {
    snprintf(out, n, "SD not found");
    return;
  }
  if (!g_sd.folderOk) {
    snprintf(out, n, "/ODYSSEY2 folder not found");
    return;
  }
  snprintf(out, n, "SD MISO=%d  %d ROMs in /ODYSSEY2", g_sd.miso, g_sd.romCount);
}
