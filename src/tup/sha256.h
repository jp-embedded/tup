/*
 * tup - A file-based build system
 *
 * SHA-256 for persistent environment fingerprints.
 * Distributed under the GNU General Public License, version 2.
 */
#ifndef tup_sha256_h
#define tup_sha256_h

#include <stddef.h>
#include <stdint.h>

struct tup_sha256 {
	uint32_t state[8];
	uint64_t bytes;
	unsigned char block[64];
	size_t used;
};

void tup_sha256_init(struct tup_sha256 *ctx);
void tup_sha256_update(struct tup_sha256 *ctx, const void *data, size_t len);
void tup_sha256_final(struct tup_sha256 *ctx, unsigned char digest[32]);

#endif
