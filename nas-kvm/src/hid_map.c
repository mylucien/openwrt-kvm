#include <stdlib.h>
#include <string.h>
#include "hid_map.h"

static const struct { const char *code; unsigned char usage; } T[] = {
	{"Enter",0x28},{"Escape",0x29},{"Backspace",0x2A},{"Tab",0x2B},{"Space",0x2C},
	{"Minus",0x2D},{"Equal",0x2E},{"BracketLeft",0x2F},{"BracketRight",0x30},{"Backslash",0x31},
	{"Semicolon",0x33},{"Quote",0x34},{"Backquote",0x35},{"Comma",0x36},{"Period",0x37},
	{"Slash",0x38},{"CapsLock",0x39},{"PrintScreen",0x46},{"ScrollLock",0x47},{"Pause",0x48},
	{"Insert",0x49},{"Home",0x4A},{"PageUp",0x4B},{"Delete",0x4C},{"End",0x4D},{"PageDown",0x4E},
	{"ArrowRight",0x4F},{"ArrowLeft",0x50},{"ArrowDown",0x51},{"ArrowUp",0x52},
	{"NumLock",0x53},{"NumpadDivide",0x54},{"NumpadMultiply",0x55},{"NumpadSubtract",0x56},
	{"NumpadAdd",0x57},{"NumpadEnter",0x58},{"NumpadDecimal",0x63},
	{"IntlBackslash",0x64},{"ContextMenu",0x65},
	{"ControlLeft",0xE0},{"ShiftLeft",0xE1},{"AltLeft",0xE2},{"MetaLeft",0xE3},
	{"ControlRight",0xE4},{"ShiftRight",0xE5},{"AltRight",0xE6},{"MetaRight",0xE7},
};

int hid_from_code(const char *c)
{
	size_t i;
	if (!c || !*c) return 0;
	if (!strncmp(c, "Key", 3) && c[3] >= 'A' && c[3] <= 'Z' && !c[4])
		return 0x04 + (c[3] - 'A');
	if (!strncmp(c, "Digit", 5) && c[5] >= '0' && c[5] <= '9' && !c[6])
		return c[5] == '0' ? 0x27 : 0x1E + (c[5] - '1');
	if (c[0] == 'F' && c[1] >= '1' && c[1] <= '9') {
		int n = atoi(c + 1);
		if (n >= 1 && n <= 12) return 0x3A + n - 1;
		return 0;
	}
	if (!strncmp(c, "Numpad", 6) && c[6] >= '0' && c[6] <= '9' && !c[7])
		return c[6] == '0' ? 0x62 : 0x59 + (c[6] - '1');
	for (i = 0; i < sizeof(T) / sizeof(T[0]); i++)
		if (!strcmp(T[i].code, c)) return T[i].usage;
	return 0;
}

int hid_from_macro_name(const char *n)
{
	if (!n) return 0;
	if (!strcmp(n, "Esc")) return 0x29;
	if (!strcmp(n, "Del")) return 0x4C;
	if (!strcmp(n, "Tab")) return 0x2B;
	if (n[0] == 'F' && n[1] >= '1' && n[1] <= '9') {
		char *e; long v = strtol(n + 1, &e, 10);
		if (!*e && v >= 1 && v <= 12) return 0x3A + (int)v - 1;
	}
	return 0;
}
