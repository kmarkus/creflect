enum sizes {
	SZ_A = 4,
	SZ_B = SZ_A * 2 + 1,
	SZ_C = (1 << 4) | 3,
	SZ_D = sizeof(long) / 2,
	SZ_E = -3 + 10 % 4,
	SZ_F = 'A',
};

struct dims {
	char a[SZ_A];
	char b[SZ_B];
	char c[SZ_C];
	char d[SZ_D];
	char e[sizeof(int) * 3];
	char f[SZ_F - 60];
};
