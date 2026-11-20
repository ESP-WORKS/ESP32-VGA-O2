#pragma once
#include <stdint.h>
#include <stddef.h>

bool o2BiosPresent();
const char *o2BiosError();
bool o2LoadBios();
bool o2LoadRom(const char *filename, char *err, int errMax);
void o2Unload();
bool o2Loaded();
void o2Reset();
void o2RunFrame();
void o2Present();
void o2SetEuro(bool euro);
bool o2Euro();
void o2SetFrameskip(int n);
int o2Frameskip();
void o2SetJoyPlayer(int n); /* 0=J1, 1=J2 */
int o2JoyPlayer();
void o2ApplyInput();
void o2SetKey(int retroKey, bool down);
void o2ClearKeys();
const char *o2RomName();
