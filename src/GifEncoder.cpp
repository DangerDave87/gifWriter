#include "GifEncoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace GifExporter {

namespace {

constexpr int kOrderedDitherSize = 4;
constexpr std::array<int, kOrderedDitherSize * kOrderedDitherSize> kBayer4x4 = {
    0, 8, 2, 10,
    12, 4, 14, 6,
    3, 11, 1, 9,
    15, 7, 13, 5};

constexpr std::array<std::uint8_t, 3> kLevels3 = {0, 128, 255};
constexpr std::array<std::uint8_t, 4> kLevels4 = {0, 85, 170, 255};
constexpr std::array<std::uint8_t, 5> kLevels5 = {0, 64, 128, 191, 255};
constexpr std::array<std::uint8_t, 6> kLevels6 = {0, 51, 102, 153, 204, 255};
constexpr std::array<std::uint8_t, 7> kLevels7 = {0, 43, 85, 128, 170, 213, 255};

struct PaletteColor {
  std::uint8_t red;
  std::uint8_t green;
  std::uint8_t blue;
};

struct PaletteConfig {
  int tableSize;
  const std::uint8_t* redLevels;
  int redCount;
  const std::uint8_t* greenLevels;
  int greenCount;
  const std::uint8_t* blueLevels;
  int blueCount;
  int activeColorCount;
  std::uint8_t transparentIndex;
  int minimumCodeSize;
  std::uint8_t packedField;
};

struct DiffRect {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  int width() const { return right - left; }
  int height() const { return bottom - top; }
};

int sanitizeMaxColors(int maxColors) {
  if (maxColors <= 32) {
    return 32;
  }
  if (maxColors <= 64) {
    return 64;
  }
  if (maxColors <= 128) {
    return 128;
  }
  return 256;
}

int paletteSizeCode(int tableSize) {
  switch (tableSize) {
  case 32:
    return 4;
  case 64:
    return 5;
  case 128:
    return 6;
  default:
    return 7;
  }
}

int paletteMinimumCodeSize(int tableSize) {
  switch (tableSize) {
  case 32:
    return 5;
  case 64:
    return 6;
  case 128:
    return 7;
  default:
    return 8;
  }
}

PaletteConfig makePaletteConfig(const GifEncoderOptions& options) {
  const int tableSize = sanitizeMaxColors(options.maxColors);

  PaletteConfig config{};
  config.tableSize = tableSize;
  config.transparentIndex = static_cast<std::uint8_t>(tableSize - 1);
  config.minimumCodeSize = paletteMinimumCodeSize(tableSize);
  config.packedField = static_cast<std::uint8_t>(0x80 | 0x70 | paletteSizeCode(tableSize));

  switch (tableSize) {
  case 32:
    config.redLevels = kLevels3.data();
    config.redCount = static_cast<int>(kLevels3.size());
    config.greenLevels = kLevels3.data();
    config.greenCount = static_cast<int>(kLevels3.size());
    config.blueLevels = kLevels3.data();
    config.blueCount = static_cast<int>(kLevels3.size());
    break;
  case 64:
    config.redLevels = kLevels4.data();
    config.redCount = static_cast<int>(kLevels4.size());
    config.greenLevels = kLevels4.data();
    config.greenCount = static_cast<int>(kLevels4.size());
    config.blueLevels = kLevels4.data();
    config.blueCount = static_cast<int>(kLevels4.size());
    break;
  case 128:
    config.redLevels = kLevels5.data();
    config.redCount = static_cast<int>(kLevels5.size());
    config.greenLevels = kLevels5.data();
    config.greenCount = static_cast<int>(kLevels5.size());
    config.blueLevels = kLevels5.data();
    config.blueCount = static_cast<int>(kLevels5.size());
    break;
  default:
    config.redLevels = kLevels6.data();
    config.redCount = static_cast<int>(kLevels6.size());
    config.greenLevels = kLevels7.data();
    config.greenCount = static_cast<int>(kLevels7.size());
    config.blueLevels = kLevels6.data();
    config.blueCount = static_cast<int>(kLevels6.size());
    break;
  }

  config.activeColorCount = config.redCount * config.greenCount * config.blueCount;
  if (options.useTransparency && config.activeColorCount >= config.tableSize) {
    config.activeColorCount = config.tableSize - 1;
  }
  config.activeColorCount = std::max(1, config.activeColorCount);

  return config;
}

int quantizeLevel(float value, int maxIndex) {
  const float scaled = std::clamp(value, 0.0F, 255.0F) * static_cast<float>(maxIndex) / 255.0F;
  const int quantized = static_cast<int>(std::lround(scaled));
  return std::clamp(quantized, 0, maxIndex);
}

std::uint8_t paletteIndexFor(float red, float green, float blue, const PaletteConfig& config) {
  const int rIndex = quantizeLevel(red, config.redCount - 1);
  const int gIndex = quantizeLevel(green, config.greenCount - 1);
  const int bIndex = quantizeLevel(blue, config.blueCount - 1);
  const int index = (rIndex * config.greenCount + gIndex) * config.blueCount + bIndex;
  return static_cast<std::uint8_t>(std::clamp(index, 0, config.activeColorCount - 1));
}

PaletteColor paletteColorFor(std::uint8_t index, const PaletteConfig& config) {
  int clampedIndex = std::clamp<int>(index, 0, config.activeColorCount - 1);
  const int redIndex = clampedIndex / (config.greenCount * config.blueCount);
  const int remainder = clampedIndex % (config.greenCount * config.blueCount);
  const int greenIndex = remainder / config.blueCount;
  const int blueIndex = remainder % config.blueCount;

  return {
      config.redLevels[redIndex],
      config.greenLevels[greenIndex],
      config.blueLevels[blueIndex]};
}

void appendU16LE(std::vector<std::uint8_t>& bytes, int value) {
  bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
  bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
}

void appendGraphicControlExtension(
    std::vector<std::uint8_t>& bytes,
    bool useTransparency,
    std::uint8_t transparentIndex,
    int delayCentiseconds,
    int disposalMethod) {
  bytes.push_back(0x21);
  bytes.push_back(0xf9);
  bytes.push_back(0x04);
  bytes.push_back(static_cast<std::uint8_t>(((disposalMethod & 0x07) << 2) | (useTransparency ? 0x01 : 0x00)));
  appendU16LE(bytes, std::clamp(delayCentiseconds, 0, 65535));
  bytes.push_back(useTransparency ? transparentIndex : 0x00);
  bytes.push_back(0x00);
}

void appendApplicationLoopExtension(
    std::vector<std::uint8_t>& bytes,
    const GifEncoderOptions& options) {
  if (options.loopMode == GifLoopMode::kNone) {
    return;
  }

  const int loopCount = options.loopMode == GifLoopMode::kInfinite
      ? 0
      : std::clamp(options.loopCount, 1, 65535);

  bytes.push_back(0x21);
  bytes.push_back(0xff);
  bytes.push_back(0x0b);
  bytes.insert(bytes.end(), {'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0'});
  bytes.push_back(0x03);
  bytes.push_back(0x01);
  appendU16LE(bytes, loopCount);
  bytes.push_back(0x00);
}

class GifBitPacker {
public:
  void writeCode(int code, int bitCount) {
    bitBuffer_ |= (static_cast<std::uint32_t>(code) << bufferedBits_);
    bufferedBits_ += bitCount;

    while (bufferedBits_ >= 8) {
      bytes_.push_back(static_cast<std::uint8_t>(bitBuffer_ & 0xffU));
      bitBuffer_ >>= 8;
      bufferedBits_ -= 8;
    }
  }

  std::vector<std::uint8_t> finish() {
    if (bufferedBits_ > 0) {
      bytes_.push_back(static_cast<std::uint8_t>(bitBuffer_ & 0xffU));
      bitBuffer_ = 0;
      bufferedBits_ = 0;
    }
    return bytes_;
  }

private:
  std::vector<std::uint8_t> bytes_;
  std::uint32_t bitBuffer_ = 0;
  int bufferedBits_ = 0;
};

std::vector<std::uint8_t> encodeLzwIndices(const std::vector<std::uint8_t>& indices, int minimumCodeSize) {
  const int clearCode = 1 << minimumCodeSize;
  const int endOfInformationCode = clearCode + 1;

  GifBitPacker packer;
  int codeSize = minimumCodeSize + 1;
  int nextDictionaryCode = endOfInformationCode + 1;
  bool firstLiteralAfterReset = true;

  packer.writeCode(clearCode, codeSize);

  for (std::uint8_t index : indices) {
    packer.writeCode(index, codeSize);

    if (firstLiteralAfterReset) {
      firstLiteralAfterReset = false;
      continue;
    }

    ++nextDictionaryCode;
    if (nextDictionaryCode == (1 << codeSize) && codeSize < 12) {
      ++codeSize;
    }

    if (nextDictionaryCode >= 4096) {
      packer.writeCode(clearCode, codeSize);
      codeSize = minimumCodeSize + 1;
      nextDictionaryCode = endOfInformationCode + 1;
      firstLiteralAfterReset = true;
    }
  }

  packer.writeCode(endOfInformationCode, codeSize);
  return packer.finish();
}

void appendImageDataBlocks(
    std::vector<std::uint8_t>& bytes,
    const std::vector<std::uint8_t>& compressedBytes) {
  std::size_t offset = 0;
  while (offset < compressedBytes.size()) {
    const std::size_t remaining = compressedBytes.size() - offset;
    const std::size_t blockSize = std::min<std::size_t>(255, remaining);
    bytes.push_back(static_cast<std::uint8_t>(blockSize));
    bytes.insert(bytes.end(), compressedBytes.begin() + static_cast<std::ptrdiff_t>(offset),
                 compressedBytes.begin() + static_cast<std::ptrdiff_t>(offset + blockSize));
    offset += blockSize;
  }
  bytes.push_back(0x00);
}

void buildGlobalPalette(
    std::vector<std::uint8_t>& paletteBytes,
    const GifEncoderOptions& options,
    const PaletteConfig& config) {
  paletteBytes.assign(static_cast<std::size_t>(config.tableSize) * 3U, 0);

  for (int index = 0; index < config.activeColorCount; ++index) {
    const PaletteColor color = paletteColorFor(static_cast<std::uint8_t>(index), config);
    const int paletteOffset = index * 3;
    paletteBytes[paletteOffset + 0] = color.red;
    paletteBytes[paletteOffset + 1] = color.green;
    paletteBytes[paletteOffset + 2] = color.blue;
  }

  const int fillStart = config.activeColorCount * 3;
  if (fillStart < static_cast<int>(paletteBytes.size())) {
    const PaletteColor lastColor = paletteColorFor(static_cast<std::uint8_t>(config.activeColorCount - 1), config);
    for (std::size_t offset = static_cast<std::size_t>(fillStart); offset < paletteBytes.size(); offset += 3) {
      paletteBytes[offset + 0] = lastColor.red;
      paletteBytes[offset + 1] = lastColor.green;
      paletteBytes[offset + 2] = lastColor.blue;
    }
  }

  if (options.useTransparency) {
    const int transparentOffset = static_cast<int>(config.transparentIndex) * 3;
    paletteBytes[transparentOffset + 0] = options.matteRed;
    paletteBytes[transparentOffset + 1] = options.matteGreen;
    paletteBytes[transparentOffset + 2] = options.matteBlue;
  }
}

void convertPixelsToIndexed(
    int width,
    int height,
    const std::vector<std::uint8_t>& rgbaPixels,
    const GifEncoderOptions& options,
    const PaletteConfig& config,
    std::vector<std::uint8_t>& indexedPixels,
    bool& hasTransparentPixels) {
  indexedPixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
  hasTransparentPixels = false;

  auto composePixel = [&](std::size_t pixelOffset, float& red, float& green, float& blue, bool& transparent) {
    const float sourceRed = static_cast<float>(rgbaPixels[pixelOffset + 0]);
    const float sourceGreen = static_cast<float>(rgbaPixels[pixelOffset + 1]);
    const float sourceBlue = static_cast<float>(rgbaPixels[pixelOffset + 2]);
    const std::uint8_t alphaByte = rgbaPixels[pixelOffset + 3];

    transparent = options.useTransparency && alphaByte <= options.transparentAlphaThreshold;
    if (transparent) {
      red = green = blue = 0.0F;
      return;
    }

    const float alpha = static_cast<float>(alphaByte) / 255.0F;
    const bool needsMatteComposite = !options.useTransparency || alphaByte < 255;
    if (needsMatteComposite) {
      red = sourceRed * alpha + static_cast<float>(options.matteRed) * (1.0F - alpha);
      green = sourceGreen * alpha + static_cast<float>(options.matteGreen) * (1.0F - alpha);
      blue = sourceBlue * alpha + static_cast<float>(options.matteBlue) * (1.0F - alpha);
    } else {
      red = sourceRed;
      green = sourceGreen;
      blue = sourceBlue;
    }
  };

  if (options.ditherMode == GifDitherMode::kNone) {
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const std::size_t pixelIndex = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
        const std::size_t pixelOffset = pixelIndex * 4U;

        float red = 0.0F;
        float green = 0.0F;
        float blue = 0.0F;
        bool transparent = false;
        composePixel(pixelOffset, red, green, blue, transparent);

        if (transparent) {
          indexedPixels[pixelIndex] = config.transparentIndex;
          hasTransparentPixels = true;
          continue;
        }

        indexedPixels[pixelIndex] = paletteIndexFor(red, green, blue, config);
      }
    }
    return;
  }

  if (options.ditherMode == GifDitherMode::kOrdered) {
    constexpr float kOrderedStrength = 18.0F;

    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const std::size_t pixelIndex = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
        const std::size_t pixelOffset = pixelIndex * 4U;

        float red = 0.0F;
        float green = 0.0F;
        float blue = 0.0F;
        bool transparent = false;
        composePixel(pixelOffset, red, green, blue, transparent);

        if (transparent) {
          indexedPixels[pixelIndex] = config.transparentIndex;
          hasTransparentPixels = true;
          continue;
        }

        const int matrixIndex = (y % kOrderedDitherSize) * kOrderedDitherSize + (x % kOrderedDitherSize);
        const float threshold = (static_cast<float>(kBayer4x4[matrixIndex]) + 0.5F) / 16.0F - 0.5F;
        const float offset = threshold * kOrderedStrength;

        indexedPixels[pixelIndex] = paletteIndexFor(
            std::clamp(red + offset, 0.0F, 255.0F),
            std::clamp(green + offset, 0.0F, 255.0F),
            std::clamp(blue + offset, 0.0F, 255.0F),
            config);
      }
    }
    return;
  }

  std::vector<float> currentErrorRow(static_cast<std::size_t>(width + 2) * 3U, 0.0F);
  std::vector<float> nextErrorRow(static_cast<std::size_t>(width + 2) * 3U, 0.0F);

  for (int y = 0; y < height; ++y) {
    std::fill(nextErrorRow.begin(), nextErrorRow.end(), 0.0F);

    for (int x = 0; x < width; ++x) {
      const std::size_t pixelIndex = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
      const std::size_t pixelOffset = pixelIndex * 4U;

      float red = 0.0F;
      float green = 0.0F;
      float blue = 0.0F;
      bool transparent = false;
      composePixel(pixelOffset, red, green, blue, transparent);

      if (transparent) {
        indexedPixels[pixelIndex] = config.transparentIndex;
        hasTransparentPixels = true;
        continue;
      }

      const std::size_t errorIndex = static_cast<std::size_t>(x + 1) * 3U;
      const float correctedRed = std::clamp(red + currentErrorRow[errorIndex + 0], 0.0F, 255.0F);
      const float correctedGreen = std::clamp(green + currentErrorRow[errorIndex + 1], 0.0F, 255.0F);
      const float correctedBlue = std::clamp(blue + currentErrorRow[errorIndex + 2], 0.0F, 255.0F);

      const std::uint8_t paletteIndex = paletteIndexFor(correctedRed, correctedGreen, correctedBlue, config);
      indexedPixels[pixelIndex] = paletteIndex;

      const PaletteColor quantized = paletteColorFor(paletteIndex, config);
      const float redError = correctedRed - static_cast<float>(quantized.red);
      const float greenError = correctedGreen - static_cast<float>(quantized.green);
      const float blueError = correctedBlue - static_cast<float>(quantized.blue);

      currentErrorRow[errorIndex + 3] += redError * (7.0F / 16.0F);
      currentErrorRow[errorIndex + 4] += greenError * (7.0F / 16.0F);
      currentErrorRow[errorIndex + 5] += blueError * (7.0F / 16.0F);

      nextErrorRow[errorIndex - 3] += redError * (3.0F / 16.0F);
      nextErrorRow[errorIndex - 2] += greenError * (3.0F / 16.0F);
      nextErrorRow[errorIndex - 1] += blueError * (3.0F / 16.0F);

      nextErrorRow[errorIndex + 0] += redError * (5.0F / 16.0F);
      nextErrorRow[errorIndex + 1] += greenError * (5.0F / 16.0F);
      nextErrorRow[errorIndex + 2] += blueError * (5.0F / 16.0F);

      nextErrorRow[errorIndex + 3] += redError * (1.0F / 16.0F);
      nextErrorRow[errorIndex + 4] += greenError * (1.0F / 16.0F);
      nextErrorRow[errorIndex + 5] += blueError * (1.0F / 16.0F);
    }

    currentErrorRow.swap(nextErrorRow);
  }
}

DiffRect findChangedRect(
    int width,
    int height,
    const std::vector<std::uint8_t>& currentPixels,
    const std::vector<std::uint8_t>& previousPixels) {
  DiffRect rect{};
  bool foundChange = false;

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const std::size_t pixelIndex = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
      if (currentPixels[pixelIndex] == previousPixels[pixelIndex]) {
        continue;
      }

      if (!foundChange) {
        rect.left = x;
        rect.right = x + 1;
        rect.top = y;
        rect.bottom = y + 1;
        foundChange = true;
      } else {
        rect.left = std::min(rect.left, x);
        rect.right = std::max(rect.right, x + 1);
        rect.top = std::min(rect.top, y);
        rect.bottom = std::max(rect.bottom, y + 1);
      }
    }
  }

  if (!foundChange) {
    rect.left = 0;
    rect.top = 0;
    rect.right = 1;
    rect.bottom = 1;
  }

  return rect;
}

void extractSubrectIndices(
    int fullWidth,
    const std::vector<std::uint8_t>& fullFrame,
    const DiffRect& rect,
    std::vector<std::uint8_t>& subrectPixels) {
  subrectPixels.resize(static_cast<std::size_t>(rect.width()) * static_cast<std::size_t>(rect.height()));

  for (int y = rect.top; y < rect.bottom; ++y) {
    const std::size_t srcOffset = static_cast<std::size_t>(y) * static_cast<std::size_t>(fullWidth) + static_cast<std::size_t>(rect.left);
    const std::size_t dstOffset = static_cast<std::size_t>(y - rect.top) * static_cast<std::size_t>(rect.width());
    std::copy_n(
        fullFrame.begin() + static_cast<std::ptrdiff_t>(srcOffset),
        rect.width(),
        subrectPixels.begin() + static_cast<std::ptrdiff_t>(dstOffset));
  }
}

} // namespace

bool QuantizeGifFrame(
    int width,
    int height,
    const std::vector<std::uint8_t>& rgbaPixels,
    const GifEncoderOptions& options,
    GifIndexedFrame& indexedFrame,
    std::string& error) {
  error.clear();

  if (width <= 0 || height <= 0) {
    error = "GIF export requires a positive image size.";
    return false;
  }

  const std::size_t expectedPixelBytes =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
  if (rgbaPixels.size() != expectedPixelBytes) {
    error = "GIF encoder received an unexpected RGBA buffer size.";
    return false;
  }

  const PaletteConfig config = makePaletteConfig(options);
  convertPixelsToIndexed(width, height, rgbaPixels, options, config, indexedFrame.pixels, indexedFrame.hasTransparentPixels);
  return true;
}

bool EncodeSingleFrameGif(
    int width,
    int height,
    const std::vector<std::uint8_t>& rgbaPixels,
    const GifEncoderOptions& options,
    std::vector<std::uint8_t>& gifBytes,
    std::string& error) {
  gifBytes.clear();
  error.clear();

  GifIndexedFrame indexedFrame;
  if (!QuantizeGifFrame(width, height, rgbaPixels, options, indexedFrame, error)) {
    return false;
  }

  if (!EncodeGifAnimationHeader(width, height, options, gifBytes, error)) {
    return false;
  }

  std::vector<std::uint8_t> frameBytes;
  if (!EncodeGifAnimationFrame(width, height, indexedFrame, nullptr, options, false, frameBytes, error)) {
    return false;
  }

  gifBytes.insert(gifBytes.end(), frameBytes.begin(), frameBytes.end());
  EncodeGifAnimationTrailer(gifBytes);
  return true;
}

bool EncodeGifAnimationHeader(
    int width,
    int height,
    const GifEncoderOptions& options,
    std::vector<std::uint8_t>& gifBytes,
    std::string& error) {
  gifBytes.clear();
  error.clear();

  if (width <= 0 || height <= 0) {
    error = "GIF export requires a positive image size.";
    return false;
  }

  const PaletteConfig config = makePaletteConfig(options);
  std::vector<std::uint8_t> paletteBytes;
  buildGlobalPalette(paletteBytes, options, config);

  gifBytes.reserve(13 + paletteBytes.size() + 32);
  gifBytes.insert(gifBytes.end(), {'G', 'I', 'F', '8', '9', 'a'});

  appendU16LE(gifBytes, width);
  appendU16LE(gifBytes, height);
  gifBytes.push_back(config.packedField);
  gifBytes.push_back(options.useTransparency ? config.transparentIndex : 0x00);
  gifBytes.push_back(0x00);

  gifBytes.insert(gifBytes.end(), paletteBytes.begin(), paletteBytes.end());
  appendApplicationLoopExtension(gifBytes, options);

  return true;
}

bool EncodeGifAnimationFrame(
    int width,
    int height,
    const GifIndexedFrame& frame,
    const GifIndexedFrame* previousFrame,
    const GifEncoderOptions& options,
    bool allowFrameDifferencing,
    std::vector<std::uint8_t>& gifBytes,
    std::string& error) {
  gifBytes.clear();
  error.clear();

  if (width <= 0 || height <= 0) {
    error = "GIF export requires a positive image size.";
    return false;
  }

  const std::size_t expectedPixelCount =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  if (frame.pixels.size() != expectedPixelCount) {
    error = "GIF encoder received an unexpected indexed frame size.";
    return false;
  }

  if (previousFrame && previousFrame->pixels.size() != expectedPixelCount) {
    error = "GIF encoder received an unexpected previous indexed frame size.";
    return false;
  }

  const PaletteConfig config = makePaletteConfig(options);

  const bool safeToDifference =
      allowFrameDifferencing &&
      previousFrame &&
      !frame.hasTransparentPixels &&
      !previousFrame->hasTransparentPixels;

  DiffRect rect{};
  std::vector<std::uint8_t> encodedPixels;
  int disposalMethod = 0;

  if (safeToDifference) {
    rect = findChangedRect(width, height, frame.pixels, previousFrame->pixels);
    extractSubrectIndices(width, frame.pixels, rect, encodedPixels);
    disposalMethod = 1;
  } else {
    rect.left = 0;
    rect.top = 0;
    rect.right = width;
    rect.bottom = height;
    encodedPixels = frame.pixels;
    disposalMethod = options.useTransparency && frame.hasTransparentPixels ? 2 : 0;
  }

  const std::vector<std::uint8_t> compressedBytes = encodeLzwIndices(encodedPixels, config.minimumCodeSize);
  gifBytes.reserve(compressedBytes.size() + 32);

  appendGraphicControlExtension(
      gifBytes,
      options.useTransparency && frame.hasTransparentPixels && !safeToDifference,
      config.transparentIndex,
      options.frameDelayCentiseconds,
      disposalMethod);

  gifBytes.push_back(0x2c);
  appendU16LE(gifBytes, rect.left);
  appendU16LE(gifBytes, rect.top);
  appendU16LE(gifBytes, rect.width());
  appendU16LE(gifBytes, rect.height());
  gifBytes.push_back(0x00);

  gifBytes.push_back(static_cast<std::uint8_t>(config.minimumCodeSize));
  appendImageDataBlocks(gifBytes, compressedBytes);

  return true;
}

void EncodeGifAnimationTrailer(std::vector<std::uint8_t>& gifBytes) {
  gifBytes.push_back(0x3b);
}

} // namespace GifExporter
