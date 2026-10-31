-- SPDX-License-Identifier: LGPL-2.0-or-later
-- Copyright (C) 2019-2026 Markus Klotzbuecher <mk@mkio.de>

local lu = require("luaunit")
local creflect = require("creflect")

local T = {}

function T:test_parse()
	local ctx = creflect.new()
	lu.assert_true(ctx:parse("struct a { int x; }; typedef struct a a_t;"))
	lu.assert_not_nil(ctx:type("struct a"))
	lu.assert_not_nil(ctx:type("a_t"))
end

function T:test_parse_error()
	local ctx = creflect.new()
	local ok, err = ctx:parse("struct a {\n int x\n};")
	lu.assert_nil(ok)
	lu.assert_str_contains(err, "line 3")
	-- a failed parse leaves the context unchanged
	lu.assert_equals(#ctx:types(), 0)
	lu.assert_true(ctx:parse("struct a { int x; };"))
end

function T:test_define()
	local ctx = creflect.new()
	lu.assert_true(ctx:define("N", 4))
	lu.assert_true(ctx:parse("struct a { int x[N * 2]; };"))
	lu.assert_equals(ctx:type("struct a"):sizeof(), 32)
end

function T:test_type_lookup()
	local ctx = creflect.new()
	local t, err = ctx:type("struct nope x")
	lu.assert_nil(t)
	lu.assert_is_string(err)
	lu.assert_equals(ctx:type("unsigned int[4]"):sizeof(), 16)
	lu.assert_equals(ctx:type("uint8_t"):sizeof(), 1)
end

function T:test_types()
	local ctx = creflect.new()
	ctx:parse("struct a { int x; }; enum e { E }; typedef int i_t;")
	local names = {}
	for _, t in ipairs(ctx:types()) do
		names[#names + 1] = t:kind() .. " " .. t:name()
	end
	lu.assert_equals(names, { "struct a", "enum e", "typedef i_t" })
end

function T:test_new()
	local ctx = creflect.new()
	ctx:parse("struct a { int x; }; struct inc;")
	lu.assert_equals(ctx:new("struct a").x, 0)
	lu.assert_equals(ctx:new(ctx:type("struct a"), { x = 3 }).x, 3)
	lu.assert_error_msg_contains("incomplete", ctx.new, ctx, "struct inc")
	lu.assert_error_msg_contains("creflect:", ctx.new, ctx, "struct nope x")
end

-- types and values keep their context alive
function T:test_lifetime()
	local ctx = creflect.new()
	ctx:parse("struct a { struct { int y; } in[2]; };")
	local t = ctx:type("struct a")
	local v = t:new()
	local inner = v["in"][1]
	ctx, t, v = nil, nil, nil
	collectgarbage()
	collectgarbage()
	inner.y = 7
	lu.assert_equals(inner.y, 7)
	lu.assert_equals(creflect.typeof(inner):sizeof(), 4)
end

return T
