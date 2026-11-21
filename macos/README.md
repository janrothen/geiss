A window that draws a few Geiss maps and waveforms on Apple Silicon. It listens to the sound coming out of this Mac.

[![macOS](https://img.shields.io/badge/macOS-14.2%2B-black)](https://developer.apple.com/documentation/macos-release-notes)
[![architecture](https://img.shields.io/badge/architecture-Apple%20Silicon-black)](https://developer.apple.com/documentation/apple-silicon)
[![license](https://img.shields.io/badge/license-BSD--3--Clause-blue)](https://github.com/janrothen/geiss/blob/main/LICENSE)

# Geiss picture window

## Build

The Windows CMake project at the repository root is unchanged. The picture uses its own builds.

Engine tests:

```console
cmake -S engine -B /tmp/geiss-engine-build
cmake --build /tmp/geiss-engine-build
/tmp/geiss-engine-build/geiss_engine_tests
```

App:

```console
xcodebuild -project macos/Geiss.xcodeproj -scheme Geiss -destination 'platform=macOS,arch=arm64' -derivedDataPath /tmp/geiss-dd build
open /tmp/geiss-dd/Build/Products/Debug/Geiss.app
```

## What you see

The window opens at 1280×720 and asks to record system audio. The picture starts as a smooth zoom with one line across the middle. About every 20 seconds, and whenever you press Next, it picks another look from the original's 25 maps (tunnels, zooms, vortices, ripples, petals, rings, a crystal ball, a sideways splitter, and more) and 6 waveforms (one or two lines, a vertical line, two diagonals, a circle, or an X-Y oscilloscope). Each look also picks one of the original's palettes, blended in over a moment, and layers one to a few of the original's effects over the map: chasers, a bar, a dotted trail, solar flares, a grid, nuclide rings that jump with the music and sometimes replace the waveform, and shade bobs. The effects rest while the Mac is quiet. The waveform follows the Mac's output. When music has a steady bass beat, the line flashes at the palette's brightest color on each hit and goes dark between them (the oscilloscope stays steady), and the next look waits a moment to land on a beat. About a third of the looks also slide the picture a few pixels sideways, alternating left and right, on loud bass hits, and keep drifting that way until the next hit. Quiet output leaves the picture calm. Visual > Tone (⌘1) replaces the Mac's sound with a steady test note, and Visual > Silence (⌘2) with nothing, so the waveform fades. Visual > System Audio (⌘0) goes back to listening to the Mac. Pause freezes the picture. Lock holds the current look, its effects, and the color. The controls hide after a few seconds and return when the pointer moves. The picture updates at your display's refresh rate, up to 120 Hz on ProMotion displays; press I to see the frame rate. While paused it drops to about 15 frames a second to save power. While you resize the window the picture stretches, and it sharpens a moment after you stop. On an HDR display, such as a Studio Display XDR, the brightest parts of the picture (fresh lines, beat hits, bright effects) glow above normal white, up to 2.5 times; the I readout shows the HDR gain. A screen counts as HDR only when it can reach that full 2.5 times, so other screens show the picture as before.

The bar reads LIVE while the tap is running, WAIT while it is starting or blocked, NO AUDIO while it hears only silence although an app has output open, TONE for the generated note, and QUIET for silence. Sensitivity is remembered. Pause and lock are not.

Press I (or Visual > Show Info) for a debug readout in the top-left corner: frames per second, the map number and name, the waveform, and the look's effects, marked "resting" while the Mac is quiet. The names are the ones in [the glossary](../docs/glossary.md). Press I again to hide it.

## Linting

A pre-commit hook lints the files you commit. clang-tidy checks the C++ in `engine/` and the Objective-C++ bridge, using [`.clang-tidy`](../.clang-tidy). Any finding blocks the commit. SwiftLint checks the Swift in `macos/`, using [`.swiftlint.yml`](../.swiftlint.yml). Only errors block the commit, and warnings are shown. The Windows sources in the repository root are not linted.

```bash
brew install llvm swiftlint pre-commit
pre-commit install
pre-commit run --all-files
```

## License

Copyright (c) 1998-2022 Ryan Geiss. The picture engine and this app are licensed under the 3-Clause BSD License. See [LICENSE](../LICENSE).

## Troubleshooting

| What you see | What to do |
|---|---|
| The window stays black | Wait a moment, or press Resume if the picture is paused. With no sound the picture fades to black; play some sound. If it stays black while music plays, Geiss lacks permission: see the next rows. |
| The info readout shows far fewer frames per second than the display's rate | Rebuild with the command above. Both build configurations compile the engine with optimization; an unoptimized engine manages only about 30 fps at 960 wide. |
| The picture looks stretched or soft while resizing | Expected. The simulation keeps its size until the window has stayed still for a moment, then sharpens. |
| The info readout shows about 15 fps | The picture is paused. Press Space to resume at the display's rate. |
| The waveform does not move | The Mac is quiet, or Visual > Silence is selected. Play some sound, or choose Visual > Tone. |
| The line vanishes between beats | That is beat mode with a steady bass beat. It returns on each hit and stays steady in quieter or beatless music. |
| Nothing glows on an HDR display | Press I. "off (screen reaches …)" means the screen's headroom is below 2.5 times; "no headroom now" means it is at full brightness. Lower the brightness a little, or turn off a fixed reference mode. |
| "Geiss cannot hear this Mac" or "may not be able to hear this Mac" | Choose Open System Settings and turn on Geiss under **System Audio Recording Only** (not the list above it). Then click back into Geiss; it picks up the change without a relaunch, so answer macOS's Quit & Reopen prompt with Later. |
| Geiss is not listed under System Audio Recording Only | macOS lists an app only after it was allowed, or when it runs from an Applications folder. Copy `Geiss.app` to `~/Applications` and open it from there, or add it with +. To see the first-launch prompt again, run `tccutil reset AudioCapture`. |
| "Geiss may not be able to hear this Mac" shows although permission is on | An app is keeping its output open while silent, such as a paused player, a browser tab with idle audio, or a call app. The message clears when sound plays or that app closes its output. |
| Playback stops while Geiss is open | Quit Geiss. The tap is meant to leave playback running. |
| `xcodebuild` cannot find Metal | Build on an Apple Silicon Mac with Xcode, for the `arm64` destination above. |
| The engine tests fail to configure | Use CMake 3.20 or newer and run the commands from the repository root. |
| The commit stops with "clang-tidy not found" | Run `brew install llvm`. The hook finds Homebrew's clang-tidy on its own. |
| SwiftLint lists warnings but the commit goes through | That is expected. Only SwiftLint errors block a commit. |
