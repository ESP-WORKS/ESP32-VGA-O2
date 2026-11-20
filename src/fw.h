#pragma once

/* VIDEO REFERENCE — confirmed Come Come (K.C. Munchkin): full score zeros,
 * full-screen picture. Do not change without a new FW banner.
 *
 *   VGA:   QVGA_320x240_60Hz DoubleScan
 *   srcY:  scanLine + GAME_YOFF, GAME_YOFF = 0  (source lines 0..239)
 *   blit:  blitO2Line4  — 2× horizontal, 160 → 320
 *   chars: 2× vertical in lib/o2em/vdc.c
 *   never: 1:1 160-wide, YOFF crop, drop DoubleScan, black bottom band
 *
 * FW 16: finish_display must not rewind to line 160 (wipes maze).
 * FW 17: Didi na Mina Encantada = Pick Axe Pete (evblclk 12000 + dump 4K).
 */
#define FW_BANNER "FW 17  o2-boot"

#define VGA_W 320
#define VGA_H 240
#define MENU_LIST_Y 24
#define MENU_ROWS 24
#define MENU_COLS 39
#define MAX_ROMS 1024

#define SD_CS 13
#define SD_MOSI 12
#define SD_SCK 14
#define SD_MISO_PRIMARY 2
#define SD_MISO_FALLBACK 35
/* GPIO 25 is the DAC. Never use it as SD MISO. */

#define PIN_VGA_R0 21
#define PIN_VGA_R1 22
#define PIN_VGA_G0 18
#define PIN_VGA_G1 19
#define PIN_VGA_B0 4
#define PIN_VGA_B1 5
#define PIN_VGA_HSYNC 23
#define PIN_VGA_VSYNC 15

#define PIN_KBD_DATA_DEFAULT 32
#define PIN_KBD_CLK_DEFAULT 33
#define PIN_AUDIO_DAC 25

#define AUDIO_RATE 22050
#define AUDIO_RING 2048
#define AUDIO_QUEUE_CAP 1024

#define ROM_DIR "/ODYSSEY2"
#define CACHE_NAME "/ODYSSEY2/cache.txt"
#define BOOTRC_NAME "/bootl.rc"
#define BOOTRC_NAME_UC "/BOOTL.RC"

#define O2_ROM_MAX 16384
#define O2_BIOS_SIZE 1024
