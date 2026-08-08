#pragma once

#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include "GifEncoder.h"

#include "DDImage/Channel.h"
#include "DDImage/FileWriter.h"
#include "DDImage/Knob.h"

namespace DD {
namespace Image {
class LUT;
} // namespace Image
} // namespace DD

namespace GifExporter {

class GifWriter final : public DD::Image::FileWriter {
public:
  enum LoopMode {
    kLoopInfinite = 0,
    kLoopFixed = 1,
    kLoopNone = 2
  };

  enum DitherMode {
    kDitherNone = 0,
    kDitherFloydSteinberg = 1,
    kDitherOrdered = 2
  };

  explicit GifWriter(DD::Image::Write* writeNode);
  ~GifWriter() override;

  void execute() override;
  bool movie() const override;
  void finish() override;
  void knobs(DD::Image::Knob_Callback callback) override;
  int knob_changed(DD::Image::Knob* knob) override;

  static DD::Image::Writer* Build(DD::Image::Write* writeNode);
  static const DD::Image::Writer::Description kDescription;

protected:
  DD::Image::LUT* defaultLUT() const override;

private:
  std::vector<DD::Image::Channel> selectedWriteChannels() const;
  bool hasOutputAlphaChannel() const;
  bool readCurrentFrameRGBA(std::vector<std::uint8_t>& rgbaPixels, std::string& error);
  bool beginExecution(std::string& error);
  bool appendFrameToSpool(const std::vector<std::uint8_t>& rgbaPixels, std::string& error);
  bool readFrameFromSpool(std::vector<std::uint8_t>& rgbaPixels, std::string& error);
  void reportExecutionFailure(const std::string& error);
  void closeSpoolFile();
  void resetExecutionState();
  void updateKnobVisibility();

  int loopMode_;
  int loopCount_;
  int ditherMode_;
  int maxColorsMode_;
  float transparencyThreshold_;
  double fps_;

  bool fileOpen_;
  bool executionFailed_;
  std::FILE* spoolFile_;
  int frameWidth_;
  int frameHeight_;
  std::size_t frameByteSize_;
  std::size_t spooledFrameCount_;
  GifEncoderOptions encoderOptions_;
  GifPaletteSampleSet paletteSamples_;
};

} // namespace GifExporter
