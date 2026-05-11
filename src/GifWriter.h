#pragma once

#include <cstdint>
#include <string>
#include <vector>

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
  bool hasOutputAlphaChannel() const;
  bool hasSourceAlphaChannel() const;
  bool readCurrentFrameRGBA(std::vector<std::uint8_t>& rgbaPixels, std::string& error);
  std::string diagnosticsLogPath() const;
  void logDiagnostics(const char* event, const char* detail = nullptr) const;
  void resetExecutionState();
  void updateKnobVisibility();

  int loopMode_;
  int loopCount_;
  int ditherMode_;
  int maxColorsMode_;
  bool transparency_;
  float matteColor_[3];
  double fps_;
  bool diagnosticsEnabled_;

  mutable int movieQueryCount_;
  int executeCallCount_;
  int finishCallCount_;
  int receivedFrameCount_;
  bool fileOpen_;
  bool headerWritten_;
  int encodedFrameCount_;
  bool hasPreviousFrame_;
  bool previousFrameHasTransparency_;
  std::vector<std::uint8_t> previousIndexedPixels_;
};

} // namespace GifExporter
