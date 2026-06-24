# Gif Exporter for Nuke

This repository contains a native NDK-based GIF writer plug-in for Nuke.

![gifWriter UI](https://github.com/DangerDave87/misc/blob/main/screenshots/gifWriter.jpg?raw=1)

## Writer settings

- `loop mode`: `Infinite` loops forever, `Fixed` shows a loop count field, and `No Loop` plays the animation once.
- `fps`: defaults to the Nuke project frame rate and controls the GIF frame delay. A single-frame render is still supported.
- `max colors`: `256`, `128`, `64`, or `32`. Lower values usually reduce file size, but can simplify gradients and fine color detail.
- `dither`: `None`, `Floyd-Steinberg`, or `Ordered`. Dithering can improve gradients, but may add visible texture or noise.
- `alpha min`: a `0-1` threshold used when the selected Write channels include alpha. Values at or below the threshold become fully transparent.
- `channels`: the writer uses the selected Write channels in order, so it also works with non-`rgba` layers.

Layer and channel behavior:

- `1` selected channel: treated as alpha, with black RGB.
- `2` selected channels: first channel is used as grayscale RGB, second channel as alpha.
- `3` selected channels: used as RGB.
- `4` or more selected channels: first three channels are used as RGB and the fourth as alpha.

## Expected output binary

The plug-in target is named:

- `gifWriter.dll` on Windows
- `gifWriter.so` on Linux
- `gifWriter.dylib` on macOS

Using the `gifWriter` name matches Nuke's writer naming convention for `.gif`.

## Build

The project is built with CMake and expects a local Nuke install or NDK that provides the `include` and `DDImage` library paths. The helper scripts detect installed Nuke versions, build once per version, and copy the final plug-in into the packaged Nuke folder layout under `nuke/`. On macOS, the output also includes an architecture subfolder so Intel and Apple Silicon builds stay separate.

### Windows

Requirements:

- CMake `3.21+`
- Visual Studio Build Tools or Visual Studio Community/Professional installed via Visual Studio Installer
- the MSVC toolset expected by your target Nuke version
- MSBuild
- a Windows SDK
- a local Nuke install such as `C:\Program Files\Nuke15.1v4`

Where to get them:

- install CMake from [cmake.org](https://cmake.org/download/) or with `winget install Kitware.CMake`
- install Visual Studio Build Tools from Microsoft and select `Desktop development with C++`
- in Visual Studio Installer, add the matching optional MSVC components such as `v141`, `v142`, or `v143`

Default compiler policy used by the Windows script:

- Nuke `13.2` -> `v141`
- Nuke `14.x` -> `v142`
- Nuke `15.x` -> `v142`
- Nuke `16.x` -> `v142`
- Nuke `17.x` -> `v143`

Windows build script:

- [scripts/build-plugin-windows.ps1](/D:/002_Projekt/NukePlugins/GifExporter/scripts/build-plugin-windows.ps1)

What it does:

- scans common install roots and Windows uninstall entries for Nuke installs
- picks the highest installed patch release for each requested minor version
- configures and builds `gifWriter.dll`
- copies the result to `nuke/Windows/<minor>/gifWriter.dll`

How to run it:

```powershell
# Show detected Nuke installs
powershell -ExecutionPolicy Bypass -File .\scripts\build-plugin-windows.ps1 -ListOnly

# Build one version
powershell -ExecutionPolicy Bypass -File .\scripts\build-plugin-windows.ps1 -Versions 15.1

# Build multiple versions
powershell -ExecutionPolicy Bypass -File .\scripts\build-plugin-windows.ps1 -Versions 13.2,15.1,16.1,17.0

# Clean and rebuild
powershell -ExecutionPolicy Bypass -File .\scripts\build-plugin-windows.ps1 -Versions 15.1 -Clean
```

If your Nuke installs are not under `Program Files`, set custom search roots first:

```powershell
$env:NUKE_INSTALL_ROOTS = "D:\Apps;E:\Tools"
```

### Linux

Requirements:

- CMake `3.21+`
- a working C++ toolchain compatible with your target Nuke version
- `make` or another generator backend that CMake can drive
- OpenGL development headers, including `GL/glu.h`
- a local Nuke install or NDK under a searchable location such as `/usr/local` or `/opt`

Where to get the C++ toolchain:

- on Ubuntu or Debian: `sudo apt install build-essential cmake libglu1-mesa-dev mesa-common-dev`
- on Rocky, RHEL, or AlmaLinux: `sudo dnf install gcc-c++ make cmake mesa-libGLU-devel mesa-libGL-devel`
- on Fedora: `sudo dnf install gcc-c++ make cmake mesa-libGLU-devel mesa-libGL-devel`
- on openSUSE: `sudo zypper install gcc-c++ make cmake Mesa-libGLU-devel Mesa-libGL-devel`

The important pieces are a C++ compiler such as `g++` or `clang++`, the standard library headers, `make`, and the Mesa/OpenGL development headers that provide `GL/glu.h`.

On some Rocky Linux 9 systems, especially when Nuke or its Qt/XCB/OpenGL dependencies are not fully present yet, you may also need the following extra packages:

```bash
sudo dnf install -y dnf-plugins-core && \
sudo dnf config-manager --set-enabled crb && \
sudo dnf makecache && \
sudo dnf install -y \
  libxcb \
  libxcb-devel \
  xcb-proto \
  xcb-util \
  xcb-util-devel \
  xcb-util-wm \
  xcb-util-wm-devel \
  xcb-util-image \
  xcb-util-image-devel \
  xcb-util-keysyms \
  xcb-util-keysyms-devel \
  xcb-util-renderutil \
  xcb-util-renderutil-devel \
  xcb-util-cursor \
  xcb-util-cursor-devel \
  libxkbcommon-x11 \
  libxkbcommon-devel \
  libX11-xcb \
  libX11-devel \
  libglvnd \
  libglvnd-opengl \
  libglvnd-glx \
  libglvnd-devel \
  mesa-libGL-devel \
  mesa-libEGL-devel
```

Linux build script:

- [scripts/build-plugin-linux.sh](/D:/002_Projekt/NukePlugins/GifExporter/scripts/build-plugin-linux.sh)

What it does:

- scans common Linux install roots for directories named like `Nuke15.1v4`
- checks that the active compiler can find `GL/glu.h` before starting the build
- picks the highest installed patch release for each requested minor version
- configures and builds `gifWriter.so`
- copies the result to `nuke/Linux/<minor>/gifWriter.so`

How to run it:

```bash
# Show detected Nuke installs
bash ./scripts/build-plugin-linux.sh --list-only

# Build one version
bash ./scripts/build-plugin-linux.sh --versions 15.1

# Build multiple versions
bash ./scripts/build-plugin-linux.sh --versions 15.1,16.1,17.0

# Clean and rebuild
bash ./scripts/build-plugin-linux.sh --versions 15.1 --clean
```

If your Nuke installs are in custom locations, set colon-separated search roots first:

```bash
export NUKE_INSTALL_ROOTS="/srv/apps:/mnt/tools"
```

### macOS

Requirements:

- CMake `3.21+`
- Apple Xcode Command Line Tools
- a local Nuke install or NDK, typically under `/Applications`

Where to get them:

- install Apple Command Line Tools with `xcode-select --install`
- install CMake from [cmake.org](https://cmake.org/download/) or with Homebrew: `brew install cmake`

The Apple Command Line Tools provide `clang`, `clang++`, the macOS SDK, and the developer headers needed for CMake builds.

The macOS helper script builds a single-architecture plug-in per run. This matches Foundry's separate Intel and Apple Silicon Nuke installers more closely and avoids trying to produce a universal binary when the target Nuke libraries are not universal.

macOS build script:

- [scripts/build-plugin-macos.sh](/D:/002_Projekt/NukePlugins/GifExporter/scripts/build-plugin-macos.sh)

What it does:

- scans common macOS install roots for directories or app bundles named like `Nuke15.1v4`
- picks the highest installed patch release for each requested minor version
- resolves one target architecture per build: `x86_64`, `arm64`, or `auto`
- when `--architectures auto` is used, it inspects the target Nuke install and picks one compatible architecture
- configures and builds `gifWriter.dylib` for that single architecture
- copies the result to `nuke/MacOS/<minor>/<arch>/gifWriter.dylib`

How to run it:

```bash
# Show detected Nuke installs
bash ./scripts/build-plugin-macos.sh --list-only

# Build one version
bash ./scripts/build-plugin-macos.sh --versions 15.1

# Build multiple versions
bash ./scripts/build-plugin-macos.sh --versions 15.1,16.1,17.0

# Build an Apple Silicon plug-in
bash ./scripts/build-plugin-macos.sh --versions 15.1 --architectures arm64

# Build an Intel plug-in
bash ./scripts/build-plugin-macos.sh --versions 15.1 --architectures x86_64

# Clean and rebuild
bash ./scripts/build-plugin-macos.sh --versions 15.1 --clean
```

If your Nuke installs are in custom locations, set colon-separated search roots first:

```bash
export NUKE_INSTALL_ROOTS="/Volumes/Apps:/Users/me/Applications"
```

### Manual configure

If you prefer to build without the helper scripts, pass `NUKE_ROOT` to CMake directly:

```powershell
cmake -S . -B build -G "Visual Studio 16 2019" -A x64 -DNUKE_ROOT="C:\Program Files\Nuke15.1v4"
cmake --build build --config Release
```

```bash
cmake -S . -B build -DNUKE_ROOT=/usr/local/Nuke15.1v4 -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

```bash
cmake -S . -B build -DNUKE_ROOT=/Applications/Nuke15.1v4/Nuke15.1v4.app/Contents/MacOS -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build
```

If auto-detection is not enough, you can also pass:

- `-DNUKE_INCLUDE_DIR=...`
- `-DNUKE_DDIMAGE_LIBRARY=...`

## Install plugin

Add the [nuke](/D:/002_Projekt/NukePlugins/GifExporter/nuke) folder to one of these locations:

- your `~/.nuke` directory
- a custom directory on `NUKE_PATH`

On startup, [nuke/init.py](/D:/002_Projekt/NukePlugins/GifExporter/nuke/init.py) adds the correct platform and version subfolder automatically, and `.gif` should become available as a native write format.

## Source layout

- [CMakeLists.txt](/D:/002_Projekt/NukePlugins/GifExporter/CMakeLists.txt)
- [cmake/FindNuke.cmake](/D:/002_Projekt/NukePlugins/GifExporter/cmake/FindNuke.cmake)
- [scripts/build-plugin-windows.ps1](/D:/002_Projekt/NukePlugins/GifExporter/scripts/build-plugin-windows.ps1)
- [scripts/build-plugin-linux.sh](/D:/002_Projekt/NukePlugins/GifExporter/scripts/build-plugin-linux.sh)
- [scripts/build-plugin-macos.sh](/D:/002_Projekt/NukePlugins/GifExporter/scripts/build-plugin-macos.sh)
- [nuke/init.py](/D:/002_Projekt/NukePlugins/GifExporter/nuke/init.py)
- [src/GifWriter.h](/D:/002_Projekt/NukePlugins/GifExporter/src/GifWriter.h)
- [src/GifWriter.cpp](/D:/002_Projekt/NukePlugins/GifExporter/src/GifWriter.cpp)
- [docs/gif_writer_plan.md](/D:/002_Projekt/NukePlugins/GifExporter/docs/gif_writer_plan.md)
