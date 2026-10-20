/*
 * tup - A file-based build system
 *
 * SHA-256, as specified in FIPS 180-4. No platform crypto dependency.
 * Distributed under the GNU General Public License, version 2.
 */
#include "sha256.h"
#include <string.h>

static uint32_t rotate(uint32_t x, unsigned int n)
{
	return (x >> n) | (x << (32 - n));
}

static void transform(struct tup_sha256 *ctx)
{
	static const uint32_t k[64] = {
		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
		0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
		0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
		0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
		0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
		0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
		0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
		0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
		0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
		0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
		0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
		0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
		0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
		0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
		0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
		0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
	};
	uint32_t w[64];
	uint32_t a, b, c, d, e, f, g, h;
	unsigned int i;
	for(i=0; i<16; i++) {
		const unsigned char *p = &ctx->block[4*i];
		w[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
		       ((uint32_t)p[2] << 8) | p[3];
	}
	for(i=16; i<64; i++) {
		uint32_t s0 = rotate(w[i-15], 7) ^ rotate(w[i-15], 18) ^ (w[i-15] >> 3);
		uint32_t s1 = rotate(w[i-2], 17) ^ rotate(w[i-2], 19) ^ (w[i-2] >> 10);
		w[i] = w[i-16] + s0 + w[i-7] + s1;
	}
	a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
	e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];
	for(i=0; i<64; i++) {
		uint32_t s1 = rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25);
		uint32_t t1 = h + s1 + ((e & f) ^ (~e & g)) + k[i] + w[i];
		uint32_t s0 = rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22);
		uint32_t t2 = s0 + ((a & b) ^ (a & c) ^ (b & c));
		h = g; g = f; f = e; e = d + t1;
		d = c; c = b; b = a; a = t1 + t2;
	}
	ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
	ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void tup_sha256_init(struct tup_sha256 *ctx)
{
	static const uint32_t initial[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
	};
	memcpy(ctx->state, initial, sizeof(initial));
	ctx->bytes = 0;
	ctx->used = 0;
}

void tup_sha256_update(struct tup_sha256 *ctx, const void *data, size_t len)
{
	const unsigned char *p = data;
	ctx->bytes += len;
	while(len) {
		size_t n = sizeof(ctx->block) - ctx->used;
		if(n > len)
			n = len;
		memcpy(ctx->block + ctx->used, p, n);
		ctx->used += n;
		p += n;
		len -= n;
		if(ctx->used == sizeof(ctx->block)) {
			transform(ctx);
			ctx->used = 0;
		}
	}
}

void tup_sha256_final(struct tup_sha256 *ctx, unsigned char digest[32])
{
	uint64_t bits = ctx->bytes * 8;
	unsigned int i;
	ctx->block[ctx->used++] = 0x80;
	if(ctx->used > 56) {
		memset(ctx->block + ctx->used, 0, 64 - ctx->used);
		transform(ctx);
		ctx->used = 0;
	}
	memset(ctx->block + ctx->used, 0, 56 - ctx->used);
	for(i=0; i<8; i++)
		ctx->block[63-i] = (unsigned char)(bits >> (8*i));
	transform(ctx);
	for(i=0; i<32; i++)
		digest[i] = (unsigned char)(ctx->state[i/4] >> (24 - 8*(i%4)));
}
