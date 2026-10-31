struct bf_basic {
	unsigned int a : 3;
	unsigned int b : 5;
	int c : 7;
	unsigned int d : 20;	/* does not fit after c: next unit */
	int e : 1;
};

struct bf_full {
	unsigned int x : 32;
	unsigned long long y : 64;
	long long z : 63;
};
