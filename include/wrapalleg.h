#ifndef WRAP_H
#define WRAP_H

#include <stdint.h>
#include "libretro.h"

#ifndef INLINE
#define INLINE inline
#endif

#define keypressed() 0
#define poll_keyboard()
#define yield_timeslice()

typedef struct {
  unsigned char *line;
  int w;
  int h;
  int pitch;
  int depth;
} ALLEGRO_BITMAP;

typedef struct {
  unsigned char r, g, b;
} APALETTE;

#ifdef __cplusplus
extern "C" {
#endif
extern unsigned char key[256 * 2];
extern int joystick_data[2][5];
void update_joy(void);
#ifdef __cplusplus
}
#endif

#define EMUWIDTH 160
#define EMUHEIGHT 256
#define TEX_WIDTH 160
#define TEX_HEIGHT 240

#endif
