# XPan for the Canon 6D (firmware 1.1.6): port status

Branch `xpan-6d`. The 6D port shares XPan's code with the EOS M
([README-XPAN.md](README-XPAN.md)); everything specific to a body is behind
`CONFIG_EOSM` or `CONFIG_6D`, and the code both use is behind
`CONFIG_XPAN_BODY` (set in each camera's `features.h`).

**Not tested on a camera yet.** Every 6D ROM address below was found in the
6D 1.1.6 ROM dump and is checked against the running firmware before it is
used, so a different firmware leaves Canon untouched.

## Building

```
CAM=6D.116 build_tools/build_xpan.sh      # Linux / WSL
RUN_XPAN_BUILD_6D.cmd                     # Windows, through WSL
```

Output: `platform/6D.116/build_xpan/magiclantern.zip`. As on the EOS M the
zip ships no modules. Delete the old `.mo` files from `ML/MODULES/` on the
card: they do not match this build and XPan is incompatible with them.

The XPan build reserves 720 KB for Magic Lantern instead of the stock 592 KB
(`src/boot-d45-am.c`, AllocateMemory pool start `0x4E0000` -> `0x500000`).
If the binary ever outgrows it, Magic Lantern refuses to boot and blinks the
LED (3 long, 3 short) instead of starting Canon's firmware.

## What works on the 6D

| Feature | 6D | Notes |
| --- | --- | --- |
| 65:24 / 3:2 framing, Top/Bottom/Split, HUD, themes, grid | ported | Live View only: the optical viewfinder cannot show the frame. |
| Settings XPan forces (Neutral, ALO/HTP off, sRGB, CA correction off, L / RAW+L, 3:2) | ported | The mode dial is physical: outside Tv/Av/M/B XPan only says so. |
| CR2/JPEG crop record (MakerNote AspectInfo) | off | The writer exists (65:24 frame 5472x2026, centred) but runs only in QEMU builds, on the EOS M too. The EOS M crops the JPEG pixels instead. |
| Equivalent lens on the HUD | ported | 35.8 mm sensor width. |
| HUD controls | ported, new mapping | See below. |
| Custom LUT on the saved JPEG (native colour table) | not working | The hook installs, but the 6D does not develop JPEGs in TTL (first probe log, below). |
| Cineon tone base (+1/+2 EV) | needs calibration | `CIN1EV/CIN2EV.cube` were fitted to the EOS M's Neutral rendering. |
| 65:24 crop of the JPEG itself | not yet | JPEG pixels stay 3:2; only the HUD is 65:24. |
| Film grain, watermark | not yet | Shown as "Not on the 6D yet" in the XPan menu. |
| Live View / movie LUT | not yet | The LUT does not show on the LCD. |
| 1:1 movie crop | not available | Magic Lantern's crop-mode hack is not built for the 6D. |

### Controls (Live View, XPan ON)

* **Top dial:** selected value down / up.
* **Rear dial:** previous / next HUD field.
* **Multi-controller:** LEFT / RIGHT field, UP / DOWN value.
* **SET** (short press): magnification 1x / 5x / 10x.
* **INFO:** 3:2 = layout on/off, 65:24 = Top / Bottom / Split.
* Q, MENU, PLAY and the trash button (Magic Lantern's menu) stay as usual.

If the two dials turn out swapped on the camera, it is one line in
`key_inner()` (`src/xpan-hud-controls.c`); *Debug info* logs every key to
`ML/LOGS/XPANKEYS.LOG`.

## How the EOS M hooks map to the 6D

Found by matching the EOS M functions against the 6D ROM (instruction
windows with branch and literal offsets masked), then reading both.

| EOS M 2.0.2 | 6D 1.1.6 | What |
| --- | --- | --- |
| `FF42D178` FOD routine | `FF412550` TTL routine | Colour-table decision: same code, every job-block field 4 bytes later (enable `+0x38`, table `+0x34`, tone row `+0x30`, OBWB `+0x2C`). Table copy to `C0F20000`, enable at `C0F1A014`, sizes 0x1000 / 0x7E0 / 0x188 as on the EOS M. |
| `FF92DA38` / `FF92DA94` job identity | unknown | The EOS M checks the job's ROM profile pointers. The 6D values are recorded in *Debug info* (`id a/b/c`); until they are known the hook relies on the other checks and on Canon's table passing the Neutral-table validation. |
| `0x517A0` OLC state | `0x7F5A4` | Dial target (shutter, aperture, ISO, compensation); same getters. |
| `FF0CC564` button dispatcher | `FF0CC1A8` | Debug key log only. |
| `FF4288AC` TTJ (JPEG crop) | `FF407040` | Same code, but see below. |
| `FF3FF044`, `FF3B4074` playback | `FF3E6228`, `FF399BF8` | Same code; not needed until the JPEG is cropped. |
| MPU wheel/LEFT decoding | none | The 6D's dials and buttons arrive as normal GUI events. |
| LCD colour probes (RAM `0x285BC`, `0x28C38`) | not found | Different Live View code. |
| Grain engine and YUV stage (`FF1703E0`, `FF41A928`...) | not found | See below. |

### Why the JPEG crop, grain and watermark are not there yet

The EOS M builds a still in two Canon jobs: FOD develops RAW to a YUV image,
then TTJ encodes the JPEG from it. XPan crops the JPEG by giving TTJ a
private descriptor and register profile, and adds grain and the watermark to
the YUV image in between.

On the 6D the TTL routine's register profile (ROM `FFD4EE68`) carries both
the develop registers (`C0F37xxx`, `C0F08xxx`, `C0F1Axxx`) and the JPEG
engine's (`C0F11xxx`, `C0F13xxx`, height 3648 = `0x0E3F`). The develop and
the encode may well be one pass, with no YUV image to work on and no TTJ job
for the main JPEG. That has to be seen on the camera before any crop is
attempted.

### First probe log (2026-11-07)

Settings were right (Neutral, sRGB, L JPEG, 3:2, XPan ON, 65:24) and the
look hook was installed and ready. Yet:

* An **L JPEG** shot called neither TTL nor TTJ: no develop job of either
  kind, and the look hook never ran.
* The **RAW+L** shot called TTL once, with a 5568x3708 job (the full sensor
  readout, `+0x3A`/`+0x3C`) and LuckyEnable `0xFFFF`: Canon skipped the
  colour table. The look hook rejected it (no table, enable > 1). Its job
  block starts with RAM pointers (`7C29C`, `0xF`, `7C2D8`), not with ROM
  profile pointers as on the EOS M.

So the 6D develops its JPEGs in a routine that is neither TTL nor TTJ, and
the colour table (picture style) is loaded there or not through the Lucky
table at all. The other Lucky-table writers in the ROM (`FF8B8Bxx`) belong
to the creative-filter engine. The probe now also records Canon's debug
messages, where each develop routine names itself.

### The 6D capture probe

**XPan > 6D capture probe** (SET: ON/OFF, PLAY: *Save probe log*) records
Canon's job blocks at the TTL entry and at the TTJ entry and exit, 0x120
bytes each, for up to 48 calls, and Canon's debug messages in a ring of up to 6144 (passed on by
Magic Lantern's own `DryosDebugMsg` replacement, `hacked_DebugMsg` in
`src/gui-common.c`; on the 6D Canon's logger itself never runs). The ring freezes 1.5 s after a photo
has finished ("photo recorded, save the log"), and *Save probe log* appends
everything to `ML/LOGS/X6DPROBE.LOG` and starts recording the next photo.
It only reads. To collect a log:

1. XPan ON, 65:24, Live View.
2. *6D capture probe* ON.
3. Take one L JPEG; wait for "photo recorded"; *Save probe log*.
4. Take one RAW+L; wait; *Save probe log*.

RAW-only is not needed (XPan allows only L JPEG and RAW+L anyway).

The log shows which jobs run for each format, their sizes and channels, and
the colour-table job identity. That is enough to design the 6D JPEG crop and
to find out whether there is a YUV stage for grain and the watermark.

## Files

| File | Role |
| --- | --- |
| `src/xpan-6d.c` | 6D stand-ins for the EOS M-only stages, capture probe, 6D menu entries |
| `src/xpan-mode.h` | sensor size and Canon crop records per body |
| `src/xpan-native-look.c`, `src/xpan-workflow.inc` | look hook address and job-block layout per body |
| `src/xpan-hud-controls.c` | 6D key mapping; MPU and touch code is EOS M only |
| `src/cropmarks.c` | OLC dial-state address per body |
| `src/boot-d45-am.c`, `src/arm-mcr.h` | 720 KB reservation for 6D XPan builds |
| `platform/6D.116/` | `XPAN=y` build, `CONFIG_XPAN_BODY`, `set_htp()` |
