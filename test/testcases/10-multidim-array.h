struct matrix {
	char tag;
	double m[3][4];
	short s[2][3][5];
	int (*rowp)[4];
	int *ptrs[2];
};
