#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sublift/models.hpp"
#include "sublift/paddle_geometry.hpp"

namespace sublift::paddle_detail {

/// CTC 解码器：将类概率 logits 转化为字符串与平均置信度
struct CtcResult {
  std::string text;
  double confidence{0.0};
};

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
    return CtcResult{"", 0.0};
  }

  std::string decoded_text;
  double score_sum = 0.0;
  int valid_count = 0;
  int last_index = 0;  // 0 is blank

  for (int i = 0; i < seq_len; ++i) {
    const float* row = logits + i * num_classes;
    int max_idx = 0;
    float max_val = row[0];

    for (int c = 1; c < num_classes; ++c) {
      if (row[c] > max_val) {
        max_val = row[c];
        max_idx = c;
      }
    }

    if (max_idx != 0 && max_idx != last_index) {
      size_t dict_idx = static_cast<size_t>(max_idx - 1);
      if (dict_idx < dictionary.size()) {
        decoded_text += dictionary[dict_idx];
        score_sum += max_val;
        valid_count++;
      }
    }
    last_index = max_idx;
  }

  double avg_conf = valid_count > 0 ? (score_sum / valid_count) : 0.0;
  return CtcResult{std::move(decoded_text), avg_conf};
}

}  // namespace sublift::paddle_detail
