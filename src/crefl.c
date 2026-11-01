// SPDX-License-Identifier: LGPL-2.0-or-later
/*
 * crefl: types, layout and data access
 *
 * Copyright (C) 2019-2026 Markus Klotzbuecher <mk@mkio.de>
 */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "crefl_int.h"

#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define CHUNK_SIZE 4096

static size_t roundup(size_t x, size_t a)
{
	return (x + a - 1) / a * a;
}

/*
 * arena
 */
struct chunk {
	struct chunk *next;
	size_t used;
	size_t cap;
	_Alignas(max_align_t) unsigned char data[];
};

void *crefl__alloc(crefl_ctx_t *ctx, size_t size)
{
	struct chunk *c = ctx->chunks;
	void *p;

	size = roundup(size ? size : 1, _Alignof(max_align_t));

	if (c == NULL || c->cap - c->used < size) {
		if (ctx->is_static)
			return NULL;

		size_t cap = MAX(size, CHUNK_SIZE);

		c = malloc(sizeof(*c) + cap);

		if (c == NULL)
			return NULL;

		c->used = 0;
		c->cap = cap;
		c->next = ctx->chunks;
		ctx->chunks = c;
	}

	p = c->data + c->used;
	c->used += size;
	memset(p, 0, size);
	return p;
}

/* old memory is not reclaimed, callers grow geometrically */
void *crefl__grow(crefl_ctx_t *ctx, void *old, size_t oldsize, size_t newsize)
{
	void *p = crefl__alloc(ctx, newsize);

	if (p && old)
		memcpy(p, old, oldsize);
	return p;
}

char *crefl__strndup(crefl_ctx_t *ctx, const char *s, size_t len)
{
	char *p = crefl__alloc(ctx, len + 1);

	if (p)
		memcpy(p, s, len);
	return p;
}

/*
 * context
 */
crefl_ctx_t *crefl_ctx_new(void)
{
	return calloc(1, sizeof(crefl_ctx_t));
}

crefl_ctx_t *crefl_ctx_new_static(void *buf, size_t len)
{
	uintptr_t a = _Alignof(max_align_t);
	uintptr_t start = ((uintptr_t)buf + a - 1) & ~(a - 1);
	size_t hdr = roundup(sizeof(crefl_ctx_t), a) + sizeof(struct chunk);
	crefl_ctx_t *ctx = (crefl_ctx_t *)start;
	struct chunk *c;

	if (buf == NULL || len < (start - (uintptr_t)buf) + hdr)
		return NULL;

	memset(ctx, 0, sizeof(*ctx));
	c = (struct chunk *)(start + roundup(sizeof(crefl_ctx_t), a));
	c->next = NULL;
	c->used = 0;
	c->cap = len - (start - (uintptr_t)buf) - hdr;
	ctx->chunks = c;
	ctx->is_static = 1;
	return ctx;
}

void crefl_ctx_free(crefl_ctx_t *ctx)
{
	struct chunk *c, *next;

	if (ctx == NULL || ctx->is_static)
		return;

	for (c = ctx->chunks; c; c = next) {
		next = c->next;
		free(c);
	}
	free(ctx);
}

int crefl__err(crefl_ctx_t *ctx, int ret, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(ctx->err, sizeof(ctx->err), fmt, ap);
	va_end(ap);
	return ret;
}

const char *crefl_errmsg(const crefl_ctx_t *ctx)
{
	return ctx->err;
}

/*
 * symbols
 */
static unsigned int hash(const char *s, size_t len)
{
	unsigned int h = 5381;

	while (len--)
		h = h * 33 + (unsigned char)*s++;
	return h % CREFL_HASH_SIZE;
}

struct sym *crefl__sym_find(const crefl_ctx_t *ctx, int ns, const char *name,
			    size_t len)
{
	struct sym *s;

	for (s = ctx->tab[hash(name, len)]; s; s = s->next) {
		if (s->ns == ns && strncmp(s->name, name, len) == 0 &&
		    s->name[len] == '\0')
			return s;
	}
	return NULL;
}

struct sym *crefl__sym_add(crefl_ctx_t *ctx, int ns, const char *name,
			   size_t len)
{
	struct sym *s = crefl__alloc(ctx, sizeof(*s));
	unsigned int h = hash(name, len);

	if (s == NULL)
		return NULL;
	s->name = crefl__strndup(ctx, name, len);
	if (s->name == NULL)
		return NULL;

	s->ns = ns;
	s->next = ctx->tab[h];
	ctx->tab[h] = s;
	s->added = ctx->symlog;
	ctx->symlog = s;
	return s;
}

/* the newest symbol is always at the head of its bucket */
void crefl__sym_pop(crefl_ctx_t *ctx)
{
	struct sym *s = ctx->symlog;

	ctx->tab[hash(s->name, strlen(s->name))] = s->next;
	ctx->symlog = s->added;
}

static int is_ident(const char *s)
{
	if (!isalpha((unsigned char)*s) && *s != '_')
		return 0;
	while (isalnum((unsigned char)*s) || *s == '_')
		s++;
	return *s == '\0';
}

int crefl_define(crefl_ctx_t *ctx, const char *name, int64_t val)
{
	struct sym *s;

	if (ctx == NULL || name == NULL)
		return -EINVAL;
	if (!is_ident(name))
		return crefl__err(ctx, -EINVAL, "define: invalid name '%s'",
				  name);

	s = crefl__sym_find(ctx, NS_ORD, name, strlen(name));
	if ((s && s->t) || crefl__builtin_typedef(name, strlen(name)))
		return crefl__err(ctx, -EINVAL, "define: '%s' is a typedef",
				  name);

	if (s == NULL) {
		s = crefl__sym_add(ctx, NS_ORD, name, strlen(name));
		if (s == NULL)
			return crefl__err(ctx, -ENOMEM, "out of memory");
	}

	s->val = val;
	return 0;
}

int crefl__types_push(crefl_ctx_t *ctx, const crefl_type_t *t)
{
	if (ctx->ntypes == ctx->captypes) {
		size_t cap = MAX(16, ctx->captypes * 2);
		const crefl_type_t **n = crefl__grow(ctx, ctx->types,
						     ctx->ntypes * sizeof(*n),
						     cap * sizeof(*n));
		if (n == NULL)
			return -ENOMEM;
		ctx->types = n;
		ctx->captypes = cap;
	}
	ctx->types[ctx->ntypes++] = t;
	return 0;
}

size_t crefl_ntypes(const crefl_ctx_t *ctx)
{
	return ctx->ntypes;
}

const crefl_type_t *crefl_type_at(const crefl_ctx_t *ctx, size_t i)
{
	return (i < ctx->ntypes) ? ctx->types[i] : NULL;
}

/*
 * primitive and builtin types
 */
#define PRIM(p, T)                   \
	[p] = { .kind = CREFL_PRIM,  \
		.prim = p,           \
		.flags = F_COMPLETE, \
		.name = #T,          \
		.size = sizeof(T),   \
		.align = _Alignof(T) }

const crefl_type_t crefl_prims[CREFL_PRIM_NUM] = {
	[CREFL_VOID] = { .kind = CREFL_PRIM,
			 .prim = CREFL_VOID,
			 .name = "void" },
	PRIM(CREFL_BOOL, _Bool),
	PRIM(CREFL_CHAR, char),
	PRIM(CREFL_SCHAR, signed char),
	PRIM(CREFL_UCHAR, unsigned char),
	PRIM(CREFL_SHORT, short),
	PRIM(CREFL_USHORT, unsigned short),
	PRIM(CREFL_INT, int),
	PRIM(CREFL_UINT, unsigned int),
	PRIM(CREFL_LONG, long),
	PRIM(CREFL_ULONG, unsigned long),
	PRIM(CREFL_LLONG, long long),
	PRIM(CREFL_ULLONG, unsigned long long),
#ifdef __SIZEOF_INT128__
	PRIM(CREFL_INT128, __int128),
	PRIM(CREFL_UINT128, unsigned __int128),
#else
	[CREFL_INT128] = { .kind = CREFL_PRIM,
			   .prim = CREFL_INT128,
			   .name = "__int128" },
	[CREFL_UINT128] = { .kind = CREFL_PRIM,
			    .prim = CREFL_UINT128,
			    .name = "unsigned __int128" },
#endif
	PRIM(CREFL_FLOAT, float),
	PRIM(CREFL_DOUBLE, double),
	PRIM(CREFL_LDOUBLE, long double),
#ifdef __SIZEOF_FLOAT128__
	PRIM(CREFL_FLOAT128, __float128),
#else
	[CREFL_FLOAT128] = { .kind = CREFL_PRIM,
			     .prim = CREFL_FLOAT128,
			     .name = "__float128" },
#endif
};

#define PREF(p, T) [p] = __alignof__(T)

static const uint8_t prim_pref_align[CREFL_PRIM_NUM] = {
	PREF(CREFL_BOOL, _Bool),
	PREF(CREFL_CHAR, char),
	PREF(CREFL_SCHAR, signed char),
	PREF(CREFL_UCHAR, unsigned char),
	PREF(CREFL_SHORT, short),
	PREF(CREFL_USHORT, unsigned short),
	PREF(CREFL_INT, int),
	PREF(CREFL_UINT, unsigned int),
	PREF(CREFL_LONG, long),
	PREF(CREFL_ULONG, unsigned long),
	PREF(CREFL_LLONG, long long),
	PREF(CREFL_ULLONG, unsigned long long),
#ifdef __SIZEOF_INT128__
	PREF(CREFL_INT128, __int128),
	PREF(CREFL_UINT128, unsigned __int128),
#endif
	PREF(CREFL_FLOAT, float),
	PREF(CREFL_DOUBLE, double),
	PREF(CREFL_LDOUBLE, long double),
#ifdef __SIZEOF_FLOAT128__
	PREF(CREFL_FLOAT128, __float128),
#endif
};

/* arrays may be aligned beyond their element, e.g. __builtin_va_list */
size_t crefl__pref_align(const crefl_type_t *t)
{
	size_t a;

	t = crefl_resolve(t);
	a = t->align;
	while (t->kind == CREFL_ARRAY)
		t = crefl_resolve(t->elem);
	if (t->kind == CREFL_PRIM && t->prim != CREFL_VOID)
		return MAX(a, prim_pref_align[t->prim]);
	return MAX(a, t->align);
}

#define PRIM_OF(T)                            \
	_Generic((T)0,                        \
		_Bool: CREFL_BOOL,            \
		char: CREFL_CHAR,             \
		signed char: CREFL_SCHAR,     \
		unsigned char: CREFL_UCHAR,   \
		short: CREFL_SHORT,           \
		unsigned short: CREFL_USHORT, \
		int: CREFL_INT,               \
		unsigned int: CREFL_UINT,     \
		long: CREFL_LONG,             \
		unsigned long: CREFL_ULONG,   \
		long long: CREFL_LLONG,       \
		unsigned long long: CREFL_ULLONG)

#define BUILTIN(T)               \
	{ .kind = CREFL_TYPEDEF, \
	  .flags = F_COMPLETE,   \
	  .name = #T,            \
	  .elem = &crefl_prims[PRIM_OF(T)] }

/* opaque, only its layout matters */
static const crefl_type_t va_list_bytes = {
	.kind = CREFL_ARRAY,
	.flags = F_COMPLETE,
	.elem = &crefl_prims[CREFL_UCHAR],
	.len = sizeof(va_list),
	.size = sizeof(va_list),
	.align = _Alignof(va_list),
};

static const crefl_type_t builtin_typedefs[] = {
	BUILTIN(int8_t),
	BUILTIN(int16_t),
	BUILTIN(int32_t),
	BUILTIN(int64_t),
	BUILTIN(uint8_t),
	BUILTIN(uint16_t),
	BUILTIN(uint32_t),
	BUILTIN(uint64_t),
	BUILTIN(intptr_t),
	BUILTIN(uintptr_t),
	BUILTIN(intmax_t),
	BUILTIN(uintmax_t),
	BUILTIN(size_t),
	BUILTIN(ssize_t),
	BUILTIN(ptrdiff_t),
	{ .kind = CREFL_TYPEDEF,
	  .flags = F_COMPLETE,
	  .name = "bool",
	  .elem = &crefl_prims[CREFL_BOOL] },
	/* incomplete without __int128, like their target */
	{ .kind = CREFL_TYPEDEF,
	  .flags = F_COMPLETE,
	  .name = "__int128_t",
	  .elem = &crefl_prims[CREFL_INT128] },
	{ .kind = CREFL_TYPEDEF,
	  .flags = F_COMPLETE,
	  .name = "__uint128_t",
	  .elem = &crefl_prims[CREFL_UINT128] },
	{ .kind = CREFL_TYPEDEF,
	  .flags = F_COMPLETE,
	  .name = "__builtin_va_list",
	  .elem = &va_list_bytes },
};

const crefl_type_t *crefl__builtin_typedef(const char *name, size_t len)
{
	for (size_t i = 0; i < ARRAY_SIZE(builtin_typedefs); i++) {
		const char *n = builtin_typedefs[i].name;

		if (strncmp(n, name, len) == 0 && n[len] == '\0')
			return &builtin_typedefs[i];
	}
	return NULL;
}

/*
 * types
 */
crefl_type_t *crefl__type_new(crefl_ctx_t *ctx, enum crefl_kind kind,
			      const char *name)
{
	crefl_type_t *t = crefl__alloc(ctx, sizeof(*t));

	if (t) {
		t->kind = kind;
		t->name = name;
		t->align = 1;
	}
	return t;
}

const crefl_type_t *crefl__ptr_to(crefl_ctx_t *ctx, const crefl_type_t *elem)
{
	crefl_type_t *t = crefl__type_new(ctx, CREFL_PTR, NULL);

	if (t) {
		t->elem = elem;
		t->size = sizeof(void *);
		t->align = _Alignof(void *);
		t->flags = F_COMPLETE;
	}
	return t;
}

/* NULL if @elem is incomplete (errno-less, caller reports) */
const crefl_type_t *crefl__array_of(crefl_ctx_t *ctx, const crefl_type_t *elem,
				    size_t len)
{
	const crefl_type_t *e = crefl_resolve(elem);
	crefl_type_t *t;

	if (!crefl_is_complete(e))
		return NULL;

	t = crefl__type_new(ctx, CREFL_ARRAY, NULL);
	if (t == NULL)
		return NULL;

	t->elem = elem;
	t->len = len;
	t->size = e->size * len;
	t->align = e->align;
	t->flags = F_COMPLETE;
	return t;
}

const crefl_type_t *crefl_resolve(const crefl_type_t *t)
{
	while (t && t->kind == CREFL_TYPEDEF)
		t = t->elem;
	return t;
}

enum crefl_kind crefl_kind(const crefl_type_t *t)
{
	return t->kind;
}
enum crefl_prim crefl_prim(const crefl_type_t *t)
{
	return t->prim;
}
const char *crefl_name(const crefl_type_t *t)
{
	return t->name;
}
size_t crefl_sizeof(const crefl_type_t *t)
{
	return crefl_resolve(t)->size;
}
size_t crefl_alignof(const crefl_type_t *t)
{
	return crefl_resolve(t)->align;
}
int crefl_is_complete(const crefl_type_t *t)
{
	return crefl_resolve(t)->flags & F_COMPLETE;
}
int crefl_is_packed(const crefl_type_t *t)
{
	return !!(crefl_resolve(t)->flags & F_PACKED);
}
const crefl_type_t *crefl_elem(const crefl_type_t *t)
{
	return t->elem;
}

size_t crefl_len(const crefl_type_t *t)
{
	return crefl_resolve(t)->len;
}

const crefl_member_t *crefl_member(const crefl_type_t *t, size_t i)
{
	t = crefl_resolve(t);
	if ((t->kind != CREFL_STRUCT && t->kind != CREFL_UNION) || i >= t->len)
		return NULL;
	return &t->members[i];
}

const crefl_enumval_t *crefl_enumval(const crefl_type_t *t, size_t i)
{
	t = crefl_resolve(t);
	if (t->kind != CREFL_ENUM || i >= t->len)
		return NULL;
	return &t->enums[i];
}

static int is_composite(const crefl_type_t *t)
{
	return t->kind == CREFL_STRUCT || t->kind == CREFL_UNION;
}

/* unnamed members are either zero width bitfields or anonymous struct/unions */
static int is_zero_bitfield(const crefl_member_t *m)
{
	return m->name == NULL && m->bit_width == 0 &&
	       !is_composite(crefl_resolve(m->type));
}

static int is_int(const crefl_type_t *t)
{
	return (t->kind == CREFL_PRIM && t->prim >= CREFL_BOOL &&
		t->prim <= CREFL_UINT128) ||
	       t->kind == CREFL_ENUM;
}

static int is_float(const crefl_type_t *t)
{
	return t->kind == CREFL_PRIM && t->prim >= CREFL_FLOAT;
}

int crefl_is_signed(const crefl_type_t *t)
{
	t = crefl_resolve(t);

	if (t->kind == CREFL_ENUM)
		return !!(t->flags & F_SIGNED);
	if (t->kind != CREFL_PRIM)
		return 0;

	switch (t->prim) {
	case CREFL_CHAR:
		return CHAR_MIN < 0;
	case CREFL_SCHAR:
	case CREFL_SHORT:
	case CREFL_INT:
	case CREFL_LONG:
	case CREFL_LLONG:
	case CREFL_INT128:
		return 1;
	default:
		return is_float(t);
	}
}

/*
 * layout (SysV/AAPCS as implemented by gcc and clang)
 *
 * Positions are tracked in bits. A bitfield of type T is moved to
 * the next alignof(T) boundary if it would not fit into sizeof(T)
 * counted from the last such boundary. Named bitfields contribute
 * alignof(T) to the struct alignment. Unnamed ones only do on ARM
 * (AAPCS), zero width ones there even in packed structs.
 */
#if defined(__arm__) || defined(__aarch64__)
#define UNNAMED_BF_ALIGN 1
#else
#define UNNAMED_BF_ALIGN 0
#endif

int crefl__layout(crefl_ctx_t *ctx, crefl_type_t *t,
		  const struct crefl_attr *sattr,
		  const struct crefl_attr *mattr)
{
	int is_union = (t->kind == CREFL_UNION);
	int spacked = sattr->packed;
	size_t bitpos = 0, maxbits = 0, align = 1;

	for (size_t i = 0; i < t->len; i++) {
		crefl_member_t *m = &t->members[i];
		const crefl_type_t *mt = crefl_resolve(m->type);
		int packed = spacked || mattr[i].packed;
		const char *mname = m->name ? m->name : "<anonymous>";

		if (!crefl_is_complete(mt))
			return crefl__err(ctx, -EINVAL,
					  "member '%s' has incomplete type",
					  mname);

		size_t talign = MAX(packed ? 1 : mt->align, mattr[i].align);
		size_t tbits = mt->size * 8;

		if (is_zero_bitfield(m)) {
			if (!is_int(mt))
				return crefl__err(
					ctx, -EINVAL,
					"bitfield with non-integer type");
			/* also at the end: struct { char a; int : 0; } has size 4 */
			if (!is_union)
				bitpos = roundup(bitpos, mt->align * 8);
			m->off = bitpos / 8;
			maxbits = MAX(maxbits, bitpos);
			if (UNNAMED_BF_ALIGN)
				align = MAX(align, mt->align);
			continue;
		}

		if (m->bit_width) {
			size_t w = m->bit_width;

			if (!is_int(mt))
				return crefl__err(
					ctx, -EINVAL,
					"bitfield '%s' has non-integer type",
					mname);
			if (w > tbits || (mt->kind == CREFL_PRIM &&
					  mt->prim == CREFL_BOOL && w > 1))
				return crefl__err(
					ctx, -EINVAL,
					"width of bitfield '%s' exceeds its type",
					mname);

			if (is_union)
				bitpos = 0;
			else if (!packed && bitpos % (talign * 8) + w > tbits)
				bitpos = roundup(bitpos, talign * 8);

			m->off = bitpos / 8;
			m->bit_off = bitpos % 8;
			bitpos += w;
			if (m->name || UNNAMED_BF_ALIGN)
				align = MAX(align, talign);
		} else {
			size_t off = is_union ? 0 :
						roundup(roundup(bitpos, 8) / 8,
							talign);

			m->off = off;
			bitpos = (off + mt->size) * 8;
			align = MAX(align, talign);
		}
		maxbits = MAX(maxbits, bitpos);
	}

	/* packed only lowers natural alignment, aligned(n) still applies */
	align = MAX(align, sattr->align);

	if (spacked)
		t->flags |= F_PACKED;
	t->mattr = mattr;
	t->attr_align = sattr->align;
	t->align = align;
	t->size = roundup(roundup(maxbits, 8) / 8, align);
	t->flags |= F_COMPLETE;
	return 0;
}

/*
 * values and paths
 */
static const crefl_val_t invalid;

crefl_val_t crefl_wrap(const crefl_type_t *t, void *p)
{
	crefl_val_t v = { .t = t, .p = p };

	return (t && p) ? v : invalid;
}

crefl_val_t crefl_new(const crefl_type_t *t)
{
	if (t == NULL || !crefl_is_complete(t) || crefl_sizeof(t) == 0)
		return invalid;

	return crefl_wrap(t, calloc(1, crefl_sizeof(t)));
}

void crefl_free(crefl_val_t v)
{
	free(v.p);
}

/* find member by name, looking through anonymous struct/union members */
static const crefl_member_t *
find_member(const crefl_type_t *t, const char *name, size_t len, size_t *off)
{
	for (size_t i = 0; i < t->len; i++) {
		const crefl_member_t *m = &t->members[i];
		const crefl_type_t *mt = crefl_resolve(m->type);

		if (m->name) {
			if (strncmp(m->name, name, len) == 0 &&
			    m->name[len] == '\0') {
				*off += m->off;
				return m;
			}
		} else if (m->bit_width == 0 && is_composite(mt)) {
			size_t sub = *off + m->off;
			const crefl_member_t *r =
				find_member(mt, name, len, &sub);

			if (r) {
				*off = sub;
				return r;
			}
		}
	}
	return NULL;
}

int crefl_path(const crefl_type_t *t, const char *expr, crefl_path_t *out)
{
	crefl_path_t p = { .t = t };
	const char *s = expr;

	if (t == NULL || expr == NULL)
		return -EINVAL;

	while (*s) {
		const crefl_type_t *rt = crefl_resolve(p.t);

		if (p.bit_width)
			return -EINVAL;

		if (*s == '[') {
			size_t esize;
			char *end;
			unsigned long long i;

			if (rt->kind != CREFL_ARRAY ||
			    !isdigit((unsigned char)s[1]))
				return -EINVAL;
			errno = 0;
			i = strtoull(s + 1, &end, 0);
			if (*end != ']')
				return -EINVAL;

			/* flexible arrays (len 0) are unbounded */
			esize = crefl_sizeof(rt->elem);
			if (errno || (rt->len && i >= rt->len) ||
			    (esize && i > (SIZE_MAX - p.off) / esize))
				return -ERANGE;

			p.t = rt->elem;
			p.off += i * esize;
			s = end + 1;
			continue;
		}

		if (*s == '.')
			s++;
		else if (s != expr)
			return -EINVAL;

		size_t len = strcspn(s, ".[");
		const crefl_member_t *m;

		if (len == 0 ||
		    (rt->kind != CREFL_STRUCT && rt->kind != CREFL_UNION))
			return -EINVAL;
		m = find_member(rt, s, len, &p.off);
		if (m == NULL)
			return -ENOENT;

		p.t = m->type;
		p.bit_off = m->bit_off;
		p.bit_width = m->bit_width;
		s += len;
	}

	*out = p;
	return 0;
}

crefl_val_t crefl_at(crefl_val_t v, const crefl_path_t *p)
{
	crefl_val_t r = {
		.t = p->t,
		.p = (char *)v.p + p->off,
		.bit_off = p->bit_off,
		.bit_width = p->bit_width,
	};
	return r;
}

crefl_val_t crefl_get(crefl_val_t v, const char *expr)
{
	crefl_path_t p;

	if (v.t == NULL || crefl_path(v.t, expr, &p))
		return invalid;
	return crefl_at(v, &p);
}

crefl_val_t crefl_index(crefl_val_t v, size_t i)
{
	const crefl_type_t *t;

	if (v.t == NULL)
		return invalid;
	t = crefl_resolve(v.t);
	if (t->kind != CREFL_ARRAY || (t->len && i >= t->len))
		return invalid;
	return crefl_wrap(t->elem, (char *)v.p + i * crefl_sizeof(t->elem));
}

crefl_val_t crefl_deref(crefl_val_t v)
{
	const crefl_type_t *t;
	void *p;

	if (v.t == NULL)
		return invalid;
	t = crefl_resolve(v.t);
	if (t->kind != CREFL_PTR)
		return invalid;
	memcpy(&p, v.p, sizeof(p));
	return crefl_wrap(t->elem, p);
}

/*
 * scalar access
 */
static uint64_t mask(unsigned int w)
{
	return (w >= 64) ? UINT64_MAX : ((uint64_t)1 << w) - 1;
}

static uint64_t shl(uint64_t x, int sh)
{
	if (sh >= 64 || sh <= -64)
		return 0;
	return (sh >= 0) ? x << sh : x >> -sh;
}

/*
 * shift of value bit 0 relative to bit 0 of byte i. A field may span
 * up to 9 bytes (64 bit wide at bit_off 7), hence the byte loop.
 */
static int byte_shift(unsigned int i, unsigned int off, unsigned int w)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	(void)w;
	return 8 * (int)i - (int)off;
#else
	return (int)(off + w) - 8 * ((int)i + 1);
#endif
}

static uint64_t bits_get(const unsigned char *p, unsigned int off,
			 unsigned int w)
{
	unsigned int n = (off + w + 7) / 8;
	uint64_t v = 0;

	for (unsigned int i = 0; i < n; i++)
		v |= shl(p[i], byte_shift(i, off, w));
	return v & mask(w);
}

static void bits_set(unsigned char *p, unsigned int off, unsigned int w,
		     uint64_t v)
{
	unsigned int n = (off + w + 7) / 8;

	for (unsigned int i = 0; i < n; i++) {
		int sh = byte_shift(i, off, w);
		unsigned char m = shl(mask(w), -sh);

		p[i] = (p[i] & ~m) | (shl(v, -sh) & m);
	}
}

/* integer storage of v: width in bits, signedness */
static int int_info(crefl_val_t v, const crefl_type_t *t, unsigned int *w,
		    int *sgn)
{
	if (!is_int(t))
		return -EINVAL;

	*sgn = crefl_is_signed(t);
	if (v.bit_width)
		*w = v.bit_width;
	else if (t->kind == CREFL_PRIM && t->prim == CREFL_BOOL)
		*w = 1;
	else
		*w = t->size * 8;
	return 0;
}

static uint64_t load_raw(crefl_val_t v, size_t size)
{
	uint8_t u8;
	uint16_t u16;
	uint32_t u32;
	uint64_t u64;

	switch (size) {
	case 1:
		memcpy(&u8, v.p, 1);
		return u8;
	case 2:
		memcpy(&u16, v.p, 2);
		return u16;
	case 4:
		memcpy(&u32, v.p, 4);
		return u32;
	default:
		memcpy(&u64, v.p, 8);
		return u64;
	}
}

static void store_raw(crefl_val_t v, size_t size, uint64_t x)
{
	uint8_t u8 = x;
	uint16_t u16 = x;
	uint32_t u32 = x;

	switch (size) {
	case 1:
		memcpy(v.p, &u8, 1);
		break;
	case 2:
		memcpy(v.p, &u16, 2);
		break;
	case 4:
		memcpy(v.p, &u32, 4);
		break;
	default:
		memcpy(v.p, &x, 8);
		break;
	}
}

/*
 * read integer, sign extended into *u if signed. -ERANGE if a 128 bit
 * value does not fit 64 bits.
 */
static int int_load(crefl_val_t v, uint64_t *u, int *sgn)
{
	const crefl_type_t *t = crefl_resolve(v.t);
	unsigned int w;

	if (int_info(v, t, &w, sgn))
		return -EINVAL;

#ifdef __SIZEOF_INT128__
	if (w == 128) {
		unsigned __int128 q;

		memcpy(&q, v.p, sizeof(q));
		if (*sgn ? ((__int128)q < INT64_MIN ||
			    (__int128)q > INT64_MAX) :
			   q > UINT64_MAX)
			return -ERANGE;
		*u = (uint64_t)q;
		return 0;
	}
#endif

	uint64_t x = v.bit_width ? bits_get(v.p, v.bit_off, w) :
				   load_raw(v, t->size);

	if (t->kind == CREFL_PRIM && t->prim == CREFL_BOOL)
		x = !!x;
	else if (*sgn && w < 64 && (x >> (w - 1)) & 1)
		x |= ~mask(w);
	*u = x;
	return 0;
}

/* store integer x, given as signed (neg) or unsigned, -ERANGE if it does not fit */
static int int_store(crefl_val_t v, uint64_t x, int neg)
{
	const crefl_type_t *t = crefl_resolve(v.t);
	unsigned int w;
	int sgn;

	if (int_info(v, t, &w, &sgn))
		return -EINVAL;

	if (neg) {
		if (!sgn ||
		    (w < 64 && (int64_t)x < -(int64_t)((uint64_t)1 << (w - 1))))
			return -ERANGE;
	} else if (x > (sgn ? mask(w - 1) : mask(w))) {
		return -ERANGE;
	}

#ifdef __SIZEOF_INT128__
	if (w == 128) {
		__int128 q = neg ? (__int128)(int64_t)x : (__int128)x;

		memcpy(v.p, &q, sizeof(q));
		return 0;
	}
#endif

	if (v.bit_width)
		bits_set(v.p, v.bit_off, w, x & mask(w));
	else
		store_raw(v, t->size, x);
	return 0;
}

static int float_load(crefl_val_t v, const crefl_type_t *t, double *out)
{
	float f;
	double d;
	long double ld;

	switch (t->prim) {
	case CREFL_FLOAT:
		memcpy(&f, v.p, sizeof(f));
		*out = f;
		break;
	case CREFL_DOUBLE:
		memcpy(&d, v.p, sizeof(d));
		*out = d;
		break;
	case CREFL_LDOUBLE:
		memcpy(&ld, v.p, sizeof(ld));
		*out = ld;
		break;
#ifdef __SIZEOF_FLOAT128__
	case CREFL_FLOAT128: {
		__float128 q;

		memcpy(&q, v.p, sizeof(q));
		*out = (double)q;
		break;
	}
#endif
	default:
		return -EINVAL;
	}
	return 0;
}

static int float_store(crefl_val_t v, const crefl_type_t *t, double x)
{
	float f = x;
	long double ld = x;

	switch (t->prim) {
	case CREFL_FLOAT:
		memcpy(v.p, &f, sizeof(f));
		break;
	case CREFL_DOUBLE:
		memcpy(v.p, &x, sizeof(x));
		break;
	case CREFL_LDOUBLE:
		memcpy(v.p, &ld, sizeof(ld));
		break;
#ifdef __SIZEOF_FLOAT128__
	case CREFL_FLOAT128: {
		__float128 q = x;

		memcpy(v.p, &q, sizeof(q));
		break;
	}
#endif
	default:
		return -EINVAL;
	}
	return 0;
}

int crefl_get_i64(crefl_val_t v, int64_t *out)
{
	uint64_t u;
	int sgn;

	int ret;

	if (v.t == NULL)
		return -EINVAL;
	ret = int_load(v, &u, &sgn);
	if (ret)
		return ret;
	if (!sgn && u > INT64_MAX)
		return -ERANGE;
	*out = (int64_t)u;
	return 0;
}

int crefl_get_u64(crefl_val_t v, uint64_t *out)
{
	const crefl_type_t *t;
	uint64_t u;
	int sgn, ret;

	if (v.t == NULL)
		return -EINVAL;

	t = crefl_resolve(v.t);
	if (t->kind == CREFL_PTR) {
		void *p;

		memcpy(&p, v.p, sizeof(p));
		*out = (uintptr_t)p;
		return 0;
	}

	ret = int_load(v, &u, &sgn);
	if (ret)
		return ret;
	if (sgn && (int64_t)u < 0)
		return -ERANGE;
	*out = u;
	return 0;
}

int crefl_get_f64(crefl_val_t v, double *out)
{
	const crefl_type_t *t;
	uint64_t u;
	int sgn;

	if (v.t == NULL)
		return -EINVAL;

	t = crefl_resolve(v.t);
	if (is_float(t))
		return float_load(v, t, out);
#ifdef __SIZEOF_INT128__
	if (t->kind == CREFL_PRIM && t->size == 16 && v.bit_width == 0) {
		unsigned __int128 q;

		memcpy(&q, v.p, sizeof(q));
		*out = (t->prim == CREFL_INT128) ? (double)(__int128)q :
						   (double)q;
		return 0;
	}
#endif

	if (int_load(v, &u, &sgn))
		return -EINVAL;
	*out = sgn ? (double)(int64_t)u : (double)u;
	return 0;
}

int crefl_set_i64(crefl_val_t v, int64_t x)
{
	const crefl_type_t *t;

	if (v.t == NULL)
		return -EINVAL;

	t = crefl_resolve(v.t);
	if (is_float(t))
		return float_store(v, t, (double)x);
	return int_store(v, (uint64_t)x, x < 0);
}

int crefl_set_u64(crefl_val_t v, uint64_t x)
{
	const crefl_type_t *t;

	if (v.t == NULL)
		return -EINVAL;

	t = crefl_resolve(v.t);
	if (t->kind == CREFL_PTR) {
		void *p = (void *)(uintptr_t)x;

		memcpy(v.p, &p, sizeof(p));
		return 0;
	}
	if (is_float(t))
		return float_store(v, t, (double)x);
	return int_store(v, x, 0);
}

int crefl_set_f64(crefl_val_t v, double x)
{
	const crefl_type_t *t;

	if (v.t == NULL)
		return -EINVAL;

	t = crefl_resolve(v.t);
	if (is_float(t))
		return float_store(v, t, x);

	/* integers: only exact integral values in range (NaN fails too) */
	if (!(x >= -0x1p63 && x < 0x1p64))
		return -ERANGE;

	if (x < 0x1p63) {
		int64_t i = (int64_t)x;

		return ((double)i == x) ? crefl_set_i64(v, i) : -ERANGE;
	}

	uint64_t u = (uint64_t)x;

	return ((double)u == x) ? crefl_set_u64(v, u) : -ERANGE;
}

int crefl_set_str(crefl_val_t v, const char *s)
{
	const crefl_type_t *t, *e;
	size_t len;

	if (v.t == NULL || s == NULL)
		return -EINVAL;

	t = crefl_resolve(v.t);
	if (t->kind != CREFL_ARRAY)
		return -EINVAL;

	e = crefl_resolve(t->elem);
	if (e->kind != CREFL_PRIM ||
	    (e->prim != CREFL_CHAR && e->prim != CREFL_SCHAR &&
	     e->prim != CREFL_UCHAR))
		return -EINVAL;

	len = strlen(s) + 1;
	if (len > t->len)
		return -ERANGE;

	memcpy(v.p, s, len);
	memset((char *)v.p + len, 0, t->len - len);
	return 0;
}

/*
 * same type after resolving typedefs: pointers and arrays are created
 * per declarator, so they compare by structure; tags by identity
 */
int crefl__same_type(const crefl_type_t *a, const crefl_type_t *b)
{
	a = crefl_resolve(a);
	b = crefl_resolve(b);

	if (a == b)
		return 1;
	if (a->kind != b->kind || a->len != b->len)
		return 0;
	if (a->kind == CREFL_PTR || a->kind == CREFL_ARRAY)
		return crefl__same_type(a->elem, b->elem);
	return 0;
}

int crefl_copy(crefl_val_t dst, crefl_val_t src)
{
	if (dst.t == NULL || src.t == NULL || !crefl__same_type(dst.t, src.t))
		return -EINVAL;

	if (dst.bit_width || src.bit_width) {
		uint64_t u;
		int sgn, ret;

		ret = int_load(src, &u, &sgn);
		if (ret)
			return ret;
		return int_store(dst, u, sgn && (int64_t)u < 0);
	}

	memmove(dst.p, src.p, crefl_sizeof(dst.t));
	return 0;
}

/*
 * printing
 */
static const char *kind_kw(const crefl_type_t *t)
{
	switch (t->kind) {
	case CREFL_STRUCT:
		return "struct";
	case CREFL_UNION:
		return "union";
	case CREFL_ENUM:
		return "enum";
	default:
		return NULL;
	}
}

static void indent(FILE *f, int n)
{
	while (n-- > 0)
		fputc('\t', f);
}

static void print_body(const crefl_type_t *t, FILE *f, int ind);

/* declarators longer than the buffer are truncated */
static void fmt(char *dst, const char *f, ...)
{
	va_list ap;

	va_start(ap, f);
	vsnprintf(dst, 256, f, ap);
	va_end(ap);
}

/*
 * print declaration of @t with name @name. Peels pointers and arrays
 * into the declarator, prints anonymous composites inline.
 */

static void print_decl(const crefl_type_t *t, const char *name, FILE *f,
		       int ind)
{
	char a[256], b[256];
	char *d = a, *tmp = b;

	fmt(d, "%s", name ? name : "");

	while (t->kind == CREFL_PTR || t->kind == CREFL_ARRAY) {
		if (t->kind == CREFL_PTR)
			fmt(tmp, t->elem->kind == CREFL_ARRAY ? "(*%s)" : "*%s",
			    d);
		else if (t->len)
			fmt(tmp, "%s[%zu]", d, t->len);
		else
			fmt(tmp, "%s[]", d);

		char *x = d;

		d = tmp;
		tmp = x;
		t = t->elem;
	}

	if (kind_kw(t)) {
		fprintf(f, "%s", kind_kw(t));
		if (t->name)
			fprintf(f, " %s", t->name);
		else
			print_body(t, f, ind);
	} else {
		fprintf(f, "%s", t->name);
	}

	if (*d)
		fprintf(f, " %s", d);
}

/* pahole style: bits up to the next byte, then whole bytes */
static void print_hole(FILE *f, int ind, size_t end, size_t start,
		       const char *what)
{
	size_t bend = (end + 7) / 8 * 8;

	if (start <= end)
		return;
	if (bend > end) {
		indent(f, ind);
		fprintf(f, "/* XXX %zu bits hole */\n", MIN(start, bend) - end);
	}
	if (start / 8 > bend / 8) {
		indent(f, ind);
		fprintf(f, "/* XXX %zu bytes %s */\n", start / 8 - bend / 8,
			what);
	}
}

static void print_body(const crefl_type_t *t, FILE *f, int ind)
{
	fprintf(f, " {\n");

	if (t->kind == CREFL_ENUM) {
		for (size_t i = 0; i < t->len; i++) {
			indent(f, ind + 1);
			if (t->flags & F_SIGNED)
				fprintf(f, "%s = %lld,\n", t->enums[i].name,
					(long long)t->enums[i].val);
			else
				fprintf(f, "%s = %llu,\n", t->enums[i].name,
					(unsigned long long)t->enums[i].val);
		}
	} else {
		size_t end = 0; /* in bits */

		for (size_t i = 0; i < t->len; i++) {
			const crefl_member_t *m = &t->members[i];

			if (t->kind == CREFL_STRUCT) {
				size_t start = m->off * 8 + m->bit_off;
				size_t bits = m->bit_width;

				if (!bits && !is_zero_bitfield(m))
					bits = crefl_sizeof(m->type) * 8;
				print_hole(f, ind + 1, end, start, "hole");
				end = MAX(end, start + bits);
			}

			indent(f, ind + 1);
			print_decl(m->type, m->name, f, ind + 1);

			if (m->bit_width || is_zero_bitfield(m))
				fprintf(f, " : %u", m->bit_width);
			if (t->mattr && t->mattr[i].packed)
				fprintf(f, " __attribute__((packed))");
			if (t->mattr && t->mattr[i].align)
				fprintf(f, " __attribute__((aligned(%zu)))",
					t->mattr[i].align);

			if (m->bit_width || is_zero_bitfield(m))
				fprintf(f, "; /* off=%zu, bit=%u */\n", m->off,
					m->bit_off);
			else
				fprintf(f, "; /* off=%zu, size=%zu */\n",
					m->off, crefl_sizeof(m->type));
		}
		if (t->kind == CREFL_STRUCT)
			print_hole(f, ind + 1, end, t->size * 8, "padding");
	}

	indent(f, ind);
	fprintf(f, "}");
	if (t->flags & F_PACKED)
		fprintf(f, " __attribute__((packed))");
	if (t->attr_align)
		fprintf(f, " __attribute__((aligned(%zu)))", t->attr_align);
}

int crefl_print(const crefl_type_t *t, FILE *f)
{
	if (t == NULL)
		return -EINVAL;

	if (kind_kw(t) && t->name) {
		if (!(t->flags & F_COMPLETE)) {
			fprintf(f, "%s %s;\n", kind_kw(t), t->name);
			return 0;
		}
		fprintf(f, "/* size=%zu, align=%zu */\n", t->size, t->align);
		fprintf(f, "%s %s", kind_kw(t), t->name);
		print_body(t, f, 0);
	} else if (t->kind == CREFL_TYPEDEF) {
		fprintf(f, "typedef ");
		print_decl(t->elem, t->name, f, 0);
	} else {
		print_decl(t, NULL, f, 0);
	}
	fprintf(f, ";\n");
	return 0;
}
