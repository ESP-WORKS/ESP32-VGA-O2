#ifndef __VDC_H
#define __VDC_H

#include <stdint.h>

/* VIDEO REF FW 15. Packed 4-bit. Chars/sprites 2× vertical.
 * VGA 320×240 DoubleScan. Source 0..239; ISR 2× horizontal (160→320). */
#define BMPW 160
#define BMPH 264
#define BMPPACKW (BMPW / 2)
#define GAME_YOFF 0
#define WNDW 320
#define WNDH 240

extern uint8_t coltab[];
extern long clip_low;
extern long clip_high;
extern uint8_t *volatile o2_vscreen;
extern volatile int o2_vscreen_packed;
extern const uint32_t o2_colortable[2][16];

int init_display(void);
void draw_display(void);
void set_textmode(void);
void draw_region(void);
void finish_display(void);
void vdc_present(void);
void close_display(void);
void grmode(void);
void display_bg(void);
void clear_collision(void);
void clearscr(void);

#endif
