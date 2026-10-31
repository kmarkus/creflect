-- SPDX-License-Identifier: LGPL-2.0-or-later
-- Copyright (C) 2019-2026 Markus Klotzbuecher <mk@mkio.de>
--
-- configure a struct from a Lua table (e.g. decoded JSON) and dump
-- it back. tolua() output can be fed to a JSON encoder as is.

local creflect = require("creflect")

local ctx = creflect.new()
assert(ctx:parse([[
enum mode { MODE_OFF, MODE_POS, MODE_VEL };

struct axis {
	char name[8];
	enum mode mode;
	double kp, ki;
	int32_t limits[2];
};

struct robot {
	uint8_t id;
	bool enabled;
	struct axis axes[3];
};
]]))

-- enums take enumerator names, unset members stay zero
local r = ctx:new("struct robot", {
	id = 7,
	enabled = true,
	axes = {
		{ name = "x", mode = "MODE_POS", kp = 1.5, limits = { -100, 100 } },
		{ name = "y", mode = "MODE_VEL", kp = 0.8, ki = 0.01 },
	},
})

-- validation happens on assignment
local ok, err = pcall(creflect.fromlua, r, { id = 256 })
print(ok, err)

-- dump as nested tables, arrays become 1-based sequences
local function dump(x, ind)
	ind = ind or ""
	if type(x) ~= "table" then
		return tostring(x)
	end
	local keys = {}
	for k in pairs(x) do
		keys[#keys + 1] = k
	end
	table.sort(keys, function(a, b)
		return tostring(a) < tostring(b)
	end)
	local out = {}
	for _, k in ipairs(keys) do
		out[#out + 1] = ind .. "  " .. tostring(k) .. " = " .. dump(x[k], ind .. "  ")
	end
	return "{\n" .. table.concat(out, ",\n") .. "\n" .. ind .. "}"
end

print(dump(creflect.tolua(r)))
