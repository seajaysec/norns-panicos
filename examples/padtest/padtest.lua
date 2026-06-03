-- padtest
-- gamepad-native demo for norns-panicos
--
-- Press any button or D-pad direction: each plays its own tone and lights up.
-- The two analog sticks show as moving dots. Built on the `pad` wrapper library
-- (lua/lib/pad.lua) over the native HID gamepad — so it needs native mode active
-- (a [script:padtest] mode=native overlay, or pad.grab() below).
--
-- E N G I N E : PolyPerc (stock norns) — one percussive tone per press.

engine.name = "PolyPerc"

local pad = require "pad"

-- 16 buttons → scale degrees (semitones over an A2 base), low→high.
local A2 = 110.0
local function hz(semi) return A2 * 2 ^ (semi / 12) end
local NOTE = {
  a = 0,  b = 2,  x = 4,  y = 5,
  l1 = 7, r1 = 9, l2 = 11, r2 = 12,
  up = 14, down = 16, left = 17, right = 19,
  select = 21, start = 23, l3 = 24, r3 = 26,
}

-- Draw order: 2 columns × 8 rows on the left; sticks on the right.
local COL1 = { "a", "b", "x", "y", "up", "down", "left", "right" }
local COL2 = { "l1", "r1", "l2", "r2", "select", "start", "l3", "r3" }

local last = "-"          -- name of the most recent press
local last_hz = nil
local redraw_metro

-- Play the tone for a button name (PolyPerc auto-decays, so press-only).
local function play(name)
  local semi = NOTE[name]
  if not semi then return end
  local f = hz(semi)
  engine.hz(f)
  last, last_hz = name, f
end

function init()
  -- Route every pad event here. Buttons fire on press (value==1); axis events
  -- (name like "lstick.x") just keep pad.lstick.* fresh, which redraw reads.
  pad.event = function(name, value)
    if NOTE[name] and value == 1 then play(name) end
  end

  -- Make this script self-sufficient even without a config overlay: ask the
  -- host for native mode at launch, release it when the script ends.
  if pad.grab then pad.grab() end

  redraw_metro = metro.init(function() redraw() end, 1 / 15, -1)
  redraw_metro:start()
end

function cleanup()
  if redraw_metro then redraw_metro:stop() end
  if pad.release then pad.release() end
end

-- A labelled button cell: bright when held, dim when idle.
local function cell(name, x, y)
  screen.level(pad[name] and 15 or 2)
  screen.move(x, y)
  screen.text(name)
end

-- A stick box with a dot at its normalised (x,y) position.
local function stick(cx, cy, r, sx, sy, label)
  screen.level(3)
  screen.rect(cx - r, cy - r, r * 2, r * 2)
  screen.stroke()
  screen.level(15)
  screen.circle(cx + sx * (r - 2), cy + sy * (r - 2), 2)
  screen.fill()
  screen.level(4)
  screen.move(cx, cy + r + 7)
  screen.text_center(label)
end

function redraw()
  screen.clear()
  screen.font_size(8)

  if not pad.available then
    screen.level(15)
    screen.move(64, 28)
    screen.text_center("no gamepad")
    screen.level(4)
    screen.move(64, 40)
    screen.text_center("enable native mode")
    screen.update()
    return
  end

  -- Header: last press + frequency.
  screen.level(15)
  screen.move(2, 7)
  screen.text("padtest")
  screen.move(126, 7)
  if last_hz then
    screen.text_right(string.format("%s %dhz", last, math.floor(last_hz + 0.5)))
  else
    screen.text_right("press a button")
  end

  -- Two columns of button labels.
  local y0, dy = 18, 6
  for i, n in ipairs(COL1) do cell(n, 2, y0 + (i - 1) * dy) end
  for i, n in ipairs(COL2) do cell(n, 30, y0 + (i - 1) * dy) end

  -- Two stick boxes on the right.
  stick(82, 24, 11, pad.lstick.x, pad.lstick.y, "L")
  stick(112, 24, 11, pad.rstick.x, pad.rstick.y, "R")

  screen.update()
end
