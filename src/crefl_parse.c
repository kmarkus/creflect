// SPDX-License-Identifier: LGPL-2.0-or-later
/*
 * crefl: parser for C type declarations
 *
 * Hand-written recursive descent over a lazily lexed string. Errors
 * longjmp back to crefl_parse(). Memory allocated before an error stays
 * in the context arena.
 *
 * Copyright (C) 2019-2026 Markus Klotzbuecher <mk@mkio.de>
 */

#include <ctype.h>
#include <limits.h>
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "crefl_int.h"

#define MAX_DEPTH 64
#define MAX_DIMS 16

enum { T_EOF = 256, T_IDENT, T_NUM, T_PPNUM, T_STR, T_SHL, T_SHR };

/*
 * value of an integer constant expression with its C type: rank 0 int,
 * 1 long, 2 long long, u unsigned. v holds the value as converted to
 * that type, i.e. 32 bit types are sign or zero extended.
 */
struct cval {
	int64_t v;
	uint8_t rank;
	uint8_t u;
};

struct lex {
	const char *s;
	int line;
	int tok;
	const char *tp;
	size_t tl;
	uint64_t num;
	struct cval numval; /* T_NUM as typed constant */
};

struct parser {
	crefl_ctx_t *ctx;
	struct lex l;
	int depth;
	int extern_c; /* open extern "C" { blocks */
	jmp_buf jb;
};

struct mlist {
	crefl_member_t *a;
	struct crefl_attr *attr;
	size_t n;
	size_t cap;
};

static const char *const ignored_kws[] = {
	"const",	 "volatile",	  "restrict",	"__restrict",
	"__restrict__",	 "__const",	  "__const__",	"__volatile",
	"__volatile__",	 "_Atomic",	  "static",	"extern",
	"inline",	 "__inline",	  "__inline__", "register",
	"auto",		 "_Thread_local", "__thread",	"thread_local",
	"__extension__", "_Noreturn",	  NULL,
};

static const char *const unsupported_kws[] = {
	"_Complex",   "__complex__", "_Alignas", "alignas",  "__auto_type",
	"_BitInt",    "_Float16",    "_Float32", "_Float64", "_Decimal32",
	"_Decimal64", "_Decimal128", NULL,
};

static const char *const prim_kws[] = {
	"void",	      "_Bool",	   "char",	 "short",    "int",   "long",
	"signed",     "__signed",  "__signed__", "unsigned", "float", "double",
	"__float128", "_Float128", "__int128",	 NULL,
};

static const crefl_type_t *specifiers(struct parser *p, int *is_typedef,
				      struct crefl_attr *a);
static const crefl_type_t *declarator(struct parser *p,
				      const crefl_type_t *base,
				      const char **name, size_t *nl);
static int64_t expr(struct parser *p);
static struct cval cexpr(struct parser *p);

static _Noreturn void fail(struct parser *p, int ret, const char *fmt, ...)
	__printf(3, 4);

static _Noreturn void fail(struct parser *p, int ret, const char *fmt, ...)
{
	char msg[128];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	crefl__err(p->ctx, ret, "line %d: %s", p->l.line, msg);
	p->ctx->errcode = ret;
	longjmp(p->jb, 1);
}

static char *strndup_(struct parser *p, const char *s, size_t len)
{
	char *d = crefl__strndup(p->ctx, s, len);

	if (d == NULL)
		fail(p, -ENOMEM, "out of memory");
	return d;
}

static void enter(struct parser *p)
{
	if (++p->depth > MAX_DEPTH)
		fail(p, -EINVAL, "nesting too deep");
}

/*
 * lexer
 */
static void skip_directive(struct parser *p, const char *s)
{
	while (*s == ' ' || *s == '\t')
		s++;

	if (strncmp(s, "pragma", 6) == 0) {
		const char *q = s + 6;

		while (*q == ' ' || *q == '\t')
			q++;
		if (strncmp(q, "pack", 4) == 0)
			fail(p, -EINVAL, "unsupported: #pragma pack");
	}

	while (*s && *s != '\n') {
		if (s[0] == '\\' && s[1] == '\n') {
			p->l.line++;
			s++;
		}
		s++;
	}
	p->l.s = s;
}

/* character literal after the quote, value as char of the host */
static int char_lit(struct parser *p, const char **sp)
{
	static const char esc[] = "a\ab\bf\fn\nr\rt\tv\v\\\\''\"\"??";
	const char *s = *sp, *e;
	unsigned int c = 0;

	if (*s == '\0' || *s == '\n') {
		fail(p, -EINVAL, "unterminated character literal");
	} else if (*s != '\\') {
		c = (unsigned char)*s++;
	} else if (s[1] >= '0' && s[1] <= '7') {
		for (s++, e = s + 3; s < e && *s >= '0' && *s <= '7'; s++)
			c = c * 8 + (*s - '0');
	} else if (s[1] == 'x') {
		for (s += 2; isxdigit((unsigned char)*s); s++)
			c = c * 16 +
			    (isdigit((unsigned char)*s) ?
				     *s - '0' :
				     (tolower((unsigned char)*s) - 'a' + 10));
	} else if (s[1] == '\0') {
		fail(p, -EINVAL, "unterminated character literal");
	} else {
		for (e = esc; *e && *e != s[1]; e += 2)
			;
		if (*e == '\0')
			fail(p, -EINVAL, "unsupported character literal");
		c = (unsigned char)e[1];
		s += 2;
	}

	if (*s != '\'' || c > 0xff)
		fail(p, -EINVAL, "unsupported character literal");
	*sp = s + 1;
	return (CHAR_MIN < 0) ? (signed char)c : (int)c;
}

static unsigned int rank_bits(int rank)
{
	return rank == 0 ? 32 : rank == 1 ? sizeof(long) * 8 : 64;
}

/* convert v to the type of c (rank, u): wrap and extend */
static struct cval norm(struct cval c)
{
	if (rank_bits(c.rank) == 32)
		c.v = c.u ? (int64_t)(uint32_t)c.v :
			    (int64_t)(int32_t)(uint32_t)c.v;
	return c;
}

/* type of an integer literal, C11 6.4.4.1 */
static struct cval lit_type(uint64_t n, int dec, int su, int nl)
{
	struct cval c = { (int64_t)n, 2, 1 };

	for (int r = nl; r <= 2; r++) {
		unsigned int w = rank_bits(r);
		uint64_t smax = (w == 64) ? INT64_MAX : (1ULL << (w - 1)) - 1;
		uint64_t umax = (w == 64) ? UINT64_MAX : (1ULL << w) - 1;

		if (!su && n <= smax) {
			c.rank = r;
			c.u = 0;
			return c;
		}
		if ((su || !dec) && n <= umax) {
			c.rank = r;
			c.u = 1;
			return c;
		}
	}
	/* too large for long long: unsigned long long, like gcc */
	return c;
}

static void next(struct parser *p)
{
	struct lex *l = &p->l;
	const char *s = l->s;

	for (;;) {
		if (*s == '\n') {
			l->line++;
			s++;
		} else if (isspace((unsigned char)*s)) {
			s++;
		} else if (s[0] == '/' && s[1] == '/') {
			while (*s && *s != '\n')
				s++;
		} else if (s[0] == '/' && s[1] == '*') {
			for (s += 2; *s && !(s[0] == '*' && s[1] == '/'); s++) {
				if (*s == '\n')
					l->line++;
			}
			if (*s == '\0')
				fail(p, -EINVAL, "unterminated comment");
			s += 2;
		} else if (*s == '#') {
			skip_directive(p, s + 1);
			s = l->s;
		} else {
			break;
		}
	}

	l->tp = s;

	if (*s == '\0') {
		l->tok = T_EOF;
	} else if (isalpha((unsigned char)*s) || *s == '_') {
		while (isalnum((unsigned char)*s) || *s == '_')
			s++;
		l->tok = T_IDENT;
	} else if (isdigit((unsigned char)*s) ||
		   (*s == '.' && isdigit((unsigned char)s[1]))) {
		/* pp-number: integers are T_NUM, others (floats) T_PPNUM */
		const char *e = s, *q;
		char *end;

		while (isalnum((unsigned char)*e) || *e == '_' || *e == '.' ||
		       ((*e == '+' || *e == '-') && strchr("eEpP", e[-1])))
			e++;

		int su = 0, nl = 0;

		errno = 0;
		l->num = strtoull(s, &end, 0);
		l->tok = T_PPNUM;
		for (q = end; q < e && strchr("uUlL", *q); q++) {
			if (*q == 'u' || *q == 'U')
				su++;
			else
				nl++;
		}
		if (*s != '.' && errno == 0 && q == e && su <= 1 && nl <= 2) {
			l->tok = T_NUM;
			l->numval = lit_type(l->num, *s != '0', su, nl);
		}
		s = e;
	} else if (*s == '\'') {
		s++;
		l->numval.v = char_lit(p, &s);
		l->numval.rank = 0;
		l->numval.u = 0;
		l->tok = T_NUM;
	} else if (*s == '"') {
		for (s++; *s != '"'; s++) {
			if (*s == '\0' || *s == '\n')
				fail(p, -EINVAL, "unterminated string");
			if (s[0] == '\\' && s[1])
				s++;
		}
		s++;
		l->tok = T_STR;
	} else if (s[0] == '<' && s[1] == '<') {
		s += 2;
		l->tok = T_SHL;
	} else if (s[0] == '>' && s[1] == '>') {
		s += 2;
		l->tok = T_SHR;
	} else {
		l->tok = (unsigned char)*s++;
	}

	l->tl = s - l->tp;
	l->s = s;
}

/* token after the current one */
static int peek(struct parser *p)
{
	struct lex save = p->l;
	int tok;

	next(p);
	tok = p->l.tok;
	p->l = save;
	return tok;
}

static int is(struct parser *p, int tok)
{
	return p->l.tok == tok;
}

static int is_kw(struct parser *p, const char *kw)
{
	return p->l.tok == T_IDENT && strlen(kw) == p->l.tl &&
	       memcmp(p->l.tp, kw, p->l.tl) == 0;
}

static int is_kw_in(struct parser *p, const char *const *kws)
{
	for (; *kws; kws++) {
		if (is_kw(p, *kws))
			return 1;
	}
	return 0;
}

static int accept(struct parser *p, int tok)
{
	if (!is(p, tok))
		return 0;
	next(p);
	return 1;
}

static void expect(struct parser *p, int tok)
{
	if (!accept(p, tok)) {
		if (is(p, T_EOF))
			fail(p, -EINVAL, "expected '%c' before end of input",
			     tok);
		fail(p, -EINVAL, "expected '%c' before '%.*s'", tok,
		     (int)p->l.tl, p->l.tp);
	}
}

/* skip from an opening (, [ or { to after its match */
static void skip_balanced(struct parser *p)
{
	int depth = 0;

	do {
		switch (p->l.tok) {
		case '(':
		case '[':
		case '{':
			depth++;
			break;
		case ')':
		case ']':
		case '}':
			depth--;
			break;
		case T_EOF:
			fail(p, -EINVAL, "unbalanced brackets");
		}
		next(p);
	} while (depth > 0);
}

static int is_attr(struct parser *p)
{
	return is_kw(p, "__attribute__") || is_kw(p, "__attribute") ||
	       is_kw(p, "__asm__") || is_kw(p, "__asm") || is_kw(p, "asm");
}

#ifdef __BIGGEST_ALIGNMENT__
#define BIGGEST_ALIGNMENT __BIGGEST_ALIGNMENT__
#else
#define BIGGEST_ALIGNMENT _Alignof(max_align_t)
#endif

/* integer modes in bytes, 0 if unsupported (floats, vectors) */
static unsigned int mode_size(const char *s, size_t len)
{
	static const struct {
		const char *name;
		unsigned int size;
	} modes[] = {
		{ "QI", 1 },
		{ "HI", 2 },
		{ "SI", 4 },
		{ "DI", 8 },
		{ "TI", 16 },
		{ "byte", 1 },
		{ "word", sizeof(long) },
		{ "pointer", sizeof(void *) },
	};

	/* __word__ is word */
	if (len > 4 && strncmp(s, "__", 2) == 0 &&
	    strncmp(s + len - 2, "__", 2) == 0) {
		s += 2;
		len -= 4;
	}
	for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		if (strlen(modes[i].name) == len &&
		    strncmp(modes[i].name, s, len) == 0)
			return modes[i].size;
	}
	return 0;
}

/*
 * gcc attributes, packed, aligned(n) and mode(m) are collected into
 * @a. If @a is NULL they are ignored where harmless (packed) and
 * rejected where they would change the layout (aligned, mode).
 */
static void skip_attrs(struct parser *p, struct crefl_attr *a)
{
	while (is_attr(p)) {
		int depth = 0;

		next(p);
		if (!is(p, '('))
			fail(p, -EINVAL, "expected '(' after attribute");

		for (;;) {
			if (is(p, '(')) {
				depth++;
			} else if (is(p, ')')) {
				if (--depth == 0) {
					next(p);
					break;
				}
			} else if (is(p, T_EOF)) {
				fail(p, -EINVAL, "unbalanced brackets");
			} else if (is_kw(p, "packed") ||
				   is_kw(p, "__packed__")) {
				if (a)
					a->packed = 1;
			} else if (is_kw(p, "aligned") ||
				   is_kw(p, "__aligned__")) {
				int64_t x = BIGGEST_ALIGNMENT;
				int paren;

				next(p);
				paren = is(p, '(');
				if (paren) {
					next(p);
					x = expr(p);
					if (!is(p, ')'))
						fail(p, -EINVAL,
						     "expected ')'");
				}
				if (x <= 0 || (x & (x - 1)))
					fail(p, -EINVAL, "invalid alignment");
				if (a == NULL)
					fail(p, -EINVAL,
					     "unsupported: aligned attribute here");
				if ((size_t)x > a->align)
					a->align = (size_t)x;
				if (!paren)
					continue;
			} else if (is_kw(p, "mode") || is_kw(p, "__mode__")) {
				next(p);
				expect(p, '(');
				if (!is(p, T_IDENT))
					fail(p, -EINVAL, "expected mode");
				if (a == NULL)
					fail(p, -EINVAL,
					     "unsupported: mode attribute here");
				a->mode = mode_size(p->l.tp, p->l.tl);
				if (a->mode == 0)
					fail(p, -EINVAL,
					     "unsupported: mode '%.*s'",
					     (int)p->l.tl, p->l.tp);
				next(p);
				if (!is(p, ')'))
					fail(p, -EINVAL, "expected ')'");
			} else if (is_kw(p, "ms_struct") ||
				   is_kw(p, "__ms_struct__") ||
				   is_kw(p, "vector_size") ||
				   is_kw(p, "__vector_size__")) {
				fail(p, -EINVAL, "unsupported attribute '%.*s'",
				     (int)p->l.tl, p->l.tp);
			}
			next(p);
		}
	}
}

static void skip_quals(struct parser *p)
{
	for (;;) {
		if (is_kw_in(p, ignored_kws))
			next(p);
		else if (is_attr(p))
			skip_attrs(p, NULL);
		else
			return;
	}
}

static void skip_static_assert(struct parser *p)
{
	next(p);
	if (!is(p, '('))
		fail(p, -EINVAL, "expected '('");
	skip_balanced(p);
	expect(p, ';');
}

/*
 * symbols
 */
static const crefl_type_t *lookup_typedef(struct parser *p, const char *name,
					  size_t len)
{
	struct sym *s = crefl__sym_find(p->ctx, NS_ORD, name, len);

	if (s)
		return s->t;
	return crefl__builtin_typedef(name, len);
}

static int is_type_start(struct parser *p)
{
	if (!is(p, T_IDENT))
		return 0;
	return is_kw_in(p, prim_kws) || is_kw_in(p, ignored_kws) ||
	       is_kw(p, "struct") || is_kw(p, "union") || is_kw(p, "enum") ||
	       lookup_typedef(p, p->l.tp, p->l.tl) != NULL;
}

static void add_typedef(struct parser *p, const char *name, size_t nl,
			const crefl_type_t *t)
{
	struct sym *s = crefl__sym_find(p->ctx, NS_ORD, name, nl);
	crefl_type_t *td;

	if (s) {
		if (s->t && crefl__same_type(s->t->elem, t))
			return;
		fail(p, -EINVAL, "redefinition of '%.*s'", (int)nl, name);
	}

	td = crefl__type_new(p->ctx, CREFL_TYPEDEF, NULL);
	s = crefl__sym_add(p->ctx, NS_ORD, name, nl);
	if (td == NULL || s == NULL)
		fail(p, -ENOMEM, "out of memory");

	td->name = s->name;
	td->elem = t;
	td->flags = F_COMPLETE;
	s->t = td;
	if (crefl__types_push(p->ctx, td))
		fail(p, -ENOMEM, "out of memory");
}

/* the integer type of the size of @a's mode with @t's signedness */
static const crefl_type_t *apply_mode(struct parser *p, const crefl_type_t *t,
				      const struct crefl_attr *a)
{
	const crefl_type_t *rt = crefl_resolve(t), *m;
	int sgn = crefl_is_signed(rt);
	enum crefl_prim prim;

	if (a->mode == 0)
		return t;
	if (rt->kind != CREFL_PRIM || rt->prim < CREFL_CHAR ||
	    rt->prim > CREFL_UINT128)
		fail(p, -EINVAL, "unsupported: mode of a non-integer type");

	switch (a->mode) {
	case 1:
		prim = sgn ? CREFL_SCHAR : CREFL_UCHAR;
		break;
	case 2:
		prim = sgn ? CREFL_SHORT : CREFL_USHORT;
		break;
	case 4:
		prim = sgn ? CREFL_INT : CREFL_UINT;
		break;
	case 8:
		if (sizeof(long) == 8)
			prim = sgn ? CREFL_LONG : CREFL_ULONG;
		else
			prim = sgn ? CREFL_LLONG : CREFL_ULLONG;
		break;
	default:
		prim = sgn ? CREFL_INT128 : CREFL_UINT128;
		break;
	}

	m = &crefl_prims[prim];
	if (!crefl_is_complete(m))
		fail(p, -EINVAL, "unsupported: mode needs __int128");
	return m;
}

/* find or create a tag, checks that the kind matches */
static crefl_type_t *tag_get(struct parser *p, int kind, const char *tag,
			     size_t tl)
{
	struct sym *s = crefl__sym_find(p->ctx, NS_TAG, tag, tl);

	if (s) {
		if (s->t->kind != kind)
			fail(p, -EINVAL, "'%.*s' defined as wrong kind of tag",
			     (int)tl, tag);
		return s->t;
	}

	s = crefl__sym_add(p->ctx, NS_TAG, tag, tl);
	if (s == NULL)
		fail(p, -ENOMEM, "out of memory");
	s->t = crefl__type_new(p->ctx, kind, s->name);
	if (s->t == NULL)
		fail(p, -ENOMEM, "out of memory");
	return s->t;
}

/*
 * struct, union, enum
 */

/* remember a type whose definition starts, reset if the parse fails */
static void undo_push(struct parser *p, crefl_type_t *t)
{
	crefl_ctx_t *ctx = p->ctx;

	if (ctx->nundo == ctx->capundo) {
		size_t cap = ctx->capundo ? ctx->capundo * 2 : 16;
		crefl_type_t **n = crefl__grow(ctx, ctx->undo,
					       ctx->nundo * sizeof(*n),
					       cap * sizeof(*n));

		if (n == NULL)
			fail(p, -ENOMEM, "out of memory");
		ctx->undo = n;
		ctx->capundo = cap;
	}
	ctx->undo[ctx->nundo++] = t;
}
static void madd(struct parser *p, struct mlist *ml, const char *name,
		 size_t nl, const crefl_type_t *t, unsigned int width,
		 const struct crefl_attr *attr)
{
	if (ml->n == ml->cap) {
		size_t cap = ml->cap ? ml->cap * 2 : 8;

		ml->a = crefl__grow(p->ctx, ml->a, ml->n * sizeof(*ml->a),
				    cap * sizeof(*ml->a));
		ml->attr = crefl__grow(p->ctx, ml->attr,
				       ml->n * sizeof(*ml->attr),
				       cap * sizeof(*ml->attr));
		if (ml->a == NULL || ml->attr == NULL)
			fail(p, -ENOMEM, "out of memory");
		ml->cap = cap;
	}

	crefl_member_t *m = &ml->a[ml->n];

	m->name = name ? strndup_(p, name, nl) : NULL;
	m->type = t;
	m->bit_width = width;
	ml->attr[ml->n++] = *attr;
}

static void struct_body(struct parser *p, struct mlist *ml)
{
	while (!accept(p, '}')) {
		const crefl_type_t *base;
		struct crefl_attr spec_attr = { 0 };

		if (is(p, T_EOF))
			fail(p, -EINVAL, "expected '}' before end of input");
		if (accept(p, ';'))
			continue;
		if (is_kw(p, "_Static_assert") || is_kw(p, "static_assert")) {
			skip_static_assert(p);
			continue;
		}

		base = specifiers(p, NULL, &spec_attr);
		if (base == NULL)
			fail(p, -EINVAL, "unknown type name '%.*s'",
			     (int)p->l.tl, p->l.tp);

		if (accept(p, ';')) {
			/* anonymous struct/union member, else ignored (e.g. a nested tagged definition) */
			if ((base->kind == CREFL_STRUCT ||
			     base->kind == CREFL_UNION) &&
			    base->name == NULL) {
				struct crefl_attr none = { 0 };

				madd(p, ml, NULL, 0, base, 0, &none);
			}
			continue;
		}

		do {
			const char *name = NULL;
			size_t nl = 0;
			const crefl_type_t *t = base;
			int64_t width = 0;
			struct crefl_attr ma = spec_attr;

			skip_attrs(p, &ma);
			if (!is(p, ':'))
				t = declarator(p, base, &name, &nl);
			skip_attrs(p, &ma);

			if (accept(p, ':')) {
				width = expr(p);
				if (width < 0 || width > 64)
					fail(p, -EINVAL,
					     "invalid bitfield width");
				if (width == 0 && name)
					fail(p, -EINVAL,
					     "zero width for bitfield '%.*s'",
					     (int)nl, name);
				skip_attrs(p, &ma);
				if (ma.align)
					fail(p, -EINVAL,
					     "unsupported: aligned bitfield");
			} else if (name == NULL) {
				fail(p, -EINVAL, "expected member name");
			}

			t = apply_mode(p, t, &ma);
			madd(p, ml, name, nl, t, (unsigned int)width, &ma);
		} while (accept(p, ','));

		expect(p, ';');
	}
}

static const crefl_type_t *struct_spec(struct parser *p)
{
	int kind = is_kw(p, "struct") ? CREFL_STRUCT : CREFL_UNION;
	const char *tag = NULL;
	size_t tl = 0;
	struct mlist ml = { 0 };
	struct crefl_attr sa = { 0 };
	crefl_type_t *t;
	int ret;

	enter(p);
	next(p);
	skip_attrs(p, &sa);

	if (is(p, T_IDENT)) {
		tag = p->l.tp;
		tl = p->l.tl;
		next(p);
		skip_attrs(p, &sa);
	}

	if (tag) {
		t = tag_get(p, kind, tag, tl);
		if (!is(p, '{')) {
			p->depth--;
			return t;
		}
		if (t->flags & (F_COMPLETE | F_DEFINING))
			fail(p, -EINVAL, "redefinition of '%.*s'", (int)tl,
			     tag);
	} else {
		if (!is(p, '{'))
			fail(p, -EINVAL, "expected '{'");
		t = crefl__type_new(p->ctx, kind, NULL);
		if (t == NULL)
			fail(p, -ENOMEM, "out of memory");
	}

	next(p);
	undo_push(p, t);
	t->flags |= F_DEFINING;
	struct_body(p, &ml);
	skip_attrs(p, &sa);

	t->flags &= ~F_DEFINING;
	t->members = ml.a;
	t->len = ml.n;

	ret = crefl__layout(p->ctx, t, &sa, ml.attr);
	if (ret)
		fail(p, ret, "%s", p->ctx->err);
	if (tag && crefl__types_push(p->ctx, t))
		fail(p, -ENOMEM, "out of memory");

	p->depth--;
	return t;
}

static const crefl_type_t *enum_spec(struct parser *p)
{
	const char *tag = NULL;
	size_t tl = 0, n = 0, cap = 0;
	crefl_enumval_t *vals = NULL;
	struct cval v = { 0, 0, 0 };
	int64_t min = 0, max = 0;
	int big = 0, nsigned = 0;
	crefl_type_t *t;
	struct crefl_attr ea = { 0 };
	enum crefl_prim prim;
	int packed;

	next(p);
	skip_attrs(p, &ea);

	if (is(p, T_IDENT)) {
		tag = p->l.tp;
		tl = p->l.tl;
		next(p);
		skip_attrs(p, &ea);
	}

	if (tag) {
		t = tag_get(p, CREFL_ENUM, tag, tl);
		if (!is(p, '{'))
			return t;
		if (t->flags & F_COMPLETE)
			fail(p, -EINVAL, "redefinition of 'enum %.*s'", (int)tl,
			     tag);
	} else {
		if (!is(p, '{'))
			fail(p, -EINVAL, "expected '{'");
		t = crefl__type_new(p->ctx, CREFL_ENUM, NULL);
		if (t == NULL)
			fail(p, -ENOMEM, "out of memory");
	}

	undo_push(p, t);
	next(p);
	while (!accept(p, '}')) {
		const char *name = p->l.tp;
		size_t nl = p->l.tl;
		struct sym *s;

		if (!is(p, T_IDENT))
			fail(p, -EINVAL, "expected enumerator");
		next(p);
		skip_attrs(p, NULL);
		if (accept(p, '='))
			v = cexpr(p);

		s = crefl__sym_find(p->ctx, NS_ORD, name, nl);
		if (s && (s->t || s->val != v.v))
			fail(p, -EINVAL, "redefinition of '%.*s'", (int)nl,
			     name);
		if (s == NULL)
			s = crefl__sym_add(p->ctx, NS_ORD, name, nl);
		if (s == NULL)
			fail(p, -ENOMEM, "out of memory");
		s->val = v.v;
		s->val_unsigned = v.u && v.v < 0;

		if (n == cap) {
			cap = cap ? cap * 2 : 8;
			vals = crefl__grow(p->ctx, vals, n * sizeof(*vals),
					   cap * sizeof(*vals));
			if (vals == NULL)
				fail(p, -ENOMEM, "out of memory");
		}
		vals[n].name = s->name;
		vals[n].val = v.v;
		if (v.u && v.v < 0) {
			big = 1; /* above INT64_MAX */
		} else {
			min = (nsigned == 0 || v.v < min) ? v.v : min;
			max = (nsigned == 0 || v.v > max) ? v.v : max;
			nsigned++;
		}
		n++;
		v.v = (int64_t)((uint64_t)v.v + 1);

		if (!accept(p, ',')) {
			expect(p, '}');
			break;
		}
	}
	skip_attrs(p, &ea);
	if (ea.align)
		fail(p, -EINVAL, "unsupported: aligned enum");
	packed = ea.packed;

	/*
	 * gcc: unsigned int unless negative, 64 bit if needed, packed:
	 * smallest. Values above INT64_MAX need unsigned long long.
	 */
	if (big && min < 0)
		fail(p, -EINVAL, "unsupported: enum needs more than 64 bits");
	else if (big)
		prim = CREFL_ULLONG;
	else if (packed && min >= INT8_MIN &&
		 max <= (min < 0 ? INT8_MAX : UINT8_MAX))
		prim = (min < 0) ? CREFL_SCHAR : CREFL_UCHAR;
	else if (packed && min >= INT16_MIN &&
		 max <= (min < 0 ? INT16_MAX : UINT16_MAX))
		prim = (min < 0) ? CREFL_SHORT : CREFL_USHORT;
	else if (min >= INT32_MIN && max <= (min < 0 ? INT32_MAX : UINT32_MAX))
		prim = (min < 0) ? CREFL_INT : CREFL_UINT;
	else
		prim = (min < 0) ? CREFL_LLONG : CREFL_ULLONG;

	t->prim = prim;
	t->size = crefl_prims[prim].size;
	t->align = crefl_prims[prim].align;
	t->enums = vals;
	t->len = n;
	t->flags |= F_COMPLETE | (min < 0 && !big ? F_SIGNED : 0) |
		    (packed ? F_PACKED : 0);

	if (tag && crefl__types_push(p->ctx, t))
		fail(p, -ENOMEM, "out of memory");
	return t;
}

/*
 * specifiers and declarators
 */
struct spec {
	unsigned int n_void, n_bool, n_char, n_short, n_int, n_long;
	unsigned int n_signed, n_unsigned, n_float, n_double, n_float128;
	unsigned int n_int128;
};

static int any_prim(const struct spec *s)
{
	return s->n_void || s->n_bool || s->n_char || s->n_short || s->n_int ||
	       s->n_long || s->n_signed || s->n_unsigned || s->n_float ||
	       s->n_double || s->n_float128 || s->n_int128;
}

static enum crefl_prim combine(struct parser *p, const struct spec *s)
{
	int u = s->n_unsigned;

	if ((s->n_signed && s->n_unsigned) || s->n_long > 2)
		fail(p, -EINVAL, "invalid type specifiers");

	if (s->n_void)
		return CREFL_VOID;
	if (s->n_bool)
		return CREFL_BOOL;
	if (s->n_int128) {
		if (s->n_int128 > 1 || s->n_char || s->n_short || s->n_int ||
		    s->n_long || s->n_float || s->n_double || s->n_float128)
			fail(p, -EINVAL, "invalid type specifiers");
		return u ? CREFL_UINT128 : CREFL_INT128;
	}
	if (s->n_float128)
		return CREFL_FLOAT128;
	if (s->n_float)
		return CREFL_FLOAT;
	if (s->n_double)
		return s->n_long ? CREFL_LDOUBLE : CREFL_DOUBLE;
	if (s->n_char)
		return s->n_signed ? CREFL_SCHAR : u ? CREFL_UCHAR : CREFL_CHAR;
	if (s->n_short)
		return u ? CREFL_USHORT : CREFL_SHORT;
	if (s->n_long == 2)
		return u ? CREFL_ULLONG : CREFL_LLONG;
	if (s->n_long == 1)
		return u ? CREFL_ULONG : CREFL_LONG;
	return u ? CREFL_UINT : CREFL_INT;
}

static const crefl_type_t *type_name(struct parser *p);
static int is_type_start(struct parser *p);
static const crefl_type_t *ptr_to(struct parser *p, const crefl_type_t *elem);

/* typeof(type-name), typeof(nullptr) from C23 stddef.h, no expressions */
static const crefl_type_t *typeof_spec(struct parser *p)
{
	const crefl_type_t *t;

	next(p);
	expect(p, '(');
	if (is_kw(p, "nullptr")) {
		next(p);
		t = ptr_to(p, &crefl_prims[CREFL_VOID]);
	} else if (is_type_start(p)) {
		t = type_name(p);
	} else {
		fail(p, -EINVAL, "unsupported: typeof expression");
	}
	expect(p, ')');
	return t;
}

/*
 * @a: collects attributes in the specifiers, they apply to all
 *     declarators. NULL where they are not supported.
 * @return base type or NULL if no type specifier was found
 */
static const crefl_type_t *specifiers(struct parser *p, int *is_typedef,
				      struct crefl_attr *a)
{
	struct spec s = { 0 };
	const crefl_type_t *t = NULL;

	while (is(p, T_IDENT)) {
		if (is_kw(p, "typedef")) {
			if (is_typedef == NULL)
				fail(p, -EINVAL, "unexpected typedef");
			*is_typedef = 1;
			next(p);
		} else if (is_kw_in(p, ignored_kws)) {
			next(p);
		} else if (is_attr(p)) {
			skip_attrs(p, a);
		} else if (is_kw_in(p, unsupported_kws)) {
			fail(p, -EINVAL, "unsupported: '%.*s'", (int)p->l.tl,
			     p->l.tp);
		} else if (is_kw_in(p, prim_kws)) {
			if (is_kw(p, "void"))
				s.n_void++;
			else if (is_kw(p, "_Bool"))
				s.n_bool++;
			else if (is_kw(p, "char"))
				s.n_char++;
			else if (is_kw(p, "short"))
				s.n_short++;
			else if (is_kw(p, "int"))
				s.n_int++;
			else if (is_kw(p, "long"))
				s.n_long++;
			else if (is_kw(p, "unsigned"))
				s.n_unsigned++;
			else if (is_kw(p, "float"))
				s.n_float++;
			else if (is_kw(p, "double"))
				s.n_double++;
			else if (is_kw(p, "__float128") ||
				 is_kw(p, "_Float128"))
				s.n_float128++;
			else if (is_kw(p, "__int128"))
				s.n_int128++;
			else
				s.n_signed++;
			if (t)
				fail(p, -EINVAL, "conflicting type specifiers");
			next(p);
		} else if (is_kw(p, "typeof") || is_kw(p, "__typeof__") ||
			   is_kw(p, "__typeof")) {
			if (t || any_prim(&s))
				fail(p, -EINVAL, "conflicting type specifiers");
			t = typeof_spec(p);
		} else if (is_kw(p, "struct") || is_kw(p, "union") ||
			   is_kw(p, "enum")) {
			if (t || any_prim(&s))
				fail(p, -EINVAL, "conflicting type specifiers");
			t = is_kw(p, "enum") ? enum_spec(p) : struct_spec(p);
		} else {
			const crefl_type_t *td;

			/* identifier: typedef name only if no type seen yet */
			if (t || any_prim(&s))
				break;
			td = lookup_typedef(p, p->l.tp, p->l.tl);
			if (td == NULL)
				break;
			t = td;
			next(p);
		}
	}

	if (t)
		return t;
	if (any_prim(&s))
		return &crefl_prims[combine(p, &s)];
	return NULL;
}

static const crefl_type_t *array_of(struct parser *p, const crefl_type_t *elem,
				    size_t len)
{
	const crefl_type_t *e = crefl_resolve(elem), *t;

	if (!crefl_is_complete(e))
		fail(p, -EINVAL, "array of incomplete type");
	if (e->size && len > SIZE_MAX / e->size)
		fail(p, -EINVAL, "array too large");
	t = crefl__array_of(p->ctx, elem, len);
	if (t == NULL)
		fail(p, -ENOMEM, "out of memory");
	return t;
}

static const crefl_type_t *ptr_to(struct parser *p, const crefl_type_t *elem)
{
	const crefl_type_t *t = crefl__ptr_to(p->ctx, elem);

	if (t == NULL)
		fail(p, -ENOMEM, "out of memory");
	return t;
}

/*
 * array and function suffixes. Function types are not modeled:
 * pointers to functions become void *, add CREFL_FUNC if needed.
 */
static const crefl_type_t *suffixes(struct parser *p, const crefl_type_t *base)
{
	size_t dims[MAX_DIMS];
	int nd = 0, func = 0;

	for (;;) {
		if (is(p, '[')) {
			size_t len = 0;

			next(p);
			skip_quals(p);
			if (!is(p, ']')) {
				int64_t x = expr(p);

				if (x < 0)
					fail(p, -EINVAL, "negative array size");
				len = (size_t)x;
			}
			expect(p, ']');
			if (nd == MAX_DIMS)
				fail(p, -EINVAL, "too many array dimensions");
			dims[nd++] = len;
		} else if (is(p, '(')) {
			skip_balanced(p);
			func = 1;
		} else {
			break;
		}
	}

	for (int i = nd - 1; i >= 0; i--)
		base = array_of(p, base, dims[i]);
	if (func)
		base = &crefl_prims[CREFL_VOID];
	return base;
}

/* after '(': nested declarator or parameter list? */
static int nested_follows(struct parser *p)
{
	struct lex save = p->l;
	int r;

	next(p);
	r = is(p, '*') || is(p, '(') || is(p, '[') || is(p, '^') ||
	    is_attr(p) || (is(p, T_IDENT) && !is_type_start(p));
	p->l = save;
	return r;
}

static const crefl_type_t *declarator(struct parser *p,
				      const crefl_type_t *base,
				      const char **name, size_t *nl)
{
	enter(p);

	while (accept(p, '*')) {
		base = ptr_to(p, base);
		skip_quals(p);
	}
	skip_quals(p);

	if (is(p, '(') && nested_follows(p)) {
		struct lex at_paren = p->l, after;

		/* outer suffixes apply first, then the inner declarator */
		skip_balanced(p);
		base = suffixes(p, base);
		after = p->l;
		p->l = at_paren;
		next(p);
		base = declarator(p, base, name, nl);
		expect(p, ')');
		p->l = after;
	} else {
		if (is(p, T_IDENT)) {
			*name = p->l.tp;
			*nl = p->l.tl;
			next(p);
		}
		base = suffixes(p, base);
	}

	p->depth--;
	return base;
}

static const crefl_type_t *type_name(struct parser *p)
{
	const crefl_type_t *t = specifiers(p, NULL, NULL);
	const char *name = NULL;
	size_t nl = 0;

	if (t == NULL)
		fail(p, -EINVAL, "expected type name");
	t = declarator(p, t, &name, &nl);
	if (name)
		fail(p, -EINVAL, "unexpected identifier '%.*s'", (int)nl, name);
	return t;
}

/*
 * integer constant expressions: | ^ & << >> + - * / % and unary
 * - + ~ !, sizeof/_Alignof(type), casts are ignored
 */
static int binprec(int tok)
{
	switch (tok) {
	case '|':
		return 1;
	case '^':
		return 2;
	case '&':
		return 3;
	case T_SHL:
	case T_SHR:
		return 4;
	case '+':
	case '-':
		return 5;
	case '*':
	case '/':
	case '%':
		return 6;
	default:
		return 0;
	}
}

/* value of an expression as mathematical integer, big: above INT64_MAX */
static int64_t math_val(struct cval c, int *big)
{
	*big = c.u && rank_bits(c.rank) == 64 && c.v < 0;
	return c.v;
}

/* typed constant for a stored value (enumerators, -D) */
static struct cval sym_val(int64_t v, int big)
{
	struct cval c = { v, 0, 0 };

	if (big)
		c.rank = 2, c.u = 1;
	else if (v < INT32_MIN || v > INT32_MAX)
		c.rank = 2;
	return c;
}

/* conversion of a cast to an integer type, then integer promotion */
static struct cval cast(struct parser *p, const crefl_type_t *t, struct cval v)
{
	const crefl_type_t *rt = crefl_resolve(t);
	unsigned int bits;
	int sgn;

	if (rt->kind == CREFL_PTR) {
		v.rank = 1;
		v.u = 1;
		return norm(v);
	}
	if (!((rt->kind == CREFL_PRIM && rt->prim >= CREFL_BOOL &&
	       rt->prim <= CREFL_ULLONG) ||
	      rt->kind == CREFL_ENUM))
		fail(p, -EINVAL, "unsupported cast");

	if (rt->kind == CREFL_PRIM && rt->prim == CREFL_BOOL) {
		v.v = v.v != 0;
		v.rank = 0;
		v.u = 0;
		return v;
	}

	bits = crefl_sizeof(rt) * 8;
	sgn = crefl_is_signed(rt);
	if (bits < 64) {
		uint64_t m = (1ULL << bits) - 1, x = (uint64_t)v.v & m;

		if (sgn && (x >> (bits - 1)) & 1)
			x |= ~m;
		v.v = (int64_t)x;
	}

	/* smaller than int promotes to int */
	if (bits < 32) {
		v.rank = 0;
		v.u = 0;
	} else {
		v.rank = bits == 32 ? 0 : rank_bits(1) == 64 ? 1 : 2;
		v.u = !sgn;
	}
	return v;
}

static struct cval unary(struct parser *p)
{
	struct cval v = { 0, 0, 0 };

	enter(p);

	if (accept(p, '-')) {
		v = unary(p);
		v.v = (int64_t)(0 - (uint64_t)v.v);
		v = norm(v);
	} else if (accept(p, '+')) {
		v = unary(p);
	} else if (accept(p, '~')) {
		v = unary(p);
		v.v = ~v.v;
		v = norm(v);
	} else if (accept(p, '!')) {
		v.v = !unary(p).v;
	} else if (accept(p, '(')) {
		if (is_type_start(p)) {
			const crefl_type_t *t = type_name(p);

			expect(p, ')');
			v = cast(p, t, unary(p));
		} else {
			v = cexpr(p);
			expect(p, ')');
		}
	} else if (is(p, T_NUM)) {
		v = p->l.numval;
		next(p);
	} else if (is(p, T_PPNUM)) {
		fail(p, -EINVAL, "unsupported number '%.*s'", (int)p->l.tl,
		     p->l.tp);
	} else if (is_kw(p, "sizeof") || is_kw(p, "_Alignof") ||
		   is_kw(p, "alignof") || is_kw(p, "__alignof__") ||
		   is_kw(p, "__alignof")) {
		int is_size = is_kw(p, "sizeof");
		int pref = is_kw(p, "__alignof__") || is_kw(p, "__alignof");
		const crefl_type_t *t;

		next(p);
		expect(p, '(');
		if (!is_type_start(p))
			fail(p, -EINVAL, "unsupported: sizeof expression");
		t = type_name(p);
		expect(p, ')');
		if (!crefl_is_complete(t))
			fail(p, -EINVAL, "sizeof incomplete type");
		if (is_size)
			v.v = (int64_t)crefl_sizeof(t);
		else
			v.v = (int64_t)(pref ? crefl__pref_align(t) :
					       crefl_alignof(t));
		v.rank = 1; /* size_t: unsigned long */
		v.u = 1;
	} else if (is(p, T_IDENT)) {
		struct sym *s =
			crefl__sym_find(p->ctx, NS_ORD, p->l.tp, p->l.tl);

		if (s == NULL || s->t)
			fail(p, -EINVAL, "unknown constant '%.*s'",
			     (int)p->l.tl, p->l.tp);
		v = sym_val(s->val, s->val_unsigned);
		next(p);
	} else {
		fail(p, -EINVAL, "expected expression");
	}

	p->depth--;
	return v;
}

/* usual arithmetic conversions of two promoted integers */
static void usual(struct cval *a, struct cval *b)
{
	struct cval *uo, *so;
	int rank, u;

	if (a->u == b->u) {
		rank = a->rank > b->rank ? a->rank : b->rank;
		u = a->u;
	} else {
		uo = a->u ? a : b;
		so = a->u ? b : a;
		if (uo->rank >= so->rank) {
			rank = uo->rank;
			u = 1;
		} else {
			rank = so->rank;
			u = rank_bits(so->rank) <= rank_bits(uo->rank);
		}
	}
	a->rank = b->rank = rank;
	a->u = b->u = u;
	*a = norm(*a);
	*b = norm(*b);
}

static struct cval binop(struct parser *p, int op, struct cval a, struct cval b)
{
	unsigned int w;
	uint64_t ua, ub;
	struct cval r;

	if (op == T_SHL || op == T_SHR) {
		/* type of the left operand, count must be below its width */
		int big;
		int64_t n = math_val(b, &big);

		w = rank_bits(a.rank);
		if (big || n < 0 || n >= w)
			fail(p, -EINVAL, "invalid shift");
		r = a;
		if (op == T_SHL)
			r.v = (int64_t)((uint64_t)a.v << n);
		else if (a.u)
			r.v = (int64_t)((w == 64 ?
						 (uint64_t)a.v :
						 (uint64_t)a.v & 0xffffffff) >>
					n);
		else
			r.v = a.v >> n;
		return norm(r);
	}

	usual(&a, &b);
	r = a;
	ua = (uint64_t)a.v;
	ub = (uint64_t)b.v;

	switch (op) {
	case '|':
		r.v = a.v | b.v;
		break;
	case '^':
		r.v = a.v ^ b.v;
		break;
	case '&':
		r.v = a.v & b.v;
		break;
	case '+':
		r.v = (int64_t)(ua + ub);
		break;
	case '-':
		r.v = (int64_t)(ua - ub);
		break;
	case '*':
		r.v = (int64_t)(ua * ub);
		break;
	default:
		w = rank_bits(r.rank);
		if (ub == 0 ||
		    (!r.u && b.v == -1 &&
		     a.v == (w == 64 ? INT64_MIN : (int64_t)INT32_MIN)))
			fail(p, -EINVAL, "division by zero or overflow");
		if (r.u && w == 64)
			r.v = (int64_t)((op == '/') ? ua / ub : ua % ub);
		else
			r.v = (op == '/') ? a.v / b.v : a.v % b.v;
		break;
	}
	return norm(r);
}

static struct cval expr_prec(struct parser *p, int minprec)
{
	struct cval a = unary(p);

	for (;;) {
		int op = p->l.tok, prec = binprec(op);

		if (prec == 0 || prec < minprec)
			return a;
		next(p);
		a = binop(p, op, a, expr_prec(p, prec + 1));
	}
}

static struct cval cexpr(struct parser *p)
{
	struct cval v;

	enter(p);
	v = expr_prec(p, 1);
	p->depth--;
	return v;
}

/* signed view, for widths, sizes and alignments */
static int64_t expr(struct parser *p)
{
	struct cval v = cexpr(p);

	if (v.u && v.v < 0)
		fail(p, -EINVAL, "constant out of range");
	return v.v;
}

/*
 * top level
 */
static void external_decl(struct parser *p)
{
	const crefl_type_t *base;
	struct crefl_attr spec_attr = { 0 };
	int is_td = 0;

	if (accept(p, ';'))
		return;
	if (is_kw(p, "_Static_assert") || is_kw(p, "static_assert")) {
		skip_static_assert(p);
		return;
	}

	/* extern "C" from #ifdef __cplusplus, kept without preprocessor */
	if (is_kw(p, "extern") && peek(p) == T_STR) {
		next(p);
		next(p);
		if (accept(p, '{'))
			p->extern_c++;
		return;
	}
	if (p->extern_c && accept(p, '}')) {
		p->extern_c--;
		return;
	}

	base = specifiers(p, &is_td, &spec_attr);
	if (base == NULL) {
		if (is(p, T_IDENT))
			fail(p, -EINVAL, "unknown type name '%.*s'",
			     (int)p->l.tl, p->l.tp);
		fail(p, -EINVAL, "expected declaration");
	}

	if (accept(p, ';'))
		return;

	for (;;) {
		const char *name = NULL;
		size_t nl = 0;
		const crefl_type_t *t = declarator(p, base, &name, &nl);
		struct crefl_attr da = spec_attr;

		/* attributes of variables and functions don't matter */
		skip_attrs(p, &da);

		if (is_td) {
			t = apply_mode(p, t, &da);
			/* a no-op if natural, e.g. linux/types.h __s128 */
			if (da.align && da.align != crefl_alignof(t))
				fail(p, -EINVAL,
				     "unsupported: aligned typedef");
			if (name == NULL)
				fail(p, -EINVAL, "expected typedef name");
			add_typedef(p, name, nl, t);
		}

		/* function body */
		if (is(p, '{')) {
			skip_balanced(p);
			return;
		}

		/* initializer */
		if (accept(p, '=')) {
			while (!is(p, ',') && !is(p, ';')) {
				if (is(p, '(') || is(p, '[') || is(p, '{'))
					skip_balanced(p);
				else if (is(p, T_EOF))
					fail(p, -EINVAL,
					     "expected ';' before end of input");
				else
					next(p);
			}
		}

		if (!accept(p, ','))
			break;
	}
	expect(p, ';');
}

/*
 * a failed parse leaves the context as it was before, except for the
 * arena memory used: symbols and types added since are dropped and
 * definitions started since are reset to incomplete
 */
static void rollback(crefl_ctx_t *ctx, struct sym *symlog, size_t ntypes)
{
	while (ctx->symlog != symlog)
		crefl__sym_pop(ctx);
	ctx->ntypes = ntypes;

	for (size_t i = 0; i < ctx->nundo; i++) {
		crefl_type_t *t = ctx->undo[i];

		t->flags = 0;
		t->prim = 0;
		t->size = 0;
		t->align = 1;
		t->len = 0;
		t->members = NULL;
		t->enums = NULL;
		t->mattr = NULL;
		t->attr_align = 0;
	}
	ctx->nundo = 0;
}

int crefl_parse(crefl_ctx_t *ctx, const char *src)
{
	struct parser p = { .ctx = ctx, .l = { .s = src, .line = 1 } };
	struct sym *symlog;
	size_t ntypes;

	if (ctx == NULL || src == NULL)
		return -EINVAL;

	ctx->err[0] = '\0';
	ctx->nundo = 0;
	symlog = ctx->symlog;
	ntypes = ctx->ntypes;

	/* setjmp may only be a whole controlling expression (C11 7.13.1.1) */
	if (setjmp(p.jb)) {
		rollback(ctx, symlog, ntypes);
		return ctx->errcode;
	}

	next(&p);
	while (!is(&p, T_EOF))
		external_decl(&p);
	ctx->nundo = 0;
	return 0;
}

const crefl_type_t *crefl_type(crefl_ctx_t *ctx, const char *decl)
{
	struct parser p = { .ctx = ctx, .l = { .s = decl, .line = 1 } };
	const crefl_type_t *t;
	struct sym *symlog;
	size_t ntypes;

	if (ctx == NULL || decl == NULL)
		return NULL;

	ctx->err[0] = '\0';
	ctx->nundo = 0;
	symlog = ctx->symlog;
	ntypes = ctx->ntypes;

	if (setjmp(p.jb)) {
		rollback(ctx, symlog, ntypes);
		return NULL;
	}

	next(&p);
	t = type_name(&p);
	if (!is(&p, T_EOF))
		fail(&p, -EINVAL, "unexpected '%.*s'", (int)p.l.tl, p.l.tp);
	ctx->nundo = 0;
	return t;
}
