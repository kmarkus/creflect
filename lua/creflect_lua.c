// SPDX-License-Identifier: LGPL-2.0-or-later
// Copyright (C) 2019-2026 Markus Klotzbuecher <mk@mkio.de>

/*
 * Lua bindings
 *
 * Objects keep their owner alive through a weak keyed registry table:
 * type -> ctx, value -> type or parent value. Only contexts and
 * values created by new() own memory.
 *
 * All functions get the three metatables as upvalues 1-3, so type
 * checks are a pointer compare instead of a registry lookup by name.
 */

#define _POSIX_C_SOURCE 200809L /* open_memstream */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#if LUA_VERSION_NUM < 503
#include "compat-5.3.h"
#endif

#include "crefl.h"

#ifndef LUA_MAXINTEGER
#define LUA_MAXINTEGER PTRDIFF_MAX
#endif
#ifndef LUA_MININTEGER
#define LUA_MININTEGER PTRDIFF_MIN
#endif

#define CTX_MT "creflect.ctx"
#define TYPE_MT "creflect.type"
#define VAL_MT "creflect.val"

#define UP_CTX lua_upvalueindex(1)
#define UP_TYPE lua_upvalueindex(2)
#define UP_VAL lua_upvalueindex(3)
#define NUP 3

struct lctx {
	crefl_ctx_t *ctx;
};

struct ltype {
	const crefl_type_t *t;
};

struct lval {
	crefl_val_t v;
	int owned;
};

static char owners_key;

static const char *const kind_names[] = {
	[CREFL_PRIM] = "prim",	     [CREFL_PTR] = "ptr",
	[CREFL_ARRAY] = "array",     [CREFL_STRUCT] = "struct",
	[CREFL_UNION] = "union",     [CREFL_ENUM] = "enum",
	[CREFL_TYPEDEF] = "typedef",
};

static int fail(lua_State *L, int ret, const char *what)
{
	const char *msg;

	switch (ret) {
	case -ERANGE:
		msg = "out of range";
		break;
	case -ENOENT:
		msg = "no such member";
		break;
	case -ENOMEM:
		msg = "out of memory";
		break;
	default:
		msg = "invalid access or type mismatch";
	}
	return luaL_error(L, "creflect: %s: %s", what, msg);
}

static void set_mt(lua_State *L, int up)
{
	lua_pushvalue(L, up);
	lua_setmetatable(L, -2);
}

/* userdata at @i with the metatable in upvalue @up, or NULL */
static void *test_ud(lua_State *L, int i, int up)
{
	void *p = lua_touserdata(L, i);
	int ok;

	if (p == NULL || !lua_getmetatable(L, i))
		return NULL;
	ok = lua_rawequal(L, -1, up);
	lua_pop(L, 1);
	return ok ? p : NULL;
}

/* the slow path only raises the usual error */
static void *check_ud(lua_State *L, int i, int up, const char *name)
{
	void *p = test_ud(L, i, up);

	return p ? p : luaL_checkudata(L, i, name);
}

/* keep the object at @owner alive as long as the object on top */
static void set_owner(lua_State *L, int owner)
{
	owner = lua_absindex(L, owner);
	lua_rawgetp(L, LUA_REGISTRYINDEX, &owners_key);
	lua_pushvalue(L, -2);
	lua_pushvalue(L, owner);
	lua_rawset(L, -3);
	lua_pop(L, 1);
}

static void push_type(lua_State *L, const crefl_type_t *t, int owner)
{
	struct ltype *lt;

	if (t == NULL) {
		lua_pushnil(L);
		return;
	}
	owner = lua_absindex(L, owner);
	lt = lua_newuserdata(L, sizeof(*lt));
	lt->t = t;
	set_mt(L, UP_TYPE);
	set_owner(L, owner);
}

static void push_view(lua_State *L, crefl_val_t v, int owner)
{
	struct lval *lv;

	owner = lua_absindex(L, owner);
	lv = lua_newuserdata(L, sizeof(*lv));
	lv->v = v;
	lv->owned = 0;
	set_mt(L, UP_VAL);
	set_owner(L, owner);
}

static const crefl_type_t *check_type(lua_State *L, int i)
{
	return ((struct ltype *)check_ud(L, i, UP_TYPE, TYPE_MT))->t;
}

static struct lval *check_val(lua_State *L, int i)
{
	return check_ud(L, i, UP_VAL, VAL_MT);
}

/* no close(): types and values point into the context */
static crefl_ctx_t *check_ctx(lua_State *L, int i)
{
	return ((struct lctx *)check_ud(L, i, UP_CTX, CTX_MT))->ctx;
}

/*
 * lua_Integer is only 32 bit with LuaJIT and Lua < 5.3 on 32 bit
 * targets: wider values are pushed as floats, and numbers are converted
 * from the float itself, as there all numbers are floats anyway.
 */
static void push_i64(lua_State *L, int64_t i)
{
	if (i >= LUA_MININTEGER && i <= LUA_MAXINTEGER)
		lua_pushinteger(L, (lua_Integer)i);
	else
		lua_pushnumber(L, (lua_Number)i);
}

static void push_u64(lua_State *L, uint64_t u)
{
	if (u <= (uint64_t)LUA_MAXINTEGER)
		lua_pushinteger(L, (lua_Integer)u);
	else
		lua_pushnumber(L, (lua_Number)u);
}

/* integer value of the number at @idx, 0 if it has none */
static int to_i64(lua_State *L, int idx, int64_t *out)
{
#if LUA_VERSION_NUM >= 503
	if (!lua_isinteger(L, idx))
		return 0;
	*out = (int64_t)lua_tointeger(L, idx);
	return 1;
#else
	lua_Number n = lua_tonumber(L, idx);

	if (!(n >= -0x1p63 && n < 0x1p63) || (lua_Number)(int64_t)n != n)
		return 0;
	*out = (int64_t)n;
	return 1;
#endif
}

/* plain char arrays read as strings, (un)signed char arrays as bytes */
static int is_string(const crefl_type_t *t)
{
	const crefl_type_t *e;

	if (crefl_kind(t) != CREFL_ARRAY)
		return 0;
	e = crefl_resolve(crefl_elem(t));
	return crefl_kind(e) == CREFL_PRIM && crefl_prim(e) == CREFL_CHAR;
}

/* push scalars and strings, return 0 for composites */
static int push_scalar(lua_State *L, crefl_val_t v, const char *what)
{
	const crefl_type_t *t = crefl_resolve(v.t);
	int64_t i;
	uint64_t u;
	double d;
	int ret;

	switch (crefl_kind(t)) {
	case CREFL_ARRAY:
		if (!is_string(t))
			return 0;
		lua_pushlstring(L, v.p, strnlen(v.p, crefl_len(t)));
		return 1;
	case CREFL_STRUCT:
	case CREFL_UNION:
		return 0;
	case CREFL_PRIM:
		if (crefl_prim(t) >= CREFL_FLOAT) {
			ret = crefl_get_f64(v, &d);
			if (ret)
				fail(L, ret, what);
			lua_pushnumber(L, d);
			return 1;
		}
		if (crefl_prim(t) == CREFL_BOOL) {
			ret = crefl_get_i64(v, &i);
			if (ret)
				fail(L, ret, what);
			lua_pushboolean(L, i != 0);
			return 1;
		}
		break;
	default:
		break;
	}

	if (crefl_is_signed(t)) {
		ret = crefl_get_i64(v, &i);
		if (ret)
			fail(L, ret, what);
		push_i64(L, i);
		return 1;
	}

	ret = crefl_get_u64(v, &u);
	if (ret)
		fail(L, ret, what);
	push_u64(L, u);
	return 1;
}

static void push_val(lua_State *L, crefl_val_t v, int owner, const char *what)
{
	if (!push_scalar(L, v, what))
		push_view(L, v, owner);
}

static void push_deep(lua_State *L, crefl_val_t v, const char *what);

/* anonymous struct/union members are flattened into the parent */
static void push_members(lua_State *L, crefl_val_t v, const crefl_type_t *t)
{
	for (size_t i = 0; i < crefl_len(t); i++) {
		const crefl_member_t *m = crefl_member(t, i);
		crefl_val_t mv = {
			.t = m->type,
			.p = (char *)v.p + m->off,
			.bit_off = m->bit_off,
			.bit_width = m->bit_width,
		};
		const crefl_type_t *mt = crefl_resolve(m->type);

		if (m->name) {
			push_deep(L, mv, m->name);
			lua_setfield(L, -2, m->name);
		} else if (crefl_kind(mt) == CREFL_STRUCT ||
			   crefl_kind(mt) == CREFL_UNION) {
			push_members(L, mv, mt);
		}
	}
}

/* enums by name if the value has one, as fromlua() accepts them */
static int push_enum_name(lua_State *L, crefl_val_t v, const crefl_type_t *t)
{
	int64_t i;

	if (crefl_get_i64(v, &i) && crefl_get_u64(v, (uint64_t *)&i))
		return 0;
	for (size_t n = 0; n < crefl_len(t); n++) {
		if (crefl_enumval(t, n)->val == i) {
			lua_pushstring(L, crefl_enumval(t, n)->name);
			return 1;
		}
	}
	return 0;
}

static void push_deep(lua_State *L, crefl_val_t v, const char *what)
{
	const crefl_type_t *t = crefl_resolve(v.t);

	luaL_checkstack(L, 4, "creflect: nesting too deep");
	if (crefl_kind(t) == CREFL_ENUM && push_enum_name(L, v, t))
		return;
	if (push_scalar(L, v, what))
		return;

	if (crefl_kind(t) == CREFL_ARRAY) {
		lua_createtable(L, (int)crefl_len(t), 0);
		for (size_t i = 0; i < crefl_len(t); i++) {
			push_deep(L, crefl_index(v, i), what);
			lua_rawseti(L, -2, (lua_Integer)i + 1);
		}
		return;
	}

	lua_createtable(L, 0, (int)crefl_len(t));
	push_members(L, v, t);
}

static int enum_lookup(const crefl_type_t *t, const char *name, int64_t *val)
{
	for (size_t i = 0; i < crefl_len(t); i++) {
		const crefl_enumval_t *e = crefl_enumval(t, i);

		if (strcmp(e->name, name) == 0) {
			*val = e->val;
			return 0;
		}
	}
	return -ENOENT;
}

static void assign(lua_State *L, crefl_val_t v, int idx, const char *what);

static void assign_table(lua_State *L, crefl_val_t v, int idx, const char *what)
{
	const crefl_type_t *t = crefl_resolve(v.t);
	size_t n;

	if (crefl_kind(t) == CREFL_ARRAY) {
		n = lua_rawlen(L, idx);
		if (crefl_len(t) == 0 || n > crefl_len(t))
			fail(L, -ERANGE, what);
		for (size_t i = 0; i < n; i++) {
			lua_rawgeti(L, idx, (lua_Integer)i + 1);
			assign(L, crefl_index(v, i), -1, what);
			lua_pop(L, 1);
		}
		return;
	}

	if (crefl_kind(t) != CREFL_STRUCT && crefl_kind(t) != CREFL_UNION)
		fail(L, -EINVAL, what);

	lua_pushnil(L);
	while (lua_next(L, idx)) {
		const char *k;
		crefl_path_t p;
		int ret;

		if (lua_type(L, -2) != LUA_TSTRING)
			luaL_error(L, "creflect: %s: non-string key", what);
		k = lua_tostring(L, -2);
		ret = crefl_path(v.t, k, &p);
		if (ret)
			fail(L, ret, k);
		assign(L, crefl_at(v, &p), -1, k);
		lua_pop(L, 1);
	}
}

static void assign(lua_State *L, crefl_val_t v, int idx, const char *what)
{
	const crefl_type_t *t = crefl_resolve(v.t);
	struct lval *src;
	size_t len;
	int64_t i;
	int ret;

	idx = lua_absindex(L, idx);
	luaL_checkstack(L, 4, "creflect: nesting too deep");

	switch (lua_type(L, idx)) {
	case LUA_TNUMBER:
		if (!to_i64(L, idx, &i))
			ret = crefl_set_f64(v, (double)lua_tonumber(L, idx));
		else if (crefl_kind(t) == CREFL_PTR)
			ret = crefl_set_u64(v, (uint64_t)i);
		else
			ret = crefl_set_i64(v, i);
		break;
	case LUA_TBOOLEAN:
		ret = crefl_set_i64(v, lua_toboolean(L, idx));
		break;
	case LUA_TNIL:
		ret = crefl_kind(t) == CREFL_PTR ? crefl_set_u64(v, 0) :
						   -EINVAL;
		break;
	case LUA_TSTRING:
		if (crefl_kind(t) != CREFL_ENUM) {
			const char *str = lua_tolstring(L, idx, &len);

			/* C strings end at the first NUL, do not cut silently */
			if (strlen(str) != len)
				luaL_error(L,
					   "creflect: %s: string contains NUL",
					   what);
			ret = crefl_set_str(v, str);
			break;
		}
		ret = enum_lookup(t, lua_tostring(L, idx), &i);
		if (ret) {
			luaL_error(L, "creflect: %s: no enumerator '%s'", what,
				   lua_tostring(L, idx));
		}
		ret = crefl_is_signed(t) ? crefl_set_i64(v, i) :
					   crefl_set_u64(v, (uint64_t)i);
		break;
	case LUA_TTABLE:
		assign_table(L, v, idx, what);
		return;
	case LUA_TUSERDATA:
		src = test_ud(L, idx, UP_VAL);
		ret = src ? crefl_copy(v, src->v) : -EINVAL;
		break;
	default:
		ret = -EINVAL;
	}
	if (ret)
		fail(L, ret, what);
}

static void new_val(lua_State *L, const crefl_type_t *t, int owner, int init)
{
	struct lval *lv;

	if (!crefl_is_complete(t) || crefl_sizeof(t) == 0)
		luaL_error(L, "creflect: cannot instantiate incomplete type");

	owner = lua_absindex(L, owner);
	lv = lua_newuserdata(L, sizeof(*lv));
	memset(lv, 0, sizeof(*lv));
	set_mt(L, UP_VAL);
	set_owner(L, owner);

	lv->v = crefl_new(t);
	if (lv->v.t == NULL)
		fail(L, -ENOMEM, "new");
	lv->owned = 1;

	if (!lua_isnoneornil(L, init))
		assign(L, lv->v, init, "init");
}

/*
 * context
 */
static int l_new(lua_State *L)
{
	struct lctx *lc = lua_newuserdata(L, sizeof(*lc));

	lc->ctx = NULL;
	set_mt(L, UP_CTX);
	lc->ctx = crefl_ctx_new();
	if (lc->ctx == NULL)
		return fail(L, -ENOMEM, "new");
	return 1;
}

static int ctx_gc(lua_State *L)
{
	struct lctx *lc = check_ud(L, 1, UP_CTX, CTX_MT);

	crefl_ctx_free(lc->ctx);
	lc->ctx = NULL;
	return 0;
}

static int ctx_parse(lua_State *L)
{
	crefl_ctx_t *ctx = check_ctx(L, 1);

	if (crefl_parse(ctx, luaL_checkstring(L, 2))) {
		lua_pushnil(L);
		lua_pushstring(L, crefl_errmsg(ctx));
		return 2;
	}
	lua_pushboolean(L, 1);
	return 1;
}

static int ctx_define(lua_State *L)
{
	crefl_ctx_t *ctx = check_ctx(L, 1);
	const char *name = luaL_checkstring(L, 2);
	int64_t val;

	luaL_checknumber(L, 3);
	if (!to_i64(L, 3, &val))
		return luaL_argerror(L, 3, "integer expected");
	if (crefl_define(ctx, name, val)) {
		lua_pushnil(L);
		lua_pushstring(L, crefl_errmsg(ctx));
		return 2;
	}
	lua_pushboolean(L, 1);
	return 1;
}

static int ctx_type(lua_State *L)
{
	crefl_ctx_t *ctx = check_ctx(L, 1);
	const crefl_type_t *t = crefl_type(ctx, luaL_checkstring(L, 2));

	if (t == NULL) {
		lua_pushnil(L);
		lua_pushstring(L, crefl_errmsg(ctx));
		return 2;
	}
	push_type(L, t, 1);
	return 1;
}

static int ctx_types(lua_State *L)
{
	crefl_ctx_t *ctx = check_ctx(L, 1);
	size_t n = crefl_ntypes(ctx);

	lua_createtable(L, (int)n, 0);
	for (size_t i = 0; i < n; i++) {
		push_type(L, crefl_type_at(ctx, i), 1);
		lua_rawseti(L, -2, (lua_Integer)i + 1);
	}
	return 1;
}

/* ctx:new(decl or type, [init]) */
static int ctx_new(lua_State *L)
{
	crefl_ctx_t *ctx = check_ctx(L, 1);
	const crefl_type_t *t;

	if (lua_type(L, 2) == LUA_TSTRING) {
		t = crefl_type(ctx, lua_tostring(L, 2));
		if (t == NULL)
			return luaL_error(L, "creflect: %s", crefl_errmsg(ctx));
		push_type(L, t, 1);
		lua_replace(L, 2);
	}
	check_type(L, 2);
	new_val(L, check_type(L, 2), 2, 3);
	return 1;
}

/*
 * types
 */
static int type_kind(lua_State *L)
{
	lua_pushstring(L, kind_names[crefl_kind(check_type(L, 1))]);
	return 1;
}

static int type_name(lua_State *L)
{
	const char *n = crefl_name(check_type(L, 1));

	if (n == NULL)
		return 0;
	lua_pushstring(L, n);
	return 1;
}

static int type_sizeof(lua_State *L)
{
	lua_pushinteger(L, (lua_Integer)crefl_sizeof(check_type(L, 1)));
	return 1;
}

static int type_alignof(lua_State *L)
{
	lua_pushinteger(L, (lua_Integer)crefl_alignof(check_type(L, 1)));
	return 1;
}

static int type_len(lua_State *L)
{
	lua_pushinteger(L, (lua_Integer)crefl_len(check_type(L, 1)));
	return 1;
}

static int type_is_complete(lua_State *L)
{
	lua_pushboolean(L, crefl_is_complete(check_type(L, 1)));
	return 1;
}

static int type_is_packed(lua_State *L)
{
	lua_pushboolean(L, crefl_is_packed(check_type(L, 1)));
	return 1;
}

static int type_is_signed(lua_State *L)
{
	lua_pushboolean(L, crefl_is_signed(check_type(L, 1)));
	return 1;
}

static int type_elem(lua_State *L)
{
	push_type(L, crefl_elem(check_type(L, 1)), 1);
	return 1;
}

static int type_resolve(lua_State *L)
{
	push_type(L, crefl_resolve(check_type(L, 1)), 1);
	return 1;
}

static int type_members(lua_State *L)
{
	const crefl_type_t *t = crefl_resolve(check_type(L, 1));
	size_t n;

	if (crefl_kind(t) != CREFL_STRUCT && crefl_kind(t) != CREFL_UNION)
		return luaL_error(L, "creflect: not a struct or union");

	n = crefl_len(t);
	lua_createtable(L, (int)n, 0);
	for (size_t i = 0; i < n; i++) {
		const crefl_member_t *m = crefl_member(t, i);

		lua_createtable(L, 0, 5);
		if (m->name) {
			lua_pushstring(L, m->name);
			lua_setfield(L, -2, "name");
		}
		push_type(L, m->type, 1);
		lua_setfield(L, -2, "type");
		lua_pushinteger(L, (lua_Integer)m->off);
		lua_setfield(L, -2, "off");
		lua_pushinteger(L, m->bit_off);
		lua_setfield(L, -2, "bit_off");
		lua_pushinteger(L, m->bit_width);
		lua_setfield(L, -2, "bit_width");
		lua_rawseti(L, -2, (lua_Integer)i + 1);
	}
	return 1;
}

static int type_enums(lua_State *L)
{
	const crefl_type_t *t = crefl_resolve(check_type(L, 1));
	size_t n;

	if (crefl_kind(t) != CREFL_ENUM)
		return luaL_error(L, "creflect: not an enum");

	n = crefl_len(t);
	lua_createtable(L, (int)n, 0);
	for (size_t i = 0; i < n; i++) {
		const crefl_enumval_t *e = crefl_enumval(t, i);
		uint64_t u = (uint64_t)e->val;

		lua_createtable(L, 0, 2);
		lua_pushstring(L, e->name);
		lua_setfield(L, -2, "name");
		if (crefl_is_signed(t))
			push_i64(L, e->val);
		else
			push_u64(L, u);
		lua_setfield(L, -2, "val");
		lua_rawseti(L, -2, (lua_Integer)i + 1);
	}
	return 1;
}

static int type_new(lua_State *L)
{
	new_val(L, check_type(L, 1), 1, 2);
	return 1;
}

/* view on memory owned by C, unchecked: the caller vouches for ptr */
static int type_wrap(lua_State *L)
{
	const crefl_type_t *t = check_type(L, 1);

	luaL_checktype(L, 2, LUA_TLIGHTUSERDATA);
	if (!crefl_is_complete(t) || crefl_sizeof(t) == 0)
		return luaL_error(L, "creflect: cannot wrap incomplete type");
	if (lua_touserdata(L, 2) == NULL)
		return luaL_error(L, "creflect: cannot wrap NULL");
	push_view(L, crefl_wrap(t, lua_touserdata(L, 2)), 1);
	return 1;
}

/* C declaration with layout comments */
static int type_tostring(lua_State *L)
{
	const crefl_type_t *t = check_type(L, 1);
	char *buf = NULL;
	size_t len = 0;
	FILE *f = open_memstream(&buf, &len);

	if (f == NULL)
		return fail(L, -ENOMEM, "tostring");
	crefl_print(t, f);
	if (fclose(f)) {
		free(buf);
		return fail(L, -ENOMEM, "tostring");
	}
	lua_pushlstring(L, buf, len);
	free(buf);
	return 1;
}

static int type_eq(lua_State *L)
{
	lua_pushboolean(L, check_type(L, 1) == check_type(L, 2));
	return 1;
}

/*
 * values
 */

/* keep "[n]" paths through flexible arrays inside the object */
static int path_inside(const crefl_type_t *t, const crefl_path_t *p)
{
	size_t end;

	if (p->bit_width)
		end = (p->off * 8 + p->bit_off + p->bit_width + 7) / 8;
	else
		end = p->off + crefl_sizeof(p->t);
	return end <= crefl_sizeof(t);
}

/* v[i] (0-based) or v["a.b[2]"] */
static crefl_val_t val_lookup(lua_State *L, struct lval *lv, int k)
{
	const crefl_type_t *t = crefl_resolve(lv->v.t);
	crefl_path_t p;
	lua_Integer i;
	int ret;

	if (lua_type(L, k) == LUA_TNUMBER) {
		i = luaL_checkinteger(L, k);
		if (crefl_kind(t) != CREFL_ARRAY)
			luaL_error(L, "creflect: [%d]: not an array", (int)i);
		/* flexible arrays are unbounded, refuse them */
		if (i < 0 || (size_t)i >= crefl_len(t))
			luaL_error(L, "creflect: [%d]: out of range", (int)i);
		return crefl_index(lv->v, (size_t)i);
	}

	const char *s = luaL_checkstring(L, k);

	ret = crefl_path(lv->v.t, s, &p);
	if (ret)
		fail(L, ret, s);

	if (!path_inside(lv->v.t, &p))
		fail(L, -ERANGE, s);

	return crefl_at(lv->v, &p);
}

static int val_index(lua_State *L)
{
	struct lval *lv = check_val(L, 1);
	crefl_val_t v = val_lookup(L, lv, 2);

	push_val(L, v, 1, lua_tostring(L, 2));
	return 1;
}

static int val_newindex(lua_State *L)
{
	struct lval *lv = check_val(L, 1);
	crefl_val_t v = val_lookup(L, lv, 2);

	assign(L, v, 3, lua_tostring(L, 2));
	return 0;
}

static int val_len(lua_State *L)
{
	const crefl_type_t *t = crefl_resolve(check_val(L, 1)->v.t);

	if (crefl_kind(t) != CREFL_ARRAY)
		return luaL_error(L, "creflect: not an array");
	lua_pushinteger(L, (lua_Integer)crefl_len(t));
	return 1;
}

static int array_next(lua_State *L)
{
	struct lval *lv = check_val(L, 1);
	lua_Integer i = lua_isnil(L, 2) ? 0 : luaL_checkinteger(L, 2) + 1;

	if ((size_t)i >= crefl_len(crefl_resolve(lv->v.t)))
		return 0;
	lua_pushinteger(L, i);
	push_val(L, crefl_index(lv->v, (size_t)i), 1, "pairs");
	return 2;
}

/* upvalues: metatables, member names, position */
static int struct_next(lua_State *L)
{
	struct lval *lv = check_val(L, 1);
	lua_Integer i = lua_tointeger(L, lua_upvalueindex(NUP + 2)) + 1;
	crefl_val_t v;

	lua_rawgeti(L, lua_upvalueindex(NUP + 1), i);
	if (lua_isnil(L, -1))
		return 0;
	lua_pushinteger(L, i);
	lua_replace(L, lua_upvalueindex(NUP + 2));
	v = val_lookup(L, lv, lua_gettop(L));
	push_val(L, v, 1, lua_tostring(L, -1));
	return 2;
}

/* names in declaration order, anonymous members flattened */
static void push_names(lua_State *L, const crefl_type_t *t)
{
	for (size_t i = 0; i < crefl_len(t); i++) {
		const crefl_member_t *m = crefl_member(t, i);
		const crefl_type_t *mt = crefl_resolve(m->type);

		if (m->name) {
			lua_pushstring(L, m->name);
			lua_rawseti(L, -2, (lua_Integer)lua_rawlen(L, -2) + 1);
		} else if (crefl_kind(mt) == CREFL_STRUCT ||
			   crefl_kind(mt) == CREFL_UNION) {
			push_names(L, mt);
		}
	}
}

static void push_mts(lua_State *L)
{
	lua_pushvalue(L, UP_CTX);
	lua_pushvalue(L, UP_TYPE);
	lua_pushvalue(L, UP_VAL);
}

/* members by name, arrays by 0-based index */
static int val_pairs(lua_State *L)
{
	const crefl_type_t *t = crefl_resolve(check_val(L, 1)->v.t);

	push_mts(L);
	if (crefl_kind(t) == CREFL_ARRAY) {
		lua_pushcclosure(L, array_next, NUP);
	} else {
		lua_createtable(L, (int)crefl_len(t), 0);
		push_names(L, t);
		lua_pushinteger(L, 0);
		lua_pushcclosure(L, struct_next, NUP + 2);
	}
	lua_pushvalue(L, 1);
	lua_pushnil(L);
	return 3;
}

static int val_tostring(lua_State *L)
{
	struct lval *lv = check_val(L, 1);
	const char *n = crefl_name(lv->v.t);

	lua_pushfstring(L, "creflect.val<%s>: %p",
			n ? n : kind_names[crefl_kind(lv->v.t)], lv->v.p);
	return 1;
}

static int val_gc(lua_State *L)
{
	struct lval *lv = check_val(L, 1);

	if (lv->owned)
		crefl_free(lv->v);
	lv->owned = 0;
	return 0;
}

/*
 * module functions, kept off the values to not shadow members
 */
static int l_typeof(lua_State *L)
{
	push_type(L, check_val(L, 1)->v.t, 1);
	return 1;
}

static int l_tolua(lua_State *L)
{
	push_deep(L, check_val(L, 1)->v, "tolua");
	return 1;
}

static int l_fromlua(lua_State *L)
{
	assign(L, check_val(L, 1)->v, 2, "fromlua");
	return 0;
}

/*
 * t:reader{paths}: paths resolved once, then rd(ptr or value) returns
 * the values in order, rd(ptr or value, tbl) sets tbl[path] = value.
 * Scalars as from v.x, composites as from tolua().
 * upvalues: metatables, struct rdr, path strings, type (owner)
 */
struct rdr {
	const crefl_type_t *t;
	int n;
	crefl_path_t p[];
};

static void rdr_push(lua_State *L, const struct rdr *r, void *base, int i)
{
	crefl_val_t v = crefl_at(crefl_wrap(r->t, base), &r->p[i]);

	if (!push_scalar(L, v, "reader"))
		push_deep(L, v, "reader");
}

static int rdr_call(lua_State *L)
{
	const struct rdr *r = lua_touserdata(L, lua_upvalueindex(NUP + 1));
	struct lval *lv;
	void *base;

	if (lua_type(L, 1) == LUA_TLIGHTUSERDATA) {
		/* unchecked like wrap() */
		base = lua_touserdata(L, 1);
		if (base == NULL)
			return luaL_error(L, "creflect: reader: NULL");
	} else {
		lv = check_val(L, 1);
		if (crefl_resolve(lv->v.t) != crefl_resolve(r->t))
			return luaL_error(L, "creflect: reader: type mismatch");
		base = lv->v.p;
	}

	if (lua_istable(L, 2)) {
		for (int i = 0; i < r->n; i++) {
			lua_rawgeti(L, lua_upvalueindex(NUP + 2), i + 1);
			rdr_push(L, r, base, i);
			lua_rawset(L, 2);
		}
		lua_settop(L, 2);
		return 1;
	}

	luaL_checkstack(L, r->n, "creflect: reader: too many paths");
	for (int i = 0; i < r->n; i++)
		rdr_push(L, r, base, i);
	return r->n;
}

static int type_reader(lua_State *L)
{
	const crefl_type_t *t = check_type(L, 1);
	struct rdr *r;
	const char *s;
	size_t n;
	int ret;

	luaL_checktype(L, 2, LUA_TTABLE);
	if (!crefl_is_complete(t) || crefl_sizeof(t) == 0)
		return luaL_error(L, "creflect: cannot read incomplete type");
	n = lua_rawlen(L, 2);
	if (n > INT32_MAX / sizeof(crefl_path_t))
		return fail(L, -ERANGE, "reader");

	push_mts(L);
	r = lua_newuserdata(L, sizeof(*r) + n * sizeof(r->p[0]));
	r->t = t;
	r->n = (int)n;
	lua_createtable(L, (int)n, 0);
	for (size_t i = 0; i < n; i++) {
		lua_rawgeti(L, 2, (lua_Integer)i + 1);
		if (lua_type(L, -1) != LUA_TSTRING)
			return luaL_error(
				L, "creflect: reader: path %d: not a string",
				(int)i + 1);
		s = lua_tostring(L, -1);
		ret = crefl_path(t, s, &r->p[i]);
		if (ret == 0 && !path_inside(t, &r->p[i]))
			ret = -ERANGE;
		if (ret)
			return fail(L, ret, s);
		lua_rawseti(L, -2, (lua_Integer)i + 1);
	}
	lua_pushvalue(L, 1);
	lua_pushcclosure(L, rdr_call, NUP + 3);
	return 1;
}

static const luaL_Reg ctx_m[] = {
	{ "parse", ctx_parse }, { "define", ctx_define }, { "type", ctx_type },
	{ "types", ctx_types }, { "new", ctx_new },	  { NULL, NULL },
};

static const luaL_Reg type_m[] = {
	{ "kind", type_kind },
	{ "name", type_name },
	{ "sizeof", type_sizeof },
	{ "alignof", type_alignof },
	{ "len", type_len },
	{ "is_complete", type_is_complete },
	{ "is_packed", type_is_packed },
	{ "is_signed", type_is_signed },
	{ "elem", type_elem },
	{ "resolve", type_resolve },
	{ "members", type_members },
	{ "enums", type_enums },
	{ "new", type_new },
	{ "wrap", type_wrap },
	{ "reader", type_reader },
	{ NULL, NULL },
};

static const luaL_Reg val_mm[] = {
	{ "__index", val_index }, { "__newindex", val_newindex },
	{ "__len", val_len },	  { "__tostring", val_tostring },
	{ "__gc", val_gc },	  { "__pairs", val_pairs },
	{ NULL, NULL },
};

static int l_addr(lua_State *L)
{
	lua_pushlightuserdata(L, check_val(L, 1)->v.p);
	return 1;
}

static int l_tobytes(lua_State *L)
{
	struct lval *lv = check_val(L, 1);

	lua_pushlstring(L, lv->v.p, crefl_sizeof(lv->v.t));
	return 1;
}

static int l_frombytes(lua_State *L)
{
	struct lval *lv = check_val(L, 1);
	size_t len;
	const char *s = luaL_checklstring(L, 2, &len);

	if (len != crefl_sizeof(lv->v.t))
		return luaL_error(L,
				  "creflect: frombytes: %d bytes, expected %d",
				  (int)len, (int)crefl_sizeof(lv->v.t));
	memcpy(lv->v.p, s, len);
	return 0;
}

static const luaL_Reg funcs[] = {
	{ "new", l_new },
	{ "typeof", l_typeof },
	{ "tolua", l_tolua },
	{ "fromlua", l_fromlua },
	{ "addr", l_addr },
	{ "tobytes", l_tobytes },
	{ "frombytes", l_frombytes },
	{ NULL, NULL },
};

static const luaL_Reg ctx_mm[] = {
	{ "__gc", ctx_gc },
	{ NULL, NULL },
};

static const luaL_Reg type_mm[] = {
	{ "__tostring", type_tostring },
	{ "__eq", type_eq },
	{ NULL, NULL },
};

/* set @f into the table at @t, metatables at @mt..@mt + 2 as upvalues */
static void set_funcs(lua_State *L, int t, int mt, const luaL_Reg *f)
{
	lua_pushvalue(L, t);
	for (int i = 0; i < NUP; i++)
		lua_pushvalue(L, mt + i);
	luaL_setfuncs(L, f, NUP);
	lua_pop(L, 1);
}

/* methods in __index of the metatable at @t */
static void set_methods(lua_State *L, int t, int mt, const luaL_Reg *f)
{
	lua_newtable(L);
	set_funcs(L, lua_gettop(L), mt, f);
	lua_setfield(L, t, "__index");
}

__attribute__((visibility("default"))) int luaopen_creflect(lua_State *L)
{
	int mt;

	lua_newtable(L);
	lua_createtable(L, 0, 1);
	lua_pushstring(L, "k");
	lua_setfield(L, -2, "__mode");
	lua_setmetatable(L, -2);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &owners_key);

	/* in upvalue order */
	luaL_newmetatable(L, CTX_MT);
	mt = lua_gettop(L);
	luaL_newmetatable(L, TYPE_MT);
	luaL_newmetatable(L, VAL_MT);

	set_methods(L, mt, mt, ctx_m);
	set_funcs(L, mt, mt, ctx_mm);
	set_methods(L, mt + 1, mt, type_m);
	set_funcs(L, mt + 1, mt, type_mm);
	set_funcs(L, mt + 2, mt, val_mm);

	lua_createtable(L, 0, sizeof(funcs) / sizeof(funcs[0]) - 1);
	set_funcs(L, lua_gettop(L), mt, funcs);
	return 1;
}
