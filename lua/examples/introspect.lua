-- SPDX-License-Identifier: LGPL-2.0-or-later
-- Copyright (C) 2019-2026 Markus Klotzbuecher <mk@mkio.de>
--
-- list all types of a header with their member layout
--   lua introspect.lua [header.h]
-- headers with #include or macros need preprocessing first, see
-- crefl-tool -E

local creflect = require("creflect")

local src = [[
typedef struct {
	uint32_t magic;
	uint8_t flags : 4, ver : 4;
	union { int32_t i; float f; } val;
	char tag[6];
} __attribute__((packed)) hdr_t;

enum level { LOW = 1, HIGH = 10 };
]]

if arg[1] then
	local f = assert(io.open(arg[1]))
	src = f:read("*a")
	f:close()
end

local ctx = creflect.new()
assert(ctx:parse(src))

for _, t in ipairs(ctx:types()) do
	local r = t:resolve()
	print(string.format("%s %s: size=%d align=%d%s", t:kind(), t:name(),
		r:sizeof(), r:alignof(), r:is_packed() and " packed" or ""))
	if r:kind() == "struct" or r:kind() == "union" then
		for _, m in ipairs(r:members()) do
			local bits = ""
			if m.bit_width > 0 then
				bits = string.format(" bits=%d+%d", m.bit_off, m.bit_width)
			end
			print(string.format("  %-8s off=%-3d size=%-3d%s", m.name or "-",
				m.off, m.type:sizeof(), bits))
		end
	elseif r:kind() == "enum" then
		for _, e in ipairs(r:enums()) do
			print(string.format("  %s = %d", e.name, e.val))
		end
	end
end
