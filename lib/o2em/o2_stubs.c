/*
 * Stubs: Videopac+ overlay, The Voice, high-score file I/O.
 * First slice is classic Odyssey 2 / G7000 only.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "vpp.h"
#include "voice.h"
#include "score.h"
#include "vdc.h"

uint8_t read_PB(uint8_t p) { (void)p; return 0xFF; }
void write_PB(uint8_t p, uint8_t val) { (void)p; (void)val; }
uint8_t vpp_read(uint16_t adr) { (void)adr; return 0; }
void vpp_write(uint8_t dat, uint16_t adr) { (void)dat; (void)adr; }
void vpp_finish_bmp(uint8_t *vmem, int offx, int offy, int w, int h, int totw, int toth) {
  (void)vmem; (void)offx; (void)offy; (void)w; (void)h; (void)totw; (void)toth;
}
int init_vpp(void) { return 1; }
void close_vpp(void) {}
void load_colplus(uint8_t *col) {
  if (col)
    memset(col, 0, (size_t)BMPW * (size_t)BMPH);
}
size_t vpp_state_size(void) { return 0; }
size_t vpp_state_save(uint8_t *data) { (void)data; return 0; }
size_t vpp_state_load(const uint8_t *data) { (void)data; return 0; }

void init_voice(const char *path) { (void)path; }
void update_voice(void) {}
void trigger_voice(int addr) { (void)addr; }
void reset_voice(void) {}
void set_voice_bank(int bank) { (void)bank; }
void close_voice(void) {}
void mute_voice(void) {}
int get_voice_status(void) { return 0; }

int get_score(int scoretype, int scoreaddress) {
  (void)scoretype; (void)scoreaddress; return 0;
}
void set_score(int scoretype, int scoreaddress, int highscore) {
  (void)scoretype; (void)scoreaddress; (void)highscore;
}
int power(int base, int higher) {
  int r = 1;
  while (higher-- > 0)
    r *= base;
  return r;
}
