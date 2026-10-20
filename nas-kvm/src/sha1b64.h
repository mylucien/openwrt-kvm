#ifndef SHA1B64_H
#define SHA1B64_H
#include <stddef.h>
#include <stdint.h>
void sha1(const uint8_t *d, size_t n, uint8_t out[20]);
/* out 至少 4*((n+2)/3)+1 字节 */
void b64enc(const uint8_t *d, size_t n, char *out);
#endif
