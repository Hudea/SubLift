#include "sublift/ffmpeg.hpp"

namespace sublift::ffmpeg {

bool available() noexcept {
  // Real PATH probe lands with the extractor implementation.
  return false;
}

}  // namespace sublift::ffmpeg
