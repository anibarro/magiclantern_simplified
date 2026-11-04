# XPan v1.11 for the Canon EOS M

A fix release for XPan on the Canon EOS M (firmware 2.0.2): no more endless
"Loading..." when a LUT cannot load.

> [!WARNING]
> **EOS M with firmware 2.0.2 only.** Experimental software that runs inside
> Canon's image pipeline. Use it at your own risk, like any Magic Lantern
> build.

> [!IMPORTANT]
> **Disable all other Magic Lantern modules.** XPan is incompatible with
> almost every module, and this release ships none. Delete old `.mo` files
> from `ML/MODULES/` (or turn every module OFF).

## What's fixed

### "Loading..." that never goes away
Before it builds a LUT, XPan sets a few Canon options: Picture Style
Neutral, Highlight Tone Priority and Auto Lighting Optimizer OFF, colour
space sRGB, image quality L JPEG or RAW+L, aspect ratio 3:2. When Canon
refused one of them, XPan kept waiting and the "Loading..." box stayed on
screen forever.

Now, if the LUT is still blocked after a few seconds, the box says
**LUT not loaded** and what to change, for example:

- `Set Canon Picture Style to Neutral.`
- `Turn Highlight Tone Priority OFF.`
- `Set image quality to L JPEG or RAW+L`
- `Look hook failed: disable all modules`

### XPan only in P, Tv, Av and M
The most common cause was shooting in **Creative Auto** or a **Scene mode**
(Portrait, Landscape, Night, HDR...), where Canon locks Picture Style, ALO,
colour space and RAW. XPan now turns itself off in those modes, as it already
did in green Auto and A+. Switch to P, Tv, Av or M to use it.

## Upgrading from v1.1

1. Unzip the new release to the root of the card, replacing existing files.
2. Restart the camera.

Coming from v1.0? Also follow the upgrade steps in the
[v1.1 notes](RELEASE_NOTES-v1.1.md) (old sample LUTs and tone bases).

## Installation (new users)

1. Use a card that boots Magic Lantern on your EOS M 2.0.2, or prepare one
   with the included `ML-SETUP.FIR` firmware update.
2. Unzip the release to the **root of the card**.
3. Set the camera to **P, Tv, Av or M**.
4. Open the Magic Lantern menu and enable **Shoot > XPan** (SET toggles it,
   PLAY opens its options).
5. For looks, set Canon's **Picture Style to Neutral** and turn **Highlight
   Tone Priority** and **Auto Lighting Optimizer** OFF (XPan also does this
   itself).

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
| `XPan-EOSM-202-v1.11.zip` | `39f35081bb75cfb2bbb790115ccffb7e6ffdc9a305628c49e634095455f8dd58` |

Build `4463590e0` (`xpan-features`), 2026-11-04 10:02 UTC.
