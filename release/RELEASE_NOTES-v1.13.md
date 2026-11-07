# XPan v1.13 for the Canon EOS M

A new Split layout for the 65:24 HUD, and a fix for photos that came out
uncropped and without the LUT when Canon's lens aberration correction was
enabled. For the Canon EOS M with firmware 2.0.2.

> [!WARNING]
> **EOS M with firmware 2.0.2 only.** Experimental software that runs inside
> Canon's image pipeline. Use it at your own risk, like any Magic Lantern
> build.

> [!IMPORTANT]
> **Disable all other Magic Lantern modules.** XPan is incompatible with
> almost every module, and this release ships none. Delete old `.mo` files
> from `ML/MODULES/` (or turn every module OFF).

## What's new

### Split layout
**Shoot > XPan > Frame alignment** now offers **Top / Bottom / Split** in
65:24. Split puts the HUD's first row (logo, equivalent focal length and
aperture, mode, clock, card space, battery) in a thin bar above the picture,
the 65:24 Live View in the middle, and the rest of the HUD below it. The AF
frame, grid, touch AF and the Loading box follow the picture. **INFO** in
Live View cycles Top / Bottom / Split.

### Fixed: JPGs without crop or LUT
With **Lens aberration correction > Chromatic aberration** set to
**Enable**, Canon develops stills through a different engine. The saved JPG
was neither cropped to 65:24 nor graded, even though Live View showed the
LUT.

- The 65:24 crop, film grain and watermark now work on both of Canon's
  develop paths.
- While XPan is ON it sets **Chromatic aberration** to **Disable**, because
  that engine skips the colour step that carries the look. Your setting comes
  back when you turn XPan OFF. Peripheral illumination correction is left as
  you set it.
- If Canon refuses the change, the box says **LUT not loaded: Set Chromatic
  aberration correction to Disable**.

> [!TIP]
> On v1.12 or older, set Chromatic aberration correction to **Disable**
> yourself (red camera menu, tab 1) to get the LUT back.

## Upgrading from v1.1 or later

1. Unzip the new release to the root of the card, replacing existing files.
2. Restart the camera.

Coming from v1.0? Also follow the upgrade steps in the
[v1.1 notes](RELEASE_NOTES-v1.1.md) (old sample LUTs and tone bases).

## Installation (new users)

1. Use a card that boots Magic Lantern on your EOS M 2.0.2, or prepare one
   with the included `ML-SETUP.FIR` firmware update.
2. Unzip the release to the **root of the card**.
3. Put the mode dial on the photo (camera) position, not A+.
4. Open the Magic Lantern menu and enable **Shoot > XPan** (SET toggles it,
   PLAY opens its options).

XPan sets the Canon options it needs by itself (shooting mode, Neutral
Picture Style, ALO, HTP, sRGB, image quality, aspect ratio, chromatic
aberration correction). See `README-XPAN.md` for the full manual.

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
| `XPan-EOSM-202-v1.13.zip` | `6a24bf7d14c20aa3b483839330562d2bd4a3939e70e1ef9775f7145a07ccbe61` |

Build `a9c158477` (`xpan-features`), 2026-11-07 19:12 UTC.
