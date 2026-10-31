/* SPDX-License-Identifier: LGPL-2.0-or-later */
/* Copyright (C) 2019-2026 Markus Klotzbuecher <mk@mkio.de> */

#ifndef CREFL_H
#define CREFL_H

/*
 * crefl: reflection of C data types
 *
 * Types are parsed from C declarations into a context. They are
 * immutable once complete and live until the context is freed.
 * Layout follows the host compiler's ABI (SysV/AAPCS, gcc/clang).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* also the project version in CMakeLists.txt */
#define CREFL_VERSION_MAJOR 0
#define CREFL_VERSION_MINOR 1
#define CREFL_VERSION_PATCH 0

typedef struct crefl_ctx crefl_ctx_t;
typedef struct crefl_type crefl_type_t;

enum crefl_kind {
	CREFL_PRIM,
	CREFL_PTR,
	CREFL_ARRAY,
	CREFL_STRUCT,
	CREFL_UNION,
	CREFL_ENUM,
	CREFL_TYPEDEF,
};

enum crefl_prim {
	CREFL_VOID,
	CREFL_BOOL,
	CREFL_CHAR,
	CREFL_SCHAR,
	CREFL_UCHAR,
	CREFL_SHORT,
	CREFL_USHORT,
	CREFL_INT,
	CREFL_UINT,
	CREFL_LONG,
	CREFL_ULONG,
	CREFL_LLONG,
	CREFL_ULLONG,
	CREFL_FLOAT,
	CREFL_DOUBLE,
	CREFL_LDOUBLE,
	CREFL_FLOAT128, /* incomplete if the compiler lacks __float128 */
	CREFL_PRIM_NUM,
};

/**
 * struct crefl_member - struct or union member
 * @name: NULL for unnamed bitfields and anonymous struct/union members
 * @type: member type (as declared, may be a typedef)
 * @off: byte offset in parent
 * @bit_off: bitfields: bit offset within byte @off, in allocation
 *           order (LSB first on little endian, MSB first on big endian)
 * @bit_width: bitfields: width in bits, 0 if not a bitfield
 */
typedef struct crefl_member {
	const char *name;
	const crefl_type_t *type;
	size_t off;
	uint16_t bit_off;
	uint16_t bit_width;
} crefl_member_t;

/* val holds the bit pattern, cast to uint64_t if !crefl_is_signed(enum) */
typedef struct crefl_enumval {
	const char *name;
	int64_t val;
} crefl_enumval_t;

/**
 * struct crefl_val - typed view on data, does not own @p
 * @t: type, NULL marks an invalid value (error)
 * @p: data
 * @bit_off, @bit_width: see crefl_member_t, bit_width == 0 if not a bitfield
 */
typedef struct crefl_val {
	const crefl_type_t *t;
	void *p;
	uint16_t bit_off;
	uint16_t bit_width;
} crefl_val_t;

/* precompiled member access path, see crefl_path() */
typedef struct crefl_path {
	const crefl_type_t *t;
	size_t off;
	uint16_t bit_off;
	uint16_t bit_width;
} crefl_path_t;

/*
 * context
 */
crefl_ctx_t *crefl_ctx_new(void);

/**
 * crefl_ctx_new_static - create a context in a user supplied buffer
 *
 * No malloc is used. Parsing fails with -ENOMEM once @buf is full.
 */
crefl_ctx_t *crefl_ctx_new_static(void *buf, size_t len);
void crefl_ctx_free(crefl_ctx_t *ctx);

/* define a constant usable in array sizes and enum values */
int crefl_define(crefl_ctx_t *ctx, const char *name, int64_t val);

/**
 * crefl_parse - parse C declarations
 *
 * Accepts struct, union, enum and typedef declarations. Preprocessor
 * lines and comments are skipped, other declarations (functions,
 * variables) are parsed and dropped. Not validating: run headers
 * through the compiler (or gcc -E -P) for checking.
 *
 * @return 0 or <0 (-EINVAL, -ENOMEM), see crefl_errmsg()
 */
int crefl_parse(crefl_ctx_t *ctx, const char *src);
const char *crefl_errmsg(const crefl_ctx_t *ctx);

/* all named types (tags and typedefs) in definition order */
size_t crefl_ntypes(const crefl_ctx_t *ctx);
const crefl_type_t *crefl_type_at(const crefl_ctx_t *ctx, size_t i);

/**
 * crefl_type - lookup or construct a type from a type name
 * @decl: e.g. "struct foo", "foo_t", "unsigned int[4]", "char *"
 * @return type or NULL
 */
const crefl_type_t *crefl_type(crefl_ctx_t *ctx, const char *decl);

/*
 * type introspection
 */
enum crefl_kind crefl_kind(const crefl_type_t *t);
enum crefl_prim crefl_prim(const crefl_type_t *t); /* CREFL_PRIM only */
/* tag, typedef or prim name, NULL if anonymous */
const char *crefl_name(const crefl_type_t *t);
size_t crefl_sizeof(const crefl_type_t *t);
size_t crefl_alignof(const crefl_type_t *t);
int crefl_is_complete(const crefl_type_t *t);
int crefl_is_packed(const crefl_type_t *t);
int crefl_is_signed(const crefl_type_t *t); /* integer, enum, char */
/* ptr, array, typedef target */
const crefl_type_t *crefl_elem(const crefl_type_t *t);
/* strip typedefs */
const crefl_type_t *crefl_resolve(const crefl_type_t *t);

/* array length, member count or enumerator count */
size_t crefl_len(const crefl_type_t *t);
const crefl_member_t *crefl_member(const crefl_type_t *t, size_t i);
const crefl_enumval_t *crefl_enumval(const crefl_type_t *t, size_t i);

/* print as C declaration with layout comments */
int crefl_print(const crefl_type_t *t, FILE *f);

/*
 * instances
 */
crefl_val_t crefl_new(const crefl_type_t *t); /* zeroed, malloc */
void crefl_free(crefl_val_t v);
crefl_val_t crefl_wrap(const crefl_type_t *t, void *p);

/**
 * crefl_path - resolve a member path once for fast access
 * @expr: C member designator, e.g. "a.b[3].c", "[2].x" for arrays.
 *        Anonymous struct/union members are looked through.
 * @return 0 or -ENOENT, -EINVAL, -ERANGE (index)
 */
int crefl_path(const crefl_type_t *t, const char *expr, crefl_path_t *out);
/* no checks, O(1) */
crefl_val_t crefl_at(crefl_val_t v, const crefl_path_t *p);
/* crefl_path + crefl_at */
crefl_val_t crefl_get(crefl_val_t v, const char *expr);
crefl_val_t crefl_index(crefl_val_t v, size_t i); /* array element */
crefl_val_t crefl_deref(crefl_val_t v); /* follow pointer */

/*
 * scalar access with conversion. Integers, bool, enums, bitfields,
 * floats; u64 also accepts pointers. Setters return -ERANGE if the
 * value does not fit, -EINVAL on type mismatch.
 */
int crefl_get_i64(crefl_val_t v, int64_t *out);
int crefl_get_u64(crefl_val_t v, uint64_t *out);
int crefl_get_f64(crefl_val_t v, double *out);
int crefl_set_i64(crefl_val_t v, int64_t x);
int crefl_set_u64(crefl_val_t v, uint64_t x);
int crefl_set_f64(crefl_val_t v, double x);

/* char arrays: copy incl. NUL, -ERANGE if it does not fit */
int crefl_set_str(crefl_val_t v, const char *s);
int crefl_copy(crefl_val_t dst, crefl_val_t src); /* same type only */

#endif
