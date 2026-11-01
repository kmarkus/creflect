/* SPDX-License-Identifier: LGPL-2.0-or-later */
/* Copyright (C) 2019-2026 Markus Klotzbuecher <mk@mkio.de> */

#ifndef CREFL_INT_H
#define CREFL_INT_H

/* crefl internals shared by crefl.c and crefl_parse.c */

#include "crefl.h"

#if !defined(__BYTE_ORDER__) || (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__ && \
				 __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__)
#error "unsupported byte order"
#endif

#define __printf(a, b) __attribute__((format(printf, a, b)))

#define F_COMPLETE (1 << 0)
#define F_PACKED (1 << 1)
#define F_SIGNED (1 << 2) /* enums */
#define F_DEFINING (1 << 3) /* struct body being parsed */

struct crefl_attr;

struct crefl_type {
	uint8_t kind;
	uint8_t prim;
	uint8_t flags;
	const char *name;
	size_t size;
	size_t align;
	const crefl_type_t *elem;
	size_t len;
	crefl_member_t *members;
	crefl_enumval_t *enums;
	const struct crefl_attr *mattr; /* per member, for printing */
	size_t attr_align; /* aligned(n) of struct/union */
};

enum { NS_TAG, NS_ORD };

/* NS_ORD: typedef if t != NULL, else constant val */
struct sym {
	struct sym *next;
	struct sym *added; /* symbol added before this one, for rollback */
	const char *name;
	uint8_t ns;
	crefl_type_t *t;
	int64_t val;
	uint8_t val_unsigned;
};

#define CREFL_HASH_SIZE 256

struct chunk;

struct crefl_ctx {
	struct chunk *chunks;
	int is_static;
	struct sym *tab[CREFL_HASH_SIZE];
	const crefl_type_t **types;
	size_t ntypes;
	size_t captypes;
	char err[160];
	int errcode; /* parser error, set before longjmp */

	/* undo state of the running parse, see crefl__rollback() */
	struct sym *symlog;
	crefl_type_t **undo;
	size_t nundo;
	size_t capundo;
};

extern const crefl_type_t crefl_prims[CREFL_PRIM_NUM];

void *crefl__alloc(crefl_ctx_t *ctx, size_t size);
void *crefl__grow(crefl_ctx_t *ctx, void *old, size_t oldsize, size_t newsize);
char *crefl__strndup(crefl_ctx_t *ctx, const char *s, size_t len);

struct sym *crefl__sym_find(const crefl_ctx_t *ctx, int ns, const char *name,
			    size_t len);
struct sym *crefl__sym_add(crefl_ctx_t *ctx, int ns, const char *name,
			   size_t len);
/* remove the most recently added symbol */
void crefl__sym_pop(crefl_ctx_t *ctx);
const crefl_type_t *crefl__builtin_typedef(const char *name, size_t len);

crefl_type_t *crefl__type_new(crefl_ctx_t *ctx, enum crefl_kind kind,
			      const char *name);
const crefl_type_t *crefl__ptr_to(crefl_ctx_t *ctx, const crefl_type_t *elem);
const crefl_type_t *crefl__array_of(crefl_ctx_t *ctx, const crefl_type_t *elem,
				    size_t len);
int crefl__types_push(crefl_ctx_t *ctx, const crefl_type_t *t);

/* gcc attributes affecting layout */
struct crefl_attr {
	uint8_t packed;
	size_t align; /* aligned(n), 0 if not set */
	uint8_t mode; /* mode(m) of integers in bytes, 0 if not set */
};

/* compute member offsets, size and alignment of struct/union @t */
int crefl__layout(crefl_ctx_t *ctx, crefl_type_t *t,
		  const struct crefl_attr *sattr,
		  const struct crefl_attr *mattr);

int crefl__same_type(const crefl_type_t *a, const crefl_type_t *b);

/* gcc __alignof__, differs from _Alignof for some types on i386 */
size_t crefl__pref_align(const crefl_type_t *t);

int crefl__err(crefl_ctx_t *ctx, int ret, const char *fmt, ...) __printf(3, 4);

#endif
