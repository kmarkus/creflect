-- SPDX-License-Identifier: LGPL-2.0-or-later
-- Copyright (C) 2019-2026 Markus Klotzbuecher <mk@mkio.de>

local lu = require("luaunit")
local creflect = require("creflect")

local T = {}

-- lua 5.1 and luajit have no 64 bit integers
local INT64 = math.maxinteger ~= nil and math.maxinteger == 0x7fffffffffffffff

local src = [[
enum mode { OFF, ON = 5, NEG = -3 };
struct pt { int x, y; };
struct cfg {
	char name[8];
	unsigned char raw[4];
	double gain;
	float f;
	bool en;
	int8_t i8;
	uint8_t u8;
	int16_t i16;
	uint16_t u16;
	int32_t i32;
	uint32_t u32;
	int64_t i64;
	uint64_t u64;
	enum mode mode;
	enum mode modes[3];
	struct pt pts[2];
	int grid[2][3];
	unsigned bu : 3;
	int bs : 4;
	enum mode bm : 4;
	union { int ui; struct { short lo, hi; }; };
	void *ptr;
};
struct flex { int n; int d[]; };
]]

local function cfg()
	local c = creflect.new()
	assert(c:parse(src))
	return c:new("struct cfg"), c
end

function T:test_zeroed()
	local v = cfg()
	lu.assert_equals(v.name, "")
	lu.assert_equals(v.gain, 0)
	lu.assert_false(v.en)
	lu.assert_equals(v.pts[1].y, 0)
	lu.assert_equals(v.ptr, 0)
end

function T:test_int_ranges()
	local v = cfg()
	local cases = {
		{ "i8", -128, 127 },
		{ "u8", 0, 255 },
		{ "i16", -32768, 32767 },
		{ "u16", 0, 65535 },
		{ "i32", -2147483648, 2147483647 },
		{ "u32", 0, 4294967295 },
		{ "bu", 0, 7 },
		{ "bs", -8, 7 },
	}
	for _, c in ipairs(cases) do
		local m, lo, hi = c[1], c[2], c[3]
		v[m] = lo
		lu.assert_equals(v[m], lo, m)
		v[m] = hi
		lu.assert_equals(v[m], hi, m)
		lu.assert_error_msg_contains(m .. ": out of range", function()
			v[m] = hi + 1
		end)
		lu.assert_error_msg_contains(m .. ": out of range", function()
			v[m] = lo - 1
		end)
		lu.assert_equals(v[m], hi, m) -- unchanged after error
	end
	-- neighbouring bitfields are not touched
	v.bu = 5
	v.bs = -3
	lu.assert_equals(v.bu, 5)
	lu.assert_equals(v.bs, -3)
end

function T:test_int64()
	local v = cfg()
	if not INT64 then
		v.i64 = -2 ^ 53
		lu.assert_equals(v.i64, -2 ^ 53)
		v.u64 = 2 ^ 63
		lu.assert_equals(v.u64, 2 ^ 63)
		return
	end
	v.i64 = math.mininteger
	lu.assert_equals(v.i64, math.mininteger)
	v.i64 = math.maxinteger
	lu.assert_equals(v.i64, math.maxinteger)
	v.u64 = math.maxinteger
	lu.assert_equals(v.u64, math.maxinteger)
	lu.assert_true(math.type(v.u64) == "integer")
	-- above maxinteger: set and read as float
	v.u64 = 2 ^ 63
	lu.assert_equals(v.u64, 2 ^ 63)
	lu.assert_error_msg_contains("out of range", function()
		v.u64 = -1
	end)
	lu.assert_error_msg_contains("out of range", function()
		v.u64 = 2 ^ 64
	end)
end

function T:test_float()
	local v = cfg()
	v.gain = 0.125
	lu.assert_equals(v.gain, 0.125)
	v.f = 1.5
	lu.assert_equals(v.f, 1.5)
	v.gain = 3
	lu.assert_equals(v.gain, 3)
	-- integers only take exact integral values
	v.i32 = 4.0
	lu.assert_equals(v.i32, 4)
	lu.assert_error_msg_contains("out of range", function()
		v.i32 = 4.5
	end)
	lu.assert_error_msg_contains("out of range", function()
		v.i32 = 0 / 0
	end)
end

function T:test_bool()
	local v = cfg()
	v.en = true
	lu.assert_true(v.en)
	v.en = 0
	lu.assert_false(v.en)
	v.en = 1
	lu.assert_true(v.en)
	lu.assert_error_msg_contains("out of range", function()
		v.en = 2
	end)
	v.i32 = true
	lu.assert_equals(v.i32, 1)
end

function T:test_enum()
	local v = cfg()
	v.mode = "ON"
	lu.assert_equals(v.mode, 5)
	v.mode = "NEG"
	lu.assert_equals(v.mode, -3)
	v.mode = 0
	lu.assert_equals(v.mode, 0)
	lu.assert_error_msg_contains("no enumerator 'XX'", function()
		v.mode = "XX"
	end)
end

function T:test_string()
	local v = cfg()
	v.name = "motor1"
	lu.assert_equals(v.name, "motor1")
	v.name = "1234567"
	lu.assert_equals(v.name, "1234567")
	lu.assert_error_msg_contains("name: out of range", function()
		v.name = "12345678"
	end)
	v.name = "ab"
	lu.assert_equals(v.name, "ab") -- tail cleared
	lu.assert_error_msg_contains("name: string contains NUL", function()
		v.name = "a\0b"
	end)
	lu.assert_equals(v.name, "ab")
	-- unsigned char arrays are bytes, not strings
	v.raw = "ab"
	lu.assert_equals(v.raw[0], 97)
	lu.assert_equals(v.raw[2], 0)
	v.raw[3] = 255
	lu.assert_equals(v.raw[3], 255)
	lu.assert_error_msg_contains("type mismatch", function()
		v.i32 = "1"
	end)
end

function T:test_nested()
	local v = cfg()
	v.pts[1].x = 3
	lu.assert_equals(v.pts[1].x, 3)
	lu.assert_equals(v["pts[1].x"], 3)
	v["pts[0].y"] = -1
	lu.assert_equals(v.pts[0].y, -1)
	v.grid[1][2] = 12
	lu.assert_equals(v["grid[1][2]"], 12)
	lu.assert_equals(#v.pts, 2)
	lu.assert_equals(#v.grid[0], 3)
	local p = v.pts[1]
	p.y = 9
	lu.assert_equals(v.pts[1].y, 9)
end

function T:test_anonymous()
	local v = cfg()
	v.ui = 0
	v.hi = 1
	lu.assert_equals(v.hi, 1)
	lu.assert_not_equals(v.ui, 0)
end

function T:test_pointer()
	local v = cfg()
	v.ptr = 0x1000
	lu.assert_equals(v.ptr, 0x1000)
	v.ptr = nil
	lu.assert_equals(v.ptr, 0)
end

function T:test_errors()
	local v = cfg()
	lu.assert_error_msg_contains("nope: no such member", function()
		return v.nope
	end)
	lu.assert_error_msg_contains("nope: no such member", function()
		v.nope = 1
	end)
	lu.assert_error_msg_contains("[2]: out of range", function()
		return v.pts[2]
	end)
	lu.assert_error_msg_contains("[-1]: out of range", function()
		return v.pts[-1]
	end)
	lu.assert_error_msg_contains("pts[2]: out of range", function()
		return v["pts[2]"]
	end)
	lu.assert_error_msg_contains("not an array", function()
		return v.pts[0][0]
	end)
	lu.assert_error_msg_contains("type mismatch", function()
		v.pts = 1
	end)
	lu.assert_error_msg_contains("type mismatch", function()
		v.i32 = {}
	end)
	lu.assert_error_msg_contains("not an array", function()
		return #v
	end)
end

-- flexible array members cannot be indexed, the size is unknown
function T:test_flexible()
	local _, c = cfg()
	local f = c:new("struct flex")
	lu.assert_error_msg_contains("out of range", function()
		return f.d[0]
	end)
	lu.assert_error_msg_contains("out of range", function()
		return f["d[0]"]
	end)
	lu.assert_error_msg_contains("out of range", function()
		f.d = { 1 }
	end)
end

function T:test_table_init()
	local _, c = cfg()
	local v = c:new("struct cfg", {
		name = "m1",
		gain = 0.5,
		mode = "ON",
		pts = { { x = 1, y = 2 }, { x = 3 } },
		grid = { { 1, 2, 3 }, { 4 } },
		["pts[1].y"] = 4,
		bu = 7,
	})
	lu.assert_equals(v.name, "m1")
	lu.assert_equals(v.mode, 5)
	lu.assert_equals(v.pts[0].y, 2)
	lu.assert_equals(v.pts[1].x, 3)
	lu.assert_equals(v.pts[1].y, 4)
	lu.assert_equals(v.grid[1][0], 4)
	lu.assert_equals(v.grid[1][1], 0)
	lu.assert_equals(v.bu, 7)

	-- partial assignment keeps other members
	v.pts[0] = { y = 5 }
	lu.assert_equals(v.pts[0].x, 1)
	lu.assert_equals(v.pts[0].y, 5)

	lu.assert_error_msg_contains("pts: out of range", function()
		v.pts = { {}, {}, {} }
	end)
	lu.assert_error_msg_contains("z: no such member", function()
		v.pts[0] = { z = 1 }
	end)
	lu.assert_error_msg_contains("non-string key", function()
		v.pts[0] = { 1 }
	end)
end

function T:test_copy()
	local v, c = cfg()
	v.pts[0] = { x = 1, y = 2 }
	v.pts[1] = v.pts[0]
	lu.assert_equals(v.pts[1].y, 2)
	local w = c:new("struct cfg")
	creflect.fromlua(w, v)
	lu.assert_equals(w.pts[1].x, 1)
	lu.assert_error_msg_contains("type mismatch", function()
		v.pts[0] = w
	end)
	lu.assert_error_msg_contains("type mismatch", function()
		creflect.fromlua(w, v.pts[0])
	end)
	-- bitfields copy by value
	v.bu = 6
	w.bu = 0
	creflect.fromlua(w, v)
	lu.assert_equals(w.bu, 6)
end

function T:test_tolua()
	local v, c = cfg()
	local init = {
		name = "m1",
		raw = { 1, 2, 3, 4 },
		gain = 0.5,
		f = 0,
		en = true,
		i8 = -1,
		u8 = 1,
		i16 = -2,
		u16 = 2,
		i32 = -3,
		u32 = 3,
		i64 = -4,
		u64 = 4,
		mode = "ON",
		modes = { "OFF", "NEG", 4 }, -- 4: no enumerator
		pts = { { x = 1, y = 2 }, { x = 3, y = 4 } },
		grid = { { 1, 2, 3 }, { 4, 5, 6 } },
		bu = 1,
		bs = -1,
		bm = "NEG",
		ui = 0x00020001,
		lo = 1,
		hi = 2,
		ptr = 0,
	}
	creflect.fromlua(v, init)
	local t = creflect.tolua(v)
	lu.assert_equals(t, init)
	-- and back
	local w = c:new("struct cfg", t)
	lu.assert_equals(creflect.tolua(w), init)
	lu.assert_equals(creflect.tolua(v.pts), init.pts)
end

-- tolua/fromlua on scalar and string roots
function T:test_tolua_scalar()
	local _, c = cfg()
	local i = c:new("int")
	creflect.fromlua(i, -5)
	lu.assert_equals(creflect.tolua(i), -5)
	local s = c:new("char[4]", "abc")
	lu.assert_equals(creflect.tolua(s), "abc")
	local b = c:new("bool", true)
	lu.assert_true(creflect.tolua(b))
	local m = c:new("enum mode", "ON")
	lu.assert_equals(creflect.tolua(m), "ON")
	lu.assert_error_msg_contains("fromlua: out of range", creflect.fromlua, s, "abcd")
end

function T:test_tolua_enum_names()
	local v = cfg()
	v.mode = "NEG"
	lu.assert_equals(v.mode, -3) -- direct access stays numeric
	lu.assert_equals(creflect.tolua(v).mode, "NEG")
	v.mode = 4 -- no enumerator
	lu.assert_equals(creflect.tolua(v).mode, 4)
end

function T:test_wrap_addr()
	local v, c = cfg()
	local t = creflect.typeof(v)
	local w = t:wrap(creflect.addr(v))
	v.i32 = 42
	lu.assert_equals(w.i32, 42) -- same memory
	w.pts[1].y = 7
	lu.assert_equals(v.pts[1].y, 7)
	lu.assert_equals(creflect.addr(w.pts), creflect.addr(v.pts))
	local p = c:type("struct pt"):wrap(creflect.addr(v.pts[1]))
	lu.assert_equals(p.y, 7)
	lu.assert_error_msg_contains("userdata expected", t.wrap, t, 1)
	lu.assert_error_msg_contains("incomplete", c:type("struct fwd_x").wrap,
		c:type("struct fwd_x"), creflect.addr(v))
end

function T:test_reader()
	local v, c = cfg()
	local t = creflect.typeof(v)
	creflect.fromlua(v, {
		name = "m1", mode = "NEG", bs = -2, u8 = 200,
		pts = { { x = 1, y = 2 }, { x = 3, y = 4 } },
	})
	local rd = t:reader({ "name", "mode", "bs", "u8", "pts[1].y", "pts[0]" })
	local name, mode, bs, u8, y, p0 = rd(v)
	lu.assert_equals({ name, mode, bs, u8, y }, { "m1", -3, -2, 200, 4 })
	lu.assert_equals(p0, { x = 1, y = 2 }) -- composites as from tolua
	-- from a pointer, into a table
	local out = {}
	lu.assert_is(rd(creflect.addr(v), out), out)
	lu.assert_equals(out["pts[1].y"], 4)
	lu.assert_equals(out.mode, -3)
	-- live memory, not a copy
	v.pts[1].y = 9
	lu.assert_equals(select(5, rd(v)), 9)
	lu.assert_equals(select("#", t:reader({})(v)), 0)
	-- errors
	lu.assert_error_msg_contains("no such member", t.reader, t, { "nope" })
	lu.assert_error_msg_contains("not a string", t.reader, t, { 1 })
	lu.assert_error_msg_contains("type mismatch", rd, v.pts[0])
	lu.assert_error_msg_contains("incomplete", c:type("struct fwd_x").reader,
		c:type("struct fwd_x"), {})
	local f = c:type("struct flex")
	lu.assert_error_msg_contains("out of range", f.reader, f, { "d[0]" })
end

function T:test_reader_owner()
	local rd = (function()
		local c = creflect.new()
		assert(c:parse("struct a { int x; };"))
		return c:type("struct a"):reader({ "x" })
	end)()
	collectgarbage()
	collectgarbage()
	-- memory from another context, rd keeps its own type alive
	local mem = creflect.new():new("int", 5)
	lu.assert_equals(rd(creflect.addr(mem)), 5)
end

function T:test_bytes()
	local _, c = cfg()
	local pt = c:type("struct pt")
	local a = pt:new({ x = 1, y = -1 })
	local s = creflect.tobytes(a)
	lu.assert_equals(#s, 8)
	local b = pt:new()
	creflect.frombytes(b, s)
	lu.assert_equals(creflect.tolua(b), { x = 1, y = -1 })
	lu.assert_error_msg_contains("3 bytes, expected 8", creflect.frombytes, b, "abc")
	-- views copy their part only
	local v = c:new("struct cfg")
	creflect.frombytes(v.pts[1], s)
	lu.assert_equals(v.pts[1].y, -1)
	lu.assert_equals(v.pts[0].y, 0)
	lu.assert_equals(#creflect.tobytes(v.pts), 16)
end

function T:test_pairs()
	if _VERSION == "Lua 5.1" then
		return -- no __pairs
	end
	local v = cfg()
	v.pts[1].x = 5
	local keys = {}
	for k, x in pairs(v) do
		keys[#keys + 1] = k
		if k == "pts" then
			lu.assert_equals(x[1].x, 5)
		end
	end
	lu.assert_equals(#keys, 24) -- incl. ui, lo, hi of the anonymous union
	lu.assert_equals(keys[1], "name")
	lu.assert_equals(keys[#keys], "ptr")
	local idx = {}
	for i, p in pairs(v.pts) do
		idx[#idx + 1] = i
		lu.assert_equals(creflect.typeof(p), creflect.typeof(v.pts[0]))
	end
	lu.assert_equals(idx, { 0, 1 })
end

function T:test_typeof_tostring()
	local v, c = cfg()
	lu.assert_equals(creflect.typeof(v), c:type("struct cfg"))
	lu.assert_equals(creflect.typeof(v.pts):len(), 2)
	lu.assert_str_matches(tostring(v), "creflect.val<cfg>: 0x%x+")
	lu.assert_str_matches(tostring(v.grid), "creflect.val<array>: 0x%x+")
end

function T:test_many()
	local _, c = cfg()
	local t = c:type("struct cfg")
	for i = 1, 10000 do
		local v = t:new({ i32 = i })
		lu.assert_equals(v.i32, i)
	end
	collectgarbage()
end

return T
