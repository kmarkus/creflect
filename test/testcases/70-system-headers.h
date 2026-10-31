#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define NAME_LEN 16

typedef struct {
	int64_t ts;
	uint32_t seq;
	bool valid;
	size_t len;
	uintptr_t addr;
	ptrdiff_t delta;
	char name[NAME_LEN];
	long double ld;
	float f;
	unsigned char *buf;
	const char *label;
	volatile unsigned int *reg;
} sample_t;
