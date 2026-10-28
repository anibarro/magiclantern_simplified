# XPan looks

Put your `.cube` files here. An `XPAN=y` build copies every `*.cube` / `*.CUBE`
in this folder to `ML/XPAN/LUTS/` in `magiclantern.zip`. The folder ends up
in the zip even when it is empty, so you can also copy looks to the card
later.

On the camera they appear under **XPan > Custom LUT > Cube file**. After
adding files to the card, use *Refresh Cube list*.

Limits (src/xpan-native-cube.c and src/xpan-cube.c):

* at most 16 files; the list is sorted by name
* file name shorter than 48 characters, printable ASCII only
* `LUT_3D_SIZE` 2..33 (17 recommended: every look is resampled onto Canon's
  10x10x10 table), `DOMAIN_MIN 0 0 0` / `DOMAIN_MAX 1 1 1`, file up to 4 MB
* input is Rec.709 / sRGB, the same as for any display-referred look. With
  *Tone base = Cineon log*, the Cineon base runs first, so the look sees
  Cineon log input.

This README is not copied to the card.
