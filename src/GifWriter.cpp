#include "GifWriter.h"
#include "GifEncoder.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "DDImage/Channel.h"
#include "DDImage/Knobs.h"
#include "DDImage/LUT.h"
#include "DDImage/OutputContext.h"
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
      transparency_(true),
      matteColor_{0.0F, 0.0F, 0.0F},
      fps_(24.0),
      diagnosticsEnabled_(false),
      movieQueryCount_(0),
      executeCallCount_(0),
      finishCallCount_(0),
      receivedFrameCount_(0),
      fileOpen_(false),
      headerWritten_(false),
      encodedFrameCount_(0),
      hasPreviousFrame_(false),
      previousFrameHasTransparency_(false) {
  if (root_real_fps) {
    fps_ = root_real_fps();
  }
}

void GifWriter::execute() {
  ++executeCallCount_;
  ++receivedFrameCount_;
  logDiagnostics("execute.begin");

  if (!fileOpen_) {
    if (!open()) {
      logDiagnostics("execute.open_failed");
      return;
    }

    fileOpen_ = true;
    logDiagnostics("execute.open_ok");
  }

  std::vector<std::uint8_t> rgbaPixels;
  std::string error;
  if (!readCurrentFrameRGBA(rgbaPixels, error)) {
    iop->critical("%s", error.c_str());
    logDiagnostics("execute.read_failed", error.c_str());
    close();
    resetExecutionState();
    return;
  }

  const bool alphaAvailable = hasOutputAlphaChannel();

  GifEncoderOptions options;
  options.useTransparency = transparency_ && alphaAvailable;
  options.ditherMode = static_cast<GifDitherMode>(ditherMode_);
  options.matteRed = static_cast<std::uint8_t>(std::clamp(matteColor_[0], 0.0F, 1.0F) * 255.0F + 0.5F);
  options.matteGreen = static_cast<std::uint8_t>(std::clamp(matteColor_[1], 0.0F, 1.0F) * 255.0F + 0.5F);
  options.matteBlue = static_cast<std::uint8_t>(std::clamp(matteColor_[2], 0.0F, 1.0F) * 255.0F + 0.5F);
  options.loopMode = loopMode_ == kLoopFixed
      ? GifLoopMode::kFixed
      : (loopMode_ == kLoopNone ? GifLoopMode::kNone : GifLoopMode::kInfinite);
  options.loopCount = loopCount_;
  options.frameDelayCentiseconds = std::max(1, static_cast<int>(std::lround(100.0 / fps_)));
  options.maxColors = selectedMaxColors(maxColorsMode_);

  GifIndexedFrame indexedFrame;
  if (!QuantizeGifFrame(width(), height(), rgbaPixels, options, indexedFrame, error)) {
    iop->critical("%s", error.c_str());
    logDiagnostics("execute.quantize_failed", error.c_str());
    close();
    resetExecutionState();
    return;
  }

  if (!headerWritten_) {
    std::vector<std::uint8_t> headerBytes;
    if (!EncodeGifAnimationHeader(width(), height(), options, headerBytes, error)) {
      iop->critical("%s", error.c_str());
      logDiagnostics("execute.header_failed", error.c_str());
      close();
      resetExecutionState();
      return;
    }

    if (!write(headerBytes.data(), static_cast<FILE_OFFSET>(headerBytes.size()))) {
      iop->critical("Failed to write GIF animation header.");
      logDiagnostics("execute.header_write_failed");
      close();
      resetExecutionState();
      return;
    }

    headerWritten_ = true;
    logDiagnostics("execute.header_ok");
  }

  GifIndexedFrame previousFrame;
  const GifIndexedFrame* previousFramePtr = nullptr;
  if (hasPreviousFrame_) {
    previousFrame.pixels = previousIndexedPixels_;
    previousFrame.hasTransparentPixels = previousFrameHasTransparency_;
    previousFramePtr = &previousFrame;
  }

  std::vector<std::uint8_t> frameBytes;
  if (!EncodeGifAnimationFrame(width(), height(), indexedFrame, previousFramePtr, options, true, frameBytes, error)) {
    iop->critical("%s", error.c_str());
    logDiagnostics("execute.encode_failed", error.c_str());
    close();
    resetExecutionState();
    return;
  }

  if (!write(frameBytes.data(), static_cast<FILE_OFFSET>(frameBytes.size()))) {
    iop->critical("Failed to write GIF frame data.");
    logDiagnostics("execute.write_failed");
    close();
    resetExecutionState();
    return;
  }

  previousIndexedPixels_ = indexedFrame.pixels;
  previousFrameHasTransparency_ = indexedFrame.hasTransparentPixels;
  hasPreviousFrame_ = true;
  ++encodedFrameCount_;
  logDiagnostics("execute.write_ok", "frame_written");
  logDiagnostics("execute.end");
}

bool GifWriter::movie() const {
  ++movieQueryCount_;
  logDiagnostics("movie.query");
  return true;
}

void GifWriter::finish() {
  ++finishCallCount_;
  logDiagnostics("finish");

  if (fileOpen_) {
    if (headerWritten_ && encodedFrameCount_ > 0) {
      std::vector<std::uint8_t> trailerBytes;
      EncodeGifAnimationTrailer(trailerBytes);
      if (!write(trailerBytes.data(), static_cast<FILE_OFFSET>(trailerBytes.size()))) {
        iop->critical("Failed to finalize the GIF animation.");
        logDiagnostics("finish.trailer_write_failed");
      } else {
        logDiagnostics("finish.trailer_ok");
      }
    }

    close();
    logDiagnostics("finish.close_ok");
  }

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

  Bool_knob(callback, &transparency_, "transparency", "transparency");
  SetFlags(callback, Knob::STARTLINE);
  Tooltip(callback, "Preserve transparency in the exported GIF when possible.");

  Enumeration_knob(callback, &ditherMode_, kDitherModeLabels, "dither", "dither");
  Tooltip(callback, "Controls how palette quantization dithering is applied.");

  Enumeration_knob(callback, &maxColorsMode_, kMaxColorsLabels, "max_colors", "max colors");
  Tooltip(callback, "Limits the effective palette size. Fewer colors usually reduce file size.");

  Color_knob(callback, matteColor_, IRange(0.0, 1.0, true), "matte_color", "matte color");
  Tooltip(callback, "Matte color used when transparency is disabled or needs flattening.");

  Double_knob(callback, &fps_, IRange(0.01, 240.0, true), "fps", "fps");
  ClearFlags(callback, Knob::SLIDER);
  Tooltip(callback, "Defaults to the current Nuke project frame rate.");

  Bool_knob(callback, &diagnosticsEnabled_, "diagnostics", "diagnostics");
  Tooltip(callback, "Write a sidecar log next to the output GIF recording movie(), execute(), and finish() calls.");

  updateKnobVisibility();
}

int GifWriter::knob_changed(Knob* knob) {
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

  updateKnobVisibility();

  if (knob == iop->knob("diagnostics")) {
    logDiagnostics("knob_changed");
  }

  return 1;
}

LUT* GifWriter::defaultLUT() const {
  return LUT::GetLut(LUT::INT8, this);
}

bool GifWriter::hasOutputAlphaChannel() const {
  const int channelCount = iop->depth();
  for (int channelIndex = 0; channelIndex < channelCount; ++channelIndex) {
    if (iop->channel_written_to(channelIndex) == Chan_Alpha) {
      return true;
    }
  }
  return false;
}

bool GifWriter::hasSourceAlphaChannel() const {
  return (input0().channels() & Mask_Alpha) != 0;
}

bool GifWriter::readCurrentFrameRGBA(std::vector<std::uint8_t>& rgbaPixels, std::string& error) {
  error.clear();

  const int imageWidth = width();
  const int imageHeight = height();
  if (imageWidth <= 0 || imageHeight <= 0) {
    error = "GIF export requires a non-empty image.";
    return false;
  }

  const bool sourceAlphaAvailable = hasSourceAlphaChannel();
  const ChannelSet channels = sourceAlphaAvailable ? Mask_RGBA : Mask_RGB;

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

    const float* alphaInput = sourceAlphaAvailable ? row[Chan_Alpha] : nullptr;
    to_byte(0, redRow.data(), row[Chan_Red], alphaInput, imageWidth);
    to_byte(1, greenRow.data(), row[Chan_Green], alphaInput, imageWidth);
    to_byte(2, blueRow.data(), row[Chan_Blue], alphaInput, imageWidth);

    if (sourceAlphaAvailable) {
      to_byte(3, alphaRow.data(), row[Chan_Alpha], nullptr, imageWidth);
    } else {
      std::fill(alphaRow.begin(), alphaRow.end(), static_cast<std::uint8_t>(255));
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

void GifWriter::updateKnobVisibility() {
  Knob* loopCountKnob = iop->knob("loop_count");
  if (loopCountKnob) {
    loopCountKnob->visible(loopMode_ == kLoopFixed);
  }

  Knob* loopCountSuffixKnob = iop->knob("loop_count_suffix");
  if (loopCountSuffixKnob) {
    loopCountSuffixKnob->visible(loopMode_ == kLoopFixed);
  }

  Knob* transparencyKnob = iop->knob("transparency");
  if (transparencyKnob) {
    transparencyKnob->enable(hasOutputAlphaChannel());
  }
}

std::string GifWriter::diagnosticsLogPath() const {
  return std::string(filename()) + ".gifwriter-phase2.log";
}

void GifWriter::logDiagnostics(const char* event, const char* detail) const {
  if (!diagnosticsEnabled_) {
    return;
  }

  std::ofstream stream(diagnosticsLogPath().c_str(), std::ios::app);
  if (!stream) {
    return;
  }

  const OutputContext& outputContext = iop->outputContext();

  std::ostringstream views;
  bool first = true;
  for (int view : executingViews()) {
    if (!first) {
      views << ",";
    }
    views << view;
    first = false;
  }

  stream
      << "event=" << event
      << " writer_mode=movie"
      << " movie=true"
      << " movie_queries=" << movieQueryCount_
      << " execute_calls=" << executeCallCount_
      << " finish_calls=" << finishCallCount_
      << " received_frames=" << receivedFrameCount_
      << " file_open=" << (fileOpen_ ? "true" : "false")
      << " header_written=" << (headerWritten_ ? "true" : "false")
      << " encoded_frames=" << encodedFrameCount_
      << " frame=" << outputContext.frame()
      << " filename=\"" << filename() << "\""
      << " views=" << views.str()
      << " aborted=" << (aborted() ? "true" : "false");

  if (detail && *detail) {
    stream << " detail=\"" << detail << "\"";
  }

  stream << "\n";
}

void GifWriter::resetExecutionState() {
  movieQueryCount_ = 0;
  executeCallCount_ = 0;
  finishCallCount_ = 0;
  receivedFrameCount_ = 0;
  fileOpen_ = false;
  headerWritten_ = false;
  encodedFrameCount_ = 0;
  hasPreviousFrame_ = false;
  previousFrameHasTransparency_ = false;
  previousIndexedPixels_.clear();
}

} // namespace GifExporter
