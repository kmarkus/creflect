-- SPDX-License-Identifier: LGPL-2.0-or-later
-- Copyright (C) 2019-2026 Markus Klotzbuecher <mk@mkio.de>
--
-- parse a struct, create an instance, read and write members

local creflect = require("creflect")

local ctx = creflect.new()
assert(ctx:parse([[
struct cfg {
	char name[16];
	double gain;
	struct { uint16_t lo, hi; } range[2];
	unsigned en : 1, mode : 3;
};
]]))

local v = ctx:new("struct cfg")
v.name = "motor1"
v.gain = 0.5
v.range[1].hi = 1000 -- C arrays are 0-based
v["range[0].lo"] = 10 -- member paths work as keys
v.mode = 5

print(v.name, v.gain, v.range[1].hi, v.range[0].lo, v.mode)

-- out of range values raise an error, the member is unchanged
local ok, err = pcall(function()
	v.mode = 8
end)
print(ok, err, v.mode)

-- C declaration with layout comments
print(creflect.typeof(v))
