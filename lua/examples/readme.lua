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
