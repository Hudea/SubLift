#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "ppocr_crop_cls.hpp"

TEST_CASE("Perspective crop high-narrow 90 deg rotation", "[paddle][crop]") {
  int w = 100;
  int h = 100;
  std::vector<uint8_t> img(static_cast<size_t>(w * h * 3), 128);

  sublift::paddle::QuadPolygon quad;
  quad.p0 = sublift::Point2D{10.0f, 10.0f};
  quad.p1 = sublift::Point2D{20.0f, 10.0f};
  quad.p2 = sublift::Point2D{20.0f, 90.0f};
  quad.p3 = sublift::Point2D{10.0f, 90.0f};

  auto res = sublift::paddle::get_rotate_crop(img.data(), w, h, w * 3, quad);
  REQUIRE(res.width > 0);
  REQUIRE(res.height > 0);
  // High-narrow 80x10 -> crop_h / crop_w >= 1.5 -> rotated 90 deg -> width > height
  CHECK(res.rotated_90 == true);
  CHECK(res.width > res.height);
}

TEST_CASE("Rec tensor preparation right-zero padding", "[paddle][rec_tensor]") {
  sublift::paddle::CropResult crop;
  crop.width = 100;
  crop.height = 48;
  crop.rgb_data.resize(100 * 48 * 3, 200);

  int target_w = 320;
  int rec_h = 48;
  std::vector<float> tensor(static_cast<size_t>(3 * rec_h * target_w), 1.0f);

  sublift::paddle::prepare_rec_tensor(crop, rec_h, target_w, tensor.data());

  // Left valid region should be normalized non-zero values
  CHECK(tensor[0] != 0.0f);

  // Right padding area (> valid_w = 100) should be exact 0.0f
  CHECK(tensor[150] == 0.0f);
  CHECK(tensor[319] == 0.0f);
}

TEST_CASE("Cls preprocessing preserves BGR model order and zero padding",
          "[paddle][cls_tensor]") {
  sublift::paddle::CropResult crop;
  crop.width = 96;
  crop.height = 48;
  crop.rgb_data.resize(96 * 48 * 3);
  for (std::size_t pixel = 0; pixel < 96U * 48U; ++pixel) {
    crop.rgb_data[pixel * 3] = 255;
    crop.rgb_data[pixel * 3 + 1] = 127;
    crop.rgb_data[pixel * 3 + 2] = 0;
  }

  std::vector<float> tensor(3 * 48 * 192, 1.0F);
  sublift::paddle::prepare_cls_tensor(crop, tensor.data());

  CHECK(tensor[0] == -1.0F);
  CHECK(tensor[48 * 192] == Catch::Approx(-0.0039215686F));
  CHECK(tensor[2 * 48 * 192] == 1.0F);
  CHECK(tensor[96] == 0.0F);
  CHECK(tensor[191] == 0.0F);
}

TEST_CASE("Cls 180 rotation uses RapidOCR strict threshold",
          "[paddle][cls]") {
  sublift::paddle::CropResult crop;
  crop.width = 2;
  crop.height = 1;
  crop.rgb_data = {1, 2, 3, 4, 5, 6};

  auto equal_threshold =
      sublift::paddle::apply_cls_result(crop, 1, 0.9F, 0.9F);
  CHECK_FALSE(equal_threshold.rotated_180);
  CHECK(crop.rgb_data == std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6});

  auto above_threshold =
      sublift::paddle::apply_cls_result(crop, 1, 0.9001F, 0.9F);
  CHECK(above_threshold.rotated_180);
  CHECK(crop.rgb_data == std::vector<std::uint8_t>{4, 5, 6, 1, 2, 3});
}
