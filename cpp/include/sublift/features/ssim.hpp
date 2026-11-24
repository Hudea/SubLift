#pragma once

#include <cstddef>
#include <memory>
#include <sublift/core/image_view.hpp>

namespace sublift::features {

class SsimWorkspace;

[[nodiscard]] double compute_ssim(const ImageView& img1, const ImageView& img2, int window_size = 7,
                                  SsimWorkspace* workspace = nullptr);

[[nodiscard]] double compute_mask_ssim(const ImageView& mask1, const ImageView& mask2, int window_size = 7,
                                       SsimWorkspace* workspace = nullptr,
                                       double aspect_ratio_threshold = 6.0);

class SsimWorkspace {
 public:
  struct Impl;

  SsimWorkspace();
  ~SsimWorkspace();

  SsimWorkspace(SsimWorkspace&&) noexcept;
  SsimWorkspace& operator=(SsimWorkspace&&) noexcept;
  SsimWorkspace(const SsimWorkspace&) = delete;
  SsimWorkspace& operator=(const SsimWorkspace&) = delete;

  [[nodiscard]] double compute(const ImageView& img1, const ImageView& img2, int window_size = 7);

  void ensure_capacity(int rows, int cols);
  [[nodiscard]] void* raw_buffer_data() const noexcept;
  [[nodiscard]] std::size_t capacity_elements() const noexcept;
  [[nodiscard]] int current_rows() const noexcept;
  [[nodiscard]] int current_cols() const noexcept;

 private:
  friend double compute_ssim(const ImageView& img1, const ImageView& img2, int window_size,
                             SsimWorkspace* workspace);
  friend double compute_mask_ssim(const ImageView& mask1, const ImageView& mask2, int window_size,
                                  SsimWorkspace* workspace, double aspect_ratio_threshold);

  std::unique_ptr<Impl> impl_;
};

} // namespace sublift::features
