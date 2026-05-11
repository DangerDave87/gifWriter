# Native GIF Writer Plan for Nuke

## Goal

Build a native NDK-based Nuke Write plug-in that exports `.gif` files from inside Nuke, ideally supporting animated GIF output from a frame range and a reduced first milestone for single-frame GIF export.

## Documentation Analysis

Primary source reviewed:

- https://learn.foundry.com/nuke/developers/15.0/ndkdevguide/2d/readerswriters.html
- https://learn.foundry.com/nuke/developers/63/ndkdevguide/intro/pluginbuildinginstallation.html
- https://learn.foundry.com/nuke/developers/140/ndkdevguide/appendixa/windows.html

### What the Nuke docs imply for a GIF exporter

1. A native image writer should be implemented by subclassing `DD::Image::Writer` or, preferably, `DD::Image::FileWriter`.
   - The docs explicitly recommend `FileWriter` when ordinary file output is needed.

2. The writer must register a static `Writer::Description`.
   - This is how Nuke associates the plug-in with the `.gif` extension.

3. The core write logic lives in `DD::Image::Writer::execute()`.
   - `execute()` is responsible for requesting image data from `input0()` and writing to `filename()`.

4. Animated-file behavior is tied to `movie() const` and `finish()`.
   - The docs say `movie()` indicates whether the writer is a movie writer.
   - Nuke’s `Write` execution behavior changes when `movie()` is true.
   - `finish()` is the right place for end-of-file work if the format needs finalization.

5. Writer UI is implemented directly on the writer via `knobs()` and `knob_changed()`.
   - This is where GIF-specific options such as looping, frame delay, palette mode, transparency, and dithering belong.

6. Colorspace must be handled on write.
   - Nuke assumes internal image data is linear.
   - If GIF output should be sRGB-like perceptual output, the conversion needs to happen in the writer path or through the writer’s LUT/default colorspace behavior.

7. Metadata access is available but must be pulled from the input explicitly.
   - GIF metadata needs are small, so this is optional for the first version.

8. `FileWriter::open()` and `close()` are useful for safe file output.
   - The class reference indicates `FileWriter` writes to a temporary file and moves it into place on success.

9. Native plug-ins are deployed as shared libraries in `~/.nuke` or a directory on `NUKE_PATH`.
   - For Windows builds, the Nuke docs recommend building compatible plug-ins with the Visual Studio version required by the target Nuke release.

## GIF-Specific Design Implications

These points are engineering inferences from the Nuke writer model plus the GIF format requirements:

1. GIF is palette-based, 8-bit indexed output.
   - Nuke delivers high-precision linear RGBA.
   - We therefore need a conversion path:
     - transform to output colorspace
     - flatten or preserve transparency
     - quantize to 256 colors
     - optionally dither

2. Animated GIF is closer to a movie writer than a still-image sequence writer.
   - A single `.gif` file can contain many frames.
   - That makes `movie() const` a likely requirement for animation support.

3. The exact frame-range execution semantics should be verified with a Nuke smoke test early.
   - The documentation makes it clear that `movie()` affects frame execution and that `finish()` is called for movie writers.
   - It does not, in the reviewed material, fully spell out the ideal animated writer control flow for a first implementation.
   - We should therefore validate this with a minimal writer plug-in before investing heavily in the encoder.

4. A staged rollout is safer than trying to ship full animation first.
   - Single-frame GIF export proves plug-in loading, writer registration, colorspace conversion, and indexed encoding.
   - Animated export can then build on a known-good writer shell.

## Recommended Architecture

### 1. NDK writer layer

Responsible for:

- Nuke registration via `Writer::Description`
- file extension association for `.gif`
- writer knobs
- frame/input access
- write lifecycle (`execute()`, `movie()`, `finish()`)
- error reporting through Nuke

Suggested files:

- `src/GifWriter.cpp`
- `src/GifWriter.h`

### 2. GIF encoding layer

Responsible for:

- GIF headers and logical screen descriptor
- global or local palette generation
- frame disposal and delay metadata
- LZW-compressed image data
- animation trailer/finalization

Suggested files:

- `src/GifEncoder.cpp`
- `src/GifEncoder.h`

### 3. Pixel conversion layer

Responsible for:

- pulling RGBA from Nuke rows/tiles
- converting from Nuke float data to 8-bit display/output space
- optional alpha handling
- palette quantization
- optional dithering

Suggested files:

- `src/GifPixelConvert.cpp`
- `src/GifPixelConvert.h`

### 4. Build and packaging layer

Responsible for:

- locating the NDK headers/libs
- building a `.dll`
- copying or installing to a testable Nuke plug-in path
- bundling any third-party encoder dependency if used

Suggested files:

- `CMakeLists.txt`
- `cmake/FindNuke.cmake` or a small local equivalent
- `README.md`

## Third-Party Dependency Decision

This is the first real architecture decision we should make before coding deeply:

### Option A: Vendor a small permissive GIF encoder

Pros:

- simple distribution
- fewer runtime dependencies
- easier to ship as one plug-in bundle

Cons:

- may still need extra work for good quantization and animated-frame control
- correctness/debugging burden stays with us

### Option B: Use `giflib` or another mature encoder library

Pros:

- stronger format correctness
- less low-level file-format work

Cons:

- extra dependency management
- more build/packaging complexity on Windows

Recommendation:

- Use a small vendored encoder only if it supports animated GIF cleanly and we are comfortable implementing or integrating quantization ourselves.
- Otherwise, use a mature library and treat the NDK writer as the Nuke-facing wrapper.

## Phased Implementation Plan

## Phase 0: Project bootstrap

Deliverables:

- repo structure
- `CMakeLists.txt`
- configurable `NUKE_ROOT` or `NUKE_INCLUDE_DIR` / `NUKE_LIBRARY_DIR`
- basic build instructions

Success criteria:

- project configures on Windows
- empty test plug-in can compile into a Nuke-loadable `.dll`

## Phase 1: Minimal writer skeleton

Deliverables:

- `GifWriter` subclass of `DD::Image::FileWriter`
- static `Writer::Description` registered for `gif`
- minimal `execute()` that opens/closes output and reports clear errors
- placeholder knobs

Success criteria:

- Nuke detects `.gif` as a file type
- Write node can select the writer
- plug-in loads through `NUKE_PATH`

## Phase 2: Execution-model spike

Purpose:

- verify how Nuke invokes our writer across frame ranges for animated output

Deliverables:

- logging or diagnostic build that records:
  - `execute()` calls
  - `frame()` values
  - whether `movie()` changes invocation behavior as expected
  - when `finish()` is called

Success criteria:

- we know whether animated GIF should:
  - stream frames across repeated `execute()` calls, or
  - collect/render frames inside a single movie-style execution path

## Phase 3: Single-frame GIF export

Deliverables:

- RGBA readback from Nuke input
- basic colorspace/output conversion
- 256-color quantization
- optional fixed transparency handling
- valid still GIF file output

Success criteria:

- a single rendered frame exports as a valid `.gif`
- file opens correctly in common viewers
- obvious color issues are documented or corrected

## Phase 4: Writer knobs and UX

Suggested knobs:

- `animated` or inferred movie mode
- `loop_count`
- `frame_delay_ms`
- `dither`
- `palette_mode`
- `transparency_mode`
- `max_colors`

Success criteria:

- knobs appear and persist correctly
- knob changes affect export behavior predictably

## Phase 5: Animated GIF support

Deliverables:

- multi-frame encode path
- loop extension support
- per-frame delay handling
- disposal strategy
- final trailer written in `finish()` or equivalent finalization point

Success criteria:

- rendering a frame range produces one animated `.gif`
- animation timing is stable
- playback works in browsers and desktop viewers

## Phase 6: Robustness and compatibility

Deliverables:

- clear error messages for unsupported cases
- abort handling through `aborted()`
- large-frame and long-sequence sanity checks
- documented behavior for stereo/views
- documented behavior for alpha

Success criteria:

- export cancels safely
- failed writes do not leave corrupt final files
- unsupported scenarios fail loudly and clearly

## Phase 7: Packaging and developer workflow

Deliverables:

- install instructions for `.nuke` / `NUKE_PATH`
- sample `menu.py` or test instructions
- optional post-build deployment step
- test checklist

Success criteria:

- another machine with a matching Nuke SDK can build and load the plug-in

## Suggested Milestone Order

Recommended order:

1. Build a loadable writer shell
2. Prove `.gif` selection and Nuke integration
3. Export a single still GIF
4. Verify movie-writer execution semantics
5. Add animated GIF output
6. Harden packaging and UX

## Main Risks

1. Animated execution behavior inside Nuke may be slightly less obvious than the high-level docs suggest.
2. Good palette quantization may matter more than the raw file-writing code for perceived quality.
3. Colorspace mistakes can make the output look washed out or too dark.
4. Third-party dependency choice can dominate build complexity on Windows.

## Recommended First Implementation Target

The best first target is:

- a native `FileWriter`-based `.gif` writer
- Windows-first build setup
- single-frame GIF output first
- animated GIF as the second milestone after validating `movie()` / `finish()` behavior in Nuke

This keeps the early work focused on proving the Nuke integration before we spend time on palette quality and animation edge cases.
