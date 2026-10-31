#ifndef _08_STRUCT_PACKED_ATTRIBUTE
#define _08_STRUCT_PACKED_ATTRIBUTE
#include <stdint.h>

struct packedstruct
{
	uint8_t first;
	uint64_t second;
} __attribute__((packed));

#endif
