#ifndef CH9329_H
#define CH9329_H
#include <stddef.h>
#include <stdint.h>

#define CH_CMD_INFO     0x01
#define CH_CMD_KBD      0x02
#define CH_CMD_MOUSE_AB 0x04
#define CH_CMD_MOUSE_RE 0x05

size_t ch_frame(uint8_t *out, uint8_t addr, uint8_t cmd, const uint8_t *data, uint8_t len);
size_t ch_info(uint8_t *out);
size_t ch_kbd(uint8_t *out, uint8_t mod, const uint8_t keys[6]);
/* x,y: 0..4095 */
size_t ch_mouse_abs(uint8_t *out, uint8_t btn, uint16_t x, uint16_t y, int8_t wheel);

struct ch_parser { uint8_t buf[80]; int n; };
/* 逐字节喂入；收到一帧校验通过的完整帧时返回 1，并给出命令与数据。 */
int ch_feed(struct ch_parser *p, uint8_t b, uint8_t *cmd, uint8_t *data, uint8_t *len);
#endif
