/* SPDX-License-Identifier: LGPL-2.0-or-later */
/* Copyright (C) 2019-2026 Markus Klotzbuecher <mk@mkio.de> */

#ifndef CREFL_H
#define CREFL_H

/*
 * crefl: reflection of C data types
 *
 * Types are parsed from C declarations into a context. Once complete
 * they are immutable and live until the context is freed. Their
 * layout follows the host compiler's ABI (SysV/AAPCS, gcc/clang).
 *
 * Memory: only crefl_ctx_new() and crefl_new() call malloc directly.
 * crefl_define(), crefl_parse() and crefl_type() take memory from the
 * context, i.e. from malloc'ed chunks or from the buffer of a static
 * context. All other functions never allocate, so once the types are
 * set up, value access is safe for real-time code.
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

/**
 * struct crefl_enumval - enumerator
 * @name: enumerator name
 * @val: value as bit pattern, cast to uint64_t if !crefl_is_signed(enum)
 */
typedef struct crefl_enumval {
	const char *name;
	int64_t val;
} crefl_enumval_t;

/**
 * struct crefl_val - typed view on data, does not own @p
 * @t: type, NULL marks an invalid value (error)
 * @p: data
 * @bit_off: bitfields: see &struct crefl_member
 * @bit_width: bitfields: width in bits, 0 if not a bitfield
 */
typedef struct crefl_val {
	const crefl_type_t *t;
	void *p;
	uint16_t bit_off;
	uint16_t bit_width;
} crefl_val_t;

/**
 * struct crefl_path - precompiled member access path, see crefl_path()
 * @t: type of the addressed member or element
 * @off: byte offset from the start of the root value
 * @bit_off: bitfields: see &struct crefl_member
 * @bit_width: bitfields: width in bits, 0 if not a bitfield
 */
typedef struct crefl_path {
	const crefl_type_t *t;
	size_t off;
	uint16_t bit_off;
	uint16_t bit_width;
} crefl_path_t;

/*
 * context
 */

/**
 * crefl_ctx_new() - create an empty context
 *
 * The context grows in malloc'ed chunks of 4 KiB as types are added.
 *
 * Context: allocates with malloc.
 *
 * Return: context or NULL if out of memory
 */
crefl_ctx_t *crefl_ctx_new(void);

/**
 * crefl_ctx_new_static() - create a context in a user supplied buffer
 * @buf: buffer, aligned internally, must outlive the context
 * @len: size of @buf in bytes
 *
 * The context never calls malloc, so parsing fails with -ENOMEM once
 * @buf is full. As a rough guide for 64-bit hosts, the context itself
 * needs 2.3 KiB, plus about 200 bytes per named type (tag, typedef or
 * constant) and 100 bytes per member or enumerator. For example,
 * 20 structs with 10 members each fit in 20 KiB. Memory is not
 * reclaimed after a failed parse, nor for the pointer and array types
 * that crefl_type() derives. So leave some headroom and look up types
 * once at init. The safest way to size @buf is to try: find the
 * smallest @len for which parsing your headers succeeds.
 *
 * Return: context or NULL if @buf is NULL or too small
 */
crefl_ctx_t *crefl_ctx_new_static(void *buf, size_t len);

/**
 * crefl_ctx_free() - free a context and all its types
 * @ctx: context, may be NULL
 *
 * A static context is not freed, release its buffer instead.
 * All types and names from @ctx are invalid afterwards.
 */
void crefl_ctx_free(crefl_ctx_t *ctx);

/**
 * crefl_define() - define a constant usable in array sizes and enum values
 * @ctx: context
 * @name: C identifier, must not be a typedef name
 * @val: value
 *
 * Redefining an existing constant overwrites its value.
 *
 * Context: allocates from @ctx for a new constant.
 *
 * Return: 0, -EINVAL (bad name, typedef) or -ENOMEM
 */
int crefl_define(crefl_ctx_t *ctx, const char *name, int64_t val);

/**
 * crefl_parse() - parse C declarations
 * @ctx: context to add the types to
 * @src: NUL terminated C source
 *
 * Accepts struct, union, enum and typedef declarations. Preprocessor
 * lines and comments are skipped, while other declarations such as
 * functions and variables are parsed and dropped. The parser does not
 * validate the source, so let the compiler check your headers (and
 * gcc -E -P expand their macros) first.
 *
 * On error all types and symbols added by this call are rolled back.
 *
 * Context: allocates from @ctx.
 *
 * Return: 0 or <0 (-EINVAL, -ENOMEM), see crefl_errmsg()
 */
int crefl_parse(crefl_ctx_t *ctx, const char *src);

/**
 * crefl_errmsg() - message of the last error
 * @ctx: context
 *
 * Return: message, or "" if the last crefl_parse() or crefl_type()
 * succeeded. It stays valid until the next call on @ctx.
 */
const char *crefl_errmsg(const crefl_ctx_t *ctx);

/**
 * crefl_ntypes() - number of named types
 * @ctx: context
 *
 * Return: number of tags and typedefs, see crefl_type_at()
 */
size_t crefl_ntypes(const crefl_ctx_t *ctx);

/**
 * crefl_type_at() - named type by index
 * @ctx: context
 * @i: index, 0 to crefl_ntypes() - 1, in definition order
 *
 * Return: type or NULL if @i is out of range
 */
const crefl_type_t *crefl_type_at(const crefl_ctx_t *ctx, size_t i);

/**
 * crefl_type() - lookup or construct a type from a type name
 * @ctx: context
 * @decl: e.g. "struct foo", "foo_t", "unsigned int[4]", "char *"
 *
 * Named types are only looked up. Pointer and array types such as
 * "char *" are created anew on every call and added to @ctx, so look
 * them up once and keep the result.
 *
 * Context: allocates from @ctx for pointer, array and unknown tag types.
 *
 * Return: type or NULL, see crefl_errmsg()
 */
const crefl_type_t *crefl_type(crefl_ctx_t *ctx, const char *decl);

/*
 * type introspection
 *
 * These functions require a non-NULL @t and resolve typedefs first,
 * except crefl_kind(), crefl_prim(), crefl_name() and crefl_elem(),
 * which look at @t itself.
 */

/**
 * crefl_kind() - kind of a type
 * @t: type
 *
 * Return: kind, CREFL_TYPEDEF for typedefs
 */
enum crefl_kind crefl_kind(const crefl_type_t *t);

/**
 * crefl_prim() - primitive type
 * @t: type of kind CREFL_PRIM
 *
 * Return: primitive, undefined for other kinds
 */
enum crefl_prim crefl_prim(const crefl_type_t *t);

/**
 * crefl_name() - name of a type
 * @t: type
 *
 * Return: tag, typedef or primitive name, NULL if anonymous
 */
const char *crefl_name(const crefl_type_t *t);

/**
 * crefl_sizeof() - size of a type
 * @t: type
 *
 * Return: size in bytes as sizeof() would, 0 if incomplete
 */
size_t crefl_sizeof(const crefl_type_t *t);

/**
 * crefl_alignof() - alignment of a type
 * @t: type
 *
 * Return: alignment in bytes as _Alignof() would
 */
size_t crefl_alignof(const crefl_type_t *t);

/**
 * crefl_is_complete() - check if a type is complete
 * @t: type
 *
 * Return: non-zero if complete (defined, size known)
 */
int crefl_is_complete(const crefl_type_t *t);

/**
 * crefl_is_packed() - check for __attribute__((packed))
 * @t: type
 *
 * Return: 1 if packed, 0 otherwise
 */
int crefl_is_packed(const crefl_type_t *t);

/**
 * crefl_is_signed() - check if a type is signed
 * @t: type
 *
 * Plain char follows the host ABI.
 *
 * Return: 1 for signed integers, signed enums and floats, 0 otherwise
 */
int crefl_is_signed(const crefl_type_t *t);

/**
 * crefl_elem() - element type
 * @t: type
 *
 * Return: pointer target, array element or typedef target, NULL for
 * other kinds
 */
const crefl_type_t *crefl_elem(const crefl_type_t *t);

/**
 * crefl_resolve() - strip typedefs
 * @t: type, may be NULL
 *
 * Return: first type that is not a typedef
 */
const crefl_type_t *crefl_resolve(const crefl_type_t *t);

/**
 * crefl_len() - number of elements
 * @t: type
 *
 * Return: array length (0 for flexible arrays), member count or
 * enumerator count
 */
size_t crefl_len(const crefl_type_t *t);

/**
 * crefl_member() - struct or union member by index
 * @t: struct or union type
 * @i: index, 0 to crefl_len() - 1
 *
 * Return: member or NULL if @t is not a struct or union or @i is out
 * of range
 */
const crefl_member_t *crefl_member(const crefl_type_t *t, size_t i);

/**
 * crefl_enumval() - enumerator by index
 * @t: enum type
 * @i: index, 0 to crefl_len() - 1
 *
 * Return: enumerator or NULL if @t is not an enum or @i is out of range
 */
const crefl_enumval_t *crefl_enumval(const crefl_type_t *t, size_t i);

/**
 * crefl_print() - print a type as C declaration with layout comments
 * @t: type
 * @f: output stream
 *
 * Return: 0 or -EINVAL if @t is NULL
 */
int crefl_print(const crefl_type_t *t, FILE *f);

/*
 * instances
 *
 * Functions returning a crefl_val_t signal errors with an invalid
 * value (.t == NULL), which all value functions accept and reject.
 */

/**
 * crefl_new() - allocate a zeroed instance
 * @t: complete type with non-zero size
 *
 * Context: allocates with malloc.
 *
 * Return: value owning its memory, to be released with crefl_free().
 * Invalid if out of memory.
 */
crefl_val_t crefl_new(const crefl_type_t *t);

/**
 * crefl_free() - free an instance from crefl_new()
 * @v: value, may be invalid
 */
void crefl_free(crefl_val_t v);

/**
 * crefl_wrap() - view existing data as a type
 * @t: type
 * @p: data, not owned, must be at least crefl_sizeof(@t) bytes
 *
 * This attaches to memory that already exists, e.g. a struct in your
 * program (&my_cfg) or a received packet. @p needs no alignment and
 * must stay valid as long as the value is used.
 *
 * Return: value, invalid if @t or @p is NULL
 */
crefl_val_t crefl_wrap(const crefl_type_t *t, void *p);

/**
 * crefl_path() - resolve a member path once for fast access
 * @t: root type
 * @expr: C member designator, e.g. "a.b[3].c", "[2].x" for arrays.
 *        Anonymous struct/union members are looked through.
 * @out: resolved path, use with crefl_at()
 *
 * Return: 0 or -ENOENT, -EINVAL, -ERANGE (index)
 */
int crefl_path(const crefl_type_t *t, const char *expr, crefl_path_t *out);

/**
 * crefl_at() - apply a precompiled path to a value
 * @v: value of the root type the path was resolved on
 * @p: path from crefl_path()
 *
 * This takes constant time and does no checks, so @v must be valid and
 * of the root type the path was resolved on.
 *
 * Return: member value
 */
crefl_val_t crefl_at(crefl_val_t v, const crefl_path_t *p);

/**
 * crefl_get() - access a member by path
 * @v: value
 * @expr: member designator, see crefl_path()
 *
 * Same as crefl_path() followed by crefl_at(), but parses @expr on
 * every call. Use crefl_path() in hot paths.
 *
 * Return: member value, invalid on error
 */
crefl_val_t crefl_get(crefl_val_t v, const char *expr);

/**
 * crefl_index() - array element
 * @v: array value
 * @i: index, unbounded for flexible arrays
 *
 * Return: element value, invalid if @v is not an array or @i is out
 * of range
 */
crefl_val_t crefl_index(crefl_val_t v, size_t i);

/**
 * crefl_deref() - follow a pointer
 * @v: pointer value
 *
 * Return: value of the pointer target, invalid if @v is not a pointer
 * or the pointer is NULL
 */
crefl_val_t crefl_deref(crefl_val_t v);

/*
 * scalar access with conversion
 *
 * Works on integers, bool, enums, bitfields and floats, and the u64
 * variants also on pointers. Getters return -ERANGE if the value does
 * not fit @out and setters if @x does not fit the type. Both return
 * -EINVAL on a type mismatch or an invalid @v.
 */

/**
 * crefl_get_i64() - read a scalar as int64_t
 * @v: value
 * @out: result
 *
 * Return: 0, -EINVAL or -ERANGE
 */
int crefl_get_i64(crefl_val_t v, int64_t *out);

/**
 * crefl_get_u64() - read a scalar or pointer as uint64_t
 * @v: value
 * @out: result
 *
 * Return: 0, -EINVAL or -ERANGE (negative value)
 */
int crefl_get_u64(crefl_val_t v, uint64_t *out);

/**
 * crefl_get_f64() - read a scalar as double
 * @v: value
 * @out: result, large integers are rounded
 *
 * Return: 0 or -EINVAL
 */
int crefl_get_f64(crefl_val_t v, double *out);

/**
 * crefl_set_i64() - write an int64_t to a scalar
 * @v: value
 * @x: new value
 *
 * Return: 0, -EINVAL or -ERANGE
 */
int crefl_set_i64(crefl_val_t v, int64_t x);

/**
 * crefl_set_u64() - write a uint64_t to a scalar or pointer
 * @v: value
 * @x: new value
 *
 * Return: 0, -EINVAL or -ERANGE
 */
int crefl_set_u64(crefl_val_t v, uint64_t x);

/**
 * crefl_set_f64() - write a double to a scalar
 * @v: value
 * @x: new value, integer targets accept only exact integral values
 *
 * Return: 0, -EINVAL or -ERANGE
 */
int crefl_set_f64(crefl_val_t v, double x);

/**
 * crefl_set_str() - copy a string into a char array
 * @v: char array value
 * @s: string, copied incl. NUL, the rest of the array is zeroed
 *
 * Return: 0, -EINVAL (not a char array) or -ERANGE if @s does not fit
 */
int crefl_set_str(crefl_val_t v, const char *s);

/**
 * crefl_copy() - copy a value
 * @dst: destination
 * @src: source of the same type after resolving typedefs
 *
 * Overlapping values and bitfields are handled.
 *
 * Return: 0 or -EINVAL (type mismatch, invalid value)
 */
int crefl_copy(crefl_val_t dst, crefl_val_t src);

#endif
