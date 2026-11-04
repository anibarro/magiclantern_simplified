# XPan v1.12 for the Canon EOS M

XPan on the Canon EOS M (firmware 2.0.2) now sets every Canon option it
needs by itself, so a LUT loads whatever the camera was set to before.

> [!WARNING]
> **EOS M with firmware 2.0.2 only.** Experimental software that runs inside
> Canon's image pipeline. Use it at your own risk, like any Magic Lantern
> build.

> [!IMPORTANT]
> **Disable all other Magic Lantern modules.** XPan is incompatible with
> almost every module, and this release ships none. Delete old `.mo` files
> from `ML/MODULES/` (or turn every module OFF).

## What's new

### XPan sets the camera up for you
While XPan is ON it takes care of:

- **Shooting mode:** P, Creative Auto and the scene modes (Portrait,
  Landscape, Night...) are switched to **M**, because Canon locks the look
  settings there. v1.11 turned XPan off in those modes instead.
- **Picture Style Neutral**, with Contrast, Saturation and Color tone at 0
  (your Sharpness is kept).
- **Auto Lighting Optimizer** and **Highlight Tone Priority** OFF, colour
  space **sRGB**.
- **Image quality** L JPEG or RAW+L, Canon **aspect ratio** 3:2.

Turning XPan OFF puts your Picture Style, its sliders, ALO, HTP and colour
space back. Only the **A+** position of the mode dial cannot be changed from
software, so XPan is off there.

### Fixed: "LUT not loaded / Waiting for Canon Neutral style"
With ALO set to Standard or Low and **"Disable during manual exposure"**
ticked, Canon runs ALO off in M but keeps the menu choice, and XPan waited
for it forever. XPan now only checks the ALO mode actually in effect.

### Clearer messages
If Canon still refuses a change, the "LUT not loaded" box names the setting
to change by hand, for example:

- `Set the shooting mode to Tv, Av or M`
- `Set Neutral Contrast, Saturation, Color tone to 0`
- `Set Canon Color space to sRGB`
- `Look hook failed: disable all modules`

## Upgrading from v1.1 or v1.11

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

See `README-XPAN.md` for the full manual.

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
| `XPan-EOSM-202-v1.12.zip` | `0998044173b04bb8c20f4079a01d6ec365d3833a2d21c75480d7663a3f85e9fe` |

Build `77aa6fae3` (`xpan-features`), 2026-11-04 13:00 UTC.
