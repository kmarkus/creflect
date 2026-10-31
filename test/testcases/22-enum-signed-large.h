enum neg { NEG_A = -1, NEG_B = 5 };
enum large { LARGE_A = 0, LARGE_B = 0x100000000 };
enum neglarge { NL_A = -0x100000000, NL_B };
enum __attribute__((packed)) small { SMALL_A, SMALL_B = 200 };
enum __attribute__((packed)) smallneg { SN_A = -2, SN_B = 300 };

struct enums {
	char c;
	enum neg n;
	enum large l;
	enum neglarge nl;
	enum small s;
	enum smallneg sn;
};
