# Hand-off: XPan on the Canon EOS M50 (2026-11-10)

Read this first in a new session, then `CLAUDE.md`, then the M50 sections of
`README-XPAN-M50.md` from "EVF screen" down (the full research log; older
sections describe superseded attempts).

## Where things stand

- Repo: `F:\OTROS\magiclantern_simplified`, branch **`xpan-m50`** (base
  `dev`). The M50 port is committed (`0765a15f8`, 65 files, not pushed),
  plus later doc fixes.
- Last build on the card: **`8f344d055b96`**, archived in
  `platform/M50.110/xpan_builds/20261110-173755-8f344d055b96/`. All camera
  tests of this build passed.
- Main M50 file: `src/xpan-m50.c`. Shared XPan files touched for the M50:
  `src/cropmarks.c` (layout geometry), `src/xpan-photo-hud.inc` (HUD),
  `src/bmp.c` (ML's RGBA layer copy), `src/patch_mmu.c` / `src/mmu_utils.c`
  (4 KB remap pages), `platform/M50.110/*`.

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
2. **EXIF height** of 65:24 JPEGs still says 4000.
3. **Canon's own UI keeps popping up** over XPan's view (user, last test):
   XPan's dim-and-freeze (`canon_ui_enter`/`canon_ui_leave` in
   `xpan-m50.c`) lets Canon's screen through on Canon events; may need a look.
4. **Important (user, 2026-11-10): 17-point colour table.** The M50's
   Lucky engine has a 17-point grid (the EOS M only 10), so 17-point
   `.cube` LUTs (what XPan recommends) can be applied without resampling to
   10. README "The 17-point grid: who could use it" has the plan:
   - work out the 17-point table layout from Canon's Opera load lists
     (`E11FD6D8`: five banked passes, `0x39070` = 0..4, pages 4 KB apart) and
     check it on the camera with a test table;
   - pass Canon's loader `E011E194` size `0x4CC4` instead of `0x1000`, only for
     XPan's own table. The colour table hook `E00E73BC` already passes XPan's
     table, so it can also pass the size;
   - convert the `.cube` to 17^3 (`xpan-native-table.c` does 10 today);
   - Live LUT preview stays 10-point (Live View has no 17-point path found).
5. **Important (user, 2026-11-10): highlight calibration and an earlier LUT.**
   - Calibrate an M50 Cineon tone base (`CIN1EV`/`CIN2EV`/`CINLV`) the way
     the EOS M one was made (`build_tools/xpan_cineon_base_v7.py` and the
     `build_tools/*.npy` model; README-XPAN.md "Tone base / Highlight
     headroom"). Today the M50 uses the EOS M's files, which are fitted to the
     EOS M's Neutral render.
   - Investigate applying the LUT **earlier**: right after debayer, before
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
10. `README-XPAN-M50.md` "To do (not forgotten)" and "Next phases" near the
   end are stale (written before these features worked).

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
