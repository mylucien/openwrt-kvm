#include <string.h>
#include "ch9329.h"

size_t ch_frame(uint8_t *o, uint8_t addr, uint8_t cmd, const uint8_t *d, uint8_t len)
{
	size_t i, n = 0;
	unsigned sum = 0;
	o[n++] = 0x57; o[n++] = 0xAB; o[n++] = addr; o[n++] = cmd; o[n++] = len;
	for (i = 0; i < len; i++) o[n++] = d[i];
	for (i = 0; i < n; i++) sum += o[i];
	o[n++] = sum & 0xFF;
	return n;
}

size_t ch_info(uint8_t *o) { return ch_frame(o, 0, CH_CMD_INFO, NULL, 0); }

size_t ch_kbd(uint8_t *o, uint8_t mod, const uint8_t keys[6])
{
	uint8_t d[8] = { mod, 0 };
	memcpy(d + 2, keys, 6);
	return ch_frame(o, 0, CH_CMD_KBD, d, 8);
}

size_t ch_mouse_abs(uint8_t *o, uint8_t btn, uint16_t x, uint16_t y, int8_t wheel)
{
	/* 实测（Windows 主机）：x、y 同时为 0 的绝对坐标报告被忽略，(1,1) 则正常落到屏幕左上角，故规避 */
	if (x == 0 && y == 0) x = y = 1;
	uint8_t d[7] = { 0x02, btn, x & 0xFF, x >> 8, y & 0xFF, y >> 8, (uint8_t)wheel };
	return ch_frame(o, 0, CH_CMD_MOUSE_AB, d, 7);
}

int ch_feed(struct ch_parser *p, uint8_t b, uint8_t *cmd, uint8_t *data, uint8_t *len)
{
	int i, total;
	unsigned sum = 0;

	if (p->n == 0 && b != 0x57) return 0;
	if (p->n == 1 && b != 0xAB) { p->n = (b == 0x57) ? 1 : 0; return 0; }
	p->buf[p->n++] = b;
	if (p->n < 5) return 0;
	if (p->buf[4] > 64) { p->n = 0; return 0; }
	total = 6 + p->buf[4];
	if (p->n < total) return 0;
	for (i = 0; i < total - 1; i++) sum += p->buf[i];
	p->n = 0;
	if ((sum & 0xFF) != p->buf[total - 1]) return 0;
	*cmd = p->buf[3];
	*len = p->buf[4];
	memcpy(data, p->buf + 5, *len);
	return 1;
}
