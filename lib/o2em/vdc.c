/*
 *   O2EM Free Odyssey2 / Videopac+ Emulator
 *
 *   Created by Daniel Boris <dboris@comcast.net>  (c) 1997,1998
 *
 *   Developed by Andre de la Rocha   <adlroc@users.sourceforge.net>
 *             Arlindo M. de Oliveira <dgtec@users.sourceforge.net>
 *
 *   http://o2em.sourceforge.net
 *
 *   O2 Video Display Controller emulation
 *   ESP32 port: native 160-wide indexed buffer (VGA ISR does 2×).
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "vmachine.h"
#include "config.h"
#include "keyboard.h"
#include "cset.h"
#include "cpu.h"
#include "vpp.h"
#include "vdc.h"
#include "audio.h"
#include "voice.h"

#ifndef INLINE
#define INLINE inline
#endif

#define COL_SP0   0x01
#define COL_SP1   0x02
#define COL_SP2   0x04
#define COL_SP3   0x08
#define COL_VGRID 0x10
#define COL_HGRID 0x20
#define COL_VPP   0x40
#define COL_CHAR  0x80

#define XOFF 10

const uint32_t o2_colortable[2][16] = {
    /* O2 palette (hand-tuned later to RGB222 so adjacent greys stay distinct) */
    {0x000000, 0x0e3dd4, 0x00981b, 0x00bbd9, 0xc70008, 0xcc16b3, 0x9d8710, 0xe1dee1,
     0x5f6e6b, 0x6aa1ff, 0x3df07a, 0x31ffff, 0xff4255, 0xff98ff, 0xd9ad5d, 0xffffff},
    /* VP+ G7400 palette */
    {0x000000, 0x0000b6, 0x00b600, 0x00b6b6, 0xb60000, 0xb600b6, 0xb6b600, 0xb6b6b6,
     0x494949, 0x4949ff, 0x49ff49, 0x49ffff, 0xff4949, 0xff49ff, 0xffff49, 0xffffff}
};

static uint8_t *col = NULL;
uint8_t *volatile o2_vscreen = NULL;
volatile int o2_vscreen_packed = 1;
static uint8_t *vscreen = NULL; /* emu packed 4-bit back buffer */
static uint8_t *vscreen_front = NULL; /* ISR packed 4-bit front */
static uint8_t *s_slab = NULL;

uint8_t coltab[256];
long clip_low;
long clip_high;
int wsize;

static INLINE void putpix(unsigned int ad, uint8_t d, uint8_t c)
{
    unsigned int pi = ad >> 1;
    d &= 15;
    if (ad & 1)
        vscreen[pi] = (uint8_t)((vscreen[pi] & 0x0F) | (d << 4));
    else
        vscreen[pi] = (uint8_t)((vscreen[pi] & 0xF0) | d);
    col[ad] |= c;
    coltab[c] |= col[ad];
}

static INLINE void mputvid(unsigned int ad, unsigned int len, uint8_t d, uint8_t c)
{
    if ((ad >= (unsigned long)clip_low) && (ad < (unsigned long)clip_high)) {
        unsigned int i;
        if (ad + len > (unsigned int)(BMPW * BMPH))
            len = (unsigned int)(BMPW * BMPH) - ad;
        for (i = 0; i < len; i++)
            putpix(ad + i, d, c);
    }
}

static void draw_char(uint8_t ypos, uint8_t xpos, uint8_t chr, uint8_t colr)
{
    int j, b, n;
    uint8_t cl, d1;
    int y = (ypos & 0xFE);
    unsigned int pnt = y * BMPW + (xpos - 8) + XOFF;

    ypos = ypos >> 1;
    n = 7 - ((ypos + chr) & 7);
    if (n == 0)
        n = 8;
    /* Bottom band: 8244 n can be 4, which is only the top half of a 16px
     * doubled zero. Force a full 8 charset rows for the score line. */
    if (y >= 160 && n < 8)
        n = 8;

    if ((pnt + BMPW * 2 * n >= (unsigned long)clip_low) && (pnt <= (unsigned long)clip_high)) {
        int c = (int)chr + ypos;
        if (colr & 0x01)
            c += 256;
        if (c > 511)
            c = c - 512;

        cl = ((colr & 0x0E) >> 1);
        cl = ((cl & 2) | ((cl & 1) << 2) | ((cl & 4) >> 2)) + 8;

        /* O2EM 340-wide draws each char row as 2 scanlines. ISR already
         * does 2× horizontal from 160; vertical 2× belongs here. FW7
         * skipped it — 8px glyphs, Come Come score looked cut in half. */
        if ((y > 0) && (y < BMPH - 1) && (xpos < 157)) {
            for (j = 0; j < n; j++) {
                d1 = cset[c + j];
                for (b = 0; b < 8; b++) {
                    if (d1 & 0x80) {
                        if ((xpos - 8 + b < 160) && (y + 2 * j + 1 < BMPH)) {
                            mputvid(pnt, 1, cl, COL_CHAR);
                            mputvid(pnt + BMPW, 1, cl, COL_CHAR);
                        }
                    }
                    pnt += 1;
                    d1 = d1 << 1;
                }
                pnt += BMPW * 2 - 8;
            }
        }
    }
}

static void draw_quad(uint8_t ypos, uint8_t xpos, uint8_t cp0l, uint8_t cp0h,
                      uint8_t cp1l, uint8_t cp1h, uint8_t cp2l, uint8_t cp2h,
                      uint8_t cp3l, uint8_t cp3h)
{
    int chp[4];
    uint8_t colr[4];
    unsigned int pnt, off;
    int i, j, lines;

    pnt = (ypos & 0xfe) * BMPW + (xpos - 8) + XOFF;
    if (pnt > (unsigned long)clip_high)
        return;
    chp[0] = cp0l | ((cp0h & 1) << 8);
    chp[1] = cp1l | ((cp1h & 1) << 8);
    chp[2] = cp2l | ((cp2h & 1) << 8);
    chp[3] = cp3l | ((cp3h & 1) << 8);
    for (i = 0; i < 4; i++)
        chp[i] = (chp[i] + (ypos >> 1)) & 0x1ff;
    lines = 7 - (((ypos >> 1) + chp[3]) & 7);
    if (lines == 0)
        lines = 8;
    if ((int)(ypos & 0xfe) >= 160 && lines < 8)
        lines = 8;
    if (pnt + BMPW * 2 * lines < (unsigned long)clip_low)
        return;
    colr[0] = (cp0h & 0xe) >> 1;
    colr[1] = (cp1h & 0xe) >> 1;
    colr[2] = (cp2h & 0xe) >> 1;
    colr[3] = (cp3h & 0xe) >> 1;
    for (i = 0; i < 4; i++)
        colr[i] = ((colr[i] & 2) | ((colr[i] & 1) << 2) | ((colr[i] & 4) >> 2)) + 8;
    while (lines-- > 0) {
        off = 0;
        for (i = 0; i < 4; i++) {
            for (j = 0; j < 8; j++) {
                if ((cset[chp[i]] & (1 << (7 - j))) && (off < (unsigned)BMPW)) {
                    mputvid(pnt + off, 1, colr[i], COL_CHAR);
                    mputvid(pnt + off + BMPW, 1, colr[i], COL_CHAR);
                }
                off += 1;
            }
            off += 8;
        }
        for (i = 0; i < 4; i++)
            chp[i] = (chp[i] + 1) & 0x1ff;
        pnt += BMPW * 2;
    }
}

static void draw_grid(void)
{
    unsigned int pnt, pn1;
    uint8_t mask, d;
    int j, i, x, w;
    uint8_t color;

    if (VDCwrite[0xA0] & 0x40) {
        for (j = 0; j < 9; j++) {
            pnt = (((j * 24) + 24) * BMPW);
            for (i = 0; i < 9; i++) {
                pn1 = pnt + (i * 16) + XOFF;
                color = ColorVector[j * 24 + 24];
                mputvid(pn1, 2, (color & 0x07) | ((color & 0x40) >> 3) | ((color & 0x80) ? 0 : 8), COL_HGRID);
                color = ColorVector[j * 24 + 25];
                mputvid(pn1 + BMPW, 2, (color & 0x07) | ((color & 0x40) >> 3) | ((color & 0x80) ? 0 : 8), COL_HGRID);
                color = ColorVector[j * 24 + 26];
                mputvid(pn1 + BMPW * 2, 2, (color & 0x07) | ((color & 0x40) >> 3) | ((color & 0x80) ? 0 : 8), COL_HGRID);
            }
        }
    }

    mask = 0x01;
    for (j = 0; j < 9; j++) {
        pnt = (((j * 24) + 24) * BMPW);
        for (i = 0; i < 9; i++) {
            pn1 = pnt + (i * 16) + XOFF;
            if ((pn1 + BMPW * 3 >= (unsigned long)clip_low) && (pn1 <= (unsigned long)clip_high)) {
                d = VDCwrite[0xC0 + i];
                if (j == 8) {
                    d = VDCwrite[0xD0 + i];
                    mask = 1;
                }
                if (d & mask) {
                    color = ColorVector[j * 24 + 24];
                    mputvid(pn1, 18, (color & 0x07) | ((color & 0x40) >> 3) | ((color & 0x80) ? 0 : 8), COL_HGRID);
                    color = ColorVector[j * 24 + 25];
                    mputvid(pn1 + BMPW, 18, (color & 0x07) | ((color & 0x40) >> 3) | ((color & 0x80) ? 0 : 8), COL_HGRID);
                    color = ColorVector[j * 24 + 26];
                    mputvid(pn1 + BMPW * 2, 18, (color & 0x07) | ((color & 0x40) >> 3) | ((color & 0x80) ? 0 : 8), COL_HGRID);
                }
            }
        }
        mask = mask << 1;
    }

    w = 2;
    if (VDCwrite[0xA0] & 0x80)
        w = 16;
    for (j = 0; j < 10; j++) {
        pnt = (j * 16);
        mask = 0x01;
        d = VDCwrite[0xE0 + j];
        for (x = 0; x < 8; x++) {
            pn1 = pnt + (((x * 24) + 24) * BMPW) + XOFF;
            if (d & mask) {
                for (i = 0; i < 24; i++) {
                    if ((pn1 >= (unsigned long)clip_low) && (pn1 <= (unsigned long)clip_high)) {
                        color = ColorVector[x * 24 + 24 + i];
                        mputvid(pn1, w, (color & 0x07) | ((color & 0x40) >> 3) | (color & 0x80 ? 0 : 8), COL_VGRID);
                    }
                    pn1 += BMPW;
                }
            }
            mask = mask << 1;
        }
    }
}

void draw_display(void)
{
    int i, j, x, sm, t;
    uint8_t y, b, d1, cl, c;
    unsigned int pnt, pnt2;

    if (!vscreen)
        return;

    for (i = clip_low / BMPW; i < clip_high / BMPW; i++) {
        uint8_t bg = (uint8_t)(((ColorVector[i] & 0x38) >> 3) | (ColorVector[i] & 0x80 ? 0 : 8));
        uint8_t pair = (uint8_t)((bg & 15) | ((bg & 15) << 4));
        memset(vscreen + i * BMPPACKW, pair, BMPPACKW);
    }

    if (VDCwrite[0xA0] & 0x08)
        draw_grid();

    if (useforen && (!(VDCwrite[0xA0] & 0x20)))
        return;

    for (i = 0x10; i < 0x40; i += 4)
        draw_char(VDCwrite[i], VDCwrite[i + 1], VDCwrite[i + 2], VDCwrite[i + 3]);

    for (i = 0x40; i < 0x80; i += 0x10)
        draw_quad(VDCwrite[i], VDCwrite[i + 1], VDCwrite[i + 2], VDCwrite[i + 3],
                  VDCwrite[i + 6], VDCwrite[i + 7],
                  VDCwrite[i + 10], VDCwrite[i + 11],
                  VDCwrite[i + 14], VDCwrite[i + 15]);

    c = 8;
    for (i = 12; i >= 0; i -= 4) {
        pnt2 = 0x80 + (i * 2);
        y = VDCwrite[i];
        x = VDCwrite[i + 1] - 8;
        t = VDCwrite[i + 2];
        cl = ((t & 0x38) >> 3);
        cl = ((cl & 2) | ((cl & 1) << 2) | ((cl & 4) >> 2)) + 8;
        if ((x < 164) && (y > 0) && (y < BMPH - 1)) {
            pnt = y * BMPW + x + XOFF + sproff;
            if (t & 4) {
                if ((pnt + BMPW * 32 >= (unsigned long)clip_low) && (pnt <= (unsigned long)clip_high)) {
                    for (j = 0; j < 8; j++) {
                        sm = (((j % 2 == 0) && (((t >> 1) & 1) != (t & 1))) || ((j % 2 == 1) && (t & 1))) ? 1 : 0;
                        d1 = VDCwrite[pnt2++];
                        for (b = 0; b < 8; b++) {
                            if (d1 & 0x01) {
                                if ((x + b + sm < 159) && (y + 4 * j + 3 < BMPH)) {
                                    mputvid(sm + pnt, 2, cl, c);
                                    mputvid(sm + pnt + BMPW, 2, cl, c);
                                    mputvid(sm + pnt + BMPW * 2, 2, cl, c);
                                    mputvid(sm + pnt + BMPW * 3, 2, cl, c);
                                }
                            }
                            pnt += 2;
                            d1 = d1 >> 1;
                        }
                        pnt += BMPW * 4 - 16;
                    }
                }
            } else {
                if ((pnt + BMPW * 16 >= (unsigned long)clip_low) && (pnt <= (unsigned long)clip_high)) {
                    for (j = 0; j < 8; j++) {
                        sm = (((j % 2 == 0) && (((t >> 1) & 1) != (t & 1))) || ((j % 2 == 1) && (t & 1))) ? 1 : 0;
                        d1 = VDCwrite[pnt2++];
                        for (b = 0; b < 8; b++) {
                            if (d1 & 0x01) {
                                if ((x + b + sm < 160) && (y + 2 * j + 1 < BMPH)) {
                                    mputvid(sm + pnt, 1, cl, c);
                                    mputvid(sm + pnt + BMPW, 1, cl, c);
                                }
                            }
                            pnt += 1;
                            d1 = d1 >> 1;
                        }
                        pnt += BMPW * 2 - 8;
                    }
                }
            }
        }
        c = c >> 1;
    }
}

void draw_region(void)
{
    int i;

    if (regionoff == 0xffff)
        i = (master_clk / (LINECNT - 1) - 5);
    else
        i = (master_clk / 22 + regionoff);
    i = (snapline(i, VDCwrite[0xA0], 0));

    if (app_data.crc == 0xA7344D1F) {
        i = (master_clk / 22 + regionoff) + 6;
        i = (snapline(i, VDCwrite[0xA0], 0) + 6);
    }
    if (app_data.crc == 0xD0BC4EE6) {
        i = (master_clk / 24 + regionoff) - 6;
        i = (snapline(i, VDCwrite[0xA0], 0) + 7);
    }
    if (app_data.crc == 0x26517E77) {
        i = (master_clk / 22 + regionoff);
        i = (snapline(i, VDCwrite[0xA0], 0) - 5);
    }
    if (app_data.crc == 0xA57E1724) {
        i = (master_clk / (LINECNT - 1) - 5);
        i = (snapline(i, VDCwrite[0xA0], 0) - 3);
    }

    if (i < 0)
        i = 0;
    clip_low = last_line * (long)BMPW;
    clip_high = i * (long)BMPW;
    if (clip_high > BMPW * BMPH)
        clip_high = BMPW * BMPH;
    if (clip_low < 0)
        clip_low = 0;
    if (clip_low < clip_high)
        draw_display();
    last_line = i;
    if (last_line > BMPH)
        last_line = BMPH;
}

void grmode(void)
{
    wsize = 1;
    clearscr();
}

void set_textmode(void)
{
    if (new_int)
        Set_Old_Int9();
}

void clearscr(void)
{
    if (vscreen)
        memset(vscreen, 0, (size_t)BMPPACKW * (size_t)BMPH);
}

void finish_display(void)
{
    /* Flush only lines the beam never reached. Never rewind to 160:
     * Come Come already painted the maze incrementally; vblank VDC
     * state has the grid off and a leftover sprite, which is why the
     * lower walls vanished while 0000 and a lone tree stayed. */
    if (!vscreen)
        return;
    if (last_line < BMPH) {
        clip_low = last_line * (long)BMPW;
        if (clip_low < 0)
            clip_low = 0;
        clip_high = (long)BMPW * (long)BMPH;
        if (clip_low < clip_high)
            draw_display();
    }
    last_line = BMPH;
}

void vdc_present(void)
{
    /* Swap packed buffers at vblank. ISR never reads the buffer emu is
     * drawing — packing in place was the FW 6 flicker. */
    if (!vscreen || !vscreen_front || vscreen == vscreen_front)
        return;
    uint8_t *t = vscreen;
    vscreen = vscreen_front;
    vscreen_front = t;
    o2_vscreen = vscreen_front;
}

void clear_collision(void)
{
    load_colplus(col);
    coltab[0x01] = coltab[0x02] = 0;
    coltab[0x04] = coltab[0x08] = 0;
    coltab[0x10] = coltab[0x20] = 0;
    coltab[0x40] = coltab[0x80] = 0;
}

void close_display(void)
{
    if (s_slab) {
        free(s_slab);
        s_slab = NULL;
    }
    vscreen = NULL;
    vscreen_front = NULL;
    col = NULL;
    o2_vscreen = NULL;
    o2_vscreen_packed = 0;
}

void display_bg(void) {}

int init_display(void)
{
    const size_t pix = (size_t)BMPW * (size_t)BMPH;
    const size_t pack = (size_t)BMPPACKW * (size_t)BMPH;
    close_display();
    /* Two packed frames (ISR + emu) + 8-bit collision. ~82 KB. */
    s_slab = (uint8_t *)malloc(pack * 2 + pix);
    if (s_slab) {
        vscreen = s_slab;
        vscreen_front = s_slab + pack;
        col = s_slab + pack * 2;
        memset(s_slab, 0, pack * 2 + pix);
        o2_vscreen_packed = 1;
        o2_vscreen = vscreen_front;
    } else {
        s_slab = (uint8_t *)malloc(pack + pix);
        if (!s_slab)
            return 0;
        vscreen = s_slab;
        vscreen_front = vscreen;
        col = s_slab + pack;
        memset(s_slab, 0, pack + pix);
        o2_vscreen_packed = 1;
        o2_vscreen = vscreen;
    }
    grmode();
    init_keyboard();
    return 1;
}
