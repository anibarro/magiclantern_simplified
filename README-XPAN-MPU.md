# EOS M: reading the wheel and LEFT from the MPU

How XPan gets the rear wheel (with its direction) and the LEFT key in plain
Live View on the Canon EOS M (firmware 2.0.2), and how this was found. The
code is `mpu_input()` and its helpers in `src/xpan-hud-controls.c`.

As far as we know this has not been done before on the EOS M: Magic Lantern
normally sees the wheel there only as "a control was used" and LEFT only as
a half-shutter press.

## The problem

In plain Live View (no Canon screen open) the EOS M behaves badly for a
custom UI:

* **Wheel.** Each click reaches Magic Lantern as GUI event `0x54`
  (`GMT_GUICMD_PRESS_BUTTON_SOMETHING`) with no direction, and Canon has
  already moved its own dial target (shutter in M/Tv, aperture in Av).
* **LEFT.** It arrives as `0x54`, then `BGMT_PRESS_HALFSHUTTER` (`0x50`) and
  `BGMT_UNPRESS_HALFSHUTTER` (`0x51`): indistinguishable from a real
  half-press at the GUI level.
* **RIGHT** sometimes arrives as its release (`0x27`) only.

In Canon's menus and the Q screen the wheel arrives normally as
`BGMT_WHEEL_LEFT/RIGHT` (GUI events 2/3).

## Background: the MPU

The EOS M's buttons, dial and shutter switch are read by a separate
microcontroller, the MPU, which talks to the main processor over a serial
link. Canon tells the MPU which GUI "category" is active (ROM `FF0D76FC`).
In category 0 (plain Live View) the MPU applies the dial to the exposure
target itself and only reports that a control was used; in other categories
it forwards the dial as button 13 with a signed value.

## What did not work

Kept here so nobody repeats them:

1. **Forcing another GUI category** while the HUD is shown (hooking
   `FF0D76FC`, or calling Canon's category handler at `0x3E164`): Canon's GUI
   switched with it, oscillated, and turned the screen off.
2. **Sending the MPU a different GUI state** (MPU message class 04 id 00)
   directly: same family of problems.
3. **Holding Canon in ML's menu GUI mode (93)** under the HUD: the dial was
   forwarded with its direction, but MENU/PLAY/Q and DOWN needed replaying,
   and Canon drew its own screen into the bitmap layer.
4. **Inferring the dial direction from Canon's changed setting** and putting
   the setting back afterwards: works until the setting reaches the end of
   its range, and Canon's exposure visibly moves.
5. **Telling LEFT from a half-press by Canon's focus start** (MPU send class
   09 id 25): a short real half-press never starts focusing, so it was taken
   for LEFT.

## How it was found

### 1. Key logs

XPan's *Debug info* option writes every GUI event, Canon's physical button
dispatcher calls (`FF0CC564`, hooked) and every message the camera sends to
the MPU (`mpu_send`, RAM `0x38C4`, hooked) to `ML/LOGS/XPANKEYS.LOG`. This
showed the `0x54`/`0x50`/`0x51` patterns above, but only half the
conversation: nothing from the MPU to the camera.

### 2. Finding Canon's MPU receive handler

Newer Magic Lantern ports know `mpu_recv` and `mpu_recv_cbr` (see
`src/log-d678.c` and e.g. `platform/200D.101/stubs.S`): Canon passes the
receive handler to `InitializeIntercom`, which stores it in a function
pointer. No EOS M address was known, so it was traced in the ROM dump
(`ROM1.BIN`, mapped at `0xFF000000`):

1. The startup code prints `"InitializeIntercom"` (string at `FF0C3ECC`). The
   instruction that computes that address (`add r2, pc, ...` at `FF0C3B90`) is
   next to the call itself, `bl 0x380C` at `FF0C3BAC`.
2. `0x380C` is in RAM: Canon copies part of the ROM to low RAM at boot. The
   EOS M stubs note `RAM_OFFSET 0xFFA69590` (ROM address = RAM address +
   offset). Check: `mpu_send` at RAM `0x38C4` maps to ROM `FFA6CE54`, whose
   first instruction `e92d4030` (`push {r4, r5, lr}`) is exactly the one the
   key log recorded when hooking `0x38C4`.
3. ROM `FFA6CD9C` (RAM `0x380C`) loads `r3` from a literal (`0x36A0`) and
   branches to the real routine at `FFA871E4`, which stores `r3` at offset
   `+0x14` of its state structure, `0x4C5BC`.
4. ROM `FFA6CC30` (RAM `0x36A0`) is the receive handler: it reads
   `buf[1]` (class), dispatches classes 5..13 through a jump table to handlers
   stored in another RAM structure, and keeps no state of its own.

So on the EOS M 2.0.2:

| What | Address |
| --- | --- |
| `mpu_recv` (Canon's receive handler) | RAM `0x36A0` (ROM `FFA6CC30`) |
| `mpu_recv_cbr` (pointer Canon calls it through) | RAM `0x4C5D0` (`0x4C5BC + 0x14`) |
| `mpu_send` | RAM `0x38C4` (ROM `FFA6CE54`) |
| `InitializeIntercom` | RAM `0x380C` (ROM `FFA6CD9C` → `FFA871E4`) |

The disassembly used `arm-none-eabi-objdump -D -b binary -m arm
--adjust-vma=0xff000000 --start-address=... --stop-address=... ROM1.BIN`,
plus a short script to find `add/sub rX, pc, #imm` instructions that compute
a given string address.

### 3. Logging the MPU input

XPan replaces the pointer at `0x4C5D0` with its own function, which logs the
message and calls Canon's handler. It only does so if the pointer still holds
`0x36A0`, and no Canon code is patched. Each message is
`buf = [length, class, id, data...]`; the log shows it as `MPU RECV`.

## The messages

Recorded in plain photo Live View (hexadecimal, `class id data`):

| Control | Messages from the MPU |
| --- | --- |
| Wheel | `06 26 01` (button 38: control used), then within 1 ms `01 05 Tv` (shutter) or `01 06 Av` (aperture): the value the MPU moved its dial target to, ±3 per click (clockwise = higher) |
| Wheel, a little later | `0A 08 ...` status message carrying the same moved value (below) |
| LEFT | `06 26 01`, `09 0C 01`, `05 00` … release: `05 0B`, `09 0C 00` |
| Real half-press | `06 26 01`, `09 18 E8`, **`09 06 00`**, `09 0C 01`, `05 00`, then focusing (`09 2E ...`) … release: `05 0B`, `09 0C 00`, `09 18 E9`, `09 07 00` |
| RIGHT | `06 1A 00` (button 26 released); Canon turns it into GUI `0x27` |

`01 xx` are camera properties: `01 05` is `PROP_SHUTTER` (`0x80000005`),
`01 06` is `PROP_APERTURE` (`0x80000006`). `05 00` / `05 0B` produce the
half-shutter GUI events. `09 06` / `09 07` appear only for the real shutter
button (the half-press switch); LEFT never sends them.

### The `0A 08` status message

```
0A 08 [mask lo] [mask hi] fields... [1 trailing byte]
```

| Mask bit | Size | Content |
| --- | --- | --- |
| 0 | 2 bytes | `01 80` |
| 1 | 6 bytes | `01 xx 98 10 00 Tv`: shutter at +5 |
| 2 | 5 bytes | `01 03 50 18 Av`: aperture at +4 |

Every sample in the logs fits this layout (masks 1 to 6). Higher mask bits
(e.g. `41 02` during focusing) carry fields that are not decoded. Canon and
Magic Lantern's lens code take the exposure value from this message too, so
dropping `01 05`/`01 06` alone is not enough.

## What XPan does

Only while the XPan HUD owns Live View (controls not blocked by a menu,
playback, recording and so on), inside the receive hook, before Canon sees
anything:

* **Wheel.** The first `01 05`/`01 06` within 20 ms of a `06 26 01` is
  compared with the camera's value: the sign is the direction. The message is
  dropped, so Canon's exposure never changes. For 1.5 s afterwards the Tv/Av
  bytes of `0A 08` messages are rewritten to the camera's values. Then, from
  ML's shoot task, the camera's value is sent back to the MPU with the same
  message Canon uses (`mpu_send(0x01, id, 2, {value, 0})`), so the next click
  starts from the right place. The click becomes one value step on the
  selected HUD field.
* **LEFT.** `09 0C 01` within 20 ms of `06 26 01` with no `09 06` in the last
  30 ms is LEFT: that press and its release (`05 00`, `05 0B`, `09 0C 00`) are
  dropped, so Canon never sees a half-press, and the HUD selection moves back.
* **Real half-presses** pass through untouched, short or long.
* **RIGHT** is handled at the GUI level (`0x26`, or `0x27` alone).

Outside the HUD (menus, Q, playback, movie mode, XPan off) every message goes
to Canon unchanged.

## Debugging

Turn on *XPan > Debug info*. `ML/LOGS/XPANKEYS.LOG` then contains, among
others:

```
   23783 MPU RECV len 5: 06260100 00000000 00000000
   23784 MPU RECV len 5: 01055d00 00000000 00000000 DROPPED
   23805 MPU RECV len 14: 0a080300 01800103 98100060 PINNED
   23844 MPU SEND class 01 id 05 len 2 data 60000000 00000000 00000000
```

`DROPPED`: not passed to Canon. `PINNED`: passed with Tv/Av rewritten. The
log is written once a second and slows the camera down; leave it off in
normal use.

## Limits and open points

* Firmware 2.0.2 only: the addresses above are checked at run time (the hook
  is not installed if `0x4C5D0` does not hold `0x36A0`).
* Only shutter and aperture dial targets are handled. Other dial targets
  (e.g. exposure compensation, if Canon ever assigns it in Live View) pass
  through to Canon as before.
* The meaning of the other `0A 08` fields and of `09 0C`, `09 18` is not
  known; XPan relies only on the patterns above.
