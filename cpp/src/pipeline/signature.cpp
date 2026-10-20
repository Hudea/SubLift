#include "sublift/signature.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <utility>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace sublift {

// --- O2: reusable SSIM workspace -------------------------------------------
// Defined before the helpers below so the workspace-aware SSIM core can use
// the complete type. Kept out of the anonymous namespace: it is the state of
// a public (opaque) type declared in signature.hpp.

struct SsimWorkspace::Impl {
  cv::Mat g1;
  cv::Mat g2;
  cv::Mat mu1;
  cv::Mat mu2;
  cv::Mat mu1_sq;
  cv::Mat mu2_sq;
  cv::Mat mu1_mu2;
  cv::Mat g1g1;
  cv::Mat g2g2;
  cv::Mat g1g2;
  cv::Mat sg1;
  cv::Mat sg2;
  cv::Mat sg12;
  cv::Mat sigma1_sq;
  cv::Mat sigma2_sq;
  cv::Mat sigma12;
};

SsimWorkspace::SsimWorkspace() : impl_(std::make_unique<Impl>()) {}
SsimWorkspace::~SsimWorkspace() = default;
SsimWorkspace::SsimWorkspace(SsimWorkspace&& other) noexcept = default;
SsimWorkspace& SsimWorkspace::operator=(SsimWorkspace&& other) noexcept = default;

namespace {

using cv::Mat;

/// Convert an ImageView to a grayscale cv::Mat (CV_8UC1), reproducing Python
/// `_to_gray`: Gray8 is returned as-is (shared memory, not copied); 3-channel
/// formats go through COLOR_RGB2GRAY *regardless of byte order* - this is the
/// historical color-space quirk (BGR bytes tagged RGB24 are processed as RGB).
/// For Gray8 input the returned Mat does NOT own pixel data.
Mat to_gray_mat(const ImageView& view) {
  if (view.empty()) {
    throw std::invalid_argument("to_gray: empty image");
  }
  const auto step = static_cast<std::size_t>(view.stride_bytes());
  if (view.format() == PixelFormat::Gray8) {
    return Mat(view.height(), view.width(), CV_8UC1,
               const_cast<std::uint8_t*>(view.data()), step);
  }
  // RGB24 / BGR24: COLOR_RGB2GRAY for both (quirk). BGR24 enum is treated
  // identically to RGB24 in 6.1; a correct BGR2GRAY path is out of scope.
  Mat src(view.height(), view.width(), CV_8UC3,
          const_cast<std::uint8_t*>(view.data()), step);
  Mat gray;
  cv::cvtColor(src, gray, cv::COLOR_RGB2GRAY);
  return gray;
}

/// Matches `_compute_block_size`: max(3, int(h*ratio)) then odd-up.
int compute_block_size(int band_height, double ratio) {
  int size = std::max(3, static_cast<int>(band_height * ratio));
  return size % 2 == 1 ? size : size + 1;
}

struct ForegroundResult {
  double ratio;
  Mat binary;
};

/// Matches `_compute_foreground_ratio`: adaptiveThreshold(GAUSSIAN_C, BINARY_INV)
/// -> MORPH_OPEN(2x2 rect) -> nonzero / total.
ForegroundResult compute_foreground_ratio_mat(const Mat& gray,
                                              const SignatureConfig& config) {
  const int block_size = compute_block_size(gray.rows, config.block_size_ratio);
  Mat binary;
  cv::adaptiveThreshold(gray, binary, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                        cv::THRESH_BINARY_INV, block_size, config.adaptive_c);
  const Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(2, 2));
  cv::morphologyEx(binary, binary, cv::MORPH_OPEN, kernel);
  const double ratio =
      cv::countNonZero(binary) / static_cast<double>(binary.total());
  return {ratio, std::move(binary)};
}

/// Matches `compute_dhash`: resize to (hash_size+1, hash_size) via INTER_AREA,
/// then strict `>` comparison along x, row-major MSB-first packing.
std::uint64_t compute_dhash_mat(const Mat& gray, int hash_size) {
  Mat resized;
  cv::resize(gray, resized, cv::Size(hash_size + 1, hash_size), 0, 0,
             cv::INTER_AREA);
  std::uint64_t hash = 0;
  for (int y = 0; y < hash_size; ++y) {
    for (int x = 0; x < hash_size; ++x) {
      hash <<= 1;
      if (resized.at<std::uint8_t>(y, x + 1) > resized.at<std::uint8_t>(y, x)) {
        hash |= 1ULL;
      }
    }
  }
  return hash;
}

/// Matches `compute_ssim` (numpy fallback over GaussianBlur). Inputs are
/// grayscale (CV_8UC1) Mats; converted to float64 internally.
///
/// `ws` (when non-null) supplies pre-allocated destinations for the OpenCV
/// function calls (O2); the MatExpr temporaries are intentionally kept as-is
/// so the arithmetic - and therefore the bit-exact result - is unchanged.
/// A null workspace behaves exactly like the original allocation-per-call
/// form.
double compute_ssim_mat_ws(const Mat& img1, const Mat& img2, int window_size,
                           struct SsimWorkspace::Impl* ws) {
  Mat g1;
  Mat g2;
  img1.convertTo(ws != nullptr ? ws->g1 : g1, CV_64F);
  img2.convertTo(ws != nullptr ? ws->g2 : g2, CV_64F);
  Mat& gray1 = ws != nullptr ? ws->g1 : g1;
  Mat& gray2 = ws != nullptr ? ws->g2 : g2;
  if (gray1.size() != gray2.size()) {
    cv::resize(gray2, gray2, gray1.size(), 0, 0, cv::INTER_AREA);
  }
  const double c1 = (0.01 * 255.0) * (0.01 * 255.0);
  const double c2 = (0.03 * 255.0) * (0.03 * 255.0);
  const cv::Size win(window_size, window_size);

  Mat mu1;
  Mat mu2;
  cv::GaussianBlur(gray1, ws != nullptr ? ws->mu1 : mu1, win, 0);
  cv::GaussianBlur(gray2, ws != nullptr ? ws->mu2 : mu2, win, 0);
  Mat& mu1_ref = ws != nullptr ? ws->mu1 : mu1;
  Mat& mu2_ref = ws != nullptr ? ws->mu2 : mu2;

  Mat mu1_sq;
  Mat mu2_sq;
  Mat mu1_mu2;
  cv::multiply(mu1_ref, mu1_ref, ws != nullptr ? ws->mu1_sq : mu1_sq);
  cv::multiply(mu2_ref, mu2_ref, ws != nullptr ? ws->mu2_sq : mu2_sq);
  cv::multiply(mu1_ref, mu2_ref, ws != nullptr ? ws->mu1_mu2 : mu1_mu2);
  Mat& mu1_sq_ref = ws != nullptr ? ws->mu1_sq : mu1_sq;
  Mat& mu2_sq_ref = ws != nullptr ? ws->mu2_sq : mu2_sq;
  Mat& mu1_mu2_ref = ws != nullptr ? ws->mu1_mu2 : mu1_mu2;

  Mat g1g1;
  Mat g2g2;
  Mat g1g2;
  cv::multiply(gray1, gray1, ws != nullptr ? ws->g1g1 : g1g1);
  cv::multiply(gray2, gray2, ws != nullptr ? ws->g2g2 : g2g2);
  cv::multiply(gray1, gray2, ws != nullptr ? ws->g1g2 : g1g2);
  Mat& g1g1_ref = ws != nullptr ? ws->g1g1 : g1g1;
  Mat& g2g2_ref = ws != nullptr ? ws->g2g2 : g2g2;
  Mat& g1g2_ref = ws != nullptr ? ws->g1g2 : g1g2;

  Mat sg1;
  Mat sg2;
  Mat sg12;
  cv::GaussianBlur(g1g1_ref, ws != nullptr ? ws->sg1 : sg1, win, 0);
  cv::GaussianBlur(g2g2_ref, ws != nullptr ? ws->sg2 : sg2, win, 0);
  cv::GaussianBlur(g1g2_ref, ws != nullptr ? ws->sg12 : sg12, win, 0);
  Mat& sg1_ref = ws != nullptr ? ws->sg1 : sg1;
  Mat& sg2_ref = ws != nullptr ? ws->sg2 : sg2;
  Mat& sg12_ref = ws != nullptr ? ws->sg12 : sg12;

  Mat sigma1_sq;
  Mat sigma2_sq;
  Mat sigma12;
  cv::subtract(sg1_ref, mu1_sq_ref, ws != nullptr ? ws->sigma1_sq : sigma1_sq);
  cv::subtract(sg2_ref, mu2_sq_ref, ws != nullptr ? ws->sigma2_sq : sigma2_sq);
  cv::subtract(sg12_ref, mu1_mu2_ref, ws != nullptr ? ws->sigma12 : sigma12);
  Mat& sigma1_sq_ref = ws != nullptr ? ws->sigma1_sq : sigma1_sq;
  Mat& sigma2_sq_ref = ws != nullptr ? ws->sigma2_sq : sigma2_sq;
  Mat& sigma12_ref = ws != nullptr ? ws->sigma12 : sigma12;

  // num = (2*mu1_mu2 + c1) * (2*sigma12 + c2)
  // den = (mu1_sq + mu2_sq + c1) * (sigma1_sq + sigma2_sq + c2)
  Mat t1 = 2.0 * mu1_mu2_ref + c1;
  Mat t2 = 2.0 * sigma12_ref + c2;
  Mat t3 = mu1_sq_ref + mu2_sq_ref + c1;
  Mat t4 = sigma1_sq_ref + sigma2_sq_ref + c2;
  Mat num;
  Mat den;
  cv::multiply(t1, t2, num);
  cv::multiply(t3, t4, den);

  Mat ssim_map;
  cv::divide(num, den, ssim_map);
  return cv::mean(ssim_map)[0];
}

double compute_ssim_mat(const Mat& img1, const Mat& img2, int window_size) {
  return compute_ssim_mat_ws(img1, img2, window_size, nullptr);
}

void check_hash_size(int hash_size) {
  if (hash_size < 1 || hash_size > 8) {
    throw std::invalid_argument("hash_size must be in [1, 8] (uint64 dhash)");
  }
}

}  // namespace

double compute_mask_ssim_reusing(const ImageView& mask1, const ImageView& mask2,
                                 std::int32_t window_size,
                                 SsimWorkspace& workspace) {
  return compute_ssim_mat_ws(to_gray_mat(mask1), to_gray_mat(mask2),
                             window_size, workspace.impl_.get());
}

ImageBuffer compute_foreground_mask(const ImageView& image,
                                    const SignatureConfig& config) {
  const Mat gray = to_gray_mat(image);
  const int block_size = compute_block_size(gray.rows, config.block_size_ratio);
  Mat binary;
  cv::adaptiveThreshold(gray, binary, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                        cv::THRESH_BINARY_INV, block_size, config.adaptive_c);
  const Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(2, 2));
  cv::morphologyEx(binary, binary, cv::MORPH_OPEN, kernel);

  // Own the mask as a tightly-packed Gray8 buffer so the caller (the
  // ChangePointDetector anchor cache, O1) needs no OpenCV types.
  ImageBuffer out(binary.cols, binary.rows, PixelFormat::Gray8);
  if (out.empty()) {
    return out;
  }
  const std::size_t row_bytes = static_cast<std::size_t>(binary.cols);
  for (int y = 0; y < binary.rows; ++y) {
    const auto* src = binary.ptr<std::uint8_t>(y);
    std::uint8_t* dst =
        out.data() + static_cast<std::size_t>(y) *
                         static_cast<std::size_t>(out.stride_bytes());
    std::memcpy(dst, src, row_bytes);
  }
  return out;
}

double compute_mask_ssim(const ImageView& mask1, const ImageView& mask2,
                         std::int32_t window_size) {
  return compute_ssim_mat(to_gray_mat(mask1), to_gray_mat(mask2), window_size);
}

FrameSignature compute_signature(const ImageView& image,
                                 std::int64_t timestamp_ms,
                                 const SignatureConfig& config) {
  const Mat gray = to_gray_mat(image);
  const auto fr = compute_foreground_ratio_mat(gray, config);
  check_hash_size(config.hash_size);
  const std::uint64_t dh = compute_dhash_mat(fr.binary, config.hash_size);
  return FrameSignature{timestamp_ms, fr.ratio, dh};
}

std::uint64_t compute_dhash(const ImageView& gray, std::int32_t hash_size) {
  check_hash_size(hash_size);
  if (gray.format() != PixelFormat::Gray8) {
    throw std::invalid_argument("compute_dhash expects a Gray8 image");
  }
  return compute_dhash_mat(to_gray_mat(gray), hash_size);
}

int hamming_distance(std::uint64_t a, std::uint64_t b) noexcept {
  return std::popcount(a ^ b);
}

double compute_ssim(const ImageView& image1, const ImageView& image2,
                    std::int32_t window_size) {
  return compute_ssim_mat(to_gray_mat(image1), to_gray_mat(image2), window_size);
}

double compute_foreground_ssim(const ImageView& current, const ImageView& anchor,
                               const SignatureConfig& config, bool use_mask,
                               std::int32_t window_size) {
  Mat g1 = to_gray_mat(current);
  Mat g2 = to_gray_mat(anchor);
  if (g1.size() != g2.size()) {
    cv::resize(g2, g2, g1.size(), 0, 0, cv::INTER_AREA);
  }
  if (use_mask) {
    const auto b1 = compute_foreground_ratio_mat(g1, config);
    const auto b2 = compute_foreground_ratio_mat(g2, config);
    return compute_ssim_mat(b1.binary, b2.binary, window_size);
  }
  return compute_ssim_mat(g1, g2, window_size);
}

}  // namespace sublift
