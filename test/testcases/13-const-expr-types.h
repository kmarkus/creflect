/*
 * constant expressions with C integer types: literal types, usual
 * arithmetic conversions, promotion and casts. long is 32 bit on
 * i386 and armhf, so results differ per ABI.
 */
enum ce_unsigned {
	CE_NOT0U = ~0U,			/* unsigned int: 4294967295 */
	CE_DIV = -1U / 2,
	CE_WRAP = 0xffffffffU + 1,	/* unsigned int wraps to 0 */
	CE_SHIFT = 1U << 31,
	CE_HEX = 0x80000000,		/* unsigned int */
	CE_NEGHEX = -0x80000000,	/* still unsigned int */
	CE_MOD = -5 % 3U,
	CE_SHORT = (short)-1 + 0U,
	CE_UCHAR = (unsigned char)300,
	CE_SCHAR = (signed char)200,
	CE_BOOL = (_Bool)7,
};

enum ce_long {
	CE_DEC = 2147483648,		/* long or long long */
	CE_NEGDEC = -2147483648,
	CE_UL = -1UL / 2,		/* depends on sizeof(long) */
	CE_LL = 1LL << 40,
	CE_SAR = -16 >> 2,
};

/* alone: with negative values gcc needs __int128 on 64 bit */
enum ce_size {
	CE_SIZE = sizeof(int) - 5,	/* size_t wraps */
	CE_MIX = -1 + 0UL,
};

enum ce_big {
	CE_BIG = 0xffffffffffffffffULL,
	CE_BIG2 = 0x8000000000000000,
	CE_SMALL = 1,
};

enum ce_char {
	CE_C1 = '\xff',			/* -1 with signed char */
	CE_C2 = '\012',
	CE_C3 = '\377',
	CE_C4 = '\a' + '\b' + '\f' + '\v' + '\?' + '\"',
};

struct ce_use {
	enum ce_unsigned u;
	enum ce_long l;
	enum ce_big b;
	enum ce_char c;
	char arr[CE_UCHAR];
	char arr2[(unsigned char)-1 % 7 + 1];
};
