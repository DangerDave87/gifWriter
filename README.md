# Gif Exporter for Nuke

This repository contains a native NDK-based GIF writer plug-in for Nuke.

## Current status

Phase 0, Phase 1, Phase 2, Phase 3, and a first Phase 5 animation path are now in place:

- CMake-based project layout
- native `gifWriter` shared-library target
- `DD::Image::FileWriter`-based writer registration for `.gif`
- GIF export settings UI
- movie-writer lifecycle skeleton for animated output
- Phase 2 diagnostics logging
- real still-frame GIF encoding
- animated GIF streaming across `execute()` / `finish()`
- palette-size control
- automatic frame differencing for smaller opaque animations

The current implementation now behaves like a movie writer:

- `movie()` always returns `true`
- the output file is opened lazily on the first `execute()`
- `execute()` is called once per input frame
- `finish()` closes and resets the writer state once at the end

The plug-in now exports real GIF image data:

- RGBA is read back from Nuke rows
- output conversion uses the writer LUT path via `to_byte()`
- pixels are quantized into a fixed indexed palette
- the `max colors` knob reduces the effective palette size
- optional dithering, transparency thresholding, and matte flattening are applied
- animated GIFs are written as one file across repeated `execute()` calls
- looping is controlled by the loop mode / loop count knobs
- frame timing is derived from the `fps` knob
- opaque animations automatically crop frames to changed regions to reduce file size

The current animated implementation uses a fixed global palette family and conservative frame differencing. For reliability, transparent animations may still fall back to full-frame updates when cropped transparency would be ambiguous.

## Expected output binary

The plug-in target is named:

- `gifWriter.dll` on Windows
- `gifWriter.so` on Linux
- `gifWriter.dylib` on macOS

Using the `gifWriter` name matches Nuke's writer naming convention for `.gif`.

## Build requirements

According to the Nuke NDK documentation and Foundry's bundled Windows toolchain info:

- use CMake
- on Windows, match the compiler family to the Nuke version you are targeting
- build against the Nuke version you plan to load the plug-in into

Recommended Windows targets for this repo:

- Nuke `14.x` -> Visual Studio `2019` / `v142`
- Nuke `15.x` -> Visual Studio `2019` / `v142`
- Nuke `16.x` -> Visual Studio `2019` / `v142`
- Nuke `17.x` -> Visual Studio `2022` / `v143`

## Configure

Point CMake at the Nuke install or SDK location. The project currently looks for:

- `DDImage/Writer.h`
- the `DDImage` library

The easiest approach is to pass `NUKE_ROOT`.

Example:

```powershell
cmake -S . -B build -G "Visual Studio 16 2019" -A x64 -DNUKE_ROOT="C:\Program Files\Nuke15.0v1"
cmake --build build --config Release
```

If auto-detection is not enough, you can also pass:

- `-DNUKE_INCLUDE_DIR=...`
- `-DNUKE_DDIMAGE_LIBRARY=...`

## Build helper script

There is a Windows PowerShell helper script at:

- [scripts/build-installed-nukes.ps1](/D:/002_Projekt/NukePlugins/GifExporter/scripts/build-installed-nukes.ps1)

It does three things:

- scans common install roots and Windows uninstall registry entries for installed Nuke versions
- matches those installs against a requested list of minor versions
- configures and builds this plug-in once per detected Nuke version, then copies `gifWriter.dll` into a minor-version `artifacts` folder such as `artifacts/13.2/`, `artifacts/16.0/`, or `artifacts/17.0/`

Default supported version list:

- `13.2`
- `14.0`
- `14.1`
- `14.2`
- `15.0`
- `15.1`
- `15.2`
- `16.0`
- `17.0`

Examples:

```powershell
# Show detected installs only
powershell -ExecutionPolicy Bypass -File .\scripts\build-installed-nukes.ps1 -ListOnly

# Build Nuke 15.1 and 16.0 if installed
powershell -ExecutionPolicy Bypass -File .\scripts\build-installed-nukes.ps1 -Versions 15.1,16.0

# Build Nuke 13.2, 16.0, and 17.0 if installed
powershell -ExecutionPolicy Bypass -File .\scripts\build-installed-nukes.ps1 -Versions 13.2,16.0,17.0

# Build Nuke 17.0 with the default VS 2022 / v143 policy
powershell -ExecutionPolicy Bypass -File .\scripts\build-installed-nukes.ps1 -Versions 17.0

# Clean and rebuild all default target versions
powershell -ExecutionPolicy Bypass -File .\scripts\build-installed-nukes.ps1 -Clean
```

The script uses these Foundry-era compiler expectations by default:

- Nuke 14.x -> Visual Studio 2019 / `v142`
- Nuke 15.x -> Visual Studio 2019 / `v142`
- Nuke 16.x -> Visual Studio 2019 / `v142`
- Nuke 17.x -> Visual Studio 2022 / `v143`

The script now checks for the required toolset before invoking CMake, so missing `v142` or `v143` is reported as a friendly preflight error.

You can still override both the generator and the toolset if your local machine has a different Visual Studio shell installed.

If your Nuke installs live outside `Program Files`, set:

```powershell
$env:NUKE_INSTALL_ROOTS = "D:\Apps;E:\Tools"
```

## Install for testing

Copy the resulting plug-in into one of these locations:

- your `~/.nuke` directory
- a custom directory on `NUKE_PATH`

On startup, Nuke should discover `gifWriter`, and `.gif` should become available as a native write format.

## Source layout

- [CMakeLists.txt](/D:/002_Projekt/NukePlugins/GifExporter/CMakeLists.txt)
- [cmake/FindNuke.cmake](/D:/002_Projekt/NukePlugins/GifExporter/cmake/FindNuke.cmake)
- [scripts/build-installed-nukes.ps1](/D:/002_Projekt/NukePlugins/GifExporter/scripts/build-installed-nukes.ps1)
- [src/GifWriter.h](/D:/002_Projekt/NukePlugins/GifExporter/src/GifWriter.h)
- [src/GifWriter.cpp](/D:/002_Projekt/NukePlugins/GifExporter/src/GifWriter.cpp)
- [docs/gif_writer_plan.md](/D:/002_Projekt/NukePlugins/GifExporter/docs/gif_writer_plan.md)

## Phase 2 result

The Phase 2 Nuke execution test established the following:

- `movie() == false` cannot render a multi-frame GIF sequence in Nuke
- animated GIF export must behave as a movie writer
- Nuke calls `execute()` once per frame
- Nuke calls `finish()` once after the frame range completes
- the output filename remains constant across the sequence

The plug-in keeps an optional `diagnostics` checkbox for this lifecycle logging.
When enabled, it appends events to:

- `<output gif path>.gifwriter-phase2.log`

## Next step

The next implementation work is quality and hardening:

- improve palette quality beyond the current fixed color cube
- reduce animated GIF file size further with smarter transparency-aware frame differencing
- harden loop-count semantics and viewer compatibility edge cases
- expand diagnostics and compatibility testing across common GIF viewers
