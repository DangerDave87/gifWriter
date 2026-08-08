#include "GifWriter.h"
#include "GifEncoder.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "DDImage/Channel.h"
#include "DDImage/Knobs.h"
#include "DDImage/LUT.h"
#include "DDImage/Row.h"
#include "DDImage/Write.h"

#include <cmath>

namespace GifExporter {

namespace {

const char* const kLoopModeLabels[] = {
    "Infinite",
    "Fixed",
    "No Loop",
    nullptr};

const char* const kDitherModeLabels[] = {
    "None",
    "Floyd-Steinberg",
    "Ordered",
    nullptr};

const char* const kMaxColorsLabels[] = {
    "256",
    "128",
    "64",
    "32",
    nullptr};

int selectedMaxColors(int mode) {
  switch (mode) {
  case 1:
    return 128;
  case 2:
    return 64;
  case 3:
    return 32;
  default:
    return 256;
  }
}

GifEncoderOptions makeEncoderOptions(
    bool alphaAvailable,
    int loopMode,
    int loopCount,
    int ditherMode,
    int maxColorsMode,
    float transparencyThreshold,
    double fps) {
  GifEncoderOptions options;
  options.useTransparency = alphaAvailable;
  options.ditherMode = static_cast<GifDitherMode>(ditherMode);
  options.transparentAlphaThreshold =
      static_cast<std::uint8_t>(std::clamp(transparencyThreshold, 0.0F, 1.0F) * 255.0F + 0.5F);
  options.loopMode = loopMode == GifWriter::kLoopFixed
      ? GifLoopMode::kFixed
      : (loopMode == GifWriter::kLoopNone ? GifLoopMode::kNone : GifLoopMode::kInfinite);
  options.loopCount = loopCount;
  options.frameDelayCentiseconds = std::max(1, static_cast<int>(std::lround(100.0 / fps)));
  options.framesPerSecond = fps;
  options.maxColors = selectedMaxColors(maxColorsMode);
  return options;
}

} // namespace

using namespace DD::Image;

const Writer::Description GifWriter::kDescription(
    "gif\0",
    "GIF image",
    GifWriter::Build);

Writer* GifWriter::Build(Write* writeNode) {
  return new GifWriter(writeNode);
}

GifWriter::GifWriter(Write* writeNode)
    : FileWriter(writeNode),
      loopMode_(kLoopInfinite),
      loopCount_(1),
      ditherMode_(kDitherFloydSteinberg),
      maxColorsMode_(0),
      transparencyThreshold_(20.0F / 255.0F),
      fps_(24.0),
      fileOpen_(false),
      executionFailed_(false),
      spoolFile_(nullptr),
      frameWidth_(0),
      frameHeight_(0),
      frameByteSize_(0),
      spooledFrameCount_(0) {
  if (root_real_fps) {
    fps_ = root_real_fps();
  }
}

GifWriter::~GifWriter() {
  closeSpoolFile();
}

void GifWriter::execute() {
  if (executionFailed_) {
    return;
  }

  std::string error;
  if (!spoolFile_ && !beginExecution(error)) {
    if (!error.empty()) {
      reportExecutionFailure(error);
    }
    return;
  }

  if (width() != frameWidth_ || height() != frameHeight_) {
    reportExecutionFailure("GIF export does not support frame dimensions changing during a render.");
    return;
  }

  std::vector<std::uint8_t> rgbaPixels;
  if (!readCurrentFrameRGBA(rgbaPixels, error)) {
    reportExecutionFailure(error);
    return;
  }

  if (!AddGifPaletteFrameSamples(
          frameWidth_,
          frameHeight_,
          rgbaPixels,
          encoderOptions_,
          paletteSamples_,
          error)) {
    reportExecutionFailure(error);
    return;
  }

  if (!appendFrameToSpool(rgbaPixels, error)) {
    reportExecutionFailure(error);
  }
}

bool GifWriter::movie() const {
  return true;
}

void GifWriter::finish() {
  if (executionFailed_) {
    resetExecutionState();
    return;
  }

  if (spooledFrameCount_ == 0 || !spoolFile_ || !fileOpen_) {
    if (fileOpen_ || spoolFile_) {
      reportExecutionFailure("GIF export finished without any frames to encode.");
    }
    resetExecutionState();
    return;
  }

  if (aborted()) {
    reportExecutionFailure("GIF export was aborted.");
    resetExecutionState();
    return;
  }

  std::string error;
  GifPalette palette;
  if (!BuildAdaptiveGifPaletteFromSamples(
          paletteSamples_,
          encoderOptions_,
          palette,
          error)) {
    reportExecutionFailure(error);
    resetExecutionState();
    return;
  }

  if (std::fflush(spoolFile_) != 0 || std::fseek(spoolFile_, 0, SEEK_SET) != 0) {
    reportExecutionFailure("Failed to rewind the temporary GIF frame cache.");
    resetExecutionState();
    return;
  }

  std::vector<std::uint8_t> bytes;
  if (!EncodeGifAnimationHeader(
          frameWidth_,
          frameHeight_,
          encoderOptions_,
          palette,
          bytes,
          error)) {
    reportExecutionFailure(error);
    resetExecutionState();
    return;
  }

  if (!write(bytes.data(), static_cast<FILE_OFFSET>(bytes.size()))) {
    reportExecutionFailure("Failed to write GIF animation header.");
    resetExecutionState();
    return;
  }

  GifIndexedFrame previousFrame;
  bool hasPreviousFrame = false;
  std::int64_t emittedCentiseconds = 0;
  std::vector<std::uint8_t> rgbaPixels;

  for (std::size_t frameIndex = 0; frameIndex < spooledFrameCount_; ++frameIndex) {
    if (aborted()) {
      reportExecutionFailure("GIF export was aborted.");
      resetExecutionState();
      return;
    }

    progressFraction(
        static_cast<double>(frameIndex) /
        static_cast<double>(std::max<std::size_t>(1, spooledFrameCount_)));

    if (!readFrameFromSpool(rgbaPixels, error)) {
      reportExecutionFailure(error);
      resetExecutionState();
      return;
    }

    GifIndexedFrame indexedFrame;
    if (!QuantizeGifFrameToPalette(
            frameWidth_,
            frameHeight_,
            rgbaPixels,
            encoderOptions_,
            palette,
            indexedFrame,
            error)) {
      reportExecutionFailure(error);
      resetExecutionState();
      return;
    }

    GifEncoderOptions frameOptions = encoderOptions_;
    frameOptions.frameDelayCentiseconds = GifFrameDelayCentiseconds(
        encoderOptions_.framesPerSecond,
        frameIndex,
        emittedCentiseconds);

    bytes.clear();
    const GifIndexedFrame* previousFramePtr = hasPreviousFrame ? &previousFrame : nullptr;
    if (!EncodeGifAnimationFrame(
            frameWidth_,
            frameHeight_,
            indexedFrame,
            previousFramePtr,
            frameOptions,
            palette,
            true,
            bytes,
            error)) {
      reportExecutionFailure(error);
      resetExecutionState();
      return;
    }

    if (!write(bytes.data(), static_cast<FILE_OFFSET>(bytes.size()))) {
      reportExecutionFailure("Failed to write GIF frame data.");
      resetExecutionState();
      return;
    }

    previousFrame = std::move(indexedFrame);
    hasPreviousFrame = true;
  }

  progressFraction(1.0);

  bytes.clear();
  EncodeGifAnimationTrailer(bytes);
  if (!write(bytes.data(), static_cast<FILE_OFFSET>(bytes.size()))) {
    reportExecutionFailure("Failed to finalize the GIF animation.");
    resetExecutionState();
    return;
  }

  closeSpoolFile();
  if (!close()) {
    fileOpen_ = false;
    resetExecutionState();
    return;
  }
  fileOpen_ = false;
  resetExecutionState();
}

void GifWriter::knobs(Knob_Callback callback) {
  Enumeration_knob(callback, &loopMode_, kLoopModeLabels, "loop_mode", "loop mode");
  Tooltip(callback, "Controls how the exported GIF repeats during playback.");

  Int_knob(callback, &loopCount_, IRange(1, 9999, true), "loop_count", "");
  ClearFlags(callback, Knob::STARTLINE);
  Tooltip(callback, "Number of times to repeat the animation when loop mode is set to Fixed.");

  Named_Text_knob(callback, "loop_count_suffix", "loops");
  ClearFlags(callback, Knob::STARTLINE);
  SetFlags(callback, Knob::ENDLINE);

  Float_knob(callback, &transparencyThreshold_, IRange(0.0, 1.0, true), "transparency_threshold", "alpha min");
  Tooltip(callback, "Alpha values at or below this threshold are treated as fully transparent when the Write channels include alpha.");

  Enumeration_knob(callback, &ditherMode_, kDitherModeLabels, "dither", "dither");
  Tooltip(callback, "Controls how palette quantization dithering is applied.");

  Enumeration_knob(callback, &maxColorsMode_, kMaxColorsLabels, "max_colors", "max colors");
  Tooltip(callback, "Limits the effective palette size. Fewer colors usually reduce file size.");

  Double_knob(callback, &fps_, IRange(0.01, 240.0, true), "fps", "fps");
  ClearFlags(callback, Knob::SLIDER);
  Tooltip(callback, "Defaults to the current Nuke project frame rate.");

  updateKnobVisibility();
}

int GifWriter::knob_changed(Knob* knob) {
  Writer::knob_changed(knob);

  if (loopCount_ < 1) {
    loopCount_ = 1;
  }

  if (ditherMode_ < kDitherNone || ditherMode_ > kDitherOrdered) {
    ditherMode_ = kDitherFloydSteinberg;
  }

  if (maxColorsMode_ < 0 || maxColorsMode_ > 3) {
    maxColorsMode_ = 0;
  }

  if (fps_ <= 0.0) {
    fps_ = root_real_fps ? root_real_fps() : 24.0;
  }

  transparencyThreshold_ = std::clamp(transparencyThreshold_, 0.0F, 1.0F);

  updateKnobVisibility();

  return 1;
}

LUT* GifWriter::defaultLUT() const {
  return LUT::GetLut(LUT::INT8, this);
}

std::vector<Channel> GifWriter::selectedWriteChannels() const {
  std::vector<Channel> channelsToWrite;
  const int selectedChannelCount = depth();
  channelsToWrite.reserve(static_cast<std::size_t>(selectedChannelCount));

  for (int index = 0; index < selectedChannelCount; ++index) {
    const Channel selectedChannel = channel(index);
    if (selectedChannel != Chan_Black) {
      channelsToWrite.push_back(selectedChannel);
    }
  }

  return channelsToWrite;
}

bool GifWriter::hasOutputAlphaChannel() const {
  const std::size_t selectedChannelCount = selectedWriteChannels().size();
  return selectedChannelCount == 1 || selectedChannelCount == 2 || selectedChannelCount >= 4;
}

bool GifWriter::readCurrentFrameRGBA(std::vector<std::uint8_t>& rgbaPixels, std::string& error) {
  error.clear();

  const int imageWidth = width();
  const int imageHeight = height();
  if (imageWidth <= 0 || imageHeight <= 0) {
    error = "GIF export requires a non-empty image.";
    return false;
  }

  const std::vector<Channel> writeChannels = selectedWriteChannels();
  if (writeChannels.empty()) {
    error = "GIF export requires at least one selected output channel.";
    return false;
  }

  ChannelSet channels;
  const std::size_t selectedChannelCount = writeChannels.size();
  const std::size_t requestedChannelCount = std::min<std::size_t>(selectedChannelCount, 4);
  for (std::size_t index = 0; index < requestedChannelCount; ++index) {
    channels += writeChannels[index];
  }

  rgbaPixels.resize(static_cast<std::size_t>(imageWidth) * static_cast<std::size_t>(imageHeight) * 4U);

  input0().request(0, 0, imageWidth, imageHeight, channels, 1);

  Row row(0, imageWidth);
  std::vector<std::uint8_t> redRow(static_cast<std::size_t>(imageWidth));
  std::vector<std::uint8_t> greenRow(static_cast<std::size_t>(imageWidth));
  std::vector<std::uint8_t> blueRow(static_cast<std::size_t>(imageWidth));
  std::vector<std::uint8_t> alphaRow(static_cast<std::size_t>(imageWidth), 255);

  for (int outputY = 0; outputY < imageHeight; ++outputY) {
    if (aborted()) {
      error = "GIF export was aborted.";
      return false;
    }

    progressFraction(outputY, imageHeight);

    const int nukeY = imageHeight - outputY - 1;
    get(nukeY, 0, imageWidth, channels, row);

    std::fill(redRow.begin(), redRow.end(), static_cast<std::uint8_t>(0));
    std::fill(greenRow.begin(), greenRow.end(), static_cast<std::uint8_t>(0));
    std::fill(blueRow.begin(), blueRow.end(), static_cast<std::uint8_t>(0));
    std::fill(alphaRow.begin(), alphaRow.end(), static_cast<std::uint8_t>(255));

    const Channel alphaChannel =
        (selectedChannelCount == 1) ? writeChannels[0]
        : (selectedChannelCount == 2) ? writeChannels[1]
        : (selectedChannelCount >= 4) ? writeChannels[3]
        : Chan_Black;
    const float* alphaInput = alphaChannel != Chan_Black ? row[alphaChannel] : nullptr;

    if (selectedChannelCount == 1) {
      const float* sourceAlpha = row[writeChannels[0]];
      for (int x = 0; x < imageWidth; ++x) {
        const float alpha = sourceAlpha ? std::clamp(sourceAlpha[x], 0.0F, 1.0F) : 1.0F;
        alphaRow[static_cast<std::size_t>(x)] =
            static_cast<std::uint8_t>(alpha * 255.0F + 0.5F);
      }
    } else if (selectedChannelCount == 2) {
      const Channel grayChannel = writeChannels[0];
      to_byte(0, redRow.data(), row[grayChannel], alphaInput, imageWidth);
      to_byte(1, greenRow.data(), row[grayChannel], alphaInput, imageWidth);
      to_byte(2, blueRow.data(), row[grayChannel], alphaInput, imageWidth);

      const float* sourceAlpha = row[alphaChannel];
      for (int x = 0; x < imageWidth; ++x) {
        const float alpha = sourceAlpha ? std::clamp(sourceAlpha[x], 0.0F, 1.0F) : 1.0F;
        alphaRow[static_cast<std::size_t>(x)] =
            static_cast<std::uint8_t>(alpha * 255.0F + 0.5F);
      }
    } else {
      to_byte(0, redRow.data(), row[writeChannels[0]], alphaInput, imageWidth);
      to_byte(1, greenRow.data(), row[writeChannels[1]], alphaInput, imageWidth);
      to_byte(2, blueRow.data(), row[writeChannels[2]], alphaInput, imageWidth);

      if (alphaChannel != Chan_Black) {
        const float* sourceAlpha = row[alphaChannel];
        for (int x = 0; x < imageWidth; ++x) {
          const float alpha = sourceAlpha ? std::clamp(sourceAlpha[x], 0.0F, 1.0F) : 1.0F;
          alphaRow[static_cast<std::size_t>(x)] =
              static_cast<std::uint8_t>(alpha * 255.0F + 0.5F);
        }
      }
    }

    const std::size_t rowOffset = static_cast<std::size_t>(outputY) * static_cast<std::size_t>(imageWidth) * 4U;
    for (int x = 0; x < imageWidth; ++x) {
      const std::size_t pixelOffset = rowOffset + static_cast<std::size_t>(x) * 4U;
      rgbaPixels[pixelOffset + 0] = redRow[static_cast<std::size_t>(x)];
      rgbaPixels[pixelOffset + 1] = greenRow[static_cast<std::size_t>(x)];
      rgbaPixels[pixelOffset + 2] = blueRow[static_cast<std::size_t>(x)];
      rgbaPixels[pixelOffset + 3] = alphaRow[static_cast<std::size_t>(x)];
    }
  }

  progressFraction(1.0);
  return true;
}

bool GifWriter::beginExecution(std::string& error) {
  error.clear();

  const int imageWidth = width();
  const int imageHeight = height();
  if (imageWidth <= 0 || imageHeight <= 0) {
    error = "GIF export requires a non-empty image.";
    return false;
  }
  if (imageWidth > 65535 || imageHeight > 65535) {
    error = "GIF dimensions cannot exceed 65535 pixels.";
    return false;
  }

  const std::size_t widthValue = static_cast<std::size_t>(imageWidth);
  const std::size_t heightValue = static_cast<std::size_t>(imageHeight);
  if (widthValue > std::numeric_limits<std::size_t>::max() / heightValue / 4U) {
    error = "GIF frame dimensions exceed the supported memory size.";
    return false;
  }

  frameWidth_ = imageWidth;
  frameHeight_ = imageHeight;
  frameByteSize_ = widthValue * heightValue * 4U;
  encoderOptions_ = makeEncoderOptions(
      hasOutputAlphaChannel(),
      loopMode_,
      loopCount_,
      ditherMode_,
      maxColorsMode_,
      transparencyThreshold_,
      fps_);

  if (!open()) {
    executionFailed_ = true;
    return false;
  }
  fileOpen_ = true;

  spoolFile_ = std::tmpfile();
  if (!spoolFile_) {
    error = "Failed to create the temporary GIF frame cache.";
    return false;
  }

  return true;
}

bool GifWriter::appendFrameToSpool(
    const std::vector<std::uint8_t>& rgbaPixels,
    std::string& error) {
  error.clear();
  if (!spoolFile_) {
    error = "The temporary GIF frame cache is not open.";
    return false;
  }
  if (rgbaPixels.size() != frameByteSize_) {
    error = "GIF frame dimensions changed while writing the temporary frame cache.";
    return false;
  }

  const std::size_t writtenBytes =
      std::fwrite(rgbaPixels.data(), 1U, rgbaPixels.size(), spoolFile_);
  if (writtenBytes != rgbaPixels.size()) {
    error = "Failed to write a frame to the temporary GIF frame cache. Check available disk space.";
    return false;
  }

  ++spooledFrameCount_;
  return true;
}

bool GifWriter::readFrameFromSpool(
    std::vector<std::uint8_t>& rgbaPixels,
    std::string& error) {
  error.clear();
  if (!spoolFile_) {
    error = "The temporary GIF frame cache is not open.";
    return false;
  }

  rgbaPixels.resize(frameByteSize_);
  const std::size_t readBytes =
      std::fread(rgbaPixels.data(), 1U, rgbaPixels.size(), spoolFile_);
  if (readBytes != rgbaPixels.size()) {
    error = "Failed to read a complete frame from the temporary GIF frame cache.";
    return false;
  }

  return true;
}

void GifWriter::reportExecutionFailure(const std::string& error) {
  if (!error.empty()) {
    iop->critical("%s", error.c_str());
  }

  executionFailed_ = true;
  closeSpoolFile();
  if (fileOpen_) {
    close();
    fileOpen_ = false;
  }

  frameWidth_ = 0;
  frameHeight_ = 0;
  frameByteSize_ = 0;
  spooledFrameCount_ = 0;
  paletteSamples_ = GifPaletteSampleSet{};
}

void GifWriter::closeSpoolFile() {
  if (spoolFile_) {
    std::fclose(spoolFile_);
    spoolFile_ = nullptr;
  }
}

void GifWriter::updateKnobVisibility() {
  Knob* loopCountKnob = iop->knob("loop_count");
  if (loopCountKnob) {
    const bool shouldBeVisible = loopMode_ == kLoopFixed;
    if (loopCountKnob->isVisible() != shouldBeVisible) {
      loopCountKnob->visible(shouldBeVisible);
      loopCountKnob->updateWidgets();
    }
  }

  Knob* loopCountSuffixKnob = iop->knob("loop_count_suffix");
  if (loopCountSuffixKnob) {
    const bool shouldBeVisible = loopMode_ == kLoopFixed;
    if (loopCountSuffixKnob->isVisible() != shouldBeVisible) {
      loopCountSuffixKnob->visible(shouldBeVisible);
      loopCountSuffixKnob->updateWidgets();
    }
  }
}

void GifWriter::resetExecutionState() {
  closeSpoolFile();
  fileOpen_ = false;
  executionFailed_ = false;
  frameWidth_ = 0;
  frameHeight_ = 0;
  frameByteSize_ = 0;
  spooledFrameCount_ = 0;
  encoderOptions_ = GifEncoderOptions{};
  paletteSamples_ = GifPaletteSampleSet{};
}

} // namespace GifExporter
