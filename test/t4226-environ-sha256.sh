#! /bin/sh -e
# Known-answer tests for the portable environment fingerprint implementation.
. ./tup.sh

cat > hash-test.c << 'HERE'
#include "tup/sha256.h"
#include <stdio.h>
#include <string.h>

static int check(const char *input, unsigned int repeat, const char *expected)
{
	struct tup_sha256 ctx;
	unsigned char digest[32];
	char hex[65];
	unsigned int i;
	tup_sha256_init(&ctx);
	for(i=0; i<repeat; i++)
		tup_sha256_update(&ctx, input, strlen(input));
	tup_sha256_final(&ctx, digest);
	for(i=0; i<32; i++)
		sprintf(hex + 2*i, "%02x", digest[i]);
	if(strcmp(hex, expected)) {
		fprintf(stderr, "SHA-256: expected %s, got %s\n", expected, hex);
		return 1;
	}
	return 0;
}

int main(void)
{
	return check("", 1, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") ||
	       check("abc", 1, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") ||
	       check("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 1,
	             "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") ||
	       check("a", 1000000, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}
HERE
gcc -I../../src hash-test.c ../../src/tup/sha256.c -o hash-test.exe
./hash-test.exe

eotup
