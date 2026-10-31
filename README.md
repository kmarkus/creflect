<div align="center">

# libcreflect: reflective C data types

[![pipeline status](https://gitlab.com/kmarkus/libcreflect/badges/main/pipeline.svg)](https://gitlab.com/kmarkus/libcreflect/-/pipelines)

**[Supported](#supported) · [Build and test](#build-and-test) · [Usage](#usage) · [crefl-tool](#crefl-tool) · [Lua](#lua) · [License](#license)**

</div>

libcreflect reads C type declarations at runtime and gives
generic access to data of those types, without code generation or
compiler support. It is plain C11 and needs nothing beyond libc.

- **Parse** structs, unions, enums, typedefs, bitfields and arrays
  from strings or headers (`crefl_parse`).
- **Lay out** types like the compiler: size, alignment, member and
  bit offsets for the host ABI. This is checked against gcc and
  clang on x86-64, i386, armhf, arm64 and s390x.
- **Access** data: create zeroed instances or wrap existing memory
  (`crefl_wrap`), then read and write members by path, e.g.
  `"range[1].hi"`. Setters are range-checked (`-ERANGE`).
- **Introspect** types, members and enumerators, and print types
  back as C with pahole-style layout comments (holes, padding).
- **crefl-tool**: list and query the types in headers, decode
  binary records from the command line.
- **Lua bindings**: values behave like Lua tables, enums take
  names, and whole values convert to and from plain Lua data.
- **No malloc** if needed: `crefl_ctx_new_static(buf, len)`.

Use cases: generic tools for C data (e.g. a fast microblx `ubx`),
filling structs from config files or JSON, dumping and decoding
binary data, checking struct layouts for holes.

Names: header `crefl.h`, prefix `crefl_`, library `-lcreflect`
(`pkg-config creflect`), tool `crefl-tool`, Lua `require("creflect")`.

## Supported

- primitive types, `stdint.h`/`stddef.h` types, `bool`
- structs, unions, enums, typedefs
- multi-dimensional arrays, pointers (function pointers as `void *`)
- bitfields, incl. packed and zero width
- anonymous and nested struct/union definitions
- `__attribute__((packed))`, `aligned(n)` on structs and members
- integer constant expressions in array sizes, enum values and
  bitfield widths with C integer types (`+ - * / % << >> & | ^ ~ !`,
  casts, `sizeof`, `_Alignof`, enum constants, `crefl_define()`)
- comments; preprocessor lines, functions and variables are skipped

Layout follows the host ABI (SysV/AAPCS, gcc/clang). Not validating:
check headers with the compiler. Rejected with an error:
`#pragma pack`, `ms_struct`, `_Complex`, `typeof(expr)`, floating
point constants in constant expressions, enums needing more than 64
bits (gcc uses `__int128`). A failed parse leaves the context as it
was.

## Build and test

```sh
cmake -B build && cmake --build build
make -C build test                           # all tests, parallel
cmake --install build --prefix /usr/local    # libcreflect, crefl.h, creflect.pc, crefl-tool, Lua creflect.so
cc app.c $(pkg-config --cflags --libs creflect)
```

The library is built shared (`libcreflect.so.0`, exports only
`crefl_*`). Options: `-DBUILD_SHARED_LIBS=OFF` (static
`libcreflect.a`), `-DCREFL_SANITIZE=OFF` (unit tests without ASan/UBSan),
`-DBUILD_TESTING=OFF` (no cmocka needed), `-DCREFL_RANDOM_SEEDS=N`
and `-DCREFL_RANDOM_FIRST=S` (random headers per oracle
configuration, default 10 starting at seed 1; CI starts at a new
seed per pipeline). The version is in `crefl.h`
(`CREFL_VERSION_MAJOR` etc.).

Fuzzing (clang):

```sh
cmake -B build-fuzz -DCMAKE_C_COMPILER=clang -DCREFL_FUZZ=ON -DBUILD_TESTING=OFF
cmake --build build-fuzz && build-fuzz/crefl-fuzz -max_total_time=60
```

Tests:

- `unit`: cmocka tests for the API, every parser error, constant
  expressions and allocation failures (`test/crefl-test.c`)
- `oracle-*`: `test/oracle.sh` generates a program per
  `test/testcases/*.h` that compares layout and get/set of every
  member with the compiler, parses the raw header and checks that
  printed types reparse to the same layout
- `oracle-random-*`: the same on random headers from
  `test/randtypes.py` (bitfields, packed, aligned, unions, nesting)
- `crefl-tool-*`: command line
- `lua-<lua>`, `lua-<lua>-example-*`: Lua bindings (`lua/test/`) and
  examples, per Lua built for

The oracles run for gcc and clang (64 and 32 bit) and through
qemu-user for s390x (big endian), armhf and arm64 if the cross
compilers and `libc6-dev-*-cross` are installed. Longer random runs:
`test/random-oracle.sh /tmp/rnd 1 1000`.

## Usage

```c
#include <errno.h>
#include <stdio.h>
#include "crefl.h"

static const char hdr[] =
	"enum mode { MODE_OFF, MODE_POS, MODE_VEL };\n"
	"typedef struct {\n"
	"	char name[16];\n"
	"	double gain;\n"
	"	enum mode mode;\n"
	"	struct { uint16_t lo, hi; } range[NRANGE];\n"
	"	union { uint32_t raw; struct { unsigned en : 1, err : 3; }; };\n"
	"} __attribute__((packed)) cfg_t;\n";

int main(void)
{
	/* or crefl_ctx_new_static(buf, len) */
	crefl_ctx_t *ctx = crefl_ctx_new();

	crefl_define(ctx, "NRANGE", 2); /* macros used in declarations */
	if (crefl_parse(ctx, hdr)) {
		fprintf(stderr, "%s\n", crefl_errmsg(ctx)); /* "line 2: ..." */
		return 1;
	}

	const crefl_type_t *t = crefl_type(ctx, "cfg_t");
	crefl_val_t v = crefl_new(t); /* zeroed instance */
	double gain;
	int64_t mode;

	crefl_set_str(crefl_get(v, "name"), "motor1");
	crefl_set_f64(crefl_get(v, "gain"), 0.5);
	crefl_set_i64(crefl_get(v, "mode"), 2); /* MODE_VEL */
	crefl_set_u64(crefl_get(v, "range[1].hi"), 1000);
	crefl_set_u64(crefl_get(v, "en"), 1); /* anonymous members */
	if (crefl_set_u64(crefl_get(v, "err"), 8) == -ERANGE) /* 3 bits */
		printf("err: out of range\n");

	crefl_get_f64(crefl_get(v, "gain"), &gain);
	crefl_get_i64(crefl_get(v, "mode"), &mode);
	printf("gain=%g mode=%lld\n", gain, (long long)mode);

	/* arrays: copy range[1] to range[0] */
	crefl_val_t r = crefl_get(v, "range");
	crefl_copy(crefl_index(r, 0), crefl_index(r, 1));

	/* hot path: resolve once, then O(1) access */
	crefl_path_t p;
	uint64_t raw;
	crefl_path(t, "raw", &p);
	crefl_get_u64(crefl_at(v, &p), &raw);
	printf("raw=0x%llx\n", (unsigned long long)raw);

	/* existing data, e.g. a received packet */
	unsigned char buf[64];
	crefl_copy(crefl_wrap(t, buf), v);

	/* introspection */
	for (size_t i = 0; i < crefl_ntypes(ctx); i++)
		printf("type %s\n", crefl_name(crefl_type_at(ctx, i)));

	const crefl_type_t *rt = crefl_resolve(t);
	for (size_t i = 0; i < crefl_len(rt); i++) {
		const crefl_member_t *m = crefl_member(rt, i);
		printf("%s off=%zu size=%zu\n", m->name ? m->name : "-", m->off,
		       crefl_sizeof(m->type));
	}

	const crefl_type_t *e = crefl_type(ctx, "enum mode");
	for (size_t i = 0; i < crefl_len(e); i++)
		printf("%s=%lld\n", crefl_enumval(e, i)->name,
		       (long long)crefl_enumval(e, i)->val);

	crefl_print(t, stdout); /* C declaration with layout comments */
	crefl_free(v);
	crefl_ctx_free(ctx);
	return 0;
}
```

Type names work like casts: `crefl_type(ctx, "unsigned long[4]")`,
`"foo_t *"`.

Memory: parsing, `crefl_define` and constructed type names (arrays,
pointers) allocate from the context, freed with it. Every such
`crefl_type` call builds a new type, so look them up once, not per
record; existing named types (`"struct foo"`, `"foo_t"`) are only
looked up. `crefl_new` mallocs the instance. Access (`crefl_wrap`,
`crefl_path`, `crefl_at`, `crefl_get`, `crefl_get_*`, `crefl_set_*`)
never allocates; for speed resolve paths once with `crefl_path`.

## crefl-tool

```sh
crefl-tool -l hdr.h                        # list types
crefl-tool -t cfg_t hdr.h                  # print one type with layout and holes
crefl-tool -D NRANGE=2 -t cfg_t -p range[1].hi -p err hdr.h
# range[1].hi: off=34 size=2 type=uint16_t
# err: off=36 size=4 bit=1 width=3 type=unsigned int
crefl-tool -D NRANGE=2 -t cfg_t -d cfg.bin hdr.h # decode binary records
# name = "motor1"
# mode = 2 (MODE_VEL)
# range[1].hi = 1000 ...
crefl-tool -E -I include -D N=16 hdr.h     # preprocess first ($CPP, default cc -E -P)
crefl-tool -q hdr.h                        # check only, exit 1 on parse errors
```

Without `-E`, includes and macros are skipped (`#if` blocks too, so
both branches are parsed); constants used in declarations need `-D`.
With `-E`, several files are preprocessed in one run, so include
guards work across them. See `crefl-tool -h`.

## Lua

```lua
local creflect = require("creflect")

local hdr = [[
enum mode { MODE_OFF, MODE_POS, MODE_VEL };
typedef struct {
	char name[16];
	double gain;
	enum mode mode;
	struct { uint16_t lo, hi; } range[NRANGE];
	union { uint32_t raw; struct { unsigned en : 1, err : 3; }; };
} __attribute__((packed)) cfg_t;
]]

local ctx = creflect.new()
ctx:define("NRANGE", 2)             -- macros used in declarations
local ok, err = ctx:parse(hdr)
if not ok then error(err) end       -- "line 2: ..."

local t = ctx:type("cfg_t")
local v = t:new()                   -- zeroed instance, or t:new{ ... }

v.name = "motor1"
v.gain = 0.5
v.mode = "MODE_VEL"                 -- enumerator names or numbers
v["range[1].hi"] = 1000             -- member paths work as keys
v.en = 1                            -- anonymous members
print(pcall(function() v.err = 8 end)) -- false, creflect: err: out of range

print(("gain=%g mode=%d"):format(v.gain, v.mode))

-- arrays are 0-based like C, assigning a value copies it
v.range[0] = v.range[1]
print(("raw=0x%x"):format(v.raw))

-- from and to plain Lua data, e.g. JSON
creflect.fromlua(v, { gain = 0.8, range = { { lo = 1 }, { hi = 2 } } })
local cfg = creflect.tolua(v)       -- arrays as 1-based sequences
print(cfg.name, cfg.gain, cfg.range[2].hi)

-- introspection
for _, ty in ipairs(ctx:types()) do
	print("type " .. ty:name())
end
for _, m in ipairs(t:resolve():members()) do
	print(("%s off=%d size=%d"):format(m.name or "-", m.off, m.type:sizeof()))
end
for _, e in ipairs(ctx:type("enum mode"):enums()) do
	print(("%s=%d"):format(e.name, e.val))
end

print(t)                            -- C declaration with layout comments
```

| call                                                 | description                                                             |
|------------------------------------------------------|-------------------------------------------------------------------------|
| `ctx = creflect.new()`                               | new context, freed by GC                                                |
| `ctx:parse(src)`                                     | `true` or `nil, err`                                                    |
| `ctx:define(name, int)`                              | constant for array sizes, enum values                                   |
| `t = ctx:type(decl)`                                 | type or `nil, err`, e.g. `"struct cfg"`, `"int[4]"`                     |
| `ctx:types()`                                        | all named types                                                         |
| `v = ctx:new(decl or t, [init])`, `t:new([init])`    | zeroed instance, optional init table                                    |
| `t:kind()`                                           | `prim ptr array struct union enum typedef`                              |
| `t:name() sizeof() alignof() len() elem() resolve()` | see C API                                                               |
| `t:is_complete() is_packed() is_signed()`            |                                                                         |
| `t:members()`                                        | `{ name, type, off, bit_off, bit_width }` list                          |
| `t:enums()`                                          | `{ name, val }` list                                                    |
| `t:wrap(ptr)`                                        | view on C memory (lightuserdata), unchecked                             |
| `rd = t:reader{path, ...}`                           | paths resolved once, for decoding many records                          |
| `rd(ptr or v)`, `rd(ptr or v, tbl)`                  | the values in path order, or set as `tbl[path]` (composites as `tolua`) |
| `tostring(t)`                                        | C declaration with layout comments                                      |
| `v.m`, `v[i]`, `v["a.b[2]"]`                         | scalars as numbers/booleans, `char[]` as strings, others as views       |
| `v.m = x`                                            | number, boolean, string (`char[]`, enum name), table, value (copy)      |
| `#v`                                                 | array length                                                            |
| `pairs(v)`                                           | members by name, arrays by 0-based index (Lua 5.2+)                     |
| `creflect.tolua(v)`                                  | deep copy to Lua: number, boolean, string (also enum names) or table    |
| `creflect.fromlua(v, x)`                             | assign `x` to the whole value, inverse of `tolua`                       |
| `creflect.typeof(v)`                                 | type of a value                                                         |
| `creflect.addr(v)`                                   | address as lightuserdata, e.g. to pass to C                             |
| `creflect.tobytes(v)`, `creflect.frombytes(v, s)`    | raw memory as a string and back, `#s` must equal `sizeof`               |

Views keep their parent alive and point into the same memory, so
for repeated access keep a view (`local b = v.a.b`) instead of
resolving the path every time. Integers above `math.maxinteger`
read as floats, pointers as integer addresses (not followed).
Flexible array members cannot be indexed. Examples: `lua/examples/`.

Decoding many records, e.g. from a ring buffer: a reader avoids the
view and the path parsing per field. With 8 fields on a 1 GHz x86 it
takes 0.27 us per record, against 1.6 us for `t:wrap(ptr)` and
`v.x`. LuaJIT's FFI is faster still (about 0.03 us).

```lua
local rd = t:reader({ "gain", "mode", "range[1].hi" })
local gain, mode, hi = rd(ptr)   -- ptr: lightuserdata, or a value
```

> *Tip*: all values share one metatable, so printing them as Lua
> data is a one-liner (affects all values):
>
> ```lua
> local pt = require("prettytable")
> getmetatable(v).__tostring = function(x) return pt.tostr(creflect.tolua(x)) end
> print(v)        -- {a={x=1, y=0}, b={x=0, y=2}}
> ```

Built against the newest Lua found (5.5, 5.4, 5.3, LuaJIT, 5.2, 5.1),
or one module per Lua listed in `-DCONFIG_LUA_VER`, e.g. `jit,5.4`
(versions 5.1 to 5.5, `jit`, or `lua` for an unversioned `lua.pc`), or
for `-DCONFIG_LUA_BIN=... -DCONFIG_LUA_INCDIR=...` (luarocks). Modules
install to `<CONFIG_LIBDIR>/<version>/creflect.so`, default `lib/lua`
in the prefix; LuaJIT and 5.1 share `5.1`, so only one of them.
5.1/5.2 need `lua-compat53-dev`, the tests `lua-unit`.
`-DCREFL_LUA=OFF` disables the bindings.

## License

LGPL-2.0-or-later, see `COPYING`.
