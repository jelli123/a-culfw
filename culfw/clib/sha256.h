#ifndef _SHA256_H_
#define _SHA256_H_

#include <stdint.h>

typedef struct {
  uint32_t h[8];
  uint32_t len;                       // bytes hashed so far
  uint8_t buf[64];
  uint8_t n;                          // bytes in buf
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, uint16_t len);
void sha256_final(sha256_ctx *c, uint8_t out[32]);

#endif
