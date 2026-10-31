#include <stdint.h>

struct bf_mixed {
	char c;
	uint8_t a : 3;
	uint16_t b : 9;
	uint32_t d : 17;
	uint64_t e : 40;
	short f : 4;
	char g : 2;
	unsigned long long h : 33;
	unsigned char i : 8;
	int j;
};

struct bf_then_member {
	unsigned char a : 1;
	char b;
	unsigned short c : 15;
	int d : 3;
	double e;
	unsigned f : 1;
};
