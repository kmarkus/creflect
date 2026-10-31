union bf_union {
	unsigned int a : 3;
	unsigned long long b : 40;
	char c;
};

union bf_union_small {
	unsigned char a : 3;
	unsigned short b : 9;
};

struct reg {
	union {
		unsigned int raw;
		struct {
			unsigned int en : 1;
			unsigned int mode : 3;
			unsigned int : 4;
			unsigned int div : 8;
			unsigned int reserved : 16;
		} f;
	} r;
};
