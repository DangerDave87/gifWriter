#include "GifEncoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace GifExporter {

namespace {

constexpr int kOrderedDitherSize = 4;
constexpr int kNearestColorBits = 6;
constexpr int kNearestColorLevels = 1 << kNearestColorBits;
constexpr int kNearestColorShift = 8 - kNearestColorBits;
constexpr std::size_t kNearestColorLookupSize =
    static_cast<std::size_t>(kNearestColorLevels) *
    static_cast<std::size_t>(kNearestColorLevels) *
    static_cast<std::size_t>(kNearestColorLevels);
constexpr std::size_t kMaxPaletteSamples = 65536;
constexpr std::size_t kPaletteSamplesPerFrame = 8192;
constexpr std::array<int, kOrderedDitherSize * kOrderedDitherSize> kBayer4x4 = {
    0, 8, 2, 10,
    12, 4, 14, 6,
    3, 11, 1, 9,
    15, 7, 13, 5};

struct PaletteColor {
  std::uint8_t red;
  std::uint8_t green;
  std::uint8_t blue;
};

struct DiffRect {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  int width() const { return right - left; }
  int height() const { return bottom - top; }
};

struct ColorSample {
  std::uint8_t red;
  std::uint8_t green;
  std::uint8_t blue;
};

struct ColorBox {
  std::size_t begin = 0;
  std::size_t end = 0;
  std::uint8_t minRed = 0;
  std::uint8_t maxRed = 0;
  std::uint8_t minGreen = 0;
  std::uint8_t maxGreen = 0;
  std::uint8_t minBlue = 0;
  std::uint8_t maxBlue = 0;
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
  explicit GifBitPacker(std::size_t expectedBytes) {
    bytes_.reserve(expectedBytes);
  }

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
  if (indices.empty()) {
    return {};
  }

  const int clearCode = 1 << minimumCodeSize;
  const int endOfInformationCode = clearCode + 1;

  std::unordered_map<std::uint32_t, int> dictionary;
  dictionary.reserve(4096);

  auto resetDictionary = [&]() {
    dictionary.clear();
  };

  GifBitPacker packer(indices.size() / 2U + 16U);
  int packedCodeSize = minimumCodeSize + 1;
  int packedNextDictionaryCode = endOfInformationCode + 1;
  bool sawPreviousPackedCode = false;

  auto emitCode = [&](int code) {
    packer.writeCode(code, packedCodeSize);

    if (code == clearCode) {
      packedCodeSize = minimumCodeSize + 1;
      packedNextDictionaryCode = endOfInformationCode + 1;
      sawPreviousPackedCode = false;
      return;
    }

    if (code == endOfInformationCode) {
      return;
    }

    if (sawPreviousPackedCode && packedNextDictionaryCode < 4096) {
      ++packedNextDictionaryCode;
      if (packedNextDictionaryCode == (1 << packedCodeSize) && packedCodeSize < 12) {
        ++packedCodeSize;
      }
    }

    sawPreviousPackedCode = true;
  };

  resetDictionary();
  emitCode(clearCode);

  int sequenceCode = static_cast<int>(indices.front());
  int nextDictionaryCode = endOfInformationCode + 1;

  for (std::size_t index = 1; index < indices.size(); ++index) {
    const int symbol = static_cast<int>(indices[index]);
    const std::uint32_t dictionaryKey =
        (static_cast<std::uint32_t>(sequenceCode) << 8U) |
        static_cast<std::uint32_t>(symbol);

    const auto found = dictionary.find(dictionaryKey);
    if (found != dictionary.end()) {
      sequenceCode = found->second;
      continue;
    }

    emitCode(sequenceCode);

    if (nextDictionaryCode < 4096) {
      dictionary.emplace(dictionaryKey, nextDictionaryCode);
      ++nextDictionaryCode;
    } else {
      emitCode(clearCode);
      resetDictionary();
      nextDictionaryCode = endOfInformationCode + 1;
    }

    sequenceCode = symbol;
  }

  emitCode(sequenceCode);
  emitCode(endOfInformationCode);

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
    bytes.insert(
        bytes.end(),
        compressedBytes.begin() + static_cast<std::ptrdiff_t>(offset),
        compressedBytes.begin() + static_cast<std::ptrdiff_t>(offset + blockSize));
    offset += blockSize;
  }
  bytes.push_back(0x00);
}

std::uint64_t mixSampleIndex(std::uint64_t value) {
  value += 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31U);
}

void addPaletteSample(GifPaletteSampleSet& samples, const ColorSample& sample) {
  ++samples.candidateCount;

  const std::size_t currentSampleCount = samples.rgbSamples.size() / 3U;
  if (currentSampleCount < kMaxPaletteSamples) {
    samples.rgbSamples.push_back(sample.red);
    samples.rgbSamples.push_back(sample.green);
    samples.rgbSamples.push_back(sample.blue);
    return;
  }

  const std::uint64_t replacementIndex =
      mixSampleIndex(samples.candidateCount) % samples.candidateCount;
  if (replacementIndex >= kMaxPaletteSamples) {
    return;
  }

  const std::size_t sampleOffset = static_cast<std::size_t>(replacementIndex) * 3U;
  samples.rgbSamples[sampleOffset + 0] = sample.red;
  samples.rgbSamples[sampleOffset + 1] = sample.green;
  samples.rgbSamples[sampleOffset + 2] = sample.blue;
}

void composePaletteSample(
    const std::vector<std::uint8_t>& rgbaPixels,
    std::size_t pixelOffset,
    const GifEncoderOptions& options,
    ColorSample& sample,
    bool& includeSample) {
  const std::uint8_t sourceAlphaByte = rgbaPixels[pixelOffset + 3];
  const bool belowTransparencyThreshold = sourceAlphaByte <= options.transparentAlphaThreshold;
  if (options.useTransparency && belowTransparencyThreshold) {
    includeSample = false;
    return;
  }

  sample.red = rgbaPixels[pixelOffset + 0];
  sample.green = rgbaPixels[pixelOffset + 1];
  sample.blue = rgbaPixels[pixelOffset + 2];
  includeSample = true;
}

void computeBoxBounds(std::vector<ColorSample>& samples, ColorBox& box) {
  if (box.begin >= box.end) {
    return;
  }

  box.minRed = box.maxRed = samples[box.begin].red;
  box.minGreen = box.maxGreen = samples[box.begin].green;
  box.minBlue = box.maxBlue = samples[box.begin].blue;

  for (std::size_t index = box.begin + 1; index < box.end; ++index) {
    const ColorSample& sample = samples[index];
    box.minRed = std::min(box.minRed, sample.red);
    box.maxRed = std::max(box.maxRed, sample.red);
    box.minGreen = std::min(box.minGreen, sample.green);
    box.maxGreen = std::max(box.maxGreen, sample.green);
    box.minBlue = std::min(box.minBlue, sample.blue);
    box.maxBlue = std::max(box.maxBlue, sample.blue);
  }
}

int boxLongestAxis(const ColorBox& box) {
  const int redRange = static_cast<int>(box.maxRed) - static_cast<int>(box.minRed);
  const int greenRange = static_cast<int>(box.maxGreen) - static_cast<int>(box.minGreen);
  const int blueRange = static_cast<int>(box.maxBlue) - static_cast<int>(box.minBlue);

  if (redRange >= greenRange && redRange >= blueRange) {
    return 0;
  }
  if (greenRange >= blueRange) {
    return 1;
  }
  return 2;
}

double boxScore(const ColorBox& box) {
  const double redRange = static_cast<double>(box.maxRed) - static_cast<double>(box.minRed);
  const double greenRange = static_cast<double>(box.maxGreen) - static_cast<double>(box.minGreen);
  const double blueRange = static_cast<double>(box.maxBlue) - static_cast<double>(box.minBlue);
  const double volume = (redRange + 1.0) * (greenRange + 1.0) * (blueRange + 1.0);
  const double count = static_cast<double>(box.end - box.begin);
  return volume * count;
}

std::vector<PaletteColor> buildAdaptivePaletteColors(
    std::vector<ColorSample> samples,
    int targetColorCount) {
  if (samples.empty()) {
    return {{0, 0, 0}};
  }

  std::vector<ColorBox> boxes;
  boxes.push_back({0, samples.size()});
  computeBoxBounds(samples, boxes.front());

  while (static_cast<int>(boxes.size()) < targetColorCount) {
    auto splitIt = std::max_element(
        boxes.begin(),
        boxes.end(),
        [](const ColorBox& left, const ColorBox& right) {
          const bool leftSplittable = (left.end - left.begin) > 1;
          const bool rightSplittable = (right.end - right.begin) > 1;
          if (leftSplittable != rightSplittable) {
            return !leftSplittable && rightSplittable;
          }
          return boxScore(left) < boxScore(right);
        });

    if (splitIt == boxes.end() || (splitIt->end - splitIt->begin) <= 1) {
      break;
    }

    const int axis = boxLongestAxis(*splitIt);
    const std::size_t mid = splitIt->begin + (splitIt->end - splitIt->begin) / 2;
    auto rangeBegin = samples.begin() + static_cast<std::ptrdiff_t>(splitIt->begin);
    auto rangeMid = samples.begin() + static_cast<std::ptrdiff_t>(mid);
    auto rangeEnd = samples.begin() + static_cast<std::ptrdiff_t>(splitIt->end);

    auto comparator = [axis](const ColorSample& left, const ColorSample& right) {
      if (axis == 0) {
        return left.red < right.red;
      }
      if (axis == 1) {
        return left.green < right.green;
      }
      return left.blue < right.blue;
    };
    std::nth_element(rangeBegin, rangeMid, rangeEnd, comparator);

    ColorBox newBox{mid, splitIt->end};
    splitIt->end = mid;
    computeBoxBounds(samples, *splitIt);
    computeBoxBounds(samples, newBox);
    boxes.push_back(newBox);
  }

  std::vector<PaletteColor> paletteColors;
  paletteColors.reserve(boxes.size());

  for (ColorBox& box : boxes) {
    std::uint64_t redSum = 0;
    std::uint64_t greenSum = 0;
    std::uint64_t blueSum = 0;
    const std::size_t count = std::max<std::size_t>(1, box.end - box.begin);

    for (std::size_t index = box.begin; index < box.end; ++index) {
      redSum += samples[index].red;
      greenSum += samples[index].green;
      blueSum += samples[index].blue;
    }

    paletteColors.push_back({
        static_cast<std::uint8_t>(redSum / count),
        static_cast<std::uint8_t>(greenSum / count),
        static_cast<std::uint8_t>(blueSum / count)});
  }

  std::sort(
      paletteColors.begin(),
      paletteColors.end(),
      [](const PaletteColor& left, const PaletteColor& right) {
        const int leftLuma = 30 * left.red + 59 * left.green + 11 * left.blue;
        const int rightLuma = 30 * right.red + 59 * right.green + 11 * right.blue;
        return leftLuma < rightLuma;
      });

  return paletteColors;
}

std::size_t nearestColorLookupIndex(int red, int green, int blue) {
  const int redCell = std::clamp(red, 0, 255) >> kNearestColorShift;
  const int greenCell = std::clamp(green, 0, 255) >> kNearestColorShift;
  const int blueCell = std::clamp(blue, 0, 255) >> kNearestColorShift;
  return
      (static_cast<std::size_t>(redCell) << (kNearestColorBits * 2)) |
      (static_cast<std::size_t>(greenCell) << kNearestColorBits) |
      static_cast<std::size_t>(blueCell);
}

void buildNearestColorLookup(GifPalette& palette) {
  palette.nearestColorLookup.resize(kNearestColorLookupSize);

  for (int redCell = 0; redCell < kNearestColorLevels; ++redCell) {
    const int red = std::min(255, (redCell << kNearestColorShift) + (1 << (kNearestColorShift - 1)));
    for (int greenCell = 0; greenCell < kNearestColorLevels; ++greenCell) {
      const int green = std::min(255, (greenCell << kNearestColorShift) + (1 << (kNearestColorShift - 1)));
      for (int blueCell = 0; blueCell < kNearestColorLevels; ++blueCell) {
        const int blue = std::min(255, (blueCell << kNearestColorShift) + (1 << (kNearestColorShift - 1)));
        int bestIndex = 0;
        std::uint32_t bestDistance = std::numeric_limits<std::uint32_t>::max();

        for (int paletteIndex = 0; paletteIndex < palette.activeColorCount; ++paletteIndex) {
          const int paletteOffset = paletteIndex * 3;
          const int redDistance = red - static_cast<int>(palette.rgbTable[paletteOffset + 0]);
          const int greenDistance = green - static_cast<int>(palette.rgbTable[paletteOffset + 1]);
          const int blueDistance = blue - static_cast<int>(palette.rgbTable[paletteOffset + 2]);
          const std::uint32_t distance = static_cast<std::uint32_t>(
              redDistance * redDistance +
              greenDistance * greenDistance +
              blueDistance * blueDistance);

          if (distance < bestDistance) {
            bestDistance = distance;
            bestIndex = paletteIndex;
          }
        }

        const std::size_t lookupIndex =
            (static_cast<std::size_t>(redCell) << (kNearestColorBits * 2)) |
            (static_cast<std::size_t>(greenCell) << kNearestColorBits) |
            static_cast<std::size_t>(blueCell);
        palette.nearestColorLookup[lookupIndex] = static_cast<std::uint8_t>(bestIndex);
      }
    }
  }
}

int nearestPaletteIndex(float red, float green, float blue, const GifPalette& palette) {
  const int roundedRed = std::clamp(static_cast<int>(std::lround(red)), 0, 255);
  const int roundedGreen = std::clamp(static_cast<int>(std::lround(green)), 0, 255);
  const int roundedBlue = std::clamp(static_cast<int>(std::lround(blue)), 0, 255);

  if (palette.nearestColorLookup.size() == kNearestColorLookupSize) {
    return static_cast<int>(palette.nearestColorLookup[
        nearestColorLookupIndex(roundedRed, roundedGreen, roundedBlue)]);
  }

  int bestIndex = 0;
  std::uint32_t bestDistance = std::numeric_limits<std::uint32_t>::max();

  for (int index = 0; index < palette.activeColorCount; ++index) {
    const int paletteOffset = index * 3;
    const int redDistance = roundedRed - static_cast<int>(palette.rgbTable[paletteOffset + 0]);
    const int greenDistance = roundedGreen - static_cast<int>(palette.rgbTable[paletteOffset + 1]);
    const int blueDistance = roundedBlue - static_cast<int>(palette.rgbTable[paletteOffset + 2]);
    const std::uint32_t distance = static_cast<std::uint32_t>(
        redDistance * redDistance +
        greenDistance * greenDistance +
        blueDistance * blueDistance);

    if (distance < bestDistance) {
      bestDistance = distance;
      bestIndex = index;
      if (distance == 0) {
        break;
      }
    }
  }

  return bestIndex;
}

PaletteColor paletteColorAt(const GifPalette& palette, int index) {
  const int clampedIndex = std::clamp(index, 0, palette.activeColorCount - 1);
  const int paletteOffset = clampedIndex * 3;
  return {
      palette.rgbTable[paletteOffset + 0],
      palette.rgbTable[paletteOffset + 1],
      palette.rgbTable[paletteOffset + 2]};
}

void convertPixelsToIndexed(
    int width,
    int height,
    const std::vector<std::uint8_t>& rgbaPixels,
    const GifEncoderOptions& options,
    const GifPalette& palette,
    std::vector<std::uint8_t>& indexedPixels,
    bool& hasTransparentPixels) {
  indexedPixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
  hasTransparentPixels = false;

  auto composePixel = [&](std::size_t pixelOffset, float& red, float& green, float& blue, bool& transparent) {
    const std::uint8_t sourceAlphaByte = rgbaPixels[pixelOffset + 3];
    const bool belowTransparencyThreshold = sourceAlphaByte <= options.transparentAlphaThreshold;

    transparent = options.useTransparency && belowTransparencyThreshold;
    if (transparent) {
      red = green = blue = 0.0F;
      return;
    }

    red = static_cast<float>(rgbaPixels[pixelOffset + 0]);
    green = static_cast<float>(rgbaPixels[pixelOffset + 1]);
    blue = static_cast<float>(rgbaPixels[pixelOffset + 2]);
  };

  if (options.ditherMode == GifDitherMode::kNone) {
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const std::size_t pixelIndex =
            static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
        const std::size_t pixelOffset = pixelIndex * 4U;

        float red = 0.0F;
        float green = 0.0F;
        float blue = 0.0F;
        bool transparent = false;
        composePixel(pixelOffset, red, green, blue, transparent);

        if (transparent) {
          indexedPixels[pixelIndex] = palette.transparentIndex;
          hasTransparentPixels = true;
          continue;
        }

        indexedPixels[pixelIndex] = static_cast<std::uint8_t>(nearestPaletteIndex(red, green, blue, palette));
      }
    }
    return;
  }

  if (options.ditherMode == GifDitherMode::kOrdered) {
    constexpr float kOrderedStrength = 18.0F;

    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const std::size_t pixelIndex =
            static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
        const std::size_t pixelOffset = pixelIndex * 4U;

        float red = 0.0F;
        float green = 0.0F;
        float blue = 0.0F;
        bool transparent = false;
        composePixel(pixelOffset, red, green, blue, transparent);

        if (transparent) {
          indexedPixels[pixelIndex] = palette.transparentIndex;
          hasTransparentPixels = true;
          continue;
        }

        const int matrixIndex = (y % kOrderedDitherSize) * kOrderedDitherSize + (x % kOrderedDitherSize);
        const float threshold = (static_cast<float>(kBayer4x4[matrixIndex]) + 0.5F) / 16.0F - 0.5F;
        const float offset = threshold * kOrderedStrength;

        indexedPixels[pixelIndex] = static_cast<std::uint8_t>(nearestPaletteIndex(
            std::clamp(red + offset, 0.0F, 255.0F),
            std::clamp(green + offset, 0.0F, 255.0F),
            std::clamp(blue + offset, 0.0F, 255.0F),
            palette));
      }
    }
    return;
  }

  std::vector<float> currentErrorRow(static_cast<std::size_t>(width + 2) * 3U, 0.0F);
  std::vector<float> nextErrorRow(static_cast<std::size_t>(width + 2) * 3U, 0.0F);

  for (int y = 0; y < height; ++y) {
    std::fill(nextErrorRow.begin(), nextErrorRow.end(), 0.0F);

    for (int x = 0; x < width; ++x) {
      const std::size_t pixelIndex =
          static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
      const std::size_t pixelOffset = pixelIndex * 4U;

      float red = 0.0F;
      float green = 0.0F;
      float blue = 0.0F;
      bool transparent = false;
      composePixel(pixelOffset, red, green, blue, transparent);

      if (transparent) {
        indexedPixels[pixelIndex] = palette.transparentIndex;
        hasTransparentPixels = true;
        continue;
      }

      const std::size_t errorIndex = static_cast<std::size_t>(x + 1) * 3U;
      const float correctedRed = std::clamp(red + currentErrorRow[errorIndex + 0], 0.0F, 255.0F);
      const float correctedGreen = std::clamp(green + currentErrorRow[errorIndex + 1], 0.0F, 255.0F);
      const float correctedBlue = std::clamp(blue + currentErrorRow[errorIndex + 2], 0.0F, 255.0F);

      const int paletteIndex = nearestPaletteIndex(correctedRed, correctedGreen, correctedBlue, palette);
      indexedPixels[pixelIndex] = static_cast<std::uint8_t>(paletteIndex);

      const PaletteColor quantized = paletteColorAt(palette, paletteIndex);
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
      const std::size_t pixelIndex =
          static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
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
    const std::size_t srcOffset =
        static_cast<std::size_t>(y) * static_cast<std::size_t>(fullWidth) + static_cast<std::size_t>(rect.left);
    const std::size_t dstOffset =
        static_cast<std::size_t>(y - rect.top) * static_cast<std::size_t>(rect.width());
    std::copy_n(
        fullFrame.begin() + static_cast<std::ptrdiff_t>(srcOffset),
        rect.width(),
        subrectPixels.begin() + static_cast<std::ptrdiff_t>(dstOffset));
  }
}

} // namespace

int GifFrameDelayCentiseconds(
    double framesPerSecond,
    std::size_t frameIndex,
    std::int64_t& emittedCentiseconds) {
  const double safeFramesPerSecond =
      std::isfinite(framesPerSecond) && framesPerSecond > 0.0
      ? framesPerSecond
      : 24.0;
  const long double idealCumulativeDelay =
      (static_cast<long double>(frameIndex) + 1.0L) * 100.0L /
      static_cast<long double>(safeFramesPerSecond);
  const std::int64_t targetCumulativeDelay =
      static_cast<std::int64_t>(std::llround(idealCumulativeDelay));
  const std::int64_t delay = std::clamp<std::int64_t>(
      targetCumulativeDelay - emittedCentiseconds,
      1,
      65535);
  emittedCentiseconds += delay;
  return static_cast<int>(delay);
}

bool AddGifPaletteFrameSamples(
    int width,
    int height,
    const std::vector<std::uint8_t>& rgbaPixels,
    const GifEncoderOptions& options,
    GifPaletteSampleSet& samples,
    std::string& error) {
  error.clear();

  if (width <= 0 || height <= 0) {
    error = "GIF palette sampling requires a positive image size.";
    return false;
  }

  const std::size_t expectedPixelBytes =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
  if (rgbaPixels.size() != expectedPixelBytes) {
    error = "GIF palette sampling received an unexpected RGBA buffer size.";
    return false;
  }

  const std::size_t pixelCount =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  const std::size_t pixelsPerSample =
      std::max<std::size_t>(1, pixelCount / kPaletteSamplesPerFrame);
  const int stride = std::max(
      1,
      static_cast<int>(std::sqrt(static_cast<double>(pixelsPerSample))));

  const std::uint64_t frameSeed = mixSampleIndex(samples.frameCount + 1U);
  const int xOffsetRange = std::max(1, std::min(stride, width));
  const int yOffsetRange = std::max(1, std::min(stride, height));
  const int xOffset = static_cast<int>(frameSeed % static_cast<std::uint64_t>(xOffsetRange));
  const int yOffset = static_cast<int>(
      mixSampleIndex(frameSeed) % static_cast<std::uint64_t>(yOffsetRange));

  for (int y = yOffset; y < height; y += stride) {
    for (int x = xOffset; x < width; x += stride) {
      const std::size_t pixelIndex =
          static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
          static_cast<std::size_t>(x);
      const std::size_t pixelOffset = pixelIndex * 4U;

      ColorSample sample{};
      bool includeSample = false;
      composePaletteSample(rgbaPixels, pixelOffset, options, sample, includeSample);
      if (includeSample) {
        addPaletteSample(samples, sample);
      }
    }
  }

  ++samples.frameCount;
  return true;
}

bool BuildAdaptiveGifPaletteFromSamples(
    const GifPaletteSampleSet& samples,
    const GifEncoderOptions& options,
    GifPalette& palette,
    std::string& error) {
  error.clear();

  if (samples.frameCount == 0) {
    error = "GIF export requires at least one sampled frame.";
    return false;
  }
  if ((samples.rgbSamples.size() % 3U) != 0U) {
    error = "GIF encoder received malformed palette samples.";
    return false;
  }

  std::vector<ColorSample> colorSamples;
  colorSamples.reserve(samples.rgbSamples.size() / 3U);
  for (std::size_t offset = 0; offset < samples.rgbSamples.size(); offset += 3U) {
    colorSamples.push_back({
        samples.rgbSamples[offset + 0],
        samples.rgbSamples[offset + 1],
        samples.rgbSamples[offset + 2]});
  }

  palette.tableSize = sanitizeMaxColors(options.maxColors);
  palette.transparentIndex = static_cast<std::uint8_t>(palette.tableSize - 1);
  palette.minimumCodeSize = paletteMinimumCodeSize(palette.tableSize);
  palette.packedField = static_cast<std::uint8_t>(0x80 | 0x70 | paletteSizeCode(palette.tableSize));

  const int targetColorCount = options.useTransparency ? (palette.tableSize - 1) : palette.tableSize;
  std::vector<PaletteColor> paletteColors =
      buildAdaptivePaletteColors(std::move(colorSamples), targetColorCount);
  if (paletteColors.empty()) {
    paletteColors.push_back({0, 0, 0});
  }

  palette.activeColorCount = std::min<int>(static_cast<int>(paletteColors.size()), targetColorCount);
  palette.rgbTable.assign(static_cast<std::size_t>(palette.tableSize) * 3U, 0);

  for (int index = 0; index < palette.activeColorCount; ++index) {
    const int paletteOffset = index * 3;
    palette.rgbTable[paletteOffset + 0] = paletteColors[static_cast<std::size_t>(index)].red;
    palette.rgbTable[paletteOffset + 1] = paletteColors[static_cast<std::size_t>(index)].green;
    palette.rgbTable[paletteOffset + 2] = paletteColors[static_cast<std::size_t>(index)].blue;
  }

  const PaletteColor filler = paletteColors[static_cast<std::size_t>(palette.activeColorCount - 1)];
  for (int index = palette.activeColorCount; index < palette.tableSize; ++index) {
    const int paletteOffset = index * 3;
    palette.rgbTable[paletteOffset + 0] = filler.red;
    palette.rgbTable[paletteOffset + 1] = filler.green;
    palette.rgbTable[paletteOffset + 2] = filler.blue;
  }

  if (options.useTransparency) {
    const int transparentOffset = static_cast<int>(palette.transparentIndex) * 3;
    palette.rgbTable[transparentOffset + 0] = 0;
    palette.rgbTable[transparentOffset + 1] = 0;
    palette.rgbTable[transparentOffset + 2] = 0;
  }

  buildNearestColorLookup(palette);
  return true;
}

bool BuildAdaptiveGifPalette(
    int width,
    int height,
    const std::vector<std::vector<std::uint8_t>>& rgbaFrames,
    const GifEncoderOptions& options,
    GifPalette& palette,
    std::string& error) {
  error.clear();

  if (width <= 0 || height <= 0) {
    error = "GIF export requires a positive image size.";
    return false;
  }
  if (rgbaFrames.empty()) {
    error = "GIF export requires at least one frame.";
    return false;
  }

  GifPaletteSampleSet samples;
  for (const std::vector<std::uint8_t>& frame : rgbaFrames) {
    if (!AddGifPaletteFrameSamples(width, height, frame, options, samples, error)) {
      return false;
    }
  }

  return BuildAdaptiveGifPaletteFromSamples(samples, options, palette, error);
}

bool QuantizeGifFrameToPalette(
    int width,
    int height,
    const std::vector<std::uint8_t>& rgbaPixels,
    const GifEncoderOptions& options,
    const GifPalette& palette,
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
  if (palette.rgbTable.empty() || palette.activeColorCount <= 0) {
    error = "GIF encoder requires a valid palette before quantization.";
    return false;
  }

  convertPixelsToIndexed(width, height, rgbaPixels, options, palette, indexedFrame.pixels, indexedFrame.hasTransparentPixels);
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

  GifPalette palette;
  std::vector<std::vector<std::uint8_t>> frames{rgbaPixels};
  if (!BuildAdaptiveGifPalette(width, height, frames, options, palette, error)) {
    return false;
  }

  GifIndexedFrame indexedFrame;
  if (!QuantizeGifFrameToPalette(width, height, rgbaPixels, options, palette, indexedFrame, error)) {
    return false;
  }

  if (!EncodeGifAnimationHeader(width, height, options, palette, gifBytes, error)) {
    return false;
  }

  std::vector<std::uint8_t> frameBytes;
  if (!EncodeGifAnimationFrame(width, height, indexedFrame, nullptr, options, palette, false, frameBytes, error)) {
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
    const GifPalette& palette,
    std::vector<std::uint8_t>& gifBytes,
    std::string& error) {
  gifBytes.clear();
  error.clear();

  if (width <= 0 || height <= 0) {
    error = "GIF export requires a positive image size.";
    return false;
  }
  if (palette.rgbTable.size() != static_cast<std::size_t>(palette.tableSize) * 3U) {
    error = "GIF encoder received an invalid global palette.";
    return false;
  }

  gifBytes.reserve(13 + palette.rgbTable.size() + 32);
  gifBytes.insert(gifBytes.end(), {'G', 'I', 'F', '8', '9', 'a'});

  appendU16LE(gifBytes, width);
  appendU16LE(gifBytes, height);
  gifBytes.push_back(palette.packedField);
  gifBytes.push_back(options.useTransparency ? palette.transparentIndex : 0x00);
  gifBytes.push_back(0x00);

  gifBytes.insert(gifBytes.end(), palette.rgbTable.begin(), palette.rgbTable.end());
  appendApplicationLoopExtension(gifBytes, options);

  return true;
}

bool EncodeGifAnimationFrame(
    int width,
    int height,
    const GifIndexedFrame& frame,
    const GifIndexedFrame* previousFrame,
    const GifEncoderOptions& options,
    const GifPalette& palette,
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

  const std::vector<std::uint8_t> compressedBytes = encodeLzwIndices(encodedPixels, palette.minimumCodeSize);
  gifBytes.reserve(compressedBytes.size() + 32);

  appendGraphicControlExtension(
      gifBytes,
      options.useTransparency && frame.hasTransparentPixels && !safeToDifference,
      palette.transparentIndex,
      options.frameDelayCentiseconds,
      disposalMethod);

  gifBytes.push_back(0x2c);
  appendU16LE(gifBytes, rect.left);
  appendU16LE(gifBytes, rect.top);
  appendU16LE(gifBytes, rect.width());
  appendU16LE(gifBytes, rect.height());
  gifBytes.push_back(0x00);

  gifBytes.push_back(static_cast<std::uint8_t>(palette.minimumCodeSize));
  appendImageDataBlocks(gifBytes, compressedBytes);

  return true;
}

void EncodeGifAnimationTrailer(std::vector<std::uint8_t>& gifBytes) {
  gifBytes.push_back(0x3b);
}

} // namespace GifExporter
