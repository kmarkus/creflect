/* 32 bit targets have no __int128 */
#ifdef __SIZEOF_INT128__
/* s390x aligns __int128 to 8, aligned(16) on typedefs is unsupported */
#ifndef __s390x__
typedef __signed__ __int128 s128 __attribute__((aligned(16)));
#else
typedef __signed__ __int128 s128;
#endif
typedef int ti_t __attribute__((mode(TI)));

struct wide {
	char c;
	__int128 i;
	unsigned __int128 u;
	__int128_t it;
	__uint128_t ut;
	s128 s;
	ti_t ti;
	unsigned long long lo : 40;
};
#endif

/* stdarg.h: typedef __builtin_va_list va_list */
struct with_va {
	char c;
	__builtin_va_list ap;
	__builtin_va_list bap[2];
	short s;
};

/* glibc sys/types.h */
typedef int register_t __attribute__((__mode__(__word__)));
typedef unsigned int u8_t __attribute__((mode(QI)));

struct modes {
	register_t r;
	u8_t b;
	int h __attribute__((__mode__(__HI__)));
	unsigned int p __attribute__((mode(pointer)));
	int bf : 5 __attribute__((mode(DI)));
};
