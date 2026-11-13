# XPan for the Canon EOS M50 (firmware 1.1.0)

Branch `xpan-m50`. The M50 shares XPan's framing and HUD code with the EOS M
([README-XPAN.md](README-XPAN.md)). Code specific to the M50 is behind
`CONFIG_M50`; the code all bodies share is behind `CONFIG_XPAN_BODY`.

**Where the port stands: [HANDOFF-XPAN-M50.md](HANDOFF-XPAN-M50.md)** (what
works on the camera, what is open, rules, build, hooks). This file is the
research log, oldest first. Its early sections ("What phase 1 does", "DIGIC 8
differences", "Known unknowns") describe the first build, which only drew on
the screen; since then the M50 has the HUD controls, Custom LUT, Live LUT
preview, film grain, the 65:24 JPEG and the 65:24 live picture, all tested
on the camera. Read from "EVF screen" down for the current design.

## Building

```
CAM=M50.110 build_tools/build_xpan.sh      # Linux / WSL
RUN_XPAN_BUILD_M50.cmd                     # Windows, through WSL
```

Output: `platform/M50.110/build_xpan/magiclantern.zip`. Like the other bodies,
the zip ships no modules (only `ML/MODULES/M50_110.sym`) and, unlike them, no
`ML/XPAN/` folder: the LUTs, grain texture and watermark are not used yet.

Without `XPAN=y` (`make -C platform/M50.110`) the M50 build still has the
framing and the HUD (Shoot > XPan), but no XPan tab and no INFO shortcut.

### Checks after every build, and the card

`build_xpan.sh` builds from a clean folder (WSL on NTFS shows clock skew,
which leaves stale objects; `CLEAN=0` for incremental) and then fails unless
every check passes:

| Check | Catches |
| --- | --- |
| no 0-byte `.o` files | the WSL/NTFS write glitch |
| `magiclantern.bin` embedded in `autoexec.bin` | a broken package |
| the zip holds this `autoexec.bin` and no `.mo` | a stale or mixed zip |
| M50: Canon keeps at least 32 KB above `MINIMUM_USER_MEM_LEFT` | the silent `fail:` stop in `boot-d678.c` |
| M50: no `backup_rom_task` / crash-log symbols | the first boot hang |
| M50: `m50_gui_wait_timeout_ms` present | the battery-door hang |
| M50: ROM signature and stub entries (`build_tools/check_m50_rom.py`, when `extras/M50/ROMS/ROM0.BIN` exists) | firmware or stub mismatch |

A verified package is archived in `platform/<cam>/xpan_builds/<date>-<hash>/`.
Copy it to the card with

```
powershell -ExecutionPolicy Bypass -File build_tools\xpan_update_card.ps1 -Cam M50.110
powershell -ExecutionPolicy Bypass -File build_tools\xpan_update_card.ps1 -Cam M50.110 -Build platform\M50.110\xpan_builds\<date>-<hash>
```

It only writes to a card whose `ML\MODULES` holds that camera's `.sym`,
compares every file on the card with the package, checks the boot markers,
the exFAT boot sector and checksum, and that no `.FIR` is left at the root,
and logs each update in `platform/<cam>/xpan_builds/card-history.log`.

## What phase 1 did (first build, 2026-11-08; superseded, see the hand-off)

| Feature | M50 | Notes |
| --- | --- | --- |
| 65:24 / 3:2 framing, Top / Bottom / Split | yes | Black matte around the framed band (LCD; the electronic viewfinder should show the same layer, not checked). See *Framing* below. |
| Photo HUD, themes, grid | yes | Display only: no field is selected. |
| Equivalent lens on the HUD | yes | 22.3 mm sensor width (the XPan's 45 mm is about 15 mm on the M50). |
| INFO | yes | 3:2: HUD on / Canon. 65:24: Top / Bottom / Split / Canon (see *Canon layout*). |
| Dial, arrows, Q/SET | Canon's | XPan cannot write settings, so it leaves every other key to Canon. The HUD shows what Canon sets. |
| Canon settings check | read only | **XPan > Canon settings** shows the aspect ratio, shooting mode and picture quality code Canon reports, and warns when the aspect ratio is not 3:2. |
| Custom LUT, tone base | no | Shown as "Not on the M50 yet". |
| 65:24 crop of the JPEG | no | The JPEG and the CR3 stay 3:2: crop afterwards (rows below). |
| Film grain, watermark | no | Same stage as the crop. |
| Live View / movie LUT, 1:1 movie crop | no | |

### Framing

The EOS M moves the Live View picture so that the centre of the sensor shows
in the 65:24 band, with the HUD beside it (a display filter that reads
Canon's DIGIC 5 Live View buffers). The M50 cannot do that yet, so the
picture stays where Canon draws it, and the matte shows which band of the
sensor is framed. In 6000x4000 pixels (Canon's L size), 65:24 is
6000x2216, at:

| Frame alignment | Rows of the photo |
| --- | --- |
| Top (default) | 0 - 2216 |
| Split | 500 - 2716 |
| Bottom | 1784 - 4000 |

Crop the 3:2 photo to those rows afterwards. Split is the closest to the
centre of the lens (centre at 1608 instead of 2000). The rows assume that
Live View shows the whole 3:2 sensor frame. That still has to be checked on
the camera, with Canon's aspect ratio at 3:2.

## DIGIC 8 differences that shaped the port

* **Bitmap.** The M50 draws ML's screen into its own 8-bit buffer, which a
  task converts into Canon's RGBA layer. There is no idle bitmap: on this body
  `bmp_vram_real()` is Canon's 32-bit layer, and `bmp_vram_idle()` is that
  address with `^ 0x80000` applied, which is not a buffer at all. The HUD's
  double buffering would have written 720x480 frames into random memory, so on
  `FEATURE_VRAM_RGBA` bodies it draws straight into ML's buffer and skips every
  idle copy (`src/xpan-photo-hud.inc`, `XPAN_HUD_IDLE_COPY`). The HUD debug
  pixel watch reads ML's buffer too (`src/xpan-hud-controls.c`).
* **Canon's dial state.** The EOS M and 6D read Canon's OLC state at a fixed
  RAM address after checking their ROM getters. Any body other than the 6D
  fell through to the EOS M's ROM addresses, which are not valid memory on the
  M50. The M50 now returns no dial target (`src/cropmarks.c`).
* **No runtime ROM hooks.** `patch_hook_function` is DIGIC 4/5 code. DIGIC 8
  patches ROM through the MMU with a table fixed at build time
  (`platform/M50.110/include/platform/mmu_patches.h`), and that table only
  redirects whole functions. The EOS M's register-level hooks (look, JPEG
  crop, grain, Live View LUT) all have to be redesigned for it. The button
  dispatcher debug hook is skipped on the M50.
* **Flexinfo** is turned on for the M50 (`FEATURE_FLEXINFO`), because the HUD
  is drawn by it. The M50 has no histogram or waveform, so XPan's monitor
  placement in `src/zebra.c` is only built where they exist.

## Known unknowns for the first camera test

* Whether the HUD flickers. ML's buffer is converted to Canon's layer
  asynchronously, so a redraw may show half drawn.
* Canon's own Live View icons. The M50's "disable Canon GUI" flag is faked
  (`platform/M50.110/function_overrides.c`), so Canon may still paint its
  icons. INFO belongs to XPan while it is ON: pick Canon's clean display
  first, with XPan OFF.
* Property codes. `PROP_IMAGE_ASPECT_RATIO` (0 = 3:2) and the picture quality
  codes the HUD's FMT field decodes come from the EOS M. *XPan > Canon
  settings* shows the raw values: note them for 3:2, 16:9, L JPEG and RAW+L.
* Taking a photo with ML loaded. That is an M50 ML limit, not XPan's.

From a1ex's M50 notes (April 2019): the M50 firmware has PowerShot parts, so
some things differ from every other EOS body:

* **Exposure units.** Exposure is stored in 1/3 EV raw units, not the 1/8 EV
  units of DIGIC 5. The HUD's EV scale and `EV_RAW_MAX` (24 = +-3 EV in 1/8
  units, `src/xpan-hud-controls.c`) assume DIGIC 5; check the shutter,
  aperture and EV values the HUD shows against Canon's.
* **Shooting-settings screen runs in Live View.** What looks like plain photo
  mode is Live View with the picture hidden. XPan's HUD runs whenever `lv` is
  set (`xpan_photo_ui_active`), so it may draw over that screen.
* **No MPU, PhySw task instead.** Buttons are read by the main CPU, as on a
  PowerShot. Canon blocks Delete in plain Live View, which is why the ML menu
  is on M.Fn in this tree; a1ex read Delete from PhySw instead.
* **Missing stubs** such as `GUI_Control` need wrappers
  (`platform/M50.110/function_overrides.c` already has that one).

### Boot test log

1. 2026-11-08, first XPan build: boot flag and card markers worked (the
   firmware signature passed and the card LED came on), then the camera hung
   with the LED lit and needed a battery pull. That build had
   `CONFIG_CRASH_LOG` and `CONFIG_AUTOBACKUP_ROM`, which upstream turned on
   for the M50 without a camera test; bilalfakhouri's working M50 branches
   keep both off, and so does this tree now.
2. 2026-11-08, the same XPan build with both off (test A): **boots**. So one
   of the two features hangs the M50 at startup (most likely the crash-log
   handler write to `DRYOS_ASSERT_HANDLER` 0x4000, which runs right after
   Canon's init task). `extras/M50/test-B-no-xpan/autoexec.bin` (no XPan, no
   flexinfo) was not needed.
3. 2026-11-08, first HUD photo: Split layout, theme, clock, card space,
   battery, drive, meter, format and EV scale draw correctly, with no Canon
   icons over them. Wrong: shutter "1/45" (a half-stop value), aperture "--",
   ISO "AUTO". ISO is expected, since `PROP_ISO` is on the M50 handler deny
   list. ROM dump: signature 05EDBC80 (firmware 1.1.0 confirmed); all 89
   THUMB stubs in ROM0 look like valid function entries.
4. 2026-11-08, **XPan > Exposure probe** (`ML/LOGS/XM50EXPO.LOG`, EF-M
   15-45 at 15 mm, M mode, ISO 200). `PROP_SHUTTER` and `PROP_APERTURE`
   are 16-bit values in 1/768 EV (0x100 per 1/3 stop), not 1-byte 1/8 EV
   codes:

   | Canon | PROP_SHUTTER | | Canon | PROP_APERTURE (low 16 bits) |
   | --- | --- | --- | --- | --- |
   | 1/100 | 0x1400 | | f/3.5 | 0x0AE0 (= lens `AVEF` wide open) |
   | 1/80 | 0x1300 | | f/4 | 0x0C00 |
   | 1/60 | 0x1200 | | f/5.6 | 0x0F00 |
   | 1/50 | 0x1100 | | f/6.3 | 0x1000 |
   | 1/40 | 0x1000 | | f/8 | 0x1200 |

   Shutter: Tv * 768, signed: 0 = 1", +0x100 per third faster (0x2400 =
   1/4000). The first conversion read it backwards (1/4000 showed as 1"); fixed
   and checked on the camera at 1/4000 and 1". Aperture: Av * 768.
   `src/lens.c` now converts both (and the `_AUTO` variants) to ML's classic
   codes on the M50 (`m50_raw_shutter`, `m50_raw_aperture`); every value
   above lands on Canon's third-stop code. `PROP_APERTURE`'s upper 16 bits
   repeat the shutter value. ISO stays unknown (`PROP_ISO` denied); the HUD
   shows "--" instead of "AUTO" on the M50.
5. 2026-11-08, the build with that conversion **hung at startup** (LED on as
   soon as the battery door closed). Card file, embedded image, empty and
   stale objects were all fine. The RAM dump gives the M50's memory budget:
   Canon's user memory is 0x114920 bytes before ML, and `boot-d678.c` hangs
   silently (`fail: while(1)`) if less than 0x80010 is left; test A left only
   18 KB of margin, the failing build about 11 KB (still above the limit).
   New in that build: with a real aperture, `focus_calc_dof()` ran on M50
   lens data for the first time. Next test (F): conversion kept, DOF maths
   skipped on the M50, XPan key log 1024 -> 256 entries (44 KB margin).
   Fallback (D, `extras/M50/test-D-no-conversion/`): same without the
   conversion. `build_tools/build_xpan.sh` now fails an M50 build that leaves
   less than 32 KB above the boot minimum, and any build with empty objects.
6. 2026-11-08, build F **boots**; apertures correct, shutter mirrored
   (1/4000 <-> 1"). Shutter decoding corrected to APEX Tv * 768. Which of
   the two F changes fixed the boot (DOF maths off, or the larger memory
   margin) is not known; both stay. `extras/M50/test-D-no-conversion/` was
   not needed.
7. 2026-11-08, the corrected build (G) hung again **as soon as the battery
   door closed**, although its only change from F was the shutter formula.
   Every hang so far started that way. Closing the door starts the firmware
   in a check mode (`GMT_GUICMD_START_AS_CHECK`) without its GUI, and
   autoexec.bin runs. `my_init_task` (`src/init.c`) runs inside Canon's
   init_task and waited with no limit for the GUI (`rgb_vram_preinit`,
   `bmp_vram_raw`), so init_task never returned: LED on, camera dead. On the
   M50 the wait now gives up after 10 s (`m50_gui_wait_timeout_ms`) and ML is
   not loaded for that start. `boot-d678.c`'s `fail:` now blinks the LED
   fast on the M50 instead of a silent hang, so a memory-check failure looks
   different from any later hang. So far it is not known whether the earlier
   hangs (first boot, E) were the same thing; the changes made for them stay.
8. 2026-11-08, with that build (H): started at 1/60 it boots; switched to 1"
   while running, no freeze; then started at 1" with XPan OFF and with XPan
   ON, it **booted both times**. So the hang is intermittent and not tied to
   the shutter speed. The HUD showed "1/45" at every start until the dial
   moved: Canon replays `PROP_SHUTTER` before `PROP_SHOOTING_MODE_2`, while
   `shooting_mode` is still 0 (= P), and `lens.c` ignores the shutter in P;
   `lens_init`'s placeholder (raw 100 = 1/45) stayed. Fixed on the M50: the
   last shutter value is kept and applied when the mode arrives, and
   `lens_init` no longer overwrites a value that already came in. A boot log
   (`ML/LOGS/XBOOT.LOG`, `src/xpan-m50.c`) now records each startup stage,
   every exposure property and the first runs of the shoot task, HUD and
   controls, flushed every 200 ms by its own task, so the next hang shows
   where startup stopped.
9. 2026-11-08, the GUI-probe build hung (switch on: LED solid, screen
   black). `XBOOT.LOG` had nothing from that start: its last entries were the
   two good starts of the previous build. The boot-log task starts right
   after ML finds the card, so this hang is earlier than that, before any of
   the recent code runs; most likely all the intermittent hangs are this one.
   Two early suspects removed at once: **MMU remap** (`CONFIG_MMU_REMAP` with
   `CONFIG_SGI_HANDLERS`, `CONFIG_RPC`, `CONFIG_MMU`), whose M50 patch tables
   are all empty, so it only switched both cores' MMU tables at every start;
   and with it **Canon's tight heap**: without the remap ML reserves 0x56E90
   instead of 0x8BB90, so Canon keeps 252 KB above the boot minimum instead
   of 36 KB. `build_xpan.sh` now fails an M50 build with MMU remap code.
10. 2026-11-08, still random with that build (about half the starts hung,
    several right at the battery-door close). Control: `autoexec.bin` from
    bilalfakhouri's `M50_rawvideo` (ce2c3a831, his daily M50 build, MMU remap
    on, 74 KB margin; `extras/M50/control-bilal-M50_rawvideo/`) hung on
    **every** battery-door close. So the hang is ML's, not XPan's: closing
    the door starts the firmware in check mode, autoexec.bin runs (LED on
    at once) and `my_init_task` waits for a GUI that never comes; upstream
    waits forever. The XPan build gave up after 10 s, which still looks like
    a hang, and a switch-on during that state fails too. The limit is now
    3 s: on a good start the GUI is ready in under 1 s (ML finds the card at
    ~990 ms, after the wait), so after a door close the LED should go out
    within about 3 s, and the power switch should then start normally.
11. 2026-11-08, with the 3 s limit: the LED still stayed lit after door
    closes (twice), and once a switch-on gave LED lit + black screen. So
    the stop is before that wait: in ML's early boot, or in Canon's own
    init_task. The boot log cannot reach that early, so the card LED marks
    the stages: off just before Canon's `create_init_task` (boot-d678.c),
    then 2 flashes at `boot_pre_init_task` (just before Canon's init_task),
    3 at `boot_post_init_task` (Canon's init_task returned; GUI wait next),
    4 before `ml_init` is started, 5 at the start of `ml_init`, and off when
    ML's init is done. The last flash count before the LED stays lit is the
    stage where startup stopped.
12. 2026-11-08, failed door close: short flash (autoexec loaded, then off
    before `create_init_task`), 2 flashes (`boot_pre_init_task`), then lit:
    it stops after `boot_pre_init_task` starts and before Canon's init_task
    returns. With MMU remap off, the rest of `boot_pre_init_task` only
    installs ML's task dispatch hook and the task monitor (`tskmon`), which
    runs on every task switch and is not used by XPan. `CONFIG_TSKMON` (and
    Show tasks / CPU usage) is now off on the M50, and the stage is split:
    2 = start of `boot_pre_init_task`, 3 = just before Canon's init_task,
    4 = Canon's init_task returned, 5 = before `ml_init`, 6 = `ml_init`.
    `build_xpan.sh` fails an M50 build with `tskmon_init`.
13. 2026-11-08, door close: autoexec, 2, 3, 4 flashes, then dark: Canon's
    init_task returned and the 3 s GUI wait gave up, so check-mode starts
    are handled. The switch-on after it: autoexec, 2 + 3 flashes, then lit:
    it stopped **inside Canon's init_task** on a fresh boot. Checked against
    the ROM: ML's patched cstart (cache wrappers keep R3; `bzero32` clobbers
    R3 but Canon's own does too and cstart does not use it) and ML's
    replacement `gui_main_task` (same struct 0x536C, queue +0x18, counter
    +0x04, table 0xE08FFB94, 7 entries) match Canon's code. One ML path that
    acts during Canon's startup remains: before ML has started,
    `handle_common_events_startup` swallows type-0 events 0x04/0x06/0x08/0x0C
    (SET to skip ML, keys to pick a config). On the M50 those are small codes
    nobody has checked against Canon's internal startup events, so on the M50
    every startup event is now passed on, and the boot log records every GUI
    event before ML starts (id 30, type<<16 | code).
14. 2026-11-08, that build **started every time** (switch-ons, door closes,
    switch-on right after a door close). The four logged good starts show
    the early type-0 events Canon sends before ML is up: 0x3A, 0x6B, 0x6D,
    0x70, 0x79 (plus type 1/2/3/5/6 events and ~450-480 type 4, code 0).
    None is 0x04/0x06/0x08/0x0C, so the logs do not prove the startup filter
    was the cause (a failed start cannot write the log); the change stays
    and the boot log stays on to keep watching. The LED stage flashes added
    ~7 s per start and are now off unless `M50_LED_STAGES` is defined. The
    M50 changes that stay for startup: crash log and ROM backup off, MMU
    remap / SGI / RPC off, task monitor off, 3 s GUI wait, no startup event
    filter, LED off before Canon's `create_init_task`.

## Canon UI signals (probe, 2026-11-08)

`XPANKEYS.LOG` with XPan > Debug info ON (UISTATE lines and type-0 events),
one action at a time in Live View:

| Canon UI | Signal |
| --- | --- |
| Q/SET menu | `gui_state` 9, GUI mode 0x3C, two more GUI controllers; event 0x79 with first word 0x150013 on open, 0x10013 on close |
| Picture-style selector (flash button, C.Fn) | `gui_state` 6, GUI mode 0x63, one more controller (ML's own menu looks the same: use `gui_menu_shown()`) |
| Idle | `gui_state` 0, GUI mode 0, five controllers |
| Shutter on the dial | event 0x79, first word 0x80010003, then 0x03010002 per click; closed by 0x79 `arg 5`, first word 0x10001, ~0.3-0.5 s after the last click |
| UP, then the dial (aperture / EV) | 0x01010006 (UP), then 0x80010005 and 0x03010004; closed by 0x10001 |
| LEFT (AF/MF) | 0x80010007; closed by 0x10001 |
| Half-press (AF info) | 0x20341 / 0x20301; closed by 0x30001 |

Event 0x79 is type 0 with `obj` pointing at a small record (first word shown
above, `arg` its size); `arg 5` with a first word ending in 0001 closes the
element. None of these changes `gui_state`. Buttons: SET 0x04/0x05, RIGHT
(flash) 0x2D/0x2E, LEFT 0x2F/0x30, UP 0x31/0x32 (only reported inside menus),
wheel 0x02/0x03, half-press 0x5B/0x5C, M.Fn 0x1A/0x1B; 0x61 precedes every
physical button.

### Dim-and-freeze (works on the camera, 2026-11-08)

First test: Q/SET, picture style, UP and the dial dim XPan under Canon's UI;
AF/MF (LEFT) is too short to notice. Restoring after the Q screen took ~3 s:
its close record (0x10013, arg 0x10) was counted as an open overlay until the
safety limit. Records ending in 0x0013 (Q screen) are now ignored as
overlays, since `gui_state` already covers that dialog.

ML's GUI task sees each event before Canon's handler. In
`xpan_hud_controls_key` (before Canon), a 0x79 open/update record (not the
half-press 0x0002xxxx family), SET (0x04, Q screen) or RIGHT (0x2D, flash
button) calls `canon_ui_enter()` (`src/xpan-m50.c`): it sets
`ml_refresh_frozen` (new in `bmp.c`: `redraw_task` stops copying ML's layer
into Canon's), turns every lit pixel of ML's layer to `COLOR_GRAY(25)`
(black, transparent black and empty stay), pushes that once with
`refresh_yuv_from_rgb()`, and stays frozen; Canon then draws its UI on top.
`xpan_m50_canon_ui_step()` (shoot task, ~50 ms) also freezes when a dialog
sets `gui_state` some other way, and leaves 150 ms after Canon is idle
(`gui_state` 0 outside ML's menu, no 0x79 element open, 3 s safety limit):
unfreeze, force a HUD redraw and `redraw()`. ML's own menu, playback or
leaving XPan Live View unfreezes at once.

## ISO reader (works on the camera, 2026-11-08)

`PROP_ISO` is on the D678 handler deny list (`property_whitelist.h`). That
entry was added for every DIGIC 6/7/8 body in one 2021 commit, with no crash
confirmed on the M50; other ports mark it "possible crash?". Instead of lifting
the deny for the boot-time registration, `src/xpan-m50.c` registers a second
property slave for `PROP_ISO` alone, 5 s after the `xpan_m50` task starts, so
it cannot stop the boot. Before registering it writes `ML/LOGS/XISOTRY.TMP`,
and it removes the file 10 s later or at a normal power-off. If the camera died
in between, the next start finds the file and skips the reader: XPan > ISO
reader shows "skipped", and SET there retries. Delete the file by hand to go
back to automatic.

Values go to `m50_iso_from_prop()` in `src/lens.c`. The encoding is a guess
from shutter and aperture: Sv × 768 with ISO 100 = 0xF00, converted to ML's
classic code 32 + 8 Sv (72 = ISO 100). Values that fit a byte are taken as
classic codes, and 0 is Auto ISO. `PROP_ISO_AUTO`, which was never denied,
uses the same decoding. Both raw values are in the Exposure probe log
(`ISO` / `ISOAUTO` lines) and on the ISO reader menu line. The HUD shows "--"
until the reader reports, and "AUTO" after it reports 0.

## Canon layout and touch (2026-11-08)

Canon's touch controls (the ISO corner and the rest) stay active under the
XPan HUD, though they cannot be seen there. INFO now also selects a Canon
layout: 65:24 goes Top, Bottom, Split, Canon, Top; in 3:2, HUD off is the Canon
layout, only for changing settings. XPan draws nothing (no matte, HUD, AF
frame, grid or band lines; the band lines of the first version did not show
over Canon's screen and stayed behind in the XPan views). It pushes the empty
layer once, freezes (`ml_refresh_frozen`) and calls
`redraw()`, so Canon redraws its Live View screen on top (`dialog_redraw` of
`LiveViewApp`). This matters because ML's copy writes its transparent pixels
too when `zebra_should_run()`, which erased Canon's screen. The layout is
saved (`xpan.canon_layout`). ML's menu, playback or leaving XPan Live View
unfreezes, and coming back pushes and freezes again. Dim-and-freeze is off in
this layout; on the camera, Canon's touch controls show and work there
(2026-11-08).

Touch in the XPan views: Canon's touch controls stayed live under the HUD.
`xpan_m50_touch_blocked()` (`src/xpan-m50.c`, called first in
`xpan_hud_controls_key`) swallows touch down (0x7F) and its touch up (0x80)
while XPan Live View shows its own layout, with no Canon dialog or overlay on
top and ML's menu closed. The codes come from the key log: pairs 80-100 ms apart
with arg 0xC. Not confirmed on the camera yet.

## Card swap after power-off (2026-11-08)

After a normal power-off, taking the card out and putting it back left the
camera unresponsive until the battery was pulled. The boot log of the start
that closing the card/battery door causes ended between "init funcs done" and
"config loaded": Canon's short check start shut down while ML was still
starting and the boot log task was writing to the card. XPan now watches
`PROP_TERMINATE_SHUT_REQ` (0), `PROP_ABORT` (1) and `PROP_SHUTDOWN_REASON`
(non-zero) (`m50_shutdown_seen`, boot log ids 40-42). From then on the boot log
stops writing, and `my_big_init_task` skips the config and ML's tasks if init
has not got that far. The boot log also keeps only the first 48 GUI events
before ML starts: Canon sends 0x40000 every few ms, which filled the 512 marks
within a second.

Second attempt: the camera still did not respond after a card swap, and that
start left no boot log at all, so it stopped at or before ML's first card
access (`_find_ml_card`, about 370 ms into the check start). XPan now
registers its own property slave for the three shutdown properties at the
very start of `my_big_init_task` (`m50_wait_for_real_start`), before any card
access, and waits until 1300 ms after power-on (a normal start reaches the
card at about 1000 ms). If Canon signals shutdown by then, ML does not load
and nothing touches the card. The build checks for
`m50_shutdown_seen` and `m50_wait_for_real_start`.

Third attempt: still dead after every power-off and card swap. None of the
recent boot logs is a check start, so either it was caught (no log by
design) or the camera was already stuck from the power-off before. Changes:

- **Catching the check start:** Canon's GUI command now flags it.
  `GMT_GUICMD_START_AS_CHECK` (43), `_OPEN_SLOT_COVER` (40) or `_LOCK_OFF`
  (38) arriving before ML has started sets `m50_check_start` in ML's
  `gui_main_task` (`src/gui.c`). None of them appears in any normal start in
  XBOOT.LOG. `m50_wait_for_real_start` stops waiting on it. The card LED
  shows which signal stopped ML: 3 quick blinks for the check command, 2 for
  a shutdown property.
- **No card writes at power-off:** XPan writes nothing once power-off has
  started (`xpan_m50_card_ok`). Before this, the key log (9 MB) was
  appended to every second with no shutdown check, and the ISO guard file
  was removed at power-off. The guard is now removed 3 s after the reader
  starts, never during a power-off.
- **Dial callback:** Canon's own callback at 0x5A54 is put back when
  shutdown starts, so no XPan code is in the dial path during power-off.
- **ISO reader on/off:** XPan > ISO reader (SET) turns it off for the next
  start. ML keeps PROP_ISO on its DIGIC 6/7/8 deny list for "crashes,
  hangs", and the first report of this hang came with the first ISO reader
  test, so this lets the slave be ruled in or out.

The build checks all of these.

Results of the third attempt:

- Power off and on again without opening the door works.
- The card LED goes out after power-off.
- After a card swap the LED blinked once and the camera stayed dead.
- The result was the same with the ISO reader off.

So the power-off is fine and the ISO reader is not the cause. The check start
dies before XPan's check-start code runs, since that code would have blinked
2 or 3 times. The single blink comes from `autoexec.bin` loading. Notes from
earlier work say Bilal's plain ML M50 build also died on every battery-door
close, so the cause is in ML's startup rather than in XPan.

Fourth attempt, which is diagnostic and fixes one suspect:

- **LED heartbeat:** the card LED shows the startup stage without delaying
  Canon (`m50_boot_stage`, task `xpan_led`). It is lit solid from just
  before Canon's `init_task`. A task then repeats N quick blinks for the
  stage reached:
  - 4: Canon's `init_task` returned;
  - 5: the GUI is up and `ml_init` was created;
  - 6: `ml_init` started;
  - 8: ML stayed out.

  A long flash follows once Canon's check command has been seen. A camera that
  hangs with power on keeps showing its stage; one that powers off goes dark.
  A normal start blinks for about 2 s and then goes dark.
- **GUI wait:** the wait for Canon's GUI, which holds Canon's init task, now
  ends as soon as the check command arrives, and checks every 20 ms instead
  of every 100 ms.

Result of the fourth attempt: after a door close the LED showed long, long,
very short, very short, long, then went dark. The camera powered off after
the check start, apparently normally: the last flash is probably Canon's own
card check. The switch-on after it lit the LED solid and hung. In this build
only the stage before Canon's `init_task` lights the LED solid, so the hang
is **inside Canon's init_task**, as in point 13 of the startup history above.

Fifth attempt: until ML has started, no ML or XPan handler sees a GUI event.
`ml_gui_main_task` (`src/gui.c`) passes every event straight to Canon, and
`xpan_hud_controls_key` returns at once. Canon's `init_task` waits on
GuiMainTask. Point 14 stopped one startup filter and starts then worked every
time. The hang came back after XPan's M50 controls (Canon-UI watch, touch
block, dial/HUD keys) were added to the key path, which ran on Canon's
startup events before XPan was initialised. The build checks both guards.

Result of the fifth attempt: after a full start, power-off and then power-on
**without touching the card** gave a solid LED and a dead camera. So the hang
is not tied to the card swap: it is the intermittent hang inside Canon's
`init_task` from the startup history, which no change so far has removed for
good. The fifth attempt only took ML code out of that window, so it is not
the cause.

Lead: Canon's heap. ML reserves its own size (0x57690, about 350 KB) from
Canon's `user_mem` (0x114920, about 1.1 MB) before Canon starts, leaving
0xBD290. `MINIMUM_USER_MEM_LEFT` (0x80010) is a guess, not a measured
need. A tight heap fits a hang that comes and goes, and it fits the history:
Bilal's build, with 36 KB above that minimum, hung on every door close, and
hangs got rarer when ML's reservation shrank. Sixth attempt, diagnostic: the
boot log records Canon's heap total (id 50), each new low of free memory
(id 51) and the largest free block (id 52), once a second for the first
minute of every start.

Key observation from the user: the first start after a battery pull
**always** works, first try. Later starts, after a normal power-off with the
battery in, are the ones that hang. ML clears its own BSS at every start
(`copy_and_restart` → `zero_bss`), so no ML variable carries over. What
the battery keeps is Canon's side, mainly the MPU, which runs while the
camera is "off" and which a long battery pull resets. So the previous
**power-off** is the suspect, not the start.

ML used to do real work inside Canon's power-off, in Canon's property task:

- save the config to the card;
- restore the AF button assignment, a property write;
- possibly blink the LED for 5 s;
- arm a timer that re-armed itself every 20 ms;
- write a module flag file.

On the M50 it did all of this twice, because `PROP_TERMINATE_SHUT_REQ` and
`PROP_SHUTDOWN_REASON` both call `ml_shutdown`.

Seventh attempt: on the M50, `ml_shutdown` only sets `ml_shutdown_requested`.
There is no LED timer and no module file. Settings are saved when they
change instead: the XPan M50 task calls `config_save_if_changed` (checksum
over every config variable) every 2 s while the ML menu is closed. The ML
menu still saves when it closes. The build checks all of this.

Result of the seventh attempt: no better. It took several battery pulls to
get a start, then the third off/on hung with a solid LED. The heap numbers
from the good starts rule out memory pressure: total 0x900000, lowest free
about 2.3 MB, largest block about 1.6 MB.

The startup changes since the first M50 builds have each looked like a fix
for a while, so the hang is random and the cause is not yet isolated.
Bisection starts now, with a **boot-only test build**
(`M50_TEST=boot-only build_tools/build_xpan.sh`, archived with a
`-TEST-boot-only` suffix). It works like this:

- `autoexec.bin` loads, relocates and patches Canon's startup as usual,
  including the same memory reservation (0x57090).
- `boot_pre_init_task` lights the LED and returns without installing ML's
  task dispatch hook, so there is no `gui_main_task` override.
- `boot_post_init_task` turns the LED off and returns, so ML and XPan never
  start. The camera runs Canon's firmware only.

If this still hangs, the cause is in ML's core M50 boot: the relocation, the
cstart patches or the memory reservation. Bilal's plain ML build hanging too
fits that. If it does not hang, the cause is in what ML does after Canon's
`init_task`, and the next split tests that. The build fails a normal build
that does not start ML, and a test build that does.

Result: the boot-only build hung on the first off/on after the first start
(solid LED). **The hang is in ML's core M50 boot**, not in ML's runtime or
XPan. This matches the known DIGIC 8 bug kitor describes on the ML forum:
random hangs that need a battery drop, cause unknown.

Next measurement, in the boot-only build: `reboot.c` compares the first 64 KB
at RESTARTSTART with the ML copy it is about to write. If most of it matches,
the previous session's ML is still in RAM, so memory kept its contents
through the power-off, and the LED blinks 3 times quickly before ML is
copied. The probe only reads. If RAM survives, Canon may be taking a warm
start path that relies on memory ML has overwritten. A cold start after a
long battery pull would not.

Result: off/on worked 3 times; no 3 blinks were seen, but that build timed
them with a counting loop, likely too fast to see. Then a card swap (card out
and back in, battery door closed) left the camera unable to start. With the
boot-only build, as with the full build, the battery-door check start is the
reliable trigger; plain off/on hangs only now and then.

The probe now times its blinks with the microsecond timer (0xD400000C, as
in `log-d678.c`), with a loop cap in case it does not run that early. It
shows 1 slow blink when memory was cleared and 3 when it was kept, so every
start shows that the probe ran.

That build (cf26678fc242) looked completely dead with the card in: no blink
at all. Its waits were capped at 200M timer reads. If the timer does not run
that early, the LED went dark right after it lit and stayed dark for
minutes. A camera still stuck from the previous card swap would look the
same. A build with a 2M cap (d39db60e799e) did the same: the LED lit for an
instant, then the camera was dark and dead, even after a battery pull. So
**reading the 0xD400000C timer in the loader hangs the camera**. The probe
turns the LED off just before its first read. The first probe build had no
timer read and started fine.

The probe now uses a counting loop again, 40M counts per half blink, and the
build fails if `reboot.c` reads 0xD400000C. It is back in the **normal XPan
build** (586c8fa76704), so the camera is usable while the probe collects
data. The boot-only test is no longer needed: it has already shown that the
hang is in ML's core boot.

Result: the first start was slow (blinks too slow to count). Every start
after it hung with a solid LED, including after a couple of battery pulls.
The probe's long counting loops delay everything before Canon's firmware
starts, and with them the camera hung almost every time. The probe is gone
from `reboot.c`, and the build fails if it comes back. The card is back on
**f0d8ee9e13b7**, the last build before the startup experiments. It started
every time after a long battery pull.

Working theory for later: the hang is a timing race in early startup. A
delay before Canon's firmware runs makes it more likely, and the probe's
delay made it near-certain. Every M50 ML build adds some delay there:
`reboot.c` computes the firmware signature over 256 KB of ROM and copies
ML, then `boot-d678.c` copies and patches Canon's startup code. That fits
the hang being in ML's core boot, Bilal's build hanging too, and kitor's
"random" DIGIC 8 hang. What Canon is racing against is not known yet; the
MPU, which stays powered while the camera is off, is the likely party.

## Startup hang: the second core (2026-11-08, not the cause)

Found by reading Canon's bootloader and firmware entry in `ROM0.BIN` and
checking the addresses against the running-camera RAM dump (`RAM4.BIN`,
offset = address; `0xDC870` holds ML's first instruction, `0x1018` the
core-1 stack top `0xDC870`, `0x402C` the SGI handler pointer that
`consts.h` names). Every step below is in the ROM or in ML's source.

1. **Both cores start in the bootloader.** At reset both cores start at
   `E0008658`, and core 1 waits at `E0004910` until `[0xBFE01FC4] == 0x10`.
   It then cleans its cache (`E0004C7C`) and calls `[0xBFE01FC8] + 1`. When
   that call returns, it carries on at `E0004932`.
2. **The bootloader releases core 1 with core 0.** On a normal start it
   writes `[0xBFE01FC8] = 0xE0040000` and `[0xBFE01FC4] = 0x10` at
   `E00049AE`, then jumps there itself, so both cores enter Canon's firmware
   entry together. `autoexec.bin` is started by `E0019738`, which makes the
   same two writes with the autoexec entry (`0x800000`). So core 1 enters
   `autoexec.bin` with core 0.
3. **ML sent core 1 into Canon's firmware early.** `reboot.c` sent core 1
   straight to `E0040000` (`BLXNE`, "not expected to return, but..."), about
   100 ms before core 0 reaches Canon's entry code. Core 0 first runs ML's
   checksum, the signature over 256 KB of ROM, the copy of ML and the
   relocation.
4. **Core 1 ran on memory core 0 had not initialised.** In Canon's entry
   (`E0040014`) core 1 branches to `E00400FC` and `E00403B8`. It takes the
   spinlock at `0x4044` (`E05D7780`), which sits in Canon's .data: initial
   value 0, but held (1) in the running RAM dump. It then reads its start
   entry at `0x56520 + 16`, which is in .bss (`0x551E8`–`0xDC070`) and 0 in
   the dump. Core 0 copies .data and zeroes .bss only when it reaches the
   entry code itself, so with ML core 1 saw last session's values. With the
   battery in, RAM keeps them; a long battery pull lets them decay. That is
   why the first start after a long pull always worked, and why the pull had
   to be long.
5. **Core 1 returned into ML's loader.** With a start entry of 0 the
   firmware call returns -1. Without ML, core 1 returns into the bootloader
   at `E0004932`. With ML it returned into `reboot.c` and ran ML's loader a
   second time, beside core 0. That second run copies the blob, clears ML's
   .bss (including `_reloc`, the copy of Canon's entry code that core 0 may
   be running), then jumps into the relocated entry code. There core 1's
   `bl E00403B8` is a relative branch out of the copied 0x220 bytes that
   nobody patches. The outcome depends on timing and on what RAM holds,
   hence a "random" hang. Every delay before Canon's code made it worse: the
   probe builds, and Bilal's build with MMU remap.

Attempted fix: park core 1 in the bootloader's wait loop (`E0004910`) and
release it with the bootloader's own two mailbox writes right before core 0
enters Canon's entry code (autoexec 9b8af8173261). **It did not help.** The
first battery-door close hung, and so did the third off/on after that,
exactly as before. Points 1–5 are what the ROM says, but they are not what
hangs the camera, or not the only thing. The change is out again (`reboot.c`
matches upstream, and the build fails if the parking comes back). The ROM
checks for those bootloader instructions stay as a record.

## Startup hang: tracing Canon's init_task (2026-11-08)

The one measured fact about where it stops is from the LED stage builds
(startup history 12–13): after ML's `boot_pre_init_task` and before
Canon's `init_task` returns. `init_task` (`E0040224`) is a fixed sequence:

| Step | ROM call | Canon's error text |
|---|---|---|
| 1 | `E01211E6`, then heap check `[0x40C4] >= [0x411C] + 0x10` | USER_MEM size checking |
| 2 | `E07319AC` | dmSetup |
| 3 | `E008F9A6` | termDriverInit |
| 4 | `E008FA50("/_term")` | termDeviceCreate |
| 5 | `E00403E0("/_term")` | (next string) |
| 6 | `E0042A28` | |
| 7 | `E00EA5C0` | |
| 8 | `E00402BA` (creates the startup task) | |
| 9 | `E00406DA` (tail call) | |

None of these calls reads its incoming registers, which was checked in the
ROM, so a C copy that makes the same calls behaves the same. The test build
`M50_TEST=trace-init` (`m50_init_task_traced`, `src/init.c`, hooked in
`src/boot.h`) runs that copy. Before each step it blinks the step number on
the card LED: dark 0.6 s, then N flashes, then lit. In a hang, the last count
before the LED stays lit is the step that never returned. The compiled call
targets were checked by disassembly.

Result: all 9 counts appeared, then the LED stayed lit and the camera hung.
The copy turns the LED off as soon as step 9 returns, so **the hang is
inside step 9, `E00406DA`** (`E00406DA`–`E0040B0E`). That is Canon's long
hardware/subsystem setup, about 100 calls, ending in a tail call to
`E0050580`. One candidate there, not yet confirmed, is the poll at
`E00407A8`: it loops until `([0xC9200100] & 0xF) == 0`, right after two
`E0670312` calls. The developer guide suspects `0xC9200000` is the DMA
controller on newer bodies, but that is not confirmed.

Second trace: just before step 9 a watchdog task (priority 1, `xpan_trace9`)
starts. If step 9 has not returned after 6 s, it scans the init task's live
stack (saved context + 0x40 up to the stack top) for the innermost Thumb
return address inside step 9. It blinks the call-site offset
(`address - 4 - 0xE00406DA`) as 3 hex digits: n flashes per digit, one long
flash for 0, then 3 s lit, repeated. `FFF` means stuck in the tail call
`E0050580`, `EEE` that nothing was found. A plain solid LED means the init
task spins with interrupts off, so the watchdog never runs.

Result: after the 9th count the LED stayed solid with no digits. This
happened even on starts right after a battery pull. The watchdog never ran,
so step 9 holds the processor (top priority or interrupts off), or stopped
the timer the watchdog sleeps on. This build hung every time; it adds about
10 s of LED blinks before step 9. The probe builds, which delayed things
before Canon's code started, hung almost every time. The normal build hangs
now and then, and a stock start never does. So **the longer it takes from
power-on to step 9, the more likely step 9 hangs.**

## Startup hang: the engine at 0xC9200000 (2026-11-08)

From the ROM:

- Step 9 polls `[0xC9200100] & 0xF` at `E00407A8` (function `E0045B64`).
  It returns 0 for idle, -1 for `0xF` and 1 otherwise, and the loop goes
  round on anything but 0. So an engine left busy, or in error (`0xF`),
  hangs Canon's startup forever with no timeout.
- Nothing in step 9 starts that engine before the poll: `E0670312`
  (`b.w E01454C8`) only stores handler buffers for IDs 1 and 2 (table
  `E08F961C` = {1, 2}).
- Canon's bootloader drives the same engine. `E0007000` is the same poll.
  `E0007E04` queues commands at `0xC9200D00`: it waits while `[D00] == 1`,
  then writes `[D08] = (0xA0 | ch << 8) << 16` and `[D0C] = arg`.
  `E0007E30` stops a channel: `[D08] = 0x10001 | ch << 8`, `[D04] = 0`, then
  waits for `[0xC9200100 + ch*8] & 0xF == 0`. The developer guide suspects
  this block is the newer DMA controller.
- On a start with ML the bootloader also reads `AUTOEXEC.BIN` from the card
  (`E000A600` → `E000A3CE`, to `0x40800000`); a stock start reads no file.

So the measurable claim is: on starts that hang, channel 0 is not idle when
Canon's code begins. The normal build (2491c601224f) now checks that, with
no added delay, in `boot_pre_init_task` (`m50_engine_ch0_check`, `init.c`):

- It reads channels 0–3 (`[0xC9200100 + ch*8]`).
- If channel 0 is idle, the LED lights as before.
- If not, the LED stays **dark** through Canon's startup. Channel 0 then gets
  `E0007E30`'s stop writes and up to 200 ms to go idle (Canon's routine
  waits with no limit).
- Starts that get far enough log the values to XBOOT.LOG (ids 53–58).

`check_m50_rom.py` checks `E0007E30`, the poll loop at `E00407A8` and
`E0045B64`.

Result: the first restart hung with the LED lit, so channel 0 was idle
before Canon's code started. The engine's state before Canon starts was not
the cause on that start. The stop command is gone; the read and the log
stay (403af8f2d2d5).

## Startup hang: status (2026-11-08)

Measured on the camera:

1. A card with no `autoexec.bin` (Canon only) always starts.
2. Every ML build hangs now and then with the LED solid. That includes
   Bilal's plain ML build and the boot-only build, where ML loads,
   patches Canon's startup, then does nothing. Battery-door closes trigger
   it most often.
3. The hang is inside Canon's `init_task`, in step 9 (`E00406DA`); steps
   1–8 return (init_task trace).
4. While it hangs, even a priority-1 task that sleeps on a timer never
   runs.
5. The more time passes before step 9, the more often it hangs. The trace
   build (about 10 s of blinks before step 9) hung every time, even right
   after a battery pull.
6. Canon's heap is fine (2.3 MB free) and engine channel 0 was idle before
   Canon started, on a start that then hung.

Theories that were tested and failed, or that the ROM rules out:

- XPan's features, its card writes, ML's power-off work and the ISO reader
  all failed by test.
- Heap pressure failed by measurement.
- Parking the second core failed by test.
- Engine channel 0 being busy failed by measurement.
- "The bootloader skips a 75 KB program on autoexec starts" is ruled out by
  the ROM: `E000816C` copies the bootloader's own main code (`E0008D40`–
  `E001B3D4`) to `0x100000` and runs it on every start, and the autoexec
  loader is part of that code.

Not known: which of step 9's ~100 calls blocks, and why elapsed time
matters. The watchdog approach cannot see it (point 4). Ways forward that
do not mean more blind builds:

- Offline study of step 9 in the ROM, for anything with a time limit or a
  dependency on another processor (`E004AED8(0xBFE01FD0)` at `E0040724`
  sits next to the core-1 mailbox).
- An observer that does not depend on core 0's scheduler.
- Comparing notes with the ML developers, who know this DIGIC 8 hang.

## Startup hang: offline study of step 9 (2026-11-08)

All from `ROM0.BIN`. The scripts are scratch tools: call lists, a loop
scanner, table decoders.

- **The calls.** Step 9 (`E00406DA`–`E0040B0E`) makes about 60 direct calls.
  The few that ML's stubs name are `uart_printf`, `DryosDebugMsg`,
  `task_create` and `create_named_semaphore`; the rest are unnamed.
- **The only uncounted hardware wait.** Searching every callee down to four
  levels for loops with no counter or timeout finds one wait on hardware:
  the `E00407A8` poll of `[0xC9200100] & 0xF` (engine channel 0). The other
  hits are a NULL-terminated list walk in `E00EA784`, which builds a fixed
  memory-region table, spinlocks with timeouts, and printf loops.
- **What channel 0 carries.** In `E00067C4`, which runs on every start
  before the autoexec decision, the bootloader checks the block at
  `E1F50000` for the magic "CMIF" (CamIF). It then starts a DMA transfer of
  its 4 entries on channel 0 and does not wait for it: it sets
  `[0xBFE01FC0] = 1` and goes on. The entries:

  | Entry | Destination | Size |
  |---|---|---|
  | 0 | `0xD9000000` | 0x198 |
  | 1 | `0x821A1000` | 194 KB |
  | 2 | `0xD9000400` | 0x188 |
  | 3 | `0x8220D000` | 104 KB |

  That is 299 KB from ROM, with descriptors at `0x48300000`, and none of it
  overlaps ML's memory or the autoexec buffer. Step 9's poll waits for this
  background CamIF firmware transfer to finish.
- **That wait should have passed.** On the start that hung with the
  read-only check, channel 0 was already idle before Canon's code, so the
  transfer had finished. The hang is most likely elsewhere in step 9,
  deeper than a static scan shows.
- **The watchdog result is weaker than first stated.** Step 9 may reset the
  timer the watchdog slept on, so its silence does not prove that step 9
  spins rather than blocks.
- **Core 1's return path.** In a stock start core 1 also enters Canon's
  entry code. If its start entry (`0x56530`) is 0, it returns into the
  bootloader's main path (`E0004932` → `E0005BDA`, plain register setup
  with no core check), which cannot be the intended path. So in a stock
  start core 1 probably joins DryOS through a resume entry it saved for
  itself (`E0558DE2`). Not confirmed.

What static study cannot give: which call inside step 9 is running when the
camera hangs. That needs instrumentation that does not rely on core 0's
scheduler or timers:

1. Run step 9 as a relocated RAM copy, including the functions its
   PC-relative pointers reach. Every outward `bl`/`b.w` is re-encoded,
   and each call goes through a stub that stores a breadcrumb (the call
   number). The copy can be checked offline against the ROM by
   disassembly before it goes near the camera.
2. Show the breadcrumb with an observer that does not use core 0's
   interrupts. Core 1, which enters `autoexec.bin` anyway, would run a plain
   loop that blinks the number, timed by counting. Whether DryOS needs core 1
   before the hang point has to be checked first.

## Startup hang: core 1 and its start entry (2026-11-08)

From the RAM dump (`RAM4.BIN`, a running session) and the ROM:

- **The CamIF firmware stays intact.** `0x8xxxxxxx` is an alias of low RAM:
  the bootloader's CamIF images are at `0x021A1000` (194 KB) and
  `0x0220D000` (104 KB), word for word equal to the ROM. Nothing in a
  session overwrites them.
- **`init` runs at priority 0.** Its entry `0x000E3B81` is ML's wrapper
  around Canon's `init_task`. A priority-1 watchdog can never pre-empt it
  while it is busy. Had it been blocked on an OS object, the watchdog would
  have run, so during the hang Canon's startup task is busy on the
  processor.
- **Core 1 has its own init task.** `init1` (`init_task1`, entry
  `E0040220` → `E00405FE`) is pinned to core 1 at priority 0. It mirrors
  step 9 and hands over to core 0 through semaphores. So core 1 has to join
  DryOS during startup.
- **How a core joins.** In `create_init_task` (`E0143CFC`, the call ML
  wraps), `E01A436C` calls `E0558648` first. That writes the marker
  `0x20101231` into both cores' start entries (table `0x56520`, 16 bytes per
  core). In Canon's firmware entry a core with the marker joins DryOS
  (`E00403B8` → `E0558D9E`). An entry of 0 means "running", and the call
  returns -1. The scheduler wakes a joined core with software interrupt
  `0xA` (`E0558CE0`).
- **The dump confirms the return path was not taken there.** Both entries
  are 0 (both cores had joined). `0x100000`–`0x112694` holds no bootloader
  code, so no second bootloader pass ran in that session.

What this means for ML:

1. **The original loader.** The bootloader releases core 1 into
   `autoexec.bin`, and the generic loader sends it on to Canon's entry about
   100 ms before core 0 reaches `create_init_task`. Core 1 then reads
   whatever RAM kept. On a warm start that is 0, the entry's value while the
   camera runs, so the call returns into ML's loader and core 1 runs the
   loader a second time. After a long battery pull RAM has decayed to a
   non-zero value, and core 1 joins at once. That is why those starts
   worked.
2. **The first parking attempt.** It released core 1 right before core 0
   entered Canon's code, so before the marker existed. Core 1 again found 0
   and returned, now into the bootloader (`E0004932`). There it reran the
   main path, whose `E000816C` copies the bootloader's code over
   `0x100000`–`0x112694`: inside ML (`0xDC870`–`0x134000`). That is why it
   hung the same.

Fix (c5d349a03213):

- `reboot.c` parks core 1 in the bootloader's wait loop (`E0004910`), as in
  the first attempt.
- The release moves to the start of `boot_pre_init_task` (`init.c`,
  `m50_release_core1`). That runs inside the `init` task, after
  `create_init_task` wrote the marker. It releases only if `[0x56530]` holds
  `0x20101231`, with the bootloader's own writes: `[0xBFE01FC8] =
  0xE0040000`, then `[0xBFE01FC4] = 0x10`.
- Core 1 then joins DryOS directly and never takes the return path.
- XBOOT.LOG ids 59/60 record the entry seen and whether core 1 was released.

Checks:

- `check_m50_rom.py` checks the marker write, the table literals, the call
  order in `create_init_task` and core 1's entry check, on top of the wait
  loop and mailbox code.
- `build_xpan.sh` fails if the parking or the release is missing, if
  `boot-d678.c` releases core 1 (too early), or if the marker check goes.
- The compiled parking and release were checked by disassembly.

Prediction: warm starts, cold starts and battery-door closes all start.

Result: on the 128 GB exFAT card the first off/on still hung. The log
showed the release had worked as designed: the entry held `0x20101231` and
core 1 was released on every logged start. So the early core 1 is not the
cause of the hang, or not the only one.

## Startup hang: the card (2026-11-08)

Bilal (`M50_rawvideo`) never sees this hang. His branch tip ce2c3a831 is
the build we used as a control, and it hung on every battery-door close
here; his `reboot.c`, `boot-d678.c` and `boot.h` are the same as ours. So
the difference is outside the code.

Same build (c5d349a03213) on a 32 GB card, FAT32, formatted in the camera,
with only `autoexec.bin` and `ML/`: **every start worked**. That includes
off/on, card swaps and no battery pulls. On the 128 GB exFAT card, which
holds large files including the 256 MB RAM dump, the same build hung on
the first off/on.

So the card plays a real part. Elapsed time before step 9 was the one
factor already shown to matter, and the bootloader reads the card
(`AUTOEXEC.BIN`) before Canon's code starts. Not yet known: which property
matters (exFAT against FAT32, size, contents), and whether core 1's hold is
also needed on the small card. Use a small FAT32 card for the M50.

`extras/M50/make-card-bootable.ps1` now also handles FAT32: it writes
`EOS_DEVELOP` at 0x47 and `BOOTDISK` at 0x5C in the boot sector and its
backup, in an elevated PowerShell.

## Startup hang: clean-up (2026-11-08)

Removed: the test builds (`M50_TEST=boot-only`, `trace-init` with its
`init_task` copy and step-9 watchdog), the LED heartbeat and stage blinks,
the engine channel check and the Canon heap figures in XBOOT.LOG, the
engine ROM checks, and the test build archives.

ML's power-off on the M50 is upstream again (`tasks.c` matches upstream).
The seventh attempt skipped it, which left the module lock file
`ML/MODULES/LOADING.LCK` behind, so "Camera was not shut down cleanly.
Skipping module loading." showed at every start.

Kept, because they run in normal use:

- core 1 held in the bootloader and released after DryOS sets its marker;
- check-start detection;
- no ML or XPan key handling before ML has started;
- no XPan card writes at power-off;
- settings saved within 2 s of a change;
- the ISO reader on/off setting;
- the boot log, with start stages and the core-1 entry and release.

`build_xpan.sh` fails if test or probe code comes back, or if `tasks.c`
gets M50 power-off changes again. The build is 17554bace209.

## Controls, step 1: ISO write and touch probe (ISO write works, 2026-11-08)

The ISO reader log gives Canon's ISO values: 0 = Auto, 0xF00 = ISO 100, then
0x100 per third of a stop up to 0x2700 = ISO 25600. The HUD now labels ISO from
that value, since ML's table stopped at 6400 and showed 8000 as "12800".

ML's `prop_request_change()` cannot write ISO on the M50: it checks the
length against ML's own `PROP_ISO` handler, which the deny list keeps out, and
on DIGIC 8 it allows only `prop_write_allow[]`, which is empty.
`CONFIG_PROP_REQUEST_CHANGE` stays undefined. `src/xpan-m50.c` has one checked
writer instead: it calls Canon's `_prop_request_change(PROP_ISO, &v, 4)` from
the `xpan_m50` task only, and only with 0 or 0xF00-0x2700 in 0x100 steps,
after the ISO reader has reported. XPan > ISO write test steps one third with
LEFT/RIGHT (below 100 = Auto). The build checks that the general writes are
still off and that the checked writer is present.

Touch-to-select needs the tap position. Each touch event (0x7F down, 0x80 up)
carries a record `[0, pointer, 0x55555555]`. XPan > Touch probe keeps the last
32 events with the record and the 8 words at the pointer; SET saves
`ML/LOGS/XTOUCH.LOG`.

Touch probe result: record word 1 is `(y << 16) | x` in LCD pixels. The four
corners and the centre landed at 28,7 / 684,60 / 49,434 / 660,443 /
368,249. Word 4 is the latest position (it moves by the touch up).

## Controls, step 2: touch ISO on the HUD (2026-11-08)

In XPan's own view, a tap is still swallowed, but XPan reads its position
(`m50_touch_xy`) and tests it against the HUD fields. Tapping ISO selects it
(underlined); the dial (clockwise = up) or UP/DOWN then step it by a third
through the checked writer, and Canon never sees those keys. Below ISO 100 is
Auto. Quick dial turns step from the value just requested for 400 ms, not
from Canon's older report. Tapping ISO again, tapping anywhere else, leaving
XPan's view or 8 s without a step releases the selection, and the dial is
Canon's again. Only ISO is selectable until the other properties get their
own checked writers.

First test: ISO changed on the first dial click only, then the shutter moved.
Key log: the first click of a turn reaches Canon only as event 0x61 ("a
control was used"), and Canon then opens its shutter overlay (0x79
80010003) without any direction event; later clicks are 0x61 plus 0x02/0x03.
The overlay made XPan dim for Canon's UI and drop the selection, so the next
clicks were Canon's.

## Controls, step 3: shutter and aperture (works, 2026-11-08)

- While a field is selected XPan also takes 0x61, and 0x79 records neither
  dim XPan nor release the selection. Only dialogs, ML's menu, the Canon
  layout, leaving XPan Live View or 8 s idle release it.
- Shutter and aperture can be selected too, when the mode allows them (M/Tv;
  M/Av with an electronic lens), with checked writers like ISO's. Shutter: 0
  (1") to 0x2400 (1/4000) in 0x100 steps; longer exposures are negative and
  their form is not logged yet, so XPan does not write them. Aperture: low
  half from AVO (0xAE0 wide open on the 15-45) to AVMAX in thirds; the high
  half is kept exactly as Canon last sent it (it repeats the shutter value).
- Events XPan consumes (touch, HUD keys) are now in the key log, with result 0.

Test: all of it works, except that the dial still changed Canon's own setting
(shutter in M) together with the selected one, and Canon's overlay showed over
the HUD. The key log shows every 0x61 and 0x02/0x03 consumed: like the EOS M
in plain Live View, the M50 applies the dial internally, not from the GUI
events.

## Controls, step 4: dialog filter (did not work, 2026-11-08)

A first attempt wrote back whatever Canon's dial had changed (dial hold) and
repainted the HUD over Canon's overlay. It was removed without a camera test:
the EOS M had already ruled that kind of workaround out. The dial has to be
stopped before Canon applies it.

ROM research (1.1.0 dump):

- The EOS M stops the dial in the MPU receive path. The M50 does not use it:
  `InitializeIntercom` (E062260E) would store `mpu_recv` (E0622581) at 0xFB20,
  and the RAM dump has 0 there and no copy of that address anywhere.
- The main CPU reads the dial itself (`./EC412/RotaryEncoder.c`, task
  "RotaryEncoder"). Canon's GUI hands each click to the active screen as
  dialog events 0x81B-0x81E. ShootOlcApp's handler (`DlgOlcMainCreative.c`,
  E02D40F2) sends exactly those to its "GUI_DIAL_Event" code (tbb table at
  E02D417A), which changes the setting; LiveViewApp's handler (E04DC446)
  tests the same range. These dialog events come after the GUI events XPan
  sees (0x61, 0x02/0x03), which is why consuming those changed nothing.
- Both dialog structs are reachable through Canon's own pointers
  (`LiveViewApp_dialog` 0x139B8, `ShootOlcApp_dialog` 0x13FA4) and hold their
  handler at offset 0x54 (RAM dump: "DIALOG" signature, handlers E04DC447 and
  E02D40F3).

The fix (`src/xpan-m50.c`, dialog filter): while XPan Live View is up, the
shoot task swaps each dialog's handler pointer for a filter that calls the
original; a rebuilt dialog is hooked again. No Canon code is patched. With no
field selected everything is forwarded. With a field selected, 0x81B-0x81E
step the selected field and return 0 (handled, as Canon's handler does), so
Canon's setting does not move and its overlay does not open. UP/DOWN step it
from the GUI events as before; 0x61 and 0x02/0x03 are still consumed while a
field is selected. Direction is a guess (0x81B/0x81D anticlockwise,
0x81C/0x81E clockwise); with Debug info on, every dialog event goes to the
key log as `DIALOG ... -> Canon` or `-> XPAN (dial)`. `check_m50_rom.py`
checks both handler addresses and the 0x81B-0x81E table on every build.

Test of step 4: ISO and aperture did not change and the shutter still did.
The key log shows the dialog filter working on LiveViewApp: with nothing
selected a click is GUI 0x03 and then dialog event 0x81E (so 0x81E =
clockwise). With a field selected XPan consumed the GUI events, so no dialog
event was made, and the shutter changed anyway in the same millisecond. The
dial is applied before the GUI path.

## Controls, step 5: UI event filter (2026-11-08, waiting for the camera test)

- The "RotaryEncoder" task looks each click up in its table (ROM E0AA7598:
  `[3, 0, 0x80000407, 0x407, 0x20000b08, 0x20000b09, 0]`) and calls E01E4034
  with 0x80000407 or 0x407. E01E4654 turns that into (id 0x407, flag 1 or 0)
  and jumps through the UI event callback at RAM 0x5A54 (struct 0x5A24 +
  0x30; Canon's E03D8D9B in the RAM dump), which queues the event and passes
  it on. Everything Canon does with a click starts there.
- XPan swaps that one pointer (only while it still holds E03D8D9B) for a
  filter that calls it. With no field selected everything goes to Canon. With
  one, 0x407 steps the selected field and is not passed on. No Canon code is
  patched. The dialog filter of step 4 is removed.
- Direction guess: flag 1 = anticlockwise, flag 0 = clockwise. Debug info logs
  every UI event (`UIEVENT id ... flag ...`) next to the GUI event Canon makes
  from it.
- `check_m50_rom.py` checks the dial table, the dispatcher's
  `ldr r2,[0x5A24]; ldr r2,[r2,#0x30]; bx r2` and Canon's callback.

First test of step 5: no change. The key log showed the filter in place and
seeing every click, but 0x407 is "dial active" (flag 1 when turning starts,
flag 0 about 0.5 s after it stops), so XPan stepped down and back up. The
clicks are 0x20000B08 (clockwise; Canon makes GUI 0x03 of it) and 0x20000B09
(anticlockwise; GUI 0x02). They went to Canon, which moved the shutter. Now
XPan steps on those and keeps them while a field is selected, and keeps a
0x407 start and its stop as a pair. The camera test confirmed it works.

## Controls, step 6: EV, WB, drive, metering, format and LUT (2026-11-08)

The photo mode stays on Canon's mode dial. The other HUD fields:

| Field | Canon properties |
|---|---|
| EV | `PROP_AE` |
| WB | `PROP_WB_MODE_LV` + `PROP_WB_MODE_PH` |
| Drive | `PROP_DRIVE` |
| Metering | `PROP_METERING_MODE` |
| Format | `PROP_PIC_QUALITY` + `PIC_QUALITY2` + `PIC_QUALITY3` |

Their M50 encodings have not been logged, and a wrong value written to a
Canon property can corrupt saved settings. So these are **learned settings**
(`src/xpan-m50.c`):

- **Recording.** When Canon reports a change to one of them (its own screen,
  Q menu or buttons), XPan waits until all of that setting's properties have
  been quiet for 400 ms. It then records their exact bytes and lengths
  together as one option, up to 16 per setting.
- **Stepping.** With the field selected on the HUD, the dial or UP/DOWN
  steps through the recorded options only. XPan writes those bytes back with
  Canon's `_prop_request_change`, from its own task, and only for the
  properties whose current bytes differ. Nothing is written while
  half-pressed, while Canon is busy (`job_state`) or while recording.
- **Order.** EV is kept in numeric order (signed 1/8 EV) and stops at both
  ends. The others cycle in the order Canon gave them.
- **Saved.** The options are kept in `ML/SETTINGS/XM50OPTS.BIN`, checked on
  load (property ids and lengths). XPan > M50 > Learned settings shows the
  counts; SET forgets them all.
- **Before learning.** A field becomes selectable once it has two or more
  options. Tapping it before that shows a reminder to change it once in
  Canon's screen.

**LUT:** the field lists `ML/XPAN/LUTS/*.cube` (and `.CUB` 8.3 aliases), up
to 16, sorted, with the choice kept in `xpan.cube.name` as on the EOS M. The
HUD shows the name with "not applied yet": no LUT is applied on the M50 so
far. The M50 package now carries `data/XPAN/LUTS` (CINEON, FUJI, IDENT,
KODAK, MONO).

`build_xpan.sh` fails if `xpan-m50.c` writes a property outside the checked
ISO/shutter/aperture writers and the recorded options, or if the
learned-settings writer loses its guard.

## Controls, step 6b: EV and format fixes (2026-11-08)

Camera test of step 6: WB, drive and metering worked; EV did not match the
values and the format field did nothing. `ML/SETTINGS/XM50OPTS.BIN` from the
card showed why:

- **EV.** `PROP_AE` is 4 bytes; byte 0 was 0 in all 16 recorded options and
  the compensation is in byte 1, in thirds of a stop (seen: -9..-1, 0, +1,
  +4, +5, +6, +8, +9, i.e. -3..+3 EV). XPan sorted by byte 0, so the list was
  in learning order, the 16 slots were full before +2/3, +1 and +7/3 were
  recorded, and ML's `lens_info.ae` (byte 0) made the HUD show 0.0 EV
  always. Now the sort key is bytes 0-1 as a signed 16-bit value, there are
  24 slots, and the HUD value and marker come from `m50_ev_thirds()`. The
  old file is still read (format "XO1", 16 slots) and re-sorted; new files
  are "XO2".
- **Format.** No option was recorded: XPan wanted all three PICQ properties
  and not all of them report on the M50. An option now holds the properties
  that did report; empty ones are stored and never written. The M50 sets RAW
  and JPEG separately in Canon's menu, so the field is a toggle, JPG <->
  RAW+JPG: it writes the learned option of the other kind whose JPEG size
  and quality match best (RAW only is never chosen). Reading the kind uses
  the EOS M PICQ layout; every report of the learned properties goes to
  `ML/LOGS/XM50OPTS.LOG` to confirm it on the M50.

**M50 PICQ layout** (`ML/LOGS/XM50OPTS.LOG`, 2026-11-09). `PROP_PIC_QUALITY`
and `PROP_PIC_QUALITY2` report the same 4 bytes; `PROP_PIC_QUALITY3` never
reports. Canon sets RAW apart from the JPEG, under INFO in the quality screen
("-", RAW, cRAW).

| byte | meaning | seen |
|---|---|---|
| 0 | JPEG size | 0 (L) only |
| 1 | kind | 0x01 JPEG only, 0x07 RAW+JPEG, 0x06 RAW only |
| 2 | JPEG quality | 3 fine, 2 normal |
| 3 | RAW type | 4 / 3, toggled by RAW / cRAW |

4 = RAW, 3 = cRAW is assumed: the first step from "-" gave 4, and Canon's
maker notes use 4 for RAW. The FORMAT field steps JPG -> RAW+JPG ->
cRAW+JPG through the learned options that keep the current JPEG; RAW only is
never chosen. The HUD label reads this layout (`m50_format_label`).

`build_xpan.sh` check 6d4 fails if the format code goes back to the EOS M
layout (0x60000) or the label stops using the M50 one.

`build_xpan.sh` check 6d3 fails if the EV key or the HUD stop reading byte 1,
if learning needs every property again, if an empty property can be written,
or if the slots drop below 24.

## Look, crop and grain: ROM study (2026-11-09)

Offline, from `extras/M50/ROMS` (ROM0, ROM1 and the RAM dump), with small
string / cross-reference / caller tools (Thumb `ADR`, `MOVW`/`MOVT`, `BL`).

### Colour table (LUT)

* **Hardware.** Canon's "Lucky" colour-table engine is at `0xD0439000`
  (`ELD/Lucky/Lucky.c`). Register `+0x4C` selects the grid: 9 points
  `0x80000000`, 10 points `0x80000006`, **17 points `0x80000300`**
  (`E087D76C`). The EOS M only has the 10-point grid.
* **Grid from table size.** The still-develop loader `E011E194` picks the
  grid from the table size it is given: `0x4CC4` (17^3 x 4) gives 17,
  `0x1000` gives 10, `0xB64` (9^3 x 4) gives 9.
* **But Canon always passes 10.** Its only caller, `E00E73BC`, hard-codes
  `mov.w r1, #0x1000` (`E00E73CC`). The table pointer comes from the develop
  path's parameter getter `E0854B5E` (`E0082C3C`, "LuckyTbl DefParam"), with
  the ROM default `E0B18330` as fallback. So picture styles use the
  10-point grid, as on the EOS M. A 17-point look needs that size changed only
  for XPan's own table: a ROM code patch (MMU), which the M50 build
  forbids today ("no MMU remap", from the startup-hang work, now known to be
  the card).
* **Same format as the EOS M.** The ROM default table has the EOS M layout:
  1024 words, `Y<<20 | Cb<<10 | Cr` (10 bits each), grey axis 0..512, same
  primary signs (`ColorLucky.c` loops 10x10x10 and copies `0x1000` bytes).
  XPan's `.cube` to Canon-table conversion (`xpan-native-table.c`) applies
  unchanged.
* **Tables live in RAM.** The RAM dump holds 39 valid tables between
  `0x694820` and `0x9E6938`, mostly 4 KB apart: decrypted picture-style
  tables (`FA_GetDecryptPcsetFlavorLuckyTable`). If the develop step reads
  Neutral's table from one of them, XPan can write its look there, with no
  hook. **XPan > Colour table probe** (read only) finds every table in RAM and
  logs it to `ML/LOGS/XM50LUT.LOG`, with the Picture Style.

### The 17-point grid: who could use it (2026-11-09)

Every driver supports grid 17, and every caller in the firmware passes 10:

| Code | Module | Grid |
|---|---|---|
| `E0082C3C` -> `E00E73BC` -> loader `E011E194` | ShootDevelop / SusanDevelop (stills) | size hard-coded `0x1000` at `E00E73CC` (`0x4CC4` would give 17) |
| `E01A2186` -> `E01C7358` | DevelopStandard (next to `MvDevelopDs`) | `movs r2, #0xa` at `E01A2196`; `E01C73AE` builds `0x4CC4` only for grid 17 |
| `E0194B20` | `ELD/Opera/FuncLuckyLUT` register lists | callers pass 10 (`E00E7394`) |
| `E08814C0`, `E0895000`, `E0895FF4`, `E0885F8A`, `E086C1A4` | creative effects (Dodge, Beauty, LesserLucky, ResizeAddSubLucky) | 10, or 9 |

`movw #0x4cc4` appears once in ROM0 (`E01C73AE`). Nothing in the firmware
ever builds a 17-point table, so its memory layout is not in any Canon
table. The Opera lists at ROM `E11FD6D8` show how it is loaded: grid 10
in one pass (`0x74` bytes); grid 17 (`0x21C` bytes) in five passes with bank
select `0x39070` = 0..4 and source pages 4 KB apart (`+0x300000` ...
`+0x304000`). The 10-point table is banked too (`table_index`). The 17-point
layout has to be worked out from these lists before XPan can write one.

**Video.** No Live View or movie code calls these drivers. Live View and
movie colour go through another path (the EOS M used Canon's LCD colour
transfer; on the M50 it is not found yet), so whether video can get 17 points
is open. The hardware block is the same one, so it may be possible.

**What a 17-point still look needs.**
1. The 17-point table layout (from the Opera lists above, and checked on
   the camera with a test table).
2. Canon's size argument changed from `0x1000` to `0x4CC4` only when the
   table is XPan's own: a ROM code change (an MMU patch of `E00E73CC`, or a
   hook on `E00E73BC`). The M50 build forbids MMU patches since the
   startup-hang work. That hang turned out to be the card, so the ban can be
   reviewed.
3. Where the shot's table pointer comes from (`E0854B5E`, or a RAM copy):
   **Colour table probe**.

### MMU remap back on, step 1 of the 17-point look (2026-11-09)

* **MMU remap** (`CONFIG_MMU_REMAP`, `CONFIG_SGI_HANDLERS`, with `CONFIG_RPC`
  and `CONFIG_MMU` from `internals.h`) is on again, as upstream. It was off
  from boot log 9 while hunting the startup hang, which was the card. No
  core-1 start hijack (`CONFIG_INIT1_HIJACK` is not defined for the M50):
  core 1 is still parked and released by `m50_release_core1`, then takes the
  new MMU tables through RPC when the patches are applied.
* **Colour-table hook.** `include/platform/mmu_patches.h` replaces Canon's
  `E00E73BC` (checked against its first 8 bytes) with
  `xpan_m50_lucky_load` (`src/xpan-m50.c`). For now it makes exactly
  Canon's call (`E011E194(*E081E8EE(), a1, a2, table, 0x1000, a3)`) and
  records the table pointer, a checksum and the arguments of each still
  develop. **Colour table probe** shows the count ("N develops") and writes
  the records to `ML/LOGS/XM50LUT.LOG`.
* **Memory.** The remap needs 86 KB (one 64 KB ROM page copy plus tables)
  in a 64 KB-aligned buffer, so up to 64 KB of padding in front of it,
  depending on everything linked before it. With it, Canon kept 31 KB
  *less* than its boot minimum. Recovered: no module loader or TCC in the
  XPan M50 build (it ships no modules; `M50_110.sym` is still packaged so
  the card is recognised), key log 32 entries, exposure ring 32, boot marks
  384 (starts use up to 296). Canon now keeps 53 KB above the minimum.
* `build_xpan.sh` 6a now requires MMU remap and the hook with its
  original-bytes check, and fails on a core-1 hijack; `check_m50_rom.py`
  checks `E00E73BC`, the two calls it makes, the size/grid test at
  `E011E1AC` and the call from ShootDevelop.

### Colour table probe results (2026-11-09)

Auto-scans after each Picture Style change and after a photo
(`ML/LOGS/XM50LUT.LOG`; SET is Canon's Quick menu on the M50, so the probe
scans by itself). MMU patch applied (`apply_normal_patches` = 0, Canon's
`E00E73BC` starts with `0xF000F8DF`, the jump to XPan), no crash.

* **`PROP_PICTURE_STYLE`**: `0x81` Standard, `0x84` Neutral, `0x86`
  Monochrome (4 bytes).
* **39 tables** in RAM, the same addresses as in the RAM dump:
  * `0x694820`...`0x6B3858` and `0x6F91EC`...: fixed (decrypted style
    library), unchanged by the Picture Style.
  * `0x9E6938`...`0x9F1388`: **eight copies of the current style's table**,
    rewritten on each change: Neutral `698c473b`, Standard `bcd92614`,
    Monochrome `18ef1b87` (same checksum as the library's `0x694820`).
  * A 40th table (`0x9F74B8`) appeared after the photo.
* **The photo's develop** called the hook once, with table **`0x44032480`**
  (uncached RAM, outside the scan range; the hook only checksums cached RAM
  and ROM, so its checksum reads 0), args `E0545D4D`, `F7B484`, `1`.

So the shot's table goes through `xpan_m50_lucky_load`: XPan can pass its
own table there (and later the 17-point size), without touching Canon's
copies.

### Custom LUT, step 1: 10 points through the hook (2026-11-09)

* **Loading.** XPan's task loads the `.cube` selected in the HUD's LUT field
  (700 ms after the dial stops) with the EOS M parser (`xpan-cube.c`,
  `xpan-lut.c`) into one heap buffer (33-point capacity, 17-point if that
  does not fit).
* **Grading.** At each still develop, the colour-table hook converts the
  LUT onto the table Canon chose for that shot, with the EOS M conversion
  (`xpan_native_table_prepare_cube`, strength 100 %), cleans XPan's table to
  RAM and passes its uncached address. Canon's table is kept when the look
  is off, no LUT is loaded, a LUT is loading (the loader waits for the hook
  to finish before it touches the buffer), or Canon's table fails the EOS M
  format check.
* **Scope.** The saved JPEG only. Live View stays Canon's (its colour path is
  another one, not found yet). The LUTs expect Canon's Picture Style
  Neutral: the HUD says SET NEUTRAL otherwise (`PROP_PICTURE_STYLE` = `0x84`).
  `IDENT.cube` keeps Canon's table exactly (the reference).
* **Menu** XPan > Custom LUT: ON/OFF (default ON), "N graded", and in the
  help the load status, how many develops kept Canon's table, the format
  flags and the conversion time. The probe log's develop lines also show
  the table passed on (`used`).
* **Camera test (IMG_2639-2644).** Every shot went through XPan's table
  (format flags 0), the look-OFF shot kept Canon's, MONO came out exactly
  grey, IDENT matched OFF. The colour looks were much harsher than on the
  EOS M (RGB spread 106-117 against Canon's 79): the LUTs expect Cineon log
  input and got Canon Neutral. Applying the same LUTs to the IDENT photo on
  the PC reproduced the camera's result, and with `CINLV.cube` first gave
  balanced looks (spread 70-76; `extras/M50/lut-cineon-base-simulation.jpg`).
* **Tone base** (XPan > Tone base, default Cineon log): `ML/XPAN/CINLV.cube`
  (the EOS M's "normal render -> Cineon" base, so a first approximation for
  the M50's Neutral) is composed before the LUT, as on the EOS M without the
  headroom render. IDENT then shows the flat Cineon base. Without the file the
  shots keep Canon's table (HUD: NO CINLV). Canon Neutral = the old behaviour.
* `build_xpan.sh` 6a2 fails if the hook stops passing size `0x1000`, or if
  the table is not built by the checked conversion, not cleaned, or not
  passed by its uncached address.

### LUT submenu, Live LUT test, JPEG encoder probe (2026-11-09)

* **Custom LUT submenu** (PLAY opens it; SET is Canon's Quick menu): Cube
  file, Refresh Cube list, Cube strength (0-100 %), Tone base, Live LUT
  preview. LEFT/RIGHT on Custom LUT switches it on and off.
* **Live View path, from the ROM.** No Live View code calls the colour-table
  drivers: every caller of the loader (`E011E194`) and of the SusanDevelop
  wrappers (`E00E72F0`...`E00E73BC`) is in ShootDevelop (`E0082xxx`-
  `E0083xxx`). The photo develops from the style library (Neutral
  `a46c2721` = `0x69A820`), while eight RAM copies with another Neutral
  (`698c473b`, `0x9E6938`...) follow the Picture Style: probably Live View's.
* **Live LUT preview (experiment, default OFF).** Writes the look, built from
  Canon's copy with the same LUT, strength and tone base, into those copies
  (found by checksum, 4 or more alike in `0x9D0000`-`0xA10000`), again after
  Canon rewrites them, and puts Canon's table back when switched off. Buffers
  from the heap at first use (boot memory).
* **Camera test: Live View flickers.** With the look written into the eight
  copies, Live View switched between the look and Canon's colours all the
  time: Canon rebuilds those copies continuously. The RAM dump has a task
  stack caught doing it (`0x215EC0`: `ColorLucky.c` `E0630767`/`E0630B9D`,
  `ColorHistAlgor.c` `E03831CD`), and a Live View parameter block at
  `0x8D020` with the table pointer at `0x8D044`. The rebuild blends tables
  read at `<<12`/`<<13` offsets from a base (`E0630914`/`E0630982`): the
  4 KB spacing of the decrypted style library (`0x694820`...), which never
  changes and is also where the photo's table comes from (`0x69A820`).
* **Live LUT preview, second version.** XPan grades the library tables
  themselves (found in `0x680000`-`0x720000`, originals kept on the heap,
  each built from Canon's original with the photo's LUT, strength and tone
  base). Canon's own rebuilds then produce graded Live View tables, and
  photos come out graded from the library, so the colour-table hook passes
  Canon's table through while it is on. OFF puts the originals back.
* **JPEG encoder setup probe.** `SetJpegEncodePath` (`E0083CA2`) logs
  "Mode Size Aspect QScale" and "InYuvAddr OutYuvAddr Fno" and calls
  `E00E7F32` (`EPM/Jpeg/StlJpegEncMainDs`) with a block holding them
  (`+0x10` InYuvAddr, `+0x18`/`+0x1C` passed on together). A second MMU hook
  there (same 64 KB ROM page as the colour-table hook, so no extra memory)
  records the block and sample words of the YUV image, then runs Canon's
  function through a trampoline that replays its first 8 bytes. That block is
  where the film grain (YUV) and the crop (sizes) will start.
* `build_xpan.sh` 6a3 checks the hook address and original bytes, the
  trampoline, and that all MMU hooks stay in one ROM page.

### Live LUT works; JPEG encoder data (2026-11-09)

* **Live LUT preview works** (library grading): steady, follows the LUT.
  11 of the 27 library tables were graded; 16 failed the EOS M chroma-range
  check (flags 4) and stay Canon's. The Neutral ones used in the test graded.
* **JPEG encoder setup** runs once per photo, about 110 ms after the colour-
  table hook. Its block (`E00E7F32` argument), L JPEG 3:2:
  `0 0 0 0 5a180000 1e 40fd2000 2 647e4000 e0a9ae64 e0a9b060 1a 1`:
  `+0x08` size code 0 (L), `+0x0C` aspect code 0 (3:2), `+0x10` InYuvAddr
  (`0x5A180000`, uncached), `+0x14` 0x1E, `+0x18` a second buffer, `+0x20`
  `0x647E4000`, `+0x24`/`+0x28` ROM parameter tables, `+0x2C` 0x1A.
  No pixel sizes in the block: the encoder takes them from the size/aspect
  codes and the ROM tables.
* **The YUV image is already developed at that point**: words sampled
  every 4 KB are image data (YCbCr 4:2:2, e.g. `20fa2006`). So XPan's film
  grain can be added there, before Canon encodes, as on the EOS M.

### Encoder size tables and film grain (2026-11-09)

* **Encoder size tables.** `E00E7F32` picks its table as
  `[E0FD0488 + set*0x40 + aspect*16 + size*4]` (set 1 is empty). Words:
  `0` source width, `1` left crop, `2` top crop (pixels), then band and
  Huffman/buffer values not decoded yet. Aspect codes: 0 3:2, 1 1:1, 2 4:3,
  3 16:9. L: 3:2 `6000 0 0`, 1:1 `6000 1000 0` (-> 4000 wide), 4:3
  `6000 336 0` (-> 5328), 16:9 `6000 0 316` (-> 3368 high). So Canon always
  develops the full 6000x4000 for L and **crops when it encodes**, from these
  offsets: the way to the 65:24 JPEG (top crop 892 for 2216 rows).
* **Film grain** (XPan > Film grain, PLAY: Strength, Grain size): the EOS M
  "Film" engine (CPU texture, mid-tone weighted luma grain), added in the
  JPEG setup hook to the encoded area of the developed image, width and crop
  from the shot's size table. L only (the source height, 4000, is known
  there); other sizes are skipped. The texture is built in XPan's task (256 KB
  heap). The probe log records, per photo, mid-row samples at rows
  1000/2000/3000/3999 before the grain (is the whole image developed?) and the
  grain time. `build_xpan.sh` 6a5 guards the shot, address and geometry checks.

* **Camera test 1:** the JPEG (IMG_2652) has the grain over the whole frame,
  and the samples at rows 1000-3999 show the image is fully developed at the
  hook. But the camera crashed after saving it: 3 s after the shot the grain
  was still running (word by word through the uncached image, inside
  Canon's JPEG task). Now each row is burst-copied (LDM/STM, 32 bytes) into a
  cached heap buffer, grained there and copied back, with a 2 s budget: past
  it the grain stops, turns itself OFF and says so. Per-photo times are kept
  (menu help).

* **Camera test 2:** the budget stopped the grain at 3007 of 4000 rows
  (2022 ms) and turned it off; IMG_2653 was saved; the camera still crashed
  afterwards. Time alone does not explain it (2 s, and both crashes came after
  the save). ML serves allocations of 512 KB or more from `shoot_malloc`,
  Canon's shooting pool, which Canon expects empty ("can lock completely,
  Err 70, or crash", `src/mem.c`), and on DIGIC 8 freeing there is
  unreliable. The 512x512 int16 texture build buffer was exactly 512 KB. Now
  the texture is 256x256 (build buffer 128 KB, texture 64 KB), a 64 KB
  lookup table replaces the per-pixel multiply and clamp, and the budget is
  5 s. `build_xpan.sh` checks the texture size and the cube buffer size.

* **Camera test 3:** same crash after the save (IMG_2655), and the grain was
  still running 3 s after the shot. Neither time nor the shooting pool explains
  it. What every crash shared: the grain ran *inside Canon's JPEG task*, on
  its stack, with `snprintf` and seconds of CPU; with the grain off (hook only
  copies a few words) no photo ever crashed. A stack overflow there corrupts
  memory and kills the camera later. Now the hook only checks the shot and
  hands it to XPan's own task (`xpan_grain`, priority 0x1C, 8 KB stack) through
  two semaphores; Canon's task waits blocked (up to 6 s, then it encodes and
  the worker stops at the next row). Status text comes from the worker, with
  the copy and compute times split. `build_xpan.sh` fails if the hook side
  does any heavy work. (Replaced before testing, see below.)

* **Why it was slow, and the fix.** The real difference from the EOS M is the
  memory path, not the arithmetic. The grain read and wrote the image through
  its uncached alias (`0x5A180000`). On DIGIC 8 (Cortex-A9 with an L2 cache)
  every uncached access is its own trip to RAM: even 32-byte bursts reached
  only ~40 MB/s (test 2: 3007 rows in 2022 ms). The EOS M (ARM946, no L2)
  does not pay that, so the same loop was fast there. Holding Canon's JPEG
  task for seconds is what every crash had in common.
  Now the image is grained **in place through its cached alias** (`0x1A180000`),
  16 rows at a time, with **Canon's own cache functions** in Canon's order:
  invalidate L2 (`E00EA712`, PL310 by address) then L1 (`E0577768`) before
  reading what the image DMA wrote (Canon does exactly this at `E0200678`);
  clean L1 (`dcache_clean`) then L2 (`dcache_clean_multicore`) after writing,
  so the JPEG encoder's DMA reads the grained rows. The pixel arithmetic is
  the EOS M one again (512-byte gain table plus one texture row, which fit in
  L1; the 64 KB lookup table did not). It runs inline in the hook as on the
  EOS M (no worker task, no printing; the menu formats the numbers), with a
  1 s safety budget that turns the grain off if ever exceeded. The 64 KB
  lookup table and the 12 KB row buffer are gone from the heap.
  `build_xpan.sh` 6a5 checks the cached alias, the four cache calls and their
  order, the alignment guard, the budget, and that the hook does not print,
  sleep or allocate.

### Camera test 5: measurements (2026-11-09)

* **Benchmark (XPan's own task, heap buffer):** 16 rows of grain arithmetic
  8578 us; **one row with interrupts off 528 us** (88 ns per pixel, about
  5 ns per instruction of the ~15 per pixel); 180 KB copy 985 us (~183 MB/s:
  every L1 miss goes to DRAM, RAM is outer non-cacheable); Canon's cache
  calls for 192 KB: L2 invalidate 187 us, L1 invalidate 533, L1 clean 537,
  L2 clean 182. So the loop is CPU-bound and just as slow without any other
  task or interrupt: a full L frame needs about 2 s of CPU on one core.
* **Shot:** stopped at 0.3 s after 416 of 4000 rows (arithmetic 253 ms, cache
  calls 55 ms), CPSR `80000053` (SVC, IRQs on). The camera still **froze by
  itself, then Canon requested shutdown** (`PROP_TERMINATE_SHUT_REQ` 3) 15.6 s
  after the shot. **The hold time is not the cause.** All JPEGs are complete
  (end `FFD9`) and grain shots are not larger than normal ones (IMG_2661
  4.9 MB), so not a JPEG buffer overflow either.
* What a grain-ON shot does that a grain-OFF shot does not: writes the image,
  uses more stack in Canon's JPEG path task, `cli`/`sei`, and (when cut short)
  turns Film grain off (a settings save).

### Camera test 6 build: stack and worker (2026-11-09)

* The hook logs Canon's JPEG path task: name, stack start and size, the stack
  pointer at the hook (bytes free), and, counted later in XPan's task, the
  untouched `0x19980218` fill words (the deepest use so far). Recorded for
  every L shot, grain on or off.
* The grain runs in XPan's own task (`xpan_grain`, 8 KB stack), like Canon's
  Grainy filter, which is its own art-filter stage; Canon's task only checks,
  hands over and waits (2 s at most, then the worker stops at the next band).
* Film grain is never switched off during a shot (no settings save then).
* L2 calls removed (RAM is outer non-cacheable); L1 invalidate/clean stay.
* Same 0.3 s cut-off as test 5, so the results compare.
* `build_xpan.sh` 6a5: L1-only cache calls in order, worker present, the hook
  does no grain work, nothing writes `film_grain=0`, stack logging present.

* **Camera test 6 result:** Canon's JPEG path task is `ShtEncode`, stack
  4096 bytes at `0x208108`; at the hook 3832 bytes free, deepest use so far
  leaves 2936 bytes untouched. **Stack ruled out.** The grain shot then hung
  with the quick preview on and "saving..." forever (battery pull): the hook
  drained a stale "done" with `take_semaphore(sem,0)`, and a timeout of 0
  means *wait forever* in DryOS, so `ShtEncode` blocked before handing over
  (the log shows 0 rows, not late). Fixed with `take_semaphore_now`;
  `build_xpan.sh` fails on any `take_semaphore(...,0)` in `xpan-m50.c`.

* **Camera test 6b result:** grain in XPan's own task, 288 rows in 309 ms,
  Canon's task not late, JPEG saved complete; then **Err 70** flashed and
  Canon requested shutdown 18 s later. With the stack, hold time and settings
  save ruled out, the trigger is changing Canon's image.

### Code review before test 7 (2026-11-09)

* **Cross-core races:** `cli()` masks only the current core, and M50 tasks run
  on both. The grain busy flag, the look busy flag (colour hook vs Live LUT vs
  cube load) and the learned options (PropMgr vs XPan task) relied on it.
  Now: DryOS semaphores taken with `take_semaphore_now` in Canon's tasks
  (`xpan_grain_lock`, `xpan_look_lock`) and a seqlock for the options.
* **Abort race:** after a 2 s timeout Canon could encode while the worker was
  still writing a band; Canon now waits up to 500 ms more for the worker.
* **Cache hygiene:** after writing, `dcache_clean_invalidate` (Canon
  `E0577834`, MCR c7,c14,1) instead of clean only, so no image lines stay
  cached.
* Checked and fine: semaphore timeouts, alignment (image start and the
  12000-byte pitch are whole cache lines), arithmetic ranges, `isqrt`
  argument, ROM table reads, tail reads. Log rings guarded by `cli()` may lose
  an entry on a cross-core collision (logs only).
* `build_xpan.sh` 6a5 now fails on any `cli()` busy flag, missing locks or
  seqlock, the abort wait, and the clean+invalidate order.

### Camera test 7: unchanged writes

`GRAIN_TEST_UNCHANGED 1`: the exact grain path (worker, cache calls, timing,
writes) with an all-zero gain table, so every pixel is written back with its
own value (status "TEST: unchanged writes"). No crash means the changed
content is the trigger; a crash means the writes themselves.

* **Camera test 7 result:** unchanged writes (256 rows, 304 ms) still froze the
  camera; a "Battery" error with a red cross showed (battery full) and the next
  boot logged `PROP_ABORT (battery/card door) -1`, consistent with a system
  hang. **The content is not the trigger.** Correction: tests 4/5 only showed
  that 1 s and 0.3 s holds both freeze, not that the hold does not matter; a
  short hold was never tried.

### Camera test 8: hold only

`GRAIN_TEST_HOLD 1`: the worker touches nothing and only waits while Canon's
`ShtEncode` task waits as before: 20 ms on the first grain shot of the boot,
100 ms on the second, 300 ms from the third. A freeze at some hold = holding
`ShtEncode` (encoder clock on, Start queued) is the trigger; no freeze even at
300 ms = the image access is.

* **Camera test 8 result:** one grain shot (IMG_2666). The worker touched
  nothing and only slept; the requested 20 ms became **97 ms** (the worker,
  priority 0x1C, wakes late while Canon processes the shot). The camera hung
  at the next half-shutter press ~10 s later and Canon requested shutdown
  (`PROP_TERMINATE_SHUT_REQ` 3). **With no image access at all, holding
  `ShtEncode` ~100 ms is enough.** Holding Canon's JPEG path task is the
  trigger; the EOS M tolerated it, the M50 does not.

### Hardware grain: research plan (2026-11-09)

Decision: the grain must use Canon's hardware engine, run the way Canon runs
it (a pipeline stage Canon schedules and waits for), never a timed hold of a
Canon task. Facts so far:

* **Engine job:** `SAS_RoughMonochrome` (`E08B9E20`) reads the image size,
  allocates a 0x40-byte job and calls `E08D4AB8`, which copies from Canon's
  grain asset a **0x4000-byte seed**, an **offset table (0x5BF entries)** and
  parameters: the same seed + offset design as the EOS M native grain
  (`GRAIN.BIN`, `xpan-film-grain.c` "Canon" engine).
* **Path:** `StartRoughMono` (`E08557CE`) / `TermRoughMono` (`E08557F2`) post
  events 5 / 6 to the CreativeFilterPath task (`CreativeFilterPath.c`,
  `StartRoughMonoPath(%#x)`); the engine driver is
  `EFM/Filter/RoughMonochromeYuv/RoughMonochromeYuv.c`, with EFM resource locks
  (`EfmErscLockResources(this->ResLockEntryForRoughMonochrome)`). A Live View
  version exists too (`LvDevRoughMonochromeYuvPath.c`).
* **Scheduling:** in a creative-filter shot the art filter state machine runs
  the filter as its own stage, then its own `sasEncodeJpeg`; the job carries
  `DcsParam.LVCFilterSetting.FilterKind` (`PROP_LV_CFILTER`, per shooting mode).

Driver trace (2026-11-09, continued):

* **CreativeFilterPath task:** `StartRoughMonoPath` (`E086212C`) locks the EFM
  resources (`E0558386`), turns on the engine clock (`PwrMng` `E0557ECE`),
  initialises the driver (`E086D94C`, three callbacks) and starts it
  (`E086DA10`, the job), then acknowledges its caller.
* **EFM driver** (`RoughMonochromeYuv.c`): `E086DA10` copies the job into a
  0x44-byte parameter block; `E086D982` runs the engine: init `E088B2FC`,
  configure `E088BA2C`, start `E088BB9A`, then waits for
  `EVENT_COMP_PATH | EVENT_ABORT` up to 3000 ms (asserts on timeout), then
  stops (`E088BBE0`/`BC06`/`BC12`). So the wait is inside Canon's own path
  task, by design.
* **Hardware:** the configuration programs EDMAC channels (`ELD/Edmac/Edmac.c`:
  `E054ADBE` set, `E054A9AE` start) and EHD effect modules; the modules next to
  it are `ColorSub`, `RestartCopy`, **`MaskUVSub`** (U/V masking: the
  monochrome step) and `OhyMag`. Same engine + EDMAC style as the EOS M
  native grain.
* **When Canon runs the stage** (`CreateJob` `E055D6EA`, decision `E01E340A`):
  filter kind set (`DcsParam+0xE8C != 0`) **and** (shooting mode `+0` in
  0x1E..0x23 **or** `+0xAC == 3`) **and** `+0xF64 == 0` (`E01E342C`).
  `DcsParam` = `[[0x5408]+0x40]`; in the RAM dump (camera in M: `+0 = 3`)
  **`+0xAC = 3`** and `+0xF64 = 0`: **in M mode only the filter kind is
  missing** for Canon to route the shot through its own art filter stage.
  The filter kind comes from `PROP_LV_CFILTER` (per shooting mode, log
  `PROP_LV_CFILTER = shotmode:%#x FilterKind:%#x`); the direct writers of
  `+0xE8C` found are in-camera RAW development (`InnerDevelopState.c`,
  `indevMakeArtFilterParam`) and initialisation.

* **Properties** (Canon's property table, entries handler / id / store /
  offset / size): `PROP_LV_CFILTER` = **0x8004004F**, 40 bytes at store
  +0x474 (`E0F1E510`; default `PropstDefaultParam.c`: filter kind, a second
  word, 8 per-filter values); `PROP_MOVIE_CFILTER` = **0x8004006E**, 32 bytes.
  The store's handler for 0x8004004F is a bare `bx lr` (no mode check in the
  store; the menu restricts it).
* **Read-only study build:** the CPU grain is off (`M50_GRAIN_AVAILABLE 0`;
  test 8). XPan logs both creative filter properties whenever Canon reports
  them, and per JPEG Canon's own decision `E01E340A` (called, after checking
  its `DcsParam` pointer) with the fields it uses, to `XM50LUT.LOG`.
  `check_m50_rom.py` verifies `E01E340A` and its literal.

Read-only camera test results (2026-11-09, `XM50LUT.LOG`):

| Shot | `PROP_LV_CFILTER` words 0..3 | DcsParam mode / +AC / kind / +F64 | Canon's decision |
|---|---|---|---|
| M | 0 0 0 0 | 3 / 3 / 0 / 0 | 0 |
| Grainy B/W, three strengths | 1 0 0 *L* (*L* = 0, 1, 2) | 1E / 3 / 1 / 0 | 1 |
| Soft focus | 2 0 0 0 | 1F / 3 / 2 / 0 | 1 |
| M again | 0 0 0 0 (Canon writes zeros on the mode change) | 3 / 3 / 0 / 0 | 0 |

* Filter kind: 1 = Grainy B/W, 2 = Soft focus; shooting mode = 0x1D + kind.
  The strength of filter *k* is word 2 + *k* (Grainy B/W: word 3, values
  0..2; the same index `E02017E6` uses, `+0xE94 + 4*kind` in DcsParam).
* `+0xAC` is 3 in every mode, so in M **only the filter kind is missing**.
* Canon writes `PROP_LV_CFILTER` to zeros itself when the mode changes back to
  M; `PROP_MOVIE_CFILTER` stays zero.

ROM follow-up (2026-11-09):

* **How the setting reaches the shot:** Canon's DCS property handler
  (`E01E162C`, its switch on the property id) sends `0x8004004F` to
  `E01E22A6`, which copies the 40 property bytes to `DcsParam+0xE8C`. The
  other writers of `+0xE8C` are in-camera RAW development
  (`InnerDevelopState.c`, `E0201366`/`E02017E6`) and `E01E3470`, which
  clears a branch job's own copy (`SRM_AllocateMemoryResourceForBranchJobObject`
  path), not the global settings. So the standard way to get Canon's grain
  stage on an M shot is Canon's property API with the bytes Canon itself
  sends (as XPan's learned-settings writer does), not a write to `DcsParam`.
* **Canon's grain data:** `E08D4AB8` builds the job from Canon's grain asset:
  a 16 KB seed (+0), one byte (+0x4000), an offset table of 0x5BF halfwords
  (+0x4000 area, 0xB7E bytes), 20 bytes of parameters (+0x7004) and **one 4 KB
  table per strength** (+0x4001 / +0x5001 / +0x6001 for strength 0 / 1 / 2).
* **Engine:** `E088BA2C` scales between the job's two sizes, programs
  `MaskUVSub` (`EHD/Effect/MaskUVSub`) and `ELD/Lucas` (seed size 0x400), and
  starts two EDMAC channels. Whether the U/V masking (the monochrome step)
  can be left out is still open; Canon's Grainy B/W output is monochrome.

### Canon grain test (2026-11-09, waiting for the camera test)

Menu `Canon grain test`: Off / Canon strength 0 / 1 / 2. It switches on
Canon's own Grainy B/W stage for M photos the standard way, through Canon's
property API (`_prop_request_change(PROP_LV_CFILTER)`); Canon's DCS handler
then copies the value to the shot settings and Canon's decision `E01E340A`
schedules its art filter stages. Rules (each a build check):

* Written only by `m50_cgrain_write_now`, from XPan's task, only in M, and only
  when Canon's current value is one Canon itself sent (all zero, or kind 1
  with strength 0..2 in word 3, everything else zero); it writes only such a
  value. Not while half-pressed, busy or recording. `PROP_MOVIE_CFILTER` is
  never written.
* Menu requests go through a sequence counter (no lost request).
* Canon writes zeros itself on a mode change; XPan does not write again by
  itself, the menu says "Canon: off". Not saved: off at every start; a value
  left from the previous start in M is set back to zeros once (3 s after
  start).
* The CPU grain stays off; the JPEG log still records Canon's decision per
  shot.

**Result (camera test, 2026-11-10):** it works. In M, Canon accepted every
write (`PROP_LV_CFILTER` reports `1 0 0 L`, L = 0/1/2; Off = zeros), its
decision `E01E340A` returned 1 with `DcsParam` mode 3, kind 1, and the photos
came out black and white with Canon's grain. No hang, no error. Also from
the log:

* XPan's colour-table hook still runs (same table each shot), and XPan's JPEG
  hook still runs once per shot, about 100 ms after the develop, as in a
  normal shot.
* The YUV XPan's JPEG hook sees is still **colour** (chroma bytes at rows
  1000..3999 are far from neutral, as in the colour shots), but the saved JPEG
  is black and white. So Canon's art filter stages work on their own copy
  and its own JPEG encode (`sasEncodeJpeg`), which does not pass through
  XPan's JPEG hook. Anything XPan does to the saved JPEG (crop, watermark)
  must then be in the art filter path, not in `SetJpegEncodePath`.

**Colour study (ROM, 2026-11-10):**

* The grain job (`SAS_RoughMonochrome` `E08B9E20`, 0x40 bytes): +0 input
  YUV, +0xC a separate output buffer, +0x10..+0x1C sizes, +4 the 16 KB seed,
  +0x30 the offset table, +0x34 its count, +0x38 the 20 parameter bytes,
  +0x3C the strength's 4 KB table, +0x2C one asset byte. **No field about
  colour.**
* The engine functions the driver calls (`E088B2FC` init, `E088BA2C`
  configure, `E088BB9A` start) are the EHD effect module **`MaskUVSub`**
  itself (its source file and assert strings sit in that range; `ColorSub`,
  `RestartCopy`, `AddPony` are separate modules next to it). So Canon's grain
  is one hardware pass that applies the grain mask and removes U/V together
  ("Mask" + "UV Sub").
* Its registers are a block at `0xD049C000`, programmed through generic
  setters (`E047B034`, `E047AFC4`, `E047B17E`, `E047B1AA`..`E047B1CE`,
  `E047B2B6`) that about 20 other effect drivers share (LucasPony, PonyFilter,
  DodgePath, FilterIndy, Beauty*, ...) with different values. Which
  bits remove the colour is not stated anywhere in the ROM.
* So Canon's grain driver has **no setting for colour**. Keeping colour would
  mean changing what Canon's driver writes to the engine: a code patch in ROM
  page `E0880000` (another 64 KB page remap; ML's static margin is about
  42 KB), and finding the right register values on the camera by trial.

### XPan's own grain in Canon's filter stage (2026-11-10)

Goal: XPan's own colour Film grain, run the way Canon runs its grain, as a
stage of Canon's art filter pipeline (Canon's engine only gives B/W).

**How Canon runs a filter stage** (ROM):

* The art filter task's stage (`SAS_RoughMonochrome` `E08B9E20`) builds the
  job, then calls `StartRoughMono` (`E08557CE`) and `TermRoughMono`. Each
  posts to the CreativeFilterPath task and **waits for its answer on a
  semaphore, up to 10 s** (`E0545D52` -> `E0545D0C`, `movw r2,#10000`; on
  timeout it logs `SemTimeOut %s` and asserts). So Canon designed this
  stage to take seconds: compare the JPEG task, where test 8 hung after
  about 100 ms.
* `StartRoughMonoPath` (`E086212C`, CreativeFilterPath task): logs, locks
  the EFM resources, turns the engine clock on, initialises the driver, runs
  it with the job (`E086DA10`, which waits for the engine itself), then
  answers the waiting stage (`reply->callback(reply,0,0)`).
* **XPan's grain slot:** the driver run `E086DA10(job)`. There Canon has
  finished every lock and gives the input (`job+0`) and a separate output
  buffer (`job+0xC`) with their sizes; whatever fills the output is what
  Canon's later stages (resize, trimming, `sasEncodeJpeg`) use. Canon's
  stage waits for it by design.

**Memory for a second hooked page:** the hook is in ROM page `E0860000`,
and each remapped page needs 64 KB of RAM. ML's MMU buffer
(`generic_mmu_space`) was a 64 KB-aligned array inside `patch_mmu.o`, so the
linker padded twice: once for `patch_mmu.o`'s `.bss`, once for the array
itself (about 128 KB lost). Now:

* `src/patch_mmu_space.c` holds the array, sized by
  `GENERIC_MMU_SPACE_SIZE` (`patch_mmu.h`) for `CONFIG_MMU_REMAP_PAGES`
  pages (default 1; M50: 2, `internals.h`).
* `src/magiclantern.lds.S` puts that object first in `.bss`, so it starts at
  the first 64 KB boundary and nothing else is padded for it.
* `calc_mmu_globals()` sized the L2 tables with `sizeof(MMU_L2_TABLE_SIZE)`
  (4 bytes) instead of 0x400; fixed, so the fit check is real for 2 pages.
* Result: two pages, and ML's memory margin went from 42 KB to **105 KB**
  (`M50 memory` line in the build). Build checks: pages hooked <= pages
  reserved, the array first in `.bss`, the L2 size fix.

### Grain stage probe (2026-11-10, waiting for the camera test)

Read-only hook on `E086DA10` (`xpan_m50_cf_grain_run`, trampoline replays
its 8 bytes, `check_m50_rom.py` checks them, the caller and the 10 s wait).
Per call it logs to `XM50LUT.LOG` ("grain stage at ..."): the 16 job words,
image words from input and output (uncached, before and after Canon's
driver), the task, its stack and free bytes, and **how long Canon's grain
driver takes**. Canon's driver always runs; the probe writes nothing. Test
with the Canon grain test option (M mode), as before.

**Probe result (camera test, 2026-11-10):** the new memory layout boots.
Per Grainy shot in M: the stage runs about 370 ms after XPan's JPEG hook, in
Canon's task **ShtVfx** (stack 4 KB, 3880 bytes free at the hook). Job:
input `0x6F960000` (uncached alias), output `0x53873800`, width 6000 in
+0x10/+0x14/+0x18, 4000 rows in +0x1C, `+0x24/+0x28` = 2976 x 1984 (Canon's
grain texture size). **Canon's engine takes 155 ms** for the whole image.
Input words are colour (`1bf61d11`); the output after Canon's engine has
zero chroma and new luma (`12001800`): MaskUVSub really writes Y with grain
and U = V = 0. `E086DA10`'s only caller is `StartRoughMonoPath`, which
ignores its result; besides the engine run it only builds Canon's grain
texture (`E086DBEC`) and its parameter block, and the driver init creates
nothing per shot, so replacing the run leaves no Canon state behind.

### XPan Film grain in Canon's grain stage (2026-11-10, waiting for the camera test)

* **Switch:** Film grain ON + M mode: XPan's task sets `PROP_LV_CFILTER` to
  Canon's Grainy B/W value (kind 1) through Canon's property API, only from
  and to values Canon sends, one write per Canon report (no loop when Canon
  resets it on a mode change: XPan sets it again after Canon's next report).
  Film grain OFF: zeros, Canon's M value. The "Canon grain test" menu is gone.
* **Stage:** in `E086DA10`, with Film grain on and a job that passes the
  checks (equal widths, multiple of 16, rows, 32-byte alignment, both buffers
  in RAM and not overlapping), XPan's worker task writes Canon's input to
  Canon's output with the Film grain on the luma (chroma copied); Canon's
  ShtVfx waits blocked on a semaphore, as it does for its engine, up to 9 s
  (Canon's stage waits 10 s). The worker grains for at most 6 s, then copies
  the remaining rows plainly (about 0.3 s for the whole image), so the
  output is always complete. Cache: invalidate input and output bands before,
  clean the output after, nothing left cached. If the texture is being built
  (settings just changed), ShtVfx copies the image without grain (colour).
  Any other job: Canon's own driver runs (B/W).
* Expected time: about 2 s of grain per photo on one core (88 ns per pixel,
  camera test 5), shown in the Film grain menu help and logged ("grain stage
  at ...: XPan grain N ms").

**Result (camera test, 2026-11-10):** XPan's grain ran in Canon's stage on
both Film grain shots, **about 3.0 s each** (2978 / 3001 ms, whole 6000 x
4000 image, worker task, ShtVfx waiting blocked, 3856 bytes of its stack
free); the output keeps the input's chroma (`16fe1902` -> `16fe1702`) with
new luma, i.e. colour with grain. XPan set the filter at start (4.7 s) and
set it back to zeros when Film grain went OFF; the OFF shot was a normal
shot. No hang.

User check: the photos have the LUT and visible grain ("works well"); **Live
View turned black and white** while Film grain was on.

### Grain speed (2026-11-10, waiting for the camera test)

3 s per photo is too slow (the EOS M was faster on a weaker CPU).

* **The loop:** the `-Os` build of the stage row used about 37 instructions
  per 2 pixels, with stack spills and every source word loaded twice
  (possible aliasing). `film_row_copy` is now written with `restrict`
  pointers and a running texture index (same values) and compiled with
  `optimize("O2")`: 22 instructions per 2 pixels, all in registers, one
  load and one store per word.
* **Second core:** the grain worker runs on CPU 1 (`task_create_ex(...,1)`):
  the second core is often idle while Canon keeps the first busy. The
  texture and amplitude table are written back (`dcache_clean`) after
  they are built, so the other core reads them from RAM.
* **Measurements in this build** (`grain bench` line, once per boot, XPan's
  task): 1 M multiply-adds (CPU speed), 1 MB copied within L1 and through
  RAM, the old in-place row and the new stage row with interrupts off.
  Per photo: XPan grain time, worker CPU, rows grained.
* Memory: this round's code moved the start of `.bss` just past a 64 KB
  boundary, so the 64 KB-aligned MMU buffer is padded again (margin 39 KB,
  still above the 32 KB check). To do: a layout that does not depend on where
  `.bss` starts.

**Speed test result (2026-11-10):** XPan grain **2.2 s** per photo (was
3.0 s), worker on CPU 1, all 4000 rows. Bench (CPU 0): old row 531 us, new
stage row 425 us (only 20% faster for 40% fewer instructions); **1 M
multiply-adds 15 ms** (a 3-instruction `mla`/`subs`/`bne` loop: 15 ns per
round, about 3 x slower than this CPU should be); **1 MB copy inside L1
6.1 ms, through RAM 6.3 ms** (the cache does not help). So XPan's code
and data seem to run without effective caching (code at about 1 MB,
`0xDC870`..). Next build logs, per CPU, SCTLR/ACTLR/TTBR/TTBCR and the MMU
descriptors of XPan's code, data, the texture and the photo buffer, with
two timed passes over the texture ("cache facts"), read only.

Live View probe result: Canon's Live View grain runs in **FilterExecTask**,
frames of **1620 x 540** (input `0x6B350000`, output `0x6A3D0000`), Canon's
engine **5.7 ms per frame** (max 6.9 ms). At the measured copy speed a CPU
copy would cost about 10 ms per frame, so the Live View fix waits for the
cache facts.

### Live View without Canon's filter (ROM study, 2026-11-10)

Goal (user): Canon's Live View filter must not run at all (no copy, nothing
overwritten), so XPan's Live LUT preview works as on the EOS M, with no
overhead.

* **How Live View picks up the filter:** Live View's property handler
  (`E048E708`, case `PROP_LV_CFILTER`; it defers the change while AF runs,
  "Save PROP_LV_CFILTER", and applies it later from `GmtAfState.c`,
  "PROP_LV_CFILTER ReStart") calls **`E048CB24(ctx, 1, filter data, 0)`**,
  which maps the filter kind with **`E0776CC6(kind)`** (`LvCFilterCommon.c`:
  Canon's table, entry 0 = no filter) into the Live View filter
  (`ctx+0x55C`, level `+0x454`) and rebuilds Live View with or without the
  post-filter path (`LvDevMvPostCfilController.c`; its filter type switch
  `E08D1494`, kind 1 -> type 2 = RoughMono, dispatch table at RAM `0x1A69C`,
  32 bytes per type).
* `E0776CC6`'s **only caller** is `E048CB24`. Photos take the filter from
  `DcsParam` (Canon's DCS handler `E01E22A6`), not from this.
* **Clean fix:** hook `E0776CC6`: when the kind is 1, Film grain wants the
  stage and the camera is in M, return Canon's own "no filter" entry
  (`table[0]`); otherwise Canon's value. Live View is then built exactly as
  without a filter (nothing to copy, no per-frame work), photos still get
  XPan's grain stage.
* **Memory:** `E0776CC6` is in ROM page `E0770000`, a third remapped page
  (64 KB). The margin is 39 KB because the 64 KB-aligned MMU buffer is padded
  again by about 64 KB (`.bss` now starts just past a boundary). Fix first:
  map remapped pages with 4 KB small pages (L2 small-page descriptors, 16
  per 64 KB block), so the RAM copies need only 4 KB alignment (the L1 table
  16 KB): the padding then stays under 16 KB whatever the code size, and a
  third page fits (about +65 KB from a ~100 KB margin).

### 4 KB pages and the Live View filter map (2026-11-10, waiting for the camera test)

* **4 KB pages** (`CONFIG_MMU_REMAP_SMALL_PAGES`, M50 only): a remapped 64 KB
  ROM block is mapped with 16 small-page L2 entries
  (`replace_large_page_with_small_pages_in_l2_table`, `mmu_utils.c`) instead of
  one large page, so its RAM copy needs only 4 KB alignment. Flags: Canon's
  large-page flags converted bit by bit (XN 15 -> 0, TEX 14:12 -> 8:6). Checked
  offline on the ROM tables: RAM section TEX 4 / C 0 / B 1 (inner write-back,
  outer non-cached), shared; ROM-copy entry `0x716` keeps exactly that, read-only
  like the ROM; RAM is identity-mapped (sections at base = address).
* **Page copies from the heap:** `generic_mmu_space` now holds only the L1 table,
  3 L2 tables and their metadata (16 KB aligned, first in `.bss`). The three
  64 KB copies are allocated by `reserve_heap_pages()` (malloc, 4 KB aligned,
  never freed) in `patch_memory_rom()` **before** it disables interrupts and
  parks cpu1; inside the critical section the code only takes the next
  pointer. Normal patches only (the M50 has no early patches; build check).
* Result: three hooked pages and **218 KB** margin (was 39 KB with two).
* **Live View filter map hook** on `E0776CC6` (third page): for kind 1 with
  Film grain on in M it returns Canon's own value for kind 0 (no filter);
  otherwise Canon's value. The trampoline reproduces the first 8 bytes,
  including the `bcc` (kind < 8 -> `E0776CE4`, else Canon's error path
  `E0776CCE`). If Live View was mapped before Film grain was known (at
  start) and shows Canon's filter, XPan's writer sets the filter to zeros
  once and back to kind 1 after Canon's report (logged: "live view filter
  map: ..."). The Live View grain probe on `E086DB00` stays: it should count
  **no new frames** once Live View has no filter.

**Camera test (2026-11-10): works.** The camera started with the 4 KB pages,
Live View showed the LUT in colour (filter map: 1 call, "no filter" for XPan's
grain), photos had the grain (2.2 s each, CPU 1).

**Cache facts (both CPUs identical):** SCTLR `40c5187d` (MMU, D-cache,
I-cache, branch prediction on), ACTLR `45` (SMP). XPan's code (large page
`f4415`), data, texture and the photo (sections `..4406`) are all TEX 4 / C 0 /
B 1: inner write-back, **outer (L2) non-cacheable**, like all of Canon's RAM.
Two passes over the 64 KB texture: 201 then 196 us (~100 ns per line both
times): it does not fit the 32 KB L1 and there is no L2 for RAM. 1 M
multiply-adds: 12 ms. So nothing is mis-set; the CPU work is the limit.

### Grain on both CPUs (2026-11-10, waiting for the camera test)

Two stage workers, `xpan_stage1` on CPU 1 and `xpan_stage0` on CPU 0, each
grain one half of the image (split on whole 16-row bands, so they never share
a cache line; same seed and absolute row numbers, so the grain is seamless),
each with its own 6 s cap. ShtVfx gives both their half and waits for both
within 9 s in total, then gives the grain lock back. CPU 0 is mostly idle
during the stage (ShtVfx waits blocked). Log: "grain stage ... workers: cpu
1 N ms, cpu 0 N ms". Expected about half of 2.2 s.

**Two-CPU result (2026-11-10):** XPan grain **1.43 s** per photo (was
2.2 s): CPU 1 1.35 s, CPU 0 1.43 s for half the image each (not half of 2.2 s:
both cores share the memory bus). The user could not feel the difference.

### 65:24 JPEG: Canon's encoder table (ROM study, 2026-11-10)

`E0FD0488 + aspect*16 + size*4` -> a 20-word table per size and aspect
(`StlJpegEncMainDs.c`, "pParamTbl"). L tables:

| aspect | w0 width | w1 left | w2 top | w3-w5 strips | w6-w9 | w10 | w11 | w14,w15 |
|---|---|---|---|---|---|---|---|---|
| 3:2 | 6000 | 0 | 0 | 672 672 624 | 8 8 8 8 | 499 | 2250 | 3 4 |
| 1:1 | 6000 | 1000 | 0 | 800 800 800 | 4 8 8 8 | 499 | 1250 | 5 8 |
| 4:3 | 6000 | 336 | 0 | 672 672 624 | 7 8 8 8 | 499 | 2664 | 1 1 |
| 16:9 | 6000 | 0 | 316 | 672 672 624 | 8 8 8 8 | 420 | 2250 | 3 4 |

w3-w9: the width in horizontal strips (8 x 672 + 624 = 6000, 4 x 800 + 800 =
4000); **w10 = rows / 8 - 1** (499 for 4000, 420 for 3368). A 65:24 JPEG
(6000 x 2216) = w2 892 (always the middle of the sensor, as on the EOS M;
Frame alignment only moves the frame on the screen), w10 = 276. The file header and EXIF sizes come from elsewhere (the 3:2
photos IMG_2680-2693 all have SOF and EXIF 6000x4000). Plan: use Canon's own
aspect pipeline (16:9 already crops at the encoder and writes a matching
header, EXIF, thumbnail and RAW crop record) and supply 65:24 values to what
Canon's 16:9 uses. Next: a 16:9 test (normal and grain photo) to see the
encoder block, header and EXIF Canon writes for an aspect crop.

**16:9 test (2026-11-10):** IMG_2694 (normal) and IMG_2695 (grain) are both
6000x3368 in SOF and EXIF. The encoder block had aspect code 3; in the grain
path Canon's filter job already had **3368 rows** (same input address as in
3:2): the filter path takes its size from Canon's size data. XPan's aspect
reading (`PROP_IMAGE_ASPECT_RATIO`) stayed 0 during the 16:9 shots: not the
M50's aspect property.

* **Canon's live size table:** pointer at `0xB4D4` (RAM dump: `0x405FC068`),
  read by Canon's size getter `E0081E4E(type, aspect, &w, &h)`: per size, four
  aspect pairs (+0x48 L 3:2 6000x4000, +0x50 4:3, +0x58 16:9 6000x3368,
  +0x60 1:1; M at +0x68.., S1 +0xC8.., S2 +0xE8.., S3 +0x128..). The spec block
  `0x759E4` is only read by in-camera RAW development.

### 65:24 JPEG via Canon's 3:2 size (2026-11-10, waiting for the camera test)

Always the middle of the sensor (user). While XPan's 65:24 frame is on (`m50_crop_poll`,
XPan's task, not while half-pressed/busy/recording), XPan changes, through
ML's patch API (`apply_patches`, old values checked; undone when 65:24 is
off or on any error):

* Canon's live size table, 3:2 L height: 4000 -> **2216**;
* the 3:2 L encoder table (`E0FCFF88`, ROM page `E0FC0000`, runtime MMU
  patch, 4th heap page): w2 top 0 -> **892**, w10 499 -> **276**.

So Canon itself writes a 6000x2216 JPEG, rows 892-3108 (centre band), with
its own header, EXIF, thumbnail and RAW crop record, in the normal and the
grain path. Only L 3:2 is changed. Menu "65:24 JPEG crop" shows the state;
the log has "65:24 JPEG crop: ..." and, per grain photo, "filter input starts
at row N of the developed image" (expected 892 if Canon centres the filter
input too).

* The M50 Live View matte still marks the band at its layout position
  (Top/Split/Bottom), not the centre: centring it would break the HUD
  layouts (frame at row 0/60/214 of 480, centre would be 107). The fix is to
  move the Live View picture so the sensor centre shows in the band, as on
  the EOS M: the Live View task.
* **Heap pages, corrected:** `apply_patches()` disables interrupts before it
  calls `apply_patch()`, so `reserve_heap_pages()` in `patch_memory_rom()`
  ran malloc with interrupts off at boot. It is now called once, first, in
  `apply_normal_patches()` (task context) and reserves all 4 pages (also the
  runtime crop page); build checks forbid it anywhere else.

**Crop test (2026-11-10): works.** IMG_2696 (normal) and IMG_2697 (grain) are
6000x2216 in the JPEG (SOF); IMG_2698 (XPan 3:2) 6000x4000. Both 65:24 crops
match the 3:2 photo near row 1000 (normal ~1050, grain ~980; handheld, so a
little camera movement): both paths give the centre band. Grain on 2216 rows:
0.8 s. To do: the **EXIF height still says 4000** (Canon fills it from
another source). The input-row probe found no match (-1).

### ROM attributes in hooked sections (2026-11-10, waiting for the camera test)

User: Live View stuttered, "like the previous frame stayed longer, leaving a
trail", in the visible band. Cause found in `apply_data_patch()`: the new L2
table for a hooked 1 MB section was filled with `flags_new` (ROM flags with
RAM memory type) for **all 16 pages**, so every untouched ROM page in
`E0000000`, `E0700000` (Live View conductor, AF), `E0800000` (Live View
device, filter pipeline) and, since the crop, `E0F00000` (Canon's data tables)
lost its outer (L2) cacheability: Canon's ROM section is TEX 5 (outer
write-back), RAM TEX 4 (outer non-cacheable). Checked on the ROM tables:
flags `5614` vs `4614`. Now (`CONFIG_MMU_REMAP_SMALL_PAGES`): untouched pages
get their own section's original flags (`5614`), only the remapped page RAM
flags (`4614`). Build check added.

**Camera test (2026-11-10):** Live View smooth again (the ROM attribute fix
was the cause). Still to do: Av mode showed Canon's B/W Live View; Live View is
not cropped to 65:24 nor moved into place.

* **Av fix:** Canon keeps `PROP_LV_CFILTER` from M to Av, and its decision
  allows the filter in every PASM mode. XPan's filter writer and the Live View
  filter map now act in P, Tv, Av and M (`m50_pasm()`).

### Live View crop: Canon's resize path (ROM study, 2026-11-10, probe waiting for the camera test)

* The Ximr context (`0x9329C`, RAM dump) composes 720x480 from two layers: a
  736x480 UYVY+alpha layer (`0x11060200`) and a 960x540 RGBA layer
  (`0x05040100`): the GUI and ML's bitmap. The live picture is not a Ximr
  layer: it reaches the screen through Canon's Live View display path, a
  hardware **resize** from a source rectangle to a destination rectangle.
* `LvDevVramSubController.c` dumps the parameter block ("<< RESIZE_PARAM>>"):
  +0 YuvMode, +4 InAddr, +8 InBW, +12/+16 InOffset X/Y, +20/+24 In W/H,
  +28 OutYuvMode, +32 OutAddr, +36 OutBW, +40/+44 OutOffset X/Y, +48/+52
  OutArea W/H, then OutAreaAspHV and SharememUnit. Cropping the centre 65:24
  band (InOffset Y, In H) and placing it at the frame position (OutOffset Y,
  OutArea H) is the EOS M result done by Canon's own hardware, with no
  per-frame CPU work.
* **Probe in this build:** read-only hook on the resize path start
  `E08D03DA` (`LvDevResizePath.c`; its only direct caller is the VramSub
  controller `E08BE64A`; 5th heap page). It logs each distinct caller and
  geometry with a count ("live view resize: ..."), to find the path that
  draws the screen. The trampoline loads the literal `E08D0570` (`0x1A530`)
  itself (its 3rd instruction is a PC-relative load).

**EVF crashes (camera test, 2026-11-10):** first EVF switch-on: black screen,
battery pull. After XPan off/on: Live View stutter back, EVF worked. After a
restart with the EVF on directly: worked, then a photo crashed. Log: the
resize probe never ran (0 calls with LCD and EVF), so the screen does not go
through `E08D03DA`; the photo's grain finished normally (0.82 s) and XPan
wrote its log 3 s later. The EVF had never been tested with an XPan build
before. Suspects: ML's M50 bitmap and XPan's HUD drawing for 720x480 while
the compositor output changes for the EVF; anything XPan does in Live View.
Next: a bisection test (XPan off; XPan on without grain and Live LUT; with
grain; with Live LUT).

**Second EVF test (2026-11-10):** EVF on, XPan on, one photo: black EVF and
LCD after the photo. The photo was saved (IMG_2704, 65:24, LUT, no grain
stage), XPan's colour table scan ran 3 s later and finished, then the camera
stopped. In the EVF, XPan's top bar (Split) sat lower than on the LCD. That
scan found 40 colour tables instead of the usual 20: Canon decrypts a second
copy of its style library (0x6A0858...) while the EVF is on.

Two places wrote into Canon memory without checking it was still the
memory they first found:
- **ML's bitmap copy** (`src/bmp.c`, `refresh_yuv_from_rgb`) kept the WINSYS
  layer (MARV) it found at boot and wrote 960x540 pixels into it forever,
  taking `winsys_sem` only around `XimrExe`. Now, on the M50, it reads Canon's
  current layer pointer (`_rgb_vram_info`) every refresh, under `winsys_sem`
  (with a 200 ms timeout; a skipped refresh is retried), and writes only
  when that layer has at least 960x540 pixels. `rgba_buffer_clear` does the
  same.
- **Live LUT preview** wrote its graded tables to the library addresses it
  found once per start. Now each write first checks that the table still
  holds Canon's original or XPan's last write; anything else means Canon
  reused the memory, and that address is dropped.

The crash cause is not proven: XM50.LOG now records Canon's display layer
(MARV, size) and the first 48 words of the Ximr context at each change, to
show what the EVF changes (and where the HUD offset comes from).

### EVF screen (2026-11-10)

Third EVF test: no crash. XM50.LOG: Canon's display layer never changed
(960x540); the Ximr output size (context word 1) is 720x480 on the LCD
(0x01E002D0) and 720x540 in the EVF (0x021C02D0). ML's 480 rows sit in the
middle of the EVF's 540, and the live picture shows in the 30 rows above,
so in Split the top bar sat 30 rows below the EVF's top edge.
`xpan_m50_screen_extra_top()` reads that size ((540-480)/2 = 30, 0 on the
LCD, never more than ML's 30 drawable rows above, `BMP_H_MINUS`), and the
Split top bar is drawn that much higher. Only the top bar moves; the frame
and the lower panel stay. The HUD clear covers those rows too, so no bar is
left behind when switching between LCD and EVF.

Fourth EVF test: the top bar's lower half went grey once, and later a black
strip showed under the bar where it used to be. Grey: XPan's dim (when
Canon's own UI opens) dimmed rows 0..480 only, so the bar's rows below 0
stayed orange and the rest went grey; it now dims from the EVF's top row
(`BMP_H_MINUS`). Black strip: the matte above the 65:24 frame still started
at row 60, under the moved bar. In Top and Split the frame
(`xpan_photo_get_liveview_frame_rect`) now gains the EVF's 30 rows above:
Top starts at the EVF's top edge, Split right under the bar. Its bottom edge,
the panel and Bottom stay. The AF frame and touch AF keep the shift of the
pixels (`fh-sh` = rows gained), so they do not move with it.

Fifth EVF test: Top was still drawn 30 rows low. In the EVF, Top now moves
as a whole: the frame starts at the EVF's top edge and the panel follows it
up 30 rows (`xpan_photo_get_hud_y`); the matte covers the rows left below.

### Live View crop: the display's image layer (2026-11-10, probe)

The live picture is not a Ximr layer (the RAM dump's Ximr context holds only
Canon's GUI layer, 736x480 UYVY+alpha, and ML's RGBA layer with its source
window 120,30 720x480). The display driver reads it from DispVram
(`*0x8E08`: +0xB0 panel, +0xB8 EVF; RAM dump: 0x7F41C000, 736x480, shown
720x480; the three buffers are 0x3F4800 apart, room for 1920x1080). Canon's
path for each frame: DispVramMgr (`SetImgVram`/`UpdateImgVram`, buffer
states matched by address, so the address itself must not change there),
then dispVramCtrl `localSetOnlyParamIMGRepVram`, which calls the panel
setter `E01389AC` or the EVF setter `E01390E0` with a parameter block p
(logs "set vram Panel addr : p+8"); they call the image layer setters
`E0154B64` / `E01550F8` with p+8..p+0x28, which program the display engine.
That is where the picture can be moved in hardware: by whole rows of the
address, or by a source offset if p has one.

First probe (LCD): p = `1 0 addr 2E0 1E0 0 0 0 0 2D0 1E0 1 0 0 counter 1`,
the address alternating between the buffers; word 14 is a counter (+2 per
call), which made every call a "change" and used up the 16 log lines at
start, so the EVF was never logged: it is now ignored. E01389AC passes words
2..10 to E0154B64: address, 736x480 (buffer), four zeros (source and
destination offsets?), 720x480 (shown). **Display offset test** (XPan menu,
not saved, LCD only, exactly that block): Canon's setter gets a copy of p
with the 65:24 band: 1 = word 6 is 74, 2 = word 8 is 74, 3 = only the shown
height 332 (Canon's own block is not touched).

**Offset test result (phone photos, `extras/M50/crops`):** test 1 showed
the band from source row 74 at the top of the screen, test 2 the picture
from screen row 74, test 3 the picture cut at 332 rows; the display left
the rest black. Words 5..10 are source x/y, destination x/y, shown
width/height. **65:24 live picture on the LCD:** with XPan's 65:24 frame on
(not in Canon's layout, not magnified, not in movie mode), Canon's panel
setter gets a copy of p with source row = the centred band the JPEG keeps
(266 rows from row 107: `xpan_photo_get_preview_rect`), destination row =
the frame's row for the layout (Top 0, Bottom 214, Split 60) and that
height. The display hardware moves and crops the picture: no copy, no
per-frame CPU work, and Canon's block is never written. (The offset test
used 74/332 only as test values.)

**EVF (display log):** p = `1 1 addr 400 300 0 0 0 0 400 2AA`: a 1024x768
buffer, the 3:2 picture shown as 1024x682 from row 0, i.e. the top 480 of
the EVF's 540 rows for ML, ML's rows -30..450. That was the "gap on top":
Canon's picture starts 30 rows above ML's area. The piece-by-piece moves
(top bar, panel, stretched frame) are replaced by one rule: in the EVF the
whole layout moves up 30 rows (`xpan_photo_get_hud_y`, `_top_y`, frame
rect), onto Canon's picture, and the EVF setter places the band the same way
scaled by 1024/720: 378 rows from row 152, destination = the LCD frame row
scaled (Top 0, Bottom 304, Split 86). The AF frame and touch AF use the
plain shift again (frame row minus source row).

### Touch AF and Canon's AF box (2026-11-10, probe)

Camera test: all layouts right on the LCD and in the EVF, but Canon's AF box
sits at the old 3:2 position and touch AF cannot get focus (it aims at the
unmoved picture). Canon still works in the unmoved picture: the box and the
tap are off by the band's move (destination row minus source row).

ROM: Canon's AF frame state machine (`GmtWakuState.c`: `wkTouchMoveAFFrame`
"(state) (x, y)->(x, y)") converts a touch through `E04235CC`/`E04236A2`,
which both call `E0423406(frame, r1, r2, r3, 7 stack words)`: it adds the
picture area to the touch position (stack words 1 and 2) and writes the
frame (+8/+0xA). The picture area table is `*(*0x7144+0x28)`, in sensor
units (RAM dump: X 276, W 6000, Y 48, H 4000; getters `E042666C` centre X,
`E042667C` centre Y, `E04266A4` top). `ComMgr` `TouchAfPosition` is the
remote (PTP) path. Writing `PROP_LV_AFFRAME` is not an option (crashes on
DIGIC 8, `property_whitelist.h`).

Probe (read only): a hook on `E0423406` records every call (arguments, the
frame before and after) with the last tap's screen position and whether
XPan passed it to Canon; XM50.LOG also logs each tap and Canon's AF frame
property as ML sees it. Next: correct the touch row (XPan passing its own
value for Canon's argument) and the box (same offset where Canon draws it),
or draw XPan's own box if the property is reported.

**Probe result (XM50.LOG):** in XPan's view every tap was kept by XPan
(never reached Canon), so touch AF could not work there. In Canon's layout
the converter got the tap scaled to sensor units (x * 6000/720, y *
4000/480) in stack words 1/2 and wrote frame = tap + area corner (276, 48),
clamped. Canon's AF frame property is never reported to ML.

**Fix:** (1) The picture area's centre-row getter `E042667C` (it feeds the
area top `E04266A4` the converter adds) returns the centre moved by the
band's own move in sensor units, `(dst-src) * H/480`, while the display
moves the band (Live View, XPan 65:24, not Canon's layout, not magnified):
a tap maps to the scene point under it and Canon draws its box on the moved
picture. The trampoline runs Canon's first four instructions with the
literal's value (0x7144). (2) In XPan's view a tap on the moved picture
(inside the frame, not an XPan field, not where Canon's hidden touch
buttons sit: Q, touch shutter, magnify, the bottom bar) goes to Canon.
The band move now also requires Live View (`lv`).

**Bottom layout (camera test: taps read as if at the top):** the band moves
down there (+107 rows), so the area top would be 48 - 891 < 0, and Canon
keeps it unsigned (`E04266A4`: `uxth`): it wrapped and the AF point was
clamped to the top. Now the area is shifted only while its top stays at 0
or above (Top, Split). In Bottom the getter returns Canon's value and, for
a tap XPan passed to Canon in the last 500 ms, the converter gets the tap
row moved back by the band's move (stack word 2, sensor units): focus lands
on what was tapped. Canon's box keeps the unmoved row in Bottom (drawn 107
rows higher); moving it needs Canon's box drawing itself.

**Box drawing probe (Bottom):** a signed (negative) area top is not an
option: the coordinate library mixes signed and unsigned compares on values
built from it (e.g. `E0421960`: `blo`/`bhs` next to `blt`/`bge`). The top
getter `E04266A4` is called by about 20 functions of that library
(`E0421000`..`E0426FFF`); one turns the AF frame into the box's screen row.
XPan now replaces the getter with exactly Canon's calculation (centre via
XPan's centre hook, height via Canon's `E0420C8C`, `uxth`) and counts its
callers by return address; XM50.LOG lists the counts every 5 s after taps.
The caller that runs when the box moves on a tap in Bottom is where the box
row can be corrected.

**Probe result:** `E0421CE7` about 30 times a second (a clamp of the frame
row into the area, `E0421CB6`), and on taps the converter, `E0422189`,
`E04226AD` and `E0425349`. `E0425318(out, r1, ctx)` is the box rectangle:
`out[0] = x - left - w/2`, `out[1] = y - top - h/2`, `out[2..3] = w, h`,
relative to the picture area in sensor units, which Canon's display code
scales to the screen. **Fix:** in Bottom, XPan moves the rectangle Canon
hands back down by the band's move (891 sensor rows = 107 screen rows);
Top and Split keep the moved area. The counting probe is removed.

**Camera test:** Top and Split perfect; in Bottom 157 rectangles were moved
(about 9 per tap) and the box did not move, so `E0425318` is not the
drawing. **Second try:** `E042268A(out, r1, r2, mode, s0, s1, frame)` writes
the frame's on-screen rectangle relative to the picture area (out+4/+6 =
x/y = frame - size/2 - area origin, out+8/+0xA = size), using the area top
getter; its callers are `E04227C6` (Canon's AF area calculation `E042279A`,
left as Canon's: focus is already right), `E0422DDE`, `E0422DF0`,
`E042416A`. In Bottom XPan moves the row it hands back (out+6) down by the
band's move; the log lists each caller with how many rectangles it moved.
The `E0425318` hook is removed.

Layout names: the camera test's "Top" with a top bar was the menu's Bottom
(panel at the top). Now in the EVF the top element of every layout goes to
the EVF's top edge (Bottom: the panel, Split: the bar, Top: the frame) and
the frame grows up to it; the HUD accepts rows down to -30 and uses
`XPAN_HUD_NONE` for "nothing drawn" (it used -1, which a panel at -30 would
have matched).

Probe (read only): hooks on both setters record p's first 16 words and the
other arguments whenever they change (not the address), logged to XM50.LOG
("display panel" / "display EVF"). One more hooked page
(`CONFIG_MMU_REMAP_PAGES 5`).

### Clean-up and XPan log (2026-11-10)

Photos were slow even with XPan off (camera test): film grain and the photo
LUT ran whatever the XPan setting. Now grain, photo LUT and Live LUT preview
run only while XPan is on, in photo mode, as on the EOS M; with XPan off
Canon's filter setting goes back to zeros (through the checked writer).

Study code removed: the colour table probe (an 8 MB RAM scan and a long card
write 3 s after every photo), the JPEG setup probe hook (`E00E7F32`) and the
old JPEG-task grain worker, the Live View grain probe (`E086DB00`), the
Live View resize probe (`E08D03DA`, one ROM page less:
`CONFIG_MMU_REMAP_PAGES 4`), cache facts and the grain bench, the trim-row
search, the creative filter record ring. XM50LUT.LOG is no longer written.

**XPan log** (`ML/LOGS/XM50.LOG`): one line per photo (XPan, 65:24 crop, LUT
graded, grain), one per grain stage (time, rows, late), display layer and
Ximr changes, Live LUT tables dropped. Hooks only fill single-producer
rings; XPan's task writes when nothing is half-pressed or in progress and
the events are at least 2 s old.

### Live View grain probe (2026-11-10, waiting for the camera test)

Canon's Live View grain path (`LvDevRoughMonochromeYuvPath.c`) uses the same
driver: `E086DAF6` copies the frame parameters (32 bytes) to `0xC83BC`,
`E086DB6A` posts `E086DB00` to the driver's task, which runs the MaskUVSub
engine and calls the path's completion. The path has a built-in "Through"
mode (completion without processing) when it has no parameters. Read-only
hook on `E086DB00` (same ROM page, no extra memory): frame count, the 32
parameter bytes (buffers, frame size), the task and the engine time per
frame, logged as "live view grain: ...". With these, the colour Live View
can be done the cheapest reliable way (engine copy, CPU copy or Canon's
"Through" route).

What the test answers: whether Canon runs its grain stage on M shots,
whether the photo is black and white, whether XPan's look and the JPEG hook
still apply (a JPEG log line per shot or not), Live View, and the time per
photo.

Open questions, in order:

1. The CreativeFilterPath and EFM driver: input/output buffers (in place or
   separate), strips, completion callback, resources.
2. Whether the engine's grain and its monochrome conversion are separate
   (XPan needs colour), and how its seed/offset/parameter block maps to the
   Film look (texture, strength, luma weighting).
3. The integration point Canon treats as a stage for a normal (non creative
   filter) shot, or how a normal shot is routed through the art filter
   stages, in the modes XPan uses.

### Where Canon adds its own grain (ROM study, 2026-11-09)

* **EOS M:** the Film grain never used Canon's grain engine, but its hook
  point came from studying Canon's Grainy filter: after the YUV-ready
  semaphore, before the JPEG lock ("Grain must never run inside JPEG setup's
  lock", `xpan-film-grain.c`).
* **M50 memory map (ROM0 + RAM4):** Canon's TTBR0 = `E0004800` (+0x80 for core
  1), TTBR1 = `E0000000`, TTBCR = 7. All RAM, ML's code included, is inner
  write-back cached and **outer (L2) non-cacheable**; the `0x4xxxxxxx` alias is
  uncached. So the L2 calls in the grain do nothing useful, and memory
  attributes do not explain the slow grain (the loop compiles to ~15
  instructions per pixel with no calls).
* **Canon's grain on the M50** is the art filter state machine
  (`ShootVfx/EC412/ArtFilter`, stage table `E0B166E0`): `sasInit` -> filter
  (`sasRoughMonochrome` = Grainy B/W, `E08AD148` -> `SAS_RoughMonochrome`
  `E08B9E20`, a hardware engine job) -> `sasYuvResize` -> `sasYuvTrimming` ->
  `sasRedEyeCorrect` -> `sasEncodeJpeg` -> `sasExit`. Grain runs as its own
  stage on the finished YUV, before its JPEG stage.
* **Normal photos:** the develop state machine decides `JudgeStartYuvToJpeg`,
  then the state `JpegEncodeSet` (`E00542DC`) builds the encoder parameters
  and *posts* "Set" (`E006BF84`, event 2) and "Start" (`E006BFA8`, event 3) to
  the JPEG path task. `SetJpegEncodePath` (`E0083CA2`, table `E0B1BFC8`)
  runs there, turns on the encoder clock (`PwrMng` `E0557ECE`, released at
  once) and calls `E00E7F32`: XPan's current hook. So the grain currently runs
  in the JPEG path task, inside a short Canon path handler, with "Start"
  already queued behind it. The EOS M-equivalent point is the entry of
  `JpegEncodeSet` (`E00542DC`): image complete, nothing of the JPEG started.
* **Moving the hook (studied, on hold until the speed data):** the clean
  point is `E006BF84`, called only from `JpegEncodeSet` (`E00543A4`) with
  Canon's finished parameter block (`+0x14` image address, `+0xC` size,
  `+0x10` aspect, `+0x34` table-set flag, the same values `SetJpegEncodePath`
  copies into the encoder block). It is in ROM page `E0060000`; XPan's hooks
  are all in `E00E0000`, and each remapped page costs 64 KB of ML's static
  memory (margin 42 KB, nothing large left to cut). Canon's develop events
  (`E0559580`, a TaskClass message queue) are not a DryOS state object, so
  ML's state object hooks do not apply. Hosting a second page needs either
  heap-backed pages in `patch_mmu` or the AllocMem pool layout (200D).
* **Restart:** Canon's terminate routine `E00446F8` ("terminateAbort") handles
  the shutdown codes; the path that sends 3 is not traced yet.

### To do (not forgotten)

The current list is in [HANDOFF-XPAN-M50.md](HANDOFF-XPAN-M50.md), "Open /
next". Done since this section was first written: Live View crop and
position (Canon's display setters), the 65:24 JPEG (Canon's size and encoder
tables), film grain (Canon's grain stage). Still open from the old list:

* **Watermark** (same stage as the grain).
* **17-point grid** (layout test below) and an **M50-calibrated Cineon base**.
* **Library tables rejected** by the chroma-range check (16 of 27; Live LUT
  preview only).

### JPEG crop (first study; done since, see "65:24 JPEG via Canon's 3:2 size")

Canon's JPEG sizes per aspect ratio are in RAM, in a camera spec block that
starts with "Canon EOS M50" at `0x7599C` (copied from ROM `E19E00B0`). At
`0x759E4`, as width/height word pairs for 3:2, 4:3, 16:9, 1:1:

| size | 3:2 | 4:3 | 16:9 | 1:1 |
|---|---|---|---|---|
| L | 6000x4000 | 5328x4000 | 6000x3368 | 4000x4000 |
| M | 3984x2656 | 3552x2664 | 3984x2240 | 2656x2656 |
| S1 | 2976x1984 | 2656x1992 | 2976x1680 | 1984x1984 |
| S2 | 2400x1600 | 2112x1600 | 2400x1344 | 1600x1600 |
| S3 | 1620x1080 | 1440x1080 | 1920x1080 | 1080x1080 |

The block is read by the develop setup (`E0201F1C`...), next to
`InnerDevelopState.c`. Canon also has a trimming service (`svcTrimming`,
`devcChangeAspectType`, `E0081810`, `E008040A`). If the shot's JPEG size
comes from this table, Canon's 16:9 L entry set to 6000x2216 (65:24) would
make Canon itself write a 65:24 JPEG, with RAW untouched. Not tried: that is
a write to Canon's memory and needs its own test. The probe logs the table.

### Grain and creative filters (first study; done since, see "XPan Film grain in Canon's grain stage")

* `mvcfNoiseGrainGeneration` / `mvcfNoiseScratchGeneration`
  (`Conductor/Cop/MvCfilter`) belong to the **movie** creative filter
  (old-movie grain and scratches).
* Still creative filters are Canon's "ArtFilter" (`ShootVfx/EC412/ArtFilter`,
  `fcsSetArtFilter`); Grainy B/W is `ROUGHMONO` internally. Their engine has
  not been traced yet.
* The EOS M grain (and watermark) run on the finished YUV image between
  Canon's develop and JPEG jobs. The M50 has the same split (`raw2yuvPath`,
  `EHD/Jpeg/JpegEnc`, `StlJpegResize...`), so XPan's CPU "Film" grain should
  carry over once that YUV buffer is found.

### 17-point grid: layout test (2026-11-10; FAILED on the camera, switched off: see the hand-off, item 4)

ROM study:

* **Canon's own 17-point path** is `E01C7358` (DevelopStandard): it sets the
  grid register (`E087D76C`: 17 -> `0x80000300` at Lucky `+0x4C`) and loads
  `0x4CC4` bytes with `E087D7DC`, nothing else. The still loader `E011E194`
  does the same from the size it is given. So the size alone selects the
  grid: XPan's hook passes `0x4CC4` with its own 17-point table and `0x1000`
  with anything else.
* **The table is 4913 contiguous words** (17^3, no padding): LesserLucky
  checks the table count per grid (`E0885E78`: 729 / 1024 / `0x1331`), and
  the Opera path (`E0194BD4`) copies it as five 4 KB pages from one base
  (`+0x1000` per pass, bank register `0x39070` = 0..4; the last pass 817
  words). `E087D74C` gives the table memory for grids 9 and 10 (`D043A000`,
  `D043B000`) and asserts for 17 ("Grid != ELD_LUCKY_LUT_GRID_17"): the
  17-point memory is only reached through the bank register or the loader.
* **Node positions.** Canon's ROM default table (`E0B18330`) is the identity
  with nodes 64 apart: node 8 = 512 = white, node 9 = 576 is a guard. So the
  10-point grid spans the input with nodes 0..8, and 17-point node j sits at
  10-point position j/2 (32 apart, node 16 = white).
* **Node order: not in the ROM.** No Canon code builds a 17-point table. The
  10-point order is eight blocks of 5x5x5 (low/high half per axis, at
  128 k + number of high axes); 17 nodes cannot split into equal halves, so
  the order has to be found on the camera.

Build:

* **XPan > Custom LUT > Colour grid**: 10-point (Canon), or 17-point test A,
  B, C, D:
  A `r + 17 g + 289 b` (the 10-point strides, red fastest), B `b + 17 g +
  289 r`, C eight blocks by half (nodes 0..8 / 9..16), D eight blocks by
  parity (even / odd). The right one gives the same picture as 10-point; a
  wrong one gives broken colours.
* **The 17-point table** is Canon's table for the shot resampled at the
  17-point nodes (even nodes are Canon's nodes; an odd position is the
  tetrahedral value of Canon's cell, so grey stays grey), then the tone base
  and the LUT on every node (`xpan_native_table_prepare_cube17`, same
  conversion as 10-point).
* **Nothing is computed in Canon's develop task.** The hook keeps a copy of
  the shot's Canon table when it changed and hands Canon a finished 17-point
  table only if it was built for exactly that Canon table, LUT, strength,
  tone base and layout; otherwise the shot is 10-point as before. XPan's
  task builds the table ahead, into the one of two buffers Canon did not get
  last. So the first photo after start-up or a Picture Style change is
  10-point. `XM50.LOG` says for each photo "LUT 17-point (layout N)",
  "10-point" or "Canon's".
* Live LUT preview stays 10-point (it grades Canon's style library; photos
  then come from the library and the hook passes them through).
* `build_xpan.sh` 6a2: Canon's table always with size `0x1000`; `0x4CC4` only
  with XPan's finished table that matches the key; the hook neither builds,
  blocks nor allocates; the build never targets the table Canon had last; a
  host test (`build_tools/xpan_grid17_test.c`, with Canon's ROM table) checks
  that the four layouts are bijections onto 0..4912, that even nodes equal
  Canon's and that the grey axis stays grey. `check_m50_rom.py` checks the
  grid register values, Canon's 17-point size and count and the ROM table.

### Still processing path: where a LUT could act earlier (ROM study, 2026-11-10)

No camera involved. From the path state strings and the parameter code.

**Order of a still (L JPEG):**

1. **ShootPreDevelop** (`ShootPreDevelopPathState.c`, set-up at `E08AEF80`):
   the RAW image, **14-bit Bayer, linear**. Engines, in the order Canon sets
   them up: Kaito (luminance-to-gain maps: `Bay2YParam`, `Y2G...Param`), Cdm,
   **Hilton** (black level, white balance gains, clip), HiltonCorr, Rshd
   (shading), Sibore (over-exposure: `SiboreOexParam`, `SiboreRecoveryParam`).
2. **ShootDevelop** (`ShootDevelopPathState.c`): SusanPath (RGGB at 1/2...1/32
   to YUV at 1/2...1/32: noise reduction pyramid), then DevelopPath with
   Susan (`SusanReg` 304 words), **David** (`DavidPictParam1` 361 words,
   `DavidPictParam2` 239, `DavidRelightPictParam` 40) and **Lucky** (the
   colour table, `E0082C3C`). Output: YCbCr 4:2:2.
3. ShootDistCorrect (Opti), the creative filter stage (XPan's grain), JPEG
   encode.

Every stage takes its registers from the shot's parameter pack (`DEV_PARAM`
and others by name, `ShtDevPictParam.c`, getters `E0854B4C`...; the "REGOUT"
dump functions at `E0854C1A`... give each block's name and size).

**Lucky is the only 3D colour table in the still path**, and it is the last
colour stage: after Susan and David, on YCbCr after Canon's tone curve. No
engine before it takes a 3D table, and before the demosaic (Susan) there is
no RGB pixel to look up. A true 3D LUT on the 14-bit linear data would have
to be XPan's own CPU code with its own demosaic, white balance and matrix on
24 MP: slow, and outside Canon's engines. Not a way forward.

**What can be done on the 14-bit linear data, with Canon's own engine: the
exposure the LUT receives.** Hilton's block (`HiltonObwbReg`, 21 words;
Canon's ROM defaults at `E0F73EDC`, `E0F73F30`, `E0F73F84`):

| words | meaning | default |
|---|---|---|
| 0 | clip level | `0x3FFF` (14 bits) |
| 1-4 | black offset per channel | `-0x200` / `-0x800` |
| 5-8 | enable per channel | 1 |
| 9-12 | white balance gain, 12 bits, `0x400` = 1.0 | `0x578 0x400 0x400 0x5DC` |
| 13-16 | gain shift (3 bits) | 0 |
| 17-20 | third register per channel | `0x200` |

(register writer `E08E56F6`, `ELD/Hilton/Hilton.c`: per channel offset, then
`enable << 24 | shift << 16 | gain`, then word 17-20.) This is the M50's
form of the EOS M's `JPEG_OBWB` block, which XPan's highlight headroom uses
there (README-XPAN.md "Tone base / Highlight headroom": gains lowered by 1 or
2 EV in the linear data, so highlights above Canon's JPEG white reach the
colour table unclipped, and the Cineon tone base maps the range back).

* **Where Canon hands the block over:** the shot's block is `DEV_PARAM +
  0x2D8C` (HiltonCorr at `+0x2DE0`), chosen by `E08BC57C`; StartPreDevelopPath
  passes both to `E08C7FC4` -> **`E08DB920(obwb, corr)`** (`E08AF01A`). A
  second caller chain (`E08B02BA`, "Upd(%d)Rshd(%d)Hilto(%d)Cdm(%d)Kaito(%d)")
  uses the same function; which path that is has to be identified before a
  hook goes there.
* **The M50 version of the EOS M headroom** would be: Canon's `E08DB920` gets
  a copy of the shot's block with words 9-12 lowered by the headroom, only
  for XPan's stills; the tone base in the colour table (to be calibrated for
  the M50, hand-off item 5) maps it back. Canon's block is not written.
* **Not known yet, needs the camera:** the real per-shot values (the RAM dump
  has no shot in it), how much range the M50 has above JPEG white, and what
  Kaito and Sibore do with a darker image (both sit next to Hilton and react
  to highlights). So the first step is a read-only log of the shot's block.

**The tone curve** (the other way to give the table a flatter input) is in
the Susan / David parameters; their content is not decoded. Also a read-only
log first.

### Hilton probe: camera test (2026-11-10, build 311ea1715a57, works)

Read-only hook on `E08DB920`. Camera normal (Live View, 65:24 crop,
photos); `apply_normal_patches` = 0, so the seventh heap page fitted. Five
photos IMG_2730-2734, ISO 100, f/3.5, 1/200 to 1/13 s (a 4-stop bracket),
manual white balance, indoor light:

* **One caller only**: `E08AF01F` (StartPreDevelopPath), once per photo.
  The recover path (`E08B02BA`) did not run.
* **The shot's OBWB block** (21 words, same in all five photos except the
  black offsets by 1):

| words | value | meaning |
|---|---|---|
| 0 | `0x3074` = 12404 | white / clip level (14-bit) |
| 1-4 | `-0x1FF`, `-0x1FE` | black level per channel, measured per shot |
| 5-8 | 1 | enable |
| 9-12 | `0x460 0x400 0x400 0x768` | gains 1.09, 1.0, 1.0, 1.85 (R, G, G, B: warm light, blue raised) |
| 13-16 | 0 | gain shift |
| 17-20 | `0x200` | unknown, Canon's default value |

* **The gains do not follow the exposure** (identical from 1/200 to 1/13 s):
  exposure is in the RAW data itself, the block carries only black level and
  white balance. So lowering words 9-12 lowers the image in the linear data,
  before every tone and colour stage, as `JPEG_OBWB` on the EOS M.
* **Clip.** Blue is multiplied by 1.85 against a white of 12404: in Canon's
  JPEG, blue clips at 54 % of the sensor's range. With the gains halved
  (-1 EV), every channel keeps the sensor's highlights up to raw white.
* HiltonCorr (13 words): `0, 7fff7fff, 7fff x5, 7ffe x3, 2000200 x2,
  30001770, 1000100` (`0x1770` = 6000, the width). Not decoded; not needed.
* The log said "LUT Canon's" for these photos: Live LUT preview was ON, so
  the photos were graded through Canon's style library and the hook passed
  the table on (by design, but the log line should say so).

### Highlight headroom on the 14-bit data (2026-11-10, camera test: works)

Build `b3a0005a584d`. XPan > Custom LUT > Highlight headroom (OFF / -1 EV /
-2 EV, off at every start). In XPan's `E08DB920`: for XPan's stills, when the
shot's OBWB block has the layout the probe logged (white 8000..16383,
enables 1, gains `0x100..0xFFF`, shifts 0), Canon's writer `E08E56F6` gets a
copy with words 9-12 halved (or quartered), rounded; black level, clip and
everything else are Canon's. Canon's block is never written. Canon's writer
copies the block into the engine's registers (`D0435000`) during the call,
so a static copy is enough. The log line says "gains -1 EV" / "-2 EV" /
"Canon's gains" per photo. `build_xpan.sh` checks: Canon's block or the
copy only, the copy only for the checked layout and XPan's stills, only the
four gains, at most 2 EV, not saved in the config.

Purpose: calibration photos (LUT `IDENT`, Canon Neutral) at 0, -1 and -2 EV
to fit the M50's own tone base (the EOS M method, README-XPAN.md "Tone base /
Highlight headroom"). Open: whether Canon's clip (word 0) acts after the
gains (then the lowered gains keep highlights) or before (then the photo is
only darker); the overexposed set answers it.

**Camera test (2026-11-10, IMG_2735-2747).** Camera normal throughout;
every photo logged with the gains chosen. Headroom OFF, -1 EV and -2 EV,
each at 1/50 (normal), 1/100, 1/200 and 1/13 s (+2 EV), ISO 100, f/3.5,
tripod; the scene has a lamp and a monitor (its picture changed between
shots, so only the 1/13 s set lines up pixel for pixel).

* **Lower gains = less exposure, in the mid-tones exactly.** -1 EV of gain
  at 1/50 has the brightness of OFF at 1/100 (mean 46.8 / 46.1), -1 EV at
  1/100 that of OFF at 1/200 (28.9 / 28.2), -2 EV at 1/50 that of OFF at
  1/200 (29.3 / 28.2).
* **The clip is after the gains: highlights are kept.** With lowered gains
  the photo has a ceiling where the sensor clips (green 234 at -1 EV, 191 at
  -2 EV; luma 227 / 183), and the pixels between Canon's JPEG white and that
  ceiling have detail: at 1/13 s, OFF has 15.4 % clipped pixels (red alone
  4.6 % at 1/50: a hue shift in the warm highlights), -1 EV 1.2 % (blue
  only), -2 EV none.
* **How much: about 0.5 EV in green** above Canon's JPEG white, more in a
  channel with a high white balance gain (blue at 1.85 here: about 0.9 EV).
  Canon's JPEG white shows at -1 EV as green 218 against the clip at 234.
* **So -1 EV is enough**: it holds everything the sensor recorded, with the
  ceiling still below 255. -2 EV only wastes the top of the table's input
  (ceiling 183) and brings the shadows down another stop.
* For the tone base: at the same exposure the -1 EV photo maps to OFF by
  Canon's own curve one EV apart (smooth up to Canon's white, 1/13 s set);
  the M50's CIN1EV is that mapping, continued into a Cineon shoulder for the
  recovered range. The EOS M one is fitted to the EOS M's Neutral render.

### M50 tone base for the headroom (2026-11-10, build 925b4bfdce76, camera test below)

* **Fit** (`build_tools/xpan_m50_cin1ev.py`, no numpy: plain Python + PIL
  in WSL). A lower gain is an exact linear pre-scale, so in each bracket of
  the headroom test (same RAW exposure at gains 0, -1, -2 EV) a pixel's value
  at one gain and at the next one are Canon's tone curve one EV apart.
  log2 of the inverse curve, piecewise linear over 11 knots in J (0..255),
  is fitted to "one EV per step" by least squares, with a smoothness term,
  per-level medians over all pixels and channels of all four brackets, and
  bins more than 0.2 EV off the fit dropped (the monitor in the scene changed
  between shots): 203 bins, residual 0.03 EV rms. Anchor: the sensor clip
  in the -1 EV photo (green J = 230) is X = 1, RAW green white, as the EOS M
  base. One curve for R, G and B; Canon's Neutral colour is kept.
* **Checks:** per level, the fitted curve predicts the 0 EV photo from the
  -1 EV photo within 0.6 levels (median, all brackets; 4.7 at worst). Canon's
  normal JPEG white is 0.36 EV below the sensor clip in green (J = 218 at -1
  EV). Against `CINLV.cube` (the EOS M's normal-render base) on the same
  scene: mid-tones within about 15 Cineon codes (0.15 EV), so a LUT keeps its
  exposure and look; where Canon's normal photo is clipped, CINLV stops at
  code 674, the new base goes on to about 770 with detail.
* **Cineon encoding** exactly as the EOS M base (`xpan_cineon_base_v7.py`:
  X <= 1, gain 0.18 x 2^3.5), so XPan's LUTs see the same scene scale.
* **In the camera:** Tone base "Cineon log +1 EV" (config value 2). The
  Hilton hook lowers the gains by 1 EV only when this photo will get the
  matching base: tone base +1 EV, Custom LUT on, a LUT loaded, CINLV and
  CIN1EV loaded, no Live LUT preview, XPan's still, the checked block
  layout. It then marks the photo (`hil_shot_lowered`), and the colour-table
  hook of the same photo uses CIN1EV instead of CINLV. A photo marked but
  left with Canon's table (e.g. a LUT loading in between) is counted
  (`hr_without_base`, shown in the menu). Live LUT preview keeps CINLV
  (Live View's gains are never lowered).
* `build_xpan.sh`: gains lowered only by 1 EV, only together with both
  bases loaded and the photo marked, a marked photo always gets CIN1EV, the
  package carries the M50's CIN1EV.cube.
* Limits: one curve for all channels and Canon's colour kept (no chart was
  used, unlike the EOS M fit); scene values above RAW green white are cut
  at 1 as on the EOS M, so a channel with a high white balance gain (blue
  under warm light) still clips there.

**Camera test (2026-11-10, IMG_2748-2753).** Camera normal; the log shows
Canon's gains for the two Cineon log photos and -1 EV for the four +1 EV
photos. IDENT at 1/50 and 1/13 s with Cineon log (2748, 2749) and Cineon
log +1 EV (2750, 2751); KODAK +1 EV at 1/50 and 1/13 s (2752, 2753). User:
"the same highlight retention, but at -1 the highlight roll-off is more
abrupt". Numbers (IDENT, luma, 8-bit; x4 = Cineon code):

* **Top of the range:** Cineon log stops at 169.5 (code 678), +1 EV at 190
  (code 762).
* **Detail recovered in this scene: almost none.** At 1/13 s, 7.8 % of the
  pixels sit at the Cineon log cap; +1 EV puts them at 187-190 (3 levels):
  the backsplash under the lamp is beyond the sensor clip in both. The M50
  has little to recover in green: Canon's JPEG white is only 0.36 EV below
  the sensor clip (calibration above); the gain is mainly in red and blue
  when their white balance gain is high.
* **Tone check by exposure** (1/50 -> 1/13 s = 1.94 EV must be a constant
  +44 levels in a log image): Cineon log (EOS M CINLV) is right (+41..+43)
  from level 45 to 105, then compresses gradually: +36 at 123, +22 at 147,
  +4 at 165. +1 EV (M50 CIN1EV) is right (+44..+45) up to 147, then falls
  to 0 between 150 and 183, and stops dead at the sensor clip (5 % of the
  pixels within 1 level of its cap at 1/13 s, against 3 % one level below).
  So CINLV's soft shoulder is the EOS M curve not fully undoing the M50's
  highlight compression (wrong, but gentle); CIN1EV undoes it (log-correct
  0.5 EV higher) and then meets the hard clip: **the abrupt roll-off is
  the sensor clip, no longer hidden by a shoulder.**
* ~~CIN1EV's lower mid-tones too contrasty~~ (corrected below): the real
  exposure step is 2.04 EV, not the nominal 1.94, so the ideal offset is
  +46; CIN1EV's +44..+52 is within a few levels, and CINLV (+41..+43) is the
  one about 8 % too flat.

### Soft shoulder for the +1 EV base (2026-11-10, build cc4394872acf, camera test: works)

* **Fit with exposure pairs too** (`xpan_m50_cin1ev.py`): besides the exact
  gain pairs (weighted 3x), pairs of neighbouring exposures at the same gain,
  each with its own unknown step, fitted. Steps found: 1.00-1.01 EV for the
  one-stop shutter steps and 2.00-2.04 EV for 1/50 -> 1/13 s; 2295 of 3033
  bins kept, 0.03 EV per bin (median). The curve is the same as before
  within 0.1 EV: the earlier fit was right.
* **The 10-point table is not the cause either:** simulating the camera
  (base applied at Canon's Neutral grey nodes J = 0, 32, ..., 255, from the
  RAM dump's Neutral table `0x69A820`, linear in between) changes nothing
  above level 90.
* **Shoulder:** above 1.5 EV below the sensor clip, log2 X is replaced by a
  quadratic with slope 1 at the knee and 0 at the clip. The clip now lands
  at Cineon code 710 (was 777; CINLV's top is 678), approached smoothly.
  Simulated exposure check (ideal +46): flat +44..+45 up to level 123, then
  +41, +34, +24, +13, 0 at 177, the shape of CINLV's roll-off (+36 at 123,
  +22 at 147, +4 at 165) about 10 levels higher. The sensor clip itself
  still stops flat: nothing can recover what the sensor clipped.


**Camera test (2026-11-10, IMG_2754-2761):** IDENT and KODAK, Cineon log
and +1 EV, 1/50 and 1/13 s; the log shows the gains as set. Exposure check
(1/50 -> 1/13 s = 2.04 EV, +46 in a log image):

| tone (8-bit) | 45-63 | 93-117 | 123 | 141 | 159 | 171 | top |
|---|---|---|---|---|---|---|---|
| IDENT Cineon log | +41..+42 | +40..+42 | +36 | +27 | +12 | -2 | 169.5 |
| IDENT +1 EV | +51..+53 | +44..+45 | +43 | +35 | +18 | +6 | 178 |
| KODAK Cineon log | +79..+88 | +77..+87 | +74 | +63 | +51 | +41 | 213 |
| KODAK +1 EV | +80..+93 | +87..+95 | +84 | +73 | +60 | +50 | 222.5 |

* **Roll-off: now gradual**, as simulated: +1 EV falls off over 117..177
  like Cineon log over 105..170, about 10 levels higher. Through KODAK both
  fall off smoothly to the top; +1 EV ends at 222.5 against 213.
* **Highlights:** at 1/13 s, the pixels at KODAK Cineon log's top (213)
  are at 216..222 with +1 EV: a little separation, no more (most of the
  bright wall is beyond the sensor clip). Same-exposure mapping, KODAK:
  Cineon 188 -> 199, 204 -> 216, 212 -> 221.
* **Lower mid-tones (45-81):** +1 EV is still about 12 % too steep there
  (+51..+53 against +46) and, at the same exposure, up to 11 levels darker
  than Cineon log in IDENT (60 -> 49, 68 -> 57); Cineon log is about 10 %
  too flat. Through KODAK the two are within 1-2 levels there. Likely
  cause: the camera evaluates the base only at Canon's 10 table nodes
  (J = 0, 32, 64, 96...) and interpolates straight lines between them; a
  log curve is far from straight between J 0 and 96, so the chords sit
  below it. The EOS M had the same effect and a "table-step compensation"
  (`xpan_cineon_stepcomp_v8.npy`): node values chosen so the straight lines
  match the curve on average, not sampled exactly at the nodes.

### Table-step compensation: tried, not used (2026-11-10)

* **Cause of the lower mid-tones confirmed.** Grey axes (Cineon codes):
  CINLV J 0/16/32/64 = 95/225/285/367, the +1 EV base 95/291/368/456. The
  +1 EV base rises steeply in Canon's first table segment (J 0..32): the
  camera draws a straight line 95 -> 368 there, 60 codes too dark at J 16,
  which is where the darker lower mid-tones are. Simulated through Canon's
  table (Neutral grey nodes J = 0, 32, 64, 96, 128, 159, 191, 223, 255), the
  exposure check gives +52, +55, +51 at levels 45, 57, 69: the camera's
  +51..+53. The lowered gains make it worse than with CINLV: every tone is
  a stop darker in Canon's render, so more of the picture lands in that
  first segment.
* **Compensation** (`xpan_m50_cin1ev.py`, reported, applied only with
  `XPAN_STEPCOMP=1`): node values chosen by least squares, weighted by the
  calibration photos' own pixels, black fixed. Error per pixel 23 -> 21
  codes, worst in J 1..32 62 -> 51, but level 99 goes from +44 to +34: no
  real gain. One straight segment cannot follow a log toe; only the node
  positions could, and they are Canon's.
* **The real fix is more room in the table's input for the shadows**, i.e.
  Canon's tone curve before the table. The EOS M did exactly that: Canon's
  HTP luma row in Canon's Angel block (`xpan_native_angel_shadow`, "Shadow
  room for the 10-step hardware table"). On the M50 the tone curve is in the
  Susan / David parameters (not decoded), or Canon's own Highlight Tone
  Priority setting could be tried first (Canon's menu, no XPan code): it
  renders a darker RAW with lifted shadows. It needs its own calibration.

### Canon's tone curve: David probe (2026-11-10, build c82e4ce99741: HUNG, not used)

**Camera test failed before any photo:** twice, XPan view -> Canon's main
menu -> MENU back gave a black screen and no power-off (battery pull). No
"David:" line in `XM50.LOG`: the probe never ran. `E00E7320` has a single
caller (`E008338A` -> `E00834FC`) and no pointer in ROM; the build's other
change is 10 KB more BSS. Cause open; `cc4394872acf` is back on the card.
The study below stays valid.

* **Where the still's picture parameters go.** DevelopPath (`E00834FC`)
  takes three blocks from the shot's parameter pack: DavidPictParam1 (361
  words, `DEV_PARAM + 0x233C`), DavidPictParam2 (239 words, `+ 0x28E0`),
  DavidRelightPictParam (40 words, `+ 0x2C9C`), dumped by "David REGOUT"
  (`E0854CD0`), then hands them to `E00E7320(p1, p2, relight, a3, s0, s1)`
  = `E011E0A4(p1, p2, relight, a3, s0); return E011E148(s1)`. `E011E0A4`
  gives each block to its David register writer (`E01A025A`, `E01A0DB4`,
  `E01A1586`). ROM defaults when the pack has none: `E0B171F0` (Param1),
  `+0x5A4` (Param2), `+0x3BC` (relight).
* **Curve candidates** in the default Param1: two monotone tables with
  shrinking steps (gamma-shaped), at `+0x338` (27 values `0x100 0x100 0x126
  0x2DA 0x401 ... 0x9AE 0x9B5`, after `0x80 0x80 0x80 0xB2 0x800`) and at
  `+0x4E4` (`0x100 0x100 0x123 0x2DD ... 0x808`, then `0x808` four times).
  `0x800` may be 1.0; the first goes above it (headroom?), the second clips
  at it. Their input positions are not known.
* **Probe:** XPan's `xpan_m50_david` makes Canon's two calls with Canon's
  arguments after copying the three blocks; `XM50.LOG` gets them per photo
  (first 12 photos). Same 64 KB ROM page as the colour-table hook: no new
  page. Build checks: Canon's callees and arguments, no write to a Canon
  block, no blocking; `check_m50_rom.py` checks the function, its callees
  and its caller.
* **Test:** the same scene with only one tone setting changed per photo
  (Neutral contrast 0 / -4 / +4, HTP on, Standard), ALO off: the words that
  follow contrast are the tone curve.

### Picture style settings under XPan's LUT (ROM study, 2026-11-12)

User: Neutral's contrast and saturation still change XPan's photos. Why, and
can the LUT get Canon's data before any picture style?

**Why the settings still act.** There are two reasons:
- **XPan grades Canon's table; it does not replace it.** `xpan_native_table_prepare_cube` takes the table Canon chose for the shot and puts each of its output colours through the LUT. Whatever Canon put into that table for the shot stays under the LUT.
- **Canon's tone stage comes before the table.** David (DevelopPath, `E00834FC` -> `E00E7320`) runs before Lucky. It gets the shot's three picture blocks (DavidPictParam1 / 2 / Relight from `DEV_PARAM`).

Canon's picture style values are stored as one record per style: sharpness `+0`, contrast `+4`, saturation `+8`, colour tone `+0xC`, filter `+0x10` (setters `E0535262`...`E05352F4`, "FlavorContrast PS(%#x)"). The develop parameters are built from that record and Canon's encrypted style data (`FA_GetDecryptPcsetFlavorLuckyTable`, `pPcsetFlavorParamHeader`), and the parameter builder is not traced yet. So it is not known yet whether a setting goes into the David blocks, into the shot's table, or into both. Contrast is most likely David's curve.

**Linear data before any style: not with Canon's engines.** The study of 2026-11-10 still holds:
- Lucky is the only 3D table in the still path, and it takes YCbCr after Susan (demosaic) and David.
- The 14-bit linear data exists only before the demosaic, where there is no RGB pixel to look up.
- Even with David made linear, a 10-point table on 10-bit YCbCr would put all the shadows into its first cell.

Film LUTs expect log input (XPan's are built on a Cineon base), not linear.

**What can be done with Canon's engines:**
1. **Pure table:** XPan's table built from the table's input nodes, not from Canon's table. This drops Canon's table colour, including anything the style puts there. Existing hook, no new page.
2. **Fixed David blocks:** XPan's stills get the same David blocks whatever the style and its settings. Canon's demosaic, white balance and matrix stay; the picture style stops acting.
3. **XPan's own curve in David** (a Cineon-like log), so the LUT gets a known log image, not Canon's Neutral render. This needs David's block format.

Options 2 and 3 use `E00E7320`, which is in the page already hooked for the colour table, so no new MMU page is needed.

**First step: a read-only probe.** Per photo, it logs the David blocks and a checksum of Canon's shot table:
- To keep the memory use small, the first photo's blocks are kept whole (2.5 KB). Later photos keep only the words that differ.
- Photos: Neutral 0, contrast -4 / +4, saturation -4 / +4, colour tone -4 / +4, Standard.
- The probe then shows where each setting lands.

**About the earlier hang.** The David probe `c82e4ce99741` hung on Canon's menu -> MENU, before any photo. It had the same symptom as the menu hang later found to be ML's patch stall at start, which was fixed in `d31841da9ff2`. So that probe was most likely not the cause. Its source was not kept and has to be written again.

**Probe build `6f96fa8cbae3`** (read only, Debug info only):
- **The hook:** `E00E7320` is replaced by XPan's `xpan_m50_david`, which makes Canon's two calls with Canon's arguments: `E011E0A4(p1, p2, relight, a3, s0)`, then `return E011E148(s1)`. Its only caller is `E0083570` (DevelopPath). It is in page E00E, already hooked, so the MMU pages stay at 6 of 7.
- **Records**, 12 photos per start:
  - photo 1's three blocks whole (640 words);
  - each later photo only the words that differ from photo 1 (a shared pool of 384);
  - per photo, the checksums of the blocks and of Canon's table for the shot (taken in the colour-table hook, before XPan grades it).
- **Memory:** ML grows by 5.4 KB (margin 199808 bytes).
- **XM50.LOG lines:** "David photo n: p1 .. p2 .. relight .. sum ..; Canon's table .. sum ..; style now ..". Photo 1 is followed by "P1 +off: words" lines; later photos by "P1+off old>new" lines.
- **Checks:** `check_m50_rom.py` checks the function, its callees and its caller. Build checks: Canon's two calls only, Debug info only, the reads range-checked, nothing of Canon's written, no blocking, and the hook list.

Test (on its own, one new hook in Canon's still develop; if the camera hangs, wait 10 s, then battery out):
- Debug info ON, ALO (Auto Lighting Optimizer) OFF, HTP OFF, tripod, same scene and exposure for every photo.
- XPan forces Neutral, so Standard is not tested.
- One photo each, in this order: Neutral all 0; contrast -4; contrast +4; contrast 0, saturation -4; saturation +4; saturation 0, colour tone -4; colour tone +4; colour tone 0, sharpness strength changed by 4.
- Then reset everything to 0, wait 10 s, and bring the card.

**Camera test of `6f96fa8cbae3`** (user, 8 photos as listed, no hang). Live LUT preview was on, so Canon's shot table was XPan's graded copy of Neutral's library table.
- **Canon's table never changed:** the same address `44032480` and the same checksum `f1ddebfd` for all 8 photos. No setting goes into the colour table; all four act in DavidPictParam1 (P2 only for sharpness).

| setting | words that change (P1 offsets) |
|---|---|
| contrast -4 / +4 | `+4D8`, the 25-point curve `+4E8`...`+548`, and `+550` `+554` `+55C` `+560` `+564` (31-32 words) |
| saturation -4 / +4 | `+404` `+408` `+40C` (16-bit pairs; Neutral 0: `7D007D 7007 9B082`) |
| colour tone -4 / +4 | `+414` `+418` (Neutral 0: `FD00F3 40019`) |
| sharpness | `+1E0` `+234`, P2 `+15C` `+230` `+280` |

Photos 6 to 8 also differ at `+290` `+294` `+56C` by 1 or 2. These are the same in all three and are not tied to one setting, so they are probably scene or exposure values.

- **Two tone curves in P1:**
  - **Curve A** at `+32C`...`+39C` (28 values, `100 100 13E 2B6 ... 829`) is the same in all photos: the style's fixed curve.
  - **Curve B** at `+4E0`...`+548` (`100 100 128 2DE ... 806`) is the contrast curve, pivoting at its 4th point (`3EB`, the same at -4, 0 and +4). At +4 it is an S: the low end goes down (`128` -> `D8`), the mid-tones go up (`594` -> `633`) and the top tops out at `7FE`. At -4 it is flatter: `128` -> `187`, `594` -> `536`, top `807`.
  - Their input positions and scale (`0x800` = white?) are not decoded.
- **So the picture style can be taken out without touching Canon's table:**
  - **(a) Canon's own settings, forced to 0** while XPan is on, as XPan already forces Neutral (Canon's property, not a memory write). Needs the M50's Neutral settings property layout, read first.
  - **(b) XPan's copy of P1** for XPan's stills, with the style words set to the Neutral 0 values above. That needs no property, but freezes values that may also depend on the scene.
  - **(c) XPan's own log curve in curve B (and A)**, so the LUT gets a known log image. This needs the curves' input positions, from a calibration like the M50 tone base (photos at known exposure steps).

**Where the two curves go (ROM, 2026-11-12).** David's Param1 writer `E01A025A` copies the block word by word into David's registers at `D0437000`:
- curve A (P1 `+32C`...) goes to `D0437450`..., in the sub-block written from `D0437400`;
- curve B (P1 `+4E0`...) goes to `D04376A8`..., in the sub-block written from `D0437694`.

Register addresses and the order of the parameter block do not prove the order in which David's hardware applies them. No ROM string names the two curves. What is known about each:
- **Curve A:** unchanged by contrast in Neutral. It may be the style's own base curve (Standard could not be tested: XPan forces Neutral) or a curve for a side path.
- **Curve B:** carries contrast.
- **Both have the same gamma-like shape at Neutral 0** (`13E 2B6 3D1` / `128 2DE 3EB`). Two such curves in series would apply the gamma twice, so they are most likely not in series on the same signal. One could be luma, the other a separate path.

The test that answers it is a write test: one photo with each curve replaced by a straight line, the other left as Canon's, on a grey ramp. It shows which curve sets the picture's tone, whether they combine, and the input spacing a log curve needs.

**Curve test build `79c670c80ccd`** (on the card, the first XPan change in David):
- **Layout:** in the camera log both curves are 25 values after the same header `800 100 100`: A at P1 `+33C`...`+39C`, B at `+4E8`...`+548`.
- **The option:** Diagnostics > Tone curve test: OFF / A straight / B straight / A+B straight. It is not saved and is OFF at every start.
- **What it does:** for XPan's stills with Debug info, Canon's writer gets a copy of Param1 in which the chosen curves are a line from `0x100` to `0x800` in 25 equal steps. Everything else is Canon's. Canon's block is never written.
- **When it applies:** only for the checked layout (the header, values `0x80`...`0xFFF`, never falling); otherwise Canon's block is used and counted.
- **Log:** each "David n:" line says which curves were straight.
- **Memory:** ML +1.5 KB (margin 198272 bytes); MMU pages unchanged (6 of 7).
- **Build checks:** Canon's block or XPan's copy only; only the 50 curve words changed, to that line; the conditions; not saved; Canon's block never written.

Test (on its own; if the camera hangs, wait 10 s, then battery out):
- Debug info ON, ALO OFF, HTP OFF, Neutral at 0, tripod.
- A scene with deep shadows to bright highlights (a grey card or white paper, something black, a lamp or window). The same exposure for all four.
- Live LUT preview OFF and Custom LUT OFF, so the JPEG shows Canon's render only.
- One photo with each setting: OFF, A straight, B straight, A+B straight. Then set it to OFF.
- Bring the card: the four JPEGs and the log.

**Camera test of `79c670c80ccd`** (user, no hang). Two sets of four (OFF, A, B, A+B), the second a little brighter: IMG_2848-2851 and IMG_2852-2855. All photos had Canon's colour table (no LUT) and Canon's gains, and the log shows the requested curve for each. Analysis on 1/8-size images, per pixel against the OFF photo (`curve_an.py`, scratchpad):

| | luma median (set 1 / set 2) | luma 99 % | chroma (mean abs Cb + Cr) |
|---|---|---|---|
| OFF | 111 / 148 | 231 / 245 | 13.3 / 13.5 |
| A straight | 111 / 148 | 231 / 244 | **5.5 / 7.9** |
| B straight | **54 / 68** | **151 / 185** | 13.2 / 13.6 |
| A+B straight | 55 / 68 | 151 / 185 | 5.5 / 8.2 |

- **Curve B is the tone curve.** With B straight every tone changes and the colour does not. Per pixel (set 1): OFF 44 -> 37, 124 -> 58, 228 -> 142, 252 -> 212. Shadows below about 30 stay the same. The picture is much darker and flatter in the mid-tones, with its top at 216 (`0x800`, the line's end).
- **Curve A is a colour curve.** With A straight the luma is the same within 1-2 levels at every level, but the chroma falls to 40-60 %. So A shapes the colour difference signals, not the tone. It is not a "first" tone curve.
- **The two do not interact:** A+B = B for the tone, and A+B = A for the colour.
- **Exposure:** set 2 against set 1 is not a constant ratio with B straight (1.1 to 1.3 in the mid-tones). So B's output is not linear light either, and the input spacing of B's points cannot be read from these two sets.

**For XPan's log curve:**
- The tone goes in **curve B**.
- Curve A has to stay Canon's, or be matched to the new tone, or the colour will be wrong.
- B's input positions need a calibration: B straight, a bracket in known steps (tripod, same scene; e.g. 1/25, 1/50, 1/100, 1/200, 1/400 s at the same ISO and aperture). Fit as for the M50 tone base: neighbouring exposures one EV apart give the curve, with no code change.

**Bracket with B straight (user, 2026-11-12).** IMG_2860, 2859, 2856, 2857 and 2858 at 1/400, 1/200, 1/100, 1/50 and 1/25 s; f/3.5, ISO 100, grain off; the log shows B straight for all five. All five decode.
- **Crash:** the camera crashed afterwards, while saving a sixth photo. That photo is not on the card, and the log ends 3 s after the fifth (it is written only when the camera is idle). So the log does not say whether the David hook was involved. No Canon or ML crash log was written.

**Fit** (`br_fit.py`, scratchpad): exposure (log2, 0 at Y = 128) as a function of JPEG luma, from 497 pixel pairs between neighbouring photos; residual 0.04 EV median, 0.15 EV at 90 %.

| Y | 16 | 32 | 48 | 64 | 80 | 96 | 112 | 128 | 144 | 160 | 176 | 192 | 208 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| EV | -5.4 | -4.2 | -2.9 | -1.7 | -1.0 | -0.55 | -0.29 | 0 | +0.35 | +0.52 | +0.59 | +0.73 | +1.14 |

Y tops out at 216 with B straight (the line's end), so values above 208 are extrapolated.

- **Not yet the input spacing.** This is the whole chain after the sensor: David with B straight, then Canon's Neutral colour table, then the JPEG encoding. Between Y = 144 and 192 it covers only 0.4 EV, which is steeper than any tone curve Canon would make. Most likely part of it is Canon's colour table (Neutral's own grey axis), applied after David.
- **Next, offline:** take Canon's grey axis out, using its Neutral table (RAM dump `0x69A820`, the photo's table at `44032480`). What remains is David's output against exposure, and so the input level of each of B's points.

**Offline follow-up (2026-11-12):**
- **Canon's colour table is not the cause.** The camera's Neutral table has a straight grey axis: nodes 1-8 give 64, 128, ..., 512 (`XM50LUT.LOG`, all eight copies of the current style's table). So on grey it passes David's output through unchanged.
- **The steep part was clipping.** With B straight the picture clips at Y = 216, but the first fit only dropped pixels above 250. Refitted (`br_fit2.py`, pixels 6..205 only): 407 pairs, residual 0.02 EV median, 0.07 EV at 90 %.

| Y | 16 | 32 | 48 | 64 | 80 | 96 | 112 | 128 | 144 | 160 | 176 | 192 | 205 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| EV | -5.5 | -4.3 | -3.0 | -1.8 | -1.06 | -0.66 | -0.27 | 0 | +0.18 | +0.39 | +0.59 | +0.84 | +1.11 |

From Y 64 to 205 the luma grows smoothly with exposure, about as exposure^0.6. Below Y 56 the steps are irregular (shadow noise at the short exposures).

- **B's input levels are still not pinned down.** Comparing the OFF and B-straight photos of the earlier set (same exposure, same pixel) fits "JPEG luma proportional to David's output" in the mid-tones. Two things break it:
  - in the shadows, the photos go darker than the curve's start value `0x100` allows;
  - at the top, Canon's `0x806` gives Y 253 while the line's `0x800` gives 216.
  So the step from B's output to the JPEG is not a plain scale. Without it, the bracket cannot be turned into B's input levels.
- **A clean way to measure that step:** photos with B **flat** at one value (every input gives the same output), at several values such as `0x200`, `0x300`, ..., `0x800`. Each photo is then nearly one uniform grey, and its luma is that step at that value. With it, the bracket gives B's input level for each point directly.

**B-flat build `99a442cd5f4a`** (on the card):
- **The option:** Diagnostics > Tone curve test gets eight more choices, "B flat 100" ... "B flat 800". All 25 values of curve B are set to that one value (hex); curve A and everything else stay Canon's.
- **What stays the same:** the header before the curve (`800 100 100`) is kept, so the darkest input still starts from `0x100`.
- **Log:** "curves B flat <value>".
- **Same rules as the straight test:** XPan's stills with Debug info only, the checked layout, Canon's block never written, not saved.
- **Build checks:** only curve B, only one value in `0x100`...`0x800`, and the menu range matches the hook. Memory unchanged.

Test: same setup as the bracket (Debug info ON, Custom LUT OFF, ALO OFF, HTP OFF, Neutral 0, grain off, tripod, the same scene). One photo at the correct exposure with each of B flat 100, 200, 300, 400, 500, 600, 700, 800. Then Tone curve test OFF, one photo at the same exposure: this is the OFF reference the crash cut short last time. Bring the card.

**Camera test of `99a442cd5f4a`** (user; several crashes, see below). 11 photos, matched to the 11 log records in order: IMG_2861-2870 = B flat 100, 200, 300, 400, 500, 500, 500, 600, 700, 800; IMG_2871 = OFF. All at 1/100 s, f/3.5, ISO 100, grain off, Canon's table.

| B flat | 100 | 200 | 300 | 400 | 500 | 600 | 700 | 800 |
|---|---|---|---|---|---|---|---|---|
| JPEG luma (median) | 32 | 64 | 96 | 129 | 161 | 193 | 225 | 250 (clips) |

- **JPEG luma = B's output / 8**, exactly. On grey, nothing after curve B bends the tone. The three B flat 500 photos agree to 1 level. Each flat photo is one uniform grey except its darkest 10 % (p1 2..17), so inputs below the curve's first point run down from it towards 0.
- **This resolves the earlier contradiction.** With B straight the brightest pixel (Y 216, B output `0x6C0`) sits at point 20.5 of the 25, not at the end. **The sensor clip is about point 20.5**: the last 4-5 points are above anything the sensor records. Canon's own curve there is flat (`7D3`...`806`). That is why Canon's OFF render reaches 253 (`0x7DB` / 8 = 251) while the line's end `0x800` is never reached.
- **B's input is linear light, with evenly spaced points.**
  - The B-straight bracket (IMG_2856-2860), with Y = 32 + 8.96 u (u = point index + 1), gives per pair one EV apart in the mid-tones (Y 60-108): Yb = 2 Ya - 8 (60 -> 80, 76 -> 111, 92 -> 146, 108 -> 175), slope 1.98. So the input x is proportional to u - 0.9. Zero light falls a little above the curve's start, about the level of the camera's black offset.
  - Below Y 55 (the first two or three points) the doubling does not hold: shadow noise around black, or a toe from another stage.
  - The "exposure^0.6" reading in the refit above was the black offset, not a power law.
- **Where the points sit, in EV below the sensor clip** (x proportional to u - 0.9, clip at u = 20.5):

| point u | 2 | 3 | 5 | 8 | 11 | 14 | 17 | 20.5 |
|---|---|---|---|---|---|---|---|---|
| EV below clip | -4.2 | -3.2 | -2.3 | -1.5 | -0.96 | -0.58 | -0.28 | 0 |

  Half of the points cover the top stop. Everything below about -4 EV falls into the first segment.

**For XPan's log curve:**
- Curve B can carry a log curve: write the log value of each point's linear input.
- The camera then draws straight lines between the points. The top 3 stops are well covered (8 or more points per stop at the top). Below -3 EV only 2-3 points remain, so the shadows get a straight-line toe, as in Canon's own curve.
- The scale is known: JPEG = output / 8, and output `0x800` = 256 (clipped), so a log code maps directly to an output value.
- Curve A (colour) has to be checked with the log tone: a test photo with Canon's A first.

**The crashes** (XBOOT.LOG, which covers the first 60 s of each start, and XM50.LOG). Four of the five starts of this round, and the bracket round's last start, end the same way:
- a photo is saved and logged (David record, JPEG on the card);
- Live View is back; the user opened ML's menu to change the tone curve test and closed it again (no INFO pressed). XPan's state then goes to the Canon layout (UI state `0xD100`, then `0xE500` = Canon layout, frozen), so XPan's current layout was the Canon layout;
- the logs stop 0.3-0.7 s later.
The crashing photo itself is not on the card because it was never taken: the camera died in Canon's layout after the previous one. It matches the open "crash after playback in the Canon layout (3:2)" (README, 2026-11-11): after a photo, back into XPan's Live View in the Canon layout, here through ML's menu. The curve test is most likely not the cause:
- the last start crashed too, after its only photo with the test OFF (the log stops 8 s after it, in Canon's layout);
- the earlier straight-curve rounds, shot in XPan's layout, never crashed.
Not proven, because no log covers the crash moment. It needs its own investigation.

### XPan's Cineon log in curve B (2026-11-12, build `c6c66370b78a`, not camera-tested)

User's goal: a consistent tone base before the LUT, whatever Canon's style settings. Colour (curve A, saturation, colour tone, the style) may stay Canon's for now, since every Canon style renders some Rec.709-like colour. The colour matrix comes later.

- **The curve** (`build_tools/xpan_m50_bcurve.py`):
  - Point u (index + 1) gets the linear scene value X = (u - 0.9) / (20.5 - 0.9), with X = 1 at the sensor clip (RAW white, as XPan's EOS M base), cut at 1 above it.
  - Its Cineon code uses the EOS M base's encoding: gain 0.18 x 2^3.5, black code 95.
  - B output = 2 x code, since JPEG luma = output / 8 = code / 4.
  - Values: `16C 33A 3D7 439 47F 4B7 4E5 50C 52E 54C 566 57F 595 5AA 5BD 5CE 5DF 5EF 5FD 60B 612 612 612 612 612`.
  - The sensor clip is at Cineon 777 (JPEG 194), as the EOS M base without its shoulder. One stop = 90 Cineon codes = 22.6 JPEG levels.
- **The option:** Diagnostics > Tone curve test > "B Cineon log", with the same rules as the other choices (XPan's stills, Debug info, checked layout, Canon's block never written, OFF at every start). Everything else in David stays Canon's, so contrast no longer acts; saturation, colour tone and sharpness still do.
- **With a LUT:** set Tone base to "Canon Neutral" (no base) so the LUT receives B's Cineon directly. "Cineon log" / "+1 EV" would apply the old Canon-to-Cineon base a second time.
- **Build checks:** the C table equals the script's output; only curve B; test 12 only.
- **Limits:**
  - Points are linear, so below about -3 EV from the clip the curve is a straight-line toe (2-3 points).
  - The 0.9 black offset and the 20.5 clip come from one scene's luma.
  - White balance gains above 1 clip that channel earlier (the highlight headroom option is not combined with this yet).

Test: Custom LUT OFF, Tone curve test "B Cineon log", ALO OFF, HTP OFF, grain off, tripod. A bracket one stop apart around the correct exposure (5 photos), plus one photo with the test OFF at the correct exposure.
- In a correct log image every stop moves the mid-tones by the same ~23 JPEG levels, and the sensor clip lands near 194.
- To avoid the crash: after setting the option, take the photos without opening ML's menu between them, and stay in XPan's layout (not the Canon layout).

**Camera test of `c6c66370b78a`** (user, no crash). IMG_2872-2877 were shot with a LUT by mistake and are not used. IMG_2878-2883, no LUT: B Cineon log at 1/100, 1/50, 1/25, 1/200, 1/400 s, and OFF at 1/100.
- **Clip: right.** Every Cineon photo tops out at exactly 194 (Cineon 777), the OFF photo at 255.
- **Steps per stop** (median luma offset for one EV, all four pairs alike; ideal 22.6):

| luma | 15 | 35 | 55 | 65 | 85 | 105 | 125 | 145 | 165 |
|---|---|---|---|---|---|---|---|---|---|
| step | +14 | +29 | +39 | +40 | +36 | +30 | +26 | +24 | +24 |

  The top two stops are right within 10 %. The lower mid-tones are up to 1.8x too steep.
- **Cause:** the points are not evenly spaced in linear light near black. Refitted (`upos.py`, scratchpad): EV of each point, both brackets together (B straight IMG_2856-2860 and this one), anchored at the clip u = 20.5. 795 pairs, residual 0.015 / 0.026 EV median.

| u | 1 | 2 | 3 | 4 | 6 | 8 | 10 | 12 | 14 | 16 | 18 | 20 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| EV below clip | -5.38 | -4.08 | -3.35 | -2.92 | -2.28 | -1.82 | -1.44 | -1.24 | -1.02 | -0.78 | -0.45 | -0.11 |

  The model reproduces this test's steps from the first curve (+44, +40, +31, +27, +26, +24 ...).

**Corrected curve, build `c367ff3a476b`** (on the card):
- `xpan_m50_bcurve.py` now takes each point's Cineon code from its fitted EV.
- Values: `279 346 3C1 40B 455 47B 4A2 4CD 4F5 510 520 534 544 55B 572 586 59F 5C1 5D5 5FE 612 612 612 612 612`.
- Predicted steps through the same model: +20...+22 per stop from luma 80 to 172.
- **Deep shadows:** the header's `0x100` (u = 0, about -5.8 EV) stays Canon's, so below -5.4 EV the curve drops steeply from luma 79 to 32 (a hard toe in the deepest shadows).
- The make run warned about clock skew; the archived autoexec.bin was checked to hold the new values (and not the old ones).

Test: the same as before (Custom LUT OFF, B Cineon log, a bracket one stop apart, one OFF photo). Expect +20...+23 per stop over the whole range and the top at 194.

**Camera test of `c367ff3a476b`** (user, no crash). IMG_2884-2888: B Cineon log at 1/100, 1/50, 1/25, 1/200, 1/400 s; IMG_2889 OFF at 1/100; no LUT, grain off.
- **Clip:** every Cineon photo tops out at 193 (`0x612` / 8 = 194.25).
- **Steps per stop** (median, ideal 22.6; the four pairs agree within 1-2 levels):

| luma | 15 | 35 | 55 | 65 | 75 | 85 | 105 | 125 | 145 | 165 | 175 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| step | +14 | +27 | +31 | +29 | +25 | **+22** | **+22** | **+22** | **+22** | **+22** | +13...+18 |

- **From luma 85 to 165 the curve is a true log, +21...+23 per stop.** That is from about 1.5 EV below Cineon's mid grey (code 470, JPEG ~117) up to about 0.5 EV under the clip.
- **Above 170** the steps shrink as the pixels reach the sensor clip (some channels clip first).
- **Below about 80** (the curve's first point, -5.4 EV below the clip, about 2 EV below mid grey) is the toe from Canon's header value `0x100`, as predicted. The steps are +25...+31 there, then fall towards black. Shadows deeper than about 2 EV under mid grey are compressed, not log.
- **Open:**
  - The toe could be extended by changing the header's `0x100` values (u = 0 and before). That is not tested; it would be the first change outside the 25 curve values.
  - The fit used one scene. A different scene or white balance moves the clip per channel.

### Extending the toe: the header test (2026-11-12, build `de32d66f274b`, not camera-tested)

Curve B's 25 values follow the header `800 100 100`, which is unchanged so far. Two hints suggest the two `100` words are the curve's points below the 25:
- in the B-flat photos the darkest 10 % went below `0x100` / 8 = 32, towards 0, so there is a segment under the 25 points;
- the point fit puts the last `100` (u = 0) at about -5.8 EV, 0.44 EV under point 1. Between them the Cineon curve jumps from luma 32 to 79, which is the toe.

If u = 0 is a curve point, giving it its Cineon value (about `0x240`, luma 72) continues the log down to about -5.8 EV. If the word before it is a point too, the curve goes further.

**The test:** Diagnostics > Tone curve test gets three more choices. Each is B flat `0x400` (every point the same grey) with:
- "B400 hdr u0": the last `100` (u = 0) raised to `0x200`;
- "B400 hdr u-1": the first `100` raised to `0x200`;
- "B400 hdr both": both raised.
`0x800` is never changed. The same rules as the other choices apply (XPan's stills, Debug info, checked layout, Canon's block never written, OFF at every start). Build checks: only these two words, only `0x200`, only with B flat `0x400`, tests 13-15.

**Reading it:** with B flat, everything above the points is luma 129 (`0x400` / 8), so only the darkest pixels can differ. If raising a word lifts them towards 64 (`0x200` / 8), that word is a point of the segment under the curve.

Test: a scene with deep shadows (a dark corner, black cloth), one exposure for all four photos (the correct one, or one stop under for more dark pixels), Custom LUT OFF, grain off, tripod. One photo each with: B flat 400 (the reference), B400 hdr u0, B400 hdr u-1, B400 hdr both.
- Before the first photo, press INFO until XPan's own layout is shown (not the Canon layout): the crashes came when XPan returned to the Canon layout after ML's menu.

**Camera test of `de32d66f274b`** (user, no crash). IMG_2890 B flat 400 (reference), 2891 hdr u0, 2892 hdr u-1, 2893 both; 1/100 s, same framing. Per pixel against the reference (`hdr_an.py`, scratchpad):

| reference luma | 2 | 10 | 22 | 30 | 42 | 54 | 66 | 78 | 94 | 106+ |
|---|---|---|---|---|---|---|---|---|---|---|
| hdr u0 200 | 3 | 10 | 22 | 30 | 42 | 54 | 66 | 77 | 94 | = |
| hdr u-1 200 | 5 | 18 | 41 | 55 | 75 | 92 | 107 | 119 | 128 | 129 |
| both 200 | 5 | 18 | 41 | 55 | 74 | 92 | 107 | 119 | 128 | 129 |

- **The word right before the curve (`100` at -1) has no effect.**
- **The first `100` (at -2) is the curve's lowest point.** Raising it to `0x200` lifts everything under point 1: 1.8-1.9x near black, fading out towards point 1 (luma 129 here). The share of the picture under 128 drops from 73 % to 44 %. "Both" equals "u-1". So it acts as a point below the 25 (u = 0 in the fit), and the dark segment runs from black through it to point 1.

**Build `b71d68d02362`** (on the card): Tone curve test > "B Cineon + toe". This is the tested Cineon curve with that word set to the Cineon value of its fitted position (-5.82 EV: code 285, `0x23A`, luma 71) instead of Canon's `0x100` (luma 32).
- It replaces the jump from luma 79 to 32 in the last 0.44 EV with log (79 -> 71). Below it, the segment still runs to black, now from luma 71.
- `xpan_m50_bcurve.py --header` computes the value; the build checks it and that only this word is written.

Test: the same as the Cineon bracket (Custom LUT OFF, a bracket one stop apart, a scene with deep shadows, XPan's layout before starting), with "B Cineon + toe". Expect +22 per stop now also from luma ~70 to 85, and the dark pixels lifted compared with IMG_2884-2888.

**Camera test of `b71d68d02362`** (user, no crash). IMG_2894-2898 "B Cineon + toe" at 1/100, 1/50, 1/25, 1/200, 1/400 s; IMG_2899 OFF; another scene with deep shadows.

| luma | 12 | 20 | 28 | 36 | 44 | 52 | 60 | 68 | 76 | 84...140 | 148 | 156 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| step per EV | +13 | +19 | +24 | +29 | +31 | +33 | +31 | +28 | +25 | **+21...+23** | +18 | +13 |

The four pairs agree within 1-3 levels.
- **84-140: log, as before.** In this scene the steps shrink from 148 up, where its brightest (coloured) areas reach the clip of single channels.
- **The toe barely changed.** Point 0 (now luma 71) only replaced the last 0.44 EV, from luma 79 down to 71. Below it, the hardware draws a straight line from black: the output is proportional to light. Going up one stop at a time, from 3 EV under point 0 to point 0, that gives +9, +18, +35, against Cineon's +8, +13, +15, which is what the table shows.

| EV under point 0 (-5.82 EV from the clip) | 0 | 1 | 2 | 3 | 4 |
|---|---|---|---|---|---|
| Cineon luma | 71 | 56 | 43 | 35 | 30 |
| curve B (straight toe) | 71 | 36 | 18 | 9 | 4 |

- **No more points exist in David.** The second header word has no effect, and `0x800` is the scale. So curve B gives Cineon from about -5.8 EV under the clip (about 2.3 EV under Cineon's mid grey) upwards, and a linear toe below.
- **The rest of the toe can be done in XPan's colour table.** On grey it maps luma straight through (Neutral) or as XPan builds it. A 1D lift from the straight toe to Cineon (36 -> 56, 18 -> 43, 9 -> 35) at its grey nodes J = 0, 32, 64 brings the deep shadows close to log. It is coarse below J = 32, since the camera interpolates between nodes. The data there is still about 11-bit from David (luma 0-71 = output 0-570), so it is not banded. Lifting them also lifts their noise, as any log does.
- `b71d68d02362`'s "B Cineon + toe" is the right curve for the setting: the toe point is where the model puts it, and it is never worse than `0x100`.

**User comparison with the same LUT (IMG_2900 old, IMG_2901 new).** User: the new one is much more saturated, and no highlight gain is visible.
- **IMG_2900 was "Cineon +1EV", not "Cineon log".** The log shows "gains -1 EV (Cineon log +1 EV)" for it, and Canon's gains for IMG_2901. So the old photo already had XPan's highlight headroom (white balance gains 1 EV lower on the RAW data), while the new curve uses Canon's gains. That is why there is no highlight gain: R reaches 255 on 0.21 % of IMG_2901 and nowhere in IMG_2900 (R max 235).
- **Measured** (1/8 size):

| | luma median | luma 99 % | chroma (mean abs Cb + Cr) | chroma at luma 128-160 |
|---|---|---|---|---|
| old, +1 EV | 83 | 222 | 22.4 | 41.6 |
| new, B Cineon + toe | 127 | 234 | 39.8 | 73.5 |

- **Why the new one is more saturated:** only the tone is log. David's colour path is still Canon's (curve A and the saturation words, made for Canon's normal tone), so the colour signals keep Canon's display strength under a flat log tone. In the old path the base cube (built from Canon's render to Cineon) also lowers the chroma. Curve A carries the chroma: straightening it cut the chroma to 40-60 %. So curve A has to be log as well, so the colour is as flat as the tone.
- **Next test:** curve A with the same Cineon values (and its header's first `0x100` as its lowest point, as on B), against "Cineon log" (not +1 EV) for a fair comparison.
- **Headroom with the new curve:** lowering the gains by 1 EV moves all data down one stop on curve B's linear points. The curve would then have to map each point to Cineon of twice its value. The top stop would then use about half of the points, at the cost of the shadows. Not done yet.

**Build `d81907851e7b`** (on the card, not camera-tested): Tone curve test > "A+B Cineon + toe". Curve A gets the same 25 Cineon values and the same lowest point (`0x23A`) as curve B; everything else stays Canon's. Build checks: only curves A and B, test 17. Curve A's own point positions are not measured; this assumes they match B's, which the shared layout suggests. The chroma result will show it.

Test, the same scene, exposure and LUT, Live LUT preview OFF, XPan's layout before starting:
1. Tone base "Cineon log" (not +1EV), Tone curve test OFF.
2. Tone base "Canon Neutral", Tone curve test "B Cineon + toe".
3. Tone base "Canon Neutral", Tone curve test "A+B Cineon + toe".
To see the bases themselves, use the IDENT cube (Custom LUT ON): with a tone base it applies the base only; with "Canon Neutral" it keeps Canon's table. Custom LUT OFF also turns the tone base off.

### Old and new Cineon base compared (2026-11-12, user, IMG_2902-2906)

All at 1/15 s and Canon's gains:
- IMG_2902: old "Cineon log" base, IDENT;
- IMG_2903: B Cineon + toe, IDENT;
- IMG_2904: A+B Cineon + toe, IDENT;
- IMG_2905: A+B with the user's look LUT;
- IMG_2906: old base with the same look LUT.

User: 2902 and 2904 should match, but 2904 is much brighter, with coarse highlights, and with the look LUT the new one looks bad.

| | luma median | luma 99 % | max R G B | chroma |
|---|---|---|---|---|
| 2902 old, IDENT | 93 | 169 | 175 171 169 | 12.9 |
| 2903 B, IDENT | 119 | 195 | 255 211 199 | 20.2 |
| 2904 A+B, IDENT | 119 | 195 | 205 197 195 | **13.1** |

- **Curve A in Cineon fixes the colour.** The chroma goes from 20.2 (B only) to 13.1, the old base's level. B only also pushed red to 255.
- **The new base sat 1.1 EV higher.** Per pixel, old -> new luma: 66 -> 88, 78 -> 102, 90 -> 115, 102 -> 127, 114 -> 140, 126 -> 152, 138 -> 163. That is a constant +25 levels (100 Cineon codes, about 1.1 EV) with the same contrast: both are log, but placed differently.
  - The new curve put the sensor clip at Cineon 777. The old base's top is about 680: Canon's white mapped by CINLV.
  - XPan's LUTs are made for the old placement. So the new base gave them every tone 1.1 EV too bright, and pushed the highlights into the top of the LUT's range, where they are not designed to be. That explains the brighter, coarse highlights in IMG_2905.
- **Fix, build `df4e97500be2`** (on the card): `xpan_m50_bcurve.py` scales the scene value by 2^-1.1 (EV_SHIFT), so the sensor clip lands at code 678, the old top.
  - New values: `1E4 297 30A 350 397 3BC 3E2 40B 433 44E 45E 471 481 498 4AF 4C2 4DB 4FD 510 539 54D 54D 54D 54D 54D`.
  - Lowest point `0x1B1`.
  - Used by "B Cineon + toe" and "A+B Cineon + toe" (the C table and value are checked against the script).
  - The make run warned about clock skew; the archived autoexec.bin holds the new values.
- **What this means for highlights:** the new base keeps the old base's white placement. So the highlight range is the same, Canon's clip. There is little above it on the M50 (0.36 EV in green). What the new base improves is precision, a true log from the shadows to the clip, and independence from Canon's contrast. More highlight range would need the lowered gains (+1 EV headroom) combined with this curve.

Test: the same as IMG_2902 and 2904 (IDENT: old "Cineon log" / curve test OFF, then "Canon Neutral" / "A+B Cineon + toe"), then both with the look LUT. The two IDENT photos should now match in brightness within a few levels in the mid-tones.

### Highlight roll-off (2026-11-12, user IMG_2907-2910, build `c273d80756a4`)

IMG_2907 old base IDENT, 2908 old base + look LUT, 2909 A+B Cineon (`df4e97500be2`) IDENT, 2910 A+B + look LUT; 1/15 s, Canon's gains. User: highlight transitions much more abrupt on the new one, colour less saturated in places, the highlights ugly.
- **Mid-tones:** per pixel, old base -> new base: 54 -> 45, 102 -> 95, 130 -> 122, 150 -> 140. That is the same slope, 8 levels (0.35 EV) lower: the 1.1 EV shift taken from the previous scene was 0.35 EV too much.
- **Highlights: no roll-off.** Old 158 -> new 147, but 166 -> 162 and 170 -> 171. The new base stays log up to the clip and stops dead: **4343 pixels on one level (171)**, while the old base spreads its top smoothly (about 3500-4100 pixels per level from 150 to 168). The old base's top is Canon's curve rolling into white (its last values `7D3 7E4 7F1 7FB 802 806`), mapped by CINLV. The pure log has no such shoulder, and each channel hits the clip separately, so the hue jumps as well. The look LUT then stretches that edge.
- **Fix** (`xpan_m50_bcurve.py`), as for the M50's CIN1EV base:
  - above 1.2 EV below the clip, log2 X becomes a quadratic with slope 1 at the knee and 0 at the clip (a soft shoulder);
  - EV_SHIFT is -0.75 (mid-tones matched to the old base);
  - the clip lands at code 656 (JPEG 164, the old top 169-171), approached smoothly over the last 7-8 points.
  - Values `210 2CD 343 38B 3D2 3F8 41F 448 471 48B 49B 4AF 4BE 4D3 4E5 4F3 502 511 518 51F 520 520 520 520 520`, lowest point `0x1D9`.
  - The archived autoexec.bin was checked to hold them.
- Test: the same four photos (old IDENT, new IDENT, old LUT, new LUT) on the same scene.

**Levels and range check (2026-11-12, user: no loss of DR or levels).** Cineon codes (10-bit, the base's precision into the table) per band of scene EV below the sensor clip:

| curve | -7..-6 | -6..-5 | -4..-3 | -2..-1 | -1..-0.5 | -0.5..0 | last 0.25 | top |
|---|---|---|---|---|---|---|---|---|
| first (clip 777, no shoulder) | 61 | 73 | 85 | 89 | 45 | 45 | 23 | 777 |
| `df4e97500be2` (shift -1.1) | 44 | 59 | 80 | 87 | 44 | 45 | 23 | 678 |
| `c273d80756a4` (shift -0.75, knee 1.2, end slope 0) | 50 | 64 | 82 | 87 | 28 | **9** | **2** | 656 |
| partial shoulder (end slope 0.5) | 50 | 64 | 82 | 87 | 36 | 27 | 12 | 683 |

- **Dynamic range is the same in every version.** Each maps everything from black to the sensor clip, and the clip is the curve's top. No version cuts scene range.
- **Levels are not the same:**
  - **The shift** (needed for XPan's LUTs, which expect the old placement) costs nothing from -5 EV up. It costs 11-28 % in the deepest two stops: Cineon's black floor, exactly where the old base puts them too.
  - **`c273d80756a4`'s full shoulder costs a lot at the top:** 9 codes for the last half stop instead of 45, and 2 for the last quarter. That would band if a LUT expands the highlights. It should not stay.
  - **A partial shoulder** (slope 1 at the knee easing to 0.5 at the clip) keeps 27 codes in the last half stop (60 % of pure log, 3x the full shoulder). It removes the hard stop's abruptness (the slope halves instead of dropping to 0 at one point), and puts the top at 683, the old base's top.
- **Real extra range, not tried yet:** curve B's points 21-25 sit above the sensor clip and are unused. Hilton's clip (OBWB word 0, `0x3074` = 12404 here) is applied after the white balance gains. So a channel with a gain above 1 (blue 1.85 under warm light) clips there before the sensor does: the hue jumps the user saw in the highlights. Raising that clip word to 14-bit full scale (`0x3FFF`) in XPan's copy (the headroom hook already passes Canon's writer a copy) lets those channels go up to 0.4 EV further. The curve's spare points 21-25 then carry it, with no cost to the shadows or mid-tones. Untested: Canon's over-exposure stage (Sibore) may react to values above the old clip.

**Build `41d9639f4e8b`** (on the card, replaces `c273d80756a4`, not camera-tested): the partial shoulder (END_SLOPE 0.5, knee 1.2 EV, EV_SHIFT -0.75).
- Values `210 2CD 343 38B 3D2 3F8 41F 448 471 48B 49B 4AF 4BE 4D4 4E9 4FA 50D 526 533 54B 556 556 556 556 556`, lowest point `0x1D9`, top code 683.
- The archived autoexec.bin holds them.

### XPan Cineon tone base (2026-11-12, build `b729a2f45f3e`, not camera-tested)

The new base is now a saved setting, no longer a Debug-only test.
- **Custom LUT > Tone base > "XPan Cineon"** (choice 3). XPan's photos get the Cineon curve in David's curves A and B (`xpan_m50_david`) and no base in the colour table, so the LUT receives Cineon from Canon's 11-bit tone stage.
  - Only when XPan grades that shot's table (`m50_shot_graded`, set by the colour-table hook of the same photo): with no LUT loaded, the LUT loading, Live LUT preview on, or XPan off, the photo stays fully Canon's.
  - Never together with a curve test.
  - Live LUT preview with this base previews through CINLV, as "Cineon log"; photos taken with the preview on come from the library (Cineon log base) and do not get the David curves.
- **Shoulder knee** (60 / 70 / 80 / 90 %, saved, default 80 %) and **Shoulder slope** (0 / 25 / 50 / 75 / 100 %, saved, default 50 %), in the Custom LUT submenu; they act only with XPan Cineon.
  - **Knee:** the shoulder starts where the pure-log output reaches that share of its top (from Cineon black 95). 60 % = 2.8 EV below the clip, 70 % = 2.1, 80 % = 1.4, 90 % = 0.7.
  - **Slope:** how much of the slope is gone at white. 0 % = pure log to the clip (most levels, hard stop); 100 % = flat at white.
  - Mid-tones are the same for every setting (EV_SHIFT -0.75, matched to the old base). Only the top changes.
- **Curves:** `xpan_m50_bcurve.py --tables` precomputes all 2 x 4 x 5 curves (clip raise off/on x knee x slope), so the camera does no floating-point work. The build checks the C block against the script's output. Lowest point `0x1D9` on A and B, as tested.
- **Highlight clip test** (Diagnostics, Debug info, not saved): with XPan Cineon, the Hilton hook gives Canon's writer a copy of the OBWB block. Its clip level (word 0, applied after the white balance gains) is raised by 5/4, at most `0x3FFF`. The David hook then uses the "raised" curves:
  - points above the old clip are extrapolated (+0.158 X per point, from the fit's last two) up to X = 1.25;
  - the shoulder ends there instead of at 1.0.
  Red and blue with gains above 1 can then go up to 0.32 EV further, if word 0 is the clip for them. Untested: Canon's over-exposure stage may react.
- **Log:** "David n: ... curves XPan Cineon rks <r><k><s>" (r = clip raised, k = knee index, s = slope index).
- **Build checks:**
  - curves A and B only, from the generated table, only on graded shots;
  - no table base with XPan Cineon;
  - the clip raise only with the test, Debug info and XPan Cineon, only word 0, at most `0x3FFF`.
- **Memory:** ML's reserve grows one 16 KB step (0x64290 -> 0x68290). The table is about 2 KB, and the reserve moves in 16 KB steps. Margin 181888 bytes.

Test (XPan's layout before starting; Live LUT preview OFF):
1. Tone base XPan Cineon, knee 80 %, slope 50 %, IDENT and the look LUT, against Tone base "Cineon log" with the same LUTs.
2. The same scene with Shoulder slope 0 % and 100 % (the range of roll-off), and knee 60 % / 90 %.
3. Highlight clip test ON (Debug info) on a warm-lit scene with bright highlights, against OFF; then check the log says "rks 1..".

### Highlight colour bands (2026-11-12, user IMG_2911-2920, build `f14c8685ef89`)

Photos:
- IMG_2911 old "Cineon log" IDENT, 2912 old + look LUT;
- 2913 XPan Cineon 80/50 IDENT, 2914 XPan Cineon 80/50 + LUT;
- 2915-2920 other knee / slope settings (log "rks" 22, 20, 24, 02, 32, 34, 30).

User: with every shoulder setting the highlight transitions are coarser with the new curve, on the white wall and the cabinet near the LED tube.
- **Not a tone or level problem.** In the top 40 luma levels of every photo all levels are used and none are combed (missing or thin bins). In smooth bright gradients, neighbouring-pixel steps of 2+ levels are as rare as on the old base.
- **It is colour.** In the cabinet's bright underside (crop 2300-3500 x 0-700, stretched) the new photos go from cream to pink to neutral along the gradient. The old ones stay one even cream. Hue (Cb/Cr angle) against luma in that area:
  - old IDENT: 145 deg at luma 145, 148 deg at 155: smooth;
  - new IDENT: 150 deg at 145, 135 deg at 155, 146 deg at 165: a wobble the look LUT then strengthens.
- **Cause (most likely): the shoulder on curve A.** The new base gave curve A (colour) the same curve as B, including the shoulder and the flat top above the clip. Canon does it differently: its B (tone) flattens at white (`... 7FB 802 806 806`), but its A keeps rising past white (`... 7F2 802 80F 81A 823 829`), with no shoulder. Compressing a colour curve near white bends the channels unevenly, so the hue moves, with every shoulder setting. That matches the report.
- **Fix:** curve A gets `m50_xcin_a`: pure Cineon log on every point, with no shoulder, continued above the clip (+0.158 X per point), as Canon's A keeps rising. Values `210 2CD 343 38B 3D2 3F8 41F 448 471 48B 49B 4AF 4BE 4D6 4ED 500 519 53B 54F 578 5A0 5C3 5E2 5FE 617`. Curve B keeps the Knee / Slope shoulder. Generated by `xpan_m50_bcurve.py --tables` and build-checked.
- Test: the same scene, old base and XPan Cineon, with IDENT and the look LUT. The cabinet underside and the wall near the tube should stay one hue towards white.

**Camera test of `f14c8685ef89`** (user, IMG_2921 old "Cineon log" IDENT, IMG_2923 XPan Cineon 80/50 IDENT; 1/15 s). User: next to the wooden wall where the picture clips, the new curve is very coarse, with colour shifts. Crop 2750-3500 x 520-1000 (the LED spot on the wood and the white wall), contrast-stretched: the new photo turns green/cyan towards the brightest areas; the old one goes to neutral white. This shot's gains: R `0x6D7` (1.71), G 1.0, B `0x50F` (1.26), so red has the largest white balance gain.

Mean R-G and B-G (8-bit) by luma below the brightest pixel:

| below top | 40 | 30 | 25 | 20 | 15 | 10 | 5 | 0 |
|---|---|---|---|---|---|---|---|---|
| old R-G | +17.7 | +16.2 | +15.0 | +13.3 | +11.2 | +9.2 | +6.1 | +0.3 |
| new R-G | +14.4 | +9.6 | +7.9 | +10.9 | +15.7 | +16.6 | +16.5 | +4.1 |
| old B-G | -20.2 | -20.8 | -20.6 | -20.1 | -19.0 | -18.1 | -12.8 | -4.0 |
| new B-G | -16.6 | -16.5 | -15.4 | -10.7 | -9.7 | -12.5 | -13.5 | -6.9 |

- **Old: the colour fades smoothly into white.** Both differences shrink steadily to about 0 at the top. Canon's own curves end at full scale (`0x806` / `0x829` of `0x800`), where all channels clip together, and CINLV maps that white to code ~680 as neutral.
- **New: the colour goes down, back up, then drops.** R-G falls to +8 about 25 levels under the top, rises to +16.5 within 5-10 levels of it, then drops to +4. Those are the colour bands.
  - The rise is where curve B (tone) is in its shoulder or flat, while curve A (colour, pure log since `f14c8685ef89`) keeps rising past the clip, so the colour is stretched while the tone is not.
  - The dip lower down is where B's shoulder begins.
  - XPan Cineon's top (code 683, about 2/3 of full scale) is not a common ceiling, so channels that clip at different levels (red first, with its 1.71 gain) do not meet in white the way Canon's curves make them.
- **Not fixed by guessing again.** A with the same shoulder as B (`b729a2f45f3e`) gave a hue wobble; A without it (`f14c8685ef89`) gives this. The next round should measure A's role directly: one scene, several A variants, judged by this same channel-balance table. The goal is the old base's monotone fade:
  1. A = B (same shoulder and flat top);
  2. A = pure log up to the clip, flat from the clip (no rise past it);
  3. A = B plus Canon's own A-B difference at the top (Canon's A rises a little past B's flat top: `802 80F 81A 823 829` against `802 806 806`).
  The Highlight clip test (Debug) is a separate lever for the channel that clips first. It was OFF in all these photos (log "rks 22").

**Build `0fb2e553a332`** (on the card, camera-tested below): Diagnostics > "Colour curve test" (Debug info, not saved, acts only with Tone base XPan Cineon):
- "Pure log" (the default, as `f14c8685ef89`);
- "A = B";
- "Log, flat at clip" (the Slope 0 % curve);
- "Canon-like": B plus a rise past white, `0 1 2 4 5 7 9 12` on points 18-25. That is Canon's own A-B difference above point 18, scaled by 0.47, XPan Cineon's range against Canon's.

The log line shows the variant as "XPan Cineon rks" 0xx / 2xx / 4xx / 6xx. Build checks: the values, Debug info only, not saved.

Test: one scene with a clipping highlight (the LED spot on the wood), tripod, one exposure, Tone base XPan Cineon, knee 80 % slope 50 %, IDENT. One photo with each of the four Colour curve test choices; then "Canon-like" (or whichever looks best) with Highlight clip test ON; then one photo with Tone base "Cineon log" (curve tests OFF) as the reference. Judged by the R-G / B-G table above: a steady fade to 0 at the top, as the old base.

**Camera test of `0fb2e553a332`** (user, IMG_2930-2936, tripod, one exposure, knee 80 % slope 50 %, IDENT; log "rks" 22 / 222 / 422 / 622 / 722 / 322, then the old base): IMG_2930 Pure log, 2931 A = B, 2932 Log flat at clip, 2933 Canon-like, 2934 Canon-like + clip raise, 2935 A = B + clip raise, 2936 old "Cineon log". The brightest level is 170 (old) and 172 (new, 177-178 with the clip raise), the Cineon top in both. Mean R-G / B-G (8-bit) by luma below the brightest pixel, on the wood under the LED (crop 2900-4500 x 0-700):

| below top | 40 | 30 | 25 | 20 | 15 | 10 | 5 | 0 |
|---|---|---|---|---|---|---|---|---|
| old R-G | +16.5 | +15.4 | +14.1 | +12.0 | +10.5 | +7.7 | +4.4 | -1.6 |
| Pure log R-G | +13.6 | +8.1 | +9.1 | +14.0 | +15.8 | +15.7 | +12.0 | +2.2 |
| Flat at clip R-G | +13.6 | +8.1 | +9.1 | +14.0 | +15.8 | +14.1 | +6.2 | +0.0 |
| A = B R-G | +13.6 | +8.0 | +8.5 | +11.7 | +11.5 | +9.0 | +4.1 | +0.0 |
| Canon-like R-G | +13.6 | +8.0 | +8.5 | +11.7 | +11.6 | +9.4 | +4.6 | +0.0 |
| A = B + raise R-G | +10.1 | +9.6 | +13.3 | +13.0 | +11.6 | +10.6 | +4.4 | +0.3 |
| old B-G | -19.9 | -20.2 | -19.8 | -19.5 | -19.2 | -16.7 | -9.8 | -2.1 |
| Pure log B-G | -15.7 | -15.5 | -11.5 | -9.2 | -10.8 | -13.7 | -12.1 | -3.2 |
| A = B B-G | -15.7 | -15.5 | -11.7 | -9.2 | -10.2 | -11.3 | -7.9 | -0.1 |

The wall left of the wood (crop 1500-2850 x 0-1000) gives the same picture.
- **A = B and Canon-like fix the band just under the clip.** The colour comes down to 0 at the top, as the old base does. Pure log and "flat at clip" rise again 10-15 levels under the top: those are the colour bands. The two are the same within 0.5, so the rise past white adds nothing. (Withdrawn: "A = B is the choice"; see the user's verdict below.)
- **Not fixed by curve A:** every variant has the same dip 25-30 levels under the top (R-G +13.6 to +8, B-G -15.5 to -9.2, then back). The variants are equal there, so this comes from curve B or the colour table, not from A. 30 levels under the top is about where the 80 % knee starts (luma ~141).
- **Highlights carry less colour.** Mean chroma by luma, whole frame: old / A = B: 70: 8.0 / 8.2, 90: 9.1 / 8.8, 110: 11.8 / 11.0, 130: 13.8 / 11.1, 150: 10.9 / 7.5. The mid-tones match; from luma ~130 up XPan Cineon has 20-30 % less colour than the old base.
- **Levels:** every variant has all 41 levels in the top 40, so nothing is coarse in level count. The pile-up at the top is small (17-98 pixels on the brightest level).
- **Clip raise:** the top goes from 172 to 178 (+6 levels of range for red and blue). It still fades to about 0, with the bump moved to 20-25 levels under the top.

**User verdict: all of them look bad.** Near the highlights there is a greenish band, then a pinkish band, then the duller tone. That is the dip and bump in the table above (R-G falls to +8 25-30 levels under the top, then comes back to +12), and it is the same in every curve A variant. Averages over the area made it look smaller than it is.

Offline checks (2026-11-12):
- **Not curve A:** identical in all four variants up to 15 levels under the top.
- **Not Canon's ROM colour table shape:** the ROM default table (`E0B18330`, same layout) is a plain RGB -> YCbCr matrix. A warm colour at a constant code offset keeps Cb / Cr constant (within 0.1) from node 1.5 to 5.4. **Neutral's own table is not the cause either:** Canon decrypts it into RAM; in the RAM dump (`extras/M50/ROMS/RAM4.BIN`, Neutral at `0x69A820`, grey axis 0, 64 ... 512) the same warm colour sweep keeps hue within 1.5 degrees and saturation within 1 % from node 1.5 to 5.5 (luma ~48 to ~176), so it has no bend where the bands are (scratchpad `neu_an.py`).
- **R and B do not follow curve B channel by channel.** The point-position fit (`upos.py`, brackets IMG_2856-2860 and 2878-2882) run on each channel alone: G fits as well as luma (median residual 0.04 EV), but R and B fit 3-4x worse (0.10-0.17 EV), and their point positions disagree with G's by up to 0.3 EV. So the colour near white is set by how David and the colour table combine the channels, which XPan does not model. A shoulder made in curve B therefore cannot be expected to keep the hue.
- **The old base works differently:** Canon's curves stay as they are and the whole roll-off to Cineon is done in the 3D colour table (CINLV), which keeps the hue as the LUT was made. It also reaches the same sensor clip, so XPan Cineon has not gained highlight range over it (top 170 vs 172; only the Debug clip raise adds about 6 levels for red and blue).

**Next: pure log in David, roll-off in the colour table (user's choice).** First a check that needs no build, with `0fb2e553a332`: Shoulder slope 0 % makes curve B pure log flat at the clip (the knee then has no effect; top luma 177 instead of 169), and Colour curve test "A = B" gives curve A the same. If the green and pink bands go away (leaving only a hard stop at the clip), the shoulder in David is their cause and the roll-off belongs in the table; if they stay, they come from the clip itself or from David's point spacing (the fit's last points step 3, 3, 4, 2, **6**, 2 levels), and a table shoulder alone would not fix them. Photos, same scene and exposure, IDENT, Tone base XPan Cineon: (1) slope 0 %, A = B; (2) slope 25 %, A = B; (3) slope 0 %, A = B, Highlight clip test ON; (4) Tone base Cineon log as the reference.

**Answered without a test (user):** the bands were already there with Shoulder slope 0 % in earlier rounds. So the shoulder is not their cause, and a roll-off moved to the table would not remove them. What is left is in David with XPan's curves: most likely the point positions near the top. Fitted on green alone (`upos.py` per channel, the channel that fits best), points 18-20 sit at -0.27 / -0.15 / -0.05 EV against -0.45 / -0.34 / -0.11 in the luma fit the curves use. With green's positions, segment 19-20 of the curve is about 2.3x too steep and 17-18 too shallow. That gives a dip and then a bump just under the clip, as the green-then-pink bands do. Not proven: red and blue fit poorly, so the position of each point is not known well enough to build an exact log. The old base does not depend on it (Canon's own curves, Cineon made in the table), reaches the same sensor clip, and feeds the table Canon's full input range (0..0x800, against about 0x550 for XPan Cineon).

### Measuring the top points (build `30e00acc5bcd`, 2026-11-12)

Diagnostics > Tone curve test > **"B top ramp"** (test 18; Debug info, not saved): curve B only, `118 130 ... 1F0` (3 levels per point up to point 10), then `260 2D0 ... 6C0` (0x70 = 14 JPEG levels per point for points 11-21), then `6D0 ... 700`. Everything stays at or under 0x700, where JPEG luma = B output / 8 exactly (B flat test), and curve A is Canon's, as in the B-straight bracket. With 14 levels per point, a 1/3 EV bracket gives each top point's exposure to a few hundredths of an EV, per channel on a neutral wall. That decides where points 17-21 really sit (luma fit or green fit) and so whether an exact log removes the bands. It is also the input for a full-range XPan Cineon (curve B to about 0x800, the table scaling back to Cineon). Build check: curve B only, these values.

Test: tripod, manual exposure, ISO 100, fixed aperture, Custom LUT OFF, Neutral 0, ALO and HTP OFF, the white wall and wood under the LED, Debug info ON, XPan's layout. Tone curve test = B top ramp. Shutter in 1/3 EV steps from the exposure where the brightest wall is just clipped, faster, about 13 photos (for example 1/15, 1/20, 1/25, 1/30, 1/40, 1/50, 1/60, 1/80, 1/100, 1/125, 1/160, 1/200, 1/250). Then one photo with Tone curve test OFF at the middle exposure as a reference. The steps between shots are fitted, so exact shutter values do not matter.

**Camera test of `30e00acc5bcd`** (user, IMG_2937-2948 = B top ramp, 1/15 ... 1/200 s in 1/3 EV steps, f/4, ISO 100; 12 log records "B top ramp". The 13th shot (1/250) crashed before its develop step; IMG_2949 is Tone curve test OFF after the restart, framing moved). Fit (scratchpad `top_fit.py`): inside a segment the output is linear in linear light X; pairs of neighbouring shots give X(u_a) = 2^(1/3) X(u_b); X at point 20 = 1.

| point u | 11 | 12 | 13 | 14 | 15 | 16 | 17 | 18 | 19 | 20 | 21 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| G, EV from point 20 | -1.13 | -1.02 | -0.93 | -0.79 | -0.70 | -0.61 | -0.48 | -0.36 | -0.24 | 0 | +0.21 |
| B, EV from point 20 | -1.08 | -0.95 | -0.88 | -0.82 | -0.70 | -0.60 | -0.51 | -0.38 | -0.24 | 0 | +0.21 |
| curves' list (luma fit), from point 20 | -1.24 | -1.13 | -1.04 | -0.91 | -0.78 | -0.67 | -0.53 | -0.34 | -0.23 | 0 | |

- **Green and blue agree** within about 0.05 EV from point 12 up. Red does not fit at all (not monotone): on this warm wall red is not a function of red's input alone, as found before (colour stage mixes channels). Points below 11 have only 3 levels each in this ramp, so the old fit stays for them.
- **The curves' list is wrong by about 0.1 EV at points 11-17**, and right at 18-20. Measured against the list, the segments' steepness in XPan Cineon is off: point 10 to 11 about 0.6x (too flat, around luma 145 on the 80/50 curve), 14 to 16 about 1.2x, **17 to 18 about 1.5x** (too steep, luma 160-163), 18 to 19 0.85x. Uneven steepness of that size, in the zone 5-30 levels under the top, fits the bands: a flat segment lowers contrast and colour (the green-looking dip), a steep one raises them (the pink bump). The green-only refit of the old bracket (`upos.py`, points 18-20 too low) was wrong: the new bracket puts them where the list has them.
- **Points are not evenly spaced in linear light:** X steps (G) 0.03-0.06 from 12 to 18, then 0.07, 0.15, 0.15 for 18-19, 19-20, 20-21.
- **The sensor clip is at point 20.1** in all three channels (top values 203-204, the same level), not 20.5: about 0.02 EV above point 20. All channels clip at one level (Hilton's clip after the gains), so they meet in white there.
- **Next:** XPan Cineon from these positions (points 11-21 from G and B, the old fit below, clip at 20.1); only the positions change, to see whether the bands go.

**Build `3ced97ccb9b3`** (on the card, camera-tested below): XPan Cineon's curves (every Knee / Slope / clip-raise table and curve A's pure log) rebuilt in `xpan_m50_bcurve.py` from the measured positions (`EV_M`: points 11-21 from G and B, points 1-10 from the old fit moved to the new reference, clip at point 20.1). Point 21 now carries the curve's continuation above the clip (the hardware interpolates 20-21 for the last tenth of a point under the clip). `EV_SHIFT_M` = -0.838 keeps points 1-10 at exactly their old values (checked: 0 difference), so mid-tones and the LUT placement do not move; only the top changes. The 80/50 clip lands at code 671 (was 683), slope 0 at 702. The old curve tests (12, 16, 17) keep their old values. Build checks unchanged (script output = C tables).

Test: the same scene and exposure as IMG_2930-2936, tripod, IDENT, Tone base XPan Cineon, Debug info ON, XPan's layout. (1) Knee 80 %, slope 50 %, Colour curve test "A = B"; (2) the same with "Pure log"; (3) slope 0 %, "A = B"; (4) Tone base Cineon log as the reference. Judged by eye on the bands at the LED and by the R-G / B-G table.

**Camera test of `3ced97ccb9b3`** (user, 1/15 s f/3.5 IDENT; IMG_2950 A = B 80/50 "rks 222", 2951 Pure log A 80/50 "rks 22", 2952 A = B slope 0 "rks 220", 2953 old Cineon log; framing moved since IMG_2930-2936). Mean R-G / B-G by luma below the brightest pixel, wood under the LED (crop 2700-4300 x 0-800):

| below top | 40 | 30 | 25 | 20 | 15 | 10 | 5 | 0 |
|---|---|---|---|---|---|---|---|---|
| old R-G | +16.1 | +14.9 | +13.4 | +12.1 | +10.6 | +7.7 | +4.3 | -1.6 |
| A = B 80/50 R-G | +12.9 | +10.7 | +9.6 | +8.2 | +8.5 | +9.1 | +5.8 | +0.2 |
| Pure log A R-G | +12.9 | +11.2 | +10.6 | +10.3 | +12.6 | +15.8 | +10.8 | +0.5 |
| A = B slope 0 R-G | +11.1 | +10.7 | +12.5 | +14.9 | +17.0 | +12.9 | +2.3 | +2.5 |
| old B-G | -18.1 | -18.5 | -17.7 | -18.0 | -18.3 | -16.0 | -9.4 | -2.1 |
| A = B 80/50 B-G | -14.2 | -13.6 | -11.0 | -12.1 | -10.4 | -8.1 | -6.3 | -1.2 |

- **A = B 80/50: the dip 25-30 levels under the top is gone** (old positions, IMG_2931: R-G +13.6 -> +8.0 -> +11.7; now +12.9 -> +10.7 -> +9.6 -> +8.2, then a small rise to +9.1 at 10 under the top). The fade to neutral is close to the old base's, with less colour overall (R-G 80 %, B-G 60-80 % of the old base's), and still a small wobble (1 level) at 10-15 under the top. The wall (crop 1500-2600 x 0-1000) gives the same.
- **Pure log A keeps the colour up to the clip** (+15.8 at 10 under the top), and slope 0 (no shoulder) gives a strong bump at 15 under the top (+17): with the positions fixed, these come from the clip, not from the curve.
- So the measured positions removed the dip; what is left of the colour bands is the clip itself when the curve has no shoulder or curve A rises to the clip. A = B with a shoulder is the variant to keep.

User: better, but the colours look like a different white balance, as if some colour is missing.

### Full-range XPan Cineon (build `d3a524db1ee5`, 2026-11-12, tested: worse)

User request: the same log curve stretched so the sensor clip lands near 0x800 (Canon's full tone range) instead of ~0x540. Diagnostics > **"Full-range curves"** (Debug info, not saved, only with Tone base XPan Cineon and no Tone curve test):
- **Colour table:** the colour-table hook gives this shot a tone base that scales the table's input by 2/3 before the LUT: a 2-point cube (`m50_xcin_scale`, corners 0 / 43690 = 65535 x 2/3), exactly linear, so the grid adds nothing. With IDENT the table becomes Neutral's x 2/3 (Neutral is a matrix within 0.5 of 512, median).
- **Curves:** the David hook scales XPan Cineon's curves A and B (and the toe point) by 3/2, at most 0x800 (Canon's Neutral table reaches white at node 8; its node 9 is the same). Only on a shot whose table got the scale (`m50_shot_full`, set by the colour-table hook, which runs first), so curves and table never disagree. Log: "XPan Cineon rks 8xx" (`a22` = full, A = B, 80/50).
- Knee 80 % / slope 50 %: points 16-25 `777 78C 7A1 7B7 7DB 7F6 800 ...`, clip about `7DD`. Slope 0 % reaches 0x800 already at point 20 (flat for the last part under the clip), so use a shoulder with this test.
- Build checks: the scale is exactly 2/3, curves only with the scaled table, x3/2 at most 0x800, not saved.

Test: the same scene and exposure as IMG_2950-2953, IDENT, Tone base XPan Cineon, knee 80 % slope 50 %, Colour curve test A = B, Debug info ON, XPan's layout. (1) Full-range curves OFF; (2) Full-range curves ON; (3) Tone base Cineon log as the reference.

**Camera test of `d3a524db1ee5`** (user, IMG_2954 A = B 80/50 "rks 222", 2955 the same with Full-range curves "rks a22", 2956 old Cineon log; 1/15 s, IDENT). Wood under the LED (crop 2700-4300 x 0-800):

| below top | 40 | 30 | 25 | 20 | 15 | 10 | 5 | 0 |
|---|---|---|---|---|---|---|---|---|
| old R-G | +16.2 | +15.0 | +13.6 | +11.8 | +10.2 | +7.5 | +4.3 | -1.7 |
| A = B 80/50 R-G | +13.0 | +10.7 | +9.6 | +8.1 | +8.3 | +8.9 | +5.9 | +0.1 |
| full range R-G | +19.2 | +17.3 | +12.0 | +5.3 | +5.4 | +6.5 | +5.4 | -0.2 |
| old B-G | -18.7 | -19.0 | -18.5 | -18.2 | -17.8 | -15.7 | -9.2 | -1.9 |
| A = B 80/50 B-G | -14.5 | -13.8 | -11.3 | -12.0 | -9.9 | -7.7 | -5.9 | -0.8 |
| full range B-G | -15.8 | -15.0 | -16.8 | -17.1 | -8.1 | -5.9 | -5.6 | -1.8 |

- **Worse at the highlights:** full colour (as much as the old base, or more) down to 20-25 levels under the top, then an abrupt drop: R-G +12 to +5.3, B-G -17 to -8 within a few levels. Contrast-stretched, the area around the LED is a large washed yellow-green patch. The wall gives the same.
- **Better colour level below that:** whole-frame mean R-G / B-G at luma 130: old +12.0 / -15.7, A = B +9.5 / -12.6, full range +11.3 / -11.6. So the "missing colour" is partly the range: stretched, the curves give back most of it.
- **Cause, most likely:** a ceiling in Canon's pipeline below 0x800. The B flat test already showed 0x700 -> 225 but 0x800 -> 250, not 256: the output clips at about 0x7D0. Neutral's table is linear up to node 7.4 for this colour (it clips only where red passes node 8). Stretched x3/2, red (the hot channel on this wall) reaches that ceiling before the sensor clip, so it stops while green and blue go on: the yellow-green patch. A smaller stretch that keeps the clip under about 0x700 would show whether there is a usable middle.
- **Correction (same day): the 0x7D0 ceiling is unlikely.** Canon's own Neutral curve B (probe log of IMG_2949, P1 +4E8: `128 2DE 3EA 4A2 529 593 5F2 641 67D 6B3 6E4 70D 730 753 770 788 7A0 7C6 7E1 7F6 808 808 808 808 808`) reaches `7F6` at point 20 and `808` above it, and its curve A rises to `829`, so Canon itself drives the stage past 0x7D0 without a colour shift. The full-range curve's top (`7A1 7B7 7DB 7F6` at points 17-20) is close to Canon's there. The cause of the abrupt yellow-green highlight is open.

### XPan Cineon from Canon's curve (build `a4728a288c70`, 2026-11-12, camera-tested)

User's idea: keep the old base's design (Canon's smooth curves, Cineon made in the colour table) but exact for the M50, and without the style's contrast.

- **CINLV is the EOS M's file** (title "XPan EOS M Cineon log base", identical to the EOS M and 6D packages). The M50 calibration photos (IMG_2736-2747) made CIN1EV, the base for the -1 EV gains.
- **A model-based base did not work:** inverting Canon's logged curve B through the measured point positions put Canon's render 55 codes (0.6 EV) under CINLV's grey axis in the mid-tones, while the photos show the old base and XPan Cineon within 2 levels of each other there (IMG_2954 / 2956, pixel by pixel). So B's point positions alone do not describe Canon's own render; the model was dropped.
- **The M50 base fitted on Canon's render instead:** `xpan_m50_cin1ev.py` with `XPAN_RENDER0=1` uses the same fit as CIN1EV (Canon's Neutral tone curve from the gain brackets; CIN1EV reproduced byte for byte) for Canon's render at its own gains: X = X_raw(J) / 2, Canon's white at 0.786 (0.35 EV under the sensor clip), the CIN1EV shoulder (1.5 EV) ending at that white. `data/XPAN/M50/CIN0EV.cube`, 17-point. Grey axis J 32 / 64 / 128 / 192 / 255: **293 / 374 / 492 / 607 / 679** (CINLV 285 / 367 / 480 / 585 / 675): about 10 % more mid-tone contrast (CINLV was measured 8 % too flat on the M50), the same top.
- **Contrast taken out:** for these photos the David hook writes Canon's Neutral contrast-0 curve B (the copy logged in 84 records, `xpan_m50_bcurve.py --canonb`) and leaves everything else Canon's. Two different Neutral-0 curves B were logged (one session: top `7BD 7D3 7E4 7F1 7FB 802 806`; all later ones: `7C6 7E1 7F6 808`); the copy is the later one.
- **In the camera:** Tone base XPan Cineon = CIN0EV in the table + that curve B (log "XPan Cineon, Canon B0"). Without CIN0EV loaded, photos keep Canon's table. Shoulder knee / slope and the colour curve, full-range and clip tests act only with Diagnostics > **"Log curves test"** (Debug, not saved), which gives the earlier XPan Cineon (David log curves). Live View keeps CINLV as before.
- Build checks: Canon's curve B copy = the script's, curve B only, only on a shot that got CIN0EV; a photo that needs a base is never graded without it; the package carries CIN0EV.cube.

Test: the same scene and exposure as IMG_2954-2956, IDENT, Debug info ON, XPan's layout. (1) Tone base XPan Cineon; (2) the same with Neutral contrast at +4 (should look the same as 1); (3) Tone base Cineon log (CINLV) as the reference; (4) optionally a look LUT on (1) and (3).

**Camera test of `a4728a288c70`** (user, 1/15 s; log matched in order): IMG_2957 XPan Cineon IDENT ("Canon B0"), 2958 Canon's table (no grading), 2959 XPan Cineon IDENT with contrast +4, 2960 Cineon log (CINLV) IDENT, 2961 Cineon log + look LUT, 2962 XPan Cineon + the same LUT (top 213 / 215). All with Canon's gains. User: 2961 has better highlights than 2962.
- **Pixel by pixel, CINLV -> CIN0EV:** the same up to luma 90, then brighter: IDENT 150 -> 153, 160 -> 163; with the LUT 150 -> 154, 170 -> 176, 190 -> 198, 200 -> 207. CIN0EV is the fitted M50 curve (more contrast than CINLV in the upper mid-tones, the same top), so the upper tones get less room before white: its roll-off is shorter.
- **Colour near the top (wood under the LED, with the LUT):** R-G at 10 / 5 / 0 under the top: CINLV +18.7 / +14.4 / +7.4, CIN0EV +15.7 / +11.4 / +8.8; B-G -35.5 / -29.6 / -25.8 against -34.2 / -24.6 / -20.4. CIN0EV loses the colour sooner near white. No bands in either (IDENT: both fade smoothly).
- **Contrast +4 (2959 vs 2957):** the colour is identical and the tones within 1 level except luma 70-90 (2-3 levels lower). Contrast also changes Canon's words `+4D8` and `+550`...`+564` (README "Picture style settings under XPan's LUT"); only curve B is replaced, so a small effect remains.
- **Shoulder:** CIN0EV has a fixed shoulder (1.5 EV under Canon's white, as CIN1EV); Knee / Slope do not change it. CINLV's softer top is the EOS M curve not fully undoing the M50's highlight compression. A longer CIN0EV shoulder (knee 2-2.5 EV) would keep the fitted mid-tones and give the top more room, as CINLV does.

**Build `3c86ee5fcaa8`** (2026-11-12, on the card, not camera-tested): Shoulder knee / slope work again with XPan Cineon, and the study options are gone (user: they only confuse).
- **The base is built in the camera:** `xpan_m50_cin1ev.py` (XPAN_RENDER0=1) now writes `data/XPAN/M50/cin0ev_nodes.txt`: Canon's white (0.786 of the sensor clip) and the scene value of Canon's 17 grey nodes relative to it. `xpan_m50_bcurve.py --cin0` turns that into `m50_cin0_nodes[knee][slope][17]` (Cineon x 65535, the same Knee / Slope meaning as before: knee = % of the top, from Cineon black, where the shoulder starts; slope = how much slope is gone at Canon's white). The camera fills a 17-point base from the row for the current settings (XPan's task, under the colour-table lock; heap at first use, 29 KB as a base file), and a photo uses it only when it was built for the current settings. CIN0EV.cube is no longer used (left on the card, harmless).
- Knee points (EV under Canon's white): 60 % 2.95, 70 % 2.20, 80 % 1.46, 90 % 0.73. 80 % / 100 % is the earlier CIN0EV file's shoulder (top code 681). Codes at nodes 8-16 (J 128 ... 255) for slope 0 / 50 / 100 %: knee 60 %: top 746 / 680 / 614; 70 %: 746 / 697 / 648; 80 %: 746 / 713 / 681. CINLV's top is 675.
- **Removed:** Diagnostics Tone curve test (all study curves), Colour curve test, Log curves test, Full-range curves, Highlight clip test, with their code (David log-curve tables, curve A variants, full-range scale, Hilton clip raise) and build checks. The David hook now only writes Canon's contrast-0 curve B on XPan Cineon photos (and logs with Debug info). ML's reserve is 16 KB smaller (0x64290).
- Build checks: the node table = the script's, built from the node row under the lock, used only for the current settings; curve B copy = the script's, curve B only, only on a shot with the XPan Cineon base.

Test: the scene of IMG_2961 / 2962 with the same look LUT, Tone base XPan Cineon: Knee 60 % / Slope 50 %, Knee 70 % / Slope 50 %, Knee 70 % / Slope 100 %; then Tone base Cineon log as the reference.

**Camera round IMG_2963-2974** (user, before `e2adc292c17a`): 12 photos, 12 log records. The log of that build gave the tone base and the gains, but not Knee / Slope or the LUT; those were estimated from the photos (top level and highlight curve against the Cineon log IDENT photo 2971) and stay uncertain. 2963-2969 and 2970 / 2974 XPan Cineon, 2964 / 2971 Cineon log, 2972 Cineon log +1 EV, 2973 Canon Neutral.

**Build `e2adc292c17a`** (2026-11-12, on the card, not camera-tested):
- **Photo log:** every "photo:" line is followed by the shot's tone base, Shoulder knee / slope, cube name and strength, as the colour-table hook saw them ("tone base XPan Cineon, knee 70%, slope 50%, cube KODAK.cube 100%").
- **Cineon log +1 EV removed** (user: it brings nothing): the tone base choice, CIN1EV (no longer in the package; `data/XPAN/M50/CIN1EV.cube` deleted; `xpan_m50_cin1ev.py` still makes it by default and the XPan Cineon nodes with XPAN_RENDER0=1), the 1 EV lower gains, and the whole Hilton hook (E08DB920: it existed for the gains; its probe log goes with it), with their ROM and build checks. Tone base values: 0 Canon Neutral, 1 Cineon log, 2 XPan Cineon; a saved 3 (XPan Cineon in the earlier builds) or the removed 2 becomes XPan Cineon.
- ML's reserve 0x63F90.

**Camera round IMG_2975-2987** (user, `e2adc292c17a`, all from the new log lines): KODAK 100 %, 1/15 s; XPan Cineon at knee 60 / 80 / 90 % x slope 25 / 50 / 75 / 100 % (2975-2986, top 190-227), Cineon log as the reference (2987, top 212). The user picked **knee 90 %, slope 25 %**.

**Build `8f66b7a2e967`** (2026-11-12, on the card, not camera-tested):
- Shoulder defaults knee 90 %, slope 25 % (build check). A card whose config already saved other values keeps them (ML saves only values that differ from the default).
- The M50 zip carries the boot-flag installer `BOOT_M50.FIR` (from `extras/M50`, unchanged; build check: byte for byte), as other ML zips carry ML-SETUP.FIR. `xpan_update_card.ps1` leaves every `.FIR` of the package out of the copy, so the working card still never holds one (its check 5 stays).

## Controls: Canon's values (build `da4df89eb1b9`, 2026-11-12)

User: a new install asked to change WB in Canon's menu "so XPan learns its values"; the learning should not be needed and costs resources. The learned settings (README "Controls, step 6") existed because the M50's encodings were not known. The camera's own record of them (`ML/SETTINGS/XM50OPTS.BIN` and `ML/LOGS/XM50OPTS.LOG` from the 32 GB card, 2026-11-08...12) gives them; every property is one 4-byte little-endian word:

| Setting | Property | Values Canon reported |
|---|---|---|
| EV | `PROP_AE` | byte 1 = thirds, -9..+9 (`00 f7 00 00` ... `00 09 00 00`); bytes 0, 2, 3 always 0 |
| WB | `PROP_WB_MODE_LV` + `_PH` (same value) | 0 AWB, 1 daylight, 8 shade, 2 cloudy, 3 tungsten, 4 fluorescent, 9 Kelvin (ML's `WB_*`) |
| Drive | `PROP_DRIVE` | 0 single, 4 high-speed, 5 continuous, 0x10 10 s, 0x11 2 s, 7 timer continuous (ML's `DRIVE_*` of the 60D / 7D / 5D3) |
| Metering | `PROP_METERING_MODE` | 3 evaluative, 4 partial, 1 spot, 5 centre-weighted |

- **Now:** fixed tables of these values in Canon's screen order (EV stops at both ends, the others cycle); the current value from ML's own handlers (`lens_info.wb_mode`, `drive_mode`, `metering_mode`) and PROP_AE's byte 1 (one 32-bit word shared by the handler). XPan's task writes only these values, with Canon's `_prop_request_change`, not while half-pressed, busy or recording. Flash and custom WB were never set on the camera, so they are not offered.
- **Removed:** the learning (8 property handlers, the 400 ms settle step every task cycle, 5 x 24 option slots), `XM50OPTS.BIN` (read at start, written after changes), the report log `XM50OPTS.LOG`, Diagnostics "Learned settings" and the "change it once in Canon's screen" message. ML's reserve 0x63590 (2.5 KB less). Old `XM50OPTS.*` files on a card are ignored.
- `src/property.h`: the M50 gets `DRIVE_HISPEED_CONTINUOUS 4` / `DRIVE_CONTINUOUS 5` (it had the generic 1, so the HUD showed both bursts as "OTHER"); the HUD shows "BURST H" for 4.
- Build checks: the tables, the EV range and byte, the writer's guard and value check, the property-write list, no learning code.

**WB as on the EOS M** (build `dceb2acbd0e8`, 2026-11-13, not camera-tested; user request):
- With WB selected, the dial steps the presets Auto, Daylight, Shade, Cloudy, Tungsten, Fluorescent, Kelvin. In Kelvin the dial changes the temperature with ML's `kelvin_toggle` steps (100 K, 500 K above 7000 K, wrapping), in Canon's M50 range 2500..10000 K; quick clicks go on from the last request.
- A tap on the selected WB field steps to the next preset (from Kelvin: Auto), and the field stays selected, so the dial goes on through the presets until Kelvin again (EOS M: `xpan-hud-controls.c` `step_wb`, `value_step`).
- Kelvin is written as ML's `lens_set_kelvin` writes it (WB mode 9 if needed, then `PROP_WB_KELVIN_LV` and `_PH`, one 4-byte word each), with Canon's `_prop_request_change` from XPan's task, and only while Canon reports a Kelvin value in range. The Kelvin write is new on the M50: the first camera test should confirm the HUD value and the photo's colour change.
- Build checks: the Kelvin range and guard, the tap step, the property-write list.

### Video LUT status (build `774cc23ddf55`, 2026-11-13, diagnostic)

User: the LUT on video (from a photo mode with the movie button, and in movie mode) worked before and does not now. The video path (`m50_live_poll`: Canon's style library graded with the LUT and CINLV) is unchanged since `fbcd57e8d`, and the card's logs hold no video session, so the cause is not known. Possible causes: Custom LUT off, no LUT loaded, CINLV not loaded, XPan off in movie mode, Canon's library tables not found, or no memory for their copy (80-160 KB from the heap, which XPan Cineon's base now also uses, 29 KB).
- Diagnostics > **"Video LUT"** (Debug info): "graded" or "Canon's", and in its help the reason or "N library tables graded". With Debug info, `XM50.LOG` gets "Video LUT: ..." each time the text changes. Read only, no memory added.
- Test: Debug info ON; in movie mode (and from a photo mode with the movie button) open XPan > Diagnostics > Video LUT and note the value and help text; record a short clip; bring the card.
- **Resolved (user):** the video LUT works; the failure was a setting on the camera, not the code. The status row stays as a diagnostic.

### Video LUT selector (build `05d5ec370686`, 2026-11-13, not camera-tested)

User: in the photo modes the LUT can be changed while recording (HUD), but movie mode has no XPan HUD, so no LUT selector. Now, in movie mode with XPan ON, Custom LUT on and two or more LUTs, a small "<  NAME  >" box sits above Canon's setting bar (y 372-404, centred; the name without its extension or 8.3 tail, in the theme colour, redrawn every 0.5 s as the loading box is). A tap on its left third picks the previous LUT, anywhere else on it the next one; the LUT then loads and grades Canon's library as from the HUD (`m50_lut_step`), also while recording. Only a tap on the box is XPan's; every other touch stays Canon's. Hidden under the loading box, in menus and dialogs. Build check: shown and taking touches only in that state, only on a hit.

**Build `ef55406f2f85`** (not camera-tested): the box fits the whole name in the large font (y 366-412, 18 px each side, at most 700 px). The first box gave the text exactly its width, but `xpan_hud_text` keeps 1 px for the bold stroke, so it shrank or cut the name; the width now includes it.

### Clean Canon screen (2026-11-13, not camera-tested)

User: with XPan off, INFO cycles Canon's Live View screens (up to 5, chosen in Canon's menu): four Live View screens and one with only the settings. Turning XPan on while Canon shows the settings-only screen leaves touch AF dead. ML draws on Canon's clean screen, INFO mode 3 (`LV_OVERLAYS_MODE`, 0x13CC8, written by Canon's `LvInfoToggle_Update`; ML's `get_global_draw` uses the same value). The EOS M already returns Canon to mode 3 with ML INFO presses (`keep_clean_display`); the M50 now does the same.

* **How:** XPan's view (Top, Bottom, Split, 3:2 HUD) and Canon not in mode 3: INFO presses through Canon's own button path (`fake_simple_button`, Canon's GUI queue; the fake MENU press was tested on this path in `46a84ccbd3f2`). XPan's INFO handler lets fake INFO through to Canon.
* **Bounds:** the next press only once Canon has changed its mode (1 s without a change counts as a try), at most 6 presses per episode, not while half-pressed or busy. Never in XPan's Canon layout, which shows Canon's own screen.
* **Clean screen switched off in Canon's INFO settings:** 6 presses never reach mode 3, and XPan says so once.
* Build check: the M50 branch, its bounds and its Canon-layout exception; XPan's INFO handler skips fakes.

### Halation prototype (2026-11-13, offline)

User: an optional, realistic film halation. On film, light from a strong highlight goes through the emulsion, reflects off the base and exposes the red-sensitive layer (nearest the base) again from behind: an orange-red rim around lamps, windows and bright lines against darker surroundings. The highlight itself stays white.

`build_tools/xpan_halation_proto.py` (offline, on finished JPEGs; `--synthetic` writes a night test scene):

1. **Key at 1/8 scale:** mean of key(Y) over 8x8 blocks, key = smoothstep from 30 levels under the base's white to the white, squared (XPan Cineon's white is Y 222: no pixel clips, so the threshold is relative to the base's white, not 255).
2. **Two radii:** Gaussian sigma 0.25 % of the width (inner) and 1.2 % (tail, 35 %), on the small map; tints in linear light: inner (1, 0.30, 0.10), tail (1, 0.12, 0.03).
3. **Only outside the highlight:** glow minus the pixel's own key, so a large bright area (a wall) gets a rim at its edge, not a cast over itself. The first version without this warmed the whole wall.
4. **Screen blend in linear light**, then back to display.

`--camera` does it with the camera's integer maths: 16-bit glow map per channel (about 2.2 MB at 750 x 500), a 256-entry key table, a 256-entry linear table and a 4096-entry inverse table; pixels with no glow are not touched. Against the float version: mean 1.3 levels, max 14 (synthetic scene). Pixels touched: 16 % (synthetic), 32 % (IMG_2975), so the cost depends on the scene.

Results: synthetic scene: orange-red rims, white cores, the grey card under the threshold untouched. IMG_2826 / IMG_2975: a thin red edge along the LED strip and a light warm spill around the lit area. Small points (8 px) glow little: the 1/8 map averages them, and after the clip a small lamp and a white card look the same.

In the camera it would run in XPan's Film grain stage (Canon's filter stage, both CPUs, YCbCr after the LUT): one pass for the small map, the blur on the map, the glow in the grain row pass. Estimate 0.3-1.2 s per 3:2 photo on top of the grain, to be measured. Not yet in the camera.

### Halation (build `e530206a6bf1`, 2026-11-13, not camera-tested)

XPan > **Halation** ON/OFF, PLAY: **Strength** 1-100 % (default 50; the prototype's 1.0 = 100 %). Saved JPEG only, P/Tv/Av/M, paused with RAW, like Film grain. With Film grain too: the glow first, the grain over it (as on film), in the same row pass.

* **Code:** `src/xpan-halation.inc`, pure C (no camera calls); `build_tools/xpan_halation_test.c` runs the same code on the host. Its file mode against the prototype (`xpan_halation_proto.py`): mean 1.1-1.7 levels, 99 % of pixels within 3-6 levels (IMG_2826, IMG_2975, synthetic scene; includes the 4:2:2 round trip of the test). Words changed: 7-21 %.
* **Differences from the prototype, for memory:** inner map at 1/16 (not 1/8), tail map at 1/32, both stored gamma-coded in 8 bits (fine steps near zero) in place of 16-bit linear; white = 99.99th percentile of every 8th row, at least Y 210 (so a scene without highlights does not glow).
* **Memory:** heap only while Halation is ON, four blocks: 192000 (1/16 map), 48000 (1/32 map), 17248 (tables), 26448 (two workers' row buffers) bytes; each under ML's 512 KB shoot_malloc limit (host test). Freed when OFF. ML's static size unchanged apart from code (margin 201344 bytes).
* **Stage** (`m50_stage_grain`, the same Canon filter stage and job checks as Film grain; `film_stage_wanted` now grain OR halation, so Canon's filter switch and the Live View colour fix follow either):
  1. histogram, every 8th row (both workers), 2. white and key tables (worker 0), 3. 1/16 key map (each worker its own cell rows), 4. blur and coding (worker 0), 5. the image: glow, then grain (both workers). Steps 1-4 within 2.5 s of the stage start (`HAL_STEPS_MS`); the image's budget is the grain's 6 s less their time; all within `STAGE_WAIT_MS` (9 s, Canon waits 10 s).
  * Late step (never expected): no halation for that photo, grain only, Halation off until restart; a worker that does not come back keeps the buffers (never freed) and the stage copies the image itself.
  * Cache (two CPUs, coherency unknown): halves split on 32 rows and map rows padded to 384 cells, so the workers never write the same cache line; every core writes back and drops every halation line at the end of each step.
* **Log** (XM50.LOG, per photo): `grain stage: N ms, R of R rows grained|done, halation: white W, steps S ms, P% changed`. Diagnostics > Grain stage shows the halation status.
* **To check on the camera:** the glow's colour. Canon's still YUV is taken as U in byte 0 and V in byte 2 (ML's UYVY): orange-red = right; blue-violet = the two are swapped. Then the time per photo (log), and the look at a few strengths.
* Build checks: the host test (rim red-orange outside a lamp, core and far pixels unchanged, no change without highlights, allocations under 512 KB), the gate, the 32-row halves, the buffer lock and stuck-worker rule, phase limits, no printing or allocation in the stage.

**Camera test of `e530206a6bf1`:** the user: "it looks very good" (so the U/V order is right). Asked for a stronger range and the menu row right below Film grain.

**Build `32b37a5b2952`** (not camera-tested): Strength rescaled, 70 % = the old 100 % (the prototype's 1.0), 100 % = 1.43 x; the default 50 % is now about the old 71 %. The XPan menu has a "Halation" placeholder right after "Film grain" (cropmarks.c; ML placed the row lower without one; on the EOS M it stays hidden). Build checks for both.

### Canon's main menu breaks Live View (2026-11-10, cause: the 65:24 live placement; no fix yet)

* **Symptom (user):** XPan view -> MENU (Canon's menu) -> MENU: black
  screen; MENU twice more: XPan's view back, Live View frozen; power-off
  hangs (battery pull). **Intermittent:** a bisect over the session's builds
  gave "works" on the two oldest, but the diagnostic build worked at one
  start and hung at the next, so those were luck; the bug comes from the
  65:24 live picture placement (`8f344d055b96` and before).
* **Diagnostic marks** (`XBOOT.LOG`, build `7c16a8e4cf67`): UI state, XPan's
  dim-and-freeze, button events, and every change in the panel / EVF setter
  calls (a2, Canon's block recognised, moved by XPan). Steady Live View:
  `a2 = 0`, block, moved. Start that worked: no setter call while the menu
  opened; at the close one call with `a2 = 1` (moved), 370 ms later `a2 = 0`.
  Start that hung: as the menu opened, a call with `a2 = 1`, Canon's block,
  **not moved** (ML's `lv` flag had dropped 14 ms before), and after that no
  change ever again: no `a2 = 0` frame after the menu, at either close.
* **Fix:** `disp_decide` decides the band on Canon's normal frames (`a2 = 0`)
  only and keeps that decision and geometry for a transition call, so the
  geometry never changes inside one of Canon's display sequences (panel and
  EVF). `build_xpan.sh` checks it.

**First fix failed** (build `103739ee3363`, latch on Canon's transition
call): hung at the first try, and that start had no transition call at all.
**Placement off** (build `e2d704c10bf4`, `M50_DISP_MOVE 0`: the hooks pass
Canon's own block): **no hang, whatever the user tried** (several starts).
So moving the picture through Canon's setter causes it.

* Canon's display around the menu, placement off (per-second call counts,
  mark 77): Live View 30 calls/s; menu open 0/s; menu closed -> about 280
  ms later one call with a2 = 1, about 370 ms later normal frames at 30/s.
  In the two hangs that step never came after the menu: Canon's display
  stopped before it reached the setter.
* Canon's panel setter `E01389AC`: the block's words 2..10 (layer address,
  buffer size, source x/y, destination x/y, shown size) go to the image
  layer setter `E0154B64` (with `p[15] == 1`, the Live View case, also
  `E01553E6` with words 0..10); a nonzero a2 is Canon's "PanelImgMute"
  (`E0154CE4(0)`, `E0155570(0)`), the image unmute after a restart; p[11],
  p[12] against the caller's `*s0`, `*s1` is a "panel mode change"
  (`E01386D0`).
* Not known: what in Canon's Live View stop / restart depends on the layer
  geometry XPan set (shown 266 of 480 rows, moved), so that Canon sometimes
  never restarts. Next: how Canon itself shows its cropped aspect ratios
  (16:9, 1:1, 4:3) in Live View, from the display probe's block log, so the
  band can be placed the way Canon does it.

**How Canon crops Live View itself** (display probe, build `e2d704c10bf4`,
Canon's aspect ratio changed in Canon's menu; block words 5-10 = source x/y,
destination x/y, shown w/h):

| Canon aspect | source | destination | shown |
|---|---|---|---|
| 3:2 | 0, 0 | 0, 0 | 720 x 480 |
| 16:9 | 0, 38 | 0, 38 | 720 x 404 |
| 1:1 | 120, 0 | 120, 0 | 480 x 480 |
| 4:3 | 40, 0 | 40, 0 | 640 x 480 |

* Canon crops **in place** (source = destination), and the shown area never
  reaches the panel's edge (16:9 ends at row 442). Each change comes with a
  transition call (a2 = 1), then normal frames (a2 = 0); word 15 counts.
* XPan's 65:24 placement moves the band (source row 107, destination 0 /
  60 / 214 for Top / Split / Bottom, 266 rows), changes it on normal frames,
  and in Bottom ends exactly at row 480.
* Next test, no new build: the same placement in **Top** and **Split**
  (moved, but away from the panel's bottom edge). No hang there = the
  bottom edge; a hang = the move itself.
* **Top test** (build `103739ee3363`, placement on): hung at the third
  start, same pattern (no restart call after the menu). So the bottom edge
  is not it; what remains is the move (source row != destination row),
  which Canon never does. After a hung power-off the camera stays dark at
  the next power-on until the battery is pulled.
* Next: build `5a8f84ef775b` crops **in place** (`M50_DISP_MOVE 2`: source
  = destination = row 107, 266 rows, the centre, as the JPEG), HUD not
  adjusted (test only).

**In-place test** (build `5a8f84ef775b`, source = destination = 107): hung at
the first try. So any geometry XPan sets in the setter call hangs, not only
a move. **Cause, from Canon's code:**

* Canon builds every Live View display block from its own **geometry table**
  (pointer at `0xB47C`, RAM dump `0x40737500`; ROM image at `E1B1B51C`),
  6 words per entry (shown w, h, source x, y, destination x, y), per output
  and aspect ratio. Screen 3:2 / 16:9 / 4:3 / 1:1: `720 480 0 0 0 0`,
  `720 404 0 38 0 38`, `640 480 40 0 40 0`, `480 480 120 0 120 0`; EVF 3:2
  `1024 682 0 0 0 0` ...; a third output (900 x 600) is shown at 62,84 from
  0,0: Canon itself moves a picture, through the table.
* The table is used by Canon's Live View VRAM manager
  (`Capture/VramDevelop.c`, code around `E07EBBC4`: "AllocImgVramCBR",
  "ReallocImgVramCBR", `WaitForAnyEventFlag(m_hEvent,
  EVENTID_REALLOCATE_IMAGE_VRAM, ALLOCATE_IMAGE_VRAM_TIMEOUT)`,
  "DISP_SetImgRepVramParameter", `vramAddr == m_VramDisplayAddr`, "Invalid
  AspectRatio"): at every Live View start it allocates and confirms the image
  VRAM for that geometry and waits for the display.
* The setter (`E01389AC`) hands the block to the image layer setter
  `E0154B64`, which goes through Canon's compositor (`E0153E48`) to the
  VRAM manager side (`E017BEF0`).
* XPan changed the geometry in the setter call only: the hardware showed
  XPan's band while Canon's display and VRAM state kept 720 x 480 at 0,0.
  When Canon's menu stopped and restarted Live View, that disagreement
  sometimes left Canon waiting for good: black screen, frozen Live View (the
  display task never answers), no power-off. Intermittent: it depends on the
  state the restart catches. Canon's own crops never hang because they come
  from the table. The exact instruction it waits in is not traced.

### 65:24 Live View through Canon's geometry tables (2026-11-10)

* `m50_lvgeo_poll` (XPan's task, not while half-pressed / busy / recording)
  checks Canon's whole table (the five entries above) and patches, through
  ML's patch API, the 3:2 entries' height, source row and destination row for
  the LCD (266 rows from row 106, at 0 / 60 / 214) and the EVF (378 rows from
  152, the scaled destination); even values only, as in Canon's table.
  Undone when 65:24 is off, in Canon's layout, in movie mode, and re-applied
  when the layout changes; refused (and logged in the 65:24 JPEG crop help)
  if the table is not Canon's.
* The display hooks only observe now (record, marks); touch AF and the AF
  box follow the band only when Canon's last LCD block really shows it.
* Open: whether Canon applies a table change at once or only at its next
  Live View start (then the band appears after the first menu / playback).

**Camera test of `f20297824a9f` (RAM table only):** no crash, but Live View
was neither cropped nor moved, in any layout, also after the menu: Canon's
blocks stayed `720 480 0 0 0 0`. The blocks come from a second, constant
copy: Live View's **DeliverStage** (`Conductor/Epp/Deliver/DeliverStage.c`,
`E00705F4`: `ldr r1,=E09E1DD4`, entry = pattern x 24 + display type +
aspect, copied into DeliverStage's state). ROM table `E09E1DD4`, three
patterns of 24 entries; pattern 0 = the RAM copy = what Canon sent in the
aspect test (photo Live View); pattern 1 a smaller picture with borders,
pattern 2 another mode (16:9 at 0,0 shown at row 37).

**Build `8e1b0daf4834`:** the band in both copies, as Canon's own crops are:
pattern 0 of the ROM table (LCD 3:2 entry 0, EVF 3:2 entry 4) through ML's
runtime MMU patch (one more reserved page: `CONFIG_MMU_REMAP_PAGES 8`), and
the RAM table as before. Both checked against Canon's values first;
`check_m50_rom.py` checks the ROM entries and DeliverStage's literal.

### Canon's main menu breaks Live View: the cause (2026-11-11, from the logs, no new test)

Analysis of every boot in `XBOOT.LOG` with the menu marks (builds
`7c16a8e4cf67` ... `332341d80a0c`) and every start in `XM50.LOG`:

| build | band on screen | menu closes | hangs |
|---|---|---|---|
| `e2d704c10bf4` placement off | no | 18 (4 boots) | 0 |
| `f20297824a9f` RAM table only | no | 13 (3 boots) | 0 |
| hook builds (`7c16`, `1037`, `5a8f`) | yes | 15 | 5 |
| `8e1b0daf4834` both tables | yes | 5 | at the next start |
| `332341d80a0c` hook + snapshot | yes | 11 | 0; Err 70 at a boot and when changing the layout |

* **Every hang is the first menu close of its boot**; in the boots where the
  first close went well, none of about 20 later closes hung.
* **Canon changes Live View geometry only inside a transition**: a setter
  call with a2 = 1 (a Live View stop / start), then normal frames (a2 = 0).
  Its own crops (16:9, 4:3, 1:1) come that way (aspect test). Apart from a
  Live View stop / start (menu, playback) Canon makes no transition after
  power-on (every start in `XM50.LOG`); INFO makes none.
* **XPan changed it outside one.** Canon starts Live View before ML is up
  (first frame ~1.33 s, MMU hooks ~1.35 s, ML settings ~1.65 s); a few frames
  later XPan's band appears on normal frames, no transition between (every
  band boot: unmoved a2 = 0 frames, then moved a2 = 0 frames). The first Live
  View stop after that is the first time Canon's display stops and restarts a
  picture whose geometry changed behind its back: sometimes it never
  restarts (black screen, frozen Live View, no power-off; Canon's
  VRAM/display waits such as `ALLOCATE_IMAGE_VRAM_TIMEOUT` are assertions,
  which on the M50 freeze the camera or end in Err 70). Once one restart has
  gone through, every transition carries the band and it never fails again.
* It fits every build: placement off and RAM-only never change the
  geometry; every hook build changes it at boot; the latch fix
  (`103739ee3363`) still changed it at boot (it decided on normal frames);
  both tables changed it at 3 s when patched (and Canon then made a
  transition of its own, with mode argument 2, followed by the stutter);
  the snapshot build changed it when the layout changed (Err 70).
* Not traced to the instruction: which Canon wait or assertion fails.

**Fix (build `fe99a229ffe7`, 2026-11-11, waiting for the camera test):**
`disp_hook` decides the band (`band_decide`) only when Canon's setter is
called with a2 != 0 and keeps it, per output, for every normal frame; the
frames before Canon's first transition after the hooks are installed keep
Canon's geometry; touch / AF follow the latched geometry. `build_xpan.sh`
fails if the geometry can change anywhere else. The table patches (RAM and
ROM), the hang snapshot and the David probe are removed; 7 MMU pages again.

Design: the geometry may change only inside Canon's transition call: XPan
decides the band on a2 = 1 calls and keeps it, unchanged, for every normal
frame until the next transition (the opposite of the failed latch). No ROM
page, no table patch, no extra memory. Cost: after power-on (and after a
layout or 65:24 / 3:2 change) the band appears at Canon's next Live View
stop / start (first menu, playback ...), not at once.

### Canon's main menu breaks Live View: forced restart (2026-11-11, build `766c9f6c6f54`, not camera-tested)

The fix above was camera-tested (`fe99a229ffe7`): no hang; the band came
on every menu close (panel mark a2 = 1, moved), and on the LCD / EVF
switches (a2 = 2); INFO, Q, SET and XPan's own layout changes make no
transition. Without a transition the band waits for the next one, so
XPan now asks Canon for one:

* **How:** Canon's menu request, `SetGUIRequestMode(GUIMODE_MENU)` (what the
  MENU button asks for; ML already uses it on the M50), then
  `SetGUIRequestMode(0)` once `MENU_MODE` and Live View has stopped (as
  after a MENU press, ~120 ms in the logs). Canon's restart makes the
  transition call, which places the band (`band_decide`, unchanged).
* **When:** the active output (the one whose setter is being called) shows
  another band than the settings want, for 1 s, with Canon idle (Live View,
  not magnified, photo mode, XPan view, no dialog, no ML menu, not the Canon
  layout, no half-press, lens idle, not recording). Covers power-on, layout
  and 65:24 / 3:2 changes. Runs in the XPan task, never blocks.
* **Bounds:** each target is tried once; a step not reached in 3 s stops
  the forced restarts until the next start (mark 78 logs Canon's GUI mode).
  Menu: XPan > "Live View band restart" (on/off, done/failed).
* **Cost:** Canon's menu shows for a moment, ~1 s of black, as MENU twice.
* **Not used:** the LCD / EVF switch (transition a2 = 2, Live View keeps
  running). The eye sensor sets PROP_DISPSENSOR_CTRL and Canon switches
  PROP_LV_OUTPUT_DEVICE (OutputChangeCtrlSTM); forcing it needs a property
  write, off on the M50.

### Forced restart: camera test (2026-11-11, `766c9f6c6f54`, failed)

First start: XPan asked for the menu at 5924 ms; Canon made a transition
call at 5954 ms (a2 = 1, not moved), which a MENU press never makes when
the menu opens; `lv` went to 0 and XPan asked to leave at 6005 ms, 80 ms
after the open. Canon's setter then stopped (5 calls, then 0 every second):
Live View frozen; the user's own MENU / MENU later did not bring it back.

* `lv` is PROP_LV_ACTION: Canon announces the stop there when it starts, not
  when the stop is done. `MENU_MODE && !lv` was no "stopped" state.
* `SetGUIRequestMode` is not the MENU button's path: it reaches Canon's GUI
  mode directly, not through the Live View dialog, and ignores Canon's UI
  lock. The user's fastest menu closes were 770-840 ms after the open.
* The band was never moved: this freeze is not the geometry problem.

Next: the MENU button's own path, fake MENU presses into Canon's GUI queue
(`fake_simple_button`, as ML's `open_canon_menu`), the close sent only once
Canon's UI lock (PROP_ICU_UILOCK) is clear; first a read-only log of the UI
lock around the user's menu cycles.

### Menu hang without the band (2026-11-11, `78556736e353`)

First start of the UI-lock log build: MENU at 4803 ms (PROP_LV_ACTION 1 at
4828), MENU again at 5650 (PROP_LV_ACTION 0 at 5661, Canon's GUI back to
Live View at 5695), then Canon's display setter was never called again (2,
then 0 calls per second; no transition call), the user's next MENU / MENU
did not help. XPan had not moved the band at all in that start (no
transition before, every frame Canon's block). So:

* The cause written above ("geometry changed outside Canon's transition")
  is wrong: the hang comes without it. The hook builds hung more often
  (5 of 7 first closes) than the others (2 of 16), small numbers.
* Canon's UI lock (PROP_ICU_UILOCK) stayed 0 the whole time.
* PROP_LV_ACTION 0 is Canon announcing the start; the start then never
  reaches the display. ML's tasks keep running during the hang (the boot
  log kept writing for 19 s), so a detector could save Canon's own debug
  messages at that point and show which step stops.

### Canon's own debug log at the hang (ROM study, 2026-11-11)

* `DryosDebugMsg` (E0577FD0) uses Canon's debug manager, pointer at RAM
  0x50C0. Per class (struct +0x38 + 3 x class): print level (+0), store
  level (+1). The message is formatted (73 characters) and, when stored,
  posted (E0559100, `msg_queue_post`) to Canon's log object at struct +4,
  handled by Canon's own log task.
* Canon registers the event procedures `dmstart`, `dmstop`, `dump`, `dumpf`,
  `dmstore`, `logstat`, `dmhistory` ... (E05789D0). `dumpf` (E057898C ->
  E05782CE) only posts command 4 to that log object: Canon's log task then
  writes the stored messages to the card itself (`%s%s_%d`, `[DM]` errors).
  It does not block the caller and allocates nothing in ML.
* ML's minimal M50 build calls `call("dumpf")` without changing any level
  (`minimal-d678.c`), so it works on the M50 with Canon's default levels;
  how much the default store keeps is only known from a dump.
* During the hang ML's tasks keep running, so XPan can ask Canon for the
  dump then: a detector (Canon announced the Live View start, PROP_LV_ACTION
  0, and its display setter got no call within 2.5 s) calls `dumpf` once,
  plus a menu item to save one after a good menu cycle for comparison. No
  memory, no hook, no wait.

### Menu hang: Canon's own log (2026-11-11, `1cf77b85ecb6`, 3 hangs, 3 dumps)

Canon's log (LOG000-002.LOG at the card root, written by `dumpf`; Canon's
clock is ML's + ~2.1 s) shows the same thing in both complete dumps:

* **At ML's start** (Canon 3429 -> 3443 ms = ML 1343 ms, "MMU
  apply_normal_patches"): ~13 ms without a single message, then
  `[LVEVFC] ERROR Blanking DeadLine`, eight `[CAPD] WARN
  EISSyncTimer0CBR:OVERRUN`, and `[CAPD] ERROR _imgCtrl_LV_EventDispatch
  Current=8 / 6, dwEventID=3, 4, 5, 6`: Canon's Live View image control got
  its events in the wrong state. Live View keeps showing pictures.
* **Menu open:** a normal Live View stop, except `[LVCOM] ERROR [ADST]
  ADST_CheckMemStatus(2):Locked`.
* **Menu close:** `process_StarLiveViewApp`, `evfActive[14]` "Start Allocate
  Resource"; the EVF device side never answers (no `evfdevSelectPath`, no
  `evfCompleteEvfDevCBR`, no "End Allocate Resource"; at power-on it answers
  in 12 ms), and 1 s later `[PO] ERROR LvCtrl ResTimeout:0x2`. That is the
  frozen Live View.
* No other error. XPan's card reads (LUT scan) came after the hang in one
  dump: not the cause. The 65:24 crop patch at ~4.7 s caused no error.

**Cause:** `apply_normal_patches` applies the 9 hooks in one
`apply_patches` call: interrupts off on cpu0 for the whole batch, cpu1
parked for each patch, and for each new ROM page a 64 KB copy from ROM inside
that section (7 pages), plus a full cache sync per patch. Canon has been
running Live View since ~1.33 s, so it misses its frame deadlines and its
image control is left out of step; the next Live View start fails. It fits
the history: more hooked pages, longer stall, more hangs; intermittent,
because it depends on where in Canon's frame the stall falls.

**Fix (build `806601dc65a3`, not camera-tested):** copy the ROM pages into the reserved heap pages
before the critical section (ROM does not change), and apply each hook in
its own `apply_patches` call, so interrupts come back between hooks and
each section only edits the tables. Verify with Canon's log at every start:
no `Blanking DeadLine` / `OVERRUN` / `_imgCtrl_LV_EventDispatch` errors.

### Menu hang: first fix tested (2026-11-11, `806601dc65a3`)

Three starts, Canon's log saved at each (LOG003-006):

* **ML's start: fixed.** No Blanking DeadLine / OVERRUN / imgCtrl error at
  ~1.34 s in any of them.
* **The same errors at ~4.7 s** (Canon 6818-6835 ms) in two of them: the
  65:24 JPEG crop patch (`m50_crop_poll`, task start + 3 s), which remaps
  the encoder table's ROM page with `apply_patches` and copied that 64 KB
  page inside the critical section. LOG004: one OVERRUN only, no imgCtrl
  error, 4 menu cycles fine. LOG005: Blanking DeadLine + imgCtrl error
  (Current=8, dwEventID=3), ADST "Locked" at the menu open, and the first
  menu close hung (ResTimeout). LOG003: no error, 3 cycles fine.

Fix (`d31841da9ff2`, not camera-tested): `apply_patches()` itself copies
the ROM pages of its patches before `cli()` (`mmu_prefill_rom_pages`), so
every caller, boot hooks and runtime patches, gets it; build check.

### Menu hang fixed; forced restart through Canon's MENU button path (2026-11-11)

`d31841da9ff2` camera test: 3 starts, 15 menu closes, no hang; Canon's log
at each start (LOG007-009) has no Blanking DeadLine / OVERRUN / imgCtrl /
ResTimeout error. The menu hang was ML's ROM page copies inside
`apply_patches`' critical section (README "Menu hang: Canon's own log").

Forced restart on again (`46a84ccbd3f2`, not camera-tested), now the way the
user does it: XPan presses MENU through Canon's button queue
(`fake_simple_button(BGMT_MENU)`, so Canon's IDLEHandler handles it as the
real button: AF stop, metering stop, then its own SetGUIRequestMode), and
presses it again once Canon is in its menu (`MENU_MODE`), Live View stopped
(`lv` 0) and Canon's display setter has had no call for 300 ms. XPan no
longer calls SetGUIRequestMode (build check). Same triggers, once per
target, 3 s bounds, XPan > "Live View band restart". The per-start Canon log
is off (`M50_BOOT_DUMP 0`); the dump at a hang stays.

### Quick review, playback and the EXIF size (2026-11-11)

`46a84ccbd3f2` camera test: the forced restart works. Two display faults:
the quick-review image sat at the Live View band's place instead of
Canon's centre, and playback images came up stretched (full height) until
Canon's next transition call. Cause: the latched band was applied to every
plain 720x480 panel block, and quick review / playback use the same block.
Fix (`de685678701f`, not camera-tested): the band moves only while Canon
shows Live View (`lv && !PLAY_OR_QR_MODE`), build check.

The photos themselves: main image 6000x2216 (JPEG SOF), EXIF
PixelX/YDimension 6000 x 4000, and Canon's embedded images 160x120 and
1620x1080 hold the full 3:2 frame (no black rows), in old and new 65:24
photos alike.

EXIF size (ROM): Canon's EXIF header builders (E00A39A8, E00A41AC,
E00A4A02, E00A5246, E00A54DC: tag A002 / A003 at header +0x136 / +0x142,
values at +0x13E / +0x14A) leave the values empty; E00A80B2 fills them from
a rectangle in a shooting-parameter structure (r7): width = [+0x1D0] -
[+0x1C8] + 1, height = [+0x1D4] - [+0x1CC] + 1 for size codes 0x1000 /
0x10000, the rectangle at +0x1D8..+0x1E4 for 0x4000 / 0x8000.

Traced (2026-11-11): E00A80B2 is inside **E00A7DF2** (Canon's EXIF entry
filler: r0 header, r1 size code, r2, r3, then three stack arguments), whose
only caller is E009A0BC in E0099F74 (FCS save job, called from fcsExecJob
E0076DC4 via E00774C8). The rectangle structure (E00A7DF2's 6th argument)
is the FCS job object + 0x20, so the EXIF size is the FCS job's own copy of
the shot's area, not the size table XPan patches (which sets the JPEG SOF
height). E00A7DF2 only reads that structure (fields up to +0x58C, no
store), so the standard fix is a hook on E00A7DF2 that, with XPan's 65:24
crop on and the rectangle 6000 x 4000, passes Canon a copy of the
structure with the rectangle's rows narrowed to the 2216-row band (top
+892), and Canon's own pointer otherwise. Cost: one more hooked ROM page
(E00A0000), CONFIG_MMU_REMAP_PAGES 7 -> 8 (64 kB heap: margin ~208 -> ~144
kB). Canon's ROM sensor table E19E0230 (276, 48, 6275, 4047 = 6000 x 4000)
is the full-frame area.

**Correction:** E00A7DF2 is the CR3 (CRAW) path (fcsExecJob's branch that
logs "PT_CRAW"). The JPEG's EXIF filler is **E00A551C** (only caller
E0072DB6, three stack arguments): it writes A002 / A003 (header +0x142 /
+0x14E) from a size table passed as its 6th argument, chosen by aspect
([r6+0x29C]); 3:2 L reads +0x48 (width) and +0x4C (height), the same layout
as Canon's size table at *(0xB4D4) that the crop patches, but the FCS job's
own copy, so it kept 4000. It only reads the table (fields up to +0x58C).

### 65:24 EXIF size (build `11febf286d06`, 2026-11-11, not camera-tested)

Hook on E00A551C (`xpan_m50_exif_jpeg`): with the 65:24 crop on and the 3:2
L entry 6000 x 4000, Canon gets a copy of the table (0x600 bytes) with the
height 2216; otherwise its own pointer. The trampoline replays push.w
{r0-r11,lr}; mov r7,r2; sub sp,#12. CONFIG_MMU_REMAP_PAGES 8 (one more 64 kB
heap page). Counts in XPan > 65:24 JPEG crop (help line). Build checks: only
the height changes, only with the crop and Canon's 6000 x 4000; ROM check of
the prologue, the table reads and the call. CR3 EXIF is left as Canon writes
it (the raw file is the full frame).

### 65:24 EXIF size: camera test and the Canon maker note (2026-11-11)

`11febf286d06` camera test: quick review centred, playback not stretched on
the first image; **EXIF PixelYDimension 2216** (IMG_2773, IMG_2774). Still
4000 in Canon's maker note, and browsing in playback shows each new image
squashed for an instant.

Canon's own 16:9 photo (IMG_2694) shows what Canon writes for a crop:
AFInfo2 (0x26) CanonImageWidth/Height 6000 x 3368 and AFImageWidth/Height
6000 x 3368; AspectInfo (0x9A) [7, 6000, 3368, 0, 0]; CropInfo (0x98)
[left 0, right 0, top 316, bottom 316]. XPan's 65:24 photos: AFInfo2 6000 x
4000 / 6000 x 4000, AspectInfo [0, 6000, 4000, 0, 0], CropInfo [0, 0, 0, 0].

ROM:
* Canon's camera-info table (ROM E19E0068, "Canon EOS KISS M", copied to
  RAM): +0x48 the L sizes per aspect (3:2 6000x4000, 4:3 5328x4000, 16:9
  6000x3368, 1:1 4000x4000), M at +0x68, S1 +0xC8, S2 +0xE8, **the playback
  preview at +0x128 (3:2 1620x1080)**, the sensor area at +0x1C8. Copies:
  *(0xB4D4) (patched by the crop), *(0xB4F4) (read by E00A020C), and the
  FCS job's own at job + 0x20 (what the EXIF fillers get).
* Maker-note builder **E00A8DE8** (4 callers in FCS, E0073D82 ...): copies
  CropInfo from its 5th argument + 0xAA2 (8 bytes) and AspectInfo from +0xAAA
  (20 bytes); it only reads that structure (up to ~+0xFC6). It is in the
  page XPan already hooks (E00A0000): a hook passing a copy with CropInfo
  [0, 0, 892, 892] and AspectInfo [0, 6000, 2216, 0, 0] needs no new page.
* AFInfo2's CanonImageWidth/Height: Canon's AF converter E00FExxx logs
  them ("[FCS] wOriginalImageWidth / Height"); source not traced yet (page
  E00F0000, would be one more page).
* The playback flash: Canon shows the embedded 1620x1080 preview first,
  now scaled to the 6000x2216 EXIF size, then the decoded image. Fixing it
  means a 65:24 preview (table +0x128 and the preview's encoder table near
  ROM E0FD0C70), a larger change in Canon's encode path.

### The AF block's image size, and where all the FCS sizes come from (ROM, 2026-11-11)

* AFInfo2's CanonImageWidth/Height and AFImageWidth/Height are written by
  E00FEDC4 (logs "[FCS] wOriginalImageWidth / Height"), from its 5th / 6th
  arguments; its caller E00FFB72 (from E00FB9B8, E00FE102, E0101424) passes
  four sizes that E00FE058 gets from **E00FDE6C(job, ..., job + 0x20, out)**:
  the same table copy at the FCS job + 0x20 that the EXIF fillers read.
* That copy is made by **fcsActivate** (E0075810, an FCS state handler, table
  E0A90C70): PROP_GetMulticastProperty(PROP_DSDEFINE) and
  memcpy(job + 0x20, data, 0x5A4). PROP_DSDEFINE is the camera-definition
  table (ROM default E19E0068, "Canon EOS KISS M": sizes per aspect at
  +0x48, preview +0x128, sensor area +0x1C8). fcsActivate runs when the FCS
  module starts, before XPan's crop (4.7 s), so every FCS consumer (EXIF
  A002/A003, the AF block, the CR3 rectangle) reads 6000 x 4000.
* The FCS object: *(0x538C) (FCS_PostShootJob E004FD70 asserts it, [+4] is
  its state machine). So the copy's 3:2 L height is very likely at
  *(0x538C) + 0x20 + 0x4C.
* *(0xB4D4) (patched by the crop) is read by E0082xxx, the capture-side
  size selection; *(0xB4F4) by E00A020C.

Proposed fix: with the crop, also patch the FCS copy's 3:2 L height (4000 ->
2216) through ML's patch API, after checking that *(0x538C) + 0x20 starts
with "Canon" and holds 6000 / 4000 at +0x48 / +0x4C (else refuse); undone
with the crop. Then every FCS writer works from Canon's own data, the way it
does for Canon's 16:9 (where CropInfo top / bottom 316 = (4000 - 3368) / 2 is
expected to become 892). The EXIF hook becomes a no-op (it acts only on
4000) and can be removed after the test, freeing its ROM page.

Built (`0a6e15fd0b08`, not camera-tested): `fcs_size_entry()` checks
*(0x538C) + 0x20 ("Canon", 6000 x 4000, 6000 x 3368) and `m50_crop_poll`
adds its 3:2 L height (4000 -> 2216) as a 4th crop patch, applied and undone
with the others; when the copy is not as expected the crop goes on without
it. XPan > 65:24 JPEG crop help: "Save module's table: 2216 / not found",
and the EXIF hook's count (it should stay 0 with the copy patched).

Camera test (IMG_2775-2778): the 65:24 photos now carry, from Canon itself,
ExifImageHeight 2216, AFInfo2 CanonImageWidth/Height 6000 x 2216 and
AFImageWidth/Height 6000 x 2216, AspectInfo [0, 6000, 2216, 0, 0]; the 3:2
photo (IMG_2778) keeps 4000 everywhere. CropInfo stays [0, 0, 0, 0]: Canon
computes the margins only for a non-3:2 aspect (316 is not in its table),
so 892 would need the maker-note hook (E00A8DE8, CropInfo at its 5th
argument + 0xAA2, same ROM page as the EXIF hook). The EXIF hook no longer
acts and can go.

Decision (user, 2026-11-11): CropInfo stays as Canon writes it (0); the EXIF
hook on E00A551C is removed (`e63d817820ba`): 9 hooks, 6 hooked ROM pages,
CONFIG_MMU_REMAP_PAGES 7 again; build check that it does not come back.

### XPan menu (2026-11-11, `60a663e074b4`, not camera-tested)

* **Greyed rows:** ML draws an IT_BOOL row from its priv; Custom LUT, Live
  LUT preview and Film grain have none (their own SET toggles), so ML drew
  them OFF even when on. Their update functions now set the state
  (MENU_SET_ENABLED).
* **Layout:** cropmarks.c's rows (aspect, alignment, format, quality,
  display, unchipped lens), then Custom LUT (Cube file, Refresh, strength,
  Tone base, Live LUT preview) and Film grain (strength, size), then one
  **Diagnostics** submenu with every development row: 65:24 JPEG crop
  status, Live View band restart, Photo LUT and Grain stage statistics,
  Canon settings, ISO reader, ISO write test, Learned settings, Save Canon
  log, Touch probe, Exposure probe, Debug info (moved there from
  cropmarks.c on the M50). The "Watermark - not on the M50 yet" row is gone
  (its placeholder stays hidden). User rows have short help without
  internal counters.
* Build checks: those rows set their state; the development rows are only
  inside Diagnostics; no Watermark row.

### Menu, Debug info, capture format and forced settings (2026-11-11, `22c3b16d96a8`, not camera-tested)

* **ISO** is read like any other setting: the PROP_ISO slave always starts 5 s
  after boot; the ISO reader switch, its guard file and the ISO reader / ISO
  write test rows are gone.
* **Debug info** (now the last row of XPan's menu) gates every development
  record and log: OFF, there is no XBOOT.LOG (the boot marks stop once ML has
  started), no XM50.LOG, no XM50OPTS.LOG report, no Canon log dumps or hang
  detector, no exposure / display / Hilton / AF records. The Diagnostics
  submenu is shown only with Debug info ON. The key log was already gated.
* **HUD LUT line** (40 px under "LUT"): the strength ("100%"), "..." while the
  LUT loads, "!" when it cannot grade, "OFF".
* **Capture format on the M50**: Canon's PICQ there is [size, kind, quality,
  RAW type] (XM50OPTS.LOG: 00 01 03 03, 00 07 03 03, 00 07 03 04, 00 07 02 03,
  00 06 03 03), not the EOS M codes, and ML's prop_request_change is off, so
  the shared rows and the HUD FMT field did nothing (the FMT field needed
  learned options). Now XPan builds the value: JPEG / RAW+JPEG / cRAW+JPEG,
  Large Fine / Large Normal, always L with a JPEG, written to the PICQ
  aliases Canon reports (m50_picq_write_now: only that layout, not while
  busy). Capture format and JPEG size/quality use it, as does the FMT field.
* **Forced Canon settings** (as on the EOS M): while XPan is on, in P / Tv /
  Av / M, idle: Picture Style Neutral (0x84, Canon's length), aspect 3:2 (0,
  only with Canon's length 4), an L format with a JPEG (RAW only becomes
  RAW+JPEG). Style and aspect are saved and put back when XPan goes off or to
  movie mode, if they are still XPan's values; one try per second, five per
  change. Not yet on the M50 (layouts unknown): Neutral's contrast /
  saturation / colour tone, ALO, HTP, colour space, lens corrections.

### RAW and Film grain, the 3:2 message, LUT names (2026-11-11, `71a89dddc5b8`, not camera-tested)

Camera test of `22c3b16d96a8`: no RAW format could be set (always JPEG,
also in Canon's own menu), and the aspect ratio was not forced.

* **RAW:** Film grain switches on Canon's creative-filter stage
  (PROP_LV_CFILTER, Rough Monochrome mapped to none in Live View), and Canon
  allows only JPEG while a creative filter is on. RAW now wins: while a RAW
  format is chosen (or being set) Film grain pauses (`film_stage_wanted`,
  HUD grain icon off, a note in Film grain's menu row), the filter goes off,
  and the RAW write waits for that (up to 3 s). Canon's own menu still
  cannot offer RAW while the grain's filter is on: choose RAW in XPan (HUD
  FMT or Capture format).
* **Aspect:** XPan's PROP_IMAGE_ASPECT_RATIO write had no effect, so it is
  gone; with another aspect the HUD shows "XPan needs 3:2 / Set Canon's
  aspect ratio to 3:2" in the picture (xpan_native_look_blocked).
* **LUT names:** Canon's file system lists only the FAT 8.3 alias of a long
  name (".cube" alone makes a name long), so the HUD now drops the alias's
  "~N" as the EOS M does: IDENT~1.CUB shows as IDENT. Names longer than 6
  characters still show their first 6.

### Menu choices, the 3:2 panel background, the UI state log (2026-11-11, `94516ebf7a17`, not camera-tested)

Camera test of `71a89dddc5b8`:

- **Tone base and Grain size** now use ML's choice lists (`.choices`, `IT_DICE`). ML's picker shows the names, not numbers, and Fine (0) is no longer drawn as OFF. Before, Grain size had no icon type, so ML guessed `IT_PERCENT_OFF` and drew 0 as off. "Cineon log +1 EV" is now "Cineon +1EV", which fits beside the label. The "N photos" counter on the right, a development count of headroom photos, is gone.
- **The 3:2 panel background.** `COLOR_TRANSPARENT_BLACK` (index 3) was fully transparent in ML's RGBA palette (`src/bmp.c`), so the 3:2 panel had no background on a white scene. On the M50 it is now 0xA0000000 (black, about 63% opaque), as the index looks on the older bodies.
  - The colour is black, so straight and premultiplied alpha give the same result.
  - ML's initial layer clear now uses `COLOR_EMPTY` (it used index 3, which mapped to the same 0 before). Without that, the whole layer would start dimmed.
  - XPan's clears already use `COLOR_EMPTY`.
  - ML's other `COLOR_BG` uses on the M50 (console, focus distance text, histogram) now get the same semi-transparent background as on the other bodies.
- **The crash after playback in the Canon layout (3:2).** It is not found yet. The card's logs of that session stop before it: the boot log covers only the first 60 s, and XM50.LOG caps its Ximr and display records. With Debug info ON, XM50.LOG now gets one line per UI state change, written from a ring of 8 (`m50_log_ui`, shoot task). Each line holds gui_state, lv, ML menu, frozen, uifrozen, Canon layout state, view, Canon's GUI mode, playback, overlay mode and whether Canon's front buffer is off. This costs 256 bytes of RAM (Canon keeps 0xB2E90, margin 208512).

Build checks: those two rows need a choice list, and the long Tone base name is refused. The M50's index-3 colour and ML's `COLOR_EMPTY` initial clear are checked, and the UI state log must run only with Debug info.

### Canon's log at a stuck power-off (2026-11-11, `f240cf24096b`, not camera-tested)

Camera test of `94516ebf7a17`, Debug info ON, XPan 3:2 in the Canon layout. The UI state log caught the session:

- Eight trips from Live View to playback and back worked.
- The ninth came back after about 5 s of photo browsing. Live View started at 70.37 s, XPan cleared its layer and froze it at 70.91 s, and then nothing more was logged (the key log stops at 70.55 s).
- The screen went black. After the power switch was turned off the LED stayed on, and only a battery pull helped.

That is the earlier menu hang's signature: Canon's power-off waits for a Live View controller that no longer answers (`[PO] ERROR LvCtrl ResTimeout`). XPan's steps at that moment were the same as in the 8 good trips, and in 3:2 XPan applies no patches.

Canon's hang detector did not fire, because Live View had started (it only catches a start that never reaches the display setter). Once the picture is up, display setter calls are too rare for a "no calls" rule to tell a black screen from a normal one.

New: `canon_dump_stuck_off()`. With Debug info ON, if the camera is still running 5 s after the power-off started, XPan calls Canon's `dumpf` once. Canon keeps its recent messages in RAM, so the file (LOG00N.LOG at the card root) should cover the hang before it.
- It runs in XPan's task, both inside the power-off guard of the task loop and after the loop ends, until the power is cut.
- It is the only card access at power-off, and it is Canon's own writer.
- A normal power-off ends before 5 s, so it never runs then.

Build checks: only this function and `canon_dump_step` call `dumpf`. This one is limited to Debug info, 5 s and once, and does not block, write properties or change Canon's log levels. The task loop's power-off guard is still checked.

Test: reproduce with Debug info ON, switch off, wait 15 s, then pull the battery and bring the card.

### The playback black screen: Canon's log (2026-11-11, `412f09eef302`, not camera-tested)

Camera test of `f240cf24096b`: the stuck power-off saved Canon's log (LOG010.LOG, Canon clock = ML + about 2.1 s).

- **XPan's side.** At ML 58.11 s you came back from playback with PLAY; Live View was on at 58.16 s. The Canon layout's clear step (state 1) followed at 58.40 s. The freeze line (state 2) never came, and the key log and the shoot task's once-a-second marks stop there too. XPan's own task stayed alive: it saved the log.
- **Canon's side.**
  - Live View started normally: `process_StarLiveViewApp` 60.215, `evfdevSelectPath`, `LvPoCom:Comp(4)` 60.564. The exposure task logged every 200 ms until the power-off, with no ERROR or WARN lines.
  - Leaving playback with PLAY, Canon muted the screen (`GUILockTask TurnOn -> MuteOn`, TFT 1), then `GUILock_TurnOffDisplay` and `GUILock_EnableProhibitState` at 60.214.
  - Canon's GUI wrote nothing after 60.296 (`guiSetTouchShutterDisableFromLV`). No `GUILock_TurnOnDisplay` and no `DisableProhibitState` followed, and the display task never got event 3 (`SetTftStatus (0)`), so the screen stayed black.
  - PLAY at 63.94 reached Canon's event manager (`SwPlay_Button[1]`) but no GUI handler.
  - The power-off at 66.69 saved ML's settings and then stopped.
- **Reading.** Canon's GuiMainTask and ML's shoot_task both stopped at the same moment, about 0.3 s after Live View came back, while Live View itself kept running. A wait between those two tasks (a lock or a semaphore one holds and the other wants) fits.
  - `DispConEmergencyStop` is not the cause: it appears at every mode change.
  - The M50's "front buffer off" flag is an ML-only variable that Canon never reads.
- **New: task states in Canon's log** (`m50_log_tasks`). Just before the stuck power-off dump, and the menu "Save Canon log", XPan writes one `XPT` line per task into Canon's own log with `DryosDebugMsg` (DM_MAGIC level 3, which is recorded: ML's `config_save_file` line shows up as `[FCACHE]`). No card access. Each line holds the name, state, wait type and object, saved PC/LR and up to 8 code addresses (Canon ROM or ML text) from the task's stack. A header line gives `winsys_sem`, `bmp_lock` and the current task. It only reads DryOS's task records.

Build check: `m50_log_tasks` is called only from the two Canon log dumps and does not write, block or call Canon.

Test: same as before. Debug info ON, playback and back in the Canon layout until the black screen, switch off, wait 15 s, battery, card.

### The playback black screen: task states (2026-11-11, `d0c43db2e1aa`, not camera-tested)

Camera test of `412f09eef302`: LOG011.LOG holds the `XPT` lines.

- Canon's log keeps only about 60 characters, and a few values, per message, so the PC and stack part of each line was lost.
- **What did survive.**
  - `shoot_task` and `livev_hiprio_task` (ML) both wait with type 6 on the same object `1fa574`. `AcquireRecursiveLock` (E06D77DC) takes a kernel mutex (handle >> 1, E0572C76), so type 6 is a mutex wait here.
  - The lock both of those ML tasks take is `bmp_lock` (`BMP_LOCK`).
  - `GuiMainTask` waits with type 2 (an event flag) on `1f7d9c`, not on its message queue.
- **Likely chain.**
  - `redraw()` posts `MLEV_REDRAW`, and `_redraw_do` (zebra.c) runs in GuiMainTask. It takes `bmp_lock` and calls Canon's `dialog_redraw(LiveViewApp_dialog)` (E054F3CA, then the window redraw E054D906 / E054D3C4).
  - The Canon layout's steps call `redraw()` right after Live View comes back from playback. That is the moment Canon rebuilds its Live View dialogs (`CreateDialogBox` 55/50/51/65) with the screen muted and turned off (`GUILock_TurnOffDisplay`, `EnableProhibitState`).
  - If Canon's redraw then waits for an event that only its own GUI task can give, GuiMainTask never returns: the screen is never turned on, PLAY is ignored, and ML's tasks wait forever on `bmp_lock`. It is intermittent because it depends on when the redraw arrives.
- **Confirmation round (no change in behaviour).**
  - Short lines: `XPT id name sN wN oOBJ`. For tasks waiting on more than a sleep or their queue, and for ML's tasks, there are also `XPC id pc lr id <full task id>` and two `XPS id` lines with 8 stack code addresses.
  - `XPM` lines hold the first 8 words of each mutex a task waits on, to show its owner.
  - `XPR stage N for T ms` shows where ML's redraw is (zebra.c `ml_redraw_stage`): 1 `bmp_lock` held, 2 inside Canon's `dialog_redraw`, 3 back from it, 0 done.

Build check: the stage marker only records, around `dialog_redraw(LiveViewApp_dialog)`.

Test: as before.

### Display on (2026-11-11, `2ff2f5e16469`, not camera-tested)

Camera test of `d0c43db2e1aa`: the black screen came this time on leaving Canon's menu. LOG012.LOG confirms the cause.

- **What the log shows.**
  - `XPR stage 2 for 5981 ms`: ML's redraw (zebra.c `_redraw_do`, in GuiMainTask) had been inside Canon's `dialog_redraw(LiveViewApp_dialog)` for 6 s.
  - shoot_task, livev_loprio_task, livev_hiprio_task and cls_task all wait on mutex `1fa574` (their stacks end in `AcquireRecursiveLock` E06D77DD/E06D7825). That is `bmp_lock`, held by the redraw.
  - GuiMainTask waits forever (timeout 0) on bit 1 of the event flag `EvZicoWait` (E02787E0 → E05532DC, KerFlag). That is the "drawing done" answer from Zico, the graphics core, through `MzrmGraphic`. CtrlSrv, which serves those jobs, waits on a semaphore.
- **When it happened.** The redraw went in at Canon 17.604 s:
  - MENU at 17.550: Canon mutes the screen (`MuteOn`, TFT 1) and closes its menu dialogs.
  - `START_IDLE_MODE`, then `GUILock_TurnOffDisplay` and `EnableProhibitState` at 17.559.
  - `StartLiveViewApp` creates the new Live View dialog at 17.569; the AF frame follows at 17.575 and the level at 17.590.
  - Canon turns the display on only once its GUI has finished. The playback hang (LOG010) had the same order.
  - ML's redraw is an ML event handled in GuiMainTask between Canon's own messages. Its drawing job cannot finish with the display layers off, and the task that would turn them on is the one waiting. Canon itself never draws in that window.
- **The fix: Canon's display-off flag.**
  - `GUILock_TurnOnDisplay` / `TurnOffDisplay` (E065ADCA / E065AE48) set byte +1 of the GUILock object 0x8EB8 through E065AD8A, under GUILock's semaphore. TurnOn writes 0 (E065ADD8); TurnOff (E065AE5A) and the power-off turn-off (E065B302) write 1.
  - On the M50, `DISPLAY_IS_ON` (consts.h, was a fixed 1) now reads that byte, the same kind of flag the older bodies use (500D/50D `fDisplayTurnOn`). ML's core then skips its display work while Canon's display is off, as it does on the other bodies.
  - Also on the M50:
    - `_redraw_do` does nothing with the display off, and marks the redraw as deferred; XPan's shoot-task step does it once the display is on.
    - `refresh_yuv_from_rgb` does not push ML's layer with the display off; the push stays pending.
    - XPan's Canon layout waits for the display before its clear and freeze steps, and the dim overlay does not start with it off.
  - The UI state log gets `display`.
  - Build checks: the ROM code of the flag and its three writers (check_m50_rom.py), the `DISPLAY_IS_ON` definition, and each guard.

Camera test of `2ff2f5e16469`: no black screen, but Canon's menu came up blank, and only the parts Canon repainted on a key press appeared.
- The UI log shows the display going off and back on about 200 ms after the menu opens (`display 0` → `1` with `lv 0`, guimode 3).
- The deferred redraw then ran `_redraw_do`. With no Live View dialog to redraw it falls back to `clrscr()`, which also clears the shared RGBA layer: Canon's freshly drawn menu.
- `b9570afb60fb`: the deferred redraw is done only back in XPan's Live View with Canon idle (`xpan_photo_ui_active()`, `GUISTATE_IDLE`, no ML menu); anywhere else it is dropped, since Canon has drawn its own screen when it turns the display on. Build check added.

Test: playback, Canon's menu and back, many times in the Canon layout and in XPan's views, with Debug info ON. Check that XPan's HUD and Canon's screen come back each time. XM50.LOG's UI lines should show `display 0` only during the switches.

### Video mode (2026-11-11, `2dc4e348c1b7`, not camera-tested)

Camera report: in the dial's video position the XPan view still showed, though the video is not cropped. The LUT was on the video too, which the user wants to keep.

- **Cause.** `is_movie_mode()` was `shooting_mode == SHOOTMODE_MOVIE` (0x14). On the M50, `shooting_mode` (PROP_SHOOTING_MODE_2) is the movie exposure mode in video, so it never matched. The mode log (`MODE:` lines in XM50.LOG, Debug info only) showed:

  | Dial | `shooting_mode` | `PROP_SHOOTING_MODE` | `lv_movie_select` |
  |---|---|---|---|
  | Photo M | 3 | 3 | 1 |
  | Video, manual | 3 | 0x14 | 2 |
  | Video, auto | 0 | 0x14 | 2 |

- **Fix.**
  - On the M50, `is_movie_mode()` is `shooting_mode_custom == SHOOTMODE_MOVIE` (PROP_SHOOTING_MODE). Everything photo-only (XPan view and HUD, 65:24 band, film grain, photo LUT hook, 3:2 message, capture format) is then off in video, as on the EOS M.
  - `m50_pasm()` is false in video, so the grain filter writer never touches Canon's video filters.
- **The LUT on video.** It came from the Live LUT library: Canon's style tables graded, used by Live View and so by the movie. In video it now follows XPan's movie ON/OFF (the shared menu's movie row, `xpan.movie`, on by default), Custom LUT and the Cube file, whatever Live LUT preview says (a photo option, hidden in video).
  - Neutral stays forced while the movie LUT is on, as the LUTs expect.
  - The L+JPEG format forcing is photo only.
- **Build checks**: the M50 `is_movie_mode`, `m50_pasm`, forced settings and the library rule in video; the older checks updated to these rules.

Test: video manual and auto. There should be no XPan view and the LUT should be on the movie, with XPan's menu row toggling it. Back to M, XPan's view returns.

### XPan only in P, Tv, Av and M (2026-11-11, `456c2afc654b`, not camera-tested)

Camera report: XPan stayed on in the fully automatic positions (scene modes, creative filters...).

- **Cause.** `xpan_auto_mode()` (cropmarks.c) excluded only green Auto and A+. That is the EOS M rule, where XPan switches every other mode to M (`keep_manual_mode`, xpan-hud-controls.c).
- **Also found.** On the M50, `keep_manual_mode` was not switched off: its `CONFIG_M50` branch held a stray copy of the key log's `#define KEY_LOG_SIZE`, and the `return` was only for the 6D. It had no visible effect, because ML's `prop_request_change` is off on the M50.
- **Fix.**
  - On the M50, `xpan_auto_mode()` is true outside video unless both Canon mode values (PROP_SHOOTING_MODE_2 and PROP_SHOOTING_MODE, equal in photo M) are P, Tv, Av or M.
  - `XPAN_ACTIVE`, and with it every photo feature (view, HUD, 65:24 band, LUT hook, Live LUT, grain, forced Neutral / L, which are put back), is off in every other dial position.
  - Video is untouched: the movie LUT keeps its own switch.
  - `keep_manual_mode` returns on the M50 as on the 6D (physical dial).
  - `m50_pasm()` uses the same two values.
  - XPan's menu row warns "XPan works in P, Tv, Av and M (and its LUT in video)".
  - Build checks added.

Test: SCN, creative filters, A+: no XPan view or HUD, Canon's screen as normal. P, Tv, Av, M: XPan back. Video: no view, LUT on.

### ML menu timeout (2026-11-11, `f83e397e5bbb`, not camera-tested)

Camera report: ML's menu closed by itself after a few seconds.

- **The UI log (Debug info ON), three opens in Live View.**
  - The GUI mode goes 99 (0x63, `GUIMODE_ML_MENU` in Live View) → 0 exactly 6.0 s after each open (20.861 → 26.880, 28.567 → 34.562, 36.020 → 42.033), with `display 1` throughout.
  - ML then closes its menu (`menu_ensure_canon_dialog`: Canon's dialog changed).
  - So Canon drops the dialog ML's menu sits on. It is not the display-off flag (`DISPLAY_IS_ON`, README "Display on").
- **Fix.** `CONFIG_MENU_TIMEOUT_FIX` for the M50 (internals.h), ML's own workaround on the EOS M, 6D, 70D, 700D, 100D and 1300D: menu.c re-requests `GUIMODE_ML_MENU` every 2 s in Live View, with the same `SetGUIRequestMode` call `piggyback_canon_menu` opens the menu with. Build check added.
- **First on the M50:** the call is ML's standard one, but this is its first use as a repeated refresh on this body.

Camera test of `f83e397e5bbb`: the menu stays open, but every 2 s pieces of Canon icons show over it.
- Canon's log (LOG017, "Save Canon log" with ML's menu open) shows each `SetGUIRequestMode(99)` restarting Canon's Live View setting mode (`IDLEHandler START_LV_SETTING_MODE[0x100000b5]`), and with it the LV Picture Style app: `StopLiveViewPictureStyleApp` / `StartLiveViewPictureStyleApp`, `CreateDialogBox id:67`, about 6 ms after the request.
- On the M50 Canon and ML share one RGBA layer, so its icons land on ML's menu.
- No 6000 ms timer was found in the GUI code around it; ML's own comment on the fix says the MPU drops the dialog.

`96a036ae8395`: under `FEATURE_VRAM_RGBA` the refresh starts ML's redraw flood first (`start_redraw_flood`), as `piggyback_canon_menu` does when it opens the menu on the same mode. That is five full menu redraws about 20 ms apart, then a push. Build check added.

Test: open ML's menu in Live View and leave it for 30 s or more. It should stay open with no Canon icons; at most a brief flash every 2 s. Then close it normally, and open and close Canon's menu once. Keep Debug info ON.

Camera test of `96a036ae8395`: OK, no icons. Committed as `fbcd57e8d`.

### Layout changes without Canon's menu (2026-11-11, `8ebaedbfdf40`, not camera-tested)

On the EOS M a layout change is instant. On the M50 every change of the 65:24 band (power-on, layout, 65:24 / 3:2) opened and closed Canon's menu (the forced Live View restart, `46a84ccbd3f2`), because the band moved only on Canon's transition call.

- **Why the restart existed.** It was built on the theory that moving the picture on a normal frame breaks Canon's next Live View restart (README "Canon's main menu breaks Live View: the cause"). Later Canon logs disproved it. The hang came with the band never moved (README "Menu hang without the band"), and its cause was ML's patch stall at start (README "Menu hang: Canon's own log"), fixed in `d31841da9ff2`. The Err 70 of `332341d80a0c` came with its hang snapshot and 8 MMU pages. `8f344d055b96` moved the band on normal frames and passed its layout tests. Its menu hangs were the stall.
- **ROM (no change to Canon).** Canon's setters are called only from `E0104192` (`localSetOnlyParamIMGRepVram`), panel `E010422E`, EVF `E010424A`. a2 is the output's image-mute mask from Canon's display state (+0x3C panel, +0x40 EVF), set and cleared per mute class by Canon's mute handlers (`E010360A`, `E01036A2`, `E010380E`; "Not Space Setting MuteClassID"). Canon reprograms the image layer from the block at every frame (`E0154B64` / `E01550F8`), so a new geometry at a frame boundary is what Canon's own frames do.
- **Change.** The XPan task (every 50 ms, after ML's start) compares each output's latched band with the settings. While Canon shows plain Live View and is idle (`lv`, not magnified, no playback / review, display on, `GUISTATE_IDLE`, no half-press, lens idle, not recording) it sets a request flag. Canon's next setter call applies it (`band_decide`, as at a transition), so the geometry still changes only inside Canon's call, between two frames. The forced restart, its menu row and config entry are removed: XPan makes no MENU presses and no GUI mode requests. Diagnostics > "Live View band" shows each output's band and the frame / transition counts.
- **Also:** the Canon layout (INFO) and video now show Canon's 3:2 picture at once; before, the band stayed until Canon's next restart.
- **Checks:** `build_xpan.sh` fails if the geometry changes outside the setter call, if the request runs without those conditions or blocks, or if MENU presses or GUI mode requests come back.

### Faster start (2026-11-11, `8ebaedbfdf40`, not camera-tested)

The XPan task held its start steps on fixed delays from its own start (~1.4 s): learned settings, forced Canon settings, Canon's grain filter and the 65:24 JPEG crop at +3 s, the LUT list at +6 s (so the Live LUT came at ~7.5 s), and the forced restart after that. They now wait for ML's own start (`ml_started`, ~2.2 s: settings, fonts and menus loaded), as ML's features do. The crop patch's ROM page copy runs before `cli()` since `d31841da9ff2`, as for ML's boot hooks. Boot log mark 78 (Debug info) records each step: 1 settings, 2 LUT list, 3 LUT loaded, 4 style write, 5 format write. The next start log shows what still changes the screen. Build check: no start step on a fixed delay.

Test (one round, the band change first: it may hang, so watch the first menu / playback cycles):
1. Power on in P/Tv/Av/M with 65:24: the band and XPan's view should come without Canon's menu, the Live LUT within about a second.
2. Change the layout (Top / Split / Bottom) and 65:24 / 3:2 several times: instant, no menu.
3. INFO to the Canon layout and back; playback and back; Canon's menu and back, three times each; LCD / EVF.
4. A photo in each layout, then tap-to-focus in Bottom.
Keep Debug info ON and bring the card (XBOOT.LOG, XM50.LOG).

Camera test of `8ebaedbfdf40`: works very well. Start: settings at 2.16 s, LUT list 2.41 s, LUT loaded 3.48 s, band moved at 3.60 s (boot log mark 78 / 74), no menu cycle. Layout changes move the band at the next frame (LCD and EVF). One issue, below.

### Canon layout exit: HUD missing (2026-11-11, diagnostic `0aba2b6a960d`)

- **Symptom (user):** on the LCD, INFO from the Canon layout to the next XPan layout: most times XPan's HUD is missing (only the black bands around the picture), sometimes pieces of Canon's screen on top.
- **Timeline (boot log, LCD):** INFO at 12020 ms; band moved back at 12109 (next frame after XPan's task asked); XPan's exit step at 12191 (`canon_layout_state` 2 -> 0, unfreeze, `xpan_photo_hud_force_redraw()`, `redraw()`). XPan's own swallows INFO, so Canon gets no INFO press.
- **Why it showed only now:** before `8ebaedbfdf40`, leaving the Canon layout changed the band, so 1 s later the forced restart opened and closed Canon's menu and everything was redrawn. That covered whatever the exit left on screen.
- **Not known from the code:** on DIGIC 8 `redraw()` makes Canon repaint its Live View screen into the shared layer (`dialog_redraw(LiveViewApp_dialog)`, GuiMainTask) while XPan's HUD draws from the shoot task and ML's layer push runs from `redraw_task`. The push writes every pixel only when `zebra_should_run()`, else it keeps Canon's pixels under ML's transparent ones (`rgb_layer_partial`). With Debug info ON the HUD redraws at every LV info pass, so a HUD that stays missing means its draw or its push is blocked, not a one-off race. Which one is what the trace shows.
- **Trace (Debug info only, read only, ~1 KB):** for 3 s after each Canon layout exit, at each shoot-task step for 1.5 s then at each change: HUD draws, ML layer pushes (all pixels / partial), `zebra_should_run`, `get_global_draw`, `liveview_display_idle`, refresh frozen / needed, Canon front buffer, ML redraw stage, gui state, Canon's overlay and GUI modes, HUD row, band, display, HUD on, Canon layout. Written to XM50.LOG ("LX" lines) after the 3 s. Build check: read only, Debug info only.

Test: Debug info ON, LCD, 65:24. Cycle INFO through every layout 6-8 times, pausing ~4 s after leaving the Canon layout each time. Note which exits showed the fault (e.g. "2nd and 5th"), then bring the card.

Camera test of `0aba2b6a960d` (user): every exit from the Canon layout, the HUD never came back; black background, sometimes parts of Canon's screen. Trace (XM50.LOG, exits at 21770, 36750 ...): the whole 3 s, `zebra_should_run` 1, global draw 1, display idle 1, not frozen, HUD on, not clipped, HUD row 266, Canon's front buffer off; `need` goes 1 -> 0 every 50-250 ms, so the HUD draws and ML's layer is pushed in full (every pixel) all the time. The HUD is in ML's layer and pushed, yet not on screen. (The counters printed "u": ML's snprintf has no `%u`.) The Ximr words the log keeps (+0x00..+0xBC) were the same as at start, but ML's own RGBA layer record sits after them: RAM dump, context +0xD8 `MARV`, data `0x02B9E100` (= ML's push target), format `0x05040100`, 960x540, Canon's MARV `0x02D9851C`-0x1C, flags 0x100, alpha 0xFF.

Diagnostic `8f24dadbfa61`: snapshots at the Canon layout entry, the exit and the exit + 1.5 s, each with Ximr context words +0xC0..+0xFC (ML's layer record) and one HUD row (HUD row + 40) of ML's bitmap compared with Canon's RGBA layer (`rgb_layer_probe_row`, bmp.c: ML pixels not empty, layer pixels with alpha, layer pixels equal to ML's). Read only, Debug info only, build check. If the layer holds ML's pixels after the exit, the compositor is not showing that layer (its record changed); if not, the pixels are overwritten after the push.

Test: Debug info ON, LCD, 65:24: INFO into the Canon layout and out 4 times, ~4 s after each exit.

Camera test of `8f24dadbfa61` (user: all exits failed except the first, maybe the second, and the last). Snapshots, every exit: at exit + 1.5 s the HUD row is 720/720 in ML's bitmap, 720/720 in Canon's RGBA layer, layer == ML 720/720; Ximr's record of that layer (+0xD8: MARV, data `0x02B9E100`, `0x05040100`, 960x540, flags 0x100, alpha 0xFF) never changes. So ML's pixels are in the layer the compositor reads, and are still not on screen: the compositor's output is not what the display shows.

**Cause (ROM):** Canon's surface refresh `refreshVrmsSurface` (E0528540, its own message "Call refreshVrmsSurface But m_fRefreshDisplay is FALSE"; state at 0xFC08) does: if `m_fRefreshDisplay` (+0x7C); set the output (E0528258); `take_semaphore(winsys_sem, 0)` (E0528660); `XimrExe(0x9329C)` (E052866A) into the current OSD buffer (Ximr context +0x90: UYVY 736x480 + alpha, two buffers `0x41100100` / `0x41340400`); build the OSD info (E0527FFC) and `DISP_SetUpdateOSDVram` (E0537E54) so the display shows that buffer; switch to the other buffer. winsys_sem is given back by the display (`E0525FA0`, registered with E0538218 at E05260B2) once it has switched. Canon's window system calls it from its refresh-rect paths ("refresh x=%d y=%d w=%d h=%d", "refresh2 ..."), bracketed only by its performance markers (`E01494D6`, winsys_util.c "WinSys_Draw").
ML's push did only `XimrExe` and gave winsys_sem itself: it renders into whatever buffer the context names and never asks the display to show it. It shows only while that buffer is the one on screen. In the Canon layout Canon refreshes several times; after the exit the context can name the other buffer, and since Canon's screen then stays still, nothing shows ML's render again. The successful exits are the ones where the buffers happened to line up.

**Fix (`5761b9766d0c`, first use of Canon's refresh by ML, not camera-tested):** ML writes its pixels under winsys_sem as before, gives it, and calls Canon's `refreshVrmsSurface()` (stub E0528540), which takes winsys_sem itself, renders, shows the buffer and lets the display give the semaphore back, as for Canon's own GUI. No other change. Callers of the push: redraw_task, the menu redraw flood, XPan's Canon UI dim (GUI task) and Canon layout push (shoot task), none under bmp_lock, all only with the display on (the push returns first otherwise; Canon's function also returns at once when `m_fRefreshDisplay` is false). `check_m50_rom.py` checks the function, its semaphore take, XimrExe and DISP calls, the context literal and the callback registration; `build_xpan.sh` checks that the M50 push no longer calls XimrExe itself and gives winsys_sem right before the call. The exit trace stays (Debug info).

Test (on its own; it changes how every ML screen update reaches the display): power on, HUD as usual; INFO through all layouts and the Canon layout several times on the LCD and the EVF; ML's menu open/close; Canon's menu and playback and back; a photo. If the screen freezes or goes black, wait 10 s, then battery out.

**Camera test of `5761b9766d0c`: FAILED.** Err 70 at the first start; at the next start every layout was broken. Boot log of the Err 70 start: ML's start done at 2.13 s, then at 2.30 s Canon's GUI left idle (XPan dimmed for "Canon's UI"), PROP_LV_ACTION 1 at 2.36 s, Live View stopped, the display setter got 15 calls in that second and 0 after: Canon died within 0.2 s of ML's first pushes through `refreshVrmsSurface`. Its first part (output setup E0528258, one-time init E0528496 with flag +0x50, buffer bookkeeping +0x18..+0x24) runs before it takes winsys_sem, so it is not safe to call from a second task next to Canon's GUI task; also the display's callback gives winsys_sem once per Canon refresh, and ML's extra take/give pairs around it may not match Canon's count. Not proven which; no Canon log of that start.
Reverted (`63ad38560f4e` = source of `8f24dadbfa61` + this note + a build check: no `refreshVrmsSurface` stub or call). The card is back on `8f24dadbfa61` (tested: works, HUD missing after the Canon layout).

Open (offline first): which task(s) Canon calls `refreshVrmsSurface` from (winsys refresh-rect paths E054D3C4 / E054DDFA); whether a refresh can be asked of Canon's own task without Canon drawing its Live View screen; and when Canon's output buffer (context +0x90) differs from the one on display. ML's own `src/compositor.c` (other DIGIC 8 bodies) notes that refreshVrmsSurface is "not needed as we do XimrExe() in bmp.c".

**ROM study (2026-11-12, offline, no build).**
- *Who refreshes.* Only `refreshVrmsSurface` (E0528540) sets Ximr's output (E0528258; its tail call E011BF50 copies Canon's current buffer record into the context) and asks the display to show a buffer (E0537E54 is called only at E0528686). It runs only through the wrapper E05286C4 when the wrapper's 6th argument is set. The callers are Canon's window-system refresh paths (E054D466, E054DAFE, E054DD4E, E054DE3C) and a BMP viewer app (E01FEBC0..., DlgMnBmpDisplay, not in Live View). E054DE3C is skipped while word +0x20 of 0xFCE4 is set. Each call clears `m_fRefreshDisplay` (+0x7C) at its end; when that flag is false the call only prints its message. It never gives winsys_sem itself.
- *Order inside it.* It sets the output to Canon's current buffer (+0x20), takes winsys_sem and runs XimrExe into that buffer. It then builds PDISPOSDINFO from the same buffer (E0527FFC: its +4 / +8 / +0x10 / +0x14), calls `DISP_SetUpdateOSDVram`, and moves +0x20 to the other buffer. So after a Canon refresh the context names the buffer just sent to the display, and ML's bare XimrExe renders into that one. A mismatch can only happen if the display did not switch.
- *The display side.* `DISP_SetUpdateOSDVram` posts two events to DispVCtrl: 19 (`DV_NotifyOSDVramAddr`) and 15 (`DV_SetUpdateOSDVram`). DispVCtrl's struct is at `*(0x8E08)`; its state object `DispVramCtrlState` is at +8 (table E0B4CBBC: 22 events x 2 states).
  - Event 19 (both states) and event 15 in state 0 (E0103C64) only store the info (+0x1D0, 40 bytes; +0x1CC is the output) and give winsys_sem back at once. The panel is **not** switched.
  - Event 15 in state 1 (E0104038, then localSetUpdateOSDVram E0103EA8) switches each output that is on (+0x1C8: 1 panel, 0x10 EVF, 0x1000 HDMI) through E01388EE (panel, with the OSD mute +0x30). It gives winsys_sem from the output's vsync callback (E0103CF2 "PanelVsyncCallBack2").
  - The stored info is applied again when an output turns on (E0105F64, from E0106176).
- *Two explanations fit the camera data, and the ROM cannot pick one:*
  - (A) After the Canon layout, Canon's later refreshes reach DispVCtrl in state 0, or with the panel off in +0x1C8. They are stored, not shown. The panel keeps an older buffer (stale Canon pieces) while Canon and ML go on rendering into the others.
  - (B) Canon makes no refresh after the exit (`m_fRefreshDisplay` false, or the window-system refresh suspended), and the panel's last switch was to a buffer ML no longer renders into.
- *What would tell them apart:* a read-only, Debug-info-only log taken with the exit snapshots, recording:
  - Ximr's LCD output address (context +0x94);
  - Canon's 0xFC08 +0x18 / +0x1C / +0x20 (each record's +4) and +0x7C;
  - DispVCtrl's state (`*(*(0x8E08)+8)+8`), +0x1C8, +0x1CC and the first two words of the stored info (+0x1D0, +0x1D4).
  If +0x1D0 (the buffer Canon last asked for) equals the context output while the screen is stale, it is (A). If they differ, it is (B).
- *A fix stays on Canon's own path.* For (B) the answer is a Canon refresh in Canon's own task (the GUI's own redraw), not ML calling Canon's refresh (`5761b9766d0c`). For (A) it is whatever puts DispVCtrl back in state 1, which Canon does itself when Live View or the panel comes back.

Diagnostic `b0f2c6ac5e0a` (= `63ad38560f4e` + the values above): each Canon layout snapshot (entry, exit, exit + 1.5 s) adds two lines to its LXS entry in XM50.LOG:
- "OSD out lcd / evf": Ximr's output addresses;
- "canon rec / cur / refresh / bufs": 0xFC08 +0x18, +0x1C, +0x20, +0x7C and each record's address;
- "OSD disp state / on / out / sent / mute": DispVCtrl's state, +0x1C8, +0x1CC, +0x1D0 / +0x1D4, +0x30;
- "winsys skip": 0xFCE4 +0x20.
Read only, each word through a RAM check (`lx_word`, 0xDEAD0000 if a pointer is bad); Debug info only; about 0.7 KB. The build checks it reads only, calls nothing of Canon's and runs only from the snapshot. No change to how ML or Canon draw.

Test: Debug info ON, LCD, 65:24. INFO into the Canon layout and out 4 times, ~4 s after each exit; note which exits lost the HUD. Then bring the card.

**Camera test of `b0f2c6ac5e0a`** (user): exits 1-3 lost the HUD, exit 4 was right. Leaving ML's menu after boot also lost it, so this is not only the Canon layout.
- DispVCtrl's state was 0 in every snapshot, good and bad, so (A) is ruled out. State 0 is the normal state; the ROM study's table read had the two states the wrong way round.
- In the record lines, A = record 41202d00 / buffer 41100100, B = record 41443000 / buffer 41340400.
- A completed Canon refresh always leaves: Ximr output = the buffer sent = not Canon's next buffer. Every good snapshot shows this, for example entry 43261: output B, next A, sent B.
- Every bad exit + 1.5 s shows: Ximr output B, Canon's next buffer **B**, sent **A**, `m_fRefreshDisplay` 0. The good exit + 1.5 s shows output B, next A, sent B.
- So on the bad exits Canon's next refresh had begun. Its first part had set Ximr's output to Canon's next buffer B (E0528258), but it had not rendered, sent or switched. The screen stays on A, which holds Canon's old screen; ML renders into B, which is not shown.
- The only blocking step between those two points is `take_semaphore(winsys_sem, 0)` at E0528660. The refresh finished by the next Canon layout entry, when XPan freezes ML's pushes.

Diagnostic `3b386d43f38f` (= `b0f2c6ac5e0a` + this): at exit + 1.5 s, if the Ximr output differs from the buffer last sent, XPan asks for Canon's log once per start, through the menu request's tested path. That path writes the task states into Canon's log (`m50_log_tasks`: each task's state, wait object and stack return addresses), then calls `dumpf`. The log also gets a new "XPW" line: winsys_sem's first four words (count, waiters), read only. Boot mark 81 = 4. Debug info only; build checks for once-only, Debug-only and the request path.

Test: as before (Debug info ON, LCD, INFO into the Canon layout and out 4 times), then wait 10 s and bring the card. A new LOGnnn.LOG should be on the card.

**Camera test of `3b386d43f38f`** (user): exits 1 and 2 bad, 3 and 4 right. Canon's log `LOG022.LOG` was saved at exit 1 + 1.5 s.
- **Task states.** GuiMainTask was waiting on its message queue (w4), and no task was waiting on winsys_sem. So Canon's refresh is not stuck, and the "next refresh waiting at take_semaphore" reading is wrong.
- **Snapshots.** Exit 2 was already wrong at the exit (output B, next B, sent A, refresh pending), and wrong again at + 1.5 s with the letters swapped (output A, next A, sent B). A single refresh always leaves output = sent, so each refresh there produced the mismatch again.
- **Only code path that fits.** Two refreshVrmsSurface calls overlap:
  - the first sets the output to Canon's next buffer X and waits for winsys_sem;
  - a second, from another task, also sets the output to X and waits;
  - the first renders X, sends X and moves Canon's next buffer to Y;
  - the second renders X again but sends Y.
  The screen shows Y (old content); ML renders into X.
- **Why ML matters.** The window for this is the wait on winsys_sem. ML's layer push holds it while converting 720x480 pixels, which widens that wait; Canon alone rarely overlaps.
- **Who calls.** Canon's log at exit 1 shows Canon handling INFO itself ("SwINFO_Button", "AFFrame PRESS_BUTTON_SOMETHING", "MainGuide_Clear") and two full window refreshes 70 ms apart ("[WINSYS] refresh x=120 y=30 w=720 h=480"; Canon's log time = XPan's + about 2116 ms).
  - ML's `redraw()` posts `MLEV_REDRAW`, so its `dialog_redraw` runs in GuiMainTask.
  - The whole-screen refresh E054DAD0 has hundreds of callers across the firmware, so the ROM alone cannot say which other task makes the second call.
- **No refreshVrmsSurface hook.** Its page (E052) is not hooked yet; an 8th MMU page is what gave Err 70 at boot on 2026-11-10 (memory rule), and every hooked page is in use by a working feature.

Diagnostic `8a8dce8ae6f3` (= `3b386d43f38f` + an OSD send probe):
- **The hook.** It sits on `DV_SetUpdateOSDVram` E00EF372, the end of each refreshVrmsSurface, in the calling task. E00EF372 is in the page already hooked for the colour table (E00E), so the MMU pages stay at 7 and ML grows by under 1 KB.
- **What it records, per send:** the task, the buffer sent (OSD info word 0) and the buffer Ximr rendered (context +0x94).
- **XM50.LOG lines** (Debug info):
  - "OSD sends from <task>: n, rendered != sent m" for each task that sends;
  - for each bad send, the send before it and the bad one ("OSD send before/BAD: <task> sent X rendered Y").
- **Checks.** The trampoline replays 8 bytes (push, mov, movs, the bl through r3) and resumes at E00EF37C. `check_m50_rom.py` checks the bytes, the callee E055B094 and the tail call E0537EC8. Build checks: read only, Canon's call made exactly once, hook list.

Test (on its own, a new hook in Canon's display path; if the screen freezes or goes black, wait 10 s, then battery out): Debug info ON, LCD, INFO into the Canon layout and out 4 times; then ML's menu open and closed once; wait 10 s; bring the card.

**Camera test of `8a8dce8ae6f3`** (user): no hang; only the 4th exit right. The probe confirms the overlap:
- **Two senders only:** CtrlSrv (9 sends, 6 bad) and GuiMainTask (26 sends, 1 bad).
- **Every bad send** comes 4 to 19 ms after a send from the other task, at a Canon layout entry or exit. Example at exit: CtrlSrv sent B (rendered B), then GuiMainTask sent A with B rendered. The later refresh rendered the buffer the earlier one had just shown and sent the other one.
- **Who they are.** CtrlSrv is Canon's own refresh for the INFO change. GuiMainTask's is ML's `redraw()`, which XPan posts at each layout change (`dialog_redraw` → winsys refresh → refreshVrmsSurface). The two run at the same moment because both react to the same INFO press.
- **Next (proposed fix).** Repair on the event, with no wait: when the probe sees a send whose buffer differs from the rendered one, XPan's task calls `redraw()` once more. That runs one more full refresh in GuiMainTask, which leaves output = sent. No new hook or MMU page; the probe stays, without its log in normal builds.

Fix build `5968e6e94392`:
- **Probe:** a bad send right after a send from another task (the overlap's signature) adds 1 to a count. The probe still only reads and counts; it never redraws in Canon's display path.
- **XPan's task** (`xpan_m50_canon_ui_step`): when the count changes, it calls `redraw()` once, only in XPan's Live View with the display on, Canon idle, no menu and not in the Canon layout. A count from inside the Canon layout waits for the exit. Debug info marks each repair (boot mark 82).
- **Bounded:** a repair redraw can collide again only with another CtrlSrv refresh, which is rare, and each collision costs one more redraw. A steady mismatch from a single task (no overlap) is never counted.
- **Build checks:** no redraw in the probe; the count is written only there and only on the signature; the repair conditions; one redraw per count.
- Memory unchanged (margin 205184 bytes, MMU pages 7).

Test: Debug info ON, LCD, INFO into the Canon layout and out 6 times; ML's menu open and closed twice; the HUD should come back each time.

## Keep ML after a Canon format: ROM study (2026-11-13, offline, nothing built)

ML's generic `CONFIG_RESTORE_AFTER_FORMAT` (src/debug.c): while Canon's Format dialog is open, copy `autoexec.bin`, `ML/` (6.4 MB on the card now) and root `.FIR` files to RAM; after Canon has formatted, write them back, write the boot flag (src/bootflags.c), restart. A button in the dialog toggles keep / remove. The EOS M and the R enable it; the M50 never did. Found in ROM0 / RAM4 (M50 1.1.0):

| Piece | M50 | How it was found / checked |
| --- | --- | --- |
| `DIALOG_MnCardFormatBegin` | **0x1AE94** | `StartMnCardFormatBeginApp` (E0686976) stores `CreateDialogBox`'s (E054F1A4) result at [0x1AE90+4]; `StopMnCardFormatBeginApp` (E0686524) deletes the dialog and writes 0 there. |
| `DIALOG_MnCardFormatExecute` | **0x20C14** | `StartMnCardFormatexcuteApp` (E06BFD3A, Canon's typo) stores it at [0x20C10+4]; `StopMnCardFormatexcuteApp` (E06BF712) clears it. |
| `dialog_set_property_str` | **E05408B8** | Same pattern as the 750D port: `UpdateStroboZoomGuidance` (E08F2D94) calls it with (dialog, 5, text on the stack). Body: strlen(text)+1, then Canon's generic dialog property setter E054FE28 with type 0xB (string) and the index. `dialog_redraw` (E054F3CA) is already a stub. |
| `sd_device` | **index 0** | RAM dump: `sd_device` (0x115B8) = {0xBC64FC, 0xE12781A4}. Entry 0 is the device (read E01232BF, write E0123649, Thumb); entry 1 points into ROM data. **ML's bootflag code uses entry 1 for the M50** (it falls to the generic `#else`): it would call into data. The M50 must join the R / 200D / 6D2 / 750D case (`sd_device[0]`). |
| read / write signature | (dev, ?, ?, count) | Both take the device struct (they read its drive letter at +0xC4 and +0x50) and three more arguments, as the R's (dev, buf, block, count); the order of buf and block is not provable from the prologue. |
| `fsuDecodePartitionTable` | inlined | The stub is a placeholder (0xDF00D365): calling it crashes. The R's C replacement (platform/R.180/function_overrides.c) decodes the MBR entry; the M50 can use it. The exFAT branch then matches what xpan_update_card.ps1 writes. |

**Not offline:**
* **Which string line** of Canon's dialog ML rewrites (`FORMAT_STR_LOC`, and index 4 in `HijackFormatDialogBox`): the R port marks its value "WRONG, taken from 5D3". ML's `HijackDialogBox` test writes each index's number into the dialog: one look on the camera.
* **The toggle button** (`FORMAT_BTN`): one Canon does not use in its Format dialog (the R uses INFO; TRASH may be Canon's low-level-format toggle).
* **Memory:** ML asks `fio_malloc` for 15 MB (`TMP_MAX_BUF_SIZE`); on the M50 that is Canon's shooting pool (`shoot_malloc`), which crashed the camera during photos (comment at FILM_TEX). In Canon's menu no photo is running, but whether Canon's format uses that pool is unknown. Smaller buffer (the ML folder's size) and a read-only test of the allocation first.
* **Restart:** ML writes `PROP_REBOOT` with `prop_request_change`, which the M50's whitelist (empty) refuses. Either add it after checking, or skip the restart and ask for an off/on.

**Test order (each step its own round, a spare card, nothing written to the card before step 3):**
1. Read only: log the two dialog pointers while the user opens and leaves Canon's Format dialog; read sector 0 through `sd_device[0]` with (buf, block) and check 0x55AA and the partition type; run `HijackDialogBox` once to see the string indices.
2. RAM copy only: copy the ML files to RAM and back into RAM, compare signatures; no format.
3. The real thing on a spare card: format, restore, boot flag, off/on.

**Read/write argument order, settled offline:** in the read routine E01232BE the loop advances its 2nd argument by `block size x blocks done` (`mla sl, [dev+0x30], r4, sl`) and its 3rd by the blocks done (`add r6, r4`), and counts the 4th down: **read_block(dev, buf, sector, count)**, as the R. The write routine E0123648 has the same prologue.

### Step 1 build `a753e986ff2e` (2026-11-13, not camera-tested): read-only probe

Debug info only (XPan > Diagnostics). Nothing is written to the card except XPan's own log; no Canon setting changes.
* **Dialog watch:** every change of Canon's two Format dialog pointers goes to XM50.LOG ("format dialog: begin X, execute Y (menu M)").
* **Card boot sector** (action): XPan's task reads sector 0 through `sd_device[0]` (refused unless it is a RAM struct whose read routine is a ROM Thumb address) into 12 KB of Canon's uncached DMA heap memory, then the partition's boot sector (exFAT: 24 sectors, main and backup; FAT32/16: 1). Help and log: MBR signature, partition type and start, the read results, whether the file system and both boot flags are there.
* **Format dialog lines** (action, arms once): the next time Canon's Format dialog opens, XPan sends an ML event (`MLEV_XPAN_FORMAT_LINES`, handled in Canon's GUI task in `handle_tricky_canon_calls`) that writes each line's number (0..39) with `dialog_set_property_str` (stub E05408B8, added) and redraws; Canon refuses lines it does not have (lookup E054FB48, error 0xD), and the log lists the lines that took text. **Do not format** during this test.
* Texts on the heap (allocated with Debug info ON): ML's fixed memory unchanged (margin 201344 bytes).
* Build checks: no `write_block` call in XPan, `sd_device[0]` only, the dialog addresses, Debug info only, dialog text only from the GUI task; `check_m50_rom.py`: the four Start/Stop literals and first bytes, and E05408B8's body.

Test: Debug info ON. 1. Diagnostics > Card boot sector: read the help (expect "MBR ok, type 7, ... file system ok, boot flags present" on the 250 GB card). 2. Diagnostics > Format dialog lines, then Canon's menu > Format card: photograph the dialog, press MENU / cancel (no format). 3. Leave Canon's menu; the card's XM50.LOG has the lines.

**Camera test of `a753e986ff2e` (2026-11-13): all three work, no hang.**
* **Card boot sector:** `sd_device[0]` 0xBC64FC, read routine E01232BF; sector 0: rc 1, signature AA55, partition type 7 (exFAT), start 2048; boot region (24 sectors): rc 24, "EXFAT", both boot flags present. The read routine returns the number of sectors read (1, 24), which is what ML's bootflag code checks (`rc != 1`).
* **Dialog watch:** begin 0x92C380 while Canon's Format dialog is open, 0 when it closes (three opens); execute stayed 0 (no format was run).
* **Lines that took text:** 3 4 5 8 9 10 11 15 16. From the photo: 3 the title (above the yellow line), 4 the main message (centred), 5 the line under it, the capacity bar, 8 and the two 9s the capacity texts, **15 and 16 the INFO "Low level format" check box and its label**, 10 Cancel, 11 OK.
* So for ML: `FORMAT_STR_LOC 4` (the centred message, as on the EOS M; also the index `HijackFormatDialogBox` uses). **INFO is Canon's own low-level-format toggle in this dialog**, so the R's `FORMAT_BTN BGMT_INFO` would take it from Canon: the M50 uses DOWN, as the EOS M (the dialog's buttons are left/right, SET, INFO, MENU).
* Still open: the 15 MB `fio_malloc` (step 2: RAM copy only, no format) and the restart (PROP_REBOOT is not in the M50's write whitelist).

### Step 2 build `c953fbf21b35` (2026-11-13, not camera-tested): ML's RAM copy, checked

XPan > Diagnostics (Debug info) > **Format RAM copy** (arms once): the next time Canon's Format dialog is open (begin dialog set, execute 0, Canon's menu), XPan's task does what ML's `TmpMem_Init` and `CopyMLFilesToRAM_BeforeFormat` do: `fio_malloc(15000000)` (ML's `TMP_MAX_BUF_SIZE`; on the M50 this size goes to `MEM_TEMPORARY`, Canon's shooting pool) and a 200-entry file list, then the same files in the same order (AUTOEXEC.BIN, ML/FONTS, ML/MODULES, ML/SETTINGS one level down, root .FIR, all of ML/ two levels down, root .LOG). Then each file is read again from the card in 32 KB pieces and compared with its RAM copy, and both buffers are freed. Nothing is written to the card except XPan's log, and nothing is formatted.
* Log / help: the buffer's address and allocation time; files, MB, **files skipped for lack of room** (ML drops them silently), files that differ or cannot be read, copy and total time.
* Step 1's probe (sector read, dialog lines) is removed: its results are above, and its code pushed ML's reserve one 16 KB step (margin 184960); without it the margin is 201344 bytes again. The dialog watch stays.
* Build checks: the RAM test opens no file for writing, runs only with Canon's Format dialog open and not formatting, uses ML's buffer size; no `sd_device[1]` in XPan.

Test: Debug info ON, Diagnostics > Format RAM copy, then Canon's menu > Format card, wait about 10 s on the dialog, cancel (**do not format**). Then take a few photos (Canon's shooting pool was used: any Err 70 or hang matters), and read the help line or XM50.LOG.

**Camera test of `c953fbf21b35` (2026-11-13): the RAM copy works.** Four runs with Canon's Format dialog open (36.6, 53.7, 71.3, 100.6 s): `fio_malloc(15000000)` = 0x46110084 in 3 ms every time; 52 files, 3.2-3.3 MB, **0 skipped, 0 differ, 0 unreadable**; copy about 125 ms, with the check about 1.4 s. Photos afterwards (138-164 s) normal: no Err 70, no hang.
* The card's ML files really are 3.4 MB (`du` says 6.9 MB because of the exFAT clusters), so ML's 15 MB buffer has plenty of room.
* The user saw Canon's menu black the first time. In the log that menu visit is at 28.3-32.0 s (guimode 3), **before** the first Format dialog and before any RAM copy (36.6 s): not the test. It looks like the earlier "Canon's menu came up blank" symptom (README "Display on"); open.
* Left for step 3: the format itself, the boot flag write (ML's bootflags.c with `sd_device[0]` and the R's partition decode), DOWN as the toggle, line 4 for the messages, and the restart (PROP_REBOOT not in the M50's write whitelist).

### Step 3 build `92589fc035bf` (2026-11-13, not camera-tested): ML's keep-after-format on the M50

ML's own `CONFIG_RESTORE_AFTER_FORMAT` (src/debug.c), now defined in the M50's features.h, with:
* **consts.h:** `DIALOG_MnCardFormatBegin` 0x1AE94, `DIALOG_MnCardFormatExecute` 0x20C14, `FORMAT_STR_LOC` 4, `FORMAT_BTN` DOWN ("[DOWN]"; INFO is Canon's low-level toggle in this dialog).
* **src/bootflags.c:** the M50 joins the R / 200D / 6D2 / 750D case, `sd_device[0]`. This also fixes ML's Bootflag menu item (`FEATURE_BOOTFLAG_MENU`, already on in the M50 build), which would have called into ROM data through `sd_device[1]`.
* **Partition decode:** the placeholder stub (0xDF00D365) is gone; `platform/M50.110/function_overrides.c` has `fsuDecodePartitionTable` in C, reading the MBR entry byte by byte (offset 446 is not 4-byte aligned).
* **src/debug.c, M50 only:** the "keep / remove ML" text goes into Canon's Format dialog through Canon's own pointer (`MEM(DIALOG_MnCardFormatBegin)`), not `gui_task_list` (layout unchecked on this body), still from Canon's GUI task. Progress ("Reading...", "Restoring...", "Writing bootflags...") goes to ML's notify box instead of the current Canon dialog (ML's debug task; after the format the current dialog is Canon's menu). No reboot request (PROP_REBOOT is not whitelisted): "Magic Lantern restored. Turn the camera off and on."
* `gui_uilock` does nothing on the M50 (PROP_ICU_UILOCK not whitelisted, refused at once), so Canon's buttons are not locked during the copy; it takes about 0.13 s (step 2).
* Step 2's RAM test code is removed (the dialog watch stays, Debug info). ML's memory unchanged (margin 201344 bytes).
* Build checks: the tested dialog values, `sd_device[0]`, no placeholder stub, the C decode, dialog text only in Canon's Format dialog, no reboot / UI lock whitelisted.

Test (a card whose photos you do not need; the format erases it): Canon's menu > Format card. The dialog should say "Format card, keep ML [DOWN]" (DOWN: "remove ML"); OK. After Canon's format, the notify boxes, then "Turn the camera off and on". Off/on: ML should start. Then on the PC the card script's boot check (or EOScard) shows whether the boot flags are right.

**Camera test of `92589fc035bf` (2026-11-13): it works.** The card was formatted in Canon's menu with "keep ML", ML restored itself and started after an off/on; on the PC the card script found the exFAT boot markers and boot checksum right (written by ML's `bootflag_write_bootblock` through `sd_device[0]`). One fault: opening the Format dialog showed a black screen with ML's notify box on top ("Reading ..." while copying), black after it went away until a button press. ML's notify box erases its area through ML's layer, which takes Canon's dialog with it until Canon redraws.

**Build `57687f1f491d`** (not camera-tested): on the M50 no notify box while Canon's Format dialog is open (the dialog itself says "keep / remove ML"); error messages and the messages after the format stay. Build check for it.

**Found on the PC after that test: five root "ML" directories.** `dir P:\` lists `ML` five times (created within one second, 19:34:27-28 camera time); Windows shows only the first (cropmks, data, doc, FONTS, LOGS, MODULES, scripts, SETTINGS, XPAN, README). exFAT never allows two entries with one name: the file system is inconsistent. DCIM was empty (freshly formatted), so no photos were at risk. Root `log000.log` is Canon's own log (XPan's automatic dumpf when Live View does not come back after Canon's menu, 19:35: the black Format screen).
* **Likely cause (not proven):** ML's restore creates each file with `FIO_CreateFile` -> `_FIO_CreateFileEx` -> `_FIO_CreateDir_recursive`, which asks `is_dir()` for every parent; `is_dir` is `FIO_FindFirstEx(path)` succeeding. On exFAT an empty directory has no "." / ".." entries, and ML passes the path without a trailing "/" ("B:/ML"), so on the M50 `is_dir` may answer "missing" for a directory that exists, and Canon's `_FIO_CreateDirectory` then adds a second entry with the same name instead of refusing. On the FAT32 cards of the other bodies that use this code, "." and ".." make the check work.
* **Feature OFF again** (`// #define CONFIG_RESTORE_AFTER_FORMAT`, build `cc6fbf76e525`, on the card): a Canon format now removes ML, as before.
* Next, read only: log what `FIO_FindFirstEx` answers on the M50 for an existing directory with and without the trailing "/", for an empty one, and what `_FIO_CreateDirectory` returns for a name that exists; then a fix in `_FIO_CreateDir_recursive` (an existence check that works on exFAT), and the format test again on a spare card.
* The card: repair it by formatting it on the PC or in the camera (ML is removed with this build), then the card script and EOScard for the boot flags.

**Folder probe build `d56760d94889`** (2026-11-13, not camera-tested): XPan > Diagnostics (Debug info) > **Folder check probe**. Logs to XM50.LOG what `FIO_FindFirstEx` answers for `B:/ML`, `B:/ML/`, `B:/ML/LOGS`, `B:/ML/LOGS/` (error, or the first entry), `is_dir` with and without "/", Canon's size call on a folder, and how many root entries are named "ML" (5 expected on the damaged card). Then it creates one new empty folder `ML/LOGS/XEnnnnn` (a unique name, checked absent first: it cannot become a duplicate) and asks the same questions about it, plus whether the parent's listing finds it. Keep-ML-after-format stays OFF (build check). Run it once **before** reformatting the card: the duplicates are part of what it measures.

**Camera test of `d56760d94889` (2026-11-13): the folder checks work in normal use.** `FindFirstEx` succeeds for `B:/ML`, `B:/ML/`, `B:/ML/LOGS`, `B:/ML/LOGS/` (mode 0x10 / 0x20 entries); `is_dir` is 1 with and without "/"; a new empty folder (`CreateDirectory` = 0) is found too (`FindFirstEx` ok with an empty first entry, `is_dir` 1 / 1, listed once); Canon's size call returns 262144 for any folder (one cluster), so it cannot tell folders apart. The root has **5 entries named ML** (the camera sees the duplicates too). So neither the empty exFAT folder nor the missing "/" is the cause.
* **New lead:** camera-time stamps of the root after the restore: DCIM and MISC 19:34:27 (Canon creates them right after its format), ML 19:34:27, four more ML 19:34:28. ML's restore starts the moment Canon's "executing" dialog closes, while Canon is still setting the new card up; for about a second ML's `is_dir("B:/ML")` right after its own create most likely still answered "missing", so each new first-level folder (FONTS, MODULES, SETTINGS, ...) made ML create "ML" again. Not proven.
* Next: log ML's restore itself on the next format (each `_FIO_CreateDirectory` with its path and time, the `is_dir` answers before it, whether DCIM and MISC exist yet), then a fix that waits for Canon's own state after the format (its folders in place) rather than a fixed time.

**Logged format test build `33312bc0ca31`** (2026-11-13, not camera-tested; keep-ML-after-format ON for this test only, build check: only with the trace):
* `src/fio-ml.c` (M50 only): `fio_dir_trace`, a hook XPan sets with Debug info ON; `_FIO_CreateDir_recursive` reports each `is_dir` answer (the path and each parent) and each `_FIO_CreateDirectory` result, `_FIO_CreateFileEx` each first create that failed. NULL otherwise: no change in behaviour.
* XPan logs each with the calling task's name and whether `B:/DCIM` exists yet (Canon's own folder after a format), in a 96-line heap ring with the Format dialog watch.
* Second lead, from planning this: ML's restore is not the only writer right after a format. XPan's task writes XM50.LOG and saves ML's settings every 2 s; both create missing `ML/...` folders through the same check-then-create code, so two tasks at once could give the duplicates. The task names in the trace will tell.
* ML's restore writes back the old XM50.LOG from RAM, so XPan's log writer waits while `ml_format_restoring` (set by debug.c around `CopyMLFilesBack_AfterFormat`) and then writes the whole trace.
* Memory: the folder probe is removed and a few new log strings are shorter; `_bss_start` is exactly on its 16 KB boundary (0x124000), margin 201344 bytes.

Test: Debug info ON; Canon's menu > Format card with "keep ML"; after "Turn the camera off and on", wait until it goes away (about 10 s, while XPan writes its log), then off/on, and the card to the PC.

**Camera test of `33312bc0ca31` (2026-11-13): 12 root "ML" folders; the trace shows why.**
* **One task only:** every folder check and creation came from ML's `debug_task` (the restore); no XPan writes, no race.
* **Not too early:** Canon's DCIM was visible when the restore started (DCIM 1).
* **What fails:** the restore made FONTS, MODULES, SETTINGS, cropmks, data (each: file create failed, `is_dir(B:/ML/x)` 0, `is_dir(B:/ML)` 1, `mkdir` 0). Right after `mkdir B:/ML/scripts`, **every `FindFirstEx` fails, even for DCIM** (DCIM 1 -> 0, line 529 of XM50.LOG) and stays failed; from then on `is_dir(B:/ML)` is 0 and every new folder makes ML create "ML" again (scripts, scripts/lib, XPAN, XPAN/LUTS, ...). `mkdir` returns 0 every time, also for the duplicates. The visible ML has 33 files.
* **So:** after some number of calls Canon's directory search stops working altogether: a resource that runs out. The first run (no trace) failed later, with 5 duplicates; this one makes about twice the searches (the trace checks DCIM on each line) and failed sooner with 12, which fits a fixed pool. Candidates: `FIO_FindClose` (E05483DA, "CloseEnt") not releasing the search handle, or Canon's `FindFirstEntry` keeping a handle when it fails (ML's `is_dir` does not close after a failure; the restore asks about many missing folders).
* Next, read only, on a working card: repeat `is_dir` of an existing folder, then of a missing one, a few hundred times each, and log the call number where `FindFirstEx` first fails and whether DCIM is still found after each series. That says which call leaks.

**Search leak probe build `3d5d04c2510a`** (2026-11-13, not camera-tested): keep-ML-after-format OFF again. XPan > Diagnostics (Debug info) > **Search leak probe**, read only: (A) search `B:/DCIM` and close it, up to 300 times; (B) search a missing folder `B:/XNONE` (an error, not closed, as ML's `is_dir` does), up to 300 times, checking every 10th call that DCIM is still found. Logs the call where searching first fails. Build check: the probe creates, writes and removes nothing. `ml_format_restoring` now lives outside the (off) feature. Margin 201344 bytes, `_bss_start` 0x123A00.

**Camera test of `3d5d04c2510a` (2026-11-13): failed searches leak.** (A) search + close of DCIM: never failed in 300 calls. (B) search of a missing folder: **DCIM lost after 110 calls**. Every failed `FIO_FindFirstEx` on the M50 keeps one of Canon's search handles, and there is nothing to close (the call returns an error, not a handle); after about 100 of them no search works until a restart. ML's `is_dir` answers "missing" by a failed search, and the restore asks about many folders that do not exist yet: that used the pool up, and from then on `is_dir` said "missing" for everything and ML created "ML" again and again.
* **Fix to build (M50):** `is_dir` that never searches a path that may be missing: list the parent folder (it exists: the root, or a folder already found the same way) and look for the name; every search succeeds and is closed. Then the format test again.
* Other searches of possibly missing folders also cost a handle each on the M50: XPan's LUT scan (xpan-m50.c, `FIO_FindFirstEx(path)`), the cube list (xpan-native-cube.c), the cropmarks list (cropmarks.c). They run at start or on a refresh, so a missing folder costs a few handles per boot, not a hundred; worth the same treatment later.
* The card (12 root "ML" folders): format it **in the camera** (Canon's own layout; with this build ML is simply removed), then on the PC the card script (it copies ML and stops at "boot markers missing") and EOScard for the boot flags.

**Fix build `f50ef0122795`** (2026-11-13, not camera-tested; on the 250 GB card):
* `src/fio-ml.c`, M50 only: `is_dir` no longer searches the path itself. It fixes the path up, then looks the last name up in its parent's listing (`m50_dir_has`, directories only, case-insensitive); a parent other than the root is checked the same way first, so only folders already known to exist are ever searched, and each search is closed. Host-tested with a fake listing (root, missing parent, nested, trailing "/", case).
* Keep-ML-after-format ON again for the format test, with the folder trace (task names; the trace's own DCIM check is gone, it was a failed search too). XPan's log still waits for `ml_format_restoring`.
* Memory: the leak probe and the Format dialog watch are removed (both done; the dialog addresses are in consts.h and check_m50_rom.py); margin 201344 bytes, `_bss_start` 0x124000 (on the boundary: the next code may push ML's reserve one 16 KB step).
* Build check: the M50 `is_dir` only lists parents.

Test: this card (with its 12 "ML" folders) is fine for it, the format erases it. Debug info ON, Canon's menu > Format card, "keep ML", OK; wait for "Turn the camera off and on" to go away; off/on; card to the PC: the root should have one "ML", and the card script checks files and boot flags.

**Camera test of `f50ef0122795` (2026-11-13): works.** Format with "keep ML": one root "ML", 43 files, ML starts after off/on, the card script finds the files and the exFAT boot flags right. The trace shows each folder created once (FONTS, MODULES, SETTINGS, cropmks, data, scripts, scripts/lib, XPAN, XPAN/LUTS ...), `is_dir(B:/ML)` 1 throughout.

### Build `dd1cc8193c52` (2026-11-13): keep ML after format final, test code removed, every folder search protected

* **Keep ML after format ON** (features.h), as tested: dialogs 0x1AE94 / 0x20C14, line 4, DOWN, `sd_device[0]`, the C partition decode, notify boxes only outside Canon's Format dialog, off/on instead of a reboot.
* **`FIO_FindFirstEx` (src/fio-ml.c), M50:** checks the folder with the leak-free `is_dir` first and answers "missing" itself (`PTR_INVALID`), so Canon never searches a missing folder. This covers every caller that may search one: the beep / sound list (beep.c), config (config.c), Eye-Fi (tweaks-eyefi.c), the cropmarks list (cropmarks.c), XPan's LUT list (xpan-m50.c), ML's restore. Each search now first lists the parents (all present, all closed); a few extra searches, no leak.
* **Removed (format test code):** the folder trace (`fio_dir_trace` in fio-ml.c), `ml_format_restoring` (debug.c), XPan's format log ring, its notes and log-writer part (xpan-m50.c). fio-ml.c is back to the committed version plus the M50 `is_dir` and the `FIO_FindFirstEx` check.
* **Debug info OFF now stops** the OSD send hook's per-task counts and event log and the AF box hook's caller counts (the parts the fixes use, `osd_stale` and the touch correction, keep running). The exposure / lens property handlers already logged only with Debug info.
* Not checked: whether Canon's other calls on a missing **file** (size, open) leak too; XPan opens only files it found in a listing, and ML's `is_file` asks for a size.
* Build checks: keep-ML ON, the `FIO_FindFirstEx` guard, no test code back. Margin 201344 bytes, `_bss_start` 0x123E40.

## Next phases

See [HANDOFF-XPAN-M50.md](HANDOFF-XPAN-M50.md), "Open / next".

## Files

| File | Role |
| --- | --- |
| `src/xpan-m50.c` | The M50's own stages: HUD controls, Custom LUT, Live LUT, grain, 65:24 JPEG and live picture, touch AF, XPan menu |
| `src/xpan-native-table.c` | Colour table conversion, 10-point (shared with the EOS M) and 17-point |
| `src/xpan-mode.h` | M50 sensor size and Canon crop records |
| `src/xpan-photo-hud.inc` | HUD drawing without an idle bitmap on RGBA bodies |
| `src/xpan-hud-controls.c` | M50: INFO only, Q/SET to Canon, no mode forcing, no ROM hooks |
| `src/cropmarks.c` | No OLC dial state on the M50 |
| `src/zebra.c` | Monitor placement only with a histogram or waveform |
| `platform/M50.110/` | `XPAN=y` build, `CONFIG_XPAN_BODY`, `FEATURE_FLEXINFO` |
| `build_tools/build_xpan.sh`, `RUN_XPAN_BUILD_M50.cmd` | `CAM=M50.110` |
