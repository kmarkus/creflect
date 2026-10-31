#!/usr/bin/env lua
-- SPDX-License-Identifier: LGPL-2.0-or-later
-- Copyright (C) 2019-2026 Markus Klotzbuecher <mk@mkio.de>
--
-- run from this directory with the module in LUA_CPATH, e.g.
--   LUA_CPATH='../../build/?.so' lua5.4 run.lua

local lu = require("luaunit")

TestCtx = require("test_ctx")
TestType = require("test_type")
TestVal = require("test_val")

-- close the state so finalizers run (valgrind leak check)
os.exit(lu.LuaUnit.run(), true)
