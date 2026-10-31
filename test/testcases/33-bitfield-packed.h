#include <stdint.h>

struct bf_packed {
	uint8_t a : 3;
	uint64_t b : 64;	/* spans 9 bytes */
	uint32_t c : 13;
	int16_t d : 11;
	uint8_t e;
	int64_t f : 57;
} __attribute__((packed));

struct bf_packed_zero {
	uint8_t a : 3;
	uint32_t : 0;
	uint8_t b : 4;
} __attribute__((packed));

struct bf_packed_member {
	char a;
	uint32_t b : 20 __attribute__((packed));
	uint16_t c;
	uint64_t d __attribute__((packed));
};
