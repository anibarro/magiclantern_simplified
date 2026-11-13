# XPan v1.14 for the Canon EOS M

Film halation on the saved JPEG, and a LUT selector in movie mode that works
while recording. For the Canon EOS M with firmware 2.0.2.

> [!WARNING]
> **EOS M with firmware 2.0.2 only.** Experimental software that runs inside
> Canon's image pipeline. Use it at your own risk, like any Magic Lantern
> build.

> [!IMPORTANT]
> **Disable all other Magic Lantern modules.** XPan is incompatible with
> almost every module, and this release ships none. Delete old `.mo` files
> from `ML/MODULES/` (or turn every module OFF).

## What's new

### Halation
**Shoot > XPan > Halation** (right below Film grain; SET: ON/OFF, PLAY:
**Strength**) adds the red-orange glow that strong highlights get on film
without an anti-halation layer: around lamps, windows and bright lines
against darker surroundings. The highlight itself stays white, and nothing
below the brightest tones changes.

- **Strength** 1-100 %, default 50 %: 30 subtle, 50 medium, 70 strong, 100
  very strong.
- Works with or without Film grain. With both, the glow goes on first and
  the grain over it, as on film; the watermark goes on top.
- Saved JPEG only (not Live View or video), with XPan ON in photo mode. In
  65:24 only the rows the JPEG keeps are processed.

> [!CAUTION]
> **Halation takes several seconds per photo on the EOS M.** It runs on the
> camera's single processor while Canon waits to save the JPEG, so each
> photo takes several seconds longer to save, and burst shooting slows down
> accordingly. Turn Halation OFF when you need speed. The Halation menu help
> shows how long the last photo took.

### Video LUT selector
In movie mode, with XPan ON, Custom LUT on and two or more LUTs on the card,
a **`<  NAME  >`** box sits just above Canon's setting bar.

- Tap the left third of the box for the previous LUT, anywhere else on it
  for the next one.
- It also works **while recording**, so the look can be changed mid-clip.
- Only taps on the box go to XPan; every other touch stays Canon's. The box
  hides while "Loading..." is shown and in menus.

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
| `XPan-EOSM-202-v1.14.zip` | `d0eab5f9ab6541fd7e301e480d0310d7625c81b9bdbb1f4d1a74c5f1a145b0ab` |

Build `998abe28e` (`xpan-features`), 2026-11-13 14:37 UTC.
