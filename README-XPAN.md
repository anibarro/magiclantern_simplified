# XPan for the Canon EOS M (firmware 2.0.2)

XPan turns the EOS M into a panoramic camera inspired by the Hasselblad XPan:
65:24 framing, a dedicated photo HUD, Canon-native JPEGs with a custom 3D LUT
look, film grain, an optional watermark, and the same LUT in movie mode.

Everything is built on Magic Lantern and runs inside Canon's own pipeline:
the JPEG is still encoded by Canon's hardware, XPan only changes the crop,
the colour tables and a few properties on the way. No extra DryOS task is
created and no image buffer is reserved at boot.

**Only the EOS M 2.0.2 is supported.** Use at your own risk, like any Magic
Lantern build.

> **Disable all external modules.** XPan changes Canon's colour tables, JPEG
> encoder, sensor crop, key routing and several properties. These changes make
> it incompatible with almost every other Magic Lantern module (`.mo` files
> in `ML/MODULES/`). Before using this build, go to the **Modules** tab, set
> every module to OFF and restart the camera. Better still, start from an
> empty `ML/MODULES/` folder. Do not load raw video, crop, Dual ISO, Lua or
> other modules together with XPan. They can conflict with it, corrupt images
> or make the camera lock up.

## Features

Photo mode (menu **Shoot > XPan** for framing and HUD, **XPan** tab for look, grain and watermark):

* **Aspect ratio** 65:24 or 3:2. In 65:24 the JPEG is cropped by Canon's
  own encoder (full sensor width, 65:24 height). RAW files keep the full
  3:2 sensor frame.
* **Frame alignment** top or bottom, so the free band can hold the HUD.
* **Photo HUD**: shutter, aperture, ISO, EV, WB, drive, meter, LUT, format,
  battery, time and card space, in four colour themes (Orange, Silver, Red,
  Blue) and with an optional rule-of-thirds grid.
* **Controls**: the wheel and arrow keys change the selected HUD field; touch
  only selects fields (tapping the already selected WB field steps the WB
  presets). With Kelvin WB, UP/DOWN change the temperature.
  A short press of SET cycles Canon's focus magnification 1x / 5x / 10x.
* **Custom LUT**: any `.cube` file from `ML/XPAN/LUTS/` (up to 33^3), with
  strength. It is converted into Canon's native colour table, so the look is
  applied by the camera itself, to the JPEG and to Live View.
* **Tone base / Highlight headroom**: *Canon Neutral*, or *Cineon log* with
  +1 or +2 EV of extra highlight headroom. The Cineon base under-exposes
  Canon's JPEG development by 1 or 2 EV (JPEG_OBWB gain) and maps it back
  through a calibrated log base (`ML/XPAN/CINEON7*.CUBE`) before the custom
  LUT, so highlights that Canon would clip survive into the LUT.
* **Film grain**: *Film* (soft, brightness-dependent, computed on the CPU)
  or *Canon* (Canon's native Grainy filter engine); three sizes, strength.
  Added after the LUT, on the saved JPEG only.
* **Watermark**: a logo in one of the four corners, small (386 px) or big
  (776 px wide), from `ML/XPAN/WMARK.BIN`.

Movie mode:

* **XPan ON/OFF** switch for movie mode, and the same Custom LUT on the
  recorded H.264 and on Live View.
* **1:1 crop**: SET in standby toggles a 1920x1080 1:1 sensor crop (about 3x),
  using Magic Lantern's crop-mode hack. It cannot be switched while recording,
  because the sensor readout has to restart.

## Making your own LUTs

Looks are ordinary 3D `.cube` files, so they can be made in DaVinci Resolve
or any other grading tool that exports `.cube` LUTs.

* **Input.** With *Tone base = Cineon log* (the default), XPan first converts
  Canon's image to Cineon log, so build the look for footage in
  **Rec.709 colour gamut with Cineon Film Log gamma**. In Resolve, for
  example, use a Color Space Transform or set the clip's input to
  Rec.709 / Cineon Film Log. With *Tone base = Canon Neutral* there is no log
  step, and the look receives normal Rec.709 (display) images instead.
* **Output.** Rec.709 display, the same as a normal JPEG.
* **Size.** Export a **17-point** cube (`LUT_3D_SIZE 17`, Resolve's
  *Generate LUT > 17 Point Cube*). Any size from 2 to 33 is accepted, so
  33-point files work too, but they take longer to load and are no more
  accurate. 65-point files are rejected.
* **How it is applied.** Canon's picture-style colour table has a 10x10x10
  grid, and XPan resamples every LUT onto it. A look is therefore smoothed
  slightly. Very narrow corrections, such as a key on a single hue, come
  out softer than in Resolve.
* **Format limits.** One 3D LUT per file: no 1D or shaper LUTs, and
  `DOMAIN_MIN 0 0 0` / `DOMAIN_MAX 1 1 1` with output values between 0 and 1.
  At most 16 files, with plain ASCII names under 48 characters.

Copy the files to `ML/XPAN/LUTS/` and use *Refresh Cube list*, or restart the
camera.

## Card layout

The XPan build puts everything in `magiclantern.zip`. Each file comes from
`data/XPAN/` in the repository:

```
ML/XPAN/CINEON7.CUBE       tone base for +1 EV headroom
ML/XPAN/CINEON7_2EV.CUBE   tone base for +2 EV headroom
ML/XPAN/CINEONLV.CUBE      Live View tone base, 17^3
ML/XPAN/WMARK.BIN          watermark logo (from extras/XPan_tools)
ML/XPAN/GRAIN.BIN          grain texture for Grain type = Canon
ML/XPAN/LUTS/              your looks: every *.cube in data/XPAN/LUTS/
```

The zip contains **no modules**: `ML/MODULES/` holds only the symbol file
(`EOSM_202.sym`). Unzipping does not delete modules already on the card, so
delete any old `.mo` files from `ML/MODULES/` yourself (see the warning above).

`ML/XPAN/LUTS/` is created even when the repository folder holds no looks.
The camera reads these files once, when a feature first needs them, so
restart it after copying new ones (or use *Refresh Cube list* for looks).

`GRAIN.BIN` is the 20 KB seed/offset texture of Canon's own grain filter,
captured once on an EOS M. Grain type *Film* does not use it.

## Building

Requirements: `arm-none-eabi-gcc` (12.x tested), `make`, `python3` with
`docutils` (for module help strings), `zip`.

```
build_tools/build_xpan.sh                  # Linux / WSL
ARM_BINPATH=/opt/gcc-arm/bin build_tools/build_xpan.sh
RUN_XPAN_BUILD.cmd                         # Windows, through WSL
```

This is `make -C platform/EOSM.202 XPAN=y CONFIG_QEMU=n` with a separate
build directory (`platform/EOSM.202/build_xpan`). Unzip
`build_xpan/magiclantern.zip` to the root of the card.

Without `XPAN=y` an EOS M build keeps only the basic 65:24 framing overlay
and HUD (Shoot > XPan); the native look, JPEG crop, grain and watermark are
behind `CONFIG_XPAN`. Other cameras are unaffected (`CONFIG_EOSM`).

## Tools

* `extras/XPan_tools/`: watermark source. `logo.png` is the PNG (with
  transparency) and `xpan_watermark.py` converts it into `WMARK.BIN`
  (386 and 776 px wide, BT.601 YCbCr + alpha, pixel pairs). `run.bat` (or
  `python xpan_watermark.py logo.png --out ../../data/XPAN/WMARK.BIN --preview`)
  rewrites `data/XPAN/WMARK.BIN`, so the next build packages the new logo.
  Requires `pip install pillow numpy`.
* `build_tools/xpan_cineon_base_v7.py`: regenerates the Cineon tone bases from
  the calibration model in `build_tools/*.npy`:

  ```
  python3 xpan_cineon_base_v7.py xpan_cineon_model_v7.npy xpan_cineon_corr_v7.npy CINEON7.CUBE 1 xpan_cineon_stepcomp_v8.npy
  python3 xpan_cineon_base_v7.py xpan_cineon_model_v7.npy xpan_cineon_corr_v7.npy CINEON7_2EV.CUBE 2 xpan_cineon_stepcomp_v7.npy
  ```

## Source map

| File | Role |
| --- | --- |
| `src/cropmarks.c`, `src/xpan-photo-hud.inc`, `src/xpan-mode.h` | XPan menu, framing, HUD drawing, grid, themes |
| `src/xpan-hud-controls.c` | key / wheel / touch routing for the HUD, SET zoom, WB/Kelvin |
| `src/xpan-native-look.c`, `src/xpan-workflow.inc` | look menu and state machine (LUT, tone base, headroom, movie) |
| `src/xpan-cube.c`, `src/xpan-lut.c`, `src/xpan-native-cube.c`, `src/xpan-native-table.c` | `.cube` parsing, interpolation and conversion into Canon's native colour table |
| `src/xpan-live-native.c`, `src/xpan-live-neutral*.inc` | the same table on the Live View / movie path |
| `src/xpan-live-color-probe.c` | guarded hooks at Canon's LCD colour-transfer writes, used by the above |
| `src/xpan-native-jpeg.c` | 65:24 crop of Canon's JPEG encoder and JPEG/CR2 metadata |
| `src/xpan-film-grain.c` | film grain through Canon's grain engine, watermark blend |
| `src/property.c`, `src/propvalues.c`, `src/shoot.c`, `src/gui-common.c`, `src/tweaks.c`, `src/zebra.c`, ... | small hooks: property allow-list, key routing, display filter, 1:1 movie crop |
