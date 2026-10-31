-- SPDX-License-Identifier: LGPL-2.0-or-later
-- Copyright (C) 2019-2026 Markus Klotzbuecher <mk@mkio.de>

local lu = require("luaunit")
local creflect = require("creflect")

local T = {}

local src = [[
struct pt { int x, y; };
typedef struct pt pt_t;
struct s {
	char c;
	double d;
	pt_t p[3];
	unsigned a : 3, b : 5;
	int : 0;
	union { int i; float f; };
	struct s *next;
};
struct __attribute__((packed)) pk { char c; int i; };
enum col { RED = -1, GREEN, BLUE = 10 };
enum big { BIG = 0xffffffffffffffff };
struct fwd;
]]

local function ctx()
	local c = creflect.new()
	assert(c:parse(src))
	return c
end

function T:test_kind()
	local c = ctx()
	lu.assert_equals(c:type("int"):kind(), "prim")
	lu.assert_equals(c:type("int *"):kind(), "ptr")
	lu.assert_equals(c:type("int[2]"):kind(), "array")
	lu.assert_equals(c:type("struct s"):kind(), "struct")
	lu.assert_equals(c:type("union { int i; }"):kind(), "union")
	lu.assert_equals(c:type("enum col"):kind(), "enum")
	lu.assert_equals(c:type("pt_t"):kind(), "typedef")
end

function T:test_name()
	local c = ctx()
	lu.assert_equals(c:type("struct s"):name(), "s")
	lu.assert_equals(c:type("pt_t"):name(), "pt_t")
	lu.assert_equals(c:type("unsigned int"):name(), "unsigned int")
	lu.assert_nil(c:type("int *"):name())
end

function T:test_size_align()
	local c = ctx()
	local pk = c:type("struct pk")
	lu.assert_equals(pk:sizeof(), 5)
	lu.assert_equals(pk:alignof(), 1)
	lu.assert_true(pk:is_packed())
	lu.assert_false(c:type("struct pt"):is_packed())
	lu.assert_equals(c:type("struct pt"):sizeof(), 8)
	lu.assert_equals(c:type("struct pt"):alignof(), 4)
end

function T:test_flags()
	local c = ctx()
	lu.assert_true(c:type("int"):is_signed())
	lu.assert_false(c:type("unsigned"):is_signed())
	lu.assert_true(c:type("enum col"):is_signed())
	lu.assert_false(c:type("enum big"):is_signed())
	lu.assert_false(c:type("struct fwd"):is_complete())
	lu.assert_true(c:type("struct s"):is_complete())
end

function T:test_elem_resolve()
	local c = ctx()
	local td = c:type("pt_t")
	lu.assert_equals(td:elem(), c:type("struct pt"))
	lu.assert_equals(td:resolve(), c:type("struct pt"))
	lu.assert_equals(c:type("int *"):elem(), c:type("int"))
	local a = c:type("int[2][3]")
	lu.assert_equals(a:len(), 2)
	lu.assert_equals(a:elem():len(), 3)
	lu.assert_nil(c:type("int"):elem())
	lu.assert_not_equals(c:type("struct pt"), c:type("struct s"))
end

function T:test_members()
	local c = ctx()
	local m = c:type("struct s"):members()
	lu.assert_equals(#m, 8)
	lu.assert_equals(m[1].name, "c")
	lu.assert_equals(m[2].off, 8)
	lu.assert_equals(m[3].type:kind(), "array")
	lu.assert_equals(m[3].type:elem(), c:type("pt_t"))
	lu.assert_equals(m[4].name, "a")
	lu.assert_equals(m[4].bit_width, 3)
	lu.assert_equals(m[5].bit_width, 5)
	lu.assert_equals(m[5].off * 8 + m[5].bit_off, m[4].off * 8 + 3)
	lu.assert_nil(m[6].name) -- int : 0
	lu.assert_nil(m[7].name) -- anonymous union
	lu.assert_equals(m[7].type:kind(), "union")
	lu.assert_equals(m[8].name, "next")
	lu.assert_equals(m[8].type:elem(), c:type("struct s"))
	lu.assert_equals(c:type("pt_t"):members()[2].name, "y")
	lu.assert_error_msg_contains("not a struct", c:type("int").members, c:type("int"))
end

function T:test_enums()
	local c = ctx()
	local e = c:type("enum col"):enums()
	lu.assert_equals(e, {
		{ name = "RED", val = -1 },
		{ name = "GREEN", val = 0 },
		{ name = "BLUE", val = 10 },
	})
	-- above maxinteger: returned as float
	lu.assert_equals(c:type("enum big"):enums()[1].val, 2 ^ 64 - 1)
	lu.assert_error_msg_contains("not an enum", c:type("int").enums, c:type("int"))
end

function T:test_tostring()
	local c = ctx()
	local s = tostring(c:type("struct pt"))
	lu.assert_str_contains(s, "size=8, align=4")
	lu.assert_str_contains(s, "struct pt {")
	lu.assert_str_contains(s, "int y; /* off=4, size=4 */")
	lu.assert_equals(tostring(c:type("struct fwd")), "struct fwd;\n")
end

-- printed declarations parse back to the same layout
function T:test_roundtrip()
	local c = ctx()
	local c2 = creflect.new()
	for _, t in ipairs(c:types()) do
		lu.assert_true(c2:parse(tostring(t)))
	end
	for _, n in ipairs({ "struct s", "struct pk", "pt_t" }) do
		lu.assert_equals(tostring(c2:type(n)), tostring(c:type(n)))
	end
end

return T
