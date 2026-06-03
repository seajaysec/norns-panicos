-- tests/test_pad.lua — drives pad.lua against a stub norns `hid` global.
-- Run: lua tests/test_pad.lua   (expects "All tests passed.")
package.path = "lua/?.lua;" .. package.path

-- Minimal stub of the norns hid API surface pad.lua uses.
local stub_dev = { name = "norns-panicos gamepad" }
hid = { vports = { stub_dev } }
hid.connect = function(n) return hid.vports[n or 1] end

local pad = require("pad")
assert(pad.available == true, "pad.available should detect the synthetic device")

local got = {}
pad.on("a", function(v) got.a = v end)
pad.event = function(name, v) got.last = name end

-- Simulate the device firing BTN_SOUTH (304) down, then ABS_X (0) to +16384.
stub_dev.event(0x01, 304, 1)
assert(got.a == 1, "pad.on('a') should fire on BTN_SOUTH down")
assert(pad.a == true, "pad.a polled state should be true while held")
assert(got.last == "a", "pad.event should receive the semantic name")

stub_dev.event(0x03, 0, 16384)
assert(math.abs(pad.lstick.x - 0.5) < 0.05, "ABS_X should normalise to ~0.5")

stub_dev.event(0x01, 304, 0)
assert(pad.a == false, "pad.a should clear on release")

print("All tests passed.")
