Magic Lantern
=============

Magic Lantern (ML) is a software enhancement that offers increased
functionality to the excellent Canon DSLR cameras.
  
It's an open framework, licensed under GPL, for developing extensions to the
official firmware.

Magic Lantern is not a *hack*, or a modified firmware, **it is an
independent program that runs alongside Canon's own software**. 
Each time you start your camera, Magic Lantern is loaded from your memory
card. Our only modification was to enable the ability to run software
from the memory card.

ML is being developed by photo and video enthusiasts, adding
functionality such as: HDR images and video, timelapse, motion
detection, focus assist tools, manual audio controls much more.

For more details on Magic Lantern please see [http://www.magiclantern.fm/](http://www.magiclantern.fm/)

There is a sibling repo for our patched version of Qemu that adds support
for emulating camera ROMs. This allows testing without access to a physical
camera, and automating tests across a suite of cameras.  
https://github.com/reticulatedpines/qemu-eos  
https://github.com/reticulatedpines/qemu-eos/tree/qemu-eos-v4.2.1 (current ML team supported branch)

---

Live Composite and Live Bulb for the Canon EOS M (livecomp module)
==================================================================

This repository includes **livecomp**, a Magic Lantern module that adds
Olympus-style **Live Composite** and **Live Bulb** to the Canon EOS M
(firmware 2.0.2). The camera keeps taking exposures at the full sensor
resolution (5208 x 3478) and shows the result building up on the screen
after every frame. You decide when it is done and press SET; the camera
saves one RAW DNG file.

The module works with a standard Magic Lantern build for the EOS M 2.0.2.
It does not need any other module or a modified Magic Lantern core.

## The two modes

| | Live Composite | Live Bulb |
| --- | --- | --- |
| What it keeps | The brightest value each pixel has seen | The sum of all frames |
| Moving lights | Leave trails | Leave trails |
| The rest of the scene | Stays at the exposure of one frame, however long you run | Keeps getting brighter, like one long bulb exposure |
| Typical use | Light painting, car light trails, star trails over a fixed foreground, fireworks | A long exposure you want to watch and stop at the right moment: dark landscapes, night scenes, very dim interiors |
| Shutter per frame | 2.5 s to 60 s | 3 s to 60 s |
| Noise reduction | Optional dark frame (one frame long) | Automatic column-stripe correction; optional dark frames (as long as the whole exposure) |
| File name | `DCIM/LIVE/LVFRnnnn.DNG` | `DCIM/LIVE/LVBLnnnn.DNG` |

Rule of thumb: if the scene around your lights is lit (a room, a city
street), use **Live Composite** so the background does not burn out. If the
scene is dark and you want its own light to build up, use **Live Bulb**.

## What you need

- A Canon EOS M with firmware **2.0.2** and Magic Lantern installed.
- A tripod (every frame must line up).
- A charged battery: a session can run for many minutes with the screen on.
- A fast card with about 33 MB free for each picture.

## Installation

1. Copy `livecomp.mo` into the `ML/modules/` folder of your Magic Lantern
   card. Do not replace anything else; keep your `autoexec.bin` and its
   `EOSM_202.sym` as they are.
2. Put the card in the camera and switch it on.
3. Open the Magic Lantern menu (trash button, or tap the screen with two
   fingers), go to the **Modules** tab,
   select **livecomp** and enable it.
4. Restart the camera. The module appears in the **Shoot** menu as
   **Live Composite** (it is called **Live Bulb** when that mode is
   selected).

## Camera setup

- Mode dial on **Movie** mode (the module only works in movie mode; in photo
  mode it does nothing and SET keeps its normal function).
- Exposure mode **M** (manual), a **fixed ISO** (not Auto; ISO 100 gives the
  cleanest result), a fixed **white balance**.
- Normal view (no zoom on screen).
- In Magic Lantern: **FPS override OFF**, and no other RAW tools active
  (crop_rec, raw video, mlv_lite, silent pictures). Global Draw can stay on;
  the module hides it during a session and brings it back afterwards.
- Focus first (manual focus is easiest), then leave the camera untouched.

**ISO and aperture** are set on the camera as usual. **The shutter speed of
each frame** is set in the module's menu, not on the camera.

## The menu (Shoot > Live Composite)

| Item | What it does |
| --- | --- |
| **Live Composite / Live Bulb** | ON arms the module. Nothing starts until you close the menu and press SET. |
| **Mode** | Live Composite (default) or Live Bulb. |
| **Shutter** | The exposure of each frame: 2.5, 3, 4, 5, 6, 8, 10, 13, 15, 20, 25, 30, 40, 50 or 60 s. Frames follow each other with no gap. The right side shows the camera's ISO and aperture. Live Bulb needs 3 s or longer. 2.5 to 6 s, 10, 15 and 50 s have been used on the camera; the menu still marks shutters from 6 s up (except 10 s) as not yet tested. |
| **Preview only** | ON: SET takes one test frame with the current settings and shows it, with the percentage of clipped highlights. Nothing is saved. Use it to get the exposure right, then turn it OFF. |
| **Frame limit** | No limit (default), 30, 60, 120 or 240 frames. The session stops and saves by itself after that many frames. In Live Bulb the right side shows the total exposure, e.g. "= 1m15s bulb". For a single long frame (for example one 50 s bulb), press SET while the first frame is being exposed. |
| **Dark frame** | OFF (default) / ON. After the last frame the camera asks you to cover the lens and takes dark frames with the same shutter, which are then subtracted, like a camera's long exposure noise reduction. Live Bulb: as many dark frames as the picture has (they last as long as the whole exposure). Live Composite: one dark frame. See *Noise reduction* below. |
| **Save unprocessed** | OFF (default) / ON. Also saves the picture without in-camera noise reduction, and the dark frames (with Dark frame ON), so you can process them yourself. Slower: extra card writes. |
| **Debug** | OFF (default): only the DNG is saved. ON: a log file and a CSV file are saved next to each DNG. Turn it on only when you want to report a problem. |
| **Status** | Shows Off / Armed / Starting / Running / Reboot, setup reminders, and the result of the last session. |

## Taking a picture

1. **Check the exposure (optional but recommended).** Set *Preview only* to
   ON, close the menu and press SET. After one shutter time plus about
   1.5 s the test frame appears, for example
   `PREVIEW  4 s  ISO 100  f/8.0  clipped 0.3%`. Press SET to go back. Change
   ISO or aperture on the camera, or the shutter in the menu, and repeat
   until it looks right. Then set *Preview only* back to OFF.
2. **Start.** Close the menu and press and release **SET**. The screen
   switches to the full-sensor view (about 1 s) and shows "Tactical
   delay... first frame in n s": the camera waits about 4 s at a fast frame
   rate before the real exposures start (this keeps a Canon quirk from
   cutting one of your frames, see *Good to know*). Then the first frame
   takes one full shutter time.
3. **Watch it build up.** After every frame the screen shows the whole
   picture so far, with a line such as `COMPOSITE 12  SET=stop` or, in Live
   Bulb, `BULB 12  36s  SET=stop` (frames and total exposure so far).
   While a session runs only SET does anything; the other buttons are
   ignored on purpose.
4. **Stop.** Press **SET** when the picture looks right (or let the frame
   limit stop it). The frame being exposed is finished and added, so
   stopping takes up to one shutter time. SET during the very first frame
   gives a picture of that one frame. Before returning to the normal view
   the camera may need to finish one more frame at the chosen shutter; the
   screen then says "Ending the current frame: up to n s".
5. **Dark frames (only with Dark frame ON).** The screen shows "COVER THE
   LENS: dark frames in 10 s  SET=skip". Put the lens cap on (or cover the
   lens completely) and keep it covered until the screen says "Dark frames
   done: lens can be uncovered". In Live Bulb this takes as long as the
   picture itself ("DARK 3/12  54s  SET=stop"); in Live Composite one
   frame. SET during the countdown skips the dark frames; SET during them
   stops after the current one.
6. **Saved.** The camera returns to the normal view, applies the noise
   reduction ("Applying denoise...") and saves the DNG in `DCIM/LIVE` on
   the card ("Saving the DNG..."). The Status line shows the result, for
   example "Composite saved: 25 frames" or "Bulb saved: 18 frames, 54 s,
   dark subtracted". You can start the next session right away with SET; no
   restart is needed.

## Getting the exposure right

**Live Composite:** expose each frame as you would expose a single photo of
the scene. The background never gets brighter than one frame, so the
shutter mostly decides how bright the scene around the lights is. Keep the
lights themselves from clipping too much with ISO and aperture.

**Live Bulb:** the picture gets brighter with every frame: n frames of t
seconds give the same exposure as one bulb shot of n x t seconds. Choose ISO
and aperture for the **total** time you have in mind, not for one frame.
*Preview only* shows a single frame, so it looks n times darker than the
final picture. The simplest way is to watch the screen and press SET when it
looks right. Highlights that reach white stay white. As in any long
exposure, shadow noise grows with the total time; ISO 100 helps.

## Noise reduction

**Column stripes (Live Bulb, automatic).** Each column of the EOS M sensor
has a small offset of its own. Adding many frames adds those offsets up into
vertical stripes in the shadows. Live Bulb measures them, from the covered
rows at the top of the sensor and from flat dark areas of the picture
itself, and removes them before saving. It needs no setting. The same
correction runs on a Live Composite after its dark frame.

**Dark frames (menu Dark frame).** With the lens covered the sensor still
records hot and warm pixels, the dark current that lifts long exposures, and
amp glow. A dark frame records exactly that, and subtracting it removes it,
as Canon's long exposure noise reduction does:

- **Live Bulb** subtracts as many dark frames as the picture has, with the
  same shutter, so the dark lasts as long as the whole exposure (a
  12 x 6 s bulb is followed by 12 x 6 s of dark frames).
- **Live Composite** keeps each pixel's brightest frame, so the hot pixels
  are in it only once: one dark frame of one shutter time is enough.

Subtracting a dark frame also adds its own random noise: shadows get about
1.4 times noisier, the same trade-off as Canon's long exposure noise
reduction. Without a cap the dark frames are not dark; always cover the lens
completely.

**Save unprocessed.** Also writes the picture before any in-camera noise
reduction and the dark frames, so you can do the subtraction yourself:

| File | Live Bulb | Live Composite |
| --- | --- | --- |
| Final picture (noise reduction applied) | `LVBLnnnn.DNG` | `LVFRnnnn.DNG` |
| Unprocessed picture | `LVBRnnnn.DNG` | `LVCRnnnn.DNG` (only with Dark frame ON) |
| Dark frames (sum / single) | `LVDKnnnn.DNG` | `LVCKnnnn.DNG` |

The camera has memory for two full-size images only, so with Save
unprocessed ON the unprocessed picture is written to the card before the
dark frames and read back afterwards: allow some extra seconds at the end.

## Your pictures

- Files are saved in `DCIM/LIVE/` on the card: `LVFRnnnn.DNG` (Live
  Composite) and `LVBLnnnn.DNG` (Live Bulb), plus the unprocessed and dark
  files of *Save unprocessed* (see above). The folder is created
  automatically.
- The camera's own playback does not show this folder; copy the files to a
  computer.
- The DNG is an uncompressed RAW file (about 32 MB). Open it in any raw
  editor that reads DNG, such as Adobe Lightroom / Camera Raw, darktable or
  RawTherapee. It carries the camera's white balance and, for Live Bulb, the
  total exposure time. The EOS M's focus pixels are already removed.

## Battery

If the battery drops to its last (red) level during a session, the session
stops on its own and the picture is saved while there is still enough power
to write the card ("Battery low: saved n frames").

## Messages and what to do

| Message | Meaning / what to do |
| --- | --- |
| Needs Movie mode | Turn the mode dial to Movie. |
| Auto ISO! (in the Shutter line) | Set a fixed ISO on the camera. |
| A refusal when you press SET (wrong mode, Auto ISO, FPS override on, another RAW tool active) | Nothing was changed. Fix the setting and press SET again. |
| Merge too slow / Too slow for n s frames | The camera could not keep up with the chosen shutter. What was taken so far is saved. Choose a longer shutter. |
| Bulb saved: n frames; use longer frames | Live Bulb stopped early because the shutter was too short. Use 3 s or longer. |
| Saved; stopped early, see LOG | Something interrupted the session; the picture so far was saved. Turn Debug ON and repeat if it happens again. |
| Tactical delay... first frame in n s | Normal start-up pause (about 4 s plus one shutter). Wait. |
| Ending the current frame: up to n s | After SET the sensor finishes a frame before the camera can return to the normal view. Wait. |
| COVER THE LENS: dark frames in n s | Dark frame is ON: cover the lens now and keep it covered (SET skips the dark frames). |
| ..., dark partial / dark failed | Fewer dark frames than needed were taken (SET, or a camera problem); the picture is saved with what was taken / without dark subtraction. |
| Dark subtraction failed: see LVBR (LVCR) | With Save unprocessed, the unprocessed file could not be read back. That file is your picture; nothing else was saved. |
| Status says **Reboot** / "DMA uncertain: reboot camera" | Switch the camera off and on before the next session. This is a safety stop; nothing is damaged. |

If the camera ever stops responding, take the battery out and put it back.
To report a problem, turn **Debug** ON, repeat the session, and include the
`.LOG` and `.CSV` files from `DCIM/LIVE`.

## Good to know

- Shutters shorter than 2.5 s (3 s in Live Bulb) are not possible: adding
  each full-resolution frame takes the camera's processor about 1.5-2.3 s.
- Canon's Live View aborts the RAW transfer of one frame a few frames after
  the camera enters the full-sensor mode (always the same frame). Before
  version LCM62 this cut one real frame short (a gap in light trails, one
  frame missing in Live Bulb). The start-up "tactical delay" now lets that
  happen on throwaway frames; with Debug ON the LOG records each abort
  (`Abort_hook` line).
- The DNG contains the full sensor area, with the usable image area marked
  for your raw editor.
- More detail on the module, its design and its test history is in
  `modules/livecomp/` (INSTALL.txt, HOW-IT-WORKS.md, FULLRES.md).
