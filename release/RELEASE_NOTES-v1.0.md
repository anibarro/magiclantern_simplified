# XPan v1.0 for the Canon EOS M

The first release of **XPan**, a Magic Lantern build that turns the original
Canon EOS M (firmware 2.0.2) into a panoramic camera inspired by the
Hasselblad XPan: 65:24 framing, a dedicated shooting HUD, custom 3D LUT looks
applied by the camera itself, film grain and an optional watermark.

> [!WARNING]
> **EOS M with firmware 2.0.2 only.** This is experimental firmware software
> that runs inside Canon's own image pipeline. Use it at your own risk, like
> any Magic Lantern build.

> [!IMPORTANT]
> **Disable all other Magic Lantern modules.** XPan changes Canon's colour
> tables, JPEG encoder, sensor crop, key handling and several properties, so it
> is incompatible with almost every module. This release ships **no modules**.
> If your card has an older Magic Lantern install, delete the `.mo` files in
> `ML/MODULES/` (or turn every module OFF) before using XPan.

## Highlights

### Panoramic shooting
- **65:24 or 3:2** framing. In 65:24 the JPEG is cropped by Canon's own
  encoder to the full sensor width (5184 × 1920). RAW files keep the full
  3:2 frame.
- **Frame alignment** top or bottom, leaving the free band for the HUD.
- **Photo HUD** with shutter, aperture, ISO, EV, white balance, drive, metering,
  LUT, format, battery, time and card space; four colour themes and an
  optional rule-of-thirds grid.
- **Direct controls:** the wheel and arrow keys change the selected HUD field,
  touch selects fields, and a short press of SET cycles focus magnification
  1x / 5x / 10x.

### Looks (Custom LUT)
- Any `.cube` file from `ML/XPAN/LUTS/` (up to 33³), with adjustable strength,
  converted into Canon's native colour table, so the look appears in
  **Live View, the saved JPEG and movie mode**.
- **Cineon log tone base** with +1 or +2 EV of extra highlight headroom, so
  highlights Canon would clip survive into the LUT.
- Changing LUTs is instant to browse: the name follows the wheel while a
  centred **"Loading..."** box shows that the camera is preparing it.
- Four sample looks included: *B&W*, *Cine FUJI*, *Cine Kodak* and
  *Cineon to rec709* (plus a neutral *DEBUG_IDENTITY* reference).

### Film grain
- *Film* (soft, brightness-dependent) or *Canon* (Canon's own Grainy filter
  engine), three sizes, adjustable strength. Applied to the saved JPEG only.

### Watermark
- XPan logo in **Orange** or **White**, small (386 px) or big (776 px),
  top-left or bottom-left.
- Optional **LUT name** next to the logo, in the logo's colour, set in the
  OneSlot typeface.

### Movie mode
- The same Custom LUT on Live View and on the recorded H.264.
- **1:1 crop** (1920 × 1080 from the sensor centre, about 3x): press SET while
  not recording.

## Installation

1. Use a card that already boots Magic Lantern on your EOS M 2.0.2, or prepare
   one following the usual Magic Lantern instructions (the `ML-SETUP.FIR`
   firmware update is included in the zip).
2. Unzip `XPan-EOSM-202-v1.0.zip` to the **root of the card**, replacing
   existing files.
3. Delete any old `.mo` files from `ML/MODULES/` (see the warning above).
4. Put the camera in photo mode, open the Magic Lantern menu and enable
   **Shoot > XPan** (SET toggles it, PLAY opens its options).
5. For looks, set Canon's **Picture Style to Neutral** and turn **Highlight Tone
   Priority** and **Auto Lighting Optimizer** OFF.

## Menu overview

**Shoot > XPan** (SET: ON/OFF, PLAY: options)

| Row | SET | PLAY opens |
| --- | --- | --- |
| Custom LUT | ON / OFF | Cube file, Refresh Cube list, Cube strength, Tone base, Highlight headroom, Live LUT preview |
| Film grain | ON / OFF | Grain strength, Grain type, Grain size |
| Watermark | ON / OFF | Logo colour, Position, Size, LUT name |

plus Aspect ratio, Frame alignment, capture format and quality, HUD,
grid, colour theme and status options.

## Making your own LUTs

Looks are standard 3D `.cube` files from DaVinci Resolve or any similar tool:

- **Input:** Rec.709 gamut with **Cineon Film Log** gamma (with the default
  *Tone base = Cineon log*). With *Tone base = Canon Neutral* the input is
  plain Rec.709.
- **Output:** Rec.709 display.
- **Size:** 17-point recommended; 2 to 33 points are accepted (65-point files
  are not). Every LUT is resampled onto Canon's 10 × 10 × 10 table, so very
  narrow corrections come out slightly softer than in Resolve.
- Up to 16 files with plain ASCII names. Copy them to `ML/XPAN/LUTS/` and use
  *Refresh Cube list*.

## Known limitations

- EOS M (first generation) with firmware 2.0.2 only.
- Grain, watermark and the 65:24 crop apply to JPEGs; RAW files are untouched.
- Canon Creative Filters must be OFF for film grain.
- Not compatible with other Magic Lantern modules.
- Files copied to `ML/XPAN/` are read once per boot: restart the camera after
  replacing tone bases, logos or the font (new LUTs only need
  *Refresh Cube list*).

## Building from source

Branch `xpan-features`, with `arm-none-eabi-gcc` (12.x tested), `make`,
`python3` + `docutils` and `zip`:

```bash
build_tools/build_xpan.sh
```

On Windows, `RUN_XPAN_BUILD.cmd` runs the same build through WSL. The result is
`platform/EOSM.202/build_xpan/magiclantern.zip`. See `README-XPAN.md` for the
full manual.

## Credits and licences

- Built on [Magic Lantern](https://github.com/reticulatedpines/magiclantern_simplified)
  (GPL).
- OneSlot typeface by Darren Rigby (Starving-4 Entertainment, 2003),
  distributed under the [Design Science License](http://dsl.org/copyleft/dsl.txt).
- XPan is an independent project, not affiliated with Canon, Hasselblad or
  Fujifilm.

## Download

| File | SHA-256 |
| --- | --- |
| `XPan-EOSM-202-v1.0.zip` | `49cca533f227ee53185d458b71c2dd7d9165a6194932dfd877bef07cebdf1102` |

Build `c65ea6400` (`xpan-features`), 2026-10-28 23:33 UTC.
