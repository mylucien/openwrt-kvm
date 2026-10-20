#include <string.h>
#include "sha1b64.h"

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void block(uint32_t h[5], const uint8_t *p)
{
	uint32_t w[80], a, b, c, d, e, f, k, t;
	int i;
	for (i = 0; i < 16; i++)
		w[i] = (uint32_t)p[4*i] << 24 | p[4*i+1] << 16 | p[4*i+2] << 8 | p[4*i+3];
	for (; i < 80; i++) w[i] = ROL(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
	a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];
	for (i = 0; i < 80; i++) {
		if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
		else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
		else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
		else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
		t = ROL(a, 5) + f + e + k + w[i];
		e = d; d = c; c = ROL(b, 30); b = a; a = t;
	}
	h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void sha1(const uint8_t *d, size_t n, uint8_t out[20])
{
	uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
	uint8_t tail[128];
	size_t i, full = n / 64, rem = n % 64, tl;
	uint64_t bits = (uint64_t)n * 8;
	for (i = 0; i < full; i++) block(h, d + 64 * i);
	memset(tail, 0, sizeof tail);
	memcpy(tail, d + 64 * full, rem);
	tail[rem] = 0x80;
	tl = (rem < 56) ? 64 : 128;
	for (i = 0; i < 8; i++) tail[tl - 1 - i] = (uint8_t)(bits >> (8 * i));
	block(h, tail);
	if (tl == 128) block(h, tail + 64);
	for (i = 0; i < 5; i++) {
		out[4*i] = h[i] >> 24; out[4*i+1] = h[i] >> 16; out[4*i+2] = h[i] >> 8; out[4*i+3] = h[i];
	}
}

void b64enc(const uint8_t *d, size_t n, char *o)
{
	static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t i;
	for (i = 0; i + 2 < n; i += 3) {
		*o++ = T[d[i] >> 2];
		*o++ = T[(d[i] & 3) << 4 | d[i+1] >> 4];
		*o++ = T[(d[i+1] & 15) << 2 | d[i+2] >> 6];
		*o++ = T[d[i+2] & 63];
	}
	if (i < n) {
		*o++ = T[d[i] >> 2];
		if (i + 1 < n) {
			*o++ = T[(d[i] & 3) << 4 | d[i+1] >> 4];
			*o++ = T[(d[i+1] & 15) << 2];
		} else {
			*o++ = T[(d[i] & 3) << 4];
			*o++ = '=';
		}
		*o++ = '=';
	}
	*o = 0;
}
