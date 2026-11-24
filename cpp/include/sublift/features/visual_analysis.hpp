#pragma once

#include <cstdint>
#include <sublift/core/image_buffer.hpp>
#include <sublift/core/image_view.hpp>
#include <sublift/features/mask.hpp>
#include <sublift/features/signature.hpp>

namespace sublift::features {

struct FrameVisualAnalysis {
  FrameSignature signature;
  ImageBuffer valid_mask;
};

[[nodiscard]] FrameVisualAnalysis compute_visual_analysis(const ImageView& image, std::int64_t timestamp_ms,
                                                         const MaskConfig& config = {});

} // namespace sublift::features
