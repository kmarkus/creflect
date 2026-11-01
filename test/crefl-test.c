// SPDX-License-Identifier: LGPL-2.0-or-later
/*
 * crefl unit tests. Layout and access of the testcases are checked
 * against the compiler by test/oracle.sh, this covers the rest.
 *
 * Copyright (C) 2019-2026 Markus Klotzbuecher <mk@mkio.de>
 */

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include "crefl.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static crefl_ctx_t *parse(const char *src)
{
	crefl_ctx_t *ctx = crefl_ctx_new();

	assert_non_null(ctx);
	if (crefl_parse(ctx, src))
		fail_msg("parse: %s", crefl_errmsg(ctx));
	return ctx;
}

static void parse_err(const char *src, const char *msg)
{
	crefl_ctx_t *ctx = crefl_ctx_new();

	assert_int_equal(crefl_parse(ctx, src), -EINVAL);
	if (strstr(crefl_errmsg(ctx), msg) == NULL)
		fail_msg("expected '%s', got '%s'", msg, crefl_errmsg(ctx));
	crefl_ctx_free(ctx);
}

static void test_type_names(void **state)
{
	(void)state;
	crefl_ctx_t *ctx = parse("struct s { int a; }; typedef struct s s_t;");
	const crefl_type_t *t;

	assert_non_null(t = crefl_type(ctx, "unsigned long int"));
	assert_int_equal(crefl_prim(t), CREFL_ULONG);
	assert_int_equal(crefl_prim(crefl_type(ctx, "long unsigned")),
			 CREFL_ULONG);
	assert_int_equal(crefl_prim(crefl_type(ctx, "signed char")),
			 CREFL_SCHAR);
	assert_int_equal(crefl_prim(crefl_type(ctx, "long long int")),
			 CREFL_LLONG);
	assert_int_equal(crefl_prim(crefl_type(ctx, "long double")),
			 CREFL_LDOUBLE);
	assert_int_equal(crefl_prim(crefl_type(ctx, "unsigned")), CREFL_UINT);
	assert_int_equal(crefl_prim(crefl_resolve(crefl_type(ctx, "uint16_t"))),
			 CREFL_USHORT);

	assert_non_null(t = crefl_type(ctx, "int[4]"));
	assert_int_equal(crefl_kind(t), CREFL_ARRAY);
	assert_int_equal(crefl_sizeof(t), 4 * sizeof(int));

	assert_non_null(t = crefl_type(ctx, "int (*)[3]"));
	assert_int_equal(crefl_kind(t), CREFL_PTR);
	assert_int_equal(crefl_len(crefl_elem(t)), 3);

	assert_non_null(t = crefl_type(ctx, "s_t *"));
	assert_int_equal(crefl_kind(t), CREFL_PTR);
	assert_ptr_equal(crefl_resolve(crefl_elem(t)),
			 crefl_type(ctx, "struct s"));

	assert_null(crefl_type(ctx, "nosuch_t"));
	assert_null(crefl_type(ctx, "int x"));
	assert_null(crefl_type(ctx, "union s"));
	crefl_ctx_free(ctx);
}

static void test_introspection(void **state)
{
	(void)state;
	crefl_ctx_t *ctx = parse(
		"enum e { A = -1, B };"
		"struct s { char c; unsigned x : 3; union { int i; float f; }; int : 0; };");
	const crefl_type_t *t = crefl_type(ctx, "struct s");
	const crefl_member_t *m;

	assert_int_equal(crefl_ntypes(ctx), 2);
	assert_ptr_equal(crefl_type_at(ctx, 1), t);
	assert_null(crefl_type_at(ctx, 2));

	assert_int_equal(crefl_len(t), 4);
	m = crefl_member(t, 1);
	assert_string_equal(m->name, "x");
	assert_int_equal(m->bit_width, 3);
	assert_null(crefl_member(t, 2)->name);
	assert_int_equal(crefl_kind(crefl_member(t, 2)->type), CREFL_UNION);
	assert_null(crefl_member(t, 4));

	t = crefl_type(ctx, "enum e");
	assert_true(crefl_is_signed(t));
	assert_int_equal(crefl_len(t), 2);
	assert_string_equal(crefl_enumval(t, 1)->name, "B");
	assert_int_equal(crefl_enumval(t, 1)->val, 0);
	crefl_ctx_free(ctx);
}

static void test_define(void **state)
{
	(void)state;
	crefl_ctx_t *ctx = crefl_ctx_new();

	assert_int_equal(crefl_define(ctx, "N", 7), 0);
	assert_int_equal(crefl_parse(ctx, "struct s { char a[N * 2]; };"), 0);
	assert_int_equal(crefl_sizeof(crefl_type(ctx, "struct s")), 14);

	/* names must be identifiers that are not typedefs */
	assert_int_equal(crefl_define(ctx, "1x", 1), -EINVAL);
	assert_string_equal(crefl_errmsg(ctx), "define: invalid name '1x'");
	assert_int_equal(crefl_define(ctx, "", 1), -EINVAL);
	assert_int_equal(crefl_define(ctx, "a-b", 1), -EINVAL);
	assert_int_equal(crefl_define(ctx, "uint8_t", 1), -EINVAL);
	assert_int_equal(crefl_define(ctx, NULL, 1), -EINVAL);
	assert_int_equal(crefl_define(ctx, "_N2", 2), 0);
	crefl_ctx_free(ctx);
}

/* holes and padding are printed, zero width bitfields at their offset */
static void test_print_holes(void **state)
{
	(void)state;
	crefl_ctx_t *ctx = crefl_ctx_new();
	char *out = NULL;
	size_t len = 0;
	FILE *f;

	assert_int_equal(crefl_parse(ctx, "struct h {\n"
					  "\tchar c;\n"
					  "\tint i;\n"
					  "\tunsigned a : 3;\n"
					  "\tint : 0;\n"
					  "\tchar d;\n"
					  "};\n"
					  "union u { char c; int i; };\n"),
			 0);
	f = open_memstream(&out, &len);
	crefl_print(crefl_type(ctx, "struct h"), f);
	crefl_print(crefl_type(ctx, "union u"), f);
	fclose(f);
	assert_string_equal(out, "/* size=16, align=4 */\n"
				 "struct h {\n"
				 "\tchar c; /* off=0, size=1 */\n"
				 "\t/* XXX 3 bytes hole */\n"
				 "\tint i; /* off=4, size=4 */\n"
				 "\tunsigned int a : 3; /* off=8, bit=0 */\n"
				 "\t/* XXX 5 bits hole */\n"
				 "\t/* XXX 3 bytes hole */\n"
				 "\tint : 0; /* off=12, bit=0 */\n"
				 "\tchar d; /* off=12, size=1 */\n"
				 "\t/* XXX 3 bytes padding */\n"
				 "};\n"
				 "/* size=4, align=4 */\n"
				 "union u {\n"
				 "\tchar c; /* off=0, size=1 */\n"
				 "\tint i; /* off=0, size=4 */\n"
				 "};\n");
	free(out);
	crefl_ctx_free(ctx);
}

static void test_access(void **state)
{
	(void)state;
	struct s {
		int i;
		struct {
			short a[3];
		} in[2];
		char name[8];
		double d;
		int *p;
		unsigned int u : 4;
		long long ll : 40;
	} x = { 0 };
	int target = 42;
	crefl_ctx_t *ctx = parse(
		"struct s { int i; struct { short a[3]; } in[2]; char name[8];"
		"double d; int *p; unsigned u : 4; long long ll : 40; };");
	crefl_val_t v = crefl_wrap(crefl_type(ctx, "struct s"), &x);
	crefl_path_t p;
	int64_t i;
	uint64_t u;
	double d;

	/* precompiled path */
	assert_int_equal(crefl_path(v.t, "in[1].a[2]", &p), 0);
	assert_int_equal(p.off, offsetof(struct s, in[1].a[2]));
	assert_int_equal(crefl_set_i64(crefl_at(v, &p), -5), 0);
	assert_int_equal(x.in[1].a[2], -5);

	assert_int_equal(crefl_path(v.t, "in[2]", &p), -ERANGE);
	assert_int_equal(crefl_path(v.t, "nosuch", &p), -ENOENT);
	assert_int_equal(crefl_path(v.t, "i.x", &p), -EINVAL);
	assert_int_equal(crefl_path(v.t, "u.x", &p), -EINVAL);
	assert_int_equal(crefl_path(v.t, "in[x]", &p), -EINVAL);
	assert_null(crefl_get(v, "nosuch").t);

	/* index */
	assert_int_equal(
		crefl_set_i64(crefl_index(crefl_get(v, "in[0].a"), 1), 9), 0);
	assert_int_equal(x.in[0].a[1], 9);
	assert_null(crefl_index(crefl_get(v, "in[0].a"), 3).t);

	/* conversions */
	assert_int_equal(crefl_set_f64(crefl_get(v, "i"), 3.0), 0);
	assert_int_equal(x.i, 3);
	assert_int_equal(crefl_set_f64(crefl_get(v, "i"), 3.5), -ERANGE);
	assert_int_equal(crefl_set_f64(crefl_get(v, "i"), 1e30), -ERANGE);
	assert_int_equal(crefl_set_i64(crefl_get(v, "i"), INT64_C(1) << 40),
			 -ERANGE);
	assert_int_equal(crefl_set_i64(crefl_get(v, "d"), -7), 0);
	assert_true(x.d == -7.0);
	assert_int_equal(crefl_get_f64(crefl_get(v, "i"), &d), 0);
	assert_true(d == 3.0);
	assert_int_equal(crefl_get_i64(crefl_get(v, "d"), &i), -EINVAL);
	assert_int_equal(crefl_set_i64(crefl_get(v, "in"), 1), -EINVAL);

	/* bitfields */
	assert_int_equal(crefl_set_u64(crefl_get(v, "u"), 15), 0);
	assert_int_equal(x.u, 15);
	assert_int_equal(crefl_set_u64(crefl_get(v, "u"), 16), -ERANGE);
	assert_int_equal(crefl_set_i64(crefl_get(v, "ll"), -(INT64_C(1) << 39)),
			 0);
	assert_true(x.ll == -(INT64_C(1) << 39));
	assert_int_equal(crefl_get_u64(crefl_get(v, "ll"), &u), -ERANGE);

	/* strings */
	assert_int_equal(crefl_set_str(crefl_get(v, "name"), "1234567"), 0);
	assert_string_equal(x.name, "1234567");
	assert_int_equal(crefl_set_str(crefl_get(v, "name"), "12345678"),
			 -ERANGE);
	assert_int_equal(crefl_set_str(crefl_get(v, "i"), "x"), -EINVAL);

	/* pointers */
	x.p = &target;
	assert_int_equal(crefl_get_i64(crefl_deref(crefl_get(v, "p")), &i), 0);
	assert_int_equal(i, 42);
	x.p = NULL;
	assert_null(crefl_deref(crefl_get(v, "p")).t);

	/* invalid values propagate */
	assert_int_equal(crefl_set_i64(crefl_get(v, "nosuch"), 1), -EINVAL);
	crefl_ctx_free(ctx);
}

static void test_new_copy(void **state)
{
	(void)state;
	crefl_ctx_t *ctx = parse(
		"struct s { int a; unsigned b : 3; }; struct o { int a; };");
	crefl_val_t a = crefl_new(crefl_type(ctx, "struct s"));
	crefl_val_t b = crefl_new(crefl_type(ctx, "struct s"));
	crefl_val_t o = crefl_new(crefl_type(ctx, "struct o"));
	int64_t i;

	assert_non_null(a.p);
	assert_int_equal(crefl_set_i64(crefl_get(a, "a"), 11), 0);
	assert_int_equal(crefl_set_i64(crefl_get(a, "b"), 5), 0);
	assert_int_equal(crefl_copy(b, a), 0);
	assert_int_equal(crefl_get_i64(crefl_get(b, "b"), &i), 0);
	assert_int_equal(i, 5);
	assert_int_equal(crefl_copy(o, a), -EINVAL);

	/* bitfield to bitfield */
	assert_int_equal(crefl_set_i64(crefl_get(a, "b"), 2), 0);
	assert_int_equal(crefl_copy(crefl_get(b, "b"), crefl_get(a, "b")), 0);
	assert_int_equal(crefl_get_i64(crefl_get(b, "b"), &i), 0);
	assert_int_equal(i, 2);

	assert_null(crefl_new(crefl_type(ctx, "void")).p);
	crefl_free(a);
	crefl_free(b);
	crefl_free(o);
	crefl_ctx_free(ctx);
}

static void test_static_ctx(void **state)
{
	(void)state;
	static _Alignas(16) unsigned char buf[8192];
	static unsigned char tiny[4096];
	char src[64];
	int ret = 0;
	crefl_ctx_t *ctx = crefl_ctx_new_static(buf + 1, sizeof(buf) - 1);

	assert_non_null(ctx);
	assert_int_equal(crefl_parse(ctx, "struct s { int a; char b[3]; };"),
			 0);
	assert_int_equal(crefl_sizeof(crefl_type(ctx, "struct s")), 8);
	crefl_ctx_free(ctx);

	assert_null(crefl_ctx_new_static(tiny, 16));
	ctx = crefl_ctx_new_static(tiny, sizeof(tiny));
	assert_non_null(ctx);

	/* fill the buffer until parsing fails */
	for (int i = 0; i < 1000 && ret == 0; i++) {
		snprintf(src, sizeof(src), "struct s%d { int a, b, c; };", i);
		ret = crefl_parse(ctx, src);
	}
	assert_int_equal(ret, -ENOMEM);
	assert_non_null(strstr(crefl_errmsg(ctx), "out of memory"));
}

/*
 * parse into static contexts of every size: each must either succeed
 * or fail with -ENOMEM, never crash (all allocation failure paths)
 */
static void test_oom_sweep(void **state)
{
	(void)state;
	static _Alignas(16) unsigned char buf[64 * 1024];
	const char *src =
		"enum e { A, B = 5 };"
		"typedef int t, *tp;"
		"struct s { int a, b[2][3]; struct { char c : 3; } in; union { t x; tp y; };"
		" enum e e; char flex[]; } __attribute__((packed));"
		"typedef struct s s_t;"
		"struct fwd *f;";
	int ok = 0;

	for (size_t len = 0; len <= sizeof(buf) && !ok; len += 8) {
		crefl_ctx_t *ctx = crefl_ctx_new_static(buf, len);
		int ret;

		if (ctx == NULL)
			continue;
		ret = crefl_parse(ctx, src);
		if (ret != 0 && ret != -ENOMEM)
			fail_msg("len %zu: %d %s", len, ret, crefl_errmsg(ctx));
		if (ret == 0) {
			ok = crefl_type(ctx, "int (*)[2]") != NULL;
			continue;
		}

		/* rolled back: nothing of the failed parse is visible */
		const crefl_type_t *st = crefl_type(ctx, "struct s");

		assert_int_equal(crefl_ntypes(ctx), 0);
		assert_true(st == NULL || !crefl_is_complete(st));
		assert_null(crefl_type(ctx, "t"));
	}
	assert_true(ok);
}

/* a failed parse leaves the context as it was */
static void test_rollback(void **state)
{
	(void)state;
	crefl_ctx_t *ctx = parse("struct fwd; typedef int keep_t;");

	assert_int_equal(crefl_parse(ctx, "struct q { int a; foo_t b; };"),
			 -EINVAL);
	assert_int_equal(crefl_parse(ctx, "struct q { int a; };"), 0);

	/* everything before the error is undone, also a completed forward */
	assert_int_equal(
		crefl_parse(ctx, "typedef long T; enum { E1 = 1 };"
				 "struct fwd { int x; };"
				 "enum en { EN1 } ; struct z { bad_t y; };"),
		-EINVAL);
	assert_false(crefl_is_complete(crefl_type(ctx, "struct fwd")));
	assert_null(crefl_type(ctx, "T"));
	assert_int_equal(crefl_ntypes(ctx), 2);
	assert_int_equal(crefl_parse(ctx, "typedef char T; enum { E1 = 2 };"
					  "enum en { EN2 };"),
			 0);
	assert_int_equal(crefl_sizeof(crefl_type(ctx, "T")), 1);

	/* failed crefl_type does not leave a half defined struct */
	assert_null(crefl_type(ctx, "struct anon_in_type { nosuch_t x; }"));
	assert_int_equal(crefl_parse(ctx, "struct anon_in_type { int x; };"),
			 0);
	crefl_ctx_free(ctx);
}

/* review findings: each scenario that failed before the fix */
static void test_review_fixes(void **state)
{
	(void)state;
	crefl_ctx_t *ctx = parse(
		"struct s { int *a; int *b; char n[4]; int m[2][3]; };"
		"struct t { char n[4]; int m[2][3]; };"
		"struct r { char c; int x[]; };"
		"static const float k = 1.5f;"
		"static const double d = 1e-3, h = 0x1p4;"
		"static inline double f(double x) { return x * 0.5 + .25; }"
		"extern \"C\" {\n"
		"struct in_c { int a; };\n"
		"}\n"
		"extern \"C\" int cfun(void);");
	struct {
		int *a;
		int *b;
		char n[4];
		int m[2][3];
	} s = { 0 };
	struct {
		char n[4];
		int m[2][3];
	} t = { "abc", { { 1, 2, 3 }, { 4, 5, 6 } } };
	crefl_val_t vs = crefl_wrap(crefl_type(ctx, "struct s"), &s);
	crefl_val_t vt = crefl_wrap(crefl_type(ctx, "struct t"), &t);
	crefl_path_t p;
	int x = 7;

	/* crefl_copy between equal derived types of different declarators */
	s.b = &x;
	assert_int_equal(crefl_copy(crefl_get(vs, "a"), crefl_get(vs, "b")), 0);
	assert_ptr_equal(s.a, &x);
	assert_int_equal(crefl_copy(crefl_get(vs, "n"), crefl_get(vt, "n")), 0);
	assert_string_equal(s.n, "abc");
	assert_int_equal(crefl_copy(crefl_get(vs, "m"), crefl_get(vt, "m")), 0);
	assert_int_equal(s.m[1][2], 6);
	assert_int_equal(crefl_copy(crefl_get(vs, "m[0]"), crefl_get(vt, "n")),
			 -EINVAL);

	/* extern "C" blocks */
	assert_true(crefl_is_complete(crefl_type(ctx, "struct in_c")));

	/* path indices: no sign, no overflow, also for flexible arrays */
	const crefl_type_t *r = crefl_type(ctx, "struct r");

	assert_int_equal(crefl_path(r, "x[-1]", &p), -EINVAL);
	assert_int_equal(crefl_path(r, "x[ 1]", &p), -EINVAL);
	assert_int_equal(crefl_path(r, "x[0xffffffffffffffff]", &p), -ERANGE);
	assert_int_equal(crefl_path(r, "x[99999999999999999999]", &p), -ERANGE);
	assert_int_equal(crefl_path(r, "x[3]", &p), 0);
	assert_int_equal(p.off, offsetof(
					struct {
						char c;
						int x[];
					},
					x[3]));

	/* floats only fail when a constant expression uses them */
	assert_int_equal(crefl_parse(ctx, "enum { FL = 1.5 };"), -EINVAL);
	assert_non_null(strstr(crefl_errmsg(ctx), "unsupported number '1.5'"));
	crefl_ctx_free(ctx);
}

static void test_print_roundtrip(void **state)
{
	(void)state;
	const char *src = "enum e { E0, E1 = 5 };"
			  "struct in { char c; };"
			  "struct s {"
			  "  int a, *b, c[2][3];"
			  "  int (*rp)[4];"
			  "  struct in i;"
			  "  union { int x; struct { short lo, hi; }; };"
			  "  unsigned f : 3, : 0, g : 5;"
			  "  enum e e;"
			  "  char tail;"
			  "} __attribute__((packed));"
			  "typedef struct { long l; } anon_t;"
			  "typedef struct s s_t;";
	crefl_ctx_t *a = parse(src), *b = crefl_ctx_new();
	char *out = NULL;
	size_t len = 0;
	FILE *f = open_memstream(&out, &len);

	for (size_t i = 0; i < crefl_ntypes(a); i++)
		crefl_print(crefl_type_at(a, i), f);
	fclose(f);

	if (crefl_parse(b, out))
		fail_msg("reparse: %s\n%s", crefl_errmsg(b), out);

	assert_int_equal(crefl_ntypes(a), crefl_ntypes(b));
	for (size_t i = 0; i < crefl_ntypes(a); i++) {
		const crefl_type_t *ta = crefl_type_at(a, i),
				   *tb = crefl_type_at(b, i);

		assert_int_equal(crefl_sizeof(ta), crefl_sizeof(tb));
		assert_int_equal(crefl_alignof(ta), crefl_alignof(tb));
		assert_int_equal(crefl_len(ta), crefl_len(tb));
		for (size_t j = 0; crefl_member(ta, j); j++) {
			assert_int_equal(crefl_member(ta, j)->off,
					 crefl_member(tb, j)->off);
			assert_int_equal(crefl_member(ta, j)->bit_off,
					 crefl_member(tb, j)->bit_off);
		}
	}

	free(out);
	crefl_ctx_free(a);
	crefl_ctx_free(b);
}

static void test_skipped_decls(void **state)
{
	(void)state;
	crefl_ctx_t *ctx =
		parse("#define FOO(x) \\\n  (x + 1)\n"
		      "# 1 \"file.h\"\n"
		      "extern int v1, v2[3] __attribute__((unused));\n"
		      "static const int c = (1, 2);\n"
		      "int f(int a, struct s *p) __attribute__((nonnull));\n"
		      "static inline int g(void) { if (1) { return 0; } }\n"
		      "_Static_assert(sizeof(int) == 4, \"int\");\n"
		      "void (*signal(int sig, void (*func)(int)))(int);\n"
		      "struct s { int a; } s1, *s2;\n");

	assert_int_equal(crefl_ntypes(ctx), 1);
	crefl_ctx_free(ctx);
}

/* every parser error path, the message must contain the expected text */
static const struct {
	const char *src;
	const char *msg;
} errors[] = {
	/* lexer */
	{ "/* never closed", "unterminated comment" },
	{ "enum { A = 'a };", "unsupported character literal" },
	{ "enum { A = '", "unterminated character literal" },
	{ "enum { A = '\\", "unterminated character literal" },
	{ "enum { A = '\n' };", "unterminated character literal" },
	{ "enum { A = 'ab' };", "unsupported character literal" },
	{ "enum { A = 1.5 };", "unsupported number" },
	{ "enum { A = 0x1g };", "unsupported number" },
	{ "int x asm(\"abc);", "unterminated string" },
	{ "#pragma pack(1)\nstruct s { int a; };", "#pragma pack" },
	{ "#  pragma   pack(push, 1)\n", "#pragma pack" },

	/* structure */
	{ "struct s { int a; ", "expected '}' before end of input" },
	{ "struct s { int a }", "expected ';' before '}'" },
	{ "struct s { int a; }", "expected ';' before end of input" },
	{ "int f(int a;", "unbalanced brackets" },
	{ "_Static_assert x;", "expected '('" },
	{ "struct;", "expected '{'" },
	{ "union s; struct s { int a; };", "wrong kind of tag" },
	{ "struct s { int a; };\nstruct s { int b; };",
	  "line 2: redefinition of 's'" },
	{ "struct s { struct s { int x; } in; };", "redefinition" },
	{ "struct s { struct t x; };", "incomplete type" },
	{ "struct s { void v; };", "incomplete type" },
	{ "struct s { int *; };", "expected member name" },
	{ "struct s { typedef int x; };", "unexpected typedef" },
	{ "struct s { foo_t a; };", "line 1: unknown type name 'foo_t'" },
	{ "foo_t x;", "unknown type name 'foo_t'" },
	{ "123;", "expected declaration" },
	{ "typedef int *;", "expected typedef name" },
	{ "typedef int t; typedef long t;", "redefinition of 't'" },
	{ "typedef int a[2]; typedef int a[3];", "redefinition of 'a'" },
	{ "enum c { X }; typedef int X;", "redefinition of 'X'" },

	/* specifiers */
	{ "signed unsigned int x;", "invalid type specifiers" },
	{ "long long long x;", "invalid type specifiers" },
	{ "int struct s { int a; } x;", "conflicting type specifiers" },
	{ "struct s { int a; } int x;", "conflicting type specifiers" },
	{ "int typeof(int) x;", "conflicting type specifiers" },
	{ "typeof(1 + 2) x;", "typeof expression" },
	{ "struct s { _Complex double c; };", "unsupported" },
	{ "_BitInt(8) x;", "unsupported" },
	{ "long __int128 x;", "invalid type specifiers" },
	{ "typedef float f __attribute__((mode(SF)));", "mode 'SF'" },
	{ "typedef double *d __attribute__((mode(DI)));", "non-integer" },
	{ "typedef int m __attribute__((mode));", "expected '('" },
	{ "typedef int m __attribute__((mode()));", "expected mode" },
	{ "typedef int m __attribute__((mode(DI x)));", "expected ')'" },

	/* bitfields */
	{ "struct s { int a : 33; };", "exceeds its type" },
	{ "struct s { _Bool a : 2; };", "exceeds its type" },
	{ "struct s { int a : 65; };", "invalid bitfield width" },
	{ "struct s { int a : -1; };", "invalid bitfield width" },
	{ "struct s { int a : 0; };", "zero width" },
	{ "struct s { double d : 3; };", "non-integer" },
	{ "struct s { double : 0; };", "non-integer" },
	{ "struct s { int a : 3 __attribute__((aligned(4))); };",
	  "aligned bitfield" },

	/* enums */
	{ "enum e { A }; enum e { B };", "redefinition of 'enum e'" },
	{ "enum;", "expected '{'" },
	{ "enum e { 1 };", "expected enumerator" },
	{ "enum { A = 1 }; enum { A = 2 };", "redefinition of 'A'" },
	{ "enum e { A } __attribute__((aligned(8)));", "aligned enum" },
	{ "enum e { A = -1, B = 0xffffffffffffffff };", "more than 64 bits" },
	{ "enum e; struct s { enum e x; };", "incomplete type" },

	/* attributes */
	{ "struct s { int a; } __attribute__ x;",
	  "expected '(' after attribute" },
	{ "struct s { int a; } __attribute__((packed);",
	  "unbalanced brackets" },
	{ "struct s { int a __attribute__((aligned(8 x))); };",
	  "expected ')'" },
	{ "struct s { int a __attribute__((aligned(3))); };",
	  "invalid alignment" },
	{ "struct s { int * __attribute__((aligned(8))) p; };",
	  "aligned attribute here" },
	{ "typedef int x __attribute__((aligned(8)));", "aligned typedef" },
	{ "typedef int x __attribute__((aligned(2)));", "aligned typedef" },
	{ "struct s { int a; } __attribute__((ms_struct));",
	  "unsupported attribute" },
	{ "typedef int v4 __attribute__((vector_size(16)));",
	  "unsupported attribute" },
	{ "typedef int x __attribute__((vector_size(16)));",
	  "unsupported attribute" },

	/* declarators and arrays */
	{ "struct s { int a[-1]; };", "negative array size" },
	{ "struct t; typedef struct t arr[2];", "array of incomplete type" },
	{ "struct s { char a[0x7fffffffffffffff][4]; };", "array too large" },
	{ "struct s { int a[1][1][1][1][1][1][1][1][1][1][1][1][1][1][1][1][1]; };",
	  "too many array dimensions" },

	/* expressions */
	{ "struct s { int a[N]; };", "unknown constant 'N'" },
	{ "typedef int T; enum { A = T };", "unknown constant 'T'" },
	{ "enum { A = 1/0 };", "division by zero" },
	{ "enum { A = 5 % 0 };", "division by zero" },
	{ "enum { A = (-9223372036854775807 - 1) / -1 };", "overflow" },
	{ "enum { A = 1 << 64 };", "invalid shift" },
	{ "enum { A = 1 >> -1 };", "invalid shift" },
	{ "enum { A = sizeof 1 };", "expected '('" },
	{ "enum { A = sizeof(1) };", "sizeof expression" },
	{ "struct t; enum { A = sizeof(struct t) };",
	  "sizeof incomplete type" },
	{ "enum { A = ; };", "expected expression" },
	{ "enum { A = (1 + 2 };", "expected ')'" },
};

static void test_errors(void **state)
{
	(void)state;

	for (size_t i = 0; i < ARRAY_SIZE(errors); i++)
		parse_err(errors[i].src, errors[i].msg);
}

/* constant expressions: each enumerator must have the value of the same C expression */
#define EXPRS(X)                     \
	X(1 + 2 * 3)                 \
	X((1 + 2) * 3)               \
	X(-7 / 2)                    \
	X(-7 % 3)                    \
	X(0x10 | 0x3)                \
	X(0xff & 0x0f)               \
	X(0xf0 ^ 0xff)               \
	X(~5)                        \
	X(!0)                        \
	X(!7)                        \
	X(+3)                        \
	X(- -4)                      \
	X(1 << 10)                   \
	X(-64 >> 2)                  \
	X(1 + 2 << 3)                \
	X(1 | 2 ^ 3 & 4)             \
	X(010)                       \
	X(0x7fffffffffffffffLL)      \
	X(4294967295u)               \
	X(10UL)                      \
	X('a')                       \
	X('\n')                      \
	X('\t')                      \
	X('\r')                      \
	X('\0')                      \
	X('\x41')                    \
	X('\\')                      \
	X((int)3 + (unsigned char)4) \
	X(sizeof(int) * 2)           \
	X(sizeof(struct {            \
		char c;              \
		double d;            \
	}))                          \
	X(sizeof(int[3][4]))         \
	X(sizeof(char *))            \
	X(_Alignof(double))          \
	X(__alignof__(double))       \
	X(__alignof__(long long[2]))

#define X_STR(e) #e,
#define X_VAL(e) (int64_t)(e),

/* precedence is under test here */
#pragma GCC diagnostic ignored "-Wparentheses"

static void test_exprs(void **state)
{
	(void)state;
	static const int64_t want[] = { EXPRS(X_VAL) };
	static const char *const srcs[] = { EXPRS(X_STR) };
	crefl_ctx_t *ctx = crefl_ctx_new();
	char src[512];

	for (size_t i = 0; i < ARRAY_SIZE(want); i++) {
		snprintf(src, sizeof(src), "enum e%zu { E%zu = %s };", i, i,
			 srcs[i]);
		if (crefl_parse(ctx, src))
			fail_msg("%s: %s", src, crefl_errmsg(ctx));
		const crefl_enumval_t *ev =
			crefl_enumval(crefl_type_at(ctx, i), 0);

		if (ev == NULL || ev->val != want[i])
			fail_msg("%s: got %lld, want %lld", src,
				 ev ? (long long)ev->val : -1LL,
				 (long long)want[i]);
	}
	crefl_ctx_free(ctx);
}

/* lexer and declarator forms that must parse, checked against the compiler */
static void test_syntax(void **state)
{
	(void)state;
	struct s1 {
		int a, *b, **c, d[2], *e[3], (*f)[4];
		int (*fp)(int, char *);
		void (*fa[2])(void);
		const volatile unsigned int restrict_ok;
		long unsigned int lui;
		short int si;
	};
	crefl_ctx_t *ctx = parse(
		"// line comment\n"
		"/* block\n comment */\n"
		"#define X \\\n  continued\n"
		"struct s1 {\n"
		"	int a, *b, **c, d[2], *e[3], (*f)[4];\n"
		"	int (*fp)(int, char *);\n"
		"	void (*fa[2])(void);\n"
		"	const volatile unsigned int restrict_ok;  // trailing\n"
		"	long unsigned int lui;\n"
		"	short int si;\n"
		"};\n"
		"int x asm(\"x\\\"y\");\n"
		"enum { SHR = 256 >> 4, AL = __alignof__(struct s1) };\n"
		";;\n"
		"int * const volatile cvp, arr[2] = { 1, 2 }, y = 3 + 4;\n"
		"typedef int *ip; typedef int *ip;\n"
		"typedef int ia[2]; typedef int ia[2];\n"
		"typedef typeof(int) tint; typedef __typeof__(nullptr) np;\n"
		"struct s2 { _Static_assert(1, \"x\"); ; int a; __attribute__((aligned)) char b; };\n");
	const crefl_type_t *t = crefl_type(ctx, "struct s1");

	assert_int_equal(crefl_sizeof(t), sizeof(struct s1));
	assert_int_equal(crefl_member(t, 5)->off, offsetof(struct s1, f));
	assert_int_equal(crefl_len(crefl_elem(crefl_member(t, 5)->type)), 4);
	assert_int_equal(crefl_member(t, 7)->off, offsetof(struct s1, fa));
	assert_int_equal(crefl_kind(crefl_member(t, 6)->type), CREFL_PTR);
	assert_int_equal(crefl_prim(crefl_resolve(crefl_type(ctx, "tint"))),
			 CREFL_INT);
	assert_int_equal(crefl_kind(crefl_resolve(crefl_type(ctx, "np"))),
			 CREFL_PTR);
	assert_int_equal(crefl_member(crefl_type(ctx, "struct s2"), 1)->off,
			 __BIGGEST_ALIGNMENT__);
	crefl_ctx_free(ctx);
}

/* remaining API edge cases */
static void test_api_edges(void **state)
{
	(void)state;
	crefl_ctx_t *ctx = parse(
		"typedef int t; struct fwd; struct s { int a; char flex[]; } __attribute__((packed));"
		"struct u { unsigned long long u; float f; double d[2]; int *p; char c[4]; };");
	const crefl_type_t *s = crefl_type(ctx, "struct s"),
			   *u = crefl_type(ctx, "struct u");
	crefl_path_t p;
	uint64_t x;
	double d;
	char *out = NULL;
	size_t len = 0;
	FILE *f;

	assert_int_equal(crefl_define(ctx, "t", 1), -EINVAL);
	assert_int_equal(crefl_define(ctx, "N", 1), 0);
	assert_int_equal(crefl_define(ctx, "N", 2), 0);
	crefl_ctx_free(NULL);

	assert_true(crefl_is_packed(s));
	assert_false(crefl_is_packed(u));
	assert_false(crefl_is_complete(crefl_type(ctx, "struct fwd")));
	assert_null(crefl_member(crefl_type(ctx, "int"), 0));
	assert_null(crefl_enumval(s, 0));
	assert_int_equal(crefl_len(crefl_member(s, 1)->type), 0);
	assert_null(crefl_new(crefl_type(ctx, "struct fwd")).p);
	assert_null(crefl_wrap(NULL, &x).t);

	/* paths */
	assert_int_equal(crefl_path(NULL, "a", &p), -EINVAL);
	assert_int_equal(crefl_path(s, NULL, &p), -EINVAL);
	assert_int_equal(crefl_path(s, "", &p), 0);
	assert_int_equal(crefl_path(s, "a.", &p), -EINVAL);
	assert_int_equal(crefl_path(s, "aa", &p), -ENOENT);
	/* flexible: unchecked */
	assert_int_equal(crefl_path(s, "flex[1000]", &p), 0);
	assert_int_equal(crefl_path(s, "[0]", &p), -EINVAL);
	assert_int_equal(crefl_path(s, "a[0]", &p), -EINVAL);
	assert_int_equal(crefl_path(s, "a]", &p), -ENOENT);
	assert_int_equal(crefl_path(u, "c[0]x", &p), -EINVAL);
	assert_null(crefl_type(ctx, "int )"));
	assert_false(crefl_is_signed(crefl_type(ctx, "int *")));

	crefl_val_t v = crefl_new(u);

	assert_null(crefl_index(crefl_get(v, "u"), 0).t);
	assert_null(crefl_deref(crefl_get(v, "u")).t);
	assert_null(crefl_index(crefl_wrap(NULL, NULL), 0).t);

	/* conversions */
	assert_int_equal(crefl_set_f64(crefl_get(v, "u"), 0x1p63), 0);
	assert_int_equal(crefl_get_u64(crefl_get(v, "u"), &x), 0);
	assert_true(x == UINT64_C(1) << 63);
	assert_int_equal(crefl_get_i64(crefl_get(v, "u"), (int64_t *)&x),
			 -ERANGE);
	assert_int_equal(crefl_set_f64(crefl_get(v, "u"), 0x1p64), -ERANGE);
	assert_int_equal(crefl_set_f64(crefl_get(v, "u"), 0.0 / 0.0), -ERANGE);
	assert_int_equal(crefl_set_u64(crefl_get(v, "f"), 3), 0);
	assert_int_equal(crefl_get_f64(crefl_get(v, "f"), &d), 0);
	assert_true(d == 3.0);
	assert_int_equal(crefl_get_u64(crefl_get(v, "f"), &x), -EINVAL);
	assert_int_equal(crefl_set_u64(crefl_get(v, "p"), 0x1000), 0);
	assert_int_equal(crefl_get_u64(crefl_get(v, "p"), &x), 0);
	assert_true(x == 0x1000);
	assert_int_equal(crefl_set_i64(crefl_get(v, "p"), 1), -EINVAL);
	assert_int_equal(crefl_get_f64(crefl_get(v, "p"), &d), -EINVAL);
	assert_int_equal(crefl_set_str(crefl_get(v, "c"), NULL), -EINVAL);
	assert_int_equal(crefl_set_str(crefl_get(v, "d"), "x"), -EINVAL);
	assert_int_equal(crefl_copy(crefl_get(v, "u"), crefl_wrap(NULL, NULL)),
			 -EINVAL);
	assert_int_equal(crefl_get_i64(crefl_wrap(NULL, NULL), (int64_t *)&x),
			 -EINVAL);
	assert_int_equal(crefl_get_u64(crefl_wrap(NULL, NULL), &x), -EINVAL);
	assert_int_equal(crefl_get_f64(crefl_wrap(NULL, NULL), &d), -EINVAL);
	assert_int_equal(crefl_set_u64(crefl_wrap(NULL, NULL), 1), -EINVAL);
	assert_int_equal(crefl_set_f64(crefl_wrap(NULL, NULL), 1), -EINVAL);
	crefl_free(v);

#ifdef __SIZEOF_FLOAT128__
	crefl_ctx_t *q = parse("struct q { __float128 x; };");
	crefl_val_t qv = crefl_new(crefl_type(q, "struct q"));

	assert_int_equal(crefl_set_f64(crefl_get(qv, "x"), 2.5), 0);
	assert_int_equal(crefl_get_f64(crefl_get(qv, "x"), &d), 0);
	assert_true(d == 2.5);
	crefl_free(qv);
	crefl_ctx_free(q);
#endif

#ifdef __SIZEOF_INT128__
	/* 128 bit integers: full layout, access within 64 bits */
	crefl_ctx_t *w = parse("struct w { char c; __int128 i; "
			       "unsigned __int128 u; __uint128_t t; };");
	const crefl_type_t *wt = crefl_type(w, "struct w");
	crefl_val_t wv = crefl_new(wt);
	struct w {
		char c;
		__int128 i;
		unsigned __int128 u;
		__uint128_t t;
	} *wp = wv.p;
	int64_t i64;

	assert_int_equal(crefl_sizeof(wt), sizeof(struct w));
	assert_int_equal(crefl_alignof(wt), _Alignof(struct w));
	assert_int_equal(crefl_set_i64(crefl_get(wv, "i"), INT64_MIN), 0);
	assert_true(wp->i == INT64_MIN);
	assert_int_equal(crefl_get_i64(crefl_get(wv, "i"), &i64), 0);
	assert_true(i64 == INT64_MIN);
	assert_int_equal(crefl_set_i64(crefl_get(wv, "u"), -1), -ERANGE);
	assert_int_equal(crefl_set_u64(crefl_get(wv, "u"), UINT64_MAX), 0);
	assert_true(wp->u == UINT64_MAX);
	wp->i = (__int128)1 << 64;
	assert_int_equal(crefl_get_i64(crefl_get(wv, "i"), &i64), -ERANGE);
	assert_int_equal(crefl_get_f64(crefl_get(wv, "i"), &d), 0);
	assert_true(d == 18446744073709551616.0);
	wp->i = -((__int128)1 << 64);
	assert_int_equal(crefl_get_u64(crefl_get(wv, "i"), &x), -ERANGE);
	wp->t = (unsigned __int128)1 << 64;
	assert_int_equal(crefl_get_u64(crefl_get(wv, "t"), &x), -ERANGE);
	crefl_free(wv);
	crefl_ctx_free(w);
#endif

	/* va_list: opaque, but laid out like the compiler */
	crefl_ctx_t *va = parse("typedef __builtin_va_list my_va; "
				"struct va { char c; my_va ap; }; "
				"typedef int al __attribute__((aligned(4)));");
	struct va {
		char c;
		va_list ap;
	};

	assert_int_equal(crefl_sizeof(crefl_type(va, "struct va")),
			 sizeof(struct va));
	assert_int_equal(crefl_alignof(crefl_type(va, "struct va")),
			 _Alignof(struct va));
	assert_int_equal(crefl_sizeof(crefl_type(va, "my_va")),
			 sizeof(va_list));
	crefl_ctx_free(va);

	/* printing of incomplete, derived and flexible types */
	f = open_memstream(&out, &len);
	assert_int_equal(crefl_print(NULL, f), -EINVAL);
	crefl_print(crefl_type(ctx, "struct fwd"), f);
	crefl_print(crefl_type(ctx, "int (*)[3]"), f);
	crefl_print(s, f);
	fclose(f);
	assert_non_null(strstr(out, "struct fwd;\n"));
	assert_non_null(strstr(out, "int (*)[3];\n"));
	assert_non_null(strstr(out, "char flex[];"));
	free(out);

	assert_null(crefl_type(ctx, NULL));
	assert_int_equal(crefl_parse(NULL, "int x;"), -EINVAL);
	assert_int_equal(crefl_parse(ctx, NULL), -EINVAL);
	crefl_ctx_free(ctx);
}

static void test_depth_limit(void **state)
{
	(void)state;
	char src[4096];
	size_t n = 0;

	n += snprintf(src + n, sizeof(src) - n, "enum { A = ");
	for (int i = 0; i < 500; i++)
		src[n++] = '(';
	src[n++] = '1';
	for (int i = 0; i < 500; i++)
		src[n++] = ')';
	snprintf(src + n, sizeof(src) - n, " };");
	parse_err(src, "nesting too deep");
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_type_names),
		cmocka_unit_test(test_introspection),
		cmocka_unit_test(test_define),
		cmocka_unit_test(test_access),
		cmocka_unit_test(test_new_copy),
		cmocka_unit_test(test_static_ctx),
		cmocka_unit_test(test_oom_sweep),
		cmocka_unit_test(test_print_roundtrip),
		cmocka_unit_test(test_print_holes),
		cmocka_unit_test(test_skipped_decls),
		cmocka_unit_test(test_errors),
		cmocka_unit_test(test_exprs),
		cmocka_unit_test(test_syntax),
		cmocka_unit_test(test_api_edges),
		cmocka_unit_test(test_rollback),
		cmocka_unit_test(test_review_fixes),
		cmocka_unit_test(test_depth_limit),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
