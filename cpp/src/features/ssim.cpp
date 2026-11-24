#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <sublift/features/ssim.hpp>

#include "opencv_thread_guard.hpp"

namespace sublift::features {

namespace {

cv::Mat image_view_to_mat(const ImageView& view) {
  if (view.empty()) {
    return cv::Mat();
  }
  int cv_type = (view.format() == PixelFormat::RGB24) ? CV_8UC3 : CV_8UC1;
  return cv::Mat(view.height(), view.width(), cv_type,
                 const_cast<std::uint8_t*>(view.data()),
                 static_cast<std::size_t>(view.stride_bytes()));
}

bool are_images_byte_identical(const ImageView& img1, const ImageView& img2) noexcept {
  if (img1.empty() || img2.empty()) {
    return false;
  }
  if (img1.width() != img2.width() || img1.height() != img2.height() || img1.format() != img2.format()) {
    return false;
  }
  if (img1.data() == img2.data() && img1.stride_bytes() == img2.stride_bytes()) {
    return true;
  }
  const std::size_t row_bytes = static_cast<std::size_t>(img1.width()) * (img1.format() == PixelFormat::RGB24 ? 3 : 1);
  for (int y = 0; y < img1.height(); ++y) {
    if (std::memcmp(img1.row(y), img2.row(y), row_bytes) != 0) {
      return false;
    }
  }
  return true;
}

cv::Mat to_gray_mat(const ImageView& view, cv::Mat& temp_storage) {
  if (view.empty()) {
    return cv::Mat();
  }
  cv::Mat src = image_view_to_mat(view);
  if (view.format() == PixelFormat::RGB24) {
    cv::cvtColor(src, temp_storage, cv::COLOR_RGB2GRAY);
    return temp_storage;
  }
  return src;
}

} // namespace

struct SsimWorkspace::Impl {
  std::vector<double> storage_;
  std::size_t capacity_elements_{0};

  cv::Mat g1, g2, mu1, mu2, mu1_sq, mu2_sq, mu1_mu2;
  cv::Mat g1g1, g2g2, g1g2, sg1, sg2, sg12;
  cv::Mat sigma1_sq, sigma2_sq, sigma12;
  cv::Mat t1, t2, t3, t4, num, den, ssim_map;

  void ensure_capacity(int rows, int cols) {
    if (rows <= 0 || cols <= 0) {
      return;
    }
    const std::size_t req = static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols);
    if (req > capacity_elements_) {
      capacity_elements_ = req;
      storage_.resize(23 * req);
    }
    double* p = storage_.data();
    const auto step = static_cast<std::size_t>(cols) * sizeof(double);
    auto bind = [&](cv::Mat& m, std::size_t idx) {
      m = cv::Mat(rows, cols, CV_64F, p + idx * capacity_elements_, step);
    };
    bind(g1, 0);
    bind(g2, 1);
    bind(mu1, 2);
    bind(mu2, 3);
    bind(mu1_sq, 4);
    bind(mu2_sq, 5);
    bind(mu1_mu2, 6);
    bind(g1g1, 7);
    bind(g2g2, 8);
    bind(g1g2, 9);
    bind(sg1, 10);
    bind(sg2, 11);
    bind(sg12, 12);
    bind(sigma1_sq, 13);
    bind(sigma2_sq, 14);
    bind(sigma12, 15);
    bind(t1, 16);
    bind(t2, 17);
    bind(t3, 18);
    bind(t4, 19);
    bind(num, 20);
    bind(den, 21);
    bind(ssim_map, 22);
  }
};

SsimWorkspace::SsimWorkspace() : impl_(std::make_unique<Impl>()) {}
SsimWorkspace::~SsimWorkspace() = default;
SsimWorkspace::SsimWorkspace(SsimWorkspace&&) noexcept = default;
SsimWorkspace& SsimWorkspace::operator=(SsimWorkspace&&) noexcept = default;

void SsimWorkspace::ensure_capacity(int rows, int cols) {
  if (impl_) {
    impl_->ensure_capacity(rows, cols);
  }
}

void* SsimWorkspace::raw_buffer_data() const noexcept {
  return (impl_ && !impl_->storage_.empty()) ? impl_->storage_.data() : nullptr;
}

std::size_t SsimWorkspace::capacity_elements() const noexcept {
  return impl_ ? impl_->capacity_elements_ : 0;
}

int SsimWorkspace::current_rows() const noexcept {
  return impl_ ? impl_->g1.rows : 0;
}

int SsimWorkspace::current_cols() const noexcept {
  return impl_ ? impl_->g1.cols : 0;
}

double SsimWorkspace::compute(const ImageView& img1, const ImageView& img2, int window_size) {
  return compute_ssim(img1, img2, window_size, this);
}

namespace {

double compute_ssim_core(const cv::Mat& gray1_in, const cv::Mat& gray2_in, int window_size,
                         SsimWorkspace::Impl* ws, bool is_mask) {
  if (window_size <= 0 || window_size % 2 == 0) {
    return 0.0;
  }
  if (gray1_in.empty() || gray2_in.empty()) {
    return (gray1_in.empty() && gray2_in.empty()) ? 1.0 : 0.0;
  }

  if (ws != nullptr) {
    ws->ensure_capacity(gray1_in.rows, gray1_in.cols);
  }

  cv::Mat g1_local, g2_local;
  gray1_in.convertTo(ws != nullptr ? ws->g1 : g1_local, CV_64F);
  gray2_in.convertTo(ws != nullptr ? ws->g2 : g2_local, CV_64F);
  cv::Mat& gray1 = ws != nullptr ? ws->g1 : g1_local;
  cv::Mat& gray2 = ws != nullptr ? ws->g2 : g2_local;

  if (gray1.size() != gray2.size()) {
    cv::resize(gray2, gray2, gray1.size(), 0, 0, cv::INTER_AREA);
  }

  const double c1 = (0.01 * 255.0) * (0.01 * 255.0); // 6.5025
  const double c2 = (0.03 * 255.0) * (0.03 * 255.0); // 58.5225
  const cv::Size win(window_size, window_size);

  cv::Mat mu1_local, mu2_local;
  cv::GaussianBlur(gray1, ws != nullptr ? ws->mu1 : mu1_local, win, 0);
  cv::GaussianBlur(gray2, ws != nullptr ? ws->mu2 : mu2_local, win, 0);
  cv::Mat& mu1 = ws != nullptr ? ws->mu1 : mu1_local;
  cv::Mat& mu2 = ws != nullptr ? ws->mu2 : mu2_local;

  cv::Mat mu1_sq_local, mu2_sq_local, mu1_mu2_local;
  cv::multiply(mu1, mu1, ws != nullptr ? ws->mu1_sq : mu1_sq_local);
  cv::multiply(mu2, mu2, ws != nullptr ? ws->mu2_sq : mu2_sq_local);
  cv::multiply(mu1, mu2, ws != nullptr ? ws->mu1_mu2 : mu1_mu2_local);
  cv::Mat& mu1_sq = ws != nullptr ? ws->mu1_sq : mu1_sq_local;
  cv::Mat& mu2_sq = ws != nullptr ? ws->mu2_sq : mu2_sq_local;
  cv::Mat& mu1_mu2 = ws != nullptr ? ws->mu1_mu2 : mu1_mu2_local;

  cv::Mat sigma1_sq_local, sigma2_sq_local, sigma12_local;
  cv::Mat& sigma1_sq = ws != nullptr ? ws->sigma1_sq : sigma1_sq_local;
  cv::Mat& sigma2_sq = ws != nullptr ? ws->sigma2_sq : sigma2_sq_local;
  cv::Mat& sigma12 = ws != nullptr ? ws->sigma12 : sigma12_local;

  if (is_mask) {
    // Binary mask linear identity: sigma^2 = 255.0 * mu - mu_sq
    cv::Mat sg1_local, sg2_local;
    cv::Mat& sg1 = ws != nullptr ? ws->sg1 : sg1_local;
    cv::Mat& sg2 = ws != nullptr ? ws->sg2 : sg2_local;
    mu1.convertTo(sg1, -1, 255.0, 0.0);
    cv::subtract(sg1, mu1_sq, sigma1_sq);
    mu2.convertTo(sg2, -1, 255.0, 0.0);
    cv::subtract(sg2, mu2_sq, sigma2_sq);
  } else {
    cv::Mat g1g1_local, g2g2_local;
    cv::multiply(gray1, gray1, ws != nullptr ? ws->g1g1 : g1g1_local);
    cv::multiply(gray2, gray2, ws != nullptr ? ws->g2g2 : g2g2_local);
    cv::Mat& g1g1 = ws != nullptr ? ws->g1g1 : g1g1_local;
    cv::Mat& g2g2 = ws != nullptr ? ws->g2g2 : g2g2_local;

    cv::Mat sg1_local, sg2_local;
    cv::GaussianBlur(g1g1, ws != nullptr ? ws->sg1 : sg1_local, win, 0);
    cv::GaussianBlur(g2g2, ws != nullptr ? ws->sg2 : sg2_local, win, 0);
    cv::Mat& sg1 = ws != nullptr ? ws->sg1 : sg1_local;
    cv::Mat& sg2 = ws != nullptr ? ws->sg2 : sg2_local;
    cv::subtract(sg1, mu1_sq, sigma1_sq);
    cv::subtract(sg2, mu2_sq, sigma2_sq);
  }

  cv::Mat g1g2_local;
  cv::multiply(gray1, gray2, ws != nullptr ? ws->g1g2 : g1g2_local);
  cv::Mat& g1g2 = ws != nullptr ? ws->g1g2 : g1g2_local;

  cv::Mat sg12_local;
  cv::GaussianBlur(g1g2, ws != nullptr ? ws->sg12 : sg12_local, win, 0);
  cv::Mat& sg12 = ws != nullptr ? ws->sg12 : sg12_local;
  cv::subtract(sg12, mu1_mu2, sigma12);

  cv::Mat t1_local, t2_local, t3_local, t4_local;
  cv::Mat& t1 = ws != nullptr ? ws->t1 : t1_local;
  cv::Mat& t2 = ws != nullptr ? ws->t2 : t2_local;
  cv::Mat& t3 = ws != nullptr ? ws->t3 : t3_local;
  cv::Mat& t4 = ws != nullptr ? ws->t4 : t4_local;
  t1 = 2.0 * mu1_mu2 + c1;
  t2 = 2.0 * sigma12 + c2;
  t3 = mu1_sq + mu2_sq + c1;
  t4 = sigma1_sq + sigma2_sq + c2;

  cv::Mat num_local, den_local, ssim_map_local;
  cv::Mat& num = ws != nullptr ? ws->num : num_local;
  cv::Mat& den = ws != nullptr ? ws->den : den_local;
  cv::Mat& ssim_map = ws != nullptr ? ws->ssim_map : ssim_map_local;
  cv::multiply(t1, t2, num);
  cv::multiply(t3, t4, den);
  cv::divide(num, den, ssim_map);

  cv::Scalar mssim = cv::mean(ssim_map);
  return mssim[0];
}

bool are_mats_byte_identical(const cv::Mat& m1, const cv::Mat& m2) noexcept {
  if (m1.empty() || m2.empty() || m1.rows != m2.rows || m1.cols != m2.cols ||
      m1.type() != m2.type()) {
    return false;
  }
  if (m1.data == m2.data && m1.step == m2.step) {
    return true;
  }
  const std::size_t row_bytes = static_cast<std::size_t>(m1.cols) * m1.elemSize();
  for (int y = 0; y < m1.rows; ++y) {
    if (std::memcmp(m1.ptr(y), m2.ptr(y), row_bytes) != 0) {
      return false;
    }
  }
  return true;
}

double normalize_mask_ssim_footprint(const cv::Mat& m1, const cv::Mat& m2,
                                     double raw_ssim) {
  if (raw_ssim >= 1.0 || are_mats_byte_identical(m1, m2)) {
    return 1.0;
  }
  cv::Mat uni;
  cv::bitwise_or(m1, m2, uni);
  if (cv::countNonZero(uni) == 0) {
    return 1.0;
  }
  static const cv::Mat kKernel7x7 =
      cv::getStructuringElement(cv::MORPH_RECT, cv::Size(7, 7));
  cv::Mat d_uni;
  cv::dilate(uni, d_uni, kKernel7x7);
  const double active_frac =
      static_cast<double>(cv::countNonZero(d_uni)) / static_cast<double>(d_uni.total());
  constexpr double kRefFootprint = 0.12;
  const double eff_frac = std::clamp(active_frac, 0.015, 0.13);
  const double norm_dissim = (1.0 - raw_ssim) * (kRefFootprint / eff_frac);
  return std::clamp(1.0 - norm_dissim, 0.0, 1.0);
}

double normalize_wide_mask_ssim(const cv::Mat& m1, const cv::Mat& m2,
                                double raw_ssim, int window_size,
                                SsimWorkspace::Impl* ws,
                                double aspect_ratio_threshold) {
  if (m1.empty() || m2.empty() || m1.size() != m2.size() ||
      aspect_ratio_threshold <= 0.0) {
    return raw_ssim;
  }
  if (raw_ssim >= 1.0 || are_mats_byte_identical(m1, m2)) {
    return 1.0;
  }
  const double aspect =
      static_cast<double>(m1.cols) / std::max(1.0, static_cast<double>(m1.rows));
  if (aspect <= aspect_ratio_threshold) {
    return raw_ssim;
  }

  double final_ssim = normalize_mask_ssim_footprint(m1, m2, raw_ssim);

  // Multiline row-wise mask comparison (H >= 60 px and active pixels in both halves)
  const int h = m1.rows;
  if (h >= 60) {
    const int half_h = h / 2;
    const cv::Mat m1_top = m1.rowRange(0, half_h);
    const cv::Mat m2_top = m2.rowRange(0, half_h);
    const cv::Mat m1_bot = m1.rowRange(half_h, h);
    const cv::Mat m2_bot = m2.rowRange(half_h, h);

    cv::Mat uni_top, uni_bot;
    cv::bitwise_or(m1_top, m2_top, uni_top);
    cv::bitwise_or(m1_bot, m2_bot, uni_bot);

    const double total_half = static_cast<double>(half_h) * static_cast<double>(m1.cols);
    const double frac_top = static_cast<double>(cv::countNonZero(uni_top)) / total_half;
    const double frac_bot = static_cast<double>(cv::countNonZero(uni_bot)) / total_half;

    if (frac_top >= 0.005 && frac_bot >= 0.005) {
      double ssim_top = 1.0;
      if (!are_mats_byte_identical(m1_top, m2_top)) {
        const double raw_top = compute_ssim_core(m1_top, m2_top, window_size, ws, true);
        ssim_top = normalize_mask_ssim_footprint(m1_top, m2_top, raw_top);
      }

      double ssim_bot = 1.0;
      if (!are_mats_byte_identical(m1_bot, m2_bot)) {
        const double raw_bot = compute_ssim_core(m1_bot, m2_bot, window_size, ws, true);
        ssim_bot = normalize_mask_ssim_footprint(m1_bot, m2_bot, raw_bot);
      }

      if (ssim_top < 0.90 || ssim_bot < 0.90) {
        final_ssim = std::min(final_ssim, std::min(ssim_top, ssim_bot));
      }
    }
  }

  // 4 overlapping horizontal sectors for wide subtitle strips
  struct SectorRange {
    double x0;
    double x1;
  };
  constexpr SectorRange kSectors[] = {
      {0.00, 0.28},
      {0.20, 0.55},
      {0.45, 0.80},
      {0.72, 1.00},
  };

  const int w = m1.cols;
  for (const auto& sec : kSectors) {
    const int x0 = std::max(0, static_cast<int>(sec.x0 * w));
    const int x1 = (sec.x1 >= 1.0) ? w : std::min(w, static_cast<int>(sec.x1 * w));
    if (x1 <= x0) {
      continue;
    }
    const cv::Mat s1 = m1.colRange(x0, x1);
    const cv::Mat s2 = m2.colRange(x0, x1);

    if (are_mats_byte_identical(s1, s2)) {
      continue;
    }

    cv::Mat uni_sec;
    cv::bitwise_or(s1, s2, uni_sec);
    const double sec_area = static_cast<double>(s1.total());
    if (sec_area <= 0.0) {
      continue;
    }
    const double sec_frac = static_cast<double>(cv::countNonZero(uni_sec)) / sec_area;

    if (sec_frac >= 0.005) {
      const double raw_sec = compute_ssim_core(s1, s2, window_size, ws, true);
      const double ssim_sec = normalize_mask_ssim_footprint(s1, s2, raw_sec);
      if (ssim_sec < 0.90) {
        final_ssim = std::min(final_ssim, ssim_sec);
      }
    }
  }

  if (ws != nullptr) {
    ws->ensure_capacity(m1.rows, m1.cols);
  }

  return final_ssim;
}

} // namespace

double compute_ssim(const ImageView& img1, const ImageView& img2, int window_size, SsimWorkspace* workspace) {
  detail::ensure_opencv_single_thread();
  if (window_size <= 0 || window_size % 2 == 0) {
    return 0.0;
  }
  if (img1.empty() || img2.empty()) {
    return (img1.empty() && img2.empty()) ? 1.0 : 0.0;
  }
  if (are_images_byte_identical(img1, img2)) {
    return 1.0;
  }

  cv::Mat temp1, temp2;
  cv::Mat g1 = to_gray_mat(img1, temp1);
  cv::Mat g2 = to_gray_mat(img2, temp2);

  return compute_ssim_core(g1, g2, window_size, workspace ? workspace->impl_.get() : nullptr, false);
}

double compute_mask_ssim(const ImageView& mask1, const ImageView& mask2, int window_size,
                         SsimWorkspace* workspace, double aspect_ratio_threshold) {
  detail::ensure_opencv_single_thread();
  if (window_size <= 0 || window_size % 2 == 0) {
    return 0.0;
  }
  if (mask1.empty() || mask2.empty()) {
    return (mask1.empty() && mask2.empty()) ? 1.0 : 0.0;
  }
  if (are_images_byte_identical(mask1, mask2)) {
    return 1.0;
  }

  cv::Mat temp1, temp2;
  cv::Mat g1 = to_gray_mat(mask1, temp1);
  cv::Mat g2 = to_gray_mat(mask2, temp2);

  SsimWorkspace::Impl* ws = workspace ? workspace->impl_.get() : nullptr;
  const double raw_ssim = compute_ssim_core(g1, g2, window_size, ws, true);
  return normalize_wide_mask_ssim(g1, g2, raw_ssim, window_size, ws, aspect_ratio_threshold);
}

} // namespace sublift::features
