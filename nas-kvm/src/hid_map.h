#ifndef HID_MAP_H
#define HID_MAP_H
/* 浏览器 KeyboardEvent.code -> USB HID usage。修饰键返回 0xE0..0xE7，未知返回 0。 */
int hid_from_code(const char *code);
/* 开机宏的键名（F1..F12 / Esc / Del / Tab）-> HID usage，未知返回 0。 */
int hid_from_macro_name(const char *name);
#endif
