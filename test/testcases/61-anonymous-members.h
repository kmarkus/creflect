struct anon {
	int kind;
	union {
		int i;
		double d;
		struct {
			short lo;
			short hi;
		};
	};
	struct {
		unsigned flag_a : 1;
		unsigned flag_b : 1;
	};
	char tail;
};

struct nested_defs {
	struct inner_def {
		int x;
		char y;
	} in;
	enum inner_enum { IE_A, IE_B } e;
	struct inner_def arr[2];
};
