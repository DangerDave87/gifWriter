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
  std::uint8_t transparentAlphaThreshold = 127;
  GifLoopMode loopMode = GifLoopMode::kInfinite;
  int loopCount = 0;
  int frameDelayCentiseconds = 4;
  int maxColors = 256;
};

bool QuantizeGifFrame(
    int width,
    int height,
    const std::vector<std::uint8_t>& rgbaPixels,
    const GifEncoderOptions& options,
    GifIndexedFrame& indexedFrame,
    std::string& error);

bool EncodeGifAnimationHeader(
    int width,
    int height,
    const GifEncoderOptions& options,
    std::vector<std::uint8_t>& gifBytes,
    std::string& error);

bool EncodeGifAnimationFrame(
    int width,
    int height,
    const GifIndexedFrame& frame,
    const GifIndexedFrame* previousFrame,
    const GifEncoderOptions& options,
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
