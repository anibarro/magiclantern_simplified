# XPan v1.1 for the Canon EOS M

A control and compatibility update for XPan on the Canon EOS M
(firmware 2.0.2): the rear wheel and the LEFT key now work properly on the
XPan HUD, and LUTs are found on FAT32 cards.

> [!WARNING]
> **EOS M with firmware 2.0.2 only.** Experimental software that runs inside
> Canon's image pipeline. Use it at your own risk, like any Magic Lantern
> build.

> [!IMPORTANT]
> **Disable all other Magic Lantern modules.** XPan is incompatible with
> almost every module, and this release ships none. Delete old `.mo` files
> from `ML/MODULES/` (or turn every module OFF).

## What's new

### Wheel and LEFT work on the HUD
- **Wheel:** changes the selected HUD value (clockwise = up), on any field:
  ISO, aperture, shutter, WB, LUT and so on. Canon's own exposure no longer
  jumps when you turn it.
- **LEFT / RIGHT:** previous / next HUD field.
- **Half-press** works normally, short or long: it is no longer mistaken for
  LEFT.

In plain Live View the EOS M's button controller normally applies the wheel
to Canon's exposure itself and turns LEFT into a half-shutter press. XPan now
reads both straight from the button controller, before Canon acts on them.
How this was found and how it works is documented in
[`README-XPAN-MPU.md`](README-XPAN-MPU.md).

### LUTs on FAT32 cards
- Fixed **"No .cube files"** and **"CINEON7.CUBE missing"** on cards
  formatted as FAT32 (usually 32 GB or smaller): the camera only sees short
  8.3 file names there, and `.cube` already counts as a long name.
- The sample looks have short names, shown in full on every card:
  `MONO`, `FUJI`, `KODAK`, `CINEON` and `IDENT` (neutral), all still `.cube`.
- The Cineon tone bases are renamed `CIN1EV.cube`, `CIN2EV.cube` and
  `CINLV.cube`.
- Your own LUTs appear on FAT32 too; keep names to at most 6 characters
  before `.cube` to see them in full (longer names are shortened, e.g.
  `Cine Kodak.cube` shows as `CINEKO`). On exFAT cards full names work,
  spaces included, which looks nicer on the watermark.
- A fresh install starts on the neutral `IDENT` LUT.

## Upgrading from v1.0

1. Unzip the new release to the root of the card, replacing existing files.
2. Delete the old sample LUTs from `ML/XPAN/LUTS/` (`B&W.cube`,
   `Cine FUJI.cube`, `Cine Kodak.cube`, `Cineon to rec709.cube`,
   `DEBUG_IDENTITY.CUBE`) and the old tone bases from `ML/XPAN/`
   (`CINEON7.CUBE`, `CINEON7_2EV.CUBE`, `CINEONLV.CUBE`); the new files
   replace them under short names.
3. Restart the camera.

> [!NOTE]
> v1.0 cannot read its tone bases on FAT32 cards; use an exFAT card with
> v1.0, or update to v1.1.

## Installation (new users)

1. Use a card that boots Magic Lantern on your EOS M 2.0.2, or prepare one
   with the included `ML-SETUP.FIR` firmware update.
2. Unzip the release to the **root of the card**.
3. In photo mode, open the Magic Lantern menu and enable **Shoot > XPan**
   (SET toggles it, PLAY opens its options).
4. For looks, set Canon's **Picture Style to Neutral** and turn **Highlight
   Tone Priority** and **Auto Lighting Optimizer** OFF.

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
| `XPan-EOSM-202-v1.1.zip` | *(add when the release zip is built)* |
