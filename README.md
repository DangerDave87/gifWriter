# Gif Exporter for Nuke

This repository contains a native NDK-based GIF writer plug-in for Nuke.

## Current status

The plug-in now provides a native animated GIF writer for Nuke with:

- CMake-based project layout
- native `gifWriter` shared-library target
- `DD::Image::FileWriter`-based writer registration for `.gif`
- GIF export settings UI
- real still-frame and animated GIF encoding
- whole-sequence buffering for shared animation palette generation
- adaptive global palette generation from sampled frames
- spec-correct GIF LZW compression
- palette-size control
- automatic frame differencing for smaller opaque animations
- loop control, channel-driven transparency, matte color, fps, and dither options

The writer behaves like a movie writer:

- `movie()` always returns `true`
- `execute()` is called once per input frame
- source frames are buffered during `execute()`
- the final GIF is encoded and written in `finish()`

The export pipeline currently does the following:

- RGBA is read back from Nuke rows
- output conversion uses the writer LUT path via `to_byte()`
- a shared adaptive palette is built from sampled animation frames
- pixels are quantized into that shared indexed palette
- GIF image data is compressed with a dictionary-based LZW encoder
- the `max colors` knob reduces the effective palette size
- optional dithering, transparency thresholding, and matte flattening are applied
- Write `channels=rgba` produces transparent GIF output, while `channels=rgb` flattens to the selected matte color
- looping is controlled by the loop mode / loop count knobs
- frame timing is derived from the `fps` knob
- opaque animations automatically crop frames to changed regions to reduce file size

The current implementation is now producing competitive results against the project reference exports for the tested sequence. Transparent animations still use conservative frame handling, so there is still room for additional size reductions on more difficult shots.

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

## Next step

The next implementation work is hardening and broader coverage:

- reduce animated GIF file size further with smarter transparency-aware frame differencing
- harden loop-count semantics and viewer compatibility edge cases
- test across a wider set of transparency-heavy and gradient-heavy sequences
- package and verify the writer across the supported Nuke versions
