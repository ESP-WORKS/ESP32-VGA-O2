#pragma once
#include <stdint.h>
#include <stddef.h>

static inline uint32_t crc32_buf(const uint8_t *data, size_t len) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    c ^= data[i];
    for (int b = 0; b < 8; b++)
      c = (c >> 1) ^ (0xEDB88320u & (-(int)(c & 1)));
  }
  return c ^ 0xFFFFFFFFu;
}
