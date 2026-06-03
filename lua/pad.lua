-- pad.lua — ergonomic wrapper over the norns-panicos synthetic HID gamepad.
-- Sugar only: it binds the stock hid device, exposes semantic names, polled
-- state, an on()/event() callback surface, and pad.available detection.
local pad = { available = false, a=false, b=false, x=false, y=false,
  l1=false, r1=false, l2=false, r2=false, select=false, start=false,
  l3=false, r3=false, up=false, down=false, left=false, right=false,
  lstick = { x=0, y=0 }, rstick = { x=0, y=0 } }

local SYNTH_NAME = "norns-panicos gamepad"

-- evdev code → semantic button name
local BTN = {
  [0x130]="a",[0x131]="b",[0x134]="x",[0x133]="y",
  [0x136]="l1",[0x137]="r1",[0x138]="l2",[0x139]="r2",
  [0x13a]="select",[0x13b]="start",[0x13d]="l3",[0x13e]="r3",
  [0x220]="up",[0x221]="down",[0x222]="left",[0x223]="right",
}
-- evdev abs code → {stick, axis}
local AXIS = { [0]={"lstick","x"},[1]={"lstick","y"},[3]={"rstick","x"},[4]={"rstick","y"} }

local handlers = {}                 -- name → fn(value)
function pad.on(name, fn) handlers[name] = fn end
pad.event = nil                     -- optional: fn(name, value)

local function fire(name, value)
  if handlers[name] then handlers[name](value) end
  if pad.event then pad.event(name, value) end
end

local function on_hid(typ, code, value)
  if typ == 0x01 then                       -- EV_KEY
    local name = BTN[code]; if not name then return end
    pad[name] = (value ~= 0)
    fire(name, value)
  elseif typ == 0x03 then                   -- EV_ABS
    local a = AXIS[code]; if not a then return end
    local norm = value / 32767.0
    if norm < -1 then norm = -1 elseif norm > 1 then norm = 1 end
    pad[a[1]][a[2]] = norm
    fire(a[1].."."..a[2], norm)
  end
end

-- Bind to the synthetic device if present.
local function bind()
  if not hid or not hid.vports then return end
  for i, d in ipairs(hid.vports) do
    if d.name == SYNTH_NAME then
      pad.available = true
      d.event = on_hid
      return
    end
  end
end
bind()

-- Optional runtime opt-in (Task 9): request native mode without editing config.
function pad.grab()    local f=io.open("/tmp/norns-native","w") if f then f:write("1") f:close() end end
function pad.release()  local f=io.open("/tmp/norns-native","w") if f then f:write("")  f:close() end end

return pad
