# Driving the Ableton Push 2 (writing to the device)

How `norns-panicos` talks **to** the Push 2 — pad/button LEDs, the colour
palette, and mode. Implemented in `src/norns-input-bridge.c` (MIDI / LEDs) and
`src/norns-push2-display.c` (display). Spec reference:
<https://github.com/Ableton/push-interface>.

## Why libusb instead of ALSA/JACK

On PanicOS the Push 2 enumerates as an ALSA USB-MIDI device, but **PipeWire's
Midi-Bridge reads its USB-MIDI without ever forwarding events into the
graph** — JACK/PipeWire clients receive nothing (kernel
`/proc/asound/card2/midi0` Rx counters still climb). A second ALSA-seq
subscriber is also refused (`snd_seq_connect_from` → `-EAGAIN`) because
PipeWire holds the rawmidi device exclusively. So we bypass the host MIDI stack
entirely and speak USB to the device, exactly as the display does.

## USB layout

The Push 2 exposes three interfaces; we use two:

| Interface | Class | Endpoints | Use |
|-----------|-------|-----------|-----|
| 0 | vendor-specific | bulk `0x01` out | display (BGR565 frames) |
| 2 | USB MIDIStreaming | bulk `0x82` in, `0x02` out | pads / knobs / buttons / LEDs |

Claiming interface 2 (detaches `snd-usb-audio`, removing it from ALSA/PipeWire):

```c
libusb_set_auto_detach_kernel_driver(h, 1);
libusb_claim_interface(h, 2);
```

Two processes may hold the same device on different interfaces, so the display
(iface 0) and the MIDI bridge (iface 2) run independently.

## USB-MIDI packet format

Every event on EP `0x02` (and `0x82`) is a **4-byte USB-MIDI packet**:

```
byte 0:  (cable << 4) | CIN     byte 1..3: the MIDI bytes
```

- **CIN** (Code Index Number) classifies the payload and gives its length.
  Channel-voice messages use `CIN = status >> 4` (e.g. note-on `0x9`, CC `0xB`).
  SysEx uses `0x4` for full 3-byte continuations and `0x5/0x6/0x7` for a final
  group of 1/2/3 bytes (the group containing `0xF7`).
- **cable** selects the virtual port — this matters (see below).

`push2_usb_send(h, cable, msg, len)` packetises both forms.

## Cables / ports — the critical gotcha

The Push 2 has two virtual MIDI ports on the one endpoint, addressed by cable:

| Cable | Port |
|-------|------|
| 0 | Live port |
| 1 | User port |

We put the device in **User mode**, after which **all controls *and* LED
control live on the User port = cable 1**. Sending LED messages on cable 0 is
silently ignored. Only the mode-set SysEx itself is sent on cable 0.

Set User mode (cable 0):

```
F0 00 21 1D 01 01 0A  01  F7        ; 01 = User (00 = Live, 02 = Dual)
```

## Pad LEDs

8×8 pads are MIDI notes **36 (bottom-left) … 99 (top-right)**, +8 per row up.
Light a pad with a **note-on on cable 1, channel 0** (channel 0 = no
animation); velocity is a **palette index**, not a brightness:

```c
uint8_t m[3] = { 0x90, note, palette_index };   // note 36..99
push2_usb_send(h, 1 /*USER*/, m, 3);
```

`note-off` or velocity 0 turns the pad off.

### Colour palette (and why we reprogram it)

Velocity indexes a 128-entry RGB palette. The factory palette has only one
bright grey, and **pads dim hard on USB bus power** (no external supply), so we
reprogram it to bright colour ramps. Set one entry — each 8-bit component is
split into a low-7-bit byte and a high-1-bit byte:

```
F0 00 21 1D 01 01 03  idx  r_lo r_hi  g_lo g_hi  b_lo b_hi  w_lo w_hi  F7
```

then apply the new palette globally:

```
F0 00 21 1D 01 01 05  F7        ; Reapply Color Palette
```

`norns-input-bridge` programs indices `1..15` as one colour ramp and `17..31`
as a second, with a high brightness floor (`LED_FLOOR`). norns grid level
(0–15) maps directly to an index; the two ramps colour the left vs right grid
half so the viewport page is obvious.

## Button LEDs

Buttons are CCs; their LEDs are set with a **CC on cable 1**, value =
brightness (0 = off, 127 = max) or palette index for RGB buttons. Useful
numbers: lower display row `CC 20–27`, upper (under-encoder) row `CC 102–109`,
arrows `CC 44–47`, Play `CC 85`, Shift `CC 49`. (We currently read these as
input; LED feedback for them is straightforward to add the same way as pads.)

## Mirroring the norns grid onto the pads

The patched `device_monome.c` streams the emulated 16×8 grid to
`/tmp/norns-grid-1` — a 128-byte frame (`y*16 + x`, level 0–15) on every
`refresh()`. The bridge reads the newest frame, maps an 8×8 viewport window
(`viewport_x` 0 or 8) to pad notes (inverting rows so the top norns row is the
top pad row), and emits note-ons only for pads whose colour changed.

## Display (interface 0)

`norns-push2-display` scales the 128×64 norns screen into a centred,
aspect-correct viewport on the 960×160 panel (no full-width stretch). Knob 8
(`CC 78`) cycles modes — aspect-fit 320×160, integer ×2 256×128 (letterboxed),
full stretch — selected via `/tmp/norns-push2-scale` (1 byte, written by the
bridge, read each frame by the display).

## On-device debugging

`touch /tmp/push2dbg` before launching norns to log inbound MIDI, grid frames,
and LED paints to `norns.log` (read once at bridge startup; `rm` + relaunch to
disable).
