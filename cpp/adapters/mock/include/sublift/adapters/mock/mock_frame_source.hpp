#pragma once

#include <atomic>
#include <cstddef>
#include <mutex>
#include <vector>
#include <sublift/ports/frame_source.hpp>

namespace sublift::adapters::mock {

class MockFrameSource : public IFrameSource {
 public:
  MockFrameSource() = default;
  ~MockFrameSource() override = default;

  MockFrameSource(MockFrameSource&& other) noexcept;
  MockFrameSource& operator=(MockFrameSource&& other) noexcept;
  MockFrameSource(const MockFrameSource&) = delete;
  MockFrameSource& operator=(const MockFrameSource&) = delete;

  void add_frame(Frame frame);
  void set_frames(std::vector<Frame> frames);
  void clear_frames();
  [[nodiscard]] std::size_t frame_count() const;
  void set_video_info(VideoInfo info);

  void set_keyframe_thumbnails(std::vector<KeyframeThumbnail> thumbnails);
  void add_keyframe_thumbnail(KeyframeThumbnail thumbnail);
  void clear_keyframe_thumbnails();
  [[nodiscard]] std::size_t sample_call_count() const noexcept;
  [[nodiscard]] double last_sample_bottom_ratio() const noexcept;
  [[nodiscard]] std::size_t last_sample_max_samples() const noexcept;

  [[nodiscard]] VideoInfo probe(const std::filesystem::path& video_path) override;

  [[nodiscard]] FrameIOPlan plan(
      const VideoInfo& info,
      const std::optional<SourceBox>& requested_box) override;

  void extract(
      const std::filesystem::path& video_path,
      double fps,
      const std::optional<SourceBox>& crop,
      std::function<bool(Frame)> consumer) override;

  [[nodiscard]] ImageBuffer extract_frame(
      const std::filesystem::path& video_path,
      std::int64_t timestamp_ms,
      const std::optional<SourceBox>& crop) override;

  [[nodiscard]] std::vector<KeyframeThumbnail> sample_keyframe_thumbnails(
      const std::filesystem::path& video_path,
      double bottom_ratio = 0.45,
      std::size_t max_samples = 60) override;

  void cancel() override;
  [[nodiscard]] bool is_cancelled() const noexcept override;

 private:
  mutable std::mutex mutex_;
  VideoInfo info_{
      .width = 1920,
      .height = 1080,
      .duration_ms = 10000,
      .fps = 25.0,
  };
  std::vector<Frame> frames_;
  std::vector<KeyframeThumbnail> mock_thumbnails_;
  mutable std::size_t sample_call_count_{0};
  mutable double last_sample_bottom_ratio_{0.0};
  mutable std::size_t last_sample_max_samples_{0};
  std::atomic<bool> cancelled_{false};
};

} // namespace sublift::adapters::mock

