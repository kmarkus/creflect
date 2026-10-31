struct bf_zero {
	unsigned char a : 3;
	unsigned int : 0;
	unsigned char b : 2;
	unsigned long long : 0;
	unsigned char c : 1;
	unsigned short : 0;
	unsigned char d : 4;
};

struct bf_unnamed {
	char a;
	int : 5;
	long long : 3;
	char b : 2;
};

struct bf_unnamed_only {
	char a;
	long long : 7;
};

/* trailing zero width bitfields extend the size */
struct bf_zero_tail {
	char a;
	int : 0;
};

struct bf_zero_tail_ll {
	char a;
	long long : 0;
	char b;
};

struct bf_zero_tail_packed {
	char a;
	int : 0;
} __attribute__((packed));
