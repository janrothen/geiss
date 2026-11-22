# macOS Port Design Brief

## Product direction

Build a modernized Geiss visualizer for Apple Silicon Macs. Preserve the recognizable visual style while updating the rendering and the macOS experience.

## Initial product shape

- Start as a standalone macOS app.
- Keep the visualization engine as a library with no SwiftUI and no permission code, so a future screen saver can be another host. The app owns audio capture. This prototype does not design the screen saver's permission model.
- On launch, open the visualizer immediately in a normal window on the main display. The picture fills that window. The window opens at 1280×720, is resizable, and has a minimum size of 640×360. Fullscreen uses the system fullscreen control. There is no separate start action.
- Multi-display support can be considered later.
- No music playback controls are needed.

## Platform target

- Apple Silicon only.
- Use macOS 14.2 as the initial minimum deployment target because Core Audio taps are available from macOS 14.2.
- Raise the minimum version if required by the chosen rendering or other APIs.
- No fallback support for older macOS versions is required.
- The app is not sandboxed. Local builds use automatic signing. The bundle identifier is `com.janrothen.geiss`.

## Language and module boundaries

- New visualization and audio-analysis code is portable C++ that follows the original formulas.
- Leave the legacy Windows sources in the tree, and keep them out of the macOS build. The warp in `proc_map.cpp` is MSVC x86 assembly, and the surrounding algorithms sit in Win32 and DirectDraw code.
- SwiftUI provides the window, overlay, and menus.
- Metal presents the frame.
- One Objective-C++ bridge is the only place that includes both Cocoa and the engine headers. Swift does not include C++ headers.
- Truecolor (user decision): the simulation buffer stores red, green, and blue per pixel, as the original's recommended 32-bit mode did, in four bytes so rows stay aligned. Warp, darkening, the waveform, and the effects write those channels. The original's 8-bit palette path and its palette generator are not ported. The engine expands the buffer to RGBA8 at the boundary. Swift creates the Metal texture and uploads it. Metal scales that texture into the window.

## Audio behavior

- React to system audio output, including music, notifications, and other sounds.
- Capture with a non-exclusive stereo Core Audio tap of the default output mix (`CATapDescription` and a private aggregate device). Playback continues. Recreate the tap when the default output device changes. If that device tap cannot be created, try one global stereo tap. A failure of both keeps calm motion and shows the settings overlay.
- `NSAudioCaptureUsageDescription` supplies the system prompt. There is no public preflight API for this permission; the prompt appears when the tap starts. macOS asks once. A declined app is not asked again, and it is not listed in System Settings until it is allowed or added with +. The switch that controls the tap is Privacy & Security → Screen & System Audio Recording → **System Audio Recording Only**; the screen-and-audio list above it does not affect it. An app gets an entry there only when it runs from an Applications folder, so a local build run from its build folder has nothing to switch on. (Observed on macOS 27.2, November 2026.)
- Split the work into three pieces:
  - Capture, in Swift, copies PCM and returns from the audio callback. The callback converts the tap's 32-bit float stream to stereo straight into a preallocated ring. It does not allocate, wait on a lock, or log. If the reader is copying from the ring at that moment, the callback drops that block. Other tap formats are reported as a capture failure.
  - Analysis, in portable C++, turns that PCM into one immutable frame: samples, level, peak, volume and its average, silence, time, and the beat state (bass, beat mode, beat brightness, and big-beat ratio).
  - The renderer reads only that frame.
- A stalled consumer keeps the latest frame and drops older audio.
- The frame carries one mono sample per simulation column, and a left and a right sample on that same grid, resampled from the tap rate into about −1…1. A one-channel block copies its sample into both sides. Level is the RMS of the mono samples. Silence means that level has stayed low for about half a second of wall time, measured between analyzed blocks rather than from their length, because successive snapshots of the ring overlap. The columns span columns / 88200 s of the newest audio, about 11 ms at 960 columns, as the original read one stereo frame for every two columns at 44.1 kHz. The span does not depend on how long the analyzed block is, so the tap's 2048-frame snapshot and Visual > Tone draw the same line. Following the original's level trigger, the window starts at the newest rising zero crossing that leaves a whole window, searched back up to one window, so the waveform holds in place. Mono, left, and right share that window. Intentional difference: the trigger does not match height and slope with the previous frame. Visual > Tone is a 220 Hz sine delivered as a 2048-frame block, like a tap snapshot.
- The engine runs on the main thread. The capture callback only copies samples. The warp and the map build split the frame into 16 bands of rows, and the bridge hands those bands to Grand Central Dispatch, so they run on several cores while the main thread waits for them and takes a share. The engine itself starts no threads: without a host it runs the bands in order, and the picture is the same either way, because each band writes its own rows and the warp's random rounding depends only on the pixel and the frame.
- Waveform brightness follows RenderWave: `base = volume*4 + averageVolume*0.4 - 10`, clamped to 0…155, where volume is the peak-to-peak of the newest 1/30 s on the original's 0…256 scale (after sensitivity) and the average smooths it like `avg_vol`. The wave color (see [Sound-synced color](#sound-synced-color)) scales that base per channel, and the display doubles it, so normal listening levels draw at full brightness and quiet passages dim. Beats scale it further when a beat is on (see [Beat response](#beat-response)).

### Beat response

- Detection is bass-focused. Two one-pole low-pass stages at 150 Hz run across each block, and the bass level is the RMS of its newest 1/30 s. Intentional difference: the original measured the composite volume.
- The detector ports the original's beat logic (`main.cpp`: beat mode, the beat-mode brightness, and `bBigBeat`). A smoothed bass level feeds a history of 120 samples written at a fixed 30 per second of elapsed time, so frame rate and overlapping blocks do not change it.
- Beat mode turns on when that history is evenly bumpy, using the original's strength measure and its 109/71 hysteresis. A quiet history (smoothed bass below 0.01 RMS) is never in beat mode. A steady tone, hi-hats alone, or a sustained bass note stay out of beat mode.
- In beat mode, the waveform brightness scales from 0 at the average bass to full at half a standard deviation above it, so the line flashes on hits and goes dark between them, as the original's `brite_scale` did. The X-Y oscilloscope keeps its steady brightness, as in the original.
- When the 20-second timer is due in beat mode, the new look waits for a big beat: the smoothed bass over the loudest of the previous 4/3 s, against a threshold that starts at 1.10. Intentional difference: the threshold relaxes by 0.2 over a 5-second wait instead of the original's 18 seconds, and the look changes when the wait ends. A steady beat therefore lands the change on a hit after about 1.3 s, and a louder hit lands it at once.
- Lock still holds the look. Next still switches at once. Pause freezes the timer and the wait.
- The slide shift ports the original's shifting (`main.cpp`: `slider1`, `g_ShiftMaxVol`, `g_FramesSinceShift`) in `engine/src/shift.cpp`. Each look allows it with the original's default chance of 33%. While a look allows it and beat mode is on, a bass peak above the tracked peak raises that peak to 5% above itself, and, when more than 2 reference frames without a peak have passed since the last shift, moves every warp source point by a new offset: 0, 1, or 2 pixels sideways at 640 wide (`((rand() % 320) + 50) / 145`), alternating direction, and one row up, down, or not at all. Both scale with the width. Between peaks the tracked peak relaxes toward 1.43 times the bass history's average at 0.975 per reference frame. The offset applies once per reference frame, whatever the map's damping, so the picture drifts that way until the next peak.
  - Intentional difference: peaks follow the bass level that drives beat mode, instead of the composite volume.
  - Intentional difference: the original flipped the sideways direction when its whole offset, vertical part included, was positive. Here only the sideways part counts, so the direction always alternates, as its comment intended.
  - Intentional difference: the original offset a flat pixel pointer, so a sideways shift spilled into the neighboring row at the edges. Here columns wrap and rows clamp, as for the maps.
  - As in the original, the drift holds when beat mode ends or the sound stops, and only a new look that does not allow the shift clears it. A new look that allows it keeps the previous drift until its first shift. Lock keeps the shift following peaks. Pause freezes it. Whether a drift that continues through silence is calm enough is open.
- There is no beat control.
- A denied tap has no OS error. It is created and started normally and delivers callbacks of exact zeros. See [When audio permission is missing](#when-audio-permission-is-missing).

## Rendering

- Preserve the existing visualization algorithms and their visual intent in portable C++ where practical.
- HDR highlights (user decision): on a screen with extended dynamic range, such as the Pro Display XDR or Studio Display XDR, the brightest parts of the picture glow above SDR white. It is on by default and has no setting. The engine puts each pixel's highlight level in the RGBA8 buffer's alpha. The display doubles each stored channel, as the original's truecolor blit did (`min(255, 2c)`), so a channel at 128 already shows at full SDR brightness. The highlight level rises from 0 to 255 as the pixel's brightest stored channel goes from 128 to 208, so only the hottest cores glow: fresh, loud lines, beat hits, and the brightest effects. Both thresholds are assumptions to judge by eye. Fixed textures are held to SDR (user decision): mode 6's edges (up to 237 per channel), the grid's pulse (up to 170), and crystals (150) keep the original's truecolor levels and show at full brightness, as they did in the original, but never glow. Each pixel's fourth byte, unused in the original, holds the held level: how much of its brightness came from such a texture. Edges set it to the pixel's brightest channel, and the grid and crystals raise it to their level when that sets the pixel's brightness, so a brighter color underneath keeps its glow. The warp carries it with the same weights, darkening, and rounding as the colors, so a texture's trail keeps its share. The highlight level is scaled by the share of the brightest channel that is not held. A waveform point that brightens a pixel, a chaser, and a dot set the pixel outright and clear the held level, so they glow over a texture; additive effects (solar, shade bobs, nuclide, spectral dots, the bar) leave it, so they glow only by what they add. White chasers and the solar flares' cores still glow. The presenter draws into a 16-bit float drawable, uploads the buffer to an sRGB texture so the texture unit decodes the colors before filtering them, and multiplies levels from 80% to the top by up to the highlight gain, eased in with a smoothstep. Each texel gets its own boost before the boosts are filtered like the colors, so a magnified line glows across its whole width and fades at its edges. The gain is 2.5 times SDR white or the screen's current headroom, whichever is lower, read every frame because the headroom follows the brightness setting. A screen counts as HDR, and asks for EDR, only when its potential headroom reaches the 2.5 cap. Many SDR Mac screens offer some headroom below full brightness by raising the backlight; asking for EDR there would dim the rest of the screen and cost power for a faint glow, so those screens draw without highlights (an assumption to check on real non-XDR and external HDR screens). The drawable is tagged with an extended linear copy of the screen's own color space, checked every frame, which keeps the colors of the earlier unmanaged 8-bit output on any screen; a screen profile with no linear form falls back to extended linear Display P3 on a screen that can show P3 and extended linear sRGB on any other. The colors are still decoded with the sRGB curve, so a screen whose profile uses another curve shows slightly different shadows than the unmanaged output did. Whether 2.5 is the right cap is judged by eye.
- In the first version, Metal uploads the RGBA8 buffer and scales it. A frame the engine reports as unchanged, such as a paused one, is drawn again from the last texture without an upload. The expansion doubles each channel and computes the highlight level through two 256-entry tables. The warp does three channels per pixel with one set of bilinear weights. A GPU warp can be considered later if CPU cost calls for it.
- Each tick draws the look's effects into the previous frame, warps it through the current map, darkens it, draws the waveform, and then expands it to RGBA8.
- Presentation follows the display (user decision): a display link ticks once per refresh, up to the highest rate of the screen the window is on, such as 120 Hz on a ProMotion display, and asks for no less than 30 Hz. While paused the picture does not move, so the link asks for about 15 Hz and each tick redraws the last texture, which keeps the HDR gain and the screen's color space current; the info readout's FPS shows that lower rate. Simulation time is elapsed seconds, measured between the display link's target times, the moments each frame will be shown, so the steps stay even when a callback runs late. A large gap after sleep or wake is capped so the picture does not jump. At 960 × 540 a tick takes about 0.5 ms with the bands spread over the cores and about 2 ms on one core in an optimized build, and a look change adds about 1 ms for its map (up to 4.5 ms on one core, for mode 6). An unoptimized engine takes about 31 ms a tick, which caps the picture near 30 fps. Debug builds therefore compile the C++ engine and bridge with `-Os`, as Release does. Swift stays unoptimized in Debug.
- The warp rounds each pixel up or down at random, in proportion to its fraction, with a new pattern every tick. Intentional difference: the original truncated its weighted sum. With plain rounding, a tick's darkening at 60 Hz or 120 Hz is under half a level for most values, so it rounded away and trails stopped fading: after 6 s of silence, map 25 kept an average of 29 levels at 120 Hz instead of 0. Random rounding keeps the darkening's expected value, so the picture fades the same way at any rate. It adds a faint grain to the dimmest trails.
- At higher rates the waveform is drawn more often while there is sound, so its trails are denser and brighter than at 30 fps, and each refresh resamples the picture through the map. Whether 120 Hz looks softer than the original is checked by eye. If it does, the simulation can step at a fixed 60 Hz while presentation follows the display.
- Simulation resolution is independent of the display. The longest side is capped at 960, and the aspect ratio follows the window.

### Sound-synced color

The original's "sync color to sound" option (`g_bSyncColorToSound`) is always on, with no control (user decision). The original had it off by default; with it off, the truecolor wave took only the slow drift below.

- Spectrum: the analyzer ports the original's colour spectrum (`FOURIER_DETAIL`, `g_power`, `g_power_smoothed` in `main.cpp`). It is a direct Fourier sum, not an FFT, over the original's 5.8 ms of the left channel from where the wave window starts (256 samples at 44.1 kHz, 279 at 48 kHz; intentional difference: the original read 256 samples whatever the rate), after the original's smoothing of each sample with the next (0.8 and 0.2) and with the mean taken out. It measures 23 bands centered at `20·2^(10n/24)` Hz for n = 1…23, about 27 Hz up to 15.3 kHz; band 0 is never measured, as in the original. Each band keeps 0.94 of its smoothed value per reference frame and takes 0.06 of the new one. Inherited limit, kept for the original's look: bands 1…7 (up to about 150 Hz, most of red) finish less than one cycle in 5.8 ms, so they barely tell bass frequencies apart and also rise with low-middle sound. A longer window would make red follow the bass more closely; whether to change that is an open question.
- Wave color (`RenderWave`): red sums bands 0…7, green 7…14, and blue 13…23 (the original's integer loop counters made those ranges, so bands 7, 13, and 14 count twice), weighted 0.93, 1.18, and 2.40, and red loses 0.4 of blue. That color is scaled to the wave's brightness, mixed 97% with 3% of the drift color (swings of 0.3 and 0.2), and raised by 1.35, each channel clamped to 0…255. So bass draws red, the middle green, and treble blue, as the original's 4.20 notes say.
- 256 samples are about 5 ms at 48 kHz, which resolves only about 190 Hz, so the bass bands blur together and a bass note's color depends on the phase the window catches, as in the original. Deep bass and kick drums read red, and a note around 80 Hz can read green.
- Drift: six rates of 0.02…0.03 per reference frame and a phase of 0…39999 reference frames (`gF`, `chaser_offset`), picked once per engine run as `FX_Init` did, from a generator of their own so the looks are unchanged. Without spectrum power, in silence or for the idle wave, the wave takes the drift color alone (swings of 0.55 and 0.50, from the original's 4.26), each channel 0.36…2.49 times the brightness before the clamp.
- Nuclide tint: rings with sound multiply each channel by 0.16…1 from the drift (`RenderDots`); the random rings in silence by 0…1, with the original's red and blue swapped (`Nuclide()`).
- Next, the 20-second timer, and Lock change or hold the pair and its effects. Color follows the sound and the drift, so Lock no longer holds a color. Pause freezes the picture and the drift clock.

Intentional differences:
- The Fourier sum uses the block's own sample rate and a true 2π, where the original assumed 44.1 kHz and wrote 6.28.
- The smoothing and the sync path's drift run on elapsed time in reference frames; the original counted frames.
- In silence the original divided by a zero-length color. Here the wave takes the drift color.
- The warp rounds each pixel's three channels with one random threshold, so rounding never speckles the hue.

## Controls and visual design

- Visual direction: modernized interpretation. Keep recognizable Geiss elements — procedural maps, waveforms, rich colors, and evolving layers — while refining rendering, motion, and visual balance.
- Visuals evolve automatically.
- The first control set is:
  - **Sensitivity.** A linear gain on the analyzed level. The default is 1.0, and the value persists. Tune the default once real tap levels are visible.
  - **Pause.** Freezes the picture. This state does not persist.
  - **Lock.** Keeps audio motion and holds the current map, waveform, and effects. Colors keep following the sound. This state does not persist.
  - **Next visual.**
- **Info readout (debug aid, user decision).** I, or Visual > Show Info, toggles a small readout in the top-left corner: frames per second (counted from real ticks, refreshed twice a second), the map number and name, the waveform number and name, and the look's effects, marked as resting in silence, and the HDR highlight gain with the screen's current and potential headroom, "no headroom now" on an HDR screen whose current headroom is 1 (for example at full brightness), or off on a screen whose potential headroom is below the cap, with that headroom when it is above 1. Names follow [the glossary](../glossary.md). It starts hidden, is not persisted, and is not a visualizer control.
- The picture is full-bleed. Vertical size, letterboxing, beat response, variation pace, and horizontal shift stay internal until a later version asks for them.

## When audio permission is missing

- Waiting for permission, or granted with sound: the visual reacts to audio.
- Granted and silent: the same motion continues, calm, with no error. The idle wave (see [Layered effects](#layered-effects)) keeps it from fading to black.
- Unavailable, or the tap fails: calm motion continues, and an overlay explains the state and opens System Settings.
- Denied: macOS creates the tap without an error and delivers only exact zeros, so a denial looks like a quiet Mac. When another process is running audio output (Core Audio's process list, `kAudioProcessPropertyIsRunningOutput`, Geiss excluded) and the tap has delivered no sample other than exact zero for 2 seconds, checked every half second, the tap counts as silenced. So the overlay appears about 2 to 2.5 seconds after a denied tap starts while an app plays. The overlay says that Geiss may not be able to hear this Mac: an app has output open but Geiss hears only silence, and if sound is playing, turn on Geiss under System Audio Recording Only. It hedges because an app that keeps its output open while silent looks the same. Because macOS does not ask again, this overlay is how a declined user is asked, on every launch while the permission is off. The first non-zero sample clears it, and so does no other process running output, because then silence is expected; it returns about 2 seconds after output starts again if the tap still hears only zeros.
- Permission applies when a tap is created. A running tap keeps the permission it started with: turning permission off does not silence it, and turning it on does not wake a silenced one. The next tap follows the new setting, for example after an output-device change or a relaunch. So revoking at runtime goes unnoticed until the tap is rebuilt, and that is accepted. A Bluetooth output can rebuild the tap more than once while it connects (AirPods were observed at 24 kHz, then at 48 kHz about 6 seconds later), and each rebuild starts the 2-second check again, so the overlay can take several seconds after such a switch. The log names the reason for each rebuild.
- Returning to the app retries a failed tap and rebuilds a silenced one, so permission turned on in System Settings applies without a relaunch, and macOS's Quit & Reopen prompt can be answered Later. A silenced tap is rebuilt at most once every 5 seconds, so switching between apps does not keep recreating Core Audio devices. The overlay stays up during that retry, and for a silenced tap until it hears sound or no other process runs output.
- Known limit: a process that keeps its output running while it plays exact silence, such as a paused player that holds its stream open, can raise the overlay with permission granted. Browsers with an idle audio context and call apps may do the same. The overlay clears when sound arrives or that output stops. With nothing playing at launch, a denial shows the calm picture alone until some app plays.
- A running tap that delivers no callbacks for three seconds is restarted, and a restart that cannot create the tap shows the overlay.

## Prototype milestone

The picture has all 25 of the original's map modes and all 6 of its waveforms. Each frame samples the previous frame through the map, red, green, and blue alike, darkens it, and draws the waveform.

Maps, from `GenerateChunkOfNewMap`. `dx` and `dy` are the pixel offset from the map center. Most modes compute a scale and read from the center plus that offset, rotated by the map's turn and multiplied by the scale. `r640` is the offset's length on a 640-wide frame, as the original's `rmult` made it, so a mode keeps its look at any simulation size. Modes 17 and up use `nx` and `ny`, the offset times `2 / width`. The [glossary](../glossary.md) describes each map and effect in plain words.

- 1: constant zoom, a constant scale of 0.985 − 0.12·u². 2: outward zoom, a constant scale of 1.00…1.02. 3: terra-landing, `0.95 − dy·(480/H)·0.0005`. 4: a sphere, `0.9 + r640·0.0025·0.14`. 5: super-perspective, `f2 − f1·r640/200·1.7` with the nuclide effect, `f2 − f1·√(r640/200)` without.
- 6: a vortex field. Five push centers each push, or swirl one way or the other, weighted by `1 / (d² + 0.1)`, normalized to 1.9 pixels with a small drift. Distances and the move use the 640-wide reference.
- 7: fuzzy, `f1 − r640·f2` plus a fixed per-pixel jitter of 0…0.0495. 8: ripples, `0.85 + 0.1·sin(√r640·f1)`. 9: flower petals, `f1 − r640·f2` on even pixels and a constant `scale2` on odd pixels.
- 10: a scroll. Rows read from `y·1.04`, and columns stretch from the center by `1.03 + 0.03·y/H`. 11: two rotations, each with its own scale, dithered by pixel parity. 12: a sideways splitter. Columns read from `±√|dx|` near the centerline (on the 640 reference), and the picture fades faster (0.98 per frame).
- 13: a black hole, `(1.04 − r640^1.5·0.00025·0.14 − 1)·f1 + 1`. 14: a split world, `0.9 + 0.2·cos(dy·12/H)`. 15: radial petals, `f2 + f3·sin(angle·f1)` with 2…6 petals. 16: a crystal ball, `1.05 − r640²·0.00025·0.09`, at least −1.5, so far pixels read from the other side.
- 17: a horizontal tunnel, `0.97 − ny²·0.40`. 18: a vertical tunnel, `0.97 − nx²·0.40`. 19: a vortex, `1.04 − 0.25·length`. 20: terra, `1.15 − √(ny + 1.4)·0.20`. 21: diced cubes, `0.95 − ⌊|nx|·10⌋·0.03 − ⌊|ny|·10⌋·0.03`. 22: phonic rings, `0.95 − ⌊length·10⌋·0.04`. 23: fast rings, `0.95 − (⌊length·20⌋ mod 4)·0.12`. 24: a fast swirl, scale 0.96 and turn 0.05. 25: the smooth zoom, `zoom / (zoom + length)` with zoom 2…6.

Each new map picks its center, turn, and constants as `GenerateChunkOfNewMap` did: per-mode turns, a random rotation direction, and turns scaled by 0.6. Modes 1, 2, 4, 5, and 7…16 move half as far per frame, as `mode_motion_dampened` made them. The original also scaled that by `suggested_damping`, a measure of how much the music was changing; that stays at full here.

Intentional differences:
- The original shrank the motion of modes 5, 7, and 9 above 640 pixels wide (`protective_factor`). Here every mode keeps its 640-wide look at any size.
- Mode 7's jitter comes from a hash per pixel and map, not the original's shared random table.
- Mode 14 leaves out a jitter of under 1/1024 in the height.
- Mode 20 reads a negative root above the frame as zero.
- Mode 5 falls off with `r·1.7` when its look has the nuclide effect and with `√r` when it does not. The original read the nuclide state of the previous look, because it built the next map while the current one played. Here it follows the new look.
- Modes 7 and 9 stop their per-pixel scale at 0. On frames much taller than wide, `r640` reaches radii the 640×480 original never did, and `f1 − r640·f2` would turn negative and flip the corners through the center.

Kept as in the original:
- A source point past the left or right edge wraps around (`newx += FXW-1` while below 0), so maps that read beyond the sides, such as 2, 10, and 16, pull in the opposite edge. Rows clamp.
- Mode 24's pixel loop sets `turn1 = 0.05`, so the random rotation direction does not reach it and every fast swirl turns the same way.

Waveforms, from `RenderWave`. A −1…1 sample covers about 35% of the height before each waveform's own scale.

- 1: one smoothed line across the frame center. Over mode 10's scroll it sits at three quarters of the height, as in the original.
- 2: two smoothed lines, left 12% of the height above the map center and right 12% below, at 0.7 of the amplitude.
- 3: one smoothed vertical line through the map center. Row y reads the sample of column y.
- 4: two smoothed diagonal lines at 0.9 of the amplitude, left from the top-left corner and right from `width − height`.
- 5: a circle around the map center. Its radius is 60 pixels on a 640-wide frame plus the wave at 0.7 of the amplitude. The first 50 points blend toward the points after 314, and the radius smoother starts from that blended first point, as the original's in-place blend did, so the circle closes without a notch.
- 6: the X-Y oscilloscope. It plots 314 smoothed stereo points around the map center at 1.2 of the amplitude, rocked by `sin(t * 0.3)`.

Intentional difference: the original walked the first frames of its capture buffer for waveforms 5 and 6. Here those points spread across the whole column wave.

Picking follows the original:
- A mode is chosen uniformly from 1…25, then has a 1-in-25 chance each of becoming fuzzy (7) or super-perspective (5).
- A waveform is chosen with 1…5 at 3/17 each and 6 at 2/17.
- The waveform is re-chosen while the pair is excluded: mode 6 with waveform 5, mode 12 with waveforms 4 or 6, mode 14 with waveforms 3 or 4, and modes 8, 23, or 24 with waveform 6.
- Next and the timer always change the pair.

### Layered effects

Nine overlays are ported from `Effects.h` and `main.cpp`: the original's seven live effects, its unused spectral dots, and its commented-out crystals. Mode 6 also draws the original's commented-out edges. They use the original's truecolor drawing. They draw into the previous frame before the warp, as the original drew into `VS1`, so the map smears them into trails. Sizes and positions are for a 640-wide frame and scale with the width. Time is counted in reference frames at 30 fps, so speeds do not depend on the display's refresh rate. Effects draw only while there is sound. In silence, and while capture is denied or failed, they rest until sound returns, so the picture stays calm. The one exception is the nuclide's random rings (user decision). The original wrote blue, green, red; where its truecolor code named a channel, this port keeps the byte it landed in.

- **Chasers:** one or two points on two-cosine curves around the map center. Each frame they take 20 steps (at 640 wide), and each step lifts every channel of its pixel 40% of the way to the top, so they draw white. The steps sit on a fixed grid of time, so the path is the same at any frame rate.
- **Bar:** a short line between two endpoints that wander near the center. Each frame it adds 16 to every channel on it below 223. Between frames it adds its share. It is drawn once per channel, green and red ahead of and behind blue in time by up to 3.5 · 4 · 2 reference frames (`chromatic_dispersion`), so it fringes in color as it moves.
- **Dots:** a slow point leaves a trail of its last 20 positions, each in a color whose channels cycle at three rates (`127 + 126·sin`). Every dot drifts one pixel right each frame. From 880 pixels wide each dot is 2×2, as the original's truecolor dots were.
- **Solar:** particles land within 35 pixels of the center, each with a 3×3 glow that is brighter near the center. Each channel takes its own random strength, `4 + rand()%30 · (35 − distance) / 25`, so the particles sparkle in many colors, and a channel at 207 minus that strength takes none. The count swings with time and with the mode's strength (`solar_max`, from the original's truecolor table, where mode 1 has 800). Wider frames get more particles, so the density stays the same. A new mode 1 look opens with a burst of 500 particles half the time, whether or not Solar is on. A look that opens in silence loses its burst.
- **Grid:** a dot every 1/30 of the width in both directions. It scrolls one pixel a frame, left or right, and its brightness pulses. It raises each channel to its level and never darkens one, so it draws grey over black.
- **Nuclide:** a ring of 3…7 soft discs, 34…41 pixels from the center, tinted by the drift (see [Sound-synced color](#sound-synced-color)). With sound, a jump in volume (above 1.1 times the original's narrow average, which takes 0.7 of each new frame) drops a ring whose disc size follows the jump. In silence, a ring with discs of a random radius of 3…10 drops about one reference frame in 12, as in the original (user decision); at 30 fps that is about 2.5 rings a second, and a ring's center can reach 250 in a channel, so it can glow on an HDR screen. In 4 looks out of 7 the nuclide replaces the waveform, except over mode 10, where the original set the waveform again afterwards.
- **Spectral dots:** one soft disc for each of three bands, each in its own random color whose channels add up to at least 1.2 (the original's commented-out `g_DotColor` pick, made with each new look): bass below 250 Hz, middle from 250 Hz to 2 kHz, and treble above 2 kHz. Each band's level is smoothed each frame (0.6 new, 0.4 old) and sets its disc's radius, `level · 0.06 − 2`, up to 10, so a band below a level of about 33 draws nothing. Each disc wanders on its own two-cosine path within about 115 pixels of the center. As in the original, the path's phase moves whenever the band's level rises or falls by less than 4: back by 2.5 per level it rises, and forward a little on a slight fall. So the dots jump with the attacks in each band, creep on a sustained note that wavers, and hold still on a steep fall. The levels and phases advance once per reference frame; above 30 fps each display frame stamps the discs at their current place with its share of a frame, like the bar, so the map smears them into the same streak at any refresh rate. A new look starts its levels from zero. Spectral dots always replace the waveform, also over mode 10, because the original set that last.
- **Crystals:** every reference frame, 2…7 evenly spaced spokes of 6…25 pixels burst from a point within 15 pixels of the center, at a random angle. Every channel of every pixel they cross is raised to at least 150.
- **Edges (mode 6):** three rows along the top and three along the bottom take noise of 0…39 over a slow sine of 0…198 per channel, each channel with its own rates, drifting sideways (left and right differently at top and bottom), so the vortex field pulls a moving, color-shifting texture in from both edges. The rows are rewritten once per reference frame, so the texture is the same at any refresh rate. They belong to the map, like the center darkening, and are not an effect pick, so they do not count toward the mode's limits. Like the effects, they rest in silence.
- **Shade bobs:** one bob wobbles a few pixels around the center and takes four jittered steps a frame. Each of its channels switches on while `sin(floatframe · micro_c)` is between 0 and 1, about half the time, at its own rate of 0.08…0.17, so its color drifts; a channel that is on adds 5 to its pixel and 3 to the four beside it, up to 250.

Each map also darkens its center every frame (`center_dwindle`), so the zoom does not burn a hot spot there: 0.99 for modes 3 and 5, 0.98 for 4, 13, 14, 16, and 20…23, 0.985 for 7 and 9, 0.96 for 8, and 0.915 for 12. Mode 12 darkens a three-pixel column, since it splits sideways from a vertical line. The others darken the center pixel and its four neighbors. Each channel darkens alike.

Picking follows `CModeInfo` and `Clip_Num_Effects`:
- Each mode gives each effect a weight out of 1000. The weights use the original's truecolor adjustment: nuclide × 1.3, chasers − 50, dots and bar + 220, shade + 150, each kept within 0…900, and grid + 8.
- Spectral dots and crystals have a weight of 100 on every map. The original gave them none (the spectral dots had 0 everywhere, and crystals were never called), so this is the port's own assumption, to judge by eye. With sound that is 70, so each comes up in about one look in 14 before the mode's maximum drops extras.
- Each effect gets one roll. While sound is playing, every weight is multiplied by 0.7.
- Without sound, effects are added until the mode's minimum is met. Then effects are dropped at random down to the mode's maximum. Most modes have a minimum of 1 and a maximum of 2. Mode 2 allows up to 5, and modes 17 and up 1…3. Modes 10, 11, 12, and 15 have no minimum, so their looks can have no effects; their maximums are 2, 4, 2, and 1. While sound is playing the minimum is not enforced, so a look on any mode can open with no effects.
- Chasers come as one or two. A grid turns the bar off. The grid's direction is random.
- Effects are part of the look. Lock holds them. Next and the 20-second timer pick a new set for the new map. Pause freezes them with the picture.

Intentional differences:
- The original's weight table had a ninth slot that no mode set. It defaulted to 111 and could take up a place without drawing anything. It is left out.
- Where no effect left could come up, the original's minimum loop would hang. Here it stops.
- The original turned its grid points into 2×2 blocks above about 1700 pixels wide. The simulation stops at 960, so they stay one pixel.
- Effects rest in silence. The original kept drawing them. The user chose a calm picture in silence and when capture is denied, so they rest, except the nuclide's random rings, which the user later chose to bring back. The center darkening is part of the map and continues. The info readout still marks the effects as resting in silence.
- Spectral dots: the original computed its three bands with a filter that is not in the source (the 4.21 notes credit Dave Ulmer's spectral analysis), so this port splits the mono samples with two-stage one-pole low-passes at 250 Hz and 2 kHz and measures each band's mean absolute level over the newest 1/30 s, after the sensitivity gain. The level keeps the original's scale: the mean times 32768 · 66 · 196 / 132 and a band weight of `0.0005 + 0.0007·u + 0.00045·√u`, u = band / 2, up to 255. The original read the band weight after its loop, so every band took the treble's; here each band takes its own, as the formula intended. The band cutoffs and scale are assumptions to judge by eye.
- Crystals and edges were commented out in the original; they are ported from that code as written. The original's commented-out neutrons, a full soft disc dropped at a random spot about three times a second, were ported and then removed because they looked bad (user decision). The original drew crystals at a fixed size; here they scale too. The original placed the edges just inside its letterbox cut; the port has no cut, so they sit on the frame's outer three rows.
- Idle wave (user decision: silence keeps a faint, calm motion instead of fading to black). Intentional difference: the original drew nothing in silence. Once there has been 1.5 s of silence, or of sound whose line would draw dimmer than the idle wave's darkest (compared as drawn colors by their brightest channel: the line in its spectrum color at its brightness before the beat scale, so beat mode's dark gaps between hits do not count, against the drift color at the idle wave's darkest brightness of 26 on RenderWave's 0…155 scale; the drift color reaches up to 2.5 times its brightness and a line's color about 1.3 times, so comparing brightness alone let quiet music draw darker than silence), a made-up signal rises over 4 s, counting only the time past the wait so the pace is the same at any frame rate, and is drawn with the current look's own waveform, so each look keeps its shape: the line becomes a slow swell, two lines braid, the vertical line and diagonals sway, the circle opens into a breathing flower, and the oscilloscope traces a slowly turning, morphing 3:2 Lissajous figure. Left runs three slow cycles across the frame and right two, each with a smaller, faster partial drifting the other way. Its size swells between 0.2 and 0.4 of full scale over 11 s, and its brightness breathes between about 26 and 46 over 7 s, drawn in the drift color, so its brightest channel peaks near 114, below full display brightness, and it never glows on HDR. Its points are soft 5×5 spots (about two thirds of the color beside the center, a twentieth at the corners) instead of single pixels, so the circle's widely spaced points at a large radius run together into a soft line. The map warps it into trails like any waveform. A look whose nuclide or spectral dots replace the waveform draws the idle wave as the circle. Quiet sound below that hand-off keeps the idle wave, with its own faint line playing inside it, so quiet music never looks darker than silence. Louder sound fades the idle wave out over 0.5 s instead of cutting it, and quiet that returns while any of it is left brings it back up without the wait, so sound hovering near the hand-off makes it dip rather than blink off and on. Faint sound still counts as sound for the effects. Its drift phases are worked out in double and wrapped to one turn, so the motion stays smooth after days of running. It also covers a denied tap and the time before the first audio. Pause holds it; Lock and Next are unchanged. The engine can turn it off (`setIdleWave`) for a host that wants the old fade.

The window opens on mode 25 and waveform 1, with its own effects. Another pair is chosen about every 20 seconds unless the visual is locked. Next chooses one immediately. Pause freezes the picture and that timer. The oscilloscope rock advances while the picture is moving, including while locked, and holds while paused. Chunked background generation waits.

The picture is truecolor, and the wave takes its color from the sound. The window listens to the default output mix. Visual > Tone (⌘1) and Visual > Silence (⌘2) override that source for testing, and Visual > System Audio (⌘0) returns to it. The tap keeps running during an override.

## Project layout and code policy

- `engine/` holds the portable C++.
- `macos/` holds the Xcode app and the Objective-C++ bridge.
- The root `CMakeLists.txt` remains the Windows build and does not gain a macOS target.
- Once the project exists, build the app with `xcodebuild`. Test analysis and map math with a small C++ executable that uses assertions. The Xcode project is checked in.
- Ported algorithm files keep the Geiss copyright and 3-Clause BSD notice. The SwiftUI shell is separate new code.
- Add unit tests for all new portable visualization and audio-analysis code in the same change. Check system permission behavior and live rendering manually. Add an automated test when a behavior is stable and testable. Screenshot comparison is not the quality gate.
- Use the existing C++ style only inside preserved legacy files. Use current Swift conventions for new app code. New C++ is a modern portable subset.
- Keep platform APIs at the edges: capture, the bridge, and SwiftUI.
- Name the original function a ported routine follows. Note intentional visual differences beside that code.
- Handle resize, sleep and wake, output-device changes, and permission changes without leaking the tap or jumping the simulation clock. A new simulation size applies once the window has held it for 0.15 s, so a live resize or a fullscreen animation does not rebuild the map and the textures every frame; until then the current picture is stretched to the window. While the window opens, sizes apply at once for the first 0.15 s, so a provisional first layout, such as a restored frame or a launch into fullscreen, does not leave a stretched picture; a view that has not been laid out yet is skipped. Whether 0.15 s covers a launch into fullscreen is checked by eye.

## Distribution and rights

- Initial use is local. Public distribution and App Store plans are out of scope for now.
- The local app name can be Geiss. An About box carries the existing copyright and 3-Clause BSD notice.
- Review the repository license, attribution requirements, name, and any bundled assets before public distribution.

## Open questions

- Which existing configuration options, beyond the prototype slice, should be preserved, modernized, or left out?
- Is the idle wave's 1.5 s wait long enough that quiet passages inside a track do not bring it up, and are a brightness of 26 for the hand-off and 36 for the idle wave right? Tune by ear and eye.
- Do the highlight thresholds (stored 128 to 208) glow the right parts of the truecolor picture on an HDR screen, and does the 256-sample spectrum color bass convincingly? Judge by eye.
- Is the silenced overlay's false alarm from apps that keep their output running while silent (paused players, idle browser audio, call apps) rare enough in practice, or does it need a second signal?
- Should a slide-shift drift stop when beat mode ends or the sound goes quiet? The original kept drifting until the next look.
- Later decisions: a GPU warp, size and quality settings, presets, multi-display, a menu-bar extra, the screensaver host, sandboxing, and a public name.

## Existing experience inventory (discussion draft)

The Windows project has a substantial set of rendering behaviors and options. Some controls belong to obsolete display or audio hardware, or to Winamp and CD playback. The inventory below records that intent. The first macOS version exposes only the controls in [Controls and visual design](#controls-and-visual-design).

### Visual identity and animation

- **Procedural evolving backgrounds/maps:** Geiss creates a new visual map in chunks while the current one is displayed, then transitions to the new map. This evolving, fluid look is central to the identity. Preserve the feedback warp and automatic change of maps. The first build generates each map completely when the visual changes. Preparing the next map in slices waits.
- **Waveforms:** The project cycles through several waveform styles, including an X-Y oscilloscope mode. Wave shape and amplitude react to the audio. Preserve the recognizable waveform family and audio response. All six of the original's waveforms are ported.
- **Layered effects:** The renderer has optional effects such as chasers, bars, dots, solar-like overlays, grids, nuclide patterns, and shading. Some are selected or varied as part of modes. Preserve the signature effects and their combinations after assessing each for visual quality and cost on modern displays. All seven of the original's live effects are ported, with its per-map weights, plus its unused spectral dots and crystals and mode 6's edges. See [Layered effects](#layered-effects).
- **Automatic variation:** Modes, waveforms, palettes, and effects can change over time. Preserve automatic variety. Pause, lock, and next visual are the first-version controls for that behavior.

### Existing configuration intent

- **Vertical visual size / letterboxing:** percentage of display height occupied by the visual. Deferred. The first version is full-bleed.
- **Wave scaling:** default audio waveform amplitude. This is the first-version sensitivity control.
- **Color behavior:** palette generation, gamma, coarse and solar palette frequency, and optional sound-synchronized color. The palette controls belonged to the original's 8-bit mode. The port draws in truecolor, as the original's recommended 32-bit default did, so it has no palette (user decision), and sound-synchronized color is always on, with no control (user decision).
- **Beat detection:** controls beat-responsive behavior. Beat brightness and look changes on a big beat follow the original, from a bass-focused detector. A user-facing beat control waits.
- **Automatic map changes:** controls how long maps are generated and shown. The first version evolves automatically. A pace setting waits, and any later control uses seconds.
- **Automatic horizontal shifting:** percentage and frequency, plus a minimum interval between shifts. The port keeps the shifting at the original's default 33% of looks, with frame-rate-independent timing, and has no setting for it.
- **Rendering resolution, color depth, adapter selection, frame idle delay:** Windows and DirectDraw performance controls. The first version uses the capped simulation size in [Rendering](#rendering). A quality setting waits.
- **Help messages, debug info, suppress all messages:** help overlay and diagnostics. Keep diagnostics developer-only. On-screen help in the visualization waits.

### Remove from the macOS product scope

- Winamp integration, track-title overlays, playback controls, and Winamp-specific minimize and flicker options.
- CD playback, CD controls, and CD auto-start.
- Windows sound input device selection and mixer instructions. The target is system output capture with macOS permission handling.
- Windows display adapter selection, video-mode selection, and DirectDraw-specific compatibility options.

### Decisions

- Visual fidelity: modernized interpretation, preserving recognizable Geiss elements while refining presentation.
- Automatic variation: visuals evolve on their own. Pause freezes the picture. Lock holds the current map, waveform, and effects while audio motion continues; colors keep following the sound. Next visual advances the look.
- Initial controls: sensitivity, pause, lock, and next visual.
- Pixels: portable C++ simulates truecolor, red, green, and blue per pixel, then expands it to RGBA8 with the original's doubled display. Metal uploads and scales that buffer. There is no palette (user decision).
- Current picture: all 25 original map modes and all 6 waveforms, picked with the original's weights and excluded pairs. The window opens on mode 25 and waveform 1. Next and the 20-second timer pick a different pair. Each look also picks up to a few of nine layered effects: the original's seven with its per-map weights, and its unused spectral dots and crystals. Mode 6 adds its edges. Lock holds the pair and its effects. Pause freezes the picture. The picture is truecolor: the wave takes its color from a 24-band spectrum (bass red, middle green, treble blue), and the effects use the original's truecolor colors. Each frame feeds back through the same warp. No new controls. Beats brighten the waveform and land look changes, from a bass-focused detector. A third of the looks drift on beats with the original's slide shift. Chunked map generation, a GPU warp, and the screen saver host wait.
- Color: truecolor replaces the palette, and sound-synced color is always on, with no control (user decisions).
- Launch: the visualizer opens immediately. Denied or failed capture keeps calm motion and shows an overlay that opens System Settings.

### Choices considered

1. **How faithful should the visuals be?**
   - *Close recreation:* retain the original compositions, palettes, and motion as closely as the new renderer allows.
   - *Modernized interpretation (chosen):* keep recognizable waveforms, procedural maps, palette richness, and evolving layers, while improving smoothness, resolution handling, and visual balance.
   - *Selective inspiration:* retain only a small set of recognizable motifs and redesign the rest.
2. **How much control should the first app expose?**
   - *Translate useful controls:* keep size, sensitivity, beat response, sound-linked color, and variation pace, and fold obsolete palette-era controls into defaults.
   - *Minimal controls (chosen):* sensitivity, pause, lock, and next visual. Size waits with the other legacy settings.
   - *Parity-oriented controls:* expose nearly every visualization setting that still has a meaningful modern equivalent.
3. **How should automatic variation work?**
   - *Chosen:* visuals evolve automatically. Pause and lock are separate actions, and next visual is available.
   - A calmer default that stays on one look until the user asks it to change.
