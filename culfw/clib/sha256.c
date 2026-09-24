/* SHA-256 (FIPS 180-4), small rather than fast. Messages up to 512 MiB. */

#include <string.h>

#include "sha256.h"

static const uint32_t k[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
  0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
  0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
  0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
  0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
  0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
  0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
  0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
  0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void
transform(sha256_ctx *c)
{
  uint32_t w[64], v[8], t1, t2;
  uint8_t i;

  for(i = 0; i < 16; i++)
    w[i] = (uint32_t)c->buf[4*i] << 24 | (uint32_t)c->buf[4*i+1] << 16 |
           (uint32_t)c->buf[4*i+2] << 8 | c->buf[4*i+3];
  for(i = 16; i < 64; i++)
    w[i] = (ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10)) + w[i-7] +
           (ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3)) + w[i-16];

  memcpy(v, c->h, sizeof(v));
  for(i = 0; i < 64; i++) {
    t1 = v[7] + (ROR(v[4], 6) ^ ROR(v[4], 11) ^ ROR(v[4], 25)) +
         ((v[4] & v[5]) ^ (~v[4] & v[6])) + k[i] + w[i];
    t2 = (ROR(v[0], 2) ^ ROR(v[0], 13) ^ ROR(v[0], 22)) +
         ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
    memmove(v + 1, v, 7 * sizeof(uint32_t));
    v[4] += t1;
    v[0] = t1 + t2;
  }
  for(i = 0; i < 8; i++)
    c->h[i] += v[i];
}

void
sha256_init(sha256_ctx *c)
{
  static const uint32_t h0[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
  };
  memcpy(c->h, h0, sizeof(h0));
  c->len = 0;
  c->n = 0;
}

void
sha256_update(sha256_ctx *c, const void *data, uint16_t len)
{
  const uint8_t *p = data;
  c->len += len;
  while(len--) {
    c->buf[c->n++] = *p++;
    if(c->n == 64) {
      transform(c);
      c->n = 0;
    }
  }
}

void
sha256_final(sha256_ctx *c, uint8_t out[32])
{
  uint32_t bits = c->len << 3;
  uint8_t i;

  c->buf[c->n++] = 0x80;
  if(c->n > 56) {
    memset(c->buf + c->n, 0, 64 - c->n);
    transform(c);
    c->n = 0;
  }
  memset(c->buf + c->n, 0, 60 - c->n);
  c->buf[59] = c->len >> 29;
  c->buf[60] = bits >> 24;
  c->buf[61] = bits >> 16;
  c->buf[62] = bits >> 8;
  c->buf[63] = bits;
  transform(c);

  for(i = 0; i < 32; i++)
    out[i] = c->h[i / 4] >> (24 - 8 * (i % 4));
}
