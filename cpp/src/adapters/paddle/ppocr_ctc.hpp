#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define SUBLIFT_PADDLE_CTC_NEON 1
#endif

#include "sublift/models.hpp"
#include "sublift/adapters/paddle_geometry.hpp"

namespace sublift::paddle_detail {

/// CTC 解码器：将类概率 logits 转化为字符串与平均置信度
struct CtcResult {
  std::string text;
  double confidence{0.0};
  std::vector<std::int32_t> token_indices;
};

namespace detail {

/// Scalar reference implementation (Phase 20 measurement baseline).
[[nodiscard]] inline int argmax_first_scalar(const float* values,
                                             int count) noexcept {
  if (values == nullptr || count <= 0) {
    return 0;
  }
  int best_index = 0;
  float best_value = values[0];
  for (int position = 1; position < count; ++position) {
    const float candidate = values[position];
    if (candidate > best_value) {
      best_value = candidate;
      best_index = position;
    }
  }
  return best_index;
}

/// Index of the first maximum of values[0..count) under a strict >
/// left-to-right scan: ties keep the first occurrence and NaN never wins.
/// This mirrors RapidOCR's np.argmax over a single timestep while remaining
/// bit-for-bit equivalent to the scalar loop it replaces.
[[nodiscard]] inline int argmax_first(const float* values,
                                      int count) noexcept {
  if (values == nullptr || count <= 0) {
    return 0;
  }
  int best_index = 0;
  const float initial_best = values[0];
#if defined(SUBLIFT_PADDLE_CTC_NEON)
  // Vector scan: once a block is known to contain no value strictly greater
  // than the running maximum, the block is skipped entirely. When it does,
  // the block maximum becomes the new running maximum and the first lane
  // equal to it is the new index (equal lanes never replace an earlier max,
  // matching the scalar > comparison).
  float best_value = initial_best;
  const float32x4_t neg_infinity = vdupq_n_f32(-__builtin_inff());
  float32x4_t best_vector = vdupq_n_f32(best_value);
  int index = 1;
  for (; index + 8 <= count; index += 8) {
    const float32x4_t lower = vld1q_f32(values + index);
    const float32x4_t upper = vld1q_f32(values + index + 4);
    const uint32x4_t greater_lower = vcgtq_f32(lower, best_vector);
    const uint32x4_t greater_upper = vcgtq_f32(upper, best_vector);
    if (vmaxvq_u32(vorrq_u32(greater_lower, greater_upper)) == 0) {
      continue;
    }
    const float block_max = vmaxvq_f32(vmaxq_f32(
        vbslq_f32(greater_lower, lower, neg_infinity),
        vbslq_f32(greater_upper, upper, neg_infinity)));
    const float32x4_t block_vector = vdupq_n_f32(block_max);
    const uint32x4_t equal_lower =
        vandq_u32(greater_lower, vceqq_f32(lower, block_vector));
    const uint32x4_t equal_upper =
        vandq_u32(greater_upper, vceqq_f32(upper, block_vector));
    const std::uint32_t mask_lower =
        vgetq_lane_u32(equal_lower, 0) |
        (vgetq_lane_u32(equal_lower, 1) << 1) |
        (vgetq_lane_u32(equal_lower, 2) << 2) |
        (vgetq_lane_u32(equal_lower, 3) << 3);
    const std::uint32_t mask_upper =
        vgetq_lane_u32(equal_upper, 0) |
        (vgetq_lane_u32(equal_upper, 1) << 1) |
        (vgetq_lane_u32(equal_upper, 2) << 2) |
        (vgetq_lane_u32(equal_upper, 3) << 3);
    best_value = block_max;
    best_vector = block_vector;
    best_index = index + (mask_lower != 0U
                              ? __builtin_ctz(mask_lower)
                              : 4 + __builtin_ctz(mask_upper));
  }
  for (int position = index; position < count; ++position) {
    const float candidate = values[position];
    if (candidate > best_value) {
      best_value = candidate;
      best_index = position;
    }
  }
  return best_index;
#else
  float best_value = initial_best;
  for (int position = 1; position < count; ++position) {
    const float candidate = values[position];
    if (candidate > best_value) {
      best_value = candidate;
      best_index = position;
    }
  }
  return best_index;
#endif
}

}  // namespace detail

/// CTC 贪婪解码
/// @param logits 打平的 [seq_len, num_classes] 概率矩阵
/// @param seq_len 序列长度
/// @param num_classes 类别数（blank 索引为 0）
/// @param dictionary 字符映射词表（索引 1 ~ num_classes-1）
inline CtcResult ctc_greedy_decode(const float* logits,
                                   int seq_len,
                                   int num_classes,
                                   const std::vector<std::string>& dictionary) {
  if (!logits || seq_len <= 0 || num_classes <= 1) {
    return CtcResult{"", 0.0, {}};
  }

  std::string decoded_text;
  std::vector<std::int32_t> token_indices;
  // A full CJK character is up to three UTF-8 bytes; reserving per timestep
  // keeps the hot path allocation-free for typical sequences.
  decoded_text.reserve(static_cast<std::size_t>(seq_len) * 3);
  token_indices.reserve(static_cast<std::size_t>(seq_len));
  double score_sum = 0.0;
  int valid_count = 0;
  int last_index = 0;  // 0 is blank
  const auto round_to_five = [](double value) {
    return std::round(value * 100000.0) / 100000.0;
  };

  for (int i = 0; i < seq_len; ++i) {
    const float* row = logits + i * num_classes;
    const int max_idx = detail::argmax_first(row, num_classes);
    const float max_val = row[max_idx];

    if (max_idx != 0 && max_idx != last_index) {
      const std::size_t dict_idx = static_cast<std::size_t>(max_idx - 1);
      if (dict_idx < dictionary.size()) {
        decoded_text += dictionary[dict_idx];
        token_indices.push_back(max_idx);
        // RapidOCR rounds each selected probability before averaging.
        score_sum += round_to_five(static_cast<double>(max_val));
        valid_count++;
      }
    }
    last_index = max_idx;
  }

  const double avg_conf =
      valid_count > 0
          ? round_to_five(score_sum / static_cast<double>(valid_count))
          : 0.0;
  return CtcResult{
      std::move(decoded_text),
      avg_conf,
      std::move(token_indices),
  };
}

}  // namespace sublift::paddle_detail
