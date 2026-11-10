# XPan for the Canon EOS M50 (firmware 1.1.0): phase 1

Branch `xpan-m50` (started from `xpan-6d`). The M50 shares XPan's framing
and HUD code with the EOS M ([README-XPAN.md](README-XPAN.md)) and the 6D
([README-XPAN-6D.md](README-XPAN-6D.md)). Code specific to the M50 is behind
`CONFIG_M50`; the code all bodies share is behind `CONFIG_XPAN_BODY`.

**Not tested on a camera yet.** Magic Lantern itself is experimental on the
M50 (DIGIC 8): photo capture with ML loaded is still marked unreliable
(`CONFIG_IMAGE_CAPTURE_NOT_WORKING`) and ML cannot change Canon settings
(`CONFIG_PROP_REQUEST_CHANGE` is off). Phase 1 is built around both limits:
it only draws on the screen and reads Canon's settings, and it installs no
hook in Canon's code.

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

## What phase 1 does

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
(6000 x 2216) = w2 by frame alignment (Top 0, Split 500, Bottom 1784),
w10 = 276. The file header and EXIF sizes come from elsewhere (the 3:2
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

User choice: centred first. While XPan's 65:24 frame is on (`m50_crop_poll`,
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

* **Live View crop and position.** The XPan view on the M50 only masks Live
  View: it does not cut the 65:24 band out of the middle and move it into
  place as the EOS M does. Still to find on DIGIC 8 (possibly Canon's XIMR
  compositor, kitor's `Ximr_Compositor_PoC` branch).
* **JPEG crop** to 65:24 (encoder sizes from the size/aspect codes and ROM
  tables `E0A9AE64`/`E0A9B060`).
* **Film grain** on the YUV image at the JPEG encoder setup.
* **Watermark** (same stage as the grain).
* **17-point grid** and an **M50-calibrated Cineon base**, together.
* **Library tables rejected** by the chroma-range check (16 of 27).

### JPEG crop

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

### Grain and creative filters

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

## Next phases

1. **Canon settings.** Turn on property writes for only the properties XPan
   needs (`is_prop_allowed` already has a DIGIC 8 allow-list in
   `platform/M50.110/property_whitelist.h`), checking each value range on the
   camera. That brings back the HUD's value controls, the forced Neutral /
   sRGB / L JPEG / 3:2 settings and the SET magnification.
2. **Capture path.** A read-only probe, like the 6D one, to find where DIGIC 8
   develops a still and where the picture-style table is loaded. That needs an
   M50 ROM dump (`CONFIG_AUTOBACKUP_ROM` is on) and reliable capture under ML.
3. **Live View position.** Move the picture into the band like the EOS M,
   possibly through Canon's XIMR compositor (kitor's `Ximr_Compositor_PoC`
   branch), or pick the band's position to match the crop.

bilalfakhouri's `M50_rawvideo` branch (MMU, EDMAC copy, FPS timers, mlv_lite
RAW video) is not merged: phase 1 needs none of it, and it overlaps
`src/raw.c`, which the 6D port changes too. It becomes relevant for the
capture probe and for movie features.

## Files

| File | Role |
| --- | --- |
| `src/xpan-m50.c` | Stand-ins for the stages not ported, XPan > Canon settings, M50 menu |
| `src/xpan-mode.h` | M50 sensor size and Canon crop records |
| `src/xpan-photo-hud.inc` | HUD drawing without an idle bitmap on RGBA bodies |
| `src/xpan-hud-controls.c` | M50: INFO only, Q/SET to Canon, no mode forcing, no ROM hooks |
| `src/cropmarks.c` | No OLC dial state on the M50 |
| `src/zebra.c` | Monitor placement only with a histogram or waveform |
| `platform/M50.110/` | `XPAN=y` build, `CONFIG_XPAN_BODY`, `FEATURE_FLEXINFO` |
| `build_tools/build_xpan.sh`, `RUN_XPAN_BUILD_M50.cmd` | `CAM=M50.110` |
