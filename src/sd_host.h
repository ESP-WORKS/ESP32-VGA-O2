#pragma once
#include <stdint.h>
#include <stddef.h>
#include "fw.h"

struct RomEntry {
  uint32_t cacheOff;
  char letter;
  char key[11];
};

struct SdState {
  bool mounted;
  bool folderOk;
  bool biosOk;
  int miso;
  int romCount;
  char lastError[48];
};

extern SdState g_sd;
extern RomEntry g_roms[MAX_ROMS];
extern int g_kbdData;
extern int g_kbdClk;
extern int g_magicb;

bool sdMount();
void sdParseBootRc();
bool sdHasFolder();
bool sdLoadCache(bool forceRebuild);
void sdRebuildCache();
void sdReadName(int index, char *out, int outMax);
int sdFindLetter(char letter);
bool sdReadFile(const char *path, uint8_t *dst, size_t maxLen, size_t *outLen);
bool sdBiosPath(char *out, int outMax);
int sdMiso();
void sdFormatStatus(char *out, int n);
bool sdCacheExists();
