# Hand-off: XPan on the Canon EOS M50 (2026-11-10)

Read this first in a new session, then `CLAUDE.md`, then the M50 sections of
`README-XPAN-M50.md` from "EVF screen" down (the full research log; older
sections describe superseded attempts).

## Where things stand

> **Status 2026-11-12, latest (read first).** On the card: **`f14c8685ef89`** = XPan Cineon with curve A (colour) as pure log without shoulder; **tested: colour bands at the clip** (R-G/B-G rise again just under the top instead of fading to white; README "Camera test of `f14c8685ef89`"). On the card now: **`0fb2e553a332`** = + Diagnostics "Colour curve test" (curve A: pure log / A = B / flat at clip / Canon-like), **tested** (README "Camera test of `0fb2e553a332`"): user verdict: **all variants look bad** (green band, then pink band, then dull, near the highlights; the same in every curve A variant). Offline: R and B do not follow curve B channel by channel (fit residual 3-4x G's), so a shoulder in curve B cannot keep the hue; the old base does the roll-off in the 3D table and has the same highlight range. Bands also with Shoulder slope 0 % (user), so not the shoulder; likely David's point positions near the top (README "Answered without a test"). Measurement round chosen: build **`30e00acc5bcd`** = + Tone curve test "B top ramp" (README "Measuring the top points"), **tested** (README "Camera test of `30e00acc5bcd`"): the curves' point list is ~0.1 EV wrong at points 11-17 (segment steepness 0.6x-1.5x, in the band zone), right at 18-20; sensor clip at point 20.1. On the card now: **`3ced97ccb9b3`** = XPan Cineon rebuilt from the measured positions (mid-tones unchanged), **tested** (README "Camera test of `3ced97ccb9b3`"): with A = B 80/50 the dip is gone and the fade is close to the old base, with less colour; Pure log A and slope 0 still band at the clip. User: better, colours look as if some colour is missing. On the card now: **`d3a524db1ee5`** = + Diagnostics "Full-range curves" (curves x3/2 to near 0x800, table x2/3 before the LUT; README "Full-range XPan Cineon"), **tested: worse** (abrupt yellow-green highlights: red hits a ceiling near 0x7D0 first), but the colour level comes back. Next option: a smaller stretch (clip under 0x700). On the card now: **`a4728a288c70`** = Tone base XPan Cineon replaced: Canon's render with its contrast-0 curve B (David hook) + the M50's own base `CIN0EV.cube` (fitted on the calibration photos like CIN1EV, `XPAN_RENDER0=1`); the David log curves stay as Diagnostics "Log curves test". README "XPan Cineon from Canon's curve". **Tested** (IMG_2957-2962): no bands; with a LUT the user prefers CINLV's highlights (CIN0EV brighter in the upper tones, shorter roll-off: fixed 1.5 EV shoulder). On the card now: **`3c86ee5fcaa8`** = the XPan Cineon base built in the camera from 17 fitted grey nodes with Shoulder knee / slope (README "Build `3c86ee5fcaa8`"); all study tests (curve tests, log curves, full range, clip raise) removed with their code and checks. Then **`e2adc292c17a`** (on the card, not camera-tested): photo log lines carry tone base, knee, slope, cube and strength; Cineon log +1 EV, CIN1EV and the Hilton hook removed (README "Build `e2adc292c17a`"). Then **`8f66b7a2e967`** (on the card): shoulder default knee 90 % / slope 25 % (user's pick from IMG_2975-2987), BOOT_M50.FIR in the zip (never copied to the card by xpan_update_card.ps1). Then **`da4df89eb1b9`** (on the 32 GB card, not camera-tested): EV/WB/drive/metering from fixed tables of Canon's own values (from XM50OPTS.BIN), learned settings removed (README "Controls: Canon's values"). Then **`dceb2acbd0e8`** (on the card, not camera-tested): WB as on the EOS M (dial: presets up to Kelvin, then Kelvin; tap on the selected WB: next preset). Then **`774cc23ddf55`** (on the card): Diagnostics "Video LUT" status + log line, for the video LUT (README "Video LUT status"); resolved: a camera setting, the video LUT works. Then **`05d5ec370686`** (on the 250 GB card, not camera-tested): video LUT selector box in movie mode (README "Video LUT selector"). Then **`ef55406f2f85`** (on the card): the box sized to the name in the large font. Then **`15d2eae48d52`** (tested OK): XPan's view returns Canon to its clean INFO screen (mode 3) with Canon's own INFO presses, as on the EOS M (README "Clean Canon screen"). Then **`e530206a6bf1`** (tested: looks very good): XPan > Halation ON/OFF + Strength, in the Film grain stage (README "Halation"). Then **`32b37a5b2952`** (on the 250 GB card, not camera-tested): Halation 70 % = the old 100 %, menu row right below Film grain. Then **`a753e986ff2e`** (tested OK): keep-ML-after-format step 1, a read-only probe: SD read through sd_device[0] works, dialog lines found (message = 4; INFO is Canon's low-level toggle, so DOWN for ML) (README "Keep ML after a Canon format"). Then **`c953fbf21b35`** (on the 250 GB card): step 2, ML's RAM copy checked (tested OK: 15 MB buffer granted, 52 files 3.2 MB, all identical; photos fine afterwards). A black Canon menu before the test, not caused by it (open). Then **`92589fc035bf`** (tested: works; format with ML kept, boot flags right): step 3, ML's keep-ML-after-format enabled for the M50 (bootflag on sd_device[0], C partition decode, DOWN toggle, off/on instead of reboot); black Format dialog from ML's notify boxes. Then **`57687f1f491d`**: no notify boxes while the Format dialog is open. **But the restore left five root "ML" directories on the exFAT card** (likely ML's is_dir on exFAT), so the feature is OFF again in **`cc6fbf76e525`** (on the 250 GB card) until fixed (README "Found on the PC after that test"). Then **`d56760d94889`** (tested): the folder checks work in normal use (also for empty folders, with or without "/"); likely cause now the restore starting while Canon is still setting up the freshly formatted card (DCIM/MISC/ML created in the same second); next: log the restore on a format. Then **`33312bc0ca31`** (tested: 12 ML folders): the trace shows one task only and Canon's directory search failing for everything (even DCIM) after about 30 calls: a search-handle resource runs out (FindClose or a failed FindFirst not releasing). Keep-ML must not be used; next: a read-only handle-leak probe. Then **`3d5d04c2510a`** (tested): search+close never fails; **a failed search of a missing folder leaks a Canon search handle (all searches fail after ~110)**. ML's is_dir uses failed searches: that was the cause. Then **`f50ef0122795`** (tested: format with ML kept works, one ML folder, boot flags right). Then **`dd1cc8193c52`** (on the 250 GB card): keep-ML final, FIO_FindFirstEx never searches a missing folder on the M50 (covers LUT, cropmarks, config, beep, Eye-Fi lists), format test code removed, OSD/AF diagnostic counters only with Debug info (README "Build `dd1cc8193c52`"). Before: On the card: **`b729a2f45f3e`** = Tone base "XPan Cineon" (saved setting: Cineon in David's curves A and B, no table base) with Shoulder knee / slope settings (precomputed curves, build-checked) and a Diagnostics "Highlight clip test" (Hilton clip x5/4 with XPan Cineon); README "XPan Cineon tone base"; not camera-tested. Before: On the card: **`41d9639f4e8b`** = A+B Cineon with a partial shoulder (end slope 0.5: 27 codes in the last half stop instead of 9, top 683 = old base's top; README "Levels and range check"), not camera-tested; proposed next: Hilton clip word raised in XPan's copy so R/B use curve B's spare points 21-25 (+0.4 EV). Before: On the card: **`c273d80756a4`** = A+B Cineon with a soft shoulder (knee 1.2 EV under the clip, as CIN1EV) and the mid-tones matched to the old base (EV_SHIFT -0.75); `df4e97500be2` tested: hard stop at the clip, ugly highlights, mids 8 levels low (README "Highlight roll-off"). Not camera-tested. Before: On the card: **`df4e97500be2`** = A+B Cineon (A fixes the colour: chroma 13.1 vs old 12.9) with the exposure placement moved down 1.1 EV to match the old "Cineon log" base (IMG_2902 vs 2904: +25 levels everywhere; README "Old and new Cineon base compared"), not camera-tested. Before: On the card: **`d81907851e7b`** = + "A+B Cineon + toe" (curve A, David's colour curve, also Cineon; README "User comparison with the same LUT": B alone left the colour at Canon's strength, 1.8x the chroma of the old base), not camera-tested. Before: On the card: **`b71d68d02362`** = Cineon curve B + "B Cineon + toe" (the header's first 0x100 is the curve's lowest point, set to its Cineon value 0x23A; README "Camera test of `de32d66f274b`"), **tested**: log (+21...+23/EV) from luma 84 up; below point 0 (luma 71, -5.8 EV) the hardware toe is linear; the rest of the toe can go in XPan's colour table (README "Camera test of `b71d68d02362`"). Was: `de32d66f274b` = the tested Cineon curve B + a header test (B flat 0x400 with either 0x100 word before curve B raised to 0x200; README "Extending the toe"), not camera-tested. Before: On the card: **`c367ff3a476b`** = the B Cineon log with point positions fitted from two brackets (first version `c6c66370b78a` tested: clip 194 right, lower mid-tones 1.8x too steep; README "Corrected curve"), **tested OK**: true log (+21...+23 per stop) from luma 85 to 165, clip 193, a toe below luma ~80 from Canon's header 0x100. Was: `c6c66370b78a` = Tone curve test + "B Cineon log" (XPan's Cineon in David's curve B from the measured point positions; README "XPan's Cineon log in curve B"), not camera-tested. Before: On the card: **`79c670c80ccd`**, the tone curve test (Diagnostics > Tone curve test; README "Curve test build `79c670c80ccd`"): Tested: curve B (P1+4E8) is the tone curve; curve A (P1+33C) acts on colour only (chroma down to 40-60 % when straight). B-straight bracket shot (IMG_2856-2860, fit in README); the camera crashed saving a 6th photo, cause unknown (log ends before it). Offline: Canon's Neutral grey axis is straight (not the cause); refit without clipped pixels is smooth, but B's output -> JPEG step is not a plain scale. On the card: **`99a442cd5f4a`** = the curve test + B flat 100..800, tested (README "Camera test of `99a442cd5f4a`"): JPEG luma = B output / 8; B's input is linear light, evenly spaced points, sensor clip at point ~20.5 of 25. Repeated crashes ~0.5 s after XPan's Canon layout resumes after a photo (user left ML's menu, no INFO) = the open "crash after playback in the Canon layout", most likely not the curve test. Next: XPan's log curve in B. Stale HUD after the Canon layout: **fixed and tested OK** in `5968e6e94392`. Next: David probe `6f96fa8cbae3` (tested: all picture style settings act in DavidPictParam1, none in Canon's table; contrast = 25-point curve at P1+4E8, saturation P1+404..40C, colour tone P1+414..418; README "Picture style settings under XPan's LUT"): where Neutral's contrast / saturation / colour tone land. Before: Cause confirmed by the `8a8dce8ae6f3` test (README "Camera test of `8a8dce8ae6f3`"): Canon's CtrlSrv refresh and ML's `redraw()` in GuiMainTask overlap at each Canon layout change; the later one sends the wrong OSD buffer. Proposed fix: on a bad send seen by the probe, `redraw()` once more. On the card: **`5968e6e94392`** = `8a8dce8ae6f3` + that fix (README "Fix build `5968e6e94392`"). Before: **`8a8dce8ae6f3`** = `3b386d43f38f` + an OSD send probe on DV_SetUpdateOSDVram (E00EF372, page E00E already hooked: no new MMU page): which task sends which OSD buffer vs the one Ximr rendered. The `3b386d43f38f` test ruled out a stuck refresh (GuiMainTask idle, nobody on winsys_sem); the data fits two overlapping refreshVrmsSurface calls from two tasks (README "Camera test of `3b386d43f38f`"). Before: **`3b386d43f38f`** = `b0f2c6ac5e0a` + Canon's log and task states once when an exit leaves the screen stale. The camera test of `b0f2c6ac5e0a` showed Canon's next refresh started (Ximr output moved to the next buffer) but never sent it; it most likely waits on winsys_sem (README "Camera test of `b0f2c6ac5e0a`"). Before: **`b0f2c6ac5e0a`** = `63ad38560f4e` + read-only OSD buffer values in the Canon layout snapshots (README "Canon layout exit: HUD missing", "ROM study" and its diagnostic note); test: Debug info ON, LCD, INFO into the Canon layout and out 4 times. Before it, on the card: **`8f24dadbfa61`** again (tested: works; HUD missing after leaving the Canon layout). `5761b9766d0c` FAILED (Err 70 at the first start, layouts broken) and is reverted in the source (`63ad38560f4e`, build check: no refreshVrmsSurface); README "Canon layout exit: HUD missing" has the cause found (ML's bare XimrExe renders into an OSD buffer that is not on screen) and what is open. Its "ROM study (2026-11-12)" narrows the cause to two cases (the display driver stores but does not apply Canon's OSD switch in its state 0, or Canon makes no refresh after the exit) and lists the read-only values that would tell them apart. Was: `5761b9766d0c` = `8f24dadbfa61` + ML's layer push ends in Canon's own `refreshVrmsSurface()` (E0528540: XimrExe + DISP_SetUpdateOSDVram, display gives winsys_sem back) instead of a bare XimrExe, for the HUD missing after the Canon layout (README "Canon layout exit: HUD missing"); first use of Canon's refresh, not camera-tested, test on its own. `8f24dadbfa61` = `0aba2b6a960d` + layer snapshots (Ximr record of ML's layer, one HUD row ML vs Canon's layer) at Canon layout entry, exit, exit+1.5 s; `0aba2b6a960d` trace: the HUD is drawn and pushed but never shown after the exit. `0aba2b6a960d` = `8ebaedbfdf40` (camera-tested: works very well) + a read-only Debug-info trace of the 3 s after each Canon layout exit, for the missing HUD on the LCD after INFO from the Canon layout (README "Canon layout exit: HUD missing"); diagnostic, no fix yet. `8ebaedbfdf40` = `fbcd57e8d` (committed; `96a036ae8395` camera-tested OK) + the 65:24 band moves at Canon's next display frame instead of through a forced menu open/close (the restart and its menu row are removed), and the XPan start steps wait for ML's start (`ml_started`) instead of +3 s / +6 s (README "Layout changes without Canon's menu", "Faster start"). Not camera-tested; the band change may hang, test it in its own round. Older: Menu hang fixed and tested
> (`d31841da9ff2`); forced Live View restart through Canon's MENU button
> path tested OK (`46a84ccbd3f2`). On the card: **`96a036ae8395`** = `f83e397e5bbb` + the menu refresh masks Canon's restarted LV Picture Style dialog with ML's redraw flood (tested before it: the menu stays open but Canon icons showed every 2 s); not camera-tested. `f83e397e5bbb` = `456c2afc654b` + `CONFIG_MENU_TIMEOUT_FIX` for the M50. Canon dropped ML's menu GUI mode (0x63) 6.0 s after each open (README "ML menu timeout"); not camera-tested. `456c2afc654b` = `2dc4e348c1b7` + XPan only in P/Tv/Av/M: `xpan_auto_mode()` checks both Canon mode values outside video, and `keep_manual_mode` is off on the M50 (README "XPan only in P, Tv, Av and M"); not camera-tested. `2dc4e348c1b7` = `b9570afb60fb` + video mode. The M50's `is_movie_mode()` now reads PROP_SHOOTING_MODE (0x14 in video; PROP_SHOOTING_MODE_2 there is only the movie exposure mode), so the XPan view is off in video, and the movie LUT (Canon's graded style library) plus Neutral follow XPan's movie switch (README "Video mode"); not camera-tested. `b9570afb60fb` (menu blank fix) = `2ff2f5e16469` with the deferred redraw only back in XPan's Live View. `2ff2f5e16469` tested: no black screen, but Canon's menu came up blank, cleared by the deferred redraw's `clrscr()`; not camera-tested. `2ff2f5e16469` = `d0c43db2e1aa` + the black-screen fix. LOG012 confirmed ML's `_redraw_do` stuck in Canon's `dialog_redraw` holding `bmp_lock`, with GuiMainTask waiting forever on the graphics core (`EvZicoWait`) while Canon had its display off between menu or playback and Live View. The M50's `DISPLAY_IS_ON` now reads Canon's GUILock display-off flag (0x8EB9, ROM-checked), and ML's redraw, its layer push and XPan's Canon layout wait for the display (README "Display on"); not camera-tested. `d0c43db2e1aa` = `412f09eef302` with short task lines (Canon's log cut the long ones), the owner words of each mutex a task waits on, and ML's redraw stage (zebra.c). It is a confirmation round for the playback black screen: LOG011 showed shoot_task and livev_hiprio_task waiting on one mutex (likely `bmp_lock`) and GuiMainTask waiting on an event flag, which fits `_redraw_do` holding `bmp_lock` inside Canon's `dialog_redraw` while Canon rebuilds Live View (README "The playback black screen: task states"); not camera-tested. `412f09eef302` = `f240cf24096b` + every task's state (name, state, wait object, PC/LR, stack code addresses) written into Canon's log just before the stuck power-off dump. LOG010.LOG showed GuiMainTask and ML's shoot_task stopping together after playback -> Live View in the Canon layout, with Live View still running and the screen never turned on (README "The playback black screen: Canon's log"); not camera-tested. `f240cf24096b` = `94516ebf7a17` + Canon's dumpf at a power-off still running after 5 s (Debug info only). It is for the black-screen hang after playback in the 3:2 Canon layout, where the power-off then never ends, LED on, battery pull (README "Canon's log at a stuck power-off"); not camera-tested. `94516ebf7a17` = `71a89dddc5b8` + Tone base / Grain size as ML choice lists, a semi-transparent 3:2 panel background (index 3 on the M50), and a UI state log line in XM50.LOG with Debug info ON, kept for the crash after playback in the Canon layout, still unexplained (README "Menu choices, the 3:2 panel background"); not camera-tested. `71a89dddc5b8` = RAW pauses Film grain (Canon's filter stage is JPEG only), 3:2 asked on the HUD instead of written, LUT names without ~N (README "RAW and Film grain"), not camera-tested. `22c3b16d96a8` = menu/Debug info gating, ISO always on, M50 capture format writer, forced Neutral / 3:2 / L (README "Menu, Debug info, capture format and forced settings"), not camera-tested. `60a663e074b4` = `e63d817820ba` + the cleaned XPan menu (README "XPan menu": Diagnostics submenu, on/off rows drawn right), not camera-tested. `e63d817820ba` = `0a6e15fd0b08` without the EXIF hook (7 MMU pages; CropInfo left at Canon's 0, user's choice), not camera-tested. `0a6e15fd0b08` = `11febf286d06` + the save module's table copy patched with the crop (README "The AF block's image size"), camera-tested OK: EXIF, AF block and AspectInfo 2216 from Canon; CropInfo still 0; the EXIF hook is now unused. `11febf286d06` = `de685678701f` + the 65:24 EXIF size hook (E00A551C, 8 MMU pages; README "65:24 EXIF size"), both **not camera-tested**. `de685678701f` =
> the band only while Canon shows Live View (quick review / playback were
> moved too), **not camera-tested**. EXIF size: README "Quick review,
> playback and the EXIF size" (E00A80B2 reads a rectangle at +0x1C8..+0x1D4
> of a shooting-parameter struct; its source not found yet). Nothing
> committed.
>
> **Menu hang: cause found in Canon's own log (2026-11-11)**, README
> "Menu hang: Canon's own log": ML's `apply_normal_patches` (9 hooks, one
> `apply_patches`, interrupts off, 7 x 64 KB ROM copies inside) stalls the
> camera for ~13 ms at 1.34 s while Canon's Live View runs; Canon logs
> Blanking DeadLine / OVERRUN / imgCtrl state errors, and the next Live View
> start times out (ResTimeout). `806601dc65a3` fixed ML's start but the 65:24 crop patch at ~4.7 s did the same (README "first fix tested"); on the card now **`d31841da9ff2`** (apply_patches copies ROM pages before cli for every caller), **not camera-tested**. Was: `806601dc65a3` (ROM pages copied before the critical section, one hook per `apply_patches`; Canon log saved 12 s after each start, LOG00N.LOG at the card root, check for no Blanking DeadLine / OVERRUN / imgCtrl errors).
>
> **Status 2026-11-11 (read first).** On the card: `1cf77b85ecb6` = `78556736e353` + Canon debug-log dump (dumpf) at the hang (no setter call 2.5 s after PROP_LV_ACTION 0, once per start, mark 81) and XPan > "Save Canon log"; `78556736e353` is
> (`fe99a229ffe7` + read-only UI-lock log). **The menu hang is NOT solved:**
> its first start hung at the first manual menu close with the band never
> moved (no transition call at all that start), so "geometry changed outside
> Canon's transition" is refuted as the cause; `fe99a229ffe7`'s two good
> starts were luck. README "Menu hang without the band". Canon's UI lock
> stayed 0 through the cycle (no use for a forced restart). Forced restart
> compiled out. Next: find which Canon step stops (Canon's own debug
> messages during the restart), offline ROM study first. Nothing committed.
>
> **Menu hang: cause found from the logs (2026-11-11)**, README "Canon's
> main menu breaks Live View: the cause": XPan changed the Live View
> geometry outside Canon's transition call (at boot, on layout changes);
> Canon only ever changes it inside one. Fix designed, not built yet: decide
> the band only on Canon's transition calls. Older notes:
>
> **Menu hang, what is known:** it happens whenever the 65:24 band is on
> screen, whichever way it is set (in the setter hook, moved or in place, or
> in Canon's geometry tables); with Canon's own geometry it never happens.
> Canon's display chain: DeliverStage (ROM table `E09E1DD4`) -> setter
> `E01389AC` -> layer setter `E0154B64` -> compositor `E0153E48` -> VRAM
> manager side; VRAM manager `VramDevelop.c` uses the RAM copy at
> `0xB47C` -> `0x40737500`. The exact Canon task and wait are **not** known:
> the snapshot that was meant to show them caused Err 70 instead. Next step
> (offline first): find where Canon waits after a Live View restart by
> reading `VramDevelop.c` / DeliverStage, without camera rounds.


- Repo: `F:\OTROS\magiclantern_simplified`, branch **`xpan-m50`** (base
  `dev`). The M50 port is committed (`0765a15f8`, 65 files, not pushed),
  plus later doc fixes.
- Last build tested on the camera: **`8f344d055b96`**, archived in
  `platform/M50.110/xpan_builds/20261110-173755-8f344d055b96/`. All camera
  tests of this build passed.
- **Canon's main menu breaks Live View: cause found, no fix yet
  (2026-11-10).** Intermittent; older than this session (the bisect's
  "works" were luck). With the 65:24 live placement switched off
  (`M50_DISP_MOVE 0`, build `e2d704c10bf4`) it never hangs. In a hang,
  Canon's display never makes its "image unmute" call (a2 = 1) after the
  menu. A first fix (keep the geometry through Canon's transition call,
  `disp_decide`) failed. README "Canon's main menu breaks Live View" has
  the data. Next: how Canon shows its own 16:9 / 1:1 / 4:3 in Live View.
- **Cause found in Canon's code (2026-11-10):** Canon builds the Live
  View display from its own geometry table (`0xB47C` -> `0x40737500`) and
  its VRAM manager (`VramDevelop.c`) allocates and confirms the image VRAM
  from it at every Live View start; XPan's in-call geometry left Canon's state
  disagreeing with the hardware (README "Canon's main menu breaks Live
  View"). `f20297824a9f` (RAM table only): no crash, but no crop either:
  the blocks come from DeliverStage's ROM table `E09E1DD4`. **On the card
  now: `8e1b0daf4834`**: the band in both tables (ROM through the MMU, RAM
  through ML's patch API); the hooks only observe. Not tested yet: same
  "Live View table test" below.
- Source (not committed): the 17-point code is in, switched off
  (`M50_GRID17_TEST 0`; `build_xpan.sh` fails if it is on). Do not use the
  archived builds `edf5ef3fb661`, `4a368c71bb7a`, `59238b91b421`.

### Working on the camera (tested)

| Feature | How |
| --- | --- |
| Custom LUT on photos | colour table hook `E00E73BC` passes XPan's 10-point table |
| Live LUT preview | grades Canon's style library tables (each write checked against Canon's original / XPan's last write) |
| Film grain (colour, on photos) | XPan's grain runs in Canon's own Grainy B/W stage (hook `E086DA10`, both CPUs, ~0.8 s with 65:24); Live View gets Canon's "no filter" value (hook `E0776CC6`) so it stays in colour |
| 65:24 JPEG (6000x2216, centred) | Canon writes it itself: 3 patches through ML's `apply_patches` (size table height, encoder top and rows) |
| Grain/LUT only with XPan on | as on the EOS M; XPan off = plain Canon |
| 65:24 live picture, LCD and EVF | Canon's display setters `E01389AC` (panel) / `E01390E0` (EVF) get a copy of Canon's block with source row / destination row / height: the display hardware moves and crops the band (no copy, no CPU per frame) |
| EVF layout | whole HUD moves up 30 rows (`xpan_m50_screen_extra_top()`, from Canon's Ximr output size 720x540 vs 720x480) |
| Touch AF and Canon's AF box | Top/Split: picture area centre getter `E042667C` shifted by the band's move; Bottom: tap row corrected in the converter `E0423406` and the box row in `E042268A` (the area top is unsigned in Canon's code, so it cannot go negative) |
| No crash with the EVF (3 runs) | ML's RGBA copy re-reads Canon's layer under `winsys_sem`, only if >= 960x540 |

### Open / next

1. Committed as `0765a15f8` (not pushed).
2. **EXIF height** of 65:24 JPEGs still says 4000. Looked at on 2026-11-10:
   Canon's size getter `E0081E4E` (the patched live size table) is only
   called by the art filter code (`E08B95E0`...`E08BC38A`), so EXIF takes its
   size elsewhere. The RAM dump has five heap copies of the size block
   ("Canon EOS M50" header, 3:2 L pair at +0x48) at `0xC38C28`, `0xC7AC68`,
   `0xD63210`, `0xD6D3B0`, `0xDB0880`: who makes them, and when, is the next
   thing to trace (the EXIF templates are built in `E00A39xx`, tags
   0xA002/0xA003).
3. **Canon's own UI keeps popping up** over XPan's view (user, last test):
   XPan's dim-and-freeze (`canon_ui_enter`/`canon_ui_leave` in
   `xpan-m50.c`) lets Canon's screen through on Canon events; may need a look.
4. **Important (user, 2026-11-10): 17-point colour table. FAILED on the
   camera (build `59238b91b421`), switched off.**
   - What ran: Canon's loader got size `0x4CC4` with XPan's 17-point table
     (layout A, `r + 17 g + 289 b`), built ahead in XPan's task.
   - What happened (user + `XM50.LOG` + IMG_2719-2725): three 17-point
     photos were saved (IMG_2723-2725). Same exposure as the 10-point
     photos, but **half the brightness** (mean 39 against 80; the first one
     18), colours otherwise coherent. Live View stopped updating, the camera
     was "extremely unreliable" and then stopped responding. The log ends at
     the third 17-point photo.
   - What that shows: giving the loader the 17-point size is **not** a
     complete 17-point set-up. Coherent colours at half brightness fit nodes
     64 apart over 0..1024 (the same spacing as 10-point, twice the range)
     instead of 32 apart over 0..512: if so, the 17-point grid adds range,
     not finer steps, and the premise of this item needs checking first.
   - Not known: what made Live View stop and the camera hang. Candidates,
     none proven: the engine left in grid 17 after the develop (Canon sets
     grid 10 at SusanDevelop start, `E011DA86` -> `E087D79A`, and nothing
     else found resets it), or XPan's task building the table.
   - Before any new camera test: (1) ROM study of everything Canon's
     DevelopStandard does around `E01C7358` for grid 17 (registers
     `+0x30/+0x34/+0x48/+0x50/+0x60`, `E087D702`..`E087D970`), and who uses
     the engine in Live View; (2) the first test must be one shot, with the
     user told it may hang, never a batch.
5. **Important (user, 2026-11-10): highlight calibration and an earlier LUT.**
   - Calibrate an M50 Cineon tone base (`CIN1EV`/`CIN2EV`/`CINLV`) the way
     the EOS M one was made (`build_tools/xpan_cineon_base_v7.py` and the
     `build_tools/*.npy` model; README-XPAN.md "Tone base / Highlight
     headroom"). Today the M50 uses the EOS M's files, which are fitted to the
     EOS M's Neutral render.
   - **Earlier LUT: ROM study done 2026-11-10** (README "Still processing
     path: where a LUT could act earlier"). Result: Lucky is the only 3D
     table and the last colour stage; nothing before the demosaic can take a
     3D LUT, so a LUT on the 14-bit linear data is not possible with Canon's
     engines. What is possible there is the EOS M method: Canon's Hilton
     stage (black level / white balance gains on the 14-bit Bayer data) gets
     a copy of its block with the gains lowered by 1-2 EV, so highlights
     reach the table unclipped. Hand-over point found: `E08DB920(obwb,
     corr)`, block `DEV_PARAM + 0x2D8C`, 21 words, gains in words 9-12
     (`0x400` = 1.0). Next: a read-only log of the shot's block (one new
     hook, single test, see the memory rule on risky tests), then the gain
     copy, then the M50 tone base calibration.
   - **Probe done (works):** one caller per photo; gains `0x460 0x400 0x400
     0x768` (R G G B), white `0x3074`, black ~`0x1FF`; the gains do not
     change with exposure. Next: Canon's `E08DB920` gets a copy of the block
     with words 9-12 lowered by 1 or 2 EV (XPan's stills only), first as a
     calibration test with LUT `IDENT` and Canon Neutral, to fit the M50
     tone base.
   - **Headroom test done (works, build `b3a0005a584d`):** the clip acts
     after the gains, so lowered gains keep the sensor's highlights. About
     0.5 EV in green above Canon's JPEG white (more in a channel with a high
     white balance gain); **-1 EV holds all of it**, -2 EV only wastes range.
     Next: the M50's own CIN1EV tone base, fitted from these photos (README
     "Highlight headroom on the 14-bit data"), then headroom -1 EV with that
     base as a real setting.
   - **M50 CIN1EV fitted and built in (2026-11-10, not tested on the
     camera):** `build_tools/xpan_m50_cin1ev.py` (plain Python + PIL)
     fits Canon's M50 Neutral tone curve from the four gain brackets
     (IMG_2736-2747) and writes `data/XPAN/M50/CIN1EV.cube` (17-point,
     shipped as `ML/XPAN/CIN1EV.cube` in the M50 package). Tone base
     "Cineon log +1 EV" = gains 1 EV lower + this base. See README
     "M50 tone base for the headroom".
   - Original note: investigate applying the LUT **earlier**: right after debayer, before
     Canon's white balance/matrix/tone curve, on the ~14-bit linear data,
     instead of on the 10-bit YCbCr after Canon's tone curve where Lucky sits
     (highlights already compressed or clipped there). Start from the
     still-develop pipeline order (ShootDevelop/SusanDevelop, the ELD/Opera
     stage lists around `E0194B20`/`E11FD6D8`) to find a stage between
     demosaic and gamma that XPan could use or feed. Another option to look
     at: give Lucky a flatter input by passing Canon's develop a copy of its
     tone curve/gamma parameters (same "modified copy" rule as the display
     blocks), so more highlight range reaches the table.
6. **Watermark** (same stage as the grain).
7. ~~JPEG crop alignment~~: not a to-do. The 65:24 JPEG is always the
   middle of the sensor (rows 892-3108), on the M50 as on the EOS M, in
   every Frame alignment. Top/Bottom/Split only move the frame and HUD on
   the screen. Keep it that way (user, 2026-11-10).
8. Probes still in the build, read only, log to `ML/LOGS/XM50.LOG`: display
   blocks (`disp_record`), the AF converter record (`af_recs`), tap lines,
   Ximr / display layer changes. Keep or trim once stable.
9. The EOS M (`EOSM.202`) shares `cropmarks.c`, `xpan-photo-hud.inc`,
   `bmp.c` changes (all M50 parts are `#ifdef CONFIG_M50`); it has not been
   rebuilt since. Build it once before a release.
10. ~~Stale texts~~ (fixed 2026-11-10): the README's intro, "To do" and
   "Next phases" now point here; `xpan-m50.c`'s header comment and the XPan
   menu help no longer say "framing and HUD only".
11. The ROM tools read the whole dump at once, which fails on the Windows
   drive under WSL ("Cannot allocate memory"): copy `ROM0.BIN` into the WSL
   home first (`~/m50/ROM0.BIN`) and run the tools on that copy.

## Live View table test (build `f20297824a9f`)

1. Power on in 65:24 (any layout). Note whether the band is placed at once
   or only after step 2.
2. MENU (Canon's menu), MENU back: Live View must move; at four separate
   starts, twice each; once with the menu open 10 s.
3. Change Frame alignment (Top / Bottom / Split): note when the picture
   follows. Switch to 3:2: full picture.
4. Play back a photo, back to shooting; take a 65:24 photo.
5. Power off normally each time.

## Canon menu fix test (build `103739ee3363`)

The hang is intermittent, so several tries: four times, power on, wait for
the XPan view, MENU (Canon's menu), MENU (back): Live View must move. Do it
twice per start, and once also with Canon's menu left open for 10 s. Then
play back a photo (Play button) and go back to shooting. Power off normally
each time. Bring the card: `XBOOT.LOG` has the marks of every start.

## David probe test (build `c82e4ce99741`)

Read only, one new hook; it may still misbehave. The same scene for every
photo, tripod or steady rest, any exposure (keep it the same). In Canon's
menu: **Auto Lighting Optimizer OFF** for all photos (it changes the curve
per scene). XPan on as usual; LUT and tone base do not matter.

1. Picture Style Neutral, contrast 0: one photo.
2. Neutral, contrast **-4**: one photo.
3. Neutral, contrast **+4**: one photo.
4. Neutral contrast 0 again, **Highlight Tone Priority ON** (Canon's menu;
   ISO 200 or higher): one photo. Then HTP OFF again.
5. Picture Style **Standard**: one photo.
6. Set Neutral, contrast 0 again; check the camera is normal.

Bring the card: `XM50.LOG` ("David:" blocks, one per photo).

## Tone base +1 EV test (build `925b4bfdce76`)

Picture Style Neutral, XPan on, 65:24, Custom LUT ON, **Live LUT preview
OFF**, film grain off. A scene with bright highlights (lamp, window, sky)
and shadow; tripod or a steady rest; the same exposure for each pair.

1. LUT `IDENT`, Tone base **Cineon log**: one photo. Then **Cineon log +1
   EV**: one photo. Both should look flat (Cineon) and about equally bright
   in the mid-tones; the +1 EV one with detail in the brightest parts.
2. LUT `KODAK` (or another you like): the same two photos.
3. The same pair 2 EV overexposed.
4. Check the camera stays normal (review, Live View, power off and on).

The menu line of Tone base shows how many photos got the +1 EV gains.
Bring the card: `XM50.LOG` ("gains -1 EV (Cineon log +1 EV)" per photo).

## Working rules (from the user; also in the old account's memory)

- `CLAUDE.md`: simplest, most efficient solution; no shortcuts; the most
  reliable, standard way; use Canon's functions instead of writing Canon's
  memory or registers directly.
- **Reliability first, no timing hacks**: never design around Canon
  tolerating a wait. Use Canon's own engines and pipeline stages (user:
  "RELIABILITY and CONSISTENCY are the key... the hardware engine is the
  correct approach"). Holding Canon's ShtEncode task hangs the camera.
- **Data first**: read-only probe builds before changing Canon's behaviour;
  pass Canon a modified *copy* of its parameters rather than writing its
  structures.
- **Review the changed code before every camera test** (blocking calls,
  races: `cli()` masks one core only, abort paths; DryOS
  `take_semaphore(sem, 0)` waits forever, use `take_semaphore_now`).
- **Batch tests**: several changes and a numbered test list per camera round.
- **Every new failure type gets a check** in `build_tools/build_xpan.sh`.
- **M50 only**: no 6D builds; EOS M only when asked or EOS M specific.
- Never put one camera's build on another camera's card; leave the card's
  own files (settings, LUTs, DCIM) alone; no modules.

## Build and card

```bash
wsl.exe env CAM=M50.110 bash build_tools/build_xpan.sh
```

```bash
powershell -ExecutionPolicy Bypass -File build_tools\xpan_update_card.ps1 -Cam M50.110
```

- The build fails unless every check passes (memory margin, hook list and
  original bytes, ROM bytes against `extras/M50/ROMS/ROM0.BIN` via
  `build_tools/check_m50_rom.py`, the XPan rules above). Never copy a build
  that printed `CHECK FAILED`.
- The card script only writes to a card whose `ML\MODULES` holds
  `M50_110.sym` (drive was `P:`).
- Environment quirks (Windows + Git Bash): Python only works through WSL
  (`wsl.exe python3 ...`); set `MSYS_NO_PATHCONV=1` when passing `/mnt/...`
  paths to `wsl.exe` from Git Bash; heredocs mangle `\n` in C strings, so
  write patch scripts to a file. CRLF and LF files are both in the tree.

## ROM tools

`extras/M50/tools/` (run with WSL Python, capstone installed there):

- `disas.py ROM0.BIN <addr hex> <len hex>`: Thumb disassembly.
- `strs.py ROM0.BIN <from> <to>`: debug strings referenced by code.
- `xref.py ROM0.BIN <addr>...`: literal / adr references to addresses.
- `bls.py ROM0.BIN <addr>...`: BL/BLX callers (scans the whole ROM).

Dumps: `extras/M50/ROMS/ROM0.BIN` (ROM at `E0000000`), `RAM4.BIN` (RAM
`0x00000000`, read uncached addresses as `addr & 0x3FFFFFFF`; open with
seek, it is 256 MB).

## M50 facts worth knowing

- MMU: XPan hooks Canon ROM through ML's MMU remap with 4 KB pages
  (`CONFIG_MMU_REMAP_SMALL_PAGES`), heap-backed pages reserved in
  `apply_normal_patches()`, untouched ROM pages keep Canon's attributes
  (otherwise Live View stutters). `CONFIG_MMU_REMAP_PAGES 6` (hooks in
  E00E, E013, E042, E077, E086 + the 65:24 encoder table page E0FC).
- Hooks (all in `platform/M50.110/include/platform/mmu_patches.h`, each
  trampoline replays the replaced 8 bytes, checked by the build):
  `E00E73BC` colour table, `E086DA10` grain stage, `E0776CC6` Live View
  filter map, `E01389AC`/`E01390E0` display panel/EVF, `E0423406` touch ->
  AF converter, `E042667C` AF picture area centre, `E042268A` AF box on
  screen.
- Display: Canon's block p = `1 dev addr bufW bufH srcX srcY dstX dstY shownW
  shownH ...`; LCD 736x480 shown 720x480; EVF 1024x768 shown 1024x682 from
  row 0. 65:24 band on the LCD: 266 rows from row 107; destination Top 0,
  Bottom 214, Split 60; EVF scaled by 1024/720.
- ML's screen: 960x540 RGBA layer (WINSYS `_rgb_vram_info`), ML draws a
  720x480 window at 120,30; in the EVF Ximr outputs 720x540 and Canon's
  picture covers ML rows -30..450.
- AF coordinates: sensor units over a picture area table `*(*0x7144+0x28)`
  (X 276, W 6000, Y 48, H 4000); a tap is scaled x*6000/720, y*4000/480.
- Logs on the card: `ML/LOGS/XM50.LOG` (XPan log), `XBOOT.LOG` (boot),
  `XM50OPTS.LOG` (learned settings).
- PROP writes: only XPan's checked writers (ISO, shutter, aperture, learned
  options, `PROP_LV_CFILTER`); `PROP_LV_AFFRAME` writes crash DIGIC 8.
