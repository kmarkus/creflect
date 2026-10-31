typedef int myint;
typedef myint myint2;
typedef struct { myint a; char b; } anon_t;
typedef struct named { double d; myint2 i; } named_t, *named_ptr_t;
typedef unsigned long long ull_t;

struct uses_typedefs {
	anon_t a;
	named_t n;
	named_ptr_t p;
	ull_t u;
	myint2 arr[3];
};
