#include "sha256.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/*
 * Minimal SHA256 implementation (public domain style) adapted for file hashing.
 * Source simplified for brevity. Not optimized.
 */

typedef struct {
    uint32_t state[8];
    uint64_t bitcount;
    unsigned char buffer[64];
} SHA256_CTX_SIMPLE;

static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
static uint32_t Ch(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
static uint32_t Maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
static uint32_t Sigma0(uint32_t x) { return rotr(x,2) ^ rotr(x,13) ^ rotr(x,22); }
static uint32_t Sigma1(uint32_t x) { return rotr(x,6) ^ rotr(x,11) ^ rotr(x,25); }
static uint32_t sigma0(uint32_t x) { return rotr(x,7) ^ rotr(x,18) ^ (x>>3); }
static uint32_t sigma1(uint32_t x) { return rotr(x,17) ^ rotr(x,19) ^ (x>>10); }

static const uint32_t K[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static void sha256_transform(SHA256_CTX_SIMPLE *ctx, const unsigned char data[64]) {
    uint32_t W[64];
    for (int t=0;t<16;++t) {
        W[t] = (data[t*4]<<24) | (data[t*4+1]<<16) | (data[t*4+2]<<8) | (data[t*4+3]);
    }
    for (int t=16;t<64;++t) W[t] = sigma1(W[t-2]) + W[t-7] + sigma0(W[t-15]) + W[t-16];
    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
    for (int t=0;t<64;++t) {
        uint32_t T1 = h + Sigma1(e) + Ch(e,f,g) + K[t] + W[t];
        uint32_t T2 = Sigma0(a) + Maj(a,b,c);
        h = g; g = f; f = e; e = d + T1; d = c; c = b; b = a; a = T1 + T2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(SHA256_CTX_SIMPLE *ctx) {
    ctx->state[0]=0x6a09e667; ctx->state[1]=0xbb67ae85; ctx->state[2]=0x3c6ef372; ctx->state[3]=0xa54ff53a;
    ctx->state[4]=0x510e527f; ctx->state[5]=0x9b05688c; ctx->state[6]=0x1f83d9ab; ctx->state[7]=0x5be0cd19;
    ctx->bitcount = 0;
    memset(ctx->buffer,0,64);
}

static void sha256_update(SHA256_CTX_SIMPLE *ctx, const unsigned char *data, size_t len) {
    size_t idx = (ctx->bitcount/8) % 64;
    ctx->bitcount += (uint64_t)len * 8;
    while (len > 0) {
        size_t take = 64 - idx;
        if (take > len) take = len;
        memcpy(ctx->buffer + idx, data, take);
        idx += take; data += take; len -= take;
        if (idx == 64) { sha256_transform(ctx, ctx->buffer); idx = 0; }
    }
}

static void sha256_final(SHA256_CTX_SIMPLE *ctx, unsigned char out[32]) {
    size_t idx = (ctx->bitcount/8) % 64;
    ctx->buffer[idx++] = 0x80;
    if (idx > 56) {
        while (idx < 64) ctx->buffer[idx++] = 0;
        sha256_transform(ctx, ctx->buffer);
        idx = 0;
    }
    while (idx < 56) ctx->buffer[idx++] = 0;
    // append bitcount big-endian
    for (int i=7;i>=0;--i) ctx->buffer[idx++] = (unsigned char)((ctx->bitcount >> (i*8)) & 0xFF);
    sha256_transform(ctx, ctx->buffer);
    for (int i=0;i<8;++i) {
        out[i*4] = (ctx->state[i] >> 24) & 0xFF;
        out[i*4+1] = (ctx->state[i] >> 16) & 0xFF;
        out[i*4+2] = (ctx->state[i] >> 8) & 0xFF;
        out[i*4+3] = (ctx->state[i]) & 0xFF;
    }
}

static void hexify(const unsigned char *in, size_t inlen, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < inlen; ++i) {
        out[i*2] = hex[(in[i] >> 4) & 0xF];
        out[i*2+1] = hex[in[i] & 0xF];
    }
    out[inlen*2] = '\0';
}

int sha256_file_hex(const char *path, char out_hex[65]) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    SHA256_CTX_SIMPLE ctx; sha256_init(&ctx);
    unsigned char buf[8192];
    size_t n;
    while ((n = fread(buf,1,sizeof(buf),f)) > 0) sha256_update(&ctx, buf, n);
    unsigned char out[32]; sha256_final(&ctx, out);
    fclose(f);
    hexify(out, 32, out_hex);
    return 0;
}
