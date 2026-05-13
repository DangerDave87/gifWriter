#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace GifExporter {

enum class GifDitherMode {
  kNone = 0,
  kFloydSteinberg = 1,
  kOrdered = 2
};

enum class GifLoopMode {
  kInfinite = 0,
  kFixed = 1,
  kNone = 2
};

struct GifIndexedFrame {
  std::vector<std::uint8_t> pixels;
  bool hasTransparentPixels = false;
};

struct GifEncoderOptions {
  bool useTransparency = false;
  GifDitherMode ditherMode = GifDitherMode::kNone;
  std::uint8_t matteRed = 0;
  std::uint8_t matteGreen = 0;
  std::uint8_t matteBlue = 0;
  std::uint8_t transparentAlphaThreshold = 20;
  GifLoopMode loopMode = GifLoopMode::kInfinite;
  int loopCount = 0;
  int frameDelayCentiseconds = 4;
  int maxColors = 256;
};

struct GifPalette {
  std::vector<std::uint8_t> rgbTable;
  int tableSize = 256;
  int activeColorCount = 256;
  std::uint8_t transparentIndex = 255;
  int minimumCodeSize = 8;
  std::uint8_t packedField = 0xf7;
};

bool BuildAdaptiveGifPalette(
    int width,
    int height,
    const std::vector<std::vector<std::uint8_t>>& rgbaFrames,
    const GifEncoderOptions& options,
    GifPalette& palette,
    std::string& error);

bool QuantizeGifFrameToPalette(
    int width,
    int height,
    const std::vector<std::uint8_t>& rgbaPixels,
    const GifEncoderOptions& options,
    const GifPalette& palette,
    GifIndexedFrame& indexedFrame,
    std::string& error);

bool EncodeGifAnimationHeader(
    int width,
    int height,
    const GifEncoderOptions& options,
    const GifPalette& palette,
    std::vector<std::uint8_t>& gifBytes,
    std::string& error);

bool EncodeGifAnimationFrame(
    int width,
    int height,
    const GifIndexedFrame& frame,
    const GifIndexedFrame* previousFrame,
    const GifEncoderOptions& options,
    const GifPalette& palette,
    bool allowFrameDifferencing,
    std::vector<std::uint8_t>& gifBytes,
    std::string& error);

void EncodeGifAnimationTrailer(std::vector<std::uint8_t>& gifBytes);

bool EncodeSingleFrameGif(
    int width,
    int height,
    const std::vector<std::uint8_t>& rgbaPixels,
    const GifEncoderOptions& options,
    std::vector<std::uint8_t>& gifBytes,
    std::string& error);

} // namespace GifExporter
